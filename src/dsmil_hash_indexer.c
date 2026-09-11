#include "../include/dsmil_hash_indexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* FNV-1a 64-bit string hash */
static int64_t dsmil_hash_string(const char* str, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)str[i];
        h *= 0x100000001b3ULL;
    }
    return (int64_t)h;
}

/* ============================================================================
 * LSD Radix Sort for fixed-width 64-bit keys (hashes) with satellite data
 * (offsets, strings, string_lens).
 *
 * 8 passes x 8 bits.  O(n) with sequential memory access and no
 * unpredictable comparator branches — substantially faster than qsort
 * for the fixed-width 64-bit hash keys at multimillion-record scale.
 * ============================================================================ */

static void radix_sort_lsd_64(
    int64_t* restrict keys,
    uint64_t* restrict offsets,
    char** restrict strings,
    size_t* restrict string_lens,
    size_t count)
{
    if (count < 2) return;

    /* Allocate parallel temp arrays */
    int64_t*  tmp_keys   = malloc(count * sizeof(int64_t));
    uint64_t* tmp_offsets = malloc(count * sizeof(uint64_t));
    char**    tmp_strings = malloc(count * sizeof(char*));
    size_t*   tmp_lens    = malloc(count * sizeof(size_t));
    if (!tmp_keys || !tmp_offsets || !tmp_strings || !tmp_lens) {
        /* Fall back to qsort if allocation fails */
        free(tmp_keys); free(tmp_offsets); free(tmp_strings); free(tmp_lens);
        goto fallback_qsort;
    }

    int64_t*  src_keys    = keys;
    uint64_t* src_offsets = offsets;
    char**    src_strings = strings;
    size_t*   src_lens    = string_lens;

    int64_t*  dst_keys    = tmp_keys;
    uint64_t* dst_offsets = tmp_offsets;
    char**    dst_strings = tmp_strings;
    size_t*   dst_lens    = tmp_lens;

    /* LSD radix sort: 8 passes x 8 bits.
     * With 8 passes (an even number), pointer ping-ponging between the original
     * and temporary arrays leaves the sorted data directly in the original
     * caller-provided arrays on the final pass with ZERO memcpys. */
    for (int pass = 0; pass < 8; pass++) {
        int shift = pass * 8;
        size_t hist[256] = {0};

        /* Histogram */
        for (size_t i = 0; i < count; i++) {
            uint8_t bucket = (uint8_t)((uint64_t)src_keys[i] >> shift);
            hist[bucket]++;
        }

        /* Prefix sum -> starting positions */
        size_t pos[256];
        size_t accum = 0;
        for (int b = 0; b < 256; b++) {
            pos[b] = accum;
            accum += hist[b];
        }

        /* Scatter from src into dst */
        for (size_t i = 0; i < count; i++) {
            uint8_t bucket = (uint8_t)((uint64_t)src_keys[i] >> shift);
            size_t dst = pos[bucket]++;
            dst_keys[dst]    = src_keys[i];
            dst_offsets[dst] = src_offsets[i];
            dst_strings[dst] = src_strings[i];
            dst_lens[dst]    = src_lens[i];
        }

        /* Ping-pong pointers: swap src and dst for the next pass */
        int64_t*  tk = src_keys;    src_keys = dst_keys;       dst_keys = tk;
        uint64_t* to = src_offsets; src_offsets = dst_offsets; dst_offsets = to;
        char**    ts = src_strings; src_strings = dst_strings; dst_strings = ts;
        size_t*   tl = src_lens;    src_lens = dst_lens;       dst_lens = tl;
    }

    free(tmp_keys);
    free(tmp_offsets);
    free(tmp_strings);
    free(tmp_lens);
    return;

fallback_qsort:
    /* Fallback: pack into pairs and qsort (original approach) */
    {
        typedef struct { int64_t hash; uint64_t offset; char* str; size_t len; } pair_t;
        pair_t* pairs = malloc(count * sizeof(pair_t));
        if (!pairs) return;
        for (size_t i = 0; i < count; i++) {
            pairs[i].hash = keys[i];
            pairs[i].offset = offsets[i];
            pairs[i].str = strings[i];
            pairs[i].len = string_lens[i];
        }
        /* Simple insertion-based comparison sort via qsort */
        /* We use a comparator that only looks at hash */
        /* (qsort is stable enough for our purposes since we re-scatter) */
        for (size_t i = 1; i < count; i++) {
            pair_t cur = pairs[i];
            size_t j = i;
            while (j > 0 && pairs[j - 1].hash > cur.hash) {
                pairs[j] = pairs[j - 1];
                j--;
            }
            pairs[j] = cur;
        }
        for (size_t i = 0; i < count; i++) {
            keys[i] = pairs[i].hash;
            offsets[i] = pairs[i].offset;
            strings[i] = pairs[i].str;
            string_lens[i] = pairs[i].len;
        }
        free(pairs);
    }
}

