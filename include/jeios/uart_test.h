/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_UART_TEST_H
#define JEIOS_UART_TEST_H

#include <stdint.h>

/*
 * TEST_MODE=15 coordination values. These are fixture controls and progress
 * records, not a service ABI. The kernel writes UART_MODE_VADDR after observing
 * the supervisor's command word; the UART service never treats a user register
 * as a mode selector.
 */
#define JE_SVC_UART_MODE_VADDR 0x40010000ull
#define JE_SVC_UART_MODE_NONE  0u
#define JE_SVC_UART_MODE_HANG  0x48414e47u /* "HANG" */

#define JE_UART_CLIENT_NORMAL   0u
#define JE_UART_CLIENT_HARDENED 1u
#define JE_SVC_UART_SERVICE_ID  0u

#define JE_UART_PROGRESS_I1       0x49310001u
#define JE_UART_PROGRESS_I2       0x49320002u
#define JE_UART_PROGRESS_D_SENT   0x44520003u
#define JE_UART_PROGRESS_R_SENT   0x52520004u
#define JE_UART_PROGRESS_DONE     0x444f0005u
#define JE_UART_COMMAND_HANG      JE_SVC_UART_MODE_HANG
#define JE_UART_COMMAND_DETACH    0x44455443u /* "DETC", supervisor -> service */
#define JE_UART_COMMAND_REVOKED   0x52564b44u /* "RVKD", supervisor -> client */

#endif /* JEIOS_UART_TEST_H */
