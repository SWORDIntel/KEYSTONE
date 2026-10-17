/**
 * SPDX-License-Identifier: LicenseRef-SRSL-1.1
 * Copyright (C) 2026 SWORDIntel. All rights reserved.
 *
 * tests/test_workflow_adversarial_gates.c
 *
 * Gate 4 Workflow Candidate Launch Gates Verification Suite:
 * "Verify gap, stale cursor, delete, failover, cross-tenant/SCI denial,
 *  and latency behavior before deployment."
 *
 * Sections:
 *   1. Gap & Out-of-Order Rejection (Native & keystoned IPC)
 *   2. Stale Cursor & Watermark Invalidation (Native & keystoned IPC)
 *   3. Deletion & Tombstone Masking (Native & keystoned IPC)
 *   4. Cross-Tenant & Cross-SCI Partitioning (Native & keystoned IPC)
 *   5. Failover & Staged Replay Reconnect (Native & keystoned IPC)
 */

#include "keystone.h"
#include "keystoned.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define WF_TEST_TYPE_1 101u
#define WF_TEST_TYPE_2 102u

#define TENANT_ALPHA 100u
#define TENANT_BRAVO 200u

static keystone_uuid_t uuid_from_tag(char tag, int seq) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%02x000000-0000-0000-0000-%012d",
             (unsigned)(tag & 0xff), seq);
    keystone_uuid_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(keystone_uuid_from_string(buf, &out) == 0);
    return out;
}

static keystone_workflow_event_t make_adv_event(
    uint64_t seq, keystone_uuid_t record, keystone_uuid_t owner,
    uint32_t type, uint32_t tenant, uint64_t due,
    uint32_t classification, uint32_t sci, uint32_t flags,
    uint32_t tombstone) {
    keystone_workflow_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.committed_seq = seq;
    ev.entry.record_id = record;
    ev.entry.owner_id = owner;
    ev.entry.workflow_type = type;
    ev.entry.tenant = tenant;
    ev.entry.classification = classification;
    ev.entry.sci = sci;
    ev.entry.flags = flags;
    ev.entry.due_at = due;
    ev.tombstone = tombstone;
    return ev;
}

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
    usleep(25000);
    *out_server = server;
}

static void stop_daemon(keystoned_server_t *server, const char *sock_path) {
    if (server) {
        keystoned_server_stop(server);
        keystoned_server_destroy(server);
    }
    if (sock_path) {
        unlink(sock_path);
    }
}

/* ========================================================================= */
/* Gate 4a: Gap & Out-of-Order Rejection                                     */
/* ========================================================================= */

static void test_gate4_gap_and_out_of_order_native(void) {
    printf("[*] Gate 4a (Native): Gap injection & out-of-order rejection...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x41, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 1);

    /* Ingest committed events 1 through 4 */
    keystone_workflow_event_t init_events[4];
    for (int i = 0; i < 4; i++) {
        init_events[i] = make_adv_event(
            (uint64_t)(i + 1), uuid_from_tag('R', i + 1), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)(100 + i * 10),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 4, init_events, 4) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    /* 1. Gap injection: from=5 to=10 when watermark is 4 -> must fail with KEYSTONE_WORKFLOW_GAP */
    keystone_workflow_event_t gap_events[2];
    gap_events[0] = make_adv_event(7, uuid_from_tag('R', 7), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 200, 0, 0, 0, 0);
    gap_events[1] = make_adv_event(10, uuid_from_tag('R', 10), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 210, 0, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 5, 10, gap_events, 2)
                == KEYSTONE_WORKFLOW_GAP);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    /* 2. Gap in single event ingest: seq=6 when watermark is 4 -> KEYSTONE_WORKFLOW_GAP */
    keystone_workflow_event_t ev6 = make_adv_event(6, uuid_from_tag('R', 6), owner,
                                                   WF_TEST_TYPE_1, TENANT_ALPHA, 220, 0, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev6) == KEYSTONE_WORKFLOW_GAP);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    /* 3. Gap injection in staged batch: from=5 to=10 when watermark is 4 -> KEYSTONE_WORKFLOW_GAP */
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 5, 10, gap_events, 2)
                == KEYSTONE_WORKFLOW_GAP);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    /* 4. Out-of-order committed sequences in batch -> KEYSTONE_WORKFLOW_BAD_ARGUMENT */
    keystone_workflow_event_t ooo_events[2];
    ooo_events[0] = make_adv_event(6, uuid_from_tag('R', 6), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 200, 0, 0, 0, 0);
    ooo_events[1] = make_adv_event(5, uuid_from_tag('R', 5), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 210, 0, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 4, 6, ooo_events, 2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    /* 5. Event sequence outside claimed interval */
    keystone_workflow_event_t outside_low = make_adv_event(4, uuid_from_tag('R', 4), owner,
                                                           WF_TEST_TYPE_1, TENANT_ALPHA, 200, 0, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 4, 6, &outside_low, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    keystone_workflow_event_t outside_high = make_adv_event(7, uuid_from_tag('R', 7), owner,
                                                            WF_TEST_TYPE_1, TENANT_ALPHA, 200, 0, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 4, 6, &outside_high, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* 6. Inverted interval (through <= from) */
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 5, 5, gap_events, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 6, 5, gap_events, 1)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* 7. Duplicate committed sequence with same record_id in same batch -> CONFLICT */
    keystone_workflow_event_t conflict_batch[2];
    conflict_batch[0] = make_adv_event(5, uuid_from_tag('R', 5), owner, WF_TEST_TYPE_1,
                                       TENANT_ALPHA, 200, 0, 0, 0, 0);
    conflict_batch[1] = make_adv_event(5, uuid_from_tag('R', 5), owner, WF_TEST_TYPE_1,
                                       TENANT_ALPHA, 210, 0, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 4, 5, conflict_batch, 2)
                == KEYSTONE_WORKFLOW_CONFLICT);

    /* Watermark must remain completely untouched */
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Native gap injection and out-of-order rejection verified.\n");
}

