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
