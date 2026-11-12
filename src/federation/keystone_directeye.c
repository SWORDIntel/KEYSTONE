#include "../../include/keystone_directeye.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef KEYSTONE_ENABLE_DIRECTEYE_QIHSE
#include <qihse_controller.h>
#endif

#define DIRECTEYE_MAP_MAGIC 0x4B53444Du /* KSDM */
#define DIRECTEYE_MAP_VERSION 1u
#define DIRECTEYE_MAP_MAX_BYTES (256u * 1024u * 1024u)
#define DIRECTEYE_OBJECT_TYPE 255u

#if defined(__GNUC__)
#define DIRECTEYE_PACKED __attribute__((packed))
#else
#define DIRECTEYE_PACKED
#endif

typedef struct DIRECTEYE_PACKED {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t reserved;
    uint64_t generation;
    uint64_t cursor;
    uint64_t entry_count;
} directeye_map_header_t;

typedef struct DIRECTEYE_PACKED {
    uint32_t entry_bytes;
    uint32_t tenant_id;
    uint32_t classification;
    uint32_t sci;
    uint32_t flags;
    uint32_t record_id_len;
    uint32_t record_key_len;
    uint32_t payload_len;
    uint64_t source_generation;
    uint64_t cursor;
    uint64_t committed_at_ms;
    keystone_uuid_t operation_id;
    keystone_uuid_t object_id;
} directeye_map_entry_t;

