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

    /* Unknown op -> UNSUPPORTED regardless of args, result 0. */
    assert(dec(3, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    assert(dec(0xdeadbeef, 1,2,3,4,5,6,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    /* Full 64-bit op decode: 0x1_0000_0000 must NOT alias op 0 (abi_query). */
    assert(dec(UINT64_C(0x100000000), 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    /* op 3 (copy_probe) is reserved for step (b): UNSUPPORTED now, no success. */
    assert(dec(3, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);

    printf("syscall: PASS (abi_query, yield, exit, reserved-arg, full-width op, unsupported)\n");
    return 0;
}
