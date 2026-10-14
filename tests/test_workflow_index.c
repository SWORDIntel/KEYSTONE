/*
 * KEYSTONE Workflow Candidate Index Tests (native index + keystoned IPC)
 *
 * Covers the metadata-only workflow projection: fail-closed binding, batch
 * interval validation, gap/duplicate/conflict rejection, ascending and
 * descending pages with cursor continuation and watermark rejection,
 * classification/SCI filtering, tombstones, staged replay with atomic
 * publication and per-connection ownership, sealed checkpoints, and the
 * daemon's SO_PEERCRED workflow authorization.
 *
 * SRSL-1.0 License.
 */

#include "keystone.h"
#include "keystoned.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WF_TYPE_A 2u
#define WF_TYPE_B 3u
#define TENANT 7u

static keystone_uuid_t uuid_from_tag(char tag, int seq) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%02x000000-0000-0000-0000-%012d", (unsigned)(tag & 0xff), seq);
    keystone_uuid_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(keystone_uuid_from_string(buf, &out) == 0);
    return out;
}

static keystone_workflow_event_t make_event(uint64_t seq, keystone_uuid_t record,
                                            keystone_uuid_t owner, uint32_t type,
                                            uint64_t due, uint32_t classification,
                                            uint32_t sci, uint32_t tombstone) {
    keystone_workflow_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.committed_seq = seq;
    ev.entry.record_id = record;
    ev.entry.owner_id = owner;
    ev.entry.workflow_type = type;
    ev.entry.tenant = TENANT;
    ev.entry.classification = classification;
    ev.entry.sci = sci;
    ev.entry.due_at = due;
    ev.tombstone = tombstone;
    return ev;
}

/* --- Part A: native workflow index ------------------------------------- */

static void test_native_lifecycle_and_fail_closed(void) {
    printf("[*] Native: lifecycle, history binding, fail-closed reads...\n");

    TEST_ASSERT(keystone_workflow_index_create(1) == NULL);
    TEST_ASSERT(keystone_workflow_index_create(1000) == NULL);

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 0);
    TEST_ASSERT(!keystone_workflow_index_history_verified(ix));

    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = uuid_from_tag(0xa0, 0);
    q.limit = 8;
    q.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_STALE);

    keystone_workflow_event_t ev = make_event(1, uuid_from_tag(1, 1),
                                              uuid_from_tag(0xa0, 0), WF_TYPE_A,
                                              100, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev)
                == KEYSTONE_WORKFLOW_STALE);

    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x11, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_history_verified(ix));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);
    identity[0] ^= 0xff;
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_CONFLICT);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Lifecycle, binding, and fail-closed reads verified.\n");
}

