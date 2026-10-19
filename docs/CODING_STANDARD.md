# Ecosystem coding and assurance policy

Owner direction: 2026-10-16T20:01:26-04:00, reaffirmed
2026-10-16T20:08:52-04:00. Apply the spirit of NASA/JPL engineering throughout
JeiOS. This is a tailored project policy adopted through merged PR #17, not NASA certification,
literal Power of Ten conformance, or evidence that existing code has been audited.
Automated enforcement and baseline assessment are tracked in
[core #19](https://github.com/auto44e/jeios/issues/19), alongside
[CI #18](https://github.com/auto44e/jeios/issues/18).

## Scope and risk profiles

Apply to original code, tests, build tools, packaging, services, SDKs, desktop,
mobile and future IoT integrations. Human and AI contributions use the same rules.
Classify by failure impact and authority, not implementation language or directory.
These are JeiOS profiles, not NASA software classifications.

| Profile | Typical components | Required emphasis |
| --- | --- | --- |
| Critical | Kernel, boot, privileged drivers, isolation, identity/key services, storage commits, updates/recovery, release tooling | Resource bounds, explicit invariants, failure/rollback evidence, independent review before a hardened/release qualification claim |
| Service | Contained brokers, networking, compatibility runtimes, persistent app services | Validated interfaces, quotas, cancellation, restart/recovery, concurrency and hostile-input tests |
| Application/tool | Ordinary UI, SDK examples, development utilities | Language-appropriate lint/type checks, error handling, tests proportionate to risk, reproducible dependencies |

A UI authorization prompt, image packager or signing helper can be critical.
An unclassified component must be assessed before privileged integration. A service
profile assumes demonstrated containment; a module running in ring 0 is critical.
Do not impose C syntax restrictions on Python or other future userspace languages.
The shared kernel remains C11 with architecture assembly at documented boundaries.

## Ten JeiOS rules

| ID | Rule | Review or test evidence |
| --- | --- | --- |
| JCS-01 | Keep control flow simple and functions focused. Critical C prohibits direct/indirect recursion and setjmp/longjmp absent an approved deviation. A forward cleanup goto is permitted when it makes ownership unwind explicit. | Call/cleanup review; flag functions over roughly 60 logical lines for decomposition or recorded rationale, not arbitrary splitting. |
| JCS-02 | Bound work, buffers, retries, stack use and queues. Each finite critical loop needs a demonstrable input/capacity bound or explicit budget. | Limit and timeout tests; document maximum work per dispatch. Persistent scheduler/event loops require progress/block/yield and shutdown semantics where applicable. |
| JCS-03 | Specify ownership, lifetime and allocation context. No unbounded allocation; prohibit blocking allocation in interrupt, fault and other nonblocking paths. | Exhaustion, fail-Nth allocation, partial-init unwind and use-after-free checks. Use preallocation/pools for deterministic paths. |
| JCS-04 | Validate external inputs before use, including sizes, arithmetic, alignment, encodings and authority. Check fallible results; justify deliberate ignored results locally. | Boundary, overflow, malformed-input, permission-denial and error propagation tests. Internal helpers may rely on documented validated preconditions. |
| JCS-05 | Encode meaningful invariants with side-effect-free assertions and explicit failure policy. Runtime input validation must survive release builds. | Pre/postcondition and failure tests; malformed user/network data must not become a kernel assertion crash. No artificial assertion quota. |
| JCS-06 | Minimize scope and mutable shared state. Document lock order, caller context, atomic ordering and cancellation ownership. | Concurrency/lifetime review and targeted race schedules as concurrency lands; volatile is not synchronization. |
| JCS-07 | Keep preprocessing, pointer manipulation and unsafe boundaries small and visible. Prefer typed helpers; validate callbacks and document their allowed targets/lifetimes. | Review casts, pointer arithmetic, macro evaluation and dispatch tables. No hidden multiple evaluation or unsupported configuration claims. |
| JCS-08 | Build with documented strict warnings and appropriate static analysis. Resolve new findings or record narrow reviewed deviations. | Exact tool versions/configuration and results; no blanket suppression or lowering warnings to pass. Analyzer coverage must name the files/configurations checked. |
| JCS-09 | Trace behavior to a requirement/contract and repeatable evidence, especially failure paths. | PR links contract, changed invariant, positive/negative tests and exact build/target; preserve regression inputs and distinguish host, emulator and physical evidence. |
| JCS-10 | Keep changes reviewable, dependencies traceable and exceptions explicit. Test infrastructure itself must detect known defects. | Review record, provenance, version pin/update policy, gate failure demonstrations and deviation register. Never fabricate independent review or a passing check. |

## Deliberate adaptations of the Power of Ten

The original C-oriented paper restricts allocation after initialization, function
pointers, pointer indirection, recursion and control flow. It also advocates small
functions, assertions, narrow scope, checked results and strong analysis. JeiOS
adopts the analyzability goal with the explicit adaptations above. Dynamic process
and filesystem workloads need allocation; drivers need reviewed dispatch tables.
These mechanisms require quotas, exhaustion handling, clear lifetimes and tests.
Only paths with a justified deterministic contract prohibit runtime allocation.

Do not translate a timing budget into a claim of proved worst-case execution time.
Do not treat passing sanitizers, high coverage or a style checker as proof of
memory safety, functional correctness or security. Assembly needs register/stack,
clobber, interrupt, barrier and architecture-precondition review plus execution
tests; a C analyzer cannot validate it by implication.

Assertions detect internal contract violations. Recovery must match the failure
domain: reject invalid input, fail a transaction, terminate an isolated service,
or stop safely on unrecoverable kernel corruption. Never continue with corrupted
authority or persistent state merely to avoid a crash.

## Adoption, review and deviations

After adoption, new and materially changed original code must address applicable
rules in its PR. Existing code has no retroactive compliance claim: first inventory
critical paths and record findings, then fix them in bounded changes. A known
critical defect blocks the affected qualification claim; an untouched baseline
finding needs an owner and disposition, not silent grandfathering.

PR evidence must identify profile, applicable rule IDs, changed contracts, tests,
unverified checks and deviations. Independent review is required before claiming
critical hardened/release qualification. If another qualified reviewer is not
available, record that gap; author/AI self-review is not independent assurance.
Development may continue with that qualification explicitly unclaimed.

A deviation record includes ID, rule, exact scope/revision, reason, alternatives,
risk, mitigation, verification evidence, responsible owner, actual reviewer/status
and expiry or a concrete revisit trigger. Pending is not approved. Put records
under docs/deviations/ when first needed; do not invent a reviewer or waiver.
Rule-specific permissions above are policy adaptations, not hidden deviations.

Imported code retains its provenance, language and license. Evaluate exposure,
upstream quality, update strategy and integration tests; isolate it where possible.
Do not relabel it as conforming or perform an enormous style rewrite just to match
local rules. Any critical dependency needs evidence suitable for its actual role.

## Delivery stages and enforcement status

1. Policy published in core and referenced by mobile. M3 and MP0 are complete;
   NEXT_PHASE.md records current sequencing. Documentation is not enforcement.
2. Near term (#19 with #18): classify current components, audit critical baseline,
   map each rule to a checker or manual review, and add bounded CI checks. Record
   tool versions and prove a representative violation fails each new gate.
3. As services arrive: extend failure injection, parser fuzzing, concurrency tests,
   durable-state recovery and traceability to POSIX/LDAP requirements. Qualify
   native targets separately; keep diagnostics and test outcomes honest.
4. Before hardened/release claims: complete applicable reviews and unresolved-risk
   disposition, verify reproducible evidence and target-specific recovery. Use
   deeper structural coverage or formal models where the identified risk warrants
   them; neither 100% coverage nor formal verification is claimed today.

Existing checks and gaps remain documented in [CI_STRATEGY.md](CI_STRATEGY.md).
This policy change adds no analyzer, workflow, branch-protection setting or runtime
mechanism. It does not certify safety-critical or aerospace use.

## Primary references reviewed 2026-10-17 UTC

- [NASA NPR 7150.2D, Chapter 4](https://nodis3.gsfc.nasa.gov/displayDir.cfm?Internal_ID=N_PR_7150_002D_&page_name=Chapter4): project coding standards, static analysis, repeatable tests, requirements and target validation.
- [Gerard J. Holzmann, The Power of Ten](https://spinroot.com/gerard/pdf/P10.pdf): compact, analyzable rules for safety-critical C, published with NASA/JPL affiliation.

These are engineering references. NASA requirements apply within their own scope;
JeiOS does not claim agency approval or complete compliance by citing them.