static void test_gate4_gap_and_out_of_order_ipc(void) {
    printf("[*] Gate 4a (IPC): Gap injection & out-of-order rejection via keystoned...\n");

    const char *sock = "/tmp/keystone_gate4_gap.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    keystone_security_context_t op_ctx = {
        .tenant_id = TENANT_ALPHA,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx) == 0);

    keystoned_client_t *client = keystoned_client_connect(sock);
    TEST_ASSERT(client != NULL);

    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x42, sizeof(identity));
    uint64_t watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_bind_history(client, &op_ctx, identity, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 0);

    keystone_uuid_t owner = uuid_from_tag('O', 1);

    /* Initial 4 events */
    keystone_workflow_event_t init_events[4];
    for (int i = 0; i < 4; i++) {
        init_events[i] = make_adv_event(
            (uint64_t)(i + 1), uuid_from_tag('R', i + 1), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)(100 + i * 10),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystoned_client_workflow_apply(client, &op_ctx, 0, 4, init_events, 4, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 4);

    /* 1. IPC Gap injection: from=5 to=10 when watermark is 4 -> KEYSTONED_STATUS_GAP */
    keystone_workflow_event_t gap_events[2];
    gap_events[0] = make_adv_event(7, uuid_from_tag('R', 7), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 200, 0, 0, 0, 0);
    gap_events[1] = make_adv_event(10, uuid_from_tag('R', 10), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 210, 0, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(client, &op_ctx, 5, 10, gap_events, 2, &watermark)
                == KEYSTONED_STATUS_GAP);
    TEST_ASSERT(watermark == 4);

    /* 2. IPC Staged batch gap injection */
    uint64_t stage_wm = 0;
    TEST_ASSERT(keystoned_client_workflow_stage(client, &op_ctx, 5, 10, gap_events, 2, &stage_wm)
                == KEYSTONED_STATUS_GAP);

    /* 3. IPC Out-of-order rejection */
    keystone_workflow_event_t ooo_events[2];
    ooo_events[0] = make_adv_event(6, uuid_from_tag('R', 6), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 200, 0, 0, 0, 0);
    ooo_events[1] = make_adv_event(5, uuid_from_tag('R', 5), owner, WF_TEST_TYPE_1,
                                   TENANT_ALPHA, 210, 0, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(client, &op_ctx, 4, 6, ooo_events, 2, &watermark)
                == KEYSTONED_STATUS_INVALID_ARG);
    TEST_ASSERT(watermark == 4);

    /* 4. IPC Duplicate batch rejection */
    TEST_ASSERT(keystoned_client_workflow_apply(client, &op_ctx, 0, 4, init_events, 4, &watermark)
                == KEYSTONED_STATUS_CONFLICT);
    TEST_ASSERT(watermark == 4);

    keystoned_client_disconnect(client);
    stop_daemon(server, sock);
    printf("    [+] IPC gap injection and out-of-order rejection verified.\n");
}

/* ========================================================================= */
/* Gate 4b: Stale Cursor & Watermark Invalidation                            */
/* ========================================================================= */

