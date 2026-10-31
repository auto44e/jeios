# Implementation ledger: M1 and M2

Standards adopted as design requirements (2026-10-26): [standards and evidence](STANDARDS.md)
and [organizational ISO 27001 support features](ORG_SECURITY.md). Implementation
and formal qualification remain gated; these documents add no runtime behavior.

Ecosystem scope: [CPU, GPU, APU and NPU architecture support](ARCHITECTURE_SUPPORT.md)
across kernel, drivers, runtimes, SDK, applications and product profiles. Planning
scaffolds do not establish runtime or hardware qualification.

RISC-V: [RV64 scaffold and gated port plan](RISCV.md) added 2026-10-24.
Planning only; no RISC-V execution or hardware support claimed. Existing core
and ARM64 implementation priorities remain unchanged.

Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
current baseline and linked issue dependencies. Future designs remain unimplemented.


This file records current contracts and execution evidence as gates are built.
DESIGN.md describes the broader target architecture. No entry below implies
userspace isolation, SMP safety, driver containment or production readiness.

## Security fix: zero physical frames on free (ADR 0006 note) — 2026-11-01

Closes a cross-process / kernel disclosure: `je_pages_alloc`/`free` never zeroed
frame contents, and the VM backend reused freed frames (including as page tables)
with stale bytes — stale page-table bytes are live PTEs. Fix (owner-approved b+c):
`vb_scrub_frames` zeroes every VM-owned frame run on free in `vb_free_extent` and
`vb_release_hold` (root cause); `vb_link_tables` also clears the four table frames
before publishing (defense in depth); `je_vm_audit` now rejects any present
upper-table entry outside the four the build sets and runs on the ring-3 spawn
path. Hosted regression test: test_vm `test_audit_rejects_stale_upper` (injects
stale entries; verified it fails without the audit strengthening). The `je_pages`
allocator contract is unchanged. Landed as PR #66. Detail and rationale in ADR
0006's security note and `docs/adr/debates/secfix-frame-zero-reviews.md`.

## M4 PR-F Stage 1 — D12 kernel mechanism — 2026-10-31

This worker-stage change implements only the kernel mechanism that a later
supervisor monitor will call. It does not add the supervisor image, monitor
loop, child images, TEST_MODE 12 or the D12 boot marker.

- `je_sched_kill_all_user` now runs only from slot 0 with interrupts disabled.
  Its first pass classifies every non-empty user slot by the thread back-link
  and `je_proc_run_link` identity. LIVE READY/BLOCKED_IPC users other than the
  exact spared identity are marked in a second pass through the unchanged
  `mark_dying` sequence; DYING users are validated and skipped. Invalid
  RUNNING/SLEEPING/ZOMBIE states, generation-zero identities and mismatched
  run links return `JE_ERR_INVALID` without changing any slot or PROCESS
  record. WOKEN reservations need no special branch because `mark_dying`
  remains the single idempotent waiter-release owner.
- `svc_report_read` is a Stage-2-only static helper. It checks that the fixed
  40-byte report stays within one page, probes the process PTE as present
  `JE_VM_PERM_RW_NX` before switching CR3, copies seq/body/seq, restores the
  kernel CR3, and accepts only a stable even sequence plus known fixed-format
  values. No report field becomes a pointer, length or index.
- `report_vaddr_in_rw_segment` is a pure unused predicate for the future
  `JE_IMAGE_SUPERVISOR` image validation hook. It requires exactly one
  non-executable RW segment, page-safe placement, 8-byte alignment and a
  location below the guard page.

**Observed evidence at local commit `b3ae3b7b136346290bda91baadc4909ad203b3fb`
on branch `wip/m4f-impl` (uncommitted worker tree):**

- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS; hosted suite, generated
  image validators, ELF checks and `test_sched_killall: PASS (66 checks)`.
- `make smoke` — PASS; rebuilt the NX ISO and QEMU reached
  `M4e-s1: ring-3 C image OK ...` followed by `QEMU smoke: PASS`.
- `git diff --check` — PASS.

The hosted scheduler test is real production `kernel/core/sched.c` included
inside the test translation unit with only private linkage exposed there;
production `kernel/core/proc.c` supplies PROCESS behavior. Its fakes cover
only hosted interrupt, console/halt and waiter-release hooks. The report
reader and placement predicate remain untested by QEMU until Stage 2 supplies
the supervisor image and monitor. AI-assisted review is not independent human
qualification.

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

## M4 PR-E — ring-3 C image path + host tool `mkimage.py` (ADR 0010 D10) — 2026-10-31

The first real ring-3 C program (`c_hello`) built by a plain host cross-compile,
validated and embedded by a host tool, and booted through the startup-v1 ABI.

**What it does.**
- **`tools/mkimage.py`** converts a user ELF into the checked C descriptor + blob
  the kernel embeds, rejecting anything `je_vm_image_validate` (kernel/core/vm.c)
  would reject plus stricter host-only rules. Its docstring is the **frozen spec**
  (rules E1–E10). Exit 0 accept, 2 reject (`mkimage: reject: <RULE-ID>: `, lowest
  violated rule), 1 usage/IO. Output is deterministic: no path, timestamp or
  hostname reaches the generated `.c`, build id is stripped, and the write is
  atomic (tempfile + `os.replace`) so a reject never touches `OUT.c` and a crash
  mid-write cannot leave a half-file with a fresh mtime.
- **`user/`**: `crt0.S` (startup-v1 entry: ORs every non-argument GPR and `ud2`s
  if any promised-zero register is nonzero, calls `je_main`, routes its return
  value into the EXIT syscall), `user.ld` (W^X segment layout, linked with
  `--orphan-handling=error` so no section lands silently in an RX segment),
  `lib/mem.c`, `services/hello.c`.
- **Kernel**: `image.c` carries the generated `c_hello` descriptor; `main.c`
  re-validates both built-in images at init and boots `c_hello` as the M4e-s1
  scenario (startup v1 → rodata/data/bss → CLOCK_TICKS → READY → exit 0). The
  compile-time `JE_C_HELLO_BLOB_SHA256` guard pins the embedded blob.

**Logic / invariants.**
- **Lowest-rule-wins.** Each rule is a complete pass over all loads in rule order
  (E6, then E7, then E8 on the sorted list, then E9 on the total, then E10), so a
  later segment's low-numbered violation always beats an earlier segment's
  high-numbered one. The earlier merged E6/E7 and E8/E9 loops violated this.
