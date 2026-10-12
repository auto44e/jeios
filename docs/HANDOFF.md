# Agent handoff: M1/M2 kernel foundation

Date: 2026-10-12. Project `auto44e/jeios`; branch `develop/m1-m2-core`.
Read AGENTS.md, IMPLEMENTATION.md, ROADMAP.md and the relevant DESIGN.md sections.
The previous M0 state is preserved in git history; this file describes the latest
branch implementation. Do not infer future capabilities from design prose.

## Owner intent

Fresh C kernel/core; modularity; modern hardware with an early AMD64 compatibility
path; graceful failures; GrapheneOS/CalyxOS-inspired security/privacy; eventual
mobile/IoT; NT-inspired object/service organization; macOS-inspired coherence;
Linux-style ricing and independent per-area UI complexity. Keep detailed records
for future agents. No existing kernel-source fusion or implied license choice.

## Implemented and tested

| Gate / component | Actual behavior | Boundary |
| --- | --- | --- |
| M1a exceptions | 256 IDT gates, normalized frames, TSS/IST for DF/NMI, bounded fatal diagnostics | Kernel faults halt; no user recovery |
| M1b boot context | Bounded Multiboot2 memory/module parser, reserved ranges, no retained loader pointers | Rejects overlapping maps; 128 records, 40 reservations |
| M1b page protection | New 4 KiB mappings, CR0.WP, NX where present, code/rodata read-only, guard pages | Early bootstrap still temporary RWX; later mappings are supervisor-only |
| M2 physical pages | First-fit contiguous extents, exact free validation and reservation exclusion | First GiB only; <=256-page allocation; caller serialization |
| M2 heap | 64 allocation records, <=1 MiB/object, zero-on-allocation and failure rollback | Page-granular; no slab/quarantine/generation safety |
| M2 execution | PIC/PIT IRQ0, eight slots including idle, seven guarded kernel workers | One CPU, CPL0, round-robin; no SIMD context or priorities |
| M2 lifecycle | Yield, tick sleep, exit and safe stack reclamation from bootstrap | No general wait queues, cancellation or SMP locks |
| Runtime checks | All GPRs, timer preemption, sleeps, retained heap bytes and page accounting | Self-test workload then halt, no application manager |

## Verification performed locally

- `ASAN_OPTIONS=detect_leaks=0 make check`: PASS with GCC 13.3.0 and binutils 2.42.
  Leak inspection is unavailable in this managed environment; ASan/UBSan remain
  active. Parser includes 20,000 mutations and truncated heap buffers. Allocation
  tests include exhaustion, fragmentation, exact frees, zeroing and rollback.
- Normal QEMU boot: PASS with NX, 128 MiB, all seven workers completing/reaped.
- NX-absent boot: PASS, including explicit weaker profile reporting.
- Low-memory (16 MiB, NX absent) and large-memory (1536 MiB, first-GiB clipping)
  profiles: PASS locally; both run the full seven-worker workload and are
  included in CI.
- `make integration`: seven real CPU fault cases pass with exact vector/error
  pairs, including stack exhaustion -> double fault on the emergency IST.
- ELF checks validate AMD64, Multiboot2, entry/load limits, separate segment flags
  and no unresolved runtime dependencies. Hardware permission tests are separate.
- GCC `-fanalyzer` with warnings as errors over every kernel C source: PASS.
- Python syntax, relative documentation links and `git diff --check`: PASS.
- No real-machine, ARM, GPU, userspace security or application ABI testing.

Emulation tools are the previous extracted QEMU 8.2.2 / GRUB 2.12 / xorriso 1.5.6
under `/tmp/jeios-tools`, with wrappers in `/tmp/jeios-bin`. Local commands use
PATH, LD_LIBRARY_PATH and QEMU_MODULE_DIR for relocation plus
`GRUB_PC_DIR=/tmp/jeios-tools/usr/lib/grub/i386-pc`. These transient paths are not
repository dependencies. Normal installed tools use DEVELOPMENT.md commands.

