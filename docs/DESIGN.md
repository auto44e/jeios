# JeiOS system design

Version: 0.2, 2026-10-12. Status: long-term architecture plus M1/M2 BSP implementation.
Audience: the project owner, kernel contributors, system developers, and future
AI agents. This is the primary design document; implementation evidence lives in
HANDOFF.md. Requirements use **must**, **should**, and **may** deliberately. A
requirement here is not a claim that the source currently implements it.

## 0. Current implementation versus target design

M1 exception entry, validated boot-memory discovery and protected mappings are
implemented. M2 adds bounded page/heap allocation, PIC/PIT interrupts, seven
preemptible guarded kernel workers, sleep/wakeup and exit/reaping. These are
single-CPU AMD64 mechanisms; there is no userspace or sandbox yet. The runtime
self-test checks registers, memory and resource accounting before halting.

Read [IMPLEMENTATION.md](IMPLEMENTATION.md) for exact limits and current contracts,
and [HANDOFF.md](HANDOFF.md) for observed tests. Historical mentions of M0 describe
the original scaffold. All other subsystem designs below remain requirements or
proposals until the implementation ledger says otherwise. In particular the first
heap is page-granular, not a slab allocator; general wait queues, canary/entropy
setup, SMP and hardened application boundaries remain future work.

## 1. Product intent and constraints

JeiOS starts a new kernel rather than mechanically combining existing kernels.
The long-term objective is an everyday system which feels coherent for users,
provides structured interfaces for developers, and remains comprehensively
customizable for enthusiasts. The same underlying services should eventually
serve workstation, server, mobile, and appropriate 64-bit IoT profiles.

The owner has specified a C OS core, modular architecture, old 64-bit machine
compatibility, modern CPU/GPU enablement, graceful failures, security/privacy
inspired by GrapheneOS and CalyxOS, granular complexity, a macOS-inspired top
level, and Linux-style desktop ricing. Earlier discussion supplies architectural
inspiration from Linux, BSD, and NT, not a requirement to merge their source.

Concrete constraints:

- Hardware support and security support are separate promises. Old machines may
  boot without satisfying a modern hardened profile.
- Modern features must not leak into baseline instruction paths.
- Interface simplicity must not obscure failures or increase authority.
- The OS must support offline use without a vendor account or telemetry.
- APIs must be explicit enough to audit, test, and describe to future agents.
- A fresh kernel has no automatic access to Linux's hardware ecosystem or native
  Windows/macOS applications. Driver and ABI compatibility are distinct projects.
- No claim of production reliability, formal verification, performance superiority,
  or equivalence to mature hardened OSes is justified by the initial scaffold.

### 1.1 Non-goals for the first release sequence

M0-M3 do not target a complete desktop, accelerated GPUs, cellular service, a web
browser, Linux kernel module loading, WDM/KMDF compatibility, Win32, Darwin binary
compatibility, or secure daily use. The first useful target is a reproducibly
booting and diagnostically understandable kernel with isolated userspace.

## 2. Architecture decision: modular core with service isolation

Adopt a small privileged kernel with explicitly layered internal subsystems and
an NT-inspired executive vocabulary. Critical execution mechanisms stay in
kernel space: traps, scheduling, address-space control, capabilities, IPC,
interrupt routing, and the minimal device primitives needed for progress.
Higher-level policy and suitable drivers should run as isolated user services.

This is a design direction, not a declaration that M0 is a complete microkernel.
Initially all implemented C runs at ring 0. We will decide service boundaries by
measured performance, recovery behavior, driver requirements, and trust budget.
A large hybrid core can evolve from the wrong defaults; each new privileged
subsystem therefore needs a documented reason to be privileged.

| Layer | Responsibilities | Intended trust and ownership |
| --- | --- | --- |
| Architecture/platform | Entry, traps, page tables, CPU state, interrupt controller, firmware discovery | Kernel, narrow architecture-specific contracts |
| Kernel mechanisms | Threads, memory objects, handles, IPC, timers, IRQ/DMA grants | Kernel; minimal authority and bounded operations |
| Executive interfaces | Objects, asynchronous I/O, device lifecycle, security checks | Mechanisms in kernel; management policy in services |
| System services | Configuration, storage, network, package/update, diagnostics, audio/display brokers | Separate processes with explicit capabilities |
| Application environment | Native SDK, POSIX layer, later compatibility environments | Sandboxed processes and scoped brokers |
| User experience | Stock shell, settings, file manager, compositor policy, theme/widget services | Replaceable interfaces; no inherent extra privilege |

