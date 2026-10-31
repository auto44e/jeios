# M4 PR-B worker notes (final worker handoff)

Observation date: 2026-10-31 UTC. Exact wall-clock minute was unavailable to
the worker runtime; no time is invented.

Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4b-impl`
Branch: `wip/m4b-impl`
Inspected base commit: `5c5a3c23cdcbefa1f0dc2dde50dd7370b491cc20`
Working tree: uncommitted. No commit, push, network access, or git-config change
was performed.

## State

- Implemented: assigned M4 PR-B kernel, x86-64 fixture, and smoke-harness
  changes for ADR 0010 D3, D4, and D-DEV.
- Tested: final strict build, default QEMU smoke, integration fault modes, and
  all three requested x86 profiles pass.
- Planned: lead integration with parallel hosted-test/documentation changes,
  then different-lineage adversarial review and owner merge decision.
- Blocked: `ASAN_OPTIONS=detect_leaks=0 make check` stops at the current stale
  `tests/test_proc.c` assertion that relinking a linked LIVE record succeeds.
  The frozen contract requires relink rejection. Tests were outside this
  worker’s edit list and were not changed.

## Files changed

- `kernel/core/proc.c`
- `kernel/core/endpoint.c`
- `kernel/core/endpoint_kern.c`
- `kernel/core/sched.c`
- `kernel/core/syscall.c`
- `kernel/core/main.c`
- `kernel/arch/x86_64/user_demo.S`
- `tools/smoke.py`
- `WORKER_NOTES.md`

No `include/`, `tests/`, documentation, or unrelated source file was edited.

## Implementation summary

- Enforced PROCESS contracts: relink rejection, unpublished-only abandon,
  bounded death reasons, LIVE-only charging, exact `NONE` handling, and
  reaper-only clearing of `{run_linked, thread_slot, vm_index}`.
- Enforced `create_by` ordering and rollback. Invalid/stale/non-LIVE creators
  return `JE_EP_INVALID` before allocation; quota returns `JE_EP_QUOTA`;
  uncharged records store `JE_PROC_ID_NONE`.
- User endpoint creation fails closed with `JE_EP_CORRUPT` when a THREAD_USER
  has no PROCESS identity; kernel seed/setup remains uncharged.
- Added exact endpoint wiring checks to create, dup, and PROCESS-handle minting.
  Mint now takes `(je_vm *, index, id, rights, out)` and checks wiring before
  retaining.
- Added typed dispatch retain/release hooks and fail-stop handling for broken
  PROCESS object/refcount invariants in status, close, and dup paths.
- Made registration metadata clear on drop/direct teardown. `drop_user` no
  longer edits the PROCESS record. Reaping is capture -> drop -> VM destroy ->
  `on_reaped`; failures after drop fail-stop.
- Added `je_kernel_idle_step()` (`irq_save -> reap/fail-stop -> sti;hlt`) and
  used it in the final idle loop and M4b waits. M4b observes child death by
  waiting for `PROC_STATUS == DEAD`, with no manual reaper calls.
- Direct fixture teardown uses `on_dying(KILLED) -> VM destroy -> on_reaped`;
  `je_proc_abandon` remains spawn-rollback-only.
- Added ABI static assertions for PROCESS type/states, death reasons, and the
  reason bound.
- Added the QEMU quota proof: parent `ep_limit == 1`; first create succeeds,
  second returns ABI `H_QUOTA == 7`, both ends close, and a third create
  succeeds. Fixed the `user_demo.S` 8-byte overlap at `0x1714` with a 4-byte
  store. Added a dedicated smoke marker.

## Commands and results

The initial sandbox shell failed with
`bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted`; required reads
and builds then used the approved escalated execution path. `rg` was unavailable
(`rg: command not found`), so `find`, `grep`, and `sed` were used.

Preparation completed:

- Read `AGENTS.md`, `README.md`, `docs/HANDOFF.md`, `docs/ROADMAP.md`,
  `docs/DESIGN.md`, `docs/AGENT_START.md`, `docs/CODING_STANDARD.md`,
  ADR 0010, `/tmp/m4b/map.md`, `/tmp/m4b/f-claude.md`, and
  `/tmp/m4b/f-codex.md`.
- Inspected frozen HEAD versions of `include/jeios/proc.h`, `abi.h`,
  `handle.h`, and `endpoint.h`.
- `git rev-parse --show-toplevel`: PASS; expected worktree.
- `git rev-parse --abbrev-ref HEAD`: `wip/m4b-impl`.
- `git rev-parse HEAD`: `5c5a3c23cdcbefa1f0dc2dde50dd7370b491cc20`.
- `git status --short --branch`: only the assigned source/harness files and
  this notes file are modified/untracked.
- `git diff --check`: PASS.

Final verification:

- `make all`: PASS with `-Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes`, freestanding C11, and baseline `-march=x86-64`.
- `ASAN_OPTIONS=detect_leaks=0 make check`: PARTIAL/FAIL at
  `tests/test_proc.c:240`, where the stale test expects relinking to return
  `JE_OK`. Before that stop, `test_core`, `test_boot`, `test_alloc`, `test_vm`,
  `test_uframe`, `test_syscall`, `test_handle`, reduced-width `test_handle_rw`,
  and `test_endpoint` printed PASS. Leak scanning was disabled as requested.
- `make smoke`: PASS; observed M3c-s7, the quota marker, the PROCESS marker,
  and `initialization complete; halted`.
- `make integration`: PASS; fault modes 1 through 11 all reported PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present`: PASS, including both M4b markers.

