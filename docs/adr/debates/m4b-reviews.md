# M4 PR-B reviews (2026-10-31, AI-assisted; not human qualification)

Worker mode: Codex wrote the implementation and Claude reviewed it. Claude wrote the tests from the headers only and Codex reviewed them. Lead: Kiro. Reviewer outputs are verbatim.

## Round 1 — Claude reviewing the Codex implementation

I found no P1. Nothing I traced revives authority, reuses a record, or underflows a count. Four P2s are fail-open or fail-silent spots at Critical-profile boundaries. This was a read-only review: I did not build or run `make` or `smoke`.

**P2**

1. **P2: a PROCESS handle can be minted into a table whose teardown never releases it.** `kernel/core/syscall.c:239` (`je_process_handle_mint`)
   - The endpoint create path refuses a VM that lacks the teardown wiring (`endpoint_kern.c:79-83`). Mint has no such check.
   - If a PROCESS handle lands in a table whose VM has no `kep_teardown`, `je_vm_destroy` zeroes the slot without calling `je_proc_release`. The child record then stays DEAD with `handle_refs` above zero forever, and the 48-record pool slowly runs out.
   - Fix: change the signature to `(je_vm*, index, id, rights, out)` and apply the same wiring check (teardown, ctx and IRQ pair). Otherwise return `JE_ERR_INVALID` before retaining.

2. **P2: invariant breaks in the syscall paths are returned to ring 3 instead of stopping the kernel.**
   - Affected lines: `syscall.c:434-441` (PROC_STATUS → `H_STALE`), `:532` (CLOSE → `H_WRONGTYPE`, the slot stays LIVE), `:636` (DUP → `H_WRONGTYPE`).
   - A LIVE PROCESS slot whose object does not map to a non-FREE record means the refcount is already broken (a use-after-free in progress).
   - `je_endpoint_release_table` treats the same condition as fatal and halts through `kep_teardown`. The syscall paths hide it and let the process carry on.
   - Fix: on that branch, call the existing fatal path (console message plus `je_arch_halt`, or `abort_scheduler`), as the release walk does.

3. **P2: a user thread with no identity creates endpoints without being charged.** `endpoint_kern.c:88`
   - When `je_sched_current_proc_id` fails, the creator falls back to NONE and the channel is created uncharged. That is fail-open.
   - Today every ring-3 thread comes through `vm_s3_build`, so all are registered. But any future spawn path that forgets `je_sched_register_process` gets unlimited channel creation, which is exactly what D-DEV exists to prevent.
   - Fix: NONE should be allowed only for a kernel-thread caller. If the current thread is `THREAD_USER` with `proc_id.gen == 0`, return `JE_EP_CORRUPT`.

4. **P2: the idle reaper throws away its error result.** `main.c:1205`
   - `je_sched_reap_dead` tears down in steps: `drop_user` clears the thread slot first, then `vm_destroy`, then `on_reaped`.
   - If `vm_destroy` or `on_reaped` fails, the slot is already gone and the record stays DYING forever. No later reap pass can find it again, and the dropped return value hides it.
   - Fix: `int r = je_sched_reap_dead(); if (r < 0 && r != (int)JE_ERR_BUSY) fail("JeiOS fatal: idle reap\r\n");`. Separately, consider making `reap_dead` abort the scheduler itself on any failure after `drop_user`.

**P3**

5. **Registrations are never cleared.** `sched.c:75-81`
   - `registered_proc[index]` and `registered_vm` keep the old identity after reap or abandon. There is one global `registered_vm`, so registering a different VM silently re-points entries that belong to the old one.
   - Nothing is revived, because `je_proc_link` rejects a non-LIVE or wrong-generation id. But this is a stale identity stored in kernel state.
   - Fix: clear `registered_proc[idx]` in `reap_dead` and `drop_user`, and key the table per VM, or pass the id to `je_thread_create_user` explicitly.

