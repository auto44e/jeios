1. D2 — choose high-half now, but D2 is incomplete as written

I recommend pinning a high-half kernel in TTBR1 for MP2. Otherwise MP3 must perform the most dangerous transition—relocating the linked kernel while also introducing per-process TTBR0 mappings. That is not a small follow-up change.

A concrete 48-bit high-half address could be:

```text
kernel VA:  0xffff000040080000
kernel PA:  0x0000000040080000
```

The current ADR is not sufficient. A high-half enable requires:

1. Keep a small identity-mapped trampoline at the current PC.
2. Identity-map the current stack, literal pool, exception vector, and every instruction executed before the jump.
3. High-map the final kernel image, high stack, data, page tables, and exception vector.
4. Program both TTBRs.
5. Enable the MMU.
6. Execute an explicit branch to the high address.

Minimal shape:

```asm
/* Still executing at identity VA 0x4008xxxx. */
msr     ttbr0_el1, x_identity_root
msr     ttbr1_el1, x_high_root
msr     mair_el1,  x_mair
msr     tcr_el1,   x_tcr

dsb     sy
isb

mrs     x0, sctlr_el1
orr     x0, x0, #(1 << 0)       /* M */
orr     x0, x0, #(1 << 2)       /* C */
orr     x0, x0, #(1 << 12)      /* I */
orr     x0, x0, #(1 << 19)      /* WXN, recommended */
msr     sctlr_el1, x0
isb

/* Do not use ADR: the target is far from the identity trampoline. */
movz    x16, #(high_entry & 0xffff)
movk    x16, #((high_entry >> 16) & 0xffff), lsl #16
movk    x16, #((high_entry >> 32) & 0xffff), lsl #32
movk    x16, #((high_entry >> 48) & 0xffff), lsl #48
br      x16
```

The identity mapping must remain valid through the `ISB` and the `br`. At the high entry, immediately switch `SP`, `VBAR_EL1`, and all address-bearing state to high-half addresses. Then remove the temporary identity aliases and perform the required TLBI sequence.

The linker must therefore support separate VMA/LMA values. The existing linker script, which places `_text` at `0x40080000`, cannot produce this design without a boot/trampoline split.

The identity-only alternative is mechanically valid:

```text
TTBR0: identity-map kernel, stack, vectors, tables
TTBR1: disabled with EPD1=1
```

It avoids the jump, but pushes the relocation and TTBR0/TTBR1 redesign into MP3. That is acceptable only if the owner explicitly chooses to defer the long-term virtual-address contract.

This decision must be escalated to the human owner because it changes the frozen address-space architecture.

2. Enable sequence and coherency

`DSB ISH; ISB` before the SCTLR write and `ISB` afterward is not sufficient as a portable contract. It is probably adequate on this single-core QEMU configuration if every relevant access is inner-shareable, but the design does not establish that precondition.

Use the stronger cold-enable sequence:

```asm
/* Tables and modified data/code have already been maintained. */
dsb     sy

/* Safe cold-start cleanup. */
ic      iallu
dsb     sy

/* Defensive: discard any stale EL1 translations. */
tlbi    vmalle1
dsb     sy
isb

msr     mair_el1,  x_mair
msr     tcr_el1,   x_tcr
msr     ttbr0_el1, x_ttbr0
msr     ttbr1_el1, x_ttbr1

dsb     sy
isb

mrs     x0, sctlr_el1
orr     x0, x0, #(SCTLR_M | SCTLR_C | SCTLR_I)
msr     sctlr_el1, x0
isb
```

The important hazard is that page-table descriptors do not classify the stores used to create the tables while `SCTLR.M=0`. After enabling the MMU, the walker uses the TCR walk attributes. If the tables were dirty in a cache, the walker could observe stale PoC contents. Clean and invalidate every table cache line with `DC CIVAC`, followed by `DSB SY`, before enabling. Cache maintenance must use aligned ranges based on `CTR_EL0.DminLine`.

With D-cache initially off, the current CPU should not have created dirty lines, but the implementation must not rely on that accidental fact. The design should still perform the maintenance.