static void test_native_batch_validation(void) {
    printf("[*] Native: batch interval and event validation...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x22, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_uuid_t rec = uuid_from_tag(1, 1);

    /* Degenerate interval. */
    keystone_workflow_event_t ev = make_event(5, rec, owner, WF_TYPE_A, 10, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 5, 5, &ev, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 5, &ev, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* Event outside the claimed interval. */
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 4, &ev, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 9, &ev, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* Unordered events inside one batch. */
    keystone_workflow_event_t two[2];
    two[0] = make_event(8, uuid_from_tag(1, 2), owner, WF_TYPE_A, 10, 0, 0, 0);
    two[1] = make_event(7, uuid_from_tag(1, 3), owner, WF_TYPE_A, 10, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 8, two, 2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* Nil record id and zero workflow type are rejected. */
    keystone_workflow_event_t bad = make_event(7, two[1].entry.record_id, owner,
                                               WF_TYPE_A, 10, 0, 0, 0);
    memset(&bad.entry.record_id, 0, sizeof(bad.entry.record_id));
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 7, &bad, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    bad = make_event(7, two[1].entry.record_id, owner, 0, 10, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 7, &bad, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* Same record twice at one committed sequence is a conflict. */
    two[0] = make_event(7, uuid_from_tag(1, 4), owner, WF_TYPE_A, 10, 0, 0, 0);
    two[1] = make_event(7, two[0].entry.record_id, owner, WF_TYPE_A, 20, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 7, two, 2)
                == KEYSTONE_WORKFLOW_CONFLICT);

    /* Batch bound: KEYSTONE_WORKFLOW_BATCH_MAX + 1 events rejected. */
    keystone_workflow_event_t *big =
        calloc(KEYSTONE_WORKFLOW_BATCH_MAX + 1, sizeof(*big));
    TEST_ASSERT(big != NULL);
    for (uint32_t i = 0; i <= KEYSTONE_WORKFLOW_BATCH_MAX; ++i)
        big[i] = make_event(10 + (uint64_t)i, uuid_from_tag(2, (int)i), owner,
                            WF_TYPE_A, 10 + i, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(
                    ix, 9, 10 + KEYSTONE_WORKFLOW_BATCH_MAX, big,
                    KEYSTONE_WORKFLOW_BATCH_MAX + 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    free(big);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Batch interval and event validation verified.\n");
}

static void test_native_apply_gap_duplicate(void) {
    printf("[*] Native: apply, sparse intervals, gap and duplicate rejection...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x33, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_workflow_event_t ev = make_event(10, uuid_from_tag(1, 1), owner,
                                              WF_TYPE_A, 100, 0, 0, 0);

    /* Gap: from_exclusive must equal the current watermark. */
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 9, 10, &ev, 1)
                == KEYSTONE_WORKFLOW_GAP);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 10, &ev, 1)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 10);

    /* Zero-event batches advance past commits with no candidate changes. */
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 10, 12, NULL, 0)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 12);

    /* Duplicate coverage of an already-applied interval. */
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 10, 12, NULL, 0)
                == KEYSTONE_WORKFLOW_DUPLICATE);
    /* An interval that overlaps but does not start at the watermark is a
     * gap: from_exclusive must equal the current watermark exactly. */
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 11, 20, NULL, 0)
                == KEYSTONE_WORKFLOW_GAP);

    /* Sparse interior transaction indexes are legal inside one interval. */
    keystone_workflow_event_t sparse = make_event(40, uuid_from_tag(1, 2),
                                                  owner, WF_TYPE_A, 200, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 12, 40, &sparse, 1)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 40);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 39, 40, &sparse, 1)
                == KEYSTONE_WORKFLOW_DUPLICATE);

    /* The single-event convenience demands exactly watermark+1. */
    keystone_workflow_event_t jump = make_event(45, uuid_from_tag(1, 3),
                                                owner, WF_TYPE_A, 300, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &jump)
                == KEYSTONE_WORKFLOW_GAP);
    keystone_workflow_event_t next = make_event(41, uuid_from_tag(1, 4),
                                                owner, WF_TYPE_A, 250, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &next)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 41);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &next)
                == KEYSTONE_WORKFLOW_DUPLICATE);

    /* A batch that does not start at the watermark is a gap. */
    ev.committed_seq = 45;
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 43, 45, &ev, 1)
                == KEYSTONE_WORKFLOW_GAP);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Apply, sparse intervals, gap/duplicate rejection verified.\n");
}

