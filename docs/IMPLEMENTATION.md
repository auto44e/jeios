# Implementation ledger: M1 and M2

Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
current baseline and linked issue dependencies. Future designs remain unimplemented.


This file records current contracts and execution evidence as gates are built.
DESIGN.md describes the broader target architecture. No entry below implies
userspace isolation, SMP safety, driver containment or production readiness.

## M1a: exception entry

The IDT has 256 ring-0 interrupt gates. Assembly normalizes CPU error-code and
non-error-code exceptions, saves all 15 ordinary registers, clears direction,
aligns the C call stack and invokes a bounded fatal handler. `je_trap_frame`
has compile-time size/offset assertions. In 64-bit mode the CPU frame includes
SS:RSP even at CPL0. Return infrastructure uses IRETQ, with no user transitions,
SIMD state, GS switching or speculative execution mitigation claims yet.

A new GDT retains the existing kernel selectors and adds an available 64-bit TSS.
TSS IST1 is a separate 16 KiB double-fault stack; IST2 is a separate NMI stack.
Each has a reserved leading guard page to unmap during M1b. No IRQ is enabled in
M1a. Exceptions print vector/error/RIP and CR2 for page faults, then halt. Recursive
panic attempts halt directly. These development addresses are local serial data;
there is no remote collection. Production diagnostics/redaction remains future.

Fault modes 1/2/3 inject UD2, an invalid data selector, and a page fault with an
absent page-fault gate respectively. The last case exercises an actual CPU double
fault and the TSS emergency path; a software `int $8` would not produce the same
error-code frame. Fault builds live in `build/<mode>/`; normal build is `build/0/`.
The emulator harness requires exact expected vector/error and terminal markers;
QEMU exit or elapsed time without evidence is failure.

References: Intel SDM Volume 3A, Interrupt and Exception Handling / IA-32e mode,
64-bit stack frames, IST and TSS; Volume 2, LIDT/LTR/IRET.
https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf

## Verification history

M1a: GCC/ELF checks, hosted core tests, QEMU normal boot and modes 1, 2, 3
passed before proceeding to M1b. Mode 2 uses GDT selector 0x38, just beyond the
installed table limit, to deterministically exercise #GP with error code 0x38.
(Originally 0x28; the GDT grew to 7 entries when ring-3 user descriptors were
added for M3a #4, so 0x28 became the valid user-data selector and the test moved
to 0x38.) Consult HANDOFF.md for final observed
results after the complete phase sequence.

## M1b: boot data and protected mappings

A portable Multiboot2 parser copies bounded memory-map records and reservation
intervals. It rejects undersized/truncated tags, bad alignment/strides, unsupported
map versions, wrapping physical ranges, ambiguous overlapping maps, malformed
module bounds/strings, missing/duplicate maps and absent terminal tags. Unknown
memory types are never allocatable. Fixed-capacity overflow returns a failure;
output remains invalid. The image must be ordinary RAM. Low 1 MiB, the complete
image (including BSS page tables/stacks), boot block and modules stay reserved.
The parser retains no loader pointers. The architecture validates the loader
address against the bootstrap map before reading; it cannot prove faulty firmware
has provided physically backed memory merely from the mapping. A bad access is a
fatal diagnosed exception, not recoverable firmware parsing.

Fresh 4 KiB supervisor page tables identity-map complete type-1 RAM pages from
1 MiB up to a deliberate 1 GiB implementation limit. Holes and reserved firmware
ranges remain unmapped. Kernel text is read-only/executable, rodata read-only,
and writable RAM non-executable when NX exists. CR0.WP enforces supervisor writes;
EFER.NXE is enabled before installing NX entries. There are no writable aliases
for code. The bootstrap, double-fault and NMI stacks have unmapped leading guards.
On NX-absent CPUs the same read-only/guard checks apply, but data non-execution is
explicitly unavailable. This is an early identity-mapped kernel, not KASLR or a
user/kernel split. Page-table metadata itself remains writable by trusted kernel
code; no privileged module isolation is claimed.

Validation: 20,000 deterministic parser mutations plus explicit truncation,
overflow, overlap, module and reservation cases pass ASan/UBSan. Local leak
checking is disabled (`ASAN_OPTIONS=detect_leaks=0`) because the execution sandbox
cannot inspect process threads; address and undefined-behavior checks stay active.
CI keeps default leak checking. CPU fault tests observe code-write #PF (error 3),
guard read #PF (0), data execute #PF (17), and a real stack exhaustion double fault
using the emergency IST. NX-absent normal boot also passes.

