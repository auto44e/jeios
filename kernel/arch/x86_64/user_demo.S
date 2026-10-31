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
 *   +0x18 query(in-range FREE slot, index CAP-1 = 39) status (expect STALE)
 *   +0x60 query(index == capacity, JE_VM_HANDLE_CAP = 40) status (expect BADINDEX)
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
    /* rights(live, h=self#1) op8 -> status@+0xa0, rights mask(RDX)@+0xa8.
     * SELF is minted with INSPECT(0x1), so expect OK + 0x1. */
    movq $8, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010a0
    movq %rdx, 0x400010a8
    /* --- attenuation (op 9) matrix, from the live self#1 in r11 --- */
    /* dup(self#1, INSPECT): full-rights alias -> status@+0xb0, new handle@+0xb8.
     * Keep the derived weak handle's value for the enforcement step below. */
    movq $9, %rax
    movq %r11, %rdi
    movq $0x1, %rsi             /* new_rights = INSPECT */
    xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010b0
    movq %rdx, 0x400010b8
    /* dup(self#1, 0): derive a RIGHTS-LESS handle -> status@+0xc0, handle@+0xc8 */
    movq $9, %rax
    movq %r11, %rdi
    xorq %rsi,%rsi             /* new_rights = 0 (drop INSPECT) */
    xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010c0
    movq %rdx, 0x400010c8
    movq %rdx, %r12            /* keep the weak (no-INSPECT) handle */
    /* THE ENFORCEMENT PROOF: handle_rights(weak) requires INSPECT, which this
     * derived handle lacks -> DENIED (status 6), from ring 3 -> status@+0xd0 */
    movq $8, %rax
    movq %r12, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010d0
    /* dup(weak, INSPECT): superset request (weak lacks INSPECT) -> DENIED@+0xd8 */
    movq $9, %rax
    movq %r12, %rdi
    movq $0x1, %rsi
    xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010d8
    /* dup(self#1, 0x80000000): still-UNKNOWN rights bit -> INVALID (2)@+0xe0.
     * (M3c made 0x2=SEND a KNOWN right per ADR 0009 D4a, so the unknown-bit
     * contract is now proven on bit 31, which remains unknown. 0x80000000 fits
     * 32 bits so it passes the wide-rights check and reaches the known-bit gate.) */
    movq $9, %rax
    movq %r11, %rdi
    movq $0x80000000, %rsi
    xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010e0
    /* close(weak) must work even with 0 rights -> OK@+0xe8 (no slot leak) */
    movq $6, %rax
    movq %r12, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010e8
    /* close the full-rights alias too (slot from +0xb8), so the later quota
     * count starts from a known live set (only self#1 live again). -> @+0xf0 */
    movq $6, %rax
    movq 0x400010b8, %rdi      /* d_full handle */
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400010f0
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
    /* query(index == capacity 40, gen 1) -> status@+0x60 (expect BADINDEX).
     * encode(40,1) = (1<<16)|40 = 0x10028. vm.c pins this literal to the
     * production JE_VM_HANDLE_CAP so this boundary probe cannot drift silently. */
    movq $7, %rax
    movq $0x10028, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001060
    /* query an in-range FREE slot (index 39, gen 1) -> status@+0x18
     * (expect STALE). This is separate from index==capacity BADINDEX. */
    movq $7, %rax
    movq $0x10027, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001018
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
    /* quota: self#2 holds 1 live; the default quota is 15 (the table capacity
     * is H=40), so exactly 14 more mints succeed and the next acquisition
     * returns QUOTA. Count the SUCCESSES (r12) so the kernel can assert exactly
     * 14. r13 bounds the loop so a bug cannot spin forever. */
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

/* ---- M3c s3: endpoint create + typed close + fault-death ------------------
 * endpoint_create (op 10) -> RDX packs (h1<<32)|h0. Store the pair, close h0 via
 * HANDLE_CLOSE (op 6, which must route through the typed endpoint close and keep
 * the channel alive because h1 still names the other end), store that status,
 * then fault by reading an unmapped arena page so the kernel reaps us and checks
 * the pool. Scratch offsets 0x1200.. avoid the handle stub's 0x1000-0x10f8. */
