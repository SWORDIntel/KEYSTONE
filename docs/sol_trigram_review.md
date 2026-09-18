## Executive assessment

The core design is reasonable for a medium-sized immutable index, but the dominant costs are not currently the SIMD instructions:

1. **One heap allocation per unique trigram posting list** is expensive and fragments memory.
2. **Each hash slot costs roughly 28–32 bytes** on 64-bit systems: 4-byte key plus a padded 24-byte posting descriptor, before posting storage.
3. **Search allocates `doc_count * 4` bytes on every query**, even when the rarest posting contains only a few candidates.
4. **The “streaming” API buffers the entire document**, then indexes it again. Retained streaming documents can temporarily require two full copies.
5. **The candidate iterator is substantially slower than the normal candidate path** because it uses `bsearch()` independently for every candidate/list pair.
6. **Case-insensitive candidate iteration is incorrect**: it calls public `keystone_trigram_extract()`, which does not fold the query, while ingestion may have folded it.
7. The current SIMD extraction is useful, but random bitmap writes, hash probes, and per-list allocation dominate once extraction becomes fast.
8. AVX2/AVX-512 help most for **balanced posting intersections** and dense bitmap operations. Highly skewed intersections should use monotonic galloping/lower-bound searches instead.

The highest-value sequence is:

1. Remove per-query `doc_count` allocation and repair the iterator.
2. Replace per-posting heap allocations with arena/chunked posting construction.
3. Add adaptive sparse intersection: balanced SIMD merge versus skewed galloping.
4. Convert high-frequency postings to bitmaps at finalize time.
5. Replace the hash directory with a direct 24-bit descriptor directory when the index is large.
6. Implement genuine streaming extraction.
7. Add thread-local builders and merge after ingestion.

---

# 1. AVX2/AVX-512 posting-list intersection

## 1.1 Use an adaptive algorithm

There is no single best intersection algorithm:

| Input shape | Recommended algorithm |
|---|---|
| Similar lengths | Blocked AVX2/AVX-512 merge |
| 8–16× size skew | Galloping smaller list into larger list |
| Very dense posting | Bitmap membership/filtering |
| Dense versus dense | Word-wise AVX2/AVX-512 AND |
| More than two sparse lists | Start with rarest list, progressively intersect |

A naïve AVX2 cross-product compares eight values from one block against eight values from another block using eight broadcasts. That is 64 logical comparisons in eight vector instructions. It is useful for balanced lists but inferior to galloping when one list is much larger.

`VPTESTMB` is not the right primitive for exact 32-bit posting equality. Use:

- AVX2: `_mm256_cmpeq_epi32`
- AVX-512F: `_mm512_cmpeq_epu32_mask`
- AVX-512VP2INTERSECT, when actually available: `VP2INTERSECTD`

AVX-512VP2INTERSECT is not widely available even on AVX-512-capable processors, so it must not be the only AVX-512 implementation.

---

## 1.2 Directly compilable adaptive intersection implementation

The following functions operate on sorted, duplicate-free `uint32_t` arrays. They can be placed directly in `keystone_trigram.c`.

```c
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

typedef struct ks_u32_list_view {
    const uint32_t *ids;
    size_t count;
} ks_u32_list_view_t;

static size_t
ks_intersect_u32_scalar(const uint32_t *a, size_t na,
                        const uint32_t *b, size_t nb,
                        uint32_t *out, size_t out_cap)
{
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

/*
 * Monotonic lower_bound with exponential bracketing.
 *
 * The caller passes the previous position, so successive probes never restart
 * from the beginning of the larger posting.
 */
static size_t
ks_lower_bound_gallop_u32(const uint32_t *a, size_t n,
                          size_t pos, uint32_t target)
{
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

/*
 * Best for highly skewed lists. Complexity is approximately:
 *
 *     O(min(na, nb) * log(max(na, nb) / min(na, nb)))
 *
 * Search positions in the larger list are monotonic.
 */
static size_t
ks_intersect_u32_galloping(const uint32_t *a, size_t na,
                           const uint32_t *b, size_t nb,
                           uint32_t *out, size_t out_cap)
{
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
        if (lp == nl) {
            break;
        }

        if (large[lp] == target) {
            out[no++] = target;
            lp++;
        }
    }

    return no;
}

#if KS_X86

/*
 * Compare every source lane against the eight values in other.
 * Returns one bit per source lane.
 */
KS_TARGET_AVX2
static inline unsigned
ks_match8_against_block_avx2(const uint32_t *source, __m256i other)
{
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

/*
 * Balanced-list AVX2 intersection.
 *
 * A block is emitted only when that block is known to be complete:
 * its maximum is <= the maximum of the opposing block. This prevents
 * duplicate output when one block must be retained for the next comparison.
 */
KS_TARGET_AVX2
static size_t
ks_intersect_u32_avx2(const uint32_t *a, size_t na,
                      const uint32_t *b, size_t nb,
                      uint32_t *out, size_t out_cap)
{
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
            __m256i vb = _mm256_loadu_si256(
                (const __m256i *)(const void *)(b + ib));
            unsigned hits = ks_match8_against_block_avx2(a + ia, vb);

            while (hits != 0u && no < out_cap) {
                unsigned lane = (unsigned)__builtin_ctz(hits);
                hits &= hits - 1u;
                out[no++] = a[ia + lane];
            }
            ia += 8u;
        } else {
            __m256i va = _mm256_loadu_si256(
                (const __m256i *)(const void *)(a + ia));
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

KS_TARGET_AVX512F
static inline uint32_t
ks_match16_against_block_avx512(const uint32_t *source, __m512i other)
{
    uint32_t hits = 0u;

    for (unsigned lane = 0u; lane < 16u; lane++) {
        __m512i x = _mm512_set1_epi32((int)source[lane]);
        __mmask16 eq = _mm512_cmpeq_epu32_mask(x, other);

        if (eq != 0u) {
            hits |= UINT32_C(1) << lane;
        }
    }

    return hits;
}

KS_TARGET_AVX512F
static size_t
ks_intersect_u32_avx512(const uint32_t *a, size_t na,
                        const uint32_t *b, size_t nb,
                        uint32_t *out, size_t out_cap)
{
    size_t ia = 0u;
    size_t ib = 0u;
    size_t no = 0u;

    while (ia + 16u <= na && ib + 16u <= nb && no < out_cap) {
        if (a[ia + 15u] < b[ib]) {
            ia += 16u;
            continue;
        }
        if (b[ib + 15u] < a[ia]) {
            ib += 16u;
            continue;
        }

        uint32_t max_a = a[ia + 15u];
        uint32_t max_b = b[ib + 15u];

        if (max_a <= max_b) {
            __m512i vb = _mm512_loadu_si512(
                (const void *)(b + ib));
            uint32_t hits = ks_match16_against_block_avx512(a + ia, vb);

            while (hits != 0u && no < out_cap) {
                unsigned lane = (unsigned)__builtin_ctz(hits);
                hits &= hits - 1u;
                out[no++] = a[ia + lane];
            }
            ia += 16u;
        } else {
            __m512i va = _mm512_loadu_si512(
                (const void *)(a + ia));
            uint32_t hits = ks_match16_against_block_avx512(b + ib, va);

            while (hits != 0u && no < out_cap) {
                unsigned lane = (unsigned)__builtin_ctz(hits);
                hits &= hits - 1u;
                out[no++] = b[ib + lane];
            }
            ib += 16u;
        }
    }

    if (no < out_cap) {
        no += ks_intersect_u32_avx2(
            a + ia, na - ia, b + ib, nb - ib,
            out + no, out_cap - no);
    }

    return no;
}
#endif

static size_t
ks_intersect_u32_adaptive(const uint32_t *a, size_t na,
                          const uint32_t *b, size_t nb,
                          uint32_t *out, size_t out_cap)
{
    if (!a || !b || !out || out_cap == 0u || na == 0u || nb == 0u) {
        return 0u;
    }

    size_t smaller = na < nb ? na : nb;
    size_t larger = na < nb ? nb : na;

    /*
     * Avoid overflow in "larger >= smaller * 16".
     */
    if (smaller == 0u || larger / smaller >= 16u) {
        return ks_intersect_u32_galloping(
            a, na, b, nb, out, out_cap);
    }

#if KS_X86 && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();

    if (__builtin_cpu_supports("avx512f")) {
        return ks_intersect_u32_avx512(
            a, na, b, nb, out, out_cap);
    }
    if (__builtin_cpu_supports("avx2")) {
        return ks_intersect_u32_avx2(
            a, na, b, nb, out, out_cap);
    }
#endif

    return ks_intersect_u32_scalar(
        a, na, b, nb, out, out_cap);
}
```

