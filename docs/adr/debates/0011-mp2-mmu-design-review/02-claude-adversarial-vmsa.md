# ADR 0011 adversarial review (VMSA correctness)

**Verdict: not ready to code.** I found five defects that will either fail silently or let a test pass while the property it checks is broken:

1. **D6 uses the wrong cache operation for the page tables.** It says to clean+invalidate them to PoC. Tables written with the MMU and caches off need invalidate-only (`DC IVAC`), done once before the tables are built and once after.
2. **D7's fault tests only check the exception class (EC).** A translation fault or an Access-flag fault has the same EC as a permission fault, so the tests pass even when the permission is wrong.
3. **Real stack overflow cannot be caught.** `vectors.S:86` starts with `sub sp, sp, #JE_TF_SIZE` on the same SP_EL1, so a real overflow faults again in the vector forever.
4. **There is no execute-from-.rodata test.** With `SCTLR.WXN=1`, the execute-from-.data test passes even if every data PTE has PXN=0. Nothing in the plan catches `.rodata` with PXN=0.
5. **The allocator treats .bss-zero as allowed.** In `alloc.c`, `state==0` means FREE, which is the same defect class as PR-H. Any page the RAM adapter fails to mark becomes allocatable.

I grounded this in `linker.ld`, `boot.S`, `vectors.S`, `alloc.c` and the ADR text. I have not built or run anything.

---

## 1. TTBR1 high-half vs TTBR0 identity

**Decision: keep the kernel identity-mapped in TTBR0 for MP2.** Set `TCR.EPD1=1` and `TTBR1_EL1=0`, and make moving to the high half the first PR of MP3. The ADR's current D2 says TTBR1, and I think that is wrong for this PR.

Why:
- **A high half can't be done by inserting a trampoline.** The kernel is linked at 0x40080000, and `boot.S` checks that load address equals link address. Moving to a high VA means relinking at the high VA (or building position-independent with relocation processing). `adrp`-only code survives that, but literal pools, `.quad` symbol addresses, function pointers and `je_arm_dtb_phys` users do not. All of that changes MP1-tested boot code, which must go to the owner.
- **The enable sequence would need extra steps the ADR leaves out.** Even with a relink, it still needs:
  - an identity map of the enable page in TTBR0;
  - a `br` to the high VA;
  - re-pointing `VBAR_EL1` and `SP` at high VAs;
  - removing the identity map, followed by TLBI.
- **A high half adds a W^X alias hole that identity does not have.** A high-half kernel needs a linear map of RAM so it can touch frames the allocator hands out. That linear map covers the kernel image, so `.text` frames become reachable twice: RX through the kernel VA and RW through the linear alias. Under identity mapping, the linear map and the kernel map are the same mapping, so each frame has one set of permissions and there is no alias. MP3 must explicitly map the linear alias of `[_text, __rodata_end)` read-only and XN; the ADR doesn't mention this today.
- **Little is thrown away.** If the descriptor builder takes `(root, va, pa, size, perms)` and the W^X audit runs on any root, MP3 reuses all of it. Only the TCR values and the enable stub change.

Minimal correct sequences:
- **Identity (recommended):** build tables (MMU off) → invalidate tables to PoC → `MSR MAIR, TCR (EPD1=1), TTBR0` → `ISB` → `TLBI VMALLE1` → `DSB NSH` → `ISB` → `MSR SCTLR` (M, C, I, WXN) → `ISB` → `IC IALLU` → `DSB NSH` → `ISB`. This must be one assembly leaf function that does no stack or memory writes; see Q2.
- **High half:** the same sequence, plus the four steps listed above, plus the relink.

If the owner still wants TTBR1 now, this is an **escalation**: it changes MP1-tested `boot.S` and its load==link check.

## 2. Enable-sequence correctness

`DSB ISH; ISB` before the SCTLR write and `ISB` after it is **not sufficient**. Missing or wrong:

