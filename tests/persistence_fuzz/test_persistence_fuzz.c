/*
 * KEYSTONE Persistence Fuzz Corpus (CITADEL brief §48/§54, audit criterion 16)
 *
 * Deterministic mutation battery over every persisted format: temporal
 * index, exact index, engine checkpoint, and the trigram index. Valid files
 * are produced by the real save paths, then mutated (bit flips, byte
 * patches, truncations, count-field lies) by a fixed-seed PRNG. The
 * contract: a loader either REJECTS the file or returns a fully usable
 * object — never a crash, hang, or out-of-bounds access. Run under the
 * sanitizer matrix (`make asan` / `make valgrind`), any memory error fails
 * hard; the same seed reproduces every case.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "../../include/keystone.h"
#include "../../include/keystone_federation.h"
#include "../../include/keystone_exact_index.h"
#include "../../include/keystone_temporal.h"
#include "../../include/keystone_trigram.h"
#include "../../include/keystone_bootstrap.h"
#include "../test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define FUZZ_SEED 0x4B53544DU /* 'KSTM' */
#define FLIP_ITERS 400
#define TRUNC_ITERS 48

static uint64_t fuzz_state = FUZZ_SEED;

static uint64_t fuzz_next(void) {
    /* xorshift64* — deterministic across platforms */
    fuzz_state ^= fuzz_state >> 12;
    fuzz_state ^= fuzz_state << 25;
    fuzz_state ^= fuzz_state >> 27;
    return fuzz_state * 0x2545F4914F6CDD1DULL;
}

