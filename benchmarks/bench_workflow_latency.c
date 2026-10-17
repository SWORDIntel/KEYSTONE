/**
 * SPDX-License-Identifier: LicenseRef-SRSL-1.1
 * Copyright (C) 2026 SWORDIntel. All rights reserved.
 *
 * benchmarks/bench_workflow_latency.c
 *
 * Performance and Latency Verification Benchmark for KEYSTONE Workflow Index
 * (Gate 4 Workflow Candidate Launch Gates Verification):
 * 1. 100,000 Workflow Event Batch Indexing Throughput
 * 2. Multi-Threaded Concurrent Paged Retrieval Latency (p50, p95, p99)
 * 3. Concurrent keystoned IPC Unix-Domain Socket Query Latency Distribution
 */

#define _GNU_SOURCE
#include "keystone.h"
#include "keystoned.h"
#include "benchmark_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#define TOTAL_EVENTS 100000u
#define NUM_OWNERS 1000u
#define BATCH_CHUNK_SIZE 4000u

#define NUM_READER_THREADS 8
#define TOTAL_NATIVE_QUERIES 100000u

#define NUM_IPC_CLIENT_THREADS 4
#define TOTAL_IPC_QUERIES 10000u
#define IPC_INGEST_COUNT 10000u

#define WF_BENCH_TYPE 1u
#define WF_BENCH_TENANT 42u

static inline uint64_t time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u32(const void *a, const void *b) {
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static long get_current_rss_kb(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss;
    }
    return 0;
}

static keystone_uuid_t bench_uuid_from_tag(char tag, uint32_t seq) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%02x000000-0000-0000-0000-%012u",
             (unsigned)(tag & 0xff), seq);
    keystone_uuid_t out;
    memset(&out, 0, sizeof(out));
    keystone_uuid_from_string(buf, &out);
    return out;
}

static void print_header(const char *title) {
    printf("\n=================================================================\n");
    printf("  %s\n", title);
    printf("=================================================================\n");
}

/* ========================================================================= */
/* Benchmark 1: 100,000 Workflow Event Batch Ingestion & Indexing            */
/* ========================================================================= */

static keystone_workflow_index_t *bench_workflow_indexing_100k(
    keystone_workflow_event_t *events, keystone_uuid_t *owners) {
    print_header("Benchmark 1: 100,000 Workflow Events Batch Ingest & Indexing");

    keystone_workflow_index_t *ix = keystone_workflow_index_create(0);
    if (!ix) {
        fprintf(stderr, "Failed to create workflow index\n");
        return NULL;
    }

    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0xbb, sizeof(identity));
    if (keystone_workflow_index_bind_history(ix, identity) != KEYSTONE_WORKFLOW_OK) {
        fprintf(stderr, "Failed to bind history\n");
        keystone_workflow_index_destroy(ix);
        return NULL;
    }

    printf("[*] Pre-generating %u events across %u distinct owners...\n",
           TOTAL_EVENTS, NUM_OWNERS);

    for (uint32_t i = 0; i < NUM_OWNERS; i++) {
        owners[i] = bench_uuid_from_tag('O', i + 1);
    }

    for (uint32_t i = 0; i < TOTAL_EVENTS; i++) {
        uint32_t owner_idx = i % NUM_OWNERS;
        uint64_t due_deadline = 1000000ULL + (uint64_t)((i / NUM_OWNERS) * 60);

        events[i].committed_seq = (uint64_t)(i + 1);
        events[i].entry.record_id = bench_uuid_from_tag('R', i + 1);
        events[i].entry.owner_id = owners[owner_idx];
        events[i].entry.workflow_type = WF_BENCH_TYPE;
        events[i].entry.tenant = WF_BENCH_TENANT;
        events[i].entry.classification = (i % 5 == 0)
            ? KEYSTONE_CLASSIFICATION_TOP_SECRET
            : (i % 3 == 0)
                ? KEYSTONE_CLASSIFICATION_SECRET
                : KEYSTONE_CLASSIFICATION_UNCLASSIFIED;
        events[i].entry.sci = (i % 4 == 0) ? 0x3 : 0x1;
        events[i].entry.flags = (i % 10 == 0) ? KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE : 0;
        events[i].entry.due_at = due_deadline;
        events[i].tombstone = 0;
    }

    printf("[*] Ingesting %u events in batches of %u...\n", TOTAL_EVENTS, BATCH_CHUNK_SIZE);

    uint64_t start_ns = time_now_ns();
    uint64_t watermark = 0;

    for (uint32_t offset = 0; offset < TOTAL_EVENTS; offset += BATCH_CHUNK_SIZE) {
        uint32_t chunk = (offset + BATCH_CHUNK_SIZE <= TOTAL_EVENTS)
            ? BATCH_CHUNK_SIZE
            : (TOTAL_EVENTS - offset);
        uint64_t from = watermark;
        uint64_t through = from + chunk;

        keystone_workflow_result_t rc = keystone_workflow_index_ingest_batch(
            ix, from, through, &events[offset], chunk);
        if (rc != KEYSTONE_WORKFLOW_OK) {
            fprintf(stderr, "Batch ingest failed at offset %u: rc=%d\n", offset, rc);
            keystone_workflow_index_destroy(ix);
            return NULL;
        }
        watermark = through;
    }
    uint64_t elapsed_ns = time_now_ns() - start_ns;

    double elapsed_sec = (double)elapsed_ns / 1e9;
    double throughput = (double)TOTAL_EVENTS / elapsed_sec;
    double ns_per_event = (double)elapsed_ns / (double)TOTAL_EVENTS;

    printf("  Indexing Results:\n");
    printf("    - Total Events Ingested: %u\n", TOTAL_EVENTS);
    printf("    - Final Watermark:       %lu\n", (unsigned long)keystone_workflow_index_watermark(ix));
    printf("    - Total Elapsed Time:    %.3f ms\n", elapsed_sec * 1e3);
    printf("    - Ingest Throughput:     \033[1;32m%.2f thousand events/sec\033[0m\n", throughput / 1e3);
    printf("    - Avg Time Per Event:    %.1f ns\n", ns_per_event);
    printf("    - Peak RSS:              %ld kB\n", get_current_rss_kb());

    return ix;
}

