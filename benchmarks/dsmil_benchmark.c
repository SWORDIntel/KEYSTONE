/**
 * DSMIL KEYSTONE Benchmark Suite
 *
 * Simple performance verification for KEYSTONE
 */

#include "../include/keystone.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <assert.h>
#include "benchmark_writer.h"
#include "../include/dsmil_keystone_wrapper.h"

/* Global optimization flags */
static int g_optimize_graviton4 = 0;
static char* g_dataset_path = NULL;
static char* g_tar_zst_path = NULL;
static char* g_tar_zst_member = NULL;

/* Timing utilities */
static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Load dataset from file */
static int64_t* load_dataset(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("Failed to open dataset");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    *n = size / sizeof(int64_t);
    int64_t* data = malloc(size);
    if (data) {
        if (fread(data, sizeof(int64_t), *n, f) != *n) {
            fprintf(stderr, "Error: Read incomplete dataset\n");
            free(data);
            data = NULL;
        }
    }

    fclose(f);
    return data;
}

/* Apply AWS Graviton4 specific optimizations */
static void apply_graviton4_optimizations(keystone_anchor_table_t* table) {
    /* Manual override or library-level auto-detection */
    uint32_t cpu_features = keystone_detect_cpu_features();
    if (g_optimize_graviton4 || (cpu_features & KEYSTONE_CPU_GRAVITON4)) {
        if (table) {
            /* Restrict anchor table to 2MB L2 cache per core (approx 65,536 anchors) */
            keystone_anchor_table_set_memory_limit(table, 65536);
        }
    }
}

/* Binary search for comparison */
static size_t bin_search(const int64_t* arr, size_t n, int64_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < key) {
            lo = mid + 1;
        } else if (arr[mid] > key) {
            hi = mid;
        } else {
            return mid;
        }
    }
    return SIZE_MAX;
}

/* Benchmark tuned/enhanced KEYSTONE search */
static void benchmark_tuned_search(const int64_t* data, size_t data_size,
                                   const int64_t* queries, size_t num_queries) {
    printf("\nTuned/Enhanced KEYSTONE Search Benchmark\n");
    printf("==========================================\n");

    /* Initialize benchmark configuration */
    keystone_config_t config;
    keystone_config_init(&config, KEYSTONE_WORKLOAD_TELEMETRY);
    keystone_set_performance_tracking(1);
    keystone_reset_performance_stats();

    /* Initialize anchor table for learned search hints */
    keystone_anchor_table_t* table = keystone_anchor_table_create();
    assert(table && "Failed to create tuned anchor table");

    /* Benchmark tuned/enhanced search */
    uint64_t tuned_start = ns_now();
    size_t tuned_found = 0;
    size_t tuned_successful = 0;

    for (size_t i = 0; i < num_queries; ++i) {
        keystone_result_t result = keystone_search_enhanced(
            data, data_size, queries[i], table, &config
        );
        if (result != KEYSTONE_NOT_FOUND) {
            tuned_found++;
            tuned_successful++;
        }
    }
    uint64_t tuned_time = ns_now() - tuned_start;

    double tuned_ns_per_op = (double)tuned_time / num_queries;
    double tuned_success_rate = (double)tuned_successful / num_queries * 100.0;

    printf("Tuned Search:      %.2f ns/op (%zu/%zu found, %.1f%% success)\n",
           tuned_ns_per_op, tuned_found, num_queries, tuned_success_rate);

    /* Get tuned/enhanced search statistics */
    keystone_performance_stats_t stats;
    if (keystone_get_performance_stats(&stats) == 0) {
        printf("Tuned Stats:       %llu searches, %.2f ns avg\n",
               (unsigned long long)stats.total_searches,
               stats.avg_search_time_ns);
    }

    keystone_anchor_table_destroy(table);
}

