Overall verdict: do not approve ADR 0013 unchanged.

The `mov sp, x0` primitive is workable on this single-core target, but D2 validation is materially insufficient, D4 lacks the dynamic mapping mechanism required for allocated stacks, and D7 does not yet prove per-worker preemption.

1. Switch asm

Conditionally correct, with one important correction: `add sp, #304` must restore the interrupted SP, not always the stack top.

Let:

```text
F_A = current thread A's trap frame
S_A = A's interrupted SP
F_A = S_A - 0x130       // 304 bytes
```

The proposed sequence is:

```asm
bl      je_arm_irq       // x0 = selected frame pointer F_B

// clear je_arm_in_handler; must not clobber x0
...
mov     sp, x0           // SP_EL1 = F_B

ldp     x0, x1, [sp, #256]
msr     ELR_EL1, x0
msr     SPSR_EL1, x1

ldp     x0, x1, [sp, #0]
...
ldr     x30, [sp, #240]

add     sp, sp, #304
eret
```

This is valid because the restore loads from the selected frame, not the old frame. The current code already restores from current SP and deliberately ignores `tf->sp` at [vectors.S:228-254](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:228>).

For a fresh thread:

```text
F_B = stack_top - 0x130
F_B + 0x130 = stack_top
```

For a preempted thread:

```text
F_B = B's interrupted_SP - 0x130
F_B + 0x130 = B's interrupted_SP
```

So the ADR’s statement that this must always land at “the new thread’s stack top” is wrong. It must land at that context’s saved interrupted SP.

Reading through the newly installed SP is not itself a hazard; it is the intended mechanism. The hazards are:

- `x0` must remain untouched between the C return and `mov sp, x0`.
- A null or malformed return value causes an immediate stack fault unless the C contract is perfect.
- The returned frame must be the frame owned by the selected thread, not merely an aligned address inside some stack.
- `tf->sp` must still be validated even though the asm does not consume it.

The global `je_arm_in_handler` guard is acceptable for the stated single-core, I-masked design. A thread cannot be preempted “mid-handler”: its saved frame represents the state before entering the handler. The guard is cleared before `eret`, so the switched-to thread sees it clear. It would need to become per-CPU or otherwise redesigned for SMP.

2. First-switch bootstrap

Yes, the first `eret` can enter the trampoline correctly, but the fake-frame contract needs to be made exact.

Assuming:

```text
B = allocated stack base
guard = [B, B + 0x1000)
T = B + 5*0x1000
F = T - 0x130
```

The frame should contain:

```text
F + 0x000..0x0f0: gpr[0..30]
F + 0x0f8:        sp   = T
F + 0x100:        elr  = trampoline
F + 0x108:        spsr = 0x345
F + 0x110:        esr  = 0
F + 0x118:        far  = 0
F + 0x120:        vector = 0
F + 0x128:        pad = 0
```

`0x345` means:

```text
M[4:0] = 0b00101   // EL1h
I = 0                // IRQs unmasked
D = A = F = 1       // masked, matching the current proof policy
NZCV = 0
```

The GPR contract should be:

```text
x0  = entry argument
x1..x30 = 0
SP_EL1 = T after the add
ELR_EL1 = trampoline
SPSR_EL1 = 0x345
```

The sequence becomes:

```asm
mov     sp, F
ldp     x0, x1, [sp, #256]
msr     ELR_EL1, x0
msr     SPSR_EL1, x1
restore all GPRs
add     sp, sp, #304       // SP_EL1 = T
eret
```

`eret` enters `trampoline` at EL1h with SP equal to `T` and IRQs enabled.

The phrase “a trampoline return” is underspecified. Because `trampoline` is entered by `eret`, not `blr`, it has no caller-provided LR. `x30 = 0` is safe only if the trampoline is strictly non-returning: it calls the entry function and then a `_Noreturn` scheduler-exit routine. Otherwise, set `x30` to an explicit fatal-return stub.

The frame layout itself is frozen at 304 bytes and documented in [trapframe.h:32-65](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/trapframe.h:32>).

3. Frame validation

No. The proposed checks are insufficient.

Required checks include:

```c
uintptr_t p = (uintptr_t)frame;
uintptr_t lo = stack_base + PAGE_SIZE;  // guard excluded
uintptr_t hi = stack_top;

p % 16 == 0
lo <= p
p <= hi - JE_TF_SIZE
frame->sp == p + JE_TF_SIZE
frame->sp % 16 == 0
frame->sp <= hi
```