static void test_gate4_stale_cursor_and_watermark_invalidation_native(void) {
    printf("[*] Gate 4b (Native): Stale cursor & watermark invalidation...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x43, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 2);

    /* Ingest 10 events with deadlines 100, 200, ..., 1000 */
    keystone_workflow_event_t events[10];
    for (int i = 0; i < 10; i++) {
        events[i] = make_adv_event(
            (uint64_t)(i + 1), uuid_from_tag('R', i + 1), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)((i + 1) * 100),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 10, events, 10) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 10);

    /* 1. Fetch Page 1: limit 3 */
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TEST_TYPE_1;
    q.tenant = TENANT_ALPHA;
    q.owner_id = owner;
    q.limit = 3;
    q.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page1) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page1.count == 3);
    TEST_ASSERT(page1.watermark == 10);
    TEST_ASSERT(page1.has_more == 1);
    TEST_ASSERT(page1.next_cursor_valid == 1);
    TEST_ASSERT(page1.next_cursor_due_at == 300);

    /* 2. Advance index: Ingest event 11 */
    keystone_workflow_event_t ev11 = make_adv_event(
        11, uuid_from_tag('R', 11), owner, WF_TEST_TYPE_1,
        TENANT_ALPHA, 1100, KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev11) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 11);

    /* 3. Fetch Page 2 with stale watermark: pass page1 cursor + expected_watermark = 10 */
    q.cursor_valid = 1;
    q.cursor = page1.next_cursor;
    q.cursor_due_at = page1.next_cursor_due_at;
    q.expected_watermark = 10; /* STALE! */
    keystone_workflow_page_t page2;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_STALE);

    /* 4. Fetch Page 2 with updated watermark: pass expected_watermark = 11 -> OK */
    q.expected_watermark = 11;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page2.count == 3);
    TEST_ASSERT(out[0].due_at == 400 && out[1].due_at == 500 && out[2].due_at == 600);

    /* 5. Required sequence invalidation (query required_seq > watermark) */
    q.cursor_valid = 0;
    q.expected_watermark = 0;
    q.required_seq = 20; /* Current watermark is 11 */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_STALE);
    q.required_seq = 11; /* At current watermark */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2) == KEYSTONE_WORKFLOW_OK);

    /* 6. Invalid cursor / query arguments rejection */
    /* cursor_valid = 1 but expected_watermark = 0 */
    q.cursor_valid = 1;
    q.expected_watermark = 0;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* cursor_valid > 1 */
    q.cursor_valid = 2;
    q.expected_watermark = 11;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* descending > 1 */
    q.cursor_valid = 0;
    q.descending = 2;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    q.descending = 0;

    /* limit = 0 */
    q.limit = 0;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* limit > KEYSTONE_WORKFLOW_PAGE_MAX (256) */
    q.limit = 257;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* capacity < limit */
    q.limit = 8;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 4, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);

    /* workflow_type = 0 */
    q.workflow_type = 0;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_BAD_ARGUMENT);
    q.workflow_type = WF_TEST_TYPE_1;

    /* 7. Fabricated cursor deadline beyond all entries */
    q.cursor_valid = 1;
    q.expected_watermark = 11;
    q.cursor_due_at = 9999999;
    memset(&q.cursor, 0xef, sizeof(q.cursor));
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page2.count == 0 && page2.has_more == 0);

    /* 8. Descending pagination stale watermark check */
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TEST_TYPE_1;
    q.tenant = TENANT_ALPHA;
    q.owner_id = owner;
    q.limit = 3;
    q.descending = 1;
    q.max_due_at = UINT64_MAX;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page1) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page1.count == 3 && page1.watermark == 11);
    TEST_ASSERT(out[0].due_at == 1100 && out[1].due_at == 1000 && out[2].due_at == 900);

    /* Ingest event 12 */
    keystone_workflow_event_t ev12 = make_adv_event(
        12, uuid_from_tag('R', 12), owner, WF_TEST_TYPE_1,
        TENANT_ALPHA, 1200, KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev12) == KEYSTONE_WORKFLOW_OK);

    /* Descending page 2 with old watermark 11 */
    q.cursor_valid = 1;
    q.cursor = page1.next_cursor;
    q.cursor_due_at = page1.next_cursor_due_at;
    q.expected_watermark = 11;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 8, &page2)
                == KEYSTONE_WORKFLOW_STALE);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Native stale cursor & watermark invalidation verified.\n");
}

static void test_gate4_stale_cursor_and_watermark_invalidation_ipc(void) {
    printf("[*] Gate 4b (IPC): Stale cursor & watermark invalidation via keystoned...\n");

    const char *sock = "/tmp/keystone_gate4_stale.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    keystone_security_context_t op_ctx = {
        .tenant_id = TENANT_ALPHA,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx) == 0);

    keystoned_client_t *proj = keystoned_client_connect(sock);
    TEST_ASSERT(proj != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x44, sizeof(identity));
    uint64_t watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_bind_history(proj, &op_ctx, identity, &watermark)
                == KEYSTONED_STATUS_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 2);

    /* Ingest 10 events via projector */
    keystone_workflow_event_t events[10];
    for (int i = 0; i < 10; i++) {
        events[i] = make_adv_event(
            (uint64_t)(i + 1), uuid_from_tag('R', i + 1), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)((i + 1) * 100),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 0, 10, events, 10, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 10);

    /* Reader client */
    keystoned_client_t *reader = keystoned_client_connect(sock);
    TEST_ASSERT(reader != NULL);

    /* Reader fetches Page 1 */
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TEST_TYPE_1;
    q.tenant = TENANT_ALPHA;
    q.owner_id = owner;
    q.limit = 3;
    q.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page1;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 8, &page1)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page1.count == 3 && page1.watermark == 10);

    /* Projector advances index to 11 */
    keystone_workflow_event_t ev11 = make_adv_event(
        11, uuid_from_tag('R', 11), owner, WF_TEST_TYPE_1,
        TENANT_ALPHA, 1100, KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 10, 11, &ev11, 1, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 11);

    /* Reader fetches Page 2 with expected_watermark = 10 -> KEYSTONED_STATUS_STALE_GEN */
    q.cursor_valid = 1;
    q.cursor = page1.next_cursor;
    q.cursor_due_at = page1.next_cursor_due_at;
    q.expected_watermark = 10;
    keystone_workflow_page_t page2;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 8, &page2)
                == KEYSTONED_STATUS_STALE_GEN);

    /* Reader fetches Page 2 with expected_watermark = 11 -> KEYSTONED_STATUS_OK */
    q.expected_watermark = 11;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 8, &page2)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page2.count == 3);

    /* Invalid cursor / expected_watermark combo */
    q.cursor_valid = 1;
    q.expected_watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 8, &page2)
                == KEYSTONED_STATUS_INVALID_ARG);

    /* Invalid limit: client/server rejects */
    q.cursor_valid = 0;
    q.expected_watermark = 11;
    q.limit = 0;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 8, &page2)
                != KEYSTONED_STATUS_OK);

    /* Required seq > watermark */
    q.limit = 3;
    q.required_seq = 50;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 8, &page2)
                == KEYSTONED_STATUS_STALE_GEN);

    keystoned_client_disconnect(reader);
    keystoned_client_disconnect(proj);
    stop_daemon(server, sock);
    printf("    [+] IPC stale cursor & watermark invalidation verified.\n");
}

