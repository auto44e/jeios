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
| GNU coreutils + tracked uutils coreutils | Native C command suite; GNU C implementation as initial base, uutils stable behavior/improvements tracked | M5/M6 descriptors/filesystem/process/runtime; per-command semantics, errors, locale/encoding and malicious-input tests; C adaptation policy below |
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

## Coreutils: C deliverable, two upstream reference tracks

The owner explicitly requests the source used to build our coreutils to remain C
while following both original GNU coreutils and its Rust rewrite, uutils. Start
with the GNU C source and its dependency/provenance review. Maintain separate
latest-stable version/digest records for GNU and uutils. Identify whether each
selected uutils behavior is standard-defined, GNU-compatible or an extension.

uutils is Rust; turning it into maintained C is a reimplementation/translation
project, not a compiler flag or an ordinary port. A C result must be labeled as a
JeiOS adaptation referencing an upstream version, never an unmodified upstream
uutils build. Do not assume translation preserves Rust safety properties.
Review memory/ownership/overflow/error semantics, fuzz parsers, compare defined
behavior and run failure/resource tests. Preserve licensing/provenance for derived
code; tests and documentation also require provenance review. Automated conversion
alone is not acceptance. Prefer one clear default command provider; alternative
implementations need explicit names/selection and cannot unpredictably shadow PATH.

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
4. Developer environment: Make and compiler components, separately proving target
   cross-build and native self-hosting. Broader tool dependencies are explicit.
5. Native QEMU: qualified host APIs and emulation before hardware acceleration.

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
