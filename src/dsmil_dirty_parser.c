#include "../include/dsmil_dirty_parser.h"
#include <ctype.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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

/*
 * SIMD-accelerated candidate scan.
 *
 * The dirty-log tokenizer only ever reacts to three anchor bytes:
 *   '@'  -> begins an email extraction attempt
 *   'h'  -> begins a "http" URL extraction attempt
 *   'H'  -> case-insensitive variant of the above
 *
 * Every other byte is a no-op that simply advances the cursor. Instead of
 * probing those bytes one at a time, we vectorize the search so that long
 * runs of uninteresting bytes are skipped in 32-byte (AVX2) or 16-byte
 * (SSE4.2) strides using _mm256_cmpeq_epi8 / _mm_cmpeq_epi8. The three
 * per-lane equality masks are OR-ed together; __builtin_ctz then yields the
 * first matching lane in constant time.
 *
 * Returns the first index >= start whose byte is one of '@', 'h', 'H', or
 * `length` if no such index exists. The remainder past the last full vector
 * stride is handled by a scalar tail so behavior is identical to the original
 * byte-by-byte scan.
 */
static inline size_t dsmil_dirty_find_next_candidate(const char* buf, size_t length, size_t start) {
#if defined(__x86_64__) || defined(__i386__)
    const unsigned char* p = (const unsigned char*)buf;
    size_t i = start;

#ifdef __AVX2__
    const __m256i v_at = _mm256_set1_epi8((char)'@');
    const __m256i v_h  = _mm256_set1_epi8((char)'h');
    const __m256i v_H  = _mm256_set1_epi8((char)'H');
    while (i + 32 <= length) {
        __m256i v  = _mm256_loadu_si256((const __m256i*)(p + i));
        __m256i m1 = _mm256_cmpeq_epi8(v, v_at);
        __m256i m2 = _mm256_cmpeq_epi8(v, v_h);
        __m256i m3 = _mm256_cmpeq_epi8(v, v_H);
        __m256i m  = _mm256_or_si256(_mm256_or_si256(m1, m2), m3);
        unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
        if (mask) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 32;
    }
#endif /* __AVX2__ */

#ifdef __SSE4_2__
    const __m128i s_at = _mm_set1_epi8((char)'@');
    const __m128i s_h  = _mm_set1_epi8((char)'h');
    const __m128i s_H  = _mm_set1_epi8((char)'H');
    while (i + 16 <= length) {
        __m128i v  = _mm_loadu_si128((const __m128i*)(p + i));
        __m128i m1 = _mm_cmpeq_epi8(v, s_at);
        __m128i m2 = _mm_cmpeq_epi8(v, s_h);
        __m128i m3 = _mm_cmpeq_epi8(v, s_H);
        __m128i m  = _mm_or_si128(_mm_or_si128(m1, m2), m3);
        unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
        if (mask) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 16;
    }
#endif /* __SSE4_2__ */

    /* Scalar tail for the sub-vector remainder */
    for (; i < length; i++) {
        unsigned char c = p[i];
        if (c == '@' || c == 'h' || c == 'H') return i;
    }
    return length;
#else
    /* Non-x86 (e.g. Graviton/Neoverse) scalar fallback */
    for (size_t i = start; i < length; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '@' || c == 'h' || c == 'H') return i;
    }
    return length;
#endif
}

size_t dsmil_dirty_log_ingest(const char* buffer, size_t length, uint64_t base_offset, dsmil_hash_index_t* index) {
    if (!buffer || !index || length == 0) return 0;

    size_t artifacts_found = 0;
    size_t i = 0;

    while (i < length) {
        /* SIMD-skip long runs of bytes that cannot begin an email ('@') or
         * URL ('h'/'H') token. The candidate finder jumps straight to the
         * next interesting byte in 16/32-byte vector strides; the extraction
         * logic below is unchanged and runs only at real candidates. */
        size_t candidate = dsmil_dirty_find_next_candidate(buffer, length, i);
        if (candidate >= length) break;
        i = candidate;

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
                    
                    /* Credential combo extraction: look for email:password pattern */
                    if (end < length && (buffer[end] == ':' || buffer[end] == '|' || buffer[end] == ';')) {
                        size_t pass_start = end + 1;
                        size_t pass_end = pass_start;
                        /* Read until whitespace or next delimiter */
                        while (pass_end < length && 
                               buffer[pass_end] > 32 && 
                               buffer[pass_end] < 127 && 
                               buffer[pass_end] != '|' && 
                               buffer[pass_end] != ';') {
                            pass_end++;
                        }
                        
                        size_t pass_len = pass_end - pass_start;
                        if (pass_len > 0 && pass_len < 255) {
                            char pass_val[256];
                            memcpy(pass_val, buffer + pass_start, pass_len);
                            pass_val[pass_len] = '\0';
                            
                            /* Passwords are case-sensitive; do not normalize them.
                             * Indexing the password allows analysts to search for password re-use
                             * across different breached databases. */
                            dsmil_hash_index_add(index, pass_val, pass_len, base_offset + pass_start);
                            artifacts_found++;
                            end = pass_end;
                        }
                    }
                    
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