The subtraction must be overflow-safe.

For a never-run thread:

```text
frame == stack_top - JE_TF_SIZE
frame->sp == stack_top
frame->elr == trampoline
```

For a preempted thread, the frame may be anywhere in the usable stack, but it must be the exact frame pointer recorded for that thread. “Somewhere inside the stack” is not enough: a wrong 16-byte-aligned offset can contain plausible-looking data and still cause an invalid restore.

Also validate:

- `ELR_EL1` is 4-byte aligned and inside approved kernel text or the registered trampoline.
- `SPSR.M == 0b00101`.
- `SPSR.I == 0`.
- Reserved/unsupported SPSR bits are clear.
- `IL` and illegal-return-sensitive state are rejected.
- D/A/F and feature bits match the selected policy.
- The frame belongs to the selected thread’s stack and ownership record.
- The frame kind is correct: fresh, preempted kernel thread, or idle frame.

The current three-hook signature is too weak for this. `frame_check(const void *)` needs either the owning thread/context or an architecture context containing stack bounds and first-run state.

The cheap design is a live-thread table containing:

```text
stack_allocation_base
usable_stack_base
stack_top
saved_frame
first_run
state
```

Then validation is a bounded table lookup, not a scan of arbitrary memory.

4. Neutral scheduler core

The `void *` frame seam is directionally right and should support hosted tests, but the vtable is incomplete.

Problems:

- `frame_check()` lacks owner, stack bounds, and frame phase.
- `dispatch_install()` returns `void`; installation can fail and needs rollback.
- There is no stack allocation/free or frame destruction hook.
- There is no guard-page lifecycle hook.
- There is no idle/wait hook; the core must not embed AArch64 `wfi`.
- `frame_init()` receives only `stack_top`, not the usable range or ownership token.
- Reaping needs to prove the frame and stack are no longer current before freeing them.
- The ARM IRQ path needs an explicit distinction between a real timer tick, spurious/busy wakeup, and fatal unexpected IRQ. A busy/spurious interrupt must not advance scheduler ticks or preempt.

A better conceptual interface is:

```c
frame_init(thread_context *, entry, arg, &frame)
frame_check(thread_context *, frame, phase)
dispatch_install(prev_context, next_context)
frame_destroy(thread_context *)
stack_destroy(thread_context *)
```

Hosted testing is absolutely viable. Fake frame objects can test:

- RR order.
- Idle fallback.
- SLEEPING-to-READY deadlines.
- ZOMBIE exclusion.
- Tick/preemption/dispatch counters.
- Frame-init failure rollback.
- Dispatch-install failure.
- Reaping rules.
- Frame ownership and frame-swap rejection.

The current x86 scheduler’s state transition shape is useful evidence, but it must remain untouched; see [sched.c:311-360](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:311>).

5. Preemption-only exit/sleep

Exit is safe if the state store happens before the WFI loop:

```text
state = ZOMBIE
compiler barrier
wfi
```

A tick before the state store sees RUNNING and performs an ordinary preemption. A tick after the state store sees ZOMBIE and must not select the thread. A spurious WFI wake must simply recheck the state.

Sleep has an additional race because it writes both a deadline and a state. The safe order is:

```text
validate ticks and overflow
wake_deadline = now + ticks
compiler barrier
state = SLEEPING       // publication store must be last
compiler barrier
wfi loop
```

Do not write `SLEEPING` first and the deadline second; a tick between those stores could observe a sleeping thread with an old or zero deadline.

Reject `ticks == 0` unless immediate wake semantics are explicitly defined. Check deadline overflow.

The RR picker must select only `READY` entries. It should sweep:

```c
if (state == SLEEPING && wake <= current_tick)
    state = READY;
```

Then, after handling the current frame, convert only `RUNNING` to `READY`. `ZOMBIE` and `SLEEPING` remain excluded. Slot 0 must remain a valid READY/RUNNING idle frame.

6. Guard page and TLBI

The maintenance sequence is broadly correct for this single-core, 4-KiB, four-level identity map:

```asm
str     xzr, [l3_leaf]
dsb     ishst
tlbi    vae1is, xN
dsb     ish
isb
```

