# W2 round 3 — tests/test_fdt.c vs contract 5a3c14b

Observed 2026-10-30, worktree `wt52-tests-r3`, branch `wip/52-tests-r3`, base commit
5a3c14b. Only `tests/test_fdt.c` modified (uncommitted). I did not read kernel/core/fdt.c.
This is AI-authored test code and has had no independent review.

## What I ran

- `make build/0/test_fdt`: compiles with no warnings (-Werror, -Wconversion, -Wshadow,
  ASan+UBSan). The ARM flags don't apply: this is a host-only test.
- `git diff --check`: clean.
- `ASAN_OPTIONS=detect_leaks=0 ./build/0/test_fdt`: aborts at the first failure
  (`test_fdt.c:3938 status_name(18) = "UNKNOWN", want "CONFLICT"`), rc 134. This is
  expected because the tree has the round-2 parser.
- Scratch copy without aborts: `/tmp/w2r3/test_fdt_na.c`, made by sed replacing `abort();`
  with a failure counter. It was built with the same flags as the Makefile and run with
  `ASAN_OPTIONS=detect_leaks=0`. It reported 79 FAIL lines and then UBSan aborted:
  `kernel/core/fdt.c:1098:21: runtime error: index 8 out of bounds for type 'je_range [8]'`
  in the new check() count-clamp case.
- Second scratch copy, `/tmp/w2r3/test_fdt_na2.c`: the same, but with only the clamp loop
  disabled so the rest of the suite runs. It was built without -Werror, because the
  disabling edit (`w < 0u`) triggers -Wtype-limits. Result: 79 FAIL lines (the same set),
  then the truncation sweep and both mutation loops pass. Against the round-2 parser:
  - fixture: 20000 iterations, 49388 byte-changing mutations, 1283 also truncated,
    4533 still OK;
  - rich blob: 10000 iterations, 24704 mutations, 641 truncated, 1021 OK.

  These counts will change with the round-3 parser.

## Failing cases (line numbers are in tests/test_fdt.c) and the contract change behind each

| Line | Case | Got (r2 parser) | Contract change |
|---|---|---|---|
| 3938-3939 | status_name(18) | "UNKNOWN" | new `JE_FDT_CONFLICT` enumerator |
| 1839 | root dup #address-cells, then a later prop with bad nameoff | BAD_STRING @later prop | DUPLICATE at the repeating PROP, as encountered |
| 1909 | memory: status "disabled", then a repeated status | NO_MEMORY | DUPLICATE "regardless of status" |
| 1922 | disabled memory node with reg twice | OK | same |
| 1934 | device_type "foo" then "memory" | OK | DUPLICATE "regardless of classification"; every depth-2 node is tracked |
| 1947 | uart@9000000 with reg twice | OK | every depth-2 node is TRACKED |
| 1957 | soc with #address-cells twice | OK | same |
| 1968 | chosen@0 with initrd-start twice | OK | same |
| 2082 | disabled rsv child with reg twice | OK | depth-3 rsv children tracked, any status |
| 2160, 2171 | /chosen device_type "memory" (enabled, disabled) | OK | new CONFLICT |
| 2185 | /reserved-memory device_type "memory" | OK | new CONFLICT |
| 2199 | disabled /reserved-memory, device_type memory, ranges absent | BAD_CELLS @END_NODE | CONFLICT at the PROP (earlier in file order) |
| 2325 (×12 status) | empty / unterminated status on a memory node (before and after device_type) and on a rsv child | NO_MEMORY / OK | BAD_STRING for a malformed consumed string value |
| 2345 (×3) | empty / unterminated device_type | OK | same |
| 2439 | disabled /reserved-memory, ranges absent, one child | BAD_CELLS err_off 272 (its END_NODE), want 232 (child BEGIN_NODE) | rsv cells decided at the first child BEGIN_NODE |
| 2733 (17 fails across the 21 cells×timing cases) | rsv cells absent/mismatch/ranges absent/non-empty, with a child (valid or malformed reg) | wrong err_off, or BAD_RANGE from the child first | same timing rule; a malformed child reg must not be reported first |
| 2750 | 1/1 rsv under a 2/2 root, one child | err_off at END_NODE | same |
| 2774 | rsv ranges absent, NOP, child containing a bad token | BAD_TOKEN | same (the decision at the child BEGIN_NODE comes before the bad token) |
| 2963 (k=0,1) | one reg with 17 ranges, the 17th invalid (zero size; overflow) | RANGE_LIMIT | "validity (BAD_RANGE) first, then count; count only VALID" |
| 3580 clamp loop | check() with ram_count 9 | UBSan OOB `ram[8]` in fdt.c:1098 | check() clamp: BAD_ARG, never index past the arrays |

Cases that already pass on the round-2 parser: status exact match (as a disabled value),
status ignored on /chosen (initrd still yielded), disabled /reserved-memory still yielding
reservations, disabled child not a candidate (17 with one disabled → OK), non-empty root
name → BAD_TOKEN at the root BEGIN_NODE, root-position name BAD_STRING, zero-size struct
at len → BAD_LAYOUT, zero-size strings at len < totalsize, second /chosen and second
/reserved-memory at the BEGIN_NODE, per-node scoping, and the untracked-node literal
reading.

