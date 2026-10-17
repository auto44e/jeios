# M3a Retrospective & Qualification Evidence

## 1. Summary

M3a (the userspace boundary) is complete and merged. The tested and merged
commit is `b28f947282ad18520e743d1343b5b4cd14d906f6` (`main`).

The milestone delivered:

- private per-process address spaces;
- ring-3 (CPL3) execution under preemption;
- fault-local process death;
- bounded v0 syscalls (`abi_query` / `yield` / `exit`); and
- recoverable user copies (`copy_from_user` / `copy_to_user` plus an exact-RIP
  exception-fixup net).

M3a is built on the existing M1/M2 foundation (protected memory, preemptive
scheduler, allocator), which remains intact — M2's 4 smoke markers are
unchanged.

## 2. What shipped, by issue/PR

All PRs below are merged to `main`.

| PR | Title |
|----|-------|
| #20 | docs(adr): freeze M3a userspace-boundary contract (ADR 0007) |
| #21 | amend ADR 0007 (pre-#3 corrections) |
| #22 | ADR 0007 two owner-approved #3 decisions (USER_RODATA, concrete kstack run) |
| #23 | docs: record the multi-agent collaboration model |
| #24 | feat(m3a): hosted per-process VM builder + bounded image loader (issue #3) |
| #25 | docs(handoff): QEMU/GRUB boot toolchain verified |
| #26 | feat(m3a): ring-3 GDT descriptors + mode-2 fault-test fix (#4 step 1) |
| #27 | feat(m3a): real per-process page-table backend + CR3, CPL0-verified (#4 step 2) |
| #28 | feat(m3a): CPL3 entry/return — real ring-3 execution (#4 step 3) |
| #29 | feat(m3a): fault-local process death + D6 int 0x80 gate + H1 fix (#4 step 4) |
| #30 | feat(m3a): v0 syscall dispatcher behind int 0x80 (#5 step a) |
| #31 | feat(m3a): recoverable user copies + exception fixup (#5 step b; completes M3a) |

PR-to-issue mapping:

| Issue | Scope | PRs |
|-------|-------|-----|
| #2 | contract / ADR | #20–#22 |
| #3 | private address spaces + bounded images | #24 |
| #4 | CPL3 entry, scheduling, fault-local death | #26–#29 |
| #5 | bounded syscalls + recoverable copies | #30–#31 |

Issues #2, #3, #4, and #5 are closed / Done.

## 3. Qualification evidence (exact, on commit `b28f947`)

All three suites pass with **no timeout-as-success**: every pass asserts an
exact marker or a hosted assertion, and fault modes assert surviving-task
progress.

### 3.1 `make check` (hosted unit tests)

| Check | Result | Asserts |
|-------|--------|---------|
| vm | PASS | validation, admission, audit, permissions, zeroing, rollback, teardown |
| uframe | PASS | bounds, selectors, canonical, rflags sanitize |
| syscall | PASS | abi_query, yield, exit, reserved-arg, full-width op, unsupported |
| ELF checks | PASS | AMD64, Multiboot2, entry, segments, no undefined symbols |
| ABI isolation | PASS | `abi.h` never includes internal `status.h` |
| dev-inject-absent | PASS | fixup-injection symbols absent from the default kernel ELF (via `nm`) |

### 3.2 `make integration` (QEMU, 11 fault modes) — ALL PASS

| Mode(s) | Description |
|---------|-------------|
| 1–9 | M1/M2 + early M3a battery |
| 10 | fault-local D7 #UD — hostile-FP task terminated, kernel alive |
| 11 | exception-fixup recovery (dev-only build `-DJE_DEV_COPY_INJECT`): a copy of a deliberately unmapped page recovered at load **and** store sites; kernel survives |