- **Validator parity.** Every rule the kernel enforces is enforced by the tool;
  the tool is additionally stricter (requires R, `vaddr ≥ BASE`, aligned offsets).
  `tests/test_image.c` ties the two together by running the GENERATED descriptor
  through the real C validator plus ~90 mutations.
- **W^X.** User segments are never W&X; the built ELF is RX `.text`, R `.rodata`,
  RW `.data`+`.bss`. The kernel builds the user stack RW+NX itself.

**Evidence (local).** `make check`: PASS — `test_image` 88 checks (image=c_hello,
blob=613 bytes, 3 segments), `test_mkimage` 562 checks (E1–E10 + CLI + output),
plus the full hosted suite. `make smoke`: M4e-s1 PASS, image
sha256=`351bc37034ee86c363684c013917239855cc62a43fd68f3898b245829484b8a3` (short
`351bc37034ee86c3`); blob 613 bytes, ELF 13,280 bytes, deterministic. `make
integration`: 11 fault modes PASS, M4e-s1 present. NX ×11 across the three
profiles and ARM64 base + 3 fault variants: PASS. Local evidence only; remote CI
reported separately on the PR.

**Mutants run by the lead through the real build.**
- Swap W↔X in the ELF→JE perm mapping → generated code segment becomes RW →
  entry no longer in an RX segment → `make` fails at generation with E10. Caught.
- Disable the E8 overlap check → `test_mkimage.py` fails on the E8 cases and the
  new `E8<E9` precedence case reports E9. Caught.

**Provenance (worker mode; AI-assisted review, not human qualification).**

| Part | Author | Adversarial reviewer |
|---|---|---|
| Frozen headers + mkimage spec + fail-closed stubs (`1df911a`) | lead | — |
| mkimage.py, crt0/user.ld, user lib/service, kernel wiring | Codex | Claude (review) + lead mutants |
| test_mkimage.py, test_image.c | Claude (from headers/spec only) | Codex (review) + lead mutants |
| Lowest-rule-wins fix, atomic output, Makefile/test hardening | lead (from the reviews) | A/B regression check |

The two cross-lineage reviews and the decisions taken (and declined) are in
`docs/adr/debates/m4e-reviews.md`; the worker notes are in
`docs/adr/debates/m4e-workers/`.

**Open hazards.**
- The M4e-s1 proof does not assert which image the child ran (only READY + exit
  0); the image id is hard-coded in the launch asm. Revisit if PR-F selects
  images dynamically.
- PR-D's unexplained slot-0 `pause` stall and the PR-F supervisor-teardown
  follow-up still stand.

## M4 PR-D — synchronous PROC_KILL (ADR 0010 D8) — 2026-10-31

Owner-approved as D-OWNER-3: kill enters the production ABI, and A6-CLARIFY-1 is re-confirmed.

**What it does.**
- **Op 16 `PROC_KILL`** needs KILL on a PROCESS handle.
- **`je_sched_kill`** wraps the unchanged static `mark_dying`; the lead verified its body is byte-identical to main. It runs at IF=0 and checks, in the order fixed in `sched.h`:
  1. invalid record;
  2. self;
  3. DYING or DEAD → OK, idempotent, decided from the record;
  4. supervisor;
  5. LIVE but unlinked → INVALID (fail closed);
  6. slot invariants: anything unexpected is fail-stop;
  7. `mark_dying(KILLED)`.
- **Release:** the single release owner releases a WAITING waiter, or a WOKEN reservation (the head stays queued). The continuation never runs, and the victim never observes CANCELLED.
- **New pure helpers:** `je_proc_run_link` and `je_proc_is_supervisor`.
- **New counter:** `je_endpoint_kern_woken_reservations_released`. It is kernel-internal and counts any release of a WOKEN, head-reserved waiter, not kills only.

**Honesty correction.** ADR 0009 A6-CLARIFY-2 describes a test-only forced-kill hook that was **never implemented**. ADR 0010 D8 now has a dated correction, and ADR 0009 a note. `PROC_KILL` is the first native evidence for killing non-current READY, BLOCKED_IPC and WOKEN-reserved user threads.

**Evidence: QEMU M4d-s1 (NX profile).**

| Case | What the fixture checks |
|---|---|
| READY | The heartbeat freezes, and dispatches stay unchanged for 0x40 ticks. |
| BLOCKED_IPC | Before reaping: the sentinel is never written, progress and dispatches are unchanged, there is no waiter, the cause is NONE, and the inflight ref is back to baseline. |
| WOKEN-reserved | The counter goes up by exactly 1, one message is still queued, and there is no waiter. |
| After each kill | The killer's very next syscall, PROC_STATUS, already reads DYING/KILLED. |
| EXIT | The first reason wins: re-killing after an EXIT(42) leaves the reason as EXIT. |
| DYING | Re-killing a DYING process is idempotent. |
| Supervisor guard | Uses a real run-linked supervisor thread. The kill returns INVALID, and the record stays LIVE. |
| Other | self, after-death, unlinked, wrong-rights, wrong-type and stale cases. |

Hosted tests: `run_link` / `is_supervisor` and the op-16 decode matrix. Mutation testing left 0 survivors.

**Mutants run by the lead through `make smoke`:**
- `mark_dying` removed from the kill path: caught.
- Supervisor guard removed: caught. The round-2 version of the fixture let this mutant survive; that was fixed.

**Provenance (worker mode; AI-assisted review, not human qualification).**

| Part | Author | Adversarial reviewer |
|---|---|---|
| Headers and stubs; counter contract; honesty correction | lead | — |
| Implementation and fixture | Codex (4 rounds) | Claude (2 reviews) and lead mutants |
| Hosted tests | Claude (from headers only) | Codex (with mutants) |

**Open hazards.**
- **Unexplained stall.** The root cause of the round-2 stall is **not determined**. A slot-0 coordinator polling with `pause` and IF=1 stalled, with four user threads READY. Waits that use `sti; hlt` work. This is recorded so a later scheduler change does not hide a real bug.
- **PR-F follow-up.** PR-F adds D12 supervisor-loss handling, so it must revisit this fixture's supervisor teardown.

## M4 PR-C — spawn, LAUNCHER, startup register ABI, CLOCK_TICKS (ADR 0010 D5/D6) — 2026-10-31

Owner-approved under D-OWNER-7 (`CLOCK_TICKS`), with D5 as accepted in ADR 0010.

