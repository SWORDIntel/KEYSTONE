#ifndef KEYSTONE_BENCHMARK_WRITER_H
#define KEYSTONE_BENCHMARK_WRITER_H

#include "../include/keystone.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEYSTONE_BENCH_FORMAT_CSV = 0,
    KEYSTONE_BENCH_FORMAT_JSON = 1
} keystone_bench_format_t;

typedef struct {
    const char* file_path; /* NULL for stdout */
    keystone_bench_format_t format;
    int append;
} keystone_bench_writer_config_t;

typedef struct keystone_bench_writer keystone_bench_writer_t;

typedef struct {
    const char* profile_name;
    size_t array_size;
    size_t num_queries;
    int thread_count;
    
    double throughput_gib_s;
    double median_latency_ns;
    double p95_latency_ns;

    /* Dataset properties */
    int hit_rate_pct;
    int data_gap;
    int data_gap_jitter;
    int query_stride;

    /* Decision provenance */
    int decision_available;
    const char* backend_name;
    const char* decision_source;
    const char* query_shape;
    uint32_t cpu_features;
    size_t calibration_runs;
    size_t candidates_measured;
} keystone_bench_record_t;

keystone_bench_writer_t* keystone_bench_writer_create(const keystone_bench_writer_config_t* config);
void keystone_bench_writer_destroy(keystone_bench_writer_t* writer);

/* Writes a row/object to the output stream. It automatically polls getrusage() for RSS/page-faults. */
int keystone_bench_writer_record(keystone_bench_writer_t* writer, const keystone_bench_record_t* record);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_BENCHMARK_WRITER_H */
