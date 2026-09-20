/*
 * KEYSTONE Exact Identity Index
 *
 * Fast O(1)/O(log N) mapping from resource/event/node UUID to latest
 * source generation, fencing epoch, location slot, and tombstone state.
 * Collision-safe with full 128-bit key verification.
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

#ifndef KEYSTONE_EXACT_INDEX_H
#define KEYSTONE_EXACT_INDEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"

#define KEYSTONE_EXACT_INDEX_MAGIC 0x4B534558u /* 'KSEX' */
#define KEYSTONE_EXACT_INDEX_VERSION 1u

typedef struct {
    keystone_uuid_t resource_id;
    keystone_uuid_t latest_event_id;
    keystone_uuid_t node_id;
    uint64_t latest_generation;
    uint64_t fencing_epoch;
    keystone_hlc_t latest_hlc;
    uint32_t tenant_id;
    uint32_t classification;
    uint32_t object_type;
    uint32_t flags;
    uint64_t location_slot;
} keystone_exact_entry_t;

typedef struct keystone_exact_index keystone_exact_index_t;

keystone_exact_index_t* keystone_exact_index_create(size_t initial_capacity);
void keystone_exact_index_destroy(keystone_exact_index_t* index);

int keystone_exact_index_upsert(
    keystone_exact_index_t* index,
    const keystone_exact_entry_t* entry
);

int keystone_exact_index_ingest_record(
    keystone_exact_index_t* index,
    const keystone_federation_record_t* rec,
    uint64_t location_slot
);

int keystone_exact_index_lookup(
    const keystone_exact_index_t* index,
    const keystone_uuid_t* id,
    keystone_exact_entry_t* out_entry
);

bool keystone_exact_index_contains(
    const keystone_exact_index_t* index,
    const keystone_uuid_t* id
);

size_t keystone_exact_index_count(const keystone_exact_index_t* index);
size_t keystone_exact_index_active_count(const keystone_exact_index_t* index);
size_t keystone_exact_index_tombstone_count(const keystone_exact_index_t* index);

/* Binary Persistence */
int keystone_exact_index_save(
    const keystone_exact_index_t* index,
    const char* filepath
);

int keystone_exact_index_load(
    keystone_exact_index_t** out_index,
    const char* filepath
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_EXACT_INDEX_H */
