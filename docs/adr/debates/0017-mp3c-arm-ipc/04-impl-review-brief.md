# MP3c adversarial review (empirical, with injection) — the load-bearing gate

You are an ADVERSARIAL reviewer. TRY TO BREAK MP3c. Do NOT rubber-stamp. JeiOS freestanding
ARM64 kernel; code in git worktree `/tmp/wt-mp3c` (branch `wip/mp3c`, HEAD fa7d1d6). You may
build, boot in QEMU, read code, and INJECT faults (edit→build→boot→REVERT; leave the tree clean,
or work in scratch copies under /tmp). This is the aarch64 analogue of x86 M3c: capability IPC
over ENDPOINT objects + a blocking receive that parks a process and restarts the SVC on wake.
Design = docs/adr/0017-mp3c-aarch64-capability-ipc.md (authoritative). Contract to preserve =
ADR 0009 (D7 one-waiter/one-terminal-transition, D8 transactional mint/attenuate, D9 bounded
reclamation, A5 dead-cap NULL marker, A6-5 owned-ref/no-re-resolve, A6-CLARIFY-1 restart prereqs).

## What MP3c did (owner-approved)
- **Shared syscall glue** (kernel/core/syscall_glue.inc) via a token-identical #include'd case body
  + per-arch accessor macros (JE_G_ARG/SET_STATUS/SET_RESULT/RETURN/BLOCK). x86 je_syscall_dispatch
  and ARM je_arm_el0_svc both include it. x86 object code is claimed BYTE-IDENTICAL (lead verified
  syscall.o .text = 11055 bytes, unchanged). The ARM-local SELF-only glue was replaced.
- **JE_RR_BLOCKED** appended to sched_rr.h; core je_rr_block (RUNNING->BLOCKED) + je_rr_wake
  (BLOCKED->READY only); pick/switch/tick unchanged in who they select.
- **ARM park/restart** (sched_arm.c): on a BLOCK action, record_current_frame, save continuation
  {args x0-x5, owned rec/side, post_svc_elr}, require post_svc_elr>=ARENA_BASE+4, rewind
  tf->elr-=4, je_rr_block, arm_switch_to_ready. On resume, a continuation prologue asserts
  tf->elr==post_svc_elr (NOT -4), process-identity, calls finish_blocked_recv into x0/x1, returns
  immediately. Reserved slot-0 idle (ERET clears je_arm_in_handler, then WFI IRQs-unmasked);
  je_arm_sched_irq ticks/sweeps-IPC-deadlines/reaps/switches from idle. FATAL only when no slot is
  BLOCKED or SLEEPING. Peer-death reaping is strictly IRQ-path (SVC fallback removed, G7 option a).
- **Endpoint core linked on ARM** (endpoint.c + endpoint_kern.c) + ARM shims (DAIF irq_save/restore,
  je_sched_wake_ipc->je_rr_wake, current_slot/proc_id/ticks). Device = fail-closed stubs. The
  JE_ARM_NO_DEVICE_HOOKS gate was SPLIT so je_lc_release_waiter is ALWAYS compiled (device stays
  stubbed) — else a dead blocked process leaks its endpoint waiter (ADR 0009 D9).
- **Proof** (el0_test.c): positive boot emits trusted markers EL0 IPC {WOKEN_RESTART, TIMED_OUT,
  TIMER_WAKE, PEER_CLOSED blocked-before-ready=1, DEADCAP_NULL_MARKER live_sibling=ok,
  DEATH_RELEASED_WAITER rec_freed=1 no_stale_id=1, ALL_BLOCKED_IDLE woke=1}; one-build-per-stage
  negatives (ARM_IPC_FAULT=<name>) + MP3C mutants.

## Build/run (from /tmp/wt-mp3c)
- `make aarch64 ARM_EL0_TEST=1`; raw boot (virt-8.2,cortex-a72,-smp 1,256M,-serial file) must reach
  `EL0 proof PASS reviewed-cache-tlb=not-observed` with the 7 IPC markers.
