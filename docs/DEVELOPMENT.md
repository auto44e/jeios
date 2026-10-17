# Build, fault injection and verification

Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
current baseline and linked issue dependencies. Future designs remain unimplemented.


## Host and dependencies

Use an x86-64 Linux host with GNU GCC/binutils, make and Python 3. This is not yet
a cross-ISA toolchain. Hosted parser/allocator tests require GCC ASan and UBSan.
No third-party source is downloaded by the Makefile.

```sh
sudo apt-get update
sudo apt-get install gcc binutils make python3 grub-pc-bin grub-common xorriso qemu-system-x86
make check
make smoke integration
```

`make check` builds the normal ELF, runs core policy/module tests, parser tests
(including 20,000 deterministic mutations), allocator tests and ELF structural
checks. `make smoke` makes a BIOS ISO and observes the normal kernel workload.
`make integration` builds independent fault kernels and requires exact CPU vector/
error evidence. Failure or timeout does not count as passing.

The ISO recipe explicitly selects `GRUB_PC_DIR` (default
`/usr/lib/grub/i386-pc`); installed EFI modules cannot silently change its type.
A relocated GRUB toolchain can override that variable. No EFI boot promise is made.

## Build outputs and normal execution

Normal artifacts: `build/0/jeios.elf`, `build/0/jeios.iso`. Test mode N uses
`build/N/`, preventing stale injected-fault objects from entering a normal build.
Run `make clean` when changing compiler/target flags. Baseline objects use
`-march=x86-64`, no red zone and no SIMD/FPU register generation.

The successful normal workload prints these milestones (addresses/counts vary):

```text
JeiOS: entered C kernel
M1: traps ready
M1: boot map validated; reservations ready
CPU: NX enforced
M1: protected mappings and stack guards ready
M2: allocator ready; free pages=...
M2: seven guarded threads ready; enabling PIT
M2: preemption/GPR/sleep/heap checks passed; ticks=...
M2: exited threads reaped; page accounting restored
JeiOS: initialization complete; halted
```

`CPU: legacy profile, NX absent` replaces the NX line on the legacy profile.
The harness terminates QEMU after verified terminal evidence. The kernel normally
halts itself with interrupts disabled; there is no shell or persistent runtime.

## Platform profiles

After `make iso`:

```sh
python3 tools/smoke.py build/0/jeios.iso --expect-nx present
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent
python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present
```

The large-memory profile tests clipping: the implementation manages only RAM below
1 GiB. The 16 MiB profile is emulator evidence, not a certified minimum real-PC
specification. QEMU uses one CPU, emulation rather than KVM, no NIC, no display
and serial stdout. The harness allows 15 seconds for terminal evidence and bounds
emulator cleanup separately. A stalled PIT fails via the harness; no kernel
watchdog exists yet.

## Fault matrix

| Mode | Injection | Required vector/error | Mechanism exercised |
| --- | --- | --- | --- |
| 1 | UD2 | #UD 6 / 0 | No-error-code exception frame |
| 2 | Out-of-table GDT selector 0x38 | #GP 13 / 0x38 | CPU error-code frame |
| 3 | #PF with absent #PF gate | #DF 8 / 0 | TSS emergency stack before new mappings |
| 4 | Write kernel code | #PF 14 / 3 | Read-only supervisor mapping and CR0.WP |
| 5 | Read bootstrap guard | #PF 14 / 0 | Absent guard mapping |
| 6 | Execute writable data | #PF 14 / 17 | NX instruction-fetch protection |
| 7 | Push through exhausted bootstrap stack | #DF 8 / 0 | Emergency stack after guard enforcement |

For a single diagnostic build: `make iso TEST_MODE=7`. The normal smoke script
expects successful initialization and intentionally rejects a fault image; use
`tools/integration.py` or the `boot()` helper in `tools/emulator.py` with the
appropriate expected terminal/vector. Mode 6 requires NX-capable emulation.

## Debugging

```sh
readelf -h -l build/0/jeios.elf
nm -u build/0/jeios.elf
objdump -d build/0/jeios.elf
qemu-system-x86_64 -cdrom build/0/jeios.iso -m 128M -display none -serial stdio -monitor none -no-reboot -no-shutdown -nic none -s -S
```

Attach locally with `gdb build/0/jeios.elf` and `target remote localhost:1234`.
The entry bridge initially executes 32-bit instructions in an ELF64 image;
disassembler mode must match the current execution state. Do not expose the
unauthenticated development debugger remotely.

## Sanitizers and CI

CI runs full default sanitizer settings, builds/boots both CPU profiles, low/large
memory profiles, and all seven fault modes. Local managed environments which deny
LeakSanitizer thread inspection may use `ASAN_OPTIONS=detect_leaks=0 make check`.
This disables only leak scanning; ASan memory-access and UBSan checks remain.
Such a local exception must be recorded in the handoff. Never globally suppress
sanitizer errors to get a passing check.

CI dependencies/runner images are not a pinned reproducible supply chain yet.
Successful tests do not prove secure boot, process isolation, arbitrary hardware
compatibility, or complete memory safety. See IMPLEMENTATION.md for exact caller,
lifetime, interrupt and memory limits.

## Curl-inspired CI direction — 2026-10-16

[CI_STRATEGY.md](CI_STRATEGY.md) records the owner's short/long-term testing goal:
portable regressions, compiler diversity, sanitizers/static analysis, replayable
failure injection and fuzzing, supplemented by native kernel execution and storage
crash-recovery qualification. [Core #18](https://github.com/auto44e/jeios/issues/18)
is the first bounded CI implementation task. Existing gates remain mandatory;
M3a and mobile MP0 remain next. This update adds planning only, not workflow jobs.
