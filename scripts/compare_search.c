#include "include/not_stisla.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

typedef struct { int64_t key; } query_t;

#ifndef NOT_STISLA_BENCH_AUTO
#define NOT_STISLA_BENCH_AUTO 0
#endif

#if NOT_STISLA_BENCH_AUTO
static const char* backend_name(not_stisla_backend_t backend) {
    switch (backend) {
        case NOT_STISLA_BACKEND_AUTO: return "auto";
        case NOT_STISLA_BACKEND_SCALAR: return "scalar";
        case NOT_STISLA_BACKEND_C_BATCH: return "c_batch";
        case NOT_STISLA_BACKEND_C_OPENMP: return "c_openmp";
        case NOT_STISLA_BACKEND_C_AVX2: return "c_avx2";
        case NOT_STISLA_BACKEND_C_AVX512: return "c_avx512";
        case NOT_STISLA_BACKEND_C_AMX: return "c_amx";
        case NOT_STISLA_BACKEND_FORTRAN: return "fortran";
        default: return "unknown";
    }
}
#endif

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static size_t env_size_or_default(const char* name, size_t fallback) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0) {
        return fallback;
    }

    return (size_t)parsed;
}

static size_t env_size_allow_zero_or_default(const char* name, size_t fallback) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }

    return (size_t)parsed;
}

static size_t env_percent_or_default(const char* name, size_t fallback) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (!end || *end != '\0' || parsed > 100) {
        return fallback;
    }

    return (size_t)parsed;
}

static const char* env_str_or_default(const char* name, const char* fallback) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    return value;
}

