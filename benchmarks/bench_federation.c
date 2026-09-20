/**
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 SWORDIntel. All rights reserved.
 *
 * bench_federation.c
 *
 * Comprehensive benchmark suite for KEYSTONE CITADEL Federation Intelligence
 * additions across all 8 phases (Phases 0 through 7):
 * 1. Federation Wire Envelope & Deduplication Ingest (Phase 0)
 * 2. Exact Identity Directory (Phase 1)
 * 3. Monotonic Temporal Timeline Index (Phase 1)
 * 4. Topology Graph Cache & Two-Tier Hybrid Planner (Phase 3)
 * 5. Streaming Telemetry & Deterministic Anomaly Engine (Phase 4)
 * 6. Incident Vector Similarity & Silicon Dispatch (Phase 5)
 * 7. Federated Distributed Query Coordinator (Phase 6)
 * 8. Grounded AI/RAG Context Retrieval & Citations (Phase 7)
 */

#define _GNU_SOURCE
#include "../include/keystone.h"
#include "../include/keystone_federation.h"
#include "../include/keystone_exact_index.h"
#include "../include/keystone_temporal.h"
#include "../include/keystone_topology.h"
#include "../include/keystone_hybrid.h"
#include "../include/keystone_telemetry.h"
#include "../include/keystone_incident.h"
#include "../include/keystone_federated_query.h"
#include "../include/keystone_rag.h"
#include "benchmark_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

/* High-resolution timer */
static inline uint64_t time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u32(const void *a, const void *b) {
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static long get_current_rss_kb(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss;
    }
    return 0;
}

static void print_header(const char *title) {
    printf("\n=================================================================\n");
    printf("  %s\n", title);
    printf("=================================================================\n");
}

/* ========================================================================= */
/* Benchmark 1: Federation Wire Envelope & Ingestion Deduplication (Phase 0)  */
/* ========================================================================= */
static void bench_federation_ingest(void) {
    print_header("Benchmark 1: Federation Wire Envelope & Ingest Deduplication");

    const size_t TOTAL_EVENTS = 200000;
    const size_t UNIQUE_EVENTS = 100000;

    keystone_ingest_config_t config = {
        .ring_buffer_capacity = 65536,
        .dedup_window_capacity = 262144,
        .tombstone_table_capacity = 8192,
        .initial_fencing_epoch = 1,
        .enable_concurrency = 0
    };

    keystone_federation_ingest_t *ingest = keystone_federation_ingest_create(&config);
    if (!ingest) {
        fprintf(stderr, "Failed to create federation ingest pipeline\n");
        return;
    }

    printf("[*] Generating %zu wire records (%zu unique, %zu duplicates)...\n",
           TOTAL_EVENTS, UNIQUE_EVENTS, TOTAL_EVENTS - UNIQUE_EVENTS);

    keystone_uuid_t obj_id, node_id;
    keystone_uuid_from_string("11111111-1111-1111-1111-111111111111", &obj_id);
    keystone_uuid_from_string("22222222-2222-2222-2222-222222222222", &node_id);

    keystone_federation_record_t *records = malloc(TOTAL_EVENTS * sizeof(keystone_federation_record_t));
    if (!records) {
        keystone_federation_ingest_destroy(ingest);
        return;
    }

    for (size_t i = 0; i < TOTAL_EVENTS; i++) {
        size_t event_idx = (i < UNIQUE_EVENTS) ? i : (i % UNIQUE_EVENTS);

        memset(&records[i], 0, sizeof(keystone_federation_record_t));
        records[i].source_object_id = obj_id;
        records[i].source_event_id.bytes[0] = (uint8_t)(event_idx & 0xFF);
        records[i].source_event_id.bytes[1] = (uint8_t)((event_idx >> 8) & 0xFF);
        records[i].source_event_id.bytes[2] = (uint8_t)((event_idx >> 16) & 0xFF);
        records[i].source_event_id.bytes[3] = (uint8_t)((event_idx >> 24) & 0xFF);
        memset(&records[i].source_event_id.bytes[4], 0xAA, 12);
        records[i].source_node_id = node_id;
        records[i].source_generation = i + 1;
        records[i].fencing_epoch = 1;
        records[i].source_hlc.physical_ms = 100000 + i;
        records[i].source_hlc.logical = 0;
        records[i].source_hlc.node_id = 1;
        records[i].tenant_id = 42;
        records[i].classification = KEYSTONE_CLASSIFICATION_OPS;
        records[i].object_type = KEYSTONE_OBJ_VM;
        records[i].flags = KEYSTONE_RECORD_FLAG_EVENT;
    }

    /* Wire Serialization Benchmark */
    uint8_t *wire_buf = malloc(1024);
    uint64_t ser_start = time_now_ns();
    const size_t SER_ROUNDS = 100000;
    size_t written = 0;
    for (size_t i = 0; i < SER_ROUNDS; i++) {
        keystone_record_serialize(&records[i], wire_buf, 1024, &written);
    }
    uint64_t ser_elapsed = time_now_ns() - ser_start;
    double ser_rate = (double)SER_ROUNDS / ((double)ser_elapsed / 1e9);
    printf("  Wire Serialization Rate:   \033[1;32m%.2f million envelopes/sec\033[0m (%.1f ns/envelope)\n",
           ser_rate / 1e6, (double)ser_elapsed / (double)SER_ROUNDS);
    free(wire_buf);

    /* Ingestion and Deduplication Benchmark */
    printf("[*] Ingesting %zu records through dedup ring with CRC32 & fencing checks...\n", TOTAL_EVENTS);

    uint64_t start_ns = time_now_ns();
    uint32_t ok_count = 0;
    uint32_t dup_count = 0;

    for (size_t i = 0; i < TOTAL_EVENTS; i++) {
        keystone_ingest_status_t st = keystone_federation_ingest_submit(ingest, &records[i]);
        if (st == KEYSTONE_INGEST_OK) {
            ok_count++;
        } else if (st == KEYSTONE_INGEST_DUPLICATE) {
            dup_count++;
        }
    }
    uint64_t elapsed_ns = time_now_ns() - start_ns;
    double elapsed_sec = (double)elapsed_ns / 1e9;
    double ops_per_sec = (double)TOTAL_EVENTS / elapsed_sec;
    double ns_per_event = (double)elapsed_ns / (double)TOTAL_EVENTS;

    printf("  Ingest & Dedup Results:\n");
    printf("    - Total Events:       %zu\n", TOTAL_EVENTS);
    printf("    - Unique Ingested:    %u (Expected %zu)\n", ok_count, UNIQUE_EVENTS);
    printf("    - Duplicates Blocked: %u (Expected %zu)\n", dup_count, TOTAL_EVENTS - UNIQUE_EVENTS);
    printf("    - Total Ingest Time:  %.3f s\n", elapsed_sec);
    printf("    - Ingest Throughput:  \033[1;32m%.2f million events/sec\033[0m\n", ops_per_sec / 1e6);
    printf("    - Average Latency:    \033[1;32m%.1f ns/event\033[0m\n", ns_per_event);
    printf("    - Max RSS:            %ld kB\n", get_current_rss_kb());

    free(records);
    keystone_federation_ingest_destroy(ingest);
}

