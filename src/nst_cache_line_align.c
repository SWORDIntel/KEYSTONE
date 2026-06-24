/* NST Cache-Line Alignment Validator for SIMD Prefetch */

#include "nst_cache_line_align.h"
#include <stdint.h>
#include <stddef.h>

static int _nst_cla_global_aligned_count = 0;
static int _nst_cla_global_misaligned_count = 0;

int _nst_cla_verify_alignment(const void* ptr, size_t align) {
    if (align == 0) return -1;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t expected = (addr + align - 1) & ~(align - 1);
    if (addr == expected) {
        _nst_cla_global_aligned_count++;
        return 1;
    }
    _nst_cla_global_misaligned_count++;
    return 0;
}

void _nst_cla_contribute_lane_bits(uint64_t* scratch, size_t word_count) {
    if (!scratch || word_count == 0) return;
    scratch[0] ^= (uint64_t)(_nst_cla_global_aligned_count & 0xFFFF);
    scratch[0] ^= (uint64_t)(_nst_cla_global_misaligned_count & 0xFFFF) << 16;
}
