# MP2 PR-1 design review brief — aarch64 kernel MMU + physical memory

You are reviewing a DESIGN (ADR 0011) for a fresh C11 microkernel, JeiOS, before
any code is written. Target: QEMU `virt-8.2`, `cortex-a72`, 256 MiB RAM, 4 KiB
granule, EL1 kernel, no userspace yet. Arm ref: DDI0487 (AArch64 VMSA).

Your job is to find design defects NOW, while they are cheap: anything that will
fault silently, ship a security hole, be impossible to build as specified, or
make a false "it works" claim. Be concrete and adversarial. A design that merely
sounds right is not good enough — point at the exact register field, barrier,
ordering, or invariant that is wrong, missing, or unprovable.

## Verified ground truth (MP1, already merged and tested)

- `boot.S` enters at EL1 with **MMU off, caches off, no atomics**. It sets an
  interim `TCR_EL1 = 0x80000000` and `SCTLR_EL1 = 0x30D00808`
  (M=0,C=0,I=0,A=0,SA=1,RES1) as explicit placeholders for the MMU PR to replace.
  DTB physical address saved in `.data` as `je_arm_dtb_phys`.
- `main.c` `je_arm_main()`: copies the DTB into a 64 KiB `.bss` buffer
  `je_arm_dtb_copy`, parses it with `fdt.c` into `je_arm_boot_ctx` (type
  `je_boot_ctx` from `fdt.h`: `ram[8]`, `rsv[16]`, `rsv_nomap[]`, `rsv_dynamic`,
  initrd), reads `ID_AA64MMFR0_EL1.PARange` (pinned profile = 44 bits).
  main.c:252-255 carries a CACHE NOTE: before enabling the D-cache, the DTB copy
  and `je_arm_boot_ctx` must be `DC IVAC`'d or later cacheable reads may hit stale
  Device-memory lines. The MMU/allocator work slots between `dtb_intake()`
  (main.c:542) and the terminal marker (main.c:554).
- `kernel/arch/aarch64/linker.ld`: `. = 0x40080000`; symbols
  `_text,_file_end,__bss_start,__bss_end,__stack_bottom,__stack_top,_end,__image_size`.
  **No per-section end symbols yet** (must add page-aligned
  `__text_end/__rodata_end/__data_end` for W^X). `--orphan-handling=error`.
- `je_pages` (alloc.c): arch-neutral bookkeeping, but hardcodes
  `JE_PHYSICAL_LIMIT = 0x40000000` (boot.h:12), `JE_PAGE_COUNT = limit/4096`, and
  `je_pages_init(p, const je_boot_context*)` consumes the **x86** `je_boot_context`
  (boot.h), NOT the ARM `je_boot_ctx` (fdt.h). QEMU virt RAM = `[0x40000000,
  0x50000000)` (256 MiB) — so the x86 hardcoded 0x40000000 ceiling is exactly the
  base of ARM RAM: the window MUST be parameterized.
- The x86 kernel MMU lives in `kernel/arch/x86_64/paging.c`
  (`je_arch_paging_init`, `je_user_alias_set/get`, `je_arch_page_guard/restore`,
  CR3). The aarch64 analogue is NEW in this PR. The per-process `je_vm_ops`
  backend (vm.h) is explicitly DEFERRED to MP3 — not in this PR.
- Smoke harness `tools/smoke_arm.py`: QEMU `virt-8.2,gic-version=2`, cortex-a72,
  256M, `-kernel`, exact-ordered-marker harness (regex HEX16), asserts a DTB CRC32
  fixture. MMU-era sub-tests add a new `ARM_*` build toggle + `--mode` flag +
  marker list; faults are asserted by EC/FAR via the existing `je_arm_exception`
  path (EC_DABT_CUR 0x25, EC_IABT_CUR 0x21), NOT by timeout. A halted emulator
  with no markers = failure.
- Prior review lesson (PR-H, x86 TSS I/O bitmap): a `.bss`-zero default meant
  "ALLOW"; the fix was to fill 0xFF (default-deny) before `ltr`. The recurring
  defect class across this project is "a green test that passes even if the
  security property is broken." Mutation testing is the acceptance bar.

## The design under review (ADR 0011 decisions, summarized)

- **D1 scope/order:** one slice = physical allocator (consumes parsed RAM map +
  reservations) + kernel MMU enable (W^X kernel map, guard pages, device MMIO
  non-cacheable, correct cache/TLB/barrier maint). GIC/timer/sched and per-process
  AS are out of scope and must be absent/fail-closed, not stubbed-as-working.
- **D2 regime/granule/layout:** 4 KiB granule; PARange read at runtime, profile 44
  bits, mismatch fails closed; IPS from measured PARange capped at profile.
  **OPEN QUESTION (biggest):** kernel in **TTBR1_EL1 high-half** (TTBR0 left for
  MP3 user) vs **keep kernel identity-mapped in TTBR0 for MP2** and defer the
  high-half split to MP3. MP1 links+runs the kernel at identity VA 0x40080000;
  moving to a high-half VA at MMU-enable is a jump needing the documented enable
  sequence. The review must DECIDE this and it gets pinned before code.
