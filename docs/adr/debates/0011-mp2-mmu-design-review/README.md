# ADR 0011 design review — transcript and resolution

Two-lineage design review run **before any MP2 code**, 2026-11-02/03. This is the
ADR 0000 part-4 debate attachment for ADR 0011.

- `00-shared-brief.md` — the shared grounding both lineages reviewed (verified MP1
  ground truth + the ADR draft + 7 focused questions).
- `01-codex-implementor-eye.md` — Codex, role: implementor's-eye adversarial
  reviewer (buildability, the MMU-enable VA-jump mechanics, the barrier/cache
  sequence).
- `02-claude-adversarial-vmsa.md` — Claude, role: adversarial VMSA-correctness
  reviewer (field-level W^X, the MAIR/TCR encodings that silently mistranslate,
  the false-success/mutation analysis).

## Convergent findings (both lineages, independently)

1. **Split the PR** at the allocator/MMU seam → PR-1a (allocator, MMU off) +
   PR-1b (MMU enable). Pinned as ADR D-SPLIT.
2. **MAIR Device byte bug:** draft implied `0x04` (Device-nGnR**E**); correct is
   `0x00` (Device-nGnRnE). Fixed in D3.
3. **Table maintenance bug:** draft said clean+invalidate; correct is
   invalidate-only `DC IVAC` (a clean can write a stale dirty line over the
   correct table). Fixed in D6.
4. **Enable sequence incomplete:** add `TLBI VMALLE1` before enable + `IC IALLU`
   after; single asm leaf, no stack writes between invalidate and `SCTLR.C=1`;
   M/C/I/WXN in one write; TG1 4K = `0b10` (≠ TG0 `0b00`). Pinned in D3/D6.
5. **Fault tests too weak:** EC alone can't tell a permission fault from a
   translation or access-flag fault; assert DFSC/IFSC (permission = `0b001111`),
   WnR, exact FAR, and prove MMU-on via a non-identity `AT`/`PAR_EL1` probe.
   Pinned in D7.
6. **Allocator widening** is a tested M1/M2 path → owner escalation (approved).

## Lineage disagreement (recorded, not smoothed)

**Kernel VA regime (D2).** Codex: high-half (TTBR1) **now** to avoid the risky
relocation later. Claude: **identity in TTBR0 for MP2**, defer high-half to MP3.
Owner chose **identity** — a high-half now would relink off 0x40080000 and change
MP1-tested `boot.S` (its load==link check), and would add a linear-RAM-map
`.text` RW alias hole that identity does not have. The descriptor builder is
written VA-generic so MP3 reuses it.

## Claude-unique catches folded into the ADR

- **AF (Access Flag) must be 1** on every live leaf — else an access-flag fault
  shares the EC of a permission fault and feeds a false success.
- **`.rodata` with PXN=0 is a live hole** WXN does not mask → dedicated
  execute-from-`.rodata` test.
- **`TTBR1_EL1` UNKNOWN at reset** + EPD1=0 = speculative garbage walk (security)
  → EPD1=1 forecloses it.
- **`je_pages_free` OOB read** if `a < base` not checked before the divide.
- **Allocator `.bss`-zero = FREE** is the PR-H defect class → default-deny
  adapter predicate.

## Owner escalations (all approved 2026-11-03)

Allocator widening; identity-vs-high-half VA regime; deferring the real
stack-overflow fix (separate exception stack) with an honest not-enforced note;
HANDOFF stating QEMU does not validate cache/barrier maintenance.

Nature: AI-assisted design review. This is not the independent human
qualification the critical profile requires before a hardened/release claim.