static size_t gcd_size(size_t a, size_t b) {
    while (b != 0) {
        size_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

static size_t data_gap_for_index(size_t i, size_t base_gap, size_t gap_jitter) {
    if (gap_jitter == 0) {
        return base_gap;
    }

    return base_gap + ((i * 1103515245ULL + 12345ULL) % (gap_jitter + 1));
}

static not_stisla_result_t binary_search(const int64_t* arr, size_t n, int64_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (arr[mid] == key) return mid;
        if (arr[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return NOT_STISLA_NOT_FOUND;
}

static void fill_batch(not_stisla_batch_item_t* batch, const query_t* queries, size_t num_queries) {
    for (size_t i = 0; i < num_queries; ++i) {
        batch[i].key = queries[i].key;
        batch[i].result = NOT_STISLA_NOT_FOUND;
        batch[i].ordinal = i;
    }
}

static double bytes_to_gib(size_t bytes) {
    return (double)bytes / 1024.0 / 1024.0 / 1024.0;
}

static double effective_data_gib_s(double ns_per_key, size_t num_queries, size_t data_bytes) {
    const double total_seconds = ns_per_key * (double)num_queries / 1000000000.0;
    if (total_seconds <= 0.0) {
        return 0.0;
    }
    return bytes_to_gib(data_bytes) / total_seconds;
}

static int run_warmup_pass(const int64_t* data,
                           size_t n,
                           const query_t* queries,
                           size_t num_queries,
                           not_stisla_batch_item_t* batch,
                           size_t expected_found,
                           not_stisla_anchor_table_t* table,
                           const not_stisla_parallel_config_t* config,
                           int fortran_available) {
    size_t found = 0;

    for (size_t i = 0; i < num_queries; ++i) {
        if (binary_search(data, n, queries[i].key) != NOT_STISLA_NOT_FOUND) {
            found++;
        }
    }
    if (found != expected_found) {
        fprintf(stderr, "warmup binary search found %zu queries; expected %zu\n", found, expected_found);
        return -1;
    }

    found = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        if (not_stisla_search(data, n, queries[i].key, table, 8) != NOT_STISLA_NOT_FOUND) {
            found++;
        }
    }
    if (found != expected_found) {
        fprintf(stderr, "warmup NOT_STISLA found %zu queries; expected %zu\n", found, expected_found);
        return -1;
    }

    not_stisla_config_t enhanced_config;
    not_stisla_config_init(&enhanced_config, NOT_STISLA_WORKLOAD_TELEMETRY);
    for (size_t i = 0; i < num_queries; ++i) {
        not_stisla_search_enhanced(data, n, queries[i].key, table, &enhanced_config);
    }

    fill_batch(batch, queries, num_queries);
    found = not_stisla_search_parallel(data, n, batch, num_queries, table, 8, config);
    if (found != expected_found) {
        fprintf(stderr, "warmup parallel batch found %zu queries; expected %zu\n", found, expected_found);
        return -1;
    }

    if (fortran_available) {
        fill_batch(batch, queries, num_queries);
        found = not_stisla_search_batch_fortran(data, n, batch, num_queries);
        if (found != expected_found) {
            fprintf(stderr, "warmup Fortran batch found %zu queries; expected %zu\n", found, expected_found);
            return -1;
        }
    }

#if NOT_STISLA_BENCH_AUTO
    fill_batch(batch, queries, num_queries);
    found = not_stisla_search_batch_auto(data, n, batch, num_queries, table, 8, config);
    if (found != expected_found) {
        fprintf(stderr, "warmup auto batch found %zu queries; expected %zu\n", found, expected_found);
        return -1;
    }
#endif

    return 0;
}

int main(void) {
    const char* profile = env_str_or_default("NOT_STISLA_PROFILE", "manual");
    const char* bench_mode = env_str_or_default("NOT_STISLA_BENCH_MODE", "cold");
    const size_t n = env_size_or_default("NOT_STISLA_N", 1000000);
    const size_t num_queries = env_size_or_default("NOT_STISLA_QUERIES", 200000);
    const size_t runs = env_size_or_default("NOT_STISLA_RUNS", 10);
    const size_t hit_rate_pct = env_percent_or_default("NOT_STISLA_HIT_RATE_PCT", 100);
    const size_t data_gap = env_size_or_default("NOT_STISLA_DATA_GAP", 1);
    const size_t data_gap_jitter = env_size_or_default("NOT_STISLA_DATA_GAP_JITTER", 0);
    const size_t query_stride = env_size_or_default("NOT_STISLA_QUERY_STRIDE", 17);
    if (strcmp(bench_mode, "cold") != 0 && strcmp(bench_mode, "warm") != 0) {
        fprintf(stderr, "NOT_STISLA_BENCH_MODE must be 'cold' or 'warm'; got '%s'\n", bench_mode);
        return 1;
    }
    const size_t default_warmup_runs = strcmp(bench_mode, "warm") == 0 ? 1 : 0;
    const size_t warmup_runs =
        env_size_allow_zero_or_default("NOT_STISLA_WARMUP_RUNS", default_warmup_runs);
    if (data_gap > (size_t)INT64_MAX) {
        fprintf(stderr, "NOT_STISLA_DATA_GAP is too large\n");
        return 1;
    }
    if (data_gap_jitter > (size_t)INT64_MAX || data_gap > (size_t)INT64_MAX - data_gap_jitter) {
        fprintf(stderr, "NOT_STISLA_DATA_GAP_JITTER is too large\n");
        return 1;
    }

    const uint64_t data_alloc_start = now_ns();
    int64_t* data = malloc(n * sizeof(int64_t));
    const uint64_t data_alloc_end = now_ns();
    if (!data) {
        fprintf(stderr, "failed to allocate data\n");
        return 1;
    }
    const uint64_t data_init_start = now_ns();
    for (size_t i = 0; i < n; ++i) {
        if (i == 0) {
            data[i] = 0;
            continue;
        }
        size_t step = data_gap_for_index(i, data_gap, data_gap_jitter);
        if (data[i - 1] > INT64_MAX - (int64_t)step) {
            fprintf(stderr, "NOT_STISLA_DATA_GAP is too large for NOT_STISLA_N\n");
            free(data);
            return 1;
        }
        data[i] = data[i - 1] + (int64_t)step;
    }
    const uint64_t data_init_end = now_ns();
    const uint64_t query_alloc_start = now_ns();
    query_t* queries = malloc(num_queries * sizeof(query_t));
    not_stisla_batch_item_t* batch = malloc(num_queries * sizeof(not_stisla_batch_item_t));
    const uint64_t query_alloc_end = now_ns();
    if (!queries || !batch) {
        fprintf(stderr, "failed to allocate query batches\n");
        free(batch);
        free(queries);
        free(data);
        return 1;
    }
    size_t expected_found = 0;
    size_t query_index = 0;
    const size_t query_step = query_stride % n;
    const size_t query_cycle = query_step == 0 ? 1 : n / gcd_size(n, query_step);
    const uint64_t query_init_start = now_ns();
    for (size_t i = 0; i < num_queries; ++i) {
        if ((i % 100) < hit_rate_pct) {
            queries[i].key = data[query_index];
            expected_found++;
        } else {
            queries[i].key = -1 - (int64_t)(i % 1000000);
        }
        query_index += query_step;
        if (query_index >= n) {
            query_index %= n;
        }
    }
    const uint64_t query_init_end = now_ns();
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();
    if (!table) {
        fprintf(stderr, "failed to allocate anchor table\n");
        free(batch);
        free(queries);
        free(data);
        return 1;
    }
    not_stisla_parallel_config_t config = {0, 0, 64};
    const int fortran_available = not_stisla_fortran_backend_available();
    const double data_alloc_ms = (double)(data_alloc_end - data_alloc_start) / 1000000.0;
    const double data_init_ms = (double)(data_init_end - data_init_start) / 1000000.0;
    const double query_alloc_ms = (double)(query_alloc_end - query_alloc_start) / 1000000.0;
    const double query_init_ms = (double)(query_init_end - query_init_start) / 1000000.0;
    const double setup_total_ms = data_alloc_ms + data_init_ms + query_alloc_ms + query_init_ms;
    const size_t data_bytes = n * sizeof(int64_t);
    const size_t query_bytes = num_queries * sizeof(query_t);
    const size_t batch_bytes = num_queries * sizeof(not_stisla_batch_item_t);
    const size_t total_buffer_bytes = data_bytes + query_bytes + batch_bytes;
    const double data_gib = bytes_to_gib(data_bytes);
    const double total_buffer_gib = bytes_to_gib(total_buffer_bytes);

    for (size_t warmup = 0; warmup < warmup_runs; ++warmup) {
        if (run_warmup_pass(
                data,
                n,
                queries,
                num_queries,
                batch,
                expected_found,
                table,
                &config,
                fortran_available) != 0) {
            not_stisla_anchor_table_destroy(table);
            free(batch);
            free(queries);
            free(data);
            return 1;
        }
    }

    printf("#profile=%s\n", profile);
    printf("#bench_mode=%s,warmup_runs=%zu\n", bench_mode, warmup_runs);
    printf("#n=%zu,queries=%zu,runs=%zu\n", n, num_queries, runs);
    printf("#workload=hit_rate_pct=%zu,data_gap=%zu,data_gap_jitter=%zu,query_stride=%zu,query_effective_stride=%zu,query_cycle=%zu,expected_hits=%zu,expected_misses=%zu\n",
           hit_rate_pct,
           data_gap,
           data_gap_jitter,
           query_stride,
           query_step,
           query_cycle,
           expected_found,
           num_queries - expected_found);
    printf("#workload_pattern=data[0]=0,data[i]=data[i-1]+data_gap+jitter(i),jitter=0..data_gap_jitter,hit_when=i%%100<hit_rate_pct,hit_index_stride=query_stride%%n,miss_key=-1-(i%%1000000)\n");
    printf("#env=NOT_STISLA_PROFILE,NOT_STISLA_BENCH_MODE,NOT_STISLA_WARMUP_RUNS,NOT_STISLA_N,NOT_STISLA_QUERIES,NOT_STISLA_RUNS,NOT_STISLA_HIT_RATE_PCT,NOT_STISLA_DATA_GAP,NOT_STISLA_DATA_GAP_JITTER,NOT_STISLA_QUERY_STRIDE\n");
    printf("#setup_ms=data_alloc=%.3f,data_init=%.3f,query_alloc=%.3f,query_init=%.3f,total=%.3f\n",
           data_alloc_ms,
           data_init_ms,
           query_alloc_ms,
           query_init_ms,
           setup_total_ms);
    printf("#buffer_bytes=data=%zu,queries=%zu,batch=%zu,total=%zu,data_gib=%.6f,total_gib=%.6f\n",
           data_bytes,
           query_bytes,
           batch_bytes,
           total_buffer_bytes,
           data_gib,
           total_buffer_gib);
    printf("#effective_data_gib_s=dataset_gib/(backend_ns_per_key*queries);proxy_not_raw_dram_bandwidth\n");
    printf("#auto_backend_bench=%d\n", NOT_STISLA_BENCH_AUTO ? 1 : 0);
    printf("#run,binary_ns,not_stisla_ns,enhanced_ns,batch_parallel_ns,fortran_batch_ns,auto_batch_ns,fortran_available,auto_backend_bench,auto_decision_available,auto_backend,auto_cpu_features,auto_array_bucket,auto_query_bucket,auto_thread_count,auto_estimated_ns_per_key,auto_p95_ns_per_key,data_alloc_ms,data_init_ms,query_alloc_ms,query_init_ms,setup_total_ms,data_bytes,query_bytes,batch_bytes,total_buffer_bytes,data_gib,total_buffer_gib,binary_effective_gib_s,not_stisla_effective_gib_s,enhanced_effective_gib_s,batch_parallel_effective_gib_s,fortran_effective_gib_s,auto_effective_gib_s,bench_mode,warmup_runs\n");
    for (size_t run = 1; run <= runs; ++run) {
        uint64_t start, end;
        start = now_ns();
        size_t binary_found = 0;
        for (size_t i = 0; i < num_queries; ++i) {
            if (binary_search(data, n, queries[i].key) != NOT_STISLA_NOT_FOUND) {
                binary_found++;
            }
        }
        end = now_ns();
        if (binary_found != expected_found) {
            fprintf(stderr, "binary search found %zu queries; expected %zu\n", binary_found, expected_found);
            return 1;
        }
        double binary_avg = (double)(end - start) / num_queries;

        start = now_ns();
        size_t classic_found = 0;
        for (size_t i = 0; i < num_queries; ++i) {
            if (not_stisla_search(data, n, queries[i].key, table, 8) != NOT_STISLA_NOT_FOUND) {
                classic_found++;
            }
        }
        end = now_ns();
        if (classic_found != expected_found) {
            fprintf(stderr, "NOT_STISLA found %zu queries; expected %zu\n", classic_found, expected_found);
            return 1;
        }
        double classic_avg = (double)(end - start) / num_queries;

        not_stisla_config_t enhanced_config;
        not_stisla_config_init(&enhanced_config, NOT_STISLA_WORKLOAD_TELEMETRY);
        start = now_ns();
        size_t enhanced_found = 0;
        for (size_t i = 0; i < num_queries; ++i) {
            if (not_stisla_search_enhanced(data, n, queries[i].key, table, &enhanced_config) != NOT_STISLA_NOT_FOUND) {
                enhanced_found++;
            }
        }
        end = now_ns();
        if (enhanced_found != expected_found) {
            fprintf(stderr, "enhanced NOT_STISLA found %zu queries; expected %zu\n", enhanced_found, expected_found);
            return 1;
        }
        double enhanced_avg = (double)(end - start) / num_queries;

        start = now_ns();
        fill_batch(batch, queries, num_queries);
        size_t found = not_stisla_search_parallel(data, n, batch, num_queries, table, 8, &config);
        end = now_ns();
        if (found != expected_found) {
            fprintf(stderr, "parallel batch found %zu queries; expected %zu\n", found, expected_found);
            return 1;
        }
        double parallel_avg = (double)(end - start) / num_queries;

        double fortran_avg = 0.0;
        if (fortran_available) {
            start = now_ns();
            fill_batch(batch, queries, num_queries);
            found = not_stisla_search_batch_fortran(data, n, batch, num_queries);
            end = now_ns();
            if (found != expected_found) {
                fprintf(stderr, "Fortran batch found %zu queries; expected %zu\n", found, expected_found);
                return 1;
            }
            fortran_avg = (double)(end - start) / num_queries;
        }

        double auto_avg = 0.0;
        int auto_decision_available = 0;
        const char* auto_backend = "disabled";
        uint32_t auto_cpu_features = 0;
        size_t auto_array_bucket = 0;
        size_t auto_query_bucket = 0;
        int auto_thread_count = 0;
        double auto_estimated = 0.0;
        double auto_p95 = 0.0;
#if NOT_STISLA_BENCH_AUTO
        start = now_ns();
        fill_batch(batch, queries, num_queries);
        found = not_stisla_search_batch_auto(data, n, batch, num_queries, table, 8, &config);
        end = now_ns();
        if (found != expected_found) {
            fprintf(stderr, "auto batch found %zu queries; expected %zu\n", found, expected_found);
            return 1;
        }
        auto_avg = (double)(end - start) / num_queries;

        not_stisla_backend_decision_t decision;
        if (not_stisla_get_last_backend_decision(&decision) == 0) {
            auto_decision_available = 1;
            auto_backend = backend_name(decision.backend);
            auto_cpu_features = decision.cpu_features;
            auto_array_bucket = decision.array_size_bucket;
            auto_query_bucket = decision.query_count_bucket;
            auto_thread_count = decision.thread_count;
            auto_estimated = decision.estimated_ns_per_key;
            auto_p95 = decision.p95_ns_per_key;
        } else {
            auto_backend = "unreported";
        }
#endif

        const double binary_effective_gib_s =
            effective_data_gib_s(binary_avg, num_queries, data_bytes);
        const double classic_effective_gib_s =
            effective_data_gib_s(classic_avg, num_queries, data_bytes);
        const double enhanced_effective_gib_s =
            effective_data_gib_s(enhanced_avg, num_queries, data_bytes);
        const double parallel_effective_gib_s =
            effective_data_gib_s(parallel_avg, num_queries, data_bytes);
        const double fortran_effective_gib_s =
            effective_data_gib_s(fortran_avg, num_queries, data_bytes);
        const double auto_effective_gib_s =
            effective_data_gib_s(auto_avg, num_queries, data_bytes);

        printf("%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%s,%u,%zu,%zu,%d,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu,%zu,%zu,%zu,%.6f,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%zu\n",
               run,
               binary_avg,
               classic_avg,
               enhanced_avg,
               parallel_avg,
               fortran_avg,
               auto_avg,
               fortran_available,
               NOT_STISLA_BENCH_AUTO ? 1 : 0,
               auto_decision_available,
               auto_backend,
               auto_cpu_features,
               auto_array_bucket,
               auto_query_bucket,
               auto_thread_count,
               auto_estimated,
               auto_p95,
               data_alloc_ms,
               data_init_ms,
               query_alloc_ms,
               query_init_ms,
               setup_total_ms,
               data_bytes,
               query_bytes,
               batch_bytes,
               total_buffer_bytes,
               data_gib,
               total_buffer_gib,
               binary_effective_gib_s,
               classic_effective_gib_s,
               enhanced_effective_gib_s,
               parallel_effective_gib_s,
               fortran_effective_gib_s,
               auto_effective_gib_s,
               bench_mode,
               warmup_runs);
    }
    not_stisla_anchor_table_destroy(table);
    free(batch);
    free(queries);
    free(data);
    return 0;
}
