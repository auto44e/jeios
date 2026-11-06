The design is sound in principle, but both questions turn up real holes. For Q5 the main bug is a WFI loop that runs with PSTATE.I set: it never gets preempted and the system hangs. For Q7, the global floors can pass while only one worker ever runs, and a frame swap is invisible whenever a worker's expected pattern comes from state that gets swapped along with the frame. Below I mark which architecture claims you should check against DDI0487 before writing them into the ADR.

---

## Q5: Races in the preemption-only design

### (a) Tick between the state store and `wfi`, or the handler re-selecting a thread before its store is visible

**The hardware is not the risk.** On a single core, the IRQ handler is the same processor observing its own earlier stores. Exceptions are taken precisely, so any store before the interrupted instruction is visible to the handler. No DMB is needed for correctness on one core.

These are the real races, each with a fix:

1. **The handler can bring a ZOMBIE or SLEEPING thread back.** If the handler does `cur->state = READY` unconditionally, a tick that lands after the thread stores ZOMBIE or SLEEPING turns it back into READY.
   - Fix: only change state on a match: `if (cur->state == RUNNING) cur->state = READY;`
   - Treat any other state as already final.
   - Add a mutation that removes the guard. It must be caught by `dispatches_after_exit[s] == 0` (see Q7c).
2. **The compiler can delay, merge or reorder the stores.** C11 does not treat `for(;;)` as a loop that ends, and an `asm volatile("wfi")` without a `"memory"` clobber can be moved across the store.
   - Fix: make `state` and `wake` `volatile`, or use `__atomic_store_n(..., __ATOMIC_RELAXED)`.
   - Give every `wfi`, `msr daif*` and compiler-barrier asm a `"memory"` clobber.
3. **Read-modify-write on shared fields loses updates.** If `state` is a bitfield, or shares a packed word with fields the handler writes, the compiler emits LDR, BFI, STR. A tick between the LDR and the STR loses the handler's write.
   - Fix: make `state` its own naturally aligned `uint32_t`.
   - Make `wake` an aligned `uint64_t`, so each is a single-copy-atomic STR.
   - Static-assert the alignment of both.
4. **Reaping a stack that is still in use.** The IRQ frame that preempts a ZOMBIE is pushed onto the ZOMBIE's own stack, so `je_arm_irq` is running on that stack until `mov sp, x0`. Reaping or re-mapping the guard inside that same IRQ is a use-after-free.
   - Fix: reap only slots where `s != cur_at_entry`, i.e. on a later tick or from the monitor thread.
   - FATAL if a reap targets the stack that holds the current `tf`.
5. **Switching before the interrupt is acknowledged.** If `je_arm_irq` ever returns another thread's frame before EOI, the GIC's running priority stays raised. No later IRQ is signalled, and every `wfi` hangs.
   - Pin the order: ACK, then EOI and re-arm, then the scheduler pick. The current `je_gic_handle_one` already does ACK+EOI first; keep that order.
   - Spurious IRQs and the `JE_ERR_BUSY` path must not tick, sweep, preempt or count.
6. **The handler's step order matters.** Use: `ticks++`, then the sweep, then the RUNNING→READY transition, then the pick, then validation, then return.
   - A sleeper whose deadline has passed may be picked straight away. That is fine as long as its loop re-checks state (see (d)).

### (b) Store order for sleep

Write the deadline first and the state second. With the reverse order, a tick can see SLEEPING with the previous sleep's deadline (an early wake), or with 0 (it never wakes, or wakes at once, depending on what 0 means).

On a single core, program order plus a compiler barrier is enough. The more robust fix is to mask I across the pair, which makes it atomic with respect to the tick and removes the problem entirely:

```c
uint64_t daif = mask_irq();               // msr daifset,#2  ("memory" clobber)
uint64_t now  = ticks;                     // volatile/relaxed LDR, inside the mask
if (n == 0 || n > JE_SCHED_SLEEP_MAX) { restore(daif); return JE_ERR_INVALID; }
t->wake  = now + n;                        // store #1
compiler_barrier();                        // asm volatile("" ::: "memory")
t->state = SLEEPING;                       // store #2
/* fall into the wait loop in (d); it unmasks */
```

- Reading `ticks` inside the masked window also removes the "deadline already passed" case. That case is harmless anyway, because the sweep uses `<=`.
- A `dmb ish` adds nothing on one core. If you add one as forward-compatibility for SMP, label it that way: SMP needs a lock, not just a barrier.
- Handler invariant: if it sees `SLEEPING` with `wake == 0` or `wake > ticks + JE_SCHED_SLEEP_MAX`, that is a FATAL invariant break, not a guess.

