/* Metadata-only workflow candidate index.
 * Licensed under SRSL-1.0; see the repository LICENSE. */
#include "keystone_workflow_index.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#error "Workflow checkpoints require O_NOFOLLOW"
#endif

#define SNAPSHOT_VERSION 3u
#define SNAPSHOT_HEADER_SIZE 68u
#define SNAPSHOT_ENTRY_SIZE 60u
#define SNAPSHOT_TRAILER_SIZE 4u
#define SNAPSHOT_MAX_ENTRIES (8u * 1024u * 1024u)
static const unsigned char snapshot_magic[8] = {'K','W','F','I','D','X','0','3'};

struct keystone_workflow_index {
    pthread_mutex_t lock;
    keystone_workflow_entry_t *entries; /* Tenant, type, owner, opaque ID. */
    size_t count;
    size_t capacity;
    uint64_t watermark;
    unsigned char history_id[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    int history_bound;
    int history_verified;
    int needs_prefix_proof;
    keystone_workflow_entry_t *stage_entries;
    size_t stage_count;
    size_t stage_capacity;
    uint64_t stage_watermark;
    int stage_active;
    int stage_changed; /* False means stage still shares published entries. */
};

static void put_u32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static void put_u64(unsigned char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t get_u32(const unsigned char *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (8 * i);
    return value;
}

static uint64_t get_u64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void init_crc_table(void) {
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        crc_table[i] = crc;
    }
}

static uint32_t crc_bytes(uint32_t crc, const unsigned char *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i)
        crc = crc_table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
    return crc;
}

static void encode_entry(unsigned char out[SNAPSHOT_ENTRY_SIZE],
                         const keystone_workflow_entry_t *entry) {
    memcpy(out, entry->record_id.bytes, 16);
    memcpy(out + 16, entry->owner_id.bytes, 16);
    put_u32(out + 32, entry->workflow_type);
    put_u32(out + 36, entry->tenant);
    put_u32(out + 40, entry->classification);
    put_u32(out + 44, entry->sci);
    put_u32(out + 48, entry->flags);
    put_u64(out + 52, entry->due_at);
}

static void decode_entry(keystone_workflow_entry_t *entry,
                         const unsigned char in[SNAPSHOT_ENTRY_SIZE]) {
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->record_id.bytes, in, 16);
    memcpy(entry->owner_id.bytes, in + 16, 16);
    entry->workflow_type = get_u32(in + 32);
    entry->tenant = get_u32(in + 36);
    entry->classification = get_u32(in + 40);
    entry->sci = get_u32(in + 44);
    entry->flags = get_u32(in + 48);
    entry->due_at = get_u64(in + 52);
}

static int write_all(int fd, const void *buffer, size_t size) {
    const unsigned char *p = buffer;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t size) {
    unsigned char *p = buffer;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

/* Walk directory components without following symlinks. The final component
 * is opened relative to the resulting descriptor, also with O_NOFOLLOW. */
static int snapshot_parent(const char *path, int *dir_out, char **name_out) {
    if (!path || !*path || path[strlen(path) - 1] == '/') return -1;
    char *copy = strdup(path);
    if (!copy) return -1;
    char *slash = strrchr(copy, '/');
    char *name = strdup(slash ? slash + 1 : copy);
    if (!name || !*name || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        free(name);
        free(copy);
        return -1;
    }
    int dir = open(path[0] == '/' ? "/" : ".",
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) { free(name); free(copy); return -1; }
    if (slash) {
        *slash = '\0';
        char *save = NULL;
        for (char *part = strtok_r(copy, "/", &save); part;
             part = strtok_r(NULL, "/", &save)) {
            if (strcmp(part, ".") == 0) continue;
            if (strcmp(part, "..") == 0) { close(dir); free(name); free(copy); return -1; }
            int next = openat(dir, part,
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) { close(dir); free(name); free(copy); return -1; }
            close(dir);
            dir = next;
        }
    }
    free(copy);
    *dir_out = dir;
    *name_out = name;
    return 0;
}

static int uuid_compare(const keystone_uuid_t *a, const keystone_uuid_t *b) {
    return memcmp(a->bytes, b->bytes, sizeof(a->bytes));
}

static int uuid_nil(const keystone_uuid_t *id) {
    static const keystone_uuid_t nil = {{0}};
    return uuid_compare(id, &nil) == 0;
}

static int entry_compare(const keystone_workflow_entry_t *a,
                         const keystone_workflow_entry_t *b) {
    if (a->tenant != b->tenant) return a->tenant < b->tenant ? -1 : 1;
    if (a->workflow_type != b->workflow_type)
        return a->workflow_type < b->workflow_type ? -1 : 1;
    int owner = uuid_compare(&a->owner_id, &b->owner_id);
    if (owner) return owner;
    if (a->due_at != b->due_at) return a->due_at < b->due_at ? -1 : 1;
    return uuid_compare(&a->record_id, &b->record_id);
}

static size_t lower_bound(const keystone_workflow_entry_t *entries,
                          size_t count, const keystone_workflow_entry_t *key) {
    size_t left = 0, right = count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (entry_compare(&entries[mid], key) < 0) left = mid + 1;
        else right = mid;
    }
    return left;
}

