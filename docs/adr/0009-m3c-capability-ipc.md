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

## AMENDMENT (2026-10-19, owner-approved) — step-6 blocking-recv (waits) decisions

This amendment finalizes the **D7** wait contract for the (owner-gated) waits PR
that edits `sched.c`. It composes with D7's already-frozen frame (state machine
`WAITING→WOKEN|TIMED_OUT|CANCELLED|PEER_CLOSED`, mandatory bounded deadline, timer
ISR performs the terminal transition, receiver-only blocking, process-death
unlinks waiters, re-validate the user buffer at copy time after wake). It does not
reopen those; it decides the six open questions the step-6 design pass raised.

**Attached correspondence (ADR 0000 part 4).** Design driven by an
implementor↔adversarial `peer-debate.sh` run (claude=implementor,
codex=adversarial-reviewer, 2 rounds), then an adversarial review of THIS
amendment's text (which caught ten contract-text defects — ABI mapping, the
single-CAS-vs-priority wording, the copy-fault re-arm, teardown resurrecting a
DYING thread, the zero-timeout short-circuit, caps composition — all fixed in the
text below before owner sign-off). Full transcript:
[`docs/adr/debates/m3c-step6-waits.md`](debates/m3c-step6-waits.md). A follow-up
**mechanism** debate (the park/resume x86 mechanism behind A6-CLARIFY-1/2) is at
[`docs/adr/debates/m3c-step6-mechanism.md`](debates/m3c-step6-mechanism.md). The
reviewer
sharpened the design materially (separate waiter vs scheduler state machines, one
arbitration CAS, no copy-out from the scheduler) and surfaced the A6-1 contract
conflict below as a genuine owner escalation, not a clarification.

### A6-STRUCT. Two separate state machines + one arbitration CAS

**Decision + why.** The **waiter** state (`WAITING→WOKEN|TIMED_OUT|CANCELLED|
PEER_CLOSED`) is kept **distinct** from the **scheduler** state
(`THREAD_BLOCKED_IPC → READY`); they are NOT collapsed into one enum. A single
pure helper **`je_ipc_wait_arbitrate(waiter, cause)`** performs the terminal
transition as **one compare-and-set** under IF=0: the first eligible cause wins,
later causes are no-ops. Every path that creates eligibility calls it in the SAME
IF=0 critical section as the endpoint/clock mutation that caused it — sender
enqueue → `WOKEN`, peer close → `PEER_CLOSED`, timer ISR deadline → `TIMED_OUT`,
internal cancel → `CANCELLED`. The parked **syscall continuation** then *validates
the already-selected result and performs the owned-record copy-out + commit*; it
is **not** a second authority and never rewrites the CAS result. (Rationale: the
adversarial review showed "ISR stamps TIMED_OUT, syscall re-checks and may upgrade
to WOKEN" would make the syscall a second authority — not the single-terminal-CAS
D7 requires. Making enqueue/close do their own CAS in their own critical section
is what keeps it single.)