### (c) `ticks == 0` and deadline overflow

- **Don't use 0 to mean "no deadline"** (the x86 `wait_deadline` convention). The deadline is only valid while `state == SLEEPING`.
- **Reject `sleep(0)`** with `JE_ERR_INVALID`, or return at once; either is fine. The deadline is then `ticks + n` with `n >= 1`, so sleeping when `ticks == 0` is well-defined.
- **Document the granularity.** `wake = now + n` with sweep `ticks >= wake` gives a sleep of n−1 to n ticks. Use `now + n + 1` if you want at least n full ticks. Pick one and test it.
- **Counter overflow is not reachable.** A 64-bit counter at 100 Hz takes about 5.8×10⁹ years to wrap, so a plain `>=` comparison is correct. Don't add a signed-difference trick; it would be wrong for 64-bit values far apart.
- **The addition can overflow.** The caller's `n` can wrap `now + n` to a small number, giving an immediate wake. Bound it with `n <= JE_SCHED_SLEEP_MAX`, for example 2³². Reject rather than saturate, because a saturated value would also be a hidden "never wakes".
- **Reads of `ticks` must be aligned.** The 64-bit `ticks` read in thread context must be an aligned LDR (single-copy atomic), declared `volatile` or read with an atomic load.

### (d) Does `wfi` with I masked wake, and which DAIF state to use

- **It wakes.** A pending physical IRQ wakes WFI even when PSTATE.I masks it. Linux arm64 idle runs `wfi` with I masked and relies on this. Check the exact "WFI wake-up events" wording in DDI0487 before citing it.
- **The wake requires the GIC to signal the interrupt.** The interrupt must be enabled, its group enabled, and its priority above both PMR and the running priority. See point 5 in (a).
- **Waking is not the same as taking the interrupt.** With I masked the IRQ is never taken, so no preemption happens. The level-triggered timer stays pending, and `wfi` falls straight through every time. A loop that never unmasks spins forever and starves every thread, including idle.

**Exit loop.** The thread runs with I clear, `spsr = 0x345`: D=1, A=1, I=0, F=1, M=EL1h. That matches the PR-2b canary, which only clears I. Check it against the DAIF value your ADR 0012 boot pins.

```c
t->state = ZOMBIE;                 // single aligned store; "memory" clobber after
if (read_daif() & DAIF_I) FATAL;   // a masked exit would spin forever
for (;;) asm volatile("wfi" ::: "memory");
```

**Sleep loop.** This uses the same pattern as Linux idle to avoid losing a wake-up. Check the condition with I masked, run `wfi` with I masked (a pending IRQ still wakes it), then unmask so the IRQ is actually taken:

```asm
1:  msr   daifset, #2
    ldr   w9, [x_t, #STATE]
    cmp   w9, #SLEEPING
    b.ne  2f
    wfi                     // wakes on a pending IRQ even though I is masked
    msr   daifclr, #2
    isb                     // let the pending IRQ be taken here (preempts us)
    b     1b
2:  msr   daifclr, #2       // woken: the handler set us RUNNING and dispatched us
```

- Without the masked check, an IRQ between `check passes` and `wfi` adds up to one extra tick of latency after resume. That is not a correctness bug, but it makes the sleep timing nondeterministic.
- `ELR` for an IRQ that interrupts a WFI is the instruction after the WFI, so the loop re-checks after resume.
- The `isb` after `daifclr` is my conservative choice. Confirm the DDI0487 rule on when a pending interrupt is recognised after PSTATE is unmasked. Even without the guarantee, the loop is live.
- The idle thread (slot 0) must never sleep or exit. FATAL if it tries.

---

## Q7: Ways the proof can pass while the property is broken

### (a) Every way it passes while broken

1. **Only idle and worker A ever run. Yes, this passes.** With N=2, `(1+N)*2 = 6` switches happen in 6 ticks of idle↔A. `min_dispatches >= 3` fails only if B's count is 0 and B is in the min set. It still passes if:
   - the min is taken over slots with nonzero dispatches;
   - B was dispatched three times but immediately went ZOMBIE, faulted, or sat in `wfi`;
   - B's `elr` points somewhere other than the canary.
   
   "Dispatched" means the scheduler picked it, not that the worker ran or was preempted.
