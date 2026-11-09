# MP3c design brief — ARM capability IPC via ENDPOINT objects + blocking-recv restart

JeiOS freestanding kernel, main HEAD 52adf4f (MP3a+MP3b merged). MP3c = the aarch64 analogue
of x86 "M3c": capability IPC over ENDPOINT objects (create/send/recv, cap transfer by
destination-side mint/attenuate) + a blocking receive that PARKS a process and RESTARTS the
syscall on wake. This is a DESIGN REVIEW before any code. Attack the design; find the hole.
Contract to preserve = ADR 0009 (D7 one-waiter/one-terminal-transition, D8 transactional
mint/attenuate-not-raw-copy, D9 bounded reclamation, A5 dead-cap NULL marker + discard order,
A6-5 owned-ref-no-re-resolve, A6-CLARIFY-1 restart prerequisites). ADR 0008 = typed handles.

## What exists (verified by code read)
- **endpoint.c + endpoint_kern.c are ALREADY frame-neutral** `(je_vm*, uint32_t index, ...)` —
  pool g_kep, create, try_send[_caps], two-phase recv (peek→stage mints→copy out→infallible
  commit), waiter register/arbitrate (single terminal CAS), cap resolve/mint/attenuate, wake.
  Nothing in them touches an x86 frame. They are NOT linked on ARM today.
- **x86 blocking recv (sched.c):** state enum has BLOCKED_IPC; `je_sched_block_current(f,rec,side,
  deadline)` saves args + rec/side, sets `wait_post_int_rip=f->rip; f->rip -= JE_INT80_LEN(2)`,
  state=BLOCKED_IPC, picks next. `je_sched_wake_ipc(slot)` = BLOCKED_IPC->READY only. Timer sweep
  arbitrates TIMED_OUT past deadline. On resume the thread re-executes int0x80; the dispatch
  prologue (keyed on wait_pending, not RAX) asserts rip==saved, calls finish_blocked_recv (branches
  on the arbitrated cause: WOKEN=recv_peek_owned+stage mints+copy out+commit; TIMED_OUT/PEER_CLOSED/
  CANCELLED=release+status), then wait_finish.
- **x86 endpoint dispatch glue (syscall.c, #ifndef JE_SYSCALL_HOSTED_TEST):** ENDPOINT_CREATE
  (packs h0|h1<<32 into rdx), TRY_SEND (preflight→copy cap array→resolve per cap→copy bytes→
  send_caps), TRY_RECV (timeout? probe→register→block : peek→stage→copy out→commit), typed
  HANDLE_CLOSE/QUERY/RIGHTS/DUP (endpoint uses kern_close/dup). Only frame coupling = reading
  f->regs, writing f->rax/rdx, and the block-return `je_sched_block_current(f,...)`.
- **ARM SVC (sched_arm.c je_arm_el0_svc):** reaps, classifies frame, checks ESR EC=0x15/IL=1/imm=0,
  x0-x5->args, x8->op, calls je_arm_syscall_dispatch (SELF-only today), writes x0=status/x1=result,
  then EXIT->death / YIELD->record+switch / else record+no_switch. `record_current_frame` fatal if
  current->frame unconsumed; `arm_switch_to_ready` flips current RUNNING->READY then picks a READY
  (pick==0 is FATAL "no live EL0 successor" — NO idle). RR states (sched_rr.h, shared+tested by
  test_sched_rr): EMPTY/RUNNING/READY/SLEEPING/ZOMBIE — NO BLOCKED. On SVC, ELR_EL1 = svc+4.

## Proposed MP3c design (critique every decision; give OK/FIX/ESCALATE each)

### D1 — the frame-neutral endpoint/handle glue extraction (cashes ADR-0016 D2 flex; TESTED x86)
Move the bodies of the x86 ENDPOINT_CREATE/TRY_SEND/TRY_RECV/HANDLE_CLOSE/QUERY/RIGHTS/DUP cases
+ finish_blocked_recv out of syscall.c's x86 dispatch into a NEW core TU (e.g.
kernel/core/syscall_glue.c) compiled for BOTH arches (no HOSTED_TEST exclusion). Shape:
`je_syscall_action je_syscall_glue(op, const uint64_t args[6], je_abi_status *st, uint64_t *res,
je_syscall_block *out_block)` where out_block{rec,side,deadline} is set when it returns a new
JE_SYSCALL_BLOCK action; `je_syscall_finish_blocked_recv(const uint64_t saved_args[6], rec, side,
st, res)`. The arch shims (x86 je_syscall_dispatch, ARM je_arm_el0_svc) ONLY marshal registers and
own the park/restart. The ARM-local SELF-only syscall_arm.c is REPLACED by calling the shared glue.
x86 must be byte-identical (objdump jeios.elf). QUESTION: is a single shared TU clean, or does the
x86 per-case decode-status handling diverge from the ARM `if(*st!=OK) return` shortcut enough to
force a reconciliation that changes x86 codegen? What's the minimal shape that keeps x86 bit-identical?

### D2 — a BLOCKED state in the shared RR scheduler (TESTED sched_rr.c)
Add JE_RR_BLOCKED to sched_rr.h's state enum; je_rr_pick must NOT select it; je_rr_switch must not
flip BLOCKED->READY; a new je_rr_wake(slot) does BLOCKED->READY only; je_rr_tick stays SLEEPING-only
(IPC deadline is swept separately). QUESTION: does adding an enum value + the wake transition break
any test_sched_rr invariant or any switch/pick assumption? Is BLOCKED distinct from SLEEPING (yes —
SLEEPING wakes on a tick count, BLOCKED wakes on an endpoint event or a deadline)? Should the ARM
deadline sweep live in je_arm_sched_irq or je_rr_tick?

