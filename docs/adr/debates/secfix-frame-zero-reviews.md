# Security fix review: zero physical frames on free

ADR 0000 part 4 attachment for the zero-on-free fix (ADR 0006 security note).
AI-assisted adversarial review across two lineages (Claude + Codex). NOT
independent human qualification. This touches tested M1/M2 backend code and was
escalated to and approved by the owner (fix choice b+c).

## Both lineages confirmed sound
- CODE/RODATA identity aliases are restored to RW (J_DEMOTE→restore_alias)
  BEFORE the extent is freed (J_EXTENT→scrub), because the journal unwinds in
  reverse and DEMOTE is pushed after the extent — so scrub never writes a
  read-only alias and cannot fault the kernel. (Lead verified independently.)
- Table-extent teardown order (J_TABLES drop + flush, then J_EXTENT scrub+free)
  means table frames are zeroed only after they are no longer the active CR3 and
  the TLB is flushed; only the four owned frames are zeroed, never the shared
  kernel PD.
- `vb_link_tables` clears the correct four frames before writing the four live
  entries; indexing and order are right.
- The audit allow-list (PML4[0], PDPT[0], PDPT[1], PD[0]) is exactly right for the
  2 MiB arena (one PD entry, one PT); a `_Static_assert(JE_VM_ARENA_SIZE <=
  0x200000)` now pins it so it can't silently go stale.
- No observable allocator behavior change beyond zeroing; `free_count`/run headers
  unchanged. Added cost is bounded (≤ ~73 pages × 512 stores per destroy, IF=0).
- No hosted ASan interaction: the real backend is kernel-only; hosted tests use
  the fake.

## Acted on
- **Narrowed the claim (both, important).** `je_heap_*` and kernel-stack frees go
  through `je_pages_free` directly and are NOT scrubbed by the backend. The claim
  is now "no VM-owned frame, and no page-table frame, carries stale bytes into
  reuse" — the present-stale-PTE disclosure path is fully closed; the heap zeroes
  on alloc; a kstack frame reused as a page table is caught by the link clear +
  audit. ADR note and comments updated to say exactly this.
- **Real-backend coverage (both, important).** `je_vm_audit` is now also called on
  the ring-3 PROC_SPAWN path (spawn.c, after the build, failing closed via
  spawn_undo). The guard therefore runs against the REAL backend on every child
  spawn (default boot spawns c_hello; PR-F spawns many), so a scrub/clear
  regression fails closed in practice, not only in boot self-tests. The hosted
  test proves the audit logic; the spawn audit proves it runs on the real path.
- **Range guard (Claude #3).** `vb_scrub_frames` bounds-checks (≥1 MiB, within
  identity RAM, page-aligned) before zeroing, since `je_pages_free` validates
  ownership only after it would return; a corrupted extent now panics instead of
  zeroing arbitrary memory.
- **Comment math (both).** Fixed "2044 others" → the 1532 other upper entries; the
  512 PT leaves are covered by the arena walk.

## Noted, not changed (with reason)
- Claude #4: `je_sched_reap` (ZOMBIE user slot, no guard restore) is unreachable
  today (user threads go DYING, not ZOMBIE); if ever reached, release_hold's scrub
  would #PF on the still-non-present kstack guard page instead of silently
  reusing it. Recorded as an ordering requirement; failing loudly is strictly
  better than the pre-fix silent leftover.
- A dedicated poison→free→realloc→verify QEMU fixture (mode 13) was considered;
  the spawn-path audit on every child is stronger standing coverage, so it was
  not added. Recorded honestly as the coverage basis.

## Verification after fixes (local)
`make check` (vm suite incl. stale-upper rejection; verified it FAILS without the
audit strengthening), `make smoke`, `make integration` (11 fault modes), NX ×4,
ARM64 base — all PASS. Local only; remote CI on the PR.
