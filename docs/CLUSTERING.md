# Dynamic clustering and distributed computing

Owner requirement: 2026-11-04 (America/New_York). Status: planned; no runtime
clustering, native distributed workload or security qualification is claimed.
Trackers: [core #82](https://github.com/auto44e/jeios/issues/82),
[mobile #14](https://github.com/auto44e/jeios-mobile/issues/14).

## Product goal and scope

Let authorized JeiOS devices cooperate as one understandable computing environment,
from a person's laptop, desktop and mobile devices to enterprise compute fleets.
One shared architecture provides discovery, membership, job placement, lifecycle,
results and policy, with per-device/profile qualification. The user sees where
work runs, what data is shared, why placement was chosen and how to stop it.

Start with explicit, portable jobs and applications using a native distributed
SDK. Candidate workloads include compilation, batch media processing, simulations
and partitionable inference where their own correctness/quality gates permit.
An ordinary application can continue locally; it does not automatically become
parallel just because more devices join. Transparent process migration,
distributed shared memory, pooled GPU memory and tightly coupled HPC are separate
research gates with workload/ISA/network constraints, not baseline promises.

## Intuitive defaults and programmable control

| Profile | Default direction | Advanced controls |
| --- | --- | --- |
| Personal | Explicitly enable a trusted-device pool; preview members and sharing scope; local execution remains available | Per-app/device permissions, schedules, quotas, placement and data rules |
| Household or team | Invite individual identities; separate owners and workspaces | Fair sharing, delegated administration, reservations and group policies |
| Enterprise | Explicit enrollment and tenant policy; isolated workload identities | Queues, priorities, quotas, reservations, placement constraints, admission policy, HA and audit export |

Discovery and same-account pairing do not authorize computation or data disclosure.
After opt-in, a conservative personal preset uses approved devices on approved
local networks, favors plugged-in/idle resources, protects foreground latency and
pauses sharing for thermal/battery limits. WAN, metered links and cloud destinations
require explicit policy. Allow users to adjust these choices within their actual
authority; UI difficulty never changes security enforcement. Provide one visible
stop-sharing control, graceful drain and bounded forced cancellation.

Expose the same versioned control contract through GUI, CLI, API and SDK. Policy
supports declarative placement/resource budgets and programmable scheduling
extensions in bounded unprivileged services; extensions cannot override mandatory
authorization or access arbitrary credentials. Explain rejected requests and
unsupported resources in plain language. Preview consequential policy changes,
record them, and support configuration rollback without undoing completed external
side effects. Administrators can manage their scope without receiving users'
personal decryption keys or universal access to their jobs.

## Security and trust contracts

Inherit [ZERO_TRUST](ZERO_TRUST.md), [IDENTITY](IDENTITY.md),
[ENCRYPTION](ENCRYPTION.md), [DOMAINS](DOMAINS.md) and
[organizational security support](ORG_SECURITY.md). Devices remain distinct
security domains. Coordinator membership never grants blanket root, file, device,
network or data-key access. Local and remote workers enforce scoped native
capabilities and mandatory policy at each operation; remote credentials are not
raw transferable kernel handles or pointers.

Specify authenticated device/workload identities, proof of possession, enrollment,
trust-domain and tenant boundaries, short-lived delegated grants, audience/job/data
scope, key rotation, revocation and bounded offline behavior. Login, authentication,
cluster membership, scheduling and data access are separate decisions. Use reviewed
protocols/libraries for mutually authenticated encrypted control and data channels,
consistent with the project's PQ migration requirements; no custom cryptography.
A required guarantee that cannot be delivered blocks that profile.

A worker that processes plaintext can generally access that plaintext within its
trust boundary. Encryption in transit does not hide it from that worker. Only send
data to authorized recipients; confidential-compute/attestation and verifiable
computation require separate evidence. Attestation alone does not prove result
correctness. Revocation prevents future grants and bounds continuing use through
leases; it cannot instantly reach a partitioned node or erase data already copied.
Define behavior on lock/relock, suspend, logout, lost devices and tenant removal;
never silently export an unlocked secret so a remote job can survive relock.

Workers are sandboxed userspace services with restricted network/file/device
access, resource quotas, signed or otherwise policy-verified workload provenance,
and validated versioned messages. Authenticate artifacts/results, bind them to
job/input/version/attempt identities, and validate results according to workload
risk. Hashes establish integrity, not honest computation. Specify redundant checks
or trusted-worker requirements when needed. Audit grants, placement, failures and
administrative changes with bounded retention and redaction; no secret-bearing
logs or mandatory personal telemetry. Imported runtimes inherit
[NATIVE_PRODUCTS](NATIVE_PRODUCTS.md) hardening and provenance gates.

## Dynamic membership, placement and resource accounting

Define node states: discovered, verified, admitted, available, draining, offline
and revoked. Authenticate capability advertisements; resource discovery is a hint,
not authority or proof of support. Negotiate protocol/runtime versions; reject
incompatible peers explicitly. Bound membership, queues, message sizes, retries,
concurrent jobs and diagnostic storage. Specify ownership, locks, cancellation,
timeouts and cleanup for each job/grant/buffer and reconnect path.

Placement considers CPU ISA/features, ABI, runtime and package versions, memory,
accelerator operation/precision, isolation, locality, link bandwidth/latency,
transfer cost, power/thermal budgets and foreground work. Follow
[ARCHITECTURE_SUPPORT](ARCHITECTURE_SUPPORT.md) and
[ACCELERATORS](ACCELERATORS.md); AMD64, ARM64, RISC-V, GPU and NPU support each
needs evidence. APU CPU/GPU components are accounted for by their actual resources.
Use architecture-specific artifacts or a separately qualified portable runtime;
never assume binaries or memory layouts migrate unchanged across architectures.

Keep inputs versioned, allowlisted and encrypted as required; define cache quota,
retention, deletion and tenant separation. Transfer only authorized inputs/results,
not whole home directories. Enforce data-residency and destination restrictions at
both scheduling and transfer boundaries. Measure end-to-end benefit including
serialization, transfer, queueing and validation. Prefer local execution when
remote work does not help; user policy can require local-only or a named pool.
If local resources cannot satisfy a guarantee, report blocked/queued/unsupported
rather than silently use a forbidden remote target.

## Failure, consistency and side effects

The initial design uses a single coordinator with explicit loss behavior; HA is a
later gate. Define durable job IDs, attempt IDs, ownership epochs, bounded leases
and storage-enforced fencing before automatic takeover or reassignment. Lease
expiry alone cannot stop an old worker writing to a sink that ignores fencing.
Treat clock skew, restart and suspend explicitly; prove the chosen timing and
coordination assumptions. During partitions, pause operations that require shared
authority/quorum; independent local work may continue only within valid grants.
Never trade tenant isolation or consistency guarantees for availability silently.

Begin with stateless/idempotent jobs. A retry may execute work more than once;
use deduplication and atomic result publication, and expose ambiguous completion.
External side effects require sink-supported transactions/idempotency keys or
explicit user/application reconciliation. Do not promise universal exactly-once
execution. Cancellation acknowledgement and disconnect are not proof a remote
side effect did not happen. Checkpoint/restart needs workload-defined compatible
state, encrypted storage and restart tests; live migration remains separate.

## Delivery gates and evidence

These gates extend the ecosystem roadmap without displacing current kernel,
ARM64, storage, networking or shell milestones. No cluster protocol/ABI is frozen
by this document; existing ADRs and tested security contracts remain authoritative.

| Gate | Deliverable | Acceptance |
| --- | --- | --- |
| CL0 | Reviewed threat model and job/membership/policy/failure contracts | State machines, trust boundaries, compatibility matrix, quotas, failure semantics and tests; choose protocols/components with provenance |
| CL1 | Hosted multi-node model and deterministic fault harness | Join/leave, replay, stale leases, partitions, duplicate attempts, fencing, cancellation, malformed messages and resource exhaustion; models do not prove native isolation |
| CL2 | Two-device native stateless jobs | Qualified networking, identity, key/storage, process and quota prerequisites; real execution, denied authority, coordinator loss, result integrity and local fallback evidence |
| CL3 | Personal pool UX and heterogeneous jobs | Tested opt-in, pause/drain/revoke, relock, mobile suspend/battery, transfer-inclusive benefit and per-ISA/backend correctness |
| CL4 | Enterprise scheduling and HA | Tenant isolation, policy precedence, admission/quotas, quorum/fencing, rolling upgrades, audit, recovery and sustained churn/load evidence |
| CL5 | Advanced distributed workloads | Workload-specific checkpoint/migration, collective communication and accelerator tests; scope claims to measured capabilities |

Use [CODING_STANDARD](CODING_STANDARD.md) risk profiles and
[CI_STRATEGY](CI_STRATEGY.md): static analysis, parser/protocol fuzzing, negative
and race tests, allocator/I/O fault injection, reproducible builds and upstream
regressions. Test stolen/revoked identities, hostile nodes, cross-tenant access,
network partitions/reordering/duplication, clock changes, coordinator recovery,
corrupt artifacts/results and mid-update compatibility. Preserve seeds/logs and
separate host, emulator and physical-device evidence. Critical authority and
release paths need independent review before hardened qualification.

Shared implementation belongs in core; mobile owns integration and evidence for
suspend/relock, battery/thermal budgets, connectivity churn and work/personal
separation. Organizations may use audit/policy evidence toward their controls;
clustering does not confer ISO 27001 certification or blanket hardware support.
