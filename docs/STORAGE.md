# Filesystem recommendation and storage contracts

Status: recommendation and future implementation plan, 2026-10-13. No VFS,
block driver or filesystem exists in JeiOS today. The final production default
remains open pending evidence, dependency license review and porting feasibility.

## Provisional default

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

## Evidence required before selecting the production default

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

