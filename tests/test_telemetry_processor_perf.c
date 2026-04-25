#include "dsmil_telemetry_processor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_SORTED_EVENTS 4096

static dsmil_telemetry_event_t make_event(
    dsmil_timestamp_t timestamp,
    uint32_t device_id,
    uint32_t event_type
) {
    dsmil_telemetry_event_t event = {
        .timestamp = timestamp,
        .event_type = event_type,
        .device_id = device_id,
        .layer_id = 7,
        .event_data = NULL,
        .data_size = 0
    };

    return event;
}

static void add_event_checked(
    dsmil_telemetry_processor_t *processor,
    dsmil_timestamp_t timestamp,
    uint32_t device_id,
    uint32_t event_type
) {
    dsmil_telemetry_event_t event = make_event(timestamp, device_id, event_type);
    int ret = dsmil_telemetry_processor_add_event(processor, &event);
    assert(ret == DSMIL_SEARCH_SUCCESS);
}

static void assert_stats(
    dsmil_telemetry_processor_t *processor,
    uint32_t expected_events,
    uint32_t expected_searches
) {
    uint32_t total_events = 0;
    uint32_t search_operations = 0;
    uint32_t memory_usage = 0;
    double avg_search_time_ns = 0.0;

    int ret = dsmil_telemetry_processor_get_stats(processor,
                                                  &total_events,
                                                  &search_operations,
                                                  &avg_search_time_ns,
                                                  &memory_usage);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(total_events == expected_events);
    assert(search_operations == expected_searches);
    (void)avg_search_time_ns;
    (void)memory_usage;
}

static void test_sorted_exact_lookup(void) {
    printf("Testing sorted exact lookup...\n");

    dsmil_telemetry_processor_t *processor =
        dsmil_telemetry_processor_create(NUM_SORTED_EVENTS);
    assert(processor != NULL);

    for (size_t i = 0; i < NUM_SORTED_EVENTS; i++) {
        add_event_checked(processor, (dsmil_timestamp_t)(1000 + i * 10),
                          (uint32_t)(i % 11), 100);
    }

    dsmil_telemetry_result_t result;
    int ret = dsmil_telemetry_processor_find_by_timestamp(processor, 1000 + 2048 * 10,
                                                          &result);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(result.event != NULL);
    assert(result.index == 2048);
    assert(result.event->timestamp == 1000 + 2048 * 10);
    assert(result.exact_match_time == result.event->timestamp);
    assert(result.is_exact_match);
    assert_stats(processor, NUM_SORTED_EVENTS, 1);

    ret = dsmil_telemetry_processor_find_by_timestamp(processor, 999999, &result);
    assert(ret == DSMIL_SEARCH_ERROR_NOT_FOUND);
    assert(result.event == NULL);
    assert(result.index == (size_t)-1);
    assert(!result.is_exact_match);
    assert_stats(processor, NUM_SORTED_EVENTS, 2);

    dsmil_telemetry_processor_destroy(processor);
}

static void test_sorted_range_lookup(void) {
    printf("Testing sorted range lookup...\n");

    dsmil_telemetry_processor_t *processor =
        dsmil_telemetry_processor_create(NUM_SORTED_EVENTS);
    assert(processor != NULL);

    for (size_t i = 0; i < NUM_SORTED_EVENTS; i++) {
        add_event_checked(processor, (dsmil_timestamp_t)(50000 + i * 5),
                          (uint32_t)(200 + (i % 5)), 200);
    }

    dsmil_telemetry_result_t results[8];
    size_t num_found = 0;
    int ret = dsmil_telemetry_processor_find_in_time_range(
        processor,
        50000 + 100 * 5,
        50000 + 105 * 5,
        results,
        8,
        &num_found
    );

    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(num_found == 6);
    for (size_t i = 0; i < num_found; i++) {
        assert(results[i].event != NULL);
        assert(results[i].index == 100 + i);
        assert(results[i].event->timestamp == 50000 + (100 + i) * 5);
        assert(results[i].exact_match_time == results[i].event->timestamp);
        assert(!results[i].is_exact_match);
    }

    num_found = 123;
    ret = dsmil_telemetry_processor_find_in_time_range(
        processor,
        50000 + 200 * 5,
        50000 + 220 * 5,
        results,
        3,
        &num_found
    );

    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(num_found == 3);
    assert(results[0].index == 200);
    assert(results[1].index == 201);
    assert(results[2].index == 202);
    assert_stats(processor, NUM_SORTED_EVENTS, 0);

    dsmil_telemetry_processor_destroy(processor);
}

static void test_unsorted_fallback(void) {
    printf("Testing unsorted fallback...\n");

    dsmil_telemetry_processor_t *processor = dsmil_telemetry_processor_create(8);
    assert(processor != NULL);

    add_event_checked(processor, 300, 3, 1);
    add_event_checked(processor, 100, 1, 2);
    add_event_checked(processor, 200, 2, 3);
    add_event_checked(processor, 150, 4, 4);

    dsmil_telemetry_result_t result;
    int ret = dsmil_telemetry_processor_find_by_timestamp(processor, 150, &result);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(result.event != NULL);
    assert(result.index == 3);
    assert(result.event->timestamp == 150);
    assert(result.event->device_id == 4);
    assert(result.is_exact_match);
    assert_stats(processor, 4, 0);

    dsmil_telemetry_result_t results[4];
    size_t num_found = 0;
    ret = dsmil_telemetry_processor_find_in_time_range(processor, 125, 250,
                                                       results, 4, &num_found);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(num_found == 2);
    assert(results[0].index == 2);
    assert(results[0].event->timestamp == 200);
    assert(results[1].index == 3);
    assert(results[1].event->timestamp == 150);
    assert_stats(processor, 4, 0);

    dsmil_telemetry_processor_destroy(processor);
}

static void test_clear_and_reuse(void) {
    printf("Testing clear and reuse...\n");

    dsmil_telemetry_processor_t *processor = dsmil_telemetry_processor_create(8);
    assert(processor != NULL);

    add_event_checked(processor, 400, 1, 1);
    add_event_checked(processor, 100, 2, 2);

    int ret = dsmil_telemetry_processor_clear(processor);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert_stats(processor, 0, 0);

    add_event_checked(processor, 10, 1, 3);
    add_event_checked(processor, 20, 2, 4);
    add_event_checked(processor, 30, 3, 5);

    dsmil_telemetry_result_t result;
    ret = dsmil_telemetry_processor_find_by_timestamp(processor, 20, &result);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(result.event != NULL);
    assert(result.index == 1);
    assert(result.event->timestamp == 20);
    assert(result.event->device_id == 2);
    assert(result.is_exact_match);
    assert_stats(processor, 3, 1);

    dsmil_telemetry_result_t results[4];
    size_t num_found = 0;
    ret = dsmil_telemetry_processor_find_in_time_range(processor, 10, 30,
                                                       results, 4, &num_found);
    assert(ret == DSMIL_SEARCH_SUCCESS);
    assert(num_found == 3);
    assert(results[0].index == 0);
    assert(results[1].index == 1);
    assert(results[2].index == 2);

    dsmil_telemetry_processor_destroy(processor);
}

int main(void) {
    printf("Telemetry processor correctness/performance tests\n");
    printf("=================================================\n\n");

    test_sorted_exact_lookup();
    test_sorted_range_lookup();
    test_unsorted_fallback();
    test_clear_and_reuse();

    printf("\nAll telemetry processor tests passed.\n");
    return 0;
}
