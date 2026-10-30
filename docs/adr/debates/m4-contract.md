# M4 contract debate (ADR 0010) — 2026-10-30

Peers: Claude (kernel-architect seat) and Codex (adversarial-reviewer seat), both read-only on main `dc41da5`; lead = Kiro. AI-assisted design, not independent human qualification. Round 1 ran via `peer-debate.sh`; its round-2 relay failed (the helper passed Codex's ~1.6 MB tool log as argv: "Argument list too long"), so round 2 was relayed manually with the clean round-1 answers. Codex tool-execution logs are omitted; only each peer's final answer is kept, verbatim.

## Lead brief

### JeiOS M4 contract (ADR 0010) — design brief for peer debate

JeiOS: fresh C11 kernel, x86-64 primary target on QEMU `pc`/qemu64 + PIT/PIC, single CPU, IF=0 kernel.
M3 is complete (main dc41da5). M4 = issue #53: native service supervisor + bounded device lifecycle.
Acceptance (#53): real QEMU service crash/restart/reconnect; bounded crash-loop exhaustion; spoofed/stale
readiness rejected; no authority revival or leaked resources; timeout/detach and failed-launch rollback.
Order fixed by NEXT_PHASE: (a) lifecycle+readiness, (b) failure/restart/reconnect, (c) scoped device
ownership/grants, (d) ONE basic virtual device with explicit DMA containment or a no-DMA profile.
No persistent storage, no desktop, no stable ABI, no hardened claim. AI review != human qualification.

## Verified facts about the code today (from a code read at dc41da5)
- Processes: `je_vm.procs[JE_VM_MAX_PROCESSES=2]` (vm.h) + scheduler `threads[JE_THREAD_MAX=8]` shared by kernel
  and user threads; 1 thread/process is structural (one 5-page kstack per process). Per-process quota 73 pages
  (4 table + <=64 user + 5 kstack); arena 2 MiB at 0x40000000; no mmap/heap/growth.
- Creation is kernel-only: main.c `vm_s3_build(prog)` copies an embedded asm stub (user_demo.S, <=1024 B) into a
  fixed 2-segment `je_vm_image` v1 (checked descriptor, not ELF; ADR 0007 D8). No spawn syscall.
- Death: fault/EXIT/bad-return -> `mark_dying` (releases IPC waiter) -> DYING; `je_sched_reap_dead()` must run
  in slot 0 (IF=0) polling from main.c; it calls drop_user + `je_vm_destroy` (teardown hook = endpoint release
  walk + peer-close wake) -> slot FREE. Exit status is delivered to NOBODY (kernel-internal accessors only).
- No process identity/generation anywhere. Handle tables are re-initialised on every build (gens restart at 1).
  Endpoint record generation is internal-only (ADR 0009 D-OWNER-1: "no shipped incarnation counter; terminal
  destruction, no restart-in-place"; reopens only if a non-owning alias/restart-in-place is introduced).
- ABI v0 (int 0x80): 0 ABI_QUERY, 1 YIELD, 2 EXIT, 3 COPY_PROBE, 4 (dev), 5 HANDLE_SELF, 6 HANDLE_CLOSE,
  7 HANDLE_QUERY, 8 HANDLE_RIGHTS, 9 HANDLE_DUP (attenuating, any holder may derive <= rights; no DUP right),
  10 ENDPOINT_CREATE (both ends in own table), 11 TRY_SEND (<=64 B, <=2 caps, needs TRANSFER to send a cap,
  never blocks), 12 TRY_RECV (timeout R10 ticks; 0 = poll; one waiter per end; bounded JE_IPC_MAX_WAIT).
  Statuses 0..15 frozen incl. PEER_CLOSED 12, EMPTY 13, TIMED_OUT 14, CANCELLED 15 (ABI-unreachable in v1).
  Object types SELF 1, ENDPOINT 2. Rights INSPECT 1, SEND 2, RECV 4, TRANSFER 8. New ops/statuses APPEND.
- IPC: channel = record with 2 ends; queue depth 4; pool cap 4 channels kernel-wide; receive is NOT exclusive
  (any holder of a RECV handle may recv). Cross-process channels can only be created by the kernel test fixture
  `je_endpoint_kern_seed` (not an ABI). PEER_CLOSED is terminal (drain then EOF); no reconnect.
- No SLEEP syscall, no wait-any/wait-set, no kill/signal, no timer object. Timer = PIT 100 Hz ticks.
- x86 has the TSS: an I/O permission bitmap per process is possible (none used today).

## Frozen constraints (owner-gated to change)
ADR 0007 D1 (2 procs, 1 thread, no shared AS/mmap) — security shape firm, scope limits FLEX for later milestones
but need ADR + owner sign-off. ADR 0007 D3 frame-kind rules (no user-mapped MMIO/page-table frames). ADR 0008
handle encoding/rights/Option A (restart = close+realloc+gen bump; no epoch in handle); D7 death path owner-gated.
ADR 0009 D2/D3/D7 (bounded waits, exactly one terminal result, send never blocks), D-OWNER-1/2, A6-CLARIFY-1
(park/resume sound only with immutable text + 1 thread/process; MUST re-gate if either changes). Kernel modules
are never restarted after suspected corruption (ADR 0002, DESIGN): crash-loop budgets apply to isolated services.

## Lead's DRAFT decisions (attack these)
D1 Scope/claim: x86-64 QEMU only; ARM gets M4 later. Claim = "a ring-3 supervisor can launch built-in images,
   authenticate per-generation readiness, observe death, restart within a budget, and a peer survives; one
   no-DMA device is owned by a ring-3 driver via scoped port-I/O". Nothing about SMP, ELF, files, stable ABI.
D2 Capacity (OWNER-GATED tested-code change): JE_VM_MAX_PROCESSES 2 -> 8; JE_THREAD_MAX 8 -> 16;
   JE_EP_POOL_CAP 4 -> 32; keep 1 thread/process (A6-CLARIFY-1 not re-gated). Re-baseline the M2 "7 workers"
   test to the new cap. Audit loop O(N^2*pages) acceptable at N=8.
D3 Process authority: new object type PROCESS (3) with rights INSPECT, WAIT(new bit 0x10), KILL(new 0x20);
   and a kernel-seeded LAUNCHER object (type 4, right SPAWN 0x40) given ONLY to the first process (the
   supervisor). spawn(launcher, image_id, grants[<=4 handles], ...) -> PROCESS handle. image_id indexes a kernel
   built-in image table (embedded stubs; still the checked descriptor; no ELF). Grants are transferred into the
   child's table at birth at fixed handle values (handle 1..n), transactional: any failure -> full rollback,
   no child. The child never receives its own PROCESS handle or the LAUNCHER.
D4 Death/reap/notice: kernel reaps automatically (reaper runs in slot 0 idle loop, IF=0) — no longer test-only
   polling. The PROCESS object outlives the vm slot: it keeps {reason, code, instance} until its last handle
   closes. proc_wait(h, timeout) -> TIMED_OUT | OK with reason/code (idempotent after death). proc_kill(h) =
   mark_dying via the SAME release owner (A6-CLARIFY-2), effective at the victim's next kernel entry/timer tick.
   Each launch gets a kernel-wide monotonic u64 `instance` (never reused); visible to the PROCESS holder only.
D5 Readiness authentication: per LAUNCH the supervisor ENDPOINT_CREATEs a fresh channel and grants the child
   one end attenuated to SEND only (no RECV, no TRANSFER, no INSPECT). Readiness = the first message on that
   channel with a fixed opcode before the deadline. Because the channel is fresh per generation and the child
   cannot forward its end (no TRANSFER) and nobody else holds it, a message there is authentic for that
   generation; an old instance's end refers to a destroyed record -> it can never reach the new one. Spoof test:
   a third process with some other handle cannot inject. Stale test: a killed generation's late message is
   impossible (its process is dead and its handles torn down) and a restart gets a NEW channel.
D6 Event multiplexing (OPEN — see Q1): single-threaded supervisor must wait on readiness channels, service
   request channels and deaths of N children with deadlines.
D7 Restart policy (userspace, supervisor-owned; kernel provides only mechanism): per-service restart_limit within
   restart_window, capped exponential backoff, aggregate budget; exhausted -> FAILED (terminal until operator).
   Units: ticks in the ABI; supervisor manifest in ms converted at 100 Hz; the supervisor implements deadlines
   longer than JE_IPC_MAX_WAIT by bounded re-arm. Numeric defaults chosen after measuring on QEMU.
D8 Clients/reconnect: clients never get lifecycle handles. Service discovery = the supervisor brokers: client asks
   supervisor (over its own granted channel) for service X; supervisor creates a fresh channel per (client,
   service generation) and transfers ends. On service death the client's channel goes PEER_CLOSED (existing
   semantics) — we map INIT's "service-restarted" to PEER_CLOSED + re-request (no new status) OR append a new
   status RESTARTED (16) (OPEN, Q4).
D9 State machine: adopt ONE service machine: inactive -> starting -> ready -> stopping -> stopped, plus failed,
   backoff; INIT.md's set; DESIGN's discovered/authorized are supervisor pre-launch validation, not states.
D10 Device (M4c/d): NO-DMA profile. Driver = ring-3 service owning COM2 (16550 UART, ports 0x2F8-0x2FF, IRQ3)
   via a per-process TSS I/O-permission bitmap granted by a kernel DEVICE object (type 5, rights PIO 0x80,
   IRQ 0x100). IRQ delivered as a kernel message on a bound endpoint (bounded: coalesced pending bit; never
   blocks the IRQ path). Grants revoked on driver death (bitmap cleared before any reassign); timeout = driver
   stops responding -> supervisor kills/restarts; detach = supervisor revokes the DEVICE grant (proc_kill + device
   reassign) and clients see PEER_CLOSED. Hardened/IOMMU requests return UNSUPPORTED ("legacy, no-DMA profile").
   No MMIO user mappings (keeps ADR 0007 D3 intact). Buffer grants in M4 = COPY-based (message payloads), no
   shared memory; zero-copy grants deferred to a later ADR.
D11 Classification: kernel parts and the supervisor are Critical profile; the UART driver runs in ring 3 but under
   a weaker "legacy trusted, no-DMA" tier (PIO cannot DMA on this device; a port bitmap bounds which ports).
D-SEQ: PR1 ADR; PR2 capacity raise + auto-reaper + PROCESS object + spawn/wait/kill (+ hosted tests);
   PR3 supervisor image + readiness + restart/budget + survivor demo on QEMU; PR4 broker/discovery/reconnect;
   PR5 DEVICE object + IOPB + IRQ notify + UART driver + timeout/detach demo. Each PR qualified on QEMU.

## OPEN QUESTIONS (answer each; pick and justify)
Q1 Multiplexing for a single-threaded supervisor. Options: (a) busy poll: try_recv(timeout 0) over all handles +
   YIELD (wasteful, no deadline primitive); (b) add a SLEEP(ticks) op + poll; (c) a WAITSET/port object: bind up
   to K endpoint ends + process handles; wait_any(set, timeout) returns which became ready (new sched/wait code —
   owner-gated, re-checks ADR 0009 D7 "exactly one terminal result"); (d) badged endpoints (seL4 style): one
   supervisor inbound queue; each child's SEND handle carries a kernel-stamped badge the receiver sees; kernel
   posts death notices into that queue with a reserved per-process slot so a notice can never be dropped.
   Which is smallest-correct under the frozen constraints? How are death notices guaranteed delivery with depth-4
   queues? Is a badge an "incarnation counter" that reopens ADR 0009 D-OWNER-1?
Q2 Is D5 readiness authentication actually sound given non-exclusive receive, unrestricted attenuating DUP, and
   that the supervisor itself holds the other end? Any confused-deputy or forwarding hole? Should READY also carry
   the instance id, or is that pointless?
Q3 Kill semantics: async victim mid-syscall, blocked in recv (park/resume continuation), or running in ring 3.
   What exactly happens, and what must be re-gated in ADR 0009 D7/A6-CLARIFY-1/2? Is CANCELLED (15) made
   reachable, and for whom?
Q4 Service restart signalling to clients: reuse PEER_CLOSED + re-request via broker, or append RESTARTED?
Q5 Capacity numbers 8/16/32 and memory: 8*73 pages = 2.3 MiB; is the kernel page pool big enough at the 16 MiB CI
   profile? (The 16 MiB profile is a CI gate.) Should caps be smaller (e.g. 4 procs) to keep 16 MiB green?
Q6 Device choice: COM2 16550 via TSS IOPB vs alternatives (PS/2? RTC/CMOS ports 0x70/0x71? a QEMU `isa-debugcon`
   / `pci-testdev` / virtio with intel-iommu on q35?). Which gives a meaningful timeout/detach demo on QEMU `pc`
   without DMA, and is the IOPB containment claim honest (port I/O to a 16550 cannot DMA; but what about ports
   shared with other devices, and IOPL)? IRQ3 delivery path to ring 3 bounded how?
Q7 Anything in D2..D10 that silently contradicts a frozen ADR or that I missed (e.g. teardown order with the new
   PROCESS object holding a death record after vm destroy; IOPB per-process TSS update cost on every switch;
   handle-table reinit making old handle values valid again in the new instance's table).
Q8 D-SEQ: is the PR slicing right? Smallest first PR after the ADR?

Answer as a principal kernel engineer: decisive, concrete, cite the constraint you rely on, flag owner-gated items.

## Measured fact (lead, QEMU 8.2.2, main dc41da5)

Kernel page pool at boot: 16 MiB profile `free pages=0xc20` (3104); 128 MiB `0x7c20`. 8 processes × 73-page quota = 584 pages.

## Round 1 — Claude (kernel-architect)

I can't verify the `-m 16` memory pin or the 16 MiB page-pool size at dc41da5, so the Q5 numbers below are a projection, not a measurement. The other code facts I cite were checked in the repo.

**My main corrections to the lead:**
1. **Q1: no waitset and no new kernel wait code in M4.** Death becomes polled object state, not a queued message.
2. **Q3: kill can be synchronous.** On one CPU with IF=0, the victim is never running when the supervisor calls kill.
3. **Q4 and D4: no `RESTARTED` status and no exposed `instance` counter.** Both conflict with ADR 0009 D-OWNER-1.
4. **D10 and Q6: the TSS has no I/O bitmap today.** It is a fixed 104-byte struct (`kernel/arch/x86_64/trap.c:16-22`), so D10 is new architecture work, not wiring.

## Q1 Multiplexing: poll the state, keep the scheduler unchanged
Use none of options (a)–(d). Build it from what exists:
- **Sleep already exists.** The supervisor `ENDPOINT_CREATE`s a private channel, keeps both ends, and calls `TRY_RECV(own_recv, timeout=k)`. That returns `TIMED_OUT` after k ticks, through the already-tested D7/A6 path.
- **Add one read-only op, `CLOCK_TICKS`.** It is an appended op with no scheduler change, and it gives the supervisor real deadlines.
- **The loop:** poll every readiness/service channel and every `PROC_STATUS` at timeout 0, then sleep 1 tick. Worst-case latency is about 10 ms at 100 Hz, which is acceptable for M4.
- **Death notices can't be dropped because they are not messages.** Death is a field on the PROCESS object: once dead, it stays dead until the last handle closes. The depth-4 queue question goes away.
- **`proc_wait` with timeout > 0 would add a new kind of blocked thread** to `sched.c`. That is ADR 0009 D7 surface (line 175, "single terminal result") and owner-gated, so M4 ships `PROC_STATUS` as non-blocking.

Against (d), badges: a badge is a per-handle label stamped at mint time. That is a handle-format change against ADR 0008 (16 index bits + 16 generation bits, `handle.h:32-35`; "no epoch in handle"). Strictly it is not an incarnation counter, but it changes frozen handle encoding. Defer waitsets and badges to a separate ADR, needed only once polling latency or CPU cost is shown to be a problem.

## Q2 Readiness authentication: sound, with three conditions
1. **The supervisor must close its own copy of the child's end after granting it.** `ENDPOINT_CREATE` puts both ends in the caller's table. If the supervisor keeps a duplicate of end B, the child's death never makes end A report `PEER_CLOSED`, and the "observe death through the channel" path silently breaks.
2. **End A must never leave the supervisor.** Receive is not exclusive, so anyone holding a RECV handle on A could steal READY. This is a supervisor rule, so the broker code must refuse to hand out readiness ends, and a test should cover it.
3. **The child can DUP B, but only to SEND-or-less inside its own table.** Without TRANSFER it can't forward B, and DUP can't add rights.

Caveats:
- **Authentic is not the same as truthful.** READY is self-reported; the claim should say "authenticated readiness", not "verified health".
- **Putting the instance id in READY adds nothing.** The fresh channel already binds the generation, and the child doesn't know its instance anyway. Drop it.
- **Check one thing before freezing D5:** whether TRANSFER is required on the carrying handle or on the carried capability. D5 is only sound if the carried capability needs it.

## Q3 Kill: synchronous `mark_dying`
On a single CPU with an IF=0 kernel, while the supervisor is inside the kill syscall the victim is in one of three states:
- READY at a ring-3 boundary;
- READY and WOKEN with a reserved queue head that hasn't re-entered yet (ADR 0009 A6-CLARIFY-2, line 1132);
- BLOCKED_IPC.

It is never mid-syscall. So `proc_kill` calls `mark_dying` immediately, through the single release owner (`endpoint_kern.c:265`). That is exactly the path the test-only forced-kill hook already proves (ADR 0009 ~line 1163).

The lead's "effective at next kernel entry/tick" is wrong: it lets the victim keep running after a successful kill, for no benefit.

Owner gates:
- Promoting that hook into the ABI breaks the "never in the production ABI" wording (line 1163). It also needs ADR 0008 D7 death-path sign-off.
- A6-CLARIFY-1 must be explicitly re-confirmed: synchronous kill relies on its "single CPU, one thread per process" premise.

`CANCELLED` stays unreachable from the ABI: the only thread that would see it is the victim, and the victim is dead.

## Q4 Restart signalling
Use `PEER_CLOSED` plus a re-request through the broker. For the kernel to say `RESTARTED`, it would have to link service identity across generations. That is the restart-in-place alias that reopens D-OWNER-1 (ADR 0009:307-325).

## Q5 Capacity
Size to the demo, not to round numbers. Needed at once: supervisor, service, client, spoofer, UART driver, plus one launch slot free (a restart needs the old slot reaped first). So:

| Constant | Today | Proposed |
|---|---|---|
| `JE_VM_MAX_PROCESSES` | 2 | 6 |
| `JE_THREAD_MAX` | 8 | today's kernel threads + 6 |
| `JE_EP_POOL_CAP` | 4 | 16 |

- **Why 16 channels:** per child one readiness + one control, one per client/service pair, plus the private sleep channels.
- **Memory:** 6 × 73 pages = 438 pages, about 1.7 MiB.
- **Gate:** the change is green only if the 16 MiB CI profile (`docs/HANDOFF.md:934`) actually passes. A measured result overrides this estimate.

## Q6 Device: COM2 16550, contained by one byte of the I/O bitmap
- **Ports:** 0x2F8–0x2FF sit exactly in bitmap byte 0x5F (0x2F8/8). Per-switch cost is writing one byte, not reloading a bitmap.
- **Bitmap size:** extend the TSS to 0x60 bytes plus the 0xFF terminator, update the GDT TSS descriptor limit, and set `iomap = 104`.
- **IOPL must stay 0** in every user RFLAGS image, including the initial frame. Add a test asserting it.
- **No DMA:** QEMU `pc` ISA COM2 has no DMA path, and nothing else aliases those ports on `pc`.
- **IRQ3 storm control:** the handler masks IRQ3 at the PIC, sets a pending bit on the DEVICE object, and sends EOI. The driver's `DEV_IRQ_ACK(h)` returns the pending bit and unmasks. A misbehaving driver can at worst stall its own device. The handler never enqueues a message, so no queue slot needs reserving.

Rejected alternatives:
- **RTC 0x70/0x71:** bit 7 of port 0x70 disables NMI.
- **PS/2 0x64:** writing 0xFE resets the CPU.
- **isa-debugcon:** no IRQ, so no meaningful timeout demo.
- **virtio + intel-iommu:** that is DMA, out of scope.

Write these rejections into the ADR.

## Q7 Contradictions and gaps
- **Reaper starvation:** if the supervisor busy-polls, does the slot-0 reaper ever run? If not, dead slots never free and every restart fails with "no slot". The tick-sleep in Q1 fixes this; the ADR must require that the reaper is guaranteed to run.
- **PROCESS record lifetime:** it needs its own bounded pool and must point to a record index, not the vm slot. Order is fixed: release walk → write death record → slot FREE. Spawn fails cleanly if the record pool is full.
- **Supervisor death and orphans are unspecified.** Proposal: if the supervisor (the root) dies, the kernel halts with an explicit diagnostic, and all children are killed. No reparenting.
- **Spawn grants should be copy-with-attenuation into the child, not a move.** Rollback then only touches the discarded child; the supervisor table never changes. Rollback uses the existing `je_vm_destroy` path.
- **Handle table reinit is not a hole.** Tables are per-process and the old one is destroyed. Grant values 1..n become a documented v0 convention.
- **Image size is unverified.** The supervisor won't fit in ≤1024 B of asm. A freestanding ring-3 C build path, linked at 0x40000000, is new surface and needs its own PR. Verify the descriptor v1 segment limit.
- **D4 `instance`:** keep it kernel-internal only; it has no ABI consumer (D-OWNER-1).
- **D11:** don't call the UART driver "legacy trusted". It is an untrusted ring-3 process with bounded port authority, contained by bitmap byte 0x5F and PIC masking.

## Q8 / D-SEQ
PR2 is too large. Proposed order:

| PR | Content |
|---|---|
| 1 | ADR 0010 |
| 2a | Capacity raise only, plus re-baselined M2 test and 16 MiB evidence (owner-gated) |
| 2b | `CLOCK_TICKS`, PROCESS object, death record, `PROC_STATUS`, reaper guarantee, using kernel-seeded processes (no spawn yet) |
| 2c | LAUNCHER, spawn, grants, rollback tests |
| 2d | `proc_kill` (owner-gated) |
| 2e | Ring-3 C image build path |
| 3 | Supervisor, readiness, budget |
| 4 | Broker and reconnect |
| 5a | TSS I/O bitmap and DEVICE object |
| 5b | UART driver, IRQ ack, timeout/detach demo |

The smallest PR after the ADR is 2a.

**Owner-gated items:** the capacity raise (tested M1/M2 code), PROCESS reaping in the death path (ADR 0008 D7), synchronous kill (ADR 0009 A6-CLARIFY-1/2), the TSS change, and any waitset or badge.

## Round 1 — Codex (adversarial reviewer)

Claude is right on the major direction: no waitset/badges in M4, no kernel `RESTARTED` status, fresh readiness channels, and COM2 as the most plausible no-DMA demo. But several claims are wrong or incomplete.
1. Q1 — polling, but no `CLOCK_TICKS`
Use a bounded supervisor poll loop:
- `TRY_RECV(..., timeout=0)` over readiness/request channels.
- Nonblocking PROCESS-state polls.
- One private endpoint on which the supervisor performs `TRY_RECV(timeout=1)` as a one-tick sleep.
`je_thread_sleep()` exists only as a kernel function (`kernel/core/sched.c:384-395`); there is no user `SLEEP` ABI operation (`include/jeios/abi.h:86-149`). Conversely, user `TRY_RECV` already accepts relative tick timeouts (`include/jeios/abi.h:130-145`, `kernel/core/syscall.c:531-562`). Therefore `CLOCK_TICKS` adds ABI surface without solving multiplexing.
Claude’s “none of (a)-(d)” is reasonable, but the private timed-receive channel must be counted against endpoint and handle capacity. Death notices are not queue messages: PROCESS records are polled and remain terminal until their last handle closes.
The reaper must run independently from the supervisor in slot 0. Current reaping is explicitly caller-driven and requires slot 0 with IF=0 (`kernel/core/sched.c:455-485`); M4 must turn that into an idle-loop invariant.
2. Q2 — readiness is sound only after closing the source alias
I agree with Claude’s three conditions, but condition 1 needs sharper wording.
Endpoint duplication is fresh-slot copy/mint with a retained reference (`include/jeios/endpoint.h:237-250`), and current transfer behavior deliberately keeps the sender’s source handle until after mint (`kernel/core/main.c:843-865`). Thus after granting the child end B, the supervisor must close its own B handle. Otherwise the supervisor can inject a fake READY and the child’s death will not close the supervisor’s A end.
Required invariant:
- supervisor retains only A, with RECV;
- child receives B, with SEND only;
- all B aliases in the supervisor table are closed after successful child mint;
- B has no TRANSFER, so the child cannot forward it;
- no readiness handle is ever brokered to clients.
READY authenticates “this generation sent this opcode”; it does not prove health. The opcode and payload length must be exact, and malformed first messages should fail readiness rather than be ignored.
An instance ID in READY is unnecessary for authentication. Keep a kernel launch ID internal unless a concrete consumer exists. ADR 0009 D-OWNER-1 forbids shipping an endpoint incarnation counter, not a separate supervisor-level service generation (`docs/adr/0009-m3c-capability-ipc.md:307-325`).
3. Q3 — Claude’s “never mid-syscall” argument is false
The current scheduler has `BLOCKED_IPC`, saved recv arguments, an owned endpoint reference, a saved continuation RIP, and `wait_pending` (`kernel/core/sched.c:16-50`, `273-309`). A blocked receiver is logically mid-syscall even though it is not executing.
Kill semantics should be:
- Other target RUNNING state: impossible on single CPU; reject self-kill or define it separately.
- READY/ring-3 boundary: mark DYING immediately; never redispatch.
- `BLOCKED_IPC/WAITING`: tombstone the waiter, release its endpoint reference, then destroy authority.
- READY with WOKEN reservation: release the reserved queue head before teardown.
- Reap later from slot 0; PROCESS status becomes terminal before VM-slot reclamation.
`mark_dying()` already releases the waiter/reservation before setting DYING (`kernel/core/sched.c:139-153`). The owner gate is still real: exposing kill changes the death path and must be separately signed off under ADR 0008 D7 / ADR 0009 A6-CLARIFY-2.
`CANCELLED` is not merely “unreachable because the victim is dead.” It is internally reachable and tested through the cancellation helper (`docs/adr/0009-m3c-capability-ipc.md:883-892`, `kernel/core/endpoint_kern.c:324-345`), and the continuation maps it to ABI status 15 (`kernel/core/syscall.c:226-246`). Process kill should not deliver CANCELLED to the killed process; it should produce a terminal kill death reason. Nonlethal future cancellation uses CANCELLED.
4. Q4 — kernel PEER_CLOSED, broker-level restart status
Use fresh client/service-generation channels. On death, the existing client endpoint becomes PEER_CLOSED; the client re-requests through the broker.
However, Claude’s “PEER_CLOSED + re-request” alone violates the INIT design’s explicit requirement that clients see a service-restarted result (`docs/INIT.md:71-89`). Do not add a kernel `RESTARTED` status. Return a broker-protocol `SERVICE_RESTARTED`/generation-change result on the re-request. The kernel remains unaware of service identity.
5. Q5 — six is not justified by Claude’s own accounting
`6 * 73 = 438` pages is correct for VM quotas (`include/jeios/vm.h:44-56`), but it is only a projection. The 16 MiB profile is an existing CI gate, not evidence for new M4 capacity (`docs/HANDOFF.md:928-936`).
Claude’s proposed six processes conflicts with the full acceptance actors:
- supervisor
- service
- survivor
- client
- readiness spoofer
- UART driver
- possibly the old DYING service before reaping
The current builder cannot reuse a DYING slot until reaping completes (`kernel/core/vm.c:273-277`, `kernel/core/sched.c:474-482`). Either restart must synchronously wait for reaping, or a spare slot is required. Eight processes is defensible for the full demo; six is only sufficient if reaping is guaranteed before relaunch and the actors are not concurrent.
More importantly, both drafts miss handle capacity. `JE_VM_HANDLE_CAP` remains 16 (`include/jeios/vm.h:51-55`). A supervisor with eight PROCESS handles, one LAUNCHER, readiness handles, broker channels, and client channels exceeds that immediately. Capacity must be derived jointly for:
- VM slots
- scheduler slots
- endpoint records
- per-process handle slots/quota
- PROCESS/DEVICE record pools
The proposed 16 scheduler slots is mathematically consistent with one idle slot, seven M2 workers, and eight user threads, but the M2 worker test must remain explicitly seven workers (`kernel/core/main.c:256-273`).
6. Q6 — COM2 is viable, but Claude’s IOPB details are incomplete
COM2 is the best candidate among the listed devices for `pc`: PIO-only, an observable IRQ path, and no intended DMA. But the repository has no UART/device implementation or qualification yet (`docs/HANDOFF.md:984-998`), so this is a design choice, not an established fact.
Claude’s “extend the TSS to 0x60 bytes” is imprecise. The current TSS is 104 bytes and deliberately has no bitmap (`kernel/arch/x86_64/trap.c:16-22`, `43-45`). To cover ports through `0x2ff`, the bitmap needs 96 bytes plus the required terminator byte; the GDT TSS descriptor limit must become `sizeof(TSS-with-bitmap)-1`, not remain 103.
The containment claim must be:
- IOPL remains zero; existing frame sanitization already forces it (`kernel/core/uframe.c:16-20`, `29-41`).
- Default-deny all port bits.
- Clear only bits for `0x2f8–0x2ff`.
- Update the bitmap before dispatching a driver and clear it before reassignment.
- Qualify only QEMU `pc`, not arbitrary physical PCs with undocumented port aliases.
- Keep IRQ3 binding, mask/unmask, pending state, and detach serialized.
Claude’s IRQ storm claim is not established. A malicious driver can acknowledge without clearing the UART interrupt source and cause repeated IRQ3 delivery. “Coalesced pending bit” alone bounds notification storage, not interrupt rate. Require a device-specific acknowledge/arming contract or rate-limited masking.
7. Q7 — major missed contradictions
The lead draft has these additional holes:
- “Fixed handle values 1..n” conflicts with the current allocator, which allocates the lowest free slot starting at slot 0 (`kernel/core/handle.c:77-99`; `tests/test_handle.c:50-52`). A handle is also index+generation, not a small ordinal. Use an explicit destination-mint/launch-info contract, not numeric assumptions.
- The child has no defined way to receive the grant list. Initial user registers are zeroed (`kernel/core/sched.c:123-132`). Pass a bounded launch-info structure or define a register/immutable startup ABI.
- `handle_query` and `handle_dup` are currently SELF-specific (`kernel/core/syscall.c:415-426`, `445-458`). PROCESS, LAUNCHER, DEVICE, and endpoint grants require a generic typed-object dispatch layer.
- PROCESS records need an independent bounded pool and lifetime. They must not point at a reused VM slot. The record becomes terminal before VM destruction and is freed only after all PROCESS handles close.
- `JE_VM_HANDLE_CAP` and quota must be increased or service count must be sharply bounded.
- Supervisor/service state needs `quiescing`/stop-drain semantics. INIT requires stopping to reject work, drain/cancel, revoke grants, then reap (`docs/INIT.md:86-91`); DESIGN also distinguishes quiescing and quarantined states (`docs/DESIGN.md:201-209`).
- Supervisor failure handling is unspecified. INIT explicitly says supervisor corruption cannot be repaired by pretending success (`docs/INIT.md:93-98`). M4 needs an explicit failure/degraded profile, not an invented “halt and kill all children” policy.
- DEVICE revocation must mask IRQ, revoke IOPB authority, close notification paths, and only then allow reassignment. “proc_kill + device reassign” is underspecified.
8. Q8 — revised sequence
Claude’s split is directionally good, but I would use:
1. ADR 0010 plus measured capacity/handle-budget formulas.
2. Owner-gated capacity change: VM, scheduler, endpoint pool, handle-table quota; rebaseline `make check`, 16 MiB, and QEMU smoke.
3. PROCESS record, automatic slot-0 reaper, nonblocking status, and death-record preservation.
4. LAUNCHER/spawn with transactional VM/thread/grant rollback and explicit child launch-info.
5. Owner-gated kill semantics and tests for READY, BLOCKED_IPC, WOKEN-reservation, and reaping.
6. Ring-3 C image path.
7. Supervisor polling, readiness authentication, restart budgets, and survivor QEMU demo.
8. Broker discovery/reconnect with protocol-level restart result.
9. DEVICE/IOPB/IRQ mechanism.
10. UART driver, timeout, detach, and no-DMA-profile QEMU demo.
The smallest code PR after the ADR is the capacity/handle-budget PR only if the measured 16 MiB and static-size audit pass. Otherwise begin with a kernel-only PROCESS lifecycle prototype at existing capacity.
Bottom line: accept Claude’s rejection of waitsets, badges, exposed kernel restart status, and immediate COM2 selection. Reject the “victim is never mid-syscall” reasoning, the `CLOCK_TICKS` addition, six-process confidence, “PEER_CLOSED alone” for INIT semantics, and the incomplete IOPB/IRQ containment claim. The largest omissions are handle-table capacity, child grant delivery, generic typed-object dispatch, and broker-level restart signaling.
Claude is right on the major direction: no waitset/badges in M4, no kernel `RESTARTED` status, fresh readiness channels, and COM2 as the most plausible no-DMA demo. But several claims are wrong or incomplete.
1. Q1 — polling, but no `CLOCK_TICKS`
Use a bounded supervisor poll loop:
- `TRY_RECV(..., timeout=0)` over readiness/request channels.
- Nonblocking PROCESS-state polls.
- One private endpoint on which the supervisor performs `TRY_RECV(timeout=1)` as a one-tick sleep.
`je_thread_sleep()` exists only as a kernel function (`kernel/core/sched.c:384-395`); there is no user `SLEEP` ABI operation (`include/jeios/abi.h:86-149`). Conversely, user `TRY_RECV` already accepts relative tick timeouts (`include/jeios/abi.h:130-145`, `kernel/core/syscall.c:531-562`). Therefore `CLOCK_TICKS` adds ABI surface without solving multiplexing.
Claude’s “none of (a)-(d)” is reasonable, but the private timed-receive channel must be counted against endpoint and handle capacity. Death notices are not queue messages: PROCESS records are polled and remain terminal until their last handle closes.
The reaper must run independently from the supervisor in slot 0. Current reaping is explicitly caller-driven and requires slot 0 with IF=0 (`kernel/core/sched.c:455-485`); M4 must turn that into an idle-loop invariant.
2. Q2 — readiness is sound only after closing the source alias
I agree with Claude’s three conditions, but condition 1 needs sharper wording.
Endpoint duplication is fresh-slot copy/mint with a retained reference (`include/jeios/endpoint.h:237-250`), and current transfer behavior deliberately keeps the sender’s source handle until after mint (`kernel/core/main.c:843-865`). Thus after granting the child end B, the supervisor must close its own B handle. Otherwise the supervisor can inject a fake READY and the child’s death will not close the supervisor’s A end.
Required invariant:
- supervisor retains only A, with RECV;
- child receives B, with SEND only;
- all B aliases in the supervisor table are closed after successful child mint;
- B has no TRANSFER, so the child cannot forward it;
- no readiness handle is ever brokered to clients.
READY authenticates “this generation sent this opcode”; it does not prove health. The opcode and payload length must be exact, and malformed first messages should fail readiness rather than be ignored.
An instance ID in READY is unnecessary for authentication. Keep a kernel launch ID internal unless a concrete consumer exists. ADR 0009 D-OWNER-1 forbids shipping an endpoint incarnation counter, not a separate supervisor-level service generation (`docs/adr/0009-m3c-capability-ipc.md:307-325`).
3. Q3 — Claude’s “never mid-syscall” argument is false
The current scheduler has `BLOCKED_IPC`, saved recv arguments, an owned endpoint reference, a saved continuation RIP, and `wait_pending` (`kernel/core/sched.c:16-50`, `273-309`). A blocked receiver is logically mid-syscall even though it is not executing.
Kill semantics should be:
- Other target RUNNING state: impossible on single CPU; reject self-kill or define it separately.
- READY/ring-3 boundary: mark DYING immediately; never redispatch.
- `BLOCKED_IPC/WAITING`: tombstone the waiter, release its endpoint reference, then destroy authority.
- READY with WOKEN reservation: release the reserved queue head before teardown.
- Reap later from slot 0; PROCESS status becomes terminal before VM-slot reclamation.
`mark_dying()` already releases the waiter/reservation before setting DYING (`kernel/core/sched.c:139-153`). The owner gate is still real: exposing kill changes the death path and must be separately signed off under ADR 0008 D7 / ADR 0009 A6-CLARIFY-2.
`CANCELLED` is not merely “unreachable because the victim is dead.” It is internally reachable and tested through the cancellation helper (`docs/adr/0009-m3c-capability-ipc.md:883-892`, `kernel/core/endpoint_kern.c:324-345`), and the continuation maps it to ABI status 15 (`kernel/core/syscall.c:226-246`). Process kill should not deliver CANCELLED to the killed process; it should produce a terminal kill death reason. Nonlethal future cancellation uses CANCELLED.
4. Q4 — kernel PEER_CLOSED, broker-level restart status
Use fresh client/service-generation channels. On death, the existing client endpoint becomes PEER_CLOSED; the client re-requests through the broker.
However, Claude’s “PEER_CLOSED + re-request” alone violates the INIT design’s explicit requirement that clients see a service-restarted result (`docs/INIT.md:71-89`). Do not add a kernel `RESTARTED` status. Return a broker-protocol `SERVICE_RESTARTED`/generation-change result on the re-request. The kernel remains unaware of service identity.
5. Q5 — six is not justified by Claude’s own accounting
`6 * 73 = 438` pages is correct for VM quotas (`include/jeios/vm.h:44-56`), but it is only a projection. The 16 MiB profile is an existing CI gate, not evidence for new M4 capacity (`docs/HANDOFF.md:928-936`).
Claude’s proposed six processes conflicts with the full acceptance actors:
- supervisor
- service
- survivor
- client
- readiness spoofer
- UART driver
- possibly the old DYING service before reaping
The current builder cannot reuse a DYING slot until reaping completes (`kernel/core/vm.c:273-277`, `kernel/core/sched.c:474-482`). Either restart must synchronously wait for reaping, or a spare slot is required. Eight processes is defensible for the full demo; six is only sufficient if reaping is guaranteed before relaunch and the actors are not concurrent.
More importantly, both drafts miss handle capacity. `JE_VM_HANDLE_CAP` remains 16 (`include/jeios/vm.h:51-55`). A supervisor with eight PROCESS handles, one LAUNCHER, readiness handles, broker channels, and client channels exceeds that immediately. Capacity must be derived jointly for:
- VM slots
- scheduler slots
- endpoint records
- per-process handle slots/quota
- PROCESS/DEVICE record pools
The proposed 16 scheduler slots is mathematically consistent with one idle slot, seven M2 workers, and eight user threads, but the M2 worker test must remain explicitly seven workers (`kernel/core/main.c:256-273`).
6. Q6 — COM2 is viable, but Claude’s IOPB details are incomplete
COM2 is the best candidate among the listed devices for `pc`: PIO-only, an observable IRQ path, and no intended DMA. But the repository has no UART/device implementation or qualification yet (`docs/HANDOFF.md:984-998`), so this is a design choice, not an established fact.
Claude’s “extend the TSS to 0x60 bytes” is imprecise. The current TSS is 104 bytes and deliberately has no bitmap (`kernel/arch/x86_64/trap.c:16-22`, `43-45`). To cover ports through `0x2ff`, the bitmap needs 96 bytes plus the required terminator byte; the GDT TSS descriptor limit must become `sizeof(TSS-with-bitmap)-1`, not remain 103.
The containment claim must be:
- IOPL remains zero; existing frame sanitization already forces it (`kernel/core/uframe.c:16-20`, `29-41`).
- Default-deny all port bits.
- Clear only bits for `0x2f8–0x2ff`.
- Update the bitmap before dispatching a driver and clear it before reassignment.
- Qualify only QEMU `pc`, not arbitrary physical PCs with undocumented port aliases.
- Keep IRQ3 binding, mask/unmask, pending state, and detach serialized.
Claude’s IRQ storm claim is not established. A malicious driver can acknowledge without clearing the UART interrupt source and cause repeated IRQ3 delivery. “Coalesced pending bit” alone bounds notification storage, not interrupt rate. Require a device-specific acknowledge/arming contract or rate-limited masking.
7. Q7 — major missed contradictions
The lead draft has these additional holes:
- “Fixed handle values 1..n” conflicts with the current allocator, which allocates the lowest free slot starting at slot 0 (`kernel/core/handle.c:77-99`; `tests/test_handle.c:50-52`). A handle is also index+generation, not a small ordinal. Use an explicit destination-mint/launch-info contract, not numeric assumptions.
- The child has no defined way to receive the grant list. Initial user registers are zeroed (`kernel/core/sched.c:123-132`). Pass a bounded launch-info structure or define a register/immutable startup ABI.
- `handle_query` and `handle_dup` are currently SELF-specific (`kernel/core/syscall.c:415-426`, `445-458`). PROCESS, LAUNCHER, DEVICE, and endpoint grants require a generic typed-object dispatch layer.
- PROCESS records need an independent bounded pool and lifetime. They must not point at a reused VM slot. The record becomes terminal before VM destruction and is freed only after all PROCESS handles close.
- `JE_VM_HANDLE_CAP` and quota must be increased or service count must be sharply bounded.
- Supervisor/service state needs `quiescing`/stop-drain semantics. INIT requires stopping to reject work, drain/cancel, revoke grants, then reap (`docs/INIT.md:86-91`); DESIGN also distinguishes quiescing and quarantined states (`docs/DESIGN.md:201-209`).
- Supervisor failure handling is unspecified. INIT explicitly says supervisor corruption cannot be repaired by pretending success (`docs/INIT.md:93-98`). M4 needs an explicit failure/degraded profile, not an invented “halt and kill all children” policy.
- DEVICE revocation must mask IRQ, revoke IOPB authority, close notification paths, and only then allow reassignment. “proc_kill + device reassign” is underspecified.
8. Q8 — revised sequence
Claude’s split is directionally good, but I would use:
1. ADR 0010 plus measured capacity/handle-budget formulas.
2. Owner-gated capacity change: VM, scheduler, endpoint pool, handle-table quota; rebaseline `make check`, 16 MiB, and QEMU smoke.
3. PROCESS record, automatic slot-0 reaper, nonblocking status, and death-record preservation.
4. LAUNCHER/spawn with transactional VM/thread/grant rollback and explicit child launch-info.
5. Owner-gated kill semantics and tests for READY, BLOCKED_IPC, WOKEN-reservation, and reaping.
6. Ring-3 C image path.
7. Supervisor polling, readiness authentication, restart budgets, and survivor QEMU demo.
8. Broker discovery/reconnect with protocol-level restart result.
9. DEVICE/IOPB/IRQ mechanism.
10. UART driver, timeout, detach, and no-DMA-profile QEMU demo.
The smallest code PR after the ADR is the capacity/handle-budget PR only if the measured 16 MiB and static-size audit pass. Otherwise begin with a kernel-only PROCESS lifecycle prototype at existing capacity.
Bottom line: accept Claude’s rejection of waitsets, badges, exposed kernel restart status, and immediate COM2 selection. Reject the “victim is never mid-syscall” reasoning, the `CLOCK_TICKS` addition, six-process confidence, “PEER_CLOSED alone” for INIT semantics, and the incomplete IOPB/IRQ containment claim. The largest omissions are handle-table capacity, child grant delivery, generic typed-object dispatch, and broker-level restart signaling.

## Round 2 — Claude



I read these at dc41da5: `sched.c`, `handle.c`, `syscall.c`, `trap.c`, `uframe.c`, `endpoint.h`, `endpoint_kern.c`, `INIT.md`, `DESIGN.md` and ADR 0009.

## Point-by-point on the Codex review

1. **Q1, `CLOCK_TICKS` adds ABI without solving multiplexing.** Partly conceded. It doesn't solve multiplexing, and I never said it did. It solves a different requirement, time, which INIT.md:76 requires ("Use monotonic deadlines"). I keep it; see C1.
2. **Q2, close the supervisor's B alias.** Conceded; I take the sharper wording. Verified: carrying a cap requires TRANSFER on the *carried* handle (`endpoint_kern.c:156-162`). So a child holding B without TRANSFER cannot forward B. D5 stands, plus Codex's rule that a malformed first message fails readiness.
3. **Q3, "never mid-syscall".** Conceded on wording, my mechanism stands. Syscalls run with IF=0 and are atomic, so the victim is never *executing* kernel code. A parked receiver, though, is logically mid-syscall (`sched.c:40-49`). Kill stays synchronous; see C2. Also conceded: CANCELLED is reachable internally and is never delivered to a killed thread.
4. **Q4, PEER_CLOSED alone breaks INIT.md:89.** Conceded. The kernel signals PEER_CLOSED; the broker's reply to a re-request is `SERVICE_RESTARTED`. The kernel holds no service identity.
5. **Q5, handle cap omitted; 6 processes too tight.** Conceded. 8 processes is 584 of 3104 free pages at 16 MiB (measured by the lead), and the margin buys a spare slot with no reaper-ordering assumption.
6. **Q6, TSS limit and IRQ rate.** Both conceded. The descriptor limit is hard-coded `103` (`trap.c:43`) and `iomap = sizeof(tss)` (`trap.c:39`), so both must change. My "coalesced pending bit" bounds storage, not rate; see C5.
7. **Q7, all items conceded:**
   - Handle values are not ordinals. The allocator picks the lowest free slot and encodes index+generation (`handle.c:85-95`). Fixed values 1..n are withdrawn.
   - User GPRs start at zero (`sched.c:126`), so launch-info needs its own ABI; see C4.
   - QUERY and DUP are hard-wired to SELF (`syscall.c:420-421`, `453-455`); see C7.
   - Quiescing semantics are missing.
   - "Halt and kill all children" needs grounding; see C6.
8. **Q8, sequence.** Accepted, with the adjustments in C8.

## C1 Clock: a read-only `CLOCK_TICKS` op is the smallest honest fix

The supervisor cannot keep time by counting its own timed receives:
- An early return can't happen, because nobody sends on the private sleep channel.
- But user threads run with IF=1 (`sched.c:129`) and every tick can preempt them (`sched.c:233-238`). With up to 7 other runnable threads, one poll iteration can lose about 7 ticks that the count never sees.

What that undercount does:
- Readiness deadlines fire late. That is a liveness bug only.
- Restart windows are judged over more real time than the policy says, so the rate is enforced wrongly in both directions.
- Either way, "3 restarts per 60 s" becomes a claim we can't back.

The fix is to append op 13 `CLOCK_TICKS`, which returns `je_sched_ticks()` (`sched.c:427`). Properties:
- No authority, no scheduler change, no wait state.
- It leaks no new information: `TRY_RECV` timeouts already expose ticks.
- It is not owner-gated (append-only ABI).

Rejected alternative: returning the current tick in RDX on `TIMED_OUT`. That changes a frozen op's outputs.

## C2 Kill semantics, by victim state

New op `PROC_KILL(h)` needs the KILL right. It calls the existing `mark_dying` (`sched.c:139-153`) with a new appended reason, `DEATH_KILLED=3`.

| Victim state | Result |
|---|---|
| READY at a ring-3 boundary | `mark_dying`; `frame=0`; never dispatched again. OK. |
| BLOCKED_IPC, waiting | `mark_dying` → `waiter_release_by_id` drops the waiter and its owned record ref; `wait_pending=false`, so the continuation never runs and the victim gets no CANCELLED. Death record is written. OK. |
| READY with a WOKEN reservation | The same single release owner (A6-CLARIFY-2) un-reserves the head; the message stays queued for any other RECV holder. Test: the ADR 0009:1216 scenario, triggered by the ABI instead of the hook. |
| Self (h names the caller) | `INVALID`. Use EXIT. This keeps "victim ≠ current", which `drop_user` relies on (`sched.c:443`). |
| Supervisor | Unreachable: it is kernel-seeded and no PROCESS handle for it exists. |
| Already DYING or dead | OK, idempotent. The first terminal reason wins. |
| Kernel threads | Can't be named. |

Reaping is unchanged. The ordering is: death record terminal → `drop_user` → `je_vm_destroy` → slot FREE.

Owner-gated surface:
- (a) ADR 0009:1163 says the forced kill is "never in the production ABI". That must be amended.
- (b) ADR 0008 D7 death-path sign-off.
- (c) Explicit re-confirmation of A6-CLARIFY-1's premise (one CPU, one thread per process, immutable text). Kill relies on it.

## C3 Capacity

Actors: supervisor, service, client/survivor, spoofer, UART driver (5), plus one DYING-overlap spare and two headroom slots.

| Item | Formula | Value |
|---|---|---|
| Processes P | actors 5 + spare 1 + headroom 2 | **8** (584 pages) |
| Threads T | 1 idle + 7 M2 workers + P | **16**; the M2 test stays at exactly 7 workers |
| Endpoint records E | 2(P−1) readiness+control, + B broker pairs (≤6), + 1 sleep, + 2 existing fixtures | 14+6+1+2 = 23 → **24** |
| Handle capacity | supervisor: 3(P−1) + 2 sleep ends + 1 SELF + 4 launch staging + 2 broker transient = 30 | **`JE_VM_HANDLE_CAP` 32**; quota is per table: supervisor 32, children default 16 |
| PROCESS records | P live + P unclosed dead records | **16**; spawn returns `NOMEM` when full |
| DEVICE records | one device | **1** |

PROCESS handles cannot leave the supervisor: send-caps must be live endpoints in the pool (`endpoint.h:338-340`). The static size of `je_ep_channel` times 24 must be measured and recorded in PR2a; I have not measured it.

## C4 Child launch-info: register ABI

At first dispatch the initial frame carries:
- `RDI` = grant count n (≤4);
- `RSI, RDX, RCX, R8` = the minted handles, with unused registers left at 0;
- `R9` = a launch-info version (1).

Why registers:
- No new frame kind and no write into the image, so A6-CLARIFY-1's immutable text is untouched.
- Fixed size, so it is bounded.
- The child must save the registers before clobbering them; that is a documented v0 convention.

Grant rules:
- Each grant is a copy with attenuation into the child's table.
- The supervisor's source handle must hold TRANSFER, the same rule as send-caps, and the requested rights must be a subset of the source's.
- The supervisor's table is never mutated by spawn. Closing B is a separate, explicit step.

Rollback: if any mint, the VM build or the thread slot fails, the child has never run. `drop_user` + `je_vm_destroy` then runs the existing teardown release walk over whatever was minted, and the PROCESS record is freed. Spawn returns the first failing status and no handle. Tests inject a failure at each mint index.

## C5 IRQ rate contract

On IRQ3 the handler does three things:
1. Masks IRQ3 at PIC1 (OCW1).
2. Sets `pending` on the DEVICE record.
3. Sends EOI.

It never touches the UART or a queue.

`DEV_IRQ_ACK(h)` returns and clears `pending`, but it does **not** unmask immediately. It sets `rearm_tick = max(now+1, last_unmask+1)`, and the timer sweep unmasks when that tick arrives.

Bound: at most one IRQ3 ISR per tick (≤100/s) whatever the driver does. A driver that never clears the source only gets one ISR per tick and starves its own device. The rate limit is device-agnostic, so the kernel never reads IIR/LSR (that would have side effects on device state the driver owns).

The bitmap default-denies the PIC ports 0x20/0x21/0xA0/0xA1, so the driver can't unmask anything itself. Detach order: mask → clear pending → set bitmap byte 0x5F to 0xFF → mark the record unowned → only then allow reassignment.

On the TSS:
- Bitmap bytes 0..0x5F follow at `iomap = 104`, then a 0xFF terminator. The limit becomes `104+0x60+1-1`.
- Ports above 0x2FF fall outside the limit and fault (#GP).
- On every switch, byte 0x5F is written 0x00 when the driver is dispatched and 0xFF otherwise.
- IOPL is already forced to 0 (`uframe.c:41`); add an assertion test.
- The claim is limited to QEMU `pc`.

## C6 Supervisor failure (M4 profile)

INIT.md:93-95 rules out repair and calls for a watchdog "once platform support exists", which M4 lacks. So the M4 behaviour is an explicit refusal (AGENTS rule 5):
1. On the supervisor's death the kernel prints `M4: supervisor lost (reason); no recovery path in this profile`.
2. It kills every launched process through C2.
3. It reaps them and checks that the page and endpoint pools are back at baseline.
4. It halts. No restart and no reboot loop.

This is testable with a QEMU variant that faults the supervisor and asserts the marker plus the zero-leak counts. Child failures go through the supervisor's own budgets.

State set:

| INIT state | Maps to |
|---|---|
| `stopping` | DESIGN's `quiescing`: reject new work, drain until the deadline, revoke grants/channels, kill, await reap |
| `failed` | Terminal until operator; this also covers DESIGN's `quarantined` |

## C7 Typed-object dispatch

Use one static table indexed by type:
`ops[type] = {known_rights, inspect_info(obj)→rdx, retain(obj), release(obj)}`.

- **QUERY** resolves with `JE_HANDLE_TYPE_ANY` and returns the type.
- **RIGHTS** requires INSPECT and masks the result with `ops[type].known_rights`.
- **DUP** generalises `dup_self`: subset check, then `retain`, then allocate, with release on allocation failure. Endpoint DUP already has this shape (`endpoint.h:237-250`).
- **CLOSE** calls `release` after the slot is freed.
- **LAUNCHER and DEVICE are not DUP-able in M4.** DUP returns `DENIED`, so the single-holder invariants hold by construction.

This changes the behaviour of tested M3b ops on non-SELF handles, so it is owner-gated.

## C8 Final D-SEQ

1. **ADR 0010:** this contract, the formulas, and the device rejections.
2. **PR2a (owner-gated):** capacity 8/16/24/32/16/1, plus the 16 MiB, smoke and `make check` evidence and the static-size audit.
3. **PR2b (owner-gated):** typed dispatch (C7) and `CLOCK_TICKS`.
4. **PR2c (owner-gated, ADR 0008 D7):** PROCESS record, automatic slot-0 reaper, `PROC_STATUS`, using kernel-seeded processes.
5. **PR2d:** LAUNCHER, spawn, register launch-info, rollback tests.
6. **PR2e (owner-gated):** `PROC_KILL`, tests per C2 state.
7. **PR2f:** ring-3 C image path.
8. **PR3:** supervisor, readiness, budgets, survivor demo, C6 test.
9. **PR4:** broker, `SERVICE_RESTARTED`, reconnect.
10. **PR5a (owner-gated):** TSS bitmap, DEVICE object, IRQ rate contract.
11. **PR5b:** UART driver, timeout and detach demo.

**Owner decisions needed:**
1. The capacity raise.
2. Typed dispatch changing M3b op behaviour.
3. PROCESS reaping in the death path (ADR 0008 D7).
4. Amending ADR 0009:1163 to put kill in the production ABI.
5. Re-confirming A6-CLARIFY-1.
6. The TSS/GDT change.
7. The supervisor-loss halt as M4's defined degraded profile.

Everything above is AI-assisted design and has not been tested.

## Round 2 — Codex

Verified at `dc41da5aaeb87c3a982acd9907ef552beaf44c21`.

## C1 — AGREE, with one test/documentation fix

`CLOCK_TICKS` is safe and justified. `je_sched_ticks()` already exposes the monotonic PIT counter internally (`kernel/core/sched.c:427`), and timer ticks increment only in the timer path (`kernel/core/sched.c:207-210`). The existing receive timeout is relative and bounded (`include/jeios/abi.h:130-170`), but a polling loop cannot infer elapsed ticks while preempted by other runnable threads.

Append op 13, return the current tick count, and document that it is a monotonic tick source, not wall time. Update the existing “op 13 unsupported” hosted assertion (`tests/test_syscall.c:127-128`) and add wrap/early-boot semantics. It requires no new wait state or authority.

## C2 — AGREE, but tighten the kill contract

The revised synchronous kill model is correct under the single-CPU/IF=0 premise. A victim cannot execute concurrently with the killer; a blocked receiver is nevertheless logically mid-syscall because it owns a waiter continuation (`kernel/core/sched.c:37-49`, `273-309`).

`mark_dying()` already releases the waiter/reservation before setting `DYING` (`kernel/core/sched.c:139-152`). This correctly handles:

- `BLOCKED_IPC`;
- a `WOKEN` waiter holding a reserved queue head;
- a victim with the only handle to an endpoint whose capability is queued elsewhere.

The latter is covered by endpoint close’s detach-and-release ordering (`kernel/core/endpoint.c:202-230`) and the self-cycle test (`tests/test_endpoint.c:612-630`). Queued capabilities retain independent inflight references, but once the sole authority handle closes, the target end becomes `CLOSED`, so later minting returns a dead capability, not revived authority.

A syscall cannot be killed mid-transfer: syscall entry is an interrupt gate with IF cleared, and capability send is one bounded critical section (`kernel/core/syscall.c:479-520`; `kernel/core/endpoint.c:761-810`). The new kill implementation must reject self-kill, make repeated kill idempotent, and never call `mark_dying()` on an already-reaped slot. Process-record state, not the VM slot, must determine idempotence.

`CANCELLED` remains internal-only for process kill; the victim must not resume to observe it. Owner gates remain ADR 0008 D7, ADR 0009 A6-CLARIFY-1/2, and amendment of the current “no production kill ABI” decision.

## C3 — OBJECT: the capacity arithmetic is incomplete

The current handle quota is `JE_VM_HANDLE_CAP - 1`, currently 15 (`kernel/core/vm.c:362-370`), not an independently configurable “children default 16” policy. Raising capacity to 32 would produce quota 31 unless a new quota field and initialization contract are added.

More importantly, ADR 0009 D5 requires a separate per-process endpoint-creation quota whose charge survives creator death and is released only when the channel record is destroyed (`docs/adr/0009-m3c-capability-ipc.md:143-151`). The implementation has no creator/charge field: endpoint creation only allocates a pool record and mints two handles (`kernel/core/endpoint.c:426-447`). C3’s `24` endpoint records therefore does not discharge D5.

The proposed PROCESS-record count is also wrong. Reaped VM slots can leave terminal PROCESS objects alive while handles remain open; record capacity is bounded by outstanding PROCESS handles and launch history, not by current VM slots. A supervisor with a 31-handle quota can retain more than `P + P` terminal records.

Fix C3 by publishing formulas for:

- VM slots and scheduler slots;
- per-process live-handle quota;
- endpoint creation charges and global pool capacity;
- endpoint `handle_refs` and queued/waiter `inflight_refs`;
- PROCESS records, including terminal records after VM reaping;
- launch rollback staging.

Keep 8/16/32 provisional until those formulas and static `.bss`/page-pool measurements pass the 16 MiB profile.

## C4 — AGREE, with a bounded startup ABI

The initial user frame is explicitly zeroed before fixed fields are written (`kernel/core/sched.c:123-132`). `je_uframe_check()` validates only selectors, canonical RIP/RSP, and address bounds; it permits arbitrary GPR values and sanitizes RFLAGS, forcing IF=1 and IOPL=0 (`kernel/core/uframe.c:29-41`). Therefore register launch-info is architecturally allowed and does not leak kernel register state: the values are deliberately kernel-minted handles.

The fix is contractual precision:

- define the exact startup register layout and version;
- require unused grant registers to be zero;
- state that handles are current-generation values, not fixed slot ordinals;
- require the image to copy startup registers before its first syscall;
- test malformed/extra startup values and handle-table rollback.

## C5 — OBJECT: the IRQ contract is not yet sound

The proposed rate limiter is plausible, but the repository currently has no IRQ3 device path at all. PIC setup unmasks only IRQ0 (`kernel/arch/x86_64/platform.c:68-76`), and trap dispatch recognizes only vector 32 as a timer (`kernel/arch/x86_64/trap.c:122-154`). IRQ3 would currently fall into the fatal path.

Required corrections:

- unmask master IRQ2 cascade and slave IRQ3 centrally; the driver must not own PIC ports;
- send EOI to both slave and master PICs for IRQ3;
- implement spurious IRQ7 handling by checking the PIC ISR before treating vector 39 as a real device interrupt;
- make timer rearm state binding-generation-aware;
- detach/kill must mask IRQ3, cancel pending rearm state, clear pending notification, revoke IOPB bits, and only then mark the device unowned;
- timer unmask must verify the same device binding/generation is still active, preventing a stale timer sweep from rearming a reassigned device.

The “one ISR per tick” bound is valid only if the kernel is the sole unmasking authority and no stale rearm can survive detach. COM2’s IOPB byte is correctly `0x2f8 / 8 = 0x5f`, but qualification must remain explicitly QEMU `pc`; it proves bounded port authority, not universal hardware alias exclusion.

## C6 — AGREE, but label it precisely

Fail-stop is an honest M4 degraded/refusal profile if it is explicitly named as such: print a diagnostic, revoke/kill children, verify cleanup, and halt. It must not be described as INIT rescue or supervisor recovery.

`INIT.md` still requires a rescue/watchdog path once platform support exists (`docs/INIT.md:93-96`). Therefore ADR 0010 should state: “M4 has no supervisor-recovery path; critical supervisor loss is a deliberate system refusal.” If cleanup or reaping cannot complete, halt immediately and report the invariant failure.

## C7 — OBJECT: direction good, dispatch contract incomplete

The current behavior is explicitly SELF-specific:

- query resolves only SELF (`kernel/core/syscall.c:415-427`);
- rights resolves only SELF and requires INSPECT (`kernel/core/syscall.c:429-443`);
- dup is SELF-only (`kernel/core/syscall.c:445-461`);
- close already has a typed SELF/ENDPOINT branch (`kernel/core/syscall.c:381-412`).

Generic dispatch would intentionally change endpoint query/rights/dup behavior and add PROCESS/LAUNCHER/DEVICE semantics. Existing M3b QEMU assertions for SELF rights, attenuation, DENIED, unknown-right INVALID, quota-at-15, and stale reuse remain valid (`kernel/core/main.c:617-668`), but new compatibility tests must assert every operation on SELF and ENDPOINT.

The dispatch table needs more than `known_rights`, inspect, retain, and release. Add explicit duplicate policy, close policy, inspect-result schema, and object-record lifetime. Define whether non-duplicable LAUNCHER/DEVICE returns DENIED or WRONGTYPE.

## C8 — OBJECT: sequence is directionally right, but reorder two boundaries

The broad progression is correct, but:

1. Capacity PR must be measurement/accounting first, not merely constant changes.
2. Generic typed dispatch should land with the first new object type, preferably PROCESS, rather than as an ABI-changing intermediate with no consumer.
3. PROCESS records, automatic reaping, and terminal death records should be one owner-gated lifecycle PR.
4. LAUNCHER/spawn follows that PR and must include explicit startup-register delivery and transactional grant rollback.
5. Kill follows spawn and requires its own death-path qualification.
6. Supervisor, broker, device, and UART work then remain appropriately separated.

## Consolidated position

Agreed: no waitset/badges in M4; fresh readiness channels with the supervisor’s child-end alias closed; no kernel `RESTARTED`; synchronous single-CPU kill; broker-level restart signaling; COM2 as the best candidate; no-DMA/PIO-only profile; register startup grants are viable.

Remaining disagreements:

- `CLOCK_TICKS`: recommend adding op 13.
- Exact capacities: do not freeze until handle, creation-charge, PROCESS-record, static-size, and 16 MiB evidence exists.
- Supervisor loss: recommend explicit M4 fail-stop refusal, not a claimed rescue path.
- C8: combine typed dispatch with PROCESS-object introduction.

Owner-gated items: capacity/tested-path changes; PROCESS death records and automatic reaping; production kill ABI; ADR 0009 D7/A6 re-confirmation; endpoint creation-charge accounting; typed dispatch behavior changes; TSS/GDT/I/O bitmap changes; PIC/IRQ ownership; and the M4 supervisor-loss profile.

## Lead verification after round 2

Codex's C3 claim checked: ADR 0009 D5 (lines 143-154) requires a per-process endpoint creation quota whose charge is released at record destruction. No creator/charge field exists in `kernel/core/endpoint.c` / `include/jeios/endpoint.h` / `endpoint_kern.c` (grep for quota/charge/creator finds only the handle-table quota). Not recorded as a deviation in IMPLEMENTATION.md or M3C_RETRO.md. Pre-existing gap in merged M3c code; escalated to the owner.
