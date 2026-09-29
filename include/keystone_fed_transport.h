/*
 * KEYSTONE Federated Query Transport (Phase 6 completion)
 *
 * Wire protocol, bounded TCP responder, and fan-out coordinator for
 * distributed queries across KEYSTONE nodes (CITADEL brief §38/§39, audit
 * criterion 14). The merge core (keystone_federated_query.h) stays the
 * authority for ordering/dedup/conflict resolution; this header adds the
 * transport that feeds it from real nodes, with per-node timeouts and
 * partial-result semantics, and enforces the caller's security context at
 * BOTH ends: the responder filters before anything leaves the node, and the
 * coordinator re-filters before anything reaches the caller.
 *
 * Wire format (v1, host byte order like every KEYSTONE wire format — the
 * fleet is same-endian): CRC32-checked packed frames, magic + version +
 * strict reserved word + explicit lengths, one request per connection.
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
 */

#ifndef KEYSTONE_FED_TRANSPORT_H
#define KEYSTONE_FED_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keystone_federation.h"
#include "keystone_exact_index.h"
#include "keystone_temporal.h"

#define KEYSTONE_FED_QMAGIC 0x4B535131u /* 'KSQ1' */
#define KEYSTONE_FED_QVERSION 1u
#define KEYSTONE_FED_MAX_RESULTS_CAP 1024u

/* --- Query types --- */
typedef enum {
    KEYSTONE_FED_QTYPE_TIMELINE_RANGE = 1, /* temporal entries in [hlc_min, hlc_max] */
    KEYSTONE_FED_QTYPE_EXACT_LOOKUP   = 2  /* freshest exact entry for one object id */
} keystone_fed_qtype_t;

/* --- Per-node fan-out outcome (partial-result semantics, §39) --- */
typedef enum {
    KEYSTONE_FED_NODE_OK = 0,
    KEYSTONE_FED_NODE_UNREACHABLE, /* connect refused / no route */
    KEYSTONE_FED_NODE_TIMEOUT,     /* connect/send/recv exceeded timeout_ms */
    KEYSTONE_FED_NODE_PROTOCOL     /* answered, but the frame was invalid */
} keystone_fed_node_status_t;

/* --- Responder (serves one node's indexes over TCP) --- */

typedef struct {
    const char* bind_addr;               /* NULL = 127.0.0.1 */
    uint16_t port;
    const keystone_temporal_index_t* temporal; /* optional */
    const keystone_exact_index_t* exact;       /* optional */
    uint32_t conn_timeout_ms;            /* per-connection receive bound */
    uint32_t max_connections;            /* 0 = default 256; bounded loop */
} keystone_fed_server_config_t;

typedef struct keystone_fed_server keystone_fed_server_t;

keystone_fed_server_t* keystone_fed_server_create(const keystone_fed_server_config_t* config);
int keystone_fed_server_start(keystone_fed_server_t* server);
void keystone_fed_server_stop(keystone_fed_server_t* server);
void keystone_fed_server_destroy(keystone_fed_server_t* server);
uint16_t keystone_fed_server_bound_port(const keystone_fed_server_t* server);

/* --- Coordinator (fan-out) --- */

typedef struct {
    char host[64];
    uint16_t port;
} keystone_fed_node_addr_t;

typedef struct {
    keystone_fed_node_status_t status;
    uint64_t node_generation;      /* responder watermark when known */
    keystone_hlc_t node_watermark;
} keystone_fed_node_result_t;

/*
 * Timeline fan-out: query every node (sequential, per-node timeout), merge
 * the responses with the k-way timeline merge (HLC order + event-id dedup),
 * and label-filter before copying to the caller — entries above the
 * caller's classification/tenant or flagged SECURITY_SENSITIVE below SECRET
 * never leave the coordinator regardless of what a node returned.
 * out_node_results (optional, node_count entries) receives per-node
 * outcomes. Returns the number of entries copied; a partially failed query
 * still returns the healthy nodes' merged results with statuses recorded.
 */
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
);

/*
 * Exact-identity fan-out: query every node for one object and resolve with
 * the federation conflict rule (Epoch > Generation > HLC). Returns 0 on
 * success with *out_entry set, -1 when no node held the object or every
 * node failed; per-node outcomes recorded as above.
 */
int keystone_fed_query_exact_fanout(
    const keystone_fed_node_addr_t* nodes,
    size_t node_count,
    const keystone_security_context_t* caller,
    const keystone_uuid_t* object_id,
    uint32_t timeout_ms,
    keystone_exact_entry_t* out_entry,
    keystone_fed_node_result_t* out_node_results
);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_FED_TRANSPORT_H */
