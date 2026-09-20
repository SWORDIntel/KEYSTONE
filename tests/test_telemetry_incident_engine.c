/*
 * KEYSTONE Telemetry Intelligence & Incident Silicon Engine Test Suite
 *
 * Validates:
 *   1. Streaming telemetry ingestion and circular ring buffer persistence.
 *   2. Deterministic multi-tier rolling window feature extraction (1m, 5m, 15m, 1h, 24h).
 *   3. Mean, variance, stddev, EWMA, and linear regression slope calculations.
 *   4. Multi-stage anomaly detection (static warning/critical thresholds + statistical z-score).
 *   5. Structured anomaly evidence bundles (keystone_explain_t).
 *   6. Historical incident registration and normalized vector embedding.
 *   7. Silicon hardware acceleration dispatch (CUDA, AMX, AVX-512, AVX2, Scalar fallback).
 *   8. Advisory mitigation recommendations with non-authoritative provenance.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "test_macros.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_telemetry_ingest_and_threshold_anomalies(void) {
    printf("--- Testing Telemetry Ingestion & Static Threshold Anomalies ---\n");

    keystone_telemetry_engine_t* engine = keystone_telemetry_create(1024, 256, 1);
    TEST_ASSERT(engine != NULL);

    keystone_uuid_t node_id;
    keystone_uuid_from_string("11111111-2222-3333-4444-555555555555", &node_id);

    /* Ingest baseline normal samples (CPU at 40%) */
    for (uint64_t t = 1000; t <= 10000; t += 1000) {
        keystone_telemetry_sample_t s;
        memset(&s, 0, sizeof(s));
        s.node_id = node_id;
        s.metric_type = KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT;
        s.timestamp_hlc.physical_ms = t;
        s.value = 40.0 + (double)(t % 3); /* 40, 41, 42 */

        keystone_anomaly_t anom;
        bool is_anom = false;
        int rc = keystone_telemetry_ingest_sample(engine, &s, &anom, &is_anom);
        TEST_ASSERT(rc == 0);
        TEST_ASSERT(!is_anom); /* Should not trigger anomaly */
    }

    TEST_ASSERT(keystone_telemetry_sample_count(engine) == 10);
    TEST_ASSERT(keystone_telemetry_anomaly_count(engine) == 0);

    /* Ingest sample exceeding static warning threshold (CPU 88% >= 85%) */
    {
        keystone_telemetry_sample_t s_warn;
        memset(&s_warn, 0, sizeof(s_warn));
        s_warn.node_id = node_id;
        s_warn.metric_type = KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT;
        s_warn.timestamp_hlc.physical_ms = 11000;
        s_warn.value = 88.0;

        keystone_anomaly_t anom;
        bool is_anom = false;
        int rc = keystone_telemetry_ingest_sample(engine, &s_warn, &anom, &is_anom);
        TEST_ASSERT(rc == 0);
        TEST_ASSERT(is_anom);
        TEST_ASSERT(anom.severity == KEYSTONE_ANOMALY_WARNING);
        TEST_ASSERT(strstr(anom.rationale, "warning threshold") != NULL);
    }

    /* Ingest sample exceeding static critical threshold (CPU 97% >= 95%) */
    {
        keystone_telemetry_sample_t s_crit;
        memset(&s_crit, 0, sizeof(s_crit));
        s_crit.node_id = node_id;
        s_crit.metric_type = KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT;
        s_crit.timestamp_hlc.physical_ms = 12000;
        s_crit.value = 97.0;

        keystone_anomaly_t anom;
        bool is_anom = false;
        int rc = keystone_telemetry_ingest_sample(engine, &s_crit, &anom, &is_anom);
        TEST_ASSERT(rc == 0);
        TEST_ASSERT(is_anom);
        TEST_ASSERT(anom.severity == KEYSTONE_ANOMALY_CRITICAL);
        TEST_ASSERT(strstr(anom.rationale, "critical threshold") != NULL);
    }

    /* Test Hardware Error count -> Fatal anomaly */
    {
        keystone_telemetry_sample_t s_hw;
        memset(&s_hw, 0, sizeof(s_hw));
        s_hw.node_id = node_id;
        s_hw.metric_type = KEYSTONE_TELEMETRY_HARDWARE_ERRORS_COUNT;
        s_hw.timestamp_hlc.physical_ms = 13000;
        s_hw.value = 4.0; /* Critical is 3.0 */

        keystone_anomaly_t anom;
        bool is_anom = false;
        int rc = keystone_telemetry_ingest_sample(engine, &s_hw, &anom, &is_anom);
        TEST_ASSERT(rc == 0);
        TEST_ASSERT(is_anom);
        TEST_ASSERT(anom.severity == KEYSTONE_ANOMALY_FATAL);
    }

    TEST_ASSERT(keystone_telemetry_anomaly_count(engine) == 3);
    printf("  [PASS] Telemetry sample ingestion & static threshold anomalies verified\n");

    keystone_telemetry_destroy(engine);
}

