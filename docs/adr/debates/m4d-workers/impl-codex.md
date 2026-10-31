# M4 PR-D worker notes

Observation time: 2026-10-31T13:57:49+00:00 (UTC; recorded at handoff)

Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4d-impl`

Branch: `wip/m4d-impl`

Inspected base commit: `c39b026bd8827cec1c8e2fa281ccf798f0402d75`

## State

- Implemented: synchronous `PROC_KILL` (operation 16), pure process helpers,
  scheduler kill path, M4d QEMU fixture, and NX smoke expectation.
- Tested: kernel build, smoke boot, integration fault modes, all three requested
  x86 profiles, five consecutive NX-profile smoke runs, and whitespace checks.
- Planned: lead integration, adversarial review, and repository-level documentation
  refresh in the lead's combined change.
- Blocked: the existing hosted syscall unit test still expects operation 16 to be
  `UNSUPPORTED`; this worker was forbidden to edit tests. No remote CI or human
  review was run here.

## Changed files

- `kernel/core/proc.c`: implemented `je_proc_run_link` and
  `je_proc_is_supervisor` according to the frozen header contracts.
- `kernel/core/sched.c`: implemented synchronous `je_sched_kill`, preserving the
  static `mark_dying` implementation and checking IF=0 before the kill.
- `kernel/core/syscall.c`: decoded and dispatched operation 16 with the required
  argument validation, handle resolution, rights, identity, and result mapping.
- `kernel/core/main.c`: added the bounded M4d-s1 fixture and its setup,
  synchronization, assertions, reaping, and exact success marker.
- `kernel/arch/x86_64/user_demo.S`: added killer and READY, BLOCKED_IPC, and
  WOKEN-reserved victim programs.
- `tools/smoke.py`: requires the M4d success marker for the NX profile.

## Verification

Commands were run from the repository root. Results below are local results at
the worktree state above; none are claims about remote CI.

- `make all` — PASS.
- `ASAN_OPTIONS=detect_leaks=0 make check` — FAIL at the existing
  `tests/test_syscall.c:351` expectation that later syscall operations,
  including operation 16, decode as `UNSUPPORTED`. The preceding hosted tests
  pass; the test was not changed because tests are outside this worker's file
  list and the new contract intentionally recognizes operation 16.
- `make smoke` — PASS. The rebuilt ISO emitted:
  `M4d-s1: PROC_KILL OK (READY, BLOCKED_IPC, WOKEN-reserved, self/after-death/unlinked/rights)`.
- `make integration` — PASS. Fault modes 1 through 11 completed successfully;
  mode 11 included the M4d marker.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent`
  — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 16M --cpu qemu64,-nx
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present` — PASS.
- Five consecutive NX-profile smoke runs after `make smoke`, using
  `python3 tools/smoke.py build/0/jeios.iso --memory 128M --expect-nx present`
  — PASS, 5/5.
- `git diff --check` — PASS.
- Final diagnostic grep found only the pre-existing timing comment in
  `kernel/core/main.c`; no temporary M4d debug instrumentation remains.

Not run here: remote CI, ARM execution, independent human review, or any
qualification claim beyond the local evidence above.

## Non-obvious decisions

1. `je_sched_kill` follows the frozen `sched.h` check order: snapshot the
   process record, reject self, accept already-DYING/DEAD as idempotent
   `JE_OK`, reject non-LIVE and supervisor records, then validate the run link
   before reading the frame and calling unchanged `mark_dying`. This preserves
   first-reason-wins behavior and avoids reading a frame after `mark_dying`
   clears it.
2. The kill path requires the linked slot to be a matching USER process in
   READY or BLOCKED_IPC state with a frame. A LIVE record without a created
   thread therefore returns `JE_INVALID` and remains LIVE; it does not invent
   a forced-kill hook. This follows the D8 correction that the historical
   forced-kill hook described by ADR 0009 never existed.
3. The syscall dispatcher returns to the killer without rescheduling and
   clears RDX. Invalid argument registers are rejected before handle
   resolution. Process-handle rights and object type are checked before
   identity lookup, preserving `WRONGTYPE`, `DENIED`, `STALE`, and `INVALID`
   distinctions.
4. Fixture observations that depend on scheduler state are taken with IF=0.
   The WOKEN case compares `je_sched_dispatches_of(V)` from the parked
   baseline through observed DYING, so an intervening dispatch cannot pass
   falsely. It checks the reserved head before reaping and only closes the
   peer endpoint after confirming one unreserved deliverable message remains.
5. The fixture explicitly executes `sti` at the handoff from the prior M4c
   coordinator because that fixture can leave IF=0. Without this, the new
   victim waits could deadlock despite correct kernel behavior.
6. The blocked victim uses a long receive timeout and never writes its
   receive-status scratch before being killed. The coordinator uses only
   `je_kernel_idle_step` for reaping, as required by the merged M4 contract.
7. The initial implementation briefly placed the fixture control word over the
   first process handle, and later closed the stale handle only after the
   stale test. Both were corrected before the final build and smoke runs.

## Handoff

The lead should integrate the six source changes plus this note, rerun the
repository's complete checks after integration, and update the current
`docs/HANDOFF.md`/implementation-status records. The unresolved test mismatch
must be addressed by the lead in the appropriate test change; this worker did
not alter frozen headers or tests.
