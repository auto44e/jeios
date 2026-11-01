/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

static const char spoofer_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS spoofer";
static volatile uint32_t spoofer_data __attribute__((used, section(".data")))
    = UINT32_C(0x11223344);

#define JE_SVC_SPOOF_BIT_GUESS 0x1u
#define JE_SVC_SPOOF_BIT_DUP   0x2u
#define JE_SVC_SPOOF_BIT_CAP   0x4u
#define JE_SVC_SPOOF_MASK (JE_SVC_SPOOF_BIT_GUESS | JE_SVC_SPOOF_BIT_DUP \
                           | JE_SVC_SPOOF_BIT_CAP)
/* Handle encoding is the frozen 16/16 ABI encoding. The supervisor's launcher
 * is slot 0/gen 1 and its first endpoint source is slot 1/gen 1. */
#define JE_GUESSED_SUPERVISOR_HANDLE UINT32_C(0x00010001)

static uint64_t spoof_exit(uint32_t mask)
{
    (void)spoofer_rodata;
    (void)spoofer_data;
    uint32_t complement = (~mask) & UINT32_C(0xffff);
    return ((uint64_t)JE_SVC_SPOOF_MAGIC << 32)
           | ((uint64_t)complement << 16) | mask;
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    uint32_t mask = 0u;
    uint64_t value = 0u;
    uint64_t derived = 0u;
    uint64_t cap = (uint64_t)h0 | ((uint64_t)JE_ABI_RIGHT_SEND << 32);
    uint8_t malformed[4] = { 0xefu, 0xbeu, 0xadu, 0xdeu };
    if (version != JE_ABI_STARTUP_V1 || n != 1u
        || h1 != 0u || h2 != 0u || h3 != 0u)
        return (int64_t)spoof_exit(0u);
    if (je_handle_rights(JE_GUESSED_SUPERVISOR_HANDLE, &value)
        == JE_ABI_H_STALE)
        mask |= JE_SVC_SPOOF_BIT_GUESS;
    if (je_handle_dup(h0, JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_TRANSFER, &derived)
        == JE_ABI_H_DENIED)
        mask |= JE_SVC_SPOOF_BIT_DUP;
    if (je_try_send_caps(h0, malformed, sizeof malformed, &cap, 1u)
        == JE_ABI_H_DENIED)
        mask |= JE_SVC_SPOOF_BIT_CAP;
    (void)je_try_send(h0, malformed, sizeof malformed);
    return (int64_t)spoof_exit(mask);
}
