# 2026-11 code audit — raw peer transcripts

These are the **raw, unedited** outputs of the two peer coding agents whose
reconciled findings became [`docs/audit/2026-11-code-audit.md`](../2026-11-code-audit.md).
They are attached for provenance, in the same spirit as the ADR debate transcripts
under `docs/adr/debates/`: the summary doc is the lead's reconciliation, and these
files are the underlying material so a reviewer can check the lead did not
misquote, drop, or invent a finding.

**Status of this content.** Peer output is an AI-assisted review, **not**
independent human qualification and not a frozen contract. Each peer ran as a
stateless, read-only, one-shot process over the tree at commit
`c6914ca9176b360ded82ed70c0884fafddbaf0e6`; neither modified the repository. The
lead re-verified every load-bearing claim in-tree before promoting it into the
summary. Where a peer asserted something the lead could not confirm, it did not
make it into the summary — so treat raw claims here as *candidate* findings, and
the summary doc as the verified set. Line numbers cited by the peers are relative
to that commit and may drift.

## The two slices (complementary, so no single author covers both a behavior and its check)

| File | Peer / effort | Slice |
| --- | --- | --- |
| [`00-shared-facts.md`](00-shared-facts.md) | — | The shared grounding both peers were given (inventory, toolchain, invariants). |
| [`01-codex-brief.md`](01-codex-brief.md) | Codex, reasoning effort xhigh | The brief: untrusted-input / architecture-safety slice. |
| [`01-codex-audit.md`](01-codex-audit.md) | Codex, reasoning effort xhigh | Raw audit of `fdt.c`, `image.c`, `boot.c`, `usercopy.c`, `alloc.c`, `handle.c`, AArch64 paging/gic/vectors/boot, x86 paging/vm_backend/trap. |
| [`02-claude-brief.md`](02-claude-brief.md) | Claude, model opus, effort high | The brief: logic-surface / test-quality slice. |
| [`02-claude-audit.md`](02-claude-audit.md) | Claude, model opus, effort high | Raw audit of `sched.c`, endpoint, `syscall.c`, `vm.c`, `device.c`, `spawn.c`, `proc.c`, `main.c`, and all `tests/test_*.c`. This peer also ran its own 28-mutant probe (6 killed / 22 survived) on a throwaway `git archive` copy; the survivors located the test-quality gaps. |

## What the lead did with these

- Re-read the cited lines in the live tree and confirmed (or dropped) each
  load-bearing claim — see the summary doc's per-finding "verified" tags.
- Turned the confirmed, high-value invariants into the committed critical-mutant
  manifest (`tools/mutants.json`): six enabled regression anchors (each with an
  existing killer test, each verified to KILL) and four disabled gap markers for
  the highest-value *missing* killers the peers found.
- Escalated the owner-gated defects (tested-code / frozen-contract: the
  `je_sched_reap_dead` sign bug, the AArch64 audit-cycle recursion, the usercopy
  NULL-kernel-buffer gap, the x86 `vb_flush` no-op) to the owner as
  recommendations rather than changing them.

No production code, contract, or status was changed by this audit.
