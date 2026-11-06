I read all the files in my slice at `c6914ca` and ran 28 targeted mutants against the hosted tests. **Only 6 were killed; 22 survived.** The worst gaps are in the VM audit (`je_vm_audit`), the device dispatch grant, the endpoint RECV checks the syscall path actually uses, and kill-all. Two real defects stand out: `je_sched_reap_dead` reports errors with the wrong sign, and the `test_sched_killall` Makefile rule silently reuses stale binaries.

**How this was produced.** This is an AI-assisted review, not independent human qualification. The mutants ran on a `git archive HEAD` copy in `/tmp/jm`, hosted tests only. I did not run QEMU or integration, so some hosted survivors may still be killed in integration; I note where I found integration coverage. I picked mutants where I suspected gaps, so 6/22 is not a score for the whole suite. Your working tree is untouched. There is an untracked `tools/mutants.py` + `tools/mutants.json` in it that I didn't create and left alone. It already deletes the binary before each build and reports a build failure as BLOCKED, which is right.

## Mutation evidence (hosted, forced rebuilds, no `-Werror` artifacts)

| Result | Mutants |
|---|---|
| **KILLED (6)** | VM-VALIDATE-NO-OVERLAP, EP-CAPDESC-DEAD-IS-LIVE, HANDLE-NO-RIGHTS, UFRAME-KEEP-IOPL, PROC-ABANDON-LINKED, SCHED-KILLALL-NO-WAITPENDING-CLEAR |
| **SURVIVED: sched (4)** | KILLALL-SPARE-BY-FLAG, MARKDYING-RELEASE-WRONG-SLOT, KILLALL-NO-LINK-CHECK, KILLALL-NO-PROCSTATE |
| **SURVIVED: vm audit (6)** | NO-ALIAS, NO-PERM, NO-ONE-TO-ONE, NO-GUARD, NO-COMPLETENESS, NO-XPROC |
| **SURVIVED: device (4)** | DISPATCH-ANY-USER, ACK-ANY-CALLER, DEATH-NO-REVOKE, ASSIGN-NO-QUOTA-PREFLIGHT |
| **SURVIVED: endpoint (6)** | PEEKCAPS-NO-RECV, PEEKCAPS-NO-RESERVED-GUARD, WAITREG-NO-RECV, WAITREG-NO-INFLIGHT, SENDCAPS-NO-REFOVERFLOW, RELWALK-SKIP-PROCESS |
| **SURVIVED: decode (2)** | SEND-LEN `>`→`>=`, SEND-CAPCOUNT `>`→`>=` (availability only, not security) |

Two traps showed up while running this:
- On the first pass every sched mutant falsely "survived" because `Makefile:221` lists `device.c` but not the `sched.c` the test `#include`s, so the binary never rebuilt.
- Mutants that delete a line can fail to build under `-Werror` (unused variable), which looks like a kill but isn't. Write them as `if (0 && (cond))` instead.

## (A) Correctness / logic

- **A1 — `sched.c:585-611`, `:616` — `je_sched_reap_dead` returns errors with the wrong sign.**
  - Observation: it returns `(int)JE_ERR_BUSY`, which is **+5** (`status.h:6`), while `sched.h:103` promises a negative value on violation. Every `< 0` check is dead: `je_kernel_idle_step` (`sched.c:616`) and `main.c` 1569, 1612, 2080, 2144, 2170, 2194, 2253, 2769, 3787.
  - Why it matters: a still-active CR3/RSP0, or a `je_sched_drop_user` → `je_arch_page_restore` failure, is reported as "5 reaped". The DYING slot stays and the idle loop retries forever instead of halting.
  - Fix: `return -(int)s`, or change the signature to `je_status (uint32_t *reaped)`. Not hosted-testable today because of `pushfq` (`:591`); add `je_arch_irqs_enabled()`. **Owner-gated (M3a).**
