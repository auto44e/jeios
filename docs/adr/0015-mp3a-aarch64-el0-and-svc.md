# ADR 0015: MP3a — AArch64 EL0 userspace boundary and the SVC syscall ABI

Status: **proposed / owner-gated** (2026-11-08). MP3a is the ARM analogue of the
x86 M3a boundary (ADR 0007): the first EL0 (ring-3) user/kernel boundary on
aarch64 — per-process address spaces, a validated EL0 entry/return, and a bounded
SVC syscall ABI. This ADR freezes the three contract decisions that MP3a needs and
that change previously-tested ARM code (the MP2 MMU audit, the frozen trap frame,
the EL1h scheduler validator) and so require owner sign-off before any code. It
reuses the x86 security *shape* of ADR 0007 (D3 ownership/alias/no-W+X, D6
validate-on-every-return, D8/D10 ABI) on ARM mechanisms. Reviewed baseline:
`main` 0a4762f (post-MP2 scheduler + ADR 0014 lifecycle extraction).

Settled by a two-lineage design review + a trap-frame tie-break debate; transcripts
under `docs/adr/debates/0015-mp3a-el0-svc/`. AI-assisted review, not independent
human qualification.

## Context

ARM runs EL1-only today. The lower-EL vector slots (8–11) are bounded-halt stubs;
`EC_SVC64` (0x15) is defined but never dispatched; an EL0 exception halts. The MMU
is identity-mapped in TTBR0_EL1 (ADR 0011 D2), kernel linked at 0x40080000,
`TCR.EPD1=1`, `TTBR1_EL1=0`, and a pre-enable audit asserts `AP[1]=0` (no EL0) on
every leaf. The frozen 304-byte `je_arm_trapframe`'s `sp` field captures the
EL1h interrupted SP (not SP_EL0). The arch-neutral cores (`handle.c`, `endpoint.c`,
`endpoint_kern.c`, `proc.c`, `lifecycle.c`, and the pure `je_syscall_decode`) are
reusable; MP3a wires ARM into them. The x86 user arena VA `0x40000000`
(`JE_VM_ARENA_BASE`) collides with the ARM kernel identity map (QEMU RAM base, with
the kernel linked at 0x40080000 inside that 2 MiB range).

ADR 0011 D2 scheduled the high-half (TTBR1) kernel relocation for "the first PR of
MP3" with a "Flex: revisit at the MP3 first PR" clause. This ADR exercises that
clause and **defers** the relink (see D1).

## Decision

### D1 — Kernel VA regime: shared per-process TTBR0; DEFER the TTBR1 relink

Each user process gets its own TTBR0_EL1 root that **shares the kernel's mappings**
(EL1-only, `AP[1]=0`, carried identically in every root) and adds a **private user
arena** branch (`AP[1]=1`, non-global `nG=1`). On dispatch the seam installs the
process's TTBR0 (shared user ASID 1, `TLBI ASIDE1IS; DSB ISH; ISB` on every
install) and the process's `SP_EL1` kernel-stack top. `TCR.EPD1` stays 1 and
`TTBR1_EL1` stays unwritten; the kernel is **not relinked** and **no RAM-wide
linear map** is created. Isolation is enforced by the AP/UXN/PXN/nG bits plus
per-process roots within one translation regime — exactly x86 parity (every x86
process root maps the kernel supervisor-only, ADR 0007 D3). Exceptions from EL0
enter EL1 via `VBAR_EL1`/`SP_EL1` **without** a translation-regime switch, so the
kernel's shared mappings are live throughout the trap path (vector page, handler
code+data incl. `je_arm_in_handler`, the current `SP_EL1` stack, GIC, and the
hardcoded PL011 at 0x09000000 used by the stack-free double-fault path).

**The ARM user arena moves to a high VA at or above 2^44** (chosen:
`JE_VM_ARENA_BASE_AARCH64 = 0x0000_1000_0000_0000`), disjoint from the kernel
identity map by construction (the audit forces every kernel leaf `VA==PA` with
`PA < 2^44` at IPS=44), and still valid in TTBR0 under a future high-half kernel so
the user ABI does not change across the later relink. `JE_VM_ARENA_BASE` becomes
per-arch (the x86 value `0x40000000` is unchanged); user-visible absolute VAs in
the demo/report path become arena-relative.

