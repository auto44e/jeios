# WORKER_NOTES — M4 PR-A (#53), TEST WRITER role

Observed 2026-10-31 (local, hosted only). Worktree `wt-m4a-tests`, branch
`wip/m4a-tests`, base commit `bd9238a` (ADR 0010 D2 pre-wiring). Nothing is committed or pushed.
This is AI-assisted work. It is not independent human review.

## State
- **implemented:** test updates for the new caps, plus the new `je_vm_build_quota` tests (only in `tests/test_vm.c` and `tests/test_endpoint.c`).
- **tested (local, hosted):** every hosted binary passes except `test_vm`. `test_vm` fails ONLY on the new positive quota
  assertions, which is expected against the fail-closed stub (see the mapping below).
- **not verified:** the tests against the REAL `je_vm_build_quota` (it is being written in parallel). I did not run `make check`, `smoke`,
  `integration`, CI profiles or ARM. Those are the lead's job.

## Files changed
- `tests/test_vm.c`
- `tests/test_endpoint.c`
- `tests/vm_fake.c`: **no change needed.** It already sizes `hold[]`/`proc[]` and all of its loops by
  `JE_VM_MAX_PROCESSES`, so it models P=8. The new P-wide tests exercise it at P.
- `test_handle.c`, `test_syscall.c` and the other hosted tests do not encode the old caps. `test_handle.c`'s `CAP 8` is a
  generic local table that is unrelated to the vm table. They are unchanged.

## What changed

### test_vm.c
1. `test_admission`: the old version built 2, then expected BUSY on the 3rd. It now builds exactly `JE_VM_MAX_PROCESSES`, checks
   that every index is in range and distinct, and that the page charge is `P*need`. The (P+1)-th build is BUSY, and pages AND
   `vm.procs` are byte-identical (memcmp). Destroying all P returns every page. A runtime guard checks
   `P*need <= 253` so that NOMEM can never mask BUSY.
2. New `test_all_processes_isolated`: the two-process isolation property, extended to all P. It checks pairwise distinct roots,
   distinct kstack runs, disjoint extents, `je_vm_audit` OK for each process, and that a full teardown restores the pages.
   The old 2-process `test_build_audit_and_teardown` is kept unchanged, because it checks PTE/bytes/aliases in depth.
3. New quota tests. The helper `assert_admits_exactly(t, q)` asserts capacity == H and `t->quota == q`. It allocates SELF handles
   until live == q, then expects QUOTA (`*out` == 0, live unchanged). It then closes one handle, re-admits exactly one, and gets QUOTA again.
   The helper does NOT assume how many handles the builder seeds.
   - `test_build_quota_range` (negative cases run first, so they pass against the stub too). Quotas
     {0, MAX+1, H, UINT32_MAX} -> INVALID, crossed with {good image+NX, NX unavailable, unsupported-version image,
     invalid image (0 segments)}. Each case checks: pages byte-identical, all `vm.procs` byte-identical, and **zero fake
     backend attempts** (`je_vm_fake_fallible_count()==0`, counter reset with `je_vm_fake_fail_at(0)`). Control
     assertions first show that the no-NX and bad-version inputs really return UNSUPPORTED through `je_vm_build`, so the
     INVALID result is the range check winning. Out-of-range quotas with all P slots READY -> INVALID, not BUSY. In-range
     boundaries (1, MAX) with NX unavailable -> UNSUPPORTED, so the range check is not over-broad.
   - `test_build_quota_admits`: `je_vm_build` admits exactly `JE_VM_HANDLE_QUOTA_DEFAULT` (15, static-asserted) live
     handles. `je_vm_build_quota(DEFAULT)` matches `je_vm_build` in state, entry, extent/journal counts, held kstack,
     table capacity/quota/live, page charge and audit. Quota 1 and quota MAX each build, pass the audit, admit exactly q,
     and tear down to the full page count.
   - `test_build_quota_independent`: P concurrent builds with strictly increasing quotas from 1 to MAX. Each table reports
     its own quota. Each table is filled in turn (in reverse), and no other table's live count or quota changes. Slot reuse:
     destroying the quota-MAX slot and rebuilding with quota 1 gives quota 1. Destroying the quota-1 slot and rebuilding via `je_vm_build`
     gives quota 15 (no stale quota). The single free slot makes the reused index deterministic.
   - `test_build_quota_rollback`: the existing fault-injection sweep, run through `je_vm_build_quota(MAX)`. After every injected
     failure, pages and `vm.procs` are byte-identical.

### test_endpoint.c
- The local table `CAP`/`QUOTA` were 16/15, copied from the M3b process table. They are now `JE_VM_HANDLE_CAP` /
  `JE_VM_HANDLE_QUOTA_DEFAULT`, which needs `#include <jeios/vm.h>` (constants only, not linked).
- Test 9 (pool exhaustion): E=24 pairs need 48 handles, which is more than any process table (H=40). The test now uses a dedicated
  table sized `2*JE_EP_POOL_CAP+2` with quota == capacity, so the pool is provably the limiter (`live == 2E` is asserted).
  Exactly E creates succeed, and the count is checked after each one. On the (E+1)-th create: NOSLOTS, `x0==x1==0`, and the pool,
  table header and slots are all byte-identical. Closing both ends of one record allows exactly one more create, and the next is
  NOSLOTS again. The compile-time pool formula assert is not duplicated.

