# ADR 0011: MP2 aarch64 kernel MMU and physical memory bring-up

Status: **accepted** (2026-11-03). Owner-approved after a two-lineage design
review (Codex implementor-eye + Claude adversarial-VMSA; transcripts under
`docs/adr/debates/0011-mp2-mmu-design-review/`). This ADR pins the aarch64
kernel-MMU and physical-allocator contract for the QEMU `virt-8.2` /
`cortex-a72` target. It does NOT cover the per-process address-space backend
(the `je_vm_ops` aarch64 analogue) or EL0 userspace — those are MP3 — nor the
GIC/architected timer or scheduling, which are later MP2 steps.

Register values are pinned against Arm DDI0487 (AArch64 VMSA) and the cortex-a72
TRM. The sibling `jeios-mobile/docs/ARM64.md` "MP2" section states requirements
but specifies no register values; this ADR is the core contract it defers to,
cross-linked from mobile ADR 0002.

## How this ADR was decided (provenance)

A full design review ran **before any code**. Both lineages independently
converged on: splitting the slice into two PRs; several concrete bugs in the
first ADR draft (wrong MAIR Device byte, wrong table cache-maintenance op, a
missing Access-Flag pin, fault tests that only checked EC); and a set of
owner-escalation decisions. The owner approved all recommendations on
2026-11-03. The sections below record each decision with the ADR 0000 three
parts (decision + why, what the alternative would have cost, flex to revisit).
Where the two lineages disagreed (the kernel VA regime, D2) the disagreement and
its resolution are recorded explicitly rather than smoothed over.

## Context (verified ground truth, 2026-11-02)

MP1 left the aarch64 port at: EL1 entry (`boot.S`), a bounded PL011 console, the
EL1 exception vectors + trap frame, and a validated FDT/boot-context parse
(`je_boot_ctx` from `fdt.h`), with **MMU off, caches off, no atomics**. `boot.S`
checks load-address == link-address (0x40080000), sets an interim
`TCR_EL1 = 0x80000000` (TBI0=0, MMU off) and `SCTLR_EL1 = 0x30D00808`
(M=0,C=0,I=0,A=0,SA=1,RES1) as explicit placeholders the MMU PR replaces, and
saves the DTB physical address in `.data` as `je_arm_dtb_phys`. `main.c`
`je_arm_main()` copies the DTB into a 64 KiB `.bss` buffer `je_arm_dtb_copy`,
parses it into `je_arm_boot_ctx`, and reads `ID_AA64MMFR0_EL1.PARange` (profile
= 44 bits). Nothing consumes the parsed memory map yet.

Reuse vs new:
- **Reuse (arch-neutral):** the `je_pages` bookkeeping algorithm (alloc.c), and
  the `fdt.c` parser that already fills `je_arm_boot_ctx`.
- **Fix/bridge:** `je_pages` hardcodes `JE_PHYSICAL_LIMIT = 0x40000000` and
  consumes the x86 `je_boot_context` (boot.h). ARM RAM on this target is
  `[0x40000000, 0x50000000)` and ARM produces `je_boot_ctx` (fdt.h).
- **New:** the aarch64 kernel MMU layer (the `paging.c` analogue), the
  per-section linker symbols + stack-guard slot for W^X, and the asm enable leaf.
- **Deferred (MP3):** the aarch64 `je_vm_ops` per-process page-table backend and
  the high-half kernel VA split.

## Decision 0 — split into two stacked PRs (D-SPLIT)

**Decision.** PR-1 is delivered as two stacked, independently-provable PRs:

- **PR-1a — physical allocator, MMU still OFF.** The `je_pages` window
  parameterization (D5), the `je_boot_ctx`→allocator default-deny adapter, all
  reservation classes honored, transactional rollback, and the allocator tests
  (bounds, fragmented rollback, exhaustion against an independent oracle). Proven
  on QEMU with the MMU still off, so it is a pure memory-bookkeeping change.
