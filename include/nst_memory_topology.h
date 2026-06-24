/* NST Memory Topology Validator & DRAM Row-Buffer Scorer
 * Validates local memory-layout topology for batch workloads
 * and estimates DRAM locality for large anchor tables.
 */

#ifndef NST_MEMORY_TOPOLOGY_H
#define NST_MEMORY_TOPOLOGY_H

#include "nst_platform_hints.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void _nst_topo_validate_external_reachability(_nst_numa_placement_hint_t* hint);
void _nst_topo_contribute_lane_bits(uint64_t* scratch, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
