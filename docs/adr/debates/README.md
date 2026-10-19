# Peer-debate transcripts

Raw multi-agent correspondence (`peer-debate.sh` runs, parallel consults,
adversarial reviews) that informed decisions in this project. Required by
**ADR 0000 part 4 (attached correspondence)**: a decision informed by an agent
exchange commits its transcript here and links it from the decision record, so
the reasoning is durable and auditable rather than ephemeral in `/tmp`.

Naming: `<area>-<step>-<topic>.md`. These are verbatim transcripts — the roles
(implementor / adversarial-reviewer) and rounds are as the helper recorded them.
A decision's prose quotes only what it needs; this is the full back-and-forth.

| Transcript | Informed |
|---|---|
| `m3c-step6-waits.md` | ADR 0009 A6 (blocking recv waits, D7) |

The earlier M3b/M3c debates (`m3b4`, `m3c`, `m3c-lifecycle`, `m3c-step3/4/5`) ran
before transcript retention and were `/tmp`-only; the ADR 0000 retroactive sweep
(2026-10-19) confirmed they were **not retained** and marked their decisions
"transcript not retained" rather than fabricating them. Only step-6 onward
(plus MP1 vectors) survive here.
| `m3c-step6-mechanism.md` | ADR 0009 A6-CLARIFY-1/2 (park/resume mechanism) |
| `m3c-step7-bootstrap-fixture.md` | ADR 0009 D-SEQ step 7 (bootstrap fixture + deferred ring-3 two-process proofs) |