**ABI.** These additions are appended to v0:
- op 13 `CLOCK_TICKS`;
- op 14 `PROC_SPAWN`;
- object type `JE_ABI_OBJ_LAUNCHER` (4) and right `JE_ABI_RIGHT_SPAWN` (0x20);
- `je_abi_spawn_grant`, the quota macros and `JE_ABI_STARTUP_V1`.

The op-14 decode list is exhaustive. Launcher, image and pointer validity are checked in the glue or in spawn, not in decode.

**Spawn** (`kernel/core/spawn.c`; contract in `include/jeios/spawn.h`) runs with IF=0. Its precedence is:
1. launcher;
2. image;
3. quotas, including n greater than the effective child quota;
4. grants (ENDPOINT only, with TRANSFER, rights a subset);
5. the caller-table `je_handle_can_alloc` preflight;
6. a journal: record, VM build, grant mints, thread with startup v1, then publish of a PROCESS handle with INSPECT|KILL.

On failure the journal is undone in reverse, and every pool returns to its baseline. Once the thread exists, rollback uses `on_dying(KILLED)` and then `on_reaped`. Internal NOMEM/BUSY become `H_NOSLOTS`. Spawn itself runs no audit; the fixture audits the child. There is a kernel-internal fail-Nth hook with a fired counter, and it is disarmed after every spawn.

**Other pieces:**
- A built-in image table (`image.c`), validated at boot.
- `je_thread_create_user_start` (startup registers).
- The LAUNCHER row in typed dispatch: `QUERY` returns 4; `RIGHTS` and `DUP` return DENIED.

**Evidence: the QEMU fixture M4c-s1 (NX profile).**
- The spawner reads its startup v1 registers (LAUNCHER in RDX) and asserts that every other GPR is zero.
- The child asserts the full zero-fill and the grant order by `RIGHTS`: `SEND|INSPECT` and `SEND|RECV|INSPECT`. It also checks attenuation behaviourally: `RECV` on the send-only end is DENIED.
- The child sends a message and exits with code 5.
- `CLOCK_TICKS` is monotonic, and a nonzero argument gives INVALID.
- Every negative spawn checks `RDX==0` and an IF=0 baseline. The cases are: bad image, no TRANSFER, superset rights, count 5, endpoint quota 1, SELF passed as launcher, a stale launcher, and BADINDEX. Separately, `n > quota` is shown to be rejected before the journal, because the step-1 failpoint never fires.
- Fail-Nth covers steps 1–5 and each grant index. It checks the fired counter and the baselines for pages, endpoints (with refs), records, threads, VMs and caller handles.
- Unfired-arm, disarm and published-rights probes are included.
- The spawn cost is measured in TSC cycles.
- The hosted tests cover `can_alloc`, the op-13/14 decode matrix, the startup-v1 uframe check and the `out_rflags` sentinel.

**Provenance (worker mode; AI-assisted, not human qualification).**

| Part | Author | Adversarial reviewer |
|---|---|---|
| Headers and stubs | lead | both |
| Implementation and fixture | Codex (4 rounds) | Claude (core review, fixture review, verification) |
| Hosted tests, written from headers only | Claude (3 rounds) | Codex (with mutants) |
| Phase-6 ack fix and disarm probe | lead | verified by running the mutant |

Details are in `docs/adr/debates/m4c-reviews.md` and `m4c-workers/`.

**Not yet:** kill (PR-D), the ring-3 C image path (PR-E), and the supervisor (PR-F).

## M4 PR-B — PROCESS records, automatic reaper, typed dispatch, PROC_STATUS, creation quota (ADR 0010 D3/D4/D-DEV) — 2026-10-31

Owner-approved as D-OWNER-2 (death path / reaper), D-OWNER-4 (typed `query`/`rights`/`dup`) and D-OWNER-8 (the D-DEV deviation fix).

**PROCESS records.** New pure module `kernel/core/proc.c` with contract `include/jeios/proc.h`. It is a pool of R = 48 records, each identified by {index, generation}.
- Lifecycle: FREE → LIVE → DYING → DEAD → FREE once no handle refs remain. A slot whose generation is exhausted is retired and never reused.
- Each record holds its run link, death reason and code, a supervisor flag, its handle-ref count, and the endpoint-creation charge.
- Only `on_reaped` clears the run link. Abandon is used only for spawn rollback, on a record that was never linked.

**Death path.**
- `mark_dying` records the reason on the record.
- `je_sched_reap_dead` captures the thread's back-link, then runs `drop_user` → `je_vm_destroy` → `je_proc_on_reaped`, all at IF=0. Any failure after `drop_user` is fatal.
- One shared idle step (IF=0: reap, fail-stop on error; then `sti; hlt`) drives both the final idle loop and the M4b fixture's wait loops. QEMU therefore exercises the automatic reaper.
- When a fixture tears down a live process directly, that teardown is recorded as KILLED.

**Typed dispatch** (syscall.c) uses a table over SELF, ENDPOINT and PROCESS, with known rights, a dup policy and retain/release hooks:
- `QUERY` works on any live type, via `je_handle_resolve_any`.
- `RIGHTS` requires INSPECT.
- `DUP` attenuates: SELF via `dup_self`; ENDPOINT via `je_endpoint_kern_dup` (this is the first time ring 3 can see endpoint dup); PROCESS by retaining the record before allocating the handle, with rollback.
- `CLOSE` releases exactly once.
- The teardown walk releases PROCESS refs as well as endpoint refs.
- A LIVE PROCESS slot whose record is invalid is a fail-stop invariant break.

**PROC_STATUS (op 15)** needs INSPECT. It takes a snapshot and writes a 16-byte copy-out; a fault gives INVALID and nothing is written.

**D-DEV** (fixes ADR 0009 D5):
- `je_endpoint_channel_create_by` checks for a free record (NOSLOTS), then charges a LIVE creator (QUOTA, or INVALID for an invalid creator), then mints, unwinding and uncharging on failure.
- The charge is released at record destruction, and only against the same identity.
- NONE is exactly {0,0} and is used only by the kernel seed.
- A user thread with no identity fails closed.

**ABI.** Appended to ABI v0: op 15, `JE_ABI_OBJ_PROCESS` 3, `JE_ABI_RIGHT_KILL` 0x10, `je_abi_proc_status`, and the `JE_ABI_PROC_*` / `JE_ABI_DEATH_*` values, all pinned by `_Static_assert`. A stale `abi.h` comment saying `cap_count` must be 0 was corrected.

