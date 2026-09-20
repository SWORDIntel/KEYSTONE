/*
 * KEYSTONE Security-Aware Service Mode (keystoned) Test Suite
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "keystoned.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void test_service_lifecycle_and_security(void) {
    printf("[*] Testing keystoned daemon lifecycle and security partitioning...\n");

    const char* sock_path = "/tmp/keystone_test_service.sock";
    unlink(sock_path);

    keystoned_config_t cfg = {
        .socket_path = sock_path,
        .cache_capacity = 1024,
        .initial_fencing_epoch = 1,
        .enable_security_audit = 1
    };

    keystoned_server_t* server = keystoned_server_create(&cfg);
    TEST_ASSERT(server != NULL);
    TEST_ASSERT(keystoned_server_start(server) == 0);

    /* Wait for socket to become active */
    usleep(20000);

    /* 1. Client Connect & Ping */
    keystoned_client_t* client = keystoned_client_connect(sock_path);
    TEST_ASSERT(client != NULL);
    TEST_ASSERT(keystoned_client_ping(client) == 0);

    /* 2. Setup Security Contexts */
    keystone_security_context_t ctx_unclass = { .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_UNCLASSIFIED, .compartment_mask = 0 };
    keystone_security_context_t ctx_ops     = { .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS, .compartment_mask = 0 };
    keystone_security_context_t ctx_secret  = { .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_SECRET, .compartment_mask = 0 };

    /* 3. Ingest 3 records with different classification levels */
    keystone_uuid_t id_unclass, id_ops, id_secret;
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000001", &id_unclass);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000002", &id_ops);
    keystone_uuid_from_string("00000000-0000-0000-0000-000000000003", &id_secret);

    keystone_uuid_t evt1, evt2, evt3, node;
    keystone_uuid_from_string("eeeeeeee-0000-0000-0000-000000000001", &evt1);
    keystone_uuid_from_string("eeeeeeee-0000-0000-0000-000000000002", &evt2);
    keystone_uuid_from_string("eeeeeeee-0000-0000-0000-000000000003", &evt3);
    keystone_uuid_from_string("nnnnnnnn-0000-0000-0000-000000000001", &node);

    keystone_federation_record_t r1 = {
        .source_object_id = id_unclass, .source_event_id = evt1, .source_node_id = node,
        .source_generation = 10, .fencing_epoch = 1,
        .source_hlc = { .physical_ms = 1000100, .logical = 0, .node_id = 1 },
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_UNCLASSIFIED,
        .object_type = KEYSTONE_OBJ_VM, .flags = KEYSTONE_RECORD_FLAG_EVENT
    };
    keystone_federation_record_t r2 = {
        .source_object_id = id_ops, .source_event_id = evt2, .source_node_id = node,
        .source_generation = 11, .fencing_epoch = 1,
        .source_hlc = { .physical_ms = 1000200, .logical = 0, .node_id = 1 },
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS,
        .object_type = KEYSTONE_OBJ_VM, .flags = KEYSTONE_RECORD_FLAG_EVENT
    };
    keystone_federation_record_t r3 = {
        .source_object_id = id_secret, .source_event_id = evt3, .source_node_id = node,
        .source_generation = 12, .fencing_epoch = 1,
        .source_hlc = { .physical_ms = 1000300, .logical = 0, .node_id = 1 },
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_SECRET,
        .object_type = KEYSTONE_OBJ_VM, .flags = KEYSTONE_RECORD_FLAG_EVENT
    };

    /* Ingest via client */
    TEST_ASSERT(keystoned_client_ingest(client, &ctx_ops, &r1) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_ingest(client, &ctx_ops, &r2) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_ingest(client, &ctx_ops, &r3) == KEYSTONED_STATUS_OK);

    /* 4. Exact Lookups with Security Partitioning */
    keystone_exact_entry_t entry;

    /* UNCLASSIFIED caller */
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_unclass, &id_unclass, &entry) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_unclass, &id_ops, &entry) == KEYSTONED_STATUS_DENIED);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_unclass, &id_secret, &entry) == KEYSTONED_STATUS_DENIED);

    /* OPS caller */
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_ops, &id_unclass, &entry) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_ops, &id_ops, &entry) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_ops, &id_secret, &entry) == KEYSTONED_STATUS_DENIED);

    /* SECRET caller */
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_secret, &id_unclass, &entry) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_secret, &id_ops, &entry) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_secret, &id_secret, &entry) == KEYSTONED_STATUS_OK);

    /* 5. Temporal Range Query with Security Filtering */
    keystone_hlc_t hmin = { .physical_ms = 1000000, .logical = 0, .node_id = 0 };
    keystone_hlc_t hmax = { .physical_ms = 1000500, .logical = 0xFFFFFFFFu, .node_id = 0xFFFFFFFFu };
    keystone_temporal_entry_t tresults[8];
    size_t count = 0;

    /* UNCLASSIFIED caller only sees 1 record */
    TEST_ASSERT(keystoned_client_temporal_range(client, &ctx_unclass, &hmin, &hmax, tresults, 8, &count) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(keystone_uuid_equal(&tresults[0].object_id, &id_unclass));

    /* OPS caller sees 2 records */
    TEST_ASSERT(keystoned_client_temporal_range(client, &ctx_ops, &hmin, &hmax, tresults, 8, &count) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(count == 2);

    /* SECRET caller sees all 3 records */
    TEST_ASSERT(keystoned_client_temporal_range(client, &ctx_secret, &hmin, &hmax, tresults, 8, &count) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(count == 3);

    /* 6. Atomic Generation Publication */
    uint64_t initial_gen = 0;
    TEST_ASSERT(keystoned_client_get_status(client, &initial_gen, NULL, NULL) == 0);
    TEST_ASSERT(initial_gen == 1);

    /* Build Generation 2 off to the side */
    keystone_exact_index_t* gen2_exact = keystone_exact_index_create(128);
    keystone_temporal_index_t* gen2_temporal = keystone_temporal_index_create(256);

    keystone_uuid_t id_new;
    keystone_uuid_from_string("99999999-9999-9999-9999-999999999999", &id_new);
    keystone_exact_entry_t e_new = {
        .resource_id = id_new, .latest_generation = 200, .fencing_epoch = 2,
        .classification = KEYSTONE_CLASSIFICATION_OPS
    };
    keystone_exact_index_upsert(gen2_exact, &e_new);

    /* Swap Generation Atomically */
    TEST_ASSERT(keystoned_server_publish_generation(server, gen2_exact, gen2_temporal, 2) == 0);

    /* Client immediately observes Generation 2 and the new record */
    uint64_t observed_gen = 0;
    TEST_ASSERT(keystoned_client_get_status(client, &observed_gen, NULL, NULL) == 0);
    TEST_ASSERT(observed_gen == 2);

    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_ops, &id_new, &entry) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(entry.latest_generation == 200);

    /* Clean Shutdown */
    keystoned_client_disconnect(client);
    keystoned_server_destroy(server);

    printf("    [+] keystoned daemon, security filtering, and atomic publication verified.\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Phase 2: Security-Aware Service Mode Tests (keystoned)\n");
    printf("=================================================================\n");

    test_service_lifecycle_and_security();

    printf("=================================================================\n");
    printf("  ALL PHASE 2 KEYSTONED SERVICE TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
