#include "../include/keystone_trigram.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdio.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define KS_X86 1
#else
#define KS_X86 0
#endif

#if KS_X86 && (defined(__GNUC__) || defined(__clang__))
#define KS_TARGET_AVX2      __attribute__((target("avx2")))
#define KS_TARGET_AVX512F   __attribute__((target("avx512f")))
#else
#define KS_TARGET_AVX2
#define KS_TARGET_AVX512F
#endif

/* SSE4.2 for SIMD-accelerated string scanning */
#ifdef __SSE4_2__
#include <nmmintrin.h>
#define HAVE_SSE42 1
#else
#define HAVE_SSE42 0
#endif

#define TRIGRAM_INITIAL_BUCKETS 65536u
#define TRIGRAM_INITIAL_POSTING_CAPACITY 8u
#define TRIGRAM_QUERY_MAX_UNIQUE 64u
/* 24-bit trigram space = 16M possible values. 2MB bitmap for dedup. */
#define TRIGRAM_BITMAP_BYTES (1u << 21) /* 2MB = 16M bits */
/* Sentinel for empty hash buckets. Outside the 24-bit trigram key range. */
#define TRIGRAM_KEY_EMPTY 0xFFFFFFFFu

static inline unsigned char fast_ascii_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32u) : c;
}

typedef struct keystone_trigram_posting_list {
    uint32_t* doc_ids;
    size_t count;
    size_t capacity;
    uint64_t* bitmap;
} keystone_trigram_posting_list_t;

typedef struct keystone_trigram_doc {
    uint32_t id;
    char* name;
    char* content;
    size_t content_len;
    bool owns_content;
} keystone_trigram_doc_t;

struct keystone_trigram_index {
    uint32_t flags;

    /* Split hash table: bucket_keys holds 4-byte keys (or TRIGRAM_KEY_EMPTY),
     * probed cache-efficiently during lookups. bucket_lists holds the inline
     * posting list structs, only dereferenced on a key match. */
    uint32_t* bucket_keys;
    keystone_trigram_posting_list_t* bucket_lists;
    size_t num_buckets;
    size_t unique_trigrams;

    /* Flattened contiguous posting array allocated at finalize() */
    uint32_t* flat_postings;
    size_t total_postings;

    keystone_trigram_doc_t* docs;
    size_t doc_count;
    size_t doc_capacity;

    /* Per-document trigram dedup bitmap. Allocated once, reused across
     * documents. 2MB covers the full 24-bit trigram space (16M values).
     * doc_seen_touched tracks which bytes were set so clearing is O(unique)
     * instead of O(2MB). This skips redundant hash lookups for trigrams
     * that already appeared in the current document. */
    unsigned char* doc_seen;
    size_t* doc_seen_touched;
    size_t doc_seen_touched_count;
    size_t doc_seen_touched_cap;

    bool is_finalized;
    bool failed;
    int failure_code;
    keystone_trigram_stats_t stats;
};