**Evidence (local).**
- `make check`: proc, proc_retire (JE_PROC_GEN_MAX=3), handle resolve_any, the endpoint D-DEV matrix, and syscall decode.
- `make smoke` / `make integration` and the three x86 profiles.
- QEMU markers `M4b-s1: PROCESS record status/typed dispatch OK (...)` and `M4b-s1: endpoint creation quota/charge release OK`.
- ARM: all 4 modes.
- Remote CI is recorded in the PR.

**Provenance (worker mode; AI-assisted, not human qualification).**

| Part | Author | Adversarial reviewer |
|---|---|---|
| Header contracts and stubs | lead | both, via review |
| Kernel implementation | Codex | Claude |
| Hosted tests, written from headers only | Claude | Codex |
| Contract pins; `JE_SYSCALL_PROC_STATUS` enum; 2-line `je_proc_charge` fix | lead | round-3 review |

Details are in `docs/adr/debates/m4b-reviews.md` and `docs/adr/debates/m4b-workers/`.

**Not yet:** spawn, kill, LAUNCHER and CLOCK_TICKS (PR-C/PR-D). Today PROCESS handles are minted only by kernel fixtures.

## M4 PR-A — capacity raise + per-table handle quota (ADR 0010 D2) — 2026-10-31

ADR 0010 D2, owner-approved as D-OWNER-1. This is a tested-path change, and the owner gated it.

**What changed**

| Constant | Before | After |
|---|---|---|
| `JE_VM_MAX_PROCESSES` | 2 | 8 |
| `JE_THREAD_MAX` | 8 | 16 (`JE_M2_WORKERS` = 7) |
| `JE_EP_POOL_CAP` | 4 | 24 |
| `JE_VM_HANDLE_CAP` | 16 | 40 |

- The D2 formulas are pinned by `_Static_assert`s: supervisor handle need + 4 ≤ H − 1; the endpoint pool formula, asserted in `endpoint_kern.c` because it needs both headers; T ≥ 1 + 7 + P.
- `je_vm_build_quota(..., handle_quota, ...)` checks the quota range **first** (0 or more than H − 1 → INVALID, no state change). It is the single build path. `je_vm_build` delegates to it with `JE_VM_HANDLE_QUOTA_DEFAULT` = 15, the M3b value, so every quota-at-15 assertion holds.
- The M2 boot workload stays at exactly 7 workers. A no-allocation probe fills the other 8 thread slots, so the "next create returns NOMEM with `free_count` unchanged" rollback check still runs at the real capacity. The preemption floor is now 2 × `JE_M2_WORKERS`. The M3 dispatch budgets stay `JE_THREAD_MAX`-scaled, i.e. full passes over the slot table.
- The ring-3 BADINDEX probe moved from index 16 to index `JE_VM_HANDLE_CAP` (40), so BADINDEX at the bound is still exercised at the new capacity.
- Hosted tests:
  - admission BUSY on build P + 1;
  - P-way isolation;
  - `je_vm_build_quota`: range checked first, crossed with every other failure class; exact admission at 1, the default and MAX; independence across P slots; fault-injection rollback;
  - endpoint pool exhaustion at exactly E.

**Static cost.** `.bss` 2,797,568 → 2,949,120 bytes (+151,552). Free kernel pages at the 16 MiB profile: 3104 → 3067. The cross-process `je_vm_audit` bound is at most pages × (P − 1) × pages ≈ 37k frame comparisons for a full process. That is fine at boot. A future spawn path must not run it for every spawn with IF=0 without measuring the latency (ADR 0010 D5 runs spawn under IF=0).

**Provenance (worker mode; AI-assisted review, not human qualification)**

| Part | Author | Adversarial reviewer |
|---|---|---|
| Header contract and fail-closed stub | lead | — |
| Kernel implementation | Codex | Claude |
| Hosted tests, written from the headers only | Claude | Codex |
| BADINDEX probe restoration | lead | — |

Notes: `docs/adr/debates/m4a-workers/`.

**Evidence.** Local: `make check`, `smoke` and `integration`; the three x86 profiles; ARM normal and vector smoke. Remote CI is recorded in the PR.

**Not yet.** No new runtime ability. PROCESS records, spawn and the supervisor arrive in PR-B onward.

## MP1 step 3 — bounded copied DTB parse + remote ARM64 CI job (QEMU virt-8.2) — 2026-10-30

Issue #52 (finishes ARM64.md MP1). Not owner-gated in substance: new
arch-neutral `kernel/core/fdt.c` + new contract `include/jeios/fdt.h`, new ARM
glue, a new CI job; x86 tested paths and its SOURCES list unchanged (fdt.c is
built for ARM and for the hosted test only). The new contract is presented to
the owner in the PR for gating like any frozen contract.

**What it does.** `boot.S` guards the firmware DTB pointer before the BSS clear
(nonzero, 8-aligned, no `x0 + 2 MiB` wrap, no overlap with the whole image
extent incl. NOLOAD) -> bounded FATAL + halt. `main.c`, after VBAR is installed,
byte-reads the 40-byte header, `je_fdt_copy_plan` -> ONE copy of
`min(totalsize, 64 KiB)` into a 16-aligned `.bss` buffer, `je_fdt_parse` on the
copy only (no pointers into the firmware blob survive), then
`je_boot_ctx_check` against arch facts (kernel image extent, DTB source range,
PARange -> pa_bits; reserved encodings FATAL). Markers print copy length,
totalsize, CRC-32 of the copied prefix, RAM ranges and the validated pa_bits.
`JE_ARM_DTB_BAD` corrupts the copy's first struct token and must produce
exactly `FATAL dtb rejected status=BAD_TOKEN off=0x40`.

**Parser contract (normative text lives in `fdt.h`).** Single pass, file order,
first violation wins, one deterministic status + `err_off` per failure. Bounds:
totalsize <= 2 MiB, copy <= 64 KiB, depth 16, 1024 nodes, 64 props/node, name
255, 8 RAM ranges, 16 kept reservations, 16 memreserve entries, 16
`/reserved-memory` candidates (walk-time). u64 arithmetic with overflow checks
everywhere; zero-size blocks valid; struct/strings overlap rejected. Semantics:
root cells required (1|2); memory by `device_type`; `/reserved-memory` cells
must equal root's with empty `ranges`, decided at the first child; `status`
honored only on memory nodes and reserved-memory children (ignored on `/chosen`
and `/reserved-memory`, matching Linux and failing safe); DUPLICATE at the
repeating PROP in every tracked node, before value checks; CONFLICT for a
`/chosen`/`/reserved-memory` claiming `device_type = "memory"`; reservations
fully outside RAM ignored+counted, partial -> BAD_RANGE; dynamic (`size`-only)
reservations counted for MP2. Freestanding: no struct copies (verified
`nm -u` empty at -O0/-Os/-O2, ARM and host); `je_fdt_parse` stack ~2.7 KiB.

