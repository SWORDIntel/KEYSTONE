/*
 * KEYSTONE Security-Aware Service Mode Client Implementation
 *
 * Compact IPC client communicating over Unix domain sockets (AF_UNIX).
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#include "keystoned.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static uint32_t crc32_client(const void* data, size_t len) {
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

struct keystoned_client {
    int fd;
    uint32_t seq;
};

keystoned_client_t* keystoned_client_connect(const char* socket_path) {
    const char* path = socket_path ? socket_path : KEYSTONED_DEFAULT_SOCKET;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        close(fd);
        return NULL;
    }
    memcpy(addr.sun_path, path, path_len + 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    keystoned_client_t* cli = (keystoned_client_t*)calloc(1, sizeof(*cli));
    if (!cli) {
        close(fd);
        return NULL;
    }
    cli->fd = fd;
    cli->seq = 1;
    return cli;
}

void keystoned_client_disconnect(keystoned_client_t* client) {
    if (!client) return;
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
    free(client);
}

static int client_send_recv(
    keystoned_client_t* client,
    uint32_t msg_type,
    const keystone_security_context_t* sec_ctx,
    const void* req_payload,
    size_t req_len,
    keystoned_msg_header_t* out_resp_hdr,
    void** out_resp_payload,
    size_t* out_resp_len
) {
    if (!client || client->fd < 0 || !out_resp_hdr) return -1;

    keystoned_msg_header_t req_hdr;
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.magic = KEYSTONED_MAGIC;
    req_hdr.version = KEYSTONED_VERSION;
    req_hdr.msg_type = msg_type;
    req_hdr.seq_id = client->seq++;
    req_hdr.payload_len = (uint32_t)req_len;
    if (sec_ctx) req_hdr.sec_ctx = *sec_ctx;

    size_t hdr_crc_len = offsetof(keystoned_msg_header_t, crc32);
    uint32_t crc = crc32_client(&req_hdr, hdr_crc_len);
    if (req_payload && req_len > 0) {
        crc ^= crc32_client(req_payload, req_len);
    }
    req_hdr.crc32 = crc;

    /* Send Request */
    if (send(client->fd, &req_hdr, sizeof(req_hdr), 0) != sizeof(req_hdr)) return -1;
    if (req_payload && req_len > 0) {
        if (send(client->fd, req_payload, req_len, 0) != (ssize_t)req_len) return -1;
    }

    /* Receive Response Header */
    keystoned_msg_header_t resp_hdr;
    ssize_t n = recv(client->fd, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL);
    if (n != sizeof(resp_hdr)) return -1;

    if (resp_hdr.magic != KEYSTONED_MAGIC || resp_hdr.version != KEYSTONED_VERSION) return -1;
    if (resp_hdr.payload_len > KEYSTONED_MAX_MSG_LEN) return -1;

    void* resp_payload = NULL;
    if (resp_hdr.payload_len > 0) {
        resp_payload = malloc(resp_hdr.payload_len);
        if (!resp_payload) return -1;
        ssize_t pn = recv(client->fd, resp_payload, resp_hdr.payload_len, MSG_WAITALL);
        if (pn != (ssize_t)resp_hdr.payload_len) {
            free(resp_payload);
            return -1;
        }
    }

    /* Verify Response CRC */
    uint32_t expected_crc = crc32_client(&resp_hdr, hdr_crc_len);
    if (resp_payload && resp_hdr.payload_len > 0) {
        expected_crc ^= crc32_client(resp_payload, resp_hdr.payload_len);
    }
    if (expected_crc != resp_hdr.crc32) {
        free(resp_payload);
        return -1;
    }

    *out_resp_hdr = resp_hdr;
    if (out_resp_payload) {
        *out_resp_payload = resp_payload;
    } else {
        free(resp_payload);
    }
    if (out_resp_len) {
        *out_resp_len = resp_hdr.payload_len;
    }
    return 0;
}

int keystoned_client_ping(keystoned_client_t* client) {
    keystoned_msg_header_t resp;
    int rc = client_send_recv(client, KEYSTONED_MSG_PING, NULL, NULL, 0, &resp, NULL, NULL);
    if (rc != 0) return -1;
    return (resp.msg_type == KEYSTONED_MSG_PONG && resp.status == KEYSTONED_STATUS_OK) ? 0 : -1;
}