/* ============================================================================ */

dsmil_hash_index_t* dsmil_hash_index_create(size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = 1024;
    dsmil_hash_index_t* idx = calloc(1, sizeof(dsmil_hash_index_t));
    if (!idx) return NULL;

    idx->hashes = malloc(initial_capacity * sizeof(int64_t));
    idx->offsets = malloc(initial_capacity * sizeof(uint64_t));
    idx->strings = calloc(initial_capacity, sizeof(char*));
    idx->string_lens = malloc(initial_capacity * sizeof(size_t));
    idx->anchor_table = keystone_anchor_table_create();

    if (!idx->hashes || !idx->offsets || !idx->strings ||
        !idx->string_lens || !idx->anchor_table) {
        dsmil_hash_index_destroy(idx);
        return NULL;
    }

    idx->capacity = initial_capacity;
    idx->count = 0;
    idx->is_sorted = 0;
    return idx;
}

void dsmil_hash_index_destroy(dsmil_hash_index_t* idx) {
    if (!idx) return;
    /* Free retained string copies */
    if (idx->strings) {
        for (size_t i = 0; i < idx->count; i++) {
            free(idx->strings[i]);
        }
        free(idx->strings);
    }
    free(idx->hashes);
    free(idx->offsets);
    free(idx->string_lens);
    if (idx->anchor_table) keystone_anchor_table_destroy(idx->anchor_table);
    free(idx);
}

int dsmil_hash_index_add(dsmil_hash_index_t* idx, const char* str, size_t len, uint64_t byte_offset) {
    if (!idx || !str) return -1;

    if (idx->count >= idx->capacity) {
        size_t new_cap = idx->capacity * 2;
        int64_t* new_h = realloc(idx->hashes, new_cap * sizeof(int64_t));
        uint64_t* new_o = realloc(idx->offsets, new_cap * sizeof(uint64_t));
        char** new_s = realloc(idx->strings, new_cap * sizeof(char*));
        size_t* new_l = realloc(idx->string_lens, new_cap * sizeof(size_t));
        if (!new_h || !new_o || !new_s || !new_l) {
            if (new_h) idx->hashes = new_h;
            if (new_o) idx->offsets = new_o;
            if (new_s) idx->strings = new_s;
            if (new_l) idx->string_lens = new_l;
            return -1;
        }
        /* Zero the new string slots so destroy doesn't free garbage */
        memset(new_s + idx->capacity, 0, (new_cap - idx->capacity) * sizeof(char*));
        idx->hashes = new_h;
        idx->offsets = new_o;
        idx->strings = new_s;
        idx->string_lens = new_l;
        idx->capacity = new_cap;
    }

    /* Retain a copy of the original string for collision verification */
    char* str_copy = malloc(len + 1);
    if (!str_copy) return -1;
    memcpy(str_copy, str, len);
    str_copy[len] = '\0';

    idx->hashes[idx->count] = dsmil_hash_string(str, len);
    idx->offsets[idx->count] = byte_offset;
    idx->strings[idx->count] = str_copy;
    idx->string_lens[idx->count] = len;
    idx->count++;
    idx->is_sorted = 0;
    return 0;
}

