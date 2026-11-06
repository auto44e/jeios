# Response to ecosystem audit PR #86

Observed 2026-11-05T23:37:57-05:00; inspected core main fc8b8af931ee3fc79a82dcecc23356ba964c662b
and mobile main d9a5146b1de5b870f495ccf5300ee7b679c3507e. Single-agent targeted
source/process assessment, not an exhaustive security audit or independent human
qualification. Original audit: [November audit](2026-11-ecosystem-audit.md).
No kernel, ABI, ADR or tested implementation is changed here.

## Assessment

I agree with the status-drift, lifecycle-coupling and repeatable mutation-evidence
priorities. The debate refinements are stronger than the original ranked list:
extract lifecycle responsibilities before changing the run queue; use named
critical mutants rather than a kill percentage; gate interface stability rather
than banning useful architecture-specific progress.

| Finding | Independent observation | Disposition |
| --- | --- | --- |
| Status drift | Entry docs still say MP2 next despite merged #80/#81/#84/#85 | Correct now: STATUS.md and history labels; automation remains #18/#19 |
| Audit revision ambiguity | Audit cites feat/mp2-aarch64-sched; main has no ADR 0013 or sched_rr.c | Treat those recommendations as branch-design review, not deployed-code findings |
| Lifecycle coupling | kernel/core/sched.c owns process_pool, registered_vm, je_proc_link and je_sched_reap_dead | Agree extraction deserves an owner-reviewed design before MP3; no blanket authorization to refactor |
| Mutation automation gap | No mutation/mutant references in .github/workflows | Named invariant/test/expected-rejection manifest first; build mutation runner next |
| Two ARM counters | gic.c je_arm_ticks and main.c je_arm_irq_tick_count; assembly references latter | Confirm debt, not proof of incorrect accounting: specify separate polled/IRQ ownership before unifying |
| Security prose drift | docs/SECURITY.md opening still says all threads CPL0 and no userspace boundary | Correct underclaim as well as possible overclaims; point to architecture-scoped status |
| Mobile pin gap | core.json pins 03aa2f3, arm64_ready=false | Preserve pin until separate native integration evidence; green mobile CI is planning validation |

## Where I would adjust the recommendations

- A merged-PR count is delivery history, not evidence of quality or completeness.
- T0–T4 should not be treated as a single confidence ladder: hardware type and
  assurance level are separate axes. Record target, execution method, invariant
  coverage and human review independently. KVM evidence cannot automatically
  qualify a physical board or every cache/TLB maintenance sequence.
- “Check enforced on this tested path” can describe observed runtime behavior;
  it is not automatically equivalent to hardened-release qualification. Require
  scope and evidence for each claim, and retain the existing human gate for
  critical release qualification. Do not silently broaden project policy.
- Prefer factual issue states and explicit dependencies over marking every future
  feature blocked. A hosted F2FS metadata probe can precede a native block driver;
  that does not make writable/runtime F2FS ready. Preserve the owner's ability to
  add ecosystem requirements; no blanket ban on planning documents is adopted.
- SMP readiness is urgent design hygiene; actual SMP requires per-CPU ownership,
  memory-ordering, scheduler/allocator synchronization and TLB shootdown evidence.
  IRQ masking alone is not a multicore lock. Add no dummy synchronization API.
- Lifecycle extraction and vector/ABI changes need bounded reviewable proposals
  before implementation. ADR 0013 is absent on this main; review its actual branch
  before proposing any amendment. Do not amend an unseen frozen contract.

## Priority and acceptance

1. Canonical status and architecture-specific claims (this documentation change).
2. Commit a small critical-mutant manifest and runner under #18/#19. Distinguish
   killed, survived, blocked and equivalent; require justified exemptions. A
   compile failure is not automatically a runtime invariant test killing a mutant.
3. Finish bounded MP2 scheduling; prepare lifecycle ownership/seam review for MP3
   without simultaneously replacing the proven AMD64 run queue.
4. D1 M5a/SH1 checked initfs/service loading/recovery; D2 dual-architecture recovery
   needs explicit ARM supervisor/device parity, not merely MP3 EL0/IPC.
5. Record single-CPU assumptions and SMP readiness, then select a safe real-ARM
   evidence target before Pixel work. No hardware purchase/flash is authorized.

Forward constraints: versioned experimental interfaces; scoped authority without
raw-pointer transfer; shared lifecycle ownership; explicit single-CPU limits;
DMA containment or explicit restricted profile before DMA-capable networking.
Clustering and imported products retain their own prerequisites and trust model.

## Verification and limits

Source checks used rg for lifecycle ownership and the two counters, git ls-files
for scheduler/ADR presence, workflow mutation references, and direct status and
security-doc reads. Missing root SECURITY.md was resolved to docs/SECURITY.md;
this path correction matters when reproducing the original audit. Current main
CI is successful in both repositories (links in STATUS.md). Local validation is
recorded in HANDOFF. No new emulator, hardware, security exploit or exhaustive
line-by-line audit is claimed; no new exploitable defect was demonstrated.
