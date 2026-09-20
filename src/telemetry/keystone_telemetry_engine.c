/*
 * KEYSTONE Streaming Telemetry & Deterministic Anomaly Engine Implementation
 *
 * Implements high-throughput telemetry ingestion, multi-tier rolling window
 * feature extraction (1m, 5m, 15m, 1h, 24h), EWMA tracking, slope/trend
 * calculation, and multi-stage anomaly detection with structured evidence bundles.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_telemetry.h"
#include "keystone_safe_alloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYSTONE_DEFAULT_SAMPLE_CAPACITY 65536u
#define KEYSTONE_DEFAULT_ANOMALY_CAPACITY 4096u
#define KEYSTONE_NODE_STATS_TABLE_SIZE 1024u

typedef struct {
    keystone_uuid_t node_id;
    uint32_t metric_type;
    uint64_t sample_count;
    double sum;
    double sum_sq;
    double ewma;
    double last_value;
    uint64_t last_timestamp_ms;
    int occupied;
} node_metric_state_t;

struct keystone_telemetry_engine {
    keystone_telemetry_sample_t* samples;
    size_t sample_capacity;
    size_t sample_count;
    size_t sample_head;

    keystone_anomaly_t* anomalies;
    size_t anomaly_capacity;
    size_t anomaly_count;
    size_t anomaly_head;

    keystone_metric_threshold_t thresholds[KEYSTONE_TELEMETRY_METRIC_COUNT];
    node_metric_state_t state_table[KEYSTONE_NODE_STATS_TABLE_SIZE];

    uint64_t index_generation;
    uint64_t total_samples_ingested;
    uint64_t total_anomalies_detected;
};

uint64_t keystone_window_to_ms(keystone_window_duration_t window) {
    switch (window) {
        case KEYSTONE_WINDOW_1M:   return 60000ULL;
        case KEYSTONE_WINDOW_5M:   return 300000ULL;
        case KEYSTONE_WINDOW_15M:  return 900000ULL;
        case KEYSTONE_WINDOW_1H:   return 3600000ULL;
        case KEYSTONE_WINDOW_24H:  return 86400000ULL;
        default:                   return 300000ULL;
    }
}

const char* keystone_telemetry_metric_name(uint32_t metric_type) {
    switch (metric_type) {
        case KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT:  return "CPU_UTILIZATION_PCT";
        case KEYSTONE_TELEMETRY_NUMA_LOAD_PCT:        return "NUMA_LOAD_PCT";
        case KEYSTONE_TELEMETRY_MEMORY_PRESSURE_PCT:  return "MEMORY_PRESSURE_PCT";
        case KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS:  return "TEMPERATURE_CELSIUS";
        case KEYSTONE_TELEMETRY_DISK_LATENCY_US:      return "DISK_LATENCY_US";
        case KEYSTONE_TELEMETRY_NET_LATENCY_US:       return "NET_LATENCY_US";
        case KEYSTONE_TELEMETRY_NET_PACKET_LOSS_PCT:  return "NET_PACKET_LOSS_PCT";
        case KEYSTONE_TELEMETRY_VM_STEAL_TIME_MS:     return "VM_STEAL_TIME_MS";
        case KEYSTONE_TELEMETRY_IO_QUEUE_DEPTH:       return "IO_QUEUE_DEPTH";
        case KEYSTONE_TELEMETRY_ECC_EVENTS_COUNT:     return "ECC_EVENTS_COUNT";
        case KEYSTONE_TELEMETRY_HARDWARE_ERRORS_COUNT:return "HARDWARE_ERRORS_COUNT";
        default:                                      return "CUSTOM_METRIC";
    }
}

const char* keystone_anomaly_severity_name(uint32_t severity) {
    switch (severity) {
        case KEYSTONE_ANOMALY_INFO:     return "INFO";
        case KEYSTONE_ANOMALY_WARNING:  return "WARNING";
        case KEYSTONE_ANOMALY_CRITICAL: return "CRITICAL";
        case KEYSTONE_ANOMALY_FATAL:    return "FATAL";
        default:                        return "UNKNOWN";
    }
}

keystone_metric_threshold_t keystone_metric_threshold_default(uint32_t metric_type) {
    keystone_metric_threshold_t cfg;
    cfg.z_score_threshold = 3.0;
    cfg.ewma_alpha = 0.20;
    cfg.min_samples_for_zscore = 5;

    switch (metric_type) {
        case KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT:
            cfg.static_warning_threshold = 85.0;
            cfg.static_critical_threshold = 95.0;
            break;
        case KEYSTONE_TELEMETRY_NUMA_LOAD_PCT:
            cfg.static_warning_threshold = 85.0;
            cfg.static_critical_threshold = 95.0;
            break;
        case KEYSTONE_TELEMETRY_MEMORY_PRESSURE_PCT:
            cfg.static_warning_threshold = 85.0;
            cfg.static_critical_threshold = 95.0;
            break;
        case KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS:
            cfg.static_warning_threshold = 80.0;
            cfg.static_critical_threshold = 90.0;
            break;
        case KEYSTONE_TELEMETRY_DISK_LATENCY_US:
            cfg.static_warning_threshold = 20000.0;  /* 20 ms */
            cfg.static_critical_threshold = 100000.0;/* 100 ms */
            break;
        case KEYSTONE_TELEMETRY_NET_LATENCY_US:
            cfg.static_warning_threshold = 10000.0;  /* 10 ms */
            cfg.static_critical_threshold = 50000.0; /* 50 ms */
            break;
        case KEYSTONE_TELEMETRY_NET_PACKET_LOSS_PCT:
            cfg.static_warning_threshold = 5.0;
            cfg.static_critical_threshold = 15.0;
            break;
        case KEYSTONE_TELEMETRY_VM_STEAL_TIME_MS:
            cfg.static_warning_threshold = 100.0;
            cfg.static_critical_threshold = 500.0;
            break;
        case KEYSTONE_TELEMETRY_IO_QUEUE_DEPTH:
            cfg.static_warning_threshold = 64.0;
            cfg.static_critical_threshold = 256.0;
            break;
        case KEYSTONE_TELEMETRY_ECC_EVENTS_COUNT:
            cfg.static_warning_threshold = 1.0;
            cfg.static_critical_threshold = 5.0;
            cfg.z_score_threshold = 2.5;
            break;
        case KEYSTONE_TELEMETRY_HARDWARE_ERRORS_COUNT:
            cfg.static_warning_threshold = 1.0;
            cfg.static_critical_threshold = 3.0;
            cfg.z_score_threshold = 2.0;
            break;
        default:
            cfg.static_warning_threshold = 0.0;
            cfg.static_critical_threshold = 0.0;
            break;
    }
    return cfg;
}

