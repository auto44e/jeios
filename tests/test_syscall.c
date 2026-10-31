/* SPDX-License-Identifier: Apache-2.0 */
/* Hosted unit tests for the M3a #5 pure syscall decoder. */
#include <jeios/syscall.h>
#include <jeios/endpoint.h>   /* JE_EP_CHILD_QUOTA_MAX (op 14 decode bound) */
#include <jeios/vm.h>         /* JE_VM_HANDLE_QUOTA_MAX / _DEFAULT (op 14 decode bound) */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static je_syscall_action dec(uint64_t op, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5,
                             je_abi_status *st, uint64_t *res)
{
    uint64_t args[6] = { a0,a1,a2,a3,a4,a5 };
    return je_syscall_decode(op, args, st, res);
}

/* ADR 0010 D5 op 14 decode. Argument order is the frozen ABI register order:
 * a0=RDI launcher, a1=RSI image_id, a2=RDX grants, a3=R10 grant_count,
 * a4=R8 quotas, a5=R9 reserved. */
#define SP_H     UINT64_C(0x00010000)   /* a well-formed 32-bit launcher handle */
#define SP_GPTR  UINT64_C(0x40002000)   /* a nonzero grants pointer */

static void spawn_ok(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5)
{
    je_abi_status st = JE_ABI_INVALID; uint64_t res = 0;
    assert(dec(JE_SYS_PROC_SPAWN, a0,a1,a2,a3,a4,a5,&st,&res)==JE_SYSCALL_PROC_SPAWN);
    assert(st==JE_ABI_OK);
}

static void spawn_invalid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5)
{
    je_abi_status st = JE_ABI_OK; uint64_t res = 0xdeadu;
    assert(dec(JE_SYS_PROC_SPAWN, a0,a1,a2,a3,a4,a5,&st,&res)==JE_SYSCALL_RETURN);
    assert(st==JE_ABI_INVALID && res==0);
}

/* abi.h constants, quota-pack macros and the grant layout (ADR 0010 D3/D5). */
static void test_spawn_abi_constants(void)
{
    assert(JE_ABI_OBJ_LAUNCHER == 4u);
    assert(JE_ABI_RIGHT_SPAWN == 0x20u);
    assert((JE_ABI_RIGHTS_KNOWN & JE_ABI_RIGHT_SPAWN) != 0u);
    /* SPAWN is unique across types (D3): no overlap with any other known right. */
    assert((JE_ABI_RIGHT_SPAWN & (JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV
                                  | JE_ABI_RIGHT_TRANSFER | JE_ABI_RIGHT_KILL)) == 0u);
    assert(JE_ABI_SPAWN_GRANTS_MAX == 4u);
    assert(JE_ABI_STARTUP_V1 == 1u);
    assert(JE_VM_HANDLE_QUOTA_DEFAULT == 15u);
    assert(JE_EP_CHILD_QUOTA_MAX == 0u);   /* M4: any nonzero endpoint quota is INVALID */

    /* Grant layout: {u32 handle @0, u32 rights @4}, 8 bytes, no padding, LE. */
    assert(sizeof(je_abi_spawn_grant) == 8u);
    assert(offsetof(je_abi_spawn_grant, handle) == 0u);
    assert(offsetof(je_abi_spawn_grant, rights) == 4u);
    {
        static const unsigned char wire[8] = { 0x01,0x00,0x02,0x00, 0x0a,0x00,0x00,0x00 };
        je_abi_spawn_grant g;
        memcpy(&g, wire, sizeof g);
        assert(g.handle == 0x00020001u && g.rights == 0x0au);
    }
    {
        je_abi_spawn_grant arr[JE_ABI_SPAWN_GRANTS_MAX];
        assert(sizeof arr == 32u);          /* grant_count x 8 bytes, no stride padding */
    }

    /* Quota pack/unpack round-trip, including both 16-bit field extremes. */
    {
        static const uint32_t hv[] = { 0u, 1u, 15u, 39u, 40u, 0xfffeu, 0xffffu };
        static const uint32_t ev[] = { 0u, 1u, 0x7fffu, 0xffffu };
        for (unsigned i = 0; i < sizeof hv / sizeof hv[0]; ++i)
            for (unsigned j = 0; j < sizeof ev / sizeof ev[0]; ++j) {
                uint64_t q = JE_ABI_SPAWN_QUOTAS(hv[i], ev[j]);
                assert((q >> 32) == 0u);
                assert(JE_ABI_SPAWN_QUOTA_HANDLES(q) == hv[i]);
                assert(JE_ABI_SPAWN_QUOTA_EPS(q) == ev[j]);
            }
    }
    /* The unpack macros only look at [31:0]: bits [63:32] are the decoder's job. */
    assert(JE_ABI_SPAWN_QUOTA_HANDLES(UINT64_C(0xffffffff00000005)) == 5u);
    assert(JE_ABI_SPAWN_QUOTA_EPS(UINT64_C(0xffffffff00070005)) == 7u);
    assert(JE_ABI_SPAWN_QUOTAS(0u, 0u) == 0u);
    assert(JE_ABI_SPAWN_QUOTAS(1u, 2u) == UINT64_C(0x00020001));
}

