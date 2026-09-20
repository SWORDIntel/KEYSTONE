/*
 * KEYSTONE Infrastructure Topology & Adjacency Cache Implementation
 *
 * Read-optimized graph index and adjacency structures for infrastructure
 * candidate expansion, dependency tracing, failure domains, and anti-affinity.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_topology.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    keystone_node_metrics_t metrics;
    uint32_t occupied;
} node_slot_t;

typedef struct {
    keystone_uuid_t src;
    keystone_uuid_t dst;
    uint32_t edge_type;
    uint32_t active;
} edge_entry_t;

struct keystone_topology_graph {
    node_slot_t* node_slots;
    size_t node_capacity;
    size_t node_mask;
    size_t node_count;

    edge_entry_t* edges;
    size_t edge_capacity;
    size_t edge_count;
};

static size_t next_pow2(size_t v) {
    if (v < 16) return 16;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
    v |= v >> 32;
#endif
    v++;
    return v;
}

keystone_topology_graph_t* keystone_topology_create(size_t node_capacity, size_t edge_capacity) {
    size_t ncap = next_pow2(node_capacity > 0 ? node_capacity : 256);
    size_t ecap = edge_capacity > 0 ? edge_capacity : 1024;

    keystone_topology_graph_t* topo = (keystone_topology_graph_t*)calloc(1, sizeof(*topo));
    if (!topo) return NULL;

    topo->node_capacity = ncap;
    topo->node_mask = ncap - 1;
    topo->node_slots = (node_slot_t*)calloc(ncap, sizeof(node_slot_t));

    topo->edge_capacity = ecap;
    topo->edges = (edge_entry_t*)calloc(ecap, sizeof(edge_entry_t));

    if (!topo->node_slots || !topo->edges) {
        keystone_topology_destroy(topo);
        return NULL;
    }

    return topo;
}

void keystone_topology_destroy(keystone_topology_graph_t* topo) {
    if (!topo) return;
    free(topo->node_slots);
    free(topo->edges);
    free(topo);
}

static void topology_grow_nodes(keystone_topology_graph_t* topo) {
    size_t new_cap = topo->node_capacity * 2;
    node_slot_t* new_slots = (node_slot_t*)calloc(new_cap, sizeof(node_slot_t));
    if (!new_slots) return;

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < topo->node_capacity; i++) {
        if (topo->node_slots[i].occupied) {
            uint64_t h = keystone_uuid_hash64(&topo->node_slots[i].metrics.node_id);
            size_t idx = (size_t)(h & new_mask);
            for (size_t p = 0; p < new_cap; p++) {
                size_t s = (idx + p) & new_mask;
                if (!new_slots[s].occupied) {
                    new_slots[s] = topo->node_slots[i];
                    break;
                }
            }
        }
    }

    free(topo->node_slots);
    topo->node_slots = new_slots;
    topo->node_capacity = new_cap;
    topo->node_mask = new_mask;
}

int keystone_topology_upsert_node(
    keystone_topology_graph_t* topo,
    const keystone_node_metrics_t* metrics
) {
    if (!topo || !metrics) return -1;

    uint64_t h = keystone_uuid_hash64(&metrics->node_id);
    size_t start_idx = (size_t)(h & topo->node_mask);

    /* 1. Update existing */
    for (size_t probe = 0; probe < topo->node_capacity; probe++) {
        size_t slot = (start_idx + probe) & topo->node_mask;
        if (!topo->node_slots[slot].occupied) break;

        if (keystone_uuid_equal(&topo->node_slots[slot].metrics.node_id, &metrics->node_id)) {
            topo->node_slots[slot].metrics = *metrics;
            return 0;
        }
    }

    /* 2. Check growth load factor (70%) */
    if ((topo->node_count + 1) * 10 >= topo->node_capacity * 7) {
        topology_grow_nodes(topo);
        start_idx = (size_t)(h & topo->node_mask);
    }

    /* 3. Insert new */
    for (size_t probe = 0; probe < topo->node_capacity; probe++) {
        size_t slot = (start_idx + probe) & topo->node_mask;
        if (!topo->node_slots[slot].occupied) {
            topo->node_slots[slot].metrics = *metrics;
            topo->node_slots[slot].occupied = 1;
            topo->node_count++;
            return 0;
        }
    }

    return -1;
}

int keystone_topology_get_node(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* node_id,
    keystone_node_metrics_t* out_metrics
) {
    if (!topo || !node_id || !out_metrics) return -1;

    uint64_t h = keystone_uuid_hash64(node_id);
    size_t start_idx = (size_t)(h & topo->node_mask);

    for (size_t probe = 0; probe < topo->node_capacity; probe++) {
        size_t slot = (start_idx + probe) & topo->node_mask;
        if (!topo->node_slots[slot].occupied) return -1;

        if (keystone_uuid_equal(&topo->node_slots[slot].metrics.node_id, node_id)) {
            *out_metrics = topo->node_slots[slot].metrics;
            return 0;
        }
    }
    return -1;
}