`VAE1IS` is valid on one core; “IS” simply names the inner-shareable domain, which contains one PE here. `VALE1IS` would be a narrower optimization because only the L3 leaf changes. The intermediate table descriptors do not need to be cleared. The current builder uses L3 leaves and no block mappings ([paging.c:278-305](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/paging.c:278>)).

The helper must explicitly define the TLBI operand encoding. In typical AArch64 inline-asm helpers, pass the page number:

```asm
and     xN, xN, #~0xfff
lsr     xN, xN, #12
tlbi    vae1is, xN
```

Do not leave `TLBI VAE1IS, va` ambiguous.

I would use `dsb ish` before TLBI unless the implementation explicitly documents and tests why `dsb ishst` is sufficient for this table-store path.

The larger defect is that D4 currently has no way to map the usable pages of dynamically allocated thread stacks. The current MMU setup maps the image stack region, not arbitrary pages returned later by `je_arm_pages` ([main.c:853-871](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/main.c:853>)). Unmapping the guard does not make the other four pages accessible.

D4 therefore needs one of:

- preallocate/map all scheduler stacks before MMU enable; or
- add a checked runtime map/unmap API in `paging.c`, with ownership and TLB maintenance.

Unmapping another thread’s guard page is safe only if it is exactly that thread’s reserved lowest page. Unmapping an arbitrary page of an active stack is unsafe. Reaping must also coordinate mapping state, allocator state, and stale frame pointers.

7. Proof quality

The proposed proof is substantially better than the x86 proof, but it is not closed as written.

Per-thread distinct patterns catch a frame swap only if the expected pattern is bound immutably to the worker identity. This can false-pass:

```text
worker A and worker B use the same loop
frame contains tid
loop derives expected pattern from tid
frames are swapped before first execution
```

Both threads may then accept the swapped identity.

Use either:

- separate worker entry code with compile-time-distinct constants;
- an immutable per-worker expected-pattern table selected by a stable scheduler-owned identity; or
- scheduler-side expected-pattern metadata not stored in the swappable GPR frame.

NZCV also needs a precise test window. `cmp`, `subs`, and conditional operations modify NZCV. Set known flags, execute only flag-preserving instructions while IRQs are enabled, and read `NZCV` immediately after return before another flag-writing instruction. Merely having NZCV in `SPSR_EL1` does not prove the loop tested it.

The proof must assert, separately for every worker:

```text
started == true
dispatches >= 2
timer_preemptions_from_worker >= 1
resume/heartbeat count advanced after a real IRQ
worker reached its completion state
```

Global `preemptions >= floor` and `min_dispatches >= 3` are not enough. The following false-success path is otherwise possible:

```text
idle <-> worker A runs repeatedly
worker B is never selected
global counters still reach their floors
```

The monitor must verify worker-specific scheduler counters, not only worker output. Those counters should be incremented by the scheduler when selecting/preempting a slot, not voluntarily reported by the worker.

The frame-swap mutation should be performed after both workers have established their distinct live contexts, and the oracle must require both workers’ identities and counters.

8. Frozen contracts and scope

This requires owner escalation before implementation.

D2 explicitly re-edits the frozen PR-2b vectors path and reverses the stated safety rationale at [vectors.S:234-236](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:234>). C-side validation is a useful mitigation, but it is not equivalent to preserving the original invariant: a C bug, validation omission, or corrupted return value can still install an arbitrary SP.

The following must be treated as owner-gated contract changes:

- `mov sp, x0` in the frozen returning IRQ path.
- Reversal of the “never load SP from frame” rationale.
- The `je_arm_irq` signature and ownership semantics at [trapframe.h:67-70](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/trapframe.h:67>).
- The complete frame-validation contract.
- Runtime page-table mutation for thread stacks.

No `kernel/core/sched.c` change should be made in this PR. The proposed ADR correctly says the tested x86 scheduler is untouched ([ADR 0013:45-55](</home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/docs/adr/0013-mp2-aarch64-scheduling.md:45>)).

I strongly recommend splitting the work:

1. `sched_rr.c` plus hosted fake-frame/state-machine tests.
2. ARM stack ownership, mapping, guard unmap, and TLBI tests.
3. Owner-reviewed ARM signature/vector switch and first-switch bootstrap.
4. Multi-worker preemption proof and mutation oracle.

The current branch contains the proposed ADR, not an implementation; this review was read-only, so no build or AArch64 runtime result was generated.
