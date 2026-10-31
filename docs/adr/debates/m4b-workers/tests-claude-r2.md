# WORKER_NOTES — M4 PR-B test writer, round 2 (#53, ADR 0010 D3/D4/D-DEV)

Observed 2026-10-31 (UTC, local worktree), branch `wip/m4b-tests`, base commit
`5c5a3c2` (lead's header pins). Uncommitted. Header-only authorship: I read
`include/jeios/*.h` (proc.h, endpoint.h, abi.h, syscall.h), the lead's pin commits
(`git log -2 -p -- include`), and `/tmp/m4b/f-codex.md`. I did NOT read
`kernel/core/proc.c`, `endpoint.c` or `handle.c` bodies. No header, Makefile or git
config edited; nothing committed or pushed.

## State

- implemented: tests for items 1-4 of the round-2 brief (below).
- tested: compiles `-Werror` clean in the real Makefile build; run against the
  ROUND-1 implementation in this tree -> pinned-behaviour assertions FAIL as
  expected (full list below). test_syscall and test_proc_retire PASS.
- NOT verified: the tests against the round-2 implementation (not in my tree;
  the lead must run them on the integrated tree).

## Files changed (tests/*.c only)

- `tests/test_syscall.c` — op 15 clean decode must return exactly
  `JE_SYSCALL_PROC_STATUS` (both handle 0x00010000 and 0xffffffff); stale
  "syscall.h does not yet name..." comment replaced. Kills the Codex "wrong
  action 13" survivor.
- `tests/test_proc.c`
  - re-link of a linked LIVE record (different link and identical link) ->
    INVALID, no change (replaces the round-1 "re-link allowed" assertion, which
    the pin reversed).
  - abandon on a LIVE, run-linked record with refs 0 / charge 0 -> INVALID, no
    change, still LIVE, used_count unchanged (kills the abandon-guard survivor).
  - on_dying leaves thread_slot/vm_index intact; on_reaped zeroes
    thread_slot AND vm_index, both on the DEAD-with-ref path and the
    immediate-free path (link 9/13 so a zero is distinguishable) (kills the
    reap-field survivor).
  - reason JE_PROC_REASON_MAX+1 and 0xffffffff on LIVE -> INVALID, no change;
    `JE_PROC_REASON_MAX == 3u` asserted.
  - charge on DYING and on DEAD with room -> INVALID, no change; uncharge of the
    same identity still works afterwards.
  - new `test_null_pool`: NULL pool for alloc (with and without out), valid,
    abandon, link, on_dying, on_reaped, retain, status, charge -> INVALID / false;
    release, uncharge, pool_init -> no effect on a live pool held alongside. Ids
    tried: a live id, a free id, JE_PROC_ID_NONE.
- `tests/test_proc_retire.c` — new `test_retire_during_failing_search`: pool with
  every slot non-FREE except two FREE last-generation slots (0, 5) and one slot
  retired earlier in a successful search (10); non-FREE records include DYING
  linked, DEAD with a ref + UINT64_MAX code, LIVE linked + charged + 2 refs. A
  NOMEM alloc must yield `*out = NONE` and a pool equal FIELD-BY-FIELD to the
  snapshot with exactly `rec[0].retired = rec[5].retired = true`; used_count
  unchanged and equal to the non-FREE count. A second NOMEM changes nothing; a
  freed non-retired slot is allocatable again. Also added "used_count == non-FREE
  count" to the all-retired check.
- `tests/test_endpoint.c`
  - section 9: stale creator -> exactly `JE_EP_INVALID` (was `!= JE_EP_OK`).
  - section 11: with procs == NULL, records created by a non-NONE creator store
    creator NONE.
  - new section 14: six invalid non-NONE creators (never-issued gen, index ==
    REC_MAX, all-ones, gen 0 on a LIVE nonzero slot, future gen of a LIVE slot,
    abandoned identity) plus a never-allocated slot -> exactly `JE_EP_INVALID`,
    outs 0, endpoint pool / proc pool / table unchanged (memcmp).
  - new section 15: DYING and DEAD creator with room -> `JE_EP_INVALID`, no
    change; its LIVE-era channel still uncharges it once on destruction.
  - new section 16: channel created while detached stores NONE; after
    re-attaching, destroying it leaves the creator's real charge (from a charged
    channel) untouched; destroying the charged one then uncharges to 0.

## Commands run and results

1. `ASAN_OPTIONS=detect_leaks=0 make test > /tmp/m4b-tests-r2.log 2>&1` -> rc=2.
   All test binaries compiled with the project's `-Werror` flags. PASS: core,
   boot, alloc, vm, uframe, syscall, handle, handle_rw. FAIL (abort):
   `tests/test_endpoint.c:391` (stale creator -> INVALID). test_proc,
   test_proc_retire, test_fdt not reached by `make` after that abort.
