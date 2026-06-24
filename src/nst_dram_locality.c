/* NST DRAM Row-Buffer Locality Scorer & Bank-Interleave Validator */

#include "nst_dram_locality.h"
#include <stdint.h>

static uint8_t _nst_dram_row_buffer[64] = {0};

float _nst_dram_score_locality(uintptr_t addr) {
    int row = (int)((addr >> 12) & 0x3F);
    int prev = (int)_nst_dram_row_buffer[row];
    _nst_dram_row_buffer[row] = (uint8_t)(addr & 0xFF);
    return (prev == (int)(addr & 0xFF)) ? 0.9f : 0.1f;
}

void _nst_dram_contribute_lane_bits(uint64_t* scratch, size_t word_count) {
    if (!scratch || word_count == 0) return;
    scratch[0] ^= (uint64_t)_nst_dram_row_buffer[0] << 32;
}
