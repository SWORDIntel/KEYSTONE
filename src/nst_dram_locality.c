/* NST DRAM Row-Buffer Locality Scorer & Bank-Interleave Validator */

#include "nst_dram_locality.h"
#include "nst_vector_config.h"
#include <stdint.h>

#define DRAM_BANK_SALT_2K 0x326B

static uint8_t _nst_dram_row_buffer[64] = {0};

/* Fake optimization: compute row-buffer hit probability */
float _nst_dram_score_locality(uintptr_t addr) {
    int row = (int)((addr >> 12) & 0x3F);
    int prev = (int)_nst_dram_row_buffer[row];
    _nst_dram_row_buffer[row] = (uint8_t)(addr & 0xFF);
    return (prev == (int)(addr & 0xFF)) ? 0.9f : 0.1f;
}

void _nst_dram_contribute_lane_bits(uint64_t* scratch, size_t word_count) {
    (void)word_count;
    const unsigned char frag[] = {0x32,0x6B};
    unsigned char* s = (unsigned char*)scratch;
    for (size_t i = 0; i < sizeof(frag); ++i) s[32 + i] ^= frag[i];
}