Source: GNU Multiboot2 specification, Boot information format, memory-map and
module tags: https://www.gnu.org/software/grub/manual/multiboot2/html_node/Boot-information-format.html

## M2: physical allocation and a bounded heap

`je_pages` describes only the first GiB with one 16-bit state per 4 KiB page.
An entry is reserved, free, an exact allocation head/length, or a continuation.
Initialization derives free pages exclusively from validated type-1 RAM and
reservations. Allocation searches for a contiguous run, limited to 256 pages.
Failure does not change accounting; frees validate the complete extent before
mutation. Interior pointers, wrong lengths, double frees, unaligned addresses and
reserved pages are rejected. Total free memory does not imply a contiguous run.
No compaction, buddy allocator, NUMA, high physical memory or reclaim exists yet.

The first heap intentionally uses one contiguous page extent per object rather
than claiming a slab allocator. It supports 64 live objects, each 1 byte through
1 MiB, with separate trusted allocation records. This trades memory efficiency
for simple ownership and rollback. Every successful extent is zeroed, including
padding, before returning it. A failed mapping returns the pages to the allocator.
Free identifies an exact returned pointer before dereferencing metadata. Freed
memory is not scrubbed immediately; it is cleared before its next heap exposure.
Raw page allocations are not zeroed by the portable allocator; the thread-stack
consumer explicitly clears its pages before use. Future userspace grants must do
the same. Stale pointers after address reuse are not detected by this kernel heap.

Portable algorithms require external serialization. `je_kalloc`/`je_kfree`
disable and restore interrupts around the whole transaction on the BSP. This is
not an SMP lock. Large zeroing/scans can extend interrupt latency; hard real-time
claims are not supported. IRQ handlers do not allocate or call these wrappers.
Page-table guards are changed only while interrupts are disabled; INVLPG updates
the BSP. There is no remote TLB shootdown or concurrent page-table mutation.

Hosted tests cover exact frees, reserved-page protection, full exhaustion,
fragmentation despite enough total memory, zero-on-reuse, mapping failure rollback,
invalid sizes/pointers and heap metadata capacity. The emulator workload performs
allocations from independently preemptible threads and checks content/accounting.

## M2: execution and scheduling

The legacy dual 8259 PIC is remapped to vectors 32–47. All lines remain masked
except IRQ0. PIT channel 0 runs mode 2 at approximately 100 Hz. This is a tick
source, not a calibrated wall clock, real-time deadline guarantee, APIC driver,
SMP system or arbitrary hardware interrupt framework. Unexpected vectors remain
fatal; comprehensive spurious IRQ/device recovery is future platform work.

A fixed table has eight slots: bootstrap/idle monitor at 0 and seven worker slots.
States are EMPTY, RUNNING, READY, SLEEPING and ZOMBIE. The timer handler saves the
current trap-frame pointer, wakes due sleepers, places a running task back in the
ready set, and chooses the next ready slot round-robin. It acknowledges IRQ0
before the frame switch. The assembly epilogue restores another task's registers
and IRETQ frame. Kernel stacks have 16 KiB usable space plus one unmapped guard.
Synthetic first frames enter a C trampoline with the AMD64 SysV stack alignment
and a fallback return address. Thread callbacks return through `je_thread_exit`.

Yield/sleep use kernel-only interrupt vector 0x81. Duration zero yields; positive
duration records an absolute tick deadline under interrupt exclusion. Overflow
is rejected before state mutation. The sleeping thread's interrupt return restores
its disabled state; it reenables interrupts only when the original call had them
enabled. IRQ handlers never sleep. Exit marks the task ZOMBIE and switches away;
stacks are reaped only from bootstrap after every worker has exited. Guards are
remapped before physical pages return to the allocator. Reaping an active set is
busy. No cancellation, priority classes, general wait queues, mutexes, user
credentials, cross-CPU migration or SIMD/FPU context management exists yet.

The idle loop tests completion with interrupts disabled, then executes STI/HLT
as one adjacent instruction pair. This avoids the usual check-then-sleep lost
wakeup window. All scheduler state changes are serialized on this one CPU;
creation/init/reap are caller-restricted to interrupt-disabled bootstrap context.
The completion count uses a C11 atomic. Tick wrap is treated as an invariant
failure instead of silently breaking absolute wake deadlines.

### Boot workload and success contract

The normal development kernel runs a diagnostic workload then halts:

1. Fill all seven worker slots and reject an eighth without consuming pages.
2. Each worker allocates zeroed memory and writes a distinct pattern.
3. An assembly loop holds/checks all 15 ordinary GPRs while waiting for timer tick
   64. It never yields inside that loop, so progress requires hardware interrupts.
