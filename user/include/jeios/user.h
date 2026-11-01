/*
 * ADR 0010 D10 ring-3 C runtime contract (JeiOS ABI v0, int 0x80). Freestanding: no
 * libc, no errno, no FPU/SIMD (ADR 0007 D7; compile with -mgeneral-regs-only), no TLS.
 * The stack is 16 KiB, so keep locals small.
 *
 * Calling convention (abi.h): RAX = op in and status out; arguments in RDI, RSI, RDX,
 * R10, R8, R9; the scalar result comes back in RDX. The kernel preserves every other
 * GPR. Each wrapper returns the je_abi_status and, when `out` is non-NULL, stores RDX.
 *
 * Entry contract (startup register ABI v1, abi.h): crt0's _start calls
 *     int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
 *                     uint64_t h2, uint64_t h3);
 * with the startup registers as arguments (they match the SysV order exactly). The
 * return value becomes the EXIT code. crt0 checks, before any call, that every GPR that
 * startup v1 promises to be zero is zero; if not, it executes ud2, so the process dies
 * with reason FAULT.
 */
#ifndef JEIOS_USER_H
#define JEIOS_USER_H

#include <stdint.h>
#include <jeios/abi.h>

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3);

static inline uint64_t je_sys6(uint64_t op, uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5, uint64_t *out)
{
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8  __asm__("r8")  = a4;
    register uint64_t r9  __asm__("r9")  = a5;
    uint64_t rax = op, rdx = a2;
    __asm__ volatile("int $0x80"
                     : "+a"(rax), "+d"(rdx)
                     : "D"(a0), "S"(a1), "r"(r10), "r"(r8), "r"(r9)
                     : "memory");
    if (out) *out = rdx;
    return rax;
}

static inline uint64_t je_clock_ticks(uint64_t *ticks)
{ return je_sys6(JE_SYS_CLOCK_TICKS, 0, 0, 0, 0, 0, 0, ticks); }

static inline uint64_t je_try_send(uint64_t h, const void *buf, uint64_t len)
{ return je_sys6(JE_SYS_TRY_SEND, h, (uint64_t)(uintptr_t)buf, len, 0, 0, 0, (uint64_t *)0); }

/* Capability-carrying send. Each entry is {low32 handle, high32 rights}. */
static inline uint64_t je_try_send_caps(uint64_t h, const void *buf, uint64_t len,
                                        const uint64_t *caps, uint64_t cap_count)
{
    return je_sys6(JE_SYS_TRY_SEND, h, (uint64_t)(uintptr_t)buf, len,
                   cap_count, (uint64_t)(uintptr_t)caps, 0, (uint64_t *)0);
}

/* timeout 0 = poll. *out (optional) = RDX: bytes[31:0] | caps[63:32]. */
static inline uint64_t je_try_recv(uint64_t h, void *buf, uint64_t cap, uint64_t timeout,
                                   uint64_t *out)
{ return je_sys6(JE_SYS_TRY_RECV, h, (uint64_t)(uintptr_t)buf, cap, timeout, 0, 0, out); }

static inline uint64_t je_handle_rights(uint64_t h, uint64_t *rights)
{ return je_sys6(JE_SYS_HANDLE_RIGHTS, h, 0, 0, 0, 0, 0, rights); }

/* PR-F ring-3 wrappers (the supervisor and its children). Thin: each forwards the
 * abi.h register contract and returns the je_abi_status; `out` (when non-NULL)
 * receives RDX. See include/jeios/abi.h for each op's exact semantics. */

static inline uint64_t je_yield(void)
{ return je_sys6(JE_SYS_YIELD, 0, 0, 0, 0, 0, 0, (uint64_t *)0); }

static inline uint64_t je_handle_close(uint64_t h)
{ return je_sys6(JE_SYS_HANDLE_CLOSE, h, 0, 0, 0, 0, 0, (uint64_t *)0); }

/* handle_dup(h, new_rights) -> *out = new handle (RDX). */
static inline uint64_t je_handle_dup(uint64_t h, uint64_t new_rights, uint64_t *out)
{ return je_sys6(JE_SYS_HANDLE_DUP, h, new_rights, 0, 0, 0, 0, out); }

/* endpoint_create(): mints BOTH ends into the caller's table; *out (RDX) packs
 * RDX[31:0]=h0, RDX[63:32]=h1 (undefined on failure). All args must be 0. */
static inline uint64_t je_endpoint_create(uint64_t *packed_ends)
{ return je_sys6(JE_SYS_ENDPOINT_CREATE, 0, 0, 0, 0, 0, 0, packed_ends); }

/* proc_spawn(launcher, image_id, grants, grant_count, quotas): see JE_ABI_SPAWN_*.
 * *out (RDX) = new PROCESS handle (INSPECT|KILL), 0 on failure. Transactional. */
static inline uint64_t je_proc_spawn(uint64_t launcher, uint64_t image_id,
                                     const void *grants, uint64_t grant_count,
                                     uint64_t quotas, uint64_t *out)
{ return je_sys6(JE_SYS_PROC_SPAWN, launcher, image_id,
                 (uint64_t)(uintptr_t)grants, grant_count, quotas, 0, out); }

/* proc_status(h, out): copies a 16-byte je_abi_proc_status to *status. */
static inline uint64_t je_proc_status(uint64_t h, void *status)
{ return je_sys6(JE_SYS_PROC_STATUS, h, (uint64_t)(uintptr_t)status,
                 0, 0, 0, 0, (uint64_t *)0); }

/* proc_kill(h): synchronous; needs KILL on a PROCESS handle. */
static inline uint64_t je_proc_kill(uint64_t h)
{ return je_sys6(JE_SYS_PROC_KILL, h, 0, 0, 0, 0, 0, (uint64_t *)0); }

__attribute__((noreturn)) static inline void je_exit(uint64_t code)
{
    (void)je_sys6(JE_SYS_EXIT, code, 0, 0, 0, 0, 0, (uint64_t *)0);
    for (;;) __asm__ volatile("ud2");
}

/* Freestanding memory functions (user/lib/mem.c). GCC may emit calls to these. */
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);

#endif /* JEIOS_USER_H */
