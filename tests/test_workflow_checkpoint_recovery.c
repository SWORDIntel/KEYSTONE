/*
 * KEYSTONE Workflow Candidate Index: Gate 1 Checkpoint & Recovery Verification
 *
 * Verifies Gate 1 of the Workflow Candidate Launch Gates:
 * - Authenticated checkpoint creation with HMAC-SHA256 seal
 * - Atomic persistence to disk and load verification
 * - Sealed checkpoint inspection state (queries rejected before unsealing)
 * - Cryptographic unsealing with matching history identity, watermark, and seal
 * - Tamper / corruption detection and rejection
 * - Stale watermark / conflict rejection on invalid unseal proofs
 *
 * SRSL-1.1 License.
 */

#include "keystone.h"
#include "keystone_workflow_index.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TENANT_ID 42u
#define WORKFLOW_TYPE 1u

static keystone_uuid_t make_uuid(int id) {
    char buf[40];
    snprintf(buf, sizeof(buf), "10000000-0000-0000-0000-%012d", id);
    keystone_uuid_t u;
    memset(&u, 0, sizeof(u));
    TEST_ASSERT(keystone_uuid_from_string(buf, &u) == 0);
    return u;
}

static keystone_workflow_event_t make_event(uint64_t seq, int id, uint64_t due) {
    keystone_workflow_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.committed_seq = seq;
    ev.entry.record_id = make_uuid(id);
    ev.entry.owner_id = make_uuid(999);
    ev.entry.workflow_type = WORKFLOW_TYPE;
    ev.entry.tenant = TENANT_ID;
    ev.entry.classification = 0;
    ev.entry.sci = 0;
    ev.entry.due_at = due;
    ev.tombstone = 0;
    return ev;
}

