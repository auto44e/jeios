# Native shell and selectable shell ecosystem

Owner decision: 2026-11-01 (America/New_York). Accepted roadmap requirements;
no shell runtime is implemented by this documentation change. PowerShell is
explicitly deferred for separate discussion.

## Two phases, one original portable C engine

| Gate | Deliverable | Acceptance boundary |
| --- | --- | --- |
| SH1 / core #71 | Native recovery console and baseline, without external coreutils | Interactive serial prompt; help/status/services and authorized recovery operations; bounded parsing/input/memory; unsupported syntax errors; malformed input and denied-authority tests |
| SH2 / core #72, #15 | Original native JeiOS sh with POSIX scripting | Requirement matrix and native execution evidence for quoting, expansion, variables, command lookup, redirection, pipelines, lists, conditionals, loops/functions, substitution, exit status, signals/traps, wait and applicable terminal/job-control semantics |
| SH3 / core #73 | Included native integrated Bash and Zsh; user-selected shells | Native runtime and terminal tests, preserved upstream language behavior, safe selection/removal/upgrade and clean recovery fallback |
| Mobile #12 | Shared implementation qualified on mobile | ARM execution, suspend/disconnect/input/accessibility, work-profile separation and recovery evidence; physical devices separately qualified |

SH1 is the first configuration of the shared original engine, not a disposable
second shell. Use portable C and narrow OS adapters for console, streams, files,
processes, time and capability-based service access. Assembly/ISA details stay in
runtime/kernel backends. Define ownership, lifetime, caller context, locking,
bounds, errors and rollback before implementation. Run the shell in userspace,
not inside the kernel or with implicit supervisor authority.

The static recovery configuration must function without external utility binaries,
a dynamic linker, user startup files or plugins. Add ls/cat when M5a read-only
storage exists; writable utilities wait for qualified storage. Built-ins still
need actual OS services. Keep authentication/encrypted-data boundaries intact.
Test resource exhaustion, cancellation, terminal loss, malformed quoting and
service failure; no successful no-op commands or silent privilege escalation.

Use a distinct recovery command identity initially. Reserve /bin/sh for the
qualified native shell profile; never advertise the restricted prompt as POSIX.
Reject unsupported syntax before side effects. SH2 grows the same parser/executor
with explicit resource limits and disclosed semantics. Native extensions must be
namespaced/opt-in and cannot silently alter standard shell behavior. Our own sh
is the selected implementation direction; dash/sash/historical code are not a
substitute or an approved source import. Original code remains Apache-2.0.

Host parser/property/fuzz tests and comparisons against established shells help
find defects; the pinned POSIX specification is the oracle for defined behavior.
Do not treat unspecified/undefined behavior or unanimous shell behavior as a
conformance rule. Qualify actual process/descriptor/filesystem/terminal semantics
on each supported ISA/product. Share tests and code across desktop/mobile/server/
IoT; architecture-agnostic design is not automatic hardware qualification.

## Included Bash and Zsh: latest stable, reproducible integration

Bash and Zsh are included product goals, built natively for JeiOS with maintained
integration patches. They run on supported runtime/terminal interfaces, not inside
our console or in a Linux guest. Preserve upstream language and script behavior.
Provide native help, completions, sensible profiles, accessibility, consistent
paths and management commands over the same authorized service APIs.

At every integration/release preparation select the latest upstream stable release
and applicable stable maintenance patches from official upstream sources. Record
version, source URL, immutable digest/signature verification, provenance/license,
toolchain, dependency versions and ordered JeiOS patch series. Recheck stable
availability before release. Builds consume these pinned verified inputs rather
than resolving floating 'latest' over the network. If latest stable fails native
qualification, block the affected shell update and report the incompatibility;
do not silently label an older version latest. Previously installed known-good
artifacts remain available for explicit recovery/rollback with accurate versions.
No recurring update automation or upstream source import is implemented here.

Keep OS adaptation patches focused and reviewable; test upstream regressions,
script semantics, noninteractive startup and interactive PTY behavior. Record
expected skips as gaps. Review dependencies and retain upstream licenses/notices;
JeiOS policy does not relicense imported code. Current release numbers are resolved
at integration time, not permanently guessed in this roadmap.

## User choice and bring-your-own shell

Provide documented registration of an installed compatible shell: executable,
arguments, runtime/ISA/ABI requirements, terminal capabilities and configuration
locations. Validate availability, executable identity and permissions before
selection; never interpolate registration fields through another shell. No promise
that a Linux/Windows binary works unchanged. A source port or qualified runtime
may be required. User-provided shells receive only the session's scoped authority.

Offer GUI/CLI/API selection with equivalent policy and an accessible preview/test
session, revert action, clean uninstall and fallback if the chosen shell is
missing, crashes or has broken startup files. Keep personal/work policy distinct.
Do not let shell registration or prompt plugins gain administrator privileges.

Changing the interactive default must not change script shebangs, /bin/sh's
contract or service launch interpreter selection. Services use explicit launch
contracts, not arbitrary user profiles. Recovery remains available independently
of optional shells. System-wide installation/default changes require their normal
administrative authority; user-scoped preference needs no extra authority.

## Sequence and dependencies

MP1 and scoped M4 are complete. MP2 is the next execution gate; M5a/core #54
provides read-only image/loading/storage foundations for SH1. Coordinate #54 and
#71 as one console implementation. SH2 depends on the M5/M6 process/descriptor,
filesystem and terminal contracts, not merely a working lexer. SH3 follows the
required libc/runtime and terminal support; no full shell claim from cross-compile
alone. Plan designs/tests now without displacing required native qualification.

Official upstream references for future release verification:
- https://www.gnu.org/software/bash/
- https://www.zsh.org/
- https://pubs.opengroup.org/onlinepubs/9799919799/
