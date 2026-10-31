# M4 PR-C worker notes

Observation: 2026-10-31T10:24:27Z. Repository: `wt-m4c-impl`, branch/commit at pickup was
`1f00b93d68fd7f330fdd4fbb3c729b730ab45f1b`. Changes are uncommitted, as
required. This is an AI-assisted worker contribution; no independent human
review or release qualification is claimed.

State:

- Implemented: startup-register thread construction, shared handle allocation
  preflight, built-in spawn-child image table and validation, journaled
  `je_proc_spawn`, CLOCK_TICKS and PROC_SPAWN decode/glue, LAUNCHER typed
  dispatch, and the M4c assembly/kernel fixture scaffolding.
- Tested: `make all` passed with the requested freestanding warning flags
  (`make: Nothing to be done for 'all'` after the final source build);
  `git diff --check` passed.
- Planned/blocked: the QEMU M4c fixture does not yet reach the completion
  marker. Hosted tests are stale in this worktree and the required checks that
  depend on them are not green.

Files changed (only assigned implementation files plus this note):

- `kernel/core/sched.c`
- `kernel/core/handle.c`
- `kernel/core/image.c`
- `kernel/core/spawn.c`
- `kernel/core/syscall.c`
- `kernel/core/main.c`
- `kernel/arch/x86_64/user_demo.S`
- `tools/smoke.py`

Decisions and non-obvious points:

1. `je_handle_can_alloc` and `je_handle_alloc` now share one preflight helper.
   This preserves QUOTA-before-NOSLOTS and lowest-free-slot behavior without
   creating a second allocation policy.
2. Startup registers are written only after the frame has been zeroed. The
   legacy creator is a NULL-startup wrapper. The output slot is written only
   after every fallible creation/link operation succeeds.
3. Spawn validates the launcher, image, quotas, grant rights and grant source
   descriptors before mutation. It registers the process before the thread so
   the existing scheduler back-link is installed by thread creation. Rollback
   is drop-user (only if created), VM destroy (only if READY), abandon, with the
   caller table never allocated until publish.
4. Internal VM NOMEM/BUSY map to H_NOSLOTS as pinned by `spawn.h`. Spawn does
   not call `je_vm_audit`; the fixture's existing build helpers audit their
   own images.
5. ADR D3 requires RIGHTS to hold INSPECT. The requested fixture text also says
   the child grants are SEND-only/SEND|RECV and asks RIGHTS to return those
   masks. Those requirements conflict: the literal frozen security contract
   makes RIGHTS return H_DENIED for both handles. The child issues RIGHTS on
   both and accepts H_DENIED, preserving no privilege escalation. This
   discrepancy needs owner/lead resolution before claiming the exact requested
   positive RIGHTS evidence.
6. The child fixture was made to pause after sending so the kernel can inspect
   the child while its VM is still live. During debugging, auditing a spawned
   child after it had entered DYING stalled in the real backend; no audit was
   added to spawn. The current fixture uses the pause/release mechanism, but
   the child-audit assertion still needs a clean, independently verified
   placement before merge.

Commands and observed results:

- `make all` — PASS at the successful build checkpoint; freestanding C and
  assembly linked with `-Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes`.
- `git diff --check` — PASS.
- `make check` — FAIL in the pre-existing/stale hosted `tests/test_syscall.c`:
  it still expects `HANDLE_DUP(self, 0x20)` to decode INVALID, while
  `JE_ABI_RIGHT_SPAWN` makes 0x20 a known-but-unheld right and the new literal
  result is DENIED. The hosted test update is owned by the parallel worker.
- `ASAN_OPTIONS=detect_leaks=0 make check` — FAIL at the same stale hosted
  `tests/test_syscall.c:133` assertion; all earlier listed hosted tests passed,
  and no sanitizer diagnostic was emitted before the abort.
- `make smoke` — FAIL/TIMEOUT after the M4b markers. The image built and QEMU
  reached the M4b fixture, but no M4c marker or fatal diagnostic was emitted
  before the emulator timeout.
- `make integration` — fault modes 1--10 passed; the normal mode then
  timed out after the M4b markers for the same M4c fixture hang.
- Required 3 x86 profiles (`qemu64,-nx`, 16 MiB, and 1536 MiB with NX) — not
  run to completion because the normal NX smoke profile is already blocked at
  the M4c timeout; no profile pass is claimed.

Not verified:

- Full rollback counter equality across all requested QEMU fail-Nth cases.
- Final ring-3 negative-spawn sequence and measured successful-spawn tick
  marker.
- `make check` with the parallel hosted-test updates.
- `make smoke`, `make integration`, and all requested memory/NX profiles at a
  passing M4c head.

Next bounded task: lead/next worker should resolve the D3 RIGHTS ambiguity,
finish the live-child fixture audit/teardown timing, then run the complete
verification matrix. Pass criteria are the exact M4c marker in the NX profile,
no fatal output, all fail-Nth baselines unchanged, and the requested hosted,
smoke, integration, and three x86 profile commands passing.