static int same_identity(const keystone_workflow_entry_t *a,
                         const keystone_workflow_entry_t *b) {
    return uuid_compare(&a->owner_id, &b->owner_id) == 0 &&
           a->workflow_type == b->workflow_type && a->tenant == b->tenant;
}

static keystone_workflow_result_t apply_event(
    keystone_workflow_entry_t *entries, size_t *count,
    const keystone_workflow_event_t *event,
    keystone_workflow_entry_t *old_entry, int *had_old) {
    const keystone_workflow_entry_t *candidate = &event->entry;
    /* Record IDs must remain globally unique, even if a later event claims a
     * different partition. Writes can be linear; reads must stay bounded. */
    size_t pos = 0;
    for (; pos < *count; ++pos)
        if (uuid_compare(&entries[pos].record_id, &candidate->record_id) == 0)
            break;
    int found = pos < *count;
    if (found && !same_identity(&entries[pos], candidate))
        return KEYSTONE_WORKFLOW_CONFLICT;
    if (had_old) *had_old = found;
    if (found && old_entry) *old_entry = entries[pos];
    if (event->tombstone) {
        if (found) {
            memmove(&entries[pos], &entries[pos + 1],
                    (*count - pos - 1) * sizeof(*entries));
            --*count;
        }
    } else if (found) {
        /* An updated deadline changes the sorted key. Reinsert it. */
        memmove(&entries[pos], &entries[pos + 1],
                (*count - pos - 1) * sizeof(*entries));
        --*count;
        pos = lower_bound(entries, *count, candidate);
        memmove(&entries[pos + 1], &entries[pos],
                (*count - pos) * sizeof(*entries));
        entries[pos] = *candidate;
        ++*count;
    } else {
        pos = lower_bound(entries, *count, candidate);
        memmove(&entries[pos + 1], &entries[pos],
                (*count - pos) * sizeof(*entries));
        entries[pos] = *candidate;
        ++*count;
    }
    return KEYSTONE_WORKFLOW_OK;
}

keystone_workflow_index_t *keystone_workflow_index_create(
    uint64_t initial_watermark) {
    /* A nonzero empty index would falsely claim that historical rows exist. */
    if (initial_watermark != 0) return NULL;
    keystone_workflow_index_t *index = calloc(1, sizeof(*index));
    if (!index) return NULL;
    if (pthread_mutex_init(&index->lock, NULL) != 0) {
        free(index);
        return NULL;
    }
    return index;
}

void keystone_workflow_index_destroy(keystone_workflow_index_t *index) {
    if (!index) return;
    pthread_mutex_destroy(&index->lock);
    free(index->entries);
    free(index->stage_entries);
    free(index);
}

keystone_workflow_result_t keystone_workflow_index_bind_history(
    keystone_workflow_index_t *index,
    const uint8_t identity[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE]) {
    if (!index || !identity) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&index->lock);
    if (index->history_bound &&
        memcmp(index->history_id, identity, sizeof(index->history_id)) != 0) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_CONFLICT;
    }
    /* Matching genesis alone does not attest the saved committed prefix.
     * Until an authenticated prefix proof is implemented, offline-loaded
     * checkpoints cannot be unsealed for live reads or writes. */
    if (index->needs_prefix_proof) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    if (!index->history_bound) {
        memcpy(index->history_id, identity, sizeof(index->history_id));
        index->history_bound = 1;
    }
    index->history_verified = 1;
    pthread_mutex_unlock(&index->lock);
    return KEYSTONE_WORKFLOW_OK;
}

