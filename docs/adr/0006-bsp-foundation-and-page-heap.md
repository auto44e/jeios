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
