/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/uframe.h>

/* RFLAGS bit positions. */
#define RF_CF   (UINT64_C(1) << 0)
#define RF_ONE  (UINT64_C(1) << 1)   /* reserved, always 1 */
#define RF_PF   (UINT64_C(1) << 2)
#define RF_AF   (UINT64_C(1) << 4)
#define RF_ZF   (UINT64_C(1) << 6)
#define RF_SF   (UINT64_C(1) << 7)
#define RF_TF   (UINT64_C(1) << 8)
#define RF_IF   (UINT64_C(1) << 9)
#define RF_DF   (UINT64_C(1) << 10)
#define RF_OF   (UINT64_C(1) << 11)

/* Arithmetic flags the user may freely keep. Everything else (IOPL, NT, RF, VM,
 * AC, VIF, VIP, ID, TF, DF, reserved) is forced off; IF and reserved-bit1 on.
 * DF is cleared too: the entry ABI gives the user a known direction flag (DF=0),
 * matching the header/ADR D6 "clear unsupported flags" intent. */
#define JE_UFRAME_SAFE (RF_CF | RF_PF | RF_AF | RF_ZF | RF_SF | RF_OF)

static bool canonical(uint64_t a)
{
    /* 48-bit canonical: bits [63:47] all equal. */
    uint64_t hi = a >> 47;
    return hi == 0 || hi == (UINT64_C(0x1ffff));
}

bool je_uframe_check(const je_trap_frame *f, uint64_t base, uint64_t size,
                     uint64_t *out_rflags)
{
    if (!f || !out_rflags || size == 0) return false;
    uint64_t end = base + size;
    if (end < base) return false; /* overflow guard */
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS) return false;
    if (!canonical(f->rip) || !canonical(f->rsp)) return false;
    /* RIP in [base, end); RSP in (base, end] (full-descending stack top). */
    if (f->rip < base || f->rip >= end) return false;
    if (f->rsp <= base || f->rsp > end) return false;
    *out_rflags = (f->rflags & JE_UFRAME_SAFE) | RF_IF | RF_ONE;
    return true;
}
