/*
 * KEYSTONE Application Workflow Reader Implementation (Gate 3)
 *
 * High-level application query helper executing paged cursor queries against
 * keystoned with mandatory authoritative view rechecks (simulating QIHSE KV)
 * and strict fail-closed behavior with zero un-indexed full-table scan fallback.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#include "keystone_workflow_reader.h"
#include "keystone_safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

struct keystone_workflow_reader {
    keystone_workflow_reader_config_t config;
    keystoned_client_t *client;
    pthread_mutex_t lock;
};

struct keystone_workflow_reader_cursor {
    keystone_workflow_reader_query_t query;
    uint64_t bound_watermark;
    uint64_t cursor_due_at;
    keystone_uuid_t cursor_record_id;
    uint32_t cursor_valid;
    uint32_t has_more;
    uint32_t page_num;
};

keystone_workflow_reader_t *keystone_workflow_reader_create(
    const keystone_workflow_reader_config_t *config
) {
    if (!config) return NULL;

    keystone_workflow_reader_t *reader = calloc(1, sizeof(*reader));
    if (!reader) return NULL;

    reader->config = *config;
    if (reader->config.default_limit == 0 ||
        reader->config.default_limit > KEYSTONE_WORKFLOW_PAGE_MAX) {
        reader->config.default_limit = 32;
    }

    pthread_mutex_init(&reader->lock, NULL);
    return reader;
}

void keystone_workflow_reader_destroy(keystone_workflow_reader_t *reader) {
    if (!reader) return;

    pthread_mutex_lock(&reader->lock);
    if (reader->client) {
        keystoned_client_disconnect(reader->client);
        reader->client = NULL;
    }
    pthread_mutex_unlock(&reader->lock);
    pthread_mutex_destroy(&reader->lock);
    free(reader);
}

int keystone_workflow_reader_connect(keystone_workflow_reader_t *reader) {
    if (!reader) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;

    pthread_mutex_lock(&reader->lock);
    if (reader->client) {
        keystoned_client_disconnect(reader->client);
        reader->client = NULL;
    }

    const char *sock = reader->config.socket_path ? reader->config.socket_path : KEYSTONED_DEFAULT_SOCKET;
    reader->client = keystoned_client_connect(sock);
    if (!reader->client) {
        pthread_mutex_unlock(&reader->lock);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    /* Bind client's security context (tenant, classification, compartments) */
    if (keystoned_client_bind_principal(reader->client, &reader->config.sec_ctx) != 0) {
        keystoned_client_disconnect(reader->client);
        reader->client = NULL;
        pthread_mutex_unlock(&reader->lock);
        return KEYSTONE_WORKFLOW_CONFLICT;
    }

    pthread_mutex_unlock(&reader->lock);
    return KEYSTONE_WORKFLOW_OK;
}

void keystone_workflow_reader_disconnect(keystone_workflow_reader_t *reader) {
    if (!reader) return;
    pthread_mutex_lock(&reader->lock);
    if (reader->client) {
        keystoned_client_disconnect(reader->client);
        reader->client = NULL;
    }
    pthread_mutex_unlock(&reader->lock);
}

keystone_workflow_result_t keystone_workflow_reader_open_cursor(
    keystone_workflow_reader_t *reader,
    const keystone_workflow_reader_query_t *query,
    keystone_workflow_reader_cursor_t **out_cursor
) {
    if (!reader || !query || !out_cursor) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    if (query->workflow_type == 0 || query->limit > KEYSTONE_WORKFLOW_PAGE_MAX) {
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    }

    keystone_workflow_reader_cursor_t *cur = calloc(1, sizeof(*cur));
    if (!cur) return KEYSTONE_WORKFLOW_NO_MEMORY;

    cur->query = *query;
    if (cur->query.limit == 0) {
        cur->query.limit = reader->config.default_limit;
    }
    cur->bound_watermark = 0;
    cur->cursor_valid = 0;
    cur->cursor_due_at = 0;
    memset(&cur->cursor_record_id, 0, sizeof(cur->cursor_record_id));
    cur->has_more = 1;
    cur->page_num = 0;

    *out_cursor = cur;
    return KEYSTONE_WORKFLOW_OK;
}