### 2.1 Why this shape

A single scheduler and VM manager prevent contradictory ownership and locking.
NT-inspired objects and requests offer predictable developer conventions; they
do not require reproducing NT's internal implementation. BSD-style capability
ideas and Unix conventions can coexist with a coherent native object API.
Linux-derived lessons guide device breadth, feature negotiation, and scalable
execution. The macOS-inspired experience is implemented through public services,
not through hidden shell-only authority.

Rejected as initial approaches: running three native kernel implementations in a
single address space; loading arbitrary foreign binary drivers; and designing a
novel stable driver binary ABI before a real device lifecycle exists. Full
microkernel separation remains a possible outcome, but committing prematurely to
an unmeasured IPC cost or forcing every driver into userspace would also be risky.

## 3. Modularity means three different things

**Build modules** have separate source ownership and dependency contracts. M0
supports this form. **Loadable privileged modules** add code to the kernel address
space and are fully trusted. **Isolated service modules** have process or VM
boundaries and capability-scoped interfaces. Only the last category contains
arbitrary memory corruption in the way people often expect from modularity.

### 3.1 Build-time module contract

Each subsystem must describe provided interfaces, dependencies, owned resources,
initialization order, partial failure cleanup, and shutdown preconditions. The
initial `je_module` list is explicitly ordered, validated before side effects,
and rolled back in reverse order on failure. A failed initializer undoes its own
partial allocations. Successful initializers have infallible cleanup callbacks.
M0 has no dependency resolver, version loader, concurrency, or restart manager.

Next registry design: stable subsystem names, versioned interface descriptors,
acyclic dependency graph, optional dependencies, required-versus-optional boot
criticality, bounded initialization deadlines, and explicit diagnostic status.
Reject duplicate names, cycles, unsatisfied versions, and inconsistent readiness.
Do not attempt to restart a kernel module after suspected corruption of shared
memory. Optional features may remain unavailable while boot continues.

### 3.2 Future privileged loading

Keep kernel internal APIs source-level and versioned by build at first. A module
manifest should bind architecture, exact kernel ABI/build identity, dependencies,
capabilities requested, integrity signature, and provenance. Signing validates
origin and authorization, not freedom from bugs. No auto-download during boot.

Loading requires executable mapping validation, relocation bounds checks,
W^X, an authorized policy decision, and an unpublished-to-live state transition.
Unloading must stop new calls, quiesce existing calls and DMA, revoke handles,
remove IRQ callbacks, synchronize references, then free code. A busy module stays
loaded. Never free a module because a timeout elapsed while callbacks still run.
Unsigned development modules require a deliberate development trust profile
with visible consequences independent of UI difficulty settings.

### 3.3 Service lifecycle

Use states: discovered -> authorized -> starting -> ready -> quiescing -> stopped;
failed and quarantined are distinct terminal/control states. A supervisor may
restart only when its restart contract permits it. Track monotonic generation
numbers so replies and capabilities from an old instance cannot target a new one.
Backoff, restart budgets, and crash loops prevent an unstable driver from
consuming all resources. Dependencies must see service unavailable rather than
waiting forever. Persistent state requires recovery before readiness.

## 4. Language, tools, and memory discipline

C11 is the baseline language for the kernel and core services. GNU assembler is
used only where exact architecture instructions or entry conventions demand it.
C cannot establish its own initial stack/page tables or fully describe trap
entry. Host tooling may use Python; later UI language selection is open.

Use fixed-width types for serialized and hardware fields. Internal `size_t` and
pointers must never enter on-disk or IPC formats without a defined encoding.
Check addition, multiplication, alignment, offsets, and truncation before using
untrusted lengths. Define signedness and endianness. Avoid C bitfields for device
registers. Volatile is not a lock or a cross-core memory-ordering mechanism.

Every public core interface must state whether it allocates, sleeps, may be
called with interrupts disabled, accepts user pointers, retains a pointer,
transfers ownership, or invokes callbacks. Document output state on failure.
Use explicit statuses; do not conflate unsupported, denied, busy, exhausted,
timed out, and hardware failure. Destructors must safely handle every legal
partial initialization state, not arbitrary invalid memory.

### 4.1 Hardening sequence

Compiler stack protection remains disabled pending canary initialization and
a validated failure/entropy strategy; this must be resolved before claiming a
hardened userspace boundary. M1/M2 now provide guarded stacks, validated mappings,
zero-initialized heap/stack allocations, and non-executable data where supported. Later add control-flow protections supported by the
compiler, hardened heap metadata, delayed reuse or quarantine where useful,
randomized layouts, and verified entropy before claiming cryptographic security.

