/*
 * KEYSTONE Exact Identity & Monotonic Temporal Index Test Suite
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void test_exact_identity_lifecycle(void) {
    printf("[*] Testing exact identity index lifecycle...\n");

    keystone_exact_index_t* idx = keystone_exact_index_create(64);
    TEST_ASSERT(idx != NULL);

    keystone_uuid_t vm_id, node_id, evt_id;
    keystone_uuid_from_string("11111111-2222-3333-4444-555555555555", &vm_id);
    keystone_uuid_from_string("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", &node_id);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000001", &evt_id);

    keystone_exact_entry_t entry = {
        .resource_id = vm_id,
        .latest_event_id = evt_id,
        .node_id = node_id,
        .latest_generation = 100,
        .fencing_epoch = 5,
        .latest_hlc = { .physical_ms = 1758332000000ULL, .logical = 1, .node_id = 1 },
        .tenant_id = 42,
        .classification = KEYSTONE_CLASSIFICATION_OPS,
        .object_type = KEYSTONE_OBJ_VM,
        .flags = 0,
        .location_slot = 1024
    };

    /* 1. Insert */
    TEST_ASSERT(keystone_exact_index_upsert(idx, &entry) == 0);
    TEST_ASSERT(keystone_exact_index_count(idx) == 1);
    TEST_ASSERT(keystone_exact_index_active_count(idx) == 1);
    TEST_ASSERT(keystone_exact_index_tombstone_count(idx) == 0);

    /* 2. Lookup */
    keystone_exact_entry_t found;
    TEST_ASSERT(keystone_exact_index_lookup(idx, &vm_id, &found) == 0);
    TEST_ASSERT(keystone_uuid_equal(&found.resource_id, &vm_id));
    TEST_ASSERT(keystone_uuid_equal(&found.node_id, &node_id));
    TEST_ASSERT(found.latest_generation == 100);
    TEST_ASSERT(found.fencing_epoch == 5);
    TEST_ASSERT(found.location_slot == 1024);

    /* 3. Non-existent lookup */
    keystone_uuid_t missing_id;
    keystone_uuid_from_string("ffffffff-ffff-ffff-ffff-ffffffffffff", &missing_id);
    TEST_ASSERT(keystone_exact_index_lookup(idx, &missing_id, &found) == -1);
    TEST_ASSERT(!keystone_exact_index_contains(idx, &missing_id));
    TEST_ASSERT(keystone_exact_index_contains(idx, &vm_id));

    /* 4. Reject stale epoch */
    keystone_exact_entry_t stale_epoch = entry;
    stale_epoch.fencing_epoch = 4; /* Less than current 5 */
    stale_epoch.latest_generation = 200;
    TEST_ASSERT(keystone_exact_index_upsert(idx, &stale_epoch) == -2);

    /* 5. Reject stale generation at same epoch */
    keystone_exact_entry_t stale_gen = entry;
    stale_gen.latest_generation = 99; /* Less than current 100 */
    TEST_ASSERT(keystone_exact_index_upsert(idx, &stale_gen) == -3);

    /* 6. Valid update (generation 101) */
    keystone_exact_entry_t updated = entry;
    updated.latest_generation = 101;
    updated.location_slot = 2048;
    TEST_ASSERT(keystone_exact_index_upsert(idx, &updated) == 0);

    TEST_ASSERT(keystone_exact_index_lookup(idx, &vm_id, &found) == 0);
    TEST_ASSERT(found.latest_generation == 101);
    TEST_ASSERT(found.location_slot == 2048);

    /* 7. Tombstone transition */
    keystone_exact_entry_t tomb = updated;
    tomb.latest_generation = 102;
    tomb.flags |= KEYSTONE_RECORD_FLAG_TOMBSTONE;
    TEST_ASSERT(keystone_exact_index_upsert(idx, &tomb) == 0);
    TEST_ASSERT(keystone_exact_index_count(idx) == 1);
    TEST_ASSERT(keystone_exact_index_active_count(idx) == 0);
    TEST_ASSERT(keystone_exact_index_tombstone_count(idx) == 1);

    /* 8. Scale to 10,000 resources and verify no collision corruption */
    const size_t SCALE_COUNT = 10000;
    for (size_t i = 1; i <= SCALE_COUNT; i++) {
        keystone_uuid_t id;
        memset(&id, 0, sizeof(id));
        id.bytes[0] = (uint8_t)(i & 0xFF);
        id.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        id.bytes[2] = (uint8_t)((i >> 16) & 0xFF);
        id.bytes[15] = 0xAA;

        keystone_exact_entry_t e = {
            .resource_id = id,
            .latest_generation = i,
            .fencing_epoch = 1,
            .object_type = KEYSTONE_OBJ_VM,
            .location_slot = i * 8
        };
        TEST_ASSERT(keystone_exact_index_upsert(idx, &e) == 0);
    }
    TEST_ASSERT(keystone_exact_index_count(idx) == SCALE_COUNT + 1);

    /* Spot check */
    for (size_t i = 1; i <= 100; i++) {
        keystone_uuid_t id;
        memset(&id, 0, sizeof(id));
        id.bytes[0] = (uint8_t)(i & 0xFF);
        id.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        id.bytes[2] = (uint8_t)((i >> 16) & 0xFF);
        id.bytes[15] = 0xAA;

        TEST_ASSERT(keystone_exact_index_lookup(idx, &id, &found) == 0);
        TEST_ASSERT(found.latest_generation == i);
        TEST_ASSERT(found.location_slot == i * 8);
    }

    /* 9. Save and Load roundtrip */
    const char* exact_path = "/tmp/keystone_exact_test.bin";
    TEST_ASSERT(keystone_exact_index_save(idx, exact_path) == 0);

    keystone_exact_index_t* loaded_idx = NULL;
    TEST_ASSERT(keystone_exact_index_load(&loaded_idx, exact_path) == 0);
    TEST_ASSERT(loaded_idx != NULL);
    TEST_ASSERT(keystone_exact_index_count(loaded_idx) == SCALE_COUNT + 1);

    /* Verify original vm_id in loaded index */
    TEST_ASSERT(keystone_exact_index_lookup(loaded_idx, &vm_id, &found) == 0);
    TEST_ASSERT(found.latest_generation == 102);
    TEST_ASSERT((found.flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) != 0);

    unlink(exact_path);
    keystone_exact_index_destroy(idx);
    keystone_exact_index_destroy(loaded_idx);

    printf("    [+] Exact identity index verified (100%% green).\n");
}

