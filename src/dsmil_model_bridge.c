#include "../include/dsmil_model_bridge.h"
#include <string.h>

/* Cleans non-printable garbage to ensure the model doesn't choke on binary blobs */
static void clean_model_buffer(char* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 32 || c > 126) {
            buf[i] = ' '; /* Replace with space to maintain token boundaries */
        }
    }
}

void dsmil_extract_model_context(const char* archive_buffer, size_t archive_length, 
                                 uint64_t hit_offset, size_t artifact_len, 
                                 dsmil_model_context_t* out_context) {
    if (!archive_buffer || !out_context || hit_offset >= archive_length) {
        memset(out_context, 0, sizeof(dsmil_model_context_t));
        return;
    }

    memset(out_context, 0, sizeof(dsmil_model_context_t));
    out_context->target_offset = hit_offset;

    /* Extract the exact artifact that caused the hit */
    size_t copy_len = artifact_len;
    if (copy_len > sizeof(out_context->target_artifact) - 1) {
        copy_len = sizeof(out_context->target_artifact) - 1;
    }
    
    /* Ensure we don't read past archive end */
    if (hit_offset + copy_len > archive_length) {
        copy_len = archive_length - hit_offset;
    }
    
    memcpy(out_context->target_artifact, archive_buffer + hit_offset, copy_len);
    out_context->target_artifact[copy_len] = '\0';
    clean_model_buffer(out_context->target_artifact, copy_len);

    /* --- EXTRACT PRE-CONTEXT --- */
    size_t pre_size = sizeof(out_context->pre_context) - 1;
    uint64_t pre_start = 0;
    
    if (hit_offset > pre_size) {
        pre_start = hit_offset - pre_size;
    } else {
        pre_start = 0;
        pre_size = (size_t)hit_offset;
        out_context->is_truncated = 1;
    }
    
    if (pre_size > 0) {
        memcpy(out_context->pre_context, archive_buffer + pre_start, pre_size);
        out_context->pre_context[pre_size] = '\0';
        clean_model_buffer(out_context->pre_context, pre_size);
    }

    /* --- EXTRACT POST-CONTEXT --- */
    size_t post_size = sizeof(out_context->post_context) - 1;
    uint64_t post_start = hit_offset + artifact_len;
    
    if (post_start < archive_length) {
        uint64_t remaining = archive_length - post_start;
        if (remaining < post_size) {
            post_size = (size_t)remaining;
            out_context->is_truncated = 1;
        }
        memcpy(out_context->post_context, archive_buffer + post_start, post_size);
        out_context->post_context[post_size] = '\0';
        clean_model_buffer(out_context->post_context, post_size);
    } else {
        out_context->is_truncated = 1;
    }
}
