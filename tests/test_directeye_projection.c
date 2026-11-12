#define _XOPEN_SOURCE 700
#include "../include/keystone_directeye.h"
#include "../include/keystoned.h"
#include "test_macros.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef KEYSTONE_ENABLE_DIRECTEYE_QIHSE
#include "test_directeye_qkp_fixture.h"
#endif

static void remove_tree(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) { unlink(path); return; }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
        else unlink(child);
    }
    closedir(dir);
    rmdir(path);
}

static keystone_directeye_event_t make_event(const char* op, uint64_t cursor,
                                              uint64_t generation, int tombstone,
                                              const char* payload) {
    keystone_directeye_event_t event;
    memset(&event, 0, sizeof(event));
    event.operation_id = op;
    event.record_id = "asset-1";
    event.record_key = "t:7/directeye/canonical/v1/asset-1";
    event.tenant_id = 7;
    event.classification = KEYSTONE_CLASSIFICATION_RESTRICTED;
    event.sci = 2u;
    event.source_generation = generation;
    event.cursor = cursor;
    event.committed_at_ms = 1700000000123ull;
    event.tombstone = tombstone != 0;
    event.payload = payload;
    event.payload_len = payload ? strlen(payload) : 0u;
    return event;
}

static void test_strict_event_decoder(void) {
    const char* payload = "{\"body\":{\"text\":\"escaped \\\\uD83D\\\\uDE00\"}}";
    char json[1024];
    int n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"event_id\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\","
        "\"record_id\":\"asset-1\",\"record_key\":\"t:7/directeye/canonical/v1/asset-1\","
        "\"mutation\":\"upsert\",\"tenant\":7,\"classification\":2,\"sci\":2,"
        "\"generation\":4,\"cursor\":9,\"committed_at_ms\":1700000000000,\"record\":%s}", payload);
    TEST_ASSERT(n > 0 && (size_t)n < sizeof(json));
    keystone_directeye_decoded_event_t decoded;
    TEST_ASSERT(keystone_directeye_decode_event(json, (size_t)n, &decoded) == 0);
    TEST_ASSERT(decoded.event.tenant_id == 7u && decoded.event.cursor == 9u &&
                decoded.event.source_generation == 4u && !decoded.event.tombstone);
    TEST_ASSERT(decoded.event.payload_len == strlen(payload) &&
                memcmp(decoded.event.payload, payload, decoded.event.payload_len) == 0);

    const char* duplicate_root =
        "{\"schema\":1,\"\\u0073chema\":1,\"event_id\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\","
        "\"record_id\":\"asset-1\",\"record_key\":\"t:7/directeye/canonical/v1/asset-1\","
        "\"mutation\":\"delete\",\"tenant\":7,\"classification\":2,\"sci\":2,"
        "\"generation\":4,\"cursor\":9,\"committed_at_ms\":1700000000000,\"record\":null}";
    TEST_ASSERT(keystone_directeye_decode_event(duplicate_root, strlen(duplicate_root), &decoded) == -1);

    n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"event_id\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\","
        "\"record_id\":\"asset-1\",\"record_key\":\"t:7/directeye/canonical/v1/asset-1\","
        "\"mutation\":\"upsert\",\"tenant\":7,\"classification\":2,\"sci\":2,"
        "\"generation\":4,\"cursor\":9,\"committed_at_ms\":1700000000000,"
        "\"record\":{\"x\":1,\"x\":2}}}");
    TEST_ASSERT(n > 0 && (size_t)n < sizeof(json));
    TEST_ASSERT(keystone_directeye_decode_event(json, (size_t)n, &decoded) == -1);

    const char* missing_time =
        "{\"schema\":1,\"event_id\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\","
        "\"record_id\":\"asset-1\",\"record_key\":\"t:7/directeye/canonical/v1/asset-1\","
        "\"mutation\":\"delete\",\"tenant\":7,\"classification\":2,\"sci\":2,"
        "\"generation\":4,\"cursor\":9,\"record\":null}";
    TEST_ASSERT(keystone_directeye_decode_event(missing_time, strlen(missing_time), &decoded) == -1);
}

