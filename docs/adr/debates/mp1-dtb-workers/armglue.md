# W3 (ARM64 glue + QEMU harness) — worker notes, issue #52

Observed 2026-10-29 (local time; no timezone recorded by the tooling), worktree
`wt52-armglue`, branch `wip/52-armglue`, base commit `a55c2ff`. Uncommitted
working-tree changes only. AI-authored (single worker); not human-reviewed.

## Files changed (only my files)
- `kernel/arch/aarch64/boot.S`: DTB-pointer guard before the BSS clear (and
  before the JE_ARM_BSS_CHECK pattern fill) + `dtb_fatal` DR-only marker path.
- `kernel/arch/aarch64/main.c`: `dtb_intake()` after `install_vectors()`, before
  the BRK test; `console_dec`; PARange decode; JE_ARM_DTB_BAD injection.
- `kernel/arch/aarch64/linker.ld`: `_end` documented as IMAGE_END (no new
  symbol needed: `_end` already covers .data+.bss+stack); ASSERTs that
  `je_arm_dtb_copy` is 16-aligned and its 64 KiB lie inside [__bss_start, __bss_end).
- `mk/aarch64.mk`: `ARM_DTB_BAD=1` (-DJE_ARM_DTB_BAD, dir build/aarch64-dtbbad,
  smoke `--dtb-bad`), `ARM_REQUIRE=1` (smoke `--require`), every smoke run
  `--log $(ARM_BUILD)/serial.log`.
- `tools/smoke_arm.py`: pinned machine/-m, DTB markers, CRC check vs fixture,
  `--dtb-bad`, `--require`, `--log`, records cc version too.

## Implemented: state = implemented, partially tested (see below)
Marker lines are exactly as specified, with these DELIBERATE additions:
1. `dtb copied` line has a trailing ` crc_len=0x<16hex>` (prefix unchanged).
2. JE_ARM_DTB_BAD prints a non-FATAL evidence line before the parse:
   `JeiOS/aarch64: dtb-bad test: wrote token 0x0000000a at off=0x<16hex>`.
3. Two glue-only fail-closed FATAL lines not in the spec (never expected to fire):
   `FATAL dtb copy plan out of bounds len=...` (planner returned copy_len < 40 or
   > JE_FDT_MAX_COPY — glue does not trust the planner with its buffer bound) and
   `FATAL dtb-bad test: off_dt_struct outside copy` (test build only).

## Contract concerns / interpretation (fdt.h NOT edited)
- **CRC extent is unspecified in fdt.h.** The fixture CRC (QEMU_VIRT82_DTB_CRC32)
  is over the 7426-byte LIVE extent; QEMU's totalsize is 1 MiB, so copy_len =
  64 KiB and a CRC over the whole copy can never match. Debate item 31 says "CRC32
  of the copied used extent", but `used` is only known after the parse while the
  `dtb copied` line precedes the parse. I CRC the copy prefix
  `min(off_dt_strings + size_dt_strings, copy_len)` read FROM THE COPY (u64 sum,
  only used as a length bounded by copy_len). Under the contract's canonical
  order (rsvmap < struct < strings) this equals `used` for every accepted blob.
  The line prints crc_len and the harness asserts crc_len == QEMU_VIRT82_DTB_LEN.
  If the lead prefers CRC over `ctx.used` after parse, move the print.
- **dtb_src size**: fdt.h says `[x0, x0 + header totalsize)`. I pass
  `max(firmware-header totalsize, copy's re-derived ctx.totalsize)` — identical
  in practice; it can only widen the range if the header changed between reads.
- **Reserved PARange**: glue emits `FATAL boot context rejected status=BAD_RANGE
  idx=0x00000000ffffffff` (JE_BOOT_ERR_FACTS) directly, without calling
  je_boot_ctx_check, rather than relying on the checker rejecting pa_bits=0.
- `docs/ARM64.md` referenced by the task does not exist in this tree; I used
  docs/adr/debates/mp1-dtb.md (items 8, 30, 31, 32) and fdt.h instead.

## Harness changes worth reviewing
- Removed the `--memory` option: -m is pinned to 256M because the DTB (and thus
  the CRC/fixture match) depends on it. Nothing in the tree passed --memory.
- Non-`--dtb-bad` modes require the exact line
  `dtb parsed ram=1 rsv=0 ignored=0 dynamic=0 initrd=0` and
  `ram[0] base=0x0000000040000000 size=0x0000000010000000`. Derived by inspecting
  the fixture: rsvmap bytes 48..63 all zero, no "reserved-memory" or
  "linux,initrd" strings, one /memory node. If the real parser legitimately
  reports differently, this expectation (not the parser) needs a look.
