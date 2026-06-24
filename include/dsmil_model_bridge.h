#ifndef DSMIL_MODEL_BRIDGE_H
#define DSMIL_MODEL_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The context window perfectly shaped for a small ML micro-model (e.g., local BERT, 
 *        regex-heuristic engine, or a tiny LLM inference node) to run semantic extraction.
 */
typedef struct {
    uint64_t target_offset;     /* The exact byte offset of the anchor (e.g., the email) */
    char pre_context[256];      /* Up to 256 bytes of text immediately preceding the hit */
    char post_context[256];     /* Up to 256 bytes of text immediately following the hit */
    char target_artifact[128];  /* The actual string that triggered the hit */
    int is_truncated;           /* 1 if the log boundaries were reached before filling context */
} dsmil_model_context_t;

/**
 * @brief Rips a perfectly sized context window out of the raw stealer log archive, 
 *        bypassing the need to stream the whole archive. This window is what you 
 *        feed to your Python/ONNX inference micro-model to classify surrounding tokens.
 * 
 * @param archive_buffer Memory-mapped or loaded raw archive.
 * @param archive_length Total length of the archive.
 * @param hit_offset The exact byte offset of the artifact found by KEYSTONE.
 * @param artifact_len The length of the artifact.
 * @param out_context The populated context struct ready for inference.
 */
void dsmil_extract_model_context(const char* archive_buffer, size_t archive_length, 
                                 uint64_t hit_offset, size_t artifact_len, 
                                 dsmil_model_context_t* out_context);

#ifdef __cplusplus
}
#endif
#endif
