/*
 * KEYSTONE Structured AI/RAG Context Retrieval & Model Governance Implementation
 *
 * Implements structured context pack generation with strict source citations,
 * security classification boundaries, provenance, and SHA-256 model governance.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_rag.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYSTONE_MAX_GOVERNED_MODELS 32

struct keystone_rag_engine {
    const keystone_topology_graph_t* topo;
    const keystone_temporal_index_t* temporal;
    const keystone_telemetry_engine_t* telemetry;
    const keystone_incident_engine_t* incident;
    uint64_t index_generation;

    keystone_model_manifest_t models[KEYSTONE_MAX_GOVERNED_MODELS];
    size_t model_count;
};

keystone_rag_engine_t* keystone_rag_create(
    const keystone_topology_graph_t* topo,
    const keystone_temporal_index_t* temporal,
    const keystone_telemetry_engine_t* telemetry,
    const keystone_incident_engine_t* incident,
    uint64_t generation
) {
    keystone_rag_engine_t* engine = (keystone_rag_engine_t*)calloc(1, sizeof(keystone_rag_engine_t));
    if (!engine) return NULL;

    engine->topo = topo;
    engine->temporal = temporal;
    engine->telemetry = telemetry;
    engine->incident = incident;
    engine->index_generation = generation;
    engine->model_count = 0;
    return engine;
}

void keystone_rag_destroy(keystone_rag_engine_t* engine) {
    if (engine) {
        free(engine);
    }
}

static void add_citation(
    keystone_context_pack_t* pack,
    const keystone_uuid_t* obj_id,
    const keystone_uuid_t* evt_id,
    const keystone_uuid_t* node_id,
    uint64_t gen,
    const keystone_hlc_t* hlc,
    const char* ctx_type
) {
    if (pack->citation_count >= KEYSTONE_MAX_RAG_CITATIONS) return;
    keystone_citation_t* c = &pack->citations[pack->citation_count++];
    if (obj_id) c->source_object_id = *obj_id;
    if (evt_id) c->source_event_id = *evt_id;
    if (node_id) c->origin_node_id = *node_id;
    c->source_generation = gen;
    if (hlc) c->source_hlc = *hlc;
    snprintf(c->context_type, sizeof(c->context_type), "%s", ctx_type);
}

int keystone_rag_query_context(
    const keystone_rag_engine_t* engine,
    const keystone_uuid_t* resource_id,
    const keystone_security_context_t* caller_security,
    uint64_t time_window_ms,
    keystone_context_pack_t* out_pack
) {
    if (!engine || !resource_id || !out_pack) return -1;
    memset(out_pack, 0, sizeof(keystone_context_pack_t));

    out_pack->resource_id = *resource_id;
    out_pack->index_generation = engine->index_generation;
    out_pack->freshness_hlc = keystone_hlc_now(1);

    /* 1. Security & Classification Check */
    uint32_t target_class = KEYSTONE_CLASSIFICATION_UNCLASSIFIED;
    uint64_t target_compartments = 0;

    if (engine->topo) {
        keystone_node_metrics_t metrics;
        if (keystone_topology_get_node(engine->topo, resource_id, &metrics) == 0) {
            target_class = metrics.classification;
            target_compartments = metrics.compartment_mask;
            out_pack->object_type = KEYSTONE_OBJ_NODE;
        } else {
            out_pack->object_type = KEYSTONE_OBJ_VM;
        }
    }

    if (caller_security) {
        if (caller_security->classification < target_class) {
            return -2; /* Security access denied (insufficient classification) */
        }
        if ((target_compartments & ~caller_security->compartment_mask) != 0) {
            return -2; /* Security access denied (missing required compartments) */
        }
    }

    out_pack->classification = target_class;
    out_pack->compartment_mask = target_compartments;

    /* 2. Topology Neighborhood Expansion */
    if (engine->topo) {
        keystone_uuid_t nbrs[KEYSTONE_MAX_RAG_NEIGHBORS];
        size_t n_count = keystone_topology_find_neighbors(engine->topo, resource_id, KEYSTONE_EDGE_ALL, nbrs, KEYSTONE_MAX_RAG_NEIGHBORS);
        out_pack->neighbor_count = (uint32_t)n_count;
        for (size_t i = 0; i < n_count; i++) {
            out_pack->neighbors[i] = nbrs[i];
            out_pack->neighbor_relations[i] = KEYSTONE_EDGE_RUNS_ON;
            add_citation(out_pack, &nbrs[i], NULL, resource_id, engine->index_generation, &out_pack->freshness_hlc, "TOPOLOGY_EDGE");
        }
    }

    /* 3. Temporal Timeline Changes */
    if (engine->temporal) {
        keystone_hlc_t min_hlc = {0, 0, 0};
        if (out_pack->freshness_hlc.physical_ms > time_window_ms) {
            min_hlc.physical_ms = out_pack->freshness_hlc.physical_ms - time_window_ms;
        }
        keystone_temporal_entry_t evts[KEYSTONE_MAX_RAG_EVENTS];
        size_t e_count = keystone_temporal_index_query_object(
            engine->temporal,
            resource_id,
            &min_hlc,
            &out_pack->freshness_hlc,
            evts,
            KEYSTONE_MAX_RAG_EVENTS
        );
        out_pack->event_count = (uint32_t)e_count;
        for (size_t i = 0; i < e_count; i++) {
            out_pack->events[i] = evts[i];
            add_citation(out_pack, &evts[i].object_id, &evts[i].event_id, resource_id,
                         engine->index_generation, &evts[i].hlc, "TIMELINE_EVENT");
        }
    }

    /* 4. Streaming Telemetry Features */
    if (engine->telemetry) {
        uint32_t m_types[] = {
            KEYSTONE_TELEMETRY_CPU_UTILIZATION_PCT,
            KEYSTONE_TELEMETRY_MEMORY_PRESSURE_PCT,
            KEYSTONE_TELEMETRY_TEMPERATURE_CELSIUS,
            KEYSTONE_TELEMETRY_DISK_LATENCY_US
        };
        size_t n_types = sizeof(m_types) / sizeof(m_types[0]);
        for (size_t i = 0; i < n_types && out_pack->metric_count < KEYSTONE_MAX_RAG_METRICS; i++) {
            keystone_feature_window_t f;
            if (keystone_telemetry_extract_features(engine->telemetry, resource_id, m_types[i], KEYSTONE_WINDOW_5M, &f) == 0) {
                if (f.sample_count > 0) {
                    out_pack->metrics[out_pack->metric_count++] = f;
                    add_citation(out_pack, resource_id, NULL, resource_id,
                                 engine->index_generation, &f.end_hlc, "TELEMETRY_WINDOW");
                }
            }
        }

        /* 5. Active Outlier Anomalies */
        keystone_anomaly_t anoms[KEYSTONE_MAX_RAG_ANOMALIES];
        size_t a_count = keystone_telemetry_query_anomalies(engine->telemetry, resource_id, KEYSTONE_ANOMALY_WARNING, anoms, KEYSTONE_MAX_RAG_ANOMALIES);
        out_pack->anomaly_count = (uint32_t)a_count;
        for (size_t i = 0; i < a_count; i++) {
            out_pack->anomalies[i] = anoms[i];
            add_citation(out_pack, &anoms[i].source_node_id, &anoms[i].anomaly_id, resource_id,
                         anoms[i].index_generation, &anoms[i].timestamp_hlc, "ANOMALY");
        }
    }

    /* 6. Historical Incident Matching */
    if (engine->incident && out_pack->metric_count > 0) {
        float q_embed[KEYSTONE_INCIDENT_EMBED_DIM];
        keystone_incident_embed_telemetry(out_pack->metrics, out_pack->metric_count, 0, 0, q_embed);

        keystone_incident_query_t inc_q;
        memset(&inc_q, 0, sizeof(inc_q));
        memcpy(inc_q.query_embedding, q_embed, sizeof(q_embed));
        inc_q.min_similarity_threshold = 0.50f;
        inc_q.force_backend = -1;

        keystone_incident_match_t matches[KEYSTONE_MAX_RAG_INCIDENTS];
        size_t m_count = keystone_incident_search_similar(engine->incident, &inc_q, matches, KEYSTONE_MAX_RAG_INCIDENTS);
        out_pack->incident_count = (uint32_t)m_count;
        for (size_t i = 0; i < m_count; i++) {
            out_pack->incidents[i] = matches[i];
            add_citation(out_pack, &matches[i].incident.incident_id, NULL, resource_id,
                         matches[i].incident.index_generation, &matches[i].incident.incident_hlc, "INCIDENT_MATCH");
        }
    }

    snprintf(out_pack->retrieval_provenance, sizeof(out_pack->retrieval_provenance),
             "RAG pack generated: %u events, %u metrics, %u anomalies, %u incidents, %u citations (gen=%llu)",
             out_pack->event_count, out_pack->metric_count, out_pack->anomaly_count,
             out_pack->incident_count, out_pack->citation_count,
             (unsigned long long)engine->index_generation);

    return 0;
}

