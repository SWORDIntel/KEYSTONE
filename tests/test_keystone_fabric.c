/**
 * test_keystone_fabric.c - Unit & integration tests for KEYSTONE AI compute fabric
 * capability frames (NODE_CAP) and QIHSE cluster bus serialization.
 */

#include "../include/keystone.h"
#include "../include/keystone_fabric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void test_isa_tier_detection(void) {
    printf("Testing ISA tier detection...\n");
    uint8_t tier = keystone_fabric_detect_isa_tier();
    assert(tier <= 4u);

    uint32_t features = keystone_detect_cpu_features();
    bool avx = (features & KEYSTONE_CPU_AVX) != 0;
    bool avx2 = (features & KEYSTONE_CPU_AVX2) != 0;
    bool avx512 = (features & KEYSTONE_CPU_AVX512) != 0;
    bool amx = (features & KEYSTONE_CPU_AMX) != 0;

    uint8_t expected = (uint8_t)((avx512 && amx) ? 4 : avx512 ? 3 : avx2 ? 2 : avx ? 1 : 0);
    assert(tier == expected);
    printf("  ✓ ISA tier detected: %u (AVX=%d, AVX2=%d, AVX512=%d, AMX=%d)\n",
           tier, avx, avx2, avx512, amx);
}

static void test_accelerator_and_memory_probes(void) {
    printf("Testing accelerator and memory probes...\n");
    uint8_t npu = 99, gpu = 99;
    keystone_fabric_probe_accelerators(&npu, &gpu);
    assert(npu == 0 || npu == 1);
    assert(gpu == 0 || gpu == 1);

    uint32_t free_ram_mb = 0;
    uint16_t load_pct = 0;
    keystone_fabric_probe_memory_load(&free_ram_mb, &load_pct);
#ifndef _WIN32
    assert(free_ram_mb > 0);
#endif
    printf("  ✓ Probes OK: NPU=%u, GPU=%u, Free RAM=%u MB, Load=%u.%02u\n",
           npu, gpu, free_ram_mb, load_pct / 100, load_pct % 100);
}

static void test_node_cap_struct_and_wire_layout(void) {
    printf("Testing node capability probing and wire frame export...\n");
    keystone_node_cap_t cap;
    assert(keystone_probe_node_cap("keystone-test-node-alpha", &cap) == 0);
    assert(strcmp(cap.node_id, "keystone-test-node-alpha") == 0);

    /* Test default fallback for NULL node_id */
    keystone_node_cap_t default_cap;
    assert(keystone_probe_node_cap(NULL, &default_cap) == 0);
    assert(strlen(default_cap.node_id) > 0);

    /* Test export frame buffer size validation */
    uint8_t small_buf[32];
    assert(keystone_export_node_cap_frame("test", small_buf, sizeof(small_buf)) == -1);

    /* Test exact 50-byte wire payload serialization */
    uint8_t frame[KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE];
    int written = keystone_export_node_cap_frame("keystone-test-node-alpha", frame, sizeof(frame));
    assert(written == (int)KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE);
    assert(written == 50);

    /* Validate wire layout */
    assert(memcmp(frame, "keystone-test-node-alpha\0", 25) == 0);
    assert(frame[KEYSTONE_FABRIC_NODE_ID_LEN] == '\0');

    uint8_t isa = frame[41];
    uint8_t npu = frame[42];
    uint8_t gpu = frame[43];
    uint32_t ram = (uint32_t)frame[44] | ((uint32_t)frame[45] << 8) |
                   ((uint32_t)frame[46] << 16) | ((uint32_t)frame[47] << 24);
    uint16_t load = (uint16_t)frame[48] | ((uint16_t)frame[49] << 8);

    assert(isa == cap.isa_tier);
    assert(npu == cap.npu);
    assert(gpu == cap.gpu);
    assert(labs((long)ram - (long)cap.free_ram_mb) <= 1024);
    assert(abs((int)load - (int)cap.load_pct) <= 200);

    /* Round-trip payload parsing */
    keystone_node_cap_t parsed;
    assert(keystone_fabric_parse_node_cap_payload(frame, sizeof(frame), &parsed) == 0);
    assert(strcmp(parsed.node_id, "keystone-test-node-alpha") == 0);
    assert(parsed.isa_tier == isa);
    assert(parsed.npu == npu);
    assert(parsed.gpu == gpu);
    assert(parsed.free_ram_mb == ram);
    assert(parsed.load_pct == load);

    printf("  ✓ Wire frame export & payload round-trip verified (50 bytes)\n");
}