/* ========================================================================= */
/* Benchmark 2: Exact Identity Directory Lookup (Phase 1)                     */
/* ========================================================================= */
static void bench_exact_identity(void) {
    print_header("Benchmark 2: Exact Identity Directory (O(1) Collision-Safe Hash)");

    const uint32_t OBJECT_COUNT = 200000;
    const uint32_t LOOKUP_COUNT = 500000;

    keystone_exact_index_t *exact_idx = keystone_exact_index_create(OBJECT_COUNT * 2);
    if (!exact_idx) {
        fprintf(stderr, "Failed to create exact index\n");
        return;
    }

    keystone_uuid_t *keys = malloc(OBJECT_COUNT * sizeof(keystone_uuid_t));
    if (!keys) {
        keystone_exact_index_destroy(exact_idx);
        return;
    }

    for (uint32_t i = 0; i < OBJECT_COUNT; i++) {
        keys[i].bytes[0] = (uint8_t)(i & 0xFF);
        keys[i].bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        keys[i].bytes[2] = (uint8_t)((i >> 16) & 0xFF);
        keys[i].bytes[3] = 0xAA;
        keys[i].bytes[4] = 0xBB;
        for (int b = 5; b < 16; b++) keys[i].bytes[b] = (uint8_t)(b * 7 + i);
    }

    printf("[*] Inserting %u objects into open-addressing table...\n", OBJECT_COUNT);
    uint64_t insert_start = time_now_ns();
    for (uint32_t i = 0; i < OBJECT_COUNT; i++) {
        keystone_exact_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.resource_id = keys[i];
        entry.latest_generation = 42;
        entry.fencing_epoch = 1;
        entry.latest_hlc.physical_ms = 1700000000000ULL + i;
        entry.latest_hlc.logical = 0;
        entry.latest_hlc.node_id = 1;
        entry.location_slot = i;
        entry.tenant_id = 1001;
        keystone_exact_index_upsert(exact_idx, &entry);
    }
    uint64_t insert_elapsed = time_now_ns() - insert_start;
    double insert_rate = (double)OBJECT_COUNT / ((double)insert_elapsed / 1e9);
    printf("  Insert Rate: %.2f million inserts/sec (%.1f ns/insert)\n",
           insert_rate / 1e6, (double)insert_elapsed / (double)OBJECT_COUNT);

    printf("[*] Executing %u lookups (80%% hits, 20%% misses)...\n", LOOKUP_COUNT);
    uint32_t *latencies = malloc(LOOKUP_COUNT * sizeof(uint32_t));
    if (!latencies) {
        free(keys);
        keystone_exact_index_destroy(exact_idx);
        return;
    }

    uint32_t hits = 0;
    uint64_t total_start = time_now_ns();

    for (uint32_t i = 0; i < LOOKUP_COUNT; i++) {
        keystone_uuid_t q;
        if ((i % 5) != 0) {
            q = keys[i % OBJECT_COUNT];
        } else {
            memset(&q, 0xFF, sizeof(q));
            q.bytes[0] = (uint8_t)(i & 0xFF);
            q.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        }

        uint64_t q_start = time_now_ns();
        keystone_exact_entry_t result;
        int rc = keystone_exact_index_lookup(exact_idx, &q, &result);
        uint64_t q_elapsed = time_now_ns() - q_start;

        latencies[i] = (uint32_t)q_elapsed;
        if (rc == 0) hits++;
    }
    uint64_t total_elapsed = time_now_ns() - total_start;

    qsort(latencies, LOOKUP_COUNT, sizeof(uint32_t), compare_u32);
    uint32_t p50 = latencies[(LOOKUP_COUNT * 50) / 100];
    uint32_t p95 = latencies[(LOOKUP_COUNT * 95) / 100];
    uint32_t p99 = latencies[(LOOKUP_COUNT * 99) / 100];

    double lookup_rate = (double)LOOKUP_COUNT / ((double)total_elapsed / 1e9);

    printf("  Exact Lookup Results:\n");
    printf("    - Total Lookups:  %u (Hits: %u, Misses: %u)\n", LOOKUP_COUNT, hits, LOOKUP_COUNT - hits);
    printf("    - Throughput:     \033[1;32m%.2f million lookups/sec\033[0m\n", lookup_rate / 1e6);
    printf("    - p50 Latency:    \033[1;32m%u ns\033[0m\n", p50);
    printf("    - p95 Latency:    %u ns\n", p95);
    printf("    - p99 Latency:    %u ns\n", p99);

    free(latencies);
    free(keys);
    keystone_exact_index_destroy(exact_idx);
}

