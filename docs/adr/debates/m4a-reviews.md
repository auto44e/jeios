# M4 PR-A reviews (2026-10-31, AI-assisted; not human qualification)

Worker mode. The implementation was written by Codex and reviewed by Claude. The tests were written by Claude from the headers alone and reviewed by Codex. Lead: Kiro. Reviewer outputs are verbatim.

## Round 1 — Claude reviewing the Codex implementation

I found 1 P1, 3 P2 and 5 P3 issues. The P1 is a weakened M2 preemption check, and it changes previously tested code, so it needs the owner rather than an agent vote. I read the code only; I did not build or run anything.

**P1**

1. **The M2 preemption check is weaker than before** (`kernel/core/main.c:290`, from `main.c:266-272`). The old check needed at least `JE_THREAD_MAX*2` = 16 preemptions with 1 idle + 7 workers. The new check needs only `JE_M2_WORKERS*2u` = 14. It is also contaminated: `preemptions` (`sched.c:239`) counts every timer-driven switch, so timer switches into the 8 capacity probes count toward the 14. The test no longer proves "7 workers were preempted at least 16 times." The worker notes say the first try at `*2` = 32 failed, but they never record the observed count, so the safety margin is unknown.
   - Fix: run M2 exactly as before (7 workers, floor `(1u+JE_M2_WORKERS)*2u` = 16, which is the old value, then reap). Do the thread-capacity fill and NOMEM rollback as a separate phase after the reap, comparing deltas, with the probes created before the timer is turned on.
   - If the change is kept as written instead, print `je_sched_preemptions()` and record the margin. Either way, flag it to the owner.

**P2**

2. **The ring-3 out-of-range handle check is gone** (`main.c:674`). `0x1060` used to prove that index == capacity returns `BADINDEX` through the syscall path. Now it expects `STALE` on a free in-range slot, so no ring-3 test covers the `>= capacity` bound any more.
   - Fix: in `user_demo.S:378-380`, encode index `JE_VM_HANDLE_CAP` (pass it in with `.equ`, or a `-D` from the header, not a literal 40) and keep expecting `BADINDEX` at `0x1060`. Add the free-slot `STALE` probe at a new scratch offset.
   - `user_demo.S:378` ("capacity 16") and `:419` ("quota = CAP-1 = 15") are now false.
3. **Old smoke marker string is now false** (`main.c:273`). "M2: seven guarded threads ready" prints when 15 threads exist. The capacity fill and its NOMEM rollback also have no marker of their own, so `tools/smoke.py` cannot tell whether they ran.
   - Fix: add a separate marker such as "M2: thread capacity T=16 exhausted; rollback OK" and require it in smoke.
4. **The ref-count comment no longer holds** (`include/jeios/endpoint.h:76`). It says `handle_refs` is "bounded by table slots", but with `JE_EP_REF_MAX`=8, H=40 and P=8 one end can be duplicated or held more than 8 times. That returns `REFOVERFLOW`, which is a policy limit, not a structural one. This does not break M4, because each broker pair or readiness channel is held by one table.
   - Fix: correct the comment.
   - Add an assert or a documented note that D2's supervisor design needs at most `JE_EP_REF_MAX` handle refs per end.
   - Add a hosted test that `dup` past 8 returns `REFOVERFLOW` and changes no state.

**P3**

5. **No compile-time check on the default quota** (`vm.h:64`). Add `_Static_assert(JE_VM_HANDLE_QUOTA_DEFAULT >= 1u && JE_VM_HANDLE_QUOTA_DEFAULT <= JE_VM_HANDLE_QUOTA_MAX)`. Without it, the `je_vm_build` wrapper would silently return INVALID if the cap shrank.
6. **Leaked process slots are now harder to catch** (`main.c` M3a/M3c fixtures). With P=2, a leaked VM slot made the next `vm_s3_build` fail almost immediately; with P=8 up to 6 more leaks go unnoticed.
   - Fix: at each section end, assert that all `vm_s3.procs[i].state` are FREE except the known-live B.
7. **The cross-process audit cost grows with the square of capacity** (`vm.c:497-515`). Each audit of a full 73-page process now makes up to 73 × 7 × 73 ≈ 37k `extent_frame_phys` calls, up from about 5.3k. That is fine at boot.
   - Fix: record the bound, and note that a supervisor spawn path must not run the audit with interrupts disabled.
8. **The fault/reap loop got longer** (`main.c:750`). It now runs 27 iterations instead of 7, and the dispatch waits double because T doubled. This adds boot time but does not weaken anything. Record the smoke wall time in the PR.
9. **Unverified: the 16 MiB page count.** The notes report 3068 free pages at 16 MiB, against 3104 in the ADR. The ADR text "measured 3104" should be updated to the new value. The 36-page drop matches the BSS growth of 151,552 bytes ≈ 37 pages.

