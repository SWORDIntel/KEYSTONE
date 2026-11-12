/* Native DIRECTEYE feed projection into KEYSTONE exact/temporal generations. */
#ifndef KEYSTONE_DIRECTEYE_H
#define KEYSTONE_DIRECTEYE_H

#include "keystone_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEYSTONE_DIRECTEYE_RECORD_ID_MAX 128u
#define KEYSTONE_DIRECTEYE_RECORD_KEY_MAX 256u
#define KEYSTONE_DIRECTEYE_OPERATION_ID_SIZE 37u

typedef struct {
    const char* operation_id;       /* canonical lowercase UUID */
    const char* record_id;          /* ASCII [A-Za-z0-9._-] */
    const char* record_key;         /* QIHSE tenant-scoped canonical key */
    uint32_t tenant_id;
    uint32_t classification;
    uint32_t sci;
    uint64_t source_generation;
    uint64_t cursor;
    uint64_t committed_at_ms;       /* source commit time; required */
    bool tombstone;
    const void* payload;            /* exact record JSON bytes; NULL for tombstone */
    size_t payload_len;
} keystone_directeye_event_t;

/* Decoded envelope owns string fields; `event.payload` remains a span into the
 * caller's raw event_json buffer, which must remain valid through projection. */
typedef struct {
    keystone_directeye_event_t event;
    char operation_id[KEYSTONE_DIRECTEYE_OPERATION_ID_SIZE];
    char record_id[KEYSTONE_DIRECTEYE_RECORD_ID_MAX + 1u];
    char record_key[KEYSTONE_DIRECTEYE_RECORD_KEY_MAX + 1u];
} keystone_directeye_decoded_event_t;

/* Strict bounded QIHSE event-envelope JSON decoder; rejects duplicate keys,
 * missing source timestamps, inconsistent tombstones, and oversized input. */
int keystone_directeye_decode_event(const void* event_json, size_t event_json_len,
                                    keystone_directeye_decoded_event_t* out_event);

/*
 * Project one authenticated QIHSE feed page. Events must already have passed
 * strict envelope validation. `source_cursor` is QIHSE's page high-water mark.
 * A zero return means the exact/temporal indexes and mapping sidecar reopened
 * as a complete published generation with that watermark. This does not ACK
 * QIHSE; the authenticated consumer must ACK only after this returns zero.
 */
int keystone_directeye_project_page(
    keystone_publication_t* publication,
    const char* staging_directory,
    const keystone_directeye_event_t* events,
    size_t event_count,
    uint64_t source_cursor,
    uint32_t stable_node_id
);

typedef struct {
    const char* host;
    uint16_t port;
    const char* username;
    const char* password;
    const char* qkp_identity_dir;
    const char* const* trusted_server_pubs;
    size_t trusted_server_pub_count;
    const char* qkp_peer_node_id;
    keystone_publication_t* publication;
    const char* staging_directory;
    uint32_t page_limit;            /* 1..128 */
    uint32_t stable_node_id;
} keystone_directeye_qihse_config_t;

/* Fetch one page through QIHSE's authenticated sealed Controller client,
 * project and reopen it, then ACK exactly the published page cursor. Returns
 * 0 after durable publication+ACK, -2 if publication succeeded but ACK failed. */
int keystone_directeye_consume_once(const keystone_directeye_qihse_config_t* config);

/* Query-time record map and authorization join. Returns 0 and copies the exact
 * JSON payload on authorized live records, 1 for absent/deleted/denied, -1 on
 * corruption or I/O failure. Caller identity/security must be QIHSE-authenticated.
 */
int keystone_directeye_record_read(
    keystone_publication_t* publication,
    const keystone_security_context_t* caller,
    const char* record_id,
    void* payload_out,
    size_t payload_capacity,
    size_t* payload_len
);

#ifdef __cplusplus
}
#endif
#endif