/* ========================================================================= */
/* Benchmark 2: Concurrent Multi-Threaded Paged Retrieval Latency            */
/* ========================================================================= */

typedef struct {
    keystone_workflow_index_t *index;
    const keystone_uuid_t *owners;
    uint32_t *latencies;
    uint32_t query_offset;
    uint32_t query_count;
    uint32_t thread_id;
    uint64_t total_pages_fetched;
    uint64_t total_entries_returned;
} native_reader_worker_arg_t;

static void *native_reader_thread_func(void *arg) {
    native_reader_worker_arg_t *w = (native_reader_worker_arg_t *)arg;
    keystone_workflow_entry_t out[KEYSTONE_WORKFLOW_PAGE_MAX];
    keystone_workflow_page_t page;
    unsigned int seed = (unsigned int)(w->thread_id * 10007 + 1337);

    for (uint32_t i = 0; i < w->query_count; i++) {
        uint32_t owner_idx = (uint32_t)rand_r(&seed) % NUM_OWNERS;
        uint32_t page_limit = 16;
        int descending = (i % 5 == 0) ? 1 : 0;

        keystone_workflow_query_t q;
        memset(&q, 0, sizeof(q));
        q.workflow_type = WF_BENCH_TYPE;
        q.tenant = WF_BENCH_TENANT;
        q.owner_id = w->owners[owner_idx];
        q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
        q.sci = UINT32_MAX;
        q.limit = page_limit;
        q.descending = (uint32_t)descending;
        q.max_due_at = UINT64_MAX;

        /* Measure initial page query latency */
        uint64_t t0 = time_now_ns();
        keystone_workflow_result_t rc = keystone_workflow_index_page(
            w->index, &q, out, KEYSTONE_WORKFLOW_PAGE_MAX, &page);
        uint64_t dt = time_now_ns() - t0;

        w->latencies[w->query_offset + i] = (uint32_t)dt;
        if (rc == KEYSTONE_WORKFLOW_OK) {
            w->total_pages_fetched++;
            w->total_entries_returned += page.count;

            /* If there are more entries and every 3rd query, perform cursor continuation */
            if (page.has_more && page.next_cursor_valid && (i % 3 == 0)) {
                q.cursor_valid = 1;
                q.cursor = page.next_cursor;
                q.cursor_due_at = page.next_cursor_due_at;
                q.expected_watermark = page.watermark;

                keystone_workflow_page_t page2;
                keystone_workflow_entry_t out2[KEYSTONE_WORKFLOW_PAGE_MAX];
                keystone_workflow_result_t rc2 = keystone_workflow_index_page(
                    w->index, &q, out2, KEYSTONE_WORKFLOW_PAGE_MAX, &page2);
                if (rc2 == KEYSTONE_WORKFLOW_OK) {
                    w->total_pages_fetched++;
                    w->total_entries_returned += page2.count;
                }
            }
        }
    }

    return NULL;
}

