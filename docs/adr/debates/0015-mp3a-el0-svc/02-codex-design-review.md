Design-only review of `0a4762f` (`feat/mp3a-el0-svc`); no files changed.

Bottom line:

1. TTBR1/high-half is the clean long-term regime, but it is not architecturally required for real EL0 isolation.
2. `x8 = op`, `x0..x5 = args`, `x0 = status`, `x1 = result` is the right ARM ABI.
3. Use a separate `je_arm_user_trapframe`, preferably with the same 304-byte physical layout but an explicitly named `sp_el0` field.

## Decision 1 — TTBR regime

### Recommendation

For the permanent MP3 design, keep the ADR 0011 direction:

- kernel in TTBR1 at a high VA;
- per-process user mappings in TTBR0;
- kernel mappings global and EL1-only;
- user mappings non-global and EL0-enabled.

However, I do not recommend making the high-half relink a prerequisite for MP3a. A smaller, sound first slice is possible:

- retain the kernel at its current low identity VA;
- build one TTBR0 root per process;
- copy/share identical kernel mappings into every root with AP[1]=0;
- add that process’s user mappings with AP[1]=1;
- switch TTBR0 and SP_EL1 on dispatch.

This provides architectural EL0 isolation. “Global” is only a TLB-tagging property; AP permissions provide the privilege boundary. Arm explicitly separates privileged and unprivileged permission checks in the translation tables. ([developer.arm.com](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Exception%20model.pdf?revision=a62f2bf2-b08a-4a4f-8cbe-38c67ddf4434))

The current arena cannot remain at `0x40000000` in this interim design: the kernel is linked at `0x40080000`, inside that arena. The ARM port therefore needs either:

- an ARM-specific user arena VA outside all kernel mappings; or
- the high-half relink.

If the arena VA is frozen at `0x40000000` across architectures, high-half relocation is unavoidable. If ARM may use a distinct arena base, real isolation without relinking is viable.

### What must be true for the low-kernel interim

Every process root must map, as EL1-only:

- kernel text, rodata, data, vectors, and exception code;
- the active `SP_EL1` kernel stack;
- page-table frames and required MMIO;
- any scheduler/kernel state accessed after TTBR0 switching.

The kernel cannot switch TTBR0 and then continue using an old stack unless the old stack is mapped in the new root. The switch path therefore needs either all kernel stacks mapped in every root or a stack-free assembly handoff.

Current `arm_dispatch_install()` is a no-op, so this is a real scheduler-path change, not just a paging change: [sched_arm.c:424](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:424).

### High-half hazards

If high-half is done in MP3a, close these explicitly:

- `TTBR1_EL1` must be written with the audited kernel root while MMU is off, before enabling `TCR_EL1.EPD1=0`. The current enable leaf deliberately writes `TTBR1_EL1=0` and leaves EPD1 set: [paging_enable.S:65](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/paging_enable.S:65).
- Do not add a RAM-wide writable linear map. It would create writable aliases of kernel text/rodata, exactly as ADR 0011 warns.
- Provide a bounded high-half mapping for page-table frames and any physical objects the kernel must access. `table_pointer(physical)` currently assumes identity VA==PA: [paging.c:181](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/paging.c:181).
- Add a low physical boot trampoline or equivalent transition. The current linker script and `boot.S` require load==link at `0x40080000`: [linker.ld:20](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/linker.ld:20), [boot.S:65](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/boot.S:65).

### Rejected alternatives and blast radius

- Move only the arena and keep one root: avoids relinking, but does not provide per-process isolation.
- Per-process TTBR0 roots containing low kernel + user mappings: legitimate smaller step, but requires arena relocation/carving, user AP classes, nG/ASID or full-TLB policy, root lifecycle, and TTBR0/SP_EL1 switching. It touches MP2-tested scheduler/paging paths but avoids `boot.S` relink.
- High-half now: clean final result, but touches `boot.S`, linker placement, MMU enable assembly, absolute/function-pointer references, page-table VA access, address-sensitive tests, and all high-half transition logic. This is the largest tested-path blast radius.

