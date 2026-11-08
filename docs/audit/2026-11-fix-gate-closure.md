# Fix-gate closure ledger — F01–F14

Observed 2026-11-06. This is the closure ledger the fix-gate
(`docs/audit/2026-11-fix-gate.md`) requires before the roadmap reopens. It records
each F-row's disposition, the implementing commit/PR, the evidence location, and
the exit-criteria status. **AI-assisted work, not independent human
qualification.** ARM paging/scheduler, endpoint/IPC and VM teardown are tested
paths and remain owner-review gated; owner-gated contract changes (F07, F08) were
owner-approved on 2026-11-06, and the four deferred F06 decisions were dispositioned
by the owner on 2026-11-06 (my recommendations, with flex to revisit).

## Per-row ledger

| F | Disposition | Where | Evidence |
|---|---|---|---|
| F01 | FIXED — mutation runner rewritten (baseline preflight, isolation, timeouts, schema validation, no compile-as-kill); the false-green defect is gone. | PR #94 `0d94a44` | `evidence/2026-11-F01-F02.md` |
| F02 | FIXED — `test_sched_killall` prerequisite; NDEBUG guards on 14 tests; container build isolation + soak validation + reproducibility honesty. | PR #94 `0d94a44` | `evidence/2026-11-F01-F02.md` |
| F03 | FIXED — `je_sched_reap_dead` returns negative on violation; `je_arch_irqs_enabled` seam; 5 reaper mutants. | PR #97 `4d8ee14` | `evidence/2026-11-F03.md` |
| F04 | FIXED — ARM audit two-pass restructure (validate every table address before deref); 6 ARM-audit mutants; foreign-sibling SEGV closed. | PR #98 `c3f3720` | `evidence/2026-11-F04-F06.md` |
| F05 | FIXED — usercopy NULL-kernel-buffer + nonnesting guards; new `test_usercopy`; 3 mutants. | PR #98 `c3f3720` | `evidence/2026-11-F04-F06.md` |
| F06 | PARTIAL+DISPOSITIONED — defensive subset FIXED (reserve-status, paging ctx bounds, ARM overflow parity, backend NX, inactive-root assert, guard-restore owner); the 4 contract items dispositioned (below). | PR #98 `c3f3720` + this PR | `evidence/2026-11-F04-F06.md`, this doc |
| F07 | FIXED (owner-approved) — `JE_EP_UNWIRED` split; internal CORRUPT fail-stops; a cross-review-caught user-reachable CORRUPT re-routed to DENIED. | PR #99 `0d8d40c` | `evidence/2026-11-F07-F11.md` |
| F08 | FIXED (owner-approved) — explicit one-thread-per-process. | PR #99 `0d8d40c` | `evidence/2026-11-F07-F11.md` |
| F09 | FIXED — vm-audit negative test per class; device authority coverage; 2 gap mutants enabled. | PR #99 `0d8d40c` | `evidence/2026-11-F07-F11.md` |
| F10 | FIXED — recv staging clamp; image.c uintptr span; hosted image-registry test; mutants (1 justified equivalent). | PR #99 `0d8d40c` | `evidence/2026-11-F07-F11.md` |
| F11 | FIXED — ARM late-register canary + stalled-timer marker; oracle negative profiles. | PR #99 `0d8d40c` | `evidence/2026-11-F07-F11.md` |
| F12 | FIXED — 49→0 core warnings (externs→headers, missing prototypes, redundant decls); SPDX headers; stale comments; FDT double-zero; tick-global encapsulation. Device-observation split + dead-param removal DEFERRED (would touch tested layout). | this PR | `evidence/2026-11-F12-F14.md` |
| F13 | PARTIAL — spawn observer decoupled from the failable harness ledger (safe mechanical). The main.c physical fixture split is DEFERRED (owner-gated note). The pre-MP3 lifecycle decision is recorded as **ADR 0014** + an ADR 0013 trigger amendment (owner-gated, deferred implementation, pre-MP3 blocker). | this PR | ADR 0014, `evidence/2026-11-F12-F14.md` |
| F14 | FIXED — this ledger; deterministic-corpus/coverage disposition (below); available compiler-analysis disposition (below). | this PR | this doc |

## F06 owner dispositions (owner-approved 2026-11-06, flex to revisit)

1. **`vb_flush` invalidation semantics → option (a): keep the no-op, assert the
   precondition.** Boot now explicitly clears CR4.PGE and CR4.PCIDE in
   `je_arch_cpu_policy_init` (trap.c), tying the no-op's correctness (single-BSP,
   CR3-reload-flushes) to an enforced invariant rather than an assumption. No PCID/
   SMP work; scoped invalidation deferred. Flex: revisit when PCID/SMP arrives.
