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

## M3a #4 step 2 — real per-process page tables + CR3 (CPL0) — 2026-10-17

`kernel/arch/x86_64/vm_backend.c` implements the real `je_vm_ops` seam against
actual page tables, and `paging.c` gains the kernel-share / user-alias / CR3
seam (`je_kernel_pd_phys`, `je_user_alias_set/get`, `je_arch_cr3_read/load`,
`je_arch_kernel_root_phys`). The #3 builder now drives real hardware: each
process gets a private PML4/PDPT/PD/PT (one TABLE extent) wired
`PML4[0]→PDPT→PDPT[0]=shared kernel PD (U clear)→…` and `PDPT[1]→PD[0]→PT`
mapping only the 2 MiB arena. Intermediate entries carry W|U (effective write
permission is the AND of W across levels under CR0.WP); leaf bits decide the
real per-page permission (CODE=RX, RODATA=R/NX, DATA=RW/NX). `out_root` is the
PML4 physical, a valid CR3 (`_Static_assert` pins frames < 1 GiB).

Owner-approved tested-code changes: `je_arch_page_guard` now refuses any leaf
that is not the exact allocator-default RW pattern (masking CPU Accessed/Dirty),
so the guard path can no longer silently un-demote a published RO_NX user alias
(ADR D3); the same A/D mask applies to the user-alias CAS.

Verified in QEMU (this env): `make check` PASS, `make smoke` PASS — mode 0 boots
through `M3a-s2: process built; audit ok; root distinct` / `arena mapped under
proc root; data RW; kernel root restored` / `process destroyed; page accounting
restored`. `make integration` PASS — all 9 fault modes, including the two new
step-2 probes: mode 8 (read arena under the kernel root) → `#PF err 0`
`cr2=0x40000000` (arena genuinely unmapped in the kernel root); mode 9 (write
the USER_CODE page under the process root) → `#PF err 3` `cr2=0x40000000`
(RX leaf is read-only; CR0.WP enforces it even at CPL0).

SCOPE / not claimed here: CPL0 construction and CPL0 CR3 load only. NO ring-3
execution, NO `sched.c` CR3 switch, NO TSS.RSP0 — those are step 3 and proven in
QEMU there. CPL0 cannot prove U/S ring-3 enforcement, user-mode NX fetch, or
scheduler address-space switching; this step does not assert them. AI-assisted
peer review only; not independent human qualification.

## M3a #4 step 3 — CPL3 entry/return (real ring-3 execution) — 2026-10-17

The scheduler now enters ring 3. `sched.c` gains a thread kind (kernel/user); a
user thread binds a vm process's CR3 and a per-process TSS.RSP0.
`je_thread_create_user` synthesizes the initial ring-3 trap frame (GPRs zero,
cs=0x33, ss=0x2b, rip=arena+entry, rsp=arena top, rflags forced safe) on the
process's held kernel stack and guards that stack's first page. `je_sched_interrupt`
validates the incoming frame by thread kind, and for a user next-thread runs the
pure validator `je_uframe_check` (`kernel/core/uframe.c`, hostable + unit-tested)
to re-validate+**sanitize** RFLAGS every dispatch (force IF=1/IOPL=0, clear
NT/TF/RF/VM/AC/ID/DF), then sets TSS.RSP0 and loads the process CR3 before the
one shared IRETQ. Kernel next-threads get RSP0=0 (fail-closed) + the kernel root.
`je_sched_reap` excludes user threads (their kstack is vm-owned, freed by
`je_vm_destroy` — avoids a double-free); `je_sched_drop_user` detaches a user
thread and restores its kstack guard before teardown. D7 (`je_arch_cpu_policy_init`)
sets CR0.EM/MP/TS and clears CR4.OSFXSR/OSXMMEXCPT/OSXSAVE/FSGSBASE before any
user thread, and normalizes DS/ES/FS/GS to null (no-TLS, ADR D6). Gates stay DPL0
(the D6 `int 0x80` gate lands in step 4 per the amendment).

Verified in QEMU (this env): `make check` PASS (adds `test_uframe`:
bounds/selectors/canonical/rflags-sanitize), `make smoke` PASS — a built-in
ring-3 program (`incq [heartbeat]; jmp`, assembled in `user_demo.S`) reached CPL3,
survived >=16 timer preemptions and advanced its heartbeat ~9-17M times across
runs, then was torn down with restored page accounting. `make integration` PASS —
10 fault modes; new mode 10 launches a user task that executes `xorps` at CPL3 and
observes `#UD` (vector 6) from a `cs=0x33` frame, proving D7 denies SIMD to ring
3 by **hardware**, not a compiler flag. All three CI smoke profiles pass locally
(no-NX cleanly skips step 3 per D2); M2 markers unchanged (owner sign-off
condition).

SCOPE / not claimed here: ONE built-in trusted task, timer-only. NOT claimed:
inter-process isolation, fault-local offender death (a user fault still PANICs
until step 4), the `int 0x80` gate, syscalls, kernel-unreachable-from-ring3 as a
positive claim, SMEP/SMAP, SMP, remote CI, independent human qualification.