/* ========================================================================= */
/* Benchmark 3: Monotonic Temporal Timeline Index (Phase 1)                   */
/* ========================================================================= */
static void bench_temporal_timeline(void) {
    print_header("Benchmark 3: Monotonic Temporal Timeline Range Search");

    const uint32_t EVENT_COUNT = 200000;
    const uint32_t QUERY_COUNT = 200000;

    keystone_temporal_index_t *timeline = keystone_temporal_index_create(EVENT_COUNT);
    if (!timeline) {
        fprintf(stderr, "Failed to create temporal index\n");
        return;
    }

    printf("[*] Appending %u monotonic timeline events...\n", EVENT_COUNT);
    uint64_t start_ms = 1700000000000ULL;

    for (uint32_t i = 0; i < EVENT_COUNT; i++) {
        keystone_temporal_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.hlc.physical_ms = start_ms + (i * 10);
        entry.hlc.logical = 0;
        entry.hlc.node_id = (i % 4) + 1;
        entry.event_id.bytes[0] = (uint8_t)(i & 0xFF);
        entry.event_id.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        entry.object_id.bytes[0] = (uint8_t)((i % 1000) & 0xFF);
        entry.object_id.bytes[1] = (uint8_t)(((i % 1000) >> 8) & 0xFF);
        entry.source_generation = 42;
        entry.event_type = (i % 5) + 1;
        entry.tenant_id = 1001;

        keystone_temporal_index_append(timeline, &entry);
    }

    printf("[*] Running %u binary-search range queries (varying windows)...\n", QUERY_COUNT);
    uint32_t *latencies = malloc(QUERY_COUNT * sizeof(uint32_t));
    if (!latencies) {
        keystone_temporal_index_destroy(timeline);
        return;
    }

    keystone_temporal_entry_t *result_buffer = malloc(512 * sizeof(keystone_temporal_entry_t));
    if (!result_buffer) {
        free(latencies);
        keystone_temporal_index_destroy(timeline);
        return;
    }

    uint64_t total_hits = 0;
    uint64_t q_total_start = time_now_ns();

    for (uint32_t i = 0; i < QUERY_COUNT; i++) {
        uint32_t base_idx = (i * 17) % (EVENT_COUNT - 50);
        uint64_t q_start_ms = start_ms + (base_idx * 10);
        uint64_t q_end_ms = q_start_ms + 150;

        keystone_hlc_t r_start = { .physical_ms = q_start_ms, .logical = 0, .node_id = 0 };
        keystone_hlc_t r_end = { .physical_ms = q_end_ms, .logical = UINT32_MAX, .node_id = UINT32_MAX };

        uint64_t t0 = time_now_ns();
        size_t count = keystone_temporal_index_query_range(timeline, &r_start, &r_end, result_buffer, 512);
        uint64_t dt = time_now_ns() - t0;

        latencies[i] = (uint32_t)dt;
        total_hits += count;
    }
    uint64_t q_total_elapsed = time_now_ns() - q_total_start;

    qsort(latencies, QUERY_COUNT, sizeof(uint32_t), compare_u32);
    uint32_t p50 = latencies[(QUERY_COUNT * 50) / 100];
    uint32_t p95 = latencies[(QUERY_COUNT * 95) / 100];
    uint32_t p99 = latencies[(QUERY_COUNT * 99) / 100];

    double range_rate = (double)QUERY_COUNT / ((double)q_total_elapsed / 1e9);

    printf("  Timeline Range Query Results:\n");
    printf("    - Total Range Queries: %u (Total Matches: %lu)\n", QUERY_COUNT, total_hits);
    printf("    - Throughput:          \033[1;32m%.2f million queries/sec\033[0m\n", range_rate / 1e6);
    printf("    - p50 Latency:         \033[1;32m%u ns\033[0m\n", p50);
    printf("    - p95 Latency:         %u ns\n", p95);
    printf("    - p99 Latency:         %u ns\n", p99);

    const uint32_t HISTO_ROUNDS = 10000;
    printf("[*] Running %u time-bucket histogram aggregations (1-minute buckets)...\n", HISTO_ROUNDS);
    keystone_temporal_bucket_t buckets[64];

    uint64_t h_start = time_now_ns();
    for (uint32_t i = 0; i < HISTO_ROUNDS; i++) {
        uint64_t h_from = start_ms + (i * 100);
        uint64_t h_to = h_from + 600000;
        keystone_temporal_index_aggregate_buckets(timeline, h_from, h_to, 60000, buckets, 64);
    }
    uint64_t h_elapsed = time_now_ns() - h_start;
    double histo_rate = (double)HISTO_ROUNDS / ((double)h_elapsed / 1e9);
    printf("  Histogram Aggregation Rate: \033[1;32m%.2f k-aggregations/sec\033[0m (%.1f µs/aggregation)\n",
           histo_rate / 1e3, (double)h_elapsed / (double)HISTO_ROUNDS / 1e3);

    free(result_buffer);
    free(latencies);
    keystone_temporal_index_destroy(timeline);
}

