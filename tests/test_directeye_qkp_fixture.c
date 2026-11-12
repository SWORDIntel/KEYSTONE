#define _XOPEN_SOURCE 700
#include "test_directeye_qkp_fixture.h"
#include "test_macros.h"

#include <qihse_auth.h>
#include <qihse_keystone.h>
#include <qihse_kv_store.h>
#include <qihse_resp_wire.h>
#include "persistence/qihse_pqc_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct qkp_fixture {
    qihse_user_t* operator_user;
    qihse_user_t* writer;
    qihse_user_t* indexer;
    qihse_kv_store_t* store;
    qihse_resp_server_t* server;
    char server_dsa[640];
    char server_kem[640];
    char server_kem_pub[640];
    const char* trusted_clients[1];
};

static uint64_t execute_mutation(qkp_fixture_t* fixture, const char* operation_id,
                                 const char* verb, const char* expected_sequence,
                                 const char* payload) {
    const char* args[6] = {"DIRECTEYE.RECORD.MUTATE", operation_id, "asset-1", verb,
                           expected_sequence, payload};
    size_t argc = payload ? 6u : 5u;
    qihse_resp_arg_t wire[6];
    for (size_t i = 0; i < argc; i++) {
        wire[i].data = (const uint8_t*)args[i];
        wire[i].len = strlen(args[i]);
    }
    uint8_t* raw = NULL;
    size_t raw_len = 0u;
    TEST_ASSERT(qihse_resp_server_execute(fixture->server, fixture->writer, argc, wire,
                                          &raw, &raw_len));
    char* text = malloc(raw_len + 1u);
    TEST_ASSERT(text != NULL);
    memcpy(text, raw, raw_len);
    text[raw_len] = '\0';
    free(raw);
    TEST_ASSERT(text[0] == '*');
    const char* p = strstr(text, "\r\n");
    TEST_ASSERT(p != NULL);
    p += 2;
    uint64_t generation = 0u;
    for (size_t value = 0; value < 2u; value++) {
        TEST_ASSERT(*p == '$');
        char* end = NULL;
        unsigned long width = strtoul(p + 1, &end, 10);
        TEST_ASSERT(end && end[0] == '\r' && end[1] == '\n');
        const char* digits = end + 2;
        char* number_end = NULL;
        unsigned long long number = strtoull(digits, &number_end, 10);
        TEST_ASSERT(number_end == digits + width && number_end[0] == '\r' && number_end[1] == '\n');
        if (value == 0u) generation = (uint64_t)number;
        p = number_end + 2;
    }
    free(text);
    return generation;
}

