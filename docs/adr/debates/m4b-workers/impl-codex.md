# M4 PR-B worker notes

Observation time: 2026-10-31T02:26:45Z (UTC)
Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4b-impl`
Branch: `wip/m4b-impl`
Base/inspected commit: `1623f1e52f06e2ff06af82c5f055fd4642291a58`
Working tree: uncommitted; no commit, push, network access, or git-config change was performed.

## State

- Implemented: ADR 0010 D3 typed dispatch, D4 PROCESS records/reaper/`PROC_STATUS`, and D-DEV endpoint creation charging in the assigned files.
- Tested: local build, hosted checks, integration fault modes, and all three requested x86 smoke profiles passed.
- Planned: lead integration with the parallel test/documentation work, followed by independent adversarial review and any owner-required contract decisions.
- Blocked: no execution blocker. Remote CI, human review, and the parallel worker’s eventual replacement of the placeholder `test_proc` assertions were not available in this worktree.

## Files changed

Only the assigned implementation files were changed:

- `kernel/core/proc.c`
- `kernel/core/handle.c`
- `kernel/core/endpoint.c`
- `kernel/core/endpoint_kern.c`
- `kernel/core/sched.c`
- `kernel/core/syscall.c`
- `kernel/core/main.c`
- `kernel/arch/x86_64/user_demo.S`
- `tools/smoke.py`

This notes file is the required additional uncommitted handoff artifact. No header or test file was edited.

## Implementation summary

- Replaced the PROCESS stub with the static-pool implementation: generation allocation and retirement, LIVE/DYING/DEAD/FREE transitions, scheduler back-links, handle references, endpoint quota charging, status snapshots, and safe release/reclamation.
- Factored handle resolution so `je_handle_resolve_any` and typed lookup/rights paths share the existing BADINDEX -> STALE -> WRONGTYPE -> DENIED precedence.
- Implemented endpoint `create_by` in the required order: find a free endpoint record, charge the creator, mint both endpoint handles with rollback/uncharge, then publish the creator identity. Endpoint reclamation uncharges exactly once before clearing the creator. The uncharged wrapper delegates through the same path.
- Added kernel endpoint duplicate wiring and current-process endpoint charging. Kernel-seeded endpoint creation remains uncharged because it passes the NONE creator identity.
- Added one kernel PROCESS pool, VM-to-PROCESS registration, thread back-links, death notification, and reaper ordering: capture link, drop user thread, destroy VM, then mark the PROCESS record DEAD.
- Added typed SELF/ENDPOINT/PROCESS dispatch for query, rights, duplicate, and close. PROCESS duplicate retains before handle allocation and rolls back on allocation failure. Endpoint duplicate uses the existing retain-before-commit endpoint path.
- Added `PROC_STATUS` decode and dispatch with reserved-register and 32-bit-handle validation, INSPECT enforcement, a snapshot copy, and fault-safe user copying.
- Added PROCESS handles to every `vm_s3_build` fixture with endpoint limit `JE_EP_POOL_CAP`, while retaining the supervisor=false policy. Added explicit fixture teardown/abandon handling and typed table release checks.
- Added the M4b-s1 ring-3 child/parent fixture and endpoint typed-dispatch checks. The NX smoke expectation now requires the M4b marker.
- Replaced the final halt with the bounded automatic reaper idle loop while retaining the initialization-complete terminal message.

## Commands and results

The normal sandbox rejected shell startup with a loopback/bwrap error. The same commands were then run with the approved escalated read-only/build execution path; this was an environment limitation, not a repository failure.

Read-only preparation and inspection:

- `git show HEAD -- include/jeios/proc.h include/jeios/abi.h include/jeios/handle.h include/jeios/endpoint.h`: PASS; frozen contracts inspected.
- Read `AGENTS.md`, `README.md`, `docs/HANDOFF.md`, `docs/ROADMAP.md`, `docs/DESIGN.md`, `docs/AGENT_START.md`, `docs/CODING_STANDARD.md`, ADR 0010, and `/tmp/m4b/map.md`: PASS.
- `git rev-parse HEAD`: `1623f1e52f06e2ff06af82c5f055fd4642291a58`.
- `git status --short`: only the nine assigned implementation files before this notes file; no header/test changes.
- An attempted `rg` audit reported `rg: command not found`; equivalent `grep`/`sed` inspection was used instead.

Build and checks:

- First `make all`: failed on missing initializers after adding the thread PROCESS field.
- Second `make all`: failed under `-Wmissing-braces`; positional thread initializers were converted to explicit assignments.
- Third `make all`: failed under `-Woverride-init`; the dispatch table initializer was corrected.
- Final `make all`: PASS with the required `-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes` flags.
- `make check`: PASS. All hosted tests and ELF checks passed. In this worktree `tests/test_proc` still prints `proc: PLACEHOLDER (no assertions yet)` because the parallel test rewrite is not present; there was no test failure.
- `ASAN_OPTIONS=detect_leaks=0 make check`: PASS with the same placeholder `test_proc` observation.
- First `make smoke`: failed at `JeiOS fatal: s3 demo size`; the new user fixture exceeded the old 1024-byte demo blob.
- After changing only the S3 fixture blob capacity to one page, second `make smoke`: PASS. M3c markers, M4b-s1 marker, and the terminal initialization marker were observed.
- `make integration`: PASS. Fault modes 1 through 11 all passed, including fault-local D7 and copy-fixup mode 11.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present`: PASS, including `M4b-s1: PROCESS record status/typed dispatch OK`.
- `git diff --check`: PASS after removing a trailing blank line.
- `size build/0/jeios.elf`: PASS; `text=81011`, `data=0`, `bss=2953216`, `dec=3034227`, `hex=2e4c73`.