Use compiler warnings as errors, host UB sanitizers for portable algorithms,
address sanitizers where available, fuzzers for parsers, and static analysis.
Portable helpers can be hosted; hardware paths need emulation and real hardware.
A successful build does not prove memory safety. Memory tagging reduces certain
risks and is not complete temporal/spatial safety. Do not hand-roll cryptography.

### 4.2 Build constraints

Baseline AMD64 objects use `-march=x86-64`, no red zone, no floating point/SIMD
register generation, freestanding compilation, and no hosted runtime dependency.
Future optimized translation units carry separate target flags and cannot be
called without feature enablement. Toolchain upgrades must preserve baseline
code generation and boot evidence. Production builds need pinned compiler,
linker, firmware/bootloader inputs, reproducibility reports, and artifact hashes.

## 5. Boot and platform abstraction

The first platform is QEMU's legacy PC using GRUB Multiboot2. It is a convenient
experimental entry path, not the final boot trust architecture. The ELF lives
at 1 MiB, enters in 32-bit protected mode, establishes a three-level path into
four-level paging, identity maps the first GiB using 2 MiB pages, loads a GDT,
sets EFER.LME, and enters 64-bit C with an aligned stack. The BSP alone runs.
Interrupts remain disabled during early setup. CPUID checks long mode before
using the long-mode MSR. M1 validates and copies loader data; M2 enables IRQ0 only
after memory and thread state are initialized.

The assembly and linker enforce placement within the bootstrap map. ELF segments
are RX, R, and RW, but the initial page tables are temporarily RWX; segment flags
alone are not hardware enforcement. M1 installs an IDT/TSS and then replaces
those maps with read-only code/rodata, guarded stacks and NX data where available.
Unexpected kernel faults produce a fatal diagnostic. Userspace recovery and
process separation are still required before running untrusted code.

### 5.1 M1 boot information contract

Parse the Multiboot2 block only after verifying its address range is mapped,
minimum fixed header size is accessible, total size has sane upper limits, each
tag has a minimum size, aligned increments cannot overflow, and the terminal tag
is consistent. Validate memory-map stride, types, overlapping regions, arithmetic,
reserved ranges, and the block's lifetime. Treat firmware tables as untrusted.
Reserve kernel, stack, boot block, page tables, modules, firmware, MMIO, and low
memory before allocator use. Never treat all identity-mapped addresses as RAM.

Translate loader-specific data into an internal `boot_context` with explicit
physical ranges, entry capabilities, framebuffer metadata, firmware handles,
and a measured/trust-state descriptor. It must never carry unchecked firmware
pointers into unrelated services. Copy or pin retained information and document
when loader memory can be reclaimed.

### 5.2 Platform evolution

Add UEFI x86-64 and later AArch64 entry paths sharing only the validated internal
context. Legacy BIOS support remains a distinct boot profile. AArch64 bring-up
needs exception-level normalization, granule discovery, translation setup,
cache maintenance, vector table, timer, GIC, PSCI, and DT/ACPI handling as
appropriate. CPU ISA compatibility alone does not imply a board can boot.

Platform interfaces should cover physical memory, timers, CPUs, interrupts,
firmware discovery, MMIO attributes, reset/poweroff, and safe DMA primitives.
Keep ACPI/DT parsing distinct from the generic device graph. Hardware errata and
firmware quirks must be named policies with affected versions and tests.

## 6. CPU compatibility and feature enablement

Interpret 'oldest 64-bit system' as a long-term per-ISA goal, beginning with early
AMD64 PCs. Alpha, SPARC64, MIPS64, POWER, IA-64, and other historical systems are
separate ports with different firmware, endianness, drivers, and toolchains;
they are not covered by an AMD64 binary. First AArch64 baseline target is Armv8-A
on a selected emulator/board, not older non-AArch64 64-bit architectures.

For every feature maintain four facts: hardware present, platform usable,
OS implementation ready, and policy allowed. Enabled is their intersection.
The initial portable helper combines hardware/OS/policy masks only; a future
platform mask must be added when firmware/device prerequisites are real.
Required features missing from the usable set cause explicit refusal, not a
false success or silent substitution with weaker isolation.

### 6.1 x86 specifics

Discover leaf availability before querying CPUID. Use architecture-defined tests
for NX, XSAVE family, SMEP/SMAP, CET, protection keys, virtualization, invariant
TSC, topology, and performance/power hints. CPUID advertising AVX does not alone
permit AVX execution: CR4.OSXSAVE, XCR0 bits, and adequate per-thread context
storage must be configured. AMX has large extended state and additional policy
requirements. Do not run those instructions in baseline kernel C.

