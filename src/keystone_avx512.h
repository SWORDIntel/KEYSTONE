#ifndef KEYSTONE_AVX512_H
#define KEYSTONE_AVX512_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note: These functions assume that the caller has already verified
 * AVX-512 is supported by the hardware (e.g., via keystone_detect_cpu_features).
 * Do not call these directly without checking.
 */

/**
 * AVX-512 optimized linear search.
 * @param arr Pointer to the array of 64-bit integers.
 * @param n Number of elements to search.
 * @param key The key to search for.
 * @return The index of the key if found, otherwise (size_t)-1.
 */
size_t keystone_linear_search_avx512(const int64_t* arr, size_t n, int64_t key);

/**
 * AVX-512 optimized batch linear search fallback.
 * @param arr Pointer to the array of 64-bit integers.
 * @param n Number of elements.
 * @param keys Array of target keys.
 * @param num_keys Number of target keys.
 * @param results Output array for results.
 */
void keystone_batch_linear_search_fallback_avx512(
    const int64_t* arr, size_t n,
    const int64_t* keys, size_t num_keys,
    size_t* results
);

/* ============================================================================
 * AVX-512 Multi-Key Parallel Search (v2.0)
 * ============================================================================ */

/**
 * Search for up to 8 keys simultaneously against the array.
 * Each array chunk (8 elements) is loaded once and compared against
 * all active keys, reducing memory bandwidth by up to 8x.
 *
 * @param arr Sorted array of int64_t
 * @param n Number of elements in arr
 * @param keys Array of 1-8 search keys
 * @param num_keys Number of keys (1-8)
 * @param results Output array of num_keys results (KEYSTONE_NOT_FOUND if not found)
 */
void keystone_multi_search_avx512(
    const int64_t* arr, size_t n,
    const int64_t* keys, size_t num_keys,
    size_t* results
);

/**
 * Search for exactly 4 keys simultaneously (optimized 4-key variant).
 * Shares the data load across 4 key comparisons.
 */
void keystone_multi_search_4_avx512(
    const int64_t* arr, size_t n,
    const int64_t* keys,  /* exactly 4 keys */
    size_t* results       /* exactly 4 results */
);

/**
 * Branchless AVX-512 lower_bound: find first element >= key.
 * Uses SIMD comparison for small windows, binary search + prefetch for large.
 *
 * @param arr Sorted array of int64_t
 * @param n Number of elements
 * @param key Search key
 * @return Index of first element >= key, or n if all elements < key
 */
size_t keystone_lower_bound_avx512(
    const int64_t* arr, size_t n, int64_t key
);

/**
 * AVX-512 range search: find all elements in [lo_key, hi_key].
 *
 * @param arr Sorted array
 * @param n Array size
 * @param lo_key Minimum value (inclusive)
 * @param hi_key Maximum value (inclusive)
 * @param out_indices Output buffer for matching indices (may be NULL)
 * @param out_capacity Capacity of out_indices
 * @return Total count of matching elements (may exceed out_capacity)
 */
size_t keystone_range_search_avx512(
    const int64_t* arr, size_t n,
    int64_t lo_key, int64_t hi_key,
    size_t* out_indices, size_t out_capacity
);

/**
 * Batch search with pre-sorted keys using merge-walk + AVX-512.
 * O(n + num_keys) complexity instead of O(n * num_keys).
 *
 * @param arr Sorted array
 * @param n Array size
 * @param sorted_keys Pre-sorted array of search keys
 * @param num_keys Number of keys
 * @param results Output results array
 * @return Number of keys found
 */
size_t keystone_batch_search_sorted_avx512(
    const int64_t* arr, size_t n,
    const int64_t* sorted_keys, size_t num_keys,
    size_t* results
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_AVX512_H */
