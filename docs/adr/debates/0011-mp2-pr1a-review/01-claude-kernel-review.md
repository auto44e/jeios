I found no default-deny hole and no out-of-bounds index path in the live boot path. There are still eight defects to fix before merge; two of them are correctness gaps in the oracle wiring and the doc claims.

## What I checked and confirmed sound

- **Out-of-window free:** I built the ARM window on the host (`-DJE_ARCH_AARCH64`, ASan+UBSan) and called `je_pages_free` with `0`, `0x3ffff000`, `BASE-PAGE`, `0x50000000` and `0xfffffffffffff000`. All returned `JE_ERR_INVALID` with no sanitizer report. The `(a-BASE)>=SPAN` check comes before the index is derived (`alloc.c:85-88`).
- **x86:** I compiled the HEAD and staged `alloc.c` with `-O2` and diffed the disassembly. `je_pages_alloc`, `je_pages_free` and `je_heap_alloc` are instruction-identical, and `sizeof(je_pages)` is unchanged. `je_pages_init` behaves the same but is not byte-identical code (see #4).
- **Interval edges:** contains-page (`base<=p && p+4K<=end`) and intersects-page (`p<end && base<p+4K`) are exact at both ends.
- **Image covers allocator state:** `je_arm_pages` sits in `.bss`, inside `[_text,_end)`, so the kernel reservation covers it. The boot stack is inside the image too (linker.ld ASSERTs).
- **allocmax is not vacuous:** with first-fit, the first 256-page run can't start at `0x40000000`, because only 128 pages sit below the kernel at `0x40080000`. The scan has to skip the reserved kernel pages, and the oracle pins the run against its own free set.
- **overcap is honest but weak:** see #6.

## Defects, ranked

1. **`.github/workflows/ci.yml:60-69` — the physmem oracle never runs in CI.** No CI step passes `ARM_PHYSALLOC=1`, so the free-count oracle, the first-fit pin and the final rollback-count check only ever run locally. `physmem_bringup()` runs in every build (`main.c:679`), yet the default smoke apparently passes without pinning those lines. Fix: add a `make smoke-aarch64 ARM_REQUIRE=1 ARM_PHYSALLOC=1` step. Better still, make the physmem block part of the default good-boot markers so an unexpected or missing line fails every profile.

2. **`docs/HANDOFF.md` (new block) and `docs/IMPLEMENTATION.md:62-65` describe code that no longer exists.** This is an honesty defect.
   - They document a `physmem rollback ok pages=` marker and claim the bring-up "preflights requests above that bound as `JE_ERR_NOMEM` without mutating allocator state". The current code emits `allocmax`/`overcap` and expects `JE_ERR_INVALID`; no preflight exists.
   - The "observed" fixture output includes a `rollback ok` line that the staged kernel cannot print. That output came from an earlier build.
   - HANDOFF says `smoke_arm.py` "does not yet accept `--phys-alloc`", but the diff adds it.
   - The marker list omits `allocmax` and `overcap`.

   Fix: regenerate both entries from a run of the staged code.

3. **The ARM window arithmetic is tested only by QEMU smoke, never by a hosted test.** `test_physmem.c:36` static-asserts `BASE==0`, and the Makefile builds it without `-DJE_ARCH_AARCH64`.
   - So the `a < BASE` wrap rejection (`alloc.c:86`) and the `BASE + i*PAGE` mapping (`alloc.c:17`) never run under ASan.
   - Smoke never frees an address below `BASE`, so the invariant you called out has no test coverage at all.
   - My probe shows the code is correct today, but nothing would catch a regression.

   Fix: add a second hosted build of the allocator tests with `-DJE_ARCH_AARCH64` that asserts INVALID with state unchanged for `0`, `BASE-PAGE`, `LIMIT` and `UINT64_MAX&~0xfff`, and that every allocation returns `>= BASE`.

4. **`include/jeios/boot.h:21-23` overclaims "BYTE-IDENTICAL".** The x86 object changes: `je_pages_init_pred` and `boot_ctx_free_pred` are new symbols, text grows from 1350 to 1630 bytes, and `je_pages_init`'s code changed. The accurate claim is: behavior-identical, with `alloc`/`free`/`heap` code identical at `-O2`. Reword it.

5. **`physmem.c:10-15` — the intersection test fails open on a range whose end wraps past 2^64.** I confirmed it on the host: `rsv={0x40000000, UINT64_MAX}` or `kernel={0x40000000, UINT64_MAX}` makes page `0x40100000` report **free**.
   - In the live boot this is unreachable: the parser rejects overflowing ranges (BAD_RANGE) and `je_boot_ctx_check` validates kernel/dtb.
   - However, the header promises the predicate is "total" and "safe to reuse outside the current window". A wrapped end makes contains-page fail safe, but makes intersects-page fail **open**.
   - Fix: `if (range.size > UINT64_MAX - range.base) return true;` in `range_intersects_page`, plus a hosted test.

6. **`main.c:610-618` — the overcap marker proves almost nothing about the kernel.** It only exercises the `n > JE_ALLOC_MAX_PAGES` argument check, which returns before any scan and is already covered by `test_alloc`.
   - Its only real value is the printed `free_count`, which the oracle compares to its own count. That is a genuine end-of-sequence rollback check, but `freemax` at `main.c:603` already enforces it in the kernel.
   - The label is honest; it is not a vacuous "rollback ok". Consider renaming it `physmem final pages=` so nobody reads it as allocator-rollback evidence.
   - The real "a failed allocation leaves state untouched" property is proven only by `test_physmem` (fragmented fixture). That is acceptable if the PR states it.

7. **`boot.h:29-35` — a missing `-DJE_ARCH_AARCH64` silently selects the x86 window.**
   - Any aarch64 translation unit compiled without the flag gets `JE_PAGE_COUNT=262144` instead of 65536. That gives a different `je_pages` layout across objects (an ODR/ABI mismatch) and the wrong window, with no diagnostic.
   - It is safe today because `ARM_CFLAGS` sets the flag for every ARM object.
   - Fix: add `#if defined(__aarch64__) && !defined(JE_ARCH_AARCH64)` → `#error`, and use `#elif defined(__x86_64__)` … `#else #error` instead of a silent default.

8. **Lower-severity items:**
   - **No degraded-profile report for RAM outside the window** (`main.c` `physmem_bringup`). With `-m` above 256M, RAM past `0x50000000` is silently unmanaged. That fails safe, but AGENTS rule 5 asks for an explicit degraded profile. Print the unmanaged RAM bytes.
   - **Overclaim in `physmem.h`:** it lists a "stack guard" as covered by the image reservation. No guard page exists (linker.ld has only a 16 KiB `.stack`). Say "planned for PR-1b".
   - **Missing make dependency (`mk/aarch64.mk`):** the `memory.o` rule lists no header dependencies. Harmless today since `memory.c` includes only `<stddef.h>`.
   - **Dead x86 code linked into the ARM image (`mk/aarch64.mk`):** `bootctx.o` pulls the whole x86 multiboot parser (`je_boot_parse`, …) in just to satisfy `je_pages_init`'s reference to `je_boot_free_page`. That adds dead untrusted-input parsing code to the ARM kernel. Consider moving `boot_ctx_free_pred` and `je_pages_init` out of `alloc.c` so ARM links only `je_pages_init_pred`.
   - **`alloc1` returns the page at `0x40000000`:** QEMU wrote its boot stub there (`loader_start`). The stub is dead after kernel entry, so reusing it matches what Linux does. It is still memory that no DTB entry proves free; record it as an accepted assumption rather than "proven free".

None of this was run in remote CI; the x86 disassembly comparison and the host probes ran locally.
