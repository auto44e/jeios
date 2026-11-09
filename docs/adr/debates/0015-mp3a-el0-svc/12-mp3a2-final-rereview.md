# MP3a-2 final adversarial re-review (Claude, 56 QEMU boots) + fix pass #2

Reviewer: Claude (opus, max effort), empirical with fault injection, scratch copies
under `/tmp/mp3a2-final-rr/` (worktree never edited). Lead independently re-verified
every load-bearing claim and the two most important vacuity fixes by its own injection.

## Verdict: FIX-FIRST — the shipped CODE was confirmed correct; the gaps were proof-vacuity + honesty.

### Held up (confirmed load-bearing / correct)
- B2 lone-survivor gate: reverting the `arm_switch_to_ready` READY-before-pick demotion
  (`sched_arm.c:922-924`) → `FATAL sched no live EL0 successor` right after
  `LONE_SURVIVOR_YIELD count=1`; no PASS. Each gate condition on its own catches the
  revert; `smoke_arm.py` independently fails ("lone-survivor reap/yield markers missing").
  Neither counter can be bumped before reap, while a negative process is alive, or by a
  non-survivor slot.
- User-SP: DROP-SP-EL0 mutant → `FATAL el0-test user-sp` (matches killer text); restoring a
  constant arena_end also → `FATAL el0-test user-sp`. S3-a closed.
- CNTKCTL/DCC code behavior real; EL0PTEN=1 → `FATAL`; TDCC cleared → `FATAL`.
- No regression: vectors.S / trapframe.h / kernel/core unchanged since 08a0d98; all
  sched_arm.c changes inside `JE_ARM_EL0_TEST`. 14/14 ARM profiles, make check, diff-check
  clean. mutants 50 killed/0 SURVIVED/0 BLOCKED. 12/12 repeat boots (3–33 lone yields
  before the IRQ reselect).

### SHOULD-FIX (proof-vacuity) — all fixed in commit ffae7cc, each re-proven by lead injection
1. TPIDRRO_ROUND_TRIP was vacuous: nothing poisoned TPIDRRO before the entry scrub, so
   checking ==0 proved nothing (deleting both scrub writes still PASSED).
   FIX: poison `tpidrro_el0` in `je_arm_el0_test_poison_tpidr`; add mutant
   `ARM-USER-NO-TPIDRRO-SCRUB`. Lead re-proof: deleting `vectors.S:305` →
   `FATAL el0-test svc-proof-result`, PASS count 0.
2. Boundary grep `[)\]]` was wrong ERE — `(*tf).sp` and `tf[0].sp` slipped through.
   FIX: `[])]` + a three-form self-test in `check_arm_trapframe_boundary.sh`.
3. Two of four MODE_CHECK cases were vacuous (EL1h case rejected by SP/ELR anyway; M[4]
   redundant). FIX: case 2 is now an otherwise-valid EL1h frame tagged vector 9 so only the
   `vector_el0 && mode!=EL0T` discriminator can reject it (mutant `ARM-USER-MODE-VECTOR-EL0T`
   kills); the M[4] claim reworded as composite-covered, not an isolating probe.
4. New TRAP markers printed for ANY fault (incl. ec=0x0). FIX: `note_fault` now requires
   EC==0x18 + exact ISS before printing the CNTKCTL/DCC marker, else `proof_fatal`
   (mutants `ARM-USER-CNTKCTL-ISS-GUARD`, `ARM-USER-DCC-ISS-GUARD` kill).

### NITs (fixed)
- `sched_arm.c` label branched: DENIED → "hardening refusal", INVALID/cookie → "frame invalid".
- Corrected killer texts: NO-SP-EL1-INSTALL, MODE-CONFUSION, SVC-WRITES-X0.
- Removed stray root-level `MP3A2-BRIEF.md` / `MP3A2-REVIEW.md`; corrected the false
  "review files untracked" claim in HANDOFF/IMPLEMENTATION to cite the committed transcripts.
- Reworded "≥2 self-yields" to "≥1 completed lone self-yield reselect + ≥1 lone IRQ reselect;
  PASS fires before the 2nd lone-yield dispatch completes."
- Documented the `(!vector_el1 && !vector_el0)` range clause as not independently enforced.

### Effort
56 QEMU boots (1 baseline, 12 repeats, 29 injection/mutant, 14 profiles), 6 planted-form
boundary-grep runs, one `make mutants` (52 mutants), one `make check`.

## Post-fix independent verification (lead)
`make mutants` 55 killed / 1 equivalent / 0 SURVIVED / 0 BLOCKED / 1 skipped (incl. 5 new
mutants); clean raw boot PASS; ARM smoke PASS; x86 make check PASS; boundary self-test PASS;
diff-check clean. TPIDRRO-scrub-removal and mode-discriminator-removal both re-proven to FATAL.

Lesson recorded: a negative stage you never made fail is not a proof.
This is AI-assisted review, not independent human qualification.
