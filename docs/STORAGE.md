# Filesystem recommendation and storage contracts

Status: recommendation and future implementation plan, 2026-10-13. No VFS,
block driver or filesystem exists in JeiOS today. The long-term policy is multiple profile-selected defaults (owner decision
2026-10-14, detailed below). Exact production profiles remain subject to evidence,
dependency license review and porting feasibility.

## Initial bring-up default

Build a filesystem-independent VFS. Start M5 with a bounded read-only init image
and RAM filesystem, then qualify an explicitly limited ext4 format profile as
the owner-approved provisional OS filesystem default. Add read-only support
before writes; this placeholder is not yet implemented or production-qualified. A mature
on-disk format provides documentation and external recovery tools; it does not
make a fresh implementation mature. Do not mount valuable existing disks writable
with an experimental driver. Use disposable images until durability gates pass.

For the eventual desktop, Btrfs-like functionality is a good target: checksummed
data/metadata, snapshots, reflinks, compression and efficient incremental backup.
Evaluate an actual Btrfs port against a continued ext4 backend plus OS-level A/B
updates. Do not start a new production filesystem merely to give it a JeiOS name.
OpenZFS is a strong later workstation/server candidate, conditional on integration,
resource measurements and license compatibility. Keep mobile/IoT storage choices
separate when media behavior or resource budgets justify it.

| Candidate | Value for JeiOS | Cost / boundary | Proposed role |
| --- | --- | --- | --- |
| ext4 | Documented format, journal, mature external tooling | New implementation still needs journal replay, feature negotiation and crash testing; not general data checksumming/snapshots | First persistent candidate, strict feature profile |
| Btrfs | CoW, data/metadata checksums, snapshots, compression, send/receive | Substantial Linux integration to replace; supported feature combinations and encryption layer require qualification | Later desktop evaluation |
| OpenZFS | Integrated integrity, datasets, snapshots, compression, native encryption | Large storage stack; portability/resource work and CDDL review | Later workstation/server evaluation |
| FAT32 | Firmware/removable interoperability | Lacks the desired security/durability semantics | Future EFI system partition and exchange only |
| New native format | Complete freedom over semantics | Recovery tools, compatibility and crash correctness all become our responsibility | Defer unless measured requirements defeat existing formats |

This recommendation does not authorize importing any implementation. The project
uses Apache-2.0 for original code. Linux implementation licensing and OpenZFS's CDDL must
be reviewed separately from reading a public format specification. Record source
provenance and selected component licenses before any port.

## M5 subdivision and compatibility rules

1. M5a: checked init image and RAM filesystem, scoped file handles, namespace/path
   rules and parser fuzzing. Init image is trusted only to the degree boot is
   verified; read-only by itself does not authenticate content.
2. M5b: bounded block I/O and read-only ext4 candidate. Pin format revision,
   block sizes, extent/inode bounds and accepted feature flags in an ADR. Validate
   lengths, overflow, checksums and traversal; bound recursion and allocation.
   For the first reader reject any unqualified feature flag, including unknown
   compatible flags. Reject dirty journals requiring replay; never call an
   unreplayed image consistent. No write support implied by successful reads.
3. M5c: qualified writable backend, journal/recovery and interrupted-commit tests.
   Narrow scope or reconsider backend if correct journal semantics cannot be
   delivered. Recovery tooling and independent Linux interoperability are gates.
4. M9: encrypted/authenticated storage and signed A/B boot policy qualification.
   Reserve format/version/key-management requirements before M5 formats stabilize.

Filesystem feature support is independent of CPU age. Avoid mandatory modern CPU
instructions; provide verified software crypto/checksum paths when permitted.
Cap caches explicitly on low-memory hardware. Hardware encryption accelerators
improve performance but do not substitute for correct cryptographic policy.

## VFS authority and lifecycle