/* ========================================================================= */
/* Gate 4c: Deletion & Tombstone Masking                                     */
/* ========================================================================= */

static void test_gate4_deletion_and_tombstone_masking_native(void) {
    printf("[*] Gate 4c (Native): Deletion & tombstone masking...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x45, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner1 = uuid_from_tag('O', 1);
    keystone_uuid_t owner2 = uuid_from_tag('O', 2);
    keystone_uuid_t rec_x  = uuid_from_tag('X', 1);

    /* 1. Upsert record X under owner 1 */
    keystone_workflow_event_t ev1 = make_adv_event(
        1, rec_x, owner1, WF_TEST_TYPE_1, TENANT_ALPHA, 100,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev1) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 1);

    /* Verify record X is returned for owner 1 */
    keystone_workflow_query_t q1;
    memset(&q1, 0, sizeof(q1));
    q1.workflow_type = WF_TEST_TYPE_1;
    q1.tenant = TENANT_ALPHA;
    q1.owner_id = owner1;
    q1.limit = 8;
    q1.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[8];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q1, out, 8, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1);
    TEST_ASSERT(memcmp(&out[0].record_id, &rec_x, sizeof(rec_x)) == 0);

    /* 2. Attempt to create record X under owner 2 while active under owner 1 -> CONFLICT */
    keystone_workflow_event_t ev_conflict = make_adv_event(
        2, rec_x, owner2, WF_TEST_TYPE_1, TENANT_ALPHA, 200,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev_conflict)
                == KEYSTONE_WORKFLOW_CONFLICT);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 1);

    /* 3. Tombstone record X under owner 1 */
    keystone_workflow_event_t ev_tomb = make_adv_event(
        2, rec_x, owner1, WF_TEST_TYPE_1, TENANT_ALPHA, 100,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 1 /* tombstone = 1 */);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev_tomb) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 2);

    /* 4. Verify record X is omitted from subsequent pages for owner 1 */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q1, out, 8, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 0);

    /* 5. Re-create record X under a different owner (owner 2) */
    keystone_workflow_event_t ev_recreate = make_adv_event(
        3, rec_x, owner2, WF_TEST_TYPE_1, TENANT_ALPHA, 250,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0 /* upsert */);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev_recreate) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 3);

    /* 6. Verify: owner 1 remains empty (old entry masked/hidden) */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q1, out, 8, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 0);

    /* 7. Verify: owner 2 indexes record X cleanly */
    keystone_workflow_query_t q2;
    memset(&q2, 0, sizeof(q2));
    q2.workflow_type = WF_TEST_TYPE_1;
    q2.tenant = TENANT_ALPHA;
    q2.owner_id = owner2;
    q2.limit = 8;
    q2.max_due_at = UINT64_MAX;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q2, out, 8, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1);
    TEST_ASSERT(memcmp(&out[0].record_id, &rec_x, sizeof(rec_x)) == 0);
    TEST_ASSERT(memcmp(&out[0].owner_id, &owner2, sizeof(owner2)) == 0);
    TEST_ASSERT(out[0].due_at == 250);

    /* 8. Tombstone on non-existent record is benign no-op */
    keystone_workflow_event_t ev_ghost = make_adv_event(
        4, uuid_from_tag('G', 99), owner2, WF_TEST_TYPE_1, TENANT_ALPHA, 999,
        0, 0, 0, 1 /* tombstone = 1 */);
    TEST_ASSERT(keystone_workflow_index_ingest(ix, &ev_ghost) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 4);

    /* Owner 2 still has record X intact */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q2, out, 8, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Native deletion, tombstone masking, and owner re-creation verified.\n");
}