static void test_authenticated_checkpoint_save_and_unseal(void) {
    printf("[*] Gate 1: Authenticated checkpoint save, load, and unseal...\n");

    const char *ckpt_path = "/tmp/test_workflow_gate1.ckpt";
    unlink(ckpt_path);

    uint8_t secret_key[32] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
    };

    uint8_t history_id[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(history_id, 0x5a, sizeof(history_id));

    /* Create index and ingest events */
    keystone_workflow_index_t *orig = keystone_workflow_index_create(0);
    TEST_ASSERT(orig != NULL);
    TEST_ASSERT(keystone_workflow_index_bind_history(orig, history_id) == KEYSTONE_WORKFLOW_OK);

    keystone_workflow_event_t events[5];
    for (int i = 0; i < 5; ++i) {
        events[i] = make_event((uint64_t)(i + 1), i + 1, (uint64_t)((i + 1) * 100));
    }
    TEST_ASSERT(keystone_workflow_index_ingest_batch(orig, 0, 5, events, 5) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(orig) == 5);

    /* Compute seal */
    uint8_t expected_seal[32];
    TEST_ASSERT(keystone_workflow_index_compute_seal(orig, secret_key, expected_seal) == KEYSTONE_WORKFLOW_OK);

    /* Save authenticated checkpoint */
    TEST_ASSERT(keystone_workflow_index_save_authenticated(orig, ckpt_path, secret_key) == KEYSTONE_WORKFLOW_OK);
    keystone_workflow_index_destroy(orig);

    /* Load checkpoint into new index */
    keystone_workflow_index_t *restored = NULL;
    TEST_ASSERT(keystone_workflow_index_load(ckpt_path, &restored) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(restored != NULL);

    /* Verify loaded checkpoint is initially SEALED */
    TEST_ASSERT(keystone_workflow_index_is_sealed(restored) == 1);
    TEST_ASSERT(keystone_workflow_index_checkpoint_watermark(restored) == 5);
    TEST_ASSERT(keystone_workflow_index_watermark(restored) == 0); /* Visible watermark remains 0 while sealed */

    /* Attempt to query while sealed: must fail closed with KEYSTONE_WORKFLOW_STALE */
    keystone_workflow_query_t query;
    memset(&query, 0, sizeof(query));
    query.workflow_type = WORKFLOW_TYPE;
    query.owner_id = make_uuid(999);
    query.tenant = TENANT_ID;
    query.max_due_at = UINT64_MAX;
    query.limit = 10;
    keystone_workflow_entry_t out_entries[10];
    keystone_workflow_page_t out_page;
    TEST_ASSERT(keystone_workflow_index_page(restored, &query, out_entries, 10, &out_page)
                == KEYSTONE_WORKFLOW_STALE);

    /* Attempt unseal with wrong history ID: must fail with CONFLICT and stay sealed */
    uint8_t wrong_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(wrong_history, 0x99, sizeof(wrong_history));
    TEST_ASSERT(keystone_workflow_index_unseal_with_proof(restored, wrong_history, 5, expected_seal)
                == KEYSTONE_WORKFLOW_CONFLICT);
    TEST_ASSERT(keystone_workflow_index_is_sealed(restored) == 1);

    /* Attempt unseal with wrong watermark: must fail with CONFLICT */
    TEST_ASSERT(keystone_workflow_index_unseal_with_proof(restored, history_id, 999, expected_seal)
                == KEYSTONE_WORKFLOW_CONFLICT);
    TEST_ASSERT(keystone_workflow_index_is_sealed(restored) == 1);

    /* Attempt unseal with tampered seal: must fail with CONFLICT */
    uint8_t bad_seal[32];
    memcpy(bad_seal, expected_seal, 32);
    bad_seal[0] ^= 0xff;
    TEST_ASSERT(keystone_workflow_index_unseal_with_proof(restored, history_id, 5, bad_seal)
                == KEYSTONE_WORKFLOW_CONFLICT);
    TEST_ASSERT(keystone_workflow_index_is_sealed(restored) == 1);

    /* Unseal with genuine proof */
    TEST_ASSERT(keystone_workflow_index_unseal_with_proof(restored, history_id, 5, expected_seal)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_is_sealed(restored) == 0);
    TEST_ASSERT(keystone_workflow_index_watermark(restored) == 5);

    /* Query now succeeds immediately */
    TEST_ASSERT(keystone_workflow_index_page(restored, &query, out_entries, 10, &out_page)
                == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(out_page.count == 5);
    TEST_ASSERT(out_page.watermark == 5);

    /* Verify incremental ingest can continue seamlessly from restored watermark */
    keystone_workflow_event_t next_ev = make_event(6, 6, 600);
    TEST_ASSERT(keystone_workflow_index_ingest_batch(restored, 5, 6, &next_ev, 1) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_watermark(restored) == 6);

    keystone_workflow_index_destroy(restored);
    unlink(ckpt_path);
    printf("    [+] Authenticated checkpoint save, load, and unseal verified.\n");
}

static void test_tampered_checkpoint_rejection(void) {
    printf("[*] Gate 1: Tampered and corrupted checkpoint rejection...\n");

    const char *ckpt_path = "/tmp/test_workflow_corrupt.ckpt";
    unlink(ckpt_path);

    uint8_t history_id[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(history_id, 0x33, sizeof(history_id));

    keystone_workflow_index_t *orig = keystone_workflow_index_create(0);
    TEST_ASSERT(keystone_workflow_index_bind_history(orig, history_id) == KEYSTONE_WORKFLOW_OK);
    keystone_workflow_event_t ev = make_event(1, 1, 100);
    TEST_ASSERT(keystone_workflow_index_ingest(orig, &ev) == KEYSTONE_WORKFLOW_OK);
    TEST_ASSERT(keystone_workflow_index_save(orig, ckpt_path) == KEYSTONE_WORKFLOW_OK);
    keystone_workflow_index_destroy(orig);

    /* Tamper with file contents: flip a byte in the entry row */
    FILE *f = fopen(ckpt_path, "r+b");
    TEST_ASSERT(f != NULL);
    fseek(f, 80, SEEK_SET);
    int byte = fgetc(f);
    fseek(f, 80, SEEK_SET);
    fputc(byte ^ 0x01, f);
    fclose(f);

    /* Loading tampered checkpoint must fail with CORRUPT and return NULL */
    keystone_workflow_index_t *loaded = NULL;
    TEST_ASSERT(keystone_workflow_index_load(ckpt_path, &loaded) == KEYSTONE_WORKFLOW_CORRUPT);
    TEST_ASSERT(loaded == NULL);

    unlink(ckpt_path);
    printf("    [+] Tampered checkpoint cleanly rejected (CRC mismatch).\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Workflow Candidate Index: Gate 1 Checkpoint Tests    \n");
    printf("=================================================================\n");

    test_authenticated_checkpoint_save_and_unseal();
    test_tampered_checkpoint_rejection();

    printf("=================================================================\n");
    printf("  GATE 1 CHECKPOINT & RECOVERY: ALL TESTS PASSED (100%% GREEN)    \n");
    printf("=================================================================\n");
    return 0;
}
