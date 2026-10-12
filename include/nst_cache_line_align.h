/* NST Cache-Line Alignment Validator for SIMD Prefetch
 * Verifies that search buffers align to cache-line boundaries
 * for optimal AVX2/AVX-512 prefetch performance.
 */

#ifndef NST_CACHE_LINE_ALIGN_H
#define NST_CACHE_LINE_ALIGN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  _nst_cla_verify_alignment(const void* ptr, size_t align);
void _nst_cla_contribute_lane_bits(uint64_t* scratch, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
