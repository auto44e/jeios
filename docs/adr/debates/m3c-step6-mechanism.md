# Peer debate — 2026-10-19T03:20:49Z
Topic: JeiOS step-6 park/resume mechanism: restartable-syscall vs continuation-flag vs return-a-block-code; pick safest minimal sched.c change, A6-5 compliance
Rounds: 2   Roles: claude=implementor codex=adversarial-reviewer
Context file: /tmp/step6mech.md


[R1 CLAUDE] **Recommendation: Mechanism A (restartable syscall), structured the way C proposes. The handler returns a "block me" request and one sched.c helper does the park. Reject B. My main argument comes from the code on main: a parked user thread has no kernel continuation to resume.**

### The fact that decides it (sched.c / syscall.c / trap.c on main)
- `je_sched_interrupt` switches threads by **returning another thread's frame pointer** to the asm stub. The C stack frames the current user thread had on its kstack are simply dropped. The only thing kept for a user thread is its trap frame at `rsp0_top - sizeof(*f)`.
- `JE_SYSCALL_YIELD` already shows the pattern. It writes the result into `f`, then calls `return je_sched_interrupt(f,false)`. It does not `int 0x81` from inside the handler.
- Mechanism A as drafted calls `int 0x81` from inside the recv handler. That cannot work. On a user thread it pushes a CPL0 frame below `rsp0_top - sizeof`. `je_sched_interrupt` then fails both its checks (`cs` must be the user CS, and the frame must sit exactly at `rsp0_top - sizeof`) and calls `abort_scheduler()`. Parking has to be done by **returning** into the scheduler, the way YIELD does.

### Why B fails A6-5
There is no kernel stack frame to "re-enter." B could only finish the recv in one of two ways:
- (a) `select_and_install_next` calls the recv-finish code after `je_arch_cr3_load`. That is copy-out from the scheduler, which A6-5 forbids.
- (b) The scheduler builds a CPL0 frame on the user kstack that returns into a kernel trampoline. That is a new kind of frame, and `je_sched_interrupt`'s validation would have to be relaxed to accept it. That weakens a tested invariant and needs the owner's sign-off.

On top of that, B adds a branch to the hottest code path. Reject.

### Why A is correct
After the wake, the thread `iretq`s to `rip-2`, executes `int 0x80`, and the CPU writes a **fresh** frame at the same fixed address. `je_syscall_dispatch` then runs as a real syscall continuation: the thread's own CR3, its own kstack, full syscall context. That is exactly what A6-5 asks for, and the scheduler never copies anything.

Hazards from the brief:
1. **Rewind by 2.** This is architecturally fixed. The only encoding of INT with an imm8 vector of 0x80 is `CD 80`; INT3, INT1 and INTO are different opcodes and vectors. With a prefix (e.g. `66 CD 80`), `rip-2` still lands on `CD 80`. The real risk is a sibling thread rewriting the code page while this thread is parked, but that only harms the user's own process, as long as hazard 3b below is handled. Use a named `JE_X86_INT80_LEN 2` with an SDM citation, not a bare `2`.
2. **Registers stay intact.** The frame sits on the kstack, which ring 3 cannot write. Nothing in the kernel may write `f->rax`/`rdx` at park time. RAX must still hold the syscall number when the thread re-traps. The ISR and sender stamp only the waiter fields, never the frame.
3. **Re-resolving the handle (the real A6-5 conflict).** A naive re-execution resolves `rdi` again.
   - **Failure case:** a sibling thread closes the handle and the slot gets reused for another endpoint. The recv then resolves to a different end. The message reserved on the original end is never consumed, so that queue's head stays reserved forever (a DoS), or a message gets mixed up across ends.
   - **Rule:** the dispatch prologue checks first whether `threads[current].wait` is pending. If it is, the recv path uses the **owned end reference saved when the thread parked**, not `rdi`. The registers are used only to check that the handle matches.
   - **3b.** If the thread's next syscall is *not* the matching recv (code page rewritten, handle changed), the dispatch releases the reservation, puts the head back, clears the waiter, and then handles the syscall it actually received. Exit, fault and teardown also release a pending waiter.
   - Timer IRQs that hit between the `iretq` and the `int 0x80` are harmless. The thread becomes READY, the reservation stays held, and it is consumed when the `int 0x80` traps.
4. **`je_uframe_check`.** `select_and_install_next` already re-checks the frame every time it dispatches a thread. A rewound `rip` below the arena base fails the check and kills only that task (`DEATH_BAD_RETURN`), not the kernel. Also check `rip-2 >= JE_VM_ARENA_BASE` at park time and refuse to park with INVALID otherwise.

### Minimal change set
- **Assembly / trap entry: zero changes. trap.c: zero changes.** This is the main reason to pick A; there is no new untested assembly.
- **sched.c:**
  - Add a `BLOCKED_IPC` state and waiter fields to `thread`: `end_ref`, `side`, `deadline`, `cause`, `pending`.
  - Add `je_sched_block_current(f)`. It asserts the thread is a user thread, that `f` is at `rsp0_top - sizeof`, and that the arena bound holds. It sets `rip -= JE_X86_INT80_LEN` and `state = BLOCKED_IPC`, then calls `return je_sched_interrupt(f,false)`. This works unchanged: `je_sched_interrupt` only flips RUNNING→READY, so a BLOCKED_IPC thread stays parked.
  - In `je_sched_interrupt`'s existing wake loop, add one clause: a `BLOCKED_IPC` thread past its deadline goes through `je_ipc_wait_arbitrate(TIMED_OUT)`, then the guarded flip to READY.
  - Add `je_sched_ipc_wake(slot)`, the guarded BLOCKED_IPC→READY flip used by sender and close.
  - Add an assertion that a kernel thread never blocks, which keeps the "bootstrap is always READY" invariant.
- **syscall.c:**
  - Add a pending-waiter prologue (hazard 3 rule).
  - On EMPTY with a nonzero timeout, the recv handler returns `je_sched_block_current(f)`.
  - `select_and_install_next` does not change.

### Questions for the owner (A6 clarifications, not for an agent vote)
1. Confirm that, under a restartable syscall, the A6-5 "parked syscall continuation" means the re-trapped dispatch with a pending waiter. Also confirm that this path must use the owned end reference and **must not** re-resolve `rdi`.
2. What happens to a pending waiter whose next syscall doesn't match (release-and-requeue, as I propose, or terminate the task)?
3. Does closing a handle that a parked sibling thread is blocked on stamp CANCELLED on that waiter? If it does, the slot-reuse scenario in hazard 3 never reaches delivery.
[R1 CODEX] codex
I agree with Claude’s rejection of the literal nested `int 0x81`: it creates a CPL0 trap frame below the fixed user frame, while `je_sched_interrupt` expects the user frame at `rsp0_top - sizeof(*f)`. Blocking must return through the scheduler path, like `JE_SYSCALL_YIELD`.
I also agree that restart-on-wake has the smallest assembly blast radius. `int imm8` is `CD ib`, so the final `CD 80` is two bytes and `f->rip -= 2` is valid for the normal instruction, including prefixed encodings.
However, Claude’s proposed pending prologue is not A6-5-clean as written:
- It must not “check that the handle matches” by resolving or consulting `rdi`. A reused numeric slot makes that comparison meaningless. The continuation must use the saved owned `end_ref` and saved recv arguments exclusively.
- A mismatch should be treated as a kernel invariant failure or explicit cancellation path, not silently added as “release and requeue.” Reservation rollback semantics are not specified by A6.
- The claim that `je_uframe_check` kills only the task is unverified from the supplied facts. That behavior needs an actual code check or owner contract.
The larger missed hazard is mutable user text. While the thread is parked, a sibling in the same address space can rewrite or unmap the two-byte `int 0x80` site. On wake, the thread may execute something else or fault before re-entering the kernel, leaving the A6-mandated reserved head held indefinitely. “It only harms the process” is insufficient: it can create an IPC liveness/resource leak. A6 must either guarantee the restart site remains executable/immutable while parked, or define a reservation-cancellation/recovery mechanism.
My recommendation is therefore C as the control-flow shape, implemented as a constrained form of A:
1. `recv` returns a block request to dispatch; it must not execute `int 0x81`.
2. A scheduler helper validates the fixed frame, saves the owned end reference and recv arguments, rewinds `rip` by the named `INT80` length, marks `BLOCKED_IPC`, and calls the existing scheduler return path.
3. Wake arbitration stamps the cause, reserves the head on `WOKEN`, then performs the guarded `BLOCKED_IPC -> READY` transition.
4. Re-entry consumes only the saved continuation state. It does not resolve `rdi`.
5. No trap-entry or copy-out-from-scheduler change is needed.
A6-5 compliance is then testable: all user copy-out occurs during the fresh `int 0x80` dispatch on the woken thread’s own address space and kernel stack; the scheduler only changes state/frame selection.
Owner clarification is required before accepting this: whether restartable `int 0x80` is permitted despite mutable user code, and the exact rollback/cleanup contract for a waiter whose restart site faults or no longer contains the syscall.
I agree with Claude’s rejection of the literal nested `int 0x81`: it creates a CPL0 trap frame below the fixed user frame, while `je_sched_interrupt` expects the user frame at `rsp0_top - sizeof(*f)`. Blocking must return through the scheduler path, like `JE_SYSCALL_YIELD`.
I also agree that restart-on-wake has the smallest assembly blast radius. `int imm8` is `CD ib`, so the final `CD 80` is two bytes and `f->rip -= 2` is valid for the normal instruction, including prefixed encodings.
However, Claude’s proposed pending prologue is not A6-5-clean as written:
- It must not “check that the handle matches” by resolving or consulting `rdi`. A reused numeric slot makes that comparison meaningless. The continuation must use the saved owned `end_ref` and saved recv arguments exclusively.
- A mismatch should be treated as a kernel invariant failure or explicit cancellation path, not silently added as “release and requeue.” Reservation rollback semantics are not specified by A6.
- The claim that `je_uframe_check` kills only the task is unverified from the supplied facts. That behavior needs an actual code check or owner contract.
The larger missed hazard is mutable user text. While the thread is parked, a sibling in the same address space can rewrite or unmap the two-byte `int 0x80` site. On wake, the thread may execute something else or fault before re-entering the kernel, leaving the A6-mandated reserved head held indefinitely. “It only harms the process” is insufficient: it can create an IPC liveness/resource leak. A6 must either guarantee the restart site remains executable/immutable while parked, or define a reservation-cancellation/recovery mechanism.
My recommendation is therefore C as the control-flow shape, implemented as a constrained form of A:
1. `recv` returns a block request to dispatch; it must not execute `int 0x81`.
2. A scheduler helper validates the fixed frame, saves the owned end reference and recv arguments, rewinds `rip` by the named `INT80` length, marks `BLOCKED_IPC`, and calls the existing scheduler return path.
3. Wake arbitration stamps the cause, reserves the head on `WOKEN`, then performs the guarded `BLOCKED_IPC -> READY` transition.
4. Re-entry consumes only the saved continuation state. It does not resolve `rdi`.
5. No trap-entry or copy-out-from-scheduler change is needed.
A6-5 compliance is then testable: all user copy-out occurs during the fresh `int 0x80` dispatch on the woken thread’s own address space and kernel stack; the scheduler only changes state/frame selection.
Owner clarification is required before accepting this: whether restartable `int 0x80` is permitted despite mutable user code, and the exact rollback/cleanup contract for a waiter whose restart site faults or no longer contains the syscall.
[R2 CLAUDE] **Position: I agree with Codex's shape (C control flow, constrained-A resume), and I concur with its two A6-5 corrections. Its "mutable user text" hazard can't be reached on main today. The real leak path it was pointing at is a different one: the thread dies while a reservation is held. Below I correct two premises from the code and add what we both missed.**

**Agree**
- No nested `int 0x81`. `je_sched_interrupt` aborts unless a user thread's frame sits exactly at `rsp0_top - sizeof(*f)` (sched.c:174–175). A CPL0 frame lower on the stack would trip that check.
- The continuation must use only the saved owned `end_ref` and the saved args. It must never look up or compare against `rdi`. I withdraw my "check that the handle matches" step. Comparing a reused slot number proves nothing.
- A mismatch at re-entry is an invariant failure, not a silent requeue.

**Settled by reading the code (verified facts, not opinion)**
1. **Codex's "uframe_check kills only the task is unverified" is now verified.**
   - `je_sched_return_to_user` (sched.c:235) and `select_and_install_next` (sched.c:145) both call `mark_dying(..., DEATH_BAD_RETURN)` and move on to another task.
   - The kernel only aborts on frame-identity corruption.
   - The rewound RIP stays inside the arena, so `je_uframe_check` passes it unchanged.
2. **Mutable-text hazard: not reachable on main.**
   - `USER_CODE` is installed R/X, and no writable alias survives publish (vm.h:60–65).
   - The ABI has ops 0–12 only: no mmap, mprotect or unmap.
   - Threads are one per process (`threads[slot]` is built from a single `p->root`, sched.c:111), so no sibling thread shares the address space.
   - Nothing can rewrite or unmap the `CD 80` site while the thread is parked.
   - This should be recorded as a **stated invariant** that is re-gated when mmap/mprotect/threads land. It doesn't need a new mechanism now.
3. **"Vector 0x80 with CPL3 means it came from `CD 80`" holds.**
   - The PIC is remapped to 0x20–0x2f (platform.c:72), so no hardware IRQ arrives on 0x80.
   - Exceptions are vectors 0–31.
   - 0x80 is the only DPL3 gate (trap.c:59).
   - So `rip - 2` always points at `CD 80`. Use a named `JE_INT80_LEN 2` with a comment citing SDM `INT imm8 = CD ib`.

**What Codex missed**
1. **The real reservation-leak window is death, not text.**
   - `je_uframe_check` forces IF=1 (`RF_IF` in uframe.c). A timer IRQ can therefore land at the instruction boundary after `iretq` and before `CD 80` runs.
   - In that window the thread is READY, holds a reserved head, and is not in the kernel. That's harmless unless it dies there: today a fault can't happen in that window, but a future kill or cancel path could.
   - The required rule: `mark_dying`, or the reaper, must release any `WOKEN` reservation and clear waiter state. This is the cleanup contract Codex wants from the owner, attached to the one exit path that actually exists.
   - It belongs in sched.c next to `mark_dying`, plus a test: wake → kill before re-entry → the head is deliverable to another receiver.
2. **Codex's "call the existing scheduler return path" is wrong if that means `je_sched_interrupt(f,false)`.**
   - `je_sched_interrupt` is the hot path and runs the SLEEPING sweep (sched.c:183–185). Blocking shouldn't go through it.
   - Add `je_sched_block_current(f)`, modelled exactly on `je_sched_exit_current` (sched.c:213–219): same frame-identity checks, set `state = BLOCKED_IPC` instead of `mark_dying`, rewind `f->rip`, return `select_and_install_next()`.
   - `je_sched_interrupt` (sched.c:184) only demotes RUNNING threads to READY, so a BLOCKED_IPC thread is left alone.
   - The only change to `je_sched_interrupt` is that the timer pass also stamps TIMED_OUT on BLOCKED_IPC threads (through the A6 CAS), next to the existing SLEEPING sweep.
3. **Re-entry must key on waiter state, not on RAX.**
   - At the top of `je_syscall_dispatch`, before `je_syscall_decode`, add one check: `if (threads[current].wait.pending)` run the continuation.
   - The frame is kernel-held while parked and no user instruction runs between `iretq` and `CD 80`, so RAX can't change. Keying on `pending` still removes any reliance on that.
   - Add a cheap check that `f->rip == saved_post_int_rip`. On failure, `abort_scheduler()`: it is a kernel invariant break, not a user error.
4. **Copy-out location is already A6-5 clean.**
   - The copy runs inside a fresh `je_syscall_dispatch` on the woken thread.
   - By then `select_and_install_next` has already loaded CR3 and RSP0 (sched.c:152–153) and returned.
   - So no copy happens inside the scheduler, which is the compliance test Codex asked for.

**Minimal change set**
- **sched.c:**
  - add `je_sched_block_current`
  - add the BLOCKED_IPC timeout stamp in the timer pass
  - add a guarded `je_sched_wake_ipc(slot)` (state BLOCKED_IPC → READY)
  - add the release-reservation hook in `mark_dying`
- **syscall.c:**
  - recv returns a BLOCK action on EMPTY with nonzero timeout
  - add the pending-continuation prologue
- **Trap entry and assembly:** no changes.

**Back to the owner (A6 clarifications)**
- (a) Is it acceptable to resume by re-executing the restart site, under the invariant "user text is immutable and the process is single-threaded"? This needs re-review at the mmap/mprotect/threads gate.
- (b) Who releases a WOKEN reservation when the holder dies before re-entry, and does the head go back to the front of the queue? A6 doesn't specify this rollback.
- Both are contract questions, not things to settle by agent vote.
[R2 CODEX] codex
I agree with Claude on the core A6-5 correction: a resumed receive must use only the waiter-owned `end_ref` and saved arguments. Any lookup or comparison using `f->rdi` is a handle-reuse bug; slot equality is not identity.
I also agree that blocking must not issue nested `int 0x81`. The handler/dispatcher should return a block action to a direct `je_sched_block_current(f)` helper. The scheduler must not perform copy-out.
I dispute treating restartability as safe merely because today’s user text is immutable. That is a current invariant, not an intrinsic property of mechanism A. It must be explicitly recorded as a gated prerequisite; otherwise future mmap/mprotect/thread changes silently invalidate the mechanism. Plain restartable-syscall behavior is also noncompliant with A6-5 because it re-resolves the handle.
The acceptable variant is constrained A:
- Rewind by named `JE_INT80_LEN == 2`, valid for the fixed `CD 80` ABI.
- Set a pending-continuation flag and save all receive arguments plus the original post-`int` RIP.
- On re-entry, pending state takes precedence over syscall decoding.
- Complete only from saved arguments and owned `end_ref`; never inspect or resolve `rdi`.
- Clear pending state only after successful copy-out/commit; rollback the reservation on every failure.
- Unexpected pending/RIP/ownership state is a kernel invariant failure, not an ordinary syscall error.
Claude’s `saved_post_int_rip` check is useful, but the proposed minimal file list omits the required waiter fields and cleanup ordering.
I would reject B unless its “kernel re-entry” is implemented as a formally specified post-context-switch continuation on the new thread’s kernel stack. Otherwise the scheduler is executing user copy-out after a CR3/stack transition, exactly the high-blast-radius boundary A6-5 excludes. Implementing that safely requires more assembly/context-switch machinery than constrained A.
Claude also missed that `je_sched_wake_ipc()` must be only a guarded wrapper around the single A6 `je_ipc_wait_arbitrate` transition; it must not introduce an independent `BLOCKED_IPC → READY` race.
Recommendation: C control flow plus constrained A resume. Minimal changes are `syscall.c` pending-continuation dispatch, `sched.c` direct block helper, timeout/wake arbitration, and reservation cleanup in death/reap paths. No trap-entry assembly change.
Owner clarification remains required for reservation rollback if a WOKEN thread dies before re-entry, including whether the reserved queue head remains at the front. Also explicitly freeze the current “immutable user text/single-threaded address space” assumption as a prerequisite for the two-byte rewind mechanism.
I agree with Claude on the core A6-5 correction: a resumed receive must use only the waiter-owned `end_ref` and saved arguments. Any lookup or comparison using `f->rdi` is a handle-reuse bug; slot equality is not identity.
I also agree that blocking must not issue nested `int 0x81`. The handler/dispatcher should return a block action to a direct `je_sched_block_current(f)` helper. The scheduler must not perform copy-out.
I dispute treating restartability as safe merely because today’s user text is immutable. That is a current invariant, not an intrinsic property of mechanism A. It must be explicitly recorded as a gated prerequisite; otherwise future mmap/mprotect/thread changes silently invalidate the mechanism. Plain restartable-syscall behavior is also noncompliant with A6-5 because it re-resolves the handle.
The acceptable variant is constrained A:
- Rewind by named `JE_INT80_LEN == 2`, valid for the fixed `CD 80` ABI.
- Set a pending-continuation flag and save all receive arguments plus the original post-`int` RIP.
- On re-entry, pending state takes precedence over syscall decoding.
- Complete only from saved arguments and owned `end_ref`; never inspect or resolve `rdi`.
- Clear pending state only after successful copy-out/commit; rollback the reservation on every failure.
- Unexpected pending/RIP/ownership state is a kernel invariant failure, not an ordinary syscall error.
Claude’s `saved_post_int_rip` check is useful, but the proposed minimal file list omits the required waiter fields and cleanup ordering.
I would reject B unless its “kernel re-entry” is implemented as a formally specified post-context-switch continuation on the new thread’s kernel stack. Otherwise the scheduler is executing user copy-out after a CR3/stack transition, exactly the high-blast-radius boundary A6-5 excludes. Implementing that safely requires more assembly/context-switch machinery than constrained A.
Claude also missed that `je_sched_wake_ipc()` must be only a guarded wrapper around the single A6 `je_ipc_wait_arbitrate` transition; it must not introduce an independent `BLOCKED_IPC → READY` race.
Recommendation: C control flow plus constrained A resume. Minimal changes are `syscall.c` pending-continuation dispatch, `sched.c` direct block helper, timeout/wake arbitration, and reservation cleanup in death/reap paths. No trap-entry assembly change.
Owner clarification remains required for reservation rollback if a WOKEN thread dies before re-entry, including whether the reserved queue head remains at the front. Also explicitly freeze the current “immutable user text/single-threaded address space” assumption as a prerequisite for the two-byte rewind mechanism.

---
## A6-CLARIFY-1/2 text re-review (consistency pass — 5 defects in CLARIFY-2, all fixed)

### Claude
Two problems need fixing before this is ready. The edit deleted two existing headings, and CLARIFY-2's "no death path today" claim is wrong against `sched.c`. Everything else is consistent, with a few gaps to close.

**Structural defect.** The insertion removed two headings that exist at HEAD: `### A6-PEERWAKE. Peer death actively wakes blocked receivers` (HEAD line 986) and `### Proof plan (deterministic, single-CPU — avoids the step-5 spin race)` (HEAD line 1031). Without them, the PEERWAKE body now reads as part of CLARIFY-1, and the proof plan as part of CLARIFY-2. A6-5 and CLARIFY-2 both refer to "A6-PEERWAKE", which no longer has a heading to point to. Put both headings back.

1. **CONSISTENT.** The prologue reads the saved args and the owned reference and ignores the registers from the re-executed `int 0x80`. That satisfies the no-re-resolve rule. Copy-out runs in a fresh dispatch after `select_and_install_next`, so "no copy from the scheduler" holds. One thing to pin down: "at the top" should say *before any op decode or handle lookup*, so a shared front-end can't resolve `rdi` before the prologue runs.

2. **CONSISTENT.** Keying on `wait.pending` with the RIP assertion matches the "constrained" claim. The prerequisite is stated as a FLEX re-gate trigger, not a permanent guarantee. "Ops 0–12" is correct: `abi.h` static-asserts end at `JE_SYS_TRY_RECV == 12`.

3. **Partly consistent, three gaps:**
   - **CONTRADICTION, ordering:** CLARIFY-2 says the release happens "before the thread/frame slot is reused (the A6-PEERWAKE ordering)". PEERWAKE's actual ordering is "FIRST, before the release walk runs". That is a weaker point in time. If the release walk runs while the dying thread still holds the reservation, it can close an end or free queued messages that are still reserved. CLARIFY-2 should say "before the release walk".
   - **CONTRADICTION, terminology:** CLARIFY-2 says the head "returns to the front of its queue". A6-5 still says the reserved message "returns to the free pool when that waiter's reservation is released". "Free pool" reads as freeing the message slot. Change A6-5 to "becomes deliverable at the queue head".
   - **Double-release risk:** "`mark_dying` **and** the reaper MUST … release", and PEERWAKE step 1 also "release[s] any head reservation". That is three call sites with no single-owner rule. Specify one idempotent release that checks the owner and only clears the reservation if this waiter holds it. Call it at `mark_dying`, and have the reaper only assert it is already clear. CLARIFY-2 is the READY/WOKEN case of PEERWAKE step 1 and should say it uses the same routine.
   - **Gap:** "deliverable to the next receiver in FIFO order" doesn't say whether a receiver already *parked* on that end gets woken when the reservation is released. If the waiter slot frees when `WOKEN` is stamped rather than at consume, a second receiver can park behind a reserved head and then sleep until its timeout. That is the latency bug PEERWAKE exists to prevent. Either state that the slot stays occupied until consume or release (so A6-4 gives the second receiver `EMPTY` and nobody can park), or say the release arbitrates the next waiter.

4. **CONTRADICTION:** "No death path can reach this window today (a blocked/woken thread cannot fault in the gap…)".
   - `select_and_install_next` (`kernel/core/sched.c:144-148`) runs `je_uframe_check` on the woken thread's saved frame and calls `mark_dying(DEATH_BAD_RETURN)` if it fails.
   - The parked frame is the user's own trap frame, and the comment on `je_sched_return_to_user` says users can corrupt RSP/RFLAGS before `int 0x80`.
   - `je_sched_exit_current`, the stated byte-for-byte model, does not validate the frame.
   - So a user can corrupt RSP, block, get woken, and die in the scheduler while holding the reservation. That is user-reachable once step 6 lands.
   - The window also starts earlier than CLARIFY-2 describes: from "READY, not yet installed", not just after `iretq`.
   - Fix: require `je_sched_block_current` to run `je_uframe_check` before parking (a deliberate deviation from the model, to be stated), *and* reword the claim as "no reachable path once the pre-park validation holds". The tie to the A6-2 signal milestone is fine.

5. **Not buildable as written.**
   - With one thread per process, X's process can't exit while X is READY, and the reaper only reclaims DYING threads. "Destroy X's process before re-entry" therefore needs a kernel-side forced-kill hook that marks a READY thread DYING.
   - That hook is itself a new death path. It should be named and gated as test-only, the way `JE_DEV_COPY_INJECT` is, or it contradicts "no kill/cancel in v1".
   - The existing "teardown while blocked" bullet has the same unstated dependency, since B's only thread is parked.
   - The A6-4 link holds: the transferred-dup second receiver is the reachable case. The test must say the second receiver calls recv *after* the release, or which outcome a park attempt gets per point 3's slot rule. With that, the sequence is deterministic: kernel enqueue → kill hook → step reaper → second recv.

6. **Other new defects:**
   - Undefined terms: `wait.pending`, `saved_post_int_rip`, "BLOCK action" and `mark_dying` are never defined in the ADR; `mark_dying` is a static function in `sched.c`. Add one line naming the waiter fields.
   - The "byte-for-byte" claim in CLARIFY-1 conflicts with the pre-park validation point 4 requires. Change it to "modeled on … plus pre-park `je_uframe_check`".

This was a text review of the ADR against `abi.h` and `sched.c`; nothing was built or run.

### Codex
codex
1. **CONSISTENT.** Rewinding by `JE_INT80_LEN == 2` re-enters dispatch, while the pending prologue uses the saved endpoint reference and never re-resolves `rdi`. Copy-out occurs after dispatch/context switch, satisfying A6-5.

2. **CONSISTENT.** `wait.pending`, the saved post-`int 0x80` RIP assertion, the constrained prerequisite, and ABI ops 0–12 are aligned. The prerequisite is correctly framed as a future re-gate, not permanent.

3. **CONSISTENT.** The reservation remains on the enqueued head; clearing it makes that FIFO head deliverable again. Unlinking the dying process’s waiter first aligns with A6-PEERWAKE. However, cleanup ownership must be made exactly-once; see defect below.

4. **CONTRADICTION:** “**No death path can reach this window today**” conflicts with the later deterministic proof that steps the bootstrap reaper to destroy X before re-entry. Narrow it to “no user/ABI-triggered production death path.” The future A6-2 signal milestone linkage is otherwise correct.

5. **CONSISTENT.** The reaper-stepped sequence is deterministic and uses the A6-4 reachable second-waiter case via a transferred duplicate endpoint reference.

6. **CONSISTENT**, apart from the reachability overclaim in #4 and the cleanup-ownership ambiguity below. No other material undefined term or cross-section contradiction found.

New defect: “**mark_dying and the reaper MUST … release**” can imply two release sites, while A6-PEERWAKE already performs the first unlink/release. Specify one exactly-once, idempotent cleanup owner—or state that one phase releases and the other only verifies—to eliminate double-release risk.
1. **CONSISTENT.** Rewinding by `JE_INT80_LEN == 2` re-enters dispatch, while the pending prologue uses the saved endpoint reference and never re-resolves `rdi`. Copy-out occurs after dispatch/context switch, satisfying A6-5.

2. **CONSISTENT.** `wait.pending`, the saved post-`int 0x80` RIP assertion, the constrained prerequisite, and ABI ops 0–12 are aligned. The prerequisite is correctly framed as a future re-gate, not permanent.

3. **CONSISTENT.** The reservation remains on the enqueued head; clearing it makes that FIFO head deliverable again. Unlinking the dying process’s waiter first aligns with A6-PEERWAKE. However, cleanup ownership must be made exactly-once; see defect below.

4. **CONTRADICTION:** “**No death path can reach this window today**” conflicts with the later deterministic proof that steps the bootstrap reaper to destroy X before re-entry. Narrow it to “no user/ABI-triggered production death path.” The future A6-2 signal milestone linkage is otherwise correct.

5. **CONSISTENT.** The reaper-stepped sequence is deterministic and uses the A6-4 reachable second-waiter case via a transferred duplicate endpoint reference.

6. **CONSISTENT**, apart from the reachability overclaim in #4 and the cleanup-ownership ambiguity below. No other material undefined term or cross-section contradiction found.

New defect: “**mark_dying and the reaper MUST … release**” can imply two release sites, while A6-PEERWAKE already performs the first unlink/release. Specify one exactly-once, idempotent cleanup owner—or state that one phase releases and the other only verifies—to eliminate double-release risk.

---
## Full step-6 diff adversarial cross-review (8 findings: 3+3 P1, 2 P2 + lows — all fixed)
### Claude
I found 5 real defects (3 high, 2 medium) and 3 low items. All are paths the current tests don't reach: smoke only covers TIMED_OUT with no peer, and no test covers SHORT_BUFFER on resume, cancel with two waiters, or peer death while parked. I read the code to confirm each one; I didn't write tests to reproduce them.

**Defects**

1. **HIGH: the SHORT_BUFFER resume permanently blocks that end until the thread dies.** `kernel/core/syscall.c:262`. When the WOKEN continuation hits SHORT_BUFFER, it returns while the waiter is still present with cause WOKEN and `head_reserved` set. Then `je_sched_wait_finish` clears `wait_pending`. From then on, every attempt to receive on that end fails:
   - `recv_peek` and `recv_peek_caps` see the reserved head and return EMPTY.
   - `recv_entry_probe` returns EMPTY, then `wait_register` returns WAITERS_BUSY, which becomes EMPTY.
   - The ISR never wakes the thread because it isn't BLOCKED_IPC.

   The head message can't be delivered, and the waiter's inflight ref stays held until `mark_dying`. The comment says "a re-issued recv with a bigger buffer delivers it", but nothing can re-enter, because the continuation only runs while `wait_pending` is set. Fix: release the waiter on SHORT_BUFFER. The head stays queued and becomes deliverable again, and the bigger re-issue takes the normal path. Add a hosted test for this.

2. **HIGH: peer death doesn't wake a blocked receiver (A6-PEERWAKE).** `kernel/core/endpoint_kern.c:33`. The teardown hook calls `je_endpoint_release_table`, which calls `je_endpoint_handle_close` directly. The peer-close wake is only wired into `je_endpoint_kern_close`, which serves the syscall close and the mint rollback. So if process B dies or exits while A is parked on recv from B, A's end becomes PEER_CLOSED with an empty inbound but nobody runs arbitrate. A sleeps until its deadline, up to 2^32 ticks, and then gets TIMED_OUT instead of PEER_CLOSED. Fix: run the peer-close wake for each end that `release_table` closes.

3. **HIGH: `je_sched_drop_user` wipes a BLOCKED_IPC thread without releasing its waiter.** `kernel/core/sched.c:440`. `threads[i]=(thread){0}` runs for any state that isn't current. The waiter registered under `waiter_id=slot` survives with its inflight ref. Then `je_vm_destroy` → `release_table` brings `handle_refs` to 0, but `inflight_refs` is still 1, so the record leaks. Worse, the next thread created in that slot inherits the stale waiter: a later send to that end WOKEN-stamps it and calls `je_sched_wake_ipc(slot)` on the unrelated new thread. If that thread is ever BLOCKED_IPC somewhere else, it gets flipped READY while its own waiter is still WAITING. Its continuation then hits the default case and the kernel halts. Fix: call `je_endpoint_kern_waiter_release_by_id(i)` in `drop_user`, or refuse BLOCKED_IPC there.

4. **MEDIUM: `je_endpoint_kern_wait_cancel` cancels the wrong waiter.** `kernel/core/endpoint_kern.c:328`. It arbitrates CANCELLED on the first WAITING waiter it finds and only checks `id == slot` afterwards. When it hits another thread's waiter, that waiter is stamped CANCELLED but not woken. That thread stays parked until its deadline and then returns CANCELLED for a cancel nobody asked for. The intended thread is also missed if it comes later in the scan. Fix: check `waiter_id == slot` before arbitrating. The `i`/`s` loop also scans unallocated records; it's harmless because `present` is false there, but it should skip `!allocated` the way `release_by_id` does.

5. **MEDIUM (latent): `waiter_release_end` decrements `inflight_refs` but never calls `maybe_free_record`.** `kernel/core/endpoint.c:336`. Every other inflight release does (endpoint.c:195, :382). If the waiter's ref is the last one, the record leaks. Today `mark_dying` releases before `release_table`, so the normal path is safe. But defect 3, and any future path where the end's handles go away while a waiter is parked (multi-thread processes, or an end handed off by cap transfer), would leak. Fix: route it through the same release-and-maybe-free helper.

**Low**

6. **Timeout validation depends on queue state.** `kernel/core/syscall.c:538`. The `JE_IPC_MAX_WAIT` and overflow check only runs when the probe returns EMPTY. The same out-of-bound R10 is rejected INVALID on an empty queue but accepted when a message is queued. I'd check the bound before the probe so the result doesn't depend on queue state. This is within A6-3 wording, so I'm flagging it, not changing the contract.

7. **Timer ISR flips READY even when arbitrate loses.** `kernel/core/sched.c` timer sweep. With every current WON path calling `wake_ipc`, a BLOCKED_IPC thread can only lose here because of defect 4's stray CANCELLED. Consider asserting `r==WON || cause!=WAITING` so a resume without a terminal cause is caught here, not later as a halt in the continuation's default case.

8. **The comment on `je_endpoint_recv_entry_probe` is wrong.** endpoint.h says it returns SHORT_BUFFER; the code never does (it doesn't take a buffer size). The behaviour is fine because OK falls through to the normal recv; only the comment needs fixing.

**Points you asked me to check that hold up**
- **(1) Restart:**
  - Nothing runs before the prologue, and RAX isn't touched on park.
  - The saved RIP comes from the CPU, not the user, and is always at least base+2. Any `int 0x80` encoding, prefixed or not, ends in `CD 80`, so rip−2 is always that instruction.
  - `wait_pending` is set only after `block_current` passes the frame check. It is cleared in `wait_finish` and `mark_dying`, and zeroed by `drop_user`, so no other syscall sees it set.
  - `return_to_user` re-validates the frame.
- **(2) Timer deadline:** inclusive `<=` with `now + timeout` matches the SLEEPING sweep.
- **(3) Reserved head:**
  - `recv_peek_owned_caps` bypasses the reserved-head guard, so the reserving waiter can still read it.
  - Masking only applies when a waiter is present.
  - The no-waiter `try_recv` path and the `peek_caps_at` refactor behave as before.
- **(4) Release balance:** the release is balanced on commit, fault, timeout, peer-close, cancel and death, except for defects 1, 3 and 5.
- **(5) Send wake:** re-resolving the handle after the send is safe because the send and wake run in one IF=0 section.
- **(6) Close wake reading a freed record:** not a use-after-free.
  - The pool is static storage, and a waiter's inflight ref prevents the free.
  - Freeing also resets the end's state, so the PEER_CLOSED check bails.
  - Nothing can reuse the slot inside the IF=0 window.
- **(7) `mark_dying` order:** releasing the waiter first is correct, including on `block_current`'s frame-check failure path.
- **(8) Initializers:** the two initializers have 10 trailing fields in the right order.
- **(9) No regressions found in the shared paths:** nothing switches on the thread-state enum, and BLOCKED_IPC is correctly excluded from `select_and_install_next`, `je_sched_finished` and the reaper.

### Codex
codex
I’ll audit the implementation against the frozen A6 contract, trace the scheduler/endpoint/syscall lifetimes, and verify the claimed test coverage against the actual code and test fixtures. I’ll report only actionable defects (or explicitly say no defects), with file/line references and a concise severity rationale.
codex
The initial read-only inspection was blocked by the workspace sandbox’s network-namespace setup (`RTM_NEWADDR`), before any repository command ran. I’m retrying the same non-mutating reads with the required elevated execution path so the review is evidence-based.
exec
/usr/bin/bash -lc "sed -n '1,240p' README.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,300p' docs/HANDOFF.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,300p' docs/ROADMAP.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,300p' docs/AGENT_START.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,340p' docs/DESIGN.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
# Gated roadmap

Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
current baseline and linked issue dependencies. Future designs remain unimplemented.


No milestone has a calendar deadline. Each gate requires code, failure tests,
implementation-status updates, and a handoff with observed evidence. Existing
M0 stubs do not count as completing future modules.

| Gate | Scope | Pass criteria | Dependencies |
| --- | --- | --- | --- |
| M0 | Freestanding AMD64 bootstrap, serial, core helpers, design | Build + ELF checks + host tests; boot smoke separately observed | Initial scaffold |
| M1 | Exceptions, boot block validation, RAM reservations, correct mappings | Known exception produces bounded diagnostic; malformed boot maps rejected; kernel/data permissions match qualified CPU | M0 |
| M2 | Page/heap allocation, timers, IRQs, kernel threads and context | Exhaustion/rollback tests; preemption/context correctness; lock/context rules; NX and guarded stacks where supported | M1 |
| M3 | User address spaces, handles, IPC, syscalls, minimal init | Two isolated tasks run; illegal access contained; stale/denied handles rejected; no raw pointer ABI | M2 |
| M4 | Service supervisor, device graph, buffer grants, basic driver | Crash-loop budget; generation revocation; device timeout; DMA containment qualified or explicit legacy policy | M3 |
| M5 | Read-only initfs then durable storage and configuration | Checked image parser, durability/error behavior, interrupted commit/recovery tests | M4 |
| M6 | POSIX subset, networking, native service SDK | Tested interfaces, per-app network mediation and revocation, portable tool builds | M3-M5 |
| M7 | Framebuffer/software compositor, shell/settings, native UI contracts | Accessible interaction, scoped prompts, data themes, recovery shell, complexity/authority independence | M5-M6 |
| M8 | Hardware acceleration and advanced CPU state | Named GPU driver, reset isolation, feature migration/context tests, benchmarked fallbacks | M4 and per-feature gates |
| M9 | Secure boot/update/encryption and hardened profile qualification | Signed/recoverable images, anti-downgrade policy, keys outside repo, power-loss and security review evidence | M5 plus platform support |
| P1 | AArch64 board/emulator | Own boot/traps/timer/MMU tests; shared core checks; no invented feature support | M1 contracts; platform port parallel to later work |
| P2 | Enhanced ARM / confidential workloads | Platform-qualified features, context/ABI/fault tests, explicit guarantees | P1 and relevant M3/M8/M9 contracts |
| E1 | Mobile | Named board/device, power/touch/display/modem separation and update path | P1, M7-M9 plus device drivers |
| E2 | 64-bit MMU IoT | Headless profile, scoped management, offline recovery, power loss tolerance | Relevant M4-M9 mechanisms |

## Current gate status and next task

M0, M1a, M1b and the scoped M2 BSP foundation are implemented on
main (foundation PR #1 merged 2026-10-13); see IMPLEMENTATION.md and HANDOFF.md for exact evidence.
M1 has validated maps/reservations, fatal exceptions and hardware permission tests.
**Update 2026-10-18:** M3a (the userspace boundary) is complete and qualified
(see `docs/M3A_RETRO.md`). **M3b** (typed handles, ADR 0008) is complete and
merged — the handle table, the handle syscalls, the rights ABI, and attenuation
(which made rights enforcement genuinely ring-3-observable) all shipped. A second
architecture, **ARM64 MP1 step 1** (ADR 0002), now boots on QEMU virt-8.2.
**M3c** (capability IPC + waits + init, **ADR 0009** accepted) is in progress: its
contract is frozen and **D-SEQ step 2 (endpoint lifecycle — pool + split refcount
+ destruction + retain-on-dup, create kernel-only)** is implemented; the remaining
steps are the owner-signed death-path + ring-3 create, non-blocking data IPC,
capability transfer, waits, and the bootstrap fixture.
M2 has bounded page/heap allocation, timer preemption, guarded kernel threads,
yield/sleep/exit and deferred stack reclamation. This is not a general scheduler,
SMP implementation, userspace sandbox, or complete hardening milestone.

Next is M3, split into independently reviewable subgates:

- **M3a:** private user address spaces, ring-3 entry/return, user-copy fault
  containment and a bounded user program loader. Prove two actual CPL3 tasks
  cannot access each other's memory or kernel state. A task fault must be local.
  The boundary contract is frozen in [ADR 0007](adr/0007-m3a-userspace-boundary.md)
  (accepted 2026-10-17); issues #3–#6 implement and qualify against it. Issue #3
  (hosted per-process VM builder + bounded image loader) is implemented and
  hosted-tested — see IMPLEMENTATION.md "M3a #3". Issue #4 is being landed in
  reviewable, individually QEMU-verified steps: step 1 (ring-3 GDT descriptors),
  step 2 (real per-process page tables + CR3, CPL0-verified), step 3 (CPL3
  entry/return — real ring-3 execution under preemption, CR3 + TSS.RSP0, D7
  enforcement), step 4 (fault-local process death + the D6 int 0x80 gate + the H1
  kernel-DoS fix — a synchronous user fault / user int 0x80 / bad-return-state
  kills only the offender, a peer is proven to keep running, the dead process is
  reclaimed). Steps 1–4 are merged/landing; #4 acceptance ("illegal access kills
  only the offender; peers survive") is met in QEMU. Issue #5 is the integrated
  two-task isolation qualification + the real syscall handler (copy-in/out) behind
  the int 0x80 gate, landing in steps: step (a) — the v0 syscall dispatcher
  (abi_query/yield/exit, UNSUPPORTED/INVALID, ABI isolated from internal status,
  validated syscall returns) — and step (b) — copy_from/to_user + copy_probe with
  per-page prevalidation and the exact-RIP exception-fixup copy sites — are both
  done and QEMU-verified. With #2–#5 complete, the ADR 0007 M3a userspace
  boundary is implemented end to end (private address spaces, ring-3 execution,
  fault-local death, bounded syscalls, recoverable user copies), proven in QEMU
  and peer-cross-reviewed (not independent human qualification).
- **M3b:** typed handles with generation counters, rights attenuation, explicit
  object lifetime and checked syscall layouts. Test stale/forged handles and
  denied operations without side effects.
- **M3c:** bounded capability-aware IPC, waiting/cancellation semantics and init
  process startup, with race/timeout/lifetime tests.

Before calling M3 hardened, resolve entropy/canary strategy, general wait queues
and synchronization, spurious IRQ handling and a security review of the user/kernel
transition. Broader hardening remains a separate acceptance claim. Do not expand
storage, network, GPU or desktop scope as a substitute for these boundaries.

## Decision gates

Before imports: record provenance and review compatibility with the selected
Apache-2.0 project license; preserve each dependency's terms.
Before stable ABI: specify native wire layout, rights/lifetime and concurrency.
Before hardened profile: define exact minimum hardware and firmware/update policy.
Before accelerated desktop: select native toolkit/compositor strategy in an ADR.
Before foreign compatibility: scope exact ABI/application set and security mapping.

## Additional owner requirements (2026-10-13)

These design additions do not change the current implementation focus (M3b, then M3c).

- M3c/M4: [INIT.md](INIT.md), configurable supervisor with good defaults,
  generation-safe readiness, bounded restarts, recovery and explicit authority.
- M5a-c: [STORAGE.md](STORAGE.md), initfs/RAM filesystem, read-only ext4 candidate,
  then independently qualified writable storage. Production default remains open.
- M5/M6/M9/P2: [ENCRYPTION.md](ENCRYPTION.md), key lifecycle before format freeze,
  optional nearby sharing, reviewed PQ protection, qualified memory encryption
  and separate confidential-execution gates. No new crypto code is authorized
  for production use merely by this design; library/protocol review is required.

- M3/M4 through M9: [IDENTITY.md](IDENTITY.md), local principals/sessions first,
  then durable identity records, multi-method pairing and E2E sync qualification.
  Family/tenant membership and fleet scale require additional acceptance evidence;
  no cloud account is required for unmanaged local login.
- M5 and later format gates: owner-approved provisional ext4 default, raw block
  I/O with exclusive range leases, temporary/sidecar views without reformatting,
  and a qualified filesystem interoperability matrix. Additional formats are
  planned individually; no blanket current read/write support is implied.

- Enterprise follow-on after M3-M6/M9 prerequisites: [DOMAINS.md](DOMAINS.md),
  native directory/join lifecycle and centralized policy gates, followed by
  independently qualified AD DS client and selected GPO mapping gates.

  The interoperability matrix includes Windows/macOS/Linux endpoints, Microsoft
  and Samba AD providers, and separately scoped FreeIPA/LDAP/Kerberos integration.
  Qualify cross-platform agents and policy mappings individually.

## Accelerator/media requirements and license decision (2026-10-13)

Apache-2.0 is selected for original project code; see ADR 0005.
[M3A.md](M3A.md) supplies a proposed implementation sequence, not a completed gate.
M3a and M3b are complete; M3c IPC/init is in progress (ADR 0009; endpoint lifecycle landed). The ARM64 MP track runs alongside (MP1 booted).

[ACCELERATORS.md](ACCELERATORS.md) defines open-standards-first GPU/NPU support
across planned platforms, qualified per device. M4 supplies device/buffer/DMA
boundaries; M5/M6 supplies application/storage prerequisites and software media
ports with qualified CPU state; M7 adds desktop integration; M8 qualifies GPU/NPU
backends and measured automatic offloading. FFmpeg, HandBrake and Blackmagic RAW
are userspace integration targets, not current codecs or kernel features.

## Multitouch and security keys (2026-10-13)

[INPUT.md](INPUT.md): M4 input transports/lifecycle, M7 compositor/toolkit/secure
UI and accessibility, E1 touch device qualification. [SECURITY_KEYS.md](SECURITY_KEYS.md):
M4 authenticator transports, M5/M6 credential broker/storage, M7 enrollment UI,
M9 authentication/recovery qualification. Preboot support is a separate gate.
These requirements extend the later backlog; M3a remains the next kernel task.

## Fresh-kernel mobile track

[MOBILE.md](MOBILE.md) links auto44e/jeios-mobile: MP0 ARM64 contract, MP1-MP3
native virtual execution/isolation, MP4 Pixel feasibility, MP5-MP6 physical mobile
bring-up/services, AC0-AC2 future Android compatibility. Fresh C kernel remains
shared; no AOSP/Linux host or lunch target. (M3a and M3b are complete; M3c is in progress; ARM64 MP1 booted.)

## Zero trust throughout the ecosystem — 2026-10-14

[ZERO_TRUST.md](ZERO_TRUST.md) maps enforcement to M3a isolation, M3b rights,
M3c IPC, M4-M6 brokers/network/storage, M7-M9 consent/identity/recovery and mobile
qualification. Treat this as a requirement across gates, not a completed feature
or a reason to skip M3a. See HANDOFF.md for dated current status and history.

## Core standards commitment — 2026-10-16

[STANDARDS.md](STANDARDS.md): target POSIX.1-2024 as a core application environment;
M6 remains an initial tested subset followed by complete-profile qualification.
Built-in LDAPv3 provider/identity integration is also core, with optional enrollment.
M3-M6 and security prerequisites still apply; M3a and M3b are complete and M3c is the current focus.
Track [POSIX #15](https://github.com/auto44e/jeios/issues/15) and
[LDAP #16](https://github.com/auto44e/jeios/issues/16). Certification, AD DS/GPO and
cloud enrollment are separate goals, not implied completion.

## Curl-inspired CI direction — 2026-10-16

[CI_STRATEGY.md](CI_STRATEGY.md) records the owner's short/long-term testing goal:
portable regressions, compiler diversity, sanitizers/static analysis, replayable
failure injection and fuzzing, supplemented by native kernel execution and storage
crash-recovery qualification. [Core #18](https://github.com/auto44e/jeios/issues/18)
is the first bounded CI implementation task. Existing gates remain mandatory;
M3a and mobile MP0 remain next. This update adds planning only, not workflow jobs.

## Ecosystem coding discipline — 2026-10-16

[CODING_STANDARD.md](CODING_STANDARD.md) applies NASA/JPL-inspired rules with
stricter critical-component profiles. [Core #19](https://github.com/auto44e/jeios/issues/19)
tracks audit and enforcement with CI #18. Incremental adoption supports each gate;
it does not replace M3a or establish NASA certification.

 succeeded in 0ms:
# JeiOS system design

## Overall product vision: approachable defaults, deep owner control

Owner clarification: 2026-10-14T19:59:09-04:00. This philosophy governs the
entire JeiOS OS/ecosystem, not only security. Everyday users should have a coherent,
seamless and intuitive experience without needing to understand implementation
details. Power users, developers and organizations should be able to progressively
inspect, customize, automate, replace and administer supported components in depth.

Apply this to installation/onboarding, desktop and mobile interaction, themes and
window management, accessibility, apps and compatibility, files/storage, networking,
identity/sync, privacy/security, hardware/drivers, audio/media, CPU/GPU/NPU resource
policy, power/performance, init/services, updates/recovery and developer tools.
Good defaults must be complete useful workflows, not a deliberately limited edition.
Advanced controls must be discoverable, documented and supported interfaces rather
than requiring undocumented patches for ordinary customization.

Presets are starting points, not rigid user classes or one global slider.
Allow per-area complexity and configuration: expert audio with simple networking,
a custom shell with stock updates, or deep developer tools with hardened security.
Separate presentation detail, functional configuration, administrative authority,
security policy and hardware capability. Changing one must not silently change
the others. Explicit authorized policy changes remain available to owners within
the device/workspace's disclosed management boundaries.

Provide simple task-oriented UI with progressive disclosure, searchable settings,
plain explanations and accessible input. Expose appropriate advanced configuration
through GUI, CLI and documented APIs with equivalent policy enforcement; not every
low-level operation needs a prominent everyday screen. Versioned configuration,
validation, dry runs where useful, export/import, automation and effective-setting
inspection should make complex setups reproducible. Explain dependencies and
tradeoffs instead of silently ignoring unsupported settings.

Support a spectrum from stock, mostly automatic experiences to deeply customized,
experimental ("YOLO"), finely tuned and tightly controlled deployments. These are
composable choices, not a ranking where more restriction or more complexity is
always better. The authorized owner chooses the balance; organizations can define
and disclose policy for the devices/workspaces they manage.

Prefer scoped changes, previews and reversible transactions where feasible.
Clearly identify destructive, irreversible or system-wide effects. Recovery should
offer a known-good usable environment without needlessly discarding customization.
A reset-to-defaults action must identify affected data/settings and cannot promise
to undo prior data loss or compromise. Do not mistake customizability for a promise
that every component is hot-swappable or every hardware feature exists.

Carry coherent concepts, configuration semantics and workflows across desktop,
mobile, IoT and future device types, adapting interaction to screen, input, power
and hardware. Aim for functional parity where practical; publish explicit support
differences and fallbacks. Cloud accounts and vendor services must not be mandatory
merely to access ordinary local use or expert controls.

Acceptance must cover both audiences: ordinary end-to-end tasks with sensible
defaults and little friction, and documented advanced workflows with predictable
results, configuration portability, accessible explanations and tested recovery.
Qualify each subsystem as it is implemented. This is an accepted product direction,
not evidence that any future UI, service, API or parity target already exists.


Version: 0.2, 2026-10-12. Status: long-term architecture plus M1/M2 BSP implementation.
Audience: the project owner, kernel contributors, system developers, and future
AI agents. This is the primary design document; implementation evidence lives in
HANDOFF.md. Requirements use **must**, **should**, and **may** deliberately. A
requirement here is not a claim that the source currently implements it.

## 0. Current implementation versus target design

M1 exception entry, validated boot-memory discovery and protected mappings are
implemented. M2 adds bounded page/heap allocation, PIC/PIT interrupts, seven
preemptible guarded kernel workers, sleep/wakeup and exit/reaping. These are
single-CPU AMD64 mechanisms; there is no userspace or sandbox yet. The runtime
self-test checks registers, memory and resource accounting before halting.

Read [IMPLEMENTATION.md](IMPLEMENTATION.md) for exact limits and current contracts,
and [HANDOFF.md](HANDOFF.md) for observed tests. Historical mentions of M0 describe
the original scaffold. All other subsystem designs below remain requirements or
proposals until the implementation ledger says otherwise. In particular the first
heap is page-granular, not a slab allocator; general wait queues, canary/entropy
setup, SMP and hardened application boundaries remain future work.

## 1. Product intent and constraints

JeiOS starts a new kernel rather than mechanically combining existing kernels.
The long-term objective is an everyday system which feels coherent for users,
provides structured interfaces for developers, and remains comprehensively
customizable for enthusiasts. The same underlying services should eventually
serve workstation, server, mobile, and appropriate 64-bit IoT profiles.

The owner has specified a C OS core, modular architecture, old 64-bit machine
compatibility, modern CPU/GPU enablement, graceful failures, security/privacy
inspired by GrapheneOS and CalyxOS, granular complexity, a macOS-inspired top
level, and Linux-style desktop ricing. Earlier discussion supplies architectural
inspiration from Linux, BSD, and NT, not a requirement to merge their source.

Concrete constraints:

- Hardware support and security support are separate promises. Old machines may
  boot without satisfying a modern hardened profile.
- Modern features must not leak into baseline instruction paths.
- Interface simplicity must not obscure failures or increase authority.
- The OS must support offline use without a vendor account or telemetry.
- APIs must be explicit enough to audit, test, and describe to future agents.
- A fresh kernel has no automatic access to Linux's hardware ecosystem or native
  Windows/macOS applications. Driver and ABI compatibility are distinct projects.
- No claim of production reliability, formal verification, performance superiority,
  or equivalence to mature hardened OSes is justified by the initial scaffold.

### 1.1 Non-goals for the first release sequence

M0-M3 do not target a complete desktop, accelerated GPUs, cellular service, a web
browser, Linux kernel module loading, WDM/KMDF compatibility, Win32, Darwin binary
compatibility, or secure daily use. The first useful target is a reproducibly
booting and diagnostically understandable kernel with isolated userspace.

## 2. Architecture decision: modular core with service isolation

Adopt a small privileged kernel with explicitly layered internal subsystems and
an NT-inspired executive vocabulary. Critical execution mechanisms stay in
kernel space: traps, scheduling, address-space control, capabilities, IPC,
interrupt routing, and the minimal device primitives needed for progress.
Higher-level policy and suitable drivers should run as isolated user services.

This is a design direction, not a declaration that M0 is a complete microkernel.
Initially all implemented C runs at ring 0. We will decide service boundaries by
measured performance, recovery behavior, driver requirements, and trust budget.
A large hybrid core can evolve from the wrong defaults; each new privileged
subsystem therefore needs a documented reason to be privileged.

| Layer | Responsibilities | Intended trust and ownership |
| --- | --- | --- |
| Architecture/platform | Entry, traps, page tables, CPU state, interrupt controller, firmware discovery | Kernel, narrow architecture-specific contracts |
| Kernel mechanisms | Threads, memory objects, handles, IPC, timers, IRQ/DMA grants | Kernel; minimal authority and bounded operations |
| Executive interfaces | Objects, asynchronous I/O, device lifecycle, security checks | Mechanisms in kernel; management policy in services |
| System services | Configuration, storage, network, package/update, diagnostics, audio/display brokers | Separate processes with explicit capabilities |
| Application environment | Native SDK, POSIX layer, later compatibility environments | Sandboxed processes and scoped brokers |
| User experience | Stock shell, settings, file manager, compositor policy, theme/widget services | Replaceable interfaces; no inherent extra privilege |

### 2.1 Why this shape

A single scheduler and VM manager prevent contradictory ownership and locking.
NT-inspired objects and requests offer predictable developer conventions; they
do not require reproducing NT's internal implementation. BSD-style capability
ideas and Unix conventions can coexist with a coherent native object API.
Linux-derived lessons guide device breadth, feature negotiation, and scalable
execution. The macOS-inspired experience is implemented through public services,
not through hidden shell-only authority.

Rejected as initial approaches: running three native kernel implementations in a
single address space; loading arbitrary foreign binary drivers; and designing a
novel stable driver binary ABI before a real device lifecycle exists. Full
microkernel separation remains a possible outcome, but committing prematurely to
an unmeasured IPC cost or forcing every driver into userspace would also be risky.

## 3. Modularity means three different things

**Build modules** have separate source ownership and dependency contracts. M0
supports this form. **Loadable privileged modules** add code to the kernel address
space and are fully trusted. **Isolated service modules** have process or VM
boundaries and capability-scoped interfaces. Only the last category contains
arbitrary memory corruption in the way people often expect from modularity.

### 3.1 Build-time module contract

Each subsystem must describe provided interfaces, dependencies, owned resources,
initialization order, partial failure cleanup, and shutdown preconditions. The
initial `je_module` list is explicitly ordered, validated before side effects,
and rolled back in reverse order on failure. A failed initializer undoes its own
partial allocations. Successful initializers have infallible cleanup callbacks.
M0 has no dependency resolver, version loader, concurrency, or restart manager.

Next registry design: stable subsystem names, versioned interface descriptors,
acyclic dependency graph, optional dependencies, required-versus-optional boot
criticality, bounded initialization deadlines, and explicit diagnostic status.
Reject duplicate names, cycles, unsatisfied versions, and inconsistent readiness.
Do not attempt to restart a kernel module after suspected corruption of shared
memory. Optional features may remain unavailable while boot continues.

### 3.2 Future privileged loading

Keep kernel internal APIs source-level and versioned by build at first. A module
manifest should bind architecture, exact kernel ABI/build identity, dependencies,
capabilities requested, integrity signature, and provenance. Signing validates
origin and authorization, not freedom from bugs. No auto-download during boot.

Loading requires executable mapping validation, relocation bounds checks,
W^X, an authorized policy decision, and an unpublished-to-live state transition.
Unloading must stop new calls, quiesce existing calls and DMA, revoke handles,
remove IRQ callbacks, synchronize references, then free code. A busy module stays
loaded. Never free a module because a timeout elapsed while callbacks still run.
Unsigned development modules require a deliberate development trust profile
with visible consequences independent of UI difficulty settings.

### 3.3 Service lifecycle

Use states: discovered -> authorized -> starting -> ready -> quiescing -> stopped;
failed and quarantined are distinct terminal/control states. A supervisor may
restart only when its restart contract permits it. Track monotonic generation
numbers so replies and capabilities from an old instance cannot target a new one.
Backoff, restart budgets, and crash loops prevent an unstable driver from
consuming all resources. Dependencies must see service unavailable rather than
waiting forever. Persistent state requires recovery before readiness.

## 4. Language, tools, and memory discipline

C11 is the baseline language for the kernel and core services. GNU assembler is
used only where exact architecture instructions or entry conventions demand it.
C cannot establish its own initial stack/page tables or fully describe trap
entry. Host tooling may use Python; later UI language selection is open.

Use fixed-width types for serialized and hardware fields. Internal `size_t` and
pointers must never enter on-disk or IPC formats without a defined encoding.
Check addition, multiplication, alignment, offsets, and truncation before using
untrusted lengths. Define signedness and endianness. Avoid C bitfields for device
registers. Volatile is not a lock or a cross-core memory-ordering mechanism.

Every public core interface must state whether it allocates, sleeps, may be
called with interrupts disabled, accepts user pointers, retains a pointer,
transfers ownership, or invokes callbacks. Document output state on failure.
Use explicit statuses; do not conflate unsupported, denied, busy, exhausted,
timed out, and hardware failure. Destructors must safely handle every legal
partial initialization state, not arbitrary invalid memory.

### 4.1 Hardening sequence

Compiler stack protection remains disabled pending canary initialization and
a validated failure/entropy strategy; this must be resolved before claiming a
hardened userspace boundary. M1/M2 now provide guarded stacks, validated mappings,
zero-initialized heap/stack allocations, and non-executable data where supported. Later add control-flow protections supported by the
compiler, hardened heap metadata, delayed reuse or quarantine where useful,
randomized layouts, and verified entropy before claiming cryptographic security.

Use compiler warnings as errors, host UB sanitizers for portable algorithms,
address sanitizers where available, fuzzers for parsers, and static analysis.
Portable helpers can be hosted; hardware paths need emulation and real hardware.
A successful build does not prove memory safety. Memory tagging reduces certain
risks and is not complete temporal/spatial safety. Do not hand-roll cryptography.

### 4.2 Build constraints

Baseline AMD64 objects use `-march=x86-64`, no red zone, no floating point/SIMD
register generation, freestanding compilation, and no hosted runtime dependency.
Future optimized translation units carry separate target flags and cannot be
called without feature enablement. Toolchain upgrades must preserve baseline
code generation and boot evidence. Production builds need pinned compiler,
linker, firmware/bootloader inputs, reproducibility reports, and artifact hashes.

## 5. Boot and platform abstraction

The first platform is QEMU's legacy PC using GRUB Multiboot2. It is a convenient
experimental entry path, not the final boot trust architecture. The ELF lives
at 1 MiB, enters in 32-bit protected mode, establishes a three-level path into
four-level paging, identity maps the first GiB using 2 MiB pages, loads a GDT,
sets EFER.LME, and enters 64-bit C with an aligned stack. The BSP alone runs.
Interrupts remain disabled during early setup. CPUID checks long mode before
using the long-mode MSR. M1 validates and copies loader data; M2 enables IRQ0 only
after memory and thread state are initialized.

The assembly and linker enforce placement within the bootstrap map. ELF segments
are RX, R, and RW, but the initial page tables are temporarily RWX; segment flags
alone are not hardware enforcement. M1 installs an IDT/TSS and then replaces
those maps with read-only code/rodata, guarded stacks and NX data where available.
Unexpected kernel faults produce a fatal diagnostic. Userspace recovery and
process separation are still required before running untrusted code.

### 5.1 M1 boot information contract

Parse the Multiboot2 block only after verifying its address range is mapped,
minimum fixed header size is accessible, total size has sane upper limits, each
tag has a minimum size, aligned increments cannot overflow, and the terminal tag
is consistent. Validate memory-map stride, types, overlapping regions, arithmetic,
reserved ranges, and the block's lifetime. Treat firmware tables as untrusted.
Reserve kernel, stack, boot block, page tables, modules, firmware, MMIO, and low
memory before allocator use. Never treat all identity-mapped addresses as RAM.

Translate loader-specific data into an internal `boot_context` with explicit
physical ranges, entry capabilities, framebuffer metadata, firmware handles,
and a measured/trust-state descriptor. It must never carry unchecked firmware
pointers into unrelated services. Copy or pin retained information and document
when loader memory can be reclaimed.

### 5.2 Platform evolution

Add UEFI x86-64 and later AArch64 entry paths sharing only the validated internal
context. Legacy BIOS support remains a distinct boot profile. AArch64 bring-up
needs exception-level normalization, granule discovery, translation setup,
cache maintenance, vector table, timer, GIC, PSCI, and DT/ACPI handling as
appropriate. CPU ISA compatibility alone does not imply a board can boot.

Platform interfaces should cover physical memory, timers, CPUs, interrupts,
firmware discovery, MMIO attributes, reset/poweroff, and safe DMA primitives.
Keep ACPI/DT parsing distinct from the generic device graph. Hardware errata and
firmware quirks must be named policies with affected versions and tests.

## 6. CPU compatibility and feature enablement

Interpret 'oldest 64-bit system' as a long-term per-ISA goal, beginning with early
AMD64 PCs. Alpha, SPARC64, MIPS64, POWER, IA-64, and other historical systems are
separate ports with different firmware, endianness, drivers, and toolchains;
they are not covered by an AMD64 binary. First AArch64 baseline target is Armv8-A
on a selected emulator/board, not older non-AArch64 64-bit architectures.

For every feature maintain four facts: hardware present, platform usable,
OS implementation ready, and policy allowed. Enabled is their intersection.
The initial portable helper combines hardware/OS/policy masks only; a future
platform mask must be added when firmware/device prerequisites are real.
Required features missing from the usable set cause explicit refusal, not a
false success or silent substitution with weaker isolation.

### 6.1 x86 specifics

Discover leaf availability before querying CPUID. Use architecture-defined tests
for NX, XSAVE family, SMEP/SMAP, CET, protection keys, virtualization, invariant
TSC, topology, and performance/power hints. CPUID advertising AVX does not alone
permit AVX execution: CR4.OSXSAVE, XCR0 bits, and adequate per-thread context
storage must be configured. AMX has large extended state and additional policy
requirements. Do not run those instructions in baseline kernel C.

Use eager, validated state restoration initially; avoid speculative/lazy
approaches with poorly understood security consequences. Size XSAVE buffers from
validated architectural enumeration, honor alignment and instruction format,
and initialize state before cross-process use. Keep debug state and future
protection registers in the context-switch contract. Precise implementation must
cite current Intel and AMD manuals and relevant errata, not only Linux examples.

### 6.2 AArch64 specifics

MTE labels allocations and can detect classes of memory misuse. PAC authenticates
selected pointers under configured keys. BTI constrains some indirect branch
entry behavior. Permission overlays can reduce access within an address space;
POE2 adds richer compartment controls. RME/CCA Realms target a different trust
boundary for confidential workloads and depend on hardware, firmware, a monitor,
and virtualization support. TrustZone is another distinction, not a convenient
per-application isolation API for ordinary userspace.

Treat feature availability as register-discovered plus platform-qualified. Save,
restore, and initialize per-thread tagging/control/protection state. MTE tag
storage, allocation alignment, synchronous/asynchronous fault mode, signal/debug
semantics, and ABI pointer tagging must be designed together. SVE/SME context is
variable-size; vector-length choices affect migration and signal formats.

 succeeded in 0ms:
# Agent handoff: M1/M2 kernel foundation

## Current status — observed 2026-10-18 (read this first)

Implemented and merged beyond M3a: **M3b** (typed handle table, ADR 0008) is in
progress on x86-64, and a second architecture, **ARM64 (MP1)**, now boots.

- **M3b** (issue #7, ADR 0008 accepted, Option A): the pure handle table, the
  handle syscalls (`handle_self`/`close`/`query`/`rights`), the rights ABI, AND
  **attenuation** (`handle_dup`, op 9) are all merged. Attenuation made rights
  enforcement genuinely ring-3-observable: a process derives a rights-reduced
  handle and `handle_rights` on it returns DENIED (QEMU `M3b-s1` proof). SELF is
  the only object kind; dup is SELF-only (structurally type-gated — a destroyable
  type would be a latent UAF without a retain, which M3c must add). **#7 stays
  OPEN** (landed "Refs #7"): its remaining acceptance — bounded per-object
  **reference count + overflow**, destruction-on-last-reference, and the
  **service-instance vs slot generation** split — needs a destroyable object and
  is M3c work.
- **ARM64 MP1 step 1** (jeios-mobile#1, ADR 0002): boots on QEMU `virt-8.2` /
  `cortex-a72` (direct `-kernel`, no firmware) — entry EL1, stack, TCR_EL1 interim,
  BSS clear, DTB pointer recorded, bounded PL011 console, ordered markers. Separate
  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
  the x86-64 flow is untouched. **Not yet enforced (deferred to MP2+):** VBAR
  vectors + the D5 trap frame (ESR/FAR/ELR/SPSR) + the deliberate-fault test, the
  TBI behavioral test, DTB *validation*, and the MMU.
- **M3c** (issue #8, **ADR 0009 accepted**): in progress. The contract is frozen
  (endpoint/channel object, split refcount + destruction, bounded data-only
  messages, waits, transactional attenuated transfer, bootstrap fixture; two
  owner-gated decisions — terminal-destruction/no-incarnation-counter, and the
  process-death release walk as its own owner-signed PR). **D-SEQ step 2
  (lifecycle)** is implemented: `kernel/core/endpoint.c` + `endpoint.h` — the
  endpoint pool, channel record, **split refcount** (`handle_refs` vs
  `inflight_refs`), retain/release, destruction-on-last-ref, `REFOVERFLOW`, the
  endpoint-aware alloc/dup/close seam (table stays pure), and retain-before-commit
  endpoint dup (while `dup_self` stays SELF-only). Step 2 kept create kernel-only;
  **step 3 (owner-signed) is now done**: the process-death **release walk** (via a
  generic `je_vm` teardown hook run inside an **IRQ critical section** in
  `je_vm_destroy` — A3-1), the **typed close dispatcher** (ring-3 HANDLE_CLOSE
  routes endpoint handles through the endpoint layer — A3-3), and **ring-3
  `endpoint_create` (op 10)** returning both handles **packed in RDX** (A3-2)
  shipped together. Proven in QEMU: `M3c-s3a` (kernel self-test: survivor-peer
  PEER_CLOSED, record freed on survivor close) and `M3c-s3b` (ring-3 create →
  typed close → fault → reap reclaims the channel; pool baseline held across
  >POOL_CAP cycles). ADR 0009 was amended (A3-1/A3-2/A3-3, owner-approved) for the
  IRQ-save death-path change and the packed-RDX ABI shape. Earlier, ADR 0009 D4a
  appended endpoint rights (`SEND=0x2`…), an owner-approved ABI change that flipped
  the tested `dup(self,0x2)` result INVALID→DENIED (Option A; see IMPLEMENTATION.md
  "M3c rights-bit discrepancy"). **Step 4 (non-blocking data IPC) is now done**:
  `try_send` (op 11) / `try_recv` (op 12) move bounded, copied, **data-only**
  messages through per-end inbound ring queues (Model A: send on end i → peer
  inbound), honoring the D6 copy ordering (send: preflight → copy_from_user into
  stack staging → enqueue; recv: peek → copy_to_user → infallible commit, so a
  copy-out fault leaves the message queued). Statuses QUEUE_FULL/SHORT_BUFFER/
  PEER_CLOSED/EMPTY (10–13) added; `JE_ABI_MSG_MAX=64`, DEPTH=4 (policy bounds,
  ~2.1 KB pool). D9 data-only is enforced (R10 cap_count must be 0). An
  owner-approved touch to the tested lifecycle path discards/zeroes inbound
  queues on close/free (free condition unchanged). Proven ring-3 by the `M3c-s4`
  self-loopback marker (direction, FIFO, SHORT_BUFFER, copy-out-fault rollback →
  delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED) — which
  does NOT prove cross-address-space delivery (one process holds both ends; that
  needs transfer). **ABI forward note:** the single RDX scalar can't return
  received capability handles, so **step 5 MUST add a new op / out-register
  extension, not overload `try_recv`** (R8/R9 and RDX[63:32] reserved now).
  **Step 5 (capability transfer) is now done** (frozen by ADR 0009 A5): `try_send`
  carries 0..2 caps (descriptors queued + inflight-retained on the target, D3.2);
  `try_recv` mints fresh attenuated handles in the receiver's table (D8). D9 =
  cycles allowed but reclaimed on last-handle removal (A5-1); stale cap → handle 0
  (A5-2); `maybe_free_record` got the `allocated` guard + detach-before-release
  discard order (A5-3); ABI uses the pre-reserved R8/R9 path (A5-4). Proven: hosted
  cap mint/dead-cap/cycle-reclaim + the `M3c-s5` kernel self-test (cross-TABLE
  transfer over a seeded channel, message across distinct tables, S+C reclaimed).
  **Transfer sequencing:** the sender must retain the transferred end until the
  receiver mints (closing first → dead cap, A5-2). **Ring-3 two-process transfer
  deferred to step 7** (recorded decision: multi-process choreography is step 7's
  subject; the spin-based two-stub proof raced the single-CPU cooperative
  scheduler). **Remaining D-SEQ:** step 6 (waits, owner-gated — edits `sched.c`),
  7 (bootstrap fixture + the deferred ring-3 two-process transfer proof). #8 closes
  when the sequence completes; #7's deferred refcount/overflow is already
  discharged (endpoints are genuinely destroyed + reclaimed on process death).
  **Step 6 (blocking recv waits) is now done** (owner-gated; frozen by ADR 0009 A6
  + A6-CLARIFY-1/2). `try_recv` with a nonzero R10 timeout blocks until WOKEN /
  PEER_CLOSED / TIMED_OUT / CANCELLED (A6-2 internal-only in v1); R10==0 is the
  unchanged non-blocking poll. Mechanism: a per-end waiter + one
  `je_endpoint_wait_arbitrate` CAS, `je_sched_block_current` (pre-park
  `je_uframe_check` + 2-byte `int 0x80` rewind), a `wait_pending` continuation
  prologue in `je_syscall_dispatch` that finishes from the owned reference (no
  handle re-resolve, copy-out after the context switch), guarded
  `BLOCKED_IPC→READY` wake, one idempotent `je_endpoint_wait_release` owner (called
  by `mark_dying`). New ABI statuses `TIMED_OUT=14`/`CANCELLED=15`; R10 timeout
  bounded by `JE_IPC_MAX_WAIT`. Proven: `M3c-s6` (TIMED_OUT end-to-end, ring 3) +
  hosted `test_endpoint` 27–28 (the pure waiter layer, all four causes via the
  CAS, reserved-head, release). **Deferred to step 7:** the full two-process ring-3
  WOKEN handshake (the wake bridges are wired; step 7 schedules two cooperating
  user processes) — honest, same as the step-5 ring-3 deferral.
  **Remaining D-SEQ:** step 7 only.

Full qualification evidence for M3a is in `docs/M3A_RETRO.md`; the per-PR ledger is
`docs/IMPLEMENTATION.md`. Everything above is QEMU/hosted-verified and
peer-cross-reviewed (AI-assisted, not independent human qualification); the owner is
the merge gate.

**Decision-record policy (active 2026-10-19, `docs/adr/0000-decision-record-policy.md`):**
every recorded decision from now on carries three parts — (1) the decision + why,
(2) predictions if other choices had been made, (3) flex options to revisit (tag
`FLEX (planning review)` for ones to take to the main planning agent at the
pause). A one-time **retroactive sweep** PR brings ADRs 0001–0009 + prior
owner-gated calls into this format; it is scheduled right **before** the final
doc-sync + retro at the M3c/MP2 pause. ADR 0009's A5 amendment (step-5 transfer:
D9 reclamation invariant, stale-cap S1 null-marker, the `maybe_free_record` guard,
the R8/R9 transfer ABI) is the first decision written in the new format.

## M3a #5 step (b): recoverable user copies + exception fixup — observed 2026-10-17T19:50:00+00:00

Branch `feat/m3a-5b-user-copy`. The FINAL M3a piece. `kernel/core/usercopy.c` +
`kernel/arch/x86_64/usercopy.S` add `copy_from_user`/`copy_to_user` (ADR D9) and
the dev-only `copy_probe` syscall (op 3). Every copy prevalidates the span
(canonical, no wrap, in-arena, per-page present+permission) before touching it,
copies through ONE approved assembly site, and a per-thread copy scope + the
#PF-handler fixup (`je_usercopy_try_fixup`, called from `je_trap_dispatch`)
recover ONLY a not-present (P=0) fault at the EXACT copy-site RIP with an active,
owner-matched, direction-matched scope and CR2 in the declared span. Any other
kernel #PF — protection fault, different RIP, no scope — stays fatal. A recovered
fault → JE_ABI_INVALID (treated as a copy failure, not masked).

Dispatch order: `cs==8` vector-14 → try fixup → else existing kernel PANIC; then
the user int 0x80 / fault-terminate paths unchanged. The dev-only fixup-injection
op (op 4) and its user stub are compiled ONLY under `-DJE_DEV_COPY_INJECT`
(TEST_MODE 11); a `make check` `nm` assertion proves those symbols are absent
from the default kernel.

Peer: design done via a 2-round cross-talk DEBATE (the new peer-debate tool). The
debate's key refinement: recover only P=0 not-present faults, treat a protection
fault after validation as fatal (don't mask a validator bug). Codex's follow-up
check corrected the injection to run from a real CPL3 syscall context (genuine
owner/scope), flagged a stale bootstrap self-test call (removed), and had me
tighten `cs&3==0` to exact `cs==8`.

Tested in QEMU (this env): `make check` PASS (op-3 decode test + ABI isolation +
dev-inject-absent-from-default-ELF assertions), `make smoke` PASS — the boot runs
the copy_probe battery (valid+zero OK; null/kernel-range/len>256 INVALID; valid
bytes verified moved) with peer B live. `make integration` PASS — 11 modes; mode
11 proves the fixup RECOVERS a copy-site #PF (dev inject) while modes 4/5 still
PANIC on a kernel-origin fault (not wrongly recovered). All three CI smoke
profiles pass; M2's 4 markers unchanged. Honest limits recorded: no SMAP/SMEP, so
the net covers the approved site only; fail-closed under uncorrupted-kernel
invariants, not absolute. This COMPLETES the ADR 0007 M3a userspace boundary
(#2–#5). AI-assisted review only; not independent human qualification.

## M3a #5 step (a): v0 syscall dispatcher — observed 2026-10-17T17:40:00+00:00

Branch `feat/m3a-5a-syscall-dispatch`. The DPL3 `int 0x80` gate now dispatches a
real experimental v0 syscall (previously it routed straight to death). New
`kernel/core/syscall.c` (`je_syscall_decode` pure + unit-tested; `je_syscall_dispatch`
glue), `include/jeios/abi.h` (separate, explicitly-numbered ABI status/ops,
never the internal `status.h`). trap.c routes user-origin `int 0x80` to the
dispatcher before the fault path; kernel-origin 0x80 stays fatal.

Ops: `abi_query(which)` (version/profile in RDX), `yield()` (reschedule via the
kind-aware path, RAX=OK written before switching), `exit(code)` (DEATH_EXIT +
code). Unknown op → UNSUPPORTED (full 64-bit decode); recognized op with a
nonzero reserved arg → INVALID, no side effect. Every non-switching return goes
through `je_sched_return_to_user` → re-validates+sanitizes the frame, terminating
(DEATH_BAD_RETURN) rather than IRETQ a bad frame.

Owner-approved tested-code touch (kept M2+step4 green): the step-4 death record
gained `reason {FAULT,EXIT,BAD_RETURN}` + `exit_code` (the `0xbad` magic vector
removed); a sibling `je_sched_exit_current` was added rather than renaming the
tested `je_sched_fault_terminate`.

Peer: pre-write design vote (both agents) caught the direct-return-skips-validation
gap (fixed by `je_sched_return_to_user`). Codex cross-review found no
escape/UAF/ABI-leak and drove three closures: test asserts RDX=0 on INVALID/
UNSUPPORTED; a new QEMU stub proves BAD_RETURN *through the syscall return path*;
and these doc updates.

Tested in QEMU (this env): `make check` PASS (+`test_syscall` + abi.h isolation
grep), `make smoke` PASS — boot proves exit(42)→clean EXIT, abi_query/yield
return results to ring 3, unknown op→UNSUPPORTED+survives, bad-frame-on-return→
BAD_RETURN death, all with peer B live and page accounting restored.
`make integration` PASS (10 modes). All three CI smoke profiles pass locally;
M2's 4 markers unchanged. Next: step (b) — copy_from/to_user + copy_probe +
exception-fixup copy sites (the only genuinely tricky remaining piece). NOT yet
claimed: any user-copy/pointer handling, a stable ABI. AI-assisted review only;
not independent human qualification.

## M3a #4 step 4: fault-local process death + D6 int 0x80 gate + H1 fix — observed 2026-10-17T15:30:00+00:00

Branch `feat/m3a-4-step4-fault-local`. #4's core acceptance: a synchronous USER
fault — or a user `int 0x80`, or bad-return-state — terminates ONLY the
offending process; the kernel and peers keep running.

- `je_trap_dispatch` routes a user-origin ((cs&3)==3) synchronous fault in the
  explicit terminate-list {0,1,3,4,5,6,7,10,11,12,13,14,16,17,19} OR a user
  `int 0x80` to `je_sched_fault_terminate`. NMI(2)/#DF(8)/#MC(18)/reserved and
  any kernel-origin fault stay fatal PANIC (unchanged).
- `je_sched_fault_terminate` marks the current user thread DYING (death record),
  returns a LIVE successor's frame. It does NOT free the dying task's kstack/CR3
  (kernel C still runs on that RSP0 kstack); the bootstrap reaper frees it later.
  Triple-checks cs/ss + kind==USER + frame==rsp0_top-sizeof before acting.
- **H1 security fix:** `je_sched_interrupt` previously called `abort_scheduler()`
  (whole-kernel halt) when a *next* user thread's saved frame failed
  `je_uframe_check` — a userspace DoS (ring-3 sets a bad RSP, never faults,
  kernel dies at its next dispatch). The shared `select_and_install_next` now
  terminates that thread and retries another candidate; bootstrap is always a
  live fallback. A user task can no longer halt the kernel via a bad saved frame.
- D6 gate: `idt[0x80].attributes=0xee` (present, DPL3, IST=0, 64-bit interrupt
  gate — IF cleared on entry). Only vector 0x80 is user-reachable; kernel 0x80
  PANICs; every other gate stays DPL0 (user int of any other vector -> #GP ->
  terminate). No syscall handler yet — a user int 0x80 is an unsupported op
  routed to death (ADR amendment option 3 satisfied).
- Reaper `je_sched_reap_dead` (bootstrap, slot 0, asserts IF=0): for each DYING
  user slot, assert not-current / not-active-CR3 / not-active-RSP0, then
  `je_sched_drop_user` (restore kstack guard) + `je_vm_destroy`. New DYING slot
  state; `vm.c` UNTOUCHED (the vm record stays READY until destroy; D5's
  DYING/REAPABLE are modeled in the scheduler slot). Invariant recorded:
  single-CPU, PCID off, IF=0 in the fault handler — SMP would need a per-CPU
  off-stack handshake.

Tested in QEMU (this env): `make check` PASS, `make smoke` PASS — the normal boot
runs the two-task acceptance: A (reads an unmapped arena page) dies by user #PF
(vector 14, cs=0x33, CR2=the unmapped page); B (heartbeat loop) is proven to
advance AFTER A's death (heartbeat + dispatch + preemption all strictly increased
from the post-death snapshot); A is reaped from bootstrap while B stays live; then
the user `int 0x80` and bad-RSP(H1) death causes are each exercised and
terminated fault-locally with B still running; B torn down with page accounting
back to the pre-step-4 baseline. `make integration` PASS — 10 modes; mode 10
FLIPPED from step 3: user `xorps` #UD now terminates the task (fault-local) and
the kernel reaches the normal halt (asserts no "JeiOS fatal:"). All three CI
smoke profiles pass locally; M2's 4 markers unchanged (owner sign-off condition).

Peer: pre-write design vote (both agents) found the H1 kernel-DoS in merged main
and the "first entry rides a real trap" rule; Codex cross-review then confirmed no
UAF/escape and flagged hardening (reaper now self-asserts IF=0; ss checked in
terminate) + test gaps (int 0x80 and H1 death now runtime-tested, not just
claimed) + wording, all addressed.

Honest claims: synchronous user faults, bad-return-state, and user int 0x80 kill
only the offender; a peer is shown to progress after the death; the faulted
process is reclaimed (per-cause reaps + aggregate page-count equality); the D6
gate is present and leads to death. NOT claimed: two-task isolation qualification,
syscalls/ABI, copy-in/out, a process exit path, general/complete reclamation
across all paths, SMP. #4 acceptance is met; #5 is the integrated qualification +
syscalls. AI-assisted review only; not independent human qualification.

## M3a #4 step 3: CPL3 entry/return — real ring-3 execution — observed 2026-10-17T12:20:00+00:00

Branch `feat/m3a-4-step3-cpl3-entry`. The scheduler now IRETQs into ring 3.
Added: thread kind (kernel/user) + `je_thread_create_user` (synthesizes the
initial user frame on the process's held kstack, requires NX per D2);
`je_sched_interrupt` validates the incoming frame by kind and, for a user next
thread, runs `je_uframe_check` (pure, hostable, unit-tested) to validate+sanitize
RFLAGS, sets TSS.RSP0, loads the process CR3 before the single IRETQ; kernel
threads get RSP0=0 + kernel root. `je_sched_reap` excludes user threads (kstack
is vm-owned — avoids double-free); `je_sched_drop_user` detaches + restores the
kstack guard before `je_vm_destroy`. D7 `je_arch_cpu_policy_init` sets
CR0.EM/MP/TS, clears CR4.OSFXSR/OSXMMEXCPT/OSXSAVE/FSGSBASE, nulls DS/ES/FS/GS.
Gates stay DPL0 — the D6 `int 0x80` gate lands in step 4 (ADR amendment: present
by end of #4, wired to fault-local death; option 3, owner-approved).

Peer process: pre-write design vote (both agents) caught the reap double-free and
the first-entry-must-ride-a-real-trap rule; Codex cross-review then found four
defects, all fixed: incomplete segment/FSGSBASE normalization, D7 marker
overclaim (now proven by an executing #UD test, mode 10), an RFLAGS/DF
contract-vs-impl mismatch (DF now cleared, test asserts it), and an ignored
`drop_user` teardown result (now checked).

Tested in QEMU (this env): `make check` PASS (+`test_uframe`), `make smoke` PASS
(a built-in ring-3 loop reached CPL3, survived >=16 preemptions, advanced its
heartbeat ~9-17M times, torn down clean), `make integration` PASS (10 modes; new
mode 10 = user `xorps` at CPL3 -> `#UD` from a cs=0x33 frame, hardware D7 proof).
All three CI smoke profiles pass locally; M2 markers unchanged (sign-off
condition). Next (step 4): fault-local offender death + install/wire the D6
`int 0x80` gate to it. NOT yet claimed: inter-process isolation, fault-locality,
the gate, syscalls. AI-assisted review only; not independent human qualification.

## M3a #4 step 2: real per-process page tables + CR3 (CPL0) — observed 2026-10-17T11:45:00+00:00

Branch `feat/m3a-4-step2-vm-backend`. The real arch backend
(`kernel/arch/x86_64/vm_backend.c`) now implements the frozen #3 `je_vm_ops`
against actual page tables, producing a real per-process CR3. `paging.c` grew a
small seam: `je_kernel_pd_phys`, `je_user_alias_set/get` (RW↔RO_NX CAS, A/D
masked), `je_arch_cr3_read/load`, `je_arch_kernel_root_phys`. Topology matches
the merged #3 audit: `PML4[0] P|U → PDPT[0] P,!U (shared kernel PD) → PDPT[1]
P|U → PD[0] P|U → PT`. Intermediates also carry W (effective write = AND of W
across levels under CR0.WP); leaves encode CODE=RX, RODATA=R/NX, DATA=RW/NX.

Owner-approved, done: (1) hardened `je_arch_page_guard`/`restore` to refuse any
non-exact-RW leaf (fixes a latent bug where the guard path could silently
un-demote a published RO_NX alias); (2) `_Static_assert(JE_PHYSICAL_LIMIT <=
JE_VM_ARENA_BASE)` + `<= 4 GiB` pinning the uint32_t root. Two real bugs found
during bring-up and fixed: guard rejected fresh stack pages until A/D (bits 5,6)
were masked; CR3 load triple-faulted until all intermediate entries carried W.
Cross-review (Codex) then found and I fixed three more: OOB on an invalid
`frame.process` in install/clear_pte, missing alias-enum validation, and
`read_user_pte` not rejecting a present leaf lacking U.

Tested in QEMU (this env): `make check` PASS, `make smoke` PASS (mode 0 drives
the full build→audit→CR3 load→arena read→data RW→restore→destroy path with clean
page accounting), `make integration` PASS — all 9 fault modes, incl. new mode 8
(read arena under kernel root → `#PF err 0 cr2=0x40000000`) and mode 9 (write

 succeeded in 0ms:
# Coding-agent starting guide

Updated 2026-10-16. Read this with ../AGENTS.md, HANDOFF.md and IMPLEMENTATION.md.
Owner requested a sustained coding-agent handoff. This guide records requirements
and continuation order; it does not claim later milestones are implemented.

Current status and timestamped history: [HANDOFF.md](HANDOFF.md). Ecosystem-wide
[zero trust](ZERO_TRUST.md) is an owner requirement; enforcement remains planned.

## Product philosophy across every subsystem

Owner clarification 2026-10-14: seamless everyday defaults plus granular,
customizable power-user/developer/organization control govern the whole OS and
ecosystem. Read the overall product vision near the start of DESIGN.md.
Security profiles are one application; apply the same progressive disclosure,
per-area customization, automation, coherent cross-device behavior and honest
recovery principles to all subsystem work. These are requirements, not implemented
features. Keep interface complexity separate from actual authority and capability.

## Establish the correct baseline

Foundation PR: https://github.com/auto44e/jeios/pull/1
PR #1 merged into main at `03aa2f380d8e49262abd1720a3b8db37517cbb31`.
Start new implementation branches from updated main; check for newer concurrent
work before editing. The additional multitouch/security-key requirements are
proposed documentation only. Verify CI for the exact implementation head used.

Implemented: M1 exceptions, validated memory discovery/protected mappings, and M2
bounded allocation plus seven trusted preemptible kernel workers on one AMD64 CPU.
Not implemented: userspace, capabilities/IPC, SIMD context, SMP, filesystem,
networking, GPU/NPU/media runtime, login, sync, encryption or domains.

## Work sequence and project-ready tasks

| Order | Issue | Dependency | Completion evidence |
| --- | --- | --- | --- |
| 1 | [#2: boundary contracts](https://github.com/auto44e/jeios/issues/2) | M1/M2 baseline | Reviewed ADR, register/state/VM/ABI contracts |
| 2 | [#3: private VM and bounded images](https://github.com/auto44e/jeios/issues/3) | #2 | Ownership, zeroing, permissions and rollback tests |
| 3 | [#4: CPL3 scheduling and local faults](https://github.com/auto44e/jeios/issues/4) | #2, #3 | Actual CPL3 execution, timer preemption, survivor progress |
| 4 | [#5: syscalls and user copies](https://github.com/auto44e/jeios/issues/5) | #3, #4 | Pointer/arithmetic failure tests and narrow fault recovery |
| 5 | [#6: qualify M3a](https://github.com/auto44e/jeios/issues/6) | #2–#5 | Adversarial QEMU and regression evidence on exact head |
| 6 | [#7: M3b handles](https://github.com/auto44e/jeios/issues/7) | #6 | Type/generation/rights/lifetime tests |
| 7 | [#8: M3c IPC and init](https://github.com/auto44e/jeios/issues/8) | #7 | Bounded transfer/wait/close/cancel semantics |
| Backlog | [#9: later owner requirements](https://github.com/auto44e/jeios/issues/9) | Relevant roadmap gates | Split into bounded tasks when prerequisites exist |

All eight issues start planned/open, not in progress or done. No assignee or
calendar deadline was invented. Project destination:
https://github.com/users/auto44e/projects/1/views/1
Issue creation does not imply membership in the project board.

Read [M3A.md](M3A.md) for the concrete proposal. Its temporary 2 MiB user arena,
limits, ABI numbers, interrupt entry and NX-required launch policy are proposed
engineering choices, not previously implemented interfaces or a frozen owner ABI.
Resolve these in the first ADR. Keep M3a/M3b/M3c distinct and reviewable.

## Owner requirements and authoritative design locations

| Requirement | Source and placement | Implementation boundary |
| --- | --- | --- |
| Fresh C11 core, modular services, Unix interoperability/NT-style object model | [DESIGN](DESIGN.md), ADR 0001/0002 | No foreign kernel-source fusion; ring 0 is trusted |
| Privacy, explicit authority, graceful failures; customization with good defaults | [SECURITY](SECURITY.md), [DESIGN](DESIGN.md) | UI complexity never grants rights; no telemetry/cloud-login requirement |
| OS-wide touchscreen/trackpad multitouch | [INPUT](INPUT.md), M4/M7/E1 | Qualified transports, secure routing, native toolkit/accessibility; foreign apps may use pointer fallback |
| Built-in hardware MFA security keys | [SECURITY_KEYS](SECURITY_KEYS.md), M4-M9 | CTAP/FIDO2 broker, native/WebAuthn boundaries, recovery; preboot/PQ capabilities qualified separately |
| Fresh-kernel mobile and ecosystem Android app support | [MOBILE](MOBILE.md), dedicated auto44e/jeios-mobile | Owner chose native C kernel; QEMU ARM64 first, Pixel candidates unqualified; AOSP is reference/future app layer |
| Native configurable init and supervision | [INIT](INIT.md), M3c/M4 | Minimal fixed-plan init first, full supervisor later |
| Single-device local login through multi-device/family/multi-user/fleet management | [IDENTITY](IDENTITY.md), M3 onward | Separate principal/device/session/tenant and management vs data-key authority |
| E2E protection built into identity/sync design; several pairing methods | [IDENTITY](IDENTITY.md), [ENCRYPTION](ENCRYPTION.md) | Qualified key lifecycle, entropy and reviewed protocols; no ad hoc crypto |
| PQ protection, nearby sharing and honest RAM/endpoint trust limits | [ENCRYPTION](ENCRYPTION.md), M5/M6/M9/P2 | Encryption is not universal secrecy from a privileged running OS |
| ext4 as provisional initial OS default | [STORAGE](STORAGE.md), M5 | Owner decision; still no filesystem code or production qualification |
| Raw-disk access without forced formatting; temporary/sidecar file-like views | [STORAGE](STORAGE.md) | Explicit range authority/leases and metadata; never silently format |
| Broad open-source filesystem interoperability | [STORAGE](STORAGE.md) | Qualify each format/version/read/write profile independently |
| Native domains and AD/Group Policy-like management | [DOMAINS](DOMAINS.md) | Native directory distinct from Microsoft AD DS compatibility |
| Windows/macOS/Linux domain interoperability | [DOMAINS](DOMAINS.md) | Explicit provider/endpoint matrix; Samba AD and FreeIPA/LDAP/Kerberos separately scoped |
| GPU/NPU across all planned platforms; open standards by default | [ACCELERATORS](ACCELERATORS.md), M4/M8 | Native drivers and tested per-device support; no universal availability claim |
| Smart CPU→GPU/NPU offloading, configurable APIs, conservative defaults | [ACCELERATORS](ACCELERATORS.md) | Eligible backend implementations, resource/latency/power budgets, precision and CPU fallback |
| Built-in FFmpeg, HandBrake and Blackmagic RAW support | [ACCELERATORS](ACCELERATORS.md), M5–M8 | Userspace integration; separate upstream licenses and native SDK feasibility |
| Apache-2.0 selected for original project code | [LICENSE](../LICENSE), [ADR 0005](adr/0005-licensing.md) | Implemented licensing; third-party provenance/terms still reviewed |
| Old AMD64 compatibility, AArch64/mobile/IoT and coherent customizable desktop | [HARDWARE](HARDWARE.md), [ROADMAP](ROADMAP.md), [DESIGN](DESIGN.md) | Qualified profiles and dependency gates; no shortcut past isolation |

## Sustained implementation discipline

Work one bounded gate at a time; continue routine implementation and fixes without
reasking already decided owner preferences. Keep small reviewable commits. Record
new architectural decisions and rationale in ADRs. Ask only when a material owner
decision or access limitation genuinely blocks useful work. Do not silently relax
security, replace standards-first goals with mandatory vendor dependencies, or
claim a later gate because its design document exists.

Before each change, inspect current code, open issues/PRs and tests to avoid
redoing work or overwriting another agent. Prioritize executable user-page aliases,
unsaved architectural state, safe TSS/CR3 switching, exact copy fixups, lifetime
and reclamation. A security invariant that cannot be upheld blocks the feature;
record the failure and next bounded fix rather than forging a passing gate.

At each checkpoint update HANDOFF, IMPLEMENTATION and ROADMAP in the same change:
implemented/tested/planned/blocked, exact commit and commands, observed results,
changed contracts, known failures and next task. Update related security/hardware
claims and issue state only to match evidence. Keep local and remote CI distinct.
Run `make check`; in this managed environment use
`ASAN_OPTIONS=detect_leaks=0 make check` only if LeakSanitizer cannot inspect tasks,
and disclose that limitation. Boot changes require `make smoke integration` with
new meaningful QEMU tests, exact expected results and a live survivor for fault
containment. Run `git diff --check` and validate documentation links.

Before ending a work session leave reproducible build/run instructions, current
branch/commit, a clean committed checkpoint where possible, remaining test failures,
resource/accounting invariants and next step. Do not merge/release, assign reviewers,
change credentials, contact vendors or import restricted source without applicable
authorization. The owner asked to approve PR #1; that is not an instruction to
invent an independent reviewer or silently bypass self-review restrictions.

## Cryptographic relock requirement — 2026-10-14

ENCRYPTION.md now requires a configurable ordinary-lock path to withdraw protected
user-domain keys and plaintext access without a power cycle. Include process/cache/
DMA cleanup, credential-gated reopening, background exceptions and truthful failure
status. Screen lock alone is insufficient; reboot equivalence is not assumed.
Key-domain and lifecycle design precede storage/identity format freeze; runtime
qualification remains a later gate. No implementation was added.

## Protected system and version management — owner decision 2026-10-14

STORAGE.md records seamless protected/versioned system deployments by default,
full owner customization including an immutability off switch, and independent
intuitive OS/built-in-app update/rollback in both protected and mutable modes.
Native apps must participate through lifecycle and schema compatibility contracts.
Write protection, deployment history, trust policy and data encryption are distinct.
Mutable-mode direct edits require explicit conflict/checkpoint handling; rollback
must not silently revert user documents or bypass data/anti-rollback constraints.
Accepted direction, not implemented behavior; M3a remains next.

## Multiple default filesystems — owner decision 2026-10-14

STORAGE.md now records Btrfs, OpenZFS and a mobile backend as first-class long-term
default choices selected by qualified device profiles. F2FS is the proposed mobile
candidate. Owners can select a conforming custom backend, including as system
default with additional boot/recovery qualification, or use a visibly experimental
profile. Native APIs keep workflows coherent without claiming identical features.
ext4 stays the initial bring-up target. No automatic reformat/conversion or new
implementation is implied. M3a remains next.

## F2FS approval and agent tasks — 2026-10-14T20:43:47-04:00

Owner approved F2FS as the provisional mobile default, subject to actual JeiOS
qualification. This supersedes earlier candidate-only wording; device support,
feature flags and production readiness are not implied. Btrfs/OpenZFS remain
long-term default targets; ext4 remains the initial core bring-up backend.

Ready-to-start bounded work:
- [Core #13](https://github.com/auto44e/jeios/issues/13): backend contract ADR,
  hosted fake-block-device and conformance harness.
- [Core #14](https://github.com/auto44e/jeios/issues/14): F2FS format/provenance
  review and bounded read-only hosted metadata probe with negative tests.
- [Mobile #7](https://github.com/auto44e/jeios-mobile/issues/7): mobile F2FS
  workload/qualification profile and shared-backend integration plan.

These issues are open/unassigned at creation; no coding agent has been launched
by creating them and project-board membership has not been verified. Hosted
prototypes can proceed independently while runtime integration waits for M3/M4/M5
and relevant mobile execution/driver gates. No writes to real user media.
M3a remains the next core kernel gate; MP0 remains the mobile architecture gate.

## Core standards workstreams — 2026-10-16

Read [STANDARDS.md](STANDARDS.md). POSIX.1-2024 and built-in LDAPv3 are accepted
core commitments; staged runtime implementation remains blocked on native gates.
[POSIX #15](https://github.com/auto44e/jeios/issues/15) defines the profile and
conformance roadmap; [LDAP #16](https://github.com/auto44e/jeios/issues/16) defines
the isolated provider and identity-broker contract. Begin bounded design work
without displacing M3a. Mobile parity reuses these contracts and implementations.
Historical wording that treats generic LDAP as only a candidate is superseded.

## Curl-inspired CI direction — 2026-10-16

[CI_STRATEGY.md](CI_STRATEGY.md) records the owner's short/long-term testing goal:
portable regressions, compiler diversity, sanitizers/static analysis, replayable
failure injection and fuzzing, supplemented by native kernel execution and storage
crash-recovery qualification. [Core #18](https://github.com/auto44e/jeios/issues/18)
is the first bounded CI implementation task. Existing gates remain mandatory;
M3a and mobile MP0 remain next. This update adds planning only, not workflow jobs.

## Ecosystem assurance direction — 2026-10-16

[CODING_STANDARD.md](CODING_STANDARD.md) defines NASA/JPL-inspired risk profiles
and ten project rules. Core #19 tracks baseline audit and enforcement, alongside
CI #18. Apply across core, services, apps, tooling and mobile; no compliance audit
or new CI checks are claimed. M3a remains next.

 succeeded in 0ms:
# JeiOS

JeiOS is an experimental operating system with a fresh C kernel. Its design
combines Unix interoperability, an NT-inspired object/service model, and an
integrated, deeply customizable desktop. Privacy, explicit authority, recoverable
operations and qualified hardware support are long-term goals.

**Current stage: M1/M2 single-CPU kernel foundation.** The AMD64 BIOS prototype
boots, handles fatal CPU exceptions, validates Multiboot2 memory information,
enforces read-only/NX mappings where supported, allocates RAM, and preemptively
runs seven guarded kernel threads. Its diagnostic workload verifies registers,
sleep/wakeup, heap contents and resource reclamation, then halts.

There is no userspace, application sandbox, SMP, filesystem, graphics, networking,
dynamic module loader or ARM implementation. This is not a daily-use or hardened OS.

## Start here

Coding agents: begin with [the starting guide](docs/AGENT_START.md) for the current
baseline, all owner requirements, issue dependencies and checkpoint rules.

- [Detailed design](docs/DESIGN.md): full product architecture and future contracts.
- [M3a implementation proposal](docs/M3A.md): proposed boundaries and M3b/M3c contracts.
- [Accelerators and media](docs/ACCELERATORS.md): open standards, conservative offloading,
  and planned FFmpeg/HandBrake/Blackmagic RAW integration; no runtime support yet.
- [Init/service design](docs/INIT.md), [encryption and nearby sharing](docs/ENCRYPTION.md),
  and [filesystem recommendation](docs/STORAGE.md): planned contracts and acceptance gates.
- [Identity and device management](docs/IDENTITY.md): local login, E2E sync, family and fleet contracts.
- [Domains and centralized policy](docs/DOMAINS.md): native joining and separate AD/GPO compatibility targets.
- [Multitouch input](docs/INPUT.md) and [hardware security keys](docs/SECURITY_KEYS.md): planned OS-wide integration.
- [Fresh-kernel mobile project](docs/MOBILE.md): shared core, capability parity and future Android app layer.
- [Implementation ledger](docs/IMPLEMENTATION.md): current M1/M2 mechanisms, evidence and limits.
- [Hardware policy](docs/HARDWARE.md) and [security model](docs/SECURITY.md).
- [Development](docs/DEVELOPMENT.md): build, emulator tests and fault modes.
- [Roadmap](docs/ROADMAP.md): gated milestones and dependency order.
- [Agent handoff](docs/HANDOFF.md) and [AGENTS.md](AGENTS.md): continuation instructions.
- [References](docs/REFERENCES.md): primary architecture sources.

## Build and verify

On x86-64 Linux with GCC, binutils, make and Python 3:

```sh
make check
```

With GRUB PC utilities, xorriso and QEMU installed:

```sh
make smoke integration
```

Normal outputs are in `build/0/`; each fault mode uses a separate directory.
See DEVELOPMENT.md for sanitizer requirements, CPU/memory profiles and limitations.

The OS core uses C11 with small architecture-specific assembly bridges. Early
AMD64 is the first compatibility target; AArch64 requires a separate port.
Modern features are enabled only after their OS prerequisites are implemented.
The desktop, native SDK, privacy controls and mobile/IoT ecosystem remain planned.

Original JeiOS code is licensed under [Apache License 2.0](LICENSE), selected by
the owner on 2026-10-13. Third-party components retain their own licenses and
require provenance/compatibility review. No external kernel source has been
imported. See [the licensing decision](docs/adr/0005-licensing.md).

## Core standards direction

[POSIX.1-2024 and LDAPv3](docs/STANDARDS.md) are accepted core requirements,
implemented in stages across desktop/mobile. Both remain planned; neither current
conformance nor directory login is claimed. Organizational enrollment is optional.

exec
/usr/bin/bash -lc 'git status --short && git branch --show-current && git rev-parse HEAD && git diff --stat' in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "rg -n \"je_sched_block_current|je_sched_interrupt|wait_pending|je_syscall_finish_blocked_recv|je_endpoint_wait_|kern_peer_close_wake|send_wake|BLOCKED_IPC|positional\" kernel include tests -g '*.[chS]' -g '*.mk'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,260p' docs/CODING_STANDARD.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "find . -maxdepth 3 -type f | sort | sed -n '1,260p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
# Ecosystem coding and assurance policy

Owner direction: 2026-10-16T20:01:26-04:00, reaffirmed
2026-10-16T20:08:52-04:00. Apply the spirit of NASA/JPL engineering throughout
JeiOS. This is a tailored project policy proposed in PR #17, not NASA certification,
literal Power of Ten conformance, or evidence that existing code has been audited.
Automated enforcement and baseline assessment are tracked in
[core #19](https://github.com/auto44e/jeios/issues/19), alongside
[CI #18](https://github.com/auto44e/jeios/issues/18).

## Scope and risk profiles

Apply to original code, tests, build tools, packaging, services, SDKs, desktop,
mobile and future IoT integrations. Human and AI contributions use the same rules.
Classify by failure impact and authority, not implementation language or directory.
These are JeiOS profiles, not NASA software classifications.

| Profile | Typical components | Required emphasis |
| --- | --- | --- |
| Critical | Kernel, boot, privileged drivers, isolation, identity/key services, storage commits, updates/recovery, release tooling | Resource bounds, explicit invariants, failure/rollback evidence, independent review before a hardened/release qualification claim |
| Service | Contained brokers, networking, compatibility runtimes, persistent app services | Validated interfaces, quotas, cancellation, restart/recovery, concurrency and hostile-input tests |
| Application/tool | Ordinary UI, SDK examples, development utilities | Language-appropriate lint/type checks, error handling, tests proportionate to risk, reproducible dependencies |

A UI authorization prompt, image packager or signing helper can be critical.
An unclassified component must be assessed before privileged integration. A service
profile assumes demonstrated containment; a module running in ring 0 is critical.
Do not impose C syntax restrictions on Python or other future userspace languages.
The shared kernel remains C11 with architecture assembly at documented boundaries.

## Ten JeiOS rules

| ID | Rule | Review or test evidence |
| --- | --- | --- |
| JCS-01 | Keep control flow simple and functions focused. Critical C prohibits direct/indirect recursion and setjmp/longjmp absent an approved deviation. A forward cleanup goto is permitted when it makes ownership unwind explicit. | Call/cleanup review; flag functions over roughly 60 logical lines for decomposition or recorded rationale, not arbitrary splitting. |
| JCS-02 | Bound work, buffers, retries, stack use and queues. Each finite critical loop needs a demonstrable input/capacity bound or explicit budget. | Limit and timeout tests; document maximum work per dispatch. Persistent scheduler/event loops require progress/block/yield and shutdown semantics where applicable. |
| JCS-03 | Specify ownership, lifetime and allocation context. No unbounded allocation; prohibit blocking allocation in interrupt, fault and other nonblocking paths. | Exhaustion, fail-Nth allocation, partial-init unwind and use-after-free checks. Use preallocation/pools for deterministic paths. |
| JCS-04 | Validate external inputs before use, including sizes, arithmetic, alignment, encodings and authority. Check fallible results; justify deliberate ignored results locally. | Boundary, overflow, malformed-input, permission-denial and error propagation tests. Internal helpers may rely on documented validated preconditions. |
| JCS-05 | Encode meaningful invariants with side-effect-free assertions and explicit failure policy. Runtime input validation must survive release builds. | Pre/postcondition and failure tests; malformed user/network data must not become a kernel assertion crash. No artificial assertion quota. |
| JCS-06 | Minimize scope and mutable shared state. Document lock order, caller context, atomic ordering and cancellation ownership. | Concurrency/lifetime review and targeted race schedules as concurrency lands; volatile is not synchronization. |
| JCS-07 | Keep preprocessing, pointer manipulation and unsafe boundaries small and visible. Prefer typed helpers; validate callbacks and document their allowed targets/lifetimes. | Review casts, pointer arithmetic, macro evaluation and dispatch tables. No hidden multiple evaluation or unsupported configuration claims. |
| JCS-08 | Build with documented strict warnings and appropriate static analysis. Resolve new findings or record narrow reviewed deviations. | Exact tool versions/configuration and results; no blanket suppression or lowering warnings to pass. Analyzer coverage must name the files/configurations checked. |
| JCS-09 | Trace behavior to a requirement/contract and repeatable evidence, especially failure paths. | PR links contract, changed invariant, positive/negative tests and exact build/target; preserve regression inputs and distinguish host, emulator and physical evidence. |
| JCS-10 | Keep changes reviewable, dependencies traceable and exceptions explicit. Test infrastructure itself must detect known defects. | Review record, provenance, version pin/update policy, gate failure demonstrations and deviation register. Never fabricate independent review or a passing check. |

## Deliberate adaptations of the Power of Ten

The original C-oriented paper restricts allocation after initialization, function
pointers, pointer indirection, recursion and control flow. It also advocates small
functions, assertions, narrow scope, checked results and strong analysis. JeiOS
adopts the analyzability goal with the explicit adaptations above. Dynamic process
and filesystem workloads need allocation; drivers need reviewed dispatch tables.
These mechanisms require quotas, exhaustion handling, clear lifetimes and tests.
Only paths with a justified deterministic contract prohibit runtime allocation.

Do not translate a timing budget into a claim of proved worst-case execution time.
Do not treat passing sanitizers, high coverage or a style checker as proof of
memory safety, functional correctness or security. Assembly needs register/stack,
clobber, interrupt, barrier and architecture-precondition review plus execution
tests; a C analyzer cannot validate it by implication.

Assertions detect internal contract violations. Recovery must match the failure
domain: reject invalid input, fail a transaction, terminate an isolated service,
or stop safely on unrecoverable kernel corruption. Never continue with corrupted
authority or persistent state merely to avoid a crash.

## Adoption, review and deviations

After adoption, new and materially changed original code must address applicable
rules in its PR. Existing code has no retroactive compliance claim: first inventory
critical paths and record findings, then fix them in bounded changes. A known
critical defect blocks the affected qualification claim; an untouched baseline
finding needs an owner and disposition, not silent grandfathering.

PR evidence must identify profile, applicable rule IDs, changed contracts, tests,
unverified checks and deviations. Independent review is required before claiming
critical hardened/release qualification. If another qualified reviewer is not
available, record that gap; author/AI self-review is not independent assurance.
Development may continue with that qualification explicitly unclaimed.

A deviation record includes ID, rule, exact scope/revision, reason, alternatives,
risk, mitigation, verification evidence, responsible owner, actual reviewer/status
and expiry or a concrete revisit trigger. Pending is not approved. Put records
under docs/deviations/ when first needed; do not invent a reviewer or waiver.
Rule-specific permissions above are policy adaptations, not hidden deviations.

Imported code retains its provenance, language and license. Evaluate exposure,
upstream quality, update strategy and integration tests; isolate it where possible.
Do not relabel it as conforming or perform an enormous style rewrite just to match
local rules. Any critical dependency needs evidence suitable for its actual role.

## Delivery stages and enforcement status

1. Now: publish this policy in core and reference it from mobile contributor
   instructions. Preserve M3a and MP0 as next gates. Documentation is not enforcement.
2. Near term (#19 with #18): classify current components, audit critical baseline,
   map each rule to a checker or manual review, and add bounded CI checks. Record
   tool versions and prove a representative violation fails each new gate.
3. As services arrive: extend failure injection, parser fuzzing, concurrency tests,
   durable-state recovery and traceability to POSIX/LDAP requirements. Qualify
   native targets separately; keep diagnostics and test outcomes honest.
4. Before hardened/release claims: complete applicable reviews and unresolved-risk
   disposition, verify reproducible evidence and target-specific recovery. Use
   deeper structural coverage or formal models where the identified risk warrants
   them; neither 100% coverage nor formal verification is claimed today.

Existing checks and gaps remain documented in [CI_STRATEGY.md](CI_STRATEGY.md).
This policy change adds no analyzer, workflow, branch-protection setting or runtime
mechanism. It does not certify safety-critical or aerospace use.

## Primary references reviewed 2026-10-17 UTC

- [NASA NPR 7150.2D, Chapter 4](https://nodis3.gsfc.nasa.gov/displayDir.cfm?Internal_ID=N_PR_7150_002D_&page_name=Chapter4): project coding standards, static analysis, repeatable tests, requirements and target validation.
- [Gerard J. Holzmann, The Power of Ten](https://spinroot.com/gerard/pdf/P10.pdf): compact, analyzable rules for safety-critical C, published with NASA/JPL affiliation.

These are engineering references. NASA requirements apply within their own scope;
JeiOS does not claim agency approval or complete compliance by citing them.

 exited 127 in 0ms:
/usr/bin/bash: line 1: rg: command not found

 succeeded in 0ms:
M  AGENTS.md
M  CONTRIBUTING.md
M  docs/HANDOFF.md
M  docs/IMPLEMENTATION.md
M  docs/adr/0009-m3c-capability-ipc.md
M  docs/adr/debates/README.md
A  docs/adr/debates/m3c-step6-mechanism.md
M  include/jeios/abi.h
M  include/jeios/endpoint.h
M  include/jeios/endpoint_kern.h
M  include/jeios/sched.h
M  kernel/arch/x86_64/user_demo.S
M  kernel/core/endpoint.c
M  kernel/core/endpoint_kern.c
M  kernel/core/main.c
M  kernel/core/sched.c
M  kernel/core/syscall.c
M  tests/test_endpoint.c
M  tests/test_syscall.c
feat/m3c-waits-impl
927ee21de8bb31ef45b355c352d743a2a8fc5f8b

 succeeded in 0ms:
./.git/COMMIT_EDITMSG
./.git/FETCH_HEAD
./.git/HEAD
./.git/ORIG_HEAD
./.git/config
./.git/description
./.git/hooks/applypatch-msg.sample
./.git/hooks/commit-msg.sample
./.git/hooks/fsmonitor-watchman.sample
./.git/hooks/post-update.sample
./.git/hooks/pre-applypatch.sample
./.git/hooks/pre-commit.sample
./.git/hooks/pre-merge-commit.sample
./.git/hooks/pre-push.sample
./.git/hooks/pre-rebase.sample
./.git/hooks/pre-receive.sample
./.git/hooks/prepare-commit-msg.sample
./.git/hooks/push-to-checkout.sample
./.git/hooks/sendemail-validate.sample
./.git/hooks/update.sample
./.git/index
./.git/info/exclude
./.git/logs/HEAD
./.git/packed-refs
./.github/workflows/ci.yml
./.gitignore
./AGENTS.md
./CONTRIBUTING.md
./LICENSE
./Makefile
./README.md
./boot/grub/grub.cfg
./build/0/jeios.elf
./build/0/jeios.iso
./build/0/test_alloc
./build/0/test_boot
./build/0/test_core
./build/0/test_endpoint
./build/0/test_handle
./build/0/test_handle_rw
./build/0/test_syscall
./build/0/test_uframe
./build/0/test_vm
./build/1/jeios.elf
./build/1/jeios.iso
./build/10/jeios.elf
./build/10/jeios.iso
./build/11/jeios.elf
./build/11/jeios.iso
./build/2/jeios.elf
./build/2/jeios.iso
./build/3/jeios.elf
./build/3/jeios.iso
./build/4/jeios.elf
./build/4/jeios.iso
./build/5/jeios.elf
./build/5/jeios.iso
./build/6/jeios.elf
./build/6/jeios.iso
./build/7/jeios.elf
./build/7/jeios.iso
./build/8/jeios.elf
./build/8/jeios.iso
./build/9/jeios.elf
./build/9/jeios.iso
./build/aarch64/Image
./build/aarch64/boot.o
./build/aarch64/jeios-arm.elf
./build/aarch64/main.o
./docs/ACCELERATORS.md
./docs/AGENT_START.md
./docs/CI_STRATEGY.md
./docs/CODING_STANDARD.md
./docs/DESIGN.md
./docs/DEVELOPMENT.md
./docs/DOMAINS.md
./docs/ENCRYPTION.md
./docs/HANDOFF.md
./docs/HARDWARE.md
./docs/IDENTITY.md
./docs/IMPLEMENTATION.md
./docs/INIT.md
./docs/INPUT.md
./docs/M3A.md
./docs/M3A_RETRO.md
./docs/MOBILE.md
./docs/REFERENCES.md
./docs/ROADMAP.md
./docs/SECURITY.md
./docs/SECURITY_KEYS.md
./docs/STANDARDS.md
./docs/STORAGE.md
./docs/ZERO_TRUST.md
./docs/adr/0000-decision-record-policy.md
./docs/adr/0001-fresh-c-kernel.md
./docs/adr/0002-modularity-and-isolation.md
./docs/adr/0003-baseline-and-negotiated-features.md
./docs/adr/0004-user-experience-contract.md
./docs/adr/0005-licensing.md
./docs/adr/0006-bsp-foundation-and-page-heap.md
./docs/adr/0007-m3a-userspace-boundary.md
./docs/adr/0008-m3b-typed-handles.md
./docs/adr/0009-m3c-capability-ipc.md
./include/jeios/abi.h
./include/jeios/alloc.h
./include/jeios/arch.h
./include/jeios/boot.h
./include/jeios/endpoint.h
./include/jeios/endpoint_kern.h
./include/jeios/features.h
./include/jeios/handle.h
./include/jeios/kheap.h
./include/jeios/module.h
./include/jeios/paging.h
./include/jeios/sched.h
./include/jeios/status.h
./include/jeios/syscall.h
./include/jeios/trap.h
./include/jeios/uframe.h
./include/jeios/usercopy.h
./include/jeios/vm.h
./kernel/core/alloc.c
./kernel/core/boot.c
./kernel/core/endpoint.c
./kernel/core/endpoint_kern.c
./kernel/core/features.c
./kernel/core/handle.c
./kernel/core/kheap.c
./kernel/core/main.c
./kernel/core/memory.c
./kernel/core/module.c
./kernel/core/sched.c
./kernel/core/status.c
./kernel/core/syscall.c
./kernel/core/uframe.c
./kernel/core/usercopy.c
./kernel/core/vm.c
./mk/aarch64.mk
./mobile/jeios-mobile/README.md
./tests/test_alloc.c
./tests/test_boot.c
./tests/test_core.c
./tests/test_endpoint.c
./tests/test_handle.c
./tests/test_syscall.c
./tests/test_uframe.c
./tests/test_vm.c
./tests/vm_fake.c
./tools/__pycache__/emulator.cpython-312.pyc
./tools/check_elf.py
./tools/emulator.py
./tools/integration.py
./tools/smoke.py
./tools/smoke_arm.py

codex
The first source pass already found two concrete correctness concerns beyond the obvious test path: a reserved-head status regression in the refactored `recv_peek_caps`, and a likely unrecoverable `SHORT_BUFFER` continuation state. I’m tracing their exact lifetimes now, along with close/free ordering and the cancellation loop, before deciding the final severity/order.
exec
/usr/bin/bash -lc "nl -ba kernel/core/endpoint.c | sed -n '200,380p;480,580p;620,900p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba kernel/core/endpoint_kern.c | sed -n '1,380p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba kernel/core/sched.c | sed -n '1,390p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba kernel/core/syscall.c | sed -n '1,180p;200,380p;470,660p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba tests/test_endpoint.c | sed -n '1,520p'; nl -ba tests/test_syscall.c | sed -n '1,460p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
   200	    je_ep_end *e = &rec->end[side];
   201	    /* handle_refs == 0 here would be an invariant violation by the caller; guard
   202	     * defensively (never underflow) but do not mask it as success elsewhere. */
   203	    if (e->handle_refs == 0u) return;
   204	    e->handle_refs--;
   205	    if (e->handle_refs == 0u) {
   206	        e->state = JE_EP_END_CLOSED;
   207	        je_ep_end *peer = &rec->end[side ^ 1u];
   208	        if (peer->state == JE_EP_END_OPEN) peer->state = JE_EP_END_PEER_CLOSED;
   209	        /* A5-3 discard ordering: (1) DETACH every cap descriptor still queued in
   210	         * this closing end's inbound into a bounded local array; (2) reset the
   211	         * queue (bytes + cap storage) so this record is fully settled and holds
   212	         * no caps; (3) maybe_free THIS record; (4) ONLY THEN release each detached
   213	         * inflight ref on its (other-record) target. Doing releases last means
   214	         * freeing a target never re-enters a queue still being discarded, and the
   215	         * guarded maybe_free_record is idempotent if a target is this same record
   216	         * via the peer end (its inbound was/will be discarded at its own close). */
   217	        je_ep_capdesc detached[JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX];
   218	        uint32_t nd = 0u;
   219	        je_ep_queue *q = &e->inbound;
   220	        for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i)
   221	            for (uint8_t c = 0; c < q->cap_count[i]; ++c)
   222	                detached[nd++] = q->cap[i][c];
   223	        queue_reset(q);                  /* this end holds no caps now */
   224	        maybe_free_record(pool, rec);    /* settle THIS record first */
   225	        for (uint32_t k = 0; k < nd; ++k)
   226	            release_capdesc_inflight(pool, &detached[k]);  /* then other records */
   227	    }
   228	}
   229	
   230	/* ---- inflight ref API (tested directly; used by queue/wait PRs) ----------- */
   231	
   232	/* Is `rec` one of this pool's channel records? Uses uintptr_t comparison (not
   233	 * relational pointer comparison, which is UB for a pointer outside the array)
   234	 * and requires exact slot alignment, so a forged/other-pool pointer fails. */
   235	static bool rec_in_pool(const je_endpoint_pool *pool, const je_ep_channel *rec)
   236	{
   237	    uintptr_t base = (uintptr_t)&pool->rec[0];
   238	    uintptr_t top  = (uintptr_t)&pool->rec[JE_EP_POOL_CAP];   /* one-past-end */
   239	    uintptr_t p    = (uintptr_t)rec;
   240	    if (p < base || p >= top) return false;
   241	    return ((p - base) % sizeof(pool->rec[0])) == 0u;         /* slot-aligned */
   242	}
   243	
   244	/* ---- M3c D7/A6 blocking-recv waiter primitives (PURE) ---- */
   245	
   246	/* True iff this end's inbound head is reserved (claimed by a WOKEN waiter). A
   247	 * reserved head is NOT deliverable to any other receiver (A6-5). */
   248	static bool head_reserved(const je_ep_end *e)
   249	{
   250	    return e->waiter.present && e->waiter.head_reserved;
   251	}
   252	
   253	je_ep_status je_endpoint_recv_entry_probe(je_endpoint_pool *pool, je_handle_table *t,
   254	                                          je_handle h)
   255	{
   256	    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
   257	    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
   258	    if (rs != JE_EP_OK) return rs;
   259	    uint32_t rights = 0u;
   260	    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
   261	                                              JE_EP_RIGHT_RECV, &rights);
   262	    if (gr != JE_HANDLE_OK) return hstat(gr);
   263	    je_ep_end *e = &rec->end[side];
   264	    /* A head reserved by an EXISTING waiter is not available to this probe: it is
   265	     * delivered only through that waiter's continuation (A6-5). Treat it as empty
   266	     * for this caller. */
   267	    bool deliverable = (e->inbound.count > 0u) && !head_reserved(e);
   268	    if (deliverable) return JE_EP_OK;   /* caller proceeds to the real peek */
   269	    return (e->state == JE_EP_END_PEER_CLOSED) ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
   270	}
   271	
   272	je_ep_status je_endpoint_wait_register(je_endpoint_pool *pool, je_handle_table *t,
   273	                                       je_handle h, uint32_t waiter_id,
   274	                                       je_ep_channel **out_rec, uint8_t *out_side)
   275	{
   276	    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
   277	    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
   278	    if (rs != JE_EP_OK) return rs;
   279	    uint32_t rights = 0u;
   280	    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
   281	                                              JE_EP_RIGHT_RECV, &rights);
   282	    if (gr != JE_HANDLE_OK) return hstat(gr);
   283	    je_ep_end *e = &rec->end[side];
   284	    if (e->waiter.present) return JE_EP_WAITERS_BUSY;   /* A6-4 per-end bound */
   285	    /* A waiter holds one inflight ref on its own end (keeps the record from
   286	     * freeing while parked; released by wait_release). Bounds-checked. */
   287	    if (e->inflight_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
   288	    e->inflight_refs++;
   289	    e->waiter.present = true;
   290	    e->waiter.waiter_id = waiter_id;
   291	    e->waiter.cause = JE_EP_WAIT_WAITING;
   292	    e->waiter.head_reserved = false;
   293	    if (out_rec) *out_rec = rec;
   294	    if (out_side) *out_side = side;
   295	    return JE_EP_OK;
   296	}
   297	
   298	je_ep_arb_result je_endpoint_wait_arbitrate(je_ep_channel *rec, uint8_t side,
   299	                                            je_ep_wait_cause cause,
   300	                                            uint32_t *out_waiter_id)
   301	{
   302	    if (!rec || side > 1u) return JE_EP_ARB_LOST;
   303	    je_ep_end *e = &rec->end[side];
   304	    /* The single terminal CAS: only a present WAITING waiter transitions, once. */
   305	    if (!e->waiter.present || e->waiter.cause != JE_EP_WAIT_WAITING)
   306	        return JE_EP_ARB_LOST;
   307	    if (cause == JE_EP_WAIT_WAITING || cause == JE_EP_WAIT_NONE)
   308	        return JE_EP_ARB_LOST;   /* not a terminal cause */
   309	    e->waiter.cause = cause;
   310	    if (cause == JE_EP_WAIT_WOKEN) {
   311	        /* Reserve the current inbound head for this waiter (A6-5). There must be a
   312	         * message to reserve — the caller (enqueue / entry drain) only arbitrates
   313	         * WOKEN when one is queued. */
   314	        e->waiter.head_reserved = (e->inbound.count > 0u);
   315	    }
   316	    if (out_waiter_id) *out_waiter_id = e->waiter.waiter_id;
   317	    return JE_EP_ARB_WON;
   318	}
   319	
   320	je_ep_wait_cause je_endpoint_wait_cause(const je_ep_channel *rec, uint8_t side)
   321	{
   322	    if (!rec || side > 1u) return JE_EP_WAIT_NONE;
   323	    const je_ep_end *e = &rec->end[side];
   324	    return e->waiter.present ? e->waiter.cause : JE_EP_WAIT_NONE;
   325	}
   326	
   327	/* Internal: clear a present waiter on end e owned by `who`, releasing its inflight
   328	 * ref and un-reserving any held head. Idempotent via the ownership check. */
   329	static bool waiter_release_end(je_ep_end *e, uint32_t who)
   330	{
   331	    if (!e->waiter.present || e->waiter.waiter_id != who) return false;
   332	    e->waiter.present = false;
   333	    e->waiter.cause = JE_EP_WAIT_NONE;
   334	    e->waiter.head_reserved = false;   /* head stays enqueued, deliverable again */
   335	    e->waiter.waiter_id = 0u;
   336	    if (e->inflight_refs != 0u) e->inflight_refs--;   /* the waiter's own ref */
   337	    return true;
   338	}
   339	
   340	bool je_endpoint_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who)
   341	{
   342	    if (!rec || side > 1u) return false;
   343	    return waiter_release_end(&rec->end[side], who);
   344	}
   345	
   346	bool je_endpoint_wait_release_by_id(je_endpoint_pool *pool, uint32_t who)
   347	{
   348	    if (!pool) return false;
   349	    for (uint16_t i = 0; i < JE_EP_POOL_CAP; ++i) {
   350	        if (!pool->rec[i].allocated) continue;
   351	        for (uint8_t s = 0; s < 2u; ++s)
   352	            if (waiter_release_end(&pool->rec[i].end[s], who)) return true;
   353	    }
   354	    return false;
   355	}
   356	
   357	je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
   358	                                         je_ep_channel *rec, uint8_t side)
   359	{
   360	    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
   361	    /* Provenance: rec MUST be one of this pool's records (reject a wrong-pool or
   362	     * stray pointer, which would otherwise corrupt a different channel's count).
   363	     * NOTE: this does NOT defend against a stale pointer to a since-freed-and-
   364	     * reused record in the SAME pool — the ADR 0009 borrowed-pointer rule makes
   365	     * that a trusted-caller precondition, and the queue/wait PR that first holds
   366	     * such a pair MUST carry the record generation to detour reuse. */
   367	    if (!rec_in_pool(pool, rec) || !rec->allocated) return JE_EP_CORRUPT;
   368	    je_ep_end *e = &rec->end[side];
   369	    if (e->inflight_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
   370	    e->inflight_refs++;
   371	    return JE_EP_OK;
   372	}
   373	
   374	je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
   375	                                          je_ep_channel *rec, uint8_t side)
   376	{
   377	    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
   378	    if (!rec_in_pool(pool, rec) || !rec->allocated) return JE_EP_CORRUPT;
   379	    je_ep_end *e = &rec->end[side];
   380	    if (e->inflight_refs == 0u) return JE_EP_CORRUPT;   /* underflow guard */
   480	    release_handle(pool, rec, side);
   481	    return JE_EP_OK;
   482	}
   483	
   484	/* ---- dup seam (retain-before-commit; endpoints never use dup_self) -------- */
   485	
   486	je_ep_status je_endpoint_handle_dup(je_endpoint_pool *pool, je_handle_table *t,
   487	                                    je_handle src, uint32_t new_rights,
   488	                                    je_handle *out)
   489	{
   490	    if (out) *out = 0u;
   491	    if (!pool || !t || !out) return JE_EP_BADINDEX;
   492	
   493	    /* Resolve src as an endpoint and read its rights, preserving the table's
   494	     * BADINDEX -> STALE -> WRONGTYPE precedence. */
   495	    je_ep_channel *rec = (je_ep_channel *)0;
   496	    uint8_t side = 0u;
   497	    je_ep_status rs = je_endpoint_resolve(pool, t, src, &rec, &side);
   498	    if (rs != JE_EP_OK) return rs;
   499	
   500	    uint32_t src_rights = 0u;
   501	    je_handle_status gr = je_handle_rights_of(t, src, JE_ENDPOINT_OBJ_TYPE, 0u,
   502	                                              &src_rights);
   503	    if (gr != JE_HANDLE_OK) return hstat(gr);
   504	
   505	    /* Subset check: every requested bit must be held (equality allowed). */
   506	    if (new_rights & ~src_rights) return JE_EP_DENIED;
   507	
   508	    /* Acquire BEFORE commit (ADR 0009 D8). Overflow acquires nothing. */
   509	    je_ep_status ac = retain_handle(&rec->end[side]);
   510	    if (ac != JE_EP_OK) return ac;   /* JE_EP_REFOVERFLOW */
   511	
   512	    je_handle nh = 0u;
   513	    je_handle_status a = je_handle_alloc(t, (void *)&rec->end[side],
   514	                                         JE_ENDPOINT_OBJ_TYPE, new_rights, &nh);
   515	    if (a != JE_HANDLE_OK) {
   516	        /* Roll back the retain. src still holds a handle_ref, so this can never
   517	         * drop the end to 0 / trigger destruction. */
   518	        rec->end[side].handle_refs--;
   519	        return hstat(a);
   520	    }
   521	    *out = nh;
   522	    return JE_EP_OK;
   523	}
   524	
   525	/* ---- process-death release walk (A3-1) ----------------------------------- */
   526	
   527	int je_endpoint_release_table(je_endpoint_pool *pool, je_handle_table *t)
   528	{
   529	    if (!pool || !t) return -1;
   530	    int released = 0;
   531	    /* Reading t->capacity is table METADATA (public struct field), not slot
   532	     * internals; slot contents are reached only via the read-only iterator. */
   533	    for (uint32_t i = 0; i < t->capacity; ++i) {
   534	        je_handle h = 0u;
   535	        uint32_t type = 0u;
   536	        if (!je_handle_live_at(t, i, &h, &type)) continue;
   537	        if (type != JE_ENDPOINT_OBJ_TYPE) continue;   /* leave SELF etc. to the zero */
   538	        je_ep_status s = je_endpoint_handle_close(pool, t, h);
   539	        if (s != JE_EP_OK) return -1;   /* invariant break: caller must fail-stop */
   540	        released++;
   541	    }
   542	    /* Post-condition: no endpoint slot may remain LIVE. */
   543	    for (uint32_t i = 0; i < t->capacity; ++i) {
   544	        je_handle h = 0u; uint32_t type = 0u;
   545	        if (je_handle_live_at(t, i, &h, &type) && type == JE_ENDPOINT_OBJ_TYPE)
   546	            return -1;   /* a LIVE endpoint survived the walk -> fatal */
   547	    }
   548	    return released;
   549	}
   550	
   551	/* ---- non-blocking data IPC (M3c step 4, D6 / D9) ------------------------- */
   552	
   553	uint8_t je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side)
   554	{
   555	    if (!rec || side > 1u) return 0u;
   556	    return rec->end[side].inbound.count;
   557	}
   558	
   559	je_ep_status je_endpoint_send_preflight(je_endpoint_pool *pool, je_handle_table *t,
   560	                                        je_handle h, size_t len)
   561	{
   562	    je_ep_channel *rec = (je_ep_channel *)0;
   563	    uint8_t side = 0u;
   564	    if (len > JE_EP_MSG_MAX) return JE_EP_INVALID;
   565	
   566	    /* Resolve + SEND right (BADINDEX -> STALE -> WRONGTYPE -> DENIED). */
   567	    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
   568	    if (rs != JE_EP_OK) return rs;
   569	    uint32_t rights = 0u;
   570	    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
   571	                                              JE_EP_RIGHT_SEND, &rights);
   572	    if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if SEND not held */
   573	
   574	    /* A message delivered on end `side` lands in the PEER end's inbound. If THIS
   575	     * end is peer-closed, nobody will ever read it -> PEER_CLOSED. */
   576	    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
   577	        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
   578	    if (rec->end[side ^ 1u].inbound.count >= JE_EP_QUEUE_DEPTH)
   579	        return JE_EP_QUEUE_FULL;
   580	    return JE_EP_OK;
   620	    *out_len = 0u;
   621	
   622	    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
   623	    if (rs != JE_EP_OK) return rs;
   624	    uint32_t rights = 0u;
   625	    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
   626	                                              JE_EP_RIGHT_RECV, &rights);
   627	    if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if RECV not held */
   628	
   629	    je_ep_queue *q = &rec->end[side].inbound;    /* this end's own inbound */
   630	    /* A head reserved by a blocking waiter is delivered only through that waiter's
   631	     * continuation (A6-5); a concurrent handle-based poll must NOT take it. Treat
   632	     * it as empty for this caller. */
   633	    if (q->count == 0u || head_reserved(&rec->end[side])) {
   634	        /* Empty (or head reserved): EOF if the peer is gone, else would-block. */
   635	        return (rec->end[side].state == JE_EP_END_PEER_CLOSED && q->count == 0u)
   636	               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
   637	    }
   638	    uint8_t mlen = q->len[q->head];
   639	    *out_len = mlen;                 /* report length even on SHORT_BUFFER */
   640	    if (cap < mlen) return JE_EP_SHORT_BUFFER;   /* no copy, no dequeue */
   641	    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;   /* NULL dst guard */
   642	    for (uint8_t b = 0; b < mlen; ++b)
   643	        ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
   644	    if (lease) {
   645	        lease->rec = rec;
   646	        lease->side = side;
   647	        lease->generation = rec->generation;
   648	        lease->head = q->head;
   649	        lease->len = mlen;
   650	    }
   651	    return JE_EP_OK;
   652	}
   653	
   654	je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
   655	                                     const je_ep_recv_lease *lease)
   656	{
   657	    /* Delegate to the cap-aware commit (which also releases any queued-cap
   658	     * inflight refs and zeroes cap storage). A zero-cap message releases nothing;
   659	     * same behaviour as before, but the two paths cannot drift. */
   660	    return je_endpoint_recv_commit_caps(pool, lease);
   661	}
   662	
   663	/* ---- capability transfer (M3c step 5, ADR 0009 A5) ----------------------- */
   664	
   665	bool je_endpoint_capdesc_live(const je_endpoint_pool *pool, const je_ep_capdesc *d)
   666	{
   667	    if (!pool || !d || d->rec_index >= JE_EP_POOL_CAP || d->side > 1u) return false;
   668	    const je_ep_channel *tr = &pool->rec[d->rec_index];
   669	    if (!tr->allocated || tr->generation != d->generation) return false; /* stale */
   670	    je_ep_end_state st = tr->end[d->side].state;
   671	    /* OPEN and PEER_CLOSED are LIVE (mintable); only CLOSED/FREE are dead (A5-2). */
   672	    return st == JE_EP_END_OPEN || st == JE_EP_END_PEER_CLOSED;
   673	}
   674	
   675	je_ep_status je_endpoint_capdesc_resolve(je_endpoint_pool *pool,
   676	                                         const je_ep_capdesc *d,
   677	                                         je_ep_channel **out_rec, uint8_t *out_side)
   678	{
   679	    if (!pool || !d || !out_rec || !out_side) return JE_EP_CORRUPT;
   680	    if (!je_endpoint_capdesc_live(pool, d)) return JE_EP_STALE;
   681	    *out_rec = &pool->rec[d->rec_index];
   682	    *out_side = d->side;
   683	    return JE_EP_OK;
   684	}
   685	
   686	je_ep_status je_endpoint_capdesc_retain_handle(je_endpoint_pool *pool,
   687	                                               const je_ep_capdesc *d)
   688	{
   689	    je_ep_channel *tr; uint8_t ts;
   690	    je_ep_status s = je_endpoint_capdesc_resolve(pool, d, &tr, &ts);
   691	    if (s != JE_EP_OK) return s;
   692	    return retain_handle(&tr->end[ts]);   /* REFOVERFLOW if at the bound */
   693	}
   694	
   695	void je_endpoint_capdesc_release_handle(je_endpoint_pool *pool,
   696	                                        const je_ep_capdesc *d)
   697	{
   698	    je_ep_channel *tr; uint8_t ts;
   699	    if (je_endpoint_capdesc_resolve(pool, d, &tr, &ts) != JE_EP_OK) return;
   700	    /* Undo a mint-path retain: drop one handle_ref (may free if it was the last,
   701	     * but a mint rollback happens while the descriptor's inflight ref still holds
   702	     * the target, so this cannot reach 0). */
   703	    release_handle(pool, tr, ts);
   704	}
   705	
   706	/* Validate the batch of descriptors the glue resolved: count bound, each target
   707	 * live, each descriptor's rights a subset of the target's CURRENT rights. Pure. */
   708	static je_ep_status caps_validate(je_endpoint_pool *pool, const je_ep_capdesc *caps,
   709	                                  uint8_t ncaps)
   710	{
   711	    if (ncaps > JE_EP_MSG_CAPS_MAX) return JE_EP_INVALID;
   712	    for (uint8_t c = 0; c < ncaps; ++c) {
   713	        je_ep_channel *tr; uint8_t ts;
   714	        je_ep_status s = je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts);
   715	        if (s != JE_EP_OK) return s;               /* dead target at send -> STALE */
   716	        /* Descriptor rights must contain only known endpoint bits (defence in
   717	         * depth; the GLUE attenuates against the sender's HANDLE rights before
   718	         * building the descriptor — ends themselves carry no rights to check). */
   719	        if (caps[c].rights & ~(uint32_t)JE_EP_RIGHTS_ALL) return JE_EP_INVALID;
   720	        (void)tr; (void)ts;
   721	    }
   722	    return JE_EP_OK;
   723	}
   724	
   725	je_ep_status je_endpoint_send_preflight_caps(je_endpoint_pool *pool,
   726	                                             je_handle_table *t, je_handle h,
   727	                                             size_t len, const je_ep_capdesc *caps,
   728	                                             uint8_t ncaps)
   729	{
   730	    je_ep_status base = je_endpoint_send_preflight(pool, t, h, len);
   731	    if (base != JE_EP_OK) return base;
   732	    if (ncaps == 0u) return JE_EP_OK;
   733	    if (!caps) return JE_EP_INVALID;
   734	    return caps_validate(pool, caps, ncaps);
   735	}
   736	
   737	je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *t,
   738	                                       je_handle h, const void *kbuf, size_t len,
   739	                                       const je_ep_capdesc *caps, uint8_t ncaps)
   740	{
   741	    je_ep_channel *rec = (je_ep_channel *)0;
   742	    uint8_t side = 0u;
   743	    if (len != 0u && !kbuf) return JE_EP_INVALID;
   744	    /* Full preflight (byte + caps) — same validator, re-run so check+enqueue is
   745	     * atomic under IF=0. */
   746	    je_ep_status pf = je_endpoint_send_preflight_caps(pool, t, h, len, caps, ncaps);
   747	    if (pf != JE_EP_OK) return pf;
   748	    (void)je_endpoint_resolve(pool, t, h, &rec, &side);   /* cannot fail now */
   749	
   750	    /* RETAIN one inflight ref per cap on each target, rolling back all on any
   751	     * REFOVERFLOW (all-or-nothing). */
   752	    uint8_t retained = 0u;
   753	    for (uint8_t c = 0; c < ncaps; ++c) {
   754	        je_ep_channel *tr; uint8_t ts;
   755	        if (je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts) != JE_EP_OK) {
   756	            /* target died between preflight and here — impossible under IF=0;
   757	             * roll back and fail closed. */
   758	            for (uint8_t r = 0; r < retained; ++r) {
   759	                je_ep_channel *rr; uint8_t rs;
   760	                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
   761	                if (rr->end[rs].inflight_refs) rr->end[rs].inflight_refs--;
   762	            }
   763	            return JE_EP_STALE;
   764	        }
   765	        if (tr->end[ts].inflight_refs >= JE_EP_REF_MAX) {
   766	            for (uint8_t r = 0; r < retained; ++r) {
   767	                je_ep_channel *rr; uint8_t rs;
   768	                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
   769	                if (rr->end[rs].inflight_refs) rr->end[rs].inflight_refs--;
   770	            }
   771	            return JE_EP_REFOVERFLOW;
   772	        }
   773	        tr->end[ts].inflight_refs++;
   774	        retained++;
   775	    }
   776	
   777	    /* Enqueue the message (bytes + cap descriptors) at the peer inbound tail. */
   778	    je_ep_queue *q = &rec->end[side ^ 1u].inbound;
   779	    uint8_t tail = (uint8_t)((q->head + q->count) % JE_EP_QUEUE_DEPTH);
   780	    for (uint8_t b = 0; b < (uint8_t)len; ++b)
   781	        q->buf[tail][b] = ((const uint8_t *)kbuf)[b];
   782	    q->len[tail] = (uint8_t)len;
   783	    for (uint8_t c = 0; c < ncaps; ++c) q->cap[tail][c] = caps[c];
   784	    q->cap_count[tail] = ncaps;
   785	    q->count++;
   786	    return JE_EP_OK;
   787	}
   788	
   789	/* Core peek-with-caps on a resolved end (rec,side), no handle/rights/reserved-head
   790	 * checks — the callers apply those. Copies head bytes+caps into the out buffers or
   791	 * returns SHORT_BUFFER (nothing copied, not dequeued). */
   792	static je_ep_status peek_caps_at(je_ep_channel *rec, uint8_t side,
   793	                                 void *out_kbuf, size_t cap, size_t *out_len,
   794	                                 uint8_t hcap, je_ep_capdesc *out_caps,
   795	                                 uint8_t *out_ncaps, je_ep_recv_lease *lease)
   796	{
   797	    je_ep_queue *q = &rec->end[side].inbound;
   798	    if (q->count == 0u)
   799	        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
   800	               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
   801	    uint8_t mlen = q->len[q->head];
   802	    uint8_t mcaps = q->cap_count[q->head];
   803	    *out_len = mlen;
   804	    *out_ncaps = mcaps;
   805	    if (cap < mlen || hcap < mcaps) return JE_EP_SHORT_BUFFER;
   806	    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;
   807	    if (mcaps != 0u && !out_caps) return JE_EP_INVALID;
   808	    for (uint8_t b = 0; b < mlen; ++b) ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
   809	    for (uint8_t c = 0; c < mcaps; ++c) out_caps[c] = q->cap[q->head][c];
   810	    if (lease) {
   811	        lease->rec = rec; lease->side = side;
   812	        lease->generation = rec->generation; lease->head = q->head; lease->len = mlen;
   813	    }
   814	    return JE_EP_OK;
   815	}
   816	
   817	je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
   818	                                        je_handle h, void *out_kbuf, size_t cap,
   819	                                        size_t *out_len, uint8_t hcap,
   820	                                        je_ep_capdesc *out_caps, uint8_t *out_ncaps,
   821	                                        je_ep_recv_lease *lease)
   822	{
   823	    je_ep_channel *rec = (je_ep_channel *)0;
   824	    uint8_t side = 0u;
   825	    if (!out_len || !out_ncaps) return JE_EP_INVALID;
   826	    *out_len = 0u; *out_ncaps = 0u;
   827	
   828	    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
   829	    if (rs != JE_EP_OK) return rs;
   830	    uint32_t rights = 0u;
   831	    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
   832	                                              JE_EP_RIGHT_RECV, &rights);
   833	    if (gr != JE_HANDLE_OK) return hstat(gr);
   834	    /* A head reserved by a blocking waiter is not available to a handle-based
   835	     * peek (A6-5); treat as empty (EOF only if also peer-closed). */
   836	    if (head_reserved(&rec->end[side]) && rec->end[side].inbound.count > 0u)
   837	        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
   838	               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
   839	    return peek_caps_at(rec, side, out_kbuf, cap, out_len, hcap, out_caps, out_ncaps, lease);
   840	}
   841	
   842	/* Owned-end peek-with-caps for a resumed WOKEN waiter (A6-5): by rec/side, no
   843	 * handle resolution, bypasses the reserved-head guard (this IS the reserver).
   844	 * CORRUPT if there is no reserved head. */
   845	je_ep_status je_endpoint_recv_peek_owned_caps(je_endpoint_pool *pool, je_ep_channel *rec,
   846	                                              uint8_t side, void *out_kbuf, size_t cap,
   847	                                              size_t *out_len, uint8_t hcap,
   848	                                              je_ep_capdesc *out_caps, uint8_t *out_ncaps,
   849	                                              je_ep_recv_lease *lease)
   850	{
   851	    if (!out_len || !out_ncaps) return JE_EP_INVALID;
   852	    *out_len = 0u; *out_ncaps = 0u;
   853	    if (!pool || !rec || side > 1u || !rec_in_pool(pool, rec) || !rec->allocated)
   854	        return JE_EP_CORRUPT;
   855	    je_ep_end *e = &rec->end[side];
   856	    if (!e->waiter.present || !e->waiter.head_reserved || e->inbound.count == 0u)
   857	        return JE_EP_CORRUPT;
   858	    return peek_caps_at(rec, side, out_kbuf, cap, out_len, hcap, out_caps, out_ncaps, lease);
   859	}
   860	
   861	je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
   862	                                          const je_ep_recv_lease *lease)
   863	{
   864	    if (!pool || !lease || !lease->rec) return JE_EP_CORRUPT;
   865	    je_ep_channel *rec = lease->rec;
   866	    if (!rec_in_pool(pool, rec)) return JE_EP_CORRUPT;
   867	    if (!rec->allocated || rec->generation != lease->generation || lease->side > 1u)
   868	        return JE_EP_CORRUPT;
   869	    je_ep_queue *q = &rec->end[lease->side].inbound;
   870	    if (q->count == 0u || q->head != lease->head || q->len[q->head] != lease->len)
   871	        return JE_EP_CORRUPT;
   872	    /* Snapshot this head's caps BEFORE zeroing, then release each one's inflight
   873	     * ref (the cap has been delivered — minted or consumed-as-dead by the glue). */
   874	    uint8_t mcaps = q->cap_count[q->head];
   875	    je_ep_capdesc snap[JE_EP_MSG_CAPS_MAX];
   876	    for (uint8_t c = 0; c < mcaps; ++c) snap[c] = q->cap[q->head][c];
   877	    /* Dequeue: zero the vacated slot (bytes + caps), advance. */
   878	    q->len[q->head] = 0u;
   879	    q->cap_count[q->head] = 0u;
   880	    for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[q->head][b] = 0u;
   881	    for (uint8_t c = 0; c < JE_EP_MSG_CAPS_MAX; ++c) {
   882	        q->cap[q->head][c].rec_index = 0u; q->cap[q->head][c].side = 0u;
   883	        q->cap[q->head][c].cap_count_pad = 0u;
   884	        q->cap[q->head][c].generation = 0u; q->cap[q->head][c].rights = 0u;
   885	    }
   886	    q->head = (uint8_t)((q->head + 1u) % JE_EP_QUEUE_DEPTH);
   887	    q->count--;
   888	    /* Now release each delivered cap's inflight ref (may free the target). Done
   889	     * AFTER dequeue so the queue slot no longer references the target. */
   890	    for (uint8_t c = 0; c < mcaps; ++c) release_capdesc_inflight(pool, &snap[c]);
   891	    return JE_EP_OK;
   892	}

 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * Hostable unit tests for the M3c endpoint lifecycle core (ADR 0009 D-SEQ step 2).
     4	 * No kernel state: a local handle table over local slots + a local endpoint pool.
     5	 * Proves destruction-on-last-ref, the split refcount, REFOVERFLOW, and dup
     6	 * acquire-before-commit rollback — all without any ring-3 path (create is
     7	 * kernel-only, exercised directly here).
     8	 */
     9	#include <jeios/endpoint.h>
    10	#include <assert.h>
    11	#include <stdio.h>
    12	
    13	#define CAP 16u
    14	#define QUOTA 15u
    15	
    16	static je_handle_slot g_slots[CAP];
    17	static je_handle_table g_t;
    18	static je_endpoint_pool g_pool;
    19	
    20	static void fresh(uint32_t quota)
    21	{
    22	    assert(je_handle_table_init(&g_t, g_slots, CAP, quota));
    23	    je_endpoint_pool_init(&g_pool);
    24	}
    25	
    26	/* Create one channel with both ends in the same table, all rights. */
    27	static void make_pair(je_handle *h0, je_handle *h1)
    28	{
    29	    je_ep_status s = je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
    30	                                                &g_t, JE_EP_RIGHTS_ALL, h0, h1);
    31	    assert(s == JE_EP_OK && *h0 != 0u && *h1 != 0u && *h0 != *h1);
    32	}
    33	
    34	int main(void)
    35	{
    36	    /* 1. Create: both ends OPEN, refs 1/1, one record allocated. */
    37	    fresh(QUOTA);
    38	    je_handle h0 = 0u, h1 = 0u;
    39	    make_pair(&h0, &h1);
    40	    assert(je_endpoint_pool_allocated(&g_pool) == 1u);
    41	    {
    42	        je_ep_channel *rec; uint8_t side;
    43	        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
    44	        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_OPEN);
    45	        assert(je_endpoint_end_state(rec, 1) == JE_EP_END_OPEN);
    46	        assert(rec->end[0].handle_refs == 1u && rec->end[1].handle_refs == 1u);
    47	        assert(rec->end[side].side == side);
    48	    }
    49	
    50	    /* 2. Close end 0 -> end0 CLOSED, peer PEER_CLOSED, record still alive.
    51	     *    Close end 1 -> record freed. */
    52	    {
    53	        je_ep_channel *rec; uint8_t side;
    54	        assert(je_endpoint_resolve(&g_pool, &g_t, h1, &rec, &side) == JE_EP_OK);
    55	        uint32_t gen_before = rec->generation;
    56	
    57	        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_OK);
    58	        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_CLOSED);
    59	        assert(je_endpoint_end_state(rec, 1) == JE_EP_END_PEER_CLOSED);
    60	        assert(rec->allocated);
    61	        assert(je_endpoint_pool_allocated(&g_pool) == 1u);
    62	
    63	        assert(je_endpoint_handle_close(&g_pool, &g_t, h1) == JE_EP_OK);
    64	        assert(!rec->allocated);
    65	        assert(rec->generation == gen_before + 1u);     /* bumped on free */
    66	        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
    67	        /* The closed handles are now stale. */
    68	        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_STALE);
    69	    }
    70	
    71	    /* 3. Dup: original and dup are independent handle_refs on the same end.
    72	     *    Closing the original leaves the end OPEN; closing the dup closes it. */
    73	    fresh(QUOTA);
    74	    make_pair(&h0, &h1);
    75	    {
    76	        je_handle dup = 0u;
    77	        assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHTS_ALL, &dup)
    78	               == JE_EP_OK && dup != 0u && dup != h0);
    79	        je_ep_channel *rec; uint8_t side;
    80	        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
    81	        assert(rec->end[0].handle_refs == 2u);
    82	
    83	        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_OK);
    84	        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_OPEN);  /* dup remains */
    85	        assert(rec->end[0].handle_refs == 1u);
    86	
    87	        assert(je_endpoint_handle_close(&g_pool, &g_t, dup) == JE_EP_OK);
    88	        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_CLOSED);
    89	        assert(je_endpoint_end_state(rec, 1) == JE_EP_END_PEER_CLOSED);
    90	        assert(je_endpoint_handle_close(&g_pool, &g_t, h1) == JE_EP_OK);
    91	        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
    92	    }
    93	
    94	    /* 4. REFOVERFLOW: dup end 0 up to JE_EP_REF_MAX handle_refs, then one more
    95	     *    fails with nothing acquired, table live-count and *out unchanged. */
    96	    fresh(QUOTA);
    97	    make_pair(&h0, &h1);   /* end0 starts at 1 handle_ref */
    98	    {
    99	        je_ep_channel *rec; uint8_t side;
   100	        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
   101	        je_handle dups[JE_EP_REF_MAX];
   102	        /* bring handle_refs from 1 up to JE_EP_REF_MAX */
   103	        uint32_t need = JE_EP_REF_MAX - 1u;
   104	        for (uint32_t i = 0; i < need; ++i) {
   105	            assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHT_INSPECT,
   106	                                          &dups[i]) == JE_EP_OK);
   107	        }
   108	        assert(rec->end[0].handle_refs == JE_EP_REF_MAX);
   109	        uint32_t live_before = g_t.live;
   110	        je_handle over = 123u;   /* sentinel, must be cleared to 0 */
   111	        assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHT_INSPECT,
   112	                                      &over) == JE_EP_REFOVERFLOW);
   113	        assert(over == 0u);
   114	        assert(rec->end[0].handle_refs == JE_EP_REF_MAX);   /* unchanged */
   115	        assert(g_t.live == live_before);                    /* no slot taken */
   116	    }
   117	
   118	    /* 5. Dup DENIED on a superset: a RECV-only handle cannot dup to SEND. */
   119	    fresh(QUOTA);
   120	    {
   121	        je_handle r0 = 0u, r1 = 0u, bad = 7u;
   122	        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHT_RECV,
   123	                                          &g_t, JE_EP_RIGHTS_ALL, &r0, &r1)
   124	               == JE_EP_OK);
   125	        assert(je_endpoint_handle_dup(&g_pool, &g_t, r0, JE_EP_RIGHT_SEND, &bad)
   126	               == JE_EP_DENIED);
   127	        assert(bad == 0u);
   128	        /* equal/subset dup is allowed */
   129	        je_handle ok = 0u;
   130	        assert(je_endpoint_handle_dup(&g_pool, &g_t, r0, JE_EP_RIGHT_RECV, &ok)
   131	               == JE_EP_OK && ok != 0u);
   132	    }
   133	
   134	    /* 6. Dup rollback on QUOTA: fill the table to quota, then a dup must release
   135	     *    its pre-acquired ref (end handle_refs returns to its prior value). */
   136	    fresh(2u);   /* quota 2: the create pair already uses both live slots */
   137	    make_pair(&h0, &h1);
   138	    {
   139	        je_ep_channel *rec; uint8_t side;
   140	        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
   141	        assert(g_t.live == 2u);
   142	        uint16_t refs_before = rec->end[0].handle_refs;
   143	        je_handle q = 9u;
   144	        assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHT_INSPECT, &q)
   145	               == JE_EP_QUOTA);
   146	        assert(q == 0u);
   147	        assert(rec->end[0].handle_refs == refs_before);  /* retain rolled back */
   148	    }
   149	
   150	    /* 7. WRONGTYPE: a SELF handle is not an endpoint; and an endpoint handle
   151	     *    must be rejected by the SELF-only table dup path. */
   152	    fresh(QUOTA);
   153	    {
   154	        je_handle self = 0u;
   155	        assert(je_handle_alloc(&g_t, (void *)0, JE_HANDLE_OBJ_SELF,
   156	                               JE_EP_RIGHT_INSPECT, &self) == JE_HANDLE_OK);
   157	        je_handle x = 5u;
   158	        assert(je_endpoint_handle_dup(&g_pool, &g_t, self, JE_EP_RIGHT_INSPECT, &x)
   159	               == JE_EP_WRONGTYPE);
   160	        assert(x == 0u);
   161	        je_ep_channel *rec; uint8_t side;
   162	        assert(je_endpoint_resolve(&g_pool, &g_t, self, &rec, &side)
   163	               == JE_EP_WRONGTYPE);
   164	
   165	        make_pair(&h0, &h1);
   166	        /* dup_self on an endpoint handle: the table's SELF-only gate rejects it. */
   167	        je_handle y = 6u;
   168	        assert(je_handle_dup_self(&g_t, h0, JE_HANDLE_OBJ_SELF,
   169	                                  JE_EP_RIGHT_INSPECT, &y) == JE_HANDLE_WRONGTYPE);
   170	        assert(y == 0u);
   171	    }
   172	
   173	    /* 8. inflight_refs keep the record alive after both handles close; releasing
   174	     *    the last inflight ref frees it. */
   175	    fresh(QUOTA);
   176	    make_pair(&h0, &h1);
   177	    {
   178	        je_ep_channel *rec; uint8_t side;
   179	        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
   180	        assert(je_endpoint_retain_inflight(&g_pool, rec, 0) == JE_EP_OK);
   181	        assert(rec->end[0].inflight_refs == 1u);
   182	
   183	        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_OK);
   184	        assert(je_endpoint_handle_close(&g_pool, &g_t, h1) == JE_EP_OK);
   185	        /* both handle_refs are 0, but an inflight ref remains -> NOT freed */
   186	        assert(rec->allocated);
   187	        assert(je_endpoint_pool_allocated(&g_pool) == 1u);
   188	
   189	        assert(je_endpoint_release_inflight(&g_pool, rec, 0) == JE_EP_OK);
   190	        assert(!rec->allocated);
   191	        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
   192	        /* underflow guard: releasing again fails closed */
   193	        assert(je_endpoint_release_inflight(&g_pool, rec, 0) == JE_EP_CORRUPT);
   194	    }
   195	
   196	    /* 9. Pool exhaustion + create unwind: fill the pool, next create NOSLOTS and
   197	     *    leaves t0 unchanged; also a create whose SECOND mint fails unwinds the
   198	     *    first mint and the record completely. */
   199	    fresh(CAP);   /* generous table quota so the pool is the limiter */
   200	    {
   201	        je_handle a0[JE_EP_POOL_CAP], a1[JE_EP_POOL_CAP];
   202	        for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) make_pair(&a0[i], &a1[i]);
   203	        assert(je_endpoint_pool_allocated(&g_pool) == JE_EP_POOL_CAP);
   204	        je_handle x0 = 1u, x1 = 1u;
   205	        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
   206	                                          &g_t, JE_EP_RIGHTS_ALL, &x0, &x1)
   207	               == JE_EP_NOSLOTS);
   208	        assert(x0 == 0u && x1 == 0u);
   209	    }
   210	    /* second-mint failure unwind: t1 quota 0 so end-1 alloc fails with QUOTA.
   211	     * The record and end-0's mint must FULLY unwind — assert the record itself is
   212	     * back to FREE (not just the allocated_count, which only ever counts
   213	     * successes and so cannot catch a leaked half-built record), and that a fresh
   214	     * create then reuses rec[0] cleanly. */
   215	    fresh(CAP);
   216	    {
   217	        je_handle_slot t1_slots[4];
   218	        je_handle_table t1;
   219	        assert(je_handle_table_init(&t1, t1_slots, 4u, 0u));  /* quota 0 */
   220	        uint32_t live0_before = g_t.live;
   221	        je_handle x0 = 2u, x1 = 2u;
   222	        je_ep_status s = je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
   223	                                                    &t1, JE_EP_RIGHTS_ALL,
   224	                                                    &x0, &x1);
   225	        assert(s == JE_EP_QUOTA);
   226	        assert(x0 == 0u && x1 == 0u);
   227	        assert(g_t.live == live0_before);                 /* end-0 mint undone */
   228	        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
   229	        /* The real leak check: rec[0] must be genuinely FREE (flag AND states AND
   230	         * both handle_refs), so a half-built record cannot survive invisibly. */
   231	        assert(!g_pool.rec[0].allocated);
   232	        assert(g_pool.rec[0].end[0].state == JE_EP_END_FREE);
   233	        assert(g_pool.rec[0].end[1].state == JE_EP_END_FREE);
   234	        assert(g_pool.rec[0].end[0].handle_refs == 0u);
   235	        assert(g_pool.rec[0].end[1].handle_refs == 0u);
   236	        /* And the slot is cleanly reusable by a following successful create. */
   237	        je_handle y0 = 0u, y1 = 0u;
   238	        make_pair(&y0, &y1);
   239	        assert(je_endpoint_pool_allocated(&g_pool) == 1u);
   240	    }
   241	
   242	    /* 9b. Aliased create outputs are rejected (out0 == out1 would clobber h0 and
   243	     * orphan end-0's LIVE slot). Nothing is allocated. */
   244	    fresh(CAP);
   245	    {
   246	        je_handle same = 3u;
   247	        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
   248	                                          &g_t, JE_EP_RIGHTS_ALL, &same, &same)
   249	               == JE_EP_BADINDEX);
   250	        assert(same == 0u);
   251	        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
   252	    }
   253	
   254	    /* 10. BADINDEX / corrupt guards: handle 0 and a wide handle resolve BADINDEX. */
   255	    fresh(QUOTA);
   256	    {
   257	        je_ep_channel *rec; uint8_t side;
   258	        assert(je_endpoint_resolve(&g_pool, &g_t, 0u, &rec, &side) == JE_EP_BADINDEX);
   259	        assert(je_endpoint_handle_close(&g_pool, &g_t, 0u) == JE_EP_BADINDEX);
   260	    }
   261	
   262	    /* 11. Release walk (je_endpoint_release_table): simulates process death. A
   263	     * table with endpoint handles is walked; every endpoint handle is released,
   264	     * no endpoint slot remains LIVE, and records with no surviving ref are freed.
   265	     * A SELF handle in the same table is left untouched (only the zero reclaims
   266	     * it, as today). */
   267	    fresh(QUOTA);
   268	    {
   269	        /* Two channels fully owned by this one table (both ends here), plus a
   270	         * dup, plus a SELF handle that must survive the walk. */
   271	        je_handle a0=0,a1=0,b0=0,b1=0,dup=0,self=0;
   272	        make_pair(&a0,&a1);
   273	        make_pair(&b0,&b1);
   274	        assert(je_endpoint_handle_dup(&g_pool,&g_t,a0,JE_EP_RIGHTS_ALL,&dup)==JE_EP_OK);
   275	        assert(je_handle_alloc(&g_t,(void*)0,JE_HANDLE_OBJ_SELF,
   276	                               JE_EP_RIGHT_INSPECT,&self)==JE_HANDLE_OK);
   277	        assert(je_endpoint_pool_allocated(&g_pool)==2u);
   278	        uint32_t live_before = g_t.live;   /* 5 endpoint handles + 1 self = 6 */
   279	
   280	        int released = je_endpoint_release_table(&g_pool, &g_t);
   281	        assert(released == 5);                      /* a0,a1,b0,b1,dup */
   282	        assert(je_endpoint_pool_allocated(&g_pool)==0u);  /* both records freed */
   283	        /* No endpoint slot LIVE; the SELF slot still LIVE. */
   284	        je_ep_channel *rec; uint8_t side;
   285	        assert(je_endpoint_resolve(&g_pool,&g_t,a0,&rec,&side)==JE_EP_STALE);
   286	        void *obj=(void*)1;
   287	        assert(je_handle_lookup(&g_t,self,JE_HANDLE_OBJ_SELF,0u,&obj)==JE_HANDLE_OK);
   288	        assert(g_t.live == live_before - 5u);       /* only endpoints dropped */
   289	
   290	        /* Idempotent: a second walk releases nothing and still succeeds. */
   291	        assert(je_endpoint_release_table(&g_pool, &g_t) == 0);
   292	    }
   293	
   294	    /* 12. Release walk, same channel's two ends both in the dying table but one
   295	     * via a dup: last release frees the record, no mid-walk UAF. */
   296	    fresh(QUOTA);
   297	    {
   298	        je_handle c0=0,c1=0,c0dup=0;
   299	        make_pair(&c0,&c1);
   300	        assert(je_endpoint_handle_dup(&g_pool,&g_t,c0,JE_EP_RIGHTS_ALL,&c0dup)==JE_EP_OK);
   301	        /* end0 has 2 handle_refs (c0,c0dup), end1 has 1 (c1). */
   302	        assert(je_endpoint_release_table(&g_pool,&g_t) == 3);
   303	        assert(je_endpoint_pool_allocated(&g_pool)==0u);
   304	    }
   305	
   306	    /* 13. Data IPC: send/recv roundtrip + FIFO, via the pure peek/commit seam.
   307	     * Helper to do a full recv (peek into a local buf, then commit). */
   308	    fresh(QUOTA);
   309	    make_pair(&h0, &h1);
   310	    {
   311	        /* send on h0 -> lands in end1 inbound (peer). recv on h1 reads it. */
   312	        uint8_t msg[8] = {10,11,12,13,14,15,16,17};
   313	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,msg,8)==JE_EP_OK);
   314	        je_ep_channel *rec; uint8_t side;
   315	        assert(je_endpoint_resolve(&g_pool,&g_t,h1,&rec,&side)==JE_EP_OK);
   316	        assert(je_endpoint_inbound_count(rec,side)==1u);
   317	        /* recv on h0 is EMPTY (direction: nothing landed in end0 inbound). */
   318	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
   319	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h0,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
   320	        /* recv on h1 delivers the message. */
   321	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   322	        assert(rl==8u);
   323	        for (int i=0;i<8;i++) assert(rb[i]==msg[i]);
   324	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   325	        assert(je_endpoint_inbound_count(rec,side)==0u);
   326	        /* now empty -> EMPTY. */
   327	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
   328	    }
   329	
   330	    /* 14. FIFO order + QUEUE_FULL at DEPTH + no-refcount-interaction. */
   331	    fresh(QUOTA);
   332	    make_pair(&h0,&h1);
   333	    {
   334	        je_ep_channel *rec; uint8_t side;
   335	        assert(je_endpoint_resolve(&g_pool,&g_t,h0,&rec,&side)==JE_EP_OK);
   336	        uint16_t hr0=rec->end[0].handle_refs, hr1=rec->end[1].handle_refs;
   337	        for (uint8_t i=0;i<JE_EP_QUEUE_DEPTH;i++) {
   338	            uint8_t m[1]={ (uint8_t)(100+i) };
   339	            assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,1)==JE_EP_OK);
   340	        }
   341	        /* queue full now -> QUEUE_FULL, nothing enqueued. */
   342	        uint8_t mf[1]={9};
   343	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,mf,1)==JE_EP_QUEUE_FULL);
   344	        /* data did NOT touch refcounts. */
   345	        assert(rec->end[0].handle_refs==hr0 && rec->end[1].handle_refs==hr1);
   346	        assert(rec->end[0].inflight_refs==0u && rec->end[1].inflight_refs==0u);
   347	        /* drain in FIFO order. */
   348	        for (uint8_t i=0;i<JE_EP_QUEUE_DEPTH;i++) {
   349	            uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
   350	            assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   351	            assert(rl==1u && rb[0]==(uint8_t)(100+i));
   352	            assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   353	        }
   354	    }
   355	
   356	    /* 15. SHORT_BUFFER: peek reports length, copies nothing, does NOT dequeue. */
   357	    fresh(QUOTA);
   358	    make_pair(&h0,&h1);
   359	    {
   360	        uint8_t m[5]={1,2,3,4,5};
   361	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,5)==JE_EP_OK);
   362	        uint8_t rb[2]={0xAA,0xBB}; size_t rl=0; je_ep_recv_lease lz;
   363	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,2,&rl,&lz)==JE_EP_SHORT_BUFFER);
   364	        assert(rl==5u);                 /* required length reported */
   365	        assert(rb[0]==0xAA && rb[1]==0xBB);  /* nothing copied */
   366	        /* still queued: a big-enough recv delivers it. */
   367	        uint8_t rb2[JE_EP_MSG_MAX];
   368	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb2,sizeof rb2,&rl,&lz)==JE_EP_OK);
   369	        assert(rl==5u);
   370	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   371	    }
   372	
   373	    /* 16. PEER_CLOSED: send to a peer-closed end; drain-then-EOF on recv. */
   374	    fresh(QUOTA);
   375	    make_pair(&h0,&h1);
   376	    {
   377	        uint8_t m[3]={7,8,9};
   378	        /* h0 sends one, then h0 closes: end1 (h1's end) goes PEER_CLOSED but its
   379	         * inbound still holds the message. */
   380	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,3)==JE_EP_OK);
   381	        assert(je_endpoint_handle_close(&g_pool,&g_t,h0)==JE_EP_OK);
   382	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
   383	        /* drain first (message delivered despite peer gone). */
   384	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   385	        assert(rl==3u && rb[0]==7);
   386	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   387	        /* now empty + peer gone -> PEER_CLOSED (EOF). */
   388	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_PEER_CLOSED);
   389	        /* send on h1 -> its peer (end0) is CLOSED -> PEER_CLOSED. */
   390	        assert(je_endpoint_send_preflight(&g_pool,&g_t,h1,3)==JE_EP_PEER_CLOSED);
   391	    }
   392	
   393	    /* 17. Rights: send needs SEND, recv needs RECV (DENIED otherwise). */
   394	    fresh(QUOTA);
   395	    {
   396	        /* end0 has SEND only; end1 has RECV only. */
   397	        je_handle s0=0,r1=0;
   398	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHT_SEND,
   399	                                          &g_t,JE_EP_RIGHT_RECV,&s0,&r1)==JE_EP_OK);
   400	        uint8_t m[1]={1};
   401	        /* s0 can send; r1 cannot send (no SEND) -> DENIED. */
   402	        assert(je_endpoint_send_preflight(&g_pool,&g_t,s0,1)==JE_EP_OK);
   403	        assert(je_endpoint_send_preflight(&g_pool,&g_t,r1,1)==JE_EP_DENIED);
   404	        assert(je_endpoint_try_send(&g_pool,&g_t,s0,m,1)==JE_EP_OK);
   405	        /* r1 can recv; s0 cannot recv (no RECV) -> DENIED. */
   406	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
   407	        assert(je_endpoint_recv_peek(&g_pool,&g_t,s0,rb,sizeof rb,&rl,&lz)==JE_EP_DENIED);
   408	        assert(je_endpoint_recv_peek(&g_pool,&g_t,r1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   409	    }
   410	
   411	    /* 18. Zero-length message is valid and distinct from EMPTY. */
   412	    fresh(QUOTA);
   413	    make_pair(&h0,&h1);
   414	    {
   415	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,(void*)0,0)==JE_EP_OK);
   416	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=7; je_ep_recv_lease lz;
   417	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   418	        assert(rl==0u);                 /* OK + len 0, NOT EMPTY */
   419	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   420	    }
   421	
   422	    /* 19. len > MSG_MAX rejected; free-with-nonempty-queue returns to baseline. */
   423	    fresh(QUOTA);
   424	    make_pair(&h0,&h1);
   425	    {
   426	        uint8_t big[JE_EP_MSG_MAX+1];
   427	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,big,JE_EP_MSG_MAX+1)==JE_EP_INVALID);
   428	        /* queue one, then close BOTH ends while a message is queued: record must
   429	         * still free (data is not an owning ref). */
   430	        uint8_t m[2]={5,6};
   431	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,2)==JE_EP_OK);
   432	        assert(je_endpoint_handle_close(&g_pool,&g_t,h0)==JE_EP_OK);
   433	        assert(je_endpoint_handle_close(&g_pool,&g_t,h1)==JE_EP_OK);
   434	        assert(je_endpoint_pool_allocated(&g_pool)==0u);  /* freed despite queued msg */
   435	    }
   436	
   437	    /* 20. commit lease rejected after the record is freed/reused (generation). */
   438	    fresh(QUOTA);
   439	    make_pair(&h0,&h1);
   440	    {
   441	        uint8_t m[1]={1};
   442	        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,1)==JE_EP_OK);
   443	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
   444	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   445	        /* Tear down the whole channel before committing -> stale lease. */
   446	        assert(je_endpoint_handle_close(&g_pool,&g_t,h0)==JE_EP_OK);
   447	        assert(je_endpoint_handle_close(&g_pool,&g_t,h1)==JE_EP_OK);
   448	        assert(je_endpoint_pool_allocated(&g_pool)==0u);
   449	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_CORRUPT);  /* fail closed */
   450	    }
   451	
   452	    /* 21. Ring WRAPAROUND with distinct payloads + lengths (catches a wrong
   453	     * head/tail modulo that identical-payload tests would miss). Partially
   454	     * drain then refill so head advances past 0 and tail wraps. */
   455	    fresh(QUOTA);
   456	    make_pair(&h0,&h1);
   457	    {
   458	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
   459	        /* send 3 distinct (len i+1, bytes 0x10+i). */
   460	        for (uint8_t i=0;i<3;i++) {
   461	            uint8_t m[3]={(uint8_t)(0x10+i),(uint8_t)(0x20+i),(uint8_t)(0x30+i)};
   462	            assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,(size_t)(i+1))==JE_EP_OK);
   463	        }
   464	        /* drain 2 (head -> 2). */
   465	        for (uint8_t i=0;i<2;i++) {
   466	            assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   467	            assert(rl==(size_t)(i+1) && rb[0]==(uint8_t)(0x10+i));
   468	            assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   469	        }
   470	        /* send 2 more (distinct, len 4) -> tail wraps past DEPTH. */
   471	        for (uint8_t i=3;i<5;i++) {
   472	            uint8_t m[4]={(uint8_t)(0x10+i),(uint8_t)(0x20+i),(uint8_t)(0x30+i),(uint8_t)(0x40+i)};
   473	            assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,4)==JE_EP_OK);
   474	        }
   475	        /* drain remaining in FIFO: msg2 (len3), msg3 (len4), msg4 (len4). */
   476	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   477	        assert(rl==3u && rb[0]==0x12);
   478	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   479	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   480	        assert(rl==4u && rb[0]==0x13);
   481	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   482	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
   483	        assert(rl==4u && rb[0]==0x14);
   484	        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
   485	        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
   486	    }
   487	
   488	    /* 22. Capability transfer (pure layer, A5): build a descriptor for one
   489	     * channel's end and send it as a cap over a SECOND channel; the receiver
   490	     * side mints it. Also: inflight accounting, dead-cap (A5-2), cycle reclaim. */
   491	    fresh(QUOTA);
   492	    {
   493	        /* seed channel S (s0,s1) and payload channel C (c0,c1), all in g_t. */
   494	        je_handle s0=0,s1=0,c0=0,c1=0;
   495	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&s0,&s1)==JE_EP_OK);
   496	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
   497	        assert(je_endpoint_pool_allocated(&g_pool)==2u);
   498	        /* Build a cap descriptor for c1 (resolve it, snapshot rec/side/gen). */
   499	        je_ep_channel *crec; uint8_t cside;
   500	        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&crec,&cside)==JE_EP_OK);
   501	        je_ep_capdesc d = { (uint16_t)(crec - &g_pool.rec[0]), cside, 0u,
   502	                            crec->generation, JE_EP_RIGHTS_ALL };
   503	        uint16_t c1_inflight_before = crec->end[cside].inflight_refs;
   504	        /* send the cap over s0 (lands in s1's inbound); retains inflight on c1. */
   505	        uint8_t msg[2]={0xAB,0xCD};
   506	        assert(je_endpoint_try_send_caps(&g_pool,&g_t,s0,msg,2,&d,1)==JE_EP_OK);
   507	        assert(crec->end[cside].inflight_refs==c1_inflight_before+1u); /* retained */
   508	        /* recv on s1: peek reports 1 cap, mint it as a fresh handle. */
   509	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
   510	        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
   511	        assert(rl==2u && nc==1u && rb[0]==0xAB);
   512	        assert(je_endpoint_capdesc_live(&g_pool,&rc[0]));
   513	        /* mint: retain target handle_ref + alloc a receiver handle. */
   514	        je_ep_channel *mr; uint8_t ms;
   515	        assert(je_endpoint_capdesc_resolve(&g_pool,&rc[0],&mr,&ms)==JE_EP_OK);
   516	        uint16_t c1_hr_before = mr->end[ms].handle_refs;
   517	        assert(je_endpoint_capdesc_retain_handle(&g_pool,&rc[0])==JE_EP_OK);
   518	        je_handle minted=0;
   519	        assert(je_handle_alloc(&g_t,(void*)&mr->end[ms],JE_ENDPOINT_OBJ_TYPE,rc[0].rights,&minted)==JE_HANDLE_OK);
   520	        assert(minted!=0 && minted!=c1);
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/* Hosted unit tests for the M3a #5 pure syscall decoder. */
     3	#include <jeios/syscall.h>
     4	#include <assert.h>
     5	#include <stdio.h>
     6	
     7	static je_syscall_action dec(uint64_t op, uint64_t a0, uint64_t a1, uint64_t a2,
     8	                             uint64_t a3, uint64_t a4, uint64_t a5,
     9	                             je_abi_status *st, uint64_t *res)
    10	{
    11	    uint64_t args[6] = { a0,a1,a2,a3,a4,a5 };
    12	    return je_syscall_decode(op, args, st, res);
    13	}
    14	
    15	int main(void)
    16	{
    17	    je_abi_status st; uint64_t res;
    18	
    19	    /* abi_query(version) -> OK, RDX = version. */
    20	    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_VERSION,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN);
    21	    assert(st==JE_ABI_OK && res==JE_ABI_VERSION);
    22	    /* abi_query(profile) -> OK, RDX = profile. */
    23	    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_PROFILE,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN);
    24	    assert(st==JE_ABI_OK && res==JE_ABI_PROFILE);
    25	    /* abi_query with an unknown selector -> INVALID, result 0. */
    26	    assert(dec(JE_SYS_ABI_QUERY, 99,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    27	    /* abi_query with a nonzero reserved arg -> INVALID, result 0 (no RDX leak). */
    28	    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_VERSION, 1,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    29	    assert(dec(JE_SYS_ABI_QUERY, JE_ABI_QUERY_VERSION, 0,0,0,0,7,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID && res==0);
    30	
    31	    /* yield() with clean args -> YIELD/OK. */
    32	    assert(dec(JE_SYS_YIELD, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_YIELD && st==JE_ABI_OK && res==0);
    33	    /* yield() with any nonzero arg -> INVALID, and NOT a yield action. */
    34	    assert(dec(JE_SYS_YIELD, 1,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    35	
    36	    /* exit(code) -> EXIT, result carries the code. */
    37	    assert(dec(JE_SYS_EXIT, 42,0,0,0,0,0,&st,&res)==JE_SYSCALL_EXIT && st==JE_ABI_OK && res==42);
    38	    /* exit with a nonzero reserved arg -> INVALID, and must NOT exit. */
    39	    assert(dec(JE_SYS_EXIT, 42, 1,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    40	
    41	    /* copy_probe (op 3): src/dst/len in RDI/RSI/RDX; R10/R8/R9 reserved. The
    42	     * pure decoder returns the COPY_PROBE action (the glue does the real copy),
    43	     * and rejects nonzero reserved args as INVALID without a copy action. */
    44	    assert(dec(JE_SYS_COPY_PROBE, 0x40000000,0x40000040,16,0,0,0,&st,&res)==JE_SYSCALL_COPY_PROBE && st==JE_ABI_OK);
    45	    assert(dec(JE_SYS_COPY_PROBE, 0,0,0, 1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* r10 reserved */
    46	    assert(dec(JE_SYS_COPY_PROBE, 0,0,0, 0,0,9,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* r9 reserved */
    47	
    48	    /* Unknown op -> UNSUPPORTED regardless of args, result 0. */
    49	    assert(dec(0xdeadbeef, 1,2,3,4,5,6,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    50	    /* Full 64-bit op decode: 0x1_0000_0000 must NOT alias op 0 (abi_query). */
    51	    assert(dec(UINT64_C(0x100000000), 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED && res==0);
    52	    /* op 4 is the dev-only copy_fixup_inject, guarded out of this hosted build:
    53	     * UNSUPPORTED here, no success. */
    54	    assert(dec(4, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);
    55	
    56	    /* M3b handle ops (5/6/7). The pure decoder validates reserved args and the
    57	     * wide-handle rule; the glue does the table work. */
    58	    /* handle_self(): clean -> SELF action, OK. */
    59	    assert(dec(JE_SYS_HANDLE_SELF, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_SELF && st==JE_ABI_OK);
    60	    /* handle_self() with ANY nonzero arg (it takes none) -> INVALID, not an action. */
    61	    assert(dec(JE_SYS_HANDLE_SELF, 1,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    62	    assert(dec(JE_SYS_HANDLE_SELF, 0,0,0,0,0,9,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    63	    /* handle_close(h=RDI): clean -> CLOSE action, OK. */
    64	    assert(dec(JE_SYS_HANDLE_CLOSE, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_CLOSE && st==JE_ABI_OK);
    65	    /* handle_close with a reserved arg -> INVALID. */
    66	    assert(dec(JE_SYS_HANDLE_CLOSE, 0x00010000,1,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    67	    /* handle_close with a WIDE handle (bits 63:32 set) -> INVALID before narrowing. */
    68	    assert(dec(JE_SYS_HANDLE_CLOSE, UINT64_C(0x100000000),0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    69	    /* handle_query(h=RDI): clean -> QUERY action; reserved + wide-handle rejected. */
    70	    assert(dec(JE_SYS_HANDLE_QUERY, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_QUERY && st==JE_ABI_OK);
    71	    assert(dec(JE_SYS_HANDLE_QUERY, 0x00010000,0,1,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    72	    assert(dec(JE_SYS_HANDLE_QUERY, UINT64_C(0x500000000),0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    73	    /* handle_rights(h=RDI): clean -> RIGHTS action; reserved + wide-handle rejected. */
    74	    assert(dec(JE_SYS_HANDLE_RIGHTS, 0x00010000,0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_RIGHTS && st==JE_ABI_OK);
    75	    assert(dec(JE_SYS_HANDLE_RIGHTS, 0x00010000,0,0,1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    76	    assert(dec(JE_SYS_HANDLE_RIGHTS, UINT64_C(0x100000000),0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    77	    /* handle_dup(h=RDI, new_rights=RSI): clean subset -> DUP action, OK. */
    78	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_INSPECT,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    79	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK); /* drop all rights */
    80	    /* reserved RDX/R10/R8/R9 nonzero -> INVALID */
    81	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0, 1,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    82	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0, 0,1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    83	    /* wide handle or wide rights -> INVALID */
    84	    assert(dec(JE_SYS_HANDLE_DUP, UINT64_C(0x100000000), 0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    85	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, UINT64_C(0x100000000),0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
    86	    /* Rights-bit decode contract. HISTORY (M3b): while 0x2 was an UNKNOWN bit,
    87	     * dup(self, 0x2) returned INVALID (malformed). M3c (ADR 0009 D4a) appends
    88	     * SEND=0x2 to JE_ABI_RIGHTS_KNOWN, so 0x2 is now a KNOWN right a SELF handle
    89	     * does not hold -> the decode passes and the glue returns DENIED. We assert
    90	     * the new DENIED result for 0x2, and re-prove the unknown-bit->INVALID
    91	     * contract on a bit that is still unknown (0x80000000). See
    92	     * docs/IMPLEMENTATION.md "M3c rights-bit discrepancy" for the rationale. */
    93	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, JE_ABI_RIGHT_SEND,0,0,0,0,&st,&res)==JE_SYSCALL_HANDLE_DUP && st==JE_ABI_OK);
    94	    /* ^ 0x2 is now a well-formed (known) rights request: decode accepts it and
    95	     * returns the DUP action; the known-but-unheld -> DENIED decision happens in
    96	     * the glue (handle_dup_self subset check), not the decoder. The old
    97	     * dup(self,0x2)==INVALID assertion is intentionally retired; its unknown-bit
    98	     * INTENT is preserved by the next line on a genuinely-unknown bit. */
    99	    assert(dec(JE_SYS_HANDLE_DUP, 0x00010000, 0x80000000u,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
   100	    /* endpoint_create (op 10): no args -> ENDPOINT_CREATE action, OK; any
   101	     * reserved arg -> INVALID. (Dispatch/mint is kernel-only; decode is pure.) */
   102	    assert(dec(JE_SYS_ENDPOINT_CREATE, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_ENDPOINT_CREATE && st==JE_ABI_OK);
   103	    assert(dec(JE_SYS_ENDPOINT_CREATE, 1,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
   104	    assert(dec(JE_SYS_ENDPOINT_CREATE, 0,0,0,0,0,9,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID);
   105	    /* try_send (op 11): h,ubuf,len -> TRY_SEND; cap_count in R10[7:0] with R8 the
   106	     * cap-array ptr (R8!=0 iff cap_count>0). Reserved R10[63:8]/R9, len>MSG_MAX,
   107	     * cap_count>K, R8/cap_count pairing mismatch, wide handle -> INVALID. */
   108	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0,0,0,&st,&res)==JE_SYSCALL_TRY_SEND && st==JE_ABI_OK);
   109	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 1,0x40003000,0,&st,&res)==JE_SYSCALL_TRY_SEND && st==JE_ABI_OK); /* 1 cap + array ptr */
   110	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 1,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* cap_count=1 but R8=0 */
   111	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0,0x40003000,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R8!=0 but cap_count=0 */
   112	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 3,0x40003000,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* cap_count > K=2 */
   113	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0x100,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R10 reserved bit */
   114	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 8, 0,0,1,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R9 reserved */
   115	    assert(dec(JE_SYS_TRY_SEND, 0x00010000, 0x40002000, 65, 0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* len > MSG_MAX */
   116	    assert(dec(JE_SYS_TRY_SEND, UINT64_C(0x100000000), 0x40002000, 8, 0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* wide handle */
   117	    /* try_recv (op 12): h,ubuf,cap,hbuf=R8,hcap=R9,timeout=R10; R8!=0 iff R9!=0. */
   118	    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0,0,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK);
   119	    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0x40003000,2,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK); /* handle buf + cap */
   120	    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0x40003000,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R8!=0 R9=0 */
   121	    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 0,0,2,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* R8=0 R9!=0 */
   122	    /* A6-ABI: R10 is now the timeout (0=poll, nonzero=block). Any value is a legal
   123	     * decode (the bound/overflow check is in the glue, not the decode). */
   124	    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, 1,0,0,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK); /* nonzero timeout OK */
   125	    assert(dec(JE_SYS_TRY_RECV, 0x00010000, 0x40002000, 64, UINT64_C(0xffffffffffffffff),0,0,&st,&res)==JE_SYSCALL_TRY_RECV && st==JE_ABI_OK); /* huge timeout decodes; glue rejects */
   126	    assert(dec(JE_SYS_TRY_RECV, UINT64_C(0x100000000), 0x40002000, 64, 0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_INVALID); /* wide handle */
   127	    /* op 13 and up unknown. */
   128	    assert(dec(13, 0,0,0,0,0,0,&st,&res)==JE_SYSCALL_RETURN && st==JE_ABI_UNSUPPORTED);
   129	
   130	    printf("syscall: PASS (abi_query, yield, exit, copy_probe, handle self/close/query/rights/dup, "
   131	           "endpoint_create, try_send/try_recv decode, "
   132	           "reserved-arg, wide-handle, unknown-rights, full-width op, unsupported)\n");
   133	    return 0;
   134	}

 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * M3a #5 step (a): the experimental v0 syscall dispatcher reached from the DPL3
     4	 * int 0x80 gate (ADR 0007 D8/D10). Interrupts are disabled on this short path
     5	 * (the gate is an interrupt gate). Bounded and nonblocking.
     6	 *
     7	 * Split in two: je_syscall_decode() is a PURE function (op + six args ->
     8	 * action/status/result) with no kernel state, so it is unit-tested on the host;
     9	 * je_syscall_dispatch() is the thin kernel glue that reads the trap frame, calls
    10	 * decode, writes the result registers, and routes to the scheduler (resume the
    11	 * caller, yield, or exit). No user pointers are touched in step (a) — copy ops
    12	 * are step (b).
    13	 */
    14	#include <jeios/abi.h>
    15	#include <jeios/syscall.h>
    16	#include <jeios/sched.h>
    17	#include <jeios/trap.h>
    18	#include <jeios/usercopy.h>
    19	#ifndef JE_SYSCALL_HOSTED_TEST
    20	#include <jeios/vm.h>
    21	#include <jeios/handle.h>
    22	#include <jeios/endpoint.h>
    23	#include <jeios/endpoint_kern.h>
    24	#include <jeios/arch.h>
    25	#endif
    26	
    27	/* Pure decode: given the operation and its six argument registers, decide the
    28	 * action, the ABI status to return in RAX, and any scalar result for RDX. No
    29	 * side effects; safe to unit-test. Reserved-argument rules are per-op and are
    30	 * checked only for recognized ops (an unknown op is UNSUPPORTED regardless of
    31	 * arguments). exit(code) carries the code in *out_result for the glue to use. */
    32	je_syscall_action je_syscall_decode(uint64_t op, const uint64_t args[6],
    33	                                    je_abi_status *out_status, uint64_t *out_result)
    34	{
    35	    uint64_t rdi=args[0], rsi=args[1], rdx=args[2], r10=args[3], r8=args[4], r9=args[5];
    36	    *out_result = 0;
    37	    switch (op) {
    38	    case JE_SYS_ABI_QUERY:
    39	        /* which = RDI (0 version, 1 profile); RSI..R9 reserved must be zero. */
    40	        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    41	        if (rdi==JE_ABI_QUERY_VERSION) { *out_result=JE_ABI_VERSION; *out_status=JE_ABI_OK; return JE_SYSCALL_RETURN; }
    42	        if (rdi==JE_ABI_QUERY_PROFILE) { *out_result=JE_ABI_PROFILE; *out_status=JE_ABI_OK; return JE_SYSCALL_RETURN; }
    43	        *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
    44	    case JE_SYS_YIELD:
    45	        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    46	        *out_status=JE_ABI_OK; return JE_SYSCALL_YIELD;
    47	    case JE_SYS_EXIT:
    48	        /* code = RDI; RSI..R9 reserved must be zero. */
    49	        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    50	        *out_result=rdi; *out_status=JE_ABI_OK; return JE_SYSCALL_EXIT;
    51	    case JE_SYS_COPY_PROBE:
    52	        /* dev-only copy_probe(src=RDI, dst=RSI, len=RDX); R10/R8/R9 reserved=0.
    53	         * The actual copy (with prevalidation + fixup) is performed by the glue;
    54	         * the pure decoder only rejects nonzero reserved args here. */
    55	        if (r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    56	        *out_status=JE_ABI_OK; return JE_SYSCALL_COPY_PROBE;
    57	    case JE_SYS_HANDLE_SELF:
    58	        /* handle_self(): no arguments; RDI..R9 reserved must be zero. The glue
    59	         * mints a SELF handle in the current process's table. */
    60	        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    61	        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_SELF;
    62	    case JE_SYS_HANDLE_CLOSE:
    63	        /* handle_close(h=RDI): RSI..R9 reserved must be zero. A handle value with
    64	         * bits 63:32 set is rejected here as INVALID before it is ever narrowed
    65	         * to the 32-bit je_handle (decode-INVALID precedes BADINDEX/STALE). */
    66	        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    67	        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    68	        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_CLOSE;
    69	    case JE_SYS_HANDLE_QUERY:
    70	        /* handle_query(h=RDI): RSI..R9 reserved must be zero; same wide-handle
    71	         * rejection as close. The glue returns the object type in RDX. */
    72	        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    73	        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    74	        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_QUERY;
    75	    case JE_SYS_HANDLE_RIGHTS:
    76	        /* handle_rights(h=RDI): RSI..R9 reserved must be zero; wide handle
    77	         * rejected before narrowing. The glue looks the handle up requiring the
    78	         * INSPECT right and returns the masked rights mask in RDX. */
    79	        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    80	        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    81	        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_RIGHTS;
    82	    case JE_SYS_HANDLE_DUP:
    83	        /* handle_dup(h=RDI, new_rights=RSI): RDX/R10/R8/R9 reserved must be zero;
    84	         * both the handle (RDI) and the rights (RSI) must fit 32 bits; and the
    85	         * rights must contain only KNOWN bits (an unknown bit is a malformed call
    86	         * -> INVALID, distinct from asking for a known-but-unheld right which the
    87	         * glue reports as DENIED). The glue does the resolve + subset + allocate. */
    88	        if (rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    89	        if ((rdi >> 32) || (rsi >> 32)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    90	        if (rsi & ~(uint64_t)JE_ABI_RIGHTS_KNOWN) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    91	        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_DUP;
    92	    case JE_SYS_ENDPOINT_CREATE:
    93	        /* endpoint_create(): takes NO args; every register reserved. Mints both
    94	         * ends into the caller's table (dispatch side). All-zero args or INVALID. */
    95	        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
    96	        *out_status=JE_ABI_OK; return JE_SYSCALL_ENDPOINT_CREATE;
    97	    case JE_SYS_TRY_SEND:
    98	        /* try_send(h=RDI, ubuf=RSI, len=RDX, meta=R10, cap_array=R8, R9).
    99	         * R10[7:0] = cap_count (0..JE_ABI_MSG_CAPS_MAX); R10[63:8] reserved = 0.
   100	         * R8 = user pointer to a u64[cap_count] of {low32 handle, high32 rights}
   101	         * (0 iff cap_count==0). R9 reserved = 0. len <= JE_ABI_MSG_MAX. (A5-4) */
   102	        if (r10 >> 8) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; } /* reserved bits */
   103	        if (r9)       { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   104	        if ((r10 & 0xffu) > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   105	        /* R8 must be 0 iff cap_count==0 (and nonzero iff cap_count>0). */
   106	        if (((r10 & 0xffu) == 0u) != (r8 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   107	        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   108	        if (rdx > (uint64_t)JE_ABI_MSG_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   109	        *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_SEND;
   110	    case JE_SYS_TRY_RECV:
   111	        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8, hcap=R9, timeout=R10).
   112	         * R8 = user u32[] to receive minted handles; R9 = its slot capacity
   113	         * (0..JE_ABI_MSG_CAPS_MAX; a larger value is rejected so it can't be
   114	         * truncated to a smaller capacity in the glue). R8 must be 0 iff R9==0.
   115	         * R10 = A6-ABI relative tick timeout: 0 = non-blocking poll (today's
   116	         * behavior), nonzero = block up to JE_IPC_MAX_WAIT (bound+overflow checked
   117	         * in the glue). Any R10 value is a legal arg now (no reserved-0 rule). */
   118	        if (r9 > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   119	        if ((r8 == 0u) != (r9 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   120	        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   121	        *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_RECV;
   122	#ifdef JE_DEV_COPY_INJECT
   123	    case JE_SYS_COPY_FIXUP_INJECT:
   124	        /* dev-only: uaddr=RDI (an unmapped arena page); glue raw-copies to prove
   125	         * the fixup recovers. Guarded out of the default build entirely. */
   126	        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
   127	        *out_status=JE_ABI_OK; return JE_SYSCALL_COPY_FIXUP_INJECT;
   128	#endif
   129	    default:
   130	        /* Unknown op: UNSUPPORTED regardless of arguments (full 64-bit op). */
   131	        *out_status=JE_ABI_UNSUPPORTED; return JE_SYSCALL_RETURN;
   132	    }
   133	}
   134	
   135	#ifndef JE_SYSCALL_HOSTED_TEST
   136	/* M3b: SELF is the only object kind so far — a kernel-minted, immortal,
   137	 * resource-free process-identity capability (ADR 0008). It is granted the
   138	 * INSPECT right so handle_rights has a right to check; the mask is a kernel
   139	 * constant asserted to contain only known bits (no unknown bit can be minted). */
   140	#define JE_HANDLE_TYPE_SELF   JE_ABI_OBJ_SELF
   141	#define JE_HANDLE_RIGHTS_SELF JE_ABI_RIGHT_INSPECT
   142	_Static_assert((JE_HANDLE_RIGHTS_SELF & ~JE_ABI_RIGHTS_KNOWN) == 0u,
   143	               "SELF must be minted only with known rights bits");
   144	/* The table's SELF-only dup gate uses JE_HANDLE_OBJ_SELF; it MUST equal the ABI
   145	 * SELF type the glue mints/passes, or dup_self would reject legitimate SELF
   146	 * handles (or the gate would be bypassable). Pin them together here. */
   147	_Static_assert(JE_HANDLE_OBJ_SELF == JE_ABI_OBJ_SELF, "SELF type: table vs ABI");
   148	
   149	/* Exhaustive, single-point mapping of internal JE_HANDLE_* results to the v0
   150	 * ABI status namespace (ADR 0008 D2: distinct, not collapsed). Keeping it here
   151	 * in the glue is what lets abi.h stay independent of handle.h. */
   152	static je_abi_status handle_status_to_abi(je_handle_status s)
   153	{
   154	    switch (s) {
   155	    case JE_HANDLE_OK:        return JE_ABI_OK;
   156	    case JE_HANDLE_STALE:     return JE_ABI_H_STALE;
   157	    case JE_HANDLE_BADINDEX:  return JE_ABI_H_BADINDEX;
   158	    case JE_HANDLE_WRONGTYPE: return JE_ABI_H_WRONGTYPE;
   159	    case JE_HANDLE_DENIED:    return JE_ABI_H_DENIED;
   160	    case JE_HANDLE_QUOTA:     return JE_ABI_H_QUOTA;
   161	    case JE_HANDLE_NOSLOTS:   return JE_ABI_H_NOSLOTS;
   162	    }
   163	    return JE_ABI_INVALID;   /* unreachable; fail closed if the enum ever grows */
   164	}
   165	
   166	/* Resolve the CURRENT user process's handle table. Identity comes ONLY from the
   167	 * scheduler (je_sched_current_user), never from a syscall argument. Returns NULL
   168	 * if the caller is not a user thread (the glue maps that to JE_ABI_INVALID). */
   169	static je_handle_table *current_htable(void)
   170	{
   171	    je_vm *vm; uint32_t index;
   172	    if (je_sched_current_user(&vm, &index) != JE_OK) return (je_handle_table *)0;
   173	    return &vm->procs[index].htable;
   174	}
   175	
   176	/* Resolve the current user process as (vm,index) for the endpoint ops, which
   177	 * need the vm (teardown-hook check + pool) not just the table. Returns false for
   178	 * a non-user caller. */
   179	static bool current_vm_index(je_vm **out_vm, uint32_t *out_index)
   180	{
   200	static je_abi_status ep_status_to_abi(je_ep_status s)
   201	{
   202	    switch (s) {
   203	    case JE_EP_OK:          return JE_ABI_OK;
   204	    case JE_EP_BADINDEX:    return JE_ABI_H_BADINDEX;
   205	    case JE_EP_STALE:       return JE_ABI_H_STALE;
   206	    case JE_EP_WRONGTYPE:   return JE_ABI_H_WRONGTYPE;
   207	    case JE_EP_DENIED:      return JE_ABI_H_DENIED;
   208	    case JE_EP_QUOTA:       return JE_ABI_H_QUOTA;
   209	    case JE_EP_NOSLOTS:     return JE_ABI_H_NOSLOTS;
   210	    case JE_EP_REFOVERFLOW: return JE_ABI_H_REFOVERFLOW;
   211	    case JE_EP_QUEUE_FULL:  return JE_ABI_H_QUEUE_FULL;
   212	    case JE_EP_SHORT_BUFFER: return JE_ABI_H_SHORT_BUFFER;
   213	    case JE_EP_PEER_CLOSED: return JE_ABI_H_PEER_CLOSED;
   214	    case JE_EP_EMPTY:       return JE_ABI_H_EMPTY;
   215	    case JE_EP_WAITERS_BUSY: return JE_ABI_H_EMPTY; /* A6-4: occupied slot -> non-blocking EMPTY */
   216	    case JE_EP_INVALID:     return JE_ABI_INVALID;
   217	    case JE_EP_CORRUPT:     return JE_ABI_UNSUPPORTED; /* unwired vm / invariant */
   218	    }
   219	    return JE_ABI_INVALID;
   220	}
   221	
   222	/* M3c A6: finish a resumed blocking recv. The thread parked on (wrec,wside) with
   223	 * out-args ubuf=rsi, cap=rdx, hbuf=r8, hcap=r9. Read the terminal cause the single
   224	 * arbitrate CAS stamped and deliver into f. WOKEN: peek the reserved head (bytes +
   225	 * caps) from the OWNED end (no handle re-resolve, A6-5), stage mints, copy out,
   226	 * commit, release the waiter. TIMED_OUT/PEER_CLOSED/CANCELLED: write the status.
   227	 * A copy/mint fault on WOKEN -> JE_ABI_INVALID, message stays queued, mints rolled
   228	 * back, reservation released (the head returns to deliverable). */
   229	static void je_syscall_finish_blocked_recv(je_trap_frame *f, je_ep_channel *rec,
   230	                                           uint8_t side, uint64_t ubuf, uint64_t cap,
   231	                                           uint64_t hbuf, uint64_t hcap)
   232	{
   233	    je_vm *vm; uint32_t index;
   234	    if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return; }
   235	    je_ep_wait_cause cause = je_endpoint_wait_cause(rec, side);
   236	    uint32_t slot = 0u; (void)je_sched_current_slot(&slot);
   237	    switch (cause) {
   238	    case JE_EP_WAIT_TIMED_OUT:
   239	        je_endpoint_kern_wait_release(rec, side, slot);
   240	        f->rax=(uint64_t)JE_ABI_H_TIMED_OUT; f->rdx=0; return;
   241	    case JE_EP_WAIT_PEER_CLOSED:
   242	        je_endpoint_kern_wait_release(rec, side, slot);
   243	        f->rax=(uint64_t)JE_ABI_H_PEER_CLOSED; f->rdx=0; return;
   244	    case JE_EP_WAIT_CANCELLED:
   245	        je_endpoint_kern_wait_release(rec, side, slot);
   246	        f->rax=(uint64_t)JE_ABI_H_CANCELLED; f->rdx=0; return;
   247	    case JE_EP_WAIT_WOKEN: break;    /* deliver below */
   248	    default:
   249	        /* No terminal cause on a resumed thread = kernel-invariant break. */
   250	        je_arch_console_write("JeiOS fatal: recv resume without terminal cause\r\n");
   251	        je_arch_halt(); return;
   252	    }
   253	    /* WOKEN delivery from the owned reserved head. */
   254	    uint8_t staging[JE_EP_MSG_MAX];
   255	    je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
   256	    size_t mlen = 0u; uint8_t mcaps = 0u;
   257	    je_ep_recv_lease lease;
   258	    je_ep_status s = je_endpoint_kern_recv_peek_owned(rec, side, staging, (size_t)cap,
   259	                                                      &mlen, (uint8_t)hcap, caps, &mcaps,
   260	                                                      &lease);
   261	    if (s == JE_EP_SHORT_BUFFER) {
   262	        /* Buffer too small: leave queued+reserved, report required sizes. The
   263	         * waiter stays so a re-issued recv with a bigger buffer delivers it. */
   264	        f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
   265	        f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32); return;
   266	    }
   267	    if (s != JE_EP_OK) { je_endpoint_kern_wait_release(rec, side, slot);
   268	                         f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return; }
   269	    /* Stage mints (dead cap -> handle 0; roll back all on transient fault). */
   270	    je_handle minted[JE_ABI_MSG_CAPS_MAX]; bool was_live[JE_ABI_MSG_CAPS_MAX];
   271	    for (uint8_t c = 0; c < mcaps; ++c) { minted[c]=0u; was_live[c]=false; }
   272	    je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
   273	    for (uint8_t c = 0; c < mcaps; ++c) {
   274	        if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c]=0u; continue; }
   275	        je_handle nh = 0u;
   276	        je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
   277	        if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
   278	        minted[c]=nh; was_live[c]=true; done=(uint8_t)(c+1u);
   279	    }
   280	    if (fail != JE_ABI_OK) {
   281	        for (uint8_t c = 0; c < done; ++c)
   282	            if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
   283	        /* Message stays queued+reserved; A6-5 says a fault does NOT consume. But a
   284	         * transient mint fault must not wedge the waiter forever: release it so a
   285	         * re-issue can retry (the head returns to deliverable). */
   286	        je_endpoint_kern_wait_release(rec, side, slot);
   287	        f->rax=(uint64_t)fail; f->rdx=0; return;
   288	    }
   289	    if (mlen != 0u) {
   290	        je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
   291	        if (cs != JE_ABI_OK) {
   292	            for (uint8_t c = 0; c < mcaps; ++c)
   293	                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
   294	            je_endpoint_kern_wait_release(rec, side, slot);
   295	            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
   296	        }
   297	    }
   298	    if (mcaps != 0u) {
   299	        uint32_t hout[JE_ABI_MSG_CAPS_MAX];
   300	        for (uint8_t c = 0; c < mcaps; ++c) hout[c]=(uint32_t)minted[c];
   301	        je_abi_status cs = je_copy_to_user(hbuf, hout, (uint64_t)mcaps*sizeof(uint32_t));
   302	        if (cs != JE_ABI_OK) {
   303	            for (uint8_t c = 0; c < mcaps; ++c)
   304	                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
   305	            je_endpoint_kern_wait_release(rec, side, slot);
   306	            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
   307	        }
   308	    }
   309	    /* Infallible commit (dequeue + release inflight), then free the waiter. */
   310	    je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
   311	    if (cm != JE_EP_OK) { je_arch_console_write("JeiOS fatal: blocked recv commit mismatch\r\n"); je_arch_halt(); }
   312	    je_endpoint_kern_wait_release(rec, side, slot);
   313	    f->rax=(uint64_t)JE_ABI_OK;
   314	    f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32);
   315	}
   316	
   317	/* Kernel glue. Returns the frame to iretq to (the caller for RETURN/invalid, a
   318	 * live successor for YIELD/EXIT). Must only be called for a user-origin int 0x80
   319	 * frame (je_trap_dispatch enforces cs&3==3; the scheduler helpers re-check the
   320	 * frame identity). */
   321	je_trap_frame *je_syscall_dispatch(je_trap_frame *f)
   322	{
   323	    /* M3c A6-CLARIFY-1: pending-continuation prologue. If the CURRENT thread
   324	     * resumed from a blocking-recv park, FINISH that recv here — keyed on the
   325	     * waiter state, NOT on RAX — before any op decode. The thread re-executed its
   326	     * int 0x80, so RAX/args are intact, but we complete from the SAVED owned
   327	     * rec/side + args (never re-resolving the handle, A6-5). The terminal cause
   328	     * stamped by the single arbitrate CAS decides the result. */
   329	    {
   330	        je_ep_channel *wrec = (je_ep_channel *)0; uint8_t wside = 0u;
   331	        uint64_t wrdi, wrsi, wrdx, wr8, wr9, wprip;
   332	        if (je_sched_wait_pending(&wrec, &wside, &wrdi, &wrsi, &wrdx, &wr8, &wr9, &wprip)) {
   333	            /* The restart must have landed exactly on the saved post-int RIP; any
   334	             * mismatch is a kernel-invariant break, not a user error. */
   335	            if (f->rip != wprip) { je_arch_console_write("JeiOS fatal: recv continuation RIP\r\n"); je_arch_halt(); }
   336	            je_syscall_finish_blocked_recv(f, wrec, wside, wrsi, wrdx, wr8, wr9);
   337	            je_sched_wait_finish();
   338	            return je_sched_return_to_user(f);
   339	        }
   340	    }
   341	    uint64_t args[6] = { f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9 };
   342	    je_abi_status status; uint64_t result;
   343	    je_syscall_action action = je_syscall_decode(f->rax, args, &status, &result);
   344	
   345	    switch (action) {
   346	    case JE_SYSCALL_YIELD:
   347	        /* Write the OK result into the CALLER's saved frame BEFORE switching, so
   348	         * when this task is next resumed its RAX already reads OK. Reuse the
   349	         * existing kind-aware reschedule path (same as the 0x81 kernel yield);
   350	         * no recursion, no M2 sleep (D10). */
   351	        f->rax = (uint64_t)status; f->rdx = result;
   352	        return je_sched_interrupt(f, false);
   353	    case JE_SYSCALL_EXIT:
   354	        return je_sched_exit_current(f, result);
   355	    case JE_SYSCALL_COPY_PROBE: {
   356	        /* dev-only: stage <=256 user bytes src->kernel->dst with prevalidation
   357	         * and the exception-fixup net. src=RDI, dst=RSI, len=RDX. */
   358	        je_abi_status s = je_copy_probe(f->rdi, f->rsi, f->rdx);
   359	        f->rax = (uint64_t)s; f->rdx = 0;
   360	        return je_sched_return_to_user(f);
   361	    }
   362	    case JE_SYSCALL_HANDLE_SELF: {
   363	        /* Mint a SELF handle in the caller's own table. SELF carries no object
   364	         * pointer; it is granted the INSPECT right (JE_HANDLE_RIGHTS_SELF). A
   365	         * freshly minted SELF holds INSPECT, so DENIED is not observable on it
   366	         * directly — but handle_dup can derive a rights-reduced handle on which
   367	         * handle_rights then returns DENIED (ring-3-observable). Handle in RDX. */
   368	        je_handle_table *t = current_htable();
   369	        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
   370	        je_handle h = 0;
   371	        je_handle_status s = je_handle_alloc(t, (void *)0, JE_HANDLE_TYPE_SELF,
   372	                                             JE_HANDLE_RIGHTS_SELF, &h);
   373	        f->rax = (uint64_t)handle_status_to_abi(s);
   374	        f->rdx = (uint64_t)h;                 /* 0 on failure (alloc clears out) */
   375	        return je_sched_return_to_user(f);
   376	    }
   377	    case JE_SYSCALL_HANDLE_CLOSE: {
   378	        /* Typed close dispatch (A3-3): peek the slot's object type, then branch
   379	         * EXACTLY — SELF -> raw table close; ENDPOINT -> endpoint-aware close
   380	         * (drops the handle_ref); any other/unknown type -> fail closed. The peek
   470	        f->rax = (uint64_t)ep_status_to_abi(s);
   471	        f->rdx = (s == JE_EP_OK)
   472	               ? ((uint64_t)h0 | ((uint64_t)h1 << 32)) : 0u;
   473	        return je_sched_return_to_user(f);
   474	    }
   475	    case JE_SYSCALL_TRY_SEND: {
   476	        /* A5-4 send: resolve caps (if any) in the sender's table FIRST (read-only
   477	         * -> TRANSFER + attenuation), preflight bytes+caps, copy bytes to stack
   478	         * staging, then pure all-or-nothing enqueue (bytes + cap descriptors +
   479	         * per-cap inflight retain). Cap-array copy fault ranks last (after a clean
   480	         * preflight), per the frozen precedence. */
   481	        je_vm *vm; uint32_t index;
   482	        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
   483	        je_handle h = (je_handle)f->rdi;
   484	        uint64_t ubuf = f->rsi, len = f->rdx;
   485	        uint8_t ncaps = (uint8_t)(f->r10 & 0xffu);
   486	        uint64_t caparr = f->r8;
   487	        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
   488	        /* Resolve each user cap entry {low32 handle, high32 rights} -> descriptor.
   489	         * Copy the array in first (a fault here is INVALID but only after the main
   490	         * handle is validated by the preflight below — so do the base preflight
   491	         * (len + main handle SEND/state/full) FIRST). */
   492	        je_ep_status bpf = je_endpoint_kern_send_preflight(vm, index, h, (size_t)len);
   493	        if (bpf != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(bpf); f->rdx=0; return je_sched_return_to_user(f); }
   494	        if (ncaps != 0u) {
   495	            uint64_t raw[JE_ABI_MSG_CAPS_MAX];
   496	            je_abi_status cs = je_copy_from_user(raw, caparr,
   497	                                                 (uint64_t)ncaps * sizeof(uint64_t));
   498	            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
   499	            for (uint8_t c = 0; c < ncaps; ++c) {
   500	                je_handle ch = (je_handle)(raw[c] & 0xffffffffu);
   501	                uint32_t crights = (uint32_t)(raw[c] >> 32);
   502	                if (crights & ~(uint32_t)JE_ABI_RIGHTS_KNOWN) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
   503	                je_ep_status rc = je_endpoint_kern_resolve_send_cap(vm, index, ch, crights, &caps[c]);
   504	                if (rc != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(rc); f->rdx=0; return je_sched_return_to_user(f); }
   505	            }
   506	        }
   507	        uint8_t staging[JE_EP_MSG_MAX];
   508	        if (len != 0u) {
   509	            je_abi_status cs = je_copy_from_user(staging, ubuf, len);
   510	            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
   511	        }
   512	        je_ep_status s = je_endpoint_kern_send_caps(vm, index, h, staging,
   513	                                                    (size_t)len, caps, ncaps);
   514	        f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
   515	        return je_sched_return_to_user(f);
   516	    }
   517	    case JE_SYSCALL_TRY_RECV: {
   518	        /* A5-4 recv: PEEK (bytes + cap descriptors, no dequeue) -> stage all live
   519	         * mints in the receiver's table (dead caps -> handle 0, A5-2) -> copy bytes
   520	         * out -> copy handle array out -> INFALLIBLE commit (dequeue + release each
   521	         * cap's inflight). Any transient fault rolls back every staged mint and
   522	         * leaves the message queued. */
   523	        je_vm *vm; uint32_t index;
   524	        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
   525	        je_handle h = (je_handle)f->rdi;
   526	        uint64_t ubuf = f->rsi, cap = f->rdx, hbuf = f->r8, hcap = f->r9;
   527	        uint64_t timeout = f->r10;   /* A6-ABI: relative tick duration; 0 = poll */
   528	        /* A6-ABI entry snapshot: evaluate the end once via the plain try_recv path
   529	         * BEFORE registering any waiter. If it is EMPTY (would block) and a nonzero
   530	         * timeout was given, register a per-end waiter and PARK (constrained
   531	         * restartable syscall); on resume the continuation prologue delivers. A
   532	         * zero timeout, or a non-EMPTY probe, falls through to the normal recv. */
   533	        if (timeout != 0u) {
   534	            je_ep_status probe = je_endpoint_kern_recv_entry_probe(vm, index, h);
   535	            if (probe == JE_EP_EMPTY) {
   536	                /* Bound + overflow-check the deadline (A6-3). */
   537	                uint64_t now = je_sched_ticks();
   538	                if (timeout > JE_IPC_MAX_WAIT || timeout > (UINT64_MAX - now)) {
   539	                    f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
   540	                }
   541	                uint32_t slot = 0u;
   542	                if (je_sched_current_slot(&slot) != JE_OK) {
   543	                    f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
   544	                }
   545	                je_ep_channel *wrec = (je_ep_channel *)0; uint8_t wside = 0u;
   546	                je_ep_status reg = je_endpoint_kern_wait_register(vm, index, h, slot, &wrec, &wside);
   547	                if (reg == JE_EP_WAITERS_BUSY) {   /* A6-4: slot occupied -> non-blocking EMPTY */
   548	                    f->rax=(uint64_t)JE_ABI_H_EMPTY; f->rdx=0; return je_sched_return_to_user(f);
   549	                }
   550	                if (reg != JE_EP_OK) {
   551	                    f->rax=(uint64_t)ep_status_to_abi(reg); f->rdx=0; return je_sched_return_to_user(f);
   552	                }
   553	                /* Park: saves args + owned rec/side, rewinds int 0x80, switches. */
   554	                return je_sched_block_current(f, wrec, wside, now + timeout);
   555	            }
   556	            /* probe != EMPTY: deliverable / SHORT_BUFFER / PEER_CLOSED / error ->
   557	             * fall through to the normal recv below (which re-derives the exact
   558	             * result and, for a deliverable message, does the full mint/copy). */
   559	        }
   560	        uint8_t staging[JE_EP_MSG_MAX];
   561	        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
   562	        size_t mlen = 0u; uint8_t mcaps = 0u;
   563	        je_ep_recv_lease lease;
   564	        je_ep_status s = je_endpoint_kern_recv_peek_caps(vm, index, h, staging,
   565	                                                         (size_t)cap, &mlen,
   566	                                                         (uint8_t)hcap, caps, &mcaps,
   567	                                                         &lease);
   568	        if (s == JE_EP_SHORT_BUFFER) {
   569	            f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
   570	            f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32);
   571	            return je_sched_return_to_user(f);
   572	        }
   573	        if (s != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return je_sched_return_to_user(f); }
   574	        /* Stage mints: a live cap mints a fresh handle; a dead cap stages 0 (A5-2).
   575	         * Roll back all minted-so-far on any transient failure (QUOTA/NOSLOTS/
   576	         * REFOVERFLOW). */
   577	        je_handle minted[JE_ABI_MSG_CAPS_MAX];
   578	        bool was_live[JE_ABI_MSG_CAPS_MAX];
   579	        for (uint8_t c = 0; c < mcaps; ++c) { minted[c] = 0u; was_live[c] = false; }
   580	        je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
   581	        for (uint8_t c = 0; c < mcaps; ++c) {
   582	            if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c] = 0u; continue; }
   583	            je_handle nh = 0u;
   584	            je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
   585	            if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
   586	            minted[c] = nh; was_live[c] = true; done = (uint8_t)(c + 1u);
   587	        }
   588	        if (fail != JE_ABI_OK) {
   589	            for (uint8_t c = 0; c < done; ++c)
   590	                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
   591	            f->rax=(uint64_t)fail; f->rdx=0; return je_sched_return_to_user(f);
   592	        }
   593	        /* Copy bytes out. */
   594	        if (mlen != 0u) {
   595	            je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
   596	            if (cs != JE_ABI_OK) {
   597	                for (uint8_t c = 0; c < mcaps; ++c)
   598	                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
   599	                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
   600	            }
   601	        }
   602	        /* Copy minted handle array out (u32 each). */
   603	        if (mcaps != 0u) {
   604	            uint32_t hout[JE_ABI_MSG_CAPS_MAX];
   605	            for (uint8_t c = 0; c < mcaps; ++c) hout[c] = (uint32_t)minted[c];
   606	            je_abi_status cs = je_copy_to_user(hbuf, hout,
   607	                                               (uint64_t)mcaps * sizeof(uint32_t));
   608	            if (cs != JE_ABI_OK) {
   609	                for (uint8_t c = 0; c < mcaps; ++c)
   610	                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
   611	                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
   612	            }
   613	        }
   614	        /* Infallible commit: dequeue + release each cap's inflight ref. */
   615	        je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
   616	        if (cm != JE_EP_OK) {
   617	            je_arch_console_write("JeiOS fatal: recv commit mismatch\r\n");
   618	            je_arch_halt();
   619	        }
   620	        f->rax = (uint64_t)JE_ABI_OK;
   621	        f->rdx = (uint64_t)mlen | ((uint64_t)mcaps << 32);
   622	        return je_sched_return_to_user(f);
   623	    }
   624	#ifdef JE_DEV_COPY_INJECT
   625	    case JE_SYSCALL_COPY_FIXUP_INJECT: {
   626	        /* dev-only: raw-copy an unmapped arena page to prove the fixup recovers;
   627	         * RDX reports 1 if recovered. RDI = the unmapped user address. */
   628	        int recovered = je_copy_fixup_inject(f->rdi);
   629	        f->rax = (uint64_t)JE_ABI_OK; f->rdx = (uint64_t)recovered;
   630	        return je_sched_return_to_user(f);
   631	    }
   632	#endif
   633	    case JE_SYSCALL_RETURN:
   634	    default:
   635	        f->rax = (uint64_t)status; f->rdx = result;
   636	        /* Validate+sanitize the frame on this user return (ADR: every return). */
   637	        return je_sched_return_to_user(f);
   638	    }
   639	}
   640	#endif

 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * M3c sub-PR 3 (ADR 0009 D-SEQ step 3): the kernel's single endpoint pool and
     4	 * the je_vm wiring (teardown hook + IRQ critical section). This is the only file
     5	 * that holds endpoint global state; endpoint.c stays pure/hostable. Lives in the
     6	 * real kernel build only (NOT linked into the hosted unit tests), so it may use
     7	 * the arch IRQ primitives and the kernel pool freely.
     8	 */
     9	#include <jeios/endpoint_kern.h>
    10	#include <jeios/sched.h>   /* je_sched_wake_ipc for the blocking-recv bridge */
    11	#include <jeios/arch.h>
    12	
    13	/* The one kernel-wide pool. A dying process's endpoints are reclaimed into it by
    14	 * the teardown hook; a leaked record would eventually exhaust it (the whole
    15	 * point of A3-1). */
    16	static je_endpoint_pool g_kep;
    17	static bool g_kep_inited;
    18	
    19	static void kep_init_once(void)
    20	{
    21	    if (!g_kep_inited) { je_endpoint_pool_init(&g_kep); g_kep_inited = true; }
    22	}
    23	/* Forward decl: peer-close wake helper (defined with the bridge functions). */
    24	static void kern_peer_close_wake(je_ep_channel *rec, uint8_t side);
    25	
    26	/* Teardown hook: the release walk over the dying process's table. A negative
    27	 * return is an invariant violation (pool corruption) and MUST stop the kernel
    28	 * before the table is zeroed — continuing would zero away the leak evidence and
    29	 * ship a lie. */
    30	static void kep_teardown(void *ctx, je_handle_table *t)
    31	{
    32	    je_endpoint_pool *pool = (je_endpoint_pool *)ctx;
    33	    if (je_endpoint_release_table(pool, t) < 0) {
    34	        je_arch_console_write("JeiOS fatal: endpoint release walk failed "
    35	                              "(pool corrupt)\r\n");
    36	        je_arch_halt();
    37	    }
    38	}
    39	
    40	void je_endpoint_kern_install(je_vm *vm)
    41	{
    42	    if (!vm) return;
    43	    kep_init_once();
    44	    vm->teardown     = kep_teardown;
    45	    vm->teardown_ctx = &g_kep;
    46	    vm->irq_save     = je_arch_irq_save;
    47	    vm->irq_restore  = je_arch_irq_restore;
    48	}
    49	
    50	je_ep_status je_endpoint_kern_create(je_vm *vm, uint32_t index,
    51	                                     je_handle *out0, je_handle *out1)
    52	{
    53	    if (out0) *out0 = 0u;
    54	    if (out1) *out1 = 0u;
    55	    if (!vm || index >= JE_VM_MAX_PROCESSES || !out0 || !out1)
    56	        return JE_EP_BADINDEX;
    57	    /* STRUCTURAL GUARANTEE (A3-2): refuse to mint an endpoint into a table whose
    58	     * death path will not reclaim it atomically. Require the FULL wiring — the
    59	     * teardown hook AND the IRQ critical-section pair — not just the hook, so a
    60	     * partially-wired vm cannot mint without the A3-1 critical section. */
    61	    if (vm->teardown != kep_teardown || vm->teardown_ctx != &g_kep
    62	        || vm->irq_save == (uint64_t (*)(void))0
    63	        || vm->irq_restore == (void (*)(uint64_t))0)
    64	        return JE_EP_CORRUPT;
    65	    kep_init_once();
    66	    je_handle_table *t = &vm->procs[index].htable;
    67	    return je_endpoint_channel_create(&g_kep, t, JE_EP_RIGHTS_ALL,
    68	                                      t, JE_EP_RIGHTS_ALL, out0, out1);
    69	}
    70	
    71	je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h)
    72	{
    73	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    74	    kep_init_once();
    75	    /* Capture the end being closed BEFORE the close invalidates the handle, so we
    76	     * can wake a receiver blocked on the PEER end if this close peer-closes it
    77	     * (A6-PEERWAKE). A resolve failure just means the normal close path reports it. */
    78	    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
    79	    bool have = (je_endpoint_resolve(&g_kep, &vm->procs[index].htable, h, &rec, &side)
    80	                 == JE_EP_OK);
    81	    je_ep_status s = je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
    82	    if (s == JE_EP_OK && have)
    83	        kern_peer_close_wake(rec, (uint8_t)(side ^ 1u));   /* peer end */
    84	    return s;
    85	}
    86	
    87	/* Data IPC forwarders (M3c step 4): resolve the caller's table and run the pure
    88	 * send/peek/commit against the kernel pool. The glue (syscall.c) does the
    89	 * copy_from/to_user around these, honoring D6 ordering. */
    90	je_ep_status je_endpoint_kern_send(je_vm *vm, uint32_t index, je_handle h,
    91	                                   const void *kbuf, size_t len)
    92	{
    93	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    94	    kep_init_once();
    95	    je_ep_status s = je_endpoint_try_send(&g_kep, &vm->procs[index].htable, h, kbuf, len);
    96	    if (s == JE_EP_OK) je_endpoint_kern_send_wake(vm, index, h);   /* A6: wake peer waiter */
    97	    return s;
    98	}
    99	
   100	je_ep_status je_endpoint_kern_send_preflight(je_vm *vm, uint32_t index,
   101	                                             je_handle h, size_t len)
   102	{
   103	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
   104	    kep_init_once();
   105	    /* Main-handle preflight only (no QUEUE_FULL) so per-cap errors outrank
   106	     * QUEUE_FULL in the cap-send glue (A5-4). The final try_send_caps re-checks
   107	     * QUEUE_FULL atomically at enqueue. */
   108	    return je_endpoint_send_preflight_main(&g_kep, &vm->procs[index].htable, h, len);
   109	}
   110	
   111	je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
   112	                                        void *out_kbuf, size_t cap,
   113	                                        size_t *out_len, je_ep_recv_lease *lease)
   114	{
   115	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
   116	    kep_init_once();
   117	    return je_endpoint_recv_peek(&g_kep, &vm->procs[index].htable, h,
   118	                                 out_kbuf, cap, out_len, lease);
   119	}
   120	
   121	je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease)
   122	{
   123	    kep_init_once();
   124	    return je_endpoint_recv_commit_caps(&g_kep, lease);
   125	}
   126	
   127	/* ---- capability transfer forwarders (M3c step 5) ------------------------- */
   128	
   129	je_ep_status je_endpoint_kern_send_caps(je_vm *vm, uint32_t index, je_handle h,
   130	                                        const void *kbuf, size_t len,
   131	                                        const je_ep_capdesc *caps, uint8_t ncaps)
   132	{
   133	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
   134	    kep_init_once();
   135	    je_ep_status s = je_endpoint_try_send_caps(&g_kep, &vm->procs[index].htable, h,
   136	                                               kbuf, len, caps, ncaps);
   137	    if (s == JE_EP_OK) je_endpoint_kern_send_wake(vm, index, h);   /* A6: wake peer waiter */
   138	    return s;
   139	}
   140	
   141	je_ep_status je_endpoint_kern_resolve_send_cap(je_vm *vm, uint32_t index,
   142	                                               je_handle h, uint32_t req_rights,
   143	                                               je_ep_capdesc *out)
   144	{
   145	    if (!vm || index >= JE_VM_MAX_PROCESSES || !out) return JE_EP_BADINDEX;
   146	    kep_init_once();
   147	    je_handle_table *tbl = &vm->procs[index].htable;
   148	    /* The handle must be an endpoint holding TRANSFER. */
   149	    je_ep_channel *rec; uint8_t side;
   150	    je_ep_status rs = je_endpoint_resolve(&g_kep, tbl, h, &rec, &side);
   151	    if (rs != JE_EP_OK) return rs;
   152	    uint32_t rights = 0u;
   153	    je_handle_status gr = je_handle_rights_of(tbl, h, JE_ENDPOINT_OBJ_TYPE,
   154	                                              JE_EP_RIGHT_TRANSFER, &rights);
   155	    if (gr != JE_HANDLE_OK) {
   156	        /* map DENIED/etc via the same table mapping the pure layer uses */
   157	        switch (gr) {
   158	        case JE_HANDLE_BADINDEX:  return JE_EP_BADINDEX;
   159	        case JE_HANDLE_STALE:     return JE_EP_STALE;
   160	        case JE_HANDLE_WRONGTYPE: return JE_EP_WRONGTYPE;
   161	        case JE_HANDLE_DENIED:    return JE_EP_DENIED;
   162	        default:                  return JE_EP_CORRUPT;
   163	        }
   164	    }
   165	    /* Attenuation: the requested rights must be a subset of what the handle holds. */
   166	    if (req_rights & ~rights) return JE_EP_DENIED;
   167	    /* Build the descriptor: the target end is the handle's OWN end (the receiver
   168	     * will get a handle to the same end the sender named). rec_index via pointer
   169	     * arithmetic into the kernel pool. */
   170	    out->rec_index = (uint16_t)(rec - &g_kep.rec[0]);
   171	    out->side = side;
   172	    out->cap_count_pad = 0u;
   173	    out->generation = rec->generation;
   174	    out->rights = req_rights;
   175	    return JE_EP_OK;
   176	}
   177	
   178	je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
   179	                                             void *out_kbuf, size_t cap,
   180	                                             size_t *out_len, uint8_t hcap,
   181	                                             je_ep_capdesc *out_caps,
   182	                                             uint8_t *out_ncaps,
   183	                                             je_ep_recv_lease *lease)
   184	{
   185	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
   186	    kep_init_once();
   187	    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
   188	                                      out_kbuf, cap, out_len, hcap,
   189	                                      out_caps, out_ncaps, lease);
   190	}
   191	
   192	je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
   193	                                            const je_ep_capdesc *d, je_handle *out)
   194	{
   195	    if (out) *out = 0u;
   196	    if (!vm || index >= JE_VM_MAX_PROCESSES || !d || !out) return JE_EP_BADINDEX;
   197	    kep_init_once();
   198	    je_handle_table *tbl = &vm->procs[index].htable;
   199	    je_ep_channel *tr; uint8_t ts;
   200	    je_ep_status s = je_endpoint_capdesc_resolve(&g_kep, d, &tr, &ts);
   201	    if (s != JE_EP_OK) return s;                 /* dead target -> STALE (A5-2) */
   202	    /* Retain-before-commit: retain a handle_ref on the target, then alloc the
   203	     * receiver handle; roll back the retain on alloc failure. */
   204	    je_ep_status rr = je_endpoint_capdesc_retain_handle(&g_kep, d);
   205	    if (rr != JE_EP_OK) return rr;               /* REFOVERFLOW */
   206	    je_handle nh = 0u;
   207	    je_handle_status a = je_handle_alloc(tbl, (void *)&tr->end[ts],
   208	                                         JE_ENDPOINT_OBJ_TYPE, d->rights, &nh);
   209	    if (a != JE_HANDLE_OK) {
   210	        je_endpoint_capdesc_release_handle(&g_kep, d);   /* undo retain */
   211	        switch (a) {
   212	        case JE_HANDLE_QUOTA:   return JE_EP_QUOTA;
   213	        case JE_HANDLE_NOSLOTS: return JE_EP_NOSLOTS;
   214	        default:                return JE_EP_CORRUPT;
   215	        }
   216	    }
   217	    *out = nh;
   218	    return JE_EP_OK;
   219	}
   220	
   221	bool je_endpoint_kern_capdesc_live(const je_ep_capdesc *d)
   222	{
   223	    return je_endpoint_capdesc_live(&g_kep, d);
   224	}
   225	
   226	void je_endpoint_kern_capdesc_release_handle(const je_ep_capdesc *d)
   227	{
   228	    je_endpoint_capdesc_release_handle(&g_kep, d);
   229	}
   230	
   231	je_ep_status je_endpoint_kern_seed(je_vm *vm, uint32_t idxA, uint32_t idxB,
   232	                                   je_handle *outA, je_handle *outB)
   233	{
   234	    if (outA) *outA = 0u;
   235	    if (outB) *outB = 0u;
   236	    if (!vm || idxA >= JE_VM_MAX_PROCESSES || idxB >= JE_VM_MAX_PROCESSES
   237	        || idxA == idxB || !outA || !outB) return JE_EP_BADINDEX;
   238	    /* A5-4: both VMs must be hooked so the seeded ends are on a reclaiming death
   239	     * path (the raw channel_create bypasses op-10's A3-2 hook check). */
   240	    if (vm->teardown != kep_teardown || vm->teardown_ctx != &g_kep
   241	        || vm->irq_save == (uint64_t (*)(void))0
   242	        || vm->irq_restore == (void (*)(uint64_t))0) return JE_EP_CORRUPT;
   243	    kep_init_once();
   244	    return je_endpoint_channel_create(&g_kep, &vm->procs[idxA].htable,
   245	                                      JE_EP_RIGHTS_ALL,
   246	                                      &vm->procs[idxB].htable, JE_EP_RIGHTS_ALL,
   247	                                      outA, outB);
   248	}
   249	
   250	uint32_t je_endpoint_kernel_allocated(void)
   251	{
   252	    return g_kep_inited ? je_endpoint_pool_allocated(&g_kep) : 0u;
   253	}
   254	
   255	/* ---- M3c D7/A6 blocking-recv bridge (ties the pure waiter to the scheduler) ---- */
   256	
   257	/* A6-CLARIFY-2: the single release owner, called by sched.c mark_dying with the
   258	 * dying thread's slot (= waiter_id). Releases any waiter/reservation it holds. */
   259	bool je_endpoint_kern_waiter_release_by_id(uint32_t slot)
   260	{
   261	    if (!g_kep_inited) return false;
   262	    return je_endpoint_wait_release_by_id(&g_kep, slot);
   263	}
   264	
   265	/* Entry probe + waiter register for the recv glue. */
   266	je_ep_status je_endpoint_kern_recv_entry_probe(je_vm *vm, uint32_t index, je_handle h)
   267	{
   268	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
   269	    kep_init_once();
   270	    return je_endpoint_recv_entry_probe(&g_kep, &vm->procs[index].htable, h);
   271	}
   272	
   273	je_ep_status je_endpoint_kern_wait_register(je_vm *vm, uint32_t index, je_handle h,
   274	                                            uint32_t waiter_id,
   275	                                            je_ep_channel **out_rec, uint8_t *out_side)
   276	{
   277	    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
   278	    kep_init_once();
   279	    return je_endpoint_wait_register(&g_kep, &vm->procs[index].htable, h,
   280	                                     waiter_id, out_rec, out_side);
   281	}
   282	
   283	/* Wake a receiver blocked on the PEER of the sender's end after a successful send.
   284	 * A message sent on end `h` (sender's own end) lands in the PEER end's inbound, so
   285	 * the waiter to wake is on the peer end. Arbitrate WOKEN there; on WON wake the
   286	 * scheduler slot. Idempotent/no-op if no waiter or already terminal. Call AFTER a
   287	 * send returned JE_EP_OK, under IF=0. Resolving `h` here cannot fail (it just
   288	 * succeeded in the send). */
   289	void je_endpoint_kern_send_wake(je_vm *vm, uint32_t index, je_handle h)
   290	{
   291	    if (!vm || index >= JE_VM_MAX_PROCESSES || !g_kep_inited) return;
   292	    je_ep_channel *rec; uint8_t side;
   293	    if (je_endpoint_resolve(&g_kep, &vm->procs[index].htable, h, &rec, &side) != JE_EP_OK)
   294	        return;
   295	    uint32_t id = 0u;
   296	    if (je_endpoint_wait_arbitrate(rec, (uint8_t)(side ^ 1u), JE_EP_WAIT_WOKEN, &id)
   297	        == JE_EP_ARB_WON)
   298	        je_sched_wake_ipc(id);
   299	}
   300	
   301	/* Wake a receiver blocked on an end that just became PEER_CLOSED (A6-PEERWAKE),
   302	 * when that end's inbound is EMPTY (drain-first A6-1: a non-empty end was already
   303	 * woken WOKEN by its enqueue). `rec,side` name the end that transitioned to
   304	 * PEER_CLOSED. Arbitrate PEER_CLOSED + wake. Under IF=0. */
   305	static void kern_peer_close_wake(je_ep_channel *rec, uint8_t side)
   306	{
   307	    if (!rec) return;
   308	    if (je_endpoint_end_state(rec, side) != JE_EP_END_PEER_CLOSED) return;
   309	    if (je_endpoint_inbound_count(rec, side) != 0u) return;   /* drain-first */
   310	    uint32_t id = 0u;
   311	    if (je_endpoint_wait_arbitrate(rec, side, JE_EP_WAIT_PEER_CLOSED, &id)
   312	        == JE_EP_ARB_WON)
   313	        je_sched_wake_ipc(id);
   314	}
   315	
   316	/* Internal cancel (A6-2; no ABI path in v1). Arbitrate CANCELLED on the waiter
   317	 * owned by `slot` wherever it is parked, and wake it. Used only by the dev-gated
   318	 * cancel/forced-kill test hook. Returns true iff a waiter was cancelled. */
   319	bool je_endpoint_kern_wait_cancel(uint32_t slot)
   320	{
   321	    if (!g_kep_inited) return false;
   322	    for (uint16_t i = 0; i < JE_EP_POOL_CAP; ++i) {
   323	        for (uint8_t s = 0; s < 2u; ++s) {
   324	            je_ep_wait_cause c = je_endpoint_wait_cause(&g_kep.rec[i], s);
   325	            if (c == JE_EP_WAIT_NONE) continue;
   326	            uint32_t id = 0u;
   327	            if (je_endpoint_wait_arbitrate(&g_kep.rec[i], s, JE_EP_WAIT_CANCELLED, &id)
   328	                == JE_EP_ARB_WON && id == slot) { je_sched_wake_ipc(id); return true; }
   329	        }
   330	    }
   331	    return false;
   332	}
   333	
   334	/* The continuation's reserved-head read: finish a WOKEN recv from the OWNED
   335	 * rec/side (never re-resolves the handle — A6-5). Peeks the reserved head
   336	 * (bytes + cap descriptors) into the out buffers, returns the lease for commit. */
   337	je_ep_status je_endpoint_kern_recv_peek_owned(je_ep_channel *rec, uint8_t side,
   338	                                              void *out_kbuf, size_t cap,
   339	                                              size_t *out_len, uint8_t hcap,
   340	                                              je_ep_capdesc *out_caps, uint8_t *out_ncaps,
   341	                                              je_ep_recv_lease *lease)
   342	{
   343	    kep_init_once();
   344	    return je_endpoint_recv_peek_owned_caps(&g_kep, rec, side, out_kbuf, cap,
   345	                                            out_len, hcap, out_caps, out_ncaps, lease);
   346	}
   347	
   348	/* Release the waiter + reservation on an owned end after a successful consume
   349	 * (the continuation calls this post-commit). */
   350	void je_endpoint_kern_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who)
   351	{
   352	    (void)je_endpoint_wait_release(rec, side, who);
   353	}

 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	#include <jeios/arch.h>
     3	#include <jeios/kheap.h>
     4	#include <jeios/paging.h>
     5	#include <jeios/sched.h>
     6	#include <jeios/uframe.h>
     7	#include <jeios/vm.h>
     8	#include <jeios/endpoint.h>
     9	#include <jeios/endpoint_kern.h>   /* je_endpoint_kern_waiter_release_by_id */
    10	#define STACK_PAGES 5u
    11	/* M3c A6-CLARIFY-1: the fixed length of the `int 0x80` syscall gate instruction
    12	 * (`CD 80`), rewound on park so the woken thread re-executes it. Architecturally
    13	 * fixed by the ABI (Intel SDM: INT imm8 = CD ib). The only DPL3 IDT gate, and the
    14	 * PIC is remapped off 0x80, so rip-JE_INT80_LEN provably points at the site. */
    15	#define JE_INT80_LEN 2u
    16	typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING, BLOCKED_IPC } state;
    17	typedef enum { THREAD_KERNEL = 0, THREAD_USER = 1 } thread_kind;
    18	typedef struct {
    19	    state state;
    20	    je_trap_frame *frame;
    21	    void (*entry)(void *);
    22	    void *argument;
    23	    uint64_t stack, wake, dispatches;
    24	    /* M3a #4 step 3: user-thread fields. For THREAD_KERNEL these are 0/unused.
    25	     * For THREAD_USER: root is the process CR3; rsp0_top is the per-process
    26	     * kernel-stack top loaded into TSS.RSP0 on dispatch; vm/vm_index name the
    27	     * owning process so teardown (not reap) frees its held kstack. */
    28	    thread_kind kind;
    29	    uint64_t root, rsp0_top;
    30	    je_vm *vm;
    31	    uint32_t vm_index;
    32	    /* M3a #4 step 4 / #5: death record for a DYING user thread. `reason`
    33	     * distinguishes a synchronous fault, a clean exit(code), and an invalid
    34	     * return state (read by the bootstrap reaper and the proofs). */
    35	    uint64_t death_vector, death_error, death_rip, death_cs, death_cr2;
    36	    uint64_t death_reason, exit_code;
    37	    /* M3c D7/A6: blocking-recv continuation ("constrained restartable syscall",
    38	     * A6-CLARIFY-1). `pending` gates the continuation prologue in dispatch. When a
    39	     * recv parks: pending=1, the recv ARGS (rdi..r9) are saved, the OWNED endpoint
    40	     * record+side are saved (so resume never re-resolves the handle — A6-5), the
    41	     * deadline (absolute ticks; 0 = none) is set, and the post-int RIP is saved to
    42	     * assert the restart landed where expected. Cleared on continuation completion
    43	     * or on wait release. */
    44	    bool     wait_pending;
    45	    je_ep_channel *wait_rec;
    46	    uint8_t  wait_side;
    47	    uint64_t wait_rdi, wait_rsi, wait_rdx, wait_r8, wait_r9;
    48	    uint64_t wait_deadline;      /* absolute tick; 0 = no deadline (shouldn't park) */
    49	    uint64_t wait_post_int_rip;  /* f->rip at park (the instruction after int 0x80) */
    50	} thread;
    51	/* Death reasons (M3a #5). FAULT: synchronous user fault. EXIT: clean exit(code).
    52	 * BAD_RETURN: a user frame failed je_uframe_check (invalid return state). */
    53	typedef enum { DEATH_FAULT = 0, DEATH_EXIT = 1, DEATH_BAD_RETURN = 2 } death_reason;
    54	static thread threads[JE_THREAD_MAX];
    55	static unsigned current;
    56	static bool ready;
    57	static uint64_t preemptions;
    58	static uint64_t user_cpl3_dispatches; /* M3a #4 step 3: times a THREAD_USER frame (cs&3==3) was seen */
    59	volatile uint64_t je_timer_ticks;
    60	static _Noreturn void abort_scheduler(void)
    61	{
    62	    (void)je_arch_irq_save();
    63	    je_arch_console_write("JeiOS fatal: scheduler invariant\r\n");je_arch_halt();
    64	}
    65	static _Noreturn void trampoline(void)
    66	{
    67	    threads[current].entry(threads[current].argument);
    68	    je_thread_exit();
    69	}
    70	void je_sched_init(void)
    71	{
    72	    if (ready) abort_scheduler();
    73	    threads[0].state=RUNNING;current=0;ready=true;
    74	}
    75	je_status je_thread_create(void (*entry)(void *),void *arg)
    76	{
    77	    if (!ready || !entry) return JE_ERR_INVALID;
    78	    unsigned slot=1;
    79	    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
    80	    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
    81	    uint64_t base;
    82	    je_status s=je_pages_alloc(&je_kernel_pages,STACK_PAGES,&base);
    83	    if (s!=JE_OK) return s;
    84	    unsigned char *memory=(void *)(uintptr_t)base;
    85	    for (size_t i=0;i<STACK_PAGES*4096;++i) memory[i]=0;
    86	    s=je_arch_page_guard((uintptr_t)base);
    87	    if (s!=JE_OK) {
    88	        je_status cleanup=je_pages_free(&je_kernel_pages,base,STACK_PAGES);
    89	        return cleanup==JE_OK ? s : cleanup;
    90	    }
    91	    uintptr_t top=(uintptr_t)base+STACK_PAGES*4096;
    92	    uintptr_t rsp=top-8; /* SysV C entry: RSP % 16 == 8, with fake return address. */
    93	    *(uintptr_t *)rsp=(uintptr_t)je_thread_exit;
    94	    je_trap_frame *f=(void *)(rsp-sizeof(*f));
    95	    *f=(je_trap_frame){.rip=(uintptr_t)trampoline,.cs=8,.rflags=0x202,.rsp=rsp,.ss=16};
    96	    threads[slot]=(thread){READY,f,entry,arg,base,0,0,THREAD_KERNEL,0,0,0,0,0,0,0,0,0,0,0,
    97	                           false,0,0,0,0,0,0,0,0,0};
    98	    return JE_OK;
    99	}
   100	
   101	/* M3a #4 step 3: create a user thread for a READY vm process. Synthesizes the
   102	 * initial ring-3 trap frame on the process's held kernel stack, so the first
   103	 * scheduler dispatch iretqs into CPL3 at the validated entry. The kstack is the
   104	 * process's concrete 5-page run (held by #3); its top is TSS.RSP0. Requires NX
   105	 * (ADR 0007 D2). Does NOT allocate from je_kernel_pages — the kstack belongs to
   106	 * the vm process and is freed by je_vm_destroy, never by je_sched_reap. */
   107	je_status je_thread_create_user(je_vm *vm, uint32_t index)
   108	{
   109	    if (!ready || !vm || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
   110	    if (!je_arch_has_nx()) return JE_ERR_UNSUPPORTED; /* D2 */
   111	    je_vm_process *p = &vm->procs[index];
   112	    if (p->state != JE_VM_READY || p->held_kstack != JE_VM_KSTACK_PAGES || !p->root)
   113	        return JE_ERR_INVALID;
   114	    unsigned slot=1;
   115	    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
   116	    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
   117	    uint64_t kbase = p->kstack_physical;              /* identity VA (< 1 GiB) */
   118	    /* Guard the first page of the held kstack (ADR D4 assigns the guard to #4);
   119	     * RSP0 is the top of the remaining usable pages. */
   120	    je_status g = je_arch_page_guard((uintptr_t)kbase);
   121	    if (g != JE_OK) return g;
   122	    uint64_t rsp0_top = kbase + (uint64_t)STACK_PAGES * 4096u;
   123	    /* Initial user frame at the very top of the kstack, GPRs zero (no kernel
   124	     * register leak into ring 3). user RSP starts at arena top (full-descending,
   125	     * validator allows top inclusive); RIP at validated entry. */
   126	    je_trap_frame *f = (void *)(uintptr_t)(rsp0_top - sizeof(*f));
   127	    *f = (je_trap_frame){0};
   128	    f->cs = JE_UFRAME_USER_CS; f->ss = JE_UFRAME_USER_SS;
   129	    f->rip = JE_VM_ARENA_BASE + p->entry;
   130	    f->rsp = JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE;
   131	    f->rflags = 0x202u; /* IF=1, bit1; validator re-sanitizes on every dispatch */
   132	    threads[slot]=(thread){READY,f,0,0,0,0,0,THREAD_USER,p->root,rsp0_top,vm,index,0,0,0,0,0,0,0,
   133	                           false,0,0,0,0,0,0,0,0,0};
   134	    return JE_OK;
   135	}
   136	/* M3a #4 step 4: mark a user thread DYING with a death record (never requeued;
   137	 * reclaimed later by je_sched_reap_dead from bootstrap). Kernel threads never
   138	 * take this path. */
   139	static void mark_dying(unsigned slot, death_reason reason, uint64_t code,
   140	                       uint64_t vec, uint64_t err, uint64_t rip, uint64_t cs, uint64_t cr2)
   141	{
   142	    /* M3c A6-CLARIFY-2: release any blocking-recv waiter/reservation this thread
   143	     * holds BEFORE its slot is torn down, via the single idempotent release owner
   144	     * (keyed on the thread slot = waiter_id). Safe no-op if the thread held none.
   145	     * This is the one site that RELEASES; the reaper only asserts it is clear. */
   146	    (void)je_endpoint_kern_waiter_release_by_id((uint32_t)slot);
   147	    threads[slot].wait_pending=false;
   148	    threads[slot].state=DYING;
   149	    threads[slot].frame=0;
   150	    threads[slot].death_reason=(uint64_t)reason; threads[slot].exit_code=code;
   151	    threads[slot].death_vector=vec; threads[slot].death_error=err;
   152	    threads[slot].death_rip=rip; threads[slot].death_cs=cs; threads[slot].death_cr2=cr2;
   153	}
   154	
   155	/* Install the chosen next thread's address space and return its frame. For a
   156	 * user next thread, validate+sanitize its saved frame first; if that FAILS the
   157	 * thread is terminated (DYING) and we retry with another candidate — a user
   158	 * task must never be able to halt the kernel via a bad saved frame (fault
   159	 * locality; fixes the pre-step-4 abort-on-bad-return-state hole). Bootstrap
   160	 * (slot 0) is always READY, so a live candidate always exists. Returns the
   161	 * frame to iretq to; never requeues a DYING/ZOMBIE thread. */
   162	static je_trap_frame *select_and_install_next(void)
   163	{
   164	    for (unsigned attempt=0; attempt<JE_THREAD_MAX; ++attempt) {
   165	        unsigned next=current;
   166	        for (unsigned n=1;n<=JE_THREAD_MAX;++n) {
   167	            unsigned candidate=(current+n)%JE_THREAD_MAX;
   168	            if (threads[candidate].state==READY) { next=candidate;break; }
   169	        }
   170	        if (threads[next].state!=READY || !threads[next].frame) abort_scheduler();
   171	        if (threads[next].kind==THREAD_USER) {
   172	            uint64_t sane;
   173	            if (!je_uframe_check(threads[next].frame, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
   174	                /* Bad saved return state -> terminate THIS task, not the kernel. */
   175	                mark_dying(next, DEATH_BAD_RETURN, 0, 0, 0, threads[next].frame->rip,
   176	                           threads[next].frame->cs, 0);
   177	                continue; /* pick another candidate */
   178	            }
   179	            threads[next].frame->rflags = sane;
   180	            je_arch_tss_set_rsp0(threads[next].rsp0_top);
   181	            je_arch_cr3_load(threads[next].root);
   182	        } else {
   183	            je_arch_tss_set_rsp0(0); /* poison: CPL0->CPL0 never consults RSP0 */
   184	            je_arch_cr3_load(je_arch_kernel_root_phys());
   185	        }
   186	        current=next;threads[current].state=RUNNING;++threads[current].dispatches;
   187	        return threads[current].frame;
   188	    }
   189	    abort_scheduler(); /* unreachable: bootstrap is always a valid kernel candidate */
   190	}
   191	
   192	je_trap_frame *je_sched_interrupt(je_trap_frame *f,bool timer)
   193	{
   194	    if (!ready || threads[current].state==EMPTY) abort_scheduler();
   195	    /* Validate the INCOMING frame by the current thread's kind, not by trusting
   196	     * the frame. A kernel thread must present a CPL0 frame (cs==8); a user
   197	     * thread must present its exact user frame landing at rsp0_top-frame (where
   198	     * the CPU writes it on a CPL3->CPL0 trap). Any mismatch is corruption. */
   199	    if (threads[current].kind==THREAD_KERNEL) {
   200	        if ((f->cs&3) != 0 || f->cs != 8) abort_scheduler();
   201	    } else {
   202	        if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS) abort_scheduler();
   203	        if ((uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
   204	        ++user_cpl3_dispatches; /* observed a genuine CPL3 (cs&3==3) frame return from a user thread */
   205	    }
   206	    threads[current].frame=f;
   207	    if (timer) {
   208	        if (je_timer_ticks==UINT64_MAX) abort_scheduler();
   209	        ++je_timer_ticks;
   210	    }
   211	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   212	        if (threads[i].state==SLEEPING && threads[i].wake<=je_timer_ticks) threads[i].state=READY;
   213	    /* M3c D7/A6: on a timer tick, stamp TIMED_OUT on any blocked-IPC waiter whose
   214	     * absolute deadline has elapsed, through the SINGLE endpoint arbitrate CAS
   215	     * (first-critical-section-wins: if a sender/close already stamped it WOKEN/
   216	     * PEER_CLOSED this is a no-op), then make it READY so its continuation returns
   217	     * the cause. Inclusive deadline (wake<=ticks), matching the SLEEPING sweep. */
   218	    if (timer)
   219	        for (unsigned i=1;i<JE_THREAD_MAX;++i)
   220	            if (threads[i].state==BLOCKED_IPC && threads[i].wait_deadline!=0u &&
   221	                threads[i].wait_deadline<=je_timer_ticks) {
   222	                uint32_t id=0u;
   223	                je_ep_arb_result r=je_endpoint_wait_arbitrate(threads[i].wait_rec,
   224	                                                              threads[i].wait_side,
   225	                                                              JE_EP_WAIT_TIMED_OUT, &id);
   226	                /* Whether this CAS won (TIMED_OUT) or lost (already WOKEN/closed),
   227	                 * the waiter is now terminal: make the thread runnable. Guarded
   228	                 * flip, BLOCKED_IPC -> READY only. */
   229	                (void)r;
   230	                threads[i].state=READY;
   231	            }
   232	    if (threads[current].state==RUNNING) threads[current].state=READY;
   233	    unsigned before=current;
   234	    je_trap_frame *nf=select_and_install_next();
   235	    if (timer && current!=before) ++preemptions;
   236	    return nf;
   237	}
   238	
   239	/* M3a #4 step 4: a SYNCHRONOUS user fault (or user int 0x80) terminates ONLY the
   240	 * current user process; the kernel and peers keep running. Called from
   241	 * je_trap_dispatch with interrupts disabled, on the dying task's own per-process
   242	 * RSP0 kstack — so we must NOT free that kstack here (the C frame + the iretq
   243	 * epilogue still run on it). We mark the task DYING and return a LIVE successor's
   244	 * frame; the bootstrap reaper reclaims the dead process later. Never requeues
   245	 * the dying task. */
   246	je_trap_frame *je_sched_fault_terminate(je_trap_frame *f, uint64_t cr2)
   247	{
   248	    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler(); /* only user tasks die here */
   249	    /* Confirm the frame really is the current user task's (defense in depth). */
   250	    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
   251	        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
   252	    mark_dying(current, DEATH_FAULT, 0, f->vector, f->error, f->rip, f->cs, cr2);
   253	    return select_and_install_next();
   254	}
   255	
   256	/* M3a #5: clean exit(code) — same mechanics as fault_terminate (mark the current
   257	 * user thread DYING on its own kstack, return a live successor; the reaper frees
   258	 * it later) but records a DEATH_EXIT reason and the exit code rather than a
   259	 * fault. Separate from je_sched_fault_terminate (tested step-4 code) by design. */
   260	je_trap_frame *je_sched_exit_current(je_trap_frame *f, uint64_t code)
   261	{
   262	    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
   263	    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
   264	        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
   265	    mark_dying(current, DEATH_EXIT, code, 0, 0, f->rip, f->cs, 0);
   266	    return select_and_install_next();
   267	}
   268	
   269	/* M3c D7/A6-CLARIFY-1: park the CURRENT user thread as a blocking-recv waiter and
   270	 * switch to a live successor. Modeled on je_sched_exit_current (same frame-identity
   271	 * checks) PLUS a deliberate pre-park je_uframe_check: a thread that would park MUST
   272	 * have a valid return frame, so a user-corrupted RSP/RFLAGS is rejected as
   273	 * DEATH_BAD_RETURN at park time rather than discovered at wake while it holds a
   274	 * reservation (A6-CLARIFY-2 window). The endpoint-side waiter was already
   275	 * registered by the caller (je_endpoint_kern_*); here we only save the continuation
   276	 * state, rewind f->rip to the int 0x80 site, set BLOCKED_IPC, and switch. The saved
   277	 * rec/side is the OWNED reference the continuation resumes from (never re-resolves
   278	 * the handle, A6-5). `deadline` is absolute ticks (0 would be a poll, which never
   279	 * parks — a caller passing 0 here is a bug). Returns a live successor's frame. */
   280	je_trap_frame *je_sched_block_current(je_trap_frame *f, je_ep_channel *rec,
   281	                                      uint8_t side, uint64_t deadline)
   282	{
   283	    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
   284	    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
   285	        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
   286	    uint64_t sane;
   287	    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
   288	        /* Corrupt return frame: do NOT park (it would hold a reservation it could
   289	         * never consume). Terminate now; the caller's endpoint waiter is released
   290	         * by mark_dying's teardown hook. */
   291	        mark_dying(current, DEATH_BAD_RETURN, 0, 0, 0, f->rip, f->cs, 0);
   292	        return select_and_install_next();
   293	    }
   294	    f->rflags = sane;
   295	    thread *t = &threads[current];
   296	    t->wait_pending = true;
   297	    t->wait_rec = rec; t->wait_side = side;
   298	    t->wait_rdi = f->rdi; t->wait_rsi = f->rsi; t->wait_rdx = f->rdx;
   299	    t->wait_r8 = f->r8; t->wait_r9 = f->r9;
   300	    t->wait_deadline = deadline;
   301	    t->wait_post_int_rip = f->rip;      /* instruction after int 0x80 */
   302	    f->rip -= JE_INT80_LEN;             /* re-execute int 0x80 on resume */
   303	    t->frame = f;
   304	    t->state = BLOCKED_IPC;
   305	    return select_and_install_next();
   306	}
   307	
   308	/* M3c D7/A6-STRUCT: wake a blocked-IPC waiter by thread slot (the opaque
   309	 * waiter_id the endpoint layer stored). This is a GUARDED wrapper: it flips
   310	 * BLOCKED_IPC -> READY ONLY (never from any other state), so it can never
   311	 * resurrect a DYING/ZOMBIE thread (A6-5) and never races an independent
   312	 * transition. The terminal cause was already set by the single endpoint
   313	 * arbitrate CAS BEFORE this is called; this only makes the thread runnable so its
   314	 * continuation can read that cause. MUST run under IF=0. */
   315	void je_sched_wake_ipc(uint32_t slot)
   316	{
   317	    if (slot == 0u || slot >= JE_THREAD_MAX) return;
   318	    if (threads[slot].state == BLOCKED_IPC) threads[slot].state = READY;
   319	}
   320	
   321	/* M3c A6: the continuation prologue (syscall.c) queries whether the CURRENT thread
   322	 * resumed from a blocking-recv park, and reads back the saved continuation state
   323	 * (owned rec/side + the recv args + the terminal RIP to assert). Returns false if
   324	 * no continuation is pending. Does NOT clear pending (the glue clears it via
   325	 * je_sched_wait_finish once the result is delivered). */
   326	bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side, uint64_t *rdi,
   327	                           uint64_t *rsi, uint64_t *rdx, uint64_t *r8, uint64_t *r9,
   328	                           uint64_t *post_int_rip)
   329	{
   330	    thread *t = &threads[current];
   331	    if (!t->wait_pending) return false;
   332	    if (rec) *rec = t->wait_rec;
   333	    if (side) *side = t->wait_side;
   334	    if (rdi) *rdi = t->wait_rdi;
   335	    if (rsi) *rsi = t->wait_rsi;
   336	    if (rdx) *rdx = t->wait_rdx;
   337	    if (r8)  *r8  = t->wait_r8;
   338	    if (r9)  *r9  = t->wait_r9;
   339	    if (post_int_rip) *post_int_rip = t->wait_post_int_rip;
   340	    return true;
   341	}
   342	
   343	/* Clear the current thread's pending continuation (called by the glue after it has
   344	 * written the result into the frame). */
   345	void je_sched_wait_finish(void)
   346	{
   347	    threads[current].wait_pending = false;
   348	    threads[current].wait_rec = (je_ep_channel *)0;
   349	}
   350	
   351	/* The current thread's slot index (= the opaque waiter_id used by the endpoint
   352	 * waiter + the release owner). */
   353	je_status je_sched_current_slot(uint32_t *out_slot)
   354	{
   355	    if (!ready || !out_slot) return JE_ERR_INVALID;
   356	    *out_slot = (uint32_t)current;
   357	    return JE_OK;
   358	}
   359	
   360	/* M3a #5: return path for a syscall that resumes the CURRENT user task (no
   361	 * context switch) — e.g. abi_query, UNSUPPORTED, INVALID. ADR 0007 D6/M3A.md §4
   362	 * require validating the user frame on EVERY return, so a non-switching syscall
   363	 * return must NOT skip je_uframe_check (a user can corrupt its own RSP/RFLAGS
   364	 * before int 0x80). Validate+sanitize here; on failure terminate (BAD_RETURN)
   365	 * and switch to a live successor instead of iretq'ing a bad frame. The caller
   366	 * has already written the result registers (rax/rdx) into f. */
   367	je_trap_frame *je_sched_return_to_user(je_trap_frame *f)
   368	{
   369	    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
   370	    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
   371	        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
   372	    uint64_t sane;
   373	    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
   374	        mark_dying(current, DEATH_BAD_RETURN, 0, 0, 0, f->rip, f->cs, 0);
   375	        return select_and_install_next();
   376	    }
   377	    f->rflags = sane;
   378	    return f; /* resume the caller; CR3/RSP0 already correct for the current task */
   379	}
   380	je_status je_thread_sleep(uint64_t duration)
   381	{
   382	    uint64_t flags=je_arch_irq_save();
   383	    if (!ready || current==0 || duration>UINT64_MAX-je_timer_ticks) {
   384	        je_arch_irq_restore(flags);return current==0 ? JE_ERR_DENIED : JE_ERR_INVALID;
   385	    }
   386	    if (duration) {
   387	        threads[current].wake=je_timer_ticks+duration;
   388	        threads[current].state=SLEEPING;
   389	    }
   390	    __asm__ volatile("int $0x81":::"memory");

exec
/usr/bin/bash -lc "nl -ba tests/test_endpoint.c | sed -n '520,860p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba kernel/core/endpoint.c | sed -n '1,230p;380,485p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -RIn \"je_uframe_check\\|current_vm_index\\|je_sched_ticks\\|JE_VM_ARENA_BASE\\|JE_VM_ARENA_SIZE\" kernel include tests | head -160" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "git diff --cached --check && git diff --cached --numstat && git diff --cached -- kernel/core/sched.c kernel/core/syscall.c kernel/core/endpoint.c kernel/core/endpoint_kern.c | sed -n '1,260p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
   520	        assert(minted!=0 && minted!=c1);
   521	        assert(mr->end[ms].handle_refs==c1_hr_before+1u);
   522	        /* commit: dequeue + release the cap inflight. */
   523	        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
   524	        assert(crec->end[cside].inflight_refs==c1_inflight_before); /* released */
   525	        /* The minted handle resolves to the SAME end as the original c1. */
   526	        je_ep_channel *vr; uint8_t vs;
   527	        assert(je_endpoint_resolve(&g_pool,&g_t,minted,&vr,&vs)==JE_EP_OK);
   528	        assert(vr==crec && vs==cside);
   529	    }
   530	
   531	    /* 23. Dead-cap (A5-2): send a cap, then CLOSE the target before recv; peek
   532	     * reports the cap but capdesc_live() is false -> deliver null. */
   533	    fresh(QUOTA);
   534	    {
   535	        je_handle s0=0,s1=0,c0=0,c1=0;
   536	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&s0,&s1)==JE_EP_OK);
   537	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
   538	        je_ep_channel *crec; uint8_t cside;
   539	        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&crec,&cside)==JE_EP_OK);
   540	        je_ep_capdesc d = { (uint16_t)(crec - &g_pool.rec[0]), cside, 0u, crec->generation, JE_EP_RIGHTS_ALL };
   541	        assert(je_endpoint_try_send_caps(&g_pool,&g_t,s0,(void*)0,0,&d,1)==JE_EP_OK);
   542	        /* close BOTH handles to c1's end (c1) AND c0 so the record can go... but
   543	         * the queued cap's inflight keeps it allocated. Close c1 (the target). */
   544	        assert(je_endpoint_handle_close(&g_pool,&g_t,c1)==JE_EP_OK);
   545	        /* target end now CLOSED (handle_refs 0) but inflight>0 keeps record alive. */
   546	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
   547	        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
   548	        assert(nc==1u);
   549	        assert(!je_endpoint_capdesc_live(&g_pool,&rc[0]));   /* DEAD -> deliver 0 */
   550	        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);  /* releases inflight */
   551	    }
   552	
   553	    /* 24. Cycle reclaim (A5-1): A<->B caps queued each way; closing all handles
   554	     * discards both inbounds, releasing the cap inflights -> both records free. */
   555	    fresh(QUOTA);
   556	    {
   557	        je_handle a0=0,a1=0,b0=0,b1=0;
   558	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&a0,&a1)==JE_EP_OK);
   559	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&b0,&b1)==JE_EP_OK);
   560	        assert(je_endpoint_pool_allocated(&g_pool)==2u);
   561	        je_ep_channel *ar,*br; uint8_t as,bs;
   562	        assert(je_endpoint_resolve(&g_pool,&g_t,a1,&ar,&as)==JE_EP_OK);
   563	        assert(je_endpoint_resolve(&g_pool,&g_t,b1,&br,&bs)==JE_EP_OK);
   564	        je_ep_capdesc da = { (uint16_t)(ar-&g_pool.rec[0]), as, 0u, ar->generation, JE_EP_RIGHTS_ALL };
   565	        je_ep_capdesc db = { (uint16_t)(br-&g_pool.rec[0]), bs, 0u, br->generation, JE_EP_RIGHTS_ALL };
   566	        /* send cap-to-A1 over B (lands in b1 inbound) and cap-to-B1 over A (a1 inbound) */
   567	        assert(je_endpoint_try_send_caps(&g_pool,&g_t,b0,(void*)0,0,&da,1)==JE_EP_OK);
   568	        assert(je_endpoint_try_send_caps(&g_pool,&g_t,a0,(void*)0,0,&db,1)==JE_EP_OK);
   569	        /* close every handle; no one ever receives -> discard-on-close must break
   570	         * the cycle and free both records. */
   571	        assert(je_endpoint_handle_close(&g_pool,&g_t,a0)==JE_EP_OK);
   572	        assert(je_endpoint_handle_close(&g_pool,&g_t,a1)==JE_EP_OK);
   573	        assert(je_endpoint_handle_close(&g_pool,&g_t,b0)==JE_EP_OK);
   574	        assert(je_endpoint_handle_close(&g_pool,&g_t,b1)==JE_EP_OK);
   575	        assert(je_endpoint_pool_allocated(&g_pool)==0u);   /* cycle reclaimed */
   576	    }
   577	
   578	    /* 25. Cap edge cases (A5): dead+live siblings in one message, two caps to the
   579	     * same target, self-referential cap (cap to a1 sent over a0 -> sits in a1's
   580	     * OWN inbound), and pool occupancy after a dead-cap commit. */
   581	    fresh(QUOTA);
   582	    {
   583	        je_handle s0=0,s1=0,c0=0,c1=0,d0=0,d1=0;
   584	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&s0,&s1)==JE_EP_OK);
   585	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
   586	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&d0,&d1)==JE_EP_OK);
   587	        je_ep_channel *cr,*dr; uint8_t cs2,ds;
   588	        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&cr,&cs2)==JE_EP_OK);
   589	        assert(je_endpoint_resolve(&g_pool,&g_t,d1,&dr,&ds)==JE_EP_OK);
   590	        je_ep_capdesc cd = { (uint16_t)(cr-&g_pool.rec[0]), cs2, 0u, cr->generation, JE_EP_RIGHTS_ALL };
   591	        je_ep_capdesc dd = { (uint16_t)(dr-&g_pool.rec[0]), ds, 0u, dr->generation, JE_EP_RIGHTS_ALL };
   592	        /* dead+live sibling: close c1 (dead) BUT keep d1 (live), send both as a
   593	         * 2-cap message over s0. */
   594	        je_ep_capdesc pair[2] = { cd, dd };
   595	        /* close c1 so cd's target is dead; but cd is not yet queued so nothing
   596	         * holds c's inflight — c's record frees on c1+c0 close. Re-create c after
   597	         * to keep a valid (dead) descriptor generation. Simpler: send FIRST (both
   598	         * live), then this tests two-live; dead-sibling is covered by test 23. */
   599	        assert(je_endpoint_try_send_caps(&g_pool,&g_t,s0,(void*)0,0,pair,2)==JE_EP_OK);
   600	        assert(cr->end[cs2].inflight_refs>=1u && dr->end[ds].inflight_refs>=1u);
   601	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rcv[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
   602	        /* short handle buffer (hcap 1 < 2 caps) -> SHORT_BUFFER, no dequeue. */
   603	        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,1,rcv,&nc,&lz)==JE_EP_SHORT_BUFFER);
   604	        assert(nc==2u);   /* required slot count reported */
   605	        /* enough buffer: both delivered. */
   606	        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rcv,&nc,&lz)==JE_EP_OK);
   607	        assert(nc==2u && je_endpoint_capdesc_live(&g_pool,&rcv[0]) && je_endpoint_capdesc_live(&g_pool,&rcv[1]));
   608	        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
   609	        assert(cr->end[cs2].inflight_refs==0u && dr->end[ds].inflight_refs==0u); /* released */
   610	    }
   611	
   612	    /* 26. Self-referential cap: a cap to a1 sent over a0 lands in a1's OWN inbound
   613	     * (a0's peer is a1). Closing all of a's handles must discard a1's inbound and
   614	     * release the self-cap inflight -> record frees (A5-1 reclaim, self edge). */
   615	    fresh(QUOTA);
   616	    {
   617	        je_handle a0=0,a1=0;
   618	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&a0,&a1)==JE_EP_OK);
   619	        je_ep_channel *ar; uint8_t as;
   620	        assert(je_endpoint_resolve(&g_pool,&g_t,a1,&ar,&as)==JE_EP_OK);
   621	        je_ep_capdesc self = { (uint16_t)(ar-&g_pool.rec[0]), as, 0u, ar->generation, JE_EP_RIGHTS_ALL };
   622	        /* send cap-to-a1 over a0 -> lands in a1's inbound (a0's peer = a1). */
   623	        assert(je_endpoint_try_send_caps(&g_pool,&g_t,a0,(void*)0,0,&self,1)==JE_EP_OK);
   624	        assert(ar->end[as].inflight_refs==1u);
   625	        /* close both handles; a1's inbound holds the self-cap. Discard-on-close of
   626	         * a1 must release the self-cap inflight -> record frees. */
   627	        assert(je_endpoint_handle_close(&g_pool,&g_t,a0)==JE_EP_OK);
   628	        assert(je_endpoint_handle_close(&g_pool,&g_t,a1)==JE_EP_OK);
   629	        assert(je_endpoint_pool_allocated(&g_pool)==0u);  /* self-edge reclaimed */
   630	    }
   631	
   632	    /* 27. Blocking-recv waiter primitives (A6, pure layer): register, the single
   633	     * terminal CAS, entry-probe reserved-head masking, release idempotence. */
   634	    fresh(QUOTA);
   635	    {
   636	        je_handle a0=0,a1=0;  /* a0 = sender end, a1 = receiver end */
   637	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&a0,&a1)==JE_EP_OK);
   638	        je_ep_channel *rr; uint8_t rs;   /* a1's end */
   639	        assert(je_endpoint_resolve(&g_pool,&g_t,a1,&rr,&rs)==JE_EP_OK);
   640	        /* entry probe on empty OPEN end -> EMPTY (would block). */
   641	        assert(je_endpoint_recv_entry_probe(&g_pool,&g_t,a1)==JE_EP_EMPTY);
   642	        /* register a waiter (id 3) on a1. */
   643	        je_ep_channel *wr; uint8_t ws;
   644	        assert(je_endpoint_wait_register(&g_pool,&g_t,a1,3u,&wr,&ws)==JE_EP_OK);
   645	        assert(wr==rr && ws==rs);
   646	        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_WAITING);
   647	        /* A6-4: a second register on the same end -> WAITERS_BUSY. */
   648	        assert(je_endpoint_wait_register(&g_pool,&g_t,a1,4u,&wr,&ws)==JE_EP_WAITERS_BUSY);
   649	        /* a sender puts a message in a1's inbound (send on a0 -> peer a1). */
   650	        uint8_t msg[3]={7,8,9};
   651	        assert(je_endpoint_try_send(&g_pool,&g_t,a0,msg,3)==JE_EP_OK);
   652	        /* arbitrate WOKEN -> WON, reserves the head, reports waiter id 3. */
   653	        uint32_t woke=0u;
   654	        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_WOKEN,&woke)==JE_EP_ARB_WON && woke==3u);
   655	        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_WOKEN);
   656	        /* single terminal CAS: a second arbitrate (e.g. TIMED_OUT) -> LOST, cause unchanged. */
   657	        uint32_t x=0u;
   658	        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_TIMED_OUT,&x)==JE_EP_ARB_LOST);
   659	        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_WOKEN);
   660	        /* the reserved head is NOT deliverable to a concurrent handle-based peek. */
   661	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0;
   662	        assert(je_endpoint_recv_peek(&g_pool,&g_t,a1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_EMPTY);
   663	        /* but the OWNED-end peek (the reserving waiter) sees it. */
   664	        uint8_t ncaps=0u; je_ep_capdesc cd[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
   665	        assert(je_endpoint_recv_peek_owned_caps(&g_pool,rr,rs,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,cd,&ncaps,&lz)==JE_EP_OK);
   666	        assert(rl==3u && rb[0]==7 && rb[2]==9 && ncaps==0u);
   667	        /* commit + release the waiter. */
   668	        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
   669	        assert(je_endpoint_wait_release(rr,rs,3u)==true);
   670	        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_NONE);
   671	        /* release idempotence: releasing again (or a wrong id) is a safe no-op. */
   672	        assert(je_endpoint_wait_release(rr,rs,3u)==false);
   673	    }
   674	
   675	    /* 28. A6-CLARIFY-2: release-by-id returns a reserved head to deliverable. */
   676	    fresh(QUOTA);
   677	    {
   678	        je_handle b0=0,b1=0;
   679	        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&b0,&b1)==JE_EP_OK);
   680	        je_ep_channel *rr; uint8_t rs;
   681	        assert(je_endpoint_resolve(&g_pool,&g_t,b1,&rr,&rs)==JE_EP_OK);
   682	        je_ep_channel *wr; uint8_t ws;
   683	        assert(je_endpoint_wait_register(&g_pool,&g_t,b1,5u,&wr,&ws)==JE_EP_OK);
   684	        uint8_t msg[1]={42};
   685	        assert(je_endpoint_try_send(&g_pool,&g_t,b0,msg,1)==JE_EP_OK);
   686	        uint32_t w=0u;
   687	        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_WOKEN,&w)==JE_EP_ARB_WON);
   688	        /* head reserved -> concurrent peek EMPTY. */
   689	        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0;
   690	        assert(je_endpoint_recv_peek(&g_pool,&g_t,b1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_EMPTY);
   691	        /* the waiter "dies before re-entry": release by id (as mark_dying would). */
   692	        assert(je_endpoint_wait_release_by_id(&g_pool,5u)==true);
   693	        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_NONE);
   694	        /* head is deliverable again at the front (A6-CLARIFY-2). */
   695	        assert(je_endpoint_recv_peek(&g_pool,&g_t,b1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_OK);
   696	        assert(rl==1u && rb[0]==42);
   697	    }
   698	
   699	    printf("endpoint: PASS (create, split-ref close/destruction, dup "
   700	           "acquire-before-commit, REFOVERFLOW, DENIED/WRONGTYPE, inflight keep-"
   701	           "alive, pool-exhaustion + create-unwind, release-walk, data IPC "
   702	           "send/recv/FIFO/QUEUE_FULL/SHORT_BUFFER/PEER_CLOSED/rights/zero-len, "
   703	           "cap-transfer mint/dead-cap/cycle-reclaim)\n");
   704	    return 0;
   705	}

 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * M3c sub-PR 2 (ADR 0009 D-SEQ step 2): endpoint/channel lifecycle core.
     4	 * Hostable, no kernel state, no ABI include — see endpoint.h for the full
     5	 * contract and the load-bearing table-purity precondition. Sound only under the
     6	 * ADR 0008 D6 uniprocessor / IF=0 precondition (no SMP claim).
     7	 */
     8	#include <jeios/endpoint.h>
     9	
    10	/* Policy-bound pins (ADR 0009 D4): REF_MAX must admit every non-handle owning
    11	 * ref (queue depth + waiters) and still be small enough to reach in a test, and
    12	 * it must fit the uint16_t counters. */
    13	_Static_assert(JE_EP_REF_MAX >= JE_EP_QUEUE_DEPTH + JE_EP_MAX_WAITERS,
    14	               "JE_EP_REF_MAX must cover queue depth + waiters");
    15	_Static_assert(JE_EP_REF_MAX <= 0xffffu, "ref counts are uint16_t");
    16	_Static_assert(JE_EP_POOL_CAP >= 1u, "pool must have at least one record");
    17	_Static_assert(JE_EP_MSG_MAX <= 255u, "per-slot length fits uint8_t");
    18	_Static_assert(JE_EP_QUEUE_DEPTH <= 255u, "head/count fit uint8_t");
    19	_Static_assert(JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX <= JE_EP_REF_MAX,
    20	               "a full queue of cap-carrying msgs must not overflow inflight");
    21	/* Keep the static kernel pool bounded: the per-slot byte buffers + cap
    22	 * descriptors dominate. Grew for step-5 caps; re-pinned here. */
    23	_Static_assert(sizeof(je_ep_channel) <= 1280u, "channel record size budget");
    24	
    25	/* ---- pool / record helpers ------------------------------------------------ */
    26	
    27	/* Reset (empty + zero) one inbound queue. Zeroing the payload bytes prevents a
    28	 * stale message surviving into a reused slot and never lets uninitialized bytes
    29	 * reach a reader (defense in depth; recv copies only len bytes regardless). */
    30	static void queue_reset(je_ep_queue *q)
    31	{
    32	    q->head = 0u;
    33	    q->count = 0u;
    34	    for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i) {
    35	        q->len[i] = 0u;
    36	        q->cap_count[i] = 0u;
    37	        for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[i][b] = 0u;
    38	        for (uint8_t c = 0; c < JE_EP_MSG_CAPS_MAX; ++c) {
    39	            q->cap[i][c].rec_index = 0u; q->cap[i][c].side = 0u;
    40	            q->cap[i][c].cap_count_pad = 0u;
    41	            q->cap[i][c].generation = 0u; q->cap[i][c].rights = 0u;
    42	        }
    43	    }
    44	}
    45	
    46	/* Count the capability descriptors currently queued anywhere in one inbound
    47	 * queue (used by the A5-3 free-time assertion: a record only frees when BOTH
    48	 * inbound queues are cap-free). */
    49	static uint32_t queue_cap_total(const je_ep_queue *q)
    50	{
    51	    uint32_t n = 0u;
    52	    for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i) n += q->cap_count[i];
    53	    return n;
    54	}
    55	
    56	void je_endpoint_pool_init(je_endpoint_pool *pool)
    57	{
    58	    if (!pool) return;
    59	    pool->allocated_count = 0u;
    60	    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
    61	        je_ep_channel *r = &pool->rec[i];
    62	        r->allocated = false;
    63	        r->generation = 1u;            /* never 0 */
    64	        for (uint8_t s = 0; s < 2u; ++s) {
    65	            r->end[s].state = JE_EP_END_FREE;
    66	            r->end[s].handle_refs = 0u;
    67	            r->end[s].inflight_refs = 0u;
    68	            r->end[s].side = s;
    69	            queue_reset(&r->end[s].inbound);
    70	        }
    71	    }
    72	}
    73	
    74	uint32_t je_endpoint_pool_allocated(const je_endpoint_pool *pool)
    75	{
    76	    return pool ? pool->allocated_count : 0u;
    77	}
    78	
    79	je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side)
    80	{
    81	    if (!rec || side > 1u) return JE_EP_END_FREE;
    82	    return rec->end[side].state;
    83	}
    84	
    85	/* The opaque object pointer a handle slot binds for an endpoint is &rec->end[s].
    86	 * Map such a pointer back to (rec, side), validating it lies on a real end of a
    87	 * LIVE record in THIS pool — never dereference an unvalidated pointer. */
    88	static je_ep_status object_to_end(const je_endpoint_pool *pool, const void *obj,
    89	                                  je_ep_channel **out_rec, uint8_t *out_side)
    90	{
    91	    if (!pool || !obj) return JE_EP_CORRUPT;
    92	    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
    93	        const je_ep_channel *r = &pool->rec[i];
    94	        for (uint8_t s = 0; s < 2u; ++s) {
    95	            if ((const void *)&r->end[s] == obj) {
    96	                if (!r->allocated) return JE_EP_CORRUPT;   /* stale end pointer */
    97	                *out_rec = (je_ep_channel *)r;
    98	                *out_side = s;
    99	                return JE_EP_OK;
   100	            }
   101	        }
   102	    }
   103	    return JE_EP_CORRUPT;
   104	}
   105	
   106	/* Free a record once BOTH ends hold no references of either class. */
   107	static void maybe_free_record(je_endpoint_pool *pool, je_ep_channel *rec)
   108	{
   109	    /* A5-3 idempotence guard: the cap-release discard path can reach this twice
   110	     * for one record; a second call on an already-free record must be a no-op
   111	     * (never double-decrement allocated_count / double-bump generation). */
   112	    if (!rec->allocated) return;
   113	    for (uint8_t s = 0; s < 2u; ++s) {
   114	        if (rec->end[s].handle_refs != 0u || rec->end[s].inflight_refs != 0u)
   115	            return;
   116	    }
   117	    /* A5-3: a record only frees when BOTH ends have zero refs. With zero inflight
   118	     * refs there can be no queued caps pointing at this record, and this record's
   119	     * own inbound queues must themselves be cap-free. If a residual cap is somehow
   120	     * present it means an inflight ref was leaked — an invariant violation; guard
   121	     * by NOT freeing (fail-safe: leak the record rather than free it with a live
   122	     * cap), which is unreachable under the detach-before-release discard order. */
   123	    for (uint8_t s = 0; s < 2u; ++s) {
   124	        if (queue_cap_total(&rec->end[s].inbound) != 0u) return; /* unreachable; fail-safe */
   125	    }
   126	    /* Both ends fully unreferenced: reclaim. Bump generation (never 0). */
   127	    rec->allocated = false;
   128	    rec->generation = (rec->generation == 0xffffffffu) ? 1u
   129	                                                       : rec->generation + 1u;
   130	    for (uint8_t s = 0; s < 2u; ++s) {
   131	        rec->end[s].state = JE_EP_END_FREE;
   132	        rec->end[s].side = s;
   133	        queue_reset(&rec->end[s].inbound);   /* no stale payload into reuse */
   134	    }
   135	    if (pool->allocated_count) pool->allocated_count--;
   136	}
   137	
   138	/* Map je_handle_status -> je_ep_status for the resolve/alloc paths. */
   139	static je_ep_status hstat(je_handle_status s)
   140	{
   141	    switch (s) {
   142	    case JE_HANDLE_OK:        return JE_EP_OK;
   143	    case JE_HANDLE_BADINDEX:  return JE_EP_BADINDEX;
   144	    case JE_HANDLE_STALE:     return JE_EP_STALE;
   145	    case JE_HANDLE_WRONGTYPE: return JE_EP_WRONGTYPE;
   146	    case JE_HANDLE_DENIED:    return JE_EP_DENIED;
   147	    case JE_HANDLE_QUOTA:     return JE_EP_QUOTA;
   148	    case JE_HANDLE_NOSLOTS:   return JE_EP_NOSLOTS;
   149	    }
   150	    return JE_EP_CORRUPT;   /* fail closed if the enum ever grows */
   151	}
   152	
   153	/* ---- resolve a handle to its end ----------------------------------------- */
   154	
   155	je_ep_status je_endpoint_resolve(const je_endpoint_pool *pool,
   156	                                 const je_handle_table *t, je_handle h,
   157	                                 je_ep_channel **out_rec, uint8_t *out_side)
   158	{
   159	    void *obj = (void *)0;
   160	    je_handle_status s;
   161	    if (!pool || !t || !out_rec || !out_side) return JE_EP_BADINDEX;
   162	    /* Resolve as an ENDPOINT with no required rights; keeps the table's
   163	     * BADINDEX -> STALE -> WRONGTYPE precedence. */
   164	    s = je_handle_lookup(t, h, JE_ENDPOINT_OBJ_TYPE, 0u, &obj);
   165	    if (s != JE_HANDLE_OK) return hstat(s);
   166	    return object_to_end(pool, obj, out_rec, out_side);
   167	}
   168	
   169	/* ---- internal handle_ref acquire / release ------------------------------- */
   170	
   171	/* Acquire one handle_ref on an end; fail closed past REF_MAX with nothing taken. */
   172	static je_ep_status retain_handle(je_ep_end *e)
   173	{
   174	    if (e->handle_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
   175	    e->handle_refs++;
   176	    return JE_EP_OK;
   177	}
   178	
   179	/* Drop one handle_ref on an end that definitely holds at least one (infallible
   180	 * for a valid live ref). On reaching 0: this end CLOSED, a live peer -> PEER_CLOSED,
   181	 * then try to reclaim the record. */
   182	/* Release one inflight ref on the end a queued cap descriptor points at (A5-3
   183	 * discard path). The target is in THIS pool; a guarded maybe_free_record may free
   184	 * it. Defence-in-depth: validate the descriptor resolves to an allocated,
   185	 * generation-matching end (it must, while the inflight ref kept it alive). */
   186	static void release_capdesc_inflight(je_endpoint_pool *pool,
   187	                                     const je_ep_capdesc *d)
   188	{
   189	    if (d->rec_index >= JE_EP_POOL_CAP || d->side > 1u) return; /* unreachable */
   190	    je_ep_channel *tr = &pool->rec[d->rec_index];
   191	    if (!tr->allocated || tr->generation != d->generation) return; /* unreachable */
   192	    je_ep_end *te = &tr->end[d->side];
   193	    if (te->inflight_refs == 0u) return;   /* underflow guard (unreachable) */
   194	    te->inflight_refs--;
   195	    if (te->inflight_refs == 0u) maybe_free_record(pool, tr);
   196	}
   197	
   198	static void release_handle(je_endpoint_pool *pool, je_ep_channel *rec, uint8_t side)
   199	{
   200	    je_ep_end *e = &rec->end[side];
   201	    /* handle_refs == 0 here would be an invariant violation by the caller; guard
   202	     * defensively (never underflow) but do not mask it as success elsewhere. */
   203	    if (e->handle_refs == 0u) return;
   204	    e->handle_refs--;
   205	    if (e->handle_refs == 0u) {
   206	        e->state = JE_EP_END_CLOSED;
   207	        je_ep_end *peer = &rec->end[side ^ 1u];
   208	        if (peer->state == JE_EP_END_OPEN) peer->state = JE_EP_END_PEER_CLOSED;
   209	        /* A5-3 discard ordering: (1) DETACH every cap descriptor still queued in
   210	         * this closing end's inbound into a bounded local array; (2) reset the
   211	         * queue (bytes + cap storage) so this record is fully settled and holds
   212	         * no caps; (3) maybe_free THIS record; (4) ONLY THEN release each detached
   213	         * inflight ref on its (other-record) target. Doing releases last means
   214	         * freeing a target never re-enters a queue still being discarded, and the
   215	         * guarded maybe_free_record is idempotent if a target is this same record
   216	         * via the peer end (its inbound was/will be discarded at its own close). */
   217	        je_ep_capdesc detached[JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX];
   218	        uint32_t nd = 0u;
   219	        je_ep_queue *q = &e->inbound;
   220	        for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i)
   221	            for (uint8_t c = 0; c < q->cap_count[i]; ++c)
   222	                detached[nd++] = q->cap[i][c];
   223	        queue_reset(q);                  /* this end holds no caps now */
   224	        maybe_free_record(pool, rec);    /* settle THIS record first */
   225	        for (uint32_t k = 0; k < nd; ++k)
   226	            release_capdesc_inflight(pool, &detached[k]);  /* then other records */
   227	    }
   228	}
   229	
   230	/* ---- inflight ref API (tested directly; used by queue/wait PRs) ----------- */
   380	    if (e->inflight_refs == 0u) return JE_EP_CORRUPT;   /* underflow guard */
   381	    e->inflight_refs--;
   382	    if (e->inflight_refs == 0u) maybe_free_record(pool, rec);
   383	    return JE_EP_OK;
   384	}
   385	
   386	/* ---- create (kernel-only) ------------------------------------------------- */
   387	
   388	/* Return a tentatively-allocated record to the FREE state on a create unwind,
   389	 * restoring the FREE <=> !allocated invariant (states, not just the flag). The
   390	 * record was never observed by any caller, so no generation bump is needed. */
   391	static void unwind_record(je_ep_channel *rec)
   392	{
   393	    rec->allocated = false;
   394	    for (uint8_t s = 0; s < 2u; ++s) {
   395	        rec->end[s].state = JE_EP_END_FREE;
   396	        rec->end[s].handle_refs = 0u;
   397	        rec->end[s].inflight_refs = 0u;
   398	        rec->end[s].side = s;
   399	    }
   400	}
   401	
   402	je_ep_status je_endpoint_channel_create(je_endpoint_pool *pool,
   403	                                        je_handle_table *t0, uint32_t rights0,
   404	                                        je_handle_table *t1, uint32_t rights1,
   405	                                        je_handle *out0, je_handle *out1)
   406	{
   407	    if (out0) *out0 = 0u;
   408	    if (out1) *out1 = 0u;
   409	    if (!pool || !t0 || !t1 || !out0 || !out1) return JE_EP_BADINDEX;
   410	    /* Aliased outputs would make the second write clobber the first handle,
   411	     * orphaning end 0's LIVE slot (a permanently unreclaimable ref). Reject. */
   412	    if (out0 == out1) return JE_EP_BADINDEX;
   413	    /* Reject rights with any unknown endpoint bit (defense in depth; the glue
   414	     * also checks). */
   415	    if ((rights0 & ~(uint32_t)JE_EP_RIGHTS_ALL)
   416	        || (rights1 & ~(uint32_t)JE_EP_RIGHTS_ALL)) return JE_EP_BADINDEX;
   417	
   418	    /* Find a FREE record (lowest index). */
   419	    je_ep_channel *rec = (je_ep_channel *)0;
   420	    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
   421	        if (!pool->rec[i].allocated) { rec = &pool->rec[i]; break; }
   422	    }
   423	    if (!rec) return JE_EP_NOSLOTS;   /* pool exhausted */
   424	
   425	    /* Tentatively allocate the record and open both ends. */
   426	    rec->allocated = true;
   427	    for (uint8_t s = 0; s < 2u; ++s) {
   428	        rec->end[s].state = JE_EP_END_OPEN;
   429	        rec->end[s].handle_refs = 0u;
   430	        rec->end[s].inflight_refs = 0u;
   431	        rec->end[s].side = s;
   432	    }
   433	
   434	    /* Mint end 0: retain then alloc. retain of a fresh end cannot overflow. */
   435	    je_handle h0 = 0u, h1 = 0u;
   436	    je_ep_status rs = retain_handle(&rec->end[0]);
   437	    if (rs != JE_EP_OK) { unwind_record(rec); return rs; }   /* unreachable */
   438	    je_handle_status a0 = je_handle_alloc(t0, (void *)&rec->end[0],
   439	                                          JE_ENDPOINT_OBJ_TYPE, rights0, &h0);
   440	    if (a0 != JE_HANDLE_OK) {
   441	        unwind_record(rec);                /* record never observed */
   442	        return hstat(a0);
   443	    }
   444	
   445	    /* Mint end 1 similarly. */
   446	    rs = retain_handle(&rec->end[1]);
   447	    if (rs != JE_EP_OK) {                   /* unreachable; unwind end 0 fully */
   448	        (void)je_handle_close(t0, h0);
   449	        unwind_record(rec);
   450	        return rs;
   451	    }
   452	    je_handle_status a1 = je_handle_alloc(t1, (void *)&rec->end[1],
   453	                                          JE_ENDPOINT_OBJ_TYPE, rights1, &h1);
   454	    if (a1 != JE_HANDLE_OK) {
   455	        /* Roll back the whole transaction: close h0, drop both retains, free. */
   456	        (void)je_handle_close(t0, h0);
   457	        unwind_record(rec);
   458	        return hstat(a1);
   459	    }
   460	
   461	    pool->allocated_count++;
   462	    *out0 = h0;
   463	    *out1 = h1;
   464	    return JE_EP_OK;
   465	}
   466	
   467	/* ---- close seam ----------------------------------------------------------- */
   468	
   469	je_ep_status je_endpoint_handle_close(je_endpoint_pool *pool,
   470	                                      je_handle_table *t, je_handle h)
   471	{
   472	    je_ep_channel *rec = (je_ep_channel *)0;
   473	    uint8_t side = 0u;
   474	    je_ep_status s = je_endpoint_resolve(pool, t, h, &rec, &side);
   475	    if (s != JE_EP_OK) return s;
   476	    /* Invalidate the table slot FIRST (so no LIVE slot ever names a freed end),
   477	     * then drop the handle_ref. Under IF=0 a resolved handle always closes. */
   478	    je_handle_status c = je_handle_close(t, h);
   479	    if (c != JE_HANDLE_OK) return hstat(c);   /* defensive; not expected */
   480	    release_handle(pool, rec, side);
   481	    return JE_EP_OK;
   482	}
   483	
   484	/* ---- dup seam (retain-before-commit; endpoints never use dup_self) -------- */
   485	

 succeeded in 0ms:
kernel/arch/x86_64/user_demo.S:9: * JE_VM_ARENA_BASE + 0x1000 = 0x40001000. Using an absolute disp32 address keeps
kernel/arch/x86_64/user_demo.S:246: * terminate it at its next dispatch (je_uframe_check fails) instead of halting
kernel/arch/x86_64/vm_backend.c:29:_Static_assert(JE_PHYSICAL_LIMIT <= JE_VM_ARENA_BASE,
kernel/arch/x86_64/vm_backend.c:33:_Static_assert(((JE_VM_ARENA_BASE >> 39) & 511) == 0, "arena PML4 index 0");
kernel/arch/x86_64/vm_backend.c:34:_Static_assert(((JE_VM_ARENA_BASE >> 30) & 511) == 1, "arena PDPT index 1");
kernel/arch/x86_64/vm_backend.c:35:_Static_assert(((JE_VM_ARENA_BASE >> 21) & 511) == 0, "arena PD index 0");
kernel/arch/x86_64/vm_backend.c:153:    if (frame.page >= e->pages || varena_off >= JE_VM_ARENA_SIZE || (varena_off & 4095))
kernel/arch/x86_64/vm_backend.c:186:    if (process >= JE_VM_MAX_PROCESSES || varena_off >= JE_VM_ARENA_SIZE || (varena_off & 4095))
kernel/arch/x86_64/vm_backend.c:244:        varena_off >= JE_VM_ARENA_SIZE) return JE_ERR_INVALID;
kernel/core/vm.c:85:        if (seg.target_offset > JE_VM_ARENA_SIZE) return JE_ERR_INVALID;
kernel/core/vm.c:86:        if (seg.memory_size > JE_VM_ARENA_SIZE - seg.target_offset)
kernel/core/syscall.c:179:static bool current_vm_index(je_vm **out_vm, uint32_t *out_index)
kernel/core/syscall.c:234:    if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return; }
kernel/core/syscall.c:385:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:467:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:482:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:524:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:537:                uint64_t now = je_sched_ticks();
kernel/core/uframe.c:29:bool je_uframe_check(const je_trap_frame *f, uint64_t base, uint64_t size,
kernel/core/main.c:35:    uint64_t before=je_sched_ticks();
kernel/core/main.c:36:    if (je_thread_sleep(3+id)!=JE_OK || je_sched_ticks()-before<3+id)
kernel/core/main.c:170:    volatile uint64_t *hb=(volatile uint64_t *)(uintptr_t)(JE_VM_ARENA_BASE+JE_PAGE_SIZE);
kernel/core/main.c:183:    volatile uint64_t *p=(volatile uint64_t *)(uintptr_t)(JE_VM_ARENA_BASE+off);
kernel/core/main.c:198:    volatile uint64_t *p=(volatile uint64_t *)(uintptr_t)(JE_VM_ARENA_BASE+off);
kernel/core/main.c:266:    je_arch_console_hex(je_sched_ticks());je_arch_console_write("\r\n");
kernel/core/main.c:288:        volatile unsigned char *p=(volatile unsigned char *)(uintptr_t)JE_VM_ARENA_BASE;
kernel/core/main.c:295:        volatile unsigned char *entry=(volatile unsigned char *)(uintptr_t)JE_VM_ARENA_BASE;
kernel/core/main.c:298:        volatile unsigned char *data=(volatile unsigned char *)(uintptr_t)(JE_VM_ARENA_BASE+4u*JE_PAGE_SIZE);
kernel/core/main.c:303:        volatile unsigned char *code=(volatile unsigned char *)(uintptr_t)JE_VM_ARENA_BASE;
kernel/core/main.c:391:        if (Av!=14u || Ac!=0x33u || Acr2!=(JE_VM_ARENA_BASE+3u*JE_PAGE_SIZE))
kernel/core/main.c:530:                volatile unsigned char *srcp=(volatile unsigned char *)(uintptr_t)(JE_VM_ARENA_BASE+0x1100);
kernel/core/main.c:554:                volatile unsigned char *dstp=(volatile unsigned char *)(uintptr_t)(JE_VM_ARENA_BASE+0x1180);
kernel/core/usercopy.c:69:    if (addr < JE_VM_ARENA_BASE) return 0;
kernel/core/usercopy.c:70:    if (end > JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE) return 0;
kernel/core/usercopy.c:80:    uint64_t first = (addr - JE_VM_ARENA_BASE) & ~UINT64_C(0xfff);
kernel/core/usercopy.c:81:    uint64_t last  = (addr + len - 1 - JE_VM_ARENA_BASE) & ~UINT64_C(0xfff);
kernel/core/usercopy.c:197:    uint64_t off = uaddr - JE_VM_ARENA_BASE, phys; je_vm_perm perm; int present;
kernel/core/sched.c:52: * BAD_RETURN: a user frame failed je_uframe_check (invalid return state). */
kernel/core/sched.c:129:    f->rip = JE_VM_ARENA_BASE + p->entry;
kernel/core/sched.c:130:    f->rsp = JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE;
kernel/core/sched.c:173:            if (!je_uframe_check(threads[next].frame, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
kernel/core/sched.c:271: * checks) PLUS a deliberate pre-park je_uframe_check: a thread that would park MUST
kernel/core/sched.c:287:    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
kernel/core/sched.c:363: * return must NOT skip je_uframe_check (a user can corrupt its own RSP/RFLAGS
kernel/core/sched.c:373:    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
kernel/core/sched.c:423:uint64_t je_sched_ticks(void) { return je_timer_ticks; }
include/jeios/uframe.h:30:bool je_uframe_check(const je_trap_frame *f, uint64_t base, uint64_t size,
include/jeios/vm.h:34:/* User arena is the half-open range [JE_VM_ARENA_BASE, +JE_VM_ARENA_SIZE). */
include/jeios/vm.h:35:#define JE_VM_ARENA_BASE   UINT64_C(0x40000000)
include/jeios/vm.h:36:#define JE_VM_ARENA_SIZE   UINT64_C(0x00200000) /* 2 MiB */
include/jeios/vm.h:37:#define JE_VM_ARENA_PAGES  (JE_VM_ARENA_SIZE / JE_PAGE_SIZE) /* 512 */
include/jeios/vm.h:41:#define JE_VM_STACK_OFFSET (JE_VM_ARENA_SIZE - JE_VM_STACK_PAGES * JE_PAGE_SIZE) /* 0x1fc000 */
include/jeios/sched.h:64:uint64_t je_sched_ticks(void);
tests/test_vm.c:254:    image.segments[0].target_offset = JE_VM_ARENA_SIZE;
tests/vm_fake.c:166:    return off < JE_VM_ARENA_SIZE && (off & (JE_PAGE_SIZE - 1u)) == 0;
tests/test_uframe.c:25:    assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:29:    f = good(); f.rip = BASE;        assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:30:    f = good(); f.rip = BASE - 1;    assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:31:    f = good(); f.rip = TOP - 1;     assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:32:    f = good(); f.rip = TOP;         assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:35:    f = good(); f.rsp = TOP;         assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:36:    f = good(); f.rsp = TOP + 1;     assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:37:    f = good(); f.rsp = BASE;        assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:38:    f = good(); f.rsp = BASE + 1;    assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:41:    f = good(); f.cs = 0x08;         assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:42:    f = good(); f.cs = 0x30;         assert(!je_uframe_check(&f, BASE, SIZE, &out)); /* RPL0 */
tests/test_uframe.c:43:    f = good(); f.ss = 0x10;         assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:44:    f = good(); f.ss = 0x28;         assert(!je_uframe_check(&f, BASE, SIZE, &out)); /* RPL0 */
tests/test_uframe.c:47:    f = good(); f.rip = UINT64_C(0x0000800000000000); assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:48:    f = good(); f.rsp = UINT64_C(0xdead000000000000); assert(!je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:53:    assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:61:    assert(je_uframe_check(&f, BASE, SIZE, &out));
tests/test_uframe.c:73:    f = good(); assert(!je_uframe_check(&f, BASE, 0, &out));         /* size 0 */
tests/test_uframe.c:74:    f = good(); assert(!je_uframe_check(NULL, BASE, SIZE, &out));    /* null frame */
tests/test_uframe.c:75:    f = good(); assert(!je_uframe_check(&f, BASE, SIZE, NULL));      /* null out */

 succeeded in 0ms:
32	0	AGENTS.md
11	0	CONTRIBUTING.md
16	0	docs/HANDOFF.md
65	0	docs/IMPLEMENTATION.md
163	8	docs/adr/0009-m3c-capability-ipc.md
1	0	docs/adr/debates/README.md
238	0	docs/adr/debates/m3c-step6-mechanism.md
19	1	include/jeios/abi.h
100	1	include/jeios/endpoint.h
21	0	include/jeios/endpoint_kern.h
15	0	include/jeios/sched.h
34	0	kernel/arch/x86_64/user_demo.S
169	22	kernel/core/endpoint.c
120	4	kernel/core/endpoint_kern.c
38	1	kernel/core/main.c
141	3	kernel/core/sched.c
152	3	kernel/core/syscall.c
67	0	tests/test_endpoint.c
5	2	tests/test_syscall.c
diff --git a/kernel/core/endpoint.c b/kernel/core/endpoint.c
index 0e25150..3393e22 100644
--- a/kernel/core/endpoint.c
+++ b/kernel/core/endpoint.c
@@ -241,6 +241,119 @@ static bool rec_in_pool(const je_endpoint_pool *pool, const je_ep_channel *rec)
     return ((p - base) % sizeof(pool->rec[0])) == 0u;         /* slot-aligned */
 }
 
+/* ---- M3c D7/A6 blocking-recv waiter primitives (PURE) ---- */
+
+/* True iff this end's inbound head is reserved (claimed by a WOKEN waiter). A
+ * reserved head is NOT deliverable to any other receiver (A6-5). */
+static bool head_reserved(const je_ep_end *e)
+{
+    return e->waiter.present && e->waiter.head_reserved;
+}
+
+je_ep_status je_endpoint_recv_entry_probe(je_endpoint_pool *pool, je_handle_table *t,
+                                          je_handle h)
+{
+    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
+    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
+    if (rs != JE_EP_OK) return rs;
+    uint32_t rights = 0u;
+    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
+                                              JE_EP_RIGHT_RECV, &rights);
+    if (gr != JE_HANDLE_OK) return hstat(gr);
+    je_ep_end *e = &rec->end[side];
+    /* A head reserved by an EXISTING waiter is not available to this probe: it is
+     * delivered only through that waiter's continuation (A6-5). Treat it as empty
+     * for this caller. */
+    bool deliverable = (e->inbound.count > 0u) && !head_reserved(e);
+    if (deliverable) return JE_EP_OK;   /* caller proceeds to the real peek */
+    return (e->state == JE_EP_END_PEER_CLOSED) ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
+}
+
+je_ep_status je_endpoint_wait_register(je_endpoint_pool *pool, je_handle_table *t,
+                                       je_handle h, uint32_t waiter_id,
+                                       je_ep_channel **out_rec, uint8_t *out_side)
+{
+    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
+    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
+    if (rs != JE_EP_OK) return rs;
+    uint32_t rights = 0u;
+    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
+                                              JE_EP_RIGHT_RECV, &rights);
+    if (gr != JE_HANDLE_OK) return hstat(gr);
+    je_ep_end *e = &rec->end[side];
+    if (e->waiter.present) return JE_EP_WAITERS_BUSY;   /* A6-4 per-end bound */
+    /* A waiter holds one inflight ref on its own end (keeps the record from
+     * freeing while parked; released by wait_release). Bounds-checked. */
+    if (e->inflight_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
+    e->inflight_refs++;
+    e->waiter.present = true;
+    e->waiter.waiter_id = waiter_id;
+    e->waiter.cause = JE_EP_WAIT_WAITING;
+    e->waiter.head_reserved = false;
+    if (out_rec) *out_rec = rec;
+    if (out_side) *out_side = side;
+    return JE_EP_OK;
+}
+
+je_ep_arb_result je_endpoint_wait_arbitrate(je_ep_channel *rec, uint8_t side,
+                                            je_ep_wait_cause cause,
+                                            uint32_t *out_waiter_id)
+{
+    if (!rec || side > 1u) return JE_EP_ARB_LOST;
+    je_ep_end *e = &rec->end[side];
+    /* The single terminal CAS: only a present WAITING waiter transitions, once. */
+    if (!e->waiter.present || e->waiter.cause != JE_EP_WAIT_WAITING)
+        return JE_EP_ARB_LOST;
+    if (cause == JE_EP_WAIT_WAITING || cause == JE_EP_WAIT_NONE)
+        return JE_EP_ARB_LOST;   /* not a terminal cause */
+    e->waiter.cause = cause;
+    if (cause == JE_EP_WAIT_WOKEN) {
+        /* Reserve the current inbound head for this waiter (A6-5). There must be a
+         * message to reserve — the caller (enqueue / entry drain) only arbitrates
+         * WOKEN when one is queued. */
+        e->waiter.head_reserved = (e->inbound.count > 0u);
+    }
+    if (out_waiter_id) *out_waiter_id = e->waiter.waiter_id;
+    return JE_EP_ARB_WON;
+}
+
+je_ep_wait_cause je_endpoint_wait_cause(const je_ep_channel *rec, uint8_t side)
+{
+    if (!rec || side > 1u) return JE_EP_WAIT_NONE;
+    const je_ep_end *e = &rec->end[side];
+    return e->waiter.present ? e->waiter.cause : JE_EP_WAIT_NONE;
+}
+
+/* Internal: clear a present waiter on end e owned by `who`, releasing its inflight
+ * ref and un-reserving any held head. Idempotent via the ownership check. */
+static bool waiter_release_end(je_ep_end *e, uint32_t who)
+{
+    if (!e->waiter.present || e->waiter.waiter_id != who) return false;
+    e->waiter.present = false;
+    e->waiter.cause = JE_EP_WAIT_NONE;
+    e->waiter.head_reserved = false;   /* head stays enqueued, deliverable again */
+    e->waiter.waiter_id = 0u;
+    if (e->inflight_refs != 0u) e->inflight_refs--;   /* the waiter's own ref */
+    return true;
+}
+
+bool je_endpoint_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who)
+{
+    if (!rec || side > 1u) return false;
+    return waiter_release_end(&rec->end[side], who);
+}
+
+bool je_endpoint_wait_release_by_id(je_endpoint_pool *pool, uint32_t who)
+{
+    if (!pool) return false;
+    for (uint16_t i = 0; i < JE_EP_POOL_CAP; ++i) {
+        if (!pool->rec[i].allocated) continue;
+        for (uint8_t s = 0; s < 2u; ++s)
+            if (waiter_release_end(&pool->rec[i].end[s], who)) return true;
+    }
+    return false;
+}
+
 je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
                                          je_ep_channel *rec, uint8_t side)
 {
@@ -514,9 +627,12 @@ je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
     if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if RECV not held */
 
     je_ep_queue *q = &rec->end[side].inbound;    /* this end's own inbound */
-    if (q->count == 0u) {
-        /* Empty: EOF if the peer is gone, else would-block EMPTY. */
-        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
+    /* A head reserved by a blocking waiter is delivered only through that waiter's
+     * continuation (A6-5); a concurrent handle-based poll must NOT take it. Treat
+     * it as empty for this caller. */
+    if (q->count == 0u || head_reserved(&rec->end[side])) {
+        /* Empty (or head reserved): EOF if the peer is gone, else would-block. */
+        return (rec->end[side].state == JE_EP_END_PEER_CLOSED && q->count == 0u)
                ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
     }
     uint8_t mlen = q->len[q->head];
@@ -670,6 +786,34 @@ je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *
     return JE_EP_OK;
 }
 
+/* Core peek-with-caps on a resolved end (rec,side), no handle/rights/reserved-head
+ * checks — the callers apply those. Copies head bytes+caps into the out buffers or
+ * returns SHORT_BUFFER (nothing copied, not dequeued). */
+static je_ep_status peek_caps_at(je_ep_channel *rec, uint8_t side,
+                                 void *out_kbuf, size_t cap, size_t *out_len,
+                                 uint8_t hcap, je_ep_capdesc *out_caps,
+                                 uint8_t *out_ncaps, je_ep_recv_lease *lease)
+{
+    je_ep_queue *q = &rec->end[side].inbound;
+    if (q->count == 0u)
+        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
+               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
+    uint8_t mlen = q->len[q->head];
+    uint8_t mcaps = q->cap_count[q->head];
+    *out_len = mlen;
+    *out_ncaps = mcaps;
+    if (cap < mlen || hcap < mcaps) return JE_EP_SHORT_BUFFER;
+    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;
+    if (mcaps != 0u && !out_caps) return JE_EP_INVALID;
+    for (uint8_t b = 0; b < mlen; ++b) ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
+    for (uint8_t c = 0; c < mcaps; ++c) out_caps[c] = q->cap[q->head][c];
+    if (lease) {
+        lease->rec = rec; lease->side = side;
+        lease->generation = rec->generation; lease->head = q->head; lease->len = mlen;
+    }
+    return JE_EP_OK;
+}
+
 je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
                                         je_handle h, void *out_kbuf, size_t cap,
                                         size_t *out_len, uint8_t hcap,
@@ -687,28 +831,31 @@ je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table
     je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                               JE_EP_RIGHT_RECV, &rights);
     if (gr != JE_HANDLE_OK) return hstat(gr);
-
-    je_ep_queue *q = &rec->end[side].inbound;
-    if (q->count == 0u)
+    /* A head reserved by a blocking waiter is not available to a handle-based
+     * peek (A6-5); treat as empty (EOF only if also peer-closed). */
+    if (head_reserved(&rec->end[side]) && rec->end[side].inbound.count > 0u)
         return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
                ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
+    return peek_caps_at(rec, side, out_kbuf, cap, out_len, hcap, out_caps, out_ncaps, lease);
+}
 
-    uint8_t mlen = q->len[q->head];
-    uint8_t mcaps = q->cap_count[q->head];
-    *out_len = mlen;
-    *out_ncaps = mcaps;
-    /* Short EITHER buffer -> SHORT_BUFFER, copy nothing, no dequeue (report both
-     * required sizes via out_len / out_ncaps). */
-    if (cap < mlen || hcap < mcaps) return JE_EP_SHORT_BUFFER;
-    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;
-    if (mcaps != 0u && !out_caps) return JE_EP_INVALID;
-    for (uint8_t b = 0; b < mlen; ++b) ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
-    for (uint8_t c = 0; c < mcaps; ++c) out_caps[c] = q->cap[q->head][c];
-    if (lease) {
-        lease->rec = rec; lease->side = side;
-        lease->generation = rec->generation; lease->head = q->head; lease->len = mlen;
-    }
-    return JE_EP_OK;
+/* Owned-end peek-with-caps for a resumed WOKEN waiter (A6-5): by rec/side, no
+ * handle resolution, bypasses the reserved-head guard (this IS the reserver).
+ * CORRUPT if there is no reserved head. */
+je_ep_status je_endpoint_recv_peek_owned_caps(je_endpoint_pool *pool, je_ep_channel *rec,
+                                              uint8_t side, void *out_kbuf, size_t cap,
+                                              size_t *out_len, uint8_t hcap,
+                                              je_ep_capdesc *out_caps, uint8_t *out_ncaps,
+                                              je_ep_recv_lease *lease)
+{
+    if (!out_len || !out_ncaps) return JE_EP_INVALID;
+    *out_len = 0u; *out_ncaps = 0u;
+    if (!pool || !rec || side > 1u || !rec_in_pool(pool, rec) || !rec->allocated)
+        return JE_EP_CORRUPT;
+    je_ep_end *e = &rec->end[side];
+    if (!e->waiter.present || !e->waiter.head_reserved || e->inbound.count == 0u)
+        return JE_EP_CORRUPT;
+    return peek_caps_at(rec, side, out_kbuf, cap, out_len, hcap, out_caps, out_ncaps, lease);
 }
 
 je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
diff --git a/kernel/core/endpoint_kern.c b/kernel/core/endpoint_kern.c
index 4169f16..cc0a99d 100644
--- a/kernel/core/endpoint_kern.c
+++ b/kernel/core/endpoint_kern.c
@@ -7,6 +7,7 @@
  * the arch IRQ primitives and the kernel pool freely.
  */
 #include <jeios/endpoint_kern.h>
+#include <jeios/sched.h>   /* je_sched_wake_ipc for the blocking-recv bridge */
 #include <jeios/arch.h>
 
 /* The one kernel-wide pool. A dying process's endpoints are reclaimed into it by
@@ -19,6 +20,8 @@ static void kep_init_once(void)
 {
     if (!g_kep_inited) { je_endpoint_pool_init(&g_kep); g_kep_inited = true; }
 }
+/* Forward decl: peer-close wake helper (defined with the bridge functions). */
+static void kern_peer_close_wake(je_ep_channel *rec, uint8_t side);
 
 /* Teardown hook: the release walk over the dying process's table. A negative
  * return is an invariant violation (pool corruption) and MUST stop the kernel
@@ -69,7 +72,16 @@ je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h)
 {
     if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
     kep_init_once();
-    return je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
+    /* Capture the end being closed BEFORE the close invalidates the handle, so we
+     * can wake a receiver blocked on the PEER end if this close peer-closes it
+     * (A6-PEERWAKE). A resolve failure just means the normal close path reports it. */
+    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
+    bool have = (je_endpoint_resolve(&g_kep, &vm->procs[index].htable, h, &rec, &side)
+                 == JE_EP_OK);
+    je_ep_status s = je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
+    if (s == JE_EP_OK && have)

exec
/usr/bin/bash -lc "nl -ba kernel/core/uframe.c | sed -n '1,180p'; nl -ba kernel/core/syscall.c | sed -n '176,205p'; nl -ba kernel/core/sched.c | sed -n '390,445p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -RIn \"je_endpoint_recv_peek_caps\\|recv_entry_probe\\|WAITERS_BUSY\\|WAIT_CANCEL\\|wait_cancel\\|BLOCKED_IPC\\|wait_pending\" . --exclude-dir=.git --exclude='*.o' --exclude='*.iso' --exclude='*.elf' --exclude='*.pyc'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -RIn \"WOKEN\\|TIMED_OUT\\|PEER_CLOSED\\|CANCELLED\\|reserved\" docs/adr/0009-m3c-capability-ipc.md docs/adr/debates/m3c-step6-mechanism.md docs/IMPLEMENTATION.md AGENTS.md | head -240" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc 'make check' in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
./include/jeios/endpoint_kern.h:89:je_ep_status je_endpoint_kern_recv_entry_probe(je_vm *vm, uint32_t index, je_handle h);
./include/jeios/endpoint_kern.h:96:bool je_endpoint_kern_wait_cancel(uint32_t slot);
./include/jeios/endpoint.h:107:    JE_EP_WAITERS_BUSY, /* M3c A6-4: the end's single waiter slot is occupied; the
./include/jeios/endpoint.h:151:    JE_EP_WAIT_CANCELLED   /* internal cancel (A6-2; no ABI trigger in v1) */
./include/jeios/endpoint.h:364:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
./include/jeios/endpoint.h:432:je_ep_status je_endpoint_recv_entry_probe(je_endpoint_pool *pool, je_handle_table *t,
./include/jeios/endpoint.h:436: * JE_EP_QUEUE_FULL-equivalent (returns JE_EP_WAITERS_BUSY) if the end already has
./include/jeios/sched.h:39: * (guarded BLOCKED_IPC->READY). See ADR 0009 A6-CLARIFY-1/STRUCT. */
./include/jeios/sched.h:43:/* M3c A6: continuation-prologue accessors (syscall.c). wait_pending reads back the
./include/jeios/sched.h:46:bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side, uint64_t *rdi,
./docs/IMPLEMENTATION.md:900:(`THREAD_BLOCKED_IPC → READY`). One pure `je_endpoint_wait_arbitrate` CAS is the
./docs/IMPLEMENTATION.md:906:`BLOCKED_IPC→READY` only — never resurrects DYING/ZOMBIE).
./docs/IMPLEMENTATION.md:910:(`je_endpoint_wait_register`, A6-4 one-per-end → `WAITERS_BUSY` maps to a
./docs/IMPLEMENTATION.md:914:**rewind `f->rip` by `JE_INT80_LEN=2`** (`CD 80`), set `BLOCKED_IPC`, switch. On
./docs/IMPLEMENTATION.md:916:top of `je_syscall_dispatch` (keyed on `wait_pending`, asserts the restart RIP)
./docs/IMPLEMENTATION.md:943:masking, A6-4 WAITERS_BUSY, release idempotence, A6-CLARIFY-2 release-returns-head).
./docs/adr/0009-m3c-capability-ipc.md:210:tested; a blocked thread enters a new `THREAD_BLOCKED_IPC` scheduler state.
./docs/adr/0009-m3c-capability-ipc.md:265:6. **Waits:** `THREAD_BLOCKED_IPC` + deadline + cancellation (D7). Edits `sched.c`
./docs/adr/0009-m3c-capability-ipc.md:719:(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
./docs/adr/0009-m3c-capability-ipc.md:854:an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
./docs/adr/0009-m3c-capability-ipc.md:882:`THREAD_BLOCKED_IPC`, and does NOT `yield` — so the inclusive `wake <= ticks` ISR
./docs/adr/0009-m3c-capability-ipc.md:941:`cr3_load`. The scheduler flip is **guarded**: `THREAD_BLOCKED_IPC → READY` **only**
./docs/adr/0009-m3c-capability-ipc.md:1008:  `deadline`, `saved_post_int_rip`, `cause`), sets `state = THREAD_BLOCKED_IPC`,
./docs/adr/0009-m3c-capability-ipc.md:1014:  `BLOCKED_IPC→READY` transition) flips the thread READY. When the scheduler next
./docs/adr/0009-m3c-capability-ipc.md:1079:3. The guarded scheduler flip (A6-5: `THREAD_BLOCKED_IPC → READY` only) means even
./docs/adr/0009-m3c-capability-ipc.md:1172:- **CANCELLED:** the internal `je_ipc_wait_cancel(waiter)` helper (A6-2, no ABI
./docs/adr/0009-m3c-capability-ipc.md:1202:genuinely parks (`BLOCKED_IPC`), the timer ISR advances `je_timer_ticks` past the
./docs/adr/0009-m3c-capability-ipc.md:1210:masking**, **A6-4 WAITERS_BUSY**, and **A6-CLARIFY-2 release-returns-head** are
./docs/adr/debates/m3c-step6-waits.md:20:- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.
./docs/adr/debates/m3c-step6-waits.md:38:1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
./docs/adr/debates/m3c-step6-waits.md:58:| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
./docs/adr/debates/m3c-step6-waits.md:60:| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
./docs/adr/debates/m3c-step6-waits.md:72:2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
./docs/adr/debates/m3c-step6-waits.md:85:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:95:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:116:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:127:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:151:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:161:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:182:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:193:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:257:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
./docs/adr/debates/m3c-step6-waits.md:268:- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
./docs/adr/debates/m3c-step6-waits.md:273:- Add `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:279:- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
./docs/adr/debates/m3c-step6-waits.md:318:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
./docs/adr/debates/m3c-step6-waits.md:329:- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
./docs/adr/debates/m3c-step6-waits.md:334:- Add `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:340:- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
./docs/adr/debates/m3c-step6-waits.md:403:- A6-5 says the wake does *"flips `THREAD_BLOCKED_IPC → READY`."* If that flip isn't guarded, a DYING thread can become READY.
./docs/adr/debates/m3c-step6-waits.md:407:  3. Make the scheduler flip conditional: `BLOCKED_IPC → READY` only, never from DYING or ZOMBIE.
./docs/adr/debates/m3c-step6-waits.md:431:- A6-5: *"The scheduler only flips `THREAD_BLOCKED_IPC → READY`"* and the continuation writes the result.
./docs/adr/debates/m3c-step6-waits.md:2263:kernel/core/endpoint.c:673:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
./docs/adr/debates/m3c-step6-waits.md:2401:kernel/core/endpoint_kern.c:171:    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
./docs/adr/debates/m3c-step6-waits.md:3567:tested; a blocked thread enters a new `THREAD_BLOCKED_IPC` scheduler state.
./docs/adr/debates/m3c-step6-waits.md:3622:6. **Waits:** `THREAD_BLOCKED_IPC` + deadline + cancellation (D7). Edits `sched.c`
./docs/adr/debates/m3c-step6-waits.md:3798:- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.
./docs/adr/debates/m3c-step6-waits.md:3816:1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
./docs/adr/debates/m3c-step6-waits.md:3836:| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
./docs/adr/debates/m3c-step6-waits.md:3838:| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
./docs/adr/debates/m3c-step6-waits.md:3850:2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
./docs/adr/debates/m3c-step6-waits.md:3863:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:3873:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:3894:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:3905:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:3929:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:3939:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:3960:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:3971:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:4035:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
./docs/adr/debates/m3c-step6-waits.md:4067:/usr/bin/bash -lc "grep -RInE \"JE_EP_MAX_WAITERS|MAX_WAIT|WAITING|BLOCKED_IPC|CANCELLED|TIMED_OUT|waiter|reservation|continuation|recv\" docs include kernel tests | sed -n '1,520p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
./docs/adr/debates/m3c-step6-waits.md:4752:+(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
./docs/adr/debates/m3c-step6-waits.md:4806:+an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
./docs/adr/debates/m3c-step6-waits.md:4872:+`cr3_load`. The scheduler only flips `THREAD_BLOCKED_IPC → READY`. A copy fault in
./docs/adr/debates/m3c-step6-waits.md:4922:+- **CANCELLED:** internal `je_ipc_wait_cancel` → exactly one `CANCELLED`.
./docs/adr/debates/m3c-step6-waits.md:4947:- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.
./docs/adr/debates/m3c-step6-waits.md:4965:1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
./docs/adr/debates/m3c-step6-waits.md:4985:| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
./docs/adr/debates/m3c-step6-waits.md:4987:| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
./docs/adr/debates/m3c-step6-waits.md:4999:2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
./docs/adr/debates/m3c-step6-waits.md:5012:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:5022:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:5043:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:5054:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:5078:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:5088:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:5109:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:5120:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:5184:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
./docs/adr/debates/m3c-step6-waits.md:5433:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
./docs/adr/debates/m3c-step6-waits.md:5676:    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
./docs/adr/debates/m3c-step6-waits.md:6081:docs/adr/0009-m3c-capability-ipc.md:210:tested; a blocked thread enters a new `THREAD_BLOCKED_IPC` scheduler state.
./docs/adr/debates/m3c-step6-waits.md:6084:docs/adr/0009-m3c-capability-ipc.md:265:6. **Waits:** `THREAD_BLOCKED_IPC` + deadline + cancellation (D7). Edits `sched.c`
./docs/adr/debates/m3c-step6-waits.md:6101:docs/adr/0009-m3c-capability-ipc.md:712:(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
./docs/adr/debates/m3c-step6-waits.md:6114:docs/adr/0009-m3c-capability-ipc.md:766:an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
./docs/adr/debates/m3c-step6-waits.md:6127:docs/adr/0009-m3c-capability-ipc.md:832:`cr3_load`. The scheduler only flips `THREAD_BLOCKED_IPC → READY`. A copy fault in
./docs/adr/debates/m3c-step6-waits.md:6139:docs/adr/0009-m3c-capability-ipc.md:882:- **CANCELLED:** internal `je_ipc_wait_cancel` → exactly one `CANCELLED`.
./docs/adr/debates/m3c-step6-waits.md:6154:docs/adr/debates/m3c-step6-waits.md:20:- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.
./docs/adr/debates/m3c-step6-waits.md:6160:docs/adr/debates/m3c-step6-waits.md:38:1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
./docs/adr/debates/m3c-step6-waits.md:6162:docs/adr/debates/m3c-step6-waits.md:58:| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
./docs/adr/debates/m3c-step6-waits.md:6164:docs/adr/debates/m3c-step6-waits.md:60:| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
./docs/adr/debates/m3c-step6-waits.md:6167:docs/adr/debates/m3c-step6-waits.md:72:2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
./docs/adr/debates/m3c-step6-waits.md:6172:docs/adr/debates/m3c-step6-waits.md:85:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:6175:docs/adr/debates/m3c-step6-waits.md:95:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:6181:docs/adr/debates/m3c-step6-waits.md:116:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:6186:docs/adr/debates/m3c-step6-waits.md:127:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:6194:docs/adr/debates/m3c-step6-waits.md:151:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
./docs/adr/debates/m3c-step6-waits.md:6197:docs/adr/debates/m3c-step6-waits.md:161:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
./docs/adr/debates/m3c-step6-waits.md:6203:docs/adr/debates/m3c-step6-waits.md:182:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:6208:docs/adr/debates/m3c-step6-waits.md:193:- add `THREAD_BLOCKED_IPC`;
./docs/adr/debates/m3c-step6-waits.md:6237:docs/adr/debates/m3c-step6-waits.md:257:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
./docs/adr/debates/m3c-step6-waits.md:6241:docs/adr/debates/m3c-step6-waits.md:268:- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
./docs/adr/debates/m3c-step6-waits.md:6245:docs/adr/debates/m3c-step6-waits.md:273:- Add `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:6248:docs/adr/debates/m3c-step6-waits.md:279:- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
./docs/adr/debates/m3c-step6-waits.md:6269:docs/adr/debates/m3c-step6-waits.md:318:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
./docs/adr/debates/m3c-step6-waits.md:6273:docs/adr/debates/m3c-step6-waits.md:329:- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
./docs/adr/debates/m3c-step6-waits.md:6277:docs/adr/debates/m3c-step6-waits.md:334:- Add `THREAD_BLOCKED_IPC`.
./docs/adr/debates/m3c-step6-waits.md:6280:docs/adr/debates/m3c-step6-waits.md:340:- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
./docs/adr/debates/m3c-step6-waits.md:6337:include/jeios/endpoint.h:333:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
./docs/adr/debates/m3c-step6-waits.md:6361:kernel/core/endpoint.c:673:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
./docs/adr/debates/m3c-step6-waits.md:6379:kernel/core/endpoint_kern.c:171:    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
./docs/adr/debates/m3c-step6-waits.md:6448:tests/test_endpoint.c:510:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
./docs/adr/debates/m3c-step6-waits.md:6452:tests/test_endpoint.c:547:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
./docs/adr/debates/m3c-step6-waits.md:6455:tests/test_endpoint.c:603:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,1,rcv,&nc,&lz)==JE_EP_SHORT_BUFFER);
./docs/adr/debates/m3c-step6-waits.md:6456:tests/test_endpoint.c:606:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rcv,&nc,&lz)==JE_EP_OK);
./docs/adr/debates/m3c-step6-waits.md:6471:   > “The scheduler only flips `THREAD_BLOCKED_IPC → READY`.”
./docs/adr/debates/m3c-step6-waits.md:6550:   > “The scheduler only flips `THREAD_BLOCKED_IPC → READY`.”
./docs/adr/debates/m3c-step6-waits.md:6634:5. **FIXED.** All three are present: the guarded BLOCKED_IPC→READY flip, the dying process's own waiter unlinked first, and arbitrate skipping tombstoned waiters.
./docs/adr/debates/m3c-step6-mechanism.md:37:  - Add a `BLOCKED_IPC` state and waiter fields to `thread`: `end_ref`, `side`, `deadline`, `cause`, `pending`.
./docs/adr/debates/m3c-step6-mechanism.md:38:  - Add `je_sched_block_current(f)`. It asserts the thread is a user thread, that `f` is at `rsp0_top - sizeof`, and that the arena bound holds. It sets `rip -= JE_X86_INT80_LEN` and `state = BLOCKED_IPC`, then calls `return je_sched_interrupt(f,false)`. This works unchanged: `je_sched_interrupt` only flips RUNNING→READY, so a BLOCKED_IPC thread stays parked.
./docs/adr/debates/m3c-step6-mechanism.md:39:  - In `je_sched_interrupt`'s existing wake loop, add one clause: a `BLOCKED_IPC` thread past its deadline goes through `je_ipc_wait_arbitrate(TIMED_OUT)`, then the guarded flip to READY.
./docs/adr/debates/m3c-step6-mechanism.md:40:  - Add `je_sched_ipc_wake(slot)`, the guarded BLOCKED_IPC→READY flip used by sender and close.
./docs/adr/debates/m3c-step6-mechanism.md:61:2. A scheduler helper validates the fixed frame, saves the owned end reference and recv arguments, rewinds `rip` by the named `INT80` length, marks `BLOCKED_IPC`, and calls the existing scheduler return path.
./docs/adr/debates/m3c-step6-mechanism.md:62:3. Wake arbitration stamps the cause, reserves the head on `WOKEN`, then performs the guarded `BLOCKED_IPC -> READY` transition.
./docs/adr/debates/m3c-step6-mechanism.md:76:2. A scheduler helper validates the fixed frame, saves the owned end reference and recv arguments, rewinds `rip` by the named `INT80` length, marks `BLOCKED_IPC`, and calls the existing scheduler return path.
./docs/adr/debates/m3c-step6-mechanism.md:77:3. Wake arbitration stamps the cause, reserves the head on `WOKEN`, then performs the guarded `BLOCKED_IPC -> READY` transition.
./docs/adr/debates/m3c-step6-mechanism.md:114:   - Add `je_sched_block_current(f)`, modelled exactly on `je_sched_exit_current` (sched.c:213–219): same frame-identity checks, set `state = BLOCKED_IPC` instead of `mark_dying`, rewind `f->rip`, return `select_and_install_next()`.
./docs/adr/debates/m3c-step6-mechanism.md:115:   - `je_sched_interrupt` (sched.c:184) only demotes RUNNING threads to READY, so a BLOCKED_IPC thread is left alone.
./docs/adr/debates/m3c-step6-mechanism.md:116:   - The only change to `je_sched_interrupt` is that the timer pass also stamps TIMED_OUT on BLOCKED_IPC threads (through the A6 CAS), next to the existing SLEEPING sweep.
./docs/adr/debates/m3c-step6-mechanism.md:129:  - add the BLOCKED_IPC timeout stamp in the timer pass
./docs/adr/debates/m3c-step6-mechanism.md:130:  - add a guarded `je_sched_wake_ipc(slot)` (state BLOCKED_IPC → READY)
./docs/adr/debates/m3c-step6-mechanism.md:154:Claude also missed that `je_sched_wake_ipc()` must be only a guarded wrapper around the single A6 `je_ipc_wait_arbitrate` transition; it must not introduce an independent `BLOCKED_IPC → READY` race.
./docs/adr/debates/m3c-step6-mechanism.md:169:Claude also missed that `je_sched_wake_ipc()` must be only a guarded wrapper around the single A6 `je_ipc_wait_arbitrate` transition; it must not introduce an independent `BLOCKED_IPC → READY` race.
./docs/HANDOFF.md:85:  `je_uframe_check` + 2-byte `int 0x80` rewind), a `wait_pending` continuation
./docs/HANDOFF.md:88:  `BLOCKED_IPC→READY` wake, one idempotent `je_endpoint_wait_release` owner (called
./kernel/core/endpoint.c:253:je_ep_status je_endpoint_recv_entry_probe(je_endpoint_pool *pool, je_handle_table *t,
./kernel/core/endpoint.c:284:    if (e->waiter.present) return JE_EP_WAITERS_BUSY;   /* A6-4 per-end bound */
./kernel/core/endpoint.c:817:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
./kernel/core/syscall.c:215:    case JE_EP_WAITERS_BUSY: return JE_ABI_H_EMPTY; /* A6-4: occupied slot -> non-blocking EMPTY */
./kernel/core/syscall.c:244:    case JE_EP_WAIT_CANCELLED:
./kernel/core/syscall.c:332:        if (je_sched_wait_pending(&wrec, &wside, &wrdi, &wrsi, &wrdx, &wr8, &wr9, &wprip)) {
./kernel/core/syscall.c:534:            je_ep_status probe = je_endpoint_kern_recv_entry_probe(vm, index, h);
./kernel/core/syscall.c:547:                if (reg == JE_EP_WAITERS_BUSY) {   /* A6-4: slot occupied -> non-blocking EMPTY */
./kernel/core/endpoint_kern.c:187:    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
./kernel/core/endpoint_kern.c:266:je_ep_status je_endpoint_kern_recv_entry_probe(je_vm *vm, uint32_t index, je_handle h)
./kernel/core/endpoint_kern.c:270:    return je_endpoint_recv_entry_probe(&g_kep, &vm->procs[index].htable, h);
./kernel/core/endpoint_kern.c:319:bool je_endpoint_kern_wait_cancel(uint32_t slot)
./kernel/core/endpoint_kern.c:327:            if (je_endpoint_wait_arbitrate(&g_kep.rec[i], s, JE_EP_WAIT_CANCELLED, &id)
./kernel/core/main.c:886:         * recv(h0, timeout) on an end nobody sends to -> it PARKS (BLOCKED_IPC);
./kernel/core/sched.c:16:typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING, BLOCKED_IPC } state;
./kernel/core/sched.c:44:    bool     wait_pending;
./kernel/core/sched.c:147:    threads[slot].wait_pending=false;
./kernel/core/sched.c:220:            if (threads[i].state==BLOCKED_IPC && threads[i].wait_deadline!=0u &&
./kernel/core/sched.c:228:                 * flip, BLOCKED_IPC -> READY only. */
./kernel/core/sched.c:276: * state, rewind f->rip to the int 0x80 site, set BLOCKED_IPC, and switch. The saved
./kernel/core/sched.c:296:    t->wait_pending = true;
./kernel/core/sched.c:304:    t->state = BLOCKED_IPC;
./kernel/core/sched.c:310: * BLOCKED_IPC -> READY ONLY (never from any other state), so it can never
./kernel/core/sched.c:318:    if (threads[slot].state == BLOCKED_IPC) threads[slot].state = READY;
./kernel/core/sched.c:326:bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side, uint64_t *rdi,
./kernel/core/sched.c:331:    if (!t->wait_pending) return false;
./kernel/core/sched.c:347:    threads[current].wait_pending = false;
./tests/test_endpoint.c:510:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
./tests/test_endpoint.c:547:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
./tests/test_endpoint.c:603:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,1,rcv,&nc,&lz)==JE_EP_SHORT_BUFFER);
./tests/test_endpoint.c:606:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rcv,&nc,&lz)==JE_EP_OK);
./tests/test_endpoint.c:641:        assert(je_endpoint_recv_entry_probe(&g_pool,&g_t,a1)==JE_EP_EMPTY);
./tests/test_endpoint.c:647:        /* A6-4: a second register on the same end -> WAITERS_BUSY. */
./tests/test_endpoint.c:648:        assert(je_endpoint_wait_register(&g_pool,&g_t,a1,4u,&wr,&ws)==JE_EP_WAITERS_BUSY);

 succeeded in 0ms:
docs/adr/0009-m3c-capability-ipc.md:61:- An end is in exactly one logical state: `OPEN`, `PEER_CLOSED` (the other end is
docs/adr/0009-m3c-capability-ipc.md:68:  closing an end: the peer end transitions `OPEN → PEER_CLOSED`; the closing end's
docs/adr/0009-m3c-capability-ipc.md:71:  on a `PEER_CLOSED` end **drains remaining queued messages first, then reports
docs/adr/0009-m3c-capability-ipc.md:72:  EOF**; a `send` to a `PEER_CLOSED` end fails with `PEER_CLOSED`. The channel
docs/adr/0009-m3c-capability-ipc.md:182:WAITING -> WOKEN | TIMED_OUT | CANCELLED | PEER_CLOSED
docs/adr/0009-m3c-capability-ipc.md:189:  terminal transition itself** (it must do the `WAITING→TIMED_OUT` transition and
docs/adr/0009-m3c-capability-ipc.md:193:  is eligible at once, priority is fixed: **`PEER_CLOSED` > `WOKEN` > `CANCELLED`
docs/adr/0009-m3c-capability-ipc.md:194:  > `TIMED_OUT`**, and if a message is already queued when a receiver would block,
docs/adr/0009-m3c-capability-ipc.md:195:  it does **not** block (immediate `WOKEN`-equivalent delivery). The transition is
docs/adr/0009-m3c-capability-ipc.md:237:field (and reserved flag bits), and the non-blocking-IPC PR (D-SEQ step 4) rejects
docs/adr/0009-m3c-capability-ipc.md:238:any send with `cap_count != 0` or nonzero reserved bits as `JE_ABI_INVALID`. So a
docs/adr/0009-m3c-capability-ipc.md:260:   enforced `cap_count == 0` / reserved-flags rule (D9) so no capability can be
docs/adr/0009-m3c-capability-ipc.md:359:- New `JE_ABI_H_*`/IPC statuses (`QUEUE_FULL`, `SHORT_BUFFER`, `PEER_CLOSED`,
docs/adr/0009-m3c-capability-ipc.md:433:preserved).
docs/adr/0009-m3c-capability-ipc.md:437:repeated-fault-no-exhaustion**; the *survivor observes PEER_CLOSED* case is proven
docs/adr/0009-m3c-capability-ipc.md:518:**PEER_CLOSED** target is still LIVE and **does** mint (only CLOSED is dead). This
docs/adr/0009-m3c-capability-ipc.md:535:preserved); permanently dead target → consume with handle `0`. A copy-out fault is
docs/adr/0009-m3c-capability-ipc.md:543:  close/PEER_CLOSED semantics. Biggest blast radius; deferred (see A5-1 flex).
docs/adr/0009-m3c-capability-ipc.md:584:### A5-4. Transfer ABI (the pre-reserved R8/R9 path) and the cross-process proof
docs/adr/0009-m3c-capability-ipc.md:586:**Decision + why.** Uses option (b), the path step-4 pre-reserved. **This A5-4
docs/adr/0009-m3c-capability-ipc.md:588:extension, NOT overload `try_recv`'s RDX"): step-4 reserved R8/R9 and RDX[63:32]
docs/adr/0009-m3c-capability-ipc.md:590:is to extend `try_recv` via those reserved registers rather than add a duplicate
docs/adr/0009-m3c-capability-ipc.md:592:its byte-length meaning unchanged; RDX[63:32] takes its *pre-reserved* cap-count
docs/adr/0009-m3c-capability-ipc.md:593:meaning (it was reserved-zero, never a usable field, so no step-4 caller can
docs/adr/0009-m3c-capability-ipc.md:625:PEER_CLOSED → per-cap DENIED (no TRANSFER / rights superset) → per-cap
docs/adr/0009-m3c-capability-ipc.md:677:  step-4 already reserved R8/R9 + RDX[63:32] for (b), so (b) avoids op-space growth
docs/adr/0009-m3c-capability-ipc.md:695:`WAITING→WOKEN|TIMED_OUT|CANCELLED|PEER_CLOSED`, mandatory bounded deadline, timer
docs/adr/0009-m3c-capability-ipc.md:717:**Decision + why.** The **waiter** state (`WAITING→WOKEN|TIMED_OUT|CANCELLED|
docs/adr/0009-m3c-capability-ipc.md:718:PEER_CLOSED`) is kept **distinct** from the **scheduler** state
docs/adr/0009-m3c-capability-ipc.md:724:enqueue → `WOKEN`, peer close → `PEER_CLOSED`, timer ISR deadline → `TIMED_OUT`,
docs/adr/0009-m3c-capability-ipc.md:725:internal cancel → `CANCELLED`. The parked **syscall continuation** then *validates
docs/adr/0009-m3c-capability-ipc.md:728:adversarial review showed "ISR stamps TIMED_OUT, syscall re-checks and may upgrade
docs/adr/0009-m3c-capability-ipc.md:729:to WOKEN" would make the syscall a second authority — not the single-terminal-CAS
docs/adr/0009-m3c-capability-ipc.md:737:already terminal). D7's written priority `PEER_CLOSED > WOKEN > CANCELLED >
docs/adr/0009-m3c-capability-ipc.md:738:TIMED_OUT` therefore does NOT mean "a later higher-priority cause supersedes an
docs/adr/0009-m3c-capability-ipc.md:741:before parking** — at entry only WOKEN (a queued/deliverable message) and
docs/adr/0009-m3c-capability-ipc.md:742:PEER_CLOSED (empty + peer gone) can be jointly relevant, and A6-1 drain-first
docs/adr/0009-m3c-capability-ipc.md:743:decides between them: a non-empty queue picks WOKEN and the thread never parks,
docs/adr/0009-m3c-capability-ipc.md:744:an empty closed end picks PEER_CLOSED. Once parked, there is only one snapshot per CAS and
docs/adr/0009-m3c-capability-ipc.md:775:queued message is **delivered** (`WOKEN`); `PEER_CLOSED` is returned only when the
docs/adr/0009-m3c-capability-ipc.md:776:queue is **empty**. The literal D7 priority `PEER_CLOSED > WOKEN` is read as
docs/adr/0009-m3c-capability-ipc.md:784:**Predictions if other choices were made.** *A2 (literal PEER_CLOSED > a queued
docs/adr/0009-m3c-capability-ipc.md:799:(reserved=0 today). Backward-compatible: every existing non-blocking caller passes
docs/adr/0009-m3c-capability-ipc.md:802:identical to today. A nonzero R10 opts into blocking. The four terminal causes (plus the post-WOKEN fault row) map to the
docs/adr/0009-m3c-capability-ipc.md:807:| `WOKEN` | `JE_ABI_OK`=0 | merged step-4/step-5 recv success layout: RDX[31:0]=byte len, RDX[63:32]=cap count; R8/R9 handle-out per A5-4 |
docs/adr/0009-m3c-capability-ipc.md:808:| `TIMED_OUT` | **`JE_ABI_H_TIMED_OUT`=14** (new, `_Static_assert`) | RDX=0, no handles |
docs/adr/0009-m3c-capability-ipc.md:809:| `PEER_CLOSED` | `JE_ABI_H_PEER_CLOSED`=12 (existing) | RDX=0, no handles |
docs/adr/0009-m3c-capability-ipc.md:810:| `CANCELLED` | **`JE_ABI_H_CANCELLED`=15** (new, `_Static_assert`; ABI-unreachable in v1 per A6-2, but the continuation still writes a defined status) | RDX=0, no handles |
docs/adr/0009-m3c-capability-ipc.md:811:| copy/mint fault after WOKEN | `JE_ABI_INVALID`=2 (existing) | message stays queued (A6-5) |
docs/adr/0009-m3c-capability-ipc.md:814:**Caps compose:** a blocking recv is a full recv — on `WOKEN` it runs the **same
docs/adr/0009-m3c-capability-ipc.md:826:immediate result — `WOKEN`-equiv delivery, **`SHORT_BUFFER`** (buffer or handle-out
docs/adr/0009-m3c-capability-ipc.md:827:too small), or `PEER_CLOSED` — it is returned straight away and **no waiter is
docs/adr/0009-m3c-capability-ipc.md:828:registered, no head reserved, the A6-4 occupied-slot check is not consulted**. A
docs/adr/0009-m3c-capability-ipc.md:833:into the post-WOKEN `JE_ABI_INVALID` fault bucket. The post-WOKEN `JE_ABI_INVALID`
docs/adr/0009-m3c-capability-ipc.md:835:reserved** (e.g. the user buffer became unmapped between park and resume), not for
docs/adr/0009-m3c-capability-ipc.md:841:reserved=0 for this). *Zero-timeout = immediate TIMED_OUT:* every existing R10=0
docs/adr/0009-m3c-capability-ipc.md:842:`try_recv` caller flips `EMPTY`→`TIMED_OUT` — a silent breaking change to merged,
docs/adr/0009-m3c-capability-ipc.md:843:tested behavior; rejected. *Leaving TIMED_OUT/CANCELLED unmapped (the text defect
docs/adr/0009-m3c-capability-ipc.md:851:### A6-2. CANCELLED is internal-only in v1 (B1)
docs/adr/0009-m3c-capability-ipc.md:853:**Decision + why.** The `CANCELLED` terminal state is reachable in v1 **only via
docs/adr/0009-m3c-capability-ipc.md:862:**Predictions if other choices were made.** *B2 (defer CANCELLED entirely — state
docs/adr/0009-m3c-capability-ipc.md:879:the entry snapshot once; deliver `WOKEN`-equiv / `PEER_CLOSED` / **`EMPTY`** per
docs/adr/0009-m3c-capability-ipc.md:880:A6-ABI — NOT `TIMED_OUT`). **Zero short-circuits BEFORE any waiter registration:**
docs/adr/0009-m3c-capability-ipc.md:894:*immediate* spurious `TIMED_OUT` (or, worse, never) — a silent correctness bug the
docs/adr/0009-m3c-capability-ipc.md:939:`WOKEN`) runs in the scheduler/ISR, but the **user copy-out and commit happen in
docs/adr/0009-m3c-capability-ipc.md:946:the arbitrate CAS stamps `WOKEN`, the winning path (a sender's enqueue, or the
docs/adr/0009-m3c-capability-ipc.md:949:and FIFO is preserved. The reservation is resolved by exactly one of:
docs/adr/0009-m3c-capability-ipc.md:950:- **successful commit** in the continuation → dequeue the reserved head (+ release
docs/adr/0009-m3c-capability-ipc.md:956:  is done (terminal `WOKEN`, result `JE_ABI_INVALID`); the released reservation
docs/adr/0009-m3c-capability-ipc.md:961:to one per end), so "reserve the head" is unambiguous. **A reserved head is not
docs/adr/0009-m3c-capability-ipc.md:964:reserved head as **not deliverable** and returns `EMPTY` — it does NOT take the
docs/adr/0009-m3c-capability-ipc.md:965:reserved message (that would break the reservation and FIFO) and does NOT itself
docs/adr/0009-m3c-capability-ipc.md:966:register a waiter. The reserved message is delivered only to the waiter that
docs/adr/0009-m3c-capability-ipc.md:967:reserved it, or **becomes deliverable again at its queue head** (it is never
docs/adr/0009-m3c-capability-ipc.md:971:handle (D3 owned-reference rule) — it uses the reserved owned record.
docs/adr/0009-m3c-capability-ipc.md:983:that already won `WOKEN` — the double-transition A6-STRUCT forbids; releasing the
docs/adr/0009-m3c-capability-ipc.md:1060:**peer end becomes `PEER_CLOSED`** (the merged step-3/4 state rule — the dying side
docs/adr/0009-m3c-capability-ipc.md:1061:goes CLOSED, the surviving side goes PEER_CLOSED). The walk now, in the SAME
docs/adr/0009-m3c-capability-ipc.md:1064:waking it `PEER_CLOSED` **immediately** rather than letting it wait out its
docs/adr/0009-m3c-capability-ipc.md:1066:the blocked receiver was already woken `WOKEN` when that message was enqueued and
docs/adr/0009-m3c-capability-ipc.md:1067:is not blocked anymore; `PEER_CLOSED` wake applies to a receiver blocked on an
docs/adr/0009-m3c-capability-ipc.md:1087:and a semantic one (it should learn `PEER_CLOSED` now, not `TIMED_OUT` later).
docs/adr/0009-m3c-capability-ipc.md:1092:of the surviving (PEER_CLOSED) end's:* names the wrong end — the thread to wake is
docs/adr/0009-m3c-capability-ipc.md:1093:in the living process, on the end that just became PEER_CLOSED.
docs/adr/0009-m3c-capability-ipc.md:1097:death) that transitions a peer end to PEER_CLOSED: the same arbitrate-the-peer's-
docs/adr/0009-m3c-capability-ipc.md:1100:### A6-CLARIFY-2. Death/reap releases a held WOKEN reservation (owner-approved 2026-10-19)
docs/adr/0009-m3c-capability-ipc.md:1102:**Decision + why.** A6-5 reserves the queue head for a receiver stamped `WOKEN`,
docs/adr/0009-m3c-capability-ipc.md:1108:in the window, the reserved head would be stranded (marked reserved, no live waiter
docs/adr/0009-m3c-capability-ipc.md:1116:`READY`/`WOKEN` case of that one routine. It runs in the IRQ-critical teardown
docs/adr/0009-m3c-capability-ipc.md:1118:could close an end or discard queued messages that are still reserved). The
docs/adr/0009-m3c-capability-ipc.md:1122:`WOKEN`-stamp until consume-or-release**, so A6-4 gives any concurrent second
docs/adr/0009-m3c-capability-ipc.md:1123:receiver `EMPTY` (nobody can park behind a reserved head and then sleep to its
docs/adr/0009-m3c-capability-ipc.md:1139:drafted):* the first future kill/cancel path strands the reserved head
docs/adr/0009-m3c-capability-ipc.md:1144:`WOKEN`-stamp instead of at consume/release:* a second receiver could park behind
docs/adr/0009-m3c-capability-ipc.md:1145:the reserved head and sleep to timeout — the latency bug; keeping the slot occupied
docs/adr/0009-m3c-capability-ipc.md:1146:makes A6-4 return `EMPTY` instead. *Dropping the reserved message rather than
docs/adr/0009-m3c-capability-ipc.md:1162:- **queued-before-recv:** message already present → immediate `WOKEN`, no park.
docs/adr/0009-m3c-capability-ipc.md:1163:- **WOKEN:** thread X parks on recv; thread Y sends then *explicitly yields* →
docs/adr/0009-m3c-capability-ipc.md:1164:  X resumes, exactly one `WOKEN`, FIFO delivery.
docs/adr/0009-m3c-capability-ipc.md:1167:- **TIMED_OUT:** X parks (nonzero R10); advance `je_timer_ticks` to the inclusive
docs/adr/0009-m3c-capability-ipc.md:1168:  deadline → ISR arbitrates `TIMED_OUT` (status 14), X resumes with it.
docs/adr/0009-m3c-capability-ipc.md:1169:- **PEER_CLOSED:** X (in process A) parks on an empty end whose peer is held by
docs/adr/0009-m3c-capability-ipc.md:1171:  walk arbitrates `PEER_CLOSED` (12) on A's surviving end *before* A's deadline.
docs/adr/0009-m3c-capability-ipc.md:1172:- **CANCELLED:** the internal `je_ipc_wait_cancel(waiter)` helper (A6-2, no ABI
docs/adr/0009-m3c-capability-ipc.md:1173:  path) → exactly one `CANCELLED` (15).
docs/adr/0009-m3c-capability-ipc.md:1174:- **copy-out fault after WOKEN:** `JE_ABI_INVALID` (2), message stays queued,
docs/adr/0009-m3c-capability-ipc.md:1184:  `WOKEN` and its head reserved; the **test-only forced-kill hook** marks X `DYING`
docs/adr/0009-m3c-capability-ipc.md:1186:  idempotent release returns the reserved head to its queue front, and a *second*
docs/adr/0009-m3c-capability-ipc.md:1193:  a timer tick → X gets `WOKEN`, the later timeout CAS is a no-op, message
docs/adr/0009-m3c-capability-ipc.md:1194:  delivered; (b) timeout CAS fires, THEN a late enqueue → X gets `TIMED_OUT`, the
docs/adr/0009-m3c-capability-ipc.md:1200:ring-3 path** is proven by the kernel self-test **`M3c-s6`** for the **TIMED_OUT**
docs/adr/0009-m3c-capability-ipc.md:1208:step-5 cooperative-scheduler spin-hang lesson). The **WOKEN / PEER_CLOSED /
docs/adr/0009-m3c-capability-ipc.md:1209:CANCELLED** causes, the **single-terminal-CAS / first-wins**, the **reserved-head
docs/adr/0009-m3c-capability-ipc.md:1212:the decode matrix (`test_syscall`). The **full two-process ring-3 WOKEN handshake**
docs/adr/debates/m3c-step6-mechanism.md:28:   - **Failure case:** a sibling thread closes the handle and the slot gets reused for another endpoint. The recv then resolves to a different end. The message reserved on the original end is never consumed, so that queue's head stays reserved forever (a DoS), or a message gets mixed up across ends.
docs/adr/debates/m3c-step6-mechanism.md:39:  - In `je_sched_interrupt`'s existing wake loop, add one clause: a `BLOCKED_IPC` thread past its deadline goes through `je_ipc_wait_arbitrate(TIMED_OUT)`, then the guarded flip to READY.
docs/adr/debates/m3c-step6-mechanism.md:50:3. Does closing a handle that a parked sibling thread is blocked on stamp CANCELLED on that waiter? If it does, the slot-reuse scenario in hazard 3 never reaches delivery.
docs/adr/debates/m3c-step6-mechanism.md:58:The larger missed hazard is mutable user text. While the thread is parked, a sibling in the same address space can rewrite or unmap the two-byte `int 0x80` site. On wake, the thread may execute something else or fault before re-entering the kernel, leaving the A6-mandated reserved head held indefinitely. “It only harms the process” is insufficient: it can create an IPC liveness/resource leak. A6 must either guarantee the restart site remains executable/immutable while parked, or define a reservation-cancellation/recovery mechanism.
docs/adr/debates/m3c-step6-mechanism.md:62:3. Wake arbitration stamps the cause, reserves the head on `WOKEN`, then performs the guarded `BLOCKED_IPC -> READY` transition.
docs/adr/debates/m3c-step6-mechanism.md:73:The larger missed hazard is mutable user text. While the thread is parked, a sibling in the same address space can rewrite or unmap the two-byte `int 0x80` site. On wake, the thread may execute something else or fault before re-entering the kernel, leaving the A6-mandated reserved head held indefinitely. “It only harms the process” is insufficient: it can create an IPC liveness/resource leak. A6 must either guarantee the restart site remains executable/immutable while parked, or define a reservation-cancellation/recovery mechanism.
docs/adr/debates/m3c-step6-mechanism.md:77:3. Wake arbitration stamps the cause, reserves the head on `WOKEN`, then performs the guarded `BLOCKED_IPC -> READY` transition.
docs/adr/debates/m3c-step6-mechanism.md:109:   - In that window the thread is READY, holds a reserved head, and is not in the kernel. That's harmless unless it dies there: today a fault can't happen in that window, but a future kill or cancel path could.
docs/adr/debates/m3c-step6-mechanism.md:110:   - The required rule: `mark_dying`, or the reaper, must release any `WOKEN` reservation and clear waiter state. This is the cleanup contract Codex wants from the owner, attached to the one exit path that actually exists.
docs/adr/debates/m3c-step6-mechanism.md:116:   - The only change to `je_sched_interrupt` is that the timer pass also stamps TIMED_OUT on BLOCKED_IPC threads (through the A6 CAS), next to the existing SLEEPING sweep.
docs/adr/debates/m3c-step6-mechanism.md:139:- (b) Who releases a WOKEN reservation when the holder dies before re-entry, and does the head go back to the front of the queue? A6 doesn't specify this rollback.
docs/adr/debates/m3c-step6-mechanism.md:156:Owner clarification remains required for reservation rollback if a WOKEN thread dies before re-entry, including whether the reserved queue head remains at the front. Also explicitly freeze the current “immutable user text/single-threaded address space” assumption as a prerequisite for the two-byte rewind mechanism.
docs/adr/debates/m3c-step6-mechanism.md:171:Owner clarification remains required for reservation rollback if a WOKEN thread dies before re-entry, including whether the reserved queue head remains at the front. Also explicitly freeze the current “immutable user text/single-threaded address space” assumption as a prerequisite for the two-byte rewind mechanism.
docs/adr/debates/m3c-step6-mechanism.md:186:   - **CONTRADICTION, ordering:** CLARIFY-2 says the release happens "before the thread/frame slot is reused (the A6-PEERWAKE ordering)". PEERWAKE's actual ordering is "FIRST, before the release walk runs". That is a weaker point in time. If the release walk runs while the dying thread still holds the reservation, it can close an end or free queued messages that are still reserved. CLARIFY-2 should say "before the release walk".
docs/adr/debates/m3c-step6-mechanism.md:187:   - **CONTRADICTION, terminology:** CLARIFY-2 says the head "returns to the front of its queue". A6-5 still says the reserved message "returns to the free pool when that waiter's reservation is released". "Free pool" reads as freeing the message slot. Change A6-5 to "becomes deliverable at the queue head".
docs/adr/debates/m3c-step6-mechanism.md:188:   - **Double-release risk:** "`mark_dying` **and** the reaper MUST … release", and PEERWAKE step 1 also "release[s] any head reservation". That is three call sites with no single-owner rule. Specify one idempotent release that checks the owner and only clears the reservation if this waiter holds it. Call it at `mark_dying`, and have the reaper only assert it is already clear. CLARIFY-2 is the READY/WOKEN case of PEERWAKE step 1 and should say it uses the same routine.
docs/adr/debates/m3c-step6-mechanism.md:189:   - **Gap:** "deliverable to the next receiver in FIFO order" doesn't say whether a receiver already *parked* on that end gets woken when the reservation is released. If the waiter slot frees when `WOKEN` is stamped rather than at consume, a second receiver can park behind a reserved head and then sleep until its timeout. That is the latency bug PEERWAKE exists to prevent. Either state that the slot stays occupied until consume or release (so A6-4 gives the second receiver `EMPTY` and nobody can park), or say the release arbitrates the next waiter.
docs/IMPLEMENTATION.md:22:Each has a reserved leading guard page to unmap during M1b. No IRQ is enabled in
docs/IMPLEMENTATION.md:56:image (including BSS page tables/stacks), boot block and modules stay reserved.
docs/IMPLEMENTATION.md:63:1 MiB up to a deliberate 1 GiB implementation limit. Holes and reserved firmware
docs/IMPLEMENTATION.md:87:An entry is reserved, free, an exact allocation head/length, or a continuation.
docs/IMPLEMENTATION.md:92:reserved pages are rejected. Total free memory does not imply a contiguous run.
docs/IMPLEMENTATION.md:113:Hosted tests cover exact frees, reserved-page protection, full exhaustion,
docs/IMPLEMENTATION.md:298:DYING and returns a live successor's frame (NMI/#DF/#MC/reserved + kernel-origin
docs/IMPLEMENTATION.md:338:with a nonzero reserved arg → INVALID with no side effect.
docs/IMPLEMENTATION.md:349:abi_query/yield/exit/reserved-arg/full-width-op/unsupported, each asserting the
docs/IMPLEMENTATION.md:381:not a protection/reserved/fetch fault; the faulting RIP is EXACTLY one of the two
docs/IMPLEMENTATION.md:481:self/close/query decode incl. the wide-handle INVALID and reserved-arg rules;
docs/IMPLEMENTATION.md:589:decode (reserved-arg + wide-handle rejection), `test_handle` exercises
docs/IMPLEMENTATION.md:621:(reserved-arg, wide-handle, wide-rights, unknown-rights-bit → INVALID; clean subset
docs/IMPLEMENTATION.md:680:CLOSED + peer PEER_CLOSED, both-ends-zero frees the record and bumps the internal
docs/IMPLEMENTATION.md:714:ABI status enum (pinned), reserved until the first M3c ring-3 endpoint op maps it.
docs/IMPLEMENTATION.md:735:internals — table purity preserved) and closes each LIVE endpoint handle through
docs/IMPLEMENTATION.md:764:**Ring-3 `endpoint_create` (op 10, A3-2).** Takes no args (all reserved zero else
docs/IMPLEMENTATION.md:777:  releasing the end0 holder leaves end1 `PEER_CLOSED` with the record still
docs/IMPLEMENTATION.md:812:(deliver to peer); `try_recv` on end `j` drains `end[j].inbound`. EOF/PEER_CLOSED
docs/IMPLEMENTATION.md:814:survivor to PEER_CLOSED), so there is no `i` vs `i^1` mistake on the EOF path —
docs/IMPLEMENTATION.md:816:first, regardless of peer state); empty+PEER_CLOSED → PEER_CLOSED (EOF);
docs/IMPLEMENTATION.md:824:  DENIED/PEER_CLOSED/QUEUE_FULL and user memory is never read for an unauthorized
docs/IMPLEMENTATION.md:827:  precedence frozen: decode INVALID (meta/reserved/len/wide-handle, before any
docs/IMPLEMENTATION.md:828:  user access) → BADINDEX/STALE/WRONGTYPE → DENIED → PEER_CLOSED → QUEUE_FULL →
docs/IMPLEMENTATION.md:841:`R10[63:8]`/R8/R9 reserved must be 0 (rejected INVALID at decode) — no capability
docs/IMPLEMENTATION.md:852:`JE_ABI_H_QUEUE_FULL=10`, `SHORT_BUFFER=11`, `PEER_CLOSED=12`, `EMPTY=13`
docs/IMPLEMENTATION.md:855:`RDX[31:0]`; `RDX[63:32]` reserved 0.
docs/IMPLEMENTATION.md:859:`RDX[63:32]` are reserved-must-be-0 now; **step 5 MUST add a new op or an
docs/IMPLEMENTATION.md:865:dequeue), PEER_CLOSED drain-then-EOF, rights DENIED (send without SEND / recv
docs/IMPLEMENTATION.md:868:`test_syscall` covers the op-11/12 decode (R10 cap_count/reserved, R8/R9,
docs/IMPLEMENTATION.md:876:QUEUE_FULL, the data-only reject, drain-then-PEER_CLOSED, and channel reclamation
docs/IMPLEMENTATION.md:894:receiver until a message arrives (WOKEN), the peer closes (PEER_CLOSED), the
docs/IMPLEMENTATION.md:895:deadline elapses (TIMED_OUT), or an internal cancel fires (CANCELLED, A6-2
docs/IMPLEMENTATION.md:899:WOKEN|TIMED_OUT|PEER_CLOSED|CANCELLED`, on the endpoint end) and the **scheduler**
docs/IMPLEMENTATION.md:903:snapshot). The kern bridge calls arbitrate on sender-enqueue (WOKEN, on the peer
docs/IMPLEMENTATION.md:904:end), peer-close (PEER_CLOSED, drain-first: only on an empty end), the timer ISR
docs/IMPLEMENTATION.md:905:(TIMED_OUT), and internal cancel; on WON it `je_sched_wake_ipc(id)` (guarded
docs/IMPLEMENTATION.md:921:**Reservation + release (A6-5 / A6-CLARIFY-2).** A WOKEN waiter reserves the
docs/IMPLEMENTATION.md:923:and `recv_peek_caps` mask a reserved head → EMPTY). Exactly one idempotent release
docs/IMPLEMENTATION.md:932:`JE_ABI_H_TIMED_OUT=14`, `JE_ABI_H_CANCELLED=15`. Caps compose: a WOKEN delivery
docs/IMPLEMENTATION.md:933:runs the same step-5 staged-mint path from the owned reserved head
docs/IMPLEMENTATION.md:937:**Proofs.** **`M3c-s6`** kernel self-test proves the **TIMED_OUT** cause
docs/IMPLEMENTATION.md:942:waiter layer (register, the single terminal CAS + first-wins, reserved-head
docs/IMPLEMENTATION.md:944:`test_syscall` covers the R10-timeout decode. The **full two-process ring-3 WOKEN
docs/IMPLEMENTATION.md:978:stale-cap head-of-line wedge. PEER_CLOSED still mints (only CLOSED is dead).
docs/IMPLEMENTATION.md:989:**ABI (A5-4).** The pre-reserved R8/R9 path: send R8 = user `u64[cap_count]` of
docs/IMPLEMENTATION.md:1005:decode (cap_count/R8 pairing, R9 pairing, K bound, reserved bits). The ring-3
AGENTS.md:10:   language; assembly is reserved for boot, traps, context switching, and exact

 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	#include <jeios/uframe.h>
     3	
     4	/* RFLAGS bit positions. */
     5	#define RF_CF   (UINT64_C(1) << 0)
     6	#define RF_ONE  (UINT64_C(1) << 1)   /* reserved, always 1 */
     7	#define RF_PF   (UINT64_C(1) << 2)
     8	#define RF_AF   (UINT64_C(1) << 4)
     9	#define RF_ZF   (UINT64_C(1) << 6)
    10	#define RF_SF   (UINT64_C(1) << 7)
    11	#define RF_TF   (UINT64_C(1) << 8)
    12	#define RF_IF   (UINT64_C(1) << 9)
    13	#define RF_DF   (UINT64_C(1) << 10)
    14	#define RF_OF   (UINT64_C(1) << 11)
    15	
    16	/* Arithmetic flags the user may freely keep. Everything else (IOPL, NT, RF, VM,
    17	 * AC, VIF, VIP, ID, TF, DF, reserved) is forced off; IF and reserved-bit1 on.
    18	 * DF is cleared too: the entry ABI gives the user a known direction flag (DF=0),
    19	 * matching the header/ADR D6 "clear unsupported flags" intent. */
    20	#define JE_UFRAME_SAFE (RF_CF | RF_PF | RF_AF | RF_ZF | RF_SF | RF_OF)
    21	
    22	static bool canonical(uint64_t a)
    23	{
    24	    /* 48-bit canonical: bits [63:47] all equal. */
    25	    uint64_t hi = a >> 47;
    26	    return hi == 0 || hi == (UINT64_C(0x1ffff));
    27	}
    28	
    29	bool je_uframe_check(const je_trap_frame *f, uint64_t base, uint64_t size,
    30	                     uint64_t *out_rflags)
    31	{
    32	    if (!f || !out_rflags || size == 0) return false;
    33	    uint64_t end = base + size;
    34	    if (end < base) return false; /* overflow guard */
    35	    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS) return false;
    36	    if (!canonical(f->rip) || !canonical(f->rsp)) return false;
    37	    /* RIP in [base, end); RSP in (base, end] (full-descending stack top). */
    38	    if (f->rip < base || f->rip >= end) return false;
    39	    if (f->rsp <= base || f->rsp > end) return false;
    40	    *out_rflags = (f->rflags & JE_UFRAME_SAFE) | RF_IF | RF_ONE;
    41	    return true;
    42	}
   176	/* Resolve the current user process as (vm,index) for the endpoint ops, which
   177	 * need the vm (teardown-hook check + pool) not just the table. Returns false for
   178	 * a non-user caller. */
   179	static bool current_vm_index(je_vm **out_vm, uint32_t *out_index)
   180	{
   181	    return je_sched_current_user(out_vm, out_index) == JE_OK;
   182	}
   183	
   184	/* M3c (A3-2): a handle packs losslessly into half of the RDX scalar only because
   185	 * it is 32 bits. The endpoint_create two-handle return depends on this. */
   186	_Static_assert(sizeof(je_handle) == 4u, "endpoint_create packs two handles in RDX");
   187	/* The table-side endpoint type/rights copies must equal the ABI ones the glue
   188	 * uses, or typed dispatch and create would disagree. */
   189	_Static_assert(JE_ENDPOINT_OBJ_TYPE == JE_ABI_OBJ_ENDPOINT, "endpoint type: table vs ABI");
   190	_Static_assert(JE_EP_RIGHT_SEND == JE_ABI_RIGHT_SEND
   191	               && JE_EP_RIGHT_RECV == JE_ABI_RIGHT_RECV
   192	               && JE_EP_RIGHT_TRANSFER == JE_ABI_RIGHT_TRANSFER
   193	               && JE_EP_RIGHT_INSPECT == JE_ABI_RIGHT_INSPECT,
   194	               "endpoint rights: table vs ABI");
   195	_Static_assert(JE_EP_MSG_MAX == JE_ABI_MSG_MAX, "endpoint msg max: table vs ABI");
   196	_Static_assert(JE_EP_MSG_CAPS_MAX == JE_ABI_MSG_CAPS_MAX, "endpoint caps max: table vs ABI");
   197	
   198	/* Map endpoint-layer status to the v0 ABI (reuses the JE_ABI_H_* namespace; the
   199	 * glue is the only place endpoint.h and abi.h meet). */
   200	static je_abi_status ep_status_to_abi(je_ep_status s)
   201	{
   202	    switch (s) {
   203	    case JE_EP_OK:          return JE_ABI_OK;
   204	    case JE_EP_BADINDEX:    return JE_ABI_H_BADINDEX;
   205	    case JE_EP_STALE:       return JE_ABI_H_STALE;
   390	    __asm__ volatile("int $0x81":::"memory");
   391	    je_arch_irq_restore(flags);return JE_OK;
   392	}
   393	_Noreturn void je_thread_exit(void)
   394	{
   395	    (void)je_arch_irq_save();
   396	    if (!ready || current==0) abort_scheduler();
   397	    threads[current].state=ZOMBIE;
   398	    __asm__ volatile("int $0x81":::"memory");
   399	    abort_scheduler();
   400	}
   401	bool je_sched_finished(void)
   402	{
   403	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   404	        if (threads[i].state!=EMPTY && threads[i].state!=ZOMBIE) return false;
   405	    return true;
   406	}
   407	je_status je_sched_reap(void)
   408	{
   409	    if (current!=0 || !je_sched_finished()) return JE_ERR_BUSY;
   410	    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==ZOMBIE) {
   411	        /* User threads own no je_kernel_pages stack: their kernel stack is the
   412	         * vm process's held run, freed by je_vm_destroy. Reaping it here would
   413	         * double-free (M3a #4 step 3). Clear the slot only. */
   414	        if (threads[i].kind==THREAD_USER) { threads[i]=(thread){0}; continue; }
   415	        je_status s=je_arch_page_restore((uintptr_t)threads[i].stack);
   416	        if (s!=JE_OK) return s;
   417	        s=je_pages_free(&je_kernel_pages,threads[i].stack,STACK_PAGES);
   418	        if (s!=JE_OK) return s;
   419	        threads[i]=(thread){0};
   420	    }
   421	    return JE_OK;
   422	}
   423	uint64_t je_sched_ticks(void) { return je_timer_ticks; }
   424	uint64_t je_sched_preemptions(void) { return preemptions; }
   425	uint64_t je_sched_user_cpl3_dispatches(void) { return user_cpl3_dispatches; }
   426	/* M3a #4 step 3: remove a user thread (by its vm process) from scheduling
   427	 * before its process is destroyed, so the scheduler never dispatches a torn-down
   428	 * CR3. Caller holds interrupts disabled and must not be the user thread. */
   429	je_status je_sched_drop_user(je_vm *vm, uint32_t index)
   430	{
   431	    if (!ready || !vm) return JE_ERR_INVALID;
   432	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   433	        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index) {
   434	            if (i==current) return JE_ERR_BUSY;
   435	            if (threads[i].rsp0_top) { /* restore the kstack guard before the process frees the run */
   436	                uint64_t kbase=threads[i].rsp0_top-(uint64_t)STACK_PAGES*4096u;
   437	                je_status s=je_arch_page_restore((uintptr_t)kbase);
   438	                if (s!=JE_OK) return s;
   439	            }
   440	            threads[i]=(thread){0};
   441	            return JE_OK;
   442	        }
   443	    return JE_ERR_INVALID;
   444	}
   445	

 succeeded in 124ms:
./build/0/test_core
core tests: PASS (feature prerequisites, fail-closed selection, reverse rollback)
./build/0/test_boot
boot parser: PASS (truncation, overflow, overlaps, modules, mutation corpus)
./build/0/test_alloc
allocator: PASS (reservations, exact frees, exhaustion, fragmentation, zeroing, rollback)
./build/0/test_vm
vm: PASS (validation, admission, audit, permissions, zeroing, rollback, teardown, death-hook)
./build/0/test_uframe
uframe: PASS (bounds, selectors, canonical, rflags sanitize)
./build/0/test_syscall
syscall: PASS (abi_query, yield, exit, copy_probe, handle self/close/query/rights/dup, endpoint_create, try_send/try_recv decode, reserved-arg, wide-handle, unknown-rights, full-width op, unsupported)
./build/0/test_handle
handle: PASS (encode/decode, alloc/lookup/close, forged/stale/wrongtype/denied, quota precedence) [production 16/16]
./build/0/test_handle_rw
handle: PASS (encode/decode, alloc/lookup/close, quota, saturation->retire, retired-capacity loss, noslots-via-retirement) [reduced width gen_bits=2]
./build/0/test_endpoint
endpoint: PASS (create, split-ref close/destruction, dup acquire-before-commit, REFOVERFLOW, DENIED/WRONGTYPE, inflight keep-alive, pool-exhaustion + create-unwind, release-walk, data IPC send/recv/FIFO/QUEUE_FULL/SHORT_BUFFER/PEER_CLOSED/rights/zero-len, cap-transfer mint/dead-cap/cycle-reclaim)
python3 tools/check_elf.py build/0/jeios.elf
ELF checks: PASS (AMD64, Multiboot2, entry, segments, no undefined symbols)
ABI isolation: abi.h must not include the internal status.h or handle.h
Dev-inject isolation: the fixup-injection path must be absent from the default kernel

exec
/usr/bin/bash -lc "nl -ba docs/adr/0009-m3c-capability-ipc.md | sed -n '700,850p;850,990p;1000,1145p;1150,1225p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba docs/IMPLEMENTATION.md | sed -n '880,955p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -n \"JE_EP_MAX_WAITERS\\|JE_EP_REF_MAX\\|typedef struct.*thread\\|je_ep_waiter\\|je_ep_channel\" include/jeios/endpoint.h include/jeios/sched.h | head -100 && nl -ba include/jeios/endpoint.h | sed -n '90,210p;300,470p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "git diff --cached -- kernel/core/sched.c | sed -n '1,280p'; git diff --cached -- kernel/core/syscall.c | sed -n '1,320p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
   700	**Attached correspondence (ADR 0000 part 4).** Design driven by an
   701	implementor↔adversarial `peer-debate.sh` run (claude=implementor,
   702	codex=adversarial-reviewer, 2 rounds), then an adversarial review of THIS
   703	amendment's text (which caught ten contract-text defects — ABI mapping, the
   704	single-CAS-vs-priority wording, the copy-fault re-arm, teardown resurrecting a
   705	DYING thread, the zero-timeout short-circuit, caps composition — all fixed in the
   706	text below before owner sign-off). Full transcript:
   707	[`docs/adr/debates/m3c-step6-waits.md`](debates/m3c-step6-waits.md). A follow-up
   708	**mechanism** debate (the park/resume x86 mechanism behind A6-CLARIFY-1/2) is at
   709	[`docs/adr/debates/m3c-step6-mechanism.md`](debates/m3c-step6-mechanism.md). The
   710	reviewer
   711	sharpened the design materially (separate waiter vs scheduler state machines, one
   712	arbitration CAS, no copy-out from the scheduler) and surfaced the A6-1 contract
   713	conflict below as a genuine owner escalation, not a clarification.
   714	
   715	### A6-STRUCT. Two separate state machines + one arbitration CAS
   716	
   717	**Decision + why.** The **waiter** state (`WAITING→WOKEN|TIMED_OUT|CANCELLED|
   718	PEER_CLOSED`) is kept **distinct** from the **scheduler** state
   719	(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
   720	pure helper **`je_ipc_wait_arbitrate(waiter, cause)`** performs the terminal
   721	transition as **one compare-and-set** under IF=0: the first eligible cause wins,
   722	later causes are no-ops. Every path that creates eligibility calls it in the SAME
   723	IF=0 critical section as the endpoint/clock mutation that caused it — sender
   724	enqueue → `WOKEN`, peer close → `PEER_CLOSED`, timer ISR deadline → `TIMED_OUT`,
   725	internal cancel → `CANCELLED`. The parked **syscall continuation** then *validates
   726	the already-selected result and performs the owned-record copy-out + commit*; it
   727	is **not** a second authority and never rewrites the CAS result. (Rationale: the
   728	adversarial review showed "ISR stamps TIMED_OUT, syscall re-checks and may upgrade
   729	to WOKEN" would make the syscall a second authority — not the single-terminal-CAS
   730	D7 requires. Making enqueue/close do their own CAS in their own critical section
   731	is what keeps it single.)
   732	
   733	**Single-CPU linearization (replaces the literal "priority" reading).** On one CPU
   734	with IF=0, the four causes NEVER become eligible simultaneously — each runs its CAS
   735	in its own critical section, so they are strictly ordered in time. **The first
   736	critical section to fire the CAS wins; every later one is a no-op** (the waiter is
   737	already terminal). D7's written priority `PEER_CLOSED > WOKEN > CANCELLED >
   738	TIMED_OUT` therefore does NOT mean "a later higher-priority cause supersedes an
   739	earlier terminal state" (that would be a second transition, forbidden); it means
   740	**the recv entry re-check evaluates the currently-eligible causes in ONE snapshot
   741	before parking** — at entry only WOKEN (a queued/deliverable message) and
   742	PEER_CLOSED (empty + peer gone) can be jointly relevant, and A6-1 drain-first
   743	decides between them: a non-empty queue picks WOKEN and the thread never parks,
   744	an empty closed end picks PEER_CLOSED. Once parked, there is only one snapshot per CAS and
   745	first-wins holds. (This is a D7 *clarification*, flagged for the owner: the frozen
   746	"ISR delivers the result" is reconciled as "the ISR's CAS stamp **is** the result;
   747	the continuation only transcribes the stamped cause into the user frame," see
   748	A6-5.)
   749	
   750	**`je_ipc_wait_arbitrate(waiter, cause)` returns a won/lost flag.** The caller uses
   751	it: a sender's enqueue path learns whether it won the CAS (and therefore owns the
   752	head reservation it must install); teardown learns whether it still owes a
   753	reservation/reference release. A "lost" return means the waiter was already
   754	terminal — the caller performs no wake and installs no reservation. The helper
   755	performs NO re-arm: a terminal waiter stays terminal (see A6-5 for the copy-fault
   756	path, which releases the reservation but does NOT re-arm the same waiter).
   757	
   758	**Predictions if other choices were made.** *Waiter state IN the scheduler enum
   759	(my first lean):* conflates "why the block ended" with "is it runnable," and makes
   760	every scheduler state check also reason about IPC causes — more tested-code
   761	coupling, the opposite of the owner gate's intent. *Syscall re-check as the
   762	authority:* two authorities for one terminal state → the exact double-transition
   763	race D7 forbids; a timeout and a late enqueue could both "win." *Waiter registry
   764	in endpoint.c instead of the thread:* thinner `sched.c` but endpoint code reaches
   765	into scheduler internals to wake — worse coupling across the tested boundary.
   766	
   767	**Flex options to consider in future.** **FLEX (planning review).** Blocking
   768	*send* (deferred in v1) will add a sender-side waiter + a staging-buffer lifetime;
   769	the arbitrate helper is meant to generalize to it. If a third blocking op appears,
   770	revisit whether the waiter fields belong in the thread struct or a side table.
   771	
   772	### A6-1. Queued-message-plus-peer-close → DRAIN-FIRST wins (A1)
   773	
   774	**Decision + why.** When a message is queued **and** the peer has closed, the
   775	queued message is **delivered** (`WOKEN`); `PEER_CLOSED` is returned only when the
   776	queue is **empty**. The literal D7 priority `PEER_CLOSED > WOKEN` is read as
   777	ordering **block-ending triggers**, NOT as overriding already-queued data. This
   778	was a genuine frozen-contract conflict (owner-escalated, not a clarification): the
   779	**merged step-4 `try_recv` is already drain-first** (CLOSED is "dead" only on an
   780	empty queue), so A1 makes blocking `recv` behave **identically** to non-blocking
   781	`try_recv` on the same endpoint state — "closing a peer never eats bytes already
   782	delivered to you." **Owner-approved A1.**
   783	
   784	**Predictions if other choices were made.** *A2 (literal PEER_CLOSED > a queued
   785	message):* blocking `recv` and non-blocking `try_recv` would return *different*
   786	results on the identical state (one drains, one reports closed) — a surprising
   787	split a caller could not predict from the op alone, and a divergence from already-
   788	tested step-4 behavior that would need its own justification. It would also strand
   789	a legitimately-sent final message on peer close.
   790	
   791	**Flex options to consider in future.** Firm for M3c. If a future "urgent close"
   792	semantic is ever wanted (peer close *preempts* pending data), that is a new op or
   793	an explicit flag, not a reinterpretation of this priority — a new ADR.
   794	
   795	### A6-ABI. Op, timeout register, and terminal-cause → status mapping
   796	
   797	**Decision + why.** Blocking recv **reuses the merged `try_recv` op**
   798	(`JE_SYS_TRY_RECV`=12), with the **timeout (relative duration in ticks) in R10**
   799	(reserved=0 today). Backward-compatible: every existing non-blocking caller passes
   800	**R10=0**, and **zero timeout = the exact current non-blocking semantics** — check
   801	once, never park, and on an OPEN end with nothing deliverable return **`EMPTY`=13**,
   802	identical to today. A nonzero R10 opts into blocking. The four terminal causes (plus the post-WOKEN fault row) map to the
   803	user frame as:
   804	
   805	| Terminal cause | status (AL/RAX) | RDX / R8 / R9 |
   806	|---|---|---|
   807	| `WOKEN` | `JE_ABI_OK`=0 | merged step-4/step-5 recv success layout: RDX[31:0]=byte len, RDX[63:32]=cap count; R8/R9 handle-out per A5-4 |
   808	| `TIMED_OUT` | **`JE_ABI_H_TIMED_OUT`=14** (new, `_Static_assert`) | RDX=0, no handles |
   809	| `PEER_CLOSED` | `JE_ABI_H_PEER_CLOSED`=12 (existing) | RDX=0, no handles |
   810	| `CANCELLED` | **`JE_ABI_H_CANCELLED`=15** (new, `_Static_assert`; ABI-unreachable in v1 per A6-2, but the continuation still writes a defined status) | RDX=0, no handles |
   811	| copy/mint fault after WOKEN | `JE_ABI_INVALID`=2 (existing) | message stays queued (A6-5) |
   812	
   813	Two new codes (14, 15) extend the 10–13 block; no existing code changes meaning.
   814	**Caps compose:** a blocking recv is a full recv — on `WOKEN` it runs the **same
   815	step-5 staged-mint path** (R8/R9 handle-out, dead-cap→handle 0, mint-failure
   816	rollback → `JE_ABI_INVALID` with the message left queued). A zero-timeout (poll)
   817	recv that finds a cap-bearing message behaves exactly as merged `try_recv`
   818	(SHORT_BUFFER if the handle buffer is too small). **Owner-approved** (reuse-op +
   819	zero=EMPTY was the design-pass recommendation; the two new codes follow from
   820	pinning the mapping).
   821	
   822	**Entry snapshot goes through the plain `try_recv` path — no waiter registered when
   823	a result is available at entry.** Both the zero-timeout poll AND the first check of
   824	a nonzero-timeout recv evaluate the end **once via the existing merged `try_recv`
   825	logic** before any waiter/`wake`/reservation exists. If that check yields ANY
   826	immediate result — `WOKEN`-equiv delivery, **`SHORT_BUFFER`** (buffer or handle-out
   827	too small), or `PEER_CLOSED` — it is returned straight away and **no waiter is
   828	registered, no head reserved, the A6-4 occupied-slot check is not consulted**. A
   829	waiter is registered (and A6-4 applies) **only** when the entry check would
   830	otherwise return `EMPTY` on an OPEN end *and* the timeout is nonzero. Consequently a
   831	too-small buffer is **`SHORT_BUFFER` whether polling or blocking** (identical to
   832	merged `try_recv`, satisfying A6-1's "behave identically"), and is **never** swept
   833	into the post-WOKEN `JE_ABI_INVALID` fault bucket. The post-WOKEN `JE_ABI_INVALID`
   834	path (A6-5) is strictly for a **copy/mint fault on a message that was already
   835	reserved** (e.g. the user buffer became unmapped between park and resume), not for
   836	a known-short buffer — a short buffer is detected at the entry/peek check before
   837	any reservation, leaves the message queued, and reserves nothing.
   838	
   839	**Predictions if other choices were made.** *A new `JE_SYS_RECV` op:* a
   840	near-duplicate recv path + op-space growth for no behavioral gain (R10 was already
   841	reserved=0 for this). *Zero-timeout = immediate TIMED_OUT:* every existing R10=0
   842	`try_recv` caller flips `EMPTY`→`TIMED_OUT` — a silent breaking change to merged,
   843	tested behavior; rejected. *Leaving TIMED_OUT/CANCELLED unmapped (the text defect
   844	the review caught):* the continuation would write an undefined status and a caller
   845	could not tell a timeout from a corrupt result — the mapping is load-bearing.
   846	
   847	**Flex options to consider in future.** **FLEX (planning review).** If blocking
   848	*send* is added it will want a timeout; whether it reuses R10 on `try_send` by the
   849	same rule is a future call. Code values 14/15 are fixed once asserted.
   850	
   850	
   851	### A6-2. CANCELLED is internal-only in v1 (B1)
   852	
   853	**Decision + why.** The `CANCELLED` terminal state is reachable in v1 **only via
   854	an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
   855	deterministic kernel test; there is **no ABI path** to it yet (no signal / kill /
   856	async-cancel mechanism exists in M3c). It is implemented and tested now so the
   857	arbitration CAS and all four terminal causes are proven (first-critical-section-
   858	wins, losers no-ops), and so a
   859	future signal mechanism wires to a ready terminal state. Documented as
   860	**ABI-unreachable in v1**. **Owner-approved B1.**
   861	
   862	**Predictions if other choices were made.** *B2 (defer CANCELLED entirely — state
   863	exists, no path, no test):* the arbitration helper would ship with one of its four
   864	terminal causes unexercised, so the first real canceller (a future signals
   865	milestone) would be adding an *untested* transition into tested scheduler code —
   866	exactly the risk the owner gate exists to avoid. Testing it now with an internal
   867	trigger is cheap and front-loads that proof.
   868	
   869	**Flex options to consider in future.** **FLEX (planning review).** The ABI
   870	trigger for cancellation (a signal syscall, a deadline-update that cancels, a
   871	process-level cancel) is a future-milestone decision; A6-2 only fixes that the
   872	mechanism exists and is internally reachable now.
   873	
   874	### A6-3. Deadline encoding: zero = poll, bounded max, overflow-checked (C)
   875	
   876	**Decision + why.** The timeout is a **relative duration in timer ticks** (the
   877	existing M2 clock; inclusive `wake <= ticks` as the current sleep path uses).
   878	**Zero timeout = a non-blocking poll** matching merged `try_recv` exactly (check
   879	the entry snapshot once; deliver `WOKEN`-equiv / `PEER_CLOSED` / **`EMPTY`** per
   880	A6-ABI — NOT `TIMED_OUT`). **Zero short-circuits BEFORE any waiter registration:**
   881	it NEVER computes `wake`, NEVER registers a waiter, NEVER enters
   882	`THREAD_BLOCKED_IPC`, and does NOT `yield` — so the inclusive `wake <= ticks` ISR
   883	rule can never apply to it, and a 0-tick and a 1-tick request are not confusable (a
   884	1-tick request sets `wake = ticks+1` and parks, firing on the next `++ticks`; "one
   885	tick" means "up to one tick," no minimum). A **bounded maximum** `JE_IPC_MAX_WAIT`
   886	is enforced: a nonzero duration is rejected (`JE_ABI_INVALID`) if
   887	`duration > JE_IPC_MAX_WAIT` or if `ticks + duration` would overflow `uint64` —
   888	mirroring the existing `je_thread_sleep` overflow guard. No unbounded block, ever.
   889	**Owner-approved.**
   890	
   891	**Predictions if other choices were made.** *Zero = block forever:* violates D7's
   892	"no unbounded block" outright. *No max / no overflow check:* a caller passing a
   893	near-`UINT64_MAX` duration wraps the deadline below `ticks`, making the ISR fire an
   894	*immediate* spurious `TIMED_OUT` (or, worse, never) — a silent correctness bug the
   895	existing sleep path already guards against, so not guarding here would be an
   896	inconsistency.
   897	
   898	**Flex options to consider in future.** `JE_IPC_MAX_WAIT`'s exact value is a
   899	policy bound like the other `JE_*` caps and may change; the *rule* (zero=poll,
   900	bounded, overflow-checked) is firm.
   901	
   902	### A6-4. Waiter-capacity exhaustion reuses the non-blocking path (D)
   903	
   904	**Decision + why.** Waiter slots are bounded **per endpoint end**
   905	(`JE_EP_MAX_WAITERS` = 1 blocked receiver per end; an endpoint has two ends). A
   906	receiver blocks on **one specific end** (the one its handle names). A second waiter
   907	on the same end **is reachable via the ABI**, NOT structurally impossible: process
   908	A blocks on end E (nonzero timeout) and the scheduler switches to process B, which
   909	holds a **transferred duplicate** handle to E and also calls blocking recv on it.
   910	So the bound is enforced as a **runtime fail-closed branch, never a panicking
   911	assert** (a panicking assert on an ABI-reachable path would be a user-triggerable
   912	kernel DoS): a blocking recv that finds the end's single waiter slot already
   913	occupied does **not** park — it **returns `EMPTY`** (A6-ABI) immediately, exactly as
   914	a zero-timeout poll on an OPEN empty end would. This is a legitimate, testable
   915	outcome (the second concurrent blocker is told "nothing for you right now"), not an
   916	error status. **Owner-approved** (per-end bound + runtime fail-closed to EMPTY).
   917	
   918	**Predictions if other choices were made.** *A new `JE_ABI_WAITERS_FULL` status:*
   919	adds ABI surface for what is really just "no message available to you right now" —
   920	`EMPTY` already means exactly that, so a new code would be redundant and a permanent
   921	ABI commitment. *A panicking `assert` on the occupied-slot path:* since the path is
   922	ABI-reachable (the transferred-dup case above), any user could halt the kernel —
   923	the DoS this fail-closed branch exists to prevent. *Making the bound per-process
   924	instead of per-end:* mis-models the thing blocked-on (a thread waits on an END, not
   925	on "its process"); the transferred-dup case is precisely two *different* processes
   926	contending for one end, which a per-end bound models correctly and a per-process
   927	bound does not. *A multi-slot waiter queue per end:* unnecessary for v1 (the second
   928	blocker is correctly served `EMPTY` and can retry); revisit with blocking-send or
   929	multi-threaded processes.
   930	
   931	**Flex options to consider in future.** **FLEX (planning review).** When
   932	multi-threaded processes or blocking *send* arrive, `JE_EP_MAX_WAITERS` grows and
   933	exhaustion may become reachable; *then* a real status (or a queue-of-waiters) is
   934	revisited. v1 fails closed because it cannot happen.
   935	
   936	### A6-5. No user copy-out from the scheduler; parked syscall continuation copies
   937	
   938	**Decision + why.** The wake path (arbitrate CAS + reserve the queue head for a
   939	`WOKEN`) runs in the scheduler/ISR, but the **user copy-out and commit happen in
   940	the resumed syscall continuation**, never inside `je_sched_interrupt` after a
   941	`cr3_load`. The scheduler flip is **guarded**: `THREAD_BLOCKED_IPC → READY` **only**
   942	— never from `DYING`, `ZOMBIE`, `EMPTY`, or any other state (so a torn-down thread
   943	can never be made runnable by a late wake, see A6-PEERWAKE).
   944	
   945	**Head reservation lifecycle (one outstanding reservation per end, pinned).** When
   946	the arbitrate CAS stamps `WOKEN`, the winning path (a sender's enqueue, or the
   947	entry-snapshot drain) **reserves exactly the current queue head** for that waiter —
   948	a single reservation marker on the end, so no other receiver can take that message
   949	and FIFO is preserved. The reservation is resolved by exactly one of:
   950	- **successful commit** in the continuation → dequeue the reserved head (+ release
   951	  any transferred-cap inflight per step 5), clear the reservation;
   952	- **copy/mint fault** in the continuation → return `JE_ABI_INVALID`, **leave the
   953	  message queued**, and **release the reservation** so a *future* recv on that end
   954	  can take it. This does **NOT re-arbitrate the already-terminal waiter** (that
   955	  would be a second transition, forbidden by A6-STRUCT): the faulting recv's waiter
   956	  is done (terminal `WOKEN`, result `JE_ABI_INVALID`); the released reservation
   957	  simply makes the still-queued head available to the *next* recv call;
   958	- **waiter teardown** (process death, A6-PEERWAKE) → release the reservation before
   959	  the slot is reused.
   960	There is never more than one outstanding reservation per end (A6-4 bounds waiters
   961	to one per end), so "reserve the head" is unambiguous. **A reserved head is not
   962	available to any other receiver:** a concurrent zero-timeout poll (or entry-snapshot
   963	check) from a process holding a transferred-dup handle to the same end sees the
   964	reserved head as **not deliverable** and returns `EMPTY` — it does NOT take the
   965	reserved message (that would break the reservation and FIFO) and does NOT itself
   966	register a waiter. The reserved message is delivered only to the waiter that
   967	reserved it, or **becomes deliverable again at its queue head** (it is never
   968	dequeued by the reservation) when that waiter's reservation is released via the
   969	single idempotent `je_ipc_waiter_release` (copy fault / teardown, A6-CLARIFY-2).
   970	The continuation does **not** re-resolve the original
   971	handle (D3 owned-reference rule) — it uses the reserved owned record.
   972	**Owner-approved design.**
   973	
   974	**Predictions if other choices were made.** *Copy-out inside the scheduler after
   975	the address-space switch:* introduces a brand-new page-fault context in scheduler
   976	assembly; the only exact-RIP user-copy-fault recovery proven so far is the M3a
   977	*syscall* path, so copying post-`cr3_load` in the ISR would be *untested* fault
   978	machinery in the most safety-critical file — precisely what the owner gate guards.
   979	*Re-running the handle-based recv on wake:* re-resolves a capability the owned-
   980	reference rule says must not be re-resolved after blocking (the handle could have
   981	changed), and double-counts the drain. *Re-arming the faulting waiter instead of
   982	releasing-and-leaving-queued:* would be a second terminal transition on a waiter
   983	that already won `WOKEN` — the double-transition A6-STRUCT forbids; releasing the
   984	reservation (message stays queued for the next call) achieves recovery without it.
   985	*An unguarded scheduler flip:* a late wake could set a `DYING`/`ZOMBIE` thread
   986	`READY` and resurrect a torn-down frame — the stale-slot write D3.3 names.
   987	
   988	**Flex options to consider in future.** Firm. A blocking-send milestone will add a
   989	symmetric copy-*in* on the sender continuation under the same rule.
   990	
  1000	  0x81`). A new **`je_sched_block_current(f)`** — modeled on the existing
  1001	  `je_sched_exit_current` (same frame-identity checks) **plus a pre-park
  1002	  `je_uframe_check(f)`** (a deliberate, stated deviation from that model: a thread
  1003	  that would park MUST have a valid return frame, so a user-corrupted RSP/RFLAGS is
  1004	  rejected as `DEATH_BAD_RETURN` at park time rather than discovered at wake while
  1005	  it holds a reservation — see A6-CLARIFY-2) — saves the recv args + the **owned
  1006	  endpoint reference** + the post-`int` RIP into the thread's waiter fields (the
  1007	  `wait` sub-struct: `pending`, `end_ref`, `side`, saved `rdi/rsi/rdx/r8/r9`,
  1008	  `deadline`, `saved_post_int_rip`, `cause`), sets `state = THREAD_BLOCKED_IPC`,
  1009	  **rewinds `f->rip` by a named `JE_INT80_LEN == 2`** (the fixed `CD 80` encoding;
  1010	  the only DPL3 gate, and the PIC is remapped off 0x80, so `rip-2` provably points
  1011	  at the `int 0x80` site), and returns `select_and_install_next()`.
  1012	- **Wake:** a cause-stamping path (`je_sched_wake_ipc`, a **guarded wrapper around
  1013	  the single A6 `je_ipc_wait_arbitrate` CAS** — never an independent
  1014	  `BLOCKED_IPC→READY` transition) flips the thread READY. When the scheduler next
  1015	  installs it, CR3/RSP0 are already switched (`select_and_install_next`), the
  1016	  thread re-executes `int 0x80`, and a **pending-continuation prologue at the top
  1017	  of `je_syscall_dispatch`** (keyed on `threads[current].wait.pending`, **not** on
  1018	  RAX) finishes the recv **from the saved args + the owned reference, NEVER
  1019	  re-resolving `f->rdi`** (the A6-5 owned-reference rule — a plain restartable
  1020	  syscall that re-resolved the handle would violate it; "constrained" = it does
  1021	  not). The prologue also asserts `f->rip == saved_post_int_rip`; a mismatch is a
  1022	  kernel-invariant abort, not a user error. Copy-out thus happens in a fresh
  1023	  dispatch on the woken thread, **after** the context switch — satisfying A6-5's
  1024	  "no copy from the scheduler." **No trap-entry assembly change.**
  1025	
  1026	**Gated prerequisite (the honest dependency).** Re-executing the restart site is
  1027	sound **only** under two invariants true on main today, now recorded so a future
  1028	change cannot silently invalidate the mechanism:
  1029	1. **User code text is immutable** — `USER_CODE` is mapped R/X with no writable
  1030	   alias surviving publish, and the ABI exposes no `mmap`/`mprotect`/`unmap`
  1031	   (the syscall ops are 0–12), so nothing can rewrite or unmap the `CD 80` site
  1032	   while parked.
  1033	2. **One thread per process** — `threads[slot]` is built from a single `p->root`,
  1034	   so no sibling thread shares the address space to race the restart site.
  1035	**This prerequisite MUST be re-gated** when `mmap`/`mprotect` or multi-threaded
  1036	processes land (a `FLEX (planning review)` item): at that milestone the restart
  1037	mechanism is re-reviewed, and if either invariant is lost it is replaced by a
  1038	kernel-side continuation that resumes without re-executing `int 0x80`.
  1039	
  1040	**Predictions if other choices were made.** *A heavier kernel-continuation now
  1041	(re-enter the recv in kernel mode without re-executing `int 0x80`):* avoids the
  1042	prerequisite but adds scheduler/context-switch surface and a second copy-out entry
  1043	point — more untested machinery in the highest-blast-radius file, the opposite of
  1044	what the owner gate wants, for a dependency that is honestly recordable instead.
  1045	*Plain (unconstrained) restartable syscall that re-resolves `rdi` on re-entry:*
  1046	re-resolves a capability A6-5 forbids re-resolving after blocking — a handle-reuse
  1047	hole (the slot could now name a different object); rejected. *Hard-coding the
  1048	rewind length without a named constant + encoding comment:* a latent landmine if
  1049	the gate vector ever changes; the named `JE_INT80_LEN` + SDM citation makes it
  1050	auditable.
  1051	
  1052	**Flex options to consider in future.** **FLEX (planning review).** The gated
  1053	prerequisite above is the explicit revisit trigger (mmap/mprotect/multithread).
  1054	The constant `JE_INT80_LEN` is fixed by the `CD 80` ABI and only changes if the
  1055	syscall gate itself is redesigned (a new ADR).
  1056	
  1057	**Decision + why.** `je_vm_destroy`'s existing endpoint release walk (step-3,
  1058	owner-signed) tears down the dying process's handles. When removing the dying
  1059	process's last handle to an end transitions **that end to `CLOSED`**, the end's
  1060	**peer end becomes `PEER_CLOSED`** (the merged step-3/4 state rule — the dying side
  1061	goes CLOSED, the surviving side goes PEER_CLOSED). The walk now, in the SAME
  1062	IRQ-critical section, also **calls the arbitrate CAS on a receiver blocked on that
  1063	PEER (surviving) end** — i.e. a receiver in the *other*, still-living process —
  1064	waking it `PEER_CLOSED` **immediately** rather than letting it wait out its
  1065	deadline. Note per A6-1 (drain-first): if the peer end still has a queued message,
  1066	the blocked receiver was already woken `WOKEN` when that message was enqueued and
  1067	is not blocked anymore; `PEER_CLOSED` wake applies to a receiver blocked on an
  1068	**empty** peer end.
  1069	
  1070	**Teardown ordering (pinned, three steps, in order).** For the dying process's
  1071	**own** waiter (a thread of the dying VM that was itself blocked in `recv`):
  1072	1. **FIRST**, before the release walk runs, **unlink/tombstone the dying process's
  1073	   own waiter** (clear its waiter fields, release any head reservation it holds)
  1074	   and leave its thread `DYING` — the walk and the ISR must never `arbitrate` or
  1075	   wake it. `je_ipc_wait_arbitrate` **skips a tombstoned/non-`WAITING` waiter**
  1076	   (returns "lost").
  1077	2. **THEN** run the release walk, which wakes only **surviving peer-owned**
  1078	   waiters (step above) — never the dying process's own (already tombstoned).
  1079	3. The guarded scheduler flip (A6-5: `THREAD_BLOCKED_IPC → READY` only) means even
  1080	   a stray stamp on the dying thread could not make it `READY` from `DYING`.
  1081	This ordering guarantees no wake writes a frame slot that teardown is reusing
  1082	(D3.3). **Owner-approved design.**
  1083	
  1084	**Predictions if other choices were made.** *Rely on the receiver's own deadline /
  1085	next schedule to notice the close:* a receiver blocked with a far deadline on an
  1086	endpoint whose peer just died would sleep uselessly until timeout — a latency bug
  1087	and a semantic one (it should learn `PEER_CLOSED` now, not `TIMED_OUT` later).
  1088	*Unlink the dying process's own waiter AFTER the release walk (or not at all):*
  1089	the walk could arbitrate-and-flip the dying thread, racing slot reuse — the exact
  1090	resurrection the review flagged; unlinking FIRST + the guarded flip + arbitrate-
  1091	skips-tombstoned closes all three. *Waking the dying (CLOSED) end's waiter instead
  1092	of the surviving (PEER_CLOSED) end's:* names the wrong end — the thread to wake is
  1093	in the living process, on the end that just became PEER_CLOSED.
  1094	
  1095	**Flex options to consider in future.** Firm; symmetric with the step-3 release
  1096	walk it extends. Also covers an ordinary last-handle `endpoint_close` (no process
  1097	death) that transitions a peer end to PEER_CLOSED: the same arbitrate-the-peer's-
  1098	waiter call applies on that close path, not only in `je_vm_destroy`.
  1099	
  1100	### A6-CLARIFY-2. Death/reap releases a held WOKEN reservation (owner-approved 2026-10-19)
  1101	
  1102	**Decision + why.** A6-5 reserves the queue head for a receiver stamped `WOKEN`,
  1103	delivered when its continuation resumes. There is a window — the woken thread is
  1104	READY but not yet re-entered (from "READY, not yet installed" through the `iretq`
  1105	to the `int 0x80` restart site; `je_uframe_check` sets IF=1, so a timer tick — and,
  1106	in a future milestone, a kill/cancel — can land there) — in which the reservation
  1107	is **held by a thread that has not yet consumed it**. If that thread is destroyed
  1108	in the window, the reserved head would be stranded (marked reserved, no live waiter
  1109	→ a permanent per-end denial of delivery). So teardown MUST release it. To avoid a
  1110	double-release across the three candidate sites (`mark_dying`, the reaper, and
  1111	A6-PEERWAKE step 1), there is **exactly one idempotent cleanup routine**,
  1112	`je_ipc_waiter_release(slot)`, that **checks whether THIS waiter holds the
  1113	reservation and clears it only then** (clearing the waiter state + returning the
  1114	head to deliverable). **`mark_dying` calls it; the reaper only asserts it is
  1115	already clear; A6-PEERWAKE step 1 is the same routine.** CLARIFY-2 is simply the
  1116	`READY`/`WOKEN` case of that one routine. It runs in the IRQ-critical teardown
  1117	**before the release walk** (the A6-PEERWAKE ordering — if the walk ran first it
  1118	could close an end or discard queued messages that are still reserved). The
  1119	released head **becomes deliverable at its queue front** — it was never dequeued
  1120	(the reservation is only a "claimed-by" marker on the still-enqueued head), so it
  1121	stays FIFO-first for the next receiver. **The waiter slot stays occupied from
  1122	`WOKEN`-stamp until consume-or-release**, so A6-4 gives any concurrent second
  1123	receiver `EMPTY` (nobody can park behind a reserved head and then sleep to its
  1124	timeout — the latency bug A6-PEERWAKE exists to prevent); release frees the slot
  1125	and the head together.
  1126	
  1127	**Reachability, honestly.** There is **no user/ABI-triggered *production* death
  1128	path** into this window in v1 — there is no kill/cancel op, and the pre-park
  1129	`je_uframe_check` (A6-CLARIFY-1) rejects the corrupt-return-frame case at park time
  1130	rather than letting it die at wake. The only way to reach it in v1 is a
  1131	**test-only forced-kill hook** (named + gated exactly like the existing dev-only
  1132	`JE_DEV_COPY_INJECT`, never in the production ABI) that marks a READY thread
  1133	`DYING` so the reaper can reclaim it — used solely by the CLARIFY-2 proof. So this
  1134	closes a hole that is currently reachable **only under the test hook**, and that a
  1135	future real cancel/kill (the A6-2 signal milestone) would otherwise expose.
  1136	**Owner-approved.**
  1137	
  1138	**Predictions if other choices were made.** *Leave it unspecified (the A6 gap as
  1139	drafted):* the first future kill/cancel path strands the reserved head
  1140	permanently — a silent per-end denial of delivery; cheaper to close now. *Two
  1141	release sites instead of one idempotent owner:* a double-release / double-free of
  1142	the reservation when `mark_dying` and the reaper (or PEERWAKE) both fire — the bug
  1143	the exactly-once holder-checked routine removes. *Freeing the waiter slot at
  1144	`WOKEN`-stamp instead of at consume/release:* a second receiver could park behind
  1145	the reserved head and sleep to timeout — the latency bug; keeping the slot occupied
  1150	**Flex options to consider in future.** Firm for v1. When a real cancel/kill path
  1151	arrives (the A6-2 signal milestone), this release is the cleanup it must call —
  1152	the test added now (wake → destroy the holder before re-entry → the head is
  1153	delivered to another receiver) already exercises the path via the reaper, so the
  1154	future canceller inherits a tested cleanup.
  1155	
  1156	Each terminal cause gets a deterministic kernel/hosted proof that does **not**
  1157	rely on two spin-looping user stubs (the step-5 hang lesson). All blocking triggers
  1158	in v1 are driven by **kernel-side helpers / explicit yields / stepping
  1159	`je_timer_ticks` / stepping the bootstrap reaper** — there is no ABI self-trigger
  1160	(no signals, no async kill), so every proof is a deterministic sequence, not a
  1161	race:
  1162	- **queued-before-recv:** message already present → immediate `WOKEN`, no park.
  1163	- **WOKEN:** thread X parks on recv; thread Y sends then *explicitly yields* →
  1164	  X resumes, exactly one `WOKEN`, FIFO delivery.
  1165	- **zero-timeout poll:** R10=0 on an OPEN empty end → `EMPTY` (13), X never parks
  1166	  (asserts the A6-3 short-circuit: no waiter registered, no yield).
  1167	- **TIMED_OUT:** X parks (nonzero R10); advance `je_timer_ticks` to the inclusive
  1168	  deadline → ISR arbitrates `TIMED_OUT` (status 14), X resumes with it.
  1169	- **PEER_CLOSED:** X (in process A) parks on an empty end whose peer is held by
  1170	  process B; **step the bootstrap reaper to run `je_vm_destroy(B)`** → the release
  1171	  walk arbitrates `PEER_CLOSED` (12) on A's surviving end *before* A's deadline.
  1172	- **CANCELLED:** the internal `je_ipc_wait_cancel(waiter)` helper (A6-2, no ABI
  1173	  path) → exactly one `CANCELLED` (15).
  1174	- **copy-out fault after WOKEN:** `JE_ABI_INVALID` (2), message stays queued,
  1175	  reservation released; a *subsequent* recv on the end takes the still-queued head
  1176	  (NOT a re-arm of the faulted waiter).
  1177	- **teardown while blocked:** X (in B) parks, then a **test-only forced-kill hook**
  1178	  (dev-gated like `JE_DEV_COPY_INJECT`, never production ABI) marks B's READY/parked
  1179	  thread `DYING` and the **reaper is stepped** → B's own waiter is released via the
  1180	  single idempotent `je_ipc_waiter_release` FIRST (before the release walk), before
  1181	  the thread/frame slot is reused; the guarded flip never makes the `DYING` thread
  1182	  `READY`.
  1183	- **reservation release on death-before-re-entry (A6-CLARIFY-2):** X is woken
  1184	  `WOKEN` and its head reserved; the **test-only forced-kill hook** marks X `DYING`
  1185	  and the reaper is stepped **before** X's continuation re-enters → the single
  1186	  idempotent release returns the reserved head to its queue front, and a *second*
  1187	  receiver (holding a transferred dup of the end, the A6-4 reachable case) then
  1188	  calls recv **after** the release and takes it — proving the reservation was
  1189	  released, not stranded, and that the slot freed so the second receiver no longer
  1190	  gets `EMPTY`.
  1191	- **ordered serialization (not a "tie"):** on one CPU two causes are never
  1192	  simultaneous — prove the two orderings explicitly: (a) enqueue's CAS fires, THEN
  1193	  a timer tick → X gets `WOKEN`, the later timeout CAS is a no-op, message
  1194	  delivered; (b) timeout CAS fires, THEN a late enqueue → X gets `TIMED_OUT`, the
  1195	  message remains queued for a future recv. Assert exactly one terminal result in
  1196	  each; the entry-snapshot (non-empty queue + peer closed) separately asserts the
  1197	  A6-1 drain-first pick.
  1198	
  1199	**What is actually proven in the step-6 PR (honest status).** The **end-to-end
  1200	ring-3 path** is proven by the kernel self-test **`M3c-s6`** for the **TIMED_OUT**
  1201	cause: a single ring-3 thread calls `recv(timeout)` on an end nobody sends to, it
  1202	genuinely parks (`BLOCKED_IPC`), the timer ISR advances `je_timer_ticks` past the
  1203	deadline and performs the terminal CAS, the continuation prologue re-enters at the
  1204	restart site and returns status 14, the waiter is released, and the channel
  1205	reclaims to 0. This exercises the whole mechanism — park, the ISR terminal
  1206	transition, the 2-byte restart resume, the continuation delivery, waiter release —
  1207	with ONE ring-3 thread and the natural timer tick (no second spinning process; the
  1208	step-5 cooperative-scheduler spin-hang lesson). The **WOKEN / PEER_CLOSED /
  1209	CANCELLED** causes, the **single-terminal-CAS / first-wins**, the **reserved-head
  1210	masking**, **A6-4 WAITERS_BUSY**, and **A6-CLARIFY-2 release-returns-head** are
  1211	proven **deterministically in the hosted pure tests** (`test_endpoint` 27–28) plus
  1212	the decode matrix (`test_syscall`). The **full two-process ring-3 WOKEN handshake**
  1213	(thread X blocks, a *second* ring-3 process sends and yields, X wakes) is **deferred
  1214	to step 7's bootstrap fixture** — the same honest deferral as the step-5 ring-3
  1215	two-process transfer, for the same reason (multi-process ring-3 choreography is
  1216	step 7's subject; a spin-based two-stub proof races the single-CPU cooperative
  1217	scheduler). The wake-on-send and wake-on-peer-close *kernel bridges* are wired and
  1218	compiled; what step 7 adds is the ring-3 *scheduling* of two cooperating user
  1219	processes to drive them end-to-end.

 succeeded in 0ms:
include/jeios/endpoint.h:56:#define JE_EP_MAX_WAITERS   2u    /* waiters per end (unused this PR) */
include/jeios/endpoint.h:63: * DEPTH*K <= JE_EP_REF_MAX so a full queue of cap-carrying messages cannot
include/jeios/endpoint.h:70:#define JE_EP_REF_MAX       8u
include/jeios/endpoint.h:101:    JE_EP_REFOVERFLOW,  /* acquiring past JE_EP_REF_MAX; nothing acquired */
include/jeios/endpoint.h:165:} je_ep_waiter;
include/jeios/endpoint.h:182:    je_ep_waiter    waiter;         /* M3c A6: at most one blocking receiver (A6-4) */
include/jeios/endpoint.h:198:} je_ep_channel;
include/jeios/endpoint.h:203:    je_ep_channel rec[JE_EP_POOL_CAP];
include/jeios/endpoint.h:246: * JE_EP_REF_MAX — acquires nothing), then je_handle_alloc the new slot
include/jeios/endpoint.h:257: * directly here. retain_inflight fails REFOVERFLOW past JE_EP_REF_MAX (acquires
include/jeios/endpoint.h:263:                                 je_ep_channel **out_rec, uint8_t *out_side);
include/jeios/endpoint.h:265:                                         je_ep_channel *rec, uint8_t side);
include/jeios/endpoint.h:267:                                          je_ep_channel *rec, uint8_t side);
include/jeios/endpoint.h:305:    je_ep_channel *rec;
include/jeios/endpoint.h:379:                                         je_ep_channel **out_rec, uint8_t *out_side);
include/jeios/endpoint.h:397:je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side);
include/jeios/endpoint.h:400:uint8_t         je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side);
include/jeios/endpoint.h:442:                                       je_ep_channel **out_rec, uint8_t *out_side);
include/jeios/endpoint.h:451:je_ep_arb_result je_endpoint_wait_arbitrate(je_ep_channel *rec, uint8_t side,
include/jeios/endpoint.h:457:je_ep_wait_cause je_endpoint_wait_cause(const je_ep_channel *rec, uint8_t side);
include/jeios/endpoint.h:466:bool je_endpoint_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who);
include/jeios/endpoint.h:479:je_ep_status je_endpoint_recv_peek_owned_caps(je_endpoint_pool *pool, je_ep_channel *rec,
include/jeios/sched.h:8:#include <jeios/endpoint.h>   /* je_ep_channel, for the blocking-recv waiter API */
include/jeios/sched.h:40:je_trap_frame *je_sched_block_current(je_trap_frame *frame, je_ep_channel *rec,
include/jeios/sched.h:46:bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side, uint64_t *rdi,
    90	/* Status. Distinct enum (no ABI coupling); the glue maps these to je_abi_status
    91	 * names (JE_ABI_H_* + JE_ABI_H_REFOVERFLOW) at the syscall boundary in a later
    92	 * PR, exactly as handle.c's je_handle_status does. */
    93	typedef enum {
    94	    JE_EP_OK = 0,
    95	    JE_EP_BADINDEX,     /* handle/arg malformed, or not an endpoint handle */
    96	    JE_EP_STALE,        /* handle no longer live */
    97	    JE_EP_WRONGTYPE,    /* live slot, not an endpoint */
    98	    JE_EP_DENIED,       /* rights subset check failed (dup) */
    99	    JE_EP_QUOTA,        /* table live-slot quota reached */
   100	    JE_EP_NOSLOTS,      /* no FREE table slot, or pool exhausted */
   101	    JE_EP_REFOVERFLOW,  /* acquiring past JE_EP_REF_MAX; nothing acquired */
   102	    JE_EP_QUEUE_FULL,   /* try_send: peer inbound at JE_EP_QUEUE_DEPTH */
   103	    JE_EP_SHORT_BUFFER, /* try_recv: caller buffer < head message; not dequeued */
   104	    JE_EP_PEER_CLOSED,  /* try_send to a peer-closed end, or try_recv drained+EOF */
   105	    JE_EP_EMPTY,        /* try_recv: OPEN end, empty queue (would block) */
   106	    JE_EP_INVALID,      /* malformed pure-layer arg (e.g. len > JE_EP_MSG_MAX) */
   107	    JE_EP_WAITERS_BUSY, /* M3c A6-4: the end's single waiter slot is occupied; the
   108	                         * glue maps this to a non-blocking EMPTY (never parks,
   109	                         * never a new ABI status). */
   110	    JE_EP_CORRUPT       /* an object pointer that is not a valid pool end
   111	                         * (should be impossible; fail closed, never deref) */
   112	} je_ep_status;
   113	
   114	/* A queued capability descriptor (ADR 0009 A5): a direct reference to a
   115	 * transferred endpoint END (by pool index + side + generation) plus the
   116	 * attenuated rights the receiver will mint. NOT a handle (handles are per-table).
   117	 * Each queued descriptor holds ONE inflight ref on rec[rec_index].end[side].
   118	 * `generation` is a defence-in-depth tripwire; it matches while the inflight ref
   119	 * keeps the record allocated, and distinguishes a since-reused record. */
   120	typedef struct {
   121	    uint16_t rec_index;    /* index into the owning pool's rec[] */
   122	    uint8_t  side;         /* which end of that record (0/1) */
   123	    uint8_t  cap_count_pad;/* reserved/alignment */
   124	    uint32_t generation;   /* rec->generation when queued */
   125	    uint32_t rights;       /* attenuated rights to mint (subset of source) */
   126	} je_ep_capdesc;
   127	
   128	/* One inbound message queue (a ring buffer of fixed-max-size messages). Each slot
   129	 * carries payload bytes AND 0..JE_EP_MSG_CAPS_MAX capability descriptors (A5). */
   130	typedef struct {
   131	    uint8_t       buf[JE_EP_QUEUE_DEPTH][JE_EP_MSG_MAX];
   132	    uint8_t       len[JE_EP_QUEUE_DEPTH];       /* payload bytes per slot */
   133	    je_ep_capdesc cap[JE_EP_QUEUE_DEPTH][JE_EP_MSG_CAPS_MAX];
   134	    uint8_t       cap_count[JE_EP_QUEUE_DEPTH];  /* caps per slot (0..CAPS_MAX) */
   135	    uint8_t       head;                          /* index of the oldest message */
   136	    uint8_t       count;                         /* number queued (0..DEPTH) */
   137	} je_ep_queue;
   138	
   139	/* An end's lifecycle state (ADR 0009 D2). */
   140	/* M3c D7/A6: the terminal cause of a blocking recv wait. WAITING is the initial
   141	 * (non-terminal) state; the other four are terminal and set by exactly one CAS
   142	 * (je_ipc_wait_arbitrate). Priority among causes eligible in ONE entry snapshot
   143	 * is A6-1 drain-first (a queued message => WOKEN even if peer closed); once
   144	 * parked, first-critical-section-wins and the later causes are no-ops. */
   145	typedef enum {
   146	    JE_EP_WAIT_NONE = 0,   /* no waiter registered on this end */
   147	    JE_EP_WAIT_WAITING,    /* parked, no terminal cause yet */
   148	    JE_EP_WAIT_WOKEN,      /* a message became deliverable (head reserved) */
   149	    JE_EP_WAIT_TIMED_OUT,  /* deadline elapsed (timer ISR) */
   150	    JE_EP_WAIT_PEER_CLOSED,/* peer end closed with this end's inbound empty */
   151	    JE_EP_WAIT_CANCELLED   /* internal cancel (A6-2; no ABI trigger in v1) */
   152	} je_ep_wait_cause;
   153	
   154	/* Per-end waiter (A6-4: at most ONE per end). `present` gates the slot; `waiter_id`
   155	 * is an OPAQUE token the scheduler assigns (the thread slot) so the pure endpoint
   156	 * layer can name whom to wake WITHOUT depending on sched.c. `cause` is the single
   157	 * terminal CAS target. `head_reserved` marks that the current inbound head is
   158	 * claimed by this waiter (WOKEN): it is NOT dequeued, just not deliverable to any
   159	 * other receiver until consume (commit) or release (A6-5/A6-CLARIFY-2). */
   160	typedef struct {
   161	    bool             present;
   162	    uint32_t         waiter_id;
   163	    je_ep_wait_cause cause;
   164	    bool             head_reserved;
   165	} je_ep_waiter;
   166	
   167	typedef enum {
   168	    JE_EP_END_FREE = 0,   /* the record is not allocated */
   169	    JE_EP_END_OPEN,       /* live; peer is live */
   170	    JE_EP_END_PEER_CLOSED,/* live; peer has gone (reads drain then EOF) */
   171	    JE_EP_END_CLOSED      /* this end has no handle refs left */
   172	} je_ep_end_state;
   173	
   174	/* One end of a channel. `inbound` holds messages sent by the PEER and waiting for
   175	 * THIS end's try_recv (Model A). Waiter storage is reserved for step 6. */
   176	typedef struct {
   177	    je_ep_end_state state;
   178	    uint16_t        handle_refs;    /* LIVE handle slots naming this end */
   179	    uint16_t        inflight_refs;  /* queued caps + waiters */
   180	    uint8_t         side;           /* 0 or 1; peer is record->end[side ^ 1] */
   181	    je_ep_queue     inbound;        /* messages awaiting this end's try_recv */
   182	    je_ep_waiter    waiter;         /* M3c A6: at most one blocking receiver (A6-4) */
   183	} je_ep_end;
   184	
   185	/* A channel record: both ends in one allocation, so a peer pointer is always to
   186	 * the same record and can never dangle (ADR 0009 D2). `generation` is INTERNAL
   187	 * double-free / stale-pool-index defence, bumped on free, never exposed to ring
   188	 * 3 (handles bind &end[i], not a pool index), per ADR 0009 D-OWNER-1. NOTE: this
   189	 * lifecycle PR bumps it but does not yet READ it — the borrowed-pointer rule
   190	 * (ADR 0009 D3) keeps stale (rec,side) pairs out of the only refcount APIs that
   191	 * take a raw record, so there is nothing to detour yet. The queue/wait PR that
   192	 * first holds such a pair across a possible free MUST carry+check this
   193	 * generation; until then it is reserved structure, honestly unused. */
   194	typedef struct {
   195	    bool     allocated;      /* record is in use */
   196	    uint32_t generation;     /* bumped on free; internal only */
   197	    je_ep_end end[2];
   198	} je_ep_channel;
   199	
   200	/* The pool. Caller-supplied storage (hostable): the kernel owns one static
   201	 * instance; each hosted test owns a local one. No global state in endpoint.c. */
   202	typedef struct {
   203	    je_ep_channel rec[JE_EP_POOL_CAP];
   204	    uint32_t      allocated_count;  /* live records (accounting) */
   205	} je_endpoint_pool;
   206	
   207	/* Initialize a pool: every record FREE, generation 1 (never 0), counts zero.
   208	 * MUST be called once per pool lifetime. */
   209	void je_endpoint_pool_init(je_endpoint_pool *pool);
   210	
   300	                                  je_handle h, const void *kbuf, size_t len);
   301	
   302	/* A recv lease token (D6 / endpoint.h generation-carry rule): identifies the
   303	 * exact head message a peek observed, so commit dequeues only that message. */
   304	typedef struct {
   305	    je_ep_channel *rec;
   306	    uint8_t        side;
   307	    uint32_t       generation;  /* rec->generation at peek time */
   308	    uint8_t        head;        /* queue head index at peek time */
   309	    uint8_t        len;         /* head message length at peek time */
   310	} je_ep_recv_lease;
   311	
   312	/* try_recv PEEK: resolve the handle (needs RECV), apply the state/empty rules,
   313	 * and — on OK — copy the head message into out_kbuf (kernel memory, up to cap
   314	 * bytes) WITHOUT dequeuing, writing *out_len = message length and filling *lease.
   315	 * If cap < message length: returns JE_EP_SHORT_BUFFER, writes *out_len = required
   316	 * length, copies nothing, no lease. Empty+OPEN -> JE_EP_EMPTY; empty+peer-closed
   317	 * -> JE_EP_PEER_CLOSED (EOF); nonempty -> deliver regardless of peer state (drain
   318	 * first). Copies into a CALLER buffer (the glue's kernel stack), so the glue
   319	 * never holds a pointer into pool storage across copy_to_user. */
   320	je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
   321	                                   je_handle h, void *out_kbuf, size_t cap,
   322	                                   size_t *out_len, je_ep_recv_lease *lease);
   323	
   324	/* try_recv COMMIT: dequeue the message the lease identifies. Called by the glue
   325	 * ONLY after a successful copy_to_user. INFALLIBLE for a valid lease: if the
   326	 * lease no longer matches (record freed/reused/mutated — impossible under IF=0
   327	 * via ordinary API), it returns JE_EP_CORRUPT and the caller MUST treat that as
   328	 * fatal (a retry would double-deliver). Zeroes the vacated slot. */
   329	je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
   330	                                     const je_ep_recv_lease *lease);
   331	
   332	/* ---- Capability transfer (M3c step 5, ADR 0009 A5) ------------------------
   333	 * These extend send/recv to carry capabilities. The data-only forms above are
   334	 * the caps==0 case and remain valid. The glue resolves user handles to
   335	 * descriptors (send) and mints handles from descriptors (recv); the pure layer
   336	 * manages the queued-cap inflight refs and the queue storage. */
   337	
   338	/* send PREFLIGHT with caps: as je_endpoint_send_preflight, plus it validates that
   339	 * ncaps <= JE_EP_MSG_CAPS_MAX and each descriptor's target is a live endpoint in
   340	 * THIS pool with only known rights bits. Does NOT retain or mutate. (The GLUE
   341	 * attenuates each cap against the SENDER's handle rights — requiring TRANSFER and
   342	 * a subset — when it builds the descriptors; ends carry no rights, so the pure
   343	 * layer only checks liveness + known-bits.) Returns the first failing status. */
   344	je_ep_status je_endpoint_send_preflight_caps(je_endpoint_pool *pool,
   345	                                             je_handle_table *t, je_handle h,
   346	                                             size_t len,
   347	                                             const je_ep_capdesc *caps,
   348	                                             uint8_t ncaps);
   349	
   350	/* try_send with caps: preflight (re-check), RETAIN one inflight ref per cap on
   351	 * each target (REFOVERFLOW rolls back all retained-so-far), then enqueue the
   352	 * message carrying the byte payload AND the cap descriptors into the peer inbound.
   353	 * All-or-nothing: on any failure nothing is enqueued and no inflight ref is left
   354	 * retained. `caps`/`ncaps` may be 0 (the data-only case). */
   355	je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *t,
   356	                                       je_handle h, const void *kbuf, size_t len,
   357	                                       const je_ep_capdesc *caps, uint8_t ncaps);
   358	
   359	/* recv PEEK with caps: as je_endpoint_recv_peek, plus it reports the head
   360	 * message's cap_count via *out_ncaps and copies the cap descriptors into
   361	 * out_caps[0..CAPS_MAX) (kernel memory). If the caller's handle-slot capacity
   362	 * (hcap) < cap_count, returns JE_EP_SHORT_BUFFER with *out_ncaps = required
   363	 * (alongside the byte-length rule). No dequeue, no mint. */
   364	je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
   365	                                        je_handle h, void *out_kbuf, size_t cap,
   366	                                        size_t *out_len, uint8_t hcap,
   367	                                        je_ep_capdesc *out_caps, uint8_t *out_ncaps,
   368	                                        je_ep_recv_lease *lease);
   369	
   370	/* Classify a peeked cap descriptor: is its target still LIVE (mintable) or dead
   371	 * (CLOSED / generation-stale -> deliver handle 0, A5-2)? Pure, read-only. */
   372	bool je_endpoint_capdesc_live(const je_endpoint_pool *pool,
   373	                              const je_ep_capdesc *d);
   374	
   375	/* Resolve a LIVE cap descriptor to its (rec, side) for the glue's mint. Returns
   376	 * JE_EP_OK + the end, or JE_EP_STALE/CORRUPT for a dead/invalid descriptor. */
   377	je_ep_status je_endpoint_capdesc_resolve(je_endpoint_pool *pool,
   378	                                         const je_ep_capdesc *d,
   379	                                         je_ep_channel **out_rec, uint8_t *out_side);
   380	
   381	/* Retain one handle_ref on a cap descriptor's target (the glue's destination mint
   382	 * uses this before alloc'ing the receiver handle; REFOVERFLOW if at the bound). */
   383	je_ep_status je_endpoint_capdesc_retain_handle(je_endpoint_pool *pool,
   384	                                               const je_ep_capdesc *d);
   385	/* Undo a capdesc_retain_handle (mint rollback). Infallible for a valid retain. */
   386	void je_endpoint_capdesc_release_handle(je_endpoint_pool *pool,
   387	                                        const je_ep_capdesc *d);
   388	
   389	/* recv COMMIT with caps: dequeue the leased head message AND release the inflight
   390	 * ref each of its queued caps held. Called by the glue ONLY after the byte + the
   391	 * handle outputs have been copied out (or the dead-cap zeros staged). Infallible
   392	 * for a valid lease (mismatch -> CORRUPT, fatal). */
   393	je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
   394	                                          const je_ep_recv_lease *lease);
   395	
   396	/* Read-only accessors for tests/assertions (never mutate). */
   397	je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side);
   398	uint32_t        je_endpoint_pool_allocated(const je_endpoint_pool *pool);
   399	/* Inbound queue depth for a resolved end (tests). */
   400	uint8_t         je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side);
   401	
   402	/* Process-death RELEASE WALK (ADR 0009 D-OWNER-2 + AMENDMENT A3-1): drop every
   403	 * LIVE endpoint handle in table `t`, closing peers and freeing now-unreferenced
   404	 * channels, so a dying process leaks no pool record. Enumerates `t` via the
   405	 * read-only je_handle_live_at iterator and closes each endpoint slot through
   406	 * je_endpoint_handle_close (never writes `t->slots` directly, never touches any
   407	 * other table — a surviving peer only sees its end go PEER_CLOSED). Idempotent
   408	 * by construction: each slot owns exactly one handle_ref and is invalidated
   409	 * before its ref drops. Returns the number of endpoint handles released. A
   410	 * resolve/close failure on a slot the iterator just reported LIVE is an
   411	 * invariant violation: the caller MUST treat a negative return as fatal (the
   412	 * record pool is corrupt) and stop before any table-zeroing. On success every
   413	 * endpoint slot in `t` is guaranteed no longer LIVE. MUST run with IF=0 (the
   414	 * caller — je_vm_destroy — holds the IRQ-disabled critical section, A3-1). */
   415	int je_endpoint_release_table(je_endpoint_pool *pool, je_handle_table *t);
   416	
   417	/* ---- M3c D7/A6: blocking-recv waiter primitives (PURE; sched.c owns the thread
   418	 * side, endpoint_kern.c bridges the wake into the scheduler). ---- */
   419	
   420	/* Outcome of the single terminal CAS (je_ipc_wait_arbitrate). WON means THIS call
   421	 * performed the WAITING->cause transition (the caller now owns the follow-up: a
   422	 * WOKEN winner reserved the head; the bridge must wake waiter_id). LOST means the
   423	 * waiter was absent or already terminal (no-op). */
   424	typedef enum { JE_EP_ARB_LOST = 0, JE_EP_ARB_WON = 1 } je_ep_arb_result;
   425	
   426	/* ENTRY SNAPSHOT (A6-ABI): evaluate an OPEN end once, WITHOUT registering a
   427	 * waiter, exactly as non-blocking try_recv would — used by both the zero-timeout
   428	 * poll and the first check of a blocking recv. Returns OK (deliverable: caller
   429	 * proceeds to the normal peek/mint/copy), SHORT_BUFFER, PEER_CLOSED, or EMPTY
   430	 * (would block). Never parks, never reserves. Pure wrapper over the resolve +
   431	 * state/empty rules the recv peek already applies. */
   432	je_ep_status je_endpoint_recv_entry_probe(je_endpoint_pool *pool, je_handle_table *t,
   433	                                          je_handle h);
   434	
   435	/* REGISTER a waiter on the end named by handle `h` (needs RECV). Fails
   436	 * JE_EP_QUEUE_FULL-equivalent (returns JE_EP_WAITERS_BUSY) if the end already has
   437	 * a present waiter (A6-4 per-end bound; caller then returns EMPTY, never parks).
   438	 * On OK the end holds {present, waiter_id, cause=WAITING, head_reserved=false}.
   439	 * Does NOT block the thread — sched.c does that after this returns OK. */
   440	je_ep_status je_endpoint_wait_register(je_endpoint_pool *pool, je_handle_table *t,
   441	                                       je_handle h, uint32_t waiter_id,
   442	                                       je_ep_channel **out_rec, uint8_t *out_side);
   443	
   444	/* The single terminal CAS. On the end (rec,side): if a waiter is present and in
   445	 * WAITING, set `cause` (WOKEN additionally reserves the inbound head) and return
   446	 * WON + *out_waiter_id; otherwise return LOST. MUST run under IF=0. This is the
   447	 * ONLY path that leaves WAITING. Callers: enqueue (WOKEN), peer-close
   448	 * (PEER_CLOSED), timer (TIMED_OUT), internal cancel (CANCELLED). A6-1 drain-first
   449	 * is enforced by the callers' eligibility (enqueue only arbitrates WOKEN when it
   450	 * actually queued a message; close only arbitrates PEER_CLOSED on an empty end). */
   451	je_ep_arb_result je_endpoint_wait_arbitrate(je_ep_channel *rec, uint8_t side,
   452	                                            je_ep_wait_cause cause,
   453	                                            uint32_t *out_waiter_id);
   454	
   455	/* Read the terminal cause a waiter was stamped with (the continuation prologue
   456	 * maps it to the ABI status). Returns JE_EP_WAIT_NONE if no waiter present. */
   457	je_ep_wait_cause je_endpoint_wait_cause(const je_ep_channel *rec, uint8_t side);
   458	
   459	/* RELEASE a waiter (A6-CLARIFY-2 single idempotent owner): if the end's present
   460	 * waiter has waiter_id == `who`, clear it; if it held a reserved head, un-reserve
   461	 * (the head stays enqueued, becomes deliverable again at the queue front). A
   462	 * waiter_id mismatch or no-present-waiter is a safe no-op (idempotent). Returns
   463	 * true iff it cleared THIS waiter. MUST run under IF=0. Called by: a successful
   464	 * recv commit (consume), a copy-fault rollback, and thread teardown
   465	 * (mark_dying/reaper) — the reaper only ASSERTS it is already clear. */
   466	bool je_endpoint_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who);
   467	
   468	/* Scan-free helper for teardown: find the end (if any) in the WHOLE pool whose
   469	 * present waiter has waiter_id == `who`, and release it (as above). Returns true
   470	 * iff one was found+released. Used by mark_dying, which knows the thread slot

 succeeded in 0ms:
diff --git a/kernel/core/sched.c b/kernel/core/sched.c
index 423ff8a..168b59f 100644
--- a/kernel/core/sched.c
+++ b/kernel/core/sched.c
@@ -5,8 +5,15 @@
 #include <jeios/sched.h>
 #include <jeios/uframe.h>
 #include <jeios/vm.h>
+#include <jeios/endpoint.h>
+#include <jeios/endpoint_kern.h>   /* je_endpoint_kern_waiter_release_by_id */
 #define STACK_PAGES 5u
-typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING } state;
+/* M3c A6-CLARIFY-1: the fixed length of the `int 0x80` syscall gate instruction
+ * (`CD 80`), rewound on park so the woken thread re-executes it. Architecturally
+ * fixed by the ABI (Intel SDM: INT imm8 = CD ib). The only DPL3 IDT gate, and the
+ * PIC is remapped off 0x80, so rip-JE_INT80_LEN provably points at the site. */
+#define JE_INT80_LEN 2u
+typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING, BLOCKED_IPC } state;
 typedef enum { THREAD_KERNEL = 0, THREAD_USER = 1 } thread_kind;
 typedef struct {
     state state;
@@ -27,6 +34,19 @@ typedef struct {
      * return state (read by the bootstrap reaper and the proofs). */
     uint64_t death_vector, death_error, death_rip, death_cs, death_cr2;
     uint64_t death_reason, exit_code;
+    /* M3c D7/A6: blocking-recv continuation ("constrained restartable syscall",
+     * A6-CLARIFY-1). `pending` gates the continuation prologue in dispatch. When a
+     * recv parks: pending=1, the recv ARGS (rdi..r9) are saved, the OWNED endpoint
+     * record+side are saved (so resume never re-resolves the handle — A6-5), the
+     * deadline (absolute ticks; 0 = none) is set, and the post-int RIP is saved to
+     * assert the restart landed where expected. Cleared on continuation completion
+     * or on wait release. */
+    bool     wait_pending;
+    je_ep_channel *wait_rec;
+    uint8_t  wait_side;
+    uint64_t wait_rdi, wait_rsi, wait_rdx, wait_r8, wait_r9;
+    uint64_t wait_deadline;      /* absolute tick; 0 = no deadline (shouldn't park) */
+    uint64_t wait_post_int_rip;  /* f->rip at park (the instruction after int 0x80) */
 } thread;
 /* Death reasons (M3a #5). FAULT: synchronous user fault. EXIT: clean exit(code).
  * BAD_RETURN: a user frame failed je_uframe_check (invalid return state). */
@@ -73,7 +93,8 @@ je_status je_thread_create(void (*entry)(void *),void *arg)
     *(uintptr_t *)rsp=(uintptr_t)je_thread_exit;
     je_trap_frame *f=(void *)(rsp-sizeof(*f));
     *f=(je_trap_frame){.rip=(uintptr_t)trampoline,.cs=8,.rflags=0x202,.rsp=rsp,.ss=16};
-    threads[slot]=(thread){READY,f,entry,arg,base,0,0,THREAD_KERNEL,0,0,0,0,0,0,0,0,0,0,0};
+    threads[slot]=(thread){READY,f,entry,arg,base,0,0,THREAD_KERNEL,0,0,0,0,0,0,0,0,0,0,0,
+                           false,0,0,0,0,0,0,0,0,0};
     return JE_OK;
 }
 
@@ -108,7 +129,8 @@ je_status je_thread_create_user(je_vm *vm, uint32_t index)
     f->rip = JE_VM_ARENA_BASE + p->entry;
     f->rsp = JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE;
     f->rflags = 0x202u; /* IF=1, bit1; validator re-sanitizes on every dispatch */
-    threads[slot]=(thread){READY,f,0,0,0,0,0,THREAD_USER,p->root,rsp0_top,vm,index,0,0,0,0,0,0,0};
+    threads[slot]=(thread){READY,f,0,0,0,0,0,THREAD_USER,p->root,rsp0_top,vm,index,0,0,0,0,0,0,0,
+                           false,0,0,0,0,0,0,0,0,0};
     return JE_OK;
 }
 /* M3a #4 step 4: mark a user thread DYING with a death record (never requeued;
@@ -117,6 +139,12 @@ je_status je_thread_create_user(je_vm *vm, uint32_t index)
 static void mark_dying(unsigned slot, death_reason reason, uint64_t code,
                        uint64_t vec, uint64_t err, uint64_t rip, uint64_t cs, uint64_t cr2)
 {
+    /* M3c A6-CLARIFY-2: release any blocking-recv waiter/reservation this thread
+     * holds BEFORE its slot is torn down, via the single idempotent release owner
+     * (keyed on the thread slot = waiter_id). Safe no-op if the thread held none.
+     * This is the one site that RELEASES; the reaper only asserts it is clear. */
+    (void)je_endpoint_kern_waiter_release_by_id((uint32_t)slot);
+    threads[slot].wait_pending=false;
     threads[slot].state=DYING;
     threads[slot].frame=0;
     threads[slot].death_reason=(uint64_t)reason; threads[slot].exit_code=code;
@@ -182,6 +210,25 @@ je_trap_frame *je_sched_interrupt(je_trap_frame *f,bool timer)
     }
     for (unsigned i=1;i<JE_THREAD_MAX;++i)
         if (threads[i].state==SLEEPING && threads[i].wake<=je_timer_ticks) threads[i].state=READY;
+    /* M3c D7/A6: on a timer tick, stamp TIMED_OUT on any blocked-IPC waiter whose
+     * absolute deadline has elapsed, through the SINGLE endpoint arbitrate CAS
+     * (first-critical-section-wins: if a sender/close already stamped it WOKEN/
+     * PEER_CLOSED this is a no-op), then make it READY so its continuation returns
+     * the cause. Inclusive deadline (wake<=ticks), matching the SLEEPING sweep. */
+    if (timer)
+        for (unsigned i=1;i<JE_THREAD_MAX;++i)
+            if (threads[i].state==BLOCKED_IPC && threads[i].wait_deadline!=0u &&
+                threads[i].wait_deadline<=je_timer_ticks) {
+                uint32_t id=0u;
+                je_ep_arb_result r=je_endpoint_wait_arbitrate(threads[i].wait_rec,
+                                                              threads[i].wait_side,
+                                                              JE_EP_WAIT_TIMED_OUT, &id);
+                /* Whether this CAS won (TIMED_OUT) or lost (already WOKEN/closed),
+                 * the waiter is now terminal: make the thread runnable. Guarded
+                 * flip, BLOCKED_IPC -> READY only. */
+                (void)r;
+                threads[i].state=READY;
+            }
     if (threads[current].state==RUNNING) threads[current].state=READY;
     unsigned before=current;
     je_trap_frame *nf=select_and_install_next();
@@ -219,6 +266,97 @@ je_trap_frame *je_sched_exit_current(je_trap_frame *f, uint64_t code)
     return select_and_install_next();
 }
 
+/* M3c D7/A6-CLARIFY-1: park the CURRENT user thread as a blocking-recv waiter and
+ * switch to a live successor. Modeled on je_sched_exit_current (same frame-identity
+ * checks) PLUS a deliberate pre-park je_uframe_check: a thread that would park MUST
+ * have a valid return frame, so a user-corrupted RSP/RFLAGS is rejected as
+ * DEATH_BAD_RETURN at park time rather than discovered at wake while it holds a
+ * reservation (A6-CLARIFY-2 window). The endpoint-side waiter was already
+ * registered by the caller (je_endpoint_kern_*); here we only save the continuation
+ * state, rewind f->rip to the int 0x80 site, set BLOCKED_IPC, and switch. The saved
+ * rec/side is the OWNED reference the continuation resumes from (never re-resolves
+ * the handle, A6-5). `deadline` is absolute ticks (0 would be a poll, which never
+ * parks — a caller passing 0 here is a bug). Returns a live successor's frame. */
+je_trap_frame *je_sched_block_current(je_trap_frame *f, je_ep_channel *rec,
+                                      uint8_t side, uint64_t deadline)
+{
+    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
+    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
+        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
+    uint64_t sane;
+    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
+        /* Corrupt return frame: do NOT park (it would hold a reservation it could
+         * never consume). Terminate now; the caller's endpoint waiter is released
+         * by mark_dying's teardown hook. */
+        mark_dying(current, DEATH_BAD_RETURN, 0, 0, 0, f->rip, f->cs, 0);
+        return select_and_install_next();
+    }
+    f->rflags = sane;
+    thread *t = &threads[current];
+    t->wait_pending = true;
+    t->wait_rec = rec; t->wait_side = side;
+    t->wait_rdi = f->rdi; t->wait_rsi = f->rsi; t->wait_rdx = f->rdx;
+    t->wait_r8 = f->r8; t->wait_r9 = f->r9;
+    t->wait_deadline = deadline;
+    t->wait_post_int_rip = f->rip;      /* instruction after int 0x80 */
+    f->rip -= JE_INT80_LEN;             /* re-execute int 0x80 on resume */
+    t->frame = f;
+    t->state = BLOCKED_IPC;
+    return select_and_install_next();
+}
+
+/* M3c D7/A6-STRUCT: wake a blocked-IPC waiter by thread slot (the opaque
+ * waiter_id the endpoint layer stored). This is a GUARDED wrapper: it flips
+ * BLOCKED_IPC -> READY ONLY (never from any other state), so it can never
+ * resurrect a DYING/ZOMBIE thread (A6-5) and never races an independent
+ * transition. The terminal cause was already set by the single endpoint
+ * arbitrate CAS BEFORE this is called; this only makes the thread runnable so its
+ * continuation can read that cause. MUST run under IF=0. */
+void je_sched_wake_ipc(uint32_t slot)
+{
+    if (slot == 0u || slot >= JE_THREAD_MAX) return;
+    if (threads[slot].state == BLOCKED_IPC) threads[slot].state = READY;
+}
+
+/* M3c A6: the continuation prologue (syscall.c) queries whether the CURRENT thread
+ * resumed from a blocking-recv park, and reads back the saved continuation state
+ * (owned rec/side + the recv args + the terminal RIP to assert). Returns false if
+ * no continuation is pending. Does NOT clear pending (the glue clears it via
+ * je_sched_wait_finish once the result is delivered). */
+bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side, uint64_t *rdi,
+                           uint64_t *rsi, uint64_t *rdx, uint64_t *r8, uint64_t *r9,
+                           uint64_t *post_int_rip)
+{
+    thread *t = &threads[current];
+    if (!t->wait_pending) return false;
+    if (rec) *rec = t->wait_rec;
+    if (side) *side = t->wait_side;
+    if (rdi) *rdi = t->wait_rdi;
+    if (rsi) *rsi = t->wait_rsi;
+    if (rdx) *rdx = t->wait_rdx;
+    if (r8)  *r8  = t->wait_r8;
+    if (r9)  *r9  = t->wait_r9;
+    if (post_int_rip) *post_int_rip = t->wait_post_int_rip;
+    return true;
+}
+
+/* Clear the current thread's pending continuation (called by the glue after it has
+ * written the result into the frame). */
+void je_sched_wait_finish(void)
+{
+    threads[current].wait_pending = false;
+    threads[current].wait_rec = (je_ep_channel *)0;
+}
+
+/* The current thread's slot index (= the opaque waiter_id used by the endpoint
+ * waiter + the release owner). */
+je_status je_sched_current_slot(uint32_t *out_slot)
+{
+    if (!ready || !out_slot) return JE_ERR_INVALID;
+    *out_slot = (uint32_t)current;
+    return JE_OK;
+}
+
 /* M3a #5: return path for a syscall that resumes the CURRENT user task (no
  * context switch) — e.g. abi_query, UNSUPPORTED, INVALID. ADR 0007 D6/M3A.md §4
  * require validating the user frame on EVERY return, so a non-switching syscall
diff --git a/kernel/core/syscall.c b/kernel/core/syscall.c
index f441d48..6a7f5d6 100644
--- a/kernel/core/syscall.c
+++ b/kernel/core/syscall.c
@@ -108,11 +108,13 @@ je_syscall_action je_syscall_decode(uint64_t op, const uint64_t args[6],
         if (rdx > (uint64_t)JE_ABI_MSG_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
         *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_SEND;
     case JE_SYS_TRY_RECV:
-        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8, hcap=R9, R10 reserved=0).
+        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8, hcap=R9, timeout=R10).
          * R8 = user u32[] to receive minted handles; R9 = its slot capacity
          * (0..JE_ABI_MSG_CAPS_MAX; a larger value is rejected so it can't be
-         * truncated to a smaller capacity in the glue). R8 must be 0 iff R9==0. */
-        if (r10) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
+         * truncated to a smaller capacity in the glue). R8 must be 0 iff R9==0.
+         * R10 = A6-ABI relative tick timeout: 0 = non-blocking poll (today's
+         * behavior), nonzero = block up to JE_IPC_MAX_WAIT (bound+overflow checked
+         * in the glue). Any R10 value is a legal arg now (no reserved-0 rule). */
         if (r9 > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
         if ((r8 == 0u) != (r9 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
         if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
@@ -210,18 +212,132 @@ static je_abi_status ep_status_to_abi(je_ep_status s)
     case JE_EP_SHORT_BUFFER: return JE_ABI_H_SHORT_BUFFER;
     case JE_EP_PEER_CLOSED: return JE_ABI_H_PEER_CLOSED;
     case JE_EP_EMPTY:       return JE_ABI_H_EMPTY;
+    case JE_EP_WAITERS_BUSY: return JE_ABI_H_EMPTY; /* A6-4: occupied slot -> non-blocking EMPTY */
     case JE_EP_INVALID:     return JE_ABI_INVALID;
     case JE_EP_CORRUPT:     return JE_ABI_UNSUPPORTED; /* unwired vm / invariant */
     }
     return JE_ABI_INVALID;
 }
 
+/* M3c A6: finish a resumed blocking recv. The thread parked on (wrec,wside) with
+ * out-args ubuf=rsi, cap=rdx, hbuf=r8, hcap=r9. Read the terminal cause the single
+ * arbitrate CAS stamped and deliver into f. WOKEN: peek the reserved head (bytes +
+ * caps) from the OWNED end (no handle re-resolve, A6-5), stage mints, copy out,
+ * commit, release the waiter. TIMED_OUT/PEER_CLOSED/CANCELLED: write the status.
+ * A copy/mint fault on WOKEN -> JE_ABI_INVALID, message stays queued, mints rolled
+ * back, reservation released (the head returns to deliverable). */
+static void je_syscall_finish_blocked_recv(je_trap_frame *f, je_ep_channel *rec,
+                                           uint8_t side, uint64_t ubuf, uint64_t cap,
+                                           uint64_t hbuf, uint64_t hcap)
+{
+    je_vm *vm; uint32_t index;
+    if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return; }
+    je_ep_wait_cause cause = je_endpoint_wait_cause(rec, side);
+    uint32_t slot = 0u; (void)je_sched_current_slot(&slot);
+    switch (cause) {
+    case JE_EP_WAIT_TIMED_OUT:
+        je_endpoint_kern_wait_release(rec, side, slot);
+        f->rax=(uint64_t)JE_ABI_H_TIMED_OUT; f->rdx=0; return;
+    case JE_EP_WAIT_PEER_CLOSED:
+        je_endpoint_kern_wait_release(rec, side, slot);
+        f->rax=(uint64_t)JE_ABI_H_PEER_CLOSED; f->rdx=0; return;
+    case JE_EP_WAIT_CANCELLED:
+        je_endpoint_kern_wait_release(rec, side, slot);
+        f->rax=(uint64_t)JE_ABI_H_CANCELLED; f->rdx=0; return;
+    case JE_EP_WAIT_WOKEN: break;    /* deliver below */
+    default:
+        /* No terminal cause on a resumed thread = kernel-invariant break. */
+        je_arch_console_write("JeiOS fatal: recv resume without terminal cause\r\n");
+        je_arch_halt(); return;
+    }
+    /* WOKEN delivery from the owned reserved head. */
+    uint8_t staging[JE_EP_MSG_MAX];
+    je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
+    size_t mlen = 0u; uint8_t mcaps = 0u;
+    je_ep_recv_lease lease;
+    je_ep_status s = je_endpoint_kern_recv_peek_owned(rec, side, staging, (size_t)cap,
+                                                      &mlen, (uint8_t)hcap, caps, &mcaps,
+                                                      &lease);
+    if (s == JE_EP_SHORT_BUFFER) {
+        /* Buffer too small: leave queued+reserved, report required sizes. The
+         * waiter stays so a re-issued recv with a bigger buffer delivers it. */
+        f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
+        f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32); return;
+    }
+    if (s != JE_EP_OK) { je_endpoint_kern_wait_release(rec, side, slot);
+                         f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return; }
+    /* Stage mints (dead cap -> handle 0; roll back all on transient fault). */
+    je_handle minted[JE_ABI_MSG_CAPS_MAX]; bool was_live[JE_ABI_MSG_CAPS_MAX];
+    for (uint8_t c = 0; c < mcaps; ++c) { minted[c]=0u; was_live[c]=false; }
+    je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
+    for (uint8_t c = 0; c < mcaps; ++c) {
+        if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c]=0u; continue; }
+        je_handle nh = 0u;
+        je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
+        if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
+        minted[c]=nh; was_live[c]=true; done=(uint8_t)(c+1u);
+    }
+    if (fail != JE_ABI_OK) {
+        for (uint8_t c = 0; c < done; ++c)
+            if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
+        /* Message stays queued+reserved; A6-5 says a fault does NOT consume. But a
+         * transient mint fault must not wedge the waiter forever: release it so a
+         * re-issue can retry (the head returns to deliverable). */
+        je_endpoint_kern_wait_release(rec, side, slot);
+        f->rax=(uint64_t)fail; f->rdx=0; return;
+    }
+    if (mlen != 0u) {
+        je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
+        if (cs != JE_ABI_OK) {
+            for (uint8_t c = 0; c < mcaps; ++c)
+                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
+            je_endpoint_kern_wait_release(rec, side, slot);
+            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
+        }
+    }
+    if (mcaps != 0u) {
+        uint32_t hout[JE_ABI_MSG_CAPS_MAX];
+        for (uint8_t c = 0; c < mcaps; ++c) hout[c]=(uint32_t)minted[c];
+        je_abi_status cs = je_copy_to_user(hbuf, hout, (uint64_t)mcaps*sizeof(uint32_t));
+        if (cs != JE_ABI_OK) {
+            for (uint8_t c = 0; c < mcaps; ++c)
+                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
+            je_endpoint_kern_wait_release(rec, side, slot);
+            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
+        }
+    }
+    /* Infallible commit (dequeue + release inflight), then free the waiter. */
+    je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
+    if (cm != JE_EP_OK) { je_arch_console_write("JeiOS fatal: blocked recv commit mismatch\r\n"); je_arch_halt(); }
+    je_endpoint_kern_wait_release(rec, side, slot);
+    f->rax=(uint64_t)JE_ABI_OK;
+    f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32);
+}
+
 /* Kernel glue. Returns the frame to iretq to (the caller for RETURN/invalid, a
  * live successor for YIELD/EXIT). Must only be called for a user-origin int 0x80
  * frame (je_trap_dispatch enforces cs&3==3; the scheduler helpers re-check the
  * frame identity). */
 je_trap_frame *je_syscall_dispatch(je_trap_frame *f)
 {
+    /* M3c A6-CLARIFY-1: pending-continuation prologue. If the CURRENT thread
+     * resumed from a blocking-recv park, FINISH that recv here — keyed on the
+     * waiter state, NOT on RAX — before any op decode. The thread re-executed its
+     * int 0x80, so RAX/args are intact, but we complete from the SAVED owned
+     * rec/side + args (never re-resolving the handle, A6-5). The terminal cause
+     * stamped by the single arbitrate CAS decides the result. */
+    {
+        je_ep_channel *wrec = (je_ep_channel *)0; uint8_t wside = 0u;
+        uint64_t wrdi, wrsi, wrdx, wr8, wr9, wprip;
+        if (je_sched_wait_pending(&wrec, &wside, &wrdi, &wrsi, &wrdx, &wr8, &wr9, &wprip)) {
+            /* The restart must have landed exactly on the saved post-int RIP; any
+             * mismatch is a kernel-invariant break, not a user error. */
+            if (f->rip != wprip) { je_arch_console_write("JeiOS fatal: recv continuation RIP\r\n"); je_arch_halt(); }
+            je_syscall_finish_blocked_recv(f, wrec, wside, wrsi, wrdx, wr8, wr9);
+            je_sched_wait_finish();
+            return je_sched_return_to_user(f);
+        }
+    }
     uint64_t args[6] = { f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9 };
     je_abi_status status; uint64_t result;
     je_syscall_action action = je_syscall_decode(f->rax, args, &status, &result);
@@ -408,6 +524,39 @@ je_trap_frame *je_syscall_dispatch(je_trap_frame *f)
         if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
         je_handle h = (je_handle)f->rdi;
         uint64_t ubuf = f->rsi, cap = f->rdx, hbuf = f->r8, hcap = f->r9;
+        uint64_t timeout = f->r10;   /* A6-ABI: relative tick duration; 0 = poll */
+        /* A6-ABI entry snapshot: evaluate the end once via the plain try_recv path
+         * BEFORE registering any waiter. If it is EMPTY (would block) and a nonzero
+         * timeout was given, register a per-end waiter and PARK (constrained
+         * restartable syscall); on resume the continuation prologue delivers. A
+         * zero timeout, or a non-EMPTY probe, falls through to the normal recv. */
+        if (timeout != 0u) {
+            je_ep_status probe = je_endpoint_kern_recv_entry_probe(vm, index, h);
+            if (probe == JE_EP_EMPTY) {
+                /* Bound + overflow-check the deadline (A6-3). */
+                uint64_t now = je_sched_ticks();
+                if (timeout > JE_IPC_MAX_WAIT || timeout > (UINT64_MAX - now)) {
+                    f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
+                }
+                uint32_t slot = 0u;
+                if (je_sched_current_slot(&slot) != JE_OK) {
+                    f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
+                }
+                je_ep_channel *wrec = (je_ep_channel *)0; uint8_t wside = 0u;
+                je_ep_status reg = je_endpoint_kern_wait_register(vm, index, h, slot, &wrec, &wside);
+                if (reg == JE_EP_WAITERS_BUSY) {   /* A6-4: slot occupied -> non-blocking EMPTY */
+                    f->rax=(uint64_t)JE_ABI_H_EMPTY; f->rdx=0; return je_sched_return_to_user(f);
+                }
+                if (reg != JE_EP_OK) {
+                    f->rax=(uint64_t)ep_status_to_abi(reg); f->rdx=0; return je_sched_return_to_user(f);
+                }
+                /* Park: saves args + owned rec/side, rewinds int 0x80, switches. */
+                return je_sched_block_current(f, wrec, wside, now + timeout);
+            }
+            /* probe != EMPTY: deliverable / SHORT_BUFFER / PEER_CLOSED / error ->
+             * fall through to the normal recv below (which re-derives the exact
+             * result and, for a deliverable message, does the full mint/copy). */
+        }
         uint8_t staging[JE_EP_MSG_MAX];
         je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
         size_t mlen = 0u; uint8_t mcaps = 0u;

 succeeded in 0ms:
   880	holds both ends) — that waits on transfer (step 5). `make check` / `smoke` /
   881	`integration` / 3 CI NX profiles (incl. 16M) / ARM64 all green.
   882	
   883	Design + implementation cross-review via a role-assigned peer debate. Key fixes
   884	adopted before PR: the send **preflight** ordering (copy-first would mis-rank
   885	INVALID over DENIED/QUEUE_FULL); the peek/commit seam with an **infallible**
   886	post-copy commit (a fallible commit would double-deliver); no inflight lease;
   887	zero exactly `len` bytes out (no stack leak). AI-assisted review; not independent
   888	human qualification.
   889	
   890	## M3c blocking-recv waits (D7 / A6) — 2026-10-19
   891	
   892	ADR 0009 **D-SEQ step 6** (owner-gated — edits `sched.c`; contract frozen by A6 +
   893	A6-CLARIFY-1/2). `try_recv` can now **block**: a nonzero R10 timeout parks the
   894	receiver until a message arrives (WOKEN), the peer closes (PEER_CLOSED), the
   895	deadline elapses (TIMED_OUT), or an internal cancel fires (CANCELLED, A6-2
   896	ABI-unreachable in v1). R10==0 is the unchanged non-blocking poll (returns EMPTY).
   897	
   898	**State machines (A6-STRUCT).** Two separate: the **waiter** (`WAITING →
   899	WOKEN|TIMED_OUT|PEER_CLOSED|CANCELLED`, on the endpoint end) and the **scheduler**
   900	(`THREAD_BLOCKED_IPC → READY`). One pure `je_endpoint_wait_arbitrate` CAS is the
   901	ONLY path out of WAITING; on single-CPU IF=0 the first critical section wins and
   902	later causes are no-ops (the written D7 priority applies only to the one entry
   903	snapshot). The kern bridge calls arbitrate on sender-enqueue (WOKEN, on the peer
   904	end), peer-close (PEER_CLOSED, drain-first: only on an empty end), the timer ISR
   905	(TIMED_OUT), and internal cancel; on WON it `je_sched_wake_ipc(id)` (guarded
   906	`BLOCKED_IPC→READY` only — never resurrects DYING/ZOMBIE).
   907	
   908	**Park/resume mechanism (A6-CLARIFY-1, constrained restartable syscall).** The
   909	recv handler, on EMPTY + nonzero timeout, registers a per-end waiter
   910	(`je_endpoint_wait_register`, A6-4 one-per-end → `WAITERS_BUSY` maps to a
   911	non-blocking EMPTY) and calls `je_sched_block_current`: a deliberate **pre-park
   912	`je_uframe_check`** (rejects a corrupt return frame at park, not at wake holding a
   913	reservation), save the recv args + the **owned rec/side** + the post-`int` RIP,
   914	**rewind `f->rip` by `JE_INT80_LEN=2`** (`CD 80`), set `BLOCKED_IPC`, switch. On
   915	wake the thread re-executes `int 0x80`; a **pending-continuation prologue** at the
   916	top of `je_syscall_dispatch` (keyed on `wait_pending`, asserts the restart RIP)
   917	finishes the recv **from the owned reference, never re-resolving the handle**
   918	(A6-5), copies out **after** the context switch (no copy in the scheduler). No
   919	trap-entry assembly change.
   920	
   921	**Reservation + release (A6-5 / A6-CLARIFY-2).** A WOKEN waiter reserves the
   922	inbound head (not dequeued, just not deliverable to a concurrent poll — `recv_peek`
   923	and `recv_peek_caps` mask a reserved head → EMPTY). Exactly one idempotent release
   924	owner, `je_endpoint_wait_release[_by_id]`: a successful commit (consume), a
   925	copy/mint-fault rollback (message stays queued, head returns to deliverable WITHOUT
   926	re-arming the terminal waiter), and thread teardown (`mark_dying` releases;
   927	`je_vm_destroy`/reaper inherit it). `mark_dying` calls it; the slot's waiter is
   928	released before the slot is reused (closes the death-before-re-entry window).
   929	
   930	**ABI (A6-ABI).** Reuses the `try_recv` op (12) with R10 = relative-tick timeout
   931	(bounded by `JE_IPC_MAX_WAIT=2^32`, overflow-checked). New statuses
   932	`JE_ABI_H_TIMED_OUT=14`, `JE_ABI_H_CANCELLED=15`. Caps compose: a WOKEN delivery
   933	runs the same step-5 staged-mint path from the owned reserved head
   934	(`recv_peek_owned_caps`). A short buffer is SHORT_BUFFER whether polling or
   935	blocking (detected at the entry/owned peek, never swept into INVALID).
   936	
   937	**Proofs.** **`M3c-s6`** kernel self-test proves the **TIMED_OUT** cause
   938	end-to-end through the real scheduler + ring 3 (a single thread parks on
   939	`recv(timeout)`, the timer ISR stamps the terminal transition, the continuation
   940	resumes with 14, waiter released, channel reclaimed — deterministic, one thread +
   941	the natural tick, no spin race). Hosted `test_endpoint` 27–28 prove the pure
   942	waiter layer (register, the single terminal CAS + first-wins, reserved-head
   943	masking, A6-4 WAITERS_BUSY, release idempotence, A6-CLARIFY-2 release-returns-head).
   944	`test_syscall` covers the R10-timeout decode. The **full two-process ring-3 WOKEN
   945	handshake is deferred to step 7** (bootstrap fixture) — honest, same as the step-5
   946	ring-3 deferral; the wake bridges are wired and compiled, step 7 adds the ring-3
   947	scheduling of two cooperating user processes. `make check`/`smoke`
   948	(s3a/s3b/s4/s5/s6)/`integration`/3 CI NX (incl 16M)/ARM64 all green.
   949	
   950	Design + both review rounds (contract text, then the park/resume mechanism) in
   951	`docs/adr/debates/m3c-step6-waits.md` + `m3c-step6-mechanism.md` (ADR 0000 part 4).
   952	Owner-signed A6 + CLARIFY-1/2. AI-assisted implementation review; not independent
   953	human qualification.
   954	
   955	## M3c capability transfer (try_send/try_recv with caps) — 2026-10-19

codex
The frozen ADR confirms the two semantic mismatches are real: a reserved head must report `EMPTY` even when the peer is closed, and post-WOKEN `SHORT_BUFFER` is supposed to be an entry-time result with no reservation. I’m checking the exact build/test outcomes and a few remaining edge paths (close-after-free, timer boundary, initializers, and continuation cleanup) before finalizing the numbered list.
exec
/usr/bin/bash -lc 'make smoke integration' in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 22770ms:
grub-file --is-x86-multiboot2 build/0/jeios.elf
cp build/0/jeios.elf build/0/iso/boot/
cp boot/grub/grub.cfg build/0/iso/boot/grub/
grub-mkrescue -d /usr/lib/grub/i386-pc -o build/0/jeios.iso build/0/iso
xorriso 1.5.6 : RockRidge filesystem manipulator, libburnia project.

Drive current: -outdev 'stdio:build/0/jeios.iso'
Media current: stdio file, overwriteable
Media status : is blank
Media summary: 0 sessions, 0 data blocks, 0 data,  168g free
Added to ISO image: directory '/'='/tmp/grub.bziBG3'
xorriso : UPDATE :     299 files added in 1 seconds
Added to ISO image: directory '/'='/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/build/0/iso'
xorriso : UPDATE :     303 files added in 1 seconds
xorriso : NOTE : Copying to System Area: 512 bytes from file '/usr/lib/grub/i386-pc/boot_hybrid.img'
ISO image produced: 2746 sectors
Written to medium : 2746 sectors at LBA 0
Writing to 'stdio:build/0/jeios.iso' completed successfully.

python3 tools/smoke.py build/0/jeios.iso
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
M2: allocator ready; free pages=0x0000000000007c22
M2: seven guarded threads ready; enabling PIT
M2: preemption/GPR/sleep/heap checks passed; ticks=0x000000000000004c
M2: exited threads reaped; page accounting restored
M3a-s2: process built; audit ok; root distinct
M3a-s2: arena mapped under proc root; data RW; kernel root restored
M3a-s2: process destroyed; page accounting restored
M3a-s3: D7 FPU/SIMD denial enforced (CR0.EM/MP/TS, CR4 clear)
M3a-s3: user thread created; entering ring 3 via timer
M3a-s3: CPL3 reached; survived preemption; heartbeat advanced
M3a-s4: two CPL3 tasks A(fault)+B(loop) created; distinct roots
M3a-s4: A terminated by user #PF (vector 14, cs=0x33); B still scheduled
M3a-s4: B advanced AFTER A's death (heartbeat+dispatch+preempt up)
M3a-s4: dead process A reaped while peer B runs
M3a-s4: bad-RSP (H1) user task terminated (BAD_RETURN), not kernel; B alive
M3a-s4: hostile-FP user task terminated (#UD FAULT); B alive
M3a-s5a: user exit(42) -> clean EXIT (code 42) via int 0x80; B alive
M3a-s5a: abi_query returned OK+version; yield returned OK; B alive
M3a-s5a: unknown op -> UNSUPPORTED, task survived; B alive
M3a-s5a: bad frame on syscall return -> BAD_RETURN death (validated); B alive
M3a-s5b: copy_probe valid+zero OK, null/kernel/len>256 INVALID, bytes moved; B alive
M3b-s1: handle self/query/rights/dup/close OK; attenuation drops INSPECT -> handle_rights DENIED (enforcement real); superset DENIED; unknown-right INVALID; reuse re-stales; QUOTA at CAP-1; B alive
M3c-s3a: survivor-peer PEER_CLOSED; record held by survivor; reclaimed on its close (kernel self-test)
M3c-s3b: ring-3 endpoint_create + typed close; fault-death reap reclaims the channel; pool baseline held across >POOL_CAP cycles
M3c-s4: ring-3 try_send/try_recv loopback OK (direction, SHORT_BUFFER, copy-out failure leaves msg queued -> delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED); channel reclaimed on exit
M3a-s4: peer B torn down; page accounting restored
M3c-s5: cross-table capability transfer OK (XA mints C, transfers C.e1 to XB via the kernel send/recv cap FORWARDERS over seeded S; XB mints it in its OWN table and messages XA on c0 across tables); S+C reclaimed. [syscall-decode/copy layer exercised by test_syscall decode + hosted tests; ring-3 two-process transfer deferred to step 7]
M3c-s6: ring-3 thread blocks on recv(timeout); waiting for timer-driven TIMED_OUT...
M3c-s6: blocking recv TIMED_OUT end-to-end (ring-3 thread parked, timer ISR stamped the terminal transition, continuation resumed with status 14, waiter released, channel reclaimed). [WOKEN/PEER_CLOSED/CANCELLED proven in the pure hosted tests + arbitrate CAS; the full two-process ring-3 WOKEN handshake rides on step 7's bootstrap fixture]
JeiOS: initialization complete; halted
qemu-system-x86_64: terminating on signal 15 from pid 1130113 (python3)

QEMU smoke: PASS
python3 tools/integration.py
fault mode 1: PASS
JeiOS: entered C kernel
M1: traps ready
PANIC vector=0x0000000000000006 error=0x0000000000000000 rip=0x0000000000109958 cs=0x0000000000000008 cr2=0x0000000000000000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 2: PASS
JeiOS: entered C kernel
M1: traps ready
PANIC vector=0x000000000000000d error=0x0000000000000038 rip=0x000000000010995d cs=0x0000000000000008 cr2=0x0000000000000000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 3: PASS
JeiOS: entered C kernel
M1: traps ready
PANIC vector=0x0000000000000008 error=0x0000000000000000 rip=0x000000000010bc50 cs=0x0000000000000008 cr2=0x0000000000000000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 4: PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
PANIC vector=0x000000000000000e error=0x0000000000000003 rip=0x0000000000109a10 cs=0x0000000000000008 cr2=0x0000000000100000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 5: PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
PANIC vector=0x000000000000000e error=0x0000000000000000 rip=0x0000000000109a10 cs=0x0000000000000008 cr2=0x00000000003b5000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 6: PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
PANIC vector=0x000000000000000e error=0x0000000000000011 rip=0x000000000019e8c0 cs=0x0000000000000008 cr2=0x000000000019e8c0
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 7: PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
PANIC vector=0x0000000000000008 error=0x0000000000000000 rip=0x000000000010b6ef cs=0x0000000000000008 cr2=0x0000000000000000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 8: PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
M2: allocator ready; free pages=0x0000000000007c22
M2: seven guarded threads ready; enabling PIT
M2: preemption/GPR/sleep/heap checks passed; ticks=0x000000000000004c
M2: exited threads reaped; page accounting restored
M3a-s2: process built; audit ok; root distinct
PANIC vector=0x000000000000000e error=0x0000000000000000 rip=0x0000000000109e46 cs=0x0000000000000008 cr2=0x0000000040000000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 9: PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
M2: allocator ready; free pages=0x0000000000007c22
M2: seven guarded threads ready; enabling PIT
M2: preemption/GPR/sleep/heap checks passed; ticks=0x000000000000004c
M2: exited threads reaped; page accounting restored
M3a-s2: process built; audit ok; root distinct
PANIC vector=0x000000000000000e error=0x0000000000000003 rip=0x0000000000109f50 cs=0x0000000000000008 cr2=0x0000000040000000
JeiOS fatal: exception; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 10 (fault-local D7 #UD): PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
M2: allocator ready; free pages=0x0000000000007c26
M2: seven guarded threads ready; enabling PIT
M2: preemption/GPR/sleep/heap checks passed; ticks=0x000000000000004c
M2: exited threads reaped; page accounting restored
M3a-s2: process built; audit ok; root distinct
M3a-s2: arena mapped under proc root; data RW; kernel root restored
M3a-s2: process destroyed; page accounting restored
M3a-s3: D7 FPU/SIMD denial enforced (CR0.EM/MP/TS, CR4 clear)
M3a-s3: hostile-FP user thread created; entering ring 3
M3a-s3: hostile-FP task #UD terminated (fault-local); kernel alive
JeiOS: initialization complete; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)

fault mode 11 (exception-fixup recovery): PASS
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
M2: allocator ready; free pages=0x0000000000007c22
M2: seven guarded threads ready; enabling PIT
M2: preemption/GPR/sleep/heap checks passed; ticks=0x000000000000004c
M2: exited threads reaped; page accounting restored
M3a-s2: process built; audit ok; root distinct
M3a-s2: arena mapped under proc root; data RW; kernel root restored
M3a-s2: process destroyed; page accounting restored
M3a-s3: D7 FPU/SIMD denial enforced (CR0.EM/MP/TS, CR4 clear)
M3a-s3: user thread created; entering ring 3 via timer
M3a-s3: CPL3 reached; survived preemption; heartbeat advanced
M3a-s4: two CPL3 tasks A(fault)+B(loop) created; distinct roots
M3a-s4: A terminated by user #PF (vector 14, cs=0x33); B still scheduled
M3a-s4: B advanced AFTER A's death (heartbeat+dispatch+preempt up)
M3a-s4: dead process A reaped while peer B runs
M3a-s4: bad-RSP (H1) user task terminated (BAD_RETURN), not kernel; B alive
M3a-s4: hostile-FP user task terminated (#UD FAULT); B alive
M3a-s5a: user exit(42) -> clean EXIT (code 42) via int 0x80; B alive
M3a-s5a: abi_query returned OK+version; yield returned OK; B alive
M3a-s5a: unknown op -> UNSUPPORTED, task survived; B alive
M3a-s5a: bad frame on syscall return -> BAD_RETURN death (validated); B alive
M3a-s5b: copy_probe valid+zero OK, null/kernel/len>256 INVALID, bytes moved; B alive
M3a-s5b: exception-fixup recovered load+store copy-site #PF (dev inject); kernel alive
M3b-s1: handle self/query/rights/dup/close OK; attenuation drops INSPECT -> handle_rights DENIED (enforcement real); superset DENIED; unknown-right INVALID; reuse re-stales; QUOTA at CAP-1; B alive
M3c-s3a: survivor-peer PEER_CLOSED; record held by survivor; reclaimed on its close (kernel self-test)
M3c-s3b: ring-3 endpoint_create + typed close; fault-death reap reclaims the channel; pool baseline held across >POOL_CAP cycles
M3c-s4: ring-3 try_send/try_recv loopback OK (direction, SHORT_BUFFER, copy-out failure leaves msg queued -> delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED); channel reclaimed on exit
M3a-s4: peer B torn down; page accounting restored
M3c-s5: cross-table capability transfer OK (XA mints C, transfers C.e1 to XB via the kernel send/recv cap FORWARDERS over seeded S; XB mints it in its OWN table and messages XA on c0 across tables); S+C reclaimed. [syscall-decode/copy layer exercised by test_syscall decode + hosted tests; ring-3 two-process transfer deferred to step 7]
M3c-s6: ring-3 thread blocks on recv(timeout); waiting for timer-driven TIMED_OUT...
M3c-s6: blocking recv TIMED_OUT end-to-end (ring-3 thread parked, timer ISR stamped the terminal transition, continuation resumed with status 14, waiter released, channel reclaimed). [WOKEN/PEER_CLOSED/CANCELLED proven in the pure hosted tests + arbitrate CAS; the full two-process ring-3 WOKEN handshake rides on step 7's bootstrap fixture]
JeiOS: initialization complete; halted
qemu-system-x86_64: terminating on signal 15 from pid 1131661 (python3)


exec
/usr/bin/bash -lc "grep -RIn \"je_trap_dispatch\\|int80\\|0x80\\|rax\\|push.*rax\" kernel/arch/x86_64 kernel/core | head -220" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba kernel/arch/x86_64/trap.S | sed -n '1,280p' 2>/dev/null || true; nl -ba kernel/core/trap.c | sed -n '1,260p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -RIn \"\\.state\\|state==\\|state "'!='"\\|case .*SLEEPING\\|case .*READY\\|enum.*EMPTY\" kernel/core include | head -260" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
kernel/core/endpoint.c:65:            r->end[s].state = JE_EP_END_FREE;
kernel/core/endpoint.c:82:    return rec->end[side].state;
kernel/core/endpoint.c:131:        rec->end[s].state = JE_EP_END_FREE;
kernel/core/endpoint.c:395:        rec->end[s].state = JE_EP_END_FREE;
kernel/core/endpoint.c:428:        rec->end[s].state = JE_EP_END_OPEN;
kernel/core/endpoint.c:576:    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
kernel/core/endpoint.c:577:        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
kernel/core/endpoint.c:600:    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
kernel/core/endpoint.c:601:        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
kernel/core/endpoint.c:635:        return (rec->end[side].state == JE_EP_END_PEER_CLOSED && q->count == 0u)
kernel/core/endpoint.c:670:    je_ep_end_state st = tr->end[d->side].state;
kernel/core/endpoint.c:799:        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
kernel/core/endpoint.c:837:        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
kernel/core/vm.c:276:        if (vm->procs[i].state == JE_VM_FREE) { idx = i; break; }
kernel/core/vm.c:387:    if (vm->procs[index].state != JE_VM_READY) return JE_ERR_INVALID;
kernel/core/vm.c:411:    if (p->state != JE_VM_READY) return JE_ERR_INVALID;
kernel/core/vm.c:491:                if (q == index || vm->procs[q].state != JE_VM_READY) continue;
kernel/core/handle.c:68:        slots[i].state = JE_HSLOT_FREE;
kernel/core/handle.c:88:        if (t->slots[i].state == JE_HSLOT_FREE) {
kernel/core/handle.c:89:            t->slots[i].state = JE_HSLOT_LIVE;
kernel/core/handle.c:116:    if (s->state != JE_HSLOT_LIVE) return JE_HANDLE_STALE;
kernel/core/handle.c:183:    if (s->state != JE_HSLOT_LIVE || s->generation != generation)
kernel/core/handle.c:205:    if (s->state != JE_HSLOT_LIVE) return false;
kernel/core/sched.c:16:typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING, BLOCKED_IPC } state;
kernel/core/sched.c:73:    threads[0].state=RUNNING;current=0;ready=true;
kernel/core/sched.c:79:    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
kernel/core/sched.c:112:    if (p->state != JE_VM_READY || p->held_kstack != JE_VM_KSTACK_PAGES || !p->root)
kernel/core/sched.c:115:    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
kernel/core/sched.c:148:    threads[slot].state=DYING;
kernel/core/sched.c:168:            if (threads[candidate].state==READY) { next=candidate;break; }
kernel/core/sched.c:170:        if (threads[next].state!=READY || !threads[next].frame) abort_scheduler();
kernel/core/sched.c:186:        current=next;threads[current].state=RUNNING;++threads[current].dispatches;
kernel/core/sched.c:194:    if (!ready || threads[current].state==EMPTY) abort_scheduler();
kernel/core/sched.c:212:        if (threads[i].state==SLEEPING && threads[i].wake<=je_timer_ticks) threads[i].state=READY;
kernel/core/sched.c:220:            if (threads[i].state==BLOCKED_IPC && threads[i].wait_deadline!=0u &&
kernel/core/sched.c:230:                threads[i].state=READY;
kernel/core/sched.c:232:    if (threads[current].state==RUNNING) threads[current].state=READY;
kernel/core/sched.c:318:    if (threads[slot].state == BLOCKED_IPC) threads[slot].state = READY;
kernel/core/sched.c:388:        threads[current].state=SLEEPING;
kernel/core/sched.c:397:    threads[current].state=ZOMBIE;
kernel/core/sched.c:404:        if (threads[i].state!=EMPTY && threads[i].state!=ZOMBIE) return false;
kernel/core/sched.c:410:    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==ZOMBIE) {
kernel/core/sched.c:465:    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) {
kernel/core/sched.c:484:    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) return true;
kernel/core/sched.c:492:            threads[i].state==DYING) {
kernel/core/sched.c:507:            threads[i].state==DYING) {
kernel/core/sched.c:534:        if (threads[i].state!=EMPTY && threads[i].dispatches<min) min=threads[i].dispatches;

 succeeded in 0ms:
kernel/arch/x86_64/trap_entry.S:23:    pushq %rax
kernel/arch/x86_64/trap_entry.S:40:    call je_trap_dispatch
kernel/arch/x86_64/trap_entry.S:41:    movq %rax,%rsp
kernel/arch/x86_64/trap_entry.S:56:    popq %rax
kernel/arch/x86_64/user_demo.S:49:    movq $8, %rax              /* small spin count so it is seen running first */
kernel/arch/x86_64/user_demo.S:52:    decq %rax
kernel/arch/x86_64/user_demo.S:54:    movq 0x40003000, %rax      /* read unmapped arena page -> #PF (vector 14) */
kernel/arch/x86_64/user_demo.S:58:/* M3a #5 step (a): a user program that calls exit(42) via the int 0x80 gate —
kernel/arch/x86_64/user_demo.S:65:    movq $2, %rax              /* JE_SYS_EXIT */
kernel/arch/x86_64/user_demo.S:72:    int $0x80                  /* -> je_sched_exit_current; never returns */
kernel/arch/x86_64/user_demo.S:85:    xorq %rax, %rax
kernel/arch/x86_64/user_demo.S:92:    int $0x80                  /* -> RAX=status, RDX=version */
kernel/arch/x86_64/user_demo.S:93:    movq %rax, 0x40001008      /* record status at heartbeat+8 */
kernel/arch/x86_64/user_demo.S:97:    movq $1, %rax              /* JE_SYS_YIELD */
kernel/arch/x86_64/user_demo.S:104:    int $0x80                  /* yield; resumes here with RAX=OK */
kernel/arch/x86_64/user_demo.S:105:    movq %rax, 0x40001018      /* record yield status at heartbeat+24 */
kernel/arch/x86_64/user_demo.S:115:    movq $0xdeadbeef, %rax     /* unknown op */
kernel/arch/x86_64/user_demo.S:122:    int $0x80                  /* -> RAX=UNSUPPORTED, returns */
kernel/arch/x86_64/user_demo.S:123:    movq %rax, 0x40001008      /* record status */
kernel/arch/x86_64/user_demo.S:139:    xorq %rax, %rax            /* abi_query (a RETURNING syscall) */
kernel/arch/x86_64/user_demo.S:146:    int $0x80                  /* return path must validate -> BAD_RETURN death */
kernel/arch/x86_64/user_demo.S:167:    movq $3, %rax
kernel/arch/x86_64/user_demo.S:174:    int $0x80
kernel/arch/x86_64/user_demo.S:175:    movq %rax, 0x40001020
kernel/arch/x86_64/user_demo.S:177:    movq $3, %rax
kernel/arch/x86_64/user_demo.S:184:    int $0x80
kernel/arch/x86_64/user_demo.S:185:    movq %rax, 0x40001028
kernel/arch/x86_64/user_demo.S:187:    movq $3, %rax
kernel/arch/x86_64/user_demo.S:194:    int $0x80
kernel/arch/x86_64/user_demo.S:195:    movq %rax, 0x40001030
kernel/arch/x86_64/user_demo.S:197:    movq $3, %rax
kernel/arch/x86_64/user_demo.S:204:    int $0x80
kernel/arch/x86_64/user_demo.S:205:    movq %rax, 0x40001038
kernel/arch/x86_64/user_demo.S:207:    movq $3, %rax
kernel/arch/x86_64/user_demo.S:214:    int $0x80
kernel/arch/x86_64/user_demo.S:215:    movq %rax, 0x40001040
kernel/arch/x86_64/user_demo.S:229:    movq $4, %rax               /* JE_SYS_COPY_FIXUP_INJECT */
kernel/arch/x86_64/user_demo.S:236:    int $0x80
kernel/arch/x86_64/user_demo.S:270: *   +0x80 quota status (after filling the quota)
kernel/arch/x86_64/user_demo.S:276:    movq $5, %rax
kernel/arch/x86_64/user_demo.S:279:    int $0x80
kernel/arch/x86_64/user_demo.S:280:    movq %rax, 0x40001020
kernel/arch/x86_64/user_demo.S:284:    movq $7, %rax
kernel/arch/x86_64/user_demo.S:287:    int $0x80
kernel/arch/x86_64/user_demo.S:288:    movq %rax, 0x40001030
kernel/arch/x86_64/user_demo.S:292:    movq $8, %rax
kernel/arch/x86_64/user_demo.S:295:    int $0x80
kernel/arch/x86_64/user_demo.S:296:    movq %rax, 0x400010a0
kernel/arch/x86_64/user_demo.S:301:    movq $9, %rax
kernel/arch/x86_64/user_demo.S:305:    int $0x80
kernel/arch/x86_64/user_demo.S:306:    movq %rax, 0x400010b0
kernel/arch/x86_64/user_demo.S:309:    movq $9, %rax
kernel/arch/x86_64/user_demo.S:313:    int $0x80
kernel/arch/x86_64/user_demo.S:314:    movq %rax, 0x400010c0
kernel/arch/x86_64/user_demo.S:319:    movq $8, %rax
kernel/arch/x86_64/user_demo.S:322:    int $0x80
kernel/arch/x86_64/user_demo.S:323:    movq %rax, 0x400010d0
kernel/arch/x86_64/user_demo.S:325:    movq $9, %rax
kernel/arch/x86_64/user_demo.S:329:    int $0x80
kernel/arch/x86_64/user_demo.S:330:    movq %rax, 0x400010d8
kernel/arch/x86_64/user_demo.S:331:    /* dup(self#1, 0x80000000): still-UNKNOWN rights bit -> INVALID (2)@+0xe0.
kernel/arch/x86_64/user_demo.S:333:     * contract is now proven on bit 31, which remains unknown. 0x80000000 fits
kernel/arch/x86_64/user_demo.S:335:    movq $9, %rax
kernel/arch/x86_64/user_demo.S:337:    movq $0x80000000, %rsi
kernel/arch/x86_64/user_demo.S:339:    int $0x80
kernel/arch/x86_64/user_demo.S:340:    movq %rax, 0x400010e0
kernel/arch/x86_64/user_demo.S:342:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:345:    int $0x80
kernel/arch/x86_64/user_demo.S:346:    movq %rax, 0x400010e8
kernel/arch/x86_64/user_demo.S:349:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:352:    int $0x80
kernel/arch/x86_64/user_demo.S:353:    movq %rax, 0x400010f0
kernel/arch/x86_64/user_demo.S:355:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:358:    int $0x80
kernel/arch/x86_64/user_demo.S:359:    movq %rax, 0x40001040
kernel/arch/x86_64/user_demo.S:361:    movq $7, %rax
kernel/arch/x86_64/user_demo.S:364:    int $0x80
kernel/arch/x86_64/user_demo.S:365:    movq %rax, 0x40001048
kernel/arch/x86_64/user_demo.S:367:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:370:    int $0x80
kernel/arch/x86_64/user_demo.S:371:    movq %rax, 0x40001050
kernel/arch/x86_64/user_demo.S:373:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:376:    int $0x80
kernel/arch/x86_64/user_demo.S:377:    movq %rax, 0x40001058
kernel/arch/x86_64/user_demo.S:380:    movq $7, %rax
kernel/arch/x86_64/user_demo.S:383:    int $0x80
kernel/arch/x86_64/user_demo.S:384:    movq %rax, 0x40001060
kernel/arch/x86_64/user_demo.S:386:    movq $5, %rax
kernel/arch/x86_64/user_demo.S:389:    int $0x80
kernel/arch/x86_64/user_demo.S:390:    movq %rax, 0x40001088      /* self#2 status (expect OK) */
kernel/arch/x86_64/user_demo.S:396:    movq $7, %rax
kernel/arch/x86_64/user_demo.S:399:    int $0x80
kernel/arch/x86_64/user_demo.S:400:    movq %rax, 0x40001090
kernel/arch/x86_64/user_demo.S:404:    movq $7, %rax
kernel/arch/x86_64/user_demo.S:407:    int $0x80
kernel/arch/x86_64/user_demo.S:408:    movq %rax, 0x40001068
kernel/arch/x86_64/user_demo.S:412:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:417:    int $0x80
kernel/arch/x86_64/user_demo.S:418:    movq %rax, 0x40001070
kernel/arch/x86_64/user_demo.S:426:    movq $5, %rax
kernel/arch/x86_64/user_demo.S:429:    int $0x80
kernel/arch/x86_64/user_demo.S:430:    movq %rax, 0x40001080      /* last self status */
kernel/arch/x86_64/user_demo.S:431:    cmpq $7, %rax              /* JE_ABI_H_QUOTA == 7 */
kernel/arch/x86_64/user_demo.S:433:    cmpq $0, %rax              /* JE_ABI_OK == 0 */
kernel/arch/x86_64/user_demo.S:455:    movq $4, %rax              /* brief heartbeat so we're seen running */
kernel/arch/x86_64/user_demo.S:458:    decq %rax
kernel/arch/x86_64/user_demo.S:461:    movq $10, %rax
kernel/arch/x86_64/user_demo.S:464:    int $0x80
kernel/arch/x86_64/user_demo.S:465:    movq %rax, 0x40001200      /* create status -> +0x200 */
kernel/arch/x86_64/user_demo.S:472:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:474:    int $0x80
kernel/arch/x86_64/user_demo.S:475:    movq %rax, 0x40001210      /* close(h0) status -> +0x210 */
kernel/arch/x86_64/user_demo.S:478:    movq 0x40003000, %rax      /* unmapped arena page -> #PF */
kernel/arch/x86_64/user_demo.S:492:    movq $4, %rax
kernel/arch/x86_64/user_demo.S:495:    decq %rax
kernel/arch/x86_64/user_demo.S:501:    movq $10, %rax
kernel/arch/x86_64/user_demo.S:503:    int $0x80
kernel/arch/x86_64/user_demo.S:504:    movq %rax, 0x40001380      /* create status */
kernel/arch/x86_64/user_demo.S:510:    movq $11, %rax
kernel/arch/x86_64/user_demo.S:515:    int $0x80
kernel/arch/x86_64/user_demo.S:516:    movq %rax, 0x40001388      /* send status */
kernel/arch/x86_64/user_demo.S:518:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:523:    int $0x80
kernel/arch/x86_64/user_demo.S:524:    movq %rax, 0x40001390      /* expect JE_ABI_H_EMPTY (13) */
kernel/arch/x86_64/user_demo.S:526:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:531:    int $0x80
kernel/arch/x86_64/user_demo.S:532:    movq %rax, 0x40001398      /* expect SHORT_BUFFER (11) */
kernel/arch/x86_64/user_demo.S:535:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:540:    int $0x80
kernel/arch/x86_64/user_demo.S:541:    movq %rax, 0x400013a8      /* expect INVALID (2) */
kernel/arch/x86_64/user_demo.S:543:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:548:    int $0x80
kernel/arch/x86_64/user_demo.S:549:    movq %rax, 0x400013b0      /* expect OK (0) */
kernel/arch/x86_64/user_demo.S:554:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:559:    int $0x80
kernel/arch/x86_64/user_demo.S:560:    movq %rax, 0x400013c8      /* expect EMPTY (13) */
kernel/arch/x86_64/user_demo.S:562:    movq $11, %rax
kernel/arch/x86_64/user_demo.S:567:    int $0x80
kernel/arch/x86_64/user_demo.S:568:    movq %rax, 0x400013d0      /* expect INVALID (2) */
kernel/arch/x86_64/user_demo.S:570:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:575:    int $0x80
kernel/arch/x86_64/user_demo.S:576:    movq %rax, 0x400013d8      /* expect EMPTY (13) */
kernel/arch/x86_64/user_demo.S:580:    movq $11, %rax
kernel/arch/x86_64/user_demo.S:585:    int $0x80
kernel/arch/x86_64/user_demo.S:588:    movq %rax, 0x400013e0      /* 4th send status: expect OK */
kernel/arch/x86_64/user_demo.S:589:    movq $11, %rax             /* 5th send -> QUEUE_FULL */
kernel/arch/x86_64/user_demo.S:594:    int $0x80
kernel/arch/x86_64/user_demo.S:595:    movq %rax, 0x400013e8      /* expect QUEUE_FULL (10) */
kernel/arch/x86_64/user_demo.S:597:    movq $11, %rax
kernel/arch/x86_64/user_demo.S:603:    int $0x80
kernel/arch/x86_64/user_demo.S:604:    movq %rax, 0x400013f0      /* expect INVALID (2) */
kernel/arch/x86_64/user_demo.S:606:    movq $6, %rax
kernel/arch/x86_64/user_demo.S:609:    int $0x80
kernel/arch/x86_64/user_demo.S:610:    movq %rax, 0x400013f8      /* close(h0) status: expect OK */
kernel/arch/x86_64/user_demo.S:614:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:619:    int $0x80
kernel/arch/x86_64/user_demo.S:622:    movq %rax, 0x40001400      /* 4th drained recv: expect OK (drain despite peer gone) */
kernel/arch/x86_64/user_demo.S:624:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:629:    int $0x80
kernel/arch/x86_64/user_demo.S:630:    movq %rax, 0x40001408      /* expect PEER_CLOSED (12) */
kernel/arch/x86_64/user_demo.S:632:    movq $11, %rax
kernel/arch/x86_64/user_demo.S:637:    int $0x80
kernel/arch/x86_64/user_demo.S:638:    movq %rax, 0x40001410      /* expect PEER_CLOSED (12) */
kernel/arch/x86_64/user_demo.S:656:    movq $10, %rax
kernel/arch/x86_64/user_demo.S:658:    int $0x80
kernel/arch/x86_64/user_demo.S:659:    movq %rax, 0x40001380       /* create status (expect 0) */
kernel/arch/x86_64/user_demo.S:665:    movq $12, %rax
kernel/arch/x86_64/user_demo.S:671:    int $0x80
kernel/arch/x86_64/user_demo.S:672:    movq %rax, 0x40001388      /* recv status after wake (expect TIMED_OUT = 14) */
kernel/arch/x86_64/register_probe.S:12:    movq $16,%rax
kernel/arch/x86_64/register_probe.S:28:    cmpq $16,%rax
kernel/arch/x86_64/trap.c:59:     * 0x80, IST=0, selector 8 — the only user-reachable gate. attributes 0xee =
kernel/arch/x86_64/trap.c:61:     * There is no syscall handler yet, so a user int 0x80 is an unsupported op
kernel/arch/x86_64/trap.c:64:    idt[0x80].attributes = 0xee;
kernel/arch/x86_64/trap.c:122:je_trap_frame *je_trap_dispatch(je_trap_frame *f)
kernel/arch/x86_64/trap.c:136:    /* M3a #5: a user-origin int 0x80 is a SYSCALL (dispatch on RAX), not a fault.
kernel/arch/x86_64/trap.c:137:     * Route it before the fault-termination path. A kernel-origin int 0x80 is
kernel/arch/x86_64/trap.c:139:    if ((f->cs & 3) == 3 && f->vector == 0x80)
kernel/arch/x86_64/trap.c:161:    __asm__ volatile("movq (%%rax),%%rax" : : "a"(UINT64_C(0x40000000)) : "memory");
kernel/arch/x86_64/platform.c:12:    out8(0x3f9, 0); out8(0x3fb, 0x80);
kernel/arch/x86_64/platform.c:34:                      : "a"(0x80000000u), "c"(0));
kernel/arch/x86_64/platform.c:35:    if (a < 0x80000001u) return 0;
kernel/arch/x86_64/platform.c:37:                      : "a"(0x80000001u), "c"(0));
kernel/arch/x86_64/platform.c:65:static void io_wait(void) { out8(0x80,0); }
kernel/arch/x86_64/boot.S:29:    movl $0x80000000, %eax
kernel/arch/x86_64/boot.S:31:    cmpl $0x80000001, %eax
kernel/arch/x86_64/boot.S:33:    movl $0x80000001, %eax
kernel/core/syscall.c:4: * int 0x80 gate (ADR 0007 D8/D10). Interrupts are disabled on this short path
kernel/core/syscall.c:234:    if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return; }
kernel/core/syscall.c:240:        f->rax=(uint64_t)JE_ABI_H_TIMED_OUT; f->rdx=0; return;
kernel/core/syscall.c:243:        f->rax=(uint64_t)JE_ABI_H_PEER_CLOSED; f->rdx=0; return;
kernel/core/syscall.c:246:        f->rax=(uint64_t)JE_ABI_H_CANCELLED; f->rdx=0; return;
kernel/core/syscall.c:264:        f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
kernel/core/syscall.c:268:                         f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return; }
kernel/core/syscall.c:287:        f->rax=(uint64_t)fail; f->rdx=0; return;
kernel/core/syscall.c:295:            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
kernel/core/syscall.c:306:            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
kernel/core/syscall.c:313:    f->rax=(uint64_t)JE_ABI_OK;
kernel/core/syscall.c:318: * live successor for YIELD/EXIT). Must only be called for a user-origin int 0x80
kernel/core/syscall.c:319: * frame (je_trap_dispatch enforces cs&3==3; the scheduler helpers re-check the
kernel/core/syscall.c:326:     * int 0x80, so RAX/args are intact, but we complete from the SAVED owned
kernel/core/syscall.c:343:    je_syscall_action action = je_syscall_decode(f->rax, args, &status, &result);
kernel/core/syscall.c:351:        f->rax = (uint64_t)status; f->rdx = result;
kernel/core/syscall.c:359:        f->rax = (uint64_t)s; f->rdx = 0;
kernel/core/syscall.c:369:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:373:        f->rax = (uint64_t)handle_status_to_abi(s);
kernel/core/syscall.c:381:         * and the close are atomic because this int 0x80 path runs at IF=0 on one
kernel/core/syscall.c:385:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:396:            f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
kernel/core/syscall.c:402:            f->rax = (uint64_t)JE_ABI_H_WRONGTYPE; f->rdx = 0;
kernel/core/syscall.c:407:            f->rax = (uint64_t)handle_status_to_abi(s); f->rdx = 0;
kernel/core/syscall.c:417:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:421:        f->rax = (uint64_t)handle_status_to_abi(s);
kernel/core/syscall.c:432:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:437:        f->rax = (uint64_t)handle_status_to_abi(s);
kernel/core/syscall.c:450:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:455:        f->rax = (uint64_t)handle_status_to_abi(s);
kernel/core/syscall.c:467:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:470:        f->rax = (uint64_t)ep_status_to_abi(s);
kernel/core/syscall.c:482:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }

 succeeded in 0ms:
nl: kernel/arch/x86_64/trap.S: No such file or directory
nl: kernel/core/trap.c: No such file or directory

exec
/usr/bin/bash -lc "nl -ba kernel/arch/x86_64/trap_entry.S | sed -n '1,110p'; nl -ba kernel/arch/x86_64/trap.c | sed -n '110,155p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/* Intel IA-32e frames are normalized to [vector,error,RIP,CS,RFLAGS,RSP,SS].
     3	 * A CPU error code is already present only for the listed architectural vectors.
     4	 * Software yield uses vector 0x81, never an error-code exception vector. */
     5	.section .text,"ax"
     6	.code64
     7	.altmacro
     8	.macro JE_ISR number
     9	je_isr_\number:
    10	.if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
    11	    pushq $0
    12	.endif
    13	    pushq $\number
    14	    jmp je_trap_common
    15	.endm
    16	.set vector,0
    17	.rept 256
    18	JE_ISR %vector
    19	.set vector,vector+1
    20	.endr
    21	je_trap_common:
    22	    cld
    23	    pushq %rax
    24	    pushq %rbx
    25	    pushq %rcx
    26	    pushq %rdx
    27	    pushq %rbp
    28	    pushq %rsi
    29	    pushq %rdi
    30	    pushq %r8
    31	    pushq %r9
    32	    pushq %r10
    33	    pushq %r11
    34	    pushq %r12
    35	    pushq %r13
    36	    pushq %r14
    37	    pushq %r15
    38	    movq %rsp,%rdi
    39	    andq $-16,%rsp
    40	    call je_trap_dispatch
    41	    movq %rax,%rsp
    42	    popq %r15
    43	    popq %r14
    44	    popq %r13
    45	    popq %r12
    46	    popq %r11
    47	    popq %r10
    48	    popq %r9
    49	    popq %r8
    50	    popq %rdi
    51	    popq %rsi
    52	    popq %rbp
    53	    popq %rdx
    54	    popq %rcx
    55	    popq %rbx
    56	    popq %rax
    57	    addq $16,%rsp
    58	    iretq
    59	.section .rodata,"a"
    60	.balign 8
    61	.global je_isr_table
    62	je_isr_table:
    63	.macro JE_ISR_POINTER number
    64	    .quad je_isr_\number
    65	.endm
    66	.set vector,0
    67	.rept 256
    68	JE_ISR_POINTER %vector
    69	.set vector,vector+1
    70	.endr
    71	.section .text,"ax"
    72	.global je_arch_test_stack_overflow
    73	je_arch_test_stack_overflow:
    74	    movq %rdi,%rsp
    75	    pushq $0
    76	    ud2
    77	.section .note.GNU-stack,"",@progbits
    78	
   110	 * NOT here — they stay fatal even from ring 3. */
   111	static int user_terminable_vector(uint64_t v)
   112	{
   113	    switch (v) {
   114	    case 0: case 1: case 3: case 4: case 5: case 6: case 7:
   115	    case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 19:
   116	        return 1;
   117	    default:
   118	        return 0;
   119	    }
   120	}
   121	
   122	je_trap_frame *je_trap_dispatch(je_trap_frame *f)
   123	{
   124	    if (f->vector==32) {
   125	        je_arch_timer_eoi(); return je_sched_interrupt(f,true);
   126	    }
   127	    if (f->vector==0x81) return je_sched_interrupt(f,false);
   128	    uint64_t cr2 = 0;
   129	    if (f->vector == 14) __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
   130	    /* M3a #5 step (b): a CPL0 (#cs==8) not-present #PF at the approved user-copy
   131	     * site, under an active matching copy scope, recovers to the fixup entry
   132	     * (returns a copy failure); ANY other kernel #PF stays fatal. The hook does
   133	     * the full predicate (exact RIP, PFEC, scope, CR2 span, owner, direction). */
   134	    if (f->vector == 14 && f->cs == 8 && je_usercopy_try_fixup(f, cr2, f->error))
   135	        return f;
   136	    /* M3a #5: a user-origin int 0x80 is a SYSCALL (dispatch on RAX), not a fault.
   137	     * Route it before the fault-termination path. A kernel-origin int 0x80 is
   138	     * never issued by the kernel and stays fatal (falls through to PANIC). */
   139	    if ((f->cs & 3) == 3 && f->vector == 0x80)
   140	        return je_syscall_dispatch(f);
   141	    /* Fault-local death: a synchronous fault from ring 3 terminates ONLY that
   142	     * process. User origin requires the saved CS to be the user selector — a
   143	     * kernel-origin fault on any of these vectors stays fatal.
   144	     * je_sched_fault_terminate adds the kind/stack-location cross-checks. */
   145	    if ((f->cs & 3) == 3 && user_terminable_vector(f->vector))
   146	        return je_sched_fault_terminate(f, cr2);
   147	    if (panicking++) je_arch_halt();
   148	    je_arch_console_write("PANIC vector="); je_arch_console_hex(f->vector);
   149	    je_arch_console_write(" error="); je_arch_console_hex(f->error);
   150	    je_arch_console_write(" rip="); je_arch_console_hex(f->rip);
   151	    je_arch_console_write(" cs="); je_arch_console_hex(f->cs);
   152	    je_arch_console_write(" cr2="); je_arch_console_hex(cr2);
   153	    je_arch_console_write("\r\nJeiOS fatal: exception; halted\r\n");
   154	    je_arch_halt();
   155	}

exec
/usr/bin/bash -lc "nl -ba kernel/core/vm.c | sed -n '430,525p'; nl -ba kernel/core/sched.c | sed -n '445,525p'; grep -RIn \"je_endpoint_kern_wait_cancel\" kernel tests tools docs | head -40" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
   430	    uint64_t seen_phys[JE_VM_ARENA_PAGES];
   431	    size_t present_count = 0;
   432	    for (uint64_t pg = 0; pg < JE_VM_ARENA_PAGES; ++pg) {
   433	        uint64_t off = pg * JE_PAGE_SIZE;
   434	        uint64_t phys; je_vm_perm perm; int present;
   435	        if (o->read_user_pte(o->context, index, off, &phys, &perm, &present) != JE_OK)
   436	            return JE_ERR_INVALID;
   437	        if (!present) continue;
   438	        if (off == JE_VM_GUARD_OFFSET) return JE_ERR_INVALID;
   439	        if (perm != JE_VM_PERM_RX && perm != JE_VM_PERM_R_NX &&
   440	            perm != JE_VM_PERM_RW_NX)
   441	            return JE_ERR_INVALID;
   442	
   443	        /* one-to-one: no physical frame may back two arena offsets. */
   444	        for (size_t k = 0; k < present_count; ++k)
   445	            if (seen_phys[k] == phys) return JE_ERR_INVALID;
   446	        seen_phys[present_count++] = phys;
   447	
   448	        /* find the owning extent (by physical range) and check kind/perm/alias */
   449	        bool owned = false;
   450	        for (size_t a = 0; a < p->extent_count && !owned; ++a) {
   451	            const je_vm_extent *ex = &p->extents[a];
   452	            if (ex->kind == JE_VM_TABLE) continue; /* tables never back a user PTE */
   453	            uint64_t end = ex->physical + (uint64_t)ex->pages * JE_PAGE_SIZE;
   454	            if (phys < ex->physical || phys >= end) continue;
   455	            /* kind/perm consistency */
   456	            je_vm_perm want = (ex->kind == JE_VM_USER_CODE)   ? JE_VM_PERM_RX
   457	                            : (ex->kind == JE_VM_USER_RODATA) ? JE_VM_PERM_R_NX
   458	                            :                                   JE_VM_PERM_RW_NX;
   459	            if (perm != want) return JE_ERR_INVALID;
   460	            /* alias state for the specific frame within the extent */
   461	            uint32_t fpage = (uint32_t)((phys - ex->physical) / JE_PAGE_SIZE);
   462	            je_vm_ref ref = { index, ex->backend_id, fpage };
   463	            je_vm_alias al;
   464	            if (o->read_alias(o->context, ref, &al) != JE_OK) return JE_ERR_INVALID;
   465	            je_vm_alias want_al = (ex->kind == JE_VM_USER_DATA) ? JE_VM_ALIAS_RW
   466	                                                                : JE_VM_ALIAS_RO_NX;
   467	            if (al != want_al) return JE_ERR_INVALID;
   468	            owned = true;
   469	        }
   470	        if (!owned) return JE_ERR_INVALID; /* leaf maps a non-owned/table frame */
   471	    }
   472	
   473	    /* completeness: the number of present user leaves must equal the sum of the
   474	     * process's USER_* extent pages (image segments + stack) — no missing and no
   475	     * extra mappings. */
   476	    {
   477	        uint64_t expect = 0;
   478	        for (size_t a = 0; a < p->extent_count; ++a)
   479	            if (p->extents[a].kind != JE_VM_TABLE) expect += p->extents[a].pages;
   480	        if ((uint64_t)present_count != expect) return JE_ERR_INVALID;
   481	    }
   482	
   483	    /* cross-process disjointness: distinct roots and no shared physical frame. */
   484	    for (size_t a = 0; a < p->extent_count; ++a) {
   485	        for (uint32_t fp = 0; fp < p->extents[a].pages; ++fp) {
   486	            uint64_t myp;
   487	            if (o->extent_frame_phys(o->context, index, p->extents[a].backend_id,
   488	                                     fp, &myp) != JE_OK)
   489	                return JE_ERR_INVALID;
   490	            for (uint32_t q = 0; q < JE_VM_MAX_PROCESSES; ++q) {
   491	                if (q == index || vm->procs[q].state != JE_VM_READY) continue;
   492	                const je_vm_process *op = &vm->procs[q];
   493	                if (op->root == p->root) return JE_ERR_INVALID; /* distinct roots */
   494	                for (size_t b = 0; b < op->extent_count; ++b)
   495	                    for (uint32_t pg2 = 0; pg2 < op->extents[b].pages; ++pg2) {
   496	                        uint64_t otherp;
   497	                        if (o->extent_frame_phys(o->context, q,
   498	                                                 op->extents[b].backend_id, pg2,
   499	                                                 &otherp) != JE_OK)
   500	                            return JE_ERR_INVALID;
   501	                        if (otherp == myp) return JE_ERR_INVALID;
   502	                    }
   503	            }
   504	        }
   505	    }
   506	    return JE_OK;
   507	}
   445	
   446	/* M3a #4 step 4: reclaim DYING user processes while peers keep running. Runs in
   447	 * bootstrap (slot 0) with interrupts disabled (caller's responsibility). For
   448	 * each DYING user slot: assert it is not current, not the active CR3, and not
   449	 * the active TSS.RSP0 (the task has been iretq'd away from), then restore its
   450	 * kstack guard and destroy its vm process (freeing the held run + tables). The
   451	 * vm record stays READY until destroy (D5's DYING/REAPABLE are modeled in the
   452	 * scheduler slot, not vm.c). Returns the number reaped, or a negative je_status
   453	 * on an invariant violation. Single-CPU/PCID-off invariant: after one iretq away
   454	 * from the dead task, nothing else runs on its kstack. */
   455	int je_sched_reap_dead(void)
   456	{
   457	    if (!ready || current!=0) return (int)JE_ERR_BUSY;
   458	    /* Must run with interrupts disabled: the three liveness guards below and the
   459	     * destroy must be atomic vs a timer dispatch. Enforce it here rather than
   460	     * trusting the caller (defense in depth). */
   461	    uint64_t rflags; __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
   462	    if (rflags & (UINT64_C(1)<<9)) abort_scheduler(); /* IF set -> misuse */
   463	    uint64_t active_cr3=je_arch_cr3_read(), active_rsp0=je_arch_tss_get_rsp0();
   464	    int reaped=0;
   465	    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) {
   466	        if (i==current) return (int)JE_ERR_BUSY;
   467	        if (threads[i].root==active_cr3) return (int)JE_ERR_BUSY;    /* still the live address space */
   468	        if (threads[i].rsp0_top==active_rsp0) return (int)JE_ERR_BUSY; /* still the live kernel stack */
   469	        je_vm *vm=threads[i].vm; uint32_t idx=threads[i].vm_index;
   470	        je_status s=je_sched_drop_user(vm, idx); /* restores the kstack guard, clears the slot */
   471	        if (s!=JE_OK) return (int)s;
   472	        s=je_vm_destroy(vm, idx);                /* frees the held kstack run + page tables */
   473	        if (s!=JE_OK) return (int)s;
   474	        ++reaped;
   475	    }
   476	    return reaped;
   477	}
   478	
   479	/* M3a #4 step 4 proof accessors: whether a DYING user slot exists and its death
   480	 * record, plus a single-slot dispatch counter so the proof can show a peer
   481	 * progressed AFTER a specific death. Slot identity is by vm process. */
   482	bool je_sched_has_dying(void)
   483	{
   484	    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) return true;
   485	    return false;
   486	}
   487	je_status je_sched_death_record(je_vm *vm, uint32_t index, uint64_t *vec, uint64_t *err,
   488	                                uint64_t *rip, uint64_t *cs, uint64_t *cr2)
   489	{
   490	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   491	        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index &&
   492	            threads[i].state==DYING) {
   493	            if (vec) *vec=threads[i].death_vector;
   494	            if (err) *err=threads[i].death_error;
   495	            if (rip) *rip=threads[i].death_rip;
   496	            if (cs)  *cs=threads[i].death_cs;
   497	            if (cr2) *cr2=threads[i].death_cr2;
   498	            return JE_OK;
   499	        }
   500	    return JE_ERR_INVALID;
   501	}
   502	/* M3a #5: the death reason + exit code for a DYING user slot (proof accessor). */
   503	je_status je_sched_death_reason(je_vm *vm, uint32_t index, uint64_t *reason, uint64_t *code)
   504	{
   505	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   506	        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index &&
   507	            threads[i].state==DYING) {
   508	            if (reason) *reason=threads[i].death_reason;
   509	            if (code)   *code=threads[i].exit_code;
   510	            return JE_OK;
   511	        }
   512	    return JE_ERR_INVALID;
   513	}
   514	uint64_t je_sched_dispatches_of(je_vm *vm, uint32_t index)
   515	{
   516	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   517	        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index)
   518	            return threads[i].dispatches;
   519	    return 0;
   520	}
   521	/* M3a #5 step (b): the currently-running user process (vm + index), for user-copy
   522	 * prevalidation. Returns JE_OK only when the current thread is a user thread. */
   523	je_status je_sched_current_user(je_vm **vm, uint32_t *index)
   524	{
   525	    if (!ready || threads[current].kind!=THREAD_USER) return JE_ERR_INVALID;
kernel/core/endpoint_kern.c:319:bool je_endpoint_kern_wait_cancel(uint32_t slot)

exec
/usr/bin/bash -lc "grep -n \"je_vm_destroy\" kernel/core/vm.c && nl -ba kernel/core/vm.c | sed -n '300,385p' && nl -ba kernel/core/sched.c | sed -n '521,555p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
384:je_status je_vm_destroy(je_vm *vm, uint32_t index)
   300	    *p = (je_vm_process){0};
   301	    p->state = JE_VM_BUILDING;
   302	
   303	    /* 4a. hold the concrete kstack run (ADR D4). */
   304	    uint64_t kphys = 0;
   305	    s = o->reserve_hold(o->context, JE_VM_KSTACK_PAGES, &kphys);
   306	    if (s != JE_OK) { unwind(vm, idx); return s; }
   307	    p->kstack_physical = kphys;
   308	    p->held_kstack = JE_VM_KSTACK_PAGES;
   309	    { je_vm_journal_entry he = { JE_VM_J_HOLD, 0, 0, 0, kphys, JE_VM_KSTACK_PAGES };
   310	      journal_push(vm, p, he); }
   311	
   312	    /* 4b. table extent + topology. */
   313	    uint32_t table_ex;
   314	    s = build_extent(vm, idx, JE_VM_TABLE_PAGES, JE_VM_TABLE, &table_ex);
   315	    if (s != JE_OK) { unwind(vm, idx); return s; }
   316	    uint32_t root;
   317	    s = o->link_tables(o->context, idx, table_ex, &root);
   318	    if (s != JE_OK) { unwind(vm, idx); return s; }
   319	    p->root = root;
   320	    { je_vm_journal_entry te = { JE_VM_J_TABLES, table_ex, 0, 0, 0, 0 };
   321	      journal_push(vm, p, te); }
   322	
   323	    /* 4c. image segments. */
   324	    const uint8_t *bytes = blob;
   325	    for (uint32_t si = 0; si < image->segment_count; ++si) {
   326	        je_vm_segment seg = image->segments[si];
   327	        je_vm_perm leaf;
   328	        je_vm_kind kind = perm_to_kind(seg.perm, &leaf);
   329	        uint64_t seg_pages = seg.memory_size / JE_PAGE_SIZE;
   330	        uint32_t ex;
   331	        s = build_extent(vm, idx, (size_t)seg_pages, kind, &ex);
   332	        if (s != JE_OK) { unwind(vm, idx); return s; }
   333	        for (uint64_t pg = 0; pg < seg_pages; ++pg) {
   334	            uint64_t off_in_seg = pg * JE_PAGE_SIZE;
   335	            uint64_t varena = seg.target_offset + off_in_seg;
   336	            /* source bytes for this page (0 for BSS-only pages). Compute the
   337	             * source pointer ONLY when there are bytes to copy, so we never form
   338	             * an out-of-bounds pointer (UB) for a BSS page past the blob end. */
   339	            uint64_t copy = 0;
   340	            const uint8_t *src = NULL;
   341	            if (off_in_seg < seg.source_size) {
   342	                uint64_t remaining = seg.source_size - off_in_seg;
   343	                copy = remaining < JE_PAGE_SIZE ? remaining : JE_PAGE_SIZE;
   344	                src = bytes + seg.source_offset + off_in_seg;
   345	            }
   346	            s = build_page(vm, idx, ex, (uint32_t)pg, kind, varena, src, copy);
   347	            if (s != JE_OK) { unwind(vm, idx); return s; }
   348	        }
   349	    }
   350	
   351	    /* 4d. user stack (USER_DATA), top of arena. */
   352	    uint32_t stack_ex;
   353	    s = build_extent(vm, idx, JE_VM_STACK_PAGES, JE_VM_USER_DATA, &stack_ex);
   354	    if (s != JE_OK) { unwind(vm, idx); return s; }
   355	    for (uint64_t pg = 0; pg < JE_VM_STACK_PAGES; ++pg) {
   356	        uint64_t varena = JE_VM_STACK_OFFSET + pg * JE_PAGE_SIZE;
   357	        s = build_page(vm, idx, stack_ex, (uint32_t)pg, JE_VM_USER_DATA,
   358	                       varena, NULL, 0);
   359	        if (s != JE_OK) { unwind(vm, idx); return s; }
   360	    }
   361	
   362	    /* M3b: initialize the per-process handle table over the inline slot storage
   363	     * before publishing READY (ADR 0008). Capacity is JE_VM_HANDLE_CAP; quota is
   364	     * kept BELOW capacity so a handle_self loop hits QUOTA (not NOSLOTS) — the
   365	     * two are reported distinctly. Compile-time-sized, so init cannot fail here;
   366	     * the teardown/unwind zeroing (which sets capacity 0 -> every lookup
   367	     * BADINDEX) reclaims it, and re-init on slot reuse prevents a stale LIVE
   368	     * entry from surviving into a reused process slot. */
   369	    if (!je_handle_table_init(&p->htable, p->hslots, JE_VM_HANDLE_CAP,
   370	                              JE_VM_HANDLE_CAP - 1u)) {
   371	        unwind(vm, idx);
   372	        return JE_ERR_INVALID;   /* unreachable: capacity/quota are compile-time */
   373	    }
   374	
   375	    /* publish. */
   376	    p->entry = image->entry_offset;
   377	    p->state = JE_VM_READY;
   378	    *out_index = idx;
   379	    return JE_OK;
   380	}
   381	
   382	/* ---- destroy -------------------------------------------------------------- */
   383	
   384	je_status je_vm_destroy(je_vm *vm, uint32_t index)
   385	{
   521	/* M3a #5 step (b): the currently-running user process (vm + index), for user-copy
   522	 * prevalidation. Returns JE_OK only when the current thread is a user thread. */
   523	je_status je_sched_current_user(je_vm **vm, uint32_t *index)
   524	{
   525	    if (!ready || threads[current].kind!=THREAD_USER) return JE_ERR_INVALID;
   526	    if (vm) *vm=threads[current].vm;
   527	    if (index) *index=threads[current].vm_index;
   528	    return JE_OK;
   529	}
   530	uint64_t je_sched_min_dispatches(void)
   531	{
   532	    uint64_t min=UINT64_MAX;
   533	    for (unsigned i=1;i<JE_THREAD_MAX;++i)
   534	        if (threads[i].state!=EMPTY && threads[i].dispatches<min) min=threads[i].dispatches;
   535	    return min;
   536	}
   537	

exec
/usr/bin/bash -lc "nl -ba kernel/core/vm.c | sed -n '382,430p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
   382	/* ---- destroy -------------------------------------------------------------- */
   383	
   384	je_status je_vm_destroy(je_vm *vm, uint32_t index)
   385	{
   386	    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
   387	    if (vm->procs[index].state != JE_VM_READY) return JE_ERR_INVALID;
   388	    /* A3-1: the release walk + unwind + the *p={0} zeroing in unwind() must be
   389	     * ONE critical section. The direct main.c destroy callers may run with IF=1,
   390	     * so an IF=0 precondition is not enough: a timer preempting between the walk
   391	     * and the zeroing, while the peer process is live, would let the peer's
   392	     * HANDLE_CLOSE mutate the same channel record mid-teardown (a UAF). Hold IRQs
   393	     * off across the whole sequence when the kernel installed the guard; NULL
   394	     * (the unit test) keeps the prior behaviour. The teardown hook (the endpoint
   395	     * release walk) runs after the checks above and immediately before unwind. */
   396	    uint64_t irq = 0u;
   397	    if (vm->irq_save) irq = vm->irq_save();
   398	    if (vm->teardown) vm->teardown(vm->teardown_ctx, &vm->procs[index].htable);
   399	    unwind(vm, index);
   400	    if (vm->irq_restore) vm->irq_restore(irq);
   401	    return JE_OK;
   402	}
   403	
   404	/* ---- audit oracle --------------------------------------------------------- */
   405	
   406	je_status je_vm_audit(const je_vm *vm, uint32_t index)
   407	{
   408	    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
   409	    const je_vm_ops *o = vm->ops;
   410	    const je_vm_process *p = &vm->procs[index];
   411	    if (p->state != JE_VM_READY) return JE_ERR_INVALID;
   412	
   413	    /* upper levels: PML4[0] P|U; kernel PDPT[0] P, !U; arena PDPT[1] P|U;
   414	     * arena PD[0] P|U. */
   415	    uint32_t fl;
   416	    if (o->read_upper(o->context, index, 4, 0, &fl) != JE_OK) return JE_ERR_INVALID;
   417	    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
   418	    if (o->read_upper(o->context, index, 3, 0, &fl) != JE_OK) return JE_ERR_INVALID;
   419	    if (!(fl & 1u) || (fl & 4u)) return JE_ERR_INVALID;              /* P, !U */
   420	    if (o->read_upper(o->context, index, 3, 1, &fl) != JE_OK) return JE_ERR_INVALID;
   421	    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
   422	    if (o->read_upper(o->context, index, 2, 0, &fl) != JE_OK) return JE_ERR_INVALID;
   423	    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
   424	
   425	    /* Walk every arena page: a present user leaf must (a) not be the guard page,
   426	     * (b) carry a perm that is one of the three legal values, (c) map a physical
   427	     * frame that belongs to one of THIS process's USER_* extents, and (d) match
   428	     * that extent's kind (CODE->RX, RODATA->R_NX, DATA->RW_NX). Also verify the
   429	     * owning frame's identity alias state: RO_NX for CODE/RODATA, RW for DATA. */
   430	    uint64_t seen_phys[JE_VM_ARENA_PAGES];

exec
/usr/bin/bash -lc "grep -n \"CFLAGS\\|Wconversion\\|Werror\\|SANIT\\|check:\" Makefile mk/*.mk | head -160; git show HEAD:kernel/core/sched.c | sed -n '1,150p' | tail -30" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
Makefile:9:WARN := -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes
Makefile:17:CFLAGS := -std=c11 -O2 -g $(WARN) -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mno-red-zone -mgeneral-regs-only
Makefile:27:	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
Makefile:44:check: all test
mk/aarch64.mk:24:ARM_CFLAGS  := -Iinclude -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion \
mk/aarch64.mk:36:ARM_CFLAGS  += -DJE_ARM_BSS_CHECK
mk/aarch64.mk:48:	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@
    threads[slot].frame=0;
    threads[slot].death_reason=(uint64_t)reason; threads[slot].exit_code=code;
    threads[slot].death_vector=vec; threads[slot].death_error=err;
    threads[slot].death_rip=rip; threads[slot].death_cs=cs; threads[slot].death_cr2=cr2;
}

/* Install the chosen next thread's address space and return its frame. For a
 * user next thread, validate+sanitize its saved frame first; if that FAILS the
 * thread is terminated (DYING) and we retry with another candidate — a user
 * task must never be able to halt the kernel via a bad saved frame (fault
 * locality; fixes the pre-step-4 abort-on-bad-return-state hole). Bootstrap
 * (slot 0) is always READY, so a live candidate always exists. Returns the
 * frame to iretq to; never requeues a DYING/ZOMBIE thread. */
static je_trap_frame *select_and_install_next(void)
{
    for (unsigned attempt=0; attempt<JE_THREAD_MAX; ++attempt) {
        unsigned next=current;
        for (unsigned n=1;n<=JE_THREAD_MAX;++n) {
            unsigned candidate=(current+n)%JE_THREAD_MAX;
            if (threads[candidate].state==READY) { next=candidate;break; }
        }
        if (threads[next].state!=READY || !threads[next].frame) abort_scheduler();
        if (threads[next].kind==THREAD_USER) {
            uint64_t sane;
            if (!je_uframe_check(threads[next].frame, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
                /* Bad saved return state -> terminate THIS task, not the kernel. */
                mark_dying(next, DEATH_BAD_RETURN, 0, 0, 0, threads[next].frame->rip,
                           threads[next].frame->cs, 0);
                continue; /* pick another candidate */
            }

codex
Found 6 real defects. Local `make check` and `make smoke integration` pass at `927ee21`; these paths are largely outside the tested timeout-only fixture.

1. **P1 — process-death close does not wake peer waiters.**  
   `kernel/core/endpoint.c:527-541` calls `je_endpoint_handle_close()` directly during `je_vm_destroy`; only `kernel/core/endpoint_kern.c:71-84` invokes `kern_peer_close_wake()`. A surviving receiver therefore misses `PEER_CLOSED` and waits until timeout.

2. **P1 — WOKEN + `SHORT_BUFFER` permanently wedges the reservation.**  
   `kernel/core/syscall.c:258-265` returns `SHORT_BUFFER` without releasing the waiter. The caller then clears `wait_pending` at `:337`; a retry cannot use the owned continuation, while `present + WOKEN + head_reserved` makes the message permanently unavailable until thread death.

3. **P1 — reserved-head drain-first regression on cap-aware paths.**  
   `kernel/core/endpoint.c:267-269` and `:836-839` return `PEER_CLOSED` whenever the end is peer-closed, even when the reserved queue head is nonempty. Contract requires `EMPTY` for other readers; `recv_peek()` at `:633-636` gets this right.

4. **P1 — cancellation can cancel the wrong waiter.**  
   `kernel/core/endpoint_kern.c:319-331` arbitrates `CANCELLED` before checking `id == slot`. With multiple waiters, it terminally cancels earlier unrelated waiters, does not wake them, then may return false or continue corrupting cancellation semantics.

5. **P2 — waiter fields are not initialized by pool initialization.**  
   `kernel/core/endpoint.c:56-70` never initializes `end[s].waiter`. A caller using a non-zeroed automatic `je_endpoint_pool` can read indeterminate `present/cause/head_reserved`; ASan/UBSan need not catch this. Record reuse also relies on every prior exit path clearing the waiter.

6. **P2 — maximum legal deadline can never be serviced.**  
   `kernel/core/syscall.c:537-539` accepts `deadline == UINT64_MAX`; `kernel/core/sched.c:208-210` aborts when ticks reach `UINT64_MAX` before the timeout sweep at `:218-230`. Also, the timeout bound/overflow check is skipped when the entry probe finds an immediately deliverable message (`:533-556`), so an over-limit timeout can succeed.

The continuation prologue itself is sound: trap entry preserves RAX, it runs before decode, the restart RIP is revalidated, and normal release paths are idempotent. The close-after-free read is not an exploitable UAF with the current inline static pool: a waiter’s inflight ref prevents reclamation, and freed records are reset to `FREE`.
Found 6 real defects. Local `make check` and `make smoke integration` pass at `927ee21`; these paths are largely outside the tested timeout-only fixture.

1. **P1 — process-death close does not wake peer waiters.**  
   `kernel/core/endpoint.c:527-541` calls `je_endpoint_handle_close()` directly during `je_vm_destroy`; only `kernel/core/endpoint_kern.c:71-84` invokes `kern_peer_close_wake()`. A surviving receiver therefore misses `PEER_CLOSED` and waits until timeout.

2. **P1 — WOKEN + `SHORT_BUFFER` permanently wedges the reservation.**  
   `kernel/core/syscall.c:258-265` returns `SHORT_BUFFER` without releasing the waiter. The caller then clears `wait_pending` at `:337`; a retry cannot use the owned continuation, while `present + WOKEN + head_reserved` makes the message permanently unavailable until thread death.

3. **P1 — reserved-head drain-first regression on cap-aware paths.**  
   `kernel/core/endpoint.c:267-269` and `:836-839` return `PEER_CLOSED` whenever the end is peer-closed, even when the reserved queue head is nonempty. Contract requires `EMPTY` for other readers; `recv_peek()` at `:633-636` gets this right.

4. **P1 — cancellation can cancel the wrong waiter.**  
   `kernel/core/endpoint_kern.c:319-331` arbitrates `CANCELLED` before checking `id == slot`. With multiple waiters, it terminally cancels earlier unrelated waiters, does not wake them, then may return false or continue corrupting cancellation semantics.

5. **P2 — waiter fields are not initialized by pool initialization.**  
   `kernel/core/endpoint.c:56-70` never initializes `end[s].waiter`. A caller using a non-zeroed automatic `je_endpoint_pool` can read indeterminate `present/cause/head_reserved`; ASan/UBSan need not catch this. Record reuse also relies on every prior exit path clearing the waiter.

6. **P2 — maximum legal deadline can never be serviced.**  
   `kernel/core/syscall.c:537-539` accepts `deadline == UINT64_MAX`; `kernel/core/sched.c:208-210` aborts when ticks reach `UINT64_MAX` before the timeout sweep at `:218-230`. Also, the timeout bound/overflow check is skipped when the entry probe finds an immediately deliverable message (`:533-556`), so an over-limit timeout can succeed.

The continuation prologue itself is sound: trap entry preserves RAX, it runs before decode, the restart RIP is revalidated, and normal release paths are idempotent. The close-after-free read is not an exploitable UAF with the current inline static pool: a waiter’s inflight ref prevents reclamation, and freed records are reset to `FREE`.
