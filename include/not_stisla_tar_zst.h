/**
 * NOT_STISLA - tar.zst streaming search interface
 *
 * Provides live streaming decompression and search over .tar.zst archives
 * without fully materialising the archive in memory.
 */

#ifndef NOT_STISLA_TAR_ZST_H
#define NOT_STISLA_TAR_ZST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations from not_stisla.h */
typedef size_t not_stisla_result_t;
typedef struct not_stisla_anchor_table not_stisla_anchor_table_t;
typedef struct not_stisla_config not_stisla_config_t;
typedef struct not_stisla_batch_item not_stisla_batch_item_t;
typedef struct not_stisla_parallel_config not_stisla_parallel_config_t;
#define NOT_STISLA_NOT_FOUND ((not_stisla_result_t)-1)

/* ============================================================================
 * Options & Types
 * ============================================================================ */

typedef enum not_stisla_tar_zst_format {
    NOT_STISLA_TAR_ZST_FORMAT_AUTO = 0,
    NOT_STISLA_TAR_ZST_FORMAT_CSV,
    NOT_STISLA_TAR_ZST_FORMAT_JSON,
    NOT_STISLA_TAR_ZST_FORMAT_TEXT
} not_stisla_tar_zst_format_t;

typedef struct not_stisla_tar_zst_options {
    not_stisla_tar_zst_format_t format;
    int zstd_workers;       /* >1 enables multi-threaded zstd decompression */
    int enable_pipeline;    /* non-zero enables producer/consumer pipelining */
    size_t chunk_size;      /* I/O chunk size (default 256 KiB) */
    size_t arena_slab_size; /* Arena slab size (default 1 MiB) */
    int skip_header;        /* For CSV: skip first line as header */
} not_stisla_tar_zst_options_t;

typedef struct not_stisla_tar_zst not_stisla_tar_zst_t;

/* ============================================================================
 * Core Archive API
 * ============================================================================ */

/**
 * @brief Open a .tar.zst archive for streaming search.
 *
 * @param path Path to the archive file.
 * @param opts Options; may be NULL for defaults.
 * @return Handle or NULL on error.
 */
not_stisla_tar_zst_t* not_stisla_tar_zst_open(const char* path,
                                              const not_stisla_tar_zst_options_t* opts);

/**
 * @brief Close archive and release all resources.
 */
void not_stisla_tar_zst_close(not_stisla_tar_zst_t* tz);

/**
 * @brief Advance to the next archive member.
 *
 * @param tz Archive handle.
 * @param out_name Receives pointer to member name (valid until next call).
 * @param out_name_len Receives length of member name.
 * @return 1 if a member was found, 0 at end-of-archive, <0 on error.
 */
int not_stisla_tar_zst_next_member(not_stisla_tar_zst_t* tz,
                                   char** out_name,
                                   size_t* out_name_len);

/**
 * @brief Search a named member by streaming, parsing, then NOT_STISLA search.
 *
 * Streams the named member, parses into an int64_t buffer via the arena,
 * then calls not_stisla_search_enhanced. The buffer is reset after search.
 *
 * @param tz Archive handle.
 * @param member_name Name of the tar entry to search.
 * @param key Key to search for.
 * @param table Anchor table (may be NULL).
 * @param config Search config (may be NULL for defaults).
 * @return Search result or NOT_STISLA_NOT_FOUND.
 */
not_stisla_result_t not_stisla_tar_zst_search_member(
    not_stisla_tar_zst_t* tz,
    const char* member_name,
    int64_t key,
    not_stisla_anchor_table_t* table,
    const not_stisla_config_t* config
);

/**
 * @brief Batch search a named member.
 *
 * Streams the member once, parses into int64_t buffer, then runs
 * not_stisla_search_batch_auto. The buffer is reset after search.
 */
size_t not_stisla_tar_zst_search_member_batch(
    not_stisla_tar_zst_t* tz,
    const char* member_name,
    not_stisla_batch_item_t* items,
    size_t num_items,
    not_stisla_anchor_table_t* table,
    size_t tol,
    const not_stisla_parallel_config_t* config
);

/**
 * @brief Extract parsed int64_t keys from a named member.
 *
 * Streams and parses the member.  The returned keys pointer is owned by
 * the arena and remains valid until the next parse operation or close.
 *
 * @param tz Archive handle.
 * @param member_name Name of the tar entry to extract.
 * @param out_keys Receives pointer to the parsed key array.
 * @param out_count Receives number of keys.
 * @return 0 on success, <0 on error.
 */
int not_stisla_tar_zst_extract_member(
    not_stisla_tar_zst_t* tz,
    const char* member_name,
    int64_t** out_keys,
    size_t* out_count
);

/* ============================================================================
 * Offset Index API
 * ============================================================================ */

/**
 * @brief Build an in-memory member offset index by scanning the archive once.
 *
 * After this call the handle is rewound; subsequent search-by-name calls
 * can jump directly to indexed members.
 *
 * @return 0 on success, <0 on error.
 */
int not_stisla_tar_zst_build_index(not_stisla_tar_zst_t* tz);

/**
 * @brief Search using the pre-built offset index (faster for repeated access).
 */
not_stisla_result_t not_stisla_tar_zst_search_indexed(
    not_stisla_tar_zst_t* tz,
    const char* member_name,
    int64_t key,
    not_stisla_anchor_table_t* table,
    const not_stisla_config_t* config
);

/* ============================================================================
 * Error & Stats Helpers
 * ============================================================================ */

/**
 * @brief Get a human-readable error string for the last operation.
 */
const char* not_stisla_tar_zst_error_string(not_stisla_tar_zst_t* tz);

/**
 * @brief Get archive-level statistics.
 */
typedef struct not_stisla_tar_zst_stats {
    uint64_t members_read;
    uint64_t bytes_read;
    uint64_t decompress_time_ns;
    uint64_t parse_time_ns;
} not_stisla_tar_zst_stats_t;

int not_stisla_tar_zst_get_stats(not_stisla_tar_zst_t* tz,
                                 not_stisla_tar_zst_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif /* NOT_STISLA_TAR_ZST_H */
