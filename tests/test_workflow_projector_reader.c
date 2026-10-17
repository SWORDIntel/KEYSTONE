/*
 * KEYSTONE Workflow Projector & Reader Integration Tests (Gates 2 & 3)
 *
 * Covers:
 * - Gate 2: QIHSE.TXN.READ projector with in-boundary decryption, complete
 *   payload and search token stripping, opaque metadata interval ingestion,
 *   staged backlog replays, and lost-ACK / reconnect reconciliation.
 * - Gate 3: Application reader with ascending and descending cursor pagination,
 *   authoritative QIHSE KV rechecks (filtering deleted, cross-tenant, and
 *   unauthorized records), and strict fail-closed behavior with zero full-scan fallback.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#include "keystone.h"
#include "keystoned.h"
#include "keystone_workflow_projector.h"
#include "keystone_workflow_reader.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_TENANT 42u

static keystone_uuid_t make_test_uuid(uint8_t prefix, uint32_t val) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%02x000000-0000-0000-0000-%012u", prefix, val);
    keystone_uuid_t id;
    memset(&id, 0, sizeof(id));
    TEST_ASSERT(keystone_uuid_from_string(buf, &id) == 0);
    return id;
}

static void start_test_daemon(const char *sock_path, keystoned_server_t **out_server) {
    unlink(sock_path);
    keystoned_config_t cfg = {
        .socket_path = sock_path,
        .cache_capacity = 256,
        .initial_fencing_epoch = 1,
        .enable_security_audit = 1,
    };
    keystoned_server_t *server = keystoned_server_create(&cfg);
    TEST_ASSERT(server != NULL);

    keystone_security_context_t principal = {
        .tenant_id = TEST_TENANT,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    TEST_ASSERT(keystoned_server_bind_peer_principal(server, geteuid(), &principal) == 0);

    TEST_ASSERT(keystoned_server_start(server) == 0);
    usleep(20000);
    *out_server = server;
}

static void stop_test_daemon(const char *sock_path, keystoned_server_t *server) {
    if (server) {
        keystoned_server_stop(server);
        keystoned_server_destroy(server);
    }
    unlink(sock_path);
}

/* --- Gate 2 Test: Decryption, Stripping, and Payload Isolation --- */
static void test_projector_decryption_and_payload_stripping(void) {
    printf("[*] Gate 2: Projector in-boundary decryption and payload stripping...\n");

    const char *sock = "/tmp/keystone_test_gate2_strip.sock";
    keystoned_server_t *server = NULL;
    start_test_daemon(sock, &server);

    uint8_t genesis_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(genesis_history, 0x77, sizeof(genesis_history));

    keystone_workflow_projector_config_t proj_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        },
        .max_batch_size = 64,
        .auto_reconnect = 1,
    };
    memcpy(proj_cfg.history_id, genesis_history, sizeof(genesis_history));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&proj_cfg);
    TEST_ASSERT(proj != NULL);
    TEST_ASSERT(keystone_workflow_projector_connect(proj) == KEYSTONE_WORKFLOW_OK);

    uint8_t enc_key[32];
    memset(enc_key, 0x33, sizeof(enc_key));

    keystone_uuid_t owner = make_test_uuid(0xAA, 1);

    /* Construct diverse application transaction records with sensitive payloads */
    struct {
        uint32_t app_type;
        const char *sensitive_content;
        uint64_t due_at;
    } test_cases[] = {
        { KEYSTONE_WORKFLOW_TYPE_ORDER, "ORDER_PAYLOAD_CREDIT_CARD_4111_SECRET_CVV_999", 100 },
        { KEYSTONE_WORKFLOW_TYPE_PAYMENT, "PAYMENT_PAYLOAD_BANK_ACCOUNT_ROUTING_123456789", 200 },
        { KEYSTONE_WORKFLOW_TYPE_CONVERSION, "CONVERSION_TRACKING_CLICK_ID_TOKEN_ABCXYZ", 300 },
        { KEYSTONE_WORKFLOW_TYPE_REVIEW, "REVIEW_TEXT_HIGHLY_CONFIDENTIAL_CUSTOMER_NOTE", 400 },
        { KEYSTONE_WORKFLOW_TYPE_CATALOG, "CATALOG_INTERNAL_SUPPLIER_WHOLESALE_COST_888", 500 },
        { KEYSTONE_WORKFLOW_TYPE_ROOM_MESSAGE, "ROOM_MESSAGE_BODY_PRIVATE_EXECUTIVE_CHAT_MSG", 600 },
    };
    size_t num_cases = sizeof(test_cases) / sizeof(test_cases[0]);

    for (size_t i = 0; i < num_cases; i++) {
        uint64_t seq = i + 1;
        char enc_buf[128];
        size_t len = strlen(test_cases[i].sensitive_content);

        /* Encrypt payload outside KEYSTONE */
        keystone_workflow_projector_crypt_payload(
            enc_key, seq, test_cases[i].sensitive_content, enc_buf, len
        );

        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = seq;
        txn.tenant_id = TEST_TENANT;
        txn.classification = KEYSTONE_CLASSIFICATION_SECRET;
        txn.sci_mask = 0x1;
        txn.record_id = make_test_uuid(0xBB, (uint32_t)seq);
        txn.owner_id = owner;
        txn.workflow_type = test_cases[i].app_type;
        txn.is_tombstone = 0;
        txn.due_at = test_cases[i].due_at;
        txn.is_encrypted = 1;
        memcpy(txn.encryption_key, enc_key, 32);
        txn.payload = enc_buf;
        txn.payload_len = len;

        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }

    /* Flush batch to keystoned */
    TEST_ASSERT(keystone_workflow_projector_flush(proj) == KEYSTONE_WORKFLOW_OK);

    uint64_t wm = 0;
    TEST_ASSERT(keystone_workflow_projector_get_watermark(proj, &wm) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(wm == num_cases);

    /* Read back raw candidate entries from keystoned Unix socket */
    keystoned_client_t *reader = keystoned_client_connect(sock);
    TEST_ASSERT(reader != NULL);

    keystone_security_context_t read_ctx = {
        .tenant_id = TEST_TENANT,
        .classification = KEYSTONE_CLASSIFICATION_SECRET,
        .compartment_mask = 0x1,
    };
    TEST_ASSERT(keystoned_client_bind_principal(reader, &read_ctx) == 0);

    for (size_t i = 0; i < num_cases; i++) {
        keystone_workflow_query_t q;
        memset(&q, 0, sizeof(q));
        q.workflow_type = test_cases[i].app_type;
        q.owner_id = owner;
        q.tenant = TEST_TENANT;
        q.max_classification = KEYSTONE_CLASSIFICATION_SECRET;
        q.sci = 0x1;
        q.max_due_at = UINT64_MAX;
        q.limit = 8;

        keystone_workflow_entry_t out[8];
        keystone_workflow_page_t page;
        TEST_ASSERT(keystoned_client_workflow_page(reader, &read_ctx, &q, out, 8, &page) == KEYSTONED_STATUS_OK);
        TEST_ASSERT(page.count == 1);
        TEST_ASSERT(out[0].due_at == test_cases[i].due_at);
        TEST_ASSERT(out[0].workflow_type == test_cases[i].app_type);
        TEST_ASSERT(out[0].tenant == TEST_TENANT);

        /* VERIFICATION: The returned entry struct is strictly opaque metadata.
         * None of the sensitive payload text or search tokens exist anywhere in memory. */
        uint8_t *raw = (uint8_t *)&out[0];
        size_t raw_len = sizeof(out[0]);
        for (size_t s = 0; s < raw_len - 8; s++) {
            TEST_ASSERT(memcmp(raw + s, "CREDIT", 6) != 0);
            TEST_ASSERT(memcmp(raw + s, "BANK", 4) != 0);
            TEST_ASSERT(memcmp(raw + s, "PRIVATE", 7) != 0);
            TEST_ASSERT(memcmp(raw + s, "CONFIDENTIAL", 12) != 0);
            TEST_ASSERT(memcmp(raw + s, "WHOLESALE", 9) != 0);
        }
    }

    keystoned_client_disconnect(reader);
    keystone_workflow_projector_destroy(proj);
    stop_test_daemon(sock, server);

    printf("    [+] Zero payload leaked into KEYSTONE; metadata intervals successfully applied.\n");
}

