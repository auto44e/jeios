**Verdict: ADJUST.** Keeping the run queue in `sched.c` (A/B) is right. Three things need to change before coding: the shape of the C seam, the wording of the release invariant, and protection for the tests and mutants that guard the code being moved.

## 1. Biggest risk: the regression tests live inside the code being split
- `tests/test_sched_killall.c:29-31` does `#define static` + `#include "../kernel/core/sched.c"`. It then reads and writes `registered_vm`, `registered_proc`, `process_pool`, `threads[]`, `current` and `ready` directly (`:190-198`, snapshots at `:391-405`).
- All six critical mutants for `sched.c` in `tools/mutants.json` must match their text exactly once in `sched.c`: REAPER-SIGN, REAPER-IF-CHECK, REAPER-CR3-GUARD, REAPER-RSP0-GUARD, REAPER-VM-INDEX and F08-ONE-THREAD. If that text moves, the mutant goes BLOCKED, which `mutants.py` treats as "never a kill". The coverage disappears without any failure.
- So "unchanged test_sched_killall + mutants" can't hold literally. If the PR also rewrites the fixtures and retargets the mutants, a behavior change can merge with every gate green.
- **Fix:** keep the test body byte-identical. Add one `#include "../kernel/core/proc_lifecycle.c"` under the same `#define static`, plus the Makefile prerequisite. Choose the split (§3) so none of the six mutant strings move.

## 2. Where the proposed split is wrong or will leak
1. **The ctx-ops getters move the coupling instead of removing it.** `thread_kind`, `thread_proc` and `thread_vm` fetch lifecycle facts back out of `sched.c`, so there would be two owners. The thread↔VM↔proc link is already stored three times: `threads[].{vm,vm_index,proc_id}`, `registered_*[vm_index]`, and `proc_rec.{run_linked,thread_slot,vm_index}`. Kill and kill_all already cross-check them.
   - Split it as authority vs cache. Lifecycle is the only writer of the registry and the proc run link, and owns the cross-check.
   - The slot's triple stays a run-queue cache, written only at create and wipe, and passed into lifecycle as arguments.
2. **There is no neutral run-queue vocabulary.** The kill gates need run-queue state, and nothing in the seam list provides it. Someone will add `thread_state()` returning the x86 enum, including BLOCKED_IPC and DYING, which `sched_rr` doesn't have (`sched_rr.h:22-26`).
3. **`snapshot_death_regs` and `set_dying(reason, code)` point the wrong way.** The arch caller has the registers; lifecycle never does. Reason and code already live in `proc_rec`. Keep the x86 register record and `je_sched_death_record` (with its cs/cr2 out-parameters) entirely in `sched.c`.
4. **"mark_dying is the sole waiter release" is false.** `je_sched_drop_user` also releases (`sched.c:582`). It is the only release when the harness or spawn tears down a live thread (e.g. `main.c:2577`).
   - A kill followed by a reap calls release twice; the second call is a no-op.
   - A conformance test asserting "called once" will either fail against today's code or push someone to delete line 582. That would leave a stale waiter on a reused slot, because waiter_id == slot.
   - Correct invariant: mark_dying releases first on the death path; every slot wipe is preceded by an idempotent release; the release takes effect once.
5. **The IRQ-off check is x86-specific and untested.** kill and kill_all test bit 9 of the saved RFLAGS (`sched.c:730`, `:799`).
   - Moving that check to neutral code breaks ARM, where the IRQ-mask bit is DAIF.I (bit 7).
   - "Fixing" it with `je_arch_irqs_enabled()` after `irq_save` makes the check always pass.
   - Nothing would catch either mistake: the hosted fake `je_arch_irq_save` always returns 0 (`test_sched_killall.c:80-83`).
