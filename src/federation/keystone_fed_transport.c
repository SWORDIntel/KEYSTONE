/*
 * KEYSTONE Federated Query Transport Implementation
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "keystone_fed_transport.h"
#include "keystone_federated_query.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* --- Wire frames (packed, CRC32 over the prefix + payload) --- */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* KEYSTONE_FED_QMAGIC */
    uint32_t version;       /* KEYSTONE_FED_QVERSION */
    uint32_t msg_type;      /* keystone_fed_qtype_t */
    uint32_t reserved;      /* strict v1: must be zero */
    uint64_t request_id;
    keystone_security_context_t sec_ctx;
    keystone_hlc_t hlc_min;
    keystone_hlc_t hlc_max;
    uint32_t max_results;   /* responder caps at KEYSTONE_FED_MAX_RESULTS_CAP */
    uint32_t payload_len;   /* exact lookup: sizeof(uuid); timeline: 0 */
    uint32_t crc32;         /* over bytes [0, offsetof(crc32)) + payload */
} keystone_fed_req_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t status;        /* 0 = ok, 1 = not found, 2 = denied, 3 = bad request */
    uint32_t reserved;
    uint64_t request_id;
    uint64_t node_generation;
    keystone_hlc_t node_watermark;
    uint32_t result_count;
    uint32_t crc32;         /* over bytes [0, offsetof(crc32)) + results */
    /* result_count entries follow: temporal entries or one exact entry */
} keystone_fed_resp_t;
#pragma pack(pop)

#define FED_REQ_CRC_LEN offsetof(keystone_fed_req_t, crc32)
#define FED_RESP_CRC_LEN offsetof(keystone_fed_resp_t, crc32)

static uint32_t fed_crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static int initialized = 0;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/*
 * Shared label predicate (the same rule keystoned and RAG enforce):
 * classification floor, tenant isolation (0 = untagged legacy), and
 * SECURITY_SENSITIVE gated at SECRET.
 */
static int fed_item_allowed(const keystone_security_context_t* caller,
                            uint32_t classification, uint32_t tenant_id, uint32_t flags) {
    if (!caller) return 0;
    if (caller->classification < classification) return 0;
    if (tenant_id != 0u && tenant_id != caller->tenant_id) return 0;
    if ((flags & KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE) &&
        caller->classification < KEYSTONE_CLASSIFICATION_SECRET) {
        return 0;
    }
    return 1;
}

/* --- Responder --- */

struct keystone_fed_server {
    keystone_fed_server_config_t cfg;
    int listen_fd;
    uint16_t bound_port;
    volatile int running;
    pthread_t thread;
};

