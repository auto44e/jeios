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

---

## ADR 0010 text reviews (2026-10-30, AI-assisted; not human qualification)

After the owner approved D-OWNER-1..8, the lead drafted ADR 0010. Both peers reviewed the text adversarially in parallel, for three rounds, read-only against the repo. Every finding was applied by the lead. Outputs are verbatim.

### Round 1 — Claude

The ADR needs fixes before PR-A starts. I found 4 must-fix issues: one arithmetic contradiction, one PR-ordering contradiction, and two soundness holes. There are also 9 should-fix issues and 5 nits. All checks were read-only against `dc41da5` plus the working-tree ADR diffs.

## P1: must fix

**1. The supervisor's handle budget is larger than its quota (D2).** The formula adds up to 3·7+2+1+1+1+4+2 = **32**, but the supervisor's quota is H−1 = **31**. The current code fixes quota at `JE_VM_HANDLE_CAP - 1u` (`kernel/core/vm.c:369-370`). The transcript's earlier sum was 30 (`m4-contract.md:486`); LAUNCHER and DEVICE admin were added later without rechecking the quota.
- *Fix:* "H = 40 (or another value with headroom ≥ 4); supervisor quota = H − 1 ≥ formula + 4 for inbound caps (see #4). PR-A adds a per-table quota field; `_Static_assert(SUP_NEED <= H-1)`."

**2. D-DEV lands in PR-A, but PROCESS records only arrive in PR-B (D-DEV vs D-SEQ).** D-DEV stores "creator's PROCESS record index and generation" in PR-A. There are no PROCESS records until PR-B, and D4's field list has no "generation" field (only a launch serial).
- *Fix:* "Creator identity = (PROCESS record index, launch serial). D-DEV moves to PR-B, or PR-A keys on (VM slot, per-slot build serial) and PR-B rekeys it." Also add a "record generation/serial" bullet to D4's field list.

**3. The creation quota does not stop the starvation it was added to prevent (D-DEV, D2 E).** Six untrusted children × default quota 2 = 12 channels. The supervisor needs 2(P−1)+B+1 = 21 of a 24-record pool. 12 + 21 > 24, so hostile children can still make a launch or broker step fail.
- *Fix:* "E ≥ supervisor need + Σ child creation quotas + fixtures, asserted. M4 children default to creation quota **0**; nothing in the demo needs a child to create channels, because the broker creates them."
- Also correct the arithmetic: 14+6+1+2 = **23**, not 24. Either state the +1 headroom or change the value.

