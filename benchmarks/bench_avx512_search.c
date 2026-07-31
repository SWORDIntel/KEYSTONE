/*
 * KEYSTONE AVX-512 Search Benchmark
 *
 * Benchmarks the new multi-key parallel search, branchless lower_bound,
 * range search, and sorted batch search against the original single-key
 * AVX-512 search and scalar binary search.
 *
 * Build: gcc -O3 -march=sapphirerapids -I./src -I./include \
 *        benchmarks/bench_avx512_search.c \
 *        src/keystone_avx512.c src/keystone_avx512_search.c \
 *        -lm -lpthread -o benchmarks/bench_avx512_search
 */

#include "keystone_avx512.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define KEYSTONE_NOT_FOUND ((size_t)-1)
#define BENCH_ITER 10000
#define WARMUP 1000

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Scalar binary search for comparison */
static size_t bin_search(const int64_t* arr, size_t n, int64_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < key) lo = mid + 1;
        else if (arr[mid] > key) hi = mid;
        else return mid;
    }
    return KEYSTONE_NOT_FOUND;
}

/* Scalar lower_bound */
static size_t lower_bound_scalar(const int64_t* arr, size_t n, int64_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Generate sorted array with given stride */
static int64_t* gen_sorted(size_t n, int64_t stride) {
    int64_t* arr = aligned_alloc(64, n * sizeof(int64_t));
    for (size_t i = 0; i < n; i++) {
        arr[i] = (int64_t)i * stride;
    }
    return arr;
}

/* Pick random keys that exist in the array */
static void gen_keys(const int64_t* arr, size_t n, int64_t* keys, size_t num_keys) {
    for (size_t k = 0; k < num_keys; k++) {
        size_t idx = (size_t)(rand() % n);
        keys[k] = arr[idx];
    }
}

/* Pick sorted keys for merge-walk benchmark */
static void gen_sorted_keys(const int64_t* arr, size_t n, int64_t* keys, size_t num_keys) {
    for (size_t k = 0; k < num_keys; k++) {
        size_t idx = (size_t)((k * n) / num_keys + (rand() % (n / num_keys)));
        if (idx >= n) idx = n - 1;
        keys[k] = arr[idx];
    }
    /* Sort the keys */
    for (size_t i = 0; i < num_keys; i++) {
        for (size_t j = i + 1; j < num_keys; j++) {
            if (keys[j] < keys[i]) {
                int64_t tmp = keys[i];
                keys[i] = keys[j];
                keys[j] = tmp;
            }
        }
    }
}

static void bench_single_search(const char* label, size_t n, int64_t stride) {
    int64_t* arr = gen_sorted(n, stride);
    int64_t key = arr[n / 2];

    /* Warmup */
    for (int i = 0; i < WARMUP; i++) {
        volatile size_t r = keystone_linear_search_avx512(arr, n, key);
        (void)r;
    }

    double t0 = now_us();
    size_t result = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        result += keystone_linear_search_avx512(arr, n, key);
    }
    double t1 = now_us();
    double ns = (t1 - t0) * 1000.0 / BENCH_ITER;
    printf("  %-35s n=%-8zu  %8.1f ns/op  (result=%zu)\n", label, n, ns, result % BENCH_ITER);

    free(arr);
}

static void bench_multi_search(const char* label, size_t n, int64_t stride, size_t num_keys) {
    int64_t* arr = gen_sorted(n, stride);
    int64_t keys[8];
    size_t results[8];
    gen_keys(arr, n, keys, num_keys);

    /* Warmup */
    for (int i = 0; i < WARMUP; i++) {
        keystone_multi_search_avx512(arr, n, keys, num_keys, results);
    }

    double t0 = now_us();
    size_t found = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        keystone_multi_search_avx512(arr, n, keys, num_keys, results);
        for (size_t k = 0; k < num_keys; k++) found += (results[k] != KEYSTONE_NOT_FOUND);
    }
    double t1 = now_us();
    double ns = (t1 - t0) * 1000.0 / BENCH_ITER;
    double ns_per_key = ns / num_keys;
    printf("  %-35s n=%-8zu keys=%zu  %8.1f ns/op  %6.1f ns/key  (found=%zu)\n",
           label, n, num_keys, ns, ns_per_key, found / BENCH_ITER);

    free(arr);
}

static void bench_vs_single(const char* label, size_t n, int64_t stride, size_t num_keys) {
    int64_t* arr = gen_sorted(n, stride);
    int64_t keys[8];
    size_t results[8];
    gen_keys(arr, n, keys, num_keys);

    /* Benchmark: single-key search x num_keys (sequential) */
    for (int i = 0; i < WARMUP; i++) {
        for (size_t k = 0; k < num_keys; k++)
            results[k] = keystone_linear_search_avx512(arr, n, keys[k]);
    }
    double t0 = now_us();
    size_t found_seq = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        for (size_t k = 0; k < num_keys; k++) {
            results[k] = keystone_linear_search_avx512(arr, n, keys[k]);
            found_seq += (results[k] != KEYSTONE_NOT_FOUND);
        }
    }
    double t1 = now_us();
    double ns_seq = (t1 - t0) * 1000.0 / BENCH_ITER;

    /* Benchmark: multi-key parallel search */
    for (int i = 0; i < WARMUP; i++) {
        keystone_multi_search_avx512(arr, n, keys, num_keys, results);
    }
    t0 = now_us();
    size_t found_par = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        keystone_multi_search_avx512(arr, n, keys, num_keys, results);
        for (size_t k = 0; k < num_keys; k++) found_par += (results[k] != KEYSTONE_NOT_FOUND);
    }
    t1 = now_us();
    double ns_par = (t1 - t0) * 1000.0 / BENCH_ITER;
    double speedup = ns_seq / ns_par;

    printf("  %-35s n=%-8zu keys=%zu  seq=%7.1fns  par=%7.1fns  speedup=%.2fx\n",
           label, n, num_keys, ns_seq, ns_par, speedup);

    free(arr);
}