A file handle binds object identity, generation, rights and mount instance; a
pathname alone is not authority. Resolve relative paths beneath an authorized
directory with explicit symlink and mount-crossing policy. Define case sensitivity,
Unicode treatment, hard links, rename, open-after-unlink and executable mappings
before an ABI freeze. Preserve filenames without silently normalizing collisions.
Foreign permissions map conservatively to JeiOS capabilities; an on-disk root UID
must not grant host privilege. Removable media is untrusted input.

The filesystem owns cached pages and metadata references. Async I/O pins only
bounded buffers until completion/cancellation is acknowledged. Unmount either
drains references or reports busy; device removal invalidates its generation and
returns I/O errors rather than redirecting stale requests to a replacement disk.
Document lock order and avoid service IPC while holding filesystem metadata locks.
An isolated filesystem service is preferred for parser fault containment, subject
to M4 buffer-grant support and measured costs; it does not make buggy writes safe.

## Durability, updates and recovery

Separate OS A/B images from user data, logs and mutable configuration. Authorize
boot selection independently of ordinary filesystem access. Update health checks
commit a boot attempt only after required services become ready; failure returns
to an authorized compatible image. Do not roll user data back automatically with
the OS. Schema migrations need backward compatibility or a documented recovery
copy; external side effects cannot be undone by a snapshot.

Define write completion versus durable completion, file/directory fsync, rename,
barriers/flush and device cache assumptions. Simulate power loss at every journal
and update transition, torn/reordered writes, ENOSPC, media errors and corrupt
metadata. Never acknowledge durability that the device has not provided. On
corruption, preserve evidence and enter an explicit read-only/failed state; no
silent repair that discards data. Checksums can detect corruption; repair requires
an independent valid copy. Snapshots share failure domains and are not backups.

Encrypt at rest according to ENCRYPTION.md. Checksums are not adversarial integrity;
encryption is not replay prevention. Whole-volume and per-user key domains have
different unlock/metadata-leakage tradeoffs and need an ADR. Compression precedes
encryption when used, with bounded decompression. Disable cross-user deduplication
by default. Define discard/TRIM leakage, snapshots retaining deleted content,
key erasure and backups before promising secure deletion, particularly on SSDs.

## Evidence required before qualifying production defaults

Compare low-memory idle/cache behavior, recovery time, boot/update reliability,
small-file latency, sustained writes, SSD write amplification and storage failures.
Run malformed-image fuzzing and independent format/tool round trips. Qualify
single-device operation before multi-device RAID. Publish the exact accepted
features and versions; unsupported formats fail closed without modifying media.
Make a new ADR with measured results before promoting any candidate to default.

## Primary references