2. Diagnostic only, outside the tree: copies of the four suites in
   `/tmp/m4b-soft/` with `assert(` rewritten to a non-aborting `SOFT(` counter
   macro, built against this tree's (round-1) `kernel/core/*.c`, to list EVERY
   failing assertion instead of the first. Results:
   - test_syscall: PASS. test_proc_retire: PASS (round-1 already retires during a
     failing search and changes nothing else).
   - test_proc failures and the pin each checks:
     - :242, :244 re-link -> INVALID [proc.h link: "not already linked"]
     - :257, :259 reason > MAX -> INVALID [on_dying reason bound]; :261, :266,
       :271, :300 are cascades (the bad reason moved the record to DYING)
     - :555, :560 charge on DYING/DEAD -> INVALID [LIVE-only charging]; :562, :564
       cascades
     - :678 `je_proc_charge(NULL, NONE)` -> INVALID [NULL pool: "every
       status-returning op"] — see ambiguity B1; round-1 returns OK.
     - (the `:21` lines are the "no change" memcmp of the same calls)
     - abandon-on-linked and on_reaped field zeroing PASS on round-1 (they guard
       against regressions/mutants, not a current bug).
   - test_endpoint failures and the pin each checks:
     - :391, :527, :535, :550, :555 invalid/stale/DYING/DEAD creator ->
       exactly INVALID [endpoint.h step 2]; :528 and the :89-:92 memcmp lines are
       the "outs 0 / no change" checks of the same calls; :538, :559 cascades.
       A follow-up probe (printing each status) showed round-1 returns status
       value 14 (not JE_EP_INVALID) for 5 of the 6 `bad[]` creators, and returns
       JE_EP_OK, CREATING an uncharged channel, for `{index 1, gen 0}` — i.e. it
       appears to treat any gen-0 id as NONE, not only `{0, 0}`. That is
       fail-open quota bypass (a non-NONE invalid identity creates uncharged);
       the lead should confirm the round-2 impl compares the whole id to NONE.
     - :421, :423, :578 uncharged record stores creator NONE [step 4]; :96, :584
       the resulting wrong uncharge after re-attach (round-1 decremented the
       creator's real charge: Codex P2-4 reproduced).
3. `git diff --check` -> clean.
4. Status probe: rebuilt the endpoint soft copy (`/tmp/m4b-soft/e2`) printing
   each `bad[]` status -> bad[0,1,2,4,5] -> 14, bad[3] `{1, 0}` -> 0 (JE_EP_OK, c0 =
   65538, a channel was minted).

## Decisions (why + rejected alternative)

- D1. op 15: assert only `== JE_SYSCALL_PROC_STATUS`, not its numeric value, and
  not that handle 0 / NULL out decode OK. Why: abi.h pins neither; round-1
  over-pinning was a review finding. Rejected: also asserting the enum value 15.
- D2. DYING + out-of-range reason: accept OK or INVALID, require no change. Why:
  proc.h does not order "already DYING -> OK" against "reason > MAX -> INVALID";
  both readings preserve "first reason wins". Rejected: pinning one order.
- D3. charge on DYING/DEAD tested with room only. Why: at-limit + DYING is
  unordered (DENIED vs INVALID) in the header. Rejected: guessing.
- D4. NULL pool + JE_PROC_ID_NONE for charge -> INVALID (literal "every
  status-returning op"). Rejected: treating NONE's "charge returns OK" as
  overriding. Flagged B1 because round-1 disagrees and endpoint create_by with
  procs == NULL never calls charge anyway, so either answer is safe for D-DEV.
- D5. `je_proc_pool_init(NULL)` called and required to do nothing (literal "void
  ops do nothing"). Rejected: skipping it as "caller bug". Flagged B2.
- D6. Retire test compares records field-by-field, not memcmp of an edited copy.
  Why: struct padding is unspecified; a field-wise expected-vs-actual makes the
  "exactly these two retired bits changed" claim precise. Rejected: memcmp.
- D7. Retire test requires BOTH passed-over slots to be retired. Why: the header
  says the search "continues" and marks such slots retired. Rejected: accepting
  an early-exit search that retires only the first.
- D8. Endpoint gen-0 invalid creator uses a NONZERO index (Z occupies 0). Why:
  `{0, 0}` is JE_PROC_ID_NONE (uncharged, OK) — my first draft had this bug and I
  fixed it before running.

## Ambiguities (flagged, implemented the literal reading)

- B1. `je_proc_charge(NULL, NONE)`: NULL-pool rule (INVALID) vs NONE rule (OK).
- B2. Whether "void ops do nothing" on NULL includes `je_proc_pool_init`.
- B3. Status order for on_dying(DYING/DEAD, reason > MAX) — see D2.
- B4. Status order for charge(DYING/DEAD, at limit) — see D3.

## Not covered / not enforced by these tests

- Kernel glue (endpoint_kern.c user-caller fail-closed, Codex P1-1), reaper
  ordering / negative reaper result (P1-3), automatic idle-loop reaping (P2-5):
  outside tests/*.c hosted scope; need kernel/fixture changes and smoke.
- No `make smoke`/integration run (not boot-affecting from my files).
- AI-assisted test authorship; not independent human review.