/* ========================================================================= */
/* Benchmark 4: Topology Graph & Two-Tier Hybrid Query Planner (Phase 3)      */
/* ========================================================================= */
static void bench_topology_hybrid_planner(void) {
    print_header("Benchmark 4: Topology Graph Cache & Two-Tier Hybrid Planner");

    const uint32_t NODE_COUNT = 1000;
    const uint32_t EDGE_COUNT = 10000;
    const uint32_t PLAN_QUERIES = 1000;

    keystone_topology_graph_t *topo = keystone_topology_create(NODE_COUNT, EDGE_COUNT);
    if (!topo) {
        fprintf(stderr, "Failed to create topology index\n");
        return;
    }

    printf("[*] Populating topology graph (%u nodes, %u edges)...\n", NODE_COUNT, EDGE_COUNT);

    keystone_uuid_t node_uuids[1000];
    for (uint32_t i = 0; i < NODE_COUNT; i++) {
        node_uuids[i].bytes[0] = (uint8_t)(i & 0xFF);
        node_uuids[i].bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        for (int b = 2; b < 16; b++) node_uuids[i].bytes[b] = (uint8_t)(b * 13 + i);

        keystone_node_metrics_t nm;
        memset(&nm, 0, sizeof(nm));
        nm.node_id = node_uuids[i];
        nm.isa_features = (i % 2 == 0) ? (KEYSTONE_CPU_AVX2 | KEYSTONE_CPU_AVX512 | KEYSTONE_CPU_AMX) : KEYSTONE_CPU_AVX2;
        nm.ram_total_mb = 262144;
        nm.ram_available_mb = 32768 + (i * 123) % 196608;
        nm.cpu_cores = 64;
        nm.cpu_usage_pct = (float)(10 + (i * 17) % 80);
        nm.temperature_c = (float)(35 + (i * 7) % 45);
        nm.numa_nodes = 2;
        nm.failure_domain_id = (i % 10) + 1;
        nm.classification = KEYSTONE_CLASSIFICATION_SECRET;

        keystone_topology_upsert_node(topo, &nm);
    }

    for (uint32_t i = 0; i < EDGE_COUNT; i++) {
        uint32_t src = i % NODE_COUNT;
        uint32_t dst = (i * 3 + 7) % NODE_COUNT;
        keystone_edge_type_t et = (keystone_edge_type_t)(1u << (i % 6));
        keystone_topology_add_edge(topo, &node_uuids[src], &node_uuids[dst], et);
    }

    printf("[*] Evaluating %u two-tier placement queries (hard pruning + soft ranking)...\n", PLAN_QUERIES);

    keystone_placement_query_t q = keystone_placement_query_default();
    q.min_classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    q.required_isa_features = KEYSTONE_CPU_AVX2;
    q.min_ram_available_mb = 65536;

    uint32_t *latencies = malloc(PLAN_QUERIES * sizeof(uint32_t));
    keystone_recommendation_t *recs = malloc(8 * sizeof(keystone_recommendation_t));

    keystone_hlc_t now_hlc = { .physical_ms = 1758333000000ULL, .logical = 1, .node_id = 1 };
    uint64_t plan_total_start = time_now_ns();
    uint32_t successful_recs = 0;

    for (uint32_t i = 0; i < PLAN_QUERIES; i++) {
        uint64_t t0 = time_now_ns();
        size_t count = keystone_hybrid_plan_placement(topo, &q, 42, &now_hlc, recs, 8);
        uint64_t dt = time_now_ns() - t0;

        latencies[i] = (uint32_t)dt;
        if (count > 0) successful_recs++;
    }
    uint64_t plan_total_elapsed = time_now_ns() - plan_total_start;

    qsort(latencies, PLAN_QUERIES, sizeof(uint32_t), compare_u32);
    uint32_t p50 = latencies[(PLAN_QUERIES * 50) / 100];
    uint32_t p95 = latencies[(PLAN_QUERIES * 95) / 100];
    uint32_t p99 = latencies[(PLAN_QUERIES * 99) / 100];

    double plan_rate = (double)PLAN_QUERIES / ((double)plan_total_elapsed / 1e9);

    printf("  Two-Tier Hybrid Planner Results:\n");
    printf("    - Total Queries:   %u (Passing Recommendations: %u)\n", PLAN_QUERIES, successful_recs);
    printf("    - Planning Rate:   \033[1;32m%.2f k-placements/sec\033[0m\n", plan_rate / 1e3);
    printf("    - p50 Latency:     \033[1;32m%u ns\033[0m (%.2f µs)\n", p50, (double)p50 / 1e3);
    printf("    - p95 Latency:     %u ns (%.2f µs)\n", p95, (double)p95 / 1e3);
    printf("    - p99 Latency:     %u ns (%.2f µs)\n", p99, (double)p99 / 1e3);

    free(recs);
    free(latencies);
    keystone_topology_destroy(topo);
}

