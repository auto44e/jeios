# M4 PR-A worker notes

Observation: 2026-10-31T00:14:12+00:00 UTC
Repository: `auto44e/jeios`
Branch: `wip/m4a-impl`
Inspected base commit: `bd9238a23bb45c7926bf2c6d1296f7e01cf8c16a`
Worktree state: uncommitted; only `kernel/core/main.c`, `kernel/core/vm.c`,
and this notes file are changed by this worker.

## State

- Implemented:
  - Replaced the `je_vm_build_quota` fail-closed stub with the existing VM
    builder under one code path.
  - The quota check is the first operation: `0` or
    `> JE_VM_HANDLE_QUOTA_MAX` returns `JE_ERR_INVALID` before pointer, NX,
    image, admission, or state validation.
  - `je_vm_build` delegates with `JE_VM_HANDLE_QUOTA_DEFAULT`.
  - Re-baselined M2 to exactly `JE_M2_WORKERS` (7) workload workers, then
    fills the remaining 8 scheduler slots with short capacity probes. The next
    create must return `JE_ERR_NOMEM` with `free_count` unchanged. Probes yield
    twice so the existing minimum-dispatch invariant remains meaningful; all
    threads then exit and are reaped.
  - Re-baselined the M3b self-test’s old encoded index-16 assumption. With
    H=40, index 16 is in range but FREE, so `STALE` is the correct result.
  - Kept the existing smoke marker strings unchanged.
- Tested:
  - Strict kernel build, default smoke, integration, all three required x86
    profiles, the invalid-quota ordering probe, diff whitespace, ELF size, and
    pool symbol sizes were run locally as recorded below.
- Planned:
  - The lead/other worker must update the worker-owned hosted VM assertions for
    P=8 and add/retain direct coverage for the valid custom quota path.
  - The lead should integrate this diff and update the repository handoff/status
    documents under their broader file scope.
- Blocked:
  - `ASAN_OPTIONS=detect_leaks=0 make check` is currently stopped by the
    worker-owned old-cap assertion in `tests/test_vm.c:314`; no other hosted
    assertion was observed because the test runner aborts at that first failure.
  - Remote CI and independent human review were not available in this worker.

## Files and decisions

Changed files:

- `kernel/core/vm.c`
- `kernel/core/main.c`

Decision 1 — quota implementation: I moved the existing `je_vm_build` body into
`je_vm_build_quota`, put the frozen range check before every other operation, and
made `je_vm_build` a thin default-quota wrapper. This preserves one rollback and
publication path. A duplicated builder was rejected because the two paths could
drift in admission, journaling, or handle-table initialization.

Decision 2 — M2 capacity test: the seven meaningful worker workload remains
exactly seven. The eight extra slots are filled by a no-allocation probe rather
than by creating 15 copies of the workload. The latter would violate the frozen
M2 workload requirement and make the capacity test’s resource pressure depend on
the worker heap workload. The probes yield twice, complete, and are included in
the same reap/accounting check. The failed first smoke run confirmed that using
the new `JE_THREAD_MAX*2` threshold for this short-lived probe population was
not the old invariant’s meaning.

Decision 3 — scheduler thresholds: the M2 preemption floor is now
`JE_M2_WORKERS * 2`, meaning two passes over the seven-worker workload. The M3
dispatch waits retain `JE_THREAD_MAX*2`, `*3`, and `*4`; those waits are
capacity-scaled conservative whole-scheduler-table budgets, so T=16 means two,
three, or four full slot passes. Replacing all M3 waits with the worker count
would weaken their scheduling-progress budget and conflate M2 workload with M3
fixture capacity.

Decision 4 — old handle bound probe: the existing ring-3 image encodes index 16
as `index == capacity`, which was true for H=16. H=40 makes that index an
in-range free slot, so the kernel-side expected result is now `STALE`. Editing
`kernel/arch/x86_64/user_demo.S` was rejected because it is outside this
worker’s allowed file list; the actual probe remains useful as an in-range-free
stale-handle check and smoke verifies it.

Decision 5 — untouched implementation files: scheduler allocation/scan loops,
endpoint pool loops, endpoint-kernel bounds, and x86 VM-backend arrays already
use the raised compile-time constants and had no old literal capacity logic.
Changing them would add risk without changing behavior. The final permitted-C
source sweep found no stale old-cap logic there. `user_demo.S` still has a
comment saying “CAP-1 = 15”; its behavior uses the frozen default quota 15, but
the comment is outside this worker’s scope.

## Size audit

The requested pre-wiring-parent baseline is recorded as:

```
text 72814
bss  2797568
```

Final local command:

```
size build/0/jeios.elf
```

Result:

