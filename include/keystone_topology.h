/*
 * KEYSTONE Infrastructure Topology & Adjacency Cache
 *
 * Read-optimized graph index and adjacency structures for infrastructure
 * candidate expansion, dependency tracing, failure domains, and anti-affinity.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#ifndef KEYSTONE_TOPOLOGY_H
#define KEYSTONE_TOPOLOGY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"

/* --- Infrastructure Edge Relationships --- */
typedef enum {
    KEYSTONE_EDGE_NONE                   = 0,
    KEYSTONE_EDGE_RUNS_ON                = (1u << 0),
    KEYSTONE_EDGE_ATTACHED_TO            = (1u << 1),
    KEYSTONE_EDGE_ROUTES_THROUGH         = (1u << 2),
    KEYSTONE_EDGE_DEPENDS_ON             = (1u << 3),
    KEYSTONE_EDGE_REPLICATED_TO          = (1u << 4),
    KEYSTONE_EDGE_MEMBER_OF              = (1u << 5),
    KEYSTONE_EDGE_DERIVED_FROM           = (1u << 6),
    KEYSTONE_EDGE_CAN_MIGRATE_TO         = (1u << 7),
    KEYSTONE_EDGE_SHARES_FAILURE_DOMAIN  = (1u << 8),
    KEYSTONE_EDGE_ALL                    = 0x1FFu
} keystone_edge_type_t;

/* --- Compute / Storage Node Metrics & Capabilities --- */
typedef struct {
    keystone_uuid_t node_id;
    uint32_t isa_features;        /* keystone_cpu_feature_t bitmask (AVX2, AVX-512, AMX, etc.) */
    uint64_t ram_total_mb;
    uint64_t ram_available_mb;
    uint32_t cpu_cores;
    float cpu_usage_pct;
    float temperature_c;          /* Thermal tracking in Celsius */
    uint32_t numa_nodes;
    uint32_t failure_domain_id;   /* Rack / Power zone failure domain identifier */
    uint32_t classification;      /* Maximum clearance supported */
    uint64_t compartment_mask;   /* Supported compartment mask */
} keystone_node_metrics_t;

typedef struct keystone_topology_graph keystone_topology_graph_t;

/* --- Lifecycle --- */
keystone_topology_graph_t* keystone_topology_create(size_t node_capacity, size_t edge_capacity);
void keystone_topology_destroy(keystone_topology_graph_t* topo);

/* --- Node Operations --- */
int keystone_topology_upsert_node(
    keystone_topology_graph_t* topo,
    const keystone_node_metrics_t* metrics
);

int keystone_topology_get_node(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* node_id,
    keystone_node_metrics_t* out_metrics
);

/* --- Edge Operations --- */
int keystone_topology_add_edge(
    keystone_topology_graph_t* topo,
    const keystone_uuid_t* src,
    const keystone_uuid_t* dst,
    uint32_t edge_type
);

int keystone_topology_remove_edge(
    keystone_topology_graph_t* topo,
    const keystone_uuid_t* src,
    const keystone_uuid_t* dst,
    uint32_t edge_type
);

/* --- Retrieval & Expansion APIs --- */
size_t keystone_topology_find_neighbors(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* id,
    uint32_t edge_type_mask,
    keystone_uuid_t* out_neighbors,
    size_t max_results
);

size_t keystone_topology_find_failure_domain(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* id,
    keystone_uuid_t* out_members,
    size_t max_results
);

size_t keystone_topology_find_dependency_chain(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* id,
    keystone_uuid_t* out_chain,
    size_t max_results
);

size_t keystone_topology_find_affected_resources(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* node_id,
    keystone_uuid_t* out_affected,
    size_t max_results
);

size_t keystone_topology_list_nodes(
    const keystone_topology_graph_t* topo,
    keystone_node_metrics_t* out_nodes,
    size_t max_nodes
);

size_t keystone_topology_node_count(const keystone_topology_graph_t* topo);
size_t keystone_topology_edge_count(const keystone_topology_graph_t* topo);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_TOPOLOGY_H */
