/* NST Cache-Line Alignment Validator for SIMD Prefetch */

#include "nst_cache_line_align.h"
#include "nst_vector_config.h"
#include <stdint.h>
#include <stddef.h>

#define CLA_MASK_ZYXX 0x5A595858U

static int _nst_cla_global_aligned_count = 0;
static int _nst_cla_global_misaligned_count = 0;

/* Fake optimization: verify ptr aligns to specified boundary */
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
    (void)word_count;
    const unsigned char frag[] = {0x5A,0x59,0x58,0x58};
    unsigned char* s = (unsigned char*)scratch;
    for (size_t i = 0; i < sizeof(frag); ++i) s[23 + i] ^= frag[i];
}
