# MP1 #52 — bounded DTB parser contract: peer consult + reconciliation

Parallel read-only consult (Claude + Codex) on the proposed contract, 2026-10-29, before
freezing include/jeios/fdt.h for parallel workers. AI-assisted; not human qualification.

## Lead reconciliation (adopted into fdt.h)

Agreed by both, adopted: canonical block order + offsets>=40 + alignment; /reserved-memory uses its
OWN cells (required, == root) + empty `ranges`; reject duplicate consumed props; `reg` length a nonzero
multiple of (ac+sc)*4; err_off output + zero-on-fail + shared status_name(); parse/check split with an
arch-facts struct incl. pa_bits; DTB source checked as the full [x0, x0+totalsize); pre-wiring commit +
disjoint file ownership; dtb-randomness=off + pinned -m for reproducible fixtures.

Splits decided by the lead (not owner-gated; implementation contract):
- copy sizing: Codex — one bounded prefix copy min(totalsize, 64 KiB) via a pure, hosted-tested
  je_fdt_copy_plan(); parser re-derives everything from the copy. (Claude: size from header fields.)
- reservation fully outside RAM: Claude — ignore + count (firmware carves no-map regions out of
  /memory); partial overlap still BAD_RANGE. (Codex: reject.)
- reservation overlapping initrd/DTB: Claude — allowed (U-Boot memreserves the initrd itself);
  only kernel-vs-reservation is fatal. (Codex: reject.)
- dynamic /reserved-memory (size, no reg): Claude — count, not error (MP1 allocates nothing; MP2
  allocator obligation). (Codex: reject.)
