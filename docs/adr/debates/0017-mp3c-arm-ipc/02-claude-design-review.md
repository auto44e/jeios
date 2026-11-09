**Verdict: FIX FIRST.** The design has 3 blockers and 1 escalation, and should not go to code until they're resolved. D1, D2, the D3 idle, and the D5 `lifecycle.c` change all touch tested code, so they're owner-gated. I recommend dropping D4.

All experiments ran in `/tmp/mp3c-rev` scratch copies of 52adf4f, with QEMU TCG plus hosted tests. The worktree is untouched. I posted 7 findings to Slack under `[claude]`, and all 7 posts returned ok. This is AI-assisted review, not human qualification.

## Per decision

| | Verdict | Concern |
|---|---|---|
| **D1** glue extraction | **ESCALATE** | The "byte-identical x86 objdump" criterion from ADR 0016's flex note can't be met by the proposed call-shaped glue. Measured on x86 TEST_MODE=0: moving just ENDPOINT_CREATE into a new TU grew `.text` from 89259 to 89355 bytes. A same-TU static helper (inlined by GCC) still changed `je_syscall_dispatch` (1499→1502 instructions). A comment-only edit, as a control, stayed identical. **The minimal shape that keeps x86 bit-identical** is a token-identical `#include`'d case body with per-arch accessor macros (`JE_G_ARG(i)`→`f->rdi`/`args[i]`, `JE_G_SET`, `JE_G_RETURN`, `JE_G_BLOCK`). I tried it on ENDPOINT_CREATE and `.text` and objdump were identical. Owner chooses: (i) the template, compared across all TEST_MODEs 0–15 (the glue contains mode 11/13/15 code); (ii) waive bit-identity — x86 dispatch has QEMU markers only, no hosted test; or (iii) ARM-only now, converge later. The decode-status question is a non-issue: decode returns a non-RETURN action only with status OK (`syscall.c:45-183`), so `syscall_arm.c:76-77` is dead code and nothing forces an x86 change. The real behavior change is on ARM: the MP3b SELF-only ops become typed, so re-run the MP3b proofs and keep every unported action UNSUPPORTED. |
| **D2** RR BLOCKED | **OK with one addition** | Prototyped: `JE_RR_BLOCKED` appended to the end of the enum, plus `je_rr_block` and a BLOCKED→READY-only `je_rr_wake`. The existing test_sched_rr suite and the new cases pass. ARM_SCHED_TEST and EL0 smoke pass. All 13 sched mutants are killed at the same kill points as base. Add **`je_rr_block` in the core**; the brief's "set slot BLOCKED" has the seam write the state directly, bypassing the tested core. BLOCKED is distinct from SLEEPING. Put the deadline sweep in `je_arm_sched_irq`, not `je_rr_tick`: it needs the endpoint arbitrate CAS (D7) and `sched_rr.c` is the pure core. Order it reap → sweep → pick so PEER_CLOSED beats TIMED_OUT within one tick. |
| **D3** park/restart/idle | **FIX** | (a) ELR−4 is the right rewind: EL0 is AArch64-only (`sched_arm.c:349,376`) and ESR.IL=1 is checked (`:1114`). **But the proposed assertion is wrong**: the re-trapped SVC's ELR equals post_svc_elr in 2 of 2 runs and never post_svc_elr−4, so the literal check would FATAL on every wake. Assert `== post_svc_elr` (as x86 does at `syscall.c:765`), after the ESR check, plus a process-identity check. (b) No IRQ can land between park and pick, because the SVC path stays I-masked. An in-handler `daifclr; wfi` gives "FATAL double fault" (measured). (c) The idle has to be **slot 0**, which the core already reserves (`sched_rr.h:106,146`). (d) `record_current_frame` is fine: the rewound frame survived a switch with its cookie validated. |
| **D4** args[6] | **FIX: drop it** | Sharing finish only needs (rec, side, ubuf, cap, hbuf, hcap), which is already the x86 signature (`syscall.c:456`). Changing `je_sched_wait_pending` (`sched.h:99`) alters x86 codegen and defeats D1 for no benefit. The kill path (`sched.c:714,792`, which reads `frame->rip`) isn't affected either way. |
| **D5** link endpoint on ARM | **FIX** | A device stub is correct: the real `device.c` needs x86 PIC/TSS-COM2 hooks. But see B2 below on the waiter release. |
| **D6** proofs | **FIX** | Several oracles can't kill their mutants (S3). |

## Findings