static void test_temporal_index_lifecycle(void) {
    printf("[*] Testing monotonic temporal index lifecycle...\n");

    keystone_temporal_index_t* tidx = keystone_temporal_index_create(256);
    TEST_ASSERT(tidx != NULL);

    keystone_uuid_t vm_a, vm_b;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000001", &vm_a);
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000002", &vm_b);

    /* Insert 1,000 events: 500 for vm_a, 500 for vm_b interleaved */
    for (size_t i = 0; i < 1000; i++) {
        keystone_temporal_entry_t te = {
            .hlc = { .physical_ms = 1000000 + (i * 10), .logical = 0, .node_id = 1 },
            .object_id = (i % 2 == 0) ? vm_a : vm_b,
            .event_type = KEYSTONE_OBJ_VM,
            .tenant_id = 1,
            .classification = KEYSTONE_CLASSIFICATION_OPS,
            .flags = (i % 50 == 0) ? KEYSTONE_RECORD_FLAG_TOMBSTONE : 0,
            .source_generation = i + 1,
            .payload_offset = i * 64
        };
        TEST_ASSERT(keystone_temporal_index_append(tidx, &te) == 0);
    }
    TEST_ASSERT(keystone_temporal_index_count(tidx) == 1000);

    /* 1. Range Query [1000100 ms, 1000500 ms] */
    keystone_hlc_t hlc_min = { .physical_ms = 1000100, .logical = 0, .node_id = 0 };
    keystone_hlc_t hlc_max = { .physical_ms = 1000500, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };

    keystone_temporal_entry_t results[64];
    size_t total_in_range = keystone_temporal_index_query_range(tidx, &hlc_min, &hlc_max, results, 64);
    /* i=10 (1000100) to i=50 (1000500) inclusive = 41 items */
    TEST_ASSERT(total_in_range == 41);
    TEST_ASSERT(results[0].hlc.physical_ms == 1000100);
    TEST_ASSERT(results[40].hlc.physical_ms == 1000500);

    /* 2. Object-Scoped Query for vm_a in that range */
    keystone_temporal_entry_t obj_results[64];
    size_t vm_a_matches = keystone_temporal_index_query_object(tidx, &vm_a, &hlc_min, &hlc_max, obj_results, 64);
    /* Even indices between 10 and 50 inclusive: 10, 12, ..., 50 -> 21 matches */
    TEST_ASSERT(vm_a_matches == 21);
    for (size_t j = 0; j < vm_a_matches; j++) {
        TEST_ASSERT(keystone_uuid_equal(&obj_results[j].object_id, &vm_a));
    }

    /* 3. Reverse-Chronological Scan */
    keystone_temporal_entry_t rev_results[10];
    size_t rev_count = keystone_temporal_index_scan_reverse(tidx, NULL, rev_results, 10);
    TEST_ASSERT(rev_count == 10);
    TEST_ASSERT(rev_results[0].hlc.physical_ms == 1000000 + (999 * 10));
    TEST_ASSERT(rev_results[9].hlc.physical_ms == 1000000 + (990 * 10));
    for (size_t j = 0; j < 9; j++) {
        TEST_ASSERT(rev_results[j].hlc.physical_ms > rev_results[j + 1].hlc.physical_ms);
    }

    /* 4. Time Bucket Aggregation (100 ms buckets across 500 ms) */
    keystone_temporal_bucket_t buckets[10];
    size_t num_buckets = keystone_temporal_index_aggregate_buckets(tidx, 1000000, 1000500, 100, buckets, 10);
    TEST_ASSERT(num_buckets == 5);
    for (size_t b = 0; b < 5; b++) {
        TEST_ASSERT(buckets[b].event_count == 10); /* 10 events per 100ms */
    }

    /* 5. Out-of-Order Append Handling */
    keystone_temporal_entry_t ooo_entry = {
        .hlc = { .physical_ms = 1000255, .logical = 1, .node_id = 99 },
        .object_id = vm_a,
        .event_type = KEYSTONE_OBJ_VM,
        .source_generation = 9999
    };
    TEST_ASSERT(keystone_temporal_index_append(tidx, &ooo_entry) == 0);
    TEST_ASSERT(keystone_temporal_index_count(tidx) == 1001);

    /* Verify timeline monotonicity after out-of-order insertion */
    keystone_hlc_t ooo_min = { .physical_ms = 1000250, .logical = 0, .node_id = 0 };
    keystone_hlc_t ooo_max = { .physical_ms = 1000259, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };
    keystone_temporal_entry_t ooo_res[5];
    size_t ooo_found = keystone_temporal_index_query_range(tidx, &ooo_min, &ooo_max, ooo_res, 5);
    TEST_ASSERT(ooo_found == 2); /* 1000250 and 1000255 */
    TEST_ASSERT(ooo_res[0].hlc.physical_ms == 1000250);
    TEST_ASSERT(ooo_res[1].hlc.physical_ms == 1000255);
    TEST_ASSERT(ooo_res[1].source_generation == 9999);

    /* 6. Binary Persistence */
    const char* temporal_path = "/tmp/keystone_temporal_test.bin";
    TEST_ASSERT(keystone_temporal_index_save(tidx, temporal_path) == 0);

    keystone_temporal_index_t* loaded_tidx = NULL;
    TEST_ASSERT(keystone_temporal_index_load(&loaded_tidx, temporal_path) == 0);
    TEST_ASSERT(loaded_tidx != NULL);
    TEST_ASSERT(keystone_temporal_index_count(loaded_tidx) == 1001);

    /* Query loaded index */
    size_t loaded_matches = keystone_temporal_index_query_range(loaded_tidx, &hlc_min, &hlc_max, results, 64);
    TEST_ASSERT(loaded_matches == 42); /* 41 + 1 OOO entry */

    unlink(temporal_path);
    keystone_temporal_index_destroy(tidx);
    keystone_temporal_index_destroy(loaded_tidx);

    printf("    [+] Monotonic temporal index verified (100%% green).\n");
}

