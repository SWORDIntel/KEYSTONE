/*
 * KEYSTONE Index Manifests & Atomic Publication Implementation
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License (see the repository root).
 */

#include "keystone_manifest.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

static uint32_t mf_crc32(const void* data, size_t len) {
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

static int mf_file_crc32(const char* path, size_t* out_bytes, uint32_t* out_crc) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t crc = 0xFFFFFFFFu; /* run the raw state manually to chain chunks */
    uint8_t buf[65536];
    size_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            crc ^= buf[i];
            for (int b = 0; b < 8; b++) {
                crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
            }
        }
        total += n;
    }
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    if (out_bytes) *out_bytes = total;
    if (out_crc) *out_crc = crc ^ 0xFFFFFFFFu;
    return 0;
}

/* Unique-enough index uuid: splitmix64 mix of time, pid, generation. */
static keystone_uuid_t mf_mint_uuid(uint64_t generation) {
    uint64_t x = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t)getpid() * 0xBF58476D1CE4E5B9ull;
    x ^= generation * 0x94D049BB133111EBull;
    keystone_uuid_t u;
    for (int i = 0; i < 16; i += 8) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        memcpy(u.bytes + i, &z, 8);
    }
    u.bytes[6] = (uint8_t)((u.bytes[6] & 0x0F) | 0x40); /* uuid v4 shape */
    u.bytes[8] = (uint8_t)((u.bytes[8] & 0x3F) | 0x80);
    return u;
}

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
) {
    if (!family || !payload_path || !out_manifest) return -1;
    if (strlen(family) >= KEYSTONE_MANIFEST_FAMILY_MAX) return -1;

    size_t bytes = 0;
    uint32_t crc = 0;
    if (mf_file_crc32(payload_path, &bytes, &crc) != 0) return -1;

    memset(out_manifest, 0, sizeof(*out_manifest));
    snprintf(out_manifest->family, sizeof(out_manifest->family), "%s", family);
    out_manifest->index_uuid = mf_mint_uuid(index_generation);
    out_manifest->index_generation = index_generation;
    out_manifest->source_cursor = source_cursor;
    if (hlc_min) out_manifest->source_hlc_min = *hlc_min;
    if (hlc_max) out_manifest->source_hlc_max = *hlc_max;
    out_manifest->record_count = record_count;
    out_manifest->tombstone_count = tombstone_count;
    out_manifest->build_version = KEYSTONE_BUILD_VERSION;
    out_manifest->algorithm_version = algorithm_version;
    out_manifest->payload_crc32 = crc;
    out_manifest->payload_bytes = (uint64_t)bytes;
    out_manifest->reserved = 0;
    out_manifest->crc32 = mf_crc32(out_manifest, offsetof(keystone_index_manifest_t, crc32));
    return 0;
}

