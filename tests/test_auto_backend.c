#include "../include/not_stisla.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void fill_data(int64_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        data[i] = (int64_t)i * 3;
    }
}

static void fill_items(not_stisla_batch_item_t* items, size_t num_items) {
    for (size_t i = 0; i < num_items; ++i) {
        items[i].key = (int64_t)i * 3;
        items[i].result = NOT_STISLA_NOT_FOUND;
        items[i].ordinal = 0;
    }
}

static void assert_all_found(const not_stisla_batch_item_t* items, size_t num_items) {
    for (size_t i = 0; i < num_items; ++i) {
        assert(items[i].result == i);
        assert(items[i].ordinal == i);
    }
}

static void test_no_decision_before_auto_call(void) {
    not_stisla_backend_decision_t decision;
    assert(not_stisla_get_last_backend_decision(&decision) == -1);
    assert(not_stisla_get_last_backend_decision(NULL) == -1);
}

static void test_invalid_inputs_use_safe_scalar_decision(void) {
    int64_t data[4] = {0, 3, 6, 9};
    not_stisla_batch_item_t items[2];
    not_stisla_backend_decision_t decision;

    fill_items(items, 2);

    assert(not_stisla_search_batch_auto(NULL, 4, items, 2, NULL, 8, NULL) == 0);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.array_size_bucket == 4);
    assert(decision.query_count_bucket == 2);

    assert(not_stisla_search_batch_auto(data, 4, NULL, 2, NULL, 8, NULL) == 0);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.array_size_bucket == 4);
    assert(decision.query_count_bucket == 2);

    assert(not_stisla_search_batch_auto(data, 0, items, 2, NULL, 8, NULL) == 0);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.array_size_bucket == 0);
    assert(decision.query_count_bucket == 2);

    assert(not_stisla_search_batch_auto(data, 4, items, 0, NULL, 8, NULL) == 0);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.array_size_bucket == 4);
    assert(decision.query_count_bucket == 0);
}

static void test_small_batch_uses_scalar(void) {
    int64_t data[64];
    not_stisla_batch_item_t items[8];
    not_stisla_backend_decision_t decision;
    not_stisla_parallel_config_t config = {
        .num_threads = 4,
        .use_thread_pool = 0,
        .batch_chunk = 16
    };
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(table != NULL);
    fill_data(data, 64);
    fill_items(items, 8);

    size_t found = not_stisla_search_batch_auto(data, 64, items, 8, table, 8, &config);

    assert(found == 8);
    assert_all_found(items, 8);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.array_size_bucket == 64);
    assert(decision.query_count_bucket == 8);
    assert(decision.estimated_ns_per_key >= 0.0);
    assert(decision.p95_ns_per_key >= 0.0);

    not_stisla_anchor_table_destroy(table);
}

static void test_sorted_8k_batch_uses_expected_backend(void) {
    const size_t n = 8192;
    int64_t* data = malloc(n * sizeof(int64_t));
    not_stisla_batch_item_t* items = malloc(n * sizeof(not_stisla_batch_item_t));
    not_stisla_backend_decision_t decision;
    not_stisla_parallel_config_t config = {
        .num_threads = 16,
        .use_thread_pool = 0,
        .batch_chunk = 64
    };
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(data != NULL);
    assert(items != NULL);
    assert(table != NULL);
    fill_data(data, n);
    fill_items(items, n);

    size_t found = not_stisla_search_batch_auto(data, n, items, n, table, 8, &config);

    assert(found == n);
    assert_all_found(items, n);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    if (not_stisla_fortran_backend_available()) {
        assert(decision.backend == NOT_STISLA_BACKEND_FORTRAN);
    } else {
        assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    }
    assert(decision.query_count_bucket == 8192);

    not_stisla_anchor_table_destroy(table);
    free(items);
    free(data);
}

static void test_unsorted_8k_batch_uses_scalar(void) {
    const size_t n = 8192;
    int64_t* data = malloc(n * sizeof(int64_t));
    not_stisla_batch_item_t* items = malloc(n * sizeof(not_stisla_batch_item_t));
    not_stisla_backend_decision_t decision;
    not_stisla_parallel_config_t config = {
        .num_threads = 16,
        .use_thread_pool = 0,
        .batch_chunk = 64
    };
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(data != NULL);
    assert(items != NULL);
    assert(table != NULL);
    fill_data(data, n);

    for (size_t i = 0; i < n; ++i) {
        size_t index = n - 1 - i;
        items[i].key = (int64_t)index * 3;
        items[i].result = NOT_STISLA_NOT_FOUND;
        items[i].ordinal = 0;
    }

    size_t found = not_stisla_search_batch_auto(data, n, items, n, table, 8, &config);

    assert(found == n);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.query_count_bucket == 8192);

    not_stisla_anchor_table_destroy(table);
    free(items);
    free(data);
}

