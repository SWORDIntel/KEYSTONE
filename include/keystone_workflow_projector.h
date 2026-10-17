/*
 * KEYSTONE Trusted Workflow Projector (Gate 2)
 *
 * Ingests raw transaction streams (simulating QIHSE.TXN.READ), decrypts and
 * parses application transactions within its boundary, strips all payload
 * bodies, paths, text, and search tokens, and emits strictly opaque metadata
 * intervals into keystoned.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#ifndef KEYSTONE_WORKFLOW_PROJECTOR_H
#define KEYSTONE_WORKFLOW_PROJECTOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "keystone_federation.h"
#include "keystone_workflow_index.h"
#include "keystoned.h"

/* Canonical application workflow types recognized by the projector boundary */
typedef enum {
    KEYSTONE_WORKFLOW_TYPE_UNKNOWN        = 0,
    KEYSTONE_WORKFLOW_TYPE_ORDER          = 1,
    KEYSTONE_WORKFLOW_TYPE_PAYMENT        = 2,
    KEYSTONE_WORKFLOW_TYPE_CONVERSION     = 3,
    KEYSTONE_WORKFLOW_TYPE_REVIEW         = 4,
    KEYSTONE_WORKFLOW_TYPE_CATALOG        = 5,
    KEYSTONE_WORKFLOW_TYPE_PENDING_WORK   = 6,
    KEYSTONE_WORKFLOW_TYPE_VENDOR_THREAD  = 7,
    KEYSTONE_WORKFLOW_TYPE_ROOM_MESSAGE   = 8
} keystone_workflow_app_type_t;

/*
 * Simulated raw application transaction record outside KEYSTONE.
 * Represents an entry produced by QIHSE committed transactions (QIHSE.TXN.READ).
 * Sensitive payload bodies, customer tokens, file paths, and plaintext remain
 * strictly isolated inside the projector and are NEVER sent to KEYSTONE.
 */
typedef struct {
    uint64_t committed_seq;      /* Monotonic committed sequence number in QIHSE */
    uint32_t tenant_id;          /* Application tenant ID */
    uint32_t classification;     /* Row classification level */
    uint32_t sci_mask;           /* Compartment bit mask */
    keystone_uuid_t record_id;   /* Opaque record UUID */
    keystone_uuid_t owner_id;    /* Opaque owner UUID */
    uint32_t workflow_type;      /* keystone_workflow_app_type_t */
    uint32_t is_tombstone;       /* 0 = upsert, 1 = delete */
    uint64_t due_at;             /* Non-content Unix deadline or canonical message ID */
    uint32_t flags;              /* e.g. KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE */

    /* Application payload (outside KEYSTONE boundary) */
    int is_encrypted;            /* 1 if payload is encrypted */
    uint8_t encryption_key[32];  /* Decryption key (boundary-local) */
    const void *payload;         /* Encrypted or raw application payload */
    size_t payload_len;          /* Byte length of payload */
} keystone_qihse_txn_record_t;

/* Configuration for the trusted workflow projector */
typedef struct {
    const char *socket_path;                   /* Path to keystoned Unix socket */
    keystone_security_context_t sec_ctx;      /* Dedicated projector principal (full clearance) */
    uint8_t history_id[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE]; /* Genesis history identity */
    uint32_t max_batch_size;                   /* Capped at KEYSTONE_WORKFLOW_BATCH_MAX (4096) */
    int auto_reconnect;                        /* Automatically reconnect on socket error */
} keystone_workflow_projector_config_t;

/* Opaque projector handle */
typedef struct keystone_workflow_projector keystone_workflow_projector_t;

/*
 * Create and initialize a workflow projector instance.
 * Returns NULL on allocation or configuration failure.
 */
keystone_workflow_projector_t *keystone_workflow_projector_create(
    const keystone_workflow_projector_config_t *config
);

/*
 * Destroy a workflow projector instance, closing connections and freeing memory.
 */
void keystone_workflow_projector_destroy(keystone_workflow_projector_t *proj);

/*
 * Connect to the keystoned Unix domain socket and bind security principal.
 * Returns 0 on success, negative error code on failure.
 */
int keystone_workflow_projector_connect(keystone_workflow_projector_t *proj);

/*
 * Disconnect from keystoned.
 */
void keystone_workflow_projector_disconnect(keystone_workflow_projector_t *proj);

/*
 * Bind the genesis history identity to keystoned.
 * Must be executed before any metadata ingestion.
 * Returns KEYSTONE_WORKFLOW_OK on success.
 */
int keystone_workflow_projector_bind_history(
    keystone_workflow_projector_t *proj,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE]
);

/*
 * Query current committed watermark from keystoned.
 */
int keystone_workflow_projector_get_watermark(
    keystone_workflow_projector_t *proj,
    uint64_t *out_watermark
);

/*
 * Ingest a raw transaction record (simulating QIHSE.TXN.READ).
 * Decrypts and parses the record inside the projector boundary, verifies
 * structure, completely strips all payload bodies/paths/search tokens,
 * and buffers the resulting opaque metadata event into the staging batch.
 *
 * Automatically flushes when max_batch_size is reached.
 * Returns KEYSTONE_WORKFLOW_OK on success, negative error code on failure.
 */
int keystone_workflow_projector_ingest_raw_txn(
    keystone_workflow_projector_t *proj,
    const keystone_qihse_txn_record_t *txn
);

/*
 * Ingest multiple raw transaction records in sequence.
 */
int keystone_workflow_projector_ingest_raw_txns(
    keystone_workflow_projector_t *proj,
    const keystone_qihse_txn_record_t *txns,
    size_t count
);

/*
 * Explicitly flush any buffered metadata events to keystoned.
 * Emits via keystoned_client_workflow_apply.
 * Returns KEYSTONE_WORKFLOW_OK on success.
 */
int keystone_workflow_projector_flush(keystone_workflow_projector_t *proj);

/*
 * Directly apply an explicit batch of pre-stripped metadata events
 * (at most KEYSTONE_WORKFLOW_BATCH_MAX) to keystoned.
 */
int keystone_workflow_projector_apply_batch(
    keystone_workflow_projector_t *proj,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events,
    size_t count
);

/*
 * Replay a large backlog (> 4096 events) using bounded staged batches
 * and atomically publish at expected_target.
 * Replay chunks are capped at 4096 events and aligned to commit boundaries.
 * In case of chunk failure, aborts the volatile stage and fails closed.
 */
int keystone_workflow_projector_stage_and_publish(
    keystone_workflow_projector_t *proj,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events,
    size_t count
);

/*
 * Lost-ACK and reconnect reconciliation:
 * Queries keystoned watermark and reconciles state. If keystoned is already at
 * expected_target_seq, the apply succeeded prior to ACK loss. If keystoned is
 * behind, flags that replay is required.
 */
int keystone_workflow_projector_reconcile_lost_ack(
    keystone_workflow_projector_t *proj,
    uint64_t expected_target_seq,
    int *out_already_committed
);

/*
 * Decrypts or encrypts an application payload stream in-place or into out_buf.
 * Used internally by projector boundary for transaction simulation and verification.
 */
void keystone_workflow_projector_crypt_payload(
    const uint8_t key[32],
    uint64_t seq,
    const void *in,
    void *out,
    size_t len
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_WORKFLOW_PROJECTOR_H */