int dsmil_hash_index_finalize(dsmil_hash_index_t* idx) {
    if (!idx || idx->count == 0) return 0;
    if (idx->is_sorted) return 0;

    /* LSD radix sort: O(n) for fixed 64-bit keys, carrying offsets,
     * strings, and string_lens alongside. */
    radix_sort_lsd_64(idx->hashes, idx->offsets, idx->strings,
                       idx->string_lens, idx->count);

    idx->is_sorted = 1;

    /* Pre-warm the KEYSTONE anchor table */
    keystone_config_t cfg;
    keystone_config_init(&cfg, KEYSTONE_WORKLOAD_IDS);
    keystone_search_enhanced(idx->hashes, idx->count, idx->hashes[idx->count/2],
                               idx->anchor_table, &cfg);

    return 0;
}

keystone_result_t dsmil_hash_index_search(dsmil_hash_index_t* idx, const char* query_str, uint64_t* out_offset) {
    if (!idx || !query_str || !idx->is_sorted || idx->count == 0) return KEYSTONE_NOT_FOUND;

    size_t query_len = strlen(query_str);
    int64_t target_hash = dsmil_hash_string(query_str, query_len);

    /* Binary search (unsigned comparison to match radix sort order) */
    uint64_t target_u = (uint64_t)target_hash;
    size_t lo = 0, hi = idx->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t mid_hash = (uint64_t)idx->hashes[mid];
        if (mid_hash < target_u) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo >= idx->count || (uint64_t)idx->hashes[lo] != target_u) {
        return KEYSTONE_NOT_FOUND;
    }

    /* Verify string match (handle hash collisions) */
    for (size_t i = lo; i < idx->count && (uint64_t)idx->hashes[i] == target_u; i++) {
        if (idx->string_lens[i] == query_len &&
            memcmp(idx->strings[i], query_str, query_len) == 0) {
            if (out_offset) {
                *out_offset = idx->offsets[i];
            }
            return (keystone_result_t)i;
        }
    }

    return KEYSTONE_NOT_FOUND;
}

keystone_result_t dsmil_hash_index_search_all(
    dsmil_hash_index_t* idx,
    const char* query_str,
    uint64_t* out_offsets,
    size_t max_offsets,
    size_t* out_count)
{
    if (!idx || !query_str || !idx->is_sorted || idx->count == 0) {
        if (out_count) *out_count = 0;
        return KEYSTONE_NOT_FOUND;
    }

    size_t query_len = strlen(query_str);
    int64_t target_hash = dsmil_hash_string(query_str, query_len);

    /* Binary search for the first entry with hash == target_hash.
     * The hash array is sorted by unsigned 64-bit key value (radix sort).
     * We use unsigned comparison to match the sort order. */
    uint64_t target_u = (uint64_t)target_hash;
    size_t lo = 0, hi = idx->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t mid_hash = (uint64_t)idx->hashes[mid];
        if (mid_hash < target_u) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    /* lo is now the first index where hash >= target_hash */
    if (lo >= idx->count || (uint64_t)idx->hashes[lo] != target_u) {
        if (out_count) *out_count = 0;
        return KEYSTONE_NOT_FOUND;
    }

    /* Collect all entries in the target_hash range whose string matches */
    size_t count = 0;
    for (size_t i = lo; i < idx->count; i++) {
        if ((uint64_t)idx->hashes[i] != target_u) break;
        /* Check string match (handles hash collisions) */
        if (idx->string_lens[i] == query_len &&
            memcmp(idx->strings[i], query_str, query_len) == 0) {
            if (count < max_offsets) {
                out_offsets[count] = idx->offsets[i];
            }
            count++;
        }
    }

    if (out_count) *out_count = count;
    return (count > 0) ? 0 : KEYSTONE_NOT_FOUND;
}

/* ── Serialization ─────────────────────────────────────────────────── */

