# ADR 0013: MP2 aarch64 preemptive kernel-thread scheduling

Status: **accepted** (2026-11-05). Owner-approved scope + decisions, then a
two-lineage design review at max effort (Codex xhigh implementor-eye + Claude
opus adversarial; transcripts under
`docs/adr/debates/0013-mp2-sched-design-review/`) that corrected the switch
mechanics, the frame-validation contract, the preemption-only race/`wfi`
discipline, the stack-mapping gap, and the proof's false-success paths. Target:
QEMU `virt-8.2` / `cortex-a72`, EL1, single core, MMU on (ADR 0011), GICv2 + CNTP
timer + the returning EL1h IRQ path (ADR 0012). This ADR adds **preemptive
round-robin scheduling of ≥2 kernel threads** on the 100 Hz tick, proving all
GPRs + NZCV + the architectural return state survive preemption. NO EL0, NO IPC,
NO cooperative yield — those are MP3.

## Context (verified ground truth, 2026-11-05)

As in the prior draft (x86 `sched.c` switches by returning a frame via
`movq %rax,%rsp; iretq`; its RR core is arch-neutral in shape but the file is
x86/M3/M4/tested-coupled and NOT linked on ARM; ARM has the returning
`je_arm_irq` on each tick, the frozen 304-byte `je_arm_trapframe`, the file-static
`je_arm_pages`, the identity MMU map, and `je_gic_handle_one`; ARM is
`-mgeneral-regs-only` so the trapframe is a complete kernel-thread context).
**Design-review correction:** the current MMU map covers the IMAGE stack region
only — it does NOT map arbitrary pages returned later by `je_arm_pages`, so
allocated thread stacks are not reachable without new mapping work (see D4).

## Decisions

### D1. Arch-neutral `sched_rr.c` core (owner: c primary; a/b fallback)
A new pure, hosted-testable **`kernel/core/sched_rr.c`**: the thread table +
states (EMPTY/RUNNING/READY/SLEEPING/ZOMBIE), init/create, the round-robin pick
(only READY; slot 0 idle always selectable; ZOMBIE/SLEEPING excluded), the
SLEEPING→READY sweep, and the tick + per-slot counter bookkeeping (D7). Frames
are opaque `void *`; the core never reads frame internals. The per-arch seam is a
**thread-context** object (carrying stack bounds + first-run phase + the saved
frame ptr + ownership) plus hooks — the 3-hook vtable in the draft was too weak
(review Q3/Q4). Hooks: `frame_init(ctx, entry, arg, &frame)`,
`frame_check(ctx, frame, phase)` (bounds + kind + EL + exact-recorded-pointer,
D2), `dispatch_install(prev_ctx, next_ctx)` (ARM kernel: no-op; may fail →
rollback), `stack_alloc(ctx)/stack_free(ctx)`, and an **idle/wait** hook (the
core must NOT embed `wfi`). x86 `sched.c` is **UNTOUCHED this PR** (migration is a
later owner-approved PR; M2 smoke + `test_sched_killall` are the regression net).
Options (a) refactor `sched.c`, (b) ARM-only slice remain occasional fallbacks;
record the deviation if used. The core is driven by host tests with fake frames
(RR order, idle fallback, SLEEPING deadlines, ZOMBIE exclusion, the counters,
frame-init/dispatch failure rollback, reap rules, frame-swap rejection).

