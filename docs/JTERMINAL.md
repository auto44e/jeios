# JTerminal: native terminal and persistent sessions

Owner requirement: 2026-11-06 (America/New_York). **Planned for future
implementation; no terminal, session persistence or remote access is implemented
by this document.** Product name: **JTerminal**.

Implement only after the [current-code stabilization gate](audit/2026-11-fix-gate.md)
and the prerequisites below. This adds a roadmap, not authorization to bypass
existing gates or modify frozen contracts. Follow [coding standards](CODING_STANDARD.md),
[system design](DESIGN.md), [standards policy](STANDARDS.md) and per-platform evidence.

## Goal and scope

A native JeiOS terminal with intuitive defaults, deep customization and documented
programmatic control, from one user's devices to enterprise fleets and distributed
systems. Inspiration: Ghostty native interaction, kitty's documented terminal
extensions, tmux session workflows, and Rex's cross-device terminal experience.
These are design references, not approved source imports or dependencies.

Separate four responsibilities:

| Component | Contract |
| --- | --- |
| JTerminal app | Accessible native rendering, input, tabs, splits, search, session browser and monitoring |
| Session service | Userspace ownership of PTYs/process groups, attachment, bounded output history and lifecycle |
| Ecosystem connectivity | Shared device identity, enrollment, discovery, encrypted transport, revocation and policy |
| Shell or application | Native sh, included Bash/Zsh, registered compatible shells and terminal applications |

The [recovery console and shell roadmap](SHELLS.md) remains independent of the
GUI, networking and session directory. No implicit root shell. Core mechanisms
follow existing C/assembly policy; GPU rendering is optional with a qualified
software fallback. Desktop/mobile/server share service contracts, not necessarily
identical layouts. Every ISA/product profile requires its own qualification.

## Standards and interoperability

Target the project's selected POSIX terminal/termios and job-control interfaces,
PTY semantics, ECMA-48 control functions, documented VT/xterm compatibility,
Unicode text handling and accurate terminfo capabilities. Pin specification
versions and publish an implemented/unsupported compatibility matrix before
claiming conformance. POSIX is not a complete graphical terminal specification.

Distinguish formal standards from de facto behavior and optional extensions.
Evaluate negotiated kitty keyboard/graphics protocols, hyperlinks, bracketed
paste and shell status integration; support safe fallback and reject malformed
sequences with bounded parser work/storage. A shell that supplies no integration
must still work. Do not infer successful command completion from prompt text.
Provide SSH interoperability when the separately qualified native SSH stack exists.
No mandatory Tailscale dependency: native connections consume shared JeiOS
connectivity, not an app-specific identity or bespoke cryptography system.

## User experience and lifecycle

- Named workspaces, tabs/splits and searchable local/remote sessions; always show
  execution host and effective identity, including elevated contexts.
- Attach/detach/reconnect without killing the host process merely because a UI
  closes or a network link drops. Explicit, distinct detach, stop and terminate
  actions with explanations and confirmation appropriate to destructive impact.
- Monitor running/exited/disconnected states, exit status and authorized resource
  metrics; opt-in completion notifications. Unknown state is not success.
- Keyboard-first controls plus touch, screen-reader navigation, scalable text,
  contrast and reduced motion. Progressive settings do not change authority.
- GUI/CLI/API parity for permitted session operations, versioned configuration,
  documented errors and bounded event streams for automation and agents.

Define session identifiers with generation protection; states include starting,
running, exited and terminated, with attachment and network states tracked
separately. Specify creator/owner, process-group ownership, foreground control,
signal delivery, reaping and final-reference cleanup. Define bounded output queues,
scrollback, retention, backpressure, resize arbitration and slow-client handling.
Separate view-only observers from input controllers; default to one controller
with explicit transfer. Shared control is separately authorized and visible.

The host owns execution. Reattachment from another device does not migrate the
process. Persisted metadata/output does not make a process survive reboot. Host
failure, restartable jobs, checkpoint/restore and cross-ISA migration have distinct
contracts; no automatic replay of arbitrary interactive commands or keystrokes.
On reconnect, negotiate offsets/generations and disclose lost output. Duplicated
requests must not duplicate destructive actions. Define idempotency and expiry.

## Personal-to-enterprise distributed operation

Integrate with [dynamic clustering](CLUSTERING.md), [identity](IDENTITY.md),
[domains](DOMAINS.md), [zero trust](ZERO_TRUST.md), [encryption](ENCRYPTION.md)
and [organizational security](ORG_SECURITY.md). Those are planned dependencies,
not existing guarantees supplied by this proposal.

