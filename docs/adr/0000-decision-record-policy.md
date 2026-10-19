# ADR 0000: Decision-record policy (how we record every decision)

Status: **accepted**, 2026-10-19 (owner-approved). Applies to **all** ADRs,
ADR amendments, and owner-gated decisions from this point forward, and is applied
retroactively to earlier decisions in a one-time sweep (see "Retroactive sweep").

## Context

The project makes frequent contract/ABI/tested-code decisions, each escalated to
and decided by the owner. Historically we recorded *what* was decided and a short
*why*, but not the roads not taken or where a decision is deliberately left open
to revisit. That loses two kinds of signal a future session (or the main planning
agent, consulted at each pause) needs: the predicted consequences of the
alternatives (so a later reversal can be judged quickly), and an explicit marker
of which decisions are firm vs. provisional.

## Decision

Every recorded decision — an ADR section, an ADR amendment, or an owner-gated
call captured in IMPLEMENTATION.md / HANDOFF.md — SHOULD carry **three parts**
(a missing part is a review comment to fix, **not a merge blocker** — this is a
recording convention, not a gate):

1. **Decision + why.** What was chosen, and the reasoning/evidence that drove it
   (including any peer-debate finding or verified fact that settled it).
2. **Predictions if other choices were made.** For each seriously-considered
   alternative, the predicted consequence had we taken it — the cost, the new
   invariant it would have forced, the contract change it would have required.
   This is the counterfactual, written at decision time while it is cheap and
   accurate, so a future reversal does not have to reconstruct it.
