/* Candidate-only, immutable AIMEM vector generation. QIHSE remains the
 * authoritative source for scope grants, current labels and record contents. */
#ifndef KEYSTONE_AIMEM_VECTOR_H
#define KEYSTONE_AIMEM_VECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYSTONE_AIMEM_VECTOR_ABI_VERSION 1u
#define KEYSTONE_AIMEM_VECTOR_MAX_DIM 1024u
#define KEYSTONE_AIMEM_SCOPE_MAX 63u

typedef struct keystone_aimem_vector_index keystone_aimem_vector_index_t;

typedef enum {
    KEYSTONE_AIMEM_VECTOR_OK = 0,
    KEYSTONE_AIMEM_VECTOR_INVALID = -1,
    KEYSTONE_AIMEM_VECTOR_NOMEM = -2,
    KEYSTONE_AIMEM_VECTOR_FULL = -3,
    KEYSTONE_AIMEM_VECTOR_STALE = -4,
    KEYSTONE_AIMEM_VECTOR_DEADLINE = -5
} keystone_aimem_vector_status_t;

typedef struct {
    uint8_t uuid[16];
    uint64_t source_change_seq; /* May be zero for pre-sequence records. */
    uint8_t content_sha256[32];
    uint32_t tenant;
    uint32_t owner;
    uint32_t kind;
    uint16_t classification;
    uint16_t sci;
    char project[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    char worktree[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    char workstream[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    char session[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
} keystone_aimem_vector_entry_t;

typedef struct {
    uint8_t uuid[16];
    uint64_t source_change_seq; /* Canonical revision to recheck before disclosure. */
    uint8_t content_sha256[32]; /* Covers pre-sequence records as well. */
    float cosine;
} keystone_aimem_vector_hit_t;

/* Called before scoring. It must check live owner/project grants; QIHSE must
 * re-read/re-authorize every returned candidate before disclosure. Never call
 * the callback while holding an index lock (sealed generations are read-only). */
typedef int (*keystone_aimem_vector_visible_fn)(
    const keystone_aimem_vector_entry_t *candidate, void *context);

typedef struct {
    uint32_t tenant;
    uint32_t kind;             /* 0 = any */
    uint16_t max_classification;
    uint16_t allowed_sci;      /* candidate SCI must be a subset */
    char project[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    char worktree[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    char workstream[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    char session[KEYSTONE_AIMEM_SCOPE_MAX + 1u];
    uint8_t model_sha256[32];
    uint64_t required_watermark;
    uint64_t deadline_ns;     /* CLOCK_MONOTONIC absolute; 0 = no deadline */
    keystone_aimem_vector_visible_fn visible;
    void *visible_context;
} keystone_aimem_vector_query_t;

/* Dynamic consumers must resolve this symbol and check the exact ABI before
 * passing any structs or function pointers across the shared-library boundary. */
uint32_t keystone_aimem_vector_abi_version(void);

/* Build privately, then seal exactly one complete source watermark before
 * sharing with concurrent query threads. No writes are permitted after seal.
 * max_entries is an explicit memory bound; model_sha256 binds full preprocessing
 * and weights, not merely the provider name. */
keystone_aimem_vector_index_t *keystone_aimem_vector_create(
    size_t dimension, size_t max_entries, const uint8_t model_sha256[32]);
keystone_aimem_vector_status_t keystone_aimem_vector_add(
    keystone_aimem_vector_index_t *index,
    const keystone_aimem_vector_entry_t *entry, const float *embedding);
keystone_aimem_vector_status_t keystone_aimem_vector_seal(
    keystone_aimem_vector_index_t *index, uint64_t watermark);
keystone_aimem_vector_status_t keystone_aimem_vector_search(
    const keystone_aimem_vector_index_t *index,
    const keystone_aimem_vector_query_t *query, const float *embedding,
    keystone_aimem_vector_hit_t *out, size_t capacity, size_t *out_count);
size_t keystone_aimem_vector_count(const keystone_aimem_vector_index_t *index);
void keystone_aimem_vector_destroy(keystone_aimem_vector_index_t *index);

#ifdef __cplusplus
}
#endif
#endif