int keystone_manifest_save(const keystone_index_manifest_t* manifest, const char* filepath) {
    if (!manifest || !filepath) return -1;

    char tmp_path[512];
    int printed = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", filepath, (int)getpid());
    if (printed < 0 || (size_t)printed >= sizeof(tmp_path)) return -1;

    FILE* f = fopen(tmp_path, "wb");
    if (!f) return -1;
    if (fwrite(manifest, 1, sizeof(*manifest), f) != sizeof(*manifest)) {
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

int keystone_manifest_load(const char* filepath, keystone_index_manifest_t* out_manifest) {
    if (!filepath || !out_manifest) return -1;

    FILE* f = fopen(filepath, "rb");
    if (!f) return -1;

    /* Hostile-input bar: exactly one manifest, nothing trailing. */
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || (uint64_t)st.st_size != (uint64_t)sizeof(*out_manifest)) {
        fclose(f);
        return -1;
    }

    if (fread(out_manifest, 1, sizeof(*out_manifest), f) != sizeof(*out_manifest)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    if (out_manifest->crc32 != mf_crc32(out_manifest, offsetof(keystone_index_manifest_t, crc32))) {
        return -1;
    }
    if (out_manifest->reserved != 0 ||
        out_manifest->build_version != KEYSTONE_BUILD_VERSION ||
        out_manifest->family[0] == '\0') {
        return -1;
    }
    return 0;
}

int keystone_manifest_verify_payload(const keystone_index_manifest_t* manifest, const char* payload_path) {
    if (!manifest || !payload_path) return 0;
    size_t bytes = 0;
    uint32_t crc = 0;
    if (mf_file_crc32(payload_path, &bytes, &crc) != 0) return 0;
    return bytes == (size_t)manifest->payload_bytes && crc == manifest->payload_crc32;
}

/* --- Publication --- */

#define PUB_ACTIVE_NAME "active"

struct keystone_publication {
    char dir[380];
};

keystone_publication_t* keystone_publication_create(const char* directory) {
    if (!directory || strlen(directory) >= 380) return NULL;
    mkdir(directory, 0755); /* best effort; publish re-checks */
    char quarantine[420];
    snprintf(quarantine, sizeof(quarantine), "%s/quarantine", directory);
    mkdir(quarantine, 0755);
    keystone_publication_t* pub = (keystone_publication_t*)calloc(1, sizeof(*pub));
    if (!pub) return NULL;
    snprintf(pub->dir, sizeof(pub->dir), "%s", directory);
    return pub;
}

void keystone_publication_destroy(keystone_publication_t* publication) {
    free(publication);
}

/* Copy src to dst with tmp + fsync + rename (durable publish of one file). */
static int pub_copy_durable(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    char tmp[512];
    int printed = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", dst, (int)getpid());
    if (printed < 0 || (size_t)printed >= sizeof(tmp)) {
        fclose(in);
        return -1;
    }
    FILE* out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (ok) {
        fflush(out);
        int fd = fileno(out);
        if (fd >= 0) fsync(fd);
    }
    fclose(out);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Write the ACTIVE pointer (generation + CRC) atomically — always LAST. */
static int pub_write_active(keystone_publication_t* pub, uint64_t generation) {
    char path[512], tmp[512];
    snprintf(path, sizeof(path), "%s/%s", pub->dir, PUB_ACTIVE_NAME);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    uint64_t payload[2] = { generation, 0 };
    payload[1] = (uint64_t)mf_crc32(payload, sizeof(uint64_t));

    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(payload, 1, sizeof(payload), f) != sizeof(payload)) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int pub_read_active(keystone_publication_t* pub, uint64_t* out_generation) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", pub->dir, PUB_ACTIVE_NAME);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t payload[2] = {0, 0};
    if (fread(payload, 1, sizeof(payload), f) != sizeof(payload)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if ((uint64_t)mf_crc32(payload, sizeof(uint64_t)) != payload[1]) return -1;
    *out_generation = payload[0];
    return 0;
}

static void pub_gen_paths(keystone_publication_t* pub, uint64_t gen,
                          char* exact, size_t esz, char* temporal, size_t tsz,
                          char* exact_mf, size_t emsz, char* temporal_mf, size_t tmsz) {
    snprintf(exact, esz, "%s/gen-%llu.exact.index", pub->dir, (unsigned long long)gen);
    snprintf(temporal, tsz, "%s/gen-%llu.temporal.index", pub->dir, (unsigned long long)gen);
    snprintf(exact_mf, emsz, "%s/gen-%llu.exact.manifest", pub->dir, (unsigned long long)gen);
    snprintf(temporal_mf, tmsz, "%s/gen-%llu.temporal.manifest", pub->dir, (unsigned long long)gen);
}

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
) {
    if (!publication || !exact_src_path || !temporal_src_path) return -1;

    /* 1. VERIFY: candidates must reload cleanly through the real loaders. */
    {
        keystone_exact_index_t* ex = NULL;
        keystone_temporal_index_t* tp = NULL;
        if (keystone_exact_index_load(&ex, exact_src_path) != 0) return -1;
        if (ex) keystone_exact_index_destroy(ex);
        if (keystone_temporal_index_load(&tp, temporal_src_path) != 0) return -1;
        if (tp) keystone_temporal_index_destroy(tp);
    }

    char ex_dst[512], tp_dst[512], ex_mf[512], tp_mf[512];
    pub_gen_paths(publication, index_generation,
                  ex_dst, sizeof(ex_dst), tp_dst, sizeof(tp_dst),
                  ex_mf, sizeof(ex_mf), tp_mf, sizeof(tp_mf));

    /* 2. Durable copies. */
    if (pub_copy_durable(exact_src_path, ex_dst) != 0) return -1;
    if (pub_copy_durable(temporal_src_path, tp_dst) != 0) return -1;

    /* 3. Manifests checksum the PUBLISHED copies. */
    keystone_index_manifest_t m;
    if (keystone_manifest_build("exact", index_generation, source_cursor, hlc_min, hlc_max,
                                record_count, tombstone_count, KEYSTONE_ALGO_VERSION_EXACT,
                                ex_dst, &m) != 0) return -1;
    if (keystone_manifest_save(&m, ex_mf) != 0) return -1;
    if (keystone_manifest_build("temporal", index_generation, source_cursor, hlc_min, hlc_max,
                                record_count, tombstone_count, KEYSTONE_ALGO_VERSION_TEMPORAL,
                                tp_dst, &m) != 0) return -1;
    if (keystone_manifest_save(&m, tp_mf) != 0) return -1;

    /* 4. Active pointer LAST: a crash here leaves the previous generation
     * active; everything written so far is complete and checksummed. */
    if (pub_write_active(publication, index_generation) != 0) return -1;
    return 0;
}

/* Collect available generations (manifest files present), descending. */
static int pub_collect_generations(keystone_publication_t* pub, uint64_t* gens, size_t cap, size_t* out_n) {
    DIR* d = opendir(pub->dir);
    if (!d) return -1;
    size_t n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        uint64_t g;
        if (sscanf(de->d_name, "gen-%llu.exact.manifest", (unsigned long long*)&g) == 1) {
            if (n < cap) gens[n++] = g;
        }
    }
    closedir(d);
    /* descending insertion is small; simple bubble sort */
    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (gens[j] > gens[i]) {
                uint64_t t = gens[i]; gens[i] = gens[j]; gens[j] = t;
            }
        }
    }
    *out_n = n;
    return 0;
}