keystone_telemetry_engine_t* keystone_telemetry_create(
    size_t sample_capacity,
    size_t anomaly_capacity,
    uint64_t generation
) {
    if (sample_capacity == 0) sample_capacity = KEYSTONE_DEFAULT_SAMPLE_CAPACITY;
    if (anomaly_capacity == 0) anomaly_capacity = KEYSTONE_DEFAULT_ANOMALY_CAPACITY;

    size_t samples_bytes, anomalies_bytes;
    if (!checked_mul_size(sample_capacity, sizeof(keystone_telemetry_sample_t), &samples_bytes) ||
        !checked_mul_size(anomaly_capacity, sizeof(keystone_anomaly_t), &anomalies_bytes)) {
        return NULL;
    }

    keystone_telemetry_engine_t* engine = (keystone_telemetry_engine_t*)calloc(1, sizeof(keystone_telemetry_engine_t));
    if (!engine) return NULL;

    engine->samples = (keystone_telemetry_sample_t*)malloc(samples_bytes);
    if (!engine->samples) {
        free(engine);
        return NULL;
    }

    engine->anomalies = (keystone_anomaly_t*)malloc(anomalies_bytes);
    if (!engine->anomalies) {
        free(engine->samples);
        free(engine);
        return NULL;
    }

    engine->sample_capacity = sample_capacity;
    engine->sample_count = 0;
    engine->sample_head = 0;

    engine->anomaly_capacity = anomaly_capacity;
    engine->anomaly_count = 0;
    engine->anomaly_head = 0;

    engine->index_generation = generation;
    engine->total_samples_ingested = 0;
    engine->total_anomalies_detected = 0;

    for (uint32_t m = 0; m < KEYSTONE_TELEMETRY_METRIC_COUNT; m++) {
        engine->thresholds[m] = keystone_metric_threshold_default(m);
    }

    memset(engine->state_table, 0, sizeof(engine->state_table));
    return engine;
}

