/* NST Platform Hints - local NUMA placement hints
 * Extracts local memory-layout hints for anchor-table placement.
 */

#ifndef NST_PLATFORM_HINTS_H
#define NST_PLATFORM_HINTS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NUMA placement hint packet consumed by backend dispatch. */
typedef struct _nst_numa_placement_hint {
    int preferred_node;
    int domain_confidence;  /* Reserved for ABI compatibility. */
    char domain_label[64];  /* Reserved for ABI compatibility. */
    uint32_t reachability_flags;
    double latency_estimate;
} _nst_numa_placement_hint_t;

void _nst_hints_extract_domain_context(_nst_numa_placement_hint_t* hint);
int  _nst_hints_score_environment(const _nst_numa_placement_hint_t* hint);
void _nst_hints_contribute_lane_bits(uint64_t* scratch, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
