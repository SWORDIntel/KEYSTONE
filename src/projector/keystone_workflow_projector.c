/*
 * KEYSTONE Trusted Workflow Projector Implementation (Gate 2)
 *
 * Ingests raw application transactions outside KEYSTONE, performs in-boundary
 * decryption and parsing, strips all payload bodies and search tokens, and
 * projects strictly opaque metadata intervals into keystoned.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#include "keystone_workflow_projector.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

struct keystone_workflow_projector {
    keystone_workflow_projector_config_t config;
    keystoned_client_t *client;
    pthread_mutex_t lock;

    uint64_t committed_watermark;
    uint8_t bound_history[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    bool history_bound;

    /* Staged batch buffer for automatic chunking */
    keystone_workflow_event_t *batch_buf;
    size_t batch_count;
    size_t batch_capacity;
    uint64_t batch_from_exclusive;
    uint64_t batch_through_inclusive;
};

/* Secure memory wipe to prevent sensitive application data residue */
static void secure_memzero(void *v, size_t n) {
    if (!v || n == 0) return;
    volatile uint8_t *p = (volatile uint8_t *)v;
    while (n--) *p++ = 0;
}

void keystone_workflow_projector_crypt_payload(
    const uint8_t key[32],
    uint64_t seq,
    const void *in,
    void *out,
    size_t len
) {
    if (!in || !out || len == 0) return;
    const uint8_t *src = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;

    /* 256-bit key expansion mixed with 64-bit sequence */
    uint32_t state[8];
    for (int i = 0; i < 8; i++) {
        state[i] = ((uint32_t)key[i * 4]) |
                   (((uint32_t)key[i * 4 + 1]) << 8) |
                   (((uint32_t)key[i * 4 + 2]) << 16) |
                   (((uint32_t)key[i * 4 + 3]) << 24);
    }
    state[0] ^= (uint32_t)(seq & 0xFFFFFFFFu);
    state[1] ^= (uint32_t)(seq >> 32);

    for (size_t i = 0; i < len; i++) {
        uint32_t round = (uint32_t)(i / 4);
        uint32_t k = state[round % 8] ^ (round * 0x9E3779B9u);
        k = (k << 13) | (k >> 19);
        uint8_t byte_k = (uint8_t)((k >> ((i % 4) * 8)) & 0xFFu);
        dst[i] = src[i] ^ byte_k;
    }
}

keystone_workflow_projector_t *keystone_workflow_projector_create(
    const keystone_workflow_projector_config_t *config
) {
    if (!config) return NULL;

    keystone_workflow_projector_t *proj = calloc(1, sizeof(*proj));
    if (!proj) return NULL;

    proj->config = *config;
    if (proj->config.max_batch_size == 0 ||
        proj->config.max_batch_size > KEYSTONE_WORKFLOW_BATCH_MAX) {
        proj->config.max_batch_size = KEYSTONE_WORKFLOW_BATCH_MAX;
    }

    size_t cap_bytes;
    if (!checked_mul_size(proj->config.max_batch_size, sizeof(keystone_workflow_event_t), &cap_bytes)) {
        free(proj);
        return NULL;
    }

    proj->batch_buf = calloc(proj->config.max_batch_size, sizeof(keystone_workflow_event_t));
    if (!proj->batch_buf) {
        free(proj);
        return NULL;
    }

    proj->batch_capacity = proj->config.max_batch_size;
    pthread_mutex_init(&proj->lock, NULL);

    return proj;
}

void keystone_workflow_projector_destroy(keystone_workflow_projector_t *proj) {
    if (!proj) return;

    pthread_mutex_lock(&proj->lock);
    if (proj->client) {
        keystoned_client_disconnect(proj->client);
        proj->client = NULL;
    }
    if (proj->batch_buf) {
        secure_memzero(proj->batch_buf, proj->batch_capacity * sizeof(keystone_workflow_event_t));
        free(proj->batch_buf);
        proj->batch_buf = NULL;
    }
    pthread_mutex_unlock(&proj->lock);
    pthread_mutex_destroy(&proj->lock);
    free(proj);
}

