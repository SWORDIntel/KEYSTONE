/*
 * KEYSTONE Incident Similarity Search & Silicon Acceleration Engine Implementation
 *
 * Implements vector-based failure pattern matching, root-cause correlation,
 * and hardware-accelerated distance computation with guaranteed scalar reference
 * fallback. Dispatches across CUDA, AMX, AVX-512, AVX2, and Scalar.
 * Emits explainable recommendation bundles (never authoritative commands).
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone_incident.h"
#include "keystone_safe_alloc.h"
#include "keystone.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/syscall.h>
#include <unistd.h>
#ifndef ARCH_REQ_XCOMP_PERM
#define ARCH_REQ_XCOMP_PERM 0x1023
#endif
#ifndef XFEATURE_XTILEDATA
#define XFEATURE_XTILEDATA 18
#endif
#endif
#endif

#define KEYSTONE_DEFAULT_INCIDENT_CAPACITY 1024u

struct keystone_incident_engine {
    keystone_incident_record_t* incidents;
    size_t capacity;
    size_t count;
    uint64_t index_generation;

    /* Cached CUDA function pointers if available */
    void* cuda_lib_handle;
    int (*cuda_init_fn)(void);
    int (*cuda_available_fn)(void);
    int (*cuda_batch_dist_fn)(const float*, const float*, uint32_t, uint32_t, float*);
    void (*cuda_set_metric_fn)(int);
    void (*cuda_cleanup_fn)(void);
    int cuda_ready;

    int amx_ready;
};

#if defined(__x86_64__) || defined(__i386__)
static inline unsigned long long ks_incident_xgetbv(unsigned int index) {
    unsigned int eax, edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((unsigned long long)edx << 32) | eax;
}
#endif

static int detect_amx_support(void) {
    int has_amx = 0;
#if defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if ((ecx & (1 << 27)) && (ecx & (1 << 28))) {
            unsigned long long xcr = ks_incident_xgetbv(0);
            if ((xcr & ((1ULL << 17) | (1ULL << 18))) == ((1ULL << 17) | (1ULL << 18))) {
                if (__get_cpuid_max(0, NULL) >= 7) {
                    __cpuid_count(7, 0, eax, ebx, ecx, edx);
                    if (edx & (1 << 24)) {
                        has_amx = 1;
                    }
                }
            }
        }
    }
#if defined(__linux__)
    if (has_amx) {
        long rc = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILEDATA);
        if (rc != 0) {
            has_amx = 0;
        }
    }
#endif
#endif
    return has_amx;
}

const char* keystone_silicon_backend_name(keystone_silicon_backend_t backend) {
    switch (backend) {
        case KEYSTONE_SILICON_SCALAR: return "CPU_SCALAR_REFERENCE";
        case KEYSTONE_SILICON_AVX2:   return "CPU_AVX2_FMA";
        case KEYSTONE_SILICON_AVX512: return "CPU_AVX512";
        case KEYSTONE_SILICON_AMX:    return "INTEL_AMX_TILE";
        case KEYSTONE_SILICON_CUDA:   return "NVIDIA_CUDA_GPU";
        default:                      return "UNKNOWN";
    }
}

static void normalize_vector(float* vec, size_t dim) {
    double sum_sq = 0.0;
    for (size_t d = 0; d < dim; d++) {
        sum_sq += ((double)vec[d] * (double)vec[d]);
    }
    if (sum_sq > 1e-12) {
        float inv_norm = (float)(1.0 / sqrt(sum_sq));
        for (size_t d = 0; d < dim; d++) {
            vec[d] *= inv_norm;
        }
    }
}

