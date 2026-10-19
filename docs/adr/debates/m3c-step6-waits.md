# Peer debate — 2026-10-19T02:08:16Z
Topic: JeiOS M3c step 6 blocking-recv waits (D7): minimal correct sched.c design + proof plan + what to escalate
Rounds: 2   Roles: claude=implementor codex=adversarial-reviewer
Context file: /tmp/step6.md


[R1 CLAUDE] **Position (implementor): the Q2 "syscall loop that resumes after the park point" doesn't fit this kernel, and that settles Q1–Q3.** Verified on main @ e914a95:

- A user syscall has no kernel continuation. `je_syscall_dispatch` *returns a frame* (`syscall.c:225-259`). The user thread's whole saved state is its trap frame at `rsp0_top - sizeof(frame)` (`sched.c:174-176`). Resuming it means `iretq` on that frame. Only kernel threads park mid-function (`je_thread_sleep` → `int $0x81`, `sched.c:242`). So a blocked recv can't "return from park and re-check". **Whoever performs the terminal transition must write the result into the blocked thread's saved frame.** That is literally what D7 says ("the ISR performs the transition and delivers the result, not merely mark READY"). Q3 isn't an interpretation question: the architecture forces the ADR reading.
- Restarting the syscall (rewind RIP over `int 0x80` and re-execute) is also ruled out. The re-executed recv would re-resolve the handle, and D3 is frozen: "a woken thread uses the waiter's owned ref, never a re-resolve of the original handle."
- The copy must happen in the receiver's CR3. A sender completing the receiver's recv is running under the sender's CR3, so it can't do the copy-out.

## Recommended design

**Q1, a hybrid split.** The thread struct holds `wait_state` (WAITING or a terminal cause), the deadline (reuse `wake`) and `wait_end`. The end record holds a single back-link `waiter` (thread slot or NONE) plus the waiter's `inflight_refs` (D3 alias 3). Neither pure A nor pure B works. The end needs the back-link so a sender or peer-close can find the waiter. The thread needs the state so the ISR can scan it.

**Q2/Q3, each trigger does an eager CAS at its own instant.** IF=0 serializes every trigger, so the case where "several triggers are eligible at once" only arises inside one atomic section. The priority table applies there (for example, the ISR finds the deadline passed AND the end PEER_CLOSED → PEER_CLOSED).
- **Sender:** enqueue, then CAS the peer's waiter WAITING→WOKEN, then READY.
- **Peer close or death:** when the end goes CLOSED, CAS the opposite waiter →PEER_CLOSED, write `rax`, READY.
- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.

A CAS that loses is a no-op. There is no re-check at the syscall; the CAS winner is the single source of truth.

**WOKEN delivery is deferred to dispatch.** I propose one hook in `select_and_install_next`, after the `je_uframe_check` and CR3 load. It runs for a thread with a pending completion and calls `je_ep_wait_finish(slot)` (endpoint.c). That function:
- unlinks the end's back-link;
- releases the waiter's inflight ref;
- for WOKEN, runs the ordinary try_recv commit (copy-out with re-validation and fixup, cap mint), so a copy fault becomes an error result, as D7 requires.

The timeout and peer-closed paths can also defer their unlink and ref release to this hook. That keeps endpoint-pool frees out of ISR context. The ISR still writes the final status itself, so it stays faithful to D7.

**New hazard: theft between the WOKEN stamp and dispatch.** A5 transfer lets both processes hold the same end. The other process can be scheduled first and `try_recv` the message that WOKEN promised. Two fixes:
- **(a) Reservation (my pick).** While a WOKEN completion is pending, the end's head message is reserved, so other receivers see `EMPTY`. No new ABI.
- **(b)** Dequeue into a per-waiter kernel staging buffer. This costs JE_THREAD_MAX × max message size, and putting the message back after a copy fault gets messy.

Also: **at most one waiter per end.** A second blocking recv needs a status code (open question below).

**Q4, both kinds of work are needed:**
1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
2. **Peer death or close.** This must actively wake the receiver with PEER_CLOSED through the close hook above. A plain `handle_close` that drops `handle_refs` to 0 must trigger it too, not only death.

**Q6, the minimal sched.c diff:**
- one enum value;
- three thread fields;
- `je_sched_block_current_ipc(f, deadline, end)` (sets state, returns the successor frame);
- `je_sched_ipc_complete(slot, cause, rax, rdx)` (CAS, frame write, READY);
- the ISR scan branch;
- the dispatch hook call;
- a `je_sched_unlink_ipc(vm)` that destroy calls.

Everything else lives in endpoint.c and syscall.c: the queue check, the immediate-delivery path, the reservation, the copy and the priority choice. Also bump the D4 `_Static_assert`: `JE_EP_REF_MAX` must be ≥ queue depth plus max waiters.

## Proof plan (deterministic; nothing spins in ring 3)

The bootstrap kernel thread drives every test and observes state through accessors. A user stub never waits on another user stub.

| Cause | Proof |
|---|---|
| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
| TIMED_OUT | The user blocks with deadline N and nothing sends. After N ticks, assert `rax=TIMED_OUT` in the resumed thread. |
| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
| PEER_CLOSED | Bootstrap destroys the peer vm while the receiver is blocked. |
| Same-instant tie | Inside one IF=0 section, enqueue then run the tick path by hand (deadline already passed). Assert WOKEN. Do the same with peer-close plus deadline and assert PEER_CLOSED. |
| Receiver death | Destroy the blocked receiver, then send. Assert no wake, the back-link is cleared and the ref count returns to baseline. |
| Theft | The other holder calls `try_recv` while a WOKEN completion is pending and gets `EMPTY`. |
| Copy fault after wake | Needs a proof-only hook that poisons the saved buffer pointer, because nothing in M3c can unmap the buffer. Flag it as an injected fault, not organic. |

Also add a host `make check` table test of the CAS and priority function.

## Escalate to the owner

1. **The D7 reading is forced by the frame-return architecture**, so the owner should confirm it rather than choose it. The ISR writes the TIMED_OUT result into the frame; dispatch-time completion handles copy-out, unlink and ref release; there is no syscall re-check. Owner sign-off is needed on the dispatch hook in `select_and_install_next`, which is tested code.
2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
3. **New ABI surface:** a blocking-recv op number, the timeout register (R10 is currently reserved=0 on try_recv), a `JE_EP_WAIT_MAX_TICKS` cap, and whether a timeout of 0 means try_recv or INVALID.
4. **A status for a second waiter on the same end**, and the choice between reservation and staging for the theft hazard. This is open for an agent majority vote unless the owner wants it.
[R1 CODEX] codex
Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
But several parts of the proposal are not yet contract-safe.
1. Q1: avoid the endpoint back-link initially
Use a thread-owned waiter:
- `wait_state`
- `wait_end` (protected by an owned endpoint reference)
- deadline
- saved receive arguments / completion metadata
- pending completion flag
Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
- stale endpoint-to-thread IDs;
- a second registry lifecycle;
- silently imposing “one waiter per endpoint,” which D7 does not authorize;
- extra unlink work during process death.
A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
2. Q2: no syscall re-check loop
The syscall path should be:
1. Check queue/closed state.
2. If immediately deliverable, complete synchronously.
3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
4. A terminal transition writes scalar results into the saved frame.
5. On selection of the thread, a resume-completion hook performs the owned-endpoint receive commit and copy-out under the receiver’s CR3.
The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
The WOKEN state and the eventual syscall result need separate concepts. A post-wake user-buffer fault must preserve the terminal wait cause while returning the specified normal copy error.
3. Q3: Claude overstates that the interpretation is settled
The architecture settles “no syscall loop,” but not the exact meaning of D7’s “ISR performs the terminal transition.”
The timer path must not implement:
```text
WAITING -> TIMED_OUT
```
merely as a timeout hint followed by a syscall re-check. The ISR must invoke the terminal-transition helper and install the timeout result itself.
The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
There is a second unresolved case: sender enqueues and marks WOKEN, then peer death occurs before the receiver runs. Both WOKEN and PEER_CLOSED are now eligible. “First CAS wins” gives WOKEN; the stated priority could imply PEER_CLOSED. The ADR needs an owner decision on whether priority applies only to simultaneous eligibility inside one critical section, or whether a later higher-priority trigger may supersede a prior terminal transition. Superseding would conflict with the single-terminal-transition rule, so the likely safe interpretation is “priority at one arbitration point,” but that must be recorded.
A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
4. Q4: teardown requires more than changing `DYING`
For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
For peer death, the endpoint release walk must actively wake affected receivers with `PEER_CLOSED`; merely setting endpoint state is insufficient because a far deadline delays re-entry indefinitely.
The close path must distinguish “this process died” from “the peer side is actually closed.” Shared/transferred capabilities may mean peer death does not immediately imply closure.
5. Q5: deterministic proof set
Use one-shot kernel/user actors and explicit yields. No ring-3 spin loops.
- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
- WOKEN: receiver blocks; a scheduled sender performs one send and yields/exits; verify reserved payload and status.
- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
- PEER_CLOSED: receiver blocks; destroy/close the peer; verify active wake and result.
- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
- Copy fault: use an available VM mapping/unmapping test hook; otherwise mark this as unverified integration coverage and test the copy-fixup path separately.
- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
Claude’s proposed `RAX=...`, `RDX=0`, timeout register, and `JE_EP_REF_MAX` assertion are not established by the supplied contract. They require ABI/source verification or owner approval.
6. Minimal sched.c change
The semantic minimum is:
- add `THREAD_BLOCKED_IPC`;
- add thread-owned waiter fields;
- add `je_sched_block_ipc(...)`;
- add one compare-and-set terminal helper used by sender, close, cancel, and timer;
- add the timer scan invoking that helper;
- add teardown unlink/tombstone before slot reuse;
- add one common return-path hook for deferred owned-endpoint completion.
Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
Owner escalation should be limited to:
1. Whether deferred WOKEN copy-out before `iretq` satisfies D7’s delivery requirement.
2. Priority semantics when a lower-priority terminal cause wins before a later higher-priority trigger becomes eligible.
3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
4. The exact v1 ABI for deadline, result registers, zero timeout, and copy-fault status.
5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
But several parts of the proposal are not yet contract-safe.
1. Q1: avoid the endpoint back-link initially
Use a thread-owned waiter:
- `wait_state`
- `wait_end` (protected by an owned endpoint reference)
- deadline
- saved receive arguments / completion metadata
- pending completion flag
Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
- stale endpoint-to-thread IDs;
- a second registry lifecycle;
- silently imposing “one waiter per endpoint,” which D7 does not authorize;
- extra unlink work during process death.
A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
2. Q2: no syscall re-check loop
The syscall path should be:
1. Check queue/closed state.
2. If immediately deliverable, complete synchronously.
3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
4. A terminal transition writes scalar results into the saved frame.
5. On selection of the thread, a resume-completion hook performs the owned-endpoint receive commit and copy-out under the receiver’s CR3.
The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
The WOKEN state and the eventual syscall result need separate concepts. A post-wake user-buffer fault must preserve the terminal wait cause while returning the specified normal copy error.
3. Q3: Claude overstates that the interpretation is settled
The architecture settles “no syscall loop,” but not the exact meaning of D7’s “ISR performs the terminal transition.”
The timer path must not implement:
```text
WAITING -> TIMED_OUT
```
merely as a timeout hint followed by a syscall re-check. The ISR must invoke the terminal-transition helper and install the timeout result itself.
The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
There is a second unresolved case: sender enqueues and marks WOKEN, then peer death occurs before the receiver runs. Both WOKEN and PEER_CLOSED are now eligible. “First CAS wins” gives WOKEN; the stated priority could imply PEER_CLOSED. The ADR needs an owner decision on whether priority applies only to simultaneous eligibility inside one critical section, or whether a later higher-priority trigger may supersede a prior terminal transition. Superseding would conflict with the single-terminal-transition rule, so the likely safe interpretation is “priority at one arbitration point,” but that must be recorded.
A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
4. Q4: teardown requires more than changing `DYING`
For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
For peer death, the endpoint release walk must actively wake affected receivers with `PEER_CLOSED`; merely setting endpoint state is insufficient because a far deadline delays re-entry indefinitely.
The close path must distinguish “this process died” from “the peer side is actually closed.” Shared/transferred capabilities may mean peer death does not immediately imply closure.
5. Q5: deterministic proof set
Use one-shot kernel/user actors and explicit yields. No ring-3 spin loops.
- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
- WOKEN: receiver blocks; a scheduled sender performs one send and yields/exits; verify reserved payload and status.
- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
- PEER_CLOSED: receiver blocks; destroy/close the peer; verify active wake and result.
- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
- Copy fault: use an available VM mapping/unmapping test hook; otherwise mark this as unverified integration coverage and test the copy-fixup path separately.
- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
Claude’s proposed `RAX=...`, `RDX=0`, timeout register, and `JE_EP_REF_MAX` assertion are not established by the supplied contract. They require ABI/source verification or owner approval.
6. Minimal sched.c change
The semantic minimum is:
- add `THREAD_BLOCKED_IPC`;
- add thread-owned waiter fields;
- add `je_sched_block_ipc(...)`;
- add one compare-and-set terminal helper used by sender, close, cancel, and timer;
- add the timer scan invoking that helper;
- add teardown unlink/tombstone before slot reuse;
- add one common return-path hook for deferred owned-endpoint completion.
Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
Owner escalation should be limited to:
1. Whether deferred WOKEN copy-out before `iretq` satisfies D7’s delivery requirement.
2. Priority semantics when a lower-priority terminal cause wins before a later higher-priority trigger becomes eligible.
3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
4. The exact v1 ABI for deadline, result registers, zero timeout, and copy-fault status.
5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
[R2 CLAUDE] I checked the repo at e914a95. Several of Codex's open questions are already answered by frozen text or merged code, so the escalation list can shrink. I agree with most of Codex's corrections. I dispute two: its timer-path rule and how open the priority question is. It also missed two hazards.

## Agree
- **Thread-owned waiter plus a bounded scan of `JE_THREAD_MAX`, no endpoint back-link.** ADR 0009 D3 item 3 says a waiter holds an `inflight_refs` on the end and a reference to the blocked thread, which must be unlinked before the thread's storage is reused. If the waiter lives in the thread slot, that thread reference points to itself and can never dangle. Teardown then only has to clear the slot's waiter and release its inflight ref.
- **The resume path needs an owned-ref commit, not plain `try_recv`.** This is verified: `je_endpoint_recv_peek` calls `je_endpoint_resolve(pool, t, h, …)` (endpoint.c ~496), which re-resolves the handle. D3 forbids that for a woken thread. We need a variant keyed on `(rec, side)` that reuses the existing `je_ep_recv_lease` two-phase commit.
- **Peer death must actively wake a blocked receiver.** The hook belongs at the refcount-reaches-zero transition (endpoint.c:208, `peer->state = JE_EP_END_PEER_CLOSED`), not at process death. That already separates "process died" from "end closed" when capabilities are shared or transferred, so it needs no extra design.
- **CANCELLED has no v1 trigger.** `je_thread_create_user(vm, index)` and the slot lookups (sched.c:295/353) give one user thread per process. The only thread that could close the waited handle is the blocked thread itself. The receiver's own death tombstones the waiter and delivers no result, so that is not CANCELLED either. Ship an internal helper with a unit test and document it as unreachable from the ABI.

## Dispute
1. **The timer ISR should not judge eligibility itself.** Make it a structural invariant instead: under IF=0, every enqueue and every peer-close transition calls the wake helper in the same critical section. So a WAITING waiter can never coexist with a deliverable message or a peer-closed state. The timer then does a plain `CAS WAITING→TIMED_OUT` and writes the result into the saved frame, with an assert-or-`JE_EP_CORRUPT` check on the invariant. If the ISR evaluated eligibility, it would have to reach into the endpoint pool, which is exactly the coupling we are keeping out of tested `sched.c`. TIMED_OUT is a plain status code, so the ISR fully delivers it. D7's "ISR performs the transition and delivers the result" is met literally for TIMED_OUT.
2. **"WOKEN, then the peer dies" has one safe reading, fixed by existing code.** Merged `try_recv` drains queued messages before reporting PEER_CLOSED (endpoint.c:517-520: PEER_CLOSED only when `count == 0`). Read literally, PEER_CLOSED > WOKEN would discard a queued message, and the merged step-4 semantics don't allow that. So the priority applies at a single arbitration point, and PEER_CLOSED counts as eligible only when the inbound queue is empty. Record this as a clarification for the owner to confirm, not as an open design question.
3. **Codex's ABI escalation #4 is partly answered already.** D6 freezes the copy-fault status: `JE_ABI_INVALID`, message stays queued, no dequeue on a faulting copy-out. Only the deadline register, zero-timeout behaviour and the maximum deadline remain open.
4. **Codex's multi-waiter escalation #3 is partly answered already.** `JE_EP_MAX_WAITERS = 2` exists (endpoint.h:56), and endpoint.c:13 asserts `REF_MAX >= QUEUE_DEPTH + MAX_WAITERS`, so two waiters per end is the configured design. A third waiter fails at its ref acquire. What needs a decision is the status a waiter beyond the limit gets.

## What Codex missed
- **Reservation interacts with copy faults.** The frozen fault rule leaves the message queued. If waiter A is WOKEN with message 0 reserved and its copy-out faults, A's terminal result is WOKEN with status `JE_ABI_INVALID`. Message 0 is then unreserved but waiter B is still WAITING next to a deliverable message, which breaks the invariant above. The fault-release path must re-run the wake helper for that end.
- **Reserving more than one message breaks the queue.** Out-of-order completion would dequeue from the middle of the ring. Rule: an end has at most one outstanding reservation, always on the head message, and the next waiter is woken only after the current one commits or releases. That keeps FIFO order without restructuring the queue.
- **Where the completion hook runs is the riskiest part of the `sched.c` change.** Dispatch is frame-return based (sched.c:140-159: pick, `je_uframe_check`, `cr3_load`, return the frame). The deferred copy-out has to run after `je_arch_cr3_load(threads[next].root)` and before the return, so it runs inside the scheduler. ADR D3 relies on the M3a exact-RIP fixup never routing a copy fault to the scheduler, but nobody has tested a fault taken from within `je_sched_interrupt`. That needs a dedicated test (unmap the receive buffer while the receiver is blocked) or an owner-accepted "unverified" label. The alternative of re-executing the syscall (rewinding RIP) is out, because it re-resolves the handle.
- **The deadline needs explicit bounds.** "Bounded" requires a maximum (for example a `JE_IPC_WAIT_MAX_TICKS` constant), an overflow-checked `ticks + duration`, and the inclusive comparison `wake <= ticks` that `je_thread_sleep` already uses, so the two clocks behave the same.

## Escalate to the owner (four items)
1. Whether deferred WOKEN copy-out, performed in the dispatch hook after the receiver's CR3 load, satisfies D7's "deliver". Also whether the exact-RIP fixup is approved for use inside `je_sched_interrupt`.
2. Confirm that priority is decided at one arbitration point and that PEER_CLOSED is eligible only on an empty queue, consistent with the merged drain-first behaviour.
3. CANCELLED in v1: internal helper with a unit test only, documented as unreachable from the ABI.
4. The remaining ABI details: deadline register, behaviour of a zero timeout, the maximum deadline, and the status for exceeding `JE_EP_MAX_WAITERS`.
[R2 CODEX] codex
Claude is right on several implementation constraints, but its proposal still leaves the central D7 arbitration underspecified.
Agree:
- Thread-owned waiter metadata minimizes `sched.c` churn, but it does not eliminate endpoint→scheduler wake coupling. Endpoint enqueue/close paths still need a bounded waiter scan or scheduler wake API.
- Peer death must actively wake blocked receivers at the endpoint close transition, not wait for their deadline.
- One outstanding head reservation per endpoint is required. Otherwise FIFO dequeue and copy-fault rollback become ambiguous.
- Deadline arithmetic needs an explicit bounded maximum, overflow checking, and the existing inclusive `wake <= ticks` rule.
- `CANCELLED` has no current ABI trigger. An internal cancellation helper plus a deterministic kernel test is reasonable, but its ABI unreachability must be documented.
Dispute:
1. The timer’s CAS cannot be justified merely by an invariant assertion.
A timer path that blindly performs `WAITING → TIMED_OUT` is correct only if every path that creates eligibility synchronously performs the competing transition first:
- enqueue: `WAITING → WOKEN`;
- peer close: `WAITING → PEER_CLOSED`;
- cancel: `WAITING → CANCELLED`;
- reservation release after copy fault: re-arbitrate and wake another waiter.
The copy-fault case Claude identified proves this invariant is easy to violate. “Assert or `JE_EP_CORRUPT`” is not an acceptable normal recovery path for a kernel mechanism. The design must specify one shared `je_ipc_wait_arbitrate()` helper, called under IF=0, and test that helper after reservation release.
The timer may then perform the final timeout CAS, but only after the same arbitration rules are applied or after the invariant is mechanically enforced. Otherwise the ISR can timeout a waiter while a queued message exists.
2. “ISR stamps timeout; syscall re-checks” is not the state-machine contract.
The terminal state must be selected by one CAS. A syscall re-check may validate the already-selected result and perform copy/commit, but it must not be a second authority that changes `TIMED_OUT` into `WOKEN`. If the syscall is allowed to override the ISR result, the transition is no longer a single terminal CAS.
Conversely, sender and close paths cannot merely enqueue or mark metadata. They must perform the `WAITING → WOKEN/PEER_CLOSED` transition in the same IF=0 critical section as the endpoint mutation.
3. The proposed deferred copy-out inside scheduler dispatch is the wrong default.
Copying after `cr3_load(next)` inside `je_sched_interrupt` introduces a new page-fault context in scheduler code. The provided facts only establish exact-RIP recovery for the M3a syscall path; they do not establish recovery for a fault originating inside the scheduler after an address-space switch. That is precisely the kind of untested assembly/frame interaction this owner gate is intended to prevent.
The safer shape is:
- wake path atomically selects the waiter result and, for WOKEN, reserves the head message;
- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
- the blocked syscall continuation resumes after park;
- syscall code performs owned-record copy-out and commit/release;
- copy faults return `JE_ABI_INVALID`, preserve the message, release the reservation, and re-arbitrate.
Do not rewind and re-execute the original handle-based receive; D3’s owned-reference rule forbids re-resolving the capability.
4. The priority interpretation is not already settled.
“PEER_CLOSED applies only when the queue is empty” is consistent with merged drain-first `try_recv` behavior, but it is not implied by the literal D7 priority `PEER_CLOSED > WOKEN`. If peer closure and a queued message coexist, two interpretations conflict:
- existing drain-first semantics: deliver the queued message;
- literal D7 priority: return `PEER_CLOSED`.
This is a frozen-contract/tested-code conflict and must remain an owner escalation, not be called merely a clarification.
Important omissions:
- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
- Process teardown must clear the waiter state, unlink it from endpoint arbitration, and release its endpoint reference before the thread/frame slot can be reused. Merely changing the thread to `DYING` is insufficient if endpoint code can later scan or wake the stale slot.
- A message enqueue must wake at most one waiter and reserve only the queue head. Peer closure with an empty queue may wake all eligible receivers, subject to the same CAS rule.
- `JE_EP_MAX_WAITERS == 2` and the ref-count assertion establish capacity, not the required ABI/error status when capacity is exhausted.
Recommended minimal `sched.c` change:
- Add `THREAD_BLOCKED_IPC`.
- Add per-thread waiter state, endpoint record/side identity, owned-reference marker, deadline, saved receive arguments, reservation marker, and terminal cause.
- Add helpers for:
  - park current receiver;
  - CAS terminal transition and make thread READY;
  - unlink/tombstone waiter during VM destruction.
- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
- Do not perform user copy-out from the scheduler or re-resolve the handle there.
Minimum deterministic proofs:
- queued message before receive: immediate WOKEN-equivalent, no block;
- receiver parks, sender enqueues, explicit yield: exactly one WOKEN and FIFO delivery;
- receiver parks, timer reaches inclusive deadline: ISR produces TIMED_OUT;
- receiver parks, peer endpoint closes: active PEER_CLOSED wake before deadline;
- copy-out fault after WOKEN: `JE_ABI_INVALID`, message remains queued, reservation released, next waiter can wake;
- internal cancellation helper: CANCELLED exactly once;
- teardown while blocked: waiter unlink/reference release precedes slot reuse;
- competing triggers: enqueue/close versus timeout, asserting one terminal CAS and the selected priority.
Escalate only these points:
1. Resolve the queued-message-plus-peer-close priority conflict.
2. Confirm that syscall-continuation copy-out is required, rejecting scheduler-dispatch copy-out unless exact-RIP recovery there is explicitly owner-approved and tested.
3. Set deadline-register encoding, zero-timeout behavior, maximum deadline, and waiter-capacity failure status.
4. Confirm whether CANCELLED is internal-test-only in v1 or exposed through a future cancellation contract.
Claude is right on several implementation constraints, but its proposal still leaves the central D7 arbitration underspecified.
Agree:
- Thread-owned waiter metadata minimizes `sched.c` churn, but it does not eliminate endpoint→scheduler wake coupling. Endpoint enqueue/close paths still need a bounded waiter scan or scheduler wake API.
- Peer death must actively wake blocked receivers at the endpoint close transition, not wait for their deadline.
- One outstanding head reservation per endpoint is required. Otherwise FIFO dequeue and copy-fault rollback become ambiguous.
- Deadline arithmetic needs an explicit bounded maximum, overflow checking, and the existing inclusive `wake <= ticks` rule.
- `CANCELLED` has no current ABI trigger. An internal cancellation helper plus a deterministic kernel test is reasonable, but its ABI unreachability must be documented.
Dispute:
1. The timer’s CAS cannot be justified merely by an invariant assertion.
A timer path that blindly performs `WAITING → TIMED_OUT` is correct only if every path that creates eligibility synchronously performs the competing transition first:
- enqueue: `WAITING → WOKEN`;
- peer close: `WAITING → PEER_CLOSED`;
- cancel: `WAITING → CANCELLED`;
- reservation release after copy fault: re-arbitrate and wake another waiter.
The copy-fault case Claude identified proves this invariant is easy to violate. “Assert or `JE_EP_CORRUPT`” is not an acceptable normal recovery path for a kernel mechanism. The design must specify one shared `je_ipc_wait_arbitrate()` helper, called under IF=0, and test that helper after reservation release.
The timer may then perform the final timeout CAS, but only after the same arbitration rules are applied or after the invariant is mechanically enforced. Otherwise the ISR can timeout a waiter while a queued message exists.
2. “ISR stamps timeout; syscall re-checks” is not the state-machine contract.
The terminal state must be selected by one CAS. A syscall re-check may validate the already-selected result and perform copy/commit, but it must not be a second authority that changes `TIMED_OUT` into `WOKEN`. If the syscall is allowed to override the ISR result, the transition is no longer a single terminal CAS.
Conversely, sender and close paths cannot merely enqueue or mark metadata. They must perform the `WAITING → WOKEN/PEER_CLOSED` transition in the same IF=0 critical section as the endpoint mutation.
3. The proposed deferred copy-out inside scheduler dispatch is the wrong default.
Copying after `cr3_load(next)` inside `je_sched_interrupt` introduces a new page-fault context in scheduler code. The provided facts only establish exact-RIP recovery for the M3a syscall path; they do not establish recovery for a fault originating inside the scheduler after an address-space switch. That is precisely the kind of untested assembly/frame interaction this owner gate is intended to prevent.
The safer shape is:
- wake path atomically selects the waiter result and, for WOKEN, reserves the head message;
- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
- the blocked syscall continuation resumes after park;
- syscall code performs owned-record copy-out and commit/release;
- copy faults return `JE_ABI_INVALID`, preserve the message, release the reservation, and re-arbitrate.
Do not rewind and re-execute the original handle-based receive; D3’s owned-reference rule forbids re-resolving the capability.
4. The priority interpretation is not already settled.
“PEER_CLOSED applies only when the queue is empty” is consistent with merged drain-first `try_recv` behavior, but it is not implied by the literal D7 priority `PEER_CLOSED > WOKEN`. If peer closure and a queued message coexist, two interpretations conflict:
- existing drain-first semantics: deliver the queued message;
- literal D7 priority: return `PEER_CLOSED`.
This is a frozen-contract/tested-code conflict and must remain an owner escalation, not be called merely a clarification.
Important omissions:
- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
- Process teardown must clear the waiter state, unlink it from endpoint arbitration, and release its endpoint reference before the thread/frame slot can be reused. Merely changing the thread to `DYING` is insufficient if endpoint code can later scan or wake the stale slot.
- A message enqueue must wake at most one waiter and reserve only the queue head. Peer closure with an empty queue may wake all eligible receivers, subject to the same CAS rule.
- `JE_EP_MAX_WAITERS == 2` and the ref-count assertion establish capacity, not the required ABI/error status when capacity is exhausted.
Recommended minimal `sched.c` change:
- Add `THREAD_BLOCKED_IPC`.
- Add per-thread waiter state, endpoint record/side identity, owned-reference marker, deadline, saved receive arguments, reservation marker, and terminal cause.
- Add helpers for:
  - park current receiver;
  - CAS terminal transition and make thread READY;
  - unlink/tombstone waiter during VM destruction.
- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
- Do not perform user copy-out from the scheduler or re-resolve the handle there.
Minimum deterministic proofs:
- queued message before receive: immediate WOKEN-equivalent, no block;
- receiver parks, sender enqueues, explicit yield: exactly one WOKEN and FIFO delivery;
- receiver parks, timer reaches inclusive deadline: ISR produces TIMED_OUT;
- receiver parks, peer endpoint closes: active PEER_CLOSED wake before deadline;
- copy-out fault after WOKEN: `JE_ABI_INVALID`, message remains queued, reservation released, next waiter can wake;
- internal cancellation helper: CANCELLED exactly once;
- teardown while blocked: waiter unlink/reference release precedes slot reuse;
- competing triggers: enqueue/close versus timeout, asserting one terminal CAS and the selected priority.
Escalate only these points:
1. Resolve the queued-message-plus-peer-close priority conflict.
2. Confirm that syscall-continuation copy-out is required, rejecting scheduler-dispatch copy-out unless exact-RIP recovery there is explicitly owner-approved and tested.
3. Set deadline-register encoding, zero-timeout behavior, maximum deadline, and waiter-capacity failure status.
4. Confirm whether CANCELLED is internal-test-only in v1 or exposed through a future cancellation contract.

---
## Amendment-text adversarial review (round 2 — 10 text defects, all fixed)

### Claude (review 1)
The text has 14 defects. Four would break an implementation outright: #1, #3, #5 and #6. I checked each one against what's on main: `docs/adr/0009` D2/D3/D7/A5, `include/jeios/abi.h`, `kernel/core/sched.c` and `kernel/core/syscall.c`.

### Blocking (the implementor would have to guess, or would build something wrong)

**1. The "parked syscall continuation" can't run on the current `sched.c`, and the text never defines it.**
- Offending text (A6-5): *"the user copy-out and commit happen in the resumed syscall continuation, never inside `je_sched_interrupt` after a `cr3_load`."*
- On main, a user thread can only be resumed through its saved CPL3 frame at `rsp0_top - sizeof(frame)`.
- `je_sched_interrupt` calls `abort_scheduler()` if a user thread presents any other frame. So a user thread cannot `int $0x81` from inside its syscall handler the way `je_thread_sleep` does for kernel threads.
- That leaves three ways to build it, and each one hits a rule:
  - Let a user thread hold a kernel-mode frame. That changes the tested ADR 0007 frame invariant.
  - Run the continuation at dispatch time. That is after `cr3_load`, which A6-5 forbids.
  - Rewind RIP and re-run `int 0x80`. A6-5 forbids re-running the handle-based recv.
- The text must choose a mechanism and name the `sched.c` invariant it changes, because that is the owner-gated surface.

**2. A copy fault after `WOKEN` contradicts the single-CAS rule (your #1 and #3).**
- Offending text: A6-5 *"releases the head reservation, and re-arbitrates"* versus A6-STRUCT *"the first eligible cause wins, later causes are no-ops … never rewrites the CAS result."*
- If "re-arbitrates" means this waiter goes `WOKEN→WAITING→…`, that is a second transition on a terminal waiter. It also breaks D7's *"if a message is already queued … it does not block"*, because the message is still queued.
- The proof bullet *"a next waiter can still wake"* implies a different meaning: the faulting waiter is finished (returns `JE_ABI_INVALID`) and a fresh CAS runs on a different WAITING waiter.
- Pin that meaning. State that a waiter is never re-armed, and that "re-arbitrate" means offering the released head to the next WAITING waiter on that end, or to nobody.

**3. The reservation lifecycle is underspecified, so a head can stay reserved forever (your #3).** Missing cases:
- (a) Under merged A5, the commit can also fail with `SHORT_BUFFER`, `QUOTA`, `NOSLOTS` or `REFOVERFLOW`. A6-5 only names the copy fault, so the text doesn't say whether those release-and-re-offer or return with the head still reserved.
- (b) Two messages arrive while two waiters are parked. D2 says *"non-exclusive receive … two processes may hold handles to the same end."* *"Reserve the queue head"* is singular, so it's unstated whether the second message wakes waiter B.
- (c) A non-blocking `try_recv` from another holder meets a reserved head. Does it skip it, steal it (leaving the continuation without its record), or get `EMPTY`?
- (d) The waiter is WOKEN and READY, but its process is torn down before the continuation runs. Nothing says teardown releases the reservation, so it's leaked and the channel is wedged.
- (e) The reservation needs a defined owner and owning-ref class. D3's list of owning refs is frozen and has no "reservation" entry.

**4. A close on a non-empty queue can stamp `PEER_CLOSED` and violate A6-1 (your #2).**
- A6-1 says *"`PEER_CLOSED` is returned only when the queue is empty,"* but the arbitrate side never requires that.
- Follow-on from 3(b): A is WOKEN with message 1 reserved; message 2 is queued and unreserved; B is still WAITING. Then the peer closes. The close path's *"peer close → PEER_CLOSED"* CAS stamps B, even though message 2 is deliverable.
- Rule needed: the close path arbitrates `PEER_CLOSED` only on waiters whose end has no unreserved queued message. Equivalently, enforce the invariant "WAITING ⇒ no unreserved message on that end," checked at every enqueue, release and close.
- The order of entry checks is also unpinned: queued → deliver; else `PEER_CLOSED` and empty → return 12 without parking; else zero-timeout poll; else park. The text never says a recv on an already `PEER_CLOSED`, empty end returns immediately.

**5. A6-PEERWAKE names the wrong end state.**
- Offending text: *"where that transitions a peer end to `CLOSED`."*
- Merged D2 says the closing end becomes `CLOSED` and the surviving peer end goes `OPEN → PEER_CLOSED`. The blocked receiver waits on the survivor's end, so this wording describes a transition that never happens.
- Also missing: D2 closes an end only *"when its handle-ref count reaches 0"*. If the dying process granted a dup to the survivor, its death closes nothing and nobody is woken. Say that.
- Also missing: an ordinary `endpoint_close` dropping the last handle (no process death) must also wake. A6-PEERWAKE covers only `je_vm_destroy`, while A6-STRUCT says "peer close" in general.

**6. Teardown can resurrect a dying thread (your #5).**
- Offending text: *"The waiter-unlink for the dying process's own blocked thread happens in the same IRQ-critical teardown, before the thread/frame slot is reused."*
- "Before slot reuse" is too late. The release walk visits both ends of every channel the dying process holds. If it holds both ends of a channel (allowed by transfer), closing one end makes the other `PEER_CLOSED` and arbitrates the process's own waiter.
- A6-5 says the wake does *"flips `THREAD_BLOCKED_IPC → READY`."* If that flip isn't guarded, a DYING thread can become READY.
- The text must require all three:
  1. Unlink or tombstone the dying process's own waiters before the release walk.
  2. Have arbitrate skip tombstoned waiters.
  3. Make the scheduler flip conditional: `BLOCKED_IPC → READY` only, never from DYING or ZOMBIE.

**7. The ABI mapping for the four terminal causes is missing (your #7).**
- `WOKEN` → presumably `JE_ABI_OK` plus the A5 RDX layout, but not stated.
- `PEER_CLOSED` → 12, implied but not stated.
- `TIMED_OUT` → no status exists. Codes 10–13 are taken, and the text mints nothing. It needs a new code (e.g. `JE_ABI_H_TIMED_OUT = 14` with a `_Static_assert`), or an explicit statement that it reuses `EMPTY`.
- `CANCELLED` → unmapped. "ABI-unreachable" doesn't remove the need for a code; the continuation still writes something into RDX/status.
- A6-4 *"returns `EMPTY`-equivalent"*: "-equivalent" is undefined. Is it literally 13?

**8. The timeout register and opcode are unspecified, and the obvious choice changes merged behavior.**
- Merged `JE_SYS_TRY_RECV` already uses RDI, RSI, RDX, R8 and R9. R10 is "reserved = 0".
- If blocking recv reuses `try_recv` with the timeout in R10, every existing caller passes R10=0. Under A6-3 *"Zero timeout = a non-blocking poll … immediate `TIMED_OUT`,"* those callers would start getting `TIMED_OUT` instead of `EMPTY` (13).
- Either pin a new op (`JE_SYS_RECV`), or say zero-timeout returns `EMPTY` so it stays identical to `try_recv`. That second option also matches A6-1's "behave identically to try_recv" argument.

### Significant

**9. "First eligible wins" contradicts D7's fixed priority, and the matching proof can't be built.**
- Offending text: *"competing triggers: enqueue vs timeout in the tie window → assert the single CAS picks the A6-1 priority."*
- Each cause runs its own CAS in its own IF=0 critical section on one CPU, so two causes are never eligible at the same moment. The earlier critical section always wins.
- Example: the ISR stamps `TIMED_OUT` at tick T, then a sender enqueues before X is dispatched. X returns `TIMED_OUT` and the message stays queued. That ignores D7's `WOKEN > TIMED_OUT`, and it's fine — but the text must say that on a single CPU the priority applies only to causes found in the same critical section, and list which ones (probably only the recv entry checks).
- A6-1 also isn't a trigger priority at all. Rewrite the proof as "the first critical section wins; the later one is a no-op; the message is not lost."

**10. A6-5 conflicts with frozen D7 text that A6 claims not to reopen.**
- D7: the ISR *"must do the `WAITING→TIMED_OUT` transition **and deliver the result**, not merely mark the thread READY."*
- A6-5: *"The scheduler only flips `THREAD_BLOCKED_IPC → READY`"* and the continuation writes the result.
- These can be reconciled (the CAS stamp is the result; the continuation only transcribes it). But the amendment says it *"does not reopen those,"* so it must state that reconciliation explicitly, or flag it as a D7 clarification for the owner.

**11. Zero versus one tick (your #4) is mostly fine, but the short-circuit isn't pinned.**
- "Never parks" implies the zero check happens before the waiter is registered. Say it outright: duration 0 never registers a waiter or computes `wake`, so the ISR rule never applies.
- On main, `je_thread_sleep(0)` still yields (`int $0x81`). The text should say a zero-timeout recv does not yield, or the poll stops matching `try_recv`.
- A 1-tick wait gets `wake = ticks+1` and fires on the next `++je_timer_ticks`, which can be after any fraction of a tick. That's distinguishable from 0, but document that one tick means "up to one tick," with no minimum.
- A6-3 mixes *"requested deadline"* with *"duration"*. Pin it as a relative duration, as the overflow check implies.

**12. The `JE_EP_MAX_WAITERS` scope is ambiguous, and "structurally unreachable" ignores non-exclusive receive.**
- Offending text: *"one blocked receiver per process → 2."* It doesn't say whether that is per end, per channel, or global.
- With a transferred dup, both processes can block on the same end. That case is reachable and has to be handled, not just asserted away. It also feeds 3(b).
- The cap must stay correct whichever scope is chosen.

**13. The "teardown while blocked" proof has no trigger in v1.**
- A blocked process can't fault or exit, and there's no kill mechanism in M3c. So a process can't be destroyed while blocked without an internal helper, just as with `CANCELLED`.
- Name that helper. Otherwise the proof is either fabricated or claims an ABI path that doesn't exist.
- The PEER_CLOSED proof's wake latency is "until the bootstrap reaper runs `je_vm_destroy`," not "at exit." The deterministic test must step the reaper.

### Minor

**14. Terms are used without definitions.**
- "reservation", "re-arbitrate", "continuation", "waiter slot" and "tombstone" (D7 says "remove or tombstone" without choosing) are never defined.
- `je_ipc_wait_arbitrate(waiter, cause)` doesn't say whether it returns a won/lost flag. The callers need it: enqueue must know whether it reserved the head, and teardown must know whether it owes a release.

**Sections with no defects of their own:** A6-2, apart from the missing code in #7, and A6-1's decision rationale. The A6-1 hole is in how the arbitrate and close paths enforce it (#4), not in the decision.

### Codex (review 1)
codex
I’ll review the amendment against the repository’s current M3c contracts and scheduler/endpoint documentation, then report only contract defects or explicitly clean sections. I won’t edit code or documents.
codex
The workspace shell is being blocked by the sandbox’s network-namespace setup before the commands run, so I’m retrying the same read-only inspection with the required elevated execution path.
exec
/usr/bin/bash -lc "git status --short --branch && git log -1 --format='%H %s'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "pwd && rg --files -g 'README.md' -g 'docs/HANDOFF.md' -g 'docs/ROADMAP.md' -g 'docs/DESIGN.md' -g 'docs/AGENT_START.md' -g 'docs/CODING_STANDARD.md' -g 'docs/SECURITY.md' -g '*adr*' -g '*ADR*' | sort" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
## main...origin/main
 M docs/adr/0000-decision-record-policy.md
 M docs/adr/0009-m3c-capability-ipc.md
?? docs/adr/debates/
e914a952658276937cf2c59b812c94b0b3e65ff4 Merge pull request #45 from auto44e/feat/m3c-cap-transfer-impl

 succeeded in 0ms:
/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
/usr/bin/bash: line 1: rg: command not found

exec
/usr/bin/bash -lc "sed -n '1,240p' README.md; sed -n '1,280p' docs/HANDOFF.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,280p' docs/ROADMAP.md; sed -n '1,320p' docs/AGENT_START.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,360p' docs/DESIGN.md; sed -n '1,300p' docs/CODING_STANDARD.md; sed -n '1,260p' docs/SECURITY.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "find . -maxdepth 4 -type f | sort | sed -n '1,260p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
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
Architectural descriptions of new features do not establish that current retail
hardware exposes them. Research-only targets stay research-only in the support
matrix until execution tests exist.

### 6.3 Heterogeneous scheduling

A scheduler can exploit performance/efficiency cores, cache topology, NUMA,
vector capabilities, thermal state, and power budgets only when the platform can
report them reliably. Initially use straightforward per-CPU queues and explicit
classes rather than inventing a complex algorithm before correctness.

Migration must respect each task's required instruction and state feature set.
Global portable exposure may be the intersection across online CPUs; opt-in
specialized tasks may use restricted affinity. CPU hotplug cannot silently strand
such tasks or move them to an incompatible CPU. Expose latency-oriented versus
throughput-oriented intent with quotas and admission control, not unlimited
realtime priority. Device IRQ affinity and DMA locality are part of placement.

## 7. Memory and virtual address spaces

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
# Security and privacy design requirements

**This remains an experimental kernel, not a hardened OS.** M1/M2 provides
read-only kernel text/rodata, NX data on qualified CPUs, guarded kernel stacks,
validated boot input and bounded allocation/rollback. Kernel faults stop the
machine with diagnostics. All threads run at CPL0 in one address space.
There is no userspace authority boundary, secure boot, encryption, randomness
service or networking. Early bootstrap maps are temporarily RWX until replaced.
The following threat model remains a requirement for future implementation.

## Assets, adversaries, and boundaries

Protect user content/credentials, application isolation, device ownership, update
integrity, service availability, and privacy of local diagnostics. Consider
hostile applications, malformed inputs, buggy drivers, malicious peripherals,
stolen devices, compromised package sources, and accidental misconfiguration.
No promise is made against all compromised firmware/hardware, invasive physical
attacks, every side channel, or unrestricted authorized privileged code.

| Boundary | Planned enforcement | Failure behavior |
| --- | --- | --- |
| App -> kernel | Checked syscall data, typed generation handles, rights and quotas | Denied/invalid without side effects |
| App -> broker | Scoped capability transfer, request bounds, broker identity | Reject/revoke with explicit status |
| Driver -> memory | Process boundary plus qualified IOMMU/IRQ routing | Disable/unavailable if required containment cannot hold |
| Service -> service | Explicit channels, rights attenuation, generation binding | Old requests cannot reach restarted instance |
| Installed image -> boot | Owner trust roots, signed images, recovery/rollback policy | Refuse unauthorized image under locked policy |
| Theme/widget -> system | Data themes; isolated code; trustworthy consent UI | Revert broken customization, retain authorized apps where possible |
| Diagnostic -> export | Local preview, redaction, user-initiated transfer | No automatic external report |

## Required defaults

No mandatory cloud login. No advertising ID or default telemetry. Network access
is policy-controlled and independently revocable. Files, microphone/camera,
location/sensors, clipboard, screen capture and raw devices are mediated. Log
retention is bounded and sensitive fields are redacted by default. Stock apps
use public broker interfaces rather than broad ambient privilege.

Installed executable identity must bind package provenance and sandbox identity.
Grant changes do not apply invisibly to a different package or restarted service.
For network revocation define existing socket teardown, DNS/proxy mediation,
child processes, shared services, IPv6, and VPN reconnect behavior. Encrypting DNS
or routing through a VPN is not a promise of anonymity.

## C hardening work

Add validated user-copy and trap containment, W^X where possible, guarded stacks,
stack protection, hardened allocation, zero-before-share, checked arithmetic,
resource accounting, and reliable entropy. PAC/BTI/CET/MTE are additive mitigations
with architecture prerequisites. Tagging detects some faults; it cannot replace
rights checks. Optional CPU instructions must not leak into baseline execution.
Fuzz untrusted parsers and fault-inject allocation/timeout/partial initialization.
Never recover arbitrary kernel corruption by simply returning an error.

## Security profiles are not difficulty presets

Keep compatibility tier, boot trust state, policy guarantees, and UI complexity
separate. An expert UI does not authorize privileged actions. Legacy hardware
may run but cannot claim guarantees such as NX-enforced data non-execution or
IOMMU-enforced device containment. Development trust permits deliberately changed
code and must show which boot/module guarantees are lost. User freedom and honest
security status coexist; neither requires pretending unsigned kernel code is safe.

## Boot, updates, and encryption

A future owner-configurable trust root is required; no hidden universal vendor
key. Verified boot, anti-rollback counters, A/B images, signed manifests, and
recovery must be coordinated. A known-good image can still be vulnerable; signed
recovery policy must distinguish authorized rollback from downgrade attacks.
Private signing keys never enter the repo. The GRUB prototype has no trust-chain enforcement.

Use vetted cryptography. Design key derivation/storage/unlock, memory lifetime,
physical access, recovery and backup before adding an 'encrypted' UI label. Mobile
bootloader relocking and secure key storage depend on the specific device; broad
Android support is not inherited from GrapheneOS or CalyxOS ideas.

## Inspiration and provenance

GrapheneOS motivates attack-surface reduction, memory hardening, scoped access,
and carefully limited security exceptions. CalyxOS motivates approachable privacy
controls and application network policy. These are reference designs, not code
imports, endorsements, or equivalent implementations. See REFERENCES.md.

## Vulnerability handling

Before production use, establish a tested private disclosure channel, triage
owners, patch distribution, supported-version policy, and a recovery process.
Do not use a public issue to post sensitive exploit details or credentials. No
private disclosure address is configured by this scaffold; do not invent one.

## Detailed encryption requirements

See [ENCRYPTION.md](ENCRYPTION.md) for the RAM threat model, PQ key agreement
and authentication requirements, key lifecycle and nearby sharing. Filesystem
checksums do not provide cryptographic authentication; encrypted storage does
not keep plaintext secret from an authorized live-memory reader.

## Identity, management and raw storage authority

See [IDENTITY.md](IDENTITY.md) for account/device/session separation, E2E group
membership, recovery and explicit family/enterprise visibility. Administering a
service does not automatically make its operator a personal-data key recipient.
Management of a privileged OS nevertheless changes endpoint trust and must be
visible at enrollment. Offline revocation and remote wipe are not instantaneous.

[STORAGE.md](STORAGE.md) requires separately authorized range-bound raw I/O,
exclusive write leases and no automatic formatting of unknown media. Ordinary
file access does not authorize bypassing filesystem or encryption boundaries.

## Accelerator and media boundaries (planned)

[ACCELERATORS.md](ACCELERATORS.md) requires scoped device/buffer/fence authority,
qualified DMA isolation, bounded submissions and parser/compiler workers, and
explicit reset/quiescence before memory reuse. Offload policy cannot expand
permissions. No silent precision downgrade or cloud transfer. Unsupported
required containment fails closed. Codecs/SDKs remain userspace components;
no GPU/NPU/media isolation is implemented by this document.

## Ecosystem-wide zero trust — owner requirement 2026-10-14

[ZERO_TRUST.md](ZERO_TRUST.md) defines explicit per-resource authority, least
privilege, identity provenance, revocation, offline rules and cross-device gates.
This applies throughout the OS and ecosystem. It adds no current enforcement;
M3a isolation remains the next implementation dependency.

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
./.git/objects/00/1b7b67927bb66ab7fd1607c7f5eda5a081cbf6
./.git/objects/00/1d2d2ef5a3766cb89220e0b98ae9676c8fe064
./.git/objects/00/6dfb8934cba3eacab8b397773ad3c3a6587deb
./.git/objects/03/49f54b4a77c1f4a64726d3bfcba52924ae3021
./.git/objects/03/85a9cf5255a69d567b7bd2a2c3f84f5f878013
./.git/objects/04/43e3cb35791b46a49b48892519113ca604e108
./.git/objects/04/a15eaa5c8817589c555bf366139a83cbba7e5f
./.git/objects/04/d9c4d28c85c2818f8f5f65f6175d9212969fda
./.git/objects/05/2982094ec4f0c5ae7333873cf19e2e2e603af2
./.git/objects/05/d1b863fc8c3460cc74f3bc5706b69dea7c1712
./.git/objects/06/1bb31f00d1afa9123658f97a45760f2d2e17f9
./.git/objects/07/6872c5c336da485f597c9a6693463f8cfd990b
./.git/objects/07/7b8cc467521a084538270a853c166d29b45dc2
./.git/objects/07/9699e05f5987cf1aede13a1b2fce00f09ec745
./.git/objects/07/d02f6ab677666156645523b81bb7cb174405a8
./.git/objects/08/174141a9c098c7b9aa01fc1e7b9d1618d32a1f
./.git/objects/08/5c1f89328668b81c1be4396240fc843026338b
./.git/objects/08/5f6955384bcdf9f0421e31c9de6c38f7c08e1c
./.git/objects/08/734b6201ea7281d2f94c32f5220df5db10a7a3
./.git/objects/09/76235ebedeca819d28c6f62a513d45cbbb3e82
./.git/objects/0a/23f84b44197ec2d0f63790a4a7ceb5dadb4b0e
./.git/objects/0a/e8998e0b59f78588cded8c1aff7a6020e478a2
./.git/objects/0b/579875901c3f91fedde88b68045e3466129fc6
./.git/objects/0b/a3c20503d670f0983db8138b3e4884d85c2fdc
./.git/objects/0b/be9345ca10778b2b717972c3fd4c29a81fb71a
./.git/objects/0b/fcbac7051218b7fb89fb9f5c4f8aa2ffb04a35
./.git/objects/0c/37ab298d5bae474e4cf5f98e4fdcb19f9a4a4c
./.git/objects/0c/8c43049080dac10ded138c28fdae4392ecb149
./.git/objects/0c/cf4a11aed81051483a3a61cad24ed6ed012fd0
./.git/objects/0c/fda3aebc92fd4ba9db9cb04a03fa85d9edd362
./.git/objects/0d/18502765b8f0f582fbd5348a50d8a0f1f78595
./.git/objects/0e/25150d51b530a295345a41ba6a04d9b53415f7
./.git/objects/0e/405f1d4d27ce65cb4ed0b93dfe0a0f768ea1c5
./.git/objects/0f/a222bbb330fa5d03d09a8a7c213011c0ca38ab
./.git/objects/0f/b5cb74590f18db236ac7640bba5b944210576b
./.git/objects/0f/bec2f795881346df13ba99228b883f48b95b85
./.git/objects/0f/f7c10a7cbc1aa0ba17fd669f86a87b1651ea95
./.git/objects/11/375f19b1b69e2b6a48a71d707df1229d88e805
./.git/objects/11/775a9a01371deddade445dde63e8cee1382b21
./.git/objects/11/ca2c5bb7634edcdab38f98505686ef36a5bee1
./.git/objects/12/40f1b69cb9a9f8a02d7f698ff1276213e51baa
./.git/objects/12/d2c774ea18e52c62ad104b09a846059129ce2b
./.git/objects/13/034a34e7c9f14217fecb936c8b9a48cd1825df
./.git/objects/13/3108f11deedc150395e7c2f4e3da7ec81d0c67
./.git/objects/13/b274f3e8292e26406d62d519ed9be390db029e
./.git/objects/13/d273167b904041eb6da479951b6ec88ea74871
./.git/objects/14/498612ecbede93fa18669102533dc4fb0ea177
./.git/objects/14/9b7e13d16fc3fd5377468f161e6ccb076c8e6c
./.git/objects/14/aaeb514a2e05e9c67c438406336bc1020b9d86
./.git/objects/15/5944b89741d5e4f38145e5b7593a9494fe6173
./.git/objects/16/173284efdb646a54c52dea2fb06ff0700a1ffa
./.git/objects/16/318b4c261875899ff2236fe595364cc3b96385
./.git/objects/16/b2022b16eb800e3a5dc8d8f61f3d54f9ad5326
./.git/objects/16/c2352bebd79d0ad186f89a8bf0a1c2502145f7
./.git/objects/17/9df50030aeef7c3888faf365b734789d78eb4a
./.git/objects/17/de5c9bdeb3fe872793858a51c4fd67e80b142f
./.git/objects/17/ea51602116b92a80513484d3b4276270506031
./.git/objects/18/6bd3287a1b59efffd3389317c7e4ade178266b
./.git/objects/1a/1739353950ad6422f5e63bc7495e56c70efc44
./.git/objects/1a/67bf2957dde9cff708e54a079dcb0fefcf8321
./.git/objects/1b/4b51a755ef111ab394ecfecaa5b2a412f63fff
./.git/objects/1b/9bd12c15e3615bcacddb34f556fe94722cc202
./.git/objects/1b/d37a299f2df7abc883c7ecdd93ed40999c4ff0
./.git/objects/1c/c1686a916085651c7bfdd65941f1ceb1c170b9
./.git/objects/1d/032ae49862193497a89039d5ee03d7c39cfb61
./.git/objects/1d/6e6c4051091686f1196907a2916e3f03fb9315
./.git/objects/1d/d21da85f3f5d368b95b2211c097ec0180628e4
./.git/objects/1e/6ab4d26c2d1577aeebe82a49de95782d115919
./.git/objects/1e/b7ce959919d4a1095f6e907ec1b8a40b183b27
./.git/objects/1f/18d74e254a78bc524ddb520870c49721cc60d3
./.git/objects/20/8559abfde156add46b480f65fc6349f3098454
./.git/objects/21/bcdac1306c55c23b1ba9d44fcd6891dc739ce9
./.git/objects/21/bf7c9eee88531d2c2d8da241405c008a70455a
./.git/objects/22/53d26885d5df04fc896899da4f31a5bf7df4a8
./.git/objects/22/5886aa58590abc893de89a513750566987736d
./.git/objects/22/bb78ebbd686a0b88cbb3bd3dd6eefea6c63be8
./.git/objects/22/c3437574bf03809589c4ac06ff04d8789c4a64
./.git/objects/22/f8b2f9e879fb0fcbf0fdf097ef477b18927c8e
./.git/objects/23/834681dbc7d4cb256942ebcef94dd2a4646095
./.git/objects/23/a0e6a694ca0d7e45c0624e3c371c79cbb2fb7d
./.git/objects/24/f0183e85f6780fcc02923760b450a657725138
./.git/objects/25/0d9a03f2cbdeb119cb4861bfec310fb919234a
./.git/objects/25/2ee154daf1ed33603bd01391d13b1353208312
./.git/objects/25/79bd8f1764d938478af58ee5d4a7ff32bb603d
./.git/objects/25/c2a71f53ed5430634e1441574b36f593b64152
./.git/objects/25/df9e7e505fae33ffd4a60bfe02e55d357d1a82
./.git/objects/26/aaf5648a41534e8165d6160726ddf4db24b3b9
./.git/objects/26/cf22d3fed6fa4012ac442fbf5ebb4efca8062b
./.git/objects/28/3604d0ea518e4fa85f7e826ee5a2f40ad0417f
./.git/objects/28/570feced2606cb0e281f9fa05c6d513e70013d
./.git/objects/2a/3659e98b72467a2ee0b4fb1c5e790a9abda97f
./.git/objects/2a/4b0aafd4984bad984888ec1230db8410da9520
./.git/objects/2a/96c0fea1c0355e6eea0db8ec52a9031b044924
./.git/objects/2b/0f91d8a5c6ef35a9ece3f076e3c05e3013a33b
./.git/objects/2b/5172853270f2e9bd9b6285dfb5f8b88ec72752
./.git/objects/2b/5a463788ae247c0e611e6c8568c05fb4cc33ad
./.git/objects/2c/4a90d6228b378385573d22e3822e49bf3d9b72
./.git/objects/2c/811236f0c84c5f1998ca9b3666c7160df2752d
./.git/objects/2c/8d9461e4d075bb1a90d9c694f8895b25a76b63
./.git/objects/2c/ad6473beab983cae8b8061ad67bb577cc354fb
./.git/objects/2e/7f192de40b737f6340a848dde50677ee8bc083
./.git/objects/2f/c2d54a409bda897dd469ff941d1f1ca82dd4ab
./.git/objects/31/0142df334cf7dbce17a42a64df32d31ff5a2f1
./.git/objects/31/af8bbb49c8240780d77a3aff57c271e429f48e
./.git/objects/32/9db29abf003990ddd3f09bd18369ee74d398b4
./.git/objects/32/cd3e2b6ff1557d77d8a56063406c48bcdc7c82
./.git/objects/33/a753d1ec6dbb5eddd8f6ea907df4d2b75a088e
./.git/objects/33/df733c3e600daa549686e55e948d3c7b28b381
./.git/objects/34/afe1a0ca39f14b0cd0e68af7cf166f1bdb72ed
./.git/objects/35/712df53bfa1c917b93b30d1b2b9e525dc5c6cd
./.git/objects/36/3f27238f7bb2a88c77b1532eb581dc9982eef8
./.git/objects/36/8718a1aa250996a42e1351d36146444c1ec4f8
./.git/objects/36/b40a476f5101411e85ff2a29dbfb73cdd90d3e
./.git/objects/37/1a4cabf438198a540635a544b9b4296fbd5f90
./.git/objects/37/1f5d9b62ab2acaad50a4b7ffffa7cb7c2e2783
./.git/objects/37/f82f5e63ce36c19eb82030e256d2872cf16618
./.git/objects/38/01bb53d4e6f31ad60dbc2563725f39a4491291
./.git/objects/38/3298a93b712dbefcc153298ed70fa0a38afb5a
./.git/objects/39/42e1c50e7cd4050a7c062186c6828920b46793
./.git/objects/3a/4b0f68fe744ea9aef50de18ec12e424a293dc5
./.git/objects/3b/45a43469092cb4fde061c67cee7d5573be0d75
./.git/objects/3b/5ecd410a99933f1b02cc418fb529a5ea870034
./.git/objects/3e/aafdd456bec4547a640fa150d4b2bc9d636f9e
./.git/objects/3f/869520b8caf0bb17602390c9606d3c4163ca85
./.git/objects/41/0084c19c52904e16c07bcd03422acfeee44b4f
./.git/objects/41/69f166ae4903f7081a2c29f5172218c4c84a11
./.git/objects/42/3ff8a5fc9813a0565081cec32c994897951f4c
./.git/objects/42/5ab4ad5d1a6bfa876517664a54233810fb46b2
./.git/objects/42/65d640f1e0dce68c1991ac1034e73a0d12e43b
./.git/objects/42/bc6fcbcbbcce86a979c79ad9aac125231374d1
./.git/objects/42/d543c9391a92fa3000dee2518031ee0670fd41
./.git/objects/43/074f1a546a1c5213b13027facb699bbca96134
./.git/objects/43/643b5e8ce09ee83bad524a53340b80d14cbc64
./.git/objects/43/c934e8e581e9bf956bb47af4f410674bcef2d9
./.git/objects/44/34b6890165080d2a686172a03fd8e84ec7fbd2
./.git/objects/44/a88244e3e196836c36e1f046691715d4ca8f47
./.git/objects/45/58d61084fed25a13ff2fa4e7dd20ce52e1b792
./.git/objects/45/94180e3e4836f9bfa9f8a41f2dc356095eede0
./.git/objects/47/8d2d21bb5d9780004e1295db8a33bffe522760
./.git/objects/47/91c4301761ef0729e58a13426ddc40f87a05bd
./.git/objects/47/da5c62328405cf2b41f928dcf8c40b608d1fb1
./.git/objects/47/e72b144e046c729f0902a5f84246c32d9ab289
./.git/objects/48/a3f0eef055defc2c4d6e9b2249e3fa59ed53cf
./.git/objects/49/576d4632713236af63f2edb1c3029447a52fd0
./.git/objects/49/8a49474301d5e299c3ca0c72a6742f2bfb7243
./.git/objects/4a/d5d3d2ded875874347586cb9cf390faa648cc0
./.git/objects/4b/26cae089b4a8f3274973cf23fe21002b3a31f7
./.git/objects/4b/33aa2cee1899a995bbe3cad2c71711e9e9d9b5
./.git/objects/4b/931f02f4d7ef296adcd9378e8c8a104bf5acf3
./.git/objects/4b/9e350696f0922f600e2e0058c9e2c3bb3bc5af
./.git/objects/4b/ae8e159db7da2f3ce932d1a0a0f6272cd3e90b
./.git/objects/4b/b50844d2d5888a47f94dc645a6df51874ad78b
./.git/objects/4c/035284fb57ebd3f40c81e61f48fd91a378982c
./.git/objects/4c/461505254b5804bc078df47268657ebeec6d05
./.git/objects/4c/bc8d90064cdc7394e7a94622bfdf5f51bb098f
./.git/objects/4d/15ffbff2020fcdf2e8d27dc59be9722a0be3cf
./.git/objects/4e/380096e2068e83fe9de246e2e931b44c20f96d
./.git/objects/4f/b73ba086260712add74f73b3659368f7efb747
./.git/objects/4f/b81ac2edfc7bb491b416fc819e2d52b2e2ff10
./.git/objects/50/2932df4117591f35170ac177abb34ffdcd9aa0
./.git/objects/50/6b6c613f6feb1636c9e1aea4d9696f671fd925
./.git/objects/51/4471e3a0bf000f228a376a70d319452a00c672
./.git/objects/52/1d38b7e13d3b680c4e0fec0c5f8a016cfdfa63
./.git/objects/52/6a7e0b27f4495df1bc10cbc187bdd22211a9b5
./.git/objects/53/7b4334f6c4c844e5374a8620c14a3d8ccd0403
./.git/objects/53/b0e11dec8585d829c1aab110972e7c22dfd703
./.git/objects/53/d15832413ab5c314afde0ebeab2c955c69c473
./.git/objects/54/3cdfccb8ba44e2eda5dc717a6827ab99dade2b
./.git/objects/54/9d2404ddf40692003f536d9fa095852cce8d9c
./.git/objects/54/cfa2a2fa47a4eedd4ec6f3b1b7d83cda783567
./.git/objects/55/ad261f46b621e7a6844351e5a4e1243940c099
./.git/objects/56/0b09d63603ce3d4d5263e6836c944b4305f36a
./.git/objects/56/1a09c6dbf248b64d50f410738c422769daf93f
./.git/objects/56/4903ccd9b22f192a0b76e96a1ead6dd865b577
./.git/objects/58/0eac5819dab2f0912a1e6a6ee10d1adfcb1049
./.git/objects/58/12b7949b746b7febb284c882421631fd063aea
./.git/objects/59/7fc68f231cfe63d1f60bf4df18d63ddc02df45
./.git/objects/5b/7a3f2c19ddc4474c4ceae52b76a6db339802b0
./.git/objects/5c/0eb9dc3443da94a43c41d966afd1021a6ea0ee
./.git/objects/5c/0f177e0bdb656d4c29a1b96a661c85dc55dd14
./.git/objects/5c/79dd34ea8cda14be5462f06fde832f627dc8c7
./.git/objects/5c/9222c8938c5728acdfc0b2b2b37019c89169ec
./.git/objects/5d/1e43c0f94dad688ce3a5cffb8de335c89da893
./.git/objects/5d/3f26412031e5309c0484a55a6a685ff09ebf64
./.git/objects/5d/77d9f8169b8c15bf84e74642d607aa1ec5908a
./.git/objects/5f/13b317bc6306086edb1a597f95fdec6e4a1add
./.git/objects/61/35cab70ca0c968fe3738af7894b072c8ca8ba7
./.git/objects/61/5bd63ca2e2720b116aa644767e61936dbbfa7c
./.git/objects/62/82ab3273ff8101bd38e5bcd384c2058fe56bc5
./.git/objects/63/3dd99a6956a893d3ffffb86f2601ee9092271d
./.git/objects/64/e78551a46740e3c83f6f5d286729946d299aec
./.git/objects/65/7f980beda2c84892a6ceacbdc3983105cf72c3
./.git/objects/67/d3a0f682765e36940a63cab5424e0ffd103ac2
./.git/objects/68/4db9f8640b4cdab100211ecd5ba1d5de50b424
./.git/objects/68/dfba315bc2f3bfd3a29f462b9e881cea45c032
./.git/objects/69/994c55518f941d7c12fa28d0d8ddf12c9970c7
./.git/objects/69/b2b2390370421799d4ee375ce126008ef798c3
./.git/objects/69/d05a126e38fc59c38f3f411cb7a759d36bd8fb
./.git/objects/6a/143ae073ee0d0d62e68fa511f83b9d69fdfde6
./.git/objects/6a/5b0f0c3337c1b94b930f893ddabce786c0e4a6
./.git/objects/6a/c5f3e9eed29fc659126d19399ae7c0be130f1f
./.git/objects/6b/8338ef59d74fe0d4346ed96222b62d5adbd1e3
./.git/objects/6c/27b85e9f59e49677fe916b57efced8e224766d
./.git/objects/6c/c2b8b8aa1f809f02455a2be6d61929542945d9
./.git/objects/6c/ebcbcc496f9e1fa706722c406237220de68a13
./.git/objects/6d/c1cdf1b90aedd8a6c66e69fc022fa9981dcdb4
./.git/objects/6e/90feb4de4c2a2712ec10e6124c55c7df264018
./.git/objects/6e/fe557a3c05b02dda1b30db15be30667cb52497
./.git/objects/6f/592107affa3d42152e1cef7d11646618c63593
./.git/objects/70/1ac9396570bd08b6b588c5ea74fd4043442363
./.git/objects/70/1ff99f4babffbb87285308ec37d76349785552
./.git/objects/70/4d15627193b6db8101b1b3177aee3ff9284503
./.git/objects/70/6504cd32c0f4f7f5bf10c93e56f33db18cd42a
./.git/objects/70/faaad21f7dc04d5e546e03ad9e4ff368a7cbbe
./.git/objects/72/0111335cdade650dd9db4329ef7174a348913b
./.git/objects/72/028cd25c10c6fe3e6b200b4b3d122ce07ff180
./.git/objects/72/4aec9d2aa1695ffc6bcfdf07d68ac876d11028
./.git/objects/72/faeabaac989ab7179fa1395649e4dd134b7f4a
./.git/objects/73/cefa1129fa939ba39fbc9fe2ed88608edfc68e
./.git/objects/73/e47c0abc9bd001c2594822175045e96b7c1495
./.git/objects/74/98ad2bd25e47dd557f1e9ab69e8db1618a5bec
./.git/objects/74/bf5a81d898f9d76a8e4e0a6042a5e9fb06c85b
./.git/objects/74/caf40371123be36a220182e8b5c1b17d402ef0
./.git/objects/74/d54a7a44e9eab04960975c9b13d0a1ae3e3999
./.git/objects/74/d85cb47233f65de6d158edaf501647d0f76900
./.git/objects/74/faebfccd3b8255172f822ace2dae4428944f2d
./.git/objects/75/06a86cae44ca1e25440dfd4f61f26da2531ecd
./.git/objects/75/163a61e6c23ab994266e395208029d2ee98c7d
./.git/objects/75/1bca256657ba55947af8debf5aa0a9412e3ec2
./.git/objects/75/7dfaf818cb1aec9250ed5527a8aa735e51a7a4
./.git/objects/75/f1c99c2d8fabf0a6a3025067286332d9610e86
./.git/objects/76/0770bf15a4bd9d53166e3457492d44ad0521e1
./.git/objects/76/b8e69867f9daaf863ed9a3ec7d6e1c0b84fe99
./.git/objects/77/9fa1f1b0a2b8512f1ad15f9115092a910e7246
./.git/objects/77/c7c123fe61bcfbc5d084b954b0b72b398829d0
./.git/objects/78/bf52613275f30ca880eb33450fd2c0dac2d335
./.git/objects/78/f82ec2f9e2dd8e97d04fb5dc3cf8cbf627bce6

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

codex
The repository is on `main` at `e914a95`, with uncommitted ADR edits already present; I’ll treat those as user-owned and read them for context only. The baseline documents confirm that M3c step 4 drain-first behavior and the step-3 IRQ-critical teardown are the authoritative merged contracts.
exec
/usr/bin/bash -lc "find kernel include tests docs/adr -type f -not -path '*/.git/*' | sort | sed -n '1,320p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -RInE \"try_recv|PEER_CLOSED|JE_ABI_INVALID|QUEUE_FULL|SHORT_BUFFER|EMPTY|endpoint|destroy|sleep|wake|SLEEPING|je_timer_ticks|JE_VM_MAX_PROCESSES|JE_THREAD_MAX\" kernel include tests docs/adr/0009-m3c-capability-ipc.md docs/IMPLEMENTATION.md 2>/dev/null | sed -n '1,420p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,420p' docs/adr/0009-m3c-capability-ipc.md; find docs/adr/debates -type f -maxdepth 2 -print -exec sed -n '1,260p' {} \\;" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,360p' kernel/core/sched.c; sed -n '1,300p' kernel/core/sched.h 2>/dev/null; sed -n '1,420p' kernel/core/endpoint.c; sed -n '1,320p' kernel/core/endpoint.h" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
docs/adr/0000-decision-record-policy.md
docs/adr/0001-fresh-c-kernel.md
docs/adr/0002-modularity-and-isolation.md
docs/adr/0003-baseline-and-negotiated-features.md
docs/adr/0004-user-experience-contract.md
docs/adr/0005-licensing.md
docs/adr/0006-bsp-foundation-and-page-heap.md
docs/adr/0007-m3a-userspace-boundary.md
docs/adr/0008-m3b-typed-handles.md
docs/adr/0009-m3c-capability-ipc.md
docs/adr/debates/README.md
docs/adr/debates/m3c-step6-waits.md
include/jeios/abi.h
include/jeios/alloc.h
include/jeios/arch.h
include/jeios/boot.h
include/jeios/endpoint.h
include/jeios/endpoint_kern.h
include/jeios/features.h
include/jeios/handle.h
include/jeios/kheap.h
include/jeios/module.h
include/jeios/paging.h
include/jeios/sched.h
include/jeios/status.h
include/jeios/syscall.h
include/jeios/trap.h
include/jeios/uframe.h
include/jeios/usercopy.h
include/jeios/vm.h
kernel/arch/aarch64/boot.S
kernel/arch/aarch64/linker.ld
kernel/arch/aarch64/main.c
kernel/arch/x86_64/boot.S
kernel/arch/x86_64/linker.ld
kernel/arch/x86_64/paging.c
kernel/arch/x86_64/platform.c
kernel/arch/x86_64/register_probe.S
kernel/arch/x86_64/trap.c
kernel/arch/x86_64/trap_entry.S
kernel/arch/x86_64/user_demo.S
kernel/arch/x86_64/usercopy.S
kernel/arch/x86_64/vm_backend.c
kernel/core/alloc.c
kernel/core/boot.c
kernel/core/endpoint.c
kernel/core/endpoint_kern.c
kernel/core/features.c
kernel/core/handle.c
kernel/core/kheap.c
kernel/core/main.c
kernel/core/memory.c
kernel/core/module.c
kernel/core/sched.c
kernel/core/status.c
kernel/core/syscall.c
kernel/core/uframe.c
kernel/core/usercopy.c
kernel/core/vm.c
tests/test_alloc.c
tests/test_boot.c
tests/test_core.c
tests/test_endpoint.c
tests/test_handle.c
tests/test_syscall.c
tests/test_uframe.c
tests/test_vm.c
tests/vm_fake.c

 succeeded in 0ms:
kernel/arch/x86_64/user_demo.S:446:/* ---- M3c s3: endpoint create + typed close + fault-death ------------------
kernel/arch/x86_64/user_demo.S:447: * endpoint_create (op 10) -> RDX packs (h1<<32)|h0. Store the pair, close h0 via
kernel/arch/x86_64/user_demo.S:448: * HANDLE_CLOSE (op 6, which must route through the typed endpoint close and keep
kernel/arch/x86_64/user_demo.S:460:    /* endpoint_create(): all args zero. */
kernel/arch/x86_64/user_demo.S:484: * endpoint_create gives h0 (RDX[31:0]) and h1 (RDX[63:32]). A send on h0 lands in
kernel/arch/x86_64/user_demo.S:486: * copy-out rollback / SHORT_BUFFER / QUEUE_FULL / PEER_CLOSED. It does NOT prove
kernel/arch/x86_64/user_demo.S:517:    /* recv(h0) -> EMPTY (direction: nothing landed in h0's inbound) */
kernel/arch/x86_64/user_demo.S:524:    movq %rax, 0x40001390      /* expect JE_ABI_H_EMPTY (13) */
kernel/arch/x86_64/user_demo.S:525:    /* recv(h1, cap=1) -> SHORT_BUFFER, RDX=8 */
kernel/arch/x86_64/user_demo.S:532:    movq %rax, 0x40001398      /* expect SHORT_BUFFER (11) */
kernel/arch/x86_64/user_demo.S:553:    /* recv(h1) again -> EMPTY (delivered exactly once) */
kernel/arch/x86_64/user_demo.S:560:    movq %rax, 0x400013c8      /* expect EMPTY (13) */
kernel/arch/x86_64/user_demo.S:569:    /* recv(h1) -> EMPTY (bad send enqueued nothing) */
kernel/arch/x86_64/user_demo.S:576:    movq %rax, 0x400013d8      /* expect EMPTY (13) */
kernel/arch/x86_64/user_demo.S:577:    /* fill the queue: 4 sends OK, 5th QUEUE_FULL */
kernel/arch/x86_64/user_demo.S:589:    movq $11, %rax             /* 5th send -> QUEUE_FULL */
kernel/arch/x86_64/user_demo.S:595:    movq %rax, 0x400013e8      /* expect QUEUE_FULL (10) */
kernel/arch/x86_64/user_demo.S:605:    /* close h0; now h1's peer is gone. Drain the 4 queued, then PEER_CLOSED. */
kernel/arch/x86_64/user_demo.S:623:    /* now empty + peer gone -> PEER_CLOSED */
kernel/arch/x86_64/user_demo.S:630:    movq %rax, 0x40001408      /* expect PEER_CLOSED (12) */
kernel/arch/x86_64/user_demo.S:631:    /* send(h1) -> peer (h0 end) closed -> PEER_CLOSED */
kernel/arch/x86_64/user_demo.S:638:    movq %rax, 0x40001410      /* expect PEER_CLOSED (12) */
kernel/arch/x86_64/register_probe.S:58:    cmpq $64,je_timer_ticks(%rip)
kernel/arch/x86_64/vm_backend.c:59:    vb_proc    procs[JE_VM_MAX_PROCESSES];
kernel/arch/x86_64/vm_backend.c:60:    vb_extent  ext[JE_VM_MAX_PROCESSES * JE_VM_MAX_EXTENTS];
kernel/arch/x86_64/vm_backend.c:91:    if (!out_extent || process >= JE_VM_MAX_PROCESSES || pages == 0) return JE_ERR_INVALID;
kernel/arch/x86_64/vm_backend.c:123:    if (!out_root || process >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
kernel/arch/x86_64/vm_backend.c:149:    if (frame.process >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
kernel/arch/x86_64/vm_backend.c:186:    if (process >= JE_VM_MAX_PROCESSES || varena_off >= JE_VM_ARENA_SIZE || (varena_off & 4095))
kernel/arch/x86_64/vm_backend.c:225:    if (process >= JE_VM_MAX_PROCESSES) { vb_panic(ctx, "drop_tables range"); return; }
kernel/arch/x86_64/vm_backend.c:243:    if (process >= JE_VM_MAX_PROCESSES || !out_phys || !out_perm || !out_present ||
kernel/arch/x86_64/vm_backend.c:266:    if (process >= JE_VM_MAX_PROCESSES || !out_flags || index >= 512) return JE_ERR_INVALID;
kernel/arch/x86_64/vm_backend.c:333:    for (uint32_t i = 0; i < JE_VM_MAX_PROCESSES; ++i) g_vb.procs[i].table_extent = UINT32_MAX;
kernel/arch/x86_64/paging.c:57:     * would silently rewrite it RW and destroy the "no writable alias survives"
kernel/arch/x86_64/paging.c:62:     * destroy "no writable alias survives" (ADR 0007 D3). M3a #4 step 2. Mask off
kernel/core/endpoint.c:3: * M3c sub-PR 2 (ADR 0009 D-SEQ step 2): endpoint/channel lifecycle core.
kernel/core/endpoint.c:4: * Hostable, no kernel state, no ABI include — see endpoint.h for the full
kernel/core/endpoint.c:8:#include <jeios/endpoint.h>
kernel/core/endpoint.c:56:void je_endpoint_pool_init(je_endpoint_pool *pool)
kernel/core/endpoint.c:74:uint32_t je_endpoint_pool_allocated(const je_endpoint_pool *pool)
kernel/core/endpoint.c:79:je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side)
kernel/core/endpoint.c:85:/* The opaque object pointer a handle slot binds for an endpoint is &rec->end[s].
kernel/core/endpoint.c:88:static je_ep_status object_to_end(const je_endpoint_pool *pool, const void *obj,
kernel/core/endpoint.c:107:static void maybe_free_record(je_endpoint_pool *pool, je_ep_channel *rec)
kernel/core/endpoint.c:155:je_ep_status je_endpoint_resolve(const je_endpoint_pool *pool,
kernel/core/endpoint.c:180: * for a valid live ref). On reaching 0: this end CLOSED, a live peer -> PEER_CLOSED,
kernel/core/endpoint.c:186:static void release_capdesc_inflight(je_endpoint_pool *pool,
kernel/core/endpoint.c:198:static void release_handle(je_endpoint_pool *pool, je_ep_channel *rec, uint8_t side)
kernel/core/endpoint.c:208:        if (peer->state == JE_EP_END_OPEN) peer->state = JE_EP_END_PEER_CLOSED;
kernel/core/endpoint.c:235:static bool rec_in_pool(const je_endpoint_pool *pool, const je_ep_channel *rec)
kernel/core/endpoint.c:244:je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
kernel/core/endpoint.c:261:je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
kernel/core/endpoint.c:289:je_ep_status je_endpoint_channel_create(je_endpoint_pool *pool,
kernel/core/endpoint.c:300:    /* Reject rights with any unknown endpoint bit (defense in depth; the glue
kernel/core/endpoint.c:356:je_ep_status je_endpoint_handle_close(je_endpoint_pool *pool,
kernel/core/endpoint.c:361:    je_ep_status s = je_endpoint_resolve(pool, t, h, &rec, &side);
kernel/core/endpoint.c:371:/* ---- dup seam (retain-before-commit; endpoints never use dup_self) -------- */
kernel/core/endpoint.c:373:je_ep_status je_endpoint_handle_dup(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:380:    /* Resolve src as an endpoint and read its rights, preserving the table's
kernel/core/endpoint.c:384:    je_ep_status rs = je_endpoint_resolve(pool, t, src, &rec, &side);
kernel/core/endpoint.c:414:int je_endpoint_release_table(je_endpoint_pool *pool, je_handle_table *t)
kernel/core/endpoint.c:425:        je_ep_status s = je_endpoint_handle_close(pool, t, h);
kernel/core/endpoint.c:429:    /* Post-condition: no endpoint slot may remain LIVE. */
kernel/core/endpoint.c:433:            return -1;   /* a LIVE endpoint survived the walk -> fatal */
kernel/core/endpoint.c:440:uint8_t je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side)
kernel/core/endpoint.c:446:je_ep_status je_endpoint_send_preflight(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:454:    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
kernel/core/endpoint.c:462:     * end is peer-closed, nobody will ever read it -> PEER_CLOSED. */
kernel/core/endpoint.c:463:    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
kernel/core/endpoint.c:464:        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
kernel/core/endpoint.c:466:        return JE_EP_QUEUE_FULL;
kernel/core/endpoint.c:470:/* Send preflight WITHOUT the QUEUE_FULL check: main-handle resolve + SEND right +
kernel/core/endpoint.c:471: * self-end PEER_CLOSED only. The glue runs this first, then resolves/validates the
kernel/core/endpoint.c:472: * caps (so per-cap DENIED/STALE outrank QUEUE_FULL per A5-4's frozen precedence),
kernel/core/endpoint.c:473: * and the final pure try_send_caps enforces QUEUE_FULL atomically at enqueue. */
kernel/core/endpoint.c:474:je_ep_status je_endpoint_send_preflight_main(je_endpoint_pool *pool,
kernel/core/endpoint.c:481:    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
kernel/core/endpoint.c:487:    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
kernel/core/endpoint.c:488:        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
kernel/core/endpoint.c:492:je_ep_status je_endpoint_try_send(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:497:    return je_endpoint_try_send_caps(pool, t, h, kbuf, len, (const je_ep_capdesc *)0, 0u);
kernel/core/endpoint.c:500:je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:509:    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
kernel/core/endpoint.c:518:        /* Empty: EOF if the peer is gone, else would-block EMPTY. */
kernel/core/endpoint.c:519:        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
kernel/core/endpoint.c:520:               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
kernel/core/endpoint.c:523:    *out_len = mlen;                 /* report length even on SHORT_BUFFER */
kernel/core/endpoint.c:524:    if (cap < mlen) return JE_EP_SHORT_BUFFER;   /* no copy, no dequeue */
kernel/core/endpoint.c:538:je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
kernel/core/endpoint.c:544:    return je_endpoint_recv_commit_caps(pool, lease);
kernel/core/endpoint.c:549:bool je_endpoint_capdesc_live(const je_endpoint_pool *pool, const je_ep_capdesc *d)
kernel/core/endpoint.c:555:    /* OPEN and PEER_CLOSED are LIVE (mintable); only CLOSED/FREE are dead (A5-2). */
kernel/core/endpoint.c:556:    return st == JE_EP_END_OPEN || st == JE_EP_END_PEER_CLOSED;
kernel/core/endpoint.c:559:je_ep_status je_endpoint_capdesc_resolve(je_endpoint_pool *pool,
kernel/core/endpoint.c:564:    if (!je_endpoint_capdesc_live(pool, d)) return JE_EP_STALE;
kernel/core/endpoint.c:570:je_ep_status je_endpoint_capdesc_retain_handle(je_endpoint_pool *pool,
kernel/core/endpoint.c:574:    je_ep_status s = je_endpoint_capdesc_resolve(pool, d, &tr, &ts);
kernel/core/endpoint.c:579:void je_endpoint_capdesc_release_handle(je_endpoint_pool *pool,
kernel/core/endpoint.c:583:    if (je_endpoint_capdesc_resolve(pool, d, &tr, &ts) != JE_EP_OK) return;
kernel/core/endpoint.c:592:static je_ep_status caps_validate(je_endpoint_pool *pool, const je_ep_capdesc *caps,
kernel/core/endpoint.c:598:        je_ep_status s = je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts);
kernel/core/endpoint.c:600:        /* Descriptor rights must contain only known endpoint bits (defence in
kernel/core/endpoint.c:609:je_ep_status je_endpoint_send_preflight_caps(je_endpoint_pool *pool,
kernel/core/endpoint.c:614:    je_ep_status base = je_endpoint_send_preflight(pool, t, h, len);
kernel/core/endpoint.c:621:je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:630:    je_ep_status pf = je_endpoint_send_preflight_caps(pool, t, h, len, caps, ncaps);
kernel/core/endpoint.c:632:    (void)je_endpoint_resolve(pool, t, h, &rec, &side);   /* cannot fail now */
kernel/core/endpoint.c:639:        if (je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts) != JE_EP_OK) {
kernel/core/endpoint.c:644:                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
kernel/core/endpoint.c:652:                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
kernel/core/endpoint.c:673:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:684:    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
kernel/core/endpoint.c:693:        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
kernel/core/endpoint.c:694:               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
kernel/core/endpoint.c:700:    /* Short EITHER buffer -> SHORT_BUFFER, copy nothing, no dequeue (report both
kernel/core/endpoint.c:702:    if (cap < mlen || hcap < mcaps) return JE_EP_SHORT_BUFFER;
kernel/core/endpoint.c:714:je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
kernel/core/vm.c:274:    uint32_t idx = JE_VM_MAX_PROCESSES;
kernel/core/vm.c:275:    for (uint32_t i = 0; i < JE_VM_MAX_PROCESSES; ++i)
kernel/core/vm.c:277:    if (idx == JE_VM_MAX_PROCESSES) return JE_ERR_BUSY;
kernel/core/vm.c:382:/* ---- destroy -------------------------------------------------------------- */
kernel/core/vm.c:384:je_status je_vm_destroy(je_vm *vm, uint32_t index)
kernel/core/vm.c:386:    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
kernel/core/vm.c:389:     * ONE critical section. The direct main.c destroy callers may run with IF=1,
kernel/core/vm.c:394:     * (the unit test) keeps the prior behaviour. The teardown hook (the endpoint
kernel/core/vm.c:408:    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
kernel/core/vm.c:490:            for (uint32_t q = 0; q < JE_VM_MAX_PROCESSES; ++q) {
kernel/core/syscall.c:22:#include <jeios/endpoint.h>
kernel/core/syscall.c:23:#include <jeios/endpoint_kern.h>
kernel/core/syscall.c:40:        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:43:        *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
kernel/core/syscall.c:45:        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:49:        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:55:        if (r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:60:        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:66:        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:67:        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:72:        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:73:        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:79:        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:80:        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:88:        if (rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:89:        if ((rdi >> 32) || (rsi >> 32)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:90:        if (rsi & ~(uint64_t)JE_ABI_RIGHTS_KNOWN) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:93:        /* endpoint_create(): takes NO args; every register reserved. Mints both
kernel/core/syscall.c:95:        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:102:        if (r10 >> 8) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; } /* reserved bits */
kernel/core/syscall.c:103:        if (r9)       { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:104:        if ((r10 & 0xffu) > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:106:        if (((r10 & 0xffu) == 0u) != (r8 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:107:        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:108:        if (rdx > (uint64_t)JE_ABI_MSG_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:111:        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8, hcap=R9, R10 reserved=0).
kernel/core/syscall.c:115:        if (r10) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:116:        if (r9 > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:117:        if ((r8 == 0u) != (r9 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:118:        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:124:        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
kernel/core/syscall.c:161:    return JE_ABI_INVALID;   /* unreachable; fail closed if the enum ever grows */
kernel/core/syscall.c:166: * if the caller is not a user thread (the glue maps that to JE_ABI_INVALID). */
kernel/core/syscall.c:174:/* Resolve the current user process as (vm,index) for the endpoint ops, which
kernel/core/syscall.c:183: * it is 32 bits. The endpoint_create two-handle return depends on this. */
kernel/core/syscall.c:184:_Static_assert(sizeof(je_handle) == 4u, "endpoint_create packs two handles in RDX");
kernel/core/syscall.c:185:/* The table-side endpoint type/rights copies must equal the ABI ones the glue
kernel/core/syscall.c:187:_Static_assert(JE_ENDPOINT_OBJ_TYPE == JE_ABI_OBJ_ENDPOINT, "endpoint type: table vs ABI");
kernel/core/syscall.c:192:               "endpoint rights: table vs ABI");
kernel/core/syscall.c:193:_Static_assert(JE_EP_MSG_MAX == JE_ABI_MSG_MAX, "endpoint msg max: table vs ABI");
kernel/core/syscall.c:194:_Static_assert(JE_EP_MSG_CAPS_MAX == JE_ABI_MSG_CAPS_MAX, "endpoint caps max: table vs ABI");
kernel/core/syscall.c:196:/* Map endpoint-layer status to the v0 ABI (reuses the JE_ABI_H_* namespace; the
kernel/core/syscall.c:197: * glue is the only place endpoint.h and abi.h meet). */
kernel/core/syscall.c:209:    case JE_EP_QUEUE_FULL:  return JE_ABI_H_QUEUE_FULL;
kernel/core/syscall.c:210:    case JE_EP_SHORT_BUFFER: return JE_ABI_H_SHORT_BUFFER;
kernel/core/syscall.c:211:    case JE_EP_PEER_CLOSED: return JE_ABI_H_PEER_CLOSED;
kernel/core/syscall.c:212:    case JE_EP_EMPTY:       return JE_ABI_H_EMPTY;
kernel/core/syscall.c:213:    case JE_EP_INVALID:     return JE_ABI_INVALID;
kernel/core/syscall.c:216:    return JE_ABI_INVALID;
kernel/core/syscall.c:234:         * no recursion, no M2 sleep (D10). */
kernel/core/syscall.c:253:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:263:         * EXACTLY — SELF -> raw table close; ENDPOINT -> endpoint-aware close
kernel/core/syscall.c:266:         * CPU (SMP revisit). A raw je_handle_close on an endpoint slot would leak
kernel/core/syscall.c:269:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:279:            je_ep_status s = je_endpoint_kern_close(vm, index, h);
kernel/core/syscall.c:301:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:316:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:334:        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:344:        /* endpoint_create() (A3-2): mint both ends into the caller's own table
kernel/core/syscall.c:346:         * teardown hook installed (UNSUPPORTED) so a created endpoint is always
kernel/core/syscall.c:351:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:353:        je_ep_status s = je_endpoint_kern_create(vm, index, &h0, &h1);
kernel/core/syscall.c:366:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:376:        je_ep_status bpf = je_endpoint_kern_send_preflight(vm, index, h, (size_t)len);
kernel/core/syscall.c:382:            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:386:                if (crights & ~(uint32_t)JE_ABI_RIGHTS_KNOWN) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:387:                je_ep_status rc = je_endpoint_kern_resolve_send_cap(vm, index, ch, crights, &caps[c]);
kernel/core/syscall.c:394:            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:396:        je_ep_status s = je_endpoint_kern_send_caps(vm, index, h, staging,
kernel/core/syscall.c:408:        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
kernel/core/syscall.c:415:        je_ep_status s = je_endpoint_kern_recv_peek_caps(vm, index, h, staging,
kernel/core/syscall.c:419:        if (s == JE_EP_SHORT_BUFFER) {
kernel/core/syscall.c:420:            f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
kernel/core/syscall.c:433:            if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c] = 0u; continue; }
kernel/core/syscall.c:435:            je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
kernel/core/syscall.c:441:                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
kernel/core/syscall.c:449:                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
kernel/core/syscall.c:450:                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
kernel/core/syscall.c:461:                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
kernel/core/syscall.c:462:                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
kernel/core/syscall.c:466:        je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
kernel/core/endpoint_kern.c:3: * M3c sub-PR 3 (ADR 0009 D-SEQ step 3): the kernel's single endpoint pool and
kernel/core/endpoint_kern.c:5: * that holds endpoint global state; endpoint.c stays pure/hostable. Lives in the
kernel/core/endpoint_kern.c:9:#include <jeios/endpoint_kern.h>
kernel/core/endpoint_kern.c:12:/* The one kernel-wide pool. A dying process's endpoints are reclaimed into it by
kernel/core/endpoint_kern.c:15:static je_endpoint_pool g_kep;
kernel/core/endpoint_kern.c:20:    if (!g_kep_inited) { je_endpoint_pool_init(&g_kep); g_kep_inited = true; }
kernel/core/endpoint_kern.c:29:    je_endpoint_pool *pool = (je_endpoint_pool *)ctx;
kernel/core/endpoint_kern.c:30:    if (je_endpoint_release_table(pool, t) < 0) {
kernel/core/endpoint_kern.c:31:        je_arch_console_write("JeiOS fatal: endpoint release walk failed "
kernel/core/endpoint_kern.c:37:void je_endpoint_kern_install(je_vm *vm)
kernel/core/endpoint_kern.c:47:je_ep_status je_endpoint_kern_create(je_vm *vm, uint32_t index,
kernel/core/endpoint_kern.c:52:    if (!vm || index >= JE_VM_MAX_PROCESSES || !out0 || !out1)
kernel/core/endpoint_kern.c:54:    /* STRUCTURAL GUARANTEE (A3-2): refuse to mint an endpoint into a table whose
kernel/core/endpoint_kern.c:64:    return je_endpoint_channel_create(&g_kep, t, JE_EP_RIGHTS_ALL,
kernel/core/endpoint_kern.c:68:je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h)
kernel/core/endpoint_kern.c:70:    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:72:    return je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
kernel/core/endpoint_kern.c:78:je_ep_status je_endpoint_kern_send(je_vm *vm, uint32_t index, je_handle h,
kernel/core/endpoint_kern.c:81:    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:83:    return je_endpoint_try_send(&g_kep, &vm->procs[index].htable, h, kbuf, len);
kernel/core/endpoint_kern.c:86:je_ep_status je_endpoint_kern_send_preflight(je_vm *vm, uint32_t index,
kernel/core/endpoint_kern.c:89:    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:91:    /* Main-handle preflight only (no QUEUE_FULL) so per-cap errors outrank
kernel/core/endpoint_kern.c:92:     * QUEUE_FULL in the cap-send glue (A5-4). The final try_send_caps re-checks
kernel/core/endpoint_kern.c:93:     * QUEUE_FULL atomically at enqueue. */
kernel/core/endpoint_kern.c:94:    return je_endpoint_send_preflight_main(&g_kep, &vm->procs[index].htable, h, len);
kernel/core/endpoint_kern.c:97:je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
kernel/core/endpoint_kern.c:101:    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:103:    return je_endpoint_recv_peek(&g_kep, &vm->procs[index].htable, h,
kernel/core/endpoint_kern.c:107:je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease)
kernel/core/endpoint_kern.c:110:    return je_endpoint_recv_commit_caps(&g_kep, lease);
kernel/core/endpoint_kern.c:115:je_ep_status je_endpoint_kern_send_caps(je_vm *vm, uint32_t index, je_handle h,
kernel/core/endpoint_kern.c:119:    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:121:    return je_endpoint_try_send_caps(&g_kep, &vm->procs[index].htable, h,
kernel/core/endpoint_kern.c:125:je_ep_status je_endpoint_kern_resolve_send_cap(je_vm *vm, uint32_t index,
kernel/core/endpoint_kern.c:129:    if (!vm || index >= JE_VM_MAX_PROCESSES || !out) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:132:    /* The handle must be an endpoint holding TRANSFER. */
kernel/core/endpoint_kern.c:134:    je_ep_status rs = je_endpoint_resolve(&g_kep, tbl, h, &rec, &side);
kernel/core/endpoint_kern.c:162:je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
kernel/core/endpoint_kern.c:169:    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:171:    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
kernel/core/endpoint_kern.c:176:je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
kernel/core/endpoint_kern.c:180:    if (!vm || index >= JE_VM_MAX_PROCESSES || !d || !out) return JE_EP_BADINDEX;
kernel/core/endpoint_kern.c:184:    je_ep_status s = je_endpoint_capdesc_resolve(&g_kep, d, &tr, &ts);
kernel/core/endpoint_kern.c:188:    je_ep_status rr = je_endpoint_capdesc_retain_handle(&g_kep, d);
kernel/core/endpoint_kern.c:194:        je_endpoint_capdesc_release_handle(&g_kep, d);   /* undo retain */
kernel/core/endpoint_kern.c:205:bool je_endpoint_kern_capdesc_live(const je_ep_capdesc *d)
kernel/core/endpoint_kern.c:207:    return je_endpoint_capdesc_live(&g_kep, d);
kernel/core/endpoint_kern.c:210:void je_endpoint_kern_capdesc_release_handle(const je_ep_capdesc *d)
kernel/core/endpoint_kern.c:212:    je_endpoint_capdesc_release_handle(&g_kep, d);
kernel/core/endpoint_kern.c:215:je_ep_status je_endpoint_kern_seed(je_vm *vm, uint32_t idxA, uint32_t idxB,
kernel/core/endpoint_kern.c:220:    if (!vm || idxA >= JE_VM_MAX_PROCESSES || idxB >= JE_VM_MAX_PROCESSES
kernel/core/endpoint_kern.c:228:    return je_endpoint_channel_create(&g_kep, &vm->procs[idxA].htable,
kernel/core/endpoint_kern.c:234:uint32_t je_endpoint_kernel_allocated(void)
kernel/core/endpoint_kern.c:236:    return g_kep_inited ? je_endpoint_pool_allocated(&g_kep) : 0u;
kernel/core/handle.c:154:     * SELF (owns nothing, never destroyed). A destroyable type reaching here
kernel/core/handle.c:157:     * for destroyable objects; it must NOT reuse this function. */
kernel/core/main.c:12:#include <jeios/endpoint.h>
kernel/core/main.c:13:#include <jeios/endpoint_kern.h>
kernel/core/main.c:33:    if (je_thread_sleep(UINT64_MAX)!=JE_ERR_INVALID) fail("JeiOS fatal: sleep overflow\r\n");
kernel/core/main.c:34:    if (je_thread_sleep(0)!=JE_OK) fail("JeiOS fatal: yield\r\n");
kernel/core/main.c:36:    if (je_thread_sleep(3+id)!=JE_OK || je_sched_ticks()-before<3+id)
kernel/core/main.c:37:        fail("JeiOS fatal: early wake\r\n");
kernel/core/main.c:107:/* Initialize the shared step-3/4 vm once (it holds up to JE_VM_MAX_PROCESSES
kernel/core/main.c:114:    /* M3c (A3-1): install the endpoint teardown hook + the IRQ critical-section
kernel/core/main.c:115:     * pair so je_vm_destroy reclaims any endpoint this vm's processes hold and
kernel/core/main.c:117:     * s3 endpoint fault-death test (below) relies on this being installed. */
kernel/core/main.c:118:    je_endpoint_kern_install(&vm_s3);
kernel/core/main.c:248:    if (je_thread_sleep(1)!=JE_ERR_DENIED) fail("JeiOS fatal: idle slept\r\n");
kernel/core/main.c:249:    for (uintptr_t i=1;i<JE_THREAD_MAX;++i)
kernel/core/main.c:258:        if (atomic_load_explicit(&completed,memory_order_acquire)==JE_THREAD_MAX-1 && je_sched_finished()) break;
kernel/core/main.c:261:    if (je_sched_preemptions()<JE_THREAD_MAX*2 || je_sched_min_dispatches()<3)
kernel/core/main.c:263:    je_arch_console_write("M2: preemption/GPR/sleep/heap checks passed; ticks=");
kernel/core/main.c:309:        if (je_vm_destroy(&vm_s2, vm_idx)!=JE_OK) fail("JeiOS fatal: s2 destroy\r\n");
kernel/core/main.c:311:        je_arch_console_write("M3a-s2: process destroyed; page accounting restored\r\n");
kernel/core/main.c:361:            if (je_sched_user_cpl3_dispatches()-before_disp>=JE_THREAD_MAX*2) break;
kernel/core/main.c:369:        if (je_vm_destroy(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 destroy\r\n");
kernel/core/main.c:466:            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,S)-sdisp0<JE_THREAD_MAX*2; ) {
kernel/core/main.c:478:            if (je_vm_destroy(&vm_s3, S)!=JE_OK) fail("JeiOS fatal: s5a syscall destroy\r\n");
kernel/core/main.c:488:            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,U)-udisp0<JE_THREAD_MAX*2; ) {
kernel/core/main.c:497:            if (je_vm_destroy(&vm_s3, U)!=JE_OK) fail("JeiOS fatal: s5a unknown destroy\r\n");
kernel/core/main.c:536:            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,C)-cdisp0<JE_THREAD_MAX*3; ) {
kernel/core/main.c:544:            if (vm_s3_scratch(C,0x1030)!=JE_ABI_INVALID) fail("JeiOS fatal: s5b null src not INVALID\r\n");
kernel/core/main.c:545:            if (vm_s3_scratch(C,0x1038)!=JE_ABI_INVALID) fail("JeiOS fatal: s5b kernel src not INVALID\r\n");
kernel/core/main.c:546:            if (vm_s3_scratch(C,0x1040)!=JE_ABI_INVALID) fail("JeiOS fatal: s5b len>256 not INVALID\r\n");
kernel/core/main.c:560:            if (je_vm_destroy(&vm_s3, C)!=JE_OK) fail("JeiOS fatal: s5b copyprobe destroy\r\n");
kernel/core/main.c:573:            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,I)-idisp0<JE_THREAD_MAX*2; ) {
kernel/core/main.c:581:            if (je_vm_destroy(&vm_s3, I)!=JE_OK) fail("JeiOS fatal: s5b inject destroy\r\n");
kernel/core/main.c:599:            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,H)-hdisp0<JE_THREAD_MAX*3; ) {
kernel/core/main.c:629:            if (vm_s3_scratch(H,0x10e0)!=JE_ABI_INVALID)   fail("JeiOS fatal: s5c dup unknown-right not INVALID\r\n");
kernel/core/main.c:652:            if (vm_s3_scratch(H,0x1070)!=JE_ABI_INVALID)   fail("JeiOS fatal: s5c wide-handle not INVALID\r\n");
kernel/core/main.c:660:            if (je_vm_destroy(&vm_s3, H)!=JE_OK) fail("JeiOS fatal: s5c handle destroy\r\n");
kernel/core/main.c:664:        /* --- M3c s3: endpoint lifecycle wired to ring 3 + process-death reclaim.
kernel/core/main.c:667:         *       table holds end0, the OTHER holds end1; destroying the holder of
kernel/core/main.c:668:         *       end0 must leave end1 PEER_CLOSED and the record still allocated
kernel/core/main.c:672:         *   (B) a RING-3 fault-death loop: a process calls endpoint_create, closes
kernel/core/main.c:691:            static je_endpoint_pool sp;
kernel/core/main.c:692:            je_endpoint_pool_init(&sp);
kernel/core/main.c:694:            if (je_endpoint_channel_create(&sp, &ta, JE_EP_RIGHTS_ALL,
kernel/core/main.c:697:            if (je_endpoint_pool_allocated(&sp)!=1u) fail("JeiOS fatal: s3 survivor not 1\r\n");
kernel/core/main.c:698:            /* Holder of end0 (table ta) 'dies': release its endpoints. */
kernel/core/main.c:699:            if (je_endpoint_release_table(&sp, &ta)!=1) fail("JeiOS fatal: s3 survivor walk!=1\r\n");
kernel/core/main.c:701:             * PEER_CLOSED. */
kernel/core/main.c:702:            if (je_endpoint_pool_allocated(&sp)!=1u) fail("JeiOS fatal: s3 survivor freed early\r\n");
kernel/core/main.c:704:            if (je_endpoint_resolve(&sp, &tb, eb, &rec, &side)!=JE_EP_OK) fail("JeiOS fatal: s3 survivor resolve\r\n");
kernel/core/main.c:705:            if (je_endpoint_end_state(rec, side)!=JE_EP_END_PEER_CLOSED) fail("JeiOS fatal: s3 survivor not PEER_CLOSED\r\n");
kernel/core/main.c:707:            if (je_endpoint_release_table(&sp, &tb)!=1) fail("JeiOS fatal: s3 survivor close\r\n");
kernel/core/main.c:708:            if (je_endpoint_pool_allocated(&sp)!=0u) fail("JeiOS fatal: s3 survivor not reclaimed\r\n");
kernel/core/main.c:709:            je_arch_console_write("M3c-s3a: survivor-peer PEER_CLOSED; record held by survivor; reclaimed on its close (kernel self-test)\r\n");
kernel/core/main.c:711:            /* (B) ring-3 endpoint fault-death loop. Pool must start and end at 0,
kernel/core/main.c:713:            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s3 kpool not 0 at start\r\n");
kernel/core/main.c:718:                 * endpoint, closes one end, then reads an unmapped page -> #PF. */
kernel/core/main.c:738:                if (je_endpoint_kernel_allocated()!=1u)   fail("JeiOS fatal: s3 ep pool not 1 at fault\r\n");
kernel/core/main.c:747:                 * critical section in je_vm_destroy) must drop the surviving h1
kernel/core/main.c:752:                if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s3 ep pool leak after reap\r\n");
kernel/core/main.c:754:            je_arch_console_write("M3c-s3b: ring-3 endpoint_create + typed close; fault-death reap reclaims the channel; pool baseline held across >POOL_CAP cycles\r\n");
kernel/core/main.c:757:        /* --- M3c s4: non-blocking data IPC (try_send/try_recv), ring-3
kernel/core/main.c:759:         * direction/copy-out rollback/SHORT_BUFFER/QUEUE_FULL/PEER_CLOSED/rights.
kernel/core/main.c:762:            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s4 kpool not 0\r\n");
kernel/core/main.c:768:            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,P)-pd0<JE_THREAD_MAX*4; ) {
kernel/core/main.c:777:            if (vm_s3_scratch(P,0x1390)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 recv(h0) not EMPTY (direction)\r\n");
kernel/core/main.c:778:            if (vm_s3_scratch(P,0x1398)!=JE_ABI_H_SHORT_BUFFER) fail("JeiOS fatal: s4 not SHORT_BUFFER\r\n");
kernel/core/main.c:780:            if (vm_s3_scratch(P,0x13a8)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 bad-dst recv not INVALID\r\n");
kernel/core/main.c:784:            if (vm_s3_scratch(P,0x13c8)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 not delivered exactly once\r\n");
kernel/core/main.c:785:            if (vm_s3_scratch(P,0x13d0)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 bad-src send not INVALID\r\n");
kernel/core/main.c:786:            if (vm_s3_scratch(P,0x13d8)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 bad send enqueued something\r\n");
kernel/core/main.c:788:            if (vm_s3_scratch(P,0x13e8)!=JE_ABI_H_QUEUE_FULL) fail("JeiOS fatal: s4 not QUEUE_FULL\r\n");
kernel/core/main.c:789:            if (vm_s3_scratch(P,0x13f0)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 cap_count send not INVALID (data-only)\r\n");
kernel/core/main.c:792:            if (vm_s3_scratch(P,0x1408)!=JE_ABI_H_PEER_CLOSED) fail("JeiOS fatal: s4 drained recv not PEER_CLOSED\r\n");
kernel/core/main.c:793:            if (vm_s3_scratch(P,0x1410)!=JE_ABI_H_PEER_CLOSED) fail("JeiOS fatal: s4 send to closed peer not PEER_CLOSED\r\n");
kernel/core/main.c:796:            if (je_vm_destroy(&vm_s3, P)!=JE_OK) fail("JeiOS fatal: s4 ipc destroy\r\n");
kernel/core/main.c:797:            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s4 ipc pool leak\r\n");
kernel/core/main.c:798:            je_arch_console_write("M3c-s4: ring-3 try_send/try_recv loopback OK (direction, SHORT_BUFFER, copy-out failure leaves msg queued -> delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED); channel reclaimed on exit\r\n");
kernel/core/main.c:804:        if (je_vm_destroy(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 destroy B\r\n");
kernel/core/main.c:823:            if (je_endpoint_kern_seed(&vm_s3, XA, XB, &sA, &sB)!=JE_EP_OK)
kernel/core/main.c:826:            if (je_endpoint_kernel_allocated()!=1u) fail("JeiOS fatal: s5 seed pool != 1\r\n");
kernel/core/main.c:829:            if (je_endpoint_kern_create(&vm_s3, XA, &c0, &c1)!=JE_EP_OK)
kernel/core/main.c:831:            if (je_endpoint_kernel_allocated()!=2u) fail("JeiOS fatal: s5 pool != 2\r\n");
kernel/core/main.c:834:            if (je_endpoint_kern_resolve_send_cap(&vm_s3, XA, c1, JE_EP_RIGHTS_ALL, &d)!=JE_EP_OK)
kernel/core/main.c:842:            if (je_endpoint_kern_send_caps(&vm_s3, XA, sA, (void*)0, 0, &d, 1)!=JE_EP_OK)
kernel/core/main.c:847:            if (je_endpoint_kern_recv_peek_caps(&vm_s3, XB, sB, (void*)0, 0, &rl,
kernel/core/main.c:851:            if (!je_endpoint_kern_capdesc_live(&rc[0])) fail("JeiOS fatal: s5 B cap not live\r\n");
kernel/core/main.c:853:            if (je_endpoint_kern_mint_recv_cap(&vm_s3, XB, &rc[0], &mintedB)!=JE_EP_OK || mintedB==0u)
kernel/core/main.c:855:            if (je_endpoint_kern_recv_commit(&lz)!=JE_EP_OK) fail("JeiOS fatal: s5 B commit\r\n");
kernel/core/main.c:858:            if (je_endpoint_kern_close(&vm_s3, XA, c1)!=JE_EP_OK)
kernel/core/main.c:865:                if (je_endpoint_kern_send(&vm_s3, XB, mintedB, msg, 4)!=JE_EP_OK)
kernel/core/main.c:868:                if (je_endpoint_kern_recv_peek(&vm_s3, XA, c0, rb, sizeof rb, &bl, &lz2)!=JE_EP_OK)
kernel/core/main.c:871:                if (je_endpoint_kern_recv_commit(&lz2)!=JE_EP_OK) fail("JeiOS fatal: s5 A commit\r\n");
kernel/core/main.c:875:             * walk in je_vm_destroy. */
kernel/core/main.c:876:            if (je_vm_destroy(&vm_s3, XA)!=JE_OK) fail("JeiOS fatal: s5 XA destroy\r\n");
kernel/core/main.c:877:            if (je_vm_destroy(&vm_s3, XB)!=JE_OK) fail("JeiOS fatal: s5 XB destroy\r\n");
kernel/core/main.c:878:            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s5 pool leak after teardown\r\n");
kernel/core/usercopy.c:97: * user span. Returns JE_ABI_OK or JE_ABI_INVALID; never dereferences the user
kernel/core/usercopy.c:103:    if (je_sched_current_user(&vm, &index) != JE_OK || !vm || !vm->ops) return JE_ABI_INVALID;
kernel/core/usercopy.c:104:    if (!span_structure_ok(uaddr, len)) return JE_ABI_INVALID;
kernel/core/usercopy.c:106:    if (!span_pages_ok(vm, index, uaddr, len, need_write)) return JE_ABI_INVALID;
kernel/core/usercopy.c:119:    return faulted ? JE_ABI_INVALID : JE_ABI_OK;  /* a recovered fault = failure */
kernel/core/usercopy.c:166: * unrestricted test op, NOT a capability-gated production endpoint; it is present
kernel/core/usercopy.c:171: * JE_ABI_INVALID; never commits output on a failed input stage. */
kernel/core/usercopy.c:176:    if (len > JE_COPY_PROBE_MAX) return JE_ABI_INVALID;
kernel/core/usercopy.c:178:    if (je_copy_from_user(scratch, usrc, len) != JE_ABI_OK) return JE_ABI_INVALID;
kernel/core/usercopy.c:179:    if (je_copy_to_user(udst, scratch, len) != JE_ABI_OK) return JE_ABI_INVALID;
kernel/core/sched.c:9:typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING } state;
kernel/core/sched.c:16:    uint64_t stack, wake, dispatches;
kernel/core/sched.c:34:static thread threads[JE_THREAD_MAX];
kernel/core/sched.c:39:volatile uint64_t je_timer_ticks;
kernel/core/sched.c:59:    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
kernel/core/sched.c:60:    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
kernel/core/sched.c:85: * the vm process and is freed by je_vm_destroy, never by je_sched_reap. */
kernel/core/sched.c:88:    if (!ready || !vm || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
kernel/core/sched.c:94:    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
kernel/core/sched.c:95:    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
kernel/core/sched.c:136:    for (unsigned attempt=0; attempt<JE_THREAD_MAX; ++attempt) {
kernel/core/sched.c:138:        for (unsigned n=1;n<=JE_THREAD_MAX;++n) {
kernel/core/sched.c:139:            unsigned candidate=(current+n)%JE_THREAD_MAX;
kernel/core/sched.c:166:    if (!ready || threads[current].state==EMPTY) abort_scheduler();
kernel/core/sched.c:180:        if (je_timer_ticks==UINT64_MAX) abort_scheduler();
kernel/core/sched.c:181:        ++je_timer_ticks;
kernel/core/sched.c:183:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:184:        if (threads[i].state==SLEEPING && threads[i].wake<=je_timer_ticks) threads[i].state=READY;
kernel/core/sched.c:242:je_status je_thread_sleep(uint64_t duration)
kernel/core/sched.c:245:    if (!ready || current==0 || duration>UINT64_MAX-je_timer_ticks) {
kernel/core/sched.c:249:        threads[current].wake=je_timer_ticks+duration;
kernel/core/sched.c:250:        threads[current].state=SLEEPING;
kernel/core/sched.c:265:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:266:        if (threads[i].state!=EMPTY && threads[i].state!=ZOMBIE) return false;
kernel/core/sched.c:272:    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==ZOMBIE) {
kernel/core/sched.c:274:         * vm process's held run, freed by je_vm_destroy. Reaping it here would
kernel/core/sched.c:285:uint64_t je_sched_ticks(void) { return je_timer_ticks; }
kernel/core/sched.c:289: * before its process is destroyed, so the scheduler never dispatches a torn-down
kernel/core/sched.c:294:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:312: * kstack guard and destroy its vm process (freeing the held run + tables). The
kernel/core/sched.c:313: * vm record stays READY until destroy (D5's DYING/REAPABLE are modeled in the
kernel/core/sched.c:321:     * destroy must be atomic vs a timer dispatch. Enforce it here rather than
kernel/core/sched.c:327:    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) {
kernel/core/sched.c:334:        s=je_vm_destroy(vm, idx);                /* frees the held kstack run + page tables */
kernel/core/sched.c:346:    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) return true;
kernel/core/sched.c:352:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:367:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:378:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:395:    for (unsigned i=1;i<JE_THREAD_MAX;++i)
kernel/core/sched.c:396:        if (threads[i].state!=EMPTY && threads[i].dispatches<min) min=threads[i].dispatches;
include/jeios/abi.h:17: * recognized operation with a nonzero reserved argument returns JE_ABI_INVALID
include/jeios/abi.h:26:    JE_ABI_INVALID     = 2,
include/jeios/abi.h:42:                              * (no ring-3 endpoint op yet) — first surfaced by a
include/jeios/abi.h:45:    JE_ABI_H_QUEUE_FULL  = 10, /* M3c D6: try_send on an end whose peer inbound
include/jeios/abi.h:47:    JE_ABI_H_SHORT_BUFFER = 11, /* M3c D6: try_recv with a buffer smaller than the
include/jeios/abi.h:49:    JE_ABI_H_PEER_CLOSED = 12, /* M3c D6: try_send to a peer-closed end, or try_recv
include/jeios/abi.h:52:    JE_ABI_H_EMPTY       = 13  /* M3c D6: non-blocking try_recv on an OPEN end with
include/jeios/abi.h:54:                              * step 6). Distinct from PEER_CLOSED (peer gone). */
include/jeios/abi.h:58:_Static_assert(JE_ABI_INVALID == 2, "abi invalid");
include/jeios/abi.h:68:_Static_assert(JE_ABI_H_QUEUE_FULL == 10, "abi h_queue_full");

 exited 2 in 0ms:
/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/arch.h>
#include <jeios/kheap.h>
#include <jeios/paging.h>
#include <jeios/sched.h>
#include <jeios/uframe.h>
#include <jeios/vm.h>
#define STACK_PAGES 5u
typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING } state;
typedef enum { THREAD_KERNEL = 0, THREAD_USER = 1 } thread_kind;
typedef struct {
    state state;
    je_trap_frame *frame;
    void (*entry)(void *);
    void *argument;
    uint64_t stack, wake, dispatches;
    /* M3a #4 step 3: user-thread fields. For THREAD_KERNEL these are 0/unused.
     * For THREAD_USER: root is the process CR3; rsp0_top is the per-process
     * kernel-stack top loaded into TSS.RSP0 on dispatch; vm/vm_index name the
     * owning process so teardown (not reap) frees its held kstack. */
    thread_kind kind;
    uint64_t root, rsp0_top;
    je_vm *vm;
    uint32_t vm_index;
    /* M3a #4 step 4 / #5: death record for a DYING user thread. `reason`
     * distinguishes a synchronous fault, a clean exit(code), and an invalid
     * return state (read by the bootstrap reaper and the proofs). */
    uint64_t death_vector, death_error, death_rip, death_cs, death_cr2;
    uint64_t death_reason, exit_code;
} thread;
/* Death reasons (M3a #5). FAULT: synchronous user fault. EXIT: clean exit(code).
 * BAD_RETURN: a user frame failed je_uframe_check (invalid return state). */
typedef enum { DEATH_FAULT = 0, DEATH_EXIT = 1, DEATH_BAD_RETURN = 2 } death_reason;
static thread threads[JE_THREAD_MAX];
static unsigned current;
static bool ready;
static uint64_t preemptions;
static uint64_t user_cpl3_dispatches; /* M3a #4 step 3: times a THREAD_USER frame (cs&3==3) was seen */
volatile uint64_t je_timer_ticks;
static _Noreturn void abort_scheduler(void)
{
    (void)je_arch_irq_save();
    je_arch_console_write("JeiOS fatal: scheduler invariant\r\n");je_arch_halt();
}
static _Noreturn void trampoline(void)
{
    threads[current].entry(threads[current].argument);
    je_thread_exit();
}
void je_sched_init(void)
{
    if (ready) abort_scheduler();
    threads[0].state=RUNNING;current=0;ready=true;
}
je_status je_thread_create(void (*entry)(void *),void *arg)
{
    if (!ready || !entry) return JE_ERR_INVALID;
    unsigned slot=1;
    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
    uint64_t base;
    je_status s=je_pages_alloc(&je_kernel_pages,STACK_PAGES,&base);
    if (s!=JE_OK) return s;
    unsigned char *memory=(void *)(uintptr_t)base;
    for (size_t i=0;i<STACK_PAGES*4096;++i) memory[i]=0;
    s=je_arch_page_guard((uintptr_t)base);
    if (s!=JE_OK) {
        je_status cleanup=je_pages_free(&je_kernel_pages,base,STACK_PAGES);
        return cleanup==JE_OK ? s : cleanup;
    }
    uintptr_t top=(uintptr_t)base+STACK_PAGES*4096;
    uintptr_t rsp=top-8; /* SysV C entry: RSP % 16 == 8, with fake return address. */
    *(uintptr_t *)rsp=(uintptr_t)je_thread_exit;
    je_trap_frame *f=(void *)(rsp-sizeof(*f));
    *f=(je_trap_frame){.rip=(uintptr_t)trampoline,.cs=8,.rflags=0x202,.rsp=rsp,.ss=16};
    threads[slot]=(thread){READY,f,entry,arg,base,0,0,THREAD_KERNEL,0,0,0,0,0,0,0,0,0,0,0};
    return JE_OK;
}

/* M3a #4 step 3: create a user thread for a READY vm process. Synthesizes the
 * initial ring-3 trap frame on the process's held kernel stack, so the first
 * scheduler dispatch iretqs into CPL3 at the validated entry. The kstack is the
 * process's concrete 5-page run (held by #3); its top is TSS.RSP0. Requires NX
 * (ADR 0007 D2). Does NOT allocate from je_kernel_pages — the kstack belongs to
 * the vm process and is freed by je_vm_destroy, never by je_sched_reap. */
je_status je_thread_create_user(je_vm *vm, uint32_t index)
{
    if (!ready || !vm || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    if (!je_arch_has_nx()) return JE_ERR_UNSUPPORTED; /* D2 */
    je_vm_process *p = &vm->procs[index];
    if (p->state != JE_VM_READY || p->held_kstack != JE_VM_KSTACK_PAGES || !p->root)
        return JE_ERR_INVALID;
    unsigned slot=1;
    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
    uint64_t kbase = p->kstack_physical;              /* identity VA (< 1 GiB) */
    /* Guard the first page of the held kstack (ADR D4 assigns the guard to #4);
     * RSP0 is the top of the remaining usable pages. */
    je_status g = je_arch_page_guard((uintptr_t)kbase);
    if (g != JE_OK) return g;
    uint64_t rsp0_top = kbase + (uint64_t)STACK_PAGES * 4096u;
    /* Initial user frame at the very top of the kstack, GPRs zero (no kernel
     * register leak into ring 3). user RSP starts at arena top (full-descending,
     * validator allows top inclusive); RIP at validated entry. */
    je_trap_frame *f = (void *)(uintptr_t)(rsp0_top - sizeof(*f));
    *f = (je_trap_frame){0};
    f->cs = JE_UFRAME_USER_CS; f->ss = JE_UFRAME_USER_SS;
    f->rip = JE_VM_ARENA_BASE + p->entry;
    f->rsp = JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE;
    f->rflags = 0x202u; /* IF=1, bit1; validator re-sanitizes on every dispatch */
    threads[slot]=(thread){READY,f,0,0,0,0,0,THREAD_USER,p->root,rsp0_top,vm,index,0,0,0,0,0,0,0};
    return JE_OK;
}
/* M3a #4 step 4: mark a user thread DYING with a death record (never requeued;
 * reclaimed later by je_sched_reap_dead from bootstrap). Kernel threads never
 * take this path. */
static void mark_dying(unsigned slot, death_reason reason, uint64_t code,
                       uint64_t vec, uint64_t err, uint64_t rip, uint64_t cs, uint64_t cr2)
{
    threads[slot].state=DYING;
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
            threads[next].frame->rflags = sane;
            je_arch_tss_set_rsp0(threads[next].rsp0_top);
            je_arch_cr3_load(threads[next].root);
        } else {
            je_arch_tss_set_rsp0(0); /* poison: CPL0->CPL0 never consults RSP0 */
            je_arch_cr3_load(je_arch_kernel_root_phys());
        }
        current=next;threads[current].state=RUNNING;++threads[current].dispatches;
        return threads[current].frame;
    }
    abort_scheduler(); /* unreachable: bootstrap is always a valid kernel candidate */
}

je_trap_frame *je_sched_interrupt(je_trap_frame *f,bool timer)
{
    if (!ready || threads[current].state==EMPTY) abort_scheduler();
    /* Validate the INCOMING frame by the current thread's kind, not by trusting
     * the frame. A kernel thread must present a CPL0 frame (cs==8); a user
     * thread must present its exact user frame landing at rsp0_top-frame (where
     * the CPU writes it on a CPL3->CPL0 trap). Any mismatch is corruption. */
    if (threads[current].kind==THREAD_KERNEL) {
        if ((f->cs&3) != 0 || f->cs != 8) abort_scheduler();
    } else {
        if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS) abort_scheduler();
        if ((uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
        ++user_cpl3_dispatches; /* observed a genuine CPL3 (cs&3==3) frame return from a user thread */
    }
    threads[current].frame=f;
    if (timer) {
        if (je_timer_ticks==UINT64_MAX) abort_scheduler();
        ++je_timer_ticks;
    }
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].state==SLEEPING && threads[i].wake<=je_timer_ticks) threads[i].state=READY;
    if (threads[current].state==RUNNING) threads[current].state=READY;
    unsigned before=current;
    je_trap_frame *nf=select_and_install_next();
    if (timer && current!=before) ++preemptions;
    return nf;
}

/* M3a #4 step 4: a SYNCHRONOUS user fault (or user int 0x80) terminates ONLY the
 * current user process; the kernel and peers keep running. Called from
 * je_trap_dispatch with interrupts disabled, on the dying task's own per-process
 * RSP0 kstack — so we must NOT free that kstack here (the C frame + the iretq
 * epilogue still run on it). We mark the task DYING and return a LIVE successor's
 * frame; the bootstrap reaper reclaims the dead process later. Never requeues
 * the dying task. */
je_trap_frame *je_sched_fault_terminate(je_trap_frame *f, uint64_t cr2)
{
    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler(); /* only user tasks die here */
    /* Confirm the frame really is the current user task's (defense in depth). */
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
    mark_dying(current, DEATH_FAULT, 0, f->vector, f->error, f->rip, f->cs, cr2);
    return select_and_install_next();
}

/* M3a #5: clean exit(code) — same mechanics as fault_terminate (mark the current
 * user thread DYING on its own kstack, return a live successor; the reaper frees
 * it later) but records a DEATH_EXIT reason and the exit code rather than a
 * fault. Separate from je_sched_fault_terminate (tested step-4 code) by design. */
je_trap_frame *je_sched_exit_current(je_trap_frame *f, uint64_t code)
{
    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
    mark_dying(current, DEATH_EXIT, code, 0, 0, f->rip, f->cs, 0);
    return select_and_install_next();
}

/* M3a #5: return path for a syscall that resumes the CURRENT user task (no
 * context switch) — e.g. abi_query, UNSUPPORTED, INVALID. ADR 0007 D6/M3A.md §4
 * require validating the user frame on EVERY return, so a non-switching syscall
 * return must NOT skip je_uframe_check (a user can corrupt its own RSP/RFLAGS
 * before int 0x80). Validate+sanitize here; on failure terminate (BAD_RETURN)
 * and switch to a live successor instead of iretq'ing a bad frame. The caller
 * has already written the result registers (rax/rdx) into f. */
je_trap_frame *je_sched_return_to_user(je_trap_frame *f)
{
    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
    uint64_t sane;
    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
        mark_dying(current, DEATH_BAD_RETURN, 0, 0, 0, f->rip, f->cs, 0);
        return select_and_install_next();
    }
    f->rflags = sane;
    return f; /* resume the caller; CR3/RSP0 already correct for the current task */
}
je_status je_thread_sleep(uint64_t duration)
{
    uint64_t flags=je_arch_irq_save();
    if (!ready || current==0 || duration>UINT64_MAX-je_timer_ticks) {
        je_arch_irq_restore(flags);return current==0 ? JE_ERR_DENIED : JE_ERR_INVALID;
    }
    if (duration) {
        threads[current].wake=je_timer_ticks+duration;
        threads[current].state=SLEEPING;
    }
    __asm__ volatile("int $0x81":::"memory");
    je_arch_irq_restore(flags);return JE_OK;
}
_Noreturn void je_thread_exit(void)
{
    (void)je_arch_irq_save();
    if (!ready || current==0) abort_scheduler();
    threads[current].state=ZOMBIE;
    __asm__ volatile("int $0x81":::"memory");
    abort_scheduler();
}
bool je_sched_finished(void)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].state!=EMPTY && threads[i].state!=ZOMBIE) return false;
    return true;
}
je_status je_sched_reap(void)
{
    if (current!=0 || !je_sched_finished()) return JE_ERR_BUSY;
    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==ZOMBIE) {
        /* User threads own no je_kernel_pages stack: their kernel stack is the
         * vm process's held run, freed by je_vm_destroy. Reaping it here would
         * double-free (M3a #4 step 3). Clear the slot only. */
        if (threads[i].kind==THREAD_USER) { threads[i]=(thread){0}; continue; }
        je_status s=je_arch_page_restore((uintptr_t)threads[i].stack);
        if (s!=JE_OK) return s;
        s=je_pages_free(&je_kernel_pages,threads[i].stack,STACK_PAGES);
        if (s!=JE_OK) return s;
        threads[i]=(thread){0};
    }
    return JE_OK;
}
uint64_t je_sched_ticks(void) { return je_timer_ticks; }
uint64_t je_sched_preemptions(void) { return preemptions; }
uint64_t je_sched_user_cpl3_dispatches(void) { return user_cpl3_dispatches; }
/* M3a #4 step 3: remove a user thread (by its vm process) from scheduling
 * before its process is destroyed, so the scheduler never dispatches a torn-down
 * CR3. Caller holds interrupts disabled and must not be the user thread. */
je_status je_sched_drop_user(je_vm *vm, uint32_t index)
{
    if (!ready || !vm) return JE_ERR_INVALID;
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index) {
            if (i==current) return JE_ERR_BUSY;
            if (threads[i].rsp0_top) { /* restore the kstack guard before the process frees the run */
                uint64_t kbase=threads[i].rsp0_top-(uint64_t)STACK_PAGES*4096u;
                je_status s=je_arch_page_restore((uintptr_t)kbase);
                if (s!=JE_OK) return s;
            }
            threads[i]=(thread){0};
            return JE_OK;
        }
    return JE_ERR_INVALID;
}

/* M3a #4 step 4: reclaim DYING user processes while peers keep running. Runs in
 * bootstrap (slot 0) with interrupts disabled (caller's responsibility). For
 * each DYING user slot: assert it is not current, not the active CR3, and not
 * the active TSS.RSP0 (the task has been iretq'd away from), then restore its
 * kstack guard and destroy its vm process (freeing the held run + tables). The
 * vm record stays READY until destroy (D5's DYING/REAPABLE are modeled in the
 * scheduler slot, not vm.c). Returns the number reaped, or a negative je_status
 * on an invariant violation. Single-CPU/PCID-off invariant: after one iretq away
 * from the dead task, nothing else runs on its kstack. */
int je_sched_reap_dead(void)
{
    if (!ready || current!=0) return (int)JE_ERR_BUSY;
    /* Must run with interrupts disabled: the three liveness guards below and the
     * destroy must be atomic vs a timer dispatch. Enforce it here rather than
     * trusting the caller (defense in depth). */
    uint64_t rflags; __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    if (rflags & (UINT64_C(1)<<9)) abort_scheduler(); /* IF set -> misuse */
    uint64_t active_cr3=je_arch_cr3_read(), active_rsp0=je_arch_tss_get_rsp0();
    int reaped=0;
    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) {
        if (i==current) return (int)JE_ERR_BUSY;
        if (threads[i].root==active_cr3) return (int)JE_ERR_BUSY;    /* still the live address space */
        if (threads[i].rsp0_top==active_rsp0) return (int)JE_ERR_BUSY; /* still the live kernel stack */
        je_vm *vm=threads[i].vm; uint32_t idx=threads[i].vm_index;
        je_status s=je_sched_drop_user(vm, idx); /* restores the kstack guard, clears the slot */
        if (s!=JE_OK) return (int)s;
        s=je_vm_destroy(vm, idx);                /* frees the held kstack run + page tables */
        if (s!=JE_OK) return (int)s;
        ++reaped;
    }
    return reaped;
}

/* M3a #4 step 4 proof accessors: whether a DYING user slot exists and its death
 * record, plus a single-slot dispatch counter so the proof can show a peer
 * progressed AFTER a specific death. Slot identity is by vm process. */
bool je_sched_has_dying(void)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) return true;
    return false;
}
je_status je_sched_death_record(je_vm *vm, uint32_t index, uint64_t *vec, uint64_t *err,
                                uint64_t *rip, uint64_t *cs, uint64_t *cr2)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index &&
            threads[i].state==DYING) {
            if (vec) *vec=threads[i].death_vector;
            if (err) *err=threads[i].death_error;
            if (rip) *rip=threads[i].death_rip;
            if (cs)  *cs=threads[i].death_cs;
            if (cr2) *cr2=threads[i].death_cr2;
            return JE_OK;
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3c sub-PR 2 (ADR 0009 D-SEQ step 2): endpoint/channel lifecycle core.
 * Hostable, no kernel state, no ABI include — see endpoint.h for the full
 * contract and the load-bearing table-purity precondition. Sound only under the
 * ADR 0008 D6 uniprocessor / IF=0 precondition (no SMP claim).
 */
#include <jeios/endpoint.h>

/* Policy-bound pins (ADR 0009 D4): REF_MAX must admit every non-handle owning
 * ref (queue depth + waiters) and still be small enough to reach in a test, and
 * it must fit the uint16_t counters. */
_Static_assert(JE_EP_REF_MAX >= JE_EP_QUEUE_DEPTH + JE_EP_MAX_WAITERS,
               "JE_EP_REF_MAX must cover queue depth + waiters");
_Static_assert(JE_EP_REF_MAX <= 0xffffu, "ref counts are uint16_t");
_Static_assert(JE_EP_POOL_CAP >= 1u, "pool must have at least one record");
_Static_assert(JE_EP_MSG_MAX <= 255u, "per-slot length fits uint8_t");
_Static_assert(JE_EP_QUEUE_DEPTH <= 255u, "head/count fit uint8_t");
_Static_assert(JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX <= JE_EP_REF_MAX,
               "a full queue of cap-carrying msgs must not overflow inflight");
/* Keep the static kernel pool bounded: the per-slot byte buffers + cap
 * descriptors dominate. Grew for step-5 caps; re-pinned here. */
_Static_assert(sizeof(je_ep_channel) <= 1280u, "channel record size budget");

/* ---- pool / record helpers ------------------------------------------------ */

/* Reset (empty + zero) one inbound queue. Zeroing the payload bytes prevents a
 * stale message surviving into a reused slot and never lets uninitialized bytes
 * reach a reader (defense in depth; recv copies only len bytes regardless). */
static void queue_reset(je_ep_queue *q)
{
    q->head = 0u;
    q->count = 0u;
    for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i) {
        q->len[i] = 0u;
        q->cap_count[i] = 0u;
        for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[i][b] = 0u;
        for (uint8_t c = 0; c < JE_EP_MSG_CAPS_MAX; ++c) {
            q->cap[i][c].rec_index = 0u; q->cap[i][c].side = 0u;
            q->cap[i][c].cap_count_pad = 0u;
            q->cap[i][c].generation = 0u; q->cap[i][c].rights = 0u;
        }
    }
}

/* Count the capability descriptors currently queued anywhere in one inbound
 * queue (used by the A5-3 free-time assertion: a record only frees when BOTH
 * inbound queues are cap-free). */
static uint32_t queue_cap_total(const je_ep_queue *q)
{
    uint32_t n = 0u;
    for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i) n += q->cap_count[i];
    return n;
}

void je_endpoint_pool_init(je_endpoint_pool *pool)
{
    if (!pool) return;
    pool->allocated_count = 0u;
    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        je_ep_channel *r = &pool->rec[i];
        r->allocated = false;
        r->generation = 1u;            /* never 0 */
        for (uint8_t s = 0; s < 2u; ++s) {
            r->end[s].state = JE_EP_END_FREE;
            r->end[s].handle_refs = 0u;
            r->end[s].inflight_refs = 0u;
            r->end[s].side = s;
            queue_reset(&r->end[s].inbound);
        }
    }
}

uint32_t je_endpoint_pool_allocated(const je_endpoint_pool *pool)
{
    return pool ? pool->allocated_count : 0u;
}

je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side)
{
    if (!rec || side > 1u) return JE_EP_END_FREE;
    return rec->end[side].state;
}

/* The opaque object pointer a handle slot binds for an endpoint is &rec->end[s].
 * Map such a pointer back to (rec, side), validating it lies on a real end of a
 * LIVE record in THIS pool — never dereference an unvalidated pointer. */
static je_ep_status object_to_end(const je_endpoint_pool *pool, const void *obj,
                                  je_ep_channel **out_rec, uint8_t *out_side)
{
    if (!pool || !obj) return JE_EP_CORRUPT;
    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        const je_ep_channel *r = &pool->rec[i];
        for (uint8_t s = 0; s < 2u; ++s) {
            if ((const void *)&r->end[s] == obj) {
                if (!r->allocated) return JE_EP_CORRUPT;   /* stale end pointer */
                *out_rec = (je_ep_channel *)r;
                *out_side = s;
                return JE_EP_OK;
            }
        }
    }
    return JE_EP_CORRUPT;
}

/* Free a record once BOTH ends hold no references of either class. */
static void maybe_free_record(je_endpoint_pool *pool, je_ep_channel *rec)
{
    /* A5-3 idempotence guard: the cap-release discard path can reach this twice
     * for one record; a second call on an already-free record must be a no-op
     * (never double-decrement allocated_count / double-bump generation). */
    if (!rec->allocated) return;
    for (uint8_t s = 0; s < 2u; ++s) {
        if (rec->end[s].handle_refs != 0u || rec->end[s].inflight_refs != 0u)
            return;
    }
    /* A5-3: a record only frees when BOTH ends have zero refs. With zero inflight
     * refs there can be no queued caps pointing at this record, and this record's
     * own inbound queues must themselves be cap-free. If a residual cap is somehow
     * present it means an inflight ref was leaked — an invariant violation; guard
     * by NOT freeing (fail-safe: leak the record rather than free it with a live
     * cap), which is unreachable under the detach-before-release discard order. */
    for (uint8_t s = 0; s < 2u; ++s) {
        if (queue_cap_total(&rec->end[s].inbound) != 0u) return; /* unreachable; fail-safe */
    }
    /* Both ends fully unreferenced: reclaim. Bump generation (never 0). */
    rec->allocated = false;
    rec->generation = (rec->generation == 0xffffffffu) ? 1u
                                                       : rec->generation + 1u;
    for (uint8_t s = 0; s < 2u; ++s) {
        rec->end[s].state = JE_EP_END_FREE;
        rec->end[s].side = s;
        queue_reset(&rec->end[s].inbound);   /* no stale payload into reuse */
    }
    if (pool->allocated_count) pool->allocated_count--;
}

/* Map je_handle_status -> je_ep_status for the resolve/alloc paths. */
static je_ep_status hstat(je_handle_status s)
{
    switch (s) {
    case JE_HANDLE_OK:        return JE_EP_OK;
    case JE_HANDLE_BADINDEX:  return JE_EP_BADINDEX;
    case JE_HANDLE_STALE:     return JE_EP_STALE;
    case JE_HANDLE_WRONGTYPE: return JE_EP_WRONGTYPE;
    case JE_HANDLE_DENIED:    return JE_EP_DENIED;
    case JE_HANDLE_QUOTA:     return JE_EP_QUOTA;
    case JE_HANDLE_NOSLOTS:   return JE_EP_NOSLOTS;
    }
    return JE_EP_CORRUPT;   /* fail closed if the enum ever grows */
}

/* ---- resolve a handle to its end ----------------------------------------- */

je_ep_status je_endpoint_resolve(const je_endpoint_pool *pool,
                                 const je_handle_table *t, je_handle h,
                                 je_ep_channel **out_rec, uint8_t *out_side)
{
    void *obj = (void *)0;
    je_handle_status s;
    if (!pool || !t || !out_rec || !out_side) return JE_EP_BADINDEX;
    /* Resolve as an ENDPOINT with no required rights; keeps the table's
     * BADINDEX -> STALE -> WRONGTYPE precedence. */
    s = je_handle_lookup(t, h, JE_ENDPOINT_OBJ_TYPE, 0u, &obj);
    if (s != JE_HANDLE_OK) return hstat(s);
    return object_to_end(pool, obj, out_rec, out_side);
}

/* ---- internal handle_ref acquire / release ------------------------------- */

/* Acquire one handle_ref on an end; fail closed past REF_MAX with nothing taken. */
static je_ep_status retain_handle(je_ep_end *e)
{
    if (e->handle_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
    e->handle_refs++;
    return JE_EP_OK;
}

/* Drop one handle_ref on an end that definitely holds at least one (infallible
 * for a valid live ref). On reaching 0: this end CLOSED, a live peer -> PEER_CLOSED,
 * then try to reclaim the record. */
/* Release one inflight ref on the end a queued cap descriptor points at (A5-3
 * discard path). The target is in THIS pool; a guarded maybe_free_record may free
 * it. Defence-in-depth: validate the descriptor resolves to an allocated,
 * generation-matching end (it must, while the inflight ref kept it alive). */
static void release_capdesc_inflight(je_endpoint_pool *pool,
                                     const je_ep_capdesc *d)
{
    if (d->rec_index >= JE_EP_POOL_CAP || d->side > 1u) return; /* unreachable */
    je_ep_channel *tr = &pool->rec[d->rec_index];
    if (!tr->allocated || tr->generation != d->generation) return; /* unreachable */
    je_ep_end *te = &tr->end[d->side];
    if (te->inflight_refs == 0u) return;   /* underflow guard (unreachable) */
    te->inflight_refs--;
    if (te->inflight_refs == 0u) maybe_free_record(pool, tr);
}

static void release_handle(je_endpoint_pool *pool, je_ep_channel *rec, uint8_t side)
{
    je_ep_end *e = &rec->end[side];
    /* handle_refs == 0 here would be an invariant violation by the caller; guard
     * defensively (never underflow) but do not mask it as success elsewhere. */
    if (e->handle_refs == 0u) return;
    e->handle_refs--;
    if (e->handle_refs == 0u) {
        e->state = JE_EP_END_CLOSED;
        je_ep_end *peer = &rec->end[side ^ 1u];
        if (peer->state == JE_EP_END_OPEN) peer->state = JE_EP_END_PEER_CLOSED;
        /* A5-3 discard ordering: (1) DETACH every cap descriptor still queued in
         * this closing end's inbound into a bounded local array; (2) reset the
         * queue (bytes + cap storage) so this record is fully settled and holds
         * no caps; (3) maybe_free THIS record; (4) ONLY THEN release each detached
         * inflight ref on its (other-record) target. Doing releases last means
         * freeing a target never re-enters a queue still being discarded, and the
         * guarded maybe_free_record is idempotent if a target is this same record
         * via the peer end (its inbound was/will be discarded at its own close). */
        je_ep_capdesc detached[JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX];
        uint32_t nd = 0u;
        je_ep_queue *q = &e->inbound;
        for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i)
            for (uint8_t c = 0; c < q->cap_count[i]; ++c)
                detached[nd++] = q->cap[i][c];
        queue_reset(q);                  /* this end holds no caps now */
        maybe_free_record(pool, rec);    /* settle THIS record first */
        for (uint32_t k = 0; k < nd; ++k)
            release_capdesc_inflight(pool, &detached[k]);  /* then other records */
    }
}

/* ---- inflight ref API (tested directly; used by queue/wait PRs) ----------- */

/* Is `rec` one of this pool's channel records? Uses uintptr_t comparison (not
 * relational pointer comparison, which is UB for a pointer outside the array)
 * and requires exact slot alignment, so a forged/other-pool pointer fails. */
static bool rec_in_pool(const je_endpoint_pool *pool, const je_ep_channel *rec)
{
    uintptr_t base = (uintptr_t)&pool->rec[0];
    uintptr_t top  = (uintptr_t)&pool->rec[JE_EP_POOL_CAP];   /* one-past-end */
    uintptr_t p    = (uintptr_t)rec;
    if (p < base || p >= top) return false;
    return ((p - base) % sizeof(pool->rec[0])) == 0u;         /* slot-aligned */
}

je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
                                         je_ep_channel *rec, uint8_t side)
{
    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
    /* Provenance: rec MUST be one of this pool's records (reject a wrong-pool or
     * stray pointer, which would otherwise corrupt a different channel's count).
     * NOTE: this does NOT defend against a stale pointer to a since-freed-and-
     * reused record in the SAME pool — the ADR 0009 borrowed-pointer rule makes
     * that a trusted-caller precondition, and the queue/wait PR that first holds
     * such a pair MUST carry the record generation to detour reuse. */
    if (!rec_in_pool(pool, rec) || !rec->allocated) return JE_EP_CORRUPT;
    je_ep_end *e = &rec->end[side];
    if (e->inflight_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
    e->inflight_refs++;
    return JE_EP_OK;
}

je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
                                          je_ep_channel *rec, uint8_t side)
{
    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
    if (!rec_in_pool(pool, rec) || !rec->allocated) return JE_EP_CORRUPT;
    je_ep_end *e = &rec->end[side];
    if (e->inflight_refs == 0u) return JE_EP_CORRUPT;   /* underflow guard */
    e->inflight_refs--;
    if (e->inflight_refs == 0u) maybe_free_record(pool, rec);
    return JE_EP_OK;
}

/* ---- create (kernel-only) ------------------------------------------------- */

/* Return a tentatively-allocated record to the FREE state on a create unwind,
 * restoring the FREE <=> !allocated invariant (states, not just the flag). The
 * record was never observed by any caller, so no generation bump is needed. */
static void unwind_record(je_ep_channel *rec)
{
    rec->allocated = false;
    for (uint8_t s = 0; s < 2u; ++s) {
        rec->end[s].state = JE_EP_END_FREE;
        rec->end[s].handle_refs = 0u;
        rec->end[s].inflight_refs = 0u;
        rec->end[s].side = s;
    }
}

je_ep_status je_endpoint_channel_create(je_endpoint_pool *pool,
                                        je_handle_table *t0, uint32_t rights0,
                                        je_handle_table *t1, uint32_t rights1,
                                        je_handle *out0, je_handle *out1)
{
    if (out0) *out0 = 0u;
    if (out1) *out1 = 0u;
    if (!pool || !t0 || !t1 || !out0 || !out1) return JE_EP_BADINDEX;
    /* Aliased outputs would make the second write clobber the first handle,
     * orphaning end 0's LIVE slot (a permanently unreclaimable ref). Reject. */
    if (out0 == out1) return JE_EP_BADINDEX;
    /* Reject rights with any unknown endpoint bit (defense in depth; the glue
     * also checks). */
    if ((rights0 & ~(uint32_t)JE_EP_RIGHTS_ALL)
        || (rights1 & ~(uint32_t)JE_EP_RIGHTS_ALL)) return JE_EP_BADINDEX;

    /* Find a FREE record (lowest index). */
    je_ep_channel *rec = (je_ep_channel *)0;
    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        if (!pool->rec[i].allocated) { rec = &pool->rec[i]; break; }
    }
    if (!rec) return JE_EP_NOSLOTS;   /* pool exhausted */

    /* Tentatively allocate the record and open both ends. */
    rec->allocated = true;
    for (uint8_t s = 0; s < 2u; ++s) {
        rec->end[s].state = JE_EP_END_OPEN;
        rec->end[s].handle_refs = 0u;
        rec->end[s].inflight_refs = 0u;
        rec->end[s].side = s;
    }

    /* Mint end 0: retain then alloc. retain of a fresh end cannot overflow. */
    je_handle h0 = 0u, h1 = 0u;
    je_ep_status rs = retain_handle(&rec->end[0]);
    if (rs != JE_EP_OK) { unwind_record(rec); return rs; }   /* unreachable */
    je_handle_status a0 = je_handle_alloc(t0, (void *)&rec->end[0],
                                          JE_ENDPOINT_OBJ_TYPE, rights0, &h0);
    if (a0 != JE_HANDLE_OK) {
        unwind_record(rec);                /* record never observed */
        return hstat(a0);
    }

    /* Mint end 1 similarly. */
    rs = retain_handle(&rec->end[1]);
    if (rs != JE_EP_OK) {                   /* unreachable; unwind end 0 fully */
        (void)je_handle_close(t0, h0);
        unwind_record(rec);
        return rs;
    }
    je_handle_status a1 = je_handle_alloc(t1, (void *)&rec->end[1],
                                          JE_ENDPOINT_OBJ_TYPE, rights1, &h1);
    if (a1 != JE_HANDLE_OK) {
        /* Roll back the whole transaction: close h0, drop both retains, free. */
        (void)je_handle_close(t0, h0);
        unwind_record(rec);
        return hstat(a1);
    }

    pool->allocated_count++;
    *out0 = h0;
    *out1 = h1;
    return JE_EP_OK;
}

/* ---- close seam ----------------------------------------------------------- */

je_ep_status je_endpoint_handle_close(je_endpoint_pool *pool,
                                      je_handle_table *t, je_handle h)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    je_ep_status s = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (s != JE_EP_OK) return s;
    /* Invalidate the table slot FIRST (so no LIVE slot ever names a freed end),
     * then drop the handle_ref. Under IF=0 a resolved handle always closes. */
    je_handle_status c = je_handle_close(t, h);
    if (c != JE_HANDLE_OK) return hstat(c);   /* defensive; not expected */
    release_handle(pool, rec, side);
    return JE_EP_OK;
}

/* ---- dup seam (retain-before-commit; endpoints never use dup_self) -------- */

je_ep_status je_endpoint_handle_dup(je_endpoint_pool *pool, je_handle_table *t,
                                    je_handle src, uint32_t new_rights,
                                    je_handle *out)
{
    if (out) *out = 0u;
    if (!pool || !t || !out) return JE_EP_BADINDEX;

    /* Resolve src as an endpoint and read its rights, preserving the table's
     * BADINDEX -> STALE -> WRONGTYPE precedence. */
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    je_ep_status rs = je_endpoint_resolve(pool, t, src, &rec, &side);
    if (rs != JE_EP_OK) return rs;

    uint32_t src_rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, src, JE_ENDPOINT_OBJ_TYPE, 0u,
                                              &src_rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);

    /* Subset check: every requested bit must be held (equality allowed). */
    if (new_rights & ~src_rights) return JE_EP_DENIED;

    /* Acquire BEFORE commit (ADR 0009 D8). Overflow acquires nothing. */
    je_ep_status ac = retain_handle(&rec->end[side]);
    if (ac != JE_EP_OK) return ac;   /* JE_EP_REFOVERFLOW */

    je_handle nh = 0u;
    je_handle_status a = je_handle_alloc(t, (void *)&rec->end[side],
                                         JE_ENDPOINT_OBJ_TYPE, new_rights, &nh);
    if (a != JE_HANDLE_OK) {
        /* Roll back the retain. src still holds a handle_ref, so this can never
         * drop the end to 0 / trigger destruction. */
        rec->end[side].handle_refs--;
        return hstat(a);
    }
    *out = nh;
    return JE_EP_OK;
}

/* ---- process-death release walk (A3-1) ----------------------------------- */

int je_endpoint_release_table(je_endpoint_pool *pool, je_handle_table *t)
{
    if (!pool || !t) return -1;
    int released = 0;
    /* Reading t->capacity is table METADATA (public struct field), not slot
     * internals; slot contents are reached only via the read-only iterator. */
    for (uint32_t i = 0; i < t->capacity; ++i) {
sed: can't read kernel/core/endpoint.h: No such file or directory

 succeeded in 0ms:
# ADR 0009: Freeze the M3c bounded-capability-IPC contract

Status: **proposed**, 2026-10-18. Scope: M3c only (issue
[#8](https://github.com/auto44e/jeios/issues/8)). No kernel code accompanies this
ADR; it converts the M3c proposals into reviewed decisions that a sequence of
small PRs implements and tests against, exactly as ADR 0007 fronted M3a and ADR
0008 fronted M3b. Reviewed baseline: `main` at the M3b attenuation merge
(`956bbcb`); M3b (the typed handle table, syscalls `self`/`close`/`query`/
`rights`/`dup`, and ring-3-observable rights enforcement) is complete per
[IMPLEMENTATION.md](../IMPLEMENTATION.md). Two decisions in this ADR are
**owner-gated** and called out explicitly (D-OWNER-1, D-OWNER-2).

## Context

M3b gave each process a handle table that can name objects, but the only object
kind is **SELF** — immortal, resource-free, no pointer. M3c introduces the first
**destroyable, transferable** kernel object: an **endpoint** (one end of a
two-ended **channel**), the substrate for bounded message IPC between the two
user processes. This is the milestone the handle table has been built toward: it
exercises the refcount/destruction machinery ADR 0008 deliberately deferred, and
it is the first place a handle names something that can go away.

The design is deliberately minimal and fail-closed, matching M3a/M3b: single CPU,
IF=0 on the syscall path, two processes max (`JE_VM_MAX_PROCESSES = 2`), no SMP,
no supervisor. The claim is "experimental single-CPU bounded capability IPC
between two processes, qualified by test," never a general message bus, a service
framework, or a restartable supervisor.

M3c is too contract-heavy to build safely without this ADR: it adds object
lifetime with destruction, a transfer transaction, and blocking/wake semantics,
each of which has invariants that are cheap to freeze on paper and expensive to
discover in code. This ADR freezes them; the PR sequence (D-SEQ) implements them.

## Decision

### D1. Scope and security claim

M3c delivers: bounded **endpoints/channels** with a per-object reference count and
destruction on last reference; **copied, bounded, data-only messages**
(`try_send`/`try_recv`, non-blocking first); **blocking waits** with deadline and
cancellation (later PR); **transactional, attenuated capability transfer** of an
endpoint handle to the other process (later PR); and a **fixed-plan bootstrap
fixture** (not an init/supervisor). Retained from M3a/M3b: one CPU, IF=0, the
per-process handle table, the v0 syscall gate, `copy_from/to_user`. **Not** in
M3c: SMP, dynamic service discovery, endpoint restart-in-place, more than two
processes, a general supervisor, or any stable ABI promise.

### D2. The endpoint/channel object and its storage

A **channel** is one kernel record holding **both ends**:

```
channel { generation; end[2] { state; refs; queue[JE_EP_QUEUE_DEPTH]; } }
```

- A handle's bound object pointer is `&channel->end[i]`; the **peer** is
  `end[i ^ 1]`, in the **same record**, so a peer pointer can never dangle.
- Channels live in a fixed **kernel pool** (`JE_EP_POOL_CAP` records), not inline
  in `je_vm_process` and not the general heap — a bounded pool makes exhaustion a
  defined `NOSLOTS`-style failure and keeps accounting simple.
- An end is in exactly one logical state: `OPEN`, `PEER_CLOSED` (the other end is
  gone), or `CLOSED`.
- **Close semantics with multiple owning refs (frozen):** `endpoint_close` drops
  ONE handle's owning ref. The end becomes `CLOSED` and signals the peer only when
  its **handle-ref count reaches 0** (no duplicate/transferred handle to it
  remains); queued-capability and waiter refs keep the *channel record* alive for
  reclamation but do not keep a fully-unhandled end `OPEN`. On the last handle
  closing an end: the peer end transitions `OPEN → PEER_CLOSED`; the closing end's
  **inbound** queue is discarded (its messages can never be read), and any
  **capabilities** in that discarded queue are released (D3). A peer doing `recv`
  on a `PEER_CLOSED` end **drains remaining queued messages first, then reports
  EOF**; a `send` to a `PEER_CLOSED` end fails with `PEER_CLOSED`. The channel
  *record* is freed only when D3's full condition holds.
- **Non-exclusive receive (stated so nothing claims point-to-point):** because
  transfer is mint/copy (D8), two processes may hold handles to the *same* end and
  both `recv` from it. M3c does not promise exclusive receive; a message is
  delivered to whichever holder dequeues it first.

### D3. Reference counting, ownership, and destruction

Each end carries **two** counts, frozen as separate fields so a later PR never has
to split a single one (which would be a tested-code change):

- **`handle_refs`** — LIVE handle slots naming the end (acquired at mint incl.
  `dup`/transfer, released at close). Drives the end's `OPEN`/`CLOSED` state (D2).
- **`inflight_refs`** — non-handle owning references: queued capabilities (an end
  transferred into a queue) and registered waiters. Keep the channel *record*
  alive for reclamation but do **not** keep the end `OPEN`.

**Pool-record ownership is separate from these counts** (resolving the earlier
circularity): the pool slot is allocated at `channel` creation and **freed when
BOTH ends have `handle_refs == 0` AND `inflight_refs == 0`** — i.e. no handle, no
queued cap, no waiter references either end. The record is not itself a counted
"reference"; it is the thing being reference-counted.

The owning references are enumerated and frozen; nothing else may hold an end (or
a waiter's target) across a syscall boundary:

1. a **LIVE handle slot** → `handle_refs` (mint/close);
2. a **queued capability** → `inflight_refs` (transfer PR; data-only until then);
3. a **waiter** → `inflight_refs` on the end, AND the waiter record itself holds a
   reference to the **blocked thread/process** that MUST be unlinked/tombstoned by
   process death before that thread's storage is reused (see D7) — a waiter→thread
   pointer is NOT protected by the endpoint ref alone, so D-OWNER-1's
   "every alias is owning" is made true only by this explicit unlink rule.

`resolve_slot()` stays a **borrowed** lookup for non-blocking ops. **No borrowed
end pointer may be held across anything that can reschedule or call
`je_vm_destroy`** (a `copy_*_user` fault is always fixed up and never routed to
fault-death or the scheduler — frozen here, relying on the M3a exact-RIP fixup).
Any op that makes an end outlive the syscall (queue a message/cap, register a
waiter, mint a handle via `dup`/transfer) MUST **acquire its owning ref before it
commits** and **release it on every failure path** (ADR 0008 D8). A woken thread
uses the **waiter's owned ref**, never a re-resolve of the original handle.
Because an end with a LIVE handle always holds `handle_refs ≥ 1`, an object a
process can still name cannot be destroyed — handles cannot dangle by construction.

### D4. Reference bound and overflow (testable, policy not hardware)

Acquiring an owning reference past a **configured** `JE_EP_REF_MAX` fails with
`JE_ABI_H_REFOVERFLOW`, **acquires nothing, and leaves the object unchanged** (it
does NOT saturate-and-continue — a stuck-at-max count would silently leak). This
is a deliberately small policy bound (not a `uint32` limit, which 2 processes
could never reach) so the overflow path is reachable in tests, and is documented
as policy, never a hardware maximum. `JE_EP_REF_MAX` MUST be ≥ the largest count
of simultaneous *non-handle* refs the design can force (so a legitimate waiter or
queued-cap acquire cannot spuriously overflow); the lifecycle PR pins the exact
value with a `_Static_assert` tying it to `JE_EP_QUEUE_DEPTH` + max waiters.

### D4a. Endpoint rights (frozen; distinct from SELF's INSPECT)

Endpoints need their own rights so attenuation (D8) and the per-op checks mean
something. Frozen as new bits in the rights ABI (appended to `JE_ABI_RIGHTS_KNOWN`
by the lifecycle PR, pinned by `_Static_assert`): **`SEND`**, **`RECV`**,
**`TRANSFER`** (may grant the end to another process), and reuse of **`INSPECT`**
(may read metadata/rights). `endpoint_create` returns the creator a handle with
**all four**. `try_send` requires `SEND`, `try_recv` requires `RECV`, transfer
requires `TRANSFER` on the source. `handle_dup`/transfer may only ever produce an
**equal-or-weaker** subset (ADR 0008), so e.g. a `RECV`-only end can be handed out
without granting `SEND` or `TRANSFER`. `close` requires **no** right (as for SELF,
so dropping all rights never leaks a slot).

### D5. Accounting (preserve-accounting acceptance)

A transferred end outlives its creator, so charging it to the creator's quota
would leak the charge when the creator dies. Therefore:

- a **pool-global** bound (`JE_EP_POOL_CAP`) caps total live channels;
- a **per-process creation quota** caps how many a process may create;
- the charge is **released when the channel record is destroyed**, regardless of
  which process holds the ends at that point.

`try_send` with a full queue → a defined `QUEUE_FULL`; pool exhaustion on create →
a defined `NOSLOTS`-style status; neither mutates state.

### D6. Message copy ordering (cheap rollback)

Messages are **fixed-max-size, copied, data-only** (no capabilities until the
transfer PR). The copy order makes rollback free and is frozen:

- **Send:** `copy_from_user` into a **kernel staging buffer**, THEN enqueue. A
  fault during the copy mutates no queue.
- **Recv:** peek, `copy_to_user`, THEN dequeue. A fault leaves the message queued.
- **Short buffer:** return the required length in RDX and do **not** dequeue
  (truncation would be silent data loss) — a defined `SHORT_BUFFER` status.
- **Cross-address-space:** there is **no** direct copy between address spaces. Send
  copies into the kernel queue in the sender's context; recv copies out in the
  receiver's own context. This resolves user-buffer validity without pinning
  pages. A blocked receiver's buffer is **re-validated at copy time after wake**
  (not merely prevalidated before blocking); a copy fault there is recovered by the
  M3a exact-RIP fixup and surfaced as **`JE_ABI_INVALID`** (the existing
  recovered-copy-fault status — there is no `EFAULT` in this ABI), leaving the
  message queued (no dequeue on a faulting copy-out).

### D7. Blocking / wait state machine (single terminal result)

Non-blocking `try_send`/`try_recv` come first (D-SEQ). Blocking is a later PR and
adds a bounded **waiter** with exactly one terminal transition producing the
syscall result:

```
WAITING -> WOKEN | TIMED_OUT | CANCELLED | PEER_CLOSED
```

Frozen for the waits PR:

- **Deadline mandatory, bounded.** No unbounded block; the deadline is in timer
  ticks (the existing M2 clock), and the **timer ISR performs the waiter's
  terminal transition itself** (it must do the `WAITING→TIMED_OUT` transition and
  deliver the result, not merely mark the thread READY — `sched.c` already wakes
  SLEEPING threads from the ISR).
- **Tie-break / linearization (one result, never two).** If more than one trigger
  is eligible at once, priority is fixed: **`PEER_CLOSED` > `WOKEN` > `CANCELLED`
  > `TIMED_OUT`**, and if a message is already queued when a receiver would block,
  it does **not** block (immediate `WOKEN`-equivalent delivery). The transition is
  a single compare-and-set on the waiter state under IF=0, so exactly one path
  wins and the losers are no-ops.
- **Receiver-only blocking in v1.** `recv` may block; **`send` does NOT block** in
  M3c (a full queue returns `QUEUE_FULL` immediately), so there is no blocked-
  sender staging-buffer lifetime to manage. (Blocking send is a later milestone.)
- **Process death unlinks waiters (D3.3).** Destroying a process must remove or
  tombstone every waiter it owns *before* the thread/frame storage is reused, so a
  later wake/timeout cannot write stale memory. This, plus registering a waiter
  edits the scheduler, means the **waits PR touches `sched.c` (tested code) and is
  therefore OWNER-GATED**, like the death-path PR (D-OWNER-2) — it is an isolated,
  owner-signed PR, not bundled.

IF=0 makes the check/enqueue/block sequence atomic on this CPU, but the ADR still
requires the timeout-vs-wake-vs-cancel linearization above to be specified and
tested; a blocked thread enters a new `THREAD_BLOCKED_IPC` scheduler state.

### D8. Transfer (later PR; frozen here even though deferred)

Capability transfer grants an endpoint handle to the **other** process. It is a
**destination-side mint**, never a raw handle copy (ADR 0008's security model),
and it is **transactional**: reserve the destination slot → validate+attenuate
rights → **acquire the object reference before the destination commit** (D8 of
ADR 0008) → commit; **roll back every reservation/reference on any failure**
(destination table full, target not live, rights superset, refoverflow). Whether
transfer is a **move** (consumes the source handle) or a **mint/copy** (source
keeps a handle) is frozen as: **mint/copy with independent attenuation** — the
sender keeps its (possibly stronger) handle; the receiver gets a fresh,
independently-closable, equal-or-weaker handle to the same end. Messages stay
**data-only until this PR lands**, because a capability in a queue is a new owning
reference (D3.2) and a capability graph needs its cycle invariant (D9) frozen
first.

### D9. Capability-graph cycle safety (frozen before any cap may be queued)

A transferred end sitting in another end's queue is an owning reference, so
unmanaged queued capabilities could form a reference cycle that never reaches
`refs == 0` (a leak, not a UAF, given D3's owning-ref rule — but still a defined
failure to prevent). The transfer PR MUST ship a stated graph invariant that
bounds nesting; candidate invariants were debated but **none is adopted yet** —
the transfer PR owns proving one. Until then (D8), messages are data-only, and
that is **enforced, not just prose**: the message wire format carries a `cap_count`
field (and reserved flag bits), and the non-blocking-IPC PR (D-SEQ step 4) rejects
any send with `cap_count != 0` or nonzero reserved bits as `JE_ABI_INVALID`. So a
capability can never be queued before D9's invariant and the transfer transaction
(D8) exist, and no cycle can form before its invariant is frozen.

### D-SEQ. PR sequence (each small, independently testable, mergeable)

1. **ADR 0009** (this document; owner-accepts D-OWNER-1/2).
2. **Lifecycle (kernel-only create):** the endpoint pool, channel record, the
   **split refcount** (D3: handle-refs vs in-flight-refs), retain/release,
   `endpoint_close`, `_REFOVERFLOW`, and `handle_dup` gaining
   **retain-before-commit** for the endpoint type (the M3b dup path was kept
   SELF-only precisely so this PR adds the destroyable-object retain). Endpoints
   are created ONLY by in-kernel direct tests here — **no ring-3 `endpoint_create`
   yet** (D-OWNER-2 safety). Tests: explicit-close, direct pool/refcount/
   destruction-on-last-ref, `_REFOVERFLOW` at `JE_EP_REF_MAX`.
3. **Process-death release walk + ring-3 `endpoint_create` (owner-signed):** wire
   `release_all` into `je_vm_destroy` (the owner-gated death path) AND expose the
   `endpoint_create` syscall. These ship **together** so a user process can create
   an endpoint only once its death reclaims it. QEMU evidence: fault a holder,
   assert the peer is closed and pool usage returns to baseline; repeated
   fault/exit cycles do not exhaust the pool.
4. **Non-blocking data IPC:** `try_send`/`try_recv`, data-only (D6), with the
   enforced `cap_count == 0` / reserved-flags rule (D9) so no capability can be
   queued before its invariant exists.
5. **Capability transfer:** transactional attenuated mint, rollback-tested, and
   the D9 cycle invariant (this PR owns proving one). Flips messages from
   data-only to capability-carrying.
6. **Waits:** `THREAD_BLOCKED_IPC` + deadline + cancellation (D7). Edits `sched.c`
   (tested code) — **owner-gated**, like the death-path PR.
7. **Bootstrap fixture:** kernel injects an endpoint pair into the two processes,
   bounded ping-pong + closure, honest "fixture not supervisor."

(Codex's ordering: transfer before waits, so the lifetime/cancellation state space
isn't multiplied before transfer's semantics are frozen. Note the two owner-gated,
tested-path PRs — step 3 death path and step 6 `sched.c` block state — are each
isolated, per the D-OWNER-2 rationale.)

## D-OWNER-1. Incarnation / restart (owner decision; recommended resolution)

ADR 0008 D-RESOLVED deferred the object-wide "service-instance vs slot-generation"
question to M3c. **Resolution for M3c: terminal destruction, no restart-in-place.**
An endpoint, once destroyed, is gone; a "new instance" is always a new pool record
(the pool slot carries a `generation` ONLY as internal double-free/stale-index
defence — it is never exposed to ring 3, since handles bind `&end[i]` pointers, not
pool indices; if it ends up with no internal consumer the lifecycle PR drops it
rather than ship dead surface). **No shipped incarnation counter** — untested ABI
with no consumer.

**This decision is explicitly CONDITIONAL on D3 (review finding):** it is sound
*only because* every live alias to an end is an owning reference (handle, queued
cap, or waiter) and **no escaped raw pointer to an end may survive its release**
(the borrowed-pointer rule in D3). Terminal destruction + mint-fresh destination
handles (D8) then leaves no cross-process stale *handle* possible. If a later
milestone introduces a non-owning alias or restart-in-place, the incarnation
question reopens with its own ADR. Given D3 as frozen, this closes the ADR 0008
incarnation question for M3c.

## D-OWNER-2. Process-death release walk is its own owner-signed PR (the split)

Reclaiming a dying process's endpoints (a `release_all` that drops each of its
LIVE endpoint handles' refs, closing peers and freeing now-unreferenced channels)
has to run on process teardown — which lives in `je_vm_destroy` (`kernel/core/
vm.c`), the path the tested fault-death flow uses. Changing its resource-release
behaviour is a **death-path semantic change** and therefore owner-gated (ADR 0008
D7), **even if `sched.c` stays byte-identical**.

**Decision (owner-approved 2026-10-18): the release walk is its OWN separate,
owner-signed PR — it is NOT bundled into the lifecycle PR (D-SEQ step 2).** The
lifecycle PR ships the endpoint pool + refcount + `endpoint_close` + direct pool
tests, but — crucially — **does NOT expose `endpoint_create` to ring 3 until the
teardown PR lands** (see the safety note below). Endpoints in the lifecycle PR are
created only by in-kernel direct tests, never by a user process that could then
die holding one.

**SAFETY (review finding — the gap is not merely cosmetic).** `je_vm_destroy`
currently zeroes the handle table. With endpoints but no release walk, a process
dying with a LIVE endpoint handle would leave its `refs` and creation charge
**permanently unreclaimed**, and repeated fault/exit cycles would **exhaust the
fixed pool** — a recoverability/availability failure, not a bounded cosmetic leak.
Therefore the gap window must expose **no user path that creates an endpoint**:
the lifecycle PR keeps `endpoint_create` kernel-only (direct tests), and the
ring-3 `endpoint_create` syscall is introduced **in or after** the owner-signed
release-walk PR, so a user process can never die holding an endpoint before
teardown reclaims it. This makes the split safe, not just honest.

**Why the split is the right fit here (recorded per owner request):**

- **It keeps an owner-gated, previously-tested-path change isolated and legible.**
  Every tested-path edit in this project (the M3a H1 fault-death fix, the #4
  death records) has been its own reviewable change with its own QEMU evidence;
  bundling the release walk into a feature PR would hide a death-path semantic
  change inside unrelated new surface, exactly what the D7 gate exists to prevent.
- **The two changes have genuinely different risk and evidence.** Lifecycle is
  pure-ish pool/refcount logic providable by hosted + explicit-close tests; the
  release walk is a death-path change whose proof is a *QEMU fault-death* run
  (fault a holder, assert the peer is closed and pool usage returns to baseline).
  Different blast radius → different PR, so a reviewer can reason about each in
  isolation.
- **It avoids a false "done" while keeping the gap honest.** The lifecycle PR
  states "process-death cleanup not yet wired," so nothing overclaims; the gap is
  closed deliberately in the signed PR rather than assumed away.

**Why NOT bundle (the alternative considered and rejected):** the counter-argument
(Claude's) was that an unwired `release_all` helper is dead code whose only real
test is in situ, so wire it and prove it in the lifecycle PR as one owner ask. That
is sound engineering in the abstract, but here it **widens an owner-gated PR to
also carry a feature**, making the death-path change harder to review in isolation
and coupling its merge to the lifecycle feature's. The helper is not left dangling:
it lands *with* its wiring and its QEMU evidence in the very next PR, so it is never
merged as untested dead code — it is merely deferred by one small PR to keep the
gate clean. On balance the isolation of the owner-gated change outweighed the
one-PR-later cost.

## Consequences

- A later milestone needing SMP-safe endpoints, restart/supervision, >2 processes,
  or a queued-capability graph beyond D9's invariant writes a new ADR.
- The bounded pool / queue / ref / message-size values (`JE_EP_POOL_CAP`,
  `JE_EP_QUEUE_DEPTH`, `JE_EP_REF_MAX`, the message max) are frozen with concrete
  numbers in the lifecycle PR and pinned by `_Static_assert`; this ADR fixes that
  they are small, configured policy bounds, not hardware limits.
- New `JE_ABI_H_*`/IPC statuses (`QUEUE_FULL`, `SHORT_BUFFER`, `PEER_CLOSED`,
  `REFOVERFLOW`, the wait terminals) are appended to the v0 ABI namespace and
  pinned, keeping `abi.h` independent of `status.h`/`handle.h` (the glue maps).

AI-assisted design (a two-round role-assigned peer debate: implementor vs
adversarial reviewer); not independent human qualification. The owner is the merge
gate and the decider of D-OWNER-1 and D-OWNER-2 (both resolved above with owner
approval on 2026-10-18); on acceptance, D-SEQ step 2 (lifecycle) may begin.

## AMENDMENT (2026-10-18, owner-approved) — step-3 wiring decisions

Two decisions were needed to implement D-SEQ step 3 (the owner-signed process-death
release walk + typed close dispatcher + ring-3 `endpoint_create`). Both were raised
by an implementor-vs-adversarial-reviewer debate on the *wiring* and approved by the
owner; they refine, not contradict, the frozen contract above.

### A3-1. `je_vm_destroy` runs the release walk + unwind + table-zero as ONE
### IRQ-disabled critical section (owner-gated death-path behaviour change)

The release walk is invoked through a **generic teardown hook** on `je_vm`
(`void (*teardown)(void *ctx, je_handle_table *t)` + `void *teardown_ctx`,
zero-defaulted by `je_vm_init`), called inside `je_vm_destroy` **after** the
`!vm`/`state != READY` checks and **immediately before** `unwind()`. This is the
single choke point covering the reap path (`sched.c`) and every direct `main.c`
caller, and it keeps `vm.c` endpoint-agnostic (the hook is NULL in the vm unit
test, which gains a fake-hook ordering case). The pool is one kernel-owned static
in a new `endpoint_kern.c`; `je_vm` never holds an endpoint-typed pointer.

The debate found a **real tested-path hazard** in the naive "walk then unwind at
IF=0" plan: the direct `je_vm_destroy` callers in `main.c` can run with **IF=1**
(the bootstrap thread after `sti`), so a timer could preempt **between the walk
and the `*p = {0}` table-zeroing** while the peer process is live — the peer's
`HANDLE_CLOSE` would then mutate the same channel record mid-teardown (the exact
use-after-free this PR exists to prevent). **Resolution:** `je_vm_destroy`
**saves-and-disables IRQs around the entire hook+unwind+zero sequence and restores
after** — it no longer merely trusts the caller's IF=0. `vm.c` reaches the arch
IRQ-save through the same indirection pattern (a hook/stub, NULL/no-op in the unit
test) so the fake backend stays arch-free. This changes behaviour in the
owner-gated death path and is therefore owner-signed.

### A3-2. `endpoint_create` (op 10) returns BOTH handles packed in RDX

`endpoint_create` takes no handle args (all of RDI/RSI/RDX/R10/R8/R9 must be zero,
else INVALID), mints both ends into the **caller's own** table with
`JE_EP_RIGHTS_ALL`, and is transactional (both slots + both refs + the pool record
all-or-nothing; a one-slot/full-table failure leaves the pool and the caller table
unchanged). The v0 ABI gives one status (RAX) and one scalar (RDX); two 32-bit
handles are returned by **packing them into RDX**: `RDX[31:0] = h0`,
`RDX[63:32] = h1`. RDX is **undefined on failure**. This is lossless only because a
handle is 32 bits, so `abi.h` pins `_Static_assert(sizeof(je_handle) == 4)` (and
the ABI endpoint object-type / rights bits are pinned equal to the table-side
copies). Rejected alternatives: a second "claim-peer" op (leaves a half-claimed
state) and `copy_to_user` of the pair (adds a fault/partial-write rollback for no
gain). The packed shape **freezes "a handle is ≤32 bits" into the v0 ABI**; a later
milestone that needs wider handles or >2 returned capabilities writes a new ADR.

`endpoint_create` returns `JE_ABI_UNSUPPORTED` on a VM with no teardown hook
installed, so "an endpoint slot exists in a ring-3 table ⇒ that VM's `je_vm_destroy`
walks it" holds **by construction**, not by convention — the structural discharge
of endpoint.h's load-bearing precondition. Pool exhaustion on create maps to the
existing `JE_ABI_H_NOSLOTS` (no new status).

docs/adr/debates/m3c-step6-waits.md
# Peer debate — 2026-10-19T02:08:16Z
Topic: JeiOS M3c step 6 blocking-recv waits (D7): minimal correct sched.c design + proof plan + what to escalate
Rounds: 2   Roles: claude=implementor codex=adversarial-reviewer
Context file: /tmp/step6.md


[R1 CLAUDE] **Position (implementor): the Q2 "syscall loop that resumes after the park point" doesn't fit this kernel, and that settles Q1–Q3.** Verified on main @ e914a95:

- A user syscall has no kernel continuation. `je_syscall_dispatch` *returns a frame* (`syscall.c:225-259`). The user thread's whole saved state is its trap frame at `rsp0_top - sizeof(frame)` (`sched.c:174-176`). Resuming it means `iretq` on that frame. Only kernel threads park mid-function (`je_thread_sleep` → `int $0x81`, `sched.c:242`). So a blocked recv can't "return from park and re-check". **Whoever performs the terminal transition must write the result into the blocked thread's saved frame.** That is literally what D7 says ("the ISR performs the transition and delivers the result, not merely mark READY"). Q3 isn't an interpretation question: the architecture forces the ADR reading.
- Restarting the syscall (rewind RIP over `int 0x80` and re-execute) is also ruled out. The re-executed recv would re-resolve the handle, and D3 is frozen: "a woken thread uses the waiter's owned ref, never a re-resolve of the original handle."
- The copy must happen in the receiver's CR3. A sender completing the receiver's recv is running under the sender's CR3, so it can't do the copy-out.

## Recommended design

**Q1, a hybrid split.** The thread struct holds `wait_state` (WAITING or a terminal cause), the deadline (reuse `wake`) and `wait_end`. The end record holds a single back-link `waiter` (thread slot or NONE) plus the waiter's `inflight_refs` (D3 alias 3). Neither pure A nor pure B works. The end needs the back-link so a sender or peer-close can find the waiter. The thread needs the state so the ISR can scan it.

**Q2/Q3, each trigger does an eager CAS at its own instant.** IF=0 serializes every trigger, so the case where "several triggers are eligible at once" only arises inside one atomic section. The priority table applies there (for example, the ISR finds the deadline passed AND the end PEER_CLOSED → PEER_CLOSED).
- **Sender:** enqueue, then CAS the peer's waiter WAITING→WOKEN, then READY.
- **Peer close or death:** when the end goes CLOSED, CAS the opposite waiter →PEER_CLOSED, write `rax`, READY.
- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.

A CAS that loses is a no-op. There is no re-check at the syscall; the CAS winner is the single source of truth.

**WOKEN delivery is deferred to dispatch.** I propose one hook in `select_and_install_next`, after the `je_uframe_check` and CR3 load. It runs for a thread with a pending completion and calls `je_ep_wait_finish(slot)` (endpoint.c). That function:
- unlinks the end's back-link;
- releases the waiter's inflight ref;
- for WOKEN, runs the ordinary try_recv commit (copy-out with re-validation and fixup, cap mint), so a copy fault becomes an error result, as D7 requires.

The timeout and peer-closed paths can also defer their unlink and ref release to this hook. That keeps endpoint-pool frees out of ISR context. The ISR still writes the final status itself, so it stays faithful to D7.

**New hazard: theft between the WOKEN stamp and dispatch.** A5 transfer lets both processes hold the same end. The other process can be scheduled first and `try_recv` the message that WOKEN promised. Two fixes:
- **(a) Reservation (my pick).** While a WOKEN completion is pending, the end's head message is reserved, so other receivers see `EMPTY`. No new ABI.
- **(b)** Dequeue into a per-waiter kernel staging buffer. This costs JE_THREAD_MAX × max message size, and putting the message back after a copy fault gets messy.

Also: **at most one waiter per end.** A second blocking recv needs a status code (open question below).

**Q4, both kinds of work are needed:**
1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
2. **Peer death or close.** This must actively wake the receiver with PEER_CLOSED through the close hook above. A plain `handle_close` that drops `handle_refs` to 0 must trigger it too, not only death.

**Q6, the minimal sched.c diff:**
- one enum value;
- three thread fields;
- `je_sched_block_current_ipc(f, deadline, end)` (sets state, returns the successor frame);
- `je_sched_ipc_complete(slot, cause, rax, rdx)` (CAS, frame write, READY);
- the ISR scan branch;
- the dispatch hook call;
- a `je_sched_unlink_ipc(vm)` that destroy calls.

Everything else lives in endpoint.c and syscall.c: the queue check, the immediate-delivery path, the reservation, the copy and the priority choice. Also bump the D4 `_Static_assert`: `JE_EP_REF_MAX` must be ≥ queue depth plus max waiters.

## Proof plan (deterministic; nothing spins in ring 3)

The bootstrap kernel thread drives every test and observes state through accessors. A user stub never waits on another user stub.

| Cause | Proof |
|---|---|
| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
| TIMED_OUT | The user blocks with deadline N and nothing sends. After N ticks, assert `rax=TIMED_OUT` in the resumed thread. |
| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
| PEER_CLOSED | Bootstrap destroys the peer vm while the receiver is blocked. |
| Same-instant tie | Inside one IF=0 section, enqueue then run the tick path by hand (deadline already passed). Assert WOKEN. Do the same with peer-close plus deadline and assert PEER_CLOSED. |
| Receiver death | Destroy the blocked receiver, then send. Assert no wake, the back-link is cleared and the ref count returns to baseline. |
| Theft | The other holder calls `try_recv` while a WOKEN completion is pending and gets `EMPTY`. |
| Copy fault after wake | Needs a proof-only hook that poisons the saved buffer pointer, because nothing in M3c can unmap the buffer. Flag it as an injected fault, not organic. |

Also add a host `make check` table test of the CAS and priority function.

## Escalate to the owner

1. **The D7 reading is forced by the frame-return architecture**, so the owner should confirm it rather than choose it. The ISR writes the TIMED_OUT result into the frame; dispatch-time completion handles copy-out, unlink and ref release; there is no syscall re-check. Owner sign-off is needed on the dispatch hook in `select_and_install_next`, which is tested code.
2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
3. **New ABI surface:** a blocking-recv op number, the timeout register (R10 is currently reserved=0 on try_recv), a `JE_EP_WAIT_MAX_TICKS` cap, and whether a timeout of 0 means try_recv or INVALID.
4. **A status for a second waiter on the same end**, and the choice between reservation and staging for the theft hazard. This is open for an agent majority vote unless the owner wants it.
[R1 CODEX] codex
Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
But several parts of the proposal are not yet contract-safe.
1. Q1: avoid the endpoint back-link initially
Use a thread-owned waiter:
- `wait_state`
- `wait_end` (protected by an owned endpoint reference)
- deadline
- saved receive arguments / completion metadata
- pending completion flag
Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
- stale endpoint-to-thread IDs;
- a second registry lifecycle;
- silently imposing “one waiter per endpoint,” which D7 does not authorize;
- extra unlink work during process death.
A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
2. Q2: no syscall re-check loop
The syscall path should be:
1. Check queue/closed state.
2. If immediately deliverable, complete synchronously.
3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
4. A terminal transition writes scalar results into the saved frame.
5. On selection of the thread, a resume-completion hook performs the owned-endpoint receive commit and copy-out under the receiver’s CR3.
The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
The WOKEN state and the eventual syscall result need separate concepts. A post-wake user-buffer fault must preserve the terminal wait cause while returning the specified normal copy error.
3. Q3: Claude overstates that the interpretation is settled
The architecture settles “no syscall loop,” but not the exact meaning of D7’s “ISR performs the terminal transition.”
The timer path must not implement:
```text
WAITING -> TIMED_OUT
```
merely as a timeout hint followed by a syscall re-check. The ISR must invoke the terminal-transition helper and install the timeout result itself.
The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
There is a second unresolved case: sender enqueues and marks WOKEN, then peer death occurs before the receiver runs. Both WOKEN and PEER_CLOSED are now eligible. “First CAS wins” gives WOKEN; the stated priority could imply PEER_CLOSED. The ADR needs an owner decision on whether priority applies only to simultaneous eligibility inside one critical section, or whether a later higher-priority trigger may supersede a prior terminal transition. Superseding would conflict with the single-terminal-transition rule, so the likely safe interpretation is “priority at one arbitration point,” but that must be recorded.
A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
4. Q4: teardown requires more than changing `DYING`
For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
For peer death, the endpoint release walk must actively wake affected receivers with `PEER_CLOSED`; merely setting endpoint state is insufficient because a far deadline delays re-entry indefinitely.
The close path must distinguish “this process died” from “the peer side is actually closed.” Shared/transferred capabilities may mean peer death does not immediately imply closure.
5. Q5: deterministic proof set
Use one-shot kernel/user actors and explicit yields. No ring-3 spin loops.
- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
- WOKEN: receiver blocks; a scheduled sender performs one send and yields/exits; verify reserved payload and status.
- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
- PEER_CLOSED: receiver blocks; destroy/close the peer; verify active wake and result.
- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
- Copy fault: use an available VM mapping/unmapping test hook; otherwise mark this as unverified integration coverage and test the copy-fixup path separately.
- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
Claude’s proposed `RAX=...`, `RDX=0`, timeout register, and `JE_EP_REF_MAX` assertion are not established by the supplied contract. They require ABI/source verification or owner approval.
6. Minimal sched.c change
The semantic minimum is:
- add `THREAD_BLOCKED_IPC`;
- add thread-owned waiter fields;
- add `je_sched_block_ipc(...)`;
- add one compare-and-set terminal helper used by sender, close, cancel, and timer;
- add the timer scan invoking that helper;
- add teardown unlink/tombstone before slot reuse;
- add one common return-path hook for deferred owned-endpoint completion.
Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
Owner escalation should be limited to:
1. Whether deferred WOKEN copy-out before `iretq` satisfies D7’s delivery requirement.
2. Priority semantics when a lower-priority terminal cause wins before a later higher-priority trigger becomes eligible.
3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
4. The exact v1 ABI for deadline, result registers, zero timeout, and copy-fault status.
5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
But several parts of the proposal are not yet contract-safe.
1. Q1: avoid the endpoint back-link initially
Use a thread-owned waiter:
- `wait_state`
- `wait_end` (protected by an owned endpoint reference)
- deadline
- saved receive arguments / completion metadata
- pending completion flag
Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
- stale endpoint-to-thread IDs;
- a second registry lifecycle;
- silently imposing “one waiter per endpoint,” which D7 does not authorize;
- extra unlink work during process death.
A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
2. Q2: no syscall re-check loop
The syscall path should be:
1. Check queue/closed state.
2. If immediately deliverable, complete synchronously.
3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
4. A terminal transition writes scalar results into the saved frame.
5. On selection of the thread, a resume-completion hook performs the owned-endpoint receive commit and copy-out under the receiver’s CR3.
The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
The WOKEN state and the eventual syscall result need separate concepts. A post-wake user-buffer fault must preserve the terminal wait cause while returning the specified normal copy error.
3. Q3: Claude overstates that the interpretation is settled
The architecture settles “no syscall loop,” but not the exact meaning of D7’s “ISR performs the terminal transition.”
The timer path must not implement:
```text
WAITING -> TIMED_OUT
```
merely as a timeout hint followed by a syscall re-check. The ISR must invoke the terminal-transition helper and install the timeout result itself.
The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
There is a second unresolved case: sender enqueues and marks WOKEN, then peer death occurs before the receiver runs. Both WOKEN and PEER_CLOSED are now eligible. “First CAS wins” gives WOKEN; the stated priority could imply PEER_CLOSED. The ADR needs an owner decision on whether priority applies only to simultaneous eligibility inside one critical section, or whether a later higher-priority trigger may supersede a prior terminal transition. Superseding would conflict with the single-terminal-transition rule, so the likely safe interpretation is “priority at one arbitration point,” but that must be recorded.
A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
4. Q4: teardown requires more than changing `DYING`
For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
For peer death, the endpoint release walk must actively wake affected receivers with `PEER_CLOSED`; merely setting endpoint state is insufficient because a far deadline delays re-entry indefinitely.
The close path must distinguish “this process died” from “the peer side is actually closed.” Shared/transferred capabilities may mean peer death does not immediately imply closure.
5. Q5: deterministic proof set
Use one-shot kernel/user actors and explicit yields. No ring-3 spin loops.
- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
- WOKEN: receiver blocks; a scheduled sender performs one send and yields/exits; verify reserved payload and status.
- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
- PEER_CLOSED: receiver blocks; destroy/close the peer; verify active wake and result.
- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
- Copy fault: use an available VM mapping/unmapping test hook; otherwise mark this as unverified integration coverage and test the copy-fixup path separately.
- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
Claude’s proposed `RAX=...`, `RDX=0`, timeout register, and `JE_EP_REF_MAX` assertion are not established by the supplied contract. They require ABI/source verification or owner approval.
6. Minimal sched.c change
The semantic minimum is:
- add `THREAD_BLOCKED_IPC`;
- add thread-owned waiter fields;
- add `je_sched_block_ipc(...)`;
- add one compare-and-set terminal helper used by sender, close, cancel, and timer;
- add the timer scan invoking that helper;
- add teardown unlink/tombstone before slot reuse;
- add one common return-path hook for deferred owned-endpoint completion.
Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
Owner escalation should be limited to:
1. Whether deferred WOKEN copy-out before `iretq` satisfies D7’s delivery requirement.
2. Priority semantics when a lower-priority terminal cause wins before a later higher-priority trigger becomes eligible.
3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
4. The exact v1 ABI for deadline, result registers, zero timeout, and copy-fault status.
5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
[R2 CLAUDE] I checked the repo at e914a95. Several of Codex's open questions are already answered by frozen text or merged code, so the escalation list can shrink. I agree with most of Codex's corrections. I dispute two: its timer-path rule and how open the priority question is. It also missed two hazards.

## Agree
- **Thread-owned waiter plus a bounded scan of `JE_THREAD_MAX`, no endpoint back-link.** ADR 0009 D3 item 3 says a waiter holds an `inflight_refs` on the end and a reference to the blocked thread, which must be unlinked before the thread's storage is reused. If the waiter lives in the thread slot, that thread reference points to itself and can never dangle. Teardown then only has to clear the slot's waiter and release its inflight ref.
- **The resume path needs an owned-ref commit, not plain `try_recv`.** This is verified: `je_endpoint_recv_peek` calls `je_endpoint_resolve(pool, t, h, …)` (endpoint.c ~496), which re-resolves the handle. D3 forbids that for a woken thread. We need a variant keyed on `(rec, side)` that reuses the existing `je_ep_recv_lease` two-phase commit.
- **Peer death must actively wake a blocked receiver.** The hook belongs at the refcount-reaches-zero transition (endpoint.c:208, `peer->state = JE_EP_END_PEER_CLOSED`), not at process death. That already separates "process died" from "end closed" when capabilities are shared or transferred, so it needs no extra design.
- **CANCELLED has no v1 trigger.** `je_thread_create_user(vm, index)` and the slot lookups (sched.c:295/353) give one user thread per process. The only thread that could close the waited handle is the blocked thread itself. The receiver's own death tombstones the waiter and delivers no result, so that is not CANCELLED either. Ship an internal helper with a unit test and document it as unreachable from the ABI.

## Dispute
1. **The timer ISR should not judge eligibility itself.** Make it a structural invariant instead: under IF=0, every enqueue and every peer-close transition calls the wake helper in the same critical section. So a WAITING waiter can never coexist with a deliverable message or a peer-closed state. The timer then does a plain `CAS WAITING→TIMED_OUT` and writes the result into the saved frame, with an assert-or-`JE_EP_CORRUPT` check on the invariant. If the ISR evaluated eligibility, it would have to reach into the endpoint pool, which is exactly the coupling we are keeping out of tested `sched.c`. TIMED_OUT is a plain status code, so the ISR fully delivers it. D7's "ISR performs the transition and delivers the result" is met literally for TIMED_OUT.
2. **"WOKEN, then the peer dies" has one safe reading, fixed by existing code.** Merged `try_recv` drains queued messages before reporting PEER_CLOSED (endpoint.c:517-520: PEER_CLOSED only when `count == 0`). Read literally, PEER_CLOSED > WOKEN would discard a queued message, and the merged step-4 semantics don't allow that. So the priority applies at a single arbitration point, and PEER_CLOSED counts as eligible only when the inbound queue is empty. Record this as a clarification for the owner to confirm, not as an open design question.
3. **Codex's ABI escalation #4 is partly answered already.** D6 freezes the copy-fault status: `JE_ABI_INVALID`, message stays queued, no dequeue on a faulting copy-out. Only the deadline register, zero-timeout behaviour and the maximum deadline remain open.
4. **Codex's multi-waiter escalation #3 is partly answered already.** `JE_EP_MAX_WAITERS = 2` exists (endpoint.h:56), and endpoint.c:13 asserts `REF_MAX >= QUEUE_DEPTH + MAX_WAITERS`, so two waiters per end is the configured design. A third waiter fails at its ref acquire. What needs a decision is the status a waiter beyond the limit gets.

## What Codex missed
- **Reservation interacts with copy faults.** The frozen fault rule leaves the message queued. If waiter A is WOKEN with message 0 reserved and its copy-out faults, A's terminal result is WOKEN with status `JE_ABI_INVALID`. Message 0 is then unreserved but waiter B is still WAITING next to a deliverable message, which breaks the invariant above. The fault-release path must re-run the wake helper for that end.
- **Reserving more than one message breaks the queue.** Out-of-order completion would dequeue from the middle of the ring. Rule: an end has at most one outstanding reservation, always on the head message, and the next waiter is woken only after the current one commits or releases. That keeps FIFO order without restructuring the queue.
- **Where the completion hook runs is the riskiest part of the `sched.c` change.** Dispatch is frame-return based (sched.c:140-159: pick, `je_uframe_check`, `cr3_load`, return the frame). The deferred copy-out has to run after `je_arch_cr3_load(threads[next].root)` and before the return, so it runs inside the scheduler. ADR D3 relies on the M3a exact-RIP fixup never routing a copy fault to the scheduler, but nobody has tested a fault taken from within `je_sched_interrupt`. That needs a dedicated test (unmap the receive buffer while the receiver is blocked) or an owner-accepted "unverified" label. The alternative of re-executing the syscall (rewinding RIP) is out, because it re-resolves the handle.
- **The deadline needs explicit bounds.** "Bounded" requires a maximum (for example a `JE_IPC_WAIT_MAX_TICKS` constant), an overflow-checked `ticks + duration`, and the inclusive comparison `wake <= ticks` that `je_thread_sleep` already uses, so the two clocks behave the same.

## Escalate to the owner (four items)
1. Whether deferred WOKEN copy-out, performed in the dispatch hook after the receiver's CR3 load, satisfies D7's "deliver". Also whether the exact-RIP fixup is approved for use inside `je_sched_interrupt`.
2. Confirm that priority is decided at one arbitration point and that PEER_CLOSED is eligible only on an empty queue, consistent with the merged drain-first behaviour.
3. CANCELLED in v1: internal helper with a unit test only, documented as unreachable from the ABI.
4. The remaining ABI details: deadline register, behaviour of a zero timeout, the maximum deadline, and the status for exceeding `JE_EP_MAX_WAITERS`.
[R2 CODEX] codex
Claude is right on several implementation constraints, but its proposal still leaves the central D7 arbitration underspecified.
Agree:
- Thread-owned waiter metadata minimizes `sched.c` churn, but it does not eliminate endpoint→scheduler wake coupling. Endpoint enqueue/close paths still need a bounded waiter scan or scheduler wake API.
- Peer death must actively wake blocked receivers at the endpoint close transition, not wait for their deadline.
- One outstanding head reservation per endpoint is required. Otherwise FIFO dequeue and copy-fault rollback become ambiguous.
- Deadline arithmetic needs an explicit bounded maximum, overflow checking, and the existing inclusive `wake <= ticks` rule.
- `CANCELLED` has no current ABI trigger. An internal cancellation helper plus a deterministic kernel test is reasonable, but its ABI unreachability must be documented.
Dispute:
1. The timer’s CAS cannot be justified merely by an invariant assertion.
A timer path that blindly performs `WAITING → TIMED_OUT` is correct only if every path that creates eligibility synchronously performs the competing transition first:
- enqueue: `WAITING → WOKEN`;
- peer close: `WAITING → PEER_CLOSED`;
- cancel: `WAITING → CANCELLED`;
- reservation release after copy fault: re-arbitrate and wake another waiter.
The copy-fault case Claude identified proves this invariant is easy to violate. “Assert or `JE_EP_CORRUPT`” is not an acceptable normal recovery path for a kernel mechanism. The design must specify one shared `je_ipc_wait_arbitrate()` helper, called under IF=0, and test that helper after reservation release.
The timer may then perform the final timeout CAS, but only after the same arbitration rules are applied or after the invariant is mechanically enforced. Otherwise the ISR can timeout a waiter while a queued message exists.
2. “ISR stamps timeout; syscall re-checks” is not the state-machine contract.
The terminal state must be selected by one CAS. A syscall re-check may validate the already-selected result and perform copy/commit, but it must not be a second authority that changes `TIMED_OUT` into `WOKEN`. If the syscall is allowed to override the ISR result, the transition is no longer a single terminal CAS.
Conversely, sender and close paths cannot merely enqueue or mark metadata. They must perform the `WAITING → WOKEN/PEER_CLOSED` transition in the same IF=0 critical section as the endpoint mutation.
3. The proposed deferred copy-out inside scheduler dispatch is the wrong default.
Copying after `cr3_load(next)` inside `je_sched_interrupt` introduces a new page-fault context in scheduler code. The provided facts only establish exact-RIP recovery for the M3a syscall path; they do not establish recovery for a fault originating inside the scheduler after an address-space switch. That is precisely the kind of untested assembly/frame interaction this owner gate is intended to prevent.
The safer shape is:
- wake path atomically selects the waiter result and, for WOKEN, reserves the head message;
- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
- the blocked syscall continuation resumes after park;
- syscall code performs owned-record copy-out and commit/release;
- copy faults return `JE_ABI_INVALID`, preserve the message, release the reservation, and re-arbitrate.
docs/adr/debates/README.md
# Peer-debate transcripts

Raw multi-agent correspondence (`peer-debate.sh` runs, parallel consults,
adversarial reviews) that informed decisions in this project. Required by
**ADR 0000 part 4 (attached correspondence)**: a decision informed by an agent
exchange commits its transcript here and links it from the decision record, so
the reasoning is durable and auditable rather than ephemeral in `/tmp`.

Naming: `<area>-<step>-<topic>.md`. These are verbatim transcripts — the roles
(implementor / adversarial-reviewer) and rounds are as the helper recorded them.
A decision's prose quotes only what it needs; this is the full back-and-forth.

| Transcript | Informed |
|---|---|
| `m3c-step6-waits.md` | ADR 0009 A6 (blocking recv waits, D7) |

Older M3b/M3c debate transcripts (`m3b4`, `m3c`, `m3c-lifecycle`, `m3c-step3/4/5`)
are back-filled by the ADR 0000 retroactive-sweep PR and linked from their
respective decisions at that time.

exec
/usr/bin/bash -lc "git diff -- docs/adr/0009-m3c-capability-ipc.md docs/adr/0000-decision-record-policy.md; sed -n '1,260p' docs/adr/debates/m3c-step6-waits.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,180p' include/jeios/abi.h; sed -n '1,280p' include/jeios/endpoint.h; sed -n '1,260p' include/jeios/sched.h; sed -n '360,430p' kernel/core/vm.c" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '430,760p' kernel/core/endpoint.c; sed -n '1,260p' kernel/core/endpoint_kern.c; sed -n '180,460p' kernel/core/syscall.c" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "grep -RInE \"JE_EP_MAX_WAITERS|MAX_WAIT|WAITING|BLOCKED_IPC|CANCELLED|TIMED_OUT|waiter|reservation|continuation|recv\" docs include kernel tests | sed -n '1,520p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ABI_H
#define JEIOS_ABI_H
/*
 * M3a #5: experimental native syscall ABI v0 (ADR 0007 D8/D10). This is the
 * ONLY status/op numbering exposed to ring 3. It is explicitly numbered and is
 * DELIBERATELY independent of the internal <jeios/status.h> enum — the kernel
 * must never pass internal je_status numbering across the user boundary (M3A.md
 * rule 5). This header includes <stdint.h> only; a make-check grep enforces that
 * it never includes status.h.
 *
 * Call convention (int 0x80 gate, DPL3, IST=0):
 *   RAX  = operation number in; je_abi_status out.
 *   RDI,RSI,RDX,R10,R8,R9 = up to six argument registers in.
 *   RDX  = optional single u64 scalar result out (0 when no result).
 * Unknown operations return JE_ABI_UNSUPPORTED regardless of arguments. A
 * recognized operation with a nonzero reserved argument returns JE_ABI_INVALID
 * and has no side effect. No stable ABI promise; v0 is experimental.
 */
#include <stdint.h>

/* Explicitly numbered status. NOT related to je_status in status.h. */
typedef enum {
    JE_ABI_OK          = 0,
    JE_ABI_UNSUPPORTED = 1,
    JE_ABI_INVALID     = 2,
    /* M3b handle statuses (ADR 0008 D2): appended, DISTINCT values exposed to
     * ring 3 (not collapsed to INVALID). These are the v0 ABI mapping of the
     * internal JE_HANDLE_* results; abi.h stays independent of handle.h (the
     * glue maps between them). Information leak is not a concern: a handle names
     * only the caller's own process-local table. */
    JE_ABI_H_STALE     = 3,  /* slot FREE/RETIRED or generation mismatch */
    JE_ABI_H_BADINDEX  = 4,  /* handle 0, or index >= table capacity */
    JE_ABI_H_WRONGTYPE = 5,  /* live slot, wrong object type (hosted-only in M3b) */
    JE_ABI_H_DENIED    = 6,  /* live slot, insufficient rights (ring-3-observable
                              * via handle_dup attenuation since the M3b dup PR) */
    JE_ABI_H_QUOTA     = 7,  /* allocate: live-slot count at the process quota */
    JE_ABI_H_NOSLOTS   = 8,  /* allocate: no FREE slot remains */
    JE_ABI_H_REFOVERFLOW = 9, /* M3c (ADR 0009 D4): acquiring an owning reference
                              * past the configured JE_EP_REF_MAX policy bound;
                              * acquires nothing, object unchanged. RESERVED here
                              * (no ring-3 endpoint op yet) — first surfaced by a
                              * later M3c syscall; pinned now so the number is
                              * frozen in the v0 ABI. */
    JE_ABI_H_QUEUE_FULL  = 10, /* M3c D6: try_send on an end whose peer inbound
                              * queue is at JE_EP_QUEUE_DEPTH; nothing enqueued. */
    JE_ABI_H_SHORT_BUFFER = 11, /* M3c D6: try_recv with a buffer smaller than the
                              * head message; RDX = required length, NOT dequeued. */
    JE_ABI_H_PEER_CLOSED = 12, /* M3c D6: try_send to a peer-closed end, or try_recv
                              * on a peer-closed end whose inbound is now drained
                              * (EOF). */
    JE_ABI_H_EMPTY       = 13  /* M3c D6: non-blocking try_recv on an OPEN end with
                              * an empty inbound queue (would block; blocking is
                              * step 6). Distinct from PEER_CLOSED (peer gone). */
} je_abi_status;
_Static_assert(JE_ABI_OK == 0, "abi ok");
_Static_assert(JE_ABI_UNSUPPORTED == 1, "abi unsupported");
_Static_assert(JE_ABI_INVALID == 2, "abi invalid");
/* M3b handle statuses are ABI too — pin their numbering so a careless reorder
 * can't silently renumber a status ring 3 already depends on. */
_Static_assert(JE_ABI_H_STALE == 3, "abi h_stale");
_Static_assert(JE_ABI_H_BADINDEX == 4, "abi h_badindex");
_Static_assert(JE_ABI_H_WRONGTYPE == 5, "abi h_wrongtype");
_Static_assert(JE_ABI_H_DENIED == 6, "abi h_denied");
_Static_assert(JE_ABI_H_QUOTA == 7, "abi h_quota");
_Static_assert(JE_ABI_H_NOSLOTS == 8, "abi h_noslots");
_Static_assert(JE_ABI_H_REFOVERFLOW == 9, "abi h_refoverflow");
_Static_assert(JE_ABI_H_QUEUE_FULL == 10, "abi h_queue_full");
_Static_assert(JE_ABI_H_SHORT_BUFFER == 11, "abi h_short_buffer");
_Static_assert(JE_ABI_H_PEER_CLOSED == 12, "abi h_peer_closed");
_Static_assert(JE_ABI_H_EMPTY == 13, "abi h_empty");

/* Operation numbers (full 64-bit RAX; any value not listed is UNSUPPORTED). */
typedef enum {
    JE_SYS_ABI_QUERY = 0,  /* abi_query(which): RDI 0=version,1=profile -> RDX */
    JE_SYS_YIELD     = 1,  /* yield(): reschedule caller; no authority grant */
    JE_SYS_EXIT      = 2,  /* exit(code): RDI=code; terminate process, no return */
    JE_SYS_COPY_PROBE = 3, /* dev-only copy_probe(src=RDI, dst=RSI, len=RDX), <=256 */
    /* M3b handle ops (ADR 0008). (vm,index) is resolved ONLY via the current
     * user process; a non-user caller gets JE_ABI_INVALID. */
    JE_SYS_HANDLE_SELF  = 5, /* handle_self(): mint a handle to this process's
                              * SELF object -> handle in RDX, status in RAX */
    JE_SYS_HANDLE_CLOSE = 6, /* handle_close(h=RDI): close; status in RAX */
    JE_SYS_HANDLE_QUERY = 7, /* handle_query(h=RDI): -> RDX = object type (rights
                              * NOT exposed here; reserved out bits zero) */
    JE_SYS_HANDLE_RIGHTS = 8, /* handle_rights(h=RDI): -> RDX = rights mask of the
                              * handle, masked to JE_ABI_RIGHTS_KNOWN. Requires
                              * the INSPECT right (policy hook; see note below). */
    JE_SYS_HANDLE_DUP = 9    /* handle_dup(h=RDI, new_rights=RSI): derive a new
                              * handle to the same object with a rights SUBSET.
                              * -> RDX = new handle. Attenuation only (never a
                              * superset: a right the source lacks -> DENIED). */
    , JE_SYS_ENDPOINT_CREATE = 10 /* M3c (ADR 0009 A3-2) endpoint_create(): no
                              * args (all of RDI/RSI/RDX/R10/R8/R9 zero, else
                              * INVALID). Mints BOTH ends of a fresh channel into
                              * the caller's own table with all endpoint rights.
                              * Transactional. -> RAX status; RDX packs the two
                              * 32-bit handles: RDX[31:0]=h0, RDX[63:32]=h1. RDX
                              * is UNDEFINED on failure. UNSUPPORTED on a VM with
                              * no teardown hook (so an endpoint can only exist in
                              * a table whose death path reclaims it). */
    , JE_SYS_TRY_SEND = 11   /* M3c D6: try_send(h=RDI, ubuf=RSI, len=RDX,
                              * meta=R10, R8, R9). Non-blocking: copies len bytes
                              * from ubuf into the peer's inbound queue. len must
                              * be <= JE_EP_MSG_MAX. The handle needs the SEND right.
                              * R10 layout: R10[7:0] = cap_count (MUST be 0 until
                              * step 5; any nonzero -> INVALID), R10[63:8] reserved
                              * (must be 0). R8 reserved for step 5 cap-array ptr
                              * (must be 0). R9 reserved (must be 0).
                              * Status precedence (frozen): decode INVALID
                              * (meta/reserved/len/wide-handle, before any user
                              * access) > BADINDEX/STALE/WRONGTYPE > DENIED >
                              * PEER_CLOSED > QUEUE_FULL > copy-fault INVALID (the
                              * copy runs only after a clean preflight, so a bad
                              * ubuf ranks LAST, never masking a handle/queue
                              * status). RDX = 0 on success. */
    , JE_SYS_TRY_RECV = 12   /* M3c D6: try_recv(h=RDI, ubuf=RSI, cap=RDX,
                              * R10/R8/R9 reserved must be 0). Non-blocking: copies
                              * the head message into ubuf (up to cap bytes). The
                              * handle needs the RECV right. -> RAX status;
                              * RDX[31:0] = bytes delivered (OK), or required length
                              * (SHORT_BUFFER, no dequeue), or 0 (all other).
                              * RDX[63:32] reserved as 0 (step 5 will carry received
                              * cap count — step 5 MUST use a new op or out-register
                              * extension, NOT overload this op). */
#ifdef JE_DEV_COPY_INJECT
    , JE_SYS_COPY_FIXUP_INJECT = 4  /* dev-only: raw-copy an unmapped page to test the fixup */
#endif
} je_abi_op;

/* M3b handle object types (RDX from handle_query). Only SELF exists in M3b: a
 * kernel-minted, immortal, resource-free process-identity capability. Type 0 is
 * reserved/invalid so a zeroed slot never reads as a valid type. */
#define JE_ABI_OBJ_SELF 1u
/* M3c endpoint object type (ADR 0009). Pinned equal to the table-side
 * JE_ENDPOINT_OBJ_TYPE in the syscall glue via _Static_assert. */
#define JE_ABI_OBJ_ENDPOINT 2u
/* M3c D6 (step 4): max message payload bytes a try_send accepts. ABI-visible
 * (ring 3 must know the cap); pinned equal to the table-side JE_EP_MSG_MAX in the
 * syscall glue. */
#define JE_ABI_MSG_MAX 64u
/* M3c step 5 (ADR 0009 A5): max capabilities a single message may carry (K).
 * ABI-visible (ring 3 sizes its cap/handle arrays by it); pinned equal to the
 * table-side JE_EP_MSG_CAPS_MAX in the glue. */
#define JE_ABI_MSG_CAPS_MAX 2u
_Static_assert(JE_SYS_HANDLE_SELF == 5, "abi op handle_self");
_Static_assert(JE_SYS_HANDLE_CLOSE == 6, "abi op handle_close");
_Static_assert(JE_SYS_HANDLE_QUERY == 7, "abi op handle_query");
_Static_assert(JE_SYS_HANDLE_RIGHTS == 8, "abi op handle_rights");
_Static_assert(JE_SYS_HANDLE_DUP == 9, "abi op handle_dup");
_Static_assert(JE_SYS_ENDPOINT_CREATE == 10, "abi op endpoint_create");
_Static_assert(JE_SYS_TRY_SEND == 11, "abi op try_send");
_Static_assert(JE_SYS_TRY_RECV == 12, "abi op try_recv");

/* M3b rights ABI (ADR 0008 revisit, frozen here). Rights are a fixed uint32_t
 * bitmask. handle_rights masks its output to JE_ABI_RIGHTS_KNOWN so no internal
 * bit can leak to ring 3. A caller-supplied rights mask with an UNKNOWN bit is
 * rejected as INVALID in the handle_dup decode (the first ring-3 path that takes
 * a rights mask); every kernel mint uses a constant, asserted known at compile
 * time.
 *
 * Rights became genuinely enforced (not just a policy hook) with the handle_dup
 * attenuation PR: a process can derive a handle that DROPS INSPECT, and
 * handle_rights on that derived handle returns DENIED — ring-3-observable, proven
 * in QEMU (the M3b-s1 marker), no longer hosted-only. WRONGTYPE from ring 3 is
 * still not reachable (SELF is the only type), and remains hosted-tested. */
#define JE_ABI_RIGHT_INSPECT 0x1u              /* may read the object's metadata */
/* M3c endpoint rights (ADR 0009 D4a), appended to the known set here. These are
 * the first rights beyond INSPECT, so they are the first to make the handle_dup
 * decode's "unknown bit -> INVALID" gate (syscall.c) distinguish a known-but-
 * unheld right (DENIED, decided in the glue) from a malformed bit (INVALID).
 * NOTE (ABI behaviour change, owner-approved 2026-10-18, Option A): appending
 * SEND=0x2 makes 0x2 a KNOWN bit, so the ring-3 call dup(self, 0x2) — asserted in
 * M3b's test_syscall.c to return INVALID while 0x2 was unknown — now returns
 * DENIED (0x2 is a known right a SELF handle does not hold). The test is updated
 * to assert the new DENIED result for 0x2 AND to re-prove the unknown-bit->INVALID
 * contract on a still-unknown bit; see test_syscall.c and docs/IMPLEMENTATION.md. */
#define JE_ABI_RIGHT_SEND     0x2u             /* endpoint: may try_send */
#define JE_ABI_RIGHT_RECV     0x4u             /* endpoint: may try_recv */
#define JE_ABI_RIGHT_TRANSFER 0x8u             /* endpoint: may grant to a peer */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ENDPOINT_H
#define JEIOS_ENDPOINT_H
/*
 * M3c sub-PR 2 (ADR 0009 D-SEQ step 2): the endpoint/channel LIFECYCLE core —
 * the first DESTROYABLE, refcounted kernel object. Standalone and hostable like
 * handle.c: it includes ONLY <jeios/handle.h> plus the standard headers (no
 * vm.h, sched.h, or abi.h), allocates nothing itself (the caller supplies the
 * pool), and has zero global state — so `make check` unit-tests destruction,
 * the split refcount, and REFOVERFLOW with no kernel state.
 *
 * SCOPE OF THIS PR (frozen by ADR 0009):
 *   - the channel pool, the channel record (both ends in one record), the SPLIT
 *     refcount (handle_refs vs inflight_refs), retain/release, destruction on
 *     last reference, and the endpoint-aware alloc/dup/close SEAM over the pure
 *     handle table.
 *   - CREATE IS KERNEL-ONLY: je_endpoint_channel_create() is a direct in-kernel
 *     entry used by the hosted test (and, later, the kernel self-test). There is
 *     NO ring-3 endpoint_create syscall in this PR, by design — it ships WITH the
 *     owner-signed process-death release-walk PR (ADR 0009 D-OWNER-2), so a user
 *     process can never die holding an endpoint before teardown can reclaim it.
 *   - NOT here: messages (data IPC), transfer, waits. inflight_refs exists and is
 *     tested directly, but nothing in this PR queues a cap or registers a waiter.
 *
 * TABLE PURITY (frozen virtue, ADR 0009 D2/D3). The handle table stays fully
 * object-agnostic: it only ever reads/writes the opaque `object` pointer and
 * NEVER dereferences it. All retain/release lives HERE. That makes one invariant
 * load-bearing:
 *
 *   >> PRECONDITION (discharged by the create-syscall PR, NOT this one):
 *   >> an ENDPOINT-typed handle slot is created and closed ONLY through this
 *   >> layer (je_endpoint_handle_* / je_endpoint_channel_*). A raw
 *   >> je_handle_alloc() of an ENDPOINT-typed slot (which takes NO ref), a raw
 *   >> je_handle_close() of one (which drops the slot WITHOUT dropping the ref),
 *   >> or a je_handle_table_init()-zeroing of one, all desynchronize handle_refs
 *   >> from the real slot count. A raw CLOSE under-drops (leaks: fail-safe); a
 *   >> raw ALLOC over-counts so a later endpoint-close drops a ref the slot never
 *   >> took, which CAN free a still-named record early (a use-after-free — NOT
 *   >> fail-safe). THIS PR keeps both impossible by minting endpoints ONLY into
 *   >> kernel-local tables, never into a process's own vm->procs[i].htable. The
 *   >> create-syscall PR MUST add a TYPED alloc+close dispatch (endpoint slots
 *   >> are only ever created/closed through this layer) before any endpoint can
 *   >> live in a process table. "Callers remember to use the wrapper" is NOT an
 *   >> acceptable invariant and is why create stays kernel-only until then.
 */
#include <jeios/handle.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Configured policy bounds (ADR 0009 D4/D5). Small ON PURPOSE so overflow
 * and exhaustion are reachable in hosted tests; these are POLICY, never hardware
 * limits. Pinned by _Static_assert in endpoint.c. -------------------------- */
#define JE_EP_POOL_CAP      4u    /* channel records in a pool */
#define JE_EP_QUEUE_DEPTH   4u    /* messages per end inbound queue */
#define JE_EP_MAX_WAITERS   2u    /* waiters per end (unused this PR) */
/* M3c step 5: max payload BYTES per message (a policy bound, newly chosen here —
 * ADR 0009 said the message max is pinned in an M3c PR; the lifecycle PR did not,
 * so this is it). <= 255 so a per-slot length fits a uint8_t. */
#define JE_EP_MSG_MAX       64u
/* M3c step 5 (ADR 0009 A5): max CAPABILITIES a single message may carry. A policy
 * bound (K); 2 so the recv/send rollback is exercised (a 2nd mint can hit QUOTA).
 * DEPTH*K <= JE_EP_REF_MAX so a full queue of cap-carrying messages cannot
 * overflow an end's inflight count. */
#define JE_EP_MSG_CAPS_MAX  2u
/* Each ref counter must admit every non-handle owning ref the design can force
 * (queued caps + waiters) without spuriously overflowing a legitimate acquire,
 * AND be small enough to hit in a test. handle_refs is bounded by table slots;
 * inflight_refs by queue depth + waiters. One shared max covers both. */
#define JE_EP_REF_MAX       8u

/* ENDPOINT object type tag carried in a handle slot. Type 0 is reserved; SELF is
 * 1 (handle.h). This is the table-side copy (endpoint.c has no ABI include); the
 * syscall glue _Static_asserts it equals the ABI's endpoint object type when the
 * ring-3 ops land. MUST differ from JE_HANDLE_OBJ_SELF. */
#define JE_ENDPOINT_OBJ_TYPE 2u
_Static_assert(JE_ENDPOINT_OBJ_TYPE != JE_HANDLE_OBJ_SELF,
               "endpoint type must differ from SELF");

/* Endpoint rights — table-side copies of the ABI bits (abi.h JE_ABI_RIGHT_*),
 * kept here so endpoint.c has no ABI dependency. The glue pins these equal to
 * the ABI constants. INSPECT is shared with SELF (0x1). */
#define JE_EP_RIGHT_INSPECT  0x1u
#define JE_EP_RIGHT_SEND     0x2u
#define JE_EP_RIGHT_RECV     0x4u
#define JE_EP_RIGHT_TRANSFER 0x8u
#define JE_EP_RIGHTS_ALL (JE_EP_RIGHT_INSPECT | JE_EP_RIGHT_SEND \
                          | JE_EP_RIGHT_RECV | JE_EP_RIGHT_TRANSFER)

/* Status. Distinct enum (no ABI coupling); the glue maps these to je_abi_status
 * names (JE_ABI_H_* + JE_ABI_H_REFOVERFLOW) at the syscall boundary in a later
 * PR, exactly as handle.c's je_handle_status does. */
typedef enum {
    JE_EP_OK = 0,
    JE_EP_BADINDEX,     /* handle/arg malformed, or not an endpoint handle */
    JE_EP_STALE,        /* handle no longer live */
    JE_EP_WRONGTYPE,    /* live slot, not an endpoint */
    JE_EP_DENIED,       /* rights subset check failed (dup) */
    JE_EP_QUOTA,        /* table live-slot quota reached */
    JE_EP_NOSLOTS,      /* no FREE table slot, or pool exhausted */
    JE_EP_REFOVERFLOW,  /* acquiring past JE_EP_REF_MAX; nothing acquired */
    JE_EP_QUEUE_FULL,   /* try_send: peer inbound at JE_EP_QUEUE_DEPTH */
    JE_EP_SHORT_BUFFER, /* try_recv: caller buffer < head message; not dequeued */
    JE_EP_PEER_CLOSED,  /* try_send to a peer-closed end, or try_recv drained+EOF */
    JE_EP_EMPTY,        /* try_recv: OPEN end, empty queue (would block) */
    JE_EP_INVALID,      /* malformed pure-layer arg (e.g. len > JE_EP_MSG_MAX) */
    JE_EP_CORRUPT       /* an object pointer that is not a valid pool end
                         * (should be impossible; fail closed, never deref) */
} je_ep_status;

/* A queued capability descriptor (ADR 0009 A5): a direct reference to a
 * transferred endpoint END (by pool index + side + generation) plus the
 * attenuated rights the receiver will mint. NOT a handle (handles are per-table).
 * Each queued descriptor holds ONE inflight ref on rec[rec_index].end[side].
 * `generation` is a defence-in-depth tripwire; it matches while the inflight ref
 * keeps the record allocated, and distinguishes a since-reused record. */
typedef struct {
    uint16_t rec_index;    /* index into the owning pool's rec[] */
    uint8_t  side;         /* which end of that record (0/1) */
    uint8_t  cap_count_pad;/* reserved/alignment */
    uint32_t generation;   /* rec->generation when queued */
    uint32_t rights;       /* attenuated rights to mint (subset of source) */
} je_ep_capdesc;

/* One inbound message queue (a ring buffer of fixed-max-size messages). Each slot
 * carries payload bytes AND 0..JE_EP_MSG_CAPS_MAX capability descriptors (A5). */
typedef struct {
    uint8_t       buf[JE_EP_QUEUE_DEPTH][JE_EP_MSG_MAX];
    uint8_t       len[JE_EP_QUEUE_DEPTH];       /* payload bytes per slot */
    je_ep_capdesc cap[JE_EP_QUEUE_DEPTH][JE_EP_MSG_CAPS_MAX];
    uint8_t       cap_count[JE_EP_QUEUE_DEPTH];  /* caps per slot (0..CAPS_MAX) */
    uint8_t       head;                          /* index of the oldest message */
    uint8_t       count;                         /* number queued (0..DEPTH) */
} je_ep_queue;

/* An end's lifecycle state (ADR 0009 D2). */
typedef enum {
    JE_EP_END_FREE = 0,   /* the record is not allocated */
    JE_EP_END_OPEN,       /* live; peer is live */
    JE_EP_END_PEER_CLOSED,/* live; peer has gone (reads drain then EOF) */
    JE_EP_END_CLOSED      /* this end has no handle refs left */
} je_ep_end_state;

/* One end of a channel. `inbound` holds messages sent by the PEER and waiting for
 * THIS end's try_recv (Model A). Waiter storage is reserved for step 6. */
typedef struct {
    je_ep_end_state state;
    uint16_t        handle_refs;    /* LIVE handle slots naming this end */
    uint16_t        inflight_refs;  /* queued caps + waiters (unused this PR) */
    uint8_t         side;           /* 0 or 1; peer is record->end[side ^ 1] */
    je_ep_queue     inbound;        /* messages awaiting this end's try_recv */
} je_ep_end;

/* A channel record: both ends in one allocation, so a peer pointer is always to
 * the same record and can never dangle (ADR 0009 D2). `generation` is INTERNAL
 * double-free / stale-pool-index defence, bumped on free, never exposed to ring
 * 3 (handles bind &end[i], not a pool index), per ADR 0009 D-OWNER-1. NOTE: this
 * lifecycle PR bumps it but does not yet READ it — the borrowed-pointer rule
 * (ADR 0009 D3) keeps stale (rec,side) pairs out of the only refcount APIs that
 * take a raw record, so there is nothing to detour yet. The queue/wait PR that
 * first holds such a pair across a possible free MUST carry+check this
 * generation; until then it is reserved structure, honestly unused. */
typedef struct {
    bool     allocated;      /* record is in use */
    uint32_t generation;     /* bumped on free; internal only */
    je_ep_end end[2];
} je_ep_channel;

/* The pool. Caller-supplied storage (hostable): the kernel owns one static
 * instance; each hosted test owns a local one. No global state in endpoint.c. */
typedef struct {
    je_ep_channel rec[JE_EP_POOL_CAP];
    uint32_t      allocated_count;  /* live records (accounting) */
} je_endpoint_pool;

/* Initialize a pool: every record FREE, generation 1 (never 0), counts zero.
 * MUST be called once per pool lifetime. */
void je_endpoint_pool_init(je_endpoint_pool *pool);

/* ---- Create (KERNEL-ONLY; no ring-3 route exists in this PR). ------------- */
/* Transactionally create a channel and mint one handle to EACH end into the two
 * supplied tables (t0 for end 0, t1 for end 1 — may be the same table). Each
 * minted handle takes a handle_ref. On ANY failure the whole operation unwinds:
 * no record is left allocated and no handle is minted (both *out are 0). A record
 * with handle_refs == 0 is never observable. Rights for each end come from the
 * caller (typically JE_EP_RIGHTS_ALL for the creator). `out0`/`out1` REQUIRED.
 * Status precedence on the second mint failing: the first mint + the record are
 * rolled back and the second mint's status (QUOTA/NOSLOTS) is returned. */
je_ep_status je_endpoint_channel_create(je_endpoint_pool *pool,
                                        je_handle_table *t0, uint32_t rights0,
                                        je_handle_table *t1, uint32_t rights1,
                                        je_handle *out0, je_handle *out1);

/* ---- The endpoint-aware handle seam over the pure table. ------------------ */
/* Close ONE endpoint handle: resolve it as an endpoint, invalidate the table
 * slot (je_handle_close), then release exactly one handle_ref on its end. When an
 * end's handle_refs reaches 0 it goes CLOSED, its live peer goes PEER_CLOSED, and
 * (when BOTH ends have handle_refs == 0 AND inflight_refs == 0) the record is
 * freed and its generation bumped. Closing a stale/non-endpoint handle returns
 * the specific status and releases nothing. Release of a valid live ref is
 * infallible by construction (an impossible failure is an invariant violation,
 * not a rollback case). */
je_ep_status je_endpoint_handle_close(je_endpoint_pool *pool,
                                      je_handle_table *t, je_handle h);

/* Duplicate an endpoint handle with an equal-or-weaker rights subset, into the
 * same table, as a FRESH slot (its own index/generation/close) naming the SAME
 * end — a genuine second handle_ref, NOT a relabel. This is the endpoint
 * equivalent of je_handle_dup_self, but WITH retain-before-commit: it is the
 * reason dup_self stays SELF-only (handle.h) and endpoints never use it.
 *
 * Order (ADR 0009 D8 acquire-before-commit): resolve src as an endpoint
 * (BADINDEX -> STALE -> WRONGTYPE), subset-check new_rights (DENIED on any bit
 * src lacks; equality allowed), RETAIN a handle_ref (REFOVERFLOW if at
 * JE_EP_REF_MAX — acquires nothing), then je_handle_alloc the new slot
 * (QUOTA before NOSLOTS). On ANY alloc failure the retained ref is RELEASED again
 * and *out is 0. The rollback can never trigger destruction because `src` still
 * holds its own handle_ref throughout. `new_rights` must contain only known
 * endpoint bits (caller/glue rejects unknown bits as INVALID upstream, as for
 * dup_self). `out` REQUIRED. */
je_ep_status je_endpoint_handle_dup(je_endpoint_pool *pool, je_handle_table *t,
                                    je_handle src, uint32_t new_rights,
                                    je_handle *out);

/* ---- Direct (non-handle) inflight ref API — for the queue/wait PRs, tested
 * directly here. retain_inflight fails REFOVERFLOW past JE_EP_REF_MAX (acquires
 * nothing); release_inflight on the last ref may free the record under the same
 * both-ends-zero rule as close. These take a resolved end obtained from a handle
 * via je_endpoint_resolve (never a raw user pointer). ----------------------- */
je_ep_status je_endpoint_resolve(const je_endpoint_pool *pool,
                                 const je_handle_table *t, je_handle h,
                                 je_ep_channel **out_rec, uint8_t *out_side);
je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
                                         je_ep_channel *rec, uint8_t side);
je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
                                          je_ep_channel *rec, uint8_t side);

/* ---- Non-blocking data IPC (M3c step 4, ADR 0009 D6, data-only per D9) ------
 * All pure/hostable: they move KERNEL bytes only (the glue does copy_from/to_user
 * around them, honoring D6's copy-then-enqueue / peek-copy-then-dequeue ordering).
 * A message is bytes only — no capabilities until step 5. */

/* try_send PREFLIGHT: the read-only half of send — resolve the handle (needs
 * SEND), check self-end state and peer-inbound capacity — WITHOUT touching user
 * memory or mutating anything. The glue calls this FIRST (so a bad user pointer
 * never masks a DENIED/PEER_CLOSED/QUEUE_FULL status, and user memory is never
 * read for an unauthorized handle), THEN copy_from_user into staging, THEN
 * je_endpoint_try_send to enqueue. Returns the same status precedence try_send
 * would (BADINDEX/STALE/WRONGTYPE -> DENIED -> PEER_CLOSED -> QUEUE_FULL -> OK).
 * `len` is validated (> JE_EP_MSG_MAX -> JE_EP_INVALID). */
je_ep_status je_endpoint_send_preflight(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, size_t len);

/* Send preflight WITHOUT the QUEUE_FULL check (main handle + SEND + self-end
 * PEER_CLOSED only). The cap-send glue runs this first, then resolves/validates
 * caps, so per-cap DENIED/STALE outrank QUEUE_FULL (A5-4 precedence); the final
 * try_send_caps enforces QUEUE_FULL atomically. */
je_ep_status je_endpoint_send_preflight_main(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len);

/* try_send: enqueue a COPY of kbuf[0..len) (kernel memory) into the PEER end's
 * inbound queue. len must be <= JE_EP_MSG_MAX (else JE_EP_INVALID). Resolves the
 * handle as an endpoint requiring the SEND right. Status precedence:
 * BADINDEX/STALE/WRONGTYPE -> DENIED -> PEER_CLOSED (self end peer-closed) ->
 * QUEUE_FULL -> OK. On any non-OK status NOTHING is enqueued. kbuf may be NULL
 * iff len == 0 (a valid empty message). */
je_ep_status je_endpoint_try_send(je_endpoint_pool *pool, je_handle_table *t,
                                  je_handle h, const void *kbuf, size_t len);

/* A recv lease token (D6 / endpoint.h generation-carry rule): identifies the
 * exact head message a peek observed, so commit dequeues only that message. */
typedef struct {
    je_ep_channel *rec;
    uint8_t        side;
    uint32_t       generation;  /* rec->generation at peek time */
    uint8_t        head;        /* queue head index at peek time */
    uint8_t        len;         /* head message length at peek time */
} je_ep_recv_lease;

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_SCHED_H
#define JEIOS_SCHED_H
#include <stdbool.h>
#include <jeios/trap.h>
#include <jeios/status.h>
#include <jeios/vm.h>
#define JE_THREAD_MAX 8u
/* Single CPU. Bootstrap slot 0 is the idle/monitor task; other slots are kernel
 * threads or (M3a #4) ring-3 user threads bound to a vm process (own CR3 +
 * TSS.RSP0). Init/create/reap require interrupts disabled. Sleep/yield serialize
 * internally. No SIMD, user credentials, SMP, priorities, cancellation or
 * dynamic slot reuse. */
void je_sched_init(void);
je_status je_thread_create(void (*entry)(void *),void *argument);
/* M3a #4 step 3: create a ring-3 thread for a READY vm process (see vm.h). The
 * process's held kernel stack becomes TSS.RSP0; requires NX (ADR 0007 D2). */
je_status je_thread_create_user(je_vm *vm, uint32_t index);
/* M3a #4 step 3: remove a user thread (by owning vm process) from scheduling
 * before je_vm_destroy frees its kernel stack. Interrupts disabled; not the
 * running thread. */
je_status je_sched_drop_user(je_vm *vm, uint32_t index);
je_status je_thread_sleep(uint64_t ticks);
_Noreturn void je_thread_exit(void);
je_trap_frame *je_sched_interrupt(je_trap_frame *frame,bool timer);
/* M3a #4 step 4: terminate ONLY the current user process on a synchronous user
 * fault (or user int 0x80); returns a live successor's frame to iretq to. Called
 * from je_trap_dispatch with interrupts disabled. Never requeues the dead task. */
je_trap_frame *je_sched_fault_terminate(je_trap_frame *frame, uint64_t cr2);
/* M3a #5: clean exit(code) — terminate the current user process (DEATH_EXIT),
 * return a live successor. And the validated non-switching syscall return path:
 * validate+sanitize the current user frame (ADR "validate on every user return")
 * and resume it, or terminate (DEATH_BAD_RETURN) if the frame is invalid. */
je_trap_frame *je_sched_exit_current(je_trap_frame *frame, uint64_t code);
je_trap_frame *je_sched_return_to_user(je_trap_frame *frame);
/* Public death-reason numbering (matches the scheduler's internal record). */
enum { JE_DEATH_FAULT = 0, JE_DEATH_EXIT = 1, JE_DEATH_BAD_RETURN = 2 };
/* M3a #4 step 4: reap DYING user processes from bootstrap (slot 0, irq off)
 * while peers run. Returns count reaped, or a negative je_status on violation. */
int je_sched_reap_dead(void);
bool je_sched_has_dying(void);
je_status je_sched_death_record(je_vm *vm, uint32_t index, uint64_t *vec, uint64_t *err,
                                uint64_t *rip, uint64_t *cs, uint64_t *cr2);
je_status je_sched_death_reason(je_vm *vm, uint32_t index, uint64_t *reason, uint64_t *code);
uint64_t je_sched_dispatches_of(je_vm *vm, uint32_t index);
je_status je_sched_current_user(je_vm **vm, uint32_t *index);
bool je_sched_finished(void);
je_status je_sched_reap(void);
uint64_t je_sched_ticks(void);
uint64_t je_sched_preemptions(void);
uint64_t je_sched_user_cpl3_dispatches(void);
uint64_t je_sched_min_dispatches(void);
extern volatile uint64_t je_timer_ticks;
int je_arch_register_probe(void);
#endif

    }

    /* M3b: initialize the per-process handle table over the inline slot storage
     * before publishing READY (ADR 0008). Capacity is JE_VM_HANDLE_CAP; quota is
     * kept BELOW capacity so a handle_self loop hits QUOTA (not NOSLOTS) — the
     * two are reported distinctly. Compile-time-sized, so init cannot fail here;
     * the teardown/unwind zeroing (which sets capacity 0 -> every lookup
     * BADINDEX) reclaims it, and re-init on slot reuse prevents a stale LIVE
     * entry from surviving into a reused process slot. */
    if (!je_handle_table_init(&p->htable, p->hslots, JE_VM_HANDLE_CAP,
                              JE_VM_HANDLE_CAP - 1u)) {
        unwind(vm, idx);
        return JE_ERR_INVALID;   /* unreachable: capacity/quota are compile-time */
    }

    /* publish. */
    p->entry = image->entry_offset;
    p->state = JE_VM_READY;
    *out_index = idx;
    return JE_OK;
}

/* ---- destroy -------------------------------------------------------------- */

je_status je_vm_destroy(je_vm *vm, uint32_t index)
{
    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    if (vm->procs[index].state != JE_VM_READY) return JE_ERR_INVALID;
    /* A3-1: the release walk + unwind + the *p={0} zeroing in unwind() must be
     * ONE critical section. The direct main.c destroy callers may run with IF=1,
     * so an IF=0 precondition is not enough: a timer preempting between the walk
     * and the zeroing, while the peer process is live, would let the peer's
     * HANDLE_CLOSE mutate the same channel record mid-teardown (a UAF). Hold IRQs
     * off across the whole sequence when the kernel installed the guard; NULL
     * (the unit test) keeps the prior behaviour. The teardown hook (the endpoint
     * release walk) runs after the checks above and immediately before unwind. */
    uint64_t irq = 0u;
    if (vm->irq_save) irq = vm->irq_save();
    if (vm->teardown) vm->teardown(vm->teardown_ctx, &vm->procs[index].htable);
    unwind(vm, index);
    if (vm->irq_restore) vm->irq_restore(irq);
    return JE_OK;
}

/* ---- audit oracle --------------------------------------------------------- */

je_status je_vm_audit(const je_vm *vm, uint32_t index)
{
    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    const je_vm_ops *o = vm->ops;
    const je_vm_process *p = &vm->procs[index];
    if (p->state != JE_VM_READY) return JE_ERR_INVALID;

    /* upper levels: PML4[0] P|U; kernel PDPT[0] P, !U; arena PDPT[1] P|U;
     * arena PD[0] P|U. */
    uint32_t fl;
    if (o->read_upper(o->context, index, 4, 0, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
    if (o->read_upper(o->context, index, 3, 0, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || (fl & 4u)) return JE_ERR_INVALID;              /* P, !U */
    if (o->read_upper(o->context, index, 3, 1, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
    if (o->read_upper(o->context, index, 2, 0, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */

    /* Walk every arena page: a present user leaf must (a) not be the guard page,
     * (b) carry a perm that is one of the three legal values, (c) map a physical
     * frame that belongs to one of THIS process's USER_* extents, and (d) match
     * that extent's kind (CODE->RX, RODATA->R_NX, DATA->RW_NX). Also verify the
     * owning frame's identity alias state: RO_NX for CODE/RODATA, RW for DATA. */
    uint64_t seen_phys[JE_VM_ARENA_PAGES];

 succeeded in 0ms:
diff --git a/docs/adr/0000-decision-record-policy.md b/docs/adr/0000-decision-record-policy.md
index a18b641..f254a85 100644
--- a/docs/adr/0000-decision-record-policy.md
+++ b/docs/adr/0000-decision-record-policy.md
@@ -33,6 +33,19 @@ recording convention, not a gate):
    alternative then. A decision with no sensible flex says so ("firm; a change
    needs a new ADR").
 
+Plus, when a peer-agent exchange informed the decision:
+
+4. **Attached correspondence.** When a decision (human or agent) was informed by
+   a multi-agent exchange — a `peer-debate.sh` run, a parallel consult, an
+   adversarial review — the **raw transcript `.md` is committed to the repo** and
+   **linked from the decision record** (part 1's "why"). Transcripts live under
+   **`docs/adr/debates/`** (durable, versioned with the code), NOT left in `/tmp`
+   (ephemeral, lost on reboot). The link is the primary artifact; the decision
+   prose quotes only what it needs. A decision reached with no agent exchange has
+   nothing to attach and says so implicitly (no link). This makes the reasoning
+   that produced a decision auditable and re-readable by a future session, not a
+   paraphrase trapped in one agent's memory.
+
 **Bonus convention:** a decision that is *provisional pending the planning agent*
 (the cross-project agent consulted at each pause) is tagged **`FLEX (planning
 review)`** in its flex section, so the pause review has a ready worklist. The
@@ -59,6 +72,14 @@ peer-agent toolkit (which surfaces the alternatives this policy then records).
 - **If we tracked flex options in a separate file instead of inline:** the
   counterfactual would drift from the decision it qualifies. Inline keeps them
   co-located and reviewed together.
+- **If we left transcripts in `/tmp` and only paraphrased them (the prior
+  practice):** the correspondence that actually drove a decision would be lost on
+  the next reboot, leaving only one agent's summary — which the AGENTS.md history
+  rule warns is where after-the-fact fabrication creeps in. A future reversal
+  could not re-read the real back-and-forth. Committing the `.md` costs a few KB
+  per decision and makes the reasoning auditable; the alternative (a transcript
+  store outside the repo) would drift from the code it justifies, same failure as
+  separate flex files.
 
 ## Flex options to consider in future
 
@@ -66,6 +87,11 @@ peer-agent toolkit (which surfaces the alternatives this policy then records).
   "Flex options…") and whether the policy should also cover non-owner routine
   decisions, are revisitable with the main planning agent at a pause. The *intent*
   (three parts) is firm; the formatting is not.
+- **FLEX (planning review):** whether `docs/adr/debates/` is the right home (vs a
+  per-ADR `debates/` subdir, or git-LFS for very large transcripts), and whether
+  transcripts should be redacted/trimmed before commit, are revisitable. The
+  *intent* (commit the raw correspondence, link it from the decision, never leave
+  it only in `/tmp`) is firm; the location and trimming are not.
 - If a decision-record tool or lint is ever added, this policy is its spec.
 
 ## Retroactive sweep
@@ -82,3 +108,12 @@ counterfactual it is marked **"unknown — not in the record"** rather than inve
 reconstructed parts are **appended annotations**, not edits inside the original
 frozen decision text. The sweep re-decides nothing and changes no executable
 behavior.
+
+The sweep ALSO applies part 4 (attached correspondence): the peer-debate
+transcripts for prior decisions that currently live only in `/tmp` are committed
+under `docs/adr/debates/` and linked from the decisions they informed (the M3b/M3c
+debates: `m3b4`, `m3c`, `m3c-lifecycle`, `m3c-step3`, `m3c-step4`, `m3c-step5`,
+`m3c-step6`, …). A prior decision whose transcript was genuinely not saved is
+marked **"transcript not retained"** rather than reconstructed — never fabricate a
+transcript. Transcripts committed during normal (non-sweep) work from this point
+on need no label; the sweep label is only for back-filled history.
diff --git a/docs/adr/0009-m3c-capability-ipc.md b/docs/adr/0009-m3c-capability-ipc.md
index e2c17d0..187a28a 100644
--- a/docs/adr/0009-m3c-capability-ipc.md
+++ b/docs/adr/0009-m3c-capability-ipc.md
@@ -687,3 +687,201 @@ bootstrap fixture subsumes this or wants a cooperative-yield primitive first.
 v0 choices; a future ABI revision (or the step-7 real bootstrap replacing the
 seeded fixture) may restate them. K (caps per message) is a policy bound like the
 others and may grow.
+
+## AMENDMENT (2026-10-19, owner-approved) — step-6 blocking-recv (waits) decisions
+
+This amendment finalizes the **D7** wait contract for the (owner-gated) waits PR
+that edits `sched.c`. It composes with D7's already-frozen frame (state machine
+`WAITING→WOKEN|TIMED_OUT|CANCELLED|PEER_CLOSED`, mandatory bounded deadline, timer
+ISR performs the terminal transition, receiver-only blocking, process-death
+unlinks waiters, re-validate the user buffer at copy time after wake). It does not
+reopen those; it decides the six open questions the step-6 design pass raised.
+
+**Attached correspondence (ADR 0000 part 4).** Design driven by an
+implementor↔adversarial `peer-debate.sh` run (claude=implementor,
+codex=adversarial-reviewer, 2 rounds). Full transcript:
+[`docs/adr/debates/m3c-step6-waits.md`](debates/m3c-step6-waits.md). The reviewer
+sharpened the design materially (separate waiter vs scheduler state machines, one
+arbitration CAS, no copy-out from the scheduler) and surfaced the A6-1 contract
+conflict below as a genuine owner escalation, not a clarification.
+
+### A6-STRUCT. Two separate state machines + one arbitration CAS
+
+**Decision + why.** The **waiter** state (`WAITING→WOKEN|TIMED_OUT|CANCELLED|
+PEER_CLOSED`) is kept **distinct** from the **scheduler** state
+(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
+pure helper **`je_ipc_wait_arbitrate(waiter, cause)`** performs the terminal
+transition as **one compare-and-set** under IF=0: the first eligible cause wins,
+later causes are no-ops. Every path that creates eligibility calls it in the SAME
+IF=0 critical section as the endpoint/clock mutation that caused it — sender
+enqueue → `WOKEN`, peer close → `PEER_CLOSED`, timer ISR deadline → `TIMED_OUT`,
+internal cancel → `CANCELLED`. The parked **syscall continuation** then *validates
+the already-selected result and performs the owned-record copy-out + commit*; it
+is **not** a second authority and never rewrites the CAS result. (Rationale: the
+adversarial review showed "ISR stamps TIMED_OUT, syscall re-checks and may upgrade
+to WOKEN" would make the syscall a second authority — not the single-terminal-CAS
+D7 requires. Making enqueue/close do their own CAS in their own critical section
+is what keeps it single.)
+
+**Predictions if other choices were made.** *Waiter state IN the scheduler enum
+(my first lean):* conflates "why the block ended" with "is it runnable," and makes
+every scheduler state check also reason about IPC causes — more tested-code
+coupling, the opposite of the owner gate's intent. *Syscall re-check as the
+authority:* two authorities for one terminal state → the exact double-transition
+race D7 forbids; a timeout and a late enqueue could both "win." *Waiter registry
+in endpoint.c instead of the thread:* thinner `sched.c` but endpoint code reaches
+into scheduler internals to wake — worse coupling across the tested boundary.
+
+**Flex options to consider in future.** **FLEX (planning review).** Blocking
+*send* (deferred in v1) will add a sender-side waiter + a staging-buffer lifetime;
+the arbitrate helper is meant to generalize to it. If a third blocking op appears,
+revisit whether the waiter fields belong in the thread struct or a side table.
+
+### A6-1. Queued-message-plus-peer-close → DRAIN-FIRST wins (A1)
+
+**Decision + why.** When a message is queued **and** the peer has closed, the
+queued message is **delivered** (`WOKEN`); `PEER_CLOSED` is returned only when the
+queue is **empty**. The literal D7 priority `PEER_CLOSED > WOKEN` is read as
+ordering **block-ending triggers**, NOT as overriding already-queued data. This
+was a genuine frozen-contract conflict (owner-escalated, not a clarification): the
+**merged step-4 `try_recv` is already drain-first** (CLOSED is "dead" only on an
+empty queue), so A1 makes blocking `recv` behave **identically** to non-blocking
+`try_recv` on the same endpoint state — "closing a peer never eats bytes already
+delivered to you." **Owner-approved A1.**
+
+**Predictions if other choices were made.** *A2 (literal PEER_CLOSED > a queued
+message):* blocking `recv` and non-blocking `try_recv` would return *different*
+results on the identical state (one drains, one reports closed) — a surprising
+split a caller could not predict from the op alone, and a divergence from already-
+tested step-4 behavior that would need its own justification. It would also strand
+a legitimately-sent final message on peer close.
+
+**Flex options to consider in future.** Firm for M3c. If a future "urgent close"
+semantic is ever wanted (peer close *preempts* pending data), that is a new op or
+an explicit flag, not a reinterpretation of this priority — a new ADR.
+
+### A6-2. CANCELLED is internal-only in v1 (B1)
+
+**Decision + why.** The `CANCELLED` terminal state is reachable in v1 **only via
+an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
+deterministic kernel test; there is **no ABI path** to it yet (no signal / kill /
+async-cancel mechanism exists in M3c). It is implemented and tested now so the
+arbitration CAS and the priority tie-break are proven on all four causes, and so a
+future signal mechanism wires to a ready terminal state. Documented as
+**ABI-unreachable in v1**. **Owner-approved B1.**
+
+**Predictions if other choices were made.** *B2 (defer CANCELLED entirely — state
+exists, no path, no test):* the arbitration helper would ship with one of its four
+terminal causes unexercised, so the first real canceller (a future signals
+milestone) would be adding an *untested* transition into tested scheduler code —
+exactly the risk the owner gate exists to avoid. Testing it now with an internal
+trigger is cheap and front-loads that proof.
+
+**Flex options to consider in future.** **FLEX (planning review).** The ABI
+trigger for cancellation (a signal syscall, a deadline-update that cancels, a
+process-level cancel) is a future-milestone decision; A6-2 only fixes that the
+mechanism exists and is internally reachable now.
+
+### A6-3. Deadline encoding: zero = poll, bounded max, overflow-checked (C)
+
+**Decision + why.** The timeout is in **timer ticks** (the existing M2 clock,
+inclusive `wake <= ticks` as the current sleep path uses). **Zero timeout = a
+non-blocking poll** (check once; immediate `TIMED_OUT` if nothing is deliverable —
+never parks). A **bounded maximum** `JE_IPC_MAX_WAIT` is enforced: a requested
+deadline is rejected (`JE_ABI_INVALID`) if `duration > JE_IPC_MAX_WAIT` or if
+`ticks + duration` would overflow `uint64` — mirroring the existing
+`je_thread_sleep` overflow guard. No unbounded block, ever. **Owner-approved.**
+
+**Predictions if other choices were made.** *Zero = block forever:* violates D7's
+"no unbounded block" outright. *No max / no overflow check:* a caller passing a
+near-`UINT64_MAX` duration wraps the deadline below `ticks`, making the ISR fire an
+*immediate* spurious `TIMED_OUT` (or, worse, never) — a silent correctness bug the
+existing sleep path already guards against, so not guarding here would be an
+inconsistency.
+
+**Flex options to consider in future.** `JE_IPC_MAX_WAIT`'s exact value is a
+policy bound like the other `JE_*` caps and may change; the *rule* (zero=poll,
+bounded, overflow-checked) is firm.
+
+### A6-4. Waiter-capacity exhaustion reuses the non-blocking path (D)
+
+**Decision + why.** Waiter slots are bounded (`JE_EP_MAX_WAITERS`, one blocked
+receiver per process → 2 with `JE_VM_MAX_PROCESSES=2`). On the single-CPU v1 this
+is **structurally unreachable** (a process has at most one thread mid-syscall
+blocking at a time). The code **asserts** the bound and, on the assert-defeating
+path, **fails closed to the non-blocking result** (returns `EMPTY`-equivalent
+rather than parking) — no new ABI status is minted. **Owner-approved.**
+
+**Predictions if other choices were made.** *A new `JE_ABI_WAITERS_FULL` status:*
+adds ABI surface for a state that cannot occur with 2 single-threaded processes —
+untestable without fabricating an impossible scheduler state, and a permanent ABI
+commitment for a v1 non-situation. *Silently parking anyway past the bound:* a
+buffer overrun of the waiter array — the exact class of bug the bound exists to
+stop.
+
+**Flex options to consider in future.** **FLEX (planning review).** When
+multi-threaded processes or blocking *send* arrive, `JE_EP_MAX_WAITERS` grows and
+exhaustion may become reachable; *then* a real status (or a queue-of-waiters) is
+revisited. v1 fails closed because it cannot happen.
+
+### A6-5. No user copy-out from the scheduler; parked syscall continuation copies
+
+**Decision + why.** The wake path (arbitrate CAS + reserve the queue head for a
+`WOKEN`) runs in the scheduler/ISR, but the **user copy-out and commit happen in
+the resumed syscall continuation**, never inside `je_sched_interrupt` after a
+`cr3_load`. The scheduler only flips `THREAD_BLOCKED_IPC → READY`. A copy fault in
+the continuation returns `JE_ABI_INVALID`, **preserves** the queued message,
+releases the head reservation, and re-arbitrates (the buffer re-validation D7
+already requires). The continuation does **not** re-resolve the original handle
+(D3 owned-reference rule) — it uses the reserved owned record. **Owner-approved
+design.**
+
+**Predictions if other choices were made.** *Copy-out inside the scheduler after
+the address-space switch:* introduces a brand-new page-fault context in scheduler
+assembly; the only exact-RIP user-copy-fault recovery proven so far is the M3a
+*syscall* path, so copying post-`cr3_load` in the ISR would be *untested* fault
+machinery in the most safety-critical file — precisely what the owner gate guards.
+*Re-running the handle-based recv on wake:* re-resolves a capability the owned-
+reference rule says must not be re-resolved after blocking (the handle could have
+changed), and double-counts the drain.
+
+**Flex options to consider in future.** Firm. A blocking-send milestone will add a
+symmetric copy-*in* on the sender continuation under the same rule.
+
+### A6-PEERWAKE. Peer death actively wakes blocked receivers
+
+**Decision + why.** `je_vm_destroy`'s existing endpoint release walk (step-3,
+owner-signed) marks the dying process's ends; where that transitions a peer end to
+`CLOSED`, it now also **calls the arbitrate CAS on any receiver blocked on that
+end**, waking it `PEER_CLOSED` **immediately** rather than letting it wait out its
+deadline. The waiter-unlink for the dying process's *own* blocked thread happens in
+the same IRQ-critical teardown, **before** the thread/frame slot is reused (D3.3),
+so a later timer tick cannot stamp a reused slot. **Owner-approved design.**
+
+**Predictions if other choices were made.** *Rely on the receiver's own deadline /
+next schedule to notice the close:* a receiver blocked with a far deadline on an
+endpoint whose peer just died would sleep uselessly until timeout — a latency bug
+and a semantic one (it should learn `PEER_CLOSED` now, not `TIMED_OUT` later).
+*Skip the unlink and let the ISR skip non-allocated slots:* risks a wake writing a
+reused frame if slot reuse races a pending stamp — the stale-slot write D3.3 names.
+
+**Flex options to consider in future.** Firm; symmetric with the step-3 release
+walk it extends.
+
+### Proof plan (deterministic, single-CPU — avoids the step-5 spin race)
+
+Each terminal cause gets a deterministic kernel/hosted proof that does **not**
+rely on two spin-looping user stubs (the step-5 hang lesson):
+- **queued-before-recv:** message already present → immediate `WOKEN`, no park.
+- **WOKEN:** thread X parks on recv; thread Y sends then *explicitly yields* →
+  X resumes, exactly one `WOKEN`, FIFO delivery.
+- **TIMED_OUT:** X parks; advance `je_timer_ticks` to the inclusive deadline →
+  ISR arbitrates `TIMED_OUT`, X resumes with it.
+- **PEER_CLOSED:** X parks; destroy the peer process → active `PEER_CLOSED` wake
+  *before* the deadline.
+- **CANCELLED:** internal `je_ipc_wait_cancel` → exactly one `CANCELLED`.
+- **copy-out fault after WOKEN:** `JE_ABI_INVALID`, message stays queued,
+  reservation released, a next waiter can still wake.
+- **teardown while blocked:** waiter unlink + reference release precede slot reuse.
+- **competing triggers:** enqueue vs timeout in the tie window → assert the single
+  CAS picks the A6-1 priority, never two results.
# Peer debate — 2026-10-19T02:08:16Z
Topic: JeiOS M3c step 6 blocking-recv waits (D7): minimal correct sched.c design + proof plan + what to escalate
Rounds: 2   Roles: claude=implementor codex=adversarial-reviewer
Context file: /tmp/step6.md


[R1 CLAUDE] **Position (implementor): the Q2 "syscall loop that resumes after the park point" doesn't fit this kernel, and that settles Q1–Q3.** Verified on main @ e914a95:

- A user syscall has no kernel continuation. `je_syscall_dispatch` *returns a frame* (`syscall.c:225-259`). The user thread's whole saved state is its trap frame at `rsp0_top - sizeof(frame)` (`sched.c:174-176`). Resuming it means `iretq` on that frame. Only kernel threads park mid-function (`je_thread_sleep` → `int $0x81`, `sched.c:242`). So a blocked recv can't "return from park and re-check". **Whoever performs the terminal transition must write the result into the blocked thread's saved frame.** That is literally what D7 says ("the ISR performs the transition and delivers the result, not merely mark READY"). Q3 isn't an interpretation question: the architecture forces the ADR reading.
- Restarting the syscall (rewind RIP over `int 0x80` and re-execute) is also ruled out. The re-executed recv would re-resolve the handle, and D3 is frozen: "a woken thread uses the waiter's owned ref, never a re-resolve of the original handle."
- The copy must happen in the receiver's CR3. A sender completing the receiver's recv is running under the sender's CR3, so it can't do the copy-out.

## Recommended design

**Q1, a hybrid split.** The thread struct holds `wait_state` (WAITING or a terminal cause), the deadline (reuse `wake`) and `wait_end`. The end record holds a single back-link `waiter` (thread slot or NONE) plus the waiter's `inflight_refs` (D3 alias 3). Neither pure A nor pure B works. The end needs the back-link so a sender or peer-close can find the waiter. The thread needs the state so the ISR can scan it.

**Q2/Q3, each trigger does an eager CAS at its own instant.** IF=0 serializes every trigger, so the case where "several triggers are eligible at once" only arises inside one atomic section. The priority table applies there (for example, the ISR finds the deadline passed AND the end PEER_CLOSED → PEER_CLOSED).
- **Sender:** enqueue, then CAS the peer's waiter WAITING→WOKEN, then READY.
- **Peer close or death:** when the end goes CLOSED, CAS the opposite waiter →PEER_CLOSED, write `rax`, READY.
- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.

A CAS that loses is a no-op. There is no re-check at the syscall; the CAS winner is the single source of truth.

**WOKEN delivery is deferred to dispatch.** I propose one hook in `select_and_install_next`, after the `je_uframe_check` and CR3 load. It runs for a thread with a pending completion and calls `je_ep_wait_finish(slot)` (endpoint.c). That function:
- unlinks the end's back-link;
- releases the waiter's inflight ref;
- for WOKEN, runs the ordinary try_recv commit (copy-out with re-validation and fixup, cap mint), so a copy fault becomes an error result, as D7 requires.

The timeout and peer-closed paths can also defer their unlink and ref release to this hook. That keeps endpoint-pool frees out of ISR context. The ISR still writes the final status itself, so it stays faithful to D7.

**New hazard: theft between the WOKEN stamp and dispatch.** A5 transfer lets both processes hold the same end. The other process can be scheduled first and `try_recv` the message that WOKEN promised. Two fixes:
- **(a) Reservation (my pick).** While a WOKEN completion is pending, the end's head message is reserved, so other receivers see `EMPTY`. No new ABI.
- **(b)** Dequeue into a per-waiter kernel staging buffer. This costs JE_THREAD_MAX × max message size, and putting the message back after a copy fault gets messy.

Also: **at most one waiter per end.** A second blocking recv needs a status code (open question below).

**Q4, both kinds of work are needed:**
1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
2. **Peer death or close.** This must actively wake the receiver with PEER_CLOSED through the close hook above. A plain `handle_close` that drops `handle_refs` to 0 must trigger it too, not only death.

**Q6, the minimal sched.c diff:**
- one enum value;
- three thread fields;
- `je_sched_block_current_ipc(f, deadline, end)` (sets state, returns the successor frame);
- `je_sched_ipc_complete(slot, cause, rax, rdx)` (CAS, frame write, READY);
- the ISR scan branch;
- the dispatch hook call;
- a `je_sched_unlink_ipc(vm)` that destroy calls.

Everything else lives in endpoint.c and syscall.c: the queue check, the immediate-delivery path, the reservation, the copy and the priority choice. Also bump the D4 `_Static_assert`: `JE_EP_REF_MAX` must be ≥ queue depth plus max waiters.

## Proof plan (deterministic; nothing spins in ring 3)

The bootstrap kernel thread drives every test and observes state through accessors. A user stub never waits on another user stub.

| Cause | Proof |
|---|---|
| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
| TIMED_OUT | The user blocks with deadline N and nothing sends. After N ticks, assert `rax=TIMED_OUT` in the resumed thread. |
| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
| PEER_CLOSED | Bootstrap destroys the peer vm while the receiver is blocked. |
| Same-instant tie | Inside one IF=0 section, enqueue then run the tick path by hand (deadline already passed). Assert WOKEN. Do the same with peer-close plus deadline and assert PEER_CLOSED. |
| Receiver death | Destroy the blocked receiver, then send. Assert no wake, the back-link is cleared and the ref count returns to baseline. |
| Theft | The other holder calls `try_recv` while a WOKEN completion is pending and gets `EMPTY`. |
| Copy fault after wake | Needs a proof-only hook that poisons the saved buffer pointer, because nothing in M3c can unmap the buffer. Flag it as an injected fault, not organic. |

Also add a host `make check` table test of the CAS and priority function.

## Escalate to the owner

1. **The D7 reading is forced by the frame-return architecture**, so the owner should confirm it rather than choose it. The ISR writes the TIMED_OUT result into the frame; dispatch-time completion handles copy-out, unlink and ref release; there is no syscall re-check. Owner sign-off is needed on the dispatch hook in `select_and_install_next`, which is tested code.
2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
3. **New ABI surface:** a blocking-recv op number, the timeout register (R10 is currently reserved=0 on try_recv), a `JE_EP_WAIT_MAX_TICKS` cap, and whether a timeout of 0 means try_recv or INVALID.
4. **A status for a second waiter on the same end**, and the choice between reservation and staging for the theft hazard. This is open for an agent majority vote unless the owner wants it.
[R1 CODEX] codex
Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
But several parts of the proposal are not yet contract-safe.
1. Q1: avoid the endpoint back-link initially
Use a thread-owned waiter:
- `wait_state`
- `wait_end` (protected by an owned endpoint reference)
- deadline
- saved receive arguments / completion metadata
- pending completion flag
Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
- stale endpoint-to-thread IDs;
- a second registry lifecycle;
- silently imposing “one waiter per endpoint,” which D7 does not authorize;
- extra unlink work during process death.
A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
2. Q2: no syscall re-check loop
The syscall path should be:
1. Check queue/closed state.
2. If immediately deliverable, complete synchronously.
3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
4. A terminal transition writes scalar results into the saved frame.
5. On selection of the thread, a resume-completion hook performs the owned-endpoint receive commit and copy-out under the receiver’s CR3.
The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
The WOKEN state and the eventual syscall result need separate concepts. A post-wake user-buffer fault must preserve the terminal wait cause while returning the specified normal copy error.
3. Q3: Claude overstates that the interpretation is settled
The architecture settles “no syscall loop,” but not the exact meaning of D7’s “ISR performs the terminal transition.”
The timer path must not implement:
```text
WAITING -> TIMED_OUT
```
merely as a timeout hint followed by a syscall re-check. The ISR must invoke the terminal-transition helper and install the timeout result itself.
The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
There is a second unresolved case: sender enqueues and marks WOKEN, then peer death occurs before the receiver runs. Both WOKEN and PEER_CLOSED are now eligible. “First CAS wins” gives WOKEN; the stated priority could imply PEER_CLOSED. The ADR needs an owner decision on whether priority applies only to simultaneous eligibility inside one critical section, or whether a later higher-priority trigger may supersede a prior terminal transition. Superseding would conflict with the single-terminal-transition rule, so the likely safe interpretation is “priority at one arbitration point,” but that must be recorded.
A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
4. Q4: teardown requires more than changing `DYING`
For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
For peer death, the endpoint release walk must actively wake affected receivers with `PEER_CLOSED`; merely setting endpoint state is insufficient because a far deadline delays re-entry indefinitely.
The close path must distinguish “this process died” from “the peer side is actually closed.” Shared/transferred capabilities may mean peer death does not immediately imply closure.
5. Q5: deterministic proof set
Use one-shot kernel/user actors and explicit yields. No ring-3 spin loops.
- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
- WOKEN: receiver blocks; a scheduled sender performs one send and yields/exits; verify reserved payload and status.
- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
- PEER_CLOSED: receiver blocks; destroy/close the peer; verify active wake and result.
- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
- Copy fault: use an available VM mapping/unmapping test hook; otherwise mark this as unverified integration coverage and test the copy-fixup path separately.
- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
Claude’s proposed `RAX=...`, `RDX=0`, timeout register, and `JE_EP_REF_MAX` assertion are not established by the supplied contract. They require ABI/source verification or owner approval.
6. Minimal sched.c change
The semantic minimum is:
- add `THREAD_BLOCKED_IPC`;
- add thread-owned waiter fields;
- add `je_sched_block_ipc(...)`;
- add one compare-and-set terminal helper used by sender, close, cancel, and timer;
- add the timer scan invoking that helper;
- add teardown unlink/tombstone before slot reuse;
- add one common return-path hook for deferred owned-endpoint completion.
Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
Owner escalation should be limited to:
1. Whether deferred WOKEN copy-out before `iretq` satisfies D7’s delivery requirement.
2. Priority semantics when a lower-priority terminal cause wins before a later higher-priority trigger becomes eligible.
3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
4. The exact v1 ABI for deadline, result registers, zero timeout, and copy-fault status.
5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
But several parts of the proposal are not yet contract-safe.
1. Q1: avoid the endpoint back-link initially
Use a thread-owned waiter:
- `wait_state`
- `wait_end` (protected by an owned endpoint reference)
- deadline
- saved receive arguments / completion metadata
- pending completion flag
Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
- stale endpoint-to-thread IDs;
- a second registry lifecycle;
- silently imposing “one waiter per endpoint,” which D7 does not authorize;
- extra unlink work during process death.
A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
2. Q2: no syscall re-check loop
The syscall path should be:
1. Check queue/closed state.
2. If immediately deliverable, complete synchronously.
3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
4. A terminal transition writes scalar results into the saved frame.
5. On selection of the thread, a resume-completion hook performs the owned-endpoint receive commit and copy-out under the receiver’s CR3.
The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
The WOKEN state and the eventual syscall result need separate concepts. A post-wake user-buffer fault must preserve the terminal wait cause while returning the specified normal copy error.
3. Q3: Claude overstates that the interpretation is settled
The architecture settles “no syscall loop,” but not the exact meaning of D7’s “ISR performs the terminal transition.”
The timer path must not implement:
```text
WAITING -> TIMED_OUT
```
merely as a timeout hint followed by a syscall re-check. The ISR must invoke the terminal-transition helper and install the timeout result itself.
The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
There is a second unresolved case: sender enqueues and marks WOKEN, then peer death occurs before the receiver runs. Both WOKEN and PEER_CLOSED are now eligible. “First CAS wins” gives WOKEN; the stated priority could imply PEER_CLOSED. The ADR needs an owner decision on whether priority applies only to simultaneous eligibility inside one critical section, or whether a later higher-priority trigger may supersede a prior terminal transition. Superseding would conflict with the single-terminal-transition rule, so the likely safe interpretation is “priority at one arbitration point,” but that must be recorded.
A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
4. Q4: teardown requires more than changing `DYING`
For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
For peer death, the endpoint release walk must actively wake affected receivers with `PEER_CLOSED`; merely setting endpoint state is insufficient because a far deadline delays re-entry indefinitely.
The close path must distinguish “this process died” from “the peer side is actually closed.” Shared/transferred capabilities may mean peer death does not immediately imply closure.
5. Q5: deterministic proof set
Use one-shot kernel/user actors and explicit yields. No ring-3 spin loops.
- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
- WOKEN: receiver blocks; a scheduled sender performs one send and yields/exits; verify reserved payload and status.
- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
- PEER_CLOSED: receiver blocks; destroy/close the peer; verify active wake and result.
- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
- Copy fault: use an available VM mapping/unmapping test hook; otherwise mark this as unverified integration coverage and test the copy-fixup path separately.
- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
Claude’s proposed `RAX=...`, `RDX=0`, timeout register, and `JE_EP_REF_MAX` assertion are not established by the supplied contract. They require ABI/source verification or owner approval.
6. Minimal sched.c change
The semantic minimum is:
- add `THREAD_BLOCKED_IPC`;
- add thread-owned waiter fields;
- add `je_sched_block_ipc(...)`;
- add one compare-and-set terminal helper used by sender, close, cancel, and timer;
- add the timer scan invoking that helper;
- add teardown unlink/tombstone before slot reuse;
- add one common return-path hook for deferred owned-endpoint completion.
Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
Owner escalation should be limited to:
1. Whether deferred WOKEN copy-out before `iretq` satisfies D7’s delivery requirement.
2. Priority semantics when a lower-priority terminal cause wins before a later higher-priority trigger becomes eligible.
3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
4. The exact v1 ABI for deadline, result registers, zero timeout, and copy-fault status.
5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
[R2 CLAUDE] I checked the repo at e914a95. Several of Codex's open questions are already answered by frozen text or merged code, so the escalation list can shrink. I agree with most of Codex's corrections. I dispute two: its timer-path rule and how open the priority question is. It also missed two hazards.

## Agree
- **Thread-owned waiter plus a bounded scan of `JE_THREAD_MAX`, no endpoint back-link.** ADR 0009 D3 item 3 says a waiter holds an `inflight_refs` on the end and a reference to the blocked thread, which must be unlinked before the thread's storage is reused. If the waiter lives in the thread slot, that thread reference points to itself and can never dangle. Teardown then only has to clear the slot's waiter and release its inflight ref.
- **The resume path needs an owned-ref commit, not plain `try_recv`.** This is verified: `je_endpoint_recv_peek` calls `je_endpoint_resolve(pool, t, h, …)` (endpoint.c ~496), which re-resolves the handle. D3 forbids that for a woken thread. We need a variant keyed on `(rec, side)` that reuses the existing `je_ep_recv_lease` two-phase commit.
- **Peer death must actively wake a blocked receiver.** The hook belongs at the refcount-reaches-zero transition (endpoint.c:208, `peer->state = JE_EP_END_PEER_CLOSED`), not at process death. That already separates "process died" from "end closed" when capabilities are shared or transferred, so it needs no extra design.
- **CANCELLED has no v1 trigger.** `je_thread_create_user(vm, index)` and the slot lookups (sched.c:295/353) give one user thread per process. The only thread that could close the waited handle is the blocked thread itself. The receiver's own death tombstones the waiter and delivers no result, so that is not CANCELLED either. Ship an internal helper with a unit test and document it as unreachable from the ABI.

## Dispute
1. **The timer ISR should not judge eligibility itself.** Make it a structural invariant instead: under IF=0, every enqueue and every peer-close transition calls the wake helper in the same critical section. So a WAITING waiter can never coexist with a deliverable message or a peer-closed state. The timer then does a plain `CAS WAITING→TIMED_OUT` and writes the result into the saved frame, with an assert-or-`JE_EP_CORRUPT` check on the invariant. If the ISR evaluated eligibility, it would have to reach into the endpoint pool, which is exactly the coupling we are keeping out of tested `sched.c`. TIMED_OUT is a plain status code, so the ISR fully delivers it. D7's "ISR performs the transition and delivers the result" is met literally for TIMED_OUT.
2. **"WOKEN, then the peer dies" has one safe reading, fixed by existing code.** Merged `try_recv` drains queued messages before reporting PEER_CLOSED (endpoint.c:517-520: PEER_CLOSED only when `count == 0`). Read literally, PEER_CLOSED > WOKEN would discard a queued message, and the merged step-4 semantics don't allow that. So the priority applies at a single arbitration point, and PEER_CLOSED counts as eligible only when the inbound queue is empty. Record this as a clarification for the owner to confirm, not as an open design question.
3. **Codex's ABI escalation #4 is partly answered already.** D6 freezes the copy-fault status: `JE_ABI_INVALID`, message stays queued, no dequeue on a faulting copy-out. Only the deadline register, zero-timeout behaviour and the maximum deadline remain open.
4. **Codex's multi-waiter escalation #3 is partly answered already.** `JE_EP_MAX_WAITERS = 2` exists (endpoint.h:56), and endpoint.c:13 asserts `REF_MAX >= QUEUE_DEPTH + MAX_WAITERS`, so two waiters per end is the configured design. A third waiter fails at its ref acquire. What needs a decision is the status a waiter beyond the limit gets.

## What Codex missed
- **Reservation interacts with copy faults.** The frozen fault rule leaves the message queued. If waiter A is WOKEN with message 0 reserved and its copy-out faults, A's terminal result is WOKEN with status `JE_ABI_INVALID`. Message 0 is then unreserved but waiter B is still WAITING next to a deliverable message, which breaks the invariant above. The fault-release path must re-run the wake helper for that end.
- **Reserving more than one message breaks the queue.** Out-of-order completion would dequeue from the middle of the ring. Rule: an end has at most one outstanding reservation, always on the head message, and the next waiter is woken only after the current one commits or releases. That keeps FIFO order without restructuring the queue.
- **Where the completion hook runs is the riskiest part of the `sched.c` change.** Dispatch is frame-return based (sched.c:140-159: pick, `je_uframe_check`, `cr3_load`, return the frame). The deferred copy-out has to run after `je_arch_cr3_load(threads[next].root)` and before the return, so it runs inside the scheduler. ADR D3 relies on the M3a exact-RIP fixup never routing a copy fault to the scheduler, but nobody has tested a fault taken from within `je_sched_interrupt`. That needs a dedicated test (unmap the receive buffer while the receiver is blocked) or an owner-accepted "unverified" label. The alternative of re-executing the syscall (rewinding RIP) is out, because it re-resolves the handle.
- **The deadline needs explicit bounds.** "Bounded" requires a maximum (for example a `JE_IPC_WAIT_MAX_TICKS` constant), an overflow-checked `ticks + duration`, and the inclusive comparison `wake <= ticks` that `je_thread_sleep` already uses, so the two clocks behave the same.

## Escalate to the owner (four items)
1. Whether deferred WOKEN copy-out, performed in the dispatch hook after the receiver's CR3 load, satisfies D7's "deliver". Also whether the exact-RIP fixup is approved for use inside `je_sched_interrupt`.
2. Confirm that priority is decided at one arbitration point and that PEER_CLOSED is eligible only on an empty queue, consistent with the merged drain-first behaviour.
3. CANCELLED in v1: internal helper with a unit test only, documented as unreachable from the ABI.
4. The remaining ABI details: deadline register, behaviour of a zero timeout, the maximum deadline, and the status for exceeding `JE_EP_MAX_WAITERS`.
[R2 CODEX] codex
Claude is right on several implementation constraints, but its proposal still leaves the central D7 arbitration underspecified.
Agree:
- Thread-owned waiter metadata minimizes `sched.c` churn, but it does not eliminate endpoint→scheduler wake coupling. Endpoint enqueue/close paths still need a bounded waiter scan or scheduler wake API.
- Peer death must actively wake blocked receivers at the endpoint close transition, not wait for their deadline.
- One outstanding head reservation per endpoint is required. Otherwise FIFO dequeue and copy-fault rollback become ambiguous.
- Deadline arithmetic needs an explicit bounded maximum, overflow checking, and the existing inclusive `wake <= ticks` rule.
- `CANCELLED` has no current ABI trigger. An internal cancellation helper plus a deterministic kernel test is reasonable, but its ABI unreachability must be documented.
Dispute:
1. The timer’s CAS cannot be justified merely by an invariant assertion.
A timer path that blindly performs `WAITING → TIMED_OUT` is correct only if every path that creates eligibility synchronously performs the competing transition first:
- enqueue: `WAITING → WOKEN`;
- peer close: `WAITING → PEER_CLOSED`;
- cancel: `WAITING → CANCELLED`;
- reservation release after copy fault: re-arbitrate and wake another waiter.
The copy-fault case Claude identified proves this invariant is easy to violate. “Assert or `JE_EP_CORRUPT`” is not an acceptable normal recovery path for a kernel mechanism. The design must specify one shared `je_ipc_wait_arbitrate()` helper, called under IF=0, and test that helper after reservation release.
The timer may then perform the final timeout CAS, but only after the same arbitration rules are applied or after the invariant is mechanically enforced. Otherwise the ISR can timeout a waiter while a queued message exists.
2. “ISR stamps timeout; syscall re-checks” is not the state-machine contract.
The terminal state must be selected by one CAS. A syscall re-check may validate the already-selected result and perform copy/commit, but it must not be a second authority that changes `TIMED_OUT` into `WOKEN`. If the syscall is allowed to override the ISR result, the transition is no longer a single terminal CAS.
Conversely, sender and close paths cannot merely enqueue or mark metadata. They must perform the `WAITING → WOKEN/PEER_CLOSED` transition in the same IF=0 critical section as the endpoint mutation.
3. The proposed deferred copy-out inside scheduler dispatch is the wrong default.
Copying after `cr3_load(next)` inside `je_sched_interrupt` introduces a new page-fault context in scheduler code. The provided facts only establish exact-RIP recovery for the M3a syscall path; they do not establish recovery for a fault originating inside the scheduler after an address-space switch. That is precisely the kind of untested assembly/frame interaction this owner gate is intended to prevent.
The safer shape is:
- wake path atomically selects the waiter result and, for WOKEN, reserves the head message;
- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
- the blocked syscall continuation resumes after park;
- syscall code performs owned-record copy-out and commit/release;
- copy faults return `JE_ABI_INVALID`, preserve the message, release the reservation, and re-arbitrate.

 succeeded in 0ms:
    for (uint32_t i = 0; i < t->capacity; ++i) {
        je_handle h = 0u; uint32_t type = 0u;
        if (je_handle_live_at(t, i, &h, &type) && type == JE_ENDPOINT_OBJ_TYPE)
            return -1;   /* a LIVE endpoint survived the walk -> fatal */
    }
    return released;
}

/* ---- non-blocking data IPC (M3c step 4, D6 / D9) ------------------------- */

uint8_t je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side)
{
    if (!rec || side > 1u) return 0u;
    return rec->end[side].inbound.count;
}

je_ep_status je_endpoint_send_preflight(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, size_t len)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len > JE_EP_MSG_MAX) return JE_EP_INVALID;

    /* Resolve + SEND right (BADINDEX -> STALE -> WRONGTYPE -> DENIED). */
    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_SEND, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if SEND not held */

    /* A message delivered on end `side` lands in the PEER end's inbound. If THIS
     * end is peer-closed, nobody will ever read it -> PEER_CLOSED. */
    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
    if (rec->end[side ^ 1u].inbound.count >= JE_EP_QUEUE_DEPTH)
        return JE_EP_QUEUE_FULL;
    return JE_EP_OK;
}

/* Send preflight WITHOUT the QUEUE_FULL check: main-handle resolve + SEND right +
 * self-end PEER_CLOSED only. The glue runs this first, then resolves/validates the
 * caps (so per-cap DENIED/STALE outrank QUEUE_FULL per A5-4's frozen precedence),
 * and the final pure try_send_caps enforces QUEUE_FULL atomically at enqueue. */
je_ep_status je_endpoint_send_preflight_main(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len > JE_EP_MSG_MAX) return JE_EP_INVALID;
    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_SEND, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);
    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
    return JE_EP_OK;   /* NO queue-full check here */
}

je_ep_status je_endpoint_try_send(je_endpoint_pool *pool, je_handle_table *t,
                                  je_handle h, const void *kbuf, size_t len)
{
    /* Data-only send is exactly the zero-cap case. Delegate so the two paths
     * cannot drift (and the enqueue always writes cap_count[tail]=0). */
    return je_endpoint_try_send_caps(pool, t, h, kbuf, len, (const je_ep_capdesc *)0, 0u);
}

je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
                                   je_handle h, void *out_kbuf, size_t cap,
                                   size_t *out_len, je_ep_recv_lease *lease)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (!out_len) return JE_EP_INVALID;
    *out_len = 0u;

    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_RECV, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if RECV not held */

    je_ep_queue *q = &rec->end[side].inbound;    /* this end's own inbound */
    if (q->count == 0u) {
        /* Empty: EOF if the peer is gone, else would-block EMPTY. */
        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
    }
    uint8_t mlen = q->len[q->head];
    *out_len = mlen;                 /* report length even on SHORT_BUFFER */
    if (cap < mlen) return JE_EP_SHORT_BUFFER;   /* no copy, no dequeue */
    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;   /* NULL dst guard */
    for (uint8_t b = 0; b < mlen; ++b)
        ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
    if (lease) {
        lease->rec = rec;
        lease->side = side;
        lease->generation = rec->generation;
        lease->head = q->head;
        lease->len = mlen;
    }
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
                                     const je_ep_recv_lease *lease)
{
    /* Delegate to the cap-aware commit (which also releases any queued-cap
     * inflight refs and zeroes cap storage). A zero-cap message releases nothing;
     * same behaviour as before, but the two paths cannot drift. */
    return je_endpoint_recv_commit_caps(pool, lease);
}

/* ---- capability transfer (M3c step 5, ADR 0009 A5) ----------------------- */

bool je_endpoint_capdesc_live(const je_endpoint_pool *pool, const je_ep_capdesc *d)
{
    if (!pool || !d || d->rec_index >= JE_EP_POOL_CAP || d->side > 1u) return false;
    const je_ep_channel *tr = &pool->rec[d->rec_index];
    if (!tr->allocated || tr->generation != d->generation) return false; /* stale */
    je_ep_end_state st = tr->end[d->side].state;
    /* OPEN and PEER_CLOSED are LIVE (mintable); only CLOSED/FREE are dead (A5-2). */
    return st == JE_EP_END_OPEN || st == JE_EP_END_PEER_CLOSED;
}

je_ep_status je_endpoint_capdesc_resolve(je_endpoint_pool *pool,
                                         const je_ep_capdesc *d,
                                         je_ep_channel **out_rec, uint8_t *out_side)
{
    if (!pool || !d || !out_rec || !out_side) return JE_EP_CORRUPT;
    if (!je_endpoint_capdesc_live(pool, d)) return JE_EP_STALE;
    *out_rec = &pool->rec[d->rec_index];
    *out_side = d->side;
    return JE_EP_OK;
}

je_ep_status je_endpoint_capdesc_retain_handle(je_endpoint_pool *pool,
                                               const je_ep_capdesc *d)
{
    je_ep_channel *tr; uint8_t ts;
    je_ep_status s = je_endpoint_capdesc_resolve(pool, d, &tr, &ts);
    if (s != JE_EP_OK) return s;
    return retain_handle(&tr->end[ts]);   /* REFOVERFLOW if at the bound */
}

void je_endpoint_capdesc_release_handle(je_endpoint_pool *pool,
                                        const je_ep_capdesc *d)
{
    je_ep_channel *tr; uint8_t ts;
    if (je_endpoint_capdesc_resolve(pool, d, &tr, &ts) != JE_EP_OK) return;
    /* Undo a mint-path retain: drop one handle_ref (may free if it was the last,
     * but a mint rollback happens while the descriptor's inflight ref still holds
     * the target, so this cannot reach 0). */
    release_handle(pool, tr, ts);
}

/* Validate the batch of descriptors the glue resolved: count bound, each target
 * live, each descriptor's rights a subset of the target's CURRENT rights. Pure. */
static je_ep_status caps_validate(je_endpoint_pool *pool, const je_ep_capdesc *caps,
                                  uint8_t ncaps)
{
    if (ncaps > JE_EP_MSG_CAPS_MAX) return JE_EP_INVALID;
    for (uint8_t c = 0; c < ncaps; ++c) {
        je_ep_channel *tr; uint8_t ts;
        je_ep_status s = je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts);
        if (s != JE_EP_OK) return s;               /* dead target at send -> STALE */
        /* Descriptor rights must contain only known endpoint bits (defence in
         * depth; the GLUE attenuates against the sender's HANDLE rights before
         * building the descriptor — ends themselves carry no rights to check). */
        if (caps[c].rights & ~(uint32_t)JE_EP_RIGHTS_ALL) return JE_EP_INVALID;
        (void)tr; (void)ts;
    }
    return JE_EP_OK;
}

je_ep_status je_endpoint_send_preflight_caps(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len, const je_ep_capdesc *caps,
                                             uint8_t ncaps)
{
    je_ep_status base = je_endpoint_send_preflight(pool, t, h, len);
    if (base != JE_EP_OK) return base;
    if (ncaps == 0u) return JE_EP_OK;
    if (!caps) return JE_EP_INVALID;
    return caps_validate(pool, caps, ncaps);
}

je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *t,
                                       je_handle h, const void *kbuf, size_t len,
                                       const je_ep_capdesc *caps, uint8_t ncaps)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len != 0u && !kbuf) return JE_EP_INVALID;
    /* Full preflight (byte + caps) — same validator, re-run so check+enqueue is
     * atomic under IF=0. */
    je_ep_status pf = je_endpoint_send_preflight_caps(pool, t, h, len, caps, ncaps);
    if (pf != JE_EP_OK) return pf;
    (void)je_endpoint_resolve(pool, t, h, &rec, &side);   /* cannot fail now */

    /* RETAIN one inflight ref per cap on each target, rolling back all on any
     * REFOVERFLOW (all-or-nothing). */
    uint8_t retained = 0u;
    for (uint8_t c = 0; c < ncaps; ++c) {
        je_ep_channel *tr; uint8_t ts;
        if (je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts) != JE_EP_OK) {
            /* target died between preflight and here — impossible under IF=0;
             * roll back and fail closed. */
            for (uint8_t r = 0; r < retained; ++r) {
                je_ep_channel *rr; uint8_t rs;
                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
                if (rr->end[rs].inflight_refs) rr->end[rs].inflight_refs--;
            }
            return JE_EP_STALE;
        }
        if (tr->end[ts].inflight_refs >= JE_EP_REF_MAX) {
            for (uint8_t r = 0; r < retained; ++r) {
                je_ep_channel *rr; uint8_t rs;
                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
                if (rr->end[rs].inflight_refs) rr->end[rs].inflight_refs--;
            }
            return JE_EP_REFOVERFLOW;
        }
        tr->end[ts].inflight_refs++;
        retained++;
    }

    /* Enqueue the message (bytes + cap descriptors) at the peer inbound tail. */
    je_ep_queue *q = &rec->end[side ^ 1u].inbound;
    uint8_t tail = (uint8_t)((q->head + q->count) % JE_EP_QUEUE_DEPTH);
    for (uint8_t b = 0; b < (uint8_t)len; ++b)
        q->buf[tail][b] = ((const uint8_t *)kbuf)[b];
    q->len[tail] = (uint8_t)len;
    for (uint8_t c = 0; c < ncaps; ++c) q->cap[tail][c] = caps[c];
    q->cap_count[tail] = ncaps;
    q->count++;
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, void *out_kbuf, size_t cap,
                                        size_t *out_len, uint8_t hcap,
                                        je_ep_capdesc *out_caps, uint8_t *out_ncaps,
                                        je_ep_recv_lease *lease)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (!out_len || !out_ncaps) return JE_EP_INVALID;
    *out_len = 0u; *out_ncaps = 0u;

    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_RECV, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);

    je_ep_queue *q = &rec->end[side].inbound;
    if (q->count == 0u)
        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;

    uint8_t mlen = q->len[q->head];
    uint8_t mcaps = q->cap_count[q->head];
    *out_len = mlen;
    *out_ncaps = mcaps;
    /* Short EITHER buffer -> SHORT_BUFFER, copy nothing, no dequeue (report both
     * required sizes via out_len / out_ncaps). */
    if (cap < mlen || hcap < mcaps) return JE_EP_SHORT_BUFFER;
    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;
    if (mcaps != 0u && !out_caps) return JE_EP_INVALID;
    for (uint8_t b = 0; b < mlen; ++b) ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
    for (uint8_t c = 0; c < mcaps; ++c) out_caps[c] = q->cap[q->head][c];
    if (lease) {
        lease->rec = rec; lease->side = side;
        lease->generation = rec->generation; lease->head = q->head; lease->len = mlen;
    }
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
                                          const je_ep_recv_lease *lease)
{
    if (!pool || !lease || !lease->rec) return JE_EP_CORRUPT;
    je_ep_channel *rec = lease->rec;
    if (!rec_in_pool(pool, rec)) return JE_EP_CORRUPT;
    if (!rec->allocated || rec->generation != lease->generation || lease->side > 1u)
        return JE_EP_CORRUPT;
    je_ep_queue *q = &rec->end[lease->side].inbound;
    if (q->count == 0u || q->head != lease->head || q->len[q->head] != lease->len)
        return JE_EP_CORRUPT;
    /* Snapshot this head's caps BEFORE zeroing, then release each one's inflight
     * ref (the cap has been delivered — minted or consumed-as-dead by the glue). */
    uint8_t mcaps = q->cap_count[q->head];
    je_ep_capdesc snap[JE_EP_MSG_CAPS_MAX];
    for (uint8_t c = 0; c < mcaps; ++c) snap[c] = q->cap[q->head][c];
    /* Dequeue: zero the vacated slot (bytes + caps), advance. */
    q->len[q->head] = 0u;
    q->cap_count[q->head] = 0u;
    for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[q->head][b] = 0u;
    for (uint8_t c = 0; c < JE_EP_MSG_CAPS_MAX; ++c) {
        q->cap[q->head][c].rec_index = 0u; q->cap[q->head][c].side = 0u;
        q->cap[q->head][c].cap_count_pad = 0u;
        q->cap[q->head][c].generation = 0u; q->cap[q->head][c].rights = 0u;
    }
    q->head = (uint8_t)((q->head + 1u) % JE_EP_QUEUE_DEPTH);
    q->count--;
    /* Now release each delivered cap's inflight ref (may free the target). Done
     * AFTER dequeue so the queue slot no longer references the target. */
    for (uint8_t c = 0; c < mcaps; ++c) release_capdesc_inflight(pool, &snap[c]);
    return JE_EP_OK;
}
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3c sub-PR 3 (ADR 0009 D-SEQ step 3): the kernel's single endpoint pool and
 * the je_vm wiring (teardown hook + IRQ critical section). This is the only file
 * that holds endpoint global state; endpoint.c stays pure/hostable. Lives in the
 * real kernel build only (NOT linked into the hosted unit tests), so it may use
 * the arch IRQ primitives and the kernel pool freely.
 */
#include <jeios/endpoint_kern.h>
#include <jeios/arch.h>

/* The one kernel-wide pool. A dying process's endpoints are reclaimed into it by
 * the teardown hook; a leaked record would eventually exhaust it (the whole
 * point of A3-1). */
static je_endpoint_pool g_kep;
static bool g_kep_inited;

static void kep_init_once(void)
{
    if (!g_kep_inited) { je_endpoint_pool_init(&g_kep); g_kep_inited = true; }
}

/* Teardown hook: the release walk over the dying process's table. A negative
 * return is an invariant violation (pool corruption) and MUST stop the kernel
 * before the table is zeroed — continuing would zero away the leak evidence and
 * ship a lie. */
static void kep_teardown(void *ctx, je_handle_table *t)
{
    je_endpoint_pool *pool = (je_endpoint_pool *)ctx;
    if (je_endpoint_release_table(pool, t) < 0) {
        je_arch_console_write("JeiOS fatal: endpoint release walk failed "
                              "(pool corrupt)\r\n");
        je_arch_halt();
    }
}

void je_endpoint_kern_install(je_vm *vm)
{
    if (!vm) return;
    kep_init_once();
    vm->teardown     = kep_teardown;
    vm->teardown_ctx = &g_kep;
    vm->irq_save     = je_arch_irq_save;
    vm->irq_restore  = je_arch_irq_restore;
}

je_ep_status je_endpoint_kern_create(je_vm *vm, uint32_t index,
                                     je_handle *out0, je_handle *out1)
{
    if (out0) *out0 = 0u;
    if (out1) *out1 = 0u;
    if (!vm || index >= JE_VM_MAX_PROCESSES || !out0 || !out1)
        return JE_EP_BADINDEX;
    /* STRUCTURAL GUARANTEE (A3-2): refuse to mint an endpoint into a table whose
     * death path will not reclaim it atomically. Require the FULL wiring — the
     * teardown hook AND the IRQ critical-section pair — not just the hook, so a
     * partially-wired vm cannot mint without the A3-1 critical section. */
    if (vm->teardown != kep_teardown || vm->teardown_ctx != &g_kep
        || vm->irq_save == (uint64_t (*)(void))0
        || vm->irq_restore == (void (*)(uint64_t))0)
        return JE_EP_CORRUPT;
    kep_init_once();
    je_handle_table *t = &vm->procs[index].htable;
    return je_endpoint_channel_create(&g_kep, t, JE_EP_RIGHTS_ALL,
                                      t, JE_EP_RIGHTS_ALL, out0, out1);
}

je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
}

/* Data IPC forwarders (M3c step 4): resolve the caller's table and run the pure
 * send/peek/commit against the kernel pool. The glue (syscall.c) does the
 * copy_from/to_user around these, honoring D6 ordering. */
je_ep_status je_endpoint_kern_send(je_vm *vm, uint32_t index, je_handle h,
                                   const void *kbuf, size_t len)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_try_send(&g_kep, &vm->procs[index].htable, h, kbuf, len);
}

je_ep_status je_endpoint_kern_send_preflight(je_vm *vm, uint32_t index,
                                             je_handle h, size_t len)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    /* Main-handle preflight only (no QUEUE_FULL) so per-cap errors outrank
     * QUEUE_FULL in the cap-send glue (A5-4). The final try_send_caps re-checks
     * QUEUE_FULL atomically at enqueue. */
    return je_endpoint_send_preflight_main(&g_kep, &vm->procs[index].htable, h, len);
}

je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
                                        void *out_kbuf, size_t cap,
                                        size_t *out_len, je_ep_recv_lease *lease)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_recv_peek(&g_kep, &vm->procs[index].htable, h,
                                 out_kbuf, cap, out_len, lease);
}

je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease)
{
    kep_init_once();
    return je_endpoint_recv_commit_caps(&g_kep, lease);
}

/* ---- capability transfer forwarders (M3c step 5) ------------------------- */

je_ep_status je_endpoint_kern_send_caps(je_vm *vm, uint32_t index, je_handle h,
                                        const void *kbuf, size_t len,
                                        const je_ep_capdesc *caps, uint8_t ncaps)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_try_send_caps(&g_kep, &vm->procs[index].htable, h,
                                     kbuf, len, caps, ncaps);
}

je_ep_status je_endpoint_kern_resolve_send_cap(je_vm *vm, uint32_t index,
                                               je_handle h, uint32_t req_rights,
                                               je_ep_capdesc *out)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES || !out) return JE_EP_BADINDEX;
    kep_init_once();
    je_handle_table *tbl = &vm->procs[index].htable;
    /* The handle must be an endpoint holding TRANSFER. */
    je_ep_channel *rec; uint8_t side;
    je_ep_status rs = je_endpoint_resolve(&g_kep, tbl, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(tbl, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_TRANSFER, &rights);
    if (gr != JE_HANDLE_OK) {
        /* map DENIED/etc via the same table mapping the pure layer uses */
        switch (gr) {
        case JE_HANDLE_BADINDEX:  return JE_EP_BADINDEX;
        case JE_HANDLE_STALE:     return JE_EP_STALE;
        case JE_HANDLE_WRONGTYPE: return JE_EP_WRONGTYPE;
        case JE_HANDLE_DENIED:    return JE_EP_DENIED;
        default:                  return JE_EP_CORRUPT;
        }
    }
    /* Attenuation: the requested rights must be a subset of what the handle holds. */
    if (req_rights & ~rights) return JE_EP_DENIED;
    /* Build the descriptor: the target end is the handle's OWN end (the receiver
     * will get a handle to the same end the sender named). rec_index via pointer
     * arithmetic into the kernel pool. */
    out->rec_index = (uint16_t)(rec - &g_kep.rec[0]);
    out->side = side;
    out->cap_count_pad = 0u;
    out->generation = rec->generation;
    out->rights = req_rights;
    return JE_EP_OK;
}

je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
                                             void *out_kbuf, size_t cap,
                                             size_t *out_len, uint8_t hcap,
                                             je_ep_capdesc *out_caps,
                                             uint8_t *out_ncaps,
                                             je_ep_recv_lease *lease)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
                                      out_kbuf, cap, out_len, hcap,
                                      out_caps, out_ncaps, lease);
}

je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
                                            const je_ep_capdesc *d, je_handle *out)
{
    if (out) *out = 0u;
    if (!vm || index >= JE_VM_MAX_PROCESSES || !d || !out) return JE_EP_BADINDEX;
    kep_init_once();
    je_handle_table *tbl = &vm->procs[index].htable;
    je_ep_channel *tr; uint8_t ts;
    je_ep_status s = je_endpoint_capdesc_resolve(&g_kep, d, &tr, &ts);
    if (s != JE_EP_OK) return s;                 /* dead target -> STALE (A5-2) */
    /* Retain-before-commit: retain a handle_ref on the target, then alloc the
     * receiver handle; roll back the retain on alloc failure. */
    je_ep_status rr = je_endpoint_capdesc_retain_handle(&g_kep, d);
    if (rr != JE_EP_OK) return rr;               /* REFOVERFLOW */
    je_handle nh = 0u;
    je_handle_status a = je_handle_alloc(tbl, (void *)&tr->end[ts],
                                         JE_ENDPOINT_OBJ_TYPE, d->rights, &nh);
    if (a != JE_HANDLE_OK) {
        je_endpoint_capdesc_release_handle(&g_kep, d);   /* undo retain */
        switch (a) {
        case JE_HANDLE_QUOTA:   return JE_EP_QUOTA;
        case JE_HANDLE_NOSLOTS: return JE_EP_NOSLOTS;
        default:                return JE_EP_CORRUPT;
        }
    }
    *out = nh;
    return JE_EP_OK;
}

bool je_endpoint_kern_capdesc_live(const je_ep_capdesc *d)
{
    return je_endpoint_capdesc_live(&g_kep, d);
}

void je_endpoint_kern_capdesc_release_handle(const je_ep_capdesc *d)
{
    je_endpoint_capdesc_release_handle(&g_kep, d);
}

je_ep_status je_endpoint_kern_seed(je_vm *vm, uint32_t idxA, uint32_t idxB,
                                   je_handle *outA, je_handle *outB)
{
    if (outA) *outA = 0u;
    if (outB) *outB = 0u;
    if (!vm || idxA >= JE_VM_MAX_PROCESSES || idxB >= JE_VM_MAX_PROCESSES
        || idxA == idxB || !outA || !outB) return JE_EP_BADINDEX;
    /* A5-4: both VMs must be hooked so the seeded ends are on a reclaiming death
     * path (the raw channel_create bypasses op-10's A3-2 hook check). */
    if (vm->teardown != kep_teardown || vm->teardown_ctx != &g_kep
        || vm->irq_save == (uint64_t (*)(void))0
        || vm->irq_restore == (void (*)(uint64_t))0) return JE_EP_CORRUPT;
    kep_init_once();
    return je_endpoint_channel_create(&g_kep, &vm->procs[idxA].htable,
                                      JE_EP_RIGHTS_ALL,
                                      &vm->procs[idxB].htable, JE_EP_RIGHTS_ALL,
                                      outA, outB);
}

uint32_t je_endpoint_kernel_allocated(void)
{
    return g_kep_inited ? je_endpoint_pool_allocated(&g_kep) : 0u;
}
}

/* M3c (A3-2): a handle packs losslessly into half of the RDX scalar only because
 * it is 32 bits. The endpoint_create two-handle return depends on this. */
_Static_assert(sizeof(je_handle) == 4u, "endpoint_create packs two handles in RDX");
/* The table-side endpoint type/rights copies must equal the ABI ones the glue
 * uses, or typed dispatch and create would disagree. */
_Static_assert(JE_ENDPOINT_OBJ_TYPE == JE_ABI_OBJ_ENDPOINT, "endpoint type: table vs ABI");
_Static_assert(JE_EP_RIGHT_SEND == JE_ABI_RIGHT_SEND
               && JE_EP_RIGHT_RECV == JE_ABI_RIGHT_RECV
               && JE_EP_RIGHT_TRANSFER == JE_ABI_RIGHT_TRANSFER
               && JE_EP_RIGHT_INSPECT == JE_ABI_RIGHT_INSPECT,
               "endpoint rights: table vs ABI");
_Static_assert(JE_EP_MSG_MAX == JE_ABI_MSG_MAX, "endpoint msg max: table vs ABI");
_Static_assert(JE_EP_MSG_CAPS_MAX == JE_ABI_MSG_CAPS_MAX, "endpoint caps max: table vs ABI");

/* Map endpoint-layer status to the v0 ABI (reuses the JE_ABI_H_* namespace; the
 * glue is the only place endpoint.h and abi.h meet). */
static je_abi_status ep_status_to_abi(je_ep_status s)
{
    switch (s) {
    case JE_EP_OK:          return JE_ABI_OK;
    case JE_EP_BADINDEX:    return JE_ABI_H_BADINDEX;
    case JE_EP_STALE:       return JE_ABI_H_STALE;
    case JE_EP_WRONGTYPE:   return JE_ABI_H_WRONGTYPE;
    case JE_EP_DENIED:      return JE_ABI_H_DENIED;
    case JE_EP_QUOTA:       return JE_ABI_H_QUOTA;
    case JE_EP_NOSLOTS:     return JE_ABI_H_NOSLOTS;
    case JE_EP_REFOVERFLOW: return JE_ABI_H_REFOVERFLOW;
    case JE_EP_QUEUE_FULL:  return JE_ABI_H_QUEUE_FULL;
    case JE_EP_SHORT_BUFFER: return JE_ABI_H_SHORT_BUFFER;
    case JE_EP_PEER_CLOSED: return JE_ABI_H_PEER_CLOSED;
    case JE_EP_EMPTY:       return JE_ABI_H_EMPTY;
    case JE_EP_INVALID:     return JE_ABI_INVALID;
    case JE_EP_CORRUPT:     return JE_ABI_UNSUPPORTED; /* unwired vm / invariant */
    }
    return JE_ABI_INVALID;
}

/* Kernel glue. Returns the frame to iretq to (the caller for RETURN/invalid, a
 * live successor for YIELD/EXIT). Must only be called for a user-origin int 0x80
 * frame (je_trap_dispatch enforces cs&3==3; the scheduler helpers re-check the
 * frame identity). */
je_trap_frame *je_syscall_dispatch(je_trap_frame *f)
{
    uint64_t args[6] = { f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9 };
    je_abi_status status; uint64_t result;
    je_syscall_action action = je_syscall_decode(f->rax, args, &status, &result);

    switch (action) {
    case JE_SYSCALL_YIELD:
        /* Write the OK result into the CALLER's saved frame BEFORE switching, so
         * when this task is next resumed its RAX already reads OK. Reuse the
         * existing kind-aware reschedule path (same as the 0x81 kernel yield);
         * no recursion, no M2 sleep (D10). */
        f->rax = (uint64_t)status; f->rdx = result;
        return je_sched_interrupt(f, false);
    case JE_SYSCALL_EXIT:
        return je_sched_exit_current(f, result);
    case JE_SYSCALL_COPY_PROBE: {
        /* dev-only: stage <=256 user bytes src->kernel->dst with prevalidation
         * and the exception-fixup net. src=RDI, dst=RSI, len=RDX. */
        je_abi_status s = je_copy_probe(f->rdi, f->rsi, f->rdx);
        f->rax = (uint64_t)s; f->rdx = 0;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_SELF: {
        /* Mint a SELF handle in the caller's own table. SELF carries no object
         * pointer; it is granted the INSPECT right (JE_HANDLE_RIGHTS_SELF). A
         * freshly minted SELF holds INSPECT, so DENIED is not observable on it
         * directly — but handle_dup can derive a rights-reduced handle on which
         * handle_rights then returns DENIED (ring-3-observable). Handle in RDX. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = 0;
        je_handle_status s = je_handle_alloc(t, (void *)0, JE_HANDLE_TYPE_SELF,
                                             JE_HANDLE_RIGHTS_SELF, &h);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (uint64_t)h;                 /* 0 on failure (alloc clears out) */
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_CLOSE: {
        /* Typed close dispatch (A3-3): peek the slot's object type, then branch
         * EXACTLY — SELF -> raw table close; ENDPOINT -> endpoint-aware close
         * (drops the handle_ref); any other/unknown type -> fail closed. The peek
         * and the close are atomic because this int 0x80 path runs at IF=0 on one
         * CPU (SMP revisit). A raw je_handle_close on an endpoint slot would leak
         * a ref, which is exactly what this dispatch prevents. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle_table *t = &vm->procs[index].htable;
        je_handle h = (je_handle)f->rdi;
        uint32_t type = 0u; je_handle peek = 0u;
        /* The decoder already rejected a wide handle (rdi>>32) as INVALID. Decode
         * to an index and read the slot type via the read-only iterator, matching
         * on the same generation by re-encoding and comparing. */
        uint32_t di = 0u, dg = 0u; je_handle_decode(h, &di, &dg);
        bool live = je_handle_live_at(t, di, &peek, &type) && peek == h;
        if (live && type == JE_ABI_OBJ_ENDPOINT) {
            je_ep_status s = je_endpoint_kern_close(vm, index, h);
            f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
        } else if (live && type != JE_ABI_OBJ_SELF) {
            /* A LIVE slot of a type that is neither ENDPOINT nor SELF must FAIL
             * CLOSED (A3-3): routing it to raw je_handle_close would drop a ref a
             * future typed object owns. Unreachable today (only SELF/ENDPOINT are
             * minted), but the contract is enforced, not assumed. */
            f->rax = (uint64_t)JE_ABI_H_WRONGTYPE; f->rdx = 0;
        } else {
            /* SELF, or a not-live handle (stale/forged/free/retired): raw close
             * gives the exact OK/BADINDEX/STALE status. */
            je_handle_status s = je_handle_close(t, h);
            f->rax = (uint64_t)handle_status_to_abi(s); f->rdx = 0;
        }
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_QUERY: {
        /* Look up the handle for its object type. No expected-type/rights are
         * required from ring 3 in M3b, so WRONGTYPE/DENIED are not user-reachable
         * here (they are covered by the hosted table tests and the mapper). On OK,
         * RDX = object type; otherwise RDX = 0. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        void *obj;
        je_handle_status s = je_handle_lookup(t, (je_handle)f->rdi,
                                              JE_HANDLE_TYPE_SELF, 0u, &obj);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (s == JE_HANDLE_OK) ? (uint64_t)JE_HANDLE_TYPE_SELF : 0u;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_RIGHTS: {
        /* Look up the handle requiring the INSPECT right, and return its rights
         * mask (masked to the known ABI bits, so an internal bit can never leak).
         * DENIED here IS ring-3-observable: handle_dup can derive a handle that
         * lacks INSPECT, and this op on that handle returns DENIED (proven by the
         * M3b-s1 QEMU marker). */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        uint32_t rights = 0;
        je_handle_status s = je_handle_rights_of(t, (je_handle)f->rdi,
                                                 JE_HANDLE_TYPE_SELF,
                                                 JE_ABI_RIGHT_INSPECT, &rights);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (s == JE_HANDLE_OK) ? (uint64_t)(rights & JE_ABI_RIGHTS_KNOWN) : 0u;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_DUP: {
        /* Attenuate: derive a new SELF handle (RDI) with a rights SUBSET (RSI).
         * The decoder already rejected unknown rights bits as INVALID; here a
         * request for a known-but-unheld right returns DENIED (never escalates).
         * The new handle is a fresh, independently-closable slot naming the same
         * SELF object; it counts against quota. This is where DENIED becomes
         * genuinely ring-3-observable: dup(self, 0) yields a rights-less handle on
         * which handle_rights then returns DENIED. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle nh = 0;
        je_handle_status s = je_handle_dup_self(t, (je_handle)f->rdi,
                                                JE_HANDLE_TYPE_SELF,
                                                (uint32_t)f->rsi, &nh);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (uint64_t)nh;                /* 0 on failure (dup clears out) */
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_ENDPOINT_CREATE: {
        /* endpoint_create() (A3-2): mint both ends into the caller's own table
         * via the kernel pool wrapper, which refuses unless this vm has the
         * teardown hook installed (UNSUPPORTED) so a created endpoint is always
         * on a death path that reclaims it. On OK, pack the two 32-bit handles:
         * RDX[31:0]=h0, RDX[63:32]=h1. RDX is left 0 on failure (undefined by
         * ABI, but we zero it rather than leak a stale value). */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h0 = 0u, h1 = 0u;
        je_ep_status s = je_endpoint_kern_create(vm, index, &h0, &h1);
        f->rax = (uint64_t)ep_status_to_abi(s);
        f->rdx = (s == JE_EP_OK)
               ? ((uint64_t)h0 | ((uint64_t)h1 << 32)) : 0u;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_TRY_SEND: {
        /* A5-4 send: resolve caps (if any) in the sender's table FIRST (read-only
         * -> TRANSFER + attenuation), preflight bytes+caps, copy bytes to stack
         * staging, then pure all-or-nothing enqueue (bytes + cap descriptors +
         * per-cap inflight retain). Cap-array copy fault ranks last (after a clean
         * preflight), per the frozen precedence. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = (je_handle)f->rdi;
        uint64_t ubuf = f->rsi, len = f->rdx;
        uint8_t ncaps = (uint8_t)(f->r10 & 0xffu);
        uint64_t caparr = f->r8;
        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
        /* Resolve each user cap entry {low32 handle, high32 rights} -> descriptor.
         * Copy the array in first (a fault here is INVALID but only after the main
         * handle is validated by the preflight below — so do the base preflight
         * (len + main handle SEND/state/full) FIRST). */
        je_ep_status bpf = je_endpoint_kern_send_preflight(vm, index, h, (size_t)len);
        if (bpf != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(bpf); f->rdx=0; return je_sched_return_to_user(f); }
        if (ncaps != 0u) {
            uint64_t raw[JE_ABI_MSG_CAPS_MAX];
            je_abi_status cs = je_copy_from_user(raw, caparr,
                                                 (uint64_t)ncaps * sizeof(uint64_t));
            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
            for (uint8_t c = 0; c < ncaps; ++c) {
                je_handle ch = (je_handle)(raw[c] & 0xffffffffu);
                uint32_t crights = (uint32_t)(raw[c] >> 32);
                if (crights & ~(uint32_t)JE_ABI_RIGHTS_KNOWN) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
                je_ep_status rc = je_endpoint_kern_resolve_send_cap(vm, index, ch, crights, &caps[c]);
                if (rc != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(rc); f->rdx=0; return je_sched_return_to_user(f); }
            }
        }
        uint8_t staging[JE_EP_MSG_MAX];
        if (len != 0u) {
            je_abi_status cs = je_copy_from_user(staging, ubuf, len);
            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        }
        je_ep_status s = je_endpoint_kern_send_caps(vm, index, h, staging,
                                                    (size_t)len, caps, ncaps);
        f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_TRY_RECV: {
        /* A5-4 recv: PEEK (bytes + cap descriptors, no dequeue) -> stage all live
         * mints in the receiver's table (dead caps -> handle 0, A5-2) -> copy bytes
         * out -> copy handle array out -> INFALLIBLE commit (dequeue + release each
         * cap's inflight). Any transient fault rolls back every staged mint and
         * leaves the message queued. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = (je_handle)f->rdi;
        uint64_t ubuf = f->rsi, cap = f->rdx, hbuf = f->r8, hcap = f->r9;
        uint8_t staging[JE_EP_MSG_MAX];
        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
        size_t mlen = 0u; uint8_t mcaps = 0u;
        je_ep_recv_lease lease;
        je_ep_status s = je_endpoint_kern_recv_peek_caps(vm, index, h, staging,
                                                         (size_t)cap, &mlen,
                                                         (uint8_t)hcap, caps, &mcaps,
                                                         &lease);
        if (s == JE_EP_SHORT_BUFFER) {
            f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
            f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32);
            return je_sched_return_to_user(f);
        }
        if (s != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return je_sched_return_to_user(f); }
        /* Stage mints: a live cap mints a fresh handle; a dead cap stages 0 (A5-2).
         * Roll back all minted-so-far on any transient failure (QUOTA/NOSLOTS/
         * REFOVERFLOW). */
        je_handle minted[JE_ABI_MSG_CAPS_MAX];
        bool was_live[JE_ABI_MSG_CAPS_MAX];
        for (uint8_t c = 0; c < mcaps; ++c) { minted[c] = 0u; was_live[c] = false; }
        je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
        for (uint8_t c = 0; c < mcaps; ++c) {
            if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c] = 0u; continue; }
            je_handle nh = 0u;
            je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
            if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
            minted[c] = nh; was_live[c] = true; done = (uint8_t)(c + 1u);
        }
        if (fail != JE_ABI_OK) {
            for (uint8_t c = 0; c < done; ++c)
                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
            f->rax=(uint64_t)fail; f->rdx=0; return je_sched_return_to_user(f);
        }
        /* Copy bytes out. */
        if (mlen != 0u) {
            je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
            if (cs != JE_ABI_OK) {
                for (uint8_t c = 0; c < mcaps; ++c)
                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
            }
        }
        /* Copy minted handle array out (u32 each). */
        if (mcaps != 0u) {
            uint32_t hout[JE_ABI_MSG_CAPS_MAX];
            for (uint8_t c = 0; c < mcaps; ++c) hout[c] = (uint32_t)minted[c];
            je_abi_status cs = je_copy_to_user(hbuf, hout,
                                               (uint64_t)mcaps * sizeof(uint32_t));
            if (cs != JE_ABI_OK) {
                for (uint8_t c = 0; c < mcaps; ++c)

 succeeded in 0ms:
docs/IMPLEMENTATION.md:3:Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
docs/IMPLEMENTATION.md:50:A portable Multiboot2 parser copies bounded memory-map records and reservation
docs/IMPLEMENTATION.md:74:overflow, overlap, module and reservation cases pass ASan/UBSan. Local leak
docs/IMPLEMENTATION.md:87:An entry is reserved, free, an exact allocation head/length, or a continuation.
docs/IMPLEMENTATION.md:89:reservations. Allocation searches for a contiguous run, limited to 256 pages.
docs/IMPLEMENTATION.md:648:(queued caps + waiters; both empty this PR) — and the record is freed only when
docs/IMPLEMENTATION.md:650:policy bound (`_Static_assert JE_EP_REF_MAX >= JE_EP_QUEUE_DEPTH + JE_EP_MAX_WAITERS`),
docs/IMPLEMENTATION.md:802:## M3c non-blocking data IPC (try_send / try_recv) — 2026-10-18
docs/IMPLEMENTATION.md:812:(deliver to peer); `try_recv` on end `j` drains `end[j].inbound`. EOF/PEER_CLOSED
docs/IMPLEMENTATION.md:831:- **Recv:** pure `recv_peek` copies the head message into a kernel-stack buffer
docs/IMPLEMENTATION.md:835:  `recv_commit` dequeues. A commit lease mismatch (impossible under IF=0
docs/IMPLEMENTATION.md:851:**ABI (ops 11/12, statuses 10–13).** `try_send`=11, `try_recv`=12; statuses
docs/IMPLEMENTATION.md:854:table-side bound. recv returns bytes-written (or required-len on SHORT_BUFFER) in
docs/IMPLEMENTATION.md:860:out-register/out-struct extension — it must NOT overload `try_recv`'s RDX**. This
docs/IMPLEMENTATION.md:863:**Proofs.** Hosted `test_endpoint` gains the full battery: send/recv roundtrip,
docs/IMPLEMENTATION.md:864:direction (recv on the sender's end is EMPTY), FIFO, QUEUE_FULL, SHORT_BUFFER (no
docs/IMPLEMENTATION.md:865:dequeue), PEER_CLOSED drain-then-EOF, rights DENIED (send without SEND / recv
docs/IMPLEMENTATION.md:890:## M3c capability transfer (try_send/try_recv with caps) — 2026-10-19
docs/IMPLEMENTATION.md:896:receiver's `try_recv` delivers the bytes AND mints fresh, independently-closable,
docs/IMPLEMENTATION.md:914:`je_endpoint_capdesc_live` classifies; the recv glue stages `0` for a dead cap.
docs/IMPLEMENTATION.md:926:recv R8 = user `u32[]` handle output, R9 = slot capacity, R8≠0 iff R9≠0; recv
docs/IMPLEMENTATION.md:942:(the kernel send/recv cap FORWARDERS — the real pure + kern logic: seed S, mint C
docs/AGENT_START.md:5:and continuation order; it does not claim later milestones are implemented.
docs/ROADMAP.md:3:Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
docs/ROADMAP.md:14:| M1 | Exceptions, boot block validation, RAM reservations, correct mappings | Known exception produces bounded diagnostic; malformed boot maps rejected; kernel/data permissions match qualified CPU | M0 |
docs/ROADMAP.md:32:M1 has validated maps/reservations, fatal exceptions and hardware permission tests.
docs/DEVELOPMENT.md:3:Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
docs/DEVELOPMENT.md:42:M1: boot map validated; reservations ready
docs/adr/0009-m3c-capability-ipc.md:40:(`try_send`/`try_recv`, non-blocking first); **blocking waits** with deadline and
docs/adr/0009-m3c-capability-ipc.md:66:  remains); queued-capability and waiter refs keep the *channel record* alive for
docs/adr/0009-m3c-capability-ipc.md:70:  **capabilities** in that discarded queue are released (D3). A peer doing `recv`
docs/adr/0009-m3c-capability-ipc.md:76:  both `recv` from it. M3c does not promise exclusive receive; a message is
docs/adr/0009-m3c-capability-ipc.md:87:  transferred into a queue) and registered waiters. Keep the channel *record*
docs/adr/0009-m3c-capability-ipc.md:93:queued cap, no waiter references either end. The record is not itself a counted
docs/adr/0009-m3c-capability-ipc.md:97:a waiter's target) across a syscall boundary:
docs/adr/0009-m3c-capability-ipc.md:101:3. a **waiter** → `inflight_refs` on the end, AND the waiter record itself holds a
docs/adr/0009-m3c-capability-ipc.md:103:   process death before that thread's storage is reused (see D7) — a waiter→thread
docs/adr/0009-m3c-capability-ipc.md:112:waiter, mint a handle via `dup`/transfer) MUST **acquire its owning ref before it
docs/adr/0009-m3c-capability-ipc.md:114:uses the **waiter's owned ref**, never a re-resolve of the original handle.
docs/adr/0009-m3c-capability-ipc.md:126:of simultaneous *non-handle* refs the design can force (so a legitimate waiter or
docs/adr/0009-m3c-capability-ipc.md:128:value with a `_Static_assert` tying it to `JE_EP_QUEUE_DEPTH` + max waiters.
docs/adr/0009-m3c-capability-ipc.md:137:**all four**. `try_send` requires `SEND`, `try_recv` requires `RECV`, transfer
docs/adr/0009-m3c-capability-ipc.md:167:  copies into the kernel queue in the sender's context; recv copies out in the
docs/adr/0009-m3c-capability-ipc.md:177:Non-blocking `try_send`/`try_recv` come first (D-SEQ). Blocking is a later PR and
docs/adr/0009-m3c-capability-ipc.md:178:adds a bounded **waiter** with exactly one terminal transition producing the
docs/adr/0009-m3c-capability-ipc.md:182:WAITING -> WOKEN | TIMED_OUT | CANCELLED | PEER_CLOSED
docs/adr/0009-m3c-capability-ipc.md:188:  ticks (the existing M2 clock), and the **timer ISR performs the waiter's
docs/adr/0009-m3c-capability-ipc.md:189:  terminal transition itself** (it must do the `WAITING→TIMED_OUT` transition and
docs/adr/0009-m3c-capability-ipc.md:193:  is eligible at once, priority is fixed: **`PEER_CLOSED` > `WOKEN` > `CANCELLED`
docs/adr/0009-m3c-capability-ipc.md:194:  > `TIMED_OUT`**, and if a message is already queued when a receiver would block,
docs/adr/0009-m3c-capability-ipc.md:196:  a single compare-and-set on the waiter state under IF=0, so exactly one path
docs/adr/0009-m3c-capability-ipc.md:198:- **Receiver-only blocking in v1.** `recv` may block; **`send` does NOT block** in
docs/adr/0009-m3c-capability-ipc.md:201:- **Process death unlinks waiters (D3.3).** Destroying a process must remove or
docs/adr/0009-m3c-capability-ipc.md:202:  tombstone every waiter it owns *before* the thread/frame storage is reused, so a
docs/adr/0009-m3c-capability-ipc.md:203:  later wake/timeout cannot write stale memory. This, plus registering a waiter
docs/adr/0009-m3c-capability-ipc.md:210:tested; a blocked thread enters a new `THREAD_BLOCKED_IPC` scheduler state.
docs/adr/0009-m3c-capability-ipc.md:218:ADR 0008) → commit; **roll back every reservation/reference on any failure**
docs/adr/0009-m3c-capability-ipc.md:259:4. **Non-blocking data IPC:** `try_send`/`try_recv`, data-only (D6), with the
docs/adr/0009-m3c-capability-ipc.md:265:6. **Waits:** `THREAD_BLOCKED_IPC` + deadline + cancellation (D7). Edits `sched.c`
docs/adr/0009-m3c-capability-ipc.md:288:cap, or waiter) and **no escaped raw pointer to an end may survive its release**
docs/adr/0009-m3c-capability-ipc.md:453:an inflight reference** on the referenced end (D3.2). The receiver's `try_recv`
docs/adr/0009-m3c-capability-ipc.md:512:**since-CLOSED** end. Decision (**S1**): on `try_recv`, a cap whose target is no
docs/adr/0009-m3c-capability-ipc.md:588:extension, NOT overload `try_recv`'s RDX"): step-4 reserved R8/R9 and RDX[63:32]
docs/adr/0009-m3c-capability-ipc.md:590:is to extend `try_recv` via those reserved registers rather than add a duplicate
docs/adr/0009-m3c-capability-ipc.md:597:`R8 == 0` legal iff `cap_count == 0`. **recv** returns minted handles via `R8` =
docs/adr/0009-m3c-capability-ipc.md:662:transfer C.e1 as a cap over S via the real send/recv cap path, mint it into table
docs/adr/0009-m3c-capability-ipc.md:675:- *Option (a) — a new `try_recv_caps` op instead of extending `try_recv` via
docs/adr/0009-m3c-capability-ipc.md:676:  R8/R9:* a cleaner signature but a new op number and a near-duplicate recv path;
docs/adr/0009-m3c-capability-ipc.md:678:  and keeps one recv path.
docs/adr/0009-m3c-capability-ipc.md:686:`{handle, rights}` u64 send-element layout and the 0-means-dead recv encoding are
docs/adr/0009-m3c-capability-ipc.md:691:## AMENDMENT (2026-10-19, owner-approved) — step-6 blocking-recv (waits) decisions
docs/adr/0009-m3c-capability-ipc.md:695:`WAITING→WOKEN|TIMED_OUT|CANCELLED|PEER_CLOSED`, mandatory bounded deadline, timer
docs/adr/0009-m3c-capability-ipc.md:697:unlinks waiters, re-validate the user buffer at copy time after wake). It does not
docs/adr/0009-m3c-capability-ipc.md:704:sharpened the design materially (separate waiter vs scheduler state machines, one
docs/adr/0009-m3c-capability-ipc.md:710:**Decision + why.** The **waiter** state (`WAITING→WOKEN|TIMED_OUT|CANCELLED|
docs/adr/0009-m3c-capability-ipc.md:712:(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
docs/adr/0009-m3c-capability-ipc.md:713:pure helper **`je_ipc_wait_arbitrate(waiter, cause)`** performs the terminal
docs/adr/0009-m3c-capability-ipc.md:717:enqueue → `WOKEN`, peer close → `PEER_CLOSED`, timer ISR deadline → `TIMED_OUT`,
docs/adr/0009-m3c-capability-ipc.md:718:internal cancel → `CANCELLED`. The parked **syscall continuation** then *validates
docs/adr/0009-m3c-capability-ipc.md:721:adversarial review showed "ISR stamps TIMED_OUT, syscall re-checks and may upgrade
docs/adr/0009-m3c-capability-ipc.md:736:*send* (deferred in v1) will add a sender-side waiter + a staging-buffer lifetime;
docs/adr/0009-m3c-capability-ipc.md:738:revisit whether the waiter fields belong in the thread struct or a side table.
docs/adr/0009-m3c-capability-ipc.md:747:**merged step-4 `try_recv` is already drain-first** (CLOSED is "dead" only on an
docs/adr/0009-m3c-capability-ipc.md:748:empty queue), so A1 makes blocking `recv` behave **identically** to non-blocking
docs/adr/0009-m3c-capability-ipc.md:749:`try_recv` on the same endpoint state — "closing a peer never eats bytes already
docs/adr/0009-m3c-capability-ipc.md:753:message):* blocking `recv` and non-blocking `try_recv` would return *different*
docs/adr/0009-m3c-capability-ipc.md:763:### A6-2. CANCELLED is internal-only in v1 (B1)
docs/adr/0009-m3c-capability-ipc.md:765:**Decision + why.** The `CANCELLED` terminal state is reachable in v1 **only via
docs/adr/0009-m3c-capability-ipc.md:766:an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
docs/adr/0009-m3c-capability-ipc.md:773:**Predictions if other choices were made.** *B2 (defer CANCELLED entirely — state
docs/adr/0009-m3c-capability-ipc.md:789:non-blocking poll** (check once; immediate `TIMED_OUT` if nothing is deliverable —
docs/adr/0009-m3c-capability-ipc.md:790:never parks). A **bounded maximum** `JE_IPC_MAX_WAIT` is enforced: a requested
docs/adr/0009-m3c-capability-ipc.md:791:deadline is rejected (`JE_ABI_INVALID`) if `duration > JE_IPC_MAX_WAIT` or if
docs/adr/0009-m3c-capability-ipc.md:798:*immediate* spurious `TIMED_OUT` (or, worse, never) — a silent correctness bug the
docs/adr/0009-m3c-capability-ipc.md:802:**Flex options to consider in future.** `JE_IPC_MAX_WAIT`'s exact value is a
docs/adr/0009-m3c-capability-ipc.md:808:**Decision + why.** Waiter slots are bounded (`JE_EP_MAX_WAITERS`, one blocked
docs/adr/0009-m3c-capability-ipc.md:819:buffer overrun of the waiter array — the exact class of bug the bound exists to
docs/adr/0009-m3c-capability-ipc.md:823:multi-threaded processes or blocking *send* arrive, `JE_EP_MAX_WAITERS` grows and
docs/adr/0009-m3c-capability-ipc.md:824:exhaustion may become reachable; *then* a real status (or a queue-of-waiters) is
docs/adr/0009-m3c-capability-ipc.md:827:### A6-5. No user copy-out from the scheduler; parked syscall continuation copies
docs/adr/0009-m3c-capability-ipc.md:831:the resumed syscall continuation**, never inside `je_sched_interrupt` after a
docs/adr/0009-m3c-capability-ipc.md:832:`cr3_load`. The scheduler only flips `THREAD_BLOCKED_IPC → READY`. A copy fault in
docs/adr/0009-m3c-capability-ipc.md:833:the continuation returns `JE_ABI_INVALID`, **preserves** the queued message,
docs/adr/0009-m3c-capability-ipc.md:834:releases the head reservation, and re-arbitrates (the buffer re-validation D7
docs/adr/0009-m3c-capability-ipc.md:835:already requires). The continuation does **not** re-resolve the original handle
docs/adr/0009-m3c-capability-ipc.md:844:*Re-running the handle-based recv on wake:* re-resolves a capability the owned-
docs/adr/0009-m3c-capability-ipc.md:849:symmetric copy-*in* on the sender continuation under the same rule.
docs/adr/0009-m3c-capability-ipc.md:857:deadline. The waiter-unlink for the dying process's *own* blocked thread happens in
docs/adr/0009-m3c-capability-ipc.md:864:and a semantic one (it should learn `PEER_CLOSED` now, not `TIMED_OUT` later).
docs/adr/0009-m3c-capability-ipc.md:875:- **queued-before-recv:** message already present → immediate `WOKEN`, no park.
docs/adr/0009-m3c-capability-ipc.md:876:- **WOKEN:** thread X parks on recv; thread Y sends then *explicitly yields* →
docs/adr/0009-m3c-capability-ipc.md:878:- **TIMED_OUT:** X parks; advance `je_timer_ticks` to the inclusive deadline →
docs/adr/0009-m3c-capability-ipc.md:879:  ISR arbitrates `TIMED_OUT`, X resumes with it.
docs/adr/0009-m3c-capability-ipc.md:882:- **CANCELLED:** internal `je_ipc_wait_cancel` → exactly one `CANCELLED`.
docs/adr/0009-m3c-capability-ipc.md:884:  reservation released, a next waiter can still wake.
docs/adr/0009-m3c-capability-ipc.md:885:- **teardown while blocked:** waiter unlink + reference release precede slot reuse.
docs/adr/0007-m3a-userspace-boundary.md:122:Amendment 2026-10-17 (allocation vs reservation): the 73-page budget is the
docs/adr/0007-m3a-userspace-boundary.md:129:A *count-only* reservation was rejected: the M2 allocator is first-fit
docs/adr/0007-m3a-userspace-boundary.md:278:  reservation (a count-only *held reservation* — **superseded by Decision 2 in the
docs/adr/0007-m3a-userspace-boundary.md:297:    not a count-only reservation.** Goal: make admission truthful so #4 cannot
docs/adr/0007-m3a-userspace-boundary.md:300:    review showed a count-only reservation cannot keep the promise — the M2
docs/adr/debates/m3c-step6-waits.md:2:Topic: JeiOS M3c step 6 blocking-recv waits (D7): minimal correct sched.c design + proof plan + what to escalate
docs/adr/debates/m3c-step6-waits.md:9:- A user syscall has no kernel continuation. `je_syscall_dispatch` *returns a frame* (`syscall.c:225-259`). The user thread's whole saved state is its trap frame at `rsp0_top - sizeof(frame)` (`sched.c:174-176`). Resuming it means `iretq` on that frame. Only kernel threads park mid-function (`je_thread_sleep` → `int $0x81`, `sched.c:242`). So a blocked recv can't "return from park and re-check". **Whoever performs the terminal transition must write the result into the blocked thread's saved frame.** That is literally what D7 says ("the ISR performs the transition and delivers the result, not merely mark READY"). Q3 isn't an interpretation question: the architecture forces the ADR reading.
docs/adr/debates/m3c-step6-waits.md:10:- Restarting the syscall (rewind RIP over `int 0x80` and re-execute) is also ruled out. The re-executed recv would re-resolve the handle, and D3 is frozen: "a woken thread uses the waiter's owned ref, never a re-resolve of the original handle."
docs/adr/debates/m3c-step6-waits.md:11:- The copy must happen in the receiver's CR3. A sender completing the receiver's recv is running under the sender's CR3, so it can't do the copy-out.
docs/adr/debates/m3c-step6-waits.md:15:**Q1, a hybrid split.** The thread struct holds `wait_state` (WAITING or a terminal cause), the deadline (reuse `wake`) and `wait_end`. The end record holds a single back-link `waiter` (thread slot or NONE) plus the waiter's `inflight_refs` (D3 alias 3). Neither pure A nor pure B works. The end needs the back-link so a sender or peer-close can find the waiter. The thread needs the state so the ISR can scan it.
docs/adr/debates/m3c-step6-waits.md:18:- **Sender:** enqueue, then CAS the peer's waiter WAITING→WOKEN, then READY.
docs/adr/debates/m3c-step6-waits.md:19:- **Peer close or death:** when the end goes CLOSED, CAS the opposite waiter →PEER_CLOSED, write `rax`, READY.
docs/adr/debates/m3c-step6-waits.md:20:- **ISR:** for each `THREAD_BLOCKED_IPC` thread with `wake<=ticks`, CAS →TIMED_OUT, write `rax=TIMED_OUT, rdx=0` into the frame, READY.
docs/adr/debates/m3c-step6-waits.md:26:- releases the waiter's inflight ref;
docs/adr/debates/m3c-step6-waits.md:27:- for WOKEN, runs the ordinary try_recv commit (copy-out with re-validation and fixup, cap mint), so a copy fault becomes an error result, as D7 requires.
docs/adr/debates/m3c-step6-waits.md:31:**New hazard: theft between the WOKEN stamp and dispatch.** A5 transfer lets both processes hold the same end. The other process can be scheduled first and `try_recv` the message that WOKEN promised. Two fixes:
docs/adr/debates/m3c-step6-waits.md:33:- **(b)** Dequeue into a per-waiter kernel staging buffer. This costs JE_THREAD_MAX × max message size, and putting the message back after a copy fault gets messy.
docs/adr/debates/m3c-step6-waits.md:35:Also: **at most one waiter per end.** A second blocking recv needs a status code (open question below).
docs/adr/debates/m3c-step6-waits.md:38:1. **Receiver death.** The end's back-link points at a thread slot, so tearing down the thread slot alone is NOT enough. `je_vm_destroy`, in its existing IRQ section, must clear `end->waiter` and drop the waiter's inflight ref for every BLOCKED_IPC thread of that vm. Without that, a later send stamps a reused slot.
docs/adr/debates/m3c-step6-waits.md:50:Everything else lives in endpoint.c and syscall.c: the queue check, the immediate-delivery path, the reservation, the copy and the priority choice. Also bump the D4 `_Static_assert`: `JE_EP_REF_MAX` must be ≥ queue depth plus max waiters.
docs/adr/debates/m3c-step6-waits.md:58:| Immediate delivery | Queue a message first, then blocking recv. Assert the thread never entered BLOCKED_IPC (accessor plus dispatch count). |
docs/adr/debates/m3c-step6-waits.md:59:| TIMED_OUT | The user blocks with deadline N and nothing sends. After N ticks, assert `rax=TIMED_OUT` in the resumed thread. |
docs/adr/debates/m3c-step6-waits.md:60:| WOKEN | Bootstrap waits until the receiver is BLOCKED_IPC, does a kernel-side enqueue on the peer end, and the receiver gets the payload. |
docs/adr/debates/m3c-step6-waits.md:64:| Theft | The other holder calls `try_recv` while a WOKEN completion is pending and gets `EMPTY`. |
docs/adr/debates/m3c-step6-waits.md:71:1. **The D7 reading is forced by the frame-return architecture**, so the owner should confirm it rather than choose it. The ISR writes the TIMED_OUT result into the frame; dispatch-time completion handles copy-out, unlink and ref release; there is no syscall re-check. Owner sign-off is needed on the dispatch hook in `select_and_install_next`, which is tested code.
docs/adr/debates/m3c-step6-waits.md:72:2. **CANCELLED has no trigger in v1.** There are no signals, no cancel syscall, and each process has one thread, so a blocked process can't cancel itself. Recommendation: keep the ABI value and the CAS path, drive it only from a kernel-internal `je_ep_wait_cancel` used by a self-test, and document "no user trigger in M3c." The alternative is to defer CANCELLED entirely.
docs/adr/debates/m3c-step6-waits.md:73:3. **New ABI surface:** a blocking-recv op number, the timeout register (R10 is currently reserved=0 on try_recv), a `JE_EP_WAIT_MAX_TICKS` cap, and whether a timeout of 0 means try_recv or INVALID.
docs/adr/debates/m3c-step6-waits.md:74:4. **A status for a second waiter on the same end**, and the choice between reservation and staging for the theft hazard. This is open for an agent majority vote unless the owner wants it.
docs/adr/debates/m3c-step6-waits.md:76:Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
docs/adr/debates/m3c-step6-waits.md:79:Use a thread-owned waiter:
docs/adr/debates/m3c-step6-waits.md:85:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
docs/adr/debates/m3c-step6-waits.md:88:- silently imposing “one waiter per endpoint,” which D7 does not authorize;
docs/adr/debates/m3c-step6-waits.md:90:A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
docs/adr/debates/m3c-step6-waits.md:95:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
docs/adr/debates/m3c-step6-waits.md:98:The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
docs/adr/debates/m3c-step6-waits.md:104:WAITING -> TIMED_OUT
docs/adr/debates/m3c-step6-waits.md:107:The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
docs/adr/debates/m3c-step6-waits.md:109:A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
docs/adr/debates/m3c-step6-waits.md:111:For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
docs/adr/debates/m3c-step6-waits.md:116:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
docs/adr/debates/m3c-step6-waits.md:118:- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
docs/adr/debates/m3c-step6-waits.md:120:- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
docs/adr/debates/m3c-step6-waits.md:122:- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
docs/adr/debates/m3c-step6-waits.md:123:`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
docs/adr/debates/m3c-step6-waits.md:127:- add `THREAD_BLOCKED_IPC`;
docs/adr/debates/m3c-step6-waits.md:128:- add thread-owned waiter fields;
docs/adr/debates/m3c-step6-waits.md:134:Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
docs/adr/debates/m3c-step6-waits.md:138:3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
docs/adr/debates/m3c-step6-waits.md:140:5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
docs/adr/debates/m3c-step6-waits.md:141:The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
docs/adr/debates/m3c-step6-waits.md:142:Claude is right on the core continuation constraint: the blocked user syscall cannot “resume its loop.” The saved trap frame is the continuation, and the result must be installed before `iretq`. Re-resolving the handle would also violate D3’s owned-reference rule.
docs/adr/debates/m3c-step6-waits.md:145:Use a thread-owned waiter:
docs/adr/debates/m3c-step6-waits.md:151:Endpoint send/close paths call a bounded scheduler helper that scans at most `JE_THREAD_MAX` threads for `THREAD_BLOCKED_IPC && wait_end == end`. With `JE_THREAD_MAX=8`, this is deterministic and avoids:
docs/adr/debates/m3c-step6-waits.md:154:- silently imposing “one waiter per endpoint,” which D7 does not authorize;
docs/adr/debates/m3c-step6-waits.md:156:A back-link is only justified if profiling or a frozen endpoint contract requires it. If multiple waiters are supported, wake one eligible waiter per message, deterministically.
docs/adr/debates/m3c-step6-waits.md:161:3. Otherwise retain the endpoint reference and receive metadata in the thread, set `THREAD_BLOCKED_IPC`, and return to the scheduler without returning to user mode.
docs/adr/debates/m3c-step6-waits.md:164:The hook must use something like `je_ep_recv_commit_owned(waiter)`, not “ordinary `try_recv`” unless that function is proven not to re-resolve the handle. D3 explicitly forbids re-resolving the original handle.
docs/adr/debates/m3c-step6-waits.md:170:WAITING -> TIMED_OUT
docs/adr/debates/m3c-step6-waits.md:173:The helper must also respect eligibility priority. A blind timer `WAITING -> TIMED_OUT` is unsafe if the endpoint is already peer-closed or a queued message is eligible. Under IF=0, sender and timer cannot execute simultaneously, but the implementation still needs an explicit linearization rule.
docs/adr/debates/m3c-step6-waits.md:175:A WOKEN completion also needs reservation. Marking WOKEN without reserving a specific queued message permits another receiver to steal it before the blocked receiver runs. Reservation must be established in the same IF=0 critical section as the wake decision and released on receiver death, endpoint close, and copy failure.
docs/adr/debates/m3c-step6-waits.md:177:For a dying receiver, clearing the thread slot is insufficient unless all waiter metadata is cleared and the owned endpoint reference is released before frame/slot reuse. With thread-owned waiters, teardown scans the bounded thread table and tombstones each waiter.
docs/adr/debates/m3c-step6-waits.md:182:- Immediate delivery: queue first; assert no `THREAD_BLOCKED_IPC`.
docs/adr/debates/m3c-step6-waits.md:184:- TIMED_OUT: receiver blocks; advance exactly to deadline; inspect that the timer path changed the saved frame before making the thread runnable.
docs/adr/debates/m3c-step6-waits.md:186:- Receiver death: destroy the blocked receiver; verify waiter metadata/ref cleanup and that a later send cannot wake a reused slot.
docs/adr/debates/m3c-step6-waits.md:188:- Priority: unit-test the arbitration function over all eligibility combinations, especially `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`.
docs/adr/debates/m3c-step6-waits.md:189:`CANCELLED` has no visible M3c trigger in the supplied context. Implementing and unit-testing an internal cancellation helper is reasonable, but it must be documented as unreachable from the v1 user ABI. Do not claim end-to-end cancellation coverage without a real trigger.
docs/adr/debates/m3c-step6-waits.md:193:- add `THREAD_BLOCKED_IPC`;
docs/adr/debates/m3c-step6-waits.md:194:- add thread-owned waiter fields;
docs/adr/debates/m3c-step6-waits.md:200:Endpoint queue operations, reservation, peer-close detection, and copy-out remain in endpoint/syscall code. Scheduler code should not re-resolve handles or perform endpoint policy.
docs/adr/debates/m3c-step6-waits.md:204:3. Whether multiple blocked receivers per endpoint are supported, or whether an explicit one-waiter error is approved.
docs/adr/debates/m3c-step6-waits.md:206:5. Whether `CANCELLED` gets only an internal/test hook in M3c or must have a production trigger.
docs/adr/debates/m3c-step6-waits.md:207:The strongest correction to Claude’s design is: no syscall re-check is correct, but “eager CAS plus deferred ordinary `try_recv`” is insufficient. The design needs an owned-reference completion path, message reservation, explicit arbitration priority, and teardown-safe waiter ownership.
docs/adr/debates/m3c-step6-waits.md:211:- **Thread-owned waiter plus a bounded scan of `JE_THREAD_MAX`, no endpoint back-link.** ADR 0009 D3 item 3 says a waiter holds an `inflight_refs` on the end and a reference to the blocked thread, which must be unlinked before the thread's storage is reused. If the waiter lives in the thread slot, that thread reference points to itself and can never dangle. Teardown then only has to clear the slot's waiter and release its inflight ref.
docs/adr/debates/m3c-step6-waits.md:212:- **The resume path needs an owned-ref commit, not plain `try_recv`.** This is verified: `je_endpoint_recv_peek` calls `je_endpoint_resolve(pool, t, h, …)` (endpoint.c ~496), which re-resolves the handle. D3 forbids that for a woken thread. We need a variant keyed on `(rec, side)` that reuses the existing `je_ep_recv_lease` two-phase commit.
docs/adr/debates/m3c-step6-waits.md:214:- **CANCELLED has no v1 trigger.** `je_thread_create_user(vm, index)` and the slot lookups (sched.c:295/353) give one user thread per process. The only thread that could close the waited handle is the blocked thread itself. The receiver's own death tombstones the waiter and delivers no result, so that is not CANCELLED either. Ship an internal helper with a unit test and document it as unreachable from the ABI.
docs/adr/debates/m3c-step6-waits.md:217:1. **The timer ISR should not judge eligibility itself.** Make it a structural invariant instead: under IF=0, every enqueue and every peer-close transition calls the wake helper in the same critical section. So a WAITING waiter can never coexist with a deliverable message or a peer-closed state. The timer then does a plain `CAS WAITING→TIMED_OUT` and writes the result into the saved frame, with an assert-or-`JE_EP_CORRUPT` check on the invariant. If the ISR evaluated eligibility, it would have to reach into the endpoint pool, which is exactly the coupling we are keeping out of tested `sched.c`. TIMED_OUT is a plain status code, so the ISR fully delivers it. D7's "ISR performs the transition and delivers the result" is met literally for TIMED_OUT.
docs/adr/debates/m3c-step6-waits.md:218:2. **"WOKEN, then the peer dies" has one safe reading, fixed by existing code.** Merged `try_recv` drains queued messages before reporting PEER_CLOSED (endpoint.c:517-520: PEER_CLOSED only when `count == 0`). Read literally, PEER_CLOSED > WOKEN would discard a queued message, and the merged step-4 semantics don't allow that. So the priority applies at a single arbitration point, and PEER_CLOSED counts as eligible only when the inbound queue is empty. Record this as a clarification for the owner to confirm, not as an open design question.
docs/adr/debates/m3c-step6-waits.md:220:4. **Codex's multi-waiter escalation #3 is partly answered already.** `JE_EP_MAX_WAITERS = 2` exists (endpoint.h:56), and endpoint.c:13 asserts `REF_MAX >= QUEUE_DEPTH + MAX_WAITERS`, so two waiters per end is the configured design. A third waiter fails at its ref acquire. What needs a decision is the status a waiter beyond the limit gets.
docs/adr/debates/m3c-step6-waits.md:223:- **Reservation interacts with copy faults.** The frozen fault rule leaves the message queued. If waiter A is WOKEN with message 0 reserved and its copy-out faults, A's terminal result is WOKEN with status `JE_ABI_INVALID`. Message 0 is then unreserved but waiter B is still WAITING next to a deliverable message, which breaks the invariant above. The fault-release path must re-run the wake helper for that end.
docs/adr/debates/m3c-step6-waits.md:224:- **Reserving more than one message breaks the queue.** Out-of-order completion would dequeue from the middle of the ring. Rule: an end has at most one outstanding reservation, always on the head message, and the next waiter is woken only after the current one commits or releases. That keeps FIFO order without restructuring the queue.
docs/adr/debates/m3c-step6-waits.md:231:3. CANCELLED in v1: internal helper with a unit test only, documented as unreachable from the ABI.
docs/adr/debates/m3c-step6-waits.md:232:4. The remaining ABI details: deadline register, behaviour of a zero timeout, the maximum deadline, and the status for exceeding `JE_EP_MAX_WAITERS`.
docs/adr/debates/m3c-step6-waits.md:236:- Thread-owned waiter metadata minimizes `sched.c` churn, but it does not eliminate endpoint→scheduler wake coupling. Endpoint enqueue/close paths still need a bounded waiter scan or scheduler wake API.
docs/adr/debates/m3c-step6-waits.md:238:- One outstanding head reservation per endpoint is required. Otherwise FIFO dequeue and copy-fault rollback become ambiguous.
docs/adr/debates/m3c-step6-waits.md:240:- `CANCELLED` has no current ABI trigger. An internal cancellation helper plus a deterministic kernel test is reasonable, but its ABI unreachability must be documented.
docs/adr/debates/m3c-step6-waits.md:243:A timer path that blindly performs `WAITING → TIMED_OUT` is correct only if every path that creates eligibility synchronously performs the competing transition first:
docs/adr/debates/m3c-step6-waits.md:244:- enqueue: `WAITING → WOKEN`;
docs/adr/debates/m3c-step6-waits.md:245:- peer close: `WAITING → PEER_CLOSED`;
docs/adr/debates/m3c-step6-waits.md:246:- cancel: `WAITING → CANCELLED`;
docs/adr/debates/m3c-step6-waits.md:247:- reservation release after copy fault: re-arbitrate and wake another waiter.
docs/adr/debates/m3c-step6-waits.md:248:The copy-fault case Claude identified proves this invariant is easy to violate. “Assert or `JE_EP_CORRUPT`” is not an acceptable normal recovery path for a kernel mechanism. The design must specify one shared `je_ipc_wait_arbitrate()` helper, called under IF=0, and test that helper after reservation release.
docs/adr/debates/m3c-step6-waits.md:249:The timer may then perform the final timeout CAS, but only after the same arbitration rules are applied or after the invariant is mechanically enforced. Otherwise the ISR can timeout a waiter while a queued message exists.
docs/adr/debates/m3c-step6-waits.md:251:The terminal state must be selected by one CAS. A syscall re-check may validate the already-selected result and perform copy/commit, but it must not be a second authority that changes `TIMED_OUT` into `WOKEN`. If the syscall is allowed to override the ISR result, the transition is no longer a single terminal CAS.
docs/adr/debates/m3c-step6-waits.md:252:Conversely, sender and close paths cannot merely enqueue or mark metadata. They must perform the `WAITING → WOKEN/PEER_CLOSED` transition in the same IF=0 critical section as the endpoint mutation.
docs/adr/debates/m3c-step6-waits.md:256:- wake path atomically selects the waiter result and, for WOKEN, reserves the head message;
docs/adr/debates/m3c-step6-waits.md:257:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
docs/adr/debates/m3c-step6-waits.md:258:- the blocked syscall continuation resumes after park;
docs/adr/debates/m3c-step6-waits.md:260:- copy faults return `JE_ABI_INVALID`, preserve the message, release the reservation, and re-arbitrate.
docs/adr/debates/m3c-step6-waits.md:263:“PEER_CLOSED applies only when the queue is empty” is consistent with merged drain-first `try_recv` behavior, but it is not implied by the literal D7 priority `PEER_CLOSED > WOKEN`. If peer closure and a queued message coexist, two interpretations conflict:
docs/adr/debates/m3c-step6-waits.md:268:- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
docs/adr/debates/m3c-step6-waits.md:269:- Process teardown must clear the waiter state, unlink it from endpoint arbitration, and release its endpoint reference before the thread/frame slot can be reused. Merely changing the thread to `DYING` is insufficient if endpoint code can later scan or wake the stale slot.
docs/adr/debates/m3c-step6-waits.md:270:- A message enqueue must wake at most one waiter and reserve only the queue head. Peer closure with an empty queue may wake all eligible receivers, subject to the same CAS rule.
docs/adr/debates/m3c-step6-waits.md:271:- `JE_EP_MAX_WAITERS == 2` and the ref-count assertion establish capacity, not the required ABI/error status when capacity is exhausted.
docs/adr/debates/m3c-step6-waits.md:273:- Add `THREAD_BLOCKED_IPC`.
docs/adr/debates/m3c-step6-waits.md:274:- Add per-thread waiter state, endpoint record/side identity, owned-reference marker, deadline, saved receive arguments, reservation marker, and terminal cause.
docs/adr/debates/m3c-step6-waits.md:278:  - unlink/tombstone waiter during VM destruction.
docs/adr/debates/m3c-step6-waits.md:279:- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
docs/adr/debates/m3c-step6-waits.md:284:- receiver parks, timer reaches inclusive deadline: ISR produces TIMED_OUT;
docs/adr/debates/m3c-step6-waits.md:286:- copy-out fault after WOKEN: `JE_ABI_INVALID`, message remains queued, reservation released, next waiter can wake;
docs/adr/debates/m3c-step6-waits.md:287:- internal cancellation helper: CANCELLED exactly once;
docs/adr/debates/m3c-step6-waits.md:288:- teardown while blocked: waiter unlink/reference release precedes slot reuse;
docs/adr/debates/m3c-step6-waits.md:292:2. Confirm that syscall-continuation copy-out is required, rejecting scheduler-dispatch copy-out unless exact-RIP recovery there is explicitly owner-approved and tested.
docs/adr/debates/m3c-step6-waits.md:293:3. Set deadline-register encoding, zero-timeout behavior, maximum deadline, and waiter-capacity failure status.
docs/adr/debates/m3c-step6-waits.md:294:4. Confirm whether CANCELLED is internal-test-only in v1 or exposed through a future cancellation contract.
docs/adr/debates/m3c-step6-waits.md:297:- Thread-owned waiter metadata minimizes `sched.c` churn, but it does not eliminate endpoint→scheduler wake coupling. Endpoint enqueue/close paths still need a bounded waiter scan or scheduler wake API.
docs/adr/debates/m3c-step6-waits.md:299:- One outstanding head reservation per endpoint is required. Otherwise FIFO dequeue and copy-fault rollback become ambiguous.
docs/adr/debates/m3c-step6-waits.md:301:- `CANCELLED` has no current ABI trigger. An internal cancellation helper plus a deterministic kernel test is reasonable, but its ABI unreachability must be documented.
docs/adr/debates/m3c-step6-waits.md:304:A timer path that blindly performs `WAITING → TIMED_OUT` is correct only if every path that creates eligibility synchronously performs the competing transition first:
docs/adr/debates/m3c-step6-waits.md:305:- enqueue: `WAITING → WOKEN`;
docs/adr/debates/m3c-step6-waits.md:306:- peer close: `WAITING → PEER_CLOSED`;
docs/adr/debates/m3c-step6-waits.md:307:- cancel: `WAITING → CANCELLED`;
docs/adr/debates/m3c-step6-waits.md:308:- reservation release after copy fault: re-arbitrate and wake another waiter.
docs/adr/debates/m3c-step6-waits.md:309:The copy-fault case Claude identified proves this invariant is easy to violate. “Assert or `JE_EP_CORRUPT`” is not an acceptable normal recovery path for a kernel mechanism. The design must specify one shared `je_ipc_wait_arbitrate()` helper, called under IF=0, and test that helper after reservation release.
docs/adr/debates/m3c-step6-waits.md:310:The timer may then perform the final timeout CAS, but only after the same arbitration rules are applied or after the invariant is mechanically enforced. Otherwise the ISR can timeout a waiter while a queued message exists.
docs/adr/debates/m3c-step6-waits.md:312:The terminal state must be selected by one CAS. A syscall re-check may validate the already-selected result and perform copy/commit, but it must not be a second authority that changes `TIMED_OUT` into `WOKEN`. If the syscall is allowed to override the ISR result, the transition is no longer a single terminal CAS.
docs/adr/debates/m3c-step6-waits.md:313:Conversely, sender and close paths cannot merely enqueue or mark metadata. They must perform the `WAITING → WOKEN/PEER_CLOSED` transition in the same IF=0 critical section as the endpoint mutation.
docs/adr/debates/m3c-step6-waits.md:317:- wake path atomically selects the waiter result and, for WOKEN, reserves the head message;
docs/adr/debates/m3c-step6-waits.md:318:- scheduler only changes `THREAD_BLOCKED_IPC → READY`;
docs/adr/debates/m3c-step6-waits.md:319:- the blocked syscall continuation resumes after park;
docs/adr/debates/m3c-step6-waits.md:321:- copy faults return `JE_ABI_INVALID`, preserve the message, release the reservation, and re-arbitrate.
docs/adr/debates/m3c-step6-waits.md:324:“PEER_CLOSED applies only when the queue is empty” is consistent with merged drain-first `try_recv` behavior, but it is not implied by the literal D7 priority `PEER_CLOSED > WOKEN`. If peer closure and a queued message coexist, two interpretations conflict:
docs/adr/debates/m3c-step6-waits.md:329:- Keep waiter state separate from scheduler state. `WAITING/WOKEN/...` is the waiter state machine; `THREAD_BLOCKED_IPC/READY` is scheduling state. Do not encode both in one enum.
docs/adr/debates/m3c-step6-waits.md:330:- Process teardown must clear the waiter state, unlink it from endpoint arbitration, and release its endpoint reference before the thread/frame slot can be reused. Merely changing the thread to `DYING` is insufficient if endpoint code can later scan or wake the stale slot.
docs/adr/debates/m3c-step6-waits.md:331:- A message enqueue must wake at most one waiter and reserve only the queue head. Peer closure with an empty queue may wake all eligible receivers, subject to the same CAS rule.
docs/adr/debates/m3c-step6-waits.md:332:- `JE_EP_MAX_WAITERS == 2` and the ref-count assertion establish capacity, not the required ABI/error status when capacity is exhausted.
docs/adr/debates/m3c-step6-waits.md:334:- Add `THREAD_BLOCKED_IPC`.
docs/adr/debates/m3c-step6-waits.md:335:- Add per-thread waiter state, endpoint record/side identity, owned-reference marker, deadline, saved receive arguments, reservation marker, and terminal cause.
docs/adr/debates/m3c-step6-waits.md:339:  - unlink/tombstone waiter during VM destruction.
docs/adr/debates/m3c-step6-waits.md:340:- Change timer scanning to process `THREAD_BLOCKED_IPC` deadlines through the timeout transition helper.
docs/adr/debates/m3c-step6-waits.md:345:- receiver parks, timer reaches inclusive deadline: ISR produces TIMED_OUT;
docs/adr/debates/m3c-step6-waits.md:347:- copy-out fault after WOKEN: `JE_ABI_INVALID`, message remains queued, reservation released, next waiter can wake;
docs/adr/debates/m3c-step6-waits.md:348:- internal cancellation helper: CANCELLED exactly once;
docs/adr/debates/m3c-step6-waits.md:349:- teardown while blocked: waiter unlink/reference release precedes slot reuse;
docs/adr/debates/m3c-step6-waits.md:353:2. Confirm that syscall-continuation copy-out is required, rejecting scheduler-dispatch copy-out unless exact-RIP recovery there is explicitly owner-approved and tested.
docs/adr/debates/m3c-step6-waits.md:354:3. Set deadline-register encoding, zero-timeout behavior, maximum deadline, and waiter-capacity failure status.
docs/adr/debates/m3c-step6-waits.md:355:4. Confirm whether CANCELLED is internal-test-only in v1 or exposed through a future cancellation contract.
docs/adr/debates/README.md:15:| `m3c-step6-waits.md` | ADR 0009 A6 (blocking recv waits, D7) |
docs/adr/0006-bsp-foundation-and-page-heap.md:22:Seven CPU-bound workers test real preemption, complete GPR preservation, sleeping,
docs/ACCELERATORS.md:64:M3a remains unchanged. M3b/M3c should leave room for future typed DEVICE, BUFFER and FENCE objects without implementing them prematurely. M4 supplies device lifecycle, scoped buffer grants and DMA/reset containment. M5/M6 provide storage, application runtime and initial software media ports; their CPU instruction prerequisites also need qualified state preservation. M7 supplies desktop media integration. M8 qualifies GPU compute/video, NPU backends and automatic workload placement. Software media need not wait for accelerated backends, but must not execute SIMD before its context support exists.
docs/HANDOFF.md:49:  `try_send` (op 11) / `try_recv` (op 12) move bounded, copied, **data-only**
docs/HANDOFF.md:52:  stack staging → enqueue; recv: peek → copy_to_user → infallible commit, so a
docs/HANDOFF.md:63:  extension, not overload `try_recv`** (R8/R9 and RDX[63:32] reserved now).
docs/HANDOFF.md:66:  `try_recv` mints fresh attenuated handles in the receiver's table (D8). D9 =
docs/HANDOFF.md:671:The current baseline above supersedes historical continuation instructions.
docs/HANDOFF.md:703:| M1b boot context | Bounded Multiboot2 memory/module parser, reserved ranges, no retained loader pointers | Rejects overlapping maps; 128 records, 40 reservations |
docs/HANDOFF.md:705:| M2 physical pages | First-fit contiguous extents, exact free validation and reservation exclusion | First GiB only; <=256-page allocation; caller serialization |
docs/STORAGE.md:163:cannot safely acquire persistent files with zero metadata and no space reservation.
docs/M3A.md:122:Receive transaction: reserve the head message, validate output spans and destination handle capacity, reserve a local slot if needed, copy payload/result, then atomically publish the new handle and consume the message. Failed output or capacity checks restore the reservation and leave the message queued. No raw sender token becomes a receiver token. Define short-buffer refusal with required length and no dequeue. No user scheduling inside this bounded receive transaction in the initial single-thread/process model.
docs/DESIGN.md:747:## 21. Repository map and continuation rules
include/jeios/abi.h:47:    JE_ABI_H_SHORT_BUFFER = 11, /* M3c D6: try_recv with a buffer smaller than the
include/jeios/abi.h:49:    JE_ABI_H_PEER_CLOSED = 12, /* M3c D6: try_send to a peer-closed end, or try_recv
include/jeios/abi.h:52:    JE_ABI_H_EMPTY       = 13  /* M3c D6: non-blocking try_recv on an OPEN end with
include/jeios/abi.h:117:    , JE_SYS_TRY_RECV = 12   /* M3c D6: try_recv(h=RDI, ubuf=RSI, cap=RDX,
include/jeios/abi.h:153:_Static_assert(JE_SYS_TRY_RECV == 12, "abi op try_recv");
include/jeios/abi.h:179:#define JE_ABI_RIGHT_RECV     0x4u             /* endpoint: may try_recv */
include/jeios/boot.h:14:typedef struct { uint64_t begin, end; } je_reservation;
include/jeios/boot.h:20:    je_reservation reserved[JE_BOOT_RESERVED_MAX];
include/jeios/endpoint_kern.h:46:je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
include/jeios/endpoint_kern.h:48:                                        size_t *out_len, je_ep_recv_lease *lease);
include/jeios/endpoint_kern.h:49:je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease);
include/jeios/endpoint_kern.h:63:je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
include/jeios/endpoint_kern.h:68:                                             je_ep_recv_lease *lease);
include/jeios/endpoint_kern.h:71:je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
include/jeios/endpoint.h:23: *     tested directly, but nothing in this PR queues a cap or registers a waiter.
include/jeios/endpoint.h:56:#define JE_EP_MAX_WAITERS   2u    /* waiters per end (unused this PR) */
include/jeios/endpoint.h:62: * bound (K); 2 so the recv/send rollback is exercised (a 2nd mint can hit QUOTA).
include/jeios/endpoint.h:67: * (queued caps + waiters) without spuriously overflowing a legitimate acquire,
include/jeios/endpoint.h:69: * inflight_refs by queue depth + waiters. One shared max covers both. */
include/jeios/endpoint.h:103:    JE_EP_SHORT_BUFFER, /* try_recv: caller buffer < head message; not dequeued */
include/jeios/endpoint.h:104:    JE_EP_PEER_CLOSED,  /* try_send to a peer-closed end, or try_recv drained+EOF */
include/jeios/endpoint.h:105:    JE_EP_EMPTY,        /* try_recv: OPEN end, empty queue (would block) */
include/jeios/endpoint.h:145: * THIS end's try_recv (Model A). Waiter storage is reserved for step 6. */
include/jeios/endpoint.h:149:    uint16_t        inflight_refs;  /* queued caps + waiters (unused this PR) */
include/jeios/endpoint.h:151:    je_ep_queue     inbound;        /* messages awaiting this end's try_recv */
include/jeios/endpoint.h:271:/* A recv lease token (D6 / endpoint.h generation-carry rule): identifies the
include/jeios/endpoint.h:279:} je_ep_recv_lease;
include/jeios/endpoint.h:281:/* try_recv PEEK: resolve the handle (needs RECV), apply the state/empty rules,
include/jeios/endpoint.h:289:je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
include/jeios/endpoint.h:291:                                   size_t *out_len, je_ep_recv_lease *lease);
include/jeios/endpoint.h:293:/* try_recv COMMIT: dequeue the message the lease identifies. Called by the glue
include/jeios/endpoint.h:298:je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
include/jeios/endpoint.h:299:                                     const je_ep_recv_lease *lease);
include/jeios/endpoint.h:302: * These extend send/recv to carry capabilities. The data-only forms above are
include/jeios/endpoint.h:304: * descriptors (send) and mints handles from descriptors (recv); the pure layer
include/jeios/endpoint.h:328:/* recv PEEK with caps: as je_endpoint_recv_peek, plus it reports the head
include/jeios/endpoint.h:333:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
include/jeios/endpoint.h:337:                                        je_ep_recv_lease *lease);
include/jeios/endpoint.h:358:/* recv COMMIT with caps: dequeue the leased head message AND release the inflight
include/jeios/endpoint.h:362:je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
include/jeios/endpoint.h:363:                                          const je_ep_recv_lease *lease);
kernel/arch/x86_64/user_demo.S:485: * h1's inbound; recv on h1 reads it. Proves ring-3 copy-in / queue / direction /
kernel/arch/x86_64/user_demo.S:488: * Scratch: src payload @0x1300 (8 bytes), recv dst @0x1340, results @0x1380+. */
kernel/arch/x86_64/user_demo.S:517:    /* recv(h0) -> EMPTY (direction: nothing landed in h0's inbound) */
kernel/arch/x86_64/user_demo.S:525:    /* recv(h1, cap=1) -> SHORT_BUFFER, RDX=8 */
kernel/arch/x86_64/user_demo.S:534:    /* recv(h1, unmapped dst 0x40003000) -> INVALID; message stays queued */
kernel/arch/x86_64/user_demo.S:542:    /* recv(h1, good dst) -> OK, 8 bytes, payload matches */
kernel/arch/x86_64/user_demo.S:553:    /* recv(h1) again -> EMPTY (delivered exactly once) */
kernel/arch/x86_64/user_demo.S:569:    /* recv(h1) -> EMPTY (bad send enqueued nothing) */
kernel/arch/x86_64/user_demo.S:622:    movq %rax, 0x40001400      /* 4th drained recv: expect OK (drain despite peer gone) */
kernel/arch/x86_64/register_probe.S:2:/* GPR preservation stress: busy until PIT tick 64, no cooperative yield. */
kernel/core/endpoint.c:11: * ref (queue depth + waiters) and still be small enough to reach in a test, and
kernel/core/endpoint.c:13:_Static_assert(JE_EP_REF_MAX >= JE_EP_QUEUE_DEPTH + JE_EP_MAX_WAITERS,
kernel/core/endpoint.c:14:               "JE_EP_REF_MAX must cover queue depth + waiters");
kernel/core/endpoint.c:29: * reach a reader (defense in depth; recv copies only len bytes regardless). */
kernel/core/endpoint.c:500:je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:502:                                   size_t *out_len, je_ep_recv_lease *lease)
kernel/core/endpoint.c:538:je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
kernel/core/endpoint.c:539:                                     const je_ep_recv_lease *lease)
kernel/core/endpoint.c:544:    return je_endpoint_recv_commit_caps(pool, lease);
kernel/core/endpoint.c:673:je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
kernel/core/endpoint.c:677:                                        je_ep_recv_lease *lease)
kernel/core/endpoint.c:714:je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
kernel/core/endpoint.c:715:                                          const je_ep_recv_lease *lease)
kernel/core/syscall.c:111:        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8, hcap=R9, R10 reserved=0).
kernel/core/syscall.c:402:        /* A5-4 recv: PEEK (bytes + cap descriptors, no dequeue) -> stage all live
kernel/core/syscall.c:414:        je_ep_recv_lease lease;
kernel/core/syscall.c:415:        je_ep_status s = je_endpoint_kern_recv_peek_caps(vm, index, h, staging,
kernel/core/syscall.c:435:            je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
kernel/core/syscall.c:466:        je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
kernel/core/syscall.c:468:            je_arch_console_write("JeiOS fatal: recv commit mismatch\r\n");
kernel/core/endpoint_kern.c:97:je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
kernel/core/endpoint_kern.c:99:                                        size_t *out_len, je_ep_recv_lease *lease)
kernel/core/endpoint_kern.c:103:    return je_endpoint_recv_peek(&g_kep, &vm->procs[index].htable, h,
kernel/core/endpoint_kern.c:107:je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease)
kernel/core/endpoint_kern.c:110:    return je_endpoint_recv_commit_caps(&g_kep, lease);
kernel/core/endpoint_kern.c:162:je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
kernel/core/endpoint_kern.c:167:                                             je_ep_recv_lease *lease)
kernel/core/endpoint_kern.c:171:    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
kernel/core/endpoint_kern.c:176:je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
kernel/core/main.c:225:    je_arch_console_write("M1: boot map validated; reservations ready\r\n");
kernel/core/main.c:757:        /* --- M3c s4: non-blocking data IPC (try_send/try_recv), ring-3
kernel/core/main.c:777:            if (vm_s3_scratch(P,0x1390)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 recv(h0) not EMPTY (direction)\r\n");
kernel/core/main.c:780:            if (vm_s3_scratch(P,0x13a8)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 bad-dst recv not INVALID\r\n");
kernel/core/main.c:781:            if (vm_s3_scratch(P,0x13b0)!=JE_ABI_OK)          fail("JeiOS fatal: s4 recv not OK\r\n");
kernel/core/main.c:782:            if (vm_s3_scratch(P,0x13b8)!=8u)                 fail("JeiOS fatal: s4 recv wrong len\r\n");
kernel/core/main.c:792:            if (vm_s3_scratch(P,0x1408)!=JE_ABI_H_PEER_CLOSED) fail("JeiOS fatal: s4 drained recv not PEER_CLOSED\r\n");
kernel/core/main.c:798:            je_arch_console_write("M3c-s4: ring-3 try_send/try_recv loopback OK (direction, SHORT_BUFFER, copy-out failure leaves msg queued -> delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED); channel reclaimed on exit\r\n");
kernel/core/main.c:817:         * into XB's OWN table via the real send/recv cap path — asserting the
kernel/core/main.c:846:            je_ep_recv_lease lz;
kernel/core/main.c:847:            if (je_endpoint_kern_recv_peek_caps(&vm_s3, XB, sB, (void*)0, 0, &rl,
kernel/core/main.c:853:            if (je_endpoint_kern_mint_recv_cap(&vm_s3, XB, &rc[0], &mintedB)!=JE_EP_OK || mintedB==0u)
kernel/core/main.c:855:            if (je_endpoint_kern_recv_commit(&lz)!=JE_EP_OK) fail("JeiOS fatal: s5 B commit\r\n");
kernel/core/main.c:867:                uint8_t rb[JE_EP_MSG_MAX]; size_t bl=0; je_ep_recv_lease lz2;
kernel/core/main.c:868:                if (je_endpoint_kern_recv_peek(&vm_s3, XA, c0, rb, sizeof rb, &bl, &lz2)!=JE_EP_OK)
kernel/core/main.c:869:                    fail("JeiOS fatal: s5 A recv on c0\r\n");
kernel/core/main.c:871:                if (je_endpoint_kern_recv_commit(&lz2)!=JE_EP_OK) fail("JeiOS fatal: s5 A commit\r\n");
kernel/core/main.c:879:            je_arch_console_write("M3c-s5: cross-table capability transfer OK (XA mints C, transfers C.e1 to XB via the kernel send/recv cap FORWARDERS over seeded S; XB mints it in its OWN table and messages XA on c0 across tables); S+C reclaimed. [syscall-decode/copy layer exercised by test_syscall decode + hosted tests; ring-3 two-process transfer deferred to step 7]\r\n");
kernel/core/boot.c:13:    c->reserved[c->reserved_count++] = (je_reservation){start,end};
tests/test_boot.c:64:     * On accepted inputs, retained state is internally bounded and reservation
tests/test_vm.c:45:    boot.reserved[0] = (je_reservation){
tests/test_endpoint.c:306:    /* 13. Data IPC: send/recv roundtrip + FIFO, via the pure peek/commit seam.
tests/test_endpoint.c:307:     * Helper to do a full recv (peek into a local buf, then commit). */
tests/test_endpoint.c:311:        /* send on h0 -> lands in end1 inbound (peer). recv on h1 reads it. */
tests/test_endpoint.c:317:        /* recv on h0 is EMPTY (direction: nothing landed in end0 inbound). */
tests/test_endpoint.c:318:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:319:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h0,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
tests/test_endpoint.c:320:        /* recv on h1 delivers the message. */
tests/test_endpoint.c:321:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:324:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:327:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
tests/test_endpoint.c:349:            uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:350:            assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:352:            assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:362:        uint8_t rb[2]={0xAA,0xBB}; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:363:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,2,&rl,&lz)==JE_EP_SHORT_BUFFER);
tests/test_endpoint.c:366:        /* still queued: a big-enough recv delivers it. */
tests/test_endpoint.c:368:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb2,sizeof rb2,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:370:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:373:    /* 16. PEER_CLOSED: send to a peer-closed end; drain-then-EOF on recv. */
tests/test_endpoint.c:382:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:384:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:386:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:388:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_PEER_CLOSED);
tests/test_endpoint.c:393:    /* 17. Rights: send needs SEND, recv needs RECV (DENIED otherwise). */
tests/test_endpoint.c:405:        /* r1 can recv; s0 cannot recv (no RECV) -> DENIED. */
tests/test_endpoint.c:406:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:407:        assert(je_endpoint_recv_peek(&g_pool,&g_t,s0,rb,sizeof rb,&rl,&lz)==JE_EP_DENIED);
tests/test_endpoint.c:408:        assert(je_endpoint_recv_peek(&g_pool,&g_t,r1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:416:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=7; je_ep_recv_lease lz;
tests/test_endpoint.c:417:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:419:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:443:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:444:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:449:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_CORRUPT);  /* fail closed */
tests/test_endpoint.c:458:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
tests/test_endpoint.c:466:            assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:468:            assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:476:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:478:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:479:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:481:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:482:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
tests/test_endpoint.c:484:        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:485:        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
tests/test_endpoint.c:508:        /* recv on s1: peek reports 1 cap, mint it as a fresh handle. */
tests/test_endpoint.c:509:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
tests/test_endpoint.c:510:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
tests/test_endpoint.c:523:        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:531:    /* 23. Dead-cap (A5-2): send a cap, then CLOSE the target before recv; peek
tests/test_endpoint.c:546:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
tests/test_endpoint.c:547:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
tests/test_endpoint.c:550:        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);  /* releases inflight */
tests/test_endpoint.c:601:        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rcv[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
tests/test_endpoint.c:603:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,1,rcv,&nc,&lz)==JE_EP_SHORT_BUFFER);
tests/test_endpoint.c:606:        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rcv,&nc,&lz)==JE_EP_OK);
tests/test_endpoint.c:608:        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
tests/test_endpoint.c:635:           "send/recv/FIFO/QUEUE_FULL/SHORT_BUFFER/PEER_CLOSED/rights/zero-len, "
tests/test_syscall.c:117:    /* try_recv (op 12): h,ubuf,cap,hbuf=R8,hcap=R9; R8!=0 iff R9!=0; R10 reserved. */
tests/test_syscall.c:128:           "endpoint_create, try_send/try_recv decode, "
tests/test_alloc.c:13:    b.reserved[0]=(je_reservation){0x100000,0x103000};return b;
tests/test_alloc.c:64:    puts("allocator: PASS (reservations, exact frees, exhaustion, fragmentation, zeroing, rollback)");

codex
Review basis: `main` at `e914a95`; no files changed.

1. **A6-STRUCT/A6-5 contradict the current frame-return architecture.**

   > “The parked syscall continuation then validates... and performs the owned-record copy-out + commit”

   > “The scheduler only flips `THREAD_BLOCKED_IPC → READY`.”

   A user syscall has no resumable kernel continuation today. If the scheduler only marks the thread ready, the resumed `iretq` returns directly to user mode and no kernel code performs copy-out. If the continuation runs after `cr3_load` during dispatch, that contradicts “never inside `je_sched_interrupt`.” The amendment must define the exact resume mechanism and where `RAX/RDX` are written.

2. **“Re-arbitrates” conflicts with the single-terminal-CAS rule.**

   > “A copy fault... releases the head reservation, and re-arbitrates”

   The waiter is already `WOKEN`; it cannot be re-armed without a second terminal transition. If “re-arbitrate” means wake another waiter, the text must say so, including release of the first waiter’s waiter-reference and terminal bookkeeping. It must also distinguish the wait cause (`WOKEN`) from the eventual syscall status (`JE_ABI_INVALID`).

3. **A6-1 does not constrain the close wake path to the empty-queue case.**

   > “`PEER_CLOSED` is returned only when the queue is empty”

   > “peer close → `PEER_CLOSED`”

   > “calls the arbitrate CAS on any receiver blocked on that end”

   The close path could stamp `PEER_CLOSED` even when a message is queued or reserved, contradicting drain-first. Required ordering is unstated: enqueue-first must reserve/wake `WOKEN`; close-first on an empty queue may produce `PEER_CLOSED`; a close on a non-empty queue must not stamp `PEER_CLOSED`.

4. **Reservation semantics are incomplete.**

   > “reserve the queue head for a `WOKEN`”

   “Reservation” has no defined owner, field, cardinality, or commit protocol. With two messages and two waiters, implementations could reserve the same head, reserve two heads and complete out of FIFO order, or let another receiver steal the reserved message. The contract must state at most one outstanding head reservation per end, selection order, and release behavior on copy fault, waiter death, cancellation, timeout, close, and successful commit.

5. **Zero timeout can still become a parked one-tick wait.**

   > “Zero timeout = a non-blocking poll... never parks.”

   > “inclusive `wake <= ticks`”

   The text does not require the zero-duration branch to return before waiter registration and scheduler blocking. Uniformly setting `wake = ticks` and parking makes duration 0 indistinguishable from duration 1 under the current ISR. It must explicitly require short-circuiting before retain/register/block.

6. **A6-PEERWAKE names the wrong endpoint state and leaves teardown ordering ambiguous.**

   > “where that transitions a peer end to `CLOSED`”

   Merged behavior transitions the dying end to `CLOSED` and the live peer end to `PEER_CLOSED`. The amendment must identify which end’s waiters are awakened. It must also require: unlink/tombstone the dying process’s own waiter first, never arbitrate it, release its reservation/reference, then wake only surviving peer-owned waiters. “Same IRQ-critical teardown” alone does not establish that separation.

7. **The terminal-cause-to-ABI mapping is missing.**

   The text names `WOKEN`, `TIMED_OUT`, `CANCELLED`, and `PEER_CLOSED`, but does not define actual `RAX/RDX/R8/R9` results:

   - `WOKEN`: byte count, transferred handles, and OK status;
   - `TIMED_OUT`: status value and `RDX`;
   - `PEER_CLOSED`: presumably status 12, but not stated for blocking recv;
   - `CANCELLED`: internal frame result, despite being ABI-unreachable;
   - capacity exhaustion: `EMPTY` 13 versus `TIMED_OUT`.

   No new status values are assigned, and existing 10–13/`JE_ABI_INVALID` must not be guessed or collided with.

8. **Blocking recv does not specify composition with merged capability transfer.**

   > “owned-record copy-out + commit”

   Step 5 recv includes staged capability minting, R8/R9 handle output, rollback on output-copy failure, dead-cap handling, quota failure, and commit-time release. A6 only specifies byte copy faults. It does not say whether blocking recv supports caps, how cap mint failures behave, or how staged handles are rolled back while the reserved message remains queued.

9. **Waiter-capacity scope and failure behavior are inconsistent.**

   > “one blocked receiver per process → 2”

   Existing `JE_EP_MAX_WAITERS` is defined per endpoint end, while the current scheduler does not state/enforce one user thread per process. The amendment must define whether the bound is per end, per channel, per process, or global, and which waiter loses when full. “Assert-defeating path” is undefined; release-build behavior must be an explicit runtime branch. “EMPTY-equivalent” also conflicts with zero-timeout’s `TIMED_OUT` unless deliberately specified.

10. **Priority and “first cause wins” are not reconciled.**

   > “the first eligible cause wins”

   > “priority is fixed: `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`”

   On one CPU with IF=0, enqueue and timeout are sequential, not simultaneous. If first CAS wins, enqueue-first yields `WOKEN` even if close/timeout becomes eligible later. If priority may supersede an earlier terminal state, that violates single-terminal-CAS. The text must say priority applies only to one eligibility snapshot, and the proof must test ordered serializations rather than an impossible “tie window.”

Clean intent: A6-1’s drain-first decision, A6-2’s internal-only cancellation decision, and A6-3’s bounded/overflow-checked deadline rule are consistent with the merged contracts; the defects are in their operational and ABI wording above.
Review basis: `main` at `e914a95`; no files changed.

1. **A6-STRUCT/A6-5 contradict the current frame-return architecture.**

   > “The parked syscall continuation then validates... and performs the owned-record copy-out + commit”

   > “The scheduler only flips `THREAD_BLOCKED_IPC → READY`.”

   A user syscall has no resumable kernel continuation today. If the scheduler only marks the thread ready, the resumed `iretq` returns directly to user mode and no kernel code performs copy-out. If the continuation runs after `cr3_load` during dispatch, that contradicts “never inside `je_sched_interrupt`.” The amendment must define the exact resume mechanism and where `RAX/RDX` are written.

2. **“Re-arbitrates” conflicts with the single-terminal-CAS rule.**

   > “A copy fault... releases the head reservation, and re-arbitrates”

   The waiter is already `WOKEN`; it cannot be re-armed without a second terminal transition. If “re-arbitrate” means wake another waiter, the text must say so, including release of the first waiter’s waiter-reference and terminal bookkeeping. It must also distinguish the wait cause (`WOKEN`) from the eventual syscall status (`JE_ABI_INVALID`).

3. **A6-1 does not constrain the close wake path to the empty-queue case.**

   > “`PEER_CLOSED` is returned only when the queue is empty”

   > “peer close → `PEER_CLOSED`”

   > “calls the arbitrate CAS on any receiver blocked on that end”

   The close path could stamp `PEER_CLOSED` even when a message is queued or reserved, contradicting drain-first. Required ordering is unstated: enqueue-first must reserve/wake `WOKEN`; close-first on an empty queue may produce `PEER_CLOSED`; a close on a non-empty queue must not stamp `PEER_CLOSED`.

4. **Reservation semantics are incomplete.**

   > “reserve the queue head for a `WOKEN`”

   “Reservation” has no defined owner, field, cardinality, or commit protocol. With two messages and two waiters, implementations could reserve the same head, reserve two heads and complete out of FIFO order, or let another receiver steal the reserved message. The contract must state at most one outstanding head reservation per end, selection order, and release behavior on copy fault, waiter death, cancellation, timeout, close, and successful commit.

5. **Zero timeout can still become a parked one-tick wait.**

   > “Zero timeout = a non-blocking poll... never parks.”

   > “inclusive `wake <= ticks`”

   The text does not require the zero-duration branch to return before waiter registration and scheduler blocking. Uniformly setting `wake = ticks` and parking makes duration 0 indistinguishable from duration 1 under the current ISR. It must explicitly require short-circuiting before retain/register/block.

6. **A6-PEERWAKE names the wrong endpoint state and leaves teardown ordering ambiguous.**

   > “where that transitions a peer end to `CLOSED`”

   Merged behavior transitions the dying end to `CLOSED` and the live peer end to `PEER_CLOSED`. The amendment must identify which end’s waiters are awakened. It must also require: unlink/tombstone the dying process’s own waiter first, never arbitrate it, release its reservation/reference, then wake only surviving peer-owned waiters. “Same IRQ-critical teardown” alone does not establish that separation.

7. **The terminal-cause-to-ABI mapping is missing.**

   The text names `WOKEN`, `TIMED_OUT`, `CANCELLED`, and `PEER_CLOSED`, but does not define actual `RAX/RDX/R8/R9` results:

   - `WOKEN`: byte count, transferred handles, and OK status;
   - `TIMED_OUT`: status value and `RDX`;
   - `PEER_CLOSED`: presumably status 12, but not stated for blocking recv;
   - `CANCELLED`: internal frame result, despite being ABI-unreachable;
   - capacity exhaustion: `EMPTY` 13 versus `TIMED_OUT`.

   No new status values are assigned, and existing 10–13/`JE_ABI_INVALID` must not be guessed or collided with.

8. **Blocking recv does not specify composition with merged capability transfer.**

   > “owned-record copy-out + commit”

   Step 5 recv includes staged capability minting, R8/R9 handle output, rollback on output-copy failure, dead-cap handling, quota failure, and commit-time release. A6 only specifies byte copy faults. It does not say whether blocking recv supports caps, how cap mint failures behave, or how staged handles are rolled back while the reserved message remains queued.

9. **Waiter-capacity scope and failure behavior are inconsistent.**

   > “one blocked receiver per process → 2”

   Existing `JE_EP_MAX_WAITERS` is defined per endpoint end, while the current scheduler does not state/enforce one user thread per process. The amendment must define whether the bound is per end, per channel, per process, or global, and which waiter loses when full. “Assert-defeating path” is undefined; release-build behavior must be an explicit runtime branch. “EMPTY-equivalent” also conflicts with zero-timeout’s `TIMED_OUT` unless deliberately specified.

10. **Priority and “first cause wins” are not reconciled.**

   > “the first eligible cause wins”

   > “priority is fixed: `PEER_CLOSED > WOKEN > CANCELLED > TIMED_OUT`”

   On one CPU with IF=0, enqueue and timeout are sequential, not simultaneous. If first CAS wins, enqueue-first yields `WOKEN` even if close/timeout becomes eligible later. If priority may supersede an earlier terminal state, that violates single-terminal-CAS. The text must say priority applies only to one eligibility snapshot, and the proof must test ordered serializations rather than an impossible “tie window.”

Clean intent: A6-1’s drain-first decision, A6-2’s internal-only cancellation decision, and A6-3’s bounded/overflow-checked deadline rule are consistent with the merged contracts; the defects are in their operational and ABI wording above.

### Claude (re-review)
**Verdict:** 7 of the 10 points are fixed. Points 1 and 3 are fixed except for leftover wording or one gap, and point 8 is still broken. Points 2, 4 and 5 have no remaining issues; 6, 7 and 9 have small nits listed after the item. Applying the fixes added three new defects (N1–N3). I checked only the amendment text, not the code.

1. **FIXED, but two leftover phrases.**
   - A6-STRUCT says the entry check "applies that order", then picks WOKEN over PEER_CLOSED, which contradicts the order as written. Suggested wording: "at entry only WOKEN and PEER_CLOSED can both be eligible, and A6-1 drain-first decides."
   - A6-2 still says "the arbitration CAS and the **priority tie-break** are proven on all four causes." A6-STRUCT now says ties cannot happen. Drop "tie-break".
2. **FIXED.** "re-arbitrate" now only appears negated ("does NOT re-arbitrate").
3. **FIXED, with one gap.** TIMED_OUT=14, CANCELLED=15, op 12, R10, and zero=EMPTY are all pinned.
   - **Gap:** nothing says what blocking recv returns when it is WOKEN and the data or handle buffer is too small. The zero-timeout poll returns SHORT_BUFFER=11, but the blocking path is either unmapped or swept into "copy/mint fault → INVALID=2". Either way, blocking and polling now disagree on the same state, which breaks A6-1's "behave identically" claim. Map it explicitly, preferably to SHORT_BUFFER with the message left queued and the reservation released.
4. **FIXED.** The only remaining "immediate TIMED_OUT" mentions are the rejected alternative and the overflow case.
5. **FIXED.** All three are present: the guarded BLOCKED_IPC→READY flip, the dying process's own waiter unlinked first, and arbitrate skipping tombstoned waiters.
6. **FIXED, with one placement nit.** The ordinary last-handle `endpoint_close` coverage sits under "Flex options to consider in future", so it reads as optional or future work. Move it into the Decision.
7. **FIXED.**
8. **STILL-BROKEN:** "only one of them can be mid-`recv`-syscall-blocking at any instant … (the other is either running or ready, not simultaneously parked)". This is false. Process A parks on end E and the scheduler switches to process B. B, holding a transferred duplicate handle to E, then calls recv with a nonzero timeout. That is a second waiter on E, reached through the ABI. The consequences:
   - The fail-closed path can be reached and tested.
   - The "untestable without fabricating an impossible scheduler state" prediction is wrong.
   - The bound must be a runtime check. An `assert` that panics would let any user trigger a kernel panic (a denial of service). Also record that a nonzero-timeout recv can return EMPTY immediately in this case.
9. **FIXED, but coverage is missing for:**
   - the A6-4 occupied-end → EMPTY path, now reachable through the transferred duplicate;
   - the PEER_CLOSED wake on an ordinary `endpoint_close`;
   - the over-max and overflow `INVALID` rejections;
   - a cap-bearing or short-buffer message on WOKEN.
10. **New contradictions:** see N1–N3 below, plus the leftover wording in points 1 and 6.

**New defects**
- **N1, reserved head versus poll.** After a sender wins the CAS and reserves the head, the woken thread is READY but not yet running. A zero-timeout `try_recv` from a process holding a duplicate handle to that end can run in that window. Nothing says whether that poll skips the reserved head (returning EMPTY) or takes it. Taking it breaks the reservation and FIFO; skipping it changes merged `try_recv` behavior. Specify it.
- **N2, entry check with nonzero timeout.** If a message is already deliverable at entry, does the call register a waiter and run the CAS? A6-5 says the "entry-snapshot drain" stamps WOKEN and reserves; A6-STRUCT and A6-3 imply no waiter is registered. If one is registered, the A6-4 occupied-end check could return EMPTY while a message is deliverable. Say that the entry check delivers through the plain `try_recv` path without registering a waiter.
- **N3, minor.** A6-ABI says "the four terminal causes map…" but the table has five rows, the fifth being the fault row. Reword.

### Codex (re-review)
codex
1. FIXED.
2. FIXED.
3. FIXED.
4. FIXED.
5. FIXED.
6. FIXED.
7. FIXED.
8. STILL-BROKEN: “at most one waiter per end is structurally reachable” is false; one thread can park, then another transferred-dup holder can park on the same end. The fail-closed `EMPTY` rule is present.
9. FIXED.
10. STILL-BROKEN: A6-4’s single-CPU reachability claim conflicts with its own possible “would-be second waiter” path.

NEW DEFECT: The transferred-dup capacity rationale must be corrected; single-CPU scheduling does not prevent multiple parked waiters.
1. FIXED.
2. FIXED.
3. FIXED.
4. FIXED.
5. FIXED.
6. FIXED.
7. FIXED.
8. STILL-BROKEN: “at most one waiter per end is structurally reachable” is false; one thread can park, then another transferred-dup holder can park on the same end. The fail-closed `EMPTY` rule is present.
9. FIXED.
10. STILL-BROKEN: A6-4’s single-CPU reachability claim conflicts with its own possible “would-be second waiter” path.

NEW DEFECT: The transferred-dup capacity rationale must be corrected; single-CPU scheduling does not prevent multiple parked waiters.
