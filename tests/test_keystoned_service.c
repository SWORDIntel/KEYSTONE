/*
 * KEYSTONE Security-Aware Service Mode (keystoned) Test Suite
 *
 * Copyright (c) 2025-2026 SWORDIntel Systems. All rights reserved.
 * AGPL-3.0 License.
 */

#include "keystone.h"
#include "keystoned.h"
#include "test_macros.h"
#include <sys/socket.h>
#include <sys/un.h>

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
    /* valid hex: from_string fails closed on non-hex input and would leave the
 * UUID uninitialized (valgrind: uninit bytes serialized onto the wire) */
    TEST_ASSERT(keystone_uuid_from_string("dddddddd-0000-0000-0000-000000000001", &node) == 0);

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


/*
 * Trust-model hardening (security sweep 2026-09-28 #4/#5/#6): tenant
 * isolation, SECURITY_SENSITIVE gating at SECRET, ingest tenant binding,
 * and malformed-frame handling on the raw socket.
 */
static void test_trust_model_hardening(void) {
    printf("[*] Testing trust-model hardening (tenant, SENSITIVE, frames)...\n");

    const char* sock_path = "/tmp/keystone_test_trust.sock";
    unlink(sock_path);

    keystoned_config_t cfg = {
        .socket_path = sock_path,
        .cache_capacity = 64,
        .initial_fencing_epoch = 1,
        .enable_security_audit = 1
    };
    keystoned_server_t* server = keystoned_server_create(&cfg);
    TEST_ASSERT(server != NULL);
    TEST_ASSERT(keystoned_server_start(server) == 0);
    usleep(20000);

    keystoned_client_t* client = keystoned_client_connect(sock_path);
    TEST_ASSERT(client != NULL);
    TEST_ASSERT(keystoned_client_ping(client) == 0);

    keystone_security_context_t ctx_t1_ops    = { .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS,    .compartment_mask = 0 };
    keystone_security_context_t ctx_t1_secret = { .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_SECRET, .compartment_mask = 0 };
    keystone_security_context_t ctx_t2_ops    = { .tenant_id = 2, .classification = KEYSTONE_CLASSIFICATION_OPS,    .compartment_mask = 0 };

    /* 1. Ingest is tenant-bound: a tenant-1 principal cannot feed tenant-2 records */
    keystone_uuid_t id_t2, evt_t2, node;
    TEST_ASSERT(keystone_uuid_from_string("22222222-0000-0000-0000-000000000002", &id_t2) == 0);
    TEST_ASSERT(keystone_uuid_from_string("eeeeeeee-0000-0000-0000-000000000009", &evt_t2) == 0);
    TEST_ASSERT(keystone_uuid_from_string("dddddddd-0000-0000-0000-000000000001", &node) == 0);
    keystone_federation_record_t rec_t2 = {
        .source_object_id = id_t2, .source_event_id = evt_t2, .source_node_id = node,
        .source_generation = 50, .fencing_epoch = 1,
        .source_hlc = { .physical_ms = 5000100, .logical = 0, .node_id = 1 },
        .tenant_id = 2, .classification = KEYSTONE_CLASSIFICATION_OPS,
        .object_type = KEYSTONE_OBJ_VM, .flags = KEYSTONE_RECORD_FLAG_EVENT
    };
    TEST_ASSERT(keystoned_client_ingest(client, &ctx_t1_ops, &rec_t2) == KEYSTONED_STATUS_DENIED);

    /* 2. SECURITY_SENSITIVE records are denied below SECRET, allowed at SECRET */
    keystone_uuid_t id_sens, evt_sens;
    TEST_ASSERT(keystone_uuid_from_string("33333333-0000-0000-0000-000000000003", &id_sens) == 0);
    TEST_ASSERT(keystone_uuid_from_string("eeeeeeee-0000-0000-0000-000000000010", &evt_sens) == 0);
    keystone_federation_record_t rec_sens = {
        .source_object_id = id_sens, .source_event_id = evt_sens, .source_node_id = node,
        .source_generation = 51, .fencing_epoch = 1,
        .source_hlc = { .physical_ms = 5000200, .logical = 0, .node_id = 1 },
        .tenant_id = 1, .classification = KEYSTONE_CLASSIFICATION_OPS,
        .object_type = KEYSTONE_OBJ_VM,
        .flags = KEYSTONE_RECORD_FLAG_EVENT | KEYSTONE_RECORD_FLAG_SECURITY_SENSITIVE
    };
    TEST_ASSERT(keystoned_client_ingest(client, &ctx_t1_ops, &rec_sens) == KEYSTONED_STATUS_OK);

    keystone_exact_entry_t ent;
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_t1_ops, &id_sens, &ent) == KEYSTONED_STATUS_DENIED);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_t1_secret, &id_sens, &ent) == KEYSTONED_STATUS_OK);

    /* 3. Tenant isolation on queries: a tenant-2-tagged object is invisible
     * to tenant-1 principals at any clearance, visible to its own tenant. */
    keystone_federation_record_t rec_t2_ok = rec_t2;
    TEST_ASSERT(keystoned_client_ingest(client, &ctx_t2_ops, &rec_t2_ok) == KEYSTONED_STATUS_OK);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_t1_secret, &id_t2, &ent) == KEYSTONED_STATUS_DENIED);
    TEST_ASSERT(keystoned_client_exact_lookup(client, &ctx_t2_ops, &id_t2, &ent) == KEYSTONED_STATUS_OK);

    /* 4. Malformed frame: a raw connection speaking garbage is closed */
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(fd >= 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
        TEST_ASSERT(connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0);
        const char garbage[8] = "NOTKEYST";
        TEST_ASSERT(send(fd, garbage, sizeof(garbage), 0) == sizeof(garbage));
        char byte = 0;
        ssize_t r = recv(fd, &byte, 1, 0);
        TEST_ASSERT(r <= 0); /* server drops the connection, no reply */
        close(fd);
    }

    keystoned_client_disconnect(client);
    keystoned_server_destroy(server);
    unlink(sock_path);

    printf("    [+] Trust-model hardening verified (tenant-bound ingest, SENSITIVE at SECRET, tenant isolation, malformed frames dropped).\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("  KEYSTONE Phase 2: Security-Aware Service Mode Tests (keystoned)\n");
    printf("=================================================================\n");

    test_service_lifecycle_and_security();
    test_trust_model_hardening();

    printf("=================================================================\n");
    printf("  ALL PHASE 2 KEYSTONED SERVICE TESTS PASSED (100%% GREEN)\n");
    printf("=================================================================\n");
    return 0;
}