- **PR-1b — MMU enable.** The linker W^X end-symbols + guard slot (D4), the
  descriptor builder + pre-enable W^X/alias audit, the corrected assembly enable
  leaf (D6), and the `AT`/`PAR_EL1` + fault-mode proofs (D7). Builds on PR-1a's
  allocator for its page-table frames.

**Why.** Both review lineages judged the combined slice too large to review or
prove safely in one PR: it mixes a tested-allocator change with low-level MMU
assembly, and the two have a clean seam (the allocator produces frames; the MMU
consumes them). Splitting lets PR-1a's allocator be proven with the MMU off
(no VMSA confound) and PR-1b's MMU be proven against a real reservation-aware
allocator. It also matches the project's small-reviewable-PR discipline.

**Alternative cost.** One combined PR would entangle an owner-escalated
tested-path change (the allocator) with ~200 lines of boot assembly and a new
page-table builder; a reviewer could not isolate a regression to one or the
other, and a mutation in either half would be harder to attribute.

**Flex.** If PR-1a turns out trivial once written, the two may still be reviewed
as a stack but the split stands for evidence purposes.

## Decision 1 — scope and order (D1)
PR-1 delivers, on QEMU `virt-8.2`, in the two PRs above: (a) the physical page
allocator consuming the parsed RAM map and reservations, and (b) the kernel MMU
enable with a W^X kernel mapping, guard pages, device MMIO mapped
Device-nGnRnE, and correct cache/TLB/barrier maintenance. The per-process
address space (MP3) and GIC/timer/scheduling (later MP2 steps) are out of scope
and must be **absent or fail-closed, not stubbed as working**.

## Decision 2 — kernel VA regime: IDENTITY in TTBR0 for MP2 (D2)

**Decision.** The kernel stays **identity-mapped in TTBR0_EL1** for MP2
(VA == PA, kernel at 0x40080000 as linked). `TCR_EL1.EPD1 = 1` and
`TTBR1_EL1 = 0`; no high-half mapping exists in MP2. The high-half (TTBR1)
kernel split is deferred to the first PR of MP3. The descriptor builder is
written **VA-generic** — `build(root, va, pa, size, perms, attridx)` — so MP3
reuses it unchanged for both the high-half kernel map and per-process TTBR0 maps.

**Lineage disagreement, recorded.** The implementor-eye lineage (Codex)
recommended going high-half **now** to avoid performing the riskiest relocation
later in MP3. The adversarial lineage (Claude) recommended identity-for-MP2, and
the owner chose identity. The decisive arguments for identity:
1. A high-half kernel must be **relinked** off 0x40080000 (or built PIC with
   relocation processing); `adrp` survives but literal pools, `.quad` symbol
   addresses, function pointers, and `je_arm_dtb_phys` consumers do not. That
   edits **MP1-tested `boot.S`** and its load==link check — a tested-path change
   that would itself require owner escalation.
2. A high-half kernel needs a **linear map of RAM** to touch allocator frames;
   that linear map re-aliases `[_text, __rodata_end)` as RW, creating a
   W^X-alias hole (a frame reachable RX via the kernel map and RW via the linear
   map) that identity mapping does not have. Closing it is extra MP3 work; not
   opening it in MP2 is strictly safer.
3. `TTBR1_EL1` is UNKNOWN at reset; enabling TTBR1 (EPD1=0) without a correct
   table base lets speculative high-half walks read a garbage table — a security
   hazard invisible on QEMU. EPD1=1 forecloses it.

**Alternative cost.** Identity defers the high-half relocation to MP3, where it
is done once alongside the per-process TTBR0 backend. Because the builder and
the W^X audit are VA-generic, only the TCR values and a small enable stub change
in MP3; little is thrown away.

**Flex.** Revisit at the MP3 first PR. If a future need (KASLR, a shared
high-half trampoline) forces high-half earlier, it returns to the owner as a
tested-`boot.S` change.

