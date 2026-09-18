/**
 * test_fortran_workloads.c
 *
 * Validates and benchmarks Fortran keystone_batch_search_i64 and
 * keystone_search_batch_fortran across sparse, strided, and mixed hit/miss
 * access patterns compared to scalar reference execution.
 */

#include "../include/keystone.h"
#include "../fortran/keystone_batch_backend.h"
#include "test_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#define BENCH_WARMUP_RUNS 2
#define BENCH_MEASURE_RUNS 5

static inline uint64_t time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sort_doubles(double* arr, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        double key = arr[i];
        size_t j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            --j;
        }
        arr[j] = key;
    }
}

static double median_double(double* arr, size_t n) {
    sort_doubles(arr, n);
    return arr[n / 2];
}

/* Compare Fortran vs Scalar batch search results */
static void assert_fortran_matches_scalar(
    const int64_t* data,
    size_t n,
    const int64_t* query_keys,
    size_t num_queries,
    const char* workload_desc
) {
    keystone_batch_item_t* items_scalar = malloc(num_queries * sizeof(keystone_batch_item_t));
    keystone_batch_item_t* items_fortran = malloc(num_queries * sizeof(keystone_batch_item_t));
    int64_t* direct_out = malloc(num_queries * sizeof(int64_t));
    TEST_ASSERT(items_scalar != NULL);
    TEST_ASSERT(items_fortran != NULL);
    TEST_ASSERT(direct_out != NULL);

    for (size_t i = 0; i < num_queries; ++i) {
        items_scalar[i].key = query_keys[i];
        items_scalar[i].result = KEYSTONE_NOT_FOUND;
        items_scalar[i].ordinal = i;

        items_fortran[i].key = query_keys[i];
        items_fortran[i].result = KEYSTONE_NOT_FOUND;
        items_fortran[i].ordinal = i;

        direct_out[i] = -999;
    }

    size_t found_scalar = keystone_search_batch_c_optimized(
        data, n, items_scalar, num_queries, NULL, 8);
    size_t found_fortran = keystone_search_batch_fortran(
        data, n, items_fortran, num_queries);

    /* Direct low-level ABI call to Fortran module */
    keystone_batch_search_i64(data, n, query_keys, num_queries, direct_out);

    TEST_ASSERT(found_scalar == found_fortran);

    for (size_t i = 0; i < num_queries; ++i) {
        if (items_scalar[i].result != items_fortran[i].result) {
            fprintf(stderr, "[%s] Mismatch at query %zu: key=%" PRId64 " scalar=%" PRId64 " fortran=%" PRId64 "\n",
                    workload_desc, i, query_keys[i], items_scalar[i].result, items_fortran[i].result);
            TEST_ASSERT(items_scalar[i].result == items_fortran[i].result);
        }
        TEST_ASSERT(items_scalar[i].ordinal == items_fortran[i].ordinal);

        /* Verify direct Fortran ABI matches */
        keystone_result_t direct_result = (direct_out[i] >= 0) ? (keystone_result_t)direct_out[i] : KEYSTONE_NOT_FOUND;
        TEST_ASSERT(direct_result == items_scalar[i].result);
    }

    free(direct_out);
    free(items_fortran);
    free(items_scalar);
}

/* Benchmark Fortran vs Scalar latency */
typedef struct {
    double scalar_ns_per_key;
    double fortran_ns_per_key;
    double speedup_ratio;
    size_t found_count;
} workload_bench_result_t;

