/*
 * KEYSTONE Streaming Telemetry & Deterministic Anomaly Engine
 *
 * High-throughput telemetry ingestion, multi-tier rolling feature extraction
 * (1m, 5m, 15m, 1h, 24h), EWMA tracking, slope/trend estimation, and
 * deterministic z-score and static threshold anomaly detection.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#ifndef KEYSTONE_TELEMETRY_H
#define KEYSTONE_TELEMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYSTONE_TELEMETRY_FEATURE_SCHEMA_VERSION 1u

/* --- Standard Infrastructure Telemetry Metrics --- */
typedef enum {
    KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT  = 0,
    KEYSTONE_TELEMETRY_NUMA_LOAD_PCT        = 1,
    KEYSTONE_TELEMETRY_MEMORY_PRESSURE_PCT   = 2,
    KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS   = 3,
    KEYSTONE_TELEMETRY_DISK_LATENCY_US       = 4,
    KEYSTONE_TELEMETRY_NET_LATENCY_US        = 5,
    KEYSTONE_TELEMETRY_NET_PACKET_LOSS_PCT   = 6,
    KEYSTONE_TELEMETRY_VM_STEAL_TIME_MS      = 7,
    KEYSTONE_TELEMETRY_IO_QUEUE_DEPTH        = 8,
    KEYSTONE_TELEMETRY_ECC_EVENTS_COUNT      = 9,
    KEYSTONE_TELEMETRY_HARDWARE_ERRORS_COUNT = 10,
    KEYSTONE_TELEMETRY_METRIC_COUNT          = 11
} keystone_telemetry_metric_t;

/* --- Standard Rolling Window Durations --- */
typedef enum {
    KEYSTONE_WINDOW_1M   = 0, /*    60,000 ms */
    KEYSTONE_WINDOW_5M   = 1, /*   300,000 ms */
    KEYSTONE_WINDOW_15M  = 2, /*   900,000 ms */
    KEYSTONE_WINDOW_1H   = 3, /* 3,600,000 ms */
    KEYSTONE_WINDOW_24H  = 4, /* 86,400,000 ms */
    KEYSTONE_WINDOW_COUNT = 5
} keystone_window_duration_t;

/* --- Anomaly Severity Levels --- */
typedef enum {
    KEYSTONE_ANOMALY_INFO     = 0,
    KEYSTONE_ANOMALY_WARNING  = 1,
    KEYSTONE_ANOMALY_CRITICAL = 2,
    KEYSTONE_ANOMALY_FATAL    = 3
} keystone_anomaly_severity_t;

/* --- Metric Threshold & Detection Configuration --- */
typedef struct {
    double z_score_threshold;         /* Default 3.0 (|z| >= 3.0 is anomalous) */
    double static_warning_threshold;  /* Static threshold for WARNING (<= 0.0 disables) */
    double static_critical_threshold; /* Static threshold for CRITICAL (<= 0.0 disables) */
    double ewma_alpha;                /* Smoothing factor for EWMA (0.0 to 1.0, e.g. 0.2) */
    size_t min_samples_for_zscore;    /* Minimum samples in window before z-score activates (e.g. 5) */
} keystone_metric_threshold_t;

/* --- Raw Ingestion Sample --- */
typedef struct {
    keystone_uuid_t node_id;
    keystone_uuid_t resource_id;      /* VM/Disk/Interface UUID or nil */
    uint32_t metric_type;             /* keystone_telemetry_metric_t */
    keystone_hlc_t timestamp_hlc;
    double value;
    uint32_t flags;
} keystone_telemetry_sample_t;

/* --- Multi-tier Window Feature Summary --- */
typedef struct {
    uint32_t feature_schema_version;  /* KEYSTONE_TELEMETRY_FEATURE_SCHEMA_VERSION */
    uint32_t metric_type;             /* keystone_telemetry_metric_t */
    uint64_t window_ms;               /* Duration of window in ms */
    uint64_t sample_count;            /* Number of samples analyzed */
    double mean;                      /* Arithmetic mean of samples */
    double variance;                  /* Sample variance */
    double stddev;                    /* Standard deviation */
    double min_value;                 /* Minimum observed value */
    double max_value;                 /* Maximum observed value */
    double ewma;                      /* Exponentially Weighted Moving Average */
    double slope;                     /* Linear regression trend (units per second) */
    uint32_t burst_count;             /* Count of values exceeding (mean + 2 * stddev) */
    double error_rate;                /* Counter delta / seconds (for error metrics) */
    keystone_hlc_t start_hlc;
    keystone_hlc_t end_hlc;
} keystone_feature_window_t;

/* --- Structured Anomaly Record --- */
typedef struct {
    keystone_uuid_t anomaly_id;
    keystone_uuid_t source_node_id;
    keystone_uuid_t source_resource_id;
    uint32_t metric_type;
    uint32_t severity;                /* keystone_anomaly_severity_t */
    double observed_value;
    double baseline_mean;
    double baseline_stddev;
    double z_score;
    uint64_t window_ms;
    keystone_hlc_t timestamp_hlc;
    uint64_t index_generation;
    char rationale[128];
} keystone_anomaly_t;

/* Opaque Telemetry Engine */
typedef struct keystone_telemetry_engine keystone_telemetry_engine_t;

/* --- Lifecycle Management --- */
keystone_telemetry_engine_t* keystone_telemetry_create(
    size_t sample_capacity,
    size_t anomaly_capacity,
    uint64_t generation
);

void keystone_telemetry_destroy(keystone_telemetry_engine_t* engine);

/* --- Threshold & Detection Configuration --- */
int keystone_telemetry_configure_metric(
    keystone_telemetry_engine_t* engine,
    uint32_t metric_type,
    const keystone_metric_threshold_t* config
);

/* Default configuration helper */
keystone_metric_threshold_t keystone_metric_threshold_default(uint32_t metric_type);

/* --- Ingestion & Anomaly Evaluation --- */
int keystone_telemetry_ingest_sample(
    keystone_telemetry_engine_t* engine,
    const keystone_telemetry_sample_t* sample,
    keystone_anomaly_t* out_anomaly,
    bool* out_is_anomaly
);

/* --- Feature Extraction --- */
int keystone_telemetry_extract_features(
    const keystone_telemetry_engine_t* engine,
    const keystone_uuid_t* node_id,
    uint32_t metric_type,
    keystone_window_duration_t window,
    keystone_feature_window_t* out_features
);

/* --- Anomaly Queries & Evidence --- */
size_t keystone_telemetry_query_anomalies(
    const keystone_telemetry_engine_t* engine,
    const keystone_uuid_t* filter_node_id,
    uint32_t min_severity,
    keystone_anomaly_t* out_anomalies,
    size_t max_anomalies
);

int keystone_telemetry_explain_anomaly(
    const keystone_anomaly_t* anomaly,
    keystone_explain_t* out_explain
);

/* --- Introspection & Utility --- */
size_t keystone_telemetry_sample_count(const keystone_telemetry_engine_t* engine);
size_t keystone_telemetry_anomaly_count(const keystone_telemetry_engine_t* engine);
const char* keystone_telemetry_metric_name(uint32_t metric_type);
const char* keystone_anomaly_severity_name(uint32_t severity);
uint64_t keystone_window_to_ms(keystone_window_duration_t window);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_TELEMETRY_H */