static void benchmark_auto_batch_search(const char* label,
                                        const int64_t* data, size_t data_size,
                                        const int64_t* queries, size_t num_queries) {
    printf("\nAuto Backend Batch Search Benchmark (%s)\n", label);
    printf("========================================\n");

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    assert(table && "Failed to create auto batch anchor table");
    apply_graviton4_optimizations(table);

    keystone_parallel_config_t config = {
        .num_threads = 8,
        .use_thread_pool = 0,
        .batch_chunk = 0
    };

    keystone_batch_item_t* items = calloc(num_queries, sizeof(*items));
    assert(items && "Failed to allocate batch items");

    for (size_t i = 0; i < num_queries; ++i) {
        items[i].key = queries[i];
        items[i].ordinal = i;
        items[i].result = KEYSTONE_NOT_FOUND;
    }

    uint64_t auto_start = ns_now();
    size_t auto_found = keystone_search_batch_auto(
        data, data_size, items, num_queries, table, 8, &config
    );
    uint64_t auto_time = ns_now() - auto_start;
    double auto_ns_per_key = (double)auto_time / num_queries;

    printf("Auto Batch Search: %.2f ns/key (%zu/%zu found)\n",
           auto_ns_per_key, auto_found, num_queries);

    keystone_backend_decision_t decision;
    if (keystone_get_last_backend_decision(&decision) == 0) {
        printf("Backend Decision:  backend=%s source=%s shape=%s threads=%d\n",
               keystone_backend_name(decision.backend),
               keystone_decision_source_name(
                   (keystone_backend_decision_source_t)decision.decision_source
               ),
               keystone_query_shape_name((keystone_query_shape_t)decision.query_shape),
               decision.thread_count);
        printf("Calibration:       median=%.2f ns/key p95=%.2f ns/key runs=%zu candidates=%zu\n",
               decision.estimated_ns_per_key,
               decision.p95_ns_per_key,
               decision.calibration_runs,
               decision.candidates_measured);
        printf("Decision Buckets:  cpu_features=0x%08x array=%zu queries=%zu\n",
               decision.cpu_features,
               decision.array_size_bucket,
               decision.query_count_bucket);
               
        keystone_bench_writer_config_t bw_config = {
            .file_path = NULL,
            .format = KEYSTONE_BENCH_FORMAT_CSV,
            .append = 1
        };
        keystone_bench_writer_t* writer = keystone_bench_writer_create(&bw_config);
        if (writer) {
            keystone_bench_record_t record = {0};
            record.profile_name = label;
            record.array_size = decision.array_size_bucket; /* Or data_size? bucket is fine or actual data_size */
            record.num_queries = decision.query_count_bucket;
            record.thread_count = decision.thread_count;
            record.throughput_gib_s = 0; /* not computed here */
            record.median_latency_ns = decision.estimated_ns_per_key;
            record.p95_latency_ns = decision.p95_ns_per_key;
            record.hit_rate_pct = -1;
            record.data_gap = -1;
            record.data_gap_jitter = -1;
            record.query_stride = -1;
            record.decision_available = 1;
            record.backend_name = keystone_backend_name(decision.backend);
            record.decision_source = keystone_decision_source_name((keystone_backend_decision_source_t)decision.decision_source);
            record.query_shape = keystone_query_shape_name((keystone_query_shape_t)decision.query_shape);
            record.cpu_features = decision.cpu_features;
            record.calibration_runs = decision.calibration_runs;
            record.candidates_measured = decision.candidates_measured;

            keystone_bench_writer_record(writer, &record);
            keystone_bench_writer_destroy(writer);
        }
    } else {
        printf("Backend Decision:  unavailable\n");
    }

    free(items);
    keystone_anchor_table_destroy(table);
}

