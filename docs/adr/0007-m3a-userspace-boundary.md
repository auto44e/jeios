# ADR 0007: Freeze the M3a userspace-boundary contract

Status: accepted contract, 2026-10-17. Scope: M3a only. No kernel code changes
accompany this ADR; it converts the [M3A.md](../M3A.md) proposals into reviewed
decisions that issues [#3](https://github.com/auto44e/jeios/issues/3)–[#6](https://github.com/auto44e/jeios/issues/6)
implement and test against. Supersedes the "proposed/recommendation" wording in
M3A.md sections 2–5 for M3a; M3b/M3c numbers there remain proposals (see ADR
follow-ups). Reviewed baseline: `main` at the merge of PR #17-era docs; M1/M2 is
the implemented foundation per [IMPLEMENTATION.md](../IMPLEMENTATION.md).

## Context

M2 ends with one AMD64 CPU, CPL0 only, a first-GiB physical allocator, a
single static page-table root in `paging.c` that identity-maps writable RAM, a
GDT/IDT with kernel descriptors only, and a scheduler that switches trap frames
without switching address spaces. M3a must introduce the first user/kernel
boundary. M3A.md proposed concrete numbers and an ABI but explicitly left them to
"the first ADR." This is that ADR. The decision is to build the smallest
demonstrable boundary with isolation proven by test, not to maximize features.

## Decision

### D1. Scope and security claim
M3a delivers two real CPL3 processes, one thread each, with distinct page-table
roots and distinct physical backing, bounded built-in programs, a minimal
experimental syscall gate, checked copy-in/copy-out, task-local synchronous fault
containment and complete per-process reclamation. Retain one CPU, BIOS/PIC/PIT,
4 KiB pages and the first-GiB allocator limit. No dynamic loading, ELF ABI
promise, fork, shared address spaces, multiple user threads per process, demand
paging, mmap, signals, SMP, filesystem, networking, graphics or cryptography.
The claim is "experimental single-CPU userspace isolation in qualified emulator
profiles," never a hardened sandbox or "M3 complete."

### D2. Launch profile requires NX
M3a permits user execution only when NX (EFER.NXE + hardware) is usable. On
NX-absent CPUs, refuse user launch with `UNSUPPORTED` and retain the existing
M1/M2 kernel-only development profile. This is a qualification rule, not a claim
that NX absence makes U/S isolation impossible; a weaker user profile may be
reviewed separately later. Hardware support and security guarantees stay distinct.

### D3. Address-space layout and ownership
Per-process virtual layout for M3a:

| Virtual range | Use |
| --- | --- |
| below `0x40000000` | existing validated supervisor-only kernel identity region; holes/guards preserved |
| `0x40000000`–`0x401fffff` | per-process 2 MiB user arena; only declared pages mapped |
| all other addresses | unmapped in M3a |

Each process owns a PML4, PDPT, user PD and user PT. Kernel PD/PT topology may be
shared read-only; leaf edits remain serialized kernel operations. Effective
permissions are checked at **every** level — a leaf U bit does not override an
ancestor. User mappings are never global; PCID stays disabled and CR3 is reloaded
on address-space switch. Code is RX, constants R/NX, data/BSS/stack RW/NX; W+X is
never mapped. Every new user page and page-table page is zeroed before
installation. Image bytes are copied through supervisor mappings, then the
identity alias of executable frames is made read-only/NX — no writable physical
alias survives publication. An explicit per-process owned-extents list (independent
of virtual address) is the only source permitted to populate user PTEs; MMIO,
reserved pages, page tables and another owner's frames are rejected. No user
operation accepts a physical address.

### D4. Admission bounds (M3a fixtures)
At most 2 user processes; 1 user thread each; ≤64 user pages/process (incl. a
4-page stack); 4 private table pages; 5 kernel-stack pages (4 usable + guard).
Maximum 73 allocated pages/process excluding preallocated process metadata. Check
this admission budget against actual free pages at boot, keep a kernel reserve,
and reject insufficient memory atomically. An unmapped page sits below the user
stack; segment edges are unmapped where feasible.

### D5. Lifecycle
`BUILDING → READY/RUNNING → DYING → REAPABLE → FREE`. A fault or exit first
prevents redispatch; reclamation switches to a live stack and a different CR3 and
removes all scheduler/frame references before freeing. Individual dead processes
are reaped from a safe kernel context while survivors continue. Data is cleared on
its next exposure; immediate erasure and temporal safety for arbitrary kernel
pointers are not promised. Any build failure unwinds mappings, alias changes,
stacks, page tables and accounting in reverse order.

### D6. Entry, return and execution state
Append ring-3 code/data GDT descriptors without moving existing kernel/TSS
selectors. Add one dedicated DPL3 interrupt gate at vector `0x80` with IST=0, so
privilege entry uses the current thread's TSS.RSP0; keep kernel-only vector `0x81`
and the emergency IST stacks. Use IRETQ; SYSCALL/SYSRET is deferred. On every user
return, validate canonical RIP/RSP within the arena, exact user CS/SS and a
permitted RFLAGS mask; force IF=1, IOPL=0, clear NT/VM and unsupported debug/control
flags. Invalid user return state terminates that process before IRETQ; unexpected
kernel-frame corruption remains fatal. Normalize DS/ES/FS/GS for the no-TLS
profile; keep CR4.FSGSBASE disabled with no user base-setting API.

### D7. No FPU/SIMD in M3a — enforced, not assumed
Configure an architectural denial policy for x87/MMX/SSE/AVX and extended state
(including CR0.TS/MP and disabled OS enablement); a user #NM/#UD terminates the
offender. Lazy FPU switching is prohibited. This is a merge prerequisite verified
by test, not a compiler flag. A compiler restriction is insufficient against
hostile machine code.

### D8. Built-in image and experimental ABI v0
Use a checked built-in image descriptor (version, bounded segment count, source
offset/size, target offset/memory size, permissions, entry offset); defer ELF
parsing. Validate arithmetic, source/arena bounds, alignment, overlaps, entry
inside executable bytes, W+X conflicts, page budget and BSS size. Image labels are
diagnostic, not signatures. Native development ABI v0: fixed-width 64-bit register
args, explicitly numbered statuses, **no kernel pointers**, **no stable-ABI
promise**. RAX = operation/result status; RDI, RSI, RDX, R10, R8, R9 = args; RDX
may carry a scalar result. Unknown operations return `UNSUPPORTED`; reserved args
must be zero. First operations: `abi_query`, `yield`, `exit(code)`, and a
development-only `copy_probe(src,dst,len)` (≤256 bytes, validates both spans,
stages through kernel scratch, no persistent effect). The experimental ABI is kept
separate from the internal `status.h` enum numbering.

### D9. User-copy discipline
`copy_from_user`/`copy_to_user` take numeric user addresses + bounded lengths;
reject noncanonical addresses, wraparound, kernel ranges and per-page permission
mismatches before copying. Zero-length touches neither pointer. Use
subtraction-based bound checks; never dereference before validation. Copy input
once into kernel storage; handlers retain no user pointer. Introduce small
assembly copy sites with an exception-fixup table: recovery requires a CPL0 #PF at
an exact approved copy instruction, an active per-thread copy scope, CR2 within
the declared span and a valid kernel-owned counterpart. No global "ignore kernel
faults" flag; no nested copy scopes or scheduling while a copy is active in M3a.
Partial copy-out on a recovered fault is unspecified bytes within the validated
span; no authority-bearing operation commits before successful output handling.

### D10. Syscall execution model
M3a syscalls are bounded and nonblocking; interrupts stay disabled on their short
critical path. Yield/exit switch directly through the trap dispatcher; user
computation remains timer-preemptible. Do not call M2 sleep wrappers or take a
recursive scheduling interrupt from syscall dispatch. General waits arrive in M3c.
Route synchronous user faults (#PF/#GP/#UD/#NM/…) to process death with a bounded
local reason, determining origin from saved CS (not merely the #PF U/S bit).
Machine checks, double faults and unexpected kernel exceptions are not converted
to app errors.

### D11. Reserve, do not implement, M3b/M3c surface
M3a reserves a process-owned handle-table field but exposes **no** placeholder
handle/IPC syscall that reports success. Typed handles (ADR follow-up for #7) and
IPC/init (follow-up for #8) are specified later. `je_status` values are explicitly
not a user ABI and must not leak their numbering.

## Consequences

- `paging.c`'s single static root and writable identity map must gain per-process
  roots and lose writable aliases for executable user frames; this is the main
  integration hazard.
- Adding user GDT descriptors invalidates the existing fault test's assumption
  that selector `0x28` is outside the GDT; that test and its expected error must
  be updated when the GDT grows (tracked with the #4 change).
- `sched.c` must switch CR3 and TSS.RSP0, and reap one dead process while a
  survivor runs, instead of waiting for all workers.
- Acceptance is proof-by-test (ADR §9 matrix in M3A.md is adopted): isolation,
  fault-locality, malicious return state, adversarial copy pointers, poisoned
  reused pages, per-step rollback, and reclamation-with-live-survivor, under NX
  and the 16/1536 MiB profiles. A timeout or a lone success string is not evidence.
- Enforcement/CI for these tests is sequenced with [#18](https://github.com/auto44e/jeios/issues/18);
  coding-policy evidence (critical profile, JCS rules) with [#19](https://github.com/auto44e/jeios/issues/19).

## Status of claims

Decided: the contract above. Not implemented: any M3a kernel code. Not claimed:
remote CI for any new head, independent review, hardened qualification, or FPU/SMP/
userspace runtime. This ADR changes documentation only.

## References

- [M3A.md](../M3A.md) (proposal this ADR freezes), [ROADMAP.md](../ROADMAP.md),
  [IMPLEMENTATION.md](../IMPLEMENTATION.md), [HANDOFF.md](../HANDOFF.md),
  [SECURITY.md](../SECURITY.md), [AGENT_START.md](../AGENT_START.md).
- ADR [0002](0002-modularity-and-isolation.md) (modularity vs isolation),
  [0006](0006-bsp-foundation-and-page-heap.md) (BSP/paging/heap bounds).
- Intel SDM Vol. 3A (paging/protection, IA-32e interrupts/TSS, IRET, control
  registers, extended state); pair with current AMD architectural documentation
  before any hardware qualification. Pin the exact revision during implementation.