int keystoned_client_bind_principal(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx
) {
    if (!client || !sec_ctx) return -1;
    keystoned_msg_header_t resp;
    int rc = client_send_recv(client, KEYSTONED_MSG_BIND_PRINCIPAL_REQ, sec_ctx, NULL, 0, &resp, NULL, NULL);
    if (rc != 0) return -1;
    return (resp.status == KEYSTONED_STATUS_OK) ? 0 : -1;
}

int keystoned_client_exact_lookup(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_uuid_t* id,
    keystone_exact_entry_t* out_entry
) {
    if (!client || !sec_ctx || !id || !out_entry) return -1;

    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;

    int rc = client_send_recv(client, KEYSTONED_MSG_EXACT_LOOKUP_REQ, sec_ctx,
                              id, sizeof(*id), &resp, &payload, &payload_len);
    if (rc != 0) return -1;

    if (resp.status == KEYSTONED_STATUS_OK && payload && payload_len >= sizeof(keystone_exact_entry_t)) {
        memcpy(out_entry, payload, sizeof(keystone_exact_entry_t));
        free(payload);
        return 0;
    }

    free(payload);
    return (int)resp.status;
}

int keystoned_client_temporal_range(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_hlc_t* hmin,
    const keystone_hlc_t* hmax,
    keystone_temporal_entry_t* out_entries,
    size_t max_results,
    size_t* out_count
) {
    if (!client || !sec_ctx || !hmin || !hmax || !out_entries || !out_count) return -1;

    uint8_t req_buf[2 * sizeof(keystone_hlc_t) + sizeof(uint32_t)];
    uint8_t* p = req_buf;
    memcpy(p, hmin, sizeof(*hmin)); p += sizeof(*hmin);
    memcpy(p, hmax, sizeof(*hmax)); p += sizeof(*hmax);
    uint32_t max_u32 = (uint32_t)max_results;
    memcpy(p, &max_u32, sizeof(max_u32));

    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;

    int rc = client_send_recv(client, KEYSTONED_MSG_TEMPORAL_RANGE_REQ, sec_ctx,
                              req_buf, sizeof(req_buf), &resp, &payload, &payload_len);
    if (rc != 0) return -1;

    if (resp.status == KEYSTONED_STATUS_OK && payload && payload_len >= sizeof(uint32_t)) {
        uint32_t count = 0;
        memcpy(&count, payload, sizeof(uint32_t));
        size_t available = (payload_len - sizeof(uint32_t)) / sizeof(keystone_temporal_entry_t);
        size_t to_copy = count < available ? count : available;
        if (to_copy > max_results) to_copy = max_results;

        if (to_copy > 0) {
            memcpy(out_entries, (uint8_t*)payload + sizeof(uint32_t), to_copy * sizeof(keystone_temporal_entry_t));
        }
        *out_count = to_copy;
        free(payload);
        return 0;
    }

    free(payload);
    return (int)resp.status;
}

int keystoned_client_ingest(
    keystoned_client_t* client,
    const keystone_security_context_t* sec_ctx,
    const keystone_federation_record_t* rec
) {
    if (!client || !sec_ctx || !rec) return -1;

    uint8_t ser_buf[2048];
    size_t written = 0;
    if (keystone_record_serialize(rec, ser_buf, sizeof(ser_buf), &written) != 0) return -1;

    keystoned_msg_header_t resp;
    int rc = client_send_recv(client, KEYSTONED_MSG_INGEST_REQ, sec_ctx,
                              ser_buf, written, &resp, NULL, NULL);
    if (rc != 0) return -1;
    return (int)resp.status;
}

int keystoned_client_workflow_bind_history(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE],
    uint64_t* out_watermark
) {
    if (!client || !sec_ctx || !identity || !out_watermark) return -1;
    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;
    int rc = client_send_recv(client, KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_REQ,
                              sec_ctx, identity, KEYSTONE_WORKFLOW_HISTORY_ID_SIZE,
                              &resp, &payload, &payload_len);
    if (rc != 0) return -1;
    if (resp.msg_type != KEYSTONED_MSG_WORKFLOW_BIND_HISTORY_RESP ||
        (resp.status == KEYSTONED_STATUS_OK &&
         payload_len != sizeof(uint64_t))) {
        free(payload);
        return -1;
    }
    if (resp.status == KEYSTONED_STATUS_OK)
        memcpy(out_watermark, payload, sizeof(uint64_t));
    free(payload);
    return (int)resp.status;
}