static void test_gate4_deletion_and_tombstone_masking_ipc(void) {
    printf("[*] Gate 4c (IPC): Deletion & tombstone masking via keystoned...\n");

    const char *sock = "/tmp/keystone_gate4_delete.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    keystone_security_context_t op_ctx = {
        .tenant_id = TENANT_ALPHA,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx) == 0);

    keystoned_client_t *proj = keystoned_client_connect(sock);
    TEST_ASSERT(proj != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x46, sizeof(identity));
    uint64_t watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_bind_history(proj, &op_ctx, identity, &watermark)
                == KEYSTONED_STATUS_OK);

    keystoned_client_t *reader = keystoned_client_connect(sock);
    TEST_ASSERT(reader != NULL);

    keystone_uuid_t owner1 = uuid_from_tag('O', 1);
    keystone_uuid_t owner2 = uuid_from_tag('O', 2);
    keystone_uuid_t rec_y  = uuid_from_tag('Y', 1);

    /* 1. Upsert record Y under owner 1 via IPC */
    keystone_workflow_event_t ev1 = make_adv_event(
        1, rec_y, owner1, WF_TEST_TYPE_1, TENANT_ALPHA, 100,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 0, 1, &ev1, 1, &watermark)
                == KEYSTONED_STATUS_OK);

    /* Reader verifies record Y visible under owner 1 */
    keystone_workflow_query_t q1;
    memset(&q1, 0, sizeof(q1));
    q1.workflow_type = WF_TEST_TYPE_1;
    q1.tenant = TENANT_ALPHA;
    q1.owner_id = owner1;
    q1.limit = 4;
    q1.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[4];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q1, out, 4, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 1);

    /* 2. Tombstone record Y via IPC */
    keystone_workflow_event_t ev_tomb = make_adv_event(
        2, rec_y, owner1, WF_TEST_TYPE_1, TENANT_ALPHA, 100,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 1);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 1, 2, &ev_tomb, 1, &watermark)
                == KEYSTONED_STATUS_OK);

    /* Reader verifies record Y omitted under owner 1 */
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q1, out, 4, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 0);

    /* 3. Re-create record Y under owner 2 */
    keystone_workflow_event_t ev_recreate = make_adv_event(
        3, rec_y, owner2, WF_TEST_TYPE_1, TENANT_ALPHA, 300,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &op_ctx, 2, 3, &ev_recreate, 1, &watermark)
                == KEYSTONED_STATUS_OK);

    /* Reader verifies owner 1 has 0 entries, owner 2 has record Y */
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q1, out, 4, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 0);

    keystone_workflow_query_t q2;
    memset(&q2, 0, sizeof(q2));
    q2.workflow_type = WF_TEST_TYPE_1;
    q2.tenant = TENANT_ALPHA;
    q2.owner_id = owner2;
    q2.limit = 4;
    q2.max_due_at = UINT64_MAX;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q2, out, 4, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 1);
    TEST_ASSERT(memcmp(&out[0].record_id, &rec_y, sizeof(rec_y)) == 0);
    TEST_ASSERT(out[0].due_at == 300);

    keystoned_client_disconnect(reader);
    keystoned_client_disconnect(proj);
    stop_daemon(server, sock);
    printf("    [+] IPC deletion, tombstone masking, and owner re-creation verified.\n");
}

/* ========================================================================= */
/* Gate 4d: Cross-Tenant & Cross-SCI Partitioning (Security Guard)           */
/* ========================================================================= */