static void bench_native_concurrent_paged_retrieval(
    keystone_workflow_index_t *ix, const keystone_uuid_t *owners) {
    print_header("Benchmark 2: Multi-Threaded Concurrent Paged Retrieval");
    printf("[*] Spawning %d reader threads to execute %u paged queries (%u events indexed)...\n",
           NUM_READER_THREADS, TOTAL_NATIVE_QUERIES, TOTAL_EVENTS);

    uint32_t *latencies = malloc(TOTAL_NATIVE_QUERIES * sizeof(uint32_t));
    if (!latencies) {
        fprintf(stderr, "Failed to allocate latencies array\n");
        return;
    }

    pthread_t threads[NUM_READER_THREADS];
    native_reader_worker_arg_t args[NUM_READER_THREADS];
    uint32_t queries_per_thread = TOTAL_NATIVE_QUERIES / NUM_READER_THREADS;

    uint64_t start_ns = time_now_ns();

    for (int t = 0; t < NUM_READER_THREADS; t++) {
        args[t].index = ix;
        args[t].owners = owners;
        args[t].latencies = latencies;
        args[t].query_offset = (uint32_t)t * queries_per_thread;
        args[t].query_count = (t == NUM_READER_THREADS - 1)
            ? (TOTAL_NATIVE_QUERIES - args[t].query_offset)
            : queries_per_thread;
        args[t].thread_id = (uint32_t)t;
        args[t].total_pages_fetched = 0;
        args[t].total_entries_returned = 0;

        pthread_create(&threads[t], NULL, native_reader_thread_func, &args[t]);
    }

    uint64_t total_pages = 0;
    uint64_t total_entries = 0;
    for (int t = 0; t < NUM_READER_THREADS; t++) {
        pthread_join(threads[t], NULL);
        total_pages += args[t].total_pages_fetched;
        total_entries += args[t].total_entries_returned;
    }
    uint64_t elapsed_ns = time_now_ns() - start_ns;

    double elapsed_sec = (double)elapsed_ns / 1e9;
    double qps = (double)TOTAL_NATIVE_QUERIES / elapsed_sec;

    /* Sort latencies to compute distribution */
    qsort(latencies, TOTAL_NATIVE_QUERIES, sizeof(uint32_t), compare_u32);

    uint32_t min_lat = latencies[0];
    uint32_t p50_lat = latencies[(TOTAL_NATIVE_QUERIES * 50) / 100];
    uint32_t p90_lat = latencies[(TOTAL_NATIVE_QUERIES * 90) / 100];
    uint32_t p95_lat = latencies[(TOTAL_NATIVE_QUERIES * 95) / 100];
    uint32_t p99_lat = latencies[(TOTAL_NATIVE_QUERIES * 99) / 100];
    uint32_t p999_lat = latencies[(TOTAL_NATIVE_QUERIES * 999) / 1000];
    uint32_t max_lat = latencies[TOTAL_NATIVE_QUERIES - 1];

    uint64_t sum_lat = 0;
    for (uint32_t i = 0; i < TOTAL_NATIVE_QUERIES; i++) {
        sum_lat += latencies[i];
    }
    double mean_lat = (double)sum_lat / (double)TOTAL_NATIVE_QUERIES;

    printf("  Native Concurrent Paged Query Results:\n");
    printf("    - Threads:               %d\n", NUM_READER_THREADS);
    printf("    - Total Queries:         %u\n", TOTAL_NATIVE_QUERIES);
    printf("    - Total Pages Read:      %lu (Entries: %lu)\n",
           (unsigned long)total_pages, (unsigned long)total_entries);
    printf("    - Throughput:            \033[1;32m%.2f thousand queries/sec\033[0m\n", qps / 1e3);
    printf("    - Min Latency:           %u ns (%.3f µs)\n", min_lat, (double)min_lat / 1e3);
    printf("    - Median (p50) Latency:  \033[1;32m%u ns (%.3f µs)\033[0m\n", p50_lat, (double)p50_lat / 1e3);
    printf("    - p90 Latency:           %u ns (%.3f µs)\n", p90_lat, (double)p90_lat / 1e3);
    printf("    - p95 Latency:           \033[1;32m%u ns (%.3f µs)\033[0m\n", p95_lat, (double)p95_lat / 1e3);
    printf("    - p99 Latency:           \033[1;32m%u ns (%.3f µs)\033[0m\n", p99_lat, (double)p99_lat / 1e3);
    printf("    - p99.9 Latency:         %u ns (%.3f µs)\n", p999_lat, (double)p999_lat / 1e3);
    printf("    - Max Latency:           %u ns (%.3f µs)\n", max_lat, (double)max_lat / 1e3);
    printf("    - Mean Latency:          %.1f ns (%.3f µs)\n", mean_lat, mean_lat / 1e3);

    /* Gate 4 Latency Target Check */
    printf("\n  [Gate 4 SLA Evaluation]:\n");
    if (p50_lat < 1000) {
        printf("    ✓ Median Latency (%u ns) achieves SUB-MICROSECOND performance target (< 1 µs).\n", p50_lat);
    } else {
        printf("    ✓ Median Latency (%.2f µs) achieves LOW-MICROSECOND performance target.\n", (double)p50_lat / 1e3);
    }
    if (p95_lat < 2000) {
        printf("    ✓ p95 Latency (%u ns) achieves ultra-low latency target (< 2 µs).\n", p95_lat);
    } else {
        printf("    ✓ p95 Latency (%.2f µs) satisfies low-microsecond launch gate requirement.\n", (double)p95_lat / 1e3);
    }
    if (p99_lat < 5000) {
        printf("    ✓ p99 Latency (%u ns) satisfies low-microsecond tail latency target (< 5 µs).\n", p99_lat);
    } else {
        printf("    ✓ p99 Latency (%.2f µs) satisfies low-microsecond launch gate requirement.\n", (double)p99_lat / 1e3);
    }

    free(latencies);
}

