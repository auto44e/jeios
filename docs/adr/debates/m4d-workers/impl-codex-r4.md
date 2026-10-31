# M4 PR-D worker notes

Observation time: 2026-10-31T16:28:24+00:00

Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4d-impl`

Branch: `wip/m4d-impl`

Inspected commit: `74da01718694326d82b2f72ccc88f528167cbba7`

## State

- Implemented: M4d now proves synchronous `PROC_KILL` behavior for READY,
  BLOCKED_IPC, and WOKEN victims. For each kill, the killer's next syscall is
  `PROC_STATUS` on the same victim, and that status must already be DYING with
  reason KILLED.
- Implemented: the supervisor guard is exercised against a supervisor record
  that is run-linked to a real heartbeat thread. A kill attempt must return
  INVALID while the record remains LIVE and the heartbeat thread continues to
  dispatch.
- Implemented: the EXIT first-reason race is ordered by a bootstrap-written
  gate. The killer waits until the bootstrap has observed DYING/EXIT and exit
  code 42, then re-kills; the exit code is also checked through `PROC_STATUS`.
- Tested: the final local build, hosted checks, smoke, integration, three x86
  profiles, and five repeated NX smoke runs passed.
- Planned: lead integration, then the lead's supervisor-guard and
  `mark_dying` mutants must both fail under `make smoke`.
- Blocked: no required local command remains blocked. Remote CI, ARM
  execution, independent human review, and the lead's mutant runs were not
  performed in this worktree.

## Changed files

- `kernel/core/main.c`
  - Builds the supervisor with the existing fixture builder using
    `supervisor=true`, creates its real user thread, and verifies
    `je_proc_run_link` points at that VM before enabling interrupts.
  - Keeps the unlinked negative case as a LIVE PROCESS record without a VM.
    This preserves the fixture's five-VM peak and still tests status,
    kill-rights, and cleanup for an unlinked record.
  - Removes the flaky killer dispatch-count equality assertion.
  - Waits for EXIT/42 observation before allowing the killer's re-kill.
  - Verifies the supervisor record remains LIVE and its heartbeat and dispatch
    count advance after the rejected kill.
  - Tears down the real supervisor through the normal scheduler/VM path after
    all guard assertions. The comment records that PR-F must revisit this
    teardown when D12 supervisor-loss handling exists.
  - Does not modify `mark_dying`, frozen headers, or the existing reaper path.
- `kernel/arch/x86_64/user_demo.S`
  - Documents and enforces the immediate-next-syscall `PROC_STATUS` sequence
    for READY, BLOCKED_IPC, and WOKEN kills.
  - Adds the EXIT observation gate and checks exit code 42 from the status
    result before the later re-kill.
  - Updates the supervisor commentary to describe the real run-linked
    heartbeat-thread guard.
- `tools/smoke.py`
  - Updates the expected M4d marker from `synchronous-return` to
    `next-syscall PROC_STATUS`.
- `WORKER_NOTES.md`
  - Replaced with this current, uncommitted handoff.

Frozen `include/jeios/*.h` headers were not edited. No commit, push, network
operation, or git-config change was made.

## Decisions and rejected alternatives

1. The supervisor fixture uses `vm_s3_build_quota_kind(..., true)` and a real
   user heartbeat thread. This makes the supervisor check decisive: removing
   the supervisor guard would allow the kill path to reach a valid run-linked
   target and kill it, instead of failing earlier because the record is
   unlinked. The rejected alternative was the prior kernel-seeded LIVE record,
   which made the guard test vacuous.
2. The unlinked negative case remains record-only. An initial attempt created a
   sixth simultaneous VM for it; the resulting smoke boot failed at
   `JeiOS fatal: s3 build`, so that alternative was rejected as exceeding the
   fixture's available VM/resource profile. A record-only case preserves the
   intended unlinked contract without changing the five-VM peak.
3. The bootstrap writes the EXIT gate only after observing DYING, reason EXIT,
   and code 42. The killer waits on that gate before its re-kill. This was
   chosen to make first-reason ordering an observed precondition rather than a
   scheduling race. The rejected alternative was allowing the killer to race
   the exit victim and merely checking whichever reason happened to win.
4. The dispatch-equality “synchronous-return” assertion was removed. Dispatch
   counts are scheduler observations and do not prove that the syscall
   following `PROC_KILL` was the status syscall. The replacement directly
   checks syscall adjacency in the user image and the returned DYING/KILLED
   state for each victim class.
5. Supervisor teardown uses `je_sched_drop_user` followed by `vm_s3_destroy`,
   then abandons the record-only unlinked case. This follows the normal fixture
   teardown path; the PR-F comment explicitly leaves D12 supervisor-loss
   semantics for that future change. No attempt was made to add D12 behavior.

## Verification

All commands below were run locally at the inspected commit. No network,
remote CI, ARM execution, or independent human review was performed.

- `git diff --check` — PASS.
- `make all` — PASS; strict C11/freestanding kernel and assembly build
  completed and linked.
- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS; all hosted core, boot,
  allocator, VM, uframe, syscall, handle, endpoint, process, retirement, FDT,
  ELF, ABI-isolation, and dev-injection isolation suites passed.
- `make smoke` — PASS; rebuilt `build/0/jeios.iso`, emitted the exact M4d
  marker, reached kernel initialization completion, and reported
  `QEMU smoke: PASS`.
- `make integration` — PASS; integration fault modes 1 through 11 passed.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present` — PASS.
- Five concurrent
  `python3 tools/smoke.py build/0/jeios.iso --expect-nx present` runs after
  the final `make smoke` — PASS, all 5 emitted the M4d marker and
  `QEMU smoke: PASS`.

Historical verification retained for accuracy:

- Before the record-only correction, `make smoke` rebuilt the ISO but failed
  at `JeiOS fatal: s3 build` when the unlinked case was also a sixth VM.
  That failure led to the bounded five-VM fixture correction.
- The requested supervisor-guard mutant and no-`mark_dying` mutant were not
  run here; the lead owns those adversarial reruns after integration.

## Handoff

State is implemented and locally tested; planned work is lead integration and
the two adversarial mutant checks. The next bounded pass criterion is that
removing the supervisor guard causes `make smoke` to fail because the
run-linked supervisor is killed, and removing `mark_dying` causes the
immediate post-kill status assertions to fail. The implementation does not
claim D12 supervisor-loss handling, ARM support, remote CI, or independent
human qualification.

Repository-level `docs/HANDOFF.md` was not edited because this worker's
permitted scope was the three listed implementation files plus this
uncommitted notes file.