/*
 * Feed-shaped resume reproducer (R6b journal feed, 2026-09-26).
 *
 * keystone_temporal_index_load claimed capacity 16 for any loaded count
 * <= 16 but allocated only `count` entry slots, so appends past a small
 * loaded array wrote past the end of the allocation (valgrind: invalid
 * write of size 8 at the block boundary). The production feed worked
 * around it by rebuilding the temporal index from its spool instead of
 * resuming the persisted one.
 */
static void test_temporal_load_then_append_resume(void) {
    printf("[*] Testing temporal index save/load/append resume (feed-shaped)...\n");

    const char* path = "/tmp/keystone_temporal_resume_test.bin";

    /* 1. Small index (5 entries — inside the <=16 danger window) */
    keystone_temporal_index_t* tidx = keystone_temporal_index_create(8);
    TEST_ASSERT(tidx != NULL);
    for (size_t i = 0; i < 5; i++) {
        keystone_temporal_entry_t te = {
            .hlc = { .physical_ms = 2000000 + (i * 10), .logical = 0, .node_id = 1 },
            .event_type = KEYSTONE_OBJ_LOG_STREAM,
            .tenant_id = 7,
            .classification = KEYSTONE_CLASSIFICATION_RESTRICTED,
            .flags = KEYSTONE_RECORD_FLAG_AUDIT,
            .source_generation = i + 1,
            .payload_offset = i * 128
        };
        TEST_ASSERT(keystone_temporal_index_append(tidx, &te) == 0);
    }
    TEST_ASSERT(keystone_temporal_index_count(tidx) == 5);
    TEST_ASSERT(keystone_temporal_index_save(tidx, path) == 0);
    keystone_temporal_index_destroy(tidx);

    /* 2. Resume: load, then keep appending. Entries 6..16 land in the
     * claimed-but-unallocated capacity region (heap-buffer-overflow under
     * ASan pre-fix); entries 17..21 force the growth path from a loaded
     * index. */
    keystone_temporal_index_t* loaded = NULL;
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == 0);
    TEST_ASSERT(loaded != NULL);
    TEST_ASSERT(keystone_temporal_index_count(loaded) == 5);

    for (size_t i = 5; i < 21; i++) {
        keystone_temporal_entry_t te = {
            .hlc = { .physical_ms = 2000000 + (i * 10), .logical = 0, .node_id = 1 },
            .event_type = KEYSTONE_OBJ_LOG_STREAM,
            .tenant_id = 7,
            .classification = KEYSTONE_CLASSIFICATION_RESTRICTED,
            .flags = KEYSTONE_RECORD_FLAG_AUDIT,
            .source_generation = i + 1,
            .payload_offset = i * 128
        };
        TEST_ASSERT(keystone_temporal_index_append(loaded, &te) == 0);
    }
    TEST_ASSERT(keystone_temporal_index_count(loaded) == 21);

    /* 3. Resumed timeline must serve pre- and post-resume entries alike */
    keystone_hlc_t hmin = { .physical_ms = 2000000, .logical = 0, .node_id = 0 };
    keystone_hlc_t hmax = { .physical_ms = 2000200, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };
    keystone_temporal_entry_t out[32];
    TEST_ASSERT(keystone_temporal_index_query_range(loaded, &hmin, &hmax, out, 32) == 21);
    TEST_ASSERT(out[0].hlc.physical_ms == 2000000);
    TEST_ASSERT(out[20].hlc.physical_ms == 2000200);
    TEST_ASSERT(out[20].source_generation == 21);

    /* 4. Second resume cycle with count > 16 (capacity == count region) */
    TEST_ASSERT(keystone_temporal_index_save(loaded, path) == 0);
    keystone_temporal_index_t* reloaded = NULL;
    TEST_ASSERT(keystone_temporal_index_load(&reloaded, path) == 0);
    TEST_ASSERT(keystone_temporal_index_count(reloaded) == 21);

    keystone_temporal_entry_t te = {
        .hlc = { .physical_ms = 2000210, .logical = 0, .node_id = 1 },
        .event_type = KEYSTONE_OBJ_LOG_STREAM,
        .tenant_id = 7,
        .classification = KEYSTONE_CLASSIFICATION_RESTRICTED,
        .flags = KEYSTONE_RECORD_FLAG_AUDIT,
        .source_generation = 22,
        .payload_offset = 21 * 128
    };
    TEST_ASSERT(keystone_temporal_index_append(reloaded, &te) == 0);
    TEST_ASSERT(keystone_temporal_index_count(reloaded) == 22);

    keystone_temporal_index_destroy(loaded);
    keystone_temporal_index_destroy(reloaded);
    unlink(path);

    printf("    [+] Temporal resume-after-load verified (no overflow past loaded array).\n");
}

