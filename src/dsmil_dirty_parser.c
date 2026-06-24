#include "../include/dsmil_dirty_parser.h"
#include <ctype.h>
#include <string.h>

/* Helper to check if char is valid for an email */
static inline int is_email_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+';
}

/* Helper to check if char is valid URL/domain char */
static inline int is_url_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '-' || c == '/' || c == ':' || c == '_' || c == '?';
}

/* Lowercase a string in place for normalized hashing */
static void normalize_string(char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}

size_t dsmil_dirty_log_ingest(const char* buffer, size_t length, uint64_t base_offset, dsmil_hash_index_t* index) {
    if (!buffer || !index || length == 0) return 0;

    size_t artifacts_found = 0;
    size_t i = 0;

    while (i < length) {
        /* Extremely aggressive, zero-allocation scanner for dirty stealer data */
        
        /* 1. Email extraction heuristic: look for '@' */
        if (buffer[i] == '@') {
            size_t start = i;
            while (start > 0 && is_email_char(buffer[start - 1])) {
                start--;
            }
            size_t end = i;
            while (end < length && is_email_char(buffer[end])) {
                end++;
            }
            
            /* Basic validation: must have chars before and after '@', and a '.' after */
            if (start < i && end > i + 1) {
                int has_dot = 0;
                for (size_t j = i + 1; j < end; j++) {
                    if (buffer[j] == '.') has_dot = 1;
                }
                
                size_t len = end - start;
                if (has_dot && len > 5 && len < 255) {
                    char clean_val[256];
                    memcpy(clean_val, buffer + start, len);
                    clean_val[len] = '\0';
                    normalize_string(clean_val, len);
                    
                    /* Index the cleaned email */
                    dsmil_hash_index_add(index, clean_val, len, base_offset + start);
                    artifacts_found++;
                    i = end;
                    continue;
                }
            }
        }
        
        /* 2. Basic URL / Domain extraction heuristic: look for "http" */
        if (i + 4 < length && 
            (buffer[i] == 'h' || buffer[i] == 'H') &&
            (buffer[i+1] == 't' || buffer[i+1] == 'T') &&
            (buffer[i+2] == 't' || buffer[i+2] == 'T') &&
            (buffer[i+3] == 'p' || buffer[i+3] == 'P')) {
            
            size_t end = i;
            while (end < length && is_url_char(buffer[end])) {
                end++;
            }
            size_t len = end - i;
            if (len > 8 && len < 255) {
                char clean_val[256];
                memcpy(clean_val, buffer + i, len);
                clean_val[len] = '\0';
                normalize_string(clean_val, len);
                
                /* Index the URL/domain */
                dsmil_hash_index_add(index, clean_val, len, base_offset + i);
                artifacts_found++;
                i = end;
                continue;
            }
        }

        /* Move forward */
        i++;
    }

    return artifacts_found;
}

void dsmil_extract_clean_context(const char* archive_buffer, size_t archive_length, uint64_t offset, char* out_clean_line, size_t out_size) {
    if (!archive_buffer || !out_clean_line || out_size == 0 || offset >= archive_length) {
        if (out_clean_line && out_size > 0) out_clean_line[0] = '\0';
        return;
    }

    /* Find the start of the line */
    uint64_t start = offset;
    while (start > 0 && archive_buffer[start - 1] != '\n' && archive_buffer[start - 1] != '\r' && (offset - start < 100)) {
        start--;
    }

    /* Find the end of the line */
    uint64_t end = offset;
    while (end < archive_length && archive_buffer[end] != '\n' && archive_buffer[end] != '\r' && (end - offset < 200)) {
        end++;
    }

    size_t len = end - start;
    if (len >= out_size) {
        len = out_size - 1;
    }

    /* Clean the output: strip non-printable characters often found in dirty dumps */
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)archive_buffer[start + i];
        if (c >= 32 && c < 127) {
            out_clean_line[w++] = c;
        } else {
            out_clean_line[w++] = ' '; /* Replace garbage with spaces */
        }
    }
    out_clean_line[w] = '\0';
}
