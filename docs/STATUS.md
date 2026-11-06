# Current ecosystem status

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