static void test_gate4_cross_tenant_and_cross_sci_native(void) {
    printf("[*] Gate 4d (Native): Cross-tenant & cross-SCI partitioning...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x47, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 7);

    /* Tenant Alpha records */
    /* Rec A1: UNCLASSIFIED, SCI=0 */
    keystone_workflow_event_t rec_a1 = make_adv_event(
        1, uuid_from_tag('A', 1), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 100,
        KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0x0, 0, 0);
    /* Rec A2: RESTRICTED, SCI=0x1 */
    keystone_workflow_event_t rec_a2 = make_adv_event(
        2, uuid_from_tag('A', 2), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 200,
        KEYSTONE_CLASSIFICATION_RESTRICTED, 0x1, 0, 0);
    /* Rec A3: SECRET, SCI=0x3 */
    keystone_workflow_event_t rec_a3 = make_adv_event(
        3, uuid_from_tag('A', 3), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 300,
        KEYSTONE_CLASSIFICATION_SECRET, 0x3, 0, 0);
    /* Rec A4: TOP_SECRET, SCI=0x7 */
    keystone_workflow_event_t rec_a4 = make_adv_event(
        4, uuid_from_tag('A', 4), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 400,
        KEYSTONE_CLASSIFICATION_TOP_SECRET, 0x7, 0, 0);
    /* Rec A5: SECRET, SECURITY_SENSITIVE, SCI=0x1 */
    keystone_workflow_event_t rec_a5 = make_adv_event(
        5, uuid_from_tag('A', 5), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 500,
        KEYSTONE_CLASSIFICATION_SECRET, 0x1, KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE, 0);

    /* Tenant Bravo records */
    /* Rec B1: SECRET, SCI=0x1 */
    keystone_workflow_event_t rec_b1 = make_adv_event(
        6, uuid_from_tag('B', 1), owner, WF_TEST_TYPE_1, TENANT_BRAVO, 150,
        KEYSTONE_CLASSIFICATION_SECRET, 0x1, 0, 0);
    /* Rec B2: TOP_SECRET, SCI=0x3 */
    keystone_workflow_event_t rec_b2 = make_adv_event(
        7, uuid_from_tag('B', 2), owner, WF_TEST_TYPE_1, TENANT_BRAVO, 250,
        KEYSTONE_CLASSIFICATION_TOP_SECRET, 0x3, 0, 0);

    keystone_workflow_event_t all[7] = { rec_a1, rec_a2, rec_a3, rec_a4, rec_a5, rec_b1, rec_b2 };
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 7, all, 7) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 7);

    /* 1. Strict Cross-Tenant Partitioning (Zero Metadata Leakage) */
    keystone_workflow_query_t q_alpha;
    memset(&q_alpha, 0, sizeof(q_alpha));
    q_alpha.workflow_type = WF_TEST_TYPE_1;
    q_alpha.tenant = TENANT_ALPHA;
    q_alpha.owner_id = owner;
    q_alpha.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q_alpha.sci = UINT32_MAX;
    q_alpha.limit = 16;
    q_alpha.max_due_at = UINT64_MAX;

    keystone_workflow_entry_t out[16];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 5);
    for (uint32_t i = 0; i < page.count; i++) {
        TEST_ASSERT(out[i].tenant == TENANT_ALPHA);
        TEST_ASSERT(out[i].record_id.bytes[0] == (uint8_t)'A');
    }

    keystone_workflow_query_t q_bravo;
    memset(&q_bravo, 0, sizeof(q_bravo));
    q_bravo.workflow_type = WF_TEST_TYPE_1;
    q_bravo.tenant = TENANT_BRAVO;
    q_bravo.owner_id = owner;
    q_bravo.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    q_bravo.sci = UINT32_MAX;
    q_bravo.limit = 16;
    q_bravo.max_due_at = UINT64_MAX;

    TEST_ASSERT(keystone_workflow_index_page(ix, &q_bravo, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2);
    for (uint32_t i = 0; i < page.count; i++) {
        TEST_ASSERT(out[i].tenant == TENANT_BRAVO);
        TEST_ASSERT(out[i].record_id.bytes[0] == (uint8_t)'B');
    }

    /* 2. Clearance Partitioning (Tenant Alpha) */
    /* UNCLASSIFIED clearance */
    q_alpha.max_classification = KEYSTONE_CLASSIFICATION_UNCLASSIFIED;
    q_alpha.sci = UINT32_MAX;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1);
    TEST_ASSERT(out[0].due_at == 100);

    /* RESTRICTED clearance */
    q_alpha.max_classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 2);
    TEST_ASSERT(out[0].due_at == 100 && out[1].due_at == 200);

    /* SECRET clearance (A1, A2, A3, A5; A4 TOP_SECRET must be omitted) */
    q_alpha.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 4);
    for (uint32_t i = 0; i < page.count; i++) {
        TEST_ASSERT(out[i].classification <= KEYSTONE_CLASSIFICATION_SECRET);
    }

    /* 3. Cross-SCI Compartment Filtering (Authorized Compartments Subset Check) */
    q_alpha.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;

    /* Compartment mask = 0: only A1 (SCI 0) returned */
    q_alpha.sci = 0x0;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 1);
    TEST_ASSERT(out[0].due_at == 100);

    /* Compartment mask = 0x1: A1 (0), A2 (0x1), A5 (0x1) returned. A3 (0x3) and A4 (0x7) omitted */
    q_alpha.sci = 0x1;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 3);
    TEST_ASSERT(out[0].due_at == 100 && out[1].due_at == 200 && out[2].due_at == 500);

    /* Compartment mask = 0x3: A1, A2, A3, A5 returned. A4 (SCI 0x7 requires bit 2) omitted */
    q_alpha.sci = 0x3;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q_alpha, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 4);
    for (uint32_t i = 0; i < page.count; i++) {
        TEST_ASSERT((out[i].sci & ~0x3u) == 0);
    }

    keystone_workflow_index_destroy(ix);
    printf("    [+] Native cross-tenant & cross-SCI partitioning verified.\n");
}