**BLOCKERS**
- **B1 – the D3 resume check (post−4) is wrong.** Every legitimate wake would halt the kernel. Measured in the restart experiment.
- **B2 – death doesn't release the waiter on ARM.** In the ARM EL0 build `je_lc_release_waiter` compiles to a bare `ret` (`lifecycle.c:89-95`, under `JE_ARM_NO_DEVICE_HOOKS`).
  - Every death leaks the waiter's inflight ref (`endpoint.c:297-299`), so the record is never freed, which breaks D9.
  - It also leaves a stale `waiter_id` that a later process reusing the slot can match.
  - Fix: split the gate so waiter release is unconditional, and add a hosted lifecycle-order test built with the ARM flag.
- **B3 – the IRQ path never leaves idle.** The non-user branch (`sched_arm.c:1235-1237`) never ticks or switches. With a slot-0 idle frame but no IRQ change, the system **hung** (measured), so a TIMED_OUT stage would never complete.
  - With a slot-0 branch that ticks, sweeps, reaps and switches when something is READY: woke at tick+5, proof PASS.
  - Keep FATAL when no BLOCKED or SLEEPING slot exists. That's a policy vote.
  - There's no deadlock risk: every park has a deadline between 1 and `JE_IPC_MAX_WAIT`.

**SHOULD-FIX**
- **S1 – peer-death wake waits for an SVC.** Reaping happens only at SVC entry (`sched_arm.c:1101`). If A dies while B is blocked and nothing makes an SVC, B gets TIMED_OUT instead of PEER_CLOSED, which ADR 0009's peer-wake amendment rejects. Moving reaping to the IRQ path passed the full EL0 proof: 11 IRQ-context reaps, one of them in idle.
- **S2 – the D1 escalation above.**
- **S3 – D6 oracle problems:**
  - **Handle numbers collide.** Handles are allocated lowest-index-first with generation 1 (`handle.c:83`), so A's h0 and B's first mint are both 0x00010000. A raw-copy mutant survives unless B uses the received handle end-to-end.
  - **The re-resolve negative can't fail.** With one thread per process, B's handle table can't change while parked, so re-resolving gives the same answer. Either poison the parked x0 with a **non-OK** value (never 0) or don't claim the stage as proven.
  - **"Blocked B is killed" has no real path on ARM**, since there's no PROC_KILL. Label it as a harness trigger of the real death path.
  - **Commit-mismatch is mutation-only.**
  - **Stages to add:** a copy fault inside the resumed continuation, SHORT_BUFFER on resume, woken-then-deadline-passes, timed-out-then-late-send, WAITERS_BUSY, and a drop-the-rewind negative.
- **S4 – one thread per process holds only by accident on ARM.** A second create_user for the same process was refused by the kernel-stack double-map (measured), not by an explicit check. Add one like x86's (`sched.c:155-166`).
- **S5 – ARM wiring:**
  - **Missing arch shims:** `je_arch_irq_save/restore` (DAIF), `je_arch_console_write`, `je_arch_halt` (must mask I).
  - **Missing sched hooks:** `je_sched_current_proc_id`, `je_sched_current_slot`, `je_sched_ticks`, and `je_sched_wake_ipc`→`je_rr_wake`.
  - **Missing calls:** `je_endpoint_kern_install` and `je_endpoint_kern_set_proc_pool`. Without the latter, endpoint creation silently skips the quota charge (`endpoint.c:456`).
  - **Silent fatals:** `smoke_arm.py` matches `b"FATAL"` case-sensitively (`:2647`), but the shared strings say "JeiOS fatal:".
- **S6 – write-back order.** Put the BLOCK branch before the x0/x1 write-back at `sched_arm.c:1137-1138`. Otherwise a lost rewind hands the user OK=0.

**NITs**
- Give `JE_SYSCALL_BLOCK` a value outside the op-number space.
- Record the ARM-only restart prerequisites (AArch64-only EL0, ESR.IL, MDSCR.SS=0) in the ADR.
- Build the idle frame below the live boot SP, not at `__stack_top`.
- Pre-existing, out of scope: on unmodified base, the `live-content-swap` sched mutant **survived 1 of 8 runs** and smoke reported it as a timeout. Mutant regression checks need repeated runs.

## Other attack questions
- **ADR 0009 restart prerequisites on ARM:** immutable user text holds (USER_CODE is EL0 read-only and executable, there's no mmap). One thread per process holds, but only incidentally (S4).
- **Cap transfer:** a resumed recv can't mint an escalated cap, because rights are fixed at send. A stale cap shows up as the NULL marker. Waiter release and commit are idempotent. The one leak is B2.

**Limits:** BLOCKED was simulated with SLEEPING in the idle experiments. The x86 identity check covered mode 0 only. Cache and TLB behavior was reviewed, not observed (QEMU TCG doesn't model it).