Use eager, validated state restoration initially; avoid speculative/lazy
approaches with poorly understood security consequences. Size XSAVE buffers from
validated architectural enumeration, honor alignment and instruction format,
and initialize state before cross-process use. Keep debug state and future
protection registers in the context-switch contract. Precise implementation must
cite current Intel and AMD manuals and relevant errata, not only Linux examples.

### 6.2 AArch64 specifics

MTE labels allocations and can detect classes of memory misuse. PAC authenticates
selected pointers under configured keys. BTI constrains some indirect branch
entry behavior. Permission overlays can reduce access within an address space;
POE2 adds richer compartment controls. RME/CCA Realms target a different trust
boundary for confidential workloads and depend on hardware, firmware, a monitor,
and virtualization support. TrustZone is another distinction, not a convenient
per-application isolation API for ordinary userspace.

Treat feature availability as register-discovered plus platform-qualified. Save,
restore, and initialize per-thread tagging/control/protection state. MTE tag
storage, allocation alignment, synchronous/asynchronous fault mode, signal/debug
semantics, and ABI pointer tagging must be designed together. SVE/SME context is
variable-size; vector-length choices affect migration and signal formats.
Architectural descriptions of new features do not establish that current retail
hardware exposes them. Research-only targets stay research-only in the support
matrix until execution tests exist.

### 6.3 Heterogeneous scheduling

A scheduler can exploit performance/efficiency cores, cache topology, NUMA,
vector capabilities, thermal state, and power budgets only when the platform can
report them reliably. Initially use straightforward per-CPU queues and explicit
classes rather than inventing a complex algorithm before correctness.

Migration must respect each task's required instruction and state feature set.
Global portable exposure may be the intersection across online CPUs; opt-in
specialized tasks may use restricted affinity. CPU hotplug cannot silently strand
such tasks or move them to an incompatible CPU. Expose latency-oriented versus
throughput-oriented intent with quotas and admission control, not unlimited
realtime priority. Device IRQ affinity and DMA locality are part of placement.

## 7. Memory and virtual address spaces

Maintain separate physical page ownership, virtual mappings, and memory object
rights. Pages have explicit states: free, kernel, user-owned, pinned for DMA,
shared, device/MMIO, and quarantined. The allocator must never return reserved
or device memory. Zero memory before exposing it to another security domain.

Address spaces define permissions, executable policy, backing, fault behavior,
and share semantics. Default user mappings are non-executable; executable code
cannot also be writable at the same time where enforcement is possible. JITs
use a mediated transition or carefully specified aliases with a bounded threat
model. Kernel code is not writable; read-only metadata stays read-only. On
hardware without NX the OS cannot guarantee non-executable data even if its
mapping model records an intended permission.

Implement checked user-copy functions that recover from validly contained page
faults without making arbitrary kernel faults recoverable. Kernel pointers never
cross the syscall boundary. Guard stacks and emergency exception stacks require
separate mappings and verified trap entry. TLB shootdown must be coordinated
before freeing or repurposing pages. Page table edits must avoid stale access
rights across CPUs and devices.

### 7.1 Allocation and pressure

M2 uses reserved static bootstrap metadata, a physical page allocator and a
bounded page-granular heap. A slab/size-class heap is a later efficiency step. Account memory by security domain, including pinned device
buffers and driver metadata. No unbounded allocation in interrupt context. Use
preallocated pools for critical completions and diagnostics. Exhaustion returns
an explicit status or invokes a documented pressure policy.

Reclaim must avoid killing the update/storage broker while it owns a transaction.
Define emergency reserve access, per-domain limits, and out-of-memory victim
selection. Invalid heap metadata is suspected corruption: terminate the affected
isolated service or stop the kernel if the integrity boundary is kernel memory.
Do not continue using an allocator whose metadata has been corrupted.

## 8. Threads, processes, and execution context

A process owns an address space, handle table, security domain, resource account,
and executable identity. A thread owns stack, registers, priority, CPU affinity,
feature requirements, and cancellation state. Shared address spaces and shared
handles are explicit operations, not ambiguous defaults.

Implement a kernel idle thread, timer-driven preemption, sleeping wait queues,
and race-free wakeup before userspace concurrency. Publish lock ordering,
interrupt-safe lock rules, and context in which sleeping is forbidden. Use C11
atomics and architecture barriers with a documented memory model. Start with
coarse locking where understandable; optimize only with contention evidence.