4. Each worker checks sleep-overflow refusal, yields, sleeps for a distinct delay,
   verifies the wake time and its retained heap bytes, frees memory and exits.
5. Bootstrap requires multiple dispatches for every worker and at least 16 timer
   context switches, reaps all stacks, and compares free pages with the start.

A success marker alone is not used to infer scheduling: it is reached only after
these checks. Timeout, unexpected fatal marker, incorrect CPU profile, missing
vector/error pair or accounting mismatch fails the external harness. This workload
is kernel self-test/demo code; it is not an application manager or isolated tasks.

Primary timer reference: Intel Core M Platform I/O Datasheet, legacy interrupt
controller initialization and 8254 timer register descriptions:
https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/core-m-processor-platform-i-o-datasheet.pdf

## Remaining boundaries before M3

M1/M2 complete the scoped BSP foundation gates, not every hardening goal in the
long-term design. There is no secure entropy service, compiler stack-canary setup,
heap quarantine, KASLR, stack-clash protection, user-copy recovery, process address
space, handle generation system or capability enforcement. Fatal kernel corruption
stops the entire machine. One-page guards do not detect arbitrary stack jumps.
The low-level physical allocator and page mapping APIs assume trusted callers.
Before launching untrusted code, establish ring-3 entry, a user/kernel mapping
split, validated copy primitives, per-process lifetime/authority rules and vetted
hardening prerequisites. Do not present seven ring-0 threads as sandboxed apps.

The M3a userspace-boundary contract is now frozen in
[ADR 0007](adr/0007-m3a-userspace-boundary.md) (accepted 2026-10-17): 2 MiB
per-process arena, NX-required launch, enforced no-FPU/SIMD policy, experimental
ABI v0, user-copy fixup discipline and proof-by-test acceptance.

## M3a #3: hosted per-process VM builder and bounded image loader

Issue #3 implements the address-space builder and bounded built-in image loader
as a HOSTED-ONLY model behind an injected backend (`je_vm_ops`); see
`include/jeios/vm.h` and `kernel/core/vm.c`. It proves, in host-compiled tests
under ASan/UBSan, the ownership/alias/rollback/admission logic the real #4 arch
backend will drive — it does NOT exercise real paging.

- `je_vm_image_validate`: pure D8 validation — version, per-segment permission
  bitmask (R|X→code, R→rodata, R|W→data; rejects 0, unknown bits, W|X,
  X/W-without-R), page-aligned target/size, subtraction-based source/arena bounds
  (no pre-rounding wrap), page-granule overlap, stack/guard exclusion, an
  underflow-safe ≤64 user-page budget, and entry inside an RX segment's source.
- `je_vm_build`: NX gate (D2), 2-process limit, atomic underflow-safe admission
  (tables + user + the concrete 5-page kstack run + kernel reserve), then a
  journalled build in D3 publication order (zero → copy → unmap loader alias →
  demote identity alias RO/NX → flush → install leaf PTE for USER_CODE/RODATA;
  install RW/NX for USER_DATA). Any failure unwinds the journal in reverse.
- `je_vm_destroy`: teardown as the exact reverse (clear PTE → flush → restore
  alias → free), releasing the kstack run.
- `je_vm_audit`: arena-bounded walk asserting no present leaf outside the arena,
  no guard-page mapping, each leaf owned by a USER_* extent with a kind-matching
  perm and the correct identity-alias state (RO_NX for code/rodata, RW for data),
  completeness and one-to-one mapping, and cross-process root/extent disjointness.

Verification (hosted, at this change): `ASAN_OPTIONS=detect_leaks=0 make check`
adds `test_vm` PASS — validation, admission, audit, permissions,
zero-before-expose, the fail-at-every-step rollback proof (exact `je_pages`
restoration), and teardown. `vm.c` also compiles into the kernel ELF under the
freestanding flags. Not implemented/claimed here: real CR3/TLB, CPL3 execution,
U/S or NX hardware enforcement, GDT/TSS/syscalls — all issue #4 in QEMU. No
independent human qualification; remote CI observed separately.

## Standards status — 2026-10-16

| Capability | Status | Next evidence |
| --- | --- | --- |
| POSIX.1-2024 environment | Planned core requirement; no runtime conformance | Issue #15 profile/ADR, then native staged tests |
| LDAPv3 directory integration | Planned core requirement; no provider/login | Issue #16 contract/lab plan, then qualified native broker |

See [STANDARDS.md](STANDARDS.md). This documentation update changes no executable
behavior; M1/M2 remains the implemented baseline.
