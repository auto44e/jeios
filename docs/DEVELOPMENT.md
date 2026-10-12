# Build and verification

## Host requirements

Use an x86-64 Linux development host. Current Makefile invokes GNU GCC and GNU ld;
it is not a cross-platform or cross-ISA build system yet. C11, binutils (`ld`,
`nm`), make, and Python 3 are required. Hosted tests link the GCC UB sanitizer.
No third-party source dependency is downloaded by the Makefile.

On Ubuntu/Debian with authorized local package administration:

```sh
sudo apt-get update
sudo apt-get install gcc binutils make python3 grub-pc-bin grub-common xorriso qemu-system-x86
```

## Core checks

```sh
make check
```

This builds `build/jeios.elf`, executes portable core tests with undefined-behavior
checks, and verifies ELF64 AMD64 metadata, the Multiboot2 header/checksum/end tag,
entry placement, load flags and absence of unresolved symbols. Baseline flags
exclude SIMD register use and the red zone. Debug information is enabled.

Hosted tests exercise feature prerequisites and no partial enablement on failure,
reverse-order cleanup, failed-init ownership, empty/invalid module lists,
prevalidation before side effects, and stable status naming. They do not prove
correctness of privileged instructions or exception isolation. The ELF check is
structural, not boot evidence or runtime W^X enforcement.

## Actual boot test

```sh
make smoke
```

`make iso` first requires GRUB to recognize Multiboot2, stages the ELF and GRUB
config, and builds `build/jeios.iso` with xorriso. The recipe explicitly selects
`GRUB_PC_DIR` (default `/usr/lib/grub/i386-pc`) to build the M0 BIOS image;
extra EFI modules on a development host do not change the image type. `tools/smoke.py` boots QEMU with
`pc`, `qemu64`, 128 MiB, no NIC and no display; serial goes to the terminal. It
waits up to 12 seconds, then requires C-entry and successful initialization
markers with no fatal marker. Intentional halt means a timeout alone is never
success. QEMU can be terminated after the success marker; the harness bounds it.

Expected serial output includes:

```text
JeiOS M0: entered C kernel
CPU: NX present (not enabled in M0)
JeiOS M0: initialization complete; halted
```

NX-absent CPUs have the explicit legacy line. This is presence reporting only.
To reproduce the legacy-path check after building the ISO:

```sh
python3 tools/smoke.py build/jeios.iso --cpu qemu64,-nx --expect-nx absent
```
The smoke test uses emulation, not KVM, so it does not require host virtualization.
It does not validate BIOS/UEFI diversity, actual old PCs, ARM, drivers or sandboxing.

## Manual inspection and debugging

```sh
readelf -h -l build/jeios.elf
nm -u build/jeios.elf
objdump -d build/jeios.elf
qemu-system-x86_64 -cdrom build/jeios.iso -m 128M -display none -serial stdio -monitor none -no-reboot -no-shutdown -nic none -s -S
```

The last command starts paused with a development GDB stub. Attach locally with
`gdb build/jeios.elf`, then `target remote localhost:1234`. Do not expose the GDB
port remotely. Before long-mode entry the machine is executing 32-bit code in an
ELF64 image; debugger disassembly mode must match the current execution state.

## CI

`.github/workflows/ci.yml` runs the same core/ELF and QEMU smoke checks on Ubuntu
24.04. Its job installs tools and does not upload binaries, publish releases, or
hold signing keys. Dependency installation and the hosted runner image are not
pinned reproducible build inputs. Pin toolchain and action revisions before a
production supply-chain claim. See HANDOFF for checks actually observed this turn.

## Known bootstrap limitations

Identity-map first GiB; no validated RAM map; no allocator; no IDT; interrupts
masked; only BSP; no SMP; ring 0 only; temporary RWX mappings; development COM1
only; no framebuffer, UEFI-specific entry, disks, network, secure boot, dynamic
module loader or AArch64 path. Unexpected faults can triple fault. A serial
iteration limit is not a monotonic time deadline. Do not expand device support
before trap/memory milestones.
