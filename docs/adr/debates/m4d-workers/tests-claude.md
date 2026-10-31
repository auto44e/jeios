# WORKER_NOTES — M4 PR-D (#53, ADR 0010 D8), TEST WRITER

Observed 2026-10-31T13:47:41+00:00. Worktree `wt-m4d-tests`, branch `wip/m4d-tests`, base
commit `c39b026` (pre-wiring + stubs). Nothing committed. AI-authored (Claude); not
independent review, not human qualification.

## State
- **Implemented:** new hosted tests in `tests/test_proc.c` and `tests/test_syscall.c` only.
- **Tested:** against the stubs (fail as expected, below) and against throwaway header-derived
  reference implementations in `/tmp/m4d-ref/` (not in the tree; pass). Not tested against
  the real proc.c/syscall.c being written in parallel — the lead must run that.
- **Not hostable:** `je_sched_kill` (see below).

## Changes
### tests/test_proc.c (appended; both run LAST in `main` so every pre-existing case runs first)
- `test_run_link`: OK with exact outputs for LIVE linked `{11,12}`, a `{0,0}` link (kills a
  "linked = nonzero fields" reader), large values `{0xfffffffe,0x80000001}`, DYING linked; the
  pool is memcmp-unchanged even on OK ("Pure"). INVALID with both sentinels
  (`0xa5a5a5a5`/`0x5a5a5a5a`) untouched and pool unchanged for: LIVE unlinked, DYING
  never-linked, DEAD (kept by a handle ref), FREE after release, stale after reuse (new
  incarnation reports the NEW link), future generation, abandoned, never-allocated, gen 0,
  NONE, index ≥ REC_MAX / 0xffffffff, NULL pool, each NULL out singly and both (the non-NULL
  out stays untouched), another pool.
- `test_is_supervisor`: true for a valid supervisor record (linked, DYING, DEAD); false for an
  ordinary record, FREE, reuse by a non-supervisor (old + new id), **stale ordinary id whose
  slot is reused by a supervisor** (kills a slot-keyed check), gen 0 / future gen, abandoned,
  never-allocated, NONE, out of range, NULL pool, other pool. Pool unchanged.
### tests/test_syscall.c
- New `test_proc_kill_decode`: OK + `JE_SYSCALL_PROC_KILL` + RDX=0 for handles 0, 1,
  0x10000, 0x7fffffff, 0x80000000, 0xffffffff. INVALID + RDX=0 + `JE_SYSCALL_RETURN` for every
  single bit of RDI[63:32] (with and without low bits), four wide patterns, every single bit of
  each of RSI/RDX/R10/R8/R9, plus 1/top-bit/all-ones/pointer values with low and max handle,
  all-dirty combos, and a PROC_STATUS-shaped call (RSI = out ptr) on op 16. Op aliasing
  `0x1_00000010` and `0xffffffff_00000010` → UNSUPPORTED. Op 15 still → PROC_STATUS.
- **Retired assertion:** op 16 removed from the "later ops UNSUPPORTED" list (HISTORY comment
  kept). 17–20 still UNSUPPORTED for clean, dirty, and the exact valid-kill arg shape.
  PASS banner updated.

## Commands and results
1. `ASAN_OPTIONS=detect_leaks=0 make test` → rc=2. Build warning-clean (full `$(WARN)` set).
   core/boot/alloc/vm/uframe PASS; aborts at test_syscall (make stops there).
2. Each binary run individually (`./build/0/test_*`):
   - PASS: test_core, test_boot, test_alloc, test_vm, test_uframe, test_handle,
     test_handle_rw, test_endpoint, test_proc_retire, test_fdt.
   - **FAIL test_syscall** `tests/test_syscall.c:193 kill_ok` — op 16 does not decode to
     `JE_SYSCALL_PROC_KILL` → the **op-16 decode stub in syscall.c** (still UNSUPPORTED).
   - **FAIL test_proc** `assert_run_link_is` (first call, LIVE linked) — **`je_proc_run_link`
     stub in proc.c** returns non-OK. All pre-existing test_proc cases ran and passed before it.
     `je_proc_is_supervisor` is never reached against the stub because assert aborts first.