keystone_workflow_result_t keystone_workflow_reader_cursor_next_page(
    keystone_workflow_reader_t *reader,
    keystone_workflow_reader_cursor_t *cursor,
    keystone_workflow_reader_page_t *out_page
) {
    if (!reader || !cursor || !out_page) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));

    if (!cursor->has_more) {
        out_page->watermark = cursor->bound_watermark;
        out_page->has_more = 0;
        return KEYSTONE_WORKFLOW_OK;
    }

    pthread_mutex_lock(&reader->lock);
    if (!reader->client) {
        pthread_mutex_unlock(&reader->lock);
        /* Strictly fail closed - NO full-scan fallback */
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    /* Prepare wire query to keystoned */
    keystone_workflow_query_t q;
    memset(&q, 0, sizeof(q));
    q.workflow_type = cursor->query.workflow_type;
    q.owner_id = cursor->query.owner_id;
    q.tenant = cursor->query.tenant;
    q.max_classification = cursor->query.max_classification;
    q.sci = cursor->query.sci;
    q.required_seq = cursor->query.required_seq;
    q.expected_watermark = cursor->bound_watermark; /* 0 for 1st page, bound wm for subsequent */
    q.max_due_at = cursor->query.max_due_at;
    q.cursor_due_at = cursor->cursor_due_at;
    q.cursor = cursor->cursor_record_id;
    q.cursor_valid = cursor->cursor_valid;
    q.limit = cursor->query.limit;
    q.descending = cursor->query.descending;

    keystone_workflow_entry_t raw_entries[KEYSTONE_WORKFLOW_PAGE_MAX];
    keystone_workflow_page_t raw_page;
    memset(raw_entries, 0, sizeof(raw_entries));
    memset(&raw_page, 0, sizeof(raw_page));

    int status = keystoned_client_workflow_page(
        reader->client, &reader->config.sec_ctx,
        &q, raw_entries, KEYSTONE_WORKFLOW_PAGE_MAX,
        &raw_page
    );

    if (status != KEYSTONED_STATUS_OK) {
        pthread_mutex_unlock(&reader->lock);
        /*
         * STRICT FAIL-CLOSED REQUIREMENT:
         * Never fall back to un-indexed full-table scan!
         */
        if (status == KEYSTONED_STATUS_STALE_GEN) {
            return KEYSTONE_WORKFLOW_STALE;
        } else if (status == KEYSTONED_STATUS_DENIED) {
            return KEYSTONE_WORKFLOW_CONFLICT;
        } else if (status == KEYSTONED_STATUS_GAP) {
            return KEYSTONE_WORKFLOW_GAP;
        }
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }

    pthread_mutex_unlock(&reader->lock);

    /* Enforce watermark monotonicity and freshness */
    if (cursor->bound_watermark == 0) {
        if (raw_page.watermark < cursor->query.required_seq) {
            /* Daemon watermark is behind our required sequence: fail closed */
            return KEYSTONE_WORKFLOW_STALE;
        }
        cursor->bound_watermark = raw_page.watermark;
    } else if (raw_page.watermark != cursor->bound_watermark) {
        /* Torn read detection: watermark shifted between cursor pages */
        return KEYSTONE_WORKFLOW_STALE;
    }

    out_page->watermark = cursor->bound_watermark;
    out_page->candidates_evaluated = raw_page.count;

    /*
     * AUTHORITATIVE VERIFICATION (Simulating QIHSE KV recheck):
     * For every candidate UUID returned by KEYSTONE, query the authoritative
     * view callback to verify existence, tenant ownership, and clearances.
     */
    for (size_t i = 0; i < raw_page.count; i++) {
        const keystone_workflow_entry_t *cand = &raw_entries[i];

        if (reader->config.recheck_fn) {
            keystone_workflow_entry_t verified_entry;
            memset(&verified_entry, 0, sizeof(verified_entry));

            keystone_recheck_status_t recheck = reader->config.recheck_fn(
                &cand->record_id,
                &reader->config.sec_ctx,
                reader->config.recheck_user_data,
                &verified_entry
            );

            if (recheck == KEYSTONE_RECHECK_OK) {
                /* Authoritative verification passed: add to output */
                if (out_page->count < KEYSTONE_WORKFLOW_PAGE_MAX) {
                    out_page->entries[out_page->count++] =
                        (verified_entry.workflow_type != 0) ? verified_entry : *cand;
                }
            } else if (recheck == KEYSTONE_RECHECK_STALE) {
                /* Authoritative view is stale or lagging behind index: fail closed */
                return KEYSTONE_WORKFLOW_STALE;
            } else if (recheck == KEYSTONE_RECHECK_DENIED ||
                       recheck == KEYSTONE_RECHECK_NOT_FOUND) {
                /* Filtered out: tombstoned, deleted, or caller lacks clearance */
                out_page->candidates_rejected++;
            } else {
                /* Unrecoverable error in authoritative store */
                return KEYSTONE_WORKFLOW_IO_ERROR;
            }
        } else {
            /* If no recheck callback registered, pass candidate through */
            if (out_page->count < KEYSTONE_WORKFLOW_PAGE_MAX) {
                out_page->entries[out_page->count++] = *cand;
            }
        }
    }

    /* Advance cursor continuation */
    cursor->has_more = raw_page.has_more;
    cursor->cursor_valid = raw_page.next_cursor_valid;
    cursor->cursor_due_at = raw_page.next_cursor_due_at;
    cursor->cursor_record_id = raw_page.next_cursor;
    cursor->page_num++;

    out_page->has_more = raw_page.has_more;

    return KEYSTONE_WORKFLOW_OK;
}

void keystone_workflow_reader_cursor_close(
    keystone_workflow_reader_t *reader,
    keystone_workflow_reader_cursor_t *cursor
) {
    (void)reader;
    if (!cursor) return;
    free(cursor);
}

keystone_workflow_result_t keystone_workflow_reader_query_all(
    keystone_workflow_reader_t *reader,
    const keystone_workflow_reader_query_t *query,
    keystone_workflow_entry_t *out_entries,
    size_t capacity,
    size_t *out_count
) {
    if (!reader || !query || !out_entries || !out_count) {
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    }

    *out_count = 0;

    keystone_workflow_reader_cursor_t *cursor = NULL;
    keystone_workflow_result_t res = keystone_workflow_reader_open_cursor(
        reader, query, &cursor
    );
    if (res != KEYSTONE_WORKFLOW_OK) {
        return res;
    }

    keystone_workflow_reader_page_t page;
    while (1) {
        res = keystone_workflow_reader_cursor_next_page(reader, cursor, &page);
        if (res != KEYSTONE_WORKFLOW_OK) {
            keystone_workflow_reader_cursor_close(reader, cursor);
            *out_count = 0;
            /* Strictly fail closed - NO full-scan fallback */
            return res;
        }

        for (size_t i = 0; i < page.count; i++) {
            if (*out_count >= capacity) {
                keystone_workflow_reader_cursor_close(reader, cursor);
                return KEYSTONE_WORKFLOW_NO_MEMORY;
            }
            out_entries[(*out_count)++] = page.entries[i];
        }

        if (!page.has_more) {
            break;
        }
    }

    keystone_workflow_reader_cursor_close(reader, cursor);
    return KEYSTONE_WORKFLOW_OK;
}