- Negative: `make aarch64 ARM_IPC_FAULT=<name>` -> build/aarch64-ipcfault-<name>/Image must FATAL.
- `make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=70`; `make check`; mutants via
  `python3 tools/mutants.py --id <ID>` (run the full `make mutants` SOLO - do not run it concurrently
  with a smoke build in the same tree, it mutates in place).

## ATTACK THIS — the questions that matter most
1. **The restart mechanism.** Can the ELR rewind + continuation ever double-execute the syscall,
   resume at the wrong PC, or skip the continuation? Inject: drop the rewind; use elr==post_svc_elr-4
   (must FATAL every wake - lead claims the -4 was the original bug); a timer IRQ landing between park
   and pick; a second (non-IPC) SVC after a resume; a block-SVC at ARENA_BASE (must take the
   error/death path, not rewind below the arena). Confirm each.
2. **The frozen scheduler + idle.** Can the kernel deadlock, busy-spin, panic wrongly, or wake a
   BLOCKED slot that shouldn't be? Inject: je_rr_wake transitioning a SLEEPING slot; pick selecting a
   BLOCKED slot; the idle NOT handing off when a user becomes READY; both-slots-blocked with a
   deadline (must idle+wake, not panic); nothing-can-wake (must FATAL). Is the slot-0 idle WFI really
   reached via ERET (guard cleared) and never inside the handler?
3. **Cap transfer safety on ARM (ADR 0009 D8/A5/A6-5).** Can a blocked-then-resumed recv mint a
   STALE or ESCALATED cap, double-release, re-resolve the handle (A6-5 violation), or leak on death?
   The dead-cap must be the NULL marker (handle 0) while a live sibling mints. Attenuation at send
   must deny a right not held. The two-phase recv (peek->stage->copy->infallible commit) must roll
   back mints on a copy fault and leave the message queued.
4. **death-releases-waiter (D9) — the gate split.** This is load-bearing: reverting the
   je_lc_release_waiter gate must LEAK the waiter (record never freed / stale waiter_id matched). The
   lead saw ARM-MP3C-WAITER-RELEASE-DEATH kill. Confirm by your own injection that without it a dead
   blocked receiver leaks, and that the positive DEATH_RELEASED_WAITER marker is decided from trusted
   endpoint state (rec freed + no stale id), not a user bit.
5. **x86 byte-identity / no regression.** Confirm the x86 jeios.elf (or at least syscall.o .text) is
   byte-identical to base 52adf4f across the syscall TEST_MODEs, and that endpoint.c/endpoint_kern.c/
   sched_rr.c behavior for x86 is unchanged (x86 make check + any hosted endpoint/sched tests pass).
6. **Vacuity sweep (recurring lesson).** For EACH of the 7 positive markers and each negative stage/
   mutant: is it load-bearing? Remove the guard it checks and confirm the marker DISAPPEARS or the
   stage stops FATAL-ing. Flag any marker that prints without its property holding. Note: the lead
   found ARM-USER-DCC-ISS-GUARD reported BLOCKED once in the aggregate but KILLS in isolation (a
   known mutant-aggregate flake) — confirm or refute that it's benign. The 5 SKIPPED mutants are
   declared gaps (2 legacy witnesses disabled after IRQ-only reaping; 3 MP3C follow-ups) — check the
   disabled_reasons are honest, not hiding a real regression.

## Output
VERDICT: SHIP or FIX-FIRST. If FIX-FIRST, each item with the exact injection + observed-vs-expected
serial. Quantify effort (boots, injections). A clean "tried X/Y/Z, could not break it" counts only
if you actually ran the injections. AI-assisted review, not human qualification.

## Slack (owner directive)
Post substantive findings directly:
curl -s -X POST "SLACK_WEBHOOK_URL_INJECTED_AT_DISPATCH" -d '{"Text":"[claude] <finding>"}'
