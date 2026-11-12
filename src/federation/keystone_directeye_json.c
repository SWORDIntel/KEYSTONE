#include "../../include/keystone_directeye.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRECTEYE_JSON_MAX_BYTES (256u * 1024u)
#define DIRECTEYE_JSON_MAX_DEPTH 32u
#define DIRECTEYE_JSON_MAX_MEMBERS 128u
#define DIRECTEYE_JSON_KEY_MAX 256u

typedef struct {
    const uint8_t* data;
    size_t length;
    size_t position;
} directeye_json_parser_t;

static void json_ws(directeye_json_parser_t* parser) {
    while (parser->position < parser->length) {
        uint8_t c = parser->data[parser->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        parser->position++;
    }
}

static int json_hex4(directeye_json_parser_t* parser, uint32_t* out) {
    if (parser->length - parser->position < 4u) return -1;
    uint32_t value = 0;
    for (size_t i = 0; i < 4u; i++) {
        uint8_t c = parser->data[parser->position++];
        uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0') :
                         c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10u) :
                         c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10u) : 16u;
        if (digit > 15u) return -1;
        value = (value << 4) | digit;
    }
    *out = value;
    return 0;
}

static int json_utf8_codepoint(directeye_json_parser_t* parser, uint32_t* out) {
    if (parser->position >= parser->length) return -1;
    const uint8_t* p = parser->data + parser->position;
    size_t available = parser->length - parser->position;
    uint32_t cp;
    size_t n;
    if (p[0] < 0x80u) { cp = p[0]; n = 1u; }
    else if (p[0] >= 0xC2u && p[0] <= 0xDFu) { cp = p[0] & 0x1Fu; n = 2u; }
    else if (p[0] >= 0xE0u && p[0] <= 0xEFu) { cp = p[0] & 0x0Fu; n = 3u; }
    else if (p[0] >= 0xF0u && p[0] <= 0xF4u) { cp = p[0] & 0x07u; n = 4u; }
    else return -1;
    if (available < n) return -1;
    for (size_t i = 1u; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) return -1;
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    if ((n == 2u && cp < 0x80u) || (n == 3u && cp < 0x800u) ||
        (n == 4u && cp < 0x10000u) || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu)) return -1;
    parser->position += n;
    *out = cp;
    return 0;
}