- `--dtb-bad`: the FATAL-line list must equal exactly
  `[FATAL dtb rejected status=BAD_TOKEN off=0x<off_dt_struct>]` where
  off_dt_struct (0x40) is parsed from the fixture's header bytes; `dtb parsed`,
  `boot context validated`, `initialization complete` must be absent. The
  `dtb copied` CRC is still checked (copy is CRC'd before the injection).
- `--dtb-bad` + `--vector-test` is refused (FAIL).
- Missing `aarch64-linux-gnu-gcc` is now a SKIP (FAIL with --require); before,
  only QEMU/image were checked. Note `make smoke-aarch64` depends on the Image,
  so without a toolchain make itself fails before the script runs (pre-existing).
- `--log` deletes a stale log first so a previous run's output is never asserted.
- Fixture is read as UTF-8 (its comment has an em dash; first attempt with ASCII
  failed — fixed).

## Commands run and results (local, this machine; NOT remote CI)
Toolchain: aarch64-linux-gnu-gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0;
QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18).
- `rm -rf build/aarch64*; make aarch64 [ARM_VECTOR_TEST=1|ARM_BSS_CHECK=1|ARM_DTB_BAD=1]`
  → all 4 rc=0, no compiler warnings (-Werror set). Image sizes 7905 / 8297 /
  7905 / 8129 bytes. `ld` prints "LOAD segment with RWX permissions" — PRE-EXISTING:
  identical warning from `git archive HEAD` (a55c2ff) built in /tmp. Not suppressed.
- `nm -u` on the default ELF: no undefined symbols. objdump scan: no q-reg/SIMD,
  exclusives, or DC ZVA. `je_arm_dtb_copy` = 0x40082130 (inside BSS
  [0x40081ef0,0x40092130)), `_end` = 0x40096130.
- `make smoke-aarch64` in all 4 modes (WITH THE STUB PARSER in this tree): each
  boots through `vectors installed vbar=0x...40080800`, prints
  `JeiOS/aarch64: FATAL dtb rejected status=UNKNOWN off=0x0000000000000000`
  (stub copy_plan → BAD_ARG; stub status_name → "UNKNOWN"), halts with no later
  markers; harness FAILs (rc 2 from make) — expected until the real parser lands.
  dtb pointer observed = 0x48000000.
- `python3 tools/smoke_arm.py --require /nonexistent` → `FAIL (--require: image
  /nonexistent not built)`, rc=1. Without --require → SKIP, rc=0.
- `make check` → rc=0 (fdt hosted suite is a placeholder in this tree).
- `git diff --check` → clean.

### Extra verification with a THROWAWAY fake parser (in /tmp, not in the tree)
To exercise the success and BAD_TOKEN paths I linked my real boot.o/main.o/
vectors.o with a minimal test double `/tmp/w3fake/fake_fdt.c` (real copy_plan
per contract, standard CRC-32, a crude struct walk that finds /memory and
returns BAD_TOKEN on an unknown token, a crude check). This proves the GLUE and
HARNESS only — NOT the real parser/checker.
- default, --vector-test, --bss-verified, --dtb-bad: all `QEMU aarch64 smoke: PASS`
  with `--require --log`. Booted output includes
  `dtb copied len=0x...10000 totalsize=0x...100000 crc32=0x...cc76dc2b crc_len=0x...1d02`
  → the booted DTB's live extent is byte-identical (by CRC-32) to the committed
  fixture; `boot context validated pa_bits=44` (cortex-a72 PARange=4);
  dtb-bad: `FATAL dtb rejected status=BAD_TOKEN off=0x0000000000000040`.
- Harness negative tests (all FAIL as required): default harness vs dtb-bad
  image; --dtb-bad harness vs good image; a fake CRC off by one bit
  (`crc32=0xcc76dc2a != fixture`); --dtb-bad with --vector-test.
- boot.S guard, by scratch copies of boot.S overriding x19 (on QEMU):
  x0=0, 0x48000004 (misaligned), 0x40000000 (overlap), _text-2MiB+8, _end-8 →
  `FATAL dtb pointer invalid or overlaps image; halted`; boundaries
  x0=_text-2MiB (0x3fe80000) and x0=_end pass the guard (then fault / BAD_MAGIC
  in C as expected for a non-DTB address — the 0x3fe80000 read took a data
  abort which the installed vectors reported, confirming faults during intake
  vector to diagnostics).

## NOT verified
- Anything with the REAL kernel/core/fdt.c (not in this tree; sibling worktrees
  still had the 41-line stub when I looked). The default boot cannot pass here.
- Real je_boot_ctx_check behavior on the QEMU facts (kernel [0x40080000,_end),
  dtb [0x48000000, +1 MiB), RAM [0x40000000, +256 MiB), pa_bits 44).
- Reserved-PARange path, TRUNCATED "(live data exceeds 64 KiB copy window)"
  suffix, and the copy-plan-out-of-bounds FATAL: compiled, never executed.
- Remote CI. Physical hardware. Other QEMU versions.
- docs/HANDOFF.md / IMPLEMENTATION status not updated (not my files) — lead.

## Known hazards / open items
- MMU/caches off: copy written non-cacheable; MP2 must `DC IVAC` je_arm_dtb_copy
  (and je_arm_boot_ctx) before enabling the D-cache (comment in main.c).
- The guard uses the 2 MiB policy cap, so it can reject a valid DTB placed
  within 2 MiB below _text. Deliberately conservative.
- Combining toggles (e.g. ARM_BSS_CHECK=1 ARM_DTB_BAD=1) uses the LAST matching
  build dir with ALL flags — pre-existing pattern; a combined build shares a dir
  with a single-toggle build (stale-object risk). Not changed.
- Parsed context is not consumed by anything yet (MP2 allocator).
