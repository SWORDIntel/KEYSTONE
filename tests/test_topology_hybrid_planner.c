/*
 * KEYSTONE Topology Graph & Two-Tier Hybrid Query Planner Tests
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "keystone_topology.h"
#include "keystone_hybrid.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_topology_graph_operations(void) {
    printf("[*] Testing topology graph adjacency and expansion...\n");

    keystone_topology_graph_t* topo = keystone_topology_create(32, 64);
    TEST_ASSERT(topo != NULL);

    keystone_uuid_t node1, node2, node3;
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000001", &node1);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000002", &node2);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000003", &node3);

    keystone_node_metrics_t nm1 = {
        .node_id = node1, .isa_features = KEYSTONE_CPU_AVX2 | KEYSTONE_CPU_AVX512 | KEYSTONE_CPU_AMX,
        .ram_total_mb = 131072, .ram_available_mb = 122880, .cpu_cores = 64,
        .cpu_usage_pct = 15.0f, .temperature_c = 42.0f, .numa_nodes = 1,
        .failure_domain_id = 101, .classification = KEYSTONE_CLASSIFICATION_SECRET
    };
    keystone_node_metrics_t nm2 = {
        .node_id = node2, .isa_features = KEYSTONE_CPU_AVX2,
        .ram_total_mb = 32768, .ram_available_mb = 8192, .cpu_cores = 16,
        .cpu_usage_pct = 75.0f, .temperature_c = 72.0f, .numa_nodes = 1,
        .failure_domain_id = 101, .classification = KEYSTONE_CLASSIFICATION_OPS
    };
    keystone_node_metrics_t nm3 = {
        .node_id = node3, .isa_features = KEYSTONE_CPU_AVX2 | KEYSTONE_CPU_AVX512,
        .ram_total_mb = 65536, .ram_available_mb = 51200, .cpu_cores = 32,
        .cpu_usage_pct = 25.0f, .temperature_c = 48.0f, .numa_nodes = 2,
        .failure_domain_id = 102, .classification = KEYSTONE_CLASSIFICATION_RESTRICTED
    };

    TEST_ASSERT(keystone_topology_upsert_node(topo, &nm1) == 0);
    TEST_ASSERT(keystone_topology_upsert_node(topo, &nm2) == 0);
    TEST_ASSERT(keystone_topology_upsert_node(topo, &nm3) == 0);
    TEST_ASSERT(keystone_topology_node_count(topo) == 3);

    /* Test Node Lookup */
    keystone_node_metrics_t out_nm;
    TEST_ASSERT(keystone_topology_get_node(topo, &node1, &out_nm) == 0);
    TEST_ASSERT(out_nm.ram_available_mb == 122880);
    TEST_ASSERT(out_nm.failure_domain_id == 101);

    /* Edges: VM-1 runs on Node-1; Volume-1 attached to VM-1 */
    keystone_uuid_t vm1, vol1;
    keystone_uuid_from_string("11111111-0000-0000-0000-000000000001", &vm1);
    keystone_uuid_from_string("22222222-0000-0000-0000-000000000001", &vol1);

    TEST_ASSERT(keystone_topology_add_edge(topo, &vm1, &node1, KEYSTONE_EDGE_RUNS_ON) == 0);
    TEST_ASSERT(keystone_topology_add_edge(topo, &vol1, &vm1, KEYSTONE_EDGE_ATTACHED_TO) == 0);
    TEST_ASSERT(keystone_topology_edge_count(topo) == 2);

    /* Test Neighborhood */
    keystone_uuid_t neighbors[4];
    size_t nn = keystone_topology_find_neighbors(topo, &vm1, KEYSTONE_EDGE_RUNS_ON, neighbors, 4);
    TEST_ASSERT(nn == 1);
    TEST_ASSERT(keystone_uuid_equal(&neighbors[0], &node1));

    /* Test Affected Resources (what runs on Node-1) */
    keystone_uuid_t affected[4];
    size_t na = keystone_topology_find_affected_resources(topo, &node1, affected, 4);
    TEST_ASSERT(na == 1);
    TEST_ASSERT(keystone_uuid_equal(&affected[0], &vm1));

    /* Test Failure Domain Expansion (Rack 101 members: Node 1 and Node 2) */
    keystone_uuid_t dom_members[4];
    size_t nd = keystone_topology_find_failure_domain(topo, &node1, dom_members, 4);
    TEST_ASSERT(nd == 2);

    /* Test Dependency Chains: SvcA -> SvcB -> DB1 */
    keystone_uuid_t svcA, svcB, db1;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000001", &svcA);
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000001", &svcB);
    keystone_uuid_from_string("cccccccc-0000-0000-0000-000000000001", &db1);

    keystone_topology_add_edge(topo, &svcA, &svcB, KEYSTONE_EDGE_DEPENDS_ON);
    keystone_topology_add_edge(topo, &svcB, &db1, KEYSTONE_EDGE_DEPENDS_ON);

    keystone_uuid_t chain[4];
    size_t nc = keystone_topology_find_dependency_chain(topo, &svcA, chain, 4);
    TEST_ASSERT(nc == 2);
    TEST_ASSERT(keystone_uuid_equal(&chain[0], &svcB));
    TEST_ASSERT(keystone_uuid_equal(&chain[1], &db1));

    keystone_topology_destroy(topo);
    printf("    [+] Topology graph primitives and traversals verified.\n");
}

