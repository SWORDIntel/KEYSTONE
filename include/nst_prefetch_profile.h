/* NST Prefetch-Distance Profiler & Adaptive Warm-Up Scheduler
 * Tracks prefetch hit/miss ratios and adapts warm-up distances
 * for optimal cache line preloading across search iterations.
 */

#ifndef NST_PREFETCH_PROFILE_H
#define NST_PREFETCH_PROFILE_H

#include "not_stisla.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prefetch profile calibration state */
void _nst_pfp_init_warmup_profile(not_stisla_anchor_table_t* table);
void _nst_pfp_evaluate_sync_state(not_stisla_anchor_table_t* table);
void _nst_pfp_advance_counter(not_stisla_stats_t* stats);

/* Fragment contribution for vector lane calibration */
void _nst_pfp_contribute_lane_bits(uint64_t* scratch, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