**Provenance (worker mode; AI-assisted, not human qualification).**

| Part | Author | Adversarial reviewer |
|------|--------|----------------------|
| Contract `fdt.h` | lead, after Claude+Codex consults (`docs/adr/debates/mp1-dtb.md`) | both peers |
| `kernel/core/fdt.c` (rounds 1–4) | Codex (W1) | Claude |
| `tests/test_fdt.c` (rounds 1–5), written from the header only | Claude (W2) | Codex |
| ARM glue + `tools/smoke_arm.py` + `mk/aarch64.mk` | Claude (W3) | Codex |
| `.github/workflows/ci.yml` `aarch64` job | Codex (W4) | Claude |
| fixture generator, build pre-wiring, integration, docs | lead | peers via review |

Each worker's notes: `docs/adr/debates/mp1-dtb-workers/`. Because the parser and
its suite were written independently against the header, a suite PASS is a
two-implementation agreement on every pinned case, not one author checking
itself.

**Evidence (local, Ubuntu host, QEMU 8.2.2; see the PR for remote CI).** Hosted
`test_fdt` under ASan+UBSan: fixture, per-status builder cases with exact
err_off, copy_plan, boot_ctx_check, a full truncation sweep of the 7426-byte live
extent, and 30000 byte-changing mutation iterations with determinism re-parse.
ARM `make smoke-aarch64 ARM_REQUIRE=1`: default, `ARM_VECTOR_TEST=1`,
`ARM_BSS_CHECK=1`, `ARM_DTB_BAD=1` all PASS; booted DTB CRC32 `0xcc76dc2b` ==
committed fixture; `ram[0] 0x40000000+0x10000000`; `pa_bits=44`. x86
check/smoke/integration and the three CI NX/memory profiles unaffected.

**Not enforced yet / MP2 obligations.** MMU and caches are off; MP2 must
`DC IVAC` the copy buffer before enabling caches, must honor `rsv_dynamic` and
`rsv_nomap` in its allocator, and must not trust anything outside the copy.
The 256 MiB QEMU profile is pinned (with QEMU's 128 MiB default the DTB at
0x48000000 lies outside RAM, so the 256 MiB pin is required; that 128 MiB boot
is not exercised as a test). Node names match exactly
(`chosen@0` is ordinary). Only the QEMU virt-8.2 DTB is qualified; no physical
board.

## MP1 step 2 — ARM64 EL1 exception vectors + trap frame (QEMU virt-8.2) — 2026-10-19

ADR 0002 D5-3 + ARM64.md MP1, owner-approved trap-frame ABI. Installs VBAR_EL1
exception vectors in `kernel/arch/aarch64/` (`vectors.S`, `trapframe.h`, extending
`boot.S`/`main.c`). Not owner-gated (new ARM64 arch code, no x86 tested-path
change); the D5-3-constrained trap-frame layout was escalated and approved.

**Trap frame (frozen ABI, `trapframe.h`):** 304 bytes, 16-aligned; `gpr[31]` =
x0..x30, then `sp@248, elr@256, spsr@264, esr@272, far@280, vector@288, pad@296`
(`_Static_assert(offsetof)` pins asm↔C agreement). D5-3 saves ESR/FAR/ELR/SPSR;
ELR is the fixup key for MP3. **Known deviation (documented in the ABI):**
gpr[16..18] hold stub scratch (vector index / guard ptr / guard value), not the
interrupted x16-x18 — harmless for the fatal, non-returning MP1 handler; a future
returning handler must preserve them.

**Vectors (`vectors.S`):** a 2 KiB-aligned 16-slot table (VBAR_EL1[10:0] RES0);
every slot an index-bearing stub (`mov w16,#idx; b common`) so the vector index
reaches C. All 16 installed though MP1 only reaches "Current EL SP_ELx Sync"
(slot 4; no EL0, IRQ/FIQ masked); the rest are defined bounded halts. The common
saver builds the frame on the EL1 stack and calls `je_arm_exception`. Reentrancy:
single stack, non-reentrant, non-returning; a plain `je_arm_in_handler` guard (set
before any faulting access; no atomics, no clear) routes a nested sync fault to a
**pure-asm** `je_arm_double_fault` (stack-free, bounded TXFF poll). No allocating
handler. The non-group-1 slots' `sp+304` interrupted-SP is correct only for SP_ELx
(the reachable group), documented.

**Diagnostic + boot gap fix:** the C handler prints a bounded redacted syndrome
(vector, EC, IL, ISS, ESR, ELR, SPSR, FAR-if-valid-incl-FnV-check) + a VBAR
readback (evidence the table installed), then halts. `boot.S` now writes
**SCTLR_EL1 = 0x30D00808** explicitly (SA=1 SP-alignment check + the ARMv8.0 RES1
set; M/A/C/I = 0) — a review-found step-1 gap, since SA resets architecturally
UNKNOWN so misaligned-SP faulting was non-deterministic.

**Proof (ARM64.md "exact expected class/status, not timeout"):** a
`JE_ARM_VECTOR_TEST` build plants `brk #0xA55A`; on real `qemu-system-aarch64` it
vectors to slot 4 with observed **ESR_EL1=0xf200a55a** (EC=0x3c BRK, IL=1,
ISS=0xa55a), ELR==the brk site, SPSR=0x800003c5 (EL1h + DAIF masked), FAR=n/a. The
in-kernel comparator asserts the exact syndrome AND `smoke_arm.py` independently
asserts the raw `esr=`/`spsr=` serial lines (so a comparator bug can't false-PASS).
`make smoke-aarch64` (default + `ARM_VECTOR_TEST=1` + `ARM_BSS_CHECK=1`) all PASS;
`make aarch64` clean under `-Werror -Wconversion -mstrict-align
-mgeneral-regs-only`; x86 check/smoke/integration unaffected.

Design + adversarial review (which verified the SCTLR value, vector-table offsets
via objdump, and the guard against the ARM ARM, and drove the FnV/double-fault-asm/
raw-assert/doc fixes): `docs/adr/debates/mp1-vectors.md`. AI-assisted review; not
independent human qualification. Remaining MP1: the bounded DTB parse.

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

## M3c non-blocking data IPC (try_send / try_recv) — 2026-10-18

ADR 0009 **D-SEQ step 4**, the first PR that moves message bytes through
endpoints. Non-blocking, bounded, copied, **data-only** (D9). Not owner-gated
except for one small approved touch (below).

**Queue model (Model A).** Each end gained an `inbound` ring queue
(`JE_EP_QUEUE_DEPTH=4` messages, each up to `JE_EP_MSG_MAX=64` payload bytes — a
newly chosen policy bound, pinned by `_Static_assert(sizeof(je_ep_channel)<=640)`;
the pool is ~2.1 KB BSS). `try_send` on end `i` enqueues into `end[i^1].inbound`
(deliver to peer); `try_recv` on end `j` drains `end[j].inbound`. EOF/PEER_CLOSED
is decided off the **caller's own** `state` (release_handle already moves a
survivor to PEER_CLOSED), so there is no `i` vs `i^1` mistake on the EOF path —
the only `i^1` index is the enqueue target. Recv: nonempty → deliver (drain
first, regardless of peer state); empty+PEER_CLOSED → PEER_CLOSED (EOF);
empty+OPEN → EMPTY. **Data owns no references** — queued messages never touch
`handle_refs`/`inflight_refs`, so the free rule is unchanged.