6. **The device hooks are core code but carry x86 semantics.**
   - `device.c:169` checks `vector != 13u` (#GP). An ARM `tf->vector` is a 0..15 vector-table index, so index 13 would be misread as #GP.
   - Revoke calls TSS-COM2 and PIC operations (`device.c:133`).
   - In the hosted test the hooks are compiled out (`#ifndef JE_SCHED_HOSTED_TEST`), so their ordering is never tested.
7. **The kstack arithmetic only works by coincidence.** `sched.c:206` and `:584` use the kernel-thread `STACK_PAGES` for the VM-held kstack. It equals `JE_VM_KSTACK_PAGES` (both 5) only by coincidence; there is no `_Static_assert`. Add one; it is compile-time only.
8. **Still not enforced after this PR — say so in the PR.**
   - `spawn_undo` (`spawn.c:95-121`) and the `main.c` fixtures assemble drop/destroy/on_dying/on_reaped themselves.
   - `je_sched_reap`'s user-ZOMBIE branch (`:557`) wipes a bound slot without release, unregister or guard restore. It is unreachable today (`je_thread_exit` has no callers outside `sched.c`), but it bypasses lifecycle and ARM must not copy it.

## 3. Where the line should be (Q1–Q3)
A rule a reviewer can check: **move only (a) registry/authority state and its writers, (b) ordering between lifecycle side effects, and (c) decisions made from the process record alone.** Everything that tests run-queue state, every arch register or flag access, every loop over `threads[]`, and the frame/continuation/INT80 code stays in `sched.c`.

- **Moves:**
  - the 5 registry functions, verbatim;
  - `je_lc_on_dying` = waiter release → hooks → `je_proc_on_dying`. mark_dying only writes run-queue state after those calls, and none of the callees read scheduler state, so the order survives if this prefix is one function;
  - kill's process-record checks (D8 steps 1–5) as `je_lc_kill_decide`;
  - drop's order: release → disarm → unregister → clear;
  - destroy_owned → on_reaped.
- **Stays in `sched.c`:**
  - the reap_dead loop and its IF/current/CR3/RSP0 guards;
  - kill/kill_all state gates and the two-pass loop;
  - the F08 scan and frame synthesis;
  - the IRQ save/restore around the link (`:235-237`). Don't remove it on the grounds that create requires IF=0; nothing proves the `main.c` callers comply.
- **Result:** all six mutant strings stay where they are.
- **Fail-stop:** neutral functions return a status; `sched.c` keeps calling `abort_scheduler`, so the console text and the longjmp test harness are unchanged.
- **Seam:** a link-time port of three functions (kstack disarm, slot clear, dying hooks) instead of a runtime 9-op table.
  - There is one run queue per image.
  - Hosted tests already swap in fakes at link time.
  - It avoids JCS-07 callback validation on the IRQ-off fault path.

## 4. Keep the names (D)
Keep them. A rename touches `spawn_undo` (D5/F13), syscall D8, endpoint A6, about 40 `main.c` call sites, and test_usercopy's fake `je_sched_current_user`. That mixes mechanical churn into a semantic move and makes moved-code review useless.

It is also the right long-term shape. In MP3, `spawn.c`, `endpoint_kern.c` and `usercopy.c` will link on ARM, so `je_sched_current_user`, `current_proc_id`, `wake_ipc`, `drop_user` and `kill` become the per-arch contract ARM has to implement. Only the 5 registry functions are truly neutral:
- define them once in `proc_lifecycle.c` under their existing names, declared in `proc_lifecycle.h`, which `sched.h` includes;
- `proc_lifecycle.h` must not include `sched.h` (which pulls in the x86 frame);
- new helpers get `je_lc_*` names.

## 5. What the seam needs for MP3 ARM to reuse it (or record as MP3 blockers)
1. **A neutral run-queue class:** PARKED (x86 READY or BLOCKED_IPC with a frame), DEAD (x86 DYING), ON_CPU, NOT_USER, INVARIANT. `sched_rr` must gain DYING and BLOCKED_IPC equivalents; that is a `sched_rr.c` change and needs owner sign-off.
2. **A user-thread stack type that the run queue doesn't free.** `je_rr_reap` always calls `stack_free` (`sched_rr.c:224`), and ARM's `stack_free` unmaps and frees the stack (`sched_arm.c:369`). That is the same double-free (M3a #4 step 3) the "reap never frees a user kstack" rule exists to prevent.
3. **A "context released" predicate that includes cached translations.**
   - On x86 with PCID off, "not current, not the active CR3, not the active RSP0" is enough.
   - On ARM a bare TTBR0 compare is not. It needs an ASID-masked compare plus a completed TLBI-by-ASID/DSB, either in the predicate or in the ARM VM destroy, stated in the contract.
4. **Per-slot kstack arm/disarm, owned by the arch.** x86 uses page_guard/page_restore; ARM uses stack map/unmap. It must run strictly before destroy, and a failure means no unregister, clear or destroy.
5. **IRQ-off precondition** checked on the saved flags, per arch. ARM currently has no `je_arch_irq_save` or `je_arch_irqs_enabled` at all.
6. **Device death hooks** need a neutral fault class instead of vector 13. Alternatively ARM links a no-device hook stub and declares a "no DEVICE" degraded profile. Changing `device.c` touches M4-tested code, so that is the owner's call.
7. **waiter_id == run-queue slot on both arches,** and no slot is cleared while a waiter keyed to it exists.
8. **Correctly arch-only, keep it out of lifecycle:**
   - frame identity at the kstack top (`rsp0_top - sizeof` on x86, `top - 304` on ARM);
   - SVC rewind length (2 on x86, 4 on ARM);
   - continuation registers.

## 6. Is the no-regression gate enough? (Q6) No. Minimum evidence:
1. **Characterization tests first,** in a commit that touches only tests, green on a88c585:
   - mark_dying order with recording fakes;
   - an unregistered thread (gen 0) releases its waiter but skips hooks and proc;
   - `proc_on_dying` failure halts before any DYING store;
   - dropping a live or BLOCKED_IPC slot releases before clearing;
   - kill + reap = 2 release calls, 1 effective;
   - hosted `je_sched_kill` D8 cases;
   - kill/kill_all halt when IF is set.
   No hosted test runs `je_sched_kill` today (test_syscall only mentions it in a comment), and the device-hook order is compiled out of the hosted build.
2. **During extraction,** `git diff --stat tests/` should show only the new include line and the Makefile change.
3. **`make mutants` before and after:** zero BLOCKED and all six KILLED. Add new mutants, each killed by a named test:
   - release moved after `proc_on_dying`;
   - drop's release deleted;
   - destroy and on_reaped swapped;
   - supervisor check deleted;
   - IF check deleted.
4. **Diff the serial-log markers** of smoke, integration and every `JE_TEST_MODE` against a88c585. ADR 0014 says a changed marker rejects the change.
5. **Boundary checks in `make check`:**
   - `nm -u proc_lifecycle.o` checked against an allowlist;
   - a forbidden-token grep on `proc_lifecycle.[ch]`: `je_trap_frame`, cr3, tss, rsp0, rflags, `0x80`/`0x81`, hlt, `<< 9`;
   - an ARM compile with `aarch64-linux-gnu-gcc` and `ARM_CFLAGS`. This is feasible: I checked that `proc.c`, `endpoint.c`, `handle.c` and the `vm.h`/`endpoint_kern.h`/`proc.h` headers already compile cleanly with those flags.
6. **Conformance suite inputs:** use the real `proc.c`, not a fake pool, which could drift from its pinned semantics. Use fakes that record call order and model idempotent release. Add cases that each arch's `je_sched_*` surface must pass, not just cases for the neutral helpers.

**Owner decision needed:** ADR 0014 D3 requires the suite to run on both the x86 and ARM paths before merge. ARM has no user path yet, so only the x86 binding and the neutral module with fakes can run now. Record the ARM binding as blocked (an MP3 entry criterion); changing the gate wording is the owner's decision, not an agent vote.

**The adjustment, in short:**
- Replace the runtime ctx-ops table with a 3-function link-time port, and drop the getters, `snapshot_death_regs` and the reason argument to `set_dying`.
- Apply the move rule so all six mutants stay in place.
- Neutral code returns a status; `sched.c` halts.
- Restate the release invariant correctly.
- Land characterization tests first, then mutant parity, the marker diff, and the nm/forbidden-token/ARM-compile checks.
- Record the MP3 blockers from §5 and escalate the ARM half of the gate to the owner.

This was a read-only, AI-assisted review, not independent human qualification. I changed no files and did not run `make check`, `make smoke` or `make integration`. The only commands I ran were the ARM test compiles (into `/tmp`, since deleted) and searches.
