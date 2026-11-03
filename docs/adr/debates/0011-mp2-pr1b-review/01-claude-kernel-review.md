The audit's independence fix is real, but only partial. My scratch probe, run against the real `paging.c` with the hosted harness, confirmed two holes that a builder bug can slip past `je_arm_mmu_audit`: the audit returned JE_OK both times. The probe is at `/tmp/pgrev/probe.c` and was not committed.

## Findings, ranked

**1. High: the audit only checks the bits it names; any other set bit passes.** `paging.c` `class_descriptor_matches` (~L455–480).
- It checks the low bits, AP, SH, AttrIdx, PXN, UXN, AF and nG. It never rejects any other set bit.
- Probe: the audit returned OK with each of these set on a live leaf: Contiguous (bit 52) on `.text`, DBM (bit 51), bit 50, NS (bit 5), and OA bit 48.
- Contiguous is a W^X hole. The 64 KiB block at 0x40080000 spans `.text`, `.rodata` (0x40086000) and `.data` (0x40087000). A Contiguous bit set by mistake on a text leaf lets the TLB apply the text RX entry to the neighbouring rodata and data pages (the architecture calls this CONSTRAINED UNPREDICTABLE). TCG ignores the hint, so smoke can never see this. Only the hosted test's `d & ~known` check catches it, and that runs against the encoder, not the installed tables.
- So the answer to your question: the audit can't be fooled on the fields it checks, but it can on every field it doesn't.
- Fix: compare the whole non-address part of the descriptor to a literal per class:
  - TEXT `0x0040000000000783`
  - RODATA `0x0060000000000783`
  - RW `0x0060000000000703`
  - DEVICE `0x0060000000000407`
  
  Also require the output address to be below 2^44 (the IPS).

**2. Medium-high: the audit follows table pointers without checking they point at the builder's own table frames.** `audit_table` (~L540) and `audit_alias_scan`.
- At levels below 3 it recurses into whatever address the descriptor holds. It never checks that the address is one of `mmu->table_pa` or that it lies outside every region.
- The only cross-check, `table_leaves == table_count`, counts identity leaves, not the tables actually walked.
- Probe: I copied the kernel L3 table into a `.data` frame (0x40088000) and pointed the L2 entry at it. The audit returned OK.
- After enable, ordinary `.data` stores would rewrite live translations, which reopens W^X after the audit has passed.
- Fix: for each table descriptor, require the address to be in `table_pa` and not inside any region. Also require that each table is reached exactly once and that the number of distinct tables walked equals `table_count`.

**3. Medium: the pinned PARange refusal is missing.**
- ADR 0011 D3/D7: a PARange encoding below `0b0100` must be FATAL, with a fault-injection mode. `main.c:596` only rejects encoding 0 (reserved), and `paging.c` hard-codes IPS to 44 bits.
- `paging.h` documents the IPS as coming from PARange, which no code does.
- No effect on cortex-a72 (it reports 44 bits), but AGENTS.md rule 5 (refuse when a guarantee can't be delivered) applies.

**4. Low: there is no write-to-`.rodata` fault mode, though the ADR D7 proof table requires one** (a data-abort permission fault with WnR=1).
- The only check is the `AT S1E1W` probe in the `rodata` and `text` fault modes, which expects a permission fault.
- So a `main.c` change of RODATA to RW is caught only in `ARM_MMU_FAULT=rodata`, not on a default boot: the map line doesn't print the class and the leaf count doesn't change.
- Either add the mode or record the deviation.

**5. Low: the invalidate range differs from the ADR.**
- `paging.c` passes `[_text, _end)`; ADR D6 and `paging.h` say `[__data_start, _end)` plus the DTB copy.
- The wider range is safe (invalidating text and rodata does no harm with C=0, and the DTB copy is in `.bss`).
- But `__data_start` is now unused apart from its alignment ASSERT, and the docs don't match the code. Use it or record the change.

**6. Low: the audit doesn't check that every page of every region is actually mapped.**
- A missing leaf is a crash, not a security hole. Rejecting a mismatch is cheap now that leaf counts are computed.

**7. Info: SMP will need broadcast TLB maintenance.** `TLBI VMALLE1` with `DSB NSH` is correct for the single-CPU profile; SMP needs the `…IS` variants. Note it in the not-enforced list.

## Checked and correct
- **Enable leaf:** follows the ADR D6 order and uses registers only, with no memory or stack writes. The `DminLine` stride `1<<(f+2)` is right, and invalidate-only (no clean) is used. M, C, I and WXN go in one SCTLR write after the TLBI. `IC IALLU` comes after the write, which matches the ADR and Linux. The TTBR0 value leaves CnP and the ASID at 0.
- **Identity mapping:** every region leaf is checked for VA==PA. Table frames are checked for VA==PA and membership in `table_pa`.
- **Alias scan:** quadratic but correct. An AP=01 leaf is not counted as writable by the scan, but the audit rejects any AP[1]=1 leaf anyway and fails as a whole, so no hole.
- **Regions and guard:** the stack region `[__stack_bottom, _end)` is exactly 0x4000 (whole pages) and the guard stays unmapped. A real stack overflow is caught by the `je_arm_in_handler` re-entry guard in `vectors.S` (ADR notes this as not enforced).
- **UART:** the Device-nGnRnE identity mapping keeps the console working after enable.
- **Build rollback:** the root-allocation failure path is clean.

Findings 1 and 2 are kernel audit changes, not test changes. Both can come with hosted tamper cases built from the probe. That would be the implementor's or owner's call, since `paging.c` is the frozen PR-1b code. This is AI-assisted review, not independent human qualification.