static void pub_quarantine(keystone_publication_t* pub, uint64_t gen) {
    char src[512], dst[512];
    const char* names[4] = { "gen-%llu.exact.index", "gen-%llu.temporal.index",
                             "gen-%llu.exact.manifest", "gen-%llu.temporal.manifest" };
    for (int i = 0; i < 4; i++) {
        snprintf(src, sizeof(src), "%s/", pub->dir);
        snprintf(src + strlen(src), sizeof(src) - strlen(src), names[i], (unsigned long long)gen);
        snprintf(dst, sizeof(dst), "%s/quarantine/", pub->dir);
        snprintf(dst + strlen(dst), sizeof(dst) - strlen(dst), names[i], (unsigned long long)gen);
        rename(src, dst); /* best effort */
    }
}

int keystone_publication_open_latest(
    keystone_publication_t* publication,
    keystone_exact_index_t** out_exact,
    keystone_temporal_index_t** out_temporal,
    keystone_index_manifest_t* out_exact_manifest
) {
    if (!publication || !out_exact || !out_temporal) return -1;
    *out_exact = NULL;
    *out_temporal = NULL;

    uint64_t gens[64];
    size_t n = 0;
    if (pub_collect_generations(publication, gens, 64, &n) != 0 || n == 0) return -1;

    /*
     * Prefer the ACTIVE generation: never serve a generation newer than the
     * active pointer (crash-before-flip must not expose unpublished work —
     * even though such candidates are complete, the operator chose not to
     * flip yet). If the active generation is missing from the list (already
     * quarantined by an earlier open), start from the newest present.
     */
    uint64_t active = 0;
    size_t start = 0;
    if (pub_read_active(publication, &active) == 0) {
        for (size_t i = 0; i < n; i++) {
            if (gens[i] == active) {
                start = i;
                break;
            }
        }
    }

    for (size_t i = start; i < n; i++) {
        uint64_t g = gens[i];

        char ex_p[512], tp_p[512], ex_m[512], tp_m[512];
        pub_gen_paths(publication, g, ex_p, sizeof(ex_p), tp_p, sizeof(tp_p),
                      ex_m, sizeof(ex_m), tp_m, sizeof(tp_m));

        keystone_index_manifest_t mex, mtp;
        keystone_exact_index_t* ex = NULL;
        keystone_temporal_index_t* tp = NULL;
        if (keystone_manifest_load(ex_m, &mex) == 0 &&
            keystone_manifest_load(tp_m, &mtp) == 0 &&
            strcmp(mex.family, "exact") == 0 && strcmp(mtp.family, "temporal") == 0 &&
            mex.index_generation == g && mtp.index_generation == g &&
            keystone_manifest_verify_payload(&mex, ex_p) &&
            keystone_manifest_verify_payload(&mtp, tp_p) &&
            keystone_exact_index_load(&ex, ex_p) == 0 &&
            keystone_temporal_index_load(&tp, tp_p) == 0) {
            *out_exact = ex;
            *out_temporal = tp;
            if (out_exact_manifest) *out_exact_manifest = mex;
            return 0;
        }
        keystone_exact_index_destroy(ex);
        keystone_temporal_index_destroy(tp);
        pub_quarantine(publication, g);
    }
    return -1;
}

uint64_t keystone_publication_active_generation(const keystone_publication_t* publication) {
    if (!publication) return 0;
    uint64_t g = 0;
    keystone_publication_t* mut = (keystone_publication_t*)publication;
    if (pub_read_active(mut, &g) != 0) return 0;
    return g;
}
