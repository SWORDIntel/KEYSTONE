/*
 * KEYSTONE Federated Distributed Query & Partial Result Merging Implementation
 *
 * Implements multi-node query routing, deduplication, conflict resolution via
 * fencing epochs and generations, monotonic HLC timeline multi-way merging,
 * top-k incident similarity aggregation, and partial-result resilience under
 * network partition.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_federated_query.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct keystone_federated_coordinator {
    keystone_uuid_t local_node_id;
    uint32_t local_site_id;
    keystone_remote_node_t nodes[KEYSTONE_MAX_FEDERATED_NODES];
    size_t node_count;
};

keystone_federated_coordinator_t* keystone_federated_coordinator_create(
    const keystone_uuid_t* local_node_id,
    uint32_t local_site_id
) {
    keystone_federated_coordinator_t* coord = (keystone_federated_coordinator_t*)calloc(1, sizeof(keystone_federated_coordinator_t));
    if (!coord) return NULL;

    if (local_node_id) {
        coord->local_node_id = *local_node_id;
    }
    coord->local_site_id = local_site_id;
    coord->node_count = 0;
    return coord;
}

void keystone_federated_coordinator_destroy(
    keystone_federated_coordinator_t* coord
) {
    if (coord) {
        free(coord);
    }
}

int keystone_federated_register_node(
    keystone_federated_coordinator_t* coord,
    const keystone_remote_node_t* node
) {
    if (!coord || !node) return -1;

    /* Check if already registered */
    for (size_t i = 0; i < coord->node_count; i++) {
        if (keystone_uuid_equal(&coord->nodes[i].node_id, &node->node_id)) {
            coord->nodes[i] = *node;
            return 0;
        }
    }

    if (coord->node_count >= KEYSTONE_MAX_FEDERATED_NODES) {
        return -2;
    }

    coord->nodes[coord->node_count++] = *node;
    return 0;
}

int keystone_federated_update_node_health(
    keystone_federated_coordinator_t* coord,
    const keystone_uuid_t* node_id,
    keystone_node_health_t health,
    uint64_t generation,
    const keystone_hlc_t* heartbeat_hlc
) {
    if (!coord || !node_id) return -1;

    for (size_t i = 0; i < coord->node_count; i++) {
        if (keystone_uuid_equal(&coord->nodes[i].node_id, node_id)) {
            coord->nodes[i].health = health;
            coord->nodes[i].latest_index_generation = generation;
            if (heartbeat_hlc) {
                coord->nodes[i].last_heartbeat_hlc = *heartbeat_hlc;
            }
            return 0;
        }
    }
    return -2; /* Node not found */
}

size_t keystone_federated_node_count(
    const keystone_federated_coordinator_t* coord
) {
    return coord ? coord->node_count : 0;
}

int keystone_federated_merge_exact_identity(
    const keystone_exact_entry_t* candidates,
    size_t candidate_count,
    keystone_exact_entry_t* out_winner
) {
    if (!candidates || candidate_count == 0 || !out_winner) return -1;

    const keystone_exact_entry_t* best = &candidates[0];

    for (size_t i = 1; i < candidate_count; i++) {
        const keystone_exact_entry_t* cur = &candidates[i];

        /* Rule 1: Highest fencing epoch always wins */
        if (cur->fencing_epoch > best->fencing_epoch) {
            best = cur;
            continue;
        }
        if (cur->fencing_epoch < best->fencing_epoch) {
            continue;
        }

        /* Rule 2: Highest source generation */
        if (cur->latest_generation > best->latest_generation) {
            best = cur;
            continue;
        }
        if (cur->latest_generation < best->latest_generation) {
            continue;
        }

        /* Rule 3: Monotonic HLC comparison */
        if (keystone_hlc_compare(&cur->latest_hlc, &best->latest_hlc) > 0) {
            best = cur;
            continue;
        }
    }

    *out_winner = *best;
    return 0;
}

static int compare_timeline_records(const void* a, const void* b) {
    const keystone_temporal_entry_t* ra = (const keystone_temporal_entry_t*)a;
    const keystone_temporal_entry_t* rb = (const keystone_temporal_entry_t*)b;
    return keystone_hlc_compare(&ra->hlc, &rb->hlc);
}