**Single-CPU linearization (replaces the literal "priority" reading).** On one CPU
with IF=0, the four causes NEVER become eligible simultaneously — each runs its CAS
in its own critical section, so they are strictly ordered in time. **The first
critical section to fire the CAS wins; every later one is a no-op** (the waiter is
already terminal). D7's written priority `PEER_CLOSED > WOKEN > CANCELLED >
TIMED_OUT` therefore does NOT mean "a later higher-priority cause supersedes an
earlier terminal state" (that would be a second transition, forbidden); it means
**the recv entry re-check evaluates the currently-eligible causes in ONE snapshot
before parking** — at entry only WOKEN (a queued/deliverable message) and
PEER_CLOSED (empty + peer gone) can be jointly relevant, and A6-1 drain-first
decides between them: a non-empty queue picks WOKEN and the thread never parks,
an empty closed end picks PEER_CLOSED. Once parked, there is only one snapshot per CAS and
first-wins holds. (This is a D7 *clarification*, flagged for the owner: the frozen
"ISR delivers the result" is reconciled as "the ISR's CAS stamp **is** the result;
the continuation only transcribes the stamped cause into the user frame," see
A6-5.)

**`je_ipc_wait_arbitrate(waiter, cause)` returns a won/lost flag.** The caller uses
it: a sender's enqueue path learns whether it won the CAS (and therefore owns the
head reservation it must install); teardown learns whether it still owes a
reservation/reference release. A "lost" return means the waiter was already
terminal — the caller performs no wake and installs no reservation. The helper
performs NO re-arm: a terminal waiter stays terminal (see A6-5 for the copy-fault
path, which releases the reservation but does NOT re-arm the same waiter).

**Predictions if other choices were made.** *Waiter state IN the scheduler enum
(my first lean):* conflates "why the block ended" with "is it runnable," and makes
every scheduler state check also reason about IPC causes — more tested-code
coupling, the opposite of the owner gate's intent. *Syscall re-check as the
authority:* two authorities for one terminal state → the exact double-transition
race D7 forbids; a timeout and a late enqueue could both "win." *Waiter registry
in endpoint.c instead of the thread:* thinner `sched.c` but endpoint code reaches
into scheduler internals to wake — worse coupling across the tested boundary.

**Flex options to consider in future.** **FLEX (planning review).** Blocking
*send* (deferred in v1) will add a sender-side waiter + a staging-buffer lifetime;
the arbitrate helper is meant to generalize to it. If a third blocking op appears,
revisit whether the waiter fields belong in the thread struct or a side table.

### A6-1. Queued-message-plus-peer-close → DRAIN-FIRST wins (A1)

**Decision + why.** When a message is queued **and** the peer has closed, the
queued message is **delivered** (`WOKEN`); `PEER_CLOSED` is returned only when the
queue is **empty**. The literal D7 priority `PEER_CLOSED > WOKEN` is read as
ordering **block-ending triggers**, NOT as overriding already-queued data. This
was a genuine frozen-contract conflict (owner-escalated, not a clarification): the
**merged step-4 `try_recv` is already drain-first** (CLOSED is "dead" only on an
empty queue), so A1 makes blocking `recv` behave **identically** to non-blocking
`try_recv` on the same endpoint state — "closing a peer never eats bytes already
delivered to you." **Owner-approved A1.**

**Predictions if other choices were made.** *A2 (literal PEER_CLOSED > a queued
message):* blocking `recv` and non-blocking `try_recv` would return *different*
results on the identical state (one drains, one reports closed) — a surprising
split a caller could not predict from the op alone, and a divergence from already-
tested step-4 behavior that would need its own justification. It would also strand
a legitimately-sent final message on peer close.

**Flex options to consider in future.** Firm for M3c. If a future "urgent close"
semantic is ever wanted (peer close *preempts* pending data), that is a new op or
an explicit flag, not a reinterpretation of this priority — a new ADR.

### A6-ABI. Op, timeout register, and terminal-cause → status mapping

**Decision + why.** Blocking recv **reuses the merged `try_recv` op**
(`JE_SYS_TRY_RECV`=12), with the **timeout (relative duration in ticks) in R10**
(reserved=0 today). Backward-compatible: every existing non-blocking caller passes
**R10=0**, and **zero timeout = the exact current non-blocking semantics** — check
once, never park, and on an OPEN end with nothing deliverable return **`EMPTY`=13**,
identical to today. A nonzero R10 opts into blocking. The four terminal causes (plus the post-WOKEN fault row) map to the
user frame as:

| Terminal cause | status (AL/RAX) | RDX / R8 / R9 |
|---|---|---|
| `WOKEN` | `JE_ABI_OK`=0 | merged step-4/step-5 recv success layout: RDX[31:0]=byte len, RDX[63:32]=cap count; R8/R9 handle-out per A5-4 |
| `TIMED_OUT` | **`JE_ABI_H_TIMED_OUT`=14** (new, `_Static_assert`) | RDX=0, no handles |
| `PEER_CLOSED` | `JE_ABI_H_PEER_CLOSED`=12 (existing) | RDX=0, no handles |
| `CANCELLED` | **`JE_ABI_H_CANCELLED`=15** (new, `_Static_assert`; ABI-unreachable in v1 per A6-2, but the continuation still writes a defined status) | RDX=0, no handles |
| copy/mint fault after WOKEN | `JE_ABI_INVALID`=2 (existing) | message stays queued (A6-5) |

Two new codes (14, 15) extend the 10–13 block; no existing code changes meaning.
**Caps compose:** a blocking recv is a full recv — on `WOKEN` it runs the **same
step-5 staged-mint path** (R8/R9 handle-out, dead-cap→handle 0, mint-failure
rollback → `JE_ABI_INVALID` with the message left queued). A zero-timeout (poll)
recv that finds a cap-bearing message behaves exactly as merged `try_recv`
(SHORT_BUFFER if the handle buffer is too small). **Owner-approved** (reuse-op +
zero=EMPTY was the design-pass recommendation; the two new codes follow from
pinning the mapping).

**Entry snapshot goes through the plain `try_recv` path — no waiter registered when
a result is available at entry.** Both the zero-timeout poll AND the first check of
a nonzero-timeout recv evaluate the end **once via the existing merged `try_recv`
logic** before any waiter/`wake`/reservation exists. If that check yields ANY
immediate result — `WOKEN`-equiv delivery, **`SHORT_BUFFER`** (buffer or handle-out
too small), or `PEER_CLOSED` — it is returned straight away and **no waiter is
registered, no head reserved, the A6-4 occupied-slot check is not consulted**. A
waiter is registered (and A6-4 applies) **only** when the entry check would
otherwise return `EMPTY` on an OPEN end *and* the timeout is nonzero. Consequently a
too-small buffer is **`SHORT_BUFFER` whether polling or blocking** (identical to
merged `try_recv`, satisfying A6-1's "behave identically"), and is **never** swept
into the post-WOKEN `JE_ABI_INVALID` fault bucket. The post-WOKEN `JE_ABI_INVALID`
path (A6-5) is strictly for a **copy/mint fault on a message that was already
reserved** (e.g. the user buffer became unmapped between park and resume), not for
a known-short buffer — a short buffer is detected at the entry/peek check before
any reservation, leaves the message queued, and reserves nothing.

**Predictions if other choices were made.** *A new `JE_SYS_RECV` op:* a
near-duplicate recv path + op-space growth for no behavioral gain (R10 was already
reserved=0 for this). *Zero-timeout = immediate TIMED_OUT:* every existing R10=0
`try_recv` caller flips `EMPTY`→`TIMED_OUT` — a silent breaking change to merged,
tested behavior; rejected. *Leaving TIMED_OUT/CANCELLED unmapped (the text defect
the review caught):* the continuation would write an undefined status and a caller
could not tell a timeout from a corrupt result — the mapping is load-bearing.

**Flex options to consider in future.** **FLEX (planning review).** If blocking
*send* is added it will want a timeout; whether it reuses R10 on `try_send` by the
same rule is a future call. Code values 14/15 are fixed once asserted.

### A6-2. CANCELLED is internal-only in v1 (B1)

**Decision + why.** The `CANCELLED` terminal state is reachable in v1 **only via
an internal kernel helper** (`je_ipc_wait_cancel(waiter)`), exercised by a
deterministic kernel test; there is **no ABI path** to it yet (no signal / kill /
async-cancel mechanism exists in M3c). It is implemented and tested now so the
arbitration CAS and all four terminal causes are proven (first-critical-section-
wins, losers no-ops), and so a
future signal mechanism wires to a ready terminal state. Documented as
**ABI-unreachable in v1**. **Owner-approved B1.**

**Predictions if other choices were made.** *B2 (defer CANCELLED entirely — state
exists, no path, no test):* the arbitration helper would ship with one of its four
terminal causes unexercised, so the first real canceller (a future signals
milestone) would be adding an *untested* transition into tested scheduler code —
exactly the risk the owner gate exists to avoid. Testing it now with an internal
trigger is cheap and front-loads that proof.

**Flex options to consider in future.** **FLEX (planning review).** The ABI
trigger for cancellation (a signal syscall, a deadline-update that cancels, a
process-level cancel) is a future-milestone decision; A6-2 only fixes that the
mechanism exists and is internally reachable now.

### A6-3. Deadline encoding: zero = poll, bounded max, overflow-checked (C)

**Decision + why.** The timeout is a **relative duration in timer ticks** (the
existing M2 clock; inclusive `wake <= ticks` as the current sleep path uses).
**Zero timeout = a non-blocking poll** matching merged `try_recv` exactly (check
the entry snapshot once; deliver `WOKEN`-equiv / `PEER_CLOSED` / **`EMPTY`** per
A6-ABI — NOT `TIMED_OUT`). **Zero short-circuits BEFORE any waiter registration:**
it NEVER computes `wake`, NEVER registers a waiter, NEVER enters
`THREAD_BLOCKED_IPC`, and does NOT `yield` — so the inclusive `wake <= ticks` ISR
rule can never apply to it, and a 0-tick and a 1-tick request are not confusable (a
1-tick request sets `wake = ticks+1` and parks, firing on the next `++ticks`; "one
tick" means "up to one tick," no minimum). A **bounded maximum** `JE_IPC_MAX_WAIT`
is enforced: a nonzero duration is rejected (`JE_ABI_INVALID`) if
`duration > JE_IPC_MAX_WAIT` or if `ticks + duration` would overflow `uint64` —
mirroring the existing `je_thread_sleep` overflow guard. No unbounded block, ever.
**Owner-approved.**

**Predictions if other choices were made.** *Zero = block forever:* violates D7's
"no unbounded block" outright. *No max / no overflow check:* a caller passing a
near-`UINT64_MAX` duration wraps the deadline below `ticks`, making the ISR fire an
*immediate* spurious `TIMED_OUT` (or, worse, never) — a silent correctness bug the
existing sleep path already guards against, so not guarding here would be an
inconsistency.

**Flex options to consider in future.** `JE_IPC_MAX_WAIT`'s exact value is a
policy bound like the other `JE_*` caps and may change; the *rule* (zero=poll,
bounded, overflow-checked) is firm.

### A6-4. Waiter-capacity exhaustion reuses the non-blocking path (D)

**Decision + why.** Waiter slots are bounded **per endpoint end**
(`JE_EP_MAX_WAITERS` = 1 blocked receiver per end; an endpoint has two ends). A
receiver blocks on **one specific end** (the one its handle names). A second waiter
on the same end **is reachable via the ABI**, NOT structurally impossible: process
A blocks on end E (nonzero timeout) and the scheduler switches to process B, which
holds a **transferred duplicate** handle to E and also calls blocking recv on it.
So the bound is enforced as a **runtime fail-closed branch, never a panicking
assert** (a panicking assert on an ABI-reachable path would be a user-triggerable
kernel DoS): a blocking recv that finds the end's single waiter slot already
occupied does **not** park — it **returns `EMPTY`** (A6-ABI) immediately, exactly as
a zero-timeout poll on an OPEN empty end would. This is a legitimate, testable
outcome (the second concurrent blocker is told "nothing for you right now"), not an
error status. **Owner-approved** (per-end bound + runtime fail-closed to EMPTY).

**Predictions if other choices were made.** *A new `JE_ABI_WAITERS_FULL` status:*
adds ABI surface for what is really just "no message available to you right now" —
`EMPTY` already means exactly that, so a new code would be redundant and a permanent
ABI commitment. *A panicking `assert` on the occupied-slot path:* since the path is
ABI-reachable (the transferred-dup case above), any user could halt the kernel —
the DoS this fail-closed branch exists to prevent. *Making the bound per-process
instead of per-end:* mis-models the thing blocked-on (a thread waits on an END, not
on "its process"); the transferred-dup case is precisely two *different* processes
contending for one end, which a per-end bound models correctly and a per-process
bound does not. *A multi-slot waiter queue per end:* unnecessary for v1 (the second
blocker is correctly served `EMPTY` and can retry); revisit with blocking-send or
multi-threaded processes.

**Flex options to consider in future.** **FLEX (planning review).** When
multi-threaded processes or blocking *send* arrive, `JE_EP_MAX_WAITERS` grows and
exhaustion may become reachable; *then* a real status (or a queue-of-waiters) is
revisited. v1 fails closed because it cannot happen.

### A6-5. No user copy-out from the scheduler; parked syscall continuation copies

**Decision + why.** The wake path (arbitrate CAS + reserve the queue head for a
`WOKEN`) runs in the scheduler/ISR, but the **user copy-out and commit happen in
the resumed syscall continuation**, never inside `je_sched_interrupt` after a
`cr3_load`. The scheduler flip is **guarded**: `THREAD_BLOCKED_IPC → READY` **only**
— never from `DYING`, `ZOMBIE`, `EMPTY`, or any other state (so a torn-down thread
can never be made runnable by a late wake, see A6-PEERWAKE).

**Head reservation lifecycle (one outstanding reservation per end, pinned).** When
the arbitrate CAS stamps `WOKEN`, the winning path (a sender's enqueue, or the
entry-snapshot drain) **reserves exactly the current queue head** for that waiter —
a single reservation marker on the end, so no other receiver can take that message
and FIFO is preserved. The reservation is resolved by exactly one of:
- **successful commit** in the continuation → dequeue the reserved head (+ release
  any transferred-cap inflight per step 5), clear the reservation;
- **copy/mint fault** in the continuation → return `JE_ABI_INVALID`, **leave the
  message queued**, and **release the reservation** so a *future* recv on that end
  can take it. This does **NOT re-arbitrate the already-terminal waiter** (that
  would be a second transition, forbidden by A6-STRUCT): the faulting recv's waiter
  is done (terminal `WOKEN`, result `JE_ABI_INVALID`); the released reservation
  simply makes the still-queued head available to the *next* recv call;
- **waiter teardown** (process death, A6-PEERWAKE) → release the reservation before
  the slot is reused.
There is never more than one outstanding reservation per end (A6-4 bounds waiters
to one per end), so "reserve the head" is unambiguous. **A reserved head is not
available to any other receiver:** a concurrent zero-timeout poll (or entry-snapshot
check) from a process holding a transferred-dup handle to the same end sees the
reserved head as **not deliverable** and returns `EMPTY` — it does NOT take the
reserved message (that would break the reservation and FIFO) and does NOT itself
register a waiter. The reserved message is delivered only to the waiter that
reserved it, or **becomes deliverable again at its queue head** (it is never
dequeued by the reservation) when that waiter's reservation is released via the
single idempotent `je_ipc_waiter_release` (copy fault / teardown, A6-CLARIFY-2).
The continuation does **not** re-resolve the original
handle (D3 owned-reference rule) — it uses the reserved owned record.
**Owner-approved design.**

**Predictions if other choices were made.** *Copy-out inside the scheduler after
the address-space switch:* introduces a brand-new page-fault context in scheduler
assembly; the only exact-RIP user-copy-fault recovery proven so far is the M3a
*syscall* path, so copying post-`cr3_load` in the ISR would be *untested* fault
machinery in the most safety-critical file — precisely what the owner gate guards.
*Re-running the handle-based recv on wake:* re-resolves a capability the owned-
reference rule says must not be re-resolved after blocking (the handle could have
changed), and double-counts the drain. *Re-arming the faulting waiter instead of
releasing-and-leaving-queued:* would be a second terminal transition on a waiter
that already won `WOKEN` — the double-transition A6-STRUCT forbids; releasing the
reservation (message stays queued for the next call) achieves recovery without it.
*An unguarded scheduler flip:* a late wake could set a `DYING`/`ZOMBIE` thread
`READY` and resurrect a torn-down frame — the stale-slot write D3.3 names.

**Flex options to consider in future.** Firm. A blocking-send milestone will add a
symmetric copy-*in* on the sender continuation under the same rule.

### A6-CLARIFY-1. The park/resume mechanism + its gated prerequisite (owner-approved 2026-10-19)

**Decision + why.** A6-5 fixed *that* the parked syscall continuation (not the
scheduler) copies out, but not the concrete x86 mechanism. The implementation
uses **constrained restartable-syscall resume**, decided by a mechanism design
debate ([`docs/adr/debates/m3c-step6-mechanism.md`](debates/m3c-step6-mechanism.md),
claude=implementor/codex=adversarial-reviewer) and verified against the tree:
- **Park:** the recv handler, when it would return `EMPTY` on an OPEN end with a
  nonzero timeout, returns a **BLOCK action** to dispatch (NOT a nested `int
  0x81`). A new **`je_sched_block_current(f)`** — modeled on the existing
  `je_sched_exit_current` (same frame-identity checks) **plus a pre-park
  `je_uframe_check(f)`** (a deliberate, stated deviation from that model: a thread
  that would park MUST have a valid return frame, so a user-corrupted RSP/RFLAGS is
  rejected as `DEATH_BAD_RETURN` at park time rather than discovered at wake while
  it holds a reservation — see A6-CLARIFY-2) — saves the recv args + the **owned
  endpoint reference** + the post-`int` RIP into the thread's waiter fields (the
  `wait` sub-struct: `pending`, `end_ref`, `side`, saved `rdi/rsi/rdx/r8/r9`,
  `deadline`, `saved_post_int_rip`, `cause`), sets `state = THREAD_BLOCKED_IPC`,
  **rewinds `f->rip` by a named `JE_INT80_LEN == 2`** (the fixed `CD 80` encoding;
  the only DPL3 gate, and the PIC is remapped off 0x80, so `rip-2` provably points
  at the `int 0x80` site), and returns `select_and_install_next()`.
- **Wake:** a cause-stamping path (`je_sched_wake_ipc`, a **guarded wrapper around
  the single A6 `je_ipc_wait_arbitrate` CAS** — never an independent
  `BLOCKED_IPC→READY` transition) flips the thread READY. When the scheduler next
  installs it, CR3/RSP0 are already switched (`select_and_install_next`), the
  thread re-executes `int 0x80`, and a **pending-continuation prologue at the top
  of `je_syscall_dispatch`** (keyed on `threads[current].wait.pending`, **not** on
  RAX) finishes the recv **from the saved args + the owned reference, NEVER
  re-resolving `f->rdi`** (the A6-5 owned-reference rule — a plain restartable
  syscall that re-resolved the handle would violate it; "constrained" = it does
  not). The prologue also asserts `f->rip == saved_post_int_rip`; a mismatch is a
  kernel-invariant abort, not a user error. Copy-out thus happens in a fresh
  dispatch on the woken thread, **after** the context switch — satisfying A6-5's
  "no copy from the scheduler." **No trap-entry assembly change.**

**Gated prerequisite (the honest dependency).** Re-executing the restart site is
sound **only** under two invariants true on main today, now recorded so a future
change cannot silently invalidate the mechanism:
1. **User code text is immutable** — `USER_CODE` is mapped R/X with no writable
   alias surviving publish, and the ABI exposes no `mmap`/`mprotect`/`unmap`
   (the syscall ops are 0–12), so nothing can rewrite or unmap the `CD 80` site
   while parked.
2. **One thread per process** — `threads[slot]` is built from a single `p->root`,
   so no sibling thread shares the address space to race the restart site.
**This prerequisite MUST be re-gated** when `mmap`/`mprotect` or multi-threaded
processes land (a `FLEX (planning review)` item): at that milestone the restart
mechanism is re-reviewed, and if either invariant is lost it is replaced by a
kernel-side continuation that resumes without re-executing `int 0x80`.

**Predictions if other choices were made.** *A heavier kernel-continuation now
(re-enter the recv in kernel mode without re-executing `int 0x80`):* avoids the
prerequisite but adds scheduler/context-switch surface and a second copy-out entry
point — more untested machinery in the highest-blast-radius file, the opposite of
what the owner gate wants, for a dependency that is honestly recordable instead.
*Plain (unconstrained) restartable syscall that re-resolves `rdi` on re-entry:*
re-resolves a capability A6-5 forbids re-resolving after blocking — a handle-reuse
hole (the slot could now name a different object); rejected. *Hard-coding the
rewind length without a named constant + encoding comment:* a latent landmine if
the gate vector ever changes; the named `JE_INT80_LEN` + SDM citation makes it
auditable.

**Flex options to consider in future.** **FLEX (planning review).** The gated
prerequisite above is the explicit revisit trigger (mmap/mprotect/multithread).
The constant `JE_INT80_LEN` is fixed by the `CD 80` ABI and only changes if the
syscall gate itself is redesigned (a new ADR).

**Decision + why.** `je_vm_destroy`'s existing endpoint release walk (step-3,
owner-signed) tears down the dying process's handles. When removing the dying
process's last handle to an end transitions **that end to `CLOSED`**, the end's
**peer end becomes `PEER_CLOSED`** (the merged step-3/4 state rule — the dying side
goes CLOSED, the surviving side goes PEER_CLOSED). The walk now, in the SAME
IRQ-critical section, also **calls the arbitrate CAS on a receiver blocked on that
PEER (surviving) end** — i.e. a receiver in the *other*, still-living process —
waking it `PEER_CLOSED` **immediately** rather than letting it wait out its
deadline. Note per A6-1 (drain-first): if the peer end still has a queued message,
the blocked receiver was already woken `WOKEN` when that message was enqueued and
is not blocked anymore; `PEER_CLOSED` wake applies to a receiver blocked on an
**empty** peer end.

**Teardown ordering (pinned, three steps, in order).** For the dying process's
**own** waiter (a thread of the dying VM that was itself blocked in `recv`):
1. **FIRST**, before the release walk runs, **unlink/tombstone the dying process's
   own waiter** (clear its waiter fields, release any head reservation it holds)
   and leave its thread `DYING` — the walk and the ISR must never `arbitrate` or
   wake it. `je_ipc_wait_arbitrate` **skips a tombstoned/non-`WAITING` waiter**
   (returns "lost").
2. **THEN** run the release walk, which wakes only **surviving peer-owned**
   waiters (step above) — never the dying process's own (already tombstoned).
3. The guarded scheduler flip (A6-5: `THREAD_BLOCKED_IPC → READY` only) means even
   a stray stamp on the dying thread could not make it `READY` from `DYING`.
This ordering guarantees no wake writes a frame slot that teardown is reusing
(D3.3). **Owner-approved design.**

**Predictions if other choices were made.** *Rely on the receiver's own deadline /
next schedule to notice the close:* a receiver blocked with a far deadline on an
endpoint whose peer just died would sleep uselessly until timeout — a latency bug
and a semantic one (it should learn `PEER_CLOSED` now, not `TIMED_OUT` later).
*Unlink the dying process's own waiter AFTER the release walk (or not at all):*
the walk could arbitrate-and-flip the dying thread, racing slot reuse — the exact
resurrection the review flagged; unlinking FIRST + the guarded flip + arbitrate-
skips-tombstoned closes all three. *Waking the dying (CLOSED) end's waiter instead
of the surviving (PEER_CLOSED) end's:* names the wrong end — the thread to wake is
in the living process, on the end that just became PEER_CLOSED.

**Flex options to consider in future.** Firm; symmetric with the step-3 release
walk it extends. Also covers an ordinary last-handle `endpoint_close` (no process
death) that transitions a peer end to PEER_CLOSED: the same arbitrate-the-peer's-
waiter call applies on that close path, not only in `je_vm_destroy`.

### A6-CLARIFY-2. Death/reap releases a held WOKEN reservation (owner-approved 2026-10-19)

**Decision + why.** A6-5 reserves the queue head for a receiver stamped `WOKEN`,
delivered when its continuation resumes. There is a window — the woken thread is
READY but not yet re-entered (from "READY, not yet installed" through the `iretq`
to the `int 0x80` restart site; `je_uframe_check` sets IF=1, so a timer tick — and,
in a future milestone, a kill/cancel — can land there) — in which the reservation
is **held by a thread that has not yet consumed it**. If that thread is destroyed
in the window, the reserved head would be stranded (marked reserved, no live waiter
→ a permanent per-end denial of delivery). So teardown MUST release it. To avoid a
double-release across the three candidate sites (`mark_dying`, the reaper, and
A6-PEERWAKE step 1), there is **exactly one idempotent cleanup routine**,
`je_ipc_waiter_release(slot)`, that **checks whether THIS waiter holds the
reservation and clears it only then** (clearing the waiter state + returning the
head to deliverable). **`mark_dying` calls it; the reaper only asserts it is
already clear; A6-PEERWAKE step 1 is the same routine.** CLARIFY-2 is simply the
`READY`/`WOKEN` case of that one routine. It runs in the IRQ-critical teardown
**before the release walk** (the A6-PEERWAKE ordering — if the walk ran first it
could close an end or discard queued messages that are still reserved). The
released head **becomes deliverable at its queue front** — it was never dequeued
(the reservation is only a "claimed-by" marker on the still-enqueued head), so it
stays FIFO-first for the next receiver. **The waiter slot stays occupied from
`WOKEN`-stamp until consume-or-release**, so A6-4 gives any concurrent second
receiver `EMPTY` (nobody can park behind a reserved head and then sleep to its
timeout — the latency bug A6-PEERWAKE exists to prevent); release frees the slot
and the head together.

**Reachability, honestly.** There is **no user/ABI-triggered *production* death
path** into this window in v1 — there is no kill/cancel op, and the pre-park
`je_uframe_check` (A6-CLARIFY-1) rejects the corrupt-return-frame case at park time
rather than letting it die at wake. The only way to reach it in v1 is a
**test-only forced-kill hook** (named + gated exactly like the existing dev-only
`JE_DEV_COPY_INJECT`, never in the production ABI) that marks a READY thread
`DYING` so the reaper can reclaim it — used solely by the CLARIFY-2 proof. So this
closes a hole that is currently reachable **only under the test hook**, and that a
future real cancel/kill (the A6-2 signal milestone) would otherwise expose.
**Owner-approved.**

**Predictions if other choices were made.** *Leave it unspecified (the A6 gap as
drafted):* the first future kill/cancel path strands the reserved head
permanently — a silent per-end denial of delivery; cheaper to close now. *Two
release sites instead of one idempotent owner:* a double-release / double-free of
the reservation when `mark_dying` and the reaper (or PEERWAKE) both fire — the bug
the exactly-once holder-checked routine removes. *Freeing the waiter slot at
`WOKEN`-stamp instead of at consume/release:* a second receiver could park behind
the reserved head and sleep to timeout — the latency bug; keeping the slot occupied
makes A6-4 return `EMPTY` instead. *Dropping the reserved message rather than
returning it to the queue front:* loses a legitimately-sent message on an unrelated
death; it was never dequeued, so returning it is correct and trivial.

**Flex options to consider in future.** Firm for v1. When a real cancel/kill path
arrives (the A6-2 signal milestone), this release is the cleanup it must call —
the test added now (wake → destroy the holder before re-entry → the head is
delivered to another receiver) already exercises the path via the reaper, so the
future canceller inherits a tested cleanup.

Each terminal cause gets a deterministic kernel/hosted proof that does **not**
rely on two spin-looping user stubs (the step-5 hang lesson). All blocking triggers
in v1 are driven by **kernel-side helpers / explicit yields / stepping
`je_timer_ticks` / stepping the bootstrap reaper** — there is no ABI self-trigger
(no signals, no async kill), so every proof is a deterministic sequence, not a
race:
- **queued-before-recv:** message already present → immediate `WOKEN`, no park.
- **WOKEN:** thread X parks on recv; thread Y sends then *explicitly yields* →
  X resumes, exactly one `WOKEN`, FIFO delivery.
- **zero-timeout poll:** R10=0 on an OPEN empty end → `EMPTY` (13), X never parks
  (asserts the A6-3 short-circuit: no waiter registered, no yield).
- **TIMED_OUT:** X parks (nonzero R10); advance `je_timer_ticks` to the inclusive
  deadline → ISR arbitrates `TIMED_OUT` (status 14), X resumes with it.
- **PEER_CLOSED:** X (in process A) parks on an empty end whose peer is held by
  process B; **step the bootstrap reaper to run `je_vm_destroy(B)`** → the release
  walk arbitrates `PEER_CLOSED` (12) on A's surviving end *before* A's deadline.
- **CANCELLED:** the internal `je_ipc_wait_cancel(waiter)` helper (A6-2, no ABI
  path) → exactly one `CANCELLED` (15).
- **copy-out fault after WOKEN:** `JE_ABI_INVALID` (2), message stays queued,
  reservation released; a *subsequent* recv on the end takes the still-queued head
  (NOT a re-arm of the faulted waiter).
- **teardown while blocked:** X (in B) parks, then a **test-only forced-kill hook**
  (dev-gated like `JE_DEV_COPY_INJECT`, never production ABI) marks B's READY/parked
  thread `DYING` and the **reaper is stepped** → B's own waiter is released via the
  single idempotent `je_ipc_waiter_release` FIRST (before the release walk), before
  the thread/frame slot is reused; the guarded flip never makes the `DYING` thread
  `READY`.
- **reservation release on death-before-re-entry (A6-CLARIFY-2):** X is woken
  `WOKEN` and its head reserved; the **test-only forced-kill hook** marks X `DYING`
  and the reaper is stepped **before** X's continuation re-enters → the single
  idempotent release returns the reserved head to its queue front, and a *second*
  receiver (holding a transferred dup of the end, the A6-4 reachable case) then
  calls recv **after** the release and takes it — proving the reservation was
  released, not stranded, and that the slot freed so the second receiver no longer
  gets `EMPTY`.
- **ordered serialization (not a "tie"):** on one CPU two causes are never
  simultaneous — prove the two orderings explicitly: (a) enqueue's CAS fires, THEN
  a timer tick → X gets `WOKEN`, the later timeout CAS is a no-op, message
  delivered; (b) timeout CAS fires, THEN a late enqueue → X gets `TIMED_OUT`, the
  message remains queued for a future recv. Assert exactly one terminal result in
  each; the entry-snapshot (non-empty queue + peer closed) separately asserts the
  A6-1 drain-first pick.

**What is actually proven in the step-6 PR (honest status).** The **end-to-end
ring-3 path** is proven by the kernel self-test **`M3c-s6`** for the **TIMED_OUT**
cause: a single ring-3 thread calls `recv(timeout)` on an end nobody sends to, it
genuinely parks (`BLOCKED_IPC`), the timer ISR advances `je_timer_ticks` past the
deadline and performs the terminal CAS, the continuation prologue re-enters at the
restart site and returns status 14, the waiter is released, and the channel
reclaims to 0. This exercises the whole mechanism — park, the ISR terminal
transition, the 2-byte restart resume, the continuation delivery, waiter release —
with ONE ring-3 thread and the natural timer tick (no second spinning process; the
step-5 cooperative-scheduler spin-hang lesson). The **WOKEN / PEER_CLOSED /
CANCELLED** causes, the **single-terminal-CAS / first-wins**, the **reserved-head
masking**, **A6-4 WAITERS_BUSY**, and **A6-CLARIFY-2 release-returns-head** are
proven **deterministically in the hosted pure tests** (`test_endpoint` 27–28) plus
the decode matrix (`test_syscall`). The **full two-process ring-3 WOKEN handshake**
(thread X blocks, a *second* ring-3 process sends and yields, X wakes) is **deferred
to step 7's bootstrap fixture** — the same honest deferral as the step-5 ring-3
two-process transfer, for the same reason (multi-process ring-3 choreography is
step 7's subject; a spin-based two-stub proof races the single-CPU cooperative
scheduler). The wake-on-send and wake-on-peer-close *kernel bridges* are wired and
compiled; what step 7 adds is the ring-3 *scheduling* of two cooperating user
processes to drive them end-to-end.
