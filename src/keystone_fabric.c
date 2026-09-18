/**
 * keystone_fabric.c - KEYSTONE AI Compute Fabric Capability Frames (NODE_CAP)
 *
 * Implements hardware capability detection, frame serialization, and UDP cluster
 * bus emission compatible with QIHSE (QIHSE_BUS_MSG_NODE_CAP = 8u).
 */

#include "../include/keystone_fabric.h"
#include "../include/keystone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

uint8_t keystone_fabric_detect_isa_tier(void) {
    uint32_t features = keystone_detect_cpu_features();
    bool avx = (features & KEYSTONE_CPU_AVX) != 0;
    bool avx2 = (features & KEYSTONE_CPU_AVX2) != 0;
    bool avx512 = (features & KEYSTONE_CPU_AVX512) != 0;
    bool amx = (features & KEYSTONE_CPU_AMX) != 0;

    if (avx512 && amx) {
        return 4u;
    }
    if (avx512) {
        return 3u;
    }
    if (avx2) {
        return 2u;
    }
    if (avx) {
        return 1u;
    }
    return 0u;
}

void keystone_fabric_probe_accelerators(uint8_t* npu, uint8_t* gpu) {
    if (npu) *npu = 0;
    if (gpu) *gpu = 0;

#ifndef _WIN32
    if (npu) {
        if (access("/dev/accel", F_OK) == 0 ||
            access("/var/run/myriad_vpu.sock", F_OK) == 0) {
            *npu = 1u;
        }
    }
    if (gpu) {
        if (access("/dev/dri", F_OK) == 0 ||
            access("/dev/nvidiactl", F_OK) == 0) {
            *gpu = 1u;
        }
    }
#endif
}

void keystone_fabric_probe_memory_load(uint32_t* free_ram_mb, uint16_t* load_pct) {
    if (free_ram_mb) *free_ram_mb = 0;
    if (load_pct) *load_pct = 0;

#ifndef _WIN32
    if (free_ram_mb) {
        FILE* f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[256];
            unsigned long available_kb = 0, free_kb = 0;
            while (fgets(line, sizeof(line), f)) {
                if (available_kb == 0 && sscanf(line, "MemAvailable: %lu kB", &available_kb) == 1) continue;
                if (free_kb == 0 && sscanf(line, "MemFree: %lu kB", &free_kb) == 1) continue;
            }
            fclose(f);
            unsigned long kb = available_kb ? available_kb : free_kb;
            *free_ram_mb = (uint32_t)(kb / 1024u);
        }
    }
    if (load_pct) {
        FILE* f = fopen("/proc/loadavg", "r");
        if (f) {
            float load1 = 0.0f;
            if (fscanf(f, "%f", &load1) == 1) {
                double pct = (double)load1 * 100.0;
                if (pct < 0.0) pct = 0.0;
                if (pct > 65535.0) pct = 65535.0;
                *load_pct = (uint16_t)(pct + 0.5);
            }
            fclose(f);
        }
    }
#endif
}

int keystone_probe_node_cap(const char* node_id, keystone_node_cap_t* out_cap) {
    if (!out_cap) return -1;
    memset(out_cap, 0, sizeof(*out_cap));

    if (node_id && node_id[0] != '\0') {
        strncpy(out_cap->node_id, node_id, KEYSTONE_FABRIC_NODE_ID_LEN);
        out_cap->node_id[KEYSTONE_FABRIC_NODE_ID_LEN] = '\0';
    } else {
#ifndef _WIN32
        if (gethostname(out_cap->node_id, KEYSTONE_FABRIC_NODE_ID_LEN) != 0 || out_cap->node_id[0] == '\0') {
            strncpy(out_cap->node_id, "keystone-node", KEYSTONE_FABRIC_NODE_ID_LEN);
        }
#else
        strncpy(out_cap->node_id, "keystone-node", KEYSTONE_FABRIC_NODE_ID_LEN);
#endif
        out_cap->node_id[KEYSTONE_FABRIC_NODE_ID_LEN] = '\0';
    }

    out_cap->isa_tier = keystone_fabric_detect_isa_tier();
    keystone_fabric_probe_accelerators(&out_cap->npu, &out_cap->gpu);
    keystone_fabric_probe_memory_load(&out_cap->free_ram_mb, &out_cap->load_pct);

    return 0;
}

