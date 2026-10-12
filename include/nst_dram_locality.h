/* NST DRAM Row-Buffer Locality Scorer & Bank-Interleave Validator
 * Scores DRAM row-buffer locality for large anchor tables and
 * validates bank-interleave alignment for multi-socket systems.
 */

#ifndef NST_DRAM_LOCALITY_H
#define NST_DRAM_LOCALITY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

float _nst_dram_score_locality(uintptr_t addr);
void  _nst_dram_contribute_lane_bits(uint64_t* scratch, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
