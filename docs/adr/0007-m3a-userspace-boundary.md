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

Each process owns a PML4, PDPT, user PD and user PT (exactly four private table
pages). Kernel PD/PT topology may be shared, but the kernel branch must remain
supervisor-only (U=0) at every level **from PDPT[0] downward**; PML4[0] is shared
by both branches and therefore carries U=1 (the arena needs it), so isolation of
the first GiB depends on the kernel's PDPT[0] carrying U=0, never on a leaf bit
alone. The arena path is PML4[0] → PDPT[1] → user PD[0] → user PT, where PDPT[0]
(the kernel branch of each process's own PDPT) carries U=0. Effective permissions are
checked at **every** level — a leaf U bit does not override an ancestor, and an
ancestor must carry the most permissive bits the arena needs (P|U|W, no NX on
PML4[0]/PDPT[1]) with restriction expressed at the leaf. User mappings are never
global; PCID stays disabled and CR3 is reloaded on address-space switch. Code is
RX, constants R/NX, data/BSS/stack RW/NX; W+X is never mapped.

Every new user page **and every page-table page (PML4, PDPT, user PD, user PT)**
is zeroed before it is installed or linked in. Publication order (corrected
2026-10-17; supersedes the earlier "publish RX then
demote alias" wording): for each frame — allocate → zero → copy image bytes
through a supervisor-writable alias. For an **executable** (`USER_CODE`) frame
only: unmap and flush any temporary loader alias, then **demote the frame's
identity alias to RO/NX via `je_user_alias_set`** → flush →
**then install the user RX leaf
PTE**, so a user-X mapping and a writable alias never coexist. (After this, the
identity alias is the only remaining alias of an executable frame.) For a
`USER_DATA` (including stack) frame: flush → install the RW/NX user leaf PTE (its
identity alias stays writable; the frame is never user-executable, so no W+X
arises). Teardown is the exact reverse: clear the user PTE → flush → for an
executable frame only, restore its identity alias to RW via `je_user_alias_set` →
free. An executable frame must have its alias back to RW before
`je_pages_free`, because the allocator/heap zeroes through the identity alias; if
the alias restore fails, panic rather than free (freeing a RO frame would fault a
later zeroing in CPL0).

Alias protection uses two disjoint per-frame paths. The **user path** operates
only on `USER_CODE` frames in the caller process's owned extents and moves them
`RW ↔ RO_NX` through a single named operation, `je_user_alias_set`, which does a
compare-and-set against the exact expected old PTE value and never falls back to
`je_arch_page_restore`; it refuses `USER_DATA`, `TABLE`, frames outside the
caller's extents and the kernel image. The **kernel-stack path** is the
pre-existing `RW ↔ GUARD` guard-page handling for kernel stacks (not an owned
extent, not a user frame) and uses `je_arch_page_restore`, which unconditionally
rewrites `present|write|nx`; it is never used on a user-frame alias. An explicit
per-process owned-extents list (independent of virtual address), with a frame
*kind* (`USER_CODE`, `USER_DATA`, `TABLE`) and owner, is the only source
permitted to populate user PTEs. Stack frames are classified as `USER_DATA`
(there is no separate `STACK` kind). The PTE-install
operation takes an owned-frame reference `(extent, page index)`, never a raw
physical address, and **rejects `kind == TABLE`**; `USER_CODE` installs only as
RX, `USER_DATA` only as RW/NX. MMIO, reserved pages, page-table frames and
another owner's frames therefore cannot be expressed as a user mapping. No user
operation accepts a physical address.

Demotion scope is deliberate: only **executable** (`USER_CODE`) user frames have
their writable alias removed (demoted to RO/NX). `USER_DATA` frames (including stack)
keep a writable identity alias — that is how the kernel scrubs and recycles them — and
are never user-executable, so no W+X exists anywhere. The Consequences section's
"no writable alias for executable user frames" is the precise claim.

### D4. Admission bounds (M3a fixtures)
At most 2 user processes; 1 user thread each; ≤64 user pages/process (incl. a
4-page stack); 4 private table pages; 5 kernel-stack pages (4 usable + guard).
Maximum 73 allocated pages/process excluding preallocated process metadata. Check
this admission budget against actual free pages at boot, keep a kernel reserve,
and reject insufficient memory atomically. An unmapped page sits below the user
stack; segment edges are unmapped where feasible.

Amendment 2026-10-17 (allocation vs reservation): the 73-page budget is the
admission *quota*; it is not all allocated by issue #3. Issue #3 allocates only
the 4 table pages and the declared user pages (image + 4-page stack, ≤64). It
additionally *holds* the 5 kernel-stack pages as a reservation against free
memory at admission time — counting them in the atomic admission check and
keeping them out of other allocations — even though issue #4 (ring-3 entry and
the per-thread kernel stack loaded into the per-CPU TSS.RSP0) performs the actual
kernel-stack allocation. Without that held reservation, #4 could fail after #3's admission succeeded, breaking the
"reject atomically" rule. Admission arithmetic and the 2-process limit are
enforced in #3. The kernel reserve is a fixed minimum of free pages that
admission must leave untouched; its exact value is set when #3 lands and recorded
in IMPLEMENTATION.md, so the atomic-reject test has a concrete threshold.

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

Amendment 2026-10-17 (image-descriptor precision). The user arena is the
half-open range `[0x40000000, 0x40200000)` (2 MiB). Descriptor fields are
fixed-width little-endian with an explicit on-wire layout; validation never runs
against a padded host struct. Segment `target_offset` and `memory_size` must be
page-aligned, so no two segments share a page. Because segments are page-aligned,
any shared page is invalid outright; "overlap" is tested at **page granule**
(byte-granule sharing cannot arise). A segment with `memory_size == 0` is
rejected. `entry_offset` is arena-relative and must lie inside the initialized
(source) bytes of an executable segment — not in BSS and not in a non-executable
segment. The 4-page user stack occupies the top of the arena with one unmapped
guard page below it; image segments must not overlap the stack or that guard
page. All bound checks are subtraction-based on `uint64_t`
(`src_size <= blob_len - src_off`, `mem_size <= ARENA - tgt_off`,
`src_size <= mem_size`) and are applied *before* any page rounding, so rounding
cannot wrap. Segment count is bounded and checked before indexing; permissions
reject unknown bits, `perm == 0`, W|X, and X-or-W without R. The page budget sums
per-segment pages plus the 4-page stack and must be ≤64 user pages.

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

Issue split (clarified 2026-10-17). Issue #3 is proven **hosted-only**: a VM /
loader model plus a transactional rollback proof (an alias ledger and a
reverse-order unwind journal whose undo steps are infallible — an undo failure is
a panic, not a silent leak). #3 does NOT claim real CR3 switching, TLB behavior,
`invlpg`/CR3 ordering, CPL3 execution, U/S or NX hardware enforcement, GDT/IDT or
TSS.RSP0 changes, syscalls, copy-fixup assembly or no-FPU enforcement; those are
issue #4 and later, verified in QEMU. The hosted model verifies modeled PTE
flags, the alias state machine, zero-before-exposure, ownership, admission
arithmetic and exact rollback after failure injected at every allocation / map /
alias-protection step.

## Amendment history

- 2026-10-17: corrected D3 publication order (demote writable aliases to RO/NX
  before installing the user RX PTE, not after); added the per-frame alias state
  machine and the owned-frame-reference PTE-install rule; pinned D4 allocation-vs-
  reservation and the kernel reserve; pinned D8 arena half-openness, page-aligned
  segments, page-granule overlap and arena-relative entry. Prompted by an
  external multi-agent design review (three AI coding agents) against commit
  `88e3f00`; the reviews converged on the D3 ordering hazard and the alias-ledger
  requirement. This is AI-assisted review, not the independent human
  qualification the critical profile requires (see Status of claims). This is a
  contract correction before any #3 code; it does not change the M3a scope.

## References

- [M3A.md](../M3A.md) (proposal this ADR freezes), [ROADMAP.md](../ROADMAP.md),
  [IMPLEMENTATION.md](../IMPLEMENTATION.md), [HANDOFF.md](../HANDOFF.md),
  [SECURITY.md](../SECURITY.md), [AGENT_START.md](../AGENT_START.md).
- ADR [0002](0002-modularity-and-isolation.md) (modularity vs isolation),
  [0006](0006-bsp-foundation-and-page-heap.md) (BSP/paging/heap bounds).
- Intel SDM Vol. 3A (paging/protection, IA-32e interrupts/TSS, IRET, control
  registers, extended state); pair with current AMD architectural documentation
  before any hardware qualification. Pin the exact revision during implementation.
