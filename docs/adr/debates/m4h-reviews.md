# M4 PR-H (device/IRQ3/TSS I/O bitmap, ADR 0010 D9) cross-lineage reviews

ADR 0000 part 4 attachment for PR-H. AI-assisted security review across two
lineages; NOT independent human qualification.

## Design review (before code)
Both lineages confirmed the core D9 mechanism sound on one-CPU QEMU and caught
four must-fix holes, folded into the design + owner-approved ADR 0010 D9 notes:
H1 the TSS bitmap is .bss zero-filled and a zero bit ALLOWS, so it must be filled
0xFF before ltr (packed wrapper, exact static asserts); H2 generation-exhaustion
sentinel; H4 a REAL IRQ7 (ISR bit 7 set) must still be EOI'd (only a spurious one
is not) + mask-IRQ3 on spurious to keep the <=1/tick bound; and two tested-teardown
integration points (close-event snapshot; a DEVICE branch in the process-death
release walk; the VM teardown callback carrying process identity) — E1. Plus the
supervisor DEVICE admin via startup n=2 — E2. E1/E2 owner-approved with ADR notes.

## Code review (Codex-authored impl) — blockers
Both lineages confirmed the mechanism holds in the code: bitmap writes touch ONLY
byte 0x5F (via je_arch_tss_set_com2_io), filled 0xFF before ltr, limit 200,
terminator never cleared; the dispatch predicate grants only to the exact live
owner; revoke ordering under all four triggers is correct and re-entry-safe; assign
is transactional; the <=1/tick bound holds by construction; DEVICE handles can't
spread; IOPL stays 0. Note: Codex's "does not compile / patch not present"
(Blocker 1) was a DIFF-VISIBILITY artifact — the review diffs omitted abi.h/arch.h/
svc.h/image.c/Makefile; the wiring IS present (UART image id 9, arch decls, the
build) and mode 14 builds + passes. Real blockers acted on in the fix pass:

- **B1 (both): mode-14 isolation is NOT decided from fault reason.** The corroboration
  counts aggregate driver deaths, not DEATH_FAULT/vector-13 per image. Claude BUILT
  a mutant that changed the forbidden-port probe to the ALLOWED port 0x2F8 — the
  driver exited cleanly and mode 14 still passed. Require exact
  m4f_death_count[UART][DEATH_FAULT]/[DEATH_EXIT] per mode and the #GP vector; make
  the binding-gen check exact (== not >=).
- **B2 (both, most privilege-sensitive): revoke-then-deny is never OBSERVED.** Claude's
  mutant deleted the je_arch_tss_set_com2_io(false) from revoke and mode 14 still
  passed, because the final "byte==0xFF" check is satisfied by any non-owner
  dispatch. The driver must do a real inb(0x2f8) AFTER revoke/self-close and that
  must fault (DEATH_FAULT vector 13), kernel-observed.
- **B3 (Codex): teardown can close the DEVICE handle WITHOUT revoking.** The release-walk
  DEVICE branch raw-closes and ignores `owner`; a direct vm_s3_destroy (bypassing
  mark_dying) leaves d->active + IRQ/pending state live. And je_device_teardown_check
  checks for a surviving SLOT, but the leak is the opposite (slot gone, active
  true). Revoke-before-close in the teardown callback using the owner identity, then
  assert !active && owner==NONE && !pending && !rearm && IRQ3 masked.
- **B4 (both): the rep-insb probe uses fixed DX=0x2ff — never crosses into 0x300.**
  Use separate inb(0x2ff) and inb(0x300), or genuinely increment the port.
- **B5 (both): mode 14 permits ZERO IRQ3 deliveries (vacuous <=1/tick).** Require
  irq3_deliveries >= 1, the pending transition observed, and je_arch_irq3_is_unmasked
  ()==false after revoke; the integration test must parse the count, not just the
  prefix.
- **B6 (Codex): a REAL IRQ7 must EOI unconditionally.** The current `if (isr&0x80){ if
  (irq7_unmasked) eoi; else panic; }` can turn a real IRQ7 into a halt; EOI
  unconditionally once ISR says real. The spurious-path mask of IRQ3 should be
  unconditional w.r.t. IRQ3's hardware mask (mask whenever unmasked; re-arm only if
  a binding is active).

Should-fix: a hosted unit test of device.c (generation boundary assign at
MAX-3/MAX-2, revoke completes; the IRQ7 branches; stale-ACK H_STALE + timer
gen-mismatch drop; assign refusal codes) so the generation-retirement and IRQ7
logic are not left "untested". L2 hardening: kep_teardown should panic if the
closing table is the active driver's, independent of the owner identity.

Honest gaps the worker flagged and both reviewers accept as recorded (to be closed
by the hosted test, not left as a security claim): real/spurious IRQ7 QEMU
injection and the generation-retirement boundary.

Verification after fixes: see WORKER_NOTES + the PR. Local only; remote CI on the PR.