/* ========================================================================= */
/* Benchmark 5: Streaming Telemetry & Anomaly Engine (Phase 4)                */
/* ========================================================================= */
static void bench_telemetry_anomaly(void) {
    print_header("Benchmark 5: Streaming Telemetry & Multi-Stage Anomaly Engine");

    const uint32_t SAMPLE_COUNT = 500000;

    keystone_telemetry_engine_t *telemetry = keystone_telemetry_create(SAMPLE_COUNT, 1024, 42);
    if (!telemetry) {
        fprintf(stderr, "Failed to create telemetry engine\n");
        return;
    }

    printf("[*] Streaming %u metric samples into circular rolling windows (1m..24h)...\n", SAMPLE_COUNT);

    keystone_uuid_t node_uuid;
    keystone_uuid_from_string("11111111-2222-3333-4444-555555555555", &node_uuid);

    uint64_t stream_start = time_now_ns();
    uint64_t base_time_ms = 1700000000000ULL;
    uint32_t anomalies_flagged = 0;

    for (uint32_t i = 0; i < SAMPLE_COUNT; i++) {
        keystone_telemetry_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        sample.node_id = node_uuid;
        sample.metric_type = KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT;
        sample.timestamp_hlc.physical_ms = base_time_ms + (i * 10);
        sample.timestamp_hlc.logical = 0;
        sample.timestamp_hlc.node_id = 1;
        sample.value = 50.0 + ((i % 100) * 0.4);

        keystone_anomaly_t anom;
        bool is_anom = false;
        keystone_telemetry_ingest_sample(telemetry, &sample, &anom, &is_anom);
        if (is_anom) anomalies_flagged++;
    }
    uint64_t stream_elapsed = time_now_ns() - stream_start;
    double sample_rate = (double)SAMPLE_COUNT / ((double)stream_elapsed / 1e9);

    printf("  Telemetry Stream Ingestion & Anomaly Results:\n");
    printf("    - Ingested Samples:   %u (Anomalies: %u)\n", SAMPLE_COUNT, anomalies_flagged);
    printf("    - Ingestion Rate:     \033[1;32m%.2f million samples/sec\033[0m\n", sample_rate / 1e6);
    printf("    - Average Latency:    \033[1;32m%.1f ns/sample\033[0m\n", (double)stream_elapsed / (double)SAMPLE_COUNT);

    keystone_telemetry_destroy(telemetry);
}