The smoke harness terminates QEMU after expected output; each run ended with
`QEMU smoke: PASS`.

## Non-obvious decisions

1. A user THREAD_USER with no PROCESS identity returns `JE_EP_CORRUPT`, rather
   than silently falling back to uncharged `NONE`, because this is an authority
   boundary and must fail closed.
2. Only the exact all-zero identity is uncharged. `{index != 0, gen == 0}` is
   invalid, not a kernel caller. Checking only `gen` would create a quota
   bypass.
3. Registration metadata is cleared during teardown, but PROCESS run-link
   fields are cleared only by `je_proc_on_reaped`. Clearing the record in
   `drop_user` would violate the frozen lifecycle and lose identity before VM
   teardown completes.
4. Direct fixture destruction uses `KILLED -> VM destroy -> on_reaped`, not
   `abandon`, because the record represented a real published process even when
   no user thread was started.
5. The idle wrapper fail-stops on any negative reap result. Returning to the
   loop would hide corruption after the thread slot had already been dropped.
6. Broken PROCESS object/refcount invariants fail-stop rather than returning
   `H_STALE` or `H_WRONGTYPE`; those statuses are for hostile input, not
   internal lifetime corruption.
7. Typed dispatch includes retain/release hooks. SELF and ENDPOINT have null
   hooks here because SELF is immortal and endpoint ownership remains in its
   endpoint-aware seam; PROCESS owns the pool hooks.
8. The quota probe preserves the first packed endpoint result across the failed
   second create. A separate process would add capacity/teardown coupling
   without improving the required proof.
9. M4b waits for `PROC_STATUS == DEAD`, not `je_sched_has_dying`, because the
   idle step may reap between observations. This directly proves automatic
   reaping and record survival.
10. Headers, tests, and docs were not edited because the worker instruction
    restricted edits to the assigned implementation/harness files plus these
    notes. The lead must merge this evidence into the shared handoff.

## Not verified / unresolved

- Parallel replacement hosted PROCESS tests were not present; current
  `test_proc` is the only observed check failure.
- No remote CI result or independent human review was observed. This is
  AI-assisted implementation evidence, not hardened/release qualification.
- ARM execution and cross-architecture qualification were not run.
- No claim is made for all hardware, drivers, application ABIs, SMP, or
  hardened isolation.

Next bounded task: lead integration with the parallel tests/docs, then
different-lineage adversarial review. Pass criteria are green updated hosted
tests, preserved frozen contracts, rerun verification, and owner acceptance.
