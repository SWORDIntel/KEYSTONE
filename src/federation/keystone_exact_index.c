/*
 * KEYSTONE Exact Identity Index Implementation
 *
 * Fast O(1)/O(log N) mapping from resource UUID to latest generation,
 * fencing epoch, location slot, and tombstone state with full 128-bit verification.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_exact_index.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t count;
    uint64_t active_count;
    uint64_t tombstone_count;
    uint32_t crc32;
    uint32_t reserved;
} exact_index_file_header_t;
#pragma pack(pop)

static uint32_t crc32_exact(const void* data, size_t len) {
    static uint32_t table[256];
    static int initialized = 0;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = 1;
    }

    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct {
    keystone_exact_entry_t entry;
    uint32_t occupied;
} exact_slot_t;

struct keystone_exact_index {
    exact_slot_t* slots;
    size_t capacity;
    size_t mask;
    size_t count;
    size_t active_count;
    size_t tombstone_count;
};

static size_t next_pow2(size_t v) {
    if (v < 16) return 16;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
    v |= v >> 32;
#endif
    v++;
    return v;
}

keystone_exact_index_t* keystone_exact_index_create(size_t initial_capacity) {
    size_t cap = next_pow2(initial_capacity > 0 ? initial_capacity : 1024);

    keystone_exact_index_t* idx = (keystone_exact_index_t*)calloc(1, sizeof(*idx));
    if (!idx) return NULL;

    idx->capacity = cap;
    idx->mask = cap - 1;
    idx->slots = (exact_slot_t*)calloc(cap, sizeof(exact_slot_t));
    if (!idx->slots) {
        free(idx);
        return NULL;
    }
    return idx;
}

void keystone_exact_index_destroy(keystone_exact_index_t* index) {
    if (!index) return;
    free(index->slots);
    free(index);
}

static void exact_index_grow(keystone_exact_index_t* index) {
    size_t new_cap = index->capacity * 2;
    exact_slot_t* new_slots = (exact_slot_t*)calloc(new_cap, sizeof(exact_slot_t));
    if (!new_slots) return;

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < index->capacity; i++) {
        if (index->slots[i].occupied) {
            uint64_t h = keystone_uuid_hash64(&index->slots[i].entry.resource_id);
            size_t idx = (size_t)(h & new_mask);
            for (size_t p = 0; p < new_cap; p++) {
                size_t s = (idx + p) & new_mask;
                if (!new_slots[s].occupied) {
                    new_slots[s] = index->slots[i];
                    break;
                }
            }
        }
    }

    free(index->slots);
    index->slots = new_slots;
    index->capacity = new_cap;
    index->mask = new_mask;
}

int keystone_exact_index_upsert(
    keystone_exact_index_t* index,
    const keystone_exact_entry_t* entry
) {
    if (!index || !entry) return -1;

    uint64_t h = keystone_uuid_hash64(&entry->resource_id);
    size_t start_idx = (size_t)(h & index->mask);

    /* 1. Check if resource already exists */
    for (size_t probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start_idx + probe) & index->mask;
        if (!index->slots[slot].occupied) break;

        if (keystone_uuid_equal(&index->slots[slot].entry.resource_id, &entry->resource_id)) {
            /* Fencing epoch check: cannot regress epoch */
            if (entry->fencing_epoch < index->slots[slot].entry.fencing_epoch) {
                return -2; /* Stale epoch rejected */
            }

            /* Generation check: reject older generation unless fencing epoch advanced */
            if (entry->fencing_epoch == index->slots[slot].entry.fencing_epoch &&
                entry->latest_generation < index->slots[slot].entry.latest_generation) {
                return -3; /* Stale generation rejected */
            }

            bool was_tombstoned = (index->slots[slot].entry.flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) != 0;
            bool now_tombstoned = (entry->flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) != 0;

            index->slots[slot].entry = *entry;
            if (!was_tombstoned && now_tombstoned) {
                index->active_count--;
                index->tombstone_count++;
            } else if (was_tombstoned && !now_tombstoned) {
                index->active_count++;
                index->tombstone_count--;
            }
            return 0;
        }
    }

    /* 2. New Entry: check load factor (70%) */
    if ((index->count + 1) * 10 >= index->capacity * 7) {
        exact_index_grow(index);
        start_idx = (size_t)(h & index->mask);
    }

    for (size_t probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start_idx + probe) & index->mask;
        if (!index->slots[slot].occupied) {
            index->slots[slot].entry = *entry;
            index->slots[slot].occupied = 1;
            index->count++;
            if (entry->flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) {
                index->tombstone_count++;
            } else {
                index->active_count++;
            }
            return 0;
        }
    }

    return -1;
}

int keystone_exact_index_ingest_record(
    keystone_exact_index_t* index,
    const keystone_federation_record_t* rec,
    uint64_t location_slot
) {
    if (!index || !rec) return -1;

    keystone_exact_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    entry.resource_id = rec->source_object_id;
    entry.latest_event_id = rec->source_event_id;
    entry.node_id = rec->source_node_id;
    entry.latest_generation = rec->source_generation;
    entry.fencing_epoch = rec->fencing_epoch;
    entry.latest_hlc = rec->source_hlc;
    entry.tenant_id = rec->tenant_id;
    entry.classification = rec->classification;
    entry.object_type = rec->object_type;
    entry.flags = rec->flags;
    entry.location_slot = location_slot;

    return keystone_exact_index_upsert(index, &entry);
}