/* ========================================================================= */
/* Benchmark 6: Incident Similarity Engine & Silicon Acceleration (Phase 5)   */
/* ========================================================================= */
static void bench_incident_similarity(void) {
    print_header("Benchmark 6: Silicon Incident Similarity (64-dim Embeddings)");

    const uint32_t INCIDENT_CORPUS_SIZE = 2000;
    const uint32_t SEARCH_COUNT = 2000;

    keystone_incident_engine_t *incident_engine = keystone_incident_engine_create(INCIDENT_CORPUS_SIZE, 42);
    if (!incident_engine) {
        fprintf(stderr, "Failed to create incident engine\n");
        return;
    }

    printf("[*] Populating incident database with %u normalized 64-dim failure vectors...\n", INCIDENT_CORPUS_SIZE);

    for (uint32_t i = 0; i < INCIDENT_CORPUS_SIZE; i++) {
        keystone_incident_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.incident_id.bytes[0] = (uint8_t)(i & 0xFF);
        rec.incident_id.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
        rec.severity = (i % 4);
        rec.index_generation = 42;
        snprintf(rec.root_cause_label, sizeof(rec.root_cause_label), "FAILURE_TYPE_%u", i % 8);
        snprintf(rec.resolution_notes, sizeof(rec.resolution_notes), "Apply mitigation rule %u", i % 4);

        float norm_sq = 0.0f;
        for (int d = 0; d < KEYSTONE_INCIDENT_EMBED_DIM; d++) {
            rec.embedding[d] = (float)((d * 17 + i * 3) % 100) / 100.0f;
            norm_sq += rec.embedding[d] * rec.embedding[d];
        }
        float inv_norm = 1.0f / sqrtf(norm_sq);
        for (int d = 0; d < KEYSTONE_INCIDENT_EMBED_DIM; d++) rec.embedding[d] *= inv_norm;

        keystone_incident_register(incident_engine, &rec);
    }

    keystone_incident_query_t query;
    memset(&query, 0, sizeof(query));
    for (int d = 0; d < KEYSTONE_INCIDENT_EMBED_DIM; d++) query.query_embedding[d] = 0.125f;
    query.min_similarity_threshold = 0.50f;
    query.force_backend = -1;

    printf("[*] Running %u top-5 incident similarity searches...\n", SEARCH_COUNT);
    uint32_t *latencies = malloc(SEARCH_COUNT * sizeof(uint32_t));
    keystone_incident_match_t top_matches[5];

    uint64_t sim_start = time_now_ns();
    for (uint32_t i = 0; i < SEARCH_COUNT; i++) {
        uint64_t t0 = time_now_ns();
        keystone_incident_search_similar(incident_engine, &query, top_matches, 5);
        uint64_t dt = time_now_ns() - t0;
        latencies[i] = (uint32_t)dt;
    }
    uint64_t sim_elapsed = time_now_ns() - sim_start;

    qsort(latencies, SEARCH_COUNT, sizeof(uint32_t), compare_u32);
    uint32_t p50 = latencies[(SEARCH_COUNT * 50) / 100];
    uint32_t p95 = latencies[(SEARCH_COUNT * 95) / 100];
    uint32_t p99 = latencies[(SEARCH_COUNT * 99) / 100];

    double search_rate = (double)SEARCH_COUNT / ((double)sim_elapsed / 1e9);

    keystone_silicon_backend_t best = keystone_incident_detect_best_backend(INCIDENT_CORPUS_SIZE);

    printf("  Incident Similarity Results:\n");
    printf("    - Corpus Size:      %u historical failure cases\n", INCIDENT_CORPUS_SIZE);
    printf("    - Silicon Backend:  %s\n", keystone_silicon_backend_name(best));
    printf("    - Search Rate:      \033[1;32m%.2f k-searches/sec\033[0m\n", search_rate / 1e3);
    printf("    - p50 Latency:      \033[1;32m%u ns\033[0m (%.2f µs)\n", p50, (double)p50 / 1e3);
    printf("    - p95 Latency:      %u ns (%.2f µs)\n", p95, (double)p95 / 1e3);
    printf("    - p99 Latency:      %u ns (%.2f µs)\n", p99, (double)p99 / 1e3);

    free(latencies);
    keystone_incident_engine_destroy(incident_engine);
}