1. **No `TLBI VMALLE1` before enable.** Whether TLB contents are invalid at reset is implementation defined. Do `TLBI VMALLE1; DSB NSH; ISB` after programming TTBR and TCR and before setting M.
2. **No `IC IALLU` after enable.** Instructions fetched while the MMU was off may have been cached with different attributes. Do `IC IALLU; DSB NSH; ISB` after the SCTLR `ISB`.
3. **D6 specifies the wrong table maintenance.** Tables written with the MMU off are written with Device-nGnRnE semantics straight to memory. TCR then makes the walker cacheable (IRGN/ORGN=WBWA). If a stale *dirty* line covers a table, "clean" writes it back over the correct table. Use `DC IVAC` over the table range before writing the tables and again after, with the stride taken from `CTR_EL0.DminLine`, not a hardcoded 64.
4. **The DTB-copy-only invalidate is incomplete.** Everything the kernel wrote with C=0 can be shadowed by a stale line once C=1: `.data` (including `je_arm_dtb_phys`), all of `.bss` (zeroed by `boot.S`), the boot stack, and the tables. Invalidate the whole writable range `[__data_start, _end)`.
5. **The invalidate and the enable can't be in C.** If C code runs between the invalidate and `SCTLR.C=1`, it pushes stack frames that were just invalidated. Both must happen in one assembly leaf with no stack use. Setting M, C and I together in one write is fine.
6. **SCTLR value:** start from the MP1 `0x30D00808`, keep the RES1 bits, set M (bit 0), C (bit 2), I (bit 12) and WXN (bit 19), and check `EE=0`. WXN may be cached in the TLB, so it must be set in the same write as M, after the TLBI. Don't toggle it later without another TLBI.
7. **QEMU can't validate any of this.** TCG does not model caches, so a sequence with none of items 1–5 still boots green on QEMU. The ADR and HANDOFF must say "cache maintenance reviewed, not observed", and smoke results must not be cited as evidence for it.

## 3. MAIR and TCR fields

| Field | Bits | Value | Notes |
|---|---|---|---|
| MAIR Attr0 | [7:0] | `0xFF` | Normal, inner and outer write-back, non-transient, read/write-allocate |
| MAIR Attr1 | [15:8] | `0x00` | Device-nGnRnE |
| MAIR Attr2..7 | | `0x00` | Unused indices must be **Device**, not Normal (see mutation 3c) |
| T0SZ | [5:0] | 16 (48-bit, 4 levels, walk starts at L0) **or** 25 (39-bit, 3 levels, walk starts at L1) | Pick one and pin the start level in a `_Static_assert` |
| EPD0 | 7 | 0 | |
| IRGN0 / ORGN0 | [9:8] / [11:10] | `01` / `01` | Write-back, write-allocate |
| SH0 | [13:12] | `11` | Inner shareable |
| TG0 | [15:14] | `00` | **4 KiB** |
| T1SZ | [21:16] | 16 | Defined even while EPD1=1 |
| EPD1 | 23 | **1** | |
| TG1 | [31:30] | **`10`** | **4 KiB.** TG1 uses a different encoding: `01`=16K, `10`=4K, `11`=64K; `00` is reserved |
| IPS | [34:32] | **`100`** | 44 bits; a measured PARange of `0b0100` maps directly |
| AS | 36 | 0 | |
| TBI0 / TBI1 | 37 / 38 | 0 | |
| HA / HD | 39 / 40 | 0 | There is no hardware Access flag, so software must set it |

The fields most likely to mistranslate silently:

- **T0SZ vs. table depth (most likely).** If the builder builds 4 levels but T0SZ=25, the walker reads the L0 table as L1, and every VA translates through the wrong index bits. Some identity pages can land correctly by coincidence, so a "boots and prints" test can pass. Derive start level and table count from T0SZ in one header, and verify with an `AT` probe of a non-identity VA (Q6).
- **TG1=`00`.** Copying the TG0 encoding into TG1 gives a reserved value, which hardware may treat as another granule size. MP1's interim `0x80000000` sets TG1=`10` correctly; make sure the new value keeps that.
- **EPD1=0 with TTBR1 never written.** `TTBR1_EL1` is UNKNOWN at reset. Any high-half access, including speculative ones, walks a garbage table base. That is a security hole, not just a crash, and it is invisible on QEMU.
- **Two descriptor bits the ADR never mentions:**
  - **AF (bit 10) must be 1.** Without it, every access takes an Access-flag fault. That fault has the same EC as a permission fault, which feeds the false-success in Q6.
  - **SH (descriptor [9:8]) must be `11` for RAM.** Leaving it non-shareable is harmless on one core but breaks coherency later, and QEMU won't show it.

## 4. W^X on aarch64

Bit-level D4 is consistent. With AP[2:1] in descriptor bits [7:6]:

| Region | AP | PXN | UXN |
|---|---|---|---|
| `.text` | `10` (EL1 read-only) | 0 | 1 |
| `.rodata` | `10` | 1 | 1 |
| RW data | `00` | 1 | 1 |
| MMIO | `00` + AttrIdx1 | 1 | 1 |

Gaps:

1. **The ADR doesn't forbid AP[1]=1 on kernel pages.** Kernel pages must never have AP[1]=1 (EL0-accessible). Two traps:
   - AP=`11` on `.text` still passes the write-to-text test, but leaks the kernel to EL0 in MP3.
   - AP=`01` makes a page implicitly PXN at EL1. That is fail-safe for exec, but it is a real EL0-writable kernel page.

   Add an audit rule that every kernel leaf has AP[1]=0, and check it with `AT S1E0R` returning `PAR_EL1.F=1`.
2. **WXN is defense in depth and can hide PTE bugs.** With WXN=1, any writable page is execute-never no matter what PXN says. So the execute-from-.data test proves WXN, not your PXN bits. WXN does nothing for `.rodata`, which is not writable, so `.rodata` with PXN=0 is a live hole no planned test catches. Add an execute-from-.rodata test. Also run the descriptor audit, which checks PXN bits directly, rather than relying on the fault test.
3. **The linker script can't express the W^X boundaries yet.** `.text.boot`, `.text.vectors`, `.text`, `.rodata` and `.data` are packed with no `ALIGN(4096)`. Add:
   - page-aligned `__text_end`, `__rodata_end` and `__data_end`, plus `ASSERT`s that each is page-aligned;
   - the 2 MiB region holding the image must use **L3 pages**. A 2 MiB block at 0x40000000 merges `.text` and `.data` permissions.
4. **The guard page needs a dedicated linker slot.** Today `.stack` sits directly on `__bss_end`, so the page below the stack is `.bss` (possibly `je_arm_dtb_copy`). Add a `.stack_guard (NOLOAD)` of one 4 KiB page with `ASSERT(__stack_guard + 0x1000 == __stack_bottom)` and a page-aligned `__stack_bottom`. Also:
   - The guard page must never be mapped, and stays reserved in the allocator.
   - **A real overflow hangs** (see the verdict): the vector's `sub sp` faults again on the same SP. Fixing this needs a separate exception stack (kernel runs on SP_EL0, vectors on SP_EL1), or an overflow check at vector entry. Until then, record "stack overflow is detected only by explicit probe; real overflow = hang" as not enforced.
5. **The table walker is not subject to stage-1 permissions.** Confirmed: walks are not instruction fetches and use the TCR IRGN/ORGN/SH attributes. Mapping table frames RW+XN is correct. Leave APTable, PXNTable and UXNTable at 0, and audit that they are 0 so they can't silently restrict the hierarchy.
6. **The no-alias invariant is checkable, but not per call.** The plan doesn't track mappings per frame, so check it with a full table audit before enable. Walk every valid leaf and assert:
   - not (writable and executable);
   - no PA appears both in an executable leaf and in a writable leaf;
   - AF=1, AP[1]=0, and the right AttrIdx per region.

   Re-run the audit after any map change. Under identity mapping this is easy. Under a high half (MP3) the audit must include the linear map; that is where an alias would appear.

