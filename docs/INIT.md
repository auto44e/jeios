# Init and service management design

Status refreshed 2026-10-19: M3c completed a kernel-seeded two-process bootstrap
fixture with IPC, waits and clean teardown. The general userspace supervisor below
remains planned for M4 (#53); the fixture does not implement it. See
[NEXT_PHASE.md](NEXT_PHASE.md). This design does not freeze a public ABI.

## Decision and division of responsibility

Use a small C first-process supervisor, with separate policy, configuration,
logging, device and session services. The supervisor owns process lifecycle,
reaping, service generations, readiness deadlines and recovery transitions.
It must not contain a desktop, network stack, package manager or arbitrary plugin
runtime. A modular policy service selects a validated launch plan; kernel handle
rights remain the authority boundary even if the policy service crashes.

This is a native JeiOS interface, not a systemd unit-file or Unix PID 1
compatibility promise. A future POSIX personality can map its semantics explicitly.
A replacement init is possible under the owner-controlled boot policy, provided
it satisfies the same startup, reaping, recovery and authority contracts. A broken
replacement must remain recoverable through a separately selectable rescue image.

## Configuration and intuitive defaults

Use versioned declarative manifests with typed fields, bounded sizes and no
implicit shell expansion. Layer distribution defaults, device/profile settings,
owner overrides and per-user overrides in a documented order. Unknown mandatory
fields, duplicate identifiers, invalid rights and dependency cycles are errors.
Show effective values, their source, and the difference from defaults. Reverting
an override restores the inherited value. UI complexity never changes authority.

GUI, CLI and automation call the same checked API: inspect, explain, validate,
plan, apply, start, stop, restart and reset-to-default. These are proposed
operations, not commands available today. Dry-run reports services affected,
new permissions, restart requirements and non-reversible effects. An owner can
customize service selection, dependencies, deadlines, resource limits, activation,
logging and recovery policy. Permission expansion requires an explicit authorized
change, never a dependency resolving itself by acquiring ambient privileges.

Default desktop: local services only, no remote administration, no telemetry,
network listeners only when needed, optional sharing disabled until enabled.
Headless/IoT profiles have bounded logs and offline recovery. Mobile profiles
coordinate activation with suspend and battery policy. Accessibility and rescue
remain available when optional desktop customization fails. No boot service may
wait forever for network connectivity or an absent peripheral.

## Manifest sketch (illustrative, not a supported parser)

```toml
schema = 1
id = "jeios.share"
executable = "package:jeios.share/service"
activation = "explicit-user-request"
after = ["jeios.network"]
requires = ["jeios.identity", "jeios.consent"]
readiness = "authenticated-channel"
startup_timeout_ms = 10000
stop_timeout_ms = 5000
restart = "on-failure"
restart_limit = 3
restart_window_ms = 60000
memory_limit_bytes = 67108864
capabilities = ["network.local", "files.user-selected"]
```

Values illustrate policy, not measured minimums. The capability names must be
resolved into actual scoped handles by an authorized broker; strings grant
nothing. No file-tree-wide or unrestricted network access follows from this
example. Identity/consent services must themselves be available without sharing.

## Lifecycle, dependencies and failures

States: inactive, starting, ready, stopping, failed and backoff. Each launch has
a fresh generation. Only an authenticated readiness message from that generation
can transition starting to ready. Merely creating a process is not readiness.
Use monotonic deadlines; wall-clock changes cannot extend a timeout indefinitely.

`after` imposes ordering when both services are selected. `requires` selects a
mandatory dependency and blocks dependent readiness when it fails. Optional
`wants` may fail with a visible degraded state. Validate the combined graph before
side effects. A runtime dependency loss uses a declared stop, pause or degrade
policy; it must not silently leave a service reporting healthy. Health probes
are bounded and distinct from process liveness. Avoid restart cascades through
per-service and aggregate budgets; capped backoff prevents tight retry loops.

Stopping first rejects new work, then drains/cancels outstanding work within a
deadline. Revoke grants and generation-bound channels before resources can be
reassigned. Reaping releases process references exactly once; clients see a
service-restarted status, not an accidental connection to a reused identifier.
Destructive operations require idempotency tokens or explicit outcome-unknown
errors when the server dies after committing but before replying.

Critical-service failure enters a minimal rescue graph with local diagnostics.
Optional-service failure does not halt the OS. Supervisor corruption cannot be
safely repaired by pretending execution succeeded: use a defined watchdog/recovery
path once platform support exists. No silent infinite reboot loop. During shutdown,
stop producers before storage/key services; report flush failure and preserve
recovery evidence. Forced power-off has an explicit data-loss implication.

## Ownership, concurrency and resource contracts

One serialized supervisor event loop initially owns the service table. Launch
plans are immutable, generation-tagged snapshots. Parsing and policy evaluation
occur outside lifecycle critical sections. Kernel syscalls provide atomic process
and handle operations; callbacks do not run while supervisor state is locked.
Bound service count, dependency edges, manifest bytes, queued events, pending
requests and log storage. Admission failure returns quota/exhaustion without
partially publishing a service. Numeric limits are set and tested at M4.

The supervisor owns child lifecycle handles; clients receive attenuated service
channels, never those lifecycle handles by default. User managers cannot control
other users or system services. Read-only inspection must redact credentials,
environment secrets and private arguments. Logs are local, rate-limited and
bounded; a failing logging service cannot block reaping or recovery.

## Applying configuration and qualifying implementation

Prepare and validate a new graph, authorize its rights delta, persist a new
configuration generation, then enact a bounded transition plan. Track desired
and observed state separately. Roll back reversible configuration failures;
record compensation/outcome-unknown for external effects. Do not claim atomicity
across arbitrary services. Keep a verified known-good recovery configuration
independent from a broken owner override; owner trust controls who can replace it.

M3c proved bounded bootstrap IPC/waits and teardown; cancellation is internal-only
in its v1 ABI. M4 must qualify general supervisor lifecycle and then test cycles,
missing optional/required services, spoofed/stale readiness, dependency loss,
crash storms, exhausted queues, supervisor policy-service loss, user separation,
configuration interruption and shutdown timeouts. Prove CLI/GUI authorization
parity later in M7. Measure launch latency and memory on a low-resource profile
before choosing defaults. Configuration format and compatibility policy need an
ADR before becoming stable; this sketch must not accidentally become the ABI.

## Identity-aware service lifecycle

[IDENTITY.md](IDENTITY.md) defines local principals, sessions and enrollment.
Per-user services bind a session/principal and cannot inherit another logged-in
user's capabilities. Session logout drains or cancels work and revokes its grants;
a separately authorized background-sync service has an explicit key-residency
policy. Device-wide enrollment policy cannot silently expand per-user service
rights. A controller outage must not prevent permitted local login or service
startup; enterprise offline policy is a separately visible configuration.