void keystone_telemetry_destroy(keystone_telemetry_engine_t* engine) {
    if (!engine) return;
    if (engine->samples) {
        free(engine->samples);
        engine->samples = NULL;
    }
    if (engine->anomalies) {
        free(engine->anomalies);
        engine->anomalies = NULL;
    }
    free(engine);
}

int keystone_telemetry_configure_metric(
    keystone_telemetry_engine_t* engine,
    uint32_t metric_type,
    const keystone_metric_threshold_t* config
) {
    if (!engine || !config || metric_type >= KEYSTONE_TELEMETRY_METRIC_COUNT) return -1;
    engine->thresholds[metric_type] = *config;
    return 0;
}

static node_metric_state_t* find_or_create_state(
    keystone_telemetry_engine_t* engine,
    const keystone_uuid_t* node_id,
    uint32_t metric_type
) {
    uint64_t hash = keystone_uuid_hash64(node_id) ^ ((uint64_t)metric_type * 0x9E3779B97F4A7C15ULL);
    size_t idx = (size_t)(hash % KEYSTONE_NODE_STATS_TABLE_SIZE);

    for (size_t i = 0; i < KEYSTONE_NODE_STATS_TABLE_SIZE; i++) {
        size_t probe = (idx + i) % KEYSTONE_NODE_STATS_TABLE_SIZE;
        if (!engine->state_table[probe].occupied) {
            engine->state_table[probe].occupied = 1;
            engine->state_table[probe].node_id = *node_id;
            engine->state_table[probe].metric_type = metric_type;
            engine->state_table[probe].sample_count = 0;
            engine->state_table[probe].sum = 0.0;
            engine->state_table[probe].sum_sq = 0.0;
            engine->state_table[probe].ewma = 0.0;
            engine->state_table[probe].last_value = 0.0;
            engine->state_table[probe].last_timestamp_ms = 0;
            return &engine->state_table[probe];
        }
        if (keystone_uuid_equal(&engine->state_table[probe].node_id, node_id) &&
            engine->state_table[probe].metric_type == metric_type) {
            return &engine->state_table[probe];
        }
    }
    return &engine->state_table[idx]; /* fallback on full table */
}

static keystone_uuid_t generate_anomaly_uuid(const keystone_telemetry_sample_t* s, uint64_t seq) {
    keystone_uuid_t u;
    uint64_t h1 = keystone_uuid_hash64(&s->node_id) ^ s->timestamp_hlc.physical_ms;
    uint64_t h2 = ((uint64_t)s->metric_type << 32) ^ seq;
    memcpy(&u.bytes[0], &h1, 8);
    memcpy(&u.bytes[8], &h2, 8);
    return u;
}

