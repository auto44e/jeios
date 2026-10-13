# JeiOS M3a implementation proposal

Review date: 13 October 2026. Status: proposed; no kernel changes made.
Companion requirements: [ACCELERATORS.md](ACCELERATORS.md). License: [ADR 0005](adr/0005-licensing.md).

Build the smallest demonstrable user/kernel boundary first, with explicit extension contracts for typed handles and IPC. Preserve the repository's existing gate split: M3a implements isolation, M3b implements handles, and M3c implements IPC and minimal init. This proposal covers all three interfaces without counting later work as M3a completion.

## 1. Reviewed baseline and evidence

PR [#1](https://github.com/auto44e/jeios/pull/1) remains open and unmerged. Reviewed head: `c5eb7cb3f7c8787ba4a883ca692c98993f67f05b`, branch `develop/m1-m2-core`; base `main`: `a454a08104e0b2bb21b86cb8aefde7264fe11889`. Its latest update was 2026-10-13 04:35 UTC, still October 12 in New York. No PR discussion comments were returned. Main remains the earlier scaffold; the M1/M2 behavior below is in the PR, not merged main.

Read the head's README, contributor instructions, roadmap, implementation ledger, handoff, security policy, relevant design sections and newer init/identity/encryption/domain requirements. Inspected paging, trap entry/dispatch, scheduler and ABI-related headers. This is a source/design review, not a complete security audit or a local execution verification.

The head's [CI run 38347150387](https://github.com/auto44e/jeios/actions/runs/38347150387) and its job/step results report success for core/ELF checks, normal boot, NX-absent boot, low-memory, large-memory clipping, and exception/protection tests. I did not rerun those tests locally. Existing CI proves the scoped M1/M2 workload, not userspace containment.

| Area | Implemented in reviewed PR | M3 requirement still absent |
| --- | --- | --- |
| Mapping | Supervisor-only identity mappings below 1 GiB; CR0.WP; optional NX; guards | Private user mappings, process CR3 ownership, safe user copying |
| Entry | 256 DPL0 gates, GPR frame, IRETQ, double-fault/NMI IST | DPL3 entry, user descriptors, per-thread TSS.RSP0 selection |
| Scheduling | Eight slots including idle; seven CPL0 workers; preemption/sleep/exit | Scheduler currently aborts when `frame->cs & 3`; process-aware switching |
| Faults | Fatal diagnostic and halt | User-fault containment and narrowly scoped copy recovery |
| Memory | Bounded extents and heap; heap zeroing; rollback | Raw-page zero-before-user exposure and process ownership/accounting |
| Authority | Internal module/helper interfaces | Typed handles, rights checks, IPC, credentials and init |

Important integration hazards:

1. `paging.c` has one static root and identity maps writable ordinary RAM. User code frames cannot simply receive RX aliases while their identity aliases remain writable.
2. `trap.c` has only kernel descriptors plus the two-slot TSS descriptor; all exceptions are fatal. Appending user descriptors also invalidates the existing fault test's assumption that selector `0x28` is outside the GDT.
3. `sched.c` switches frames without switching address spaces or TSS.RSP0. Reaping waits for all workers to finish; a surviving user task must not indefinitely prevent a dead task's reclamation.
4. No SIMD context exists. A compiler restriction is insufficient for hostile machine code; unsupported architectural state must be inaccessible or saved/restored.
5. Internal `je_status` values explicitly are not a stable userspace ABI. Do not expose their enum numbering accidentally.

## 2. Scope and security claim

M3a target: two real CPL3 processes, one thread each, distinct page-table roots and physical backing, bounded built-in programs, a minimal experimental syscall gate, safe copy-in/copy-out, task-local synchronous faults and complete reclamation. Retain one AMD64 CPU, BIOS/PIC/PIT, 4 KiB pages and the first-GiB physical allocator limit.

Threat: a user program can execute arbitrary instructions, corrupt its own stack, choose arbitrary syscall registers/pointers/lengths, spin forever and fault repeatedly. It must not read/write kernel or peer memory architecturally, acquire authority by naming an object, or exhaust resources without a bounded refusal. Kernel code, boot image and firmware remain trusted. This does not establish resistance to speculative side channels, DMA, physical access or compromised privileged code.

No dynamic loading, ELF compatibility promise, fork, shared address spaces, multiple user threads per process, demand paging, mmap, JIT, TLS API, signals, SMP or persistent executable storage in M3a. No filesystem, networking, graphics, cryptography or domain integration is introduced.

Proposed profile choice: permit M3a user execution only when NX is usable. On NX-absent hardware retain the explicitly identified M1/M2 kernel-only development profile and refuse user launch with `UNSUPPORTED`. This is a new proposed qualification rule, not a claim that NX absence makes U/S isolation impossible. A weaker userspace profile could be reviewed separately. Hardware support and security guarantees remain distinct.

## 3. Address-space and ownership contract

Use a deliberately small temporary virtual layout that avoids a higher-half kernel relocation in the same gate:

| Virtual range | Proposed use |
| --- | --- |
| Below `0x40000000` | Existing validated kernel identity region, supervisor-only; existing holes/guards preserved |
| `0x40000000`–`0x401fffff` | Per-process 2 MiB user arena; only declared pages mapped |
| All other addresses | Unmapped in M3a |

Each process owns a PML4, PDPT, user PD and user PT. Existing kernel PD/PT tables can be shared read-only as topology, while leaf edits remain serialized kernel operations. PML4[0] must permit user traversal; PDPT[0] remains supervisor-only for the kernel branch and PDPT[1] permits the private user branch. Test effective permissions at every level; do not assume a leaf U bit overrides an ancestor. User mappings are never global; keep PCID disabled and reload CR3 when changing address spaces.

Proposed fixture bounds: at most two user processes and one user thread each; at most 64 user pages per process including a four-page stack, plus four private table pages and five kernel-stack pages (four usable plus guard). The maximum is 73 allocated pages per process, excluding preallocated process metadata. Check this admission budget against actual free pages at boot; retain a kernel reserve and reject insufficient memory atomically. Other arena pages remain absent, with an unmapped page below the stack and unmapped edges around image segments where feasible.

Code RX, constants R/NX, data/BSS/stack RW/NX. Never map W+X. Zero every new user page and page-table page before installation, including padding and BSS. Copy image bytes through supervisor mappings before publication. Then make the identity alias of executable frames read-only/NX; no writable physical alias may survive publication. Mapping helpers must separate generic page permissions from stack guard operations. Keep executable frames immutable until the process is stopped and unmapped; restore allocator-facing identity permissions only after no runnable context or stale translation can use them.

Maintain an explicit per-process owned-extents list, independent of virtual addresses; only those frames may populate its user PTEs. Reject MMIO, reserved pages, page tables and another owner's frames. No user operation accepts physical addresses. Build privately, validate completely, then publish READY. Any failure unwinds mappings, alias changes, stacks, page tables and accounting in reverse order.

Process lifecycle: BUILDING → READY/RUNNING → DYING → REAPABLE → FREE. A fault/exit first prevents redispatch. Switch to a live stack and a different CR3 before reclamation; remove all scheduler/frame references before freeing. Reap individual dead processes from a safe kernel context while survivors continue. Clear data on its next exposure; do not promise immediate erasure or temporal safety for arbitrary kernel pointers.

## 4. Entry, return and execution state

Append ring-3 code/data GDT descriptors without moving the existing kernel/TSS selectors. Keep vector `0x81` kernel-only. Add one dedicated DPL3 interrupt gate, proposed `int 0x80`, with IST=0 so privilege entry uses the current thread's TSS.RSP0. Retain emergency IST stacks for their existing purposes. Use IRETQ first; SYSCALL/SYSRET is deferred.

During a switch with interrupts disabled, choose the next owned frame, install its CR3 and TSS.RSP0, normalize segment/base state, then return. All kernel stacks, entry code, GDT/IDT/TSS and emergency stacks must remain mapped identically supervisor-only across roots. Trap-frame offsets and assembly/C alignment remain compile-time asserted.

Never accept a user-supplied return frame. On every user return, validate canonical RIP/RSP within the user arena, exact user CS/SS, and a permitted flags mask; force IF=1 and IOPL=0, clear NT/VM and unsupported debug/control flags. Preserve only explicitly allowed arithmetic state. Invalid user return state terminates that process before IRETQ; unexpected kernel-frame corruption remains fatal. Tests must exercise bad return state and malicious segment changes as well as normal syscalls.

Preserve all GPRs except documented result registers. Normalize DS/ES and FS/GS selectors/bases for the no-TLS profile; do not depend on inherited user segment state in kernel C. Keep CR4.FSGSBASE disabled and no user base-setting API. Keep debug facilities, user performance-counter access and other unpreserved optional state disabled or explicitly qualified. Audit control-register/MSR initialization against Intel and AMD manuals before accepting arbitrary CPL3 instructions.

No FPU/SIMD until eager context support exists: configure an architectural denial policy (including CR0.TS/MP and disabled OS enablement for extended state), then test x87, MMX, SSE, AVX, state-save/restore and FWAIT paths. User #NM/#UD terminates the offender. Audit instructions not covered by a simplistic “TS blocks everything” assumption. Never enable lazy FPU switching as a shortcut. This is a merge prerequisite, not merely a build flag.

For M3a, syscalls are bounded and nonblocking; keep interrupts disabled during their short critical path. Yield/exit switch directly through the trap dispatcher. User computation remains timer-preemptible. Do not call existing sleep wrappers or take a scheduling interrupt recursively from syscall dispatch. General waits arrive in M3c.

Route synchronous user faults (#PF, #GP, #UD, #NM, etc.) to process death, recording a bounded local reason. Determine origin from saved CS, not merely the page-fault U/S error bit. Machine checks, double faults and unexpected kernel exceptions are not converted to ordinary app errors. Broad spurious IRQ/NMI recovery remains unqualified.

## 5. Built-in image and experimental ABI

Use a checked built-in image descriptor now, leaving ELF parsing to a later gate. Each descriptor carries version, bounded segment count, source offset/size, target offset/memory size, permissions and entry offset. Check arithmetic, source bounds, arena bounds, page alignment, overlaps, entry inside executable bytes, executable/writable conflicts, total page budget and BSS size. Reject shared pages between segments with conflicting permissions. Image labels are diagnostic identities, not signatures or verified package provenance.

Native development ABI v0: fixed-width 64-bit register arguments, explicitly numbered statuses, no kernel pointers and no stable ABI promise. Proposed registers: RAX operation/result status; RDI, RSI, RDX, R10, R8, R9 arguments; RDX optional scalar result. Unknown operations return `UNSUPPORTED`; reserved arguments must be zero. Suggested first operations:

| Operation | Contract |
| --- | --- |
| `abi_query` | Return version/profile as scalars |
| `yield` | Reschedule caller; no authority grant |
| `exit(code)` | Terminate current process; never returns |
| Test-only `copy_probe(src,dst,len)` | At most 256 bytes, validates both spans, stages through kernel scratch; no logging or persistent effect |

The probe provides real copy boundary tests and must be marked development-only. It is not an ambient debug-console capability. Production-facing diagnostics can become an explicitly granted endpoint later.

`copy_from_user`/`copy_to_user` accept numeric user addresses plus bounded lengths. Reject noncanonical addresses, wraparound, kernel ranges and permissions mismatches across every page before copying. Define zero-length operations as touching neither pointer. Use subtraction-based bound checks; never dereference before validation. Copy input once into kernel storage; handlers retain no user pointer.

Introduce small assembly copy sites with an exception-fixup table. Recovery requires a CPL0 #PF at an exact approved copy instruction, an active per-thread copy scope, CR2 within its declared user span and a valid kernel-owned counterpart. Clear recovery state on every path. Any other fault follows normal fault policy; never use a global “ignore kernel faults” flag. No nested copy scopes or scheduling while a copy is active in M3a.

All pages are resident and immutable during a syscall, and there is one thread per process, so prevalidation avoids normal partial-copy failures. Still define partial copy-out on a recovered exceptional fault as unspecified bytes within the validated output span; no authority-bearing operation may commit before successful output handling. The test probe stages input before output and accepts overlap. Future multi-threaded/mutable mappings require pinning or stronger VM synchronization; this shortcut must not silently survive that expansion.

## 6. M3b typed-handle contract to settle now

M3a reserves a process-owned handle-table field but exposes no placeholder handle syscall that reports success. Proposed M3b limits: 64 slots per process and bounded global object pools. A 64-bit token encodes slot plus a wide generation; zero is invalid. Exhausting the generation space permanently retires the slot. Do not wrap into an old token. Tokens are process-local selectors, not random secrets or globally transferable authority.

An entry holds a kernel-only object reference, immutable object type, granted rights, generation and lifecycle state. Lookup uses the current process's table and atomically checks slot/generation/type/rights/live state while acquiring an object reference. Copying another process's numeric token transfers nothing: it may be invalid or name an unrelated local handle. Binding authority to the local table is the guarantee.

Start with CHANNEL_ENDPOINT and EVENT objects; do not offer unimplemented device/file/memory privileges. Rights are type-specific, with explicit DUPLICATE and TRANSFER controls where allowed. Duplication creates an independently closable handle with a subset of source rights; unknown bits, wrong types and expansion fail before mutation. Close invalidates the entry before dropping its object reference. Closing one duplicate is not transitive revocation.

Under BSP interrupt exclusion: reserve destination slot/reference, validate, then publish atomically or unwind. Allocation and user copies occur outside object critical sections. Destruction runs after unlocking, when handle, queued-message, wait and in-flight references are zero. Reference overflow fails closed. Maintain per-process charges even when objects are shared; quotas cannot be bypassed by transferring ownership.

Service instance generation is separate from handle-slot generation. Future supervisor restart creates new objects/endpoints; old references never bind to a replacement instance. Explicit revocation, when added, requires an object state/epoch checked on each operation, not an assumption that closing the supervisor's copy revokes all clients.

## 7. M3c IPC contract to settle now

First transport: kernel-mediated copied messages over explicitly installed endpoint pairs. No global name lookup, raw pointer payload interpretation, shared-memory grants or zero-copy. Proposed bounds: eight channel pairs system-wide; sixteen messages per direction; 256 payload bytes and at most one transferred handle per message. Preallocate queue storage; account endpoint/queue resources at creation and pending messages at admission.

Fixed-width v0 header: version, byte length, transfer count, flags and request ID; all reserved fields zero. Specify field offsets, little-endian encoding and total size with static assertions. Never copy an internal C object or uninitialized padding onto the wire. Request IDs correlate messages; they do not authenticate a sender. The endpoint capability and kernel-bound peer instance supply identity; payload identity strings grant nothing.

Start nonblocking send/receive, returning `WOULD_BLOCK` on full/empty queues. Add bounded wait separately: one wait per thread, monotonic absolute deadline, and exactly one terminal outcome among readiness, timeout, cancellation and peer closure. Readiness is advisory; the next operation may still return `WOULD_BLOCK`. Register condition check and wait state under the same exclusion as enqueue/close/wakeup to prevent lost wakeups. No allocation, user-copy or sleeping while holding that exclusion.

Send transaction: validate/copy payload, validate transfer source/rights and quota, reserve queue capacity, retain the object reference, then enqueue once. Failed sends leave no message or rights change. Initially transfer by duplication with attenuation; defer MOVE semantics. Initially only EVENT handles are transferable; channel endpoints are installed by the authorized bootstrap/init path and cannot be attached to messages. This avoids reference cycles from queued channels; broader transfer requires a reviewed cycle/lifetime policy.

Receive transaction: reserve the head message, validate output spans and destination handle capacity, reserve a local slot if needed, copy payload/result, then atomically publish the new handle and consume the message. Failed output or capacity checks restore the reservation and leave the message queued. No raw sender token becomes a receiver token. Define short-buffer refusal with required length and no dequeue. No user scheduling inside this bounded receive transaction in the initial single-thread/process model.

Last-close semantics: queued outbound messages may drain; after drain the peer sees `PEER_CLOSED`. Unread messages destined for the closed endpoint are discarded and their transfer references released. Death closes the process's handles and cancels its waits. Restart never reconnects old queues. Successful send means enqueued, not executed; a receive timeout cannot undo a server-side action. Later persistent services need idempotency/outcome-unknown rules.

Minimal init starts from a fixed bounded launch plan and explicitly distributed startup handles; it is not the future configurable service manager. Launch/control capabilities belong to init, not every child. Local principal/session fields can remain opaque internal identities until separately specified; neither a name nor a future administrator role bypasses capability checks. E2E encryption, account sync and domain membership remain future policy/services work.

## 8. Reviewable implementation sequence

| Change | Files/areas | Exit evidence |
| --- | --- | --- |
| A: freeze contracts | New `docs/adr/0007-m3a-userspace-boundary.md`, M3a design note | Address layout, profile refusal, ABI/error/lifetime rules agreed |
| B: VM ownership and loader | `paging.c`, new `vm.c`, `process.c`, built-in image fixtures; narrow headers | Permission walks, zeroing, malformed images and allocation-failure rollback |
| C: CPL3 entry and switching | `trap.c`, `trap_entry.S`, `trap.h`, `sched.c`; feature initialization | Two roots and real CPL3 frames; TSS.RSP0 and context checks; fault-local death |
| D: syscall/copy boundary | New syscall dispatcher and user-copy assembly/fixups | Adversarial pointers, bounded requests, controlled recovery, no arbitrary kernel recovery |
| E: containment qualification | Hosted tests, `tools/integration.py`, emulator fixtures, CI | Positive and negative runtime evidence; reclamation with live survivor |
| Later B gate | New object/handle modules and tests | Stale/type/rights/lifetime/rollback semantics pass |
| Later C gate | IPC/wait modules and minimal init | Transfer, queue pressure, close/timeout/cancel races pass |

For each behavior change update IMPLEMENTATION, HANDOFF, ROADMAP and SECURITY with implemented/tested/planned/blocked entries. Keep experimental ABI separate from internal `status.h`. Update the invalid-selector test and its expected error when GDT layout grows. Preserve existing M1/M2 regression workloads in distinct boot modes.

## 9. Acceptance matrix and merge gates

| Test | Required observed outcome |
| --- | --- |
| Two tasks use same user VA with different patterns | Kernel confirms CPL3, distinct CR3 roots and physical frames; patterns preserved through timer preemption |
| Endless user loop | Other user task and idle make progress without voluntary yield |
| Kernel read/write/execute; unmapped peer-only VA | Exact expected task fault; victim dies; survivor continues and kernel state remains intact |
| User code write; data/stack execute; guard overflow | Permission fault local to offender; no emergency-stack corruption |
| I/O instructions, CLI/HLT, privileged software vectors | User failure contained; no authority gained |
| Malicious flags/segments and unsupported CPU state | Validated return or local rejection; no stale cross-process state |
| Null, noncanonical, wrapping and boundary-crossing pointers | Explicit error, no kernel panic, no committed effect |
| Copy fixup fault injection versus unrelated kernel fault | Only named copy site recovers; unrelated kernel corruption remains fatal |
| Poisoned reused pages and image padding | User sees zeroed data, no prior-owner bytes |
| Failure after each loader allocation/map step | Exact restoration of page counts, aliases and process slots |
| Process faults while peer remains live | Dead process is safely reaped; repeated cycles do not leak resources |
| NX absent and 16/1536 MiB profiles | Explicit user-launch refusal without NX; bounded admission with NX; first-GiB clipping preserved |
| Later handle tests | Stale reuse, forged value, wrong type, excess rights, slot exhaustion and close/use interleavings fail safely |
| Later IPC tests | Full queue, short buffer, invalid destination, transfer rollback, peer death, duplicate close and deadline/cancel races preserve exact accounting |

Run `make check`, `make smoke`, `make integration`, new user-boundary modes and `git diff --check`; use hosted sanitizer tests for portable validation/ownership logic. Negative QEMU tests need exact fault/result markers plus continued survivor progress. A timeout, emulator exit or single success string is not containment evidence. Test parent and leaf permissions and writable aliases, not only linker flags.

M3a is complete only after A–E pass with independently observed CI on its exact head. Describe the result as “experimental single-CPU userspace isolation in qualified emulator profiles.” Do not call it a hardened sandbox or M3 complete. Hardened qualification still needs entropy/canary decisions, general synchronization review, spurious interrupt policy, transition security review and explicit platform threat limits.

## 10. Source map

All repository links below pin the reviewed PR head, avoiding drift:

- [Roadmap and M3a/b/c split](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/ROADMAP.md)
- [Implementation ledger](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/IMPLEMENTATION.md), [handoff](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/HANDOFF.md)
- [Design §§7–9](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/DESIGN.md), [security requirements](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/SECURITY.md)
- [Paging](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/kernel/arch/x86_64/paging.c), [trap dispatch/GDT/TSS](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/kernel/arch/x86_64/trap.c), [trap entry](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/kernel/arch/x86_64/trap_entry.S), [scheduler](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/kernel/core/sched.c)
- [Init](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/INIT.md), [identity](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/IDENTITY.md), [encryption](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/ENCRYPTION.md), [domains](https://github.com/auto44e/jeios/blob/c5eb7cb3f7c8787ba4a883ca692c98993f67f05b/docs/DOMAINS.md)
- [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): implementation must cite the selected revision's paging/protection, IA-32e interrupt/TSS, IRET, control-register and extended-state instruction sections. Pair with current AMD architectural documentation before hardware qualification; no new hardware test is claimed here.

All numeric limits, ABI assignments, new file names and policy choices in sections 2–9 are recommendations for review, not pre-existing owner approvals or implemented interfaces.
