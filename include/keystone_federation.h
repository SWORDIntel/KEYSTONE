/*
 * KEYSTONE Federation Wire Envelope & Ingestion Pipeline
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KEYSTONE_FEDERATION_H
#define KEYSTONE_FEDERATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Wire & Checkpoint Magic Identifiers --- */
#define KEYSTONE_FEDERATION_ENVELOPE_MAGIC 0x4B534645u /* 'KSFE' */
#define KEYSTONE_FEDERATION_ENVELOPE_VERSION 1u
#define KEYSTONE_CHECKPOINT_MAGIC 0x4B534643u          /* 'KSFC' */
#define KEYSTONE_CHECKPOINT_VERSION 1u

/* Maximum allowed serialized payload length (64 MiB limit against allocation bombs) */
#define KEYSTONE_MAX_PAYLOAD_LEN (64u * 1024u * 1024u)

/* --- Record Flags --- */
#define KEYSTONE_RECORD_FLAG_TOMBSTONE          (1u << 0)
#define KEYSTONE_RECORD_FLAG_SNAPSHOT           (1u << 1)
#define KEYSTONE_RECORD_FLAG_EVENT              (1u << 2)
#define KEYSTONE_RECORD_FLAG_TELEMETRY          (1u << 3)
#define KEYSTONE_RECORD_FLAG_AUDIT              (1u << 4)
#define KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE (1u << 5)
#define KEYSTONE_RECORD_FLAG_DERIVED            (1u << 6)
#define KEYSTONE_RECORD_FLAG_COMPACTED          (1u << 7)

/* --- Canonical 128-bit UUID Primitive --- */
typedef struct {
    uint8_t bytes[16];
} keystone_uuid_t;

/* QIHSE compatibility alias */
typedef keystone_uuid_t qihse_uuid_t;

/* --- Hybrid Logical Clock (HLC) Primitive --- */
typedef struct {
    uint64_t physical_ms;  /* Monotonic physical timestamp in milliseconds */
    uint32_t logical;      /* Logical sequence counter for same-ms events */
    uint32_t node_id;      /* Node identity tie-breaker */
} keystone_hlc_t;

/* QIHSE compatibility alias */
typedef keystone_hlc_t qihse_hlc_t;

/* --- Infrastructure Object Classification & Types --- */
typedef enum {
    KEYSTONE_CLASSIFICATION_UNCLASSIFIED = 0,
    KEYSTONE_CLASSIFICATION_OPS          = 1,
    KEYSTONE_CLASSIFICATION_RESTRICTED   = 2,
    KEYSTONE_CLASSIFICATION_SECRET       = 3,
    KEYSTONE_CLASSIFICATION_TOP_SECRET   = 4
} keystone_classification_t;

typedef enum {
    KEYSTONE_OBJ_UNKNOWN                 = 0,
    KEYSTONE_OBJ_NODE                    = 1,
    KEYSTONE_OBJ_VM                      = 2,
    KEYSTONE_OBJ_VOLUME                  = 3,
    KEYSTONE_OBJ_NETWORK                 = 4,
    KEYSTONE_OBJ_DEVICE                  = 5,
    KEYSTONE_OBJ_SERVICE_DOMAIN          = 6,
    KEYSTONE_OBJ_TEMPLATE                = 7,
    KEYSTONE_OBJ_SNAPSHOT                = 8,
    KEYSTONE_OBJ_SECURITY_DOMAIN         = 9,
    KEYSTONE_OBJ_INCIDENT                = 10,
    KEYSTONE_OBJ_LOG_STREAM              = 11,
    KEYSTONE_OBJ_CUSTOM                  = 255
} keystone_object_type_t;

typedef struct {
    uint32_t tenant_id;
    uint32_t classification;
    uint64_t compartment_mask;
} keystone_security_context_t;

/* --- Canonical Ingest Record Envelope --- */
typedef struct {
    keystone_uuid_t source_object_id;
    keystone_uuid_t source_event_id;
    keystone_uuid_t source_node_id;

    uint64_t source_generation;
    uint64_t fencing_epoch;
    keystone_hlc_t source_hlc;

    uint32_t tenant_id;
    uint32_t classification;
    uint32_t object_type;
    uint32_t flags;

    const void *payload;
    size_t payload_len;
} keystone_federation_record_t;

/* --- Persistent Checkpoint Structure --- */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t checkpoint_generation;
    uint64_t active_fencing_epoch;
    keystone_hlc_t last_applied_hlc;
    keystone_uuid_t last_event_id;
    uint64_t total_records_ingested;
    uint64_t total_tombstones_applied;
    uint64_t total_duplicates_suppressed;
    uint64_t total_stale_rejected;
    uint32_t crc32;
    uint32_t reserved;
} keystone_federation_checkpoint_t;

/* --- Ingestion Engine Status & Configuration --- */
typedef enum {
    KEYSTONE_INGEST_OK                  = 0,
    KEYSTONE_INGEST_DUPLICATE           = 1,
    KEYSTONE_INGEST_TOMBSTONE_APPLIED   = 2,
    KEYSTONE_INGEST_OUT_OF_ORDER        = 3,
    KEYSTONE_INGEST_STALE_GENERATION    = 4,
    KEYSTONE_INGEST_STALE_FENCING_EPOCH = 5,
    KEYSTONE_INGEST_ERR_INVALID_PARAM   = -1,
    KEYSTONE_INGEST_ERR_BUFFER_FULL     = -2,
    KEYSTONE_INGEST_ERR_ALLOC           = -3,
    KEYSTONE_INGEST_ERR_IO              = -4,
    KEYSTONE_INGEST_ERR_CORRUPT         = -5
} keystone_ingest_status_t;

