# W2 worker notes — tests/test_fdt.c (issue #52, hostile hosted suite)

Observed 2026-10-29 (local time zone of the dev host; exact clock time not recorded),
worktree `wt52-tests`, branch `wip/52-tests`, base commit `a55c2ff`. Not committed.

## What I built (state: implemented; tested only against the stub and a scratch model)

`tests/test_fdt.c` (only file edited), written from `include/jeios/fdt.h` alone. I did
not look at any real parser.

- **In-test FDT builder**: big-endian header/rsvmap/struct/strings, string offset
  reuse including dtc-style suffix sharing, optional gaps between blocks, an option to
  omit the rsvmap terminator, and header/struct word patching for single-defect blobs.
- **Exact-size heap copy**: every `je_fdt_parse` input is `malloc(len)`-copied
  (`parse_bytes`), so ASan catches any read at or after `len`. `*out` is pre-poisoned
  with 0xA5 and `*err_off` with 0xDEADBEEF. Every call checks: status is a valid
  enumerator; failure → `*out` fully zeroed (field-wise); OK → `err_off == 0`.
  `copy_plan` headers are exact 40-byte mallocs too.
- **Fixture** (item 2): the exact context as specified, plus `node_count` and
  `max_depth` checked against an independent in-test token walker (56 / 6), and
  `ram_off[0]` inside the struct block. The same ctx must come back with zero padding
  present (16 KiB and exactly `JE_FDT_MAX_COPY`). totalsize = 2 MiB → OK,
  2 MiB+1 → TOO_LARGE, == used → OK, used−1 → BAD_LAYOUT.
- **Positive** (item 3): all items requested, plus mixed 2/1 and 1/2 cells, the
  largest non-overflowing range, adjacent RAM, NOPs everywhere, block gaps with garbage,
  rsvmap at 48, the 16-entry rsvmap / 16-kept / 8-RAM / depth-16 / 1024-node /
  64-prop / 255-byte-name boundaries, and `addr==0,size!=0` treated as an entry
  rather than the terminator.
- **Negative** (item 4): at least one exact-status fixture for every status except
  OK, covering each rule listed in the header. Also tests check order for the header
  checks, the struct pass (first violation in file order) and the post-walk phase
  (NO_MEMORY → OVERLAP → memreserve → /reserved-memory → initrd).
- **copy_plan** (item 5), **boot_ctx_check** (item 6, using hand-built contexts so it
  does not depend on `parse()`, plus one real-parse round trip), **names/CRC**
  (item 8; also `crc32(len 0) == 0` and `crc32("1") == 0x83DCEFB7`).
- **Robustness** (item 7): truncation sweep over every `len` in `[0, 7426)`, which
  must return exactly TRUNCATED (see A5), with `err_off == 0` when `len < 40`. A
  fixed-seed LCG mutation loop runs 20000 mutations of the fixture plus 10000 of a
  rich builder blob: 1–4 byte/bit/word mutations, a quarter of them biased into the
  header, and 1/16 with a random length. Each blob is parsed twice and must give an
  identical status, `err_off` and ctx. Every OK result must also satisfy the
  contract-derived invariants: cells in {1,2}, bounds, valid and non-overlapping RAM,
  kept reservations and the initrd each inside one RAM range.

## What I ran (exact commands, results)

1. `make build/0/test_fdt` → compiled with **no warnings** (gcc, -Wall -Wextra -Werror
   -Wconversion -Wshadow -Wstrict-prototypes, ASan+UBSan).
2. `./build/0/test_fdt` against the in-tree STUB → **fails as expected**. First failing
   assertion: `fdt: FAIL tests/test_fdt.c:2721: status_name(0) = "UNKNOWN", want "OK"`
   (`test_names_crc` runs first). I then fixed only the names in a scratch copy of the
   stub (`/tmp/w2model/stub2.c`). The next failure is the stub's CRC:
   `test_fdt.c:2734: je_fdt_crc32(p, 9u) == 0xCBF43926u`.
3. Extra: `gcc ... -O2 -Werror ... -c tests/test_fdt.c` → clean. clang is not installed,
   so I did not run it.
4. **Scratch reference model** (my own extra step): `/tmp/w2model/model.c`, outside the
   worktree, never committed, written by me from my reading of the header. The suite
   **PASSES** against it: `fdt: PASS (... 30000 mutations: 4708/1068 OK)`. I then ran
   about 35 single-line mutants of the model (rsvmap limit, addr0-as-terminator,
   non-empty root, name/depth/node/RAM/RSV off-by-ones, PROP-after-child, second
   root, END-not-last, u32 wrap, strings truncation, unzeroed out, status ignored,
   memory duplicates, missing NO_MEMORY/OVERLAP, partial reservation ignored, initrd
   checks, check() order/lowest-i/pa bound, copy_plan min/<40, CRC, name OOB). All
   were killed except `nameoff >= size` → `> size`. That one is an equivalent mutant:
   the NUL search still fails, so the status is still BAD_STRING. **Caveat:** the
   model and the suite share one author, so a misreading common to both would not
   show up here. This proves the suite is self-consistent and live, NOT that it
   matches the real parser.

## Not verified

- The real parser (`kernel/core/fdt.c` from the other worker) has not been run
  against this suite.
- ARM build flags: not applicable, because this is a hosted-only test file. Nothing was
  run on ARM.
- No CI was run, local or remote.

## Contract ambiguities and how I read them (literal reading)