/* Bounded full send/recv: returns 0 on success, -1 on error/timeout. */
static int send_all_bounded(int fd, const void* buf, size_t len, uint32_t timeout_ms) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
        if (poll(&pfd, 1, (int)timeout_ms) <= 0) return -1;
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all_bounded(int fd, void* buf, size_t len, uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, (int)timeout_ms) <= 0) return -1;
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static void fed_server_handle_conn(keystone_fed_server_t* srv, int fd) {
    keystone_fed_req_t req;
    if (recv_all_bounded(fd, &req, sizeof(req), srv->cfg.conn_timeout_ms) != 0) {
        close(fd);
        return;
    }

    /* Hostile-frame gate: strict v1 validation before anything else. */
    if (req.magic != KEYSTONE_FED_QMAGIC || req.version != KEYSTONE_FED_QVERSION ||
        req.reserved != 0 || req.max_results == 0 || req.max_results > KEYSTONE_FED_MAX_RESULTS_CAP ||
        (req.msg_type == KEYSTONE_FED_QTYPE_TIMELINE_RANGE && req.payload_len != 0) ||
        (req.msg_type == KEYSTONE_FED_QTYPE_EXACT_LOOKUP && req.payload_len != sizeof(keystone_uuid_t)) ||
        (req.msg_type != KEYSTONE_FED_QTYPE_TIMELINE_RANGE && req.msg_type != KEYSTONE_FED_QTYPE_EXACT_LOOKUP)) {
        close(fd);
        return;
    }

    uint8_t payload[sizeof(keystone_uuid_t)] = {0};
    if (req.payload_len > 0 && recv_all_bounded(fd, payload, req.payload_len, srv->cfg.conn_timeout_ms) != 0) {
        close(fd);
        return;
    }

    uint32_t expect = fed_crc32(&req, FED_REQ_CRC_LEN);
    if (req.payload_len > 0) expect ^= fed_crc32(payload, req.payload_len);
    if (expect != req.crc32) {
        close(fd);
        return;
    }

    /* Unauthenticated callers get nothing (the coordinator always sends a
     * context; a NULL-equivalent zero context still yields nothing above
     * UNCLASSIFIED, and SENSITIVE/tenant rules apply). */
    keystone_fed_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic = KEYSTONE_FED_QMAGIC;
    resp.version = KEYSTONE_FED_QVERSION;
    resp.request_id = req.request_id;
    resp.node_generation = 0;
    if (srv->cfg.temporal) {
        keystone_hlc_t wm;
        size_t cnt = 0;
        if (keystone_temporal_index_watermark(srv->cfg.temporal, &wm, &cnt)) {
            resp.node_watermark = wm;
        }
    }

    const keystone_security_context_t* caller = &req.sec_ctx;
    uint32_t cap = req.max_results;
    uint8_t* body = NULL;
    size_t body_len = 0;

    if (req.msg_type == KEYSTONE_FED_QTYPE_TIMELINE_RANGE && srv->cfg.temporal) {
        keystone_temporal_entry_t* raw = (keystone_temporal_entry_t*)malloc((size_t)cap * sizeof(keystone_temporal_entry_t));
        if (raw) {
            size_t total = keystone_temporal_index_query_range(srv->cfg.temporal, &req.hlc_min, &req.hlc_max, raw, cap);
            size_t copied = total < cap ? total : cap;
            /* Server-side label enforcement: uncleared entries never leave
             * the node, regardless of what the coordinator would do. */
            size_t kept = 0;
            for (size_t i = 0; i < copied && kept < cap; i++) {
                if (fed_item_allowed(caller, raw[i].classification, raw[i].tenant_id, raw[i].flags)) {
                    raw[kept++] = raw[i];
                }
            }
            resp.status = 0;
            resp.result_count = (uint32_t)kept;
            body_len = kept * sizeof(keystone_temporal_entry_t);
            body = (uint8_t*)raw; /* reuse the buffer as the response body */
        } else {
            resp.status = 3;
        }
    } else if (req.msg_type == KEYSTONE_FED_QTYPE_EXACT_LOOKUP && srv->cfg.exact) {
        keystone_uuid_t id;
        memcpy(&id, payload, sizeof(id));
        keystone_exact_entry_t ent;
        if (keystone_exact_index_lookup(srv->cfg.exact, &id, &ent) == 0 &&
            fed_item_allowed(caller, ent.classification, ent.tenant_id, ent.flags)) {
            resp.status = 0;
            resp.result_count = 1;
            body_len = sizeof(ent);
            body = (uint8_t*)malloc(body_len);
            if (body) memcpy(body, &ent, body_len);
            else resp.status = 3;
        } else if (resp.status == 0) {
            resp.status = 1; /* not found (or denied — indistinguishable by design) */
        }
    } else {
        resp.status = 3; /* query type not served by this node */
    }

    resp.reserved = 0;
    resp.crc32 = fed_crc32(&resp, FED_RESP_CRC_LEN);
    if (body && body_len > 0) {
        resp.crc32 ^= fed_crc32(body, body_len);
    }

    send_all_bounded(fd, &resp, sizeof(resp), srv->cfg.conn_timeout_ms);
    if (body && body_len > 0) {
        send_all_bounded(fd, body, body_len, srv->cfg.conn_timeout_ms);
    }
    free(body);
    close(fd);
}

static void* fed_server_thread(void* arg) {
    keystone_fed_server_t* srv = (keystone_fed_server_t*)arg;
    uint32_t served = 0;
    uint32_t max_conn = srv->cfg.max_connections ? srv->cfg.max_connections : 256;
    while (srv->running && served < max_conn) {
        struct pollfd pfd = { .fd = srv->listen_fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, 200);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int fd = accept(srv->listen_fd, NULL, NULL);
            if (fd >= 0) {
                fed_server_handle_conn(srv, fd);
                served++;
            }
        }
    }
    return NULL;
}

