/*
 * KEYSTONE Federated Distributed Query & AI/RAG Context Retrieval Test Suite
 *
 * Validates:
 *   1. Federated query coordinator node registry and health tracking.
 *   2. Conflict resolution across nodes (fencing epoch > generation > HLC).
 *   3. Monotonic multi-way timeline merging and deduplication.
 *   4. Resilient top-K incident similarity merging with partial-result tracking.
 *   5. Structured AI/RAG context pack extraction with explicit citations.
 *   6. Security-aware context isolation (classification and compartment checks).
 *   7. Model governance registration, task validation, and cryptographic hash verification.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_federated_node_registry_and_conflict_resolution(void) {
    printf("--- Testing Federated Coordinator & Conflict Resolution ---\n");

    keystone_uuid_t local_id;
    keystone_uuid_from_string("11111111-1111-1111-1111-111111111111", &local_id);

    keystone_federated_coordinator_t* coord = keystone_federated_coordinator_create(&local_id, 10);
    TEST_ASSERT(coord != NULL);

    /* Register remote nodes */
    keystone_remote_node_t r1, r2;
    memset(&r1, 0, sizeof(r1));
    keystone_uuid_from_string("22222222-2222-2222-2222-222222222222", &r1.node_id);
    r1.site_id = 10;
    r1.health = KEYSTONE_NODE_HEALTHY;
    strncpy(r1.endpoint, "unix:///run/keystoned_site10.sock", sizeof(r1.endpoint) - 1);
    TEST_ASSERT(keystone_federated_register_node(coord, &r1) == 0);

    memset(&r2, 0, sizeof(r2));
    keystone_uuid_from_string("33333333-3333-3333-3333-333333333333", &r2.node_id);
    r2.site_id = 20;
    r2.health = KEYSTONE_NODE_HEALTHY;
    strncpy(r2.endpoint, "unix:///run/keystoned_site20.sock", sizeof(r2.endpoint) - 1);
    TEST_ASSERT(keystone_federated_register_node(coord, &r2) == 0);

    TEST_ASSERT(keystone_federated_node_count(coord) == 2);

    /* Update node health to degraded */
    keystone_hlc_t hb = keystone_hlc_now(2);
    int rc = keystone_federated_update_node_health(coord, &r2.node_id, KEYSTONE_NODE_DEGRADED, 45, &hb);
    TEST_ASSERT(rc == 0);

    /* Test Conflict Resolution across 3 nodes for an exact resource ID */
    keystone_exact_entry_t cands[3];
    memset(cands, 0, sizeof(cands));

    /* Node A: Fencing Epoch 1, Generation 100 */
    cands[0].fencing_epoch = 1;
    cands[0].latest_generation = 100;
    cands[0].latest_hlc.physical_ms = 1000;

    /* Node B: Fencing Epoch 2, Generation 50 (Higher epoch beats generation 100!) */
    cands[1].fencing_epoch = 2;
    cands[1].latest_generation = 50;
    cands[1].latest_hlc.physical_ms = 500;

    /* Node C: Fencing Epoch 2, Generation 52 (Same epoch 2, but generation 52 > 50!) */
    cands[2].fencing_epoch = 2;
    cands[2].latest_generation = 52;
    cands[2].latest_hlc.physical_ms = 600;

    keystone_exact_entry_t winner;
    rc = keystone_federated_merge_exact_identity(cands, 3, &winner);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(winner.fencing_epoch == 2);
    TEST_ASSERT(winner.latest_generation == 52);
    TEST_ASSERT(winner.latest_hlc.physical_ms == 600);
    printf("  [PASS] Conflict resolution selected freshest epoch and generation (epoch=2, gen=52)\n");

    keystone_federated_coordinator_destroy(coord);
}

