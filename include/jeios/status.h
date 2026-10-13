/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_STATUS_H
#define JEIOS_STATUS_H
/* Internal status values, not yet a stable userspace ABI. Never discard errors. */
typedef enum {
    JE_OK = 0, JE_ERR_INVALID, JE_ERR_UNSUPPORTED, JE_ERR_TIMEOUT,
    JE_ERR_NOMEM, JE_ERR_BUSY, JE_ERR_IO, JE_ERR_DENIED
} je_status;
const char *je_status_name(je_status status);
#endif

