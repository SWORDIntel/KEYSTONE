#ifndef QIHSE_KEYSTONE_BRIDGE_H
#define QIHSE_KEYSTONE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Forward declaration of a QIHSE KV Store handle.
 * 
 * We use a void pointer to avoid pulling in the massive QIHSE header tree
 * if KEYSTONE is built standalone. When the bridge is active, this maps
 * directly to qihse_kv_store_t*.
 */
typedef void qihse_kv_bridge_handle_t;

/**
 * @brief Number of CRC16 hash slots used for distributed QIHSE cluster
 *        ingestion routing (2^14 = 16,384).
 *
 * Credentials are routed to a slot in [0, KEYSTONE_QIHSE_ROUTING_SLOTS) via a
 * CRC16 of the email (the routing key). Slots are then mapped evenly across
 * the configured cluster nodes, so ingestion load is distributed without
 * hot-spotting on a single QIHSE instance.
 */
#define KEYSTONE_QIHSE_ROUTING_SLOTS 16384u

/**
 * @brief Configuration for the KEYSTONE -> QIHSE bridge
 */
typedef struct {
    qihse_kv_bridge_handle_t* kv_target;  /* Destination for credentials (single-node legacy path) */
    uint16_t default_clearance;           /* SCI classification level */
    uint16_t default_compartment;         /* SCI compartment mask */
    /* --- Distributed QIHSE cluster ingestion (16,384 CRC16 hash slots) --- */
    qihse_kv_bridge_handle_t** cluster_targets; /* Array of per-node KV handles, length = num_cluster_nodes */
    uint32_t num_cluster_nodes;                 /* Number of nodes in the QIHSE cluster (0 = single-node) */
    uint32_t routing_slots;                     /* Hash slot count (0 defaults to KEYSTONE_QIHSE_ROUTING_SLOTS) */
} keystone_qihse_bridge_config_t;

/**
 * @brief Initialize the QIHSE UWP Bridge
 * 
 * Opens the pipeline so that dirty-parsed credentials and semantic tags
 * flow directly into the QIHSE engine via UWP memory layout.
 * 
 * @param config Pointer to bridge configuration.
 * @return 0 on success, -1 if the bridge is not compiled in.
 */
int keystone_qihse_bridge_init(const keystone_qihse_bridge_config_t* config);

/**
 * @brief Dispatch a discovered credential to QIHSE
 * 
 * Called by the dirty parser when an `email:pass` hit is extracted.
 * The semantic_class is the output of the native micro-model.
 * 
 * When a cluster is configured, the email is routed via CRC16 into one of
 * KEYSTONE_QIHSE_ROUTING_SLOTS hash slots and forwarded to the owning node.
 * 
 * @param email Null-terminated email string
 * @param pass Null-terminated password string
 * @param semantic_class Output from dsmil_micro_model_infer
 * @return 0 on success
 */
int keystone_qihse_bridge_dispatch_credential(
    const char* email, 
    const char* pass, 
    int semantic_class);

/**
 * @brief Compute a CRC16-CCITT (poly 0x1021, init 0xFFFF) checksum.
 *
 * Used as the routing hash for distributed cluster ingestion. This is a pure
 * function and is available in standalone (non-bridge) builds so that slot
 * distribution can be unit-tested without linking libqihse.
 */
uint16_t keystone_qihse_crc16(const void* data, size_t len);

/**
 * @brief Route a key (e.g. an email) to one of 16,384 CRC16 hash slots.
 * @return Slot index in [0, KEYSTONE_QIHSE_ROUTING_SLOTS).
 */
uint32_t keystone_qihse_bridge_route_slot(const char* key, size_t key_len);

/**
 * @brief Map a routing slot to a cluster node index.
 * @param slot      Slot from keystone_qihse_bridge_route_slot().
 * @param num_nodes Number of nodes in the cluster (>=1).
 * @return Node index in [0, num_nodes), or 0 if num_nodes == 0.
 */
uint32_t keystone_qihse_bridge_slot_to_node(uint32_t slot, uint32_t num_nodes);

#ifdef __cplusplus
}
#endif
#endif