int keystone_telemetry_ingest_sample(
    keystone_telemetry_engine_t* engine,
    const keystone_telemetry_sample_t* sample,
    keystone_anomaly_t* out_anomaly,
    bool* out_is_anomaly
) {
    if (!engine || !sample || !out_is_anomaly) return -1;
    *out_is_anomaly = false;

    /* 1. Append sample to circular buffer */
    engine->samples[engine->sample_head] = *sample;
    engine->sample_head = (engine->sample_head + 1) % engine->sample_capacity;
    if (engine->sample_count < engine->sample_capacity) {
        engine->sample_count++;
    }
    engine->total_samples_ingested++;

    /* 2. Retrieve / update metric state */
    node_metric_state_t* st = find_or_create_state(engine, &sample->node_id, sample->metric_type);
    const keystone_metric_threshold_t* thresh = (sample->metric_type < KEYSTONE_TELEMETRY_METRIC_COUNT)
        ? &engine->thresholds[sample->metric_type]
        : &engine->thresholds[0];

    double val = sample->value;
    double mean = 0.0;
    double stddev = 0.0;
    double z = 0.0;
    bool is_anomaly = false;
    uint32_t severity = KEYSTONE_ANOMALY_INFO;
    char rationale[128] = {0};

    if (st->sample_count >= thresh->min_samples_for_zscore && st->sample_count > 1) {
        mean = st->sum / (double)st->sample_count;
        double variance = (st->sum_sq - (st->sum * st->sum) / (double)st->sample_count) / (double)(st->sample_count - 1);
        if (variance > 1e-6) {
            stddev = sqrt(variance);
            z = (val - mean) / stddev;
        } else if (fabs(val - mean) > 1e-6) {
            stddev = 0.001;
            z = (val - mean) / stddev;
        }
    }

    /* Check Static Critical Threshold */
    if (thresh->static_critical_threshold > 0.0 && val >= thresh->static_critical_threshold) {
        is_anomaly = true;
        severity = (sample->metric_type == KEYSTONE_TELEMETRY_HARDWARE_ERRORS_COUNT) ? KEYSTONE_ANOMALY_FATAL : KEYSTONE_ANOMALY_CRITICAL;
        snprintf(rationale, sizeof(rationale),
                 "Static critical threshold exceeded: observed %.2f >= %.2f (%s)",
                 val, thresh->static_critical_threshold, keystone_telemetry_metric_name(sample->metric_type));
    }
    /* Check Static Warning Threshold */
    else if (thresh->static_warning_threshold > 0.0 && val >= thresh->static_warning_threshold) {
        is_anomaly = true;
        severity = KEYSTONE_ANOMALY_WARNING;
        snprintf(rationale, sizeof(rationale),
                 "Static warning threshold exceeded: observed %.2f >= %.2f (%s)",
                 val, thresh->static_warning_threshold, keystone_telemetry_metric_name(sample->metric_type));
    }
    /* Check Dynamic z-score Outlier */
    else if (st->sample_count >= thresh->min_samples_for_zscore && fabs(z) >= thresh->z_score_threshold) {
        is_anomaly = true;
        severity = (fabs(z) >= (thresh->z_score_threshold * 1.5)) ? KEYSTONE_ANOMALY_CRITICAL : KEYSTONE_ANOMALY_WARNING;
        snprintf(rationale, sizeof(rationale),
                 "Statistical z-score outlier: z=%.2f (|z| >= %.2f, baseline mean=%.2f, stddev=%.2f)",
                 z, thresh->z_score_threshold, mean, stddev);
    }

    /* Update online rolling state */
    st->sample_count++;
    st->sum += val;
    st->sum_sq += (val * val);
    if (st->sample_count == 1) {
        st->ewma = val;
    } else {
        st->ewma = (thresh->ewma_alpha * val) + ((1.0 - thresh->ewma_alpha) * st->ewma);
    }
    st->last_value = val;
    st->last_timestamp_ms = sample->timestamp_hlc.physical_ms;

    /* 3. Record anomaly if detected */
    if (is_anomaly) {
        keystone_anomaly_t anom;
        memset(&anom, 0, sizeof(anom));
        anom.anomaly_id = generate_anomaly_uuid(sample, engine->total_anomalies_detected + 1);
        anom.source_node_id = sample->node_id;
        anom.source_resource_id = sample->resource_id;
        anom.metric_type = sample->metric_type;
        anom.severity = severity;
        anom.observed_value = val;
        anom.baseline_mean = mean;
        anom.baseline_stddev = stddev;
        anom.z_score = z;
        anom.window_ms = 300000ULL; /* 5m default window context */
        anom.timestamp_hlc = sample->timestamp_hlc;
        anom.index_generation = engine->index_generation;
        snprintf(anom.rationale, sizeof(anom.rationale), "%s", rationale);

        engine->anomalies[engine->anomaly_head] = anom;
        engine->anomaly_head = (engine->anomaly_head + 1) % engine->anomaly_capacity;
        if (engine->anomaly_count < engine->anomaly_capacity) {
            engine->anomaly_count++;
        }
        engine->total_anomalies_detected++;

        *out_is_anomaly = true;
        if (out_anomaly) {
            *out_anomaly = anom;
        }
    }

    return 0;
}