static void test_federated_timeline_and_topk_merging(void) {
    printf("--- Testing Federated Timeline & Top-K Merging with Partial Degradation ---\n");

    /* Stream 1 from Node 1 (3 events) */
    keystone_temporal_entry_t n1_events[3];
    memset(n1_events, 0, sizeof(n1_events));
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000001", &n1_events[0].event_id);
    n1_events[0].hlc.physical_ms = 1000;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000002", &n1_events[1].event_id);
    n1_events[1].hlc.physical_ms = 3000;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000003", &n1_events[2].event_id);
    n1_events[2].hlc.physical_ms = 5000;

    /* Stream 2 from Node 2 (3 events with an overlapping event and interleaved timestamps) */
    keystone_temporal_entry_t n2_events[3];
    memset(n2_events, 0, sizeof(n2_events));
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000002", &n2_events[0].event_id); /* Duplicate of n1[1] */
    n2_events[0].hlc.physical_ms = 3000;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000004", &n2_events[1].event_id);
    n2_events[1].hlc.physical_ms = 2000;
    keystone_uuid_from_string("aaaaaaaa-0000-0000-0000-000000000005", &n2_events[2].event_id);
    n2_events[2].hlc.physical_ms = 4000;

    const keystone_temporal_entry_t* streams[3] = { n1_events, n2_events, NULL }; /* Node 3 timed out (NULL) */
    size_t stream_counts[3] = { 3, 3, 0 };

    keystone_temporal_entry_t merged_events[8];
    keystone_partial_status_t p_status;

    size_t merged_count = keystone_federated_merge_timelines(streams, stream_counts, 3, merged_events, 8, &p_status);

    /* 6 total items - 1 duplicate = 5 unique events */
    TEST_ASSERT(merged_count == 5);
    TEST_ASSERT(p_status.is_partial == true);
    TEST_ASSERT(p_status.nodes_queried == 3);
    TEST_ASSERT(p_status.nodes_responded == 2);

    /* Verify strict monotonic HLC ordering: 1000, 2000, 3000, 4000, 5000 */
    for (size_t i = 1; i < merged_count; i++) {
        TEST_ASSERT(keystone_hlc_compare(&merged_events[i - 1].hlc, &merged_events[i].hlc) < 0);
    }
    printf("  [PASS] Federated timeline multi-way merge deduplicated and sorted 5 events (partial=true)\n");

    /* Test Top-K Incident Similarity Merging */
    keystone_incident_match_t n1_matches[2];
    memset(n1_matches, 0, sizeof(n1_matches));
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000001", &n1_matches[0].incident.incident_id);
    n1_matches[0].similarity_score = 0.95f;
    n1_matches[0].incident.index_generation = 10;
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000002", &n1_matches[1].incident.incident_id);
    n1_matches[1].similarity_score = 0.80f;
    n1_matches[1].incident.index_generation = 12;

    keystone_incident_match_t n2_matches[2];
    memset(n2_matches, 0, sizeof(n2_matches));
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000001", &n2_matches[0].incident.incident_id); /* Duplicate */
    n2_matches[0].similarity_score = 0.95f;
    n2_matches[0].incident.index_generation = 10;
    keystone_uuid_from_string("bbbbbbbb-0000-0000-0000-000000000003", &n2_matches[1].incident.incident_id);
    n2_matches[1].similarity_score = 0.99f; /* Highest similarity */
    n2_matches[1].incident.index_generation = 15;

    const keystone_incident_match_t* inc_streams[2] = { n1_matches, n2_matches };
    size_t inc_counts[2] = { 2, 2 };

    keystone_incident_match_t merged_incidents[4];
    keystone_partial_status_t inc_p_status;
    size_t merged_inc_count = keystone_federated_merge_topk_incidents(inc_streams, inc_counts, 2, merged_incidents, 4, &inc_p_status);

    TEST_ASSERT(merged_inc_count == 3);
    TEST_ASSERT(inc_p_status.is_partial == false);
    TEST_ASSERT(inc_p_status.freshest_generation == 15);
    TEST_ASSERT(merged_incidents[0].similarity_score == 0.99f);
    TEST_ASSERT(merged_incidents[1].similarity_score == 0.95f);
    TEST_ASSERT(merged_incidents[2].similarity_score == 0.80f);
    printf("  [PASS] Federated top-k similarity merge deduplicated and ranked matches (top score=0.99)\n");
}