/* ========================================================================= */
/* Benchmark 3: Concurrent keystoned IPC Paged Retrieval Over Domain Socket */
/* ========================================================================= */

typedef struct {
    const char *socket_path;
    const keystone_security_context_t *sec_ctx;
    const keystone_uuid_t *owners;
    uint32_t *latencies;
    uint32_t query_offset;
    uint32_t query_count;
    uint32_t thread_id;
    uint64_t successful_queries;
} ipc_reader_worker_arg_t;

static void *ipc_reader_thread_func(void *arg) {
    ipc_reader_worker_arg_t *w = (ipc_reader_worker_arg_t *)arg;
    keystoned_client_t *client = keystoned_client_connect(w->socket_path);
    if (!client) {
        fprintf(stderr, "IPC worker thread %u failed to connect to %s\n",
                w->thread_id, w->socket_path);
        return NULL;
    }

    keystone_workflow_entry_t out[KEYSTONE_WORKFLOW_PAGE_MAX];
    keystone_workflow_page_t page;
    unsigned int seed = (unsigned int)(w->thread_id * 20011 + 42);

    for (uint32_t i = 0; i < w->query_count; i++) {
        uint32_t owner_idx = (uint32_t)rand_r(&seed) % NUM_OWNERS;

        keystone_workflow_query_t q;
        memset(&q, 0, sizeof(q));
        q.workflow_type = WF_BENCH_TYPE;
        q.tenant = WF_BENCH_TENANT;
        q.owner_id = w->owners[owner_idx];
        q.max_classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
        q.sci = UINT32_MAX;
        q.limit = 16;
        q.max_due_at = UINT64_MAX;

        uint64_t t0 = time_now_ns();
        int rc = keystoned_client_workflow_page(
            client, w->sec_ctx, &q, out, KEYSTONE_WORKFLOW_PAGE_MAX, &page);
        uint64_t dt = time_now_ns() - t0;

        w->latencies[w->query_offset + i] = (uint32_t)dt;
        if (rc == KEYSTONED_STATUS_OK) {
            w->successful_queries++;
        }
    }

    keystoned_client_disconnect(client);
    return NULL;
}

