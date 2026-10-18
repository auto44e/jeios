# ADR 0008: Freeze the M3b typed-handle contract

Status: **accepted contract, 2026-10-17** (owner-approved; the one open decision,
D-OPEN, was resolved in favour of **Option A** — see below). Scope: M3b only. No
kernel code changes accompany this ADR; it converts the M3b handle proposals into
reviewed decisions that issue [#7](https://github.com/auto44e/jeios/issues/7)
implements and tests against, landed as small PRs mirroring the M3a sequence.
Reviewed baseline: `main` at the M3a completion merge (`b28f947`); the M3a
qualification retrospective [M3A_RETRO.md](../M3A_RETRO.md) landed just after, at
`73cefa1`. The ADR itself was merged in PR #33.

## Context

M3a ended with two isolated CPL3 processes, fault-local death, a provisional v0
syscall gate (`abi_query`/`yield`/`exit`), and checked `copy_from/to_user`. There
is no way yet for a process to *name* a kernel object: every syscall so far takes
scalars, not references. M3b introduces the first object-naming layer — a
per-process **handle table** — so that later work (M3c capability IPC, services)
can hand a process a reference to a kernel object and have the kernel verify, on
every use, that the reference is live, well-typed, and within the authority the
process was granted.

The hazard a handle table exists to defeat is **confused-deputy / use-after-free
via a stale name**: a process must never, by presenting an integer it legitimately
held, be able to reach an object that has since been freed or an object of a
different type than it believes. The design is built around *rejecting* stale and
mismatched names cheaply and deterministically, not around trusting them.

**Security claim — narrow, stated up front.** A handle is **stale-name
protection, not an unforgeable capability.** A process that *guesses* a currently
live `(index, generation)` pair would pass lookup; generations make *reused* slots
safe, they do not make handles cryptographically unguessable. The protection model
therefore relies on two things the kernel *does* enforce: (1) a handle is only ever
**minted by the kernel** into the owning process's own table, and (2) a handle is
**never transferred raw between process tables** — any future cross-process grant
(M3c) is a destination-side *mint* of a fresh handle in the recipient's table, not
a copy of the sender's integer. Within one process's table, guessing another of
your *own* live handles grants nothing you were not already granted. Consistent
with ADR 0007 D1, the claim is "experimental single-CPU process-local object naming
with generation-checked lifetime, qualified by test," never a capability-security
guarantee against a hostile multi-threaded or SMP caller.

## Decision

### D1. Scope and security claim

M3b delivers a **bounded, process-local, typed handle table** with:

- fixed-capacity slot allocation (no dynamic growth);
- an explicit per-slot state machine **`FREE | LIVE | RETIRED`** (D3);
- a **generation counter** per slot that makes a reused slot's old handles stale;
- **typed** slots (a handle carries/implies an object type; wrong-type use is
  rejected);
- **typed rights** on a slot (a rights mask checked on use); and
- explicit `close`, with slot reuse only through a generation bump; and
- defined, non-authority-expanding failure semantics for stale / forged /
  wrong-type / denied / quota-exhausted / reference-overflow cases.

**Attenuation (deriving a weaker handle) and multi-handle references to one object
are NOT frozen by this ADR** and are NOT in the first PR; they are deferred to a
later PR + ADR amendment (see D-RESOLVED and the follow-up note in D3), because the
same-slot-vs-fresh-slot design question materially changes quota, close semantics,
and reference ownership and should be decided against running code. What is frozen
here is only: the rights mask exists and is checked on use, and any future
attenuation may only ever *remove* rights, never add them.

Retained from M3a: one CPU, PIC/PIT, IF=0 on kernel paths, the per-process arena,
and the v0 syscall gate. **Not** in M3b: transferring a handle to another process
(that is M3c), SMP-safe refcounting, dynamic table growth, a global object
namespace, or any stable ABI promise. M3b objects are kernel-internal; the only
objects that need to exist to test the table may be minimal stand-ins until M3c
introduces real transferable endpoints.

### D2. Handle encoding is a frozen ABI (freeze it before code)

A handle is a single fixed-width unsigned integer. Even though M3b's first PR
keeps the table internal, the handle value **becomes the syscall ABI** the moment
a syscall takes a handle argument, so its encoding is frozen here, not discovered
later:

- **Total width:** 32 bits (`je_handle`, a `uint32_t`). Rationale: it is the
  natural width for a syscall scalar, leaves generous room for the split below at
  M3a's bounded table sizes, and avoids 64-bit handle bloat in every future
  message. If a future milestone needs more, that is a new ADR and a new ABI
  version, not a silent widening.
- **Bit split:** low bits = slot **index**, high bits = slot **generation**. The
  exact split is a compile-time constant pair `(JE_HANDLE_INDEX_BITS,
  JE_HANDLE_GEN_BITS)` with `INDEX_BITS + GEN_BITS == 32` enforced by a
  `_Static_assert`. The **production** split is fixed here as **index = 16 bits
  (<= 65536 slots), generation = 16 bits**; the table's actual capacity may be far
  smaller than `2^INDEX_BITS`, but no index wider than the field is ever emitted.
- **Reserved zero:** the all-zero handle value `0` is permanently **invalid** (a
  natural null). To guarantee this, **slot generations start at 1, never 0**, so a
  freshly allocated slot 0 encodes to a nonzero handle; allocation never returns
  `0`. This gives callers a zero-initialized "no handle" for free and makes a
  forged/cleared handle fail closed.
- **Invalid encodings:** any handle whose index is `>=` the table's **fixed**
  capacity, or whose generation does not match the slot's current generation, or
  that is `0`, is rejected at lookup with a defined error and no object access.
- **Register width:** a handle passed as a syscall argument occupies the low 32
  bits of its 64-bit argument register; the **upper 32 bits MUST be zero** (the
  ADR 0007 "reserved bits are zero" rule). A nonzero upper half is a **malformed
  syscall call**, not a malformed handle: it is rejected at the syscall decode as
  `JE_ABI_INVALID` **before** the value is narrowed to a `je_handle` and before the
  table is consulted — exactly as any other reserved-argument violation. (This is
  distinct from a well-formed 32-bit handle that is simply wrong, e.g. handle 0 or
  an out-of-range index, which reaches the table and returns `BADINDEX`.) Amended
  2026-10-17 at the first handle-syscall PR: the earlier wording "checked before
  decode, else BADINDEX" is superseded — the wide-handle rejection is the ABI's
  INVALID, keeping "malformed call" and "bad handle" as separate, honest signals.
- **Status namespace (latent ABI, frozen here):** the `JE_HANDLE_*` statuses are
  part of the **v0 ABI status namespace** and MUST NOT leak internal `je_status`
  numbering (ADR 0007 D11). Their external numeric values are assigned in the v0
  ABI header alongside the existing `JE_ABI_*` values; whether userspace sees
  `STALE`/`BADINDEX`/`WRONGTYPE`/`DENIED` as **distinct** statuses or one collapsed
  `INVALID` is itself an ABI choice frozen at the first syscall PR (default:
  distinct, since they are a useful non-secret diagnostic — a handle oracle over
  your *own* table reveals nothing you did not already hold).
- **Rights mask (latent ABI, frozen here):** rights are a fixed-width bitmask whose
  width and per-bit meaning are assigned in the ABI header; **unknown rights bits
  are rejected** (not silently ignored) wherever a rights value crosses the
  boundary.
  - *Assigned 2026-10-18 (M3b third PR):* `uint32_t`, `JE_ABI_RIGHT_INSPECT = 0x1`,
    `JE_ABI_RIGHTS_KNOWN = INSPECT`, in `abi.h`. `handle_rights` (op 8) returns the
    mask masked to the known bits. **Owner decision recorded:** this PR exposes the
    rights ABI and wires the enforcement check as a POLICY HOOK (option A); it is
    NOT end-to-end ring-3 enforcement, because no ring-3 path yet mints a
    rights-reduced handle, so DENIED/WRONGTYPE are hosted-test-only. The **(B)/A+B
    follow-up** — genuine ring-3-observable denial — lands with M3c attenuation,
    which also introduces the runtime rejection of unknown caller-supplied rights
    bits (in M3b every mint is a kernel constant, compile-asserted known).
  - *Open question for M3c (raised in review, not yet decided):* `handle_query`
    (op 7, returns the object TYPE) requires **no** right, while `handle_rights`
    (op 8) requires INSPECT. Once attenuation can mint a zero-rights handle,
    `query` would still reveal the type while `rights` returns DENIED. Decide
    before M3c makes that observable whether reading the type should also require
    INSPECT, or whether INSPECT is narrowed to "the rights mask only" — so op 7's
    frozen meaning is settled deliberately, not changed implicitly later.
  - *Not frozen:* `JE_ABI_RIGHTS_KNOWN` is a per-ABI-version SET that grows as M3c
    adds rights bits; the individual bit *values* are what's frozen. Userspace
    learns the known set via a version bump or a future `abi_query` selector.

> Encoding/decoding is pure and will be unit-tested in isolation (like
> `je_syscall_decode` in M3a): `encode(index, gen)`, `decode(h) -> {index, gen}`,
> and the validity predicate, with exhaustive tests at a reduced width (see D5).

### D3. Authority lifetime — the central contract

This is the design point most easily gotten wrong, so it is pinned explicitly.
Each slot is in exactly one of three states: **`FREE`** (allocatable), **`LIVE`**
(bound to an object), or **`RETIRED`** (permanently unusable, see D4). For each
slot the kernel tracks: its state, the current **generation** (initialized to
**1**, never 0 — D2), and, when `LIVE`, the bound **object**, the **type**, and
the **rights** mask. The lifetime rules:

1. **Allocation** takes a `FREE` slot, binds the object/type/rights, moves it to
   `LIVE`, and returns a handle carrying the slot's *current* generation.
2. **Lookup**`(h, expected_type, required_rights)` succeeds **only if**: `h != 0`
   and its upper 32 bits are zero; `index < capacity`; the slot is `LIVE` (a `FREE`
   or `RETIRED` slot fails here *before* any generation comparison); the slot's
   generation **exactly equals** `h`'s generation; the slot's type equals
   `expected_type`; and the slot's rights **superset** `required_rights`. Any
   failure returns a defined status (`JE_HANDLE_STALE`, `_BADINDEX`, `_WRONGTYPE`,
   `_DENIED`) and **no** object pointer. Lookup never mutates state. `expected_type`
   and `required_rights` are supplied by the *syscall implementation*, never by the
   user.
3. **Close** moves a `LIVE` slot to `FREE` and **advances its generation by one**.
   Every handle that carried the old generation is now permanently stale. Closing
   an already-`FREE`/`RETIRED` (i.e. stale) handle returns `_STALE`, not success.
4. **Generation advance on every LIVE→FREE.** A slot's generation increments on
   each close. It is **monotonic and non-wrapping**: see D4.
5. **Attenuation and multi-handle references are deferred (not frozen here).** A
   later PR + ADR amendment decides whether attenuation consumes a fresh slot (new
   generation, independent close) or annotates the same slot, and introduces the
   bounded per-object reference count that multi-naming requires. The invariants
   fixed **now**: attenuation may only ever *remove* rights, checked against the
   source handle's current rights; and any reference count is a plain integer under
   the UP/IF=0 precondition (D6), not an atomic. Until that PR, exactly one `LIVE`
   slot names an object and `close` is its sole teardown trigger, which keeps the
   first PR's lifetime trivially analyzable.
6. **Reference acquisition / overflow (deferred with attenuation).** When
   multi-naming is introduced, acquiring past the bound fails with `_REFOVERFLOW`
   and acquires nothing; the object is destroyed only when the count reaches zero
   **and** no `LIVE` slot names it. The acquire-then-commit ordering is frozen in
   D8 so a failed acquire cannot leak a slot.

### D4. Generation saturation retires the slot (no wraparound)

A generation counter that wraps would let a *very* old handle alias a *new*
binding once the counter cycles back — the exact bug generations exist to prevent.
Therefore:

- A slot's generation **saturates into the `RETIRED` state**: when a close would
  advance the generation past its maximum (`2^GEN_BITS - 1`), the slot instead
  transitions `LIVE → RETIRED` and is **never allocated again** for the life of the
  table. A `RETIRED` slot fails lookup at the state check (step 2), before any
  generation comparison, so no handle can ever match it.
- **Retired capacity is permanently lost.** Retirement shrinks the table's usable
  capacity. This is a deliberate, testable, fail-safe trade: correctness (no
  aliasing) over reclaiming the last generation of a slot. At the production
  `GEN_BITS = 16` a slot must be closed 65534 times before it retires (generations
  run 1..65535), so in practice retirement is a safety backstop, not an operational
  event — but the behavior is defined and tested, not assumed unreachable.
- **Quota accounting (owner-visible decision, recorded here):** `RETIRED` slots
  count against **table capacity** (they are gone) but are **not** chargeable to a
  live quota (a process is not "using" a retired slot). `allocate` fails with
  `_QUOTA` when the `LIVE`-slot count hits the process's quota, and fails with
  `_NOSLOTS` (distinct) when **no `FREE` slot remains** (all are `LIVE` or
  `RETIRED`). The two exhaustion causes are reported distinctly so tests — and
  operators — can tell "you hit your limit" from "the table wore out."

### D5. Generation width is a compile-time parameter (so saturation is testable)

Saturation and retirement are untestable at the production 16-bit width (it would
take 65535 close cycles per slot). Therefore `JE_HANDLE_GEN_BITS` (and the paired
`INDEX_BITS`) are **compile-time parameters**:

- The **hosted test build** additionally compiles a deliberately tiny width (e.g.
  `GEN_BITS = 2`, so saturation hits after 3 closes) to exercise generation
  wrap-avoidance, retirement, and capacity loss exhaustively — cases unreachable
  at 16 bits.
- The reduced width **changes field positions**, so it cannot stand in for the
  production encoding. `make check` therefore tests **both**: the reduced width for
  exhaustive saturation/retirement, **and** the exact production `16/16` encoding
  with boundary vectors (index 0 and `capacity-1`, generation 1 and 65535, the
  reserved-zero handle, an upper-32-bits-set handle). The production build carries
  a `_Static_assert(INDEX_BITS == 16 && GEN_BITS == 16)` — an exact pin, not a
  floor — so a careless edit cannot ship a different production encoding, and a
  separate assert that `INDEX_BITS + GEN_BITS == 32`.

### D6. Linearization precondition — stated honestly, asserted in code

All of the above (close/use ordering, the non-atomic reference count, the
"lookup never races a close" assumption) are sound **only** because M3b inherits
M3a's execution model: **one CPU, one thread per process, and IF=0 on every kernel
path** so no kernel code is preempted mid-operation. This is the *precondition*,
not a property the handle code provides. The ADR requires:

- the uniprocessor / non-preemptible-kernel precondition is **asserted both at
  boot and at every handle-API entry point** (a cheap "interrupts are disabled"
  assert on each `allocate`/`lookup`/`close`), so a later code path that enables
  interrupts and then calls into the table fails loudly rather than silently
  racing. (In the hosted test model there is no real IF; the assert is a no-op
  there, and the ADR records that the hosted tests therefore do **not** exercise
  this precondition — only the on-target build does.) Destructor reentrancy and
  any future temporary-borrow lifetime get explicit rules in the attenuation/
  reference PR, not hand-waved under IF=0; and
- M3b **makes no SMP-safety claim**. Building atomic/locked refcounting for a
  hypothetical SMP future now would be overbuild and is explicitly out of scope.
  SMP handle safety is a later, separate ADR with its own linearization proof.

### D7. Teardown ordering and the M3a fault-death path (owner-gated)

Per-process teardown must drain the handle table as part of process death
(M3a's existing `je_sched_fault_terminate` / reap path). The required order:
**revoke/close all of the dying process's slots → drop the object references those
slots held → destroy any object whose reference count and naming slot are both
gone → then the existing M3a page/account reclamation.** Two constraints:

- A dying process's teardown must not free an object another process still
  references (M3c will make cross-process references possible; even in M3b the
  ordering must be correct by construction).
- **Changing M3a's tested fault-death path is a contract/tested-code change.**
  Under the repository's rules it is **escalated to the owner**, not decided by an
  agent or a peer vote, and it does **not** belong in M3b's first PR. The first PR
  keeps the table standalone (no object destruction, no death-path change); the
  teardown wiring is a later PR with its own QEMU evidence and an explicit owner
  sign-off on any death-path edit.

### D8. Failure semantics (no authority expansion)

Every error path returns a defined status and performs **no** object access and
**no** state mutation beyond what the operation explicitly commits:

| Condition | Status | Effect |
| --- | --- | --- |
| handle `0`, or index `>=` capacity | `JE_HANDLE_BADINDEX` | none |
| generation mismatch (reused/retired slot) | `JE_HANDLE_STALE` | none |
| wrong object type | `JE_HANDLE_WRONGTYPE` | none |
| insufficient rights | `JE_HANDLE_DENIED` | none |
| attenuation requesting a right the source lacks | `JE_HANDLE_DENIED` | none |
| allocate at live-quota | `JE_HANDLE_QUOTA` | none |
| allocate with no non-retired slot free | `JE_HANDLE_NOSLOTS` | none |
| reference acquire past bound | `JE_HANDLE_REFOVERFLOW` | none |

No error path ever returns a usable object, a different object, or more rights
than requested. A failed `allocate` leaves the table exactly as it was. For the
later attenuation/reference PR, the commit order is frozen now so a partial failure
cannot leak: **acquire the object reference first, then allocate the slot**; if the
allocate fails (`QUOTA`/`NOSLOTS`), the acquired reference is released before
returning. When both `QUOTA` and `NOSLOTS` could apply, **`QUOTA` is reported**
(the process-visible limit takes precedence over the table-wear condition).

### D9. First PR scope (smallest independently testable + mergeable)

Mirroring the M3a decomposition, the first M3b PR is **this ADR (accepted) plus a
pure, hosted, standalone slot-table model** — no syscalls, no object destruction,
no change to any M3a-tested path:

- `encode`/`decode`/validity (D2), and `allocate → lookup → close → stale-lookup`
  over a fixed-capacity table;
- **deterministic allocation order = lowest-`FREE`-index.** This is an **internal
  implementation policy made deliberately deterministic for testability**, not a
  user-visible ABI promise (userspace must never depend on *which* index it gets).
  A test can then assert the exact index a fresh handle receives, catching
  reuse-order bugs before generations can hide them;
- hosted tests, at **both** the reduced generation width and the exact production
  `16/16` width (D5): deterministic lowest-free allocation; forged/`BADINDEX`
  handles (incl. upper-32-bits-set); `STALE` handles after close and against
  `FREE`/`RETIRED` slots; generation **saturation → `RETIRED`**; **retired-slot
  capacity loss**; `QUOTA` vs `NOSLOTS` exhaustion distinctness; and the
  zero-handle-is-invalid / generation-starts-at-1 rules.

Typed rights checking on use, typed objects, attenuation + cross-handle references
+ overflow, and the process-death teardown wiring (D7) each follow as **separate**
PRs, each with its own tests and — for the teardown PR — owner sign-off on any
death-path edit.

## D-RESOLVED. Owner decision (resolved): the slot-vs-instance split — **Option A**

M3b slots name objects; a **service restart** (M3c) replaces the object behind a
name. The peers split on how a handle stays safe across that restart; the owner
resolved it: **Option A is accepted for M3b**, with B / a hybrid A+B recorded as a
future consideration (see "Future consideration" below).

- **Option A (ACCEPTED):** the handle carries **only** the slot generation. A
  service restart is modelled as **close + re-allocate**: the slot is revoked, its
  **generation bumped**, and a *new* handle minted for the restarted instance. Old
  handles go `STALE` automatically. The handle encoding stays narrow (index +
  slot-gen, exactly D2); there is no second epoch field and no caller-supplied
  instance generation to compare. Restart safety falls out of the existing
  generation mechanism. This keeps the 32-bit encoding in D2 intact, needs no new
  comparison in the lookup hot path, and reuses the generation mechanism the first
  PR already tests exhaustively (a restart is just a close+allocate).
- **Option B (not chosen for M3b):** the handle **also** carries an **instance
  epoch** (a second field), so a slot can be rebound to a restarted instance
  *without* a generation bump and a stale-instance handle is caught by comparing
  the epoch. This survives "same slot, same slot-generation, new instance" but
  **widens the handle ABI** (less room for index/slot-gen in 32 bits, or a wider
  handle) and adds a field to every lookup — a capability M3b does not need.

**Scope caveat (carried forward to M3c).** Option A fully handles **single-process,
local slot reuse**, which is all M3b itself needs. It does **not** by itself make a
*cross-process* alias stale when a shared object is restarted — closing and
reallocating one process's slot says nothing about another process's handle to the
same object. That cross-process "restart revokes every alias" property is an
**object-wide incarnation** question that only arises once M3c introduces
transferable endpoints, and it is **deferred to the M3c ADR** as its own decision
(either every alias checks a shared incarnation value, or restart walks and revokes
all naming slots).

**Future consideration (owner direction):** revisit Option B — or a **hybrid A+B**
— when M3c forces the object-wide incarnation decision. The natural hybrid keeps
Option A's narrow per-process handle as-is and adds a *separate* object-side
incarnation counter checked only on cross-process / transferred references, so the
common single-process lookup stays exactly as frozen here while restarted shared
objects still invalidate every alias. That evaluation is a **documentation/ADR
update at M3c**, not an M3b change: because Option A does not widen the handle ABI,
adopting a hybrid later is additive (a new check on the transfer path) rather than
a breaking re-encode — so starting with A costs nothing if B-like behavior is added
later. Nothing in the M3b first PR forecloses it.

With D-RESOLVED settled, the first PR (D9) may begin.

## Consequences

- A later milestone that needs >65536 slots, a wider handle, SMP-safe refcounting,
  or a cross-process transfer ABI writes a new ADR and a new handle ABI version;
  none of those are silent changes.
- Retirement means a pathologically close/reopen-heavy process can slowly shrink
  its own table; this is defined, bounded, and far below any realistic workload at
  16-bit generations, and is preferred over any wraparound aliasing risk.
- Keeping the first PR standalone (no death-path edit) preserves the M3a tested
  fault-death path untouched until an explicit, owner-signed teardown PR.

AI-assisted design (including a two-round peer cross-talk debate); not independent
human qualification. The owner is the merge gate; D-RESOLVED records the owner's
Option-A decision, and any D7 death-path change remains owner-gated.
