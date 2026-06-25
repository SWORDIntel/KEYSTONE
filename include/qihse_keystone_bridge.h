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
 * @brief Configuration for the KEYSTONE -> QIHSE bridge
 */
typedef struct {
    qihse_kv_bridge_handle_t* kv_target;  /* Destination for credentials */
    uint16_t default_clearance;           /* SCI classification level */
    uint16_t default_compartment;         /* SCI compartment mask */
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
 * @param email Null-terminated email string
 * @param pass Null-terminated password string
 * @param semantic_class Output from dsmil_micro_model_infer
 * @return 0 on success
 */
int keystone_qihse_bridge_dispatch_credential(
    const char* email, 
    const char* pass, 
    int semantic_class);

#ifdef __cplusplus
}
#endif
#endif
