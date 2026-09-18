/**
 * tgrep: High-Performance Trigram-Accelerated CLI Code Search
 *
 * Built directly on KEYSTONE's 24-bit direct-directory trigram indexing engine.
 * Supports blazing-fast ad-hoc searches, persistent binary index caching (-I),
 * pre-built index creation (--build-index), multi-threaded parallel ingestion,
 * and SIMD candidate verification.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "keystone_trigram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define KS_X86 1
#else
#define KS_X86 0
#endif

#define TGREP_VERSION "1.0.0"
#define DEFAULT_MAX_FILESIZE_MB 32
#define BINARY_PROBE_BYTES 1024

/* ANSI color escape sequences */
static const char* ANSI_RESET       = "\033[0m";
static const char* ANSI_BOLD_RED    = "\033[1;31m";
static const char* ANSI_MAGENTA     = "\033[35m";
static const char* ANSI_GREEN       = "\033[32m";
static const char* ANSI_CYAN        = "\033[36m";
static const char* ANSI_BOLD_CYAN   = "\033[1;36m";

/* Fast branchless ASCII lowercase folding */
static inline unsigned char fast_tolower_u8(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32u) : c;
}

/* Fast substring match (case-sensitive or case-insensitive) */
static const char* find_substring(
    const char* haystack,
    size_t haystack_len,
    const char* needle,
    size_t needle_len,
    bool ci
) {
    if (!haystack || !needle || needle_len == 0u || haystack_len < needle_len) {
        return NULL;
    }

    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    size_t limit = haystack_len - needle_len;

    if (!ci) {
        if (needle_len == 1u) {
            return (const char*)memchr(haystack, (int)n[0], haystack_len);
        }
#if KS_X86 && defined(__SSE4_2__)
        __m128i first_byte = _mm_set1_epi8((char)n[0]);
        __m128i last_byte  = _mm_set1_epi8((char)n[needle_len - 1u]);
        size_t i = 0u;
        while (i + 16u <= limit) {
            __m128i chunk_f = _mm_loadu_si128((const __m128i*)(h + i));
            __m128i chunk_l = _mm_loadu_si128((const __m128i*)(h + i + needle_len - 1u));
            unsigned int mask_f = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk_f, first_byte));
            unsigned int mask_l = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk_l, last_byte));
            unsigned int mask = mask_f & mask_l;
            while (mask) {
                int bit = __builtin_ctz(mask);
                mask &= mask - 1;
                size_t pos = i + (size_t)bit;
                if (pos <= limit && memcmp(h + pos, n, needle_len) == 0) {
                    return (const char*)(h + pos);
                }
            }
            i += 16u;
        }
        for (; i <= limit; i++) {
            if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0) {
                return (const char*)(h + i);
            }
        }
        return NULL;
#else
        for (size_t i = 0u; i <= limit; i++) {
            if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0) {
                return (const char*)(h + i);
            }
        }
        return NULL;
#endif
    } else {
        /* Case-insensitive matching */
        unsigned char n0_low = fast_tolower_u8(n[0]);
        unsigned char n0_up  = (n0_low >= 'a' && n0_low <= 'z') ? (unsigned char)(n0_low - 32u) : n0_low;

        for (size_t i = 0u; i <= limit; i++) {
            unsigned char hc = h[i];
            if (hc == n0_low || hc == n0_up) {
                bool match = true;
                for (size_t j = 1u; j < needle_len; j++) {
                    if (fast_tolower_u8(h[i + j]) != fast_tolower_u8(n[j])) {
                        match = false;
                        break;
                    }
                }
                if (match) return (const char*)(h + i);
            }
        }
        return NULL;
    }
}

/* File list container */
typedef struct {
    char** paths;
    size_t count;
    size_t capacity;
} file_list_t;

static void file_list_init(file_list_t* list) {
    list->paths = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

static void file_list_push(file_list_t* list, const char* path) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity ? (list->capacity * 2u) : 256u;
        char** new_paths = (char**)realloc(list->paths, new_cap * sizeof(char*));
        if (!new_paths) return;
        list->paths = new_paths;
        list->capacity = new_cap;
    }
    list->paths[list->count++] = strdup(path);
}

