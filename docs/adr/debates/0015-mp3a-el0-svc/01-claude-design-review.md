**Verdict:** MP3a can get real, hardware-enforced EL0 isolation without relinking the kernel. My recommendations:

1. **Kernel VA:** give each process its own TTBR0 root that shares the kernel's identity mappings (global, EL1-only) and adds a private arena branch. Defer the TTBR1 relink.
2. **SVC ABI:** `SVC #0`, op in x8, args in x0–x5, status in x0, result in x1.
3. **Trap frame:** keep the one frozen 304 B frame and redefine `sp` as "the SP the interrupted PSTATE was using", which means SP_EL0 for EL0 frames.

No code was changed and nothing was built or run. This is an AI-assisted review, not human qualification.

**The prompt's framing of option (a) is off.** x86 does not get isolation from a CR3 that excludes the kernel. Every x86 process root maps the kernel's page directory supervisor-only (`kernel/arch/x86_64/vm_backend.c:149`, ADR 0007 D3). A shared TTBR0 is x86 parity; the TTBR1 split is the design that departs from x86.

## Decision 1 — kernel VA regime: recommend the shared-TTBR0 design

**How the kernel is reached when EL0 traps.** Taking an exception changes the exception level, the SP selection and DAIF. It never changes TTBR0, TTBR1 or the ASID, because EL0 and EL1 share one translation regime. So everything the trap path touches must already be mapped in every root that EL0 can run under:
- the vector page and the current thread's SP_EL1 stack;
- handler code and data, including `je_arm_in_handler`;
- the GIC registers;
- the PL011 UART, because the stack-free double-fault path hardcodes physical address 0x09000000 (`vectors.S:153`, `sched_arm.c:90`).

This design guarantees that by copying the kernel root's L0 descriptors byte-for-byte into each process root. Those kernel slots must stay within L0[0..31], and an audit should enforce it.

**Invariants the ADR has to pin:**
- **Move the ARM arena to VA ≥ 2^44** (for example 0x0000_1000_0000_0000, which is L0[32]).
  - The audit already forces every kernel leaf to VA==PA with PA < 2^44 (`paging.c:438`, `paging.c:652`), so the arena is provably disjoint while IPS stays at 44.
  - That address is also valid in TTBR0 under a future high-half kernel, so a later relink does not change the user ABI.
  - Two constants become per-arch: `JE_VM_ARENA_BASE` (`vm.h:36`, x86 value unchanged) and the absolute `JE_SVC_REPORT_VADDR` (`svc.h:235`), which should become arena-relative.
- **Kernel entries must be identical in every root.**
  - They are global, so if two roots disagree the TLB may return either version or raise a conflict abort.
  - Copy them byte-for-byte and audit `proc_L0[i] == kern_L0[i]`.
  - The kernel's L0 entries are already frozen after boot, because the runtime path never creates tables (`paging.h:156`).
- **Hierarchical deny, to satisfy ADR 0007 D3's "never on a leaf bit alone".**
  - Set APTable=01 and UXNTable=1 on the kernel root's own L0 descriptors (they get copied as-is), and PXNTable=1 on the arena's L0 descriptor.
  - Do not set the kernel-branch bits only in process roots: that makes the global entries inconsistent.
  - This reverses the MP2 audit rule that requires table attributes to be 0 (`paging.c:591-596`), so it needs the owner. It cannot mask the EL1 W^X proofs, because these bits only restrict EL0.
- **User leaves are non-global (nG=1) with their own exact-match literals:**
  - code `0x0020000000000FC3`
  - rodata `0x0060000000000FC3`
  - data `0x0060000000000F43`
- **ASIDs.** The kernel root uses ASID 0. All user roots share ASID 1, with `TLBI ASIDE1IS; DSB ISH; ISB` every time a user root is installed. This mirrors x86's "no PCID, reload CR3". Per-process ASIDs come later (see the risk section).
- **Kernel alias of a code or rodata frame.**
  - Use break-before-make to change it from RW to read-only: unmap, TLBI, then map RODATA. Never just unmap it.
  - The non-zero ledger entry is what makes `je_arm_mmu_map_page` refuse a later RW alias (`paging.c:881`).
  - Publish code with `DC CVAU` + `IC IVAU` + barriers. QEMU will not observe this.