- [ext4 format specification](https://www.kernel.org/doc/html/latest/filesystems/ext4/).
- [Btrfs capabilities](https://btrfs.readthedocs.io/en/stable/Introduction.html).
- [Btrfs snapshots and their limits](https://btrfs.readthedocs.io/en/latest/Subvolumes.html).
- [OpenZFS overview and licensing](https://github.com/openzfs/zfs/blob/master/README.md).
- [OpenZFS encryption](https://openzfs.github.io/openzfs-docs/man/v2.2/8/zfs-unload-key.8.html).

Reviewed 2026-10-13 for planning. Pin exact specifications and current support
status at implementation time; no foreign implementation was copied.

## Raw block access without formatting

Raw I/O is a first-class planned block-device API, independent of filesystems or
partition tables. A whole device, partition or authorized extent can expose
bounded reads/writes without formatting it. An application can manage its own
record format, disk image or database layout. Raw bytes have no inherent filenames,
directories, permissions or crash-recovery semantics; the application owns those.
No request to inspect or open unrecognized media may initialize or format it.

Separate raw-read, raw-write, flush, discard, partition-change and format rights.
Handles bind device identity and attachment generation plus allowed byte ranges.
Check offset/length overflow, logical/physical sector alignment, maximum transfer
size, read-only state and range on every request. Report partial completion and
uncertain writes explicitly. Buffer grants pin only authorized memory until the
hardware is quiescent; a caller timeout does not mean a write was cancelled.
Document cache/flush semantics and never claim unsupported atomic sector writes.

Default to read-only inspection. Require exclusive ownership for raw writes to
ranges otherwise owned by a mounted filesystem, volume/encryption layer or other
writer. Resolve aliases (whole disk versus partitions, mappings and loop devices)
before granting the lease. Freeze/drain/unmount through the relevant owner; deny
when ownership cannot be proven. Do not let raw access bypass a locked volume,
user namespace or tenant boundary. Filesystem rights never imply raw-device rights.
Device removal invalidates handles; replugging a similar disk cannot revive them.
Destructive operations require separately authorized rights and a concrete scope,
including explicit boot/system-volume handling. Never retry an ambiguous write to
a newly attached device just because its path was reused.

## File-like views without changing the source format

Offer distinct modes instead of an ambiguous 'formatless filesystem':

| Mode | Metadata location | Source modification / limit |
| --- | --- | --- |
| Existing-format adapter | Existing on-disk filesystem | Mount qualified format without reformatting; ordinary writes still change its metadata |
| Temporary virtual view | RAM | Present an image or selected ranges as virtual files; names/mappings disappear on restart unless exported |
| Persistent sidecar view | Separate already-initialized storage | Store names/ranges and copy-on-write edits elsewhere; original disk remains read-only |
| Explicit application object store | Allocated source region | Writes headers/index/allocation metadata; this is initialization even without a conventional filesystem or whole-disk format |

A persistent filesystem needs metadata somewhere. Arbitrary existing disk contents
cannot safely acquire persistent files with zero metadata and no space reservation.
Do not assume unpartitioned or zero-looking sectors are free. A sidecar view binds
the exact source identity/version and range layout; source changes invalidate the
view until checked. Encrypt/authenticate sensitive sidecar metadata under the same
access policy, bound its growth, and fail cleanly when its storage is unavailable.

A copy-on-write overlay stores modifications in the sidecar without touching the
base. Committing an overlay back to raw media is a separate destructive operation,
requires an unchanged source and exclusive access, and may not be atomically
reversible. Recovery/file-carving views are best-effort reconstructions, not a
writable replacement for an unknown format. No silent 'repair' on mount.

## Broad filesystem interoperability backlog

Owner goal: read and write common filesystems with open-source implementations.
This is a long-term compatibility program, not a present 'all filesystems' claim.
Open-source implementation availability, public format documentation and import
license compatibility are different questions. The backlog below defines scope
for investigation, not current support, ranking, or guaranteed writable coverage.

| Family | Intended evaluation scope | Separate qualification concern |
| --- | --- | --- |
| ext2/ext3/ext4 | Linux disk interchange and provisional ext4 system default | Exact feature flags, journal replay and supported write combinations |
| Btrfs, XFS | Desktop/workstation and Linux storage interchange | Metadata/recovery features, reflinks and optional multi-device behavior |
| OpenZFS | Dataset/pool interoperability and server workloads | Pool feature flags, import policy, encryption and license review |
| F2FS | Managed-flash mobile/IoT storage | Device behavior, checkpoints and write amplification |
| FAT variants, exFAT | Removable media and firmware interoperability | Permission translation, name rules, dirty state and licensing provenance |
| UFS families | BSD media interchange | Explicit variant/endian support and write recovery |
| ISO 9660, UDF, SquashFS, EROFS | Optical/install/read-only system images | Some formats are intrinsically read-only at runtime; create/rebuild an image instead of claiming in-place writes |
| NTFS via a qualified open-source implementation | Common cross-platform removable/internal media | This is a proprietary-origin format; independent implementation licensing and exact write features need review |
| NFS, SMB | Optional network filesystem clients | Separate network authentication, server trust and credential policy; not local disk formats |
| Userspace filesystem adapters | Extensible experimental/third-party formats | Versioned native ABI; FUSE compatibility requires its own implementation and tests |

Publish a matrix per format/version with states: planned, probe-only, read-only,
qualified read/write, or unsupported. Record journal/recovery, encrypted-volume,
case/Unicode, links, sparse files, ACL/xattr and size limits separately. Never
advertise read/write just because a parser lists files. Each write implementation
needs independent-tool round trips, power-loss/media-error tests and a recovery
path. Unknown/incompatible media stays unchanged with an actionable explanation.

M5 adds block access, leases, RAM views and strict ext4 support in bounded changes.
Persistent overlays and additional formats follow their own reviewable gates;
network mounts depend on M6 and identity controls. Before release, choose and
publish a finite supported subset. Broad compatibility does not justify delaying
process isolation or importing unreviewed foreign kernel code.


## Owner decision: protected defaults and independent version management

Decision: 2026-10-14T20:30:05-04:00. Implement protected, versioned system
deployments as the seamless default for ordinary users. Native JeiOS apps and
components must integrate naturally with this model. Keep it fully configurable,
including an authorized owner-controlled off switch for system immutability.
Provide intuitive forward updates and rollback for the OS and built-in apps
independently of whether system immutability is enabled.

This is an accepted product requirement and future implementation direction,
not implemented storage/update behavior. Existing M3a and mobile MP0 sequencing
is unchanged; design deployment/data contracts before M5 formats freeze and
qualify trusted update/recovery at M9. Provisional ext4 remains compatible with
the direction; snapshots or a particular filesystem are not mandatory.

### Separate mechanisms and controls

Treat these as independent concepts:
- System write protection: whether active OS-managed files can be modified in place.
- Version/deployment management: retaining, preparing and selecting coherent versions.
- Trust policy: which publishers, owner builds and system modifications are authorized.
- Data protection: encryption, relock domains and mutable-data migration/backup rules.

Turning off system write protection does not disable version history, the update
manager, authorization, encryption or recovery. Nor does it automatically disable
verified boot or allow unsigned code: settings can interact and those dependencies
must be explained. If direct changes invalidate a verified image, report its
changed trust state and offer a qualified owner-build/reseal/development path.
Managed devices may enforce disclosed policy; an unauthorized user cannot bypass
that policy via the off switch.

The proposed UX is a simple protected-system control plus an independent version
history with actions such as update, return to previous version and reapply a
retained compatible version. Exact wording/interaction is not frozen. Everyday
users should not need to understand images, overlays, package graphs or partitions.
Expert GUI/CLI/APIs expose manifests, diffs, dependencies, retention, trust state
and custom compositions with the same permission checks.

### Native OS and app update contracts

Maintain a version manifest covering OS components and built-in apps, their
dependencies/ABI ranges, provenance and data-schema requirements. Applications
should update independently when qualified compatibility permits; components
requiring a coordinated OS/app set move together. Built-in status does not imply
blanket privileges. Avoid forcing a whole OS reboot for an app-only update where
safe replacement is supported; show when an actual restart/reboot is required.

Use a shared native lifecycle contract for update preparation, bounded quiescence,
protected state saving, resource release and restoration. Reuse crypto-relock
primitives where appropriate, but update and relock are distinct events with
different key availability and state transitions. Apps must survive termination
without a final callback and validate versioned saved state on restart.

Stage and validate an update before activation. Require a crash-safe activation
record and dependency-consistent deployment selection. Health checks determine
whether to retain the new deployment or return to an authorized compatible one.
Keep a recovery path independent of a broken desktop, app manager or writable
development deployment. Define bounded retries, ENOSPC behavior, incomplete
downloads and power-loss recovery; never delete the last recovery candidate
merely to free space for an update.

### Mutable mode with version history

When write protection is off, allow authorized direct modifications. Preserve
managed version manifests and recoverable prior deployments independently.
Before replacing a modified deployment, identify its divergence and offer an
explicit preserve/checkpoint, rebase/merge, discard or abort path as applicable.
Do not silently overwrite local changes or represent an altered deployment as
byte-identical to its original release. Never blindly replay old modifications
over new privileged binaries.

Native managed updates should retain transactional behavior in mutable mode.
Arbitrary out-of-band writes cannot automatically be promised atomic or recoverable.
Specify what is checkpointed, when, and which writes bypass tracking. A complete
recoverable system checkpoint must capture a coherent state and account for
concurrent writers; an arbitrary folder copy is not sufficient. Atomic deployment
switching may require staging another generation even if the current one is writable.

Reenabling protection validates/recomposes the chosen system version and reports
remaining trust limitations. Making modified files read-only does not restore
their integrity or undo prior compromise. Keep the off switch practical and
documented without claiming protections that have been disabled.

### Forward/back navigation and data compatibility

The user-facing history should offer a simple way to move between retained,
compatible versions of the OS and built-in apps, in either protection mode.
Prepare a reversible plan where possible and explain any migration, restart,
missing dependency, blocked version or data consequence before activation.
Retained releases remain subject to owner/organization trust and anti-rollback
policy: do not silently boot a revoked vulnerable version to satisfy a rollback.

Executable rollback is separate from user-data rollback. Native apps must declare
schema/checkpoint compatibility and migration behavior. Prefer backward-compatible
changes; where needed retain protected recovery copies or use a separately reviewed
transactional migration. Non-reversible migrations require an explicit data recovery
choice or a blocked downgrade, not a falsely successful rollback. Documents created
since the older OS version must not disappear merely because OS code was reverted.

Forward navigation after rollback is conditional on validated manifests and data
compatibility, not guaranteed replay of an old session. Never replay external
messages, purchases or remote actions. Retention/garbage collection must preserve
referenced recovery data and respect storage quotas, encryption domains and key
lifecycle. History is not a substitute for independent backups; retained copies
may retain deleted secrets and must be reflected in deletion/privacy controls.

### Acceptance and implementation gates

At M5 specify version manifests, configuration/data separation, immutable versus
mutable ownership and crash-safe deployment selection. At M7 qualify accessible
everyday history/recovery UX and advanced controls. At M9 qualify signatures,
owner trust roots, anti-rollback policy and recovery. Shared native SDK and built-in
apps must pass lifecycle/migration tests; mobile qualifies its own boot/storage
constraints rather than inheriting desktop guarantees.

Test protected and mutable modes, owner-built deployments, direct modification
conflicts, switching protection on/off, independent and coordinated app updates,
old/new schemas, crypto-relock checkpoints, low disk space, corrupt manifests,
power loss at activation/migration, failed health checks, credential/key changes
and forward-after-rollback. Verify user files survive OS rollback and unauthorized
apps cannot disable protection or choose a different trusted boot version.
No runtime implementation or completed gate is implied by this decision.

## Owner decision: multiple first-class default backends and owner replacements

Decision: 2026-10-14T20:40:22-04:00. The long-term core storage offering should
include Btrfs, OpenZFS and an appropriate mobile filesystem as first-class default
choices, with a seamless native JeiOS experience above them. Owners may substitute
their own filesystem as the system default within documented technical and policy
limits. This supersedes the earlier framing of selecting one universal production
default; it does not certify any backend or authorize unreviewed source imports.

Proposed profile mapping: Btrfs for general desktops/laptops; OpenZFS for
storage-focused workstation/server profiles; F2FS as the mobile flash candidate.
These mappings and F2FS selection require measurements and qualification, rather
than restricting advanced owners to a device category. ext4 remains the initial
bounded bring-up target and a possible supported alternative. No filesystem is
implemented yet; do not expand the next gate beyond M3a to deliver these in parallel.

### Common experience and capability contracts

Core default means a maintained, integrated and tested choice for named profiles,
not all filesystems simultaneously managing the same block ranges. Default
installation chooses a qualified profile without asking ordinary users to compare
filesystem internals. Advanced installation and administration expose supported
choices, constraints and tradeoffs. Multiple backend volumes may coexist with
separate ownership, mounts and resource accounting.

Native applications use JeiOS file/storage, identity, encryption-domain, lifecycle
and version-management APIs. Keep snapshots, datasets, subvolumes and backend
commands out of ordinary workflows. Native namespaces and permissions must retain
their intended semantics across backends; unsupported operations return explicit
capability errors. Publish a backend/version/feature matrix and negotiate actual
capabilities rather than pretending every backend has identical native features.

The OS deployment manager supplies version history and recovery even without
native filesystem snapshots, using qualified A/B or versioned-image mechanisms.
Per-user cryptographic relock requires separately qualified encryption/key-domain
and cache-revocation integration; a common API is not evidence of equal protection.
Filesystem encryption, external encryption layers and OS protection domains need
explicit backend-specific implementations and threat models.

Use baseline contracts for durability, atomic namespace operations, ownership,
bounds, quotas/accounting, locking, cancellation, key availability and failure.
Map optional checksums, compression, snapshots, cloning and replication separately.
Equivalent UX may use different implementations; do not silently simulate guarantees
such as durable commits, authentication or instant atomic snapshots.

### Owner-provided system-default filesystem

Provide a documented, versioned backend interface and conformance tooling.
Distinguish an ordinary data-volume adapter from a system/root-capable backend.
System-default qualification additionally requires early boot/init integration,
update staging/activation, recovery tools, crash consistency, locked-volume behavior,
and a way to recover when the backend cannot mount. Drivers belong behind isolated
service boundaries where feasible; early trusted components need explicit review.
A plugin interface does not grant arbitrary kernel privilege or bypass boot policy.

Allow owners to build, register, select and package a custom backend and recovery
implementation. Offer an explicitly experimental profile for unqualified owner
builds with clear lost guarantees; do not advertise production support or a hardened
profile when required invariants are absent. Managed systems may restrict selection
under disclosed policy. A safe recovery environment should not depend solely on
the experimental filesystem it is meant to repair.

Changing the configured default generally affects new installs/volumes/deployments.
Existing on-disk data does not change format by flipping a setting. Migration needs
a separately designed copy/export/import or specifically qualified conversion path,
sufficient storage, verification, backup/recovery and explicit destructive-write
authorization. Never silently reformat media, assume universal in-place conversion,
or let two backends concurrently own the same writable ranges.

### Qualification and maintenance

Qualify each backend independently for read-only, read/write, system boot,
protected/mutable deployments, version navigation, crypto relock, quotas and
multi-user boundaries. Test power loss, ENOSPC, corruption, concurrent operations,
device loss, memory budgets, sustained latency, flash write amplification,
checkpoint compatibility and independent-tool recovery. Keep exact feature flags
and versions pinned; a successful desktop port does not qualify mobile.

Maintaining several defaults multiplies integration and recovery work. Stage
delivery rather than making every backend a prerequisite to the first usable
system. Upstream formats and implementations retain their own licenses and
provenance requirements. Any custom or imported backend must have an explicit
maintenance owner and supported-profile declaration before production promotion.

## F2FS approval and agent tasks — 2026-10-14T20:43:47-04:00

Owner approved F2FS as the provisional mobile default, subject to actual JeiOS
qualification. This supersedes earlier candidate-only wording; device support,
feature flags and production readiness are not implied. Btrfs/OpenZFS remain
long-term default targets; ext4 remains the initial core bring-up backend.

Ready-to-start bounded work:
- [Core #13](https://github.com/auto44e/jeios/issues/13): backend contract ADR,
  hosted fake-block-device and conformance harness.
- [Core #14](https://github.com/auto44e/jeios/issues/14): F2FS format/provenance
  review and bounded read-only hosted metadata probe with negative tests.
- [Mobile #7](https://github.com/auto44e/jeios-mobile/issues/7): mobile F2FS
  workload/qualification profile and shared-backend integration plan.

These issues are open/unassigned at creation; no coding agent has been launched
by creating them and project-board membership has not been verified. Hosted
prototypes can proceed independently while runtime integration waits for M3/M4/M5
and relevant mobile execution/driver gates. No writes to real user media.
M3a remains the next core kernel gate; MP0 remains the mobile architecture gate.
