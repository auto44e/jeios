/* SPDX-License-Identifier: Apache-2.0 */
/* Hosted unit tests for the M3a #4 step-3 user-frame validator/sanitizer. */
#include <jeios/uframe.h>
#include <jeios/abi.h>   /* JE_ABI_STARTUP_V1 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BASE  UINT64_C(0x40000000)
#define SIZE  UINT64_C(0x00200000)
#define TOP   (BASE + SIZE)

static je_trap_frame good(void)
{
    je_trap_frame f = {0};
    f.cs = JE_UFRAME_USER_CS; f.ss = JE_UFRAME_USER_SS;
    f.rip = BASE; f.rsp = TOP; f.rflags = 0x202u;
    return f;
}

int main(void)
{
    uint64_t out = 0;

    /* Baseline valid frame: entry at base, stack at top (full-descending). */
    je_trap_frame f = good();
    assert(je_uframe_check(&f, BASE, SIZE, &out));
    assert(out == ((UINT64_C(1)<<9) | (UINT64_C(1)<<1))); /* IF | reserved-1 only */

    /* RIP bounds: base ok, base-1 reject, top-1 ok, top reject. */
    f = good(); f.rip = BASE;        assert(je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rip = BASE - 1;    assert(!je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rip = TOP - 1;     assert(je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rip = TOP;         assert(!je_uframe_check(&f, BASE, SIZE, &out));

    /* RSP bounds: top ok (inclusive), top+1 reject, base reject, base+1 ok. */
    f = good(); f.rsp = TOP;         assert(je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rsp = TOP + 1;     assert(!je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rsp = BASE;        assert(!je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rsp = BASE + 1;    assert(je_uframe_check(&f, BASE, SIZE, &out));

    /* Wrong selectors reject (DPL0 or wrong RPL). */
    f = good(); f.cs = 0x08;         assert(!je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.cs = 0x30;         assert(!je_uframe_check(&f, BASE, SIZE, &out)); /* RPL0 */
    f = good(); f.ss = 0x10;         assert(!je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.ss = 0x28;         assert(!je_uframe_check(&f, BASE, SIZE, &out)); /* RPL0 */

    /* Non-canonical RIP/RSP reject. */
    f = good(); f.rip = UINT64_C(0x0000800000000000); assert(!je_uframe_check(&f, BASE, SIZE, &out));
    f = good(); f.rsp = UINT64_C(0xdead000000000000); assert(!je_uframe_check(&f, BASE, SIZE, &out));

    /* RFLAGS sanitize: IF forced on even if user cleared it; IOPL/NT/TF/VM/AC/
     * ID/RF/DF forced off; arithmetic flags preserved. */
    f = good(); f.rflags = 0; /* user tried IF=0 */
    assert(je_uframe_check(&f, BASE, SIZE, &out));
    assert(out & (UINT64_C(1)<<9));            /* IF forced on */
    f = good();
    f.rflags = (UINT64_C(3)<<12) /*IOPL=3*/ | (UINT64_C(1)<<14) /*NT*/ |
               (UINT64_C(1)<<8) /*TF*/ | (UINT64_C(1)<<18) /*AC*/ |
               (UINT64_C(1)<<21) /*ID*/ | (UINT64_C(1)<<16) /*RF*/ |
               (UINT64_C(1)<<10) /*DF*/ |
               (UINT64_C(1)<<0) /*CF, keep*/ | (UINT64_C(1)<<11) /*OF, keep*/;
    assert(je_uframe_check(&f, BASE, SIZE, &out));
    assert(!(out & (UINT64_C(3)<<12)));        /* IOPL cleared */
    assert(!(out & (UINT64_C(1)<<14)));        /* NT cleared */
    assert(!(out & (UINT64_C(1)<<8)));         /* TF cleared */
    assert(!(out & (UINT64_C(1)<<18)));        /* AC cleared */
    assert(!(out & (UINT64_C(1)<<21)));        /* ID cleared */
    assert(!(out & (UINT64_C(1)<<16)));        /* RF cleared */
    assert(!(out & (UINT64_C(1)<<10)));        /* DF cleared */
    assert(out & (UINT64_C(1)<<0));            /* CF preserved */
    assert(out & (UINT64_C(1)<<11));           /* OF preserved */

    /* Degenerate args reject. */
    f = good(); assert(!je_uframe_check(&f, BASE, 0, &out));         /* size 0 */
    f = good(); assert(!je_uframe_check(NULL, BASE, SIZE, &out));    /* null frame */
    f = good(); assert(!je_uframe_check(&f, BASE, SIZE, NULL));      /* null out */

    /* ADR 0010 D5 startup register ABI v1: the spawned thread's first frame has
     * nonzero RDI/RSI/RDX/RCX/R8/R9 (version, count, handles). je_uframe_check
     * must accept such a frame, must not touch the argument registers (it takes
     * a const frame; checked here byte-for-byte), and must still force IF=1 and
     * IOPL=0 whatever RFLAGS the frame carries. */
    {
        static const uint64_t rfl_in[] = {
            0x202u,                                   /* the kernel-built value */
            0u,                                       /* IF clear */
            (UINT64_C(3)<<12),                        /* IOPL=3, IF clear */
            (UINT64_C(3)<<12) | (UINT64_C(1)<<9) | 0x2u,
            UINT64_C(0xffffffffffffffff),             /* every bit set */
        };
        for (unsigned i = 0; i < sizeof rfl_in / sizeof rfl_in[0]; ++i) {
            f = good();
            f.rdi = JE_ABI_STARTUP_V1;           /* version */
            f.rsi = 4u;                               /* n grants */
            f.rdx = UINT64_C(0x00010003);             /* minted handles */
            f.rcx = UINT64_C(0x00010004);
            f.r8  = UINT64_C(0x00020005);
            f.r9  = UINT64_C(0xffffffff);             /* largest zero-extended handle */
            f.rflags = rfl_in[i];
            je_trap_frame before = f;
            out = 0;
            assert(je_uframe_check(&f, BASE, SIZE, &out));
            assert(memcmp(&before, &f, sizeof f) == 0);    /* frame untouched */
            assert(f.rdi == JE_ABI_STARTUP_V1 && f.rsi == 4u
                   && f.rdx == UINT64_C(0x00010003) && f.rcx == UINT64_C(0x00010004)
                   && f.r8 == UINT64_C(0x00020005) && f.r9 == UINT64_C(0xffffffff));
            assert(out & (UINT64_C(1)<<9));            /* IF forced on */
            assert(!(out & (UINT64_C(3)<<12)));        /* IOPL forced 0 */
        }
        /* Arbitrary (even all-ones) values in every GPR do not affect validity:
         * only cs/ss/rip/rsp/rflags are inspected. */
        f = good();
        f.r15 = f.r14 = f.r13 = f.r12 = f.r11 = f.r10 = f.r9 = f.r8 = UINT64_C(0xffffffffffffffff);
        f.rdi = f.rsi = f.rbp = f.rdx = f.rcx = f.rbx = f.rax = UINT64_C(0xffffffffffffffff);
        out = 0;
        assert(je_uframe_check(&f, BASE, SIZE, &out));
        assert(out == ((UINT64_C(1)<<9) | (UINT64_C(1)<<1)));
        /* ...and nonzero argument registers do not rescue an invalid frame. */
        f.rip = TOP;
        assert(!je_uframe_check(&f, BASE, SIZE, &out));
    }

    printf("uframe: PASS (bounds, selectors, canonical, rflags sanitize, startup-v1 GPRs)\n");
    return 0;
}
