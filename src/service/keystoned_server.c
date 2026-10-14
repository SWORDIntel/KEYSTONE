/*
 * KEYSTONE Security-Aware Service Mode Server Implementation
 *
 * Dedicated unprivileged service communicating over Unix domain sockets (AF_UNIX).
 * Implements security-context partitioned queries, generation-keyed caching,
 * and atomic generation publication without read locks.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "keystoned.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

static uint32_t crc32_table[256];
static pthread_once_t crc32_once = PTHREAD_ONCE_INIT;

static void init_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
}

static uint32_t crc32_server(const void* data, size_t len) {
    pthread_once(&crc32_once, init_crc32_table);
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct {
    keystone_exact_index_t* exact_index;
    keystone_temporal_index_t* temporal_index;
    uint64_t generation;
} keystoned_gen_state_t;

#define KEYSTONED_MAX_PEER_BINDINGS 64
#define KEYSTONED_MAX_CLIENTS 32

typedef struct {
    uid_t uid;
    keystone_security_context_t sec_ctx;
    int is_set;
} keystoned_peer_binding_t;

typedef struct {
    struct keystoned_server* server;
    pthread_t thread;
    int fd;
    /* 0 = free, 1 = active, 2 = finished, 3 = being joined. */
    int state;
} keystoned_client_slot_t;

struct keystoned_server {
    char socket_path[256];
    int listen_fd;
    atomic_int running;
    pthread_t thread;
    keystoned_client_slot_t clients[KEYSTONED_MAX_CLIENTS];
    pthread_mutex_t clients_mutex;

    /* Generation management with reader-writer synchronization */
    keystoned_gen_state_t* active_gen;
    pthread_rwlock_t gen_rwlock;

    /* Ingestion Pipeline */
    keystone_federation_ingest_t* ingest_pipeline;
    keystone_workflow_index_t* workflow_index;
    pthread_mutex_t workflow_mutex;
    /* A staged replay belongs to one authenticated socket connection. */
    pthread_mutex_t workflow_stage_owner_mutex;
    int workflow_stage_owner_fd;

    int security_audit_enabled;

    /* Authenticated transport peer bindings */
    keystoned_peer_binding_t peer_bindings[KEYSTONED_MAX_PEER_BINDINGS];
    size_t num_peer_bindings;
    pthread_mutex_t bindings_mutex;
};