**4. A child can push capabilities into the supervisor over its readiness channel (D7).** Carrying a cap needs TRANSFER on the *carried* handle (`endpoint.h:338-343`), not on B. So a child sending through B with SEND only can attach its own all-rights endpoints. Two consequences:
- If the supervisor receives with hcap = 0, the head message returns SHORT_BUFFER and is never dequeued (`endpoint.c:829`), so it stays stuck at the front.
- If the supervisor does accept the caps, they use up its quota (see #1).
- The queued caps also keep pool records alive after the creator dies (D-DEV drops the charge, but the record stays).
- *Fix (add to D7):* "The supervisor receives from A with hcap = 0. SHORT_BUFFER, or any first message other than exact READY, is a failed start, and the supervisor **closes A without draining it**. Closing A destroys the queue and releases the queued caps. A is also closed on every death or restart of that generation. The same applies to every control and broker channel the supervisor reads."

## P2: should fix

**5. PROC_SPAWN mutates the caller's table, and that step can fail after the point of no return (D5).** "The caller's table is never mutated by spawn" contradicts returning a PROCESS handle in the caller's table. The rollback list has no step that allocates that handle, so publishing can fail with H_QUOTA after the thread slot exists.
- *Fix:* "Step 0: reserve the caller's PROCESS slot (QUOTA/NOSLOTS checked first). Publish = commit the reserved slot, which cannot fail. The only mutation of the caller's table is that one slot."

**6. A PROCESS record must reach the live thread, but D4 says it never points at the VM slot.** "Never point at it" (D4 Why) conflicts with PROC_KILL and D9, which must find the victim's thread. Today death data is keyed only by thread slot (`sched.c:139-156`, `:503-526`).
- *Fix:* "A LIVE/DYING record holds (thread slot, vm index), cleared by the reaper in the same IF=0 section that sets DEAD. A DEAD record holds no slot link."

**7. Driver death is not wired to device detach (D9 vs D4).** `mark_dying` only releases the waiter (`sched.c:139-156`); the handle drain happens at reap, inside `je_vm_destroy`.
- *Fix:* "`mark_dying` runs D9 revoke steps 1–6 synchronously when the slot is the bound driver."
- The binding should name (record index, serial), and the bitmap byte 0x5F opens only when it matches the dispatched thread's record. Otherwise slot or record reuse could open COM2 to a different process.

**8. DEVICE authority comes from the binding, not the handle (D9/D3).** Nothing uses the PIO right: the bitmap opens on "bound driver dispatched". A driver that closes its DEVICE handle keeps port access. Also, "make the driver's DEVICE handle stale" (step 5) has no mechanism: handle slots hold only object, type and rights (`handle.h:72-78`). Nor is it said what DEVICE `release` does.
- *Fix:* "The DEVICE handle's slot records the binding generation. Ops check handle-gen == record-gen, else STALE. Closing the bound driver's last PIO handle runs revoke. The PIO right is what gates the bitmap: the bitmap opens only if the bound PROCESS still holds a live PIO handle."

**9. The IRQ path has no defined first unmask, and assign preconditions are missing (D9).**
- *Fix:* "DEV_ASSIGN unmasks IRQ3 after the mint and sets last_unmask = now. DEV_ASSIGN requires KILL on `process_h` and a LIVE record, else STALE. DEV_REVOKE with no binding returns OK (idempotent). A stale IRR edge latched from a prior binding may produce one pending event for the new driver; the driver must tolerate it."

**10. The superseded-ADR lists disagree.**
- ADR 0010's Consequences omits **ADR 0009 D5**. The 0009 back-pointer lists it, and D-DEV changes D5's "released at record destroy regardless of holder" to dropping the charge when the creator dies.
- ADR 0010 also omits **ADR 0008 D7** and **ADR 0008's SELF-only dup/query**. Both are listed as blocking contracts, but no back-pointer was added to 0008 (the diff touches only 0007 and 0009).
- ADR 0007's pointer says "process count only", but ADR 0007 D1 also scopes out a ring-3 device and assumes a kernel-only PIC. D9 changes that.
- *Fix:* list all of these under "Superseded in part", and add a back-pointer to 0008.

**11. The service state machine is incomplete (D11).** `failed-start` has no outgoing edge, and `backoff` is used but is not a state. `stopped` is terminal with no restart edge.
- *Fix:* add rows "failed-start → backoff | failed-terminal", "backoff → starting", "stopped → starting (operator)".

**12. D13 and D-DEV lack ADR 0000 parts.** Neither has Why, If otherwise, or Flex.
- *Fix (D-DEV):* "If otherwise: charging the holder leaks on transfer (0009 D5). Flex: revisit with per-service pools."
- *Fix (D13):* "If otherwise: Standard profile for the supervisor understates reaper authority. Flex: drop the supervisor to Standard once policy/mechanism are split (INIT.md:12)."

**13. D12 cannot reap from where supervisor death happens.** The reaper requires `current == 0` (`sched.c:466`), but supervisor death happens on the supervisor's own syscall or fault path.
- *Fix:* "Supervisor death sets a kernel flag. The slot-0 idle loop performs the kill-all, reap, and pool check, then halts."

## P3: nits

**14. "Bits are globally unique across types" is false (D3).** INSPECT 0x1 is shared by SELF, ENDPOINT and PROCESS.
- *Fix:* "Bits are unique across types, except INSPECT, which is common."
- Also note that LAUNCHER and DEVICE lack INSPECT, so RIGHTS on them always returns DENIED.

**15. "ENDPOINT (retain-before-commit as today)" overstates what ring 3 can do today (D3).** Ring-3 dup and query are SELF-typed (`syscall.c:423-457`), so ring 3 gets WRONGTYPE on an endpoint today. Endpoint dup exists only in-kernel (`endpoint.h:252`). Say this exposes a new ring-3 behaviour.

**16. D4's reap order contradicts the transcript without explanation.** The transcript says "release walk → death record → slot FREE" (`m4-contract.md:213`); D4 says record → drop_user → destroy. Record which order was chosen and why. Both are unobservable under IF=0.

**17. The quota packing layout is not defined (D5).** Specify it, e.g. `quotas[15:0] = handle quota`, `[31:16] = creation quota`, `[63:32] = 0, else INVALID`.

**18. Two small precision gaps in D8 and D9.**
- D8's victim table should say that SLEEPING is unreachable for user threads (`sched.c:387`) and that RUNNING is impossible on one CPU.
- D9 should say "master spurious IRQ7: no EOI", and IRQ gates 32–47 must stay DPL0.

## Confirmed sound

- **Op and constant numbers** are consistent across the D-sections and Consequences: ops 13–19, rights 0x10–0x100, and KILLED = 3 after FAULT 0, EXIT 1, BAD_RETURN 2 (`sched.h`).
- **D9 I/O-bitmap arithmetic is correct:** 0x2F8/8 = 0x5F; 96 bytes cover ports 0–0x2FF; the 0xFF terminator is required; iomap = 104 equals the current `sizeof(tss64)` (`trap.c:22,45`).
- **D9 IRQ facts match the code:** the PIC remap to 0x20 gives IRQ3 = vector 35 and IRQ7 = 39 on the master (`platform.c`). PIT divisor 11932 gives about 100 Hz.
- **D5 startup registers are safe:** `je_uframe_check` leaves GPRs alone and forces IF=1 with IOPL and the other flags cleared (`uframe.c`). The initial frame is zeroed (`sched.c:124`).
- **D6:** `je_sched_ticks()` exists. The hosted test asserting op 13 returns UNSUPPORTED is real (`tests/test_syscall.c:128`).
- **D4 reaper:** slot 0 is always READY and cannot sleep, so round-robin visits it every round, and `reap_dead` enforces IF=0 (`sched.c:466-470`).
- **D8 kill:** `mark_dying` is the single release owner (`sched.c:145`), and it clears `wait_pending`.
- **D7 readiness authentication:** closing the supervisor's B is required for A to see PEER_CLOSED, and because the child's B has no TRANSFER it cannot be forwarded. It holds once #4 is fixed.
- **D2 records and pages:** R = P+H is enough even with dup, because dup creates no records. 584 of 3104 pages matches the transcript's measurement (`m4-contract.md:132`).
- **D9 profile:** the "legacy no-DMA PIO" refusal follows AGENTS rule 5.

### Round 1 — Codex

1. **P1 — PROCESS-record incarnation is underspecified.** D4 lists only a launch serial, while D-DEV requires `(record index, generation)` to distinguish a reused creator. Current endpoint records have their own generation, but no PROCESS-record generation exists (`docs/adr/0010-m4-supervisor-and-devices.md:179-185,591-595`; `include/jeios/endpoint.h:185-198`).
   **Fix text:** “Each PROCESS record has a non-wrapping record generation. D-DEV stores `(record_index, record_generation)` and decrements charge only on an exact match; exhausted generations retire the record slot.”

2. **P1 — DEVICE revoke does not define how a live driver handle becomes stale.** D9 says “make the driver’s DEVICE handle stale,” but revocation is initiated by the supervisor against another process’s table. Current close only mutates the current/specified table (`kernel/core/syscall.c:381-412`). Driver death currently releases only IPC waiters (`kernel/core/sched.c:139-152`).
   **Fix text:** “The DEVICE binding owns the exact child-table slot/generation. Revoke atomically closes that slot, releases the DEVICE reference, masks IRQ, cancels re-arm, clears IOPB authority, and only then clears ownership. Driver death invokes the same path before table destruction. Define lookup result as `STALE`.”

3. **P1 — Supervisor-loss cleanup has no defined authority registry.** D12 requires killing every launched process, but D4/D5 define no kernel-owned supervisor identity, child list, or launch registry. Current reaping only scans scheduler slots and requires slot 0 (`kernel/core/sched.c:464-485`); callers explicitly poll it from `main.c` (`kernel/core/main.c:354-376`).
   **Fix text:** “The kernel maintains a bounded launch registry keyed by PROCESS-record identity, marks the first process as supervisor, and uses it on supervisor death. Cleanup scans at most `P` records/threads; failure to clean any entry is fail-stop.”

4. **P2 — D4 overstates current implementation of automatic reaping.** The ADR says the slot-0 idle loop calls the reaper every iteration, but no such call exists; current calls are test-harness polling only (`docs/adr/0010-m4-supervisor-and-devices.md:200-203`; `kernel/core/main.c:354-376,412-521`; `kernel/core/sched.c:455-485`).
   **Fix text:** “Planned in PR-B; not implemented at the reviewed baseline. PR-B must add and test the slot-0 idle-loop call.”

5. **P2 — Capacity formulas do not equal their provisional values.** At `P=8,B=6`, the endpoint formula is `14+6+1+2=23`, not 24. The PROCESS formula says records are bounded by the supervisor quota, but computes `P+H=40` while the stated supervisor quota is `H−1=31` (`docs/adr/0010-m4-supervisor-and-devices.md:103-110`).
   **Fix text:** “Set `E=23` plus one explicitly named reserve, or change the formula. Set `R=P+Q_supervisor` with `Q_supervisor=H−1`, or explicitly account for the extra slot.”

6. **P2 — “Globally unique rights bits” is false.** D3 assigns PROCESS `INSPECT=0x1`, while `INSPECT=0x1` is already shared by SELF and ENDPOINT (`docs/adr/0010-m4-supervisor-and-devices.md:140-145`; `include/jeios/endpoint.h:80-88`; `include/jeios/abi.h:192-207`).
   **Fix text:** “Rights meanings are globally shared; new rights bits are globally unique. `INSPECT` is intentionally reused across object types.”

7. **P2 — Spawn rollback calls are not valid for every failure point.** `drop_user` requires an existing scheduler user thread, while `je_vm_destroy` requires VM state `READY` (`kernel/core/sched.c:433-452`; `kernel/core/vm.c:375-401`). The stated “drop_user/je_vm_destroy” rollback is therefore incomplete for failures before thread creation.
   **Fix text:** “Use a journal with `thread_created`/`vm_ready` flags: call `drop_user` only after thread creation; destroy the READY VM independently; release grants and PROCESS record in reverse order.”

8. **P2 — Startup-register ABI contradicts the retained transcript.** ADR D5 specifies `RDI=version, RSI=count, RDX..R9=handles`; transcript C4 specifies `RDI=count, RSI..R8=handles, R9=version` (`docs/adr/0010-m4-supervisor-and-devices.md:241-250`; `docs/adr/debates/m4-contract.md:492-507`).
   **Fix text:** “Record why C4 was rejected and update the transcript/ADR to one canonical layout, including required zero values for unused registers.”

9. **P2 — Spawn input bounds are incomplete.** `grants_ptr` has no specified byte span/overflow check, and `quotas` has no bit layout, minimums, defaults, or zero semantics (`docs/adr/0010-m4-supervisor-and-devices.md:223-239`).
   **Fix text:** “Define fixed-width grant/quota layouts; reject `grant_count>4`, multiplication overflow, invalid pointers, unknown quota bits, and out-of-range values before any mutation.”

10. **P2 — D12 silently diverges from INIT’s critical-failure contract.** INIT requires a rescue/watchdog path once supported; D12 chooses halt for M4 but does not list this as a supersession/explicit profile exception (`docs/INIT.md:93-96`; `docs/adr/0010-m4-supervisor-and-devices.md:534-557`).
    **Fix text:** “Add to Consequences: ‘M4 explicitly defines a fail-stop refusal profile and does not claim INIT rescue/recovery; the INIT rescue requirement remains planned for M5a+.’”

11. **P2 — D-DEV lacks ADR 0000 decision-record parts.** It has impact, resolution, and tests, but no explicit Why, If otherwise, or Flex despite the ADR-wide requirement (`docs/adr/0010-m4-supervisor-and-devices.md:18-20,579-601`).
    **Fix text:** Add all three headings and record the rejected creator-quota alternatives.

12. **P3 — Current ABI comments contradict implemented capability transfer.** `abi.h` says cap_count must remain zero until a later step, while decoder/tests and dispatch already accept and process caps (`include/jeios/abi.h:115-145`; `kernel/core/syscall.c:479-520`; `tests/test_syscall.c:108-116`).
    **Fix text:** Update the stale comments to the current M3c transfer contract.

### Confirmed sound

- Fresh readiness channel, SEND-only child end, closing the supervisor’s B alias, and exact first-message validation are sound for authenticating sender generation—not health (`docs/adr/0010-m4-supervisor-and-devices.md:308-337`).
- Kill releasing blocked waiters/reservations through `mark_dying` and not delivering `CANCELLED` matches the current scheduler/IPC contract (`kernel/core/sched.c:139-152`; `docs/adr/0009-m3c-capability-ipc.md:883-904`).
- ABI op numbers 13–19 are internally sequential.
- TSS geometry is arithmetically correct: offset 104, 96 bitmap bytes, one terminator; COM2 maps to bitmap byte `0x5f`, and current frame sanitization forces IOPL=0 (`kernel/arch/x86_64/trap.c:16-49`; `kernel/core/uframe.c:29-41`).
### Round 2 — Claude

# ADR 0010 round-2 review

Most round-1 findings are fixed. One revoke-path bug is new and must be fixed before PR-H. I read the working tree only, against `dc41da5` plus the diffs; nothing was edited or built.

## Round-1 findings (Claude)

1. **FIXED.** H = 40, Qs = 39, SUP 32 + 4 = 36 ≤ 39, and the assert is stated (§D2, lines 108–123).
2. **FIXED.** The record generation is added (lines 197–201), and D-DEV moves to PR-B (lines 709, 750).
3. **FIXED.** Children default to quota 0, and E = 21 + 2 + 0 + 1 = 24 (lines 107, 722). The formula still cannot be checked at build time; see N3.
4. **FIXED.** The supervisor receives with `hcap = 0` and closes A without draining it, on every channel it reads (lines 377–394). This matches `endpoint.c:829`: SHORT_BUFFER means no dequeue.
5. **FIXED-BUT-WRONG.** Step 0 reserves and step 5 commits (lines 302–308). But line 279–280 still says "The caller's table is never mutated by spawn", which contradicts lines 257–258.
6. **FIXED.** The run link is cleared when the record becomes DEAD (lines 202–204).
7. **FIXED-BUT-WRONG.** `mark_dying` does run revoke (lines 215, 521), but revoke step 5 closes the handle only "if the driver is still LIVE". A dying driver is DYING, so its DEVICE handle survives until reap. See N1.
8. **FIXED-BUT-WRONG.** Line 478 says "the binding generation **in the handle**". A handle slot holds only state, generation, object, type and rights (`handle.h:74-80`). The 0008 back-pointer says the generation is "not encoded in the handle". The mechanism is still undefined. See N2.
9. **FIXED.** Assign now requires KILL and a LIVE target, then unmasks; revoke on an unbound device is idempotent; one spurious event is tolerated (lines 481–492, 533).
10. **FIXED.** Consequences lines 799–805 list every superseded item; 0008 now has a back-pointer; the 0007 pointer covers the ring-3 device and the PIC.
11. **FIXED, with a gap.** The table now has all the edges (lines 588–597). But line 602 says failed-terminal "stays until the operator acts", and the table has no edge out of failed-terminal.
12. **FIXED.** D13 has its parts at lines 688–695 and D-DEV at lines 730–740. "Service" is the correct profile name (CODING_STANDARD line 21).
13. **FIXED.** Supervisor death sets `supervisor_lost`, and slot 0 does the cleanup (lines 643–645).
14. **FIXED** (lines 154–157).
15. **FIXED** (lines 174–176).
16. **FIXED.** The chosen reap order and its reason are recorded (lines 217–222).
17. **FIXED** (lines 264–267).
18. **FIXED** (lines 429–430, 513–516).

## Round-1 findings (Codex)

1. **FIXED** (lines 197–201, 711–715).
2. **FIXED-BUT-WRONG.** Same cause as Claude #7: the "still LIVE" guard skips the close on the death path, which is the one path Codex named.
3. **FIXED.** The PROCESS pool doubles as the launch registry, so D12 scans at most R records (lines 210–211, 647).
4. **FIXED** (lines 231–235).
5. **FIXED** (lines 107–110).
6. **FIXED** (lines 154–157).
7. **FIXED.** The journal uses `thread_created` and `vm_ready` flags (lines 310–316).
8. **FIXED.** The rejected C4 layout and the reason are recorded (lines 294–297). The new text has a register defect; see N4.
9. **FIXED** (lines 260–267). With `grant_count` ≤ 4, `grant_count × 8` cannot overflow.
10. **FIXED** (lines 808–810).
11. **FIXED** (lines 730–740).
12. **FIXED as planned work.** The `abi.h` comment fix is scheduled in PR-B (line 750); `abi.h` itself is not changed yet.

## New defects

**N1 (P1): a stale DEVICE release can revoke a new binding.**
- *Scenario:* the driver dies, and `mark_dying` revokes, skipping step 5 because the driver is now DYING. The supervisor can run before slot 0 in the same round. It respawns the driver and calls `DEV_ASSIGN`, which creates binding gen+1. The reaper's release walk then closes the old DEVICE handle. The release hook "runs revoke" (line 488) and tears down the **new** driver's binding.
- Closing the exact slot inside revoke (step 5) also re-enters the release hook.
- *Fix:* "Step 5: if the driver's table still exists (LIVE or DYING), close the recorded slot, but only if the slot's handle generation matches. The DEVICE `release` hook runs revoke only if the closed slot equals the record's recorded (owner identity, slot index, handle generation) **and** a binding is active. Otherwise it only drops the reference. Revoke clears the recorded slot before step 5, so the re-entry is a no-op."

**N2 (P2): the binding check has no storage and would break the admin handle.**
- No slot field can hold "binding generation in the handle".
- "Every DEVICE op checks" it, including ops on the supervisor's ASSIGN handle. The first assign bumps the generation, so `DEV_REVOKE` on the admin handle would then return `H_STALE`.
- *Fix:* "PIO/IRQ ops check that (caller's record identity, handle index, handle generation) equal the record's recorded driver slot; otherwise `H_STALE`. ASSIGN-handle ops are exempt from the binding check." Then line 478 and the 0008 back-pointer say the same thing.

**N3 (P2): the endpoint formula cannot be asserted at build time.** Σ child quotas is a runtime spawn parameter, so `_Static_assert(E >= 21 + 2 + Σ + 1)` (line 123) is not expressible.
- *Fix:* "Assert `E >= 24 + (P−1)·JE_EP_CHILD_QUOTA_MAX`. Alternatively, spawn rejects a creation quota c when the outstanding child quotas plus c exceed the reserve, returning `H_QUOTA`." Also name the "+1 reserve", or drop the word "named".

**N4 (P2): "every other GPR is zero" includes RSP.** RSP must be the initial user stack top. It is also unstated that handle registers beyond n are zero.
- *Fix:* "RSP = the image stack top. Handle registers beyond n are zero. All other GPRs are zero."

**N5 (P2): broker channels can drain the supervisor's creation quota.**
- Charges follow the creator until the record is destroyed. A client that keeps its dead (PEER_CLOSED) end keeps that record alive.
- Each re-request creates another supervisor-charged channel. Over repeated restarts, this exceeds the supervisor's 21-channel quota, and then a readiness channel cannot be created.
- *Fix:* "The broker issues at most one channel per (client, service generation). B is counted as clients × (restart_limit + 1), or the broker refuses beyond B with a v0 broker error."

**N6 (P3): context-switch lookup is missing.** The bitmap check (line 463) needs a mapping from thread slot to record identity, but only record → thread is defined.
- *Fix:* add a back-link from the thread to its record, set at spawn and cleared at reap.

**N7 (P3): the IRQ rate bound needs `last_unmask` updated.** The "one handler run per tick" bound only holds if `last_unmask` is set at every unmask, including the timer-sweep re-arm. Also, `DEV_IRQ_ACK` with `pending` clear should not schedule a re-arm.

**N8 (P3): small wording gaps.**
- `DEV_ASSIGN` into a full child table should be listed as `H_QUOTA`.
- D-DEV says "live creator", but the creator is DYING during its own release walk. State that charges are dropped then.

## Confirmed sound

- The arithmetic now holds:
  - E = 14 + 6 + 1 + 2 + 0 + 1 = 24;
  - SUP = 21 + 2 + 1 + 1 + 1 + 4 + 2 = 32, and 32 + 4 ≤ 39;
  - R = 8 + 39 = 47, rounded to 48.
- Only the supervisor can hold PROCESS handles: they are no-send, and LAUNCHER cannot be duplicated. So the R bound by Qs is valid.
- Closing A without draining it, combined with `hcap = 0`, closes the cap-injection and stuck-head holes.
- Keying the bitmap on record identity defeats both slot reuse and record reuse.
- Stale re-arms are cancelled by the generation check, and the kernel is the only code that unmasks.
- Spawn rollback is ordered so each undo step is valid at its failure point, and it matches the reap order.
- The state-machine edges are complete apart from the failed-terminal exit (Claude #11).
- The supersession list and the three back-pointers are consistent with each other.

This is AI-assisted review, not independent human qualification.

### Round 2 — Codex

## Round-1 dispositions

Claude:

1. **FIXED** — H=40, Qs=39, and `SUP+4 ≤ H−1` are explicit. `docs/adr/0010-m4-supervisor-and-devices.md:108-109`
2. **FIXED** — PROCESS identity is `(index, record generation)`; D-DEV is PR-B. `:194-201,709-715`
3. **FIXED** — child creation quota defaults to 0; E arithmetic includes the named reserve. `:107,112,721-725`
4. **FIXED** — `hcap=0`, failed-start closure, queued-cap release, and all supervisor-read channels are covered. `:377-394`
5. **FIXED-BUT-WRONG** — reserved caller slot is correctly added, but “caller’s table is never mutated” still contradicts the returned PROCESS slot. `:255-258,274-280`
6. **FIXED** — LIVE/DYING records retain `{thread slot, vm index}` and DEAD records clear it. `:197-204`
7. **FIXED** — death invokes synchronous device revoke and binding identity is generation-keyed. `:214-216,471-480`
8. **FIXED** — authority is gated by binding generation and live PIO handle; release revokes. `:463-490`
9. **FIXED** — assign preconditions, first unmask, `last_unmask`, idempotent revoke, and IRR behavior are specified. `:481-492,533-534`
10. **FIXED** — Consequences and all three ADR back-pointers now enumerate the supersessions. `:798-807`; `docs/adr/0008-m3b-typed-handles.md:452-459`
11. **FIXED** — failed-start, backoff, and operator restart edges are present. `:588-597`
12. **FIXED** — D13 and D-DEV now include Why, If otherwise, and Flex. `:688-695,730-740`
13. **FIXED** — supervisor-loss flag plus slot-0 cleanup is defined. `:639-657`
14. **FIXED** — shared INSPECT and LAUNCHER/DEVICE denial are explicit. `:153-157`
15. **FIXED** — new ring-3 endpoint behavior is distinguished from current WRONGTYPE behavior. `:171-176`
16. **FIXED** — reap order and rationale are explicitly frozen. `:217-222`
17. **FIXED** — quota bit layout and zero semantics are specified. `:260-267`
18. **FIXED** — SLEEPING/RUNNING and spurious IRQ7/DPL constraints are stated. `:421-430,513-516`

Codex:

1. **FIXED** — non-wrapping PROCESS-record generation is defined. `:194-201`
2. **FIXED** — exact driver-table slot/generation, synchronous revoke, and H_STALE are defined. `:471-531`
3. **FIXED** — PROCESS pool is the bounded launch registry and supervisor flag identifies the first process. `:206-211`
4. **FIXED** — automatic reaping is explicitly planned, not claimed at baseline. `:231-235`
5. **FIXED** — E=24 and R=48 are arithmetically reconciled. `:103-110`
6. **FIXED** — rights-bit wording is corrected. `:153-157`
7. **FIXED** — rollback uses `thread_created`/`vm_ready` journal flags. `:301-320`
8. **FIXED** — ADR 0010 rejects and explains the transcript’s C4 layout. `:285-299`
9. **FIXED** — fixed grant width, count bound, checked copy, quota packing, defaults, and reserved bits are specified. `:260-283`
10. **FIXED** — M4’s fail-stop exception to INIT rescue is explicit. `:808-810`
11. **FIXED** — D-DEV has all ADR-record parts. `:730-740`
12. **NOT FIXED** — ADR only schedules the stale `abi.h` comment for PR-B; current source still says cap transfer “MUST be 0 until step 5.” `:749-751`; `include/jeios/abi.h:119-122`

## New or newly exposed defects

- **P1 — `DEV_ASSIGN` is not transactional.** Minting the child DEVICE handle can fail due to quota/slots, but the text says binding occurs before minting and gives no rollback. Fix: reserve the child slot first; on any mint failure leave binding, IRQ mask, bitmap, and ownership unchanged; unmask only after commit. `:481-485`

- **P2 — revoke recursion is unspecified.** Supervisor revoke closes the exact DEVICE slot, while closing that slot invokes the DEVICE release hook, which invokes revoke again. Fix: add an idempotent `revoking` guard/shared internal revoke path; bump binding generation exactly once. `:488-490,518-531`

- **P2 — dynamic child endpoint quotas can exceed the fixed E budget.** The formula contains `Σ child quotas`, but spawn has no cumulative reservation/admission rule. Fix: reserve quota against `E−fixed overhead`, or reject every nonzero child quota in M4. `:107,264-267,721-725`

- **P2 — binding-generation storage is ambiguous.** ADR 0008 says object generations are not encoded in handles, while D9 says the generation is “in the handle.” Fix: state that DEVICE slot metadata stores `binding_generation`; the 32-bit handle encoding remains unchanged. `:478-480`; `docs/adr/0008-m3b-typed-handles.md:459`

- **P2 — `failed-terminal` says the operator can act but has no outgoing transition.** Fix: add `failed-terminal → starting` only after explicit operator reset/budget reset. `:593,602`

## confirmed sound

Readiness cap injection, PROCESS identity/reaping, capacity arithmetic, IRQ7 handling, D12 fail-stop cleanup, and requested state-machine additions are now internally consistent. Read-only review only; no build or tests run.
### Round 3 — Claude

## Round 3 verdict on 0010 at the working tree: all 13 items are fixed, none wrongly

**Claude items**

| # | Verdict | Where |
|---|---|---|
| N1 | FIXED | :513-522 (exact-slot + active + not-revoking release guard), :561-570 (step 0 clears the recorded slot, step 5 checks the generation). This is sound: ADR 0008 :170 bumps the generation on close, so step 5 is a no-op on the driver-close path. |
| N2 | FIXED | :491-499 (object-side check, admin handle exempt); 0008 back-pointer :459 agrees. |
| N3 | FIXED | :122-125. `JE_EP_RESERVE` is now named; :26 and :276 force child quota to 0. A P3 remains, below. |
| N4 | FIXED | :296-302 |
| N5 | FIXED for the bound | :662-668. It adds a new P2, below. |
| N6 | FIXED | :217-218 |
| N7 | FIXED | :537-543 |
| N8 | FIXED | :503 (`H_QUOTA`/`H_NOSLOTS`) and :763-764 (DYING during the release walk). |

**Codex items**

| # | Verdict | Where |
|---|---|---|
| ASSIGN transactional | FIXED | :500-511. Only step 1 can fail, and unmask happens after commit. |
| Revoke recursion | FIXED | :559-571 (`revoking` flag; the generation is bumped once, at step 2). |
| Dynamic child E | FIXED | :26 and :276 (`JE_EP_CHILD_QUOTA_MAX` = 0, nonzero → `INVALID`). |
| Binding-generation storage | FIXED | :491 and 0008 :459 |
| failed-terminal exit | FIXED | :638 |

## New defects introduced by these edits

**P2: the broker cap contradicts the new operator reset and the sliding restart window.**
- :663 caps each client at `restart_limit + 1` per service. It doesn't say whether that is a lifetime count or a per-window count.
- If it is a lifetime count: after an operator reset (:638), or once the restart window slides, the service can legitimately restart again. A well-behaved client that closed its old ends is then refused with `BROKER_EXHAUSTED` forever.
- If the counter resets with the budget: a client that keeps its dead ends breaks B ≤ 6 again, which reopens N5.
- *Fix text:* "The per-(client, service) broker counter counts for the client's whole life. Operator reset and window expiry do not reset it. A client that reaches `BROKER_EXHAUSTED` recovers only by its own restart. A new client record identity starts a fresh counter, and its predecessor's death destroys its ends and releases the supervisor's charges (D-DEV)."

**P3: the endpoint assert at :124 hard-codes 21.** 21 is 2(P−1) + B + 1. If P or B changes, the assert silently stops tracking the formula, which defeats it.
- *Fix text:* "`E >= 2*(P−1) + JE_BROKER_MAX + 1 + 2 + (P−1)*JE_EP_CHILD_QUOTA_MAX + JE_EP_RESERVE`, with `_Static_assert(JE_BROKER_MAX <= 6)`."
- It is also worth stating that `JE_BROKER_MAX` lives in a header shared by the kernel and the supervisor image. Otherwise the B assert promised at :129 has nothing to check against.

**P3: the D-DEV wording disagrees with itself.**
- :762-764 releases the charge against the creator's record in any state, including DEAD. :769 says "only ever against the creator's own live record". The 0009 back-pointer repeats "live".
- :774 still says "Σ child quotas" and leaves out the reserve.
- *Fix:* change "live" to "same-identity (LIVE, DYING or DEAD)" in both places, and make :774 quote the D2 formula.

**P3 (optional): IRQ rate bound.** Revoking and then assigning within one tick allows two unmasks in that tick (:506 and the sweep). Only the trusted supervisor can do this, so either add "the bound holds per binding" at :545 or have assign also respect `last_unmask + 1`.

Nothing else in D2, D4, D5, D9, D11 or D-DEV regressed. This is AI-assisted review, not independent human qualification.

### Round 3 — Codex

N1 — **FIXED** — D9 L513–521, L559–571: exact owner/slot/generation match, cleared recorded slot, and `revoking` guard prevent stale release from revoking a new binding.

N2 — **FIXED** — D9 L485–499: binding metadata is stored in the DEVICE record; PIO/IRQ checks are distinct from exempt ASSIGN-handle operations.

N3 — **FIXED** — D2 L107–129; D5 L274–277: M4 fixes child endpoint quota max at zero and asserts the compile-time reserve formula.

N4 — **FIXED** — D5 L296–302: RSP is the image stack top, handles beyond `n` are zero, and all other GPRs are zero.

N5 — **FIXED** — D11 L657–668: one channel per client/service generation, per-client restart cap, and bounded `BROKER_EXHAUSTED`.

N6 — **FIXED** — D4 L217–218: thread-to-PROCESS back-link is set at spawn and cleared at reap.

N7 — **FIXED** — D9 L537–543: no re-arm when `pending` is clear; every unmask updates `last_unmask`.

N8 — **FIXED** — D9 L500–506 lists `H_QUOTA`; D-DEV L762–778 explicitly handles LIVE, DYING, and DEAD creators.

Codex P1 — **FIXED-BUT-WRONG** — D9 L500–510 reserves the slot before binding and delays unmasking, but only failure at step 1 is specified as rollback-safe. It does not state that post-reservation mint/commit is infallible or provide rollback for such failure. Fix text: “After slot reservation, steps 2–4 are infallible; if any can fail, undo the binding, reserved slot, generation, mask, bitmap, and ownership before returning.”

Codex P2 (revoke recursion) — **FIXED** — D9 L559–571.

Codex P2 (dynamic endpoint quota) — **FIXED** — D2 L107–125; D5 L274–277; D-DEV L771–778.

Codex P2 (generation storage) — **FIXED** — D9 L485–499; ADR 0008 L452–459.

Codex P2 (`failed-terminal`) — **FIXED** — D11 L628–638.

New P1/P2 introduced by these edits:

- **P1 — `any → failed-terminal` can bypass cleanup.** D11 L637 permits a ready service to become terminal without D11 L640–643’s stopping/revoke/kill/reap sequence. Fix: remove the unrestricted edge; require live-service policy failures to enter `stopping`, then transition to `failed-terminal` only after authority revocation and reap.

- **P2 — DEVICE binding-generation exhaustion is unspecified.** D9 L487–488 and L504/L564 require non-wrapping increment but define no max-value behavior. Fix: on `UINT32_MAX`, retire the DEVICE (cancel re-arms, reject future `DEV_ASSIGN` with `H_QUOTA`) rather than incrementing or wrapping.
### Lead disposition of round 3

All round-3 items were applied: broker counter counted over the client's whole life; endpoint assert written in terms of P and `JE_BROKER_MAX`; D-DEV and ADR 0009 wording changed from "live" to "same-identity (LIVE, DYING or DEAD)"; IRQ rate bound holds across bindings (assign waits for `last_unmask + 1`); DEV_ASSIGN steps 2–4 stated infallible; `any → failed-terminal` replaced by `stopping` then failed-terminal after the full stopping sequence; binding-generation exhaustion retires the DEVICE. Round 3 raised one new P1 (Codex: the `any → failed-terminal` edge could bypass the stopping cleanup) and several P2/P3 items. The lead applied each reviewer's own fix text. No round 4 was run, so these last edits were checked by the lead only, not re-reviewed by a peer. The PR records this.