/* ========================================================================= */
/* Benchmark 7: Federated Distributed Query Coordinator (Phase 6)            */
/* ========================================================================= */
static void bench_federated_coordinator(void) {
    print_header("Benchmark 7: Federated Distributed Query & Multi-Way Merge");

    const uint32_t NODES = 5;
    const uint32_t EVENTS_PER_NODE = 20000;
    const uint32_t TOTAL_EVENTS = NODES * EVENTS_PER_NODE;

    printf("[*] Simulating %u-node federated timeline merge (%u total events)...\n",
           NODES, TOTAL_EVENTS);

    keystone_temporal_entry_t *node_events[5];
    size_t event_counts[5];

    for (uint32_t n = 0; n < NODES; n++) {
        node_events[n] = malloc(EVENTS_PER_NODE * sizeof(keystone_temporal_entry_t));
        event_counts[n] = EVENTS_PER_NODE;
        for (uint32_t i = 0; i < EVENTS_PER_NODE; i++) {
            node_events[n][i].hlc.physical_ms = 1700000000000ULL + (i * NODES * 10) + (n * 10);
            node_events[n][i].hlc.logical = 0;
            node_events[n][i].hlc.node_id = n + 1;
            node_events[n][i].event_id.bytes[0] = (uint8_t)(i & 0xFF);
            node_events[n][i].event_id.bytes[1] = (uint8_t)(n & 0xFF);
            node_events[n][i].object_id.bytes[0] = 0xAA;
            node_events[n][i].source_generation = 100;
            node_events[n][i].event_type = 1;
            node_events[n][i].tenant_id = 1001;
        }
    }

    keystone_temporal_entry_t *merged = malloc(TOTAL_EVENTS * sizeof(keystone_temporal_entry_t));
    keystone_partial_status_t p_status;

    const uint32_t MERGE_ROUNDS = 50;
    uint64_t merge_start = time_now_ns();
    size_t last_merged_count = 0;

    for (uint32_t r = 0; r < MERGE_ROUNDS; r++) {
        last_merged_count = keystone_federated_merge_timelines(
            (const keystone_temporal_entry_t * const *)node_events,
            event_counts, NODES, merged, TOTAL_EVENTS, &p_status);
    }
    uint64_t merge_elapsed = time_now_ns() - merge_start;

    double total_merged_events = (double)TOTAL_EVENTS * (double)MERGE_ROUNDS;
    double event_rate = total_merged_events / ((double)merge_elapsed / 1e9);
    double latency_ms = ((double)merge_elapsed / (double)MERGE_ROUNDS) / 1e6;

    printf("  Federated Multi-Way Merge Results:\n");
    printf("    - Total Merged Events: %zu across %u nodes\n", last_merged_count, NODES);
    printf("    - Merge Throughput:    \033[1;32m%.2f million events/sec\033[0m\n", event_rate / 1e6);
    printf("    - Average Merge Time:  \033[1;32m%.2f ms/merge\033[0m (%u events)\n", latency_ms, TOTAL_EVENTS);

    for (uint32_t n = 0; n < NODES; n++) free(node_events[n]);
    free(merged);
}