### AVX-512VP2INTERSECT enhancement

Where supported, a 16×16 block can be reduced to one instruction:

```c
#if defined(__AVX512VP2INTERSECT__)
#include <immintrin.h>

static inline __mmask16
ks_intersect16_vp2intersect(__m512i a, __m512i b)
{
    __mmask16 mask_a;
    __mmask16 mask_b;
    _mm512_2intersect_epi32(a, b, &mask_a, &mask_b);
    return mask_a;
}
#endif
```

This should be compiled in a separate function with target attribute
`"avx512f,avx512vp2intersect"` and runtime-dispatched. Do not assume that AVX-512F implies VP2INTERSECT.

---

## 1.3 Multi-way sparse intersection

Do not repeatedly scan the original smallest posting using `bsearch()`. Intersect progressively, so each additional list operates only on surviving candidates.

A reusable query workspace avoids allocation in the hot path:

```c
typedef struct ks_intersection_workspace {
    uint32_t *buf_a;
    uint32_t *buf_b;
    size_t capacity;
} ks_intersection_workspace_t;

static int
ks_compare_list_size(const void *pa, const void *pb)
{
    const ks_u32_list_view_t *a = (const ks_u32_list_view_t *)pa;
    const ks_u32_list_view_t *b = (const ks_u32_list_view_t *)pb;
    return (a->count > b->count) - (a->count < b->count);
}

/*
 * Returns the number of output IDs. The lists must be sorted and duplicate-free.
 * `views` is reordered by size.
 */
static size_t
ks_intersect_many_u32(ks_u32_list_view_t *views,
                      size_t nviews,
                      ks_intersection_workspace_t *ws,
                      uint32_t *out,
                      size_t out_cap)
{
    if (!views || nviews == 0u || !ws || !ws->buf_a || !ws->buf_b ||
        !out || out_cap == 0u) {
        return 0u;
    }

    qsort(views, nviews, sizeof(views[0]), ks_compare_list_size);

    if (views[0].count == 0u) {
        return 0u;
    }

    size_t n = views[0].count;
    if (n > ws->capacity) n = ws->capacity;
    if (n > out_cap) n = out_cap;

    memcpy(ws->buf_a, views[0].ids, n * sizeof(uint32_t));

    uint32_t *current = ws->buf_a;
    uint32_t *next = ws->buf_b;

    for (size_t i = 1u; i < nviews && n != 0u; i++) {
        n = ks_intersect_u32_adaptive(
            current, n,
            views[i].ids, views[i].count,
            next, ws->capacity < out_cap ? ws->capacity : out_cap);

        uint32_t *tmp = current;
        current = next;
        next = tmp;
    }

    memcpy(out, current, n * sizeof(uint32_t));
    return n;
}
```

For KEYSTONE, the workspace only needs capacity equal to:

```c
min(rarest_posting_count, max_candidates)
```

It never needs `doc_count` entries unless the query has fewer than three bytes.

---

## 1.4 Replace the current iterator implementation

The iterator currently does this:

```c
bsearch(&doc_id, list, count, ...)
```

for every candidate and every list. That loses all monotonicity.

Add one cursor per posting:

```c
struct keystone_trigram_candidate_iter {
    const keystone_trigram_index_t* idx;
    const keystone_trigram_posting_list_t* lists[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t cursor[TRIGRAM_QUERY_MAX_UNIQUE];
    size_t num_lists;
    size_t base_pos;
    bool empty_result;
};
```

Then probe with `ks_lower_bound_gallop_u32()`:

```c
for (size_t l = 1u; l < iter->num_lists; l++) {
    const keystone_trigram_posting_list_t *pl = iter->lists[l];

    size_t pos = ks_lower_bound_gallop_u32(
        pl->doc_ids, pl->count, iter->cursor[l], doc_id);

    iter->cursor[l] = pos;

    if (pos == pl->count || pl->doc_ids[pos] != doc_id) {
        in_all = false;
        break;
    }

    iter->cursor[l] = pos + 1u;
}
```

Also fix case-insensitive extraction:

```c
bool fold =
    (idx->flags & KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE) != 0u;

size_t num_trigrams = keystone_trigram_extract_internal(
    pattern, pattern_len,
    query_trigrams, TRIGRAM_QUERY_MAX_UNIQUE,
    fold);
```

An absent posting is an **empty iterator**, not necessarily allocation failure. Add `empty_result = true` instead of returning `NULL`.

---

# 2. AVX2 rolling trigram extraction

## 2.1 Important AVX2 lane limitation

`_mm256_shuffle_epi8()` operates independently on the two 128-bit lanes. A single ordinary 32-byte load cannot directly create all 30 overlapping trigrams because windows at positions 14 and 15 cross the lane boundary.

The clean approach is:

- Low 128-bit lane loads bytes `p[0..15]`.
- High 128-bit lane loads bytes `p[14..29]`.
- Each lane emits 14 trigrams.
- Together they emit positions `0..27`.
- Advance by 28 bytes.

This avoids special handling for lane-crossing trigrams.

---

## 2.2 Compilable AVX2 extraction function

```c
#if KS_X86

KS_TARGET_AVX2
static inline __m256i
ks_ascii_tolower_32(__m256i v)
{
    const __m256i a_minus_1 = _mm256_set1_epi8('A' - 1);
    const __m256i z_plus_1  = _mm256_set1_epi8('Z' + 1);
    const __m256i lower_bit = _mm256_set1_epi8(0x20);

    /*
     * High-bit bytes are negative in signed comparison and therefore do not
     * enter the ASCII A..Z range. This intentionally implements ASCII folding,
     * independent of the process locale.
     */
    __m256i ge_a = _mm256_cmpgt_epi8(v, a_minus_1);
    __m256i le_z = _mm256_cmpgt_epi8(z_plus_1, v);
    __m256i upper = _mm256_and_si256(ge_a, le_z);

    return _mm256_or_si256(v, _mm256_and_si256(upper, lower_bit));
}

/*
 * Extract 28 trigrams from bytes p[0..29].
 *
 * Output ordering is:
 *   0..3, 14..17,
 *   4..7, 18..21,
 *   8..11, 22..25,
 *   12..13, 26..27.
 *
 * The order is irrelevant for per-document deduplication and posting append.
 */
KS_TARGET_AVX2
static inline void
ks_extract_28_trigrams_avx2(const unsigned char *p,
                            uint32_t out[28],
                            int fold_ascii)
{
    const __m128i lo = _mm_loadu_si128(
        (const __m128i *)(const void *)(p));
    const __m128i hi = _mm_loadu_si128(
        (const __m128i *)(const void *)(p + 14u));

    __m256i bytes = _mm256_castsi128_si256(lo);
    bytes = _mm256_inserti128_si256(bytes, hi, 1);

    if (fold_ascii) {
        bytes = ks_ascii_tolower_32(bytes);
    }

    const __m256i m0 = _mm256_setr_epi8(
        2,1,0,(char)0x80, 3,2,1,(char)0x80,
        4,3,2,(char)0x80, 5,4,3,(char)0x80,
        2,1,0,(char)0x80, 3,2,1,(char)0x80,
        4,3,2,(char)0x80, 5,4,3,(char)0x80);

    const __m256i m1 = _mm256_setr_epi8(
        6,5,4,(char)0x80, 7,6,5,(char)0x80,
        8,7,6,(char)0x80, 9,8,7,(char)0x80,
        6,5,4,(char)0x80, 7,6,5,(char)0x80,
        8,7,6,(char)0x80, 9,8,7,(char)0x80);

    const __m256i m2 = _mm256_setr_epi8(
        10,9,8,(char)0x80, 11,10,9,(char)0x80,
        12,11,10,(char)0x80, 13,12,11,(char)0x80,
        10,9,8,(char)0x80, 11,10,9,(char)0x80,
        12,11,10,(char)0x80, 13,12,11,(char)0x80);

    const __m256i m3 = _mm256_setr_epi8(
        14,13,12,(char)0x80, 15,14,13,(char)0x80,
        (char)0x80,(char)0x80,(char)0x80,(char)0x80,
        (char)0x80,(char)0x80,(char)0x80,(char)0x80,
        14,13,12,(char)0x80, 15,14,13,(char)0x80,
        (char)0x80,(char)0x80,(char)0x80,(char)0x80,
        (char)0x80,(char)0x80,(char)0x80,(char)0x80);

    uint32_t tmp[8];

    _mm256_storeu_si256(
        (__m256i *)(void *)tmp,
        _mm256_shuffle_epi8(bytes, m0));
    memcpy(out + 0u, tmp, 8u * sizeof(uint32_t));

    _mm256_storeu_si256(
        (__m256i *)(void *)tmp,
        _mm256_shuffle_epi8(bytes, m1));
    memcpy(out + 8u, tmp, 8u * sizeof(uint32_t));

    _mm256_storeu_si256(
        (__m256i *)(void *)tmp,
        _mm256_shuffle_epi8(bytes, m2));
    memcpy(out + 16u, tmp, 8u * sizeof(uint32_t));

    _mm256_storeu_si256(
        (__m256i *)(void *)tmp,
        _mm256_shuffle_epi8(bytes, m3));

    out[24] = tmp[0];
    out[25] = tmp[1];
    out[26] = tmp[4];
    out[27] = tmp[5];
}
#endif
```

