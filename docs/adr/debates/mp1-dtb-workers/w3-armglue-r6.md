# W3 round 6 — tools/smoke_arm.py + mk/aarch64.mk (harness) — worker notes

Observed 2026-10-30, worktree wt52-armglue-r5, branch wip/52-armglue-r5, base a199412.
Changed: `tools/smoke_arm.py`, `mk/aarch64.mk` only (git status: both ` M`). Not committed.
Implemented by a single AI agent (W3); AI-assisted, not human-reviewed.

## Fix (Codex rev6 P2: vector evidence forgeable)

1. **PASS line pinned exactly.** New `VECTOR_PASS` = the full literal from
   `kernel/arch/aarch64/main.c:213` (without `\r\n`), matched by `exact()`
   (whole-line equality). Wildcard `\([^\n]*\)` removed.
2. **ELR pinned to `&je_arm_brk_site`.** `VECTOR_TEST_MARKERS` (static) replaced by
   `vector_test_markers(brk_site)`; the esr/elr/spsr line is now a single `exact()`
   with `elr=hx(brk_site)`. `brk_site` comes from `resolve_brk_site(elf, image)`:
   - pure-Python bounded ELF64 parse (`elf_defined_symbols`; no nm, no subprocess):
     checks magic, ELFCLASS64/LE, `e_machine == EM_AARCH64`, shentsize 64, exactly one
     `SHT_SYMTAB`, symtab entsize 24, sh_link in range, strtab inside file, every
     struct read bounds-checked, NUL-terminated names; file > 64 MiB refused.
     Any malformation -> `ElfError` -> FAIL.
   - requires exactly one DEFINED (0 < shndx < 0xff00) `je_arm_brk_site` with
     `STB_GLOBAL`, and exactly one defined `_text`.
   - **extra (stricter than asked):** binds the ELF to the Image actually booted:
     the Image word at offset `je_arm_brk_site - _text` must equal
     `0xd434ab40` = BRK #0xa55a (`0xd4200000 | imm16<<5`, DDI0487 BRK). Relies on
     linker.ld: Image = `objcopy -O binary` starting at `_text` (VMA==LMA). A stale or
     foreign ELF therefore FAILs instead of supplying a plausible address.
   - prints evidence: `elf: <path> sha256=... je_arm_brk_site=0x... (Image word ...)`.
3. **`--elf PATH`** option; default = `os.path.join(dirname(image), "jeios-arm.elf")`.
   `mk/aarch64.mk` passes `--elf $(ARM_BUILD)/jeios-arm.elf` explicitly (+ comment).
   Policy: under `--vector-test` a missing ELF is a FAIL even without `--require`;
   under `--require` a missing ELF is a FAIL in every mode; otherwise (no vector test,
   no require) the ELF is unused and its absence is ignored. Check runs after the
   image-exists SKIP check (an unbuilt tree still SKIPs without `--require`).

Deviation: chose pure Python over `aarch64-linux-gnu-nm` (instruction allowed either,
preferred Python), so no nm tool requirement was added under `--require`. Cross-checked
manually: `aarch64-linux-gnu-nm build/aarch64-vectest/jeios-arm.elf` ->
`0000000040080fe0 T je_arm_brk_site`, `0000000040080000 T _text` == harness result.

Coupling: if main.c's PASS text or the BRK imm changes, `VECTOR_PASS` /
`BRK_A55A_INSN` must change with it (fail-closed: mismatch is a FAIL).

## Verification actually run (local, this machine; NOT remote CI)

Env: Python 3.12.3, QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18), aarch64-linux-gnu-gcc.

Real QEMU, all four modes, `make smoke-aarch64 <mode> ARM_REQUIRE=1`, each rc=0 PASS:
default; `ARM_VECTOR_TEST=1` (resolved `je_arm_brk_site=0x0000000040080fe0`, wire
`elr=0x0000000040080fe0`); `ARM_BSS_CHECK=1`; `ARM_DTB_BAD=1`.
Also `python3 tools/smoke_arm.py build/aarch64-vectest/Image --vector-test --require`
(default sibling `--elf`) -> PASS.

Shim (`/tmp/w3r6/qemu-system-aarch64`: answers `--version` with 8.2.2, replays a log
into the `-serial file:` path, sleeps until SIGTERM; logs derived from the real
vector serial.log by sed), all with `--vector-test`:

| case | result |
|---|---|
| control: real vector log replayed (`--require`) | rc=0 PASS |
| `elr=0xffffffffffffffff` | rc=1 FAIL (missing esr/elr/spsr whole line) |
| elr off by 4 (`0x...40080fe4`) | rc=1 FAIL |
| PASS text `(bogus)` | rc=1 FAIL (missing exact PASS line) |
| PASS text with one extra space | rc=1 FAIL |
| missing ELF, no `--require` | rc=1 FAIL (`--vector-test: ELF ... not found`) |
| stripped ELF (`objcopy --strip-all`) | rc=1 FAIL (no SHT_SYMTAB) |
| foreign ELF (default build, no symbol) | rc=1 FAIL (found []) |
| Image patched NOP over the BRK, correct ELF | rc=1 FAIL (word 0xd503201f != 0xd434ab40) |
| Image file passed as `--elf` | rc=1 FAIL (bad magic) |
| ELF truncated to 100 bytes | rc=1 FAIL (truncated ELF) |

Non-vector: real default build `--elf /tmp/w3r6/nope.elf --require` -> rc=1 FAIL
(`--require: ELF ... not found`); same without `--require` -> rc=0 PASS (ELF unused).

Also: `ASAN_OPTIONS=detect_leaks=0 make check` rc=0; `git diff --check` clean;
`python3 -m py_compile tools/smoke_arm.py` ok.

## Not verified / open

- Remote ARM CI not run/observed.
- r5 stderr-allowlist shim cases were not re-run this round (code path untouched).
- ELF parser only handles ELF64 LE AArch64 with a normal section table
  (e_shnum != 0); extended section numbering / SHN_XINDEX symbols are rejected
  (fail-closed), not supported.
- The ELF↔Image binding checks one instruction word, not the whole image; it
  proves the symbol points at a BRK #0xa55a in the booted Image, not full
  ELF/Image byte identity.
