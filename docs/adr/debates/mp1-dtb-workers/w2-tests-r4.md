# W2 round 4 — tests (tests/test_fdt.c only)

Observed 2026-10-30 (local worktree `wip/52-tests-r4`, base commit 44187ac). Not committed.
Did not read kernel/core/fdt.c. Did not edit include/jeios/fdt.h or any other file.

## What I built (implemented)

Five new functions in `tests/test_fdt.c` (+615 lines), called from `main()` after
`test_bad_cells()`, and three new asserts in `test_check()`. Every negative case
pins exact status AND err_off.

- `test_dup_before_value` (Codex test review 2; Claude probe 2a)
  - root `#address-cells` / `#size-cells` repeated with len 0, value 3 and len 8 -> DUPLICATE
    at the second PROP.
  - memory `status` "okay" repeated with an empty value, with "okay" (no NUL) and with
    "disabled" (no NUL). Each runs with status before and after device_type -> DUPLICATE.
  - memory node whose FIRST status is empty, then a repeat -> DUPLICATE at the repeat. The
    memory-status value is checked at END_NODE, so the repeat comes first.
  - rsv child: status "okay" then an empty status -> DUPLICATE.
  - depth-2 device_type "memory" then an empty device_type -> DUPLICATE.
  - /chosen and /reserved-memory: device_type "foo" then "memory" -> DUPLICATE, not CONFLICT.
  - /reserved-memory: `#address-cells=<3>` or an empty one, followed by a valid repeat ->
    DUPLICATE at the repeat (probe 2a).
- `test_unconsumed_strings` (Claude probes 1a-1g). Each case uses both an empty value and
  "okay" without a NUL, and expects OK plus a check of the semantic result:
  - root status and device_type (1b, 1g);
  - /chosen status, which still yields the initrd (1c);
  - /reserved-memory status, which still yields its reservation (1d);
  - non-memory depth-2 nodes: no device_type, and device_type "foo" on either side of the
    status (1e);
  - depth-3 untracked nodes under soc and under a memory node, for both status and
    device_type (1a, 1f);
  - device_type on a depth-3 rsv child (see N3);
  - a depth-4 grandchild of a rsv child, for both.
- `test_memory_status_timing`. Every case runs with the empty status before and after
  device_type "memory":
  - control: BAD_STRING at the status PROP;
  - empty status followed by a repeated reg -> DUPLICATE at the second reg;
  - empty status, then a depth-3 child of the memory node with a bad nameoff -> BAD_STRING
    at the child's PROP, and err_off is not the status PROP;
  - "first of the END_NODE checks": a malformed status (no NUL) beats an absent reg
    (BAD_RANGE) and a 12-byte reg (BAD_CELLS) -> BAD_STRING at the status PROP.
- `test_rsv_cells_deferral` (probes 2b, 2c)
  - Each cell gets a bad value: 3, 0, len 0 or len 8. With a child the result is BAD_CELLS
    at the child BEGIN_NODE; childless, BAD_CELLS at END_NODE. That is 16 cases.
  - A defect between the bad cell PROP and the decision point wins: a repeated `ranges` gives
    DUPLICATE, and a bad nameoff gives BAD_STRING.
- `test_reg_timing` (probe 3; Codex test review 3 and 4)
  - rsv child with a zero-size reg or a 12-byte reg. Each is followed by one of three
    defects, and the defect wins:
    - a grandchild with a bad nameoff -> BAD_STRING at the grandchild;
    - a repeated no-map -> DUPLICATE;
    - an empty status -> BAD_STRING at the status, since rsv-child status is checked at the
      PROP.
  - err_off = the reg PROP for:
    - memory BAD_CELLS: len 0/4/12/20, reg before and after device_type, and the reg is not
      the node's last PROP; plus 12 bytes under 1/1 cells;
    - memory BAD_RANGE: zero size, overflow, a zero-size 2nd pair, and 1/1 zero size;
    - rsv child BAD_CELLS (len 0, len 12) and BAD_RANGE (zero size, overflow, a zero-size 2nd
      pair).
  - A disabled memory node and a disabled rsv child, each with a 12-byte reg under 2/2 cells
    and status on either side of reg -> OK. Also checks ram/rsv/dynamic/node_count.
- `test_check`: `je_boot_ctx_check(NULL,&F,&e)`, `(&K,NULL,&e)` and `(NULL,NULL,&e)` ->
  BAD_ARG with `e == JE_BOOT_ERR_FACTS`. `e` is pre-poisoned to 0x5a5a5a5a.

## What I ran (tested), exact commands and results

1. `make build/0/test_fdt` compiled with no warnings. The flags are
   `-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes`, plus ASan+UBSan.