3. **Flex options to consider in future.** Where the decision is revisitable,
   what later milestone or condition would reopen it, and the shape of the
   alternative then. A decision with no sensible flex says so ("firm; a change
   needs a new ADR").

Plus, when a peer-agent exchange informed the decision:

4. **Attached correspondence.** When a decision (human or agent) was informed by
   a multi-agent exchange — a `peer-debate.sh` run, a parallel consult, an
   adversarial review — the **raw transcript `.md` is committed to the repo** and
   **linked from the decision record** (part 1's "why"). Transcripts live under
   **`docs/adr/debates/`** (durable, versioned with the code), NOT left in `/tmp`
   (ephemeral, lost on reboot). The link is the primary artifact; the decision
   prose quotes only what it needs. A decision reached with no agent exchange has
   nothing to attach and says so implicitly (no link). This makes the reasoning
   that produced a decision auditable and re-readable by a future session, not a
   paraphrase trapped in one agent's memory.

**Bonus convention:** a decision that is *provisional pending the planning agent*
(the cross-project agent consulted at each pause) is tagged **`FLEX (planning
review)`** in its flex section, so the pause review has a ready worklist. The
planning agent **recommends**; the **owner decides** — tagging a frozen
contract/ABI item `FLEX (planning review)` flags it for discussion, it does NOT
license the planning agent (or any agent) to reopen a frozen decision without
owner sign-off (AGENTS.md).

This is a *recording* policy, not a *process* change: it does not add approval
gates or ceremony. It changes only the shape of what we write down. It composes
with the existing escalation rule (contract/ABI/tested-code → owner) and the
peer-agent toolkit (which surfaces the alternatives this policy then records).

## Predictions if other choices were made

- **If we kept the old "what + short why" format:** future reversals (likely at
  SMP, supervisor, or a wider-handle ABI) would each pay to reconstruct the
  alternatives from memory or re-debate them — exactly the cost this policy
  front-loads once. Over a long milestone chain that is the larger total cost.
- **If we made this a heavier process (required alternatives matrix, sign-off per
  part):** it would slow routine small decisions and get skipped under time
  pressure, which is worse than a lightweight always-on convention. Hence the
  three parts are *recording* requirements, not *gates*.
- **If we tracked flex options in a separate file instead of inline:** the
  counterfactual would drift from the decision it qualifies. Inline keeps them
  co-located and reviewed together.
- **If we left transcripts in `/tmp` and only paraphrased them (the prior
  practice):** the correspondence that actually drove a decision would be lost on
  the next reboot, leaving only one agent's summary — which the AGENTS.md history
  rule warns is where after-the-fact fabrication creeps in. A future reversal
  could not re-read the real back-and-forth. Committing the `.md` costs a few KB
  per decision and makes the reasoning auditable; the alternative (a transcript
  store outside the repo) would drift from the code it justifies, same failure as
  separate flex files.

## Flex options to consider in future

- **FLEX (planning review):** the exact section headings ("Predictions…",
  "Flex options…") and whether the policy should also cover non-owner routine
  decisions, are revisitable with the main planning agent at a pause. The *intent*
  (three parts) is firm; the formatting is not.
- **FLEX (planning review):** whether `docs/adr/debates/` is the right home (vs a
  per-ADR `debates/` subdir, or git-LFS for very large transcripts), and whether
  transcripts should be redacted/trimmed before commit, are revisitable. The
  *intent* (commit the raw correspondence, link it from the decision, never leave
  it only in `/tmp`) is firm; the location and trimming are not.
- If a decision-record tool or lint is ever added, this policy is its spec.

## Retroactive sweep

Earlier decisions (ADRs 0001–0009 and their amendments, and the owner-gated calls
recorded in **IMPLEMENTATION.md and HANDOFF.md**) predate this policy. They are
brought into conformance by a **one-time docs-only sweep PR**, scheduled
**before** the final doc-sync + retro at the M3c/MP2 pause. The sweep adds the
missing "Predictions" and "Flex options" parts to each prior decision **from the
existing record only** — each such part is explicitly **labelled "reconstructed
(sweep YYYY-MM-DD)"**, and where the record does not actually support a
counterfactual it is marked **"unknown — not in the record"** rather than invented
(the AGENTS.md history rule: never fabricate reasoning after the fact). The
reconstructed parts are **appended annotations**, not edits inside the original
frozen decision text. The sweep re-decides nothing and changes no executable
behavior.

The sweep ALSO applies part 4 (attached correspondence): the peer-debate
transcripts for prior decisions that currently live only in `/tmp` are committed
under `docs/adr/debates/` and linked from the decisions they informed (the M3b/M3c
debates: `m3b4`, `m3c`, `m3c-lifecycle`, `m3c-step3`, `m3c-step4`, `m3c-step5`,
`m3c-step6`, …). A prior decision whose transcript was genuinely not saved is
marked **"transcript not retained"** rather than reconstructed — never fabricate a
transcript. Transcripts committed during normal (non-sweep) work from this point
on need no label; the sweep label is only for back-filled history.

### Sweep completion record (2026-10-19)

The one-time sweep PR was executed on 2026-10-19 (`main` after the MP1-vectors
merge). Honest scope of what it did:
- **ADRs 0001–0008:** each gained an appended "Decision-record annotation
  (reconstructed — sweep 2026-10-19)" with Predictions + Flex reconstructed **from
  that ADR's own text only** (Status / Consequences / open-item lines + any in-text
  alternatives, e.g. 0008's recorded Option A vs B). Nothing invented; unsupported
  items are "unknown — not in the record." Original frozen decision text was not
  edited — the parts are appended annotations.
- **ADR 0009:** already authored under this policy (Predictions + Flex throughout,
  A1–A6 amendments) — already conformant, not re-annotated.
- **Part 4:** ADRs 0001–0008 and the M3b debate predate transcript retention; their
  transcripts were `/tmp`-only and **not retained**, so each annotation says
  "transcript not retained" rather than fabricating one. The retained transcripts
  (M3c step-6/step-7, MP1 vectors) are under `docs/adr/debates/` and linked from
  their decisions. (Correction to the list above: of the enumerated `m3b4` /
  `m3c` / `m3c-lifecycle` / `m3c-step3..5` debates, NONE were retained — only
  step-6 onward survived in-repo; the earlier names are recorded for history, not
  as present files.)
- **Standalone owner-gated calls in HANDOFF.md** not in an ADR (F2FS provisional
  mobile default; POSIX.1-2024 / LDAPv3 core; the 2026-10-14 owner decisions)
  remain in the append-only timestamped HANDOFF history with their reasoning
  inline, recorded here as pre-policy calls whose transcripts were not retained,
  rather than rewritten in place. Future such calls use the 3-part + part-4 format
  at decision time.