int keystone_workflow_index_history_verified(keystone_workflow_index_t *index) {
    if (!index) return 0;
    pthread_mutex_lock(&index->lock);
    int verified = index->history_verified;
    pthread_mutex_unlock(&index->lock);
    return verified;
}

static keystone_workflow_result_t validate_batch(
    uint64_t from_exclusive, uint64_t through_inclusive,
    const keystone_workflow_event_t *events, size_t count) {
    if ((count && !events) || count > KEYSTONE_WORKFLOW_BATCH_MAX ||
        through_inclusive <= from_exclusive)
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].committed_seq <= from_exclusive ||
            events[i].committed_seq > through_inclusive ||
            (i && events[i].committed_seq < events[i - 1].committed_seq) ||
            uuid_nil(&events[i].entry.record_id) ||
            events[i].entry.workflow_type == 0 || events[i].tombstone > 1)
            return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
        for (size_t j = 0; j < i; ++j)
            if (events[i].committed_seq == events[j].committed_seq &&
                uuid_compare(&events[i].entry.record_id,
                             &events[j].entry.record_id) == 0)
                return KEYSTONE_WORKFLOW_CONFLICT;
    }
    return KEYSTONE_WORKFLOW_OK;
}

keystone_workflow_result_t keystone_workflow_index_ingest_batch(
    keystone_workflow_index_t *index, uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events, size_t count) {
    if (!index) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    keystone_workflow_result_t validity =
        validate_batch(from_exclusive, through_inclusive, events, count);
    if (validity != KEYSTONE_WORKFLOW_OK) return validity;

    pthread_mutex_lock(&index->lock);
    if (!index->history_verified) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    if (index->stage_active) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_CONFLICT;
    }
    if (through_inclusive <= index->watermark) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_DUPLICATE;
    }
    if (from_exclusive != index->watermark) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_GAP;
    }
    if (count == 0) {
        index->watermark = through_inclusive;
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_OK;
    }
    if (count > (SIZE_MAX / sizeof(*index->entries)) - index->count) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_NO_MEMORY;
    }
    size_t capacity = index->count + count;
    keystone_workflow_entry_t *staged = malloc(capacity * sizeof(*staged));
    if (!staged) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_NO_MEMORY;
    }
    if (index->count)
        memcpy(staged, index->entries, index->count * sizeof(*staged));
    size_t staged_count = index->count;
    keystone_workflow_result_t status = KEYSTONE_WORKFLOW_OK;
    for (size_t i = 0; i < count; ++i) {
        status = apply_event(staged, &staged_count, &events[i], NULL, NULL);
        if (status != KEYSTONE_WORKFLOW_OK) break;
    }
    if (status == KEYSTONE_WORKFLOW_OK) {
        free(index->entries);
        index->entries = staged;
        index->capacity = capacity;
        index->count = staged_count;
        index->watermark = through_inclusive;
    } else {
        free(staged);
    }
    pthread_mutex_unlock(&index->lock);
    return status;
}

