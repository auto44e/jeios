# ADR 0017: MP3c — AArch64 capability IPC over endpoints + blocking-receive restart

Status: **accepted** (owner-approved 2026-11-09). The owner approved all recommendations:
D1 = Option 1 (a token-identical `#include`'d case body with per-arch accessor macros,
keeping x86 codegen byte-identical) and the three tested-path touches (a `JE_RR_BLOCKED`
state in the shared RR scheduler, a reserved slot-0 ARM idle, and splitting
`JE_ARM_NO_DEVICE_HOOKS` so the endpoint waiter-release is always compiled). The rejected
D1 alternatives are recorded as dated flex notes below. As with ADR 0015/0016, once MP3 is
entirely complete the planning agent audits the result and may adjust without reopening the
decision shape.

MP3c is the aarch64 analogue of the x86 M3c milestone and the final MP3 slice: capability
IPC over ENDPOINT objects (create / send / receive, capability transfer by **destination-side
mint with independent attenuation**, never a raw handle copy) plus a **blocking receive** that
parks a process and **restarts** the SVC on wake. It also cashes the ADR-0016 D2 flex: the
ARM-local SELF-only handle glue is replaced by a shared, frame-neutral syscall-glue layer that
both x86 and ARM drive.

It changes previously-tested code — the shared round-robin scheduler (`sched_rr.c`, tested by
`test_sched_rr`), the ARM scheduler's no-idle/fatal-on-no-successor behavior, the
`JE_ARM_NO_DEVICE_HOOKS` gate, and (via the shared glue) the x86 endpoint dispatch — under
that approval, and it preserves the frozen IPC contract of **ADR 0009** (D7 one-waiter /
one-terminal-transition, D8 transactional mint/attenuate, D9 bounded reclamation, A5 dead-cap
NULL marker + discard order, A6-5 owned-reference / no re-resolve on resume, A6-CLARIFY-1
restart prerequisites) and **ADR 0008** typed handles.

Settled by a two-lineage pre-code design review (Codex + Claude, both read the code; Claude
prototyped the RR change and measured the x86 codegen sensitivity), which caught three blockers
and corrected the first design's resume assertion before any code was written. Transcripts under
`docs/adr/debates/0017-mp3c-arm-ipc/`. AI-assisted review, not independent human qualification.
Reviewed baseline: `main` 52adf4f (post-MP3b).

## Context

After MP3b, ARM EL0 processes can copy memory and use SELF-only typed handles, but there is no
IPC: `endpoint.c`/`endpoint_kern.c` (the pure endpoint core + the frame-neutral kernel bridge
with the global pool `g_kep`, cap mint/resolve/attenuate, and the single-waiter arbitrate CAS)
are not linked on ARM, and the ARM SVC glue (`syscall_arm.c`) handles only SELF + copy_probe.
The shared RR scheduler (`sched_rr.h` states EMPTY/RUNNING/READY/SLEEPING/ZOMBIE) has **no
BLOCKED state**, and ARM's `arm_switch_to_ready` **fatals** when no user slot is READY ("no live
EL0 successor") — there is no idle. On an AArch64 SVC, `ELR_EL1` is saved as the instruction
after the SVC (`svc_addr + 4`).

The x86 side (reference): `je_sched_block_current` rewinds `rip -= JE_INT80_LEN` and parks
BLOCKED_IPC; `je_sched_wake_ipc` does BLOCKED_IPC→READY only; the dispatch prologue (keyed on
`wait_pending`) asserts the saved post-`int` RIP and calls `je_syscall_finish_blocked_recv`,
which branches on the endpoint-arbitrated cause.

## Decision

### D1 — Shared frame-neutral syscall glue via a token-identical case body (Option 1)

The x86 endpoint/handle dispatch bodies (ENDPOINT_CREATE / TRY_SEND / TRY_RECV /
HANDLE_CLOSE/QUERY/RIGHTS/DUP and `finish_blocked_recv`) are factored into a shared source
fragment that BOTH `je_syscall_dispatch` (x86) and `je_arm_el0_svc` (ARM) include, parameterized
by per-arch accessor macros: `JE_G_ARG(i)` (→ `f->rdi…r9` on x86, `args[i]` on ARM),
`JE_G_SET_STATUS` / `JE_G_SET_RESULT` (→ `f->rax`/`f->rdx` on x86, `*out_status`/`*out_result`
on ARM), `JE_G_RETURN`, and `JE_G_BLOCK(rec, side, deadline)` (→ `je_sched_block_current(f,…)`
on x86, the ARM park on ARM). The decode is done once by the shared pure `je_syscall_decode`
(`{RDI,RSI,RDX,R10,R8,R9}` canonical order = `x0..x5` on ARM). The ARM-local SELF-only
`syscall_arm.c` glue is retired in favor of this shared layer; any action ARM does not yet
support stays `UNSUPPORTED`. x86 PROCESS/DEVICE-extension dispatch and the x86 continuation
prologue stay in the x86 wrapper (ARM must not gain x86-only authority).

