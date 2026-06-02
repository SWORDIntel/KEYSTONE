/**
 * KEYSTONE - high-performance interpolation search for sorted int64_t data.
 *
 * The core library exposes interpolation/anchor search, tuned wrappers,
 * anchor-table management, statistics, configuration, and workload helpers.
 */

#ifndef KEYSTONE_H
#define KEYSTONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Anchor and search tuning limits. */
#define KEYSTONE_MAX_ANCHORS 1048576
#define KEYSTONE_CHUNK_SIZE 4
#define KEYSTONE_MIN_ANCHORS 2
#define KEYSTONE_ANCHOR_PRUNE_THRESHOLD 0.8f
#define KEYSTONE_MEMORY_BUDGET_MB 8

/* DSMIL workload types for optimization. */
enum keystone_workload_type {
    KEYSTONE_WORKLOAD_TELEMETRY = 0,
    KEYSTONE_WORKLOAD_IDS = 1,
    KEYSTONE_WORKLOAD_OFFSETS = 2,
    KEYSTONE_WORKLOAD_EVENTS = 3
};

typedef enum keystone_cpu_feature {
    KEYSTONE_CPU_AVX2 = (1 << 0),
    KEYSTONE_CPU_AVX512 = (1 << 1),
    KEYSTONE_CPU_AMX = (1 << 2),
    KEYSTONE_CPU_VNNI = (1 << 3),
    KEYSTONE_CPU_NEON = (1 << 4),
    KEYSTONE_CPU_SVE = (1 << 5),
    KEYSTONE_CPU_SVE2 = (1 << 6),
    KEYSTONE_CPU_I8MM = (1 << 7),
    KEYSTONE_CPU_GRAVITON4 = (1 << 8)
} keystone_cpu_feature_t;

typedef struct {
    int64_t v;
    size_t i;
    uint32_t use_count;
    uint64_t last_used;
} keystone_anchor_t;

typedef struct keystone_stats {
    uint64_t searches_total;
    uint64_t searches_successful;
    uint64_t anchors_learned;
    uint64_t anchors_pruned;
    uint64_t memory_reallocations;
    double avg_search_time_ns;
    double avg_interpolation_error;
    uint32_t cpu_features_detected;
} keystone_stats_t;

typedef struct keystone_anchor_table {
    keystone_anchor_t* anchors;
    size_t capacity;
    size_t size;
    size_t max_capacity;
    size_t searches_performed;
    int workload_type;
    keystone_stats_t stats;
    uint64_t creation_time;
} keystone_anchor_table_t;

typedef size_t keystone_result_t;
#define KEYSTONE_NOT_FOUND ((keystone_result_t)-1)

typedef struct keystone_batch_item {
    int64_t key;
    keystone_result_t result;
    size_t ordinal;
} keystone_batch_item_t;

typedef struct keystone_parallel_config {
    int num_threads;
    int use_thread_pool;
    size_t batch_chunk;
} keystone_parallel_config_t;

typedef enum keystone_backend {
    KEYSTONE_BACKEND_AUTO = 0,
    KEYSTONE_BACKEND_SCALAR,
    KEYSTONE_BACKEND_C_BATCH,
    KEYSTONE_BACKEND_C_OPENMP,
    KEYSTONE_BACKEND_C_AVX2,
    KEYSTONE_BACKEND_C_AVX512,
    KEYSTONE_BACKEND_C_AMX,
    KEYSTONE_BACKEND_FORTRAN
} keystone_backend_t;

typedef struct keystone_backend_decision {
    keystone_backend_t backend;
    uint32_t cpu_features;
    size_t array_size_bucket;
    size_t query_count_bucket;
    int thread_count;
    double estimated_ns_per_key;
    double p95_ns_per_key;
} keystone_backend_decision_t;

typedef struct keystone_performance_stats {
    uint64_t total_time_ns;
    uint64_t search_time_ns;
    uint64_t total_searches;
    uint64_t successful_searches;
    double avg_search_time_ns;
    double search_success_rate;
    double speedup_vs_binary;
    size_t peak_memory_usage;
    size_t avg_memory_usage;
    uint64_t anchors_learned;
    uint64_t anchors_pruned;
    uint32_t cpu_features_used;
    double vectorization_efficiency;
    uint64_t memory_allocation_failures;
    uint64_t archive_bytes_read;
    uint64_t archive_decompress_time_ns;
    uint64_t archive_parse_time_ns;
    uint64_t archive_members_searched;
} keystone_performance_stats_t;

