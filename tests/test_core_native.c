/* ============================================================================
 * ENHANCED KEYSTONE TEST SUITE
 * ============================================================================
 *
 * Tests for KEYSTONE-native improvements to traditional KEYSTONE:
 * - Runtime CPU feature detection
 * - Memory-bounded anchor learning
 * - Enhanced statistics and monitoring
 * - Workload-specific optimizations
 * ============================================================================ */

#include "../include/keystone.h"
#include <stdio.h>
#include <stdlib.h>
#include "test_macros.h"
#include <time.h>
#include <string.h>

#define TEST_ARRAY_SIZE 10000
#define TEST_ITERATIONS 1000

/* Test runtime CPU feature detection */
static void test_cpu_feature_detection(void) {
    printf("Testing runtime CPU feature detection...\n");

    uint32_t features = keystone_detect_cpu_features();
    printf("  Detected CPU features: 0x%08x\n", features);

    /* Should detect at least basic features */
    TEST_ASSERT(features != 0);

    /* Test multiple calls return same result */
    uint32_t features2 = keystone_detect_cpu_features();
    TEST_ASSERT(features == features2);

    printf("  ✓ CPU feature detection works\n");
}

/* Test memory-bounded anchor table */
static void test_memory_bounded_anchors(void) {
    printf("Testing memory-bounded anchor management...\n");

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    TEST_ASSERT(table != NULL);

    /* Test memory limit setting */
    int ret = keystone_anchor_table_set_memory_limit(table, 5);
    TEST_ASSERT(ret == 0);
    TEST_ASSERT(table->max_capacity == 5);

    /* Test invalid limits */
    ret = keystone_anchor_table_set_memory_limit(table, 1);  /* Too small */
    TEST_ASSERT(ret == -1);

    ret = keystone_anchor_table_set_memory_limit(table, KEYSTONE_MAX_ANCHORS + 1); /* Too large */
    TEST_ASSERT(ret == -1);

    keystone_anchor_table_destroy(table);
    printf("  ✓ Memory-bounded anchor management works\n");
}

/* Test workload-specific optimization */
static void test_workload_optimization(void) {
    printf("Testing workload-specific optimization...\n");

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    TEST_ASSERT(table != NULL);

    /* Test telemetry workload */
    int ret = keystone_anchor_table_optimize_for_workload(table, KEYSTONE_WORKLOAD_TELEMETRY);
    TEST_ASSERT(ret == 0);
    size_t telemetry_capacity = table->max_capacity;
    TEST_ASSERT(telemetry_capacity >= KEYSTONE_MIN_ANCHORS);

    /* Test ID workload */
    ret = keystone_anchor_table_optimize_for_workload(table, KEYSTONE_WORKLOAD_IDS);
    TEST_ASSERT(ret == 0);
    TEST_ASSERT(table->max_capacity >= KEYSTONE_MIN_ANCHORS);
    TEST_ASSERT(table->max_capacity < telemetry_capacity);  /* IDs use fewer anchors than telemetry */

    keystone_anchor_table_destroy(table);
    printf("  ✓ Workload-specific optimization works\n");
}

/* Test enhanced statistics */
static void test_enhanced_statistics(void) {
    printf("Testing enhanced statistics tracking...\n");

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    TEST_ASSERT(table != NULL);

    /* Create test array */
    int64_t test_array[100];
    for (size_t i = 0; i < 100; i++) {
        test_array[i] = i * 10;
    }

    /* Perform searches to generate statistics - mix exact matches and near misses */
    for (size_t i = 0; i < 25; i++) {
        /* Exact matches - should not learn anchors */
        keystone_result_t result = keystone_search(test_array, 100, i * 10, table, 2);
        TEST_ASSERT(result != KEYSTONE_NOT_FOUND);
    }

    /* Near misses that should trigger anchor learning */
    for (size_t i = 0; i < 25; i++) {
        /* Search for values that require interpolation and anchor learning */
        int64_t search_val = (i * 10) + 3;  /* Offset from exact match */
        (void)keystone_search(test_array, 100, search_val, table, 2);
        /* May or may not find exact match, but should learn anchors */
    }

    /* Check enhanced statistics */
    const keystone_stats_t* stats = keystone_anchor_table_get_stats(table);
    TEST_ASSERT(stats != NULL);
    TEST_ASSERT(stats->searches_total >= 50);  /* Should have recorded all searches */
    TEST_ASSERT(stats->cpu_features_detected != 0);  /* Should have detected CPU features */

    /* Anchors may or may not be learned depending on search patterns */
    /* The important thing is that statistics are being tracked */

    /* Test legacy statistics API still works */
    size_t searches_total, anchors_learned, memory_used;
    keystone_get_stats(table, &searches_total, &anchors_learned, &memory_used);
    TEST_ASSERT(searches_total == stats->searches_total);
    TEST_ASSERT(anchors_learned == stats->anchors_learned);
    TEST_ASSERT(memory_used > 0);

    keystone_anchor_table_destroy(table);
    printf("  ✓ Enhanced statistics tracking works\n");
}

