# WORKER_NOTES — PR-E test writer (issue #53, ADR 0010 D10)

Worker: AI test-writer agent (single agent within a multi-agent PR; AI-assisted, NOT
independent human review). Worktree `wt-m4e-tests`, branch `wip/m4e-tests`, base
commit `1df911a`. Observed 2026-10-31 (UTC, host clock), Ubuntu 24.04, gcc 13.3.0,
Python 3.12.3. Nothing committed or pushed. No Makefile or frozen-file edits.

## State

| Item | State |
|---|---|
| `tools/test_mkimage.py` (new, stdlib-only) | implemented; tested against stub (FAILS, expected) and against a test-writer throwaway reference (PASS); seeded-defect run below |
| `tests/test_image.c` (new) | implemented; tested against a hand-written stand-in descriptor (PASS) and 4 corrupted stand-ins (all FAIL as required). NOT yet run against a real mkimage.py output (blocked on the parallel implementation) |
| Makefile wiring | planned — lead must add (rule below) |

I did NOT read mkimage.py's body (only its spec docstring via `git show HEAD`).

## Files

- `tools/test_mkimage.py` — crafts ELF64 with `struct` (header + phdrs + segment bytes,
  offsets auto-placed congruent to vaddr), runs `python3 tools/mkimage.py` as a
  subprocess. Usage `python3 tools/test_mkimage.py [--keep]`; exit 0 iff all pass;
  last line `test_mkimage: PASS (N checks, ...)` or `FAIL (k of N ...)`. N varies by
  implementation (accepted paths run more sub-checks), so don't pin N. ~5-10 s.
  - Rejects: every rule E1-E10, each in `--verify` AND `--out` mode; asserts exit 2,
    the exact `mkimage: reject: <RULE>: ` prefix, and no OUT.c left behind.
    E1 size 1MiB+1/empty/magic/MZ/class 32+NONE/BE/EI_VERSION/ET_DYN/ET_REL/EM_386/
    EM_AARCH64/e_version 0,2; E2 phentsize 64,0 / phnum 0,17 / phoff past EOF /
    phoff+n*56 = size+1 / 64-bit overflow / 2^63; E3 INTERP DYNAMIC TLS NOTE PHDR SHLIB
    GNU_RELRO GNU_PROPERTY GNU_EH_FRAME LOPROC+1 (+ at index 0); E4 GNU_STACK RWX, X;
    E5 0 loads (two forms), 9 loads; E6 unaligned vaddr, below base, vaddr 0,
    non-congruent offset, filesz>memsz, memsz 0, offset+filesz=size+1, offset past
    EOF, offset+filesz overflow; E7 rodata flags none/X/W/W|X/RWX/R|0x00100000/
    R|0x80000000/R|0x8, code RWX, data W|X; E8 page overlap, duplicate vaddr,
    out-of-order headers with BSS overlap, memsz rounding into guard, at guard, on
    stack, at arena top, vaddr near 2^64, memsz 2^64-1; E9 65 pages, 65 via
    rounding, one huge BSS; E10 RX BSS, rodata, data init, data BSS, gap, 0,
    base-1, 2^64-1. Ordering: bad machine + PT_INTERP -> E1; W|X LOAD at index 0 +
    later PT_NOTE -> E3.
  - Accepts (boundary passes): default 3-seg W^X; out-of-order headers; single RX;
    GNU_STACK R / 0 / first; memsz 0xfff; pure-BSS RW; phnum 16; phdrs ending at EOF;
    file exactly 1 MiB; last segment at EOF; 8 loads; 64 pages exactly; segment
    ending exactly at 0x1fb000; gaps; entry at last initialized byte; entry in 2nd RX;
    names "a" and 32 chars.
  - For deep accepts: compile hosted (`-std=c11` + project WARN) and with kernel
    freestanding flags; `nm` shows exactly `je_img_NAME_{blob,len,desc}`, all type
    `R`; a host probe #includes the generated .c and prints len, sizeof(blob),
    header, all 8 slots and the blob, compared with a spec-derived expectation
    (running source_offset, source_size=filesz, target=vaddr-base, memory_size
    page-rounded, ELF->JE perm, zero unused slots, vaddr-ordered concatenation);
    explicit ELF R|X->5, R->1, R|W->3 assertions; input and blob sha256 appear in
    the comment; second run in another dir byte-identical; `--verify` leaves the
    directory containing only the input.
  - Cross-check: builds `tests/test_image.c` + `kernel/core/vm.c` + `handle.c` against
    6 generated images (incl. 8 segments, 64 pages, guard edge, toolchain ELF).
  - Real toolchain ELF: gcc/ld with a TEST-LOCAL linker script (not `user/user.ld`,
    which is another worker's) producing RX/R/RW-with-BSS; must be accepted and
    cross-checked. Skipped (reported, not counted as pass) only if gcc is missing.
  - CLI: bad NAMEs ("", Hello, 1abc, _a, a-b, a.b, "a b", 33 chars, é) -> 1, missing
    file, directory, neither --out/--verify, missing --name, no args, unknown
    option, unwritable out path -> 1.
- `tests/test_image.c` — `#include JE_TEST_IMAGE_C` (generated source) after
  `<jeios/image.h>` in ONE translation unit, so for c_hello the compiler also checks
  the generated definitions against image.h's externs and `sizeof blob == len`.
  Asserts `je_vm_image_validate == JE_OK`, tool output promises (version, reserved,
  contiguous source offsets, sum == len, ascending targets, page-rounded sizes, zero
  unused slots, len-1 rejected), then ~70 mutations that must be rejected: every
  illegal perm (0, W, X, W|X, R|W|X, unknown bits) on every segment, entry past RX
  initialized bytes / in every non-X segment and its BSS / RX source_size 0 /
  outside arena, overlap three ways, guard/stack/arena-top intrusion, page budget
  65, version 2 (exactly JE_ERR_UNSUPPORTED), reserved fields, count 0/9, unaligned
  memory_size/target, source_size>memory_size, source beyond blob. Controls:
  unmodified copy and entry at last initialized byte must validate. Uses a CHECK
  macro (survives NDEBUG, JCS-05). Prints `test_image: PASS (N checks) image=...`.

## Makefile rule needed (lead to add; I did not edit the Makefile)

The include path MUST be absolute: GCC resolves `#include "..."` relative to the
including file's directory (`tests/`), not the cwd. Verified: a relative
`"build/..."` path fails with "No such file or directory"; an absolute path works.
Only vm.c + handle.c are needed to link (vm.c alone: undefined `je_handle_table_init`).

```make
# ADR 0010 D10: real kernel validator vs a mkimage.py-generated image.
$(BUILD)/test_image: tests/test_image.c $(BUILD)/gen/image_c_hello.c kernel/core/vm.c kernel/core/handle.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DJE_TEST_IMAGE_C='"$(abspath $(BUILD)/gen/image_c_hello.c)"' -DJE_TEST_IMAGE_NAME=c_hello -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_image.c kernel/core/vm.c kernel/core/handle.c -o $@
```
Add `$(BUILD)/test_image` to `test:`'s prerequisites and `./$(BUILD)/test_image` to its
recipe, and `$(PYTHON) tools/test_mkimage.py` to `check` (it needs gcc and nm on PATH).

## Commands run and results

1. `gcc ... tests/test_image.c kernel/core/{vm,alloc,boot,handle}.c` (project WARN,
   ASan+UBSan) against hand-written `/tmp/m4e-tw/image_c_hello.c` (3 segs R|X/R/R|W):
   `test_image: PASS (76 checks) image=c_hello blob=10 segments=3`.
2. Same, link with only vm.c + handle.c (no sanitizers): PASS (76 checks).
3. Corrupted stand-ins (perm 5->7, len 10->9, desc reserved 2, data seg overlapping
   rodata): each printed `test_image: FAIL (...)`.
4. `python3 tools/test_mkimage.py` against the in-tree STUB:
   `test_mkimage: FAIL (218 of 321 checks failed)`, exit 1. Expected: every
   tool-behaviour check fails (the stub always exits 2 with `reject: STUB`, so every
   rule-ID, accept and CLI exit-1 check fails). The checks that pass are only the
   ones a write-nothing stub satisfies trivially: "no OUT.c on reject", "--verify
   writes nothing", "CLI left the input dir unchanged".
5. Throwaway reference: I wrote my OWN mkimage from the spec (in `/tmp/m4e-tw/ref`,
   never in the repo) and ran the test in a copy of the tree:
   `test_mkimage: PASS (538 checks, rules E1-E10 + CLI + output)`.
6. Seeded-defect run (JCS-10: the test must detect known defects), each defect
   injected into the throwaway reference; every one must yield FAIL:
   SEE "Seeded-defect results" below.

## Seeded-defect results

All 27 seeded defects were detected (test exit 1); restored reference -> PASS (538).
Failed-check counts: no ELF->JE swap 20; allow W|X / RWX 11; budget off-by-one 6;
entry uses memsz 3; entry in any segment 6; blob in file order 2; memory_size not
rounded 21; guard allowed 6; overlap allowed 9; PT_NOTE allowed 5; GNU_STACK X
allowed 6; phnum 17 allowed 3; 9 loads allowed 3; 1MiB+1 allowed 3; filesz>memsz
allowed 3; congruence skipped 3; below base allowed 4; file range unchecked 9;
wrong rule id (E7 reported as E6) 20; OUT.c written on reject 79; --verify writes
a file 21; timestamp in output 15; no hash comment 15; extra global symbol 15;
non-const blob 16; bad NAME accepted 4; descriptor reserved != 0 23.
Dead end recorded: the first "OUT.c written on reject" mutant had a literal `\n`
(syntax error, 201 failures, not a valid detection); redone correctly -> 79.
The first "allow W|X" line was mis-split by the `|` delimiter; rerun separately.

## Could NOT verify

- The real mkimage.py (being written in parallel): untested against it. My
  throwaway reference is NOT evidence about it.
- `tests/test_image.c` against a real generated c_hello image (needs mkimage.py +
  the ring-3 build path from the other worker).
- `make check` / `make smoke` NOT run: my files are not wired into the Makefile, and
  I changed no kernel/boot file, so smoke has nothing new to exercise.
- Remote CI: none observed.

## Decisions (with rejected alternatives)

- D1 Same-TU include (`#include JE_TEST_IMAGE_C`) instead of linking the generated
  .c as a separate object. Why: sizeof(blob) becomes checkable against `_len`, and
  image.h's externs are type-checked against the generated definitions. Rejected:
  separate object (the `_len`/size mismatch and type drift would be invisible).
- D2 Spec-derived expected descriptor computed in the test from the crafted phdrs
  (an independent model), read back from the generated C via a compiled probe,
  rather than regex-parsing the C. Why: the compiler is the ground truth for what
  the initializer means (field order, suffixes, designated vs positional). Rejected:
  parsing C text (fragile, would constrain formatting the spec leaves free).
- D3 Each reject case runs in both --verify and --out mode. Why: an implementation
  could share code paths differently per mode. Cost ~2x runtime (fine, < 10 s).
- D4 Cross-check from the Python test too (builds test_image.c against 6 generated
  images), not only via the Makefile's c_hello. Why: covers boundary images
  (8 segs, 64 pages, guard edge) through the REAL C validator. Rejected: Makefile-only
  (would cross-check one image).
- D5 Test-local linker script for the toolchain case instead of `user/user.ld`. Why:
  that file is another worker's, may not exist yet, and my test must not depend on
  its content. The lead may later add a case using user.ld.
- D6 The test harness itself is validated with a throwaway reference + seeded
  defects, kept outside the repo. Why: tests that only ever ran against a stub
  could be wrong in either direction. Rejected: committing the reference (it would
  be a second, divergent implementation).

## Spec ambiguities (flagged, literal reading implemented, NOT resolved)

- A1 "The first violated rule is the one reported": I read it as RULE order (lowest
  E-number), not program-header scan order. Test "E3 before E7" (W|X PT_LOAD at
  index 0, PT_NOTE at index 3 -> E3) depends on this. A one-pass per-phdr
  implementation would report E7. Lead/owner should confirm; drop that one case if
  the other reading is chosen.
- A2 E7 "flags ... must be exactly R|X, R or R|W": I read the WHOLE p_flags word, so
  unknown bits (PF_MASKOS 0x00100000, PF_MASKPROC 0x80000000, 0x8) with R are
  rejected as E7. A reading that masks to the three low bits would accept them.
  (The C validator rejects unknown perm bits, so the strict reading matches it.)
- A3 Truncated file (< 64 bytes, valid magic): spec has no explicit header-size rule.
  I only test the empty file (fails "ELF magic" -> E1). Implementations should
  report E1 for a short header; not asserted.
- A4 Reject with --out: the spec says "convert; write OUT.c" for accepted input but
  does not say a reject must leave no file. I assert no OUT.c exists after a reject
  (fail-closed for the build). Not asserted: behaviour when OUT.c pre-exists.
- A5 Neither --out nor --verify, or both: the CLI lists two forms; I assert
  "neither" -> exit 1 and do NOT test "both".
- A6 Multiple PT_GNU_STACK headers: not forbidden; not tested together.
- A7 sha256 format in the comment: I search for the lowercase hex digest
  case-insensitively anywhere in the file; the spec does not fix a format.
- A8 Name "é": rejected via the [a-z] grammar; how non-ASCII argv is handled is
  implementation-defined; I only assert exit 1.
- A9 Comment must not depend on the output/input PATH (my determinism check uses
  identical basenames in different directories). If the tool records the full
  input path, determinism across directories fails — I consider that a defect
  (the build path would leak into the image source) but the spec is silent.
- A10 E8 for a single out-of-arena segment vs E9: a 2^64-1 memsz satisfies both;
  rule order gives E8, which is what I assert.
