# M4 retrospective — the service supervisor and bounded device contract

Observed 2026-11-02T06:40Z. Core repo `auto44e/jeios`, main at the #70 merge
(`0f70203`), plus four owner/others docs PRs since (#74/#76/#77/#78). This retro
covers the M4 execution arc (issue #53) across PRs #60–#70, written at the owner's
request to accompany the next PR. It is history, not a status claim: each PR's own
evidence lives at its merge commit; nothing here is a rerun.

## What shipped (ADR 0010 D-SEQ, all merged, each owner-gated)

| PR | Decision(s) | What it added |
| --- | --- | --- |
| #60 | ADR 0010 | Froze the M4 contract and PR sequence; 8 owner decisions (capacity, PROCESS records + reaper, PROC_KILL, typed dispatch, TSS I/O bitmap + IRQ3, supervisor-loss halt, CLOCK_TICKS, the ADR 0009 quota deviation fix). |
| #61 | D2 | Capacity raise (P=8/T=16/E=24/H=40/R=48) + a per-table handle quota field; re-baselined M2/M3 asserts. |
| #62 | D3/D4/D-DEV | PROCESS records, the automatic reaper, PROC_STATUS, typed dispatch, the per-identity endpoint-creation quota. |
| #63 | D5/D6 | PROC_SPAWN, the LAUNCHER capability, the startup register ABI (v1), CLOCK_TICKS; rollback tested at every spawn failpoint. |
| #64 | D8 | Synchronous PROC_KILL, one test per victim state; found+fixed that the ADR 0009 forced-kill hook was never implemented. |
| #65 | D10 | The ring-3 C image path + the host tool `mkimage.py` (frozen rules E1–E10). |
| #66 | ADR 0006 | **Security fix found mid-M4**: zero physical frames on free; reject stale upper PTEs in `je_vm_audit`. |
| #67 | D7/D11/D12 | The supervisor + the D12 supervisor-loss slot-0 monitor. |
| #68 | D11 | The broker, SERVICE_RESTARTED, client reconnect. |
| #69 | D9 | COM2 DEVICE, the TSS I/O bitmap, kernel-owned IRQ3, spurious IRQ7. |
| #70 | D9 | The brokered UART driver service + timeout/detach/revoke + hardened→UNSUPPORTED. |

M4 (#53) is complete and closed. The whole chain runs on QEMU as TEST_MODE
scenarios 12 (supervisor-loss), 13 (broker), 14 (device), 15 (UART service),
alongside the unchanged default boot and the ARM profiles.

## Process that worked

- **Worker mode with cross-lineage review.** Codex implemented; each lineage (Codex
  + Claude) adversarially reviewed the other's output; the lead (me) pre-wired
  frozen headers + fail-closed stubs, integrated, ran mutants, and owned the
  result. Design review ran BEFORE code on every non-trivial PR.
- **Design review before code repeatedly paid off.** It caught, before a line was
  written: the broker's cap-send-doesn't-transfer-ownership (needed a two-phase
  ACK) and that a per-client cap can't bound resources (needed a both-dead ledger,
  #68); that the TSS I/O bitmap defaults to *allow* because .bss is zero and a zero
  bit permits (#69); that a real IRQ7 must still be EOI'd (#69); and that the D12
  detection couldn't hook `mark_dying` without spurious fires (#67).
- **Mutation testing as the acceptance bar.** The recurring failure mode across the
  milestone was *a green test that would still pass if the security property were
  broken*. We made the reviews build-and-boot mutants: #66 (stale-PTE audit fails
  without the strengthening), #67 (the Stage-2 review found a vacuous rollback test
  and report-trusting corroboration), #68 (an audited client could inflate the
  kernel's own proof via a bad-buffer recv retry), #69 (a mutant pointing a
  forbidden-port probe at the allowed port still passed; another deleting the
  revoke deny-write still passed), #70 (the PING/PONG timeout was only ever sent to
  an already-hung driver; the hardened no-side-effect check compared a value with
  itself). In each case the fix made the proof decide from KERNEL state (or, for
  #70, QEMU-observed COM2 bytes), never a self-report.
- **Owner escalation held the line.** Every change touching a frozen ADR/contract
  or tested M1/M2/M3 code went to the owner, not an agent vote: the D12 detection
  mechanism (#67), the frame-zero security fix (#66), the DEVICE teardown
  integration into the tested release walk and the startup n=2 contract (#69), the
  PROC_STATUS ABI question (#68, resolved by keying the broker ledger on a stable
  handle instead — option 2 + a recorded flex), and the hardened-refusal evidence
  mechanism (#70, Option A + a flex).

## Honest bumps (recorded, not smoothed over)

- #65 first CI run failed: an `--orphan-handling=error` link hardening tripped the
  CI linker's empty GOT/PLT orphans. A verified CI fact overrode the choice; fixed
  by discarding the section family in `user.ld` (image byte-identical).
- #66 was unplanned: the worker hit a child-spawn failure that turned out to be a
  real cross-process disclosure (recycled page-table frames with stale PTEs). It
  became its own PR, landed before PR-F rebased onto it.
- #70's first worker pass was cut off mid-implementation by a dispatch timeout and
  left mode 15 stalled at phase RUNNING — the PR-F kernel monitor watchdog caught
  it rather than hanging silently (a nice validation of that safety net). A
  continuation finished it; the review then hardened the evidence.
- A lesson I had to apply more than once: `make all` does not rebuild the ISO (run
  `make smoke` first), and a review diff that omits a file makes the reviewer
  report a false "doesn't compile / missing wiring" (happened on #67 and #69 — both
  dismissed by checking the real worktree).

## Known gaps carried forward (claimed honestly, not as done)

- IRQ7 real/spurious behavior is hosted-tested (test_device) but not QEMU
  hardware-injected (#69/#70).
- The broker lifetime-cap EXHAUSTED test proves check-order, not a duplicate-free
  cap (#68) — a host unit test would close it.
- M4 qualifies bounded supervisor/broker/COM2 scenarios on QEMU `pc` only — not
  arbitrary hardware, DMA, or a release. AI-assisted review is not the independent
  human qualification the critical profile requires.
- Two recorded flexes to revisit with the owner: the PR-G ledger key could move to
  a PROC_STATUS identity op (option 1); the #70 hardened refusal could move from
  kernel-corroborated (A) to supervisor-attested (B).

## What's next

Per `docs/NEXT_PHASE.md` (owner refresh): MP2 (ARM64 MMU/memory/timer/scheduling,
mobile #2) is the next execution work, then M5a #54 (read-only initfs + SH1 #71
recovery console) and the shell roadmap (#71–#73). The next PR begins MP2 in the
`jeios-mobile` repo and ships with this retro.
