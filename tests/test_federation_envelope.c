/*
 * KEYSTONE Federation Envelope & Ingestion Pipeline Tests
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "keystone_federation.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void test_uuid_primitives(void) {
    printf("[*] Testing UUID primitives...\n");

    keystone_uuid_t nil_id = keystone_uuid_nil();
    TEST_ASSERT(keystone_uuid_is_nil(&nil_id));

    const char* test_str = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
    keystone_uuid_t parsed_id;
    TEST_ASSERT(keystone_uuid_from_string(test_str, &parsed_id) == 0);
    TEST_ASSERT(!keystone_uuid_is_nil(&parsed_id));

    char out_str[37];
    keystone_uuid_to_string(&parsed_id, out_str);
    TEST_ASSERT(strcmp(test_str, out_str) == 0);

    /* Test Equality and Comparison */
    keystone_uuid_t id2 = parsed_id;
    TEST_ASSERT(keystone_uuid_equal(&parsed_id, &id2));
    TEST_ASSERT(keystone_uuid_compare(&parsed_id, &id2) == 0);

    id2.bytes[15]++;
    TEST_ASSERT(!keystone_uuid_equal(&parsed_id, &id2));
    TEST_ASSERT(keystone_uuid_compare(&parsed_id, &id2) < 0);

    /* Test Hash Function */
    uint64_t h1 = keystone_uuid_hash64(&parsed_id);
    uint64_t h2 = keystone_uuid_hash64(&id2);
    TEST_ASSERT(h1 != 0);
    TEST_ASSERT(h1 != h2);

    /* Hostile UUID string parsing */
    keystone_uuid_t bad;
    TEST_ASSERT(keystone_uuid_from_string("invalid-uuid-string", &bad) == -1);
    TEST_ASSERT(keystone_uuid_from_string("6ba7b810-9dad-11d1-80b4-00c04fd430cZ", &bad) == -1);
    TEST_ASSERT(keystone_uuid_from_string("6ba7b8109dad11d180b400c04fd430c80000", &bad) == -1);

    printf("    [+] UUID primitives verified successfully.\n");
}

static void test_hlc_primitives(void) {
    printf("[*] Testing Hybrid Logical Clock (HLC) primitives...\n");

    keystone_hlc_t clock_a = { .physical_ms = 1000, .logical = 0, .node_id = 1 };
    keystone_hlc_t clock_b = { .physical_ms = 1000, .logical = 1, .node_id = 1 };
    keystone_hlc_t clock_c = { .physical_ms = 1001, .logical = 0, .node_id = 1 };
    keystone_hlc_t clock_d = { .physical_ms = 1000, .logical = 0, .node_id = 2 };

    TEST_ASSERT(keystone_hlc_compare(&clock_a, &clock_a) == 0);
    TEST_ASSERT(keystone_hlc_compare(&clock_a, &clock_b) < 0);
    TEST_ASSERT(keystone_hlc_compare(&clock_b, &clock_c) < 0);
    TEST_ASSERT(keystone_hlc_compare(&clock_a, &clock_d) < 0);

    /* Test HLC update with incoming message */
    keystone_hlc_t local = clock_a;
    keystone_hlc_t incoming = clock_b;
    keystone_hlc_t updated = keystone_hlc_update(&local, &incoming);

    TEST_ASSERT(keystone_hlc_compare(&updated, &incoming) > 0);
    TEST_ASSERT(keystone_hlc_compare(&updated, &clock_a) > 0);

    printf("    [+] HLC ordering and causality propagation verified.\n");
}

