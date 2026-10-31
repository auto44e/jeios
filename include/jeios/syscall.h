/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_SYSCALL_H
#define JEIOS_SYSCALL_H
/* M3a #5 step (a): v0 syscall dispatcher (see kernel/core/syscall.c). */
#include <stdint.h>
#include <jeios/abi.h>
#include <jeios/trap.h>

/* What the pure decoder decided the kernel glue should do. */
typedef enum {
    JE_SYSCALL_RETURN = 0,  /* resume the caller with status in RAX, result in RDX */
    JE_SYSCALL_YIELD  = 1,  /* write OK into the caller frame, then reschedule */
    JE_SYSCALL_EXIT   = 2,  /* terminate the caller; *out_result is the exit code */
    JE_SYSCALL_COPY_PROBE = 3,  /* glue performs je_copy_probe with the frame args */
    /* M3b: handle ops. Decode validates reserved args and returns these; the glue
     * resolves the current user process and calls the per-process handle table. */
    JE_SYSCALL_HANDLE_SELF  = 5,
    JE_SYSCALL_HANDLE_CLOSE = 6,
    JE_SYSCALL_HANDLE_QUERY = 7,
    JE_SYSCALL_HANDLE_RIGHTS = 8,
    JE_SYSCALL_HANDLE_DUP = 9,
    JE_SYSCALL_ENDPOINT_CREATE = 10, /* M3c: glue mints both ends into the
                                      * caller's table and packs the two handles
                                      * into RDX (A3-2) */
    JE_SYSCALL_TRY_SEND = 11,        /* M3c D6: glue copies user->staging then
                                      * enqueues into the peer inbound */
    JE_SYSCALL_TRY_RECV = 12,        /* M3c D6: glue peeks, copies out, commits */
    JE_SYSCALL_CLOCK_TICKS = 13,     /* ADR 0010 D6: glue sets RDX = je_sched_ticks() */
    JE_SYSCALL_PROC_SPAWN = 14,      /* ADR 0010 D5: glue copies grants in, calls je_proc_spawn */
    JE_SYSCALL_PROC_STATUS = 15,     /* ADR 0010 D4: glue snapshots the record, copies out */
    JE_SYSCALL_PROC_KILL = 16        /* ADR 0010 D8: glue resolves KILL, calls je_sched_kill */
#ifdef JE_DEV_COPY_INJECT
    , JE_SYSCALL_COPY_FIXUP_INJECT = 4  /* glue raw-copies an unmapped page (fixup test) */
#endif
} je_syscall_action;

/* Pure: (op, six arg registers) -> action + ABI status + optional scalar result.
 * No kernel state; unit-tested on the host. */
je_syscall_action je_syscall_decode(uint64_t op, const uint64_t args[6],
                                    je_abi_status *out_status, uint64_t *out_result);

/* Kernel glue: dispatch a user-origin int 0x80 trap frame; returns the frame to
 * iretq to (the caller, or a live successor for yield/exit). */
je_trap_frame *je_syscall_dispatch(je_trap_frame *frame);

#endif
