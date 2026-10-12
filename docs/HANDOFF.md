# Agent handoff: M1/M2 development

Current work is on `develop/m1-m2-core`. Read `docs/IMPLEMENTATION.md` for the
new gate contracts and evidence; the M0 record below is retained as history.
M1a adds the IDT, TSS/IST exception stacks and real CPU fault-injection tests.
M1b memory discovery and M2 scheduling are next in this development sequence.

Date: 2026-10-12. Owner project: `auto44e/jeios`. Stage: M0.
Read AGENTS.md, DESIGN.md and ROADMAP.md before continuing.

## Owner intent to preserve

Fresh C kernel and OS core. Modular construction, qualified modern CPU/GPU
support with an early 64-bit compatibility goal, graceful failures, privacy and
security inspired by GrapheneOS/CalyxOS, future mobile/IoT profiles, verbose
continuation documentation. Earlier product intent: NT-inspired service/object
organization; macOS-inspired coherence; native framework with Linux-style ricing;
per-area complexity independent of authority. No existing kernel source fusion.

## Actual implementation

| Component | State | Evidence / limitations |
| --- | --- | --- |
| AMD64 Multiboot2 entry | Implemented | Assembly enters C in long mode; first GiB temporary identity map |
| Kernel console | Implemented | COM1, bounded polling; missing UART is optional |
| CPU feature helper | Implemented and host-tested | Intersects hardware/OS/policy; does not enable registers |
| NX | Detection only | CPUID presence printed; page-table enforcement not implemented |
| Static module init | Implemented and host-tested | Prevalidation and reverse rollback; no dynamic loading/isolation |
| Status contract | Implemented and host-tested | Distinct failures; no stable external ABI yet |
| ELF validation | Implemented and tested | Header, target, entry/load bounds, no runtime unresolved symbols |
| QEMU smoke | Executed and passed | C entry, NX presence line, successful initialization and intentional halt observed |
| CI | Configured | Same checks plus QEMU; remote execution must be observed separately |
| Kernel/runtime isolation | Absent | No IDT, scheduler, allocator, process, handle table, IPC, DMA isolation |
| Devices / UI / other ISAs | Absent | No storage, network, GPU, desktop, ARM port, mobile or IoT implementation |

The `policy-placeholder` module has no security behavior. Do not infer any
protection from its name. Modules are caller-owned static descriptors with no
runtime state tracking; they are intended to start once, not provide restart or
reentrancy semantics. C helpers are reusable policy mechanics, not a complete
hardware detector or supervisor.

## Verification record

- Host: x86-64 Linux; GCC 13.3.0; GNU binutils ld 2.42.
- `make check`: PASS, freestanding kernel linked; core tests executed with UB
  sanitizer; ELF structural checks passed.
- `python3 -m py_compile tools/*.py`: PASS.
- `git diff --cached --check`: PASS during initial review.
- Boot: PASS using QEMU 8.2.2, GRUB 2.12 and xorriso 1.5.6. The ISO was built
  with the repository recipe; `tools/smoke.py build/jeios.iso` observed both C-entry
  and initialization markers plus NX-present reporting, with no fatal marker.
  Emulator halt was intentional. This is execution evidence, not just an ELF build.
- Local environment note: system package installation was unavailable. Packages
  were downloaded and extracted outside the repository; PATH/library/module/data
  overrides selected those tools. Initial QEMU attempts lacked its relocated
  module/BIOS paths; correcting the tool paths produced the successful smoke test.
  These environment overrides are not project source dependencies or committed
  binaries. A normally installed toolchain uses `make smoke` directly.
- Legacy-path check: PASS with `tools/smoke.py build/jeios.iso --cpu qemu64,-nx
  --expect-nx absent`; the NX-absent line and successful halt were observed.
  Both CPU profiles are configured in CI.
- Initial GitHub run passed core/ELF checks but failed ISO creation because GRUB
  auto-selected extra EFI modules on the runner and attempted to invoke missing
  `mformat`. The Makefile now explicitly selects the BIOS i386-pc module directory
  via `GRUB_PC_DIR`, matching the documented M0 platform.
- Follow-up GitHub CI: PASS for code commit
  `23900ff6dd408205064b4ec0cacd66de64ff18e3`. Core/ELF, ordinary QEMU boot,
  and NX-absent legacy-profile checks all completed successfully. Evidence:
  https://github.com/auto44e/jeios/actions/runs/38313541117
  This handoff-only evidence update changes no tested code or build recipe.
- No real-machine, AArch64, GPU, security, compatibility ABI, or mobile tests.

## Build and layout

`make check`; `make smoke` with GRUB/xorriso/QEMU dependencies. See DEVELOPMENT.md.
Do not use host-specific baseline compiler flags. Outputs stay in ignored build/.
Entry is 32-bit code in an ELF64 image; linker keeps the header within the first
32 KiB and below the mapped range. GRUB supplies protected mode/A20 and loads
segments including zero-initialized BSS. Bootstrap stack top is 16-byte aligned;
C call follows the AMD64 SysV ABI. SIMD use is disabled until state management.

## Known hazards

1. No IDT: unexpected exceptions can triple fault. Interrupts stay disabled.
2. First GiB map is RWX supervisor memory, not a validated RAM ownership map.
3. NX is never enabled; ELF segment flags do not establish runtime W^X.
4. No boot information parsing. Pointer is intentionally not dereferenced.
5. No allocation or device probing beyond development COM1/CPUID.
6. Serial polling count is bounded but is not a time-based deadline.
7. No current recovery from kernel faults, signing, trust chain or cryptography.
8. Licensing remains owner-pending. Do not import external kernel/driver code.
9. CI configuration is not observed CI evidence; runner dependencies are unpinned.

## Next task: M1a exceptions

Implement IDT/trap stubs and a bounded diagnostic path. Specify the trap frame,
register preservation, stack alignment, privilege transition fields and error
code distinctions. Keep interrupts disabled until handlers and controller setup
are ready. Include a dedicated stack for double-fault handling when its TSS setup
is correct. Distinguish safe deliberate test faults from arbitrary corruption.

Pass criteria: ordinary `make smoke` still observes both success markers;
a separate fault-injection build/test observes the expected exception and halt
rather than reset/triple fault; trap-frame fields match architecture semantics.
Run emulator tests; hosted tests cannot validate CPU trap entry.

Then M1b: validated Multiboot2 memory-map parsing and reserved-range tracking.
Use mapped-range/length checks before memory reads. Add malformed-tag, overflow,
stride, overlap and truncated-input tests. Only then implement M2 allocation.

## Unresolved decisions

Owner distribution license, exact hardened hardware minimum, actual old-PC
minimum RAM/support list, native ABI layouts, service placement benchmarking,
UI toolkit/compositor strategy, and phone/GPU vendor targets. These are documented
questions, not blockers to M1a. Record future decisions in ADRs.
