#include "../include/not_stisla.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void test_backend_unavailable_fallback(void) {
    int64_t data[] = {-10, -3, 0, 8, 42};
    not_stisla_batch_item_t items[] = {
        {.key = -10},
        {.key = 42},
        {.key = 7},
    };

    size_t found = not_stisla_search_batch_fortran(
        data,
        ARRAY_LEN(data),
        items,
        ARRAY_LEN(items)
    );

    assert(found == 0);
    for (size_t i = 0; i < ARRAY_LEN(items); ++i) {
        assert(items[i].result == NOT_STISLA_NOT_FOUND);
        assert(items[i].ordinal == i);
    }
}

static void assert_batch_results(const int64_t* data,
                                 size_t n,
                                 not_stisla_batch_item_t* items,
                                 size_t num_items,
                                 const not_stisla_result_t* expected,
                                 size_t expected_found) {
    size_t found = not_stisla_search_batch_fortran(data, n, items, num_items);
    assert(found == expected_found);

    for (size_t i = 0; i < num_items; ++i) {
        assert(items[i].result == expected[i]);
        assert(items[i].ordinal == i);
    }
}

static void test_fortran_hits_and_misses(void) {
    int64_t data[] = {-12, -4, 0, 9, 17, 32};
    not_stisla_batch_item_t items[] = {
        {.key = -12},
        {.key = -5},
        {.key = 0},
        {.key = 31},
        {.key = 32},
        {.key = 100},
    };
    not_stisla_result_t expected[] = {
        0,
        NOT_STISLA_NOT_FOUND,
        2,
        NOT_STISLA_NOT_FOUND,
        5,
        NOT_STISLA_NOT_FOUND,
    };

    assert_batch_results(data, ARRAY_LEN(data), items, ARRAY_LEN(items), expected, 3);
}

static void test_fortran_duplicate_query_keys(void) {
    int64_t data[] = {-3, 0, 5, 9, 11};
    not_stisla_batch_item_t items[] = {
        {.key = 5},
        {.key = 5},
        {.key = 7},
        {.key = 7},
        {.key = 11},
        {.key = 11},
    };
    not_stisla_result_t expected[] = {
        2,
        2,
        NOT_STISLA_NOT_FOUND,
        NOT_STISLA_NOT_FOUND,
        4,
        4,
    };

    assert_batch_results(data, ARRAY_LEN(data), items, ARRAY_LEN(items), expected, 4);
}

static void test_fortran_unsorted_keys(void) {
    int64_t data[] = {2, 4, 6, 8, 10, 12};
    not_stisla_batch_item_t items[] = {
        {.key = 10},
        {.key = 2},
        {.key = 11},
        {.key = 6},
        {.key = 4},
    };
    not_stisla_result_t expected[] = {
        4,
        0,
        NOT_STISLA_NOT_FOUND,
        2,
        1,
    };

    assert_batch_results(data, ARRAY_LEN(data), items, ARRAY_LEN(items), expected, 4);
}

static void test_fortran_duplicate_data_matches_binary_semantics(void) {
    int64_t data[] = {1, 3, 3, 3, 5, 7, 9};
    not_stisla_batch_item_t items[] = {
        {.key = 3},
        {.key = 3},
        {.key = 4},
        {.key = 7},
    };
    not_stisla_result_t expected[ARRAY_LEN(items)];

    for (size_t i = 0; i < ARRAY_LEN(items); ++i) {
        expected[i] = NOT_STISLA_NOT_FOUND;
    }
    expected[0] = 3;
    expected[1] = 3;
    expected[3] = 5;

    assert_batch_results(data, ARRAY_LEN(data), items, ARRAY_LEN(items), expected, 3);
}

static void test_fortran_sorted_key_fast_path_correctness(void) {
    int64_t data[256];
    not_stisla_batch_item_t items[128];
    not_stisla_result_t expected[128];

    for (size_t i = 0; i < ARRAY_LEN(data); ++i) {
        data[i] = (int64_t)i * 2;
    }

    for (size_t i = 0; i < ARRAY_LEN(items); ++i) {
        items[i].key = (int64_t)i * 3;
        if (items[i].key % 2 == 0) {
            expected[i] = (not_stisla_result_t)(items[i].key / 2);
        } else {
            expected[i] = NOT_STISLA_NOT_FOUND;
        }
    }

    assert_batch_results(data, ARRAY_LEN(data), items, ARRAY_LEN(items), expected, 64);
}

static void test_fortran_batch_matches_scalar(void) {
    int64_t data[] = {
        -500, -21, -3, 0, 4, 8, 13, 99, 1024, 4096, 9999999999LL
    };
    const size_t n = ARRAY_LEN(data);
    not_stisla_batch_item_t items[] = {
        {.key = -500},
        {.key = -4},
        {.key = -3},
        {.key = 0},
        {.key = 7},
        {.key = 8},
        {.key = 9999999999LL},
        {.key = 9999999998LL},
        {.key = -500},
    };
    const size_t num_items = ARRAY_LEN(items);
    not_stisla_anchor_table_t* table = not_stisla_anchor_table_create();

    assert(table != NULL);

    size_t found = not_stisla_search_batch_fortran(data, n, items, num_items);
    assert(found == 6);

    for (size_t i = 0; i < num_items; ++i) {
        not_stisla_result_t expected = not_stisla_search(
            data,
            n,
            items[i].key,
            table,
            8
        );
        assert(items[i].result == expected);
        assert(items[i].ordinal == i);
    }

    not_stisla_anchor_table_destroy(table);
}

static void test_empty_inputs(void) {
    int64_t data[] = {1, 2, 3};
    not_stisla_batch_item_t item = {.key = 2};

    assert(not_stisla_search_batch_fortran(data, 0, &item, 1) == 0);
    assert(item.result == NOT_STISLA_NOT_FOUND);
    assert(not_stisla_search_batch_fortran(data, 3, NULL, 1) == 0);
    assert(not_stisla_search_batch_fortran(NULL, 3, &item, 1) == 0);
    assert(not_stisla_search_batch_fortran(data, 3, &item, 0) == 0);
}

int main(void) {
    printf("Running Fortran backend tests\n");
    printf("=============================\n\n");

    test_empty_inputs();

    if (!not_stisla_fortran_backend_available()) {
        test_backend_unavailable_fallback();
        printf("Fortran backend unavailable; fallback behavior verified.\n");
        return 0;
    }

    test_fortran_batch_matches_scalar();
    test_fortran_hits_and_misses();
    test_fortran_duplicate_query_keys();
    test_fortran_unsorted_keys();
    test_fortran_duplicate_data_matches_binary_semantics();
    test_fortran_sorted_key_fast_path_correctness();
    printf("Fortran backend correctness verified.\n");
    return 0;
}