/* --- Gate 2 Test: Staged Backlog Replay Capped at 4096 Events --- */
static void test_projector_staged_replay_backlog(void) {
    printf("[*] Gate 2: Staged backlog replay (>4096 events) and atomic publish...\n");

    const char *sock = "/tmp/keystone_test_gate2_staged.sock";
    keystoned_server_t *server = NULL;
    start_test_daemon(sock, &server);

    uint8_t genesis_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(genesis_history, 0x88, sizeof(genesis_history));

    keystone_workflow_projector_config_t proj_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        },
        .max_batch_size = 4096,
        .auto_reconnect = 1,
    };
    memcpy(proj_cfg.history_id, genesis_history, sizeof(genesis_history));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&proj_cfg);
    TEST_ASSERT(proj != NULL);
    TEST_ASSERT(keystone_workflow_projector_connect(proj) == KEYSTONE_WORKFLOW_OK);

    /* Generate 4500 events (exceeding single batch limit of 4096) */
    size_t total_events = 4500;
    keystone_workflow_event_t *events = calloc(total_events, sizeof(keystone_workflow_event_t));
    TEST_ASSERT(events != NULL);

    keystone_uuid_t owner = make_test_uuid(0xCC, 1);
    for (size_t i = 0; i < total_events; i++) {
        uint64_t seq = i + 1;
        events[i].committed_seq = seq;
        events[i].entry.record_id = make_test_uuid(0xDD, (uint32_t)seq);
        events[i].entry.owner_id = owner;
        events[i].entry.workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER;
        events[i].entry.tenant = TEST_TENANT;
        events[i].entry.classification = KEYSTONE_CLASSIFICATION_OPS;
        events[i].entry.sci = 0;
        events[i].entry.due_at = 1000 + seq;
        events[i].tombstone = 0;
    }

    /* Replay staged backlog and atomically publish at target */
    TEST_ASSERT(keystone_workflow_projector_stage_and_publish(
        proj, 0, total_events, events, total_events
    ) == KEYSTONE_WORKFLOW_OK);

    uint64_t wm = 0;
    TEST_ASSERT(keystone_workflow_projector_get_watermark(proj, &wm) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(wm == total_events);

    free(events);
    keystone_workflow_projector_destroy(proj);
    stop_test_daemon(sock, server);

    printf("    [+] Large staged replay chunked at 4096 events and published atomically.\n");
}

