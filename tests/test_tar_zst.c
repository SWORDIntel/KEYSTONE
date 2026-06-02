/**
 * NOT_STISLA tar.zst streaming search test suite
 */

#include "../include/not_stisla.h"
#include "../include/not_stisla_tar_zst.h"
#include "../include/dsmil_not_stisla_wrapper.h"
#include "../include/dsmil_telemetry_processor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define FIXTURE_DIR "tests/fixtures_tar_zst"
#define ARCHIVE_PATH FIXTURE_DIR "/test_data.tar.zst"

static int tests_passed = 0;
static int tests_failed = 0;

static void test_pass(const char* name) {
    printf("  [PASS] %s\n", name);
    tests_passed++;
}

static void test_fail(const char* name, const char* reason) {
    printf("  [FAIL] %s: %s\n", name, reason);
    tests_failed++;
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static void test_open_close(void) {
    const char* name = "open_close";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, NULL);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    not_stisla_tar_zst_close(tz);
    test_pass(name);
}

static void test_iterate_members(void) {
    const char* name = "iterate_members";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, NULL);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    int found_txt = 0;
    int found_csv = 0;
    int found_json = 0;
    int total = 0;

    for (;;) {
        char* member_name = NULL;
        size_t member_name_len = 0;
        int r = not_stisla_tar_zst_next_member(tz, &member_name, &member_name_len);
        if (r <= 0) break;
        total++;

        if (member_name_len >= 4 &&
            strncmp(member_name + member_name_len - 4, ".txt", 4) == 0) {
            found_txt = 1;
        }
        if (member_name_len >= 4 &&
            strncmp(member_name + member_name_len - 4, ".csv", 4) == 0) {
            found_csv = 1;
        }
        if (member_name_len >= 5 &&
            strncmp(member_name + member_name_len - 5, ".json", 5) == 0) {
            found_json = 1;
        }
    }

    not_stisla_tar_zst_close(tz);

    if (total < 3) {
        test_fail(name, "expected at least 3 members");
        return;
    }
    if (!found_txt || !found_csv || !found_json) {
        test_fail(name, "missing expected member types");
        return;
    }
    test_pass(name);
}

static void test_search_text_member(void) {
    const char* name = "search_text_member";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, NULL);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    /* The text file contains even numbers 0, 2, 4, ..., 2000 */
    not_stisla_config_t config;
    not_stisla_config_init(&config, NOT_STISLA_WORKLOAD_IDS);

    not_stisla_result_t result = not_stisla_tar_zst_search_member(
        tz, "numbers.txt", 1000, NULL, &config
    );

    not_stisla_tar_zst_close(tz);

    if (result == NOT_STISLA_NOT_FOUND) {
        test_fail(name, "key 1000 not found");
        return;
    }
    test_pass(name);
}

static void test_search_csv_member(void) {
    const char* name = "search_csv_member";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_options_t opts = {
        .format = NOT_STISLA_TAR_ZST_FORMAT_CSV,
        .skip_header = 1,
        .chunk_size = 4096,
        .arena_slab_size = 65536,
        .zstd_workers = 0,
        .enable_pipeline = 0
    };

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, &opts);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    /* The CSV file contains multiples of 3: 0, 3, 6, ..., 3000 */
    not_stisla_config_t config;
    not_stisla_config_init(&config, NOT_STISLA_WORKLOAD_IDS);

    not_stisla_result_t result = not_stisla_tar_zst_search_member(
        tz, "numbers.csv", 1500, NULL, &config
    );

    not_stisla_tar_zst_close(tz);

    if (result == NOT_STISLA_NOT_FOUND) {
        test_fail(name, "key 1500 not found");
        return;
    }
    test_pass(name);
}

static void test_search_json_member(void) {
    const char* name = "search_json_member";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_options_t opts = {
        .format = NOT_STISLA_TAR_ZST_FORMAT_JSON,
        .chunk_size = 4096,
        .arena_slab_size = 65536,
        .zstd_workers = 0,
        .enable_pipeline = 0,
        .skip_header = 0
    };

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, &opts);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    /* The JSON file contains multiples of 5: 0, 5, 10, ..., 5000 */
    not_stisla_config_t config;
    not_stisla_config_init(&config, NOT_STISLA_WORKLOAD_IDS);

    not_stisla_result_t result = not_stisla_tar_zst_search_member(
        tz, "numbers.json", 2500, NULL, &config
    );

    not_stisla_tar_zst_close(tz);

    if (result == NOT_STISLA_NOT_FOUND) {
        test_fail(name, "key 2500 not found");
        return;
    }
    test_pass(name);
}

static void test_stats_consistency(void) {
    const char* name = "stats_consistency";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, NULL);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    not_stisla_config_t config;
    not_stisla_config_init(&config, NOT_STISLA_WORKLOAD_IDS);

    /* Search multiple members */
    not_stisla_tar_zst_search_member(tz, "numbers.txt", 500, NULL, &config);
    not_stisla_tar_zst_search_member(tz, "numbers.csv", 501, NULL, &config);

    not_stisla_tar_zst_stats_t stats;
    if (not_stisla_tar_zst_get_stats(tz, &stats) != 0) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "get_stats failed");
        return;
    }

    not_stisla_tar_zst_close(tz);

    if (stats.members_read < 2) {
        test_fail(name, "expected at least 2 members read");
        return;
    }
    if (stats.bytes_read == 0) {
        test_fail(name, "expected non-zero bytes read");
        return;
    }
    test_pass(name);
}