**Why.** The glue must be shared so the ARM path reuses the exact, reviewed x86 IPC behavior
rather than re-deriving it. The ADR-0016 D2 flex required byte-identical x86 codegen; a design
review measurement showed that moving the cases into a new TU — or even a same-TU static helper
GCC inlines — grows x86 `.text` and changes `je_syscall_dispatch`'s instruction count. A
token-identical `#include`'d case body compiled with x86's existing accessor expansions is the
only shape that leaves x86 object code provably unchanged (verified on a trial ENDPOINT_CREATE
port; the full port is compared across all syscall `TEST_MODE`s 0–15, since the glue contains
mode-11/13/15 code).

**If otherwise (rejected).**
- *Option 2 — waive bit-identity:* edit the x86 dispatch directly and re-verify it end-to-end
  via the x86 QEMU smoke fixtures (it has no hosted unit test, only integration markers). Simpler
  code, but it turns a frozen-codegen guarantee into a smoke-only guarantee on a tested path.
- *Option 3 — ARM-only now, converge later:* keep the ARM glue separate and defer the shared
  extraction a second time. Smallest blast radius, but carries the SELF-only duplication forward
  and leaves the D2 flex uncashed.

**Flex (what reopens D1).** If the macro-templated case body proves too hard to maintain, or a
future x86 change makes the token-identical constraint impractical, revisit with Option 2 under a
fresh owner gate and a recorded smoke-equivalence argument. *(Dated flex note 2026-11-09.)*

### D2 — A `JE_RR_BLOCKED` state in the shared RR scheduler

`JE_RR_BLOCKED` is **appended** to the `sched_rr.h` state enum (never inserted, to keep existing
values stable). A new core `je_rr_block(ctx, slot)` performs RUNNING→BLOCKED; a new
`je_rr_wake(ctx, slot)` performs **BLOCKED→READY only**. `je_rr_pick` continues to select only
READY; `je_rr_switch` continues to reject non-READY targets; `je_rr_tick` stays SLEEPING-only.
BLOCKED is distinct from SLEEPING (SLEEPING wakes on a tick count; BLOCKED wakes on an endpoint
event or an IPC deadline). The IPC deadline sweep lives in `je_arm_sched_irq` — **not**
`je_rr_tick` — because releasing a blocked waiter requires the endpoint single-arbitrate CAS
(D7) to stamp the terminal cause first, and `sched_rr.c` is the pure arch-neutral core.

**Why.** A blocking receive needs a scheduler state that `pick` never selects and that only an
endpoint wake (or a deadline) can clear, without disturbing the tested SLEEPING/READY semantics.
Appending the enum value and adding a BLOCKED-only wake transition was prototyped in review:
`test_sched_rr` and all 13 sched mutants stayed green.

**Flex.** SMP or multiple threads per process would require per-CPU block/wake and revisiting the
single-waiter assumption. *(Dated flex note 2026-11-09.)*

### D3 — ARM park / restart and a reserved slot-0 idle

**Park** (on a BLOCK action in `je_arm_el0_svc`): `record_current_frame`; save the continuation
(the six args, the owned `rec`/`side`, and `post_svc_elr = tf->elr`); require
`post_svc_elr >= JE_VM_ARENA_BASE + 4` (else take the normal death/error path, so a block-SVC at
the very first arena instruction cannot rewind below the arena); rewind `tf->elr -= 4`
(`JE_SVC_LEN`; the SVC re-executes on resume and the rewound ELR is still 4-aligned and in-arena,
so `frame_shape_class` accepts it); `je_rr_block` the slot; `return arm_switch_to_ready()`. The
BLOCK branch is taken **before** the `x0/x1` status/result write-back, so a parked frame is never
handed a stale OK.

**Restart** (a continuation prologue in `je_arm_el0_svc`, before the decode): if this slot has a
pending IPC continuation, after the ESR check assert `tf->elr == post_svc_elr` (the re-trapped
SVC saves `ELR = svc_addr + 4 = post_svc_elr`, NOT `post_svc_elr − 4`), plus a process-identity
check; call the shared `finish_blocked_recv` into `x0/x1`; clear pending and **return
immediately** (never fall through into ordinary decode, or the syscall runs twice).

