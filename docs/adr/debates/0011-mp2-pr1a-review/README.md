# PR-1a cross-lineage code review — transcript and resolution

AI-assisted cross-lineage review of MP2 PR-1a (aarch64 physical allocator, MMU
off). Codex implemented the kernel path; Claude wrote the tests + oracle; then
each lineage reviewed the OTHER's output adversarially.

- `01-claude-kernel-review.md` — Claude reviewing the kernel code (physmem.c,
  main.c wiring, alloc.c/boot.h/alloc.h window change, mk).
- `02-codex-tests-review.md` — Codex reviewing the tests + oracle
  (test_physmem.c, smoke_arm.py --phys-alloc, Makefile).

## Both lineages independently flagged the same top gap

**No hosted allocator test at the aarch64 nonzero base** (Claude #3, Codex #2):
test_physmem.c static-asserts base 0, so the base-relative `BASE+i*PAGE` mapping
and the out-of-window `(a-BASE)>=SPAN` wrap-reject in je_pages_free — the heart
of the owner-approved tested-path change — never ran under ASan. A base-handling
regression would pass everything. FIXED: added `tests/test_physmem_arm.c`, built
with `-DJE_ARCH_AARCH64`, driving je_pages over the ARM window and asserting the
first allocation returns BASE (not 0), out-of-window/misaligned/window-crossing
frees return INVALID with state unchanged, exhaustion, and NOMEM-leaves-state.

## Other findings and resolutions

- `range_intersects_page` fails OPEN on a reserved range whose base+size wraps
  past 2^64 (Claude #5) — unreachable in-boot (parser rejects overflow) but the
  header promises totality. FIXED: overflow branch treats it as covering [base,∞)
  (fail safe) + a `phys` top-page guard in je_arm_page_free.
- Docs described the removed `rollback ok` marker (Claude #2). FIXED: HANDOFF +
  IMPLEMENTATION rewritten for the honest `allocmax` / `overcap` markers.
- "BYTE-IDENTICAL" overclaim for x86 (Claude #4, Codex #6): the TU gains two new
  symbols; alloc/free/heap CODE is identical at -O2 but the object is not. FIXED:
  reworded to "behavior-identical; alloc/free/heap code identical at -O2".
- A missing `-DJE_ARCH_AARCH64` would silently pick the x86 window on an aarch64
  TU (Claude #7, ODR/ABI hazard). FIXED: `#error` guard on
  `__aarch64__ && !JE_ARCH_AARCH64` in boot.h.
- No degraded-profile report for RAM beyond the window (Claude 8, AGENTS rule 5).
  FIXED: a `physmem unmanaged ram bytes=<hex>` marker; the oracle recomputes it
  independently; verified non-zero at -m 512M (0x10000000 unmanaged).
- CI never ran the oracle (Claude #1). FIXED: added an `ARM_PHYSALLOC=1` CI step.

## Accepted-as-is, with rationale recorded

- The smoke oracle uses `any()` RAM membership and trusts kernel-emitted rsv
  values (Codex #1, #5). At the pinned -m 256M fixture there is ONE RAM range and
  ZERO FDT reservations, so neither exactly-one-membership nor independent-rsv
  parsing is exercisable in smoke. The hosted test_physmem.c covers exactly-one
  membership, no-map, and multi-reservation exclusion directly. Recorded as a
  fixture limitation in HANDOFF; a multi-range / with-reservation ARM fixture is
  a reasonable later addition.
- `overcap` only exercises the argument-bound check; its value is the end-state
  free_count the oracle pins (Claude #6). Kept, honestly labeled a bounds check,
  not a rollback claim; the rollback property is proven in the hosted test.
- `alloc1` returns 0x40000000 (QEMU's dead boot stub) — memory no DTB entry
  proves free but which is dead after kernel entry (matches Linux). Recorded as
  an accepted assumption.

Nature: AI-assisted multi-agent review. NOT the independent human qualification
the critical profile requires; no remote CI observed at authoring time.