## Commands run and results
| Command | Result |
|---|---|
| `ASAN_OPTIONS=detect_leaks=0 make test` (baseline, before edits) | rc=2: `test_vm.c:314` 3rd-build BUSY assert (old cap 2) |
| `make build/0/test_endpoint build/0/test_vm` (after edits) | builds clean with `-Werror -Wconversion -Wshadow ...` |
| `ASAN_OPTIONS=detect_leaks=0 make test` (after edits) | rc=2: aborts in `test_vm` (expected, see below) |
| each `./build/0/test_*` run individually | core, boot, alloc, uframe, syscall, handle, handle_rw, endpoint, fdt: **PASS**. vm: **abort** at `test_vm.c:673` |
| `git diff --check` | clean |
| Scratch `/tmp/m4a_scratch/test_vm_soft.c` (non-aborting `assert`, one test per argv), linked with the real tree | tests 0-5 (old + P-isolation) 0 failures. Quota tests fail as mapped below |
| Same soft copy plus `/tmp/m4a_scratch/quota_model.c` (a header-only reference model: range check, then `je_vm_build`, then re-init table quota; linked with `--allow-multiple-definition`) | tests 0-6 and 9: 0 failures. Tests 7 and 8 fail ONLY at the `je_vm_build`→15 checks (see note) |

### Failure mapping against the stub (expected, one line each)
- `test_build_quota_range:673/675` (soft 677/679): in-range quota 1 / MAX with NX unavailable expects UNSUPPORTED, but the stub returns INVALID.
- `test_build_quota_admits`, first `assert_admits_exactly` (`t->quota == quota` and the following asserts): `je_vm_build`'s table quota
  is 39, not 15. Pre-wired `kernel/core/vm.c` still passes `JE_VM_HANDLE_CAP - 1u` to `je_handle_table_init`.
- `test_build_quota_admits` DEFAULT-equivalence block, quota-1 and quota-MAX builds: the stub returns INVALID instead of OK.
- `test_build_quota_independent`: all P quota builds are INVALID (stub). The cascaded quota/destroy asserts follow from that.
- `test_build_quota_rollback`: the initial quota build is INVALID (stub), so the sweep has 0 attempts.
- Against the reference model, the only failures left are the two `je_vm_build`-admits-15 checks (test 7 first block, plus the
  quota comparison against `via_build`, and the final `je_vm_build` slot reuse in test 8). All of them come from the quota-39 line above.

## Decisions (with the alternative I rejected)
1. **Do not assume that build seeds zero handles.** `assert_admits_exactly` starts from `t->live`. The header says nothing about
   seeded handles. Rejected: asserting `live == 0` after build, which would over-specify the implementation.
2. **Test "admits exactly q" through `je_handle_alloc`, not only through the `quota` field.** The field could be right while the table
   is wrong, or the other way round, so the test asserts both. Rejected: checking only `htable.quota`.
3. **Count zero backend attempts to show the range check comes "FIRST".** Status alone cannot separate "range check first" from "image
   validation first" for an invalid image, because both return INVALID. Zero fake attempts plus byte-identical state does.
   Caveat: pure validation makes no backend call either. For the invalid-image case, the order evidence is the INVALID status on
   unsupported-version and no-NX inputs (those would otherwise return UNSUPPORTED). Rejected: relying on status alone.
4. **Dedicated 2E+2 table for endpoint pool exhaustion** instead of raising the shared fixture table above H. This keeps the shared
   fixture equal to a real process table. Rejected: `CAP = 2*E+2` globally, which would silently stop the other tests from modelling a
   process table.
5. **Field-wise `je_vm_build` vs `je_vm_build_quota(DEFAULT)` equivalence, not memcmp of the whole `je_vm_process`.** A whole-struct
   memcmp could fail on padding or on intentionally different internals and would be a false failure for the implementer.
   Rejected: memcmp.
6. **Keep the 2-process deep-audit test** and add a separate P-wide isolation test, instead of rewriting the deep test for P. The PTE/byte/alias
   checks stay readable, and P-wide coverage is still there.
7. **Spread quotas from 1 to MAX with an integer formula.** The values come only from the constants and include both endpoints.
   A static assert requires `P <= MAX` so the quotas are distinct.

## Contract observations (header is frozen; literal reading implemented)
- vm.h does not say whether `*out_index` is written on INVALID. The tests do not assert it.
- vm.h does not say that INVALID beats BUSY when all slots are READY. I read "checked FIRST (before the NX check and any other
  validation)" as covering admission too, and `test_build_quota_range` asserts INVALID with a full table. **If the implementer reads
  "validation" narrowly, this is the one assertion that may need a lead decision.**

## Disclosure
- I did not read `kernel/core/vm.c`'s build implementation. I ran ONE `grep -n HANDLE_CAP kernel/core/vm.c` while diagnosing a failure.
  It showed the quota line `je_handle_table_init(..., JE_VM_HANDLE_CAP, JE_VM_HANDLE_CAP - 1u)` (lines 363-370) and nothing else.
- The scratch files in `/tmp/m4a_scratch/` are not part of the diff.