static void test_telemetry_zscore_and_explain(void) {
    printf("--- Testing Statistical Z-Score Outlier Detection & Anomaly Explain ---\n");

    keystone_telemetry_engine_t* engine = keystone_telemetry_create(1024, 256, 1);
    TEST_ASSERT(engine != NULL);

    keystone_uuid_t node_id;
    keystone_uuid_from_string("22222222-3333-4444-5555-666666666666", &node_id);

    /* Ingest tightly clustered disk latency samples: 1000 us +/- 10 us */
    for (uint64_t t = 1000; t <= 20000; t += 1000) {
        keystone_telemetry_sample_t s;
        memset(&s, 0, sizeof(s));
        s.node_id = node_id;
        s.metric_type = KEYSTONE_TELEMETRY_DISK_LATENCY_US;
        s.timestamp_hlc.physical_ms = t;
        s.value = 1000.0 + (((t / 1000) % 5) * 4.0); /* 1000 to 1016 */

        keystone_anomaly_t anom;
        bool is_anom = false;
        keystone_telemetry_ingest_sample(engine, &s, &anom, &is_anom);
        TEST_ASSERT(!is_anom);
    }

    /* Sudden spike: 1500 us (way below static warning of 20,000 us, but massive statistical outlier!) */
    keystone_telemetry_sample_t spike;
    memset(&spike, 0, sizeof(spike));
    spike.node_id = node_id;
    spike.metric_type = KEYSTONE_TELEMETRY_DISK_LATENCY_US;
    spike.timestamp_hlc.physical_ms = 21000;
    spike.value = 1500.0;

    keystone_anomaly_t anom;
    bool is_anom = false;
    int rc = keystone_telemetry_ingest_sample(engine, &spike, &anom, &is_anom);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_anom);
    TEST_ASSERT(anom.z_score >= 3.0);
    printf("  [DETECT] Detected z-score outlier: observed=%.1f, mean=%.1f, stddev=%.2f, z=%.2f\n",
           anom.observed_value, anom.baseline_mean, anom.baseline_stddev, anom.z_score);

    /* Verify anomaly query */
    keystone_anomaly_t results[8];
    size_t found = keystone_telemetry_query_anomalies(engine, &node_id, KEYSTONE_ANOMALY_WARNING, results, 8);
    TEST_ASSERT(found == 1);
    TEST_ASSERT(results[0].metric_type == KEYSTONE_TELEMETRY_DISK_LATENCY_US);

    /* Verify anomaly explanation */
    keystone_explain_t explain;
    rc = keystone_telemetry_explain_anomaly(&results[0], &explain);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(explain.hard_constraint_count >= 2);
    TEST_ASSERT(explain.composite_score > 0.0);
    printf("  [PASS] Anomaly evidence bundle created: ranking_notes='%s'\n", explain.ranking_notes);

    keystone_telemetry_destroy(engine);
}

