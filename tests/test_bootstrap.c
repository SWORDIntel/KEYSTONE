/*
 * KEYSTONE Snapshot Bootstrap & Live Handoff Tests (audit criterion 4)
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "../include/keystone.h"
#include "../include/keystone_federation.h"
#include "../include/keystone_exact_index.h"
#include "../include/keystone_temporal.h"
#include "../include/keystone_bootstrap.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static keystone_federation_record_t make_rec(uint8_t obj, uint8_t evt, uint64_t gen,
                                             uint64_t ms, uint32_t flags) {
    keystone_federation_record_t r;
    memset(&r, 0, sizeof(r));
    r.source_object_id.bytes[0] = obj;
    r.source_event_id.bytes[0] = evt;
    r.source_node_id.bytes[0] = 0x5A;
    r.source_generation = gen;
    r.fencing_epoch = 1;
    r.source_hlc = (keystone_hlc_t){ .physical_ms = ms, .logical = 0, .node_id = 1 };
    r.tenant_id = 1;
    r.classification = KEYSTONE_CLASSIFICATION_OPS;
    r.object_type = KEYSTONE_OBJ_VM;
    r.flags = flags ? flags : KEYSTONE_RECORD_FLAG_EVENT;
    return r;
}

static void apply_live(keystone_federation_ingest_t* eng, keystone_exact_index_t* ex,
                       keystone_temporal_index_t* tp, keystone_federation_record_t r) {
    keystone_federation_ingest_submit(eng, &r);
    keystone_exact_index_ingest_record(ex, &r, 0);
    keystone_temporal_index_ingest_record(tp, &r, 0);
}

/*
 * The criterion-4 core: an index bootstrapped from a snapshot at watermark
 * W, then fed the FULL event history (pre-W replays + post-W live events),
 * must equal an index built purely live — pre-W events detected, never
 * double-applied; post-W events all applied.
 */