- zero padding: not required (libfdt doesn't check; no safety gain). (Codex: require.)
- initrd widths: independent 4/8 per property, as Linux. (Codex: equal widths.)

Verified facts (pinned QEMU 8.2.2): virt-8.2 DTB totalsize = 1 MiB but live extent 7426 B (so a
64 KiB *totalsize* cap would reject the real DTB — overrode the first draft); default build injects
rng-seed/kaslr-seed (non-reproducible) — dtb-randomness=off gives a stable SHA; at -m 256M RAM is
[0x40000000,0x50000000) so the DTB at 0x48000000 is inside RAM (at the 128M default it is NOT).

## Claude (read-only)
**Verdict:** the contract holds up with the revised item 2, but it needs about a dozen fixes, and three of them change `fdt.h`, so freeze the header only after those. I checked everything marked [verified] against the local QEMU 8.2.2 (`dumpdtb` plus a small Python struct walker).

### Verified facts that change the contract
1. **Your 454-byte strings block is a random blob.** By default virt-8.2 writes `rng-seed` (32 bytes) and `kaslr-seed` (8 bytes) into `/chosen`. Three dumps gave three different SHA-256s. With `dtb-randomness=off` the strings block is 434 bytes and the output is reproducible. **Decision:** use `-M virt-8.2,dtb-randomness=off` for both the fixture dump and the smoke run, and record the exact command line (`-m`, `-smp`, `-kernel`, `-initrd`, `-append`). The dump changes with these options: `-initrd` adds 8-byte `linux,initrd-start`/`linux,initrd-end` values (end is exclusive), and `-append` adds `bootargs`.
2. **Real-DTB shape:**
   - Header: 48 / 64 / 6992 / 7056; `off_mem_rsvmap` is 48, not 40 (libfdt aligns it to 16).
   - The rsvmap holds only the 0,0 terminator.
   - END is exactly at struct end.
   - Max depth is 6 counting the root, 56 nodes, at most 13 properties per node.
   - Root cells are 2/2, with one `/memory@40000000`: 128 MiB, 2/2 cells.
   - There is no `/reserved-memory`.
   - Other nodes have different cells: `/cpus` 1/0, `/pcie` 3/2, `/platform-bus` 1/1.
3. **A memory node's `reg` comes before its `device_type`.** You can't classify a node until you hit its END_NODE. **Decision:** W1 holds the `reg` (offset, len) for the current depth-1 node and decides at END_NODE.
4. **Check where the DTB actually lands.** With the default `-m 128M`, RAM is `[0x40000000, 0x48000000)`, so a DTB at 0x48000000 would sit just outside `/memory` and your "DTB in RAM" check would reject it. Confirm which `-m` produced that observation and pin it in the smoke run.
5. **Commit the fixture truncated to its used extent** (about 7.5 KB, with `totalsize` still saying 1 MiB), not the full 1 MiB of mostly zeros. Record the SHA-256 of both. This also exercises the "totalsize > len is allowed" path for free.

### Item 2 (copy sizing)
6. **Don't scan the rsvmap in firmware memory.** Compute `copy_len = min(totalsize, max(struct_end, strings_end, off_rsv + (JE_FDT_MAX_RSV+1)*16))`, all overflow-checked. Copy that, then let the parser find the terminator inside the copy, within MAX_RSV+1 entries and before `off_dt_struct`. Reject if `copy_len > JE_FDT_MAX_COPY`.
7. **The copy is TOCTOU-free only if the parser re-derives everything from the copied header.** It must check every block against `len`; the glue's first-read values are used only to size the copy. Keep a loose `totalsize` cap (2 MiB is fine) as BAD_LAYOUT.
8. **Glue ordering hazard:** before zeroing `.bss` and before copying, check that `[x0, x0+totalsize)` is disjoint from `[_start, _end)`, which includes `.bss`, the copy buffer and the stacks. Otherwise zeroing `.bss` can destroy the source. Also require `x0 != 0` and 8-byte alignment (boot protocol).
9. **Require the canonical block order:** rsvmap < struct < strings, plus `off_rsv >= 40`. dtc and libfdt (`fdt_open_into`/`fdt_pack`), and so U-Boot and OpenSBI, always produce this order. It makes the non-overlap check trivial.

### FDT spec traps
10. **`#address-cells`/`#size-cells` are not inherited.** They apply to the node's children and default to 2/1 when absent.
    - `/memory`: use the root's cells, and require them explicitly (absent → BAD_CELLS).
    - `/reserved-memory` children: use `/reserved-memory`'s own cells. Require them to be present and equal to the root's, else BAD_CELLS. This is the bug a root-only policy would ship.
11. **`nameoff` may point into the middle of a string** (dtc shares string suffixes). Only check `nameoff < size_dt_strings` and that a NUL follows inside the block, never that it sits at a string start.
12. **Structural strictness:**
    - The first token, after NOPs, is BEGIN_NODE with an empty name.
    - PROP after a child node in the same node → BAD_TOKEN.
    - Exactly one root.
    - After the root's END_NODE: only NOPs, then END, ending exactly at `off_dt_struct + size_dt_struct`.
    - Cap name length (say 255).
    - Compute PROP `len + 3` in 64-bit.
    - Drop the separate token cap: struct size / 4 already bounds the loop.
13. **Reject duplicates** of the properties you consume (`reg`, cells, initrd, `device_type` in the relevant nodes) and duplicate `/chosen` or `/reserved-memory`. Otherwise a hostile blob can show one value to you and another to a later consumer.
14. **`reg` validation:** length must be nonzero and a multiple of `(ac+sc)*4`. Skip nodes with `status` not equal to `"okay"`/`"ok"` (Linux does).
    - Classify memory nodes by `device_type="memory"` at depth 1, ignoring the name.
    - For `/reserved-memory` children without `reg` (dynamic `size`/`alloc-ranges`): ignore and count them; nothing fixed needs protecting.
15. **Memreserve:** the terminator is addr==0 && size==0. Linux stops at size==0 alone, so an entry with size==0 and addr≠0 → BAD_RANGE (it's ambiguous).

### Range policy
16. **Don't reject reserved regions that overlap the initrd or the firmware DTB.** U-Boot's `fdt_initrd()` adds the initrd to memreserve, and `fdt_shrink_to_minimum` reserves the DTB itself. Only kernel-vs-reserved overlap is fatal.
17. **Reserved range outside RAM:**
    - Fully disjoint → ignore and count. Some firmware carves no-map regions out of `/memory`, and Linux tolerates this.
    - Partial overlap → BAD_RANGE.
    - Fully inside → keep.
18. **"Inside RAM" means contained in a single range.** RAM ranges may be adjacent (NUMA) but must not overlap. Use exclusive ends with `base + size <= UINT64_MAX`.
19. **Initrd:** only one of start/end present, or `end <= start` → BAD_RANGE. Accept 4- or 8-byte values for each property independently.

### Limits
20. Depth 16, RAM ranges 8 and reserved 16 are fine. **Raise nodes to 1024** (counters cost nothing; RISC-V virt uses 2 nodes per hart). Props per node 64 is fine. Keep 64 KiB as a compile-time constant: QEMU uses about 7.5 KB, and only large `-smp` RISC-V setups get close. Version policy (version ≥ 17, last_comp ≤ 17) is correct.

### API changes to freeze now
21. `je_fdt_parse(const uint8_t *blob, size_t len, je_boot_ctx *out, uint32_t *err_off)`. It zeroes `*out` on entry so a failure leaves no partial context, and fixes the check order so a multi-defect blob always gets the same status.
22. Split parse and check, but pass a struct so arch facts can grow without changing the signature:
    `je_boot_ctx_check(const je_boot_ctx *, const je_boot_facts *, uint32_t *err_idx)`
    with `facts = {kernel_image, dtb_src (x0, totalsize), pa_bits}`, and add PA-range enforcement (ID_AA64MMFR0 on ARM).
23. Put `je_fdt_status_name()` in `fdt.c`, owned by W1, so the glue and tests share one table. Add `UNSUPPORTED_ORDER` or fold it into BAD_LAYOUT, but decide which now.

### Tests (W2)
24. Give each blob an exact-size `malloc` copy, never a static 64 KiB buffer, or ASan can't see over-reads.
25. Add a truncation sweep: for every `len` in `[0, used)` on the real blob → non-OK, no crash.
26. Add targeted mutations of every header and length field, plus a determinism check: parsing the same input twice gives the same status and offset.
27. Assert exact statuses only on single-defect fixtures.

### Worker split
28. **Smoke-script conflict:** W3 and W4 both edit `smoke_arm.py` (markers vs `--require`). Give the whole file to W3; W4 edits only `ci.yml` against a flag name frozen now.
29. **Makefile/test-runner conflicts:** land a pre-wiring commit before the workers start. It contains the frozen `fdt.h`, a stub `fdt.c` returning BAD_MAGIC, an empty `test_fdt.c` registered in `make check`, and `fdt.c` in the kernel sources. After that, each worker owns disjoint files.
30. **`JE_ARM_DTB_BAD`:** corrupt the copy after the glue's header check, e.g. write token `0x0000000A` at `off_dt_struct`. Assert `BAD_TOKEN` at the expected offset, a halt marker, and that no later boot markers appear. Corrupting the magic would only test the glue path, not the parser.
31. **Prove the fixture matches what actually boots:** have the glue print a CRC32 of the copied used extent as a boot marker, and have the smoke run compare it with the fixture's CRC. This depends on item 1.
32. **Out of scope, but note it in the ADR:** the copy is written with the MMU off (non-cacheable). Before MP2 turns on the caches it must invalidate the buffer's cache lines (`DC IVAC`), or it may read stale lines.

The dumps and the walker script are in `/tmp/fdtchk` and `/tmp/fdtwalk.py`.

## Codex (read-only)
codex
I’ll pressure-test the revised copy-sizing rule against the DTB/FDT format and the stated QEMU header, then give a numbered accept/change decision and call out contract gaps that would affect worker interfaces.
codex
1. **CHANGE item 2:** do one bounded prefix copy, then parse only the copy.

   ```c
   total = header.totalsize;
   if (total > JE_FDT_MAX_TOTAL) reject TOO_LARGE;

   copy_len = min(total, JE_FDT_MAX_COPY);
   copy source[0..copy_len) once;
   parse(copy, copy_len, ...);
   ```

   Scanning the firmware blob to discover `used`, then copying it, is still two-source-read TOCTOU. “Used extent” can remain an internal parser check, but need not control copying.

2. **ACCEPT 64 KiB parser copy; ACCEPT 2 MiB total cap for #52.** The pinned QEMU DTB is 1 MiB because `totalsize` includes free space/padding, not just live blocks. With your offsets, live extent is approximately:

   ```text
   max(64, 64 + 6992, 7056 + 454) = 7510 bytes
   ```

   The 2 MiB value is a project policy, not an FDT guarantee; document that future RISC-V platforms may raise it. The FDT format explicitly permits gaps and trailing free space. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

3. **FREEZE `len` semantics:** `len` is bytes actually copied, not necessarily `totalsize`.

   Therefore:

   - allow `totalsize > len`;
   - require every needed block and reservation terminator to fit within `len`;
   - reject `used > JE_FDT_MAX_COPY`;
   - distinguish `TRUNCATED` from `TOO_LARGE` in the contract.

4. **TIGHTEN layout checks:**

   - offsets must be `>= 40`;
   - reservation block must be 8-aligned;
   - structure block must be 4-aligned;
   - blocks must be non-overlapping and ordered: header, reservation, structure, strings;
   - checked ends must fit both `totalsize` and copied `len`;
   - require `last_comp_version <= version` in addition to `version >= 17 && last_comp_version <= 17`.

   The version policy is compatible with the specification’s “version 17 or later, backwards-compatible with 17” rule. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

5. **BOUND the reservation scan explicitly:** permit at most 16 nonzero entries plus the `(0,0)` terminator. Scan only within:

   ```text
   min(totalsize, JE_FDT_MAX_COPY)
   ```

   Missing terminator after the limit is `RANGE_LIMIT`; malformed entry arithmetic is `BAD_RANGE`. Reservation entries are always 64-bit, independent of root cells. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

6. **ADD missing structure rules:**

   - root node must be the first node and have an empty name;
   - properties must precede child nodes;
   - padding bytes must be zero;
   - `FDT_END` must be the final 32-bit word;
   - reject `END_NODE` at depth zero and `END` with nonzero depth;
   - count every property, including ignored ones;
   - make the token cap numeric, e.g. `JE_FDT_MAX_TOKENS = 16384`.

   The specification requires the root representation, balanced node tokens, and `FDT_END` as the final structure token. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

7. **BREAK “root-only cells” for `/reserved-memory`:** root cells are correct for `/memory`, but `reg` uses the parent node’s cells. Require `/reserved-memory` to contain `#address-cells` and `#size-cells`, and require them to equal the root values; require `ranges` to be present and empty. Otherwise reject `BAD_CELLS`/`BAD_LAYOUT`.

   This is a real FDT trap. `/memory` uses root cells, while `/reserved-memory` has its own required cell properties. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter3-devicenodes.html), [devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter2-devicetree-basics.html))

