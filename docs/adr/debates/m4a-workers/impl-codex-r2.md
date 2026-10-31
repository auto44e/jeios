# M4 PR-A worker notes — implementor round 2

Observation time: `2026-10-31T01:17:22+00:00`
Repository: `auto44e/jeios`
Branch: `wip/m4a-impl`
Inspected base commit: `f2e85e7eab75acd1e9f25e03fa794d02a6ef8e9e`
Worktree state: uncommitted.

## State

Implemented:

- Restored the exact seven-worker M2 workload: `JE_M2_WORKERS == 7`,
  preemption floor `(1u + JE_M2_WORKERS) * 2u == 16u`, and
  `min_dispatches >= 3`; the original reap and page-accounting check remains
  immediately after it.
- Added a separate post-M2 capacity phase. With interrupts disabled during all
  creates, it fills all `JE_THREAD_MAX - 1` non-idle slots with probes whose
  bodies perform no allocation, checks the next create returns `JE_ERR_NOMEM`
  without changing `free_count`, lets them exit, reaps them, and checks pages
  return to the phase baseline.
- Added the hex preemption count to the existing M2 marker. Observed count:
  `0x49`.
- Restored the ring-3 capacity-bound probe to encode
  `index == JE_VM_HANDLE_CAP`, generation 1 (`0x10028` for H=40), expecting
  `JE_ABI_H_BADINDEX` at `0x1060`.
- Added an in-range FREE-slot probe at `0x1018` (`index 39`, generation 1),
  expecting `JE_ABI_H_STALE`.
- Added a C `_Static_assert` in `vm.c` coupling the assembly literal to
  `JE_VM_HANDLE_CAP == 40u`.
- Corrected the assembly comments so table capacity H=40 is distinct from the
  default live-handle quota 15.
- Updated the M3b marker to state restored capacity-bound `BADINDEX` and
  in-range-free `STALE`.
- Added the requested default-quota range `_Static_assert` in `vm.h`.
- Updated the `JE_EP_REF_MAX` comment to describe `handle_refs` as an explicit
  policy bound enforced by `REFOVERFLOW`, independent of H=40 table slots, and
  records ADR 0010's per-end supervisor bound.
- Added VM-state leak assertions at the M3a, M3b, and M3c boundaries. M3a and
  M3b intentionally retain only survivor B; M3c intentionally retains no VM
  process.
- Required the new capacity marker in `tools/smoke.py`.

Changed files:

- `kernel/core/main.c`
- `kernel/arch/x86_64/user_demo.S`
- `kernel/core/vm.c`
- `include/jeios/endpoint.h` (comment only at `JE_EP_REF_MAX`)
- `include/jeios/vm.h` (one requested `_Static_assert` only)
- `tools/smoke.py`
- `WORKER_NOTES.md` (required uncommitted evidence)

Tested locally:

- `make all`: PASS.
- `ASAN_OPTIONS=detect_leaks=0 make check`: PASS.
- `make smoke`: PASS.
- `make integration`: PASS; fault modes 1 through 11 and normal mode passed.
- All three requested x86 profile commands: PASS.
- `git diff --check`: PASS.

Planned:

- Lead integration, independent adversarial review, remote CI observation, and
  repository-level handoff/status updates remain outside this worker's file
  scope.

Not verified:

- Remote CI was not run or observed.
- No independent human review was performed; AI-assisted review is not a
  critical-profile qualification.
- ARM build/execution was not requested for this x86-only change and was not
  claimed.
- `docs/HANDOFF.md` and the implementation ledger were not edited because the
  user restricted this worker to the listed files.

## Decisions and rejected alternatives

1. Capacity probes run after the original M2 workload's reap. Keeping them in
   the first workload would let short-lived probes alter the preemption margin
   and would make the M2 floor describe T=16 rather than the frozen
   seven-worker proof. Rejected: filling spare slots before the M2 marker and
   counting their dispatches as part of M2.

2. The M2 floor uses `(1u + JE_M2_WORKERS) * 2u`, with a compile-time assert
   that it equals 16. This is the literal old `JE_THREAD_MAX * 2` floor when T
   was 8 while keeping the worker count explicit. Rejected: `JE_THREAD_MAX * 2`,
   because capacity probes are not workload iterations; rejected
   `JE_M2_WORKERS * 2`, because it omits the idle slot represented by the old
   formula.

