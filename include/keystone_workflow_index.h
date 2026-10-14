/* Opaque, metadata-only workflow candidate index.
 * Licensed under SRSL-1.0; see the repository LICENSE. */
#ifndef KEYSTONE_WORKFLOW_INDEX_H
#define KEYSTONE_WORKFLOW_INDEX_H

#include "keystone_federation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEYSTONE_WORKFLOW_PAGE_MAX 256u
#define KEYSTONE_WORKFLOW_BATCH_MAX 4096u
#define KEYSTONE_WORKFLOW_HISTORY_ID_SIZE 32u

typedef struct keystone_workflow_index keystone_workflow_index_t;

typedef enum {
    KEYSTONE_WORKFLOW_OK = 0,
    KEYSTONE_WORKFLOW_BAD_ARGUMENT = -1,
    KEYSTONE_WORKFLOW_NO_MEMORY = -2,
    KEYSTONE_WORKFLOW_GAP = -3,
    KEYSTONE_WORKFLOW_DUPLICATE = -4,
    KEYSTONE_WORKFLOW_CONFLICT = -5,
    KEYSTONE_WORKFLOW_STALE = -6,
    KEYSTONE_WORKFLOW_IO_ERROR = -7,
    KEYSTONE_WORKFLOW_CORRUPT = -8
} keystone_workflow_result_t;

/* No payload, source key, path, plaintext, or content-derived token is accepted. */
typedef struct {
    keystone_uuid_t record_id;
    keystone_uuid_t owner_id;
    uint32_t workflow_type;
    uint32_t tenant;
    uint32_t classification;
    uint32_t sci;             /* Compartment bit mask. */
    uint32_t flags;
    uint64_t due_at;          /* Non-content Unix deadline; zero if unscheduled. */
} keystone_workflow_entry_t;

typedef struct {
    uint64_t committed_seq;
    keystone_workflow_entry_t entry;
    uint32_t tombstone;       /* Zero = upsert; one = delete. */
} keystone_workflow_event_t;

typedef struct {
    uint32_t workflow_type;
    keystone_uuid_t owner_id; /* Exact; nil matches only nil-owned entries. */
    uint32_t tenant;          /* Exact tenant. */
    uint32_t max_classification;
    uint32_t sci;             /* Authorized compartment bits; entry bits must be a subset. */
    uint64_t required_seq;    /* Reject pages from a replica below this watermark. */
    uint64_t expected_watermark; /* Nonzero follow-up pages require an exact match. */
    uint64_t max_due_at;      /* Inclusive deadline; UINT64_MAX disables bound. */
    uint64_t cursor_due_at;   /* Last returned deadline in the selected direction. */
    keystone_uuid_t cursor;   /* Last returned record_id in the selected direction. */
    uint32_t cursor_valid;
    uint32_t limit;           /* 1..KEYSTONE_WORKFLOW_PAGE_MAX. */
    uint32_t descending;      /* Zero = ascending; one = descending (due_at, record_id). */
} keystone_workflow_query_t;

typedef struct {
    uint64_t watermark;
    uint64_t next_cursor_due_at;
    keystone_uuid_t next_cursor;
    uint32_t next_cursor_valid;
    uint32_t count;
    uint32_t has_more;
} keystone_workflow_page_t;

/* An empty index always starts at zero. Restore nonzero watermarks only from
 * a complete checkpoint, then replay the authenticated feed after it. */
keystone_workflow_index_t *keystone_workflow_index_create(uint64_t initial_watermark);
void keystone_workflow_index_destroy(keystone_workflow_index_t *index);

/* Save a bounded, versioned checkpoint in path's directory with fsync and
 * atomic rename. Refuses symlink directory components and a symlink target.
 * Load builds a new index; *out_index is NULL on every failure. A corrupt or
 * partial checkpoint never supplies a watermark. Loaded indexes are sealed
 * for offline inspection only until an authenticated committed-prefix proof
 * exists; bind_history alone cannot unseal a saved prefix. */
keystone_workflow_result_t keystone_workflow_index_save(
    keystone_workflow_index_t *index, const char *path);
keystone_workflow_result_t keystone_workflow_index_load(
    const char *path, keystone_workflow_index_t **out_index);

/* Bind to SHA-256 of the first committed source row payload. Repeated binds
 * must present the same identity. Loaded checkpoints reject this call until
 * a future API can verify their entire saved committed prefix. */
keystone_workflow_result_t keystone_workflow_index_bind_history(
    keystone_workflow_index_t *index,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE]);
int keystone_workflow_index_history_verified(keystone_workflow_index_t *index);

/* Convenience for a single event at exactly watermark+1. */
keystone_workflow_result_t keystone_workflow_index_ingest(
    keystone_workflow_index_t *index, const keystone_workflow_event_t *event);

/* Cover one contiguous feed interval, with at most
 * KEYSTONE_WORKFLOW_BATCH_MAX events. Source transaction indexes inside that
 * interval may be sparse. Events are ordered by nondecreasing committed_seq,
 * each in (from_exclusive, through_inclusive]. count=0 covers an interval with
 * no workflow changes. The batch and watermark publish atomically. */
keystone_workflow_result_t keystone_workflow_index_ingest_batch(
    keystone_workflow_index_t *index, uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events, size_t count);

/* Replay a large source backlog in bounded, volatile chunks. The first chunk
 * starts at the published watermark; each subsequent chunk starts at the
 * previous staged watermark. A chunk must end on a committed_seq boundary:
 * events at one committed_seq cannot be split across chunks. Pages, the
 * published watermark, and checkpoints continue to use the old complete
 * index until publish. A failed chunk leaves the stage unchanged. Ordinary
 * ingest is rejected while a stage is active. After restart, replay starts
 * again from the last published checkpoint. */
keystone_workflow_result_t keystone_workflow_index_stage_batch(
    keystone_workflow_index_t *index, uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events, size_t count);

/* Atomically expose all staged entries and exactly expected_target. No stage
 * or a mismatched target is an error; the caller must prove that target is a
 * complete authenticated source prefix before publishing. */
keystone_workflow_result_t keystone_workflow_index_publish(
    keystone_workflow_index_t *index, uint64_t expected_target);
keystone_workflow_result_t keystone_workflow_index_abort_stage(
    keystone_workflow_index_t *index);
/* Zero if no stage exists or its history is unverified. */
uint64_t keystone_workflow_index_stage_watermark(
    keystone_workflow_index_t *index);

/* Returns candidates only. The caller must re-read each ID from QIHSE and
 * enforce current authorization before revealing or acting on any record. */
keystone_workflow_result_t keystone_workflow_index_page(
    keystone_workflow_index_t *index, const keystone_workflow_query_t *query,
    keystone_workflow_entry_t *out_entries, size_t capacity,
    keystone_workflow_page_t *out_page);

uint64_t keystone_workflow_index_watermark(keystone_workflow_index_t *index);

#ifdef __cplusplus
}
#endif
#endif
