/*
 * KEYSTONE Federated Distributed Query & Partial Result Merging
 *
 * Provides multi-node query routing across local compute node indexers,
 * site indexers, and global aggregators. Implements query-type specific merging
 * (top-k rerank, monotonic HLC timeline merge, freshest generation resolution)
 * and resilient partial-result semantics under network partition.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#ifndef KEYSTONE_FEDERATED_QUERY_H
#define KEYSTONE_FEDERATED_QUERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"
#include "keystone_topology.h"
#include "keystone_incident.h"
#include "keystone_telemetry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYSTONE_MAX_FEDERATED_NODES 64
#define KEYSTONE_MAX_ENDPOINT_LEN 128

/* --- Node Health & Partition Status --- */
typedef enum {
    KEYSTONE_NODE_HEALTHY     = 0,
    KEYSTONE_NODE_DEGRADED    = 1,
    KEYSTONE_NODE_UNREACHABLE = 2
} keystone_node_health_t;

/* --- Routing Target Strategy --- */
typedef enum {
    KEYSTONE_ROUTE_LOCAL_ONLY    = 0,
    KEYSTONE_ROUTE_SPECIFIC_NODE = 1,
    KEYSTONE_ROUTE_SITE          = 2,
    KEYSTONE_ROUTE_ALL_HEALTHY   = 3
} keystone_route_target_t;

/* --- Remote Federated Node Descriptor --- */
typedef struct {
    keystone_uuid_t node_id;
    uint32_t site_id;
    keystone_node_health_t health;
    char endpoint[KEYSTONE_MAX_ENDPOINT_LEN];
    uint64_t latest_index_generation;
    keystone_hlc_t last_heartbeat_hlc;
    uint32_t latency_us;
} keystone_remote_node_t;

/* --- Distributed Partial Execution Status --- */
typedef struct {
    bool is_partial;                 /* True if one or more nodes were unreachable/degraded */
    uint32_t nodes_queried;          /* Total target nodes in scope */
    uint32_t nodes_responded;        /* Number of nodes that successfully contributed */
    uint64_t slowest_response_us;    /* Maximum latency encountered across responding nodes */
    uint64_t freshest_generation;    /* Highest index generation observed */
    keystone_hlc_t max_hlc_observed; /* Highest HLC timestamp in merged result */
} keystone_partial_status_t;

/* Opaque Federated Query Coordinator */
typedef struct keystone_federated_coordinator keystone_federated_coordinator_t;

/* --- Coordinator Lifecycle --- */
keystone_federated_coordinator_t* keystone_federated_coordinator_create(
    const keystone_uuid_t* local_node_id,
    uint32_t local_site_id
);

void keystone_federated_coordinator_destroy(
    keystone_federated_coordinator_t* coord
);

/* --- Node Registry Operations --- */
int keystone_federated_register_node(
    keystone_federated_coordinator_t* coord,
    const keystone_remote_node_t* node
);

int keystone_federated_update_node_health(
    keystone_federated_coordinator_t* coord,
    const keystone_uuid_t* node_id,
    keystone_node_health_t health,
    uint64_t generation,
    const keystone_hlc_t* heartbeat_hlc
);

size_t keystone_federated_node_count(
    const keystone_federated_coordinator_t* coord
);

/* --- Federated Query Merging Primitives --- */

/*
 * Merge exact identity lookups across nodes.
 * Resolves conflict by selecting the entry with the highest fencing epoch,
 * highest source generation, and monotonic HLC tie-breaker.
 */
int keystone_federated_merge_exact_identity(
    const keystone_exact_entry_t* candidates,
    size_t candidate_count,
    keystone_exact_entry_t* out_winner
);

/*
 * Merge temporal timeline event streams from multiple nodes.
 * Deduplicates events by event UUID and preserves strict ascending HLC order.
 */
size_t keystone_federated_merge_timelines(
    const keystone_temporal_entry_t* const* node_records,
    const size_t* record_counts,
    size_t num_nodes,
    keystone_temporal_entry_t* out_merged,
    size_t max_out,
    keystone_partial_status_t* out_status
);

/*
 * Merge top-K similarity search results from multiple indexer nodes.
 * Deduplicates by incident UUID, sorts by similarity score descending,
 * and tracks partition resilience status.
 */
size_t keystone_federated_merge_topk_incidents(
    const keystone_incident_match_t* const* node_matches,
    const size_t* match_counts,
    size_t num_nodes,
    keystone_incident_match_t* out_merged,
    size_t max_out,
    keystone_partial_status_t* out_status
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_FEDERATED_QUERY_H */
