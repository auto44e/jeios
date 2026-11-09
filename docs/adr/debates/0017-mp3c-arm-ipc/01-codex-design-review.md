Overall verdict: MP3c is not ready to implement as written. The largest holes are the ARM resume assertion, the idle-frame design, and ARM waiter reclamation.

| Decision | Verdict |
|---|---|
| D1 | ESCALATE / owner-gated |
| D2 | ESCALATE / owner-gated |
| D3 | FIX + ESCALATE / owner-gated |
| D4 | ESCALATE / owner-gated |
| D5 | FIX |
| D6 | FIX + ESCALATE |

### D1 — shared syscall glue

ESCALATE. The frame-neutral extraction is directionally correct, but the proposed single API is too coarse.

`je_syscall_decode()` already defines the canonical argument order as `{RDI, RSI, RDX, R10, R8, R9}`. [syscall.c:39](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/syscall.c:39) ARM currently returns early for non-OK decode status, while x86 dispatches process/device actions before its main switch. [syscall_arm.c:72](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/syscall_arm.c:72) [syscall.c:775](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/syscall.c:775)

Recommended shape:

- Decode exactly once.
- Pass the decoded action plus canonical args to a neutral glue worker.
- Keep frame writes, scheduling actions, and x86 process/device extensions in arch-specific wrappers.
- Ensure ARM never gains x86-only PROCESS/DEVICE authority merely because HANDLE_CLOSE moved into shared code.

As written, `je_syscall_glue(op,args,...)` risks double decoding or moving x86-specific dispatch semantics into the common TU.

The x86 byte-identical requirement is incompatible with materially moving the tested cases into a new TU and changing their call graph/register allocation. The minimal byte-preserving shape is to leave the x86 dispatcher and continuation prologue unchanged and use shared neutral workers from ARM first. A genuinely shared x86 path requires owner approval plus an exact pre/post `objdump` comparison.

### D2 — shared `BLOCKED` scheduler state

ESCALATE. The state model is sound if `JE_RR_BLOCKED` is appended, not inserted.

Current `je_rr_pick()` selects only `READY`, and `je_rr_switch()` already rejects non-READY targets. [sched_rr.c:88](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched_rr.c:88) [sched_rr.c:125](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched_rr.c:125)

Required invariants:

- `BLOCKED` is distinct from `SLEEPING`.
- `je_rr_tick()` remains SLEEPING-only; otherwise it could make a waiter runnable without a D7 terminal cause. [sched_rr.c:107](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched_rr.c:107)
- `je_rr_wake()` must only perform `BLOCKED → READY`.
- The endpoint must arbitrate `WAITING → TIMED_OUT/WOKEN/PEER_CLOSED` before calling the scheduler wake.
- Add tests for blocked exclusion, invalid blocked target switching, idempotent wake, and blocked-to-ZOMBIE death.

The ARM deadline sweep belongs in `je_arm_sched_irq()`, not `je_rr_tick()`, because endpoint arbitration owns the terminal cause.

### D3 — ARM park/restart and idle

FIX, owner-gated.

#### ELR restart

`ELR -= 4` is the correct architectural rewind for a fixed-width AArch64 `SVC`: the vector saves the architectural `ELR_EL1` into the frame, and the SVC instruction is 4 bytes. [vectors.S:540](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:540)

However, the proposed resume assertion is wrong.

If:

```text
P = original SVC address
post_svc_elr = P + 4
parked frame ELR = P
```

then re-entering SVC creates a new trap frame whose ELR is again `P + 4`, not `P`. Therefore the continuation must assert:

```text
new_tf->elr == saved_post_svc_elr
```

not:

```text
new_tf->elr == saved_post_svc_elr - 4
```

This should mirror x86, where the restarted trap asserts the saved post-`int` RIP. [syscall.c:753](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/syscall.c:753)

There is a second hole: `frame_shape_class()` requires resumed ELR to remain at or above `JE_VM_ARENA_BASE`. [sched_arm.c:387](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:387) A blocking SVC located at the first instruction of the arena would rewind to `ARENA_BASE - 4`. Add a pre-park condition requiring:

```text
post_svc_elr >= JE_VM_ARENA_BASE + 4
```

or reject that block attempt through the normal death/error path.

The continuation must return immediately after finishing the blocked receive. If it clears pending state and then falls through into ordinary decode, the syscall executes twice.

#### Timer race and lost wake

The race is safe only if DAIF.I remains masked through:

```text
entry probe → waiter register → continuation save → BLOCKED state → scheduler switch
```

The endpoint wake bridge explicitly assumes interrupts are disabled. [endpoint_kern.c:420](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/endpoint_kern.c:420)

Add an ARM assertion for that invariant. Also handle rollback: if frame validation, the restart-site check, or scheduler switching fails after waiter registration, release the waiter before death. The x86 implementation does this through its pre-park validation and lifecycle release. [sched.c:365](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:365)