### D2. Switch: return-a-frame + FULL C-side validation
`je_arm_irq` becomes **`void *je_arm_irq(je_arm_trapframe *tf)`** returning the
frame to resume. `vectors.S` `je_arm_irq_common` gains `mov sp, x0` after
`bl je_arm_irq` (x0 preserved across the guard-clear) and before the register
restore; the existing `add sp, sp, #JE_TF_SIZE` then lands SP at **that context's
saved interrupted SP** (= `frame + 304`) — which for a fresh thread is the stack
top and for a preempted thread is its interrupted SP. (The draft's "always the
stack top" was wrong — review Q1.) This re-edits the frozen PR-2b asm and
reverses its "never load SP from the frame" rationale, so the returned pointer is
**validated in C** (and the asm assumes validated); a failing pointer is FATAL,
never eret'd into. Validation (review Q3) — all required:
- `p % 16 == 0`; `p` lies in the SELECTED thread's usable stack
  `[usable_base, top - JE_TF_SIZE]` (guard page excluded); overflow-safe.
- **`p` equals the exact frame pointer recorded for that thread** (a right-stack
  wrong-offset frame is rejected — this is what kills a live frame-swap, D7).
- `frame->sp == p + JE_TF_SIZE`, 16-aligned, `<= top`.
- `frame->spsr.M == 0b0101` (EL1h), `spsr.I == 0`, IL and reserved/unsupported
  SPSR bits clear, D/A/F per the pinned policy.
- `frame->elr` 4-byte aligned and inside approved kernel text or the registered
  trampoline.
The global `je_arm_in_handler` guard stays (correct for single-core, I-masked
in-handler; becomes per-CPU for SMP). ACK+EOI happen in `je_gic_handle_one`
BEFORE the scheduler pick (else the GIC running priority stays raised and every
`wfi` hangs — review Q5.5); the spurious/`JE_ERR_BUSY` path never ticks, sweeps,
preempts, or counts.

### D3. Preemption-only yield + the `wfi` discipline (owner rec + flex)
No cooperative yield. Exit/sleep rely on the next tick preempting a `wfi`.
Review-corrected mechanics:
- `state`/`wake` are naturally-aligned, own-word, `volatile` (or relaxed-atomic)
  fields (never a bitfield — an RMW would lose the handler's write); every `wfi`
  / `msr daif*` / barrier asm has a `"memory"` clobber; `_Static_assert` their
  alignment.
- **Exit:** `state = ZOMBIE` (single aligned store); if `DAIF.I` is masked →
  FATAL (a masked exit `wfi` would spin forever); then `for(;;) wfi` with I
  CLEAR, so the next tick is TAKEN and preempts.
- **Sleep:** mask I across the pair → read `ticks` → reject `n==0` or
  `n > JE_SCHED_SLEEP_MAX` (bounds `now+n` overflow) → `wake = now+n` (store #1) →
  barrier → `state = SLEEPING` (store #2) → the wait loop: with I masked, re-check
  `state==SLEEPING`, `wfi` (wakes on a pending IRQ even with I masked), then
  `daifclr #2; isb` so the pending IRQ is TAKEN and preempts; loop. The handler
  FATALs if it sees `SLEEPING` with `wake==0` or an out-of-range deadline. Sleep
  granularity (n−1..n ticks with `ticks>=wake`) is documented + tested.
- The handler sets `cur->state = READY` **only if it was RUNNING** (never resurrect
  a ZOMBIE/SLEEPING — review Q5.1). Slot 0 idle never sleeps/exits (FATAL if it
  tries). The 64-bit tick counter never wraps (5.8e9 yr at 100 Hz); plain `>=`.
- **Reap only a slot that is not the one whose stack holds the current `tf`**
  (the IRQ runs on the ZOMBIE's own stack until `mov sp,x0`); reaping that stack
  is a use-after-free → do it on a later tick / from the monitor; FATAL if a reap
  targets the current `tf`'s stack (review Q5.4).

### D4. Mapped, guarded per-thread stacks (owner approved; review-corrected)
**The allocated stack must be MAPPED** — the static MMU map does not cover
`je_arm_pages` frames (review Q6, a real defect in the draft). `paging.c`/
`je_arm_mmu` gain a minimal, checked **runtime map + unmap** API:
`je_arm_mmu_map_page(va, pa, class)` and `je_arm_mmu_unmap_page(va)`, each doing
the single-leaf edit + maintenance `DSB ISHST; TLBI VALE1IS, (va>>12); DSB ISH;
ISB` (VALE1IS — only the L3 leaf changes; IS domain is one PE here; the TLBI
operand is the page number, encoded explicitly). Each kernel thread gets a 5-page
stack from `je_arm_pages`: the top 4 pages mapped RW+XN (identity VA==PA, inside
the existing window), the lowest page left UNMAPPED as a guard. A store into the
guard faults deterministically (data abort, translation fault, FAR in the guard),
proven by a deliberate sub-test. Unmapping only ever targets a thread's own
reserved guard page. On reap the stack pages are unmapped and returned to the
allocator, coordinated so no stale frame pointer or mapping outlives the thread.
(Alternative considered + rejected for MP2: pre-map all scheduler stacks before
MMU enable — less flexible and still needs unmap for reap.)

### D5. IRQ unmask only in `ARM_SCHED_TEST` (owner approved)
A new `ARM_SCHED_TEST` build (joins the mutually-exclusive toggle group) creates
the workers, unmasks IRQ, runs the proof. The DEFAULT boot stays IRQ-masked and
halts as today (ADR 0012 D1). Default-unmask flips on when MP3 gives real work
(recorded flex).

### D6. The fake frame (pinned)
`gpr[0] = entry arg`, `gpr[1..30] = 0`; `elr = trampoline`; `spsr = 0x345`
(M=EL1h, I=0, D=A=F=1, NZCV=0 — matching the ADR 0012 boot/canary policy);
`sp = stack_top`; frame at `stack_top - JE_TF_SIZE` (so post-eret SP = stack_top,
16-aligned); `esr/far/vector/pad = 0`. The trampoline is strictly `_Noreturn`
(calls `entry(arg)` then `je_sched_rr_exit`), so `x30 = 0` is safe; else set x30
to a fatal stub. No FP state.

### D7. Proof: per-slot scheduler-side counters + scheduler-owned identity
The draft's global floors + frame-stored identity false-pass (review Q7): a
missing `mov sp,x0` passes all canaries while one thread runs throughout; an
idle↔one-worker schedule meets `(1+N)*2`; a tid read from the swappable frame
makes a swap self-consistent. Corrected proof (ARM_SCHED_TEST):
- **Identity is scheduler-owned.** The RR core exposes `je_sched_rr_current()`;
  the trampoline FATALs unless `arg == current` and bumps `starts[current]`
  (must end == 1). Each worker's canary loads `current` FRESH (volatile, never
  cached) each iteration and checks its registers against `rodata_pattern[current]
  + reg` (per-thread DISTINCT, via the ADR 0012 D8 two-phase rotating-scratch for
  x29/x30) plus `sp ∈ stack_extent[current]`.
- **NZCV window:** set `nzcv` to a per-(slot,phase) value (never 0x0; never 0x6 =
  the handler's `cmp #0x5` residue; distinct across threads, e.g. A={0x9,0xA}
  B={0x5,0xC}), spin with flag-PRESERVING ops (`sub`/`cbnz`, not `subs`), `mrs
  nzcv` BEFORE any `cmp`. The handler asserts `tf->spsr[31:28]` == expected
  (save proof); the `mrs` proves restore.
- **Per-slot counters written by the handler** (indexed by the scheduler slot,
  read by the monitor with I masked): `observed_running[s]` (FATAL at entry if
  `cur!=s` or `tf`/`tf->sp` outside `stack_extent[s]` — kills the missing-`mov
  sp,x0`), `preempted_from[s]`, `preempted_in_canary[s]` (elr in the canary
  range), `dispatched_to[s]`, `resumed[s]` (frame was IRQ-saved, not the fake
  frame — kills frame-rebuild), `verified_resumes[s]`, `interleave[s]` (another
  worker ran since s's last dispatch — proves real round-robin, not sequential),
  `nzcv_window_preempts[s]`, `starts[s]==1`, `dispatches_after_exit[s]==0`.
  Requirements per worker: `observed_running≥1`, `preempted_in_canary≥1`,
  `resumed≥1`, `verified_resumes≥1`, `interleave≥1`, `dispatched_to≥3`,
  `nzcv_window_preempts≥1`. The oracle recomputes Σ`dispatched_to` ==
  global `preemptions` + non-preempt switch-ins, and that every READY slot is
  picked within (#READY) ticks — NOT just the global floor.
- **Monitor sequence:** wait until every worker meets the table; signal exit;
  wait all ZOMBIE; wait ≥(N+2) ticks and assert no ZOMBIE was dispatched; mask I;
  print every counter. The canary fail path sets `fail[current]` and prints FATAL
  immediately (never loops silently).
- **Mutation battery (acceptance bar), each with its named detector:** drop
  `mov sp,x0` → `observed_running` extent FATAL; pick skips slot B →
  per-slot `preempted_in_canary`/`verified_resumes`; rebuild fake frame each
  dispatch → `starts==1`/`resumed`; unconditional `cur=READY` →
  `dispatches_after_exit==0`; `wfi` loop with I masked → bounded oracle timeout +
  missing completion / the exit DAIF FATAL; corrupt `spsr[31:28]` on switch → the
  NZCV window check; set `spsr.I` → D2 validation; and the three **frame-swap**
  variants (early fake-frame swap → trampoline `arg!=current`; live pointer swap →
  D2 exact-recorded-pointer / `next` stack-extent; live content swap → the
  canary register-vs-`rodata[current]`). Each mutation must emit a `MUTATION
  FIRED` marker, the expected detector's FATAL within one RR round, and NO PASS;
  the normal build prints `MUTATION POINT REACHABLE` to prove the trigger occurs.

## Why / predictions / flex (ADR 0000 form)
- **Why:** preemption + context switch is the last MP2 kernel capability before
  MP3; the returning IRQ path already carries a full context one `mov sp,x0` from
  a switch; a neutral `sched_rr.c` shares the algorithm without risking tested
  `sched.c`.
- **If other choices:** refactoring `sched.c` now pushes a big diff through tested
  user paths for no MP2 benefit; a cooperative `svc` yield is a second frozen-asm
  change with no MP2 consumer; mapping stacks statically is less flexible and
  still needs unmap.
- **Flex:** migrate `sched.c` onto `sched_rr.c` (later); cooperative yield (MP3);
  default-boot unmask; per-CPU run queue + TLBI …IS + GIC redistributors (SMP);
  priorities/cancellation. Later gates.

## Evidence required
Core commit, toolchain + QEMU versions, the pinned invocation, Image hash, serial
log, exact ordered markers. Prove: ≥2 threads with mapped+guarded stacks; the
per-slot counter table met for every worker (not just global floors); each
worker's distinct GPR + NZCV pattern survived every resume; the guard-page fault;
the mutation battery each FIRED + killed by its named detector; MP1/MMU/GIC/timer
markers still pass; x86 `make check` + M2 smoke unchanged (no `sched.c` touch).
QEMU TCG models no real interrupt latency or caches — timing is a lower bound, not
a precision/fairness claim. Two DDI0487 points to confirm in-code with citations:
WFI waking on a PSTATE.I-masked IRQ, and the `daifclr`+`isb` point at which a
pending IRQ is taken. AI-assisted review is not independent human qualification.

## Amendment (2026-11-06): migration trigger for the "migrate later" flex

Fix-gate F13 requires the trigger-less flex above ("migrate `sched.c` onto
`sched_rr.c` (later)") to name a concrete trigger. That migration is now governed
by **ADR 0014 (pre-MP3 process-lifecycle ownership)**, which records:

- a bounded **lifecycle-only** extraction (not a run-queue rewrite) with a single
  ownership boundary, as a **pre-MP3 blocker**, owner-gated and not yet
  authorized; and
- an explicit **migration trigger**: the x86 `sched.c` run queue migrates onto
  `sched_rr.c` only when **either** the next scheduler *feature* lands that would
  otherwise be implemented twice (cooperative yield, priorities, SMP readiness)
  **or** a **cross-arch scheduler parity claim** is made.

This amends only the Flex line's open-endedness; D1 (x86 `sched.c` untouched this
PR) and all other ADR 0013 decisions are unchanged. The migration itself remains
a further owner-approved ADR. See `docs/adr/0014-pre-mp3-process-lifecycle-ownership.md`.