**Copy ordering (D6), the peek/commit seam.** `endpoint.c` stays pure (no
`copy_*_user`): it moves kernel bytes only, and the glue wraps it.
- **Send:** glue runs a read-only `je_endpoint_send_preflight` (resolve + SEND
  right + self-state + peer-queue-full) FIRST — so a bad user pointer never masks
  DENIED/PEER_CLOSED/QUEUE_FULL and user memory is never read for an unauthorized
  handle — THEN `copy_from_user` into a **stack** staging buffer (fault → INVALID,
  nothing enqueued) THEN the pure `try_send` re-checks + enqueues. Status
  precedence frozen: decode INVALID (meta/reserved/len/wide-handle, before any
  user access) → BADINDEX/STALE/WRONGTYPE → DENIED → PEER_CLOSED → QUEUE_FULL →
  copy-fault INVALID (last — the copy runs only after a clean preflight, so a bad
  ubuf never masks a handle/queue status).
- **Recv:** pure `recv_peek` copies the head message into a kernel-stack buffer
  (no dequeue) and returns a `{rec,side,generation,head,len}` lease; the glue
  checks `cap < len` → SHORT_BUFFER (RDX=required len, not dequeued), else
  `copy_to_user` (fault → INVALID, message stays queued), then the **infallible**
  `recv_commit` dequeues. A commit lease mismatch (impossible under IF=0
  single-CPU) halts the kernel — never a user status, because a retry would
  double-deliver. No `inflight` lease is held across the user copy (both peers
  agreed it's unneeded under IF=0 and would wrongly spend the REF_MAX budget).

**Data-only enforcement (D9).** `try_send`'s `R10[7:0]=cap_count` must be 0 and
`R10[63:8]`/R8/R9 reserved must be 0 (rejected INVALID at decode) — no capability
can be queued before transfer (step 5).

**Owner-approved lifecycle touch.** `release_handle` now discards the *closing*
end's inbound queue (no one can read it), and `maybe_free_record` zeroes both
queues on free (no stale payload into a reused record). This is a change to
M3c-tested lifecycle code (per AGENTS.md) — **owner-approved 2026-10-18**. The
free *condition* and refcount rule are unchanged; only zeroing/discard was added,
covered by a free-with-nonempty-queue test (returns to baseline).

**ABI (ops 11/12, statuses 10–13).** `try_send`=11, `try_recv`=12; statuses
`JE_ABI_H_QUEUE_FULL=10`, `SHORT_BUFFER=11`, `PEER_CLOSED=12`, `EMPTY=13`
(appended, pinned). `JE_ABI_MSG_MAX=64` is ABI-visible, pinned equal to the
table-side bound. recv returns bytes-written (or required-len on SHORT_BUFFER) in
`RDX[31:0]`; `RDX[63:32]` reserved 0.

**ABI forward note (owner-flagged, item 2).** The v0 single-RDX scalar cannot
carry *received capability handles*, which step 5 (transfer) will need. R8/R9 and
`RDX[63:32]` are reserved-must-be-0 now; **step 5 MUST add a new op or an
out-register/out-struct extension — it must NOT overload `try_recv`'s RDX**. This
is recorded so step 5 does not silently imply the current ABI is transfer-ready.

**Proofs.** Hosted `test_endpoint` gains the full battery: send/recv roundtrip,
direction (recv on the sender's end is EMPTY), FIFO, QUEUE_FULL, SHORT_BUFFER (no
dequeue), PEER_CLOSED drain-then-EOF, rights DENIED (send without SEND / recv
without RECV), zero-length (OK with len 0, distinct from EMPTY), `len>MSG_MAX`
INVALID, free-with-nonempty-queue, and a stale-lease commit → CORRUPT.
`test_syscall` covers the op-11/12 decode (R10 cap_count/reserved, R8/R9,
len>MSG_MAX, wide-handle; op 13 the new UNSUPPORTED boundary). The ring-3
**`M3c-s4`** marker is a self-loopback (create puts both ends in one table):
proves copy-in, queue, direction, SHORT_BUFFER, the **copy-out FAILURE leaves the
message queued and a retry delivers it exactly once** (D6 rollback, ring-3 — the
dst is a pre-validated unmapped page, so the copy is rejected at span
prevalidation, NOT a mid-copy #PF/fixup; the rollback *property* is proven, the
fault *mechanism* is exercised separately by the M3a copy-fixup inject path),
QUEUE_FULL, the data-only reject, drain-then-PEER_CLOSED, and channel reclamation
on exit. FIFO ordering is proven in the hosted test (the ring-3 stub sends
identical payloads, so the marker does not claim FIFO). **Honesty:** it does NOT
prove cross-address-space delivery (one process
holds both ends) — that waits on transfer (step 5). `make check` / `smoke` /
`integration` / 3 CI NX profiles (incl. 16M) / ARM64 all green.

Design + implementation cross-review via a role-assigned peer debate. Key fixes
adopted before PR: the send **preflight** ordering (copy-first would mis-rank
INVALID over DENIED/QUEUE_FULL); the peek/commit seam with an **infallible**
post-copy commit (a fallible commit would double-deliver); no inflight lease;
zero exactly `len` bytes out (no stack leak). AI-assisted review; not independent
human qualification.

## M3c bootstrap fixture (D-SEQ step 7) — 2026-10-19

ADR 0009 **D-SEQ step 7**, the final step; closes #8. The kernel injects a seeded
endpoint pair into two ring-3 processes and runs a bounded two-process protocol —
"fixture, not supervisor." It **discharges the two honest deferrals** carried from
steps 5 and 6 **end-to-end in one run**, which blocking recv (step 6) is what makes
possible: a receiver with nothing queued PARKS and yields the CPU to the peer, so
there is no cooperative-scheduler spin deadlock (the reason steps 5/6 deferred), and
the lost-wakeup is closed by the entry-probe snapshot being in the same IF=0 syscall
path as the park.

**`M3c-s7`** (kernel self-test, two real ring-3 processes A=pinger + B=ponger over a
kernel-seeded channel S):
- **Two-process WOKEN handshake (step-6 deferral):** B is created first and **confirmed
  parked** on a blocking recv (its progress marker holds at the pre-recv value across
  ticks far below its timeout) **before A is created** — so A's send is a
  **causally-forced WOKEN**, not a race. B resumes with the real message (status 0,
  never 14).
- **Two-process capability transfer over the WOKEN continuation (step-5 deferral):**
  A creates channel C and sends `c1` as a **capability** in that first message; B
  mints the transferred end **on wake** (the cap-bearing mint-on-wake continuation
  path — never exercised end-to-end before: s5 used the kernel forwarders, s6 only
  TIMED_OUT). **Retained-ownership (copy, not move) is proven** by A closing `c1`
  **before** B uses the minted handle (a MINTED/GO handshake): B's **post-close send**
  on the minted handle reaches A's `c0` with the expected payload.
- A bounded **4-round ping-pong** on S (functional progress + no timeout), then an
  explicit **DONE ack teardown** (closure never races a queued message); both
  processes `exit(0)`, are reaped, and the channel pool **reclaims to 0**.
Any nonzero-timeout recv returning ABI EMPTY, a TIMED_OUT where a WOKEN was due, a
stub assertion failure, or the watchdog tripping (a hang = a real lost-wakeup/deadlock
bug) **fails** the fixture. Honest scope: only the staged first recv and the
cap-transfer recv are independently *causally-forced* WOKEN; later ping-pong receives
prove progress + timeout-absence (they may be immediate deliveries). No new kernel
surface — the handles are injected via the fixture's arena write; not owner-gated.

`make check` / `smoke` (s3a/s3b/s4/s5/s6/s7) / `integration` / 3 CI NX (incl 16M) /
ARM64 all green. Design + review transcript:
`docs/adr/debates/m3c-step7-bootstrap-fixture.md` (ADR 0000 part 4). AI-assisted
review; not independent human qualification. **M3c (#8) is functionally complete.**

## M3c blocking-recv waits (D7 / A6) — 2026-10-19

ADR 0009 **D-SEQ step 6** (owner-gated — edits `sched.c`; contract frozen by A6 +
A6-CLARIFY-1/2). `try_recv` can now **block**: a nonzero R10 timeout parks the
receiver until a message arrives (WOKEN), the peer closes (PEER_CLOSED), the
deadline elapses (TIMED_OUT), or an internal cancel fires (CANCELLED, A6-2
ABI-unreachable in v1). R10==0 is the unchanged non-blocking poll (returns EMPTY).

**State machines (A6-STRUCT).** Two separate: the **waiter** (`WAITING →
WOKEN|TIMED_OUT|PEER_CLOSED|CANCELLED`, on the endpoint end) and the **scheduler**
(`THREAD_BLOCKED_IPC → READY`). One pure `je_endpoint_wait_arbitrate` CAS is the
ONLY path out of WAITING; on single-CPU IF=0 the first critical section wins and
later causes are no-ops (the written D7 priority applies only to the one entry
snapshot). The kern bridge calls arbitrate on sender-enqueue (WOKEN, on the peer
end), peer-close (PEER_CLOSED, drain-first: only on an empty end), the timer ISR
(TIMED_OUT), and internal cancel; on WON it `je_sched_wake_ipc(id)` (guarded
`BLOCKED_IPC→READY` only — never resurrects DYING/ZOMBIE).

**Park/resume mechanism (A6-CLARIFY-1, constrained restartable syscall).** The
recv handler, on EMPTY + nonzero timeout, registers a per-end waiter
(`je_endpoint_wait_register`, A6-4 one-per-end → `WAITERS_BUSY` maps to a
non-blocking EMPTY) and calls `je_sched_block_current`: a deliberate **pre-park
`je_uframe_check`** (rejects a corrupt return frame at park, not at wake holding a
reservation), save the recv args + the **owned rec/side** + the post-`int` RIP,
**rewind `f->rip` by `JE_INT80_LEN=2`** (`CD 80`), set `BLOCKED_IPC`, switch. On
wake the thread re-executes `int 0x80`; a **pending-continuation prologue** at the
top of `je_syscall_dispatch` (keyed on `wait_pending`, asserts the restart RIP)
finishes the recv **from the owned reference, never re-resolving the handle**
(A6-5), copies out **after** the context switch (no copy in the scheduler). No
trap-entry assembly change.

**Reservation + release (A6-5 / A6-CLARIFY-2).** A WOKEN waiter reserves the
inbound head (not dequeued, just not deliverable to a concurrent poll — `recv_peek`
and `recv_peek_caps` mask a reserved head → EMPTY). Exactly one idempotent release
owner, `je_endpoint_wait_release[_by_id]`: a successful commit (consume), a
copy/mint-fault rollback (message stays queued, head returns to deliverable WITHOUT
re-arming the terminal waiter), and thread teardown (`mark_dying` releases;
`je_vm_destroy`/reaper inherit it). `mark_dying` calls it; the slot's waiter is
released before the slot is reused (closes the death-before-re-entry window).

**ABI (A6-ABI).** Reuses the `try_recv` op (12) with R10 = relative-tick timeout
(bounded by `JE_IPC_MAX_WAIT=2^32`, overflow-checked). New statuses
`JE_ABI_H_TIMED_OUT=14`, `JE_ABI_H_CANCELLED=15`. Caps compose: a WOKEN delivery
runs the same step-5 staged-mint path from the owned reserved head
(`recv_peek_owned_caps`). A short buffer is SHORT_BUFFER whether polling or
blocking (detected at the entry/owned peek, never swept into INVALID).

**Proofs.** **`M3c-s6`** kernel self-test proves the **TIMED_OUT** cause
end-to-end through the real scheduler + ring 3 (a single thread parks on
`recv(timeout)`, the timer ISR stamps the terminal transition, the continuation
resumes with 14, waiter released, channel reclaimed — deterministic, one thread +
the natural tick, no spin race). Hosted `test_endpoint` 27–28 prove the pure
waiter layer (register, the single terminal CAS + first-wins, reserved-head
masking, A6-4 WAITERS_BUSY, release idempotence, A6-CLARIFY-2 release-returns-head).
`test_syscall` covers the R10-timeout decode. The **full two-process ring-3 WOKEN
handshake is deferred to step 7** (bootstrap fixture) — honest, same as the step-5
ring-3 deferral; the wake bridges are wired and compiled, step 7 adds the ring-3
scheduling of two cooperating user processes. `make check`/`smoke`
(s3a/s3b/s4/s5/s6)/`integration`/3 CI NX (incl 16M)/ARM64 all green.

Design + both review rounds (contract text, then the park/resume mechanism) in
`docs/adr/debates/m3c-step6-waits.md` + `m3c-step6-mechanism.md` (ADR 0000 part 4).
Owner-signed A6 + CLARIFY-1/2. AI-assisted implementation review; not independent
human qualification.

## M3c capability transfer (try_send/try_recv with caps) — 2026-10-19

ADR 0009 **D-SEQ step 5** (contract frozen by the A5 amendment). Endpoints can now
be **transferred** between processes: a `try_send` carries 0..`JE_EP_MSG_CAPS_MAX`=2
capabilities, each a descriptor `{rec_index, side, generation, rights}` queued in
the peer inbound and holding one **inflight ref** on its target end (D3.2). The
receiver's `try_recv` delivers the bytes AND mints fresh, independently-closable,
equal-or-weaker handles in the receiver's own table (destination-side mint, D8).
Data-only sends (`cap_count==0`) are unchanged.

**D9 cycle reclamation (A5-1).** No nesting restriction: a queued-cap edge's
inflight ref releases when the holding end's LAST handle is removed (D2 discard
rule) — not on mere death (D2 co-holders). An A↔B cap cycle is reclaimed once each
end's last handle goes; proven by hosted test 24 (queue caps both ways, close all
handles, both records free). `maybe_free_record` gained an `allocated` idempotence
guard + a cap-free assertion (A5-3); `release_handle` detaches a closing end's
queued cap descriptors into a bounded local array → resets the queue → settles
THIS record → then releases each detached inflight ref (which may free OTHER
records via the guarded free). This detach-before-release order is what prevents
a double-free / recursion into a queue being discarded.

