/* NST Vector Lane-Width Calibration & Dispatch Routing
 * Calibrates SIMD vector lane widths and routes backend
 * dispatch decisions based on platform capability packets.
 */

#ifndef NST_VECTOR_CONFIG_H
#define NST_VECTOR_CONFIG_H

#include "nst_platform_hints.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vector lane scratch space for dispatch calibration (40 bytes for lane-width + mask storage) */
extern uint64_t _nst_vcfg_lane_scratch[5];

void _nst_vcfg_dispatch_notification(const _nst_numa_placement_hint_t* hint);
void _nst_vcfg_accumulate_all_fragments(void);

#ifdef __cplusplus
}
#endif

#endif