static void test_rag_context_retrieval_and_model_governance(void) {
    printf("--- Testing Structured AI/RAG Context Retrieval & Model Governance ---\n");

    /* 1. Setup indices */
    keystone_topology_graph_t* topo = keystone_topology_create(16, 32);
    TEST_ASSERT(topo != NULL);

    keystone_temporal_index_t* temporal = keystone_temporal_index_create(64);
    TEST_ASSERT(temporal != NULL);

    keystone_telemetry_engine_t* telemetry = keystone_telemetry_create(256, 32, 1);
    TEST_ASSERT(telemetry != NULL);

    keystone_incident_engine_t* incident = keystone_incident_engine_create(32, 1);
    TEST_ASSERT(incident != NULL);

    keystone_uuid_t node_id;
    keystone_uuid_from_string("44444444-4444-4444-4444-444444444444", &node_id);

    /* Populate Topology Node */
    keystone_node_metrics_t nm;
    memset(&nm, 0, sizeof(nm));
    nm.node_id = node_id;
    nm.classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    nm.compartment_mask = 0x05;
    nm.ram_total_mb = 65536;
    nm.ram_available_mb = 32768;
    nm.cpu_cores = 32;
    TEST_ASSERT(keystone_topology_upsert_node(topo, &nm) == 0);

    /* Add Neighbor */
    keystone_uuid_t vm_id;
    keystone_uuid_from_string("55555555-5555-5555-5555-555555555555", &vm_id);
    TEST_ASSERT(keystone_topology_add_edge(topo, &vm_id, &node_id, KEYSTONE_EDGE_RUNS_ON) == 0);

    /* Add Timeline Event */
    keystone_federation_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.source_object_id = node_id;
    keystone_uuid_from_string("66666666-6666-6666-6666-666666666666", &rec.source_event_id);
    rec.source_hlc = keystone_hlc_now(1);
    rec.classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    TEST_ASSERT(keystone_temporal_index_ingest_record(temporal, &rec, 0) == KEYSTONE_INGEST_OK);

    /* Add Telemetry Sample & Anomaly */
    keystone_telemetry_sample_t s;
    memset(&s, 0, sizeof(s));
    s.node_id = node_id;
    s.metric_type = KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS;
    s.timestamp_hlc = keystone_hlc_now(1);
    s.value = 92.0; /* Exceeds critical threshold 90.0 */

    keystone_anomaly_t anom;
    bool is_anom = false;
    TEST_ASSERT(keystone_telemetry_ingest_sample(telemetry, &s, &anom, &is_anom) == 0);
    TEST_ASSERT(is_anom);

    /* Add Historical Incident */
    keystone_incident_record_t inc;
    memset(&inc, 0, sizeof(inc));
    keystone_uuid_from_string("77777777-7777-7777-7777-777777777777", &inc.incident_id);
    strncpy(inc.root_cause_label, "THERMAL_SHUTDOWN", sizeof(inc.root_cause_label) - 1);
    strncpy(inc.resolution_notes, "Inspect server fans and rack airflow", sizeof(inc.resolution_notes) - 1);
    inc.embedding[3] = 0.95f; /* High temp */
    TEST_ASSERT(keystone_incident_register(incident, &inc) == 0);

    /* Create RAG Engine */
    keystone_rag_engine_t* rag = keystone_rag_create(topo, temporal, telemetry, incident, 77);
    TEST_ASSERT(rag != NULL);

    /* Test 1: Security Partitioning (Denial on lower classification) */
    keystone_security_context_t caller_sec;
    memset(&caller_sec, 0, sizeof(caller_sec));
    caller_sec.classification = KEYSTONE_CLASSIFICATION_UNCLASSIFIED; /* Lower than RESTRICTED */
    caller_sec.compartment_mask = 0x05;

    keystone_context_pack_t pack;
    int rc = keystone_rag_query_context(rag, &node_id, &caller_sec, 3600000ULL, &pack);
    TEST_ASSERT(rc == -2); /* Access Denied */
    printf("  [PASS] Unclassified caller denied access to Restricted context pack\n");

    /* Test 2: Security Clearance Granted */
    caller_sec.classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    caller_sec.compartment_mask = 0x05;
    rc = keystone_rag_query_context(rag, &node_id, &caller_sec, 3600000ULL, &pack);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(pack.classification == KEYSTONE_CLASSIFICATION_RESTRICTED);
    TEST_ASSERT(pack.event_count >= 1);
    TEST_ASSERT(pack.anomaly_count >= 1);
    TEST_ASSERT(pack.incident_count >= 1);
    TEST_ASSERT(pack.citation_count >= 3);
    printf("  [PASS] Context pack retrieved: %u events, %u anomalies, %u incidents, %u citations\n",
           pack.event_count, pack.anomaly_count, pack.incident_count, pack.citation_count);

    /* Test 3: Model Governance Registry & SHA-256 Validation */
    keystone_model_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    keystone_uuid_from_string("88888888-8888-8888-8888-888888888888", &manifest.model_id);
    strncpy(manifest.model_name, "keystone-remediation-v2", sizeof(manifest.model_name) - 1);
    manifest.model_version = 2;
    manifest.feature_schema_version = 1;
    manifest.supported_tasks = KEYSTONE_TASK_SIMILARITY | KEYSTONE_TASK_RAG;
    memset(manifest.weights_sha256, 0xAB, 32); /* Dummy SHA-256 */
    manifest.validation_accuracy = 0.985;
    manifest.validation_p95_latency_ns = 1420.0;

    TEST_ASSERT(keystone_model_governance_register(rag, &manifest) == 0);
    TEST_ASSERT(keystone_model_governance_count(rag) == 1);

    /* Validate model with authentic weights */
    uint8_t authentic_sha[32];
    memset(authentic_sha, 0xAB, 32);
    rc = keystone_model_governance_validate(rag, &manifest.model_id, KEYSTONE_TASK_SIMILARITY, authentic_sha);
    TEST_ASSERT(rc == 0);

    /* Validate model with tampered weights */
    uint8_t tampered_sha[32];
    memset(tampered_sha, 0xFF, 32);
    rc = keystone_model_governance_validate(rag, &manifest.model_id, KEYSTONE_TASK_SIMILARITY, tampered_sha);
    TEST_ASSERT(rc == -3); /* SHA mismatch */

    /* Validate model for unsupported task (PLACEMENT) */
    rc = keystone_model_governance_validate(rag, &manifest.model_id, KEYSTONE_TASK_PLACEMENT, authentic_sha);
    TEST_ASSERT(rc == -2); /* Task not supported */

    printf("  [PASS] Model governance validated authentic weights, caught tampering, and checked task permissions\n");

    keystone_rag_destroy(rag);
    keystone_incident_engine_destroy(incident);
    keystone_telemetry_destroy(telemetry);
    keystone_temporal_index_destroy(temporal);
    keystone_topology_destroy(topo);
}

int main(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    printf("=====================================================\n");
    printf("KEYSTONE Federated Query & AI/RAG Context Test Suite\n");
    printf("=====================================================\n");

    test_federated_node_registry_and_conflict_resolution();
    test_federated_timeline_and_topk_merging();
    test_rag_context_retrieval_and_model_governance();

    printf("\nAll Federated Query and AI/RAG Context tests passed!\n");
    return 0;
}
