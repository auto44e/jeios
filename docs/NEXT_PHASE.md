# Next phase: ARM64 MP2 and the native shell foundation

## Current ecosystem status — observed 2026-11-01T21:30:30-05:00

Core main `0f702036c52d40d03bb4c8919d0dcae53ba9f2b1`: AMD64 M3 and scoped M4 complete; PR #70
merged. ARM64 MP1 complete (#52); MP2 then MP3 remain. Main CI passed:
https://github.com/auto44e/jeios/actions/runs/41376309893 . M4 qualifies bounded
supervisor/broker/COM2 UART scenarios, not arbitrary hardware, DMA or a release.
Mobile main `f1d9b0d87746e9dc353a2f48e7b9d27e7fe86d07` has passing planning CI:
https://github.com/auto44e/jeios-mobile/actions/runs/39243515951 . Its core pin
remains older M1/M2; upstream progress does not qualify that integration.
No open PRs were observed before this refresh. M4 #53 is now closed as completed;
MP2 (mobile #2) is next execution work; M5a #54 remains planned.

Shell roadmap: SH1 recovery #71, SH2 original native sh #72, included native
Bash/Zsh and custom-shell registration #73; mobile parity #12. PowerShell deferred.
All shell implementation remains planned. Storage, networking, desktop, POSIX
completion, LDAP, encryption, fleet/ISO 27001 features, RISC-V, accelerators and
Android execution remain later gates. Standards adoption is not certification.
This current status supersedes older next-task/status wording below; timestamped
records remain evidence at their original revisions, not new test runs.

## Current order

1. MP2: ARM64 MMU/memory/timer/scheduling, then MP3 userspace parity (mobile #2).
2. M5a #54: checked read-only initfs, bounded service loading and SH1 #71 recovery
   console; one shared shell foundation, no second console parser.
3. SH2 #72 under POSIX #15: original native sh scripting qualification.
4. SH3 #73: included latest-stable Bash/Zsh native integration and user shell choice.
5. Qualify mobile shell parity #12 and each additional ISA/product separately.

See [SHELLS.md](SHELLS.md) for contracts and acceptance. No runtime or frozen ABI
change is made by this refresh. Storage #13/#14, CI #18 and assurance #19 continue
as bounded supporting tracks. No real-device flashing/formatting authorized.

## Historical phase plan — 2026-10-19 (superseded ordering/status)

# Next phase: native service foundation

Owner requested this planning/docs/board update on 2026-10-19 after reviewing the
next-phase recommendation. Status: planned, not a runtime implementation or a new
frozen ABI. Existing ADR and tested-path owner gates remain in force.

## Outcome and order

JeiOS should boot a reproducible read-only image, load isolated services, expose a
small serial diagnostic console, and recover a deliberately failed service while
an independent service continues. This means running JeiOS's own services, not
building the OS with a compiler hosted inside it.

| Order | Work and tracker | Acceptance boundary |
| --- | --- | --- |
| First bounded task | [#52: finish MP1 and ARM boot CI](https://github.com/auto44e/jeios/issues/52) | Bounded copied DTB parser; malformed-input tests; normal/vector boots on pinned QEMU in remote CI |
| Next core gate | [#53: M4 supervisor and devices](https://github.com/auto44e/jeios/issues/53) | Reviewed lifecycle/authority contracts; authenticated readiness; bounded restart; stale grants rejected; device timeout/detach |
| After relevant M4 gates | [#54: M5a read-only boot environment](https://github.com/auto44e/jeios/issues/54) | Checked initfs; bounded service loading; diagnostic console; crash/recovery demonstration |
| Alongside, sequential ARM subgates | [Mobile #2: MP2 then MP3](https://github.com/auto44e/jeios-mobile/issues/2) | MMU/timer/scheduling first, then EL0 isolation/handles/IPC with native ARM evidence |
| Supporting assurance | [#18 CI](https://github.com/auto44e/jeios/issues/18), [#19 coding profiles](https://github.com/auto44e/jeios/issues/19) | Reproducible failures/logs, qualified compiler/static checks, bounded rule enforcement demonstrated to reject violations |

M4 is not implied by M3 completion. M3's init is a kernel-seeded two-process
fixture, not the general supervisor in INIT.md. The current two-process limit and
one-thread-per-process model cannot simply host a supervisor, failing service and
independent survivor simultaneously. Review process creation, loader, capacity and
reaping changes before expanding them; do not silently edit frozen/tested paths.
A new service instance must receive new scoped authority; a reusable handle slot
or endpoint object does not by itself establish the service-generation contract.

M4 starts with a reviewed contract and small implementation steps: lifecycle and
readiness, failure/restart/reconnect, then scoped device ownership/grants and one
qualified basic virtual device. Driver selection, DMA policy and numeric budgets
remain decisions for that contract. Kernel-driver failure is not process isolation.

M5a coordinates its storage interface with [#13](https://github.com/auto44e/jeios/issues/13)
rather than inventing a second VFS. Reject corrupt/truncated/out-of-range images,
deny writes, bound memory/work, and prove rollback after failed loads. Read-only
block access/ext4 follows separately. This phase does not finish durable M5.

## Demonstration and evidence

Record exact commit, image hash, compiler/QEMU versions, profile, commands, logs
and expected markers. Prove successful startup/readiness, file reads, survivor
progress after a peer fault, bounded restart/backoff, rejection of stale/spoofed
readiness/authority, exhausted-resource cleanup and failure diagnostics.
Retain AMD64 regression gates. Qualify each ARM gate independently; compiling ARM
or passing hosted tests cannot establish native execution or cross-ISA parity.
No deadline, active assignee or independent human qualification is implied.

## Scope and decision record

Decision and reason: prioritize native services and a read-only useful environment
because storage, identity, networking and applications depend on their lifecycle
and authority boundaries. ARM progresses beside core work using the same kernel.

Predictions for alternatives: a graphical demo would add presentation before
service recovery is established; writable multi-backend storage would add recovery
and format obligations before the read-only loading path is qualified. These are
planning judgments, not measured results.

Flex: revisit device choice, process budgets and slice boundaries at the M4 ADR
and after measured emulator results. Reordering cannot waive isolation, ownership,
provenance, authority or recovery requirements. No existing ADR is changed here.

Desktop graphics, POSIX completion, LDAP login, encryption/relock, immutable
updates/rollback, production Btrfs/OpenZFS/F2FS, Pixel bring-up and Android apps
remain later gates. Their accepted requirements stay intact. Bounded contracts
and probes (#13–#16 and mobile #7) may proceed without displacing the phase.
No real-device flashing, formatting or security-policy relaxation is authorized.
