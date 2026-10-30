# W2 round 5 — tests/test_fdt.c (worker notes, not committed)

Observed 2026-10-30, worktree wt52-tests-r5, branch wip/52-tests-r5, base a4712a8.
Only file edited: tests/test_fdt.c. kernel/core/fdt.c was NOT read. fdt.h unchanged.

## What changed
1. `EXPECT_BAD_NAMEOFF_AT(prop_rel)` is now exact: `EXPECT_AT(BAD_STRING, off_st + prop_rel)`
   (was "anywhere in the 12-byte PROP header"). Moved next to EXPECT_AT so earlier tests use it.
   Also tightened previously loose nameoff cases to the exact PROP token:
   - test_zero_size_strings: PROP with nameoff 0 into an empty strings block (was EXPECT_IN_ST).
   - test_strings: nameoff == size_dt_strings and 0xffffffff (was status-only).
   - test_strings: no NUL before strings-block end (both variants) (was status-only).
     Literal reading: "no NUL inside the strings block" is a bad-nameoff failure, so it pins to
     the PROP whose name runs off the block (all earlier names are terminated).
2. test_bad_range: memory node without reg -> BAD_RANGE at its END_NODE; exactly one
   initrd property (start-only, end-only) -> BAD_RANGE at /chosen's END_NODE.
3. New test_end_node_pins() (run after test_reg_timing):
   - memory no-reg: END_NODE of the memory node with valid "okay"/"ok" status, trailing
     unconsumed prop, or a child subtree (not the child's END_NODE); beats a later sibling's
     bad nameoff and post-walk NO_MEMORY; an earlier subtree bad nameoff wins.
   - one initrd (both start-only and end-only): /chosen's END_NODE with a trailing prop and a
     child subtree; beats a later sibling bad nameoff; beats NO_MEMORY (no memory node, and a
     memory node after /chosen); earlier subtree bad nameoff wins; repeat -> DUPLICATE.
   - memory END_NODE order, status before/after device_type:
     (1) malformed (empty) status beats zero-size reg, overflow reg, 12-byte reg, absent reg;
     (2) valid "disabled" skips before reg-absent/reg-decode: sole node -> NO_MEMORY, with
         another memory node -> OK (no reg / zero-size / 12-byte reg);
     (3) valid "okay" + no reg -> END_NODE (not status PROP);
     (4) reg decode deferred to END_NODE: subtree bad nameoff and a later DUPLICATE beat a
         zero-size / 12-byte reg; control: same reg alone -> BAD_RANGE/BAD_CELLS at reg PROP.
   - depth-3: empty / non-NUL / "memory" device_type on a rsv child -> OK, still one kept
     reservation (kept the existing test_unconsumed_strings case too); repeated device_type on
     a rsv child (tracked) -> DUPLICATE at repeat; repeated on an untracked depth-3 node -> OK.

## Commands run (this worktree, host gcc 13.3.0, Ubuntu 24.04)
- `ASAN_OPTIONS=detect_leaks=0 make build/0/test_fdt && ./build/0/test_fdt` -> build clean
  (-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes, ASan+UBSan), `fdt: PASS`.
- `ASAN_OPTIONS=detect_leaks=0 make check` -> exit 0 (all suites incl. test_fdt PASS, ELF checks PASS).
- `git diff --check` -> clean.

## Parser/contract mismatches found
None: every new/tightened assertion passes against the round-4 parser in this tree.

## Not verified
- No ARM build of the test (it is a hosted test; ARM flags apply to kernel code, not touched).
- Not run: make smoke / integration / remote CI. No mutation of the parser to prove each new
  assertion discriminates (I was told not to look at fdt.c); discrimination is by construction
  (the wrong candidates — child END_NODE, PROP, later sibling, post-walk status — are distinct
  offsets/statuses).

## Contract notes (no workaround applied)
- "no NUL inside the strings block" being covered by the round-4 "bad nameoff -> PROP token"
  pin is my literal reading; if the lead intends otherwise, test_strings' two no-NUL cases are
  the ones to relax.
- The contract pins only the one-present initrd case to END_NODE; end<=start and bad-length
  err_off are left unasserted (status-only), as before.