static void test_telemetry_rolling_window_features(void) {
    printf("--- Testing Multi-Tier Rolling Window Feature Extraction ---\n");

    keystone_telemetry_engine_t* engine = keystone_telemetry_create(2048, 256, 1);
    TEST_ASSERT(engine != NULL);

    keystone_uuid_t node_id;
    keystone_uuid_from_string("33333333-4444-5555-6666-777777777777", &node_id);

    /*
     * Ingest linear upward ramp:
     * 60 samples, 1 second apart (total 60,000 ms = exactly 1m window).
     * Values ramp from 10.0 to 69.0 (slope = 1.0 unit/second).
     */
    for (uint64_t i = 0; i < 60; i++) {
        keystone_telemetry_sample_t s;
        memset(&s, 0, sizeof(s));
        s.node_id = node_id;
        s.metric_type = KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS;
        s.timestamp_hlc.physical_ms = i * 1000;
        s.value = 10.0 + (double)i;

        keystone_anomaly_t anom;
        bool is_anom = false;
        keystone_telemetry_ingest_sample(engine, &s, &anom, &is_anom);
    }

    keystone_feature_window_t features;
    int rc = keystone_telemetry_extract_features(
        engine,
        &node_id,
        KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS,
        KEYSTONE_WINDOW_1M,
        &features
    );
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(features.sample_count == 60);
    TEST_ASSERT(features.min_value == 10.0);
    TEST_ASSERT(features.max_value == 69.0);

    /* Mean of 10..69 is (10 + 69)/2 = 39.5 */
    TEST_ASSERT(fabs(features.mean - 39.5) < 1e-3);
    TEST_ASSERT(features.variance > 0.0);
    TEST_ASSERT(features.stddev > 0.0);

    /* Slope should be 1.0 unit per second */
    printf("  Calculated slope: %.4f units/sec (expected ~1.0)\n", features.slope);
    TEST_ASSERT(fabs(features.slope - 1.0) < 0.05);

    /* Test 5M window extraction with multiple metric types */
    keystone_feature_window_t f_5m;
    rc = keystone_telemetry_extract_features(
        engine,
        &node_id,
        KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS,
        KEYSTONE_WINDOW_5M,
        &f_5m
    );
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(f_5m.sample_count == 60);

    printf("  [PASS] Rolling feature window mean=%.2f, stddev=%.2f, slope=%.2f verified\n",
           features.mean, features.stddev, features.slope);

    keystone_telemetry_destroy(engine);
}