6. **`je_proc_link` accepts a record that is already linked.** `proc.c:85`
   - A second link overwrites `thread_slot`. Two threads on one record means a double `on_reaped`, and the second one returns `JE_ERR_INVALID` from `reap_dead`.
   - Fix: reject when `run_linked` is already set.

7. **`drop_user` edits the record directly.** `sched.c:513-519`
   - It writes `run_linked`, `thread_slot` and `vm_index` itself, bypassing `proc.c`.
   - On the fixture path (`drop_user` on a LIVE thread, then `vm_s3_destroy` → `je_proc_abandon`), a record that was published and ran is "abandoned". That stretches the D5 "unpublished" contract.
   - Fix: add `je_proc_unlink()` to `proc.c`. Document, or `_Static`-guard, that abandon after a run is a fixture-only path. PR-D kill must go through `mark_dying`.

8. **Underflow and no-op guards hide refcount bugs.**
   - `proc.c:34` (`used_count` guard) and `proc.c:133` (release with zero refs or an invalid id) silently do nothing.
   - The header specifies the release no-op, so keep that API behaviour. But callers that have just closed a live PROCESS slot (`syscall.c` CLOSE, `endpoint.c` walk) should check `handle_refs > 0` first and stop the kernel otherwise.

9. **ABI value pinning is missing.**
   - `endpoint.c:10` re-defines `JE_PROCESS_OBJ_TYPE 3u` with no assert against `JE_ABI_OBJ_PROCESS`.
   - `je_proc_state` (LIVE/DYING/DEAD) is copied raw to user space with no assert against `JE_ABI_PROC_*`.
   - The `death_reason` enum in `sched.c` has no assert against `JE_ABI_DEATH_*`.
   - Fix: add `_Static_assert`s for all three.

10. **The dispatch table doesn't match the ADR shape.** `syscall.c` `type_dispatch`
    - ADR D3 specifies `{known_rights, dup_policy, inspect_result, retain, release}`. The code has only the first two fields, with retain and release hand-coded at three call sites (CLOSE, DUP, and the walk in `endpoint.c`).
    - Behaviour matches the ADR today, but this deviation should be recorded, or the hooks added.

11. **IF=0 discipline isn't enforced.**
    - `proc.h` requires IF=0 for every call. `vm_s3_build` (`je_proc_alloc`, register), `je_process_handle_mint` in the fixture, and `je_thread_create_user` → `je_proc_link` right after the M4b child-wait loop (which exits with IF=1) all run with IF unspecified.
    - Nothing racing touches the pool there today, but `used_count++/--` is a non-atomic read-modify-write.
    - Fix: wrap the kernel callers in `irq_save`/`irq_restore`, or assert IF=0 in a kernel wrapper.

12. **`je_endpoint_kern_dup` lacks the A3-2 wiring check** (`endpoint_kern.c:93`). It isn't reachable unwired today, because a table with an endpoint handle is already wired. Mirror the check from create for defence in depth.

13. **Fixture byte overlap.** `user_demo.S:235` stores 8 bytes (`movq`) at `0x1714`, which overlaps `0x1710` and the `0x1718` PROC_STATUS output. It's harmless because the output is rewritten later, but it corrupts the evidence word. Use `0x1738` or a 4-byte store.

14. **Test-claim gap.** The fixture sets `ep_limit = JE_EP_POOL_CAP`, so the creation-quota path is never exercised in QEMU. QUOTA evidence is hosted-only and the PR should say so. A creation-quota failure and a handle-table quota failure both surface as `H_QUOTA`, which is fine per the ADR but worth documenting.

