# ADR 0010: Freeze the M4 service-supervisor and bounded-device contract

Status: **accepted contract**, 2026-10-30. The owner approved all eight escalated
decisions (D-OWNER-1 through D-OWNER-8) on 2026-10-30.

Scope: M4 only, for issue [#53](https://github.com/auto44e/jeios/issues/53). This
ADR contains no kernel code. It turns the M4 plan into reviewed decisions that a
sequence of small PRs (D-SEQ) implements and tests against. ADR 0007 played the same
role for M3a, 0008 for M3b and 0009 for M3c.

Reviewed baseline: `main` at `dc41da5` (MP1 merged; M3 complete).

Design record: [debates/m4-contract.md](debates/m4-contract.md). Claude (in the
kernel-architect seat) and Codex (in the adversarial-reviewer seat) debated the
lead's draft for two rounds. Both read the repository. AI-assisted design is not
independent human qualification; the owner is the merge gate.

Every decision below follows ADR 0000. **Why** gives the decision and the finding
that settled it, **If otherwise** predicts what each rejected alternative would
cost, and **Flex** says what would reopen the decision.

## Context

M3 ended with two ring-3 processes, one thread each. Both are kernel-seeded from
embedded assembly stubs. They communicate over channels that only the kernel test
fixture can make cross-process. When a process dies, it is reaped by test-only
polling in `main.c`, and its exit status reaches nobody. There is no process
identity, no spawn, no kill, no clock query, and no device that ring 3 can own.

M4 has to deliver the [NEXT_PHASE](../NEXT_PHASE.md) gate in this order:

1. lifecycle and readiness;
2. failure, restart and reconnect;
3. scoped device ownership and grants;
4. one qualified basic virtual device, either DMA-contained or explicitly no-DMA.

[INIT.md](../INIT.md) describes the supervisor itself. Five existing contracts
block it. Each is a frozen or tested surface, and each was escalated and approved:

| Blocking contract | What it currently says |
|---|---|
| ADR 0007 D1 | two processes |
| ADR 0008 D7 | the death path is owner-gated |
| ADR 0009, A6-CLARIFY-2 annotation | kill is "never in the production ABI" |
| ADR 0009 D1 | no supervisor and no more than two processes |
| M3b ops `query`/`rights`/`dup` | SELF-only |

The claim stays narrow. It applies to one CPU, x86-64 QEMU `pc`, and built-in
images. It makes no promise of a stable ABI, SMP, ELF, files, DMA or a hardened
profile.

## Decision

### D1. Scope and security claim

The claim, in full:

> On x86-64 QEMU `pc` with one CPU, a ring-3 supervisor can do the following:
> - launch built-in images with explicitly granted, attenuated handles;
> - authenticate readiness per launch;
> - observe a death and its reason;
> - kill a child;
> - restart it within a bounded budget, while an independent peer keeps making progress.
>
> Clients reconnect through the supervisor and are told the service restarted.
> One port-I/O-only device (COM2, a 16550) is owned by a ring-3 driver. Its authority
> is bounded to its eight ports and a rate-limited IRQ. It can be revoked, timed out
> and detached.

What M4 does **not** cover:
- SMP, more than one thread per process, shared memory, mmap or zero-copy buffer grants;
- user-mapped MMIO, DMA, IOMMU, PCI or virtio;
- ELF loading in the kernel, files or storage;
- a stable ABI or a hardened or release claim;
- any work on ARM. MP2 follows M4 separately.

**Why:** ADR 0007's security shape (D2, D3, D6, D8) is reused unchanged. Only the
FLEX scope limits are widened, by D2 and D-OWNER-1, and each widening has its own
test.

**If otherwise:** a broader claim, such as "drivers are contained", would contradict
DESIGN §10.1 and ADR 0002. On this platform there is no IOMMU and no IRQ remapping.

**Flex:** the device profile and the platform are revisited at M8, or when an IOMMU
profile is qualified.

### D2. Capacity, with formulas (D-OWNER-1)

Actors needed at the same time in the full demo:

| Actor | Count |
|---|---|
| Supervisor | 1 |
| Restartable service | 1 |
| Independent survivor/client | 1 |
| Readiness spoofer | 1 |
| UART driver | 1 |
| Spare slot for an old instance still DYING while its successor launches | 1 |
| Headroom | 2 |

Formulas, with the **provisional** values PR-A must confirm:

| Quantity | Formula | Provisional |
|---|---|---|
| `JE_VM_MAX_PROCESSES` P | actors + spare + headroom | 8 |
| `JE_THREAD_MAX` T | 1 idle + 7 M2 workers + P | 16 (the M2 test stays at exactly 7 workers) |
| `JE_EP_POOL_CAP` E | supervisor need 2(P−1) readiness and control + B broker pairs (B ≤ 6) + 1 sleep = 21, + 2 kept M3 fixtures, + (P−1) × `JE_EP_CHILD_QUOTA_MAX` (= 0 in M4, see D-DEV), + 1 reserve (`JE_EP_RESERVE`, unassigned headroom for a fixture or test) | 24 |
| `JE_VM_HANDLE_CAP` H | supervisor need SUP = 3(P−1) + 2 sleep ends + 1 SELF + 1 LAUNCHER + 1 DEVICE admin + 4 launch staging + 2 broker transient = 32, plus 4 headroom for inbound caps; require SUP + 4 ≤ H − 1 | 40 |
| Supervisor handle quota Qs | H − 1 | 39 |
| `JE_PROC_REC_MAX` R | P live + terminal records still named by the supervisor. PROCESS handles cannot leave its table (D3), so this is bounded by Qs. | P + Qs = 47, rounded to 48 |
| Child handle quota | spawn parameter, 1 ≤ q ≤ H − 1 (default 15) | — |
| Child endpoint creation quota | spawn parameter, 0 ≤ c ≤ `JE_EP_CHILD_QUOTA_MAX`. **`JE_EP_CHILD_QUOTA_MAX` = 0 in M4**, so spawn rejects any nonzero value with `INVALID`. | 0 |

Per-process page quota stays at 73. The measured kernel page pool at the 16 MiB CI
profile is 3104 pages (at `dc41da5`; 3067 after PR-A's larger static pools), and
P × 73 = 584.

**PR-A rule:** the numbers above are provisional. PR-A must do three things before
any of them is frozen:
- record the static `.bss` growth for every pool;
- pass `make check`, `make smoke`, `make integration` and all three x86 CI profiles,
  including 16 MiB;
- assert each formula with a `_Static_assert` where it is expressible, in
  particular `SUP + 4 <= H − 1` and
  `E >= 2*(P−1) + JE_BROKER_MAX + 1 + 2 + (P−1) * JE_EP_CHILD_QUOTA_MAX + JE_EP_RESERVE`,
  with `_Static_assert(JE_BROKER_MAX <= 6)`. `JE_BROKER_MAX` lives in a header shared
  by the kernel and the supervisor image. Both are compile-time
  constants, so the asserts are expressible.

B is defined by D11's broker cap: B = Σ over broker clients and services of
(`restart_limit` + 1). PR-F picks the demo's client count and `restart_limit` so
that B ≤ `JE_BROKER_MAX` (6), and asserts it. Otherwise E grows by the same formula in a reviewed
change.

> **D2 enforcement clarification (2026-11-01, owner-approved; numeric bound unchanged).**
> The Σ formula above is the SIZING budget; it is NOT the runtime enforcement. PR-G
> cross-lineage design review established that a brokered channel is destroyed (and
> the supervisor's `ep_charge` released) only when BOTH its ends reach zero refs —
> and a *service* can pin a dead client's end exactly as a client can pin a dead
> service's. So the per-(client,service) lifetime counter alone does not bound the
> number of live broker channels. The true bound is enforced by a supervisor-owned
> **ledger of live granted channels**: an entry is provably freeable only when both
> the client and the service PROCESS identities are dead (the brokered ends carry no
> `TRANSFER`, and queued cap copies are discarded when a RECV end closes, so those
> two processes are the only possible holders). The broker refuses to create a new
> channel (answering a v0 `BROKER_UNAVAILABLE`/`BROKER_EXHAUSTED`) once the live
> ledger count reaches `JE_BROKER_MAX`. The numbers (B ≤ 6, E = 24) and the Σ
> sizing are unchanged; this note only states where the bound is actually enforced.
> The per-client lifetime cap remains as D11 policy on top of the ledger.

PR-A also replaces the fixed handle quota (`JE_VM_HANDLE_CAP − 1`, set in
`vm.c`) with a per-table quota field. The quota value is a build parameter in
PR-A, and becomes a spawn parameter in PR-C.

**Why:** both peers agreed the original two slots cannot host the demo. Codex
showed that handle capacity and terminal PROCESS records were missing from both
drafts, and that six processes forces a reaper-ordering assumption. The ADR text
review then found that the first version's supervisor need (32) exceeded its
quota (31), and that its endpoint formula summed to 23, not 24. Both are fixed
above with explicit headroom and a named reserve.

**If otherwise:**
- P = 6 needs the old instance reaped before every relaunch, a coupling that is hard to test.
- Larger round numbers grow the O(N²·pages) `je_vm_audit` and the static pools with no consumer.

**Flex:** recompute the numbers when the actor list changes. Raising a cap is a
reviewed change, not a tuning knob.

### D3. Object types, rights and typed dispatch (D-OWNER-2, D-OWNER-4)

New object types are appended to the existing ones (SELF 1, ENDPOINT 2):

| Type | Value | Rights | Who holds it | Can DUP? | Can be granted or sent? |
|---|---|---|---|---|---|
| PROCESS | 3 | INSPECT 0x1, KILL 0x10 | The parent, which is the supervisor in M4. Never the child itself. | Yes (attenuating) | No |
| LAUNCHER | 4 | SPAWN 0x20 | Only the kernel-seeded first process | No (`H_DENIED`) | No |
| DEVICE | 5 | PIO 0x40, IRQ 0x80, ASSIGN 0x100 | ASSIGN on the supervisor's admin handle; PIO and IRQ on the bound driver's handle (D9) | No | No |

Rights-bit values are frozen once asserted; ADR 0008 allows `RIGHTS_KNOWN` to grow.
Every new bit is unique across types. INSPECT (0x1) is deliberately shared by SELF,
ENDPOINT and PROCESS, as it is today. LAUNCHER and DEVICE do not carry INSPECT, so
`RIGHTS` on them returns `H_DENIED`. `QUERY` needs no right, so it still works on
them.

Only ENDPOINT handles can be sent with `try_send` (endpoint.h, the send-cap path).
PROCESS, LAUNCHER and DEVICE handles therefore stay in the table they were minted
into. D9 is what places the driver's DEVICE handle.

`query`, `rights` and `dup` become typed through one static table indexed by type:

```c
{ known_rights, dup_policy, inspect_result, retain, release }
```

- `QUERY` resolves any live type and returns it in RDX.
- `RIGHTS` requires INSPECT and returns rights masked by `known_rights`.
- `DUP` follows `dup_policy`:
  - DENIED: LAUNCHER, DEVICE;
  - fresh-slot attenuate: SELF; ENDPOINT, using the existing in-kernel
    retain-before-commit `je_endpoint_handle_dup` (this exposes endpoint
    dup/query/rights to ring 3 for the first time; today ring 3 gets
    `H_WRONGTYPE`); PROCESS, retaining the record.
- `CLOSE` calls `release` after the slot is freed. The existing exact type-peek
  (ADR 0009 A3-3) is generalised to all types.

Every existing M3b SELF assertion stays valid. The PR adds an exhaustive matrix
of op × type × rights on the tested M3b paths.

**Why:** today query and dup are hard-wired to SELF (`syscall.c` query/dup
handlers). Codex asked that dispatch land together with its first consumer, so it
ships in PR-B with PROCESS.

**If otherwise:** per-type syscalls would multiply the ABI. A dispatch PR without a
consumer would change tested behaviour with no observable reason.

**Flex:** a generic DUP/GRANT right (ADR 0008 amendment) remains future work.

### D4. PROCESS records, death, reaping and status (D-OWNER-2)

**The record.** A PROCESS record lives in a static pool `[JE_PROC_REC_MAX]`. Its
fields are:
- a state: FREE, LIVE, DYING or DEAD;
- a **record generation**, u32, non-wrapping, starting at 1 and bumped on every
  FREE→LIVE. A slot whose generation reaches its maximum is retired, as in ADR 0008
  D4. **Record identity** is the pair (index, generation). Everything that must not
  confuse two incarnations keys on it: the D-DEV charge, the D9 binding, the D12
  registry.
- a **run link** {thread slot, vm index}, valid only while LIVE or DYING. The
  reaper clears it in the same IF=0 section that sets DEAD. A DEAD record holds no
  slot link, so a reused slot can never answer for it.
- the reason and exit code;
- a `supervisor` flag, set only on the kernel-seeded first process;
- a handle reference count;
- the endpoint-creation charge counter and its limit (D-DEV).

Each user thread slot gains a **back-link** to its PROCESS record identity. It is
set at spawn, cleared at reap, and used by D9's per-dispatch bitmap decision.

The PROCESS pool is also the **launch registry**: D12 scans it, at most R entries.
No separate child list exists.

**Lifecycle:**
- **Death** reaches the record through the single existing release owner,
  `mark_dying`. It sets the record DYING with its reason and code. If the dying
  process is the bound driver, it also runs the D9 revoke sequence synchronously.
- **Reaping order** is fixed, all inside one IF=0 section: `drop_user`, then
  `je_vm_destroy` (release walk and unwind), then the record becomes DEAD and its
  run link is cleared, then the slot is FREE. The debate transcript gave this same
  order; the ADR text review noted that a first draft of this ADR listed DEAD first.
  The two orders cannot be told apart under IF=0. This one was chosen because DEAD
  then really does mean "nothing of this process remains".
- **Freeing the record:** the record is freed only after the slot is FREE and its
  last handle has closed.

**Status op.** `PROC_STATUS(h, out_ptr)` (op 15) requires INSPECT. It copies
`{u32 state, u32 reason, u64 code}` to user memory through the existing
copy-out/fixup path. It is non-blocking and idempotent. There is no blocking
`proc_wait` in M4: that would add a new kind of blocked thread under ADR 0009 D7.

**Automatic reaper (planned for PR-B; not implemented at the reviewed baseline,
where the only callers are the test harness loops in `main.c`).** The slot-0 idle
loop will call `je_sched_reap_dead()` with IF=0 on every iteration. Slot 0 is a candidate on every round-robin pass, so the reaper runs
at least once per scheduler round. The test-only polling callers in `main.c` remain
valid, because the call is idempotent.

**Death reasons:** FAULT 0, EXIT 1 and BAD_RETURN 2 stay. **KILLED 3** is appended.

**The launch serial** is kernel-internal and never exported (D5, D-OWNER-1 of
ADR 0009).

**Why:** INIT.md:86-89 says reaping releases references exactly once and records
must not be confused with reused identifiers. Both peers required the record to
outlive the VM slot and never to point at it.

**If otherwise:**
- Reading status from the VM slot would let a relaunched process answer for its predecessor.
- A queued death message could be dropped from a queue of depth 4.

**Flex:** a blocking wait, or a wait-set, needs its own ADR once polling cost is
measured (D6).

> **D4 flex note (2026-11-01, owner direction).** A future `PROC_STATUS` could
> additionally return the record identity `(index,gen)` in RDX as an additive,
> v0 extension, giving ring-3 a stable process-identity primitive instead of
> relying on handle-value stability. Deferred 2026-11-01 (owner): PR-G keys its
> broker ledger on the supervisor-owned PROCESS handle instead, so no ABI change
> is needed now. Revisit if another component needs process identity at ring 3.

### D5. Spawn, launch grants and rollback

The call is `PROC_SPAWN(launcher, image_id, grants_ptr, grant_count ≤ 4, quotas)`
(op 14). It requires SPAWN and returns RDX = the new PROCESS handle, with rights
INSPECT | KILL, in the caller's table. That one slot is the **only** change spawn
makes to the caller's table.

**Inputs**, all validated before any state changes:
- `grant_count > 4` → `INVALID`.
- `grants_ptr` is copied in as `grant_count × 8` bytes through the checked user-copy
  path. A bad pointer gives the existing copy-fault status, and nothing changes.
- `quotas`: bits [15:0] are the child's handle quota (1 ≤ q ≤ H − 1; 0 means the
  default, 15). Bits [31:16] are its endpoint creation quota (≤
  `JE_EP_CHILD_QUOTA_MAX`, which is 0 in M4; any other value → `INVALID`). Bits [63:32] must be zero, else
  `INVALID`.
- An unknown `image_id` → `INVALID`.

**Images.** `image_id` indexes a kernel built-in image table. Each image is still
the checked `je_vm_image` v1 descriptor (ADR 0007 D8). It is produced at build time
by a host tool, never parsed as ELF in the kernel (see D10).

**Grants.**
- Each grant is `{u32 handle, u32 rights}`: 8 bytes, little-endian, with no
  padding. It names an ENDPOINT in the caller's table.
- The source must hold TRANSFER, and `rights` must be a subset of the source's rights.
  This is the same rule as send-caps.
- Each grant is copied with attenuation into the child's table. The grants never
  change the caller's table. The only change to the caller's table is the one
  reserved PROCESS slot.

**Quotas.** `quotas` packs the child's handle quota and endpoint-creation quota. Each
is bounded by its maximum.

**Startup register ABI (v1).** At the child's first dispatch:
- RDI = 1, the startup version;
- RSI = n, the number of grants;
- RDX, RCX, R8 and R9 = the minted handles in grant order. A handle register beyond
  n is zero;
- RSP = the image's stack top, as today;
- every other GPR is zero.

The values are real index/generation handles, not ordinals. Each is
zero-extended to 64 bits. The image must save them before its first syscall.

The debate transcript's C4 proposal used a different layout: RDI = count, RSI–R8 =
handles, R9 = version. The ADR puts the version in RDI instead. A future startup
version can then change every other register while the first one read still
identifies the layout. The initial frame is still zeroed, then these fields
are set (`sched.c` user frame build). `je_uframe_check` permits arbitrary GPRs and
still forces IF=1 and IOPL=0, so no kernel value leaks.

**Rollback.** The operation is transactional, and its steps are journaled:
0. reserve the caller's PROCESS handle slot, so the quota and free-slot checks
   happen first;
1. allocate the PROCESS record;
2. VM build (`vm_ready`);
3. grant mints into the child's table;
4. create the thread slot (`thread_created`);
5. publish, which commits the reserved slot and cannot fail.

On failure the journal is undone in reverse:
- `drop_user`, only if `thread_created`;
- `je_vm_destroy`, only if `vm_ready`. Its release walk closes any grants already
  minted.
- return the PROCESS record to FREE. Its identity was never exposed, and the next
  allocation bumps the generation anyway;
- release the reserved caller slot.

Spawn returns the first failing status. The caller gets no handle and the child
never runs. Tests inject a failure at every step and at every grant index, and
assert that page, endpoint, handle and record pools all return to baseline.

**Why:** codex found that fixed handle values 1..n conflict with the lowest-free
allocator, and that the child had no way to learn its grants (`handle.c`
allocator; zeroed initial frame). Registers need no new frame kind and touch no
image page, so the A6-CLARIFY-1 immutable-text premise is untouched.

**If otherwise:**
- A kernel-written launch-info page is a new kernel→user write into the image after validation.
- Ordinal handles would break when the allocator changes.

**Flex:** if more than 4 grants are needed, move to a read-only launch-info segment
under a new startup version.

*Implementation note (PR-C, 2026-10-31; lead decisions, not new owner-gated contract):*
- **Step 0 uses a preflight.** "Reserve the caller's slot" is implemented as a read-only `je_handle_can_alloc` preflight plus the publishing `je_handle_alloc`, not as a RESERVED slot state. A new slot state would change ADR 0008's frozen slot states. The preflight is equivalent here, because the whole syscall runs at IF=0 on one CPU and nothing touches the caller's table in between.
- **The supervisor gets its LAUNCHER through startup v1.** RSI = 1 and RDX = launcher. The ADR left this unspecified.
- **Precedence** is launcher, then image id, then quotas (including n > the effective child quota), then grants, then the preflight, then the journal (pinned in `spawn.h`). Internal NOMEM/BUSY map to `H_NOSLOTS`, because ADR 0010 adds no new statuses.
- **Rollback once the child thread exists** uses the pinned kernel-forced teardown, `on_dying(KILLED)` then `on_reaped`. Abandon is only for an unlinked record.
- **No `je_vm_audit` inside spawn**, because spawn runs at IF=0 and the audit is O(N²·pages). The QEMU fixture audits the child instead. This is recorded as a deviation from the M3 fixture habit of auditing every build.
- **The fail-Nth hook stays compiled in, but kernel-internal only.** Nothing in the syscall path calls it, and it is disarmed after every spawn.

### D6. Supervisor event loop and time (D-OWNER-7)

The kernel adds no wait-any, wait-set or badge primitive in M4. The supervisor
polls:
- `TRY_RECV(timeout=0)` over its readiness and request channels;
- `PROC_STATUS` for every child.

It then sleeps one tick with `TRY_RECV(timeout=1)` on a private channel. Nobody
sends on that channel, so it always times out.

`CLOCK_TICKS` (op 13) is appended. It takes no arguments and needs no authority. RDX
is the monotonic count of 100 Hz PIT ticks since boot (`je_sched_ticks()`). It is not
wall time and never decreases. The hosted test currently expects op 13 to return
UNSUPPORTED; it is updated.

All supervisor deadlines and restart windows are computed from `CLOCK_TICKS`.
Manifest milliseconds are converted at 100 Hz, rounding up. A deadline longer than
`JE_IPC_MAX_WAIT` is reached by a bounded number of re-arms.

**Why:** Claude showed that counting one's own timed receives undercounts by up to
one tick per other runnable thread per iteration, because user code runs with IF=1
and can be preempted. That would make "3 restarts in 60 s" unenforceable. The read
is side-effect free, and TRY_RECV timeouts already expose tick granularity.

**If otherwise:**
- A wait-set means new scheduler wait states, which reopens ADR 0009 D7.
- Badges change the frozen handle encoding (ADR 0008 D2).
- Returning the tick count from `TIMED_OUT` changes a frozen op's outputs.

**Flex:** add a wait-set ADR if the measured polling CPU cost or latency (about
10 ms at worst) blocks a later gate.

### D7. Readiness authentication

For each launch, the supervisor does the following:
1. `ENDPOINT_CREATE` a fresh channel (A, B).
2. Grant B to the child with rights SEND only: no RECV, no TRANSFER, no INSPECT.
3. Close its own B handle immediately after a successful spawn.
4. Keep A (RECV) and never broker it.

Readiness is the **first** message on A. It must be exactly the opcode
`JE_SVC_READY` with the exact length, received before the deadline.

The supervisor always receives from A with `hcap = 0`. A child that attaches
capabilities makes the receive return `SHORT_BUFFER`. Its message is not
dequeued, and the supervisor's table cannot be filled through B.

A failed start is any of:
- `SHORT_BUFFER`;
- a malformed first message;
- `PEER_CLOSED` before READY;
- the deadline passing.

On a failed start the supervisor kills the child, and closes A **without
draining it**. Closing a RECV end discards its inbound queue and releases every
queued cap (`release_handle`, ADR 0009 A5-3 discard ordering), so nothing a hostile
child queued survives. A is also closed when that generation dies or restarts.

The same rules apply to **every** channel the supervisor reads: control, broker and
driver. Each receive uses an explicit `hcap` sized for what the protocol expects
(0 unless the protocol carries caps). A protocol violation closes that channel.

This authenticates "this launch generation sent this opcode". The fresh record is
reachable only through B. B cannot be forwarded: carrying a cap requires TRANSFER
on the carried handle, and `dup` cannot add rights. A dead generation's B was torn
down with its table, and a restart gets a new channel.

Readiness proves neither health nor correctness. INIT.md calls health probes a
separate liveness mechanism. No instance id is carried in READY.

**Why:** the D5 draft, plus both peers' conditions. Leaving the supervisor's B alias
open would let it fake READY, and it would also stop A from ever reporting
`PEER_CLOSED` on the child's death. The ADR text review (Claude) found the
cap-injection hole: carrying a cap needs TRANSFER on the *carried* handle, not on
B. The `hcap = 0` and close-without-drain rules close it.

**If otherwise:**
- Caller-asserted identity violates ZERO_TRUST:48.
- A shared readiness channel needs badges, which D6 rejects.

**Flex:** firm for M4. Revisit if receive becomes exclusive, or a GRANT right is added.

### D8. Kill (D-OWNER-3)

`PROC_KILL(h)` (op 16) requires KILL. It is synchronous on one CPU with IF=0, and it
calls the existing `mark_dying` with reason KILLED.

| Victim state | Result |
|---|---|
| READY at a ring-3 boundary | DYING; never dispatched again |
| BLOCKED_IPC, waiting | The waiter and its owned record reference are released by the single release owner. `wait_pending` is cleared, so the continuation never runs. |
| READY with a WOKEN reservation | The reserved head is un-reserved (A6-CLARIFY-2). The message stays queued for any other RECV holder. |
| Self (`h` names the caller) | `INVALID`; use EXIT |
| Already DYING or DEAD | `OK`, idempotent. The first terminal reason wins. Idempotence is decided from the PROCESS record, never the VM slot. |
| Kernel threads, the supervisor | Cannot be named. The supervisor has no PROCESS handle. |
| RUNNING (another thread) | Impossible on one CPU: only the caller is RUNNING. |
| SLEEPING | Unreachable for user threads: only kernel threads sleep (`je_thread_sleep`). |

No syscall can be killed half-way. Syscalls run with IF=0 in one bounded critical
section, and a capability send is a single section.

The killed process never observes CANCELLED. CANCELLED stays reachable only
internally (ADR 0009 A6-2), for a future non-lethal cancellation.

**Amends:** the ADR 0009 A6-CLARIFY-2 annotation that the forced-kill path is "never
in the production ABI". The test-only hook remains for its existing tests.

*Correction (PR-D, 2026-10-31; found by the PR-D code map and verified by the lead with grep):* the sentence above says "The test-only hook remains for its existing tests". That is **false**. The forced-kill test hook described in ADR 0009 A6-CLARIFY-2, "gated like `JE_DEV_COPY_INJECT`", was never implemented:
- `JE_DEV_COPY_INJECT` is the only `JE_DEV_` gate in the code.
- `je_endpoint_kern_wait_cancel` has no callers.
- The release-on-death proof is hosted-only (`tests/test_endpoint.c` case 28). ADR 0009 itself records that.

`PROC_KILL` is therefore the **first** real way to reach `mark_dying` on a non-current READY or BLOCKED_IPC user thread. The original sentence is kept as history. PR-D's QEMU fixture provides the first native evidence for these victim states.

**Re-confirms:** the A6-CLARIFY-1 premise (one CPU, one thread per process,
immutable text). M4 changes none of these three facts.

**Why:** Claude's synchronous model plus Codex's correction. A parked receiver is
logically mid-syscall, but it is released through the same owner.

**If otherwise:** "effective at the next tick" lets the victim keep running after a
successful kill, and needs a second pending-kill state.

**Flex:** SMP or multiple threads must re-gate kill together with A6-CLARIFY-1.

### D9. Device: COM2, the TSS I/O bitmap and the IRQ3 contract (D-OWNER-5)

M4 uses a **no-DMA, port-I/O-only profile**. The device is COM2, a 16550 UART at
ports 0x2F8–0x2FF on IRQ3. It is qualified on **QEMU `pc` only**.

**TSS I/O bitmap**
- The TSS gains an I/O permission bitmap at `iomap = 104`, covering bytes
  0x00–0x5F (ports 0x000–0x2FF), followed by the required 0xFF terminator byte.
- The GDT TSS limit becomes `sizeof(tss_with_bitmap) − 1`.
- Every bitmap bit defaults to deny. Ports above 0x2FF lie outside the limit and
  raise #GP.
- Byte 0x5F (0x2F8/8) is written 0x00 only when the dispatched thread's PROCESS
  record identity (index, generation) equals the active binding's owner **and** the
  binding still has a live PIO handle. It is written 0xFF for every other dispatch.
  That is one byte per context switch.
- IOPL stays 0 in every user RFLAGS. It is already forced by `je_uframe_check`; a
  test asserts it.
- The PIC ports (0x20/0x21/0xA0/0xA1) and COM1 are never granted.

**Ownership**
- A kernel-seeded DEVICE record (one in M4) holds:
  - the owner's PROCESS record identity (index, generation);
  - a **binding generation**, u32, non-wrapping, bumped on every assign and every
    revoke. At `UINT32_MAX` the DEVICE is retired: re-arms are cancelled and every
    later `DEV_ASSIGN` returns `H_QUOTA`;
  - the exact slot (index, handle generation) of the driver's DEVICE handle in the
    driver's table.
- **Driver-op check (object side; ADR 0008's handle encoding and slot layout are
  unchanged).** A PIO or IRQ op must satisfy three conditions, else `H_STALE`:
  - the caller's PROCESS record identity equals the recorded owner;
  - the handle's slot index and handle generation equal the recorded driver slot;
  - a binding is active.

  Admin-handle ops (ASSIGN) check only the ASSIGN right. They are never subject to
  the binding check, so the first assign never invalidates the supervisor's admin
  handle.
- **`DEV_ASSIGN(admin_h, process_h)`** (op 17) needs ASSIGN on `admin_h` and KILL on
  `process_h`. The target must be LIVE (else `H_STALE`) and the device unbound (else
  `H_DENIED`). It is transactional:
  1. reserve a slot in the child's table (`H_QUOTA` / `H_NOSLOTS` on failure);
  2. commit the binding: owner identity, recorded slot, binding generation + 1;
  3. commit the reserved slot as the PIO|IRQ DEVICE handle;
  4. **only then** unmask IRQ3, at the next tick at or after `last_unmask + 1`
     (the same rule as the sweep re-arm), setting `last_unmask` when it unmasks.

  Steps 2–4 are infallible: plain stores into a reserved slot and a fixed record,
  with IF=0. A failure at step 1 leaves the binding, the IRQ mask, the bitmap and
  the ownership unchanged. The minted handle value is returned to the caller, which may
  tell the driver as plain message data. The value grants nothing outside the
  driver's own table.
- Only one binding exists at a time.
- **The DEVICE `release` hook** (D3) runs revoke only if three things hold:
  - the slot being closed is **exactly** the recorded driver slot (same owner
    identity, slot index and handle generation);
  - a binding is active;
  - no revoke is already in progress.

  Otherwise it only drops the reference. So a stale handle closed late can never
  revoke a newer binding. For example, a dead driver's table drained by the reaper
  after a new driver was already assigned does not touch the new binding. Port
  authority still ends as soon as the live driver closes its handle.
- A level or edge latched in the PIC's IRR before a new binding may produce one
  pending event for the new driver. The driver must tolerate one spurious event.

**IRQ contract.** The kernel is the **only** code that unmasks.

IRQ3 is on the master 8259. Codex said "slave"; that is a correction, verified
against the standard PC IRQ assignment, and no cascade change is needed.

When IRQ3 fires, the kernel:
1. masks IRQ3 at the master PIC;
2. sets `pending` on the DEVICE record;
3. sends EOI to the master;
4. never touches the UART or any queue.

`DEV_IRQ_ACK(h)` (op 19, needs IRQ) returns and clears `pending`, but does not
unmask. If `pending` was set, it schedules a re-arm at
`max(now+1, last_unmask+1)`. If `pending` was clear, it schedules nothing and
returns 0. The timer sweep then unmasks, **only if** the record's binding generation
still equals the one recorded at ACK time. A stale re-arm can therefore never unmask
after a detach. **Every** unmask, whether from assign or from the sweep, sets
`last_unmask = now`.

Bound: at most one IRQ3 handler run per tick (100/s), whatever the driver does. The
bound holds across bindings too, because assign also waits for `last_unmask + 1`. A
driver that never clears its interrupt source only starves itself.

Spurious master IRQ7 (vector 39) is detected by reading the master PIC's ISR. A
spurious IRQ7 gets **no EOI** and is never treated as a device interrupt. Today any
vector other than the timer is fatal; this path is new. IRQ gates 32–47 stay DPL0,
so ring 3 cannot raise them with `int`.

**Revoke and detach.** Four paths trigger revoke:
- `DEV_REVOKE(admin_h)` (op 18, needs ASSIGN);
- the driver closing its DEVICE handle;
- the driver's death, from `mark_dying` (synchronously, before reap);
- kill.

All four run one internal IF=0 sequence, guarded by a `revoking` flag so it is
re-entry safe and idempotent:
0. if no binding is active, or `revoking` is set, return `OK`; otherwise set
   `revoking`, and copy and then **clear** the recorded driver slot;
1. mask IRQ3;
2. bump the binding generation, which cancels any re-arm and makes every handle to
   the old binding `H_STALE`;
3. clear `pending`;
4. write 0xFF to bitmap byte 0x5F;
5. if the driver's table still exists (LIVE or DYING), close the copied slot, but
   only if its handle generation still matches. The release hook this triggers sees
   a cleared recorded slot (step 0) and only drops the reference;
6. clear the owner and `revoking`, so the device is unowned.

Only after that may a new assign happen. `DEV_REVOKE` on an unbound device returns
`OK` (idempotent).

**Timeout** is supervisor policy: the driver fails to answer a request within its
deadline, and the supervisor kills it and restarts it within budget. Clients see
`PEER_CLOSED`, and then `SERVICE_RESTARTED` from the broker (D11).

**Hardened isolation.** A request for hardened or IOMMU-contained isolation returns
`UNSUPPORTED` with the profile name "legacy no-DMA PIO". This follows DESIGN:479
and HARDWARE:71, and there is no false-green UI.

**Rejected devices** (from the debate):

| Device | Reason rejected |
|---|---|
| RTC 0x70/0x71 | bit 7 of port 0x70 masks NMI |
| PS/2 0x64 | writing 0xFE resets the CPU |
| isa-debugcon | no IRQ, so the timeout demo would be meaningless |
| virtio and intel-iommu (q35) | DMA, which is out of the M4 scope |

**Why:** the device has port-I/O authority and no DMA path, and its bound is one
bitmap byte. It is the smallest device that still has an IRQ, a timeout and a
detach.

**If otherwise:** mapping MMIO to user space would amend ADR 0007 D3's frame-kind
rules.

**Flex:** reopen at M8 or with an IOMMU profile. Physical PCs with undocumented port
aliases are explicitly unqualified.

### D10. Ring-3 C images

A freestanding ring-3 C build path is added, along with a host tool.
- **Build flags:** `-ffreestanding -fno-pic -nostdlib`, linked at 0x40000000, with
  W^X segments.
- **Host tool:** it converts the linked output into a `je_vm_image` v1 descriptor
  plus its blob. It enforces the same validation rules as `je_vm_image_validate`.
- **Image table:** the result is embedded in the kernel image table.

The kernel never parses ELF, as ADR 0007 D8 requires. The supervisor and the M4
demo services are written in C. Each image's source, hash and size are recorded.

**Why:** the supervisor will not fit in a 1024-byte assembly stub. Converting at
build time keeps the checked-descriptor boundary.

**If otherwise:** an ELF loader in the kernel is a new parser in the Critical
profile with no M4 need.

**Flex:** ELF loading comes in its own ADR, at M5a or later.

### D11. Supervisor policy, the state machine and the broker

**State machine.** One service state machine replaces both the INIT.md and the
DESIGN.md variants:

| From | To |
|---|---|
| inactive | starting |
| starting | ready, or failed-start |
| ready | stopping, or failed |
| failed-start, failed | backoff (budget left), or failed-terminal (budget exhausted) |
| backoff | starting, when the backoff deadline passes |
| stopping | stopped |
| stopped | starting (operator request only) |
| any live state | stopping, then failed-terminal once the full stopping sequence has finished: revoke, kill, reap (supervisor policy) |
| failed-terminal | starting, only after an explicit operator reset that also resets the budget |

How the existing terms map onto it:
- "Stopping" is DESIGN's "quiescing": reject new work, drain until a deadline,
  revoke grants and channels, kill, then wait for the reap.
- "Failed-terminal" covers DESIGN's "quarantined". It stays until the operator acts.
- "Discovered" and "authorized" are supervisor pre-launch validation steps, not states.

**Restart budget**
- Each service has `restart_limit` within `restart_window`. Each restart uses capped
  exponential backoff.
- An aggregate budget applies across all services.
- When a budget is exhausted the service becomes failed-terminal.
- Numeric defaults are chosen after measuring on QEMU, then recorded.

**Dependencies** are ordering only in M4 (one dependency edge in the demo). Losing a
dependency follows its declared stop or degrade policy, and a service never keeps
reporting healthy after losing one.

**Broker and discovery.** A client asks the supervisor for service X over its own
granted channel. The supervisor then:
1. creates a fresh channel for that client and that service generation;
2. transfers one end to the client and one end to the service.

The broker issues **at most one channel per (client, service generation)**. Each
client's total is capped at `restart_limit + 1` per service, counted over the
**client's whole life**. Operator reset and window expiry do not reset that count.
Beyond the cap the broker answers with a v0 `BROKER_EXHAUSTED` reply and creates
nothing.

A client that reaches `BROKER_EXHAUSTED` recovers only by being restarted itself.
The new client record identity starts a fresh count, and the old instance's death
destroys its ends, which releases the supervisor's charges (D-DEV).

Why the cap is needed: the supervisor is charged for a broker channel until the
record is destroyed (D-DEV), and a client can keep its dead end open. The cap keeps
the B ≤ 6 term in D2 a true bound.

When the service dies, the client's end reports `PEER_CLOSED` (existing semantics).
On re-request, the broker answers `SERVICE_RESTARTED`, which carries a
supervisor-level generation number.

The kernel holds no service identity and gains no new status. The broker message
formats live in a v0 header. They are explicitly not a stable ABI, and the manifest
is not an ABI either (INIT.md:131).

**Why:** INIT.md:71-89 requires a visible restarted result. Codex argued it belongs
in the broker protocol, not the kernel. A kernel-level RESTARTED would link identity
across generations, which is exactly what ADR 0009 D-OWNER-1 forbids.

**If otherwise:** a kernel name service that survives a restart is the
"non-owning alias" that reopens D-OWNER-1.

**Flex:** add policy-service separation (INIT.md:12) and multi-edge dependency
graphs at M5a.

> **D11 v0 broker protocol note (2026-11-01, owner-approved; v0, not a stable ABI).**
> Implementing this safely needs more v0 messages than the sketch above. A cap send
> COPIES a descriptor (ADR 0009 A5); the receiver mints on `try_recv`, so the
> supervisor must NOT close its source end until the receiver has minted — a
> premature close hands the receiver handle 0. The broker is therefore a two-phase
> transaction: mint the channel, send the SERVICE end (cap) and the generation via a
> `SERVICE_BIND`, await the service's ACK, send the CLIENT end (cap) + body via
> `BROKER_GRANT`, await the client's ACK, THEN close both source handles (bounded
> pending state + ACK deadlines; on timeout close sources, abort, and charge the
> lifetime cap only if the client actually received its end). Replies beyond
> `BROKER_GRANT`/`BROKER_EXHAUSTED`/`SERVICE_RESTARTED`: `BROKER_UNAVAILABLE`
> (service not ready / died before the supervisor noticed — does NOT consume the
> lifetime cap, distinct from EXHAUSTED which means the cap is spent) and
> `BROKER_ALREADY_GRANTED` (duplicate request for the same still-live generation —
> no second channel, per "at most one channel per (client, generation)").
> `service_generation` is a per-service supervisor u32 (monotonic, non-wrapping,
> bumped on each transition to READY, NOT the kernel PROCESS gen); the service learns
> it only from `SERVICE_BIND`, never from client bytes. Brokered ends carry exactly
> `SEND|RECV` (no `TRANSFER`, no `INSPECT`); the client demuxes replies by channel.
> This is all ring-3 supervisor policy; the kernel gains no new status or identity.

### D12. Supervisor loss: a deliberate refusal (D-OWNER-6)

M4 has no watchdog or rescue path. INIT.md:93-96 defers both until platform support
exists. If the supervisor dies, for any reason, the kernel does the following:
1. `mark_dying` sees the `supervisor` flag and sets a kernel `supervisor_lost`
   flag. It cannot reap there, because reaping must run in slot 0.
2. The slot-0 idle loop sees the flag and prints
   `M4: supervisor lost (reason=…); no recovery path in this profile`.
3. It then scans the PROCESS registry (at most R entries), kills every LIVE record
   (D8) and reaps them.
4. It checks that the page, endpoint, handle and PROCESS pools are back at their
   baseline.
5. It halts.

If cleanup cannot complete, the kernel halts immediately with an invariant-failure
diagnostic. The system never restarts or reboots on its own.

This is a refusal, not recovery, and it must not be labelled "rescue". A QEMU
variant faults the supervisor and asserts the marker and the zero-leak counts.

**Why:** AGENTS rule 5 requires an explicit refusal when a requested guarantee
cannot be delivered.

**If otherwise:** silent continuation would leave orphans with no reaper policy, and
an automatic reboot is the "silent infinite reboot loop" INIT.md forbids.

**Flex:** add a rescue image and watchdog when the platform has a watchdog, at M5a
or later.

#### D12 note — detection mechanism revised to a slot-0 scan (2026-10-31, owner-approved)

Step 1 above says `mark_dying` sets a kernel `supervisor_lost` flag. During PR-F
design this was found to be **unsafe to implement as written**, so the owner
approved (2026-10-31) revising the *detection mechanism* while leaving the D12
*policy* (refuse, kill-all, reap, verify baselines, halt) unchanged.

**Decision.** The slot-0 monitor loop detects supervisor loss by **scanning the
supervisor's PROCESS record by identity before it reaps**: the monitor holds the
supervisor's `je_proc_id` as a plain value (no handle ref) and treats any
`je_proc_status(sup_id)` result other than LIVE as loss (DYING, DEAD, or
reaped-to-FREE — `record_for` rejects a FREE slot or a bumped generation). This is
a strict superset of scanning for DYING and is necessary because
`je_kernel_idle_step` itself reaps: a DYING-only scan can miss a supervisor that
was reaped between two scans. The monitor therefore does **not** call `idle_step`;
it uses its own wait — `cli; identity-scan; je_sched_reap_dead(); sti; hlt` — with
the scan and the reap in a single IF=0 section, and the monitor is the **sole**
reaper entry after seeding. `mark_dying` and `reap_dead` stay byte-identical to
the tested M2/kill paths.

A supervisor that EXITs normally also leaves its record non-LIVE, so the monitor
disambiguates success from loss by the report's `phase`: `phase == DONE` prints a
success marker and runs the same kill-all/reap/baseline/halt sequence; anything
else is a loss. The **loss** decision never depends on the report being readable
(a missing magic prints "no report" and still halts); only the success path reads
`phase == DONE`.

**Why.** `mark_dying` (via `je_proc_on_dying`) is *also* on the normal
kernel-forced teardown path: `vm_s3_destroy` and `spawn_undo` reach it for fixture
teardown and failed-spawn rollback. A `supervisor_lost` flag set inside
`mark_dying` would fire spuriously whenever a supervisor-flagged record is torn
down for a benign reason, turning ordinary teardown and D-rollback into a false
"supervisor lost" halt. The scan keys on the record actually being DYING in the
registry at monitor time, which only happens when the supervisor truly died and
has not yet been reaped — not during a synchronous forced teardown that reaps in
the same slot-0 step.

**If otherwise (rejected: the `mark_dying` hook).** Keeping the flag in
`mark_dying` would require a guard distinguishing supervisor-loss from the forced
teardown callers (an added argument or caller-context check threaded through a
tested path), which is more invasive to already-qualified code and widens the
blast radius of a mistake into the kill/teardown machinery. Predicted cost:
re-qualifying `mark_dying`'s callers and the M4d supervisor-guard fixture, for a
detection that the scan already provides without touching them.

**Ordering constraint.** No other slot-0 reap may run between the supervisor's
death and the monitor scan. This holds because slot 0 is the *only* context that
reaps after seeding, the monitor loop is the sole reaper entry, and it scans
before each `reap_dead`. The monitor asserts the saved supervisor identity is
still the expected `(index, generation)` each pass. (Identity-based detection also
makes this robust: even if the record were reaped first, `je_proc_status(sup_id)`
returning non-LIVE is still observed as loss.)

**Future flex (toward the original D12 wording / "option 2").** If a later design
needs **synchronous** detection at the instant of death — e.g. a watchdog or a
rescue image (D12 Flex above) that must act before the next slot-0 step, or a
multi-core slot-0 where "only slot 0 reaps" no longer holds — revisit this and
move detection into the death path with a proper forced-teardown-vs-loss
discriminator. At that point the `mark_dying`-hook alternative becomes the right
choice and this scan is insufficient. Revisit trigger: any of (a) a watchdog/
rescue path added, (b) reaping moved off a single slot-0 context, (c) a
requirement to observe loss before the next monitor iteration.

**Success-vs-loss signalling, and a planned second mechanism (owner, 2026-10-31).**
Because a clean supervisor EXIT also leaves its record non-LIVE, the success path
reads `phase == JE_SVC_PHASE_DONE` from the report to tell "finished cleanly" from
"lost". The report therefore sits on the *success* decision (never on the loss
decision, which stays fail-closed: a missing/garbage report always halts as loss).
The owner accepted this as the PR-F mechanism and additionally directed that a
**second, independent success signal be added down the line**: corroborate
`phase == DONE` with the supervisor's **death record** — `reason == EXIT` and an
agreed nonzero EXIT code — so success requires BOTH the report phase AND the
kernel-observed exit, matching the "evidence, not attestation" rule already
applied to the spoofer (D3 of the PR-F design notes). The report alone is
untrusted; pairing it with the kernel-side exit code removes the last place a
buggy/garbage report could, by itself, turn a loss into a reported success.
Tracked as a follow-up; the `je_svc_report` layout and the EXIT path already carry
everything needed, so it is additive (no contract change). Revisit trigger: adding
it, or any change that makes the report the sole success signal.

### D13. Coding profile and evidence

All M4 kernel code is in the **Critical** profile (CODING_STANDARD:20). So is the
supervisor, because it holds recovery and reaping authority.

The UART driver is an untrusted ring-3 process whose authority is bounded as
described in D9. Under the no-DMA profile it is **not** called "contained" in the
IOMMU sense.

Each PR records:
- the exact commit and image hash;
- the compiler and QEMU versions;
- the commands and expected markers;
- local and remote CI results, kept separate;
- positive and negative tests, including fail-Nth rollback.

Every PR also carries its decision record, ADR 0000 parts 1–4: decision and why,
predictions for each alternative, flex, and the committed peer transcript. A
hosted test supplements a QEMU run but never replaces it.

**Why:** CODING_STANDARD:20 puts recovery and isolation authority in Critical, and
the supervisor reaps and restarts.

**If otherwise:** putting the supervisor in the Service profile would understate
its authority over every child's lifecycle.

**Flex:** the supervisor can drop to Service once policy is split from mechanism
(INIT.md:12) and the mechanism half is shown to be contained.

### D-DEV. Recorded deviation: ADR 0009 D5 creation quota (D-OWNER-8)

ADR 0009 D5 requires a per-process endpoint-creation quota whose charge is released
when the record is destroyed. **It was never implemented** in M3c:
`endpoint.c`/`endpoint_kern.c` have no creator or charge field. It was also not
recorded as a deviation. Codex found the gap during this debate and the lead
verified it.

Impact today: one process can take the whole channel pool. That is harmless with
two kernel-seeded processes. Under M4 it would let a hostile child starve the
supervisor.

Resolution, implemented in **PR-B**, because it needs PROCESS record identity. In
PR-A the pool formula keeps supervisor need and child charges disjoint.
- Each channel record stores its creator's PROCESS record identity (index,
  generation; D4).