**Idle** (reserved slot 0): the RR core already reserves slot 0 with no core-created frame. ARM
builds a real slot-0 idle frame and switches to it when no user slot is READY; the exception
return clears the `je_arm_in_handler` guard, and slot-0 code executes `WFI` with interrupts
unmasked. A `WFI` is **never** executed inside `arm_switch_to_ready`/the handler (the guard is
still set there — it would wedge). `je_arm_sched_irq` must, from the idle context, tick, sweep
IPC deadlines, reap, and switch to a newly-READY user. The kernel stays **FATAL only when no slot
is BLOCKED or SLEEPING** (nothing can ever wake); an all-blocked kernel idles and waits.

**Why.** The ELR rewind is the architectural restart with no trap-entry assembly. The review
measured that the first-draft `post_svc_elr − 4` assertion would FATAL every legitimate wake. A
real ERET-reached idle is required because a bare `WFI` inside the handler runs with the
double-fault guard set and with interrupts masked.

**Flex.** Multiple threads per process invalidates the one-frame-per-slot continuation and the
restart prerequisite; revisit then. *(Dated flex note 2026-11-09.)*

### D4 — Split `JE_ARM_NO_DEVICE_HOOKS`; link endpoint core on ARM

`endpoint.c` and `endpoint_kern.c` are linked on ARM. ARM supplies the required shims
(`je_arch_irq_save/restore` over DAIF, `je_sched_wake_ipc`→`je_rr_wake`, `je_sched_current_slot`,
`je_sched_current_proc_id`, `je_sched_ticks`) and installs the endpoint bridge
(`je_endpoint_kern_install` + `je_endpoint_kern_set_proc_pool`) before any endpoint create/seed.
Device operations on ARM are **fail-closed stubs** (device is M4/x86 scope). Critically, the
`JE_ARM_NO_DEVICE_HOOKS` gate is split so that `je_lc_release_waiter` is **always** compiled:
today it is stubbed out on ARM, which would leak a dead blocked process's endpoint waiter
(inflight ref never released → the record is never freed, breaking ADR 0009 D9) and leave a stale
`waiter_id` a slot-reusing process could match.

**Why.** The endpoint core is needed for IPC; device is not in MP3c scope and its real
implementation needs x86 PIC/TSS hooks. But the waiter-release path is part of the IPC contract,
not the device subsystem, and must run on every death.

**Flex.** Real ARM device capabilities arrive in a later milestone; the fail-closed stub is
replaced then. *(Dated flex note 2026-11-09.)*

## Settled by the design review (not separately gated)

- **Peer-death wake on the IRQ path.** Reaping happens today only at SVC entry; if a sender dies
  while the receiver is blocked and nothing issues an SVC, the receiver would wrongly time out
  instead of getting PEER_CLOSED. Peer-death reaping moves to the IRQ path.
- **Drop the args[6] continuation-storage change (former D4).** `finish_blocked_recv` needs only
  `(rec, side, ubuf, cap, hbuf, hcap)`, which is already the x86 signature; changing the x86
  `wait_*` storage would alter x86 codegen for no benefit.
- **An explicit one-thread-per-process check on ARM.** Today the second-thread refusal holds only
  incidentally (via the kernel-stack double-map); add an explicit check mirroring x86.
- **Proof shape.** An EL0 two-process IPC test that uses the **seeded bootstrap channel** to hand
  the receiver its first endpoint cap (a created pair cannot otherwise reach a second process),
  exercising: byte+cap send/recv with destination-side mint; a blocking recv that is WOKEN by a
  later send across the timer-live scheduler (proving the ELR rewind + continuation); a TIMED_OUT
  recv; a PEER_CLOSED wake (drain-first); the dead-cap NULL marker (handle 0) with a live sibling
  still minting; death-releases-the-waiter; a commit-mismatch stays fatal; and idle-when-all-
  blocked does not panic. Each guard is proven load-bearing by a one-build-per-stage negative and
  a mutant, through the **real** path (not a test-inject seam) — the recurring lesson from
  MP3a-2/MP3b: a negative stage you never made fail is not a proof.

## Honest limits

QEMU TCG does not model stale TLB/cache; cortex-a72 has no PAN/UAO/HAFDBS; physical-ARM and SMP
not exercised. The restart prerequisites (immutable EL0 text, one thread per process,
AArch64-only EL0 with ESR.IL=1) hold on ARM and are re-gated if any changes. This is AI-assisted
work, not independent human qualification.