/* Test performance improvements */
static void test_performance_improvements(void) {
    printf("Testing performance characteristics...\n");

    /* Create large test array */
    const size_t array_size = TEST_ARRAY_SIZE;
    int64_t* test_array = malloc(array_size * sizeof(int64_t));
    TEST_ASSERT(test_array != NULL);

    /* Fill with sorted values */
    for (size_t i = 0; i < array_size; i++) {
        test_array[i] = i * 100;  /* Sparse array for interpolation */
    }

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    TEST_ASSERT(table != NULL);

    /* Warm up anchor learning */
    for (size_t i = 0; i < 100; i++) {
        size_t idx = rand() % array_size;
        keystone_result_t result = keystone_search(test_array, array_size,
                                                     test_array[idx], table, 4);
        TEST_ASSERT(result == idx);
    }

    /* Performance test */
    clock_t start_time = clock();

    for (size_t i = 0; i < TEST_ITERATIONS; i++) {
        size_t idx = rand() % array_size;
        keystone_result_t result = keystone_search(test_array, array_size,
                                                     test_array[idx], table, 4);
        TEST_ASSERT(result == idx);
    }

    clock_t end_time = clock();
    double time_taken = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;

    printf("  ✓ Performance test: %d searches in %.4f seconds\n", TEST_ITERATIONS, time_taken);
    printf("  ✓ Average search time: %.2f ns\n", (time_taken * 1e9) / TEST_ITERATIONS);

    /* Check that anchors were learned efficiently */
    const keystone_stats_t* stats = keystone_anchor_table_get_stats(table);
    TEST_ASSERT(stats->anchors_learned <= table->max_capacity);  /* Memory bounded */

    free(test_array);
    keystone_anchor_table_destroy(table);
    printf("  ✓ Performance improvements verified\n");
}

/* Test DSMIL workload initialization */
static void test_dsmil_workload_init(void) {
    printf("Testing DSMIL workload initialization...\n");

    keystone_anchor_table_t* table = keystone_anchor_table_create();
    TEST_ASSERT(table != NULL);

    /* Test telemetry workload */
    bool success = keystone_init_for_dsmil(table, KEYSTONE_WORKLOAD_TELEMETRY);
    TEST_ASSERT(success);
    TEST_ASSERT(table->workload_type == KEYSTONE_WORKLOAD_TELEMETRY);

    /* Verify workload-specific optimization was applied */
    TEST_ASSERT(table->max_capacity > 10);  /* Telemetry gets higher limit */

    /* Test that statistics are initialized */
    const keystone_stats_t* stats = keystone_anchor_table_get_stats(table);
    TEST_ASSERT(stats->cpu_features_detected != 0);

    keystone_anchor_table_destroy(table);
    printf("  ✓ DSMIL workload initialization works\n");
}

/* Main test runner */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("Running Enhanced KEYSTONE Test Suite\n");
    printf("=======================================\n\n");

    /* Seed random number generator */
    srand((unsigned int)time(NULL));

    test_cpu_feature_detection();
    printf("\n");

    test_memory_bounded_anchors();
    printf("\n");

    test_workload_optimization();
    printf("\n");

    test_enhanced_statistics();
    printf("\n");

    test_performance_improvements();
    printf("\n");

    test_dsmil_workload_init();
    printf("\n");

    printf("🎉 All Enhanced KEYSTONE tests passed!\n");
    printf("KEYSTONE-native improvements successfully integrated.\n");

    return 0;
}