2. **Zeroization → keep the distributed zero-before-expose policy (no-change,
   documented).** Every user/page-table exposure zeroes on build/link and VM frees
   scrub; no demonstrated leak. Not collapsed to a single enforcement point. Flex:
   collapse later if a new owner path appears. (Rationale + the per-owner scrub
   map are in `evidence/2026-11-F04-F06.md`.)
3. **Boot-block RAM profile → do not tighten (no-change, documented).**
   Loader-owned reserved (non-type-1) RAM is permitted for the boot block (the GRUB
   multiboot2 profile loads no modules); the contract is "the block must be
   accessible via the bootstrap identity map, not necessarily in a type-1 RAM
   entry." The existing image-in-RAM check is retained. Flex: revisit if a
   modules-in-non-RAM loader profile is adopted.
4. **No-NX console line → reworded (small change applied).** The unconditional
   "M1: protected mappings and stack guards ready" now states the guarantee
   unambiguously: "(W^X enforced)" with NX, and "(DEGRADED: NX absent, kernel
   data/heap/stack executable, user VM refused)" without. The `M1: protected
   mappings` prefix is retained so `smoke.py`/`integration.py` still match
   (verified: NX-present and no-NX smoke both pass). Flex: wording may change.

## F13 deferred items (owner-gated)

- **main.c physical fixture split** (3874 lines, 44 `JE_TEST_MODE` blocks): the
  gate defers this mechanical move until after the defect fixes; recorded as a
  deferred, owner-gated follow-up. Not done this PR.
- **Pre-MP3 lifecycle extraction**: recorded as ADR 0014 (proposed/owner-gated),
  with an explicit migration trigger and kept as a **pre-MP3 blocker**;
  implementation deferred pending owner approval. `sched.c`/`sched_rr.c` untouched.

## F14 assurance/coverage disposition

- **Deterministic corpora:** FDT retains its exact-size/truncation/independent-
  builder coverage; boot, VM-image (`je_vm_image_validate` + the new
  `test_image_registry`), and usercopy now have hosted negative/boundary tests
  added across F05/F09/F10. The mutant harness (29 named mutants, 27 killed /
  1 equivalent / 1 disabled gap) is the standing property check. Coverage-guided
  fuzzing, Clang/MSan/cppcheck lanes, TSan, SMP and hardware lanes remain
  separately-tracked enhancements (not closure conditions), per the gate.
- **Available compiler analysis:** the F12 warning sweep
  (`-Wmissing-prototypes -Wnested-externs -Wredundant-decls`) was run and driven
  to **0 for `kernel/core`**; ARM has 1 residual (noted). `-Wall -Wextra
  -Werror -Wconversion -Wshadow` is the standing build gate. clang/cppcheck/MSan
  are not installed in this environment (documented limitation, not a closure
  blocker). Coverage as a diagnostic only — never "100% == secure."

## Exit criteria status

- F01–F14 dispositions: **accepted** (fixed, or owner-dispositioned no-change, or
  owner-gated deferral recorded as a blocker).
- Confirmed defects repaired: reaper sign (F03), ARM audit deref (F04), usercopy
  NULL (F05), endpoint-corruption fail-stop (F07) incl. the cross-review DoS fix.
- Critical security-boundary killers enabled: alloc/handle/vm/fdt/reaper/usercopy/
  ARM-audit/vm-audit/device mutants — 27 killed, 0 survived, 0 blocked.
- No unexplained baseline failures/survivors; clean native + container builds.
- Full AMD64 (`make check` + `make integration`) and ARM (`make aarch64` + 11/11
  smoke matrix) at this branch's head.
- STATUS/HANDOFF: to be synchronized in this PR (see `docs/HANDOFF.md`,
  `docs/STATUS.md` updates).
- Residual tracked follow-ups (NOT closure blockers; owner-acknowledged):
  `GAP-EP-WAIT-INFLIGHT` killer; x86 trap-routing hosted seam; the `(uint8_t)hcap`
  recv truncation; the recv shared-helper refactor; F13's main.c split and the
  ADR 0014 lifecycle extraction (pre-MP3 blocker); device-observation-state split.

With F01–F14 dispositioned and the gates green, the repair-first gate is closed
and the roadmap (MP2 scheduling implementation, then MP3) may resume — subject to
the ADR 0014 pre-MP3 blocker and the standing owner gates. Mobile pin unchanged.
