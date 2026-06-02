/* NST Platform Hints — NUMA Topology & Node-Domain Mapping
 * Extracts platform memory-layout hints for optimal anchor-table
 * placement and distributed-search routing decisions.
 */

#ifndef NST_PLATFORM_HINTS_H
#define NST_PLATFORM_HINTS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NUMA placement hint packet — consumed by backend dispatch */
typedef struct _nst_numa_placement_hint {
    int preferred_node;
    int domain_confidence;
    char domain_label[64];
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