2. **Bookkeeping updates but no real switch happens.** For example, `mov sp, x0` is missing, or the asm still unwinds from the old SP. Counters say B was dispatched while A keeps running on its own intact registers. All canaries and floors pass. This is the biggest false-success.
3. **The frame is rebuilt fresh on every dispatch** (for example, the fake-frame build is re-run). The worker re-seeds from entry every time and never checks a preempted state, so the canary always passes.
4. **The IRQ never lands inside the canary.** For example, the worker finishes within one quantum and every preemption lands in the trampoline or the exit `wfi`. The canary checks never cover a resume.
5. **Both workers compute the same pattern** (a tid source bug gives tid=1 to both). A swap then becomes invisible.
6. **The expected pattern comes from swappable state** (see (b)). A whole-frame swap is self-consistent.
7. **The failure path loops instead of reporting.** A worker spinning in a fail loop still gets preempted and dispatched, so the floors keep passing.
8. **Completion is self-reported.** Done flags indexed by a register tid can be set by the wrong thread.
9. **Spurious or BUSY IRQs inflate the counts**, or a "switch" where `next == cur` is counted.
10. **The floor formula leaves out the monitor slot.** `(1+N)*2` covers idle plus workers only, so per-slot accounting doesn't add up.
11. **The oracle only greps for PASS.** It must parse and recompute the numbers itself.
12. **Exit or sleep is claimed but never exercised,** or a ZOMBIE is re-dispatched later and nobody checks.

### (b) Worker tid stored in its own frame

**Yes, this false-passes, in three variants:**

- **Swap before first run:** worker A starts with B's argument, seeds B's pattern and checks B's pattern. It is self-consistent and passes.
- **Live pointer swap:** dispatching slot A runs B's whole context on B's stack. It is consistent and passes.
- **Live content swap:** A's registers end up on B's stack. A canary that only uses registers passes. SP comes from `frame + TF_SIZE`, not from `tf->sp`, and the loop never touches the stack.

**Fix:** the expected identity must come from scheduler state that cannot be swapped.

- **Trampoline check:** `if (arg != je_sched_rr_current_slot()) FATAL;` and `starts[current]++` (must end at exactly 1).
- **Canary check, every iteration:** do a fresh volatile load of `je_sched_rr_current`. Never cache it in a register, because a cached copy is swappable state.
  - Compare each register against a value computed from `current`, e.g. `rodata_pattern[current] + r`, using the ADR 0012 D8 rotating-scratch two-phase method.
  - Also check `sp ∈ stack_extent[current]`.
  - In correct code, `current` always equals the slot that is running, because only the handler changes it, and the handler also swaps the registers.

### (c) Scheduler-side counters the monitor must assert

The handler writes these, indexed by the scheduler's slot. The monitor reads them with I masked to get a consistent snapshot.

| Counter (written in `je_arm_irq`) | Definition | Required |
|---|---|---|
| `observed_running[s]` | At IRQ entry, `cur == s` and `tf` (and `tf->sp`) lie in `stack_extent[s]`; otherwise FATAL immediately | ≥ 1 per worker. Kills item 2 (no real switch) |
| `preempted_from[s]` | `cur == s`, the state was RUNNING, and `next != s` | ≥ 1 (aim for ≥ 2) |
| `preempted_in_canary[s]` | The above, and `tf->elr ∈ [canary_start, canary_end)` | ≥ 1. Kills item 4 |
| `dispatched_to[s]` | `next == s` and `next != cur` | ≥ 3 |
| `resumed[s]` | `dispatched_to` where the frame was saved by an IRQ, not the initial fake frame | ≥ 1. Kills item 3 |
| `verified_resumes[s]` | At each preemption of s, snapshot `checks[current]`. Count the times `checks` grew between a preempt and the next preempt of s (a full canary check passed after resume) | ≥ 1 |
| `interleave[s]` | When dispatching s, another worker was dispatched since s's last dispatch | ≥ 1. Proves round-robin, not sequential |
| `starts[s]` | Trampoline entries, indexed by `current` | == 1 |
| `dispatches_after_exit[s]` | Dispatches after s became ZOMBIE | == 0 |

Consistency checks the oracle recomputes from the per-slot output:
- Σ`dispatched_to` == Σ`preempted_from` + (switch-ins from SLEEPING/ZOMBIE) == global `preemptions`.
- Every READY slot is picked within (number of READY slots) ticks.

Replace the global floor with these per-slot requirements.

