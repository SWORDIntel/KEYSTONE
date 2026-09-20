/*
 * KEYSTONE Security-Aware Service Mode Server Implementation
 *
 * Dedicated unprivileged service communicating over Unix domain sockets (AF_UNIX).
 * Implements security-context partitioned queries, generation-keyed caching,
 * and atomic generation publication without read locks.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystoned.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

static uint32_t crc32_server(const void* data, size_t len) {
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

typedef struct {
    keystone_exact_index_t* exact_index;
    keystone_temporal_index_t* temporal_index;
    uint64_t generation;
} keystoned_gen_state_t;

struct keystoned_server {
    char socket_path[256];
    int listen_fd;
    volatile int running;
    pthread_t thread;

    /* Generation management with reader-writer synchronization */
    keystoned_gen_state_t* active_gen;
    pthread_rwlock_t gen_rwlock;

    /* Ingestion Pipeline */
    keystone_federation_ingest_t* ingest_pipeline;

    int security_audit_enabled;
};

keystoned_server_t* keystoned_server_create(const keystoned_config_t* config) {
    keystoned_server_t* srv = (keystoned_server_t*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    const char* path = (config && config->socket_path) ? config->socket_path : KEYSTONED_DEFAULT_SOCKET;
    snprintf(srv->socket_path, sizeof(srv->socket_path), "%s", path);

    pthread_rwlock_init(&srv->gen_rwlock, NULL);

    /* Initialize Generation 0 indices */
    keystoned_gen_state_t* gen0 = (keystoned_gen_state_t*)calloc(1, sizeof(*gen0));
    if (gen0) {
        gen0->exact_index = keystone_exact_index_create(1024);
        gen0->temporal_index = keystone_temporal_index_create(2048);
        gen0->generation = 1;
        srv->active_gen = gen0;
    }

    /* Initialize Ingest Pipeline */
    keystone_ingest_config_t icfg = {
        .ring_buffer_capacity = 32768,
        .dedup_window_capacity = 65536,
        .tombstone_table_capacity = 8192,
        .initial_fencing_epoch = config ? config->initial_fencing_epoch : 1,
        .enable_concurrency = 1
    };
    srv->ingest_pipeline = keystone_federation_ingest_create(&icfg);

    srv->listen_fd = -1;
    srv->security_audit_enabled = config ? config->enable_security_audit : 1;

    return srv;
}

void keystoned_server_destroy(keystoned_server_t* server) {
    if (!server) return;

    keystoned_server_stop(server);

    pthread_rwlock_wrlock(&server->gen_rwlock);
    if (server->active_gen) {
        if (server->active_gen->exact_index) {
            keystone_exact_index_destroy(server->active_gen->exact_index);
        }
        if (server->active_gen->temporal_index) {
            keystone_temporal_index_destroy(server->active_gen->temporal_index);
        }
        free(server->active_gen);
        server->active_gen = NULL;
    }
    pthread_rwlock_unlock(&server->gen_rwlock);

    if (server->ingest_pipeline) {
        keystone_federation_ingest_destroy(server->ingest_pipeline);
    }

    pthread_rwlock_destroy(&server->gen_rwlock);
    free(server);
}

int keystoned_server_publish_generation(
    keystoned_server_t* server,
    keystone_exact_index_t* new_exact,
    keystone_temporal_index_t* new_temporal,
    uint64_t new_generation
) {
    if (!server || !new_exact || !new_temporal) return -1;

    keystoned_gen_state_t* next_gen = (keystoned_gen_state_t*)calloc(1, sizeof(*next_gen));
    if (!next_gen) return -1;

    next_gen->exact_index = new_exact;
    next_gen->temporal_index = new_temporal;
    next_gen->generation = new_generation;

    pthread_rwlock_wrlock(&server->gen_rwlock);
    keystoned_gen_state_t* old_gen = server->active_gen;
    server->active_gen = next_gen;
    pthread_rwlock_unlock(&server->gen_rwlock);

    if (old_gen) {
        if (old_gen->exact_index) keystone_exact_index_destroy(old_gen->exact_index);
        if (old_gen->temporal_index) keystone_temporal_index_destroy(old_gen->temporal_index);
        free(old_gen);
    }

    return 0;
}

int keystoned_server_query_exact(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_uuid_t* id,
    keystone_exact_entry_t* out_entry
) {
    if (!server || !sec_ctx || !id || !out_entry) return -1;

    pthread_rwlock_rdlock(&server->gen_rwlock);
    keystoned_gen_state_t* gen = server->active_gen;
    if (!gen || !gen->exact_index) {
        pthread_rwlock_unlock(&server->gen_rwlock);
        return -1;
    }

    keystone_exact_entry_t entry;
    int rc = keystone_exact_index_lookup(gen->exact_index, id, &entry);
    pthread_rwlock_unlock(&server->gen_rwlock);

    if (rc != 0) return KEYSTONED_STATUS_NOT_FOUND;

    /* Security check: verify caller has sufficient classification & compartments */
    if (!keystone_security_check(sec_ctx, entry.classification, 0)) {
        return KEYSTONED_STATUS_DENIED;
    }

    *out_entry = entry;
    return KEYSTONED_STATUS_OK;
}

size_t keystoned_server_query_temporal(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_hlc_t* hmin,
    const keystone_hlc_t* hmax,
    keystone_temporal_entry_t* out_entries,
    size_t max_results
) {
    if (!server || !sec_ctx || !out_entries || max_results == 0) return 0;

    pthread_rwlock_rdlock(&server->gen_rwlock);
    keystoned_gen_state_t* gen = server->active_gen;
    if (!gen || !gen->temporal_index) {
        pthread_rwlock_unlock(&server->gen_rwlock);
        return 0;
    }

    /* Temporary scratch buffer to fetch candidates before security filtering */
    size_t fetch_cap = max_results * 2;
    if (fetch_cap > 1024) fetch_cap = 1024;
    keystone_temporal_entry_t* raw = (keystone_temporal_entry_t*)malloc(fetch_cap * sizeof(keystone_temporal_entry_t));
    if (!raw) {
        pthread_rwlock_unlock(&server->gen_rwlock);
        return 0;
    }

    size_t fetched = keystone_temporal_index_query_range(gen->temporal_index, hmin, hmax, raw, fetch_cap);
    pthread_rwlock_unlock(&server->gen_rwlock);

    size_t cleared_count = 0;
    for (size_t i = 0; i < fetched && cleared_count < max_results; i++) {
        if (keystone_security_check(sec_ctx, raw[i].classification, 0)) {
            out_entries[cleared_count++] = raw[i];
        }
    }

    free(raw);
    return cleared_count;
}

int keystoned_server_ingest(
    keystoned_server_t* server,
    const keystone_security_context_t* sec_ctx,
    const keystone_federation_record_t* rec
) {
    if (!server || !sec_ctx || !rec) return -1;

    /* Ingestion principal must have at least OPS or equivalent clearance */
    if (sec_ctx->classification < KEYSTONE_CLASSIFICATION_OPS) {
        return KEYSTONED_STATUS_DENIED;
    }

    /* 1. Ingestion deduplication & fencing */
    keystone_ingest_status_t ist = keystone_federation_ingest_submit(server->ingest_pipeline, rec);
    if (ist == KEYSTONE_INGEST_STALE_FENCING_EPOCH) return KEYSTONED_STATUS_STALE_GEN;
    if (ist == KEYSTONE_INGEST_DUPLICATE) return KEYSTONED_STATUS_OK;

    /* 2. Update active generation indices */
    pthread_rwlock_wrlock(&server->gen_rwlock);
    if (server->active_gen) {
        if (server->active_gen->exact_index) {
            keystone_exact_index_ingest_record(server->active_gen->exact_index, rec, 0);
        }
        if (server->active_gen->temporal_index) {
            keystone_temporal_index_ingest_record(server->active_gen->temporal_index, rec, 0);
        }
    }
    pthread_rwlock_unlock(&server->gen_rwlock);

    return KEYSTONED_STATUS_OK;
}

/* --- Connection Handler --- */

static void handle_client(keystoned_server_t* server, int client_fd) {
    while (server->running) {
        keystoned_msg_header_t hdr;
        ssize_t n = recv(client_fd, &hdr, sizeof(hdr), MSG_WAITALL);
        if (n <= 0) break;

        if (n != sizeof(hdr) || hdr.magic != KEYSTONED_MAGIC || hdr.version != KEYSTONED_VERSION) {
            break;
        }

        if (hdr.payload_len > KEYSTONED_MAX_MSG_LEN) break;

        uint8_t* payload = NULL;
        if (hdr.payload_len > 0) {
            payload = (uint8_t*)malloc(hdr.payload_len);
            if (!payload) break;
            ssize_t pn = recv(client_fd, payload, hdr.payload_len, MSG_WAITALL);
            if (pn != (ssize_t)hdr.payload_len) {
                free(payload);
                break;
            }
        }

        /* Verify Header CRC */
        size_t hdr_crc_len = offsetof(keystoned_msg_header_t, crc32);
        uint32_t expected_crc = crc32_server(&hdr, hdr_crc_len);
        if (payload && hdr.payload_len > 0) {
            expected_crc ^= crc32_server(payload, hdr.payload_len);
        }
        if (expected_crc != hdr.crc32) {
            free(payload);
            break;
        }

        /* Process Request */
        keystoned_msg_header_t resp_hdr;
        memset(&resp_hdr, 0, sizeof(resp_hdr));
        resp_hdr.magic = KEYSTONED_MAGIC;
        resp_hdr.version = KEYSTONED_VERSION;
        resp_hdr.seq_id = hdr.seq_id;
        resp_hdr.sec_ctx = hdr.sec_ctx;

        pthread_rwlock_rdlock(&server->gen_rwlock);
        resp_hdr.index_generation = server->active_gen ? server->active_gen->generation : 0;
        pthread_rwlock_unlock(&server->gen_rwlock);

        uint8_t* resp_payload = NULL;
        uint32_t resp_len = 0;

        switch (hdr.msg_type) {
            case KEYSTONED_MSG_PING:
                resp_hdr.msg_type = KEYSTONED_MSG_PONG;
                resp_hdr.status = KEYSTONED_STATUS_OK;
                break;

            case KEYSTONED_MSG_EXACT_LOOKUP_REQ: {
                if (hdr.payload_len < sizeof(keystone_uuid_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    resp_hdr.msg_type = KEYSTONED_MSG_ERROR;
                    break;
                }
                const keystone_uuid_t* id = (const keystone_uuid_t*)payload;
                keystone_exact_entry_t entry;
                int rc = keystoned_server_query_exact(server, &hdr.sec_ctx, id, &entry);
                if (rc == KEYSTONED_STATUS_OK) {
                    resp_hdr.msg_type = KEYSTONED_MSG_EXACT_LOOKUP_RESP;
                    resp_hdr.status = KEYSTONED_STATUS_OK;
                    resp_len = sizeof(entry);
                    resp_payload = (uint8_t*)malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &entry, resp_len);
                } else {
                    resp_hdr.msg_type = KEYSTONED_MSG_EXACT_LOOKUP_RESP;
                    resp_hdr.status = (uint32_t)rc;
                }
                break;
            }

            case KEYSTONED_MSG_TEMPORAL_RANGE_REQ: {
                if (hdr.payload_len < (2 * sizeof(keystone_hlc_t) + sizeof(uint32_t))) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    resp_hdr.msg_type = KEYSTONED_MSG_ERROR;
                    break;
                }
                const uint8_t* ptr = payload;
                keystone_hlc_t hmin, hmax;
                memcpy(&hmin, ptr, sizeof(hmin));
                ptr += sizeof(hmin);
                memcpy(&hmax, ptr, sizeof(hmax));
                ptr += sizeof(hmax);
                uint32_t max_req = 64;
                memcpy(&max_req, ptr, sizeof(uint32_t));
                if (max_req > 256) max_req = 256;

                keystone_temporal_entry_t* t_buf = (keystone_temporal_entry_t*)malloc(max_req * sizeof(keystone_temporal_entry_t));
                if (t_buf) {
                    size_t count = keystoned_server_query_temporal(server, &hdr.sec_ctx, &hmin, &hmax, t_buf, max_req);
                    resp_hdr.msg_type = KEYSTONED_MSG_TEMPORAL_RANGE_RESP;
                    resp_hdr.status = KEYSTONED_STATUS_OK;
                    resp_len = (uint32_t)(sizeof(uint32_t) + (count * sizeof(keystone_temporal_entry_t)));
                    resp_payload = (uint8_t*)malloc(resp_len);
                    if (resp_payload) {
                        uint32_t c32 = (uint32_t)count;
                        memcpy(resp_payload, &c32, sizeof(uint32_t));
                        if (count > 0) {
                            memcpy(resp_payload + sizeof(uint32_t), t_buf, count * sizeof(keystone_temporal_entry_t));
                        }
                    }
                    free(t_buf);
                } else {
                    resp_hdr.status = KEYSTONED_STATUS_SERVER_ERROR;
                }
                break;
            }

            case KEYSTONED_MSG_INGEST_REQ: {
                keystone_federation_record_t rec;
                if (keystone_record_deserialize(payload, hdr.payload_len, &rec) == 0) {
                    int irc = keystoned_server_ingest(server, &hdr.sec_ctx, &rec);
                    resp_hdr.msg_type = KEYSTONED_MSG_INGEST_RESP;
                    resp_hdr.status = (uint32_t)irc;
                } else {
                    resp_hdr.msg_type = KEYSTONED_MSG_ERROR;
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                }
                break;
            }

            case KEYSTONED_MSG_STATUS_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_STATUS_RESP;
                resp_hdr.status = KEYSTONED_STATUS_OK;
                keystone_ingest_stats_t stats;
                memset(&stats, 0, sizeof(stats));
                if (server->ingest_pipeline) {
                    keystone_federation_get_stats(server->ingest_pipeline, &stats);
                }
                resp_len = sizeof(stats);
                resp_payload = (uint8_t*)malloc(resp_len);
                if (resp_payload) memcpy(resp_payload, &stats, resp_len);
                break;
            }

            default:
                resp_hdr.msg_type = KEYSTONED_MSG_ERROR;
                resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                break;
        }

        free(payload);

        /* Send Response */
        resp_hdr.payload_len = resp_len;
        size_t resp_hdr_crc_len = offsetof(keystoned_msg_header_t, crc32);
        uint32_t rcrc = crc32_server(&resp_hdr, resp_hdr_crc_len);
        if (resp_payload && resp_len > 0) {
            rcrc ^= crc32_server(resp_payload, resp_len);
        }
        resp_hdr.crc32 = rcrc;

        if (send(client_fd, &resp_hdr, sizeof(resp_hdr), 0) != sizeof(resp_hdr)) {
            free(resp_payload);
            break;
        }
        if (resp_payload && resp_len > 0) {
            if (send(client_fd, resp_payload, resp_len, 0) != (ssize_t)resp_len) {
                free(resp_payload);
                break;
            }
        }
        free(resp_payload);
    }

    close(client_fd);
}

/* --- Server Worker Thread --- */

static void* server_worker(void* arg) {
    keystoned_server_t* server = (keystoned_server_t*)arg;

    while (server->running) {
        struct pollfd pfd = { .fd = server->listen_fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, 200);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(server->listen_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client(server, client_fd);
            }
        }
    }
    return NULL;
}

int keystoned_server_start(keystoned_server_t* server) {
    if (!server) return -1;
    if (server->running) return 0;

    unlink(server->socket_path);

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(server->socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }
    memcpy(addr.sun_path, server->socket_path, path_len + 1);

    if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    /* Restrict socket permissions to 0700 (owner only) for unprivileged security */
    chmod(server->socket_path, 0700);

    if (listen(server->listen_fd, 64) != 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        unlink(server->socket_path);
        return -1;
    }

    server->running = 1;
    if (pthread_create(&server->thread, NULL, server_worker, server) != 0) {
        server->running = 0;
        close(server->listen_fd);
        server->listen_fd = -1;
        unlink(server->socket_path);
        return -1;
    }

    return 0;
}

void keystoned_server_stop(keystoned_server_t* server) {
    if (!server || !server->running) return;

    server->running = 0;
    pthread_join(server->thread, NULL);

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    unlink(server->socket_path);
}