static int directeye_record_id_valid(const char* id) {
    if (!id) return 0;
    size_t len = strlen(id);
    if (!len || len > KEYSTONE_DIRECTEYE_RECORD_ID_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int directeye_uuid_parse(const char* text, keystone_uuid_t* out) {
    if (!text || !out || keystone_uuid_from_string(text, out) != 0) return -1;
    char normalized[37];
    keystone_uuid_to_string(out, normalized);
    return strcmp(text, normalized) == 0 ? 0 : -1;
}

static uint64_t directeye_fnv(uint64_t state, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        state ^= data[i];
        state *= 1099511628211ull;
    }
    return state;
}

/* Deterministic tenant+record key. The full tuple is retained in the sidecar;
 * projection rejects a hash collision instead of aliasing two source records. */
static keystone_uuid_t directeye_object_id(uint32_t tenant, const char* id) {
    static const uint8_t domain[] = "DIRECTEYE-CANONICAL-v1";
    uint8_t tenant_bytes[4] = {
        (uint8_t)(tenant >> 24), (uint8_t)(tenant >> 16),
        (uint8_t)(tenant >> 8), (uint8_t)tenant
    };
    uint64_t a = directeye_fnv(14695981039346656037ull, domain, sizeof(domain) - 1u);
    uint64_t b = directeye_fnv(7809847782465536322ull, domain, sizeof(domain) - 1u);
    a = directeye_fnv(directeye_fnv(a, tenant_bytes, sizeof(tenant_bytes)),
                      (const uint8_t*)id, strlen(id));
    b = directeye_fnv(directeye_fnv(b, tenant_bytes, sizeof(tenant_bytes)),
                      (const uint8_t*)id, strlen(id));
    keystone_uuid_t result;
    for (size_t i = 0; i < 8u; i++) {
        result.bytes[i] = (uint8_t)(a >> (56u - (unsigned)i * 8u));
        result.bytes[i + 8u] = (uint8_t)(b >> (56u - (unsigned)i * 8u));
    }
    result.bytes[6] = (uint8_t)((result.bytes[6] & 0x0fu) | 0x50u);
    result.bytes[8] = (uint8_t)((result.bytes[8] & 0x3fu) | 0x80u);
    return result;
}

static int directeye_source_key_valid(const keystone_directeye_event_t* event) {
    char expected[KEYSTONE_DIRECTEYE_RECORD_KEY_MAX + 1u];
    int n = snprintf(expected, sizeof(expected), "t:%u/directeye/canonical/v1/%s",
                     event->tenant_id, event->record_id);
    return n > 0 && (size_t)n < sizeof(expected) &&
           strlen(event->record_key) == (size_t)n &&
           memcmp(event->record_key, expected, (size_t)n) == 0;
}

static int directeye_copy_file(const char* source, const char* destination) {
    FILE* in = fopen(source, "rb");
    if (!in) return -1;
    FILE* out = fopen(destination, "wb");
    if (!out) { fclose(in); return -1; }
    uint8_t buffer[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) != 0u) {
        if (fwrite(buffer, 1, n, out) != n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    if (fclose(in) != 0) ok = 0;
    if (ok && fflush(out) != 0) ok = 0;
    int fd = fileno(out);
    if (ok && (fd < 0 || fsync(fd) != 0)) ok = 0;
    if (fclose(out) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int directeye_map_header(FILE* file, directeye_map_header_t* header) {
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(*header), file) != sizeof(*header) ||
        header->magic != DIRECTEYE_MAP_MAGIC || header->version != DIRECTEYE_MAP_VERSION ||
        header->header_bytes != sizeof(*header) || header->reserved != 0u ||
        header->generation == 0u || fseek(file, 0, SEEK_END) != 0) return -1;
    long file_size = ftell(file);
    if (file_size < (long)sizeof(*header) || (uint64_t)file_size > DIRECTEYE_MAP_MAX_BYTES ||
        header->entry_count > ((uint64_t)file_size - sizeof(*header)) / sizeof(directeye_map_entry_t))
        return -1;
    return 0;
}

static int directeye_map_collision_check(FILE* file, const directeye_map_header_t* header,
                                         const keystone_uuid_t* object_id,
                                         const keystone_uuid_t* operation_id, uint32_t tenant,
                                         const char* record_id) {
    if (fseek(file, (long)sizeof(*header), SEEK_SET) != 0) return -1;
    for (uint64_t i = 0; i < header->entry_count; i++) {
        directeye_map_entry_t entry;
        if (fread(&entry, 1, sizeof(entry), file) != sizeof(entry) ||
            entry.entry_bytes < sizeof(entry) || entry.entry_bytes > DIRECTEYE_MAP_MAX_BYTES)
            return -1;
        size_t strings_len = (size_t)entry.record_id_len + (size_t)entry.record_key_len;
        if (strings_len > entry.entry_bytes - sizeof(entry) ||
            entry.payload_len != entry.entry_bytes - sizeof(entry) - strings_len) return -1;
        if (keystone_uuid_equal(operation_id, &entry.operation_id)) return -2;
        if (keystone_uuid_equal(object_id, &entry.object_id)) {
            char existing[KEYSTONE_DIRECTEYE_RECORD_ID_MAX + 1u];
            if (entry.record_id_len == 0u || entry.record_id_len > KEYSTONE_DIRECTEYE_RECORD_ID_MAX ||
                fread(existing, 1, entry.record_id_len, file) != entry.record_id_len) return -1;
            existing[entry.record_id_len] = '\0';
            if (entry.tenant_id != tenant || strcmp(existing, record_id) != 0) return -2;
            if (fseek(file, (long)(entry.record_key_len + entry.payload_len), SEEK_CUR) != 0) return -1;
        } else if (fseek(file, (long)(strings_len + entry.payload_len), SEEK_CUR) != 0) {
            return -1;
        }
    }
    return 0;
}

static int directeye_append_event(FILE* map, directeye_map_header_t* header,
                                  const keystone_directeye_event_t* event,
                                  uint32_t node_id, keystone_exact_index_t* exact,
                                  keystone_temporal_index_t* temporal) {
    keystone_uuid_t operation_id = keystone_uuid_nil();
    if (!event || !event->operation_id || !event->record_key ||
        directeye_uuid_parse(event->operation_id, &operation_id) != 0 ||
        !directeye_record_id_valid(event->record_id) || !directeye_source_key_valid(event) ||
        event->tenant_id == 0u || event->classification > KEYSTONE_CLASSIFICATION_TOP_SECRET ||
        event->sci > UINT16_MAX || event->source_generation == 0u || event->cursor == 0u ||
        event->committed_at_ms == 0u || event->payload_len > KEYSTONE_MAX_PAYLOAD_LEN ||
        (event->tombstone ? (event->payload != NULL || event->payload_len != 0u) :
                            (event->payload == NULL || event->payload_len == 0u))) return -1;

    size_t id_len = strlen(event->record_id);
    size_t key_len = strlen(event->record_key);
    if (key_len > KEYSTONE_DIRECTEYE_RECORD_KEY_MAX ||
        sizeof(directeye_map_entry_t) + id_len + key_len + event->payload_len > UINT32_MAX)
        return -1;
    keystone_uuid_t object_id = directeye_object_id(event->tenant_id, event->record_id);
    int collision = directeye_map_collision_check(map, header, &object_id,
                                                   &operation_id, event->tenant_id,
                                                   event->record_id);
    if (collision != 0) return collision;

    if (fseek(map, 0, SEEK_END) != 0) return -1;
    long offset_long = ftell(map);
    if (offset_long < 0 || (unsigned long)offset_long > DIRECTEYE_MAP_MAX_BYTES) return -1;
    uint64_t offset = (uint64_t)offset_long;
    directeye_map_entry_t mapped;
    memset(&mapped, 0, sizeof(mapped));
    mapped.entry_bytes = (uint32_t)(sizeof(mapped) + id_len + key_len + event->payload_len);
    if (mapped.entry_bytes > DIRECTEYE_MAP_MAX_BYTES - offset) return -1;
    mapped.tenant_id = event->tenant_id;
    mapped.classification = event->classification;
    mapped.sci = event->sci;
    mapped.flags = event->tombstone ? KEYSTONE_RECORD_FLAG_TOMBSTONE : 0u;
    mapped.record_id_len = (uint32_t)id_len;
    mapped.record_key_len = (uint32_t)key_len;
    mapped.payload_len = (uint32_t)event->payload_len;
    mapped.source_generation = event->source_generation;
    mapped.cursor = event->cursor;
    mapped.committed_at_ms = event->committed_at_ms;
    mapped.operation_id = operation_id;
    mapped.object_id = object_id;
    if (fwrite(&mapped, 1, sizeof(mapped), map) != sizeof(mapped) ||
        fwrite(event->record_id, 1, id_len, map) != id_len ||
        fwrite(event->record_key, 1, key_len, map) != key_len ||
        (event->payload_len && fwrite(event->payload, 1, event->payload_len, map) != event->payload_len))
        return -1;

    keystone_hlc_t hlc = { .physical_ms = event->committed_at_ms, .logical = 0u, .node_id = node_id };
    const keystone_temporal_entry_t* entries = keystone_temporal_index_entries(temporal);
    size_t temporal_count = keystone_temporal_index_count(temporal);
    /* Do not let a source wall-clock regression make a later cursor appear older. */
    for (size_t i = 0; i < temporal_count; i++) {
        if (entries[i].hlc.physical_ms > hlc.physical_ms) return -1;
        if (entries[i].hlc.physical_ms == hlc.physical_ms) {
            if (entries[i].hlc.logical == UINT32_MAX) return -1;
            if (hlc.logical <= entries[i].hlc.logical) hlc.logical = entries[i].hlc.logical + 1u;
        }
    }

    keystone_exact_entry_t exact_entry;
    memset(&exact_entry, 0, sizeof(exact_entry));
    exact_entry.resource_id = object_id;
    exact_entry.latest_event_id = operation_id;
    exact_entry.latest_generation = event->source_generation;
    exact_entry.latest_hlc = hlc;
    exact_entry.tenant_id = event->tenant_id;
    exact_entry.classification = event->classification;
    exact_entry.object_type = DIRECTEYE_OBJECT_TYPE;
    exact_entry.flags = mapped.flags;
    exact_entry.location_slot = offset;
    exact_entry.sci = event->sci;
    if (keystone_exact_index_upsert(exact, &exact_entry) != 0) return -1;

    keystone_temporal_entry_t temporal_entry;
    memset(&temporal_entry, 0, sizeof(temporal_entry));
    temporal_entry.hlc = hlc;
    temporal_entry.event_id = operation_id;
    temporal_entry.object_id = object_id;
    temporal_entry.event_type = DIRECTEYE_OBJECT_TYPE;
    temporal_entry.tenant_id = event->tenant_id;
    temporal_entry.classification = event->classification;
    temporal_entry.flags = mapped.flags;
    temporal_entry.source_generation = event->source_generation;
    temporal_entry.payload_offset = offset;
    temporal_entry.sci = event->sci;
    if (keystone_temporal_index_append(temporal, &temporal_entry) != 0) return -1;
    header->entry_count++;
    return 0;
}

static int directeye_stage_path(char* out, size_t capacity, const char* dir,
                                uint64_t generation, const char* suffix) {
    int n = snprintf(out, capacity, "%s/directeye-%llu.%s", dir,
                     (unsigned long long)generation, suffix);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}

int keystone_directeye_project_page(keystone_publication_t* publication,
                                    const char* staging_directory,
                                    const keystone_directeye_event_t* events,
                                    size_t event_count, uint64_t source_cursor,
                                    uint32_t stable_node_id) {
    if (!publication || !staging_directory || !*staging_directory ||
        (event_count && !events) || event_count > 128u) return -1;
    (void)mkdir(staging_directory, 0755);

    keystone_exact_index_t* exact = NULL;
    keystone_temporal_index_t* temporal = NULL;
    keystone_index_manifest_t current_manifest;
    int have_current = keystone_publication_open_latest(publication, &exact, &temporal,
                                                        &current_manifest) == 0;
    uint64_t current_cursor = have_current ? current_manifest.source_cursor : 0u;
    uint64_t current_generation = have_current ? current_manifest.index_generation : 0u;
    if (have_current && current_cursor > source_cursor) goto fail;
    if (!have_current) {
        exact = keystone_exact_index_create(32u);
        temporal = keystone_temporal_index_create(32u);
        if (!exact || !temporal) goto fail;
    }
    if (current_generation == UINT64_MAX) goto fail;
    uint64_t next_generation = current_generation + 1u;

    char map_stage[512], exact_stage[512], temporal_stage[512], map_current[512];
    if (directeye_stage_path(map_stage, sizeof(map_stage), staging_directory,
                             next_generation, "mapping") != 0 ||
        directeye_stage_path(exact_stage, sizeof(exact_stage), staging_directory,
                             next_generation, "exact") != 0 ||
        directeye_stage_path(temporal_stage, sizeof(temporal_stage), staging_directory,
                             next_generation, "temporal") != 0) goto fail;
    FILE* map = fopen(map_stage, "wb+");
    if (!map) goto fail;
    directeye_map_header_t header;
    memset(&header, 0, sizeof(header));
    if (have_current) {
        if (keystone_publication_sidecar_path(publication, current_generation,
                                              map_current, sizeof(map_current)) != 0 ||
            directeye_copy_file(map_current, map_stage) != 0) {
            fclose(map);
            unlink(map_stage);
            goto fail;
        }
        fclose(map);
        map = fopen(map_stage, "rb+");
        if (!map || directeye_map_header(map, &header) != 0 ||
            header.generation != current_generation || header.cursor != current_cursor) {
            if (map) fclose(map);
            unlink(map_stage);
            goto fail;
        }
        if (fseek(map, 0, SEEK_END) != 0) { fclose(map); unlink(map_stage); goto fail; }
    } else {
        header.magic = DIRECTEYE_MAP_MAGIC;
        header.version = DIRECTEYE_MAP_VERSION;
        header.header_bytes = sizeof(header);
        header.generation = next_generation;
        if (fwrite(&header, 1, sizeof(header), map) != sizeof(header)) {
            fclose(map); unlink(map_stage); goto fail;
        }
    }

    uint64_t previous_cursor = 0u;
    size_t new_event_count = 0u;
    uint32_t page_tenant = 0u;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].cursor <= previous_cursor || events[i].cursor > source_cursor) {
            fclose(map); unlink(map_stage); goto fail;
        }
        previous_cursor = events[i].cursor;
        if (events[i].tenant_id == 0u || (page_tenant && page_tenant != events[i].tenant_id)) {
            fclose(map); unlink(map_stage); goto fail;
        }
        page_tenant = events[i].tenant_id;
        if (events[i].cursor <= current_cursor) continue;
        if (directeye_append_event(map, &header, &events[i], stable_node_id,
                                   exact, temporal) != 0) {
            fclose(map); unlink(map_stage); goto fail;
        }
        new_event_count++;
    }
    if (source_cursor < previous_cursor || source_cursor < current_cursor) {
        fclose(map); unlink(map_stage); goto fail;
    }
    if (source_cursor == current_cursor && new_event_count == 0u) {
        fclose(map); unlink(map_stage);
        keystone_exact_index_destroy(exact);
        keystone_temporal_index_destroy(temporal);
        return have_current ? 0 : -1;
    }
    header.generation = next_generation;
    header.cursor = source_cursor;
    int map_ok = fseek(map, 0, SEEK_SET) == 0 &&
                 fwrite(&header, 1, sizeof(header), map) == sizeof(header) &&
                 fflush(map) == 0 && fsync(fileno(map)) == 0;
    if (fclose(map) != 0) map_ok = 0;
    map = NULL;
    if (!map_ok) { unlink(map_stage); goto fail; }
    if (keystone_exact_index_save(exact, exact_stage) != 0 ||
        keystone_temporal_index_save(temporal, temporal_stage) != 0) {
        unlink(map_stage); unlink(exact_stage); unlink(temporal_stage); goto fail;
    }
    keystone_hlc_t hlc_min, hlc_max;
    keystone_hlc_t* min_ptr = keystone_temporal_index_hlc_bounds(temporal, &hlc_min, &hlc_max) ? &hlc_min : NULL;
    keystone_hlc_t* max_ptr = min_ptr ? &hlc_max : NULL;
    size_t exact_count = keystone_exact_index_count(exact);
    size_t tombstone_count = keystone_exact_index_tombstone_count(exact);
    int publish_rc = keystone_publication_publish_with_sidecar(
        publication, next_generation, source_cursor, exact_stage, temporal_stage,
        map_stage, min_ptr, max_ptr, (uint64_t)exact_count,
        (uint64_t)tombstone_count);
    unlink(map_stage); unlink(exact_stage); unlink(temporal_stage);
    keystone_exact_index_destroy(exact);
    keystone_temporal_index_destroy(temporal);

    /* A post-rename directory-fsync failure is resolved from the reopened
     * active generation; this is the only condition under which -1 is ignored. */
    keystone_exact_index_t* verify_exact = NULL;
    keystone_temporal_index_t* verify_temporal = NULL;
    keystone_index_manifest_t verify_manifest;
    int verified = keystone_publication_open_latest(publication, &verify_exact,
                                                     &verify_temporal,
                                                     &verify_manifest) == 0 &&
                   verify_manifest.index_generation == next_generation &&
                   verify_manifest.source_cursor == source_cursor;
    keystone_exact_index_destroy(verify_exact);
    keystone_temporal_index_destroy(verify_temporal);
    (void)publish_rc;
    return verified ? 0 : -1;

