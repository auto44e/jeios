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
    /* op 9 and up unknown. */
    assert(dec(9, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);

    printf("syscall: PASS (abi_query, yield, exit, copy_probe, handle self/close/query/rights, "
           "reserved-arg, wide-handle, full-width op, unsupported)\n");
    return 0;
}