static void test_clock_ticks_decode(void)
{
    je_abi_status st; uint64_t res;
    /* ADR 0010 D6: no args, no authority -> the CLOCK_TICKS action, OK. The tick
     * value itself is written by the glue (not hostable). */
    assert(dec(JE_SYS_CLOCK_TICKS, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_CLOCK_TICKS && st==JE_ABI_OK);
    /* every argument register reserved: low bit and top bit, one at a time */
    for (unsigned k = 0; k < 6u; ++k) {
        static const uint64_t vals[] = { 1u, UINT64_C(0x8000000000000000), UINT64_C(0xffffffffffffffff) };
        for (unsigned v = 0; v < 3u; ++v) {
            uint64_t a[6] = { 0,0,0,0,0,0 };
            a[k] = vals[v];
            res = 0xdeadu;
            assert(je_syscall_decode(JE_SYS_CLOCK_TICKS, a, &st, &res)==JE_SYSCALL_RETURN);
            assert(st==JE_ABI_INVALID && res==0);
        }
    }
    /* all args dirty */
    assert(dec(JE_SYS_CLOCK_TICKS, 1,2,3,4,5,6,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    /* no 64-bit op aliasing */
    assert(dec(UINT64_C(0x100000000) | JE_SYS_CLOCK_TICKS, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
}

static void test_proc_spawn_decode(void)
{
    je_abi_status st; uint64_t res;
    const uint64_t QMAX = JE_ABI_SPAWN_QUOTAS(JE_VM_HANDLE_QUOTA_MAX, 0u);

    /* --- boundary OKs --- */
    spawn_ok(SP_H, 0, 0, 0, 0, 0);                         /* no grants, default quotas */
    for (uint64_t n = 1; n <= JE_ABI_SPAWN_GRANTS_MAX; ++n)
        spawn_ok(SP_H, 0, SP_GPTR, n, 0, 0);               /* 1..4 grants, ptr present */
    spawn_ok(SP_H, 0, SP_GPTR, JE_ABI_SPAWN_GRANTS_MAX, QMAX, 0);  /* count 4 + quota MAX */
    spawn_ok(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(1u, 0u), 0);       /* handle quota 1 */
    spawn_ok(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(JE_VM_HANDLE_QUOTA_DEFAULT, 0u), 0);
    spawn_ok(SP_H, 0, 0, 0, QMAX, 0);                      /* handle quota MAX (39) */
    spawn_ok(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(0u, JE_EP_CHILD_QUOTA_MAX), 0); /* eps = MAX (0) */
    /* abi.h op 14 (include/jeios/abi.h:154-157): the decode reject list is
     * EXHAUSTIVE; launcher validity, image_id validity and the grants pointer are
     * glue/spawn checks, not decode. So these must DECODE OK.
     * RDI and RSI at the largest 32-bit values are not wide. An unknown image_id
     * and a bad/forged launcher handle are glue/spawn-level, not decode. */
    spawn_ok(UINT64_C(0xffffffff), 0, 0, 0, 0, 0);
    spawn_ok(SP_H, UINT64_C(0xffffffff), 0, 0, 0, 0);
    spawn_ok(0, 0, 0, 0, 0, 0);                             /* handle 0 -> glue BADINDEX */
    /* the grants pointer is checked by the glue's user copy, not by decode */
    spawn_ok(SP_H, 0, UINT64_C(0xffffffffffffffff), 1, 0, 0);
    /* spawn.h step 3 pins "n > effective child handle quota -> INVALID". That is a
     * spawn (kernel) check, not decode: it is absent from the exhaustive abi.h
     * reject list. n=2 with handle quota 1, and n=4 (GRANTS_MAX) with quota 1, must
     * still DECODE OK. The spawn-level INVALID is proven by the QEMU fixture, not
     * by this hosted test. */
    spawn_ok(SP_H, 0, SP_GPTR, 2, JE_ABI_SPAWN_QUOTAS(1u, 0u), 0);
    spawn_ok(SP_H, 0, SP_GPTR, JE_ABI_SPAWN_GRANTS_MAX, JE_ABI_SPAWN_QUOTAS(1u, 0u), 0);

    /* --- every pinned INVALID rule (abi.h op 14) --- */
    /* wide RDI (launcher) */
    spawn_invalid(UINT64_C(0x100000000), 0, 0, 0, 0, 0);
    spawn_invalid(UINT64_C(0x8000000000010000), 0, 0, 0, 0, 0);
    spawn_invalid(UINT64_C(0xffffffffffffffff), 0, 0, 0, 0, 0);
    /* wide RSI (image_id) */
    spawn_invalid(SP_H, UINT64_C(0x100000000), 0, 0, 0, 0);
    spawn_invalid(SP_H, UINT64_C(0x8000000000000000), 0, 0, 0, 0);
    /* grant_count > JE_ABI_SPAWN_GRANTS_MAX (with a pointer, so only count is wrong) */
    spawn_invalid(SP_H, 0, SP_GPTR, JE_ABI_SPAWN_GRANTS_MAX + 1u, 0, 0);
    spawn_invalid(SP_H, 0, SP_GPTR, 0xffu, 0, 0);
    spawn_invalid(SP_H, 0, SP_GPTR, UINT64_C(0x100000000), 0, 0);       /* wide count, low bits 0 */
    spawn_invalid(SP_H, 0, SP_GPTR, UINT64_C(0x100000001), 0, 0);       /* must not narrow to 1 */
    spawn_invalid(SP_H, 0, SP_GPTR, UINT64_C(0xffffffffffffffff), 0, 0);
    /* RDX/count pairing: RDX != 0 with count 0; RDX == 0 with count > 0 */
    spawn_invalid(SP_H, 0, SP_GPTR, 0, 0, 0);
    for (uint64_t n = 1; n <= JE_ABI_SPAWN_GRANTS_MAX; ++n)
        spawn_invalid(SP_H, 0, 0, n, 0, 0);
    /* quotas [63:32] != 0 (low half otherwise valid) */
    spawn_invalid(SP_H, 0, 0, 0, UINT64_C(0x100000000), 0);
    spawn_invalid(SP_H, 0, 0, 0, UINT64_C(0x8000000000000000), 0);
    spawn_invalid(SP_H, 0, 0, 0, UINT64_C(0xffffffff00000000) | JE_ABI_SPAWN_QUOTAS(1u, 0u), 0);
    /* handle quota > JE_VM_HANDLE_QUOTA_MAX */
    spawn_invalid(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(JE_VM_HANDLE_QUOTA_MAX + 1u, 0u), 0);
    spawn_invalid(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(JE_VM_HANDLE_CAP, 0u), 0);
    spawn_invalid(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(0xffffu, 0u), 0);
    /* endpoint quota > JE_EP_CHILD_QUOTA_MAX (= 0 in M4, so 1 is already INVALID) */
    spawn_invalid(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(0u, JE_EP_CHILD_QUOTA_MAX + 1u), 0);
    spawn_invalid(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(1u, 1u), 0);
    spawn_invalid(SP_H, 0, 0, 0, JE_ABI_SPAWN_QUOTAS(0u, 0xffffu), 0);
    /* R9 reserved */
    spawn_invalid(SP_H, 0, 0, 0, 0, 1);
    spawn_invalid(SP_H, 0, 0, 0, 0, UINT64_C(0x8000000000000000));
    spawn_invalid(SP_H, 0, SP_GPTR, 1, QMAX, 1);            /* otherwise-valid call + R9 */
    /* several rules broken at once: still INVALID, never the spawn action */
    spawn_invalid(UINT64_C(0x100000000), UINT64_C(0x100000000), 0, 5, UINT64_C(0x100000000), 1);

    /* no 64-bit op aliasing */
    assert(dec(UINT64_C(0x100000000) | JE_SYS_PROC_SPAWN, SP_H,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
}

/* ADR 0010 D8 op 16 decode (abi.h JE_SYS_PROC_KILL): h=RDI must fit 32 bits;
 * RSI..R9 must all be 0; RDX = 0 on return. A clean call decodes to exactly
 * JE_SYSCALL_PROC_KILL (syscall.h) with OK. Handle resolution (BADINDEX/STALE/
 * WRONGTYPE/DENIED) and the je_sched_kill outcome (OK/INVALID) are glue/kernel
 * decisions, not decode: they are proven by the QEMU fixture, not here. */
static void kill_ok(uint64_t h)
{
    je_abi_status st = JE_ABI_INVALID; uint64_t res = 0xdeadu;
    assert(dec(JE_SYS_PROC_KILL, h,0,0,0,0,0,&st,&res)==JE_SYSCALL_PROC_KILL);
    assert(st==JE_ABI_OK && res==0);
}

static void kill_invalid(const uint64_t a[6])
{
    je_abi_status st = JE_ABI_OK; uint64_t res = 0xdeadu;
    assert(je_syscall_decode(JE_SYS_PROC_KILL, a, &st, &res)==JE_SYSCALL_RETURN);
    assert(st==JE_ABI_INVALID && res==0);
}

static void test_proc_kill_decode(void)
{
    je_abi_status st; uint64_t res;
    assert(JE_SYS_PROC_KILL == 16u && JE_SYSCALL_PROC_KILL == 16);

    /* --- OK: any 32-bit handle value. Handle 0, a SELF-looking handle and the
     * largest 32-bit value are all well-formed; their rejection is the glue's. */
    kill_ok(UINT64_C(0x00010000));
    kill_ok(0u);
    kill_ok(1u);
    kill_ok(UINT64_C(0x7fffffff));
    kill_ok(UINT64_C(0x80000000));
    kill_ok(UINT64_C(0xffffffff));

    /* --- wide RDI: each bit of [63:32] alone, plus combined patterns --- */
    for (unsigned b = 32u; b < 64u; ++b) {
        uint64_t a[6] = { (UINT64_C(1) << b) | UINT64_C(0x00010000), 0,0,0,0,0 };
        kill_invalid(a);
        a[0] = UINT64_C(1) << b;              /* low half 0: must not narrow to 0 */
        kill_invalid(a);
    }
    {
        static const uint64_t wide[] = { UINT64_C(0x100000000), UINT64_C(0x8000000000010000),
                                         UINT64_C(0xffffffff00000000), UINT64_C(0xffffffffffffffff) };
        for (unsigned i = 0; i < sizeof wide / sizeof wide[0]; ++i) {
            uint64_t a[6] = { wide[i], 0,0,0,0,0 };
            kill_invalid(a);
        }
    }

    /* --- every reserved register RSI..R9 (a1..a5), one at a time: lowest bit,
     * top bit, all ones, and a plausible pointer --- */
    {
        static const uint64_t vals[] = { 1u, UINT64_C(0x8000000000000000),
                                         UINT64_C(0xffffffffffffffff), UINT64_C(0x40002000) };
        for (unsigned k = 1u; k < 6u; ++k)
            for (unsigned v = 0; v < sizeof vals / sizeof vals[0]; ++v)
                for (unsigned hb = 0; hb < 2u; ++hb) {
                    uint64_t a[6] = { hb ? UINT64_C(0xffffffff) : UINT64_C(0x00010000), 0,0,0,0,0 };
                    a[k] = vals[v];
                    kill_invalid(a);
                }
        /* every single bit of every reserved register */
        for (unsigned k = 1u; k < 6u; ++k)
            for (unsigned b = 0; b < 64u; ++b) {
                uint64_t a[6] = { UINT64_C(0x00010000), 0,0,0,0,0 };
                a[k] = UINT64_C(1) << b;
                kill_invalid(a);
            }
    }
    /* several rules broken at once: still INVALID, never the kill action */
    {
        uint64_t a[6] = { UINT64_C(0x100000000), 1,2,3,4,5 };
        kill_invalid(a);
        uint64_t b[6] = { UINT64_C(0x00010000), 1,2,3,4,5 };
        kill_invalid(b);
    }

    /* no 64-bit op aliasing onto 16 */
    assert(dec(UINT64_C(0x100000000) | JE_SYS_PROC_KILL, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    assert(dec(UINT64_C(0xffffffff00000010), 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    /* the neighbours keep their own actions: op 15 is not routed to kill, and op 16
     * with a PROC_STATUS-shaped call (RSI = out ptr) is INVALID, not a status */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000,0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_PROC_STATUS && st==JE_ABI_OK);
    assert(dec(JE_SYS_PROC_KILL, 0x00010000,0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
}

int main(void)
{
    je_abi_status st; uint64_t res;

    /* abi_query(version) -> OK, RDX = version. */
    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_VERSION,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN);
    assert(st==JE_ABI_OK && res==JE_ABI_VERSION);
    /* abi_query(profile) -> OK, RDX = profile. */
    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_PROFILE,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN);
    assert(st==JE_ABI_OK && res==JE_ABI_PROFILE);
    /* abi_query with an unknown selector -> INVALID, result 0. */
    assert(dec(JE_SYS_ABI_QUERY, 99,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    /* abi_query with a nonzero reserved arg -> INVALID, result 0 (no RDX leak). */
    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_VERSION, 1,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_VERSION, 0,0,0,0,7,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);

    /* yield() with clean args -> YIELD/OK. */
    assert(dec(JE_SYS_YIELD, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_YIELD && st==JE_ABI_OK && res==0);
    /* yield() with any nonzero arg -> INVALID, and NOT a yield action. */
    assert(dec(JE_SYS_YIELD, 1,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);

    /* exit(code) -> EXIT, result carries the code. */
    assert(dec(JE_SYS_EXIT, 42,0,0,0,0,0,&st,&res)==JE_SYSCALL_EXIT && st==JE_ABI_OK && res==42);
    /* exit with a nonzero reserved arg -> INVALID, and must NOT exit. */
    assert(dec(JE_SYS_EXIT, 42, 1,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);

    /* copy_probe (op 3): src/dst/len in RDI/RSI/RDX; R10/R8/R9 reserved. The
     * pure decoder returns the COPY_PROBE action (the glue does the real copy),
     * and rejects nonzero reserved args as INVALID without a copy action. */
    assert(dec(JE_SYS_COPY_PROBE, 0x40000000,0x40000040,16,0,0,0,&st,&res)==JE_SYSCALL_COPY_PROBE && st==JE_ABI_OK);
    assert(dec(JE_SYS_COPY_PROBE, 0,0,0, 1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* r10 reserved */
    assert(dec(JE_SYS_COPY_PROBE, 0,0,0, 0,0,9,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* r9 reserved */

    /* Unknown op -> UNSUPPORTED regardless of args, result 0. */
    assert(dec(0xdeadbeef, 1,2,3,4,5,6,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    /* Full 64-bit op decode: 0x1_0000_0000 must NOT alias op 0 (abi_query). */
    assert(dec(UINT64_C(0x100000000), 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    /* op 4 is the dev-only copy_fixup_inject, guarded out of this hosted build:
     * UNSUPPORTED here, no success. */
    assert(dec(4, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);

    /* M3b handle ops (5/6/7). The pure decoder validates reserved args and the
     * wide-handle rule; the glue does the table work. */
    /* handle_self(): clean -> SELF action, OK. */
    assert(dec(JE_SYS_HANDLE_SELF, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_SELF && st==JE_ABI_OK);
    /* handle_self() with ANY nonzero arg (it takes none) -> INVALID, not an action. */
    assert(dec(JE_SYS_HANDLE_SELF, 1,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(dec(JE_SYS_HANDLE_SELF, 0,0,0,0,0,9,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* handle_close(h=RDI): clean -> CLOSE action, OK. */
    assert(dec(JE_SYS_HANDLE_CLOSE, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_CLOSE && st==JE_ABI_OK);
    /* handle_close with a reserved arg -> INVALID. */
    assert(dec(JE_SYS_HANDLE_CLOSE, 0x00010000,1,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* handle_close with a WIDE handle (bits 63:32 set) -> INVALID before narrowing. */
    assert(dec(JE_SYS_HANDLE_CLOSE, UINT64_C(0x100000000),0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* handle_query(h=RDI): clean -> QUERY action; reserved + wide-handle rejected. */
    assert(dec(JE_SYS_HANDLE_QUERY, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_QUERY && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_QUERY, 0x00010000,0,1,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(dec(JE_SYS_HANDLE_QUERY, UINT64_C(0x500000000),0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* handle_rights(h=RDI): clean -> RIGHTS action; reserved + wide-handle rejected. */
    assert(dec(JE_SYS_HANDLE_RIGHTS, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_RIGHTS && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_RIGHTS, 0x00010000,0,0,1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(dec(JE_SYS_HANDLE_RIGHTS, UINT64_C(0x100000000),0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* handle_dup(h=RDI, new_rights=RSI): clean subset -> DUP action, OK. */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_INSPECT,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK); /* drop all rights */
    /* reserved RDX/R10/R8/R9 nonzero -> INVALID */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0, 1,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0, 0,1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* wide handle or wide rights -> INVALID */
    assert(dec(JE_SYS_HANDLE_DUP, UINT64_C(0x100000000), 0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, UINT64_C(0x100000000),0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* Rights-bit decode contract. HISTORY (M3b): while 0x2 was an UNKNOWN bit,
     * dup(self, 0x2) returned INVALID (malformed). M3c (ADR 0009 D4a) appends
     * SEND=0x2 to JE_ABI_RIGHTS_KNOWN, so 0x2 is now a KNOWN right a SELF handle
     * does not hold -> the decode passes and the glue returns DENIED. We assert
     * the new DENIED result for 0x2, and re-prove the unknown-bit->INVALID
     * contract on a bit that is still unknown (0x80000000). See
     * docs/IMPLEMENTATION.md "M3c rights-bit discrepancy" for the rationale. */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_SEND,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    /* ^ 0x2 is now a well-formed (known) rights request: decode accepts it and
     * returns the DUP action; the known-but-unheld -> DENIED decision happens in
     * the glue (handle_dup_self subset check), not the decoder. The old
     * dup(self,0x2)==INVALID assertion is intentionally retired; its unknown-bit
     * INTENT is preserved by the next line on a genuinely-unknown bit. */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0x80000000u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* endpoint_create (op 10): no args -> ENDPOINT_CREATE action, OK; any
     * reserved arg -> INVALID. (Dispatch/mint is kernel-only; decode is pure.) */
    assert(dec(JE_SYS_ENDPOINT_CREATE, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_ENDPOINT_CREATE && st==JE_ABI_OK);
    assert(dec(JE_SYS_ENDPOINT_CREATE, 1,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(dec(JE_SYS_ENDPOINT_CREATE, 0,0,0,0,0,9,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    /* try_send (op 11): h,ubuf,len -> TRY_SEND; cap_count in R10[7:0] with R8 the
     * cap-array ptr (R8!=0 iff cap_count>0). Reserved R10[63:8]/R9, len>MSG_MAX,
     * cap_count>K, R8/cap_count pairing mismatch, wide handle -> INVALID. */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0,0,0,&st,&res)==JE_SYSCALL_TRY_SEND && st==JE_ABI_OK);
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 1,0x40003000,0,&st,&res)==JE_SYSCALL_TRY_SEND && st==JE_ABI_OK); /* 1 cap + array ptr */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* cap_count=1 but R8=0 */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0,0x40003000,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R8!=0 but cap_count=0 */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 3,0x40003000,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* cap_count > K=2 */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0x100,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R10 reserved bit */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0,0,1,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R9 reserved */
    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 65, 0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* len > MSG_MAX */
    assert(dec(JE_SYS_TRY_SEND, UINT64_C(0x100000000), 0x40002000, 8, 0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* wide handle */
    /* try_recv (op 12): h,ubuf,cap,hbuf=R8,hcap=R9,timeout=R10; R8!=0 iff R9!=0. */
    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0,0,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK);
    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0x40003000,2,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK); /* handle buf + cap */
    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0x40003000,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R8!=0 R9=0 */
    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0,2,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R8=0 R9!=0 */
    /* A6-ABI: R10 is now the timeout (0=poll, nonzero=block). Any value is a legal
     * decode (the bound/overflow check is in the glue, not the decode). */
    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 1,0,0,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK); /* nonzero timeout OK */
    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, UINT64_C(0xffffffffffffffff),0,0,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK); /* huge timeout decodes; glue rejects */
    assert(dec(JE_SYS_TRY_RECV, UINT64_C(0x100000000), 0x40002000, 64, 0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* wide handle */
    /* ADR 0010 D3: KILL (0x10) joins JE_ABI_RIGHTS_KNOWN in PR-B, so a DUP that
     * names it is well-formed (the held-or-not decision, DENIED, is the glue's).
     * Each still-unknown bit above KILL stays a malformed call -> INVALID. */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_KILL,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_INSPECT|JE_ABI_RIGHT_KILL,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHTS_KNOWN,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    /* HISTORY (PR-B): 0x20 was unknown here and dup(self, 0x20) was INVALID. ADR
     * 0010 D5 (PR-C) appends SPAWN=0x20 to JE_ABI_RIGHTS_KNOWN, so 0x20 is now a
     * well-formed request: decode returns the DUP action and the known-but-unheld
     * (or LAUNCHER not-DUP-able) DENIED is the glue's decision. The unknown-bit
     * INVALID contract moves to the next still-unknown bit, 0x40. NOTE: ADR 0010
     * D3 reserves 0x40/0x80/0x100 for DEVICE; the PR that makes them known must
     * move these lines again. */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_SPAWN,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_INSPECT|JE_ABI_RIGHT_SPAWN,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0x40u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* still unknown */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_KILL|0x40u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* known+unknown */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_SPAWN|0x40u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* known+unknown */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHTS_KNOWN|0x80000000u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    for (uint32_t bit = 0x40u; bit != 0u; bit <<= 1)   /* every bit above SPAWN */
        assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, bit,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    assert(JE_ABI_RIGHTS_KNOWN == 0x3fu);   /* exactly INSPECT..SPAWN known in PR-C */

    /* proc_status (op 15, ADR 0010 D4): h=RDI (must fit 32 bits), out=RSI,
     * RDX..R9 reserved (0). A clean call decodes to exactly the named action
     * JE_SYSCALL_PROC_STATUS (syscall.h), not merely "some action": a decoder
     * that routed op 15 to a neighbouring action must fail here. The out
     * pointer itself is checked by the glue's copy-out, not by decode. */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_PROC_STATUS && st==JE_ABI_OK && res==0);
    /* the largest 32-bit handle is not wide */
    assert(dec(JE_SYS_PROC_STATUS, UINT64_C(0xffffffff), 0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_PROC_STATUS && st==JE_ABI_OK && res==0);
    /* wide handle (any of RDI[63:32]) -> INVALID before narrowing, result 0 */
    assert(dec(JE_SYS_PROC_STATUS, UINT64_C(0x100000000), 0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    assert(dec(JE_SYS_PROC_STATUS, UINT64_C(0x8000000000010000), 0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    /* each reserved register nonzero -> INVALID (and never the status action) */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,1,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0); /* RDX */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,0,1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0); /* R10 */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,0,0,1,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0); /* R8 */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,0,0,0,1,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0); /* R9 */
    assert(dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,0,0,0,UINT64_C(0x8000000000000000),&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R9 top bit */
    /* wide handle AND a reserved arg together: still INVALID, never an action */
    assert(dec(JE_SYS_PROC_STATUS, UINT64_C(0x100000000), 0x40002000,1,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);

    /* ADR 0010 PR-C: ops 13 CLOCK_TICKS and 14 PROC_SPAWN are now decoded. */
    test_spawn_abi_constants();
    test_clock_ticks_decode();
    test_proc_spawn_decode();

    test_proc_kill_decode();

    /* Ops reserved by ADR 0010 for later PRs stay UNSUPPORTED (17-19 DEV_*, and 20
     * beyond), clean or dirty args alike. HISTORY: 13 and 14 were in this list
     * until PR-C; 16 PROC_KILL was in it until PR-D (ADR 0010 D8), and its
     * UNSUPPORTED expectation is intentionally retired (see test_proc_kill_decode). */
    {
        static const uint64_t later_ops[] = { 17u, 18u, 19u, 20u };
        for (unsigned i = 0; i < sizeof later_ops / sizeof later_ops[0]; ++i) {
            assert(dec(later_ops[i], 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
            assert(dec(later_ops[i], 0x00010000,0x40002000,1,2,3,4,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
            /* the arg shape that is a valid PROC_KILL must not be one for 17-19 */
            assert(dec(later_ops[i], 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
        }
    }
    /* op 15 must not alias across the 64-bit op field */
    assert(dec(UINT64_C(0x100000000) | JE_SYS_PROC_STATUS, 0x00010000,0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);

    printf("syscall: PASS (abi_query, yield, exit, copy_probe, handle self/close/query/rights/dup, "
           "endpoint_create, try_send/try_recv decode, dup KILL known, proc_status decode, "
           "clock_ticks decode, proc_spawn decode, spawn ABI layout, dup SPAWN known, "
           "proc_kill decode, reserved-arg, wide-handle, unknown-rights, full-width op, "
           "unsupported 17-20)\n");
    return 0;
}
