/*
 * KEYSTONE Security-Aware Service Mode (keystoned)
 *
 * Dedicated unprivileged service communicating over Unix domain sockets (AF_UNIX).
 * Implements security-context partitioned queries, generation-keyed caching,
 * and atomic generation publication without read locks.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#ifndef KEYSTONED_H
#define KEYSTONED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include "keystone_federation.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"
#include "keystone_workflow_index.h"

#define KEYSTONED_DEFAULT_SOCKET "/run/keystone/keystoned.sock"
#define KEYSTONED_MAGIC 0x4B535356u /* 'KSSV' */
#define KEYSTONED_VERSION 1u

/* Maximum IPC payload length to protect against buffer exhaustion */
#define KEYSTONED_MAX_MSG_LEN (16u * 1024u * 1024u)

/* --- IPC Message Types --- */
typedef enum {
    KEYSTONED_MSG_PING                = 1,
    KEYSTONED_MSG_PONG                = 2,
    KEYSTONED_MSG_BIND_PRINCIPAL_REQ  = 6,
    KEYSTONED_MSG_BIND_PRINCIPAL_RESP = 7,
    KEYSTONED_MSG_EXACT_LOOKUP_REQ    = 10,
    KEYSTONED_MSG_EXACT_LOOKUP_RESP   = 11,
    KEYSTONED_MSG_TEMPORAL_RANGE_REQ  = 20,
    KEYSTONED_MSG_TEMPORAL_RANGE_RESP = 21,
    KEYSTONED_MSG_INGEST_REQ          = 30,
    KEYSTONED_MSG_INGEST_RESP         = 31,
    KEYSTONED_MSG_PUBLISH_GEN_REQ     = 40,
    KEYSTONED_MSG_PUBLISH_GEN_RESP    = 41,
    KEYSTONED_MSG_STATUS_REQ          = 50,
    KEYSTONED_MSG_STATUS_RESP         = 51,
    KEYSTONED_MSG_WORKFLOW_APPLY_REQ  = 60,
    KEYSTONED_MSG_WORKFLOW_APPLY_RESP = 61,
    KEYSTONED_MSG_WORKFLOW_PAGE_REQ   = 62,
    KEYSTONED_MSG_WORKFLOW_PAGE_RESP  = 63,
    KEYSTONED_MSG_WORKFLOW_WATERMARK_REQ = 64,
    KEYSTONED_MSG_WORKFLOW_WATERMARK_RESP = 65,
    KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_REQ = 66,
    KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_RESP = 67,
    KEYSTONED_MSG_WORKFLOW_STAGE_REQ = 68,
    KEYSTONED_MSG_WORKFLOW_STAGE_RESP = 69,
    KEYSTONED_MSG_WORKFLOW_PUBLISH_REQ = 70,
    KEYSTONED_MSG_WORKFLOW_PUBLISH_RESP = 71,
    KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_REQ = 72,
    KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_RESP = 73,
    KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_REQ = 74,
    KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_RESP = 75,
    KEYSTONED_MSG_ERROR               = 99
} keystoned_msg_type_t;

/* --- IPC Status Codes --- */
typedef enum {
    KEYSTONED_STATUS_OK               = 0,
    KEYSTONED_STATUS_NOT_FOUND        = 1,
    KEYSTONED_STATUS_DENIED           = 2,
    KEYSTONED_STATUS_STALE_GEN        = 3,
    KEYSTONED_STATUS_INVALID_ARG      = 4,
    KEYSTONED_STATUS_SERVER_ERROR     = 5,
    KEYSTONED_STATUS_CONFLICT         = 6,
    KEYSTONED_STATUS_GAP              = 7
} keystoned_status_t;

/* Local Unix-socket payloads. Source sequence intervals must be complete
 * QIHSE committed intervals, supplied by a trusted projector. The daemon
 * never receives QIHSE row contents or field keys. */
typedef struct {
    uint64_t from_exclusive;
    uint64_t through_inclusive;
    uint32_t count;
    uint32_t reserved;
    /* keystone_workflow_event_t events[count] follows */
} keystoned_workflow_apply_req_t;

typedef struct {
    keystone_workflow_page_t page;
    /* keystone_workflow_entry_t entries[page.count] follows */
} keystoned_workflow_page_resp_t;

/* --- Packed Wire IPC Message Header --- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                 /* KEYSTONED_MAGIC (0x4B535356) */
    uint32_t version;               /* KEYSTONED_VERSION (1) */
    uint32_t msg_type;              /* keystoned_msg_type_t */
    uint32_t status;                /* keystoned_status_t */
    uint32_t seq_id;                /* Request/Response sequence identifier */
    uint32_t payload_len;           /* Length of following payload bytes */
    keystone_security_context_t sec_ctx; /* Calling principal's security context */
    uint64_t index_generation;      /* Generation snapshot ID */
    uint32_t crc32;                 /* Header + payload CRC32 */
    uint32_t reserved;
} keystoned_msg_header_t;
#pragma pack(pop)