static uint8_t* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static int write_file(const char* path, const uint8_t* buf, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(buf, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Loader drivers: return 0 when the (possibly mutated) file was accepted,
 * -1 when rejected. Any memory fault aborts the process (that is the bug
 * signal under sanitizers). */
static int load_temporal(const char* path) {
    keystone_temporal_index_t* idx = NULL;
    if (keystone_temporal_index_load(&idx, path) != 0) return -1;
    keystone_temporal_index_destroy(idx);
    return 0;
}

static int load_exact(const char* path) {
    keystone_exact_index_t* idx = NULL;
    if (keystone_exact_index_load(&idx, path) != 0) return -1;
    keystone_exact_index_destroy(idx);
    return 0;
}

static int load_checkpoint(const char* path) {
    keystone_federation_checkpoint_t cp;
    if (keystone_checkpoint_load(path, &cp) != 0) return -1;
    return 0;
}

static int load_snapshot(const char* path) {
    keystone_exact_index_t* ex = NULL;
    keystone_temporal_index_t* tp = NULL;
    keystone_snapshot_meta_t meta;
    if (keystone_snapshot_load(path, &ex, &tp, &meta) != 0) return -1;
    /* accepted snapshots must be usable */
    keystone_exact_index_count(ex);
    keystone_temporal_index_count(tp);
    keystone_exact_index_destroy(ex);
    keystone_temporal_index_destroy(tp);
    return 0;
}

static int load_trigram(const char* path) {
    keystone_trigram_index_t* idx = keystone_trigram_index_load(path);
    if (!idx) return -1;
    /* a mutated-but-accepted index must survive real queries */
    uint32_t hits[4];
    keystone_trigram_index_search(idx, "hostile", 7, hits, 4);
    keystone_trigram_index_destroy(idx);
    return 0;
}

static void fuzz_file(const char* label, const char* valid_path, const char* work_path,
                      int (*loader)(const char*)) {
    size_t len = 0;
    uint8_t* orig = read_file(valid_path, &len);
    TEST_ASSERT(orig != NULL);
    uint8_t* work = (uint8_t*)malloc(len);
    TEST_ASSERT(work != NULL);

    int accepted = 0, rejected = 0;

    /* sanity: the untouched file loads */
    TEST_ASSERT(loader(valid_path) == 0);

    /* 1. bit flips at random offsets (1-8 bits, one byte) */
    for (int i = 0; i < FLIP_ITERS; i++) {
        memcpy(work, orig, len);
        size_t off = (size_t)(fuzz_next() % len);
        uint8_t mask = (uint8_t)(1u << (fuzz_next() % 8));
        work[off] ^= mask;
        TEST_ASSERT(write_file(work_path, work, len) == 0);
        if (loader(work_path) == 0) accepted++; else rejected++;
    }

    /* 2. random multi-byte patches (count/offset-field shaped lies) */
    for (int i = 0; i < FLIP_ITERS / 2; i++) {
        memcpy(work, orig, len);
        size_t off = (size_t)(fuzz_next() % len);
        uint64_t v = fuzz_next();
        size_t n = (size_t)(fuzz_next() % 8) + 1;
        for (size_t k = 0; k < n && off + k < len; k++) {
            work[off + k] = (uint8_t)(v >> (8 * (k % 8)));
        }
        TEST_ASSERT(write_file(work_path, work, len) == 0);
        if (loader(work_path) == 0) accepted++; else rejected++;
    }

    /* 3. truncations at random cut points */
    for (int i = 0; i < TRUNC_ITERS; i++) {
        size_t cut = (size_t)(fuzz_next() % len);
        TEST_ASSERT(write_file(work_path, orig, cut) == 0);
        if (loader(work_path) == 0) accepted++; else rejected++;
    }

    free(orig);
    free(work);
    printf("    [%s] %d mutations: %d accepted (benign), %d rejected — no crashes\n",
           label, FLIP_ITERS + FLIP_ITERS / 2 + TRUNC_ITERS, accepted, rejected);
}

static void build_valid_files(char* temporal_path, char* exact_path, char* cp_path, char* trigram_path,
                              char* snap_path, size_t buf_len) {
    /* temporal: 6 entries (inside the <=16 danger window) */
    keystone_temporal_index_t* t = keystone_temporal_index_create(8);
    TEST_ASSERT(t != NULL);
    for (size_t i = 0; i < 6; i++) {
        keystone_temporal_entry_t te = {
            .hlc = { .physical_ms = 9000000 + i * 10, .logical = 0, .node_id = 1 },
            .event_type = KEYSTONE_OBJ_VM,
            .tenant_id = 1,
            .source_generation = i + 1,
            .payload_offset = i * 64
        };
        TEST_ASSERT(keystone_temporal_index_append(t, &te) == 0);
    }
    TEST_ASSERT(keystone_temporal_index_save(t, temporal_path) == 0);
    keystone_temporal_index_destroy(t);

    /* exact: 4 entries incl. one tombstone */
    keystone_exact_index_t* e = keystone_exact_index_create(8);
    TEST_ASSERT(e != NULL);
    for (size_t i = 1; i <= 4; i++) {
        keystone_uuid_t id;
        memset(&id, 0, sizeof(id));
        id.bytes[0] = (uint8_t)i;
        keystone_exact_entry_t ent = {
            .resource_id = id,
            .latest_generation = i,
            .fencing_epoch = 1,
            .flags = (i == 4) ? KEYSTONE_RECORD_FLAG_TOMBSTONE : 0
        };
        TEST_ASSERT(keystone_exact_index_upsert(e, &ent) == 0);
    }
    TEST_ASSERT(keystone_exact_index_save(e, exact_path) == 0);
    keystone_exact_index_destroy(e);

    /* checkpoint via a real engine */
    keystone_ingest_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    keystone_federation_ingest_t* ing = keystone_federation_ingest_create(&cfg);
    TEST_ASSERT(ing != NULL);
    TEST_ASSERT(keystone_federation_save_checkpoint(ing, cp_path) == 0);
    keystone_federation_ingest_destroy(ing);

    /* trigram via the real build + save path */
    keystone_trigram_index_t* g = keystone_trigram_index_create_options(
        4, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    TEST_ASSERT(g != NULL);
    const char* d0 = "fuzz corpus hostile input one";
    const char* d1 = "fuzz corpus hostile input two";
    TEST_ASSERT(keystone_trigram_index_add_document(g, "f0", d0, strlen(d0), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(g, "f1", d1, strlen(d1), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_finalize(g) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_save(g, trigram_path) == KEYSTONE_TRIGRAM_OK);
    keystone_trigram_index_destroy(g);

    /* snapshot built from small fresh indexes (uses the two loaders above
     * only via the same public APIs; independent for clarity) */
    {
        keystone_exact_index_t* se = keystone_exact_index_create(8);
        keystone_temporal_index_t* stp = keystone_temporal_index_create(8);
        TEST_ASSERT(se != NULL && stp != NULL);
        for (size_t i = 1; i <= 3; i++) {
            keystone_uuid_t id;
            memset(&id, 0, sizeof(id));
            id.bytes[0] = (uint8_t)i;
            keystone_exact_entry_t ent = {
                .resource_id = id, .latest_generation = i, .fencing_epoch = 1,
                .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS
            };
            TEST_ASSERT(keystone_exact_index_upsert(se, &ent) == 0);
            keystone_temporal_entry_t te = {
                .object_id = id,
                .hlc = { .physical_ms = 9400000 + i * 10, .logical = 0, .node_id = 1 },
                .event_type = KEYSTONE_OBJ_VM, .tenant_id = 1,
                .classification = KEYSTONE_CLASSIFICATION_OPS,
                .flags = KEYSTONE_RECORD_FLAG_EVENT, .source_generation = i
            };
            TEST_ASSERT(keystone_temporal_index_append(stp, &te) == 0);
        }
        TEST_ASSERT(keystone_snapshot_save(se, stp, 3, 77, snap_path) == 0);
        keystone_exact_index_destroy(se);
        keystone_temporal_index_destroy(stp);
    }
    (void)buf_len;
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Persistence Fuzz Corpus (seed 0x%08llX, deterministic)\n",
           (unsigned long long)FUZZ_SEED);
    printf("=================================================================\n");

    const char* dir = "/tmp/keystone_fuzz";
    mkdir(dir, 0755);
    char tp[256], ep[256], cpp[256], gp[256], sp[256], wp[256];
    snprintf(tp, sizeof(tp), "%s/temporal.index", dir);
    snprintf(ep, sizeof(ep), "%s/exact.index", dir);
    snprintf(cpp, sizeof(cpp), "%s/engine.checkpoint", dir);
    snprintf(gp, sizeof(gp), "%s/trigram.index", dir);
    snprintf(sp, sizeof(sp), "%s/boot.snapshot", dir);
    snprintf(wp, sizeof(wp), "%s/mutated.bin", dir);

    build_valid_files(tp, ep, cpp, gp, sp, sizeof(tp));

    fuzz_file("temporal", tp, wp, load_temporal);
    fuzz_file("exact   ", ep, wp, load_exact);
    fuzz_file("checkpoint", cpp, wp, load_checkpoint);
    fuzz_file("trigram ", gp, wp, load_trigram);
    fuzz_file("snapshot", sp, wp, load_snapshot);

    unlink(tp); unlink(ep); unlink(cpp); unlink(gp); unlink(sp); unlink(wp); rmdir(dir);

    printf("=================================================================\n");
    printf("  PERSISTENCE FUZZ COMPLETE — NO CRASHES, NO MEMORY ERRORS\n");
    printf("=================================================================\n");
    return 0;
}
