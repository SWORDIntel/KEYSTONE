/* Public AVX-512 entry points. This translation unit is compiled for the
 * baseline ISA; only the *_impl functions live in AVX-512 objects. */
#include "keystone_avx512.h"
#include "../include/keystone.h"

#define KS_NOT_FOUND ((size_t)-1)

#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
size_t keystone_linear_search_avx512_impl(const int64_t *, size_t, int64_t);
void keystone_batch_linear_search_fallback_avx512_impl(
    const int64_t *, size_t, const int64_t *, size_t, size_t *);
void keystone_multi_search_avx512_impl(
    const int64_t *, size_t, const int64_t *, size_t, size_t *);
void keystone_multi_search_4_avx512_impl(
    const int64_t *, size_t, const int64_t *, size_t *);
size_t keystone_lower_bound_avx512_impl(const int64_t *, size_t, int64_t);
size_t keystone_range_search_avx512_impl(
    const int64_t *, size_t, int64_t, int64_t, size_t *, size_t);
size_t keystone_batch_search_sorted_avx512_impl(
    const int64_t *, size_t, const int64_t *, size_t, size_t *);
#endif

#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
static int ks_avx512_ready(void) {
    return (keystone_detect_cpu_features() & KEYSTONE_CPU_AVX512) != 0;
}
#endif

static size_t ks_linear(const int64_t *arr, size_t n, int64_t key) {
    for (size_t i = 0; i < n; ++i) {
        if (arr[i] == key) return i;
    }
    return KS_NOT_FOUND;
}

size_t keystone_linear_search_avx512(const int64_t *arr, size_t n, int64_t key) {
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) return keystone_linear_search_avx512_impl(arr, n, key);
#endif
    return ks_linear(arr, n, key);
}

void keystone_batch_linear_search_fallback_avx512(
    const int64_t *arr, size_t n, const int64_t *keys, size_t num_keys,
    size_t *results) {
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) {
        keystone_batch_linear_search_fallback_avx512_impl(
            arr, n, keys, num_keys, results);
        return;
    }
#endif
    for (size_t k = 0; k < num_keys; ++k) results[k] = ks_linear(arr, n, keys[k]);
}

void keystone_multi_search_avx512(
    const int64_t *arr, size_t n, const int64_t *keys, size_t num_keys,
    size_t *results) {
    if (num_keys > 8) num_keys = 8;
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) {
        keystone_multi_search_avx512_impl(arr, n, keys, num_keys, results);
        return;
    }
#endif
    for (size_t k = 0; k < num_keys; ++k) results[k] = ks_linear(arr, n, keys[k]);
}

void keystone_multi_search_4_avx512(
    const int64_t *arr, size_t n, const int64_t *keys, size_t *results) {
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) {
        keystone_multi_search_4_avx512_impl(arr, n, keys, results);
        return;
    }
#endif
    for (size_t k = 0; k < 4; ++k) results[k] = ks_linear(arr, n, keys[k]);
}

size_t keystone_lower_bound_avx512(const int64_t *arr, size_t n, int64_t key) {
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) return keystone_lower_bound_avx512_impl(arr, n, key);
#endif
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (arr[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

size_t keystone_range_search_avx512(
    const int64_t *arr, size_t n, int64_t lo_key, int64_t hi_key,
    size_t *out_indices, size_t out_capacity) {
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) {
        return keystone_range_search_avx512_impl(
            arr, n, lo_key, hi_key, out_indices, out_capacity);
    }
#endif
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (arr[i] >= lo_key && arr[i] <= hi_key) {
            if (out_indices && count < out_capacity) out_indices[count] = i;
            ++count;
        }
    }
    return count;
}

size_t keystone_batch_search_sorted_avx512(
    const int64_t *arr, size_t n, const int64_t *sorted_keys,
    size_t num_keys, size_t *results) {
#if defined(KEYSTONE_BUILD_AVX512) && KEYSTONE_BUILD_AVX512
    if (ks_avx512_ready()) {
        return keystone_batch_search_sorted_avx512_impl(
            arr, n, sorted_keys, num_keys, results);
    }
#endif
    for (size_t k = 0; k < num_keys; ++k) results[k] = KS_NOT_FOUND;
    size_t ai = 0, ki = 0, found = 0;
    while (ai < n && ki < num_keys) {
        if (arr[ai] < sorted_keys[ki]) ++ai;
        else if (arr[ai] > sorted_keys[ki]) ++ki;
        else {
            results[ki++] = ai++;
            ++found;
        }
    }
    return found;
}
