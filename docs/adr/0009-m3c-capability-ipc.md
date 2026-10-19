# ADR 0009: Freeze the M3c bounded-capability-IPC contract

Status: **proposed**, 2026-10-18. Scope: M3c only (issue
[#8](https://github.com/auto44e/jeios/issues/8)). No kernel code accompanies this
ADR; it converts the M3c proposals into reviewed decisions that a sequence of
small PRs implements and tests against, exactly as ADR 0007 fronted M3a and ADR
0008 fronted M3b. Reviewed baseline: `main` at the M3b attenuation merge
(`956bbcb`); M3b (the typed handle table, syscalls `self`/`close`/`query`/
`rights`/`dup`, and ring-3-observable rights enforcement) is complete per
[IMPLEMENTATION.md](../IMPLEMENTATION.md). Two decisions in this ADR are
**owner-gated** and called out explicitly (D-OWNER-1, D-OWNER-2).

## Context

M3b gave each process a handle table that can name objects, but the only object
kind is **SELF** — immortal, resource-free, no pointer. M3c introduces the first
**destroyable, transferable** kernel object: an **endpoint** (one end of a
two-ended **channel**), the substrate for bounded message IPC between the two
user processes. This is the milestone the handle table has been built toward: it
exercises the refcount/destruction machinery ADR 0008 deliberately deferred, and
it is the first place a handle names something that can go away.

The design is deliberately minimal and fail-closed, matching M3a/M3b: single CPU,
IF=0 on the syscall path, two processes max (`JE_VM_MAX_PROCESSES = 2`), no SMP,
no supervisor. The claim is "experimental single-CPU bounded capability IPC
between two processes, qualified by test," never a general message bus, a service
framework, or a restartable supervisor.

M3c is too contract-heavy to build safely without this ADR: it adds object
lifetime with destruction, a transfer transaction, and blocking/wake semantics,
each of which has invariants that are cheap to freeze on paper and expensive to
discover in code. This ADR freezes them; the PR sequence (D-SEQ) implements them.

## Decision

### D1. Scope and security claim

M3c delivers: bounded **endpoints/channels** with a per-object reference count and
destruction on last reference; **copied, bounded, data-only messages**
(`try_send`/`try_recv`, non-blocking first); **blocking waits** with deadline and
cancellation (later PR); **transactional, attenuated capability transfer** of an
endpoint handle to the other process (later PR); and a **fixed-plan bootstrap
fixture** (not an init/supervisor). Retained from M3a/M3b: one CPU, IF=0, the
per-process handle table, the v0 syscall gate, `copy_from/to_user`. **Not** in
M3c: SMP, dynamic service discovery, endpoint restart-in-place, more than two
processes, a general supervisor, or any stable ABI promise.

### D2. The endpoint/channel object and its storage

A **channel** is one kernel record holding **both ends**:

```
channel { generation; end[2] { state; refs; queue[JE_EP_QUEUE_DEPTH]; } }
```

- A handle's bound object pointer is `&channel->end[i]`; the **peer** is
  `end[i ^ 1]`, in the **same record**, so a peer pointer can never dangle.
- Channels live in a fixed **kernel pool** (`JE_EP_POOL_CAP` records), not inline
  in `je_vm_process` and not the general heap — a bounded pool makes exhaustion a
  defined `NOSLOTS`-style failure and keeps accounting simple.
- An end is in exactly one logical state: `OPEN`, `PEER_CLOSED` (the other end is
  gone), or `CLOSED`.
- **Close semantics with multiple owning refs (frozen):** `endpoint_close` drops
  ONE handle's owning ref. The end becomes `CLOSED` and signals the peer only when
  its **handle-ref count reaches 0** (no duplicate/transferred handle to it
  remains); queued-capability and waiter refs keep the *channel record* alive for
  reclamation but do not keep a fully-unhandled end `OPEN`. On the last handle
  closing an end: the peer end transitions `OPEN → PEER_CLOSED`; the closing end's
  **inbound** queue is discarded (its messages can never be read), and any
  **capabilities** in that discarded queue are released (D3). A peer doing `recv`
  on a `PEER_CLOSED` end **drains remaining queued messages first, then reports
  EOF**; a `send` to a `PEER_CLOSED` end fails with `PEER_CLOSED`. The channel
  *record* is freed only when D3's full condition holds.
- **Non-exclusive receive (stated so nothing claims point-to-point):** because
  transfer is mint/copy (D8), two processes may hold handles to the *same* end and
  both `recv` from it. M3c does not promise exclusive receive; a message is
  delivered to whichever holder dequeues it first.

### D3. Reference counting, ownership, and destruction

Each end carries **two** counts, frozen as separate fields so a later PR never has
to split a single one (which would be a tested-code change):

- **`handle_refs`** — LIVE handle slots naming the end (acquired at mint incl.
  `dup`/transfer, released at close). Drives the end's `OPEN`/`CLOSED` state (D2).
- **`inflight_refs`** — non-handle owning references: queued capabilities (an end
  transferred into a queue) and registered waiters. Keep the channel *record*
  alive for reclamation but do **not** keep the end `OPEN`.

**Pool-record ownership is separate from these counts** (resolving the earlier
circularity): the pool slot is allocated at `channel` creation and **freed when
BOTH ends have `handle_refs == 0` AND `inflight_refs == 0`** — i.e. no handle, no
queued cap, no waiter references either end. The record is not itself a counted
"reference"; it is the thing being reference-counted.

The owning references are enumerated and frozen; nothing else may hold an end (or
a waiter's target) across a syscall boundary:

1. a **LIVE handle slot** → `handle_refs` (mint/close);
2. a **queued capability** → `inflight_refs` (transfer PR; data-only until then);
3. a **waiter** → `inflight_refs` on the end, AND the waiter record itself holds a
   reference to the **blocked thread/process** that MUST be unlinked/tombstoned by
   process death before that thread's storage is reused (see D7) — a waiter→thread
   pointer is NOT protected by the endpoint ref alone, so D-OWNER-1's
   "every alias is owning" is made true only by this explicit unlink rule.

`resolve_slot()` stays a **borrowed** lookup for non-blocking ops. **No borrowed
end pointer may be held across anything that can reschedule or call
`je_vm_destroy`** (a `copy_*_user` fault is always fixed up and never routed to
fault-death or the scheduler — frozen here, relying on the M3a exact-RIP fixup).
Any op that makes an end outlive the syscall (queue a message/cap, register a
waiter, mint a handle via `dup`/transfer) MUST **acquire its owning ref before it
commits** and **release it on every failure path** (ADR 0008 D8). A woken thread
uses the **waiter's owned ref**, never a re-resolve of the original handle.
Because an end with a LIVE handle always holds `handle_refs ≥ 1`, an object a
process can still name cannot be destroyed — handles cannot dangle by construction.

### D4. Reference bound and overflow (testable, policy not hardware)

Acquiring an owning reference past a **configured** `JE_EP_REF_MAX` fails with
`JE_ABI_H_REFOVERFLOW`, **acquires nothing, and leaves the object unchanged** (it
does NOT saturate-and-continue — a stuck-at-max count would silently leak). This
is a deliberately small policy bound (not a `uint32` limit, which 2 processes
could never reach) so the overflow path is reachable in tests, and is documented
as policy, never a hardware maximum. `JE_EP_REF_MAX` MUST be ≥ the largest count
of simultaneous *non-handle* refs the design can force (so a legitimate waiter or
queued-cap acquire cannot spuriously overflow); the lifecycle PR pins the exact
value with a `_Static_assert` tying it to `JE_EP_QUEUE_DEPTH` + max waiters.

### D4a. Endpoint rights (frozen; distinct from SELF's INSPECT)

Endpoints need their own rights so attenuation (D8) and the per-op checks mean
something. Frozen as new bits in the rights ABI (appended to `JE_ABI_RIGHTS_KNOWN`
by the lifecycle PR, pinned by `_Static_assert`): **`SEND`**, **`RECV`**,
**`TRANSFER`** (may grant the end to another process), and reuse of **`INSPECT`**
(may read metadata/rights). `endpoint_create` returns the creator a handle with
**all four**. `try_send` requires `SEND`, `try_recv` requires `RECV`, transfer
requires `TRANSFER` on the source. `handle_dup`/transfer may only ever produce an
**equal-or-weaker** subset (ADR 0008), so e.g. a `RECV`-only end can be handed out
without granting `SEND` or `TRANSFER`. `close` requires **no** right (as for SELF,
so dropping all rights never leaks a slot).

### D5. Accounting (preserve-accounting acceptance)

A transferred end outlives its creator, so charging it to the creator's quota
would leak the charge when the creator dies. Therefore:

- a **pool-global** bound (`JE_EP_POOL_CAP`) caps total live channels;
- a **per-process creation quota** caps how many a process may create;
- the charge is **released when the channel record is destroyed**, regardless of
  which process holds the ends at that point.

`try_send` with a full queue → a defined `QUEUE_FULL`; pool exhaustion on create →
a defined `NOSLOTS`-style status; neither mutates state.

### D6. Message copy ordering (cheap rollback)

Messages are **fixed-max-size, copied, data-only** (no capabilities until the
transfer PR). The copy order makes rollback free and is frozen:

- **Send:** `copy_from_user` into a **kernel staging buffer**, THEN enqueue. A
  fault during the copy mutates no queue.
- **Recv:** peek, `copy_to_user`, THEN dequeue. A fault leaves the message queued.
- **Short buffer:** return the required length in RDX and do **not** dequeue
  (truncation would be silent data loss) — a defined `SHORT_BUFFER` status.
- **Cross-address-space:** there is **no** direct copy between address spaces. Send
  copies into the kernel queue in the sender's context; recv copies out in the
  receiver's own context. This resolves user-buffer validity without pinning
  pages. A blocked receiver's buffer is **re-validated at copy time after wake**
  (not merely prevalidated before blocking); a copy fault there is recovered by the
  M3a exact-RIP fixup and surfaced as **`JE_ABI_INVALID`** (the existing
  recovered-copy-fault status — there is no `EFAULT` in this ABI), leaving the
  message queued (no dequeue on a faulting copy-out).

### D7. Blocking / wait state machine (single terminal result)

Non-blocking `try_send`/`try_recv` come first (D-SEQ). Blocking is a later PR and
adds a bounded **waiter** with exactly one terminal transition producing the
syscall result:

```
WAITING -> WOKEN | TIMED_OUT | CANCELLED | PEER_CLOSED
```

Frozen for the waits PR:

- **Deadline mandatory, bounded.** No unbounded block; the deadline is in timer
  ticks (the existing M2 clock), and the **timer ISR performs the waiter's
  terminal transition itself** (it must do the `WAITING→TIMED_OUT` transition and
  deliver the result, not merely mark the thread READY — `sched.c` already wakes
  SLEEPING threads from the ISR).
- **Tie-break / linearization (one result, never two).** If more than one trigger
  is eligible at once, priority is fixed: **`PEER_CLOSED` > `WOKEN` > `CANCELLED`
  > `TIMED_OUT`**, and if a message is already queued when a receiver would block,
  it does **not** block (immediate `WOKEN`-equivalent delivery). The transition is
  a single compare-and-set on the waiter state under IF=0, so exactly one path
  wins and the losers are no-ops.
- **Receiver-only blocking in v1.** `recv` may block; **`send` does NOT block** in
  M3c (a full queue returns `QUEUE_FULL` immediately), so there is no blocked-
  sender staging-buffer lifetime to manage. (Blocking send is a later milestone.)
- **Process death unlinks waiters (D3.3).** Destroying a process must remove or
  tombstone every waiter it owns *before* the thread/frame storage is reused, so a
  later wake/timeout cannot write stale memory. This, plus registering a waiter
  edits the scheduler, means the **waits PR touches `sched.c` (tested code) and is
  therefore OWNER-GATED**, like the death-path PR (D-OWNER-2) — it is an isolated,
  owner-signed PR, not bundled.

IF=0 makes the check/enqueue/block sequence atomic on this CPU, but the ADR still
requires the timeout-vs-wake-vs-cancel linearization above to be specified and
tested; a blocked thread enters a new `THREAD_BLOCKED_IPC` scheduler state.

### D8. Transfer (later PR; frozen here even though deferred)

Capability transfer grants an endpoint handle to the **other** process. It is a
**destination-side mint**, never a raw handle copy (ADR 0008's security model),
and it is **transactional**: reserve the destination slot → validate+attenuate
rights → **acquire the object reference before the destination commit** (D8 of
ADR 0008) → commit; **roll back every reservation/reference on any failure**
(destination table full, target not live, rights superset, refoverflow). Whether
transfer is a **move** (consumes the source handle) or a **mint/copy** (source
keeps a handle) is frozen as: **mint/copy with independent attenuation** — the
sender keeps its (possibly stronger) handle; the receiver gets a fresh,
independently-closable, equal-or-weaker handle to the same end. Messages stay
**data-only until this PR lands**, because a capability in a queue is a new owning
reference (D3.2) and a capability graph needs its cycle invariant (D9) frozen
first.

### D9. Capability-graph cycle safety (frozen before any cap may be queued)

A transferred end sitting in another end's queue is an owning reference, so
unmanaged queued capabilities could form a reference cycle that never reaches
`refs == 0` (a leak, not a UAF, given D3's owning-ref rule — but still a defined
failure to prevent). The transfer PR MUST ship a stated graph invariant that
bounds nesting; candidate invariants were debated but **none is adopted yet** —
the transfer PR owns proving one. Until then (D8), messages are data-only, and
that is **enforced, not just prose**: the message wire format carries a `cap_count`
field (and reserved flag bits), and the non-blocking-IPC PR (D-SEQ step 4) rejects
any send with `cap_count != 0` or nonzero reserved bits as `JE_ABI_INVALID`. So a
capability can never be queued before D9's invariant and the transfer transaction
(D8) exist, and no cycle can form before its invariant is frozen.

### D-SEQ. PR sequence (each small, independently testable, mergeable)

1. **ADR 0009** (this document; owner-accepts D-OWNER-1/2).
2. **Lifecycle (kernel-only create):** the endpoint pool, channel record, the
   **split refcount** (D3: handle-refs vs in-flight-refs), retain/release,
   `endpoint_close`, `_REFOVERFLOW`, and `handle_dup` gaining
   **retain-before-commit** for the endpoint type (the M3b dup path was kept
   SELF-only precisely so this PR adds the destroyable-object retain). Endpoints
   are created ONLY by in-kernel direct tests here — **no ring-3 `endpoint_create`
   yet** (D-OWNER-2 safety). Tests: explicit-close, direct pool/refcount/
   destruction-on-last-ref, `_REFOVERFLOW` at `JE_EP_REF_MAX`.
3. **Process-death release walk + ring-3 `endpoint_create` (owner-signed):** wire
   `release_all` into `je_vm_destroy` (the owner-gated death path) AND expose the
   `endpoint_create` syscall. These ship **together** so a user process can create
   an endpoint only once its death reclaims it. QEMU evidence: fault a holder,
   assert the peer is closed and pool usage returns to baseline; repeated
   fault/exit cycles do not exhaust the pool.
4. **Non-blocking data IPC:** `try_send`/`try_recv`, data-only (D6), with the
   enforced `cap_count == 0` / reserved-flags rule (D9) so no capability can be
   queued before its invariant exists.
5. **Capability transfer:** transactional attenuated mint, rollback-tested, and
   the D9 cycle invariant (this PR owns proving one). Flips messages from
   data-only to capability-carrying.
6. **Waits:** `THREAD_BLOCKED_IPC` + deadline + cancellation (D7). Edits `sched.c`
   (tested code) — **owner-gated**, like the death-path PR.
7. **Bootstrap fixture:** kernel injects an endpoint pair into the two processes,
   bounded ping-pong + closure, honest "fixture not supervisor."

(Codex's ordering: transfer before waits, so the lifetime/cancellation state space
isn't multiplied before transfer's semantics are frozen. Note the two owner-gated,
tested-path PRs — step 3 death path and step 6 `sched.c` block state — are each
isolated, per the D-OWNER-2 rationale.)

## D-OWNER-1. Incarnation / restart (owner decision; recommended resolution)

ADR 0008 D-RESOLVED deferred the object-wide "service-instance vs slot-generation"
question to M3c. **Resolution for M3c: terminal destruction, no restart-in-place.**
An endpoint, once destroyed, is gone; a "new instance" is always a new pool record
(the pool slot carries a `generation` ONLY as internal double-free/stale-index
defence — it is never exposed to ring 3, since handles bind `&end[i]` pointers, not
pool indices; if it ends up with no internal consumer the lifecycle PR drops it
rather than ship dead surface). **No shipped incarnation counter** — untested ABI
with no consumer.

**This decision is explicitly CONDITIONAL on D3 (review finding):** it is sound
*only because* every live alias to an end is an owning reference (handle, queued
cap, or waiter) and **no escaped raw pointer to an end may survive its release**
(the borrowed-pointer rule in D3). Terminal destruction + mint-fresh destination
handles (D8) then leaves no cross-process stale *handle* possible. If a later
milestone introduces a non-owning alias or restart-in-place, the incarnation
question reopens with its own ADR. Given D3 as frozen, this closes the ADR 0008
incarnation question for M3c.

## D-OWNER-2. Process-death release walk is its own owner-signed PR (the split)

Reclaiming a dying process's endpoints (a `release_all` that drops each of its
LIVE endpoint handles' refs, closing peers and freeing now-unreferenced channels)
has to run on process teardown — which lives in `je_vm_destroy` (`kernel/core/
vm.c`), the path the tested fault-death flow uses. Changing its resource-release
behaviour is a **death-path semantic change** and therefore owner-gated (ADR 0008
D7), **even if `sched.c` stays byte-identical**.

**Decision (owner-approved 2026-10-18): the release walk is its OWN separate,
owner-signed PR — it is NOT bundled into the lifecycle PR (D-SEQ step 2).** The
lifecycle PR ships the endpoint pool + refcount + `endpoint_close` + direct pool
tests, but — crucially — **does NOT expose `endpoint_create` to ring 3 until the
teardown PR lands** (see the safety note below). Endpoints in the lifecycle PR are
created only by in-kernel direct tests, never by a user process that could then
die holding one.

**SAFETY (review finding — the gap is not merely cosmetic).** `je_vm_destroy`
currently zeroes the handle table. With endpoints but no release walk, a process
dying with a LIVE endpoint handle would leave its `refs` and creation charge
**permanently unreclaimed**, and repeated fault/exit cycles would **exhaust the
fixed pool** — a recoverability/availability failure, not a bounded cosmetic leak.
Therefore the gap window must expose **no user path that creates an endpoint**:
the lifecycle PR keeps `endpoint_create` kernel-only (direct tests), and the
ring-3 `endpoint_create` syscall is introduced **in or after** the owner-signed
release-walk PR, so a user process can never die holding an endpoint before
teardown reclaims it. This makes the split safe, not just honest.

**Why the split is the right fit here (recorded per owner request):**

- **It keeps an owner-gated, previously-tested-path change isolated and legible.**
  Every tested-path edit in this project (the M3a H1 fault-death fix, the #4
  death records) has been its own reviewable change with its own QEMU evidence;
  bundling the release walk into a feature PR would hide a death-path semantic
  change inside unrelated new surface, exactly what the D7 gate exists to prevent.
- **The two changes have genuinely different risk and evidence.** Lifecycle is
  pure-ish pool/refcount logic providable by hosted + explicit-close tests; the
  release walk is a death-path change whose proof is a *QEMU fault-death* run
  (fault a holder, assert the peer is closed and pool usage returns to baseline).
  Different blast radius → different PR, so a reviewer can reason about each in
  isolation.
- **It avoids a false "done" while keeping the gap honest.** The lifecycle PR
  states "process-death cleanup not yet wired," so nothing overclaims; the gap is
  closed deliberately in the signed PR rather than assumed away.

**Why NOT bundle (the alternative considered and rejected):** the counter-argument
(Claude's) was that an unwired `release_all` helper is dead code whose only real
test is in situ, so wire it and prove it in the lifecycle PR as one owner ask. That
is sound engineering in the abstract, but here it **widens an owner-gated PR to
also carry a feature**, making the death-path change harder to review in isolation
and coupling its merge to the lifecycle feature's. The helper is not left dangling:
it lands *with* its wiring and its QEMU evidence in the very next PR, so it is never
merged as untested dead code — it is merely deferred by one small PR to keep the
gate clean. On balance the isolation of the owner-gated change outweighed the
one-PR-later cost.

## Consequences

- A later milestone needing SMP-safe endpoints, restart/supervision, >2 processes,
  or a queued-capability graph beyond D9's invariant writes a new ADR.
- The bounded pool / queue / ref / message-size values (`JE_EP_POOL_CAP`,
  `JE_EP_QUEUE_DEPTH`, `JE_EP_REF_MAX`, the message max) are frozen with concrete
  numbers in the lifecycle PR and pinned by `_Static_assert`; this ADR fixes that
  they are small, configured policy bounds, not hardware limits.
- New `JE_ABI_H_*`/IPC statuses (`QUEUE_FULL`, `SHORT_BUFFER`, `PEER_CLOSED`,
  `REFOVERFLOW`, the wait terminals) are appended to the v0 ABI namespace and
  pinned, keeping `abi.h` independent of `status.h`/`handle.h` (the glue maps).

AI-assisted design (a two-round role-assigned peer debate: implementor vs
adversarial reviewer); not independent human qualification. The owner is the merge
gate and the decider of D-OWNER-1 and D-OWNER-2 (both resolved above with owner
approval on 2026-10-18); on acceptance, D-SEQ step 2 (lifecycle) may begin.

## AMENDMENT (2026-10-18, owner-approved) — step-3 wiring decisions

Two decisions were needed to implement D-SEQ step 3 (the owner-signed process-death
release walk + typed close dispatcher + ring-3 `endpoint_create`). Both were raised
by an implementor-vs-adversarial-reviewer debate on the *wiring* and approved by the
owner; they refine, not contradict, the frozen contract above.

### A3-1. `je_vm_destroy` runs the release walk + unwind + table-zero as ONE
### IRQ-disabled critical section (owner-gated death-path behaviour change)

The release walk is invoked through a **generic teardown hook** on `je_vm`
(`void (*teardown)(void *ctx, je_handle_table *t)` + `void *teardown_ctx`,
zero-defaulted by `je_vm_init`), called inside `je_vm_destroy` **after** the
`!vm`/`state != READY` checks and **immediately before** `unwind()`. This is the
single choke point covering the reap path (`sched.c`) and every direct `main.c`
caller, and it keeps `vm.c` endpoint-agnostic (the hook is NULL in the vm unit
test, which gains a fake-hook ordering case). The pool is one kernel-owned static
in a new `endpoint_kern.c`; `je_vm` never holds an endpoint-typed pointer.

The debate found a **real tested-path hazard** in the naive "walk then unwind at
IF=0" plan: the direct `je_vm_destroy` callers in `main.c` can run with **IF=1**
(the bootstrap thread after `sti`), so a timer could preempt **between the walk
and the `*p = {0}` table-zeroing** while the peer process is live — the peer's
`HANDLE_CLOSE` would then mutate the same channel record mid-teardown (the exact
use-after-free this PR exists to prevent). **Resolution:** `je_vm_destroy`
**saves-and-disables IRQs around the entire hook+unwind+zero sequence and restores
after** — it no longer merely trusts the caller's IF=0. `vm.c` reaches the arch
IRQ-save through the same indirection pattern (a hook/stub, NULL/no-op in the unit
test) so the fake backend stays arch-free. This changes behaviour in the
owner-gated death path and is therefore owner-signed.

### A3-2. `endpoint_create` (op 10) returns BOTH handles packed in RDX

`endpoint_create` takes no handle args (all of RDI/RSI/RDX/R10/R8/R9 must be zero,
else INVALID), mints both ends into the **caller's own** table with
`JE_EP_RIGHTS_ALL`, and is transactional (both slots + both refs + the pool record
all-or-nothing; a one-slot/full-table failure leaves the pool and the caller table
unchanged). The v0 ABI gives one status (RAX) and one scalar (RDX); two 32-bit
handles are returned by **packing them into RDX**: `RDX[31:0] = h0`,
`RDX[63:32] = h1`. RDX is **undefined on failure**. This is lossless only because a
handle is 32 bits, so `abi.h` pins `_Static_assert(sizeof(je_handle) == 4)` (and
the ABI endpoint object-type / rights bits are pinned equal to the table-side
copies). Rejected alternatives: a second "claim-peer" op (leaves a half-claimed
state) and `copy_to_user` of the pair (adds a fault/partial-write rollback for no
gain). The packed shape **freezes "a handle is ≤32 bits" into the v0 ABI**; a later
milestone that needs wider handles or >2 returned capabilities writes a new ADR.

`endpoint_create` returns `JE_ABI_UNSUPPORTED` on a VM with no teardown hook
installed, so "an endpoint slot exists in a ring-3 table ⇒ that VM's `je_vm_destroy`
walks it" holds **by construction**, not by convention — the structural discharge
of endpoint.h's load-bearing precondition. Pool exhaustion on create maps to the
existing `JE_ABI_H_NOSLOTS` (no new status).

### A3-3. Typed close dispatch is an exact read-only type peek, not a fallthrough

Ring-3 `HANDLE_CLOSE` (op 6) must route an ENDPOINT handle through
`je_endpoint_handle_close` (which drops the ref) and a SELF handle through raw
`je_handle_close`. The glue does a **read-only type peek then an exact branch**
(SELF → raw close; ENDPOINT → endpoint close; any other/unknown/corrupt type →
fail closed), rather than a "try SELF, else endpoint" fallthrough (which would
obscure BADINDEX/STALE/WRONGTYPE precedence and silently admit a future type). The
peek+close are atomic because the `int 0x80` path runs at IF=0 on one CPU — stated
as an explicit, SMP-revisit assumption, not a general proof. The table API does not
grow a typed-close; the walk and the dispatcher use one new **read-only** iterator
(`je_handle_live_at`) so endpoint code never writes table internals (table purity
preserved).

Scope honesty: because `endpoint_create` puts both ends in the caller's table, the
ring-3 fault-death proof shows **no-survivor reclaim + pool-returns-to-baseline +
repeated-fault-no-exhaustion**; the *survivor observes PEER_CLOSED* case is proven
in a **kernel-only self-test** (two tables via `je_endpoint_channel_create`), since
a cross-process surviving peer needs transfer (D-SEQ step 5). The ring-3 proof is
labelled reclaim-only and does not claim the survivor case.

## AMENDMENT (2026-10-19, owner-approved) — step-5 capability-transfer decisions

D-SEQ step 5 (capability transfer) needs three decisions the frozen D8/D9 left
open or under-specified. All three were raised by an implementor-vs-adversarial
peer debate and decided by the owner. Recorded per the ADR 0000 decision-record
policy (decision + why / predictions if other choices / flex options).

Capability transfer, concretely: a `try_send` may carry 0..K **capabilities** —
endpoint handles from the sender's table — alongside the byte payload. Each cap
is enqueued in the peer's inbound as a **descriptor** `{rec_index, side,
generation, rights}` (never a raw handle; handles are per-table), which **retains
an inflight reference** on the referenced end (D3.2). The receiver's `try_recv`
delivers the bytes AND **mints fresh, independently-closable, equal-or-weaker
handles** in the receiver's table (destination-side mint, D8), releasing the
inflight refs. The sender keeps its handle (mint/copy, not move). A cap may only
be sent if the handle holds the `TRANSFER` right and the requested rights are a
subset (else DENIED).

### A5-1. D9 cycle invariant = bounded RECLAMATION (cycles allowed; released on last-handle removal)

**Decision + why.** D9 is stated as: *queued-cap edges MAY form cycles; every
edge is owned by whichever end's inbound holds the cap; the edge's inflight ref is
released when that end's **final handle_ref is removed** (its inbound is then
discarded, per D2) — NOT merely when one owner process dies.* (D2 lets two
processes hold the same end, so a process dying removes only *its* handle_refs; the
inbound is discarded only when the **last** holder across all processes closes or
dies. A still-live co-holder is a legitimate reference, not a leak.) No new
nesting/ordering restriction is imposed. This follows directly from the
already-frozen D2/D3 rule ("a closing end's inbound is discarded on last handle
close, releasing any capabilities in it"): the peer debate proved that rule alone
makes every queued-cap cycle **reclaimable** — an A↔B cycle is released once each
end's last handle is gone, and the death release-walk removes a dead process's
contribution to that count. The alternative invariants (a per-end depth-1
bipartite rule, or an index-ordering acyclic rule) were considered and **rejected
as unnecessary** given D2, and as costly (they forbid legitimate forwarding of a
received end and add per-end graph counters that need their own proofs). The
honest scope limit, stated plainly: this guarantees **reclamation when an end's
last handle is removed**, **not** that every queued cap is eventually *deliverable*
— a live process holding the last handle to a queue end can retain its queued caps
indefinitely (a legitimate reference); and A5-2 handles a cap whose *target* dies.
With `JE_EP_POOL_CAP = 4` and two processes, the blast radius of any retention is
bounded and recoverable, acceptable for M3c's single-machine/2-process scope.

**Predictions if other choices were made.**
- *Per-end depth-1 bipartite rule (Codex's I2 variant):* would make cycles
  structurally impossible rather than merely reclaimable, but would **reject a
  valid forward** (a process receiving an end whose inbound already carries caps
  could not re-send it), and would add two per-end counters + their invariants.
  Net: more code, a real usability loss, no reclamation benefit over A5-1 given
  D2. Reopen only if D2 is amended (see A5-2 prediction).
- *Index-ordering acyclic rule (I3):* cheapest to check and provably acyclic, but
  asymmetric/surprising (you may send end X→Y but not Y→X purely by pool index) —
  a confusing ABI contract for no gain here.
- *Pure I4 with no stated bound (first suggestion):* rejected — "bounds nothing"
  contradicts D9's "MUST bound nesting", and read naively it either mints dead
  caps or pins cycles forever. A5-1 is I4 *corrected* by grounding it in D2's
  discard rule and stating the reclamation bound explicitly.

**Flex options to consider in future.** **FLEX (planning review).** If a later
milestone wants true **move** semantics or wants every queued cap guaranteed
deliverable (not just reclaimable), that requires amending D2 so an inflight cap
keeps its target end OPEN — which *reintroduces real cycles* and then makes a
nesting restriction (the rejected depth-1 or index-ordering rule) **mandatory**.
That is a larger contract change deferred to an SMP/supervisor-era ADR. Flagged
for the pause review with the planning agent.

### A5-2. Stale queued cap = deliver a NULL marker (S1), siblings still mint

**Decision + why.** Because D2 lets a sender close the target end after queuing
the cap but before the receiver dequeues it, a queued descriptor can point at a
**since-CLOSED** end. Decision (**S1**): on `try_recv`, a cap whose target is no
longer live (end CLOSED, or — defence-in-depth — generation stale) is
**consumed** — the receiver gets **handle value `0`** (the by-construction invalid
handle, `handle.h`; NOT a status code — there is no `JE_ABI_H_INVALID` symbol) in
that cap's output slot, its inflight ref is released, and the message **dequeues
normally**; any live sibling caps in the same message still mint. A
**PEER_CLOSED** target is still LIVE and **does** mint (only CLOSED is dead). This
amends D8 on **two** points: its "successful receive delivers a fresh
equal-or-weaker handle" becomes "…or handle `0` for a since-dead cap", **and** its
rollback-case list is corrected — "target not live" moves OUT of the
roll-back-and-requeue set and INTO this consume-with-`0` set (the other rollback
cases — destination QUOTA/NOSLOTS, rights superset, REFOVERFLOW — remain
roll-back). The driving reason is a **stale-cap head-of-line wedge** the peer
debate found: a dead cap that instead rolled back and left the message queued (the
naive D6 "nothing delivered on failure" reading) would sit at the queue head
forever and make every later message unreachable — a permanent liveness bug. S1
removes **that specific permanent wedge** (it does NOT remove *transient*
head-of-line blocking: a short byte/handle buffer, destination QUOTA/NOSLOTS,
REFOVERFLOW, or a copy fault still intentionally leave the head message queued for
a retry, per D6 — those are recoverable by the receiver, the stale-cap case is
not). S1 matches how real capability systems treat a transferred cap to a
since-destroyed object (a dead/null marker, not a stuck channel). The
transient-vs-permanent split: transient → roll back fully, leave queued (D6
preserved); permanently dead target → consume with handle `0`. A copy-out fault is
surfaced as `JE_ABI_INVALID` (the recovered-copy-fault status — there is **no
`EFAULT`** in this ABI, per D6), and is transient (rolls back, message stays).

**Predictions if other choices were made.**
- *S2 — amend D2 so an inflight cap keeps the target end OPEN:* no dead caps ever,
  send-then-close fully safe, but **real cycles become reachable**, forcing a
  mandatory nesting restriction (A5-1's rejected rules) and changing the frozen
  close/PEER_CLOSED semantics. Biggest blast radius; deferred (see A5-1 flex).
- *S3 — forbid closing a transferred-target handle while a cap to it is queued:*
  sidesteps dead caps, but needs a per-end "is-queued-somewhere" predicate at
  close time, restricts a legitimate API action, and is surprising (close
  silently refused/deferred). More enforcement machinery than S1 for a narrower
  outcome.
- *Naive rollback-and-requeue (no S-choice):* the rejected baseline — the
  head-of-line wedge above. Not viable.

**Flex options to consider in future.** **FLEX (planning review).** S1's null
marker is the minimal honest contract; if the planning agent later prefers an
explicit *stale-cap status* surfaced per-slot (rather than overloading handle
value 0) or adopts S2's move semantics, that is a clean future amendment. The
per-slot output encoding (0 = dead) needs no extra ABI surface now.

### A5-3. `maybe_free_record` gains an `allocated` guard (tested-lifecycle touch)

**Decision + why.** The cap-release path can call `maybe_free_record` for the same
record more than once in one discard (the closing end settles, then a detached cap
pointing into the same record is released). The existing `maybe_free_record` has
no idempotence guard, so a second call on an already-freed record would
double-decrement `allocated_count` and double-bump the generation. Decision: add
`if (!rec->allocated) return;` at the top, plus an assertion that both inbound
queues are cap-free when it frees. The safe discard ordering is frozen too:
**detach all descriptors into a bounded local array → clear queue metadata →
settle the closing end → release each detached inflight ref (each may invoke the
now-guarded `maybe_free_record`); freeing never recurses into a queue still being
discarded.** This touches M3c-tested lifecycle code (release_handle /
maybe_free_record), so it is owner-gated (approved 2026-10-19).

**Predictions if other choices were made.**
- *No guard, rely on ordering alone:* works only while every future caller
  preserves the exact detach-before-release ordering — a fragile invariant a later
  edit could silently break into a double-free. The guard makes the function
  idempotent regardless of caller ordering; strictly safer for ~2 lines.
- *A separate "freed" flag instead of reusing `allocated`:* redundant — `allocated`
  already is that flag; a second would be one more thing to keep in sync.

**Flex options to consider in future.** Firm; a change needs a new ADR. (Pure
defense-in-depth, no contract surface to flex.)

### A5-4. Transfer ABI (the pre-reserved R8/R9 path) and the cross-process proof

**Decision + why.** Uses option (b), the path step-4 pre-reserved. **This A5-4
explicitly AMENDS the step-4 note** ("step 5 MUST add a new op or an out-register
extension, NOT overload `try_recv`'s RDX"): step-4 reserved R8/R9 and RDX[63:32]
**for exactly this extension**, so the chosen reading of "out-register extension"
is to extend `try_recv` via those reserved registers rather than add a duplicate
op. The step-4 "not overload RDX" caution is honoured in spirit — RDX[31:0] keeps
its byte-length meaning unchanged; RDX[63:32] takes its *pre-reserved* cap-count
meaning (it was reserved-zero, never a usable field, so no step-4 caller can
observe a change there). **send** reads caps via `R8` = user pointer to a
`u64[cap_count]` array, each element `{ low32 = handle, high32 = requested
rights }`, with `R10[7:0] = cap_count` (≤ K); unknown rights bits → INVALID;
`R8 == 0` legal iff `cap_count == 0`. **recv** returns minted handles via `R8` =
user `u32[]` output buffer, `R9` = its slot capacity; `RDX[31:0]` = byte length
(or required byte length on a short *byte* buffer), `RDX[63:32]` = received-cap
count — which **counts consumed slots including a handle-`0` dead-cap slot** (a
dead cap is delivered, so it occupies an output slot) — or the required slot count
on a short *handle* buffer → `SHORT_BUFFER`, no dequeue. When BOTH buffers are
short, `SHORT_BUFFER` is returned with RDX[31:0] = required bytes and RDX[63:32] =
required slots. **Behaviour-change note (frozen):** a step-4-style caller passing
`R8 == 0, R9 == 0` that receives a message carrying ≥1 cap now gets `SHORT_BUFFER`
(required slot count in RDX[63:32]) **even if its byte buffer would have fit** —
it must supply a handle buffer to drain that message; a naive
retry-without-a-handle-buffer loops. This is the price of delivering caps and is
documented so a data-only caller knows to size a handle buffer once caps are in
use.

**Send transaction (frozen, all-or-nothing).** The send side is transactional
per D3: (1) copy the whole `u64[cap_count]` cap array into kernel staging and
validate EVERY element first (each resolves to an endpoint in the sender's table
holding `TRANSFER`, requested rights ⊆ source rights, `rec(target) != ` the
destination queue's own end is NOT required under A5-1, but each must be a live
endpoint); (2) only then retain the inflight ref on each target in order; (3) on
ANY failure at any element — cap-array copy fault (`JE_ABI_INVALID`), not an
endpoint (BADINDEX/STALE/WRONGTYPE), missing TRANSFER or rights superset (DENIED),
or REFOVERFLOW — **unwind every inflight ref already retained for this batch** and
enqueue nothing (D6: nothing enqueued on failure). The byte payload and the caps
commit together or not at all. These new send-side statuses slot into the frozen
step-4 precedence AFTER the main-handle resolution: decode INVALID (incl. cap-array
shape) → main-handle BADINDEX/STALE/WRONGTYPE → main-handle DENIED (no SEND) →
PEER_CLOSED → per-cap DENIED (no TRANSFER / rights superset) → per-cap
REFOVERFLOW → QUEUE_FULL → cap-array copy-fault INVALID (last, after a clean
preflight).

**Recv transaction (frozen, all-or-nothing).** Mirroring send and honouring D6:
preflight (classify each queued cap live-vs-dead per A5-2, check byte and handle
capacity), then **stage all live mints in kernel memory** — retain the target
handle_ref (REFOVERFLOW) and allocate the destination slot (QUOTA/NOSLOTS) for
each live cap, with handle `0` staged for each dead cap — then copy the bytes out,
then copy the handle array out, and **only then** dequeue the message and release
the consumed caps' inflight refs. Any transient failure (short buffer, QUOTA,
NOSLOTS, REFOVERFLOW, or a copy-out fault surfaced as `JE_ABI_INVALID` — **not**
`EFAULT`, which does not exist in this ABI) rolls back every staged mint (no
minted handle left live) and leaves the message queued; user output buffers may
hold partial bytes on a copy-out fault (documented, D6). A dead-target cap is
consumed-with-`0` (A5-2), not a rollback.

**The cross-process proof** is a kernel-seeded channel (one end minted into
process A's table, one into B's via the existing `je_endpoint_channel_create`) —
explicitly a **test fixture, not a bootstrap ABI**. The fixture MUST install the
teardown hook on **both** VMs (`je_endpoint_kern_install`) before seeding, so
A5-1's "last handle / death discards" reclamation holds for the seeded ends too
(the raw `je_endpoint_channel_create` bypasses op-10's A3-2 hook check, so the
fixture asserts the hook explicitly). Over the seeded channel A transfers a
*second* channel's end to B; B mints it and uses it.

**Transfer sequencing (frozen, review-driven).** Under the frozen D2, closing the
LAST handle to a transferred end marks it CLOSED, making an in-flight queued cap
to it *dead* (A5-2). So the sender MUST retain its handle to the transferred end
until the receiver has minted: A sends the cap → **B peeks + mints** (B now
co-owns the end) → only THEN A closes its copy. "Send then immediately close, then
B receives" delivers a *dead* cap (handle 0) — the A5-2 path, not the
successful-transfer path.

**Proof status (recorded decision, 2026-10-19).** The step-5 PR proves the
transfer MECHANISM across two **distinct process tables** deterministically as a
**kernel self-test** (`M3c-s5`): seed S across two tables, mint C in table A,
transfer C.e1 as a cap over S via the real send/recv cap path, mint it into table
B, message A on C.e0 across the tables, reclaim S+C. A full **ring-3 two-process**
transfer (two scheduled user tasks cooperatively exchanging over the seeded
channel) is **deferred to step 7 (the bootstrap fixture)**, whose explicit subject
is multi-process choreography. *Decision + why:* the cross-table mechanism is
fully proven deterministically here; the cooperative-scheduling choreography is
step 7's job, not a reason to add a race-prone two-stub test to step 5 (a
spin-based receiver can exhaust its bounded poll before the sender runs on a
single CPU). *Prediction if forced into step 5:* a brittle timing-dependent test
that could flake in CI. *Flex (planning review):* step 7 revisits whether the
bootstrap fixture subsumes this or wants a cooperative-yield primitive first.

**Predictions if other choices were made.**
- *Option (a) — a new `try_recv_caps` op instead of extending `try_recv` via
  R8/R9:* a cleaner signature but a new op number and a near-duplicate recv path;
  step-4 already reserved R8/R9 + RDX[63:32] for (b), so (b) avoids op-space growth
  and keeps one recv path.
- *Caps in the byte buffer (inline wire header):* reintroduces the
  header-copy/parse + fault-rollback surface step-4's D6 analysis rejected; keeps
  data and caps entangled.
- *Self-loopback "proof":* would not prove cross-address-space transfer at all
  (one table), and is forbidden by the invariant anyway.

**Flex options to consider in future.** **FLEX (planning review).** The
`{handle, rights}` u64 send-element layout and the 0-means-dead recv encoding are
v0 choices; a future ABI revision (or the step-7 real bootstrap replacing the
seeded fixture) may restate them. K (caps per message) is a policy bound like the
others and may grow.