**Confirmed sound**
- **No revival.** A PROCESS handle object is `&rec[i]`, and its identity is rebuilt from the record's current generation. That is safe because a record is freed only at DEAD with zero refs, or by abandon with zero refs, so a live handle always pins the generation. Cap transfer resolves only ENDPOINT, so PROCESS handles can't leave their table.
- **DUP of PROCESS:** retain comes before `handle_alloc`, and an alloc failure releases once. It can't free the record because the source still holds a ref. `REF_MAX` gives BUSY → `H_REFOVERFLOW`, and the rights-subset check (with KNOWN enforced at decode) comes before the policy check.
- **CLOSE of PROCESS:** releases exactly once, after the slot is freed. SELF and not-live statuses are unchanged, because `resolve_slot` gives the same BADINDEX/STALE as `je_handle_close`.
- **Teardown walk:** each live PROCESS slot is closed and released once. SELF is skipped. The post-condition now covers PROCESS. The walk runs while the record is DYING, and the DEAD-or-free decision is left to `on_reaped`.
- **Uncharge:** gated on the exact identity in any non-FREE state. A freed or reused creator is a no-op, which matches D-DEV. All four create-rollback paths uncharge, and `creator` is set only on commit.
- **Error order:** NOSLOTS before QUOTA, as `endpoint.h` specifies.
- **Reap order** (capture → `drop_user` → `vm_destroy` → `on_reaped`) runs inside one IF=0 section, and `reap_dead` enforces IF=0.
- **PROC_STATUS:** precedence is handle → WRONGTYPE → DENIED. The 16-byte padding-free snapshot goes through the prevalidated `copy_core`, so no partial write is possible on a single CPU.
- **QUERY/RIGHTS/DUP:** the only M3 behaviour change is the intended ENDPOINT/PROCESS typing. The SELF results are identical.
- **Idle reaper loop:** IF discipline is correct (`irq_save` → reap → `sti; hlt`), and each pass is bounded by `JE_THREAD_MAX`.

## Round 1 — Codex reviewing the Claude tests (and the code they exercise)

Review at `fa17e6b`; repository untouched and `make` not run.

1. **P1 — user endpoint creation can bypass D-DEV quota.** `kernel/core/endpoint_kern.c:86-89` converts any failed `je_sched_current_proc_id()` into `JE_PROC_ID_NONE`, which disables charging. A user thread created without a registered PROCESS identity can mint unlimited endpoints. Fix: fail closed for user callers; reserve `NONE` for explicit kernel-seed paths.

2. **P1 — terminal PROCESS identities can still create endpoints.** `kernel/core/proc.c:149-156` charges any valid non-FREE record, including DYING/DEAD. A stale-but-valid terminal identity can revive creation authority through `create_by`. Pin and enforce `state == LIVE` for creation.

3. **P1 — reaper ordering/error handling leaves inconsistent records.** `kernel/core/sched.c:513-518` clears the PROCESS run link before `je_vm_destroy`; `kernel/core/main.c:1203-1206` ignores a negative reaper result. A post-drop failure leaves a DYING record unlinked and the idle loop continues. Fix: clear the record link only in `je_proc_on_reaped()` after VM teardown, and fail-stop on negative reaper results.

4. **P2 — detached charging stores an active creator identity.** `kernel/core/endpoint.c:456-506` stores `creator` even when `pool->procs == NULL`; later reattachment can uncharge the wrong pool/record at `endpoint.c:135`. Fix: store `NONE` when uncharged, or store an immutable charged-pool/charged-bit and reject pool rebinding while records live.

5. **P2 — automatic reaping is not tested.** `kernel/core/main.c:1174` and `1191` manually reap both M4b processes, so a broken idle-loop reaper passes the fixture. Fix: remove those direct calls and wait for `PROC_STATUS == DEAD` while the idle loop performs the reap.

6. **P2 — PROC_STATUS decoder test accepts the wrong action.** `tests/test_syscall.c:138-155` only requires a new action distinct from existing ones. A mutant returning action `13` passes. Fix: assert `a == JE_SYSCALL_PROC_STATUS`; update the stale comment now that `syscall.h:28` defines it.