static void test_wire_serialization(void) {
    printf("[*] Testing wire serialization and hostile-input safety...\n");

    keystone_uuid_t obj_id, evt_id, node_id;
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000001", &obj_id);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000002", &evt_id);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000003", &node_id);

    const char* sample_payload = "{\"action\":\"VM_MIGRATE\",\"source\":\"node-1\",\"target\":\"node-2\"}";
    size_t payload_len = strlen(sample_payload);

    keystone_federation_record_t rec = {
        .source_object_id = obj_id,
        .source_event_id = evt_id,
        .source_node_id = node_id,
        .source_generation = 42,
        .fencing_epoch = 7,
        .source_hlc = { .physical_ms = 1758332400000ULL, .logical = 12, .node_id = 3 },
        .tenant_id = 101,
        .classification = KEYSTONE_CLASSIFICATION_RESTRICTED,
        .object_type = KEYSTONE_OBJ_VM,
        .flags = KEYSTONE_RECORD_FLAG_EVENT | KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE,
        .payload = sample_payload,
        .payload_len = payload_len
    };

    uint8_t buffer[1024];
    size_t written = 0;
    TEST_ASSERT(keystone_record_serialize(&rec, buffer, sizeof(buffer), &written) == 0);
    TEST_ASSERT(written > payload_len);

    /* Deserialize */
    keystone_federation_record_t restored;
    TEST_ASSERT(keystone_record_deserialize(buffer, written, &restored) == 0);

    TEST_ASSERT(keystone_uuid_equal(&rec.source_object_id, &restored.source_object_id));
    TEST_ASSERT(keystone_uuid_equal(&rec.source_event_id, &restored.source_event_id));
    TEST_ASSERT(keystone_uuid_equal(&rec.source_node_id, &restored.source_node_id));
    TEST_ASSERT(restored.source_generation == 42);
    TEST_ASSERT(restored.fencing_epoch == 7);
    TEST_ASSERT(restored.source_hlc.physical_ms == rec.source_hlc.physical_ms);
    TEST_ASSERT(restored.source_hlc.logical == 12);
    TEST_ASSERT(restored.tenant_id == 101);
    TEST_ASSERT(restored.classification == KEYSTONE_CLASSIFICATION_RESTRICTED);
    TEST_ASSERT(restored.object_type == KEYSTONE_OBJ_VM);
    TEST_ASSERT(restored.flags == (KEYSTONE_RECORD_FLAG_EVENT | KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE));
    TEST_ASSERT(restored.payload_len == payload_len);
    TEST_ASSERT(memcmp(restored.payload, sample_payload, payload_len) == 0);

    /* Hostile input 1: Truncated buffer */
    keystone_federation_record_t dummy;
    TEST_ASSERT(keystone_record_deserialize(buffer, 30, &dummy) == -1);

    /* Hostile input 2: Corrupt magic bytes */
    uint8_t corrupted[1024];
    memcpy(corrupted, buffer, written);
    corrupted[0] = 0xDE;
    corrupted[1] = 0xAD;
    TEST_ASSERT(keystone_record_deserialize(corrupted, written, &dummy) == -1);

    /* Hostile input 3: Corrupt payload CRC */
    memcpy(corrupted, buffer, written);
    corrupted[written - 1] ^= 0xFF; /* flip payload bit */
    TEST_ASSERT(keystone_record_deserialize(corrupted, written, &dummy) == -1);

    /* Hostile input 4: Corrupt header field and CRC mismatch */
    memcpy(corrupted, buffer, written);
    corrupted[8] ^= 0x01; /* tamper with flags */
    TEST_ASSERT(keystone_record_deserialize(corrupted, written, &dummy) == -1);

    printf("    [+] Serialization roundtrip and hostile-input protection verified.\n");
}