keystone_fed_server_t* keystone_fed_server_create(const keystone_fed_server_config_t* config) {
    if (!config) return NULL;
    keystone_fed_server_t* srv = (keystone_fed_server_t*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    srv->cfg = *config;
    if (!srv->cfg.bind_addr) srv->cfg.bind_addr = "127.0.0.1";
    if (srv->cfg.conn_timeout_ms == 0) srv->cfg.conn_timeout_ms = 5000;
    srv->listen_fd = -1;
    return srv;
}

int keystone_fed_server_start(keystone_fed_server_t* server) {
    if (!server || server->running) return -1;

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) return -1;

    int one = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0; /* bind ephemeral; expose via bound_port */
    addr.sin_addr.s_addr = inet_addr(server->cfg.bind_addr);
    if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(server->listen_fd, 16) != 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    struct sockaddr_in got;
    socklen_t got_len = sizeof(got);
    if (getsockname(server->listen_fd, (struct sockaddr*)&got, &got_len) == 0) {
        server->bound_port = ntohs(got.sin_port);
    }

    server->running = 1;
    if (pthread_create(&server->thread, NULL, fed_server_thread, server) != 0) {
        server->running = 0;
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }
    return 0;
}

void keystone_fed_server_stop(keystone_fed_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    pthread_join(server->thread, NULL);
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
}

void keystone_fed_server_destroy(keystone_fed_server_t* server) {
    if (!server) return;
    keystone_fed_server_stop(server);
    free(server);
}

uint16_t keystone_fed_server_bound_port(const keystone_fed_server_t* server) {
    return server ? server->bound_port : 0;
}

/* --- Coordinator --- */

static int fed_connect_bounded(const char* host, uint16_t port, uint32_t timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
        if (poll(&pfd, 1, (int)timeout_ms) <= 0) {
            close(fd);
            return -1; /* timeout or poll failure */
        }
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
            close(fd);
            return -1;
        }
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

/*
 * One request/response exchange with a single node. On protocol-valid
 * success returns 0 and fills *out_entries (caller buffer, at least
 * *inout_cap entries) and *out_count; writes the node watermark.
 */