**Stale cap A5-2.** A cap whose target end is CLOSED is consumed-with-handle-`0`
(not a status symbol), siblings still mint, the message dequeues — removing the
stale-cap head-of-line wedge. PEER_CLOSED still mints (only CLOSED is dead).
`je_endpoint_capdesc_live` classifies; the recv glue stages `0` for a dead cap.

**Transactions.** Send is all-or-nothing: resolve+validate every cap, then retain
each target's inflight (rolling back all on REFOVERFLOW), then enqueue bytes+caps;
nothing enqueued on any failure. Recv stages ALL mints in the receiver table
(retain-before-commit per cap, dead caps → 0), copies bytes then the handle array
out, then the **infallible** commit dequeues + releases each cap's inflight; any
transient fault (QUOTA/NOSLOTS/REFOVERFLOW/copy-fault `JE_ABI_INVALID`) closes the
staged mints and leaves the message queued.

**ABI (A5-4).** The pre-reserved R8/R9 path: send R8 = user `u64[cap_count]` of
`{low32 handle, high32 rights}`, R10[7:0]=cap_count (≤K), R8≠0 iff cap_count>0;
recv R8 = user `u32[]` handle output, R9 = slot capacity, R8≠0 iff R9≠0; recv
RDX[31:0]=byte len, RDX[63:32]=cap count (or required counts on SHORT_BUFFER, no
dequeue). `JE_ABI_MSG_CAPS_MAX=2` pinned to the table bound. A data-only caller
(R8=R9=0) receiving a cap-carrying message now gets SHORT_BUFFER (must supply a
handle buffer) — documented behaviour change.