8. **ACCEPT `/memory@*` extraction with stricter matching:** direct child of root, unit-name base exactly `memory`, `device_type == "memory"`, valid nonempty `reg`. Multiple memory nodes are valid; aggregate up to eight ranges. The specification permits either multiple memory nodes or multiple ranges in one node. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter3-devicenodes.html))

9. **CHANGE reserved-memory policy:** explicitly reject dynamic-only children containing `size` without `reg`, because their address is not known. If both exist, `reg` wins. Record `no-map`, but do not act on it in MP1. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter3-devicenodes.html))

10. **TIGHTEN semantic ranges:**

    - reject duplicate recognized properties;
    - require `reg` lengths to be exact multiples of `(address_cells + size_cells) * 4`;
    - reject overlapping RAM ranges;
    - reject overlapping reserved ranges;
    - require initrd start/end to appear together, use equal widths, satisfy `start < end`, lie in RAM, and not overlap RAM reservations;
    - treat missing/empty RAM as `NO_MEMORY`;
    - treat reserved/initrd outside RAM as `BAD_RANGE`.

11. **ACCEPT the parse/check split.** Keep all DTB-internal validation in `je_fdt_parse`; keep kernel-image and firmware-source checks in `je_boot_ctx_check`. Pass the DTB source range as the conservative full `[dtb_phys, dtb_phys + totalsize)` range, not merely the copied prefix. Also check initrd versus reserved ranges and kernel versus every reservation.

12. **CHANGE the parser API for diagnostics:**

    ```c
    je_fdt_status je_fdt_parse(const uint8_t *blob,
                               size_t len,
                               je_boot_ctx *out,
                               size_t *error_offset);

    const char *je_fdt_status_name(je_fdt_status);
    ```

    On failure, zero `out` and return the first deterministic offset. Without an error-offset output, the requested bounded diagnostic contract is incomplete.

13. **WORKER split is sound with one ownership rule:**

    - W1: `fdt.c` and frozen header only.
    - W2: hosted builder, fixtures, mutation tests, and host test runner.
    - W3: AArch64 glue, linker/startup changes, copy buffer, markers, bad-copy build.
    - W4: CI workflow only.
    - Makefile changes must be assigned explicitly: W2 owns `make check`; W3 owns ARM targets; W4 must not independently edit the Makefile.

14. **ADD copy-path tests separate from parser fuzzing.** Parser mutation tests prove bounded parsing. They do not prove the ARM glue never reads beyond the bounded prefix. Add a hosted fake-source test for the prefix-copy planner, including:

    - `totalsize = 1 MiB`, live blocks under 64 KiB;
    - no reservation terminator within the limit;
    - block end beyond 64 KiB;
    - totalsize above the total cap;
    - source-range overflow.

15. **Final decision:** freeze the contract only after changing the copy algorithm, adding the error offset, defining `len` when `totalsize > len`, and handling `/reserved-memory` parent cells. Everything else is compatible with QEMU virt-8.2 and a reasonable MP1 bounded parser.
1. **CHANGE item 2:** do one bounded prefix copy, then parse only the copy.

   ```c
   total = header.totalsize;
   if (total > JE_FDT_MAX_TOTAL) reject TOO_LARGE;

   copy_len = min(total, JE_FDT_MAX_COPY);
   copy source[0..copy_len) once;
   parse(copy, copy_len, ...);
   ```

   Scanning the firmware blob to discover `used`, then copying it, is still two-source-read TOCTOU. “Used extent” can remain an internal parser check, but need not control copying.

2. **ACCEPT 64 KiB parser copy; ACCEPT 2 MiB total cap for #52.** The pinned QEMU DTB is 1 MiB because `totalsize` includes free space/padding, not just live blocks. With your offsets, live extent is approximately:

   ```text
   max(64, 64 + 6992, 7056 + 454) = 7510 bytes
   ```

   The 2 MiB value is a project policy, not an FDT guarantee; document that future RISC-V platforms may raise it. The FDT format explicitly permits gaps and trailing free space. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

3. **FREEZE `len` semantics:** `len` is bytes actually copied, not necessarily `totalsize`.

   Therefore:

   - allow `totalsize > len`;
   - require every needed block and reservation terminator to fit within `len`;
   - reject `used > JE_FDT_MAX_COPY`;
   - distinguish `TRUNCATED` from `TOO_LARGE` in the contract.

4. **TIGHTEN layout checks:**

   - offsets must be `>= 40`;
   - reservation block must be 8-aligned;
   - structure block must be 4-aligned;
   - blocks must be non-overlapping and ordered: header, reservation, structure, strings;
   - checked ends must fit both `totalsize` and copied `len`;
   - require `last_comp_version <= version` in addition to `version >= 17 && last_comp_version <= 17`.

   The version policy is compatible with the specification’s “version 17 or later, backwards-compatible with 17” rule. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

5. **BOUND the reservation scan explicitly:** permit at most 16 nonzero entries plus the `(0,0)` terminator. Scan only within:

   ```text
   min(totalsize, JE_FDT_MAX_COPY)
   ```

   Missing terminator after the limit is `RANGE_LIMIT`; malformed entry arithmetic is `BAD_RANGE`. Reservation entries are always 64-bit, independent of root cells. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