static void file_list_free(file_list_t* list) {
    if (!list) return;
    for (size_t i = 0u; i < list->count; i++) {
        free(list->paths[i]);
    }
    free(list->paths);
    list->paths = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

/* Common excluded directories */
static bool is_excluded_dir(const char* name) {
    if (name[0] == '.') return true;
    if (strcmp(name, "node_modules") == 0) return true;
    if (strcmp(name, "target") == 0) return true;
    if (strcmp(name, "build") == 0) return true;
    if (strcmp(name, "dist") == 0) return true;
    if (strcmp(name, "__pycache__") == 0) return true;
    if (strcmp(name, "venv") == 0) return true;
    if (strcmp(name, ".venv") == 0) return true;
    return false;
}

/* Fast check if a file is binary */
static bool is_binary_file(int fd) {
    char probe[BINARY_PROBE_BYTES];
    ssize_t n = pread(fd, probe, sizeof(probe), 0);
    if (n <= 0) return false;
    return (memchr(probe, '\0', (size_t)n) != NULL);
}

/* Recursive directory walker */
static void scan_path_recursive(
    const char* path,
    file_list_t* list,
    size_t max_bytes
) {
    struct stat st;
    if (lstat(path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return;

        struct dirent* entry;
        char subpath[PATH_MAX];
        size_t plen = strlen(path);
        while (plen > 1 && path[plen - 1] == '/') plen--;

        while ((entry = readdir(dir)) != NULL) {
            const char* dname = entry->d_name;
            if (strcmp(dname, ".") == 0 || strcmp(dname, "..") == 0) continue;

            if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) {
                if (is_excluded_dir(dname)) continue;
            }

            int n = 0;
            if (plen == 1 && path[0] == '.') {
                n = snprintf(subpath, sizeof(subpath), "%s", dname);
            } else {
                n = snprintf(subpath, sizeof(subpath), "%.*s/%s", (int)plen, path, dname);
            }
            if (n <= 0 || (size_t)n >= sizeof(subpath)) continue;

            struct stat sub_st;
            if (lstat(subpath, &sub_st) != 0) continue;

            if (S_ISDIR(sub_st.st_mode)) {
                if (!is_excluded_dir(dname)) {
                    scan_path_recursive(subpath, list, max_bytes);
                }
            } else if (S_ISREG(sub_st.st_mode)) {
                if (dname[0] == '.') continue; /* skip hidden files by default */
                if (sub_st.st_size > 0 && (size_t)sub_st.st_size <= max_bytes) {
                    file_list_push(list, subpath);
                }
            }
        }
        closedir(dir);
    } else if (S_ISREG(st.st_mode)) {
        if (st.st_size > 0 && (size_t)st.st_size <= max_bytes) {
            file_list_push(list, path);
        }
    }
}

/* High-resolution timer helper */
static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Program options */
typedef struct {
    bool ignore_case;
    bool show_line_num;
    bool files_with_matches;
    bool count_only;
    bool show_filename;
    bool use_color;
    bool show_stats;
    int threads;
    size_t max_filesize_mb;
    const char* index_file;
    const char* build_index_file;
    const char* pattern;
    int path_count;
    char** paths;
} tgrep_options_t;

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS] <PATTERN> [PATH...]\n", prog);
    printf("       %s --build-index <INDEX_FILE> [PATH...]\n\n", prog);
    printf("Fast, trigram-accelerated code and text search powered by KEYSTONE.\n\n");
    printf("Arguments:\n");
    printf("  <PATTERN>             Text string to search for\n");
    printf("  [PATH...]             Files or directories to search (default: \".\")\n\n");
    printf("Options:\n");
    printf("  -i, --ignore-case     Case-insensitive matching\n");
    printf("  -n, --line-number     Show 1-based line numbers (default: true if TTY)\n");
    printf("  -N, --no-line-number  Suppress line numbers\n");
    printf("  -l, --files-with-matches Print only names of matching files\n");
    printf("  -c, --count           Print only match count per file\n");
    printf("  -j, --threads <N>     Worker thread count (default: half host cores)\n");
    printf("  -I, --index <FILE>    Load or cache persistent index (.tgrep.idx)\n");
    printf("      --build-index <F> Build index over PATH... and exit\n");
    printf("      --stats           Display search and indexing telemetry\n");
    printf("      --color[=WHEN]    Output colors: 'always', 'never', or 'auto' (default)\n");
    printf("      --max-filesize <M> Max file size in MB to index (default: %d MB)\n", DEFAULT_MAX_FILESIZE_MB);
    printf("  -H, --with-filename   Always print file path with matches\n");
    printf("      --no-filename     Never print file path with matches\n");
    printf("  -h, --help            Display this help and exit\n");
    printf("  -v, --version         Display version information and exit\n");
}