A1. **Unterminated node name at the end of the struct block.** The header lists both
    BAD_STRING ("node name unterminated") and BAD_LAYOUT ("a ... name ... running past
    the struct block"). `test_unterminated_name` **accepts either**. The contract
    owner should pick one.
A2. **err_off.** The header defines it only as 0 for header checks and 0 on success.
    The suite asserts: exact 0 for BAD_ARG, TRUNCATED(len<40), BAD_MAGIC, TOO_LARGE
    and BAD_VERSION; the **exact token-word offset** for BAD_TOKEN (unknown token,
    first-token-not-BEGIN, second root, PROP after child, END_NODE at depth 0, END
    with depth, trailing token); **within the struct block** for non-empty root name,
    BAD_STRING name-too-long, the *_LIMITs and DUPLICATE; **within the rsvmap** for
    rsvmap BAD_RANGE. It is not asserted for BAD_LAYOUT, TRUNCATED(len≥40),
    nameoff BAD_STRING, BAD_CELLS or post-walk errors. If the parser reports the
    name byte instead of the token for "non-empty root", the range check still passes.
A3. **"Zeroed" *out.** Checked field by field, including every array slot, with
    `has_initrd` read as a byte. Struct padding bytes are not checked.
A4. **BAD_ARG.** When the other pointers are non-NULL, `*out` must be zeroed and
    `*err_off` must be 0 ("Zeroes *out first"). `NULL` blob with `len 0` → BAD_ARG
    (the NULL check comes before the len check).
A5. **Layout before truncation.** "then layout → BAD_LAYOUT ... Any block byte at/after
    len → TRUNCATED" is read sequentially. `len 48` with an unaligned `off_dt_struct`
    → BAD_LAYOUT. Every truncation of the fixture with `len ≥ 40` → exactly TRUNCATED,
    because the strings block ends at 7426 > len.
A6. **What counts as "consumed".** I read it as: root cells; in depth-2 memory nodes
    reg/device_type/status; in /reserved-memory, its cells and ranges; in its depth-3
    children reg/status/no-map/size; in /chosen the two initrd properties. So a
    repeated `reg` in a depth-2 **non-memory** node (uart) is OK, a repeated
    non-consumed `compatible` in root is OK, and a 4-byte `reg` in a non-memory node is
    not a BAD_CELLS. Memory duplicates must be DUPLICATE even when `device_type` comes
    after the duplicate (classification happens at END_NODE).
A7. **Exact names and depths.** `chosen@0` and `reserved-memory@0` are not special,
    depth-3 `chosen` or `device_type="memory"` nodes are not special, and
    device_type `"Memory"`/`"memor"` is not memory.
A8. **Cells are not inherited.** A memory node's own `#address-cells` does not govern
    its `reg`; the root's cells do.
A9. **Version.** `version ≥ 17` is accepted (18/17 → OK), and so is `last_comp == 17`.
A10. **Status names.** Asserted to equal the enumerator name minus `JE_FDT_` (the
    header's examples are "OK" and "BAD_MAGIC"). `JE_FDT_STATUS_COUNT` and 999 →
    "UNKNOWN".
A11. **Order of the kept-reservation array.** Asserted as processing order: memreserve
    entries first, then /reserved-memory in walk order. The header specifies the
    processing order; that `rsv[]` follows it is my inference. RAM order is **not**
    asserted (set membership only).
A12. **`rsv_off` of a memreserve entry** = the byte offset of that 16-byte entry
    (exact). `ram_off` and `rsv_off` from the struct are only required to lie inside
    the struct block (the header doesn't say whether the BEGIN_NODE or the `reg`
    PROP supplies them).
A13. **check() with pa_bits.** The err_idx for an "end above 2^pa_bits" failure is not
    documented, so only BAD_RANGE is asserted. An end exactly equal to 2^pa_bits is
    OK, because the end is exclusive.
A14. **Counting.** `node_count` includes the root and status-disabled nodes. The root
    is depth 1, so `max_depth` is 16 with 15 nested levels and 17 → DEPTH_LIMIT.
    Props in one node are 64 OK / 65 → PROP_LIMIT, and consumed props count; NOPs do
    not.
A15. **Parse with totalsize < 40.** Gives BAD_LAYOUT through the layout rules.
    copy_plan reads only the header and does only its four checks, so a bad
    version/offset still returns OK there.
A16. **Missing rsvmap terminator.** When entries would run into `off_dt_struct`, the
    result is BAD_LAYOUT. Struct bytes are not reinterpreted as entries that could
    yield RANGE_LIMIT/BAD_RANGE first.
A17. **Walk-time before post-walk.** Root cells absent with no memory → BAD_CELLS, not
    NO_MEMORY. A struct BAD_TOKEN beats NO_MEMORY. The rsvmap RANGE_LIMIT beats a
    later struct defect.
A18. **Initrd timing.** Whether bad-length, half-present or end≤start initrd is
    detected during the walk or after it is not ordered against reservation errors.
    Only "initrd not inside RAM is checked after reservations" is asserted.
A19. **check() details.** It uses only the first `rsv_count` reservations and ignores
    `initrd` when `has_initrd` is false. Adjacent kernel/dtb/initrd is not an overlap.

No deviation from the assignment: only `tests/test_fdt.c` was edited (plus this
uncommitted notes file). The Makefile and `fdt.h` were untouched, and no
commit/push/network.