static void test_native_pages_ascending_descending(void) {
    printf("[*] Native: ascending/descending pages, cursors, watermarks...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x44, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_uuid_t other_owner = uuid_from_tag(0xb0, 0);

    /* Owner A: five records across two workflow types; ties on due_at. */
    keystone_workflow_event_t ev[7];
    ev[0] = make_event(1, uuid_from_tag(3, 1), owner, WF_TYPE_A, 500, 0, 0, 0);
    ev[1] = make_event(2, uuid_from_tag(3, 2), owner, WF_TYPE_A, 200, 0, 0, 0);
    ev[2] = make_event(3, uuid_from_tag(3, 3), owner, WF_TYPE_A, 800, 0, 0, 0);
    ev[3] = make_event(4, uuid_from_tag(3, 4), owner, WF_TYPE_A, 500, 0, 0, 0);
    ev[4] = make_event(5, uuid_from_tag(3, 5), owner, WF_TYPE_A, 100, 0, 0, 0);
    /* Same due_at as ev[3]/ev[4] but a different type partition. */
    ev[5] = make_event(6, uuid_from_tag(4, 1), owner, WF_TYPE_B, 500, 0, 0, 0);
    /* Different owner: never visible to owner A queries. */
    ev[6] = make_event(7, uuid_from_tag(5, 1), other_owner, WF_TYPE_A, 50, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 7, ev, 7)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 7);

    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;

    /* Whole partition ascending: due order, record-id tie-break. */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 5);
    TEST_ASSERT(!page.has_more);
    TEST_ASSERT(page.watermark == 7);
    uint64_t expect_due[5] = {100, 200, 500, 500, 800};
    for (int i = 0; i < 5; ++i)
        TEST_ASSERT(out[i].due_at == expect_due[i]);
    TEST_ASSERT(keystone_uuid_compare(&out[2].record_id, &out[3].record_id) < 0);

    /* Bounded limit with continuation, exact watermark required. */
    q.limit = 2;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2 && page.has_more && page.next_cursor_valid);
    TEST_ASSERT(out[0].due_at == 100 && out[1].due_at == 200);

    keystone_uuid_t cont = page.next_cursor;
    uint64_t cont_due = page.next_cursor_due_at;
    q.cursor = cont;
    q.cursor_due_at = cont_due;
    q.cursor_valid = 1;
    q.expected_watermark = page.watermark;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2 && page.has_more);
    TEST_ASSERT(out[0].due_at == 500 && out[1].due_at == 500);

    q.cursor = page.next_cursor;
    q.cursor_due_at = page.next_cursor_due_at;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1 && !page.has_more && out[0].due_at == 800);

    /* A follow-up cursor without a watermark is malformed. */
    q.expected_watermark = 0;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* Changed projection watermark rejects the page. */
    q.expected_watermark = 6;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_STALE);
    q.expected_watermark = 8; /* also wrong: must match exactly */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_STALE);

    /* required_seq above the committed watermark fails closed. */
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    q.required_seq = 8;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_STALE);
    q.required_seq = 7;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);

    /* Inclusive max_due_at bound, ascending. */
    q.limit = 8;
    q.max_due_at = 200;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2);
    TEST_ASSERT(out[0].due_at == 100 && out[1].due_at == 200);

    /* Descending first page: newest-first within the bound. */
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    q.descending = 1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 5 && !page.has_more);
    TEST_ASSERT(out[0].due_at == 800 && out[4].due_at == 100);
    TEST_ASSERT(keystone_uuid_compare(&out[1].record_id, &out[2].record_id) > 0);

    /* Descending continuation collects the remaining older half. */
    q.limit = 2;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2 && page.has_more);
    TEST_ASSERT(out[0].due_at == 800 && out[1].due_at == 500);
    q.cursor = page.next_cursor;
    q.cursor_due_at = page.next_cursor_due_at;
    q.cursor_valid = 1;
    q.expected_watermark = page.watermark;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2 && page.has_more);
    TEST_ASSERT(out[0].due_at == 500 && out[1].due_at == 200);
    q.cursor = page.next_cursor;
    q.cursor_due_at = page.next_cursor_due_at;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1 && !page.has_more && out[0].due_at == 100);

    /* Descending with an inclusive max_due_at starts at that deadline. */
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = 500;
    q.limit = 8;
    q.descending = 1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 4);
    TEST_ASSERT(out[0].due_at == 500 && out[3].due_at == 100);

    /* Workflow-type partition isolation. */
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_B;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1 && out[0].due_at == 500);

    /* Tenant isolation: a different tenant sees nothing. */
    q.tenant = TENANT + 1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 0 && !page.has_more);

    /* Owner isolation: exact opaque owner only. */
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = other_owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1 && out[0].due_at == 50);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Ascending/descending pages, cursors, watermark rejection verified.\n");
}

static void test_native_security_filtering(void) {
    printf("[*] Native: classification, SCI, and SENSITIVE filtering...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x55, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_workflow_event_t ev[4];
    ev[0] = make_event(1, uuid_from_tag(6, 1), owner, WF_TYPE_A, 10,
                       KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0);
    ev[1] = make_event(2, uuid_from_tag(6, 2), owner, WF_TYPE_A, 20,
                       KEYSTONE_CLASSIFICATION_SECRET, 0x3, 0);
    ev[2] = make_event(3, uuid_from_tag(6, 3), owner, WF_TYPE_A, 30,
                       KEYSTONE_CLASSIFICATION_SECRET, 0x1, 0);
    ev[2].entry.flags = KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE;
    ev[3] = make_event(4, uuid_from_tag(6, 4), owner, WF_TYPE_A, 40,
                       KEYSTONE_CLASSIFICATION_TOP_SECRET, 0x1, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 4, ev, 4)
                == KEYSTONE_WORKFLOW_OK);

    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;

    /* SECRET reader with both compartments: misses only the TOP_SECRET row. */
    q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    q.sci = 0x3;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 3);
    TEST_ASSERT(out[0].due_at == 10 && out[1].due_at == 20 && out[2].due_at == 30);

    /* Missing compartment 0x2 hides the SCI=0x3 row. */
    q.sci = 0x1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2);
    TEST_ASSERT(out[0].due_at == 10 && out[1].due_at == 30);

    /* SECURITY_SENSITIVE needs SECRET or above regardless of SCI. */
    q.max_classification = KEYSTONE_CLASSIFICATION_OPS;
    q.sci = 0x3;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1 && out[0].due_at == 10);

    /* Full authority sees everything. */
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0x3;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 4);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Classification, SCI, and SENSITIVE filtering verified.\n");
}

