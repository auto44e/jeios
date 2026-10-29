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