- **A2 — `syscall.c:416` + `endpoint.c:96-112` — one status means two different things.**
  - Observation: `JE_EP_CORRUPT` covers both "vm not wired" (expected configuration, `endpoint_kern.c:99,112,127`) and "a handle names an end of an unallocated record" (a dangling handle, i.e. use-after-free evidence).
  - Why it matters: both map to `JE_ABI_UNSUPPORTED`, so the use-after-free case is handed back to ring 3 instead of halting the kernel.
  - Fix: split into UNWIRED vs CORRUPT, and send CORRUPT to `syscall_invariant`. **Owner-gated (M3c).**
- **A3 — waiter identity is a bare slot number.**
  - Observation: `je_sched_wake_ipc(slot)` (`sched.c:439-443`) wakes any BLOCKED_IPC thread in that slot without checking which endpoint it parked on. Both release-by-id functions stop at the first match (`endpoint.c:375-384`, `endpoint_kern.c:379-394`).
  - Why it matters: it is correct today only because `mark_dying` and `drop_user` always release before a slot is reused. Any future path that misses a release becomes an ABA (stale-identity) bug.
  - Fix: `wake_ipc(slot, rec, side)` asserting `wait_rec`/`wait_side` match; make release-by-id scan the whole pool and halt on a second match. **Owner-gated (M3c API).**
- **A4 — `sched.c:404-430` — deadline 0 parks forever.** The comment says a 0 deadline is a caller bug, but nothing checks it, and the timeout sweep skips 0 (`:340`). Fix: `if (!deadline) abort_scheduler();`.
- **A5 — `sched.c:174-230` — "one thread per vm process" is enforced only by accident.**
  - Observation: what actually stops a second create is that `je_arch_page_guard` fails on an already-guarded page (`paging.c:65`), plus `je_proc_link` for registered processes. I first suspected double-create corruption; this guard prevents it.
  - Fix: make it explicit with `je_sched_has_process()`. The ARM guard backend may not share this behaviour. **Owner-gated.**
- **A6 — `syscall.c:1090`, `:457` — kernel buffer safety rests on a queue invariant.** The user's `cap` (RDX, unbounded) is passed as the capacity for the 64-byte `staging` buffer. It is safe only because every queued message is at most `JE_EP_MSG_MAX`. Fix: clamp to `sizeof staging`.
- **A7 — `vm_backend.c:177` — no NX means executable data.** With `nx == 0`, data and rodata leaves are installed executable. `vm_build` already refuses when NX is absent, but the backend should refuse too. Also document that the no-op `vb_flush` (`:280`) is valid only with no PCID and never destroying the active root (relevant for ARM/PCID).
- **A8 — `endpoint.c:591` — dead `owner` parameter.** `je_endpoint_release_table_owned` does `(void)owner`; the name promises scoping it doesn't do. Either use the owner or drop the parameter.
- **A9 — `sched.c:504` — `je_thread_sleep` has no thread-kind check.** Called on a user thread it would abort the kernel. Return DENIED instead.

## (B) Structure / maintainability

- **B1 — core spawn depends on the test harness.** `spawn.c:259` calls `je_proc_record_image`, which is defined in `main.c:371` (the harness) and can `fail()`, halting the kernel. `je_kernel_main` is **1972 lines** (`main.c:1901`) with 29 `JE_TEST_MODE` blocks. Fix: an observer hook that is a no-op in core; move the harness to `kernel/arch/x86_64/selftest/`, split per fixture.
- **B2 — `extern` declarations in `.c` files.**
  - Locations: `spawn.c:10-20`, `syscall.c:28-33` and `:301`, `endpoint_kern.c:104-105` (inside a function body), `main.c:24-33`.
  - Why it matters: C does not check these against the definitions, so a signature change is silent undefined behaviour.
  - Fix: move them into headers. I measured `-Wmissing-prototypes -Wnested-externs -Wredundant-decls`: only 20 warnings across core (sched 7, endpoint_kern 7, syscall 3, main 2, spawn 1), so the fix is cheap.