6. **ADD missing structure rules:**

   - root node must be the first node and have an empty name;
   - properties must precede child nodes;
   - padding bytes must be zero;
   - `FDT_END` must be the final 32-bit word;
   - reject `END_NODE` at depth zero and `END` with nonzero depth;
   - count every property, including ignored ones;
   - make the token cap numeric, e.g. `JE_FDT_MAX_TOKENS = 16384`.

   The specification requires the root representation, balanced node tokens, and `FDT_END` as the final structure token. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter5-flattened-format.html))

7. **BREAK “root-only cells” for `/reserved-memory`:** root cells are correct for `/memory`, but `reg` uses the parent node’s cells. Require `/reserved-memory` to contain `#address-cells` and `#size-cells`, and require them to equal the root values; require `ranges` to be present and empty. Otherwise reject `BAD_CELLS`/`BAD_LAYOUT`.

   This is a real FDT trap. `/memory` uses root cells, while `/reserved-memory` has its own required cell properties. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter3-devicenodes.html), [devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter2-devicetree-basics.html))

8. **ACCEPT `/memory@*` extraction with stricter matching:** direct child of root, unit-name base exactly `memory`, `device_type == "memory"`, valid nonempty `reg`. Multiple memory nodes are valid; aggregate up to eight ranges. The specification permits either multiple memory nodes or multiple ranges in one node. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter3-devicenodes.html))