## M3a #4 step 4 — fault-local death + D6 int 0x80 gate + H1 fix — 2026-10-17

#4's core acceptance. A synchronous USER fault, a user `int 0x80`, or
bad-return-state terminates ONLY the offending process; kernel + peers survive.
`je_trap_dispatch` routes user-origin faults in an explicit terminate-list (and
user int 0x80) to `je_sched_fault_terminate`, which marks the current user thread
DYING and returns a live successor's frame (NMI/#DF/#MC/reserved + kernel-origin
faults stay fatal). The dying task's kstack/CR3 are freed later by the bootstrap
reaper `je_sched_reap_dead` (asserts IF=0; not-current / not-active-CR3 /
not-active-RSP0 guards), never inside the handler (the iretq epilogue still runs
on that RSP0 kstack). **Security fix (H1):** `je_sched_interrupt` previously
halted the whole kernel (`abort_scheduler`) when a next user thread's saved frame
failed validation — a userspace DoS; now that thread is terminated and another
candidate chosen. The D6 gate is `idt[0x80]=0xee` (DPL3, IST=0, interrupt gate),
the only user-reachable gate; no syscall handler yet, so user int 0x80 is an
unsupported op routed to death. `vm.c` is untouched: DYING/REAPABLE are modeled
in the scheduler slot; the vm record stays READY until `je_vm_destroy`.

Verified in QEMU (this env): `make check` PASS, `make smoke` PASS — the boot runs
the two-task proof (A dies by user #PF vector 14 / cs=0x33 / CR2=unmapped arena
page; B proven to advance AFTER A's death; A reaped from bootstrap while B lives;
user int 0x80 and bad-RSP(H1) death causes each exercised fault-locally with B
alive; B torn down, page count back to the pre-step-4 baseline). `make
integration` PASS — 10 modes; mode 10 flipped: user #UD now terminates the task
(fault-local), kernel halts normally. All three CI smoke profiles pass locally;
M2 markers unchanged.

Invariants recorded: single-CPU, PCID off, IF=0 in the fault handler — SMP would
need a per-CPU off-stack handshake before freeing a dying kstack. NOT claimed:
two-task isolation qualification, syscalls/ABI, copy-in/out, a process exit path,
general/complete reclamation across all paths, SMP. AI-assisted review only; not
independent human qualification.

## M3a #5 step (a) — v0 syscall dispatcher — 2026-10-17