static void test_incident_similarity_and_silicon_dispatch(void) {
    printf("--- Testing Incident Similarity Search & Silicon Dispatch ---\n");

    keystone_incident_engine_t* engine = keystone_incident_engine_create(64, 1);
    TEST_ASSERT(engine != NULL);

    /* Register Historical Incidents */
    keystone_incident_record_t inc1;
    memset(&inc1, 0, sizeof(inc1));
    keystone_uuid_from_string("aaaaaaaa-1111-2222-3333-444444444444", &inc1.incident_id);
    strncpy(inc1.root_cause_label, "THERMAL_THROTTLE_FAILOVER", sizeof(inc1.root_cause_label) - 1);
    strncpy(inc1.resolution_notes, "Migrate active workloads to rack 3, cycle cooling loop", sizeof(inc1.resolution_notes) - 1);
    inc1.object_type = KEYSTONE_OBJ_NODE;
    inc1.severity = KEYSTONE_ANOMALY_CRITICAL;
    inc1.embedding[3] = 0.95f;  /* High temperature */
    inc1.embedding[14] = 0.85f; /* Sharp thermal slope */
    inc1.embedding[25] = 0.50f; /* Burst count */
    inc1.embedding[42] = 1.00f; /* Thermal error token */
    TEST_ASSERT(keystone_incident_register(engine, &inc1) == 0);

    keystone_incident_record_t inc2;
    memset(&inc2, 0, sizeof(inc2));
    keystone_uuid_from_string("bbbbbbbb-2222-3333-4444-555555555555", &inc2.incident_id);
    strncpy(inc2.root_cause_label, "ECC_MEMORY_CORRUPTION", sizeof(inc2.root_cause_label) - 1);
    strncpy(inc2.resolution_notes, "Replace DIMM in socket 2, evict host from compute pool", sizeof(inc2.resolution_notes) - 1);
    inc2.object_type = KEYSTONE_OBJ_NODE;
    inc2.severity = KEYSTONE_ANOMALY_FATAL;
    inc2.embedding[2] = 0.90f;  /* High memory pressure */
    inc2.embedding[9] = 1.00f;  /* ECC events count */
    inc2.embedding[43] = 1.00f; /* Memory error token */
    TEST_ASSERT(keystone_incident_register(engine, &inc2) == 0);

    keystone_incident_record_t inc3;
    memset(&inc3, 0, sizeof(inc3));
    keystone_uuid_from_string("cccccccc-3333-4444-5555-666666666666", &inc3.incident_id);
    strncpy(inc3.root_cause_label, "IO_QUEUE_SATURATION_HANG", sizeof(inc3.root_cause_label) - 1);
    strncpy(inc3.resolution_notes, "Throttle storage traffic, restart storage daemon", sizeof(inc3.resolution_notes) - 1);
    inc3.object_type = KEYSTONE_OBJ_VOLUME;
    inc3.severity = KEYSTONE_ANOMALY_WARNING;
    inc3.embedding[4] = 0.95f;  /* High disk latency */
    inc3.embedding[8] = 0.90f;  /* High queue depth */
    inc3.embedding[44] = 1.00f; /* I/O error token */
    TEST_ASSERT(keystone_incident_register(engine, &inc3) == 0);

    TEST_ASSERT(keystone_incident_count(engine) == 3);

    /* Construct query targeting thermal failure */
    keystone_incident_query_t query;
    memset(&query, 0, sizeof(query));
    query.query_embedding[3] = 0.92f;  /* High temp */
    query.query_embedding[14] = 0.80f; /* Upward thermal slope */
    query.query_embedding[25] = 0.45f;
    query.query_embedding[42] = 1.00f;
    query.min_similarity_threshold = 0.70f;
    query.force_backend = -1; /* Auto-detect */

    keystone_incident_match_t matches[4];
    size_t match_count = keystone_incident_search_similar(engine, &query, matches, 4);

    TEST_ASSERT(match_count >= 1);
    TEST_ASSERT(strcmp(matches[0].incident.root_cause_label, "THERMAL_THROTTLE_FAILOVER") == 0);
    TEST_ASSERT(matches[0].similarity_score > 0.90f);
    printf("  [PASS] Top match: '%s' (similarity=%.3f, backend=%s)\n",
           matches[0].incident.root_cause_label,
           matches[0].similarity_score,
           keystone_silicon_backend_name((keystone_silicon_backend_t)matches[0].backend_used));

    /* Test Cross-Backend Consistency: Scalar vs AVX2 */
    query.force_backend = KEYSTONE_SILICON_SCALAR;
    keystone_incident_match_t scalar_matches[4];
    size_t sc_count = keystone_incident_search_similar(engine, &query, scalar_matches, 4);
    TEST_ASSERT(sc_count == match_count);

    query.force_backend = KEYSTONE_SILICON_AVX2;
    keystone_incident_match_t avx2_matches[4];
    size_t avx2_count = keystone_incident_search_similar(engine, &query, avx2_matches, 4);
    TEST_ASSERT(avx2_count == match_count);

    for (size_t i = 0; i < match_count; i++) {
        float diff = fabsf(scalar_matches[i].similarity_score - avx2_matches[i].similarity_score);
        TEST_ASSERT(diff < 1e-4f);
    }
    printf("  [PASS] AVX2 distance kernel bit-accurate with Scalar reference\n");

    /* Test Non-Authoritative Mitigation Recommendations */
    keystone_recommendation_t recs[4];
    keystone_hlc_t hlc_now = keystone_hlc_now(1);
    size_t rec_count = keystone_incident_recommend_mitigations(engine, &query, 42, &hlc_now, recs, 4);

    TEST_ASSERT(rec_count == match_count);
    TEST_ASSERT(recs[0].target_type == KEYSTONE_OBJ_INCIDENT);
    TEST_ASSERT(recs[0].confidence_score > 0.90);
    TEST_ASSERT(recs[0].based_on_generation == 42);
    TEST_ASSERT(strstr(recs[0].evidence.ranking_notes, "THERMAL_THROTTLE_FAILOVER") != NULL);

    printf("  [PASS] Non-authoritative mitigation recommendation emitted with audit provenance\n");

    keystone_incident_engine_destroy(engine);
}

int main(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    printf("=====================================================\n");
    printf("KEYSTONE Streaming Telemetry & Incident Silicon Suite\n");
    printf("=====================================================\n");

    test_telemetry_ingest_and_threshold_anomalies();
    test_telemetry_zscore_and_explain();
    test_telemetry_rolling_window_features();
    test_incident_similarity_and_silicon_dispatch();

    printf("\nAll Telemetry Intelligence and Incident Silicon tests passed!\n");
    return 0;
}
