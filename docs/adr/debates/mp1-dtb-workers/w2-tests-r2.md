# W2 round 2 notes — tests/test_fdt.c updated to contract bdf446a (issue #52)

Observed 2026-10-30T00:11:37+00:00, worktree `wt52-tests-r2`, branch `wip/52-tests-r2`,
base commit `bdf446ad3ab4b96a560bd56da46b30587d67ceba`. Not committed. The only file edited is
`tests/test_fdt.c`; this file is the notes file. I did not read `kernel/core/fdt.c`. It was only
compiled and linked into the test binary.

State: **implemented**. The suite has been run against the round-1 parser; it has not been run
against the fixed parser.

## Changes (by contract item)

1. **/reserved-memory candidate bound (walk-time RANGE_LIMIT).**
   - `rm_child()` now returns the struct-relative offset of its `reg` PROP token. The buffer in
     `b_reg()` grew from 16 to 17 pairs.
   - Round-1 test "partial memreserve + 17 kept children → BAD_RANGE" (`test_postwalk_order`):
     now **RANGE_LIMIT with exact err_off = the 17th child's `reg` PROP** (`I.off_st + reg_at`).
   - New `test_rsv_candidates()` (called from `test_range_limit`). Each case gives an exact
     err_off:
     - 17 candidates inside RAM → RANGE_LIMIT.
     - 17 candidates **all outside RAM** → RANGE_LIMIT. This is the candidate bound, not the
       kept bound.
     - 17 **partial** candidates → RANGE_LIMIT. The walk-time check beats the post-walk
       BAD_RANGE.
     - 17th candidate followed by an unknown token → RANGE_LIMIT, because the earlier violation
       in file order wins.
     - **Exactly 16 candidates outside RAM → OK, `rsv_count == 0`, `rsv_ignored == 16`.**
     - Counting is per range: one `reg` with 17 pairs → RANGE_LIMIT at that PROP; 16 pairs plus
       one more child → RANGE_LIMIT at the second `reg`; 16 pairs → OK with 16 ignored.
     - Things that are not candidates: 16 memreserve entries (outside RAM) + 16 candidates
       (outside) + 1 status-disabled child with `reg` + 1 size-only child → OK with
       `rsv_ignored == 32` and `rsv_dynamic == 1`.
     - 16 candidates, half kept and half ignored → OK with 8/8.
   - `test_postwalk_order`:
     - "memreserve before /reserved-memory" is kept by using only 16 candidates: a partial
       memreserve plus 16 kept children → BAD_RANGE.
     - Added: 1 kept memreserve + 16 kept children → post-walk kept-bound RANGE_LIMIT. A partial
       memreserve placed first still wins → BAD_RANGE.
     - "Reservations before initrd" now uses the post-walk kept bound (16 memreserve + 1 child).
       A walk-time variant with exact err_off was added.
   - The existing kept-bound case (16 memreserve + 1 child → RANGE_LIMIT) is unchanged. Its
     err_off is post-walk and not specified, so it is not asserted.
2. **A1, unterminated name → BAD_STRING only.** `test_unterminated_name` no longer accepts
   BAD_LAYOUT. It has three cases, each `EXPECT_IN_ST(BAD_STRING)`:
   - the struct block ends inside the name, with the strings block (which contains NULs)
     directly after it;
   - the same with an 8-byte gap of non-NUL bytes before the strings block;
   - a 295-byte name with no NUL (both unterminated and oversized).
3. **Other two clarifications.**
   - Root cells absence (`test_bad_cells`), each with an exact err_off:
     - only `#size-cells` → first child's BEGIN_NODE;
     - only `#address-cells` → first child's BEGIN_NODE;
     - neither cell property, plus `compatible` and NOPs before the child → the child's
       BEGIN_NODE (NOPs do not move it);
     - root with `compatible`, and a child whose first inner token is unknown → BAD_CELLS at
       the child's BEGIN, not BAD_TOKEN;
     - a memory child with a 4-byte `reg` → BAD_CELLS at the child's BEGIN, not at its reg/END;
     - childless root → its END_NODE, in three variants: no props; `#address-cells` + NOP;
       `#size-cells` with NOP after END_NODE.
     - Positive control: cells present and childless → NO_MEMORY.
   - Zero-size blocks: new `test_zero_size_strings()` (called from `test_layout`) and
     `size_dt_struct = 0` → BAD_LAYOUT. **Observation:** a blob with an empty strings block
     can never parse OK. No PROP can have `nameoff < 0`, so the root cannot have its cells.
     The "positive" cases therefore prove that header, layout and truncation all pass by
     requiring the next rule's exact result:
     - empty strings at struct end == totalsize == len → BAD_CELLS at root END_NODE;
     - the same with a child → BAD_CELLS at the child's BEGIN;
     - empty strings after a gap, exactly at totalsize == len → BAD_CELLS at END_NODE;
     - **empty strings exactly at `len`** with totalsize = len+64 → BAD_CELLS at END_NODE,
       and with a 12-byte gap and totalsize = len+4096 → BAD_CELLS at the child;
     - a PROP with empty strings → BAD_STRING in the struct block.
     - Negative control: the empty block starting at totalsize+4 → BAD_LAYOUT.

## What I ran (exact commands, results)