## Decision 3 — granule, PA width, MAIR, TCR (D3, pinned to DDI0487)

- **Granule 4 KiB**, matching the Image-header flag and cortex-a72.
- **PA width:** read `ID_AA64MMFR0_EL1.PARange` at runtime. **Acceptance rule
  (pinned, unambiguous): measured PARange encoding `< 0b0100` (< 44 bits) is
  FATAL; `>= 0b0100` sets `TCR.IPS = 0b100` (44 bits).** (The first draft's "fail
  on mismatch" vs "cap at profile" ambiguity is resolved to this.)

**MAIR_EL1** (two attributes; unused indices are Device, never Normal, so a
stray AttrIdx lands on Device not cacheable RAM):

| Index | Byte | Meaning |
| --- | --- | --- |
| AttrIdx 0 | `0xFF` | Normal, Inner+Outer Write-Back non-transient, RW-allocate (RAM) |
| AttrIdx 1 | `0x00` | **Device-nGnRnE** (MMIO; PL011 @ 0x09000000) |
| AttrIdx 2..7 | `0x00` | unused → Device (fail-safe) |

→ `MAIR_EL1 = 0x00000000000000FF`.
**Bug fixed from draft:** the device byte is `0x00` (Device-nGnRnE), **not
`0x04`** — `0x04` is Device-nGnR**E**, which is not the ordering the ADR claims.

**TCR_EL1** (4 KiB granule, 48-bit input, 4-level walk, identity in TTBR0):

| Field | Bits | Value | Note |
| --- | --- | --- | --- |
| T0SZ | [5:0] | 16 | 48-bit VA, walk starts at L0 (pinned; `_Static_assert` ties start level to T0SZ) |
| EPD0 | 7 | 0 | TTBR0 walks enabled |
| IRGN0 | [9:8] | `01` | walk mem Inner WB RA/WA |
| ORGN0 | [11:10] | `01` | walk mem Outer WB RA/WA |
| SH0 | [13:12] | `11` | Inner Shareable |
| TG0 | [15:14] | `00` | **4 KiB** |
| T1SZ | [21:16] | 16 | defined though EPD1=1 |
| EPD1 | 23 | **1** | TTBR1 walks DISABLED (D2) |
| TG1 | [31:30] | **`10`** | **4 KiB — TG1 encoding differs from TG0** |
| IPS | [34:32] | `100` | 44-bit PA (from PARange, per acceptance rule) |
| TBI0/TBI1 | 37/38 | 0 | |
| HA/HD | 39/40 | 0 | **no hardware Access flag → software sets AF=1 in every live descriptor** |

The implementation generates TCR symbolically from named field macros and a
`_Static_assert` on the composed value; it does not hardcode a magic constant.
Easiest silent-mistranslation traps, called out for the builder + tests: **T0SZ
vs table depth** (build 4 levels but T0SZ≠16 → walker reads L0 as L1), **TG1=`00`**
(reserved; copying the TG0 encoding), and **EPD1 left 0 with TTBR1 unwritten**.

## Decision 4 — W^X mapping, Access Flag, guard page (D4)

Leaf descriptor permissions, per section (AP in descriptor bits [7:6];
`AF` bit 10 = 1 on **every** live leaf; `SH` = `11` on RAM leaves):

| Region | AP[2:1] | PXN | UXN | AttrIdx | AF |
| --- | --- | --- | --- | --- | --- |
| `.text.boot`/`.text.vectors`/`.text` | `10` (EL1 RO) | 0 | 1 | 0 | 1 |
| `.rodata` | `10` | **1** | 1 | 0 | 1 |
| `.data`/`.bss`/`.stack` + page tables | `00` (EL1 RW) | 1 | 1 | 0 | 1 |
| Device MMIO | `00` | 1 | 1 | **1** | 1 |

Invariants (all **audited by a descriptor-level pass before MMU enable**, not
inferred from a passing boot):
- **No leaf is writable-and-executable.** No physical frame is reachable through
  both an executable leaf and a writable leaf (checked by PA, across the whole
  table — under identity this is one map, so the check is cheap and total).
- **`AF = 1` on every live leaf.** (Without it the access faults with the *same
  EC* as a permission fault — see D7; pinning AF=1 keeps the permission tests
  honest.)
- **`AP[1] = 0` on every kernel leaf** (no EL0 access); audited so a stray
  `AP=11`/`AP=01` can't silently open the kernel to future EL0.
- **`APTable/PXNTable/UXNTable = 0`** at every table level (audited), so the
  hierarchy never silently restricts a leaf below what its own bits say.
- **`SCTLR_EL1.WXN = 1`** as defense-in-depth (writable ⇒ XN regardless of PXN).
  WXN is explicitly **not** a substitute for correct PXN bits: the tests exercise
  the descriptor bits directly (the audit) AND effective behavior (the faults),
  because WXN can *mask* a cleared-PXN mutation on writable pages — and WXN does
  nothing for read-only `.rodata`, so a `.rodata` PXN=0 hole needs its own test.

**Bug fixed from draft:** the draft never pinned `AF`, and had no
execute-from-`.rodata` obligation; both are the kind of silent hole this
project's mutation discipline exists to catch.

Per-section boundaries require new **page-aligned** linker symbols
`__text_end`/`__rodata_end`/`__data_end` with `ASSERT`s that each is page-aligned,
and the image region mapped with **L3 (4 KiB) leaves** (a 2 MiB block at
0x40000000 would merge `.text` and `.data` permissions). The **guard page** gets a
dedicated `.stack_guard (NOLOAD)` 4 KiB slot with
`ASSERT(__stack_guard + 0x1000 == __stack_bottom)`; it is never mapped and stays
RESERVED in the allocator. **Known-not-enforced (D2/owner, option a):** a *real*
stack overflow currently re-faults forever because the EL1 vector does
`sub sp, sp, #JE_TF_SIZE` on the same SP_EL1; the guard is proven only by an
explicit probe. The separate-exception-stack fix (kernel on SP_EL0, vectors on
SP_EL1) is deferred to its own later step and recorded as not-enforced in PR-1b
and HANDOFF.

## Decision 5 — allocator: parameterize window + default-deny adapter (D5)

**This widens `je_pages`, a tested M1/M2 path — owner-escalated and approved.**

- Replace the hardcoded `0x40000000` ceiling with per-arch compile-time
  `JE_PHYSICAL_BASE`/`JE_PHYSICAL_LIMIT`. **x86 → `0` / `0x40000000` (a
  byte-identical no-op); ARM → `0x40000000` / `0x50000000`.** The bookkeeping
  algorithm is unchanged; only the window is parameterized. Guard: the existing
  x86 allocator tests + `make smoke` must be unchanged/green.
- Address arithmetic uses **64-bit** throughout: `base + index*PAGE_SIZE`,
  `limit - base`, reservation endpoints. `je_pages_alloc` returns
  `base + start*PAGE_SIZE` (not `start*PAGE_SIZE`). `je_pages_free` rejects
  `a < base` **before** computing `(a - base)/PAGE_SIZE` (else `start` wraps and
  the bounds check passes on an out-of-range index → OOB read). Tests: `free(0)`,
  `free(base-4096)`, `free(limit)`, `free(limit-4096, 2)`.
- The `0` sentinel (`*out = 0` means failure) must never be allocatable; on ARM
  `base > 0` ensures it, on x86 page 0 stays reserved — pinned with a check.
- An init variant takes a **default-deny predicate** `is_free(ctx, pa)`; the x86
  `je_boot_context` path is wrapped as one such predicate (so there is ONE
  allocator, not a fork). The ARM adapter's predicate is
  **"`pa` is inside a DTB RAM range AND inside no reservation / kernel image /
  DTB source+copy / boot stack / stack-guard / initial page tables / initrd /
  `rsv_dynamic`"**. Dropping the RAM-membership half still passes at `-m 256M`,
  so the exhaustion test (D7) uses an independent oracle to catch exactly that.
  The allocator never learns FDT types — the adapter translates.
- Allocation is transactional: a failed multi-page alloc frees nothing (D7
  rollback test).

**Why default-deny.** `alloc.c` encodes FREE as state `0`; `.bss` is zero. A page
the adapter *fails to classify* defaults to allocatable — the exact PR-H
".bss-zero means ALLOW" defect class. The predicate is written so the safe answer
(reserved/unavailable) is the one you get on any omission.

## Decision 6 — cache, barrier, TLB maintenance + enable leaf (D6, pinned)

**Table maintenance (bug fixed from draft).** Initial tables are built with the
MMU and D-cache **off**, i.e. written with Device-nGnRnE semantics straight to
memory; the walker (post-enable) reads them as Normal cacheable per TCR. If a
*stale dirty* line covered the table range, a **clean** would write it back over
the correct table. Therefore maintenance is **invalidate-only `DC IVAC`** over
the table range (stride from `CTR_EL0.DminLine`, not a hardcoded 64), done after
the tables are written and before enable — **not** the draft's "clean+invalidate".

**Pre-enable invalidation scope.** Everything the kernel wrote with C=0 can be
shadowed by a stale line once C=1: `.data` (incl. `je_arm_dtb_phys`), all `.bss`
(zeroed by `boot.S`), the boot stack, the DTB copy + `je_arm_boot_ctx` (the MP1
cache note), and the tables. Invalidate the whole writable range
`[__data_start, _end)` plus the DTB copy buffer, by `DC IVAC`, stride from
`CTR_EL0.DminLine`.

**Enable leaf (single assembly leaf, no stack/memory writes between the
invalidate and `SCTLR.C=1`).** C code must not run in that window because it
would push frames onto a stack whose lines were just invalidated:

```
build tables (MMU off)
DC IVAC over [tables] and [__data_start,_end) + DTB copy ; DSB SY
MSR MAIR_EL1 ; MSR TCR_EL1 (EPD1=1) ; MSR TTBR0_EL1 ; MSR TTBR1_EL1=0
DSB SY ; ISB
TLBI VMALLE1 ; DSB NSH ; ISB            // reset TLB state is IMPLEMENTATION DEFINED
MRS/ORR SCTLR_EL1 |= M|C|I|WXN ; MSR SCTLR_EL1 ; ISB   // one write
IC IALLU ; DSB NSH ; ISB                // instrs fetched MMU-off may be stale
```

Every barrier's reason is documented inline. `WXN` is set in the **same** SCTLR
write as `M` (after the TLBI) because it can be cached in the TLB; it is never
toggled later without another TLBI. The exception vector, its stack, the handler
code and its data are mapped and usable **before** enable, so an MMU-enable fault
is reportable (not a silent double fault). Later mapping changes (MP3) use
`DSB ISHST; TLBI VAE1IS/VMALLE1IS; DSB ISH; ISB`; executable publication uses
`DC CVAU + IC IVAU + barriers` — neither is needed for the once-mapped static
kernel. ASIDs: the kernel map is global (nG=0); no user ASID exists yet, and the
ADR records that an ASID is never recycled without a TLBI.

**QEMU caveat (honesty gate).** QEMU TCG models no caches, so a sequence missing
the maintenance above still boots green. PR-1b and HANDOFF MUST state: QEMU
validates translation and permissions, **not** cache maintenance or barriers —
those are reviewed, not observed.

## Decision 7 — failure behavior + proofs (D7, fail-closed, tested)

**Fail-closed (FATAL marker, never degrade), each with a fault-injection test
mode that asserts the marker appears AND no later marker does:** PARange below
profile; RAM map empty / overlapping the image; a reservation overlapping the
kernel; insufficient free RAM for the initial tables; an MMU-enable fault.

**Proofs (smoke sub-tests; faults asserted by full ESR, not EC alone).** The
recurring defect is "a green test that passes though the property is broken," so
each invariant names the mutation it must defeat:

| Invariant | Observation that proves it (defeats the stated mutation) |
| --- | --- |
| MMU really on | Map a test frame P at a **non-identity** VA V (RW+XN); write a magic value via V, read it back via P's identity addr; assert `AT S1E1R V` ⇒ `PAR_EL1.PA == P`. Then unmap V + run TLBI and re-read V ⇒ translation fault. (Defeats "read a software flag / identity-everything-RWX / missing TLBI".) |
| write-to-`.text` faults | Target a real `.text` symbol; assert EC `0x25`, **DFSC `0b001111`** (permission, L3), **WnR=1**, FAR==target; `AT S1E1W` ⇒ F=1, `AT S1E1R` ⇒ F=0. (Defeats "missing AF / translation fault / wrong-page target", all of which share EC.) |
| execute-from-`.data` faults | First read+write the buffer OK, write a `ret`, branch to it; assert EC `0x21`, **IFSC `0b001111`**, FAR==ELR==buffer; audit asserts PXN=1 on every writable leaf. (Defeats "unmapped `.data` (EC 0x21 via translation) / PXN=0 masked by WXN".) |
| `.rodata` is execute-never | Branch into `.rodata` ⇒ IFSC permission L3; write to `.rodata` ⇒ DFSC permission, WnR=1. (Defeats "`.rodata` PXN=0", which WXN does not cover and the draft never tested.) |
| MMIO attributes | `AT S1E1R` on the UART VA ⇒ `PAR_EL1.ATTR[63:56]==0x00`; on a RAM VA ⇒ `0xFF`, `PAR.SH==11`. (Defeats "UART mapped Normal" — QEMU prints either way.) |
| stack guard | `ldr` of `__stack_bottom-8` (from the linker symbol) while SP valid ⇒ DFSC `0b000111` (translation, L3), FAR==that addr; linker `ASSERT` keeps the guard adjacent. (Real overflow = hang, recorded not-enforced.) |
| rollback | Fragment so `free_count>=n` but no contiguous run of `n`; assert NOMEM and that a digest of `state[]`+`free_count` is identical before/after; inject failure at each step. (Defeats "partial commit / early NOMEM before the scan runs".) |
| reservations honored | **Exhaustion vs independent oracle:** allocate one page at a time to NOMEM, emit the count + a hash of returned frames; `smoke_arm.py` computes the expected free set from the **DTB fixture in Python**, not the kernel's own math. Add a `-m 128M` / hole fixture and a fixture reservation placed inside free RAM so ignoring it would hand it out. (Defeats "predicate drops RAM-membership / misses rsv_dynamic — nothing breaks at 256M by luck".) |

Markers carry full ESR/FAR/ELR as HEX16 and the harness matches exact values; a
halted emulator with no marker is failure.

## Evidence required (per mobile ARM64.md + core discipline)
Core commit, aarch64 toolchain + QEMU versions, the pinned
`virt-8.2`/`cortex-a72` invocation, Image hash, serial log, and exact ordered
markers. Prove, for PR-1a: the RAM map consumed with every reservation class
honored; a successful allocation and a rolled-back failed allocation; the
exhaustion count+hash matching the Python oracle on two RAM sizes; x86 allocator
tests + `make smoke` unchanged. For PR-1b: MMU enabled (non-identity translation
via `AT`/`PAR`); write-to-code, execute-from-data, execute-from-rodata and
stack-guard faults with exact ESR/FAR; the pre-enable W^X/alias audit passing;
MP1 markers still pass. AMD64 regression gates retained. **This qualifies the
QEMU virtual board only** — not a physical ARM SoC or Pixel — and QEMU does not
validate cache/barrier maintenance. AI-assisted review is not independent human
qualification.
