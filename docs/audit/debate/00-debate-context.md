# JeiOS ecosystem audit — debate context

Two AI lineages each produced an independent audit of the JeiOS project (a
fresh C11 microkernel + ecosystem, built by AI agents under a human owner,
honesty/evidence discipline). Those two reviews AGREED on the factual findings
(status drift across 5 docs; mutation testing not in CI; no real-ARM-hardware
tier; no SMP gate / no lock primitive anywhere; x86 at M4 vs ARM at MP2;
planning-doc backlog outpacing execution). This debate is NOT to re-establish
those facts — they are verified in-tree. It is to pressure-test the JUDGMENT
CALLS where reasonable engineers could disagree, so the owner + planning agent
get the tradeoffs, not just a consensus list.

You (the two peers) will each take a POSITION and then respond to each other.
Push back hard; concede where the other is right; surface tradeoffs the
single reviews glossed. Keep it concrete to THIS project's state.

## Verified current state (do not relitigate)
- Completed (AMD64): M0-M4 (boot, exceptions, alloc, timer/IRQ, preemptive
  kernel threads, userspace/handles/IPC, service supervisor + devices).
- Completed (ARM64): MP1 (entry/console/vectors/DTB), MP2 (MMU, memory, GICv2,
  CNTP timer, returning IRQ path). MP2 scheduling = ADR 0013 ACCEPTED, no code.
- No userspace/IPC/scheduler on ARM yet. No storage, network, UI on either arch.
- Shared "core" is increasingly x86-shaped (kernel/core/main.c ~3900 lines is
  really the x86 harness; arch.h exports PC-isms). sched.c is tested x86 code;
  ADR 0013 adds a neutral sched_rr.c for ARM, leaving sched.c untouched (=> two
  schedulers until a later migration).
- No lock primitive anywhere; SMP is a disclaimer, not a gate. Every real target
  (desktop, Pixel 9) is multicore.
- Process: design-review -> cross-lineage review -> mutation testing (manual,
  uncommitted) catches real security holes. All 69 PRs merged, AI-review-only.
- Broad parked backlog: clustering, Android compat, GPU/NPU, RISC-V, LDAP,
  POSIX, F2FS, Bash/Zsh shells.

## Debate questions (take opposing/nuanced positions, then cross-examine)

Q1. SEQUENCING: Should ARM MP3 (userspace/IPC/scheduler parity) be forced to
    close BEFORE any x86 gate past M5a (the "parity rule"), accepting slower
    x86 progress to keep the arches converged and exercise the ABI on a 2nd ISA?
    OR is it better to let x86 race ahead to a compelling M5-M7 demo (storage,
    shell, UI) to prove the OS vision, treating ARM parity as a later catch-up?
    Argue both; where's the real breakeven?

Q2. SMP: The audit says add an SMP gate before M6 and write an SMP-unsafe-globals
    inventory before M5a adds more globals. But SMP is a huge multiplier on
    complexity (locking, per-CPU, memory ordering, TLB shootdown) and the kernel
    is single-core-correct today. Is investing in SMP-readiness NOW (conventions,
    inventory, maybe a lock primitive) worth slowing the single-core critical
    path? Or is "stay rigorously single-core, add SMP as its own milestone much
    later" the disciplined choice? What is the CHEAPEST SMP-insurance that
    doesn't derail current work?

Q3. THE NEUTRAL-CORE / sched.c MIGRATION: ADR 0013 introduces sched_rr.c for ARM
    and defers migrating tested x86 sched.c onto it. Is "two schedulers
    temporarily" an acceptable, well-contained bridge, or a debt trap that will
    never be paid down (and will drift)? Should the migration be forced BEFORE
    MP3 (blocking ARM userspace on an x86 refactor), or is that over-coupling two
    risky changes? What's the least-risk ordering?

Q4. MUTATION TESTING IN CI: Both reviews want mutation testing promoted from
    manual to a committed CI gate. But mutation testing is slow, can be flaky,
    and a naive kill-threshold gate can become noise that agents learn to game.
    Is a CI mutation GATE actually the right mechanism, or is there a better way
    to institutionalize "a green test that passes though the property is broken"
    resistance (e.g. mandatory committed negative/denial tests per invariant,
    periodic not per-PR)? Design the mechanism that maximizes real assurance per
    unit of CI time/agent effort.

Q5. HONESTY AT SCALE vs VELOCITY: The all-AI-review, all-merged, no-human-gate
    model is fast and has caught real bugs. At what exact point does
    AI-review-only stop being sufficient — i.e. what is the FIRST claim/gate that
    MUST NOT be made without a named human qualification, and how should the
    project make that boundary concrete without killing velocity on everything
    below it?

Format: Round 1 each peer stakes positions on all five with reasons. Round 2
each peer responds to the other's Round 1 — concede, rebut, or refine — and lands
a recommendation per question. Be specific and honest; no false consensus.