.globl je_user_epfault_start
.globl je_user_epfault_end
je_user_epfault_start:
    movq $4, %rax              /* brief heartbeat so we're seen running */
20:
    incq 0x40001000
    decq %rax
    jnz 20b
    /* endpoint_create(): all args zero. */
    movq $10, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001200      /* create status -> +0x200 */
    movq %rdx, 0x40001208      /* packed handles  -> +0x208 */
    movq %rdx, %r14            /* keep the pair */
    /* h0 = low 32 bits; close it via HANDLE_CLOSE (op 6). Writing EDI
     * zero-extends into RDI, isolating the low dword (h0). */
    movq %r14, %rdi
    movl %edi, %edi
    movq $6, %rax
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001210      /* close(h0) status -> +0x210 */
    /* Now fault: the surviving h1 is still LIVE, so reap must release it and the
     * channel must return to the pool. */
    movq 0x40003000, %rax      /* unmapped arena page -> #PF */
21:
    jmp 21b                    /* unreached */
je_user_epfault_end:

/* ---- M3c s4: data IPC self-loopback (both ends in this one process's table) --
 * endpoint_create gives h0 (RDX[31:0]) and h1 (RDX[63:32]). A send on h0 lands in
 * h1's inbound; recv on h1 reads it. Proves ring-3 copy-in / queue / direction /
 * copy-out rollback / SHORT_BUFFER / QUEUE_FULL / PEER_CLOSED. It does NOT prove
 * cross-address-space delivery (one process holds both ends).
 * Scratch: src payload @0x1300 (8 bytes), recv dst @0x1340, results @0x1380+. */
.globl je_user_epipc_start
.globl je_user_epipc_end
je_user_epipc_start:
    movq $4, %rax
30:
    incq 0x40001000
    decq %rax
    jnz 30b
    /* seed an 8-byte source payload at 0x40001300 = 0xdeadbeef11223344 */
    movq $0xdeadbeef11223344, %rbx
    movq %rbx, 0x40001300
    /* create() */
    movq $10, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001380      /* create status */
    movq %rdx, %r14            /* h0 = low32 */
    movl %r14d, %r14d
    movq %rdx, %r13
    shrq $32, %r13             /* h1 = high32 */
    /* send(h0, src=0x1300, len=8) */
    movq $11, %rax
    movq %r14, %rdi
    movq $0x40001300, %rsi
    movq $8, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001388      /* send status */
    /* recv(h0) -> EMPTY (direction: nothing landed in h0's inbound) */
    movq $12, %rax
    movq %r14, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001390      /* expect JE_ABI_H_EMPTY (13) */
    /* recv(h1, cap=1) -> SHORT_BUFFER, RDX=8 */
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40001340, %rsi
    movq $1, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001398      /* expect SHORT_BUFFER (11) */
    movq %rdx, 0x400013a0      /* expect required len 8 */
    /* recv(h1, unmapped dst 0x40003000) -> INVALID; message stays queued */
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40003000, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013a8      /* expect INVALID (2) */
    /* recv(h1, good dst) -> OK, 8 bytes, payload matches */
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013b0      /* expect OK (0) */
    movq %rdx, 0x400013b8      /* expect 8 */
    movq 0x40001340, %rbx      /* delivered payload */
    movq %rbx, 0x400013c0      /* expect 0xdeadbeef11223344 */
    /* recv(h1) again -> EMPTY (delivered exactly once) */
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013c8      /* expect EMPTY (13) */
    /* bad-source send(h0, src=unmapped) -> INVALID; nothing enqueued */
    movq $11, %rax
    movq %r14, %rdi
    movq $0x40003000, %rsi
    movq $8, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013d0      /* expect INVALID (2) */
    /* recv(h1) -> EMPTY (bad send enqueued nothing) */
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013d8      /* expect EMPTY (13) */
    /* fill the queue: 4 sends OK, 5th QUEUE_FULL */
    movq $4, %r12