size_t keystone_federated_merge_timelines(
    const keystone_temporal_entry_t* const* node_records,
    const size_t* record_counts,
    size_t num_nodes,
    keystone_temporal_entry_t* out_merged,
    size_t max_out,
    keystone_partial_status_t* out_status
) {
    if (!node_records || !record_counts || !out_merged || max_out == 0) return 0;

    size_t total_incoming = 0;
    uint32_t responding = 0;
    for (size_t n = 0; n < num_nodes; n++) {
        if (node_records[n] != NULL && record_counts[n] > 0) {
            total_incoming += record_counts[n];
            responding++;
        }
    }

    if (out_status) {
        out_status->nodes_queried = (uint32_t)num_nodes;
        out_status->nodes_responded = responding;
        out_status->is_partial = (responding < num_nodes);
        out_status->slowest_response_us = 0;
        out_status->freshest_generation = 0;
        memset(&out_status->max_hlc_observed, 0, sizeof(keystone_hlc_t));
    }

    if (total_incoming == 0) return 0;

    keystone_temporal_entry_t* pool = (keystone_temporal_entry_t*)malloc(total_incoming * sizeof(keystone_temporal_entry_t));
    if (!pool) return 0;

    size_t pool_idx = 0;
    for (size_t n = 0; n < num_nodes; n++) {
        if (node_records[n] != NULL && record_counts[n] > 0) {
            memcpy(&pool[pool_idx], node_records[n], record_counts[n] * sizeof(keystone_temporal_entry_t));
            pool_idx += record_counts[n];
        }
    }

    /* Sort all candidate records by ascending HLC */
    qsort(pool, total_incoming, sizeof(keystone_temporal_entry_t), compare_timeline_records);

    /* Deduplicate by event_id */
    size_t merged_count = 0;
    for (size_t i = 0; i < total_incoming && merged_count < max_out; i++) {
        bool dup = false;
        for (size_t j = 0; j < merged_count; j++) {
            if (keystone_uuid_equal(&out_merged[j].event_id, &pool[i].event_id)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out_merged[merged_count++] = pool[i];
            if (out_status && keystone_hlc_compare(&pool[i].hlc, &out_status->max_hlc_observed) > 0) {
                out_status->max_hlc_observed = pool[i].hlc;
            }
        }
    }

    free(pool);
    return merged_count;
}

static int compare_incident_matches(const void* a, const void* b) {
    const keystone_incident_match_t* ma = (const keystone_incident_match_t*)a;
    const keystone_incident_match_t* mb = (const keystone_incident_match_t*)b;
    if (ma->similarity_score > mb->similarity_score) return -1;
    if (ma->similarity_score < mb->similarity_score) return 1;
    return 0;
}

size_t keystone_federated_merge_topk_incidents(
    const keystone_incident_match_t* const* node_matches,
    const size_t* match_counts,
    size_t num_nodes,
    keystone_incident_match_t* out_merged,
    size_t max_out,
    keystone_partial_status_t* out_status
) {
    if (!node_matches || !match_counts || !out_merged || max_out == 0) return 0;

    size_t total_incoming = 0;
    uint32_t responding = 0;
    uint64_t max_gen = 0;

    for (size_t n = 0; n < num_nodes; n++) {
        if (node_matches[n] != NULL && match_counts[n] > 0) {
            total_incoming += match_counts[n];
            responding++;
            for (size_t i = 0; i < match_counts[n]; i++) {
                if (node_matches[n][i].incident.index_generation > max_gen) {
                    max_gen = node_matches[n][i].incident.index_generation;
                }
            }
        }
    }

    if (out_status) {
        out_status->nodes_queried = (uint32_t)num_nodes;
        out_status->nodes_responded = responding;
        out_status->is_partial = (responding < num_nodes);
        out_status->freshest_generation = max_gen;
        memset(&out_status->max_hlc_observed, 0, sizeof(keystone_hlc_t));
    }

    if (total_incoming == 0) return 0;

    keystone_incident_match_t* pool = (keystone_incident_match_t*)malloc(total_incoming * sizeof(keystone_incident_match_t));
    if (!pool) return 0;

    size_t pool_idx = 0;
    for (size_t n = 0; n < num_nodes; n++) {
        if (node_matches[n] != NULL && match_counts[n] > 0) {
            memcpy(&pool[pool_idx], node_matches[n], match_counts[n] * sizeof(keystone_incident_match_t));
            pool_idx += match_counts[n];
        }
    }

    /* Sort by similarity descending */
    qsort(pool, total_incoming, sizeof(keystone_incident_match_t), compare_incident_matches);

    /* Deduplicate by incident_id */
    size_t merged_count = 0;
    for (size_t i = 0; i < total_incoming && merged_count < max_out; i++) {
        bool dup = false;
        for (size_t j = 0; j < merged_count; j++) {
            if (keystone_uuid_equal(&out_merged[j].incident.incident_id, &pool[i].incident.incident_id)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out_merged[merged_count++] = pool[i];
            if (out_status && keystone_hlc_compare(&pool[i].incident.incident_hlc, &out_status->max_hlc_observed) > 0) {
                out_status->max_hlc_observed = pool[i].incident.incident_hlc;
            }
        }
    }

    free(pool);
    return merged_count;
}