int keystoned_client_workflow_apply(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive, uint64_t through_inclusive,
    const keystone_workflow_event_t* events, size_t count,
    uint64_t* out_watermark
) {
    if (!client || !sec_ctx || !out_watermark || (count && !events) ||
        count > KEYSTONE_WORKFLOW_BATCH_MAX ||
        count > (KEYSTONED_MAX_MSG_LEN - sizeof(keystoned_workflow_apply_req_t)) /
                sizeof(keystone_workflow_event_t)) return -1;
    size_t request_len = sizeof(keystoned_workflow_apply_req_t) +
                         count * sizeof(keystone_workflow_event_t);
    uint8_t* request = calloc(1, request_len);
    if (!request) return -1;
    keystoned_workflow_apply_req_t header = {
        .from_exclusive = from_exclusive,
        .through_inclusive = through_inclusive,
        .count = (uint32_t)count,
        .reserved = 0
    };
    memcpy(request, &header, sizeof(header));
    if (count) memcpy(request + sizeof(header), events,
                      count * sizeof(keystone_workflow_event_t));

    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;
    int rc = client_send_recv(client, KEYSTONED_MSG_WORKFLOW_APPLY_REQ,
                              sec_ctx, request, request_len,
                              &resp, &payload, &payload_len);
    free(request);
    if (rc != 0) return -1;
    if (resp.msg_type != KEYSTONED_MSG_WORKFLOW_APPLY_RESP ||
        (resp.status == KEYSTONED_STATUS_OK &&
         payload_len != sizeof(uint64_t))) {
        free(payload);
        return -1;
    }
    if (resp.status == KEYSTONED_STATUS_OK)
        memcpy(out_watermark, payload, sizeof(uint64_t));
    free(payload);
    return (int)resp.status;
}

int keystoned_client_workflow_stage(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint64_t from_exclusive, uint64_t through_inclusive,
    const keystone_workflow_event_t* events, size_t count,
    uint64_t* out_stage_watermark
) {
    if (!client || !sec_ctx || !out_stage_watermark || (count && !events) ||
        count > KEYSTONE_WORKFLOW_BATCH_MAX ||
        count > (KEYSTONED_MAX_MSG_LEN - sizeof(keystoned_workflow_apply_req_t)) /
                sizeof(keystone_workflow_event_t)) return -1;
    size_t request_len = sizeof(keystoned_workflow_apply_req_t) +
                         count * sizeof(keystone_workflow_event_t);
    uint8_t* request = calloc(1, request_len);
    if (!request) return -1;
    keystoned_workflow_apply_req_t header = {
        .from_exclusive = from_exclusive,
        .through_inclusive = through_inclusive,
        .count = (uint32_t)count,
        .reserved = 0
    };
    memcpy(request, &header, sizeof(header));
    if (count) memcpy(request + sizeof(header), events,
                      count * sizeof(keystone_workflow_event_t));
    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;
    int rc = client_send_recv(client, KEYSTONED_MSG_WORKFLOW_STAGE_REQ,
                              sec_ctx, request, request_len,
                              &resp, &payload, &payload_len);
    free(request);
    if (rc != 0) return -1;
    if (resp.msg_type != KEYSTONED_MSG_WORKFLOW_STAGE_RESP ||
        (resp.status == KEYSTONED_STATUS_OK && payload_len != sizeof(uint64_t))) {
        free(payload);
        return -1;
    }
    if (resp.status == KEYSTONED_STATUS_OK)
        memcpy(out_stage_watermark, payload, sizeof(uint64_t));
    free(payload);
    return (int)resp.status;
}

static int client_workflow_watermark_operation(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint32_t request_type, uint32_t response_type,
    const void* request, size_t request_len, uint64_t* out_watermark
) {
    if (!client || !sec_ctx || !out_watermark) return -1;
    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;
    int rc = client_send_recv(client, request_type, sec_ctx,
                              request, request_len, &resp, &payload, &payload_len);
    if (rc != 0) return -1;
    if (resp.msg_type != response_type ||
        (resp.status == KEYSTONED_STATUS_OK && payload_len != sizeof(uint64_t))) {
        free(payload);
        return -1;
    }
    if (resp.status == KEYSTONED_STATUS_OK)
        memcpy(out_watermark, payload, sizeof(uint64_t));
    free(payload);
    return (int)resp.status;
}

