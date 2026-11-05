# PR-2a cross-lineage code review — transcript and resolution

AI-assisted cross-lineage review of MP2 PR-2a (aarch64 GICv2 + CNTP timer,
POLLED, IRQs masked). Codex implemented gic.c + main.c; Claude wrote the hosted
test + smoke oracle; each lineage reviewed the OTHER's output.

- 01-claude-kernel-review.md — Claude on the kernel (gic.c/main.c/mk).
- 02-codex-tests-review.md — Codex on the tests + oracle.

## Kernel fixes applied by the lead (both reviews converged)

1. ISB before CNTPCT_EL0 read (Claude #3 / Codex #3) — the "sample before re-arm"
   ordering the delta>=interval bound relies on was only program-order; now
   architectural (matches Linux __arch_counter_get_cntpct).
2. Read GICD_TYPER/IIDR/GICC_IIDR BEFORE the first GICD_CTLR write (Claude #4) —
   identify before touching an unverified device.
3. je_gic_handle_one on ISTATUS==0 returns JE_ERR_BUSY (was JE_ERR_IO) and does
   NOT bump ticks (Claude #2) — a benign re-pend race, not fatal; matters when
   PR-2b reuses handle_one from the IRQ path.
4. je_gic_handle_one rejects interval>INT32_MAX up front (Claude #6).

## Test/oracle hardening applied (Claude, the gaps Codex + Claude found)

A. CNTPCT-before-rearm ordering now asserted (fake advances the counter on
   re-arm; sample-after-rearm mutant fails). B. The fake MODELS the timer deadline
   (TVAL write actually re-arms / clears ISTATUS). C. Spurious "no EOIR" checks
   ALL widths + violations==0. D. Post-enable GICD_CTLR readback independently
   covered (separate fakes). E. One-shot IAR + exactly-one-read on all four paths.
   F. violations==0 asserted every case + a detector self-test. G. 300GHz CNTFRQ
   asserts the exact masked outcome (no longer "either"). H. Smoke adds AT/PAR
   Device probes for GICD/GICC + an unmapped-GICC_DIR probe (kernel emits
   mmu xlate gicd/gicc/gicc_dir + halts on a bad mapping). I. group-0 + priority
   0x80 checked. J. full DTB timer-flags (0x104) validated.

Hosted test now 1503 checks; mutation battery 22/23 hosted + 12/12 oracle caught.

## Honestly-recorded residuals

- One benign mutant survives: reporting ipriority30 as the constant 0x80 instead
  of the readback (init already refuses any non-0x80, so a good init always
  reports 0x80 — the oracle's prio30 check guards only the marker).
- The kernel's new halt-on-bad-GIC-mapping branch is not boot-exercised (oracle
  side mutated only).
- Three init refusals share JE_ERR_IO (distinguished by the access log).
- The fake models the GICv2 spec, not QEMU/real-HW exactly; the live QEMU smoke
  covers the real device.
- A late-refusal (PMR/GICC_CTLR readback) leaves GICD/INTID 30 enabled — harmless
  while IRQs are masked and the kernel halts; noted, not rolled back.

Nature: AI-assisted review + lead mutation testing (FIQEn, spurious-EOIR caught).
NOT independent human qualification. Polled proof, IRQs masked; QEMU TCG models
no real interrupt latency or caches.