qkp_fixture_t* qkp_fixture_create(const char* root, char* client_keys, size_t client_keys_size,
                                  char* server_public_key, size_t server_public_key_size) {
    qkp_fixture_t* fixture = calloc(1u, sizeof(*fixture));
    TEST_ASSERT(fixture != NULL);
    char data_dir[512], server_keys[512];
    TEST_ASSERT(snprintf(data_dir, sizeof(data_dir), "%s/qihse", root) > 0);
    TEST_ASSERT(snprintf(server_keys, sizeof(server_keys), "%s/server", root) > 0);
    TEST_ASSERT(snprintf(client_keys, client_keys_size, "%s/client", root) > 0);
    TEST_ASSERT(snprintf(server_public_key, server_public_key_size,
                         "%s/qihse_dsa_pub.pem", server_keys) > 0);
    TEST_ASSERT(mkdir(data_dir, 0700) == 0 && mkdir(server_keys, 0700) == 0 &&
                mkdir(client_keys, 0700) == 0);
    TEST_ASSERT(setenv("QIHSE_DATA_DIR", data_dir, 1) == 0);
    TEST_ASSERT(setenv("QIHSE_FIPS_MODE", "disabled", 1) == 0);
    TEST_ASSERT(qihse_auth_init());
    TEST_ASSERT(qihse_auth_bootstrap_operator("NativeProjectionOperator123!"));
    fixture->operator_user = qihse_auth_get_user(0u);
    TEST_ASSERT(fixture->operator_user != NULL);
    fixture->writer = qihse_auth_create_tenant_user(
        fixture->operator_user, 77u, 7701u, QIHSE_ROLE_ANALYST, 4u, 2u,
        "NativeProjectionWriter123!", false);
    fixture->indexer = qihse_keystone_feed_identity_provision(
        fixture->operator_user, 77u, 7702u, 5u, UINT16_MAX,
        "NativeProjectionIndexer123!");
    TEST_ASSERT(fixture->writer && fixture->indexer);
    TEST_ASSERT(qihse_pqc_keygen(server_keys) && qihse_pqc_keygen(client_keys));

    char client_pub[640];
    TEST_ASSERT(snprintf(fixture->server_dsa, sizeof(fixture->server_dsa), "%s/qihse_dsa_key.pem", server_keys) > 0);
    TEST_ASSERT(snprintf(fixture->server_kem, sizeof(fixture->server_kem), "%s/qihse_kem_key.pem", server_keys) > 0);
    TEST_ASSERT(snprintf(fixture->server_kem_pub, sizeof(fixture->server_kem_pub), "%s/qihse_kem_pub.pem", server_keys) > 0);
    TEST_ASSERT(snprintf(client_pub, sizeof(client_pub), "%s/qihse_dsa_pub.pem", client_keys) > 0);
    fixture->trusted_clients[0] = strdup(client_pub);
    TEST_ASSERT(fixture->trusted_clients[0] != NULL);
    fixture->store = qihse_kv_store_create_user(fixture->operator_user);
    TEST_ASSERT(fixture->store != NULL);
    qihse_resp_server_config_t config;
    qihse_resp_server_config_init(&config);
    config.store = fixture->store;
    config.auth_required = true;
    config.enable_uwp_bridge = true;
    config.bind_address = "127.0.0.1";
    config.port = 17497u;
    config.pqc.dsa_key_path = fixture->server_dsa;
    config.pqc.kem_key_path = fixture->server_kem;
    config.pqc.kem_pub_path = fixture->server_kem_pub;
    config.pqc.trusted_pubs = fixture->trusted_clients;
    config.pqc.trusted_count = 1u;
    config.pqc.node_id = "native-directeye-server-fixture";
    config.pqc.require = true;
    fixture->server = qihse_resp_server_create(&config);
    TEST_ASSERT(fixture->server != NULL);
    TEST_ASSERT(execute_mutation(fixture, "11111111-1111-4111-8111-111111111111",
                                 "UPSERT", "0", "{\"value\":\"first\"}") > 0u);
    TEST_ASSERT(qihse_resp_server_start(fixture->server));
    return fixture;
}

uint16_t qkp_fixture_port(const qkp_fixture_t* fixture) {
    return fixture ? qihse_resp_server_port(fixture->server) : 0u;
}

uint64_t qkp_fixture_mutate(qkp_fixture_t* fixture, const char* operation_id,
                            const char* verb, const char* expected_sequence,
                            const char* payload) {
    TEST_ASSERT(fixture != NULL);
    return execute_mutation(fixture, operation_id, verb, expected_sequence, payload);
}

int qkp_fixture_ack_is_durable(qkp_fixture_t* fixture) {
    if (!fixture || !fixture->server || !fixture->indexer) return 0;
    const char* arg = "0";
    qihse_resp_arg_t wire[2] = {
        {.data = (const uint8_t*)"DIRECTEYE.FEED.ACK", .len = 18u},
        {.data = (const uint8_t*)arg, .len = 1u}
    };
    uint8_t* raw = NULL;
    size_t raw_len = 0u;
    if (!qihse_resp_server_execute(fixture->server, fixture->indexer, 2u,
                                   wire, &raw, &raw_len))
        return 0;
    int regressed_ack_rejected = raw_len > 0u && raw[0] == '-';
    free(raw);
    return regressed_ack_rejected;
}

void qkp_fixture_destroy(qkp_fixture_t* fixture) {
    if (!fixture) return;
    qihse_resp_server_stop(fixture->server);
    qihse_resp_server_destroy(fixture->server);
    qihse_kv_store_destroy(fixture->store);
    free((void*)fixture->trusted_clients[0]);
    free(fixture);
}