static void test_snapshot_bootstrap_equivalence(void) {
    printf("[*] Testing snapshot bootstrap == all-live (exact-cursor handoff)...\n");

    const char* snap_path = "/tmp/keystone_bootstrap_test.snapshot";
    const int N_SNAP = 10;   /* events in the snapshot era */
    const int N_REPLAY = 6;  /* how many snapshot-era events get replayed live */
    const int N_LIVE = 5;    /* genuinely new live events */

    /* World A: pure live ingestion of everything. */
    keystone_ingest_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.initial_fencing_epoch = 1;
    keystone_federation_ingest_t* eng_a = keystone_federation_ingest_create(&cfg);
    keystone_exact_index_t* ex_a = keystone_exact_index_create(32);
    keystone_temporal_index_t* tp_a = keystone_temporal_index_create(32);
    TEST_ASSERT(eng_a && ex_a && tp_a);

    /* World B: ingest the snapshot era, snapshot, destroy. */
    keystone_federation_ingest_t* eng_b = keystone_federation_ingest_create(&cfg);
    keystone_exact_index_t* ex_b = keystone_exact_index_create(32);
    keystone_temporal_index_t* tp_b = keystone_temporal_index_create(32);
    TEST_ASSERT(eng_b && ex_b && tp_b);

    for (int i = 0; i < N_SNAP; i++) {
        uint32_t flags = (i == 7) ? KEYSTONE_RECORD_FLAG_TOMBSTONE : 0;
        keystone_federation_record_t r = make_rec((uint8_t)(i % 4), (uint8_t)(i + 1),
                                                  (uint64_t)(i + 1), 9500000ull + (uint64_t)i * 10, flags);
        apply_live(eng_a, ex_a, tp_a, r);
        apply_live(eng_b, ex_b, tp_b, r);
    }

    /* Snapshot world B at its current watermark. */
    keystone_snapshot_meta_t meta;
    TEST_ASSERT(keystone_snapshot_save(ex_b, tp_b, (uint64_t)N_SNAP, 1000, snap_path) == 0);
    keystone_federation_ingest_destroy(eng_b);
    keystone_exact_index_destroy(ex_b);
    keystone_temporal_index_destroy(tp_b);
    eng_b = NULL; ex_b = NULL; tp_b = NULL;

    /* Bootstrap world B from the snapshot. */
    keystone_bootstrap_session_t sess;
    TEST_ASSERT(keystone_bootstrap_begin(snap_path, &cfg, &sess) == 0);
    TEST_ASSERT(sess.meta.snapshot_generation == (uint64_t)N_SNAP);
    TEST_ASSERT(sess.meta.cursor == 1000);
    TEST_ASSERT(sess.meta.temporal_count == (size_t)N_SNAP);
    TEST_ASSERT(sess.meta.watermark_hlc.physical_ms == 9500000ull + (N_SNAP - 1) * 10);

    /* Replay the old era (duplicates; must be detected, never applied) ... */
    int dup_reported = 0;
    for (int i = 0; i < N_REPLAY; i++) {
        uint32_t flags = (i == 7 % N_REPLAY && N_REPLAY > 7) ? KEYSTONE_RECORD_FLAG_TOMBSTONE : 0;
        keystone_federation_record_t r = make_rec((uint8_t)(i % 4), (uint8_t)(i + 1),
                                                  (uint64_t)(i + 1), 9500000ull + (uint64_t)i * 10, flags);
        keystone_ingest_status_t st = keystone_bootstrap_apply_live(&sess, &r);
        TEST_ASSERT(st == KEYSTONE_INGEST_DUPLICATE);
        dup_reported++;
    }

    /* ... then feed the live era to BOTH worlds. */
    for (int i = 0; i < N_LIVE; i++) {
        keystone_federation_record_t r = make_rec((uint8_t)(i % 4), (uint8_t)(100 + i),
                                                  (uint64_t)(100 + i),
                                                  sess.meta.watermark_hlc.physical_ms + 100 + (uint64_t)i * 10, 0);
        apply_live(eng_a, ex_a, tp_a, r);
        keystone_ingest_status_t st = keystone_bootstrap_apply_live(&sess, &r);
        TEST_ASSERT(st == KEYSTONE_INGEST_OK);
    }

    /* Equivalence: the bootstrap path added exactly the live-era events. */
    TEST_ASSERT(keystone_temporal_index_count(sess.temporal) ==
                keystone_temporal_index_count(tp_a));
    TEST_ASSERT((size_t)(N_SNAP + N_LIVE) == keystone_temporal_index_count(tp_a));
    TEST_ASSERT(keystone_exact_index_count(sess.exact) == keystone_exact_index_count(ex_a));

    keystone_hlc_t wm_a, wm_b;
    TEST_ASSERT(keystone_temporal_index_watermark(tp_a, &wm_a, NULL) == true);
    TEST_ASSERT(keystone_temporal_index_watermark(sess.temporal, &wm_b, NULL) == true);
    TEST_ASSERT(keystone_hlc_compare(&wm_a, &wm_b) == 0);

    /* Per-object state matches, including the tombstoned object. */
    for (uint8_t obj = 0; obj < 4; obj++) {
        keystone_uuid_t id;
        memset(&id, 0, sizeof(id));
        id.bytes[0] = obj;
        keystone_exact_entry_t ea, eb;
        TEST_ASSERT(keystone_exact_index_lookup(ex_a, &id, &ea) == 0);
        TEST_ASSERT(keystone_exact_index_lookup(sess.exact, &id, &eb) == 0);
        TEST_ASSERT(ea.latest_generation == eb.latest_generation);
        TEST_ASSERT(ea.flags == eb.flags);
        /* and the active-history view agrees */
        keystone_hlc_t lo = {0}, hi = { .physical_ms = UINT64_MAX / 2, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };
        keystone_temporal_entry_t out_a[8], out_b[8];
        size_t na = keystone_temporal_index_query_object_active(tp_a, &id, &lo, &hi, out_a, 8);
        size_t nb = keystone_temporal_index_query_object_active(sess.temporal, &id, &lo, &hi, out_b, 8);
        TEST_ASSERT(na == nb);
    }
    (void)dup_reported;

    /* Stale fencing still fenced after bootstrap. */
    keystone_federation_record_t stale = make_rec(0x50, 0x51, 500, wm_b.physical_ms + 500, 0);
    stale.fencing_epoch = 0;
    TEST_ASSERT(keystone_federation_advance_fencing_epoch(sess.engine, 9) == 0);
    TEST_ASSERT(keystone_bootstrap_apply_live(&sess, &stale) == KEYSTONE_INGEST_STALE_FENCING_EPOCH);

    /* Rolling a new snapshot from the session works. */
    const char* snap2 = "/tmp/keystone_bootstrap_test2.snapshot";
    TEST_ASSERT(keystone_bootstrap_snapshot_now(&sess, 2000, snap2) == 0);
    keystone_bootstrap_session_t sess2;
    TEST_ASSERT(keystone_bootstrap_begin(snap2, &cfg, &sess2) == 0);
    TEST_ASSERT(sess2.meta.temporal_count == keystone_temporal_index_count(sess.temporal));
    TEST_ASSERT(sess2.meta.cursor == 2000);
    keystone_bootstrap_destroy(&sess2);
    unlink(snap2);

    keystone_bootstrap_destroy(&sess);
    keystone_federation_ingest_destroy(eng_a);
    keystone_exact_index_destroy(ex_a);
    keystone_temporal_index_destroy(tp_a);
    unlink(snap_path);
    printf("    [+] Snapshot bootstrap == all-live verified (replay detected, no double-apply).\n");
}

