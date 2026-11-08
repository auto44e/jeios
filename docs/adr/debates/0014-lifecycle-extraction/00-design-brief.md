# Design review — ADR 0014 process-lifecycle extraction (BEFORE coding)

You are reviewing a DESIGN, not code. Attack the boundary choice. This is a
refactor of M1/M2/M3/M4-tested `kernel/core/sched.c` in the JeiOS freestanding
kernel; the owner has approved ADR 0014 Option A (a bounded lifecycle-only
extraction as its own PR, landed BEFORE MP3 ARM userspace). The hard constraints
from ADR 0014 are:

- **Lifecycle only. No run-queue change.** Do NOT merge/migrate the x86 run queue
  onto `sched_rr.c`. Do NOT add PCID/SMP/priorities.
- **Single ownership boundary.** One module owns: the process registry
  (`registered_vm[]`/`registered_proc[]`/`process_pool`), the thread↔VM↔proc link,
  dying/reap/drop/kill decision logic, VM+kstack lifetime rule (kstack freed by
  `je_vm_destroy`, never by reap), and the sole-waiter-release rule (`mark_dying`
  is the one site that releases an endpoint waiter).
- **Arch specifics stay behind seams**, not embedded in neutral code: x86 CR3 load,
  TSS.RSP0, `int 0x81`, `sti;hlt`, RFLAGS/IF checks, the `je_trap_frame` cs/ss/
  rflags/cr2 fields, the INT80 2-byte rewind, the death-record register fields.
- **No-regression gate:** unchanged x86 `make check` / `make smoke` /
  `make integration` / every `JE_TEST_MODE` / `test_sched_killall` / M3+M4 proofs,
  plus a NEW shared lifecycle conformance suite exercised by both arch paths.

## Ground truth (verified, core main a88c585, read directly)

`sched.c` (874 lines) today interleaves THREE things in one `thread threads[JE_THREAD_MAX]`
array + file-static registry:
1. **The x86 run queue / context:** per-slot `state` (EMPTY/RUNNING/READY/SLEEPING/
   ZOMBIE/DYING/BLOCKED_IPC), `je_trap_frame *frame`, round-robin `select_and_install_next`,
   `je_sched_interrupt`, `current`, preemption counting. **(Stays — run queue.)**
2. **The process lifecycle / ownership:** `process_pool`, `registered_vm[]`,
   `registered_proc[]`; register/unregister/has/find/current_proc_id; the
   thread↔VM↔proc link in `je_thread_create_user_start` (`je_proc_link`); `mark_dying`
   (waiter release + device death hooks + `je_proc_on_dying` + death record);
   `je_sched_reap`/`reap_dead`/`drop_user` (kstack guard restore + `je_vm_destroy_owned`
   + `je_proc_on_reaped`); `je_sched_kill`/`kill_all_user`. **(Extraction target.)**
3. **x86 arch couplings:** CR3 load, TSS.RSP0 set/get, `je_arch_kernel_root_phys`,
   `int 0x81` (yield/exit), `sti;hlt` idle, `je_arch_irqs_enabled`/`cr3_read`/
   `tss_get_rsp0` liveness guards in reap_dead, the `je_trap_frame` field reads/writes,
   RFLAGS sanitize, `JE_INT80_LEN` rewind, death-record cs/cr2/vector. **(Behind seams.)**

The hard entanglement: lifecycle data (kind, vm, vm_index, proc_id, wait_* fields,
death_* record) lives IN the same `threads[]` slot as the run-queue state and the
x86 `frame`. `mark_dying`, `drop_user`, `reap_dead`, `kill`, `kill_all_user` all read
BOTH lifecycle fields AND run-queue state (e.g. "state must be READY or BLOCKED_IPC",
"not current", "not the active CR3/RSP0") in the same function.

Public API that MUST be preserved (callers, verified): spawn.c uses
register/unregister/drop_user/proc_pool; syscall.c uses block_current/current_proc_id/
current_slot/current_user/exit_current/find_process/interrupt/kill/proc_pool/
return_to_user/ticks/wait_finish/wait_pending; usercopy.c uses current_user;
endpoint_kern.c uses current_proc_id/current_user/wake_ipc; main.c uses the reap/
death-record/counter/kill_all/init/register surface. Tests: test_proc, test_proc_retire,
test_endpoint, test_device, test_usercopy, test_sched_killall.

ARM today (MP2) has NONE of this linked — sched_arm.c is a pure kernel-thread seam
over sched_rr.c, no process/VM/lifecycle. MP3 will need the ARM user path to reach
the SAME lifecycle ownership (register, link, mark_dying, reap, drop, current_user,
wait/wake) with ARM arch seams (TTBR0 instead of CR3, SP_EL1 instead of TSS.RSP0,
svc instead of int 0x80, wfi instead of hlt, an ARM user frame instead of je_trap_frame).