- When the record is destroyed, the creator's charge is decremented only if the
  creator's PROCESS record still has that exact identity, in any state: LIVE, DYING
  (including during its own release walk) or DEAD with handles open. If the record
  was freed or reused, the charge is dropped, because it no longer bounds anyone.

  This narrows ADR 0009 D5's wording "released when the record is destroyed,
  regardless of holder". The charge is still released at destruction, but only
  ever against the creator's own same-identity record (LIVE, DYING or DEAD).
- The pool-global cap still bounds channels whose creator has died.
- Quota values: the kernel-seeded supervisor may create up to its D2 need. Spawned
  children get the spawn parameter, which is capped at `JE_EP_CHILD_QUOTA_MAX` = 0 in M4. Nothing in the M4 demo needs a
  child to create channels, because the broker creates them. So
  E satisfies the D2 formula (supervisor need + fixtures + (P−1) × JE_EP_CHILD_QUOTA_MAX
  + JE_EP_RESERVE), and hostile children cannot
  starve the supervisor.
- Tests: creating past the quota returns `H_QUOTA` with no state change; the charge
  is released when the record is destroyed, after a transfer and after the creator's
  death; and a reused creator record is never decremented by an older channel.

**Why:** ADR 0009 D5 is a frozen requirement that was silently unmet. M4 is the
first point where it matters, because untrusted children run.