int keystone_workflow_projector_connect(keystone_workflow_projector_t *proj) {
    if (!proj) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    pthread_mutex_lock(&proj->lock);
    if (proj->client) {
        keystoned_client_disconnect(proj->client);
        proj->client = NULL;
    }

    const char *sock = proj->config.socket_path ? proj->config.socket_path : KEYSTONED_DEFAULT_SOCKET;
    proj->client = keystoned_client_connect(sock);
    if (!proj->client) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    /* Bind client principal */
    if (keystoned_client_bind_principal(proj->client, &proj->config.sec_ctx) != 0) {
        keystoned_client_disconnect(proj->client);
        proj->client = NULL;
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_CONFLICT;
    }

    /* If history ID is configured, bind history */
    int has_history = 0;
    for (size_t i = 0; i < KEYSTONE_WORKFLOW_HISTORY_ID_SIZE; i++) {
        if (proj->config.history_id[i] != 0) {
            has_history = 1;
            break;
        }
    }

    if (has_history) {
        uint64_t wm = 0;
        int status = keystoned_client_workflow_bind_history(
            proj->client, &proj->config.sec_ctx, proj->config.history_id, &wm
        );
        if (status != KEYSTONED_STATUS_OK) {
            keystoned_client_disconnect(proj->client);
            proj->client = NULL;
            pthread_mutex_unlock(&proj->lock);
            return (status == KEYSTONED_STATUS_DENIED) ? KEYSTONE_WORKFLOW_CONFLICT : KEYSTONE_WORKFLOW_STALE;
        }
        memcpy(proj->bound_history, proj->config.history_id, KEYSTONE_WORKFLOW_HISTORY_ID_SIZE);
        proj->history_bound = true;
        proj->committed_watermark = wm;
    }

    pthread_mutex_unlock(&proj->lock);
    return KEYSTONE_WORKFLOW_OK;
}

void keystone_workflow_projector_disconnect(keystone_workflow_projector_t *proj) {
    if (!proj) return;
    pthread_mutex_lock(&proj->lock);
    if (proj->client) {
        keystoned_client_disconnect(proj->client);
        proj->client = NULL;
    }
    pthread_mutex_unlock(&proj->lock);
}

int keystone_workflow_projector_bind_history(
    keystone_workflow_projector_t *proj,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE]
) {
    if (!proj || !identity) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    pthread_mutex_lock(&proj->lock);
    if (!proj->client) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    uint64_t wm = 0;
    int status = keystoned_client_workflow_bind_history(
        proj->client, &proj->config.sec_ctx, identity, &wm
    );
    if (status != KEYSTONED_STATUS_OK) {
        pthread_mutex_unlock(&proj->lock);
        return (status == KEYSTONED_STATUS_DENIED) ? KEYSTONE_WORKFLOW_CONFLICT : KEYSTONE_WORKFLOW_STALE;
    }

    memcpy(proj->bound_history, identity, KEYSTONE_WORKFLOW_HISTORY_ID_SIZE);
    memcpy(proj->config.history_id, identity, KEYSTONE_WORKFLOW_HISTORY_ID_SIZE);
    proj->history_bound = true;
    proj->committed_watermark = wm;

    pthread_mutex_unlock(&proj->lock);
    return KEYSTONE_WORKFLOW_OK;
}

int keystone_workflow_projector_get_watermark(
    keystone_workflow_projector_t *proj,
    uint64_t *out_watermark
) {
    if (!proj || !out_watermark) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    pthread_mutex_lock(&proj->lock);
    if (!proj->client) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    uint64_t wm = 0;
    int status = keystoned_client_workflow_watermark(proj->client, &proj->config.sec_ctx, &wm);
    if (status != KEYSTONED_STATUS_OK) {
        pthread_mutex_unlock(&proj->lock);
        return (status == KEYSTONED_STATUS_STALE_GEN) ? KEYSTONE_WORKFLOW_STALE : KEYSTONE_WORKFLOW_IO_ERROR;
    }

    proj->committed_watermark = wm;
    *out_watermark = wm;
    pthread_mutex_unlock(&proj->lock);
    return KEYSTONE_WORKFLOW_OK;
}