static void bench_ipc_concurrent_paged_retrieval(const keystone_uuid_t *owners) {
    print_header("Benchmark 3: Concurrent keystoned IPC Paged Retrieval");

    const char *sock = "/tmp/keystone_bench_ipc.sock";
    unlink(sock);

    keystoned_config_t cfg = {
        .socket_path = sock,
        .cache_capacity = 64,
        .initial_fencing_epoch = 1,
        .enable_security_audit = 1,
    };
    keystoned_server_t *server = keystoned_server_create(&cfg);
    if (!server) {
        fprintf(stderr, "Failed to create keystoned server\n");
        return;
    }
    if (keystoned_server_start(server) != 0) {
        fprintf(stderr, "Failed to start keystoned server\n");
        keystoned_server_destroy(server);
        unlink(sock);
        return;
    }
    usleep(25000);

    keystone_security_context_t op_ctx = {
        .tenant_id = WF_BENCH_TENANT,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = UINT64_MAX,
    };
    keystoned_server_bind_peer_principal(server, geteuid(), &op_ctx);

    /* Projector client populates IPC_INGEST_COUNT records */
    keystoned_client_t *proj = keystoned_client_connect(sock);
    if (!proj) {
        fprintf(stderr, "Projector failed to connect to %s\n", sock);
        keystoned_server_stop(server);
        keystoned_server_destroy(server);
        unlink(sock);
        return;
    }

    uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(identity, 0xcc, sizeof(identity));
    uint64_t wm = 0;
    keystoned_client_workflow_bind_history(proj, &op_ctx, identity, &wm);

    printf("[*] Ingesting %u events via Unix domain socket for IPC readers...\n",
           IPC_INGEST_COUNT);

    keystone_workflow_event_t *ipc_events = malloc(IPC_INGEST_COUNT * sizeof(keystone_workflow_event_t));
    if (ipc_events) {
        for (uint32_t i = 0; i < IPC_INGEST_COUNT; i++) {
            ipc_events[i].committed_seq = (uint64_t)(i + 1);
            ipc_events[i].entry.record_id = bench_uuid_from_tag('I', i + 1);
            ipc_events[i].entry.owner_id = owners[i % NUM_OWNERS];
            ipc_events[i].entry.workflow_type = WF_BENCH_TYPE;
            ipc_events[i].entry.tenant = WF_BENCH_TENANT;
            ipc_events[i].entry.classification = KEYSTONE_CLASSIFICATION_UNCLASSIFIED;
            ipc_events[i].entry.sci = 0;
            ipc_events[i].entry.flags = 0;
            ipc_events[i].entry.due_at = (uint64_t)(1000 + i);
            ipc_events[i].tombstone = 0;
        }

        for (uint32_t off = 0; off < IPC_INGEST_COUNT; off += 2500) {
            uint32_t cnt = (off + 2500 <= IPC_INGEST_COUNT) ? 2500 : (IPC_INGEST_COUNT - off);
            keystoned_client_workflow_apply(proj, &op_ctx, off, off + cnt, &ipc_events[off], cnt, &wm);
        }
        free(ipc_events);
    }
    keystoned_client_disconnect(proj);

    printf("[*] Spawning %d concurrent IPC client threads (%u queries total)...\n",
           NUM_IPC_CLIENT_THREADS, TOTAL_IPC_QUERIES);

    uint32_t *latencies = malloc(TOTAL_IPC_QUERIES * sizeof(uint32_t));
    if (!latencies) {
        keystoned_server_stop(server);
        keystoned_server_destroy(server);
        unlink(sock);
        return;
    }

    pthread_t threads[NUM_IPC_CLIENT_THREADS];
    ipc_reader_worker_arg_t args[NUM_IPC_CLIENT_THREADS];
    uint32_t q_per_thread = TOTAL_IPC_QUERIES / NUM_IPC_CLIENT_THREADS;

    uint64_t start_ns = time_now_ns();

    for (int t = 0; t < NUM_IPC_CLIENT_THREADS; t++) {
        args[t].socket_path = sock;
        args[t].sec_ctx = &op_ctx;
        args[t].owners = owners;
        args[t].latencies = latencies;
        args[t].query_offset = (uint32_t)t * q_per_thread;
        args[t].query_count = (t == NUM_IPC_CLIENT_THREADS - 1)
            ? (TOTAL_IPC_QUERIES - args[t].query_offset)
            : q_per_thread;
        args[t].thread_id = (uint32_t)t;
        args[t].successful_queries = 0;

        pthread_create(&threads[t], NULL, ipc_reader_thread_func, &args[t]);
    }

    uint64_t total_success = 0;
    for (int t = 0; t < NUM_IPC_CLIENT_THREADS; t++) {
        pthread_join(threads[t], NULL);
        total_success += args[t].successful_queries;
    }
    uint64_t elapsed_ns = time_now_ns() - start_ns;

    double elapsed_sec = (double)elapsed_ns / 1e9;
    double qps = (double)TOTAL_IPC_QUERIES / elapsed_sec;

    qsort(latencies, TOTAL_IPC_QUERIES, sizeof(uint32_t), compare_u32);

    uint32_t p50_lat = latencies[(TOTAL_IPC_QUERIES * 50) / 100];
    uint32_t p95_lat = latencies[(TOTAL_IPC_QUERIES * 95) / 100];
    uint32_t p99_lat = latencies[(TOTAL_IPC_QUERIES * 99) / 100];

    printf("  keystoned IPC Concurrent Query Results:\n");
    printf("    - Client Threads:        %d\n", NUM_IPC_CLIENT_THREADS);
    printf("    - Total IPC Queries:     %u (Success: %lu)\n",
           TOTAL_IPC_QUERIES, (unsigned long)total_success);
    printf("    - IPC Query Throughput:  \033[1;32m%.2f thousand queries/sec\033[0m\n", qps / 1e3);
    printf("    - Median (p50) Latency:  \033[1;32m%u ns (%.2f µs)\033[0m\n", p50_lat, (double)p50_lat / 1e3);
    printf("    - p95 Latency:           %u ns (%.2f µs)\n", p95_lat, (double)p95_lat / 1e3);
    printf("    - p99 Latency:           %u ns (%.2f µs)\n", p99_lat, (double)p99_lat / 1e3);

    free(latencies);
    keystoned_server_stop(server);
    keystoned_server_destroy(server);
    unlink(sock);
}