#ifdef KEYSTONE_ENABLE_DIRECTEYE_QIHSE
static void test_qkp_consumer_end_to_end(void) {
    char root_template[] = ".test_directeye_qkp.XXXXXX";
    char* root = mkdtemp(root_template);
    TEST_ASSERT(root != NULL && chmod(root, 0700) == 0);
    pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child != 0) {
        int status = 0;
        TEST_ASSERT(waitpid(child, &status, 0) == child);
        remove_tree(root);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        return;
    }
    char pub_dir[512], stage_dir[512], client_keys[512], server_pub[512];
    TEST_ASSERT(snprintf(pub_dir, sizeof(pub_dir), "%s/publication", root) > 0);
    TEST_ASSERT(snprintf(stage_dir, sizeof(stage_dir), "%s/stage", root) > 0);
    qkp_fixture_t* fixture = qkp_fixture_create(root, client_keys, sizeof(client_keys),
                                                 server_pub, sizeof(server_pub));
    TEST_ASSERT(fixture != NULL);
    keystone_publication_t* publication = keystone_publication_create(pub_dir);
    TEST_ASSERT(publication != NULL);
    const char* trusted_server[] = {server_pub};
    keystone_directeye_qihse_config_t config = {
        .host = "127.0.0.1",
        .port = qkp_fixture_port(fixture),
        .username = "keystone-indexer-7702",
        .password = "NativeProjectionIndexer123!",
        .qkp_identity_dir = client_keys,
        .trusted_server_pubs = trusted_server,
        .trusted_server_pub_count = 1u,
        .qkp_peer_node_id = "native-directeye-server-fixture",
        .publication = publication,
        .staging_directory = stage_dir,
        .page_limit = 16u,
        .stable_node_id = 77u
    };
    keystone_security_context_t caller = {
        .tenant_id = 77u,
        .classification = KEYSTONE_CLASSIFICATION_TOP_SECRET,
        .compartment_mask = 2u
    };
    char payload[128];
    size_t payload_len = 0u;
    TEST_ASSERT(keystone_directeye_consume_once(&config) == 0);
    TEST_ASSERT(qkp_fixture_ack_is_durable(fixture));
    TEST_ASSERT(keystone_directeye_record_read(publication, &caller, "asset-1",
                                                payload, sizeof(payload), &payload_len) == 0);
    TEST_ASSERT(payload_len == strlen("{\"value\":\"first\"}") &&
                memcmp(payload, "{\"value\":\"first\"}", payload_len) == 0);
    uint64_t sequence = qkp_fixture_mutate(fixture, "22222222-2222-4222-8222-222222222222",
                                           "UPSERT", "1", "{\"value\":\"second\"}");
    TEST_ASSERT(sequence > 1u);
    TEST_ASSERT(keystone_directeye_consume_once(&config) == 0);
    TEST_ASSERT(keystone_directeye_record_read(publication, &caller, "asset-1",
                                                payload, sizeof(payload), &payload_len) == 0);
    TEST_ASSERT(payload_len == strlen("{\"value\":\"second\"}") &&
                memcmp(payload, "{\"value\":\"second\"}", payload_len) == 0);

    char socket_path[512];
    int socket_len = snprintf(socket_path, sizeof(socket_path), "%s/keystoned.sock", root);
    TEST_ASSERT(socket_len > 0 && (size_t)socket_len < sizeof(socket_path));
    keystoned_config_t daemon_config = {
        .socket_path = socket_path,
        .cache_capacity = 64u,
        .initial_fencing_epoch = 1u,
        .enable_security_audit = 0,
        .directeye_publication = publication
    };
    keystoned_server_t* daemon = keystoned_server_create(&daemon_config);
    TEST_ASSERT(daemon != NULL && keystoned_server_start(daemon) == 0);
    keystoned_client_t* daemon_client = keystoned_client_connect(socket_path);
    TEST_ASSERT(daemon_client != NULL);
    char daemon_payload[128];
    size_t daemon_payload_len = 0u;
    TEST_ASSERT(keystoned_client_directeye_record_get(
        daemon_client, &caller, "asset-1", daemon_payload,
        sizeof(daemon_payload), &daemon_payload_len) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(daemon_payload_len == strlen("{\"value\":\"second\"}") &&
                memcmp(daemon_payload, "{\"value\":\"second\"}", daemon_payload_len) == 0);
    keystone_security_context_t denied = caller;
    denied.tenant_id = 78u;
    TEST_ASSERT(keystoned_client_directeye_record_get(
        daemon_client, &denied, "asset-1", daemon_payload,
        sizeof(daemon_payload), &daemon_payload_len) == KEYSTONED_STATUS_NOT_FOUND);
    denied = caller;
    denied.classification = KEYSTONE_CLASSIFICATION_SECRET;
    TEST_ASSERT(keystoned_client_directeye_record_get(
        daemon_client, &denied, "asset-1", daemon_payload,
        sizeof(daemon_payload), &daemon_payload_len) == KEYSTONED_STATUS_NOT_FOUND);
    denied = caller;
    denied.compartment_mask = 0u;
    TEST_ASSERT(keystoned_client_directeye_record_get(
        daemon_client, &denied, "asset-1", daemon_payload,
        sizeof(daemon_payload), &daemon_payload_len) == KEYSTONED_STATUS_NOT_FOUND);

    char expected_sequence[32];
    snprintf(expected_sequence, sizeof(expected_sequence), "%llu",
             (unsigned long long)sequence);
    TEST_ASSERT(qkp_fixture_mutate(fixture, "33333333-3333-4333-8333-333333333333",
                                   "DELETE", expected_sequence, NULL) > sequence);
    TEST_ASSERT(keystone_directeye_consume_once(&config) == 0);
    TEST_ASSERT(keystone_directeye_record_read(publication, &caller, "asset-1",
                                                payload, sizeof(payload), &payload_len) == 1);
    TEST_ASSERT(keystoned_client_directeye_record_get(
        daemon_client, &caller, "asset-1", daemon_payload,
        sizeof(daemon_payload), &daemon_payload_len) == KEYSTONED_STATUS_NOT_FOUND);
    keystoned_client_disconnect(daemon_client);
    keystoned_server_destroy(daemon);
    keystone_publication_destroy(publication);
    publication = keystone_publication_create(pub_dir);
    TEST_ASSERT(publication != NULL);
    TEST_ASSERT(keystone_directeye_record_read(publication, &caller, "asset-1",
                                                payload, sizeof(payload), &payload_len) == 1);
    keystone_publication_destroy(publication);
    qkp_fixture_destroy(fixture);
    fflush(NULL);
    _exit(0);
}
#endif