static workload_bench_result_t benchmark_workload(
    const int64_t* data,
    size_t n,
    const int64_t* query_keys,
    size_t num_queries
) {
    keystone_batch_item_t* items = malloc(num_queries * sizeof(keystone_batch_item_t));
    TEST_ASSERT(items != NULL);

    /* Warmup */
    for (int w = 0; w < BENCH_WARMUP_RUNS; ++w) {
        for (size_t i = 0; i < num_queries; ++i) {
            items[i].key = query_keys[i];
            items[i].result = KEYSTONE_NOT_FOUND;
            items[i].ordinal = i;
        }
        keystone_search_batch_c_optimized(data, n, items, num_queries, NULL, 8);

        for (size_t i = 0; i < num_queries; ++i) {
            items[i].key = query_keys[i];
            items[i].result = KEYSTONE_NOT_FOUND;
            items[i].ordinal = i;
        }
        keystone_search_batch_fortran(data, n, items, num_queries);
    }

    double scalar_samples[BENCH_MEASURE_RUNS];
    size_t found_scalar = 0;
    for (int r = 0; r < BENCH_MEASURE_RUNS; ++r) {
        for (size_t i = 0; i < num_queries; ++i) {
            items[i].key = query_keys[i];
            items[i].result = KEYSTONE_NOT_FOUND;
            items[i].ordinal = i;
        }
        uint64_t t0 = time_now_ns();
        found_scalar = keystone_search_batch_c_optimized(data, n, items, num_queries, NULL, 8);
        uint64_t t1 = time_now_ns();
        scalar_samples[r] = (double)(t1 - t0) / (double)num_queries;
    }

    double fortran_samples[BENCH_MEASURE_RUNS];
    size_t found_fortran = 0;
    for (int r = 0; r < BENCH_MEASURE_RUNS; ++r) {
        for (size_t i = 0; i < num_queries; ++i) {
            items[i].key = query_keys[i];
            items[i].result = KEYSTONE_NOT_FOUND;
            items[i].ordinal = i;
        }
        uint64_t t0 = time_now_ns();
        found_fortran = keystone_search_batch_fortran(data, n, items, num_queries);
        uint64_t t1 = time_now_ns();
        fortran_samples[r] = (double)(t1 - t0) / (double)num_queries;
    }

    TEST_ASSERT(found_scalar == found_fortran);
    free(items);

    workload_bench_result_t res;
    res.scalar_ns_per_key = median_double(scalar_samples, BENCH_MEASURE_RUNS);
    res.fortran_ns_per_key = median_double(fortran_samples, BENCH_MEASURE_RUNS);
    res.speedup_ratio = res.scalar_ns_per_key / (res.fortran_ns_per_key > 0.0 ? res.fortran_ns_per_key : 1.0);
    res.found_count = found_fortran;
    return res;
}

static void test_sparse_sorted_workloads(void) {
    printf("Testing Sparse Sorted Workloads...\n");
    const size_t n = 65536;
    int64_t* data = malloc(n * sizeof(int64_t));
    TEST_ASSERT(data != NULL);
    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 4;
    }

    /* Sparse sorted: sample every 16th element */
    const size_t q1_count = 4096;
    int64_t* q1 = malloc(q1_count * sizeof(int64_t));
    TEST_ASSERT(q1 != NULL);
    for (size_t i = 0; i < q1_count; ++i) {
        q1[i] = data[i * 16];
    }
    assert_fortran_matches_scalar(data, n, q1, q1_count, "sparse_sorted_stride16");
    workload_bench_result_t b1 = benchmark_workload(data, n, q1, q1_count);
    printf("  [Sparse stride 16] (Q=%zu, Hits=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
           q1_count, b1.found_count, b1.scalar_ns_per_key, b1.fortran_ns_per_key, b1.speedup_ratio);

    /* Sparse sorted: sample every 64th element */
    const size_t q2_count = 1024;
    int64_t* q2 = malloc(q2_count * sizeof(int64_t));
    TEST_ASSERT(q2 != NULL);
    for (size_t i = 0; i < q2_count; ++i) {
        q2[i] = data[i * 64];
    }
    assert_fortran_matches_scalar(data, n, q2, q2_count, "sparse_sorted_stride64");
    workload_bench_result_t b2 = benchmark_workload(data, n, q2, q2_count);
    printf("  [Sparse stride 64] (Q=%zu, Hits=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
           q2_count, b2.found_count, b2.scalar_ns_per_key, b2.fortran_ns_per_key, b2.speedup_ratio);

    /* Sparse non-uniform sorted with varying gaps */
    const size_t q3_count = 2048;
    int64_t* q3 = malloc(q3_count * sizeof(int64_t));
    TEST_ASSERT(q3 != NULL);
    size_t curr_idx = 0;
    for (size_t i = 0; i < q3_count; ++i) {
        curr_idx += 1 + (i % 29);
        if (curr_idx >= n) curr_idx = n - 1;
        q3[i] = data[curr_idx];
    }
    assert_fortran_matches_scalar(data, n, q3, q3_count, "sparse_sorted_nonuniform");
    workload_bench_result_t b3 = benchmark_workload(data, n, q3, q3_count);
    printf("  [Sparse non-uniform] (Q=%zu, Hits=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
           q3_count, b3.found_count, b3.scalar_ns_per_key, b3.fortran_ns_per_key, b3.speedup_ratio);

    free(q3);
    free(q2);
    free(q1);
    free(data);
    printf("✓ Sparse Sorted Workloads verified successfully.\n\n");
}