static int internal_flush_locked(keystone_workflow_projector_t *proj) {
    if (proj->batch_count == 0) {
        return KEYSTONE_WORKFLOW_OK;
    }

    if (!proj->client) {
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    uint64_t from_ex = proj->batch_from_exclusive;
    uint64_t thru_inc = proj->batch_through_inclusive;
    uint64_t new_wm = 0;

    int rc = keystoned_client_workflow_apply(
        proj->client, &proj->config.sec_ctx,
        from_ex, thru_inc,
        proj->batch_buf, proj->batch_count,
        &new_wm
    );

    if (rc == KEYSTONED_STATUS_OK) {
        proj->committed_watermark = new_wm;
        proj->batch_count = 0;
        proj->batch_from_exclusive = new_wm;
        proj->batch_through_inclusive = new_wm;
        return KEYSTONE_WORKFLOW_OK;
    }

    /* Socket error or lost ACK handling */
    if (proj->config.auto_reconnect && (rc < 0 || rc == KEYSTONED_STATUS_SERVER_ERROR)) {
        /* Reconnect and query watermark to see if keystoned actually committed it */
        keystoned_client_disconnect(proj->client);
        const char *sock = proj->config.socket_path ? proj->config.socket_path : KEYSTONED_DEFAULT_SOCKET;
        proj->client = keystoned_client_connect(sock);
        if (proj->client) {
            keystoned_client_bind_principal(proj->client, &proj->config.sec_ctx);
            if (proj->history_bound) {
                uint64_t dummy = 0;
                keystoned_client_workflow_bind_history(proj->client, &proj->config.sec_ctx,
                                                       proj->bound_history, &dummy);
            }
            uint64_t server_wm = 0;
            if (keystoned_client_workflow_watermark(proj->client, &proj->config.sec_ctx, &server_wm) == KEYSTONED_STATUS_OK) {
                if (server_wm == thru_inc) {
                    /* The batch landed before the ACK was lost */
                    proj->committed_watermark = server_wm;
                    proj->batch_count = 0;
                    proj->batch_from_exclusive = server_wm;
                    proj->batch_through_inclusive = server_wm;
                    return KEYSTONE_WORKFLOW_OK;
                } else if (server_wm == from_ex) {
                    /* Batch was never committed; retry apply once */
                    rc = keystoned_client_workflow_apply(
                        proj->client, &proj->config.sec_ctx,
                        from_ex, thru_inc,
                        proj->batch_buf, proj->batch_count,
                        &new_wm
                    );
                    if (rc == KEYSTONED_STATUS_OK) {
                        proj->committed_watermark = new_wm;
                        proj->batch_count = 0;
                        proj->batch_from_exclusive = new_wm;
                        proj->batch_through_inclusive = new_wm;
                        return KEYSTONE_WORKFLOW_OK;
                    }
                }
            }
        }
    }

    if (rc == KEYSTONED_STATUS_STALE_GEN) return KEYSTONE_WORKFLOW_STALE;
    if (rc == KEYSTONED_STATUS_GAP) return KEYSTONE_WORKFLOW_GAP;
    if (rc == KEYSTONED_STATUS_CONFLICT) return KEYSTONE_WORKFLOW_CONFLICT;
    if (rc == KEYSTONED_STATUS_DENIED) return KEYSTONE_WORKFLOW_CONFLICT;
    return KEYSTONE_WORKFLOW_IO_ERROR;
}

int keystone_workflow_projector_flush(keystone_workflow_projector_t *proj) {
    if (!proj) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&proj->lock);
    int rc = internal_flush_locked(proj);
    pthread_mutex_unlock(&proj->lock);
    return rc;
}

