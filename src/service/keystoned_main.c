/*
 * KEYSTONE Service Daemon (keystoned) Main Entrypoint
 *
 * Dedicated unprivileged service communicating over Unix domain sockets (AF_UNIX).
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * SRSL-1.1 License.
 */

#include "keystoned.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -s <path>    Unix domain socket path (default: %s)\n", KEYSTONED_DEFAULT_SOCKET);
    printf("  -e <epoch>   Initial fencing epoch (default: 1)\n");
    printf("  -T <tenant>  Enable workflow IPC for this dedicated daemon UID and tenant\n");
    printf("  -h           Show this help message\n");
}

int main(int argc, char* argv[]) {
    const char* socket_path = KEYSTONED_DEFAULT_SOCKET;
    uint64_t epoch = 1;
    uint32_t workflow_tenant = 0;
    int workflow_enabled = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            epoch = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            char* end = NULL;
            errno = 0;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (errno || !end || *end || value > UINT32_MAX) {
                fprintf(stderr, "invalid workflow tenant\n");
                return 2;
            }
            workflow_tenant = (uint32_t)value;
            workflow_enabled = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("========================================================\n");
    printf("  Starting keystoned (KEYSTONE Service Daemon)\n");
    printf("  Socket: %s\n", socket_path);
    printf("  Epoch:  %llu\n", (unsigned long long)epoch);
    printf("========================================================\n");

    keystoned_config_t config = {
        .socket_path = socket_path,
        .cache_capacity = 4096,
        .initial_fencing_epoch = epoch,
        .enable_security_audit = 1
    };

    keystoned_server_t* server = keystoned_server_create(&config);
    if (!server) {
        fprintf(stderr, "[-] Failed to create keystoned server\n");
        return 1;
    }

    if (workflow_enabled) {
        /* Run this daemon under a dedicated OS account. The restricted socket
         * and SO_PEERCRED bind that account to this one tenant. */
        keystone_security_context_t principal = {
            .tenant_id = workflow_tenant,
            .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
            .compartment_mask = UINT64_MAX,
        };
        if (keystoned_server_bind_peer_principal(server, geteuid(),
                                                 &principal) != 0) {
            fprintf(stderr, "cannot bind workflow principal\n");
            keystoned_server_destroy(server);
            return 1;
        }
    }

    if (keystoned_server_start(server) != 0) {
        fprintf(stderr, "[-] Failed to bind and listen on socket: %s\n", socket_path);
        keystoned_server_destroy(server);
        return 1;
    }

    printf("[+] keystoned running. Press Ctrl+C to terminate.\n");

    while (!g_stop_requested) {
        usleep(100000);
    }

    printf("\n[*] Stopping keystoned...\n");
    keystoned_server_destroy(server);
    printf("[+] keystoned shutdown complete.\n");

    return 0;
}
