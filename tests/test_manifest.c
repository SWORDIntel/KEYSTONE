/*
 * KEYSTONE Index Manifests & Atomic Publication Tests (audit criterion 13)
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "../include/keystone.h"
#include "../include/keystone_federation.h"
#include "../include/keystone_exact_index.h"
#include "../include/keystone_temporal.h"
#include "../include/keystone_manifest.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

static void build_world(keystone_exact_index_t** ex, keystone_temporal_index_t** tp,
                        int n_events, uint64_t base_ms) {
    *ex = keystone_exact_index_create(32);
    *tp = keystone_temporal_index_create(32);
    TEST_ASSERT(*ex && *tp);
    for (int i = 0; i < n_events; i++) {
        keystone_federation_record_t r;
        memset(&r, 0, sizeof(r));
        r.source_object_id.bytes[0] = (uint8_t)(i % 3);
        r.source_event_id.bytes[0] = (uint8_t)(base_ms + i);
        r.source_generation = (uint64_t)(base_ms + i);
        r.fencing_epoch = 1;
        r.source_hlc = (keystone_hlc_t){ .physical_ms = base_ms + (uint64_t)i * 10, .logical = 0, .node_id = 1 };
        r.tenant_id = 1;
        r.classification = KEYSTONE_CLASSIFICATION_OPS;
        r.object_type = KEYSTONE_OBJ_VM;
        r.flags = (i == n_events - 1) ? KEYSTONE_RECORD_FLAG_TOMBSTONE : KEYSTONE_RECORD_FLAG_EVENT;
        TEST_ASSERT(keystone_exact_index_ingest_record(*ex, &r, 0) == 0);
        TEST_ASSERT(keystone_temporal_index_ingest_record(*tp, &r, 0) == 0);
    }
}

static void save_world(keystone_exact_index_t* ex, keystone_temporal_index_t* tp,
                       const char* ex_path, const char* tp_path) {
    TEST_ASSERT(keystone_exact_index_save(ex, ex_path) == 0);
    TEST_ASSERT(keystone_temporal_index_save(tp, tp_path) == 0);
}

static void test_manifest_roundtrip_and_hostility(void) {
    printf("[*] Testing manifest build/save/load/verify + hostility...\n");

    keystone_exact_index_t* ex;
    keystone_temporal_index_t* tp;
    build_world(&ex, &tp, 5, 7000000);
    const char* ex_p = "/tmp/ksmf_exact.index";
    const char* mf_p = "/tmp/ksmf_exact.manifest";
    save_world(ex, tp, ex_p, "/tmp/ksmf_tp.index");
    keystone_temporal_index_destroy(tp);

    keystone_hlc_t lo, hi;
    keystone_temporal_index_t* probe = NULL;
    TEST_ASSERT(keystone_temporal_index_load(&probe, "/tmp/ksmf_tp.index") == 0);
    TEST_ASSERT(keystone_temporal_index_hlc_bounds(probe, &lo, &hi) == true);
    TEST_ASSERT(keystone_temporal_index_tombstone_count(probe) == 1);
    keystone_temporal_index_destroy(probe);

    keystone_index_manifest_t m;
    TEST_ASSERT(keystone_manifest_build("exact", 42, 1000, &lo, &hi,
                                        (uint64_t)keystone_exact_index_count(ex), 1,
                                        KEYSTONE_ALGO_VERSION_EXACT, ex_p, &m) == 0);
    TEST_ASSERT(m.index_generation == 42);
    TEST_ASSERT(m.source_cursor == 1000);
    TEST_ASSERT(m.build_version == KEYSTONE_BUILD_VERSION);
    TEST_ASSERT(m.payload_bytes > 0);
    TEST_ASSERT(keystone_manifest_save(&m, mf_p) == 0);

    keystone_index_manifest_t ml;
    TEST_ASSERT(keystone_manifest_load(mf_p, &ml) == 0);
    TEST_ASSERT(memcmp(&m.index_uuid, &ml.index_uuid, sizeof(keystone_uuid_t)) == 0);
    TEST_ASSERT(ml.record_count == keystone_exact_index_count(ex));
    TEST_ASSERT(keystone_manifest_verify_payload(&ml, ex_p) == 1);

    /* Payload tampering is detected */
    FILE* f = fopen(ex_p, "r+b");
    TEST_ASSERT(f != NULL);
    fseek(f, 100, SEEK_SET);
    fputc(fgetc(f) ^ 0xFF, f);
    fclose(f);
    TEST_ASSERT(keystone_manifest_verify_payload(&ml, ex_p) == 0);

    /* Manifest hostility: corrupt byte, truncation, trailing */
    f = fopen(mf_p, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz + 4);
    TEST_ASSERT(buf != NULL);
    TEST_ASSERT(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    FILE* w;
    buf[sz - 5] ^= 0x5A;
    w = fopen(mf_p, "wb"); TEST_ASSERT(w && fwrite(buf, 1, (size_t)sz, w) == (size_t)sz); fclose(w);
    TEST_ASSERT(keystone_manifest_load(mf_p, &ml) == -1);

    w = fopen(mf_p, "wb"); TEST_ASSERT(w && fwrite(buf, 1, (size_t)sz - 4, w) == (size_t)sz - 4); fclose(w);
    TEST_ASSERT(keystone_manifest_load(mf_p, &ml) == -1);

    buf[sz - 5] ^= 0x5A; /* repair */
    buf[sz] = 0x41;
    w = fopen(mf_p, "wb"); TEST_ASSERT(w && fwrite(buf, 1, (size_t)sz + 1, w) == (size_t)sz + 1); fclose(w);
    TEST_ASSERT(keystone_manifest_load(mf_p, &ml) == -1);

    free(buf);
    keystone_exact_index_destroy(ex);
    unlink(ex_p); unlink(mf_p); unlink("/tmp/ksmf_tp.index");
    printf("    [+] Manifest roundtrip, payload verification, and hostility verified.\n");
}

