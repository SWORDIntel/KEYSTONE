#ifndef DSMIL_DIRTY_PARSER_H
#define DSMIL_DIRTY_PARSER_H

#include "dsmil_hash_indexer.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Represents a single extracted and cleaned artifact from dirty logs.
 */
typedef struct {
    char type[16];        /* e.g., "EMAIL", "URL", "CREDS" */
    char value[256];      /* The cleaned extracted value */
    uint64_t byte_offset; /* Where it was found in the raw stream */
} dsmil_extracted_artifact_t;

/**
 * @brief Parse a dirty log buffer, extract artifacts, and insert them into the hash index.
 * 
 * @param buffer The raw, unstructured log buffer (e.g., from a stealer log archive).
 * @param length The length of the buffer.
 * @param base_offset The absolute byte offset of the start of this buffer in the overall archive.
 * @param index The hash indexer to populate.
 * @return The number of artifacts extracted and indexed.
 */
size_t dsmil_dirty_log_ingest(const char* buffer, size_t length, uint64_t base_offset, dsmil_hash_index_t* index);

/**
 * @brief Helper to reconstruct/clean the context around a given byte offset.
 * Useful for displaying the "clean output" after a search hit.
 * 
 * @param archive_buffer The memory-mapped or loaded archive buffer.
 * @param archive_length Total length.
 * @param offset The byte offset found via index search.
 * @param out_clean_line Buffer to hold the sanitized/extracted line context.
 * @param out_size Size of the output buffer.
 */
void dsmil_extract_clean_context(const char* archive_buffer, size_t archive_length, uint64_t offset, char* out_clean_line, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif
