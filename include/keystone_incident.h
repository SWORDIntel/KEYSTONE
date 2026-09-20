/*
 * KEYSTONE Incident Similarity Search & Silicon Acceleration Engine
 *
 * Vector-based failure pattern matching, root-cause correlation, and hardware-
 * accelerated distance computation with guaranteed scalar reference fallback.
 * Binds to NVIDIA CUDA kernels, Intel Sapphire Rapids AMX, and AVX-512.
 * Emits explainable recommendation bundles (never authoritative commands).
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#ifndef KEYSTONE_INCIDENT_H
#define KEYSTONE_INCIDENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include "keystone_telemetry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYSTONE_INCIDENT_EMBED_DIM 64
#define KEYSTONE_INCIDENT_LABEL_MAX 64
#define KEYSTONE_INCIDENT_NOTES_MAX 256

/* --- Hardware Silicon Acceleration Backends --- */
typedef enum {
    KEYSTONE_SILICON_SCALAR  = 0, /* Scalar CPU baseline (always available) */
    KEYSTONE_SILICON_AVX2    = 1, /* AVX2 + FMA SIMD */
    KEYSTONE_SILICON_AVX512  = 2, /* AVX-512F / AVX-512DQ vector registers */
    KEYSTONE_SILICON_AMX     = 3, /* Intel Sapphire Rapids AMX-TILE matrix multiply */
    KEYSTONE_SILICON_CUDA    = 4  /* NVIDIA CUDA GPU batch distance kernel */
} keystone_silicon_backend_t;

/* --- Canonical Historical Incident Record --- */
typedef struct {
    keystone_uuid_t incident_id;
    char root_cause_label[KEYSTONE_INCIDENT_LABEL_MAX];
    char resolution_notes[KEYSTONE_INCIDENT_NOTES_MAX];
    uint32_t object_type;          /* e.g. KEYSTONE_OBJ_NODE, KEYSTONE_OBJ_VM */
    uint32_t severity;             /* keystone_anomaly_severity_t */
    keystone_hlc_t incident_hlc;
    uint64_t index_generation;
    float embedding[KEYSTONE_INCIDENT_EMBED_DIM];
} keystone_incident_record_t;

/* --- Incident Similarity Match Result --- */
typedef struct {
    keystone_incident_record_t incident;
    float similarity_score;        /* Cosine similarity [0.0, 1.0] */
    float distance;                /* Metric distance (1.0 - cosine) */
    uint32_t backend_used;         /* keystone_silicon_backend_t */
    keystone_explain_t evidence;   /* Explainable evidence bundle */
} keystone_incident_match_t;

/* --- Similarity Query Specification --- */
typedef struct {
    float query_embedding[KEYSTONE_INCIDENT_EMBED_DIM];
    float min_similarity_threshold; /* e.g. 0.70 */
    uint32_t filter_severity_mask;  /* (1 << severity) or 0 for any */
    int force_backend;              /* -1 for automatic dispatch */
} keystone_incident_query_t;

/* Opaque Incident Engine */
typedef struct keystone_incident_engine keystone_incident_engine_t;

/* --- Lifecycle Management --- */
keystone_incident_engine_t* keystone_incident_engine_create(
    size_t capacity,
    uint64_t generation
);

void keystone_incident_engine_destroy(keystone_incident_engine_t* engine);

/* --- Catalog Registration --- */
int keystone_incident_register(
    keystone_incident_engine_t* engine,
    const keystone_incident_record_t* incident
);

/* --- Incident Similarity Retrieval --- */
size_t keystone_incident_search_similar(
    const keystone_incident_engine_t* engine,
    const keystone_incident_query_t* query,
    keystone_incident_match_t* out_matches,
    size_t max_matches
);

/*
 * Emit explainable recommendations from historical failure patterns.
 * Non-authoritative: advises controller/operator on root causes and known mitigations.
 */
size_t keystone_incident_recommend_mitigations(
    const keystone_incident_engine_t* engine,
    const keystone_incident_query_t* query,
    uint64_t current_generation,
    const keystone_hlc_t* current_hlc,
    keystone_recommendation_t* out_recommendations,
    size_t max_recommendations
);

/* --- Feature Vector Embedding Synthesizer --- */
void keystone_incident_embed_telemetry(
    const keystone_feature_window_t* features,
    size_t num_features,
    uint32_t isa_features,
    uint32_t error_code,
    float out_embedding[KEYSTONE_INCIDENT_EMBED_DIM]
);

/* --- Introspection & Hardware Helpers --- */
size_t keystone_incident_count(const keystone_incident_engine_t* engine);
keystone_silicon_backend_t keystone_incident_detect_best_backend(size_t candidate_count);
const char* keystone_silicon_backend_name(keystone_silicon_backend_t backend);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_INCIDENT_H */