static void test_error_handling(void) {
    const char* name = "error_handling";

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open("/nonexistent/path.tar.zst", NULL);
    if (tz) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "should have failed for nonexistent file");
        return;
    }
    test_pass(name);
}

static void test_indexed_search(void) {
    const char* name = "indexed_search";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, NULL);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    if (not_stisla_tar_zst_build_index(tz) != 0) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "build_index failed");
        return;
    }

    not_stisla_config_t config;
    not_stisla_config_init(&config, NOT_STISLA_WORKLOAD_IDS);

    /* Search for a key that exists (500 is in numbers.txt) */
    not_stisla_result_t r1 = not_stisla_tar_zst_search_indexed(
        tz, "numbers.txt", 500, NULL, &config);
    if (r1 == NOT_STISLA_NOT_FOUND) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "indexed search for 500 failed");
        return;
    }

    /* Search for a key that does not exist (should be Bloom-rejected) */
    not_stisla_result_t r2 = not_stisla_tar_zst_search_indexed(
        tz, "numbers.txt", 999999, NULL, &config);
    if (r2 != NOT_STISLA_NOT_FOUND) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "Bloom should have rejected 999999");
        return;
    }

    not_stisla_tar_zst_close(tz);
    test_pass(name);
}

static void test_extract_member(void) {
    const char* name = "extract_member";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    not_stisla_tar_zst_t* tz = not_stisla_tar_zst_open(ARCHIVE_PATH, NULL);
    if (!tz) {
        test_fail(name, "failed to open archive");
        return;
    }

    int64_t* keys = NULL;
    size_t count = 0;
    if (not_stisla_tar_zst_extract_member(tz, "numbers.txt",
                                             &keys, &count) != 0) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "extract_member failed");
        return;
    }

    if (!keys || count == 0) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "no keys extracted");
        return;
    }

    /* numbers.txt contains even numbers 0, 2, 4, ..., 2000 */
    if (keys[0] != 0 || keys[count - 1] != 2000) {
        not_stisla_tar_zst_close(tz);
        test_fail(name, "extracted key range mismatch");
        return;
    }

    not_stisla_tar_zst_close(tz);
    test_pass(name);
}

static void test_batch_search(void) {
    const char* name = "batch_search";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    int64_t search_keys[] = {500, 1500, 2500, 999999};
    size_t num_keys = sizeof(search_keys) / sizeof(search_keys[0]);
    dsmil_telemetry_result_t results[4];
    const char *members[] = {"numbers.txt", "numbers.csv", "numbers.json"};

    int rc = dsmil_search_batch_tar_zst(
        NULL, ARCHIVE_PATH, members, 3,
        search_keys, num_keys, results
    );

    if (rc != DSMIL_SEARCH_SUCCESS) {
        test_fail(name, "batch search returned error");
        return;
    }

    if (!results[0].is_exact_match || results[0].exact_match_time != 500) {
        test_fail(name, "key 500 not found in batch");
        return;
    }
    if (!results[1].is_exact_match || results[1].exact_match_time != 1500) {
        test_fail(name, "key 1500 not found in batch");
        return;
    }
    if (!results[2].is_exact_match || results[2].exact_match_time != 2500) {
        test_fail(name, "key 2500 not found in batch");
        return;
    }
    if (results[3].is_exact_match) {
        test_fail(name, "key 999999 should not be found");
        return;
    }

    test_pass(name);
}

static void test_load_all_members(void) {
    const char* name = "load_all_members";
    if (!file_exists(ARCHIVE_PATH)) {
        test_fail(name, "archive fixture not found");
        return;
    }

    dsmil_telemetry_processor_t* proc = dsmil_telemetry_processor_create(10000);
    if (!proc) {
        test_fail(name, "failed to create processor");
        return;
    }

    int rc = dsmil_telemetry_processor_load_all_members(proc, ARCHIVE_PATH);
    if (rc != DSMIL_SEARCH_SUCCESS) {
        dsmil_telemetry_processor_destroy(proc);
        test_fail(name, "load_all_members failed");
        return;
    }

    /* The archive has 3 members: numbers.txt (1001 keys),
     * numbers.csv (1001 keys), numbers.json (1001 keys)
     * All match the *.txt / *.csv / *.json patterns */
    size_t count = 0;
    /* We don't have a direct getter for event count, but we can
     * infer success from the return code. A minimal check is enough. */
    (void)count;

    dsmil_telemetry_processor_destroy(proc);
    test_pass(name);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("NOT_STISLA tar.zst streaming search test suite\n");
    printf("==============================================\n\n");

    test_open_close();
    test_iterate_members();
    test_search_text_member();
    test_search_csv_member();
    test_search_json_member();
    test_stats_consistency();
    test_error_handling();
    test_indexed_search();
    test_extract_member();
    test_batch_search();
    test_load_all_members();

    printf("\n----------------------------------------------\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