Integration into ingestion:

```c
size_t i = 0u;

#if KS_X86 && (defined(__GNUC__) || defined(__clang__))
__builtin_cpu_init();

if (__builtin_cpu_supports("avx2")) {
    while (i + 30u <= text_len) {
        uint32_t keys[28];
        ks_extract_28_trigrams_avx2(p + i, keys, fold ? 1 : 0);

        for (size_t k = 0u; k < 28u; k++) {
            uint32_t key = keys[k];
            KEYSTONE_EMIT_TRIGRAM(key);
        }

        i += 28u;
    }
}
#endif

for (; i + 2u < text_len; i++) {
    uint32_t b0 = p[i];
    uint32_t b1 = p[i + 1u];
    uint32_t b2 = p[i + 2u];

    if (fold) {
        b0 = (b0 >= 'A' && b0 <= 'Z') ? b0 + 0x20u : b0;
        b1 = (b1 >= 'A' && b1 <= 'Z') ? b1 + 0x20u : b1;
        b2 = (b2 >= 'A' && b2 <= 'Z') ? b2 + 0x20u : b2;
    }

    uint32_t key = (b0 << 16) | (b1 << 8) | b2;
    KEYSTONE_EMIT_TRIGRAM(key);
}
```

### Important case-folding correction

Do not mix locale-dependent scalar `tolower()` with ASCII-only SIMD folding. That can produce different indexed keys depending on whether a byte happened to be processed by the vector or scalar loop.

Use one explicit ASCII helper everywhere:

```c
static inline unsigned char
ks_ascii_lower(unsigned char c)
{
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        ? (unsigned char)(c + 0x20u)
        : c;
}
```

Use the same rule in:

- ingestion,
- query extraction,
- exact case-insensitive verification,
- streaming boundary handling.

---

## 2.3 SIMD extraction will not remove the main ingestion bottleneck

After extraction is vectorized, each unique trigram still causes:

1. a random 2 MB bitmap access,
2. a hash-table probe,
3. possibly an allocation,
4. a posting-list append.

The next major improvement is therefore not wider extraction. It is:

- eliminate the per-list initial allocation,
- aggregate postings in append-only chunks,
- flatten at finalize,
- optionally use a direct key directory.

---

# 3. Hybrid compressed postings

## 3.1 Representation

Use sparse `uint32_t[]` for rare trigrams and a document bitmap for common trigrams.

```c
typedef enum ks_posting_kind {
    KS_POSTING_SPARSE32 = 0,
    KS_POSTING_BITMAP64 = 1
} ks_posting_kind_t;

typedef struct ks_sparse_posting {
    uint32_t *ids;
    uint32_t count;
    uint32_t capacity;
} ks_sparse_posting_t;

typedef struct ks_bitmap_posting {
    uint64_t *words;
    uint32_t nwords;
    uint32_t count;
} ks_bitmap_posting_t;

typedef struct ks_hybrid_posting {
    uint32_t count;
    uint8_t kind;
    uint8_t reserved[3];

    union {
        ks_sparse_posting_t sparse;
        ks_bitmap_posting_t bitmap;
    } u;
} ks_hybrid_posting_t;
```

Because `count` exists both outside and inside the variants in this illustrative layout, the inner counts may be removed in the final implementation. A compact production descriptor can be:

```c
typedef struct ks_posting_desc {
    uint64_t data_offset;
    uint32_t count;
    uint32_t aux;       /* sparse capacity or bitmap word count */
    uint8_t kind;
    uint8_t reserved[7];
} ks_posting_desc_t;
```

This is suitable for arena-backed and memory-mapped persistence.

---

## 3.2 Threshold selection

Sparse storage costs approximately:

```text
4 * document_frequency bytes
```

A bitmap costs:

```text
ceil(document_count / 64) * 8 bytes
```

Pure memory break-even is approximately:

```text
frequency >= document_count / 32
```

Therefore, converting at `doc_count / 64` uses up to about twice the sparse payload memory, but can still improve query speed substantially.

Recommended defaults:

- **Memory-oriented:** `frequency >= doc_count / 32`
- **Throughput-oriented:** `frequency >= doc_count / 64`
- Also require a minimum frequency, such as 4096, to avoid tiny bitmaps.

---

## 3.3 Sparse-to-bitmap conversion

```c
static int
ks_posting_convert_to_bitmap(ks_hybrid_posting_t *p,
                             size_t document_count)
{
    if (!p || p->kind != KS_POSTING_SPARSE32) {
        return -1;
    }

    size_t nwords = (document_count + 63u) >> 6;
    if (nwords > UINT32_MAX || nwords > SIZE_MAX / sizeof(uint64_t)) {
        return -1;
    }

    uint64_t *words = (uint64_t *)calloc(nwords, sizeof(uint64_t));
    if (!words) {
        return -1;
    }

    for (uint32_t i = 0u; i < p->u.sparse.count; i++) {
        uint32_t id = p->u.sparse.ids[i];
        words[id >> 6] |= UINT64_C(1) << (id & 63u);
    }

    uint32_t count = p->u.sparse.count;
    free(p->u.sparse.ids);

    p->kind = KS_POSTING_BITMAP64;
    p->count = count;
    p->u.bitmap.words = words;
    p->u.bitmap.nwords = (uint32_t)nwords;
    p->u.bitmap.count = count;

    return 0;
}
```

Perform conversion during `finalize()`, after `doc_count` is known.

---

## 3.4 Hybrid intersection functions