#### Idle

Do not execute `wfi` inside `arm_switch_to_ready()` or `arm_idle_wait()`. The existing code explicitly avoids that because the exception-handler guard is still active. [sched_arm.c:451](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:451)

A real reserved slot-0 idle frame is required:

1. Switch to slot 0.
2. Exception return clears the handler guard.
3. Slot-0 code executes WFI with interrupts unmasked.
4. Timer IRQ enters normally and returns to the scheduler.

The RR core currently documents that slot 0 has no core-created frame. [sched_rr.h:105](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/include/jeios/sched_rr.h:105) ARM currently fatals when no worker is selected. [sched_arm.c:1002](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:1002)

There is another required change: when slot 0 is idle and a timer wakes a blocked user, `je_arm_sched_irq()` must switch to the newly READY user. The current ARM path only switches when the current slot is a user. [sched_arm.c:1225](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:1225)

### D4 — saved `args[6]`

ESCALATE, owner-gated.

The canonical array is appropriate:

```text
x86:  rdi, rsi, rdx, r10, r8, r9
ARM:  x0,  x1,  x2,  x3,  x4, x5
```

Save all six, even though the continuation currently consumes only args 1, 2, 4, and 5. Do not recompute the timeout or re-resolve args[0] on resume. The owned `rec/side` is authoritative under A6-5.

The x86 kill path reads the saved frame RIP, not the named wait fields, so changing the storage layout does not alter the kill decision. [sched.c:701](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:701)

Still required:

- Store continuation state per scheduler slot.
- Save the post-trap PC separately from `args[6]`.
- Clear pending state during death before slot reuse.
- Preserve the invariant that only `finish_blocked_recv()` may consume the owned endpoint reference.

This change cannot independently guarantee x86 byte identity.

### D5 — ARM endpoint/device wiring

FIX. The fail-closed device stub is the correct choice for MP3c, but the current macro is unsafe.

`JE_ARM_NO_DEVICE_HOOKS` currently disables `je_lc_release_waiter()`. [lifecycle.c:89](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/lifecycle.c:89) That would make a dead blocked ARM process leak its endpoint waiter and possibly a reserved queue head. Split the policies:

- ARM device operations: fail-closed stub returning `UNSUPPORTED`.
- Endpoint waiter release: always enabled.

The endpoint bridge also refuses creation and seeding unless the VM is wired. [endpoint_kern.c:78](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/endpoint_kern.c:78) [endpoint_kern.c:334](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/endpoint_kern.c:334)

The current ARM EL0 setup initializes the VM and process pool but does not install the endpoint bridge or set the endpoint process pool. [el0_test.c:1047](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/el0_test.c:1047) Those calls must occur before endpoint creation or bootstrap seeding.

### D6 — proof plan

FIX + ESCALATE. The proposed coverage is appropriate, but several tests are not yet constructible through the real ARM path.

- A creates an endpoint pair cannot by itself give B an endpoint. Use the existing seeded bootstrap channel, then transfer the created capability over it. ADR 0009 already specifies this fixture mechanism. [0009-m3c-capability-ipc.md:279](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/docs/adr/0009-m3c-capability-ipc.md:279)
- Force B to park before A sends, proving `WOKEN` rather than a timeout race.
- Timeout must perform endpoint arbitration before `BLOCKED → READY`.
- Peer-close must prove drain-first behavior; the existing bridge intentionally refuses a peer-close wake while inbound data remains. [endpoint_kern.c:438](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/endpoint_kern.c:438)
- Dead caps must produce handle `0` while later live siblings still mint.
- Commit mismatch must remain fatal after all staging/copy steps.
- Death must release both the waiter and any WOKEN reservation before slot reuse.

The A6-5 re-resolve negative is currently under-specified. With one thread per process, B cannot concurrently close and reuse its own handle while parked. `je_proc_link()` does enforce one thread per process. [proc.c:85](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/proc.c:85) But the proof needs either an approved kernel-side mutation/fault profile or a revised test argument; a simple user-level two-process test cannot create that handle-reuse race.

The ARM creation path also allocates an RR slot before discovering a duplicate process link. [sched_arm.c:755](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:755) This does not violate one-thread execution, but it should be preflighted or rolled back cleanly before claiming the prerequisite is fully enforced.

Frozen-contract result:

- D7: preserved only if endpoint cause arbitration precedes scheduler wake.
- D8: preserved only if resume uses owned `rec/side` and destination-side mint.
- D9/A5: preserved only if dequeue, cap-ref release, waiter release, and dead-cap NULL handling retain current ordering. [endpoint.c:958](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/endpoint.c:958)
- A6-5: the ARM continuation must never re-resolve the original handle.
- A6-CLARIFY-1: current RX-only user text and one-thread-per-process contracts remain valid, but ARM needs the restart-site lower-bound check.

No code or tests were changed in this review. The findings were posted to the requested agent channel successfully (`HTTP 200`).