## Published verification evidence

Implementation commit `96d4dee037389e7b30132ad8fd7425c80a90ea1d` passed both
GitHub push and pull-request CI, including default sanitizer checks, normal and
NX-absent boots, 16/1536 MiB profiles and every fault-injection case.

- Pull request: https://github.com/auto44e/jeios/pull/1
- PR verification: https://github.com/auto44e/jeios/actions/runs/38317024232
- Branch verification: https://github.com/auto44e/jeios/actions/runs/38316970273

This subsequent handoff-only evidence update changes no tested implementation.
Check the current PR checks when continuing; never substitute a local PASS for
observed remote execution, or the original M0 checks for these new phase gates.

## Critical implementation contracts

- Boot/main context has interrupts disabled through map/allocator/thread creation.
  IRQ0 is unmasked/programmed only after scheduler state and stacks are ready.
- Handler entry preserves 15 GPRs; IRETQ restores the full 64-bit hardware frame.
  Never use SSE/AVX/FPU in this kernel before context management exists.
- Page tables describe only known complete RAM pages above 1 MiB and below 1 GiB.
  Boot, image, modules and low memory remain reserved by the allocator. Loader
  tables are copied before paging changes, and never dereferenced afterward.
- `je_pages`/`je_heap` portable functions are not internally thread-safe. Kernel
  wrappers use interrupt exclusion on the BSP. IRQ handlers must not allocate.
- Thread stacks are zeroed before guarding. An exited thread must never free its
  own live stack. Bootstrap remaps guards and frees only zombie stacks after all
  workers are done. Never free a stack still referenced by a scheduler frame.
- Sleep sets state under interrupt exclusion before the software scheduling
  interrupt. Idle checks readiness with interrupts off before adjacent STI/HLT.
- A fault kernel uses its own `build/N/` tree. Do not accidentally boot it as the
  normal image, and do not treat any timeout as proof of successful containment.

## Known limitations and hazards

No process boundary, handle/capability enforcement, secure boot, cryptography,
secure entropy, stack canaries, heap quarantine, KASLR, userspace copy recovery,
SMP, APIC, IOMMU, device lifecycle, filesystem, networking, GPU or desktop. The
remaining protection goals in DESIGN.md are future requirements. Heap pointers
can become stale after address reuse; no temporal safety is promised. One-page
stack guards do not catch arbitrary stack jumps. Large allocator scans/zeroing
occur with interrupts disabled and have no hard real-time latency guarantee.

Legacy PIC/PIT support is qualified only in the tested PC emulator profiles.
Unexpected IRQs/NMIs are fatal; broader spurious IRQ and platform fault handling
remain future work. The memory map is conservatively rejected if ranges overlap.
No RAM above 1 GiB is used even on larger systems. All seven workers are trusted
kernel threads in one address space. Never call them isolated processes/apps.

## Next bounded work: M3a userspace boundary

Start a new branch after reviewing/merging this work. Specify user/kernel address
layout, user code/data/stack mapping rights, private kernel stacks, TSS RSP0,
entry/return state, and copy-in/copy-out fault semantics before adding syscalls.
Keep CPU feature exposure restricted to state the scheduler preserves. Add a
small checked ELF loader or a deliberately bounded built-in user image, not a
large application compatibility layer at the same time.

Acceptance: two actual CPL3 tasks with distinct page tables run; a user task's
illegal access terminates only that task; kernel/other-task memory stays protected;
malformed syscall pointers cannot panic the kernel; argument lengths/overflows
are tested; no raw kernel pointer ABI. Add explicit handle rights, generation
and lifetime contracts in M3b, then bounded IPC in M3c. General wait queues/locks,
stack-canary/entropy planning and spurious interrupt handling need review before
calling M3 hardened. Do not jump straight to desktop, GPU or phone support.

## Open decisions

Owner distribution license; hardened hardware baseline; actual old-PC support;
stable native ABI; toolkit/compositor strategy; service placement measurements;
and vendor GPU/phone targets. No external kernel source has been imported.
