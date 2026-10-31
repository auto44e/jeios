# M4 PR-C worker notes

Observation: 2026-10-31T11:20:38Z. Repository/worktree:
`wt-m4c-impl`; branch `wip/m4c-impl`; exact inspected base commit
`36cba322ce4ace81050bcae7f31d8f39fb46bc23`. Changes are uncommitted. This is
AI-assisted implementation/review evidence; no independent human review,
release qualification, remote CI, or merged-branch claim is made.

State:

- Implemented: PR-C fixes P2-1, P2-2, P3-1 through P3-6; the accepted
  published-thread rollback rule remains `drop_user -> vm_destroy ->
  on_dying(KILLED) -> on_reaped`.
- Tested: final strict build, leak-disabled host checks, QEMU smoke, integration
  fault modes, and the requested x86 profiles passed.
- Planned: lead integration and adversarial review of this uncommitted diff.
- Blocked: no local blocker.

Files changed:

- `kernel/core/spawn.c`
- `kernel/core/syscall.c`
- `kernel/core/main.c`
- `kernel/arch/x86_64/user_demo.S`
- `tools/smoke.py`
- `WORKER_NOTES.md`

Implementation and decisions:

1. `je_proc_spawn` now applies the effective child handle quota (default 15
   when the argument is zero) before grant resolution or any state mutation.
   `n > handle_quota` returns `JE_ABI_INVALID`. This follows frozen
   `spawn.h` precedence and rejects the quota-1/two-grant case before VM,
   PROCESS, endpoint, thread, or handle changes.
2. All normal spawn exits pass through `spawn_finish`, which clears the
   failpoint. `je_spawn_failpoint_set` disarms step 0, step > 5, and every
   grant index outside `JE_ABI_SPAWN_GRANTS_MAX`. Thus an unfired or malformed
   arm cannot affect a later call.
3. VM-build failure now uses the same `spawn_undo` journal as other
   pre-publication failures. Impossible endpoint and internal statuses call
   the exported `syscall_invariant` fail-stop path rather than becoming a
   ring-3 status. The caller-table preflight now explicitly accepts only
   QUOTA/NOSLOTS after a non-OK result; other values fail-stop.
4. `je_process_handle_mint` reports its allocation status through a
   core-private out parameter. Spawn maps `JE_HANDLE_QUOTA` to
   `JE_ABI_H_QUOTA`; it is not collapsed to `H_DENIED`. Frozen headers were
   not edited.
5. Spawn syscall glue resolves the LAUNCHER with SPAWN before copying the
   user grant array. A caller without launcher authority therefore receives
   the launcher status even when the grant pointer is bad.
6. The M4c kernel fixture audits the newly spawned child with `je_vm_audit`
   in the IF=0 observation section before releasing it to continue. The
   existing child assertions were preserved and reviewed: with n=2, R8 and
   R9 are zero; RIGHTS on RDX is SEND|INSPECT (0x3), and RIGHTS on RCX is
   SEND|RECV|INSPECT (0x7), proving register zero-fill and grant order.
7. The ring-3 parent now performs a quota-1/two-grant spawn and expects
   INVALID plus zero result. A two-way scratch handshake lets the kernel
   snapshot pages, endpoints, PROCESS records, VM slots, parent handles, and
   user-thread count immediately around the call with IF=0; the kernel then
   asserts all baselines are unchanged. The smoke marker records this proof.
8. A first QEMU smoke attempt exposed a fixture-only phase race: the parent
   advanced from phase 10 to phase 8 before the kernel observed phase 10.
   The rejected approach was relying on a transient phase value. The final
   approach adds a reciprocal acknowledgment; the parent waits for the
   kernel's post-baseline acknowledgment before continuing.
9. Decode now uses `JE_VM_HANDLE_QUOTA_MAX` and `JE_EP_CHILD_QUOTA_MAX` and
   removes the dead `handle_quota < 1` condition and duplicated constants.

Commands and results:

- `make all` — PASS after the final code adjustment; strict C11 kernel build
  and link completed with `-Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes`.
- `make check` — PASS before the final defensive preflight switch.
- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS before and after the final
  defensive preflight switch; final run passed hosted tests, ELF checks, ABI
  isolation, and dev-injection isolation. Leak detection was intentionally
  disabled because this environment requires that project-documented setting.
- `make smoke` — first run failed at the fixture-only phase race described
  above; after the reciprocal acknowledgment fix, PASS; final rerun after
  the last status-mapping adjustment PASS. NX-present QEMU reached:
  `M4c-s1: spawn/LAUNCHER/startup-v1/CLOCK_TICKS OK; quota-invalid baseline
  OK; fail-Nth rollback OK (steps 1-5); spawn ticks=...`.
- `make integration` — PASS for fault modes 1 through 11, including the
  normal NX-present run and exception-fixup mode.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present` — PASS.
- `git diff --check` — PASS at final inspection.

Not verified or not claimed:

- No remote CI, ARM execution, physical hardware, SMP, all-driver support,
  general application ABI, independent human review, or hardened/release
  qualification is claimed.
- The failpoint remains compiled into the normal kernel because the QEMU
  fixture is its evidence path; it is not reachable through the ring-3 ABI.
- `docs/HANDOFF.md` and the implementation ledger were not edited because
  this worker was restricted to the listed implementation/fixture/smoke
  files plus this notes file. The lead must carry the final integrated status,
  exact final-head evidence, and next bounded task into the handoff.

Next bounded task:

- Lead integration and adversarial review of the uncommitted diff.
- Pass criteria: preserve the frozen header contracts, keep the quota-invalid
  probe at INVALID with unchanged baselines, retain child audit and startup
  register assertions, and rerun the final `make all`, sanitizer check,
  `make smoke`, `make integration`, and three x86 profiles on the integrated
  head.