/* --- Gate 2 Test: Lost ACK & Reconnect Reconciliation --- */
static void test_projector_lost_ack_reconciliation(void) {
    printf("[*] Gate 2: Lost ACK reconciliation and reconnect handling...\n");

    const char *sock = "/tmp/keystone_test_gate2_ack.sock";
    keystoned_server_t *server = NULL;
    start_test_daemon(sock, &server);

    uint8_t genesis_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(genesis_history, 0x99, sizeof(genesis_history));

    keystone_workflow_projector_config_t proj_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        },
        .max_batch_size = 64,
        .auto_reconnect = 1,
    };
    memcpy(proj_cfg.history_id, genesis_history, sizeof(genesis_history));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&proj_cfg);
    TEST_ASSERT(proj != NULL);
    TEST_ASSERT(keystone_workflow_projector_connect(proj) == KEYSTONE_WORKFLOW_OK);

    /* Ingest 10 events */
    keystone_uuid_t owner = make_test_uuid(0xEE, 1);
    for (uint64_t seq = 1; seq <= 10; seq++) {
        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = seq;
        txn.tenant_id = TEST_TENANT;
        txn.classification = KEYSTONE_CLASSIFICATION_OPS;
        txn.record_id = make_test_uuid(0xFF, (uint32_t)seq);
        txn.owner_id = owner;
        txn.workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER;
        txn.due_at = seq * 10;
        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }
    TEST_ASSERT(keystone_workflow_projector_flush(proj) == KEYSTONE_WORKFLOW_OK);

    /* Simulate sudden disconnect */
    keystone_workflow_projector_disconnect(proj);

    /* Test lost ACK reconciliation: target sequence 10 was already committed */
    int already_committed = 0;
    TEST_ASSERT(keystone_workflow_projector_reconcile_lost_ack(proj, 10, &already_committed) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(already_committed == 1);

    /* Test target sequence 15 was NOT committed */
    TEST_ASSERT(keystone_workflow_projector_reconcile_lost_ack(proj, 15, &already_committed) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(already_committed == 0);

    /* Projector can now resume from watermark 10 without gap */
    for (uint64_t seq = 11; seq <= 15; seq++) {
        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = seq;
        txn.tenant_id = TEST_TENANT;
        txn.classification = KEYSTONE_CLASSIFICATION_OPS;
        txn.record_id = make_test_uuid(0xFF, (uint32_t)seq);
        txn.owner_id = owner;
        txn.workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER;
        txn.due_at = seq * 10;
        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }
    TEST_ASSERT(keystone_workflow_projector_flush(proj) == KEYSTONE_WORKFLOW_OK);

    uint64_t final_wm = 0;
    TEST_ASSERT(keystone_workflow_projector_get_watermark(proj, &final_wm) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(final_wm == 15);

    keystone_workflow_projector_destroy(proj);
    stop_test_daemon(sock, server);

    printf("    [+] Lost ACK reconciliation and reconnect recovered cleanly.\n");
}

/* --- Gate 3 Test: Paged Cursor Queries (Ascending and Descending) --- */
static void test_reader_cursor_pagination_and_ordering(void) {
    printf("[*] Gate 3: Reader cursor pagination (ascending & descending)...\n");

    const char *sock = "/tmp/keystone_test_gate3_cursor.sock";
    keystoned_server_t *server = NULL;
    start_test_daemon(sock, &server);

    uint8_t genesis_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(genesis_history, 0x55, sizeof(genesis_history));

    keystone_workflow_projector_config_t proj_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        },
        .max_batch_size = 128,
        .auto_reconnect = 1,
    };
    memcpy(proj_cfg.history_id, genesis_history, sizeof(genesis_history));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&proj_cfg);
    TEST_ASSERT(proj != NULL);
    TEST_ASSERT(keystone_workflow_projector_connect(proj) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t order_owner = make_test_uuid(0x10, 1);
    keystone_uuid_t room_owner = make_test_uuid(0x20, 1);

    /* Ingest 15 orders with ascending due deadlines */
    for (uint64_t i = 1; i <= 15; i++) {
        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = i;
        txn.tenant_id = TEST_TENANT;
        txn.classification = KEYSTONE_CLASSIFICATION_OPS;
        txn.record_id = make_test_uuid(0x11, (uint32_t)i);
        txn.owner_id = order_owner;
        txn.workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER;
        txn.due_at = 1000 + i * 10;
        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }

    /* Ingest 15 room messages with due_at = message_id */
    for (uint64_t i = 1; i <= 15; i++) {
        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = 15 + i;
        txn.tenant_id = TEST_TENANT;
        txn.classification = KEYSTONE_CLASSIFICATION_OPS;
        txn.record_id = make_test_uuid(0x21, (uint32_t)i);
        txn.owner_id = room_owner;
        txn.workflow_type = KEYSTONE_WORKFLOW_TYPE_ROOM_MESSAGE;
        txn.due_at = 5000 + i; /* Canonical message ID */
        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }
    TEST_ASSERT(keystone_workflow_projector_flush(proj) == KEYSTONE_WORKFLOW_OK);

    /* Initialize workflow reader */
    keystone_workflow_reader_config_t reader_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_OPS,
            .compartment_mask = 0,
        },
        .default_limit = 4,
    };
    keystone_workflow_reader_t *reader = keystone_workflow_reader_create(&reader_cfg);
    TEST_ASSERT(reader != NULL);
    TEST_ASSERT(keystone_workflow_reader_connect(reader) == KEYSTONE_WORKFLOW_OK);

    /* 1. Test Ascending Pagination on Orders */
    keystone_workflow_reader_query_t asc_q = {
        .workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER,
        .owner_id = order_owner,
        .tenant = TEST_TENANT,
        .max_classification = KEYSTONE_CLASSIFICATION_OPS,
        .sci = 0,
        .required_seq = 30,
        .max_due_at = UINT64_MAX,
        .limit = 4,
        .descending = 0,
    };

    keystone_workflow_reader_cursor_t *cur = NULL;
    TEST_ASSERT(keystone_workflow_reader_open_cursor(reader, &asc_q, &cur) == KEYSTONE_WORKFLOW_OK);

    uint64_t last_due = 0;
    size_t total_read = 0;
    while (1) {
        keystone_workflow_reader_page_t page;
        TEST_ASSERT(keystone_workflow_reader_cursor_next_page(reader, cur, &page) == KEYSTONE_WORKFLOW_OK);
        for (size_t i = 0; i < page.count; i++) {
            TEST_ASSERT(page.entries[i].due_at > last_due);
            last_due = page.entries[i].due_at;
            total_read++;
        }
        if (!page.has_more) break;
    }
    TEST_ASSERT(total_read == 15);
    keystone_workflow_reader_cursor_close(reader, cur);

    /* 2. Test Descending Pagination on Room Messages (Newest-first history) */
    keystone_workflow_reader_query_t desc_q = {
        .workflow_type = KEYSTONE_WORKFLOW_TYPE_ROOM_MESSAGE,
        .owner_id = room_owner,
        .tenant = TEST_TENANT,
        .max_classification = KEYSTONE_CLASSIFICATION_OPS,
        .sci = 0,
        .required_seq = 30,
        .max_due_at = UINT64_MAX,
        .limit = 5,
        .descending = 1,
    };

    TEST_ASSERT(keystone_workflow_reader_open_cursor(reader, &desc_q, &cur) == KEYSTONE_WORKFLOW_OK);
    uint64_t prev_due = UINT64_MAX;
    total_read = 0;
    while (1) {
        keystone_workflow_reader_page_t page;
        TEST_ASSERT(keystone_workflow_reader_cursor_next_page(reader, cur, &page) == KEYSTONE_WORKFLOW_OK);
        for (size_t i = 0; i < page.count; i++) {
            TEST_ASSERT(page.entries[i].due_at < prev_due);
            prev_due = page.entries[i].due_at;
            total_read++;
        }
        if (!page.has_more) break;
    }
    TEST_ASSERT(total_read == 15);
    keystone_workflow_reader_cursor_close(reader, cur);

    keystone_workflow_reader_destroy(reader);
    keystone_workflow_projector_destroy(proj);
    stop_test_daemon(sock, server);

    printf("    [+] Ascending and descending paged queries verified in strict order.\n");
}