/* Comprehensive benchmark comparing all algorithms */
static void benchmark_comprehensive(const int64_t* data, size_t data_size,
                                  const int64_t* queries, size_t num_queries) {
    printf("🔬 Comprehensive Algorithm Comparison\n");
    printf("=====================================\n");

    /* Benchmark baseline binary search */
    uint64_t bin_start = ns_now();
    size_t bin_found = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        if (bin_search(data, data_size, queries[i]) != SIZE_MAX) {
            bin_found++;
        }
    }
    uint64_t bin_time = ns_now() - bin_start;
    double bin_ns_per_op = (double)bin_time / num_queries;

    /* Benchmark KEYSTONE core search */
    keystone_anchor_table_t* table = keystone_anchor_table_create();
    assert(table && "Failed to create anchor table");

    /* Apply Graviton4 optimizations (if not already handled by library defaults) */
    apply_graviton4_optimizations(table);

    uint64_t core_start = ns_now();
    size_t core_found = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        if (keystone_search(data, data_size, queries[i], table, 8) != KEYSTONE_NOT_FOUND) {
            core_found++;
        }
    }
    uint64_t core_time = ns_now() - core_start;
    double core_ns_per_op = (double)core_time / num_queries;

    /* Benchmark tuned/enhanced KEYSTONE search */
    keystone_config_t config;
    keystone_config_init(&config, KEYSTONE_WORKLOAD_TELEMETRY);
    keystone_set_performance_tracking(1);
    keystone_reset_performance_stats();

    /* Apply Graviton4 optimizations (if not already handled by library defaults) */
    apply_graviton4_optimizations(table);

    uint64_t tuned_start = ns_now();
    size_t tuned_found = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        keystone_result_t result = keystone_search_enhanced(
            data, data_size, queries[i], table, &config
        );
        if (result != KEYSTONE_NOT_FOUND) {
            tuned_found++;
        }
    }
    uint64_t tuned_time = ns_now() - tuned_start;
    double tuned_ns_per_op = (double)tuned_time / num_queries;

    /* Results */
    printf("Algorithm                     | Time/op | Found | Speedup vs Baseline\n");
    printf("------------------------------|---------|-------|--------------------\n");
    printf("Baseline Binary Search        | %6.1f ns| %5zu | 1.00x\n",
           bin_ns_per_op, bin_found);
    printf("KEYSTONE Core Search        | %6.1f ns| %5zu | %.2fx\n",
           core_ns_per_op, core_found, bin_ns_per_op / core_ns_per_op);
    printf("Tuned/Enhanced KEYSTONE     | %6.1f ns| %5zu | %.2fx\n",
           tuned_ns_per_op, tuned_found, bin_ns_per_op / tuned_ns_per_op);

    keystone_performance_stats_t stats;
    if (keystone_get_performance_stats(&stats) == 0) {
        printf("\nTuned/Enhanced Search Details:\n");
        printf("Total searches:       %llu\n", (unsigned long long)stats.total_searches);
        printf("Successful searches:  %llu\n", (unsigned long long)stats.successful_searches);
        printf("Average search time:  %.2f ns\n", stats.avg_search_time_ns);
        printf("Reported speedup:     use measured table above\n");
    }

    /* Cleanup */
    keystone_anchor_table_destroy(table);
}

#ifdef KEYSTONE_ENABLE_TAR_ZST
/* Benchmark tar.zst streaming search */
static void benchmark_tar_zst_search(const char* archive_path,
                                      const char* member_name,
                                      const int64_t* queries,
                                      size_t num_queries) {
    printf("\n.tar.zst Streaming Search Benchmark\n");
    printf("=====================================\n");
    printf("Archive: %s\n", archive_path);
    printf("Member:  %s\n", member_name);

    keystone_config_t config;
    keystone_config_init(&config, KEYSTONE_WORKLOAD_TELEMETRY);
    keystone_set_performance_tracking(1);
    keystone_reset_performance_stats();

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    assert(table && "Failed to create anchor table");

    uint64_t tz_start = ns_now();
    size_t tz_found = 0;
    dsmil_telemetry_result_t* results = calloc(num_queries, sizeof(dsmil_telemetry_result_t));
    const char* members[] = { member_name };

    /* Batch search is far more efficient than reopening the archive per query */
    int rc = dsmil_search_batch_tar_zst(
        NULL, archive_path, members, 1,
        (int64_t*)queries, num_queries, results
    );

    if (rc == DSMIL_SEARCH_SUCCESS) {
        for (size_t i = 0; i < num_queries; ++i) {
            if (results[i].is_exact_match) {
                tz_found++;
            }
        }
    } else {
        fprintf(stderr, "Archive batch search failed with code %d\n", rc);
    }
    
    free(results);
    uint64_t tz_time = ns_now() - tz_start;

    double tz_ns_per_op = (double)tz_time / num_queries;
    printf("tar.zst Search:    %.2f ns/op (%zu/%zu found)\n",
           tz_ns_per_op, tz_found, num_queries);

    keystone_tar_zst_stats_t tz_stats;
    /* stats would need to be accumulated across opens; simplified here */
    (void)tz_stats;

    keystone_anchor_table_destroy(table);
}
#endif /* KEYSTONE_ENABLE_TAR_ZST */

