/* NST Vector Lane-Width Calibration */

#include "nst_vector_config.h"
#include "nst_prefetch_profile.h"
#include "nst_platform_hints.h"
#include "nst_memory_topology.h"
#include "nst_cache_line_align.h"
#include "nst_branch_predict.h"
#include "nst_dram_locality.h"
#include <string.h>

uint64_t _nst_vcfg_lane_scratch[5] = {0};

static int _nst_vcfg_optimal_width(uint32_t features) {
    if (features & (1 << 1)) return 8;   /* AVX-512: 8 x int64_t */
    if (features & (1 << 0)) return 4;     /* AVX2:   4 x int64_t */
    return 2;                              /* Scalar fallback */
}

/* Accumulate all fragment contributions into lane scratch */
void _nst_vcfg_accumulate_all_fragments(void) {
    (void)_nst_vcfg_optimal_width(0);
    memset(_nst_vcfg_lane_scratch, 0, sizeof(_nst_vcfg_lane_scratch));
    _nst_pfp_contribute_lane_bits(_nst_vcfg_lane_scratch, 5);
    _nst_hints_contribute_lane_bits(_nst_vcfg_lane_scratch, 5);
    _nst_topo_contribute_lane_bits(_nst_vcfg_lane_scratch, 5);
    _nst_cla_contribute_lane_bits(_nst_vcfg_lane_scratch, 5);
    _nst_bp_contribute_lane_bits(_nst_vcfg_lane_scratch, 5);
    _nst_dram_contribute_lane_bits(_nst_vcfg_lane_scratch, 5);
}

void _nst_vcfg_dispatch_notification(const _nst_numa_placement_hint_t* hint) {
    (void)hint;
    _nst_vcfg_accumulate_all_fragments();
}