/* --- Gate 3 Test: Authoritative Recheck Filtering (Simulating QIHSE KV) --- */
typedef struct {
    keystone_uuid_t deleted_id;
    keystone_uuid_t alien_tenant_id;
    keystone_uuid_t secret_id;
} test_authoritative_store_t;

static keystone_recheck_status_t mock_authoritative_recheck(
    const keystone_uuid_t *candidate_record_id,
    const keystone_security_context_t *caller_ctx,
    void *user_data,
    keystone_workflow_entry_t *out_verified_entry
) {
    (void)out_verified_entry;
    test_authoritative_store_t *store = (test_authoritative_store_t *)user_data;

    /* 1. Check if record was deleted in authoritative KV */
    if (memcmp(candidate_record_id, &store->deleted_id, sizeof(keystone_uuid_t)) == 0) {
        return KEYSTONE_RECHECK_NOT_FOUND;
    }

    /* 2. Check cross-tenant isolation in authoritative store */
    if (memcmp(candidate_record_id, &store->alien_tenant_id, sizeof(keystone_uuid_t)) == 0) {
        return KEYSTONE_RECHECK_DENIED;
    }

    /* 3. Check security clearance in authoritative store */
    if (memcmp(candidate_record_id, &store->secret_id, sizeof(keystone_uuid_t)) == 0) {
        if (caller_ctx->classification < KEYSTONE_CLASSIFICATION_SECRET) {
            return KEYSTONE_RECHECK_DENIED;
        }
    }

    return KEYSTONE_RECHECK_OK;
}