Priority inversion needs bounded mitigation or inheritance for latency-critical
paths. Cancellation is advisory until a defined point; it cannot invalidate
memory still referenced by a completion. Real-time audio should eventually use
preallocated buffers, explicit deadlines, and a supervised graph; it cannot be
made reliable simply by elevating every audio thread to maximum priority.

## 9. NT-inspired objects and native APIs

Objects represent processes, threads, memory regions, channels, events, timers,
files, devices, and I/O requests. Users receive opaque handles with rights, not
raw kernel pointers. A handle table includes generation counters so slot reuse
does not revive stale references. Explicit rights attenuation can create weaker
handles; duplicating a handle cannot add rights.

Names support discovery but do not grant authority. A global object namespace
would be a powerful leak if unconstrained; provide scoped service namespaces and
mediated lookup. Object operations must validate type, generation, domain, and
rights atomically against lifetime transitions. Capability transfer through IPC
must explicitly specify transferred rights and rollback if enqueue fails.

POSIX UID/GID/ACL compatibility can map onto a separate credential model without
letting a legacy root assumption bypass native capability checks. An initial
native ABI should be versioned and small. Stable userspace syscall numbers and
structure layouts follow only after tests establish semantics. Internal kernel
interfaces are not promised stable merely because the userspace API is stable.

### 9.1 Asynchronous I/O

Use a request ID, target handle, operation, checked buffer references, deadlines,
cancellation policy, completion destination, and quota accounting. Requests move
through created, validated, queued, executing, completing, and completed states.
Cancellation and completion races must have exactly one terminal completion.
A timeout means the caller stopped waiting; it does not prove hardware stopped
DMA or writes ceased. Drivers must independently quiesce and report effects.

Completion ports/queues offer scalable notification; integrate them with GUI and
CLI event loops through the same service APIs. Bound queue depth and backpressure.
Partial reads/writes, short buffers, device removal, and interrupted operations
have explicit result fields. Avoid an IRP clone whose fields are arbitrary shared
kernel pointers. Design buffer grants and ownership first, fast paths second.

## 10. Device framework, drivers, and DMA

The device graph contains discovered devices, bus topology, resources, matching
IDs, power states, dependencies, and trust assignments. Discovery cannot
arbitrarily bind a downloaded driver. The broker selects an authorized driver
whose manifest describes supported devices and resource requests.

Lifecycle: discovered -> resources assigned -> initialized -> active -> quiescing
-> stopped/removed. Power transitions and hot removal have explicit invariants.
MMIO mappings expose only approved ranges with correct cache attributes. IRQs
are delivered through bounded notifications or verified kernel callbacks.
Driver failures can revoke future access but revocation of already-issued DMA
requires hardware-level quiescence and translation invalidation.

### 10.1 IOMMU policy

User-mode drivers are not strongly isolated if a device can DMA into arbitrary
RAM. An enabled and validated IOMMU plus safe interrupt remapping is a prerequisite
for the strong driver containment claim on relevant platforms. Devices without
isolation groups or reset support may have to share a trusted domain. Avoid
unrestricted bus mastering before mappings and handlers are ready.

Legacy mode can allow selected trusted drivers without IOMMU, visibly classified
as a weaker containment profile. A request for hardened driver isolation must
fail if unavailable; process separation alone is not a fallback equivalent.
Untrusted hotplug DMA-capable hardware is disabled by default on weak platforms.

### 10.2 Driver ecosystem realism

A new kernel must implement or port drivers. LinuxKPI-like source adapters may
be considered after the native lifecycle exists; they reproduce semantics, not
just names. Windows binary drivers need substantial NT and framework behavior.
GPU drivers depend on memory management, synchronization, interrupts, firmware,
and userspace interface contracts. A driver wrapper does not create this stack.
Maintain first-party simple drivers before adapting complex foreign frameworks.

## 11. GPU, display, media, and accelerator architecture

GPU support is layered: firmware framebuffer -> software rendering -> modeset
and display -> device-specific acceleration -> compute/media. A working CPU and
PCIe enumeration are not GPU support. New CPU compatibility does not imply a new
GPU has documentation or redistributable firmware.

A display service owns output topology and modeset policy. Applications submit
surfaces; the compositor operates through a standardized protocol. The GPU
service mediates memory objects, commands, fences, synchronization, context
scheduling, reset, and quotas. Validate commands according to each driver model;
prevent applications from programming arbitrary DMA or accessing another
application's VRAM. Reset may destroy contexts; clients must receive loss notices
and reconstruct resources. Software rendering is a valid fallback for daily
interaction but cannot claim accelerated performance.

