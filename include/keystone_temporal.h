/*
 * KEYSTONE Monotonic Temporal Index
 *
 * Monotonic interval indexing over hybrid logical timestamps (HLC).
 * Supports sub-10ns range queries, object-scoped histories, and reverse-chronological scans.
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

#ifndef KEYSTONE_TEMPORAL_H
#define KEYSTONE_TEMPORAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"

#define KEYSTONE_TEMPORAL_INDEX_MAGIC 0x4B53544Du /* 'KSTM' */
#define KEYSTONE_TEMPORAL_INDEX_VERSION 1u

typedef struct {
    keystone_hlc_t hlc;
    keystone_uuid_t event_id;
    keystone_uuid_t object_id;
    uint32_t event_type;
    uint32_t tenant_id;
    uint32_t classification;
    uint32_t flags;
    uint64_t source_generation;
    uint64_t payload_offset;
} keystone_temporal_entry_t;

typedef struct {
    uint64_t bucket_start_ms;
    uint64_t bucket_end_ms;
    uint64_t event_count;
    uint64_t tombstone_count;
    uint64_t error_count;
} keystone_temporal_bucket_t;

typedef struct keystone_temporal_index keystone_temporal_index_t;

keystone_temporal_index_t* keystone_temporal_index_create(size_t initial_capacity);
void keystone_temporal_index_destroy(keystone_temporal_index_t* index);

int keystone_temporal_index_append(
    keystone_temporal_index_t* index,
    const keystone_temporal_entry_t* entry
);

int keystone_temporal_index_ingest_record(
    keystone_temporal_index_t* index,
    const keystone_federation_record_t* rec,
    uint64_t payload_offset
);

/* Range Query over HLC interval [hlc_min, hlc_max] */
size_t keystone_temporal_index_query_range(
    const keystone_temporal_index_t* index,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
);

/* Object-scoped timeline query over [hlc_min, hlc_max] */
size_t keystone_temporal_index_query_object(
    const keystone_temporal_index_t* index,
    const keystone_uuid_t* object_id,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
);

/* Reverse-chronological scan (from newest backwards up to max_results) */
size_t keystone_temporal_index_scan_reverse(
    const keystone_temporal_index_t* index,
    const keystone_hlc_t* hlc_max,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
);

/* Aggregation over time buckets */
size_t keystone_temporal_index_aggregate_buckets(
    const keystone_temporal_index_t* index,
    uint64_t start_ms,
    uint64_t end_ms,
    uint64_t bucket_size_ms,
    keystone_temporal_bucket_t* out_buckets,
    size_t max_buckets
);

size_t keystone_temporal_index_count(const keystone_temporal_index_t* index);

/* Binary Persistence */
int keystone_temporal_index_save(
    const keystone_temporal_index_t* index,
    const char* filepath
);

int keystone_temporal_index_load(
    keystone_temporal_index_t** out_index,
    const char* filepath
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_TEMPORAL_H */
