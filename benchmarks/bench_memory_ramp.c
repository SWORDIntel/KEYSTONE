/**
 * bench_memory_ramp.c
 *
 * Formalized RAM-capacity sweeps and cold-cache profiling for KEYSTONE.
 *
 * Capabilities:
 * - Sweeps memory usage across fractions of available RAM (e.g. 5%, 10%, 25%, 40%, 60% of MemAvailable)
 *   or scaled test sizes (1M - 16M elements in smoke/test mode).
 * - Tracks ru_maxrss, ru_minflt, and ru_majflt using getrusage(RUSAGE_SELF, &usage).
 * - Verifies transparent huge-page hints via keystone_optimize_array_memory() (madvise(MADV_HUGEPAGE)).
 * - Implements non-root cold-cache invalidation sweeps by dirtying and reading a 64MB+ buffer
 *   to measure cold-start search latency vs warm-cache latency.
 */

#include "../include/keystone.h"
#include "benchmark_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <unistd.h>

#define EVICT_BUFFER_SIZE_BYTES (64 * 1024 * 1024) /* 64 MB LLC eviction */

static inline uint64_t time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static size_t get_mem_available_bytes(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;

    char line[256];
    size_t mem_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %zu kB", &mem_kb) == 1) {
            break;
        }
    }
    fclose(f);
    return mem_kb * 1024ULL;
}

/* Evict L1/L2/L3 by dirtying and reading 64MB+ buffer without requiring root drop_caches */
static void evict_cpu_caches(uint8_t* evict_buf, size_t size_bytes) {
    if (!evict_buf || size_bytes == 0) return;

    /* Write pass: forces line allocations in L1/L2/L3 */
    for (size_t i = 0; i < size_bytes; i += 64) {
        evict_buf[i] = (uint8_t)((i * 37 + 19) & 0xFF);
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Read pass: ensures all lines are pulled and displaced */
    volatile uint64_t checksum = 0;
    for (size_t i = 0; i < size_bytes; i += 64) {
        checksum += evict_buf[i];
    }

    __asm__ volatile("" : : "r"(checksum) : "memory");
}

typedef struct {
    int fraction_pct;
    size_t n_elements;
    double gib_allocated;
    long max_rss_kb;
    long minflt_alloc;
    long majflt_alloc;
    double cold_ns_per_key;
    double warm_ns_per_key;
    double cold_warm_ratio;
    double cold_gib_s;
    double warm_gib_s;
    int hugepage_verified;
} ramp_result_t;

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --smoke, -s         Run fast test mode (1M - 16M elements)\n");
    printf("  --fractions <list>  Comma-separated RAM fractions (e.g. 5,10,25,40,60)\n");
    printf("  --queries <count>   Number of queries per search pass (default: 100000)\n");
    printf("  --csv <file>        Output results to CSV file\n");
    printf("  --help, -h          Show this help message\n");
}