static void test_gate4_cross_tenant_and_cross_sci_ipc(void) {
    printf("[*] Gate 4d (IPC): Cross-tenant & cross-SCI security denial via keystoned...\n");

    const char *sock = "/tmp/keystone_gate4_security.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    /* Projector bound to Tenant Alpha */
    keystone_security_context_t proj_ctx = {
        .tenant_id = TENANT_ALPHA,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &proj_ctx) == 0);

    keystoned_client_t *proj = keystoned_client_connect(sock);
    TEST_ASSERT(proj != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x48, sizeof(identity));
    uint64_t watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_bind_history(proj, &proj_ctx, identity, &watermark)
                == KEYSTONED_STATUS_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 7);

    /* 1. Projector attempts to ingest record for Tenant Bravo -> KEYSTONED_STATUS_DENIED */
    keystone_workflow_event_t rec_alien = make_adv_event(
        1, uuid_from_tag('B', 1), owner, WF_TEST_TYPE_1, TENANT_BRAVO, 100,
        KEYSTONE_CLASSIFICATION_SECRET, 0x1, 0, 0);
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &proj_ctx, 0, 1, &rec_alien, 1, &watermark)
                == KEYSTONED_STATUS_DENIED);
    TEST_ASSERT(watermark == 0);

    /* Ingest legitimate Tenant Alpha records */
    keystone_workflow_event_t rec_a1 = make_adv_event(
        1, uuid_from_tag('A', 1), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 100,
        KEYSTONE_CLASSIFICATION_SECRET, 0x1, 0, 0);
    keystone_workflow_event_t rec_a2 = make_adv_event(
        2, uuid_from_tag('A', 2), owner, WF_TEST_TYPE_1, TENANT_ALPHA, 200,
        KEYSTONE_CLASSIFICATION_TOP_SECRET, 0x3, 0, 0);
    keystone_workflow_event_t a_batch[2] = { rec_a1, rec_a2 };
    TEST_ASSERT(keystoned_client_workflow_apply(proj, &proj_ctx, 0, 2, a_batch, 2, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 2);

    /* 2. Reader downgraded to SECRET, SCI mask 0x1 */
    keystoned_client_t *reader = keystoned_client_connect(sock);
    TEST_ASSERT(reader != NULL);
    keystone_security_context_t reader_ctx = {
        .tenant_id = TENANT_ALPHA,
        .classification = KEYSTONE_CLASSIFICATION_SECRET,
        .compartment_mask = 0x1,
    };
    TEST_ASSERT(keystoned_client_bind_principal(reader, &reader_ctx) == 0);

    /* Reader attempts cross-tenant query for Tenant Bravo -> KEYSTONED_STATUS_DENIED */
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TEST_TYPE_1;
    q.tenant = TENANT_BRAVO;
    q.owner_id = owner;
    q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    q.sci = 0x1;
    q.limit = 4;
    q.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[4];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &reader_ctx, &q, out, 4, &page)
                == KEYSTONED_STATUS_DENIED);

    /* Reader attempts query above its own clearance (TOP_SECRET) -> KEYSTONED_STATUS_DENIED */
    q.tenant = TENANT_ALPHA;
    q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &reader_ctx, &q, out, 4, &page)
                == KEYSTONED_STATUS_DENIED);

    /* Reader attempts query with compartment bits beyond its mask (0x3 > 0x1) -> KEYSTONED_STATUS_DENIED */
    q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
    q.sci = 0x3;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &reader_ctx, &q, out, 4, &page)
                == KEYSTONED_STATUS_DENIED);

    /* Authorized query: returns exactly A1 (A2 TOP_SECRET is omitted) */
    q.sci = 0x1;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &reader_ctx, &q, out, 4, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 1);
    TEST_ASSERT(out[0].due_at == 100);

    keystoned_client_disconnect(reader);
    keystoned_client_disconnect(proj);
    stop_daemon(server, sock);
    printf("    [+] IPC cross-tenant & cross-SCI security denial verified.\n");
}

/* ========================================================================= */
/* Gate 4e: Failover & Staged Replay Reconnect                                */
/* ========================================================================= */

static void test_gate4_failover_and_staged_replay_reconnect_native(void) {
    printf("[*] Gate 4e (Native): Staged replay failover & abort...\n");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    TEST_ASSERT(ix != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x49, sizeof(identity));
    TEST_ASSERT(keystone_workflow_index_bind_history(ix, identity) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 9);

    /* Ingest base events 1..5 */
    keystone_workflow_event_t base[5];
    for (int i = 0; i < 5; i++) {
        base[i] = make_adv_event(
            (uint64_t)(i + 1), uuid_from_tag('P', i + 1), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)((i + 1) * 100),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystone_workflow_index_ingest_batch(ix, 0, 5, base, 5) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 5);

    /* Stage events 6..10 */
    keystone_workflow_event_t stage_events[5];
    for (int i = 0; i < 5; i++) {
        stage_events[i] = make_adv_event(
            (uint64_t)(i + 6), uuid_from_tag('S', i + 6), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)((i + 6) * 100),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 5, 10, stage_events, 5)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 10);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 5);

    /* Readers must only see published records (5 entries), NOT staged records */
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TEST_TYPE_1;
    q.tenant = TENANT_ALPHA;
    q.owner_id = owner;
    q.limit = 16;
    q.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[16];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 5 && page.watermark == 5);

    /* Abort stage (simulating replay crash / failover) */
    TEST_ASSERT(keystone_workflow_index_abort_stage(ix) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 0);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 5);

    /* Verify published index remains completely intact */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 5 && page.watermark == 5);

    /* Re-stage and publish cleanly */
    TEST_ASSERT(keystone_workflow_index_stage_batch(ix, 5, 10, stage_events, 5)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_stage_watermark(ix) == 10);
    TEST_ASSERT(keystone_workflow_index_publish(ix, 10) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(ix) == 10);

    /* Now all 10 entries are visible */
    TEST_ASSERT(keystone_workflow_index_page(ix, &q, out, 16, &page) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(page.count == 10 && page.watermark == 10);

    keystone_workflow_index_destroy(ix);
    printf("    [+] Native staged replay failover and abort verified.\n");
}

