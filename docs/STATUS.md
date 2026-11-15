# Current ecosystem status — 2026-11-14

Inspected core main `b2b209f3863a27cf25d746547038f4985bf7e984`.
Audit repair gate closed with accepted residual dispositions; MP2 scheduler #101,
lifecycle extraction #102, ARM EL0 #103/#104, handles #105 and IPC #106 are merged.
Xcode tooling #107 is merged; Mac validation remains distinct from Linux checks.

Next: [audit continuation A0–A8](audit/2026-11-continuation.md), tracked by #87,
#18/#19 and mobile #2. ARM service parity and mobile/physical qualification remain
pending. Current main CI 43607930996 was running at observation, not yet passed.
This documentation adds no runtime fixes or new hardware support.

## Historical checkpoints (superseded where inconsistent)

# Current ecosystem status

Observed 2026-11-08T12:40Z (MP2 ADR 0013 scheduler). Branch `feat/mp2-sched` atop
core main `18cfce6` (= merge of fix-gate PR #100). State: **ADR 0013 implemented /
tested locally (host + QEMU) / planned (owner review, remote CI, physical ARM,
independent human qualification) / not blocked.** PR to main is pending; this is
not yet merged runtime and remote CI is not yet observed.

MP2 is now functionally complete: D1 round-robin policy (`kernel/core/sched_rr.c`,
arch-neutral; x86 `sched.c` untouched per D1), D4 ARM runtime MMU map/unmap with
guarded per-thread stacks, and D2/D3/D5/D6/D7 the AArch64 switch seam + QEMU proof
(`kernel/arch/aarch64/sched_arm.c`, the `vectors.S` returning-IRQ edit). Local
verification at this branch: `make check`, `make integration`, `make aarch64`
(normal + `ARM_SCHED_TEST=1`), the 9-profile ARM smoke matrix, the scheduler proof
smoke (counter table + guard-page fault; `dispatched=24 = 8 preemptions + 16
nonpreempt switch-ins`), the 10-mutation battery (each FIRED + killed by its named
detector, no PASS), `make mutants` (31 killed / 1 equivalent / 0 survived / 0
blocked / 1 skipped, incl. the 4 RR-* policy mutants), and runner `--selftest`.
Evidence: [`audit/evidence/2026-11-mp2-sched.md`](audit/evidence/2026-11-mp2-sched.md).
AI-assisted implementation and cross-lineage review, not independent human
qualification. The pre-MP3 ADR 0014 lifecycle-ownership blocker is unchanged.

---

## Historical checkpoint (2026-11-06, fix-gate closure)

Observed 2026-11-06 (fix-gate closure). Core main at the merge of fix-gate PRs
#94 (F01/F02), #97 (F03), #98 (F04–F06), #99 (F07–F11), and the F12–F14 PR that
adds this closure. Mobile main `9221701ab2d5333978a294f54574ee384f4216e3` (pin
unchanged).

**The repair-first gate is CLOSED.** F01–F14 are dispositioned — confirmed defects
repaired (reaper sign, ARM audit deref, usercopy NULL, endpoint-corruption
fail-stop incl. a cross-review-caught DoS), security-boundary mutant killers
enabled (27 killed / 1 equivalent / 0 survived / 0 blocked), owner-approved
contract changes landed (F07 JE_EP_CORRUPT split, F08 one-thread-per-process), and
the four deferred F06 decisions owner-dispositioned. See the
[closure ledger](audit/2026-11-fix-gate-closure.md) for the per-row dispositions
and exit-criteria status. Verified at the final head: x86 `make check` +
`make integration`, ARM `make aarch64` + 11/11 smoke matrix, `make mutants`,
runner `--selftest`. AI-assisted, not independent human qualification.

**Pre-MP3 blocker:** the lifecycle-ownership extraction is recorded as
[ADR 0014](adr/0014-pre-mp3-process-lifecycle-ownership.md) (proposed/owner-gated)
with an explicit migration trigger; MP3 must not begin until the owner approves it
(or waives it). With the gate closed, the roadmap (MP2 scheduler implementation,
then MP3) may resume subject to that blocker and the standing owner gates.

Tracked non-blocker follow-ups: `GAP-EP-WAIT-INFLIGHT` killer, x86 trap-routing
hosted seam, the `(uint8_t)hcap` recv truncation, the recv shared-helper refactor,
F13's main.c fixture split, device-observation-state split.

---

## Historical checkpoint (2026-11-06T02:10:28-05:00, pre-fix)

Observed 2026-11-06T02:10:28-05:00. Core main `e4bd9c24d69de4fc6798d733b1f2e6398522748f`;
mobile main `9221701ab2d5333978a294f54574ee384f4216e3` (pin unchanged).
PRs #89/#90 add audit and mutation tooling/provenance; #91 adds accepted ADR 0013;
#92 adds builder/container CI. Six enabled mutants and four disabled gap markers
exist. Baseline core CI 42177874117 passed; this does not resolve the audit defects.
ARM scheduler implementation is still planned; ADR 0013 is now on main.

**Next work: [current-code stabilization](audit/2026-11-fix-gate.md), before new
implementations.** Confirmed reaper, ARM alias-audit and assurance-runner defects
and remaining boundary tests need repair. The response PR adds no runtime fixes.
Architecture capability limits in the historical matrix below remain applicable.

# Historical checkpoint before PRs #89–#92

Observed 2026-11-05T23:37:57-05:00. This is the canonical current checkpoint; older status
blocks elsewhere are retained as historical evidence, not current instructions.

Core main: `fc8b8af931ee3fc79a82dcecc23356ba964c662b` (audit PR #86 merged).
[Core CI passed](https://github.com/auto44e/jeios/actions/runs/42162741426).
Mobile main: `d9a5146b1de5b870f495ccf5300ee7b679c3507e`.
[Mobile planning CI passed](https://github.com/auto44e/jeios-mobile/actions/runs/41378838543).
These are observed baseline runs, not test results for this documentation branch.

| Capability | AMD64 | ARM64 | Evidence boundary |
| --- | --- | --- | --- |
| Boot, memory protection and allocation | Implemented/tested | MP1 plus MP2 #80/#81 implemented/tested | Named QEMU configurations; physical-board qualification not established |
| Interrupt/timer return | Implemented/tested | #84/#85 merged: GICv2/CNTP and returning EL1h IRQ proof | QEMU evidence; real cache/barrier behavior not established by TCG |
| Preemptive scheduling | Implemented/tested | Remaining MP2 work; not present in inspected main | Development-branch designs are not merged runtime |
| Userspace, handles and IPC | Scoped M3 implemented/tested | MP3 planned | No ARM userspace parity claim |
| Supervisor/broker/device recovery | Scoped M4 implemented/tested | ARM parity planned after prerequisites | Bounded UART scenarios; no universal DMA containment |
| Read-only initfs and recovery shell | M5a/SH1 planned | Shared-format/service parity later | No interactive shell or filesystem runtime claimed |
| Mobile product integration | Not applicable | Planning only | core.json still pins 03aa2f380d8e49262abd1720a3b8db37517cbb31; arm64_ready=false |

Next execution work: remaining ARM MP2 scheduling, then reviewed shared lifecycle
contracts for MP3. D1 is AMD64 M5a + SH1 recovery using one console engine; D2 is
the same service crash/recovery demonstration on both architectures. Scope future
interfaces explicitly before calling them stable. SMP readiness, ARM M4 parity,
mutation automation and physical-ARM evidence need bounded follow-up tasks.

Clustering #82, products #75, POSIX/LDAP, storage backends, desktop/accelerators,
enterprise standards support and Android remain preserved planned requirements.
No broad security, release, certification or hardware claim follows from merge.
[Audit response and assessment](audit/2026-11-audit-response.md).