static void test_cluster_bus_datagram_construction(void) {
    printf("Testing cluster bus datagram construction and parsing...\n");

    uint8_t dgram[KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE];
    size_t dlen = 0;
    uint32_t sender_index = 42u;

    assert(keystone_build_node_cap_datagram("node-cluster-bus-77", sender_index,
                                           dgram, sizeof(dgram), &dlen) == 0);
    assert(dlen == 66u);
    assert(dlen == KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE);

    /* Header verification (little-endian) */
    uint32_t magic = (uint32_t)dgram[0] | ((uint32_t)dgram[1] << 8) |
                     ((uint32_t)dgram[2] << 16) | ((uint32_t)dgram[3] << 24);
    uint32_t msg_type = (uint32_t)dgram[4] | ((uint32_t)dgram[5] << 8) |
                        ((uint32_t)dgram[6] << 16) | ((uint32_t)dgram[7] << 24);
    uint32_t sender_idx = (uint32_t)dgram[8] | ((uint32_t)dgram[9] << 8) |
                          ((uint32_t)dgram[10] << 16) | ((uint32_t)dgram[11] << 24);
    uint32_t plen = (uint32_t)dgram[12] | ((uint32_t)dgram[13] << 8) |
                    ((uint32_t)dgram[14] << 16) | ((uint32_t)dgram[15] << 24);

    assert(magic == KEYSTONE_FABRIC_BUS_MAGIC);
    assert(magic == 0x51424E53u); /* 'QBNS' */
    assert(msg_type == KEYSTONE_FABRIC_BUS_MSG_NODE_CAP);
    assert(msg_type == 8u);
    assert(sender_idx == sender_index);
    assert(plen == 50u);

    /* Datagram parsing */
    uint32_t parsed_sender = 0;
    keystone_node_cap_t parsed_cap;
    assert(keystone_fabric_parse_node_cap_datagram(dgram, dlen, &parsed_sender, &parsed_cap) == 0);
    assert(parsed_sender == sender_index);
    assert(strcmp(parsed_cap.node_id, "node-cluster-bus-77") == 0);

    /* Negative tests */
    uint8_t bad_dgram[KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE];
    memcpy(bad_dgram, dgram, sizeof(bad_dgram));
    bad_dgram[0] = 0x00; /* corrupt magic */
    assert(keystone_fabric_parse_node_cap_datagram(bad_dgram, sizeof(bad_dgram), NULL, &parsed_cap) == -1);

    memcpy(bad_dgram, dgram, sizeof(bad_dgram));
    bad_dgram[4] = 0x01; /* change msg_type to PING (1u) */
    assert(keystone_fabric_parse_node_cap_datagram(bad_dgram, sizeof(bad_dgram), NULL, &parsed_cap) == -1);

    assert(keystone_fabric_parse_node_cap_datagram(dgram, dlen - 5, NULL, &parsed_cap) == -1);

    printf("  ✓ Cluster bus datagram construction & verification verified (66 bytes)\n");
}

static void test_loopback_udp_broadcast(void) {
    printf("Testing UDP broadcast of NODE_CAP frame over loopback...\n");

    /* Create local UDP listening socket on ephemeral port */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock >= 0);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;

    assert(bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == 0);

    socklen_t len = sizeof(bind_addr);
    assert(getsockname(sock, (struct sockaddr*)&bind_addr, &len) == 0);
    uint16_t port = ntohs(bind_addr.sin_port);

    /* Broadcast capability frame from KEYSTONE to this ephemeral port */
    const char* test_node_id = "ks-fabric-node-101";
    uint32_t sender_idx = 101u;
    int rc = keystone_broadcast_node_cap("127.0.0.1", port, test_node_id, sender_idx);
    assert(rc == 0);

    /* Receive datagram */
    uint8_t recv_buf[256];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    ssize_t nrecv = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr*)&from_addr, &from_len);
    assert(nrecv == (ssize_t)KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE);

    /* Parse received datagram */
    uint32_t received_sender = 0;
    keystone_node_cap_t cap;
    assert(keystone_fabric_parse_node_cap_datagram(recv_buf, (size_t)nrecv, &received_sender, &cap) == 0);
    assert(received_sender == sender_idx);
    assert(strcmp(cap.node_id, test_node_id) == 0);
    assert(cap.isa_tier <= 4u);
#ifndef _WIN32
    assert(cap.free_ram_mb > 0);
#endif

    close(sock);
    printf("  ✓ UDP loopback broadcast & receipt verified (port %u)\n", port);
}

int main(void) {
    printf("=========================================================\n");
    printf("  KEYSTONE AI Compute Fabric Capability Tests (NODE_CAP)  \n");
    printf("=========================================================\n");

    test_isa_tier_detection();
    test_accelerator_and_memory_probes();
    test_node_cap_struct_and_wire_layout();
    test_cluster_bus_datagram_construction();
    test_loopback_udp_broadcast();

    printf("\n🎉 All KEYSTONE AI Compute Fabric tests passed successfully!\n");
    return 0;
}
