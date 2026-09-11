#ifndef DSMIL_HASH_INDEXER_H
#define DSMIL_HASH_INDEXER_H

#include "keystone.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Columnar Hash Index for heterogeneous logs (e.g. JSON, unstructured text).
 * Maps arbitrary hashed string identifiers to uncompressed byte offsets.
 *
 * The 64-bit FNV-1a hash is used as a KEYSTONE accelerator (fast sorted-array
 * lookup), but the original string bytes are retained and verified on every
 * positive hit to eliminate false matches from hash collisions.
 */
typedef struct dsmil_hash_index {
    int64_t* hashes;       /* Contiguous array for KEYSTONE SIMD searches */
    uint64_t* offsets;     /* Parallel array for payload byte offsets */
    /* --- Collision verification: retained source strings --- */
    char** strings;        /* Parallel array of NUL-terminated string copies */
    size_t* string_lens;   /* Parallel array of string lengths */
    size_t count;
    size_t capacity;
    keystone_anchor_table_t* anchor_table;
    int is_sorted;
} dsmil_hash_index_t;

/**
 * @brief Initialize a new heterogeneous log index.
 */
dsmil_hash_index_t* dsmil_hash_index_create(size_t initial_capacity);

/**
 * @brief Destroy the index and its buffers.
 */
void dsmil_hash_index_destroy(dsmil_hash_index_t* idx);

/**
 * @brief Hash a string (e.g. email, IP, username) and record its byte offset.
 */
int dsmil_hash_index_add(dsmil_hash_index_t* idx, const char* str, size_t len, uint64_t byte_offset);

/**
 * @brief Lock and sort the index to prepare for KEYSTONE acceleration.
 */
int dsmil_hash_index_finalize(dsmil_hash_index_t* idx);

/**
 * @brief Execute a sub-logarithmic search for the target string.
 *
 * The 64-bit hash is used as a KEYSTONE accelerator.  On a positive hash
 * match, the original string bytes are compared to eliminate false matches
 * from hash collisions.
 *
 * @return KEYSTONE_NOT_FOUND if absent, or the index ordinal on success.
 */
keystone_result_t dsmil_hash_index_search(dsmil_hash_index_t* idx, const char* query_str, uint64_t* out_offset);

/**
 * @brief Search for all entries matching the target string.
 *
 * Like dsmil_hash_index_search, but returns ALL matching offsets (doc IDs)
 * rather than just the first.  Caller provides a buffer and receives the
 * count of matches found.
 *
 * @param idx       Finalized hash index.
 * @param query_str String to search for.
 * @param out_offsets Caller-provided buffer for results.
 * @param max_offsets Size of out_offsets buffer.
 * @param out_count Number of matches written to out_offsets.
 * @return KEYSTONE_OK on success (including zero matches), KEYSTONE_NOT_FOUND on error.
 */
keystone_result_t dsmil_hash_index_search_all(
    dsmil_hash_index_t* idx,
    const char* query_str,
    uint64_t* out_offsets,
    size_t max_offsets,
    size_t* out_count);

/**
 * @brief Serialize a finalized hash index to a file.
 *
 * @param idx  Finalized hash index.
 * @param path Output file path.
 * @return 0 on success, -1 on error.
 */
int dsmil_hash_index_save(dsmil_hash_index_t* idx, const char* path);

/**
 * @brief Deserialize a hash index from a file.
 *
 * @param path Input file path.
 * @return Pointer to a new hash index, or NULL on error.
 */
dsmil_hash_index_t* dsmil_hash_index_load(const char* path);

#ifdef __cplusplus
}
#endif
#endif