static keystone_fed_node_status_t fed_exchange(
    const keystone_fed_node_addr_t* node,
    keystone_fed_qtype_t qtype,
    const keystone_security_context_t* caller,
    const keystone_hlc_t* hmin,
    const keystone_hlc_t* hmax,
    const keystone_uuid_t* object_id,
    uint32_t timeout_ms,
    uint32_t max_results,
    keystone_temporal_entry_t* out_entries, /* timeline scratch (>= max_results) */
    keystone_exact_entry_t* out_exact,
    size_t* out_count,
    uint64_t* out_generation,
    keystone_hlc_t* out_watermark
) {
    int fd = fed_connect_bounded(node->host, node->port, timeout_ms);
    if (fd < 0) {
        /* Distinguish refused (fast) from timeout: a refused connect fails
         * the poll immediately with SO_ERROR != 0; a silent drop would use
         * the whole timeout. Both surface as UNREACHABLE/TIMEOUT below. */
        return KEYSTONE_FED_NODE_UNREACHABLE;
    }

    keystone_fed_req_t req;
    memset(&req, 0, sizeof(req));
    req.magic = KEYSTONE_FED_QMAGIC;
    req.version = KEYSTONE_FED_QVERSION;
    req.msg_type = (uint32_t)qtype;
    req.request_id = (uint64_t)getpid() << 32 | (uint64_t)(uintptr_t)&req;
    if (caller) req.sec_ctx = *caller;
    if (hmin) req.hlc_min = *hmin;
    if (hmax) req.hlc_max = *hmax;
    req.max_results = max_results ? max_results : 1;
    if (qtype == KEYSTONE_FED_QTYPE_EXACT_LOOKUP && object_id) {
        req.payload_len = sizeof(keystone_uuid_t);
    }
    req.crc32 = fed_crc32(&req, FED_REQ_CRC_LEN);
    if (req.payload_len > 0 && object_id) {
        req.crc32 ^= fed_crc32(object_id, sizeof(*object_id));
    }

    if (send_all_bounded(fd, &req, sizeof(req), timeout_ms) != 0) {
        close(fd);
        return KEYSTONE_FED_NODE_TIMEOUT;
    }
    if (req.payload_len > 0 && object_id &&
        send_all_bounded(fd, object_id, sizeof(*object_id), timeout_ms) != 0) {
        close(fd);
        return KEYSTONE_FED_NODE_TIMEOUT;
    }

    keystone_fed_resp_t resp;
    if (recv_all_bounded(fd, &resp, sizeof(resp), timeout_ms) != 0) {
        close(fd);
        return KEYSTONE_FED_NODE_TIMEOUT;
    }
    if (resp.magic != KEYSTONE_FED_QMAGIC || resp.version != KEYSTONE_FED_QVERSION ||
        resp.reserved != 0 || resp.result_count > KEYSTONE_FED_MAX_RESULTS_CAP) {
        close(fd);
        return KEYSTONE_FED_NODE_PROTOCOL;
    }

    size_t entry_sz = (qtype == KEYSTONE_FED_QTYPE_EXACT_LOOKUP)
        ? sizeof(keystone_exact_entry_t)
        : sizeof(keystone_temporal_entry_t);
    size_t body_len = 0;
    if (!checked_mul_size((size_t)resp.result_count, entry_sz, &body_len)) {
        close(fd);
        return KEYSTONE_FED_NODE_PROTOCOL;
    }
    if (resp.status != 0 || body_len == 0) {
        close(fd);
        if (out_count) *out_count = 0;
        if (out_generation) *out_generation = resp.node_generation;
        if (out_watermark) *out_watermark = resp.node_watermark;
        return (resp.status == 0 || resp.status == 1) ? KEYSTONE_FED_NODE_OK : KEYSTONE_FED_NODE_PROTOCOL;
    }

    uint8_t* body = (uint8_t*)malloc(body_len);
    if (!body) {
        close(fd);
        return KEYSTONE_FED_NODE_PROTOCOL;
    }
    if (recv_all_bounded(fd, body, body_len, timeout_ms) != 0) {
        free(body);
        close(fd);
        return KEYSTONE_FED_NODE_TIMEOUT;
    }

    uint32_t expect = fed_crc32(&resp, FED_RESP_CRC_LEN) ^ fed_crc32(body, body_len);
    if (expect != resp.crc32) {
        free(body);
        close(fd);
        return KEYSTONE_FED_NODE_PROTOCOL;
    }

    if (qtype == KEYSTONE_FED_QTYPE_EXACT_LOOKUP) {
        if (out_exact) memcpy(out_exact, body, sizeof(keystone_exact_entry_t));
        if (out_count) *out_count = 1;
    } else {
        size_t n = (size_t)resp.result_count;
        if (n > max_results) n = max_results;
        memcpy(out_entries, body, n * sizeof(keystone_temporal_entry_t));
        if (out_count) *out_count = n;
    }
    if (out_generation) *out_generation = resp.node_generation;
    if (out_watermark) *out_watermark = resp.node_watermark;
    free(body);
    close(fd);
    return KEYSTONE_FED_NODE_OK;
}