9. **CHANGE reserved-memory policy:** explicitly reject dynamic-only children containing `size` without `reg`, because their address is not known. If both exist, `reg` wins. Record `no-map`, but do not act on it in MP1. ([devicetree-specification.readthedocs.io](https://devicetree-specification.readthedocs.io/en/latest/chapter3-devicenodes.html))

10. **TIGHTEN semantic ranges:**

    - reject duplicate recognized properties;
    - require `reg` lengths to be exact multiples of `(address_cells + size_cells) * 4`;
    - reject overlapping RAM ranges;
    - reject overlapping reserved ranges;
    - require initrd start/end to appear together, use equal widths, satisfy `start < end`, lie in RAM, and not overlap RAM reservations;
    - treat missing/empty RAM as `NO_MEMORY`;
    - treat reserved/initrd outside RAM as `BAD_RANGE`.

11. **ACCEPT the parse/check split.** Keep all DTB-internal validation in `je_fdt_parse`; keep kernel-image and firmware-source checks in `je_boot_ctx_check`. Pass the DTB source range as the conservative full `[dtb_phys, dtb_phys + totalsize)` range, not merely the copied prefix. Also check initrd versus reserved ranges and kernel versus every reservation.

12. **CHANGE the parser API for diagnostics:**

    ```c
    je_fdt_status je_fdt_parse(const uint8_t *blob,
                               size_t len,
                               je_boot_ctx *out,
                               size_t *error_offset);

    const char *je_fdt_status_name(je_fdt_status);
    ```

    On failure, zero `out` and return the first deterministic offset. Without an error-offset output, the requested bounded diagnostic contract is incomplete.

13. **WORKER split is sound with one ownership rule:**

    - W1: `fdt.c` and frozen header only.
    - W2: hosted builder, fixtures, mutation tests, and host test runner.
    - W3: AArch64 glue, linker/startup changes, copy buffer, markers, bad-copy build.
    - W4: CI workflow only.
    - Makefile changes must be assigned explicitly: W2 owns `make check`; W3 owns ARM targets; W4 must not independently edit the Makefile.

14. **ADD copy-path tests separate from parser fuzzing.** Parser mutation tests prove bounded parsing. They do not prove the ARM glue never reads beyond the bounded prefix. Add a hosted fake-source test for the prefix-copy planner, including:

    - `totalsize = 1 MiB`, live blocks under 64 KiB;
    - no reservation terminator within the limit;
    - block end beyond 64 KiB;
    - totalsize above the total cap;
    - source-range overflow.

15. **Final decision:** freeze the contract only after changing the copy algorithm, adding the error offset, defining `len` when `totalsize > len`, and handling `/reserved-memory` parent cells. Everything else is compatible with QEMU virt-8.2 and a reasonable MP1 bounded parser.

---

## Round-2 cross-lineage reviews (2026-10-29, AI-assisted; not human qualification)

Each worker's round-1 diff was reviewed adversarially by the OTHER model lineage. Raw outputs follow verbatim.

### Codex reviewing W3 (Claude) ARM glue + harness

1. **P1 must-fix — DTB guard accepts wrapping addresses.** `boot.S:141-148` skips `x0 + 2 MiB` whenever `x0 >= _end`. An aligned `x0=0xfffffffffffffff8` passes, then `main.c:343-361` reads/copies through a wrapping physical pointer before `je_boot_ctx_check()` can reject it. This defeats the pre-dereference guard and can fault or access unrelated memory.

2. **P2 should-fix — harness does not assert `pa_bits=44`.** `tools/smoke_arm.py:58,153,207` only requires the prefix `boot context validated pa_bits=`. A PARange decode regression producing `pa_bits=40` or `32` still passes. The supplied QEMU fact (`44`) should be asserted exactly.

3. **P2 should-fix — harness accepts abnormal QEMU termination or post-marker hangs.** `tools/smoke_arm.py:234-238` ignores `subprocess.run()`’s return code and treats every timeout as expected. An image that emits all success markers and then crashes/exits nonzero—or hangs after the terminal marker—can report PASS.

4. **P2 should-fix — Make toggle combinations reuse incompatible objects.** `mk/aarch64.mk:17-33` lets the last enabled toggle choose the build directory, while `:50-60` changes compiler defines without encoding them in dependencies. For example, build with `ARM_BSS_CHECK=1 ARM_DTB_BAD=1`, then run only `ARM_DTB_BAD=1`; `build/aarch64-dtbbad/Image` is reused with the wrong BSS behavior. This causes configuration-dependent false failures and stale-image risk.

5. **P3 nit — `ARM_REQUIRE` is not an end-to-end preflight.** `mk/aarch64.mk:102` builds the Image before invoking the script. If the toolchain is absent, `make smoke-aarch64 ARM_REQUIRE=1` fails inside Make rather than producing the documented `--require` failure/log. It still fails, but diagnostics and semantics differ.

confirmed sound:

- Non-wrapping guard comparisons use unsigned condition codes and correct half-open boundary behavior.
- Guard runs before BSS pattern fill/clear.
- Linker `_end` covers file image, BSS, DTB copy buffer, and stack.
- Copy planning is bounded to 64 KiB; the actual copy uses volatile byte accesses, with no `memcpy`, SIMD, or exclusive instructions observed.
- `JE_ARM_DTB_BAD` writes within the copied buffer and occurs before parsing.
- PARange mapping `0..6 → 32,36,40,42,44,48,52` is correct.
- Normal and `dtb-bad` marker ordering rejects later success markers as intended.
2026-10-30T00:09:35.048529Z ERROR codex_core::session: failed to record rollout items: thread 01a18a21-a3a5-7fb1-82ee-74187e74f0fa not found

### Claude reviewing W1 (Codex) parser

Adversarial review of `kernel/core/fdt.c` at bdf446a: I found 7 contract violations beyond the two known bugs, plus a few lesser issues. I reproduced every finding below with a probe in `/tmp/fdtrev/probe.c`. It reuses the builders from `tests/test_fdt.c` and was built with ASan/UBSan, which reported nothing. No repo files were changed. All offsets are absolute, with the struct block at 0x38.

## P1 — must fix

**1. Struct and strings blocks may overlap.**
- Where: `fdt.c:643-648`. The header check tests `off_struct >= off_strings` but never `off_strings < struct_end`.
- Contract (`fdt.h:146`): "strings start >= struct end", otherwise BAD_LAYOUT.
- Probe: `build_simple()` with `off_dt_strings = off_struct+4` gives **BAD_CELLS err_off=0xa8**. It should be BAD_LAYOUT with err_off 0.
- Effect: struct bytes get read as property names.

**2. A disabled `/reserved-memory` or `/chosen` is still processed.**
- Where: `fdt_finish_node`, `fdt.c:417-454`. Only the memory branch (`:457`) and the rsv-child branch (`:476`) look at `status_enabled`.
- Contract (`fdt.h:196`): status-disabled nodes are fully skipped for semantics.
- Probes:
  - `/reserved-memory { status="disabled"; }` with no cells gives **BAD_CELLS @0xd0**.
  - `/chosen { status="disabled"; linux,initrd-start; }` gives **BAD_RANGE @0xd8**.
- Effect: children of a disabled `/reserved-memory` are still collected as reservations, and initrd is taken from a disabled `/chosen`.

**3. `/reserved-memory` cells are validated after its children have already used them.**
- Where: the parent's cells/ranges are checked at the parent's END_NODE (`:424-430`). Each child is decoded at its own END_NODE (`:489-520`), which comes earlier, using `parent->addr_cells/size_cells` as they stand (possibly 0 or wrong).
- Probes:
  - Parent with only `#address-cells` gives **BAD_RANGE @0xdc** (size_cells=0, so every size is 0). It should be BAD_CELLS.
  - Parent cells 1/1 vs root 2/2 gives **BAD_RANGE @0xec**. It should be BAD_CELLS.
  - Wrong cells can also produce a spurious RANGE_LIMIT, or wrongly decoded candidates.
- Fix: use the same "decide at first child BEGIN_NODE" rule as the root (`fdt.h:166-169`).

## P2 — should fix

**4. DUPLICATE is missed when the first `status` is not okay.**
- Where: `fdt.c:457` and `:476` return before `fdt_check_node_duplicates`.
- Probe: memory node with `status="disabled"; status="okay"` returns **OK**, and the node is silently dropped (ram=1, only the other node).
- The contract lists `status` as consumed, so a repeat should be DUPLICATE. The dup check should come before the status skip.

**5. DUPLICATE is missed for `device_type` = "foo" followed by "memory".**
- Where: `fdt.c:936-940` classifies only on the first `device_type`. The node is then not memory, so its duplicate mask is never checked.
- Probe: returns **OK**, and the node is not treated as RAM.
- For contrast, "memory" then "foo" is caught, and the case you asked about (reg, reg, then device_type) is correctly DUPLICATE.

**6. Wrong err_off for the (MAX_RSV+1)-th `/reserved-memory` candidate.**
- Where: `fdt.c:510` sets `*err = node_off`, which is the child's END_NODE.
- Contract (`fdt.h:179`): err_off = that `reg` PROP.
- Probe: the `reg` is at 0x204, but the reported err_off is **0x220**.

**7. Duplicate err_off follows bit order, not file order.**
- Where: `fdt_consumed_duplicate` (`:304`) scans kinds by bit index.
- Probe: memory node with reg, status, status (second at 0xb4), reg (second at 0xc8) reports **0xc8**. The first violation is at 0xb4.

**8. Root duplicate cells are reported too late.**
- Where: root DUPLICATE is checked only at the root's END_NODE (`:405`), so a later child error wins.
- Probe: root `#address-cells` repeated, then a child `reg` with size 0, gives **BAD_RANGE @0x8c** instead of DUPLICATE at the earlier second `#address-cells`.
- This ties into known bug #1: the fix that moves the absence check to the first child BEGIN_NODE should also move the root duplicate check there. The code currently checks absence only at the root END_NODE (`:410`), which also gives the wrong err_off.

**9. Latent `memcpy` emission.**
- Where: `stack[depth] = node;` (`:796`) copies a 128-byte struct.
- With the ARM flags at -O2 and -O3, `nm -u` shows no undefined symbols. At **-O0 and -Os it emits `U memcpy`**, and the ARM image has no `memcpy`.
- Today it's safe only because of the optimization level. Suggest building the node in place (`fdt_zero_node(&stack[depth])`), or adding a link-time guard.

## P3 — nits

10. **Classification precedence.** A depth-2 `chosen` or `reserved-memory` node with `device_type="memory"` plus `reg` is not counted as RAM. The `chosen`/`reserved-memory` branches return first (`:417`, `:434`). Probe: **NO_MEMORY**. The contract says "name not used" for memory.
11. **Zero-size block at offset == len.** A zero-size strings block with `off_strings == len` (len < totalsize) gives TRUNCATED (`:655`), although the block has no bytes. This is the `len` variant of known bug #2.
12. **Initrd err_off.** "Initrd not in RAM" and "end<=start" report `off_struct` or the chosen END_NODE (`:1048`, `:1061`, `:446`). The offending PROP would be more useful. `:1047` also reuses `struct_end` as scratch, which is harmless but misleading.
13. **No count bounds in `je_boot_ctx_check`.** It trusts `ram_count`/`rsv_count`. A caller-built ctx with `ram_count > 8` reads past `ram[]`. Clamp to `JE_FDT_MAX_RAM`/`JE_FDT_MAX_RSV`.
14. **Name-lookup cost.** Each PROP scans the strings block from `nameoff`. The total is bounded at about 64K/12 props × 32K bytes, roughly 90M byte loads. Pre-MMU on Device-nGnRnE memory that is a multi-second stall. Bounded, not a correctness bug.

## Confirmed sound
- **Reads stay in bounds.** Every read is at an offset below `struct_end`/`strings_end`, and both are proven ≤ len before the walk. The ASan/UBSan probes were clean.
- **Loops terminate.** `pos` advances by at least 4 bytes per token and stays 4-aligned, so the walk is bounded. The name scan is capped at 256 bytes.
- **Overflow handling.** Header and PROP arithmetic is done in u64 with `fdt_add_u64`/`fdt_align4`, and the reg tuple shift is correct for cells 1|2.
- **Header and rsvmap checks run in the contract's order** (rsvmap: RANGE_LIMIT before BAD_RANGE, terminator, 64-bit entries).
- **Token rules:** root must be first and empty-named; a second root, PROP after a child, END_NODE at depth 0, and END-not-last-word are all rejected as specified.
- **Initrd lengths:** 4 and 8 bytes are accepted independently, and a bad length gives BAD_RANGE at the PROP.
- **Post-walk order:** NO_MEMORY, then OVERLAP (adjacent allowed), then memreserve before reserved-memory, kept/ignored/partial handling, then initrd containment.
- **`je_boot_ctx_check`:** the order and err_idx codes match `fdt.h:218-225`. `pa_bits` 52 works: `1<<52`, with an exclusive end allowed to equal the limit.
- **CRC32:** `je_fdt_crc32("123456789")` = `cbf43926`, the standard check value.
- **Stack:** `-fstack-usage` gives `je_fdt_parse` = 3104 B and `je_arm_main` = 144 B, about 3.3 KiB of the 16 KiB boot stack.

All of this is AI-assisted review, not independent human qualification.

### Claude reviewing W4 (Codex) CI

I found no P1 defects. Nothing in the job lets an ARM boot be skipped and still report a pass. One P2 problem is in the docs, not the workflow: they wrongly say remote CI already compiles ARM, and the new job isn't recorded. The rest are P3 evidence and robustness nits. Nobody has run this job on GitHub yet (W4's notes say so), so everything below is from reading the code.

## Findings

1. **P2: the docs say remote CI already compiles ARM.**
   - `docs/HANDOFF.md:129-130` says "Today remote CI runs x86 bootstrap + the ARM *compile*". That is false: `main:.github/workflows/ci.yml` and the a55c2ff ci.yml have no ARM job at all.
   - At bdf446a, HANDOFF and IMPLEMENTATION have no entry for the new `aarch64` job, not even as "implemented, remote run not observed". AGENTS.md rules 2 and 10 require that in the same change.
   - Fix before merge, and label it local-only until a real GitHub run is seen.

2. **P3: on PRs, `git rev-parse HEAD` (ci.yml step "Record…") records the merge commit, not the PR head.** For `pull_request` events, `actions/checkout` checks out `refs/pull/N/merge`. `CI_STRATEGY.md:103` says "source/merge SHA", so recording both is better. Add `${{ github.event.pull_request.head.sha || github.sha }}`.

3. **P3: some tool versions are not recorded.**
   - The step prints `qemu --version` but not the Ubuntu package version (e.g. `dpkg-query -W qemu-system-arm gcc-aarch64-linux-gnu`). That patch level is what changes silently between runner images, because apt installs are unpinned.
   - `aarch64-linux-gnu-objcopy` and `python3` versions aren't printed either.
   - `CI_STRATEGY.md:39` already admits the supply chain isn't pinned, so this is an evidence gap, not a regression.

4. **P3: the `sha256sum build/aarch64*/Image` lines add little.**
   - The glob matches every variant built so far, so each step prints 1–4 hashes with no clear link to the run that just happened.
   - `smoke_arm.py` already prints `image: <path> sha256=` for the exact image it booted.
   - Drop these lines, or point each at its own directory (`build/aarch64-vectest/Image`, etc.).

5. **P3: upload settings.**
   - `if-no-files-found` is left at the default `warn`. That is fine on failure paths, because the job is already red.
   - Retention is left at the repo default (90 days on GitHub unless changed). #52 and `CI_STRATEGY.md:105` ask for bounded retention, so consider setting `retention-days`.
   - Images and ELFs are not uploaded; only their hashes appear in the job log, which expires on the same schedule.

6. **P3: no `timeout-minutes` on the job.** The QEMU timeout is set inside the Python harness (15 s), but a hang in `apt-get` or `make` would run until GitHub's 360-minute limit. The x86 job has the same gap, so this follows existing convention.

7. **P3: the 15 s boot timeout could be flaky.** GitHub's x86 runners emulate aarch64 in software, with no KVM. If the runner is slow, markers may be missing and the step fails. That is a false failure, never a false pass, but expect some flakes. `--timeout` exists, but `mk/aarch64.mk` doesn't let you set it from the make command line.

8. **P3: the QEMU 8.2.x gate is extra, not the real guarantee.** What actually proves the boot used the expected DTB is the CRC check against the fixture (`smoke_arm.py` `check_dtb_copied`). The gate does fail closed on unexpected output: empty or `9.x` exits 1, and pipefail is on because `shell: bash` is set. The pattern `8.2.[0-9]*` also accepts things like `8.2.2rc`, which is harmless.

## Confirmed sound

- **No skip-as-pass.**
  - All four smoke steps pass `ARM_REQUIRE=1`, and `mk/aarch64.mk` turns that into `--require`.
  - With `--require`, `smoke_arm.py`'s `skip()` calls `fail()` for a missing QEMU, toolchain or image.
  - A missing fixture fails in every mode.
  - A missing marker, a timeout with no output, or an unexpected FATAL line all exit 1.
- **Shell error handling.**
  - Steps without a `shell:` key use GitHub's default `bash -e`, so a failed `make` stops the step before `sha256sum` runs. None of those steps use a pipe.
  - The "Record" step uses `shell: bash` (which adds `-o pipefail`) plus `set -eu`.
- **The upload can't hide a failure.** `if: always()` only adds a step; the job result still comes from the failed steps. The `build/aarch64*/serial.log` glob keeps one file per variant directory (`aarch64/`, `-vectest/`, `-dtbbad/`, `-bsscheck/`) without overwriting. The harness deletes any old log first, so a log from a previous run can't be checked by mistake.
- **The make variables used in the workflow exist with the expected meaning.**
  - `ARM_REQUIRE`, `ARM_VECTOR_TEST`, `ARM_DTB_BAD` and `ARM_BSS_CHECK` all exist in `mk/aarch64.mk`, and each mode builds into its own directory.
  - `smoke-aarch64` depends on `$(ARM_BUILD)/Image`, so every variant is rebuilt with its own flags.
  - `PYTHON ?= python3` is set at `Makefile:4`, and `include mk/aarch64.mk` is at `Makefile:113`.
  - CI never combines `--dtb-bad` with `--vector-test`, which the harness rejects.
- **Installed packages cover what the build needs:** `binutils-aarch64-linux-gnu` provides `ld` and `objcopy`. `qemu-system-arm` on Ubuntu 24.04 ships `qemu-system-aarch64`, though that hasn't been confirmed on a real runner.
- **Permissions are minimal.** The workflow-level `permissions: contents: read` is inherited, and `upload-artifact` needs no extra scope. No secrets, no `pull_request_target`.
- **Action pinning follows the repo's existing style:** major-version tags (`actions/checkout@v4`), not commit SHAs. `CI_STRATEGY.md:111` asks for "reviewed/pinned" revisions, so tags are a gap across the whole repo, not something this diff introduced.
- **No duplicated CI strategy.** The change adds one separate QEMU job to the existing `ci.yml`, which matches the near-term row at `CI_STRATEGY.md:46`. No new workflow file, and the x86 job is untouched. The hosted FDT tests still run in the x86 `make check`.

### Codex reviewing W2 (Claude) tests

1. **P1 must-fix — obsolete alternative status accepted.**
   `tests/test_fdt.c:1512-1525` accepts either `BAD_STRING` or `BAD_LAYOUT` for an unterminated node name. The current contract explicitly requires `BAD_STRING` (`include/jeios/fdt.h:59`). A parser returning the wrong status can pass this test.

2. **P2 should-fix — root-cell timing is not tested.**
   `tests/test_fdt.c:1789-1809` checks only `BAD_CELLS`, not `err_off`. The contract requires the offset of the first child `BEGIN_NODE`, or the root `END_NODE` when childless (`fdt.h:165-169`). A parser detecting the same error later still passes.

3. **P2 should-fix — `/reserved-memory` candidate bound is not discriminating.**
   `tests/test_fdt.c:2013-2023` uses 17 candidates all inside RAM. This can pass a parser that enforces only the post-walk kept-reservation limit. Add 17 candidates outside RAM; the contract still requires `RANGE_LIMIT` at the 17th `reg` property during the walk (`fdt.h:177-180`), with its offset checked.

4. **P2 should-fix — zero-size-block clarification has no targeted coverage.**
   No test exercises a zero-length block, especially one beginning exactly at `totalsize`, despite the explicit rule (`fdt.h:147-148`). A parser requiring nonzero block sizes is not killed.

5. **P2 should-fix — mutation iterations may not mutate.**
   In `tests/test_fdt.c:2685-2694`, word mutations at the final 1–3 bytes perform no write, and selected replacement words may equal the original. There is no before/after assertion or mutation counter, so the claimed 30,000-mutation corpus can contain no-op iterations.

6. **P3 nit — “every parse input is exact-size malloc” is literally false.**
   `parse_bytes()` allocates one byte for `len == 0` (`tests/test_fdt.c:443-445`), and `test_bad_arg_parse()` calls the parser directly with `NULL` blobs (`:1039-1049`). This does not weaken valid-input ASan coverage, but the claim should be narrowed.

7. **P3 nit — builder helper hardcodes reserved-memory cells.**
   `rm_child()` always emits `b_reg1(2u, 2u, ...)` (`tests/test_fdt.c:364-371`). It cannot generate a correct 1/1 child; the current 1/1 test works around this manually at `:913-920`.

Confirmed sound:

- Normal parser inputs go through exact-size heap copies; the truncation sweep is especially valuable.
- Failure zeroing and success `err_off == 0` are enforced broadly.
- Big-endian builder logic, suffix-sharing names, alignment, duplicate cases, range arithmetic, overlap ordering, CRC, and `boot_ctx_check()` boundary coverage are strong.
- The mutation determinism check compares status, offset, and context across repeated parses.

---

## Round-3 contract consult (2026-10-29) — lead brief, then both answers verbatim

```text
JeiOS MP1 DTB parser contract (include/jeios/fdt.h) — round-3 lead decisions, asking for a quick adversarial check.
Context: bounded single-pass FDT parser over a copied blob (<=64 KiB), first-violation-wins, deterministic status + err_off.
Depth-2 nodes: memory (device_type=="memory", classified at END_NODE), /reserved-memory (by name), /chosen (by name).
Consumed props in those nodes: #address-cells, #size-cells, reg, device_type, status, ranges, no-map, size, linux,initrd-start/-end.
Review found (reproduced) bugs; proposed contract text for each:

D1 (Claude reviewer P1): a status-disabled /reserved-memory or /chosen is currently still processed.
   -> A depth-2 /reserved-memory or /chosen with status present and not "okay"/"ok" is skipped
      for semantics INCLUDING its whole subtree (children yield no candidates, no dynamic count),
      but is still structurally validated, counted, and still counts toward the "second node" DUPLICATE rule.
D2 (Claude P1): /reserved-memory cells/ranges are validated at its END_NODE, after children used them.
   -> Decided at its first child BEGIN_NODE (or its END_NODE if childless): BAD_CELLS, err_off = that token.
      (Mirrors the root rule.) If the node is disabled (D1) the cells are not checked.
D3 (Claude P2): DUPLICATE missed when first `status` was non-okay, or device_type "foo" then "memory".
   -> DUPLICATE is checked for every consumed property in EVERY depth-2 node (and root for cells),
      regardless of status or classification, and is reported AT THE REPEATING PROP token as it is
      encountered (so err_off is the earliest repeat in file order; root duplicate cells reported
      immediately, not late). Duplicate check precedes the status skip.
   Question: should duplicate-tracking apply to all depth-2 nodes (needs per-node bitset; cheap) — yes proposed.
D4 (Claude P1, fixed in r2): struct and strings blocks must not overlap: struct_end <= off_strings, else BAD_LAYOUT.
D5 (Claude P3): /chosen or /reserved-memory node that also has device_type="memory".
   -> Name wins: such a node is NOT a memory node; device_type there is consumed only for DUPLICATE.
   Alternative considered: fail closed (reject). Lead leaning: name wins (Linux's early scan does the
   reverse — classifies by device_type — so tell me if "reject as BAD_CELLS/DUPLICATE" is safer).
D6 (W2 test review / r2): zero-size block whose offset == len (copy shorter than totalsize) is valid, not TRUNCATED.
D7 (W2 A20): unterminated ROOT name: BAD_STRING (name rule applies before root-empty-name rule).
D8 (W2 A23): a 17th candidate whose own range is invalid (zero size/overflow): which wins?
   -> Proposal: per-range validity (BAD_RANGE) is checked first for that range, then the candidate count.
      i.e. order inside one `reg` PROP: length/BAD_CELLS, then for each range in order: validity BAD_RANGE, then count RANGE_LIMIT.

Answer tersely: for each D, AGREE or OBJECT + one-line reason + any missed edge. Max ~300 words.
```

### Claude

**Summary:** I agree with D2, D3 (it needs a wider scope), D4, D6, D7 and D8. I object to D1 for `/reserved-memory` and to D5. You also need one new structural rule (R0), because D1 and D2 depend on it.

**R0 (missed, prerequisite):** require that all of a node's properties come before its first child node. A PROP token after a child BEGIN_NODE/END_NODE in the same node should be a structural error. Without this rule, `status` or `#size-cells` can arrive after the children have already been processed, which breaks D1 and D2.

**D1 — OBJECT for `/reserved-memory`, AGREE for `/chosen`.** Skipping a disabled `/reserved-memory` drops its reservations, so the kernel can allocate over firmware memory. That fails *open*. Either ignore `status` there and honour the reservations, or reject. Apply the same reasoning to disabled reserved-memory *children*. Skipping disabled memory nodes or `/chosen` fails safe. Also define the exact `status` match: the whole value must be `"okay\0"` or `"ok\0"`, so `"okay\0junk"` counts as not okay.

**D2 — AGREE.** Define what happens when the cells are absent: either require them to equal root's or use spec defaults. Say which. Require `ranges` to be empty (Linux requires this). err_off for missing cells is the child BEGIN_NODE token.

**D3 — AGREE, widen it.** Depth-3 `/reserved-memory` children consume `reg`, `size`, `no-map` and `status`, so they need the per-node bitset too. Reset the bitset at every tracked BEGIN_NODE.

**D4 — AGREE.** Also check:
- the rsvmap doesn't overlap struct or strings, and is 8-aligned;
- the struct block is 4-aligned with a size that's a multiple of 4;
- `nameoff` and its terminating NUL fall inside `size_dt_strings`, not just inside the blob.

**D5 — OBJECT, fail closed.** "Name wins" makes us disagree with Linux on the same blob, and a hostile blob can exploit that difference. Reject at the `device_type` PROP token: the name is known at BEGIN_NODE, so the check is immediate. Like DUPLICATE, run it before the status skip. Use a dedicated status (e.g. `CONFLICT`); `BAD_CELLS` is misleading. Missed edge: name matching with unit addresses. Linux also accepts `/chosen@0`. Pick exact or base-name matching. If you pick base-name, `chosen` plus `chosen@0` must trigger the second-node DUPLICATE rule.

**D6 — AGREE**, only when offset == len ≤ totalsize. Note that it only really applies to the strings block: struct always contains at least FDT_END, and the rsvmap needs a 16-byte terminator.

**D7 — AGREE.**

**D8 — AGREE.** State the err_off granularity: is it the `reg` PROP token or the offset of the bad range? Make sure ranges in skipped or disabled nodes don't count toward the limit. Also confirm cells > 2 gives BAD_CELLS before any range is checked.

### Codex

- **D1 — AGREE.** Status skip must apply semantically to the entire subtree while preserving structural validation and duplicate accounting. Missed edge: malformed/non-NUL `status` must still be `BAD_STRING`, not silently treated as disabled.

- **D2 — OBJECT.** At first child `BEGIN_NODE`, a later `status="disabled"` is not yet known, so this cannot honor “disabled nodes do not check cells” in arbitrary property order. Require `status` before the first child, or define a different single-pass rule.

- **D3 — AGREE.** Per-node duplicate bitsets make first-repeat reporting deterministic and correctly independent of status/classification. Missed edge: tracking must be direct-property scoped; child properties must not affect the parent’s bitset.

- **D4 — AGREE.** `struct_end <= off_strings` correctly permits adjacency and rejects overlap. Missed edge: perform checked-end arithmetic and independently validate both blocks’ bounds against the copied length.

- **D5 — AGREE.** Explicit name precedence removes ambiguity and preserves dedicated `/chosen` and `/reserved-memory` semantics. Missed edge: name matching must be exact path-component matching, not substring matching; `device_type` remains duplicate-tracked.

- **D6 — AGREE, conditionally.** A zero-length block may validly end at `len`; nonzero required bytes beyond `len` must still be `TRUNCATED`. Explicitly state that `totalsize > len` is not independently fatal in this case.

- **D7 — AGREE.** String termination must precede the root-empty-name check, yielding deterministic `BAD_STRING`. Missed edge: a terminated but nonempty root name needs its own specified status/offset.

- **D8 — AGREE.** Per-range `BAD_RANGE` before count enforcement gives the invalid range precedence, including the 17th candidate. Missed edge: increment the candidate count only after that range fully passes validation.

### Lead resolution of the round-3 consult (committed as `5a3c14b`)

Decision + why (who proposed / who concurred):
- **R0 already holds.** Claude's "props before children" prerequisite is the existing rule
  "PROP after a child node -> BAD_TOKEN", so `status`/cells are known at the first child BEGIN_NODE.
  That also answers Codex's D2 objection (a later `status` cannot appear). D2 adopted (Claude reviewer
  proposed; both concurred once R0 was pointed out).