- **B3 — duplicated recv delivery.** `je_syscall_dispatch` is 454 lines. The mint → copy → rollback → commit sequence is duplicated between the non-blocking recv (`syscall.c:1100-1158`) and the blocked-recv finish (`:472-528`). Fix: one `deliver_peeked()` helper. **Owner-gated (M3c).**
- **B4 — duplicated RECV checks.** The RECV right is checked separately in four functions (`endpoint.c:268`, `:290`, `:721`, `:927`). The mutants show the untested copies are the ones the syscall path uses. Fix: a single `resolve_with_right()`.
- **B5 — stale contract text.**
  - `endpoint.h:4-45` still says "NOT here: messages, transfer, waits" and "includes ONLY handle.h".
  - `endpoint.c:4` says "no ABI include", but `:9` includes `abi.h`.
  - `JE_EP_MAX_WAITERS 2` is marked "unused" while ADR rule A6-4 pins one waiter per end.
  - The generation comment says it is "not yet READ", but it is read now.
  - `sched.h:16` says "no dynamic slot reuse", but slots are reused.
- **B6 — x86 code inside core `sched.c`.** `pushfq` (`:591`), `int $0x81` (`:514`, `:522`), `sti; hlt` (`:617`), and RFLAGS bit-9 tests (`:592`, `:709`, `:778`). `device.c` is really a COM2/8259 driver in core: it calls `irq3_mask`, `master_isr/eoi`, `tss_set_com2_io`. Fix: `je_arch_*` hooks, and give the device layer a backend ops table like `je_vm_ops`. This is a prerequisite for the neutral scheduler. **Owner-gated.**
- **B7 — test-only state in production objects.** `je_device` carries about 20 test-observation fields (`device.h:28-49`). Dead parameters: `device.c:295`, `:319-320`, and `sched.c:294` computes a handle-table pointer only for it to be ignored.
- **B8 — writable global tick counter.** `je_timer_ticks` is an exported, writable `volatile` (`sched.h:119`) alongside the `je_sched_ticks()` accessor, and `register_probe.S:58` reads the global directly. Make it static behind the accessor.
- **B9 — small items.**
  - Scans such as `je_sched_has_process` rely on `THREAD_KERNEL == 0` so empty slots never match; add a static assert or a `state != EMPTY` check.
  - `spawn.c` and `spawn.h` have no SPDX header.

## (C) Test quality

- **C1 — vm audit: 6 of 6 audit-check mutants survive.** The only negative tests are for stale upper-level entries (`test_vm.c:898-928`). The audit is the only gate on the ring-3 spawn path (`spawn.c:197-206`).
  - Fix: add `vm_fake` hooks to poke a leaf's perm/phys, present-at-guard, alias state, and extent sharing, with one negative test per check.
- **C2 — `test_sched_killall`.**
  - Every case passes `spare` equal to the flagged supervisor (`:102`, `:125`, `:148`, `:170`), so replacing the identity check with the supervisor flag survives.
  - The waiter-release fake only counts calls (`:56-61`), so releasing the wrong slot survives.
  - The link-mismatch and proc-state fail-stop branches are untested.
  - The `irq_save` fake always returns 0 (`:35`), so the IF=0 checks never run.
  - The `halt` stub loops forever (`:50`), so no abort path can be tested.
  - `je_sched_kill` (single kill, ADR 0010 D8) has **no hosted test**.
  - The `#define static` include trick is safe today (I verified there are no block-scope statics in `sched.c`), but `JE_SCHED_HOSTED_TEST` compiles out the device hooks in `mark_dying` (`sched.c:248-256`).
- **C3 — `test_endpoint`.**
  - RECV denial is tested only through `recv_peek` (`:1051-1067`), which ring 3 never reaches; `recv_peek_caps`, `wait_register` and `entry_probe` are uncovered.
  - **WAITREG-NO-INFLIGHT survived.** That mutant drops the inflight ref a waiter holds, so with both handles closed the record can be freed while a thread is parked. The timer sweep then dereferences `wait_rec`: use-after-free.
  - Case 22 builds the cap descriptor and mints inline (`:1159-1177`), so production attenuation and mint (`endpoint_kern.c:202-280`) are integration-only. Integration does cover missing-TRANSFER (`spoofer.c:45`) and the superset spawn grant (M4c negatives in `user_demo.S`).