typedef struct {
    size_t ring_buffer_capacity;     /* Capacity for event queue (0 for default 65536) */
    size_t dedup_window_capacity;    /* Size of dedup hash table (0 for default 131072) */
    size_t tombstone_table_capacity; /* Initial capacity for tombstone registry (0 for default 16384) */
    uint64_t initial_fencing_epoch;  /* Baseline fencing epoch */
    int enable_concurrency;          /* 1 to use thread synchronization */
} keystone_ingest_config_t;

typedef struct {
    uint64_t records_ingested;
    uint64_t duplicates_suppressed;
    uint64_t tombstones_active;
    uint64_t stale_generations_rejected;
    uint64_t stale_epochs_rejected;
    uint64_t current_generation;
    uint64_t current_fencing_epoch;
    keystone_hlc_t high_watermark_hlc;
} keystone_ingest_stats_t;

typedef struct keystone_federation_ingest keystone_federation_ingest_t;

/* --- Explainable Recommendation & Evidence Architecture --- */
#define KEYSTONE_MAX_EXPLAIN_ENTRIES 32

typedef struct {
    char constraint_name[64];
    int satisfied;              /* 1 = pass / satisfied, 0 = fail / excluded */
    char rationale[128];
} keystone_hard_constraint_result_t;

typedef struct {
    keystone_uuid_t candidate_id;
    uint32_t candidate_type;
    double composite_score;
    uint32_t hard_constraint_count;
    keystone_hard_constraint_result_t hard_constraints[KEYSTONE_MAX_EXPLAIN_ENTRIES];
    char ranking_notes[256];
} keystone_explain_t;

typedef struct {
    keystone_uuid_t recommended_target_id;
    uint32_t target_type;
    double confidence_score;
    uint64_t based_on_generation;
    keystone_hlc_t based_on_hlc;
    keystone_explain_t evidence;
} keystone_recommendation_t;

/* --- UUID Helper Functions --- */
keystone_uuid_t keystone_uuid_nil(void);
bool keystone_uuid_is_nil(const keystone_uuid_t* uuid);
bool keystone_uuid_equal(const keystone_uuid_t* a, const keystone_uuid_t* b);
int keystone_uuid_compare(const keystone_uuid_t* a, const keystone_uuid_t* b);
int keystone_uuid_from_string(const char* str, keystone_uuid_t* out);
void keystone_uuid_to_string(const keystone_uuid_t* uuid, char out[37]);
uint64_t keystone_uuid_hash64(const keystone_uuid_t* uuid);

/* --- HLC Helper Functions --- */
int keystone_hlc_compare(const keystone_hlc_t* a, const keystone_hlc_t* b);
keystone_hlc_t keystone_hlc_now(uint32_t node_id);
keystone_hlc_t keystone_hlc_update(keystone_hlc_t* local_clock, const keystone_hlc_t* received_hlc);

/* --- Serialization / Wire Protocol (Hostile-Input Safe) --- */
int keystone_record_serialize(
    const keystone_federation_record_t* rec,
    void* out_buf,
    size_t max_len,
    size_t* out_written
);

int keystone_record_deserialize(
    const void* in_buf,
    size_t in_len,
    keystone_federation_record_t* out_rec
);

/* --- Checkpoint File Persistence --- */
int keystone_checkpoint_save(
    const char* filepath,
    const keystone_federation_checkpoint_t* cp
);

int keystone_checkpoint_load(
    const char* filepath,
    keystone_federation_checkpoint_t* out_cp
);

/* --- Ingestion Engine Lifecycle & Operation --- */
keystone_federation_ingest_t* keystone_federation_ingest_create(
    const keystone_ingest_config_t* config
);

void keystone_federation_ingest_destroy(
    keystone_federation_ingest_t* ingest
);

keystone_ingest_status_t keystone_federation_ingest_submit(
    keystone_federation_ingest_t* ingest,
    const keystone_federation_record_t* record
);

bool keystone_federation_is_tombstoned(
    const keystone_federation_ingest_t* ingest,
    const keystone_uuid_t* object_id
);

int keystone_federation_get_stats(
    const keystone_federation_ingest_t* ingest,
    keystone_ingest_stats_t* out_stats
);

int keystone_federation_advance_fencing_epoch(
    keystone_federation_ingest_t* ingest,
    uint64_t new_epoch
);

int keystone_federation_save_checkpoint(
    const keystone_federation_ingest_t* ingest,
    const char* filepath
);

int keystone_federation_load_checkpoint(
    keystone_federation_ingest_t* ingest,
    const char* filepath
);

/* --- Evidence Bundle Builders --- */
void keystone_explain_init(
    keystone_explain_t* explain,
    const keystone_uuid_t* candidate_id,
    uint32_t candidate_type
);

int keystone_explain_add_constraint(
    keystone_explain_t* explain,
    const char* name,
    int satisfied,
    const char* rationale
);

void keystone_explain_set_ranking(
    keystone_explain_t* explain,
    double score,
    const char* notes
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_FEDERATION_H */