static bool checked_mul_size(size_t a, size_t b, size_t* out) {
    if (!out) return false;
    if (a != 0u && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static bool checked_add_size(size_t a, size_t b, size_t* out) {
    if (!out || b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

static size_t max_document_count(void) {
    size_t max_docs = (size_t)UINT32_MAX;
    if (sizeof(size_t) > sizeof(uint32_t)) max_docs += 1u;
    return max_docs;
}

static void secure_zero(void* ptr, size_t len) {
    volatile unsigned char* p = (volatile unsigned char*)ptr;
    while (ptr && len-- > 0u) {
        *p++ = 0u;
    }
}

static int poison_index(keystone_trigram_index_t* idx, int code) {
    if (idx) {
        idx->failed = true;
        if (idx->failure_code == KEYSTONE_TRIGRAM_OK) {
            idx->failure_code = code;
        }
    }
    return code;
}

static char* duplicate_c_string(const char* src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    size_t alloc_len;
    if (!checked_add_size(len, 1u, &alloc_len)) return NULL;
    char* copy = (char*)malloc(alloc_len);
    if (!copy) return NULL;
    memcpy(copy, src, alloc_len);
    return copy;
}

static char* duplicate_content(const char* text, size_t text_len) {
    size_t alloc_len;
    if (!checked_add_size(text_len, 1u, &alloc_len)) return NULL;
    char* copy = (char*)malloc(alloc_len);
    if (!copy) return NULL;
    if (text_len > 0u) memcpy(copy, text, text_len);
    copy[text_len] = '\0';
    return copy;
}

/* Fibonacci hashing: 1 multiply + 1 shift. 0x9E3779B1 = 2^32/phi (golden
 * ratio), giving excellent distribution for power-of-two table sizes. */
static inline size_t hash_trigram_key(uint32_t key, size_t num_buckets) {
    unsigned shift = 32u - (unsigned)__builtin_ctzll((unsigned long long)num_buckets);
    return (size_t)((key * 0x9E3779B1u) >> shift);
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int resize_trigram_hash_table(keystone_trigram_index_t* idx) {
    if (!idx || !idx->bucket_keys || !idx->bucket_lists || idx->num_buckets == 0u) {
        return KEYSTONE_TRIGRAM_EINVAL;
    }
    if (idx->num_buckets > SIZE_MAX / 2u) {
        return KEYSTONE_TRIGRAM_EOVERFLOW;
    }

    size_t new_num_buckets = idx->num_buckets * 2u;
    size_t keys_bytes;
    if (!checked_mul_size(new_num_buckets, sizeof(uint32_t), &keys_bytes)) {
        return KEYSTONE_TRIGRAM_EOVERFLOW;
    }
    size_t lists_bytes;
    if (!checked_mul_size(new_num_buckets, sizeof(keystone_trigram_posting_list_t), &lists_bytes)) {
        return KEYSTONE_TRIGRAM_EOVERFLOW;
    }

    uint32_t* new_keys = (uint32_t*)malloc(keys_bytes);
    if (!new_keys) return KEYSTONE_TRIGRAM_ENOMEM;
    memset(new_keys, 0xFF, keys_bytes);

    keystone_trigram_posting_list_t* new_lists =
        (keystone_trigram_posting_list_t*)calloc(
            new_num_buckets, sizeof(keystone_trigram_posting_list_t));
    if (!new_lists) {
        free(new_keys);
        return KEYSTONE_TRIGRAM_ENOMEM;
    }

    for (size_t i = 0u; i < idx->num_buckets; i++) {
        if (idx->bucket_keys[i] == TRIGRAM_KEY_EMPTY) continue;

        uint32_t key = idx->bucket_keys[i];
        size_t bucket_idx = hash_trigram_key(key, new_num_buckets);
        while (new_keys[bucket_idx] != TRIGRAM_KEY_EMPTY) {
            bucket_idx = (bucket_idx + 1u) & (new_num_buckets - 1u);
        }
        new_keys[bucket_idx] = key;
        new_lists[bucket_idx] = idx->bucket_lists[i];
    }

    free(idx->bucket_keys);
    free(idx->bucket_lists);
    idx->bucket_keys = new_keys;
    idx->bucket_lists = new_lists;
    idx->num_buckets = new_num_buckets;
    return KEYSTONE_TRIGRAM_OK;
}

keystone_trigram_index_t* keystone_trigram_index_create_options(
    size_t initial_doc_capacity,
    uint32_t flags
) {
    if (initial_doc_capacity == 0u) initial_doc_capacity = 64u;
    if (initial_doc_capacity > max_document_count()) return NULL;

    size_t ignored;
    if (!checked_mul_size(initial_doc_capacity, sizeof(keystone_trigram_doc_t), &ignored)) {
        return NULL;
    }

    keystone_trigram_index_t* idx =
        (keystone_trigram_index_t*)calloc(1u, sizeof(keystone_trigram_index_t));
    if (!idx) return NULL;

    idx->flags = flags;
    idx->num_buckets = TRIGRAM_INITIAL_BUCKETS;
    size_t keys_bytes;
    if (!checked_mul_size(idx->num_buckets, sizeof(uint32_t), &keys_bytes)) {
        free(idx);
        return NULL;
    }
    idx->bucket_keys = (uint32_t*)malloc(keys_bytes);
    if (!idx->bucket_keys) {
        free(idx);
        return NULL;
    }
    memset(idx->bucket_keys, 0xFF, keys_bytes);

    size_t lists_bytes;
    if (!checked_mul_size(idx->num_buckets, sizeof(keystone_trigram_posting_list_t), &lists_bytes)) {
        free(idx->bucket_keys);
        free(idx);
        return NULL;
    }
    idx->bucket_lists = (keystone_trigram_posting_list_t*)calloc(
        idx->num_buckets, sizeof(keystone_trigram_posting_list_t));
    if (!idx->bucket_lists) {
        free(idx->bucket_keys);
        free(idx);
        return NULL;
    }

    idx->doc_seen = (unsigned char*)calloc(TRIGRAM_BITMAP_BYTES, 1);
    if (!idx->doc_seen) {
        free(idx->bucket_lists);
        free(idx->bucket_keys);
        free(idx);
        return NULL;
    }
    idx->doc_seen_touched_cap = 4096u;
    idx->doc_seen_touched =
        (size_t*)malloc(idx->doc_seen_touched_cap * sizeof(size_t));
    if (!idx->doc_seen_touched) {
        free(idx->doc_seen);
        free(idx->bucket_lists);
        free(idx->bucket_keys);
        free(idx);
        return NULL;
    }

    idx->doc_capacity = initial_doc_capacity;
    idx->docs = (keystone_trigram_doc_t*)calloc(
        idx->doc_capacity, sizeof(keystone_trigram_doc_t));
    if (!idx->docs) {
        free(idx->doc_seen_touched);
        free(idx->doc_seen);
        free(idx->bucket_lists);
        free(idx->bucket_keys);
        free(idx);
        return NULL;
    }

    idx->failure_code = KEYSTONE_TRIGRAM_OK;
    return idx;
}

keystone_trigram_index_t* keystone_trigram_index_create(size_t initial_doc_capacity) {
    return keystone_trigram_index_create_options(initial_doc_capacity, KEYSTONE_TRIGRAM_OPT_NONE);
}

uint32_t keystone_trigram_index_get_flags(const keystone_trigram_index_t* idx) {
    return idx ? idx->flags : 0u;
}

void keystone_trigram_index_destroy(keystone_trigram_index_t* idx) {
    if (!idx) return;

    if (idx->bucket_lists) {
        for (size_t i = 0u; i < idx->num_buckets; i++) {
            if (idx->bucket_keys && idx->bucket_keys[i] != TRIGRAM_KEY_EMPTY) {
                if (!idx->flat_postings) {
                    free(idx->bucket_lists[i].doc_ids);
                }
                if (idx->bucket_lists[i].bitmap) {
                    free(idx->bucket_lists[i].bitmap);
                    idx->bucket_lists[i].bitmap = NULL;
                }
            }
        }
        free(idx->bucket_lists);
    }
    free(idx->flat_postings);
    free(idx->bucket_keys);

    free(idx->doc_seen_touched);
    free(idx->doc_seen);

    if (idx->docs) {
        for (size_t i = 0u; i < idx->doc_count; i++) {
            if (idx->docs[i].name) {
                secure_zero(idx->docs[i].name, strlen(idx->docs[i].name));
                free(idx->docs[i].name);
            }
            if (idx->docs[i].owns_content && idx->docs[i].content) {
                secure_zero(idx->docs[i].content, idx->docs[i].content_len);
                free(idx->docs[i].content);
            }
        }
        free(idx->docs);
    }

    secure_zero(idx, sizeof(*idx));
    free(idx);
}

#ifdef __SSE4_2__
/* Each output dword holds a trigram key packed little-endian as
 * (p[i+2] | p[i+1]<<8 | p[i]<<16); 0x80 lanes zero the high byte so
 * pshufb produces a clean 24-bit key. Four shuffles over a single
 * 16-byte load yield the 14 trigrams whose three-byte windows fit
 * entirely within that load (positions 0..13). */
static const unsigned char KEYSHUF_M0[16] = {2,1,0,0x80, 3,2,1,0x80, 4,3,2,0x80, 5,4,3,0x80};
static const unsigned char KEYSHUF_M1[16] = {6,5,4,0x80, 7,6,5,0x80, 8,7,6,0x80, 9,8,7,0x80};
static const unsigned char KEYSHUF_M2[16] = {10,9,8,0x80, 11,10,9,0x80, 12,11,10,0x80, 13,12,11,0x80};
static const unsigned char KEYSHUF_M3[16] = {14,13,12,0x80, 15,14,13,0x80, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80};

static inline __m128i tolower16(__m128i v) {
    __m128i a = _mm_set1_epi8((char)('A' - 0x80));
    __m128i z = _mm_set1_epi8((char)('Z' - 0x80));
    __m128i v_sub = _mm_add_epi8(v, _mm_set1_epi8(-0x80));
    __m128i ge_a = _mm_cmpgt_epi8(v_sub, _mm_add_epi8(a, _mm_set1_epi8(-1)));
    __m128i le_z = _mm_cmplt_epi8(v_sub, _mm_add_epi8(z, _mm_set1_epi8(1)));
    __m128i mask = _mm_and_si128(ge_a, le_z);
    return _mm_add_epi8(v, _mm_and_si128(mask, _mm_set1_epi8(32)));
}

static inline void keystone_extract_trigrams16_folded(const unsigned char* p, uint32_t* out, bool fold) {
    __m128i v = _mm_loadu_si128((const __m128i*)p);
    if (fold) {
        v = tolower16(v);
    }
    _mm_storeu_si128((__m128i*)(out + 0),  _mm_shuffle_epi8(v, _mm_loadu_si128((const __m128i*)KEYSHUF_M0)));
    _mm_storeu_si128((__m128i*)(out + 4),  _mm_shuffle_epi8(v, _mm_loadu_si128((const __m128i*)KEYSHUF_M1)));
    _mm_storeu_si128((__m128i*)(out + 8),  _mm_shuffle_epi8(v, _mm_loadu_si128((const __m128i*)KEYSHUF_M2)));
    _mm_storeu_si128((__m128i*)(out + 12), _mm_shuffle_epi8(v, _mm_loadu_si128((const __m128i*)KEYSHUF_M3)));
}

static inline void keystone_extract_trigrams16(const unsigned char* p, uint32_t* out) {
    keystone_extract_trigrams16_folded(p, out, false);
}
#endif

static size_t keystone_trigram_extract_internal(
    const char* pattern,
    size_t pattern_len,
    uint32_t* out_trigrams,
    size_t max_trigrams,
    bool fold
) {
    if (!pattern || pattern_len < 3u || !out_trigrams || max_trigrams == 0u) return 0u;

    const unsigned char* p = (const unsigned char*)pattern;
    size_t extracted = 0u;
    size_t i = 0u;

#ifdef __SSE4_2__
    while (i + 16u <= pattern_len && extracted < max_trigrams) {
        uint32_t keys[16];
        keystone_extract_trigrams16_folded(p + i, keys, fold);
        for (int k = 0; k < 14 && extracted < max_trigrams; k++) {
            uint32_t key = keys[k];
            bool dup = false;
            for (size_t j = 0u; j < extracted; j++) {
                if (out_trigrams[j] == key) { dup = true; break; }
            }
            if (!dup) {
                out_trigrams[extracted++] = key;
            }
        }
        i += 14u;
    }
#endif
    for (; i <= pattern_len - 3u && extracted < max_trigrams; i++) {
        uint32_t b0 = fold ? (uint32_t)fast_ascii_tolower(p[i]) : (uint32_t)p[i];
        uint32_t b1 = fold ? (uint32_t)fast_ascii_tolower(p[i + 1u]) : (uint32_t)p[i + 1u];
        uint32_t b2 = fold ? (uint32_t)fast_ascii_tolower(p[i + 2u]) : (uint32_t)p[i + 2u];
        uint32_t key = (b0 << 16) | (b1 << 8) | b2;
        bool dup = false;
        for (size_t j = 0u; j < extracted; j++) {
            if (out_trigrams[j] == key) { dup = true; break; }
        }
        if (!dup) {
            out_trigrams[extracted++] = key;
        }
    }

    return extracted;
}

size_t keystone_trigram_extract(
    const char* pattern,
    size_t pattern_len,
    uint32_t* out_trigrams,
    size_t max_trigrams
) {
    return keystone_trigram_extract_internal(pattern, pattern_len, out_trigrams, max_trigrams, false);
}

static keystone_trigram_posting_list_t* find_or_create_posting_list(
    keystone_trigram_index_t* idx,
    uint32_t key
) {
    if (!idx || idx->failed) return NULL;

    /* Resize at approximately 70% occupancy. Never continue after a failed
     * resize: silently degrading a candidate index can create false negatives. */
    size_t resize_threshold = (idx->num_buckets / 10u) * 7u;
    if (resize_threshold == 0u) resize_threshold = 1u;
    if (idx->unique_trigrams >= resize_threshold) {
        int rc = resize_trigram_hash_table(idx);
        if (rc != KEYSTONE_TRIGRAM_OK) {
            poison_index(idx, rc);
            return NULL;
        }
    }

    size_t bucket_idx = hash_trigram_key(key, idx->num_buckets);
    size_t mask = idx->num_buckets - 1u;
    size_t original = bucket_idx;

    while (idx->bucket_keys[bucket_idx] != TRIGRAM_KEY_EMPTY &&
           idx->bucket_keys[bucket_idx] != key) {
        bucket_idx = (bucket_idx + 1u) & mask;
        if (bucket_idx == original) {
            poison_index(idx, KEYSTONE_TRIGRAM_ESTATE);
            return NULL;
        }
    }

    keystone_trigram_posting_list_t* plist = &idx->bucket_lists[bucket_idx];
    if (idx->bucket_keys[bucket_idx] == TRIGRAM_KEY_EMPTY) {
        size_t bytes;
        if (!checked_mul_size(TRIGRAM_INITIAL_POSTING_CAPACITY, sizeof(uint32_t), &bytes)) {
            poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
            return NULL;
        }
        uint32_t* ids = (uint32_t*)malloc(bytes);
        if (!ids) {
            poison_index(idx, KEYSTONE_TRIGRAM_ENOMEM);
            return NULL;
        }

        plist->doc_ids = ids;
        plist->count = 0u;
        plist->capacity = TRIGRAM_INITIAL_POSTING_CAPACITY;
        idx->bucket_keys[bucket_idx] = key;
        idx->unique_trigrams++;
    }

    return plist;
}

static int add_doc_to_posting_list(
    keystone_trigram_index_t* idx,
    keystone_trigram_posting_list_t* plist,
    uint32_t doc_id
) {
    if (!idx || !plist || !plist->doc_ids || plist->capacity == 0u) {
        return poison_index(idx, KEYSTONE_TRIGRAM_ESTATE);
    }

    if (plist->count > 0u && plist->doc_ids[plist->count - 1u] == doc_id) {
        return KEYSTONE_TRIGRAM_OK;
    }

    if (plist->count >= plist->capacity) {
        if (plist->capacity > SIZE_MAX / 2u) {
            return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
        }
        size_t new_cap = plist->capacity * 2u;
        size_t bytes;
        if (!checked_mul_size(new_cap, sizeof(uint32_t), &bytes)) {
            return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
        }
        uint32_t* new_ids = (uint32_t*)realloc(plist->doc_ids, bytes);
        if (!new_ids) return poison_index(idx, KEYSTONE_TRIGRAM_ENOMEM);
        plist->doc_ids = new_ids;
        plist->capacity = new_cap;
    }

    plist->doc_ids[plist->count++] = doc_id;
    return KEYSTONE_TRIGRAM_OK;
}

static int ensure_doc_capacity(keystone_trigram_index_t* idx) {
    if (!idx) return KEYSTONE_TRIGRAM_EINVAL;
    if (idx->doc_count < idx->doc_capacity) return KEYSTONE_TRIGRAM_OK;

    if (idx->doc_capacity > SIZE_MAX / 2u) {
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }
    size_t new_cap = idx->doc_capacity * 2u;
    size_t max_docs = max_document_count();
    if (new_cap > max_docs) new_cap = max_docs;
    if (new_cap <= idx->doc_capacity) {
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }

    size_t bytes;
    if (!checked_mul_size(new_cap, sizeof(keystone_trigram_doc_t), &bytes)) {
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }
    keystone_trigram_doc_t* new_docs =
        (keystone_trigram_doc_t*)realloc(idx->docs, bytes);
    if (!new_docs) return poison_index(idx, KEYSTONE_TRIGRAM_ENOMEM);

    memset(new_docs + idx->doc_capacity, 0,
           (new_cap - idx->doc_capacity) * sizeof(keystone_trigram_doc_t));
    idx->docs = new_docs;
    idx->doc_capacity = new_cap;
    return KEYSTONE_TRIGRAM_OK;
}

static void clear_doc_seen(keystone_trigram_index_t* idx) {
    for (size_t t = 0u; t < idx->doc_seen_touched_count; t++) {
        idx->doc_seen[idx->doc_seen_touched[t]] = 0u;
    }
    idx->doc_seen_touched_count = 0u;
}

/* Per-trigram dedup + posting-list append. Factored so both the SIMD
 * batch path and the scalar tail share one copy of the bitmap logic.
 * `continue` inside the do/while exits the block and advances the
 * enclosing for-loop to the next trigram. */
#define KEYSTONE_EMIT_TRIGRAM(KEY) do { \
    size_t byte_idx = (KEY) >> 3; \
    unsigned char bit_mask = (unsigned char)(1u << ((KEY) & 7u)); \
    unsigned char old = idx->doc_seen[byte_idx]; \
    if (old & bit_mask) continue; \
    if (old == 0u) { \
        if (idx->doc_seen_touched_count >= idx->doc_seen_touched_cap) { \
            size_t new_cap = idx->doc_seen_touched_cap * 2u; \
            size_t new_bytes; \
            if (!checked_mul_size(new_cap, sizeof(size_t), &new_bytes)) { \
                rc = KEYSTONE_TRIGRAM_EOVERFLOW; \
                goto fail_prepared_doc; \
            } \
            size_t* new_touched = \
                (size_t*)realloc(idx->doc_seen_touched, new_bytes); \
            if (!new_touched) { \
                rc = KEYSTONE_TRIGRAM_ENOMEM; \
                goto fail_prepared_doc; \
            } \
            idx->doc_seen_touched = new_touched; \
            idx->doc_seen_touched_cap = new_cap; \
        } \
        idx->doc_seen_touched[idx->doc_seen_touched_count++] = byte_idx; \
    } \
    idx->doc_seen[byte_idx] = old | bit_mask; \
    keystone_trigram_posting_list_t* plist = find_or_create_posting_list(idx, (KEY)); \
    if (!plist) { \
        rc = idx->failure_code ? idx->failure_code : KEYSTONE_TRIGRAM_ESTATE; \
        goto fail_prepared_doc; \
    } \
    rc = add_doc_to_posting_list(idx, plist, doc_id); \
    if (rc != KEYSTONE_TRIGRAM_OK) goto fail_prepared_doc; \
} while (0)

static int add_document_internal(
    keystone_trigram_index_t* idx,
    const char* name,
    const char* text,
    size_t text_len,
    bool retain_content,
    uint32_t* out_doc_id
) {
    if (!idx || !text) return KEYSTONE_TRIGRAM_EINVAL;
    if (idx->failed) return idx->failure_code ? idx->failure_code : KEYSTONE_TRIGRAM_ESTATE;
    if (idx->is_finalized) return KEYSTONE_TRIGRAM_ESTATE;
    if (idx->doc_count > (size_t)UINT32_MAX) {
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }
    if (text_len > SIZE_MAX - idx->stats.bytes_indexed) {
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }

    int rc = ensure_doc_capacity(idx);
    if (rc != KEYSTONE_TRIGRAM_OK) return rc;

    char* name_copy = NULL;
    char* content_copy = NULL;
    if (name) {
        name_copy = duplicate_c_string(name);
        if (!name_copy) return poison_index(idx, KEYSTONE_TRIGRAM_ENOMEM);
    }
    if (retain_content) {
        content_copy = duplicate_content(text, text_len);
        if (!content_copy) {
            if (name_copy) {
                secure_zero(name_copy, strlen(name_copy));
                free(name_copy);
            }
            return poison_index(idx, KEYSTONE_TRIGRAM_ENOMEM);
        }
    }

    uint64_t start_time = get_time_ns();
    uint32_t doc_id = (uint32_t)idx->doc_count;

    bool fold = (idx->flags & KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE) != 0;

    if (text_len >= 3u) {
        const unsigned char* p = (const unsigned char*)text;
        size_t i = 0u;
#ifdef __SSE4_2__
        while (i + 16u <= text_len) {
            uint32_t keys[16];
            keystone_extract_trigrams16_folded(p + i, keys, fold);
            for (int k = 0; k < 14; k++) {
                KEYSTONE_EMIT_TRIGRAM(keys[k]);
            }
            i += 14u;
        }
#endif
        for (; i <= text_len - 3u; i++) {
            uint32_t b0 = fold ? (uint32_t)fast_ascii_tolower(p[i]) : (uint32_t)p[i];
            uint32_t b1 = fold ? (uint32_t)fast_ascii_tolower(p[i + 1u]) : (uint32_t)p[i + 1u];
            uint32_t b2 = fold ? (uint32_t)fast_ascii_tolower(p[i + 2u]) : (uint32_t)p[i + 2u];
            uint32_t key = (b0 << 16) | (b1 << 8) | b2;
            KEYSTONE_EMIT_TRIGRAM(key);
        }
    }

    clear_doc_seen(idx);

    keystone_trigram_doc_t* doc = &idx->docs[idx->doc_count];
    doc->id = doc_id;
    doc->name = name_copy;
    doc->content = content_copy;
    doc->content_len = text_len;
    doc->owns_content = retain_content;
    idx->doc_count++;

    idx->stats.total_documents = idx->doc_count;
    idx->stats.bytes_indexed += text_len;
    uint64_t end_time = get_time_ns();
    if (end_time >= start_time && UINT64_MAX - idx->stats.build_time_ns >= end_time - start_time) {
        idx->stats.build_time_ns += end_time - start_time;
    } else {
        idx->stats.build_time_ns = UINT64_MAX;
    }

    if (out_doc_id) *out_doc_id = doc_id;
    return KEYSTONE_TRIGRAM_OK;

fail_prepared_doc:
    clear_doc_seen(idx);
    if (content_copy) {
        secure_zero(content_copy, text_len);
        free(content_copy);
    }
    if (name_copy) {
        secure_zero(name_copy, strlen(name_copy));
        free(name_copy);
    }
    return poison_index(idx, rc == KEYSTONE_TRIGRAM_OK ? KEYSTONE_TRIGRAM_ESTATE : rc);
}

int keystone_trigram_index_add_document(
    keystone_trigram_index_t* idx,
    const char* name,
    const char* text,
    size_t text_len,
    uint32_t* out_doc_id
) {
    return add_document_internal(idx, name, text, text_len, true, out_doc_id);
}

int keystone_trigram_index_add_document_external(
    keystone_trigram_index_t* idx,
    const char* name,
    const char* text,
    size_t text_len,
    uint32_t* out_doc_id
) {
    return add_document_internal(idx, name, text, text_len, false, out_doc_id);
}

static int compare_uint32(const void* a, const void* b) {
    uint32_t u1 = *(const uint32_t*)a;
    uint32_t u2 = *(const uint32_t*)b;
    return (u1 > u2) - (u1 < u2);
}

int keystone_trigram_index_finalize(keystone_trigram_index_t* idx) {
    if (!idx) return KEYSTONE_TRIGRAM_EINVAL;
    if (idx->failed) return idx->failure_code ? idx->failure_code : KEYSTONE_TRIGRAM_ESTATE;
    if (idx->is_finalized) return KEYSTONE_TRIGRAM_OK;

    /* Doc IDs are assigned in insertion order and deduplicated per-document.
     * Posting lists are already sorted ascending. No sort needed. */
    size_t total_postings = 0u;
    for (size_t i = 0u; i < idx->num_buckets; i++) {
        if (idx->bucket_keys[i] == TRIGRAM_KEY_EMPTY) continue;
        keystone_trigram_posting_list_t* plist = &idx->bucket_lists[i];
        if (plist->count == 0u) continue;
        if (!plist->doc_ids || plist->count > plist->capacity) {
            return poison_index(idx, KEYSTONE_TRIGRAM_ESTATE);
        }
        if (plist->count > SIZE_MAX - total_postings) {
            return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
        }
        total_postings += plist->count;
    }

    /* Phase 2: Contiguous flattened postings pool */
    if (total_postings > 0u) {
        size_t bytes;
        if (!checked_mul_size(total_postings, sizeof(uint32_t), &bytes)) {
            return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
        }
        uint32_t* flat = (uint32_t*)malloc(bytes);
        if (!flat) {
            return poison_index(idx, KEYSTONE_TRIGRAM_ENOMEM);
        }

        size_t offset = 0u;
        for (size_t i = 0u; i < idx->num_buckets; i++) {
            if (idx->bucket_keys[i] == TRIGRAM_KEY_EMPTY) continue;
            keystone_trigram_posting_list_t* plist = &idx->bucket_lists[i];
            if (plist->count == 0u) continue;

            memcpy(flat + offset, plist->doc_ids, plist->count * sizeof(uint32_t));
            free(plist->doc_ids);
            plist->doc_ids = flat + offset;
            plist->capacity = plist->count;
            offset += plist->count;
        }
        idx->flat_postings = flat;
        idx->total_postings = total_postings;
    }

    /* Phase 2: Dense Posting Bitmap Conversion
     * Convert high-frequency trigrams (count >= doc_count / 32, with doc_count >= 64)
     * to 64-bit word bitmaps for O(1) bit tests. */
    if (idx->doc_count >= 64u) {
        size_t threshold = idx->doc_count / 32u;
        if (threshold < 64u) threshold = 64u;
        size_t bitmap_words = (idx->doc_count + 63u) / 64u;

        for (size_t i = 0u; i < idx->num_buckets; i++) {
            if (idx->bucket_keys[i] == TRIGRAM_KEY_EMPTY) continue;
            keystone_trigram_posting_list_t* plist = &idx->bucket_lists[i];
            if (plist->count >= threshold) {
                uint64_t* bm = (uint64_t*)calloc(bitmap_words, sizeof(uint64_t));
                if (bm) {
                    for (size_t k = 0u; k < plist->count; k++) {
                        uint32_t did = plist->doc_ids[k];
                        bm[did >> 6u] |= (UINT64_C(1) << (did & 63u));
                    }
                    plist->bitmap = bm;
                }
            }
        }
    }

    idx->stats.unique_trigrams = idx->unique_trigrams;
    idx->stats.total_postings = total_postings;
    idx->is_finalized = true;

    /* Release build-only deduplication bitsets to reclaim memory */
    if (idx->doc_seen) {
        free(idx->doc_seen);
        idx->doc_seen = NULL;
    }
    if (idx->doc_seen_touched) {
        free(idx->doc_seen_touched);
        idx->doc_seen_touched = NULL;
    }
    idx->doc_seen_touched_count = 0u;
    idx->doc_seen_touched_cap = 0u;

    return KEYSTONE_TRIGRAM_OK;
}

static const keystone_trigram_posting_list_t* get_posting_list(
    const keystone_trigram_index_t* idx,
    uint32_t key
) {
    if (!idx || !idx->bucket_keys || idx->num_buckets == 0u) return NULL;

    size_t mask = idx->num_buckets - 1u;
    size_t bucket_idx = hash_trigram_key(key, idx->num_buckets);
    size_t original = bucket_idx;

    while (idx->bucket_keys[bucket_idx] != TRIGRAM_KEY_EMPTY) {
        if (idx->bucket_keys[bucket_idx] == key) {
            return &idx->bucket_lists[bucket_idx];
        }
        bucket_idx = (bucket_idx + 1u) & mask;
        if (bucket_idx == original) break;
    }

    return NULL;
}

size_t keystone_trigram_index_document_count(const keystone_trigram_index_t* idx) {
    return idx ? idx->doc_count : 0u;
}

/*
 * Monotonic lower_bound with exponential bracketing (galloping search).
 * Successive probes advance from the previous position without restarting from 0.
 */
static size_t ks_lower_bound_gallop_u32(
    const uint32_t *a, size_t n, size_t pos, uint32_t target
) {
    if (pos >= n || a[pos] >= target) {
        return pos;
    }

    size_t lo = pos + 1u;
    size_t step = 1u;

    while (lo < n) {
        size_t probe;
        if (step > n - lo) {
            probe = n;
        } else {
            probe = lo + step;
        }

        if (probe == n || a[probe - 1u] >= target) {
            size_t hi = probe;
            while (lo < hi) {
                size_t mid = lo + ((hi - lo) >> 1);
                if (a[mid] < target) {
                    lo = mid + 1u;
                } else {
                    hi = mid;
                }
            }
            return lo;
        }

        lo = probe;
        if (step > SIZE_MAX / 2u) {
            step = n - lo;
        } else {
            step <<= 1;
        }
    }

    return n;
}

static size_t ks_intersect_u32_scalar(
    const uint32_t *a, size_t na,
    const uint32_t *b, size_t nb,
    uint32_t *out, size_t out_cap
) {
    size_t ia = 0u;
    size_t ib = 0u;
    size_t no = 0u;

    while (ia < na && ib < nb && no < out_cap) {
        uint32_t va = a[ia];
        uint32_t vb = b[ib];

        if (va == vb) {
            out[no++] = va;
            ia++;
            ib++;
        } else if (va < vb) {
            ia++;
        } else {
            ib++;
        }
    }

    return no;
}

static size_t ks_intersect_u32_galloping(
    const uint32_t *a, size_t na,
    const uint32_t *b, size_t nb,
    uint32_t *out, size_t out_cap
) {
    const uint32_t *small = a;
    const uint32_t *large = b;
    size_t ns = na;
    size_t nl = nb;

    if (na > nb) {
        small = b;
        large = a;
        ns = nb;
        nl = na;
    }

    size_t lp = 0u;
    size_t no = 0u;

    for (size_t i = 0u; i < ns && lp < nl && no < out_cap; i++) {
        uint32_t target = small[i];
        lp = ks_lower_bound_gallop_u32(large, nl, lp, target);
        if (lp == nl) break;

        if (large[lp] == target) {
            out[no++] = target;
            lp++;
        }
    }

    return no;
}

#if KS_X86
KS_TARGET_AVX2
static inline unsigned ks_match8_against_block_avx2(const uint32_t *source, __m256i other) {
    unsigned hits = 0u;
    for (unsigned lane = 0u; lane < 8u; lane++) {
        __m256i x = _mm256_set1_epi32((int)source[lane]);
        __m256i eq = _mm256_cmpeq_epi32(x, other);
        if (_mm256_movemask_ps(_mm256_castsi256_ps(eq)) != 0) {
            hits |= 1u << lane;
        }
    }
    return hits;
}

KS_TARGET_AVX2
static size_t ks_intersect_u32_avx2(
    const uint32_t *a, size_t na,
    const uint32_t *b, size_t nb,
    uint32_t *out, size_t out_cap
) {
    size_t ia = 0u;
    size_t ib = 0u;
    size_t no = 0u;

    while (ia + 8u <= na && ib + 8u <= nb && no < out_cap) {
        if (a[ia + 7u] < b[ib]) {
            ia += 8u;
            continue;
        }
        if (b[ib + 7u] < a[ia]) {
            ib += 8u;
            continue;
        }

        uint32_t max_a = a[ia + 7u];
        uint32_t max_b = b[ib + 7u];

        if (max_a <= max_b) {
            __m256i vb = _mm256_loadu_si256((const __m256i *)(const void *)(b + ib));
            unsigned hits = ks_match8_against_block_avx2(a + ia, vb);

            while (hits != 0u && no < out_cap) {
                unsigned lane = (unsigned)__builtin_ctz(hits);
                hits &= hits - 1u;
                out[no++] = a[ia + lane];
            }
            ia += 8u;
        } else {
            __m256i va = _mm256_loadu_si256((const __m256i *)(const void *)(a + ia));
            unsigned hits = ks_match8_against_block_avx2(b + ib, va);

            while (hits != 0u && no < out_cap) {
                unsigned lane = (unsigned)__builtin_ctz(hits);
                hits &= hits - 1u;
                out[no++] = b[ib + lane];
            }
            ib += 8u;
        }
    }

    if (no < out_cap) {
        no += ks_intersect_u32_scalar(
            a + ia, na - ia, b + ib, nb - ib,
            out + no, out_cap - no);
    }

    return no;
}
#endif

static size_t ks_intersect_u32_adaptive(
    const uint32_t *a, size_t na,
    const uint32_t *b, size_t nb,
    uint32_t *out, size_t out_cap
) {
    if (!a || !b || !out || out_cap == 0u || na == 0u || nb == 0u) {
        return 0u;
    }

    size_t smaller = na < nb ? na : nb;
    size_t larger = na < nb ? nb : na;

    if (smaller == 0u || larger / smaller >= 16u) {
        return ks_intersect_u32_galloping(a, na, b, nb, out, out_cap);
    }

#if KS_X86 && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        return ks_intersect_u32_avx2(a, na, b, nb, out, out_cap);
    }
#endif

    return ks_intersect_u32_scalar(a, na, b, nb, out, out_cap);
}

size_t keystone_trigram_index_get_candidates(
    const keystone_trigram_index_t* idx,
    const char* pattern,
    size_t pattern_len,
    uint32_t* out_candidates,
    size_t max_candidates
) {
    if (!idx || idx->failed || !idx->is_finalized || !pattern ||
        !out_candidates || max_candidates == 0u) {
        return 0u;
    }

    if (pattern_len < 3u) {
        size_t count = idx->doc_count < max_candidates ? idx->doc_count : max_candidates;
        for (size_t i = 0u; i < count; i++) out_candidates[i] = (uint32_t)i;
        return count;
    }

    bool fold = (idx->flags & KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE) != 0;
    uint32_t query_trigrams[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t num_trigrams = keystone_trigram_extract_internal(
        pattern, pattern_len, query_trigrams, TRIGRAM_QUERY_MAX_UNIQUE, fold);
    if (num_trigrams == 0u) return 0u;

    const keystone_trigram_posting_list_t* lists[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t num_lists = 0u;
    for (size_t i = 0u; i < num_trigrams; i++) {
        const keystone_trigram_posting_list_t* plist =
            get_posting_list(idx, query_trigrams[i]);
        if (!plist || plist->count == 0u) return 0u;
        lists[num_lists++] = plist;
    }

    /* Sort by size (smallest first) */
    for (size_t i = 0u; i < num_lists; i++) {
        for (size_t j = i + 1u; j < num_lists; j++) {
            if (lists[j]->count < lists[i]->count) {
                const keystone_trigram_posting_list_t* tmp = lists[i];
                lists[i] = lists[j];
                lists[j] = tmp;
            }
        }
    }

    /* Prune to 16 rarest lists to minimize intersection work */
    if (num_lists > 16u) {
        num_lists = 16u;
    }

    if (num_lists == 1u) {
        size_t count = lists[0]->count < max_candidates ? lists[0]->count : max_candidates;
        memcpy(out_candidates, lists[0]->doc_ids, count * sizeof(uint32_t));
        return count;
    }

    if (num_lists == 2u) {
        if (lists[0]->bitmap && lists[1]->bitmap) {
            size_t words = (idx->doc_count + 63u) / 64u;
            size_t count = 0u;
            const uint64_t* bm0 = lists[0]->bitmap;
            const uint64_t* bm1 = lists[1]->bitmap;
            for (size_t w = 0u; w < words && count < max_candidates; w++) {
                uint64_t word = bm0[w] & bm1[w];
                while (word && count < max_candidates) {
                    unsigned bit = (unsigned)__builtin_ctzll(word);
                    uint32_t doc_id = (uint32_t)(w * 64u + bit);
                    if (doc_id < idx->doc_count) {
                        out_candidates[count++] = doc_id;
                    }
                    word &= word - 1u;
                }
            }
            return count;
        } else if (lists[1]->bitmap) {
            const uint64_t* bm = lists[1]->bitmap;
            size_t count = 0u;
            for (size_t i = 0u; i < lists[0]->count && count < max_candidates; i++) {
                uint32_t doc_id = lists[0]->doc_ids[i];
                if ((bm[doc_id >> 6u] & (UINT64_C(1) << (doc_id & 63u))) != 0) {
                    out_candidates[count++] = doc_id;
                }
            }
            return count;
        }
        return ks_intersect_u32_adaptive(
            lists[0]->doc_ids, lists[0]->count,
            lists[1]->doc_ids, lists[1]->count,
            out_candidates, max_candidates
        );
    }

    /* Galloping / bitmap intersection across rarest lists */
    const keystone_trigram_posting_list_t* base = lists[0];
    size_t candidate_count = 0u;
    size_t cursor[TRIGRAM_QUERY_MAX_UNIQUE];
    for (size_t l = 0u; l < num_lists; l++) cursor[l] = 0u;

    for (size_t i = 0u; i < base->count; i++) {
        uint32_t doc_id = base->doc_ids[i];
        bool in_all = true;

        for (size_t l = 1u; l < num_lists; l++) {
            const keystone_trigram_posting_list_t* plist = lists[l];
            if (plist->bitmap) {
                if ((plist->bitmap[doc_id >> 6u] & (UINT64_C(1) << (doc_id & 63u))) == 0) {
                    in_all = false;
                    break;
                }
            } else {
                size_t pos = ks_lower_bound_gallop_u32(
                    plist->doc_ids, plist->count, cursor[l], doc_id);

                cursor[l] = pos;
                if (pos == plist->count || plist->doc_ids[pos] != doc_id) {
                    in_all = false;
                    break;
                }
                cursor[l] = pos + 1u;
            }
        }

        if (in_all) {
            out_candidates[candidate_count++] = doc_id;
            if (candidate_count >= max_candidates) break;
        }
    }

    return candidate_count;
}

/* SIMD-accelerated substring search.
 * For needles >= 16 bytes the first 16 bytes are compared directly with
 * a single XMM compare, an extremely selective filter. For shorter
 * needles the first and last byte are scanned simultaneously: a
 * candidate position must match both the first byte (at offset 0) and
 * the last byte (at offset needle_len-1), eliminating most false
 * positives before memcmp touches the middle. Falls back to a
 * byte-at-a-time scan when SSE4.2 is unavailable. */
static const void* bounded_memmem(
    const void* haystack,
    size_t haystack_len,
    const void* needle,
    size_t needle_len
) {
    if (!haystack || !needle || needle_len == 0u || haystack_len < needle_len) return NULL;

    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    size_t limit = haystack_len - needle_len;

    if (needle_len == 1u) {
        return memchr(haystack, n[0], haystack_len);
    }

#if HAVE_SSE42
    __m128i first_byte = _mm_set1_epi8((char)n[0]);
    __m128i last_byte = _mm_set1_epi8((char)n[needle_len - 1u]);
    __m128i n16 = (needle_len >= 16u)
        ? _mm_loadu_si128((const __m128i*)n)
        : _mm_setzero_si128();
    size_t i = 0u;
    while (i + 16u <= limit) {
        __m128i chunk_f = _mm_loadu_si128((const __m128i*)(h + i));
        __m128i chunk_l = _mm_loadu_si128((const __m128i*)(h + i + needle_len - 1u));
        unsigned int mask_f = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk_f, first_byte));
        unsigned int mask_l = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk_l, last_byte));
        unsigned int mask = mask_f & mask_l;
        while (mask) {
            int bit = __builtin_ctz(mask);
            mask &= mask - 1;
            size_t pos = i + (size_t)bit;
            if (pos > limit) continue;
#ifdef __SSE4_2__
            if (needle_len >= 16u) {
                __m128i h16 = _mm_loadu_si128((const __m128i*)(h + pos));
                if ((unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(h16, n16)) != 0xFFFFu)
                    continue;
                if (needle_len > 16u &&
                    memcmp(h + pos + 16u, n + 16u, needle_len - 16u) != 0)
                    continue;
                return h + pos;
            }
#endif
            if (memcmp(h + pos, n, needle_len) == 0)
                return h + pos;
        }
        i += 16u;
    }
    for (; i <= limit; i++) {
#else
    for (size_t i = 0u; i <= limit; i++) {
#endif
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0)
            return h + i;
    }
    return NULL;
}

static const void* bounded_memmem_ci(
    const void* haystack,
    size_t haystack_len,
    const void* needle,
    size_t needle_len
) {
    if (!haystack || !needle || needle_len == 0u || haystack_len < needle_len) return NULL;

    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    size_t limit = haystack_len - needle_len;

    unsigned char needle_buf[256];
    unsigned char* folded_needle = needle_buf;
    if (needle_len > sizeof(needle_buf)) {
        folded_needle = (unsigned char*)malloc(needle_len);
        if (!folded_needle) return NULL;
    }
    for (size_t k = 0u; k < needle_len; k++) {
        folded_needle[k] = fast_ascii_tolower(n[k]);
    }

    const unsigned char target0 = folded_needle[0];
    for (size_t i = 0u; i <= limit; i++) {
        if (fast_ascii_tolower(h[i]) == target0) {
            size_t match = 1u;
            while (match < needle_len && fast_ascii_tolower(h[i + match]) == folded_needle[match]) {
                match++;
            }
            if (match == needle_len) {
                if (folded_needle != needle_buf) free(folded_needle);
                return h + i;
            }
        }
    }

    if (folded_needle != needle_buf) free(folded_needle);
    return NULL;
}

size_t keystone_trigram_index_search(
    keystone_trigram_index_t* idx,
    const char* pattern,
    size_t pattern_len,
    uint32_t* out_matches,
    size_t max_matches
) {
    if (!idx || idx->failed || !idx->is_finalized || !pattern || pattern_len == 0u ||
        !out_matches || max_matches == 0u) {
        return 0u;
    }

    idx->stats.total_searches++;
    if (idx->doc_count == 0u) return 0u;

    bool ci = (idx->flags & KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE) != 0;

    /* Patterns < 3 bytes: every document is a candidate. Directly scan docs without allocation. */
    if (pattern_len < 3u) {
        size_t matches = 0u;
        idx->stats.candidate_docs_evaluated += idx->doc_count;
        for (size_t doc_id = 0u; doc_id < idx->doc_count; doc_id++) {
            keystone_trigram_doc_t* doc = &idx->docs[doc_id];
            if (!doc->owns_content || !doc->content || doc->content_len < pattern_len) continue;

            const void* found = ci ?
                bounded_memmem_ci(doc->content, doc->content_len, pattern, pattern_len) :
                bounded_memmem(doc->content, doc->content_len, pattern, pattern_len);
            if (found) {
                out_matches[matches++] = (uint32_t)doc_id;
                if (matches >= max_matches) break;
            }
        }
        return matches;
    }

    /* Extract trigrams and find rarest posting list to bound candidate allocation */
    uint32_t query_trigrams[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t num_trigrams = keystone_trigram_extract_internal(
        pattern, pattern_len, query_trigrams, TRIGRAM_QUERY_MAX_UNIQUE, ci);
    if (num_trigrams == 0u) return 0u;

    size_t rarest_count = SIZE_MAX;
    for (size_t i = 0u; i < num_trigrams; i++) {
        const keystone_trigram_posting_list_t* plist = get_posting_list(idx, query_trigrams[i]);
        if (!plist || plist->count == 0u) {
            idx->stats.candidate_docs_rejected += idx->doc_count;
            return 0u;
        }
        if (plist->count < rarest_count) {
            rarest_count = plist->count;
        }
    }

    size_t max_cands = rarest_count < idx->doc_count ? rarest_count : idx->doc_count;
    #define KS_STACK_CANDS_CAP 1024u
    uint32_t stack_cands[KS_STACK_CANDS_CAP];
    uint32_t* candidates = stack_cands;
    bool heap_allocated = false;

    if (max_cands > KS_STACK_CANDS_CAP) {
        size_t candidate_bytes;
        if (!checked_mul_size(max_cands, sizeof(uint32_t), &candidate_bytes)) return 0u;
        candidates = (uint32_t*)malloc(candidate_bytes);
        if (!candidates) return 0u;
        heap_allocated = true;
    }

    size_t num_candidates = keystone_trigram_index_get_candidates(
        idx, pattern, pattern_len, candidates, max_cands);

    idx->stats.candidate_docs_evaluated += num_candidates;
    idx->stats.candidate_docs_rejected += (idx->doc_count > num_candidates) ? (idx->doc_count - num_candidates) : 0u;

    size_t matches = 0u;
    for (size_t i = 0u; i < num_candidates; i++) {
        uint32_t doc_id = candidates[i];
        if ((size_t)doc_id >= idx->doc_count) {
            poison_index(idx, KEYSTONE_TRIGRAM_ESTATE);
            break;
        }

        keystone_trigram_doc_t* doc = &idx->docs[doc_id];
        if (!doc->owns_content || !doc->content || doc->content_len < pattern_len) continue;

        const void* found = ci ?
            bounded_memmem_ci(doc->content, doc->content_len, pattern, pattern_len) :
            bounded_memmem(doc->content, doc->content_len, pattern, pattern_len);
        if (found) {
            out_matches[matches++] = doc_id;
            if (matches >= max_matches) break;
        }
    }

    if (heap_allocated) {
        secure_zero(candidates, max_cands * sizeof(uint32_t));
        free(candidates);
    }
    return matches;
}

void keystone_trigram_index_get_stats(
    const keystone_trigram_index_t* idx,
    keystone_trigram_stats_t* stats
) {
    if (idx && stats) *stats = idx->stats;
}

/* ══════════════════════════════════════════════════════════════════
 * tgrep extension APIs (Changes 1-6)
 * ══════════════════════════════════════════════════════════════════ */

/* --- Change 1: Streaming ingestion --- */

struct keystone_trigram_stream {
    keystone_trigram_index_t* idx;
    char* name;
    char* buffer;
    size_t buf_len;
    size_t buf_cap;
    int failed;
    bool retain_content;
    uint32_t doc_id;
    unsigned char carry[2];
    size_t carry_len;
    size_t total_bytes;
    uint64_t start_time;
};

keystone_trigram_stream_t* keystone_trigram_begin_document_options(
    keystone_trigram_index_t* idx,
    const char* name,
    bool retain_content
) {
    if (!idx || idx->failed || idx->is_finalized) return NULL;
    if (idx->doc_count > (size_t)UINT32_MAX) {
        poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
        return NULL;
    }
    if (ensure_doc_capacity(idx) != KEYSTONE_TRIGRAM_OK) return NULL;

    keystone_trigram_stream_t* stream =
        (keystone_trigram_stream_t*)calloc(1, sizeof(keystone_trigram_stream_t));
    if (!stream) return NULL;

    stream->idx = idx;
    stream->retain_content = retain_content;
    stream->doc_id = (uint32_t)idx->doc_count;
    stream->start_time = get_time_ns();

    if (retain_content) {
        stream->buf_cap = 65536u;
        stream->buffer = (char*)malloc(stream->buf_cap);
        if (!stream->buffer) { free(stream); return NULL; }
    }
    if (name) {
        stream->name = duplicate_c_string(name);
        if (!stream->name) {
            if (stream->buffer) free(stream->buffer);
            free(stream);
            return NULL;
        }
    }
    return stream;
}

keystone_trigram_stream_t* keystone_trigram_begin_document(
    keystone_trigram_index_t* idx,
    const char* name
) {
    return keystone_trigram_begin_document_options(idx, name, false);
}

int keystone_trigram_feed_bytes(
    keystone_trigram_stream_t* stream,
    const char* data,
    size_t len
) {
    if (!stream || !data || stream->failed) return KEYSTONE_TRIGRAM_EINVAL;
    if (len == 0u) return KEYSTONE_TRIGRAM_OK;

    keystone_trigram_index_t* idx = stream->idx;
    if (!idx || idx->failed || idx->is_finalized) {
        stream->failed = 1;
        return KEYSTONE_TRIGRAM_ESTATE;
    }

    size_t new_total;
    if (!checked_add_size(stream->total_bytes, len, &new_total)) {
        stream->failed = 1;
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }
    if (new_total > SIZE_MAX - idx->stats.bytes_indexed) {
        stream->failed = 1;
        return poison_index(idx, KEYSTONE_TRIGRAM_EOVERFLOW);
    }

    if (stream->retain_content) {
        size_t needed;
        if (!checked_add_size(stream->buf_len, len, &needed)) {
            stream->failed = 1;
            return KEYSTONE_TRIGRAM_EOVERFLOW;
        }
        if (needed > stream->buf_cap) {
            size_t new_cap = stream->buf_cap ? stream->buf_cap : 65536u;
            while (new_cap < needed) {
                if (new_cap > SIZE_MAX / 2u) {
                    stream->failed = 1;
                    return KEYSTONE_TRIGRAM_EOVERFLOW;
                }
                new_cap *= 2u;
            }
            char* new_buf = (char*)realloc(stream->buffer, new_cap);
            if (!new_buf) { stream->failed = 1; return KEYSTONE_TRIGRAM_ENOMEM; }
            stream->buffer = new_buf;
            stream->buf_cap = new_cap;
        }
        memcpy(stream->buffer + stream->buf_len, data, len);
        stream->buf_len += len;
        stream->total_bytes = new_total;
        return KEYSTONE_TRIGRAM_OK;
    }

    /* True streaming ingestion with 2-byte boundary carry */
    bool fold = (idx->flags & KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE) != 0;
    uint32_t doc_id = stream->doc_id;
    int rc = KEYSTONE_TRIGRAM_OK;

    /* 1. Bridge carry bytes with prefix of incoming chunk */
    if (stream->carry_len > 0u) {
        unsigned char stitch[5];
        memcpy(stitch, stream->carry, stream->carry_len);
        size_t take = len < 2u ? len : 2u;
        memcpy(stitch + stream->carry_len, data, take);
        size_t stitch_len = stream->carry_len + take;

        if (stitch_len >= 3u) {
            for (size_t k = 0u; k < stream->carry_len && k + 2u < stitch_len; k++) {
                uint32_t b0 = fold ? (uint32_t)fast_ascii_tolower(stitch[k]) : (uint32_t)stitch[k];
                uint32_t b1 = fold ? (uint32_t)fast_ascii_tolower(stitch[k + 1u]) : (uint32_t)stitch[k + 1u];
                uint32_t b2 = fold ? (uint32_t)fast_ascii_tolower(stitch[k + 2u]) : (uint32_t)stitch[k + 2u];
                uint32_t key = (b0 << 16) | (b1 << 8) | b2;
                KEYSTONE_EMIT_TRIGRAM(key);
            }
        }
    }

    /* 2. Process trigrams wholly within incoming chunk */
    if (len >= 3u) {
        const unsigned char* p = (const unsigned char*)data;
        size_t i = 0u;
#ifdef __SSE4_2__
        while (i + 16u <= len) {
            uint32_t keys[16];
            keystone_extract_trigrams16_folded(p + i, keys, fold);
            for (int k = 0; k < 14; k++) {
                KEYSTONE_EMIT_TRIGRAM(keys[k]);
            }
            i += 14u;
        }
#endif
        for (; i <= len - 3u; i++) {
            uint32_t b0 = fold ? (uint32_t)fast_ascii_tolower(p[i]) : (uint32_t)p[i];
            uint32_t b1 = fold ? (uint32_t)fast_ascii_tolower(p[i + 1u]) : (uint32_t)p[i + 1u];
            uint32_t b2 = fold ? (uint32_t)fast_ascii_tolower(p[i + 2u]) : (uint32_t)p[i + 2u];
            uint32_t key = (b0 << 16) | (b1 << 8) | b2;
            KEYSTONE_EMIT_TRIGRAM(key);
        }
    }

    /* 3. Update carry bytes */
    const unsigned char* udata = (const unsigned char*)data;
    if (len >= 2u) {
        stream->carry[0] = udata[len - 2u];
        stream->carry[1] = udata[len - 1u];
        stream->carry_len = 2u;
    } else if (len == 1u) {
        if (stream->carry_len == 0u) {
            stream->carry[0] = udata[0];
            stream->carry_len = 1u;
        } else if (stream->carry_len == 1u) {
            stream->carry[1] = udata[0];
            stream->carry_len = 2u;
        } else {
            stream->carry[0] = stream->carry[1];
            stream->carry[1] = udata[0];
            stream->carry_len = 2u;
        }
    }

    stream->total_bytes = new_total;
    return KEYSTONE_TRIGRAM_OK;

fail_prepared_doc:
    stream->failed = 1;
    clear_doc_seen(idx);
    return poison_index(idx, rc == KEYSTONE_TRIGRAM_OK ? KEYSTONE_TRIGRAM_ESTATE : rc);
}

int keystone_trigram_end_document(
    keystone_trigram_stream_t* stream,
    uint32_t* out_doc_id
) {
    if (!stream) return KEYSTONE_TRIGRAM_EINVAL;
    int rc = KEYSTONE_TRIGRAM_OK;
    if (stream->failed) {
        rc = KEYSTONE_TRIGRAM_ESTATE;
    } else if (stream->retain_content) {
        rc = keystone_trigram_index_add_document(
            stream->idx, stream->name, stream->buffer, stream->buf_len, out_doc_id);
    } else {
        keystone_trigram_index_t* idx = stream->idx;
        if (!idx || idx->failed || idx->is_finalized) {
            rc = KEYSTONE_TRIGRAM_ESTATE;
        } else {
            clear_doc_seen(idx);

            keystone_trigram_doc_t* doc = &idx->docs[idx->doc_count];
            doc->id = stream->doc_id;
            doc->name = stream->name;
            stream->name = NULL;
            doc->content = NULL;
            doc->content_len = stream->total_bytes;
            doc->owns_content = false;
            idx->doc_count++;

            idx->stats.total_documents = idx->doc_count;
            idx->stats.bytes_indexed += stream->total_bytes;
            uint64_t end_time = get_time_ns();
            if (end_time >= stream->start_time &&
                UINT64_MAX - idx->stats.build_time_ns >= end_time - stream->start_time) {
                idx->stats.build_time_ns += end_time - stream->start_time;
            } else {
                idx->stats.build_time_ns = UINT64_MAX;
            }
            if (out_doc_id) *out_doc_id = stream->doc_id;
        }
    }
    if (stream->name) free(stream->name);
    if (stream->buffer) free(stream->buffer);
    free(stream);
    return rc;
}

void keystone_trigram_cancel_document(
    keystone_trigram_stream_t* stream
) {
    if (!stream) return;
    if (!stream->retain_content && stream->total_bytes > 0u && stream->idx) {
        clear_doc_seen(stream->idx);
        poison_index(stream->idx, KEYSTONE_TRIGRAM_ESTATE);
    }
    if (stream->name) free(stream->name);
    if (stream->buffer) free(stream->buffer);
    free(stream);
}

/* --- Change 2: Posting export visitor --- */

int keystone_trigram_visit_postings(
    const keystone_trigram_index_t* idx,
    keystone_trigram_posting_visitor_t visitor,
    void* ctx
) {
    if (!idx || !idx->is_finalized || !visitor) return KEYSTONE_TRIGRAM_EINVAL;
    for (size_t i = 0u; i < idx->num_buckets; i++) {
        if (idx->bucket_keys[i] == TRIGRAM_KEY_EMPTY) continue;
        const keystone_trigram_posting_list_t* plist = &idx->bucket_lists[i];
        if (plist->count == 0u) continue;
        if (visitor(idx->bucket_keys[i], plist->doc_ids, plist->count, ctx))
            break;
    }
    return KEYSTONE_TRIGRAM_OK;
}

/* --- Change 3: Frequency lookup --- */

size_t keystone_trigram_index_frequency(
    const keystone_trigram_index_t* idx,
    uint32_t gram
) {
    if (!idx || !idx->is_finalized) return 0u;
    const keystone_trigram_posting_list_t* plist = get_posting_list(idx, gram);
    return plist ? plist->count : 0u;
}

/* --- Change 4: Paginated candidate iterator --- */

struct keystone_trigram_candidate_iter {
    const keystone_trigram_index_t* idx;
    const keystone_trigram_posting_list_t* lists[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t cursor[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t num_lists;
    size_t base_pos;
    bool empty_result;
};

keystone_trigram_candidate_iter_t* keystone_trigram_candidates_begin(
    const keystone_trigram_index_t* idx,
    const char* pattern,
    size_t pattern_len
) {
    if (!idx || !idx->is_finalized || !pattern) return NULL;

    keystone_trigram_candidate_iter_t* iter =
        (keystone_trigram_candidate_iter_t*)calloc(
            1, sizeof(keystone_trigram_candidate_iter_t));
    if (!iter) return NULL;
    iter->idx = idx;

    if (pattern_len < 3u) {
        iter->num_lists = 0u;
        return iter;
    }

    bool fold = (idx->flags & KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE) != 0;
    uint32_t query_trigrams[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t num_trigrams = keystone_trigram_extract_internal(
        pattern, pattern_len, query_trigrams, TRIGRAM_QUERY_MAX_UNIQUE, fold);
    if (num_trigrams == 0u) {
        iter->empty_result = true;
        return iter;
    }

    for (size_t i = 0u; i < num_trigrams; i++) {
        const keystone_trigram_posting_list_t* plist =
            get_posting_list(idx, query_trigrams[i]);
        if (!plist || plist->count == 0u) {
            iter->empty_result = true;
            return iter;
        }
        iter->lists[iter->num_lists++] = plist;
    }

    /* Sort by size (smallest first) */
    for (size_t i = 0u; i < iter->num_lists; i++) {
        for (size_t j = i + 1u; j < iter->num_lists; j++) {
            if (iter->lists[j]->count < iter->lists[i]->count) {
                const keystone_trigram_posting_list_t* tmp = iter->lists[i];
                iter->lists[i] = iter->lists[j];
                iter->lists[j] = tmp;
            }
        }
    }

    /* Prune to 16 rarest lists */
    if (iter->num_lists > 16u) {
        iter->num_lists = 16u;
    }

    for (size_t l = 0u; l < iter->num_lists; l++) {
        iter->cursor[l] = 0u;
    }

    return iter;
}

int keystone_trigram_candidates_next(
    keystone_trigram_candidate_iter_t* iter,
    uint32_t* out_buf,
    size_t capacity,
    size_t* out_count,
    int* out_exhausted
) {
    if (!iter || !out_buf || capacity == 0u) return KEYSTONE_TRIGRAM_EINVAL;
    if (out_count) *out_count = 0u;
    if (out_exhausted) *out_exhausted = 0u;

    if (iter->empty_result) {
        if (out_exhausted) *out_exhausted = 1;
        return KEYSTONE_TRIGRAM_OK;
    }

    /* No lists = pattern < 3 bytes = all docs are candidates */
    if (iter->num_lists == 0u) {
        size_t pos = iter->base_pos;
        size_t count = 0u;
        while (pos < iter->idx->doc_count && count < capacity) {
            out_buf[count++] = (uint32_t)pos;
            pos++;
        }
        if (out_count) *out_count = count;
        iter->base_pos = pos;
        if (out_exhausted) *out_exhausted = (pos >= iter->idx->doc_count) ? 1 : 0;
        return KEYSTONE_TRIGRAM_OK;
    }

    const keystone_trigram_posting_list_t* base = iter->lists[0];
    size_t count = 0u;

    while (iter->base_pos < base->count && count < capacity) {
        uint32_t doc_id = base->doc_ids[iter->base_pos++];
        bool in_all = true;

        for (size_t l = 1u; l < iter->num_lists; l++) {
            const keystone_trigram_posting_list_t* pl = iter->lists[l];
            if (pl->bitmap) {
                if ((pl->bitmap[doc_id >> 6u] & (UINT64_C(1) << (doc_id & 63u))) == 0) {
                    in_all = false;
                    break;
                }
            } else {
                size_t pos = ks_lower_bound_gallop_u32(
                    pl->doc_ids, pl->count, iter->cursor[l], doc_id);

                iter->cursor[l] = pos;
                if (pos == pl->count || pl->doc_ids[pos] != doc_id) {
                    in_all = false;
                    break;
                }
                iter->cursor[l] = pos + 1u;
            }
        }

        if (in_all) {
            out_buf[count++] = doc_id;
        }
    }

    if (out_count) *out_count = count;
    if (out_exhausted) *out_exhausted = (iter->base_pos >= base->count) ? 1 : 0;
    return KEYSTONE_TRIGRAM_OK;
}

void keystone_trigram_candidates_free(
    keystone_trigram_candidate_iter_t* iter
) {
    free(iter);
}

/* --- Change 6: Memory usage reporting --- */

size_t keystone_trigram_index_memory_usage(
    const keystone_trigram_index_t* idx
) {
    if (!idx) return 0u;
    size_t total = 0u;
    /* Hash table (keys + lists) */
    total += idx->num_buckets * sizeof(uint32_t);
    total += idx->num_buckets * sizeof(keystone_trigram_posting_list_t);
    /* Posting list arrays / flattened postings */
    if (idx->flat_postings) {
        total += idx->total_postings * sizeof(uint32_t);
    } else {
        for (size_t i = 0u; i < idx->num_buckets; i++) {
            if (idx->bucket_keys[i] != TRIGRAM_KEY_EMPTY) {
                total += idx->bucket_lists[i].capacity * sizeof(uint32_t);
            }
        }
    }

    /* Dense posting bitmaps */
    size_t bitmap_bytes = ((idx->doc_count + 63u) / 64u) * sizeof(uint64_t);
    for (size_t i = 0u; i < idx->num_buckets; i++) {
        if (idx->bucket_keys[i] != TRIGRAM_KEY_EMPTY && idx->bucket_lists[i].bitmap) {
            total += bitmap_bytes;
        }
    }
    /* Doc table */
    total += idx->doc_capacity * sizeof(keystone_trigram_doc_t);
    /* Doc names + content */
    for (size_t i = 0u; i < idx->doc_count; i++) {
        if (idx->docs[i].name) total += strlen(idx->docs[i].name) + 1u;
        if (idx->docs[i].owns_content && idx->docs[i].content)
            total += idx->docs[i].content_len + 1u;
    }
    return total;
}

/* --- Change 7: Binary Persistence --- */

#define TRIGRAM_FILE_MAGIC "KEYSTRIG"
#define TRIGRAM_FILE_VERSION 1u

int keystone_trigram_index_save(
    const keystone_trigram_index_t* idx,
    const char* filepath
) {
    if (!idx || !idx->is_finalized || idx->failed || !filepath) {
        return KEYSTONE_TRIGRAM_EINVAL;
    }

    FILE* fp = fopen(filepath, "wb");
    if (!fp) return KEYSTONE_TRIGRAM_EINVAL;

    /* Write magic & version */
    if (fwrite(TRIGRAM_FILE_MAGIC, 1, 8, fp) != 8) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
    uint32_t version = TRIGRAM_FILE_VERSION;
    if (fwrite(&version, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

    /* Flags & stats */
    uint32_t flags = idx->flags;
    if (fwrite(&flags, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
    if (fwrite(&idx->stats, sizeof(keystone_trigram_stats_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

    /* Document records */
    uint64_t doc_count = (uint64_t)idx->doc_count;
    if (fwrite(&doc_count, sizeof(uint64_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

    for (size_t i = 0u; i < idx->doc_count; i++) {
        const keystone_trigram_doc_t* doc = &idx->docs[i];
        uint32_t doc_id = doc->id;
        if (fwrite(&doc_id, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

        size_t name_len_raw = doc->name ? strlen(doc->name) : 0u;
        if (name_len_raw > UINT32_MAX) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
        uint32_t name_len = (uint32_t)name_len_raw;
        if (fwrite(&name_len, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
        if (name_len > 0u) {
            if (fwrite(doc->name, 1, name_len, fp) != name_len) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
        }

        uint8_t owns_content = doc->owns_content ? 1u : 0u;
        if (fwrite(&owns_content, sizeof(uint8_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

        uint64_t content_len = (uint64_t)doc->content_len;
        if (fwrite(&content_len, sizeof(uint64_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

        if (owns_content && content_len > 0u && doc->content) {
            if (fwrite(doc->content, 1, doc->content_len, fp) != doc->content_len) {
                fclose(fp);
                return KEYSTONE_TRIGRAM_EINVAL;
            }
        }
    }

    /* Posting lists */
    uint64_t unique_trigrams = (uint64_t)idx->unique_trigrams;
    if (fwrite(&unique_trigrams, sizeof(uint64_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }

    for (size_t i = 0u; i < idx->num_buckets; i++) {
        if (idx->bucket_keys[i] == TRIGRAM_KEY_EMPTY) continue;
        uint32_t key = idx->bucket_keys[i];
        if (idx->bucket_lists[i].count > UINT32_MAX) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
        uint32_t count = (uint32_t)idx->bucket_lists[i].count;
        if (fwrite(&key, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
        if (fwrite(&count, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return KEYSTONE_TRIGRAM_EINVAL; }
        if (count > 0u) {
            if (fwrite(idx->bucket_lists[i].doc_ids, sizeof(uint32_t), count, fp) != count) {
                fclose(fp);
                return KEYSTONE_TRIGRAM_EINVAL;
            }
        }
    }

    fclose(fp);
    return KEYSTONE_TRIGRAM_OK;
}

keystone_trigram_index_t* keystone_trigram_index_load(const char* filepath) {
    if (!filepath) return NULL;

    FILE* fp = fopen(filepath, "rb");
    if (!fp) return NULL;

    char magic[8];
    if (fread(magic, 1, 8, fp) != 8 || memcmp(magic, TRIGRAM_FILE_MAGIC, 8) != 0) {
        fclose(fp);
        return NULL;
    }

    uint32_t version = 0u;
    if (fread(&version, sizeof(uint32_t), 1, fp) != 1 || version != TRIGRAM_FILE_VERSION) {
        fclose(fp);
        return NULL;
    }

    uint32_t flags = 0u;
    if (fread(&flags, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return NULL; }

    keystone_trigram_stats_t stats;
    if (fread(&stats, sizeof(keystone_trigram_stats_t), 1, fp) != 1) { fclose(fp); return NULL; }

    uint64_t doc_count = 0u;
    if (fread(&doc_count, sizeof(uint64_t), 1, fp) != 1) { fclose(fp); return NULL; }
    if (doc_count > (uint64_t)max_document_count()) { fclose(fp); return NULL; }

    keystone_trigram_index_t* idx = keystone_trigram_index_create_options((size_t)doc_count, flags);
    if (!idx) { fclose(fp); return NULL; }

    for (size_t i = 0u; i < (size_t)doc_count; i++) {
        uint32_t doc_id = 0u;
        if (fread(&doc_id, sizeof(uint32_t), 1, fp) != 1) { goto load_fail; }

        uint32_t name_len = 0u;
        if (fread(&name_len, sizeof(uint32_t), 1, fp) != 1) { goto load_fail; }

        char* name_copy = NULL;
        if (name_len > 0u) {
            size_t name_alloc;
            if (!checked_add_size((size_t)name_len, 1u, &name_alloc)) goto load_fail;
            name_copy = (char*)malloc(name_alloc);
            if (!name_copy) goto load_fail;
            if (fread(name_copy, 1, name_len, fp) != name_len) { free(name_copy); goto load_fail; }
            name_copy[name_len] = '\0';
        }

        uint8_t owns_content = 0u;
        if (fread(&owns_content, sizeof(uint8_t), 1, fp) != 1) {
            if (name_copy) free(name_copy);
            goto load_fail;
        }

        uint64_t content_len = 0u;
        if (fread(&content_len, sizeof(uint64_t), 1, fp) != 1) {
            if (name_copy) free(name_copy);
            goto load_fail;
        }

        char* content_copy = NULL;
        if (owns_content && content_len > 0u) {
            if (content_len > SIZE_MAX - 1) {
                if (name_copy) free(name_copy);
                goto load_fail;
            }
            size_t content_alloc = (size_t)content_len + 1u;
            content_copy = (char*)malloc(content_alloc);
            if (!content_copy) {
                if (name_copy) free(name_copy);
                goto load_fail;
            }
            if (fread(content_copy, 1, (size_t)content_len, fp) != (size_t)content_len) {
                free(content_copy);
                if (name_copy) free(name_copy);
                goto load_fail;
            }
            content_copy[content_len] = '\0';
        }

        keystone_trigram_doc_t* doc = &idx->docs[i];
        doc->id = doc_id;
        doc->name = name_copy;
        doc->owns_content = (owns_content != 0u);
        doc->content_len = (size_t)content_len;
        doc->content = content_copy;
        idx->doc_count++;
    }

    uint64_t unique_trigrams = 0u;
    if (fread(&unique_trigrams, sizeof(uint64_t), 1, fp) != 1) { goto load_fail; }

    for (size_t p = 0u; p < (size_t)unique_trigrams; p++) {
        uint32_t key = 0u;
        uint32_t count = 0u;
        if (fread(&key, sizeof(uint32_t), 1, fp) != 1) goto load_fail;
        if (fread(&count, sizeof(uint32_t), 1, fp) != 1) goto load_fail;

        keystone_trigram_posting_list_t* plist = find_or_create_posting_list(idx, key);
        if (!plist) goto load_fail;

        if (count > 0u) {
            size_t cap = count > TRIGRAM_INITIAL_POSTING_CAPACITY ? (size_t)count : TRIGRAM_INITIAL_POSTING_CAPACITY;
            size_t ids_bytes;
            if (!checked_mul_size(cap, sizeof(uint32_t), &ids_bytes)) goto load_fail;
            uint32_t* new_ids = (uint32_t*)realloc(plist->doc_ids, ids_bytes);
            if (!new_ids) goto load_fail;
            plist->doc_ids = new_ids;
            plist->capacity = cap;
            if (fread(plist->doc_ids, sizeof(uint32_t), count, fp) != count) goto load_fail;
            plist->count = count;
        }
    }

    int fin_rc = keystone_trigram_index_finalize(idx);
    if (fin_rc != KEYSTONE_TRIGRAM_OK) goto load_fail;
    idx->stats = stats;
    fclose(fp);
    return idx;

load_fail:
    fclose(fp);
    keystone_trigram_index_destroy(idx);
    return NULL;
}