## Review items A1–A7

1. **A1:** confirmed. An unterminated name, including the root-position cases newly added
   now that A20 is settled, expects BAD_STRING only.
2. **A2:** confirmed. Root-cells timing uses EXPECT_AT at the child BEGIN_NODE or the root
   END_NODE (test_bad_cells, test_zero_size_strings).
3. **A3:** confirmed. 17 candidates outside RAM → RANGE_LIMIT at the 17th reg PROP (exact).
   16 outside RAM → OK, which is the discriminating control.
4. **A4:** strings-side coverage confirmed. I added a struct-side comment and case: zero-size
   struct and zero-size strings, both starting at len, with totalsize > len → BAD_LAYOUT
   (the first token runs past the empty block), never TRUNCATED. The rsvmap can't be
   zero-size because it has no size field and its terminator is required.
5. **A5:** fixed.
   - A byte write equal to the old value becomes `~old`; a word write equal to the old
     value becomes `~old`; a word write that doesn't fit becomes a bit flip. Each one is
     checked with `CHECK(after != before)`.
   - Mutations only target bytes inside the parsed length.
   - If the mutations cancel out, one forced flip is applied.
   - Each iteration asserts `plen < len || memcmp(m, seed, len) != 0`.
   - Truncation now always picks a strict prefix (0..len-1).
   - Iterations, mutations, truncations and OKs are counted, and the PASS line prints them.
   - The determinism double-parse is kept.
6. **A6:** fixed. The header comment now states the exceptions: a len-0 call uses a 1-byte
   buffer, and test_bad_arg_parse passes NULL or a fixture buffer directly.
7. **A7:** fixed. `rm_child(name, ac, sc, base, size, nomap)` replaces the old version, and
   the 1/1 positive test uses it.

## Existing cases changed because the contract changed

- test_positive_memory: `status ""` is now BAD_STRING, so that case uses "OK", which is
  disabled under exact matching.
- test_positive_literal_scope: the uart with reg twice (now a DUPLICATE, since depth-2
  nodes are tracked) became a single reg plus status. The untracked-repeat reading moved
  into its own isolated positive case in test_duplicate.
- All DUPLICATE and /reserved-memory BAD_CELLS cases now pin an exact err_off.
- b_reg now allows up to 18 pairs.

## New ambiguities (not resolved silently; the literal reading is used or the case is avoided)

- **N1 — BAD_STRING scope for status/device_type.** The contract's "consumed string
  value" and the list of consumed names are global, but status is IGNORED on /chosen and
  /reserved-memory, and it is unclear whether the value is validated on non-memory depth-2
  nodes or untracked nodes. Tested only on: memory-node status (both orders relative to
  device_type), rsv-child status, and depth-2 device_type. Malformed status on /chosen,
  /reserved-memory, the root, a non-memory depth-2 node or a depth-3+ untracked node is
  NOT asserted.
- **N2 — err_off for a walk-time BAD_RANGE in a rsv child's reg.** The contract only gives
  an err_off for RANGE_LIMIT. I pinned BAD_RANGE to the reg PROP token, by analogy with
  rsv_off ("the token that supplied each range"). The lead should confirm.
- **N3 — DUPLICATE vs CONFLICT at the same PROP.** Example: /chosen with device_type "foo"
  and then device_type "memory". The order is unspecified, so this is not tested.
- **N4 — a repeat whose own value is malformed.** Examples: a second `status ""`, or a
  second #address-cells with value 3. Whether that is DUPLICATE or BAD_STRING/BAD_CELLS at
  the same PROP is unspecified. Avoided: every tested repeat carries a valid value.
- **N5 — untracked nodes.** Literal reading: a repeated consumed name in a depth-3 non-rsv
  node, or a depth-4 node under a rsv child, is NOT a DUPLICATE (asserted OK). A reviewer
  may argue this fails open. Flagging it for the lead.
- **N6 — disabled nodes, "structurally validated".** Whether a bad reg *length*
  (BAD_CELLS) in a disabled memory node or rsv child is still fatal is unclear. Not tested.
  Tested: zero-size and partial ranges are skipped, and a bad token inside the subtree is
  fatal.
- **N7 — "subtree skipped" for rsv children.** This isn't observable in MP1, because depth-4
  nodes are never semantic anyway. I asserted only that the subtree is still counted and
  structurally validated.
- **N8 — zero-size struct at len.** I read this as BAD_LAYOUT (the header says the block is
  empty, so a token running past it is a layout violation, not TRUNCATED).
- **N9 — check() clamp and ASan.** ASan cannot see a read of ram[8] or rsv[16] because it
  stays inside the struct; only UBSan -fsanitize=bounds catches direct indexing of the
  declared array. The case fills every slot with a valid entry so an unclamped checker
  really walks past the end, and it uses an exact-size heap copy so a 0xffffffff count runs
  off the object. This was observed: UBSan flagged the r2 checker at fdt.c:1098.

## Not verified

- I did not run the suite against a round-3 parser (none in this tree).
- I did not run `make check`, smoke tests or the ARM build: the suite is host-only, and
  the lead integrates.
- No remote CI.