Separate APIs for presentation, compute, video decode/encode, and inference.
Vulkan/Mesa integration is a future port requiring their host/runtime and driver
interfaces; it is not implemented by declaring support for those names. NPU
operators need appropriate model formats, compiler/runtime support, memory
movement, and scheduling. Do not treat CPU/GPU/NPU as interchangeable arbitrary
instruction engines. Kernel responsibilities are isolation and resource control;
user runtimes own most compilation and optimization.

### 11.1 Fast paths

Allow zero-copy surfaces, explicit fences, scatter/gather DMA, large pages where
appropriate, CPU-specific kernels, and low-latency queues once their validation
and accounting exist. Record correctness, throughput, tail latency, memory cost,
and power tradeoffs. Old hardware fallback must remain tested. Fast paths cannot
bypass capability validation or expose stale cross-domain buffers.

## 12. Storage, filesystems, and transactional operation

Begin with an in-memory filesystem and read-only init image. Next add a simple
block device and a proven on-disk format with explicit recovery tests. A VFS
contract defines identity, permissions, handles, mount namespaces, locking,
path resolution, symlinks, durability, and error reporting.

Updates and configuration must use prepare/validate/commit/health-check semantics.
Storage snapshots alone do not roll back firmware, external services, network
messages, or application data safely. A filesystem snapshot is not a backup.
A/B OS images should be separate from mutable user data. An unbootable new image
can return to an authorized known-good image within an anti-rollback policy.
Old vulnerable images cannot remain indefinitely accepted in a hardened profile.

A pending update journal records selected image, attempts, generation, and health
criteria. Power loss between every transition must be tested. Explicit fsync and
barrier semantics are essential; successful buffering is not durable success.
Disk error recovery must not silently discard user data. Encryption and integrity
need vetted implementations, secure key handling, and a documented boot-unlock
chain. No cryptographic feature is present in M0.

## 13. Networking and privacy controls

Start with local IPC, then a basic tested network device and a userspace or
carefully bounded kernel networking stack. Decide placement using throughput and
attack surface measurements. Namespaces, interfaces, routes, socket rights, and
firewall identities must agree on the process security domain.

Network permission is independently revocable per application and service.
Revocation must handle existing connections, child processes, shared services,
and DNS/proxy requests; otherwise blocking only socket creation is misleading.
Scoped permissions can distinguish local LAN, general Internet, background
access, and required VPN routing. Explain metadata leakage and avoid promising
anonymity from a firewall or encrypted DNS. A VPN must fail closed where a policy
requires it, including reconnects, DNS, IPv6, and captive portal handling.

No telemetry by default. Remote crash reports are user-initiated, previewable,
and redacted. Local logs have retention limits and access controls. Identifiers,
MAC behavior, discovery protocols, and network time policies must be disclosed.
A stock service must not acquire network access because a theme requests it.

## 14. Security domains and sandboxing

Use capabilities plus a mandatory policy layer to constrain powerful objects.
Each application declares requested operations; installation does not grant them
all. Brokers mediate file selection, screenshots, microphone/camera, clipboard,
sensors, location, device access, and network. A selected file yields a scoped
handle, not permission to crawl the whole home directory. Global keyboard
capture or compositor control requires a distinct privilege and visible state.

Policy checks happen at kernel/service trust boundaries, not only in UI buttons.
UI complexity, administrative authority, boot trust state, and security profile
are independent axes. The owner can develop their system without an account;
entering development mode must report which guarantees cease, such as trust in
unsigned privileged code. Expert mode must not enable raw DMA or disable W^X.

Threat model: hostile apps, malformed media/network/firmware data, buggy drivers,
malicious peripherals, physical loss, supply-chain compromise, and accidental
configuration damage. Complete protection from compromised firmware, hostile
hardware, all side channels, or invasive physical attacks is out of scope.
The platform matrix must state which threats are meaningfully mitigated.

GrapheneOS inspires memory hardening, attack-surface reduction, scoped permissions,
and careful treatment of security exceptions. CalyxOS inspires approachable
privacy controls and per-app network policy. Android mechanisms are not directly
available in a fresh kernel; we must implement and verify our own boundaries.
Never market 'GrapheneOS-level security' from architectural resemblance.

## 15. Graceful failures and recovery

The principle is containment and truthful reporting, not continuing through
unknown kernel corruption. Recoverability depends on where the fault occurred
and whether state remains trustworthy.

