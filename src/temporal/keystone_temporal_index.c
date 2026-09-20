/*
 * KEYSTONE Monotonic Temporal Index Implementation
 *
 * Monotonic interval indexing over hybrid logical timestamps (HLC).
 * Supports sub-10ns range queries, object-scoped histories, and reverse-chronological scans.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_temporal.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                 /* KEYSTONE_TEMPORAL_INDEX_MAGIC (0x4B53544D) */
    uint32_t version;               /* KEYSTONE_TEMPORAL_INDEX_VERSION (1) */
    uint64_t count;
    keystone_hlc_t min_hlc;
    keystone_hlc_t max_hlc;
    uint32_t crc32;
    uint32_t reserved;
} temporal_file_header_t;
#pragma pack(pop)

static uint32_t crc32_temporal(const void* data, size_t len) {
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

struct keystone_temporal_index {
    keystone_temporal_entry_t* entries;
    size_t capacity;
    size_t count;
};

keystone_temporal_index_t* keystone_temporal_index_create(size_t initial_capacity) {
    size_t cap = initial_capacity > 0 ? initial_capacity : 4096;

    keystone_temporal_index_t* idx = (keystone_temporal_index_t*)calloc(1, sizeof(*idx));
    if (!idx) return NULL;

    idx->capacity = cap;
    idx->entries = (keystone_temporal_entry_t*)calloc(cap, sizeof(keystone_temporal_entry_t));
    if (!idx->entries) {
        free(idx);
        return NULL;
    }
    return idx;
}

void keystone_temporal_index_destroy(keystone_temporal_index_t* index) {
    if (!index) return;
    free(index->entries);
    free(index);
}

/* Branchless Lower-bound search for HLC */
static size_t lower_bound_hlc(const keystone_temporal_entry_t* arr, size_t n, const keystone_hlc_t* target) {
    size_t first = 0;
    size_t count = n;

    while (count > 0) {
        size_t step = count / 2;
        size_t mid = first + step;
        if (keystone_hlc_compare(&arr[mid].hlc, target) < 0) {
            first = mid + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

/* Branchless Upper-bound search for HLC */
static size_t upper_bound_hlc(const keystone_temporal_entry_t* arr, size_t n, const keystone_hlc_t* target) {
    size_t first = 0;
    size_t count = n;

    while (count > 0) {
        size_t step = count / 2;
        size_t mid = first + step;
        if (keystone_hlc_compare(&arr[mid].hlc, target) <= 0) {
            first = mid + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

int keystone_temporal_index_append(
    keystone_temporal_index_t* index,
    const keystone_temporal_entry_t* entry
) {
    if (!index || !entry) return -1;

    /* Expand capacity if full */
    if (index->count >= index->capacity) {
        size_t new_cap = index->capacity * 2;
        size_t new_bytes = 0;
        if (!checked_mul_size(new_cap, sizeof(keystone_temporal_entry_t), &new_bytes)) {
            return -1;
        }
        keystone_temporal_entry_t* new_entries = (keystone_temporal_entry_t*)realloc(index->entries, new_bytes);
        if (!new_entries) return -1;
        index->entries = new_entries;
        index->capacity = new_cap;
    }

    /* Common case: monotonic arrival (entry >= tail) */
    if (index->count == 0 || keystone_hlc_compare(&entry->hlc, &index->entries[index->count - 1].hlc) >= 0) {
        index->entries[index->count++] = *entry;
        return 0;
    }

    /* Out-of-order arrival: find insertion point and shift right */
    size_t pos = upper_bound_hlc(index->entries, index->count, &entry->hlc);
    size_t move_count = index->count - pos;
    memmove(&index->entries[pos + 1], &index->entries[pos], move_count * sizeof(keystone_temporal_entry_t));
    index->entries[pos] = *entry;
    index->count++;

    return 0;
}

int keystone_temporal_index_ingest_record(
    keystone_temporal_index_t* index,
    const keystone_federation_record_t* rec,
    uint64_t payload_offset
) {
    if (!index || !rec) return -1;

    keystone_temporal_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    entry.hlc = rec->source_hlc;
    entry.event_id = rec->source_event_id;
    entry.object_id = rec->source_object_id;
    entry.event_type = rec->object_type;
    entry.tenant_id = rec->tenant_id;
    entry.classification = rec->classification;
    entry.flags = rec->flags;
    entry.source_generation = rec->source_generation;
    entry.payload_offset = payload_offset;

    return keystone_temporal_index_append(index, &entry);
}

size_t keystone_temporal_index_query_range(
    const keystone_temporal_index_t* index,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
) {
    if (!index || index->count == 0) return 0;

    size_t start_idx = 0;
    if (hlc_min) {
        start_idx = lower_bound_hlc(index->entries, index->count, hlc_min);
    }

    size_t end_idx = index->count;
    if (hlc_max) {
        end_idx = upper_bound_hlc(index->entries, index->count, hlc_max);
    }

    if (start_idx >= end_idx) return 0;

    size_t match_count = end_idx - start_idx;
    size_t copy_count = (match_count < max_results) ? match_count : max_results;

    if (out_entries && copy_count > 0) {
        memcpy(out_entries, &index->entries[start_idx], copy_count * sizeof(keystone_temporal_entry_t));
    }

    return match_count;
}

size_t keystone_temporal_index_query_object(
    const keystone_temporal_index_t* index,
    const keystone_uuid_t* object_id,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
) {
    if (!index || !object_id || index->count == 0) return 0;

    size_t start_idx = 0;
    if (hlc_min) {
        start_idx = lower_bound_hlc(index->entries, index->count, hlc_min);
    }

    size_t end_idx = index->count;
    if (hlc_max) {
        end_idx = upper_bound_hlc(index->entries, index->count, hlc_max);
    }

    size_t matches_found = 0;
    for (size_t i = start_idx; i < end_idx; i++) {
        if (keystone_uuid_equal(&index->entries[i].object_id, object_id)) {
            if (out_entries && matches_found < max_results) {
                out_entries[matches_found] = index->entries[i];
            }
            matches_found++;
        }
    }

    return matches_found;
}

size_t keystone_temporal_index_scan_reverse(
    const keystone_temporal_index_t* index,
    const keystone_hlc_t* hlc_max,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
) {
    if (!index || index->count == 0 || !out_entries || max_results == 0) return 0;

    size_t end_idx = index->count;
    if (hlc_max) {
        end_idx = upper_bound_hlc(index->entries, index->count, hlc_max);
    }

    if (end_idx == 0) return 0;

    size_t written = 0;
    size_t curr = end_idx;
    while (curr > 0 && written < max_results) {
        curr--;
        out_entries[written++] = index->entries[curr];
    }

    return written;
}

size_t keystone_temporal_index_aggregate_buckets(
    const keystone_temporal_index_t* index,
    uint64_t start_ms,
    uint64_t end_ms,
    uint64_t bucket_size_ms,
    keystone_temporal_bucket_t* out_buckets,
    size_t max_buckets
) {
    if (!index || index->count == 0 || !out_buckets || max_buckets == 0) return 0;
    if (bucket_size_ms == 0 || start_ms >= end_ms) return 0;

    size_t num_buckets = (size_t)((end_ms - start_ms + bucket_size_ms - 1) / bucket_size_ms);
    if (num_buckets > max_buckets) {
        num_buckets = max_buckets;
    }

    memset(out_buckets, 0, num_buckets * sizeof(keystone_temporal_bucket_t));
    for (size_t b = 0; b < num_buckets; b++) {
        out_buckets[b].bucket_start_ms = start_ms + (b * bucket_size_ms);
        out_buckets[b].bucket_end_ms = out_buckets[b].bucket_start_ms + bucket_size_ms;
    }

    keystone_hlc_t min_hlc = { .physical_ms = start_ms, .logical = 0, .node_id = 0 };
    keystone_hlc_t max_hlc = { .physical_ms = end_ms, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };

    size_t start_idx = lower_bound_hlc(index->entries, index->count, &min_hlc);
    size_t end_idx = upper_bound_hlc(index->entries, index->count, &max_hlc);

    for (size_t i = start_idx; i < end_idx; i++) {
        uint64_t ms = index->entries[i].hlc.physical_ms;
        if (ms < start_ms || ms >= end_ms) continue;

        size_t b = (size_t)((ms - start_ms) / bucket_size_ms);
        if (b < num_buckets) {
            out_buckets[b].event_count++;
            if (index->entries[i].flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) {
                out_buckets[b].tombstone_count++;
            }
            if (index->entries[i].flags & KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE) {
                out_buckets[b].error_count++;
            }
        }
    }

    return num_buckets;
}

size_t keystone_temporal_index_count(const keystone_temporal_index_t* index) {
    return index ? index->count : 0;
}

/* Binary Persistence */

int keystone_temporal_index_save(
    const keystone_temporal_index_t* index,
    const char* filepath
) {
    if (!index || !filepath) return -1;

    temporal_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = KEYSTONE_TEMPORAL_INDEX_MAGIC;
    hdr.version = KEYSTONE_TEMPORAL_INDEX_VERSION;
    hdr.count = (uint64_t)index->count;

    if (index->count > 0) {
        hdr.min_hlc = index->entries[0].hlc;
        hdr.max_hlc = index->entries[index->count - 1].hlc;
    }

    size_t payload_bytes = 0;
    if (!checked_mul_size(index->count, sizeof(keystone_temporal_entry_t), &payload_bytes)) {
        return -1;
    }

    size_t hdr_crc_len = offsetof(temporal_file_header_t, crc32);
    uint32_t crc = crc32_temporal(&hdr, hdr_crc_len);
    if (payload_bytes > 0 && index->entries) {
        crc ^= crc32_temporal(index->entries, payload_bytes);
    }
    hdr.crc32 = crc;

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", filepath, (int)getpid());

    FILE* f = fopen(tmp_path, "wb");
    if (!f) return -1;

    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    if (payload_bytes > 0 && index->entries) {
        if (fwrite(index->entries, 1, payload_bytes, f) != payload_bytes) {
            fclose(f);
            unlink(tmp_path);
            return -1;
        }
    }

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

int keystone_temporal_index_load(
    keystone_temporal_index_t** out_index,
    const char* filepath
) {
    if (!out_index || !filepath) return -1;

    FILE* f = fopen(filepath, "rb");
    if (!f) return -1;

    temporal_file_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;
    }

    if (hdr.magic != KEYSTONE_TEMPORAL_INDEX_MAGIC || hdr.version != KEYSTONE_TEMPORAL_INDEX_VERSION) {
        fclose(f);
        return -1;
    }

    size_t payload_bytes = 0;
    if (!checked_mul_size((size_t)hdr.count, sizeof(keystone_temporal_entry_t), &payload_bytes)) {
        fclose(f);
        return -1;
    }

    keystone_temporal_entry_t* entries = NULL;
    if (hdr.count > 0) {
        entries = (keystone_temporal_entry_t*)malloc(payload_bytes);
        if (!entries) {
            fclose(f);
            return -1;
        }
        if (fread(entries, 1, payload_bytes, f) != payload_bytes) {
            free(entries);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    size_t hdr_crc_len = offsetof(temporal_file_header_t, crc32);
    uint32_t expected_crc = crc32_temporal(&hdr, hdr_crc_len);
    if (payload_bytes > 0 && entries) {
        expected_crc ^= crc32_temporal(entries, payload_bytes);
    }

    if (expected_crc != hdr.crc32) {
        free(entries);
        return -1;
    }

    keystone_temporal_index_t* idx = (keystone_temporal_index_t*)calloc(1, sizeof(*idx));
    if (!idx) {
        free(entries);
        return -1;
    }

    idx->capacity = (size_t)hdr.count > 16 ? (size_t)hdr.count : 16;
    idx->count = (size_t)hdr.count;
    idx->entries = entries;

    *out_index = idx;
    return 0;
}