static void test_large_single_thread_batch_uses_scalar(void) {
    const size_t n = 20000;
    int64_t* data = malloc(n * sizeof(int64_t));
    not_stisla_batch_item_t* items = malloc(n * sizeof(not_stisla_batch_item_t));
    not_stisla_backend_decision_t decision;
    not_stisla_parallel_config_t config = {
        .num_threads = 1,
        .use_thread_pool = 0,
        .batch_chunk = 64
    };
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(data != NULL);
    assert(items != NULL);
    assert(table != NULL);
    fill_data(data, n);
    fill_items(items, n);

    size_t found = not_stisla_search_batch_auto(data, n, items, n, table, 8, &config);

    assert(found == n);
    assert_all_found(items, n);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.thread_count == 1);
    assert(decision.estimated_ns_per_key >= 0.0);
    assert(decision.p95_ns_per_key >= 0.0);
    assert(decision.p95_ns_per_key == decision.estimated_ns_per_key);

    not_stisla_anchor_table_destroy(table);
    free(items);
    free(data);
}

static void test_repeated_large_batch_decision_is_stable(void) {
    const size_t n = 20000;
    int64_t* data = malloc(n * sizeof(int64_t));
    not_stisla_batch_item_t* items = malloc(n * sizeof(not_stisla_batch_item_t));
    not_stisla_backend_decision_t first_decision;
    not_stisla_backend_decision_t second_decision;
    not_stisla_parallel_config_t config = {
        .num_threads = 2,
        .use_thread_pool = 0,
        .batch_chunk = 64
    };
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(data != NULL);
    assert(items != NULL);
    assert(table != NULL);
    fill_data(data, n);

    fill_items(items, n);
    assert(not_stisla_search_batch_auto(data, n, items, n, table, 8, &config) == n);
    assert_all_found(items, n);
    assert(not_stisla_get_last_backend_decision(&first_decision) == 0);

    fill_items(items, n);
    assert(not_stisla_search_batch_auto(data, n, items, n, table, 8, &config) == n);
    assert_all_found(items, n);
    assert(not_stisla_get_last_backend_decision(&second_decision) == 0);

    assert(second_decision.backend == first_decision.backend);
    assert(second_decision.cpu_features == first_decision.cpu_features);
    assert(second_decision.array_size_bucket == first_decision.array_size_bucket);
    assert(second_decision.query_count_bucket == first_decision.query_count_bucket);
    assert(second_decision.thread_count == first_decision.thread_count);
    assert(second_decision.estimated_ns_per_key >= 0.0);
    assert(second_decision.p95_ns_per_key >= 0.0);

    not_stisla_anchor_table_destroy(table);
    free(items);
    free(data);
}

static void test_large_multi_thread_batch_policy(void) {
    const size_t n = 32768;
    int64_t* data = malloc(n * sizeof(int64_t));
    not_stisla_batch_item_t* items = malloc(n * sizeof(not_stisla_batch_item_t));
    not_stisla_backend_decision_t decision;
    not_stisla_parallel_config_t config = {
        .num_threads = 2,
        .use_thread_pool = 0,
        .batch_chunk = 64
    };
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(data != NULL);
    assert(items != NULL);
    assert(table != NULL);
    fill_data(data, n);
    fill_items(items, n);

    size_t found = not_stisla_search_batch_auto(data, n, items, n, table, 8, &config);

    assert(found == n);
    assert_all_found(items, n);
    assert(not_stisla_get_last_backend_decision(&decision) == 0);
#ifdef _OPENMP
    assert(decision.backend == NOT_STISLA_BACKEND_C_OPENMP);
    assert(decision.thread_count == 2);
#else
    assert(decision.backend == NOT_STISLA_BACKEND_SCALAR);
    assert(decision.thread_count == 1);
#endif
    assert(decision.query_count_bucket == 32768);
    assert(decision.estimated_ns_per_key >= 0.0);
    assert(decision.p95_ns_per_key >= 0.0);

    not_stisla_anchor_table_destroy(table);
    free(items);
    free(data);
}

int main(void) {
    printf("Running auto backend selector tests\n");
    printf("===================================\n\n");

    test_no_decision_before_auto_call();
    test_invalid_inputs_use_safe_scalar_decision();
    test_small_batch_uses_scalar();
    test_sorted_8k_batch_uses_expected_backend();
    test_unsorted_8k_batch_uses_scalar();
    test_large_single_thread_batch_uses_scalar();
    test_repeated_large_batch_decision_is_stable();
    test_large_multi_thread_batch_policy();

    printf("Auto backend selector policy verified.\n");
    return 0;
}