static void test_ingestion_pipeline(void) {
    printf("[*] Testing ingestion deduplication, fencing, and tombstone mechanics...\n");

    keystone_ingest_config_t config = {
        .ring_buffer_capacity = 1024,
        .dedup_window_capacity = 2048,
        .tombstone_table_capacity = 512,
        .initial_fencing_epoch = 10,
        .enable_concurrency = 1
    };

    keystone_federation_ingest_t* ingest = keystone_federation_ingest_create(&config);
    TEST_ASSERT(ingest != NULL);

    keystone_uuid_t obj_id, evt_id_1, evt_id_2, node_id;
    keystone_uuid_from_string("11111111-2222-3333-4444-555555555555", &obj_id);
    keystone_uuid_from_string("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", &evt_id_1);
    keystone_uuid_from_string("ffffffff-bbbb-cccc-dddd-eeeeeeeeeeee", &evt_id_2);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000001", &node_id);

    /* 1. Stale Epoch Rejection */
    keystone_federation_record_t rec_stale = {
        .source_object_id = obj_id,
        .source_event_id = evt_id_1,
        .source_node_id = node_id,
        .source_generation = 1,
        .fencing_epoch = 9, /* Lower than initial 10 */
        .source_hlc = { .physical_ms = 1000, .logical = 0, .node_id = 1 },
        .tenant_id = 1,
        .flags = KEYSTONE_RECORD_FLAG_EVENT
    };
    TEST_ASSERT(keystone_federation_ingest_submit(ingest, &rec_stale) == KEYSTONE_INGEST_STALE_FENCING_EPOCH);

    /* 2. Valid Ingest */
    keystone_federation_record_t rec1 = rec_stale;
    rec1.fencing_epoch = 10;
    TEST_ASSERT(keystone_federation_ingest_submit(ingest, &rec1) == KEYSTONE_INGEST_OK);

    /* 3. Idempotent Duplicate Rejection */
    TEST_ASSERT(keystone_federation_ingest_submit(ingest, &rec1) == KEYSTONE_INGEST_DUPLICATE);

    /* 4. Advance Fencing Epoch */
    keystone_federation_record_t rec2 = {
        .source_object_id = obj_id,
        .source_event_id = evt_id_2,
        .source_node_id = node_id,
        .source_generation = 2,
        .fencing_epoch = 12, /* Advance epoch */
        .source_hlc = { .physical_ms = 1100, .logical = 0, .node_id = 1 },
        .tenant_id = 1,
        .flags = KEYSTONE_RECORD_FLAG_EVENT
    };
    TEST_ASSERT(keystone_federation_ingest_submit(ingest, &rec2) == KEYSTONE_INGEST_OK);

    /* 5. Tombstone Semantics */
    TEST_ASSERT(!keystone_federation_is_tombstoned(ingest, &obj_id));

    keystone_uuid_t evt_tomb;
    keystone_uuid_from_string("99999999-9999-9999-9999-999999999999", &evt_tomb);
    keystone_federation_record_t rec_tomb = {
        .source_object_id = obj_id,
        .source_event_id = evt_tomb,
        .source_node_id = node_id,
        .source_generation = 3,
        .fencing_epoch = 12,
        .source_hlc = { .physical_ms = 1200, .logical = 0, .node_id = 1 },
        .tenant_id = 1,
        .flags = KEYSTONE_RECORD_FLAG_TOMBSTONE
    };
    TEST_ASSERT(keystone_federation_ingest_submit(ingest, &rec_tomb) == KEYSTONE_INGEST_TOMBSTONE_APPLIED);
    TEST_ASSERT(keystone_federation_is_tombstoned(ingest, &obj_id));

    /* 6. Verify Ingest Stats */
    keystone_ingest_stats_t stats;
    TEST_ASSERT(keystone_federation_get_stats(ingest, &stats) == 0);
    TEST_ASSERT(stats.records_ingested == 3);
    TEST_ASSERT(stats.duplicates_suppressed == 1);
    TEST_ASSERT(stats.stale_epochs_rejected == 1);
    TEST_ASSERT(stats.tombstones_active == 1);
    TEST_ASSERT(stats.current_fencing_epoch == 12);
    TEST_ASSERT(stats.current_generation == 3);

    /* 7. Checkpoint Save & Load */
    const char* cp_path = "/tmp/keystone_test_checkpoint.bin";
    TEST_ASSERT(keystone_federation_save_checkpoint(ingest, cp_path) == 0);

    keystone_federation_ingest_t* ingest_restored = keystone_federation_ingest_create(&config);
    TEST_ASSERT(ingest_restored != NULL);
    TEST_ASSERT(keystone_federation_load_checkpoint(ingest_restored, cp_path) == 0);

    keystone_ingest_stats_t restored_stats;
    TEST_ASSERT(keystone_federation_get_stats(ingest_restored, &restored_stats) == 0);
    TEST_ASSERT(restored_stats.records_ingested == 3);
    TEST_ASSERT(restored_stats.duplicates_suppressed == 1);
    TEST_ASSERT(restored_stats.current_fencing_epoch == 12);
    TEST_ASSERT(restored_stats.current_generation == 3);

    unlink(cp_path);
    keystone_federation_ingest_destroy(ingest);
    keystone_federation_ingest_destroy(ingest_restored);

    printf("    [+] Ingest pipeline, deduplication, fencing, and checkpoints verified.\n");
}

