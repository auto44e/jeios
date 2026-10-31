/* SPDX-License-Identifier: Apache-2.0 */
/* Hosted unit tests for the M3a #5 pure syscall decoder. */
#include <jeios/syscall.h>
#include <assert.h>
#include <stdio.h>

static je_syscall_action dec(uint64_t op, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5,
                             je_abi_status *st, uint64_t *res)
{
    uint64_t args[6] = { a0,a1,a2,a3,a4,a5 };
    return je_syscall_decode(op, args, st, res);
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
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0x20u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* SPAWN: PR-C, unknown now */
    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_KILL|0x20u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* known+unknown */
    for (uint32_t bit = 0x20u; bit != 0u; bit <<= 1)   /* every bit above KILL */
        assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, bit,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);

    /* proc_status (op 15, ADR 0010 D4): h=RDI (must fit 32 bits), out=RSI,
     * RDX..R9 reserved (0). syscall.h does not yet name a PROC_STATUS action
     * (frozen header for this worker), so a valid decode is pinned as "OK and a
     * real action, distinct from every existing one"; tighten to the named action
     * once the enum gains it. The out pointer itself is checked by the glue's
     * copy-out, not by decode. */
    {
        je_syscall_action a = dec(JE_SYS_PROC_STATUS, 0x00010000, 0x40002000,0,0,0,0,&st,&res);
        assert(st==JE_ABI_OK && res==0);
        assert(a!=JE_SYSCALL_RETURN && a!=JE_SYSCALL_YIELD && a!=JE_SYSCALL_EXIT
               && a!=JE_SYSCALL_COPY_PROBE && a!=JE_SYSCALL_HANDLE_SELF
               && a!=JE_SYSCALL_HANDLE_CLOSE && a!=JE_SYSCALL_HANDLE_QUERY
               && a!=JE_SYSCALL_HANDLE_RIGHTS && a!=JE_SYSCALL_HANDLE_DUP
               && a!=JE_SYSCALL_ENDPOINT_CREATE && a!=JE_SYSCALL_TRY_SEND
               && a!=JE_SYSCALL_TRY_RECV);
        /* the largest 32-bit handle is not wide */
        assert(dec(JE_SYS_PROC_STATUS, UINT64_C(0xffffffff), 0x40002000,0,0,0,0,&st,&res)==a && st==JE_ABI_OK);
    }
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

    /* Ops reserved by ADR 0010 for later PRs stay UNSUPPORTED (13 CLOCK_TICKS,
     * 14 PROC_SPAWN, 16 PROC_KILL, 17-19 DEV_*), clean or dirty args alike. */
    {
        static const uint64_t later_ops[] = { 13u, 14u, 16u, 17u, 18u, 19u, 20u };
        for (unsigned i = 0; i < sizeof later_ops / sizeof later_ops[0]; ++i) {
            assert(dec(later_ops[i], 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
            assert(dec(later_ops[i], 0x00010000,0x40002000,1,2,3,4,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
        }
    }
    /* op 15 must not alias across the 64-bit op field */
    assert(dec(UINT64_C(0x100000000) | JE_SYS_PROC_STATUS, 0x00010000,0x40002000,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);

    printf("syscall: PASS (abi_query, yield, exit, copy_probe, handle self/close/query/rights/dup, "
           "endpoint_create, try_send/try_recv decode, dup KILL known, proc_status decode, "
           "reserved-arg, wide-handle, unknown-rights, full-width op, unsupported 13/14/16-20)\n");
    return 0;
}