/* An empty index persisted and resumed must accept its first append. */
static void test_temporal_empty_persist_resume(void) {
    printf("[*] Testing temporal empty-index persist/resume...\n");

    const char* path = "/tmp/keystone_temporal_empty_test.bin";

    keystone_temporal_index_t* tidx = keystone_temporal_index_create(4);
    TEST_ASSERT(tidx != NULL);
    TEST_ASSERT(keystone_temporal_index_count(tidx) == 0);
    TEST_ASSERT(keystone_temporal_index_save(tidx, path) == 0);
    keystone_temporal_index_destroy(tidx);

    keystone_temporal_index_t* loaded = NULL;
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == 0);
    TEST_ASSERT(loaded != NULL);
    TEST_ASSERT(keystone_temporal_index_count(loaded) == 0);

    keystone_temporal_entry_t te = {
        .hlc = { .physical_ms = 3000000, .logical = 0, .node_id = 1 },
        .event_type = KEYSTONE_OBJ_LOG_STREAM,
        .tenant_id = 7,
        .flags = KEYSTONE_RECORD_FLAG_AUDIT,
        .source_generation = 1
    };
    TEST_ASSERT(keystone_temporal_index_append(loaded, &te) == 0);
    TEST_ASSERT(keystone_temporal_index_count(loaded) == 1);

    keystone_temporal_index_destroy(loaded);
    unlink(path);

    printf("    [+] Empty-index resume verified.\n");
}

