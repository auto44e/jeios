# ADR 0006: Bound the first memory and execution implementation

Status: implemented foundation, 2026-10-12.

M1/M2 uses the QEMU legacy PC/BSP platform, a first-GiB physical limit, static
page-table/page-state metadata, and eight kernel execution slots including idle.
These limits make ownership, fault handling and context switching inspectable
before userspace or hardware breadth expands. They are implementation bounds,
not permanent restrictions in the product architecture.

The initial heap allocates page extents with separate trusted records. A slab
allocator is deferred until fragmentation, resource ownership and failure paths
have a verified baseline. This wastes memory for small objects but simplifies
zeroing and rollback. A mapping failure releases the whole extent. It does not
supply stale-pointer detection, quarantine or complete memory safety.

The BSP serializes allocator/scheduler mutations by masking interrupts. This
must be replaced or augmented by explicit locks, lock ordering and cross-CPU TLB
invalidation before enabling additional CPUs. Unbounded priority claims, SIMD,
real-time behavior and SMP compatibility are explicitly out of scope.

Seven CPU-bound workers test real preemption, complete GPR preservation, sleeping,
heap ownership and deferred stack reclamation. This is a deliberate development
workload. M3 will introduce separately reviewed user address spaces/authority;
CPL0 workers do not count as sandboxed applications.

## Decision-record annotation (reconstructed — sweep 2026-10-19)

Per ADR 0000; reconstructed from this ADR's text only. Part 4: no transcript
retained.

**Decision + why (as recorded).** Bound the first memory/execution implementation:
QEMU legacy PC/BSP, first-GiB physical limit, static page-table/page-state
metadata, 8 kernel execution slots (incl. idle); a page-extent heap with separate
trusted records (slab deferred); interrupt-masking serialization on the BSP; seven
CPU-bound workers as the preemption/GPR/sleep/heap/stack-reclaim test workload.
Status: implemented foundation (2026-10-12). These are "implementation bounds, not
permanent restrictions."

**Predictions if other choices were made (reconstructed).**
- *A slab allocator up front:* explicitly deferred "until fragmentation, resource
  ownership and failure paths have a verified baseline"; the ADR records the
  accepted trade (page-extent heap "wastes memory for small objects but simplifies
  zeroing and rollback," and "a mapping failure releases the whole extent").
- *Enable SMP on the interrupt-masking serialization:* the Consequences state this
  "must be replaced or augmented by explicit locks, lock ordering and cross-CPU TLB
  invalidation before enabling additional CPUs" — enabling SMP first would be
  unsound.
- *Treat the 7 CPL0 workers as sandboxed apps:* explicitly not — "CPL0 workers do
  not count as sandboxed applications"; M3 introduces the reviewed user boundary
  (ADR 0007).

**Flex options to consider in future (reconstructed).** Every bound here
(first-GiB, 8 slots, page-extent heap, BSP masking) is an explicit
`FLEX (planning review)` item unlocked by later milestones (slab when
fragmentation/ownership baselines exist; locks before SMP). The
"bounds-not-permanent-limits" framing IS the flex.


## Security note — zero physical frames on free (owner-approved 2026-11-01)

**Defect.** `je_pages_alloc`/`je_pages_free` (kernel/core/alloc.c) manage only the
`state[]` free-list bookkeeping; they never touch the 4 KiB of frame memory. Only
`je_heap_alloc` zeroes, and only for the kalloc heap. The VM backend
(`kernel/arch/x86_64/vm_backend.c`) took frames straight from `je_pages_alloc`, and
`vb_link_tables` carried a FALSE comment ("je_pages_alloc returns zeroed frames")
while setting only 4 of 2048 page-table entries and trusting the rest to be zero.
On teardown `vb_free_extent`/`vb_release_hold` freed frames WITHOUT zeroing, and
the first-fit allocator hands the lowest free run straight back. A freed frame can
be reused by a later process in ANY role (page table, user data, kernel stack), so
leftover bytes became a cross-process / kernel disclosure — and if a recycled frame
is reused as a page table, its stale bytes are live PTEs (a user process could have
written present, user, writable entries pointing at identity-mapped physical
memory). `je_vm_audit` only checked 4 upper entries, and the hosted fake backend
zeroes tables, so neither caught it. Earlier boots stayed benign only because
frames were recycled into the same roles with their leaves cleared; M4 PR-F's
spawn-many/reap-many workload is the first to make role-shifted reuse likely, which
surfaced it as a child-build failure.

**Decision (b + c).** Fix the ROOT CAUSE plus defense in depth:
- **(b, root cause)** `vb_scrub_frames` zeroes every byte of a frame run at the
  single point VM-owned frames are released — `vb_free_extent` and
  `vb_release_hold`. No freed frame carries stale bytes into reuse, for ANY role.
- **(c, defense in depth)** `vb_link_tables` clears all four table frames before
  publishing the topology, so the "only 4 of 2048 entries present" invariant holds
  unconditionally even for never-yet-freed frames at first boot, independent of
  allocator history.
- **Audit strengthened** to a regression guard: `je_vm_audit` now rejects any
  PRESENT upper-table entry outside the four the build sets (PML4[0], PDPT[0],
  PDPT[1], PD[0]). A hosted test (test_vm `test_audit_rejects_stale_upper`) injects
  stale entries and proves the audit rejects them; verified it FAILS without the
  strengthening.

**Why not zero inside je_pages_free.** `je_pages` is a pure, hostable bookkeeping
module with no way to address frame BYTES (it has no phys→virtual map). The backend
is the lowest layer that can (`phys_ptr` identity VA), so free-time zeroing lives
there. The allocator contract is unchanged.

**Cost of alternatives.** (a) clearing only in `vb_link_tables` leaves user-data and
kstack frames carrying stale bytes into non-table reuse — closes only the
page-table case, not the disclosure class. Zeroing on ALLOC instead of free would
leak the dying process's bytes until the next allocation and duplicate work for the
heap path. (b)+(c)+audit was chosen as root-cause + belt-and-suspenders + a
standing check.

**Flex / revisit.** When a zero-on-free DMA or scrubbing-cost concern appears,
revisit whether a page-poisoning debug mode or a skip-zero fast path for
provably-not-reused frames is worth it. For SMP, the scrub must stay before the
frame re-enters the shared free list under the allocator lock.