The smoke harness terminates QEMU after the expected terminal output; its final result was `QEMU smoke: PASS`.

## Non-obvious decisions

1. Frozen headers were not changed. The frozen `syscall.h` does not expose a named action for operation 15, so `syscall.c` uses the literal local action value `13u`, matching the existing action numbering. Rejected: editing the header, because the worker instruction explicitly freezes `include/jeios/*.h`.
2. The PROCESS object type is named locally as `3u` in `endpoint.c`, whose pure endpoint header does not provide the ABI object-type symbol. Rejected: adding an include or changing a header, because the frozen-header boundary is part of this task.
3. Endpoint charging happens only after a free record is found and before any handle minting. Every mint failure uncharges before the record is unwound; successful publication stores the creator last. Rejected: charging after minting, because a second-table failure would temporarily leak quota or require observing an unpublished record.
4. Endpoint reclamation uncharges before clearing `allocated` and `creator`, and process teardown clears the creator identity only through that reclamation path. Rejected: uncharging from generic handle-table teardown, because endpoint queue/peer-close ownership must remain centralized in endpoint reclamation.
5. PROCESS handles retain the record independently of the scheduler link. Reaping changes DYING to DEAD and leaves the record available while handle references remain; the last PROCESS close releases it. Rejected: freeing at VM destruction, because `PROC_STATUS` must work after the process slot is gone.
6. Scheduler teardown captures the PROCESS identity before `je_sched_drop_user`, then destroys the VM, then calls `je_proc_on_reaped`, as required by D4. Rejected: notifying before VM destruction or recovering the link afterward, because the thread slot/link is intentionally cleared during drop.
7. All S3 fixture processes receive records, including fixtures that do not exercise PROCESS handles. Their endpoint limit is `JE_EP_POOL_CAP` and supervisor is false. Rejected: creating records only for M4b, because D-DEV must be exercised by existing M3c ring-3 endpoint tests.
8. Kernel-seeded endpoint creation uses NONE and is uncharged. Current user endpoint creation obtains the scheduler’s current PROCESS identity. Rejected: charging all endpoint creation, because kernel seed/setup has no user PROCESS owner.
9. Typed PROCESS close resolves the object, closes the raw handle, then releases the matching record reference. PROCESS duplicate retains before allocation and releases on allocation failure. Rejected: using SELF duplication or releasing before allocation, because those choices can create a dangling record binding.
10. `PROC_STATUS` copies a local snapshot with `je_copy_to_user` only after all handle and argument checks. Rejected: writing individual fields directly, because a fault must return INVALID without partial user-visible output.
11. The endpoint typed-dispatch fixture decodes the packed endpoint-create result into two 32-bit handles and checks QUERY, RIGHTS, attenuating DUP, and close from ring 3. Rejected: adding a separate fixture process, because the existing M4b parent has the required rights and scratch space.
12. The S3 demo blob was expanded from 1024 bytes to one page after the observed smoke failure. Rejected: shrinking the M4b assembly, because the complete ring-3 assertions are the required behavioral evidence and one page remains bounded by the existing VM fixture contract.
13. The final idle loop ignores the bounded reaper’s integer count but preserves the requested `irq_save -> reap -> sti;hlt` sequence. Rejected: halting on a negative count in the idle path, because existing test callers already fail-stop on reaper errors and the final loop is the production cleanup path requested by the ADR.
14. `vm_s3_destroy` abandons the corresponding PROCESS record after VM destruction for fixtures that are destroyed directly rather than through the scheduler reaper. Rejected: leaving records live, because the final pool baseline check would conflate fixture teardown with a leaked process identity.
15. Documentation files were not updated in this worker. Rejected: editing `docs/HANDOFF.md` or other non-assigned files, because the user explicitly limited this worker’s editable implementation scope; the lead should merge the evidence into the repository handoff.

## Not verified here

- No remote CI result was observed.
- No independent human review was performed; this is an implementation-worker result and remains subject to the requested different-lineage adversarial review.
- The parallel worker’s intended replacement of the hosted PROCESS tests was not present, so the local hosted `test_proc` result is only the existing placeholder, not meaningful behavioral coverage.
- No claim is made that all hardware, drivers, application ABIs, or release/hardened qualification are supported.

Next bounded task: lead integration and review. Pass criteria are a clean merge with the parallel tests/docs, preservation of the frozen-header contracts, rerun of the required build/check/smoke/integration/profile commands, and adversarial review acceptance of the lifecycle and typed-dispatch invariants.
