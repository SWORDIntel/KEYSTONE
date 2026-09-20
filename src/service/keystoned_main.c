/*
 * KEYSTONE Service Daemon (keystoned) Main Entrypoint
 *
 * Dedicated unprivileged service communicating over Unix domain sockets (AF_UNIX).
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystoned.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

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
    printf("  -h           Show this help message\n");
}

int main(int argc, char* argv[]) {
    const char* socket_path = KEYSTONED_DEFAULT_SOCKET;
    uint64_t epoch = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            epoch = (uint64_t)strtoull(argv[++i], NULL, 10);
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