int main(int argc, char** argv) {
    int smoke_mode = 0;
    size_t num_queries = 100000;
    const char* csv_output = NULL;
    char fractions_buf[128] = "5,10,25,40,60";

    /* Check environment overrides */
    const char* env_smoke = getenv("KEYSTONE_BENCH_SMOKE");
    if (env_smoke && strcmp(env_smoke, "1") == 0) {
        smoke_mode = 1;
    }
    const char* env_queries = getenv("KEYSTONE_MEMORY_QUERIES");
    if (env_queries && env_queries[0] != '\0') {
        num_queries = (size_t)strtoull(env_queries, NULL, 10);
    }
    const char* env_out = getenv("KEYSTONE_MEMORY_OUT");
    if (env_out && env_out[0] != '\0') {
        csv_output = env_out;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--smoke") == 0 || strcmp(argv[i], "-s") == 0) {
            smoke_mode = 1;
        } else if (strcmp(argv[i], "--queries") == 0 && i + 1 < argc) {
            num_queries = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_output = argv[++i];
        } else if (strcmp(argv[i], "--fractions") == 0 && i + 1 < argc) {
            strncpy(fractions_buf, argv[++i], sizeof(fractions_buf) - 1);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("=================================================================================\n");
    printf("  KEYSTONE Memory Ramp & Cold-Cache Profiling Benchmark\n");
    printf("=================================================================================\n\n");

    size_t mem_avail_bytes = get_mem_available_bytes();
    double mem_avail_gib = (double)mem_avail_bytes / (1024.0 * 1024.0 * 1024.0);
    printf("Host Available Memory: %.2f GiB (%zu bytes)\n", mem_avail_gib, mem_avail_bytes);
    printf("Query Count: %zu keys per pass\n", num_queries);
    printf("Mode: %s\n\n", smoke_mode ? "Smoke / Test Sweep (1M - 16M rows)" : "RAM Capacity Sweep (5% - 60% MemAvailable)");

    /* Allocate 64MB cache eviction buffer */
    uint8_t* evict_buf = NULL;
    if (posix_memalign((void**)&evict_buf, 4096, EVICT_BUFFER_SIZE_BYTES) != 0 || !evict_buf) {
        fprintf(stderr, "Failed to allocate 64MB eviction buffer\n");
        return 1;
    }

    /* Build targets list */
    size_t target_elements[16];
    int target_fractions[16];
    size_t num_targets = 0;

    if (smoke_mode) {
        size_t smoke_sizes[] = {1000000, 2000000, 4000000, 8000000, 16000000};
        num_targets = sizeof(smoke_sizes) / sizeof(smoke_sizes[0]);
        for (size_t i = 0; i < num_targets; ++i) {
            target_elements[i] = smoke_sizes[i];
            target_fractions[i] = (int)((smoke_sizes[i] * sizeof(int64_t) * 100) / (mem_avail_bytes > 0 ? mem_avail_bytes : 1));
        }
    } else {
        /* Parse fractions_buf */
        char* token = strtok(fractions_buf, ", ");
        while (token && num_targets < 16) {
            int pct = atoi(token);
            if (pct > 0 && pct <= 80) {
                target_fractions[num_targets] = pct;
                size_t target_bytes = (mem_avail_bytes * (size_t)pct) / 100ULL;
                target_elements[num_targets] = target_bytes / sizeof(int64_t);
                num_targets++;
            }
            token = strtok(NULL, ", ");
        }
    }

    FILE* csv_file = NULL;
    if (csv_output) {
        csv_file = fopen(csv_output, "w");
        if (csv_file) {
            fprintf(csv_file, "fraction_pct,n_elements,gib_allocated,max_rss_kb,minflt_alloc,majflt_alloc,cold_ns,warm_ns,cold_warm_ratio,cold_gib_s,warm_gib_s,hugepage_ok\n");
        }
    }

    printf("%-8s | %-12s | %-9s | %-10s | %-8s | %-8s | %-10s | %-10s | %-7s | %-8s\n",
           "RAM %", "Elements", "Size(GiB)", "MaxRSS(KB)", "MinFault", "MajFault", "Cold(ns/k)", "Warm(ns/k)", "Ratio", "HugePage");
    printf("---------+--------------+-----------+------------+----------+----------+------------+------------+---------+---------\n");

    for (size_t t = 0; t < num_targets; ++t) {
        size_t n = target_elements[t];
        int frac = target_fractions[t];
        size_t bytes = n * sizeof(int64_t);
        double gib = (double)bytes / (1024.0 * 1024.0 * 1024.0);

        struct rusage r_init_before, r_init_after;
        getrusage(RUSAGE_SELF, &r_init_before);

        int64_t* data = NULL;
        int alloc_ret = posix_memalign((void**)&data, 4096, bytes);
        if (alloc_ret != 0 || !data) {
            fprintf(stderr, "Failed to allocate %zu elements (%.2f GiB)\n", n, gib);
            continue;
        }

        /* Parallel initialization */
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < n; ++i) {
            data[i] = (int64_t)i * 2;
        }

        /* Optimize memory (transparent huge-pages) */
        int opt_ret = keystone_optimize_array_memory(data, n);
        int hugepage_ok = (opt_ret == 0);

        getrusage(RUSAGE_SELF, &r_init_after);
        long minflt_alloc = r_init_after.ru_minflt - r_init_before.ru_minflt;
        long majflt_alloc = r_init_after.ru_majflt - r_init_before.ru_majflt;
        long max_rss_kb = r_init_after.ru_maxrss;

        /* Prepare query batch */
        size_t q_count = num_queries;
        if (q_count > n) q_count = n;

        keystone_batch_item_t* items = malloc(q_count * sizeof(keystone_batch_item_t));
        if (!items) {
            free(data);
            continue;
        }

        for (size_t i = 0; i < q_count; ++i) {
            size_t idx = (i * 997) % n;
            items[i].key = data[idx];
            items[i].result = KEYSTONE_NOT_FOUND;
            items[i].ordinal = i;
        }

        /* 1. Cold pass: evict L1/L2/L3 caches via 64MB buffer */
        evict_cpu_caches(evict_buf, EVICT_BUFFER_SIZE_BYTES);

        uint64_t t0 = time_now_ns();
        size_t found_cold = keystone_search_batch_c_optimized(data, n, items, q_count, NULL, 8);
        uint64_t t1 = time_now_ns();
        double cold_ns = (double)(t1 - t0) / (double)q_count;

        /* Reset batch items for warm pass */
        for (size_t i = 0; i < q_count; ++i) {
            items[i].result = KEYSTONE_NOT_FOUND;
            items[i].ordinal = i;
        }

        /* 2. Warm pass: immediate re-query with hot CPU caches */
        uint64_t t2 = time_now_ns();
        size_t found_warm = keystone_search_batch_c_optimized(data, n, items, q_count, NULL, 8);
        uint64_t t3 = time_now_ns();
        double warm_ns = (double)(t3 - t2) / (double)q_count;

        double ratio = cold_ns / (warm_ns > 0.0 ? warm_ns : 1.0);
        double cold_gib_s = (gib) / ((cold_ns * (double)q_count) / 1e9);
        double warm_gib_s = (gib) / ((warm_ns * (double)q_count) / 1e9);

        printf("%6d%%  | %12zu | %9.3f | %10ld | %+8ld | %+8ld | %10.2f | %10.2f | %6.2fx | %s\n",
               frac, n, gib, max_rss_kb, minflt_alloc, majflt_alloc,
               cold_ns, warm_ns, ratio, hugepage_ok ? "VERIFIED" : "FAILED");

        if (csv_file) {
            fprintf(csv_file, "%d,%zu,%.3f,%ld,%ld,%ld,%.2f,%.2f,%.2f,%.3f,%.3f,%d\n",
                    frac, n, gib, max_rss_kb, minflt_alloc, majflt_alloc,
                    cold_ns, warm_ns, ratio, cold_gib_s, warm_gib_s, hugepage_ok);
            fflush(csv_file);
        }

        (void)found_cold;
        (void)found_warm;

        free(items);
        free(data);
    }

    if (csv_file) {
        fclose(csv_file);
        printf("\nCSV results saved to: %s\n", csv_output);
    }

    free(evict_buf);
    printf("\n=================================================================================\n");
    printf("  Memory Ramp & Cold-Cache Profiling Complete\n");
    printf("=================================================================================\n");
    return 0;
}