keystone_workflow_result_t keystone_workflow_index_stage_batch(
    keystone_workflow_index_t *index, uint64_t from_exclusive,
    uint64_t through_inclusive,
    const keystone_workflow_event_t *events, size_t count) {
    if (!index) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    keystone_workflow_result_t validity =
        validate_batch(from_exclusive, through_inclusive, events, count);
    if (validity != KEYSTONE_WORKFLOW_OK) return validity;
    pthread_mutex_lock(&index->lock);
    if (!index->history_verified) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    uint64_t current = index->stage_active ? index->stage_watermark
                                           : index->watermark;
    if (through_inclusive <= current) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_DUPLICATE;
    }
    if (from_exclusive != current) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_GAP;
    }
    if (count) {
        size_t base_count = index->stage_changed ? index->stage_count
                                                 : index->count;
        if (count > (SIZE_MAX / sizeof(*index->entries)) - base_count ||
            count > SIZE_MAX / sizeof(keystone_workflow_entry_t)) {
            pthread_mutex_unlock(&index->lock);
            return KEYSTONE_WORKFLOW_NO_MEMORY;
        }
        /* A failed chunk must leave the prior stage intact. Keep one undo
         * record per event instead of copying the entire staged index. */
        struct workflow_undo {
            keystone_workflow_entry_t before;
            int existed;
        };
        struct workflow_undo *undo = calloc(count, sizeof(*undo));
        if (!undo) {
            pthread_mutex_unlock(&index->lock);
            return KEYSTONE_WORKFLOW_NO_MEMORY;
        }
        keystone_workflow_entry_t *next = index->stage_entries;
        size_t capacity = index->stage_capacity;
        if (!index->stage_changed) {
            capacity = base_count + count;
            next = malloc(capacity * sizeof(*next));
            if (next && base_count)
                memcpy(next, index->entries, base_count * sizeof(*next));
        } else if (capacity < base_count + count) {
            size_t needed = base_count + count;
            size_t increment = capacity / 8;
            if (increment < KEYSTONE_WORKFLOW_BATCH_MAX)
                increment = KEYSTONE_WORKFLOW_BATCH_MAX;
            if (increment > SIZE_MAX / sizeof(*next) - capacity)
                capacity = needed;
            else {
                capacity += increment;
                if (capacity < needed) capacity = needed;
            }
            next = realloc(next, capacity * sizeof(*next));
        }
        if (!next) {
            free(undo);
            pthread_mutex_unlock(&index->lock);
            return KEYSTONE_WORKFLOW_NO_MEMORY;
        }
        /* realloc may have moved a previously staged buffer. Its logical
         * state and watermark remain unchanged until the whole chunk passes. */
        if (index->stage_changed) {
            index->stage_entries = next;
            index->stage_capacity = capacity;
        }
        size_t next_count = base_count;
        keystone_workflow_result_t result = KEYSTONE_WORKFLOW_OK;
        size_t applied = 0;
        for (; applied < count; ++applied) {
            result = apply_event(next, &next_count, &events[applied],
                                 &undo[applied].before,
                                 &undo[applied].existed);
            if (result != KEYSTONE_WORKFLOW_OK) break;
        }
        if (result != KEYSTONE_WORKFLOW_OK) {
            while (applied) {
                --applied;
                keystone_workflow_event_t reverse = {
                    .entry = undo[applied].existed
                        ? undo[applied].before : events[applied].entry,
                    .tombstone = !undo[applied].existed,
                };
                /* Reversing successful events in order cannot conflict, and
                 * the buffer already has room for the pre-chunk state. */
                (void)apply_event(next, &next_count, &reverse, NULL, NULL);
            }
            if (!index->stage_changed) free(next);
            free(undo);
            pthread_mutex_unlock(&index->lock);
            return result;
        }
        free(undo);
        index->stage_entries = next;
        index->stage_count = next_count;
        index->stage_capacity = capacity;
        index->stage_changed = 1;
    } else if (!index->stage_active) {
        index->stage_count = index->count;
    }
    index->stage_watermark = through_inclusive;
    index->stage_active = 1;
    pthread_mutex_unlock(&index->lock);
    return KEYSTONE_WORKFLOW_OK;
}

keystone_workflow_result_t keystone_workflow_index_publish(
    keystone_workflow_index_t *index, uint64_t expected_target) {
    if (!index || expected_target == 0) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&index->lock);
    if (!index->history_verified) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    if (!index->stage_active || index->stage_watermark != expected_target) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_CONFLICT;
    }
    if (index->stage_changed) {
        free(index->entries);
        index->entries = index->stage_entries;
        index->capacity = index->stage_count;
        index->count = index->stage_count;
        index->stage_entries = NULL;
    }
    index->watermark = expected_target;
    index->stage_count = 0;
    index->stage_capacity = 0;
    index->stage_watermark = 0;
    index->stage_active = 0;
    index->stage_changed = 0;
    pthread_mutex_unlock(&index->lock);
    return KEYSTONE_WORKFLOW_OK;
}