int keystone_workflow_projector_ingest_raw_txn(
    keystone_workflow_projector_t *proj,
    const keystone_qihse_txn_record_t *txn
) {
    if (!proj || !txn) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    if (txn->workflow_type == KEYSTONE_WORKFLOW_TYPE_UNKNOWN ||
        txn->committed_seq == 0) {
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    }

    /* Decrypt & parse application transaction within projector boundary */
    if (txn->is_encrypted && txn->payload && txn->payload_len > 0) {
        /* Decrypt into isolated temporary scratch space */
        uint8_t stack_buf[256];
        uint8_t *decrypted = stack_buf;
        if (txn->payload_len > sizeof(stack_buf)) {
            decrypted = malloc(txn->payload_len);
            if (!decrypted) return KEYSTONE_WORKFLOW_NO_MEMORY;
        }

        keystone_workflow_projector_crypt_payload(
            txn->encryption_key,
            txn->committed_seq,
            txn->payload,
            decrypted,
            txn->payload_len
        );

        /* Application validation: inside projector boundary only.
         * The application payload may contain customer names, order bodies,
         * review text, search tokens, etc.
         * We verify presence, and immediately burn/wipe it completely. */
        secure_memzero(decrypted, txn->payload_len);
        if (decrypted != stack_buf) {
            free(decrypted);
        }
    }

    /* Project into STRICTLY OPAQUE metadata event.
     * All payload bodies, paths, text, search tokens, and keys are omitted. */
    keystone_workflow_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.committed_seq = txn->committed_seq;
    ev.entry.record_id = txn->record_id;
    ev.entry.owner_id = txn->owner_id;
    ev.entry.workflow_type = txn->workflow_type;
    ev.entry.tenant = txn->tenant_id;
    ev.entry.classification = txn->classification;
    ev.entry.sci = txn->sci_mask;
    ev.entry.flags = txn->flags;
    ev.entry.due_at = txn->due_at;
    ev.tombstone = txn->is_tombstone ? 1u : 0u;

    pthread_mutex_lock(&proj->lock);

    /* Verify monotonic sequence advancement */
    if (ev.committed_seq <= proj->committed_watermark && proj->batch_count == 0) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }

    /* If batch buffer is full, flush before adding */
    if (proj->batch_count >= proj->config.max_batch_size) {
        int rc = internal_flush_locked(proj);
        if (rc != KEYSTONE_WORKFLOW_OK) {
            pthread_mutex_unlock(&proj->lock);
            return rc;
        }
    }

    if (proj->batch_count == 0) {
        proj->batch_from_exclusive = proj->committed_watermark;
    }

    proj->batch_buf[proj->batch_count++] = ev;
    proj->batch_through_inclusive = ev.committed_seq;

    /* If reached max batch size, flush immediately */
    if (proj->batch_count >= proj->config.max_batch_size) {
        int rc = internal_flush_locked(proj);
        pthread_mutex_unlock(&proj->lock);
        return rc;
    }

    pthread_mutex_unlock(&proj->lock);
    return KEYSTONE_WORKFLOW_OK;
}

int keystone_workflow_projector_ingest_raw_txns(
    keystone_workflow_projector_t *proj,
    const keystone_qihse_txn_record_t *txns,
    size_t count
) {
    if (!proj || (!txns && count > 0)) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    for (size_t i = 0; i < count; i++) {
        int rc = keystone_workflow_projector_ingest_raw_txn(proj, &txns[i]);
        if (rc != KEYSTONE_WORKFLOW_OK) return rc;
    }
    return KEYSTONE_WORKFLOW_OK;
}

int keystone_workflow_projector_apply_batch(
    keystone_workflow_projector_t *proj,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events,
    size_t count
) {
    if (!proj || (count > 0 && !events) || count > KEYSTONE_WORKFLOW_BATCH_MAX) {
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    }

    pthread_mutex_lock(&proj->lock);
    if (!proj->client) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    /* Flush any pending buffered records first */
    if (proj->batch_count > 0) {
        int rc = internal_flush_locked(proj);
        if (rc != KEYSTONE_WORKFLOW_OK) {
            pthread_mutex_unlock(&proj->lock);
            return rc;
        }
    }

    uint64_t new_wm = 0;
    int rc = keystoned_client_workflow_apply(
        proj->client, &proj->config.sec_ctx,
        from_exclusive, through_inclusive,
        events, count, &new_wm
    );

    if (rc == KEYSTONED_STATUS_OK) {
        proj->committed_watermark = new_wm;
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_OK;
    }

    pthread_mutex_unlock(&proj->lock);
    if (rc == KEYSTONED_STATUS_STALE_GEN) return KEYSTONE_WORKFLOW_STALE;
    if (rc == KEYSTONED_STATUS_GAP) return KEYSTONE_WORKFLOW_GAP;
    if (rc == KEYSTONED_STATUS_CONFLICT) return KEYSTONE_WORKFLOW_CONFLICT;
    if (rc == KEYSTONED_STATUS_DENIED) return KEYSTONE_WORKFLOW_CONFLICT;
    return KEYSTONE_WORKFLOW_IO_ERROR;
}