/* Generate uniform test data */
static void generate_test_data(int64_t* arr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        arr[i] = (int64_t)i * 2;  // 0, 2, 4, 6, ... (uniform)
    }
}

int main(int argc, char** argv) {
    /* Parse CLI arguments */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--graviton4") == 0) {
            g_optimize_graviton4 = 1;
        } else if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) {
            g_dataset_path = argv[++i];
        } else if (strcmp(argv[i], "--tar-zst") == 0 && i + 1 < argc) {
            g_tar_zst_path = argv[++i];
        } else if (strcmp(argv[i], "--tar-zst-member") == 0 && i + 1 < argc) {
            g_tar_zst_member = argv[++i];
        }
    }

    printf("DSMIL KEYSTONE Search Benchmark Suite\n");
    printf("KEYSTONE Version: %s\n", keystone_version());
    printf("Build: %s\n", keystone_build_info());

    uint32_t cpu_features = keystone_detect_cpu_features();
    if (g_optimize_graviton4 || (cpu_features & KEYSTONE_CPU_GRAVITON4)) {
        printf("\n⚙️  AWS Graviton4 Optimizations ACTIVE (%s):\n", 
               (cpu_features & KEYSTONE_CPU_GRAVITON4) ? "Auto-detected" : "Manual override");
        printf("   - Architecture: 64-bit ARM (Neoverse V2)\n");
        printf("   - Threads: 48 vCPUs (~2.7 GHz)\n");
        printf("   - Cache: L1 64KB I/D, L2 2MB dedicated per core\n");
        printf("   - Tuning: Anchor table locked to 2MB L2 boundary\n");
    }
    
    printf("\nHost Capabilities Detected:\n");
    printf("   - AVX2 Support:    %s\n", (cpu_features & KEYSTONE_CPU_AVX2) ? "YES" : "NO");
    printf("   - AVX-512 Support: %s\n", (cpu_features & KEYSTONE_CPU_AVX512) ? "YES" : "NO");
    printf("   - AMX Support:     %s (detection only, not actively routed)\n", (cpu_features & KEYSTONE_CPU_AMX) ? "YES" : "NO");
    
    printf("\n");
    size_t DATA_SIZE = 100000;
    size_t NUM_QUERIES = 50000;
    int64_t* data = NULL;

    if (g_dataset_path) {
        printf("📂 Loading custom dataset: %s\n", g_dataset_path);
        data = load_dataset(g_dataset_path, &DATA_SIZE);
        if (!data) return 1;
        printf("   - Elements: %zu\n", DATA_SIZE);
    } else {
        /* Generate test data */
        data = malloc(DATA_SIZE * sizeof(int64_t));
        assert(data && "Failed to allocate memory");
        generate_test_data(data, DATA_SIZE);
    }

    int64_t* queries = malloc(NUM_QUERIES * sizeof(int64_t));
    assert(queries && "Failed to allocate memory");

    /* Generate queries (all exist in data) */
    srand(42);
    for (size_t i = 0; i < NUM_QUERIES; ++i) {
        size_t idx = rand() % DATA_SIZE;
        queries[i] = data[idx];
    }

    /* Warm-up phase */
    printf("Warming up search paths...\n");
    keystone_anchor_table_t* warm_table = keystone_anchor_table_create();
    apply_graviton4_optimizations(warm_table);
    for (size_t i = 0; i < 1000; ++i) {
        keystone_search(data, DATA_SIZE, queries[i % 1000], warm_table, 8);
    }
    keystone_anchor_table_destroy(warm_table);

    /* Run comprehensive benchmark */
    benchmark_comprehensive(data, DATA_SIZE, queries, NUM_QUERIES);

    /* Additional tuned/enhanced benchmark */
    benchmark_tuned_search(data, DATA_SIZE, queries, NUM_QUERIES);

    /* Auto backend benchmark with decision provenance */
    benchmark_auto_batch_search("random query profile", data, DATA_SIZE, queries, NUM_QUERIES);

    const size_t DENSE_SORTED_QUERIES = DATA_SIZE < 8192 ? DATA_SIZE : 8192;
    int64_t* dense_sorted_queries = (int64_t*)calloc(DENSE_SORTED_QUERIES, sizeof(int64_t));
    assert(dense_sorted_queries && "Failed to allocate dense sorted queries");
    for (size_t i = 0; i < DENSE_SORTED_QUERIES; ++i) {
        dense_sorted_queries[i] = data[i % DATA_SIZE];
    }
    benchmark_auto_batch_search("dense sorted profile",
                                data, DATA_SIZE, dense_sorted_queries, DENSE_SORTED_QUERIES);
    
    int64_t* sparse_sorted_queries = (int64_t*)calloc(DENSE_SORTED_QUERIES, sizeof(int64_t));
    assert(sparse_sorted_queries && "Failed to allocate sparse sorted queries");
    for (size_t i = 0; i < DENSE_SORTED_QUERIES; ++i) {
        sparse_sorted_queries[i] = data[(i * 10) % DATA_SIZE];
        /* ensure strictly increasing to be sorted */
        if (i > 0 && sparse_sorted_queries[i] <= sparse_sorted_queries[i-1]) {
            sparse_sorted_queries[i] = sparse_sorted_queries[i-1] + 10;
        }
    }
    benchmark_auto_batch_search("sparse sorted profile",
                                data, DATA_SIZE, sparse_sorted_queries, DENSE_SORTED_QUERIES);

    int64_t* strided_queries = (int64_t*)calloc(DENSE_SORTED_QUERIES, sizeof(int64_t));
    assert(strided_queries && "Failed to allocate strided queries");
    for (size_t i = 0; i < DENSE_SORTED_QUERIES; ++i) {
        strided_queries[i] = data[0] + (int64_t)i * 15;
    }
    benchmark_auto_batch_search("strided profile",
                                data, DATA_SIZE, strided_queries, DENSE_SORTED_QUERIES);

    int64_t* mixed_queries = (int64_t*)calloc(NUM_QUERIES, sizeof(int64_t));
    assert(mixed_queries && "Failed to allocate mixed queries");
    for (size_t i = 0; i < NUM_QUERIES; ++i) {
        if (rand() % 2 == 0) {
            mixed_queries[i] = data[rand() % DATA_SIZE]; /* hit */
        } else {
            mixed_queries[i] = data[rand() % DATA_SIZE] + 1; /* likely miss since data is 0, 2, 4... */
        }
    }
    benchmark_auto_batch_search("mixed hit-rate profile",
                                data, DATA_SIZE, mixed_queries, NUM_QUERIES);

    free(dense_sorted_queries);
    free(sparse_sorted_queries);
    free(strided_queries);
    free(mixed_queries);

#ifdef KEYSTONE_ENABLE_TAR_ZST
    /* tar.zst streaming benchmark */
    if (g_tar_zst_path && g_tar_zst_member) {
        benchmark_tar_zst_search(g_tar_zst_path, g_tar_zst_member,
                                  queries, NUM_QUERIES);
    }
#endif

    printf("\nKEYSTONE Search Technology\n");
    printf("============================\n");
    printf("✓ Baseline binary search comparison\n");
    printf("✓ Anchor-guided core search\n");
    printf("✓ Tuned/enhanced search configuration\n");
    printf("✓ Auto backend calibration decision reporting\n");
    printf("✓ SIMD-aware CPU feature detection\n");
    printf("✓ Workload-optimized anchor management\n");

    printf("\nKEYSTONE benchmark suite completed.\n");
    printf("Realistic search paths compared against the binary-search baseline.\n");

    /* Cleanup */
    free(queries);
    free(data);

    return 0;
}