| Fault | Intended action | Evidence required |
| --- | --- | --- |
| Missing optional CPU feature | Use qualified fallback or explicit unsupported result | No illegal instruction; visible profile |
| Permission denied | Return stable denied status | No side effects before authorization |
| Application memory error | Terminate domain, preserve approved diagnostics | Other domains unaffected; resources revoked |
| Service crash | Quarantine/restart per contract | Generations revoked, budgets enforced |
| Driver timeout | Cancel wait, quiesce/reset device if safe | DMA stopped before buffer reuse |
| GPU reset | Invalidate contexts and notify clients | Cross-client memory not exposed |
| Storage fault | Report partial/durability effects, enter recovery | No false success or silent deletion |
| Kernel invariant failure | Minimal panic and controlled halt/reset | Panic path cannot depend on broken allocator |

Panic records should include stage, CPU, exception, sanitized context, and a
bounded log excerpt. Emergency console/dump paths use reserved memory and no
unbounded locks. Recursive faults use an even smaller last-resort path. Watchdog
and restart policies must distinguish liveness failures from intentional idle.
No automatic upload. Sensitive memory dumps require explicit local consent.

M1/M2 provide bounded fatal exception reporting, protected mappings, allocation
failures/rollback, and basic kernel-thread lifecycle checks. They do not recover
arbitrary kernel corruption. Crash dumps, supervisors, watchdogs and recoverable
userspace processes remain future milestones.

## 16. Executive services and configuration

System services expose the same typed operations to GUI, CLI, and automation.
Configuration is schema-versioned, validated, auditable, exportable, and applied
transactionally where possible. Separate desired configuration from observed
hardware state. A setting saved to disk but refused by a device must appear as
pending/failed rather than active.

Service descriptors define dependencies, granted rights, restart policy,
resource budgets, readiness signal, and update compatibility. Dependency cycles
are rejected before launch. Device/service names have stable identity separate
from user-facing labels. Maintain a monotonic event sequence and bounded event
history for diagnostics. Applications must tolerate service absence and retries
without duplicating irreversible actions.

Avoid a single privileged 'settings' daemon with unrestricted access to every
subsystem. Use brokers with narrow capabilities. Configuration transactions may
span multiple brokers; define preparation, timeout, commit ordering, compensation,
and persistence. Not every action is reversible: formatting a disk or transmitting
a message must not be described as rollback-safe.

## 17. Native application SDK and customizable desktop

The native framework standardizes behavior: accessibility, focus, keyboard
navigation, input methods, file dialogs, permission prompts, notifications,
window roles, and application lifecycle. Visual presentation uses public design
tokens, component schemas, icon/font packs, spacing, decoration, and animation
settings. Applications should not hardcode system theme colors or shell geometry.

Separate shell composition, window management policy, rendering, and privileged
input/display mechanisms. Support floating, tiled, tabbed, stacked, and mixed
workflows. Stock dock/panels and third-party shells use the same public APIs.
Users may combine a dock with tiling and programmable status panels without
patching every application. Compositor replacement is a session operation until
safe live handoff is demonstrated; it is not guaranteed to preserve surfaces
under every failure. A shell service restart should preserve apps where the
protocol and compositor lifetime allow it.

Themes are data by default. Executable widgets/scripts are isolated with bounded
resource use and explicitly granted capabilities. Accessibility overrides may
limit illegible themes or provide a user-controlled contrast mode. Apps with
custom renderers, games, or foreign toolkits can honor integration protocols
without being fully themeable. Do not promise uniform control over their pixels.

### 17.1 Granular complexity settings

Use presets such as Everyday, Advanced, Expert, and Systems as discoverability
choices, plus per-area overrides for networking, storage, devices, diagnostics,
and customization. Preserve access to search, undo history, failure details,
and exportable configuration in every mode. Complexity must never change rights,
security policy, installed service capability, or silently reveal sensitive data.

An expert networking view can be read-only. An everyday view can administer a
machine after proper authorization. Security prompts remain trustworthy and
accessible even with a custom theme; themes cannot impersonate trusted consent
UI. A recovery session uses a known-good minimal shell and ignores broken user
extensions. Recovery must also offer CLI/serial diagnostics where available.

## 18. Compatibility strategy

Native APIs come first, then a POSIX environment sufficient to build standard
portable tooling. POSIX source compatibility, Linux syscall/binary compatibility,
FreeBSD ABI compatibility, Windows user APIs, macOS frameworks, and foreign kernel
drivers are different scopes with separate acceptance suites.

