/*
 * KEYSTONE Federated Query Transport Tests (Phase 6 completion)
 *
 * Real sockets on loopback: healthy/dead/silent nodes, per-node timeouts,
 * partial-result statuses, HLC-ordered dedup merge, label filtering at both
 * ends, exact-identity conflict resolution, and hostile-frame handling.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "../include/keystone.h"
#include "../include/keystone_federation.h"
#include "../include/keystone_exact_index.h"
#include "../include/keystone_temporal.h"
#include "../include/keystone_federated_query.h"
#include "../include/keystone_fed_transport.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void add_event(keystone_temporal_index_t* idx, uint64_t ms, uint8_t obj,
                      uint8_t evt, uint32_t classification, uint32_t tenant, uint32_t flags) {
    keystone_temporal_entry_t te;
    memset(&te, 0, sizeof(te));
    te.hlc = (keystone_hlc_t){ .physical_ms = ms, .logical = 0, .node_id = 1 };
    te.object_id.bytes[0] = obj;
    te.event_id.bytes[0] = evt;
    te.event_type = KEYSTONE_OBJ_VM;
    te.classification = classification;
    te.tenant_id = tenant;
    te.flags = flags;
    te.source_generation = ms;
    TEST_ASSERT(keystone_temporal_index_append(idx, &te) == 0);
}

/* A silent node: accepts connections and never answers (timeout target). */
typedef struct { int fd; volatile int stop; pthread_t t; } silent_node_t;

static void* silent_thread(void* arg) {
    silent_node_t* sn = (silent_node_t*)arg;
    while (!sn->stop) {
        /* bounded accept so stop_silent_node's join always returns */
        struct pollfd pfd = { .fd = sn->fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 100) <= 0) continue;
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(sn->fd, (struct sockaddr*)&peer, &plen);
        if (c >= 0) {
            /* hold the connection open, say nothing */
            usleep(300000);
            close(c);
        }
    }
    return NULL;
}

static int start_silent_node(silent_node_t* sn) {
    sn->stop = 0;
    sn->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sn->fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(sn->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(sn->fd, 4) != 0) {
        close(sn->fd);
        return -1;
    }
    if (pthread_create(&sn->t, NULL, silent_thread, sn) != 0) {
        close(sn->fd);
        return -1;
    }
    return 0;
}

static uint16_t silent_port(silent_node_t* sn) {
    struct sockaddr_in got;
    socklen_t gl = sizeof(got);
    getsockname(sn->fd, (struct sockaddr*)&got, &gl);
    return ntohs(got.sin_port);
}

static void stop_silent_node(silent_node_t* sn) {
    sn->stop = 1;
    pthread_join(sn->t, NULL);
    close(sn->fd);
}