**Confirmed sound**
- **Quota check order:** `je_vm_build_quota` checks the quota range before the null-pointer, NX, slot, image and admission checks, and nothing changes before it (`vm.c:270`). `je_vm_build` is just a wrapper passing the default quota, and there is one rollback path.
- **Quota bound:** `je_handle_table_init` accepts quota ≤ capacity, so 39 is fine. The 16-bit index field has plenty of room for 40.
- **Thread rollback:** in the thread-capacity rollback, the slot check (`sched.c:79`) comes before the stack allocation, so NOMEM leaves `free_count` unchanged.
- **Capacity constants:** `sched.c`, `endpoint.c`, `endpoint_kern.c` and the `vm_backend.c` arrays and range checks are all sized from the macros. `rec_index` is uint16 and the pool is 24. The bounded discard array on the stack is unchanged, and no large pool objects are on the stack.
- **Old-cap literals:** none remain in `kernel`, `include` or `tools` code paths. Only comments are stale (items 2 and 4), and nothing in `main.c` depends on a third build returning BUSY.
- **D2 constants:** the formulas and `_Static_assert`s in the pre-wiring commit `bd9238a` match the contract (P=8, T=16, E=24 ≥ 23, SUP=28 with 28 + 4 ≤ 39).
- **M3 progress waits:** the waits scaled by `JE_THREAD_MAX` only got stricter (they now require more dispatches), not weaker.
- **Fairness:** with probes present, `min_dispatches ≥ 3` is still a valid lower bound across all threads.

These findings are AI-assisted review only, not independent human review.

## Round 1 — Codex reviewing the Claude tests

1. P2 — `tests/test_vm.c:607-619`: quota “checked FIRST” is not tested against null validation. A mutant moving the quota check after `!vm || !image || !out_index` passes `test_vm`. Add invalid-quota cases with null VM/image/output/blob and verify no backend/state change where applicable.

2. P2 — `tests/test_endpoint.c:249-253`: the second `(E+1)` pool-exhaustion attempt checks only status and outputs, not immutability. A mutant incrementing `allocated_count` only on repeated exhaustion passes. Snapshot pool/table/slots again before this call and compare afterward; also assert both recreated handles are live/distinct.

3. P3 — `tests/test_vm.c:539,685,802`: new tests duplicate the fake pool baseline as literal `253u`; `build_need()` also hard-codes fixture page counts. Centralize test-fixture constants or derive them from the initialized allocator/image.

confirmed sound

- P-wide admission and `(P+1)` `BUSY` coverage preserve the old property.
- Quota tests discriminate NX-order, full-table ordering, `MAX` boundaries, ignored quotas, default 15, per-table independence, slot reuse, and rollback.
- The dedicated `2E+2` endpoint table correctly prevents handle capacity from masking pool exhaustion.

## Lead disposition of round 1

- **Impl P1 (M2 preemption floor weakened from 16 to 14, and the count padded by probe switches): accepted.** The lead chose the reviewer's first option: restore M2 exactly (floor 16, the old value) and run the capacity fill as a separate phase afterwards. This keeps the previously tested M2 property rather than changing it, so no further owner escalation is needed. The capacity raise and re-baselined assertions were approved as D-OWNER-1. Rejected: keeping 14 and only recording the margin, because that weakens a tested invariant.
- **Impl P2 (ring-3 BADINDEX at the bound lost): accepted.** The lead had found the same gap independently from the worker's own decision 4. The probe moves to index `JE_VM_HANDLE_CAP`, plus a new in-range-free STALE probe.
- **All other P2/P3 items: accepted** (markers, REF_MAX comment, default-quota assert, leak checks, audit-bound note, NULL-order quota tests, second-exhaustion immutability, fixture constants, REFOVERFLOW test).

## Round 2 — Claude verifying the Codex fixes

The P1 and all three P2s are fixed, and I found no new P1 or P2 in the round-2 diff. I only read code and diffs; I did not build or run anything.

**Round-1 items**

1. **P1, M2 preemption: FIXED.** The M2 workload runs unchanged again: 7 workers, a floor of `(1u+JE_M2_WORKERS)*2u` = 16 with a `_Static_assert` on it, then reap. `main.c:283-295`.
   - Thread capacity is now its own phase after the reap (`main.c:296-325`). It creates 15 probes with interrupts off, checks that NOMEM leaves `free_count` unchanged, then reaps and compares against the page count taken at the start of the phase.
   - Creating threads after a reap works: reap resets each slot to `{0}`, which is `EMPTY` (`sched.c:16,423`).
   - The worker notes record `preemptions=0x49` (73) against the floor of 16, so the margin is about 4.5×.
   - Owner escalation is still needed, because this phase is new code inside previously tested M2.