/* ========================================================================= */
/* Benchmark 8: Grounded AI/RAG Context Retrieval & Citations (Phase 7)       */
/* ========================================================================= */
static void bench_rag_context(void) {
    print_header("Benchmark 8: Grounded AI/RAG Context Retrieval & Citations");

    const uint32_t RAG_QUERIES = 2000;

    keystone_topology_graph_t *topo = keystone_topology_create(64, 128);
    keystone_temporal_index_t *temporal = keystone_temporal_index_create(1024);
    keystone_telemetry_engine_t *telemetry = keystone_telemetry_create(64, 128, 42);
    keystone_incident_engine_t *incident = keystone_incident_engine_create(64, 42);

    keystone_uuid_t target_vm;
    memset(&target_vm, 0x11, sizeof(target_vm));

    keystone_node_metrics_t nm = {
        .node_id = target_vm,
        .ram_total_mb = 65536,
        .ram_available_mb = 32768,
        .cpu_cores = 32,
        .classification = KEYSTONE_CLASSIFICATION_RESTRICTED
    };
    keystone_topology_upsert_node(topo, &nm);

    keystone_federation_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.source_object_id = target_vm;
    rec.source_hlc = keystone_hlc_now(1);
    rec.classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    keystone_temporal_index_ingest_record(temporal, &rec, 0);

    keystone_rag_engine_t *rag = keystone_rag_create(topo, temporal, telemetry, incident, 77);
    if (!rag) {
        fprintf(stderr, "Failed to create RAG engine\n");
        return;
    }

    keystone_security_context_t caller_sec = {
        .classification = KEYSTONE_CLASSIFICATION_RESTRICTED,
        .compartment_mask = 0xFFFFFFFFFFFFFFFFULL
    };

    printf("[*] Extracting %u citation-backed RAG context packs...\n", RAG_QUERIES);

    uint32_t *latencies = malloc(RAG_QUERIES * sizeof(uint32_t));
    uint64_t rag_start = time_now_ns();

    for (uint32_t i = 0; i < RAG_QUERIES; i++) {
        keystone_context_pack_t pack;

        uint64_t t0 = time_now_ns();
        int rc = keystone_rag_query_context(rag, &target_vm, &caller_sec, 3600000ULL, &pack);
        uint64_t dt = time_now_ns() - t0;

        latencies[i] = (uint32_t)dt;
        (void)rc;
    }
    uint64_t rag_elapsed = time_now_ns() - rag_start;

    qsort(latencies, RAG_QUERIES, sizeof(uint32_t), compare_u32);
    uint32_t p50 = latencies[(RAG_QUERIES * 50) / 100];
    uint32_t p95 = latencies[(RAG_QUERIES * 95) / 100];
    uint32_t p99 = latencies[(RAG_QUERIES * 99) / 100];

    double rag_rate = (double)RAG_QUERIES / ((double)rag_elapsed / 1e9);

    printf("  RAG Context Retrieval Results:\n");
    printf("    - Context Extractions: %u\n", RAG_QUERIES);
    printf("    - Extraction Rate:     \033[1;32m%.2f k-packs/sec\033[0m\n", rag_rate / 1e3);
    printf("    - p50 Latency:         \033[1;32m%u ns\033[0m (%.2f µs)\n", p50, (double)p50 / 1e3);
    printf("    - p95 Latency:         %u ns (%.2f µs)\n", p95, (double)p95 / 1e3);
    printf("    - p99 Latency:         %u ns (%.2f µs)\n", p99, (double)p99 / 1e3);

    free(latencies);
    keystone_rag_destroy(rag);
    keystone_topology_destroy(topo);
    keystone_temporal_index_destroy(temporal);
    keystone_telemetry_destroy(telemetry);
    keystone_incident_engine_destroy(incident);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("\n");
    printf("=================================================================\n");
    printf("   KEYSTONE CITADEL Federation Intelligence Benchmark Suite      \n");
    printf("=================================================================\n");
    printf("  Host PID: %d, System Architecture: x86_64, Standard: C11\n", getpid());
    printf("  Initial RSS: %ld kB\n", get_current_rss_kb());

    bench_federation_ingest();
    bench_exact_identity();
    bench_temporal_timeline();
    bench_topology_hybrid_planner();
    bench_telemetry_anomaly();
    bench_incident_similarity();
    bench_federated_coordinator();
    bench_rag_context();

    printf("\n=================================================================\n");
    printf("  All Federation Intelligence Benchmarks Completed Successfully  \n");
    printf("  Final Peak RSS: %ld kB\n", get_current_rss_kb());
    printf("=================================================================\n\n");

    return 0;
}