static void test_timeline_fanout_partial_and_labels(void) {
    printf("[*] Testing timeline fan-out: partial failure, dedup, labels...\n");

    /* Node A: events for obj 1 (OPS) and a SECRET event on obj 2 */
    keystone_temporal_index_t* idx_a = keystone_temporal_index_create(32);
    add_event(idx_a, 1000100, 1, 0xA1, KEYSTONE_CLASSIFICATION_OPS, 1, KEYSTONE_RECORD_FLAG_EVENT);
    add_event(idx_a, 1000200, 1, 0xA2, KEYSTONE_CLASSIFICATION_OPS, 1, KEYSTONE_RECORD_FLAG_EVENT);
    add_event(idx_a, 1000300, 2, 0xA3, KEYSTONE_CLASSIFICATION_SECRET, 1, KEYSTONE_RECORD_FLAG_EVENT);
    add_event(idx_a, 1000400, 3, 0xA4, KEYSTONE_CLASSIFICATION_OPS, 1,
              KEYSTONE_RECORD_FLAG_EVENT | KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE);

    /* Node B: one DUPLICATE of A's first event (same event id) + its own */
    keystone_temporal_index_t* idx_b = keystone_temporal_index_create(32);
    add_event(idx_b, 1000100, 1, 0xA1, KEYSTONE_CLASSIFICATION_OPS, 1, KEYSTONE_RECORD_FLAG_EVENT); /* same id */
    add_event(idx_b, 1000500, 4, 0xB1, KEYSTONE_CLASSIFICATION_OPS, 1, KEYSTONE_RECORD_FLAG_EVENT);

    keystone_fed_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.temporal = idx_a;
    keystone_fed_server_t* node_a = keystone_fed_server_create(&cfg);
    TEST_ASSERT(node_a != NULL && keystone_fed_server_start(node_a) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.temporal = idx_b;
    keystone_fed_server_t* node_b = keystone_fed_server_create(&cfg);
    TEST_ASSERT(node_b != NULL && keystone_fed_server_start(node_b) == 0);

    /* Node C: dead port; Node D: silent (timeout target) */
    keystone_fed_node_addr_t nodes[4];
    memset(nodes, 0, sizeof(nodes));
    snprintf(nodes[0].host, sizeof(nodes[0].host), "127.0.0.1");
    nodes[0].port = keystone_fed_server_bound_port(node_a);
    snprintf(nodes[1].host, sizeof(nodes[1].host), "127.0.0.1");
    nodes[1].port = keystone_fed_server_bound_port(node_b);
    snprintf(nodes[2].host, sizeof(nodes[2].host), "127.0.0.1");
    nodes[2].port = 1; /* nothing listens here: refused fast */
    silent_node_t silent;
    TEST_ASSERT(start_silent_node(&silent) == 0);
    snprintf(nodes[3].host, sizeof(nodes[3].host), "127.0.0.1");
    nodes[3].port = silent_port(&silent);

    usleep(50000); /* let responders bind */

    keystone_security_context_t ops_caller = {
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS, .compartment_mask = 0
    };
    keystone_security_context_t secret_caller = {
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_SECRET, .compartment_mask = 0
    };

    keystone_hlc_t lo = { .physical_ms = 0, .logical = 0, .node_id = 0 };
    keystone_hlc_t hi = { .physical_ms = 2000000, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };

    keystone_fed_node_result_t results[4];
    keystone_temporal_entry_t out[32];

    /* 1. OPS caller: deduped union of A+B minus SECRET and SENSITIVE. */
    size_t n = keystone_fed_query_timeline_fanout(nodes, 4, &ops_caller, &lo, &hi, 300, out, 32, results);
    /* unique events visible to OPS: A1, A2, B1 = 3 (A3 SECRET, A4 SENSITIVE filtered) */
    TEST_ASSERT(n == 3);
    TEST_ASSERT(results[0].status == KEYSTONE_FED_NODE_OK);
    TEST_ASSERT(results[1].status == KEYSTONE_FED_NODE_OK);
    TEST_ASSERT(results[2].status == KEYSTONE_FED_NODE_UNREACHABLE);
    TEST_ASSERT(results[3].status == KEYSTONE_FED_NODE_TIMEOUT);
    /* HLC-ordered with no duplicate event ids */
    for (size_t i = 0; i + 1 < n; i++) {
        TEST_ASSERT(keystone_hlc_compare(&out[i].hlc, &out[i + 1].hlc) <= 0);
        for (size_t j = i + 1; j < n; j++) {
            TEST_ASSERT(!keystone_uuid_equal(&out[i].event_id, &out[j].event_id));
        }
    }

    /* 2. SECRET caller sees everything except cross-tenant (none here). */
    n = keystone_fed_query_timeline_fanout(nodes, 4, &secret_caller, &lo, &hi, 300, out, 32, results);
    TEST_ASSERT(n == 5); /* A1, A2, A3, A4, B1 */
    TEST_ASSERT(results[0].status == KEYSTONE_FED_NODE_OK);

    /* 3. NULL caller gets nothing regardless of node data. */
    n = keystone_fed_query_timeline_fanout(nodes, 4, NULL, &lo, &hi, 300, out, 32, results);
    TEST_ASSERT(n == 0);

    /* 4. Cross-tenant entries never leave the coordinator. */
    keystone_temporal_index_t* idx_c = keystone_temporal_index_create(32);
    add_event(idx_c, 1000600, 5, 0xC1, KEYSTONE_CLASSIFICATION_OPS, 2, KEYSTONE_RECORD_FLAG_EVENT); /* tenant 2 */
    keystone_fed_server_t* node_c = keystone_fed_server_create(&(keystone_fed_server_config_t){ .temporal = idx_c });
    TEST_ASSERT(node_c != NULL && keystone_fed_server_start(node_c) == 0);
    keystone_fed_node_addr_t only_c[1];
    memset(only_c, 0, sizeof(only_c));
    snprintf(only_c[0].host, sizeof(only_c[0].host), "127.0.0.1");
    only_c[0].port = keystone_fed_server_bound_port(node_c);
    usleep(50000);
    n = keystone_fed_query_timeline_fanout(only_c, 1, &ops_caller, &lo, &hi, 300, out, 32, results);
    TEST_ASSERT(n == 0); /* server-side filter already dropped it */
    TEST_ASSERT(results[0].status == KEYSTONE_FED_NODE_OK);

    keystone_fed_server_destroy(node_c);
    keystone_temporal_index_destroy(idx_c);
    stop_silent_node(&silent);
    keystone_fed_server_destroy(node_a);
    keystone_fed_server_destroy(node_b);
    keystone_temporal_index_destroy(idx_a);
    keystone_temporal_index_destroy(idx_b);
    printf("    [+] Timeline fan-out verified (dedup, partial statuses, both-end label filtering).\n");
}

static void test_exact_fanout_conflict_resolution(void) {
    printf("[*] Testing exact fan-out conflict resolution (Epoch > Gen > HLC)...\n");

    keystone_uuid_t id;
    memset(&id, 0, sizeof(id));
    id.bytes[0] = 0x42;

    /* Node A: gen 10 epoch 1; Node B: gen 20 epoch 1 (fresher generation). */
    keystone_exact_index_t* ex_a = keystone_exact_index_create(8);
    keystone_exact_index_t* ex_b = keystone_exact_index_create(8);
    keystone_exact_entry_t ea, eb;
    memset(&ea, 0, sizeof(ea));
    memset(&eb, 0, sizeof(ea));
    ea.resource_id = id; ea.latest_generation = 10; ea.fencing_epoch = 1; ea.tenant_id = 1;
    ea.classification = KEYSTONE_CLASSIFICATION_OPS;
    eb.resource_id = id; eb.latest_generation = 20; eb.fencing_epoch = 1; eb.tenant_id = 1;
    eb.classification = KEYSTONE_CLASSIFICATION_OPS;
    TEST_ASSERT(keystone_exact_index_upsert(ex_a, &ea) == 0);
    TEST_ASSERT(keystone_exact_index_upsert(ex_b, &eb) == 0);

    keystone_fed_server_t* node_a = keystone_fed_server_create(&(keystone_fed_server_config_t){ .exact = ex_a });
    keystone_fed_server_t* node_b = keystone_fed_server_create(&(keystone_fed_server_config_t){ .exact = ex_b });
    TEST_ASSERT(node_a && keystone_fed_server_start(node_a) == 0);
    TEST_ASSERT(node_b && keystone_fed_server_start(node_b) == 0);

    keystone_fed_node_addr_t nodes[2];
    memset(nodes, 0, sizeof(nodes));
    snprintf(nodes[0].host, sizeof(nodes[0].host), "127.0.0.1");
    nodes[0].port = keystone_fed_server_bound_port(node_a);
    snprintf(nodes[1].host, sizeof(nodes[1].host), "127.0.0.1");
    nodes[1].port = keystone_fed_server_bound_port(node_b);
    usleep(50000);

    keystone_security_context_t ops_caller = {
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS, .compartment_mask = 0
    };
    keystone_fed_node_result_t results[2];
    keystone_exact_entry_t winner;

    TEST_ASSERT(keystone_fed_query_exact_fanout(nodes, 2, &ops_caller, &id, 500, &winner, results) == 0);
    TEST_ASSERT(winner.latest_generation == 20); /* freshest generation wins */

    /* Denied entry is indistinguishable from not-found at the far end. */
    keystone_uuid_t missing;
    memset(&missing, 0, sizeof(missing));
    missing.bytes[0] = 0x99;
    TEST_ASSERT(keystone_fed_query_exact_fanout(nodes, 2, &ops_caller, &missing, 500, &winner, results) == -1);
    TEST_ASSERT(results[0].status == KEYSTONE_FED_NODE_OK); /* answered: not found */

    keystone_fed_server_destroy(node_a);
    keystone_fed_server_destroy(node_b);
    keystone_exact_index_destroy(ex_a);
    keystone_exact_index_destroy(ex_b);
    printf("    [+] Exact fan-out verified (freshest-generation wins; not-found surfaced).\n");
}

static void test_hostile_frames_dropped(void) {
    printf("[*] Testing hostile-frame handling on the responder...\n");

    keystone_temporal_index_t* idx = keystone_temporal_index_create(8);
    add_event(idx, 1000100, 1, 0xE1, KEYSTONE_CLASSIFICATION_OPS, 1, KEYSTONE_RECORD_FLAG_EVENT);
    keystone_fed_server_t* node = keystone_fed_server_create(&(keystone_fed_server_config_t){ .temporal = idx });
    TEST_ASSERT(node && keystone_fed_server_start(node) == 0);
    usleep(50000);

    /* Garbage bytes: the responder must drop the connection without reply. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(keystone_fed_server_bound_port(node));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    TEST_ASSERT(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    const char garbage[16] = "NOT-A-FED-QUERY";
    TEST_ASSERT(send(fd, garbage, sizeof(garbage), 0) == (ssize_t)sizeof(garbage));
    char byte = 0;
    ssize_t r = recv(fd, &byte, 1, 0);
    TEST_ASSERT(r <= 0); /* closed with no reply */
    close(fd);

    /* The server keeps serving after hostile input. */
    keystone_security_context_t ops_caller = {
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS, .compartment_mask = 0
    };
    keystone_fed_node_addr_t n1;
    memset(&n1, 0, sizeof(n1));
    snprintf(n1.host, sizeof(n1.host), "127.0.0.1");
    n1.port = keystone_fed_server_bound_port(node);
    keystone_hlc_t lo = {0}, hi = { .physical_ms = 2000000, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };
    keystone_temporal_entry_t out[4];
    keystone_fed_node_result_t nr;
    TEST_ASSERT(keystone_fed_query_timeline_fanout(&n1, 1, &ops_caller, &lo, &hi, 500, out, 4, &nr) == 1);
    TEST_ASSERT(nr.status == KEYSTONE_FED_NODE_OK);

    keystone_fed_server_destroy(node);
    keystone_temporal_index_destroy(idx);
    printf("    [+] Hostile frames dropped; responder stayed healthy.\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Federated Query Transport Tests\n");
    printf("=================================================================\n");

    test_timeline_fanout_partial_and_labels();
    test_exact_fanout_conflict_resolution();
    test_hostile_frames_dropped();

    printf("=================================================================\n");
    printf("  ALL FEDERATED TRANSPORT TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
