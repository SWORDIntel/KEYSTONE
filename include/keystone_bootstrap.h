/*
 * KEYSTONE Snapshot Bootstrap & Live Handoff (Phase 0/1 completion)
 *
 * CITADEL brief §5.1: an index may be bootstrapped from a snapshot of the
 * authoritative state, then handed off to the live event stream at an exact
 * cursor — "events are neither lost nor double-applied without detection."
 *
 * The snapshot captures the exact and temporal index state plus the source
 * watermark (generation, journal cursor, high-water HLC) at snapshot time.
 * The bootstrap session applies live records under the handoff rule:
 *
 *   - record HLC <= snapshot watermark  -> already covered by the snapshot:
 *     detected (returns KEYSTONE_INGEST_DUPLICATE), exact state re-asserted
 *     idempotently, timeline NOT appended (no double-apply);
 *   - record HLC  > snapshot watermark  -> applied through the normal
 *     federation ingest path (dedup, fencing, tombstones, indexes).
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
 */

#ifndef KEYSTONE_BOOTSTRAP_H
#define KEYSTONE_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"

#define KEYSTONE_SNAPSHOT_MAGIC 0x4B53534Eu /* 'KSSN' */
#define KEYSTONE_SNAPSHOT_VERSION 1u

typedef struct {
    uint64_t snapshot_generation; /* source generation at snapshot time */
    uint64_t cursor;              /* source journal cursor at snapshot time */
    keystone_hlc_t watermark_hlc; /* snapshot covers all events <= this HLC */
    size_t exact_count;
    size_t temporal_count;
} keystone_snapshot_meta_t;

/*
 * Capture a snapshot (exact + temporal state + watermarks) to a file.
 * Same persistence bar as the other index formats: atomic publish
 * (tmp + fsync + rename), whole-file CRC32, strict validation on load.
 */
int keystone_snapshot_save(
    const keystone_exact_index_t* exact,
    const keystone_temporal_index_t* temporal,
    uint64_t snapshot_generation,
    uint64_t cursor,
    const char* filepath
);

/*
 * Load and validate a snapshot (hostile input: magic/version/reserved,
 * checked arithmetic, exact file-size bound, CRC). Rebuilds both indexes.
 */
int keystone_snapshot_load(
    const char* filepath,
    keystone_exact_index_t** out_exact,
    keystone_temporal_index_t** out_temporal,
    keystone_snapshot_meta_t* out_meta
);

/* --- Bootstrap session (snapshot -> live handoff) --- */

typedef struct {
    keystone_federation_ingest_t* engine; /* fresh engine for the live era */
    keystone_exact_index_t* exact;        /* rebuilt from the snapshot */
    keystone_temporal_index_t* temporal;  /* rebuilt from the snapshot */
    keystone_snapshot_meta_t meta;
} keystone_bootstrap_session_t;

/*
 * Bootstrap from a snapshot file: validates it, rebuilds both indexes, and
 * starts a fresh ingest engine for the live era. The session's watermark is
 * the snapshot's high-water HLC.
 */
int keystone_bootstrap_begin(
    const char* snapshot_path,
    const keystone_ingest_config_t* engine_config,
    keystone_bootstrap_session_t* out_session
);

/*
 * Live-stream apply with the exact-cursor handoff rule (see header top).
 * Returns KEYSTONE_INGEST_DUPLICATE for snapshot-covered events (detected,
 * not applied to the timeline), or the normal ingest statuses for live-era
 * events. Stale fencing epochs are rejected exactly as in steady state.
 */
keystone_ingest_status_t keystone_bootstrap_apply_live(
    keystone_bootstrap_session_t* session,
    const keystone_federation_record_t* record
);

/* Persist the session's current state as a NEW snapshot (generation rolls
 * forward; cursor comes from the caller — usually the live cursor). */
int keystone_bootstrap_snapshot_now(
    const keystone_bootstrap_session_t* session,
    uint64_t new_cursor,
    const char* filepath
);

void keystone_bootstrap_destroy(keystone_bootstrap_session_t* session);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_BOOTSTRAP_H */