typedef enum keystone_error {
    KEYSTONE_SUCCESS = 0,
    KEYSTONE_ERROR_INVALID_PARAM = -1,
    KEYSTONE_ERROR_MEMORY = -2,
    KEYSTONE_ERROR_NOT_FOUND = -3,
    KEYSTONE_ERROR_CONFIG = -7,
    KEYSTONE_ERROR_CPU_FEATURE = -8,
    KEYSTONE_ERROR_ARCHIVE_OPEN = -20,
    KEYSTONE_ERROR_ARCHIVE_READ = -21,
    KEYSTONE_ERROR_ARCHIVE_FORMAT = -22,
    KEYSTONE_ERROR_PARSE = -23
} keystone_error_t;

typedef struct keystone_config {
    size_t tol;
    int enable_anchor_learning;
    size_t max_anchors;
    int workload_type;
    int enable_simd;
    uint32_t force_cpu_features;
    int enable_profiling;
    int strict_mode;
} keystone_config_t;

keystone_anchor_table_t* keystone_anchor_table_create(void);
void keystone_anchor_table_destroy(keystone_anchor_table_t* table);
size_t keystone_anchor_table_size(const keystone_anchor_table_t* table);
void keystone_anchor_table_reset(keystone_anchor_table_t* table);
const keystone_stats_t* keystone_anchor_table_get_stats(const keystone_anchor_table_t* table);
int keystone_anchor_table_set_memory_limit(keystone_anchor_table_t* table, size_t max_anchors);
int keystone_anchor_table_optimize_for_workload(keystone_anchor_table_t* table, int workload_type);

uint32_t keystone_detect_cpu_features(void);

int keystone_get_performance_stats(keystone_performance_stats_t* stats);
void keystone_reset_performance_stats(void);
void keystone_set_performance_tracking(int enabled);
int keystone_is_performance_tracking_enabled(void);

const char* keystone_error_message(keystone_error_t error);

void keystone_config_init(keystone_config_t* config, int workload_type);
int keystone_config_validate(const keystone_config_t* config);
void keystone_config_optimize_for_workload(keystone_config_t* config, int workload_type);
void keystone_get_tuned_config(size_t array_size, keystone_config_t* config);

keystone_result_t keystone_search(
    const int64_t* arr,
    size_t n,
    int64_t key,
    keystone_anchor_table_t* table,
    size_t tol
);

keystone_result_t keystone_search_enhanced(
    const int64_t* arr,
    size_t n,
    int64_t key,
    keystone_anchor_table_t* table,
    const keystone_config_t* config
);

size_t keystone_search_batch(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items,
    keystone_anchor_table_t* table,
    size_t tol
);

size_t keystone_search_parallel(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items,
    keystone_anchor_table_t* table,
    size_t tol,
    const keystone_parallel_config_t* config
);

size_t keystone_search_batch_auto(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items,
    keystone_anchor_table_t* table,
    size_t tol,
    const keystone_parallel_config_t* config
);

int keystone_get_last_backend_decision(keystone_backend_decision_t* decision);

int keystone_fortran_backend_available(void);

size_t keystone_search_batch_fortran(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items
);

size_t keystone_search_batch_c_optimized(
    const int64_t* arr,
    size_t n,
    keystone_batch_item_t* items,
    size_t num_items,
    keystone_anchor_table_t* table,
    size_t tol
);

void keystone_get_stats(
    const keystone_anchor_table_t* table,
    size_t* searches_total,
    size_t* anchors_learned,
    size_t* memory_used_bytes
);

keystone_result_t keystone_search_telemetry(
    const int64_t* timestamps,
    size_t n,
    int64_t target_time,
    keystone_anchor_table_t* table
);

keystone_result_t keystone_search_ids(
    const int64_t* ids,
    size_t n,
    int64_t target_id,
    keystone_anchor_table_t* table
);

keystone_result_t keystone_search_offsets(
    const int64_t* offsets,
    size_t n,
    int64_t target_offset,
    keystone_anchor_table_t* table
);

keystone_result_t keystone_search_events(
    const int64_t* events,
    size_t n,
    int64_t target_time,
    keystone_anchor_table_t* table
);

bool keystone_init_for_dsmil(keystone_anchor_table_t* table, int workload_type);
int keystone_optimize_array_memory(int64_t* arr, size_t n);

#ifdef KEYSTONE_ENABLE_TAR_ZST
#include "keystone_tar_zst.h"
#endif

#define KEYSTONE_VERSION_MAJOR 1
#define KEYSTONE_VERSION_MINOR 1
#define KEYSTONE_VERSION_PATCH 0

const char* keystone_version(void);
const char* keystone_build_info(void);
bool enhanced_available(void);
const char* enhanced_build_info(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_H */