```
text 72506
data     0
bss  2949120
```

Final grown-pool measurements:

```
sizeof(je_vm)         = 76152 bytes
sizeof(je_vm_process) =  9512 bytes
thread table `threads` (nm) = 0x0e80 = 3712 bytes
endpoint pool `g_kep` (nm)   = 0x4a44 = 19012 bytes
vm_backend `g_vb` (nm)       = 0x0ad0 = 2768 bytes
```

The `sizeof` values came from a temporary host program compiled from stdin and
run from `/tmp`; it was not left in the repository. The private/static arrays
were measured with `nm -S` on the final kernel objects.

The historical 16 MiB pre-raise measurement is `0xC20` (3104 pages), recorded
in the ADR debate. The exact final command below printed `0xBFC` (3068 pages),
a decrease of 36 pages. This is a page-pool measurement, not a claim that the
static BSS delta alone explains every allocator reservation.

## Command ledger

Read-only baseline and audit commands:

- `git status --short --branch`: clean initially; branch `wip/m4a-impl`.
- Required AGENTS, coding-standard, ADR, README, HANDOFF, ROADMAP, AGENT_START,
  DESIGN, and NEXT_PHASE reads: completed before editing.
- `git show --format=fuller --stat HEAD` and `git show HEAD`: confirmed the
  pre-wiring base and frozen P=8/T=16/E=24/H=40 quota API.
- Repository capacity searches with `grep`/`find`: completed. `rg` was
  unavailable, so `grep`/`find` were used as the documented fallback.

Build and source checks:

- `make all`: PASS, including strict C11
  `-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes` and
  baseline `-march=x86-64`.
- `git diff --check`: initially reported one temporary extra blank line at EOF;
  after correction: PASS.
- `git diff --check && git status --short --branch`: PASS; final implementation
  scope is `kernel/core/main.c` and `kernel/core/vm.c`, plus this notes file.
- Final permitted-source capacity sweep: PASS; all VM/process/thread/endpoint
  bounds are macro-based or formula-asserted. No `tools/smoke.py` marker change
  was needed.

Hosted checks:

- `ASAN_OPTIONS=detect_leaks=0 make check`: FAIL at
  `tests/test_vm.c:314`, assertion
  `je_vm_build(... second capacity exhaustion ...) == JE_ERR_BUSY`.
  `test_core`, `test_boot`, and `test_alloc` passed before that assertion.
  The assertion is the old P=2 expectation; with P=8 the third build is
  admitted, so it must be rewritten by the tests worker. The runner aborts at
  this first failure, so no additional failing assertion was observed.
- Temporary quota-order host probe, compiled from stdin and linked with
  `kernel/core/vm.c kernel/core/handle.c`: PASS. It verified both quota `0` and
  `JE_VM_HANDLE_QUOTA_MAX + 1` return `JE_ERR_INVALID` even with null build
  pointers, demonstrating the range check precedes other validation.

Boot and integration:

- First `make smoke`: FAIL at M2 with
  `JeiOS fatal: insufficient preemption`; this was the rejected
  `JE_THREAD_MAX*2` threshold after short capacity probes exited.
- Second `make smoke`: reached M3b, then failed at the old H=16 bound-index
  assertion; fixed by the H=40 stale-result re-baseline above.
- Final `make smoke`: PASS. M2 exhaustion/rollback, M3a, M3b, M3c, and the
  existing terminal marker all passed.
- First `make integration`: fault modes 1–7 passed, then the normal mode failed
  at the same old M2 preemption floor.
- Final `make integration`: PASS. Fault modes 1–11 and the normal run passed.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent`:
  PASS; NX absent and M2 free pages `0x7BFC` in the default memory profile.
- `python3 tools/smoke.py build/0/jeios.iso --memory 16M`: PASS, exploratory
  run; exact required command follows.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent`:
  PASS; M2 free pages `0xBFC` (3068).
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present`:
  PASS; M2 free pages `0x3FC1C`.
- `size build/0/jeios.elf`: final result recorded in the size audit above.
- `nm -S --size-sort build/0/kernel/core/sched.o build/0/kernel/core/endpoint_kern.o build/0/kernel/arch/x86_64/vm_backend.o`:
  PASS; symbol sizes recorded above.

## Verification limits and handoff

All test results above are local observations, not remote CI. No ARM build or
execution was requested or claimed. No tests, headers, `user_demo.S`, docs, git
config, commit, push, or network operation was changed by this worker. The
critical-profile independent human review gate remains open. The lead should
integrate this uncommitted diff, have the tests worker update the stale P=2
assertion and add direct valid-quota coverage, then rerun the complete gate and
update the repository handoff/status documents.
