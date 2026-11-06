# ADR 0013 design review — transcript and resolution

Two-lineage design review at MAX effort (owner directive), BEFORE any scheduling
code, 2026-11-05. ADR 0000 part-4 attachment.
- 00-shared-brief.md — the 8-question brief (verified ground truth + the draft).
- 01-codex-implementor-eye.md — Codex (xhigh): the switch asm walk, frame
  validation, the stacks-not-mapped defect, vtable gaps, the full 8 questions.
- 02-claude-adversarial-q5-q7.md — Claude (opus): a focused deep read of Q5
  (preemption races / wfi discipline) + Q7 (proof false-success). [The first
  Claude run at --effort max over-ran a 20-min timeout with zero output on the
  full 8-question brief; re-dispatched at --effort high on a tight 2-question
  brief — the lesson is recorded in memory + the peer-agents steering.]

## Findings folded into the accepted ADR 0013

Both lineages converged on the core gaps; each added unique depth.

D2 (switch + validation):
- The `add sp,#304` lands at the context's SAVED interrupted SP, not "the stack
  top" (draft bug; true for fresh threads, wrong for preempted). — Codex Q1.
- Frame validation must also require `frame->sp == ptr+304`, ELR in approved
  text/trampoline, SPSR IL/reserved clear, and crucially `ptr == the EXACT frame
  recorded for that thread` (a right-stack/wrong-offset frame is rejected). The
  3-hook vtable was too weak (needs the owning context + bounds + phase). — both.

D3 (preemption-only):
- CRITICAL: a `wfi` loop that keeps I masked spins forever — WFI wakes on a
  pending IRQ but the IRQ is never TAKEN; the sleep loop must `daifclr;isb` after
  wfi; the exit loop runs I-clear; a masked exit = FATAL. — Claude Q5d.
- state/wake must be aligned own-word volatile/atomic (not bitfield — RMW loses
  the handler's write); sleep writes deadline-then-state (or masks I across the
  pair); reject n==0, bound n for overflow; handler sets READY only if RUNNING;
  cannot reap the stack holding the current tf. — Claude Q5a-c, Codex Q5.

D4 (stacks): REAL DEFECT — the static MMU map does NOT cover allocated
je_arm_pages frames, so a thread stack is unreachable; unmapping the guard
doesn't map the other 4 pages. Need a checked runtime map+unmap API (TLBI
VALE1IS). — Codex Q6.

D7 (proof false-success — the biggest value):
- A missing `mov sp,x0` passes ALL canaries + global floors while one thread runs
  throughout → kill with `observed_running[s]` stack-extent FATAL at IRQ entry.
- Global floors pass with idle↔ONE worker → need PER-SLOT scheduler-side counters
  (observed_running/preempted_in_canary/resumed/verified_resumes/interleave/
  starts==1/dispatches_after_exit==0).
- Identity must be scheduler-owned (volatile je_sched_rr_current, rodata pattern),
  never read from the swappable frame (else a swap self-passes).
- NZCV needs a flag-preserving window + specific patterns (avoid 0x6 = the
  handler's cmp residue). Three frame-swap mutation variants each with a named
  detector. — Claude Q7, Codex Q7.

## Owner escalations (recorded; all within the already-approved scope)
The frozen-vectors re-edit (mov sp,x0) + the reversed "never load SP from frame"
rationale (mitigated by full C-side validation, but NOT equivalent to preserving
the invariant — a C bug can still install an arbitrary SP); the je_arm_irq
signature change (trapframe.h); runtime page-table mutation for stacks; NO
sched.c touch this PR. Codex recommends splitting into: (1) sched_rr.c + hosted
tests, (2) ARM stack map/guard/TLBI, (3) the vector switch + first-switch
bootstrap, (4) the multi-worker proof + mutation oracle.

Nature: AI-assisted design review at max effort; NOT independent human
qualification. Two DDI0487 points to confirm in-code: WFI wake under PSTATE.I
mask, and the daifclr+isb take point.