## 5. Allocator window

**Escalate to the owner.** AGENTS.md names "the M1/M2 allocator" as previously tested code that agents may not change by majority vote.

The risk isn't the index width (`size_t` is 64-bit, and a 256 MiB window is 65,536 pages). The real risks are in the arithmetic and the defaults:

1. **Allocations ignore the base.** `je_pages_alloc` returns `start*JE_PAGE_SIZE`, which must become `base + start*PAGE_SIZE`.
2. **Out-of-bounds read in `je_pages_free`.** It must reject `a < base` *before* computing `(a - base)/PAGE_SIZE`. Otherwise `start` wraps huge, `JE_PAGE_COUNT - start` underflows, the `n > ...` check passes, and the next line reads `state[start]` out of bounds. Test `free(0)`, `free(base - 4096)`, `free(limit)` and `free(limit - 4096, 2)`.
3. **The 0 sentinel.** `*out = 0` signals failure, so 0 must never be allocatable. On ARM this holds because base > 0. On x86 it relies on page 0 being reserved; add a `_Static_assert` or runtime check either way.
4. **The 16-bit state encoding.** Allocation heads store `n` in a `uint16_t`, with RESERVED=0xFFFF and TAIL=0xFFFE. Add `_Static_assert(JE_ALLOC_MAX_PAGES < TAIL)`.
5. **FREE is 0 (the .bss-zero problem).** The current init explicitly writes every slot, which is good. The ARM adapter's predicate must be default-deny: "inside a DTB RAM range AND not inside any reservation or kernel/DTB/stack/tables/guard range". Dropping the RAM-membership half still passes at `-m 256M`.

**Minimal change:** per-arch compile-time `JE_PHYSICAL_BASE` and `JE_PHYSICAL_LIMIT` (x86: 0 and 0x40000000, a byte-identical no-op; ARM: 0x40000000 and 0x50000000). Plus an init variant that takes a `bool (*is_free)(void *ctx, uint64_t pa)` predicate, with the x86 `je_boot_context` path wrapped as one such predicate. That keeps a single allocator and leaves x86 tests unchanged. Re-run the x86 allocator tests and `make smoke` as the guard.

If the DTB reports RAM outside the compiled window, fail closed (D7). Don't silently ignore it.

## 6. False-success mutations and how to catch them

