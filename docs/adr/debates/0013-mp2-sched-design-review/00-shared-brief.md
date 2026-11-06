# MP2 scheduling design review — aarch64 preemptive kernel-thread RR

Reviewing a DESIGN (ADR 0013) for JeiOS, a fresh C11 microkernel, BEFORE any code.
Target: QEMU virt-8.2 / cortex-a72, EL1, MMU on (identity TTBR0), GICv2 + CNTP
timer + a returning EL1h IRQ path already merged. Goal: preemptive round-robin of
>=2 KERNEL threads on the 100 Hz tick, proving all GPRs + return state survive
preemption. NO EL0, NO IPC, NO cooperative yield (those are MP3). Find design
defects NOW: anything that corrupts a switched-to context, hangs, races, ships a
false "it works", is impossible to build, or touches a frozen/tested contract
unsafely. Be concrete to the register/instruction/bit.

## Verified ground truth (merged)

- x86 sched.c (TESTED M2/M3/M4, owner-gated) switches by RETURNING a different
  saved je_trap_frame*: timer IRQ -> je_trap_dispatch -> je_sched_interrupt(f,
  true) returns a frame* -> trap_entry.S `movq %rax,%rsp; …; iretq`. The RR core
  (pick (current+n)%MAX first READY; slot 0 idle always READY; SLEEPING sweep;
  ++je_timer_ticks; RUNNING->READY; ++preemptions if current changed on a timer
  tick; ++dispatches) is arch-neutral in shape but the file is x86/M3/M4-coupled
  (cs==8 check, CR3/TSS, je_uframe_check, int 0x80 rewind, device/IPC hooks,
  sti;hlt) and is NOT linked on ARM.
- x86 fresh-thread fake frame: rsp=top-8 holds je_thread_exit; frame at
  rsp-sizeof with {rip=trampoline, cs=8, rflags=0x202, rsp, ss=16}. trampoline
  calls entry(arg) then je_thread_exit. Kernel stacks: 5 pages from je_kernel_pages,
  page 0 guarded via je_arch_page_guard.
- x86 M2 proof (core/main.c): 7 worker threads; register_probe.S loads FIXED
  constants 16..30 into x-regs and spin-compares until ticks>=64; floor
  preemptions >= (1+7)*2 = 16 and min_dispatches >= 3; reap + page accounting.
  WEAKNESS: every thread uses the SAME constants, so a cross-thread frame SWAP is
  UNDETECTED; no NZCV check.
- ARM (ADR 0012): je_arm_irq runs on each 100 Hz tick; frozen je_arm_trapframe
  (gpr[0..30]@0, sp@248, elr@256, spsr@264, esr@272, far@280, vector@288,
  size 304). je_arm_irq_common restores from the CURRENT SP, NEVER tf->sp
  (PR-2b "a corrupted field cannot redirect the exception stack" rationale). ARM
  is -mgeneral-regs-only (no FP; the trapframe is a COMPLETE kernel-thread
  context). je_arm_pages is file-static in main.c. paging.h has NO single-page
  unmap/TLBI. The ARM svc lands in the FATAL sync slot 4. Default boot stays
  IRQ-masked; only a test build unmasks.

## The design under review (ADR 0013, summarized)

- D1: a NEW pure hosted-testable kernel/core/sched_rr.c (thread table, RR pick,
  SLEEPING sweep, tick+preemption/dispatch/min_dispatches bookkeeping, reap),
  frames as opaque void*, per-arch hooks {frame_init, frame_check,
  dispatch_install}. ARM supplies them via aarch64/sched_arm.c NOW; x86 sched.c
  UNTOUCHED this PR (migrated later). (a) refactor sched.c / (b) ARM-only slice
  are fallbacks.
- D2: je_arm_irq changes to `void *je_arm_irq(je_arm_trapframe *tf)` returning the
  frame to resume; vectors.S adds `mov sp, x0` after `bl je_arm_irq`, before the
  restore (mirrors x86 movq %rax,%rsp). Reverses the frozen "never load SP from
  frame" rule, so the returned ptr is VALIDATED in C: 16-aligned, inside a known
  thread stack, spsr.M==EL1h, I-bit clear; else FATAL.
- D3: PREEMPTION-ONLY. exit -> ZOMBIE then wfi-until-preempted; sleep -> SLEEPING
  + deadline then wfi. No int-0x81/svc yield (MP3 flex).
- D4: per-thread 5-page stack from je_arm_pages, lowest page UNMAPPED as a guard
  via a new je_arm_mmu_unmap_page(va) (clear L3 leaf + DSB ISHST;TLBI VAE1IS,va;
  DSB ISH;ISB). Guard store faults deterministically.
- D5: a new ARM_SCHED_TEST build creates workers + unmasks IRQ; default boot
  stays masked.