/* --- Security Clearance & Compartment Check --- */
static inline bool keystone_security_check(
    const keystone_security_context_t* caller,
    uint32_t record_classification,
    uint64_t record_compartment_mask
) {
    if (!caller) return false;

    /* 1. Clearance must be >= record classification */
    if (caller->classification < record_classification) {
        return false;
    }

    /* 2. All record compartments must be present in caller's compartment mask */
    if ((record_compartment_mask & ~caller->compartment_mask) != 0) {
        return false;
    }

    return true;
}

/* --- Server Configuration & Handle --- */
typedef struct keystoned_server keystoned_server_t;

typedef struct {
    const char* socket_path;
    size_t cache_capacity;
    uint64_t initial_fencing_epoch;
    int enable_security_audit;
} keystoned_config_t;

int keystoned_server_workflow_bind_history(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE],
    uint64_t* out_watermark
);

keystoned_server_t* keystoned_server_create(const keystoned_config_t* config);
void keystoned_server_destroy(keystoned_server_t* server);

int keystoned_server_start(keystoned_server_t* server);
void keystoned_server_stop(keystoned_server_t* server);

/* Atomic generation swap (double-buffered pointer swap without locking readers) */
int keystoned_server_publish_generation(
    keystoned_server_t* server,
    keystone_exact_index_t* new_exact,
    keystone_temporal_index_t* new_temporal,
    uint64_t new_generation
);

/* Direct in-process query methods (for embedded or daemon-internal dispatch) */
int keystoned_server_query_exact(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_uuid_t* id,
    keystone_exact_entry_t* out_entry
);

size_t keystoned_server_query_temporal(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_hlc_t* hmin,
    const keystone_hlc_t* hmax,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
);

int keystoned_server_ingest(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_federation_record_t* rec
);

int keystoned_server_workflow_apply(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t* events,
    size_t count,
    uint64_t* out_watermark
);

/* Stage bounded committed intervals without exposing them to readers.
 * Publish only at the exact final source sequence; staged state is volatile. */
int keystoned_server_workflow_stage(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t* events,
    size_t count,
    uint64_t* out_stage_watermark
);

int keystoned_server_workflow_publish(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    uint64_t expected_target,
    uint64_t* out_watermark
);

int keystoned_server_workflow_abort_stage(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
);

int keystoned_server_workflow_stage_watermark(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
);

int keystoned_server_workflow_page(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_workflow_query_t* query,
    keystone_workflow_entry_t* out_entries,
    size_t capacity,
    keystone_workflow_page_t* out_page
);

int keystoned_server_workflow_watermark(
    keystoned_server_t* server,
    uint64_t* out_watermark
);

int keystoned_server_bind_peer_principal(
    keystoned_server_t* server,
    uid_t uid,
    const keystone_security_context_t* sec_ctx
);

/* --- Client Handle & Methods --- */
typedef struct keystoned_client keystoned_client_t;

keystoned_client_t* keystoned_client_connect(const char* socket_path);
void keystoned_client_disconnect(keystoned_client_t* client);

int keystoned_client_ping(keystoned_client_t* client);

int keystoned_client_bind_principal(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx
);

int keystoned_client_exact_lookup(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_uuid_t* id,
    keystone_exact_entry_t* out_entry
);

int keystoned_client_temporal_range(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_hlc_t* hmin,
    const keystone_hlc_t* hmax,
    keystone_temporal_entry_t* out_entries,
    size_t max_results,
    size_t* out_count
);

int keystoned_client_ingest(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_federation_record_t* rec
);

int keystoned_client_workflow_apply(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t* events,
    size_t count,
    uint64_t* out_watermark
);

int keystoned_client_workflow_stage(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t* events,
    size_t count,
    uint64_t* out_stage_watermark
);

int keystoned_client_workflow_publish(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    uint64_t expected_target,
    uint64_t* out_watermark
);

int keystoned_client_workflow_abort_stage(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
);

int keystoned_client_workflow_stage_watermark(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
);

int keystoned_client_workflow_bind_history(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE],
    uint64_t* out_watermark
);

int keystoned_client_workflow_page(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_workflow_query_t* query,
    keystone_workflow_entry_t* out_entries,
    size_t capacity,
    keystone_workflow_page_t* out_page
);

int keystoned_client_workflow_watermark(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
);

int keystoned_client_get_status(
    keystoned_client_t* client,
    uint64_t* out_generation,
    uint64_t* out_epoch,
    keystone_hlc_t* out_hlc
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONED_H */