int keystone_telemetry_extract_features(
    const keystone_telemetry_engine_t* engine,
    const keystone_uuid_t* node_id,
    uint32_t metric_type,
    keystone_window_duration_t window,
    keystone_feature_window_t* out_features
) {
    if (!engine || !out_features) return -1;
    memset(out_features, 0, sizeof(keystone_feature_window_t));

    uint64_t window_ms = keystone_window_to_ms(window);
    out_features->feature_schema_version = KEYSTONE_TELEMETRY_FEATURE_SCHEMA_VERSION;
    out_features->metric_type = metric_type;
    out_features->window_ms = window_ms;

    if (engine->sample_count == 0) return 0;

    /* Find newest timestamp for this node and metric */
    uint64_t newest_ms = 0;
    bool any_node = (!node_id || keystone_uuid_is_nil(node_id));

    for (size_t i = 0; i < engine->sample_count; i++) {
        const keystone_telemetry_sample_t* s = &engine->samples[i];
        if (s->metric_type == metric_type && (any_node || keystone_uuid_equal(&s->node_id, node_id))) {
            if (s->timestamp_hlc.physical_ms > newest_ms) {
                newest_ms = s->timestamp_hlc.physical_ms;
            }
        }
    }

    if (newest_ms == 0) return 0;
    uint64_t window_start_ms = (newest_ms >= window_ms) ? (newest_ms - window_ms) : 0;

    /* First pass: count, sum, sum_sq, min, max, start/end HLC */
    uint64_t count = 0;
    double sum = 0.0;
    double sum_sq = 0.0;
    double min_val = 1e30;
    double max_val = -1e30;
    keystone_hlc_t start_hlc = {0, 0, 0};
    keystone_hlc_t end_hlc = {0, 0, 0};

    for (size_t i = 0; i < engine->sample_count; i++) {
        const keystone_telemetry_sample_t* s = &engine->samples[i];
        if (s->metric_type == metric_type && (any_node || keystone_uuid_equal(&s->node_id, node_id))) {
            if (s->timestamp_hlc.physical_ms >= window_start_ms && s->timestamp_hlc.physical_ms <= newest_ms) {
                if (count == 0 || keystone_hlc_compare(&s->timestamp_hlc, &start_hlc) < 0) {
                    start_hlc = s->timestamp_hlc;
                }
                if (count == 0 || keystone_hlc_compare(&s->timestamp_hlc, &end_hlc) > 0) {
                    end_hlc = s->timestamp_hlc;
                }
                double v = s->value;
                sum += v;
                sum_sq += (v * v);
                if (v < min_val) min_val = v;
                if (v > max_val) max_val = v;
                count++;
            }
        }
    }

    if (count == 0) return 0;

    double mean = sum / (double)count;
    double variance = (count > 1) ? (sum_sq - (sum * sum) / (double)count) / (double)(count - 1) : 0.0;
    if (variance < 0.0) variance = 0.0;
    double stddev = sqrt(variance);

    /* Second pass: EWMA, slope (linear regression), and burst counts */
    double ewma = 0.0;
    double alpha = 0.20;
    uint32_t burst_count = 0;
    double burst_threshold = mean + 2.0 * stddev;

    double t0_sec = (double)start_hlc.physical_ms / 1000.0;
    double sum_t = 0.0;
    double sum_v = 0.0;
    double sum_tt = 0.0;
    double sum_tv = 0.0;

    size_t pass_count = 0;
    for (size_t i = 0; i < engine->sample_count; i++) {
        const keystone_telemetry_sample_t* s = &engine->samples[i];
        if (s->metric_type == metric_type && (any_node || keystone_uuid_equal(&s->node_id, node_id))) {
            if (s->timestamp_hlc.physical_ms >= window_start_ms && s->timestamp_hlc.physical_ms <= newest_ms) {
                double v = s->value;
                if (pass_count == 0) {
                    ewma = v;
                } else {
                    ewma = alpha * v + (1.0 - alpha) * ewma;
                }
                if (v > burst_threshold) {
                    burst_count++;
                }

                double t_sec = ((double)s->timestamp_hlc.physical_ms / 1000.0) - t0_sec;
                sum_t += t_sec;
                sum_v += v;
                sum_tt += (t_sec * t_sec);
                sum_tv += (t_sec * v);
                pass_count++;
            }
        }
    }

    double slope = 0.0;
    if (count > 1) {
        double denom = ((double)count * sum_tt) - (sum_t * sum_t);
        if (fabs(denom) > 1e-9) {
            slope = (((double)count * sum_tv) - (sum_t * sum_v)) / denom;
        }
    }

    double window_span_sec = (double)(newest_ms - window_start_ms) / 1000.0;
    if (window_span_sec <= 0.0) window_span_sec = 1.0;
    double error_rate = sum / window_span_sec;

    out_features->sample_count = count;
    out_features->mean = mean;
    out_features->variance = variance;
    out_features->stddev = stddev;
    out_features->min_value = min_val;
    out_features->max_value = max_val;
    out_features->ewma = ewma;
    out_features->slope = slope;
    out_features->burst_count = burst_count;
    out_features->error_rate = error_rate;
    out_features->start_hlc = start_hlc;
    out_features->end_hlc = end_hlc;

    return 0;
}