- D6: fake frame gpr=0 (+arg in gpr[0]), elr=trampoline, spsr=EL1h I-clear, frame
  at stack_top-304.
- D7: proof with >=2 workers, each a canary loop with PER-THREAD DISTINCT x0..x30
  patterns + NZCV + the two-phase x29/x30 (ADR 0012 D8); floor preemptions >=
  (1+N)*2, min_dispatches >= 3; guard-page fault; oracle recomputes floors +
  distinct patterns. Mutations: single-reg corrupt; FRAME SWAP (the one x86
  misses); skip preemption count; drop guard unmap.

## Questions to answer directly

1. The switch asm: is `mov sp, x0` after `bl je_arm_irq` (returned validated
   frame ptr) then the existing restore + add sp,#304 + eret CORRECT? The current
   handler does `add sp, sp, #JE_TF_SIZE` to unwind — with a switched frame, x0
   points at the NEW thread's saved frame on the NEW thread's stack; after
   restoring x0..x30/ELR/SPSR from [x0-relative offsets], the `add sp,#304` must
   land SP at the new thread's stack top. Walk the exact sequence; is there a
   hazard if the restore reads from sp (now = x0) vs from a saved base? Does the
   transient je_arm_in_handler guard clear correctly across a switch (the thread
   we switch TO may have been preempted mid-handler on a PRIOR tick — is the
   guard per-thread or global, and does that matter when preemption is only at
   EL1h with I masked in-handler)?
2. First-switch bootstrap: switching INTO a freshly created thread for the first
   time — its fake frame has elr=trampoline, spsr=EL1h-I-clear. After `mov sp,x0`
   + restore + eret, does control correctly arrive at trampoline with a valid SP
   and I unmasked? What exact gpr/elr/spsr/sp must the fake frame hold so the
   FIRST eret into it is correct (x86 parallel: the fake iretq frame)?
3. Frame validation (D2): is "16-aligned + inside a known thread stack +
   spsr.M==EL1h + I clear" SUFFICIENT? What about: the frame ptr must equal
   stack_top-304 for a never-run thread but be anywhere-in-stack for a preempted
   one; could a validated-but-wrong frame (right stack, wrong offset) still
   corrupt? How do we bound "inside a known thread stack" cheaply (a table of
   [base,top) per live thread)?
4. The neutral core (D1): is frames-as-void* + the 3-hook vtable the right seam,
   or is something missing (e.g. the core needs to know stack bounds for
   validation, or the idle/wfi behavior, or how exit/ZOMBIE interacts with the
   RR pick never selecting a ZOMBIE)? Is there a hosted-test angle (the core is
   pure C over void* frames + hooks — can a host test drive it with fake frames
   and assert the RR order, SLEEPING sweep, preemption counting)?
5. Preemption-only correctness (D3): exit->ZOMBIE-then-wfi and sleep->SLEEPING-
   then-wfi rely on the NEXT tick preempting the wfi'd thread. Is there a race
   where a thread sets ZOMBIE/SLEEPING, and a tick arrives BEFORE the wfi, or the
   thread is re-selected before the state change is visible? With I masked in the
   handler and the state write before wfi, is it safe? What makes the RR pick
   skip ZOMBIE/SLEEPING deterministically?
6. Guard page (D4): is the TLBI VAE1IS + DSB sequence correct for a single-page
   unmap on this single-core target (vs the broadcast …IS variants — is IS
   needed on one core)? Any hazard unmapping a page whose stack is in active use
   by ANOTHER thread? Is clearing just the L3 leaf enough, or must intermediate
   tables be touched?
7. The proof (D7): does per-thread-distinct-pattern + NZCV + frame-swap mutation
   actually close the x86 gap? Is a canary loop PER THREAD (each thread runs its
   own) the right structure, and can the monitor thread observe all of them? Name
   any false-success path: e.g. could two threads with distinct patterns still
   pass if the scheduler never actually switched between them (only idle<->one
   worker)? How do we PROVE both workers ran AND were preempted (not just one)?
8. Frozen-contract / scope: D2 edits the frozen PR-2b vectors.S again + reverses
   its SP rationale, and changes the je_arm_irq signature (trapframe.h:70). Is the
   C-side validation a sufficient mitigation of the reversed rationale? Is
   anything here touching tested sched.c (it must NOT this PR)? Is the PR the
   right size or should it split (e.g. sched_rr.c core + hosted test first, then
   the ARM switch + proof)?

Answer each number. Flag owner escalations (frozen vectors re-edit, the SP-from-
frame rationale reversal, any sched.c touch). If a decision is wrong, say so and
give the correct one. Peers: you are at max reasoning effort — be exhaustive on
the switch asm and the race analysis.