int keystone_export_node_cap_frame(const char* node_id, uint8_t* out_buf, size_t buf_size) {
    if (!out_buf || buf_size < KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE) {
        return -1;
    }

    keystone_node_cap_t cap;
    if (keystone_probe_node_cap(node_id, &cap) != 0) {
        return -1;
    }

    uint8_t* p = out_buf;

    /* 1. node_id[41]: 40 chars max + NUL terminator */
    memcpy(p, cap.node_id, KEYSTONE_FABRIC_NODE_ID_LEN + 1u);
    p += KEYSTONE_FABRIC_NODE_ID_LEN + 1u;

    /* 2. isa_tier: u8 */
    *p++ = cap.isa_tier;

    /* 3. npu: u8 */
    *p++ = cap.npu;

    /* 4. gpu: u8 */
    *p++ = cap.gpu;

    /* 5. free_ram_mb: u32 (little-endian) */
    p[0] = (uint8_t)(cap.free_ram_mb & 0xFFu);
    p[1] = (uint8_t)((cap.free_ram_mb >> 8) & 0xFFu);
    p[2] = (uint8_t)((cap.free_ram_mb >> 16) & 0xFFu);
    p[3] = (uint8_t)((cap.free_ram_mb >> 24) & 0xFFu);
    p += 4;

    /* 6. load_pct: u16 (little-endian) */
    p[0] = (uint8_t)(cap.load_pct & 0xFFu);
    p[1] = (uint8_t)((cap.load_pct >> 8) & 0xFFu);
    p += 2;

    return (int)KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE;
}

int keystone_build_node_cap_datagram(const char* node_id,
                                    uint32_t sender_index,
                                    uint8_t* out_buf,
                                    size_t buf_size,
                                    size_t* out_len) {
    if (!out_buf || buf_size < KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE) {
        return -1;
    }

    uint32_t magic = KEYSTONE_FABRIC_BUS_MAGIC;
    uint32_t msg_type = KEYSTONE_FABRIC_BUS_MSG_NODE_CAP;
    uint32_t payload_len = KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE;

    uint8_t* p = out_buf;

    /* Little-endian header packing */
    p[0] = (uint8_t)(magic & 0xFFu);
    p[1] = (uint8_t)((magic >> 8) & 0xFFu);
    p[2] = (uint8_t)((magic >> 16) & 0xFFu);
    p[3] = (uint8_t)((magic >> 24) & 0xFFu);
    p += 4;

    p[0] = (uint8_t)(msg_type & 0xFFu);
    p[1] = (uint8_t)((msg_type >> 8) & 0xFFu);
    p[2] = (uint8_t)((msg_type >> 16) & 0xFFu);
    p[3] = (uint8_t)((msg_type >> 24) & 0xFFu);
    p += 4;

    p[0] = (uint8_t)(sender_index & 0xFFu);
    p[1] = (uint8_t)((sender_index >> 8) & 0xFFu);
    p[2] = (uint8_t)((sender_index >> 16) & 0xFFu);
    p[3] = (uint8_t)((sender_index >> 24) & 0xFFu);
    p += 4;

    p[0] = (uint8_t)(payload_len & 0xFFu);
    p[1] = (uint8_t)((payload_len >> 8) & 0xFFu);
    p[2] = (uint8_t)((payload_len >> 16) & 0xFFu);
    p[3] = (uint8_t)((payload_len >> 24) & 0xFFu);
    p += 4;

    int res = keystone_export_node_cap_frame(node_id, p, buf_size - KEYSTONE_FABRIC_BUS_HEADER_SIZE);
    if (res < 0) {
        return -1;
    }

    if (out_len) {
        *out_len = KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE;
    }
    return 0;
}