static void test_strided_workloads(void) {
    printf("Testing Strided Access Workloads...\n");
    const size_t n = 65536;
    int64_t* data = malloc(n * sizeof(int64_t));
    TEST_ASSERT(data != NULL);
    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 2;
    }

    const size_t num_queries = 8192;
    int64_t* queries = malloc(num_queries * sizeof(int64_t));
    TEST_ASSERT(queries != NULL);

    /* Test positive stride 7 */
    size_t idx = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        queries[i] = data[idx % n];
        idx += 7;
    }
    assert_fortran_matches_scalar(data, n, queries, num_queries, "strided_step7");
    workload_bench_result_t b1 = benchmark_workload(data, n, queries, num_queries);
    printf("  [Strided step +7] (Q=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
           num_queries, b1.scalar_ns_per_key, b1.fortran_ns_per_key, b1.speedup_ratio);

    /* Test positive stride 17 */
    idx = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        queries[i] = data[idx % n];
        idx += 17;
    }
    assert_fortran_matches_scalar(data, n, queries, num_queries, "strided_step17");
    workload_bench_result_t b2 = benchmark_workload(data, n, queries, num_queries);
    printf("  [Strided step +17] (Q=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
           num_queries, b2.scalar_ns_per_key, b2.fortran_ns_per_key, b2.speedup_ratio);

    /* Test negative (reverse) stride */
    for (size_t i = 0; i < num_queries; ++i) {
        queries[i] = data[(num_queries - 1 - i) * (n / num_queries)];
    }
    assert_fortran_matches_scalar(data, n, queries, num_queries, "strided_reverse");
    workload_bench_result_t b3 = benchmark_workload(data, n, queries, num_queries);
    printf("  [Strided reverse] (Q=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
           num_queries, b3.scalar_ns_per_key, b3.fortran_ns_per_key, b3.speedup_ratio);

    free(queries);
    free(data);
    printf("✓ Strided Workloads verified successfully.\n\n");
}

static void test_mixed_hit_miss_workloads(void) {
    printf("Testing Mixed Hit/Miss Distribution Workloads...\n");
    const size_t n = 32768;
    int64_t* data = malloc(n * sizeof(int64_t));
    TEST_ASSERT(data != NULL);
    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 10; /* even multiples of 10 */
    }

    const size_t num_queries = 8192;
    int64_t* queries = malloc(num_queries * sizeof(int64_t));
    TEST_ASSERT(queries != NULL);

    int hit_percentages[] = {100, 75, 50, 25, 0};
    for (size_t h = 0; h < sizeof(hit_percentages)/sizeof(hit_percentages[0]); ++h) {
        int target_pct = hit_percentages[h];
        for (size_t i = 0; i < num_queries; ++i) {
            if ((int)(i % 100) < target_pct) {
                /* Hit: multiple of 10 present in array */
                queries[i] = data[(i * 3) % n];
            } else {
                /* Miss: odd value (e.g. ends in 5) not in array */
                queries[i] = ((int64_t)(i * 3 % n) * 10) + 5;
            }
        }

        char desc[64];
        snprintf(desc, sizeof(desc), "hit_rate_%d_pct", target_pct);
        assert_fortran_matches_scalar(data, n, queries, num_queries, desc);
        workload_bench_result_t b = benchmark_workload(data, n, queries, num_queries);
        printf("  [Hit rate %3d%%] (Q=%zu, Hits=%zu): Scalar=%.2f ns/key, Fortran=%.2f ns/key (%.2fx)\n",
               target_pct, num_queries, b.found_count, b.scalar_ns_per_key, b.fortran_ns_per_key, b.speedup_ratio);
    }

    free(queries);
    free(data);
    printf("✓ Mixed Hit/Miss Workloads verified successfully.\n\n");
}