```c
static size_t
ks_intersect_sparse_bitmap(const uint32_t *ids, size_t count,
                           const uint64_t *bitmap, size_t nwords,
                           uint32_t *out, size_t out_cap)
{
    size_t no = 0u;

    for (size_t i = 0u; i < count && no < out_cap; i++) {
        uint32_t id = ids[i];
        size_t word = (size_t)id >> 6;

        if (word < nwords &&
            (bitmap[word] & (UINT64_C(1) << (id & 63u))) != 0u) {
            out[no++] = id;
        }
    }

    return no;
}

static size_t
ks_intersect_bitmap_bitmap_scalar(const uint64_t *a,
                                  const uint64_t *b,
                                  size_t nwords,
                                  uint32_t *out,
                                  size_t out_cap)
{
    size_t no = 0u;

    for (size_t w = 0u; w < nwords && no < out_cap; w++) {
        uint64_t bits = a[w] & b[w];

        while (bits != 0u && no < out_cap) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            bits &= bits - 1u;
            out[no++] = (uint32_t)(w * 64u + bit);
        }
    }

    return no;
}

#if KS_X86
KS_TARGET_AVX2
static size_t
ks_intersect_bitmap_bitmap_avx2(const uint64_t *a,
                                const uint64_t *b,
                                size_t nwords,
                                uint32_t *out,
                                size_t out_cap)
{
    size_t no = 0u;
    size_t w = 0u;

    for (; w + 4u <= nwords && no < out_cap; w += 4u) {
        __m256i va = _mm256_loadu_si256(
            (const __m256i *)(const void *)(a + w));
        __m256i vb = _mm256_loadu_si256(
            (const __m256i *)(const void *)(b + w));
        __m256i vi = _mm256_and_si256(va, vb);

        uint64_t words[4];
        _mm256_storeu_si256((__m256i *)(void *)words, vi);

        for (unsigned lane = 0u; lane < 4u; lane++) {
            uint64_t bits = words[lane];

            while (bits != 0u && no < out_cap) {
                unsigned bit = (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1u;
                out[no++] =
                    (uint32_t)((w + lane) * 64u + bit);
            }
        }
    }

    if (no < out_cap) {
        no += ks_intersect_bitmap_bitmap_scalar(
            a + w, b + w, nwords - w,
            out + no, out_cap - no);
    }

    return no;
}
#endif

static size_t
ks_intersect_hybrid(const ks_hybrid_posting_t *a,
                    const ks_hybrid_posting_t *b,
                    uint32_t *out,
                    size_t out_cap)
{
    if (!a || !b || !out || out_cap == 0u) {
        return 0u;
    }

    if (a->kind == KS_POSTING_SPARSE32 &&
        b->kind == KS_POSTING_SPARSE32) {
        return ks_intersect_u32_adaptive(
            a->u.sparse.ids, a->u.sparse.count,
            b->u.sparse.ids, b->u.sparse.count,
            out, out_cap);
    }

    if (a->kind == KS_POSTING_SPARSE32 &&
        b->kind == KS_POSTING_BITMAP64) {
        return ks_intersect_sparse_bitmap(
            a->u.sparse.ids, a->u.sparse.count,
            b->u.bitmap.words, b->u.bitmap.nwords,
            out, out_cap);
    }

    if (a->kind == KS_POSTING_BITMAP64 &&
        b->kind == KS_POSTING_SPARSE32) {
        return ks_intersect_sparse_bitmap(
            b->u.sparse.ids, b->u.sparse.count,
            a->u.bitmap.words, a->u.bitmap.nwords,
            out, out_cap);
    }

    size_t nwords = a->u.bitmap.nwords < b->u.bitmap.nwords
        ? a->u.bitmap.nwords
        : b->u.bitmap.nwords;

#if KS_X86 && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        return ks_intersect_bitmap_bitmap_avx2(
            a->u.bitmap.words, b->u.bitmap.words,
            nwords, out, out_cap);
    }
#endif

    return ks_intersect_bitmap_bitmap_scalar(
        a->u.bitmap.words, b->u.bitmap.words,
        nwords, out, out_cap);
}
```

## 3.5 Best multi-way strategy for hybrid postings

For a mixed query:

1. Choose the smallest sparse posting as the candidate stream.
2. For each candidate:
   - bitmap posting: one word load and bit test,
   - sparse posting: monotonic galloping.
3. If all postings are bitmaps:
   - AND them word-by-word into a query workspace,
   - enumerate set bits only at the end.

Do not materialize every intermediate dense result as `uint32_t[]`; that throws away the bitmap advantage.

---

## 3.6 Better sparse compression after finalization

A bitmap is not enough for all workloads. For immutable storage, a useful three-tier representation is:

1. Tiny lists: `uint32_t[]`
2. Medium lists: block delta encoding
3. Dense lists: bitmap

A practical block format:

```c
#define KS_DELTA_BLOCK 128u

typedef struct ks_delta_block_header {
    uint32_t base;
    uint16_t count;
    uint8_t bit_width;
    uint8_t reserved;
    uint32_t payload_offset;
} ks_delta_block_header_t;
```

Each block stores:

- first ID as `base`,
- up to 127 deltas,
- bit-packed fixed-width deltas,
- block maximum available from the next block/base metadata.

This reduces cache bandwidth but complicates random membership. Introduce it only after arena storage and bitmap conversion; those changes are simpler and usually larger wins.

---

# 4. Multi-threaded ingestion

## 4.1 Do not lock every trigram insertion

Striped locks around the global hash table are easy to implement but perform poorly because:

- common trigrams concentrate contention,
- posting-list growth still needs synchronized reallocations,
- hash-table resizing requires global coordination,
- the shared 2 MB `doc_seen` bitmap cannot be reused concurrently.

The preferred design is:

```text
documents
   |
parallel partition
   |
thread-local trigram builders
   |
parallel/local finalize
   |
global merge by trigram key
```

Each worker owns:

- its own trigram directory,
- posting chunks,
- dedup bitmap,
- touched-byte list,
- document metadata staging.

No locks are required in the ingestion hot path.

---

## 4.2 Assign contiguous document ranges

Partition documents into contiguous ranges:

```text
worker 0: doc IDs       0 .. 249999
worker 1: doc IDs  250000 .. 499999
worker 2: doc IDs  500000 .. 749999
worker 3: doc IDs  750000 .. 999999
```

Then every local posting is sorted, and global merging for a trigram is simple concatenation in worker order. No k-way sort is needed.

Dynamic scheduling gives better load balancing for highly variable document sizes, but produces interleaved ID ranges. In that case, either:

- assign IDs after ingestion, or
- perform a true k-way merge of local postings.

For raw throughput, static byte-balanced contiguous partitions are usually preferable.

---

## 4.3 OpenMP build skeleton

```c
typedef struct ks_input_document {
    const char *name;
    const char *data;
    size_t length;
} ks_input_document_t;

typedef struct ks_local_builder ks_local_builder_t;

/* Application-specific local builder functions. */
static int ks_local_builder_init(ks_local_builder_t *b,
                                 uint32_t first_doc_id,
                                 uint32_t doc_count);
static int ks_local_builder_add(ks_local_builder_t *b,
                                uint32_t doc_id,
                                const ks_input_document_t *doc);
static void ks_local_builder_destroy(ks_local_builder_t *b);
static int ks_merge_local_builders(keystone_trigram_index_t *idx,
                                   ks_local_builder_t *builders,
                                   size_t builder_count);

int
ks_build_parallel(keystone_trigram_index_t *idx,
                  const ks_input_document_t *docs,
                  size_t doc_count,
                  unsigned thread_count)
{
    if (!idx || (!docs && doc_count != 0u) || thread_count == 0u) {
        return KEYSTONE_TRIGRAM_EINVAL;
    }

    ks_local_builder_t *builders =
        (ks_local_builder_t *)calloc(thread_count, sizeof(*builders));
    if (!builders) {
        return KEYSTONE_TRIGRAM_ENOMEM;
    }

    int failed = 0;

#pragma omp parallel num_threads(thread_count) shared(failed)
    {
        unsigned tid = (unsigned)omp_get_thread_num();
        unsigned nth = (unsigned)omp_get_num_threads();

        size_t begin = (doc_count * tid) / nth;
        size_t end = (doc_count * (tid + 1u)) / nth;

        if (ks_local_builder_init(
                &builders[tid],
                (uint32_t)begin,
                (uint32_t)(end - begin)) != 0) {
#pragma omp atomic write
            failed = 1;
        }

#pragma omp barrier

        if (!failed) {
            for (size_t i = begin; i < end; i++) {
                if (ks_local_builder_add(
                        &builders[tid], (uint32_t)i, &docs[i]) != 0) {
#pragma omp atomic write
                    failed = 1;
                    break;
                }
            }
        }
    }

    int rc = failed
        ? KEYSTONE_TRIGRAM_ESTATE
        : ks_merge_local_builders(idx, builders, thread_count);

    for (unsigned t = 0u; t < thread_count; t++) {
        ks_local_builder_destroy(&builders[t]);
    }
    free(builders);

    return rc;
}
```

In real code, use an atomic failure flag or OpenMP atomic operations consistently rather than racing on a plain `int`.

---

## 4.4 Merge architecture

The merge should be performed in two passes.

### Pass 1: determine global frequencies

Each local builder exports:

```c
(gram, local_count)
```

Accumulate total counts into the global directory. This allows exact one-time allocation for every final posting.

### Pass 2: copy postings

Compute a destination offset per trigram. Since each worker owns a contiguous document range, copy worker postings in worker order:

```text
global_posting =
    local_worker_0_posting ||
    local_worker_1_posting ||
    ...
```

This eliminates all global posting `realloc()` operations.

The merge can itself be parallelized by partitioning the 24-bit trigram key space among workers.

---

## 4.5 If immediate shared ingestion is required

Use striped ownership rather than striped locks:

```text
owner_thread = hash(gram) & (thread_count - 1)
```

Document workers extract unique trigrams and enqueue `(gram, doc_id)` records to owner-thread SPSC/MPSC queues. Each owner is the sole writer for its posting subset.

This avoids bucket locks, but queue traffic and cross-core cache movement are usually slower than thread-local builders followed by a merge.

---

# 5. Hash-table and directory optimization

## 5.1 Current split table assessment

The split design has one good property:

- misses inspect only the compact `bucket_keys[]`.

However, `bucket_lists[]` is still allocated at full table capacity, even though most misses do not read it. On a 64-bit ABI:

```c
typedef struct {
    uint32_t *doc_ids; /* 8 */
    size_t count;      /* 8 */
    size_t capacity;   /* 8 */
} posting;             /* 24 bytes */
```

At a 70% load factor, approximate directory cost per unique trigram is:

```text
(4 + 24) / 0.70 = 40 bytes per trigram
```

That is excessive. One million unique trigrams require roughly 40 MB before posting arrays.

The initial 65,536 buckets also guarantee repeated full-table rehashes for broad corpora.

---

## 5.2 Direct 24-bit radix directory

The entire trigram space contains exactly 16,777,216 keys. A direct array of `uint32_t` descriptor indices costs:

```text
16,777,216 * 4 = 64 MiB
```

Use zero as absent and `descriptor_index + 1` as present:

```c
#define KS_TRIGRAM_SPACE (1u << 24)

typedef struct ks_direct_directory {
    uint32_t *slot; /* 64 MiB */
} ks_direct_directory_t;

static inline uint32_t
ks_direct_lookup(const ks_direct_directory_t *d, uint32_t gram)
{
    return d->slot[gram & 0x00FFFFFFu];
}
```

Advantages:

- one indexed load,
- no hash multiply,
- no probing,
- no key comparison,
- no resize,
- deterministic latency.

The direct directory becomes memory-competitive against the current hash table at roughly:

```text
64 MiB / 40 bytes ≈ 1.67 million unique trigrams
```

It can be faster well below that threshold because it eliminates probing and rehashing.

Recommended policy:

- small/sparse index: compact hash directory,
- large index or broad binary corpus: direct 24-bit directory.

---

## 5.3 Better sparse hash directory