The existing `DC IVAC` requirement for the DTB and boot context is only safe if those ranges are known not to contain dirty stores. Otherwise use `DC CIVAC`; invalidate-only maintenance can discard dirty data. Arm’s cache guidance also requires synchronization around instruction-cache invalidation before instruction caching is enabled. ([developer.arm.com](https://developer.arm.com/ja/docs/dui0808/latest/the-c-and-c-library-functions-reference/_platform_pre_stackheap_init), [developer.arm.com](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-implementing-clear-cache))

Also, D7’s “MMU-enable fault” is unprovable unless the exception vector, exception stack, handler code, and handler data are mapped and usable immediately after the enable. Otherwise the first fault can become an unreported double fault.

3. MAIR/TCR fields

For 48-bit VA input and a 4 KiB granule:

```text
T0SZ = 16
T1SZ = 16
TG0  = 00b       /* 4 KiB */
TG1  = 10b       /* 4 KiB on this AArch64 TCR layout */
IPS  = 100b      /* 44-bit physical address size */
SH0/SH1   = 11b  /* Inner Shareable */
IRGN0/1   = 01b  /* Inner WB, write-allocate */
ORGN0/1   = 01b  /* Outer WB, write-allocate */
```

For identity-only MP2:

```text
EPD0 = 0
EPD1 = 1
```

For the high-half transition:

```text
EPD0 = 0       /* temporary identity trampoline */
EPD1 = 0       /* high-half kernel */
```

A concrete identity-mode TCR value, assuming the other optional features remain zero, is approximately:

```text
TCR_EL1 = 0x00000004b009b500
```

The exact value must be generated symbolically, not copied as a magic constant.

The easiest field to get wrong is `TG1`: 4 KiB is `00b` for TG0 but `10b` for TG1 on the relevant layout. The A72 register description also has a reserved/reserved-one bit around this field; writing the TG0 encoding into TG1 can produce an unsupported configuration. ([developer.arm.com](https://developer.arm.com/docs/100095/0001/system-control/aarch64-register-descriptions/translation-control-register-el1), [developer.arm.com](https://developer.arm.com/docs/ddi0500/j/system-control/aarch64-register-descriptions/translation-control-register-el1))

The MAIR value in D3 has a concrete error if `Device-nGnRnE` is intended:

```text
AttrIdx0 = 0xff   /* Normal WB RA/WA, inner + outer */
AttrIdx1 = 0x00   /* Device-nGnRnE */
MAIR_EL1 = 0x00000000000000ff
```

`0x04` is Device-nGnRE, not Device-nGnRnE. ([developer.arm.com](https://developer.arm.com/docs/ddi0500/j/system-control/aarch64-register-descriptions/memory-attribute-indirection-register-el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/mair_el1))

There is also an ADR ambiguity:

```text
PARange mismatch fails closed
IPS is measured PARange capped at 44
```

These conflict unless “mismatch” means “measured value below 44.” Pin one rule. I recommend:

```text
measured PARange < 44: fatal
measured PARange >= 44: IPS = 44
```

The page-table base addresses must be 4 KiB aligned and below the selected physical-address limit.

4. W^X

D4’s permissions are basically correct:

```text
.text:
    AP = 10b       /* EL1 read-only, EL0 inaccessible */
    PXN = 0
    UXN = 1

.rodata:
    AP = 10b
    PXN = 1
    UXN = 1

.data/.bss/.stack/page tables:
    AP = 00b       /* EL1 read/write, EL0 inaccessible */
    PXN = 1
    UXN = 1
```

The AP encodings are architectural: `00` is privileged RW/no EL0 access, and `10` is privileged RO/no EL0 access. ([developer.arm.com](https://developer.arm.com/architectures/learn-the-architecture/armv8-a-memory-model/permissions-attributes))

Set `SCTLR_EL1.WXN=1` as defense in depth. It makes writable privileged mappings execute-never, but it is not a replacement for correct PXN/UXN descriptors. In fact, WXN can mask a mutation that accidentally clears PXN, so the tests must deliberately exercise both descriptor permissions and effective behavior.

Page-table frames may be mapped RW+XN. The walker reads descriptors as translation-table data; it does not fetch instructions from those frames. The table frame’s own PXN/UXN bits do not prevent a hardware table walk.

The no-alias invariant is not checkable from virtual ranges alone. At map-install time, track permissions by physical frame:

```text
physical frame -> { writable, executable, mappings }
```

Reject any new mapping that makes the same frame both writable and executable, including aliases in TTBR0 and TTBR1. Block mappings must be split or accounted for at page granularity where section boundaries differ. Page tables must be built through a validation pass before being installed.

5. Allocator window

Yes, changing `je_pages` from a fixed physical origin/limit to a base-plus-limit interface touches tested allocator behavior and must be escalated to the owner. The repository explicitly treats this as a tested M1/M2 path.

The minimal non-forking design is:

```c
struct je_phys_window {
    uint64_t base;
    uint64_t limit;   /* exclusive */
};
```

Keep the generic allocator unchanged in concept:

```text
page index 0 -> physical base
page index i -> base + i * 4096
```

Add an x86 wrapper that supplies the existing x86 window, and an ARM adapter that supplies the DTB-derived RAM window and reservation set. The allocator should not learn about FDT types.

Use 64-bit physical addresses and 64-bit arithmetic for:

```text
base + index * PAGE_SIZE
limit - base
reservation endpoints
```

A 32-bit page index is enough for this exact 256 MiB target, but it is an unsafe generic representation for a 44-bit physical regime and invites overflow in address arithmetic. `size_t` is also acceptable for counts on AArch64, provided conversions are range-checked.

The adapter must preserve distinct semantics for:

```text
reserved
reserved/no-map
dynamic reservation
free
```

It must also reserve the allocator’s own metadata if that metadata lies in managed RAM.

6. Testability and false success

| Invariant | Mutation that can fool a naive test | Observation that defeats it |
|---|---|---|
| W^X | Clear PXN on `.data`, or make `.text` writable; leave WXN enabled so the bug is masked | Execute from `.data` and write `.text`; require exact EC, FAR, and permission-fault FSC. Run a descriptor-level physical-frame alias audit |
| Stack guard | Map the guard page accidentally, or test an address one page away | Access the exact guard-page VA and require the exact FAR and translation-fault ESR |
| Rollback | Free only the last page after a multi-page allocation fails | Inject failure after each allocation step; compare the complete bitmap/free count before and after; reallocate the same deterministic range |
| Reservations honored | Ignore `rsv_dynamic`, or have the allocator simply never happen to choose that frame | Use a fixture where the reserved frame is the only candidate, then request it through the normal allocator path; log and assert the returned physical address |
| MMU actually on | Leave `M=0` and continue using identity addresses | Map `VA=0x0000000100000000` to `PA=0x40090000`, initialize the PA through identity, read/write through the non-identity VA, then verify through identity. MMU-off execution cannot update that PA |
| MMIO non-cacheable | Map PL011 as Normal memory but pass a basic UART smoke test | Inspect the descriptor AttrIndx and perform a mutation test that changes the descriptor to Normal; require the test to fail |
| High-half jump | Keep identity aliases everywhere, so an incorrect branch target is never exercised | Remove the identity alias after the high entry, switch SP/VBAR, and verify a non-identity code/data access |
| MMU fault handling | Exception path itself faults, and the harness interprets a halt as success | Require an ordered fatal marker plus EC/FAR/ESR fields; a halted emulator with no marker is failure |

For the MMU proof, reading `SCTLR_EL1.M` is only supplementary evidence. The non-identity read/write test is the actual proof that translation is active. A page-permission fault is also strong evidence, but it should not be the only proof because an unrelated bus or translation fault could produce a misleading “did not continue” result.

For reservations, the test must be deterministic. “The allocator did not return a reserved frame this time” is insufficient. Construct a case where ignoring the reservation necessarily returns the forbidden frame.

7. Scope

The slice is too large for one safe, reviewable PR if it includes:

```text
allocator redesign
FDT-to-allocator adaptation
page-table construction
W^X section layout
high-half linker transition
cache maintenance
MMU enable
fault-path validation
mutation testing
```

The clean seam is the allocator interface.

First PR:

```text
ARM boot-context adapter
base+limit allocator parameterization
all reservation classes
transactional allocation
allocator mutation tests
MMU-off QEMU evidence
```

Second PR:

```text
page-table builder
W^X mappings
MAIR/TCR construction
identity trampoline and high-half jump
cache/TLB maintenance
non-identity translation proof
fault probes
real allocator integration
```

The second PR may use a deterministic fake frame provider for unit tests, but it must not claim the complete MP2 gate until it also runs against the real reservation-aware allocator.

Owner escalation is required for the high-half decision, the allocator tested-path change, the PARange acceptance rule, and any relaxation of W^X, reservation, or fail-closed guarantees.
