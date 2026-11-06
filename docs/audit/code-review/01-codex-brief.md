You are a MAX-effort code reviewer. Read the ACTUAL files in this repo (you have
read access). Your slice: the SAFETY-CRITICAL + UNTRUSTED-INPUT C and the aarch64
low-level code:
  kernel/core/fdt.c (untrusted DTB parser, 1208 lines) + include/jeios/fdt.h
  kernel/core/image.c (untrusted image loader) + kernel/core/boot.c (x86 multiboot parse)
  kernel/core/usercopy.c, kernel/core/alloc.c, kernel/core/handle.c
  kernel/arch/aarch64/{paging.c, gic.c, physmem.c, vectors.S, paging_enable.S, boot.S}
  kernel/arch/x86_64/{paging.c, vm_backend.c, trap.c}

For EACH concrete finding: file:line, the observation, why it matters (bug?
latent UB? overflow? a green test that doesn't actually prove the property?
fragile pattern? dead code? inconsistent error handling?), and a concrete fix.
Separate: (A) real correctness/safety issues, (B) robustness/clarity/consistency
improvements, (C) places a test LOOKS like it proves a property but doesn't
(the recurring defect class) + what mutant would expose it. Call out anything
that is genuinely EXCELLENT and should be a pattern for the rest of the code.

Then, for the CURL-STYLE assurance deliverable, recommend concretely:
- which untrusted-input parsers most need a FUZZ/torture harness (fdt.c, image.c,
  boot.c, usercopy bounds) and what a minimal hosted libFuzzer/AFL-style or
  deterministic-corpus fuzzer would target;
- a short list of NAMED CRITICAL MUTANTS (per security invariant: W^X, stale
  PTE/handle, zeroization, bounds, rights masks, EOI/IRQ routing, fail-closed
  parser rejects) that a committed mutant-runner should build-and-kill, each with
  the invariant + the killer test that must fail;
- static-analysis / sanitizer-matrix additions that are LOCALLY runnable
  (scan-build, cppcheck, -fsanitize=undefined already on; add MSan? thread? a
  -Wconversion sweep? coverage?).

End with "top 8 code improvements, ranked" and "top 5 assurance/CI additions,
ranked". Keep it concrete and tied to file:line.
# JeiOS code-improvability audit — shared facts (2026-11-06)

JeiOS: a fresh-from-scratch C11 microkernel (NOT Linux), AMD64 + aarch64, built
by AI agents under a human owner with strict honesty/evidence discipline. Repo:
~/oss/jeios-workspace/jeios (you have read access; READ THE ACTUAL FILES, don't
guess). Build flags are strict: -std=c11 -Wall -Wextra -Werror -Wconversion
-Wshadow -Wstrict-prototypes; hosted tests add -fsanitize=address,undefined
-fno-sanitize-recover=all. ARM is -mstrict-align -mgeneral-regs-only.

This is a CODE-QUALITY / IMPROVABILITY review of everything written so far. The
owner + planning agent will read it. Goal: find concrete, actionable
improvements — correctness, safety, clarity, consistency, dead code, missing
tests, fragile patterns — AND confirm what is genuinely good. Do NOT invent
problems; do NOT propose rewrites of working, tested code without a concrete
defect. Rank by value. Cite file:line.

## Ground rules / context
- All code is merged + CI-green; M0-M4 on AMD64 (boot, exceptions, page/heap
  allocator, timer/IRQ, preemptive kernel threads, userspace/handles/IPC/
  syscalls, service supervisor + devices). aarch64 at MP2 (entry, MMU identity
  W^X map, GICv2 + CNTP timer, returning IRQ path). No ARM userspace/scheduler
  yet (ADR 0013 accepted on a branch, not in main).
- Many modules are hosted-testable (tests/test_*.c) with ASan/UBSan. Mutation
  testing has been MANUAL so far (not committed/automated) — the planning agent
  wants a committed critical-mutant manifest + runner (curl-style assurance).
- Honesty rules: no overclaim; "a green test that passes though the security
  property is broken" is the recurring defect class the review process exists to
  catch. Prefer finding MORE of that class.
- Owner-gated: changing tested M1/M2/M3/M4 code or a frozen ADR/contract or the
  frozen aarch64 trapframe/vectors needs owner escalation — so FLAG such changes
  as "owner-gated proposal", don't assume they're free.

## Key files (LOC)
kernel/core: main.c 3874 (x86 integration harness), fdt.c 1208 (untrusted DTB
parser), syscall.c 1177, endpoint.c 989, sched.c 853, vm.c 549,
endpoint_kern.c 495, device.c 324, spawn.c 261, handle.c 238, usercopy.c 218,
image.c 214 (untrusted image loader), proc.c 189, alloc.c 136, boot.c 101
(x86 multiboot parser).
kernel/arch/x86_64: user_demo.S 2255, vm_backend.c 391, trap.c 187, paging.c 151,
platform.c 98, boot.S 111.
kernel/arch/aarch64: main.c 1434, vectors.S 624, paging.c 610, gic.c 383,
boot.S 253, paging_enable.S 94 (+ physmem.c, gic.h, paging.h, trapframe.h).
include/jeios: endpoint.h 533, abi.h 370, svc.h 335, vm.h 327, fdt.h 294,
handle.h 192, proc.h 160, sched.h 122, device.h 94, alloc.h, boot.h, status.h.
tests: test_{alloc,boot,core,device,endpoint,fdt,gic_bits,handle,image,
paging_bits,physmem,physmem_arm,proc,proc_retire,sched_killall,syscall,uframe,
vm}.c + vm_fake.c.
tools: check_elf.py, check_user_image.py, emulator.py, gen_dtb_fixture.py,
integration.py, mkimage.py, smoke.py, smoke_arm.py, test_mkimage.py,
test_smoke_arm_timer.py.

## Known debt already recorded (don't just re-report; go deeper or confirm)
- kernel/core/main.c is really the x86 integration harness but lives in core/.
- include/jeios/arch.h exports PC-isms (irq3_mask, master_isr, tss_set_com2_io,
  cpu_policy_cr0/cr4).
- two ARM tick counters (gic.c je_arm_ticks, main.c je_arm_irq_tick_count).
- file-static je_arm_pages exposed for the scheduler.
- vectors.S re-edited each ARM PR though treated as frozen.
- no lock primitive anywhere; single-core-correct only.