/*
 * Persisted-format hostility: the temporal index file is untrusted input
 * (CITADEL brief #31). Layout (packed, little-endian):
 *   off 0  u32 magic   'KSTM'   off 48 u32 crc32 (hdr[0..48) ^ entries)
 *   off 4  u32 version 1        off 52 u32 reserved (must be 0)
 *   off 8  u64 count            off 56 entries, 80 bytes each
 *   off 16 hlc min/max (32 B)
 */
#define T_HDR_SIZE 56u
#define T_ENTRY_SIZE 80u

static uint32_t temporal_test_crc32(const void* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void temporal_test_store_u32(uint8_t* buf, size_t off, uint32_t v) {
    buf[off] = (uint8_t)v; buf[off + 1] = (uint8_t)(v >> 8);
    buf[off + 2] = (uint8_t)(v >> 16); buf[off + 3] = (uint8_t)(v >> 24);
}

static void temporal_test_store_u64(uint8_t* buf, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) buf[off + (size_t)i] = (uint8_t)(v >> (8 * i));
}

static uint32_t temporal_test_file_crc(const uint8_t* file, size_t len) {
    uint64_t count = 0;
    for (int i = 0; i < 8; i++) count |= (uint64_t)file[8 + i] << (8 * i);
    size_t payload = (size_t)count * T_ENTRY_SIZE;
    uint32_t crc = temporal_test_crc32(file, 48);
    if (payload > 0 && 56 + payload <= len) {
        crc ^= temporal_test_crc32(file + T_HDR_SIZE, payload);
    }
    return crc;
}

