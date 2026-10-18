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