static void test_snapshot_hostile_files(void) {
    printf("[*] Testing snapshot loader hostility rejection...\n");

    const char* path = "/tmp/keystone_bootstrap_hostile.snapshot";
    keystone_exact_index_t* ex = keystone_exact_index_create(8);
    keystone_temporal_index_t* tp = keystone_temporal_index_create(8);
    keystone_federation_record_t r = make_rec(1, 2, 3, 9600000, 0);
    apply_live(NULL, ex, tp, r);
    TEST_ASSERT(keystone_snapshot_save(ex, tp, 3, 42, path) == 0);
    keystone_exact_index_destroy(ex);
    keystone_temporal_index_destroy(tp);

    FILE* f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* valid = (uint8_t*)malloc((size_t)sz);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz + 8);
    TEST_ASSERT(fread(valid, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    keystone_exact_index_t* oe = NULL;
    keystone_temporal_index_t* ot = NULL;
    keystone_snapshot_meta_t om;

#define SNAP_WRITE(len) do { FILE* w = fopen(path, "wb"); TEST_ASSERT(w != NULL); \
    TEST_ASSERT(fwrite(buf, 1, (size_t)(len), w) == (size_t)(len)); fclose(w); } while (0)

    /* sanity */
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == 0);
    keystone_exact_index_destroy(oe); keystone_temporal_index_destroy(ot); oe = NULL; ot = NULL;

    /* bad magic */
    memcpy(buf, valid, (size_t)sz); buf[0] = 0xEE; SNAP_WRITE(sz);
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == -1);
    /* nonzero reserved */
    memcpy(buf, valid, (size_t)sz); buf[8] = 1; SNAP_WRITE(sz);
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == -1);
    /* truncated */
    memcpy(buf, valid, (size_t)sz); SNAP_WRITE(sz - 3);
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == -1);
    /* trailing garbage */
    memcpy(buf, valid, (size_t)sz); buf[sz] = 0x41; SNAP_WRITE(sz + 1);
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == -1);
    /* lying exact_count (huge) */
    memcpy(buf, valid, (size_t)sz);
    for (int i = 0; i < 8; i++) buf[40 + i] = (uint8_t)(0x4000000000000000ull >> (8 * i));
    SNAP_WRITE(sz);
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == -1);
    /* corrupted body byte */
    memcpy(buf, valid, (size_t)sz); buf[sz - 1] ^= 0xFF; SNAP_WRITE(sz);
    TEST_ASSERT(keystone_snapshot_load(path, &oe, &ot, &om) == -1);

#undef SNAP_WRITE
    free(valid); free(buf); unlink(path);
    printf("    [+] Hostile snapshot files all rejected.\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Snapshot Bootstrap & Live Handoff Tests\n");
    printf("=================================================================\n");

    test_snapshot_bootstrap_equivalence();
    test_snapshot_hostile_files();

    printf("=================================================================\n");
    printf("  ALL BOOTSTRAP TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
