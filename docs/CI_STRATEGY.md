# CI and qualification strategy

Owner direction: 2026-10-16T19:55:53-04:00. Adopt curl-inspired CI/testing in
near-term work and as a long-term engineering discipline, using complementary
OS-specific techniques where appropriate. Status: accepted direction; this change
adds a plan, not new workflow jobs, test frameworks or coverage claims.

## Why curl, and what to adapt

Curl combines a portable regression suite, build/configuration diversity, static
analysis, sanitizers, fuzzing and repeatable error-path testing. Its torture mode
first measures fallible calls, then reruns a case failing each call individually
and checks for crashes/leaks; bounded sampling and failure replay are available.
Test Clutch aggregates results across runs to expose patterns. This is a testing
architecture and maintenance practice, not a single CI service to install.

Use GitHub Actions as the current orchestrator; additional providers are justified
only by distinct target coverage or infrastructure resilience. More jobs/providers
are not themselves better evidence. Do not blindly copy curl workflows, protocol
fixtures, custom harness, language choices or supported-platform claims into a
freestanding C kernel. Review provenance/licenses before importing any source.

Complement curl with Linux's distinction between internal kernel tests and
userspace-driven interface tests, and SQLite's allocation/I/O/crash testing for
persistent state. KUnit/kselftest depend on Linux; adapt concepts to JeiOS rather
than claiming those frameworks run unchanged. SQLite's published methods are
references, not a license to redistribute every SQLite test suite. No universal
ranking of these projects is needed: each addresses different failure surfaces.

## Existing evidence and gaps

Inspected core PR #17 head: 507c06ab397add3d42ba2440670ace7af04ddeab.
Current .github/workflows/ci.yml has a single Ubuntu 24.04 job on push/PR: GCC
make check (hosted sanitizers and ELF checks), BIOS boot smoke, NX/no-NX profiles,
16 MiB/1536 MiB memory profiles and seven CPU exception/protection fault modes.
The parser includes deterministic mutations and allocator tests cover exhaustion
and rollback. QEMU requires explicit success/fault markers; timeout is failure.
These are valuable foundations, not full fuzzing, syscall coverage or OS security
qualification. Current dependency installations are not a fully pinned supply chain.
Mobile has manifest/plan validation and four schema tests, not ARM64 execution.

## Staged delivery

| Stage | Work | Acceptance boundary |
| --- | --- | --- |
| Near term, alongside M3a | Separate fast hosted checks and QEMU jobs; GCC plus qualified Clang hosted tests; static analysis; stable IDs and replay logs; bounded fail-Nth-operation tests | Preserve every current gate; intentional defects fail; recorded case reproduces locally; no missing/skipped suite shown as pass |
| As M3/M4 mechanisms land | Syscall/handle/IPC negative tests, service crash/restart, allocation/map/transfer rollback and concurrency schedules | Actual isolated processes and survivor progress; resource accounting and authority remain correct |
| As M5/M6 services land | Storage interrupted/reordered/torn-write tests, protocol fixtures, parser fuzzing, POSIX behavior and LDAP/TLS failure tests | Disposable images/labs, independent data checks, native execution; no real organization or user media |
| ARM64/mobile qualification | Reuse applicable core tests on native ARM64, then named physical hardware | Host build or QEMU success is distinct from physical device support; test suspend, power, thermal and detach |
| Release maturity | Upgrade/rollback/relock/recovery suites, artifact provenance, reproducible-build checks and result trend dashboard | Exact release configuration and target evidence; signing only through separate trusted release process |