size_t keystone_fed_query_timeline_fanout(
    const keystone_fed_node_addr_t* nodes,
    size_t node_count,
    const keystone_security_context_t* caller,
    const keystone_hlc_t* hlc_min,
    const keystone_hlc_t* hlc_max,
    uint32_t timeout_ms,
    keystone_temporal_entry_t* out_entries,
    size_t max_results,
    keystone_fed_node_result_t* out_node_results
) {
    if (!nodes || !out_entries || max_results == 0) return 0;

    uint32_t cap = (uint32_t)(max_results < KEYSTONE_FED_MAX_RESULTS_CAP
        ? max_results : KEYSTONE_FED_MAX_RESULTS_CAP);

    keystone_temporal_entry_t** per_node =
        (keystone_temporal_entry_t**)calloc(node_count, sizeof(keystone_temporal_entry_t*));
    size_t* per_node_counts = (size_t*)calloc(node_count, sizeof(size_t));
    const keystone_temporal_entry_t** merge_in =
        (const keystone_temporal_entry_t**)calloc(node_count, sizeof(keystone_temporal_entry_t*));
    if (!per_node || !per_node_counts || !merge_in) {
        free(per_node); free(per_node_counts); free(merge_in);
        return 0;
    }

    for (size_t n = 0; n < node_count; n++) {
        if (out_node_results) {
            out_node_results[n].status = KEYSTONE_FED_NODE_UNREACHABLE;
            out_node_results[n].node_generation = 0;
            memset(&out_node_results[n].node_watermark, 0, sizeof(keystone_hlc_t));
        }
        per_node[n] = (keystone_temporal_entry_t*)malloc((size_t)cap * sizeof(keystone_temporal_entry_t));
        if (!per_node[n]) continue;

        uint64_t gen = 0;
        keystone_hlc_t wm;
        size_t got = 0;
        keystone_fed_node_status_t st = fed_exchange(
            &nodes[n], KEYSTONE_FED_QTYPE_TIMELINE_RANGE, caller, hlc_min, hlc_max,
            NULL, timeout_ms, cap, per_node[n], NULL, &got, &gen, &wm);
        per_node_counts[n] = got;
        merge_in[n] = per_node[n];
        if (out_node_results) {
            out_node_results[n].status = st;
            out_node_results[n].node_generation = gen;
            out_node_results[n].node_watermark = wm;
        }
    }

    /* Merge with the existing conflict-resolution core (HLC order, event-id
     * dedup), then label-filter into the caller buffer — defense in depth:
     * the responder already filtered, the coordinator enforces again. */
    keystone_temporal_entry_t* merged =
        (keystone_temporal_entry_t*)malloc((size_t)cap * node_count > 0
            ? (size_t)cap * node_count * sizeof(keystone_temporal_entry_t) : 1);
    size_t out_count = 0;
    if (merged) {
        keystone_partial_status_t pst;
        size_t merged_n = keystone_federated_merge_timelines(merge_in, per_node_counts, node_count,
                                                             merged, (size_t)cap * node_count, &pst);
        for (size_t i = 0; i < merged_n && out_count < max_results; i++) {
            if (!caller) break; /* unauthenticated: nothing leaves */
            if (fed_item_allowed(caller, merged[i].classification, merged[i].tenant_id, merged[i].flags)) {
                out_entries[out_count++] = merged[i];
            }
        }
        free(merged);
    }

    for (size_t n = 0; n < node_count; n++) free(per_node[n]);
    free(per_node); free(per_node_counts); free(merge_in);
    return out_count;
}

int keystone_fed_query_exact_fanout(
    const keystone_fed_node_addr_t* nodes,
    size_t node_count,
    const keystone_security_context_t* caller,
    const keystone_uuid_t* object_id,
    uint32_t timeout_ms,
    keystone_exact_entry_t* out_entry,
    keystone_fed_node_result_t* out_node_results
) {
    if (!nodes || !caller || !object_id || !out_entry) return -1;

    int found = 0;
    keystone_exact_entry_t best;
    memset(&best, 0, sizeof(best));

    for (size_t n = 0; n < node_count; n++) {
        if (out_node_results) {
            out_node_results[n].status = KEYSTONE_FED_NODE_UNREACHABLE;
            out_node_results[n].node_generation = 0;
            memset(&out_node_results[n].node_watermark, 0, sizeof(keystone_hlc_t));
        }

        keystone_exact_entry_t ent;
        memset(&ent, 0, sizeof(ent));
        size_t got = 0;
        uint64_t gen = 0;
        keystone_hlc_t wm;
        keystone_fed_node_status_t st = fed_exchange(
            &nodes[n], KEYSTONE_FED_QTYPE_EXACT_LOOKUP, caller, NULL, NULL,
            object_id, timeout_ms, 1, NULL, &ent, &got, &gen, &wm);
        if (out_node_results) {
            out_node_results[n].status = st;
            out_node_results[n].node_generation = gen;
            out_node_results[n].node_watermark = wm;
        }
        if (st != KEYSTONE_FED_NODE_OK || got == 0) continue;

        if (!found) {
            best = ent;
            found = 1;
        } else {
            /* Federation conflict rule: Epoch > Generation > HLC. */
            if (ent.fencing_epoch > best.fencing_epoch ||
                (ent.fencing_epoch == best.fencing_epoch &&
                 (ent.latest_generation > best.latest_generation ||
                  (ent.latest_generation == best.latest_generation &&
                   keystone_hlc_compare(&ent.latest_hlc, &best.latest_hlc) > 0)))) {
                best = ent;
            }
        }
    }

    if (!found) return -1;
    *out_entry = best;
    return 0;
}