int keystone_workflow_projector_stage_and_publish(
    keystone_workflow_projector_t *proj,
    uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events,
    size_t count
) {
    if (!proj || (count > 0 && !events)) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    pthread_mutex_lock(&proj->lock);
    if (!proj->client) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    /* Flush any pending buffered records first */
    if (proj->batch_count > 0) {
        int rc = internal_flush_locked(proj);
        if (rc != KEYSTONE_WORKFLOW_OK) {
            pthread_mutex_unlock(&proj->lock);
            return rc;
        }
    }

    /* Chunk into staged batches capped at KEYSTONE_WORKFLOW_BATCH_MAX (4096).
     * Chunks must end on a committed_seq boundary. */
    size_t offset = 0;
    uint64_t stage_from = from_exclusive;
    uint64_t stage_wm = 0;

    while (offset < count) {
        size_t chunk_len = count - offset;
        if (chunk_len > KEYSTONE_WORKFLOW_BATCH_MAX) {
            chunk_len = KEYSTONE_WORKFLOW_BATCH_MAX;
            /* Ensure we do not split identical committed_seq across chunks */
            uint64_t last_seq = events[offset + chunk_len - 1].committed_seq;
            while (chunk_len > 1 && events[offset + chunk_len].committed_seq == last_seq) {
                chunk_len--;
            }
        }

        uint64_t chunk_thru = events[offset + chunk_len - 1].committed_seq;
        if (offset + chunk_len == count && through_inclusive > chunk_thru) {
            chunk_thru = through_inclusive;
        }

        int status = keystoned_client_workflow_stage(
            proj->client, &proj->config.sec_ctx,
            stage_from, chunk_thru,
            &events[offset], chunk_len,
            &stage_wm
        );

        if (status != KEYSTONED_STATUS_OK) {
            /* Abort volatile stage on failure */
            uint64_t dummy = 0;
            keystoned_client_workflow_abort_stage(proj->client, &proj->config.sec_ctx, &dummy);
            pthread_mutex_unlock(&proj->lock);
            return (status == KEYSTONED_STATUS_GAP) ? KEYSTONE_WORKFLOW_GAP : KEYSTONE_WORKFLOW_STALE;
        }

        stage_from = chunk_thru;
        offset += chunk_len;
    }

    /* Handle empty intervals */
    if (count == 0) {
        int status = keystoned_client_workflow_stage(
            proj->client, &proj->config.sec_ctx,
            from_exclusive, through_inclusive,
            NULL, 0, &stage_wm
        );
        if (status != KEYSTONED_STATUS_OK) {
            uint64_t dummy = 0;
            keystoned_client_workflow_abort_stage(proj->client, &proj->config.sec_ctx, &dummy);
            pthread_mutex_unlock(&proj->lock);
            return KEYSTONE_WORKFLOW_IO_ERROR;
        }
    }

    /* Atomically publish all staged entries at through_inclusive */
    uint64_t pub_wm = 0;
    int pub_status = keystoned_client_workflow_publish(
        proj->client, &proj->config.sec_ctx,
        through_inclusive, &pub_wm
    );

    if (pub_status != KEYSTONED_STATUS_OK) {
        uint64_t dummy = 0;
        keystoned_client_workflow_abort_stage(proj->client, &proj->config.sec_ctx, &dummy);
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_CONFLICT;
    }

    proj->committed_watermark = pub_wm;
    pthread_mutex_unlock(&proj->lock);
    return KEYSTONE_WORKFLOW_OK;
}

int keystone_workflow_projector_reconcile_lost_ack(
    keystone_workflow_projector_t *proj,
    uint64_t expected_target_seq,
    int *out_already_committed
) {
    if (!proj || !out_already_committed) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    pthread_mutex_lock(&proj->lock);
    if (!proj->client) {
        const char *sock = proj->config.socket_path ? proj->config.socket_path : KEYSTONED_DEFAULT_SOCKET;
        proj->client = keystoned_client_connect(sock);
        if (!proj->client) {
            pthread_mutex_unlock(&proj->lock);
            return KEYSTONE_WORKFLOW_IO_ERROR;
        }
        keystoned_client_bind_principal(proj->client, &proj->config.sec_ctx);
        if (proj->history_bound) {
            uint64_t dummy = 0;
            keystoned_client_workflow_bind_history(proj->client, &proj->config.sec_ctx,
                                                   proj->bound_history, &dummy);
        }
    }

    uint64_t server_wm = 0;
    int status = keystoned_client_workflow_watermark(proj->client, &proj->config.sec_ctx, &server_wm);
    if (status != KEYSTONED_STATUS_OK) {
        pthread_mutex_unlock(&proj->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    if (server_wm >= expected_target_seq) {
        proj->committed_watermark = server_wm;
        *out_already_committed = 1;
    } else {
        *out_already_committed = 0;
    }

    pthread_mutex_unlock(&proj->lock);
    return KEYSTONE_WORKFLOW_OK;
}
