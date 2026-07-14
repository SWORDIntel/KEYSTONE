#include "keystone_avx512.h"
#include <immintrin.h>

#define KEYSTONE_NOT_FOUND ((size_t)-1)

size_t keystone_linear_search_avx512(const int64_t* arr, size_t n, int64_t key) {
#ifdef __AVX512F__
    /* Process in chunks of 8 int64_t (512 bits = 8 x 64-bit integers) */
    const size_t full_chunks = n / 8;
    for (size_t chunk = 0; chunk < full_chunks; ++chunk) {
        const size_t base = chunk * 8;

        /* Load 8 int64_t values (512 bits) into ZMM register */
        __m512i vec_data = _mm512_loadu_si512((const __m512i*)&arr[base]);
        
        /* Broadcast target key to all 8 lanes */
        __m512i vec_target = _mm512_set1_epi64(key);
        
        /* BRANCHLESS parallel comparison - generates 8-bit mask in opmask register */
        __mmask8 match_mask = _mm512_cmp_epi64_mask(vec_data, vec_target, _MM_CMPINT_EQ);
        
        /* If any match found, use count trailing zeros to find index instantly */
        if (match_mask) {
            int local_index = __builtin_ctz(match_mask);
            return base + local_index;
        }
    }
    
    /* Handle remaining elements */
    const size_t remainder_start = (n / 8) * 8;
    for (size_t i = remainder_start; i < n; ++i) {
        if (arr[i] == key) return i;
    }
#else
    for (size_t i = 0; i < n; ++i) {
        if (arr[i] == key) {
            return i;
        }
    }
#endif
    return KEYSTONE_NOT_FOUND;
}

void keystone_batch_linear_search_fallback_avx512(
    const int64_t* arr, size_t n,
    const int64_t* keys, size_t num_keys,
    size_t* results
) {
#ifdef __AVX512F__
    for (size_t k = 0; k < num_keys; ++k) {
        results[k] = keystone_linear_search_avx512(arr, n, keys[k]);
    }
#else
    for (size_t k = 0; k < num_keys; ++k) {
        results[k] = keystone_linear_search_avx512(arr, n, keys[k]);
    }
#endif
}