static void test_temporal_persistence_rejects_corrupt(void) {
    printf("[*] Testing temporal persisted-format corruption rejection...\n");

    const char* path = "/tmp/keystone_temporal_corrupt_test.bin";

    /* Valid 3-entry reference file */
    keystone_temporal_index_t* tidx = keystone_temporal_index_create(8);
    TEST_ASSERT(tidx != NULL);
    for (size_t i = 0; i < 3; i++) {
        keystone_temporal_entry_t te = {
            .hlc = { .physical_ms = 4000000 + (i * 10), .logical = 0, .node_id = 1 },
            .event_type = KEYSTONE_OBJ_LOG_STREAM,
            .source_generation = i + 1
        };
        TEST_ASSERT(keystone_temporal_index_append(tidx, &te) == 0);
    }
    TEST_ASSERT(keystone_temporal_index_save(tidx, path) == 0);
    keystone_temporal_index_destroy(tidx);

    const size_t valid_len = T_HDR_SIZE + 3 * T_ENTRY_SIZE;
    uint8_t* valid = (uint8_t*)malloc(valid_len);
    uint8_t* buf = (uint8_t*)malloc(valid_len + 4096);
    TEST_ASSERT(valid != NULL && buf != NULL);
    FILE* f = fopen(path, "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(valid, 1, valid_len, f) == valid_len);
    fclose(f);

    keystone_temporal_index_t* loaded = NULL;

    /* 1. Sanity: the untouched file loads */
    memcpy(buf, valid, valid_len);
    FILE* w = fopen(path, "wb");
    TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len);
    fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == 0);
    TEST_ASSERT(keystone_temporal_index_count(loaded) == 3);
    keystone_temporal_index_destroy(loaded);
    loaded = NULL;

    /* 2. Bad magic */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u32(buf, 0, 0xDEADBEEFu);
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 3. Bad version */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u32(buf, 4, 99);
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 4. Nonzero reserved word */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u32(buf, 52, 1);
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 5. Truncated payload (declared count larger than the bytes present) */
    memcpy(buf, valid, valid_len);
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len - 10, w) == valid_len - 10); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 6. Trailing garbage after the declared payload */
    memcpy(buf, valid, valid_len);
    buf[valid_len] = 0x41;
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len + 1, w) == valid_len + 1); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 7. Absurd declared count (allocation bomb attempt, stale CRC) */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u64(buf, 8, 0x4000000000000000ULL);
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 8. CRC-valid lie: count patched down with recomputed CRC, leaving
     * undeclared trailing entries — the loader must reject by explicit
     * length, not trust the (self-consistent) header. */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u64(buf, 8, 1);
    temporal_test_store_u32(buf, 48, temporal_test_file_crc(buf, valid_len));
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 9. Corrupted entry byte (CRC mismatch) */
    memcpy(buf, valid, valid_len);
    buf[T_HDR_SIZE + 5] ^= 0xFF;
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(buf, 1, valid_len, w) == valid_len); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 10. Header-only stub and empty file */
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL && fwrite(valid, 1, 20, w) == 20); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);
    w = fopen(path, "wb"); TEST_ASSERT(w != NULL); fclose(w);
    TEST_ASSERT(keystone_temporal_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    free(valid);
    free(buf);
    unlink(path);

    printf("    [+] Corrupt/truncated/malformed temporal files all rejected.\n");
}