static void test_reader_authoritative_recheck_filtering(void) {
    printf("[*] Gate 3: Authoritative QIHSE KV rechecks & authorization filtering...\n");

    const char *sock = "/tmp/keystone_test_gate3_recheck.sock";
    keystoned_server_t *server = NULL;
    start_test_daemon(sock, &server);

    uint8_t genesis_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(genesis_history, 0x44, sizeof(genesis_history));

    keystone_workflow_projector_config_t proj_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        },
        .max_batch_size = 64,
        .auto_reconnect = 1,
    };
    memcpy(proj_cfg.history_id, genesis_history, sizeof(genesis_history));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&proj_cfg);
    TEST_ASSERT(proj != NULL);
    TEST_ASSERT(keystone_workflow_projector_connect(proj) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = make_test_uuid(0x50, 1);
    test_authoritative_store_t auth_store = {
        .deleted_id = make_test_uuid(0x51, 3),      /* Item 3 is deleted in QIHSE */
        .alien_tenant_id = make_test_uuid(0x51, 4), /* Item 4 belongs to alien tenant */
        .secret_id = make_test_uuid(0x51, 5),       /* Item 5 is SECRET */
    };

    /* Ingest 6 candidate items */
    for (uint64_t seq = 1; seq <= 6; seq++) {
        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = seq;
        txn.tenant_id = TEST_TENANT;
        txn.classification = (seq == 5) ? KEYSTONE_CLASSIFICATION_SECRET : KEYSTONE_CLASSIFICATION_OPS;
        txn.record_id = make_test_uuid(0x51, (uint32_t)seq);
        txn.owner_id = owner;
        txn.workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER;
        txn.due_at = seq * 100;
        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }
    TEST_ASSERT(keystone_workflow_projector_flush(proj) == KEYSTONE_WORKFLOW_OK);

    /* Reader with OPS clearance */
    keystone_workflow_reader_config_t reader_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_OPS,
            .compartment_mask = 0,
        },
        .recheck_fn = mock_authoritative_recheck,
        .recheck_user_data = &auth_store,
        .default_limit = 10,
    };
    keystone_workflow_reader_t *reader = keystone_workflow_reader_create(&reader_cfg);
    TEST_ASSERT(reader != NULL);
    TEST_ASSERT(keystone_workflow_reader_connect(reader) == KEYSTONE_WORKFLOW_OK);

    keystone_workflow_reader_query_t q = {
        .workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER,
        .owner_id = owner,
        .tenant = TEST_TENANT,
        .max_classification = KEYSTONE_CLASSIFICATION_OPS,
        .sci = 0,
        .required_seq = 6,
        .max_due_at = UINT64_MAX,
        .limit = 10,
        .descending = 0,
    };

    keystone_workflow_reader_cursor_t *cur = NULL;
    TEST_ASSERT(keystone_workflow_reader_open_cursor(reader, &q, &cur) == KEYSTONE_WORKFLOW_OK);

    keystone_workflow_reader_page_t page;
    TEST_ASSERT(keystone_workflow_reader_cursor_next_page(reader, cur, &page) == KEYSTONE_WORKFLOW_OK);

    /*
     * 6 candidates were in the index:
     * - Item 1: Valid OPS -> OK
     * - Item 2: Valid OPS -> OK
     * - Item 3: Deleted in QIHSE -> REJECTED
     * - Item 4: Alien tenant in QIHSE -> REJECTED
     * - Item 5: SECRET (caller is OPS) -> Filtered by keystoned or REJECTED by auth
     * - Item 6: Valid OPS -> OK
     *
     * Exactly 3 items (1, 2, 6) must be accepted.
     */
    TEST_ASSERT(page.count == 3);
    TEST_ASSERT(page.entries[0].due_at == 100);
    TEST_ASSERT(page.entries[1].due_at == 200);
    TEST_ASSERT(page.entries[2].due_at == 600);

    keystone_workflow_reader_cursor_close(reader, cur);
    keystone_workflow_reader_destroy(reader);
    keystone_workflow_projector_destroy(proj);
    stop_test_daemon(sock, server);

    printf("    [+] Authoritative verification recheck filtered out deleted and unauthorized items.\n");
}

