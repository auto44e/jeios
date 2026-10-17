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

/* M3a #5 step (b): a user program that exercises copy_probe (op 3) across a
 * battery of cases and records each returned status into its data page, so the
 * kernel can read back the results. Layout of recorded statuses (at data page
 * 0x40001000, after the heartbeat word which stays at +0): each case stores its
 * JE_ABI status at a fixed offset. src/dst are in-arena scratch at +0x100/+0x180.
 *   +0x20 valid small copy       (expect OK)
 *   +0x28 len=0                  (expect OK)
 *   +0x30 null src               (expect INVALID)
 *   +0x38 kernel-range src       (expect INVALID)
 *   +0x40 len>256                (expect INVALID)
 * After the battery it loops on the heartbeat so the kernel sees it alive.
 * RAX=3 (copy_probe), RDI=src, RSI=dst, RDX=len; reserved R10/R8/R9=0. */
.globl je_user_copyprobe_start
.globl je_user_copyprobe_end
je_user_copyprobe_start:
    /* helper expectation: a macro-free sequence; registers set per call. */
    /* case: valid small copy (16 bytes, arena scratch src+0x100 -> dst+0x180) */
    movq $3, %rax
    movq $0x40001100, %rdi      /* src in-arena */
    movq $0x40001180, %rsi      /* dst in-arena */
    movq $16, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
    movq %rax, 0x40001020
    /* case: len == 0 (expect OK, no touch) */
    movq $3, %rax
    movq $0x40001100, %rdi
    movq $0x40001180, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
    movq %rax, 0x40001028
    /* case: null src (expect INVALID) */
    movq $3, %rax
    xorq %rdi, %rdi
    movq $0x40001180, %rsi
    movq $16, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
    movq %rax, 0x40001030
    /* case: kernel-range src 0x100000 (expect INVALID) */
    movq $3, %rax
    movq $0x100000, %rdi
    movq $0x40001180, %rsi
    movq $16, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
    movq %rax, 0x40001038
    /* case: len > 256 (expect INVALID) */
    movq $3, %rax
    movq $0x40001100, %rdi
    movq $0x40001180, %rsi
    movq $300, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
    movq %rax, 0x40001040
7:
    incq 0x40001000             /* heartbeat; keep running */
    jmp 7b
je_user_copyprobe_end:

#ifdef JE_DEV_COPY_INJECT
/* M3a #5 step (b), DEV-ONLY (compile-guarded): invoke the fixup-injection op
 * (RAX=4) with RDI = an unmapped arena page (0x40003000), record the recovered
 * flag (RDX) at heartbeat+0x20, then loop. Proves the kernel recovered a
 * copy-site #PF rather than panicking. Absent from the default build. */
.globl je_user_inject_start
.globl je_user_inject_end
je_user_inject_start:
    movq $4, %rax               /* JE_SYS_COPY_FIXUP_INJECT */
    movq $0x40003000, %rdi      /* unmapped arena page */
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
    movq %rdx, 0x40001020        /* recovered flag */
8:
    incq 0x40001000
    jmp 8b
je_user_inject_end:
#endif

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

/* M3b second PR: exercise the handle syscalls (self=5, close=6, query=7) and
 * record each result at a fixed data-page offset for the kernel to assert. SELF
 * is this process's identity capability (type JE_ABI_OBJ_SELF=1). Layout of the
 * recorded words (data page at 0x40001000):
 *   +0x20 self#1 status     +0x28 self#1 handle
 *   +0x30 query(live) status +0x38 query(live) type
 *   +0x40 close(live) status
 *   +0x48 query(closed) status   +0x50 close(closed) status
 *   +0x58 close(handle 0) status
 *   +0x60 query(bound index) status
 *   +0x68 query(wrong gen, live slot) status
 *   +0x70 close(wide handle) status
 *   +0x78 self#2 handle (slot reuse; must differ from self#1)
 *   +0x80 quota status (after filling the quota)
 * then a heartbeat loop. r11 holds self#1's handle across the sequence. */
.globl je_user_handle_start
.globl je_user_handle_end
je_user_handle_start:
    /* self#1 -> status@+0x20, handle(RDX)@+0x28 */
    movq $5, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001020
    movq %rdx, 0x40001028
    movq %rdx, %r11             /* keep self#1 handle */
    /* query(live, h=self#1) -> status@+0x30, type(RDX)@+0x38 */
    movq $7, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001030
    movq %rdx, 0x40001038
    /* close(live, h=self#1) -> status@+0x40 */
    movq $6, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001040
    /* query(closed, h=self#1) -> status@+0x48 (expect STALE) */
    movq $7, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001048
    /* close(closed, h=self#1) -> status@+0x50 (expect STALE) */
    movq $6, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001050
    /* close(handle 0) -> status@+0x58 (expect BADINDEX) */
    movq $6, %rax
    xorq %rdi,%rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001058
    /* query(index == capacity 16, gen 1) -> status@+0x60 (expect BADINDEX).
     * encode(16,1) = (1<<16)|16 = 0x10010. */
    movq $7, %rax
    movq $0x10010, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001060
    /* self#2 (slot 0 reused at gen 2) -> status@+0x88, handle@+0x78, kept in r10 */
    movq $5, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001088      /* self#2 status (expect OK) */
    movq %rdx, 0x40001078      /* self#2 handle */
    movq %rdx, %r10            /* self#2 handle */
    /* RE-STALE PROOF: now that slot 0 is LIVE again at gen 2, present the OLD
     * self#1 handle (r11, gen 1) again -> status@+0x90 (expect STALE). This is
     * the real proof that reuse re-stales, not merely that the new handle differs. */
    movq $7, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001090
    movq 0x40001078, %r10      /* restore self#2 handle into r10 */
    /* query(wrong gen on the now-LIVE slot 0): encode(0,99)=99<<16=0x630000
     * -> status@+0x68 (expect STALE, since slot 0 is at gen 2 not 99). */
    movq $7, %rax
    movq $0x630000, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001068
    /* close(wide handle: bit 32 set) -> status@+0x70 (expect INVALID).
     * Reload self#2 from memory and set bit 32, so it names a real slot but is
     * rejected pre-narrow on the high bits. */
    movq $6, %rax
    movq 0x40001078, %rdi       /* self#2 handle */
    movq $0x100000000, %rcx
    orq %rcx, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001070
    /* quota: self#2 holds 1 live; quota = CAP-1 = 15, so exactly 14 more mints
     * succeed and the 15th acquisition returns QUOTA. Count the SUCCESSES (r12)
     * so the kernel can assert exactly 14 — proving QUOTA (not NOSLOTS) ends the
     * loop at CAP-1. r13 bounds the loop so a bug can't spin forever. */
    xorq %r12, %r12            /* success counter */
    movq $64, %r13             /* loop bound > quota */
9:
    movq $5, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001080      /* last self status */
    cmpq $7, %rax              /* JE_ABI_H_QUOTA == 7 */
    je 10f
    cmpq $0, %rax              /* JE_ABI_OK == 0 */
    jne 10f                    /* any other status: stop (unexpected) */
    incq %r12                  /* a successful mint */
    decq %r13
    jnz 9b
10:
    movq %r12, 0x40001098      /* count of successful mints after self#2 */
    /* done; heartbeat loop */
11:
    incq 0x40001000
    jmp 11b
je_user_handle_end:
.section .note.GNU-stack,"",@progbits