static int json_put_utf8(char* output, size_t capacity, size_t* used, uint32_t cp) {
    uint8_t encoded[4];
    size_t n;
    if (cp == 0u) return -1;
    if (cp <= 0x7Fu) { encoded[0] = (uint8_t)cp; n = 1u; }
    else if (cp <= 0x7FFu) {
        encoded[0] = (uint8_t)(0xC0u | (cp >> 6));
        encoded[1] = (uint8_t)(0x80u | (cp & 0x3Fu)); n = 2u;
    } else if (cp <= 0xFFFFu) {
        encoded[0] = (uint8_t)(0xE0u | (cp >> 12));
        encoded[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[2] = (uint8_t)(0x80u | (cp & 0x3Fu)); n = 3u;
    } else {
        encoded[0] = (uint8_t)(0xF0u | (cp >> 18));
        encoded[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        encoded[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[3] = (uint8_t)(0x80u | (cp & 0x3Fu)); n = 4u;
    }
    if (output) {
        if (n > capacity - *used) return -1;
        memcpy(output + *used, encoded, n);
    }
    *used += n;
    return 0;
}

static int json_string(directeye_json_parser_t* parser, char* output, size_t capacity,
                       size_t* decoded_length) {
    if (parser->position >= parser->length || parser->data[parser->position++] != '"') return -1;
    size_t used = 0;
    while (parser->position < parser->length) {
        uint8_t c = parser->data[parser->position++];
        if (c == '"') {
            if (output) {
                if (used >= capacity) return -1;
                output[used] = '\0';
            }
            if (decoded_length) *decoded_length = used;
            return 0;
        }
        uint32_t cp;
        if (c == '\\') {
            if (parser->position >= parser->length) return -1;
            uint8_t esc = parser->data[parser->position++];
            switch (esc) {
                case '"': cp = '"'; break;
                case '\\': cp = '\\'; break;
                case '/': cp = '/'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u':
                    if (json_hex4(parser, &cp) != 0) return -1;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        if (parser->length - parser->position < 6u ||
                            parser->data[parser->position] != '\\' ||
                            parser->data[parser->position + 1u] != 'u') return -1;
                        parser->position += 2u;
                        uint32_t low;
                        if (json_hex4(parser, &low) != 0 || low < 0xDC00u || low > 0xDFFFu) return -1;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) return -1;
                    break;
                default: return -1;
            }
            if (json_put_utf8(output, capacity, &used, cp) != 0) return -1;
        } else {
            if (c < 0x20u) return -1;
            parser->position--;
            if (json_utf8_codepoint(parser, &cp) != 0 ||
                json_put_utf8(output, capacity, &used, cp) != 0) return -1;
        }
    }
    return -1;
}

static int json_value(directeye_json_parser_t* parser, unsigned depth);

static int json_object(directeye_json_parser_t* parser, unsigned depth) {
    if (depth > DIRECTEYE_JSON_MAX_DEPTH || parser->data[parser->position++] != '{') return -1;
    char (*keys)[DIRECTEYE_JSON_KEY_MAX + 1u] = calloc(
        DIRECTEYE_JSON_MAX_MEMBERS, sizeof(*keys));
    if (!keys) return -1;
    size_t count = 0;
    json_ws(parser);
    if (parser->position < parser->length && parser->data[parser->position] == '}') {
        parser->position++;
        free(keys);
        return 0;
    }
    int result = -1;
    while (parser->position < parser->length && count < DIRECTEYE_JSON_MAX_MEMBERS) {
        size_t key_len = 0;
        if (json_string(parser, keys[count], DIRECTEYE_JSON_KEY_MAX + 1u, &key_len) != 0) break;
        for (size_t i = 0; i < count; i++) {
            if (strlen(keys[i]) == key_len && memcmp(keys[i], keys[count], key_len) == 0) goto done;
        }
        count++;
        json_ws(parser);
        if (parser->position >= parser->length || parser->data[parser->position++] != ':') break;
        json_ws(parser);
        if (json_value(parser, depth + 1u) != 0) break;
        json_ws(parser);
        if (parser->position >= parser->length) break;
        uint8_t delim = parser->data[parser->position++];
        if (delim == '}') { result = 0; break; }
        if (delim != ',') break;
        json_ws(parser);
    }
done:
    free(keys);
    return result;
}

static int json_array(directeye_json_parser_t* parser, unsigned depth) {
    if (depth > DIRECTEYE_JSON_MAX_DEPTH || parser->data[parser->position++] != '[') return -1;
    json_ws(parser);
    if (parser->position < parser->length && parser->data[parser->position] == ']') {
        parser->position++;
        return 0;
    }
    while (parser->position < parser->length) {
        if (json_value(parser, depth + 1u) != 0) return -1;
        json_ws(parser);
        if (parser->position >= parser->length) return -1;
        uint8_t delim = parser->data[parser->position++];
        if (delim == ']') return 0;
        if (delim != ',') return -1;
        json_ws(parser);
    }
    return -1;
}

static int json_number(directeye_json_parser_t* parser) {
    size_t p = parser->position;
    if (p < parser->length && parser->data[p] == '-') p++;
    if (p >= parser->length) return -1;
    if (parser->data[p] == '0') p++;
    else {
        if (parser->data[p] < '1' || parser->data[p] > '9') return -1;
        while (p < parser->length && parser->data[p] >= '0' && parser->data[p] <= '9') p++;
    }
    if (p < parser->length && parser->data[p] == '.') {
        p++;
        if (p >= parser->length || parser->data[p] < '0' || parser->data[p] > '9') return -1;
        while (p < parser->length && parser->data[p] >= '0' && parser->data[p] <= '9') p++;
    }
    if (p < parser->length && (parser->data[p] == 'e' || parser->data[p] == 'E')) {
        p++;
        if (p < parser->length && (parser->data[p] == '+' || parser->data[p] == '-')) p++;
        if (p >= parser->length || parser->data[p] < '0' || parser->data[p] > '9') return -1;
        while (p < parser->length && parser->data[p] >= '0' && parser->data[p] <= '9') p++;
    }
    parser->position = p;
    return 0;
}

static int json_value(directeye_json_parser_t* parser, unsigned depth) {
    if (depth > DIRECTEYE_JSON_MAX_DEPTH) return -1;
    json_ws(parser);
    if (parser->position >= parser->length) return -1;
    uint8_t c = parser->data[parser->position];
    if (c == '"') return json_string(parser, NULL, 0u, NULL);
    if (c == '{') return json_object(parser, depth);
    if (c == '[') return json_array(parser, depth);
    if (c == '-' || (c >= '0' && c <= '9')) return json_number(parser);
    const char* literals[] = {"true", "false", "null"};
    const size_t lengths[] = {4u, 5u, 4u};
    for (size_t i = 0; i < 3u; i++) {
        if (parser->length - parser->position >= lengths[i] &&
            memcmp(parser->data + parser->position, literals[i], lengths[i]) == 0) {
            parser->position += lengths[i];
            return 0;
        }
    }
    return -1;
}

static int json_uint(const uint8_t* data, size_t length, uint64_t* out) {
    if (!data || !length || !out || (length > 1u && data[0] == '0')) return -1;
    uint64_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (data[i] < '0' || data[i] > '9') return -1;
        unsigned digit = (unsigned)(data[i] - '0');
        if (value > (UINT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

static int envelope_string(const uint8_t* raw, size_t length, char* output, size_t capacity) {
    directeye_json_parser_t parser = { .data = raw, .length = length, .position = 0u };
    size_t decoded = 0;
    return json_string(&parser, output, capacity, &decoded) == 0 &&
           parser.position == length && decoded < capacity ? 0 : -1;
}

static int record_id_valid(const char* id) {
    size_t length = strlen(id);
    if (length == 0u || length > KEYSTONE_DIRECTEYE_RECORD_ID_MAX) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int keystone_directeye_decode_event(const void* event_json, size_t event_json_len,
                                    keystone_directeye_decoded_event_t* out_event) {
    if (!event_json || !event_json_len || event_json_len > DIRECTEYE_JSON_MAX_BYTES || !out_event)
        return -1;
    memset(out_event, 0, sizeof(*out_event));
    directeye_json_parser_t parser = { .data = event_json, .length = event_json_len, .position = 0u };
    json_ws(&parser);
    if (parser.position >= parser.length || parser.data[parser.position++] != '{') return -1;

    enum { F_SCHEMA, F_EVENT, F_RECORD_ID, F_RECORD_KEY, F_MUTATION, F_TENANT,
           F_CLASSIFICATION, F_SCI, F_GENERATION, F_CURSOR, F_COMMITTED, F_RECORD,
           F_COUNT };
    const char* names[F_COUNT] = {"schema", "event_id", "record_id", "record_key",
        "mutation", "tenant", "classification", "sci", "generation", "cursor",
        "committed_at_ms", "record"};
    size_t spans_start[F_COUNT] = {0};
    size_t spans_length[F_COUNT] = {0};
    char (*keys)[DIRECTEYE_JSON_KEY_MAX + 1u] = calloc(DIRECTEYE_JSON_MAX_MEMBERS, sizeof(*keys));
    if (!keys) return -1;
    size_t key_count = 0u;
    int parsed = 0;
    json_ws(&parser);
    if (parser.position < parser.length && parser.data[parser.position] == '}') goto done;
    while (parser.position < parser.length && key_count < DIRECTEYE_JSON_MAX_MEMBERS) {
        size_t key_len = 0;
        if (json_string(&parser, keys[key_count], DIRECTEYE_JSON_KEY_MAX + 1u, &key_len) != 0) goto done;
        for (size_t i = 0; i < key_count; i++) {
            if (strlen(keys[i]) == key_len && memcmp(keys[i], keys[key_count], key_len) == 0) goto done;
        }
        key_count++;
        json_ws(&parser);
        if (parser.position >= parser.length || parser.data[parser.position++] != ':') goto done;
        json_ws(&parser);
        size_t start = parser.position;
        if (json_value(&parser, 1u) != 0) goto done;
        size_t end = parser.position;
        for (size_t f = 0; f < F_COUNT; f++) {
            if (strcmp(keys[key_count - 1u], names[f]) == 0) {
                spans_start[f] = start;
                spans_length[f] = end - start;
                break;
            }
        }
        json_ws(&parser);
        if (parser.position >= parser.length) goto done;
        uint8_t delimiter = parser.data[parser.position++];
        if (delimiter == '}') { parsed = 1; break; }
        if (delimiter != ',') goto done;
        json_ws(&parser);
    }
    if (!parsed) goto done;
    json_ws(&parser);
    if (parser.position != parser.length) { parsed = 0; goto done; }
    for (size_t f = 0; f < F_COUNT; f++) if (!spans_length[f]) { parsed = 0; goto done; }

    uint64_t number;
    if (json_uint(parser.data + spans_start[F_SCHEMA], spans_length[F_SCHEMA], &number) != 0 || number != 1u)
        { parsed = 0; goto done; }
    uint64_t tenant, classification, sci, generation, cursor, committed;
    if (json_uint(parser.data + spans_start[F_TENANT], spans_length[F_TENANT], &tenant) != 0 || tenant == 0u || tenant > UINT32_MAX ||
        json_uint(parser.data + spans_start[F_CLASSIFICATION], spans_length[F_CLASSIFICATION], &classification) != 0 || classification > KEYSTONE_CLASSIFICATION_TOP_SECRET ||
        json_uint(parser.data + spans_start[F_SCI], spans_length[F_SCI], &sci) != 0 || sci > UINT16_MAX ||
        json_uint(parser.data + spans_start[F_GENERATION], spans_length[F_GENERATION], &generation) != 0 || generation == 0u ||
        json_uint(parser.data + spans_start[F_CURSOR], spans_length[F_CURSOR], &cursor) != 0 || cursor == 0u ||
        json_uint(parser.data + spans_start[F_COMMITTED], spans_length[F_COMMITTED], &committed) != 0 || committed == 0u)
        { parsed = 0; goto done; }

    if (envelope_string(parser.data + spans_start[F_EVENT], spans_length[F_EVENT],
                        out_event->operation_id, sizeof(out_event->operation_id)) != 0 ||
        envelope_string(parser.data + spans_start[F_RECORD_ID], spans_length[F_RECORD_ID],
                        out_event->record_id, sizeof(out_event->record_id)) != 0 ||
        envelope_string(parser.data + spans_start[F_RECORD_KEY], spans_length[F_RECORD_KEY],
                        out_event->record_key, sizeof(out_event->record_key)) != 0 ||
        !record_id_valid(out_event->record_id)) { parsed = 0; goto done; }
    keystone_uuid_t operation;
    if (keystone_uuid_from_string(out_event->operation_id, &operation) != 0) { parsed = 0; goto done; }
    char normalized[37];
    keystone_uuid_to_string(&operation, normalized);
    if (strcmp(normalized, out_event->operation_id) != 0) { parsed = 0; goto done; }

    char expected_key[KEYSTONE_DIRECTEYE_RECORD_KEY_MAX + 1u];
    int key_len = snprintf(expected_key, sizeof(expected_key), "t:%llu/directeye/canonical/v1/%s",
                           (unsigned long long)tenant, out_event->record_id);
    if (key_len < 0 || (size_t)key_len >= sizeof(expected_key) ||
        strcmp(expected_key, out_event->record_key) != 0) { parsed = 0; goto done; }
    char mutation[8];
    if (envelope_string(parser.data + spans_start[F_MUTATION], spans_length[F_MUTATION],
                        mutation, sizeof(mutation)) != 0 ||
        (strcmp(mutation, "upsert") != 0 && strcmp(mutation, "delete") != 0))
        { parsed = 0; goto done; }
    size_t record_at = spans_start[F_RECORD];
    uint8_t first = parser.data[record_at];
    int tombstone = first == 'n' && spans_length[F_RECORD] == 4u &&
                    memcmp(parser.data + record_at, "null", 4u) == 0;
    if ((!tombstone && first != '{') || (tombstone != (strcmp(mutation, "delete") == 0)))
        { parsed = 0; goto done; }

    out_event->event.operation_id = out_event->operation_id;
    out_event->event.record_id = out_event->record_id;
    out_event->event.record_key = out_event->record_key;
    out_event->event.tenant_id = (uint32_t)tenant;
    out_event->event.classification = (uint32_t)classification;
    out_event->event.sci = (uint32_t)sci;
    out_event->event.source_generation = generation;
    out_event->event.cursor = cursor;
    out_event->event.committed_at_ms = committed;
    out_event->event.tombstone = tombstone != 0;
    if (!tombstone) {
        out_event->event.payload = parser.data + record_at;
        out_event->event.payload_len = spans_length[F_RECORD];
    }

done:
    free(keys);
    return parsed ? 0 : -1;
}