- **Route every new physical-address-to-pointer conversion through one `je_arm_phys_to_virt()`** (identity today), so the eventual relink has a single place to change.

**ADR 0011 D2's three hazards are avoided entirely, not left open:**
- No relink.
- No linear map: frames reach the kernel only through single, on-demand identity leaves.
- EPD1 stays 1, so the UNKNOWN TTBR1 value is never walked. TCR, MAIR and the MMU enable routine do not change.

**Rejected alternatives:**
- **TTBR1 high-half kernel now (option c).** It would re-qualify MP1 and MP2:
  - boot.S: the load==link check plus a two-stage jump to the high address;
  - linker.ld: separate virtual and load addresses;
  - paging_enable.S: write TTBR1 before clearing EPD1, and update the TCR asserts;
  - paging.c's identity assumptions (`:183`, `:652`, image addresses in the ledger);
  - every MMIO, DTB and PL011 pointer;
  - every MP1/MP2 smoke test that checks exact FAR/ELR values;
  - closing hazard 2 with a 4 KiB linear map that excludes `[_text,_end)` and an alias audit across two maps.

  None of that strengthens MP3a's isolation claim.
- **One root shared by all processes.** It breaks D3's distinct-roots rule, and rewriting leaves plus TLBI on every switch costs time proportional to the page count and is fragile.

**Costs both designs pay, so they should not decide D1:**
- The arch-neutral `je_vm_audit` hardcodes x86 indices PML4[0], PDPT[0..1] and PD[0] (`vm.c:435-465`).
  - No honest ARM backend can pass it.
  - It needs an arch topology hook that leaves x86 behaviour byte-identical. That is owner-level, because it is M3a-tested code.
- The runtime mapper cannot create tables, and `JE_ARM_MAX_TABLES` is 128 (`paging.c:54`).
  - Pre-building empty L3 coverage for 256 MiB of RAM takes about 134 tables.
  - The pre-enable alias scan grows with the number of leaves × L3 tables × 512, so its TCG run time needs measuring.
- The syscall glue and the restart path are x86-typed (`syscall.c:771`, `sched.c:17`, `sched.c:398`).

**Which tested code each option touches:**

| Tested path | Shared TTBR0 | TTBR1 kernel |
|---|---|---|
| boot.S, linker.ld (MP1) | untouched | relink |
| paging_enable.S, TCR asserts (MP2) | untouched | rewritten |
| paging.c (MP2) | L0 attribute rule, table cap, L3 pre-build | the same, plus identity assumptions throughout |
| MMIO/DTB/PL011 sites, MP1/MP2 smoke checks | untouched | all re-baselined |
| vm.h / svc.h arena constants | per-arch (x86 unchanged) | untouched |
| vm.c audit, vectors.S restore tail, sched_arm.c | changed | changed |

## Decision 2 — SVC ABI

**Recommendation:** `SVC #0`. Op in x8, args in x0–x5. Status in x0. Result in x1, always written, 0 when there is none (x86 writes rdx=0 on failure, `syscall.c:461`). Every other GPR, SP_EL0 and NZCV is preserved.

- **x8 rather than x16/x17.** x16/x17 (IP0/IP1) are scratch registers the linker may clobber in a veneer between an out-of-line setter and the `svc`; x8 is never touched that way. x8 is also the most common AArch64 convention, which helps tooling. A 16-byte {status, result} struct comes back in x0:x1 under the standard calling convention, so the C wrapper needs no shuffling.
- **Op in a register, not the SVC immediate.** This mirrors x86, and the immediate cannot be computed at runtime.
  - A non-zero immediate should kill the process, the analogue of a ring-3 `int 0x81` → #GP. That keeps the immediate space reserved.
  - Also check that ESR.EC is 0x15 and IL is 1.
