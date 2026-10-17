/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3a #4 step 3: a tiny ring-3 demo program for the CPL3-entry boot proof.
 * It increments a heartbeat word in its USER_DATA page forever. Assembled with
 * the kernel toolchain (no hand-encoded bytes); the kernel copies the bytes
 * between je_user_demo_start/_end into the image blob's code segment.
 *
 * The heartbeat lives at arena offset 0x1000 (the USER_DATA page), absolute VA
 * JE_VM_ARENA_BASE + 0x1000 = 0x40001000. Using an absolute disp32 address keeps
 * the code position-independent of where the object is linked (it runs from the
 * arena, not from the kernel image), and emits no relocation.
 *
 * Runs at CPL3 with no stack use, no syscalls, no FP/SIMD: just a memory
 * increment loop, which is exactly enough to prove real ring-3 execution under
 * timer preemption.
 */
.section .rodata.user_demo,"a"
.code64
.globl je_user_demo_start
.globl je_user_demo_end
je_user_demo_start:
1:
    incq 0x40001000            /* absolute disp32; heartbeat in USER_DATA page */
    jmp 1b
je_user_demo_end:

/* M3a #4 step 3: a hostile-FP demo for the D7 enforcement proof. Executes an SSE
 * instruction at CPL3; with CR0.EM=1 (set by je_arch_cpu_policy_init) the CPU
 * raises #UD (vector 6) instead of executing it, proving D7 denies SIMD to ring
 * 3 by hardware, not just by compiler flag. (Until step 4's fault-locality, the
 * resulting fault is a kernel PANIC; the test asserts vector 6 from a cs=0x33
 * frame.) */
.globl je_user_fp_start
.globl je_user_fp_end
je_user_fp_start:
    .byte 0x0f, 0x57, 0xc0      /* xorps %xmm0,%xmm0 */
2:
    jmp 2b
je_user_fp_end:

/* M3a #4 step 4: a user program that faults by reading an unmapped arena address
 * after a short delay, to prove fault-local termination. It increments its
 * heartbeat a few times (so the kernel can confirm it ran at CPL3) then reads
 * arena offset 0x3000 — a page that is NOT mapped in its image (image is code
 * at 0, heartbeat at 0x1000) — raising a user #PF (vector 14). */
.globl je_user_fault_start
.globl je_user_fault_end
je_user_fault_start:
    movq $8, %rax              /* small spin count so it is seen running first */
3:
    incq 0x40001000            /* heartbeat */
    decq %rax
    jnz 3b
    movq 0x40003000, %rax      /* read unmapped arena page -> #PF (vector 14) */
    jmp 3b                     /* unreached */
je_user_fault_end:

/* M3a #5 step (a): a user program that calls exit(42) via the int 0x80 gate —
 * a clean, controlled termination through the D6 gate (DEATH_EXIT, code 42),
 * as opposed to a fault. RAX=JE_SYS_EXIT(2), RDI=42. */
.globl je_user_exit_start
.globl je_user_exit_end
je_user_exit_start:
    incq 0x40001000            /* one heartbeat so we can see it ran at CPL3 */
    movq $2, %rax              /* JE_SYS_EXIT */
    movq $42, %rdi             /* exit code 42 */
    xorq %rsi, %rsi            /* reserved args zero */
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80                  /* -> je_sched_exit_current; never returns */
    jmp je_user_exit_start     /* unreached */
je_user_exit_end:

/* M3a #5 step (a): a user program exercising returning syscalls — abi_query and
 * yield — then looping on its heartbeat. Proves the dispatcher returns to the
 * caller with a result (RAX/RDX) and that yield reschedules and resumes with OK.
 * It stores abi_query's returned version into the heartbeat+8 word so the kernel
 * can read back that a real syscall result reached ring 3. */
.globl je_user_syscall_start
.globl je_user_syscall_end
je_user_syscall_start:
    /* abi_query(version): RAX=0 (JE_SYS_ABI_QUERY), RDI=0 (version). */
    xorq %rax, %rax
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80                  /* -> RAX=status, RDX=version */
    movq %rax, 0x40001008      /* record status at heartbeat+8 */
    movq %rdx, 0x40001010      /* record version at heartbeat+16 */
5:
    incq 0x40001000            /* heartbeat */
    movq $1, %rax              /* JE_SYS_YIELD */
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80                  /* yield; resumes here with RAX=OK */
    movq %rax, 0x40001018      /* record yield status at heartbeat+24 */
    jmp 5b
je_user_syscall_end:

/* M3a #5: a user program that calls an UNKNOWN syscall op (must return
 * UNSUPPORTED and keep running), records the status, then loops. Proves an
 * unknown op does NOT terminate the caller. */
.globl je_user_unknownop_start
.globl je_user_unknownop_end
je_user_unknownop_start:
    movq $0xdeadbeef, %rax     /* unknown op */
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80                  /* -> RAX=UNSUPPORTED, returns */
    movq %rax, 0x40001008      /* record status */
6:
    incq 0x40001000            /* heartbeat; keeps running (not terminated) */
    jmp 6b
je_user_unknownop_end:

/* M3a #5 step (a): a user program that corrupts its RSP, then immediately makes
 * a RETURNING syscall (abi_query). The dispatcher's je_sched_return_to_user must
 * validate the frame on the way back and terminate (DEATH_BAD_RETURN) rather than
 * IRETQ the bad frame — proving validation happens on the SYSCALL return path
 * specifically (not only at the next timer dispatch). */
.globl je_user_badret_start
.globl je_user_badret_end
je_user_badret_start:
    incq 0x40001000            /* one heartbeat */
    movq $0x1000, %rsp         /* RSP outside the arena; no fault yet */
    xorq %rax, %rax            /* abi_query (a RETURNING syscall) */
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80                  /* return path must validate -> BAD_RETURN death */
    jmp je_user_badret_start   /* unreached */
je_user_badret_end:

/* M3a #4 step 4: a user program that corrupts its own RSP to an out-of-arena
 * value WITHOUT faulting, then loops. It never traps itself; the scheduler must
 * terminate it at its next dispatch (je_uframe_check fails) instead of halting
 * the kernel (the H1 hole). Proves bad-return-state is fault-local. */
.globl je_user_badrsp_start
.globl je_user_badrsp_end
je_user_badrsp_start:
    incq 0x40001000            /* one heartbeat */
    movq $0x1000, %rsp         /* RSP now outside the arena; no fault yet */
4:
    jmp 4b                     /* loop; next timer dispatch validates+terminates */
je_user_badrsp_end:
.section .note.GNU-stack,"",@progbits