keystone_workflow_result_t keystone_workflow_index_abort_stage(
    keystone_workflow_index_t *index) {
    if (!index) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&index->lock);
    free(index->stage_entries);
    index->stage_entries = NULL;
    index->stage_count = 0;
    index->stage_capacity = 0;
    index->stage_watermark = 0;
    index->stage_active = 0;
    index->stage_changed = 0;
    pthread_mutex_unlock(&index->lock);
    return KEYSTONE_WORKFLOW_OK;
}

uint64_t keystone_workflow_index_stage_watermark(
    keystone_workflow_index_t *index) {
    if (!index) return 0;
    pthread_mutex_lock(&index->lock);
    uint64_t watermark = index->history_verified && index->stage_active
        ? index->stage_watermark : 0;
    pthread_mutex_unlock(&index->lock);
    return watermark;
}

keystone_workflow_result_t keystone_workflow_index_ingest(
    keystone_workflow_index_t *index, const keystone_workflow_event_t *event) {
    if (!index || !event || event->committed_seq == 0)
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    uint64_t current = keystone_workflow_index_watermark(index);
    if (event->committed_seq <= current) return KEYSTONE_WORKFLOW_DUPLICATE;
    if (current == UINT64_MAX || event->committed_seq != current + 1)
        return KEYSTONE_WORKFLOW_GAP;
    return keystone_workflow_index_ingest_batch(index, current,
                                                event->committed_seq, event, 1);
}

keystone_workflow_result_t keystone_workflow_index_page(
    keystone_workflow_index_t *index, const keystone_workflow_query_t *query,
    keystone_workflow_entry_t *out_entries, size_t capacity,
    keystone_workflow_page_t *out_page) {
    if (!index || !query || !out_entries || !out_page ||
        query->limit == 0 || query->limit > KEYSTONE_WORKFLOW_PAGE_MAX ||
        capacity < query->limit || query->cursor_valid > 1 ||
        query->descending > 1 ||
        (query->cursor_valid && query->expected_watermark == 0) ||
        query->workflow_type == 0)
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    pthread_mutex_lock(&index->lock);
    if (!index->history_verified) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    out_page->watermark = index->watermark;
    if (index->watermark < query->required_seq) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    if (query->expected_watermark &&
        index->watermark != query->expected_watermark) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    keystone_workflow_entry_t seek = {0};
    seek.tenant = query->tenant;
    seek.workflow_type = query->workflow_type;
    seek.owner_id = query->owner_id;
    if (query->cursor_valid) {
        seek.due_at = query->cursor_due_at;
        seek.record_id = query->cursor;
    } else if (query->descending) {
        seek.due_at = query->max_due_at;
        memset(seek.record_id.bytes, 0xff, sizeof(seek.record_id.bytes));
    }
    size_t start = lower_bound(index->entries, index->count, &seek);
    if ((query->cursor_valid != query->descending) &&
        start < index->count &&
        entry_compare(&index->entries[start], &seek) == 0)
        ++start;
    size_t i = start;
    while (query->descending ? i > 0 : i < index->count) {
        const keystone_workflow_entry_t *entry =
            &index->entries[query->descending ? --i : i++];
        if (entry->tenant != query->tenant ||
            entry->workflow_type != query->workflow_type ||
            uuid_compare(&entry->owner_id, &query->owner_id) != 0)
            break;
        if (entry->due_at > query->max_due_at) {
            if (query->descending) continue;
            break;
        }
        if (entry->classification > query->max_classification ||
            ((entry->flags & KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE) &&
             query->max_classification < KEYSTONE_CLASSIFICATION_SECRET) ||
            (entry->sci & ~query->sci) != 0)
            continue;
        if (out_page->count == query->limit) {
            out_page->has_more = 1;
            break;
        }
        out_entries[out_page->count++] = *entry;
        out_page->next_cursor = entry->record_id;
        out_page->next_cursor_due_at = entry->due_at;
        out_page->next_cursor_valid = 1;
    }
    pthread_mutex_unlock(&index->lock);
    return KEYSTONE_WORKFLOW_OK;
}

uint64_t keystone_workflow_index_watermark(keystone_workflow_index_t *index) {
    if (!index) return 0;
    pthread_mutex_lock(&index->lock);
    uint64_t watermark = index->history_verified ? index->watermark : 0;
    pthread_mutex_unlock(&index->lock);
    return watermark;
}