static void test_native_tombstone_and_reorder(void) {
    printf("[*] Native: tombstones and deadline reordering...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x66, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_uuid_t doomed = uuid_from_tag(7, 1);
    keystone_uuid_t moved = uuid_from_tag(7, 2);
    keystone_workflow_event_t ev[3];
    ev[0] = make_event(1, doomed, owner, WF_TYPE_A, 100, 0, 0, 0);
    ev[1] = make_event(2, moved, owner, WF_TYPE_A, 900, 0, 0, 0);
    ev[2] = make_event(3, uuid_from_tag(7, 3), owner, WF_TYPE_A, 300, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 3, ev, 3)
                == KEYSTONE_WORKFLOW_OK);

    /* Delete the doomed record and move the other's deadline. */
    keystone_workflow_event_t upd[2];
    upd[0] = make_event(4, doomed, owner, WF_TYPE_A, 0, 0, 0, 1);
    upd[1] = make_event(5, moved, owner, WF_TYPE_A, 50, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 3, 5, upd, 2)
                == KEYSTONE_WORKFLOW_OK);

    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    q.descending = 1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2);
    TEST_ASSERT(out[0].due_at == 300 && out[1].due_at == 50);
    TEST_ASSERT(keystone_uuid_equal(&out[1].record_id, &moved));

    /* A tombstone for an unknown record is a harmless no-op. */
    keystone_workflow_event_t ghost =
        make_event(6, uuid_from_tag(7, 99), owner, WF_TYPE_A, 0, 0, 0, 1);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ghost)
                == KEYSTONE_WORKFLOW_OK);

    /* A live record id cannot be moved to another owner's partition. */
    keystone_uuid_t stranger = uuid_from_tag(0xb0, 0);
    keystone_workflow_event_t clash =
        make_event(7, moved, stranger, WF_TYPE_A, 10, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &clash)
                == KEYSTONE_WORKFLOW_CONFLICT);

    /* After a tombstone the id is gone from the live set: uniqueness is
     * enforced against live entries, so the id may be re-created, now owned
     * by the new partition. */
    keystone_workflow_event_t reborn =
        make_event(7, doomed, stranger, WF_TYPE_A, 70, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &reborn)
                == KEYSTONE_WORKFLOW_OK);
    q.owner_id = stranger;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1 && out[0].due_at == 70);
    q.owner_id = owner;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Tombstones and deadline reordering verified.\n");
}

