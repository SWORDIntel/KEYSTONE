#define _POSIX_C_SOURCE 200809L
#include "keystone_aimem_vector.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

struct keystone_aimem_vector_index {
    size_t dim, max_entries, count, allocated, slots_cap;
    uint8_t model[32];
    uint64_t watermark;
    bool sealed;
    keystone_aimem_vector_entry_t *entries;
    float *vectors;
    uint32_t *slots; /* One-based indexes, zero means empty. */
};

static bool valid_id(const char id[KEYSTONE_AIMEM_SCOPE_MAX + 1u], bool required) {
    size_t n = strnlen(id, KEYSTONE_AIMEM_SCOPE_MAX + 1u);
    if ((required && !n) || n > KEYSTONE_AIMEM_SCOPE_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

static bool valid_scope(const char project[64], const char worktree[64],
                        const char workstream[64], const char session[64]) {
    return valid_id(project, true) && valid_id(worktree, false) &&
           valid_id(workstream, false) && valid_id(session, false) &&
           (!workstream[0] || worktree[0]) && (!session[0] || workstream[0]);
}

static size_t uuid_hash(const uint8_t uuid[16], size_t mask) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < 16; i++) { h ^= uuid[i]; h *= UINT64_C(1099511628211); }
    return (size_t)h & mask;
}

keystone_aimem_vector_index_t *keystone_aimem_vector_create(
        size_t dim, size_t max_entries, const uint8_t model[32]) {
    if (!model || dim == 0 || dim > KEYSTONE_AIMEM_VECTOR_MAX_DIM ||
        !max_entries || max_entries > 200000u) return NULL;
    uint8_t model_set = 0u;
    for (size_t i = 0; i < 32u; i++) model_set |= model[i];
    if (!model_set) return NULL;
    size_t slots_cap = 2u;
    while (slots_cap <= max_entries * 2u) slots_cap *= 2u;
    keystone_aimem_vector_index_t *index = calloc(1, sizeof *index);
    if (!index) return NULL;
    index->slots = calloc(slots_cap, sizeof *index->slots);
    if (!index->slots) { free(index); return NULL; }
    index->slots_cap = slots_cap;
    index->dim = dim;
    index->max_entries = max_entries;
    memcpy(index->model, model, sizeof index->model);
    return index;
}

void keystone_aimem_vector_destroy(keystone_aimem_vector_index_t *index) {
    if (!index) return;
    free(index->slots); free(index->entries); free(index->vectors); free(index);
}

size_t keystone_aimem_vector_count(const keystone_aimem_vector_index_t *index) {
    return index ? index->count : 0u;
}

keystone_aimem_vector_status_t keystone_aimem_vector_add(
        keystone_aimem_vector_index_t *index,
        const keystone_aimem_vector_entry_t *entry, const float *vec) {
    if (!index || !entry || !vec || index->sealed || !entry->tenant ||
        !valid_scope(entry->project, entry->worktree, entry->workstream, entry->session))
        return KEYSTONE_AIMEM_VECTOR_INVALID;
    uint8_t nonzero = 0u, digest_nonzero = 0u;
    for (size_t i = 0; i < 16u; i++) nonzero |= entry->uuid[i];
    for (size_t i = 0; i < 32u; i++) digest_nonzero |= entry->content_sha256[i];
    if (!nonzero || !digest_nonzero) return KEYSTONE_AIMEM_VECTOR_INVALID;
    size_t slot = uuid_hash(entry->uuid, index->slots_cap - 1u);
    while (index->slots[slot]) {
        if (memcmp(index->entries[index->slots[slot] - 1u].uuid, entry->uuid, 16u) == 0)
            return KEYSTONE_AIMEM_VECTOR_INVALID;
        slot = (slot + 1u) & (index->slots_cap - 1u);
    }
    if (index->count == index->max_entries) return KEYSTONE_AIMEM_VECTOR_FULL;
    double norm2 = 0.0;
    for (size_t i = 0; i < index->dim; i++) {
        if (!isfinite(vec[i])) return KEYSTONE_AIMEM_VECTOR_INVALID;
        norm2 += (double)vec[i] * (double)vec[i];
    }
    if (!(norm2 > 0.0) || !isfinite(norm2)) return KEYSTONE_AIMEM_VECTOR_INVALID;
    if (index->count == index->allocated) {
        size_t cap = index->allocated ? index->allocated * 2u : 64u;
        if (cap > index->max_entries) cap = index->max_entries;
        /* Commit either growth only after both allocations succeed. */
        keystone_aimem_vector_entry_t *entries = malloc(cap * sizeof *entries);
        float *vectors = malloc(cap * index->dim * sizeof *vectors);
        if (!entries || !vectors) { free(entries); free(vectors); return KEYSTONE_AIMEM_VECTOR_NOMEM; }
        if (index->count) {
            memcpy(entries, index->entries, index->count * sizeof *entries);
            memcpy(vectors, index->vectors, index->count * index->dim * sizeof *vectors);
        }
        free(index->entries); free(index->vectors);
        index->entries = entries; index->vectors = vectors; index->allocated = cap;
    }
    index->entries[index->count] = *entry;
    float *dst = &index->vectors[index->count * index->dim];
    double inverse = 1.0 / sqrt(norm2);
    for (size_t i = 0; i < index->dim; i++) dst[i] = (float)((double)vec[i] * inverse);
    index->slots[slot] = (uint32_t)(++index->count);
    return KEYSTONE_AIMEM_VECTOR_OK;
}

