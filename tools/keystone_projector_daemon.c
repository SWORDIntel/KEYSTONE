/*
 * KEYSTONE Projector Daemon CLI (Gate 2 Worker)
 *
 * Standalone background worker utility demonstrating the trusted
 * QIHSE.TXN.READ projector connected to the keystoned Unix domain socket.
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#include "keystone_workflow_projector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -s <path>        Unix domain socket path (default: %s)\n", KEYSTONED_DEFAULT_SOCKET);
    printf("  -T <tenant>      Workflow tenant ID (default: 1)\n");
    printf("  -c <class>       Classification (0=UNCLASS, 1=OPS, 2=SECRET, 3=TOP_SECRET, default: 3)\n");
    printf("  -C <mask>        Compartment bitmask in hex or dec (default: 0xFFFFFFFFFFFFFFFF)\n");
    printf("  -g <hex32>       Genesis 32-byte history identity in hex (64 chars)\n");
    printf("  -b <batch_size>  Batch size (1..4096, default: 256)\n");
    printf("  -n <count>       Total events to stream in simulation (0 = infinite, default: 1000)\n");
    printf("  -i <interval_us> Simulation interval per batch in microseconds (default: 50000)\n");
    printf("  -h, --help       Show this help message\n");
}

int main(int argc, char *argv[]) {
    const char *socket_path = KEYSTONED_DEFAULT_SOCKET;
    uint32_t tenant_id = 1;
    uint32_t classification = KEYSTONE_CLASSIFICATION_TOP_SECRET;
    uint64_t compartment_mask = UINT64_MAX;
    uint32_t batch_size = 256;
    uint64_t total_events = 1000;
    useconds_t interval_us = 50000;

    uint8_t history_id[KEYSTONE_WORKFLOW_HISTORY_ID_SIZE];
    memset(history_id, 0xAA, sizeof(history_id)); /* Default test genesis identity */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            tenant_id = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            classification = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            compartment_mask = (uint64_t)strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            batch_size = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (batch_size == 0 || batch_size > KEYSTONE_WORKFLOW_BATCH_MAX) {
                batch_size = KEYSTONE_WORKFLOW_BATCH_MAX;
            }
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            total_events = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_us = (useconds_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            const char *hex = argv[++i];
            if (strlen(hex) == 64) {
                for (size_t b = 0; b < KEYSTONE_WORKFLOW_HISTORY_ID_SIZE; b++) {
                    unsigned int byte_val = 0;
                    if (sscanf(hex + b * 2, "%02x", &byte_val) == 1) {
                        history_id[b] = (uint8_t)byte_val;
                    }
                }
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("========================================================\n");
    printf("  Starting keystone_projector_daemon\n");
    printf("  Socket:      %s\n", socket_path);
    printf("  Tenant:      %u\n", tenant_id);
    printf("  Clearance:   %u\n", classification);
    printf("  Batch Size:  %u\n", batch_size);
    printf("  Sim Events:  %llu\n", (unsigned long long)total_events);
    printf("========================================================\n");

    keystone_workflow_projector_config_t cfg = {
        .socket_path = socket_path,
        .sec_ctx = {
            .tenant_id = tenant_id,
            .classification = classification,
            .compartment_mask = compartment_mask,
        },
        .max_batch_size = batch_size,
        .auto_reconnect = 1,
    };
    memcpy(cfg.history_id, history_id, sizeof(history_id));

    keystone_workflow_projector_t *proj = keystone_workflow_projector_create(&cfg);
    if (!proj) {
        fprintf(stderr, "[-] Failed to allocate workflow projector\n");
        return 1;
    }

    if (keystone_workflow_projector_connect(proj) != KEYSTONE_WORKFLOW_OK) {
        fprintf(stderr, "[-] Failed to connect to keystoned socket at %s\n", socket_path);
        keystone_workflow_projector_destroy(proj);
        return 1;
    }

    printf("[+] Connected to keystoned. History bound. Ready to project.\n");

    uint64_t current_seq = 0;
    uint64_t wm = 0;
    if (keystone_workflow_projector_get_watermark(proj, &wm) == KEYSTONE_WORKFLOW_OK) {
        current_seq = wm;
        printf("[+] Initial keystoned watermark: %llu\n", (unsigned long long)wm);
    }

    uint64_t events_streamed = 0;
    uint8_t simulated_enc_key[32];
    memset(simulated_enc_key, 0x55, sizeof(simulated_enc_key));

    while (!g_stop_requested && (total_events == 0 || events_streamed < total_events)) {
        current_seq++;
        events_streamed++;

        /* Simulate raw application commit with sensitive payload */
        char sensitive_payload[128];
        snprintf(sensitive_payload, sizeof(sensitive_payload),
                 "CUSTOMER_PII_SSN_999-00-%04llu_CARD_4111_SECRET_BODY",
                 (unsigned long long)current_seq);
        size_t payload_len = strlen(sensitive_payload);

        /* Encrypt payload inside raw QIHSE transaction stream */
        uint8_t encrypted_buf[128];
        keystone_workflow_projector_crypt_payload(
            simulated_enc_key, current_seq, sensitive_payload, encrypted_buf, payload_len
        );

        /* Format record and owner UUIDs */
        char rec_buf[40], own_buf[40];
        snprintf(rec_buf, sizeof(rec_buf), "00000001-0000-0000-0000-%012llu", (unsigned long long)current_seq);
        snprintf(own_buf, sizeof(own_buf), "00000002-0000-0000-0000-%012llu", (unsigned long long)(current_seq % 10));

        keystone_qihse_txn_record_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.committed_seq = current_seq;
        txn.tenant_id = tenant_id;
        txn.classification = (current_seq % 5 == 0) ? KEYSTONE_CLASSIFICATION_SECRET : KEYSTONE_CLASSIFICATION_OPS;
        txn.sci_mask = 0x1;
        keystone_uuid_from_string(rec_buf, &txn.record_id);
        keystone_uuid_from_string(own_buf, &txn.owner_id);
        txn.workflow_type = (current_seq % 8) + 1; /* Cycles through app types */
        txn.is_tombstone = (current_seq % 100 == 0) ? 1 : 0;
        txn.due_at = 1700000000ULL + current_seq;
        txn.is_encrypted = 1;
        memcpy(txn.encryption_key, simulated_enc_key, 32);
        txn.payload = encrypted_buf;
        txn.payload_len = payload_len;

        int rc = keystone_workflow_projector_ingest_raw_txn(proj, &txn);
        if (rc != KEYSTONE_WORKFLOW_OK) {
            fprintf(stderr, "[-] Ingest failed at seq %llu: rc=%d\n", (unsigned long long)current_seq, rc);
            break;
        }

        if (events_streamed % batch_size == 0) {
            keystone_workflow_projector_flush(proj);
            uint64_t cur_wm = 0;
            keystone_workflow_projector_get_watermark(proj, &cur_wm);
            printf("[*] Streamed %llu events. Committed watermark: %llu\n",
                   (unsigned long long)events_streamed, (unsigned long long)cur_wm);
            if (interval_us > 0) {
                usleep(interval_us);
            }
        }
    }

    printf("\n[*] Flushing final batches and shutting down...\n");
    keystone_workflow_projector_flush(proj);

    uint64_t final_wm = 0;
    keystone_workflow_projector_get_watermark(proj, &final_wm);
    printf("[+] Projector daemon complete. Final committed watermark: %llu\n",
           (unsigned long long)final_wm);

    keystone_workflow_projector_destroy(proj);
    return 0;
}