static void test_hybrid_query_planner(void) {
    printf("[*] Testing two-tier hybrid query planner and recommendation engine...\n");

    keystone_topology_graph_t* topo = keystone_topology_create(32, 64);
    TEST_ASSERT(topo != NULL);

    keystone_uuid_t n1, n2, n3;
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000001", &n1);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000002", &n2);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000003", &n3);

    /* Host 1: 120GB Free RAM, AVX-512 + AMX, 42C temp, Rack 101, SECRET */
    keystone_node_metrics_t nm1 = {
        .node_id = n1, .isa_features = KEYSTONE_CPU_AVX2 | KEYSTONE_CPU_AVX512 | KEYSTONE_CPU_AMX,
        .ram_total_mb = 131072, .ram_available_mb = 122880, .cpu_cores = 64,
        .cpu_usage_pct = 15.0f, .temperature_c = 42.0f, .numa_nodes = 1,
        .failure_domain_id = 101, .classification = KEYSTONE_CLASSIFICATION_SECRET
    };
    /* Host 2: 8GB Free RAM, AVX2 only, 72C temp, Rack 101, OPS */
    keystone_node_metrics_t nm2 = {
        .node_id = n2, .isa_features = KEYSTONE_CPU_AVX2,
        .ram_total_mb = 32768, .ram_available_mb = 8192, .cpu_cores = 16,
        .cpu_usage_pct = 75.0f, .temperature_c = 72.0f, .numa_nodes = 1,
        .failure_domain_id = 101, .classification = KEYSTONE_CLASSIFICATION_OPS
    };
    /* Host 3: 50GB Free RAM, AVX-512, 48C temp, Rack 102, RESTRICTED */
    keystone_node_metrics_t nm3 = {
        .node_id = n3, .isa_features = KEYSTONE_CPU_AVX2 | KEYSTONE_CPU_AVX512,
        .ram_total_mb = 65536, .ram_available_mb = 51200, .cpu_cores = 32,
        .cpu_usage_pct = 25.0f, .temperature_c = 48.0f, .numa_nodes = 2,
        .failure_domain_id = 102, .classification = KEYSTONE_CLASSIFICATION_RESTRICTED
    };

    keystone_topology_upsert_node(topo, &nm1);
    keystone_topology_upsert_node(topo, &nm2);
    keystone_topology_upsert_node(topo, &nm3);

    /* Query 1: Hard Constraints: AVX-512 required, 32GB RAM min, RESTRICTED min */
    keystone_placement_query_t q1 = keystone_placement_query_default();
    q1.min_classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    q1.required_isa_features = KEYSTONE_CPU_AVX512;
    q1.min_ram_available_mb = 32768;

    keystone_recommendation_t recs[4];
    keystone_hlc_t now_hlc = { .physical_ms = 1758333000000ULL, .logical = 1, .node_id = 1 };
    size_t count = keystone_hybrid_plan_placement(topo, &q1, 42, &now_hlc, recs, 4);

    /* Host 2 fails (missing AVX-512, RAM 8GB < 32GB, OPS < RESTRICTED).
     * Host 1 and Host 3 pass.
     * Host 1 has higher RAM and lower temp -> ranked #1. */
    TEST_ASSERT(count == 2);
    TEST_ASSERT(keystone_uuid_equal(&recs[0].recommended_target_id, &n1));
    TEST_ASSERT(keystone_uuid_equal(&recs[1].recommended_target_id, &n3));
    TEST_ASSERT(recs[0].confidence_score > recs[1].confidence_score);

    /* Verify Evidence Bundle on Top Candidate */
    keystone_explain_t* exp = &recs[0].evidence;
    TEST_ASSERT(exp->hard_constraint_count >= 5);
    for (size_t c = 0; c < exp->hard_constraint_count; c++) {
        TEST_ASSERT(exp->hard_constraints[c].satisfied == 1);
    }
    TEST_ASSERT(recs[0].based_on_generation == 42);
    TEST_ASSERT(recs[0].based_on_hlc.physical_ms == 1758333000000ULL);

    /* Query 2: Anti-Affinity Pruning */
    /* Add existing VM on Node 1 */
    keystone_uuid_t replica_vm;
    keystone_uuid_from_string("99999999-0000-0000-0000-000000000001", &replica_vm);
    keystone_topology_add_edge(topo, &replica_vm, &n1, KEYSTONE_EDGE_RUNS_ON);

    keystone_placement_query_t q2 = q1;
    q2.anti_affinity_resource = replica_vm; /* Cannot be in same failure domain as replica_vm (Rack 101) */

    size_t count_anti = keystone_hybrid_plan_placement(topo, &q2, 43, &now_hlc, recs, 4);

    /* Host 1 and Host 2 are in Rack 101 -> disqualified.
     * Only Host 3 survives! */
    TEST_ASSERT(count_anti == 1);
    TEST_ASSERT(keystone_uuid_equal(&recs[0].recommended_target_id, &n3));

    keystone_topology_destroy(topo);
    printf("    [+] Two-tier planner, constraint pruning, and evidence provenance verified.\n");
}