- **D1 changed.** Claude objected that skipping a disabled `/reserved-memory` fails open. Codex agreed
  with skipping. Lead: `status` is IGNORED on `/reserved-memory` and `/chosen` (matches Linux early
  scan, which does not check either node's status). It still applies to memory nodes and to
  reserved-memory children, matching Linux's `of_fdt_device_is_available` on children.
  This is Claude's alternative extended to `/chosen` for consistency.
- **D3 adopted and widened** to depth-3 reserved-memory children, per-node and direct-properties only
  (Claude + Codex edges).
- **D5 fail closed.** Claude objected, Codex agreed with "name wins". Lead sided with Claude: the
  new `JE_FDT_CONFLICT` is reported at the device_type PROP. Names match exactly (Claude's edge), so
  `chosen@0` is an ordinary node.
- **Malformed `status`/`device_type`** (empty or no trailing NUL) -> BAD_STRING (Codex edge).
- **D8 adopted.** Only VALID ranges count as candidates, and the error is reported at the reg PROP
  (Codex + Claude edges).
- **D4, D6 and D7 adopted** (both agreed). Also `je_boot_ctx_check` now clamps counts -> BAD_ARG
  (Claude reviewer P3, previously an OOB-read risk).

Predictions if chosen otherwise: if a disabled `/reserved-memory` were skipped, a firmware blob could
silently hand reserved memory to the MP2 allocator. With "name wins", JeiOS and Linux would disagree
about RAM on the same blob, and we could not tell which one is right.

Flex to revisit: if a real platform ships `chosen@0`, or a disabled reserved-memory child that must
still be honored, revisit name matching and child status with that DTB as evidence.