static void test_explainable_recommendations(void) {
    printf("[*] Testing explainable recommendation evidence bundles...\n");

    keystone_uuid_t cand_id;
    keystone_uuid_from_string("c0a80001-0000-0000-0000-000000000042", &cand_id);

    keystone_explain_t explain;
    keystone_explain_init(&explain, &cand_id, KEYSTONE_OBJ_NODE);

    TEST_ASSERT(keystone_explain_add_constraint(&explain, "CLASSIFICATION_CLEARANCE", 1, "Host cleared for SECRET") == 0);
    TEST_ASSERT(keystone_explain_add_constraint(&explain, "MIN_RAM_AVAILABLE", 1, "64GB available >= 32GB required") == 0);
    TEST_ASSERT(keystone_explain_add_constraint(&explain, "HARDWARE_ISA_AVX512", 1, "Host supports AVX-512F/DQ") == 0);
    TEST_ASSERT(keystone_explain_add_constraint(&explain, "SAME_RACK_ANTI_AFFINITY", 0, "Host co-located in Rack 4") == 0);

    keystone_explain_set_ranking(&explain, 0.875, "Ranked #1 due to thermal headroom and NUMA fit");

    TEST_ASSERT(explain.hard_constraint_count == 4);
    TEST_ASSERT(explain.hard_constraints[0].satisfied == 1);
    TEST_ASSERT(explain.hard_constraints[3].satisfied == 0);
    TEST_ASSERT(strcmp(explain.hard_constraints[3].constraint_name, "SAME_RACK_ANTI_AFFINITY") == 0);
    TEST_ASSERT(explain.composite_score == 0.875);

    /* Form complete recommendation structure */
    keystone_recommendation_t rec = {
        .recommended_target_id = cand_id,
        .target_type = KEYSTONE_OBJ_NODE,
        .confidence_score = 0.875,
        .based_on_generation = 104,
        .based_on_hlc = { .physical_ms = 1758332450000ULL, .logical = 4, .node_id = 1 },
        .evidence = explain
    };

    TEST_ASSERT(rec.confidence_score == 0.875);
    TEST_ASSERT(rec.evidence.hard_constraint_count == 4);

    printf("    [+] Recommendation and explanation contracts verified.\n");
}

static void test_high_throughput_ingest_benchmark(void) {
    printf("[*] Running 100,000-event ingestion throughput benchmark...\n");

    keystone_ingest_config_t config = {
        .ring_buffer_capacity = 65536,
        .dedup_window_capacity = 131072,
        .tombstone_table_capacity = 16384,
        .initial_fencing_epoch = 1,
        .enable_concurrency = 0 /* Test raw single-thread ingestion path */
    };

    keystone_federation_ingest_t* ingest = keystone_federation_ingest_create(&config);
    TEST_ASSERT(ingest != NULL);

    const size_t NUM_EVENTS = 100000;
    keystone_uuid_t obj_id, node_id;
    keystone_uuid_from_string("11111111-1111-1111-1111-111111111111", &obj_id);
    keystone_uuid_from_string("22222222-2222-2222-2222-222222222222", &node_id);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < NUM_EVENTS; i++) {
        keystone_uuid_t evt_id;
        evt_id.bytes[0] = (uint8_t)(i & 0xFF);
        evt_id.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        evt_id.bytes[2] = (uint8_t)((i >> 16) & 0xFF);
        evt_id.bytes[3] = (uint8_t)((i >> 24) & 0xFF);
        memset(&evt_id.bytes[4], 0xAA, 12);

        keystone_federation_record_t r = {
            .source_object_id = obj_id,
            .source_event_id = evt_id,
            .source_node_id = node_id,
            .source_generation = i + 1,
            .fencing_epoch = 1,
            .source_hlc = { .physical_ms = 100000 + i, .logical = 0, .node_id = 1 },
            .tenant_id = 42,
            .classification = KEYSTONE_CLASSIFICATION_OPS,
            .object_type = KEYSTONE_OBJ_VM,
            .flags = KEYSTONE_RECORD_FLAG_EVENT
        };

        keystone_ingest_status_t st = keystone_federation_ingest_submit(ingest, &r);
        TEST_ASSERT(st == KEYSTONE_INGEST_OK);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_sec = (double)(end.tv_sec - start.tv_sec) +
                         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    double throughput = (double)NUM_EVENTS / elapsed_sec;

    printf("    [+] Ingested %zu events in %.3f s (%.1f events/sec, %.1f ns/event)\n",
           NUM_EVENTS, elapsed_sec, throughput, (elapsed_sec / (double)NUM_EVENTS) * 1e9);

    TEST_ASSERT(throughput > 50000.0); /* Must exceed 50k events/sec minimum */

    keystone_federation_ingest_destroy(ingest);
}

int main(void) {
    printf("========================================================\n");
    printf("  KEYSTONE Federation Wire Envelope & Ingestion Tests\n");
    printf("========================================================\n");

    test_uuid_primitives();
    test_hlc_primitives();
    test_wire_serialization();
    test_ingestion_pipeline();
    test_explainable_recommendations();
    test_high_throughput_ingest_benchmark();

    printf("========================================================\n");
    printf("  ALL FEDERATION ENVELOPE TESTS PASSED (100%% GREEN)\n");
    printf("========================================================\n");
    return 0;
}
