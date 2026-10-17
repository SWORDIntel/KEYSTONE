/*
 * KEYSTONE Application Workflow Reader with Authoritative Recheck (Gate 3)
 *
 * High-level application query helper executing paged cursor queries against
 * keystoned with mandatory authoritative view rechecks (simulating QIHSE KV)
 * and strict fail-closed behavior with zero un-indexed full-table scan fallback.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#ifndef KEYSTONE_WORKFLOW_READER_H
#define KEYSTONE_WORKFLOW_READER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "keystone_federation.h"
#include "keystone_workflow_index.h"
#include "keystoned.h"

/* Status of the authoritative view recheck (simulating QIHSE KV store) */
typedef enum {
    KEYSTONE_RECHECK_OK         = 0,  /* Verified: row exists, tenant matches, clearance valid */
    KEYSTONE_RECHECK_NOT_FOUND  = 1,  /* Row deleted, purged, or missing from authoritative store */
    KEYSTONE_RECHECK_DENIED     = 2,  /* Principal lacks required clearance or compartment */
    KEYSTONE_RECHECK_STALE      = 3,  /* Authoritative store is lagging or commit is pending */
    KEYSTONE_RECHECK_ERROR      = 4   /* IO or unexpected failure */
} keystone_recheck_status_t;

/*
 * Authoritative verification callback.
 * For every candidate record UUID returned by KEYSTONE's index, the reader
 * invokes this callback to verify existence, tenant ownership, and clearance.
 *
 * An authoritative copy of the metadata entry may optionally be populated in
 * out_verified_entry.
 */
typedef keystone_recheck_status_t (*keystone_workflow_authoritative_recheck_fn)(
    const keystone_uuid_t *candidate_record_id,
    const keystone_security_context_t *caller_ctx,
    void *user_data,
    keystone_workflow_entry_t *out_verified_entry
);

/* Configuration for application workflow reader */
typedef struct {
    const char *socket_path;
    keystone_security_context_t sec_ctx;
    keystone_workflow_authoritative_recheck_fn recheck_fn;
    void *recheck_user_data;
    uint32_t default_limit;           /* 1..KEYSTONE_WORKFLOW_PAGE_MAX (default 32) */
} keystone_workflow_reader_config_t;

/* High-level workflow query specification */
typedef struct {
    uint32_t workflow_type;           /* keystone_workflow_app_type_t */
    keystone_uuid_t owner_id;         /* Exact owner/room/channel UUID */
    uint32_t tenant;                  /* Exact tenant ID */
    uint32_t max_classification;      /* Clearance cap */
    uint32_t sci;                     /* Authorized compartment mask */
    uint64_t required_seq;            /* Minimum committed sequence watermark */
    uint64_t max_due_at;              /* Inclusive deadline bound; UINT64_MAX for unbounded */
    uint32_t limit;                   /* Items per page (1..256) */
    uint32_t descending;              /* 0 = ascending due_at, 1 = descending (e.g. room messages) */
} keystone_workflow_reader_query_t;

/* Output page of verified records */
typedef struct {
    keystone_workflow_entry_t entries[KEYSTONE_WORKFLOW_PAGE_MAX];
    size_t count;                     /* Number of authorized & verified entries in this page */
    size_t candidates_evaluated;      /* Number of index candidates inspected */
    size_t candidates_rejected;       /* Candidates dropped due to deletion or lack of auth */
    uint64_t watermark;               /* Bound watermark for this pagination stream */
    uint32_t has_more;                /* 1 if subsequent pages exist */
} keystone_workflow_reader_page_t;

/* Opaque reader and cursor handles */
typedef struct keystone_workflow_reader keystone_workflow_reader_t;
typedef struct keystone_workflow_reader_cursor keystone_workflow_reader_cursor_t;

/*
 * Create an application workflow reader.
 */
keystone_workflow_reader_t *keystone_workflow_reader_create(
    const keystone_workflow_reader_config_t *config
);

/*
 * Destroy the reader, closing sockets and freeing memory.
 */
void keystone_workflow_reader_destroy(keystone_workflow_reader_t *reader);

/*
 * Connect to keystoned and bind the caller's principal.
 */
int keystone_workflow_reader_connect(keystone_workflow_reader_t *reader);

/*
 * Disconnect from keystoned.
 */
void keystone_workflow_reader_disconnect(keystone_workflow_reader_t *reader);

/*
 * Open a paged cursor for the given workflow query.
 * Binds to the snapshot watermark returned on the first page.
 * Returns KEYSTONE_WORKFLOW_OK on success, or:
 *   KEYSTONE_WORKFLOW_STALE if watermark < required_seq
 *   KEYSTONE_WORKFLOW_CONFLICT if permission is denied
 */
keystone_workflow_result_t keystone_workflow_reader_open_cursor(
    keystone_workflow_reader_t *reader,
    const keystone_workflow_reader_query_t *query,
    keystone_workflow_reader_cursor_t **out_cursor
);

/*
 * Fetch the next page of verified records from the open cursor.
 * Performs authoritative rechecks on every candidate returned by KEYSTONE.
 *
 * If keystoned returns STALE or the authoritative check detects stale data,
 * strictly fails closed with KEYSTONE_WORKFLOW_STALE without falling back
 * to any full-scan.
 */
keystone_workflow_result_t keystone_workflow_reader_cursor_next_page(
    keystone_workflow_reader_t *reader,
    keystone_workflow_reader_cursor_t *cursor,
    keystone_workflow_reader_page_t *out_page
);

/*
 * Close and free an active query cursor.
 */
void keystone_workflow_reader_cursor_close(
    keystone_workflow_reader_t *reader,
    keystone_workflow_reader_cursor_t *cursor
);

/*
 * Convenience function: executes cursor pagination to completion and collects
 * all verified entries into out_entries up to capacity.
 * Fails closed on any inconsistency; never falls back to un-indexed scan.
 */
keystone_workflow_result_t keystone_workflow_reader_query_all(
    keystone_workflow_reader_t *reader,
    const keystone_workflow_reader_query_t *query,
    keystone_workflow_entry_t *out_entries,
    size_t capacity,
    size_t *out_count
);

/*
 * Policy assertion: scan-dependent readers NEVER allow un-indexed full-table
 * scan fallback. This function returns false under all conditions.
 */
static inline bool keystone_workflow_reader_allow_full_scan_fallback(void) {
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_WORKFLOW_READER_H */
