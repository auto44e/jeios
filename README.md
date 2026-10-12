# JeiOS

JeiOS is an experimental operating system with a fresh C kernel. Its design
combines Unix interoperability, an NT-inspired object/service model, and an
integrated, deeply customizable desktop. Privacy, explicit authority, recoverable
operations, and hardware feature negotiation are foundational goals.

**Current stage: M0 scaffold.** The source builds an AMD64 Multiboot2 ELF with a
32-bit-to-64-bit entry bridge, development serial output, static initialization
rollback, and pure capability selection helpers. It is not a usable or secure OS.
There are no processes, interrupts, allocator, filesystem, graphics, dynamic
modules, Linux/BSD/Windows compatibility, or ARM implementation yet.

## Start here

- [Detailed design](docs/DESIGN.md): architecture, contracts, alternatives, and acceptance criteria.
- [Hardware policy](docs/HARDWARE.md): baseline versus optional hardware features.
- [Security and privacy](docs/SECURITY.md): threat boundaries, degraded operation, and limitations.
- [Development](docs/DEVELOPMENT.md): exact build, emulation, and verification commands.
- [Roadmap](docs/ROADMAP.md): gated milestones and dependency order.
- [Agent handoff](docs/HANDOFF.md) and [AGENTS.md](AGENTS.md): continuation instructions.
- [References](docs/REFERENCES.md): primary sources and limits of borrowed concepts.

## Build

On an x86-64 Linux host with GCC, binutils, make, and Python 3:

```sh
make check
```

With GRUB PC utilities, xorriso, and QEMU installed:

```sh
make smoke
```

Outputs are under `build/`. See DEVELOPMENT.md for dependencies and what these
checks do and do not prove. Early page tables are deliberately temporary RWX
supervisor mappings. Do not install this image on a machine holding valuable data.

## Project decisions

The kernel and OS core use C11 with small architecture-specific assembly bridges.
The initial PC target is early AMD64, not every historical 64-bit ISA. AArch64 and
later RV64 need independent ports and platform drivers. Modern acceleration is
optional and advertised only when the OS can safely enable and preserve it.

The native desktop will have consistent interaction and accessibility contracts,
public theming/composition APIs, replaceable shells, and per-area complexity
settings independent of administrative permissions. These are planned features.

Licensing is **pending owner selection**. No external kernel code has been
imported and no permissive or copyleft license is implicitly selected. See
`docs/adr/0005-licensing.md` before accepting third-party contributions.