/*
 * Persisted-format hostility for the exact identity index (same policy as
 * the temporal matrix above). Layout (packed):
 *   off 0  u32 magic 'KSXI'    off 32 u32 crc32 (hdr[0..32) ^ entries)
 *   off 4  u32 version 1       off 36 u32 reserved (must be 0)
 *   off 8  u64 count           off 40 entries, sizeof(entry) bytes each
 *   off 16 u64 active_count
 *   off 24 u64 tombstone_count
 */
#define E_HDR_SIZE 40u

static uint32_t exact_test_file_crc(const uint8_t* file, size_t len, size_t entry_size) {
    uint64_t count = 0;
    for (int i = 0; i < 8; i++) count |= (uint64_t)file[8 + i] << (8 * i);
    size_t payload = (size_t)count * entry_size;
    uint32_t crc = temporal_test_crc32(file, 32);
    if (payload > 0 && E_HDR_SIZE + payload <= len) {
        crc ^= temporal_test_crc32(file + E_HDR_SIZE, payload);
    }
    return crc;
}

static void test_exact_persistence_rejects_corrupt(void) {
    printf("[*] Testing exact-index persisted-format corruption rejection...\n");

    const char* path = "/tmp/keystone_exact_corrupt_test.bin";
    const size_t entry_size = sizeof(keystone_exact_entry_t);

    /* Valid 3-entry reference file */
    keystone_exact_index_t* idx = keystone_exact_index_create(8);
    TEST_ASSERT(idx != NULL);
    for (size_t i = 0; i < 3; i++) {
        keystone_uuid_t id;
        memset(&id, 0, sizeof(id));
        id.bytes[0] = (uint8_t)(i + 1);
        keystone_exact_entry_t e = {
            .resource_id = id,
            .latest_generation = i + 1,
            .fencing_epoch = 1
        };
        TEST_ASSERT(keystone_exact_index_upsert(idx, &e) == 0);
    }
    TEST_ASSERT(keystone_exact_index_save(idx, path) == 0);
    keystone_exact_index_destroy(idx);

    const size_t valid_len = E_HDR_SIZE + 3 * entry_size;
    uint8_t* valid = (uint8_t*)malloc(valid_len);
    uint8_t* buf = (uint8_t*)malloc(valid_len + 4096);
    TEST_ASSERT(valid != NULL && buf != NULL);
    FILE* f = fopen(path, "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(valid, 1, valid_len, f) == valid_len);
    fclose(f);

    keystone_exact_index_t* loaded = NULL;
    FILE* w;

#define EXACT_WRITE_FILE(len) do { \
    w = fopen(path, "wb"); \
    TEST_ASSERT(w != NULL && fwrite(buf, 1, (len), w) == (len)); \
    fclose(w); \
} while (0)

    /* 1. Sanity: the untouched file loads */
    memcpy(buf, valid, valid_len);
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == 0);
    TEST_ASSERT(loaded != NULL);
    TEST_ASSERT(keystone_exact_index_count(loaded) == 3);
    keystone_exact_index_destroy(loaded);
    loaded = NULL;

    /* 2. Bad magic */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u32(buf, 0, 0xDEADBEEFu);
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 3. Bad version */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u32(buf, 4, 99);
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 4. Nonzero reserved word */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u32(buf, 36, 1);
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 5. Truncated payload */
    memcpy(buf, valid, valid_len);
    EXACT_WRITE_FILE(valid_len - 7);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 6. Trailing garbage after the declared payload */
    memcpy(buf, valid, valid_len);
    buf[valid_len] = 0x41;
    EXACT_WRITE_FILE(valid_len + 1);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 7. Absurd declared count (allocation bomb attempt) */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u64(buf, 8, 0x4000000000000000ULL);
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 8. CRC-valid lie: count patched down, CRC recomputed, trailing
     * entries left in the file — must be rejected by explicit length. */
    memcpy(buf, valid, valid_len);
    temporal_test_store_u64(buf, 8, 1);
    temporal_test_store_u32(buf, 32, exact_test_file_crc(buf, valid_len, entry_size));
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 9. Corrupted entry byte (CRC mismatch) */
    memcpy(buf, valid, valid_len);
    buf[E_HDR_SIZE + 5] ^= 0xFF;
    EXACT_WRITE_FILE(valid_len);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

    /* 10. Header-only stub and empty file */
    EXACT_WRITE_FILE(20);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);
    EXACT_WRITE_FILE(0);
    TEST_ASSERT(keystone_exact_index_load(&loaded, path) == -1);
    TEST_ASSERT(loaded == NULL);