static void print_matched_line(
    const char* filename,
    size_t line_no,
    const char* line,
    size_t line_len,
    const char* pattern,
    size_t pattern_len,
    bool ci,
    bool show_line_num,
    bool show_filename,
    bool color
) {
    if (color) {
        if (show_filename) {
            printf("%s%s%s%s:%s", ANSI_MAGENTA, filename, ANSI_RESET, ANSI_CYAN, ANSI_RESET);
        }
        if (show_line_num) {
            printf("%s%zu%s%s:%s", ANSI_GREEN, line_no, ANSI_RESET, ANSI_CYAN, ANSI_RESET);
        }

        const char* p = line;
        const char* end = line + line_len;
        while (p < end) {
            const char* m = find_substring(p, (size_t)(end - p), pattern, pattern_len, ci);
            if (!m) {
                fwrite(p, 1, (size_t)(end - p), stdout);
                break;
            }
            if (m > p) {
                fwrite(p, 1, (size_t)(m - p), stdout);
            }
            printf("%s", ANSI_BOLD_RED);
            fwrite(m, 1, pattern_len, stdout);
            printf("%s", ANSI_RESET);
            p = m + pattern_len;
        }
        fputc('\n', stdout);
    } else {
        if (show_filename) {
            printf("%s:", filename);
        }
        if (show_line_num) {
            printf("%zu:", line_no);
        }
        fwrite(line, 1, line_len, stdout);
        fputc('\n', stdout);
    }
}