keystone_aimem_vector_status_t keystone_aimem_vector_seal(
        keystone_aimem_vector_index_t *index, uint64_t watermark) {
    if (!index || !watermark || index->sealed) return KEYSTONE_AIMEM_VECTOR_INVALID;
    index->watermark = watermark;
    index->sealed = true;
    return KEYSTONE_AIMEM_VECTOR_OK;
}

static float score_scalar(const float *a, const float *b, size_t dim) {
    double dot = 0.0;
    for (size_t i = 0; i < dim; i++) dot += (double)a[i] * (double)b[i];
    return (float)dot;
}

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2,fma")))
static float score_avx2(const float *a, const float *b, size_t dim) {
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8u <= dim; i += 8u)
        sum = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), sum);
    float lanes[8];
    _mm256_storeu_ps(lanes, sum);
    float dot = 0.0f;
    for (size_t j = 0; j < 8; j++) dot += lanes[j];
    for (; i < dim; i++) dot += a[i] * b[i];
    return dot;
}
#endif

static bool better(float score, const uint8_t uuid[16],
                   const keystone_aimem_vector_hit_t *hit) {
    return score > hit->cosine ||
           (score == hit->cosine && memcmp(uuid, hit->uuid, 16u) < 0);
}

static bool deadline_expired(uint64_t deadline) {
    if (!deadline) return false;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return true;
    return (uint64_t)now.tv_sec > deadline / UINT64_C(1000000000) ||
           ((uint64_t)now.tv_sec == deadline / UINT64_C(1000000000) &&
            (uint64_t)now.tv_nsec >= deadline % UINT64_C(1000000000));
}

keystone_aimem_vector_status_t keystone_aimem_vector_search(
        const keystone_aimem_vector_index_t *index,
        const keystone_aimem_vector_query_t *query, const float *embedding,
        keystone_aimem_vector_hit_t *out, size_t capacity, size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!index || !index->sealed || !query || !embedding || !out ||
        !out_count || !capacity || capacity > 256u || !query->tenant ||
        !query->visible || !valid_scope(query->project, query->worktree,
                                       query->workstream, query->session))
        return KEYSTONE_AIMEM_VECTOR_INVALID;
    if (memcmp(index->model, query->model_sha256, 32u) ||
        index->watermark < query->required_watermark)
        return KEYSTONE_AIMEM_VECTOR_STALE;
    float normalized[KEYSTONE_AIMEM_VECTOR_MAX_DIM];
    double norm2 = 0.0;
    for (size_t i = 0; i < index->dim; i++) {
        if (!isfinite(embedding[i])) return KEYSTONE_AIMEM_VECTOR_INVALID;
        norm2 += (double)embedding[i] * (double)embedding[i];
    }
    if (!(norm2 > 0.0) || !isfinite(norm2)) return KEYSTONE_AIMEM_VECTOR_INVALID;
    double inverse = 1.0 / sqrt(norm2);
    for (size_t i = 0; i < index->dim; i++) normalized[i] = (float)((double)embedding[i] * inverse);
#if defined(__x86_64__) && defined(__GNUC__)
    __builtin_cpu_init();
    bool avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
    size_t count = 0;
    for (size_t i = 0; i < index->count; i++) {
        if (deadline_expired(query->deadline_ns)) {
            memset(out, 0, count * sizeof *out);
            return KEYSTONE_AIMEM_VECTOR_DEADLINE;
        }
        const keystone_aimem_vector_entry_t *e = &index->entries[i];
        if (e->tenant != query->tenant || (query->kind && e->kind != query->kind) ||
            e->classification > query->max_classification ||
            (e->sci & query->allowed_sci) != e->sci ||
            strcmp(e->project, query->project) ||
            (query->worktree[0] && strcmp(e->worktree, query->worktree)) ||
            (query->workstream[0] && strcmp(e->workstream, query->workstream)) ||
            (query->session[0] && strcmp(e->session, query->session))) continue;
        int visible = query->visible(e, query->visible_context);
        if (visible < 0) {
            memset(out, 0, count * sizeof *out);
            return KEYSTONE_AIMEM_VECTOR_INVALID;
        }
        if (!visible) continue;
        const float *v = &index->vectors[i * index->dim];
        float score;
#if defined(__x86_64__) && defined(__GNUC__)
        score = avx2 ? score_avx2(normalized, v, index->dim) :
                       score_scalar(normalized, v, index->dim);
#else
        score = score_scalar(normalized, v, index->dim);
#endif
        if (score > 1.0f) score = 1.0f;
        if (score < -1.0f) score = -1.0f;
        size_t pos = count;
        if (pos == capacity) {
            if (!better(score, e->uuid, &out[count - 1u])) continue;
            pos = count - 1u;
        } else count++;
        while (pos && better(score, e->uuid, &out[pos - 1u])) {
            out[pos] = out[pos - 1u];
            pos--;
        }
        memcpy(out[pos].uuid, e->uuid, 16u);
        out[pos].source_change_seq = e->source_change_seq;
        memcpy(out[pos].content_sha256, e->content_sha256, 32u);
        out[pos].cosine = score;
    }
    if (deadline_expired(query->deadline_ns)) {
        memset(out, 0, count * sizeof *out);
        return KEYSTONE_AIMEM_VECTOR_DEADLINE;
    }
    *out_count = count;
    return KEYSTONE_AIMEM_VECTOR_OK;
}