static void test_native_staged_replay(void) {
    printf("[*] Native: staged replay, atomic publish, failed-chunk undo...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x77, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_workflow_event_t first[2];
    first[0] = make_event(1, uuid_from_tag(8, 1), owner, WF_TYPE_A, 100, 0, 0, 0);
    first[1] = make_event(2, uuid_from_tag(8, 2), owner, WF_TYPE_A, 200, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 2, first, 2)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 2);

    /* Stage two chunks beyond the published watermark. */
    keystone_workflow_event_t chunk[2];
    chunk[0] = make_event(3, uuid_from_tag(8, 3), owner, WF_TYPE_A, 300, 0, 0, 0);
    chunk[1] = make_event(4, uuid_from_tag(8, 4), owner, WF_TYPE_A, 400, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 2, 4, chunk, 2)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 4);

    keystone_workflow_event_t chunk2[2];
    chunk2[0] = make_event(5, uuid_from_tag(8, 5), owner, WF_TYPE_A, 500, 0, 0, 0);
    chunk2[1] = make_event(6, uuid_from_tag(8, 6), owner, WF_TYPE_A, 600, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 4, 6, chunk2, 2)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 6);

    /* Readers and the published watermark still see the old complete index. */
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 2);
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2 && page.watermark == 2);

    /* Ordinary ingest is rejected while a stage is active. The convenience
     * form must sit at watermark+1 so the stage conflict is what fires. */
    keystone_workflow_event_t direct =
        make_event(3, uuid_from_tag(8, 7), owner, WF_TYPE_A, 700, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &direct)
                == KEYSTONE_WORKFLOW_CONFLICT);

    /* A conflicting chunk must leave the prior stage fully intact. */
    keystone_workflow_event_t badchunk[2];
    badchunk[0] = make_event(7, uuid_from_tag(8, 8), owner, WF_TYPE_A, 800, 0, 0, 0);
    badchunk[1] = make_event(8, uuid_from_tag(8, 1), uuid_from_tag(0xb0, 0),
                             WF_TYPE_A, 900, 0, 0, 0); /* id reuse, new owner */
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 6, 8, badchunk, 2)
                == KEYSTONE_WORKFLOW_CONFLICT);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 6);

    /* Wrong publish target is rejected; correct one publishes atomically. */
    TEST_ASSERT(keystone_workflow_index_publish(ix, 7)
                == KEYSTONE_WORKFLOW_CONFLICT);
    TEST_ASSERT(keystone_workflow_index_publish(ix, 6)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 6);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 0);

    /* Published view now has the staged rows and none of the undone chunk. */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 6 && page.watermark == 6);
    TEST_ASSERT(out[5].due_at == 600);

    /* The undone row can be staged cleanly afterwards. */
    keystone_workflow_event_t fixup[2];
    fixup[0] = make_event(7, uuid_from_tag(8, 8), owner, WF_TYPE_A, 800, 0, 0, 0);
    fixup[1] = make_event(8, uuid_from_tag(8, 1), owner, WF_TYPE_A, 90, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 6, 8, fixup, 2)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_publish(ix, 8)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 8);
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    /* Six staged rows plus the new 8,8; the re-sent 8,1 is an update, so
     * seven live entries with 8,1 moved to its new deadline position. */
    TEST_ASSERT(page.count == 7);
    keystone_uuid_t moved_id = uuid_from_tag(8, 1);
    TEST_ASSERT(out[0].due_at == 90);
    TEST_ASSERT(keystone_uuid_equal(&out[0].record_id, &moved_id));

    /* Abort discards a stage without publishing. */
    keystone_workflow_event_t discard =
        make_event(9, uuid_from_tag(8, 9), owner, WF_TYPE_A, 990, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 8, 9, &discard, 1)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_abort_stage(ix)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 0);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 8);
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 7);

    /* Publishing with no stage is an error. */
    TEST_ASSERT(keystone_workflow_index_publish(ix, 9)
                == KEYSTONE_WORKFLOW_CONFLICT);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Staged replay, atomic publish, failed-chunk undo verified.\n");
}

static void test_native_checkpoint_sealed(void) {
    printf("[*] Native: versioned checkpoint save/load stays sealed...\n");

    const char *path = "/tmp/keystone_test_workflow.ckpt";
    unlink(path);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x88, sizeof(identity));

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    /* Saving before the history is bound fails closed. */
    TEST_ASSERT(keystone_workflow_index_save(ix, path)
                == KEYSTONE_WORKFLOW_STALE);
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity)
                == KEYSTONE_WORKFLOW_OK);
    keystone_workflow_event_t ev = make_event(1, uuid_from_tag(9, 1), owner,
                                              WF_TYPE_A, 100, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_save(ix, path)
                == KEYSTONE_WORKFLOW_OK);
    keystone_workflow_index_destroy(ix);

    keystone_workflow_index_t *loaded = NULL;
    TEST_ASSERT(keystone_workflow_index_load(path, &loaded)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(loaded != NULL);
    /* Sealed for offline inspection: no live reads, no unsealing bind. */
    TEST_ASSERT(!keystone_workflow_index_history_verified(loaded));
    TEST_ASSERT(keystone_workflow_index_watermark(loaded) == 0);
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystone_workflow_index_page(loaded, &q, out, 8, &page)
                == KEYSTONE_WORKFLOW_STALE);
    TEST_ASSERT(keystone_workflow_index_bind_history(loaded, identity)
                == KEYSTONE_WORKFLOW_STALE);
    keystone_workflow_index_destroy(loaded);

    /* Corrupted payload byte is rejected. */
    FILE *f = fopen(path, "r+b");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fseek(f, 128, SEEK_SET) == 0);
    int ch = fgetc(f);
    TEST_ASSERT(fseek(f, 128, SEEK_SET) == 0);
    TEST_ASSERT(fputc(ch ^ 0x40, f) != EOF);
    TEST_ASSERT(fclose(f) == 0);
    loaded = NULL;
    TEST_ASSERT(keystone_workflow_index_load(path, &loaded)
                == KEYSTONE_WORKFLOW_CORRUPT);
    TEST_ASSERT(loaded == NULL);

    /* Truncated file is rejected. */
    TEST_ASSERT(truncate(path, 64) == 0);
    TEST_ASSERT(keystone_workflow_index_load(path, &loaded)
                == KEYSTONE_WORKFLOW_CORRUPT);
    TEST_ASSERT(loaded == NULL);
    unlink(path);

    printf("    [+] Sealed checkpoint save/load and corruption rejection verified.\n");
}

