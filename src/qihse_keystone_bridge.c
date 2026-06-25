#include "../include/qihse_keystone_bridge.h"
#include <stdio.h>
#include <string.h>

#ifdef KEYSTONE_ENABLE_QIHSE_BRIDGE
/* 
 * If the bridge is enabled, we link against libqihse.so and use its headers.
 * We include the bare minimum needed for UWP dispatch.
 */
#include <qihse.h>

static keystone_qihse_bridge_config_t g_bridge_cfg = {0};
static int g_bridge_active = 0;

int keystone_qihse_bridge_init(const keystone_qihse_bridge_config_t* config) {
    if (!config || !config->kv_target) return -1;
    g_bridge_cfg = *config;
    g_bridge_active = 1;
    return 0;
}

int keystone_qihse_bridge_dispatch_credential(
    const char* email, 
    const char* pass, 
    int semantic_class) 
{
    if (!g_bridge_active || !g_bridge_cfg.kv_target) return -1;

    /* QIHSE UWP Target 0x01 = Key-Value Set */
    /* Map the semantic class to QIHSE's metadata fields if necessary, 
       but for now we just shove the email:pass combo into the KV store 
       with the proper SCI compartment clearance. */
       
    qihse_kv_store_t* kv = (qihse_kv_store_t*)g_bridge_cfg.kv_target;
    
    /* Prepend the class integer to the value so QIHSE retains the semantic hit */
    char enriched_value[512];
    snprintf(enriched_value, sizeof(enriched_value), "class=%d|pass=%s", semantic_class, pass);

    int rc = qihse_kv_set(
        kv, 
        email, 
        enriched_value, 
        g_bridge_cfg.default_clearance, 
        g_bridge_cfg.default_compartment
    );

    return rc;
}

#else

/* 
 * Stub implementation for standalone KEYSTONE builds.
 * The bridge does nothing and returns an error if not explicitly compiled in.
 */

int keystone_qihse_bridge_init(const keystone_qihse_bridge_config_t* config) {
    (void)config;
    return -1;
}

int keystone_qihse_bridge_dispatch_credential(
    const char* email, 
    const char* pass, 
    int semantic_class) 
{
    (void)email;
    (void)pass;
    (void)semantic_class;
    return -1;
}

#endif