static void test_auto_route_widening(void) {
    printf("Testing Auto-Route Widening on Sparse and Strided Queries...\n");
    const size_t n = 32768;
    int64_t* data = malloc(n * sizeof(int64_t));
    keystone_batch_item_t* items = malloc(n * sizeof(keystone_batch_item_t));
    TEST_ASSERT(data != NULL);
    TEST_ASSERT(items != NULL);

    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 2;
    }

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    TEST_ASSERT(table != NULL);
    keystone_parallel_config_t config = {
        .num_threads = 8,
        .use_thread_pool = 0,
        .batch_chunk = 64
    };

    /* 1. Sparse Sorted Shape */
    const size_t sparse_q = 8192;
    for (size_t i = 0; i < sparse_q; ++i) {
        items[i].key = data[i * 4]; /* sorted, large regular gap */
        items[i].result = KEYSTONE_NOT_FOUND;
        items[i].ordinal = 0;
    }

    size_t found = keystone_search_batch_auto(data, n, items, sparse_q, table, 8, &config);
    TEST_ASSERT(found == sparse_q);
    for (size_t i = 0; i < sparse_q; ++i) {
        TEST_ASSERT(items[i].result == (keystone_result_t)(i * 4));
        TEST_ASSERT(items[i].ordinal == i);
    }

    keystone_backend_decision_t decision;
    TEST_ASSERT(keystone_get_last_backend_decision(&decision) == 0);
    printf("  [Auto-route Sparse] shape=%s, backend=%s, source=%s, candidates=%zu\n",
           keystone_query_shape_name(decision.query_shape),
           keystone_backend_name(decision.backend),
           keystone_decision_source_name(decision.decision_source),
           decision.candidates_measured);

    TEST_ASSERT(decision.query_shape == KEYSTONE_QUERY_SHAPE_SPARSE_SORTED);
    TEST_ASSERT(decision.backend == KEYSTONE_BACKEND_FORTRAN ||
                decision.backend == KEYSTONE_BACKEND_SCALAR ||
                decision.backend == KEYSTONE_BACKEND_C_OPENMP);

    /* 2. Strided Shape */
    const size_t strided_q = 8192;
    for (size_t i = 0; i < strided_q; ++i) {
        size_t idx = (strided_q - 1 - i); /* descending stride */
        items[i].key = data[idx * 3];
        items[i].result = KEYSTONE_NOT_FOUND;
        items[i].ordinal = 0;
    }

    found = keystone_search_batch_auto(data, n, items, strided_q, table, 8, &config);
    TEST_ASSERT(found == strided_q);
    for (size_t i = 0; i < strided_q; ++i) {
        size_t expected_idx = (strided_q - 1 - i) * 3;
        TEST_ASSERT(items[i].result == (keystone_result_t)expected_idx);
        TEST_ASSERT(items[i].ordinal == i);
    }

    TEST_ASSERT(keystone_get_last_backend_decision(&decision) == 0);
    printf("  [Auto-route Strided] shape=%s, backend=%s, source=%s, candidates=%zu\n",
           keystone_query_shape_name(decision.query_shape),
           keystone_backend_name(decision.backend),
           keystone_decision_source_name(decision.decision_source),
           decision.candidates_measured);

    TEST_ASSERT(decision.query_shape == KEYSTONE_QUERY_SHAPE_STRIDED);
    TEST_ASSERT(decision.backend == KEYSTONE_BACKEND_FORTRAN ||
                decision.backend == KEYSTONE_BACKEND_SCALAR ||
                decision.backend == KEYSTONE_BACKEND_C_OPENMP);

    keystone_anchor_table_destroy(table);
    free(items);
    free(data);
    printf("✓ Auto-Route Widening verified successfully.\n\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Fortran Workload & Scientific Search Test Suite      \n");
    printf("=================================================================\n\n");

    if (!keystone_fortran_backend_available()) {
        printf("Notice: Fortran backend unavailable in this build environment.\n");
        printf("Skipping Fortran workload benchmarks.\n");
        return 0;
    }

    test_sparse_sorted_workloads();
    test_strided_workloads();
    test_mixed_hit_miss_workloads();
    test_auto_route_widening();

    printf("=================================================================\n");
    printf("  All Fortran Workload Tests & Benchmarks Passed (100%% Verified)  \n");
    printf("=================================================================\n");
    return 0;
}
