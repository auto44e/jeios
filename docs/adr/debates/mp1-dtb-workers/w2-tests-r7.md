# W2 round 7 — tests (tests/test_fdt.c only)

## What changed
Two new functions in tests/test_fdt.c, called from main() right after
`test_dup_before_value()`:

### `test_dup_before_value_r7()` (Codex finding 1)
Every case: first instance valid (or first malformed where the value is decided
later), repeat malformed -> `EXPECT_AT(DUPLICATE, repeat PROP)`. Bad values: len 0
and len 12.
- /chosen `linux,initrd-start` valid (4 and 8 bytes) then repeated bad, -end after.
- /chosen `linux,initrd-end` valid (8 bytes) then repeated bad, -start before / after.
- /chosen only -start, repeated bad (repeat beats END_NODE "exactly one").
- memory `reg` valid then repeated bad, device_type before / after.
- memory `reg` bad then valid repeat (reg decoded at END_NODE).
- rsv child `reg` valid->bad and bad->valid.
- /reserved-memory `ranges` empty->non-empty(4B) and non-empty->empty, with a child
  and childless.
- rsv child `size` 8B -> empty / 4B; `no-map` empty -> 4B; each on an okay and a
  `status="disabled"` child (DUPLICATE regardless of status).

### `test_special_devtype_scope()` (Codex finding 2)
device_type `{'o','k','a','y'}` len 4 and len 0 -> `EXPECT_AT(BAD_STRING, that PROP)`:
- /chosen: device_type first / after a valid initrd pair / alone in a childless node.
- /reserved-memory: device_type first (before cells) / after cells+ranges with a
  child / alone in a childless node with cells missing (BAD_CELLS would come later
  at END_NODE).
- Positive control: terminated `"okay"` device_type on both special nodes -> OK,
  initrd and reservation still recorded.

## What I ran
- `ASAN_OPTIONS=detect_leaks=0 make build/0/test_fdt && ./build/0/test_fdt`
  -> built with -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes +
  ASan/UBSan; `fdt: PASS (...)`, rc=0.
- `git diff --check` -> clean.
- Mutant kill check: compiled `tests/test_fdt.c` against the reviewer's
  `/tmp/mut_dup_initrd_bad.c`, `/tmp/mut_dup_reg_empty.c`, `/tmp/mut_scope.c`
  (I did not read them or kernel/core/fdt.c) into /tmp/w2r7/, then ran each:
  - mut_dup_initrd_bad: `FAIL tests/test_fdt.c:3033: want DUPLICATE got BAD_RANGE`
  - mut_dup_reg_empty:  `FAIL tests/test_fdt.c:3080: want DUPLICATE got BAD_CELLS`
  - mut_scope:          `FAIL tests/test_fdt.c:3202: want BAD_STRING got OK`
  All three now killed.

## NOT verified
- Did not run `make check`, smoke, or ARM cross builds (test file is host-only).
- Did not write new mutants for ranges/size/no-map value-first behavior; those cases
  are covered by assertions but no mutant was run against them.
- The initrd bad-length TIMING (PROP vs END_NODE) is not pinned by the contract, so I
  did not test "bad first initrd, valid repeat"; only valid-first cases are asserted.

## Contract concerns
None. All expectations follow the literal fdt.h text (DUPLICATE decided first at a
PROP; device_type consumed on every depth-2 node, checked at the PROP).
