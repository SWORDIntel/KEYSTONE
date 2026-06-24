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

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_AVX512_H */