int keystone_broadcast_node_cap(const char* host,
                               uint16_t port,
                               const char* node_id,
                               uint32_t sender_index) {
#ifndef _WIN32
    uint8_t dgram[KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE];
    size_t dlen = 0;
    if (keystone_build_node_cap_datagram(node_id, sender_index, dgram, sizeof(dgram), &dlen) != 0) {
        return -1;
    }

    if (!host || host[0] == '\0') {
        host = "127.0.0.1";
    }
    if (port == 0) {
        port = KEYSTONE_FABRIC_BUS_DEFAULT_PORT;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    ssize_t sent = sendto(sock, dgram, dlen, 0, (struct sockaddr*)&dest, sizeof(dest));
    close(sock);

    return (sent == (ssize_t)dlen) ? 0 : -1;
#else
    (void)host; (void)port; (void)node_id; (void)sender_index;
    return -1;
#endif
}

int keystone_fabric_parse_node_cap_payload(const uint8_t* payload,
                                          size_t payload_len,
                                          keystone_node_cap_t* out_cap) {
    if (!payload || !out_cap || payload_len < KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE) {
        return -1;
    }

    memset(out_cap, 0, sizeof(*out_cap));
    memcpy(out_cap->node_id, payload, KEYSTONE_FABRIC_NODE_ID_LEN + 1u);
    out_cap->node_id[KEYSTONE_FABRIC_NODE_ID_LEN] = '\0';

    const uint8_t* p = payload + KEYSTONE_FABRIC_NODE_ID_LEN + 1u;
    out_cap->isa_tier = *p++;
    out_cap->npu = *p++;
    out_cap->gpu = *p++;

    out_cap->free_ram_mb = (uint32_t)p[0] |
                          ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) |
                          ((uint32_t)p[3] << 24);
    p += 4;

    out_cap->load_pct = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

    return 0;
}

int keystone_fabric_parse_node_cap_datagram(const uint8_t* dgram,
                                           size_t dgram_len,
                                           uint32_t* out_sender_index,
                                           keystone_node_cap_t* out_cap) {
    if (!dgram || dgram_len < KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE) {
        return -1;
    }

    uint32_t magic = (uint32_t)dgram[0] |
                    ((uint32_t)dgram[1] << 8) |
                    ((uint32_t)dgram[2] << 16) |
                    ((uint32_t)dgram[3] << 24);

    uint32_t msg_type = (uint32_t)dgram[4] |
                       ((uint32_t)dgram[5] << 8) |
                       ((uint32_t)dgram[6] << 16) |
                       ((uint32_t)dgram[7] << 24);

    uint32_t sender_idx = (uint32_t)dgram[8] |
                         ((uint32_t)dgram[9] << 8) |
                         ((uint32_t)dgram[10] << 16) |
                         ((uint32_t)dgram[11] << 24);

    uint32_t plen = (uint32_t)dgram[12] |
                   ((uint32_t)dgram[13] << 8) |
                   ((uint32_t)dgram[14] << 16) |
                   ((uint32_t)dgram[15] << 24);

    if (magic != KEYSTONE_FABRIC_BUS_MAGIC || msg_type != KEYSTONE_FABRIC_BUS_MSG_NODE_CAP) {
        return -1;
    }

    if (plen < KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE) {
        return -1;
    }

    if (out_sender_index) {
        *out_sender_index = sender_idx;
    }

    return keystone_fabric_parse_node_cap_payload(dgram + KEYSTONE_FABRIC_BUS_HEADER_SIZE,
                                                  dgram_len - KEYSTONE_FABRIC_BUS_HEADER_SIZE,
                                                  out_cap);
}
