/*
 * KEYSTONE Two-Tier Hybrid Query Planner & Recommendation Implementation
 *
 * Implements strict two-tier candidate evaluation:
 *   Tier 1 (Hard Constraints): Security clearance, CPU ISA flags, RAM, failure domains -> Boolean pruning
 *   Tier 2 (Soft Objectives): Thermal headroom, NUMA fit, storage locality, load -> Weighted ranking
 * Emits explainable recommendation bundles with audit-ready provenance.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_hybrid.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

keystone_placement_query_t keystone_placement_query_default(void) {
    keystone_placement_query_t q;
    memset(&q, 0, sizeof(q));
    q.min_classification = KEYSTONE_CLASSIFICATION_UNCLASSIFIED;
    q.min_ram_available_mb = 1024;
    q.min_cpu_cores = 1;

    q.weight_ram_headroom = 0.35f;
    q.weight_thermal_headroom = 0.25f;
    q.weight_cpu_headroom = 0.25f;
    q.weight_numa_locality = 0.15f;
    return q;
}

int keystone_hybrid_evaluate_candidate(
    const keystone_node_metrics_t* node,
    const keystone_placement_query_t* query,
    const keystone_topology_graph_t* topo,
    keystone_explain_t* out_explain,
    double* out_score
) {
    if (!node || !query || !out_explain || !out_score) return -1;

    keystone_explain_init(out_explain, &node->node_id, KEYSTONE_OBJ_NODE);
    int all_hard_satisfied = 1;

    /* 1. Hard Constraint: Security Classification */
    int class_ok = (node->classification >= query->min_classification);
    char class_rat[128];
    snprintf(class_rat, sizeof(class_rat), "Host class %u %s required %u",
             node->classification, class_ok ? ">=" : "<", query->min_classification);
    keystone_explain_add_constraint(out_explain, "SECURITY_CLASSIFICATION", class_ok, class_rat);
    if (!class_ok) all_hard_satisfied = 0;

    /* 2. Hard Constraint: Security Compartments */
    int comp_ok = ((query->required_compartments & ~node->compartment_mask) == 0);
    char comp_rat[128];
    snprintf(comp_rat, sizeof(comp_rat), "Compartments 0x%llx %s 0x%llx",
             (unsigned long long)node->compartment_mask,
             comp_ok ? "satisfies" : "missing",
             (unsigned long long)query->required_compartments);
    keystone_explain_add_constraint(out_explain, "SECURITY_COMPARTMENTS", comp_ok, comp_rat);
    if (!comp_ok) all_hard_satisfied = 0;

    /* 3. Hard Constraint: CPU ISA Features */
    int isa_ok = ((query->required_isa_features & ~node->isa_features) == 0);
    char isa_rat[128];
    snprintf(isa_rat, sizeof(isa_rat), "ISA flags 0x%x %s required 0x%x",
             node->isa_features, isa_ok ? "satisfies" : "missing", query->required_isa_features);
    keystone_explain_add_constraint(out_explain, "CPU_ISA_FEATURES", isa_ok, isa_rat);
    if (!isa_ok) all_hard_satisfied = 0;

    /* 4. Hard Constraint: Minimum Available RAM */
    int ram_ok = (node->ram_available_mb >= query->min_ram_available_mb);
    char ram_rat[128];
    snprintf(ram_rat, sizeof(ram_rat), "Free RAM %llu MB %s %llu MB",
             (unsigned long long)node->ram_available_mb, ram_ok ? ">=" : "<",
             (unsigned long long)query->min_ram_available_mb);
    keystone_explain_add_constraint(out_explain, "MIN_RAM_AVAILABLE", ram_ok, ram_rat);
    if (!ram_ok) all_hard_satisfied = 0;

    /* 5. Hard Constraint: Minimum CPU Cores */
    int cores_ok = (node->cpu_cores >= query->min_cpu_cores);
    char core_rat[128];
    snprintf(core_rat, sizeof(core_rat), "Cores %u %s %u",
             node->cpu_cores, cores_ok ? ">=" : "<", query->min_cpu_cores);
    keystone_explain_add_constraint(out_explain, "MIN_CPU_CORES", cores_ok, core_rat);
    if (!cores_ok) all_hard_satisfied = 0;

    /* 6. Hard Constraint: Failure Domain Anti-Affinity */
    if (topo && !keystone_uuid_is_nil(&query->anti_affinity_resource)) {
        keystone_node_metrics_t anti_node;
        int anti_found = 0;

        /* Check if anti_affinity_resource is a node directly or a VM with RUNS_ON edge */
        if (keystone_topology_get_node(topo, &query->anti_affinity_resource, &anti_node) == 0) {
            anti_found = 1;
        } else {
            keystone_uuid_t host_id;
            if (keystone_topology_find_neighbors(topo, &query->anti_affinity_resource, KEYSTONE_EDGE_RUNS_ON, &host_id, 1) > 0) {
                if (keystone_topology_get_node(topo, &host_id, &anti_node) == 0) {
                    anti_found = 1;
                }
            }
        }

        if (anti_found) {
            int domain_ok = (node->failure_domain_id != anti_node.failure_domain_id);
            char dom_rat[128];
            snprintf(dom_rat, sizeof(dom_rat), "Host domain %u %s replica domain %u",
                     node->failure_domain_id, domain_ok ? "!=" : "==", anti_node.failure_domain_id);
            keystone_explain_add_constraint(out_explain, "FAILURE_DOMAIN_ISOLATION", domain_ok, dom_rat);
            if (!domain_ok) all_hard_satisfied = 0;
        }
    }

    if (!all_hard_satisfied) {
        *out_score = 0.0;
        keystone_explain_set_ranking(out_explain, 0.0, "DISQUALIFIED: Failed one or more hard constraints");
        return 0; /* Evaluated, pruned */
    }

    /* Tier 2: Soft Objectives Evaluation (0.0 to 1.0) */
    double ram_ratio = (node->ram_total_mb > 0)
        ? (double)node->ram_available_mb / (double)node->ram_total_mb : 0.5;

    double temp_c = (double)node->temperature_c;
    if (temp_c < 30.0) temp_c = 30.0;
    if (temp_c > 90.0) temp_c = 90.0;
    double thermal_score = (90.0 - temp_c) / 60.0; /* 30C = 1.0, 90C = 0.0 */

    double cpu_usage = (double)node->cpu_usage_pct;
    if (cpu_usage < 0.0) cpu_usage = 0.0;
    if (cpu_usage > 100.0) cpu_usage = 100.0;
    double cpu_score = (100.0 - cpu_usage) / 100.0;

    double numa_score = (node->numa_nodes <= 1) ? 1.0 : (node->numa_nodes == 2 ? 0.85 : 0.70);

    double total_weight = query->weight_ram_headroom + query->weight_thermal_headroom +
                          query->weight_cpu_headroom + query->weight_numa_locality;
    if (total_weight <= 0.0) total_weight = 1.0;

    double composite = (ram_ratio * query->weight_ram_headroom +
                        thermal_score * query->weight_thermal_headroom +
                        cpu_score * query->weight_cpu_headroom +
                        numa_score * query->weight_numa_locality) / total_weight;

    *out_score = composite;

    char notes[256];
    snprintf(notes, sizeof(notes),
             "Score: %.3f (RAM=%.2f, Temp=%.2f, CPU=%.2f, NUMA=%.2f)",
             composite, ram_ratio, thermal_score, cpu_score, numa_score);
    keystone_explain_set_ranking(out_explain, composite, notes);

    return 1; /* Passed hard constraints and ranked */
}

