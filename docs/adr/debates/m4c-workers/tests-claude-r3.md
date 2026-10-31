# WORKER_NOTES — M4 PR-C test writer, round 3 (issue #53, ADR 0010 D5/D6)

Observed 2026-10-31 (UTC, same day as HEAD commit 11:14:52Z; exact wall time not recorded),
worktree `wt-m4c-tests`, branch `wip/m4c-tests`, HEAD `36cba32`. Not committed.
Authorship: a single AI agent (Claude), working as the TEST WRITER. This is AI-assisted
work, not independent human review. Input review: `/tmp/m4c/f-codex.md` (Codex, AI).

State: implemented + tested locally (hosted only). Files changed: `tests/test_uframe.c`,
`tests/test_syscall.c`. No header, kernel or doc file was touched.

## Changes

### P2-1 — `tests/test_uframe.c`: invalid frames leave `*out_rflags` untouched
- I added a `reject(f, base, size)` oracle. It seeds `o = 0xa5a5a5a5a5a5a5a5`, asserts that
  `je_uframe_check` returns false, then asserts `o` still holds the sentinel. The contract
  is uframe.h:28-29: "On any violation return false and leave *out_rflags untouched."
- All 13 invalid cases now go through it:
  - RIP base-1 and top;
  - RSP top+1 and base;
  - the 4 selector cases;
  - the 2 non-canonical cases;
  - size 0;
  - NULL frame;
  - the startup-v1 frame with RIP=TOP.
- The `out == NULL` case keeps its plain boolean assert, because it has no output to check.
- **Decision:** I used a nonzero, non-flag-shaped sentinel. A sentinel of 0 would miss a
  mutant that writes 0, and a sentinel of 0x202 would miss a mutant that writes the
  sanitized value. Rejected alternative: a per-call-site sentinel check inlined 13 times,
  which is noisier and easier to forget on a new case.

### P3 — `tests/test_syscall.c`: decode-OK cases cite the exhaustive list
- I kept the decode-OK cases unchanged:
  - RDI/RSI = 0xffffffff;
  - RDI = 0;
  - grants pointer all-ones.
- I added a comment that cites `include/jeios/abi.h:154-157`. Those lines say the reject
  list is EXHAUSTIVE, and that launcher, image_id and pointer validity are glue/spawn checks.

### spawn.h "n > effective child handle quota -> INVALID" is not a decode check
- New decode-OK cases:
  - n=2 with handle quota 1;
  - n=4 (GRANTS_MAX) with handle quota 1.
- They come with a comment: the check is spawn step 3, it is absent from the exhaustive
  decode list, and the spawn-level INVALID is proven by the QEMU fixture, not by this test.
- **Decision:** I added n=4 as well as n=2, so that a decode mutant that only checks
  `n > quota+1` or similar is also caught. I did NOT add a "quota 0 (default 15) with n=4"
  case here, because it would be OK under both readings and so proves nothing about where
  the check lives.

## Commands run and results
- `ASAN_OPTIONS=detect_leaks=0 make test > /tmp/m4c-tests-r3.log 2>&1` → rc=0, every
  suite PASS:
  - core, boot, alloc, vm, uframe, syscall;
  - handle, both production 16/16 and reduced-width;
  - endpoint, proc, proc_retire, fdt.
- `git diff --check` → clean.
- Mutation check, done out of tree in `/tmp/m4c-mut-r3`; the tree is unmodified:
  - I made a copy of `kernel/core/uframe.c` with `*out_rflags = 0;` inserted before the
    selector check, which is after the NULL guard.
  - `test_uframe` against the mutant aborted at `reject: Assertion 'o == SENTINEL'` (rc=134).
    So the mutant is killed.
  - Against the original it PASSes.
  - Note: the literal Codex mutant (writing before the `size == 0` reject, on the same line
    as the NULL guard) would dereference NULL in the `out == NULL` case. I therefore placed
    the write after the guard.

## NOT verified / open
- I did not run `make check`, `make smoke` or the QEMU fixture. The spawn-level
  `n > quota -> INVALID` and rollback baselines rest on the lead's fixture run.
- Codex P2-2 (construction of the startup frame, zero-fill beyond n, grant order, rollback
  baselines) is still not covered by hosted tests. It is out of scope for this round, and
  only a kernel fixture or a constructor seam can cover it. That would need non-test files.
- No remote CI was observed.
