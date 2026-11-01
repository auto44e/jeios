# M4 PR-G (broker) cross-lineage reviews

ADR 0000 part 4 attachment for PR-G. AI-assisted review across two lineages; NOT
independent human qualification.

## Design review (before code)
Both lineages blocked the initial plan and converged: a cap send COPIES (needs a
two-phase ACK before the supervisor closes its source end); the per-client cap
does NOT bound resources (a service pins a dead client's channel too) so the true
bound is a both-dead ledger; need BROKER_UNAVAILABLE for the race; service_generation
is supervisor-minted and learned via SERVICE_BIND; mode 13 corroborates from kernel
state. Folded into the ADR 0010 D2/D11 notes (owner-approved) and the svc.h
pre-wiring.

## Code review (Codex-authored impl) — blockers found
Both lineages confirmed the happy-path two-phase ordering, the ledger free rule
(modulo DYING-vs-DEAD below), cap-before-duplicate, service_generation integrity,
client demux-by-channel, re-broker DENIED, wrap-safe deadlines, and teardown
zero-leak. Note: Codex's "P0 images not built" and the restart-handle-leak claim
were DIFF-VISIBILITY artifacts (the review diff omitted Makefile/image.c; the
wiring is present and mode 13 builds+passes). Real items acted on in the fix pass:

Blockers:
- B1 (both): the mode-13 delivery count is written by VM slot index but read by
  PROCESS-record index — different index spaces (8 vs 48), correct only by
  allocation luck. Index both by the resolved client id.index.
- B2 (Claude): the cap-delivery hook fires in mint_recv_cap BEFORE the syscall
  commits, so a rolled-back mint (bad hbuf/ubuf, later-cap failure) is counted;
  an audited client can inflate the kernel's OWN proof by retrying recv. Record
  deliveries only AFTER je_endpoint_kern_recv_commit, from the syscall layer.
- B3 (both): a late/garbage message (a stale ACK read as a request, or any
  unexpected op) makes the supervisor call failed() — one client DoSes the broker.
  Drop/strike unexpected messages; never fail the supervisor; the loop stays live.
- H1/#4 (both): ACKs are not tied to a transaction — a stale ACK from a prior bind
  is consumed and breaks the two-phase guarantee. Add a supervisor-minted txn id
  echoed in BROKER_ACK.detail; discard non-matching ACKs until the deadline.
- #6 (Codex): broker_ledger_collect frees on state != LIVE (i.e. DYING), but a
  DYING process can still hold endpoints until reap — freeing then can exceed B=6.
  Require DEAD (or prove endpoint teardown) before collecting.

High / acted on:
- H2 (Claude): the lifetime-cap (and duplicate) check must precede the service
  restart side-effect, else an exhausted client still spends restart budget.
- H4/#8 (both): corroborate the SERVICE side (exactly 3 binds: A-gen1, B-gen1,
  A-gen2) and require B's brokered peer to be the live service, not just
  brokered_count==1; FLAG a wrong-rights (TRANSFER-bearing) delivery as bad rather
  than skipping it.
- #2/#3 (Codex): service-ACK-timeout must not forget the possibly-minted service
  end (leave it to the both-dead ledger, don't zero the slot); a PENDING entry
  must count as a duplicate so a client can't re-request the same generation and
  bypass the lifetime cap.
- H3/#9 (both): the "EXHAUSTED observed" marker overclaims — reword to
  "client-reported EXHAUSTED; kernel delivery count A==2 corroborated", and add
  real kernel evidence that A's 3rd request created NO endpoint.

Medium/Low acted on: gate the test-hook CALL SITE with #if JE_TEST_MODE==13 (B2's
fix moves it to the syscall commit path anyway); client vs supervisor deadline
units (use clock_ticks on both); generation wrap -> terminal; exact per-op length
validation in both images (close channel on violation); the integration.py
assertion must check the M4g "supervisor lost" string (not M4f); assert the
supervisor ep_charge peak <= 21 and live broker <= 6; ledger keys store the
INSPECT-derived (index,gen), not handle values.

Not changed (recorded): M5 — the EXHAUSTED test lands where a live duplicate also
exists, so it proves check-order but not a duplicate-free cap; recorded as a
coverage gap (a host unit test of the cap would close it).

Verification after fixes: see WORKER_NOTES + the PR. Local only; remote CI on the PR.