**Monitor sequence:**
1. Wait until every worker meets the table above.
2. Signal the workers to exit.
3. Wait until every worker is ZOMBIE.
4. Wait at least `(N+2)` more ticks and assert no ZOMBIE was dispatched.
5. Mask I, then print every counter.

The canary's failure path sets `fail[current]` and prints FATAL immediately. It never loops silently.

### (d) NZCV test window

The flags must be set, then left untouched by any flag-setting instruction until they are read back, with the IRQ landing inside that span:

```asm
    msr   nzcv, x_pat           // per-thread, per-phase value (bits 31:28)
win_start:
    mov   x9, #SPIN
1:  sub   x9, x9, #1            // NOT subs
    cbnz  x9, 1b                // cbz/cbnz/tbz do not touch flags
win_end:
    mrs   x10, nzcv             // read BEFORE any cmp
    /* only now: cmp x10, x_pat ; b.ne fail */
```

- **Prove the IRQ landed in the window, from the scheduler side:**
  - Add `nzcv_window_preempts[s]`, counting preemptions with `tf->elr ∈ [win_start, win_end]`. An ELR equal to the `mrs` address counts, because the `mrs` hasn't run yet. Require ≥ 1 per worker.
  - In the handler, also assert that `tf->spsr[31:28]` equals the expected value for (s, phase). That proves the save was correct; the later `mrs` proves the restore.
  - Make the window take up most of the loop time so ticks naturally land in it.
- **Choose the patterns carefully:**
  - Each thread gets two values and alternates them every iteration.
  - Never use 0x0 (reset or zeroed state).
  - Never use 0x6 (Z|C): that is exactly what the handler's `cmp x1, #0x5` leaves in the live flags, so a "restored live flags instead of SPSR" bug would match it.
  - Values must differ across threads.
  - Example: A = {0x9, 0xA}, B = {0x5, 0xC}.
- **Check DAIF too:** after resume, `mrs daif` must show the expected D/A/F with I clear. That proves the correct thread's SPSR was restored.

### (e) The frame-swap mutation

Inject three variants, each targeting a specific detector:

| Variant | When to inject | Expected detector |
|---|---|---|
| Early swap (fake frames) | Before first dispatch | Trampoline `arg != current` |
| Live pointer swap | Once `preempted_in_canary[A] ≥ 1 && preempted_in_canary[B] ≥ 1`, so both frames are IRQ-saved and seeded | D2 validation if it checks `tf ∈ stack_extent[next]`; otherwise the canary |
| Live content swap (memcpy) | Same trigger | Canary register-vs-`rodata[current]` check, or the SP-extent check |

Make D2 check the stack extent of `next` specifically, not "any thread stack". Pointer swaps then die in D2 and content swaps exercise the canary, so both detectors get proven.

**The oracle must require all of these:**
1. A `MUTATION FIRED <variant> A<->B tick=T` marker, along with the `preempted_in_canary` values printed at the moment it fired.
2. The failure appears after T and within `2*(slots)` ticks, i.e. one round-robin round.
3. The *expected* detector's marker appears. For the canary, `got` must equal the other slot's pattern for that register, which proves it was the swap and not unrelated corruption.
4. There is no PASS marker.
5. A timeout, a different FATAL, or no FIRED marker means the mutation was **not killed**. It is a test defect, not a catch.
6. The normal build prints `MUTATION POINT REACHABLE` at the same trigger, proving the trigger condition can actually occur.

**Add these mutations to the battery:**

| Mutation | Must be caught by |
|---|---|
| Drop `mov sp, x0` | `observed_running` stack-extent FATAL |
| Pick skips slot B | Per-slot `preempted_in_canary`/`verified_resumes` |
| Rebuild the fake frame on each dispatch | `starts == 1` / `resumed` |
| Unconditional `cur->state = READY` | `dispatches_after_exit == 0` |
| `wfi` loop with I masked | Bounded oracle timeout plus missing completion marker, or the exit-path DAIF FATAL |
| Corrupt `spsr[31:28]` on switch | NZCV check inside the window |
| Set `spsr.I` | D2 validation |

I only read ADR 0013 and the current `vectors.S`/`je_arm_irq` for this; none of it has been implemented or run. Two architecture points still need checking against DDI0487: WFI waking on an IRQ masked by PSTATE.I, and the `daifclr` + `isb` rule for when a pending interrupt is taken. This is AI-assisted review, not independent human qualification.