/* --- Part B: keystoned workflow IPC ------------------------------------ */

static void run_daemon(const char *sock_path, keystoned_server_t **out_server) {
    unlink(sock_path);
    keystoned_config_t cfg = {
        .socket_path = sock_path,
        .cache_capacity = 64,
        .initial_fencing_epoch = 1,
        .enable_security_audit = 1,
    };
    keystoned_server_t *server = keystoned_server_create(&cfg);
    TEST_ASSERT(server != NULL);
    TEST_ASSERT(keystoned_server_start(server) == 0);
    usleep(20000);
    *out_server = server;
}

static void test_ipc_requires_registered_principal(void) {
    printf("[*] IPC: workflow endpoints refuse client-asserted principals...\n");

    const char *sock = "/tmp/keystone_test_wf_unbound.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    keystoned_client_t *client = keystoned_client_connect(sock);
    TEST_ASSERT(client != NULL);

    keystone_security_context_t ctx = {
        .tenant_id = TENANT,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    uint64_t watermark = 12345;
    TEST_ASSERT(keystoned_client_workflow_watermark(client, &ctx, &watermark)
                == KEYSTONED_STATUS_DENIED);
    /* A denied call leaves the caller's watermark untouched. */
    TEST_ASSERT(watermark == 12345);
    watermark = 0;

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_workflow_event_t ev = make_event(1, uuid_from_tag(1, 1), owner,
                                              WF_TYPE_A, 10, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(client, &ctx, 0, 1, &ev, 1,
                                                &watermark)
                == KEYSTONED_STATUS_DENIED);

    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q.sci = 0xFFFFFFFFu;
    q.max_due_at = UINT64_MAX;
    q.limit = 4;
    keystone_workflow_entry_t out[4];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystoned_client_workflow_page(client, &ctx, &q, out, 4, &page)
                == KEYSTONED_STATUS_DENIED);

    keystoned_client_disconnect(client);
    keystoned_server_stop(server);
    keystoned_server_destroy(server);
    unlink(sock);
    printf("    [+] Unregistered workflow principals denied on every endpoint.\n");
}