## The proposed design (ATTACK THIS)

**A. New `kernel/core/proc_lifecycle.c` + `include/jeios/proc_lifecycle.h`** (name is
FLEX per ADR 0014). It owns ONLY the ownership state + neutral decision logic:
- the registry arrays + `process_pool`, and register/unregister/has/find/current_proc_id;
- the neutral *policy* of mark_dying (ordering: waiter-release FIRST via a seam, then
  device death hooks via a seam, then `je_proc_on_dying`, then record the terminal
  reason), reap/drop/kill/kill_all decision logic;
- the VM+kstack lifetime RULE (reap must not free a user kstack; drop restores the
  guard; destroy frees the run) expressed against seams.

**B. The run queue + per-slot context STAYS in sched.c** (x86) and will have an ARM
analogue in sched_arm.c (MP3). Neither is "the lifecycle module."

**C. A `je_thread_ctx_ops` arch/run-queue seam** the lifecycle module calls to read/
mutate the per-thread facts it needs WITHOUT owning the thread table or the frame:
e.g. `thread_kind(slot)`, `thread_proc(slot)`, `thread_vm(slot,&vm,&idx)`,
`set_dying(slot, reason, code)`, `is_current(slot)`, `is_live_address_space(slot)` (the
CR3/RSP0 liveness guard, arch-provided), `restore_kstack_guard(slot)`,
`clear_slot(slot)`, `snapshot_death_regs(slot, ...)`. x86 implements these over
`threads[]`+`je_trap_frame`+CR3/TSS; ARM later implements them over its own table.

**D. Keep the `je_sched_*` names as thin wrappers** so NO caller changes in this PR
(spawn/syscall/usercopy/endpoint_kern/main/tests all keep compiling unchanged). The
wrappers forward into proc_lifecycle + the run-queue seam. (Alternative: rename to
`je_lifecycle_*` and update all callers — more churn, bigger diff, more risk. I lean
against it for THIS PR.)

**E. New shared conformance suite** `tests/test_proc_lifecycle.c` driving the neutral
module against a fake thread-ctx-ops + fake proc pool + fake endpoint-release, asserting:
register/unregister idempotence, link failure rollback, mark_dying ordering + single
release, reap-skips-user-kstack, drop restores guard, kill/kill_all state gates,
find_process. This is the "exercised by both arch paths" gate (x86 via sched.c; ARM
later via sched_arm.c).

## Questions to attack
1. Is the A/B/C boundary right, or does the `threads[]`-slot entanglement make a clean
   "lifecycle owns state, run-queue owns the table" split impossible without also
   moving run-queue state? Where exactly should the line be? Is a `je_thread_ctx_ops`
   seam the right shape, or is it just relocating the coupling?
2. mark_dying reads run-queue state (`state`, `frame`) AND lifecycle state AND calls
   arch (device hooks) AND calls proc.c. Which half owns it? Can the ordering
   invariant (waiter-release is the SOLE owner, happens first) survive the split?
3. reap_dead's three liveness guards (not current, not active CR3, not active RSP0)
   are arch. kill/kill_all's gates ("READY or BLOCKED_IPC", "frame != NULL") are
   run-queue. Do these belong behind the ctx-ops seam, or do they force kill/reap to
   stay in sched.c with only a thin neutral core?
4. Keep-the-names (D) vs rename-and-update-callers: which is lower RISK for a
   tested-path refactor? The owner values small reviewable diffs on tested paths.
5. What would MAKE THE ARM PATH ACTUALLY REUSE THIS in MP3 — is the seam set
   sufficient for TTBR0/SP_EL1/svc/wfi, or does the design bake in an x86 assumption
   (e.g. "one kstack top = RSP0", the INT80 rewind length, the trap-frame identity
   check at rsp0_top-sizeof(frame)) that ARM can't satisfy?
6. The no-regression gate + conformance suite: is it enough to prove behavior is
   UNCHANGED, or do we need a characterization/mutation step (e.g. prove the extracted
   mark_dying ordering with a mutant that releases the waiter LAST and watch a test
   fail)? What's the minimum to be confident a tested path didn't silently change?

## Output
A prioritized critique: the single biggest risk in this boundary; any place the split
is wrong or will leak; the keep-names vs rename call with a reason; and concretely what
the seam set must include so MP3's ARM user path reuses it without an x86 assumption
leaking through. End with: BOUNDARY SOUND / ADJUST (and the specific adjustment).