31:
    movq $11, %rax
    movq %r14, %rdi
    movq $0x40001300, %rsi
    movq $8, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    decq %r12
    jnz 31b
    movq %rax, 0x400013e0      /* 4th send status: expect OK */
    movq $11, %rax             /* 5th send -> QUEUE_FULL */
    movq %r14, %rdi
    movq $0x40001300, %rsi
    movq $8, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013e8      /* expect QUEUE_FULL (10) */
    /* data-only reject: send with R10 cap_count=1 -> INVALID */
    movq $11, %rax
    movq %r14, %rdi
    movq $0x40001300, %rsi
    movq $8, %rdx
    movq $1, %r10
    xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013f0      /* expect INVALID (2) */
    /* close h0; now h1's peer is gone. Drain the 4 queued, then PEER_CLOSED. */
    movq $6, %rax
    movq %r14, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x400013f8      /* close(h0) status: expect OK */
    /* drain 4 */
    movq $4, %r12
32:
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    decq %r12
    jnz 32b
    movq %rax, 0x40001400      /* 4th drained recv: expect OK (drain despite peer gone) */
    /* now empty + peer gone -> PEER_CLOSED */
    movq $12, %rax
    movq %r13, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001408      /* expect PEER_CLOSED (12) */
    /* send(h1) -> peer (h0 end) closed -> PEER_CLOSED */
    movq $11, %rax
    movq %r13, %rdi
    movq $0x40001300, %rsi
    movq $8, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001410      /* expect PEER_CLOSED (12) */
    /* done; heartbeat loop so the kernel sees progress then tears us down. */
33:
    incq 0x40001000
    jmp 33b
je_user_epipc_end:

/* M3c step 6 (ADR 0009 A6): blocking recv that TIMES OUT. Create a channel, then
 * recv(h0, timeout=R10) on an end nobody ever sends to -> the thread PARKS; the
 * timer advances past the deadline; the ISR stamps TIMED_OUT; the continuation
 * resumes the recv, returning status 14. A single ring-3 thread + the natural
 * timer tick — fully deterministic, no second process (the step-5 spin-hang
 * lesson). Scratch layout (arena 0x40001xxx): 0x1380 create status, 0x1388 recv
 * status (expect 14), 0x1390 a liveness counter the bootstrap polls. */
.globl je_user_eprecv_to_start
.globl je_user_eprecv_to_end
je_user_eprecv_to_start:
    /* create() -> RDX packs {h0 low32, h1 high32} */
    movq $10, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001380       /* create status (expect 0) */
    movq %rdx, %r14
    movl %r14d, %r14d           /* h0 = low32 */
    /* recv(h0, dst, cap=64, timeout=0x40 ticks). Nothing is ever sent to h0's
     * inbound, so this blocks; after ~0x40 ticks the ISR stamps TIMED_OUT and the
     * continuation returns 14 here. */
    movq $12, %rax
    movq %r14, %rdi
    movq $0x40001340, %rsi
    movq $64, %rdx
    movq $0x40, %r10           /* nonzero timeout -> block */
    xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    movq %rax, 0x40001388      /* recv status after wake (expect TIMED_OUT = 14) */
    /* liveness heartbeat so the bootstrap can confirm the thread resumed. */
50:
    incq 0x40001390
    jmp 50b
je_user_eprecv_to_end:

/* M3c step 7 (ADR 0009 D-SEQ step 7) — the bootstrap fixture's two ring-3 stubs.
 * A (PINGER) and B (PONGER) share a kernel-seeded channel S; the bootstrap injects
 * each process's S handle at arena 0x40001500 BEFORE the thread starts (fixture
 * ABI, not process ABI). Blocking recv (step 6) is what makes this deterministic:
 * a receiver with nothing queued PARKS and yields to the peer, so there is no
 * cooperative-scheduler deadlock. Each stub writes a monotonic progress word at
 * 0x40001508 (the bootstrap polls it) and a final status at 0x40001510 (0 = all
 * asserts held; nonzero = the failing step). Timeout on every blocking recv is a
 * large hang-breaker; the asserts require WOKEN delivery (status 0), never 14.
 *
 * Scratch (per-process, same offsets — distinct address spaces):
 *   0x2000 seeded S handle (injected)      0x2008 progress word
 *   0x2010 final status (0 ok)             0x2040 recv byte buffer
 *   0x2048 recv handle-out (u32)           0x2050 send payload scratch
 * Message tags: MINTED=0x4d GO=0x60 DONE=0xd0 PINGv=0xaa PONGv=0xbb XFERv=0xcc
 * Timeout ticks = 0x4000 (large; a WOKEN path never reaches it). */

/* ---- A / PINGER ---- holds S handle hS; creates C, transfers c1 to B. */
.globl je_user_ping_start
.globl je_user_ping_end
je_user_ping_start:
    movq 0x40001500, %r15         /* hS = seeded S handle (injected) */
    movq $1, 0x40001508           /* progress = 1: started */
    /* create C -> RDX packs {c0 low32, c1 high32} */
    movq $10, %rax
    xorq %rdi,%rdi; xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Aerr_create
    movq %rdx, %r14               /* c0 = low32 */
    movl %r14d, %r14d
    movq %rdx, %r13
    shrq $32, %r13                /* c1 = high32 */
    /* send a cap-carrying message over S: 1 byte XFERv + cap[0] = {c1 handle,
     * rights=0xe (SEND|RECV|TRANSFER)}. R10[7:0]=cap_count=1, R8=cap array ptr. */
    movq %r13, 0x40001550         /* u64[0] low32 = c1 handle */
    movl $0xe, %eax
    shlq $32, %rax
    orq  %rax, 0x40001550         /* high32 = rights 0xe */
    movb $0xcc, 0x40001558        /* 1 payload byte XFERv */
    movq $11, %rax                /* try_send */
    movq %r15, %rdi               /* hS */
    movq $0x40001558, %rsi        /* ubuf (1 byte) */
    movq $1, %rdx                 /* len = 1 */
    movq $1, %r10                 /* cap_count = 1 */
    movq $0x40001550, %r8         /* cap array */
    xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Aerr_xfer
    movq $2, 0x40001508           /* progress = 2: cap sent */
    /* recv MINTED ack on S (blocks until B mints + replies -> WOKEN). */
    call A_blockrecv_S
    cmpb $0x4d, 0x40001540        /* expect MINTED tag */
    jne  Aerr_minted
    /* close c1 BEFORE B uses the transferred handle (prove retained ownership). */
    movq $6, %rax                 /* handle_close */
    movq %r13, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Aerr_close
    movq $3, 0x40001508           /* progress = 3: c1 closed */
    /* send GO on S. */
    movb $0x60, 0x40001558
    call A_send1_S
    /* recv on c0 the transferred-channel reply B sends on its minted handle. */
    movq $12, %rax                /* try_recv */
    movq %r14, %rdi               /* c0 */
    movq $0x40001540, %rsi
    movq $64, %rdx
    movq $0x4000, %r10            /* block */
    xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax                 /* expect OK (WOKEN) */
    jne  Aerr_c0recv
    movl %edx, %ebx               /* byte len (low32, zero-extended) must be 1 */
    cmpq $1, %rbx
    jne  Aerr_c0len
    cmpb $0xcc, 0x40001540        /* expect XFERv echoed on the transferred channel */
    jne  Aerr_c0val
    movq $4, 0x40001508           /* progress = 4: transfer round-trip OK */
    /* ping-pong on S: 4 rounds. A sends PING, blocks for PONG. */
    movq $4, %rcx