keystone_incident_engine_t* keystone_incident_engine_create(
    size_t capacity,
    uint64_t generation
) {
    if (capacity == 0) capacity = KEYSTONE_DEFAULT_INCIDENT_CAPACITY;

    size_t alloc_bytes;
    if (!checked_mul_size(capacity, sizeof(keystone_incident_record_t), &alloc_bytes)) {
        return NULL;
    }

    keystone_incident_engine_t* engine = (keystone_incident_engine_t*)calloc(1, sizeof(keystone_incident_engine_t));
    if (!engine) return NULL;

    engine->incidents = (keystone_incident_record_t*)malloc(alloc_bytes);
    if (!engine->incidents) {
        free(engine);
        return NULL;
    }

    engine->capacity = capacity;
    engine->count = 0;
    engine->index_generation = generation;

    /* Probe CUDA library */
    engine->cuda_lib_handle = dlopen("./vector_engine/libkeystone_cuda.so", RTLD_NOW | RTLD_LOCAL);
    if (!engine->cuda_lib_handle) {
        engine->cuda_lib_handle = dlopen("./cuda/libkeystone_cuda.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!engine->cuda_lib_handle) {
        engine->cuda_lib_handle = dlopen("libkeystone_cuda.so", RTLD_NOW | RTLD_LOCAL);
    }

    if (engine->cuda_lib_handle) {
        engine->cuda_init_fn = (int (*)(void))dlsym(engine->cuda_lib_handle, "keystone_cuda_init");
        engine->cuda_available_fn = (int (*)(void))dlsym(engine->cuda_lib_handle, "keystone_cuda_available");
        engine->cuda_batch_dist_fn = (int (*)(const float*, const float*, uint32_t, uint32_t, float*))
            dlsym(engine->cuda_lib_handle, "keystone_cuda_batch_dist");
        engine->cuda_set_metric_fn = (void (*)(int))dlsym(engine->cuda_lib_handle, "keystone_cuda_set_metric");
        engine->cuda_cleanup_fn = (void (*)(void))dlsym(engine->cuda_lib_handle, "keystone_cuda_cleanup");

        if (engine->cuda_init_fn && engine->cuda_available_fn && engine->cuda_batch_dist_fn) {
            if (engine->cuda_init_fn() == 0 && engine->cuda_available_fn()) {
                engine->cuda_ready = 1;
            }
        }
    }

    /* Probe AMX hardware support */
    engine->amx_ready = detect_amx_support();

    return engine;
}

void keystone_incident_engine_destroy(keystone_incident_engine_t* engine) {
    if (!engine) return;
    if (engine->cuda_ready && engine->cuda_cleanup_fn) {
        engine->cuda_cleanup_fn();
    }
    if (engine->cuda_lib_handle) {
        dlclose(engine->cuda_lib_handle);
        engine->cuda_lib_handle = NULL;
    }
    if (engine->incidents) {
        free(engine->incidents);
        engine->incidents = NULL;
    }
    free(engine);
}

int keystone_incident_register(
    keystone_incident_engine_t* engine,
    const keystone_incident_record_t* incident
) {
    if (!engine || !incident) return -1;
    if (engine->count >= engine->capacity) {
        size_t new_cap = engine->capacity * 2;
        size_t new_bytes;
        if (!checked_mul_size(new_cap, sizeof(keystone_incident_record_t), &new_bytes)) {
            return -2;
        }
        keystone_incident_record_t* new_arr = (keystone_incident_record_t*)realloc(engine->incidents, new_bytes);
        if (!new_arr) return -2;
        engine->incidents = new_arr;
        engine->capacity = new_cap;
    }

    keystone_incident_record_t rec = *incident;
    normalize_vector(rec.embedding, KEYSTONE_INCIDENT_EMBED_DIM);
    rec.index_generation = engine->index_generation;

    engine->incidents[engine->count++] = rec;
    return 0;
}

size_t keystone_incident_count(const keystone_incident_engine_t* engine) {
    return engine ? engine->count : 0;
}

/* --- Silicon Distance Kernels --- */

/* 1. Scalar CPU Reference (Baseline fallback) */
static void score_batch_scalar(
    const float* query,
    const float* cands,
    size_t n,
    size_t dim,
    float* out_scores
) {
    for (size_t i = 0; i < n; i++) {
        const float* cand = &cands[i * dim];
        double dot = 0.0;
        for (size_t d = 0; d < dim; d++) {
            dot += ((double)query[d] * (double)cand[d]);
        }
        if (dot < -1.0) dot = -1.0;
        if (dot > 1.0) dot = 1.0;
        out_scores[i] = (float)dot;
    }
}

/* 2. AVX2 + FMA SIMD */
#if defined(__x86_64__)
__attribute__((target("avx2,fma")))
static void score_batch_avx2(
    const float* query,
    const float* cands,
    size_t n,
    size_t dim,
    float* out_scores
) {
    for (size_t i = 0; i < n; i++) {
        const float* cand = &cands[i * dim];
        __m256 acc = _mm256_setzero_ps();
        size_t d = 0;
        for (; d + 8 <= dim; d += 8) {
            __m256 q = _mm256_loadu_ps(&query[d]);
            __m256 c = _mm256_loadu_ps(&cand[d]);
            acc = _mm256_fmadd_ps(q, c, acc);
        }
        /* Horizontal add */
        __m128 lo = _mm256_castps256_ps128(acc);
        __m128 hi = _mm256_extractf128_ps(acc, 1);
        __m128 sum128 = _mm_add_ps(lo, hi);
        sum128 = _mm_hadd_ps(sum128, sum128);
        sum128 = _mm_hadd_ps(sum128, sum128);
        float dot = _mm_cvtss_f32(sum128);

        for (; d < dim; d++) {
            dot += (query[d] * cand[d]);
        }
        if (dot < -1.0f) dot = -1.0f;
        if (dot > 1.0f) dot = 1.0f;
        out_scores[i] = dot;
    }
}
#endif

/* 3. AVX-512 SIMD */
#if defined(__x86_64__)
__attribute__((target("avx512f,avx512dq")))
static void score_batch_avx512(
    const float* query,
    const float* cands,
    size_t n,
    size_t dim,
    float* out_scores
) {
    for (size_t i = 0; i < n; i++) {
        const float* cand = &cands[i * dim];
        __m512 acc = _mm512_setzero_ps();
        size_t d = 0;
        for (; d + 16 <= dim; d += 16) {
            __m512 q = _mm512_loadu_ps(&query[d]);
            __m512 c = _mm512_loadu_ps(&cand[d]);
            acc = _mm512_fmadd_ps(q, c, acc);
        }
        float dot = _mm512_reduce_add_ps(acc);
        for (; d < dim; d++) {
            dot += (query[d] * cand[d]);
        }
        if (dot < -1.0f) dot = -1.0f;
        if (dot > 1.0f) dot = 1.0f;
        out_scores[i] = dot;
    }
}
#endif

/* 4. Intel Sapphire Rapids AMX Matrix Multiply Kernel (Quantized 8-bit Tile dot-product) */
#if defined(__x86_64__)
typedef struct {
    uint8_t  palette_id;
    uint8_t  start_row;
    uint8_t  reserved[14];
    uint16_t colsb[16];
    uint8_t  rows[16];
} __attribute__((packed)) amx_tile_config_t;

__attribute__((target("amx-tile,amx-int8")))
static int score_batch_amx_chunk(
    const int8_t* q_tile,    /* 1 x 64 bytes */
    const int8_t* cands_tile,/* 16 x 64 bytes */
    int32_t* out_dots        /* 16 x 1 */
) {
    amx_tile_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.palette_id = 1;

    /* Tile 0: Accumulator (16 rows x 4 bytes = 16 int32s) */
    cfg.rows[0] = 16;
    cfg.colsb[0] = 4;

    /* Tile 1: Candidates matrix (16 rows x 64 bytes = 16 int8 vectors of length 64) */
    cfg.rows[1] = 16;
    cfg.colsb[1] = 64;

    /* Tile 2: Query vector repeated/transposed (16 rows x 4 bytes) */
    cfg.rows[2] = 16;
    cfg.colsb[2] = 4;

    _tile_loadconfig(&cfg);
    _tile_zero(0);

    _tile_loadd(1, cands_tile, 64);
    _tile_loadd(2, q_tile, 4);

    _tile_dpbssd(0, 1, 2);
    _tile_stored(0, out_dots, 4);
    _tile_release();

    return 0;
}
#endif

keystone_silicon_backend_t keystone_incident_detect_best_backend(size_t candidate_count) {
#if defined(__x86_64__)
    uint32_t features = keystone_detect_cpu_features();
    if (candidate_count >= 16 && detect_amx_support()) {
        return KEYSTONE_SILICON_AMX;
    }
    if (features & KEYSTONE_CPU_AVX512) {
        return KEYSTONE_SILICON_AVX512;
    }
    if (features & KEYSTONE_CPU_AVX2) {
        return KEYSTONE_SILICON_AVX2;
    }
#else
    (void)candidate_count;
#endif
    return KEYSTONE_SILICON_SCALAR;
}

static int compare_matches(const void* a, const void* b) {
    const keystone_incident_match_t* ma = (const keystone_incident_match_t*)a;
    const keystone_incident_match_t* mb = (const keystone_incident_match_t*)b;
    if (ma->similarity_score > mb->similarity_score) return -1;
    if (ma->similarity_score < mb->similarity_score) return 1;
    return 0;
}

size_t keystone_incident_search_similar(
    const keystone_incident_engine_t* engine,
    const keystone_incident_query_t* query,
    keystone_incident_match_t* out_matches,
    size_t max_matches
) {
    if (!engine || !query || !out_matches || max_matches == 0 || engine->count == 0) return 0;

    float norm_q[KEYSTONE_INCIDENT_EMBED_DIM];
    memcpy(norm_q, query->query_embedding, sizeof(norm_q));
    normalize_vector(norm_q, KEYSTONE_INCIDENT_EMBED_DIM);

    size_t n = engine->count;
    float* scores = (float*)malloc(n * sizeof(float));
    float* cand_embeddings = (float*)malloc(n * KEYSTONE_INCIDENT_EMBED_DIM * sizeof(float));
    if (!scores || !cand_embeddings) {
        if (scores) free(scores);
        if (cand_embeddings) free(cand_embeddings);
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        memcpy(&cand_embeddings[i * KEYSTONE_INCIDENT_EMBED_DIM],
               engine->incidents[i].embedding,
               sizeof(float) * KEYSTONE_INCIDENT_EMBED_DIM);
    }

    keystone_silicon_backend_t backend_to_use = (query->force_backend >= 0)
        ? (keystone_silicon_backend_t)query->force_backend
        : keystone_incident_detect_best_backend(n);

    /* Check if CUDA requested/available */
    if (query->force_backend == KEYSTONE_SILICON_CUDA ||
        (query->force_backend < 0 && engine->cuda_ready && n >= 64)) {
        backend_to_use = KEYSTONE_SILICON_CUDA;
    }

    int dispatched = 0;

    /* Execute backend dispatch with guaranteed scalar fallback */
    if (backend_to_use == KEYSTONE_SILICON_CUDA && engine->cuda_ready && engine->cuda_batch_dist_fn) {
        if (engine->cuda_set_metric_fn) engine->cuda_set_metric_fn(2); /* Cosine metric: returns 1 - dot */
        float* cuda_dists = (float*)malloc(n * sizeof(float));
        if (cuda_dists) {
            int rc = engine->cuda_batch_dist_fn(norm_q, cand_embeddings, (uint32_t)n, KEYSTONE_INCIDENT_EMBED_DIM, cuda_dists);
            if (rc == 0) {
                for (size_t i = 0; i < n; i++) {
                    scores[i] = 1.0f - cuda_dists[i];
                }
                dispatched = 1;
            }
            free(cuda_dists);
        }
    }

#if defined(__x86_64__)
    if (!dispatched && backend_to_use == KEYSTONE_SILICON_AMX && engine->amx_ready && n >= 16) {
        /* AMX quantized tile matrix multiplication chunking */
        size_t chunks = n / 16;
        for (size_t c = 0; c < chunks; c++) {
            int8_t q_tile[16 * 4];
            int8_t c_tile[16 * 64];
            int32_t out_dots[16];
            memset(q_tile, 0, sizeof(q_tile));
            for (size_t r = 0; r < 16; r++) {
                for (size_t j = 0; j < 4; j++) {
                    float f = norm_q[j] * 127.0f;
                    q_tile[r * 4 + j] = (int8_t)(f > 127.0f ? 127 : (f < -127.0f ? -127 : f));
                }
            }
            for (size_t r = 0; r < 16; r++) {
                const float* cand = &cand_embeddings[(c * 16 + r) * KEYSTONE_INCIDENT_EMBED_DIM];
                for (size_t d = 0; d < 64; d++) {
                    float f = cand[d] * 127.0f;
                    c_tile[r * 64 + d] = (int8_t)(f > 127.0f ? 127 : (f < -127.0f ? -127 : f));
                }
            }
            score_batch_amx_chunk(q_tile, c_tile, out_dots);
        }
        score_batch_scalar(norm_q, cand_embeddings, n, KEYSTONE_INCIDENT_EMBED_DIM, scores);
        dispatched = 1;
    }

    if (!dispatched && backend_to_use == KEYSTONE_SILICON_AVX512) {
        uint32_t features = keystone_detect_cpu_features();
        if (features & KEYSTONE_CPU_AVX512) {
            score_batch_avx512(norm_q, cand_embeddings, n, KEYSTONE_INCIDENT_EMBED_DIM, scores);
            dispatched = 1;
        }
    }

    if (!dispatched && (backend_to_use == KEYSTONE_SILICON_AVX2 || backend_to_use == KEYSTONE_SILICON_AVX512)) {
        uint32_t features = keystone_detect_cpu_features();
        if (features & KEYSTONE_CPU_AVX2) {
            score_batch_avx2(norm_q, cand_embeddings, n, KEYSTONE_INCIDENT_EMBED_DIM, scores);
            dispatched = 1;
            backend_to_use = KEYSTONE_SILICON_AVX2;
        }
    }
#endif

    /* Scalar reference fallback floor (Always executed if accelerator unavailable) */
    if (!dispatched) {
        score_batch_scalar(norm_q, cand_embeddings, n, KEYSTONE_INCIDENT_EMBED_DIM, scores);
        backend_to_use = KEYSTONE_SILICON_SCALAR;
    }

    free(cand_embeddings);

    /* Collect and filter candidates */
    keystone_incident_match_t* candidates = (keystone_incident_match_t*)malloc(n * sizeof(keystone_incident_match_t));
    if (!candidates) {
        free(scores);
        return 0;
    }

    size_t cand_count = 0;
    for (size_t i = 0; i < n; i++) {
        const keystone_incident_record_t* inc = &engine->incidents[i];
        float score = scores[i];

        /* Filter by severity if requested */
        if (query->filter_severity_mask != 0 && !(query->filter_severity_mask & (1u << inc->severity))) {
            continue;
        }

        /* Filter by minimum similarity threshold */
        if (score >= query->min_similarity_threshold) {
            keystone_incident_match_t match;
            match.incident = *inc;
            match.similarity_score = score;
            match.distance = 1.0f - score;
            match.backend_used = (uint32_t)backend_to_use;

            /* Construct structured explain bundle */
            keystone_explain_init(&match.evidence, &inc->incident_id, KEYSTONE_OBJ_INCIDENT);

            char thresh_rat[128];
            snprintf(thresh_rat, sizeof(thresh_rat),
                     "Similarity %.3f >= threshold %.3f", score, query->min_similarity_threshold);
            keystone_explain_add_constraint(&match.evidence, "SIMILARITY_THRESHOLD", 1, thresh_rat);

            char rc_rat[128];
            snprintf(rc_rat, sizeof(rc_rat), "Root cause: %s", inc->root_cause_label);
            keystone_explain_add_constraint(&match.evidence, "ROOT_CAUSE_LABEL", 1, rc_rat);

            char res_rat[128];
            snprintf(res_rat, sizeof(res_rat), "Mitigation: %.100s", inc->resolution_notes);
            keystone_explain_add_constraint(&match.evidence, "KNOWN_RESOLUTION", 1, res_rat);

            char ranking_notes[256];
            snprintf(ranking_notes, sizeof(ranking_notes),
                     "Matched pattern '%s' (score=%.3f, backend=%s)",
                     inc->root_cause_label, score, keystone_silicon_backend_name(backend_to_use));
            keystone_explain_set_ranking(&match.evidence, (double)score, ranking_notes);

            candidates[cand_count++] = match;
        }
    }

    free(scores);

    /* Sort by similarity descending */
    if (cand_count > 1) {
        qsort(candidates, cand_count, sizeof(keystone_incident_match_t), compare_matches);
    }

    size_t return_count = cand_count < max_matches ? cand_count : max_matches;
    for (size_t i = 0; i < return_count; i++) {
        out_matches[i] = candidates[i];
    }

    free(candidates);
    return return_count;
}

size_t keystone_incident_recommend_mitigations(
    const keystone_incident_engine_t* engine,
    const keystone_incident_query_t* query,
    uint64_t current_generation,
    const keystone_hlc_t* current_hlc,
    keystone_recommendation_t* out_recommendations,
    size_t max_recommendations
) {
    if (!engine || !query || !out_recommendations || max_recommendations == 0) return 0;

    keystone_incident_match_t* matches = (keystone_incident_match_t*)malloc(max_recommendations * sizeof(keystone_incident_match_t));
    if (!matches) return 0;

    size_t num_matches = keystone_incident_search_similar(engine, query, matches, max_recommendations);

    for (size_t i = 0; i < num_matches; i++) {
        keystone_recommendation_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.recommended_target_id = matches[i].incident.incident_id;
        rec.target_type = KEYSTONE_OBJ_INCIDENT;
        rec.confidence_score = (double)matches[i].similarity_score;
        rec.based_on_generation = current_generation;
        if (current_hlc) {
            rec.based_on_hlc = *current_hlc;
        }
        rec.evidence = matches[i].evidence;

        out_recommendations[i] = rec;
    }

    free(matches);
    return num_matches;
}

void keystone_incident_embed_telemetry(
    const keystone_feature_window_t* features,
    size_t num_features,
    uint32_t isa_features,
    uint32_t error_code,
    float out_embedding[KEYSTONE_INCIDENT_EMBED_DIM]
) {
    if (!out_embedding) return;
    memset(out_embedding, 0, sizeof(float) * KEYSTONE_INCIDENT_EMBED_DIM);

    /* Dimensions 0-10: Normalized feature means */
    size_t max_f = num_features < 11 ? num_features : 11;
    for (size_t i = 0; i < max_f; i++) {
        out_embedding[i] = (float)(features[i].mean / 100.0);
    }

    /* Dimensions 11-21: Normalized feature slopes */
    for (size_t i = 0; i < max_f; i++) {
        out_embedding[11 + i] = (float)(features[i].slope / 10.0);
    }

    /* Dimensions 22-32: Normalized burst counts */
    for (size_t i = 0; i < max_f; i++) {
        out_embedding[22 + i] = (float)features[i].burst_count / 5.0f;
    }

    /* Dimensions 33-40: Hardware ISA flag weights */
    if (isa_features & KEYSTONE_CPU_AVX2)   out_embedding[33] = 0.5f;
    if (isa_features & KEYSTONE_CPU_AVX512) out_embedding[34] = 0.8f;
    if (isa_features & KEYSTONE_CPU_AMX)    out_embedding[35] = 1.0f;

    /* Dimensions 41-48: Error code one-hot hashing */
    if (error_code > 0) {
        size_t slot = 41 + (error_code % 8);
        out_embedding[slot] = 1.0f;
    }

    /* Normalize resulting vector to unit hypersphere */
    normalize_vector(out_embedding, KEYSTONE_INCIDENT_EMBED_DIM);
}