int keystone_exact_index_lookup(
    const keystone_exact_index_t* index,
    const keystone_uuid_t* id,
    keystone_exact_entry_t* out_entry
) {
    if (!index || !id || !out_entry) return -1;

    uint64_t h = keystone_uuid_hash64(id);
    size_t start_idx = (size_t)(h & index->mask);

    for (size_t probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start_idx + probe) & index->mask;
        if (!index->slots[slot].occupied) return -1;

        if (keystone_uuid_equal(&index->slots[slot].entry.resource_id, id)) {
            *out_entry = index->slots[slot].entry;
            return 0;
        }
    }
    return -1;
}

bool keystone_exact_index_contains(
    const keystone_exact_index_t* index,
    const keystone_uuid_t* id
) {
    if (!index || !id) return false;

    uint64_t h = keystone_uuid_hash64(id);
    size_t start_idx = (size_t)(h & index->mask);

    for (size_t probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start_idx + probe) & index->mask;
        if (!index->slots[slot].occupied) return false;

        if (keystone_uuid_equal(&index->slots[slot].entry.resource_id, id)) {
            return true;
        }
    }
    return false;
}

size_t keystone_exact_index_count(const keystone_exact_index_t* index) {
    return index ? index->count : 0;
}

size_t keystone_exact_index_active_count(const keystone_exact_index_t* index) {
    return index ? index->active_count : 0;
}

size_t keystone_exact_index_tombstone_count(const keystone_exact_index_t* index) {
    return index ? index->tombstone_count : 0;
}

/* Binary Persistence */

int keystone_exact_index_save(
    const keystone_exact_index_t* index,
    const char* filepath
) {
    if (!index || !filepath) return -1;

    exact_index_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = KEYSTONE_EXACT_INDEX_MAGIC;
    hdr.version = KEYSTONE_EXACT_INDEX_VERSION;
    hdr.count = (uint64_t)index->count;
    hdr.active_count = (uint64_t)index->active_count;
    hdr.tombstone_count = (uint64_t)index->tombstone_count;

    /* Build contiguous array of entries to compute CRC and write */
    size_t entries_bytes = 0;
    if (!checked_mul_size(index->count, sizeof(keystone_exact_entry_t), &entries_bytes)) {
        return -1;
    }

    keystone_exact_entry_t* entries = NULL;
    if (index->count > 0) {
        entries = (keystone_exact_entry_t*)malloc(entries_bytes);
        if (!entries) return -1;

        size_t written_idx = 0;
        for (size_t i = 0; i < index->capacity; i++) {
            if (index->slots[i].occupied) {
                entries[written_idx++] = index->slots[i].entry;
            }
        }
    }

    /* CRC covers header up to crc32 member plus the entries payload */
    size_t hdr_crc_len = offsetof(exact_index_file_header_t, crc32);
    uint32_t crc = crc32_exact(&hdr, hdr_crc_len);
    if (entries && entries_bytes > 0) {
        /* Fold in entries CRC */
        uint32_t payload_crc = crc32_exact(entries, entries_bytes);
        crc ^= payload_crc;
    }
    hdr.crc32 = crc;

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", filepath, (int)getpid());

    FILE* f = fopen(tmp_path, "wb");
    if (!f) {
        free(entries);
        return -1;
    }

    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        unlink(tmp_path);
        free(entries);
        return -1;
    }

    if (entries && entries_bytes > 0) {
        if (fwrite(entries, 1, entries_bytes, f) != entries_bytes) {
            fclose(f);
            unlink(tmp_path);
            free(entries);
            return -1;
        }
    }

    free(entries);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    if (rename(tmp_path, filepath) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int keystone_exact_index_load(
    keystone_exact_index_t** out_index,
    const char* filepath
) {
    if (!out_index || !filepath) return -1;

    FILE* f = fopen(filepath, "rb");
    if (!f) return -1;

    exact_index_file_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;
    }

    if (hdr.magic != KEYSTONE_EXACT_INDEX_MAGIC || hdr.version != KEYSTONE_EXACT_INDEX_VERSION) {
        fclose(f);
        return -1;
    }

    size_t entries_bytes = 0;
    if (!checked_mul_size((size_t)hdr.count, sizeof(keystone_exact_entry_t), &entries_bytes)) {
        fclose(f);
        return -1;
    }

    keystone_exact_entry_t* entries = NULL;
    if (hdr.count > 0) {
        entries = (keystone_exact_entry_t*)malloc(entries_bytes);
        if (!entries) {
            fclose(f);
            return -1;
        }
        if (fread(entries, 1, entries_bytes, f) != entries_bytes) {
            free(entries);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    /* Verify CRC */
    size_t hdr_crc_len = offsetof(exact_index_file_header_t, crc32);
    uint32_t expected_crc = crc32_exact(&hdr, hdr_crc_len);
    if (entries && entries_bytes > 0) {
        uint32_t payload_crc = crc32_exact(entries, entries_bytes);
        expected_crc ^= payload_crc;
    }

    if (expected_crc != hdr.crc32) {
        free(entries);
        return -1;
    }

    keystone_exact_index_t* idx = keystone_exact_index_create((size_t)hdr.count * 2);
    if (!idx) {
        free(entries);
        return -1;
    }

    for (size_t i = 0; i < (size_t)hdr.count; i++) {
        keystone_exact_index_upsert(idx, &entries[i]);
    }

    free(entries);
    *out_index = idx;
    return 0;
}