7. **P2 — lifecycle cleanup tests have survivors.** `tests/test_proc.c:255-305` does not test `abandon()` on a linked LIVE record, and only checks `run_linked`, not `thread_slot/vm_index`. Mutants survived:
   - removing the `run_linked` abandon guard;
   - removing `thread_slot`/`vm_index` clearing in `on_reaped`.

   Fix: add both assertions, or pin that `run_linked == false` alone is sufficient.

8. **P2 — `alloc()` contradicts its own NOMEM contract.** `kernel/core/proc.c:53-67` retires exhausted slots and may then return `JE_ERR_NOMEM`, despite `proc.h:78` promising “nothing changes.” `test_proc_retire.c:113-140` only checks a subsequent NOMEM. Fix: either make retirement-on-final-search explicit and test it, or make NOMEM transactional.

9. **P3 — death reasons are not validated.** `kernel/core/proc.c:96-105` stores arbitrary `uint32_t reason`; ABI permits only 0–3. Fix: define the proc-layer bound and reject values above `JE_DEATH_KILLED`; add a failure test.

Ambiguity triage: A1 is resolved in the current header but the test is stale; A2–A10 and A12 are real contract gaps needing a decision (A8 is security-critical); A11 is cosmetic only. The tests over-pin stale/invalid `create_by` to “non-OK” without a header-defined status (`tests/test_endpoint.c:388-391`); either document fail-closed behavior or remove that assertion. The NOSLOTS-before-charge and `NONE`/`procs == NULL` behavior are explicitly pinned and are not over-pinning.

Mutation summary: `resolve_any` type-check mutant and generation-wrap mutant were killed. The three survivors were the abandon-link guard, reap-field clearing, and wrong PROC_STATUS action.

confirmed sound

- Exact `(index, generation)` validation and stale rejection.
- Reference overflow/underflow handling and DEAD-record retention.
- `resolve_any` output preservation and absence of type/rights filtering.
- `create_by` free-record-before-charge ordering and transactional mint rollback.
- PROC_STATUS rights check plus snapshot-before-copyout structure.
## Lead disposition of round 1 (contract pins in `5c5a3c2`)

- **Fail-open identity (Codex P1-1 = Claude P2-3): accepted.** A user thread with no PROCESS identity must not create channels uncharged. Fail closed; NONE is reserved for the kernel seed.
- **Terminal identity still charged (Codex P1-2 = Claude A8): accepted and pinned.** Only a LIVE record can be charged.
- **Reaper (Codex P1-3, Claude P2-4, the lead's own note): accepted.**
  - Only `on_reaped` clears the run link, and `drop_user` no longer touches the record.
  - A failure after `drop_user` is fatal.
  - One idle-step function is shared by the final idle loop and the M4b fixture. The fixture's manual reaps are removed, so QEMU now proves the automatic reaper (Codex P2-5).
- **Abandon after a run (Codex mutant, Claude P3-7, the lead's own note): resolved by pinning.**
  - "Published" means run-linked.
  - Abandon is spawn rollback only.
  - The direct teardown of a live process by a fixture goes through `on_dying(KILLED)` and then `on_reaped`. This is honest: the kernel test harness is killing it.
- **Header ambiguities A2–A10 and A12: pinned in proc.h / endpoint.h**, with the proposer recorded inline. A3 (QUOTA overloaded) is kept deliberately, because ADR 0010 adds no new statuses. A11 (comment placement) is fixed.
- **Claude P2-1 (mint into an unwired table) and P2-2 (fail-stop on invariant breaks in syscalls): accepted.** So are all of Claude's P3 items, including the ABI `_Static_assert` pins and retain/release hooks in the dispatch table to match ADR D3. Claude P3-14 is accepted too: one QEMU creation-quota check is added, so quota evidence is no longer hosted-only.
- **Codex P2-6 (PROC_STATUS action not pinned): accepted.** The lead added `JE_SYSCALL_PROC_STATUS` to `syscall.h` during integration, replacing the worker's local `#define 13u`.