keystone_workflow_result_t keystone_workflow_index_save(
    keystone_workflow_index_t *index, const char *path) {
    if (!index || !path || !*path || sizeof(((keystone_uuid_t *)0)->bytes) != 16)
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&index->lock);
    if (!index->history_verified || !index->history_bound) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_STALE;
    }
    size_t count = index->count;
    uint64_t watermark = index->watermark;
    unsigned char history_id[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memcpy(history_id, index->history_id, sizeof(history_id));
    if (count > SNAPSHOT_MAX_ENTRIES) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    }
    keystone_workflow_entry_t *copy = count ? malloc(count * sizeof(*copy)) : NULL;
    if (count && !copy) {
        pthread_mutex_unlock(&index->lock);
        return KEYSTONE_WORKFLOW_NO_MEMORY;
    }
    if (count) memcpy(copy, index->entries, count * sizeof(*copy));
    pthread_mutex_unlock(&index->lock);

    int dir = -1, fd = -1;
    char *name = NULL;
    if (snapshot_parent(path, &dir, &name) != 0) {
        free(copy);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }
    struct stat target;
    int stat_result = fstatat(dir, name, &target, AT_SYMLINK_NOFOLLOW);
    if ((stat_result == 0 && !S_ISREG(target.st_mode)) ||
        (stat_result < 0 && errno != ENOENT)) {
        close(dir); free(name); free(copy);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }
    char temporary[80] = {0};
    static unsigned long serial;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        unsigned long value = __sync_add_and_fetch(&serial, 1);
        int length = snprintf(temporary, sizeof(temporary),
                              ".kwfi-%ld-%lu", (long)getpid(), value);
        if (length < 0 || (size_t)length >= sizeof(temporary)) break;
        fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL |
                    O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        close(dir); free(name); free(copy);
        return KEYSTONE_WORKFLOW_IO_ERROR;
    }
    pthread_once(&crc_once, init_crc_table);
    unsigned char header[SNAPSHOT_HEADER_SIZE] = {0};
    memcpy(header, snapshot_magic, sizeof(snapshot_magic));
    put_u32(header + 8, SNAPSHOT_VERSION);
    put_u64(header + 12, watermark);
    put_u64(header + 20, count);
    put_u32(header + 28, SNAPSHOT_ENTRY_SIZE);
    memcpy(header + 36, history_id, sizeof(history_id));
    uint32_t crc = crc_bytes(UINT32_MAX, header, sizeof(header));
    int failed = write_all(fd, header, sizeof(header)) != 0;
    for (size_t i = 0; i < count && !failed; ++i) {
        unsigned char row[SNAPSHOT_ENTRY_SIZE];
        encode_entry(row, &copy[i]);
        crc = crc_bytes(crc, row, sizeof(row));
        failed = write_all(fd, row, sizeof(row)) != 0;
    }
    unsigned char trailer[SNAPSHOT_TRAILER_SIZE];
    put_u32(trailer, crc ^ UINT32_MAX);
    if (!failed) failed = write_all(fd, trailer, sizeof(trailer)) != 0;
    if (!failed) failed = fsync(fd) != 0;
    if (close(fd) != 0) failed = 1;
    if (!failed) {
        stat_result = fstatat(dir, name, &target, AT_SYMLINK_NOFOLLOW);
        if (stat_result == 0 && !S_ISREG(target.st_mode)) failed = 1;
        else if (stat_result < 0 && errno != ENOENT) failed = 1;
    }
    if (!failed) failed = renameat(dir, temporary, dir, name) != 0;
    if (!failed) failed = fsync(dir) != 0;
    if (failed) unlinkat(dir, temporary, 0);
    close(dir);
    free(name);
    free(copy);
    return failed ? KEYSTONE_WORKFLOW_IO_ERROR : KEYSTONE_WORKFLOW_OK;
}

typedef struct { unsigned char bytes[16]; } checkpoint_id_t;

static int checkpoint_id_compare(const void *a, const void *b) {
    return memcmp(a, b, sizeof(checkpoint_id_t));
}

