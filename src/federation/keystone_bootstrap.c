/*
 * KEYSTONE Snapshot Bootstrap & Live Handoff Implementation
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

#include "keystone_bootstrap.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;               /* KEYSTONE_SNAPSHOT_MAGIC */
    uint32_t version;             /* KEYSTONE_SNAPSHOT_VERSION */
    uint32_t reserved;            /* strict v1: zero */
    uint32_t reserved2;           /* strict v1: zero */
    uint64_t snapshot_generation;
    uint64_t cursor;
    keystone_hlc_t watermark_hlc;
    uint64_t exact_count;
    uint64_t temporal_count;
    uint32_t crc32;               /* hdr[0..offsetof(crc32)) ^ entries */
} ks_snap_header_t;
#pragma pack(pop)

#define SNAP_HDR_CRC_LEN offsetof(ks_snap_header_t, crc32)

static uint32_t snap_crc32(const void* data, size_t len) {
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

/* --- Snapshot persistence --- */

int keystone_snapshot_save(
    const keystone_exact_index_t* exact,
    const keystone_temporal_index_t* temporal,
    uint64_t snapshot_generation,
    uint64_t cursor,
    const char* filepath
) {
    if (!exact || !temporal || !filepath) return -1;

    char tmp_path[512];
    int printed = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", filepath, (int)getpid());
    if (printed < 0 || (size_t)printed >= sizeof(tmp_path)) return -1;

    FILE* f = fopen(tmp_path, "wb");
    if (!f) return -1;

    size_t exact_n = keystone_exact_index_count(exact);
    size_t temporal_n = keystone_temporal_index_count(temporal);

    ks_snap_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = KEYSTONE_SNAPSHOT_MAGIC;
    hdr.version = KEYSTONE_SNAPSHOT_VERSION;
    hdr.snapshot_generation = snapshot_generation;
    hdr.cursor = cursor;
    hdr.exact_count = (uint64_t)exact_n;
    hdr.temporal_count = (uint64_t)temporal_n;
    keystone_temporal_index_watermark(temporal, &hdr.watermark_hlc, NULL);

    /* Extract the exact entries in table order (same as the exact-index save). */
    keystone_exact_entry_t* ents = NULL;
    if (exact_n > 0) {
        size_t bytes = 0;
        if (!checked_mul_size(exact_n, sizeof(keystone_exact_entry_t), &bytes)) {
            fclose(f);
            unlink(tmp_path);
            return -1;
        }
        ents = (keystone_exact_entry_t*)malloc(bytes);
        if (!ents) {
            fclose(f);
            unlink(tmp_path);
            return -1;
        }
        size_t w = 0;
        for (size_t slot = 0; slot < keystone_exact_index_capacity(exact); slot++) {
            if (keystone_exact_index_slot_at(exact, slot, &ents[w])) {
                w++;
                if (w == exact_n) break;
            }
        }
        if (w != exact_n) { /* count/table mismatch: refuse to write a lying snapshot */
            free(ents);
            fclose(f);
            unlink(tmp_path);
            return -1;
        }
    }

    uint32_t crc = snap_crc32(&hdr, SNAP_HDR_CRC_LEN);
    if (ents && exact_n > 0) {
        crc ^= snap_crc32(ents, exact_n * sizeof(keystone_exact_entry_t));
    }
    const keystone_temporal_entry_t* tbase = keystone_temporal_index_entries(temporal);
    if (tbase && temporal_n > 0) {
        crc ^= snap_crc32(tbase, temporal_n * sizeof(keystone_temporal_entry_t));
    }
    hdr.crc32 = crc;

    int ok = 0;
    do {
        if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) break;
        if (ents && exact_n > 0 &&
            fwrite(ents, 1, exact_n * sizeof(keystone_exact_entry_t), f) != exact_n * sizeof(keystone_exact_entry_t)) break;
        if (tbase && temporal_n > 0 &&
            fwrite(tbase, 1, temporal_n * sizeof(keystone_temporal_entry_t), f) != temporal_n * sizeof(keystone_temporal_entry_t)) break;
        ok = 1;
    } while (0);

    free(ents);

    if (!ok) {
        fclose(f);
        unlink(tmp_path);
        return -1;
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

int keystone_snapshot_load(
    const char* filepath,
    keystone_exact_index_t** out_exact,
    keystone_temporal_index_t** out_temporal,
    keystone_snapshot_meta_t* out_meta
) {
    if (!filepath || !out_exact || !out_temporal) return -1;
    *out_exact = NULL;
    *out_temporal = NULL;

    FILE* f = fopen(filepath, "rb");
    if (!f) return -1;

    ks_snap_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return -1;
    }

    if (hdr.magic != KEYSTONE_SNAPSHOT_MAGIC || hdr.version != KEYSTONE_SNAPSHOT_VERSION ||
        hdr.reserved != 0 || hdr.reserved2 != 0) {
        fclose(f);
        return -1;
    }

    /* Hostile-input bar: exact file size before any allocation. */
    size_t exact_bytes = 0, temporal_bytes = 0, total_bytes = 0;
    if (!checked_mul_size((size_t)hdr.exact_count, sizeof(keystone_exact_entry_t), &exact_bytes) ||
        !checked_mul_size((size_t)hdr.temporal_count, sizeof(keystone_temporal_entry_t), &temporal_bytes) ||
        !checked_add_size(sizeof(hdr), exact_bytes, &total_bytes) ||
        !checked_add_size(total_bytes, temporal_bytes, &total_bytes)) {
        fclose(f);
        return -1;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || (uint64_t)st.st_size != (uint64_t)total_bytes) {
        fclose(f); /* truncated, trailing garbage, or lying counts */
        return -1;
    }

    keystone_exact_entry_t* ents = NULL;
    if (exact_bytes > 0) {
        ents = (keystone_exact_entry_t*)malloc(exact_bytes);
        if (!ents || fread(ents, 1, exact_bytes, f) != exact_bytes) {
            free(ents);
            fclose(f);
            return -1;
        }
    }

    keystone_temporal_entry_t* tents = NULL;
    if (temporal_bytes > 0) {
        tents = (keystone_temporal_entry_t*)malloc(temporal_bytes);
        if (!tents || fread(tents, 1, temporal_bytes, f) != temporal_bytes) {
            free(ents);
            free(tents);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    uint32_t expect = snap_crc32(&hdr, SNAP_HDR_CRC_LEN);
    if (ents && exact_bytes > 0) expect ^= snap_crc32(ents, exact_bytes);
    if (tents && temporal_bytes > 0) expect ^= snap_crc32(tents, temporal_bytes);
    if (expect != hdr.crc32) {
        free(ents);
        free(tents);
        return -1;
    }

    keystone_exact_index_t* exact = keystone_exact_index_create((size_t)hdr.exact_count + 1);
    keystone_temporal_index_t* temporal = keystone_temporal_index_create(16);
    if (!exact || !temporal) {
        keystone_exact_index_destroy(exact);
        keystone_temporal_index_destroy(temporal);
        free(ents);
        free(tents);
        return -1;
    }

    for (uint64_t i = 0; i < hdr.exact_count; i++) {
        if (keystone_exact_index_upsert(exact, &ents[i]) != 0) {
            /* entries only fail param/stale rules against themselves here —
             * a hostile file with regressing duplicates is still coherent */
            keystone_exact_index_upsert(exact, &ents[i]);
        }
    }
    for (uint64_t i = 0; i < hdr.temporal_count; i++) {
        if (keystone_temporal_index_append(temporal, &tents[i]) != 0) {
            keystone_exact_index_destroy(exact);
            keystone_temporal_index_destroy(temporal);
            free(ents);
            free(tents);
            return -1;
        }
    }

    free(ents);
    free(tents);

    if (out_meta) {
        out_meta->snapshot_generation = hdr.snapshot_generation;
        out_meta->cursor = hdr.cursor;
        out_meta->watermark_hlc = hdr.watermark_hlc;
        out_meta->exact_count = (size_t)hdr.exact_count;
        out_meta->temporal_count = (size_t)hdr.temporal_count;
    }

    *out_exact = exact;
    *out_temporal = temporal;
    return 0;
}

/* --- Bootstrap session --- */

int keystone_bootstrap_begin(
    const char* snapshot_path,
    const keystone_ingest_config_t* engine_config,
    keystone_bootstrap_session_t* out_session
) {
    if (!snapshot_path || !out_session) return -1;
    memset(out_session, 0, sizeof(*out_session));

    keystone_snapshot_meta_t meta;
    keystone_exact_index_t* exact = NULL;
    keystone_temporal_index_t* temporal = NULL;
    if (keystone_snapshot_load(snapshot_path, &exact, &temporal, &meta) != 0) return -1;

    keystone_federation_ingest_t* engine = keystone_federation_ingest_create(engine_config);
    if (!engine) {
        keystone_exact_index_destroy(exact);
        keystone_temporal_index_destroy(temporal);
        return -1;
    }

    out_session->engine = engine;
    out_session->exact = exact;
    out_session->temporal = temporal;
    out_session->meta = meta;
    return 0;
}

keystone_ingest_status_t keystone_bootstrap_apply_live(
    keystone_bootstrap_session_t* session,
    const keystone_federation_record_t* record
) {
    if (!session || !record) return KEYSTONE_INGEST_ERR_INVALID_PARAM;

    /*
     * Exact-cursor handoff (brief §5.1): events at or before the snapshot
     * watermark are already reflected in the bootstrapped state. Re-assert
     * them idempotently on the exact index (stale generations no-op), never
     * append the timeline (no double-apply), and report DUPLICATE so the
     * consumer's cursor logic can see the coverage.
     */
    if (keystone_hlc_compare(&record->source_hlc, &session->meta.watermark_hlc) <= 0) {
        keystone_exact_index_ingest_record(session->exact, record, 0);
        return KEYSTONE_INGEST_DUPLICATE;
    }

    keystone_ingest_status_t st = keystone_federation_ingest_submit(session->engine, record);
    if (st == KEYSTONE_INGEST_DUPLICATE || st < 0) return st;
    if (st == KEYSTONE_INGEST_STALE_FENCING_EPOCH || st == KEYSTONE_INGEST_STALE_GENERATION) return st;

    keystone_exact_index_ingest_record(session->exact, record, 0);
    keystone_temporal_index_ingest_record(session->temporal, record, 0);
    return st;
}

int keystone_bootstrap_snapshot_now(
    const keystone_bootstrap_session_t* session,
    uint64_t new_cursor,
    const char* filepath
) {
    if (!session || !filepath) return -1;
    keystone_ingest_stats_t st;
    if (keystone_federation_get_stats(session->engine, &st) != 0) return -1;
    return keystone_snapshot_save(session->exact, session->temporal,
                                   st.current_generation, new_cursor, filepath);
}

void keystone_bootstrap_destroy(keystone_bootstrap_session_t* session) {
    if (!session) return;
    keystone_federation_ingest_destroy(session->engine);
    keystone_exact_index_destroy(session->exact);
    keystone_temporal_index_destroy(session->temporal);
    memset(session, 0, sizeof(*session));
}
