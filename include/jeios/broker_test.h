/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PR-G mode-13 scenario constants shared by the ring-3 images and the kernel
 * corroborator. These are test-protocol values, not a public service ABI.
 */
#ifndef JEIOS_BROKER_TEST_H
#define JEIOS_BROKER_TEST_H

#include <stdint.h>

#define JE_BROKER_TEST_ECHO_REQUEST  0x4f484345u /* "ECHO" */
#define JE_BROKER_TEST_ECHO_RESPONSE 0x52504345u /* "ECPR" */
#define JE_BROKER_TEST_FINISH        0xffffffffu

#define JE_BROKER_TEST_ROLE_A        0u
#define JE_BROKER_TEST_ROLE_B        1u
#define JE_BROKER_TEST_EXERCISED     UINT32_C(0x42585243)

#define JE_BROKER_TEST_BIT_READY       UINT32_C(0x0001)
#define JE_BROKER_TEST_BIT_GRANTS      UINT32_C(0x0002)
#define JE_BROKER_TEST_BIT_ECHO        UINT32_C(0x0004)
#define JE_BROKER_TEST_BIT_NO_TRANSFER UINT32_C(0x0008)
#define JE_BROKER_TEST_BIT_PEER_CLOSED UINT32_C(0x0010)
#define JE_BROKER_TEST_BIT_RESTARTED   UINT32_C(0x0020)
#define JE_BROKER_TEST_BIT_EXHAUSTED   UINT32_C(0x0040)
#define JE_BROKER_TEST_BIT_ROLE_B      UINT32_C(0x0080)

typedef struct {
    uint32_t op;
    uint32_t sequence;
} je_broker_test_echo;

_Static_assert(sizeof(je_broker_test_echo) == 8u, "broker echo size");

#endif /* JEIOS_BROKER_TEST_H */