static void test_gate4_failover_and_staged_replay_reconnect_ipc(void) {
    printf("[*] Gate 4e (IPC): Staged replay disconnect failover & reconnect reconciliation...\n");

    const char *sock = "/tmp/keystone_gate4_failover.sock";
    keystoned_server_t *server = NULL;
    run_daemon(sock, &server);

    keystone_security_context_t op_ctx = {
        .tenant_id = TENANT_ALPHA,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx) == 0);

    /* Projector 1 connects */
    keystoned_client_t *proj1 = keystoned_client_connect(sock);
    TEST_ASSERT(proj1 != NULL);
    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0x4a, sizeof(identity));
    uint64_t watermark = 0;
    TEST_ASSERT(keystoned_client_workflow_bind_history(proj1, &op_ctx, identity, &watermark)
                == KEYSTONED_STATUS_OK);

    keystone_uuid_t owner = uuid_from_tag('O', 9);

    /* Ingest base events 1..5 */
    keystone_workflow_event_t base[5];
    for (int i = 0; i < 5; i++) {
        base[i] = make_adv_event(
            (uint64_t)(i + 1), uuid_from_tag('P', i + 1), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)((i + 1) * 100),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    TEST_ASSERT(keystoned_client_workflow_apply(proj1, &op_ctx, 0, 5, base, 5, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 5);

    /* Projector 1 begins staged replay: stages 6..10 */
    keystone_workflow_event_t stage_events[5];
    for (int i = 0; i < 5; i++) {
        stage_events[i] = make_adv_event(
            (uint64_t)(i + 6), uuid_from_tag('S', i + 6), owner,
            WF_TEST_TYPE_1, TENANT_ALPHA, (uint64_t)((i + 6) * 100),
            KEYSTONE_CLASSIFICATION_UNCLASSIFIED, 0, 0, 0);
    }
    uint64_t stage_wm = 0;
    TEST_ASSERT(keystoned_client_workflow_stage(proj1, &op_ctx, 5, 10, stage_events, 5, &stage_wm)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(stage_wm == 10);

    /* Reader verifies published watermark is still 5 and staged events are hidden */
    keystoned_client_t *reader = keystoned_client_connect(sock);
    TEST_ASSERT(reader != NULL);
    uint64_t reader_wm = 0;
    TEST_ASSERT(keystoned_client_workflow_watermark(reader, &op_ctx, &reader_wm)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(reader_wm == 5);

    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = WF_TEST_TYPE_1;
    q.tenant = TENANT_ALPHA;
    q.owner_id = owner;
    q.limit = 16;
    q.max_due_at = UINT64_MAX;
    keystone_workflow_entry_t out[16];
    keystone_workflow_page_t page;
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 16, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 5 && page.watermark == 5);

    /* FAILOVER: Projector 1 disconnects abruptly without publishing */
    keystoned_client_disconnect(proj1);

    /* Allow server to reap disconnected connection and abort stage */
    usleep(50000);

    /* RECONNECT: Projector 2 connects to take over */
    keystoned_client_t *proj2 = keystoned_client_connect(sock);
    TEST_ASSERT(proj2 != NULL);

    /* Reconcile watermark from published state */
    uint64_t reconciled_wm = 0;
    TEST_ASSERT(keystoned_client_workflow_watermark(proj2, &op_ctx, &reconciled_wm)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(reconciled_wm == 5);

    /* Verify stage was reaped: stage watermark is now 0 */
    uint64_t cur_stage_wm = 999;
    TEST_ASSERT(keystoned_client_workflow_stage_watermark(proj2, &op_ctx, &cur_stage_wm)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(cur_stage_wm == 0);

    /* Re-stage events 6..10 from reconciled watermark */
    TEST_ASSERT(keystoned_client_workflow_stage(proj2, &op_ctx, 5, 10, stage_events, 5, &stage_wm)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(stage_wm == 10);

    /* Publish stage at expected_target = 10 */
    TEST_ASSERT(keystoned_client_workflow_publish(proj2, &op_ctx, 10, &watermark)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(watermark == 10);

    /* Reader now observes published entries 1..10 */
    TEST_ASSERT(keystoned_client_workflow_page(reader, &op_ctx, &q, out, 16, &page)
                == KEYSTONED_STATUS_OK);
    TEST_ASSERT(page.count == 10 && page.watermark == 10);

    keystoned_client_disconnect(reader);
    keystoned_client_disconnect(proj2);
    stop_daemon(server, sock);
    printf("    [+] IPC staged replay failover and reconnect reconciliation verified.\n");
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Workflow Candidate Launch Gates: Gate 4 Verification   \n");
    printf("=================================================================\n");

    /* Gate 4a: Gap & Out-of-Order Rejection */
    test_gate4_gap_and_out_of_order_native();
    test_gate4_gap_and_out_of_order_ipc();

    /* Gate 4b: Stale Cursor & Watermark Invalidation */
    test_gate4_stale_cursor_and_watermark_invalidation_native();
    test_gate4_stale_cursor_and_watermark_invalidation_ipc();

    /* Gate 4c: Deletion & Tombstone Masking */
    test_gate4_deletion_and_tombstone_masking_native();
    test_gate4_deletion_and_tombstone_masking_ipc();

    /* Gate 4d: Cross-Tenant & Cross-SCI Partitioning */
    test_gate4_cross_tenant_and_cross_sci_native();
    test_gate4_cross_tenant_and_cross_sci_ipc();

    /* Gate 4e: Failover & Staged Replay Reconnect */
    test_gate4_failover_and_staged_replay_reconnect_native();
    test_gate4_failover_and_staged_replay_reconnect_ipc();

    printf("=================================================================\n");
    printf("  GATE 4 ADVERSARIAL VERIFICATION: ALL GATES PASSED (100%% GREEN) \n");
    printf("=================================================================\n");

    return 0;
}
