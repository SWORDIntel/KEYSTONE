/*
 * KEYSTONE Index Manifests & Atomic Publication (CITADEL brief §29/§30)
 *
 * Every index family maintains a manifest: identity, generation, source
 * cursor, HLC bounds, record/tombstone counts, build + algorithm versions,
 * and a checksum of the payload index file it describes. Publications are
 * durable across restarts:
 *
 *   build generation N+1 (off to the side)
 *     -> verify (candidates reload through the real loaders)
 *     -> fsync published copies + per-family manifests
 *     -> publish the ACTIVE POINTER last (atomic tmp+rename)
 *     -> open_latest serves the active generation; on corruption it walks
 *        generations descending, serves the newest fully-verified one, and
 *        quarantines the failed newer files.
 *
 * A crash before the pointer flip can only ever expose a fully durable,
 * checksum-verified generation — never a partial build. The in-memory
 * reader switch (keystoned_server_publish_generation) remains the serving
 * pointer; this is the durable layer beneath it.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License (see the repository root).
 */

#ifndef KEYSTONE_MANIFEST_H
#define KEYSTONE_MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"

#define KEYSTONE_MANIFEST_MAGIC 0x4B534D46u /* 'KSMF' */
#define KEYSTONE_MANIFEST_VERSION 1u

/* Bump on any change to an index family's on-disk format or algorithm. */
#define KEYSTONE_BUILD_VERSION 1u
#define KEYSTONE_ALGO_VERSION_EXACT 1u
#define KEYSTONE_ALGO_VERSION_TEMPORAL 1u

#define KEYSTONE_MANIFEST_FAMILY_MAX 32

typedef struct {
    char family[KEYSTONE_MANIFEST_FAMILY_MAX]; /* "exact" / "temporal" / ... */
    keystone_uuid_t index_uuid;                /* unique per built generation */
    uint64_t index_generation;
    uint64_t source_cursor;                    /* source journal cursor */
    keystone_hlc_t source_hlc_min;
    keystone_hlc_t source_hlc_max;
    uint64_t record_count;
    uint64_t tombstone_count;
    uint32_t build_version;                    /* KEYSTONE_BUILD_VERSION */
    uint32_t algorithm_version;                /* family-specific */
    uint32_t payload_crc32;                    /* CRC32 over the payload file bytes */
    uint64_t payload_bytes;
    uint32_t crc32;                            /* manifest's own integrity */
    uint32_t reserved;                         /* strict v1: zero */
} keystone_index_manifest_t;

/*
 * Build a manifest for a payload index file: checksums the file, mints a
 * unique index_uuid, fills versions. The file must exist.
 */
int keystone_manifest_build(
    const char* family,
    uint64_t index_generation,
    uint64_t source_cursor,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    uint64_t record_count,
    uint64_t tombstone_count,
    uint32_t algorithm_version,
    const char* payload_path,
    keystone_index_manifest_t* out_manifest
);

int keystone_manifest_save(const keystone_index_manifest_t* manifest, const char* filepath);
int keystone_manifest_load(const char* filepath, keystone_index_manifest_t* out_manifest);

/* True when the payload file on disk still matches the manifest (size +
 * CRC32). Detects staleness and tampering of a published index. */
int keystone_manifest_verify_payload(const keystone_index_manifest_t* manifest, const char* payload_path);

/* --- Durable publication (per-directory, exact + temporal families) --- */

typedef struct keystone_publication keystone_publication_t;

keystone_publication_t* keystone_publication_create(const char* directory);
void keystone_publication_destroy(keystone_publication_t* publication);

/*
 * Publish a generation: verify both candidate files reload cleanly, copy
 * them into the publication directory (tmp + fsync + rename), write both
 * per-family manifests (checksummed against the PUBLISHED copies), and flip
 * the ACTIVE pointer last. Returns 0 with *out_generation set on success,
 * -1 without touching the currently active generation.
 */
int keystone_publication_publish(
    keystone_publication_t* publication,
    uint64_t index_generation,
    uint64_t source_cursor,
    const char* exact_src_path,
    const char* temporal_src_path,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    uint64_t record_count,
    uint64_t tombstone_count
);

/*
 * Serve the active generation: load both manifests, verify payload
 * checksums, load both indexes. On any failure, walk generations in
 * descending order, quarantine (move aside) every invalid newer candidate,
 * and serve the newest fully-verified generation. Returns 0 on success,
 * -1 when no valid generation exists.
 */
int keystone_publication_open_latest(
    keystone_publication_t* publication,
    keystone_exact_index_t** out_exact,
    keystone_temporal_index_t** out_temporal,
    keystone_index_manifest_t* out_exact_manifest
);

uint64_t keystone_publication_active_generation(const keystone_publication_t* publication);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_MANIFEST_H */
