#include "../include/keystone.h"
#include "../include/keystone_trigram.h"
#ifdef KEYSTONE_ENABLE_TAR_ZST
#include "../include/keystone_tar_zst.h"
#include "../include/dsmil_keystone_wrapper.h"
#endif
#include "test_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_trigram_extraction(void) {
    printf("Testing trigram extraction...\n");
    const char* text = "hello world";
    uint32_t trigrams[32];
    size_t count = keystone_trigram_extract(text, strlen(text), trigrams, 32);

    TEST_ASSERT(count == 9);

    const char* dup_text = "aaaaa";
    count = keystone_trigram_extract(dup_text, strlen(dup_text), trigrams, 32);
    TEST_ASSERT(count == 1);

    printf("✓ Trigram extraction verified.\n");
}

static void test_owned_snapshot_survives_caller_mutation(void) {
    printf("Testing owned document snapshot...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create(2);
    TEST_ASSERT(idx != NULL);

    char mutable_doc[64] = "classified-looking source buffer";
    TEST_ASSERT(keystone_trigram_index_add_document(
                    idx, "owned", mutable_doc, strlen(mutable_doc), NULL) == KEYSTONE_TRIGRAM_OK);

    memset(mutable_doc, 'X', strlen(mutable_doc));
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    uint32_t matches[4];
    size_t count = keystone_trigram_index_search(
        idx, "source buffer", strlen("source buffer"), matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 0);

    keystone_trigram_index_destroy(idx);
    printf("✓ Owned snapshot lifetime verified.\n");
}

static void test_external_candidate_only_mode(void) {
    printf("Testing external candidate-only mode...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create(2);
    TEST_ASSERT(idx != NULL);

    char external_doc[64] = "qihse authoritative plaintext record";
    uint32_t doc_id = UINT32_MAX;
    TEST_ASSERT(keystone_trigram_index_add_document_external(
                    idx, "external", external_doc, strlen(external_doc), &doc_id) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(doc_id == 0);
    TEST_ASSERT(keystone_trigram_index_document_count(idx) == 1);

    memset(external_doc, 0, sizeof(external_doc));
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    uint32_t candidates[4];
    size_t candidate_count = keystone_trigram_index_get_candidates(
        idx, "authoritative", strlen("authoritative"), candidates, 4);
    TEST_ASSERT(candidate_count == 1);
    TEST_ASSERT(candidates[0] == 0);

    /* Exact search deliberately cannot inspect external-only plaintext. */
    uint32_t matches[4];
    TEST_ASSERT(keystone_trigram_index_search(
                    idx, "authoritative", strlen("authoritative"), matches, 4) == 0);

    keystone_trigram_index_destroy(idx);
    printf("✓ Candidate-only plaintext separation verified.\n");
}

static void test_finalize_is_security_boundary(void) {
    printf("Testing finalize state boundary...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create(1);
    TEST_ASSERT(idx != NULL);

    const char* doc = "immutable after finalize";
    TEST_ASSERT(keystone_trigram_index_add_document(
                    idx, "doc", doc, strlen(doc), NULL) == KEYSTONE_TRIGRAM_OK);

    uint32_t candidates[2];
    TEST_ASSERT(keystone_trigram_index_get_candidates(
                    idx, "immutable", strlen("immutable"), candidates, 2) == 0);

    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(
                    idx, "late", "late mutation", strlen("late mutation"), NULL) == KEYSTONE_TRIGRAM_ESTATE);

    keystone_trigram_index_destroy(idx);
    printf("✓ Finalize state boundary verified.\n");
}

static void test_trigram_index_build_and_search(void) {
    printf("Testing trigram index build and search...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create(8);
    TEST_ASSERT(idx != NULL);

    const char* doc0 = "The quick brown fox jumps over the lazy dog";
    const char* doc1 = "KEYSTONE interpolation search engine for sorted int64";
    const char* doc2 = "Trigram index accelerates regex search across text files";
    const char* doc3 = "High-performance SIMD search and data ingestion";

    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc0.txt", doc0, strlen(doc0), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc1.txt", doc1, strlen(doc1), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc2.txt", doc2, strlen(doc2), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc3.txt", doc3, strlen(doc3), NULL) == KEYSTONE_TRIGRAM_OK);

    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_document_count(idx) == 4);

    uint32_t matches[8];
    size_t match_count = keystone_trigram_index_search(idx, "KEYSTONE", 8, matches, 8);
    TEST_ASSERT(match_count == 1);
    TEST_ASSERT(matches[0] == 1);

    match_count = keystone_trigram_index_search(idx, "search", 6, matches, 8);
    TEST_ASSERT(match_count == 3);

    match_count = keystone_trigram_index_search(idx, "nonexistent_pattern", 19, matches, 8);
    TEST_ASSERT(match_count == 0);

    keystone_trigram_stats_t stats;
    keystone_trigram_index_get_stats(idx, &stats);
    TEST_ASSERT(stats.total_documents == 4);
    TEST_ASSERT(stats.total_searches == 3);
    TEST_ASSERT(stats.candidate_docs_rejected > 0);

    keystone_trigram_index_destroy(idx);
    printf("✓ Trigram index build and search verified.\n");
}

static void test_case_insensitive_search(void) {
    printf("Testing case-insensitive trigram indexing and search...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create_options(
        4, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE
    );
    TEST_ASSERT(idx != NULL);
    TEST_ASSERT(keystone_trigram_index_get_flags(idx) == KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);

    const char* doc0 = "The Quick BROWN Fox Jumps Over The Lazy Dog";
    const char* doc1 = "KEYSTONE interpolation search engine for sorted int64";

    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc0", doc0, strlen(doc0), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc1", doc1, strlen(doc1), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    uint32_t matches[4];
    /* Search with various casings */
    size_t count = keystone_trigram_index_search(idx, "quick", 5, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 0);

    count = keystone_trigram_index_search(idx, "brown", 5, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 0);

    count = keystone_trigram_index_search(idx, "FOX", 3, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 0);

    count = keystone_trigram_index_search(idx, "keystone", 8, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 1);

    count = keystone_trigram_index_search(idx, "SEARCH", 6, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 1);

    keystone_trigram_index_destroy(idx);
    printf("✓ Case-insensitive trigram search verified.\n");
}

static void test_binary_persistence(void) {
    printf("Testing binary persistence (save / load)...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create_options(
        4, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE
    );
    TEST_ASSERT(idx != NULL);

    const char* doc0 = "Alpha Bravo Charlie Delta";
    const char* doc1 = "Echo Foxtrot Golf Hotel";
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d0.txt", doc0, strlen(doc0), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d1.txt", doc1, strlen(doc1), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    const char* tmp_file = "/tmp/test_keystone_trigram_persist.bin";
    TEST_ASSERT(keystone_trigram_index_save(idx, tmp_file) == KEYSTONE_TRIGRAM_OK);

    /* Load back into a fresh index */
    keystone_trigram_index_t* loaded = keystone_trigram_index_load(tmp_file);
    TEST_ASSERT(loaded != NULL);
    TEST_ASSERT(keystone_trigram_index_document_count(loaded) == 2);
    TEST_ASSERT(keystone_trigram_index_get_flags(loaded) == KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);

    uint32_t matches[4];
    size_t count = keystone_trigram_index_search(loaded, "bravo", 5, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 0);

    count = keystone_trigram_index_search(loaded, "FOXTROT", 7, matches, 4);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(matches[0] == 1);

    count = keystone_trigram_index_search(loaded, "Nonexistent", 11, matches, 4);
    TEST_ASSERT(count == 0);

    keystone_trigram_index_destroy(idx);
    keystone_trigram_index_destroy(loaded);
    unlink(tmp_file);
    printf("✓ Binary persistence save/load verified.\n");
}

#ifdef KEYSTONE_ENABLE_TAR_ZST
static void test_archive_streaming_trigram(void) {
    printf("Testing tar.zst archive streaming trigram indexing...\n");
    const char* archive_path = "tests/fixtures_tar_zst/test_data.tar.zst";

    keystone_tar_zst_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.format = KEYSTONE_TAR_ZST_FORMAT_TEXT;

    keystone_tar_zst_t* tz = keystone_tar_zst_open(archive_path, &opts);
    TEST_ASSERT(tz != NULL);

    keystone_trigram_index_t* idx = keystone_trigram_index_create_options(
        8, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE
    );
    TEST_ASSERT(idx != NULL);

    /* Index all members, retaining content for exact verification */
    int count = keystone_tar_zst_index_trigram(tz, idx, NULL, 1);
    TEST_ASSERT(count > 0);
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    /* Search for content from corrupt.txt ("not a number") */
    uint32_t matches[8];
    size_t match_count = keystone_trigram_index_search(idx, "not a number", strlen("not a number"), matches, 8);
    TEST_ASSERT(match_count >= 1);

    keystone_trigram_index_destroy(idx);
    keystone_tar_zst_close(tz);
    printf("✓ tar.zst archive streaming trigram indexing verified.\n");
}

static void test_dsmil_wrapper_tar_zst_trigram(void) {
    printf("Testing DSMIL wrapper tar.zst trigram indexing...\n");
    const char* archive_path = "tests/fixtures_tar_zst/test_data.tar.zst";

    keystone_trigram_index_t* idx = NULL;
    int count = dsmil_trigram_index_tar_zst(
        archive_path, "*.txt", true, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE, &idx
    );
    TEST_ASSERT(count > 0);
    TEST_ASSERT(idx != NULL);

    uint32_t matches[8];
    size_t match_count = keystone_trigram_index_search(idx, "number", 6, matches, 8);
    TEST_ASSERT(match_count >= 1);

    keystone_trigram_index_destroy(idx);
    printf("✓ DSMIL wrapper tar.zst trigram indexing verified.\n");
}
#endif

static void test_candidate_iterator_case_folding_and_galloping(void) {
    printf("Testing candidate iterator case folding and galloping search...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create_options(8, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    TEST_ASSERT(idx != NULL);

    const char* doc0 = "kernel module privilege escalation";
    const char* doc1 = "network packet manipulation and spoofing";
    const char* doc2 = "kernel memory corruption vulnerability";
    const char* doc3 = "application privilege drop failure";
    const char* doc4 = "kernel livepatch injection";

    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d0", doc0, strlen(doc0), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d1", doc1, strlen(doc1), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d2", doc2, strlen(doc2), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d3", doc3, strlen(doc3), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "d4", doc4, strlen(doc4), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    /* Test 1: Uppercase query on case-insensitive index with candidate iterator.
     * Prior to the fix, candidate iterator called unfolded extract, causing false negative. */
    keystone_trigram_candidate_iter_t* iter = keystone_trigram_candidates_begin(idx, "KERNEL", 6);
    TEST_ASSERT(iter != NULL);

    uint32_t buf[2];
    size_t count = 0;
    int exhausted = 0;

    /* Batch 1: first 2 matching docs (0 and 2) */
    TEST_ASSERT(keystone_trigram_candidates_next(iter, buf, 2, &count, &exhausted) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(count == 2);
    TEST_ASSERT(buf[0] == 0 && buf[1] == 2);
    TEST_ASSERT(exhausted == 0);

    /* Batch 2: next matching doc (4) and exhausted */
    TEST_ASSERT(keystone_trigram_candidates_next(iter, buf, 2, &count, &exhausted) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(buf[0] == 4);
    TEST_ASSERT(exhausted == 1);

    keystone_trigram_candidates_free(iter);

    /* Test 2: Non-existent term returns empty result cleanly without crashing */
    keystone_trigram_candidate_iter_t* empty_iter = keystone_trigram_candidates_begin(idx, "XYZ123NOTTHERE", 14);
    TEST_ASSERT(empty_iter != NULL);
    TEST_ASSERT(keystone_trigram_candidates_next(empty_iter, buf, 2, &count, &exhausted) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(count == 0);
    TEST_ASSERT(exhausted == 1);
    keystone_trigram_candidates_free(empty_iter);

    /* Test 3: Search with bounded allocation produces exact matches */
    uint32_t search_matches[4];
    size_t s_count = keystone_trigram_index_search(idx, "PRIVILEGE", 9, search_matches, 4);
    TEST_ASSERT(s_count == 2);
    TEST_ASSERT(search_matches[0] == 0 && search_matches[1] == 3);

    keystone_trigram_index_destroy(idx);
    printf("✓ Candidate iterator case folding and galloping search verified.\n");
}

static void test_true_streaming_carry_and_flattening_and_bitmaps(void) {
    printf("Testing Phase 2: True streaming carry, flat postings, and dense bitmaps...\n");

    /* Part 1: True streaming with 1-byte, 2-byte, and varying chunk feeds */
    keystone_trigram_index_t* idx_ref = keystone_trigram_index_create_options(2, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    keystone_trigram_index_t* idx_stream = keystone_trigram_index_create_options(2, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    TEST_ASSERT(idx_ref != NULL && idx_stream != NULL);

    const char* text0 = "The quick brown fox jumps over the lazy dog";
    size_t text0_len = strlen(text0);

    TEST_ASSERT(keystone_trigram_index_add_document_external(idx_ref, "doc0", text0, text0_len, NULL) == KEYSTONE_TRIGRAM_OK);

    keystone_trigram_stream_t* stream0 = keystone_trigram_begin_document_options(idx_stream, "doc0", false);
    TEST_ASSERT(stream0 != NULL);
    /* Feed strictly 1 byte at a time */
    for (size_t i = 0; i < text0_len; i++) {
        TEST_ASSERT(keystone_trigram_feed_bytes(stream0, &text0[i], 1) == KEYSTONE_TRIGRAM_OK);
    }
    uint32_t s_doc_id = 999;
    TEST_ASSERT(keystone_trigram_end_document(stream0, &s_doc_id) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(s_doc_id == 0);

    TEST_ASSERT(keystone_trigram_index_finalize(idx_ref) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_finalize(idx_stream) == KEYSTONE_TRIGRAM_OK);

    const char* queries[] = {"quick", "brown", "fox", "lazy", "dog", "jumps over", "THE QUICK"};
    for (size_t q = 0; q < sizeof(queries)/sizeof(queries[0]); q++) {
        uint32_t c_ref[2], c_stream[2];
        size_t n_ref = keystone_trigram_index_get_candidates(idx_ref, queries[q], strlen(queries[q]), c_ref, 2);
        size_t n_stream = keystone_trigram_index_get_candidates(idx_stream, queries[q], strlen(queries[q]), c_stream, 2);
        TEST_ASSERT(n_ref == 1);
        TEST_ASSERT(n_stream == 1);
        TEST_ASSERT(c_ref[0] == c_stream[0]);
    }

    keystone_trigram_index_destroy(idx_ref);
    keystone_trigram_index_destroy(idx_stream);

    /* Part 2: Dense Posting Bitmaps and Contiguous Flat Postings (>64 docs) */
    keystone_trigram_index_t* idx_dense = keystone_trigram_index_create_options(128, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    TEST_ASSERT(idx_dense != NULL);

    char buf[256];
    for (uint32_t d = 0; d < 128; d++) {
        if (d == 42 || d == 99) {
            snprintf(buf, sizeof(buf), "COMMON_SIGNAL_EVERYWHERE doc_%u RARE_SECRET_TOKEN payload", d);
        } else if (d % 2 == 1) {
            snprintf(buf, sizeof(buf), "COMMON_SIGNAL_EVERYWHERE doc_%u ODD_TARGET_PAYLOAD data", d);
        } else {
            snprintf(buf, sizeof(buf), "COMMON_SIGNAL_EVERYWHERE doc_%u EVEN_TARGET_PAYLOAD data", d);
        }
        TEST_ASSERT(keystone_trigram_index_add_document(idx_dense, NULL, buf, strlen(buf), NULL) == KEYSTONE_TRIGRAM_OK);
    }

    size_t pre_mem = keystone_trigram_index_memory_usage(idx_dense);
    TEST_ASSERT(pre_mem > 0);

    TEST_ASSERT(keystone_trigram_index_finalize(idx_dense) == KEYSTONE_TRIGRAM_OK);

    size_t post_mem = keystone_trigram_index_memory_usage(idx_dense);
    TEST_ASSERT(post_mem > 0);

    /* Query dense term: should return all 128 candidate docs via dense bitmap fast path */
    uint32_t all_cands[150];
    size_t total_cands = keystone_trigram_index_get_candidates(
        idx_dense, "COMMON_SIGNAL_EVERYWHERE", strlen("COMMON_SIGNAL_EVERYWHERE"), all_cands, 150);
    TEST_ASSERT(total_cands == 128);

    /* Query rarest term: should return exactly 2 candidate docs */
    uint32_t rare_cands[10];
    size_t rare_count = keystone_trigram_index_get_candidates(
        idx_dense, "RARE_SECRET_TOKEN", strlen("RARE_SECRET_TOKEN"), rare_cands, 10);
    TEST_ASSERT(rare_count == 2);
    TEST_ASSERT(rare_cands[0] == 42 && rare_cands[1] == 99);

    /* Combined query: common + rare */
    uint32_t combo_matches[10];
    size_t combo_count = keystone_trigram_index_search(
        idx_dense, "RARE_SECRET_TOKEN", strlen("RARE_SECRET_TOKEN"), combo_matches, 10);
    TEST_ASSERT(combo_count == 2);
    TEST_ASSERT(combo_matches[0] == 42 && combo_matches[1] == 99);

    /* Paginated iterator test across dense posting list */
    keystone_trigram_candidate_iter_t* p_iter = keystone_trigram_candidates_begin(
        idx_dense, "COMMON_SIGNAL_EVERYWHERE", strlen("COMMON_SIGNAL_EVERYWHERE"));
    TEST_ASSERT(p_iter != NULL);

    size_t iterated = 0;
    int exhausted = 0;
    uint32_t chunk[32];
    while (!exhausted) {
        size_t c = 0;
        TEST_ASSERT(keystone_trigram_candidates_next(p_iter, chunk, 32, &c, &exhausted) == KEYSTONE_TRIGRAM_OK);
        iterated += c;
    }
    TEST_ASSERT(iterated == 128);
    keystone_trigram_candidates_free(p_iter);

    keystone_trigram_index_destroy(idx_dense);
    printf("✓ Phase 2 streaming carry, flat postings, and dense bitmaps verified.\n");
}

static void test_direct_24bit_directory(void) {
    printf("Testing Direct 24-bit directory acceleration...\n");
    keystone_trigram_index_t* idx = keystone_trigram_index_create_options(
        4, KEYSTONE_TRIGRAM_OPT_DIRECT_DIRECTORY | KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    TEST_ASSERT(idx != NULL);

    const char* doc0 = "ALPHA BRAVO CHARLIE DELTA";
    const char* doc1 = "BRAVO CHARLIE ECHO FOXTROT";
    const char* doc2 = "GOLF HOTEL INDIA JULIETT";

    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc0", doc0, strlen(doc0), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc1", doc1, strlen(doc1), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_add_document(idx, "doc2", doc2, strlen(doc2), NULL) == KEYSTONE_TRIGRAM_OK);
    TEST_ASSERT(keystone_trigram_index_finalize(idx) == KEYSTONE_TRIGRAM_OK);

    /* Memory usage should reflect the 64 MiB direct directory table */
    size_t mem = keystone_trigram_index_memory_usage(idx);
    TEST_ASSERT(mem >= 16777216u * sizeof(uint32_t));

    /* Direct zero-probe candidate query */
    uint32_t cands[10];
    size_t n = keystone_trigram_index_get_candidates(idx, "CHARLIE", 7, cands, 10);
    TEST_ASSERT(n == 2);
    TEST_ASSERT(cands[0] == 0 && cands[1] == 1);

    /* Direct zero-probe search */
    uint32_t matches[10];
    size_t m = keystone_trigram_index_search(idx, "JULIETT", 7, matches, 10);
    TEST_ASSERT(m == 1);
    TEST_ASSERT(matches[0] == 2);

    /* Non-existent trigram query */
    size_t none = keystone_trigram_index_get_candidates(idx, "ZZZ_NOT_HERE_999", 16, cands, 10);
    TEST_ASSERT(none == 0);

    keystone_trigram_index_destroy(idx);
    printf("✓ Direct 24-bit directory acceleration verified.\n");
}

int main(void) {
    printf("Running Trigram Index Test Suite\n");
    printf("===============================\n\n");

    test_trigram_extraction();
    test_owned_snapshot_survives_caller_mutation();
    test_external_candidate_only_mode();
    test_finalize_is_security_boundary();
    test_trigram_index_build_and_search();
    test_case_insensitive_search();
    test_candidate_iterator_case_folding_and_galloping();
    test_true_streaming_carry_and_flattening_and_bitmaps();
    test_direct_24bit_directory();
    test_binary_persistence();
#ifdef KEYSTONE_ENABLE_TAR_ZST
    test_archive_streaming_trigram();
    test_dsmil_wrapper_tar_zst_trigram();
#endif

    printf("\nAll Trigram Index tests passed.\n");
    return 0;
}