static void test_projection_replay_tombstone_authorization(void) {
    keystone_directeye_qihse_config_t unconfigured = {0};
    TEST_ASSERT(keystone_directeye_consume_once(&unconfigured) == -1);

    char root_template[] = ".test_directeye_projection.XXXXXX";
    char* root = mkdtemp(root_template);
    TEST_ASSERT(root != NULL);
    char stage[512];
    int n = snprintf(stage, sizeof(stage), "%s/stage", root);
    TEST_ASSERT(n > 0 && (size_t)n < sizeof(stage));
    keystone_publication_t* publication = keystone_publication_create(root);
    TEST_ASSERT(publication != NULL);

    const char* payload1 = "{\"value\":\"first\"}";
    keystone_directeye_event_t first = make_event(
        "11111111-1111-4111-8111-111111111111", 1u, 1u, 0, payload1);
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &first, 1u, 1u, 42u) == 0);

    keystone_exact_index_t* exact = NULL;
    keystone_temporal_index_t* temporal = NULL;
    keystone_index_manifest_t manifest;
    TEST_ASSERT(keystone_publication_open_latest(publication, &exact, &temporal, &manifest) == 0);
    TEST_ASSERT(manifest.source_cursor == 1u && manifest.index_generation == 1u);
    TEST_ASSERT(keystone_exact_index_count(exact) == 1u);
    TEST_ASSERT(keystone_temporal_index_count(temporal) == 1u);
    keystone_exact_index_destroy(exact);
    keystone_temporal_index_destroy(temporal);

    keystone_directeye_event_t conflicting_reuse = first;
    conflicting_reuse.record_id = "asset-other";
    conflicting_reuse.record_key = "t:7/directeye/canonical/v1/asset-other";
    conflicting_reuse.cursor = 2u;
    conflicting_reuse.source_generation = 2u;
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &conflicting_reuse,
                                                 1u, 2u, 42u) == -1);
    TEST_ASSERT(keystone_publication_active_generation(publication) == 1u);

    keystone_security_context_t allowed = {
        .tenant_id = 7u,
        .classification = KEYSTONE_CLASSIFICATION_SECRET,
        .compartment_mask = 2u
    };
    char readback[128];
    size_t readback_len = 0u;
    TEST_ASSERT(keystone_directeye_record_read(publication, &allowed, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 0);
    TEST_ASSERT(readback_len == strlen(payload1) && memcmp(readback, payload1, readback_len) == 0);
    keystone_security_context_t low_clearance = allowed;
    low_clearance.classification = KEYSTONE_CLASSIFICATION_OPS;
    TEST_ASSERT(keystone_directeye_record_read(publication, &low_clearance, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 1);
    keystone_security_context_t missing_sci = allowed;
    missing_sci.compartment_mask = 0u;
    TEST_ASSERT(keystone_directeye_record_read(publication, &missing_sci, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 1);
    keystone_security_context_t other_tenant = allowed;
    other_tenant.tenant_id = 8u;
    TEST_ASSERT(keystone_directeye_record_read(publication, &other_tenant, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 1);

    const char* payload2 = "{\"value\":\"replacement\"}";
    keystone_directeye_event_t replacement = make_event(
        "22222222-2222-4222-8222-222222222222", 2u, 2u, 0, payload2);
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &replacement, 1u, 2u, 42u) == 0);
    TEST_ASSERT(keystone_directeye_record_read(publication, &allowed, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 0);
    TEST_ASSERT(readback_len == strlen(payload2) && memcmp(readback, payload2, readback_len) == 0);
    exact = NULL;
    temporal = NULL;
    TEST_ASSERT(keystone_publication_open_latest(publication, &exact, &temporal, &manifest) == 0);
    TEST_ASSERT(manifest.source_cursor == 2u && manifest.index_generation == 2u);
    TEST_ASSERT(keystone_temporal_index_count(temporal) == 2u);
    const keystone_temporal_entry_t* history = keystone_temporal_index_entries(temporal);
    TEST_ASSERT(history && history[0].hlc.physical_ms == first.committed_at_ms &&
                history[1].hlc.physical_ms == first.committed_at_ms &&
                history[0].hlc.logical == 0u && history[1].hlc.logical == 1u);
    keystone_exact_index_destroy(exact);
    keystone_temporal_index_destroy(temporal);

    keystone_directeye_event_t deletion = make_event(
        "33333333-3333-4333-8333-333333333333", 3u, 3u, 1, NULL);
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &deletion, 1u, 3u, 42u) == 0);
    TEST_ASSERT(keystone_directeye_record_read(publication, &allowed, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 1);
    keystone_publication_destroy(publication);

    publication = keystone_publication_create(root);
    TEST_ASSERT(publication != NULL);
    TEST_ASSERT(keystone_directeye_record_read(publication, &allowed, "asset-1",
                                                readback, sizeof(readback), &readback_len) == 1);
    uint64_t active = keystone_publication_active_generation(publication);
    keystone_directeye_event_t malformed = make_event(
        "44444444-4444-4444-8444-444444444444", 4u, 4u, 0, payload1);
    malformed.committed_at_ms = 0u;
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &malformed, 1u, 4u, 42u) == -1);
    malformed.committed_at_ms = first.committed_at_ms - 1u;
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &malformed, 1u, 4u, 42u) == -1);
    keystone_directeye_event_t replay = deletion;
    TEST_ASSERT(keystone_directeye_project_page(publication, stage, &replay, 1u, 3u, 42u) == 0);
    TEST_ASSERT(keystone_publication_active_generation(publication) == active);

    keystone_publication_destroy(publication);
    remove_tree(root);
}

int main(void) {
    test_strict_event_decoder();
    test_projection_replay_tombstone_authorization();
#ifdef KEYSTONE_ENABLE_DIRECTEYE_QIHSE
    test_qkp_consumer_end_to_end();
#endif
#ifdef KEYSTONE_ENABLE_DIRECTEYE_QIHSE
    puts("DIRECTEYE native projection: JSON validation, replacement, tombstone, replay, authorization, restart, and QKP consumption passed");
#else
    puts("DIRECTEYE native projection: JSON validation, replacement, tombstone, replay, authorization, and restart passed");
#endif
    return 0;
}
