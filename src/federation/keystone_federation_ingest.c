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

#include "keystone_federation.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

/* --- Packed Wire Record Header Layout --- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                 /* KEYSTONE_FEDERATION_ENVELOPE_MAGIC (0x4B534645) */
    uint32_t version;               /* KEYSTONE_FEDERATION_ENVELOPE_VERSION (1) */
    uint32_t flags;
    uint32_t tenant_id;
    uint32_t classification;
    uint32_t object_type;
    uint32_t reserved;
    uint64_t source_generation;
    uint64_t fencing_epoch;
    uint64_t hlc_physical_ms;
    uint32_t hlc_logical;
    uint32_t hlc_node_id;
    uint8_t  source_object_id[16];
    uint8_t  source_event_id[16];
    uint8_t  source_node_id[16];
    uint64_t payload_len;
    uint32_t header_crc32;
    uint32_t payload_crc32;
} wire_record_header_t;
#pragma pack(pop)

/* --- Fast CRC32 Implementation (IEEE 802.3 Standard) --- */
static uint32_t crc32_ieee(const void* data, size_t len) {
    static uint32_t table[256];
    static int table_initialized = 0;

    if (!table_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_initialized = 1;
    }

    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* --- UUID Operations --- */

keystone_uuid_t keystone_uuid_nil(void) {
    keystone_uuid_t u;
    memset(u.bytes, 0, sizeof(u.bytes));
    return u;
}

bool keystone_uuid_is_nil(const keystone_uuid_t* uuid) {
    if (!uuid) return true;
    for (size_t i = 0; i < 16; i++) {
        if (uuid->bytes[i] != 0) return false;
    }
    return true;
}

bool keystone_uuid_equal(const keystone_uuid_t* a, const keystone_uuid_t* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

int keystone_uuid_compare(const keystone_uuid_t* a, const keystone_uuid_t* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return memcmp(a->bytes, b->bytes, 16);
}

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int keystone_uuid_from_string(const char* str, keystone_uuid_t* out) {
    if (!str || !out) return -1;
    if (strlen(str) != 36) return -1;

    /* Format: 8-4-4-4-12 */
    if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
        return -1;
    }

    size_t byte_idx = 0;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;

        int hi = hex_val(str[i]);
        i++;
        int lo = hex_val(str[i]);
        if (hi < 0 || lo < 0) return -1;

        out->bytes[byte_idx++] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

void keystone_uuid_to_string(const keystone_uuid_t* uuid, char out[37]) {
    if (!out) return;
    if (!uuid) {
        memset(out, '0', 36);
        out[8] = out[13] = out[18] = out[23] = '-';
        out[36] = '\0';
        return;
    }
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid->bytes[0], uuid->bytes[1], uuid->bytes[2], uuid->bytes[3],
        uuid->bytes[4], uuid->bytes[5],
        uuid->bytes[6], uuid->bytes[7],
        uuid->bytes[8], uuid->bytes[9],
        uuid->bytes[10], uuid->bytes[11], uuid->bytes[12],
        uuid->bytes[13], uuid->bytes[14], uuid->bytes[15]
    );
}

uint64_t keystone_uuid_hash64(const keystone_uuid_t* uuid) {
    if (!uuid) return 0;
    uint64_t h1, h2;
    memcpy(&h1, uuid->bytes, 8);
    memcpy(&h2, uuid->bytes + 8, 8);
    uint64_t x = h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* --- HLC Operations --- */

int keystone_hlc_compare(const keystone_hlc_t* a, const keystone_hlc_t* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    if (a->physical_ms < b->physical_ms) return -1;
    if (a->physical_ms > b->physical_ms) return 1;
    if (a->logical < b->logical) return -1;
    if (a->logical > b->logical) return 1;
    if (a->node_id < b->node_id) return -1;
    if (a->node_id > b->node_id) return 1;
    return 0;
}

keystone_hlc_t keystone_hlc_now(uint32_t node_id) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = ((uint64_t)ts.tv_sec * 1000ULL) + (uint64_t)(ts.tv_nsec / 1000000L);

    keystone_hlc_t hlc;
    hlc.physical_ms = ms;
    hlc.logical = 0;
    hlc.node_id = node_id;
    return hlc;
}

keystone_hlc_t keystone_hlc_update(keystone_hlc_t* local_clock, const keystone_hlc_t* received_hlc) {
    if (!local_clock) {
        keystone_hlc_t empty = {0, 0, 0};
        return empty;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t physical_now = ((uint64_t)ts.tv_sec * 1000ULL) + (uint64_t)(ts.tv_nsec / 1000000L);

    uint64_t max_physical = physical_now;
    if (local_clock->physical_ms > max_physical) {
        max_physical = local_clock->physical_ms;
    }
    if (received_hlc && received_hlc->physical_ms > max_physical) {
        max_physical = received_hlc->physical_ms;
    }

    if (received_hlc && max_physical == local_clock->physical_ms && max_physical == received_hlc->physical_ms) {
        uint32_t max_log = (local_clock->logical > received_hlc->logical) ? local_clock->logical : received_hlc->logical;
        local_clock->logical = max_log + 1;
    } else if (max_physical == local_clock->physical_ms) {
        local_clock->logical++;
    } else if (received_hlc && max_physical == received_hlc->physical_ms) {
        local_clock->logical = received_hlc->logical + 1;
    } else {
        local_clock->logical = 0;
    }

    local_clock->physical_ms = max_physical;
    return *local_clock;
}

/* --- Wire Serialization & Deserialization --- */

int keystone_record_serialize(
    const keystone_federation_record_t* rec,
    void* out_buf,
    size_t max_len,
    size_t* out_written
) {
    if (!rec || !out_buf || !out_written) return -1;
    if (rec->payload_len > KEYSTONE_MAX_PAYLOAD_LEN) return -1;

    size_t total_size = 0;
    if (!checked_add_size(sizeof(wire_record_header_t), rec->payload_len, &total_size)) {
        return -1;
    }

    if (max_len < total_size) return -1;

    wire_record_header_t* hdr = (wire_record_header_t*)out_buf;
    memset(hdr, 0, sizeof(*hdr));

    hdr->magic = KEYSTONE_FEDERATION_ENVELOPE_MAGIC;
    hdr->version = KEYSTONE_FEDERATION_ENVELOPE_VERSION;
    hdr->flags = rec->flags;
    hdr->tenant_id = rec->tenant_id;
    hdr->classification = rec->classification;
    hdr->object_type = rec->object_type;
    hdr->reserved = 0;
    hdr->source_generation = rec->source_generation;
    hdr->fencing_epoch = rec->fencing_epoch;
    hdr->hlc_physical_ms = rec->source_hlc.physical_ms;
    hdr->hlc_logical = rec->source_hlc.logical;
    hdr->hlc_node_id = rec->source_hlc.node_id;

    memcpy(hdr->source_object_id, rec->source_object_id.bytes, 16);
    memcpy(hdr->source_event_id, rec->source_event_id.bytes, 16);
    memcpy(hdr->source_node_id, rec->source_node_id.bytes, 16);
    hdr->payload_len = (uint64_t)rec->payload_len;

    /* Compute payload CRC32 */
    if (rec->payload_len > 0 && rec->payload) {
        uint8_t* payload_dst = (uint8_t*)out_buf + sizeof(wire_record_header_t);
        memcpy(payload_dst, rec->payload, rec->payload_len);
        hdr->payload_crc32 = crc32_ieee(rec->payload, rec->payload_len);
    } else {
        hdr->payload_crc32 = 0;
    }

    /* Compute header CRC32 excluding the last two CRC fields (8 bytes) */
    size_t crc_header_len = sizeof(wire_record_header_t) - (2 * sizeof(uint32_t));
    hdr->header_crc32 = crc32_ieee(hdr, crc_header_len);

    *out_written = total_size;
    return 0;
}

int keystone_record_deserialize(
    const void* in_buf,
    size_t in_len,
    keystone_federation_record_t* out_rec
) {
    if (!in_buf || !out_rec) return -1;
    if (in_len < sizeof(wire_record_header_t)) return -1;

    const wire_record_header_t* hdr = (const wire_record_header_t*)in_buf;

    if (hdr->magic != KEYSTONE_FEDERATION_ENVELOPE_MAGIC) return -1;
    if (hdr->version != KEYSTONE_FEDERATION_ENVELOPE_VERSION) return -1;
    if (hdr->payload_len > KEYSTONE_MAX_PAYLOAD_LEN) return -1;

    size_t total_size = 0;
    if (!checked_add_size(sizeof(wire_record_header_t), (size_t)hdr->payload_len, &total_size)) {
        return -1;
    }
    if (in_len < total_size) return -1;

    /* Validate header CRC32 */
    size_t crc_header_len = sizeof(wire_record_header_t) - (2 * sizeof(uint32_t));
    uint32_t computed_hdr_crc = crc32_ieee(hdr, crc_header_len);
    if (computed_hdr_crc != hdr->header_crc32) return -1;

    /* Validate payload CRC32 if payload present */
    const uint8_t* payload_ptr = (const uint8_t*)in_buf + sizeof(wire_record_header_t);
    if (hdr->payload_len > 0) {
        uint32_t computed_payload_crc = crc32_ieee(payload_ptr, (size_t)hdr->payload_len);
        if (computed_payload_crc != hdr->payload_crc32) return -1;
    }

    memset(out_rec, 0, sizeof(*out_rec));
    out_rec->flags = hdr->flags;
    out_rec->tenant_id = hdr->tenant_id;
    out_rec->classification = hdr->classification;
    out_rec->object_type = hdr->object_type;
    out_rec->source_generation = hdr->source_generation;
    out_rec->fencing_epoch = hdr->fencing_epoch;
    out_rec->source_hlc.physical_ms = hdr->hlc_physical_ms;
    out_rec->source_hlc.logical = hdr->hlc_logical;
    out_rec->source_hlc.node_id = hdr->hlc_node_id;

    memcpy(out_rec->source_object_id.bytes, hdr->source_object_id, 16);
    memcpy(out_rec->source_event_id.bytes, hdr->source_event_id, 16);
    memcpy(out_rec->source_node_id.bytes, hdr->source_node_id, 16);

    out_rec->payload_len = (size_t)hdr->payload_len;
    out_rec->payload = (hdr->payload_len > 0) ? payload_ptr : NULL;

    return 0;
}

/* --- Checkpoint File Persistence --- */

int keystone_checkpoint_save(
    const char* filepath,
    const keystone_federation_checkpoint_t* cp
) {
    if (!filepath || !cp) return -1;

    keystone_federation_checkpoint_t tmp_cp = *cp;
    tmp_cp.magic = KEYSTONE_CHECKPOINT_MAGIC;
    tmp_cp.version = KEYSTONE_CHECKPOINT_VERSION;

    /* Checksum covers all fields up to the crc32 member */
    size_t crc_len = offsetof(keystone_federation_checkpoint_t, crc32);
    tmp_cp.crc32 = crc32_ieee(&tmp_cp, crc_len);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", filepath, (int)getpid());

    FILE* f = fopen(tmp_path, "wb");
    if (!f) return -1;

    size_t written = fwrite(&tmp_cp, 1, sizeof(tmp_cp), f);
    if (written != sizeof(tmp_cp)) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);

    if (rename(tmp_path, filepath) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int keystone_checkpoint_load(
    const char* filepath,
    keystone_federation_checkpoint_t* out_cp
) {
    if (!filepath || !out_cp) return -1;

    FILE* f = fopen(filepath, "rb");
    if (!f) return -1;

    keystone_federation_checkpoint_t cp;
    size_t read_bytes = fread(&cp, 1, sizeof(cp), f);
    fclose(f);

    if (read_bytes != sizeof(cp)) return -1;
    if (cp.magic != KEYSTONE_CHECKPOINT_MAGIC) return -1;
    if (cp.version != KEYSTONE_CHECKPOINT_VERSION) return -1;

    size_t crc_len = offsetof(keystone_federation_checkpoint_t, crc32);
    uint32_t computed = crc32_ieee(&cp, crc_len);
    if (computed != cp.crc32) return -1;

    *out_cp = cp;
    return 0;
}

/* --- Internal Hash Tables for Ingestion Engine --- */

typedef struct {
    keystone_uuid_t key;
    uint32_t occupied;
} dedup_slot_t;

typedef struct {
    keystone_uuid_t key;
    uint32_t occupied;
} tombstone_slot_t;

struct keystone_federation_ingest {
    pthread_mutex_t lock;
    int concurrency_enabled;

    /* Ingestion Statistics & Watermarks */
    keystone_ingest_stats_t stats;
    keystone_uuid_t last_event_id;

    /* Deduplication Table (Hash Set + Ring for FIFO Replacement) */
    dedup_slot_t* dedup_slots;
    size_t dedup_capacity;
    size_t dedup_mask;
    keystone_uuid_t* dedup_ring;
    size_t dedup_ring_head;
    size_t dedup_ring_count;

    /* Tombstone Registry (Open Addressing Hash Set) */
    tombstone_slot_t* tombstone_slots;
    size_t tombstone_capacity;
    size_t tombstone_mask;
    size_t tombstone_count;
};

static size_t next_power_of_two(size_t v) {
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

keystone_federation_ingest_t* keystone_federation_ingest_create(
    const keystone_ingest_config_t* config
) {
    size_t dedup_cap = (config && config->dedup_window_capacity > 0)
        ? config->dedup_window_capacity : 131072u;
    size_t tomb_cap = (config && config->tombstone_table_capacity > 0)
        ? config->tombstone_table_capacity : 16384u;

    dedup_cap = next_power_of_two(dedup_cap);
    tomb_cap = next_power_of_two(tomb_cap);

    keystone_federation_ingest_t* ing = (keystone_federation_ingest_t*)calloc(1, sizeof(*ing));
    if (!ing) return NULL;

    if (config && config->enable_concurrency) {
        ing->concurrency_enabled = 1;
        pthread_mutex_init(&ing->lock, NULL);
    }

    ing->dedup_capacity = dedup_cap;
    ing->dedup_mask = dedup_cap - 1;
    ing->dedup_slots = (dedup_slot_t*)calloc(dedup_cap, sizeof(dedup_slot_t));
    ing->dedup_ring = (keystone_uuid_t*)calloc(dedup_cap, sizeof(keystone_uuid_t));

    ing->tombstone_capacity = tomb_cap;
    ing->tombstone_mask = tomb_cap - 1;
    ing->tombstone_slots = (tombstone_slot_t*)calloc(tomb_cap, sizeof(tombstone_slot_t));

    if (!ing->dedup_slots || !ing->dedup_ring || !ing->tombstone_slots) {
        keystone_federation_ingest_destroy(ing);
        return NULL;
    }

    if (config) {
        ing->stats.current_fencing_epoch = config->initial_fencing_epoch;
    }

    return ing;
}

void keystone_federation_ingest_destroy(keystone_federation_ingest_t* ingest) {
    if (!ingest) return;

    if (ingest->concurrency_enabled) {
        pthread_mutex_destroy(&ingest->lock);
    }

    free(ingest->dedup_slots);
    free(ingest->dedup_ring);
    free(ingest->tombstone_slots);
    free(ingest);
}

/* Internal Dedup Operations */
static int dedup_lookup(keystone_federation_ingest_t* ing, const keystone_uuid_t* id) {
    uint64_t h = keystone_uuid_hash64(id);
    size_t idx = (size_t)(h & ing->dedup_mask);

    for (size_t probe = 0; probe < ing->dedup_capacity; probe++) {
        size_t slot = (idx + probe) & ing->dedup_mask;
        if (!ing->dedup_slots[slot].occupied) return 0;
        if (keystone_uuid_equal(&ing->dedup_slots[slot].key, id)) return 1;
    }
    return 0;
}

static void dedup_remove(keystone_federation_ingest_t* ing, const keystone_uuid_t* id) {
    uint64_t h = keystone_uuid_hash64(id);
    size_t idx = (size_t)(h & ing->dedup_mask);

    for (size_t probe = 0; probe < ing->dedup_capacity; probe++) {
        size_t slot = (idx + probe) & ing->dedup_mask;
        if (!ing->dedup_slots[slot].occupied) return;
        if (keystone_uuid_equal(&ing->dedup_slots[slot].key, id)) {
            ing->dedup_slots[slot].occupied = 0;
            return;
        }
    }
}

static void dedup_insert(keystone_federation_ingest_t* ing, const keystone_uuid_t* id) {
    if (ing->dedup_ring_count >= (ing->dedup_capacity * 3 / 4)) {
        /* Evict oldest */
        size_t oldest_idx = (ing->dedup_ring_head + ing->dedup_capacity - ing->dedup_ring_count) & ing->dedup_mask;
        dedup_remove(ing, &ing->dedup_ring[oldest_idx]);
        ing->dedup_ring_count--;
    }

    uint64_t h = keystone_uuid_hash64(id);
    size_t idx = (size_t)(h & ing->dedup_mask);

    for (size_t probe = 0; probe < ing->dedup_capacity; probe++) {
        size_t slot = (idx + probe) & ing->dedup_mask;
        if (!ing->dedup_slots[slot].occupied || keystone_uuid_equal(&ing->dedup_slots[slot].key, id)) {
            ing->dedup_slots[slot].key = *id;
            ing->dedup_slots[slot].occupied = 1;
            break;
        }
    }

    ing->dedup_ring[ing->dedup_ring_head] = *id;
    ing->dedup_ring_head = (ing->dedup_ring_head + 1) & ing->dedup_mask;
    ing->dedup_ring_count++;
}

/* Internal Tombstone Operations */
static int tombstone_lookup(const keystone_federation_ingest_t* ing, const keystone_uuid_t* id) {
    if (ing->tombstone_count == 0) return 0;
    uint64_t h = keystone_uuid_hash64(id);
    size_t idx = (size_t)(h & ing->tombstone_mask);

    for (size_t probe = 0; probe < ing->tombstone_capacity; probe++) {
        size_t slot = (idx + probe) & ing->tombstone_mask;
        if (!ing->tombstone_slots[slot].occupied) return 0;
        if (keystone_uuid_equal(&ing->tombstone_slots[slot].key, id)) return 1;
    }
    return 0;
}

static void tombstone_grow(keystone_federation_ingest_t* ing) {
    size_t new_cap = ing->tombstone_capacity * 2;
    tombstone_slot_t* new_slots = (tombstone_slot_t*)calloc(new_cap, sizeof(tombstone_slot_t));
    if (!new_slots) return;

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < ing->tombstone_capacity; i++) {
        if (ing->tombstone_slots[i].occupied) {
            uint64_t h = keystone_uuid_hash64(&ing->tombstone_slots[i].key);
            size_t idx = (size_t)(h & new_mask);
            for (size_t p = 0; p < new_cap; p++) {
                size_t s = (idx + p) & new_mask;
                if (!new_slots[s].occupied) {
                    new_slots[s] = ing->tombstone_slots[i];
                    break;
                }
            }
        }
    }

    free(ing->tombstone_slots);
    ing->tombstone_slots = new_slots;
    ing->tombstone_capacity = new_cap;
    ing->tombstone_mask = new_mask;
}

static void tombstone_insert(keystone_federation_ingest_t* ing, const keystone_uuid_t* id) {
    if (tombstone_lookup(ing, id)) return;

    if (ing->tombstone_count * 2 >= ing->tombstone_capacity) {
        tombstone_grow(ing);
    }

    uint64_t h = keystone_uuid_hash64(id);
    size_t idx = (size_t)(h & ing->tombstone_mask);

    for (size_t probe = 0; probe < ing->tombstone_capacity; probe++) {
        size_t slot = (idx + probe) & ing->tombstone_mask;
        if (!ing->tombstone_slots[slot].occupied) {
            ing->tombstone_slots[slot].key = *id;
            ing->tombstone_slots[slot].occupied = 1;
            ing->tombstone_count++;
            ing->stats.tombstones_active = ing->tombstone_count;
            return;
        }
    }
}

keystone_ingest_status_t keystone_federation_ingest_submit(
    keystone_federation_ingest_t* ingest,
    const keystone_federation_record_t* record
) {
    if (!ingest || !record) return KEYSTONE_INGEST_ERR_INVALID_PARAM;

    if (ingest->concurrency_enabled) {
        pthread_mutex_lock(&ingest->lock);
    }

    /* 1. Epoch fencing validation */
    if (record->fencing_epoch < ingest->stats.current_fencing_epoch) {
        ingest->stats.stale_epochs_rejected++;
        if (ingest->concurrency_enabled) pthread_mutex_unlock(&ingest->lock);
        return KEYSTONE_INGEST_STALE_FENCING_EPOCH;
    }

    /* 2. Idempotent Deduplication by Event ID */
    if (!keystone_uuid_is_nil(&record->source_event_id)) {
        if (dedup_lookup(ingest, &record->source_event_id)) {
            ingest->stats.duplicates_suppressed++;
            if (ingest->concurrency_enabled) pthread_mutex_unlock(&ingest->lock);
            return KEYSTONE_INGEST_DUPLICATE;
        }
    }

    /* 3. Advance Fencing Epoch if record has higher epoch */
    if (record->fencing_epoch > ingest->stats.current_fencing_epoch) {
        ingest->stats.current_fencing_epoch = record->fencing_epoch;
    }

    /* 4. Generation Monotonic Check */
    if (record->source_generation > ingest->stats.current_generation) {
        ingest->stats.current_generation = record->source_generation;
    }

    /* 5. Update High Watermark HLC */
    if (keystone_hlc_compare(&record->source_hlc, &ingest->stats.high_watermark_hlc) > 0) {
        ingest->stats.high_watermark_hlc = record->source_hlc;
    }

    /* 6. Process Tombstones */
    keystone_ingest_status_t result = KEYSTONE_INGEST_OK;
    if (record->flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) {
        tombstone_insert(ingest, &record->source_object_id);
        result = KEYSTONE_INGEST_TOMBSTONE_APPLIED;
    }

    /* 7. Record to Dedup Cache */
    if (!keystone_uuid_is_nil(&record->source_event_id)) {
        dedup_insert(ingest, &record->source_event_id);
        ingest->last_event_id = record->source_event_id;
    }

    ingest->stats.records_ingested++;

    if (ingest->concurrency_enabled) {
        pthread_mutex_unlock(&ingest->lock);
    }

    return result;
}

bool keystone_federation_is_tombstoned(
    const keystone_federation_ingest_t* ingest,
    const keystone_uuid_t* object_id
) {
    if (!ingest || !object_id) return false;

    if (ingest->concurrency_enabled) {
        pthread_mutex_lock((pthread_mutex_t*)&ingest->lock);
    }

    bool res = tombstone_lookup(ingest, object_id);

    if (ingest->concurrency_enabled) {
        pthread_mutex_unlock((pthread_mutex_t*)&ingest->lock);
    }

    return res;
}

int keystone_federation_get_stats(
    const keystone_federation_ingest_t* ingest,
    keystone_ingest_stats_t* out_stats
) {
    if (!ingest || !out_stats) return -1;

    if (ingest->concurrency_enabled) {
        pthread_mutex_lock((pthread_mutex_t*)&ingest->lock);
    }

    *out_stats = ingest->stats;

    if (ingest->concurrency_enabled) {
        pthread_mutex_unlock((pthread_mutex_t*)&ingest->lock);
    }

    return 0;
}

int keystone_federation_advance_fencing_epoch(
    keystone_federation_ingest_t* ingest,
    uint64_t new_epoch
) {
    if (!ingest) return -1;

    if (ingest->concurrency_enabled) {
        pthread_mutex_lock(&ingest->lock);
    }

    if (new_epoch < ingest->stats.current_fencing_epoch) {
        if (ingest->concurrency_enabled) pthread_mutex_unlock(&ingest->lock);
        return -1;
    }

    ingest->stats.current_fencing_epoch = new_epoch;

    if (ingest->concurrency_enabled) {
        pthread_mutex_unlock(&ingest->lock);
    }

    return 0;
}

int keystone_federation_save_checkpoint(
    const keystone_federation_ingest_t* ingest,
    const char* filepath
) {
    if (!ingest || !filepath) return -1;

    keystone_federation_checkpoint_t cp;
    memset(&cp, 0, sizeof(cp));

    if (ingest->concurrency_enabled) {
        pthread_mutex_lock((pthread_mutex_t*)&ingest->lock);
    }

    cp.checkpoint_generation = ingest->stats.current_generation;
    cp.active_fencing_epoch = ingest->stats.current_fencing_epoch;
    cp.last_applied_hlc = ingest->stats.high_watermark_hlc;
    cp.last_event_id = ingest->last_event_id;
    cp.total_records_ingested = ingest->stats.records_ingested;
    cp.total_tombstones_applied = ingest->stats.tombstones_active;
    cp.total_duplicates_suppressed = ingest->stats.duplicates_suppressed;
    cp.total_stale_rejected = ingest->stats.stale_epochs_rejected + ingest->stats.stale_generations_rejected;

    if (ingest->concurrency_enabled) {
        pthread_mutex_unlock((pthread_mutex_t*)&ingest->lock);
    }

    return keystone_checkpoint_save(filepath, &cp);
}

int keystone_federation_load_checkpoint(
    keystone_federation_ingest_t* ingest,
    const char* filepath
) {
    if (!ingest || !filepath) return -1;

    keystone_federation_checkpoint_t cp;
    if (keystone_checkpoint_load(filepath, &cp) != 0) return -1;

    if (ingest->concurrency_enabled) {
        pthread_mutex_lock(&ingest->lock);
    }

    ingest->stats.current_generation = cp.checkpoint_generation;
    ingest->stats.current_fencing_epoch = cp.active_fencing_epoch;
    ingest->stats.high_watermark_hlc = cp.last_applied_hlc;
    ingest->last_event_id = cp.last_event_id;
    ingest->stats.records_ingested = cp.total_records_ingested;
    ingest->stats.tombstones_active = cp.total_tombstones_applied;
    ingest->stats.duplicates_suppressed = cp.total_duplicates_suppressed;

    if (ingest->concurrency_enabled) {
        pthread_mutex_unlock(&ingest->lock);
    }

    return 0;
}

/* --- Evidence Bundle Builders --- */

void keystone_explain_init(
    keystone_explain_t* explain,
    const keystone_uuid_t* candidate_id,
    uint32_t candidate_type
) {
    if (!explain) return;
    memset(explain, 0, sizeof(*explain));
    if (candidate_id) {
        explain->candidate_id = *candidate_id;
    }
    explain->candidate_type = candidate_type;
}

int keystone_explain_add_constraint(
    keystone_explain_t* explain,
    const char* name,
    int satisfied,
    const char* rationale
) {
    if (!explain || !name) return -1;
    if (explain->hard_constraint_count >= KEYSTONE_MAX_EXPLAIN_ENTRIES) return -1;

    uint32_t idx = explain->hard_constraint_count++;
    snprintf(explain->hard_constraints[idx].constraint_name,
             sizeof(explain->hard_constraints[idx].constraint_name), "%s", name);
    explain->hard_constraints[idx].satisfied = satisfied ? 1 : 0;
    if (rationale) {
        snprintf(explain->hard_constraints[idx].rationale,
                 sizeof(explain->hard_constraints[idx].rationale), "%s", rationale);
    } else {
        explain->hard_constraints[idx].rationale[0] = '\0';
    }

    return 0;
}

void keystone_explain_set_ranking(
    keystone_explain_t* explain,
    double score,
    const char* notes
) {
    if (!explain) return;
    explain->composite_score = score;
    if (notes) {
        snprintf(explain->ranking_notes, sizeof(explain->ranking_notes), "%s", notes);
    } else {
        explain->ranking_notes[0] = '\0';
    }
}
