/* SPDX-License-Identifier: Apache-2.0 */
/* Hosted unit tests for the M3a #4 step-3 user-frame validator/sanitizer. */
#include <jeios/uframe.h>
#include <assert.h>
#include <stdio.h>

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

    printf("uframe: PASS (bounds, selectors, canonical, rflags sanitize)\n");
    return 0;
}