1. `rm -f build/0/test_fdt; make build/0/test_fdt` → compiled with **no warnings or errors**:
   `gcc ... -std=c11 -O1 -g -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes
   -fsanitize=address,undefined ... tests/test_fdt.c kernel/core/fdt.c`.
2. `./build/0/test_fdt` (round-1 parser) → exit 134. First failure:
   `tests/test_fdt.c:1132: want BAD_CELLS got BAD_LAYOUT (err_off 0)`.
3. To list **every** failing assertion, I made a scratch copy, `/tmp/w2r2/t.c`, which is
   `tests/test_fdt.c` with `abort()` in FAILF/want_ replaced by a counter. That inserts 1 line,
   so its line numbers are **+1** relative to the real file. I compiled it with ASan/UBSan
   against the same parser and ran it. The run completed, including the truncation sweep and
   30000 mutations (4708/1068 OK, the same split as round 1). There were no sanitizer reports.
   The failures, with real-file lines (scratch − 1):

   | real line(s) | case | got | maps to |
   |---|---|---|---|
   | 1132, 1142, 1151 | empty strings at totalsize (== len) | BAD_LAYOUT, err_off 0 | known bug: zero-size strings at totalsize (item 3) |
   | 1160, 1171 | empty strings exactly at `len` < totalsize | TRUNCATED (err_off 72 / 96) | item 3 "zero-size block at the end of len" — same bug family |
   | 1184 | PROP with empty strings | BAD_LAYOUT | same zero-size-strings bug |
   | 1978 | childless root, no props at all | BAD_LAYOUT | same bug (an empty strings block is a side effect of having no props) |
   | 1929 | root has only `#address-cells` | **BAD_RANGE** at 108 | known bug: root #size-cells absent → BAD_CELLS (item 3) |
   | 1919 | only `#size-cells` | BAD_CELLS but err_off 140 (want 80) | item 3: absence decided late |
   | 1943 | neither + NOPs | BAD_CELLS err_off 116 (want 88) | item 3 |
   | 1954 | no cells + child with unknown token | **BAD_TOKEN** at 88 (want BAD_CELLS at 80) | item 3: decision point must be the child's BEGIN_NODE |
   | 1966 | only `#address-cells` + child with 4-byte reg | BAD_CELLS err_off 108 (want 80) | item 3 |
   | 2177, 2188, 2200, 2213, 2626, 2695 | 17 candidates | RANGE_LIMIT ✓ but err_off = reg PROP **+28** | item 1: err_off must be the `reg` PROP token |
   | 2243 | one reg with 17 pairs | RANGE_LIMIT ✓, err_off 512 (want 228 = PROP; 512 = PROP + 12 + 17·16) | item 1 |
   | 2256 | 16-pair reg + second child | RANGE_LIMIT ✓, err_off 536 (want 508) | item 1 |

   Every failure maps to items 1 or 3 and to the two known parser bugs. **Item 1 note for the
   parser author:** the round-1 parser already returns walk-time RANGE_LIMIT, so the status
   agrees in every candidate case. Its err_off, however, is the offset just past the `reg`
   property (the cursor after the value), not the PROP token the updated contract names. This
   is a third fix, separate from the two known bugs. The candidate-count/OK cases already pass:
   16 ignored, per-range counting, and disabled/dynamic/memreserve not counted.
   **Item 2:** all three unterminated-name cases already pass (the parser returns BAD_STRING).
   The childless-root cases that have props (lines 1986 and 1994) already pass with exact
   END_NODE err_off.
4. `git diff --check` → clean. `git diff --stat` → `tests/test_fdt.c | 485 +++++---` (only
   that file).

## Not verified

- I did not run the fixed parser, because I don't have it. Round-1 parser only.
- ARM flags: not applicable, since this is a hosted test. `aarch64-linux-gnu-gcc` exists, but
  I did not build anything for ARM. No QEMU, no `make check` / `make smoke`, no CI.
- clang was not run.
- I did not rerun the mutation/self-consistency check against my round-1 scratch model.

## Ambiguities (new; literal reading implemented, nothing worked around)

- **A20.** An unterminated name in the **root** position: BAD_STRING vs "first node not an
  empty-named root" (BAD_TOKEN) is not ordered by the contract. **Not tested**; there is a
  comment in `test_unterminated_name`.
- **A21.** A zero-size block starting **beyond** `len` but ≤ totalsize. Literally it has no
  byte at/after len, so it is not TRUNCATED. I asserted only "exactly at len", the lead's
  wording, and did not test beyond len. A zero-size block with totalsize < len is not
  exercised, since the glue never copies more than totalsize.
- **A22.** What counts as a "candidate". Implemented as: one {base,size} range of an
  **enabled** depth-3 child's `reg`. Status-disabled children, size-only children and
  memreserve entries do not count. The contract says "candidate range" and "status-disabled
  children skipped", which supports this, but it is my reading. A parser that counts a disabled
  child's reg fails the "not candidates" case.
- **A23.** Zero-size strings can never be OK (see item 3 above). If the lead intended an OK
  positive, that is impossible under the contract. The suite uses the next-rule proof instead.
- Ordering of a 17th candidate whose range is itself invalid (zero size / overflow) is
  deliberately **not tested**: RANGE_LIMIT vs BAD_RANGE is not ordered.