Exception reachability is non-negotiable: translation does not switch merely because an exception is taken. Arm preserves ELR/PSTATE, enters using the target EL’s stack, and `ERET` restores from SPSR/ELR. ([developer.arm.com](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Exception%20model.pdf?revision=a62f2bf2-b08a-4a4f-8cbe-38c67ddf4434), [developer.arm.com](https://developer.arm.com/docs/ddi0488/h/programmers-model/armv8-a-architecture-concepts/stack-pointer-selection))

## Decision 2 — SVC ABI

### Recommendation

Freeze:

```text
SVC #0
x8      = operation
x0..x5  = args[0..5]
x0      = status on return
x1      = scalar result on return
```

`x8` is preferable to `x16/x17`. AAPCS64 defines `x8` as caller-saved and an indirect-result register, so an OS syscall ABI may explicitly repurpose it. `x16/x17` are IP0/IP1 and may be overwritten by linker veneers; the current ARM exception veneer also uses them as scratch. ([github.com](https://github.com/ARM-software/abi-aa/blob/master/aapcs64/aapcs64.rst))

The reason is not merely “Linux does it”; it is that `x8` is available at the explicit syscall boundary, while `x16/x17` already have stronger toolchain and veneer obligations.

`je_syscall_decode()` already returns `{status,result}`, so the ARM glue can map that directly without changing the neutral decoder. The current x86-coupled `je_syscall_dispatch()` must not be generalized by pretending ARM has `rax/rdx`; add an ARM register/frame adapter.

Use `SVC #0`, not the immediate as the operation number. A64 instructions are fixed 32-bit instructions, and the SVC immediate is only a small auxiliary field. Reserve/check the immediate as ABI selector `0`; keep the full 64-bit operation in `x8`. ([developer.arm.com](https://developer.arm.com/-/media/Files/pdf/graphics-and-multimedia/ARMv8_InstructionSetOverview.pdf), [developer.arm.com](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Exception%20model.pdf?revision=a62f2bf2-b08a-4a4f-8cbe-38c67ddf4434))

### Restart wrinkle

`ELR_EL1` should normally remain the address after the 4-byte SVC. MP3a should have no restart logic because the frozen execution model is bounded and nonblocking.

For a future blocking continuation, do not blindly copy x86’s two-byte rewind. Choose one:

- finish the continuation in kernel state and return directly to post-SVC `ELR_EL1`; or
- subtract exactly four bytes only after validating alignment, bounds, executable mapping, and that the preceding instruction is exactly `SVC #0`.

The current x86 restart behavior is explicitly tied to rewinding `int 0x80`: [syscall.c:753](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/syscall.c:753), [syscall.c:1108](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/syscall.c:1108).

## Decision 3 — user trapframe

### Recommendation

Choose option **(b)**: a separate type:

```c
typedef struct {
    uint64_t gpr[31];
    uint64_t sp_el0;
    uint64_t elr;
    uint64_t spsr;
    uint64_t esr;
    uint64_t far;
    uint64_t vector;
    uint64_t pad;
} je_arm_user_trapframe;
```

Keep it 304 bytes and keep the same offsets if convenient, but give it separate names, assertions, saver, validator, and restore path. Do not extend or semantically overload the frozen EL1 type.

The existing lower-EL slots are bounded stubs, while the common saver uses `x16` for the vector and `x17/x18` for the guard: [vectors.S:61](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:61), [vectors.S:78](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:78). A returning EL0 path needs dedicated lower-EL veneers that preserve at least `x16..x19` before using scratch registers and read `SP_EL0` into `sp_el0`.

Option (c), dual-use `sp`, looks smaller but is not. The current validator requires:

```c
frame->sp == frame_end
```

because it treats `sp` as the post-frame kernel stack address: [sched_arm.c:253](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:253). An EL0 frame’s field must instead contain the user stack pointer. Dual-use would silently break existing diagnostics, scheduler validation, and IRQ evidence consumers.

Option (a) reopens the frozen 304-byte contract, changes assembly offsets and stack sizing, and risks crash/fixup tooling. Reject it.

### Validator changes

The scheduler needs two validators:

- EL1 frame:
  - `SPSR.M == 0x5` / EL1h;
  - existing `0x345` policy;
  - `sp == frame_address + JE_TF_SIZE`;
  - ELR inside kernel text.

- EL0 frame:
  - `SPSR.M == 0` / EL0t;
  - AArch64 state and explicitly permitted DAIF;
  - ELR aligned and inside the process’s executable user mapping;
  - `sp_el0` aligned, canonical, and inside the user stack;
  - frame itself belongs to the current process’s kernel stack;
  - generation cookie remains in `pad`.

Arm documents `M=0` as User and `M=5` as EL1h, so the current single `JE_ARM_SPSR_POLICY` cannot validate both classes. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/spsr_el1))

The EL0 return epilogue must:

1. restore ELR/SPSR/GPRs;
2. write `SP_EL0 = frame->sp_el0`;
3. restore the kernel `SP_EL1` by advancing past the frame;
4. execute `ERET`.

## Recommended MP3a boundary

Include:

- one explicitly chosen TTBR0 regime;
- real per-process user roots;
- user/kernel AP permission classes and alias audit;
- lower-EL SVC and IRQ entry;
- separate user trapframe;
- EL0 return validation;
- `x8`/`x0..x5`/`x0`/`x1` ABI;
- nonblocking `abi_query`, `yield`, and `exit` proof paths;
- tests for kernel denial, cross-process user denial, SVC results, malformed returns, and user-origin timer IRQs.

Defer:

- high-half relink if the interim low-kernel regime is approved;
- ASID optimization if MP3a uses conservative full-TLB switching;
- blocking syscall restart;
- copy-fixup assembly;
- full typed-handle/IPC syscall surface;
- dynamic high-half physical mapping and linear-map policy.

The single biggest risk is the first user-origin exception handoff: TTBR0, SP_EL1, lower-EL frame layout, and `ERET` must all agree. A mistake there can fault before C runs and make the isolation result look like a random boot hang.