- **C4 — `test_device`.** `je_device_apply_dispatch`, which grants ring-3 port I/O, has no hosted test. Ack-caller identity, revoke on death, and the assign quota preflight are also untested.
- **C5 — decode boundaries and glue.** Decode tests don't assert acceptance at `len == 64` or `cap_count == 2`. `je_syscall_dispatch` has no hosted tests at all. The kill-denial ring-3 probes are good: `user_demo.S:~1158-1196` covers self→INVALID, no KILL→DENIED, SELF→WRONGTYPE, stale→STALE.
- **C6 — `usercopy.c` has no hosted test.** Its span validators and fixup checks are pure given `vm_fake` plus a scheduler stub.
- **C7 — 15 test files depend on `assert()`.** They become vacuous under `-DNDEBUG`, and several do side effects inside the assert (e.g. `test_syscall.c:25` `spawn_ok`). Add `#ifdef NDEBUG #error`.

## Code and tests worth copying

- **Tests:**
  - `test_uframe.c:20-29`: on reject, an output sentinel must be untouched, and every bound is tested on both sides.
  - `test_vm.c:458-492`: fails the build at every possible step and checks state is restored byte-for-byte.
  - `vm_fake.c`: an independent model that enforces the ADR 0007 D3 ordering rules itself.
  - `test_handle_rw`: a reduced-width build that exhausts handle generations.
  - `test_sched_killall`: the before/after `memcmp` that proves the fail-stop path changes nothing.
- **Code:**
  - `endpoint.c:218-235`: on close, queued caps are detached, the queue reset, then refs released. An end with no handle refs always has an empty inbound queue, so cap cycles can't become unreclaimable.
  - `je_sched_kill_all_user`: validates every slot before mutating any.
  - `usercopy.c:139-161`: the fixup requires the exact RIP, matching direction, CR2 in the declared span, and the right owner.
  - `proc.c`, `uframe.c`, and the journaled build in `vm.c`.

## Assurance deliverable

**1. Critical-mutant manifest.** Each `find` string below occurs exactly once at `c6914ca`. Write replacements as `if (0 && (...))`. Tag each mutant hosted or QEMU so a QEMU-only killer isn't mistaken for a hosted one.

| id | file: find → replace | invariant | killer test |
|---|---|---|---|
| SCHED-SPARE-ID | `sched.c`: `t->proc_id.index == spare.index && …` → supervisor-flag check | the supervisor is spared by identity, not by flag | new: spare is a non-flagged id → the flagged process is killed |
| SCHED-REL-SLOT | `(void)je_endpoint_kern_waiter_release_by_id((uint32_t)slot);` → `current` | the victim's own waiter is released | fake records ids; assert {2,3} |
| SCHED-KILL-SUP | `if (je_proc_is_supervisor(process_pool, victim)) {` → `if (0) {` | the supervisor can't be killed | new `test_sched_kill` |
| EP-RECV-CAPS | `recv_peek_caps` RECV right → `0u` | non-blocking recv needs RECV | case 17 via `peek_caps` |
| EP-WAIT-INFLIGHT | drop `e->inflight_refs++;` in `wait_register` | a parked waiter pins its record | close both handles while parked → record stays allocated |
| EP-RESERVED | `if (head_reserved(&rec->end[side]) && …)` → `if (0)` | a reserved head isn't stolen by another reader | case 29 via `peek_caps` |
| EP-ATTEN / EP-TRANSFER | `endpoint_kern.c:227` subset check, `:215` TRANSFER | caps are attenuated and need TRANSFER | QEMU today; new hosted `test_endpoint_kern` |
| VM-AUDIT-{ALIAS,PERM,1TO1,GUARD,COMPLETE,XPROC} | `vm.c:501`, `:509`, `:487`, `:480`, `:522`, `:543` | the audit rejects each class of bad state | new poke-based negatives |
| DEV-DISPATCH | `allow = d && d->owner.index == id.index && …` → `allow = d && d->active` | only the owner gets port I/O | new `apply_dispatch` test |
| DEV-ACK / DEV-DEATH | `device.c:204`, `:220` | ack is owner-only; the device is revoked on owner death | new tests |
| UCOPY-END / UCOPY-WRITE / UCOPY-FIXUP | `usercopy.c:70`, `:87`, `:143`, `:154` | copies stay inside the arena; writes need RW; only not-present faults recover | new `test_usercopy` |
| UFRAME / HANDLE-RIGHTS / PROC-ABANDON | already killed | — | keep as regression anchors |

