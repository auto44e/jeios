# W2 round 6 — tests (tests/test_fdt.c only)

## What changed
Added a round-6 block at the end of `test_bad_cells()` in tests/test_fdt.c:
- Length-8 cell values whose FIRST word is valid (=2, equal to root 2/2): `{2,0}` and `{2,2}`.
  (The pre-existing root len-8 case used `{0,2}`, whose first word is already invalid, so it
  did not discriminate a `len == 4` -> `len >= 4` mutant.)
- ROOT #address-cells / #size-cells len 8 -> BAD_CELLS, err_off = that PROP token
  (contract: "only the root rejects bad cell values at the PROP").
- /reserved-memory #address-cells / #size-cells len 8 under a 2/2 root:
  with one child (valid 2/2 reg) -> BAD_CELLS at the child BEGIN_NODE;
  childless -> BAD_CELLS at /reserved-memory END_NODE.
- Positive control: same tree with len-4 <2>/<2> cells and a child -> OK.
Total new assertions: 2 values x (2 root + 4 reserved-memory) + 1 = 13.

## What I ran
- `ASAN_OPTIONS=detect_leaks=0 make build/0/test_fdt && ./build/0/test_fdt`
  -> builds with -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes + ASan/UBSan; `fdt: PASS (...)`.
- `git diff --check` -> clean.

## NOT verified
- I did NOT apply the `value_len >= 4u` mutant to kernel/core/fdt.c to confirm it is now
  killed (role forbids reading that file). Lead should confirm by mutating and re-running.
- Did not run `make check`, smoke, or ARM cross builds (test file is host-only).

## Contract concerns
None. Root PROP-level err_off pin follows the literal fdt.h text.