int keystone_topology_add_edge(
    keystone_topology_graph_t* topo,
    const keystone_uuid_t* src,
    const keystone_uuid_t* dst,
    uint32_t edge_type
) {
    if (!topo || !src || !dst || edge_type == 0) return -1;

    /* Check if duplicate */
    for (size_t i = 0; i < topo->edge_count; i++) {
        if (topo->edges[i].active &&
            topo->edges[i].edge_type == edge_type &&
            keystone_uuid_equal(&topo->edges[i].src, src) &&
            keystone_uuid_equal(&topo->edges[i].dst, dst)) {
            return 0;
        }
    }

    /* Expand capacity if needed */
    if (topo->edge_count >= topo->edge_capacity) {
        size_t new_cap = topo->edge_capacity * 2;
        size_t bytes = 0;
        if (!checked_mul_size(new_cap, sizeof(edge_entry_t), &bytes)) return -1;
        edge_entry_t* new_edges = (edge_entry_t*)realloc(topo->edges, bytes);
        if (!new_edges) return -1;
        topo->edges = new_edges;
        topo->edge_capacity = new_cap;
    }

    topo->edges[topo->edge_count].src = *src;
    topo->edges[topo->edge_count].dst = *dst;
    topo->edges[topo->edge_count].edge_type = edge_type;
    topo->edges[topo->edge_count].active = 1;
    topo->edge_count++;

    return 0;
}

int keystone_topology_remove_edge(
    keystone_topology_graph_t* topo,
    const keystone_uuid_t* src,
    const keystone_uuid_t* dst,
    uint32_t edge_type
) {
    if (!topo || !src || !dst) return -1;

    for (size_t i = 0; i < topo->edge_count; i++) {
        if (topo->edges[i].active &&
            (edge_type == 0 || topo->edges[i].edge_type == edge_type) &&
            keystone_uuid_equal(&topo->edges[i].src, src) &&
            keystone_uuid_equal(&topo->edges[i].dst, dst)) {
            topo->edges[i].active = 0;
            return 0;
        }
    }
    return -1;
}

size_t keystone_topology_find_neighbors(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* id,
    uint32_t edge_type_mask,
    keystone_uuid_t* out_neighbors,
    size_t max_results
) {
    if (!topo || !id || !out_neighbors || max_results == 0) return 0;

    size_t found = 0;
    for (size_t i = 0; i < topo->edge_count && found < max_results; i++) {
        if (!topo->edges[i].active) continue;

        if (keystone_uuid_equal(&topo->edges[i].src, id) && (topo->edges[i].edge_type & edge_type_mask)) {
            out_neighbors[found++] = topo->edges[i].dst;
        }
    }
    return found;
}

size_t keystone_topology_find_failure_domain(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* id,
    keystone_uuid_t* out_members,
    size_t max_results
) {
    if (!topo || !id || !out_members || max_results == 0) return 0;

    /* 1. If 'id' is a node, look up its failure_domain_id */
    keystone_node_metrics_t nm;
    if (keystone_topology_get_node(topo, id, &nm) == 0) {
        size_t found = 0;
        for (size_t i = 0; i < topo->node_capacity && found < max_results; i++) {
            if (topo->node_slots[i].occupied) {
                if (topo->node_slots[i].metrics.failure_domain_id == nm.failure_domain_id) {
                    out_members[found++] = topo->node_slots[i].metrics.node_id;
                }
            }
        }
        return found;
    }

    /* 2. Alternatively, check KEYSTONE_EDGE_SHARES_FAILURE_DOMAIN edges */
    return keystone_topology_find_neighbors(topo, id, KEYSTONE_EDGE_SHARES_FAILURE_DOMAIN, out_members, max_results);
}

size_t keystone_topology_find_dependency_chain(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* id,
    keystone_uuid_t* out_chain,
    size_t max_results
) {
    if (!topo || !id || !out_chain || max_results == 0) return 0;

    size_t found = 0;
    keystone_uuid_t current = *id;

    /* Traverse dependencies iteratively */
    for (size_t depth = 0; depth < max_results; depth++) {
        keystone_uuid_t next_dep;
        size_t n = keystone_topology_find_neighbors(topo, &current, KEYSTONE_EDGE_DEPENDS_ON, &next_dep, 1);
        if (n == 0) break;

        out_chain[found++] = next_dep;
        current = next_dep;
    }

    return found;
}

size_t keystone_topology_find_affected_resources(
    const keystone_topology_graph_t* topo,
    const keystone_uuid_t* node_id,
    keystone_uuid_t* out_affected,
    size_t max_results
) {
    if (!topo || !node_id || !out_affected || max_results == 0) return 0;

    /* Find all resources with RUNS_ON pointing to node_id */
    size_t found = 0;
    for (size_t i = 0; i < topo->edge_count && found < max_results; i++) {
        if (!topo->edges[i].active) continue;

        if (topo->edges[i].edge_type == KEYSTONE_EDGE_RUNS_ON &&
            keystone_uuid_equal(&topo->edges[i].dst, node_id)) {
            out_affected[found++] = topo->edges[i].src;
        }
    }
    return found;
}

size_t keystone_topology_list_nodes(
    const keystone_topology_graph_t* topo,
    keystone_node_metrics_t* out_nodes,
    size_t max_nodes
) {
    if (!topo || !out_nodes || max_nodes == 0) return 0;

    size_t written = 0;
    for (size_t i = 0; i < topo->node_capacity && written < max_nodes; i++) {
        if (topo->node_slots[i].occupied) {
            out_nodes[written++] = topo->node_slots[i].metrics;
        }
    }
    return written;
}

size_t keystone_topology_node_count(const keystone_topology_graph_t* topo) {
    return topo ? topo->node_count : 0;
}

size_t keystone_topology_edge_count(const keystone_topology_graph_t* topo) {
    if (!topo) return 0;
    size_t active = 0;
    for (size_t i = 0; i < topo->edge_count; i++) {
        if (topo->edges[i].active) active++;
    }
    return active;
}