Hierarchical deny (ADR 0007 D3 "never on a leaf bit alone"): the kernel root's L0
descriptors carry `APTable=0b01` + `UXNTable=1`, and the arena's L0 descriptor
carries `PXNTable=1`. These **reverse the MP2 audit rule that table attributes must
be 0** (owner-gated below); they only restrict EL0 and cannot mask the EL1 W^X
proofs. Kernel code/rodata frames that also back a user mapping are moved RW→RO via
break-before-make (unmap, TLBI, map RODATA — never a bare unmap); the W^X ledger
entry is what makes a later RW alias refuse.

**Why.** Real hardware-enforced EL0 isolation does not require the TTBR1 relink —
the relink is a TLB-tagging/high-VA convenience, not the privilege boundary. The
relink has the largest tested-path blast radius (see predictions). Deferring it
keeps MP1 `boot.S`/`linker.ld` and the MP2 `paging_enable.S`/TCR asserts untouched.

### D2 — SVC syscall ABI (frozen; mirrors ADR 0007 D8/D10)

```
SVC #0
x8      = operation number
x0..x5  = args[0..5]
x0      = status  (on return)
x1      = scalar result (on return; always written, 0 when none)
```

All other GPRs, `SP_EL0` and NZCV are preserved across a syscall. The operation is
a register (`x8`), not the SVC immediate (mirrors the x86 register op; an immediate
cannot be computed at runtime). A non-zero SVC immediate, `ESR.EC != 0x15`, or
`IL != 1` kills the process (the analogue of a ring-3 `int 0x81` → #GP). `x8` is
chosen over `x16/x17` (IP0/IP1), which the linker may clobber in a veneer and which
the ARM IRQ veneer already uses as scratch. The ARM glue marshals `gpr[]` → the
pure `je_syscall_decode(op, args[6], &status, &result)` and writes `{status,result}`
back to `x0/x1`; it does **not** generalize the x86-typed `je_syscall_dispatch`. The
glue snapshots `x8` + `x0..x5` before writing anything and writes status/result
last; a (future) blocking-park path never writes `x0`. MP3a ops: `abi_query`,
`yield`, `exit(code)`; any other decoded action returns `UNSUPPORTED`, never
success. **Because SVC is always live at EL0 (there is no DPL-gated gate to leave
closed), the first PR that ERETs to EL0 routes every lower-EL synchronous exception
— including SVC — to process death** (the hazard ADR 0007's #4 amendment closed on
x86). Blocking-syscall restart (MP3c) will rewind `ELR_EL1` by 4 (vs x86's 2),
only for vector-8 / EC-0x15 / IL-1 frames; an EL0 IRQ's `ELR` is the interrupted PC
and is never rewound.

### D3 — User trap frame: one frozen layout, `sp` is the interrupted-mode SP

MP3a keeps the single frozen 304-byte `je_arm_trapframe` (offsets/size/asserts
unchanged) and defines `tf->sp` (offset 248) as **whichever stack pointer the
interrupted PSTATE was using**: `SP_EL1` for EL1h frames, `SP_EL0` for EL0t frames,
discriminated at runtime. A separate EL0 frame type was rejected: the scheduler
holds the frame as an opaque `void *` and the restore tail must read the resumed
frame's `SPSR.M` to decide whether to write `SP_EL0` regardless, so a second type
only duplicates the saver/restore/validator/fixup/crash-tool paths (and adds a
strict-aliasing hazard under `-O2`), while cross-privilege resumes (an EL0 IRQ
resuming the EL1h idle frame, or vice versa) are normal. Accessors
`je_arm_tf_kernel_sp()` (fatal unless EL1h) and `je_arm_tf_user_sp()` (fatal unless
EL0t) wrap the field; a `make check` grep bans raw `->sp` outside the
saver/validator/evidence code.

**The discriminator is the validated vector group cross-checked against `SPSR.M`,
never a slot-fixed class** (a user process legitimately holds an EL0 frame or an
EL1h frame while servicing a kernel entry). `SPSR.M` is read from the fixed offset
only **after** the position proof (the frame + its full 304 bytes lie inside the
current slot's mapped kernel stack, which is `AP=00` so EL0 cannot write it; and
hardware writes `M` on entry so a user cannot forge it). `SPSR.M` is never trusted
alone.

Validator split (authoritative):

| Scope | Check | On failure |
| --- | --- | --- |
| Shared | position proof (frame+304 inside the slot kstack) before any deref | fatal |
| Shared | `M[4]==0` (AArch64); vector-group ⇔ `SPSR.M` agreement (4–7⇔EL1h, 8–11⇔EL0t) | fatal |
| Shared | `pad` generation cookie (ADR 0013) | fatal |
| EL1h | `sp == frame_end`; `(spsr & ~NZCV) == 0x345`; ELR in kernel text; phase-0 trampoline + `gpr[0]` | fatal |
| EL0t | frame exactly at `kstack_top − 304` (MP3a keeps I masked in SVC handlers; no preemptible syscalls yet) | fatal |
| EL0t | force `spsr = (spsr & NZCV) | 0x340` on resume (DIT/SSBS/TCO absent on cortex-a72 — target-profile assumption) | never fatal |
| EL0t | phase-0 (kernel-built): ELR == image entry, `sp` == arena stack top, all GPRs zero | fatal |
| EL0t | phase-1 (captured): ELR in `[base,end)` 4-aligned, `SP_EL0` in `(base,end]` 16-aligned | **user death** |

The phase-1 ELR/`SP_EL0` range check is kept (not dropped) for exact ADR 0007 D6
parity ("validate RIP/RSP within the arena on every user return"); negative tests
are made deterministic by driving entry/preemption at a known PC rather than by
weakening the check. EL0 return epilogue: restore GPRs/ELR/SPSR, `msr SP_EL0,
tf->sp`, **zero `TPIDR_EL0`** (EL0-writable, unsaved → otherwise a cross-process
channel; ADR 0007 D6 "normalize for no-TLS"), advance `SP_EL1` past the frame,
`ERET`. A fresh EL0 frame zeroes all GPRs (no kernel-register leak). EL0-exposing
system registers are pinned on the user path: `CPACR_EL1.FPEN=00` (D7 no-FPU),
`CNTKCTL_EL1` (EL0 must not reprogram the preemption timer, D10), and
`PMUSERENR_EL0`/`MDSCR_EL1`/`TPIDRRO_EL0`; `SCTLR_EL1` EL0 bits are pinned by a
readback check rather than editing MP1-tested `boot.S`.

### D4 — Scope, bounds, and staging

MP3a delivers (mirroring ADR 0007 D1/D4 scope on ARM): ≥2 EL0 processes, one thread
each, distinct TTBR0 roots + distinct physical backing, bounded built-in images,
the 3-op SVC ABI, synchronous-fault containment to the offending process, and
reap-with-survivor through `lifecycle.c`. Retained limits: one CPU, 4 KiB pages, no
FPU/SIMD (enforced via `CPACR`), no dynamic loading / fork / threads / demand
paging / mmap / signals / SMP. The claim is "experimental single-CPU aarch64 EL0
isolation in the pinned QEMU `virt`/cortex-a72 profile," never a hardened sandbox.

Staged:
- **MP3a-1** (verified at EL1, like x86 #4 step 2): the ARM `je_vm_ops` backend, L3
  coverage, per-process roots, the kernel-L0 attribute change + audit update, the
  `je_vm_audit` arch topology hook, the per-arch arena, and TTBR0/ASID + SP_EL1
  install in `arm_dispatch_install`. Proof: `AT S1E0R`/`S1E0W` probes from EL1
  (kernel text/data/devices → `PAR.F=1`; arena → per-kind EL0 perms), a cross-root
  kernel-L0 consistency audit, and mutants.
- **MP3a-2**: the slot-8/9 veneers + the EL0 restore-tail branch, the validator
  split, EL0 frame synthesis, process death for every lower-EL synchronous
  exception, the EL0-exposing sysreg hardening, the SVC ABI, and `exit` +
  reap-with-survivor. Proof: two EL0 processes; EL0 read/write/execute of kernel
  memory yields the exact `ESR`/`FAR` and kills the process; the same VA shows
  different content per process; a bad `SP_EL0`/`ELR` kills; FP use, a `CNTVCT`
  read and `SVC #1` kill; `x16`–`x18` survive an SVC (a canary, ADR-0012 style);
  `TPIDR_EL0` does not leak.
- **Deferred** (separate ADRs / MP3 slices): MP3b user-copy with ELR-keyed fault
  fixups + typed handles; MP3c capability IPC + the blocking-recv restart; later —
  per-process ASIDs, the TTBR1 high-half relink, a separate exception stack,
  software PAN.

## Predictions if other choices were made

- **TTBR1 high-half kernel now (instead of deferring):** re-qualifies MP1/MP2 — a
  `boot.S` relink off 0x40080000 + load==link change, `linker.ld` split VA/LMA,
  `paging_enable.S` writing TTBR1 before clearing EPD1 + new TCR asserts, every
  identity assumption in `paging.c` (`table_pointer`, the PA<2^44 audit, ledger
  image addresses), every MMIO/DTB/PL011 pointer, and every MP1/MP2 smoke test that
  checks exact `FAR`/`ELR`, plus closing the linear-map W^X-alias hole. No isolation
  gain for MP3a. Deferred under D1's trigger (IPS>44, KASLR, or software-PAN via
  EPD0 toggling).
- **One root shared by all processes (no per-process TTBR0):** breaks ADR 0007 D3's
  distinct-roots rule and forces a rewrite-leaves-plus-TLBI on every switch, cost
  proportional to the mapped page count and fragile.
- **Keeping the arena at 0x40000000 on ARM:** it is inside the kernel identity map
  (kernel linked at 0x40080000); with a shared-TTBR0 kernel this is a direct
  collision. Moving it ≥2^44 is forced by the shared design and is free under a
  future relink.
- **A separate EL0 trap-frame type:** duplicates the saver/restore/validator/fixup/
  crash-tool paths behind the scheduler's opaque frame pointer and adds a
  strict-aliasing hazard; the runtime `SPSR.M` discriminator is needed either way.
- **SVC immediate as the op / `x16`/`x17` for the op:** the immediate is not
  runtime-computable; IP0/IP1 may be clobbered by linker veneers and are used by the
  ARM IRQ veneer.
- **Dropping the phase-1 ELR/SP_EL0 range check:** weaker than the frozen x86 ADR
  0007 D6 contract; a delayed EL0 fault at an arbitrary address is less deterministic
  than refusing the frame at the trust boundary.

## Owner decisions required (frozen / previously-tested paths)

1. **D1:** defer the ADR 0011 D2 TTBR1 relink to a later MP3 slice (exercising D2's
   Flex clause), with the named trigger.
2. **D1:** reverse the MP2 paging-audit rule "table attributes must be 0" for the
   kernel root L0 (`APTable=01`, `UXNTable=1`) and the arena L0 (`PXNTable=1`).
3. **D1:** add EL0 leaf classes (`AP[1]=1`, `nG=1`, per-kind UXN/PXN) and their
   exact-match audit literals; bump `JE_ARM_MAX_TABLES` (128) to pre-build L3
   coverage for RAM (~134 tables for 256 MiB) and measure the pre-enable alias-scan
   TCG runtime.
4. **D1:** add an arch topology hook to the M3a-tested arch-neutral `je_vm_audit`
   (x86 PML4[0]/PDPT[0..1]/PD[0] indices), keeping x86 behavior byte-identical.
5. **D1/D3:** change the MP2-tested `vectors.S` restore tail (EL0 branch) and the
   `sched_arm.c` EL1h-only validator (the validator split).
6. **D3:** widen the frozen `trapframe.h` `sp` semantics (offsets/size/asserts
   unchanged; comment + accessor change only).
7. **D1:** make `JE_VM_ARENA_BASE` per-arch (x86 unchanged) and the user-visible
   report VA arena-relative.

## Consequences / biggest risks

- The first user-origin exception handoff (TTBR0 + SP_EL1 + the EL0 frame layout +
  `ERET`) must all agree, or it faults before C runs and looks like a boot hang.
  MP3a-1 de-risks this by proving the mappings from EL1 (`AT S1E0*`) before any
  `ERET` to EL0.
- TLB maintenance for **non-global user leaves** is the subtlest hazard and is
  partly invisible on QEMU TCG (`leaf_changed_sync` hardcodes ASID 0, correct for
  global kernel leaves but not user entries). MP3a uses the shared user ASID with a
  flush on every root install; TLBI operands are captured in the hosted seam
  (`je_arm_tlbi_hosted_last_operand`); anything still unobservable on QEMU is
  recorded as "reviewed, not observed," never as hardware qualification.
- Acceptance is proof-by-test (the ADR 0007 bar): isolation, fault-locality,
  malicious return state, cross-process VA aliasing, reap-with-survivor — a timeout
  or a lone success string is not evidence. AI-assisted implementation and
  cross-lineage review is not the independent human qualification the critical
  profile requires.

## References

- ADR [0007](0007-m3a-userspace-boundary.md) (x86 M3a boundary — the security shape
  MP3a mirrors), [0011](0011-mp2-aarch64-mmu-and-memory.md) (MP2 ARM MMU; D2 TTBR
  regime + its MP3 Flex), [0013](0013-mp2-aarch64-scheduling.md) (the ARM scheduler
  seam + the frozen trap frame + the `pad` generation cookie), [0014](0014-pre-mp3-process-lifecycle-ownership.md)
  (the arch-neutral lifecycle the ARM user path links).
- Design transcripts: `docs/adr/debates/0015-mp3a-el0-svc/`.
- Arm DDI0487 (exception model, SP selection, SPSR.M, stage-1 descriptor
  attributes, `AT S1E0*`, TLBI); pin the exact revision during implementation.
