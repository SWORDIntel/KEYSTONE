#define _POSIX_C_SOURCE 200809L
#include "keystone_aimem_vector.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DIM 384u

static int visible_owner(const keystone_aimem_vector_entry_t *e, void *ctx) {
    return e->owner == *(uint32_t *)ctx;
}

static int fail_second(const keystone_aimem_vector_entry_t *e, void *ctx) {
    (void)e;
    int *calls = ctx;
    return ++*calls == 2 ? -1 : 1;
}

static uint64_t now_ns(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

typedef struct {
    const keystone_aimem_vector_index_t *index;
    keystone_aimem_vector_query_t query;
    float vector[DIM];
} reader_t;

static void *reader(void *arg) {
    reader_t *ctx = arg;
    for (unsigned i = 0; i < 100; i++) {
        keystone_aimem_vector_hit_t hits[3];
        size_t count = 8;
        assert(keystone_aimem_vector_search(ctx->index, &ctx->query, ctx->vector,
                                             hits, 3u, &count) == KEYSTONE_AIMEM_VECTOR_OK);
        assert(count == 2u && hits[0].uuid[0] == 1u && hits[1].uuid[0] == 2u);
    }
    return NULL;
}

int main(void) {
    assert(keystone_aimem_vector_abi_version() == KEYSTONE_AIMEM_VECTOR_ABI_VERSION);
    uint8_t model[32] = {1u};
    uint8_t unset_model[32] = {0};
    assert(!keystone_aimem_vector_create(DIM, 4u, unset_model));
    assert(!keystone_aimem_vector_create(0, 4u, model));
    assert(!keystone_aimem_vector_create(DIM, 0, model));
    keystone_aimem_vector_index_t *index = keystone_aimem_vector_create(DIM, 5u, model);
    assert(index);
    float a[DIM] = {0}; a[0] = 2.0f;
    float b[DIM] = {0}; b[0] = 1.0f; b[1] = 1.0f;
    float forbidden[DIM] = {0}; forbidden[0] = 100.0f;
    keystone_aimem_vector_entry_t e = {.tenant = 1u, .owner = 20u,
        .source_change_seq = 11u,
        .kind = 2u, .classification = 2u, .sci = 1u};
    strcpy(e.project, "project"); strcpy(e.worktree, "tree");
    assert(keystone_aimem_vector_add(index, &e, a) == KEYSTONE_AIMEM_VECTOR_INVALID);
    e.uuid[0] = 1u;
    assert(keystone_aimem_vector_add(index, &e, a) == KEYSTONE_AIMEM_VECTOR_INVALID);
    e.content_sha256[0] = 42u;
    float zero[DIM] = {0};
    assert(keystone_aimem_vector_add(index, &e, zero) == KEYSTONE_AIMEM_VECTOR_INVALID);
    assert(keystone_aimem_vector_add(index, &e, a) == KEYSTONE_AIMEM_VECTOR_OK);
    assert(keystone_aimem_vector_add(index, &e, a) == KEYSTONE_AIMEM_VECTOR_INVALID);
    e.uuid[0] = 2u; e.source_change_seq = 0u; /* Imported legacy record. */
    assert(keystone_aimem_vector_add(index, &e, b) == KEYSTONE_AIMEM_VECTOR_OK);
    e.uuid[0] = 3u; e.owner = 30u;
    assert(keystone_aimem_vector_add(index, &e, forbidden) == KEYSTONE_AIMEM_VECTOR_OK);
    e.uuid[0] = 4u; e.owner = 20u; e.tenant = 2u;
    assert(keystone_aimem_vector_add(index, &e, forbidden) == KEYSTONE_AIMEM_VECTOR_OK);
    e.uuid[0] = 5u; e.tenant = 1u; e.classification = 4u;
    assert(keystone_aimem_vector_add(index, &e, forbidden) == KEYSTONE_AIMEM_VECTOR_OK);
    e.uuid[0] = 6u;
    assert(keystone_aimem_vector_add(index, &e, a) == KEYSTONE_AIMEM_VECTOR_FULL);
    assert(keystone_aimem_vector_count(index) == 5u);
    assert(keystone_aimem_vector_seal(index, 22u) == KEYSTONE_AIMEM_VECTOR_OK);
    assert(keystone_aimem_vector_add(index, &e, a) == KEYSTONE_AIMEM_VECTOR_INVALID);
    uint32_t owner = 20u;
    keystone_aimem_vector_query_t q = {.tenant = 1u, .kind = 2u,
        .max_classification = 2u, .allowed_sci = 1u, .required_watermark = 22u,
        .visible = visible_owner, .visible_context = &owner};
    memcpy(q.model_sha256, model, 32u);
    strcpy(q.project, "project"); strcpy(q.worktree, "tree");
    keystone_aimem_vector_hit_t hits[4];
    size_t count = 999u;
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_OK);
    assert(count == 2u && hits[0].uuid[0] == 1u && hits[1].uuid[0] == 2u);
    assert(hits[0].source_change_seq == 11u && hits[0].content_sha256[0] == 42u);
    assert(hits[1].source_change_seq == 0u && hits[1].content_sha256[0] == 42u);
    assert(fabsf(hits[0].cosine - 1.0f) < 1e-5f);
    assert(fabsf(hits[1].cosine - 0.70710678f) < 1e-4f);
    q.required_watermark = 23u;
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_STALE && count == 0u);
    q.required_watermark = 22u; q.model_sha256[0]++;
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_STALE && count == 0u);
    q.model_sha256[0]--; q.allowed_sci = 0u;
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_OK && count == 0u);
    q.allowed_sci = 1u; strcpy(q.workstream, "missing");
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_OK && count == 0u);
    q.workstream[0] = '\0'; q.deadline_ns = now_ns() - 1u;
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_DEADLINE && count == 0u);
    q.deadline_ns = 0u;
    int calls = 0;
    q.visible = fail_second; q.visible_context = &calls;
    assert(keystone_aimem_vector_search(index, &q, a, hits, 4u, &count) == KEYSTONE_AIMEM_VECTOR_INVALID);
    assert(count == 0u && hits[0].uuid[0] == 0u);
    q.visible = visible_owner; q.visible_context = &owner;
    reader_t ctx = {.index = index, .query = q};
    ctx.vector[0] = 2.0f;
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++) assert(pthread_create(&threads[i], NULL, reader, &ctx) == 0);
    for (size_t i = 0; i < 8; i++) assert(pthread_join(threads[i], NULL) == 0);
    keystone_aimem_vector_destroy(index);
    puts("AIMEM vector candidates: filtered cosine, model/watermark and concurrent readers OK");
    return 0;
}