keystone_workflow_result_t keystone_workflow_index_load(
    const char *path, keystone_workflow_index_t **out_index) {
    if (!out_index) return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    *out_index = NULL;
    if (!path || !*path || sizeof(((keystone_uuid_t *)0)->bytes) != 16)
        return KEYSTONE_WORKFLOW_BAD_ARGUMENT;
    int dir = -1;
    char *name = NULL;
    if (snapshot_parent(path, &dir, &name) != 0)
        return KEYSTONE_WORKFLOW_IO_ERROR;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(dir);
    free(name);
    if (fd < 0) return KEYSTONE_WORKFLOW_IO_ERROR;
    struct stat st;
    keystone_workflow_result_t result = KEYSTONE_WORKFLOW_CORRUPT;
    keystone_workflow_index_t *loaded = NULL;
    checkpoint_id_t *ids = NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        result = KEYSTONE_WORKFLOW_IO_ERROR;
        goto done;
    }
    if (st.st_size < (off_t)(SNAPSHOT_HEADER_SIZE + SNAPSHOT_TRAILER_SIZE) ||
        st.st_size > (off_t)(SNAPSHOT_HEADER_SIZE +
                             (uint64_t)SNAPSHOT_MAX_ENTRIES * SNAPSHOT_ENTRY_SIZE +
                             SNAPSHOT_TRAILER_SIZE))
        goto done;
    unsigned char header[SNAPSHOT_HEADER_SIZE];
    if (read_all(fd, header, sizeof(header)) != 0) goto done;
    uint64_t watermark = get_u64(header + 12);
    uint64_t count64 = get_u64(header + 20);
    if (memcmp(header, snapshot_magic, sizeof(snapshot_magic)) != 0 ||
        get_u32(header + 8) != SNAPSHOT_VERSION ||
        get_u32(header + 28) != SNAPSHOT_ENTRY_SIZE ||
        get_u32(header + 32) != 0 ||
        count64 > SNAPSHOT_MAX_ENTRIES ||
        (uint64_t)st.st_size != SNAPSHOT_HEADER_SIZE +
            count64 * SNAPSHOT_ENTRY_SIZE + SNAPSHOT_TRAILER_SIZE ||
        (count64 && watermark == 0))
        goto done;
    size_t count = (size_t)count64;
    loaded = keystone_workflow_index_create(0);
    if (!loaded) { result = KEYSTONE_WORKFLOW_NO_MEMORY; goto done; }
    memcpy(loaded->history_id, header + 36, sizeof(loaded->history_id));
    loaded->history_bound = 1;
    loaded->history_verified = 0;
    loaded->needs_prefix_proof = 1;
    if (count) {
        loaded->entries = calloc(count, sizeof(*loaded->entries));
        ids = malloc(count * sizeof(*ids));
        if (!loaded->entries || !ids) {
            result = KEYSTONE_WORKFLOW_NO_MEMORY;
            goto done;
        }
    }
    pthread_once(&crc_once, init_crc_table);
    uint32_t crc = crc_bytes(UINT32_MAX, header, sizeof(header));
    for (size_t i = 0; i < count; ++i) {
        unsigned char row[SNAPSHOT_ENTRY_SIZE];
        if (read_all(fd, row, sizeof(row)) != 0) goto done;
        crc = crc_bytes(crc, row, sizeof(row));
        decode_entry(&loaded->entries[i], row);
        if (uuid_nil(&loaded->entries[i].record_id) ||
            loaded->entries[i].workflow_type == 0 ||
            (i && entry_compare(&loaded->entries[i - 1],
                                &loaded->entries[i]) >= 0))
            goto done;
        memcpy(ids[i].bytes, loaded->entries[i].record_id.bytes, 16);
    }
    unsigned char trailer[SNAPSHOT_TRAILER_SIZE];
    if (read_all(fd, trailer, sizeof(trailer)) != 0 ||
        get_u32(trailer) != (crc ^ UINT32_MAX))
        goto done;
    if (count) qsort(ids, count, sizeof(*ids), checkpoint_id_compare);
    for (size_t i = 1; i < count; ++i)
        if (checkpoint_id_compare(&ids[i - 1], &ids[i]) == 0) goto done;
    loaded->count = count;
    loaded->capacity = count;
    loaded->watermark = watermark;
    *out_index = loaded;
    loaded = NULL;
    result = KEYSTONE_WORKFLOW_OK;
done:
    free(ids);
    keystone_workflow_index_destroy(loaded);
    close(fd);
    return result;
}