Crucial negative result: **modes 4 and 5 (kernel-origin #PF) STILL PANIC** — an
unrelated kernel fault is **not** wrongly recovered.

### 3.3 `make smoke` — boot markers (verbatim)

These are the positive execution evidence, quoted exactly as emitted:

```
M3a-s2: process built; audit ok; root distinct
M3a-s2: arena mapped under proc root; data RW; kernel root restored
M3a-s2: process destroyed; page accounting restored
M3a-s3: D7 FPU/SIMD denial enforced (CR0.EM/MP/TS, CR4 clear)
M3a-s3: user thread created; entering ring 3 via timer
M3a-s3: CPL3 reached; survived preemption; heartbeat advanced
M3a-s4: two CPL3 tasks A(fault)+B(loop) created; distinct roots
M3a-s4: A terminated by user #PF (vector 14, cs=0x33); B still scheduled
M3a-s4: B advanced AFTER A death (heartbeat+dispatch+preempt up)
M3a-s4: dead process A reaped while peer B runs
M3a-s4: bad-RSP (H1) user task terminated (BAD_RETURN), not kernel; B alive
M3a-s4: hostile-FP user task terminated (#UD FAULT); B alive
M3a-s5a: user exit(42) -> clean EXIT (code 42) via int 0x80; B alive
M3a-s5a: abi_query returned OK+version; yield returned OK; B alive
M3a-s5a: unknown op -> UNSUPPORTED, task survived; B alive
M3a-s5a: bad frame on syscall return -> BAD_RETURN death (validated); B alive
M3a-s5b: copy_probe valid+zero OK, null/kernel/len>256 INVALID, bytes moved; B alive
M3a-s5b: exception-fixup recovered load+store copy-site #PF (dev inject); kernel alive
M3a-s4: peer B torn down; page accounting restored
```

### 3.4 CI smoke profiles

3 CI smoke profiles pass:

| Profile | Memory | NX | Note |
|---------|--------|----|------|
| qemu64,-nx | default | absent | cleanly skips the NX-gated M3a blocks |
| qemu64,-nx | 16M | absent | cleanly skips the NX-gated M3a blocks |
| qemu64 | 1536M | present | full M3a battery |

## 4. What went well (retro)

- **Small, independently reviewable PRs.** Decomposition into ~8 small PRs,
  each with its own QEMU evidence, kept risk low and review tractable.
- **A real multi-agent peer process caught genuine defects before they
  shipped.** Two peer coding agents were consulted by default, plus a
  cross-talk "debate" tool for the hardest calls. This caught:
  - a wrong page-table topology;
  - a reap double-free;
  - a userspace→kernel denial-of-service (the "H1" fix in #29, where a user
    with a bad RSP could halt the whole kernel); and
  - a syscall-return validation gap.

  The step-(b) fixup design — "recover only not-present faults; a protection
  fault after validation stays fatal" — came out of that debate.
- **Honesty discipline held.** Local PASS was never equated with remote CI;
  AI-assisted review was always labelled as not independent human
  qualification; and implemented-vs-tested-vs-planned framing kept claims
  precise.

## 5. What was hard / lessons

- **Bring-up foot-guns found by testing, not theory:** A/D-bit masking in the
  guard/alias CAS, and the requirement that intermediate page-table entries
  carry the W bit or the CR3 load triple-faults.
- **CI caught a real portability gap local runs hid:** an NX-dependent check
  failed on a no-NX CPU profile, forcing the M3a VM check to be gated on
  `je_arch_has_nx()`. Remote CI across profiles beats a single local pass.
- **Keeping ADR 0007 in sync with reality took two amendments (#21, #22).**
  Recording the reasoning, not just the decision, paid off.

## 6. Honest limits carried forward

- Single CPU, PCID off, IF=0 in the fault handler; SMP would need a per-CPU
  off-stack handshake.
- No SMAP/SMEP: the user-copy exception-fixup net covers the approved copy site
  only, not stray kernel dereferences of mapped user addresses. "Fail-closed"
  holds under uncorrupted-kernel invariants, not absolutely.
- The v0 syscall ABI is explicitly provisional — no stable ABI promise.
- All review was AI-assisted, **not** independent human qualification; the
  human owner is the merge gate.

## 7. Conclusion

The ADR 0007 M3a userspace boundary (#2–#5) is implemented end-to-end and
qualified by the evidence above on commit `b28f947`. Issue #6's acceptance
(exact fault markers + surviving-task progress, no timeout-as-success, and
ledger / limits / roadmap / handoff updated on the exact tested commit) is met
by this document plus the per-PR doc updates.

Next tracks: M3b (typed handles), M3c (capability IPC + init), MP0 (ARM64 boot
contract).