A future Linux ABI environment needs syscall semantics, ELF loader, expected
runtime libraries, threading/futex behavior, signal frames, filesystem behavior,
and security mapping. A FreeBSD personality has its own layouts and conventions.
Wine-like user-space compatibility may later support selected Windows apps without
loading Windows kernel drivers. Darling-like approaches would not automatically
implement current Apple frameworks or licensed assets. No native application or
kernel driver compatibility is present in M0.

Use explicit compatibility domains so legacy requirements do not become the
native sandbox's ambient authority. If a program requests semantics we cannot
support securely, return an incompatibility error rather than a hidden global
security bypass. Publish tested application/version matrices instead of blanket
compatibility percentages.

## 19. Mobile and IoT ecosystem

Share object semantics, update format principles, capability policy, configuration
schemas, IPC protocols, SDK concepts, and source modules where practical. Device
profiles select UI, power budgets, sensors, and peripheral brokers. Cross-device
sync must be optional, authenticated, encrypted, revocable, and self-hostable in
principle. An ecosystem is not a mandatory vendor cloud account.

Mobile requires GPU/display, touch, suspend/resume, modem isolation, GNSS,
Wi-Fi/Bluetooth, audio, camera ISP, secure storage, and a supported boot/signing
chain. Vendor firmware and proprietary interfaces may block a device. Bootloader
relocking requires device-specific verified support; it cannot be promised for
arbitrary phones. Begin with a documented development board before consumer
phone support. Prefer long-supported firmware and publish support lifetimes.

64-bit IoT can reuse the core. Microcontrollers with no MMU or 32-bit cores cannot
provide the same process isolation architecture; a separate restricted runtime
would be another project with explicitly weaker guarantees. Headless profiles
need safe remote management, local recovery, minimal services, offline updates,
and power-failure tolerance. Cloud accessibility does not imply open inbound
ports or permanent remote authority.

## 20. Validation and release gates

Every milestone needs reproducible pass/fail criteria and explicit unsupported
features. Baseline and feature-enhanced paths must both run. Test malicious
lengths, stale handles, races, driver disappearance, device timeout, low memory,
and interrupted writes when those mechanisms exist. Fault injection is part of
normal development, not a late reliability exercise.

A hardware profile is not supported solely because its emulator boots. Maintain
real-machine evidence by firmware version, CPU model, devices, kernel commit,
configuration, and test date. Benchmarks report comparable workload, compiler,
mitigation state, CPU affinity, energy/thermal state, median and tail latency.
Do not remove mitigations and advertise the result as ordinary performance.

Release artifacts eventually need reproducible builds, signed manifests,
provenance, SBOM, review of imported code, trusted key custody, a tested recovery
path, and a vulnerability process. Keys never live in this repository. A tag
alone does not constitute a secure release. M0 is source-only experimental work.

## 21. Repository map and continuation rules

`kernel/core/` holds architecture-neutral core code. `kernel/arch/x86_64/` holds
entry, platform operations, and linker placement. `include/jeios/` defines current
internal contracts. `boot/grub/` is the initial boot recipe. `tests/` hosts portable
core tests, and `tools/` holds ELF and emulator verification. Services, drivers,
userspace SDK, and additional architecture directories should be created only
when they contain an actual bounded implementation or a clearly labeled spec.

Future agents must read actual source and HANDOFF before interpreting this design.
Update implemented/planned status with each milestone, cite sources for register
semantics, and record unresolved failure modes. A detailed proposal must never
be presented as implemented security. A small correct next step is preferable
to stubs claiming entire subsystems.

## 22. Open decisions and risks

- Owner license selection is pending; imported code cannot proceed without it.
- Exact hardened x86 baseline and minimum RAM are not yet selected; old AMD64 is
  a compatibility ambition, not a security-certification guarantee.
- Userspace-versus-kernel placement of networking/filesystems needs measurements.
- Native ABI wire layout, UI implementation language, toolkit strategy, and
  compositor protocol selection need concrete prototypes and ADRs.
- GPU/phone vendor documentation, firmware rights, and porting effort can dominate
  schedules. No dates or universal device-support promise are made.
- A new C kernel adds a large correctness and memory-safety burden. Hardening and
  isolation reduce risk only after implementation and validation.
- Strict feature support can constrain scheduler migration on heterogeneous CPUs.
  Conflating reported and usable features risks illegal instructions or leaks.
- Graceful recovery of userspace faults is feasible; recovery from arbitrary
  privileged corruption is not a safe universal guarantee.

These decisions should be resolved with evidence and the owner's product intent,
not assumptions buried in code. Record changes in ADRs and handoff notes.
