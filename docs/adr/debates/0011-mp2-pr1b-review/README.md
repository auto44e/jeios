# PR-1b cross-lineage code review — transcript and resolution

AI-assisted cross-lineage review of MP2 PR-1b (aarch64 MMU enable). Codex
implemented the kernel MMU (paging.c/.S, main.c wiring); Claude wrote the tests +
fault proofs + oracle; then each lineage reviewed the OTHER's output. This review
found and the lead FIXED two real security holes in the audit.

- `01-claude-kernel-review.md` — Claude reviewing the kernel MMU code. Ran a live
  hosted probe that confirmed two audit holes.
- `02-codex-tests-review.md` — Codex reviewing the tests + oracle + fault modes.

## High-value kernel findings (FIXED in paging.c)

**F1 (High) — audit only checked named bits.** `class_descriptor_matches`
checked AP/PXN/UXN/AF/SH/AttrIdx/nG but ignored every other bit, so Contiguous
(bit 52), DBM, NS, software bits could be set and pass. The Contiguous bit on a
`.text` leaf is a W^X hole (the TLB may apply the RX entry to neighbouring
rodata/data pages); TCG ignores the hint so smoke never sees it. FIX: compare the
WHOLE non-address part of each leaf against a per-class LITERAL
(TEXT 0x…783, RODATA 0x…783 w/PXN, RW 0x…703, DEVICE 0x…407) and require OA <
2^44. Validated by mutation: the Contiguous-bit mutant now FATAL-halts at the
boot audit.

**F2 (Medium-high) — audit followed table pointers without checking them.**
`audit_table` recursed into whatever address a table descriptor held; a table
descriptor redirected into a `.data` frame passed (post-enable `.data` writes
would then rewrite live translations, reopening W^X AFTER the audit). FIX:
require every table descriptor's output to be one of the builder's own
`table_pa` frames; combined with the existing `table_leaves==table_count` tie,
every table is accounted for.

**F3 (Medium) — PARange refusal was missing.** ADR D3/D7 pin IPS to 44 bits; the
code hardcoded IPS=44 but never refused a narrower-PA part. FIX: `mmu_bringup`
FATAL-halts if `pa_bits < 44` (AGENTS rule 5). No effect on cortex-a72 (44).

## Lower findings (recorded, not all fixed)

- F4 (Codex P1 / the identity-check interaction): the lead's earlier VA==PA
  identity fix now rejects the alias-test mutations BEFORE `audit_alias_scan`
  runs, so those two hosted tests no longer exercise the alias scanner directly.
  The alias scanner remains correct defense-in-depth; identity-uniqueness is the
  primary enforcement. Recorded as a tested-mechanism clarification.
- F4b (Claude low): no write-to-`.rodata` deliberate-fault mode (only AT probes
  cover it); a RODATA→RW change is caught in ARM_MMU_FAULT=rodata + the hosted
  test, not on default boot. Recorded deviation.
- F5 (Claude low): the DC IVAC range is [_text,_end) vs the ADR's
  [__data_start,_end)+DTB; the wider range is safe (text/rodata invalidation is
  harmless with C=0; the DTB copy is in .bss ⊂ the range). Recorded.
- Codex P2: the smoke leaf-count oracle pins the count, not table placement; the
  hosted test pins the first-fit layout. Enable assembly is argument-tested only
  (cache/barrier maintenance is review-only under TCG — ADR D6 honesty).
- SMP (Claude info): TLBI VMALLE1 + DSB NSH is correct for single-core; SMP needs
  the …IS broadcast variants. In the not-enforced list.

## Mutation testing (lead, the acceptance bar)

- rodata PXN=0 → hosted encoder test FAILS + boot audit FATAL (F1 path).
- Contiguous bit on .text → boot audit FATAL (F1 fix).
- WXN dropped from SCTLR → smoke oracle "sctlr missing ['WXN']" (the kernel
  can't self-catch this; the independent harness does).
All caught by the right layer.

Nature: AI-assisted multi-agent review + the lead's mutation testing. NOT the
independent human qualification the critical profile requires; no remote CI at
authoring time. QEMU TCG models no caches, so the DC IVAC/barrier/TLB
maintenance is reviewed, not observed.