2. `git diff --check`: clean.
3. `ASAN_OPTIONS=detect_leaks=0 ./build/0/test_fdt` -> rc 134. It aborts at the first
   expected failure, `tests/test_fdt.c:2868: want DUPLICATE got BAD_STRING (err_off 124)`
   (the round-3 parser checks a memory node's status value at the PROP).
4. `make check` -> rc 2. Every suite before fdt passed: core, boot, alloc, vm, uframe,
   syscall, handle x2 and endpoint. fdt aborted at the same line, so the later make-check
   steps did not run. Log: /tmp/w2r4/check.log.
5. Non-aborting scratch copy:
   - Source: `/tmp/w2r4/test_fdt_scratch.c`, which replaces `abort()` with a counter.
   - Built with the identical flags against this tree's round-3 `kernel/core/fdt.c`.
   - Run: `ASAN_OPTIONS=detect_leaks=0 /tmp/w2r4/scratch`.
   - Result: 57 failure lines (`SCRATCH: 57 failures`), including cascaded CHECKs. No ASan
     or UBSan reports. The rest of the suite, including truncation and mutation, still
     printed PASS.

The ARM cross-build is not applicable: the test file is hosted only. Smoke and integration
were not run because they are not affected by a hosted test file.

### Scratch failures (line numbers in the scratch copy), mapped to findings

All of them are expected against the round-3 parser:

| Scratch line | Case | Got | Finding |
|---|---|---|---|
| 2869 | memory: empty status then repeated status | BAD_STRING at 1st status | parser #1/#4 (memory status decided at PROP, not END_NODE) |
| 2938 (x2) | rsv `#address-cells=<3>`/empty then repeat | BAD_CELLS at 1st PROP | parser #2, probe 2a |
| 2960 (x2) | root empty/no-NUL status + device_type | BAD_STRING | probes 1b, 1g |
| 2973 (x2) | /chosen status | BAD_STRING | probe 1c |
| 2985 (x2) | /reserved-memory status | BAD_STRING | probe 1d |
| 3006 (x2) | non-memory depth-2 status | BAD_STRING | probe 1e |
| 3039 (x2) | depth-3/4 untracked status/device_type, rsv-child device_type | BAD_STRING | probes 1a, 1f |
| 3081 (x2) | memory: empty status then repeated reg | BAD_STRING at status | parser #1/#4 timing |
| 3100 (x2) | memory: empty status then child bad nameoff | BAD_STRING at status | parser #1/#4 timing |
| 3164 (x16) | rsv bad cell value, with a child or childless | BAD_CELLS at the PROP | parser #2, probes 2b, 2c |
| 3181 | rsv bad cell then repeated ranges | BAD_CELLS at PROP | parser #2 |
| 3194 | rsv bad cell then bad nameoff | BAD_CELLS at PROP | parser #2 |
| 4162-4166 | check() NULL ctx/facts with non-NULL err_idx | err_idx unwritten | parser nit (fdt.c:1091 per review) |

Each entry marked "(x2)" or "(x16)" fails in every loop variant. The 3164 line covers all 16
deferral variants.

### Cases that PASSED against the round-3 parser

- Every case in `test_reg_timing`: rsv-child reg decoded at END_NODE (probe 3), the reg-PROP
  err_off pinning, and the disabled 12-byte reg cases.
- All root and memory-status dup-before-value cases, apart from the empty-first one at 2869.
- The device_type "foo" then "memory" -> DUPLICATE cases.
- The memory "first END_NODE check" cases (status beats an absent or malformed reg).
  Against a PROP-time status check these pass trivially, so they only catch a parser that
  runs reg checks before status at END_NODE. The cases that tell PROP-time and END_NODE-time
  checking apart are 2869, 3081 and 3100.

## Could NOT verify / not done

- I did not see the round-4 parser, so I don't know whether the new cases pass against it.
- No remote CI.
- The ARM cross-compile is not relevant to this file and was not run.
- No human review. Any review of this diff is AI-assisted.

## New ambiguities (flagged, not resolved)

- **N1 — err_off for a bad-nameoff BAD_STRING.** The contract says "first violation" but not
  which word of the PROP header. The new tests accept any offset in the PROP's 12-byte header
  window (`EXPECT_BAD_NAMEOFF_AT`), and every alternative they discriminate against lies
  outside that window. Proposal: pin it to the PROP token.
- **N2 — memory-node status when device_type "memory" already came first.** Literal reading
  of 44187ac: the check still happens at END_NODE, so a later repeated reg gives DUPLICATE.
  I tested it that way (w==1 variants at 3081 and 3100). A parser that checks at the PROP
  once the node is already classifiable would fail. Please confirm this was intended.
- **N3 — device_type on a depth-3 /reserved-memory child.** It is a consumed name, so it is
  duplicate-tracked, but its value is consumed "on every depth-2 node" only. I asserted that
  an empty value there is OK.
- **N4 — err_off for "memory node without reg -> BAD_RANGE".** This is END_NODE-time and has
  no reg PROP, so err_off is unspecified (probably the END_NODE). Not asserted.
- **N5 — order of END_NODE checks after status.** For example, a reg that is absent versus
  one that is malformed. Only "status first" is pinned; the rest is not asserted.
