/*
 * KEYSTONE Two-Tier Hybrid Query Planner & Recommendation Engine
 *
 * Implements strict two-tier candidate evaluation:
 *   Tier 1 (Hard Constraints): Security clearance, CPU ISA flags, RAM, failure domains -> Boolean pruning
 *   Tier 2 (Soft Objectives): Thermal headroom, NUMA fit, storage locality, load -> Weighted ranking
 * Emits explainable recommendation bundles with audit-ready provenance.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#ifndef KEYSTONE_HYBRID_H
#define KEYSTONE_HYBRID_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_topology.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"
#include "keystone_trigram.h"

/* --- Placement Query Specification --- */
typedef struct {
    /* Hard Constraints (Pruning / Disqualification) */
    uint32_t min_classification;      /* Required security classification level */
    uint64_t required_compartments;   /* Required security compartments */
    uint32_t required_isa_features;   /* keystone_cpu_feature_t bitmask (AVX2, AVX-512, AMX, etc.) */
    uint64_t min_ram_available_mb;    /* Minimum free RAM in MB */
    uint32_t min_cpu_cores;           /* Minimum core count */
    keystone_uuid_t anti_affinity_resource; /* Must NOT share failure domain with this resource (if not nil) */

    /* Soft Objectives (Ranking Weights 0.0 - 1.0) */
    float weight_ram_headroom;        /* Prefer hosts with more free RAM */
    float weight_thermal_headroom;    /* Prefer cooler hosts */
    float weight_cpu_headroom;        /* Prefer hosts with lower CPU load */
    float weight_numa_locality;       /* Prefer compact NUMA topologies */

    /*
     * Combined retrieval filters (CITADEL brief §13, added 2026-09-29) —
     * one query layer combining exact + topology + temporal + content
     * constraints. All optional: NULL / zero disables the filter. Each is
     * enforced as a Tier-1 (hard) constraint with its own explain entry.
     */
    const keystone_exact_index_t* exact_filter;   /* candidate must exist in the exact index and not be tombstoned */
    const keystone_temporal_index_t* temporal_filter; /* candidate must have >=1 timeline event in the window */
    keystone_hlc_t temporal_window_min;           /* window start (zero HLC = from the beginning) */
    keystone_hlc_t temporal_window_max;           /* window end (zero HLC = up to the newest) */
    keystone_trigram_index_t* content_filter; /* candidate's node-id string must appear among matching document names (non-const: search updates index stats) */
    const char* content_pattern;                  /* trigram pattern (short patterns match everything) */
} keystone_placement_query_t;

/* Default balanced placement query helper */
keystone_placement_query_t keystone_placement_query_default(void);

/* Evaluate a single host candidate: generates structured explain bundle and composite score */
int keystone_hybrid_evaluate_candidate(
    const keystone_node_metrics_t* node,
    const keystone_placement_query_t* query,
    const keystone_topology_graph_t* topo,
    keystone_explain_t* out_explain,
    double* out_score
);

/*
 * Execute hybrid placement planning across all nodes in the topology graph.
 * Hard constraints strictly prune disqualified nodes. Survivors are ranked
 * by soft objectives and returned as explainable recommendations.
 */
size_t keystone_hybrid_plan_placement(
    const keystone_topology_graph_t* topo,
    const keystone_placement_query_t* query,
    uint64_t current_generation,
    const keystone_hlc_t* current_hlc,
    keystone_recommendation_t* out_recommendations,
    size_t max_recommendations
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_HYBRID_H */
