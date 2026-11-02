# Included native software portfolio

Owner decision: 2026-11-01 (America/New_York). Accepted inclusion targets for the
JeiOS ecosystem, not installed packages or qualified ports. Applies across named
desktop, mobile, server and IoT profiles; required runtime/hardware gates apply.
Tracks [core #75](https://github.com/auto44e/jeios/issues/75).
Extends [SHELLS.md](SHELLS.md). PowerShell remains deferred. No source imports,
new runtime languages, ABI changes or default network services are enabled here.

## Source and release policy

For every component, track official latest stable releases and stable maintenance
patches. At integration/update preparation verify upstream identity, record exact
release/tag/commit, immutable source digest and available signature verification,
license/provenance, dependencies, build tools, configuration and ordered native
patch series. Recheck latest stable before release. Build reproducibly from those
pinned sources; never use floating network 'latest' as a build input. Development
branches and nightly builds do not qualify as stable.

Use focused native adapters and documented service APIs; preserve upstream
behavior where promised. Test the upstream suite plus JeiOS negative, resource,
permissions, lifecycle and architecture tests. Track release lag and unresolved
failures explicitly. If newest stable cannot qualify, block that package update;
do not advertise an older version as latest. Preserve accurately versioned
known-good artifacts for rollback. Security fixes need prompt triage and a
reviewed port; source translation must not silently delay or erase that obligation.
This is release policy, not a configured scheduled updater.

Retain upstream licensing/notices and fulfill distribution obligations after
review; original JeiOS Apache-2.0 policy does not relicense imported or translated
code. Document which components are direct ports, modified forks or reimplementations.
No claim of upstream endorsement, exact equivalence or security certification.

## Inclusion inventory and gates

All rows are planned, with implementation owner, release pins and native evidence
unassigned until a bounded integration task is accepted.

| Product | Intended native role | Main prerequisites and acceptance |
| --- | --- | --- |
| doas | Default enabled elevation frontend after qualification; OpenBSD doas lineage, portable OpenDoas candidate | Critical-profile native identity/authorization/credential broker integration; deny by default, feature-parity and abuse tests below |
| sudo | Included advanced elevation option, disabled by default | Explicit administrator activation; sudoers, plugins, logging, session and editing feature matrix; no usable elevation authority while disabled |
| vi (nvi2 planned implementation) | Native ex/vi text editor | Latest stable nvi2 source, dependency/license review, curses/terminal support, locale, storage and safe recovery; qualify before exposing as vi |
| GNU Binutils | GNU as/ld, ar/ranlib, objcopy/objdump, readelf and related binary tools | Per-target object format, relocation, archive, linker-script, debug and malformed-input tests |
| NASM | x86/x86-64 NASM-syntax assembler | Native host port plus target-format/ABI tests; not an ARM or RISC-V assembler |
| CMake, Ninja, pkgconf | Native configure/build and dependency discovery tools alongside Make | Process/filesystem/runtime support; correct sysroot isolation, deterministic dependency resolution and failed/parallel build cleanup |
| GMP, MPFR, MPC | Required GCC support libraries | Stable compatible versions, arithmetic/ABI tests and dependency provenance; inventory optional isl/zstd when selected |
| ncurses | Terminal UI support for vi and qualified tools | Native termios/terminfo integration; hostile terminal/input, resize, locale and capability tests |
| GNU coreutils + tracked uutils coreutils | Native C command suite; stable GNU C implementation preferred; uutils improvements selected only with evidence | M5/M6 descriptors/filesystem/process/runtime; per-command semantics, errors, locale/encoding and malicious-input tests; C adaptation policy below |
| OpenSSH | Native client and optional administratively enabled server | Networking, identities, entropy, key storage, process/session/PTY support; host-key verification, auth denial, privilege separation and session cleanup; server off by default |
| LibreSSL (default) | Default cryptographic/TLS provider where compatible and qualified | Entropy/time/trust stores, secure memory/key ownership, platform acceleration/context; protocol/certificate and failure tests |
| OpenSSL | Included alternative for consumers requiring its APIs/features | Separate versioned dependency selection and qualification; never assume LibreSSL ABI/API equivalence or silently swap providers |
| llvm-project | Native compiler/toolchain ecosystem, staged components | Target ABI/object/linker/runtime work, host build prerequisites and native compiler execution; inventory Clang/LLVM/linker/runtime subprojects separately |
| GCC | Native compiler toolchain | Bootstrap toolchain, target description, assembler/linker/libc and selected runtimes; cross-compile first, native execution and bootstrap comparisons later |
| GNU Make | Native build execution | Shell/process/wait/filesystem/timestamps; job control/parallel-job cleanup and build failure propagation; distinguish make from a compiler |
| curl / libcurl | CLI transfer tool and programmatic transfer APIs | Networking/DNS/TLS and per-protocol dependencies; certificate verification, cancellation/timeouts, bounds and proxy/redirect policy tests |
| libfido2 | FIDO authenticator integration behind native credential broker | CTAP transports/device access, consent/PIN/UV and cryptographic dependencies; device removal, cancellation, malicious responses and user/profile isolation |
| zlib | Compression library | libc/allocator/stream adapters; corrupted/truncated input, overflow, allocation failures and decompression resource budgets |
| QEMU | Emulator/virtualization tools running on JeiOS | Native host threads/memory/timers/signals/I/O and dependency ports; initially bounded software emulation, then independently qualified acceleration |

Bash and Zsh remain included native shells under SHELLS.md. Native means executing
on qualified JeiOS runtime interfaces, not wrapping a Linux guest. Current use of
QEMU on a development host to boot JeiOS is not a native QEMU port. A GCC/LLVM
cross-toolchain targeting JeiOS is not evidence that it runs inside JeiOS.

LibreSSL is the default provider policy, not a universal binary replacement for
OpenSSL. Pin a provider per consumer/build; avoid symbol/library collisions,
implicit downgrade and dependency confusion. If a consumer needs OpenSSL, declare
and test that dependency visibly. Do not claim validated crypto modules merely
because a provider offers them elsewhere. Cryptographic primitives should not be
rewritten simply for branding or default-provider uniformity.

## Shared hardening and feature-preservation gate

Owner extension: 2026-11-02 (America/New_York). Every included source-built
product, including shells, libraries, compilers and optional components, must be
assessed against [CODING_STANDARD.md](CODING_STANDARD.md) and the native trust
boundaries. Apply necessary feasible source hardening with focused, reviewable
patches, a stated threat/failure case and regression evidence. Prefer fixes that
can be maintained with or contributed upstream; do not gratuitously rewrite code.

Before qualification, record the promised feature/configuration matrix and test
it before and after native/hardening changes. Preserve the intended feature set;
disabling a risky feature by default is distinct from deleting it. If safe native
semantics cannot be provided, mark that feature or product blocked and disclose
the gap rather than silently removing behavior or weakening a security boundary.
A feasibility exception needs a recorded risk, compensating control, owner and
review gate; it cannot waive mandatory isolation or authorization requirements.

Required evidence is risk-based: upstream regression suites; native positive and
negative integration tests; static analysis; sanitized test builds; parser and
protocol fuzzing with retained regression inputs; allocation/I/O failures,
resource limits, races, cancellation, teardown, permissions and rollback tests.
Validate applicable production hardening (for example stack protection, W^X,
PIE/ASLR, RELRO and control-flow defenses) against actual compiler/loader/ISA
support. Report unsupported mitigations rather than claiming flags enforce them.
Sanitizer/fuzzer builds belong in isolated test environments, never installed as
privileged production executables. Require vulnerability/dependency triage,
patch-rebase review on each release, reproducible source/patch/configuration
records and the independent review required for critical release qualification.
Passing tests or smaller code size alone does not establish security.

## doas and sudo: defaults, authority and qualification

doas is the preferred enabled frontend because its narrower design offers a
smaller review scope; smaller size is not proof of fewer exploitable defects.
Select and pin a maintained stable source lineage explicitly: OpenBSD release
source and portable OpenDoas are distinct upstreams, not interchangeable release
numbers. Compare portable changes and upstream security fixes before selection.
Both doas and sudo follow the latest-stable policy and receive native source
adaptation and extensive hardening while retaining their promised feature sets.

Enabled doas means available to administrators under explicit policy, never an
unconditional privilege grant. No broad passwordless grants ship by default.
Sudo is included but disabled: no active elevation grant, privileged launch path
or automatically loaded privileged plugins. Activation requires an authorized,
audited configuration transaction; validate policy before granting authority.
Disabling must revoke new elevation and cached authorization, with explicit
handling of already-running sessions. Test enable/disable, update and rollback
without silently re-enabling sudo. Retain its advanced functionality for approved
use; inventory sudoers, visudo, sudoedit, policy/I/O plugins, remote logging and
session controls against the selected upstream stable version.

Both frontends must use native identity and scoped authorization contracts;
legacy root/setuid assumptions cannot bypass capabilities or mandatory policy.
Do not implement elevation inside the shell or invent a second credential store.
Define caller/target identity, requested command/arguments, environment, cwd,
descriptor inheritance, executable resolution and granted authority at the broker
boundary. Fail closed on broker, authentication, policy or required-audit failure.
Do not assume OpenBSD pledge/unveil or a host PAM module exists on JeiOS; provide
reviewed native equivalents for the required guarantees or block qualification.

Critical acceptance includes allow/deny and rule-precedence tests; malformed,
unowned or writable policy/config rejection; UID/group/role mapping; revoked and
locked identities; FIDO/MFA and cancellation where configured; environment/PATH
and dynamic-loader injection; symlink and executable/config replacement races;
TTY/session binding; timestamp reuse/expiry, logout and reboot; descriptor and
capability leaks; plugin loading and sudoedit temporary-file attacks; concurrent
requests, resource exhaustion and crash cleanup. Exercise authorized advanced
features as well as rejection paths. Logs must support audit without leaking
passwords, keys or sensitive terminal input. Default caching and optional
passwordless behavior require explicit scoped policy and separate evidence.

## Complete native C/C++ and assembly developer environment

The planned out-of-box development environment includes GCC and Clang, GNU
Binutils, LLVM's integrated assembler/llvm-mc and LLD, NASM, GNU Make, CMake,
Ninja and pkgconf. Inventory compiler-rt/libgcc, libc++/libc++abi/libunwind and
libstdc++ support as appropriate; provide native libc, headers, startup objects,
sysroot, target triples and dynamic/static linker contracts. These are required
integration deliverables, not evidence an existing compiler runs on JeiOS. Select
runtime families explicitly per toolchain; do not mix incompatible C++ ABIs.
Build-time generators/interpreters and optional dependencies remain separately
inventoried under the existing language policy.

| Assembly family | Planned provider and evidence |
| --- | --- |
| AMD64/x86 GNU syntax (AT&T and supported Intel forms) | GNU as and LLVM assembler; assemble/link/run ABI and relocation fixtures |
| AMD64/x86 NASM syntax | NASM; qualify supported output formats and linkage with native C/C++ |
| ARM64 and RISC-V GNU-style syntax | GNU as and LLVM where the selected stable target supports it; per-ISA instruction/relocation tests |
| Other CPU targets and vendor dialects | Extend the architecture matrix with named providers and tests; do not imply MASM/TASM or every dialect is interchangeable |
| GPU/NPU/device assembly | Target/vendor-specific SDK and driver qualification under ARCHITECTURE_SUPPORT; CPU assemblers do not provide universal accelerator support |

“All common assembly” is a coverage goal, not a claim that one syntax or assembler
supports every machine. Track gaps explicitly; add further assemblers only with a
concrete dialect need and reviewed stable source. Cross-target code generation
must be distinguished from native host execution and target runtime support.
Acceptance: from a fresh qualified development profile, compile/link/run C and
C++ programs, exercise exceptions/unwinding/threads as promised, link assembly
with C/C++, create libraries, and build Make/CMake/Ninja projects without fetching
undeclared tools. Cover each supported ISA, reproducibility and malformed binary
inputs; obey baseline ISA policy. Resource-constrained profiles must disclose
any development-package availability limits rather than claim identical installs.

vi is an editor interface, so name its implementation explicitly: plan nvi2's
latest stable source as the ex/vi provider, subject to dependency/provenance review.
Test normal/ex editing, save/permissions, multibyte text, terminal resizing,
interrupted writes, disk-full and crash/recovery paths. Protect recovery files;
untrusted files/local configuration must not silently run commands. Keep shell
escapes as an intentional user capability under normal process authority. Editing
privileged files needs reviewed elevation workflows; vi itself is not setuid.
Recovery-console availability is separately gated and must not make SH1 depend
on a full-screen editor, ncurses or writable storage.

## Coreutils: stable GNU C baseline, selective uutils improvements

GNU coreutils' latest stable C release is the preferred source and default native
command provider. Keep the built coreutils source in C. Review uutils stable
release changes only as a source of selectively adopted improvements; it is not
a parallel shipped implementation or a full C rewrite target.

Adopt a uutils change only when comparison with the current stable GNU baseline
shows a genuine correctness, security, performance or portability improvement.
Record the upstream release/change, reproducible before/after evidence, relevant
workloads, compatibility impact and maintenance cost in the PR. Skip changes with
no demonstrated benefit or whose benefit GNU already supplies. Distinguish
standard-defined behavior, GNU compatibility and optional extensions.

Here, cherry-picking means selecting an improvement and adapting or implementing
it in C, not directly applying Rust source to the C build. Preserve provenance
and applicable licensing for derived code, tests and documentation. Label the
result as a JeiOS adaptation. Review memory, ownership, overflow and error
semantics; use relevant regression, failure and performance tests. Translation
does not preserve Rust safety guarantees automatically. Pin the GNU release and
record exact uutils versions/changes for reviews and accepted adaptations.

Owner clarification: existing C++ is allowed for included upstream products,
including LLVM and GCC. Preserve their established implementation languages where
C/C++ and adjust source for native JeiOS compatibility/integration; this does not
change the original C11 kernel policy or coreutils' explicit C-only deliverable.
Inventory non-C/C++ runtime components (including selected QEMU Rust components)
and build-time tools separately; do not infer permission for every other language
or silently omit required functionality. Resolve those components during the
bounded dependency review. No portfolio-wide C rewrite is required.

## FIDO and native integration

libfido2 is a planned dependency of [SECURITY_KEYS.md](SECURITY_KEYS.md), not the
entire MFA solution. The broker still owns consent, credential scope, authorization,
revocation and protected key/PIN handling. Hardware transport drivers and account/
login/browser integrations remain separate gates. FIDO support does not imply
PIV, OpenPGP, arbitrary OTP, every authenticator or automatic domain authentication.
Keep device permissions narrow; test unplug, timeout, malformed data, cancellation
and cross-user/work-profile isolation. Never place credentials in diagnostics.

## Delivery and evidence

1. Inventory/provenance: dependency/language/license matrix, latest-stable pins,
   native API gaps and accountable owners. No imports until review.
2. Foundation: bounded zlib/coreutils command subsets as libc/storage permit;
   coordinate SH1 without making recovery depend on external coreutils.
3. Network/security: qualify TLS providers, curl, SSH and libfido2 against real
   networking, identity, device and entropy services. No placeholder success.
4. Native identity gate: doas and optional sudo require the critical authorization,
   feature-parity and lifecycle evidence above before activation.
5. Developer environment and vi: complete compiler/assembler/runtime/build-tool
   dependencies, separately proving cross-build and native execution.
6. Native QEMU: qualified host APIs and emulation before hardware acceleration.

These tracks do not displace MP2, M5a and native shell gates. Each release records
component version, source and patch hashes, build/profile/ISA, test commands/logs,
expected versus observed behavior, unsupported features and rollback results.
No estimate of whole-ecosystem completion or blanket hardware support is implied.

## Primary upstream references

Resolve release versions when preparing an implementation; none is imported here.
- https://www.gnu.org/software/coreutils/
- https://uutils.org/coreutils/
- https://www.openssh.com/portable.html
- https://www.libressl.org/
- https://www.openssl.org/
- https://llvm.org/docs/GettingStarted.html
- https://gcc.gnu.org/install/prerequisites.html
- https://www.gnu.org/software/make/
- https://curl.se/
- https://developers.yubico.com/libfido2/
- https://zlib.net/
- https://www.qemu.org/docs/master/about/build-platforms.html

- https://man.openbsd.org/doas
- https://man.openbsd.org/doas.conf
- https://github.com/Duncaen/OpenDoas
- https://www.sudo.ws/
- https://github.com/sudo-project/sudo/blob/main/INSTALL.md
- https://github.com/lichray/nvi2
- https://www.gnu.org/software/binutils/
- https://clang.llvm.org/docs/Toolchain.html
- https://www.nasm.us/
- https://cmake.org/
- https://ninja-build.org/
- https://github.com/pkgconf/pkgconf
- https://invisible-island.net/ncurses/