fail:
    keystone_exact_index_destroy(exact);
    keystone_temporal_index_destroy(temporal);
    return -1;
}

#ifdef KEYSTONE_ENABLE_DIRECTEYE_QIHSE
static int directeye_reply_u64(const qihse_ctrl_reply_t* reply, uint64_t* out) {
    if (!reply || !out) return -1;
    if (reply->kind == QIHSE_CTRL_INT) {
        if (reply->integer < 0) return -1;
        *out = (uint64_t)reply->integer;
        return 0;
    }
    if (reply->kind != QIHSE_CTRL_BULK || !reply->text || !reply->text_len) return -1;
    uint64_t value = 0;
    for (size_t i = 0; i < reply->text_len; i++) {
        unsigned char c = (unsigned char)reply->text[i];
        if (c < '0' || c > '9') return -1;
        unsigned digit = (unsigned)(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

int keystone_directeye_consume_once(const keystone_directeye_qihse_config_t* config) {
    if (!config || !config->host || !*config->host || !config->port ||
        !config->username || !*config->username || !config->password ||
        !config->qkp_identity_dir || !*config->qkp_identity_dir ||
        !config->trusted_server_pubs || !config->trusted_server_pub_count ||
        !config->qkp_peer_node_id || !*config->qkp_peer_node_id ||
        !config->publication || !config->staging_directory ||
        config->page_limit == 0u || config->page_limit > 128u) return -1;
    for (size_t i = 0; i < config->trusted_server_pub_count; i++) {
        if (!config->trusted_server_pubs[i] || !*config->trusted_server_pubs[i]) return -1;
    }

    qihse_controller_config_t controller_config;
    memset(&controller_config, 0, sizeof(controller_config));
    controller_config.host = config->host;
    controller_config.port = config->port;
    controller_config.username = config->username;
    controller_config.password = config->password;
    controller_config.qkp_identity_dir = config->qkp_identity_dir;
    controller_config.qkp_trusted_pubs = config->trusted_server_pubs;
    controller_config.qkp_trusted_count = config->trusted_server_pub_count;
    controller_config.qkp_node_id = config->qkp_peer_node_id;
    qihse_controller_t* controller = qihse_controller_connect(&controller_config);
    if (!controller) return -1;
    if (!qihse_controller_connected(controller)) {
        qihse_controller_destroy(controller);
        return -1;
    }

    uint64_t after_cursor = 0u;
    keystone_exact_index_t* current_exact = NULL;
    keystone_temporal_index_t* current_temporal = NULL;
    keystone_index_manifest_t current_manifest;
    if (keystone_publication_open_latest(config->publication, &current_exact,
                                         &current_temporal, &current_manifest) == 0)
        after_cursor = current_manifest.source_cursor;
    keystone_exact_index_destroy(current_exact);
    keystone_temporal_index_destroy(current_temporal);

    char after_text[32], limit_text[16];
    snprintf(after_text, sizeof(after_text), "%llu", (unsigned long long)after_cursor);
    snprintf(limit_text, sizeof(limit_text), "%u", config->page_limit);
    const char* read_args[] = {"DIRECTEYE.FEED.READ", after_text, limit_text};
    qihse_ctrl_reply_t* page = qihse_ctrl_call(controller, 3u, read_args);
    if (!page || page->kind != QIHSE_CTRL_ARRAY || page->count < 2u) {
        qihse_ctrl_reply_free(page);
        qihse_controller_destroy(controller);
        return -1;
    }
    uint64_t next_cursor = 0u, count_u64 = 0u;
    if (directeye_reply_u64(&page->items[0], &next_cursor) != 0 ||
        directeye_reply_u64(&page->items[1], &count_u64) != 0 ||
        count_u64 > config->page_limit || page->count != 2u + (size_t)count_u64 * 2u ||
        next_cursor < after_cursor) {
        qihse_ctrl_reply_free(page);
        qihse_controller_destroy(controller);
        return -1;
    }
    size_t count = (size_t)count_u64;
    keystone_directeye_decoded_event_t* decoded = calloc(count ? count : 1u, sizeof(*decoded));
    keystone_directeye_event_t* events = calloc(count ? count : 1u, sizeof(*events));
    if (!decoded || !events) {
        free(decoded); free(events);
        qihse_ctrl_reply_free(page);
        qihse_controller_destroy(controller);
        return -1;
    }
    uint64_t prior_event_cursor = after_cursor;
    int valid = 1;
    for (size_t i = 0; i < count; i++) {
        uint64_t event_cursor = 0u;
        const qihse_ctrl_reply_t* cursor_reply = &page->items[2u + i * 2u];
        const qihse_ctrl_reply_t* json_reply = &page->items[3u + i * 2u];
        if (directeye_reply_u64(cursor_reply, &event_cursor) != 0 ||
            event_cursor <= prior_event_cursor || event_cursor > next_cursor ||
            json_reply->kind != QIHSE_CTRL_BULK || !json_reply->text ||
            keystone_directeye_decode_event(json_reply->text, json_reply->text_len,
                                             &decoded[i]) != 0 ||
            decoded[i].event.cursor != event_cursor) {
            valid = 0;
            break;
        }
        prior_event_cursor = event_cursor;
        events[i] = decoded[i].event;
    }
    int projected = valid ? keystone_directeye_project_page(
        config->publication, config->staging_directory, events, count,
        next_cursor, config->stable_node_id) : -1;
    qihse_ctrl_reply_free(page);
    free(events);
    free(decoded);
    if (projected != 0) {
        qihse_controller_destroy(controller);
        return -1;
    }

    char ack_text[32];
    snprintf(ack_text, sizeof(ack_text), "%llu", (unsigned long long)next_cursor);
    const char* ack_args[] = {"DIRECTEYE.FEED.ACK", ack_text};
    qihse_ctrl_reply_t* ack = qihse_ctrl_call(controller, 2u, ack_args);
    uint64_t ack_cursor = 0u;
    int ack_ok = directeye_reply_u64(ack, &ack_cursor) == 0 && ack_cursor == next_cursor;
    qihse_ctrl_reply_free(ack);
    qihse_controller_destroy(controller);
    return ack_ok ? 0 : -2;
}
#else
int keystone_directeye_consume_once(const keystone_directeye_qihse_config_t* config) {
    (void)config;
    return -1;
}
#endif

int keystone_directeye_record_read(keystone_publication_t* publication,
                                   const keystone_security_context_t* caller,
                                   const char* record_id, void* payload_out,
                                   size_t payload_capacity, size_t* payload_len) {
    if (payload_len) *payload_len = 0u;
    if (!publication || !caller || !payload_len || !directeye_record_id_valid(record_id) ||
        (!payload_out && payload_capacity != 0u)) return -1;
    keystone_uuid_t object_id = directeye_object_id(caller->tenant_id, record_id);
    keystone_exact_index_t* exact = NULL;
    keystone_temporal_index_t* temporal = NULL;
    keystone_index_manifest_t manifest;
    if (keystone_publication_open_latest(publication, &exact, &temporal, &manifest) != 0)
        return -1;
    keystone_temporal_index_destroy(temporal);
    keystone_exact_entry_t latest;
    int found = keystone_exact_index_lookup(exact, &object_id, &latest);
    keystone_exact_index_destroy(exact);
    if (found != 0 || (latest.flags & KEYSTONE_RECORD_FLAG_TOMBSTONE) != 0u ||
        latest.tenant_id != caller->tenant_id || latest.classification > caller->classification ||
        (latest.sci & ~caller->compartment_mask) != 0u) return 1;

    char sidecar_path[512];
    if (keystone_publication_sidecar_path(publication, manifest.index_generation,
                                          sidecar_path, sizeof(sidecar_path)) != 0) return -1;
    FILE* map = fopen(sidecar_path, "rb");
    if (!map) return -1;
    directeye_map_header_t header;
    if (directeye_map_header(map, &header) != 0 ||
        header.generation != manifest.index_generation || header.cursor != manifest.source_cursor ||
        latest.location_slot < sizeof(header) || latest.location_slot > DIRECTEYE_MAP_MAX_BYTES ||
        fseek(map, (long)latest.location_slot, SEEK_SET) != 0) { fclose(map); return -1; }
    directeye_map_entry_t entry;
    if (fread(&entry, 1, sizeof(entry), map) != sizeof(entry) ||
        entry.entry_bytes < sizeof(entry) || entry.entry_bytes > DIRECTEYE_MAP_MAX_BYTES ||
        entry.tenant_id != latest.tenant_id || entry.classification != latest.classification ||
        entry.sci != latest.sci || entry.source_generation != latest.latest_generation ||
        !keystone_uuid_equal(&entry.object_id, &object_id) ||
        !keystone_uuid_equal(&entry.operation_id, &latest.latest_event_id) ||
        entry.flags != latest.flags || entry.record_id_len == 0u ||
        entry.record_id_len > KEYSTONE_DIRECTEYE_RECORD_ID_MAX ||
        entry.record_key_len > KEYSTONE_DIRECTEYE_RECORD_KEY_MAX ||
        entry.payload_len > KEYSTONE_MAX_PAYLOAD_LEN || entry.cursor == 0u ||
        entry.cursor > header.cursor || entry.committed_at_ms == 0u ||
        entry.entry_bytes != sizeof(entry) + entry.record_id_len +
                             entry.record_key_len + entry.payload_len) { fclose(map); return -1; }
    char stored_id[KEYSTONE_DIRECTEYE_RECORD_ID_MAX + 1u];
    char stored_key[KEYSTONE_DIRECTEYE_RECORD_KEY_MAX + 1u];
    if (fread(stored_id, 1, entry.record_id_len, map) != entry.record_id_len ||
        fread(stored_key, 1, entry.record_key_len, map) != entry.record_key_len) {
        fclose(map); return -1;
    }
    stored_id[entry.record_id_len] = '\0';
    stored_key[entry.record_key_len] = '\0';
    char expected_key[KEYSTONE_DIRECTEYE_RECORD_KEY_MAX + 1u];
    int key_len = snprintf(expected_key, sizeof(expected_key),
                           "t:%u/directeye/canonical/v1/%s", entry.tenant_id, record_id);
    if (strcmp(stored_id, record_id) != 0 || key_len < 0 ||
        (size_t)key_len >= sizeof(expected_key) || strcmp(stored_key, expected_key) != 0) {
        fclose(map); return -1;
    }
    if (entry.payload_len > payload_capacity || (entry.payload_len && !payload_out)) {
        fclose(map); return -1;
    }
    if (entry.payload_len && fread(payload_out, 1, entry.payload_len, map) != entry.payload_len) {
        fclose(map); return -1;
    }
    *payload_len = entry.payload_len;
    fclose(map);
    return 0;
}