3. The capacity phase takes an IRQ-save before all create calls and restores
   flags only after the failed-create/accounting check. It then waits with the
   timer enabled. Rejected: relying on incidental IF state, because thread
   creation is an IF=0 operation and the test must exercise that contract.

4. The assembly keeps literal `0x10028` because the user explicitly allowed a
   literal plus C `_Static_assert`, while including the full C VM header in
   assembly would expose C declarations to the assembler. The assert makes a
   future H change fail the kernel build instead of silently drifting the probe.
   Rejected: the old `0x10010`, which is in range at H=40; rejected treating
   index 16 as the capacity boundary.

5. Index 39 at `0x1018` was selected because it was unused in the handle-demo
   scratch map and remains FREE before self#2 and quota operations. Rejected
   reusing `0x1060`, because that offset must carry the distinct capacity-bound
   `BADINDEX` result.

6. M3a and M3b retain B because later proofs require the survivor; all other VM
   records must be FREE. M3c tears B down before its later proofs and ends with
   all VM records FREE. Rejected checking only page counts or endpoint-pool
   counts, because balanced resources can still hide a logically READY record.

7. `JE_EP_REF_MAX` is documented as policy with `REFOVERFLOW`, not as a
   consequence of table slots. H=40 makes the old implication false. Rejected
   changing the endpoint constant or behavior because this request was
   comment-only and the endpoint contract is frozen.

## Quantitative evidence

Requested cross-process audit bound:

```
pages * (P - 1) * pages = 73 * (8 - 1) * 73 = 37,247 frame comparisons
```

This is a conservative bound calculation, not a measured worst-case execution
time claim.

Smoke wall time:

- Before this round-2 change:
  `/usr/bin/time -p make smoke` → `real 10.84`, `user 6.20`, `sys 0.14`.
- After this round-2 change:
  `/usr/bin/time -p make smoke` → `real 10.84`, `user 6.19`, `sys 0.15`.

Default post-change smoke recorded:

```
M2: preemption/GPR/sleep/heap checks passed; ticks=0x000000000000004c; preemptions=0x0000000000000049
M2: thread capacity exhausted at T=0x0000000000000010; NOMEM rollback and reap OK
```

The 16 MiB profile observed M2 free pages `0xBFB` before the capacity phase;
the NX-present 1536 MiB profile observed `0x3FC1B`. These are emulator
observations, not general hardware support claims.

## Exact command ledger

Required reads and baseline inspection:

- Read `AGENTS.md`, `README.md`, `docs/HANDOFF.md`, `docs/ROADMAP.md`,
  `docs/DESIGN.md`, `docs/AGENT_START.md`, `docs/NEXT_PHASE.md`,
  `docs/CODING_STANDARD.md`, and ADR 0010.
- `git show HEAD` and targeted history inspection confirmed frozen P=8, T=16,
  E=24, H=40, `JE_M2_WORKERS=7`, quota constants, and
  `je_vm_build_quota` at commit
  `f2e85e7eab75acd1e9f25e03fa794d02a6ef8e9e`.
- `grep`/`find` were used instead of `rg` because `rg` is unavailable.

Environment and fallback results:

- The first read-only `exec_command` was blocked before execution by the
  sandbox namespace error `bwrap: loopback: Failed RTM_NEWADDR:
  Operation not permitted`; the same reads succeeded through the approved
  elevated read-only path.
- `rg` search commands returned `rg: command not found`; equivalent bounded
  searches were rerun with `grep` and `find`.
- The initial notes delete/replace attempts were rejected by the patch tool for
  the pre-existing untracked file. The old file was moved recoverably to
  `/tmp/jeios-worker-notes-round1.md`, then the replacement was created with
  `apply_patch`; no temporary file is in the repository.

Checks and builds:

- `git diff --check`: PASS.
- `make all`: PASS with C11,
  `-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes`,
  freestanding baseline `-march=x86-64`, and preprocessed assembly.
- `ASAN_OPTIONS=detect_leaks=0 make check`: PASS; hosted tests, ELF checks,
  ABI isolation, and default dev-inject isolation passed. Leak scanning was
  disabled as requested for this environment.

Boot and integration:

- `/usr/bin/time -p make smoke`: PASS before the change, for the before-time
  measurement.
- `/usr/bin/time -p make smoke`: PASS after the change, with capacity marker
  and preemption count above.
- `make integration`: PASS for fault modes 1–11 and normal mode.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present`: PASS.

No commit, push, git-config change, network operation, secret, binary, or
credential was added by this worker.