static void test_ipc_projection_and_paging(void) {
    printf("[*] IPC: trusted projector apply, staged publish, scoped reads...\n");

    const char *sock = "/tmp/keystone_test_wf_bound.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    /* Register the dedicated projector principal for this daemon's own uid. */
    keystone_security_context_t op_ctx = {
        .tenant_id = TENANT,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx) == 0);

    keystoned_client_t *proj = keystoned_client_connect(sock);
    TEST_ASSERT(proj != NULL);

    uint64_t watermark = 0;
    /* Everything fails closed until the history identity is bound. */
    TEST_ASSERT(keystoned_client_workflow_watermark(proj, &op_ctx, &watermark)
                == KEYSTONED_STATUS_STALE_GEN);
    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_workflow_event_t ev = make_event(1, uuid_from_tag(1, 1), owner,
                                              WF_TYPE_A, 10, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 0, 1, &ev, 1,
                                                &watermark)
                == KEYSTONED_STATUS_STALE_GEN);

    /* A weaker principal cannot bind the history identity. */
    keystone_security_context_t weak_ctx = {
        .tenant_id = TENANT,
        .classification = KEYSTONE_CLASSIFICATION_SECRET,
        .compartment_mask = 0x3,
    };
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x99, sizeof(identity));
    TEST_ASSERT(keystoned_client_workflow_bind_history(proj, &weak_ctx,
                                                       identity, &watermark)
                == KEYSTONED_STATUS_DENIED);
    TEST_ASSERT(keystoned_client_workflow_bind_history(proj, &op_ctx,
                                                       identity, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 0);

    /* Events for a different tenant are refused. */
    keystone_workflow_event_t alien = make_event(1, uuid_from_tag(1, 2), owner,
                                                 WF_TYPE_A, 10, 0, 0, 0);
    alien.entry.tenant = TENANT + 1;
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 0, 1, &alien, 1,
                                                &watermark)
                == KEYSTONED_STATUS_DENIED);

    /* Classified rows the projector itself is authorized to relabel. */
    keystone_workflow_event_t batch[4];
    batch[0] = make_event(1, uuid_from_tag(1, 1), owner, WF_TYPE_A, 100,
                          KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0);
    batch[1] = make_event(2, uuid_from_tag(1, 2), owner, WF_TYPE_A, 200,
                          KEYSTONE_CLASSIFICATION_SECRET, 0x3, 0);
    batch[2] = make_event(3, uuid_from_tag(1, 3), owner, WF_TYPE_A, 300,
                          KEYSTONE_CLASSIFICATION_SECRET, 0x1, 0);
    batch[2].entry.flags = KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE;
    batch[3] = make_event(4, uuid_from_tag(1, 4), owner, WF_TYPE_A, 400,
                          KEYSTONE_CLASSIFICATION_TOP_SECRET, 0x1, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 0, 4, batch, 4,
                                                &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 4);
    TEST_ASSERT(keystoned_client_workflow_watermark(proj, &op_ctx, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 4);

    /* A lower-clearance reader on the same registered uid: effective context
     * is downgraded to the asserted SECRET/0x3 view. */
    keystoned_client_t *reader = keystoned_client_connect(sock);
    TEST_ASSERT(reader != NULL);
    TEST_ASSERT(keystoned_client_bind_principal(reader, &weak_ctx) == 0);

    /* The reader cannot mutate the index. */
    keystone_workflow_event_t mut = make_event(5, uuid_from_tag(1, 5), owner,
                                               WF_TYPE_A, 500, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(reader, &weak_ctx, 4, 5, &mut, 1,
                                                &watermark)
                == KEYSTONED_STATUS_DENIED);

    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TYPE_A;
    q.tenant = TENANT;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    q.sci = 0x3;
    q.max_due_at = UINT64_MAX;
    q.limit = 8;
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &weak_ctx, &q, out, 8,
                                               &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 3 && page.watermark == 4);
    TEST_ASSERT(out[0].due_at == 100 && out[1].due_at == 200 && out[2].due_at == 300);

    /* Querying above the caller's own clearance is denied. */
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &weak_ctx, &q, out, 8,
                                               &page)
                == KEYSTONED_STATUS_DENIED);

    /* Cross-tenant queries are denied. */
    q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    q.tenant = TENANT + 1;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &weak_ctx, &q, out, 8,
                                               &page)
                == KEYSTONED_STATUS_DENIED);

    /* Staged publish through the socket: exclusive connection ownership. */
    uint64_t stage_wm = 0;
    keystone_workflow_event_t chunk[2];
    chunk[0] = make_event(5, uuid_from_tag(1, 5), owner, WF_TYPE_A, 500, 0, 0, 0);
    chunk[1] = make_event(6, uuid_from_tag(1, 6), owner, WF_TYPE_A, 600, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_stage(proj, &op_ctx, 4, 6, chunk, 2,
                                                &stage_wm)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(stage_wm == 6);

    /* The reader still sees the published watermark. */
    TEST_ASSERT(keystoned_client_workflow_watermark(reader, &weak_ctx, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 4);

    /* Publish with a wrong target is rejected. */
    TEST_ASSERT(keystoned_client_workflow_publish(proj, &op_ctx, 5, &watermark)
                == KEYSTONED_STATUS_CONFLICT);
    TEST_ASSERT(keystoned_client_workflow_publish(proj, &op_ctx, 6, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 6);

    q.tenant = TENANT;
    q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    q.sci = 0x3;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &weak_ctx, &q, out, 8,
                                               &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 5 && page.watermark == 6);

    keystoned_client_disconnect(reader);
    keystoned_client_disconnect(proj);
    keystoned_server_stop(server);
    keystoned_server_destroy(server);
    unlink(sock);
    printf("    [+] Trusted projector, scoped reads, staged publish verified.\n");
}

static void test_ipc_stage_ownership(void) {
    printf("[*] IPC: stage ownership, cross-connection rejection, disconnect abort...\n");

    const char *sock = "/tmp/keystone_test_wf_stage.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    keystone_security_context_t op_ctx = {
        .tenant_id = TENANT,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx) == 0);

    keystoned_client_t *a = keystoned_client_connect(sock);
    keystoned_client_t *b = keystoned_client_connect(sock);
    TEST_ASSERT(a != NULL && b != NULL);

    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0xaa, sizeof(identity));
    uint64_t watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_bind_history(a, &op_ctx, identity,
                                                       &watermark)
                == KEYSTONED_STATUS_OK);

    keystone_uuid_t owner = uuid_from_tag(0xa0, 0);
    keystone_workflow_event_t ev = make_event(1, uuid_from_tag(1, 1), owner,
                                              WF_TYPE_A, 10, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(a, &op_ctx, 0, 1, &ev, 1,
                                                &watermark)
                == KEYSTONED_STATUS_OK);

    /* A holds a stage. */
    uint64_t stage_wm = 0;
    keystone_workflow_event_t chunk[1];
    chunk[0] = make_event(2, uuid_from_tag(1, 2), owner, WF_TYPE_A, 20, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_stage(a, &op_ctx, 1, 2, chunk, 1,
                                                &stage_wm)
                == KEYSTONED_STATUS_OK);

    /* B cannot stage, apply, publish, or even abort another owner's stage. */
    keystone_workflow_event_t other[1];
    other[0] = make_event(3, uuid_from_tag(1, 3), owner, WF_TYPE_A, 30, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_stage(b, &op_ctx, 2, 3, other, 1,
                                                &stage_wm)
                == KEYSTONED_STATUS_CONFLICT);
    TEST_ASSERT(keystoned_client_workflow_apply(b, &op_ctx, 1, 3, other, 1,
                                                &watermark)
                == KEYSTONED_STATUS_CONFLICT);
    TEST_ASSERT(keystoned_client_workflow_publish(b, &op_ctx, 3, &watermark)
                == KEYSTONED_STATUS_CONFLICT);
    TEST_ASSERT(keystoned_client_workflow_abort_stage(b, &op_ctx, &watermark)
                == KEYSTONED_STATUS_CONFLICT);

    /* A aborts its own stage; B can then apply again. */
    TEST_ASSERT(keystoned_client_workflow_abort_stage(a, &op_ctx, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 1);
    TEST_ASSERT(keystoned_client_workflow_apply(b, &op_ctx, 1, 3, other, 1,
                                                &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 3);

    /* A stages again, then drops the connection: the volatile stage dies. */
    keystone_workflow_event_t more[1];
    more[0] = make_event(4, uuid_from_tag(1, 4), owner, WF_TYPE_A, 40, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_stage(a, &op_ctx, 3, 4, more, 1,
                                                &stage_wm)
                == KEYSTONED_STATUS_OK);
    keystoned_client_disconnect(a);

    /* Give the server a moment to reap the disconnected owner. */
    usleep(100000);

    keystone_workflow_event_t next[1];
    next[0] = make_event(5, uuid_from_tag(1, 5), owner, WF_TYPE_A, 50, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(b, &op_ctx, 3, 5, next, 1,
                                                &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 5);

    keystoned_client_disconnect(b);
    keystoned_server_stop(server);
    keystoned_server_destroy(server);
    unlink(sock);
    printf("    [+] Stage ownership and disconnect abort verified.\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Workflow Candidate Index Tests\n");
    printf("=================================================================\n");

    test_native_lifecycle_and_fail_closed();
    test_native_batch_validation();
    test_native_apply_gap_duplicate();
    test_native_pages_ascending_descending();
    test_native_security_filtering();
    test_native_tombstone_and_reorder();
    test_native_staged_replay();
    test_native_checkpoint_sealed();

    test_ipc_requires_registered_principal();
    test_ipc_projection_and_paging();
    test_ipc_stage_ownership();

    printf("=================================================================\n");
    printf("  ALL WORKFLOW CANDIDATE INDEX TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