| Personal default | Enterprise extension |
| --- | --- |
| Explicit trusted-device pairing | Managed enrollment, tenant groups and delegated administrators |
| Own authorized sessions | Scoped team sessions, role/capability policy and approvals |
| Device/session picker | Filtered fleet/site/cluster directory with no unauthorized metadata disclosure |
| Explicit remote task launch | Cluster admission, placement, quotas, reservations and job accounting |
| Local status and notifications | Central health/events, policy-controlled audit export and alerting |
| Local operation without coordinator | Self-hosted/isolated deployments, HA management and bounded offline policy |

Session access and job submission are separate privileges. A terminal may submit
an explicit distributed workload through cluster APIs and display its status;
an arbitrary shell process does not automatically parallelize across nodes.
Keep management separate from execution: coordinator loss should not by itself
kill workloads, but expired authorization leases may require suspend/termination
under disclosed policy. HA management does not imply live-session host failover.
Bound discovery, fan-out, queues, retry rates, retained output and per-tenant
resource usage. Qualify scale with named workloads/node counts, latency, memory,
failure recovery and fairness measurements; do not promise unlimited scale.

## Security and failure contracts

Pairing or cluster membership never grants blanket terminal access. Check scoped
view, attach/input, launch, signal, terminate, share and admin authority server-side.
Authenticate peers; bind grants to identity, tenant, device, session and expiry.
Use reviewed encrypted transport and project PQ policy; define key rotation,
revocation, MFA/step-up and lock/logout/lost-device behavior. A remote grant is
not a serialized raw kernel handle. Management must not acquire users' decryption
keys merely to keep sessions alive. Separate personal/work trust domains.

Treat terminal output, titles, paths, hyperlinks and remote capability messages
as untrusted. Mediate clipboard reads/writes, file transfer, notifications, link
opening and terminal remote-control extensions. Never execute received escape
sequences as local shell commands. Bound parser inputs and GPU/image resources;
isolate rendering faults from session ownership where feasible.

Audit access and administrative operations with retention/redaction policy.
Terminal-content recording is a separate explicit policy because secrets may
appear in input/output; do not enable mandatory personal recording or telemetry.
Recording, export and deletion permissions must be scoped and visible. Specify
storage encryption and locking behavior before retaining sensitive scrollback.
Record support for organizational controls without claiming ISO certification.

## Staged implementation and acceptance

| Gate | Prerequisites and deliverable | Required evidence |
| --- | --- | --- |
| JT0 contract | Stabilization closed; reviewed PTY/process/job-control and capability designs aligned with SH2/M6 | Ownership/state/error tables; protocol/version/limits and threat model; no runtime claims |
| JT1 local sessions | Qualified processes, PTYs, signals and userspace services | Attach/detach, shell/job control, exit/reap, exhaustion, cancellation, stale IDs, resize and service-crash tests |
| JT2 native app | JT1 plus M7 UI/accessibility; optional M8 acceleration | Parser corpus/fuzzing, Unicode/input, compatibility matrix, shell/TUI regression, accessibility and software fallback |
| JT3 device access | JT1 plus qualified identity/network/encrypted transport; JT2 for GUI | Two-device reconnect, dropped/duplicated messages, revocation, expired grants, tenant isolation, controller transfer and SSH interoperability |
| JT4 enterprise/cluster | JT3 plus qualified cluster admission/policy/accounting | Multi-tenant negative tests, quotas/fairness, management failover/partitions, audit/privacy, isolated deployment and measured scale |

CLI clients may precede the GUI once JT1 is qualified. Each gate needs exact
revision/toolchain/configuration evidence, negative security tests and named
mutants where applicable; hosted, CI, emulator and hardware results stay separate.
Update implementation status/handoff and relevant trackers (#71–#73, #82) as work
lands. No dates or unsupported hardware guarantees. Mobile integration remains
separately qualified; this roadmap does not advance its core pin.

## Reference material

- Owner's Rex video reference: https://www.youtube.com/watch?v=pde1xL9D7ic
- Ghostty: https://ghostty.org/docs/features
- kitty extensions: https://sw.kovidgoyal.net/kitty/protocol-extensions/
- tmux: https://github.com/tmux/tmux/wiki
- Rex remote access: https://www.superlogical.com/rex/docs/linux/remote-access

References are architectural inspiration. Verify current upstream behavior and
licenses at implementation time; no source is imported by this plan.
