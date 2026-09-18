/**
 * keystone_fabric.h - KEYSTONE AI Compute Fabric Capability Frames (NODE_CAP)
 *
 * Exposes node capability profiling and serialization for integration with the
 * QIHSE cluster bus (QIHSE_BUS_MSG_NODE_CAP = 8u).
 */

#ifndef KEYSTONE_FABRIC_H
#define KEYSTONE_FABRIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wire protocol specification compatible with QIHSE Cluster Bus:
 *
 * Datagram header (16 bytes, little-endian):
 *   uint32_t magic          = 0x51424E53 ('QBNS')
 *   uint32_t message_type   = 8u (NODE_CAP)
 *   uint32_t sender_index   = sender cluster topology index
 *   uint32_t payload_len    = 50u
 *
 * Wire payload (50 bytes, packed little-endian):
 *   char node_id[41]        NUL-terminated sender node ID (40 chars max + NUL)
 *   uint8_t isa_tier        0=generic, 1=AVX, 2=AVX2, 3=AVX-512, 4=AVX-512+AMX
 *   uint8_t npu             1 if accelerator device (/dev/accel or VPU) present
 *   uint8_t gpu             1 if GPU device (/dev/dri or /dev/nvidiactl) present
 *   uint32_t free_ram_mb    MemAvailable (fallback MemFree) from /proc/meminfo in MB
 *   uint16_t load_pct       1-minute load average * 100, clamped to 65535
 */

#define KEYSTONE_FABRIC_NODE_ID_LEN           40u
#define KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE 50u
#define KEYSTONE_FABRIC_BUS_HEADER_SIZE       16u
#define KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE     (KEYSTONE_FABRIC_BUS_HEADER_SIZE + KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE) /* 66u */

#define KEYSTONE_FABRIC_BUS_MAGIC             0x51424E53u /* 'QBNS' */
#define KEYSTONE_FABRIC_BUS_MSG_NODE_CAP      8u
#define KEYSTONE_FABRIC_BUS_DEFAULT_PORT      16379u

typedef struct {
    char node_id[KEYSTONE_FABRIC_NODE_ID_LEN + 1u];
    uint8_t isa_tier;
    uint8_t npu;
    uint8_t gpu;
    uint32_t free_ram_mb;
    uint16_t load_pct;
} keystone_node_cap_t;

/**
 * Detect ISA tier based on KEYSTONE's runtime CPU feature detection.
 * Returns:
 *   4 = AVX-512 + AMX (both present)
 *   3 = AVX-512
 *   2 = AVX2
 *   1 = AVX
 *   0 = Generic baseline
 */
uint8_t keystone_fabric_detect_isa_tier(void);

/**
 * Probe local accelerator (NPU) and GPU hardware availability.
 * Sets *npu to 1 if /dev/accel or VPU socket is present, 0 otherwise.
 * Sets *gpu to 1 if /dev/dri or /dev/nvidiactl is present, 0 otherwise.
 */
void keystone_fabric_probe_accelerators(uint8_t* npu, uint8_t* gpu);

/**
 * Probe available memory (MB) and 1-minute load average percentage (* 100).
 */
void keystone_fabric_probe_memory_load(uint32_t* free_ram_mb, uint16_t* load_pct);

/**
 * Probe the full capability profile for the local node.
 * If node_id is NULL or empty, hostname or "keystone-node" is used.
 * Returns 0 on success, -1 on error (e.g. out_cap is NULL).
 */
int keystone_probe_node_cap(const char* node_id, keystone_node_cap_t* out_cap);

/**
 * Export a 50-byte NODE_CAP wire payload into out_buf.
 * Returns KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE (50) on success, or -1 on error.
 */
int keystone_export_node_cap_frame(const char* node_id, uint8_t* out_buf, size_t buf_size);

/**
 * Export a full 66-byte cluster bus datagram (16 bytes header + 50 bytes payload).
 * Returns 0 on success, or -1 on error.
 */
int keystone_build_node_cap_datagram(const char* node_id,
                                    uint32_t sender_index,
                                    uint8_t* out_buf,
                                    size_t buf_size,
                                    size_t* out_len);

/**
 * Broadcast/send a NODE_CAP datagram via UDP to the specified host and port.
 * If host is NULL, defaults to "127.0.0.1".
 * If port is 0, defaults to KEYSTONE_FABRIC_BUS_DEFAULT_PORT (16379).
 * Returns 0 on success, or -1 on network error.
 */
int keystone_broadcast_node_cap(const char* host,
                               uint16_t port,
                               const char* node_id,
                               uint32_t sender_index);

/**
 * Parse a 50-byte NODE_CAP wire payload into a keystone_node_cap_t struct.
 * Returns 0 on success, or -1 on error.
 */
int keystone_fabric_parse_node_cap_payload(const uint8_t* payload,
                                          size_t payload_len,
                                          keystone_node_cap_t* out_cap);

/**
 * Parse a full cluster bus datagram, verifying magic (0x51424E53) and message type (8u).
 * Returns 0 on success, or -1 on error.
 */
int keystone_fabric_parse_node_cap_datagram(const uint8_t* dgram,
                                           size_t dgram_len,
                                           uint32_t* out_sender_index,
                                           keystone_node_cap_t* out_cap);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTONE_FABRIC_H */