First implementation task: [core #18](https://github.com/auto44e/jeios/issues/18).
This testing work supports M3a; it must not replace the isolation gate with a CI
redesign. Stage manageable PRs and measure latency/cost before expanding matrices.
Mobile owns platform qualification while common harnesses stay with shared core.

## Execution tiers and honest outcomes

- PR: bounded deterministic regressions, selected compiler/analyzer lanes and
  existing mandatory emulator cases; short fuzz corpus replay where applicable.
- Scheduled/deeper: longer fuzz campaigns, complete rather than sampled failure
  sweeps, broader configurations and stress schedules; explicit seeds and budgets.
- Release: curated complete supported-profile evidence, recovery and physical
  hardware results where those claims apply. Unavailable hardware blocks that
  support claim, not an unrelated target's development.

A workflow schedule is an implementation follow-up, not an automation created by
this document. Choose numerical budgets after measurement. Keep coverage of hosted
code, kernel runtime, specification requirements and hardware distinct. Coverage
percentages guide missing cases; they are not proof of correctness or security.

Expose pass, assertion failure, infrastructure error, unsupported, skipped and
quarantined outcomes separately. Retrying an infrastructure failure must preserve
its first result. Do not rerun a flaky functional test until green and call it fixed.
A quarantine needs a tracked reason, responsible maintainer and review/expiry;
it does not waive a mandatory safety/conformance requirement silently.

## Curl-style failure injection adapted to JeiOS

Start with a bounded existing allocator/module rollback path. Run baseline, count
eligible operations, fail each operation index in independent resets, and assert
resource accounting, ownership and externally visible state. Record seed, operation
index, exact build/configuration and replay command. Sample only when explicitly
reported, retaining failed inputs as deterministic regression cases.

As native mechanisms arrive, cover map failure, handle-table exhaustion, partial
IPC transfer, cancellation, device loss and persistence errors. Define whether a
fault is one-shot or sustained and where it is observed; do not assume a single
failure sweep covers correlated faults or concurrency schedules. After a failed
transaction, no leaked pages/handles, stale authority or half-published object may
remain. Recovery invariants must be subsystem-specific and independently asserted.

Build injection hooks only into explicit test configurations; normal builds must
not expose a production fault-trigger interface. A diagnostic crash must occur in
the intended domain; arbitrary kernel corruption is not recoverable by assertion.
Hosted ASan/UBSan and Valgrind do not automatically instrument a freestanding guest.
Kernel instrumentation, races, DMA and architectural state need separate tooling
and actual execution evidence. Keep leak checking enabled in capable CI; local
process-inspection exceptions must remain recorded, never global suppression.

## Reproducibility and CI trust

Record source/merge SHA, compiler/linker/emulator versions, target CPU and memory,
flags, feature profile, corpus/seed, test ID, expected and observed markers, logs
and relevant artifact hashes. Capture failures even when a step aborts; bound
retention/storage and redact data. Small replay commands should work without a
production account or live external service. Use versioned disposable fixtures.

Untrusted PR jobs must not receive production signing keys or persistent privileged
runner access. Do not execute untrusted PR code under a privileged target-event
workflow. Use least-privilege job permissions, reviewed/pinned action and dependency
revisions with an update process, and isolated hardware runners when introduced.
Changes to paid services, runner trust, release signing and repository required
checks need their own reviewed setup; this plan changes none of those settings.

## Primary references reviewed 2026-10-16

- [curl CI](https://github.com/curl/curl/blob/master/docs/tests/CI.md).
- [curl test architecture](https://everything.curl.dev/internals/tests/index.html).
- [curl torture tests](https://everything.curl.dev/internals/tests/torture.html).
- [curl Test Clutch](https://testclutch.curl.se/).
- [SQLite testing](https://www.sqlite.org/testing.html).
- [Linux testing overview](https://docs.kernel.org/next/dev-tools/testing-overview.html).

Pin applicable upstream revisions when implementing; moving documentation links
are design references, not reproducible build dependencies.

## Coding-policy enforcement

[CODING_STANDARD.md](CODING_STANDARD.md) supplies risk profiles and JCS rule IDs.
Core #19 maps rules to versioned checkers or manual evidence alongside #18.
Demonstrate that new gates reject representative defects before relying on them.
No whole-ecosystem compliance or new static-analysis job is claimed by this plan.