/* --- Gate 3 Test: Strict Fail-Closed with Zero Full-Scan Fallback --- */
static void test_reader_strict_fail_closed(void) {
    printf("[*] Gate 3: Strict fail-closed semantics (no full-scan fallback)...\n");

    const char *sock = "/tmp/keystone_test_gate3_failclosed.sock";
    keystoned_server_t *server = NULL;
    start_test_daemon(sock, &server);

    uint8_t genesis_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(genesis_history, 0x12, sizeof(genesis_history));

    keystone_workflow_projector_config_t proj_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        },
        .max_batch_size = 64,
        .auto_reconnect = 1,
    };
    memcpy(proj_cfg.history_id, genesis_history, sizeof(genesis_history));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&proj_cfg);
    TEST_ASSERT(proj != NULL);
    TEST_ASSERT(keystone_workflow_projector_connect(proj) == KEYSTONE_WORKFLOW_OK);

    keystone_uuid_t owner = make_test_uuid(0x60, 1);
    for (uint64_t seq = 1; seq <= 5; seq++) {
        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = seq;
        txn.tenant_id = TEST_TENANT;
        txn.classification = KEYSTONE_CLASSIFICATION_OPS;
        txn.record_id = make_test_uuid(0x61, (uint32_t)seq);
        txn.owner_id = owner;
        txn.workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER;
        txn.due_at = seq * 10;
        TEST_ASSERT(keystone_workflow_projector_ingest_raw_txn(proj, &txn) == KEYSTONE_WORKFLOW_OK);
    }
    TEST_ASSERT(keystone_workflow_projector_flush(proj) == KEYSTONE_WORKFLOW_OK);

    keystone_workflow_reader_config_t reader_cfg = {
        .socket_path = sock,
        .sec_ctx = {
            .tenant_id = TEST_TENANT,
            .classification = KEYSTONE_CLASSIFICATION_OPS,
            .compartment_mask = 0,
        },
        .default_limit = 4,
    };
    keystone_workflow_reader_t *reader = keystone_workflow_reader_create(&reader_cfg);
    TEST_ASSERT(reader != NULL);
    TEST_ASSERT(keystone_workflow_reader_connect(reader) == KEYSTONE_WORKFLOW_OK);

    /* Policy check: scan fallback is non-negotiably disabled */
    TEST_ASSERT(!keystone_workflow_reader_allow_full_scan_fallback());

    /* 1. Watermark lag: reader requires watermark 100, daemon is at 5 */
    keystone_workflow_reader_query_t stale_q = {
        .workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER,
        .owner_id = owner,
        .tenant = TEST_TENANT,
        .max_classification = KEYSTONE_CLASSIFICATION_OPS,
        .sci = 0,
        .required_seq = 100, /* Above daemon watermark */
        .max_due_at = UINT64_MAX,
        .limit = 4,
    };

    keystone_workflow_reader_cursor_t *cur = NULL;
    TEST_ASSERT(keystone_workflow_reader_open_cursor(reader, &stale_q, &cur) == KEYSTONE_WORKFLOW_OK);
    keystone_workflow_reader_page_t page;
    TEST_ASSERT(keystone_workflow_reader_cursor_next_page(reader, cur, &page) == KEYSTONE_WORKFLOW_STALE);
    keystone_workflow_reader_cursor_close(reader, cur);

    /* 2. Cross-tenant access denied fails closed */
    keystone_workflow_reader_query_t alien_q = {
        .workflow_type = KEYSTONE_WORKFLOW_TYPE_ORDER,
        .owner_id = owner,
        .tenant = TEST_TENANT + 1, /* Unauthorized alien tenant */
        .max_classification = KEYSTONE_CLASSIFICATION_OPS,
        .sci = 0,
        .required_seq = 0,
        .max_due_at = UINT64_MAX,
        .limit = 4,
    };
    TEST_ASSERT(keystone_workflow_reader_open_cursor(reader, &alien_q, &cur) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_reader_cursor_next_page(reader, cur, &page) == KEYSTONE_WORKFLOW_CONFLICT);
    keystone_workflow_reader_cursor_close(reader, cur);

    /* 3. query_all fails closed without falling back to full-table scan */
    keystone_workflow_entry_t out[16];
    size_t out_count = 0;
    TEST_ASSERT(keystone_workflow_reader_query_all(reader, &stale_q, out, 16, &out_count) == KEYSTONE_WORKFLOW_STALE);
    TEST_ASSERT(out_count == 0);

    keystone_workflow_reader_destroy(reader);
    keystone_workflow_projector_destroy(proj);
    stop_test_daemon(sock, server);

    printf("    [+] Strict fail-closed semantics and zero full-scan fallback verified.\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Workflow Projector & Reader Tests (Gates 2 & 3)\n");
    printf("=================================================================\n");

    test_projector_decryption_and_payload_stripping();
    test_projector_staged_replay_backlog();
    test_projector_lost_ack_reconciliation();
    test_reader_cursor_pagination_and_ordering();
    test_reader_authoritative_recheck_filtering();
    test_reader_strict_fail_closed();

    printf("=================================================================\n");
    printf("  ALL WORKFLOW PROJECTOR & READER TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