3. Reference check (`/tmp/m4d-ref/`, compiled with the same warnings + ASan/UBSan; the stubs
   renamed away with `-D`, so I did not read the real/stub bodies):
   - `ref.c` (run_link/is_supervisor written from proc.h) + test_proc → PASS.
   - `sref.c` (op-16 decode front-end from abi.h, delegating other ops to syscall.c) +
     test_syscall → PASS.
   - Mutants for test_proc: (1) slot-keyed is_supervisor — killed; (2) linked = nonzero
     fields — killed; (3) outputs written before the validity check — killed; (4) no
     LIVE/DYING state check — **survived at first** (equivalent under the proc.h invariant),
     killed after adding the white-box forged-record check (decision D3).
4. `git diff --check` → clean.

## Decisions
- **D1 New tests run last in `main`.** Why: assert aborts the binary; placing them last means
  every pre-existing case is observed passing against the stubs. Rejected: adding run_link /
  is_supervisor to the shared `assert_rejected_everywhere` helper — broader coverage, but the
  stub would abort `test_valid` early and hide whether old cases still pass. The new tests
  cover the same stale/never-allocated/NONE ids directly instead.
- **D2 is_supervisor on DYING/DEAD supervisor → true.** Literal reading of proc.h ("valid" =
  non-FREE with that generation). See ambiguity A1.
- **D3 White-box forged "DEAD + run_linked" record.** Why: without it, a run_link that ignores
  the state is indistinguishable (mutant 4). It sets fields the header says only on_reaped
  writes, then restores them. Rejected: leave the mutant alive and note it. A reviewer may
  find the poke outside the contract; it is labelled in the test and easy to drop.
- **D4 OK result asserted RDX = 0** (abi.h op 16: "RDX = 0"), same as the op 15 test.
- **D5 handle 0 / max handle decode OK** — abi.h only requires RDI < 2^32; resolution is the glue's.

## Header ambiguities (flagged, not resolved)
- **A1** `je_proc_is_supervisor`: "valid" covers DYING/DEAD, so a dead supervisor record is
  still "the supervisor". That is harmless for je_sched_kill (step 3 returns OK for DYING/DEAD
  before step 4), but a future caller might read it as "a live supervisor". Tests pin the
  literal reading.
- **A2** Check order differs between `/tmp/m4d/map.md` and the frozen `sched.h`: the map puts
  "not LIVE → INVALID" and the supervisor check after DYING/DEAD; sched.h step 1 is "record
  invalid" and has no separate "not LIVE" step (FREE ⇒ invalid; the other states are covered).
  The map's step 5→6 also differs (supervisor before run-link). I took sched.h as authoritative.
  It is not testable on the host anyway.
- **A3** `je_proc_run_link` "Any NULL pointer … outputs untouched": tested for each NULL out
  singly. The header says "outputs" plural, so the non-NULL one must also stay unwritten.
  An impl that wrote `*thread_slot` before checking `vm_index` would fail — intended.

## je_sched_kill is kernel-only (not hostable)
`je_sched_kill` lives in sched.c (scheduler thread table, `mark_dying`, IF=0, current-thread
identity). There is no hosted scheduler harness, so this PR adds **no hosted test** for it.
Its evidence must be the QEMU fixture M4d-s1 (READY, BLOCKED_IPC, WOKEN-reserved victims,
self-kill INVALID, kill-after-death OK with reason KILLED kept, WRONGTYPE/DENIED/STALE), run by
the lead via `make smoke` (rebuilds the ISO; do not run tools/smoke.py on a stale image).
I did not run `make smoke` (TEST WRITER scope: tests/*.c only; no fixture in my files).

## Not verified
- Behaviour against the real parallel implementations of run_link/is_supervisor/op-16 decode.
- `make check`, `make smoke`, QEMU, CI-profile, ARM: not run by me.
- The subcase where the reserved register is set together with a wide RDI is checked only in
  the all-dirty combo.
