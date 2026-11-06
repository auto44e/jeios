# JeiOS ecosystem audit — recommendations & improvements (2026-11-05)

Observation time: 2026-11-05 (ISO-8601, local). Core repo `auto44e/jeios` at
`feat/mp2-aarch64-sched` tip `645a41f` (main contains PRs through #85); mobile
repo `auto44e/jeios-mobile` docs-only. Scope of the scan: **all PRs** (core 63,
mobile 6 — every one merged), **all issues** (core 9 closed / 13 open, mobile 1
closed / 7 open), the **GitHub Project** `jeios-project` (34 items), ADRs
0000–0013, and the committed doc/code state of both repos.

Method: a full inventory via `gh` + repo reads, then two independent max-effort
AI analysis passes (one engineering-rigor/process lens, one roadmap/scope/risk
lens) whose findings were cross-checked against the repo and reconciled here. The
raw analysis transcripts are in the appendix links. **This document is
AI-assisted planning input, not independent human qualification** — it is written
for the main planning agent and the owner to review and act on, nothing here
changes code or a frozen contract.

---

## 1. What is working well (keep doing)

These are genuine, evidenced strengths — the audit is not neutral on them:

- **Review discipline catches real defects before merge.** Every non-trivial PR
  runs design-review-before-code → cross-lineage code review (author ≠ reviewer,
  two AI lineages) → mutation testing as the acceptance bar, with transcripts
  committed under `docs/adr/debates/`. This has caught *real* security holes
  pre-merge: a cross-process stale-PTE disclosure (#66), a default-ALLOW TSS I/O
  bitmap, a self-referential W^X audit, a Contiguous-bit W^X hole, a timer routed
  to the fatal FIQ slot, and several proof false-success paths. This is the
  project's single biggest asset.
- **Honest evidence ledger.** Local-vs-remote CI stated separately;
  implemented/tested/planned/blocked; "QEMU TCG models no caches → cache/barrier
  maintenance is reviewed, not observed"; "AI review is not independent human
  qualification." `STANDARDS.md` ("certification is never inferred") is a good
  model.
- **Clean delivery cadence.** 63/63 core PRs merged (no abandoned work), small
  reviewable PRs, ADR-0000 3-part decision records, owner-gated escalation for
  frozen-contract / tested-path changes.
- **Portability by construction where it was done early.** `handle.c`,
  `endpoint.c`, `proc.c`, `vm.c`, `syscall.c` are hosted-built and tested, so
  they port to ARM largely as reuse.

The recommendations below are about **keeping these strengths trustworthy as the
surface grows**, plus a few concrete gaps.

---

## 2. Prioritized recommendations

Priority key: **P0** act now / this gate · **P1** next · **P2** soon-ish.
Each item: observation (evidence) → why → action. Items the two analysis lenses
independently agreed on are marked ⊕.

### A. Status & tracking hygiene

**A1 ⊕ (P0) Single-source the status; it has already drifted.**
Evidence: the "Current ecosystem status — observed 2026-11-01" block is
duplicated verbatim atop **five** files (`ROADMAP.md`, `IMPLEMENTATION.md`,
`NEXT_PHASE.md`, `AGENT_START.md`, `HANDOFF.md`) and predates **all** of MP2 — it
still says "MP2 (mobile #2) is next execution work" though #80/#81/#84/#85 are
merged; `NEXT_PHASE.md`'s order table still lists the closed #52/#53 as
First/Next. Action: create one `docs/STATUS.md` as the only "current" block;
other docs link to it and their copies become dated history. Add a CI lint that
fails if the "Current ecosystem status" heading appears in >1 file, or if the
commit STATUS.md records is more than N merges behind `main` when a gate PR
closes.

**A2 ⊕ (P0) Track status as a capability × architecture matrix, not one gate
row.** Evidence: the board + docs mark M1/M2 "preemptive threading" **Done** with
no arch qualifier — true only on AMD64; ARM scheduling is ADR 0013 design-only.
Action: replace binary gate rows with `capability × arch × state × evidence-tier`
so "Done" cannot hide that the ARM side is pending. "M4 complete" → "M4 complete
(x86, tier T1)".

**A3 (P1) Make the GitHub Project a legible work queue.** Evidence: core repo has
no milestones, no labels; all 13 open core issues have 0 comments and were
bulk-touched once (2026-11-02); ARM work lives in core code but is tracked only
under mobile #2, so the core board can't show MP2/MP3. Action: add fields Lane
(Now/Next/Later/Parked), Kind (gate/contract/assurance/planning), Arch
(x86/arm/both); WIP-limit "Now" to one execution gate per arch + one assurance
item; make milestones = demos (below); open core issues for MP2-sched / MP3a-c /
ARM-parity with mobile #2 as the umbrella; post-merge checklist to update the
board + record the merge commit.

### B. Test / CI / assurance rigor

**B1 ⊕ (P0) Promote mutation testing from manual to a CI gate.** Evidence: the
process repeatedly finds "green tests that pass though the property is broken,"
but mutation runs are manual/lead-driven (no `mutation`/`mutant` reference in
`.github/workflows/`), and the asm mutants in recent PRs were run in `/tmp` "not
committed." Issues #18 (CI) / #19 (coding profiles & automation) are still open
planning. Action: commit mutant definitions (patch files + a runner) for each
acceptance-bar proof; add `make mutation` with a bounded PR set + a larger
nightly suite + per-invariant kill thresholds; fail CI on a newly surviving
security mutant. This is the concrete core of #18/#19.

**B2 ⊕ (P0) Define explicit support tiers and attach one to every status line.**
Evidence: AGENTS.md rule 12 requires "a named support tier," but no tier
vocabulary exists in `HARDWARE.md` or anywhere in `docs/`. Action: adopt
T0 hosted-tests-only · T1 pinned-QEMU-TCG · T2 HW-accelerated-virt on real
silicon · T3 named physical board · T4 qualified release incl. the human gate.
Today: x86 M4 = T1; ARM MP2 = T1. Missing hardware evidence yields `unqualified`,
not a doc footnote.

**B3 ⊕ (P1) Add a real-ARM evidence lane BEFORE the Pixel (MP4).** Evidence: QEMU
TCG models no caches / weak memory, so the aarch64 cache-maintenance, TLB and
barrier sequences (ADR 0011 D6, ADR 0012, the ADR 0013 switch) are *reviewed, not
observed*; the first real observation would otherwise be the Pixel — the most
expensive, riskiest place for it. Action: add an ARM T2/T3 step (Raspberry Pi
4/5, or an arm64 KVM host if available) that observes MMU/TLB/barrier/IRQ-return
on real silicon at low risk, ahead of MP4.

**B4 (P1) Give the ARM IRQ path a hosted seam.** Evidence: `gic_hosted.h` and
`test_sched_killall` show the pattern, but `je_arm_irq` has none — the whole
interrupt policy is only exercised through QEMU asm execution, so negative paths
are weakly tested. Action: split the tiny asm entry/`eret` wrapper from the
hosted-testable C policy (vector validation, frame interpretation, timer
accounting, spurious/EOI ordering, return-state validation); keep exact asm under
QEMU/hardware smoke + objdump checks.

**B5 (P1) Automate the acceptance-process checklist.** Evidence: the
design-review / cross-lineage / mutation / status / handoff requirements are
strong but procedural (enforced by agent diligence). Action: a CI check that a
non-trivial PR carries the ADR/debate record, author≠reviewer lineage, a mutation
result, the exact test commands, and a status/handoff update — the practical
minimum of #18/#19.

### C. Architecture risk & technical debt

**C1 ⊕ (P0) Establish the arch-neutral seams before MP3 (ARM userspace).**
Evidence: x86 is at M4, ARM at MP2, and the gap widens each core PR —
`kernel/core/main.c` is ~3,900 lines and is really the **x86 integration
harness** (direct `je_arch_cr3_load`, M3/M4 test orchestration) yet lives in
`core/`; `include/jeios/arch.h` (the "neutral" interface) exports PC-isms
(`irq3_mask`, `master_isr`, `tss_set_com2_io`, `cpu_policy_cr0/cr4`). Action:
split `arch.h` into a neutral interface (console, irq save/restore, timer, halt,
vm-backend ops) + `arch/x86_64/pc.h`; move the M3/M4 orchestration out of
`kernel/core/main.c` into `arch/x86_64/` or test-mode modules; add a CI check
that fails if `kernel/core/*.c` gains arch-specific inline asm or `cr3`/`tss`
symbols. **A second ISA is the cheapest audit of an ABI** — the longer MP3 is
deferred, the more "parity" becomes a rewrite.

**C2 ⊕ (P0) Converge on one scheduler before ARM scheduling calcifies.**
Evidence: ADR 0013 adds a neutral `sched_rr.c` for ARM but leaves the tested x86
`sched.c` untouched with no migration date → two pickers that will drift
(fairness, wakeup, kill, accounting). Action: schedule the owner-gated
`sched.c` → `sched_rr.c` migration as the PR right after MP2 scheduling lands;
the regression net already exists (M2 smoke + `test_sched_killall`);
differential-test the neutral core against current x86 behavior, then freeze/
remove the old picker.

**C3 ⊕ (P1) Pay down the immediate ARM debt cluster.** Evidence & actions:
(a) **two ARM tick counters** — `gic.c:je_arm_ticks` (bumped in
`je_gic_handle_one`) and `main.c:je_arm_irq_tick_count` — unify to one
timer-owned counter or rename+document ownership/rollover and add monotonicity/
overflow tests; (b) **`vectors.S` is re-edited every ARM PR** though treated as
frozen — do one owner-gated vector-ABI cleanup (stable trampoline + dispatch
policy elsewhere + objdump/static offset assertions) so MP3's syscall/fault work
isn't another table edit; (c) **file-static `je_arm_pages` is being exposed** —
keep production state private, expose a narrow alloc/query API + a test snapshot
hook; (d) **mutually-exclusive single-feature ARM build dirs** won't scale once
userspace/IPC/sched/board variants combine — move to a small compile-time
feature-config with dependency + invalid-combo checks and keep baseline +
full-feature ARM CI configs.

### D. Security posture

**D1 ⊕ (P0) Add a standing per-gate security-invariant ledger (not only reactive
mutation).** Evidence: mutation testing catches false-success *after* tests
exist; it doesn't guarantee every gate enumerated its invariants. The recurring
defect class is local (stale mappings, privilege defaults, routing, revocation).
Action: per gate, maintain `invariant → trust boundary → positive test →
negative/denial test → mutation → evidence → owner`; require a denial test for
every security-sensitive invariant, plus a short gate threat model (assets, trust
boundaries, attacker inputs, fail-closed, rollback, evidence gaps) tied to tests
and ADRs — not a new disconnected planning doc.

**D2 ⊕ (P1) Make hardware evidence part of the *claim model*, and keep a visible
human-review-debt list.** Evidence: every closed gate is AI-review-only; a future
hardware/hardened claim would currently rest on QEMU-only evidence. Action: the
support-tier matrix (B2) gates claims — T4 requires a named human qualification
record; track human-review debt per gate so the backlog for the human gate stays
visible. DMA/IOMMU is explicitly unqualified (M4) — put "DMA containment
(IOMMU) qualified or explicit legacy-tier policy" into the **M6** entry criteria
now, before networking arrives with a no-IOMMU virtio driver by default.

### E. Scope & roadmap sequencing

**E1 ⊕ (P0) Separate the planning backlog from the critical path; don't let it
mask the work.** Evidence: a very broad planned ecosystem (clustering #82,
Android AC0 #4, GPU/NPU accelerators, RISC-V, ISO 27001, LDAP #16, POSIX #15,
F2FS #13/#14, Bash/Zsh #73) sits on a kernel with no storage/network/UI and no
ARM userspace; the planning docs aren't large (~3–4k lines) but each becomes a
"later gate" line in the status header, which is now mostly a list of
not-being-done. #14 (F2FS probe) even inverts the order (assumes a block device +
#13 contract, but M5a uses an in-memory initfs). Action: label the premature
issues **`parked`** (keep them — the owner wants requirements preserved — don't
close), use #9 as the requirements register pointing at them, and extract the
1–3 *forward constraints* each parked doc imposes on near-term design (handle
width/generation bits vs. future distribution; `PROC_SPAWN`/`CLOCK_TICKS` ABI vs.
POSIX; a per-CPU guard in ADR 0013) into a single "Forward constraints" table in
ROADMAP. Rule: no new ecosystem planning doc until a demo milestone closes,
unless the owner overrides.

**E2 ⊕ (P0) Define the two demo milestones and drive to them.** The smallest
honest dual-arch "it works":
- **D1 (x86):** M5a minimal = a checked read-only initfs parser (hosted
  malformed/fuzz tests) + services loaded from initfs instead of linked blobs +
  a recovery console whose minimal **SH1** command set (list/status/restart/logs)
  *is* M5a's diagnostic console (don't build two) + only the read-only slice of
  the #13 storage contract. Acceptance criterion: *no new arch-specific code in
  `kernel/core/`* (ties to C1).
- **D2 (both arches):** the same initfs format and the same service sources boot
  and pass the crash-and-recover scenario on QEMU x86 **and** QEMU virt — the
  compelling, honest demo, labelled tier T1.
Map M5a #54, SH1 #71, the #13 read-only slice, ADR 0013 sched, and MP3 onto this
path; keep them "Now/Next", park the rest.

**E3 ⊕ (P1) Parity rule to stop the arches diverging.** M5a (x86) and MP3 (ARM)
run in parallel — one execution gate per arch — but **no x86 gate past M5a starts
until MP3 closes.** This forces the ABI to be exercised on a second ISA before M6+
piles on.

### F. Missing gates (both lenses / verified gaps)

**F1 ⊕ (P0-plan) There is no SMP gate and no lock primitive anywhere.** Evidence:
a grep of `kernel/` + `include/` finds **no** spinlock/mutex/atomic-flag lock
primitive; correctness rests on single-core IRQ masking + globals
(`je_arm_in_handler`, `current`, the tick counters); SMP appears only as a
"future work" disclaimer (`ROADMAP.md:84`, `DESIGN.md`), not as an M0–M9/P1 gate —
yet every real target (desktop, Pixel 9) is multicore and clustering is planned
in detail while SMP is not planned at all. Action: add an SMP gate (M4.5 / MP3.5)
after D2 and before M6; **before M5a adds more global state**, write an inventory
of SMP-unsafe globals + the locking/per-CPU conventions so new code is
SMP-ready-by-convention rather than needing a later sweep.

**F2 ⊕ (P1) No ARM gate covers supervisor/device parity.** Evidence: MP3 is "EL0
isolation + native IPC"; MP4 is Pixel feasibility — nothing between them ports the
M4 supervisor/broker/device graph to ARM, so D2 (dual-arch recovery) has no home.
Action: add an "MP3d / ARM-M4 parity" gate: the supervisor + broker recompiled
for an aarch64 `crt0` + a second PL011 as the brokered device.

**F3 (P2) Rotate append-only docs at gate close.** Evidence: `HANDOFF.md` ~2.5k
lines, `IMPLEMENTATION.md` ~2.1k — a new agent reads top-down to find "current."
Action: keep the owner's preserve-history rule but rotate superseded entries into
`docs/history/YYYY-MM.md` at each gate close so each file's head stays short.

---

## 3. Consolidated top-10 actions (ranked, for the planning agent)

1. **(A1/A2)** One `docs/STATUS.md` + a capability×arch×tier matrix; CI lint for
   duplicated/stale status headers. Fix the stale MP2 wording immediately.
2. **(E1/E2)** Define D1 (x86 M5a + SH1 console) and D2 (dual-arch recovery) as
   the only milestones; `parked`-label the planning backlog behind #9.
3. **(B1)** Commit mutant definitions + a `make mutation` CI gate (nightly for the
   slow set) — the concrete core of #18/#19.
4. **(C1)** Split `arch.h` neutral vs PC-specific + move the x86 harness out of
   `kernel/core/main.c`; CI-fail on arch symbols leaking into `core/`.
5. **(C2)** Migrate x86 `sched.c` onto the neutral `sched_rr.c` right after MP2
   scheduling, before MP3.
6. **(F1)** Add an SMP gate before M6 and write the SMP-unsafe-globals +
   locking-convention inventory before M5a adds more globals.
7. **(B2/B3)** Define support tiers T0–T4; add a real-ARM (Pi/KVM) evidence lane
   before MP4.
8. **(F2/E3)** Add the MP3d ARM-parity gate; adopt the parity rule (no x86 gate
   past M5a until MP3 closes).
9. **(D1)** Per-gate security-invariant ledger + short threat model with denial
   tests; **(D2)** DMA/IOMMU criterion into M6 now.
10. **(C3)** Pay down the ARM debt cluster: unify the two tick counters, stabilize
    the vector ABI, re-privatize `je_arm_pages`, move to a feature-config matrix.

---

## 4. Appendix — evidence & provenance

- Inventory: core 63 PRs (all merged), issues 9 closed / 13 open; mobile 6 PRs
  (all merged), 1 closed / 7 open; Project `jeios-project` 34 items; ADRs
  0000–0013.
- Verified-in-repo during this audit: the stale status header in 5 docs
  (`grep -rl 'observed 2026-11-01T21:30:30-05:00' docs/`); two ARM tick counters
  (`gic.c:76` / `main.c:976`); no mutation reference in `.github/workflows/`; no
  lock primitive in `kernel/`+`include/`; SMP only a disclaimer in `ROADMAP.md`;
  no support-tier vocabulary in `HARDWARE.md`/`STANDARDS.md`;
  `include/jeios/arch.h` PC-isms; `kernel/core/main.c` size + x86 coupling.
- Analysis passes (AI-assisted, max effort; planning input, not human
  qualification): an engineering-rigor/process lens and a roadmap/scope/risk
  lens, reconciled here. Both independently flagged the status drift, the
  mutation-in-CI gap, the real-hardware tier gap, the scheduler-convergence/
  arch-seam debt, and the park-the-backlog recommendation; the roadmap lens
  additionally surfaced the missing SMP gate and the missing ARM-parity gate,
  both verified above.

Nothing in this document changes code, a frozen contract, or a status claim; it
is a prioritized recommendation set for the main planning agent and the owner.
