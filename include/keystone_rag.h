/*
 * KEYSTONE Structured AI/RAG Context Retrieval & Model Governance
 *
 * Constrained retrieval backend for operator and AI assistants, delivering
 * structured evidence context packs with strict source citations, freshness
 * metadata, security classification boundaries, and formal model governance.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#ifndef KEYSTONE_RAG_H
#define KEYSTONE_RAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"
#include "keystone_topology.h"
#include "keystone_telemetry.h"
#include "keystone_incident.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYSTONE_MAX_RAG_EVENTS     32
#define KEYSTONE_MAX_RAG_METRICS    16
#define KEYSTONE_MAX_RAG_ANOMALIES  8
#define KEYSTONE_MAX_RAG_INCIDENTS  8
#define KEYSTONE_MAX_RAG_NEIGHBORS  16
#define KEYSTONE_MAX_RAG_CITATIONS  64

/* --- Task Capabilities for Model Governance --- */
#define KEYSTONE_TASK_PLACEMENT   (1u << 0)
#define KEYSTONE_TASK_ANOMALY     (1u << 1)
#define KEYSTONE_TASK_SIMILARITY  (1u << 2)
#define KEYSTONE_TASK_RAG         (1u << 3)

/* --- Explicit Source Citation Record --- */
typedef struct {
    keystone_uuid_t source_object_id;
    keystone_uuid_t source_event_id;
    keystone_uuid_t origin_node_id;
    uint64_t source_generation;
    keystone_hlc_t source_hlc;
    char context_type[32]; /* "TIMELINE_EVENT", "TELEMETRY", "ANOMALY", "INCIDENT" */
} keystone_citation_t;

/* --- Structured AI/RAG Evidence Context Pack --- */
typedef struct {
    keystone_uuid_t resource_id;
    uint32_t object_type;
    uint32_t classification;
    uint64_t compartment_mask;
    uint64_t index_generation;
    keystone_hlc_t freshness_hlc;
    uint64_t staleness_ms;

    /* Recent Timeline Changes */
    uint32_t event_count;
    keystone_temporal_entry_t events[KEYSTONE_MAX_RAG_EVENTS];

    /* Rolling Telemetry Features */
    uint32_t metric_count;
    keystone_feature_window_t metrics[KEYSTONE_MAX_RAG_METRICS];

    /* Active Outlier Anomalies */
    uint32_t anomaly_count;
    keystone_anomaly_t anomalies[KEYSTONE_MAX_RAG_ANOMALIES];

    /* Historical Incident Matches */
    uint32_t incident_count;
    keystone_incident_match_t incidents[KEYSTONE_MAX_RAG_INCIDENTS];

    /* Topology Neighborhood & Relations */
    uint32_t neighbor_count;
    keystone_uuid_t neighbors[KEYSTONE_MAX_RAG_NEIGHBORS];
    uint32_t neighbor_relations[KEYSTONE_MAX_RAG_NEIGHBORS];

    /* Explicit Traceable Citations */
    uint32_t citation_count;
    keystone_citation_t citations[KEYSTONE_MAX_RAG_CITATIONS];

    char retrieval_provenance[256];
} keystone_context_pack_t;

/* --- Model Governance Manifest --- */
typedef struct {
    keystone_uuid_t model_id;
    char model_name[64];
    uint32_t model_version;
    uint32_t feature_schema_version;
    uint32_t supported_tasks; /* Bitmask: KEYSTONE_TASK_* */
    uint8_t weights_sha256[32];
    double validation_accuracy;
    double validation_p95_latency_ns;
    keystone_hlc_t created_at_hlc;
    uint64_t trained_generation;
    char training_data_manifest[128];
} keystone_model_manifest_t;

/* Opaque RAG & Model Governance Engine */
typedef struct keystone_rag_engine keystone_rag_engine_t;

/* --- Lifecycle Management --- */
keystone_rag_engine_t* keystone_rag_create(
    const keystone_topology_graph_t* topo,
    const keystone_temporal_index_t* temporal,
    const keystone_telemetry_engine_t* telemetry,
    const keystone_incident_engine_t* incident,
    uint64_t generation
);

void keystone_rag_destroy(keystone_rag_engine_t* engine);

/* --- Context Retrieval API --- */
int keystone_rag_query_context(
    const keystone_rag_engine_t* engine,
    const keystone_uuid_t* resource_id,
    const keystone_security_context_t* caller_security,
    uint64_t time_window_ms,
    keystone_context_pack_t* out_pack
);

/* --- Model Governance Registry --- */
int keystone_model_governance_register(
    keystone_rag_engine_t* engine,
    const keystone_model_manifest_t* manifest
);

int keystone_model_governance_validate(
    const keystone_rag_engine_t* engine,
    const keystone_uuid_t* model_id,
    uint32_t task_flag,
    const uint8_t weights_sha256[32]
);

size_t keystone_model_governance_count(
    const keystone_rag_engine_t* engine
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_RAG_H */