Aloop:
    movb $0xaa, 0x40001558
    call A_send1_S
    call A_blockrecv_S
    cmpb $0xbb, 0x40001540        /* expect PONG */
    jne  Aerr_pong
    decq %rcx
    jnz  Aloop
    movq $5, 0x40001508           /* progress = 5: ping-pong done */
    /* DONE ack teardown: A sends DONE, closes c0 + hS, exits. */
    movb $0xd0, 0x40001558
    call A_send1_S
    movq $6, %rax                 /* close c0 */
    movq %r14, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Aerr_closec0
    movq $6, %rax                 /* close hS */
    movq %r15, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Aerr_closehs
    movq $0, 0x40001510           /* final status = 0 OK */
    movq $6, 0x40001508           /* progress = 6: done */
    movq $2, %rax                 /* exit(0) */
    xorq %rdi,%rdi
    int $0x80
A_hang: jmp A_hang

/* helper: send the 1 byte at 0x2058 on hS (r15). clobbers rax/rdi/rsi/rdx/r10/r8/r9. */
A_send1_S:
    movq $11, %rax
    movq %r15, %rdi
    movq $0x40001558, %rsi
    movq $1, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Aerr_send
    ret
/* helper: blocking recv 1+ bytes on hS into 0x2040. */
A_blockrecv_S:
    movq $12, %rax
    movq %r15, %rdi
    movq $0x40001540, %rsi
    movq $64, %rdx
    movq $0x4000, %r10            /* block */
    xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax                 /* must be OK (WOKEN); 13/14 -> fail */
    jne  Aerr_recv
    movl %edx, %ebx               /* RDX low32 = byte len (zero-extended); must be 1 */
    cmpq $1, %rbx
    jne  Aerr_rlen                /* zero-length delivery would pass a stale-byte tag check */
    ret
Aerr_create:  movq $11, 0x40001510; jmp Afail
Aerr_xfer:    movq $12, 0x40001510; jmp Afail
Aerr_minted:  movq $13, 0x40001510; jmp Afail
Aerr_close:   movq $14, 0x40001510; jmp Afail
Aerr_c0recv:  movq $15, 0x40001510; jmp Afail
Aerr_c0val:   movq $16, 0x40001510; jmp Afail
Aerr_pong:    movq $17, 0x40001510; jmp Afail
Aerr_send:    movq $18, 0x40001510; jmp Afail
Aerr_recv:    movq $19, 0x40001510; jmp Afail
Aerr_rlen:    movq $20, 0x40001510; jmp Afail
Aerr_c0len:   movq $31, 0x40001510; jmp Afail
Aerr_closec0: movq $32, 0x40001510; jmp Afail
Aerr_closehs: movq $33, 0x40001510; jmp Afail
Afail:
    movq $99, 0x40001508          /* progress = 99: failed (status slot has which) */
    movq $2, %rax; movq $1, %rdi; int $0x80   /* exit(1) */
Afail_hang: jmp Afail_hang
je_user_ping_end:

/* ---- B / PONGER ---- holds S handle hS; receives the cap, mints, uses it. */
.globl je_user_pong_start
.globl je_user_pong_end
je_user_pong_start:
    movq 0x40001500, %r15         /* hS */
    movq $1, 0x40001508           /* progress = 1: started (pre-recv marker) */
    /* blocking recv the cap-carrying message on S (B parks here until A sends;
     * this is the CAUSALLY-FORCED WOKEN — the bootstrap confirms B parked before
     * creating A). R8 = handle-out (u32), R9 = slot cap = 1. */
    movq $12, %rax
    movq %r15, %rdi
    movq $0x40001540, %rsi        /* byte buf */
    movq $64, %rdx
    movq $0x4000, %r10            /* block */
    movq $0x40001548, %r8         /* handle-out array */
    movq $1, %r9                  /* 1 slot */
    int $0x80
    cmpq $0, %rax                 /* OK (WOKEN) */
    jne  Berr_recv
    /* RDX high32 must report 1 cap delivered; byte payload XFERv. */
    movq %rdx, %rbx
    shrq $32, %rbx
    cmpq $1, %rbx
    jne  Berr_ncaps
    cmpb $0xcc, 0x40001540
    jne  Berr_xval
    movl 0x40001548, %r14d        /* cB = minted transferred handle (u32) */
    testl %r14d, %r14d
    jz   Berr_mint                /* 0 = dead cap -> fail */
    movq $2, 0x40001508           /* progress = 2: cap minted */
    /* send MINTED ack on S, then block for GO (so A can close c1 before we use cB). */
    movb $0x4d, 0x40001558
    call B_send1_S
    call B_blockrecv_S
    cmpb $0x60, 0x40001540        /* expect GO */
    jne  Berr_go
    movq $3, 0x40001508           /* progress = 3: GO received (A has closed c1) */
    /* use the transferred handle cB AFTER A closed c1: send XFERv on it -> lands
     * on A's c0. This is the retained-ownership proof (post-close send succeeds). */
    movb $0xcc, 0x40001558
    movq $11, %rax
    movl %r14d, %edi              /* cB */
    movq $0x40001558, %rsi
    movq $1, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Berr_cbsend
    movq $4, 0x40001508           /* progress = 4: used transferred handle post-close */
    /* ping-pong: 4 rounds. B blocks for PING, replies PONG. */
    movq $4, %rcx