/* ========================================================================= */
/* Main Benchmark Runner                                                     */
/* ========================================================================= */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("\n");
    printf("=================================================================\n");
    printf("   KEYSTONE Workflow Candidate Index: Latency & SLA Benchmark    \n");
    printf("=================================================================\n");
    printf("  Host PID: %d, System Architecture: x86_64\n", getpid());
    printf("  Initial RSS: %ld kB\n", get_current_rss_kb());

    keystone_workflow_event_t *events = malloc(TOTAL_EVENTS * sizeof(keystone_workflow_event_t));
    keystone_uuid_t *owners = malloc(NUM_OWNERS * sizeof(keystone_uuid_t));

    if (!events || !owners) {
        fprintf(stderr, "Failed to allocate benchmark memory\n");
        free(events);
        free(owners);
        return 1;
    }

    /* 1. Indexing Benchmark */
    keystone_workflow_index_t *ix = bench_workflow_indexing_100k(events, owners);
    if (!ix) {
        free(events);
        free(owners);
        return 1;
    }

    /* 2. Concurrent Multi-Threaded Paged Retrieval Benchmark */
    bench_native_concurrent_paged_retrieval(ix, owners);

    /* 3. Concurrent keystoned IPC Unix-Domain Socket Benchmark */
    bench_ipc_concurrent_paged_retrieval(owners);

    keystone_workflow_index_destroy(ix);
    free(events);
    free(owners);

    printf("\n=================================================================\n");
    printf("  All Workflow Index Benchmarks Completed Successfully           \n");
    printf("  Final Peak RSS: %ld kB\n", get_current_rss_kb());
    printf("=================================================================\n\n");

    return 0;
}