int main(int argc, char** argv) {
    tgrep_options_t opt;
    memset(&opt, 0, sizeof(opt));

    /* Auto-detect cores: default threads = host_cores / 2 per user rules */
    int host_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (host_cores < 1) host_cores = 1;
    opt.threads = host_cores / 2;
    if (opt.threads < 1) opt.threads = 1;

    opt.max_filesize_mb = DEFAULT_MAX_FILESIZE_MB;
    bool is_tty = isatty(STDOUT_FILENO);
    opt.use_color = is_tty;
    opt.show_line_num = is_tty;
    opt.show_filename = true;

    static const struct option long_options[] = {
        {"ignore-case",        no_argument,       0, 'i'},
        {"line-number",        no_argument,       0, 'n'},
        {"no-line-number",     no_argument,       0, 'N'},
        {"files-with-matches", no_argument,       0, 'l'},
        {"count",              no_argument,       0, 'c'},
        {"threads",            required_argument, 0, 'j'},
        {"index",              required_argument, 0, 'I'},
        {"build-index",        required_argument, 0, 1001},
        {"stats",              no_argument,       0, 1002},
        {"color",              optional_argument, 0, 1003},
        {"max-filesize",       required_argument, 0, 1004},
        {"with-filename",      no_argument,       0, 'H'},
        {"no-filename",        no_argument,       0, 1005},
        {"help",               no_argument,       0, 'h'},
        {"version",            no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "inNlcj:I:Hhv", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                opt.ignore_case = true;
                break;
            case 'n':
                opt.show_line_num = true;
                break;
            case 'N':
                opt.show_line_num = false;
                break;
            case 'l':
                opt.files_with_matches = true;
                break;
            case 'c':
                opt.count_only = true;
                break;
            case 'j':
                opt.threads = atoi(optarg);
                if (opt.threads < 1) opt.threads = 1;
                break;
            case 'I':
                opt.index_file = optarg;
                break;
            case 1001: /* --build-index */
                opt.build_index_file = optarg;
                break;
            case 1002: /* --stats */
                opt.show_stats = true;
                break;
            case 1003: /* --color */
                if (!optarg || strcmp(optarg, "auto") == 0) {
                    opt.use_color = is_tty;
                } else if (strcmp(optarg, "always") == 0) {
                    opt.use_color = true;
                } else if (strcmp(optarg, "never") == 0) {
                    opt.use_color = false;
                }
                break;
            case 1004: /* --max-filesize */
                opt.max_filesize_mb = (size_t)atoi(optarg);
                if (opt.max_filesize_mb < 1) opt.max_filesize_mb = 1;
                break;
            case 'H':
                opt.show_filename = true;
                break;
            case 1005: /* --no-filename */
                opt.show_filename = false;
                break;
            case 'v':
                printf("tgrep v%s (KEYSTONE C11/SIMD 24-bit Trigram Engine)\n", TGREP_VERSION);
                return 0;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

#ifdef _OPENMP
    omp_set_num_threads(opt.threads);
#endif

    /* Positional argument handling */
    if (opt.build_index_file) {
        /* Index build mode: remaining args are paths */
        opt.path_count = argc - optind;
        opt.paths = &argv[optind];
    } else {
        /* Search mode: first arg is pattern, remaining args are paths */
        if (optind >= argc) {
            fprintf(stderr, "Error: missing search PATTERN\n\n");
            print_usage(argv[0]);
            return 1;
        }
        opt.pattern = argv[optind++];
        opt.path_count = argc - optind;
        opt.paths = &argv[optind];
    }

    /* Default path to "." if none provided */
    char* default_path = ".";
    if (opt.path_count == 0) {
        opt.path_count = 1;
        opt.paths = &default_path;
    }

    size_t max_bytes = opt.max_filesize_mb * 1024u * 1024u;
    keystone_trigram_index_t* idx = NULL;
    double t_load_start = get_time_sec();
    double t_load_end = t_load_start;
    double t_build_start = 0.0;
    double t_build_end = 0.0;
    size_t total_bytes_read = 0;

    /* Check if loading from existing index file */
    bool loaded_from_index = false;
    if (opt.index_file && !opt.build_index_file) {
        if (access(opt.index_file, R_OK) == 0) {
            t_load_start = get_time_sec();
            idx = keystone_trigram_index_load(opt.index_file);
            t_load_end = get_time_sec();
            if (idx) {
                loaded_from_index = true;
            } else {
                fprintf(stderr, "Warning: failed to load index '%s', rebuilding...\n", opt.index_file);
            }
        }
    }

    double t_scan_start = 0.0;
    double t_scan_end = 0.0;

    /* If not loaded from index, scan and ingest */
    if (!idx) {
        file_list_t flist;
        file_list_init(&flist);

        t_scan_start = get_time_sec();
        for (int p = 0; p < opt.path_count; p++) {
            scan_path_recursive(opt.paths[p], &flist, max_bytes);
        }
        t_scan_end = get_time_sec();

        if (flist.count == 0) {
            file_list_free(&flist);
            return 0;
        }

        /* Parallel document loading */
        keystone_input_document_t* docs = (keystone_input_document_t*)calloc(
            flist.count, sizeof(keystone_input_document_t)
        );
        if (!docs) {
            fprintf(stderr, "Error: out of memory allocating document metadata\n");
            file_list_free(&flist);
            return 1;
        }

        size_t valid_docs = 0;

#if defined(_OPENMP)
        #pragma omp parallel for schedule(dynamic, 64) reduction(+:total_bytes_read)
#endif
        for (size_t i = 0u; i < flist.count; i++) {
            int fd = open(flist.paths[i], O_RDONLY);
            if (fd < 0) continue;

            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max_bytes) {
                close(fd);
                continue;
            }

            if (is_binary_file(fd)) {
                close(fd);
                continue;
            }

            char* buf = (char*)malloc((size_t)st.st_size + 1u);
            if (!buf) {
                close(fd);
                continue;
            }

            ssize_t total = 0;
            while (total < st.st_size) {
                ssize_t rd = read(fd, buf + total, (size_t)(st.st_size - total));
                if (rd <= 0) break;
                total += rd;
            }
            close(fd);

            buf[total] = '\0';
            docs[i].name = flist.paths[i];
            docs[i].text = buf;
            docs[i].text_len = (size_t)total;
            docs[i].owns_content = 1;
            total_bytes_read += (size_t)total;
        }

        /* Compact array to retain only valid docs */
        for (size_t i = 0u; i < flist.count; i++) {
            if (docs[i].text != NULL) {
                if (valid_docs != i) {
                    docs[valid_docs] = docs[i];
                }
                valid_docs++;
            }
        }

        if (valid_docs == 0) {
            free(docs);
            file_list_free(&flist);
            return 0;
        }

        /* Build index with direct 24-bit directory and case folding */
        t_build_start = get_time_sec();
        idx = keystone_trigram_index_create_options(
            valid_docs,
            KEYSTONE_TRIGRAM_OPT_DIRECT_DIRECTORY | KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE
        );
        if (!idx) {
            fprintf(stderr, "Error: failed to allocate trigram index\n");
            for (size_t i = 0u; i < valid_docs; i++) free((void*)docs[i].text);
            free(docs);
            file_list_free(&flist);
            return 1;
        }

        int rc = keystone_trigram_index_build_parallel(idx, docs, valid_docs, (unsigned)opt.threads);
        t_build_end = get_time_sec();

        /* Free temporary read buffers now that index owns content copies */
        for (size_t i = 0u; i < valid_docs; i++) {
            free((void*)docs[i].text);
        }
        free(docs);
        file_list_free(&flist);

        if (rc != KEYSTONE_TRIGRAM_OK) {
            fprintf(stderr, "Error: index construction failed with code %d\n", rc);
            keystone_trigram_index_destroy(idx);
            return 1;
        }

        /* Save index if requested */
        const char* save_target = opt.build_index_file ? opt.build_index_file : opt.index_file;
        if (save_target) {
            int src = keystone_trigram_index_save(idx, save_target);
            if (src != KEYSTONE_TRIGRAM_OK) {
                fprintf(stderr, "Error: failed to save index to '%s' (code %d)\n", save_target, src);
            } else if (opt.build_index_file) {
                printf("%sSuccessfully created trigram index:%s %s\n",
                       ANSI_BOLD_CYAN, ANSI_RESET, save_target);
                printf("  Documents indexed: %zu\n", valid_docs);
                printf("  Total data size:   %.2f MB\n", (double)total_bytes_read / (1024.0 * 1024.0));
                printf("  Build time:        %.3f s (%.1f MB/s)\n",
                       (t_build_end - t_build_start),
                       ((double)total_bytes_read / (1024.0 * 1024.0)) / (t_build_end - t_build_start));
                keystone_trigram_index_destroy(idx);
                return 0;
            }
        }
    }

    if (opt.build_index_file) {
        keystone_trigram_index_destroy(idx);
        return 0;
    }

    /* Execute Search */
    size_t doc_count = keystone_trigram_index_document_count(idx);
    if (doc_count == 0) {
        keystone_trigram_index_destroy(idx);
        return 0;
    }

    size_t pattern_len = strlen(opt.pattern);
    uint32_t* candidates = (uint32_t*)malloc(doc_count * sizeof(uint32_t));
    if (!candidates) {
        fprintf(stderr, "Error: out of memory allocating candidate buffer\n");
        keystone_trigram_index_destroy(idx);
        return 1;
    }

    double t_search_start = get_time_sec();
    size_t num_candidates = keystone_trigram_index_get_candidates(
        idx, opt.pattern, pattern_len, candidates, doc_count
    );
    double t_candidates_done = get_time_sec();

    size_t total_matches = 0;
    size_t matching_files = 0;

    for (size_t i = 0u; i < num_candidates; i++) {
        uint32_t doc_id = candidates[i];
        const char* name = NULL;
        const char* content = NULL;
        size_t content_len = 0;

        if (keystone_trigram_index_get_document(idx, doc_id, &name, &content, &content_len) != KEYSTONE_TRIGRAM_OK) {
            continue;
        }

        if (!content || content_len < pattern_len) continue;

        /* SIMD / fast document-level substring check to reject candidate false positives */
        if (!find_substring(content, content_len, opt.pattern, pattern_len, opt.ignore_case)) {
            continue;
        }

        matching_files++;

        if (opt.files_with_matches) {
            if (opt.use_color) {
                printf("%s%s%s\n", ANSI_MAGENTA, name ? name : "<unnamed>", ANSI_RESET);
            } else {
                printf("%s\n", name ? name : "<unnamed>");
            }
            continue;
        }

        size_t file_match_count = 0;
        size_t line_no = 1u;
        const char* line_start = content;
        const char* content_end = content + content_len;

        while (line_start < content_end) {
            const char* nl = (const char*)memchr(line_start, '\n', (size_t)(content_end - line_start));
            const char* line_end = nl ? nl : content_end;
            size_t line_len = (size_t)(line_end - line_start);

            if (line_len > 0u && line_start[line_len - 1u] == '\r') {
                line_len--;
            }

            if (find_substring(line_start, line_len, opt.pattern, pattern_len, opt.ignore_case)) {
                file_match_count++;
                total_matches++;
                if (!opt.count_only) {
                    print_matched_line(
                        name ? name : "<unnamed>",
                        line_no,
                        line_start,
                        line_len,
                        opt.pattern,
                        pattern_len,
                        opt.ignore_case,
                        opt.show_line_num,
                        opt.show_filename,
                        opt.use_color
                    );
                }
            }

            line_no++;
            line_start = nl ? (nl + 1u) : content_end;
        }

        if (opt.count_only && file_match_count > 0) {
            if (opt.show_filename) {
                if (opt.use_color) {
                    printf("%s%s%s%s:%s%zu\n",
                           ANSI_MAGENTA, name ? name : "<unnamed>", ANSI_RESET,
                           ANSI_CYAN, ANSI_RESET, file_match_count);
                } else {
                    printf("%s:%zu\n", name ? name : "<unnamed>", file_match_count);
                }
            } else {
                printf("%zu\n", file_match_count);
            }
        }
    }

    double t_search_end = get_time_sec();
    free(candidates);

    /* Telemetry stats */
    if (opt.show_stats) {
        double candidate_ms = (t_candidates_done - t_search_start) * 1000.0;
        double total_search_ms = (t_search_end - t_search_start) * 1000.0;
        double rejection_rate = doc_count > 0 ?
            (100.0 * (double)(doc_count - num_candidates) / (double)doc_count) : 0.0;

        fprintf(stderr, "\n%s--- tgrep Performance Telemetry ---%s\n", ANSI_BOLD_CYAN, ANSI_RESET);
        if (loaded_from_index) {
            fprintf(stderr, "  Index source:      Loaded from %s (%.2f ms)\n",
                    opt.index_file, (t_load_end - t_load_start) * 1000.0);
        } else {
            fprintf(stderr, "  Directory crawl:   %.2f ms\n", (t_scan_end - t_scan_start) * 1000.0);
            fprintf(stderr, "  Index source:      Built in-memory (%.2f ms, %zu docs, %.2f MB)\n",
                    (t_build_end - t_build_start) * 1000.0, doc_count,
                    (double)total_bytes_read / (1024.0 * 1024.0));
        }
        fprintf(stderr, "  Total documents:   %zu\n", doc_count);
        fprintf(stderr, "  Candidates probed: %zu (%.2f%% rejected by trigrams)\n",
                num_candidates, rejection_rate);
        fprintf(stderr, "  Matching files:    %zu\n", matching_files);
        fprintf(stderr, "  Total matches:     %zu\n", total_matches);
        fprintf(stderr, "  Trigram lookup:    %.3f ms\n", candidate_ms);
        fprintf(stderr, "  Total search time: %.3f ms\n", total_search_ms);
        fprintf(stderr, "%s------------------------------------%s\n", ANSI_BOLD_CYAN, ANSI_RESET);
    }

    keystone_trigram_index_destroy(idx);
    return (total_matches > 0 || (opt.files_with_matches && matching_files > 0)) ? 0 : 1;
}