Bloop:
    call B_blockrecv_S
    cmpb $0xaa, 0x40001540        /* expect PING */
    jne  Berr_ping
    movb $0xbb, 0x40001558        /* PONG */
    call B_send1_S
    decq %rcx
    jnz  Bloop
    movq $5, 0x40001508
    /* DONE: B blocks for DONE, then closes cB + hS, exits. */
    call B_blockrecv_S
    cmpb $0xd0, 0x40001540        /* expect DONE */
    jne  Berr_done
    movq $6, %rax                 /* close cB */
    movl %r14d, %edi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Berr_closecb
    movq $6, %rax                 /* close hS */
    movq %r15, %rdi
    xorq %rsi,%rsi; xorq %rdx,%rdx; xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Berr_closehs
    movq $0, 0x40001510           /* final status = 0 OK */
    movq $6, 0x40001508
    movq $2, %rax; xorq %rdi,%rdi; int $0x80   /* exit(0) */
B_hang: jmp B_hang

B_send1_S:
    movq $11, %rax
    movq %r15, %rdi
    movq $0x40001558, %rsi
    movq $1, %rdx
    xorq %r10,%r10; xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Berr_send
    ret
B_blockrecv_S:
    movq $12, %rax
    movq %r15, %rdi
    movq $0x40001540, %rsi
    movq $64, %rdx
    movq $0x4000, %r10
    xorq %r8,%r8; xorq %r9,%r9
    int $0x80
    cmpq $0, %rax
    jne  Berr_brecv
    movl %edx, %ebx               /* byte len (low32, zero-extended) must be 1 */
    cmpq $1, %rbx
    jne  Berr_brlen
    ret
Berr_recv:   movq $21, 0x40001510; jmp Bfail
Berr_ncaps:  movq $22, 0x40001510; jmp Bfail
Berr_xval:   movq $23, 0x40001510; jmp Bfail
Berr_mint:   movq $24, 0x40001510; jmp Bfail
Berr_go:     movq $25, 0x40001510; jmp Bfail
Berr_cbsend: movq $26, 0x40001510; jmp Bfail
Berr_ping:   movq $27, 0x40001510; jmp Bfail
Berr_done:   movq $28, 0x40001510; jmp Bfail
Berr_send:   movq $29, 0x40001510; jmp Bfail
Berr_brecv:  movq $30, 0x40001510; jmp Bfail
Berr_brlen:  movq $34, 0x40001510; jmp Bfail
Berr_closecb: movq $35, 0x40001510; jmp Bfail
Berr_closehs: movq $36, 0x40001510; jmp Bfail
Bfail:
    movq $99, 0x40001508
    movq $2, %rax; movq $1, %rdi; int $0x80
Bfail_hang: jmp Bfail_hang
je_user_pong_end:

.section .note.GNU-stack,"",@progbits