/*
 * Combined retrieval filters (CITADEL brief §13, audit criterion 9): one
 * placement query combining topology + exact + temporal + trigram
 * constraints as Tier-1 filters with explain entries.
 */
static void test_combined_retrieval_filters(void) {
    printf("[*] Testing combined exact+temporal+trigram placement filters...\n");

    keystone_topology_graph_t* topo = keystone_topology_create(8, 8);
    keystone_exact_index_t* exact = keystone_exact_index_create(8);
    keystone_temporal_index_t* temporal = keystone_temporal_index_create(16);
    keystone_trigram_index_t* tri = keystone_trigram_index_create_options(4, KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE);
    TEST_ASSERT(topo && exact && temporal && tri);

    /* Four nodes. Expectations with all filters on:
     *   A: exact-active yes, window event yes, doc matches pattern yes -> PASSES
     *   B: tombstoned in exact -> pruned by exact-active
     *   C: no event in window -> pruned by temporal-window
     *   D: document without the pattern -> pruned by content-match
     */
    keystone_uuid_t na, nb, nc, nd;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000001", &na);
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000002", &nb);
    keystone_uuid_from_string("cccccccc-0000-0000-0000-000000000003", &nc);
    keystone_uuid_from_string("dddddddd-0000-0000-0000-000000000004", &nd);

    keystone_node_metrics_t m;
    memset(&m, 0, sizeof(m));
    m.cpu_cores = 8;
    m.ram_total_mb = 65536;
    m.ram_available_mb = 32768;
    m.isa_features = 0;
    m.classification = KEYSTONE_CLASSIFICATION_OPS;

    m.node_id = na; TEST_ASSERT(keystone_topology_upsert_node(topo, &m) == 0);
    m.node_id = nb; TEST_ASSERT(keystone_topology_upsert_node(topo, &m) == 0);
    m.node_id = nc; TEST_ASSERT(keystone_topology_upsert_node(topo, &m) == 0);
    m.node_id = nd; TEST_ASSERT(keystone_topology_upsert_node(topo, &m) == 0);

    /* Exact: A and B present (B tombstoned); C and D absent. */
    keystone_exact_entry_t e;
    memset(&e, 0, sizeof(e));
    e.resource_id = na; e.latest_generation = 10; e.fencing_epoch = 1;
    TEST_ASSERT(keystone_exact_index_upsert(exact, &e) == 0);
    e.resource_id = nb; e.latest_generation = 11; e.fencing_epoch = 1;
    e.flags = KEYSTONE_RECORD_FLAG_TOMBSTONE;
    TEST_ASSERT(keystone_exact_index_upsert(exact, &e) == 0);

    /* Temporal: A and B have events in the window; C and D do not. */
    for (int obj = 0; obj < 2; obj++) {
        keystone_temporal_entry_t te;
        memset(&te, 0, sizeof(te));
        te.object_id = obj == 0 ? na : nb;
        te.hlc = (keystone_hlc_t){ .physical_ms = 5000100, .logical = 0, .node_id = 1 };
        te.event_type = KEYSTONE_OBJ_NODE;
        te.flags = KEYSTONE_RECORD_FLAG_EVENT;
        te.source_generation = 10 + obj;
        TEST_ASSERT(keystone_temporal_index_append(temporal, &te) == 0);
    }

    /* Trigram: documents NAMED by node uuid strings; content carries the
     * pattern for A, B, C but not D. */
    const keystone_uuid_t* ids[4] = { &na, &nb, &nc, &nd };
    char names[4][37];
    for (int i = 0; i < 4; i++) {
        keystone_uuid_to_string(ids[i], names[i]);
        const char* content = (i < 3) ? "nvme-fabric backend node" : "sata archive node";
        TEST_ASSERT(keystone_trigram_index_add_document(tri, names[i], content, strlen(content), NULL) == KEYSTONE_TRIGRAM_OK);
    }
    TEST_ASSERT(keystone_trigram_index_finalize(tri) == KEYSTONE_TRIGRAM_OK);

    keystone_placement_query_t q = keystone_placement_query_default();
    q.min_classification = KEYSTONE_CLASSIFICATION_OPS;
    q.exact_filter = exact;
    q.temporal_filter = temporal;
    q.temporal_window_min = (keystone_hlc_t){ .physical_ms = 5000000, .logical = 0, .node_id = 0 };
    q.content_filter = tri;
    q.content_pattern = "nvme-fabric";

    keystone_recommendation_t recs[8];
    size_t n = keystone_hybrid_plan_placement(topo, &q, 1, NULL, recs, 8);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(keystone_uuid_equal(&recs[0].recommended_target_id, &na));

    /* Explain carries one entry per combined filter on the passing candidate */
    int seen_exact = 0, seen_temporal = 0, seen_content = 0;
    for (uint32_t i = 0; i < recs[0].evidence.hard_constraint_count; i++) {
        const char* cn = recs[0].evidence.hard_constraints[i].constraint_name;
        if (strcmp(cn, "exact-active") == 0) seen_exact = 1;
        if (strcmp(cn, "temporal-window") == 0) seen_temporal = 1;
        if (strcmp(cn, "content-match") == 0) seen_content = 1;
    }
    TEST_ASSERT(seen_exact && seen_temporal && seen_content);

    /* Filters are individually isolatable: only-exact keeps C and D out but
     * B stays tombstone-pruned; only-content keeps B, C in. */
    keystone_placement_query_t q2 = keystone_placement_query_default();
    q2.min_classification = KEYSTONE_CLASSIFICATION_OPS;
    q2.exact_filter = exact;
    n = keystone_hybrid_plan_placement(topo, &q2, 1, NULL, recs, 8);
    TEST_ASSERT(n == 1); /* only A (B tombstoned, C/D absent) */
    TEST_ASSERT(keystone_uuid_equal(&recs[0].recommended_target_id, &na));

    keystone_placement_query_t q3 = keystone_placement_query_default();
    q3.min_classification = KEYSTONE_CLASSIFICATION_OPS;
    q3.content_filter = tri;
    q3.content_pattern = "nvme-fabric";
    n = keystone_hybrid_plan_placement(topo, &q3, 1, NULL, recs, 8);
    TEST_ASSERT(n == 3); /* A, B, C all have pattern-matching documents */

    keystone_topology_destroy(topo);
    keystone_exact_index_destroy(exact);
    keystone_temporal_index_destroy(temporal);
    keystone_trigram_index_destroy(tri);
    printf("    [+] Combined retrieval filters verified (exact+temporal+trigram as Tier-1 with explain).\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Phase 3: Topology Graph & Hybrid Planner Tests\n");
    printf("=================================================================\n");

    test_topology_graph_operations();
    test_hybrid_query_planner();
    test_combined_retrieval_filters();

    printf("=================================================================\n");
    printf("  ALL PHASE 3 TOPOLOGY & HYBRID PLANNER TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