| Invariant | Mutation that passes a naive test | Observation that catches it |
|---|---|---|
| **MMU really on** | Test reads a software flag, or checks `SCTLR.M` after a build where tables are identity-everything RWX | (a) Map a test frame P at a **non-identity** VA V, RW+XN. Write a magic value through V, read it back through P's identity address, and assert `AT S1E1R V` gives `PAR_EL1.PA == P`. (b) Read an unmapped VA and require a DABT with DFSC=translation fault, FAR=VA. (c) Unmap V, run the TLBI sequence, and re-read V: it must fault. QEMU's software TLB is stale without a TLBI, so this catches a missing TLBI or barrier. Then remove V. |
| **Write-to-.text faults** | `.text` page left without AF, a translation fault, or a gap page outside `.text` used as the target. EC is 0x25 in every case | Target `&some_text_function`. Assert EC=0x25, **DFSC=0b001111** (permission fault, level 3), **WnR=1**, FAR == target. Also require `AT S1E1W target` to give F=1 and `AT S1E1R` to give F=0 before the fault. |
| **Execute-from-.data faults** | (a) `.data` unmapped, which gives EC 0x21 with a translation fault. (b) PXN=0, masked by WXN=1 | First read and write the `.data` buffer successfully. Put a `ret` encoding there and branch to it. Assert EC=0x21, **IFSC=0b001111**, FAR == ELR == buffer. Separately, the descriptor audit asserts PXN=1 on every writable leaf. |
| **.rodata is execute-never** | `.rodata` PXN=0. WXN doesn't cover it, and no test is planned | New test: branch into `.rodata` and require IFSC=permission fault level 3. A write to `.rodata` must give DFSC=permission, WnR=1. |
| **MMIO attributes** | UART mapped Normal-WB, or RAM pointed at AttrIdx 2..7. QEMU ignores cacheability, so both still print | `AT S1E1R` on the UART VA must give `PAR_EL1.ATTR` [63:56] == 0x00. On a RAM VA it must give 0xFF with `PAR.SH` == `11`. Leave MAIR Attr2..7 = 0x00 so a stray index lands on Device, not Normal. |
| **Stack guard** | Guard mapped by an off-by-one range such as `[__bss_end, _end]`, or the guard placed somewhere other than directly under the stack | Probe `__stack_bottom - 8` (taken from the linker symbol) with a plain `ldr` while SP is valid. Assert DFSC=**0b000111** (translation fault level 3), FAR == that address. Keep the linker `ASSERT` that the guard is adjacent. Record that a real overflow is not handled (Q4.4). |
| **Rollback** | Partial commit during the scan. The naive test asks for `n > free_count` and hits the early `NOMEM` before the scan ever runs | Fragment memory so `free_count >= n` but no contiguous run of `n` exists. Assert NOMEM, and assert a digest of `state[]` plus `free_count` is identical before and after. For the heap: inject a `je_map_ram` failure and assert `free_count` is unchanged. |
| **Reservations honored** | Predicate drops RAM membership, or misses `rsv_dynamic`, initrd or tables. Nothing goes wrong at 256M by luck | **Exhaustion test with an independent oracle:** allocate one page at a time until NOMEM, then emit the count and a hash of the returned frames. `smoke_arm.py` computes the expected free set from the DTB fixture in Python, not from the kernel's own math. Also run a `-m 128M` / hole fixture so out-of-RAM pages are exercised, and add a fixture reservation placed inside free RAM so it would be handed out if ignored. |
| **Fail-closed paths** | The FATAL marker is printed but execution continues, or the halt path is dead code | Each D7 condition gets a fault-injection mode. The harness asserts the FATAL marker *and* that no later marker appears. |
| **Fault handler** | Handler treats any EC as success | Markers carry the full ESR, FAR and ELR as HEX16. The harness matches exact values, not "an abort happened". |

## 7. Scope: split into two PRs

Split at the boundary between allocator and MMU. Each half can be proven on QEMU on its own:

- **PR-1a: physical allocator (MMU still off).** The base/limit parameterization (owner escalation), the `je_boot_ctx` adapter, reservations including `rsv_dynamic`, initrd, image, DTB source and copy, and stack. Also the bounds tests for `free`, the fragmented rollback test, and the exhaustion test against the Python oracle. Pass condition: the exhaustion count and hash match the oracle on two RAM sizes, and the x86 allocator tests are unchanged.
- **PR-1b: MMU enable.** Linker alignment, end symbols and guard slot; the descriptor builder plus the pre-enable W^X audit; the assembly enable leaf with the corrected maintenance; and the `AT`/`PAR` and fault-mode tests from the table. Initial tables come from the PR-1a allocator, or from a reserved `.bss` pool if the owner prefers MMU to not depend on the allocator. Pass condition: every row in Q6 shows the exact ESR, FAR and PAR values.

## Owner escalations

1. Widening `je_pages` (tested M1/M2 path), in any form.
2. TTBR1 high half now, if the owner wants it over my recommendation: it changes MP1-tested `boot.S` and its load==link check.
3. The decision to accept that real stack overflow currently hangs, and when to add a separate exception stack. This is a security-relevant invariant that is not enforced.
4. HANDOFF must state that QEMU validates translation and permissions but **not** cache maintenance or barriers.