The DPL3 `int 0x80` gate (installed in #4) now dispatches a real experimental v0
syscall instead of terminating. `kernel/core/syscall.c`: `je_syscall_decode` is a
pure op-decoder (unit-tested) and `je_syscall_dispatch` is the kernel glue.
`kernel/arch/x86_64/trap.c` routes a user-origin `int 0x80` to the dispatcher
before the fault-termination path (kernel-origin 0x80 stays fatal). ABI
(`include/jeios/abi.h`, `<stdint.h>`-only, never `status.h`): RAX=op in /
`je_abi_status` out {OK=0, UNSUPPORTED=1, INVALID=2}; RDI..R9 args; RDX optional
scalar result. Ops: `abi_query(which)` → version/profile in RDX; `yield()` →
reschedule via the existing kind-aware path, writing RAX=OK before switching;
`exit(code)` → `je_sched_exit_current` (DEATH_EXIT + code). Unknown op →
UNSUPPORTED (full 64-bit op decode, so `0x1_0000_0000` ≠ op 0); a recognized op
with a nonzero reserved arg → INVALID with no side effect.

Every non-switching syscall return goes through `je_sched_return_to_user`, which
re-runs `je_uframe_check` (validate + sanitize RFLAGS) on the current frame and
terminates (DEATH_BAD_RETURN) rather than IRETQ a bad frame — closing the gap
where a direct syscall return would skip the "validate on every user return"
rule. The step-4 death record gained a `reason {FAULT, EXIT, BAD_RETURN}` +
`exit_code` (owner-approved tested-code touch; the `0xbad` magic vector removed);
`je_sched_fault_terminate` kept unchanged (reason=FAULT).

Verified in QEMU (this env): `make check` PASS (adds `test_syscall`:
abi_query/yield/exit/reserved-arg/full-width-op/unsupported, each asserting the
RDX result, + a `make check` grep enforcing abi.h never includes status.h),
`make smoke` PASS — the boot proves, with peer B live throughout: `exit(42)` →
clean DEATH_EXIT code 42 via the gate; `abi_query` returned OK+version and
`yield` returned OK (read back from the user's data page, so the result reached
ring 3); an unknown op → UNSUPPORTED and the task **survived**; a bad frame
presented on the **syscall return path** → BAD_RETURN death (validated there, not
only at the next timer dispatch). `make integration` PASS — 10 modes. All three
CI smoke profiles pass locally; M2's 4 markers unchanged.

NOT claimed (step (b)): `copy_from_user`/`copy_to_user`, `copy_probe`, the
exception-fixup copy sites, or any user-pointer handling — no step-(a) op takes a
pointer. No stable ABI promise. AI-assisted review only; not independent human
qualification.

## M3a #5 step (b) — recoverable user copies + exception fixup — 2026-10-17

The final M3a piece: `copy_from_user`/`copy_to_user` (ADR D9) with prevalidation
and an exception-fixup net, plus the dev-only `copy_probe` syscall (op 3).
`kernel/core/usercopy.c` + `kernel/arch/x86_64/usercopy.S`.

Every copy prevalidates the user span before touching it: canonical, no
wraparound, entirely inside the arena (never a kernel range), and — per page —
present with the required permission (`read_user_pte`; writes need RW/NX).
Zero-length touches neither pointer. The copy itself runs through ONE approved
assembly site (`je_copy_user_run`); its two faulting instructions (load, store)
have exact RIPs registered with the #PF handler. A per-thread copy scope
(`active`, direction, `[lo,hi)` span, owner vm+index) is set before the copy and
cleared on EVERY exit path.

`je_usercopy_try_fixup` (called from `je_trap_dispatch` on a `cs==8` vector-14
fault, before the fatal path) recovers ONLY when: PFEC is not-present (P=0) and
not a protection/reserved/fetch fault; the faulting RIP is EXACTLY one of the two
approved copy instructions (never a range match); a scope is active; CR2 is inside
the declared span; the owner is the current user process; and the PFEC write bit
matches the direction (load site↔read fault, store site↔write fault). Anything
else keeps the fatal PANIC. A recovered fault returns JE_ABI_INVALID — a
prevalidated copy should not fault in M3a, so a recovery indicates a validator/
scope bug and is reported as a copy failure, not masked. A *protection* fault is
deliberately NOT recovered (it would hide an invariant break).

`copy_probe(src,dst,len)` op 3 (dev tool, ≤256 B): stage src→kernel scratch via
`copy_from_user`, then `copy_to_user`→dst; accepts overlap; zero-len = OK/no-op;
any adversarial span → INVALID with no copy and no output commit.

Verified in QEMU (this env): `make check` PASS (test_syscall incl. the op-3
decode + an assertion the dev-inject symbols are ABSENT from the default ELF),
`make smoke` PASS — the boot runs the copy_probe battery (valid + zero → OK;
null / kernel-range / len>256 → INVALID; the valid copy's bytes verified moved),
with peer B live. `make integration` PASS — 11 modes; the new mode 11 (built with
`-DJE_DEV_COPY_INJECT`, which the default kernel never gets) raw-copies a
deliberately unmapped arena page and proves the fixup RECOVERS the copy-site #PF
(kernel survives), while the pre-existing kernel-fault modes (4/5) still PANIC —
confirming an unrelated kernel #PF is NOT wrongly recovered. All three CI smoke
profiles pass; M2's 4 markers unchanged.

Honest limits (recorded): SMAP/SMEP are not enabled in M3a, so the fixup net
covers faults from the approved copy site only, not stray kernel dereferences of
mapped user addresses; "never recovers an unrelated fault" holds under the stated
invariants (uncorrupted kernel state), not against arbitrary prior corruption.
This closes the ADR 0007 M3a userspace boundary. AI-assisted review only; not
independent human qualification.

## M3b first PR — pure typed handle table — 2026-10-17

First implementation step of M3b (ADR 0008, Option A): a pure, process-local,
bounded, typed handle table. `kernel/core/handle.c` + `include/jeios/handle.h`,
standalone — **no syscalls, no object destruction, no kernel wiring, no M3a path
touched** (the module compiles into the kernel under the strict flags but is not
yet called). Exercised entirely by the hosted `test_handle`.

Handle = `uint32` (production 16 index / 16 generation); value 0 is the invalid
null; slot generations start at 1 so slot 0's first handle is nonzero; the width
is a compile-time parameter with the production 16/16 split pinned by
`_Static_assert` (plus asserts that each field is in [1,31] and the pair sums to
32). Slots run a `FREE/LIVE/RETIRED` state machine; `lookup` checks state before
generation; `close` bumps the generation or, at `GEN_MAX`, **retires** the slot
(never wraps — retired capacity is permanently lost); `alloc` takes the lowest
FREE index (deterministic internal policy), requires a non-NULL `out`, and reports
`QUOTA` (live at quota) in precedence over `NOSLOTS` (no FREE slot). The security
claim is stale-name protection, NOT an unforgeable capability; no SMP claim; sound
under the UP/IF=0 precondition.

Verified in this env (ASan+UBSan hosted): `make check` PASS including
`test_handle` at the **production 16/16** width (encode/decode, alloc/lookup/close,
forged/stale/wrong-type/denied, QUOTA precedence, `table_init` rejections, the
non-NULL-`out` rule) **and** `test_handle_rw` at a **reduced width** (`gen_bits=2`)
that exhaustively drives generation **saturation → RETIRED**, retired-capacity
loss, and `NOSLOTS`-via-retirement. The full M3a suite is unchanged — 11
integration modes, M2's 4 markers, the ELF/ABI/dev-inject assertions — and all 3
CI smoke profiles pass, as expected since the module is not yet wired in.

Peer cross-review (two independent reads) found no correctness defect in the
production build and drove two hardening fixes applied here: the degenerate-width
`_Static_assert` (a 0-width field would be `1u<<32` UB) and rejecting a NULL
`alloc` out (which would otherwise consume a LIVE slot whose handle is lost), plus
the `table_init` rejection-path tests. Deferred per ADR 0008 to later PRs: typed
rights checking on use, attenuation + references, and the owner-gated process-death
teardown wiring. AI-assisted review only; not independent human qualification.

## M3b second PR — handle table wired + first handle syscalls — 2026-10-17

Wires the (unchanged) M3b handle table into the live kernel and adds the first
three handle syscalls: `handle_self` (op 5), `handle_close` (op 6), `handle_query`
(op 7). The per-process table is now inline in `je_vm_process` (`JE_VM_HANDLE_CAP`
= 16 slots, quota 15), initialized in `je_vm_build` just before the process is
published READY. **This supersedes the first-PR "no syscalls / no kernel wiring"
status for the wiring itself** (the pure table in `handle.c` is unchanged).

The only nameable object in M3b is **SELF**: a kernel-minted, immortal,
resource-free process-identity capability (`JE_ABI_OBJ_SELF`), carrying no
dereferenceable pointer. `handle_self` mints a SELF handle in the caller's own
table; `handle_close` closes one; `handle_query` returns the object type.

ABI (ADR 0008 D2, settled here): distinct `JE_ABI_H_*` statuses (STALE=3,
BADINDEX=4, WRONGTYPE=5, DENIED=6, QUOTA=7, NOSLOTS=8) are appended to `abi.h`
and pinned with `_Static_assert`; an exhaustive mapper from the internal
`JE_HANDLE_*` lives in the syscall glue, so `abi.h` stays independent of both
`status.h` **and** `handle.h` (the `make check` isolation grep now enforces both).
A wide handle (RDI bits 63:32 set) is a **malformed call → `JE_ABI_INVALID`** at
decode, distinct from a well-formed-but-bad handle → BADINDEX (ADR 0008 D2 amended
to say so). The process is resolved ONLY via `je_sched_current_user`, never from a
syscall argument; a non-user caller gets INVALID.

Reclamation needs **no fault-death-path edit**: the table lives in the struct the
existing `je_vm_destroy`/build-unwind already zero (`*p = {0}`), and a zeroed table
has capacity 0 so every lookup fails BADINDEX and every alloc fails QUOTA — fail
-safe even if init never ran. `je_vm_build` re-inits before READY, so a reused
process slot cannot carry a stale LIVE handle. SELF owns no external resource, so a
faulted-but-not-yet-destroyed process holding one is inert.

Verified in this env: `make check` PASS (hosted `test_syscall` now covers the
self/close/query decode incl. the wide-handle INVALID and reserved-arg rules;
`test_vm` links `handle.c`; both handle hosted tests pass). `make smoke` PASS with
a new `M3b-s1` marker from a ring-3 stub that mints SELF, queries it (type==SELF),
closes it, and asserts the full matrix: query/close-closed → STALE; handle 0 and
index==capacity → BADINDEX; wrong generation → STALE; wide handle → INVALID; slot
reuse mints a new handle AND the OLD handle re-queries as STALE (the real
reuse-invalidation proof); and the mint loop ends at QUOTA after **exactly 14**
further mints (quota 15 minus the live self#2) — a measured count, not just "ended
at QUOTA". 11 integration modes, M2's 4 markers, and all 3 CI smoke profiles
unchanged.

Scope honesty: the `M3b-s1` run exercises ONE process's table; it does not by
itself prove tables are *per-process* (that holds by construction + the hosted
vm/table tests), and the marker/comments say so rather than overclaiming.

REVISIT (owner-approved for this PR, recorded here): (1) `je_vm_process` grew by
an inline handle table — watch the struct-size budget as object kinds and the
handle count grow. (2) `handle_query` returns **type only**; rights are carried
internally but **NOT enforced and NOT exposed** in M3b — exposing a frozen rights
layout waits until a real rights-enforcing operation exists. (3) `NOSLOTS` is
reachable from ring 3 only via generation saturation of every slot (a bounded
self-DoS of the caller's own table over ~1M syscalls), not truly "unreachable";
it is covered exhaustively by the reduced-width hosted test.

Peer cross-review (two independent reads) found no identity / slot-reuse / ABI-
mapping / decode defect, and drove the evidence hardening applied here (the
re-stale-after-reuse assertion, the measured quota count, the softened per-process
wording, the ADR wide-handle amendment, and the pinned ABI static-asserts).
AI-assisted review only; not independent human qualification.

## MP1 step 1 — ARM64 entry + bounded console (QEMU virt-8.2) — 2026-10-17

First *executable* ARM64 work (ADR 0002, jeios-mobile). The shared kernel core
stays in this repo per ADR 0001; this adds a second architecture under
`kernel/arch/aarch64/` (`boot.S`, `main.c`, `linker.ld`), built by a separate
`mk/aarch64.mk` (targets `make aarch64`, `make smoke-aarch64`) that leaves the
x86-64 build/check/smoke/integration flow completely untouched. Scope is entry
only: **no VBAR/vectors, no DTB parse, no MMU** (those are later MP PRs).

On the QEMU-validated `virt-8.2` / `cortex-a72` target (direct `-kernel`, no
firmware), `boot.S`: parks x0(DTB) in x19, masks DAIF, asserts **EL1** (FATAL
marker + halt otherwise), **self-checks the load address == link address**
(text_offset=0x80000 → 0x40080000; FATAL if QEMU placed us elsewhere, so a future
absolute reference can't silently be wrong), selects SP_EL1, writes **TCR_EL1 =
0x80000000** (RES1 bit31 set for cortex-a72, TBI0=0 interim per ADR 0002 D5 — a
field init, NOT a tag-rejection claim), clears BSS with aligned stores (no DC
ZVA/atomics — pre-MMU Device-nGnRnE memory), stashes the DTB pointer into `.data`,
and enters C. `main.c` drives a bounded PL011 (DR-write + bounded TXFF poll, no
init — a documented virt-8.2 support-tier assumption) and emits ordered markers.

Built `-mstrict-align -mgeneral-regs-only -ffreestanding -fno-builtin` so the
compiler emits neither unaligned nor FP/SIMD code (a stray SIMD op would trap
into absent vectors and hang). Image header: text_offset 0x80000, flags **0x1**
(4 KiB pages, LE), magic, `__image_size` from the linker. Linker asserts keep the
stack+BSS 16-byte aligned, after the file-backed image, within `__image_size`,
non-overlapping; `--orphan-handling=error` forbids any unplaced section landing
in the image.

Verified in this env: `make aarch64` + `make smoke-aarch64` PASS with the ordered
markers — `entry EL1` / `stack ready sp=0x400844e0` (in the linked range, proving
load==link) / `tcr_el1 initialized (tbi0=0) tcr=0x80000000` / `bss range [...]` +
`bss cleared` / `dtb pointer recorded dtb=0x48000000` / `initialization complete;
halted`. The `ARM_BSS_CHECK=1` build (separate build dir) pattern-fills BSS before
the production clear and prints **`bss clear verified`**, so the clear is proven
to zero rather than relying on QEMU's already-zero RAM. The smoke harness
(`tools/smoke_arm.py`) captures serial to a FILE (stdout races the timeout kill),
asserts exact ordered markers, rejects `FATAL`, records image SHA-256 + tool
versions + the exact command, uses `-no-reboot`, and is NOT wired into
`make check`. The x86-64 side is unchanged — `make check` + smoke + 11 integration
modes still pass.

Not yet enforced (handoff): the ADR 0002 D5 trap-frame fields (ESR/FAR/ELR/SPSR),
VBAR exception vectors + the deliberate-fault test, the TBI behavioral (tag
rejection) test, DTB *validation* (the pointer is recorded, never dereferenced),
and the MMU. Peer cross-review (two independent reads) found and drove real fixes
the smoke had passed over: the Image flags (0x2→0x1), TCR_EL1 RES1 (0→0x80000000),
the load≠link latent hazard (now a boot-time self-check), the vacuous empty-BSS
marker (now a real probe + range), `_file_end` placement, `--orphan-handling`,
and the DAIF-by-construction mask. AI-assisted review only; not independent human
qualification.

## M3b third PR — rights ABI exposure + enforcement policy hook — 2026-10-18

Freezes the handle **rights ABI** (ADR 0008's deferred revisit) and wires rights
enforcement on a live syscall path, WITHOUT attenuation/references (next PR) and
without touching M3a's fault-death path. Rights are a fixed `uint32_t` mask;
`JE_ABI_RIGHT_INSPECT = 0x1`, `JE_ABI_RIGHTS_KNOWN = INSPECT`, defined in `abi.h`
and pinned with `_Static_assert` (the glue maps them; `abi.h` stays independent of
`status.h` and `handle.h`). SELF is now minted with INSPECT (a kernel constant,
compile-asserted to contain only known bits). New op **`handle_rights` (op 8)**:
`lookup` requiring INSPECT, returns the slot's rights mask in RDX **masked to
`JE_ABI_RIGHTS_KNOWN`** so no internal bit can leak. `handle_query` (op 7) stays
type-only; `handle_close` requires no right.

A pure accessor `je_handle_rights_of()` was added to the handle API (same validity
checks as lookup, returns the rights mask) — additive, the table stays ABI-
agnostic.

**HONESTY — this is a rights ABI + policy HOOK, not end-to-end enforcement, and
the docs/markers say so.** Because the only ring-3 minting path (`handle_self`)
always grants INSPECT, ring 3 can observe success and the mask but **cannot reach
DENIED or WRONGTYPE** yet. Those branches are exercised by the hosted tests only.
Genuine ring-3-observable denial arrives with **M3c attenuation** (which can mint a
rights-reduced handle) — the planned **(B)/A+B follow-up**, chosen by the owner:
do the ABI-exposure hook now (A), make enforcement genuinely demonstrable when
attenuation lands (B/A+B).

Verified in this env: `make check` PASS — hosted `test_syscall` covers the op-8
decode (reserved-arg + wide-handle rejection), `test_handle` exercises
`je_handle_rights_of` including the **DENIED** (rights-0 slot) and **WRONGTYPE**
branches that ring 3 can't reach in M3b. `make smoke` PASS with the updated
`M3b-s1` marker — a ring-3 task calls `handle_rights` and gets OK + mask == INSPECT
(`0x1`). 11 integration modes, M2's 4 markers, all CI profiles unchanged; the
ARM64 build/smoke unaffected. Peer design pass (two-round debate) drove the honest
"hook not enforcement" framing and the output-masking/compile-assert hardening.
AI-assisted review only; not independent human qualification.

## M3b fourth PR — attenuation (handle_dup); rights enforcement made real — 2026-10-18

Adds handle **attenuation** and, with it, makes rights enforcement genuinely
ring-3-observable (the "B/A+B" payoff from the rights-hook PR). New op
**`handle_dup` (op 9)** and pure primitive `je_handle_dup_self`: derive a NEW
handle to the same object as the source with a rights **subset**. Owner decision:
landed as **"Refs #7", not "Closes"** — #7 stays open for the reference-count /
overflow / instance-generation work that belongs to M3c (it needs a destroyable
object, which M3b's immortal SELF is not).

Semantics (frozen in the ADR 0008 amendment): **fresh-slot** — the derived handle
gets its own slot/index/generation and independent `close`, and counts against
quota (not a re-label of the source slot). **SELF-only** — the primitive copies
the binding with no retain/refcount (sound only because SELF owns nothing); every
other type is `WRONGTYPE`; M3c must add a retain-before-commit path for
destroyable endpoints. **No DUP/GRANT right** — any holder may derive an
equal-or-weaker handle; a superset request is `DENIED` (never escalates); an
**unknown** rights bit is a malformed call → `INVALID` in decode, distinct from a
known-but-unheld right → `DENIED`. `handle_query`/`handle_close` stay **rightless**
so a zero-rights derived handle still learns its type and can be closed (no slot
leak). `lookup`/`rights_of`/`dup_self` all share the one `resolve_slot` helper.

Verified in this env: `make check` PASS — `test_syscall` covers the op-9 decode
(reserved-arg, wide-handle, wide-rights, unknown-rights-bit → INVALID; clean subset
→ DUP); `test_handle` `test_dup` covers fresh-slot distinct handle, subset OK,
**attenuation→DENIED**, superset DENIED, WRONGTYPE, the derived handle surviving
the source's close, independent close, and NULL-out. `make smoke` PASS with the
updated `M3b-s1` marker: a ring-3 task derives a rights-less handle and
`handle_rights` on it returns **DENIED from ring 3** — the real enforcement proof,
not a hosted one. 11 integration modes, M2's 4 markers, CI profiles, and the
ARM64 build/smoke all unchanged.

Design + review via a role-assigned peer debate (implementor vs adversarial
reviewer). Key findings adopted: refcount-on-SELF would be untestable theatre
(so attenuation-only, refcount deferred to M3c); a generic `derive` that copies an
opaque pointer without a retain is a latent UAF API (so SELF-only, type-gated);
the superset test must use a known-but-unheld right (unknown bit is INVALID); and
#7 cannot honestly be closed by this PR. AI-assisted review only; not independent
human qualification.

## M3c lifecycle (endpoint pool + split refcount + retain-on-dup) — 2026-10-18

ADR 0009 accepted; this is **D-SEQ step 2**, the first destroyable, refcounted
kernel object. New hostable module `kernel/core/endpoint.c` +
`include/jeios/endpoint.h`, built on the `handle.c` pattern: includes only
`<jeios/handle.h>` (no `vm.h`/`sched.h`/`abi.h`), allocates nothing (caller
supplies the `je_endpoint_pool`), zero global state. A **channel** is one pool
record holding both ends (`end[2]`), so a peer pointer is always to the same
record and can never dangle. Each end carries a **split refcount** —
`handle_refs` (LIVE handle slots; drives OPEN/CLOSED) and `inflight_refs`
(queued caps + waiters; both empty this PR) — and the record is freed only when
**both ends have both counts at 0**. `JE_EP_REF_MAX=8` is a small configured
policy bound (`_Static_assert JE_EP_REF_MAX >= JE_EP_QUEUE_DEPTH + JE_EP_MAX_WAITERS`),
so `REFOVERFLOW` is reachable in a hosted test; acquiring past it acquires
nothing and leaves the object unchanged (fail-closed, not saturate).

**The table stayed pure** (ADR 0009 D2/D3): `handle.c` is byte-for-byte
unchanged and never dereferences the opaque `object`. All retain/release lives
in `endpoint.c`, which maps a slot's `object` (= `&rec->end[side]`) back to its
end by bounds-checking the pointer into the pool (never deref an unvalidated
pointer; a stray pointer fails `JE_EP_CORRUPT`). The dup path is the endpoint
equivalent of `je_handle_dup_self` but **with retain-before-commit**
(`je_endpoint_handle_dup`): resolve → subset-check (DENIED) → retain
(REFOVERFLOW) → `je_handle_alloc` (QUOTA before NOSLOTS), releasing the retained
ref on every alloc failure. `je_handle_dup_self` **stays SELF-only** (an endpoint
handle through it still returns WRONGTYPE) — exactly as its header demanded.

**Create is kernel-only** (`je_endpoint_channel_create`): transactional (both
mints + the record unwind completely on any failure), used only by the hosted
test. There is **no ring-3 `endpoint_create`** — no `abi.h` op, no decode case,
no dispatch — so no user process can create (then die holding) an endpoint
before the owner-signed death-path release-walk PR (D-OWNER-2) wires reclamation.
That PR also owes the **typed close dispatcher** that makes the load-bearing
precondition true table-wide (today a raw `je_handle_close`/table-zero of an
endpoint slot would *leak* a ref — a leak, not a UAF — which this PR keeps
impossible by minting endpoints only into kernel-local tables, never a process's
own `htable`). `endpoint.c` is compiled into the kernel (links clean, no
undefined symbols) but unreferenced — the intended "kernel-only, no ring-3 route"
state.

**Tests:** `tests/test_endpoint.c` (hostable, ASan/UBSan) proves create, the
split-ref close semantics (first close keeps the end OPEN, last close →
CLOSED + peer PEER_CLOSED, both-ends-zero frees the record and bumps the internal
generation), dup independence, REFOVERFLOW (count + table live-count + out
unchanged), dup rollback on QUOTA, DENIED on a superset dup, WRONGTYPE for a SELF
handle and for `dup_self` on an endpoint, an `inflight_ref` keeping the record
alive past both handle closes, and pool-exhaustion + second-mint-failure unwind.
`make check` / `smoke` / `integration` / the 3 CI NX profiles / the ARM64 build
all green.

### M3c rights-bit discrepancy (owner-approved Option A, 2026-10-18)

ADR 0009 D4a appends the endpoint rights `SEND=0x2`, `RECV=0x4`, `TRANSFER=0x8`
to `JE_ABI_RIGHTS_KNOWN` (INSPECT stays `0x1`). This is an **ABI behaviour
change to previously-tested ring-3 code**: M3b asserted `dup(self, 0x2) → INVALID`
*because 0x2 was an unknown bit*. With `SEND=0x2` now known, that call passes the
decode's known-bit gate and the glue returns **DENIED** (a known right a SELF
handle does not hold). The owner chose **Option A**: accept the INVALID→DENIED
change as correct, update the assertion to its new result, AND keep proving the
unknown-bit→INVALID *contract* on a bit that is still unknown. Concretely:

- `tests/test_syscall.c`: the old `dup(self,0x2)==INVALID` line is retired; it now
  asserts `dup(self, SEND)` decodes to the DUP action (OK at the decoder; the
  DENIED is a glue-level decision) **and** `dup(self, 0x80000000)==INVALID`
  (bit 31, still unknown) re-proves the contract. A HISTORY comment records the
  old behaviour.
- `kernel/arch/x86_64/user_demo.S` + `kernel/core/main.c` (the QEMU `M3b-s1`
  proof): the ring-3 "unknown-right" dup changed from `0x2` to `0x80000000`; the
  marker still prints `unknown-right INVALID`.

Both edits preserve the original assertion's *intent* (an unknown rights bit is
rejected as malformed) on a genuinely-unknown bit, rather than silently deleting
it — the owner asked to keep the old check's meaning visible alongside the new
reality. `0x80000000` fits 32 bits, so it passes the wide-rights check and
reaches the known-bit gate (a `>>32` value would be rejected earlier, also as
INVALID, but for the wrong reason). `JE_ABI_H_REFOVERFLOW=9` was appended to the
ABI status enum (pinned), reserved until the first M3c ring-3 endpoint op maps it.

Design + review via a role-assigned peer debate (implementor vs adversarial
reviewer) on the *implementation seam* (the ADR itself was already frozen). Key
findings adopted: distinct `retain_handle`/`retain_inflight` APIs (one ambiguous
`retain()` invites bumping the wrong counter); the raw-close-unreachable
invariant must be enforced by a typed dispatcher, not by "callers remember the
wrapper" (so create stays kernel-only until that PR); the s5b inject precedent is
a *bad* security analogy (it is a real ring-3 dev op; endpoint create has no
ring-3 surface at all). AI-assisted review only; not independent human
qualification.

## M3c death-path release walk + typed dispatch + ring-3 endpoint_create — 2026-10-18

ADR 0009 **D-SEQ step 3**, the owner-signed tested-death-path PR (the amendment
A3-1/A3-2/A3-3 decisions, both owner-approved). Ships three things together so a
user process can never die holding an endpoint before teardown reclaims it:

**Process-death release walk.** `je_endpoint_release_table(pool, t)` (in the pure
`endpoint.c`) enumerates a dying process's table via a new **read-only** iterator
`je_handle_live_at` (added to `handle.c`, so endpoint code never writes table
internals — table purity preserved) and closes each LIVE endpoint handle through
`je_endpoint_handle_close`, dropping its ref, closing peers, and freeing
now-unreferenced channels. It post-asserts no endpoint slot remains LIVE and
returns <0 on any invariant break; the kernel hook treats that as fatal
(`console_write`+`halt`) **before** any table zeroing — a leak is never zeroed
away into a lie. A SELF handle in the same table is left for the existing zeroing.

**Wired through a generic `je_vm` hook + IRQ critical section (A3-1).** `je_vm`
gained `teardown`/`teardown_ctx` and `irq_save`/`irq_restore`, all zero-defaulted
by `je_vm_init` so `test_vm.c`/`vm_fake.c` are untouched and `vm.c` stays
endpoint- and arch-agnostic. `je_vm_destroy` now, after its existing checks, runs
the hook (the release walk) and `unwind` **inside one `irq_save()/irq_restore()`
critical section**. This is the owner-gated behaviour change: the naive "walk then
unwind at IF=0" plan had a real hazard the peer review caught — the direct
`main.c` destroy callers can run with IF=1, so a timer preempting between the walk
and the `*p={0}` zeroing, while the peer process is live, would let the peer's
HANDLE_CLOSE mutate the same channel record mid-teardown (a UAF). The IRQ save
closes that window. The kernel pool + hook live in a new `endpoint_kern.c` (the
only file with endpoint global state); `je_endpoint_kern_install(vm)` sets the
four fields once.

**Typed close dispatch (A3-3).** Ring-3 `HANDLE_CLOSE` (op 6) now does a read-only
type peek (`je_handle_live_at` + generation match) then an exact branch: ENDPOINT
→ `je_endpoint_kern_close` (drops the ref), else raw `je_handle_close` (SELF).
Atomic because the int 0x80 path runs at IF=0 on one CPU (stated as an explicit
SMP-revisit assumption, not a general proof). No ring-3 op allocates a slot of a
caller-chosen type, and op 9 stays SELF-only, so raw endpoint alloc is
unreachable — the structural discharge of endpoint.h's precondition.

**Ring-3 `endpoint_create` (op 10, A3-2).** Takes no args (all reserved zero else
INVALID), mints both ends into the caller's table with all endpoint rights,
transactional. The v0 ABI's single RDX scalar carries **both 32-bit handles
packed**: `RDX[31:0]=h0`, `RDX[63:32]=h1`, undefined on failure —
`_Static_assert(sizeof(je_handle)==4)` pins the losslessness. It returns
`UNSUPPORTED` on a VM with no teardown hook, so "an endpoint in a ring-3 table ⇒
that VM's destroy walks it" holds by construction (the create wrapper
`je_endpoint_kern_create` enforces the same for kernel callers). `JE_ABI_OBJ_ENDPOINT`
and the endpoint rights bits are `_Static_assert`-pinned equal to the table-side
copies in the glue; pool exhaustion maps to the existing `JE_ABI_H_NOSLOTS`.

**Proofs.** Two QEMU markers extend the fault-death family:
- `M3c-s3a` (**kernel self-test**): one table holds end0, another holds end1;
  releasing the end0 holder leaves end1 `PEER_CLOSED` with the record still
  allocated (survivor keeps it), and the survivor's close then frees it — the
  honest survivor-peer proof (a ring-3 cross-process peer needs transfer, step 5,
  so this is deliberately kernel-only and labelled as such).
- `M3c-s3b` (**ring-3**): a process calls `endpoint_create`, closes h0 via the
  typed dispatch (pool is 1 — h1 keeps the channel), then faults; after
  `reap_dead` the pool is back to baseline 0, held across `JE_EP_POOL_CAP+3`
  fault/reap cycles (no leak exhausts the pool). Scope-honest: it proves
  reclaim + no-exhaustion, not the survivor case.

Hosted coverage added: `test_endpoint` gains release-walk cases (mixed
endpoints+SELF, idempotent re-walk, dup/both-ends-in-dying-table); `test_syscall`
asserts op 10 decodes to `ENDPOINT_CREATE` (op 11 is the new UNSUPPORTED
boundary); and `test_vm` gains a **fake-hook ordering test** proving `je_vm_destroy`
runs the teardown hook while the table is still live (before the `*p={0}`
zeroing), pairs `irq_save`/`irq_restore` exactly once, skips both on the
bad-state early return, and preserves the prior behaviour when the hooks are NULL
(the default everywhere else). `make check` / `smoke` / `integration` / 3 CI NX
profiles / ARM64 all green.

Design + implementation cross-review via a role-assigned peer debate. The IRQ
critical-section hazard and the packed-RDX ABI shape were both escalated to and
approved by the owner (recorded as ADR 0009 AMENDMENT A3-1/A3-2). AI-assisted
review only; not independent human qualification.

## Standards status — 2026-10-16

| Capability | Status | Next evidence |
| --- | --- | --- |
| POSIX.1-2024 environment | Planned core requirement; no runtime conformance | Issue #15 profile/ADR, then native staged tests |
| LDAPv3 directory integration | Planned core requirement; no provider/login | Issue #16 contract/lab plan, then qualified native broker |

See [STANDARDS.md](STANDARDS.md). This documentation update changes no executable
behavior; M1/M2 remains the implemented baseline.