#undef EXACT_WRITE_FILE
    free(valid);
    free(buf);
    unlink(path);

    printf("    [+] Corrupt/truncated/malformed exact-index files all rejected.\n");
}

static void test_high_throughput_query_benchmark(void) {
    printf("[*] Running 100,000 temporal range query benchmark...\n");

    keystone_temporal_index_t* tidx = keystone_temporal_index_create(65536);
    TEST_ASSERT(tidx != NULL);

    const size_t TOTAL_EVENTS = 50000;
    for (size_t i = 0; i < TOTAL_EVENTS; i++) {
        keystone_temporal_entry_t te = {
            .hlc = { .physical_ms = 1000000 + i, .logical = 0, .node_id = 1 },
            .event_type = KEYSTONE_OBJ_VM,
            .source_generation = i
        };
        keystone_temporal_index_append(tidx, &te);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    const size_t QUERY_COUNT = 100000;
    keystone_temporal_entry_t buffer[16];
    size_t total_hits = 0;

    for (size_t q = 0; q < QUERY_COUNT; q++) {
        uint64_t target_ms = 1000000 + (q % (TOTAL_EVENTS - 10));
        keystone_hlc_t hmin = { .physical_ms = target_ms, .logical = 0, .node_id = 0 };
        keystone_hlc_t hmax = { .physical_ms = target_ms + 5, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };

        total_hits += keystone_temporal_index_query_range(tidx, &hmin, &hmax, buffer, 16);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_sec = (double)(end.tv_sec - start.tv_sec) +
                         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    double queries_per_sec = (double)QUERY_COUNT / elapsed_sec;
    double ns_per_query = (elapsed_sec / (double)QUERY_COUNT) * 1e9;

    printf("    [+] Executed %zu queries in %.3f s (%.1f queries/sec, %.1f ns/query, total_hits=%zu)\n",
           QUERY_COUNT, elapsed_sec, queries_per_sec, ns_per_query, total_hits);

    /*
     * Perf gate policy: 500 ns/query is the performance *budget* (tracked
     * via the benchmark harness; warn here when exceeded), 5000 ns is the
     * regression *ceiling* that still indicates an algorithmic fault.
     * The ceiling is skipped under sanitizers (instrumentation multiplies
     * latency) and on demand via KEYSTONE_SKIP_PERF_GATES (the valgrind
     * target sets it) — a loaded host must never flake the correctness
     * suite, but a 10x regression must always fail it.
     */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#    define KS_TEST_SANITIZED 1
#  endif
#elif defined(__SANITIZE_ADDRESS__)
#  define KS_TEST_SANITIZED 1
#endif
#ifndef KS_TEST_SANITIZED
    if (!getenv("KEYSTONE_SKIP_PERF_GATES")) {
        if (ns_per_query > 500.0) {
            printf("    [!] WARN: %.1f ns/query exceeds the 500 ns budget "
                   "(host load or regression — see benchmarks/bench_federation)\n",
                   ns_per_query);
        }
        TEST_ASSERT(ns_per_query < 5000.0);
    }
#endif

    keystone_temporal_index_destroy(tidx);
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Phase 1: Exact Identity & Temporal Index Tests\n");
    printf("=================================================================\n");

    test_exact_identity_lifecycle();
    test_exact_persistence_rejects_corrupt();
    test_temporal_index_lifecycle();
    test_temporal_load_then_append_resume();
    test_temporal_empty_persist_resume();
    test_temporal_persistence_rejects_corrupt();
    test_high_throughput_query_benchmark();

    printf("=================================================================\n");
    printf("  ALL PHASE 1 EXACT & TEMPORAL INDEX TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