static void test_publication_lifecycle(void) {
    printf("[*] Testing publication lifecycle: publish, open-latest, fallback, quarantine...\n");

    const char* dir = "/tmp/ksmf_publication";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }

    keystone_publication_t* pub = keystone_publication_create(dir);
    TEST_ASSERT(pub != NULL);

    /* Generation 1 */
    keystone_exact_index_t* ex1;
    keystone_temporal_index_t* tp1;
    build_world(&ex1, &tp1, 5, 7000000);
    save_world(ex1, tp1, "/tmp/ksmf_p1.exact", "/tmp/ksmf_p1.temporal");
    keystone_hlc_t lo1, hi1;
    TEST_ASSERT(keystone_temporal_index_hlc_bounds(tp1, &lo1, &hi1) == true);
    TEST_ASSERT(keystone_publication_publish(pub, 1, 100, "/tmp/ksmf_p1.exact", "/tmp/ksmf_p1.temporal",
                                              &lo1, &hi1, 5, 1) == 0);
    TEST_ASSERT(keystone_publication_active_generation(pub) == 1);
    keystone_exact_index_destroy(ex1);
    keystone_temporal_index_destroy(tp1);

    keystone_exact_index_t* lex = NULL;
    keystone_temporal_index_t* ltp = NULL;
    keystone_index_manifest_t lm;
    TEST_ASSERT(keystone_publication_open_latest(pub, &lex, &ltp, &lm) == 0);
    TEST_ASSERT(keystone_exact_index_count(lex) == 3);
    TEST_ASSERT(keystone_temporal_index_count(ltp) == 5);
    TEST_ASSERT(lm.index_generation == 1);
    keystone_exact_index_destroy(lex);
    keystone_temporal_index_destroy(ltp);

    /* Generation 2 (superset) */
    build_world(&ex1, &tp1, 9, 7000000);
    save_world(ex1, tp1, "/tmp/ksmf_p2.exact", "/tmp/ksmf_p2.temporal");
    TEST_ASSERT(keystone_temporal_index_hlc_bounds(tp1, &lo1, &hi1) == true);
    TEST_ASSERT(keystone_publication_publish(pub, 2, 200, "/tmp/ksmf_p2.exact", "/tmp/ksmf_p2.temporal",
                                              &lo1, &hi1, 9, 1) == 0);
    TEST_ASSERT(keystone_publication_active_generation(pub) == 2);
    keystone_exact_index_destroy(ex1);
    keystone_temporal_index_destroy(tp1);

    lex = NULL; ltp = NULL;
    TEST_ASSERT(keystone_publication_open_latest(pub, &lex, &ltp, &lm) == 0);
    TEST_ASSERT(keystone_temporal_index_count(ltp) == 9);
    TEST_ASSERT(lm.index_generation == 2);
    keystone_exact_index_destroy(lex);
    keystone_temporal_index_destroy(ltp);

    FILE* f;
    /* Torn generation 3: publish files + manifests but NO active flip is
     * simulated by tampering AFTER a manual copy — simpler: corrupt gen-2's
     * temporal payload on disk; open-latest must fall back to gen 1 and
     * quarantine gen 2. */
    char gen2_tp[512];
    snprintf(gen2_tp, sizeof(gen2_tp), "%s/gen-2.temporal.index", dir);
    f = fopen(gen2_tp, "r+b");
    TEST_ASSERT(f != NULL);
    fseek(f, 80, SEEK_SET);
    fputc(fgetc(f) ^ 0xFF, f); /* corrupt a temporal entry byte -> CRC mismatch */
    fclose(f);

    lex = NULL; ltp = NULL;
    TEST_ASSERT(keystone_publication_open_latest(pub, &lex, &ltp, &lm) == 0);
    TEST_ASSERT(lm.index_generation == 1); /* fell back */
    TEST_ASSERT(keystone_temporal_index_count(ltp) == 5);
    keystone_exact_index_destroy(lex);
    keystone_temporal_index_destroy(ltp);
    /* gen-2 files were quarantined away */
    snprintf(cmd, sizeof(cmd), "%s/quarantine/gen-2.temporal.index", dir);
    struct stat qst;
    TEST_ASSERT(stat(cmd, &qst) == 0);
    TEST_ASSERT(stat(gen2_tp, &qst) != 0);

    /* Publishing a candidate that fails verification never touches active. */
    f = NULL;
    build_world(&ex1, &tp1, 3, 7000000);
    save_world(ex1, tp1, "/tmp/ksmf_p3.exact", "/tmp/ksmf_p3.temporal");
    keystone_exact_index_destroy(ex1);
    keystone_temporal_index_destroy(tp1);
    /* corrupt the temporal candidate so the loader rejects it */
    f = fopen("/tmp/ksmf_p3.temporal", "r+b");
    TEST_ASSERT(f != NULL);
    fseek(f, 90, SEEK_SET);
    fputc(fgetc(f) ^ 0xFF, f);
    fclose(f);
    TEST_ASSERT(keystone_publication_publish(pub, 3, 300, "/tmp/ksmf_p3.exact", "/tmp/ksmf_p3.temporal",
                                              NULL, NULL, 3, 0) == -1);
    /* the active pointer is untouched by a failed publish (still the
     * quarantined gen 2; open-latest self-heals to the newest valid) */
    TEST_ASSERT(keystone_publication_active_generation(pub) == 2);
    lex = NULL; ltp = NULL;
    TEST_ASSERT(keystone_publication_open_latest(pub, &lex, &ltp, &lm) == 0);
    TEST_ASSERT(lm.index_generation == 1);
    keystone_exact_index_destroy(lex);
    keystone_temporal_index_destroy(ltp);

    keystone_publication_destroy(pub);
    snprintf(cmd, sizeof(cmd), "rm -rf %s /tmp/ksmf_p1.* /tmp/ksmf_p2.* /tmp/ksmf_p3.*", dir);
    if (system(cmd) != 0) { /* cleanup best effort */ }
    printf("    [+] Publication lifecycle verified (publish, fallback, quarantine, no partial exposure).\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Index Manifests & Atomic Publication Tests\n");
    printf("=================================================================\n");

    test_manifest_roundtrip_and_hostility();
    test_publication_lifecycle();

    printf("=================================================================\n");
    printf("  ALL MANIFEST TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