size_t keystone_telemetry_query_anomalies(
    const keystone_telemetry_engine_t* engine,
    const keystone_uuid_t* filter_node_id,
    uint32_t min_severity,
    keystone_anomaly_t* out_anomalies,
    size_t max_anomalies
) {
    if (!engine || !out_anomalies || max_anomalies == 0) return 0;

    bool any_node = (!filter_node_id || keystone_uuid_is_nil(filter_node_id));
    size_t matched = 0;

    /* Scan reverse-chronologically from newest anomaly */
    for (size_t i = 0; i < engine->anomaly_count && matched < max_anomalies; i++) {
        size_t idx = (engine->anomaly_head + engine->anomaly_capacity - 1 - i) % engine->anomaly_capacity;
        const keystone_anomaly_t* a = &engine->anomalies[idx];

        if (a->severity >= min_severity && (any_node || keystone_uuid_equal(&a->source_node_id, filter_node_id))) {
            out_anomalies[matched++] = *a;
        }
    }
    return matched;
}

int keystone_telemetry_explain_anomaly(
    const keystone_anomaly_t* anomaly,
    keystone_explain_t* out_explain
) {
    if (!anomaly || !out_explain) return -1;

    keystone_explain_init(out_explain, &anomaly->anomaly_id, KEYSTONE_OBJ_INCIDENT);

    char z_rat[128];
    snprintf(z_rat, sizeof(z_rat), "z-score %.2f (mean=%.2f, stddev=%.2f)",
             anomaly->z_score, anomaly->baseline_mean, anomaly->baseline_stddev);
    keystone_explain_add_constraint(out_explain, "STATISTICAL_ZSCORE_DEVIATION", 0, z_rat);

    char val_rat[128];
    snprintf(val_rat, sizeof(val_rat), "Observed value %.2f vs baseline %.2f",
             anomaly->observed_value, anomaly->baseline_mean);
    keystone_explain_add_constraint(out_explain, "THRESHOLD_BOUNDS_CHECK", 0, val_rat);

    char sev_rat[128];
    snprintf(sev_rat, sizeof(sev_rat), "Assigned severity: %s",
             keystone_anomaly_severity_name(anomaly->severity));
    keystone_explain_add_constraint(out_explain, "SEVERITY_CLASSIFICATION", 1, sev_rat);

    double score = fabs(anomaly->z_score) / 10.0;
    if (score > 1.0) score = 1.0;
    keystone_explain_set_ranking(out_explain, score, anomaly->rationale);

    return 0;
}

size_t keystone_telemetry_sample_count(const keystone_telemetry_engine_t* engine) {
    return engine ? engine->sample_count : 0;
}

size_t keystone_telemetry_anomaly_count(const keystone_telemetry_engine_t* engine) {
    return engine ? engine->anomaly_count : 0;
}