static void bench_lower_bound(const char* label, size_t n, int64_t stride) {
    int64_t* arr = gen_sorted(n, stride);
    int64_t key = arr[n / 2] + stride / 2;  /* Key that doesn't exist */

    for (int i = 0; i < WARMUP; i++) {
        volatile size_t r = keystone_lower_bound_avx512(arr, n, key);
        (void)r;
    }
    double t0 = now_us();
    size_t result = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        result += keystone_lower_bound_avx512(arr, n, key);
    }
    double t1 = now_us();
    double ns_avx = (t1 - t0) * 1000.0 / BENCH_ITER;

    for (int i = 0; i < WARMUP; i++) {
        volatile size_t r = lower_bound_scalar(arr, n, key);
        (void)r;
    }
    t0 = now_us();
    for (int i = 0; i < BENCH_ITER; i++) {
        result += lower_bound_scalar(arr, n, key);
    }
    t1 = now_us();
    double ns_scalar = (t1 - t0) * 1000.0 / BENCH_ITER;

    printf("  %-35s n=%-8zu  avx512=%7.1fns  scalar=%7.1fns  speedup=%.2fx\n",
           label, n, ns_avx, ns_scalar, ns_scalar / ns_avx);

    free(arr);
}

static void bench_range_search(const char* label, size_t n, int64_t stride) {
    int64_t* arr = gen_sorted(n, stride);
    int64_t lo_key = arr[n / 4];
    int64_t hi_key = arr[3 * n / 4];
    size_t* indices = malloc(n * sizeof(size_t));

    for (int i = 0; i < WARMUP; i++) {
        volatile size_t r = keystone_range_search_avx512(arr, n, lo_key, hi_key, indices, n);
        (void)r;
    }
    double t0 = now_us();
    size_t count = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        count = keystone_range_search_avx512(arr, n, lo_key, hi_key, indices, n);
    }
    double t1 = now_us();
    double ns = (t1 - t0) * 1000.0 / BENCH_ITER;
    printf("  %-35s n=%-8zu  %8.1f ns/op  (matches=%zu)\n", label, n, ns, count);

    free(arr);
    free(indices);
}

static void bench_batch_sorted(const char* label, size_t n, int64_t stride, size_t num_keys) {
    int64_t* arr = gen_sorted(n, stride);
    int64_t* keys = aligned_alloc(64, num_keys * sizeof(int64_t));
    size_t* results = malloc(num_keys * sizeof(size_t));
    gen_sorted_keys(arr, n, keys, num_keys);

    for (int i = 0; i < WARMUP; i++) {
        keystone_batch_search_sorted_avx512(arr, n, keys, num_keys, results);
    }
    double t0 = now_us();
    size_t found = 0;
    for (int i = 0; i < BENCH_ITER; i++) {
        found = keystone_batch_search_sorted_avx512(arr, n, keys, num_keys, results);
    }
    double t1 = now_us();
    double ns = (t1 - t0) * 1000.0 / BENCH_ITER;
    double ns_per_key = ns / num_keys;
    printf("  %-35s n=%-8zu keys=%-5zu  %8.1f ns/op  %6.1f ns/key  (found=%zu)\n",
           label, n, num_keys, ns, ns_per_key, found);

    free(arr);
    free(keys);
    free(results);
}

int main(void) {
    printf("=== KEYSTONE AVX-512 Search Benchmark ===\n\n");

    size_t sizes[] = {256, 1024, 4096, 16384, 65536, 262144};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    printf("--- Single-Key AVX-512 Linear Search ---\n");
    for (int i = 0; i < num_sizes; i++) {
        bench_single_search("AVX-512 single-key", sizes[i], 100);
    }
    printf("\n");

    printf("--- Multi-Key Parallel Search (8 keys) ---\n");
    for (int i = 0; i < num_sizes; i++) {
        bench_multi_search("AVX-512 multi-key (8)", sizes[i], 100, 8);
    }
    printf("\n");

    printf("--- Sequential vs Parallel (8 keys) ---\n");
    for (int i = 0; i < num_sizes; i++) {
        bench_vs_single("8x single vs multi", sizes[i], 100, 8);
    }
    printf("\n");

    printf("--- Sequential vs Parallel (4 keys) ---\n");
    for (int i = 0; i < num_sizes; i++) {
        bench_vs_single("4x single vs multi", sizes[i], 100, 4);
    }
    printf("\n");

    printf("--- Branchless Lower Bound ---\n");
    for (int i = 0; i < num_sizes; i++) {
        bench_lower_bound("lower_bound", sizes[i], 100);
    }
    printf("\n");

    printf("--- Range Search ---\n");
    for (int i = 0; i < num_sizes; i++) {
        bench_range_search("range_search [n/4, 3n/4]", sizes[i], 100);
    }
    printf("\n");

    printf("--- Sorted Batch Search (Merge-Walk) ---\n");
    bench_batch_sorted("merge-walk", 65536, 100, 64);
    bench_batch_sorted("merge-walk", 65536, 100, 256);
    bench_batch_sorted("merge-walk", 262144, 100, 64);
    bench_batch_sorted("merge-walk", 262144, 100, 256);
    bench_batch_sorted("merge-walk", 262144, 100, 1024);

    printf("\n=== Benchmark Complete ===\n");
    return 0;
}