**If otherwise:**
- Charging the current holder leaks the charge on transfer (the ADR 0009 D5
  rationale).
- Keying on the VM slot lets a relaunched process inherit its predecessor's
  charges.

**Flex:** move to per-service resource pools if a later milestone budgets
resources per service.

## D-SEQ. PR sequence

Each PR is small, gated by the owner, qualified on QEMU and observed in remote CI.

| PR | Content |
|---|---|
| PR-0 | This ADR and its transcript |
| PR-A | Capacity raise (D2), the per-table handle quota field, measured static sizes, and the re-baselined M2/M3 assertions. Evidence includes the 16 MiB profile. |
| PR-B | PROCESS records, the automatic reaper, `PROC_STATUS`, typed dispatch (D3, D4) and the D-DEV creation quota, using kernel-seeded processes. Also fixes the stale `abi.h` comment that says `cap_count` must be zero (transfer has been live since M3c). |
| PR-C | `CLOCK_TICKS`, LAUNCHER, `PROC_SPAWN`, the startup register ABI and rollback tests at every failure point (D5, D6) |
| PR-D | `PROC_KILL`, with one test per victim state (D8) |
| PR-E | The ring-3 C image path and its host tool (D10) |
| PR-F | The supervisor: readiness, budgets and backoff, a surviving peer, a spoofer, a crash loop running into exhaustion, failed-launch rollback, and the supervisor-loss variant (D7, D11, D12) |
| PR-G | The broker, `SERVICE_RESTARTED`, and client reconnect (D11) |
| PR-H | The TSS I/O bitmap, the DEVICE object, the IRQ3 contract and spurious IRQ7 handling (D9) |
| PR-I | The UART driver service, with timeout, detach and revoke demos, and the hardened request returning `UNSUPPORTED` (D9) |