**Transfer sequencing (frozen, review-driven).** Under D2, closing the last handle
to a transferred end makes an in-flight cap to it dead (A5-2), so the sender MUST
retain its handle until the receiver has minted (A sends → B mints → then A
closes). "Close before B receives" is the dead-cap path.

**Proofs.** Hosted `test_endpoint` tests 22–24: cap mint across a seeded channel
(inflight retained on send, released on commit, minted handle resolves to the same
end), dead-cap A5-2, and A↔B cycle reclaim. `test_syscall` covers the op-11/12 cap
decode (cap_count/R8 pairing, R9 pairing, K bound, reserved bits). The ring-3
**`M3c-s5`** marker is a **kernel self-test** across two distinct process tables
(the kernel send/recv cap FORWARDERS — the real pure + kern logic: seed S, mint C
in table A, transfer C.e1 to table B, B messages A on c0 across tables, S+C
reclaimed). **Honest coverage note:** s5 drives the `je_endpoint_kern_*`
forwarders, not `je_syscall_dispatch`, so the syscall-layer cap-array copy-in,
unknown-rights rejection, staged-mint copy-out rollback, and RDX packing are
covered by `test_syscall` (decode) + the hosted pure tests + inspection, NOT by a
ring-3 end-to-end run; the full ring-3 two-process transfer (which would exercise
them) is deferred to step 7. A full **ring-3 two-process**
transfer is deferred to step 7 (recorded decision — ADR A5-4 proof-status;
multi-process choreography is step 7's subject and the spin-based two-stub version
raced the single-CPU cooperative scheduler). `make check` / `smoke` (s3a/s3b/s4/s5)
/ `integration` / 3 CI NX (incl. 16M) / ARM64 all green; the ~3KB pool growth from
cap descriptors fits the 16M profile.

Design frozen by the A5 amendment (owner-approved); implementation cross-review to
follow in the PR. AI-assisted; not independent human qualification.

## Standards status — 2026-10-16

| Capability | Status | Next evidence |
| --- | --- | --- |
| POSIX.1-2024 environment | Planned core requirement; no runtime conformance | Issue #15 profile/ADR, then native staged tests |
| LDAPv3 directory integration | Planned core requirement; no provider/login | Issue #16 contract/lab plan, then qualified native broker |

See [STANDARDS.md](STANDARDS.md). This documentation update changes no executable
behavior; M1/M2 remains the implemented baseline.