### D3 — ARM park + restart (ELR -= 4; needs idle-when-all-blocked)
On a BLOCK action je_arm_el0_svc: record_current_frame(cur,tf); save the continuation (args x0-x5,
rec/side, post_svc_elr=tf->elr); rewind tf->elr -= 4 (JE_SVC_LEN; the SVC re-executes on resume,
still 4-aligned + in-arena so frame_shape_class accepts it — NO trap-entry asm needed, unlike MP3b);
set slot BLOCKED; return arm_switch_to_ready(). arm_switch_to_ready flips only RUNNING->READY so a
BLOCKED slot set before the call is safe. On resume, a continuation prologue in je_arm_el0_svc BEFORE
the decode: check pending for this slot, assert tf->elr == post_svc_elr-4 (the rewound addr the SVC
re-entered from), finish_blocked_recv into x0/x1, clear pending. IDLE: today arm_switch_to_ready
FATALs when pick==0; MP3c must add a real idle (wfi loop with the timer live) so a kernel where every
user slot is BLOCKED waits for a wake/deadline instead of panicking. QUESTION: (a) is ELR-=4 the
correct rewind (confirm SVC preferred-return = svc_addr+4, so the faulting/again-executed SVC is at
elr-4)? (b) can a timer IRQ land between park and the next pick and corrupt the continuation? (c) the
idle: where does it run (a reserved idle slot? a bare wfi in switch when pick==0 AND some slot is
BLOCKED, vs still-fatal when ALL slots EMPTY)? (d) does record_current_frame's "fatal if unconsumed"
interact badly with a BLOCKED frame that is parked then resumed?

### D4 — x86 continuation stores args[6] not named registers (TESTED sched.c)
To share finish_blocked_recv, the saved continuation becomes `uint64_t args[6]` instead of
wait_rdi/rsi/rdx/r8/r9. x86 block_current fills args from f->regs; ARM from x0-x5. QUESTION: does this
change any x86 test or the kill path that reads frame->rip? Keep x86 behavior identical.

### D5 — link endpoint.c/endpoint_kern.c/device on ARM
mk/aarch64.mk must add endpoint.o + endpoint_kern.o (+ their deps). endpoint_kern.c references
je_kernel_device / je_device_* and je_sched_wake_ipc/current_slot/ticks/current_proc_id. ARM must
supply those (or a fail-closed device stub, cf JE_ARM_NO_DEVICE_HOOKS). lifecycle.c l.89-95
je_lc_release_waiter is #if JE_ARM_NO_DEVICE_HOOKS'd out — death must release the IPC waiter on ARM
or a dead blocked process leaks a waiter/inflight ref. QUESTION: wire real device on ARM, or a
fail-closed stub so endpoint cap-transfer works but device-cap ops are UNSUPPORTED on ARM for MP3c?
Recommend the stub (device is M4/x86 scope). Confirm the death-release path is restored on ARM.

### D6 — the proof (ARM EL0 IPC test + negative stages, one build per fatal-expected stage)
Two EL0 processes: A creates an endpoint pair, sends bytes+a cap to B; B recv (non-blocking) gets the
bytes + mints the transferred cap + uses it; then B blocks on recv, A sends, the timer-live scheduler
wakes B and it RESTARTS the SVC and receives (prove ELR rewind + continuation); a deadline-timeout recv
(B blocks, nobody sends, deadline fires -> TIMED_OUT); peer-close wake; dead-cap NULL marker (send a
cap, close its source, recv sees handle 0 while a sibling cap still mints); death-releases-waiter (a
blocked B is killed, the waiter/inflight ref is released, no leak). NEGATIVE one-build-per-stage proofs
(fatal-expected, ARM_MMU_FAULT style): attenuation bypass (send a right not held -> DENIED not granted);
raw-handle-copy instead of mint (must be impossible by construction); re-resolve-on-resume (A6-5 — must
use the owned ref, not re-resolve); commit-mismatch fatal; idle-when-all-blocked does NOT panic. Each
guard proven load-bearing by injection (MP3a-2/MP3b lesson: a negative stage you never made fail is not
a proof; prove it through the REAL path, not a test-inject seam).

## Your task (adversarial design review)
For EACH D1-D6: correct? minimal? does it preserve the frozen ADR-0009 contract AND the tested x86
sched/endpoint/handle behavior? Specifically attack: (1) the ELR-=4 restart — any way it re-enters
wrong, double-executes, or lets a timer IRQ corrupt the parked continuation; (2) the BLOCKED state +
idle — can the scheduler deadlock, busy-spin, or panic when all user slots block; can a wake race the
park; (3) the glue extraction — anything that changes x86 codegen or diverges the two arches' decode
semantics; (4) cap-transfer safety on ARM — can a blocked-then-resumed recv mint a stale/escalated
cap, double-release, or leak on death; (5) the A6-CLARIFY-1 restart prerequisites (immutable user
text, one thread per process) — are they still true on ARM. Cite file:line. Give OK/FIX/ESCALATE per
decision, with the exact concern. D1/D2/D4 are tested-path changes and D3's idle touches the
scheduler — these are owner-gated; say so.

## Slack (owner directive — pass-through)
Post your own substantive design findings directly to the agent group channel:
curl -s -X POST "SLACK_WEBHOOK_URL_INJECTED_AT_DISPATCH" -d '{"Text":"[codex|claude] <finding>"}'