**2. Denial tests to add, one per security rule.**
- Each RECV-gated entry point gets a SEND-only-handle case: `entry_probe`, `wait_register`, `peek_caps`.
- Cap transfer: missing TRANSFER, a superset request, and an unknown rights bit, all through `resolve_send_cap`.
- Device:
  - `apply_dispatch` for the owner, a non-owner, after revoke, and after the driver handle is closed;
  - ack by a non-owner;
  - assign into a table at quota;
  - assign with an invalid caller.
- Kill:
  - each of the 5 return codes in `je_sched_kill`;
  - kill-all with a link mismatch, a proc-state mismatch, and a NULL frame.
- Every rejection branch in `je_vm_audit`.
- Decode: acceptance exactly at each maximum.

**3. Test-infra changes.**
- Fix `Makefile:221` (add `kernel/core/sched.c`) and use `-MMD -MP` for test binaries.
- Add the NDEBUG guard.
- Replace the `halt` stubs with `setjmp`/`longjmp` so fail-stop paths can be tested, and make the IF flag configurable in the fakes.
- Run gcov/llvm-cov branch coverage in CI, and report every `return JE_ERR_*` / `JE_EP_*` line no test reaches. That gives an "every rejection path is tested" metric.
- **Property and fuzz targets:**
  - `je_vm_image_validate` against an independent page-bitmap oracle;
  - syscall decode as a property ("any reserved bit set → INVALID", for each op and register);
  - the endpoint layer driven by random op sequences against a reference refcount model, checking the pool is quiescent at the end.
- **Neutral-scheduler differential:**
  - move `pushfq`, `int $0x81` and `sti; hlt` behind arch hooks;
  - replay one event trace (create / timer / block / wake / kill / fault / exit / reap) through both the current `sched.c` and the new scheduler;
  - compare thread states, the selected slot and the proc records after every step.

## Top 8 code improvements, ranked
1. Fix the `je_sched_reap_dead` sign bug (A1). **Owner-gated.**
2. Move the `.c` externs into headers and add `-Wmissing-prototypes -Wnested-externs` (B2; 20 warnings).
3. Remove core spawn's dependency on the `main.c` ledger, then move the harness out of core (B1).
4. Split `JE_EP_CORRUPT` and halt on a dangling handle (A2). **Owner-gated.**
5. Waiter identity check plus the deadline-0 assert (A3, A4). **Owner-gated.**
6. Arch-neutral seams in `sched.c` and `device.c` (B6), needed before the ARM scheduler. **Owner-gated.**
7. One `resolve_with_right()` helper and one shared recv-delivery path (B3, B4). **Owner-gated.**
8. Contract-text sweep, device diagnostic struct, dead parameters, staging clamp, and the backend NX refusal (B5, B7, A6, A7).

## Top 5 test/assurance additions, ranked
1. `je_vm_audit` negative tests (6 survivors on the only spawn gate).
2. Endpoint denials on the real ring-3 entry points, plus the waiter-inflight use-after-free killer (6 survivors).
3. `test_device`: `apply_dispatch`, ack caller, death revoke, quota (4 survivors).
4. Scheduler: a new `test_sched_kill`, a kill-all fake that records slot ids, the spare-by-identity case, and the `longjmp` fail-stop catcher.
5. Commit the hosted/QEMU-tagged manifest above, plus `test_usercopy`, the `Makefile:221` fix and the NDEBUG guard.

My scratch probe is `/tmp/jm_tools/run_mutants.py`, run on the `/tmp/jm` export; all 28 mutant transforms are written out in it.