A SwissTable-style structure should separate:

```c
uint8_t  control[num_slots + group_width];
uint32_t keys[num_slots];
uint32_t descriptor_index[num_slots];
```

Do not put the full posting descriptor inline in every hash slot. Store descriptors densely only for occupied keys.

A control byte can encode:

- empty,
- deleted if mutation requires it,
- 7-bit hash fingerprint.

Probe 16 or 32 control bytes at once, then inspect only matching keys.

Suggested structure:

```c
typedef struct ks_posting_directory {
    uint8_t *ctrl;
    uint32_t *keys;
    uint32_t *desc_index;
    size_t capacity;
    size_t size;
} ks_posting_directory_t;
```

This is substantially more cache-efficient than the current `bucket_lists[num_buckets]`.

Because KEYSTONE has no deletion and becomes immutable, Robin Hood or Swiss-style insertion can be simplified.

---

## 5.4 Fibonacci hash observations

Fibonacci hashing is acceptable for 24-bit keys and power-of-two capacities, but it is not the dominant problem. The main problem is the slot footprint and probing.

There is also an implicit limit in:

```c
unsigned shift = 32u - ctz(num_buckets);
```

If the table grows beyond `2^32`, the shift calculation becomes invalid for a 32-bit multiplied hash. That size should never be needed for a 24-bit key universe; enforce:

```c
num_buckets <= (1u << 25)
```

or switch to a 64-bit multiply.

A direct directory makes this irrelevant.

---

# 6. Posting construction and memory layout

## 6.1 Remove one allocation per unique trigram

This code is especially costly:

```c
plist->doc_ids = malloc(8 * sizeof(uint32_t));
```

for every newly observed trigram.

For one million unique trigrams, this creates one million tiny allocations and at least 32 MB of mostly underused initial posting capacity, excluding allocator metadata.

Use chunked posting construction:

```c
#define KS_POSTING_CHUNK_IDS 256u

typedef struct ks_posting_chunk {
    struct ks_posting_chunk *next;
    uint32_t used;
    uint32_t ids[KS_POSTING_CHUNK_IDS];
} ks_posting_chunk_t;

typedef struct ks_build_posting {
    ks_posting_chunk_t *head;
    ks_posting_chunk_t *tail;
    uint32_t count;
} ks_build_posting_t;
```

Allocate chunks from large arenas:

```c
#define KS_ARENA_CHUNK_BYTES (4u * 1024u * 1024u)

typedef struct ks_arena_block {
    struct ks_arena_block *next;
    size_t used;
    size_t capacity;
    unsigned char data[];
} ks_arena_block_t;
```

At finalize:

1. determine exact posting counts,
2. allocate one contiguous posting pool,
3. flatten chunks into it,
4. discard build arenas.

Final immutable descriptor:

```c
typedef struct ks_final_posting {
    uint32_t offset;
    uint32_t count;
    uint8_t kind;
    uint8_t reserved[3];
} ks_final_posting_t;
```

If total postings can exceed `UINT32_MAX`, make `offset` 64-bit.

---

## 6.2 Compact at finalize

The current geometric capacities remain allocated forever. Finalization should shrink or flatten postings. For immutable indexes, retaining capacities has no value.

At minimum:

```c
if (plist->count < plist->capacity) {
    uint32_t *p = realloc(
        plist->doc_ids,
        plist->count * sizeof(uint32_t));
    if (p || plist->count == 0u) {
        plist->doc_ids = p;
        plist->capacity = plist->count;
    }
}
```

A single contiguous posting pool is better because:

- fewer allocator objects,
- better locality,
- cheaper persistence,
- easier memory mapping,
- smaller descriptor.

---

# 7. Search and candidate-path corrections

## 7.1 Stop allocating `doc_count` candidates per search

Current behavior:

```c
malloc(idx->doc_count * sizeof(uint32_t))
```

is catastrophic for large collections and concurrent searches.

Instead:

1. extract query trigrams,
2. locate and sort postings,
3. allocate or reuse only `min(rarest_count, max_needed)` candidate slots,
4. verify candidates as they are produced.

For search, there is no need to materialize all candidates. Use a fixed page:

```c
#define KS_VERIFY_BATCH 1024u
uint32_t candidates[KS_VERIFY_BATCH];
```

Iterate pages and stop when `max_matches` is reached.

Thread-local query workspaces are preferable:

```c
typedef struct keystone_query_workspace {
    uint32_t *a;
    uint32_t *b;
    size_t capacity;
} keystone_query_workspace_t;
```

Expose an optional workspace API or maintain caller-owned contexts. Avoid putting one mutable workspace inside the index because concurrent searches would contend.

---

## 7.2 Query trigram cap

`TRIGRAM_QUERY_MAX_UNIQUE == 64` limits candidate rejection for long patterns. It does not cause false negatives because omitted trigrams merely weaken filtering, but long patterns should select the most discriminating trigrams rather than the first 64.

A better query planner:

1. extract unique trigrams,
2. look up frequencies,
3. retain the 8–32 rarest,
4. intersect in frequency order.

Usually 8 rare trigrams are enough. Processing 64 lists often costs more than exact verification of the already-small candidate set.

For very long patterns, sample trigrams across the full pattern rather than using only its prefix.

---

## 7.3 Exact verification

The comment says SSE4.2 substring search using `_mm_cmpestri`, but the implementation does not use `_mm_cmpestri`; it uses ordinary SSE2-style comparisons and `movemask`.

That is not inherently a problem. `_mm_cmpestri` is often not faster than a first/last-byte filter.

Improvements:

- use AVX2 to scan 32 candidate positions,
- compare first and last bytes,
- for long needles compare first and last 16/32-byte blocks before full `memcmp`,
- use the same ASCII-fold semantics as indexing.

The case-insensitive verifier is currently scalar and calls `tolower()` for every compared byte. Pre-fold the query once and add an AVX2 ASCII-folding verifier.

---

## 7.4 Statistics semantics

This line is misleading:

```c
candidate_docs_rejected += doc_count - num_candidates;
```

It records trigram-stage rejection, not candidates rejected by exact verification.

Use separate counters:

```c
trigram_docs_rejected
verification_docs_evaluated
verification_docs_rejected
```

Also saturate additions to avoid wraparound.

---

# 8. Genuine streaming ingestion

The current stream is a document buffer, not a streaming indexer. It:

- stores all bytes,
- reallocates as the document grows,
- indexes only at `end_document()`,
- duplicates content again when retention is enabled.

A real stream needs only:

- the last two bytes from the previous chunk,
- current document ID,
- per-document dedup state,
- optional retained-content buffer.

```c
struct keystone_trigram_stream {
    keystone_trigram_index_t *idx;
    char *name;

    unsigned char carry[2];
    size_t carry_len;

    char *retained;
    size_t retained_len;
    size_t retained_cap;

    uint32_t doc_id;
    size_t total_len;
    bool retain_content;
    bool failed;
};
```

For every feed:

1. combine `carry + first bytes of new chunk`,
2. emit the one or two boundary-crossing trigrams,
3. vectorize the body,
4. preserve the final two bytes,
5. optionally append raw content.

This removes the full temporary document copy for external-only ingestion.

A document slot and ID should be reserved at `begin_document()`. On cancellation or failure, either roll back before any later document starts or poison the index, matching the existing fail-closed policy.

---

# 9. Additional correctness and performance findings

## High priority

### Case-insensitive iterator bug

As noted, `keystone_trigram_candidates_begin()` does not fold query trigrams. This can return an empty iterator for valid case-insensitive matches.

### Locale inconsistency

Vector code performs ASCII folding; scalar code uses locale-sensitive `tolower()`. Standardize on ASCII or explicitly document and implement a stable byte-folding table.

### Iterator loses monotonicity

Repeated `bsearch()` is avoidable and should be replaced with per-list cursors.

### Streaming doubles work

The external streaming mode should not buffer the entire document at all.

### Search allocation scales with all documents

Candidate storage should scale with the rarest posting or use paging.

## Medium priority

### SIMD feature guard is too restrictive

`_mm_shuffle_epi8` requires SSSE3, not SSE4.2. Most of the substring code requires only SSE2/SSSE3. Compile distinct target functions and runtime-dispatch them instead of using a single `__SSE4_2__` compile-time gate.

### Hash resize check on every trigram

`find_or_create_posting_list()` recomputes the resize threshold and checks it even when the key already exists. Resize only when insertion discovers an empty slot, or maintain `resize_at` in the index.

### Finalized index retains ingestion bitmap

After finalize, free:

```c
doc_seen
doc_seen_touched
```

unless incremental mutation will be supported. This releases at least 2 MB plus touched-list capacity.

### `memory_usage()` is incomplete

It omits:

- `sizeof(*idx)`,
- `doc_seen`,
- `doc_seen_touched`,
- allocator overhead,
- stream/query workspaces.

It also performs unchecked arithmetic.

### Short-pattern queries enumerate all documents

This is unavoidable with only trigrams. If one- and two-byte search is important, add optional byte and bigram indexes, or explicitly route these queries to a linear SIMD verifier.

## Lower-priority hardening

### Persistence is ABI-dependent

The file writes:

- native endianness,
- native `size_t` fields inside `stats`,
- compiler-dependent struct padding.

It is not portable across architectures or ABI versions. Use an explicitly encoded fixed-width disk format.

### Load validation is incomplete

Validate:

- trigram key `< 1 << 24`,
- unique count `<= 1 << 24`,
- document IDs equal expected IDs or at least in range,
- posting IDs strictly increasing,
- posting IDs `< doc_count`,
- `owns_content` is 0 or 1,
- stats are recomputed rather than trusted.

### Saving should be atomic

Write to a temporary file, flush, and rename. This is secondary to the hot-path work but prevents partial files.

---

# 10. Recommended final architecture

A high-throughput version of KEYSTONE should use the following phases.

## Build representation

```text
thread-local direct/sparse directory
    -> arena-backed posting chunks
    -> per-thread document dedup bitmap
```

## Merge/finalize

```text
frequency pass
    -> exact global allocation
    -> concatenate sorted local postings
    -> convert high-frequency postings to bitmap
    -> compact descriptors
    -> free build-only state
```

## Final directory

Choose adaptively:

```text
few unique trigrams:
    Swiss-style control bytes + key + descriptor index

many unique trigrams:
    64 MiB direct 24-bit descriptor directory
```

## Query execution

```text
extract query trigrams
    -> frequency lookup
    -> choose rarest/selective grams
    -> sparse SIMD or galloping intersection
    -> bitmap membership filtering
    -> paged exact verification
```

## Persistence

Store:

```text
fixed header
document metadata
optional content blob
direct/hash directory
posting descriptors
contiguous sparse posting pool
bitmap pool
```

This layout can later be memory-mapped with almost no reconstruction.

---

# 11. Implementation priority

### Phase 1: immediate wins

1. Fix case-insensitive iterator extraction.
2. Replace iterator `bsearch()` with monotonic cursors.
3. Eliminate `doc_count` candidate allocation.
4. Free ingestion-only bitmap state at finalize.
5. Standardize ASCII folding.
6. Add adaptive galloping/SIMD sparse intersection.

### Phase 2: memory and indexing throughput

1. Arena/chunk posting construction.
2. Exact flattening at finalize.
3. Dense bitmap conversion.
4. Direct 24-bit directory for large indexes.
5. Real streaming extraction with two-byte carry.

### Phase 3: parallel construction

1. Thread-local builders.
2. Contiguous document range assignment.
3. Frequency/count merge pass.
4. Parallel flattening and bitmap conversion.

The largest practical gains will come from eliminating allocator traffic and unnecessary memory movement. AVX2/AVX-512 intersection then becomes valuable because the data representation is compact enough for SIMD execution rather than being dominated by cache misses and heap indirection.