- **D3 MAIR_EL1:** AttrIdx0 = Normal Inner/Outer WB non-transient RW-alloc (RAM);
  AttrIdx1 = Device-nGnRnE (MMIO: PL011 at 0x09000000). Device never cacheable.
- **D4 W^X + guard:** per-section: `.text*` RX (AP ro, PXN=0, UXN=1); `.rodata` R
  (PXN=1,UXN=1); `.data/.bss/.stack`+page tables RW+XN (PXN=1,UXN=1); MMIO RW+XN
  AttrIdx1. No-W^X-alias invariant: no frame reachable both executable and
  writable. Unmapped guard page precedes the boot stack.
- **D5 allocator:** replace hardcoded 0x40000000 ceiling with configured
  base+limit (x86 window unchanged); adapter bridges `je_boot_ctx`
  (ram/rsv/rsv_nomap/rsv_dynamic/initrd) into free/reserved view; kernel image,
  DTB source+copy, boot stack, initial page tables, and every reservation (incl.
  rsv_dynamic) marked RESERVED; only free RAM allocatable; transactional alloc,
  rollback-tested (failed multi-page alloc frees nothing).
- **D6 cache/barrier/TLB:** DC IVAC the DTB copy + je_arm_boot_ctx before D-cache;
  clean+invalidate initial tables to PoC; enable sequence: build tables MMU off →
  MSR MAIR/TCR/TTBR1(+TTBR0) → DSB ISH; ISB → MSR SCTLR M=1 (C/I per profile) →
  ISB. TLBI for later changes: DSB ISHST; TLBI VAE1IS/VMALLE1IS; DSB ISH; ISB.
  Kernel mapping global (nG=0 or single reserved ASID); ASIDs never recycled
  without TLBI (none exist yet).
- **D7 fail-closed:** halt with FATAL marker (never degrade) on PARange!=profile,
  empty/overlapping RAM map, reservation overlapping kernel, insufficient RAM for
  tables, or an MMU-enable fault. Post-enable smoke PROVES invariants by observing
  faults: write-to-.text, execute-from-.data, stack-guard touch each produce the
  exact EL1 abort (EC+FAR) via je_arm_exception; rollback proven by markers.

## Specific questions I need answered

1. **D2 — TTBR1 high-half vs TTBR0 identity for MP2.** Which is correct for THIS
   PR? Weigh: the enable-time VA jump (PC is at identity 0x4008xxxx when SCTLR.M
   flips; if the kernel's new VA is high-half, the very next instruction fetch
   after `ISB` is at the OLD PC — is a trampoline / dual-mapping / `br` to the
   high VA required, and does the ADR's enable sequence actually account for it?).
   What is the minimal correct sequence for each option? Which defers less risk to
   MP3 without shipping a throwaway? Give a decision + the exact reason.
2. **Enable-sequence correctness.** Is `DSB ISH; ISB` before the SCTLR write, then
   `ISB` after, sufficient and correctly ordered for a single-core cold enable?
   Any missing maintenance (e.g. TLBI VMALLE1 before enable, IC IALLU, cleaning
   the tables to PoC when C was off so table walks see them)? When the MMU is
   enabled with D-cache previously OFF, the tables were written with cacheable or
   non-cacheable attributes? — call out the coherency hazard explicitly.
3. **MAIR/TCR field-level.** With granule 4K, 48-bit input (or what T0SZ/T1SZ?),
   IPS from PARange=44 → IPS encoding, SH/ORGN/IRGN for a WB inner-shareable
   walk, TG0/TG1 encodings (note TG1 encoding differs from TG0), EPD0/EPD1 per the
   chosen regime. Which concrete field is easiest to get wrong and silently fault?
4. **W^X on aarch64.** AP[2:1]+PXN+UXN+the SCTLR.WXN interaction: is anything in
   D4 contradictory or incomplete? Does mapping page-table frames themselves RW+XN
   while the walker still reads them work (walker access is not an instruction
   fetch — confirm)? Is the no-alias invariant actually checkable at map time?
5. **Allocator window parameterization.** `je_pages` is TESTED x86 code with a
   hardcoded limit. Is widening it to base+limit a frozen-contract/tested-path
   change that must be escalated to the owner? Is a 32-bit vs 64-bit physical
   index a real risk at PARange 44 on a 256 MiB window? What's the minimal change
   that doesn't fork the allocator or break x86?
6. **Testability / false-success.** For each claimed invariant (W^X, guard,
   rollback, reservations-honored, MMU-actually-on), name the mutation that would
   make a naive test pass while the property is broken, and the observation that
   defeats it. Especially: how do we PROVE the MMU is actually translating (not
   just "didn't crash")? How do we prove a frame marked RESERVED was truly
   excluded (not just absent by luck)?
7. **Scope.** Is this slice too big for one PR? If it should split, where is the
   clean seam that still leaves each half independently provable on QEMU?

Answer each numbered question directly. Flag anything that must be escalated to
the human owner (frozen contract / tested path / security invariant). If you think
a decision in the ADR is simply wrong, say so and give the correct one.