- **x0 is both arg0 and status.**
  - The glue must snapshot x8 and x0–x5 before writing anything (x86 already does, `syscall.c:771`) and write status/result last.
  - The park path for a blocking call must never write x0.
- **Restart (MP3c).**
  - Rewind ELR by 4, versus `JE_INT80_LEN` = 2 on x86.
  - Only rewind frames that came in on vector 8 with EC 0x15 and IL=1. For an IRQ taken at EL0, ELR is the interrupted PC and must never be rewound.
- **SVC is always live at EL0, and slot 8 is a halt today.**
  - Unlike x86, there is no DPL-gated gate to leave closed, so any user `svc` would halt the kernel.
  - The first PR that ERETs to EL0 must therefore route every lower-EL synchronous exception, including SVC, to process death. This is the same hazard ADR 0007's #4 amendment closed.
- **MP3a op set:** `abi_query`, `yield`, `exit`.
  - The decoder will also return HANDLE/TRY/etc. actions for valid encodings.
  - The ARM glue's `default:` must return UNSUPPORTED for those, never success.

## Decision 3 — trap frame

**Recommendation (option c):** `tf->sp` holds whichever stack pointer the interrupted context was using, as selected by SPSR.M: SP_EL1 for EL1h frames, SP_EL0 for EL0t frames.
- Offsets, size and static asserts stay the same. Only the comments at `trapframe.h:34` and `trapframe.h:48` change; the saver comment at `vectors.S:111-116` already says the lower-EL value would be SP_EL0.
- Origin is the vector group cross-checked against SPSR.M (D10's "saved CS" check). A mismatch is fatal.
- It stays compatible with ADR 0011 D4's deferred plan to run the kernel on SP_EL0.

**Rejected:**
- **(a) Extend the frame.** Growing 304 → 320 B ripples through every `sub/add sp,#JE_TF_SIZE`, frame placement, the asserts and ADR 0013 — just to store SP_EL1 for an EL0 frame, which is always the frame address + 304. Nothing is gained.
- **(b) A separate EL0 frame type.** Two layouts behind the scheduler's opaque frame pointer mean the validator must know the type before it can read SPSR, which invites type confusion. It also means two savers, two restore tails, and two layouts for the ELR-keyed fixup table and crash tooling. x86 uses one frame for CPL0 and CPL3.
- **Using `pad` for SP_EL0** is not available: it already holds the ADR 0013 generation cookie (`sched_arm.c:276-283`, `:483`).

**What has to change around the frame:**
- **Slots 8 and 9** need dedicated veneers in the style of slot 5:
  - spill x16–x19 first, then copy SP_EL0 into `tf->sp`;
  - the shared `mov w16` saver clobbers x16–x18 (`trapframe.h:16-23`), which is fine for fatal frames but wrong for a returning SVC.
- **The restore tail** (`vectors.S:239-277`, MP2-tested) needs extra steps for EL0 frames:
  - `msr SP_EL0, tf->sp`;
  - zero TPIDR_EL0 on every EL0 return. It is EL0-writable and not saved in the frame, so it is otherwise a cross-process channel; this matches D6's "normalize for no-TLS".
- **The validator for EL0 frames** (today `sched_arm.c:253-261` is EL1h-only):
  - The frame must sit at exactly kstack_top − 304. This is ARM's TSS.RSP0: SP_EL1 while EL0 runs is frame + 304, so any other position corrupts the next EL0 entry.
  - Re-sanitize SPSR to `(spsr & NZCV) | 0x340` (the EL1 policy 0x345 with M=EL0t).
  - Require M[4]=0. The A72 supports AArch32 at EL0, and slots 12–15 halt, so entering AArch32 would hand users a kernel halt.
  - If ELR is outside [base, end) or not 4-aligned, or SP_EL0 is outside (base, end], kill the process (user state).
  - If the frame position, the mode or the cookie is wrong, it is fatal (kernel state).
- **`arm_dispatch_install`** only needs to install TTBR0 and flush; no SP_EL1 install is required.
- **A fresh EL0 frame** must have all GPRs zero; `sched_arm.c:409` currently puts `arg` in x0.
- **System registers that expose state to EL0 are never written today** (grep finds no hits):
  - `CPACR_EL1`: FPEN=00 is how D7's no-FPU rule is enforced.
  - `CNTKCTL_EL1`: otherwise EL0 may be able to reprogram the preemption timer, which defeats D10.
  - `PMUSERENR_EL0`, `MDSCR_EL1`, `TPIDRRO_EL0`.

  Their reset values are UNKNOWN on real hardware. QEMU resets them to zero, so leaving them out passes smoke tests.
- **SCTLR's EL0 bits are already restrictive** (`boot.S:111-113`, value 0x30D00808). Pin them with a readback check rather than editing MP1-tested boot.S.

## Can MP3a skip the relink?
Yes. Isolation comes from the AP/UXN/PXN/nG bits plus per-process roots inside one translation regime, not from which TTBR holds the kernel. A TTBR1 kernel is also mapped while EL0 runs. Neither PAN (the A72 doesn't have it) nor Meltdown (the A72 is not affected by variant 3) separates the two designs.

The limits of the shared design are the triggers for scheduling the relink: IPS above 44 bits, KASLR, or wanting cheap software-PAN by toggling EPD0. Deferring it amends ADR 0011 D2's schedule; D2's own "Flex" clause names this PR as the revisit point, but the decision is the owner's.

## MP3a scope
- **MP3a-1 (verified at EL1, like x86 #4 step 2):**
  - Build: the ARM je_vm_ops backend, L3 coverage, per-process roots, the kernel L0 attributes plus audit change, the vm.c topology hook, the per-arch arena, and TTBR0/ASID install.
  - Proof: `AT S1E0R`/`S1E0W` probes from EL1 on kernel text, data and devices return a fault (PAR.F=1); arena pages return their per-kind permissions; a cross-root consistency audit; mutants.
- **MP3a-2:**
  - Build: the slot 8/9 veneers and restore tail, the validator split, EL0 frame setup, process death for every lower-EL synchronous exception, the system-register hardening, the 3-op SVC ABI, and exit plus reap-with-survivor through lifecycle.c.
  - Proof: two EL0 processes; EL0 read/write/execute of kernel memory gives the exact ESR/FAR and kills the process; the same VA shows different content in the two processes; a bad SP_EL0 or ELR kills; FP use, `CNTVCT` reads and `SVC #1` kill; x16–x18 survive an SVC (an ADR 0012-style canary); TPIDR_EL0 does not leak.
- **Deferred:**
  - MP3b: user copy with ELR-keyed fixups (the first returning slot-4 path; LDTR/STTR give copies checked with EL0 permissions), alongside handles.
  - MP3c: IPC and the blocking-recv restart.
  - Later: per-process ASIDs, the TTBR1 relink, a separate exception stack, software PAN.

## Single biggest risk
TLB maintenance for user leaves that QEMU cannot see.
- `leaf_changed_sync` hardcodes ASID 0 (`paging.c:750`). That is correct for global kernel leaves but silently misses non-global user entries.
- QEMU flushes its TLB when the ASID changes in a TTBR write (expected; verify on the pinned version). With per-process ASIDs, a missing or wrong-ASID TLBI would pass every smoke test and become a stale-translation bug on real hardware, with one process reaching another's freed frames.

Mitigations:
- Use the shared user ASID with a flush on every install.
- Swapping roots under the same ASID should leave stale entries in QEMU, so a mutant that drops the flush ought to be visible there. Measure that before claiming it.
- Capture TLBI operands in the existing hosted test seam (`je_arm_tlbi_hosted_last_operand`).
- Anything still not observable gets recorded as "reviewed, not observed".

**Owner decisions needed:**
- ADR 0011 D2's schedule (deferring the relink)
- the MP2 audit's table-attribute rule and the table cap
- the `vectors.S` restore tail
- the `sched_arm.c` validator
- the wording of `trapframe.h` `sp`
- the arena constants in `vm.h` and `svc.h`
- the `vm.c` audit topology hook