M4 is complete when #53's acceptance list is observed on QEMU in remote CI. MP2
(mobile #2) follows.

## D-OWNER. Owner decisions (all approved 2026-10-30)

1. Capacity raise (D2): ADR 0007 D1 / ADR 0009 D1 scope, tested M1/M2/M3 constants.
2. PROCESS records and automatic reaping in the death path (D4): ADR 0008 D7.
3. Kill in the production ABI (D8): amends the ADR 0009 A6-CLARIFY-2 annotation and
   re-confirms A6-CLARIFY-1.
4. Typed `query`/`rights`/`dup` (D3): changes the behaviour of tested M3b ops on
   non-SELF handles.
5. TSS/GDT I/O bitmap, and the kernel-owned PIC IRQ3 path (D9).
6. Supervisor loss as a deliberate halt (D12).
7. `CLOCK_TICKS` op 13 (D6).
8. Recording and fixing the ADR 0009 D5 quota deviation (D-DEV).

## Consequences

- ABI v0 grows by appending only:

  | Op | Name |
  |---|---|
  | 13 | `CLOCK_TICKS` |
  | 14 | `PROC_SPAWN` |
  | 15 | `PROC_STATUS` |
  | 16 | `PROC_KILL` |
  | 17 | `DEV_ASSIGN` |
  | 18 | `DEV_REVOKE` |
  | 19 | `DEV_IRQ_ACK` |

  - Object types 3–5 and rights bits 0x10–0x100 are added. Death reason KILLED 3
    is appended.
  - No new status values. Existing statuses cover every M4 failure: `H_DENIED`,
    `H_QUOTA`, `H_NOSLOTS`, `INVALID`, `UNSUPPORTED`.
  - Numbers are frozen once each is asserted by `_Static_assert` in its PR. No
    stable-ABI promise is made.
- These still require a new ADR: SMP; more than one thread per process; shared
  memory or zero-copy grants; DMA or an IOMMU; a blocking wait or wait-set; a kernel
  name service; ELF in the kernel.
- Superseded in part:
  - ADR 0007 D1: the process count, a ring-3 device, and the kernel-only PIC.
  - ADR 0008 D7: the death path gains PROCESS records, the automatic reaper and kill.
  - ADR 0008's SELF-only `query`/`rights`/`dup` ops become typed (D3).
  - ADR 0009 D1: the "no supervisor, at most two processes" scope.
  - ADR 0009 D5: its charge-release wording is narrowed (D-DEV).
  - The ADR 0009 A6-CLARIFY-2 annotation on kill.

  Each of ADRs 0007, 0008 and 0009 gets a pointer back to this one. Everything else
  in them is unchanged.
- INIT.md's critical-failure contract is not met in M4. M4 explicitly defines a
  fail-stop refusal profile (D12) and does not claim INIT rescue or recovery. The
  INIT rescue requirement remains planned for M5a or later.
- This is AI-assisted design review, not independent human qualification. The
  hardened or release gate still needs independent human review (CODING_STANDARD).
