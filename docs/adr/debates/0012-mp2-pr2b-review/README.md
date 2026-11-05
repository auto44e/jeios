# PR-2b cross-lineage review — transcript and resolution

AI-assisted review of MP2 PR-2b (aarch64 returning EL1h IRQ path). Codex
implemented the frozen-asm surgery + je_arm_irq; Claude wrote the --timer-test
oracle + reviewed the asm from the test side; Codex did the final adversarial
pass. The LEAD applied the fixes.

- 01-claude-test-and-asm-review.md — Claude's oracle + asm review (found the two
  canary-proof gaps).
- 02-codex-final-review.md — Codex's final adversarial review.

## Asm confirmed sound (both lineages)
Slot-5 stub spills the REAL x16-x19 before scratch; slots 0-4,6-15 + the fatal
common saver + double_fault byte-identical; vector=5 immediate; transient guard
set-after-save / cleared-before-eret (a sync fault in je_arm_irq takes the fatal
path — proven by the BRK control); add-sp unwind (never ldr sp); SPSR.M==EL1h
check; full GPR restore + eret. Verified on QEMU: 4 vector-path ticks, BRK
control, x16-corruption caught (FATAL canary mismatch reg=16).

## Fixes applied by the lead
1. Canary proof (Claude #1,#2 / Codex #1): added a final I-masked x0..x28 check
   pass (closes the "corruption on the Nth return never re-checked" gap AND
   covers x16-x18 race-free). x29/x30 cannot be checked by the PRIMARY loop (they
   are its scratch/LR — a check would read a saved-good copy = false proof).
   >>> OWNER DECISION (2026-11-05): do BOTH (a)+(b). (a) is the primary x0..x28
   loop above; (b) a SECOND je_arm_canary_hi_loop seeds x29,x30, uses the
   already-proven x0..x2 as scratch, and checks x29,x30 over a few more live
   ticks + a final I-masked pass. Together they cover all 31 GPRs. Mutation-
   confirmed: x16=0-on-return caught by (a) (reg=16); x30=0-on-return caught by
   (b) (canary-hi reg=30) — the exact mutant (a) alone missed. ADR 0012 D8
   amended to describe both phases.
2. Unexpected-INTID now FATAL (Codex #2 / ADR D6): je_arm_irq emits a bounded
   FATAL + halt on an unexpected non-spurious INTID (only INTID 30 is enabled;
   gic.c already EOIR'd it). Was: silently counted.
3. CI (Codex #3): added ARM_TIMER_POLL + ARM_TIMER_TEST smoke steps and the
   tools/test_smoke_arm_timer.py oracle self-test to ci.yml.

## Recorded residuals
- ISTATUS==1 is proven by the PR-2a hosted test, not the --timer-test oracle
  (cross-PR coverage, recorded).
- je_arm_irq lives in main.c (no hosted seam); its OK/BUSY/spurious branching is
  proven only on QEMU. Moving it to irq.c would enable a hosted test (future).

Nature: AI-assisted review + lead mutation testing. NOT independent human
qualification. Vector-path proof on QEMU TCG — timing is a lower bound, no cache
model. Editing the owner-frozen vectors.S was owner-approved for this PR.