typedef struct {
    keystone_recommendation_t rec;
} candidate_sort_item_t;

static int compare_candidate_scores(const void* a, const void* b) {
    const candidate_sort_item_t* ca = (const candidate_sort_item_t*)a;
    const candidate_sort_item_t* cb = (const candidate_sort_item_t*)b;
    if (ca->rec.confidence_score > cb->rec.confidence_score) return -1;
    if (ca->rec.confidence_score < cb->rec.confidence_score) return 1;
    return 0;
}

size_t keystone_hybrid_plan_placement(
    const keystone_topology_graph_t* topo,
    const keystone_placement_query_t* query,
    uint64_t current_generation,
    const keystone_hlc_t* current_hlc,
    keystone_recommendation_t* out_recommendations,
    size_t max_recommendations
) {
    if (!topo || !query || !out_recommendations || max_recommendations == 0) return 0;

    size_t total_nodes = keystone_topology_node_count(topo);
    if (total_nodes == 0) return 0;

    keystone_node_metrics_t* all_nodes = (keystone_node_metrics_t*)malloc(total_nodes * sizeof(keystone_node_metrics_t));
    if (!all_nodes) return 0;

    size_t count = keystone_topology_list_nodes(topo, all_nodes, total_nodes);

    candidate_sort_item_t* candidates = (candidate_sort_item_t*)malloc(count * sizeof(candidate_sort_item_t));
    if (!candidates) {
        free(all_nodes);
        return 0;
    }

    size_t qualified_count = 0;
    for (size_t i = 0; i < count; i++) {
        keystone_explain_t explain;
        double score = 0.0;
        int pass = keystone_hybrid_evaluate_candidate(&all_nodes[i], query, topo, &explain, &score);
        if (pass && score > 0.0) {
            candidates[qualified_count].rec.recommended_target_id = all_nodes[i].node_id;
            candidates[qualified_count].rec.target_type = KEYSTONE_OBJ_NODE;
            candidates[qualified_count].rec.confidence_score = score;
            candidates[qualified_count].rec.based_on_generation = current_generation;
            if (current_hlc) {
                candidates[qualified_count].rec.based_on_hlc = *current_hlc;
            } else {
                memset(&candidates[qualified_count].rec.based_on_hlc, 0, sizeof(keystone_hlc_t));
            }
            candidates[qualified_count].rec.evidence = explain;
            qualified_count++;
        }
    }

    free(all_nodes);

    if (qualified_count > 1) {
        qsort(candidates, qualified_count, sizeof(candidate_sort_item_t), compare_candidate_scores);
    }

    size_t return_count = qualified_count < max_recommendations ? qualified_count : max_recommendations;
    for (size_t i = 0; i < return_count; i++) {
        out_recommendations[i] = candidates[i].rec;
    }

    free(candidates);
    return return_count;
}