int keystoned_client_workflow_publish(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint64_t expected_target, uint64_t* out_watermark
) {
    return client_workflow_watermark_operation(
        client, sec_ctx, KEYSTONED_MSG_WORKFLOW_PUBLISH_REQ,
        KEYSTONED_MSG_WORKFLOW_PUBLISH_RESP,
        &expected_target, sizeof(expected_target), out_watermark);
}

int keystoned_client_workflow_abort_stage(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
) {
    return client_workflow_watermark_operation(
        client, sec_ctx, KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_REQ,
        KEYSTONED_MSG_WORKFLOW_ABORT_STAGE_RESP,
        NULL, 0, out_watermark);
}

int keystoned_client_workflow_stage_watermark(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
) {
    return client_workflow_watermark_operation(
        client, sec_ctx, KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_REQ,
        KEYSTONED_MSG_WORKFLOW_STAGE_WATERMARK_RESP,
        NULL, 0, out_watermark);
}

int keystoned_client_workflow_page(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    const keystone_workflow_query_t* query,
    keystone_workflow_entry_t* out_entries, size_t capacity,
    keystone_workflow_page_t* out_page
) {
    if (!client || !sec_ctx || !query || !out_entries || !out_page ||
        query->limit == 0 || query->limit > KEYSTONE_WORKFLOW_PAGE_MAX ||
        query->cursor_valid > 1 || query->descending > 1 ||
        query->workflow_type == 0 ||
        capacity < query->limit) return -1;
    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;
    int rc = client_send_recv(client, KEYSTONED_MSG_WORKFLOW_PAGE_REQ,
                              sec_ctx, query, sizeof(*query),
                              &resp, &payload, &payload_len);
    if (rc != 0) return -1;
    if (resp.msg_type != KEYSTONED_MSG_WORKFLOW_PAGE_RESP) {
        free(payload);
        return -1;
    }
    if (resp.status != KEYSTONED_STATUS_OK) {
        free(payload);
        return (int)resp.status;
    }
    if (payload_len < sizeof(keystone_workflow_page_t)) {
        free(payload);
        return -1;
    }
    keystone_workflow_page_t page;
    memcpy(&page, payload, sizeof(page));
    if (page.count > query->limit || page.count > capacity ||
        payload_len != sizeof(page) +
                       page.count * sizeof(keystone_workflow_entry_t)) {
        free(payload);
        return -1;
    }
    if (page.count) memcpy(out_entries, (uint8_t*)payload + sizeof(page),
                           page.count * sizeof(keystone_workflow_entry_t));
    *out_page = page;
    free(payload);
    return KEYSTONED_STATUS_OK;
}

int keystoned_client_workflow_watermark(
    keystoned_client_t* client, const keystone_security_context_t* sec_ctx,
    uint64_t* out_watermark
) {
    if (!client || !sec_ctx || !out_watermark) return -1;
    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;
    int rc = client_send_recv(client, KEYSTONED_MSG_WORKFLOW_WATERMARK_REQ,
                              sec_ctx, NULL, 0, &resp, &payload, &payload_len);
    if (rc != 0) return -1;
    if (resp.msg_type != KEYSTONED_MSG_WORKFLOW_WATERMARK_RESP ||
        (resp.status == KEYSTONED_STATUS_OK &&
         payload_len != sizeof(uint64_t))) {
        free(payload);
        return -1;
    }
    if (resp.status == KEYSTONED_STATUS_OK)
        memcpy(out_watermark, payload, sizeof(uint64_t));
    free(payload);
    return (int)resp.status;
}

int keystoned_client_get_status(
    keystoned_client_t* client,
    uint64_t* out_generation,
    uint64_t* out_epoch,
    keystone_hlc_t* out_hlc
) {
    if (!client) return -1;

    keystoned_msg_header_t resp;
    void* payload = NULL;
    size_t payload_len = 0;

    int rc = client_send_recv(client, KEYSTONED_MSG_STATUS_REQ, NULL, NULL, 0,
                              &resp, &payload, &payload_len);
    if (rc != 0) return -1;

    if (out_generation) *out_generation = resp.index_generation;

    if (payload && payload_len >= sizeof(keystone_ingest_stats_t)) {
        const keystone_ingest_stats_t* stats = (const keystone_ingest_stats_t*)payload;
        if (out_epoch) *out_epoch = stats->current_fencing_epoch;
        if (out_hlc) *out_hlc = stats->high_watermark_hlc;
    }

    free(payload);
    return 0;
}