2. **P2, ring-3 BADINDEX: FIXED.** `user_demo.S:381` encodes `0x10028` (index 40), and `vm.c:14` adds `_Static_assert(JE_VM_HANDLE_CAP==40u)` to pin it. That is an acceptable alternative to `.equ`. The free-slot STALE probe now writes to `+0x18` (`user_demo.S:386-392`), and the kernel checks it at `main.c:710`. No other handle-demo offset collides; the other `0x40001018` write (`user_demo.S:105`) is in a different program's address space.
3. **P2, marker: FIXED.** "seven guarded threads" is true again, the new marker is at `main.c:323-325`, and `smoke.py:15` requires it.
4. **P2, REF_MAX: FIXED.** The comment at `endpoint.h:76-80` is corrected. Hosted test 4b (`test_endpoint.c:136-178`) uses a table that cannot be the limiting factor. It shows that a dup past `JE_EP_REF_MAX` returns REFOVERFLOW, writes `d=0`, leaves pool, table header and slots byte-identical, and that the bound is per end. The supervisor bound is a documented note rather than an assert, which is acceptable.
5. **P3, default-quota assert: FIXED.** `vm.h:66-68`.
6. **P3, leak checks: FIXED.** Section-end asserts are at `main.c:652, 736, 1105`. `B` is in scope at all three, and `JE_VM_READY` is the correct live state.
7. **P3, audit bound: PARTIAL.** The bound of 37,247 comparisons is recorded in `impl-codex-r2.md:124`. The rule that a supervisor spawn path must not run the audit with interrupts disabled is not recorded anywhere.
8. **P3, smoke wall time: FIXED.** Recorded as 10.84 s before and 10.84 s after, in `impl-codex-r2.md:132-137`.
9. **P3, page count: NOT FIXED.** `docs/adr/0010-m4-supervisor-and-devices.md:115` still says "3104 pages". Only the uncommitted `IMPLEMENTATION.md` has 3068. Changing the measured figure in a frozen ADR needs an owner-approved amendment note; otherwise the PR should say plainly that the ADR figure is now stale.

**New P3s**

- **N1.** The layout comment for the handle demo (`user_demo.S:260-270`) does not list `+0x18`. Add "+0x18 query(in-range free slot 39) status".
- **N2.** `main.c:299` says "no-allocation probes", but each `je_thread_create` allocates a guarded stack. Only the probe body itself allocates nothing. Reword it.
- **N3.** The literal `0x10027` (index `CAP-1`) is also tied to CAP, but the static assert only covers 40 in the comment. Mention 39 in the `vm.c:11-14` comment.

**Confirmed sound in round 2**
- **Interrupts in the capacity phase:** the saved flags restore the disabled state, and the wait loop's `sti; hlt` is the only place interrupts get enabled.
- **Counter reset:** `capacity_completed` is reset to 0 before the phase.
- **Slot check order:** the slot check still comes before any allocation, so the NOMEM case cannot leak pages.
- **REFOVERFLOW test:** test 4b checks that nothing changed with snapshot `memcmp` comparisons, and the second NOSLOTS rejection on a full pool is checked the same way.
- **NULL-argument tests:** these quota-range rows honestly say that "quota checked first" and "NULL check first" cannot be told apart. They still catch NX, BUSY or validation running before the quota check.
- **Admission fixture:** `build_need()` now derives its page count from the fixture, with an assert that it is 3.

These findings are AI-assisted review only, not independent human review.

## Round 2 — Codex verifying the Claude fixes

1. **FIXED** — `tests/test_vm.c:690-706`: adds NULL VM/image/blob/output cases and verifies no backend/state mutation. Quota-first vs NULL-first is observationally indistinguishable when both are side-effect-free and return `JE_ERR_INVALID`; the author’s claim is correct.

2. **FIXED** — `tests/test_endpoint.c:314-325`: snapshots pool, table, and slots around the second exhaustion attempt; recreated handles are also checked live, distinct, and opposite-sided.

3. **FIXED** — `tests/test_vm.c:38,120-125`: centralizes fixture free-page accounting and derives build charge from the image/macros. The remaining `3u` is only a fixture sanity assertion.

No new P1/P2 findings.

confirmed sound
## Lead disposition of round 2

No new P1 or P2. The lead applied the remaining P3s: the scratch-layout comment, the probe-stack wording, the CAP-1 note in vm.c, the audit-bound note in IMPLEMENTATION.md, and the ADR 0010 16 MiB figure (the original value is kept as measured at `dc41da5`). Claude's point that the new capacity phase is new code inside previously tested M2 is surfaced to the owner in the PR.
