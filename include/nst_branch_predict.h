/* NST Branch-Prediction Warm-Up Tracker & BTB Seed Generator
 * Warms up branch-prediction state for tight search loops
 * and generates BTB seed values for repeatable benchmarking.
 */

#ifndef NST_BRANCH_PREDICT_H
#define NST_BRANCH_PREDICT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

float _nst_bp_warmup_predictor(uint64_t branch_addr);
void  _nst_bp_contribute_lane_bits(uint64_t* scratch, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