keystoned_server_t* keystoned_server_create(const keystoned_config_t* config) {
    keystoned_server_t* srv = (keystoned_server_t*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    const char* path = (config && config->socket_path) ? config->socket_path : KEYSTONED_DEFAULT_SOCKET;
    snprintf(srv->socket_path, sizeof(srv->socket_path), "%s", path);
    srv->listen_fd = -1;
    for (size_t i = 0; i < KEYSTONED_MAX_CLIENTS; ++i)
        srv->clients[i].fd = -1;

    pthread_rwlock_init(&srv->gen_rwlock, NULL);
    pthread_mutex_init(&srv->clients_mutex, NULL);
    pthread_mutex_init(&srv->bindings_mutex, NULL);
    pthread_mutex_init(&srv->workflow_stage_owner_mutex, NULL);
    srv->workflow_stage_owner_fd = -1;
    pthread_mutex_init(&srv->workflow_mutex, NULL);

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
    /* Live replay starts from zero. Restoring a checkpoint requires proof of
     * its committed prefix, which a first-row history digest cannot supply. */
    srv->workflow_index = keystone_workflow_index_create(0);
    if (!srv->active_gen || !srv->active_gen->exact_index ||
        !srv->active_gen->temporal_index || !srv->ingest_pipeline ||
        !srv->workflow_index) {
        keystoned_server_destroy(srv);
        return NULL;
    }

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
    keystone_workflow_index_destroy(server->workflow_index);

    pthread_rwlock_destroy(&server->gen_rwlock);
    pthread_mutex_destroy(&server->clients_mutex);
    pthread_mutex_destroy(&server->bindings_mutex);
    pthread_mutex_destroy(&server->workflow_mutex);
    pthread_mutex_destroy(&server->workflow_stage_owner_mutex);
    free(server);
}

int keystoned_server_bind_peer_principal(
    keystoned_server_t* server,
    uid_t uid,
    const keystone_security_context_t* sec_ctx
) {
    if (!server || !sec_ctx) return -1;
    pthread_mutex_lock(&server->bindings_mutex);
    for (size_t i = 0; i < server->num_peer_bindings; i++) {
        if (server->peer_bindings[i].uid == uid) {
            server->peer_bindings[i].sec_ctx = *sec_ctx;
            server->peer_bindings[i].is_set = 1;
            pthread_mutex_unlock(&server->bindings_mutex);
            return 0;
        }
    }
    if (server->num_peer_bindings < KEYSTONED_MAX_PEER_BINDINGS) {
        server->peer_bindings[server->num_peer_bindings].uid = uid;
        server->peer_bindings[server->num_peer_bindings].sec_ctx = *sec_ctx;
        server->peer_bindings[server->num_peer_bindings].is_set = 1;
        server->num_peer_bindings++;
        pthread_mutex_unlock(&server->bindings_mutex);
        return 0;
    }
    pthread_mutex_unlock(&server->bindings_mutex);
    return -1;
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

/*
 * Query-time authorization (sweep 2026-09-28 #5/#6 + Phase 8 SCI): classification floor,
 * tenant isolation (untagged tenant 0 = legacy, readable by any cleared
 * caller), SECURITY_SENSITIVE flag gated at SECRET or above, and strict
 * SCI compartment bitmask enforcement with zero metadata leakage.
 */
static int ksd_entry_allowed(const keystone_security_context_t* ctx,
                             uint32_t classification, uint32_t tenant_id, uint32_t flags,
                             uint32_t sci) {
    if (!ctx) return 0;
    if (ctx->classification < classification) return 0;
    if (tenant_id != 0u && tenant_id != ctx->tenant_id) return 0;
    if ((flags & KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE) &&
        ctx->classification < KEYSTONE_CLASSIFICATION_SECRET) {
        return 0;
    }
    if (sci != 0u && (sci & ~ctx->compartment_mask) != 0u) {
        return 0;
    }
    return 1;
}

static int ksd_workflow_status(keystone_workflow_result_t result) {
    switch (result) {
        case KEYSTONE_WORKFLOW_OK: return KEYSTONED_STATUS_OK;
        case KEYSTONE_WORKFLOW_BAD_ARGUMENT: return KEYSTONED_STATUS_INVALID_ARG;
        case KEYSTONE_WORKFLOW_GAP: return KEYSTONED_STATUS_GAP;
        case KEYSTONE_WORKFLOW_DUPLICATE:
        case KEYSTONE_WORKFLOW_CONFLICT: return KEYSTONED_STATUS_CONFLICT;
        case KEYSTONE_WORKFLOW_STALE: return KEYSTONED_STATUS_STALE_GEN;
        default: return KEYSTONED_STATUS_SERVER_ERROR;
    }
}

static uint32_t ksd_response_type(uint32_t request_type) {
    switch (request_type) {
        case KEYSTONED_MSG_EXACT_LOOKUP_REQ: return KEYSTONED_MSG_EXACT_LOOKUP_RESP;
        case KEYSTONED_MSG_TEMPORAL_RANGE_REQ: return KEYSTONED_MSG_TEMPORAL_RANGE_RESP;
        case KEYSTONED_MSG_INGEST_REQ: return KEYSTONED_MSG_INGEST_RESP;
        case KEYSTONED_MSG_WORKFLOW_APPLY_REQ: return KEYSTONED_MSG_WORKFLOW_APPLY_RESP;
        case KEYSTONED_MSG_WORKFLOW_PAGE_REQ: return KEYSTONED_MSG_WORKFLOW_PAGE_RESP;
        case KEYSTONED_MSG_WORKFLOW_WATERMARK_REQ: return KEYSTONED_MSG_WORKFLOW_WATERMARK_RESP;
        case KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_REQ: return KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_RESP;
        case KEYSTONED_MSG_WORKFLOW_STAGE_REQ: return KEYSTONED_MSG_WORKFLOW_STAGE_RESP;
        case KEYSTONED_MSG_WORKFLOW_PUBLISH_REQ: return KEYSTONED_MSG_WORKFLOW_PUBLISH_RESP;
        case KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_REQ: return KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_RESP;
        case KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_REQ: return KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_RESP;
        default: return KEYSTONED_MSG_ERROR;
    }
}

int keystoned_server_workflow_bind_history(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE],
    uint64_t* out_watermark
) {
    if (!server || !sec_ctx || !identity || !out_watermark)
        return KEYSTONED_STATUS_INVALID_ARG;
    if (sec_ctx->classification != KEYSTONE_CLASSIFICATION_TOP_SECRET ||
        sec_ctx->compartment_mask != UINT64_MAX)
        return KEYSTONED_STATUS_DENIED;
    pthread_mutex_lock(&server->workflow_mutex);
    int status = ksd_workflow_status(keystone_workflow_index_bind_history(
        server->workflow_index, identity));
    if (status == KEYSTONED_STATUS_OK)
        *out_watermark = keystone_workflow_index_watermark(server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return status;
}

int keystoned_server_workflow_apply(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive, uint64_t through_inclusive,
    const keystone_workflow_event_t* events, size_t count,
    uint64_t* out_watermark
) {
    if (!server || !sec_ctx || !out_watermark || (count && !events))
        return KEYSTONED_STATUS_INVALID_ARG;
    /* Only the dedicated, full-clearance committed-log projector may mutate
     * this derived index. A lower-clearance caller must not relabel or delete
     * a row by presenting a weaker label in its event. */
    if (sec_ctx->classification != KEYSTONE_CLASSIFICATION_TOP_SECRET ||
        sec_ctx->compartment_mask != UINT64_MAX)
        return KEYSTONED_STATUS_DENIED;
    for (size_t i = 0; i < count; ++i) {
        const keystone_workflow_entry_t* entry = &events[i].entry;
        if (entry->tenant != sec_ctx->tenant_id ||
            !ksd_entry_allowed(sec_ctx, entry->classification, entry->tenant,
                               entry->flags, entry->sci))
            return KEYSTONED_STATUS_DENIED;
    }
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    int status = ksd_workflow_status(keystone_workflow_index_ingest_batch(
        server->workflow_index, from_exclusive, through_inclusive,
        events, count));
    if (status == KEYSTONED_STATUS_OK)
        *out_watermark = keystone_workflow_index_watermark(server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return status;
}

static int ksd_workflow_operator(const keystone_security_context_t* sec_ctx) {
    return sec_ctx &&
        sec_ctx->classification == KEYSTONE_CLASSIFICATION_TOP_SECRET &&
        sec_ctx->compartment_mask == UINT64_MAX;
}

int keystoned_server_workflow_stage(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive, uint64_t through_inclusive,
    const keystone_workflow_event_t* events, size_t count,
    uint64_t* out_stage_watermark
) {
    if (!server || !sec_ctx || !out_stage_watermark || (count && !events) ||
        count > KEYSTONE_WORKFLOW_BATCH_MAX)
        return KEYSTONED_STATUS_INVALID_ARG;
    if (!ksd_workflow_operator(sec_ctx)) return KEYSTONED_STATUS_DENIED;
    for (size_t i = 0; i < count; ++i) {
        const keystone_workflow_entry_t* entry = &events[i].entry;
        if (entry->tenant != sec_ctx->tenant_id ||
            !ksd_entry_allowed(sec_ctx, entry->classification, entry->tenant,
                               entry->flags, entry->sci))
            return KEYSTONED_STATUS_DENIED;
    }
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    int status = ksd_workflow_status(keystone_workflow_index_stage_batch(
        server->workflow_index, from_exclusive, through_inclusive,
        events, count));
    if (status == KEYSTONED_STATUS_OK)
        *out_stage_watermark = keystone_workflow_index_stage_watermark(
            server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return status;
}

int keystoned_server_workflow_publish(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    uint64_t expected_target, uint64_t* out_watermark
) {
    if (!server || !sec_ctx || !out_watermark)
        return KEYSTONED_STATUS_INVALID_ARG;
    if (!ksd_workflow_operator(sec_ctx)) return KEYSTONED_STATUS_DENIED;
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    int status = ksd_workflow_status(keystone_workflow_index_publish(
        server->workflow_index, expected_target));
    if (status == KEYSTONED_STATUS_OK)
        *out_watermark = keystone_workflow_index_watermark(server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return status;
}

int keystoned_server_workflow_abort_stage(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
) {
    if (!server || !sec_ctx || !out_watermark)
        return KEYSTONED_STATUS_INVALID_ARG;
    if (!ksd_workflow_operator(sec_ctx)) return KEYSTONED_STATUS_DENIED;
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    int status = ksd_workflow_status(keystone_workflow_index_abort_stage(
        server->workflow_index));
    if (status == KEYSTONED_STATUS_OK)
        *out_watermark = keystone_workflow_index_watermark(server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return status;
}

int keystoned_server_workflow_stage_watermark(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
) {
    if (!server || !sec_ctx || !out_watermark)
        return KEYSTONED_STATUS_INVALID_ARG;
    if (!ksd_workflow_operator(sec_ctx)) return KEYSTONED_STATUS_DENIED;
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    *out_watermark = keystone_workflow_index_stage_watermark(server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return KEYSTONED_STATUS_OK;
}

int keystoned_server_workflow_page(
    keystoned_server_t* server, const keystone_security_context_t* sec_ctx,
    const keystone_workflow_query_t* query,
    keystone_workflow_entry_t* out_entries, size_t capacity,
    keystone_workflow_page_t* out_page
) {
    if (!server || !sec_ctx || !query || !out_entries || !out_page)
        return KEYSTONED_STATUS_INVALID_ARG;
    if (query->tenant != sec_ctx->tenant_id ||
        query->max_classification > sec_ctx->classification ||
        (query->sci & ~sec_ctx->compartment_mask) != 0)
        return KEYSTONED_STATUS_DENIED;
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    int status = ksd_workflow_status(keystone_workflow_index_page(
        server->workflow_index, query, out_entries, capacity, out_page));
    pthread_mutex_unlock(&server->workflow_mutex);
    return status;
}

int keystoned_server_workflow_watermark(
    keystoned_server_t* server, uint64_t* out_watermark
) {
    if (!server || !out_watermark) return KEYSTONED_STATUS_INVALID_ARG;
    pthread_mutex_lock(&server->workflow_mutex);
    if (!keystone_workflow_index_history_verified(server->workflow_index)) {
        pthread_mutex_unlock(&server->workflow_mutex);
        return KEYSTONED_STATUS_STALE_GEN;
    }
    *out_watermark = keystone_workflow_index_watermark(server->workflow_index);
    pthread_mutex_unlock(&server->workflow_mutex);
    return KEYSTONED_STATUS_OK;
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

    if (!ksd_entry_allowed(sec_ctx, entry.classification, entry.tenant_id, entry.flags, entry.sci)) {
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

    /*
     * query_range returns the TOTAL match count; only fetch_cap entries were
     * actually copied into raw. Iterating past fetch_cap read out of bounds
     * for in-process callers with max_results > 1024 (sweep #11).
     */
    size_t total_matches = keystone_temporal_index_query_range(gen->temporal_index, hmin, hmax, raw, fetch_cap);
    pthread_rwlock_unlock(&server->gen_rwlock);
    size_t copied = total_matches < fetch_cap ? total_matches : fetch_cap;

    size_t cleared_count = 0;
    for (size_t i = 0; i < copied && cleared_count < max_results; i++) {
        if (ksd_entry_allowed(sec_ctx, raw[i].classification, raw[i].tenant_id, raw[i].flags, raw[i].sci)) {
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

    /* Ingestion principal must have at least OPS or equivalent clearance,
     * may only ingest records for its own tenant (sweep #5), and must hold
     * every SCI compartment the record requires (criterion 8). */
    if (sec_ctx->classification < KEYSTONE_CLASSIFICATION_OPS) {
        return KEYSTONED_STATUS_DENIED;
    }
    if (rec->tenant_id != sec_ctx->tenant_id) {
        return KEYSTONED_STATUS_DENIED;
    }
    if (rec->sci != 0u && (rec->sci & ~sec_ctx->compartment_mask) != 0u) {
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

static int send_all(int fd, const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    while (length > 0) {
        ssize_t sent = send(fd, bytes, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        bytes += (size_t)sent;
        length -= (size_t)sent;
    }
    return 0;
}

static void handle_client(keystoned_server_t* server, int client_fd) {
    /*
     * Peer authentication (sweep #4): the security context in each message
     * is client-asserted; the socket restricts WHO can connect, and this
     * credential check pins that to the daemon's own effective user. A
     * cross-user process that reaches the socket is dropped here.
     */
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred_len != sizeof(cred) || cred.uid != geteuid()) {
        return;
    }

    /*
     * Peer authentication: SO_PEERCRED checks peer UID. If a peer principal
     * binding is registered on the server, bind the transport to it immediately.
     */
    int transport_bound = 0;
    int trusted_binding = 0;
    keystone_security_context_t bound_ctx;
    memset(&bound_ctx, 0, sizeof(bound_ctx));

    pthread_mutex_lock(&server->bindings_mutex);
    for (size_t i = 0; i < server->num_peer_bindings; i++) {
        if (server->peer_bindings[i].is_set && server->peer_bindings[i].uid == cred.uid) {
            bound_ctx = server->peer_bindings[i].sec_ctx;
            transport_bound = 1;
            trusted_binding = 1;
            break;
        }
    }
    pthread_mutex_unlock(&server->bindings_mutex);

    /* Bound a partially received frame, while allowing authenticated local
     * clients to stay connected during a long upstream projection. The
     * service handles clients on separate bounded worker slots. */
    struct timeval rcv_to = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &rcv_to, sizeof(rcv_to));

    while (server->running) {
        keystoned_msg_header_t hdr;
        ssize_t n = recv(client_fd, &hdr, sizeof(hdr), MSG_WAITALL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR))
            continue;
        if (n <= 0) break;

        if (n != sizeof(hdr) || hdr.magic != KEYSTONED_MAGIC || hdr.version != KEYSTONED_VERSION) {
            break;
        }

        /* Strict v1 wire format: the spare word must be zero (sweep #4). */
        if (hdr.reserved != 0) break;

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

        /* Enforce transport-bound principal if bound */
        keystone_security_context_t effective_sec = hdr.sec_ctx;
        if (transport_bound && hdr.msg_type != KEYSTONED_MSG_PING &&
            hdr.msg_type != KEYSTONED_MSG_STATUS_REQ &&
            hdr.msg_type != KEYSTONED_MSG_BIND_PRINCIPAL_REQ) {
            if (hdr.sec_ctx.tenant_id != 0 && hdr.sec_ctx.tenant_id != bound_ctx.tenant_id) {
                resp_hdr.status = KEYSTONED_STATUS_DENIED;
                resp_hdr.msg_type = ksd_response_type(hdr.msg_type);
                goto send_reply;
            }
            if (hdr.sec_ctx.classification > bound_ctx.classification) {
                resp_hdr.status = KEYSTONED_STATUS_DENIED;
                resp_hdr.msg_type = ksd_response_type(hdr.msg_type);
                goto send_reply;
            }
            if ((hdr.sec_ctx.compartment_mask & ~bound_ctx.compartment_mask) != 0) {
                resp_hdr.status = KEYSTONED_STATUS_DENIED;
                resp_hdr.msg_type = ksd_response_type(hdr.msg_type);
                goto send_reply;
            }
            effective_sec = bound_ctx;
            if (hdr.sec_ctx.classification < bound_ctx.classification) {
                effective_sec.classification = hdr.sec_ctx.classification;
            }
            effective_sec.compartment_mask = hdr.sec_ctx.compartment_mask & bound_ctx.compartment_mask;
        }

        switch (hdr.msg_type) {
            case KEYSTONED_MSG_PING:
                resp_hdr.msg_type = KEYSTONED_MSG_PONG;
                resp_hdr.status = KEYSTONED_STATUS_OK;
                break;

            case KEYSTONED_MSG_BIND_PRINCIPAL_REQ: {
                if (transport_bound) {
                    if (hdr.sec_ctx.tenant_id != 0 && hdr.sec_ctx.tenant_id != bound_ctx.tenant_id) {
                        resp_hdr.msg_type = KEYSTONED_MSG_BIND_PRINCIPAL_RESP;
                        resp_hdr.status = KEYSTONED_STATUS_DENIED;
                        break;
                    }
                    if (hdr.sec_ctx.classification > bound_ctx.classification) {
                        resp_hdr.msg_type = KEYSTONED_MSG_BIND_PRINCIPAL_RESP;
                        resp_hdr.status = KEYSTONED_STATUS_DENIED;
                        break;
                    }
                    if ((hdr.sec_ctx.compartment_mask & ~bound_ctx.compartment_mask) != 0) {
                        resp_hdr.msg_type = KEYSTONED_MSG_BIND_PRINCIPAL_RESP;
                        resp_hdr.status = KEYSTONED_STATUS_DENIED;
                        break;
                    }
                }
                bound_ctx = hdr.sec_ctx;
                transport_bound = 1;
                resp_hdr.msg_type = KEYSTONED_MSG_BIND_PRINCIPAL_RESP;
                resp_hdr.status = KEYSTONED_STATUS_OK;
                break;
            }

            case KEYSTONED_MSG_EXACT_LOOKUP_REQ: {
                if (hdr.payload_len < sizeof(keystone_uuid_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    resp_hdr.msg_type = KEYSTONED_MSG_ERROR;
                    break;
                }
                const keystone_uuid_t* id = (const keystone_uuid_t*)payload;
                keystone_exact_entry_t entry;
                int rc = keystoned_server_query_exact(server, &effective_sec, id, &entry);
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
                    size_t count = keystoned_server_query_temporal(server, &effective_sec, &hmin, &hmax, t_buf, max_req);
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
                    int irc = keystoned_server_ingest(server, &effective_sec, &rec);
                    resp_hdr.msg_type = KEYSTONED_MSG_INGEST_RESP;
                    resp_hdr.status = (uint32_t)irc;
                } else {
                    resp_hdr.msg_type = KEYSTONED_MSG_ERROR;
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_APPLY_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_APPLY_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len < sizeof(keystoned_workflow_apply_req_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                keystoned_workflow_apply_req_t request;
                memcpy(&request, payload, sizeof(request));
                if (request.reserved != 0 ||
                    request.count > KEYSTONE_WORKFLOW_BATCH_MAX ||
                    hdr.payload_len != sizeof(request) + request.count *
                                       sizeof(keystone_workflow_event_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                const keystone_workflow_event_t* events =
                    (const keystone_workflow_event_t*)(payload + sizeof(request));
                uint64_t watermark = 0;
                pthread_mutex_lock(&server->workflow_stage_owner_mutex);
                if (server->workflow_stage_owner_fd != -1) {
                    resp_hdr.status = KEYSTONED_STATUS_CONFLICT;
                } else {
                    resp_hdr.status = keystoned_server_workflow_apply(
                        server, &effective_sec, request.from_exclusive,
                        request.through_inclusive, events, request.count, &watermark);
                }
                pthread_mutex_unlock(&server->workflow_stage_owner_mutex);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_STAGE_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_STAGE_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len < sizeof(keystoned_workflow_apply_req_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                keystoned_workflow_apply_req_t request;
                memcpy(&request, payload, sizeof(request));
                if (request.reserved != 0 ||
                    request.count > KEYSTONE_WORKFLOW_BATCH_MAX ||
                    hdr.payload_len != sizeof(request) + request.count *
                                       sizeof(keystone_workflow_event_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                const keystone_workflow_event_t* events =
                    (const keystone_workflow_event_t*)(payload + sizeof(request));
                uint64_t watermark = 0;
                pthread_mutex_lock(&server->workflow_stage_owner_mutex);
                if (server->workflow_stage_owner_fd != -1 &&
                    server->workflow_stage_owner_fd != client_fd) {
                    resp_hdr.status = KEYSTONED_STATUS_CONFLICT;
                } else {
                    resp_hdr.status = keystoned_server_workflow_stage(
                        server, &effective_sec, request.from_exclusive,
                        request.through_inclusive, events, request.count, &watermark);
                    if (resp_hdr.status == KEYSTONED_STATUS_OK)
                        server->workflow_stage_owner_fd = client_fd;
                }
                pthread_mutex_unlock(&server->workflow_stage_owner_mutex);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_PUBLISH_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_PUBLISH_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len != sizeof(uint64_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                uint64_t target = 0;
                memcpy(&target, payload, sizeof(target));
                uint64_t watermark = 0;
                pthread_mutex_lock(&server->workflow_stage_owner_mutex);
                if (server->workflow_stage_owner_fd != client_fd) {
                    resp_hdr.status = KEYSTONED_STATUS_CONFLICT;
                } else {
                    resp_hdr.status = keystoned_server_workflow_publish(
                        server, &effective_sec, target, &watermark);
                    if (resp_hdr.status == KEYSTONED_STATUS_OK)
                        server->workflow_stage_owner_fd = -1;
                }
                pthread_mutex_unlock(&server->workflow_stage_owner_mutex);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len != 0) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                uint64_t watermark = 0;
                pthread_mutex_lock(&server->workflow_stage_owner_mutex);
                if (server->workflow_stage_owner_fd != -1 &&
                    server->workflow_stage_owner_fd != client_fd) {
                    resp_hdr.status = KEYSTONED_STATUS_CONFLICT;
                } else {
                    resp_hdr.status = keystoned_server_workflow_abort_stage(
                        server, &effective_sec, &watermark);
                    if (resp_hdr.status == KEYSTONED_STATUS_OK)
                        server->workflow_stage_owner_fd = -1;
                }
                pthread_mutex_unlock(&server->workflow_stage_owner_mutex);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len != 0) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                uint64_t watermark = 0;
                pthread_mutex_lock(&server->workflow_stage_owner_mutex);
                if (server->workflow_stage_owner_fd != -1 &&
                    server->workflow_stage_owner_fd != client_fd) {
                    resp_hdr.status = KEYSTONED_STATUS_CONFLICT;
                } else {
                    resp_hdr.status = keystoned_server_workflow_stage_watermark(
                        server, &effective_sec, &watermark);
                }
                pthread_mutex_unlock(&server->workflow_stage_owner_mutex);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len != KEYSTONE_WORKFLOW_HISTORY_ID_SIZE) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                uint64_t watermark = 0;
                resp_hdr.status = keystoned_server_workflow_bind_history(
                    server, &effective_sec, payload, &watermark);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_PAGE_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_PAGE_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len != sizeof(keystone_workflow_query_t)) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                keystone_workflow_query_t request;
                memcpy(&request, payload, sizeof(request));
                if (request.limit == 0 || request.limit > KEYSTONE_WORKFLOW_PAGE_MAX) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                keystone_workflow_entry_t entries[KEYSTONE_WORKFLOW_PAGE_MAX];
                keystone_workflow_page_t page;
                resp_hdr.status = keystoned_server_workflow_page(
                    server, &effective_sec, &request, entries,
                    KEYSTONE_WORKFLOW_PAGE_MAX, &page);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = (uint32_t)(sizeof(page) +
                        page.count * sizeof(keystone_workflow_entry_t));
                    resp_payload = malloc(resp_len);
                    if (resp_payload) {
                        memcpy(resp_payload, &page, sizeof(page));
                        memcpy(resp_payload + sizeof(page), entries,
                               page.count * sizeof(keystone_workflow_entry_t));
                    }
                }
                break;
            }

            case KEYSTONED_MSG_WORKFLOW_WATERMARK_REQ: {
                resp_hdr.msg_type = KEYSTONED_MSG_WORKFLOW_WATERMARK_RESP;
                if (!trusted_binding) {
                    resp_hdr.status = KEYSTONED_STATUS_DENIED;
                    break;
                }
                if (hdr.payload_len != 0) {
                    resp_hdr.status = KEYSTONED_STATUS_INVALID_ARG;
                    break;
                }
                uint64_t watermark = 0;
                resp_hdr.status = keystoned_server_workflow_watermark(
                    server, &watermark);
                if (resp_hdr.status == KEYSTONED_STATUS_OK) {
                    resp_len = sizeof(watermark);
                    resp_payload = malloc(resp_len);
                    if (resp_payload) memcpy(resp_payload, &watermark, resp_len);
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

    send_reply:
        free(payload);

        if (resp_len && !resp_payload) {
            resp_len = 0;
            resp_hdr.status = KEYSTONED_STATUS_SERVER_ERROR;
        }

        /* Send Response */
        resp_hdr.payload_len = resp_len;
        size_t resp_hdr_crc_len = offsetof(keystoned_msg_header_t, crc32);
        uint32_t rcrc = crc32_server(&resp_hdr, resp_hdr_crc_len);
        if (resp_payload && resp_len > 0) {
            rcrc ^= crc32_server(resp_payload, resp_len);
        }
        resp_hdr.crc32 = rcrc;

        if (send_all(client_fd, &resp_hdr, sizeof(resp_hdr)) != 0) {
            free(resp_payload);
            break;
        }
        if (resp_payload && resp_len > 0) {
            if (send_all(client_fd, resp_payload, resp_len) != 0) {
                free(resp_payload);
                break;
            }
        }
        free(resp_payload);
    }

    /* A disconnected projector must not leave a volatile stage that another
     * connection could accidentally publish or abort. Socket ownership ends
     * before the descriptor is closed or reused. */
    pthread_mutex_lock(&server->workflow_stage_owner_mutex);
    if (server->workflow_stage_owner_fd == client_fd) {
        pthread_mutex_lock(&server->workflow_mutex);
        keystone_workflow_index_abort_stage(server->workflow_index);
        pthread_mutex_unlock(&server->workflow_mutex);
        server->workflow_stage_owner_fd = -1;
    }
    pthread_mutex_unlock(&server->workflow_stage_owner_mutex);

}

/* --- Server Worker Thread --- */

static void* client_worker(void* arg) {
    keystoned_client_slot_t* slot = (keystoned_client_slot_t*)arg;
    keystoned_server_t* server = slot->server;
    handle_client(server, slot->fd);

    /* Serialize close with stop's shutdown to avoid targeting a reused fd. */
    pthread_mutex_lock(&server->clients_mutex);
    close(slot->fd);
    slot->fd = -1;
    slot->state = 2;
    pthread_mutex_unlock(&server->clients_mutex);
    return NULL;
}

static void reap_finished_clients(keystoned_server_t* server) {
    for (size_t i = 0; i < KEYSTONED_MAX_CLIENTS; ++i) {
        pthread_mutex_lock(&server->clients_mutex);
        keystoned_client_slot_t* slot = &server->clients[i];
        if (slot->state != 2) {
            pthread_mutex_unlock(&server->clients_mutex);
            continue;
        }
        slot->state = 3;
        pthread_t thread = slot->thread;
        pthread_mutex_unlock(&server->clients_mutex);

        pthread_join(thread, NULL);

        pthread_mutex_lock(&server->clients_mutex);
        slot->state = 0;
        pthread_mutex_unlock(&server->clients_mutex);
    }
}

static void dispatch_client(keystoned_server_t* server, int fd) {
    pthread_mutex_lock(&server->clients_mutex);
    keystoned_client_slot_t* slot = NULL;
    for (size_t i = 0; i < KEYSTONED_MAX_CLIENTS; ++i) {
        if (server->clients[i].state == 0) {
            slot = &server->clients[i];
            break;
        }
    }
    if (!slot || !atomic_load(&server->running)) {
        pthread_mutex_unlock(&server->clients_mutex);
        close(fd);
        return;
    }
    slot->server = server;
    slot->fd = fd;
    slot->state = 1;
    if (pthread_create(&slot->thread, NULL, client_worker, slot) != 0) {
        slot->state = 0;
        slot->fd = -1;
        pthread_mutex_unlock(&server->clients_mutex);
        close(fd);
        return;
    }
    pthread_mutex_unlock(&server->clients_mutex);
}

static void* server_worker(void* arg) {
    keystoned_server_t* server = (keystoned_server_t*)arg;

    while (server->running) {
        reap_finished_clients(server);
        struct pollfd pfd = { .fd = server->listen_fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, 200);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(server->listen_fd, NULL, NULL);
            if (client_fd >= 0) {
                dispatch_client(server, client_fd);
            }
        }
    }
    return NULL;
}

int keystoned_server_start(keystoned_server_t* server) {
    if (!server) return -1;
    if (server->running) return 0;

    /* Best-effort parent-directory creation (e.g. /run/keystoned). */
    {
        char dir_buf[256];
        snprintf(dir_buf, sizeof(dir_buf), "%s", server->socket_path);
        char* slash = strrchr(dir_buf, '/');
        if (slash && slash != dir_buf) {
            *slash = '\0';
            mkdir(dir_buf, 0755);
        }
    }

    /*
     * Sweep #15: a pre-existing socket path we cannot remove means either a
     * running daemon or a hostile placeholder (e.g. in a world-writable
     * dir). Fail loudly instead of silently missing the bind below.
     */
    if (unlink(server->socket_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "keystoned: cannot remove existing socket path %s: %s\n",
                server->socket_path, strerror(errno));
        return -1;
    }

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

    pthread_mutex_lock(&server->clients_mutex);
    for (size_t i = 0; i < KEYSTONED_MAX_CLIENTS; ++i) {
        if (server->clients[i].state == 1 && server->clients[i].fd >= 0)
            shutdown(server->clients[i].fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&server->clients_mutex);

    for (size_t i = 0; i < KEYSTONED_MAX_CLIENTS; ++i) {
        keystoned_client_slot_t* slot = &server->clients[i];
        if (slot->state != 0) {
            pthread_join(slot->thread, NULL);
            slot->state = 0;
        }
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    unlink(server->socket_path);
}
