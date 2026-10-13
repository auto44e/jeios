# Fresh-kernel mobile project and ecosystem parity

Owner decision, 2026-10-13: mobile uses the fresh C JeiOS kernel from the start.
AOSP is a reference and future Android application compatibility layer across
desktop/mobile/other qualified profiles, not the mobile host OS. This explicitly
supersedes the earlier AOSP/Linux bring-up proposal in PR #11 history.

The dedicated repository is [auto44e/jeios-mobile](https://github.com/auto44e/jeios-mobile).
It owns mobile target metadata, packaging plans, ARM64/Pixel qualification docs
and parity tests. Shared kernel/architecture/driver code stays in auto44e/jeios;
mobile pins reviewed core commits rather than maintaining another kernel copy.
Original additions use Apache-2.0; no foreign code or vendor blob was imported.

First execution target is QEMU ARM64 virt after an exact board/tool/loader ADR.
Pixel 9 tokay is the first physical candidate; Pixel 10 frankel requires independent
feasibility. Neither is supported. Cuttlefish may be an external Android behavioral
reference later, not the fresh-kernel boot target. No Android lunch product applies
to the native port. Android vendor Linux modules cannot run unchanged in JeiOS.

Mobile MP0 specifies architecture contracts; MP1 entry/console/exceptions; MP2
MMU/timer/scheduling; MP3 EL0 isolation/native IPC; MP4 Pixel evidence/recovery;
MP5 physical bring-up; MP6 native mobile services. AC0 maps Android dependencies,
AC1 probes ABI/Binder/ART, AC2 qualifies a curated app set across ISAs. Security
release qualification remains independent. Core M3a remains the next core gate.

Near 1:1 parity preserves user goals, data/permission semantics, identity, E2E
sharing, family/fleet/domain management, multitouch, MFA, storage, customization
and media/accelerator APIs within device limits. Publish supported/limited/missing
capabilities; never weaken security or hide remote execution to manufacture parity.

Current mobile status: documentation, manifests, planning validator and negative
schema tests only. No native ARM boot, Pixel drivers/image or Android execution.
See the mobile repository HANDOFF and ROADMAP for exact prerequisites and tasks.