int keystone_model_governance_register(
    keystone_rag_engine_t* engine,
    const keystone_model_manifest_t* manifest
) {
    if (!engine || !manifest) return -1;
    if (engine->model_count >= KEYSTONE_MAX_GOVERNED_MODELS) return -2;

    /* Verify model_id is non-nil */
    if (keystone_uuid_is_nil(&manifest->model_id)) return -3;

    engine->models[engine->model_count++] = *manifest;
    return 0;
}

int keystone_model_governance_validate(
    const keystone_rag_engine_t* engine,
    const keystone_uuid_t* model_id,
    uint32_t task_flag,
    const uint8_t weights_sha256[32]
) {
    if (!engine || !model_id || !weights_sha256) return -1;

    for (size_t i = 0; i < engine->model_count; i++) {
        if (keystone_uuid_equal(&engine->models[i].model_id, model_id)) {
            const keystone_model_manifest_t* m = &engine->models[i];

            /* Check task support */
            if ((m->supported_tasks & task_flag) == 0) {
                return -2; /* Task not supported by governed model */
            }

            /* Check weights cryptographic hash integrity */
            if (memcmp(m->weights_sha256, weights_sha256, 32) != 0) {
                return -3; /* SHA-256 weight hash mismatch (tampered/unverified) */
            }

            return 0; /* Fully validated and approved for inference */
        }
    }
    return -4; /* Model not found in governance registry */
}

size_t keystone_model_governance_count(
    const keystone_rag_engine_t* engine
) {
    return engine ? engine->model_count : 0;
}
