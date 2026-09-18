/**
 * test_memory_ramp.c
 *
 * Tests memory ramp capacity scaling (1M - 16M elements) and cold-cache
 * profiling via cache invalidation sweeps (dirtying and reading a 64MB+ buffer).
 * Tracks ru_maxrss, ru_minflt, and ru_majflt via getrusage() and validates
 * keystone_optimize_array_memory() (madvise(MADV_HUGEPAGE)).
 */

#include "../include/keystone.h"
#include "test_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <unistd.h>

#define EVICT_BUFFER_SIZE_BYTES (64 * 1024 * 1024) /* 64 MB */

static inline uint64_t time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Dirty and read a 64MB+ memory buffer to evict L1/L2/L3 caches without root drop_caches */
static void evict_cpu_caches(uint8_t* evict_buf, size_t size_bytes) {
    TEST_ASSERT(evict_buf != NULL);
    TEST_ASSERT(size_bytes >= EVICT_BUFFER_SIZE_BYTES);

    /* Phase 1: Dirty the entire working set buffer (forces cache allocations) */
    for (size_t i = 0; i < size_bytes; i += 64) {
        evict_buf[i] = (uint8_t)((i * 31 + 17) & 0xFF);
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Phase 2: Read through the buffer to displace all lingering cache lines */
    volatile uint64_t checksum = 0;
    for (size_t i = 0; i < size_bytes; i += 64) {
        checksum += evict_buf[i];
    }

    __asm__ volatile("" : : "r"(checksum) : "memory");
}

static void test_optimize_array_memory_boundary_conditions(void) {
    printf("Testing keystone_optimize_array_memory boundary conditions...\n");

    int64_t dummy = 42;
    TEST_ASSERT(keystone_optimize_array_memory(NULL, 1000) == -1);
    TEST_ASSERT(keystone_optimize_array_memory(&dummy, 0) == -1);

    /* Array smaller than 1MB threshold should succeed as no-op */
    const size_t small_n = 1024;
    int64_t* small_arr = malloc(small_n * sizeof(int64_t));
    TEST_ASSERT(small_arr != NULL);
    TEST_ASSERT(keystone_optimize_array_memory(small_arr, small_n) == 0);
    free(small_arr);

    printf("✓ Boundary conditions verified.\n\n");
}

static void test_ram_sweeps(void) {
    printf("Testing RAM Sweeps (1M -> 16M elements) with rusage & huge-page verification...\n");

    const size_t test_sizes[] = {
        1000000,   /* ~8 MB */
        2000000,   /* ~16 MB */
        4000000,   /* ~32 MB */
        8000000,   /* ~64 MB */
        16000000   /* ~128 MB */
    };
    const size_t num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (size_t s = 0; s < num_sizes; ++s) {
        size_t n = test_sizes[s];
        size_t bytes = n * sizeof(int64_t);
        double mib = (double)bytes / (1024.0 * 1024.0);

        struct rusage usage_before, usage_after;
        getrusage(RUSAGE_SELF, &usage_before);

        int64_t* arr = NULL;
        int alloc_ret = posix_memalign((void**)&arr, 4096, bytes);
        TEST_ASSERT(alloc_ret == 0);
        TEST_ASSERT(arr != NULL);

        /* Touch/initialize data */
        for (size_t i = 0; i < n; ++i) {
            arr[i] = (int64_t)i * 3;
        }

        /* Verify transparent huge-page hints */
        int opt_ret = keystone_optimize_array_memory(arr, n);
        TEST_ASSERT(opt_ret == 0);

        getrusage(RUSAGE_SELF, &usage_after);

        long delta_minflt = usage_after.ru_minflt - usage_before.ru_minflt;
        long delta_majflt = usage_after.ru_majflt - usage_before.ru_majflt;
        long max_rss_kb = usage_after.ru_maxrss;

        printf("  [N=%8zu (~%6.1f MiB)] RSS=%ld KB, MinFlt=%+ld, MajFlt=%+ld, Optimize=OK\n",
               n, mib, max_rss_kb, delta_minflt, delta_majflt);

        /* Zero major page faults expected in allocated RAM */
        TEST_ASSERT(delta_majflt == 0);

        /* Validate correctness by searching sample elements across the array */
        const size_t num_queries = 2048;
        keystone_batch_item_t* items = malloc(num_queries * sizeof(keystone_batch_item_t));
        TEST_ASSERT(items != NULL);

        for (size_t q = 0; q < num_queries; ++q) {
            size_t target_idx = (q * (n / num_queries)) % n;
            items[q].key = arr[target_idx];
            items[q].result = KEYSTONE_NOT_FOUND;
            items[q].ordinal = q;
        }

        size_t found = keystone_search_batch_c_optimized(arr, n, items, num_queries, NULL, 8);
        TEST_ASSERT(found == num_queries);
        for (size_t q = 0; q < num_queries; ++q) {
            size_t target_idx = (q * (n / num_queries)) % n;
            TEST_ASSERT(items[q].result == (keystone_result_t)target_idx);
            TEST_ASSERT(items[q].ordinal == q);
        }

        free(items);
        free(arr);
    }

    printf("✓ RAM Capacity Sweeps verified successfully.\n\n");
}

static void test_cold_cache_profiling(void) {
    printf("Testing Cold-Cache Profiling via 64MB Cache Invalidation Sweeps...\n");

    /* Allocate 64MB eviction buffer */
    uint8_t* evict_buf = NULL;
    int ret = posix_memalign((void**)&evict_buf, 4096, EVICT_BUFFER_SIZE_BYTES);
    TEST_ASSERT(ret == 0);
    TEST_ASSERT(evict_buf != NULL);

    /* Array size: 4M elements (32 MB) */
    const size_t n = 4000000;
    const size_t bytes = n * sizeof(int64_t);
    int64_t* data = NULL;
    ret = posix_memalign((void**)&data, 4096, bytes);
    TEST_ASSERT(ret == 0);
    TEST_ASSERT(data != NULL);

    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 2;
    }
    TEST_ASSERT(keystone_optimize_array_memory(data, n) == 0);

    const size_t num_queries = 4096;
    keystone_batch_item_t* items_cold = malloc(num_queries * sizeof(keystone_batch_item_t));
    keystone_batch_item_t* items_warm = malloc(num_queries * sizeof(keystone_batch_item_t));
    TEST_ASSERT(items_cold != NULL);
    TEST_ASSERT(items_warm != NULL);

    for (size_t i = 0; i < num_queries; ++i) {
        size_t idx = (i * 997) % n;
        items_cold[i].key = data[idx];
        items_cold[i].result = KEYSTONE_NOT_FOUND;
        items_cold[i].ordinal = i;

        items_warm[i].key = data[idx];
        items_warm[i].result = KEYSTONE_NOT_FOUND;
        items_warm[i].ordinal = i;
    }

    /* 1. Cold Pass: Invalidate L1/L2/L3 cache */
    evict_cpu_caches(evict_buf, EVICT_BUFFER_SIZE_BYTES);

    struct rusage rusage_cold_before, rusage_cold_after;
    getrusage(RUSAGE_SELF, &rusage_cold_before);
    uint64_t t0 = time_now_ns();
    size_t found_cold = keystone_search_batch_c_optimized(data, n, items_cold, num_queries, NULL, 8);
    uint64_t t1 = time_now_ns();
    getrusage(RUSAGE_SELF, &rusage_cold_after);
    double cold_ns_per_key = (double)(t1 - t0) / (double)num_queries;

    /* 2. Warm Pass: Immediately re-execute with warm cache */
    struct rusage rusage_warm_before, rusage_warm_after;
    getrusage(RUSAGE_SELF, &rusage_warm_before);
    uint64_t t2 = time_now_ns();
    size_t found_warm = keystone_search_batch_c_optimized(data, n, items_warm, num_queries, NULL, 8);
    uint64_t t3 = time_now_ns();
    getrusage(RUSAGE_SELF, &rusage_warm_after);
    double warm_ns_per_key = (double)(t3 - t2) / (double)num_queries;

    /* Correctness cross-check */
    TEST_ASSERT(found_cold == num_queries);
    TEST_ASSERT(found_warm == num_queries);
    for (size_t i = 0; i < num_queries; ++i) {
        TEST_ASSERT(items_cold[i].result == items_warm[i].result);
        TEST_ASSERT(items_cold[i].ordinal == items_warm[i].ordinal);
    }

    double cold_warm_ratio = cold_ns_per_key / (warm_ns_per_key > 0.0 ? warm_ns_per_key : 1.0);
    printf("  [Cold Search]: %.2f ns/key (found=%zu, minflt=%+ld, majflt=%+ld)\n",
           cold_ns_per_key, found_cold,
           rusage_cold_after.ru_minflt - rusage_cold_before.ru_minflt,
           rusage_cold_after.ru_majflt - rusage_cold_before.ru_majflt);
    printf("  [Warm Search]: %.2f ns/key (found=%zu, minflt=%+ld, majflt=%+ld)\n",
           warm_ns_per_key, found_warm,
           rusage_warm_after.ru_minflt - rusage_warm_before.ru_minflt,
           rusage_warm_after.ru_majflt - rusage_warm_before.ru_majflt);
    printf("  [Cold Penalty Ratio]: %.2fx\n", cold_warm_ratio);

    TEST_ASSERT(cold_ns_per_key > 0.0);
    TEST_ASSERT(warm_ns_per_key > 0.0);

    free(items_warm);
    free(items_cold);
    free(data);
    free(evict_buf);
    printf("✓ Cold-Cache Profiling verified successfully.\n\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Memory Ramp & Cold-Cache Verification Test Suite     \n");
    printf("=================================================================\n\n");

    test_optimize_array_memory_boundary_conditions();
    test_ram_sweeps();
    test_cold_cache_profiling();

    printf("=================================================================\n");
    printf("  All Memory Ramp & Cold-Cache Tests Passed (100%% Verified)      \n");
    printf("=================================================================\n");
    return 0;
}