int dsmil_hash_index_save(dsmil_hash_index_t* idx, const char* path) {
    if (!idx || !path) return -1;

    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    /* Header: magic(4) + version(4) + count(8) + is_sorted(4) */
    const char magic[4] = {'T','G','H','I'};
    uint32_t version = 1;
    fwrite(magic, 1, 4, f);
    fwrite(&version, 4, 1, f);
    fwrite(&idx->count, sizeof(size_t), 1, f);
    fwrite(&idx->is_sorted, sizeof(int), 1, f);

    /* Hashes array */
    fwrite(idx->hashes, sizeof(int64_t), idx->count, f);
    /* Offsets array */
    fwrite(idx->offsets, sizeof(uint64_t), idx->count, f);
    /* String lengths array */
    fwrite(idx->string_lens, sizeof(size_t), idx->count, f);

    /* String data: all strings concatenated */
    for (size_t i = 0; i < idx->count; i++) {
        fwrite(idx->strings[i], 1, idx->string_lens[i], f);
    }

    fclose(f);
    return 0;
}

dsmil_hash_index_t* dsmil_hash_index_load(const char* path) {
    if (!path) return NULL;

    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    /* Header */
    char magic[4];
    uint32_t version;
    size_t count;
    int is_sorted;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "TGHI", 4) != 0) {
        fclose(f); return NULL;
    }
    if (fread(&version, 4, 1, f) != 1 || version != 1) {
        fclose(f); return NULL;
    }
    if (fread(&count, sizeof(size_t), 1, f) != 1) {
        fclose(f); return NULL;
    }
    if (fread(&is_sorted, sizeof(int), 1, f) != 1) {
        fclose(f); return NULL;
    }

    dsmil_hash_index_t* idx = dsmil_hash_index_create(count > 0 ? count : 1);
    if (!idx) { fclose(f); return NULL; }

    /* Ensure capacity */
    if (count > idx->capacity) {
        /* Grow to fit */
        size_t new_cap = count;
        int64_t* new_h = realloc(idx->hashes, new_cap * sizeof(int64_t));
        uint64_t* new_o = realloc(idx->offsets, new_cap * sizeof(uint64_t));
        char** new_s = realloc(idx->strings, new_cap * sizeof(char*));
        size_t* new_l = realloc(idx->string_lens, new_cap * sizeof(size_t));
        if (!new_h || !new_o || !new_s || !new_l) {
            fclose(f);
            dsmil_hash_index_destroy(idx);
            return NULL;
        }
        memset(new_s, 0, new_cap * sizeof(char*));
        idx->hashes = new_h;
        idx->offsets = new_o;
        idx->strings = new_s;
        idx->string_lens = new_l;
        idx->capacity = new_cap;
    }

    idx->count = count;
    idx->is_sorted = is_sorted;

    /* Read arrays */
    if (count > 0) {
        if (fread(idx->hashes, sizeof(int64_t), count, f) != count) {
            fclose(f); dsmil_hash_index_destroy(idx); return NULL;
        }
        if (fread(idx->offsets, sizeof(uint64_t), count, f) != count) {
            fclose(f); dsmil_hash_index_destroy(idx); return NULL;
        }
        if (fread(idx->string_lens, sizeof(size_t), count, f) != count) {
            fclose(f); dsmil_hash_index_destroy(idx); return NULL;
        }

        /* Read string data */
        for (size_t i = 0; i < count; i++) {
            char* s = malloc(idx->string_lens[i] + 1);
            if (!s) { fclose(f); dsmil_hash_index_destroy(idx); return NULL; }
            if (idx->string_lens[i] > 0) {
                if (fread(s, 1, idx->string_lens[i], f) != idx->string_lens[i]) {
                    free(s); fclose(f); dsmil_hash_index_destroy(idx); return NULL;
                }
            }
            s[idx->string_lens[i]] = '\0';
            idx->strings[i] = s;
        }
    }

    fclose(f);

    /* Re-warm the anchor table for KEYSTONE acceleration */
    if (idx->count > 0 && idx->is_sorted) {
        keystone_config_t cfg;
        keystone_config_init(&cfg, KEYSTONE_WORKLOAD_IDS);
        keystone_search_enhanced(idx->hashes, idx->count,
                                  idx->hashes[idx->count / 2],
                                  idx->anchor_table, &cfg);
    }

    return idx;
}
