#ifndef TEST_DIRECTEYE_QKP_FIXTURE_H
#define TEST_DIRECTEYE_QKP_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct qkp_fixture qkp_fixture_t;
qkp_fixture_t* qkp_fixture_create(const char* root, char* client_keys, size_t client_keys_size,
                                  char* server_public_key, size_t server_public_key_size);
uint16_t qkp_fixture_port(const qkp_fixture_t* fixture);
uint64_t qkp_fixture_mutate(qkp_fixture_t* fixture, const char* operation_id,
                            const char* verb, const char* expected_sequence,
                            const char* payload);
int qkp_fixture_ack_is_durable(qkp_fixture_t* fixture);
void qkp_fixture_destroy(qkp_fixture_t* fixture);

#endif
