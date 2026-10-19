# Peer debate — 2026-10-19T07:46:14Z
Topic: JeiOS MP1 ARM64 VBAR_EL1 vectors + trap frame (D5-3): table layout, SP/reentrancy, frame struct + save asm, C diagnostic, BRK deliberate-exception test, alignment
Rounds: 2   Roles: claude=implementor codex=adversarial-reviewer
Context file: /tmp/mp1v.md


[R1 CLAUDE] I agree with most of the brief. Pushback on five points: the re-entry guard, FAR handling, the exact ESR value, the SPSR assertion and the restore path. Facts below are from the AArch64 architecture; I checked `boot.S` and `linker.ld` at f46f4f9.

## 1. Table: install all 16, only one reachable
- **Reachable in MP1:** only Current-EL/SP_ELx Sync (vector index 4, offset 0x200).
  - SError is masked by PSTATE.A, so it pends rather than vectors. The brief's "SError possible" is wrong while DAIF stays masked.
  - SP_EL0 group: boot.S sets SPSel=1, so it is unreachable.
  - Lower EL groups: no EL0 yet.
- **Install all 16 anyway.** Each slot is `sub sp,sp,#TF_SIZE; stp x0,x1,[sp]; mov x0,#idx; b je_arm_vec_common`. Each slot is wrapped in `.balign 0x80` plus `.org slot_start+0x80`, so an overfull slot fails at assembly time instead of silently running into the next one.
- **Alignment:** put the table in its own `.text.vectors` section with `.balign 2048`. In `linker.ld`, add an explicit `.text.vectors : ALIGN(2048) { KEEP(...) }`, because `*(.text*)` would also absorb it. Add `ASSERT((je_arm_vectors & 0x7FF) == 0)`.
- **Install sequence:** `msr VBAR_EL1; isb`. The ISB is required before anything depends on the new VBAR, including the BRK test. Then read VBAR_EL1 back with `mrs` and print the comparison against `&je_arm_vectors`. That readback is the evidence that the install took.

## 2. SP / reentrancy: one stack is fine; a flag in the saver is not
- Taking an exception to EL1 selects SP_EL1 and sets all of PSTATE.DAIF. So the handler is non-reentrant for async exceptions by construction, and a single stack is acceptable for MP1.
- **The proposed guard is broken.** If the guard flag is checked after `sub sp; stp`, a fault caused by a bad SP faults again on that same `stp`. The CPU re-vectors, subtracts again and faults again, forever. That is a livelock, which is exactly the "timeout, not observed behavior" outcome the task forbids.
- There is no MMU and no guard page. Stack overflow therefore grows down silently into `.bss` (the stack sits just above BSS), and every frame push makes it worse.
- **Fix: validate before touching the stack.** The slot's first instruction is `msr SP_EL0, x0`. SP_EL0 is free scratch in MP1 because there is no EL0 and SPSel=1. Then:
  1. Check the guard flag using `adr` (PC-relative, no stack).
  2. Check that SP is 16-aligned and that `SP-TF_SIZE ∈ [__stack_bottom, __stack_top)`.
  3. If either check fails, branch to a stackless asm path (reuse boot.S's no-stack PL011 putc and halt) that prints `EXC NESTED|BADSP vec=N` and runs WFE forever.
  4. If both pass, restore x0 with `mrs x0, SP_EL0`, then push.

  This costs about 10 instructions, which fits in 32 per slot. Alternatively keep the slot to 4 instructions and do the checks in common code before any push. Then the `sub` has to move into common code too, and the slot carries the index in SP_EL0.
- Documented hazard: SP_EL0 as scratch is MP1-only. When EL0 arrives, the lower-EL entry must save the user SP_EL0 first, and this scheme moves to TPIDR_EL1 or a per-CPU area. Record this as an invariant that changes later.
- **Device-memory hazard:** with the MMU off, data accesses are Device-nGnRnE, so any unaligned access faults whatever SCTLR.A says. The frame has to keep strict 16-byte/8-byte alignment.

## 3. Frame: proposed layout, owner sign-off needed (D5-3)
```c
struct je_arm_trap_frame {   /* 0x130 = 304 B, 16-aligned */
    uint64_t x[31];          /* 0x000..0x0F0  x0..x30 (x18, x29 fp, x30 lr saved as plain GPRs) */
    uint64_t sp;             /* 0x0F8  interrupted SP: entry sp + TF_SIZE (SP_ELx group) or SP_EL0 (SP_EL0/lower groups) */
    uint64_t elr;            /* 0x100  ELR_EL1 — fixup key (D5-3) */
    uint64_t spsr;           /* 0x108 */
    uint64_t esr;            /* 0x110 */
    uint64_t far;            /* 0x118  valid only for EC in {0x20,0x21,0x22,0x24,0x25,0x34,0x35} */
    uint64_t vector;         /* 0x120  0..15 */
    uint64_t _pad;           /* 0x128 */
};
```
- Offsets come from one shared header, `#define JE_TF_ELR 0x100` and so on, used by both the asm and C. C carries `_Static_assert(offsetof(...) == JE_TF_*)` for every field plus a size assert.
- **Save order:**
  1. `stp` pairs x0/x1 through x28/x29.
  2. `str x30`.
  3. `add x21, sp, #TF_SIZE; str x21` into the sp slot.
  4. `mrs` ELR, SPSR, ESR, FAR into four registers, then two `stp`s.
  5. `str` the vector index.
  6. `mov x0, sp; bl je_arm_exception`.
  7. If C returns, fall through to a halt.
- **No FP/SIMD state:** `-mgeneral-regs-only` means the frame does not hold Q registers. Declare it "complete for GPR state only." Any accidental FP use traps with EC 0x07, which this handler would report.
- **No restore/`eret` path in this PR.** An untested restore is dead code that a later PR would trust. The frame is write-once. The layout already leaves room for a symmetric restore; add it when there is a returning path to test.
- **FAR:** the architecture defines FAR_EL1 as UNKNOWN for BRK, so the brief's "FAR (the faulting VA)" is wrong for the test. C prints `far=n/a` unless the EC is one of the abort, PC-alignment or watchpoint classes. The test must never assert FAR.
- **Owner flag:** the frame layout is the D5-3-constrained ABI, and future fixups key on `elr`. It needs owner sign-off; agent majority cannot settle it.

## 4. Diagnostic: fixed format, bounded output, no GPR dump
```
JEIOS-ARM: VBAR ok vbar=0x0000000040081000 expect=0x0000000040081000     (before any test)
JEIOS-ARM: EXC vec=4 ec=0x3c il=1 iss=0x0004a45 esr=0x00000000f2004a45 elr=0x... spsr=0x00000000000003c5 far=n/a
JEIOS-ARM: HALT unhandled-exception
```
- All values are fixed-width hex from a non-allocating formatter. No GPRs and no x29/x30 are printed; that is the redaction.
- Absolute addresses are fine for MP1 because there is no KASLR and load==link is enforced. Note the future redaction policy, `elr` printed as `_text+off`, in the docs.
- The C handler must be leaf-ish: no stack protector, and only the polling putc.

## 5. BRK test: right trigger; correcting the expected values
- **Masking:** BRK is a synchronous Breakpoint Instruction exception. PSTATE.D masks breakpoint, watchpoint and step debug exceptions, but not the BRK instruction. So BRK vectors with DAIF=0xF. Confirmed.
- **Exact ESR.** The brief left out the IL bit:
  - Formula: ESR = `(0x3C<<26) | (IL=1<<25) | imm16`.
  - With `brk #0x4a45` ("JE"), ESR = **0xF2004A45**.
  - Assert EC=0x3C, IL=1, ISS[15:0]=0x4A45, and ISS[24:16]=0.
- **ELR:** equals the address of the BRK instruction itself. Unlike SVC, the preferred return address is the BRK, not BRK+4. Assert `elr == &je_arm_vector_test_brk`, using a global label on the instruction. This is exactly the PC-keyed property D5-3's fixup will depend on.
- **SPSR:** assert `(spsr & 0x3CF) == 0x3C5` (M=EL1h, DAIF=1111). Mask out NZCV and other bits, because they depend on preceding code.
- **Vector:** assert `vector == 4`.
- **Where the check runs:** under `-DJE_ARM_VECTOR_TEST`, after the step-1 markers and the VBAR readback line. The C handler compares every field and prints `VECTOR-TEST PASS` or `VECTOR-TEST FAIL field=...`, then halts.
- **What the smoke run asserts:** the PASS line, the raw `esr=0x00000000f2004a45`, and the absence of FAIL. The harness checks the raw value independently of the in-kernel comparator, so a bug in that comparator can't produce a false pass.
- **Normal boot:** installs the vectors and prints the VBAR readback, but executes no BRK.

## 6. Other gotchas
- `b` from a slot to common code has ±128 MiB reach, which is fine.
- The saver is pure GPR code with no FP/SIMD and no exclusives.
- MMU off means VA==PA. The table sits in loaded `.text` (not NOLOAD), so it is present in the image.
- D-cache is off and the code is never modified at runtime, so there are no I/D coherency concerns. That changes at MP2: when the MMU comes up, the VBAR must stay valid across the switch, so re-point VBAR or identity-map it.
- SCTLR_EL1 state on entry isn't defined by the boot protocol beyond the MMU and D-cache being off. Recommend boot.S write SCTLR_EL1 to a known value (SA=1, A=0, M=0) so the fault behaviour the handler depends on is deterministic.

**Open items for the other engineer:**
- Do you accept SP_EL0-as-scratch for the stack check, or do you want a dedicated exception stack now? I argue against the stack: there is nothing that nests yet.
- Should the SCTLR_EL1 write go in this PR or in a separate one?
[R1 CODEX] codex
I’ll pressure-test the proposed design against the architectural edge cases, especially the SP_EL0 scratch idea, exception-entry state, BRK semantics, and frame correctness.
web search: 
web search: site:developer.arm.com AArch64 exception vector table VBAR_EL1 2048 bytes SP_EL1 exception entry DAIF
web search: 
web search: site:developer.arm.com BRK PSTATE.D masked AArch64 breakpoint instruction exception not masked
web search: 
web search: site:developer.arm.com AArch64 BRK ELR_EL1 address of BRK instruction
web search: 
web search: site:developer.arm.com SCTLR_EL1 M 0 MMU disabled memory type Normal non-cacheable AArch64
web search: 
web search: site:developer.arm.com FAR_EL1 valid exception class SP alignment PC alignment watchpoint AArch64
web search: 
web search: AArch64 BRK instruction PSTATE.D exception masked source
web search: 
web search: AArch64 SoftwareBreakpoint PSTATE.D BRK pseudocode
web search: 
web search: site:developer.arm.com SError PSTATE.A uncontainable error mask
codex
I agree with Claude on the 2 KiB table, `VBAR_EL1; isb`, vector 4 for Current EL/SP_ELx Sync, FAR=`n/a` for BRK, no `eret` path, and the frame offsets. FAR is architecturally defined for aborts, PC alignment, and watchpoints—not BRK. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/vbar_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/b/aarch64-system-registers/far_el1))
I dispute the proposed SP_EL0 scratch protocol:
- It is internally incomplete: SP_EL0 stores original `x0`, but the vector index is also proposed to live there. There is no free GPR for bounds checking while preserving `x1`–`x30`.
- It destroys the future lower-EL user stack. Lower-EL entry must preserve SP_EL0 before using it.
- A guard checked after `sub sp`/`stp` cannot detect failure of that first push; Claude correctly identifies the recursive-fault risk, but the replacement does not solve register preservation.
For MP1 I recommend the narrower contract: trust the boot-established, 16-byte-aligned SP_EL1 range; save `x0/x1` first, then set/check `in_handler`. Nested exceptions after that point take a stackless fatal path. Explicitly document that a pre-frame stack fault is outside the MP1 guarantee. A genuinely stackless precheck requires reserved per-CPU/system-register scratch or a separately designed exception stack.
The frame is otherwise sound:
```c
uint64_t x[31];   // x0..x30; no x31 exists to save
uint64_t sp;
uint64_t elr;
uint64_t spsr;
uint64_t esr;
uint64_t far;
uint64_t vector;
uint64_t pad;
```
The `sp` contract must distinguish interrupted SP_EL1 from SP_EL0/user SP for future entries. The field order and offsets are D5-3 ABI decisions and require owner sign-off. `x18`, `x29`, and `x30` are ordinary saved GPRs; compiler disassembly must verify the C ABI assumptions.
BRK remains the best deterministic test, but record higher-level debug routing as a prerequisite; BRK is a software breakpoint/debug exception, not merely an ordinary data fault. Assert defined ESR fields and the low 32 bits:
```text
EC   = 0x3c
IL   = 1
ISS  = 0x4a45
ESR[31:0] = 0xf2004a45
ELR  = address of the BRK instruction
vector = 4
(spsr & 0x3cf) == 0x3c5
```
Do not require the entire 64-bit ESR unless the implementation explicitly guarantees upper bits. Arm documents BRK’s `EC=0x3c` and immediate in ISS. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/a/aarch64-system-registers/esr_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/elr_el1))
Missed issues:
- “MMU off means Device-nGnRnE” is wrong. With `SCTLR_EL1.M=0`, Normal-memory accesses are non-cacheable; UART’s Device mapping is a separate concern. The alignment discipline remains necessary, but that rationale should be corrected. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/b/aarch64-system-registers/sctlr_el1))
- `adr` is only ±1 MiB; use `adrp` or linker assertions. `b` is ±128 MiB.
- A bounded diagnostic requires bounded UART polling and a maximum output length, not merely a non-allocating formatter.
- `VBAR` readback proves register installation, not executable table contents; the BRK smoke marker supplies the latter.
- Ordinary SError is masked by `PSTATE.A` in this boot state, so vector 4 is the normal reachable path; retain all 16 entries because exceptional/unexpected routing must still land in known code. DAIF masking semantics are architectural. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-and-interrupt-handling))
My position: accept the table, frame shape, FAR policy, and BRK test; reject SP_EL0-as-scratch until its scratch-register and future lower-EL contracts are made explicit.
I agree with Claude on the 2 KiB table, `VBAR_EL1; isb`, vector 4 for Current EL/SP_ELx Sync, FAR=`n/a` for BRK, no `eret` path, and the frame offsets. FAR is architecturally defined for aborts, PC alignment, and watchpoints—not BRK. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/vbar_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/b/aarch64-system-registers/far_el1))
I dispute the proposed SP_EL0 scratch protocol:
- It is internally incomplete: SP_EL0 stores original `x0`, but the vector index is also proposed to live there. There is no free GPR for bounds checking while preserving `x1`–`x30`.
- It destroys the future lower-EL user stack. Lower-EL entry must preserve SP_EL0 before using it.
- A guard checked after `sub sp`/`stp` cannot detect failure of that first push; Claude correctly identifies the recursive-fault risk, but the replacement does not solve register preservation.
For MP1 I recommend the narrower contract: trust the boot-established, 16-byte-aligned SP_EL1 range; save `x0/x1` first, then set/check `in_handler`. Nested exceptions after that point take a stackless fatal path. Explicitly document that a pre-frame stack fault is outside the MP1 guarantee. A genuinely stackless precheck requires reserved per-CPU/system-register scratch or a separately designed exception stack.
The frame is otherwise sound:
```c
uint64_t x[31];   // x0..x30; no x31 exists to save
uint64_t sp;
uint64_t elr;
uint64_t spsr;
uint64_t esr;
uint64_t far;
uint64_t vector;
uint64_t pad;
```
The `sp` contract must distinguish interrupted SP_EL1 from SP_EL0/user SP for future entries. The field order and offsets are D5-3 ABI decisions and require owner sign-off. `x18`, `x29`, and `x30` are ordinary saved GPRs; compiler disassembly must verify the C ABI assumptions.
BRK remains the best deterministic test, but record higher-level debug routing as a prerequisite; BRK is a software breakpoint/debug exception, not merely an ordinary data fault. Assert defined ESR fields and the low 32 bits:
```text
EC   = 0x3c
IL   = 1
ISS  = 0x4a45
ESR[31:0] = 0xf2004a45
ELR  = address of the BRK instruction
vector = 4
(spsr & 0x3cf) == 0x3c5
```
Do not require the entire 64-bit ESR unless the implementation explicitly guarantees upper bits. Arm documents BRK’s `EC=0x3c` and immediate in ISS. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/a/aarch64-system-registers/esr_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/elr_el1))
Missed issues:
- “MMU off means Device-nGnRnE” is wrong. With `SCTLR_EL1.M=0`, Normal-memory accesses are non-cacheable; UART’s Device mapping is a separate concern. The alignment discipline remains necessary, but that rationale should be corrected. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/b/aarch64-system-registers/sctlr_el1))
- `adr` is only ±1 MiB; use `adrp` or linker assertions. `b` is ±128 MiB.
- A bounded diagnostic requires bounded UART polling and a maximum output length, not merely a non-allocating formatter.
- `VBAR` readback proves register installation, not executable table contents; the BRK smoke marker supplies the latter.
- Ordinary SError is masked by `PSTATE.A` in this boot state, so vector 4 is the normal reachable path; retain all 16 entries because exceptional/unexpected routing must still land in known code. DAIF masking semantics are architectural. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-and-interrupt-handling))
My position: accept the table, frame shape, FAR policy, and BRK test; reject SP_EL0-as-scratch until its scratch-register and future lower-EL contracts are made explicit.
[R2 CLAUDE] **Implementor response to Codex: MP1 ARM64 vectors and trap frame (D5-3)**

Most of Codex's points survive. I concede the SP_EL0 scratch protocol. I dispute the claim that a stackless precheck needs reserved scratch, and I refine three of its assertions.

### Agreed
- The 2 KiB table, `msr VBAR_EL1; isb`, all 16 entries present, vector 4 as the only normally reachable entry, no `eret` path, `adrp`+`b` instead of `adr`, and correcting the "MMU off means Device memory" rationale.
- **SP_EL0 scratch is withdrawn.** Codex is right that it has no free register for the vector index and that it collides with the future lower-EL SP_EL0 contract. The D5-3 frame should not bake in a scheme we have to undo at EL0 bring-up.
- Exception entry to EL1 sets PSTATE.{D,A,I,F}=1 in hardware. "DAIF stays masked in the handler" is architectural and needs no code. The rule is simply "never unmask in an MP1 handler".

### Disputed: a stackless precheck does not need reserved scratch
Linux arm64 `kernel_ventry` checks for stack overflow with no free register, using an add/sub swap:
```asm
sub  sp, sp, #FRAME_SIZE
add  sp, sp, x0          // sp' = sp + x0
sub  x0, sp, x0          // x0 = original sp
tbnz x0, #STACK_SHIFT, overflow
sub  x0, sp, x0          // x0 restored
sub  sp, sp, x0          // sp restored
```
- **Prerequisite:** the 16 KiB boot stack is aligned to 32 KiB in the linker script (`ASSERT`ed). A valid SP then has bit 14 clear, and an underflow of up to 16 KiB sets it.
- **Cost:** about 6 instructions, which fits in the 32-instruction (128-byte) entry.
- **Effect:** it closes the hole Codex says MP1 must leave open ("a pre-frame stack fault is outside the guarantee"), at negligible cost.
- **Overflow path only:** stash x0 in TPIDR_EL1, which is unused in MP1, then switch to a 4 KiB emergency stack.
- **Flag for later:** TPIDR_EL1 will become the per-CPU pointer at SMP. That reservation must be revisited then, and Linux uses TPIDR_EL0/TPIDRRO_EL0 for the same reason.

This gives three fatal tiers:
1. Normal path: the frame goes on SP_EL1, then C prints the diagnostic and halts.
2. Overflow, or nested entry while `in_handler` is set: switch to the emergency stack, print a minimal "nested/overflow" line plus the second ESR, then halt.
3. A fault while already on the emergency stack: a bare `wfe` loop with no output. This is still a bounded halt.

**Ordering hazard Codex missed:** a nested exception overwrites ELR, SPSR, ESR and FAR. The saver must copy those into the frame immediately after the first `stp x0, x1`, before setting `in_handler` and before any other work. Otherwise the first fault's state is lost exactly when it matters.

### Refinements to Codex's assertions
- **SPSR mask.** `0x3cf` skips M[4], so it would not detect AArch32 state. Use `(spsr & 0x3df) == 0x3c5`: DAIF set, M[4]=0 (AArch64), M[3:0]=EL1h.
- **One `sp` field is enough.** Its meaning is "SP of the interrupted context, as selected by SPSR.M". For the current-EL groups it is `sp + FRAME_SIZE`. For lower-EL groups it comes from `mrs sp_el0`. Defining it per vector group avoids a second field that is meaningless half the time. This is an ABI choice, so it goes to the owner.
- **ESR.** Agree on asserting the low 32 bits. On Cortex-A72 (v8.0) bits [63:32] are RES0, and RES0 is not a value to rely on. We can log the full 64 bits as an observation without making it part of the assertion.
- **ELR redaction and determinism.** Print ELR as an offset from the image base (`ELR - _start`), and assert `ELR == je_arm_brk_site`, a global label in the asm. Raw kernel addresses become sensitive once KASLR exists, and the offset is the same regardless of where QEMU loads the image. Same for FAR when it is valid.
- **Debug routing.** Agree it is a prerequisite. More precisely: BRK is not masked by PSTATE.D, but it routes to EL2 if EL2 exists with MDCR_EL2.TDE=1 or HCR_EL2.TGE=1. The smoke must pin `virtualization=off` (no EL2) and record that.

### Missed by both
- **SCTLR_EL1 must be written explicitly in boot.** Many of its bits reset to an architecturally UNKNOWN value, including SA, the SP-alignment check. Boot should write a defined value (M=0, C=0, SA=1, A per policy). Otherwise whether a misaligned `stp` faults is not deterministic. If step 1 did not do this, it is a step-1 gap.
- **No I-cache maintenance needed.** The table is static image content loaded by QEMU and nothing modifies it at runtime. `isb` after the VBAR write is sufficient.
- **The guard needs no atomics.** `in_handler` is a plain byte store: single CPU, no reentrancy before the flag is set, and the MMU off is fine for a Normal non-cacheable store. That fits the no-exclusives constraint.

### Frame
I accept Codex's layout:

| Offset | Field |
|---|---|
| 0 | `x[31]` |
| 248 | `sp` |
| 256 | `elr` |
| 264 | `spsr` |
| 272 | `esr` |
| 280 | `far` |
| 288 | `vector` |
| 296 | `pad` |

Size is 304 bytes, which is 16-aligned, and `elr`/`spsr` form a 16-aligned `stp` pair. Offsets live in one shared header used by both the asm and the C code, checked with `_Static_assert(offsetof(...))`. The field order is D5-3 ABI and needs owner sign-off.

### Bounded diagnostic
- At most 256 bytes, written through the existing bounded-poll PL011 path. On TX timeout the output is dropped and the handler still halts.
- Fields: vector, EC, IL, ISS, ELR offset, SPSR, FAR (printed as `n/a` unless the EC defines it), and `VBAR == &table`.
- VBAR readback proves the register was installed. The BRK marker proves the table contents execute. Both are needed.

**Position:** adopt Codex's frame and BRK assertions with the 0x3df mask, use the stack-alignment overflow precheck instead of SP_EL0 scratch, reserve TPIDR_EL1 for the fatal path only until SMP, and add the explicit SCTLR_EL1 write.
[R2 CODEX] codex
I agree with the broad direction: install all 16 slots, use no SP_EL0 scratch protocol, keep handlers non-returning, add VBAR readback, and use `adrp`/`add`/`br`-style position-independent reachability. The 32-KiB alignment precheck is valid only if the linker and stack invariant are explicit and asserted.
I dispute or qualify these points:
- `SPSR == 0x3c5` is test-specific, not an architectural handler invariant. `SPSR_EL1` captures pre-exception PSTATE; exception entry then masks DAIF. It is `0x3c5` here only because boot deliberately has DAIF masked and execution is EL1h. Assert `M[4:0]` and expected DAIF separately. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling))
- BRK is a valid trigger even with D set: Arm documents software breakpoint exceptions as synchronous and unmaskable. The remaining routing prerequisite is proving execution remains at EL1, not merely assuming QEMU’s virtualization setting. ([developer.arm.com](https://developer.arm.com/docs/den0024/a/debug/arm-debug-hardware/debug-events))
- The overflow scheme is not yet a complete nesting design. `in_handler` must be set before any potentially faulting access, and a second flag or explicit SP-range test is required to distinguish “nested on normal stack” from “nested on emergency stack.” Otherwise the claimed bare-WFE tier is unspecified.
- The emergency path using `TPIDR_EL1` is acceptable only as a temporary MP1 contract. It destroys any existing per-CPU value and therefore cannot be described as future-proof; SMP must redesign this path before assigning TPIDR_EL1 as the CPU pointer.
- “Copy system registers immediately after the first `stp`” does not preserve the first frame if the first stack access or subsequent store faults. It only makes the common path less vulnerable. If nested-fault preservation is required, the emergency scratch protocol must be specified; otherwise document nested faults as fatal with partial first-frame state.
- The frame description is internally ambiguous. `x[31]` means x0–x30, but the prose also says “a slot for x31/alignment.” Those are different layouts. Either use `gpr[31]` with no x31 slot, or define a 32-slot register area and freeze the larger ABI. This is a D5-3 owner decision.
- No mechanism transports the vector index into a common saver. A plain `b common_saver` loses the originating slot. Each entry needs a small index-bearing stub, or the saver must derive the slot from a preserved entry PC.
- The stack precheck catches crossing the designated 16-KiB boundary; it does not validate arbitrary corrupted SP values. State it as a bounded invariant check, not general stack safety.
- “Normal non-cacheable store” is the wrong MMU-off rationale. The relevant memory type must be stated from the configured architectural regime; do not rely on an unverified cacheability claim.
- Explicit SCTLR initialization is a legitimate prerequisite, but write a value respecting RES1/RES0 fields; “write zero except SA/M/C” is unsafe.
For the BRK test, use `brk #0xA55A` and assert:
```text
EC       == 0x3c
IL       == 1
ISS[15:0] == 0xA55A
ESR_EL1[31:0] == 0xF200A55A
ELR_EL1 == &je_arm_brk_site
```
Also assert the handler’s VBAR readback equals the aligned table address and that the observed vector index is the EL1h synchronous slot (index 4). The exact frame field order and whether emergency nesting is required beyond bounded halt remain owner-escalated contract choices.
I agree with the broad direction: install all 16 slots, use no SP_EL0 scratch protocol, keep handlers non-returning, add VBAR readback, and use `adrp`/`add`/`br`-style position-independent reachability. The 32-KiB alignment precheck is valid only if the linker and stack invariant are explicit and asserted.
I dispute or qualify these points:
- `SPSR == 0x3c5` is test-specific, not an architectural handler invariant. `SPSR_EL1` captures pre-exception PSTATE; exception entry then masks DAIF. It is `0x3c5` here only because boot deliberately has DAIF masked and execution is EL1h. Assert `M[4:0]` and expected DAIF separately. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling))
- BRK is a valid trigger even with D set: Arm documents software breakpoint exceptions as synchronous and unmaskable. The remaining routing prerequisite is proving execution remains at EL1, not merely assuming QEMU’s virtualization setting. ([developer.arm.com](https://developer.arm.com/docs/den0024/a/debug/arm-debug-hardware/debug-events))
- The overflow scheme is not yet a complete nesting design. `in_handler` must be set before any potentially faulting access, and a second flag or explicit SP-range test is required to distinguish “nested on normal stack” from “nested on emergency stack.” Otherwise the claimed bare-WFE tier is unspecified.
- The emergency path using `TPIDR_EL1` is acceptable only as a temporary MP1 contract. It destroys any existing per-CPU value and therefore cannot be described as future-proof; SMP must redesign this path before assigning TPIDR_EL1 as the CPU pointer.
- “Copy system registers immediately after the first `stp`” does not preserve the first frame if the first stack access or subsequent store faults. It only makes the common path less vulnerable. If nested-fault preservation is required, the emergency scratch protocol must be specified; otherwise document nested faults as fatal with partial first-frame state.
- The frame description is internally ambiguous. `x[31]` means x0–x30, but the prose also says “a slot for x31/alignment.” Those are different layouts. Either use `gpr[31]` with no x31 slot, or define a 32-slot register area and freeze the larger ABI. This is a D5-3 owner decision.
- No mechanism transports the vector index into a common saver. A plain `b common_saver` loses the originating slot. Each entry needs a small index-bearing stub, or the saver must derive the slot from a preserved entry PC.
- The stack precheck catches crossing the designated 16-KiB boundary; it does not validate arbitrary corrupted SP values. State it as a bounded invariant check, not general stack safety.
- “Normal non-cacheable store” is the wrong MMU-off rationale. The relevant memory type must be stated from the configured architectural regime; do not rely on an unverified cacheability claim.
- Explicit SCTLR initialization is a legitimate prerequisite, but write a value respecting RES1/RES0 fields; “write zero except SA/M/C” is unsafe.
For the BRK test, use `brk #0xA55A` and assert:
```text
EC       == 0x3c
IL       == 1
ISS[15:0] == 0xA55A
ESR_EL1[31:0] == 0xF200A55A
ELR_EL1 == &je_arm_brk_site
```
Also assert the handler’s VBAR readback equals the aligned table address and that the observed vector index is the EL1h synchronous slot (index 4). The exact frame field order and whether emergency nesting is required beyond bounded halt remain owner-escalated contract choices.

---
## MP1 vectors diff adversarial review (core verified correct via objdump + ARM ARM; drove FnV/double-fault-asm/raw-smoke-assert/doc fixes)
### Claude
No defects that would break MP1 as it stands. There are two hazards worth fixing before the trap-frame ABI is relied on, plus a few minor ones. I built `ARM_VECTOR_TEST=1` and checked the claims against `objdump` of `build/aarch64-vectest/jeios-arm.elf`. I did not re-run QEMU.

### Findings

1. **The frozen ABI says "x0..x30", but slots 16–18 don't hold x16–x18** (`trapframe.h:11-14,38` and `vectors.S:113-118`). The vector index recovery itself is correct. `mov w16,#idx` zero-extends into x16, and nothing writes x16 before the `stp x16,x17`, so the `and #0xffffffff` is redundant but harmless. In the saved frame, though, `gpr[16]` is the vector index, `gpr[17]` is `&je_arm_in_handler` and `gpr[18]` is 1.
   - The justification in the comment is wrong. Linker veneers may clobber IP0/IP1 (x16/x17) only at call boundaries. An exception can land on any instruction, and x18 is the platform register.
   - For a halting MP1 handler this doesn't matter. Once MP3 adds the fixup path (or IRQ return), restoring `gpr[]` would silently corrupt live x16–x18.
   - The offsets are frozen, so this goes to the owner. Either write the deviation into `trapframe.h` as part of the contract now, or change the saver before MP3 (for example, park a scratch register in `TPIDR_EL1`, or spill x16/x17 before using them).

2. **`je_arm_double_fault` depends on the compiler not using the stack** (`main.c` `je_arm_double_fault`). At `-O2` the disassembly (0x400811b0–0x400811dc) shows no stack use. Nothing guarantees that, though: `-O0` or a flag change would add a prologue that touches SP. Here's how that goes wrong:
   - Exception entry does not check or realign SP. With SA=1, code can hold a misaligned SP left by arithmetic and take an unrelated exception (e.g. BRK) before any SP-relative access.
   - The saver's first `stp` then faults (EC 0x26), and the guard sends it to `double_fault`.
   - If `double_fault` touches the stack, it faults again and loops forever with no marker.
   
   That answers your Q3: such a path exists. It is only bounded while `double_fault` stays stack-free. Write it in asm in `vectors.S`, or add a build check. Also, it writes the PL011 data register without polling TXFF, which works on QEMU but would drop characters on real hardware.

3. **`ec_far_valid` misses FnV** (`main.c` `ec_far_valid`). For data aborts (EC 0x25), FAR is only valid when ISS.FnV (bit 10) is 0. A synchronous external abort can set FnV=1, and the handler would then print an UNKNOWN FAR as if it were valid. Two smaller points:
   - Lower-EL aborts (0x20/0x24) and watchpoints aren't covered; they can't happen until EL0 exists.
   - `EC_UNKNOWN`, `EC_SVC64`, `EC_SP_ALIGN` and `EC_BRK64` are unused. The comment says the diagnostic "names explicitly" these ECs, but it only prints hex.

4. **`je_arm_brk_site` is a local symbol** (`main.c` `trigger_brk`; symtab shows `l .text`). It links only because the reference is in the same file. `trigger_brk` was inlined into `je_arm_main`. GCC's documentation warns that asm statements can be duplicated, and a duplicated label is a loud assembler error rather than a silent bug. Adding `.globl` doesn't prevent duplication; mark `trigger_brk` `noinline,noclone`, or move the label into a `.S` file.

5. **Stale and wrong comments in `boot.S`:**
   - Line 85 still says "SCTLR_EL1.M untouched", and the header list (lines 7-17) doesn't mention the new SCTLR init.
   - Pre-existing, not from this diff: lines 85-87 say TCR_EL1 bit 31 is RES1. It isn't; TCR_EL1[31:30] is TG1, so 0x80000000 sets a 4 KB TTBR1 granule. Harmless, but the claim is false.

### Confirmed clean

- **Q5, table layout:** each slot N sits exactly at `0x40080800 + N*0x80`. Every `mov w16,#N` was found at 0x800, 0x880, … 0xf80. The first `.balign 0x80` does nothing on the 0x800-aligned base, so there is no offset bug.
- **Q2, guard:** I found no window. With the MMU off, the `adrp`/`ldrb`/`strb` on `.data` and the instruction fetch can't fault, and entry masks D/A/I/F. Never clearing `in_handler` is correct for non-returning handlers. A future returning handler must clear it; add that sentence to the `vectors.S` contract.
- **Q3, alignment otherwise:** `__stack_top` is asserted 16-aligned and 304 is a multiple of 16, so the frame is aligned.
- **Q4, SCTLR value:** 0x30D00808 is bits 3, 11, 20, 22, 23, 28 and 29. M, A, C and I are all 0, SA=1, and bits 11/20/22/23/28/29 are the correct ARMv8.0 RES1 set (EOS, TSCXT, EIS, SPAN, nTLSMD, LSMAOE). No stray enables. SA0=0 means EL0 SP alignment isn't checked, which the EL0 work needs to revisit.
- **Q6, other exceptions:** non-BRK synchronous exceptions all land in slot 4, and printing the EC then halting is fine. A mismatch prints no PASS marker, so the smoke harness fails.
- **Q8, `-Wconversion`:** no signedness or truncation issues.
- **Q9, cache maintenance:** none needed. The MMU and both caches are off, the table is unmodified image content, and the ISB after the VBAR write is enough.

**Hazard (not a defect):** `je_arm_in_handler` sits at 0x40081788, directly below `.bss` and the 16 KiB stack, with no guard page while the MMU is off. A stack overflow during handling would corrupt `.bss` and then `.data`, including the guard byte. Record this in the handoff.

All of this is AI-assisted review; it doesn't count as independent human qualification.

### Codex
codex
I’ll verify the implementation and linker output directly, then cross-check the ARM architectural points that are easy to get subtly wrong. I’ll keep the result to numbered findings with exact file/line references and distinguish real defects from acceptable MP1 limitations.
codex
The repository reads are being blocked by the sandbox’s network-namespace setup before the commands start, so I can’t inspect the checked-out files yet. I’m requesting a read-only escalated shell for the same commands; no files will be changed.
exec
/usr/bin/bash -lc "sed -n '1,220p' README.md; sed -n '1,260p' docs/HANDOFF.md; sed -n '1,260p' docs/ROADMAP.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,260p' docs/AGENT_START.md; sed -n '1,320p' docs/DESIGN.md; sed -n '1,280p' docs/CODING_STANDARD.md" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba kernel/arch/aarch64/vectors.S; nl -ba kernel/arch/aarch64/main.c | sed -n '1,360p'; nl -ba kernel/arch/aarch64/boot.S | sed -n '1,190p'; nl -ba kernel/arch/aarch64/linker.ld; nl -ba kernel/arch/aarch64/trapframe.h" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "git status --short; rg -n \"SCTLR|vectors|trapframe|in_handler|JE_ARM_VECTOR|brk_site|VBAR\" -S . --glob '"'!build/**'"' --glob '"'!*.o'"' --glob '"'!*.elf'"'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * MP1 (ADR 0002 D5-3 + ARM64.md MP1): the AArch64 EL1 exception vector table and
     4	 * the common trap-frame saver. Design frozen by the owner-approved vectors pass.
     5	 *
     6	 * VBAR_EL1 points here. The table is 16 entries of 0x80 (128) bytes each, 4
     7	 * groups of 4 — {Cur EL SP_EL0, Cur EL SP_ELx, Lower EL AArch64, Lower EL
     8	 * AArch32} x {Sync, IRQ, FIQ, SError} — and MUST be 2 KiB aligned (VBAR_EL1[10:0]
     9	 * are RES0). We install ALL 16 even though MP1 only legitimately reaches "Current
    10	 * EL with SP_ELx, Synchronous" (no EL0 yet, IRQ/FIQ masked via DAIF, no lower EL,
    11	 * SError possible from a bad access): an unexpected vector must still land on a
    12	 * defined bounded halt, never execute garbage.
    13	 *
    14	 * Each 0x80 slot is a tiny INDEX-BEARING stub (`mov w16,#idx; b je_arm_vec_common`)
    15	 * so the common saver knows which of the 16 fired — a plain `b` would lose it.
    16	 * The saver builds a je_arm_trapframe (trapframe.h, FROZEN offsets) on the
    17	 * current EL1 stack and calls je_arm_exception(tf).
    18	 *
    19	 * MP1 reentrancy contract (ARM64.md "declare reentrancy assumptions"): single
    20	 * kernel stack, handlers are NON-reentrant and NON-returning. DAIF stays masked
    21	 * throughout (exception entry masks it; we never unmask), so no async exception
    22	 * nests. A NESTED SYNCHRONOUS fault (a fault while already in a handler) is a
    23	 * FATAL double-fault -> bounded DR-only halt via je_arm_double_fault; the frame
    24	 * of the SECOND fault is only partially trustworthy, documented, not recovered.
    25	 * No allocating fault handler. MMU is off in MP1 so table VA==PA (flat); the
    26	 * table is static image content (no I-cache maintenance needed; the ISB after
    27	 * the VBAR write in C is sufficient).
    28	 *
    29	 * Built -mstrict-align -mgeneral-regs-only: the saver uses only GPR stp/ldr and
    30	 * aligned SP, no FP/SIMD, no unaligned, no atomics/exclusives.
    31	 */
    32	#include "trapframe.h"
    33	
    34	/* One vector slot: stamp the index into w16 and branch to the common saver.
    35	 * `.balign 0x80` pads each entry to the architectural 128-byte stride. */
    36	.macro VEC idx
    37	    .balign 0x80
    38	    mov     w16, #\idx
    39	    b       je_arm_vec_common
    40	.endm
    41	
    42	    .section .text.vectors, "ax"
    43	    .balign 0x800                 /* VBAR_EL1 2 KiB alignment (bits[10:0] RES0) */
    44	    .globl  je_arm_vectors
    45	je_arm_vectors:
    46	    /* Group 0: Current EL with SP_EL0 (not used at EL1h, but defined). */
    47	    VEC 0    /* Sync   */
    48	    VEC 1    /* IRQ    */
    49	    VEC 2    /* FIQ    */
    50	    VEC 3    /* SError */
    51	    /* Group 1: Current EL with SP_ELx  (EL1h — the reachable group in MP1). */
    52	    VEC 4    /* Sync   <- BRK / undefined / data abort land here */
    53	    VEC 5    /* IRQ    (masked in MP1) */
    54	    VEC 6    /* FIQ    (masked in MP1) */
    55	    VEC 7    /* SError */
    56	    /* Group 2: Lower EL using AArch64 (no EL0 yet in MP1). */
    57	    VEC 8    /* Sync   */
    58	    VEC 9    /* IRQ    */
    59	    VEC 10   /* FIQ    */
    60	    VEC 11   /* SError */
    61	    /* Group 3: Lower EL using AArch32 (unsupported; defined halt). */
    62	    VEC 12   /* Sync   */
    63	    VEC 13   /* IRQ    */
    64	    VEC 14   /* FIQ    */
    65	    VEC 15   /* SError */
    66	
    67	    .section .text, "ax"
    68	    .globl  je_arm_vec_common
    69	    .extern je_arm_exception
    70	    .extern je_arm_double_fault
    71	    .extern je_arm_in_handler
    72	je_arm_vec_common:
    73	    /* ---- Reentrancy guard FIRST, before any stack access that could itself
    74	     * fault: if je_arm_in_handler != 0 we are already handling an exception ->
    75	     * nested/double fault -> fatal bounded halt. Set it to 1 otherwise. This is
    76	     * a plain byte load/store (single CPU, MMU-off Normal access, no atomics). */
    77	    adrp    x17, je_arm_in_handler
    78	    add     x17, x17, #:lo12:je_arm_in_handler
    79	    ldrb    w18, [x17]
    80	    cbnz    w18, je_arm_double_fault
    81	    mov     w18, #1
    82	    strb    w18, [x17]
    83	
    84	    /* ---- Build the trap frame on the current (EL1) stack. Reserve JE_TF_SIZE
    85	     * (304, 16-aligned) and fill it to the trapframe.h layout. ---- */
    86	    sub     sp, sp, #JE_TF_SIZE
    87	    /* x0..x30 -> gpr[0..30] (offset 0). stp in pairs; x30 alone last. */
    88	    stp     x0,  x1,  [sp, #(JE_TF_GPR + 0)]
    89	    stp     x2,  x3,  [sp, #(JE_TF_GPR + 16)]
    90	    stp     x4,  x5,  [sp, #(JE_TF_GPR + 32)]
    91	    stp     x6,  x7,  [sp, #(JE_TF_GPR + 48)]
    92	    stp     x8,  x9,  [sp, #(JE_TF_GPR + 64)]
    93	    stp     x10, x11, [sp, #(JE_TF_GPR + 80)]
    94	    stp     x12, x13, [sp, #(JE_TF_GPR + 96)]
    95	    stp     x14, x15, [sp, #(JE_TF_GPR + 112)]
    96	    stp     x16, x17, [sp, #(JE_TF_GPR + 128)]
    97	    stp     x18, x19, [sp, #(JE_TF_GPR + 144)]
    98	    stp     x20, x21, [sp, #(JE_TF_GPR + 160)]
    99	    stp     x22, x23, [sp, #(JE_TF_GPR + 176)]
   100	    stp     x24, x25, [sp, #(JE_TF_GPR + 192)]
   101	    stp     x26, x27, [sp, #(JE_TF_GPR + 208)]
   102	    stp     x28, x29, [sp, #(JE_TF_GPR + 224)]
   103	    str     x30,      [sp, #(JE_TF_GPR + 240)]
   104	
   105	    /* Faulting SP = the value BEFORE we subtracted the frame. */
   106	    add     x0, sp, #JE_TF_SIZE
   107	    str     x0, [sp, #JE_TF_SP]
   108	
   109	    /* D5-3 system registers: ELR, SPSR, ESR, FAR. */
   110	    mrs     x0, ELR_EL1
   111	    mrs     x1, SPSR_EL1
   112	    stp     x0, x1, [sp, #JE_TF_ELR]      /* ELR @256, SPSR @264 (aligned pair) */
   113	    mrs     x0, ESR_EL1
   114	    mrs     x1, FAR_EL1
   115	    stp     x0, x1, [sp, #JE_TF_ESR]      /* ESR @272, FAR @280 (aligned pair) */
   116	
   117	    /* Vector index: the slot stub put it in w16, and we saved x16 to gpr[16]
   118	     * above BEFORE anything clobbered x16, so gpr[16]'s low word is the index.
   119	     * (Honest caveat: gpr[17]/gpr[18] hold this stub's guard scratch, NOT the
   120	     * pre-exception x17/x18 — x16-x18 are IP0/IP1/scratch, conventionally
   121	     * clobberable by an exception veneer; irrelevant to the MP1 diagnostic.) */
   122	    ldr     x0, [sp, #(JE_TF_GPR + 128)]  /* saved x16 = {idx in low 32} */
   123	    and     x0, x0, #0xffffffff
   124	    str     x0, [sp, #JE_TF_VECTOR]
   125	    str     xzr, [sp, #JE_TF_PAD]
   126	
   127	    /* Call the C handler with a pointer to the frame. It does not return. */
   128	    mov     x0, sp
   129	    bl      je_arm_exception
   130	    /* je_arm_exception is _Noreturn; if it ever returns, halt hard. */
   131	1:  wfe
   132	    b       1b
   133	
   134	.section .note.GNU-stack, "", @progbits
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * MP1 step 1 (ADR 0002): the C entry for the ARM64 (QEMU virt-8.2) bring-up.
     4	 * Reached from boot.S after EL1 is confirmed, the stack is set, TCR_EL1 is
     5	 * initialized (TBI0=0 interim) and BSS is cleared. This file drives a bounded
     6	 * PL011 polling console and emits the ordered step-1 markers, then halts.
     7	 *
     8	 * Scope boundaries (ADR 0002 / MP1 decomposition, locked by the design pass):
     9	 *   - NO VBAR / exception vectors yet (next PR);
    10	 *   - NO DTB *parse* — the pointer is only RECORDED, never dereferenced;
    11	 *   - NO MMU, no atomics; single CPU.
    12	 * The handoff records that the D5 trap-frame fields, VBAR, fault fixups, and DTB
    13	 * validation are not yet enforced.
    14	 *
    15	 * Built -mstrict-align -mgeneral-regs-only -ffreestanding -fno-builtin, so the
    16	 * compiler emits neither unaligned accesses (Device memory pre-MMU faults them)
    17	 * nor FP/SIMD (which would trap into absent vectors and hang silently).
    18	 */
    19	#include <stdint.h>
    20	#include "trapframe.h"
    21	
    22	/* MP1 reentrancy guard (vectors.S): 0 outside a handler, 1 inside. A nested
    23	 * synchronous fault (fault while handling) trips je_arm_double_fault. Plain byte,
    24	 * single CPU, no atomics (ADR 0002: no exclusives pre-MMU). In .data so it is a
    25	 * defined 0 at boot without depending on the BSS clear order. */
    26	volatile uint8_t je_arm_in_handler __attribute__((section(".data"))) = 0u;
    27	
    28	/* DTB physical address, stashed by boot.S into .data BEFORE the BSS clear so it
    29	 * survives (a .bss slot would have been zeroed). Recorded, not parsed. */
    30	uint64_t je_arm_dtb_phys __attribute__((section(".data"))) = 0;
    31	
    32	/* ---- PL011 UART @ 0x09000000 (confirmed from the QEMU virt DTB). Bounded,
    33	 * DR-write + TXFF-poll console. No IBRD/FBRD/LCR/CR init: on this pinned target
    34	 * QEMU accepts DR writes without programming, which is a documented virt-8.2
    35	 * support-tier assumption, not a portable UART driver. The TXFF poll is bounded
    36	 * so a wedged UART can never block the boot forever. ---- */
    37	#define PL011_BASE 0x09000000UL
    38	#define PL011_DR   (PL011_BASE + 0x00)
    39	#define PL011_FR   (PL011_BASE + 0x18)
    40	#define PL011_FR_TXFF (1u << 5)   /* transmit FIFO full */
    41	
    42	static inline void mmio_w32(uint64_t addr, uint32_t v)
    43	{
    44	    *(volatile uint32_t *)(uintptr_t)addr = v;
    45	}
    46	static inline uint32_t mmio_r32(uint64_t addr)
    47	{
    48	    return *(volatile uint32_t *)(uintptr_t)addr;
    49	}
    50	
    51	static void console_putc(char c)
    52	{
    53	    /* Bounded wait for TX space: spin at most N times on TXFF. If it never
    54	     * clears (a wedged UART), fall through and write DR anyway — a real PL011
    55	     * would then drop the byte, so the expected marker would be missing and the
    56	     * smoke harness FAILs. The point of the bound is only that a wedged device
    57	     * can never block the boot forever (no success-via-timeout). */
    58	    for (unsigned i = 0; i < 100000u; ++i) {
    59	        if ((mmio_r32(PL011_FR) & PL011_FR_TXFF) == 0u) break;
    60	    }
    61	    mmio_w32(PL011_DR, (uint32_t)(unsigned char)c);
    62	}
    63	
    64	static void console_write(const char *s)
    65	{
    66	    /* Bounded length so a missing NUL cannot run off into unmapped MMIO. */
    67	    for (unsigned i = 0; i < 4096u && s[i] != '\0'; ++i) console_putc(s[i]);
    68	}
    69	
    70	/* Write a 64-bit value as 0x-prefixed hex (fixed 16 digits). */
    71	static void console_hex(uint64_t v)
    72	{
    73	    static const char d[] = "0123456789abcdef";
    74	    char buf[19];
    75	    buf[0] = '0'; buf[1] = 'x';
    76	    for (int i = 0; i < 16; ++i) buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
    77	    buf[18] = '\0';
    78	    console_write(buf);
    79	}
    80	
    81	_Noreturn static void halt(void)
    82	{
    83	    for (;;) __asm__ volatile("wfe");
    84	}
    85	
    86	/* Read-back helpers to make the markers evidence, not assertion. */
    87	static inline uint64_t read_currentel(void)
    88	{
    89	    uint64_t v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return (v >> 2) & 3u;
    90	}
    91	static inline uint64_t read_tcr_el1(void)
    92	{
    93	    uint64_t v; __asm__ volatile("mrs %0, TCR_EL1" : "=r"(v)); return v;
    94	}
    95	static inline uint64_t read_sp(void)
    96	{
    97	    uint64_t v; __asm__ volatile("mov %0, sp" : "=r"(v)); return v;
    98	}
    99	static inline uint64_t read_vbar_el1(void)
   100	{
   101	    uint64_t v; __asm__ volatile("mrs %0, VBAR_EL1" : "=r"(v)); return v;
   102	}
   103	
   104	/* ---- MP1 (ADR 0002 D5-3): exception vectors. je_arm_vectors is the 2 KiB-aligned
   105	 * 16-slot table in vectors.S; install it into VBAR_EL1 and ISB so the write takes
   106	 * before any subsequent exception. No I-cache maintenance: the table is static
   107	 * image content nothing modifies. ---- */
   108	extern uint8_t je_arm_vectors[];
   109	static void install_vectors(void)
   110	{
   111	    __asm__ volatile("msr VBAR_EL1, %0; isb" :: "r"((uint64_t)(uintptr_t)je_arm_vectors) : "memory");
   112	}
   113	
   114	/* AArch64 ESR_EL1 field extracts (DDI0487): EC[31:26], IL[25], ISS[24:0]. */
   115	#define ESR_EC(esr)  (((esr) >> 26) & 0x3fu)
   116	#define ESR_IL(esr)  (((esr) >> 25) & 0x1u)
   117	#define ESR_ISS(esr) ((esr) & 0x1ffffffu)
   118	/* EC values this diagnostic names explicitly. */
   119	#define EC_UNKNOWN   0x00u   /* undefined/unknown instruction */
   120	#define EC_SVC64     0x15u   /* SVC from AArch64 (syscall; not a fault) */
   121	#define EC_IABT_CUR  0x21u   /* instruction abort, current EL */
   122	#define EC_PC_ALIGN  0x22u   /* PC alignment fault */
   123	#define EC_DABT_CUR  0x25u   /* data abort, current EL */
   124	#define EC_SP_ALIGN  0x26u   /* SP alignment fault */
   125	#define EC_BRK64     0x3cu   /* BRK instruction (AArch64) */
   126	/* Whether FAR_EL1 is architecturally meaningful for this EC (abort/alignment). */
   127	static int ec_far_valid(uint64_t ec)
   128	{
   129	    return ec == EC_IABT_CUR || ec == EC_DABT_CUR || ec == EC_PC_ALIGN;
   130	}
   131	
   132	/* MP1 common C exception handler: bounded redacted diagnostic + halt. No
   133	 * recovery, no allocation (ARM64.md). "Redacted" = we print the syndrome fields
   134	 * (vector, EC, IL, ISS, ELR, SPSR, FAR-if-valid) and a VBAR readback, NOT the
   135	 * full GPR dump — a fault diagnostic names WHERE and WHAT, not register
   136	 * contents. Marker strings are fixed so the smoke harness can assert them. */
   137	_Noreturn void je_arm_exception(je_arm_trapframe *tf)
   138	{
   139	    uint64_t ec = ESR_EC(tf->esr);
   140	    console_write("JeiOS/aarch64: EXCEPTION vector=");
   141	    console_hex(tf->vector);
   142	    console_write(" ec=");
   143	    console_hex(ec);
   144	    console_write(" il=");
   145	    console_hex(ESR_IL(tf->esr));
   146	    console_write(" iss=");
   147	    console_hex(ESR_ISS(tf->esr));
   148	    console_write("\r\n");
   149	    console_write("JeiOS/aarch64:   esr=");
   150	    console_hex(tf->esr);
   151	    console_write(" elr=");
   152	    console_hex(tf->elr);
   153	    console_write(" spsr=");
   154	    console_hex(tf->spsr);
   155	    console_write("\r\n");
   156	    console_write("JeiOS/aarch64:   far=");
   157	    if (ec_far_valid(ec)) console_hex(tf->far);
   158	    else                  console_write("n/a");
   159	    console_write(" vbar=");
   160	    console_hex(read_vbar_el1());        /* evidence the table was installed */
   161	    console_write("\r\n");
   162	#ifdef JE_ARM_VECTOR_TEST
   163	    /* Deliberate-exception self-test: the ONLY exception we ever expect is the
   164	     * planted BRK #0xA55A. Assert the exact syndrome end-to-end:
   165	     *   EC == 0x3c (BRK64), IL == 1, ISS[15:0] == 0xA55A  => ESR == 0xF200A55A,
   166	     *   vector == 4 (Current EL SP_ELx, Synchronous), ELR == &je_arm_brk_site.
   167	     * A distinctive PASS marker precedes the halt so the harness asserts success
   168	     * (not timeout). SPSR is config-specific (M[4:0]=0b0101 EL1h + DAIF masked =
   169	     * 0x3c5 here) so we check M[4:0] and DAIF separately, not the whole word. */
   170	    extern uint8_t je_arm_brk_site[];
   171	    uint64_t spsr_m = tf->spsr & 0x1fu;          /* mode field */
   172	    uint64_t spsr_daif = (tf->spsr >> 6) & 0xfu; /* D,A,I,F */
   173	    if (tf->vector == 4u && tf->esr == 0xF200A55Aull &&
   174	        tf->elr == (uint64_t)(uintptr_t)je_arm_brk_site &&
   175	        spsr_m == 0x5u /* EL1h */ && spsr_daif == 0xfu /* all masked */) {
   176	        console_write("JeiOS/aarch64: vector self-test PASS (planted BRK #0xa55a vectored to EL1h sync slot 4 with exact ESR 0xf200a55a; ELR==brk site; SPSR EL1h+DAIF-masked)\r\n");
   177	    } else {
   178	        console_write("JeiOS/aarch64: FATAL vector self-test MISMATCH\r\n");
   179	    }
   180	#endif
   181	    console_write("JeiOS/aarch64: exception handled; halted\r\n");
   182	    for (;;) __asm__ volatile("wfe");
   183	}
   184	
   185	/* Nested/double synchronous fault: we re-entered the saver with je_arm_in_handler
   186	 * already set. The second frame is not trustworthy, so bypass the normal path and
   187	 * write a bounded DR-only marker (no stack, no re-entry) then halt. Called
   188	 * directly from vectors.S before any frame is built. */
   189	_Noreturn void je_arm_double_fault(void)
   190	{
   191	    volatile uint32_t *dr = (volatile uint32_t *)0x09000000UL;  /* PL011 DR */
   192	    static const char msg[] = "JeiOS/aarch64: FATAL double fault; halted\r\n";
   193	    for (unsigned i = 0; msg[i] != '\0' && i < 128u; ++i) *dr = (uint32_t)(unsigned char)msg[i];
   194	    for (;;) __asm__ volatile("wfe");
   195	}
   196	
   197	#ifdef JE_ARM_VECTOR_TEST
   198	/* The planted deliberate exception. je_arm_brk_site is the exact faulting PC the
   199	 * handler asserts ELR_EL1 equals. BRK is a synchronous, DAIF-unmaskable software
   200	 * breakpoint (DDI0487): it vectors to the EL1h sync slot even though we never
   201	 * unmasked interrupts. imm16 = 0xA55A lands in ESR ISS[15:0]. */
   202	static void trigger_brk(void)
   203	{
   204	    __asm__ volatile("je_arm_brk_site: brk #0xA55A");
   205	}
   206	#endif
   207	
   208	/* Optional patterned-BSS self-check: in the dedicated test build the boot.S
   209	 * entry fills BSS with a nonzero pattern BEFORE the production clear runs, and
   210	 * this probe (defined below in .bss) is then checked to confirm the clear zeroed
   211	 * it — rather than relying on QEMU's already-zero RAM (which would make the
   212	 * claim tautological). */
   213	
   214	/* A probe in .bss so the section is non-empty in EVERY build (otherwise the
   215	 * clear loop runs zero times and "bss cleared" proves nothing). In the
   216	 * patterned-BSS build (boot.S pre-fills BSS) this same probe is checked to be
   217	 * zero after the production clear. Linker symbols bound the cleared range, which
   218	 * we print as evidence. */
   219	volatile uint64_t je_arm_bss_probe;   /* in .bss */
   220	extern uint8_t __bss_start[], __bss_end[];
   221	
   222	_Noreturn void je_arm_main(void)
   223	{
   224	    uint64_t el = read_currentel();
   225	    /* Single-digit EL so the marker reads "entry EL1" exactly (EL is 0..3). */
   226	    char elc = (char)('0' + (int)(el & 3u));
   227	    console_write("JeiOS/aarch64: entry EL");
   228	    console_putc(elc);
   229	    console_write("\r\n");
   230	
   231	    console_write("JeiOS/aarch64: stack ready sp=");
   232	    console_hex(read_sp());
   233	    console_write("\r\n");
   234	
   235	    console_write("JeiOS/aarch64: tcr_el1 initialized (tbi0=0) tcr=");
   236	    console_hex(read_tcr_el1());
   237	    console_write("\r\n");
   238	
   239	    /* Evidence: print the cleared range so "bss cleared" is not vacuous even in
   240	     * the default build (the probe above guarantees the range is non-empty). */
   241	    console_write("JeiOS/aarch64: bss range [");
   242	    console_hex((uint64_t)(uintptr_t)__bss_start);
   243	    console_write(",");
   244	    console_hex((uint64_t)(uintptr_t)__bss_end);
   245	    console_write(")\r\n");
   246	#ifdef JE_ARM_BSS_CHECK
   247	    /* The clear has already run (boot.S) after the stub pre-filled BSS; prove it
   248	     * actually zeroed the .bss probe that was pattern-filled. */
   249	    if (je_arm_bss_probe != 0u) {
   250	        console_write("JeiOS/aarch64: FATAL bss not cleared\r\n");
   251	        halt();
   252	    }
   253	    console_write("JeiOS/aarch64: bss clear verified\r\n");
   254	#else
   255	    /* Default build: the probe is in .bss and must read zero after the clear. */
   256	    if (je_arm_bss_probe != 0u) {
   257	        console_write("JeiOS/aarch64: FATAL bss probe nonzero\r\n");
   258	        halt();
   259	    }
   260	    console_write("JeiOS/aarch64: bss cleared\r\n");
   261	#endif
   262	
   263	    console_write("JeiOS/aarch64: dtb pointer recorded dtb=");
   264	    console_hex(je_arm_dtb_phys);
   265	    console_write("\r\n");
   266	
   267	    /* MP1 (ADR 0002 D5-3): install the EL1 exception vectors and prove it with a
   268	     * VBAR readback. From here an unexpected exception is a bounded diagnostic
   269	     * halt instead of a jump into an UNKNOWN VBAR. */
   270	    install_vectors();
   271	    console_write("JeiOS/aarch64: vectors installed vbar=");
   272	    console_hex(read_vbar_el1());
   273	    console_write("\r\n");
   274	#ifdef JE_ARM_VECTOR_TEST
   275	    /* Deliberate synchronous exception: trigger the planted BRK. It vectors into
   276	     * je_arm_exception, which asserts the exact syndrome, prints PASS, and halts
   277	     * — so control never returns here. */
   278	    console_write("JeiOS/aarch64: triggering deliberate BRK #0xa55a ...\r\n");
   279	    trigger_brk();
   280	    /* Unreachable: the BRK handler halts. If we get here the BRK did not vector. */
   281	    console_write("JeiOS/aarch64: FATAL BRK did not vector\r\n");
   282	    halt();
   283	#endif
   284	
   285	    console_write("JeiOS/aarch64: initialization complete; halted\r\n");
   286	    halt();
   287	}
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * MP1 step 1 (ADR 0002): ARM64 entry for the QEMU virt-8.2 / cortex-a72 target.
     4	 * Direct -kernel boot, no firmware. On entry (ADR 0002 D3/D4, validated on QEMU):
     5	 * EL1, MMU off, D-cache off, DAIF masked, x0 = DTB physical address, x1=x2=x3=0.
     6	 *
     7	 * This file does the irreducible arch bring-up and hands to je_arm_main():
     8	 *   - preserve x0 (DTB) across BSS clear by parking it in a callee-saved reg;
     9	 *   - assert we really are at EL1 (halt with a marker path otherwise);
    10	 *   - select SPSel=1 and set SP_EL1 to the top of a reserved in-image stack;
    11	 *   - initialize TCR_EL1 to a complete known interim value with TBI0=0
    12	 *     (ADR 0002 D5 interim: tagged user pointers rejected once the MMU is on;
    13	 *     this only sets the field — behavioral enforcement is the MMU/vectors PR);
    14	 *   - clear BSS with plain aligned stores (NO DC ZVA: pre-MMU RAM is
    15	 *     Device-nGnRnE, so unaligned / cache-maintenance ops fault);
    16	 *   - stash the DTB pointer into .data (survives, unlike a .bss slot);
    17	 *   - call the C entry, which drives the console + markers.
    18	 *
    19	 * Constraints (ADR 0002 D2/D5, locked by the design pass): no atomics/exclusives
    20	 * and no DC ZVA before the MMU is on; ISB after system-register writes; built
    21	 * -mstrict-align -mgeneral-regs-only so the compiler emits neither unaligned
    22	 * nor FP/SIMD code (a stray SIMD op would trap into absent vectors = silent hang).
    23	 */
    24	.section .text.boot, "ax"
    25	.globl _start
    26	_start:
    27	    /* ---- 64-byte arm64 Linux Image header (Documentation/arm64/booting.rst).
    28	     * QEMU reads this to place and enter the image. ---- */
    29	    b    real_start              /* code0: branch to real entry */
    30	    .long 0                      /* code1 */
    31	    .quad 0x80000                /* text_offset: 0x80000 — matches our link base
    32	                                  * (RAM 0x40000000 + 0x80000 = 0x40080000), so
    33	                                  * the load address equals the link address and
    34	                                  * absolute references (future VBAR/page tables)
    35	                                  * are correct, not luck. */
    36	    .quad __image_size           /* image_size: whole in-memory extent (linker) */
    37	    .quad 0x0000000000000001     /* flags: bits[2:1]=0b01 -> 4 KiB pages, LE */
    38	    .quad 0                      /* res2 */
    39	    .quad 0                      /* res3 */
    40	    .quad 0                      /* res4 */
    41	    .long 0x644d5241             /* magic "ARM\x64" */
    42	    .long 0                      /* res5 */
    43	
    44	real_start:
    45	    /* x0 holds the DTB phys addr on entry. Park it in a callee-saved register
    46	     * so the BSS clear below cannot clobber it (x0 is caller-saved and the clear
    47	     * uses low registers). */
    48	    mov     x19, x0
    49	
    50	    /* Enforce the ADR 0002 D3 entry assumption by construction: mask all
    51	     * exceptions (DAIF). With no vectors installed yet, an unmasked exception
    52	     * would jump to an UNKNOWN VBAR; masking makes "DAIF masked" true rather
    53	     * than merely assumed. */
    54	    msr     daifset, #0xf
    55	
    56	    /* ---- EL check: we require EL1. If not EL1, jump to a bounded halt that
    57	     * still writes a FATAL marker via the DR-only console (safe at any EL on
    58	     * this target). ---- */
    59	    mrs     x0, CurrentEL
    60	    lsr     x0, x0, #2
    61	    and     x0, x0, #3
    62	    cmp     x0, #1
    63	    b.ne    el_fatal
    64	
    65	    /* ---- Load-address self-check: confirm QEMU placed us where we are linked.
    66	     * adr gives the RUN-time address of _text; compare to the LINK-time address
    67	     * (loaded via a movz/movk immediate, which is absolute). A mismatch means a
    68	     * future absolute reference would be wrong, so halt with a FATAL marker now
    69	     * rather than miscompute a page table later. ---- */
    70	    adr     x0, _text
    71	    movz    x1, #0x4008, lsl #16    /* linked _text = 0x40080000 */
    72	    cmp     x0, x1
    73	    b.ne    load_fatal
    74	
    75	    /* ---- Stack: select SP_EL1 and point it at the top of the reserved stack.
    76	     * SPSel=1 makes SP resolve to SP_EL1 at EL1. ---- */
    77	    mov     x0, #1
    78	    msr     SPSel, x0
    79	    isb
    80	    adrp    x0, __stack_top
    81	    add     x0, x0, #:lo12:__stack_top
    82	    mov     sp, x0
    83	
    84	    /* ---- TCR_EL1 interim init (ADR 0002 D5): complete known value, TBI0=0.
    85	     * MMU stays OFF (SCTLR_EL1.M untouched). bit31 is RES1 on cortex-a72, so we
    86	     * write 0x80000000 (RES1 set, TBI0=0, everything else 0) — a defined interim,
    87	     * NOT a behavioral TBI claim. The MMU PR replaces this with a full config. */
    88	    movz    x0, #0x8000, lsl #16    /* 0x80000000: bit31 RES1, TBI0=0 */
    89	    msr     TCR_EL1, x0
    90	    isb
    91	
    92	    /* ---- SCTLR_EL1 explicit init (MP1 review gap): several SCTLR_EL1 bits reset
    93	     * to an architecturally UNKNOWN value, notably SA (bit3, SP-alignment check)
    94	     * and A (bit1, alignment check). With the MMU off we need a DEFINED value so
    95	     * whether a misaligned stp faults is deterministic, not luck. Write a known
    96	     * value: M=0 (MMU off), C=0 (D-cache off), I=0 (I-cache off), A=0 (no strict
    97	     * element alignment — we rely on -mstrict-align + aligned code, and A=1 would
    98	     * also fault legitimate Device accesses), SA=1 (SP must be 16-aligned at EL1),
    99	     * SA0 irrelevant (no EL0 yet). Preserve the architecturally RES1 bits for
   100	     * ARMv8.0 SCTLR_EL1 (bits 11, 20, 22, 23, 28, 29) so we don't write a
   101	     * reserved-0 into a reserved-1 field. Value = RES1 mask | SA.
   102	     *   RES1 bits: (1<<11)|(1<<20)|(1<<22)|(1<<23)|(1<<28)|(1<<29) = 0x30D00800
   103	     *   SA       : (1<<3)                                         = 0x00000008
   104	     *   total                                                     = 0x30D00808 */
   105	    movz    x0, #0x0808
   106	    movk    x0, #0x30D0, lsl #16
   107	    msr     SCTLR_EL1, x0
   108	    isb
   109	
   110	    /* ---- Clear BSS with plain aligned 16-byte stores. __bss_start/__bss_end
   111	     * are 16-byte aligned by the linker script. No DC ZVA (Device memory). ---- */
   112	    adrp    x0, __bss_start
   113	    add     x0, x0, #:lo12:__bss_start
   114	    adrp    x1, __bss_end
   115	    add     x1, x1, #:lo12:__bss_end
   116	#ifdef JE_ARM_BSS_CHECK
   117	    /* Test-only: BEFORE the real clear, pattern-fill the whole BSS with a
   118	     * nonzero value. QEMU hands us already-zeroed RAM, so without this the
   119	     * "cleared" check is tautological. The production clear loop below must then
   120	     * drive je_arm_bss_probe (a .bss symbol) back to zero for the C check. */
   121	    mov     x4, x0
   122	    movz    x5, #0xa5a5, lsl #48
   123	    movk    x5, #0xa5a5, lsl #32
   124	    movk    x5, #0xa5a5, lsl #16
   125	    movk    x5, #0xa5a5
   126	5:  cmp     x4, x1
   127	    b.hs    6f
   128	    stp     x5, x5, [x4], #16
   129	    b       5b
   130	6:
   131	#endif
   132	1:  cmp     x0, x1
   133	    b.hs    2f
   134	    stp     xzr, xzr, [x0], #16
   135	    b       1b
   136	2:
   137	
   138	    /* ---- Stash the DTB pointer into .data (survives; a .bss slot would have
   139	     * just been zeroed above). ---- */
   140	    adrp    x0, je_arm_dtb_phys
   141	    add     x0, x0, #:lo12:je_arm_dtb_phys
   142	    str     x19, [x0]
   143	
   144	    /* ---- Enter C. je_arm_main() never returns; it halts via WFE. ---- */
   145	    bl      je_arm_main
   146	
   147	    /* Defensive: if C ever returns, halt. */
   148	el_hang:
   149	    wfe
   150	    b       el_hang
   151	
   152	    /* ---- Not-EL1 path: write a bounded FATAL marker straight to the PL011 DR
   153	     * (works at any EL on this target, no stack needed) and halt. Kept tiny and
   154	     * self-contained because we cannot trust the normal console setup here. ---- */
   155	el_fatal:
   156	    movz    x1, #0x0900, lsl #16    /* PL011 DR @ 0x09000000 */
   157	    adr     x2, el_fatal_msg
   158	3:  ldrb    w3, [x2], #1
   159	    cbz     w3, 4f
   160	    str     w3, [x1]
   161	    b       3b
   162	4:  wfe
   163	    b       4b
   164	el_fatal_msg:
   165	    .asciz  "JeiOS/aarch64: FATAL entry not at EL1; halted\r\n"
   166	
   167	    /* ---- Load-address mismatch: QEMU placed us somewhere other than the link
   168	     * address. Bounded DR-only FATAL marker + halt. ---- */
   169	load_fatal:
   170	    movz    x1, #0x0900, lsl #16
   171	    adr     x2, load_fatal_msg
   172	7:  ldrb    w3, [x2], #1
   173	    cbz     w3, 8f
   174	    str     w3, [x1]
   175	    b       7b
   176	8:  wfe
   177	    b       8b
   178	load_fatal_msg:
   179	    .asciz  "JeiOS/aarch64: FATAL load address != link address; halted\r\n"
   180	
   181	.section .note.GNU-stack, "", @progbits
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * MP1 step 1 (ADR 0002): ARM64 linker script for the QEMU virt-8.2 target.
     4	 * RAM base is 0x40000000; with text_offset=0x80000 in the Image header QEMU
     5	 * places us at 0x40080000, so we link there and the load address equals the
     6	 * link address (a boot.S self-check enforces this). The whole in-memory extent
     7	 * [_text, _end) is published as __image_size in the Image header, and the linker
     8	 * asserts below keep the stack and BSS inside that extent, 16-byte aligned, and
     9	 * non-overlapping. NOTE: QEMU does NOT use image_size to place the DTB/initrd —
    10	 * on virt it puts them at RAM base + min(ram/2, 128 MiB) (hence dtb=0x48000000
    11	 * at -m 256M), well clear of our image; image_size is nonetheless set correctly
    12	 * per the boot protocol for loaders that DO honor it.
    13	 */
    14	ENTRY(_start)
    15	
    16	STACK_SIZE = 0x4000;   /* 16 KiB boot stack, in-image (NOLOAD) */
    17	
    18	SECTIONS
    19	{
    20	    . = 0x40080000;
    21	    _text = .;
    22	
    23	    .text.boot : { KEEP(*(.text.boot)) }
    24	    . = ALIGN(0x800);                       /* VBAR_EL1 requires 2 KiB alignment */
    25	    .text.vectors : { KEEP(*(.text.vectors)) }
    26	    .text      : { *(.text*) }
    27	    .rodata    : { *(.rodata*) }
    28	    .data      : { *(.data*) }
    29	    . = ALIGN(16);
    30	    _file_end = .;          /* end of file-backed (loaded) content, incl. .data */
    31	
    32	    .bss (NOLOAD) : {
    33	        . = ALIGN(16);
    34	        __bss_start = .;
    35	        *(.bss*)
    36	        *(COMMON)
    37	        . = ALIGN(16);
    38	        __bss_end = .;
    39	    }
    40	
    41	    .stack (NOLOAD) : {
    42	        . = ALIGN(16);
    43	        __stack_bottom = .;
    44	        . += STACK_SIZE;
    45	        . = ALIGN(16);
    46	        __stack_top = .;
    47	    }
    48	
    49	    . = ALIGN(16);
    50	    _end = .;
    51	    __image_size = _end - _text;
    52	
    53	    /* Build metadata kept in the ELF but outside the loaded image. Debug and
    54	     * DWARF sections are placed (not discarded) so debuggers still work; they
    55	     * carry no VMA in the binary image. */
    56	    .debug_info    0 : { *(.debug_info) }
    57	    .debug_abbrev  0 : { *(.debug_abbrev) }
    58	    .debug_line    0 : { *(.debug_line) }
    59	    .debug_str     0 : { *(.debug_str) }
    60	    .debug_line_str 0 : { *(.debug_line_str) }
    61	    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    62	    .debug_addr    0 : { *(.debug_addr) }
    63	    .debug_loc     0 : { *(.debug_loc) }
    64	    .debug_loclists 0 : { *(.debug_loclists) }
    65	    .debug_ranges  0 : { *(.debug_ranges) }
    66	    .debug_rnglists 0 : { *(.debug_rnglists) }
    67	    .debug_aranges 0 : { *(.debug_aranges) }
    68	    .debug_frame   0 : { *(.debug_frame) }
    69	    .comment       0 : { *(.comment) }
    70	
    71	    /* A correct static freestanding link should resolve all relocations and
    72	     * emit no PLT/GOT; discard any such orphans so they can never land in the
    73	     * image. (--orphan-handling=error makes an UNlisted section a build error.) */
    74	    /DISCARD/ : {
    75	        *(.note*) *(.eh_frame*)
    76	        *(.iplt) *(.rela.iplt) *(.igot.plt) *(.got) *(.got.plt) *(.plt)
    77	        *(.rela*)
    78	    }
    79	}
    80	
    81	/* --- Placement invariants (design-pass requirements). --- */
    82	ASSERT((__bss_start   & 0xf) == 0, "BSS start must be 16-byte aligned");
    83	ASSERT((__bss_end     & 0xf) == 0, "BSS end must be 16-byte aligned");
    84	ASSERT((__stack_top   & 0xf) == 0, "stack top must be 16-byte aligned");
    85	ASSERT((__stack_bottom & 0xf) == 0, "stack bottom must be 16-byte aligned");
    86	/* BSS and stack are after the file-backed image... */
    87	ASSERT(__bss_start   >= _file_end, "BSS must follow the file-backed image");
    88	ASSERT(__stack_bottom >= __bss_end, "stack must follow BSS (no overlap)");
    89	/* ...and inside the published in-memory image extent. */
    90	ASSERT(__stack_top <= _end, "stack must lie within the image extent");
    91	ASSERT(__bss_end   <= _end, "BSS must lie within the image extent");
     1	/* SPDX-License-Identifier: Apache-2.0 */
     2	/*
     3	 * MP1 (ADR 0002 D5-3): the AArch64 EL1 exception trap-frame ABI — owner-approved
     4	 * 2026-10-19. The vector saver (boot/vectors asm) writes this layout; the C
     5	 * handler reads it. The field ORDER and OFFSETS are a FROZEN contract: the MP3
     6	 * exact-site fixup table and any crash-dump tooling will bind to these offsets,
     7	 * and D5-3 requires the fixup be keyed on the exact faulting PC (ELR_EL1), so
     8	 * `elr` must be a stable, documented field.
     9	 *
    10	 * D5-3 mandates ESR_EL1, FAR_EL1, ELR_EL1, SPSR_EL1 are saved. We also save the
    11	 * full GPR set x0-x30 and the faulting SP for a complete saved state (ARM64.md
    12	 * MP1 "complete saved-state layout"). GPR area is gpr[31] = x0..x30 exactly
    13	 * (AArch64 has no architectural x31 in GPR context; SP is a separate field),
    14	 * per the owner's DECISION 2.
    15	 *
    16	 * Total size 304 bytes, 16-byte aligned (AArch64 requires SP 16-aligned on
    17	 * exception entry and for stp). elr/spsr and esr/far form 16-aligned stp pairs.
    18	 * The asm and C MUST agree exactly; _Static_assert(offsetof(...)) below pins it.
    19	 */
    20	#ifndef JEIOS_AARCH64_TRAPFRAME_H
    21	#define JEIOS_AARCH64_TRAPFRAME_H
    22	
    23	/* Byte offsets — shared by the asm saver (via the JE_TF_* macros) and C. */
    24	#define JE_TF_GPR     0    /* x0..x30, 31 * 8 = 248 bytes */
    25	#define JE_TF_SP      248  /* SP_EL1 at fault time */
    26	#define JE_TF_ELR     256  /* ELR_EL1 — faulting PC (D5-3 fixup key) */
    27	#define JE_TF_SPSR    264  /* SPSR_EL1 — pre-exception PSTATE */
    28	#define JE_TF_ESR     272  /* ESR_EL1 — syndrome (EC/IL/ISS) */
    29	#define JE_TF_FAR     280  /* FAR_EL1 — faulting VA (valid per EC) */
    30	#define JE_TF_VECTOR  288  /* originating vector index 0..15 */
    31	#define JE_TF_PAD     296  /* reserved; keeps size 16-aligned */
    32	#define JE_TF_SIZE    304
    33	
    34	#ifndef __ASSEMBLER__
    35	#include <stdint.h>
    36	
    37	typedef struct {
    38	    uint64_t gpr[31];   /* x0..x30 */
    39	    uint64_t sp;        /* SP_EL1 at fault */
    40	    uint64_t elr;       /* ELR_EL1 (faulting PC) */
    41	    uint64_t spsr;      /* SPSR_EL1 */
    42	    uint64_t esr;       /* ESR_EL1 */
    43	    uint64_t far;       /* FAR_EL1 */
    44	    uint64_t vector;    /* 0..15 */
    45	    uint64_t pad;       /* reserved */
    46	} je_arm_trapframe;
    47	
    48	_Static_assert(sizeof(je_arm_trapframe) == JE_TF_SIZE, "arm trapframe size");
    49	_Static_assert(__builtin_offsetof(je_arm_trapframe, gpr)    == JE_TF_GPR,    "tf gpr");
    50	_Static_assert(__builtin_offsetof(je_arm_trapframe, sp)     == JE_TF_SP,     "tf sp");
    51	_Static_assert(__builtin_offsetof(je_arm_trapframe, elr)    == JE_TF_ELR,    "tf elr");
    52	_Static_assert(__builtin_offsetof(je_arm_trapframe, spsr)   == JE_TF_SPSR,   "tf spsr");
    53	_Static_assert(__builtin_offsetof(je_arm_trapframe, esr)    == JE_TF_ESR,    "tf esr");
    54	_Static_assert(__builtin_offsetof(je_arm_trapframe, far)    == JE_TF_FAR,    "tf far");
    55	_Static_assert(__builtin_offsetof(je_arm_trapframe, vector) == JE_TF_VECTOR, "tf vector");
    56	_Static_assert((JE_TF_SIZE % 16) == 0, "arm trapframe 16-aligned");
    57	
    58	/* The common C exception handler (MP1: diagnose + halt; no recovery). */
    59	_Noreturn void je_arm_exception(je_arm_trapframe *tf);
    60	#endif /* __ASSEMBLER__ */
    61	
    62	#endif /* JEIOS_AARCH64_TRAPFRAME_H */

 succeeded in 0ms:
# Coding-agent starting guide

Updated 2026-10-16. Read this with ../AGENTS.md, HANDOFF.md and IMPLEMENTATION.md.
Owner requested a sustained coding-agent handoff. This guide records requirements
and continuation order; it does not claim later milestones are implemented.

Current status and timestamped history: [HANDOFF.md](HANDOFF.md). Ecosystem-wide
[zero trust](ZERO_TRUST.md) is an owner requirement; enforcement remains planned.

## Product philosophy across every subsystem

Owner clarification 2026-10-14: seamless everyday defaults plus granular,
customizable power-user/developer/organization control govern the whole OS and
ecosystem. Read the overall product vision near the start of DESIGN.md.
Security profiles are one application; apply the same progressive disclosure,
per-area customization, automation, coherent cross-device behavior and honest
recovery principles to all subsystem work. These are requirements, not implemented
features. Keep interface complexity separate from actual authority and capability.

## Establish the correct baseline

Foundation PR: https://github.com/auto44e/jeios/pull/1
PR #1 merged into main at `03aa2f380d8e49262abd1720a3b8db37517cbb31`.
Start new implementation branches from updated main; check for newer concurrent
work before editing. The additional multitouch/security-key requirements are
proposed documentation only. Verify CI for the exact implementation head used.

Implemented: M1 exceptions, validated memory discovery/protected mappings, and M2
bounded allocation plus seven trusted preemptible kernel workers on one AMD64 CPU.
Not implemented: userspace, capabilities/IPC, SIMD context, SMP, filesystem,
networking, GPU/NPU/media runtime, login, sync, encryption or domains.

## Work sequence and project-ready tasks

| Order | Issue | Dependency | Completion evidence |
| --- | --- | --- | --- |
| 1 | [#2: boundary contracts](https://github.com/auto44e/jeios/issues/2) | M1/M2 baseline | Reviewed ADR, register/state/VM/ABI contracts |
| 2 | [#3: private VM and bounded images](https://github.com/auto44e/jeios/issues/3) | #2 | Ownership, zeroing, permissions and rollback tests |
| 3 | [#4: CPL3 scheduling and local faults](https://github.com/auto44e/jeios/issues/4) | #2, #3 | Actual CPL3 execution, timer preemption, survivor progress |
| 4 | [#5: syscalls and user copies](https://github.com/auto44e/jeios/issues/5) | #3, #4 | Pointer/arithmetic failure tests and narrow fault recovery |
| 5 | [#6: qualify M3a](https://github.com/auto44e/jeios/issues/6) | #2–#5 | Adversarial QEMU and regression evidence on exact head |
| 6 | [#7: M3b handles](https://github.com/auto44e/jeios/issues/7) | #6 | Type/generation/rights/lifetime tests |
| 7 | [#8: M3c IPC and init](https://github.com/auto44e/jeios/issues/8) | #7 | Bounded transfer/wait/close/cancel semantics |
| Backlog | [#9: later owner requirements](https://github.com/auto44e/jeios/issues/9) | Relevant roadmap gates | Split into bounded tasks when prerequisites exist |

All eight issues start planned/open, not in progress or done. No assignee or
calendar deadline was invented. Project destination:
https://github.com/users/auto44e/projects/1/views/1
Issue creation does not imply membership in the project board.

Read [M3A.md](M3A.md) for the concrete proposal. Its temporary 2 MiB user arena,
limits, ABI numbers, interrupt entry and NX-required launch policy are proposed
engineering choices, not previously implemented interfaces or a frozen owner ABI.
Resolve these in the first ADR. Keep M3a/M3b/M3c distinct and reviewable.

## Owner requirements and authoritative design locations

| Requirement | Source and placement | Implementation boundary |
| --- | --- | --- |
| Fresh C11 core, modular services, Unix interoperability/NT-style object model | [DESIGN](DESIGN.md), ADR 0001/0002 | No foreign kernel-source fusion; ring 0 is trusted |
| Privacy, explicit authority, graceful failures; customization with good defaults | [SECURITY](SECURITY.md), [DESIGN](DESIGN.md) | UI complexity never grants rights; no telemetry/cloud-login requirement |
| OS-wide touchscreen/trackpad multitouch | [INPUT](INPUT.md), M4/M7/E1 | Qualified transports, secure routing, native toolkit/accessibility; foreign apps may use pointer fallback |
| Built-in hardware MFA security keys | [SECURITY_KEYS](SECURITY_KEYS.md), M4-M9 | CTAP/FIDO2 broker, native/WebAuthn boundaries, recovery; preboot/PQ capabilities qualified separately |
| Fresh-kernel mobile and ecosystem Android app support | [MOBILE](MOBILE.md), dedicated auto44e/jeios-mobile | Owner chose native C kernel; QEMU ARM64 first, Pixel candidates unqualified; AOSP is reference/future app layer |
| Native configurable init and supervision | [INIT](INIT.md), M3c/M4 | Minimal fixed-plan init first, full supervisor later |
| Single-device local login through multi-device/family/multi-user/fleet management | [IDENTITY](IDENTITY.md), M3 onward | Separate principal/device/session/tenant and management vs data-key authority |
| E2E protection built into identity/sync design; several pairing methods | [IDENTITY](IDENTITY.md), [ENCRYPTION](ENCRYPTION.md) | Qualified key lifecycle, entropy and reviewed protocols; no ad hoc crypto |
| PQ protection, nearby sharing and honest RAM/endpoint trust limits | [ENCRYPTION](ENCRYPTION.md), M5/M6/M9/P2 | Encryption is not universal secrecy from a privileged running OS |
| ext4 as provisional initial OS default | [STORAGE](STORAGE.md), M5 | Owner decision; still no filesystem code or production qualification |
| Raw-disk access without forced formatting; temporary/sidecar file-like views | [STORAGE](STORAGE.md) | Explicit range authority/leases and metadata; never silently format |
| Broad open-source filesystem interoperability | [STORAGE](STORAGE.md) | Qualify each format/version/read/write profile independently |
| Native domains and AD/Group Policy-like management | [DOMAINS](DOMAINS.md) | Native directory distinct from Microsoft AD DS compatibility |
| Windows/macOS/Linux domain interoperability | [DOMAINS](DOMAINS.md) | Explicit provider/endpoint matrix; Samba AD and FreeIPA/LDAP/Kerberos separately scoped |
| GPU/NPU across all planned platforms; open standards by default | [ACCELERATORS](ACCELERATORS.md), M4/M8 | Native drivers and tested per-device support; no universal availability claim |
| Smart CPU→GPU/NPU offloading, configurable APIs, conservative defaults | [ACCELERATORS](ACCELERATORS.md) | Eligible backend implementations, resource/latency/power budgets, precision and CPU fallback |
| Built-in FFmpeg, HandBrake and Blackmagic RAW support | [ACCELERATORS](ACCELERATORS.md), M5–M8 | Userspace integration; separate upstream licenses and native SDK feasibility |
| Apache-2.0 selected for original project code | [LICENSE](../LICENSE), [ADR 0005](adr/0005-licensing.md) | Implemented licensing; third-party provenance/terms still reviewed |
| Old AMD64 compatibility, AArch64/mobile/IoT and coherent customizable desktop | [HARDWARE](HARDWARE.md), [ROADMAP](ROADMAP.md), [DESIGN](DESIGN.md) | Qualified profiles and dependency gates; no shortcut past isolation |

## Sustained implementation discipline

Work one bounded gate at a time; continue routine implementation and fixes without
reasking already decided owner preferences. Keep small reviewable commits. Record
new architectural decisions and rationale in ADRs. Ask only when a material owner
decision or access limitation genuinely blocks useful work. Do not silently relax
security, replace standards-first goals with mandatory vendor dependencies, or
claim a later gate because its design document exists.

Before each change, inspect current code, open issues/PRs and tests to avoid
redoing work or overwriting another agent. Prioritize executable user-page aliases,
unsaved architectural state, safe TSS/CR3 switching, exact copy fixups, lifetime
and reclamation. A security invariant that cannot be upheld blocks the feature;
record the failure and next bounded fix rather than forging a passing gate.

At each checkpoint update HANDOFF, IMPLEMENTATION and ROADMAP in the same change:
implemented/tested/planned/blocked, exact commit and commands, observed results,
changed contracts, known failures and next task. Update related security/hardware
claims and issue state only to match evidence. Keep local and remote CI distinct.
Run `make check`; in this managed environment use
`ASAN_OPTIONS=detect_leaks=0 make check` only if LeakSanitizer cannot inspect tasks,
and disclose that limitation. Boot changes require `make smoke integration` with
new meaningful QEMU tests, exact expected results and a live survivor for fault
containment. Run `git diff --check` and validate documentation links.

Before ending a work session leave reproducible build/run instructions, current
branch/commit, a clean committed checkpoint where possible, remaining test failures,
resource/accounting invariants and next step. Do not merge/release, assign reviewers,
change credentials, contact vendors or import restricted source without applicable
authorization. The owner asked to approve PR #1; that is not an instruction to
invent an independent reviewer or silently bypass self-review restrictions.

## Cryptographic relock requirement — 2026-10-14

ENCRYPTION.md now requires a configurable ordinary-lock path to withdraw protected
user-domain keys and plaintext access without a power cycle. Include process/cache/
DMA cleanup, credential-gated reopening, background exceptions and truthful failure
status. Screen lock alone is insufficient; reboot equivalence is not assumed.
Key-domain and lifecycle design precede storage/identity format freeze; runtime
qualification remains a later gate. No implementation was added.

## Protected system and version management — owner decision 2026-10-14

STORAGE.md records seamless protected/versioned system deployments by default,
full owner customization including an immutability off switch, and independent
intuitive OS/built-in-app update/rollback in both protected and mutable modes.
Native apps must participate through lifecycle and schema compatibility contracts.
Write protection, deployment history, trust policy and data encryption are distinct.
Mutable-mode direct edits require explicit conflict/checkpoint handling; rollback
must not silently revert user documents or bypass data/anti-rollback constraints.
Accepted direction, not implemented behavior; M3a remains next.

## Multiple default filesystems — owner decision 2026-10-14

STORAGE.md now records Btrfs, OpenZFS and a mobile backend as first-class long-term
default choices selected by qualified device profiles. F2FS is the proposed mobile
candidate. Owners can select a conforming custom backend, including as system
default with additional boot/recovery qualification, or use a visibly experimental
profile. Native APIs keep workflows coherent without claiming identical features.
ext4 stays the initial bring-up target. No automatic reformat/conversion or new
implementation is implied. M3a remains next.

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

## Core standards workstreams — 2026-10-16

Read [STANDARDS.md](STANDARDS.md). POSIX.1-2024 and built-in LDAPv3 are accepted
core commitments; staged runtime implementation remains blocked on native gates.
[POSIX #15](https://github.com/auto44e/jeios/issues/15) defines the profile and
conformance roadmap; [LDAP #16](https://github.com/auto44e/jeios/issues/16) defines
the isolated provider and identity-broker contract. Begin bounded design work
without displacing M3a. Mobile parity reuses these contracts and implementations.
Historical wording that treats generic LDAP as only a candidate is superseded.

## Curl-inspired CI direction — 2026-10-16

[CI_STRATEGY.md](CI_STRATEGY.md) records the owner's short/long-term testing goal:
portable regressions, compiler diversity, sanitizers/static analysis, replayable
failure injection and fuzzing, supplemented by native kernel execution and storage
crash-recovery qualification. [Core #18](https://github.com/auto44e/jeios/issues/18)
is the first bounded CI implementation task. Existing gates remain mandatory;
M3a and mobile MP0 remain next. This update adds planning only, not workflow jobs.

## Ecosystem assurance direction — 2026-10-16

[CODING_STANDARD.md](CODING_STANDARD.md) defines NASA/JPL-inspired risk profiles
and ten project rules. Core #19 tracks baseline audit and enforcement, alongside
CI #18. Apply across core, services, apps, tooling and mobile; no compliance audit
or new CI checks are claimed. M3a remains next.
# JeiOS system design

## Overall product vision: approachable defaults, deep owner control

Owner clarification: 2026-10-14T19:59:09-04:00. This philosophy governs the
entire JeiOS OS/ecosystem, not only security. Everyday users should have a coherent,
seamless and intuitive experience without needing to understand implementation
details. Power users, developers and organizations should be able to progressively
inspect, customize, automate, replace and administer supported components in depth.

Apply this to installation/onboarding, desktop and mobile interaction, themes and
window management, accessibility, apps and compatibility, files/storage, networking,
identity/sync, privacy/security, hardware/drivers, audio/media, CPU/GPU/NPU resource
policy, power/performance, init/services, updates/recovery and developer tools.
Good defaults must be complete useful workflows, not a deliberately limited edition.
Advanced controls must be discoverable, documented and supported interfaces rather
than requiring undocumented patches for ordinary customization.

Presets are starting points, not rigid user classes or one global slider.
Allow per-area complexity and configuration: expert audio with simple networking,
a custom shell with stock updates, or deep developer tools with hardened security.
Separate presentation detail, functional configuration, administrative authority,
security policy and hardware capability. Changing one must not silently change
the others. Explicit authorized policy changes remain available to owners within
the device/workspace's disclosed management boundaries.

Provide simple task-oriented UI with progressive disclosure, searchable settings,
plain explanations and accessible input. Expose appropriate advanced configuration
through GUI, CLI and documented APIs with equivalent policy enforcement; not every
low-level operation needs a prominent everyday screen. Versioned configuration,
validation, dry runs where useful, export/import, automation and effective-setting
inspection should make complex setups reproducible. Explain dependencies and
tradeoffs instead of silently ignoring unsupported settings.

Support a spectrum from stock, mostly automatic experiences to deeply customized,
experimental ("YOLO"), finely tuned and tightly controlled deployments. These are
composable choices, not a ranking where more restriction or more complexity is
always better. The authorized owner chooses the balance; organizations can define
and disclose policy for the devices/workspaces they manage.

Prefer scoped changes, previews and reversible transactions where feasible.
Clearly identify destructive, irreversible or system-wide effects. Recovery should
offer a known-good usable environment without needlessly discarding customization.
A reset-to-defaults action must identify affected data/settings and cannot promise
to undo prior data loss or compromise. Do not mistake customizability for a promise
that every component is hot-swappable or every hardware feature exists.

Carry coherent concepts, configuration semantics and workflows across desktop,
mobile, IoT and future device types, adapting interaction to screen, input, power
and hardware. Aim for functional parity where practical; publish explicit support
differences and fallbacks. Cloud accounts and vendor services must not be mandatory
merely to access ordinary local use or expert controls.

Acceptance must cover both audiences: ordinary end-to-end tasks with sensible
defaults and little friction, and documented advanced workflows with predictable
results, configuration portability, accessible explanations and tested recovery.
Qualify each subsystem as it is implemented. This is an accepted product direction,
not evidence that any future UI, service, API or parity target already exists.


Version: 0.2, 2026-10-12. Status: long-term architecture plus M1/M2 BSP implementation.
Audience: the project owner, kernel contributors, system developers, and future
AI agents. This is the primary design document; implementation evidence lives in
HANDOFF.md. Requirements use **must**, **should**, and **may** deliberately. A
requirement here is not a claim that the source currently implements it.

## 0. Current implementation versus target design

M1 exception entry, validated boot-memory discovery and protected mappings are
implemented. M2 adds bounded page/heap allocation, PIC/PIT interrupts, seven
preemptible guarded kernel workers, sleep/wakeup and exit/reaping. These are
single-CPU AMD64 mechanisms; there is no userspace or sandbox yet. The runtime
self-test checks registers, memory and resource accounting before halting.

Read [IMPLEMENTATION.md](IMPLEMENTATION.md) for exact limits and current contracts,
and [HANDOFF.md](HANDOFF.md) for observed tests. Historical mentions of M0 describe
the original scaffold. All other subsystem designs below remain requirements or
proposals until the implementation ledger says otherwise. In particular the first
heap is page-granular, not a slab allocator; general wait queues, canary/entropy
setup, SMP and hardened application boundaries remain future work.

## 1. Product intent and constraints

JeiOS starts a new kernel rather than mechanically combining existing kernels.
The long-term objective is an everyday system which feels coherent for users,
provides structured interfaces for developers, and remains comprehensively
customizable for enthusiasts. The same underlying services should eventually
serve workstation, server, mobile, and appropriate 64-bit IoT profiles.

The owner has specified a C OS core, modular architecture, old 64-bit machine
compatibility, modern CPU/GPU enablement, graceful failures, security/privacy
inspired by GrapheneOS and CalyxOS, granular complexity, a macOS-inspired top
level, and Linux-style desktop ricing. Earlier discussion supplies architectural
inspiration from Linux, BSD, and NT, not a requirement to merge their source.

Concrete constraints:

- Hardware support and security support are separate promises. Old machines may
  boot without satisfying a modern hardened profile.
- Modern features must not leak into baseline instruction paths.
- Interface simplicity must not obscure failures or increase authority.
- The OS must support offline use without a vendor account or telemetry.
- APIs must be explicit enough to audit, test, and describe to future agents.
- A fresh kernel has no automatic access to Linux's hardware ecosystem or native
  Windows/macOS applications. Driver and ABI compatibility are distinct projects.
- No claim of production reliability, formal verification, performance superiority,
  or equivalence to mature hardened OSes is justified by the initial scaffold.

### 1.1 Non-goals for the first release sequence

M0-M3 do not target a complete desktop, accelerated GPUs, cellular service, a web
browser, Linux kernel module loading, WDM/KMDF compatibility, Win32, Darwin binary
compatibility, or secure daily use. The first useful target is a reproducibly
booting and diagnostically understandable kernel with isolated userspace.

## 2. Architecture decision: modular core with service isolation

Adopt a small privileged kernel with explicitly layered internal subsystems and
an NT-inspired executive vocabulary. Critical execution mechanisms stay in
kernel space: traps, scheduling, address-space control, capabilities, IPC,
interrupt routing, and the minimal device primitives needed for progress.
Higher-level policy and suitable drivers should run as isolated user services.

This is a design direction, not a declaration that M0 is a complete microkernel.
Initially all implemented C runs at ring 0. We will decide service boundaries by
measured performance, recovery behavior, driver requirements, and trust budget.
A large hybrid core can evolve from the wrong defaults; each new privileged
subsystem therefore needs a documented reason to be privileged.

| Layer | Responsibilities | Intended trust and ownership |
| --- | --- | --- |
| Architecture/platform | Entry, traps, page tables, CPU state, interrupt controller, firmware discovery | Kernel, narrow architecture-specific contracts |
| Kernel mechanisms | Threads, memory objects, handles, IPC, timers, IRQ/DMA grants | Kernel; minimal authority and bounded operations |
| Executive interfaces | Objects, asynchronous I/O, device lifecycle, security checks | Mechanisms in kernel; management policy in services |
| System services | Configuration, storage, network, package/update, diagnostics, audio/display brokers | Separate processes with explicit capabilities |
| Application environment | Native SDK, POSIX layer, later compatibility environments | Sandboxed processes and scoped brokers |
| User experience | Stock shell, settings, file manager, compositor policy, theme/widget services | Replaceable interfaces; no inherent extra privilege |

### 2.1 Why this shape

A single scheduler and VM manager prevent contradictory ownership and locking.
NT-inspired objects and requests offer predictable developer conventions; they
do not require reproducing NT's internal implementation. BSD-style capability
ideas and Unix conventions can coexist with a coherent native object API.
Linux-derived lessons guide device breadth, feature negotiation, and scalable
execution. The macOS-inspired experience is implemented through public services,
not through hidden shell-only authority.

Rejected as initial approaches: running three native kernel implementations in a
single address space; loading arbitrary foreign binary drivers; and designing a
novel stable driver binary ABI before a real device lifecycle exists. Full
microkernel separation remains a possible outcome, but committing prematurely to
an unmeasured IPC cost or forcing every driver into userspace would also be risky.

## 3. Modularity means three different things

**Build modules** have separate source ownership and dependency contracts. M0
supports this form. **Loadable privileged modules** add code to the kernel address
space and are fully trusted. **Isolated service modules** have process or VM
boundaries and capability-scoped interfaces. Only the last category contains
arbitrary memory corruption in the way people often expect from modularity.

### 3.1 Build-time module contract

Each subsystem must describe provided interfaces, dependencies, owned resources,
initialization order, partial failure cleanup, and shutdown preconditions. The
initial `je_module` list is explicitly ordered, validated before side effects,
and rolled back in reverse order on failure. A failed initializer undoes its own
partial allocations. Successful initializers have infallible cleanup callbacks.
M0 has no dependency resolver, version loader, concurrency, or restart manager.

Next registry design: stable subsystem names, versioned interface descriptors,
acyclic dependency graph, optional dependencies, required-versus-optional boot
criticality, bounded initialization deadlines, and explicit diagnostic status.
Reject duplicate names, cycles, unsatisfied versions, and inconsistent readiness.
Do not attempt to restart a kernel module after suspected corruption of shared
memory. Optional features may remain unavailable while boot continues.

### 3.2 Future privileged loading

Keep kernel internal APIs source-level and versioned by build at first. A module
manifest should bind architecture, exact kernel ABI/build identity, dependencies,
capabilities requested, integrity signature, and provenance. Signing validates
origin and authorization, not freedom from bugs. No auto-download during boot.

Loading requires executable mapping validation, relocation bounds checks,
W^X, an authorized policy decision, and an unpublished-to-live state transition.
Unloading must stop new calls, quiesce existing calls and DMA, revoke handles,
remove IRQ callbacks, synchronize references, then free code. A busy module stays
loaded. Never free a module because a timeout elapsed while callbacks still run.
Unsigned development modules require a deliberate development trust profile
with visible consequences independent of UI difficulty settings.

### 3.3 Service lifecycle

Use states: discovered -> authorized -> starting -> ready -> quiescing -> stopped;
failed and quarantined are distinct terminal/control states. A supervisor may
restart only when its restart contract permits it. Track monotonic generation
numbers so replies and capabilities from an old instance cannot target a new one.
Backoff, restart budgets, and crash loops prevent an unstable driver from
consuming all resources. Dependencies must see service unavailable rather than
waiting forever. Persistent state requires recovery before readiness.

## 4. Language, tools, and memory discipline

C11 is the baseline language for the kernel and core services. GNU assembler is
used only where exact architecture instructions or entry conventions demand it.
C cannot establish its own initial stack/page tables or fully describe trap
entry. Host tooling may use Python; later UI language selection is open.

Use fixed-width types for serialized and hardware fields. Internal `size_t` and
pointers must never enter on-disk or IPC formats without a defined encoding.
Check addition, multiplication, alignment, offsets, and truncation before using
untrusted lengths. Define signedness and endianness. Avoid C bitfields for device
registers. Volatile is not a lock or a cross-core memory-ordering mechanism.

Every public core interface must state whether it allocates, sleeps, may be
called with interrupts disabled, accepts user pointers, retains a pointer,
transfers ownership, or invokes callbacks. Document output state on failure.
Use explicit statuses; do not conflate unsupported, denied, busy, exhausted,
timed out, and hardware failure. Destructors must safely handle every legal
partial initialization state, not arbitrary invalid memory.

### 4.1 Hardening sequence

Compiler stack protection remains disabled pending canary initialization and
a validated failure/entropy strategy; this must be resolved before claiming a
hardened userspace boundary. M1/M2 now provide guarded stacks, validated mappings,
zero-initialized heap/stack allocations, and non-executable data where supported. Later add control-flow protections supported by the
compiler, hardened heap metadata, delayed reuse or quarantine where useful,
randomized layouts, and verified entropy before claiming cryptographic security.

Use compiler warnings as errors, host UB sanitizers for portable algorithms,
address sanitizers where available, fuzzers for parsers, and static analysis.
Portable helpers can be hosted; hardware paths need emulation and real hardware.
A successful build does not prove memory safety. Memory tagging reduces certain
risks and is not complete temporal/spatial safety. Do not hand-roll cryptography.

### 4.2 Build constraints

Baseline AMD64 objects use `-march=x86-64`, no red zone, no floating point/SIMD
register generation, freestanding compilation, and no hosted runtime dependency.
Future optimized translation units carry separate target flags and cannot be
called without feature enablement. Toolchain upgrades must preserve baseline
code generation and boot evidence. Production builds need pinned compiler,
linker, firmware/bootloader inputs, reproducibility reports, and artifact hashes.

## 5. Boot and platform abstraction

The first platform is QEMU's legacy PC using GRUB Multiboot2. It is a convenient
experimental entry path, not the final boot trust architecture. The ELF lives
at 1 MiB, enters in 32-bit protected mode, establishes a three-level path into
four-level paging, identity maps the first GiB using 2 MiB pages, loads a GDT,
sets EFER.LME, and enters 64-bit C with an aligned stack. The BSP alone runs.
Interrupts remain disabled during early setup. CPUID checks long mode before
using the long-mode MSR. M1 validates and copies loader data; M2 enables IRQ0 only
after memory and thread state are initialized.

The assembly and linker enforce placement within the bootstrap map. ELF segments
are RX, R, and RW, but the initial page tables are temporarily RWX; segment flags
alone are not hardware enforcement. M1 installs an IDT/TSS and then replaces
those maps with read-only code/rodata, guarded stacks and NX data where available.
Unexpected kernel faults produce a fatal diagnostic. Userspace recovery and
process separation are still required before running untrusted code.

### 5.1 M1 boot information contract

Parse the Multiboot2 block only after verifying its address range is mapped,
minimum fixed header size is accessible, total size has sane upper limits, each
tag has a minimum size, aligned increments cannot overflow, and the terminal tag
is consistent. Validate memory-map stride, types, overlapping regions, arithmetic,
reserved ranges, and the block's lifetime. Treat firmware tables as untrusted.
Reserve kernel, stack, boot block, page tables, modules, firmware, MMIO, and low
memory before allocator use. Never treat all identity-mapped addresses as RAM.

Translate loader-specific data into an internal `boot_context` with explicit
physical ranges, entry capabilities, framebuffer metadata, firmware handles,
and a measured/trust-state descriptor. It must never carry unchecked firmware
pointers into unrelated services. Copy or pin retained information and document
when loader memory can be reclaimed.

### 5.2 Platform evolution

Add UEFI x86-64 and later AArch64 entry paths sharing only the validated internal
context. Legacy BIOS support remains a distinct boot profile. AArch64 bring-up
needs exception-level normalization, granule discovery, translation setup,
cache maintenance, vector table, timer, GIC, PSCI, and DT/ACPI handling as
appropriate. CPU ISA compatibility alone does not imply a board can boot.

Platform interfaces should cover physical memory, timers, CPUs, interrupts,
firmware discovery, MMIO attributes, reset/poweroff, and safe DMA primitives.
Keep ACPI/DT parsing distinct from the generic device graph. Hardware errata and
firmware quirks must be named policies with affected versions and tests.

## 6. CPU compatibility and feature enablement

Interpret 'oldest 64-bit system' as a long-term per-ISA goal, beginning with early
AMD64 PCs. Alpha, SPARC64, MIPS64, POWER, IA-64, and other historical systems are
separate ports with different firmware, endianness, drivers, and toolchains;
they are not covered by an AMD64 binary. First AArch64 baseline target is Armv8-A
on a selected emulator/board, not older non-AArch64 64-bit architectures.

For every feature maintain four facts: hardware present, platform usable,
OS implementation ready, and policy allowed. Enabled is their intersection.
The initial portable helper combines hardware/OS/policy masks only; a future
platform mask must be added when firmware/device prerequisites are real.
Required features missing from the usable set cause explicit refusal, not a
false success or silent substitution with weaker isolation.

### 6.1 x86 specifics

Discover leaf availability before querying CPUID. Use architecture-defined tests
for NX, XSAVE family, SMEP/SMAP, CET, protection keys, virtualization, invariant
TSC, topology, and performance/power hints. CPUID advertising AVX does not alone
permit AVX execution: CR4.OSXSAVE, XCR0 bits, and adequate per-thread context
storage must be configured. AMX has large extended state and additional policy
requirements. Do not run those instructions in baseline kernel C.

Use eager, validated state restoration initially; avoid speculative/lazy
approaches with poorly understood security consequences. Size XSAVE buffers from
# Ecosystem coding and assurance policy

Owner direction: 2026-10-16T20:01:26-04:00, reaffirmed
2026-10-16T20:08:52-04:00. Apply the spirit of NASA/JPL engineering throughout
JeiOS. This is a tailored project policy proposed in PR #17, not NASA certification,
literal Power of Ten conformance, or evidence that existing code has been audited.
Automated enforcement and baseline assessment are tracked in
[core #19](https://github.com/auto44e/jeios/issues/19), alongside
[CI #18](https://github.com/auto44e/jeios/issues/18).

## Scope and risk profiles

Apply to original code, tests, build tools, packaging, services, SDKs, desktop,
mobile and future IoT integrations. Human and AI contributions use the same rules.
Classify by failure impact and authority, not implementation language or directory.
These are JeiOS profiles, not NASA software classifications.

| Profile | Typical components | Required emphasis |
| --- | --- | --- |
| Critical | Kernel, boot, privileged drivers, isolation, identity/key services, storage commits, updates/recovery, release tooling | Resource bounds, explicit invariants, failure/rollback evidence, independent review before a hardened/release qualification claim |
| Service | Contained brokers, networking, compatibility runtimes, persistent app services | Validated interfaces, quotas, cancellation, restart/recovery, concurrency and hostile-input tests |
| Application/tool | Ordinary UI, SDK examples, development utilities | Language-appropriate lint/type checks, error handling, tests proportionate to risk, reproducible dependencies |

A UI authorization prompt, image packager or signing helper can be critical.
An unclassified component must be assessed before privileged integration. A service
profile assumes demonstrated containment; a module running in ring 0 is critical.
Do not impose C syntax restrictions on Python or other future userspace languages.
The shared kernel remains C11 with architecture assembly at documented boundaries.

## Ten JeiOS rules

| ID | Rule | Review or test evidence |
| --- | --- | --- |
| JCS-01 | Keep control flow simple and functions focused. Critical C prohibits direct/indirect recursion and setjmp/longjmp absent an approved deviation. A forward cleanup goto is permitted when it makes ownership unwind explicit. | Call/cleanup review; flag functions over roughly 60 logical lines for decomposition or recorded rationale, not arbitrary splitting. |
| JCS-02 | Bound work, buffers, retries, stack use and queues. Each finite critical loop needs a demonstrable input/capacity bound or explicit budget. | Limit and timeout tests; document maximum work per dispatch. Persistent scheduler/event loops require progress/block/yield and shutdown semantics where applicable. |
| JCS-03 | Specify ownership, lifetime and allocation context. No unbounded allocation; prohibit blocking allocation in interrupt, fault and other nonblocking paths. | Exhaustion, fail-Nth allocation, partial-init unwind and use-after-free checks. Use preallocation/pools for deterministic paths. |
| JCS-04 | Validate external inputs before use, including sizes, arithmetic, alignment, encodings and authority. Check fallible results; justify deliberate ignored results locally. | Boundary, overflow, malformed-input, permission-denial and error propagation tests. Internal helpers may rely on documented validated preconditions. |
| JCS-05 | Encode meaningful invariants with side-effect-free assertions and explicit failure policy. Runtime input validation must survive release builds. | Pre/postcondition and failure tests; malformed user/network data must not become a kernel assertion crash. No artificial assertion quota. |
| JCS-06 | Minimize scope and mutable shared state. Document lock order, caller context, atomic ordering and cancellation ownership. | Concurrency/lifetime review and targeted race schedules as concurrency lands; volatile is not synchronization. |
| JCS-07 | Keep preprocessing, pointer manipulation and unsafe boundaries small and visible. Prefer typed helpers; validate callbacks and document their allowed targets/lifetimes. | Review casts, pointer arithmetic, macro evaluation and dispatch tables. No hidden multiple evaluation or unsupported configuration claims. |
| JCS-08 | Build with documented strict warnings and appropriate static analysis. Resolve new findings or record narrow reviewed deviations. | Exact tool versions/configuration and results; no blanket suppression or lowering warnings to pass. Analyzer coverage must name the files/configurations checked. |
| JCS-09 | Trace behavior to a requirement/contract and repeatable evidence, especially failure paths. | PR links contract, changed invariant, positive/negative tests and exact build/target; preserve regression inputs and distinguish host, emulator and physical evidence. |
| JCS-10 | Keep changes reviewable, dependencies traceable and exceptions explicit. Test infrastructure itself must detect known defects. | Review record, provenance, version pin/update policy, gate failure demonstrations and deviation register. Never fabricate independent review or a passing check. |

## Deliberate adaptations of the Power of Ten

The original C-oriented paper restricts allocation after initialization, function
pointers, pointer indirection, recursion and control flow. It also advocates small
functions, assertions, narrow scope, checked results and strong analysis. JeiOS
adopts the analyzability goal with the explicit adaptations above. Dynamic process
and filesystem workloads need allocation; drivers need reviewed dispatch tables.
These mechanisms require quotas, exhaustion handling, clear lifetimes and tests.
Only paths with a justified deterministic contract prohibit runtime allocation.

Do not translate a timing budget into a claim of proved worst-case execution time.
Do not treat passing sanitizers, high coverage or a style checker as proof of
memory safety, functional correctness or security. Assembly needs register/stack,
clobber, interrupt, barrier and architecture-precondition review plus execution
tests; a C analyzer cannot validate it by implication.

Assertions detect internal contract violations. Recovery must match the failure
domain: reject invalid input, fail a transaction, terminate an isolated service,
or stop safely on unrecoverable kernel corruption. Never continue with corrupted
authority or persistent state merely to avoid a crash.

## Adoption, review and deviations

After adoption, new and materially changed original code must address applicable
rules in its PR. Existing code has no retroactive compliance claim: first inventory
critical paths and record findings, then fix them in bounded changes. A known
critical defect blocks the affected qualification claim; an untouched baseline
finding needs an owner and disposition, not silent grandfathering.

PR evidence must identify profile, applicable rule IDs, changed contracts, tests,
unverified checks and deviations. Independent review is required before claiming
critical hardened/release qualification. If another qualified reviewer is not
available, record that gap; author/AI self-review is not independent assurance.
Development may continue with that qualification explicitly unclaimed.

A deviation record includes ID, rule, exact scope/revision, reason, alternatives,
risk, mitigation, verification evidence, responsible owner, actual reviewer/status
and expiry or a concrete revisit trigger. Pending is not approved. Put records
under docs/deviations/ when first needed; do not invent a reviewer or waiver.
Rule-specific permissions above are policy adaptations, not hidden deviations.

Imported code retains its provenance, language and license. Evaluate exposure,
upstream quality, update strategy and integration tests; isolate it where possible.
Do not relabel it as conforming or perform an enormous style rewrite just to match
local rules. Any critical dependency needs evidence suitable for its actual role.

## Delivery stages and enforcement status

1. Now: publish this policy in core and reference it from mobile contributor
   instructions. Preserve M3a and MP0 as next gates. Documentation is not enforcement.
2. Near term (#19 with #18): classify current components, audit critical baseline,
   map each rule to a checker or manual review, and add bounded CI checks. Record
   tool versions and prove a representative violation fails each new gate.
3. As services arrive: extend failure injection, parser fuzzing, concurrency tests,
   durable-state recovery and traceability to POSIX/LDAP requirements. Qualify
   native targets separately; keep diagnostics and test outcomes honest.
4. Before hardened/release claims: complete applicable reviews and unresolved-risk
   disposition, verify reproducible evidence and target-specific recovery. Use
   deeper structural coverage or formal models where the identified risk warrants
   them; neither 100% coverage nor formal verification is claimed today.

Existing checks and gaps remain documented in [CI_STRATEGY.md](CI_STRATEGY.md).
This policy change adds no analyzer, workflow, branch-protection setting or runtime
mechanism. It does not certify safety-critical or aerospace use.

## Primary references reviewed 2026-10-17 UTC

- [NASA NPR 7150.2D, Chapter 4](https://nodis3.gsfc.nasa.gov/displayDir.cfm?Internal_ID=N_PR_7150_002D_&page_name=Chapter4): project coding standards, static analysis, repeatable tests, requirements and target validation.
- [Gerard J. Holzmann, The Power of Ten](https://spinroot.com/gerard/pdf/P10.pdf): compact, analyzable rules for safety-critical C, published with NASA/JPL affiliation.

These are engineering references. NASA requirements apply within their own scope;
JeiOS does not claim agency approval or complete compliance by citing them.

 succeeded in 0ms:
# JeiOS

JeiOS is an experimental operating system with a fresh C kernel. Its design
combines Unix interoperability, an NT-inspired object/service model, and an
integrated, deeply customizable desktop. Privacy, explicit authority, recoverable
operations and qualified hardware support are long-term goals.

**Current stage: M1/M2 single-CPU kernel foundation.** The AMD64 BIOS prototype
boots, handles fatal CPU exceptions, validates Multiboot2 memory information,
enforces read-only/NX mappings where supported, allocates RAM, and preemptively
runs seven guarded kernel threads. Its diagnostic workload verifies registers,
sleep/wakeup, heap contents and resource reclamation, then halts.

There is no userspace, application sandbox, SMP, filesystem, graphics, networking,
dynamic module loader or ARM implementation. This is not a daily-use or hardened OS.

## Start here

Coding agents: begin with [the starting guide](docs/AGENT_START.md) for the current
baseline, all owner requirements, issue dependencies and checkpoint rules.

- [Detailed design](docs/DESIGN.md): full product architecture and future contracts.
- [M3a implementation proposal](docs/M3A.md): proposed boundaries and M3b/M3c contracts.
- [Accelerators and media](docs/ACCELERATORS.md): open standards, conservative offloading,
  and planned FFmpeg/HandBrake/Blackmagic RAW integration; no runtime support yet.
- [Init/service design](docs/INIT.md), [encryption and nearby sharing](docs/ENCRYPTION.md),
  and [filesystem recommendation](docs/STORAGE.md): planned contracts and acceptance gates.
- [Identity and device management](docs/IDENTITY.md): local login, E2E sync, family and fleet contracts.
- [Domains and centralized policy](docs/DOMAINS.md): native joining and separate AD/GPO compatibility targets.
- [Multitouch input](docs/INPUT.md) and [hardware security keys](docs/SECURITY_KEYS.md): planned OS-wide integration.
- [Fresh-kernel mobile project](docs/MOBILE.md): shared core, capability parity and future Android app layer.
- [Implementation ledger](docs/IMPLEMENTATION.md): current M1/M2 mechanisms, evidence and limits.
- [Hardware policy](docs/HARDWARE.md) and [security model](docs/SECURITY.md).
- [Development](docs/DEVELOPMENT.md): build, emulator tests and fault modes.
- [Roadmap](docs/ROADMAP.md): gated milestones and dependency order.
- [Agent handoff](docs/HANDOFF.md) and [AGENTS.md](AGENTS.md): continuation instructions.
- [References](docs/REFERENCES.md): primary architecture sources.

## Build and verify

On x86-64 Linux with GCC, binutils, make and Python 3:

```sh
make check
```

With GRUB PC utilities, xorriso and QEMU installed:

```sh
make smoke integration
```

Normal outputs are in `build/0/`; each fault mode uses a separate directory.
See DEVELOPMENT.md for sanitizer requirements, CPU/memory profiles and limitations.

The OS core uses C11 with small architecture-specific assembly bridges. Early
AMD64 is the first compatibility target; AArch64 requires a separate port.
Modern features are enabled only after their OS prerequisites are implemented.
The desktop, native SDK, privacy controls and mobile/IoT ecosystem remain planned.

Original JeiOS code is licensed under [Apache License 2.0](LICENSE), selected by
the owner on 2026-10-13. Third-party components retain their own licenses and
require provenance/compatibility review. No external kernel source has been
imported. See [the licensing decision](docs/adr/0005-licensing.md).

## Core standards direction

[POSIX.1-2024 and LDAPv3](docs/STANDARDS.md) are accepted core requirements,
implemented in stages across desktop/mobile. Both remain planned; neither current
conformance nor directory login is claimed. Organizational enrollment is optional.
# Agent handoff: M1/M2 kernel foundation

## Current status — observed 2026-10-18 (read this first)

Implemented and merged beyond M3a: **M3b** (typed handle table, ADR 0008) is in
progress on x86-64, and a second architecture, **ARM64 (MP1)**, now boots.

- **M3b** (issue #7, ADR 0008 accepted, Option A): the pure handle table, the
  handle syscalls (`handle_self`/`close`/`query`/`rights`), the rights ABI, AND
  **attenuation** (`handle_dup`, op 9) are all merged. Attenuation made rights
  enforcement genuinely ring-3-observable: a process derives a rights-reduced
  handle and `handle_rights` on it returns DENIED (QEMU `M3b-s1` proof). SELF is
  the only object kind; dup is SELF-only (structurally type-gated — a destroyable
  type would be a latent UAF without a retain, which M3c must add). **#7 stays
  OPEN** (landed "Refs #7"): its remaining acceptance — bounded per-object
  **reference count + overflow**, destruction-on-last-reference, and the
  **service-instance vs slot generation** split — needs a destroyable object and
  is M3c work.
- **ARM64 MP1 step 1** (jeios-mobile#1, ADR 0002): boots on QEMU `virt-8.2` /
  `cortex-a72` (direct `-kernel`, no firmware) — entry EL1, stack, TCR_EL1 interim,
  BSS clear, DTB pointer recorded, bounded PL011 console, ordered markers. Separate
  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
  the x86-64 flow is untouched. **Not yet enforced (deferred to MP2+):** VBAR
  vectors + the D5 trap frame (ESR/FAR/ELR/SPSR) + the deliberate-fault test, the
  TBI behavioral test, DTB *validation*, and the MMU.
- **M3c** (issue #8, **ADR 0009 accepted**): in progress. The contract is frozen
  (endpoint/channel object, split refcount + destruction, bounded data-only
  messages, waits, transactional attenuated transfer, bootstrap fixture; two
  owner-gated decisions — terminal-destruction/no-incarnation-counter, and the
  process-death release walk as its own owner-signed PR). **D-SEQ step 2
  (lifecycle)** is implemented: `kernel/core/endpoint.c` + `endpoint.h` — the
  endpoint pool, channel record, **split refcount** (`handle_refs` vs
  `inflight_refs`), retain/release, destruction-on-last-ref, `REFOVERFLOW`, the
  endpoint-aware alloc/dup/close seam (table stays pure), and retain-before-commit
  endpoint dup (while `dup_self` stays SELF-only). Step 2 kept create kernel-only;
  **step 3 (owner-signed) is now done**: the process-death **release walk** (via a
  generic `je_vm` teardown hook run inside an **IRQ critical section** in
  `je_vm_destroy` — A3-1), the **typed close dispatcher** (ring-3 HANDLE_CLOSE
  routes endpoint handles through the endpoint layer — A3-3), and **ring-3
  `endpoint_create` (op 10)** returning both handles **packed in RDX** (A3-2)
  shipped together. Proven in QEMU: `M3c-s3a` (kernel self-test: survivor-peer
  PEER_CLOSED, record freed on survivor close) and `M3c-s3b` (ring-3 create →
  typed close → fault → reap reclaims the channel; pool baseline held across
  >POOL_CAP cycles). ADR 0009 was amended (A3-1/A3-2/A3-3, owner-approved) for the
  IRQ-save death-path change and the packed-RDX ABI shape. Earlier, ADR 0009 D4a
  appended endpoint rights (`SEND=0x2`…), an owner-approved ABI change that flipped
  the tested `dup(self,0x2)` result INVALID→DENIED (Option A; see IMPLEMENTATION.md
  "M3c rights-bit discrepancy"). **Step 4 (non-blocking data IPC) is now done**:
  `try_send` (op 11) / `try_recv` (op 12) move bounded, copied, **data-only**
  messages through per-end inbound ring queues (Model A: send on end i → peer
  inbound), honoring the D6 copy ordering (send: preflight → copy_from_user into
  stack staging → enqueue; recv: peek → copy_to_user → infallible commit, so a
  copy-out fault leaves the message queued). Statuses QUEUE_FULL/SHORT_BUFFER/
  PEER_CLOSED/EMPTY (10–13) added; `JE_ABI_MSG_MAX=64`, DEPTH=4 (policy bounds,
  ~2.1 KB pool). D9 data-only is enforced (R10 cap_count must be 0). An
  owner-approved touch to the tested lifecycle path discards/zeroes inbound
  queues on close/free (free condition unchanged). Proven ring-3 by the `M3c-s4`
  self-loopback marker (direction, FIFO, SHORT_BUFFER, copy-out-fault rollback →
  delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED) — which
  does NOT prove cross-address-space delivery (one process holds both ends; that
  needs transfer). **ABI forward note:** the single RDX scalar can't return
  received capability handles, so **step 5 MUST add a new op / out-register
  extension, not overload `try_recv`** (R8/R9 and RDX[63:32] reserved now).
  **Step 5 (capability transfer) is now done** (frozen by ADR 0009 A5): `try_send`
  carries 0..2 caps (descriptors queued + inflight-retained on the target, D3.2);
  `try_recv` mints fresh attenuated handles in the receiver's table (D8). D9 =
  cycles allowed but reclaimed on last-handle removal (A5-1); stale cap → handle 0
  (A5-2); `maybe_free_record` got the `allocated` guard + detach-before-release
  discard order (A5-3); ABI uses the pre-reserved R8/R9 path (A5-4). Proven: hosted
  cap mint/dead-cap/cycle-reclaim + the `M3c-s5` kernel self-test (cross-TABLE
  transfer over a seeded channel, message across distinct tables, S+C reclaimed).
  **Transfer sequencing:** the sender must retain the transferred end until the
  receiver mints (closing first → dead cap, A5-2). **Ring-3 two-process transfer
  deferred to step 7** (recorded decision: multi-process choreography is step 7's
  subject; the spin-based two-stub proof raced the single-CPU cooperative
  scheduler). **Remaining D-SEQ:** step 6 (waits, owner-gated — edits `sched.c`),
  7 (bootstrap fixture + the deferred ring-3 two-process transfer proof). #8 closes
  when the sequence completes; #7's deferred refcount/overflow is already
  discharged (endpoints are genuinely destroyed + reclaimed on process death).
  **Step 6 (blocking recv waits) is now done** (owner-gated; frozen by ADR 0009 A6
  + A6-CLARIFY-1/2). `try_recv` with a nonzero R10 timeout blocks until WOKEN /
  PEER_CLOSED / TIMED_OUT / CANCELLED (A6-2 internal-only in v1); R10==0 is the
  unchanged non-blocking poll. Mechanism: a per-end waiter + one
  `je_endpoint_wait_arbitrate` CAS, `je_sched_block_current` (pre-park
  `je_uframe_check` + 2-byte `int 0x80` rewind), a `wait_pending` continuation
  prologue in `je_syscall_dispatch` that finishes from the owned reference (no
  handle re-resolve, copy-out after the context switch), guarded
  `BLOCKED_IPC→READY` wake, one idempotent `je_endpoint_wait_release` owner (called
  by `mark_dying`). New ABI statuses `TIMED_OUT=14`/`CANCELLED=15`; R10 timeout
  bounded by `JE_IPC_MAX_WAIT`. Proven: `M3c-s6` (TIMED_OUT end-to-end, ring 3) +
  hosted `test_endpoint` 27–28 (the pure waiter layer, all four causes via the
  CAS, reserved-head, release). **Deferred to step 7:** the full two-process ring-3
  WOKEN handshake (the wake bridges are wired; step 7 schedules two cooperating
  user processes) — honest, same as the step-5 ring-3 deferral.
  **Step 7 (bootstrap fixture) is now done — M3c (#8) is functionally complete.**
  `M3c-s7` injects a kernel-seeded channel into two ring-3 processes and discharges
  BOTH deferrals end-to-end: the two-process WOKEN handshake (B confirmed parked
  before A exists -> A's send is a causally-forced wake) AND the two-process
  capability transfer over the WOKEN continuation (A sends c1 as a cap, B mints it
  on wake, A closes c1 BEFORE B uses the mint -> retained-ownership proof via the
  post-close send reaching A's c0), then a bounded ping-pong + DONE teardown, both
  exit 0, pool reclaims to 0. Blocking recv (step 6) is what made the two-process
  ping-pong deterministic (a parked receiver yields; no spin deadlock). No new
  kernel surface. **D-SEQ steps 1-7 all complete.**
  **Remaining D-SEQ:** none.

Full qualification evidence for M3a is in `docs/M3A_RETRO.md`; the per-PR ledger is
`docs/IMPLEMENTATION.md`. Everything above is QEMU/hosted-verified and
peer-cross-reviewed (AI-assisted, not independent human qualification); the owner is
the merge gate.

**Decision-record policy (active 2026-10-19, `docs/adr/0000-decision-record-policy.md`):**
every recorded decision from now on carries three parts — (1) the decision + why,
(2) predictions if other choices had been made, (3) flex options to revisit (tag
`FLEX (planning review)` for ones to take to the main planning agent at the
pause). A one-time **retroactive sweep** PR brings ADRs 0001–0009 + prior
owner-gated calls into this format; it is scheduled right **before** the final
doc-sync + retro at the M3c/MP2 pause. ADR 0009's A5 amendment (step-5 transfer:
D9 reclamation invariant, stale-cap S1 null-marker, the `maybe_free_record` guard,
the R8/R9 transfer ABI) is the first decision written in the new format.

## M3a #5 step (b): recoverable user copies + exception fixup — observed 2026-10-17T19:50:00+00:00

Branch `feat/m3a-5b-user-copy`. The FINAL M3a piece. `kernel/core/usercopy.c` +
`kernel/arch/x86_64/usercopy.S` add `copy_from_user`/`copy_to_user` (ADR D9) and
the dev-only `copy_probe` syscall (op 3). Every copy prevalidates the span
(canonical, no wrap, in-arena, per-page present+permission) before touching it,
copies through ONE approved assembly site, and a per-thread copy scope + the
#PF-handler fixup (`je_usercopy_try_fixup`, called from `je_trap_dispatch`)
recover ONLY a not-present (P=0) fault at the EXACT copy-site RIP with an active,
owner-matched, direction-matched scope and CR2 in the declared span. Any other
kernel #PF — protection fault, different RIP, no scope — stays fatal. A recovered
fault → JE_ABI_INVALID (treated as a copy failure, not masked).

Dispatch order: `cs==8` vector-14 → try fixup → else existing kernel PANIC; then
the user int 0x80 / fault-terminate paths unchanged. The dev-only fixup-injection
op (op 4) and its user stub are compiled ONLY under `-DJE_DEV_COPY_INJECT`
(TEST_MODE 11); a `make check` `nm` assertion proves those symbols are absent
from the default kernel.

Peer: design done via a 2-round cross-talk DEBATE (the new peer-debate tool). The
debate's key refinement: recover only P=0 not-present faults, treat a protection
fault after validation as fatal (don't mask a validator bug). Codex's follow-up
check corrected the injection to run from a real CPL3 syscall context (genuine
owner/scope), flagged a stale bootstrap self-test call (removed), and had me
tighten `cs&3==0` to exact `cs==8`.

Tested in QEMU (this env): `make check` PASS (op-3 decode test + ABI isolation +
dev-inject-absent-from-default-ELF assertions), `make smoke` PASS — the boot runs
the copy_probe battery (valid+zero OK; null/kernel-range/len>256 INVALID; valid
bytes verified moved) with peer B live. `make integration` PASS — 11 modes; mode
11 proves the fixup RECOVERS a copy-site #PF (dev inject) while modes 4/5 still
PANIC on a kernel-origin fault (not wrongly recovered). All three CI smoke
profiles pass; M2's 4 markers unchanged. Honest limits recorded: no SMAP/SMEP, so
the net covers the approved site only; fail-closed under uncorrupted-kernel
invariants, not absolute. This COMPLETES the ADR 0007 M3a userspace boundary
(#2–#5). AI-assisted review only; not independent human qualification.

## M3a #5 step (a): v0 syscall dispatcher — observed 2026-10-17T17:40:00+00:00

Branch `feat/m3a-5a-syscall-dispatch`. The DPL3 `int 0x80` gate now dispatches a
real experimental v0 syscall (previously it routed straight to death). New
`kernel/core/syscall.c` (`je_syscall_decode` pure + unit-tested; `je_syscall_dispatch`
glue), `include/jeios/abi.h` (separate, explicitly-numbered ABI status/ops,
never the internal `status.h`). trap.c routes user-origin `int 0x80` to the
dispatcher before the fault path; kernel-origin 0x80 stays fatal.

Ops: `abi_query(which)` (version/profile in RDX), `yield()` (reschedule via the
kind-aware path, RAX=OK written before switching), `exit(code)` (DEATH_EXIT +
code). Unknown op → UNSUPPORTED (full 64-bit decode); recognized op with a
nonzero reserved arg → INVALID, no side effect. Every non-switching return goes
through `je_sched_return_to_user` → re-validates+sanitizes the frame, terminating
(DEATH_BAD_RETURN) rather than IRETQ a bad frame.

Owner-approved tested-code touch (kept M2+step4 green): the step-4 death record
gained `reason {FAULT,EXIT,BAD_RETURN}` + `exit_code` (the `0xbad` magic vector
removed); a sibling `je_sched_exit_current` was added rather than renaming the
tested `je_sched_fault_terminate`.

Peer: pre-write design vote (both agents) caught the direct-return-skips-validation
gap (fixed by `je_sched_return_to_user`). Codex cross-review found no
escape/UAF/ABI-leak and drove three closures: test asserts RDX=0 on INVALID/
UNSUPPORTED; a new QEMU stub proves BAD_RETURN *through the syscall return path*;
and these doc updates.

Tested in QEMU (this env): `make check` PASS (+`test_syscall` + abi.h isolation
grep), `make smoke` PASS — boot proves exit(42)→clean EXIT, abi_query/yield
return results to ring 3, unknown op→UNSUPPORTED+survives, bad-frame-on-return→
BAD_RETURN death, all with peer B live and page accounting restored.
`make integration` PASS (10 modes). All three CI smoke profiles pass locally;
M2's 4 markers unchanged. Next: step (b) — copy_from/to_user + copy_probe +
exception-fixup copy sites (the only genuinely tricky remaining piece). NOT yet
claimed: any user-copy/pointer handling, a stable ABI. AI-assisted review only;
not independent human qualification.

## M3a #4 step 4: fault-local process death + D6 int 0x80 gate + H1 fix — observed 2026-10-17T15:30:00+00:00

Branch `feat/m3a-4-step4-fault-local`. #4's core acceptance: a synchronous USER
fault — or a user `int 0x80`, or bad-return-state — terminates ONLY the
offending process; the kernel and peers keep running.

- `je_trap_dispatch` routes a user-origin ((cs&3)==3) synchronous fault in the
  explicit terminate-list {0,1,3,4,5,6,7,10,11,12,13,14,16,17,19} OR a user
  `int 0x80` to `je_sched_fault_terminate`. NMI(2)/#DF(8)/#MC(18)/reserved and
  any kernel-origin fault stay fatal PANIC (unchanged).
- `je_sched_fault_terminate` marks the current user thread DYING (death record),
  returns a LIVE successor's frame. It does NOT free the dying task's kstack/CR3
  (kernel C still runs on that RSP0 kstack); the bootstrap reaper frees it later.
  Triple-checks cs/ss + kind==USER + frame==rsp0_top-sizeof before acting.
- **H1 security fix:** `je_sched_interrupt` previously called `abort_scheduler()`
  (whole-kernel halt) when a *next* user thread's saved frame failed
  `je_uframe_check` — a userspace DoS (ring-3 sets a bad RSP, never faults,
  kernel dies at its next dispatch). The shared `select_and_install_next` now
  terminates that thread and retries another candidate; bootstrap is always a
  live fallback. A user task can no longer halt the kernel via a bad saved frame.
- D6 gate: `idt[0x80].attributes=0xee` (present, DPL3, IST=0, 64-bit interrupt
  gate — IF cleared on entry). Only vector 0x80 is user-reachable; kernel 0x80
  PANICs; every other gate stays DPL0 (user int of any other vector -> #GP ->
  terminate). No syscall handler yet — a user int 0x80 is an unsupported op
  routed to death (ADR amendment option 3 satisfied).
- Reaper `je_sched_reap_dead` (bootstrap, slot 0, asserts IF=0): for each DYING
  user slot, assert not-current / not-active-CR3 / not-active-RSP0, then
  `je_sched_drop_user` (restore kstack guard) + `je_vm_destroy`. New DYING slot
  state; `vm.c` UNTOUCHED (the vm record stays READY until destroy; D5's
  DYING/REAPABLE are modeled in the scheduler slot). Invariant recorded:
  single-CPU, PCID off, IF=0 in the fault handler — SMP would need a per-CPU
  off-stack handshake.

Tested in QEMU (this env): `make check` PASS, `make smoke` PASS — the normal boot
runs the two-task acceptance: A (reads an unmapped arena page) dies by user #PF
(vector 14, cs=0x33, CR2=the unmapped page); B (heartbeat loop) is proven to
advance AFTER A's death (heartbeat + dispatch + preemption all strictly increased
from the post-death snapshot); A is reaped from bootstrap while B stays live; then
the user `int 0x80` and bad-RSP(H1) death causes are each exercised and
terminated fault-locally with B still running; B torn down with page accounting
back to the pre-step-4 baseline. `make integration` PASS — 10 modes; mode 10
FLIPPED from step 3: user `xorps` #UD now terminates the task (fault-local) and
the kernel reaches the normal halt (asserts no "JeiOS fatal:"). All three CI
smoke profiles pass locally; M2's 4 markers unchanged (owner sign-off condition).

Peer: pre-write design vote (both agents) found the H1 kernel-DoS in merged main
and the "first entry rides a real trap" rule; Codex cross-review then confirmed no
UAF/escape and flagged hardening (reaper now self-asserts IF=0; ss checked in
terminate) + test gaps (int 0x80 and H1 death now runtime-tested, not just
claimed) + wording, all addressed.

Honest claims: synchronous user faults, bad-return-state, and user int 0x80 kill
only the offender; a peer is shown to progress after the death; the faulted
process is reclaimed (per-cause reaps + aggregate page-count equality); the D6
gate is present and leads to death. NOT claimed: two-task isolation qualification,
syscalls/ABI, copy-in/out, a process exit path, general/complete reclamation
across all paths, SMP. #4 acceptance is met; #5 is the integrated qualification +
syscalls. AI-assisted review only; not independent human qualification.

## M3a #4 step 3: CPL3 entry/return — real ring-3 execution — observed 2026-10-17T12:20:00+00:00

Branch `feat/m3a-4-step3-cpl3-entry`. The scheduler now IRETQs into ring 3.
Added: thread kind (kernel/user) + `je_thread_create_user` (synthesizes the
initial user frame on the process's held kstack, requires NX per D2);
`je_sched_interrupt` validates the incoming frame by kind and, for a user next
# Gated roadmap

Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
current baseline and linked issue dependencies. Future designs remain unimplemented.


No milestone has a calendar deadline. Each gate requires code, failure tests,
implementation-status updates, and a handoff with observed evidence. Existing
M0 stubs do not count as completing future modules.

| Gate | Scope | Pass criteria | Dependencies |
| --- | --- | --- | --- |
| M0 | Freestanding AMD64 bootstrap, serial, core helpers, design | Build + ELF checks + host tests; boot smoke separately observed | Initial scaffold |
| M1 | Exceptions, boot block validation, RAM reservations, correct mappings | Known exception produces bounded diagnostic; malformed boot maps rejected; kernel/data permissions match qualified CPU | M0 |
| M2 | Page/heap allocation, timers, IRQs, kernel threads and context | Exhaustion/rollback tests; preemption/context correctness; lock/context rules; NX and guarded stacks where supported | M1 |
| M3 | User address spaces, handles, IPC, syscalls, minimal init | Two isolated tasks run; illegal access contained; stale/denied handles rejected; no raw pointer ABI | M2 |
| M4 | Service supervisor, device graph, buffer grants, basic driver | Crash-loop budget; generation revocation; device timeout; DMA containment qualified or explicit legacy policy | M3 |
| M5 | Read-only initfs then durable storage and configuration | Checked image parser, durability/error behavior, interrupted commit/recovery tests | M4 |
| M6 | POSIX subset, networking, native service SDK | Tested interfaces, per-app network mediation and revocation, portable tool builds | M3-M5 |
| M7 | Framebuffer/software compositor, shell/settings, native UI contracts | Accessible interaction, scoped prompts, data themes, recovery shell, complexity/authority independence | M5-M6 |
| M8 | Hardware acceleration and advanced CPU state | Named GPU driver, reset isolation, feature migration/context tests, benchmarked fallbacks | M4 and per-feature gates |
| M9 | Secure boot/update/encryption and hardened profile qualification | Signed/recoverable images, anti-downgrade policy, keys outside repo, power-loss and security review evidence | M5 plus platform support |
| P1 | AArch64 board/emulator | Own boot/traps/timer/MMU tests; shared core checks; no invented feature support | M1 contracts; platform port parallel to later work |
| P2 | Enhanced ARM / confidential workloads | Platform-qualified features, context/ABI/fault tests, explicit guarantees | P1 and relevant M3/M8/M9 contracts |
| E1 | Mobile | Named board/device, power/touch/display/modem separation and update path | P1, M7-M9 plus device drivers |
| E2 | 64-bit MMU IoT | Headless profile, scoped management, offline recovery, power loss tolerance | Relevant M4-M9 mechanisms |

## Current gate status and next task

M0, M1a, M1b and the scoped M2 BSP foundation are implemented on
main (foundation PR #1 merged 2026-10-13); see IMPLEMENTATION.md and HANDOFF.md for exact evidence.
M1 has validated maps/reservations, fatal exceptions and hardware permission tests.
**Update 2026-10-18:** M3a (the userspace boundary) is complete and qualified
(see `docs/M3A_RETRO.md`). **M3b** (typed handles, ADR 0008) is complete and
merged — the handle table, the handle syscalls, the rights ABI, and attenuation
(which made rights enforcement genuinely ring-3-observable) all shipped. A second
architecture, **ARM64 MP1 step 1** (ADR 0002), now boots on QEMU virt-8.2.
**M3c** (capability IPC + waits + init, **ADR 0009** accepted) is in progress: its
contract is frozen and **D-SEQ step 2 (endpoint lifecycle — pool + split refcount
+ destruction + retain-on-dup, create kernel-only)** is implemented; the remaining
steps are the owner-signed death-path + ring-3 create, non-blocking data IPC,
capability transfer, waits, and the bootstrap fixture.
M2 has bounded page/heap allocation, timer preemption, guarded kernel threads,
yield/sleep/exit and deferred stack reclamation. This is not a general scheduler,
SMP implementation, userspace sandbox, or complete hardening milestone.

Next is M3, split into independently reviewable subgates:

- **M3a:** private user address spaces, ring-3 entry/return, user-copy fault
  containment and a bounded user program loader. Prove two actual CPL3 tasks
  cannot access each other's memory or kernel state. A task fault must be local.
  The boundary contract is frozen in [ADR 0007](adr/0007-m3a-userspace-boundary.md)
  (accepted 2026-10-17); issues #3–#6 implement and qualify against it. Issue #3
  (hosted per-process VM builder + bounded image loader) is implemented and
  hosted-tested — see IMPLEMENTATION.md "M3a #3". Issue #4 is being landed in
  reviewable, individually QEMU-verified steps: step 1 (ring-3 GDT descriptors),
  step 2 (real per-process page tables + CR3, CPL0-verified), step 3 (CPL3
  entry/return — real ring-3 execution under preemption, CR3 + TSS.RSP0, D7
  enforcement), step 4 (fault-local process death + the D6 int 0x80 gate + the H1
  kernel-DoS fix — a synchronous user fault / user int 0x80 / bad-return-state
  kills only the offender, a peer is proven to keep running, the dead process is
  reclaimed). Steps 1–4 are merged/landing; #4 acceptance ("illegal access kills
  only the offender; peers survive") is met in QEMU. Issue #5 is the integrated
  two-task isolation qualification + the real syscall handler (copy-in/out) behind
  the int 0x80 gate, landing in steps: step (a) — the v0 syscall dispatcher
  (abi_query/yield/exit, UNSUPPORTED/INVALID, ABI isolated from internal status,
  validated syscall returns) — and step (b) — copy_from/to_user + copy_probe with
  per-page prevalidation and the exact-RIP exception-fixup copy sites — are both
  done and QEMU-verified. With #2–#5 complete, the ADR 0007 M3a userspace
  boundary is implemented end to end (private address spaces, ring-3 execution,
  fault-local death, bounded syscalls, recoverable user copies), proven in QEMU
  and peer-cross-reviewed (not independent human qualification).
- **M3b:** typed handles with generation counters, rights attenuation, explicit
  object lifetime and checked syscall layouts. Test stale/forged handles and
  denied operations without side effects.
- **M3c:** bounded capability-aware IPC, waiting/cancellation semantics and init
  process startup, with race/timeout/lifetime tests.

Before calling M3 hardened, resolve entropy/canary strategy, general wait queues
and synchronization, spurious IRQ handling and a security review of the user/kernel
transition. Broader hardening remains a separate acceptance claim. Do not expand
storage, network, GPU or desktop scope as a substitute for these boundaries.

## Decision gates

Before imports: record provenance and review compatibility with the selected
Apache-2.0 project license; preserve each dependency's terms.
Before stable ABI: specify native wire layout, rights/lifetime and concurrency.
Before hardened profile: define exact minimum hardware and firmware/update policy.
Before accelerated desktop: select native toolkit/compositor strategy in an ADR.
Before foreign compatibility: scope exact ABI/application set and security mapping.

## Additional owner requirements (2026-10-13)

These design additions do not change the current implementation focus (M3b, then M3c).

- M3c/M4: [INIT.md](INIT.md), configurable supervisor with good defaults,
  generation-safe readiness, bounded restarts, recovery and explicit authority.
- M5a-c: [STORAGE.md](STORAGE.md), initfs/RAM filesystem, read-only ext4 candidate,
  then independently qualified writable storage. Production default remains open.
- M5/M6/M9/P2: [ENCRYPTION.md](ENCRYPTION.md), key lifecycle before format freeze,
  optional nearby sharing, reviewed PQ protection, qualified memory encryption
  and separate confidential-execution gates. No new crypto code is authorized
  for production use merely by this design; library/protocol review is required.

- M3/M4 through M9: [IDENTITY.md](IDENTITY.md), local principals/sessions first,
  then durable identity records, multi-method pairing and E2E sync qualification.
  Family/tenant membership and fleet scale require additional acceptance evidence;
  no cloud account is required for unmanaged local login.
- M5 and later format gates: owner-approved provisional ext4 default, raw block
  I/O with exclusive range leases, temporary/sidecar views without reformatting,
  and a qualified filesystem interoperability matrix. Additional formats are
  planned individually; no blanket current read/write support is implied.

- Enterprise follow-on after M3-M6/M9 prerequisites: [DOMAINS.md](DOMAINS.md),
  native directory/join lifecycle and centralized policy gates, followed by
  independently qualified AD DS client and selected GPO mapping gates.

  The interoperability matrix includes Windows/macOS/Linux endpoints, Microsoft
  and Samba AD providers, and separately scoped FreeIPA/LDAP/Kerberos integration.
  Qualify cross-platform agents and policy mappings individually.

## Accelerator/media requirements and license decision (2026-10-13)

Apache-2.0 is selected for original project code; see ADR 0005.
[M3A.md](M3A.md) supplies a proposed implementation sequence, not a completed gate.
M3a and M3b are complete; M3c IPC/init is in progress (ADR 0009; endpoint lifecycle landed). The ARM64 MP track runs alongside (MP1 booted).

[ACCELERATORS.md](ACCELERATORS.md) defines open-standards-first GPU/NPU support
across planned platforms, qualified per device. M4 supplies device/buffer/DMA
boundaries; M5/M6 supplies application/storage prerequisites and software media
ports with qualified CPU state; M7 adds desktop integration; M8 qualifies GPU/NPU
backends and measured automatic offloading. FFmpeg, HandBrake and Blackmagic RAW
are userspace integration targets, not current codecs or kernel features.

## Multitouch and security keys (2026-10-13)

[INPUT.md](INPUT.md): M4 input transports/lifecycle, M7 compositor/toolkit/secure
UI and accessibility, E1 touch device qualification. [SECURITY_KEYS.md](SECURITY_KEYS.md):
M4 authenticator transports, M5/M6 credential broker/storage, M7 enrollment UI,
M9 authentication/recovery qualification. Preboot support is a separate gate.
These requirements extend the later backlog; M3a remains the next kernel task.

## Fresh-kernel mobile track

[MOBILE.md](MOBILE.md) links auto44e/jeios-mobile: MP0 ARM64 contract, MP1-MP3
native virtual execution/isolation, MP4 Pixel feasibility, MP5-MP6 physical mobile
bring-up/services, AC0-AC2 future Android compatibility. Fresh C kernel remains
shared; no AOSP/Linux host or lunch target. (M3a and M3b are complete; M3c is in progress; ARM64 MP1 booted.)

## Zero trust throughout the ecosystem — 2026-10-14

[ZERO_TRUST.md](ZERO_TRUST.md) maps enforcement to M3a isolation, M3b rights,
M3c IPC, M4-M6 brokers/network/storage, M7-M9 consent/identity/recovery and mobile
qualification. Treat this as a requirement across gates, not a completed feature
or a reason to skip M3a. See HANDOFF.md for dated current status and history.

## Core standards commitment — 2026-10-16

[STANDARDS.md](STANDARDS.md): target POSIX.1-2024 as a core application environment;
M6 remains an initial tested subset followed by complete-profile qualification.
Built-in LDAPv3 provider/identity integration is also core, with optional enrollment.
M3-M6 and security prerequisites still apply; M3a and M3b are complete and M3c is the current focus.
Track [POSIX #15](https://github.com/auto44e/jeios/issues/15) and
[LDAP #16](https://github.com/auto44e/jeios/issues/16). Certification, AD DS/GPO and
cloud enrollment are separate goals, not implied completion.

## Curl-inspired CI direction — 2026-10-16

[CI_STRATEGY.md](CI_STRATEGY.md) records the owner's short/long-term testing goal:
portable regressions, compiler diversity, sanitizers/static analysis, replayable
failure injection and fuzzing, supplemented by native kernel execution and storage
crash-recovery qualification. [Core #18](https://github.com/auto44e/jeios/issues/18)
is the first bounded CI implementation task. Existing gates remain mandatory;
M3a and mobile MP0 remain next. This update adds planning only, not workflow jobs.

## Ecosystem coding discipline — 2026-10-16

[CODING_STANDARD.md](CODING_STANDARD.md) applies NASA/JPL-inspired rules with
stricter critical-component profiles. [Core #19](https://github.com/auto44e/jeios/issues/19)
tracks audit and enforcement with CI #18. Incremental adoption supports each gate;
it does not replace M3a or establish NASA certification.

 exited 127 in 0ms:
A  docs/adr/debates/mp1-vectors.md
M  kernel/arch/aarch64/boot.S
M  kernel/arch/aarch64/linker.ld
M  kernel/arch/aarch64/main.c
A  kernel/arch/aarch64/trapframe.h
A  kernel/arch/aarch64/vectors.S
M  mk/aarch64.mk
M  tools/smoke_arm.py
/usr/bin/bash: line 1: rg: command not found

exec
/usr/bin/bash -lc "nl -ba docs/adr/0002-arm64-mobile.md 2>/dev/null || find docs/adr -maxdepth 1 -type f -iname '*0002*' -print" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "nl -ba docs/adr/debates/mp1-vectors.md; find build -maxdepth 3 -type f \\( -name '*.elf' -o -name '*.o' -o -name 'Image' \\) -print 2>/dev/null | sort" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "sed -n '1,180p' mk/aarch64.mk; find . -maxdepth 3 -type f | sort | sed -n '1,240p'; find . -type f -maxdepth 5 | xargs grep -nE 'make (aarch64|smoke-aarch64)|objdump|aarch64-linux|SCTLR_EL1|0x30D00808' 2>/dev/null | head -200" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
docs/adr/0002-modularity-and-isolation.md

 succeeded in 0ms:
     1	# Peer debate — 2026-10-19T07:46:14Z
     2	Topic: JeiOS MP1 ARM64 VBAR_EL1 vectors + trap frame (D5-3): table layout, SP/reentrancy, frame struct + save asm, C diagnostic, BRK deliberate-exception test, alignment
     3	Rounds: 2   Roles: claude=implementor codex=adversarial-reviewer
     4	Context file: /tmp/mp1v.md
     5	
     6	
     7	[R1 CLAUDE] I agree with most of the brief. Pushback on five points: the re-entry guard, FAR handling, the exact ESR value, the SPSR assertion and the restore path. Facts below are from the AArch64 architecture; I checked `boot.S` and `linker.ld` at f46f4f9.
     8	
     9	## 1. Table: install all 16, only one reachable
    10	- **Reachable in MP1:** only Current-EL/SP_ELx Sync (vector index 4, offset 0x200).
    11	  - SError is masked by PSTATE.A, so it pends rather than vectors. The brief's "SError possible" is wrong while DAIF stays masked.
    12	  - SP_EL0 group: boot.S sets SPSel=1, so it is unreachable.
    13	  - Lower EL groups: no EL0 yet.
    14	- **Install all 16 anyway.** Each slot is `sub sp,sp,#TF_SIZE; stp x0,x1,[sp]; mov x0,#idx; b je_arm_vec_common`. Each slot is wrapped in `.balign 0x80` plus `.org slot_start+0x80`, so an overfull slot fails at assembly time instead of silently running into the next one.
    15	- **Alignment:** put the table in its own `.text.vectors` section with `.balign 2048`. In `linker.ld`, add an explicit `.text.vectors : ALIGN(2048) { KEEP(...) }`, because `*(.text*)` would also absorb it. Add `ASSERT((je_arm_vectors & 0x7FF) == 0)`.
    16	- **Install sequence:** `msr VBAR_EL1; isb`. The ISB is required before anything depends on the new VBAR, including the BRK test. Then read VBAR_EL1 back with `mrs` and print the comparison against `&je_arm_vectors`. That readback is the evidence that the install took.
    17	
    18	## 2. SP / reentrancy: one stack is fine; a flag in the saver is not
    19	- Taking an exception to EL1 selects SP_EL1 and sets all of PSTATE.DAIF. So the handler is non-reentrant for async exceptions by construction, and a single stack is acceptable for MP1.
    20	- **The proposed guard is broken.** If the guard flag is checked after `sub sp; stp`, a fault caused by a bad SP faults again on that same `stp`. The CPU re-vectors, subtracts again and faults again, forever. That is a livelock, which is exactly the "timeout, not observed behavior" outcome the task forbids.
    21	- There is no MMU and no guard page. Stack overflow therefore grows down silently into `.bss` (the stack sits just above BSS), and every frame push makes it worse.
    22	- **Fix: validate before touching the stack.** The slot's first instruction is `msr SP_EL0, x0`. SP_EL0 is free scratch in MP1 because there is no EL0 and SPSel=1. Then:
    23	  1. Check the guard flag using `adr` (PC-relative, no stack).
    24	  2. Check that SP is 16-aligned and that `SP-TF_SIZE ∈ [__stack_bottom, __stack_top)`.
    25	  3. If either check fails, branch to a stackless asm path (reuse boot.S's no-stack PL011 putc and halt) that prints `EXC NESTED|BADSP vec=N` and runs WFE forever.
    26	  4. If both pass, restore x0 with `mrs x0, SP_EL0`, then push.
    27	
    28	  This costs about 10 instructions, which fits in 32 per slot. Alternatively keep the slot to 4 instructions and do the checks in common code before any push. Then the `sub` has to move into common code too, and the slot carries the index in SP_EL0.
    29	- Documented hazard: SP_EL0 as scratch is MP1-only. When EL0 arrives, the lower-EL entry must save the user SP_EL0 first, and this scheme moves to TPIDR_EL1 or a per-CPU area. Record this as an invariant that changes later.
    30	- **Device-memory hazard:** with the MMU off, data accesses are Device-nGnRnE, so any unaligned access faults whatever SCTLR.A says. The frame has to keep strict 16-byte/8-byte alignment.
    31	
    32	## 3. Frame: proposed layout, owner sign-off needed (D5-3)
    33	```c
    34	struct je_arm_trap_frame {   /* 0x130 = 304 B, 16-aligned */
    35	    uint64_t x[31];          /* 0x000..0x0F0  x0..x30 (x18, x29 fp, x30 lr saved as plain GPRs) */
    36	    uint64_t sp;             /* 0x0F8  interrupted SP: entry sp + TF_SIZE (SP_ELx group) or SP_EL0 (SP_EL0/lower groups) */
    37	    uint64_t elr;            /* 0x100  ELR_EL1 — fixup key (D5-3) */
    38	    uint64_t spsr;           /* 0x108 */
    39	    uint64_t esr;            /* 0x110 */
    40	    uint64_t far;            /* 0x118  valid only for EC in {0x20,0x21,0x22,0x24,0x25,0x34,0x35} */
    41	    uint64_t vector;         /* 0x120  0..15 */
    42	    uint64_t _pad;           /* 0x128 */
    43	};
    44	```
    45	- Offsets come from one shared header, `#define JE_TF_ELR 0x100` and so on, used by both the asm and C. C carries `_Static_assert(offsetof(...) == JE_TF_*)` for every field plus a size assert.
    46	- **Save order:**
    47	  1. `stp` pairs x0/x1 through x28/x29.
    48	  2. `str x30`.
    49	  3. `add x21, sp, #TF_SIZE; str x21` into the sp slot.
    50	  4. `mrs` ELR, SPSR, ESR, FAR into four registers, then two `stp`s.
    51	  5. `str` the vector index.
    52	  6. `mov x0, sp; bl je_arm_exception`.
    53	  7. If C returns, fall through to a halt.
    54	- **No FP/SIMD state:** `-mgeneral-regs-only` means the frame does not hold Q registers. Declare it "complete for GPR state only." Any accidental FP use traps with EC 0x07, which this handler would report.
    55	- **No restore/`eret` path in this PR.** An untested restore is dead code that a later PR would trust. The frame is write-once. The layout already leaves room for a symmetric restore; add it when there is a returning path to test.
    56	- **FAR:** the architecture defines FAR_EL1 as UNKNOWN for BRK, so the brief's "FAR (the faulting VA)" is wrong for the test. C prints `far=n/a` unless the EC is one of the abort, PC-alignment or watchpoint classes. The test must never assert FAR.
    57	- **Owner flag:** the frame layout is the D5-3-constrained ABI, and future fixups key on `elr`. It needs owner sign-off; agent majority cannot settle it.
    58	
    59	## 4. Diagnostic: fixed format, bounded output, no GPR dump
    60	```
    61	JEIOS-ARM: VBAR ok vbar=0x0000000040081000 expect=0x0000000040081000     (before any test)
    62	JEIOS-ARM: EXC vec=4 ec=0x3c il=1 iss=0x0004a45 esr=0x00000000f2004a45 elr=0x... spsr=0x00000000000003c5 far=n/a
    63	JEIOS-ARM: HALT unhandled-exception
    64	```
    65	- All values are fixed-width hex from a non-allocating formatter. No GPRs and no x29/x30 are printed; that is the redaction.
    66	- Absolute addresses are fine for MP1 because there is no KASLR and load==link is enforced. Note the future redaction policy, `elr` printed as `_text+off`, in the docs.
    67	- The C handler must be leaf-ish: no stack protector, and only the polling putc.
    68	
    69	## 5. BRK test: right trigger; correcting the expected values
    70	- **Masking:** BRK is a synchronous Breakpoint Instruction exception. PSTATE.D masks breakpoint, watchpoint and step debug exceptions, but not the BRK instruction. So BRK vectors with DAIF=0xF. Confirmed.
    71	- **Exact ESR.** The brief left out the IL bit:
    72	  - Formula: ESR = `(0x3C<<26) | (IL=1<<25) | imm16`.
    73	  - With `brk #0x4a45` ("JE"), ESR = **0xF2004A45**.
    74	  - Assert EC=0x3C, IL=1, ISS[15:0]=0x4A45, and ISS[24:16]=0.
    75	- **ELR:** equals the address of the BRK instruction itself. Unlike SVC, the preferred return address is the BRK, not BRK+4. Assert `elr == &je_arm_vector_test_brk`, using a global label on the instruction. This is exactly the PC-keyed property D5-3's fixup will depend on.
    76	- **SPSR:** assert `(spsr & 0x3CF) == 0x3C5` (M=EL1h, DAIF=1111). Mask out NZCV and other bits, because they depend on preceding code.
    77	- **Vector:** assert `vector == 4`.
    78	- **Where the check runs:** under `-DJE_ARM_VECTOR_TEST`, after the step-1 markers and the VBAR readback line. The C handler compares every field and prints `VECTOR-TEST PASS` or `VECTOR-TEST FAIL field=...`, then halts.
    79	- **What the smoke run asserts:** the PASS line, the raw `esr=0x00000000f2004a45`, and the absence of FAIL. The harness checks the raw value independently of the in-kernel comparator, so a bug in that comparator can't produce a false pass.
    80	- **Normal boot:** installs the vectors and prints the VBAR readback, but executes no BRK.
    81	
    82	## 6. Other gotchas
    83	- `b` from a slot to common code has ±128 MiB reach, which is fine.
    84	- The saver is pure GPR code with no FP/SIMD and no exclusives.
    85	- MMU off means VA==PA. The table sits in loaded `.text` (not NOLOAD), so it is present in the image.
    86	- D-cache is off and the code is never modified at runtime, so there are no I/D coherency concerns. That changes at MP2: when the MMU comes up, the VBAR must stay valid across the switch, so re-point VBAR or identity-map it.
    87	- SCTLR_EL1 state on entry isn't defined by the boot protocol beyond the MMU and D-cache being off. Recommend boot.S write SCTLR_EL1 to a known value (SA=1, A=0, M=0) so the fault behaviour the handler depends on is deterministic.
    88	
    89	**Open items for the other engineer:**
    90	- Do you accept SP_EL0-as-scratch for the stack check, or do you want a dedicated exception stack now? I argue against the stack: there is nothing that nests yet.
    91	- Should the SCTLR_EL1 write go in this PR or in a separate one?
    92	[R1 CODEX] codex
    93	I’ll pressure-test the proposed design against the architectural edge cases, especially the SP_EL0 scratch idea, exception-entry state, BRK semantics, and frame correctness.
    94	web search: 
    95	web search: site:developer.arm.com AArch64 exception vector table VBAR_EL1 2048 bytes SP_EL1 exception entry DAIF
    96	web search: 
    97	web search: site:developer.arm.com BRK PSTATE.D masked AArch64 breakpoint instruction exception not masked
    98	web search: 
    99	web search: site:developer.arm.com AArch64 BRK ELR_EL1 address of BRK instruction
   100	web search: 
   101	web search: site:developer.arm.com SCTLR_EL1 M 0 MMU disabled memory type Normal non-cacheable AArch64
   102	web search: 
   103	web search: site:developer.arm.com FAR_EL1 valid exception class SP alignment PC alignment watchpoint AArch64
   104	web search: 
   105	web search: AArch64 BRK instruction PSTATE.D exception masked source
   106	web search: 
   107	web search: AArch64 SoftwareBreakpoint PSTATE.D BRK pseudocode
   108	web search: 
   109	web search: site:developer.arm.com SError PSTATE.A uncontainable error mask
   110	codex
   111	I agree with Claude on the 2 KiB table, `VBAR_EL1; isb`, vector 4 for Current EL/SP_ELx Sync, FAR=`n/a` for BRK, no `eret` path, and the frame offsets. FAR is architecturally defined for aborts, PC alignment, and watchpoints—not BRK. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/vbar_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/b/aarch64-system-registers/far_el1))
   112	I dispute the proposed SP_EL0 scratch protocol:
   113	- It is internally incomplete: SP_EL0 stores original `x0`, but the vector index is also proposed to live there. There is no free GPR for bounds checking while preserving `x1`–`x30`.
   114	- It destroys the future lower-EL user stack. Lower-EL entry must preserve SP_EL0 before using it.
   115	- A guard checked after `sub sp`/`stp` cannot detect failure of that first push; Claude correctly identifies the recursive-fault risk, but the replacement does not solve register preservation.
   116	For MP1 I recommend the narrower contract: trust the boot-established, 16-byte-aligned SP_EL1 range; save `x0/x1` first, then set/check `in_handler`. Nested exceptions after that point take a stackless fatal path. Explicitly document that a pre-frame stack fault is outside the MP1 guarantee. A genuinely stackless precheck requires reserved per-CPU/system-register scratch or a separately designed exception stack.
   117	The frame is otherwise sound:
   118	```c
   119	uint64_t x[31];   // x0..x30; no x31 exists to save
   120	uint64_t sp;
   121	uint64_t elr;
   122	uint64_t spsr;
   123	uint64_t esr;
   124	uint64_t far;
   125	uint64_t vector;
   126	uint64_t pad;
   127	```
   128	The `sp` contract must distinguish interrupted SP_EL1 from SP_EL0/user SP for future entries. The field order and offsets are D5-3 ABI decisions and require owner sign-off. `x18`, `x29`, and `x30` are ordinary saved GPRs; compiler disassembly must verify the C ABI assumptions.
   129	BRK remains the best deterministic test, but record higher-level debug routing as a prerequisite; BRK is a software breakpoint/debug exception, not merely an ordinary data fault. Assert defined ESR fields and the low 32 bits:
   130	```text
   131	EC   = 0x3c
   132	IL   = 1
   133	ISS  = 0x4a45
   134	ESR[31:0] = 0xf2004a45
   135	ELR  = address of the BRK instruction
   136	vector = 4
   137	(spsr & 0x3cf) == 0x3c5
   138	```
   139	Do not require the entire 64-bit ESR unless the implementation explicitly guarantees upper bits. Arm documents BRK’s `EC=0x3c` and immediate in ISS. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/a/aarch64-system-registers/esr_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/elr_el1))
   140	Missed issues:
   141	- “MMU off means Device-nGnRnE” is wrong. With `SCTLR_EL1.M=0`, Normal-memory accesses are non-cacheable; UART’s Device mapping is a separate concern. The alignment discipline remains necessary, but that rationale should be corrected. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/b/aarch64-system-registers/sctlr_el1))
   142	- `adr` is only ±1 MiB; use `adrp` or linker assertions. `b` is ±128 MiB.
   143	- A bounded diagnostic requires bounded UART polling and a maximum output length, not merely a non-allocating formatter.
   144	- `VBAR` readback proves register installation, not executable table contents; the BRK smoke marker supplies the latter.
   145	- Ordinary SError is masked by `PSTATE.A` in this boot state, so vector 4 is the normal reachable path; retain all 16 entries because exceptional/unexpected routing must still land in known code. DAIF masking semantics are architectural. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-and-interrupt-handling))
   146	My position: accept the table, frame shape, FAR policy, and BRK test; reject SP_EL0-as-scratch until its scratch-register and future lower-EL contracts are made explicit.
   147	I agree with Claude on the 2 KiB table, `VBAR_EL1; isb`, vector 4 for Current EL/SP_ELx Sync, FAR=`n/a` for BRK, no `eret` path, and the frame offsets. FAR is architecturally defined for aborts, PC alignment, and watchpoints—not BRK. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/vbar_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/b/aarch64-system-registers/far_el1))
   148	I dispute the proposed SP_EL0 scratch protocol:
   149	- It is internally incomplete: SP_EL0 stores original `x0`, but the vector index is also proposed to live there. There is no free GPR for bounds checking while preserving `x1`–`x30`.
   150	- It destroys the future lower-EL user stack. Lower-EL entry must preserve SP_EL0 before using it.
   151	- A guard checked after `sub sp`/`stp` cannot detect failure of that first push; Claude correctly identifies the recursive-fault risk, but the replacement does not solve register preservation.
   152	For MP1 I recommend the narrower contract: trust the boot-established, 16-byte-aligned SP_EL1 range; save `x0/x1` first, then set/check `in_handler`. Nested exceptions after that point take a stackless fatal path. Explicitly document that a pre-frame stack fault is outside the MP1 guarantee. A genuinely stackless precheck requires reserved per-CPU/system-register scratch or a separately designed exception stack.
   153	The frame is otherwise sound:
   154	```c
   155	uint64_t x[31];   // x0..x30; no x31 exists to save
   156	uint64_t sp;
   157	uint64_t elr;
   158	uint64_t spsr;
   159	uint64_t esr;
   160	uint64_t far;
   161	uint64_t vector;
   162	uint64_t pad;
   163	```
   164	The `sp` contract must distinguish interrupted SP_EL1 from SP_EL0/user SP for future entries. The field order and offsets are D5-3 ABI decisions and require owner sign-off. `x18`, `x29`, and `x30` are ordinary saved GPRs; compiler disassembly must verify the C ABI assumptions.
   165	BRK remains the best deterministic test, but record higher-level debug routing as a prerequisite; BRK is a software breakpoint/debug exception, not merely an ordinary data fault. Assert defined ESR fields and the low 32 bits:
   166	```text
   167	EC   = 0x3c
   168	IL   = 1
   169	ISS  = 0x4a45
   170	ESR[31:0] = 0xf2004a45
   171	ELR  = address of the BRK instruction
   172	vector = 4
   173	(spsr & 0x3cf) == 0x3c5
   174	```
   175	Do not require the entire 64-bit ESR unless the implementation explicitly guarantees upper bits. Arm documents BRK’s `EC=0x3c` and immediate in ISS. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/a/aarch64-system-registers/esr_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/elr_el1))
   176	Missed issues:
   177	- “MMU off means Device-nGnRnE” is wrong. With `SCTLR_EL1.M=0`, Normal-memory accesses are non-cacheable; UART’s Device mapping is a separate concern. The alignment discipline remains necessary, but that rationale should be corrected. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/b/aarch64-system-registers/sctlr_el1))
   178	- `adr` is only ±1 MiB; use `adrp` or linker assertions. `b` is ±128 MiB.
   179	- A bounded diagnostic requires bounded UART polling and a maximum output length, not merely a non-allocating formatter.
   180	- `VBAR` readback proves register installation, not executable table contents; the BRK smoke marker supplies the latter.
   181	- Ordinary SError is masked by `PSTATE.A` in this boot state, so vector 4 is the normal reachable path; retain all 16 entries because exceptional/unexpected routing must still land in known code. DAIF masking semantics are architectural. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-and-interrupt-handling))
   182	My position: accept the table, frame shape, FAR policy, and BRK test; reject SP_EL0-as-scratch until its scratch-register and future lower-EL contracts are made explicit.
   183	[R2 CLAUDE] **Implementor response to Codex: MP1 ARM64 vectors and trap frame (D5-3)**
   184	
   185	Most of Codex's points survive. I concede the SP_EL0 scratch protocol. I dispute the claim that a stackless precheck needs reserved scratch, and I refine three of its assertions.
   186	
   187	### Agreed
   188	- The 2 KiB table, `msr VBAR_EL1; isb`, all 16 entries present, vector 4 as the only normally reachable entry, no `eret` path, `adrp`+`b` instead of `adr`, and correcting the "MMU off means Device memory" rationale.
   189	- **SP_EL0 scratch is withdrawn.** Codex is right that it has no free register for the vector index and that it collides with the future lower-EL SP_EL0 contract. The D5-3 frame should not bake in a scheme we have to undo at EL0 bring-up.
   190	- Exception entry to EL1 sets PSTATE.{D,A,I,F}=1 in hardware. "DAIF stays masked in the handler" is architectural and needs no code. The rule is simply "never unmask in an MP1 handler".
   191	
   192	### Disputed: a stackless precheck does not need reserved scratch
   193	Linux arm64 `kernel_ventry` checks for stack overflow with no free register, using an add/sub swap:
   194	```asm
   195	sub  sp, sp, #FRAME_SIZE
   196	add  sp, sp, x0          // sp' = sp + x0
   197	sub  x0, sp, x0          // x0 = original sp
   198	tbnz x0, #STACK_SHIFT, overflow
   199	sub  x0, sp, x0          // x0 restored
   200	sub  sp, sp, x0          // sp restored
   201	```
   202	- **Prerequisite:** the 16 KiB boot stack is aligned to 32 KiB in the linker script (`ASSERT`ed). A valid SP then has bit 14 clear, and an underflow of up to 16 KiB sets it.
   203	- **Cost:** about 6 instructions, which fits in the 32-instruction (128-byte) entry.
   204	- **Effect:** it closes the hole Codex says MP1 must leave open ("a pre-frame stack fault is outside the guarantee"), at negligible cost.
   205	- **Overflow path only:** stash x0 in TPIDR_EL1, which is unused in MP1, then switch to a 4 KiB emergency stack.
   206	- **Flag for later:** TPIDR_EL1 will become the per-CPU pointer at SMP. That reservation must be revisited then, and Linux uses TPIDR_EL0/TPIDRRO_EL0 for the same reason.
   207	
   208	This gives three fatal tiers:
   209	1. Normal path: the frame goes on SP_EL1, then C prints the diagnostic and halts.
   210	2. Overflow, or nested entry while `in_handler` is set: switch to the emergency stack, print a minimal "nested/overflow" line plus the second ESR, then halt.
   211	3. A fault while already on the emergency stack: a bare `wfe` loop with no output. This is still a bounded halt.
   212	
   213	**Ordering hazard Codex missed:** a nested exception overwrites ELR, SPSR, ESR and FAR. The saver must copy those into the frame immediately after the first `stp x0, x1`, before setting `in_handler` and before any other work. Otherwise the first fault's state is lost exactly when it matters.
   214	
   215	### Refinements to Codex's assertions
   216	- **SPSR mask.** `0x3cf` skips M[4], so it would not detect AArch32 state. Use `(spsr & 0x3df) == 0x3c5`: DAIF set, M[4]=0 (AArch64), M[3:0]=EL1h.
   217	- **One `sp` field is enough.** Its meaning is "SP of the interrupted context, as selected by SPSR.M". For the current-EL groups it is `sp + FRAME_SIZE`. For lower-EL groups it comes from `mrs sp_el0`. Defining it per vector group avoids a second field that is meaningless half the time. This is an ABI choice, so it goes to the owner.
   218	- **ESR.** Agree on asserting the low 32 bits. On Cortex-A72 (v8.0) bits [63:32] are RES0, and RES0 is not a value to rely on. We can log the full 64 bits as an observation without making it part of the assertion.
   219	- **ELR redaction and determinism.** Print ELR as an offset from the image base (`ELR - _start`), and assert `ELR == je_arm_brk_site`, a global label in the asm. Raw kernel addresses become sensitive once KASLR exists, and the offset is the same regardless of where QEMU loads the image. Same for FAR when it is valid.
   220	- **Debug routing.** Agree it is a prerequisite. More precisely: BRK is not masked by PSTATE.D, but it routes to EL2 if EL2 exists with MDCR_EL2.TDE=1 or HCR_EL2.TGE=1. The smoke must pin `virtualization=off` (no EL2) and record that.
   221	
   222	### Missed by both
   223	- **SCTLR_EL1 must be written explicitly in boot.** Many of its bits reset to an architecturally UNKNOWN value, including SA, the SP-alignment check. Boot should write a defined value (M=0, C=0, SA=1, A per policy). Otherwise whether a misaligned `stp` faults is not deterministic. If step 1 did not do this, it is a step-1 gap.
   224	- **No I-cache maintenance needed.** The table is static image content loaded by QEMU and nothing modifies it at runtime. `isb` after the VBAR write is sufficient.
   225	- **The guard needs no atomics.** `in_handler` is a plain byte store: single CPU, no reentrancy before the flag is set, and the MMU off is fine for a Normal non-cacheable store. That fits the no-exclusives constraint.
   226	
   227	### Frame
   228	I accept Codex's layout:
   229	
   230	| Offset | Field |
   231	|---|---|
   232	| 0 | `x[31]` |
   233	| 248 | `sp` |
   234	| 256 | `elr` |
   235	| 264 | `spsr` |
   236	| 272 | `esr` |
   237	| 280 | `far` |
   238	| 288 | `vector` |
   239	| 296 | `pad` |
   240	
   241	Size is 304 bytes, which is 16-aligned, and `elr`/`spsr` form a 16-aligned `stp` pair. Offsets live in one shared header used by both the asm and the C code, checked with `_Static_assert(offsetof(...))`. The field order is D5-3 ABI and needs owner sign-off.
   242	
   243	### Bounded diagnostic
   244	- At most 256 bytes, written through the existing bounded-poll PL011 path. On TX timeout the output is dropped and the handler still halts.
   245	- Fields: vector, EC, IL, ISS, ELR offset, SPSR, FAR (printed as `n/a` unless the EC defines it), and `VBAR == &table`.
   246	- VBAR readback proves the register was installed. The BRK marker proves the table contents execute. Both are needed.
   247	
   248	**Position:** adopt Codex's frame and BRK assertions with the 0x3df mask, use the stack-alignment overflow precheck instead of SP_EL0 scratch, reserve TPIDR_EL1 for the fatal path only until SMP, and add the explicit SCTLR_EL1 write.
   249	[R2 CODEX] codex
   250	I agree with the broad direction: install all 16 slots, use no SP_EL0 scratch protocol, keep handlers non-returning, add VBAR readback, and use `adrp`/`add`/`br`-style position-independent reachability. The 32-KiB alignment precheck is valid only if the linker and stack invariant are explicit and asserted.
   251	I dispute or qualify these points:
   252	- `SPSR == 0x3c5` is test-specific, not an architectural handler invariant. `SPSR_EL1` captures pre-exception PSTATE; exception entry then masks DAIF. It is `0x3c5` here only because boot deliberately has DAIF masked and execution is EL1h. Assert `M[4:0]` and expected DAIF separately. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling))
   253	- BRK is a valid trigger even with D set: Arm documents software breakpoint exceptions as synchronous and unmaskable. The remaining routing prerequisite is proving execution remains at EL1, not merely assuming QEMU’s virtualization setting. ([developer.arm.com](https://developer.arm.com/docs/den0024/a/debug/arm-debug-hardware/debug-events))
   254	- The overflow scheme is not yet a complete nesting design. `in_handler` must be set before any potentially faulting access, and a second flag or explicit SP-range test is required to distinguish “nested on normal stack” from “nested on emergency stack.” Otherwise the claimed bare-WFE tier is unspecified.
   255	- The emergency path using `TPIDR_EL1` is acceptable only as a temporary MP1 contract. It destroys any existing per-CPU value and therefore cannot be described as future-proof; SMP must redesign this path before assigning TPIDR_EL1 as the CPU pointer.
   256	- “Copy system registers immediately after the first `stp`” does not preserve the first frame if the first stack access or subsequent store faults. It only makes the common path less vulnerable. If nested-fault preservation is required, the emergency scratch protocol must be specified; otherwise document nested faults as fatal with partial first-frame state.
   257	- The frame description is internally ambiguous. `x[31]` means x0–x30, but the prose also says “a slot for x31/alignment.” Those are different layouts. Either use `gpr[31]` with no x31 slot, or define a 32-slot register area and freeze the larger ABI. This is a D5-3 owner decision.
   258	- No mechanism transports the vector index into a common saver. A plain `b common_saver` loses the originating slot. Each entry needs a small index-bearing stub, or the saver must derive the slot from a preserved entry PC.
   259	- The stack precheck catches crossing the designated 16-KiB boundary; it does not validate arbitrary corrupted SP values. State it as a bounded invariant check, not general stack safety.
   260	- “Normal non-cacheable store” is the wrong MMU-off rationale. The relevant memory type must be stated from the configured architectural regime; do not rely on an unverified cacheability claim.
   261	- Explicit SCTLR initialization is a legitimate prerequisite, but write a value respecting RES1/RES0 fields; “write zero except SA/M/C” is unsafe.
   262	For the BRK test, use `brk #0xA55A` and assert:
   263	```text
   264	EC       == 0x3c
   265	IL       == 1
   266	ISS[15:0] == 0xA55A
   267	ESR_EL1[31:0] == 0xF200A55A
   268	ELR_EL1 == &je_arm_brk_site
   269	```
   270	Also assert the handler’s VBAR readback equals the aligned table address and that the observed vector index is the EL1h synchronous slot (index 4). The exact frame field order and whether emergency nesting is required beyond bounded halt remain owner-escalated contract choices.
   271	I agree with the broad direction: install all 16 slots, use no SP_EL0 scratch protocol, keep handlers non-returning, add VBAR readback, and use `adrp`/`add`/`br`-style position-independent reachability. The 32-KiB alignment precheck is valid only if the linker and stack invariant are explicit and asserted.
   272	I dispute or qualify these points:
   273	- `SPSR == 0x3c5` is test-specific, not an architectural handler invariant. `SPSR_EL1` captures pre-exception PSTATE; exception entry then masks DAIF. It is `0x3c5` here only because boot deliberately has DAIF masked and execution is EL1h. Assert `M[4:0]` and expected DAIF separately. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling), [developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling))
   274	- BRK is a valid trigger even with D set: Arm documents software breakpoint exceptions as synchronous and unmaskable. The remaining routing prerequisite is proving execution remains at EL1, not merely assuming QEMU’s virtualization setting. ([developer.arm.com](https://developer.arm.com/docs/den0024/a/debug/arm-debug-hardware/debug-events))
   275	- The overflow scheme is not yet a complete nesting design. `in_handler` must be set before any potentially faulting access, and a second flag or explicit SP-range test is required to distinguish “nested on normal stack” from “nested on emergency stack.” Otherwise the claimed bare-WFE tier is unspecified.
   276	- The emergency path using `TPIDR_EL1` is acceptable only as a temporary MP1 contract. It destroys any existing per-CPU value and therefore cannot be described as future-proof; SMP must redesign this path before assigning TPIDR_EL1 as the CPU pointer.
   277	- “Copy system registers immediately after the first `stp`” does not preserve the first frame if the first stack access or subsequent store faults. It only makes the common path less vulnerable. If nested-fault preservation is required, the emergency scratch protocol must be specified; otherwise document nested faults as fatal with partial first-frame state.
   278	- The frame description is internally ambiguous. `x[31]` means x0–x30, but the prose also says “a slot for x31/alignment.” Those are different layouts. Either use `gpr[31]` with no x31 slot, or define a 32-slot register area and freeze the larger ABI. This is a D5-3 owner decision.
   279	- No mechanism transports the vector index into a common saver. A plain `b common_saver` loses the originating slot. Each entry needs a small index-bearing stub, or the saver must derive the slot from a preserved entry PC.
   280	- The stack precheck catches crossing the designated 16-KiB boundary; it does not validate arbitrary corrupted SP values. State it as a bounded invariant check, not general stack safety.
   281	- “Normal non-cacheable store” is the wrong MMU-off rationale. The relevant memory type must be stated from the configured architectural regime; do not rely on an unverified cacheability claim.
   282	- Explicit SCTLR initialization is a legitimate prerequisite, but write a value respecting RES1/RES0 fields; “write zero except SA/M/C” is unsafe.
   283	For the BRK test, use `brk #0xA55A` and assert:
   284	```text
   285	EC       == 0x3c
   286	IL       == 1
   287	ISS[15:0] == 0xA55A
   288	ESR_EL1[31:0] == 0xF200A55A
   289	ELR_EL1 == &je_arm_brk_site
   290	```
   291	Also assert the handler’s VBAR readback equals the aligned table address and that the observed vector index is the EL1h synchronous slot (index 4). The exact frame field order and whether emergency nesting is required beyond bounded halt remain owner-escalated contract choices.
build/0/jeios.elf
build/1/jeios.elf
build/10/jeios.elf
build/11/jeios.elf
build/2/jeios.elf
build/3/jeios.elf
build/4/jeios.elf
build/5/jeios.elf
build/6/jeios.elf
build/7/jeios.elf
build/8/jeios.elf
build/9/jeios.elf
build/aarch64-bsscheck/Image
build/aarch64-bsscheck/boot.o
build/aarch64-bsscheck/jeios-arm.elf
build/aarch64-bsscheck/main.o
build/aarch64-bsscheck/vectors.o
build/aarch64-vectest/Image
build/aarch64-vectest/boot.o
build/aarch64-vectest/jeios-arm.elf
build/aarch64-vectest/main.o
build/aarch64-vectest/vectors.o
build/aarch64/Image
build/aarch64/boot.o
build/aarch64/jeios-arm.elf
build/aarch64/main.o
build/aarch64/vectors.o

 succeeded in 475ms:
# SPDX-License-Identifier: Apache-2.0
# MP1 step 1 (ADR 0002): ARM64 (QEMU virt-8.2 / cortex-a72) build fragment.
# Deliberately separate from the x86-64 rules so the default build/test/smoke/
# integration flow is untouched. Invoked via `make aarch64` and
# `make smoke-aarch64`. Toolchain is the pinned aarch64-linux-gnu cross set.

ARM_CC      := aarch64-linux-gnu-gcc
ARM_LD      := aarch64-linux-gnu-ld
ARM_OBJCOPY := aarch64-linux-gnu-objcopy
ARM_BUILD   := build/aarch64
ARM_ARCH    := kernel/arch/aarch64

# Optional patterned-BSS verification build (proves the clear actually zeroes,
# since QEMU RAM starts zero). `make aarch64 ARM_BSS_CHECK=1`. It uses a SEPARATE
# build dir so toggling the flag never reuses stale objects from the default
# build (a config change must not be silently ignored).
ifeq ($(ARM_BSS_CHECK),1)
ARM_BUILD   := build/aarch64-bsscheck
endif
# Optional deliberate-exception build (MP1 vectors): plants a BRK #0xA55A after
# installing vectors and asserts the exact syndrome in the handler.
# `make aarch64 ARM_VECTOR_TEST=1`. Separate build dir so the toggle never reuses
# stale objects.
ifeq ($(ARM_VECTOR_TEST),1)
ARM_BUILD   := build/aarch64-vectest
endif

# Freestanding, no PIC/stack-protector, STRICT alignment (pre-MMU RAM is
# Device-nGnRnE so unaligned accesses fault), and general-regs-only (a stray
# FP/SIMD op would trap into absent vectors and hang). No native tuning.
ARM_CFLAGS  := -Iinclude -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion \
               -Wshadow -Wstrict-prototypes -ffreestanding -fno-builtin \
               -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
               -mstrict-align -mgeneral-regs-only -mcpu=cortex-a72
ARM_ASFLAGS := -Iinclude -mstrict-align -mgeneral-regs-only -mcpu=cortex-a72
# Orphan sections (.got, .data.rel.ro, .init_array, ...) must not be silently
# placed outside the image extent — make an unhandled section a build error.
ARM_LDFLAGS := -nostdlib --orphan-handling=error

# Optional patterned-BSS verification build (proves the clear actually zeroes,
# since QEMU RAM starts zero). `make aarch64 ARM_BSS_CHECK=1`.
ifeq ($(ARM_BSS_CHECK),1)
ARM_CFLAGS  += -DJE_ARM_BSS_CHECK
ARM_ASFLAGS += -DJE_ARM_BSS_CHECK
endif
ifeq ($(ARM_VECTOR_TEST),1)
ARM_CFLAGS  += -DJE_ARM_VECTOR_TEST
ARM_ASFLAGS += -DJE_ARM_VECTOR_TEST
endif

ARM_OBJS := $(ARM_BUILD)/boot.o $(ARM_BUILD)/main.o $(ARM_BUILD)/vectors.o

$(ARM_BUILD)/boot.o: $(ARM_ARCH)/boot.S $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_ASFLAGS) -c $< -o $@

$(ARM_BUILD)/vectors.o: $(ARM_ARCH)/vectors.S $(ARM_ARCH)/trapframe.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/main.o: $(ARM_ARCH)/main.c $(ARM_ARCH)/trapframe.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/jeios-arm.elf: $(ARM_OBJS) $(ARM_ARCH)/linker.ld
	$(ARM_LD) $(ARM_LDFLAGS) -T $(ARM_ARCH)/linker.ld -o $@ $(ARM_OBJS)

$(ARM_BUILD)/Image: $(ARM_BUILD)/jeios-arm.elf
	$(ARM_OBJCOPY) -O binary $< $@

.PHONY: aarch64 smoke-aarch64
aarch64: $(ARM_BUILD)/Image
	@echo "aarch64 Image: $$(wc -c <$(ARM_BUILD)/Image) bytes"

# Boot the Image on the pinned QEMU target and assert the ordered markers. The
# harness captures serial to a FILE (stdout races the timeout kill), rejects any
# FATAL line, requires the terminal marker, records image SHA-256 + tool
# versions + the exact command, and SKIPs (never PASSes) if the toolchain or
# qemu-system-aarch64 is absent. Not wired into `make check`.
smoke-aarch64: $(ARM_BUILD)/Image
	$(PYTHON) tools/smoke_arm.py $(ARM_BUILD)/Image $(if $(filter 1,$(ARM_BSS_CHECK)),--bss-verified,) $(if $(filter 1,$(ARM_VECTOR_TEST)),--vector-test,)
./.git/COMMIT_EDITMSG
./.git/FETCH_HEAD
./.git/HEAD
./.git/ORIG_HEAD
./.git/config
./.git/description
./.git/hooks/applypatch-msg.sample
./.git/hooks/commit-msg.sample
./.git/hooks/fsmonitor-watchman.sample
./.git/hooks/post-update.sample
./.git/hooks/pre-applypatch.sample
./.git/hooks/pre-commit.sample
./.git/hooks/pre-merge-commit.sample
./.git/hooks/pre-push.sample
./.git/hooks/pre-rebase.sample
./.git/hooks/pre-receive.sample
./.git/hooks/prepare-commit-msg.sample
./.git/hooks/push-to-checkout.sample
./.git/hooks/sendemail-validate.sample
./.git/hooks/update.sample
./.git/index
./.git/info/exclude
./.git/logs/HEAD
./.git/packed-refs
./.github/workflows/ci.yml
./.gitignore
./AGENTS.md
./CONTRIBUTING.md
./LICENSE
./Makefile
./README.md
./boot/grub/grub.cfg
./build/0/jeios.elf
./build/0/jeios.iso
./build/0/test_alloc
./build/0/test_boot
./build/0/test_core
./build/0/test_endpoint
./build/0/test_handle
./build/0/test_handle_rw
./build/0/test_syscall
./build/0/test_uframe
./build/0/test_vm
./build/1/jeios.elf
./build/1/jeios.iso
./build/10/jeios.elf
./build/10/jeios.iso
./build/11/jeios.elf
./build/11/jeios.iso
./build/2/jeios.elf
./build/2/jeios.iso
./build/3/jeios.elf
./build/3/jeios.iso
./build/4/jeios.elf
./build/4/jeios.iso
./build/5/jeios.elf
./build/5/jeios.iso
./build/6/jeios.elf
./build/6/jeios.iso
./build/7/jeios.elf
./build/7/jeios.iso
./build/8/jeios.elf
./build/8/jeios.iso
./build/9/jeios.elf
./build/9/jeios.iso
./build/aarch64-bsscheck/Image
./build/aarch64-bsscheck/boot.o
./build/aarch64-bsscheck/jeios-arm.elf
./build/aarch64-bsscheck/main.o
./build/aarch64-bsscheck/vectors.o
./build/aarch64-vectest/Image
./build/aarch64-vectest/boot.o
./build/aarch64-vectest/jeios-arm.elf
./build/aarch64-vectest/main.o
./build/aarch64-vectest/vectors.o
./build/aarch64/Image
./build/aarch64/boot.o
./build/aarch64/jeios-arm.elf
./build/aarch64/main.o
./build/aarch64/vectors.o
./docs/ACCELERATORS.md
./docs/AGENT_START.md
./docs/CI_STRATEGY.md
./docs/CODING_STANDARD.md
./docs/DESIGN.md
./docs/DEVELOPMENT.md
./docs/DOMAINS.md
./docs/ENCRYPTION.md
./docs/HANDOFF.md
./docs/HARDWARE.md
./docs/IDENTITY.md
./docs/IMPLEMENTATION.md
./docs/INIT.md
./docs/INPUT.md
./docs/M3A.md
./docs/M3A_RETRO.md
./docs/MOBILE.md
./docs/REFERENCES.md
./docs/ROADMAP.md
./docs/SECURITY.md
./docs/SECURITY_KEYS.md
./docs/STANDARDS.md
./docs/STORAGE.md
./docs/ZERO_TRUST.md
./docs/adr/0000-decision-record-policy.md
./docs/adr/0001-fresh-c-kernel.md
./docs/adr/0002-modularity-and-isolation.md
./docs/adr/0003-baseline-and-negotiated-features.md
./docs/adr/0004-user-experience-contract.md
./docs/adr/0005-licensing.md
./docs/adr/0006-bsp-foundation-and-page-heap.md
./docs/adr/0007-m3a-userspace-boundary.md
./docs/adr/0008-m3b-typed-handles.md
./docs/adr/0009-m3c-capability-ipc.md
./include/jeios/abi.h
./include/jeios/alloc.h
./include/jeios/arch.h
./include/jeios/boot.h
./include/jeios/endpoint.h
./include/jeios/endpoint_kern.h
./include/jeios/features.h
./include/jeios/handle.h
./include/jeios/kheap.h
./include/jeios/module.h
./include/jeios/paging.h
./include/jeios/sched.h
./include/jeios/status.h
./include/jeios/syscall.h
./include/jeios/trap.h
./include/jeios/uframe.h
./include/jeios/usercopy.h
./include/jeios/vm.h
./kernel/core/alloc.c
./kernel/core/boot.c
./kernel/core/endpoint.c
./kernel/core/endpoint_kern.c
./kernel/core/features.c
./kernel/core/handle.c
./kernel/core/kheap.c
./kernel/core/main.c
./kernel/core/memory.c
./kernel/core/module.c
./kernel/core/sched.c
./kernel/core/status.c
./kernel/core/syscall.c
./kernel/core/uframe.c
./kernel/core/usercopy.c
./kernel/core/vm.c
./mk/aarch64.mk
./mobile/jeios-mobile/README.md
./tests/test_alloc.c
./tests/test_boot.c
./tests/test_core.c
./tests/test_endpoint.c
./tests/test_handle.c
./tests/test_syscall.c
./tests/test_uframe.c
./tests/test_vm.c
./tests/vm_fake.c
./tools/__pycache__/emulator.cpython-312.pyc
./tools/check_elf.py
./tools/emulator.py
./tools/integration.py
./tools/smoke.py
./tools/smoke_arm.py
./docs/IMPLEMENTATION.md:516:`mk/aarch64.mk` (targets `make aarch64`, `make smoke-aarch64`) that leaves the
./docs/IMPLEMENTATION.md:539:Verified in this env: `make aarch64` + `make smoke-aarch64` PASS with the ordered
./docs/DEVELOPMENT.md:96:objdump -d build/0/jeios.elf
./docs/adr/debates/m3c-step6-waits.md:578:  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
./docs/adr/debates/m3c-step7-bootstrap-fixture.md:747:  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
./docs/adr/debates/m3c-step7-bootstrap-fixture.md:9383:/usr/bin/bash -lc "sed -n '1,240p' Makefile; find build -type f -name 'user_demo.o' -print -exec file {} \\; -exec objdump -t {} \\; 2>/dev/null | grep -A4 -B2 -E 'user_demo.o|je_user_ping|je_user_pong' | tail -n 120; objdump -h build/0/kernel/arch/x86_64/user_demo.o 2>/dev/null || true; objdump -h build/9/kernel/arch/x86_64/user_demo.o 2>/dev/null || true" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
./docs/adr/debates/m3c-step6-mechanism.md:854:  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
./docs/adr/debates/mp1-vectors.md:87:- SCTLR_EL1 state on entry isn't defined by the boot protocol beyond the MMU and D-cache being off. Recommend boot.S write SCTLR_EL1 to a known value (SA=1, A=0, M=0) so the fault behaviour the handler depends on is deterministic.
./docs/adr/debates/mp1-vectors.md:91:- Should the SCTLR_EL1 write go in this PR or in a separate one?
./docs/adr/debates/mp1-vectors.md:101:web search: site:developer.arm.com SCTLR_EL1 M 0 MMU disabled memory type Normal non-cacheable AArch64
./docs/adr/debates/mp1-vectors.md:141:- “MMU off means Device-nGnRnE” is wrong. With `SCTLR_EL1.M=0`, Normal-memory accesses are non-cacheable; UART’s Device mapping is a separate concern. The alignment discipline remains necessary, but that rationale should be corrected. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/b/aarch64-system-registers/sctlr_el1))
./docs/adr/debates/mp1-vectors.md:177:- “MMU off means Device-nGnRnE” is wrong. With `SCTLR_EL1.M=0`, Normal-memory accesses are non-cacheable; UART’s Device mapping is a separate concern. The alignment discipline remains necessary, but that rationale should be corrected. ([developer.arm.com](https://developer.arm.com/docs/ddi0601/b/aarch64-system-registers/sctlr_el1))
./docs/adr/debates/mp1-vectors.md:223:- **SCTLR_EL1 must be written explicitly in boot.** Many of its bits reset to an architecturally UNKNOWN value, including SA, the SP-alignment check. Boot should write a defined value (M=0, C=0, SA=1, A per policy). Otherwise whether a misaligned `stp` faults is not deterministic. If step 1 did not do this, it is a step-1 gap.
./docs/adr/debates/mp1-vectors.md:248:**Position:** adopt Codex's frame and BRK assertions with the 0x3df mask, use the stack-alignment overflow precheck instead of SP_EL0 scratch, reserve TPIDR_EL1 for the fatal path only until SMP, and add the explicit SCTLR_EL1 write.
./docs/HANDOFF.md:22:  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
./mk/aarch64.mk:4:# integration flow is untouched. Invoked via `make aarch64` and
./mk/aarch64.mk:5:# `make smoke-aarch64`. Toolchain is the pinned aarch64-linux-gnu cross set.
./mk/aarch64.mk:7:ARM_CC      := aarch64-linux-gnu-gcc
./mk/aarch64.mk:8:ARM_LD      := aarch64-linux-gnu-ld
./mk/aarch64.mk:9:ARM_OBJCOPY := aarch64-linux-gnu-objcopy
./mk/aarch64.mk:14:# since QEMU RAM starts zero). `make aarch64 ARM_BSS_CHECK=1`. It uses a SEPARATE
./mk/aarch64.mk:22:# `make aarch64 ARM_VECTOR_TEST=1`. Separate build dir so the toggle never reuses
./mk/aarch64.mk:41:# since QEMU RAM starts zero). `make aarch64 ARM_BSS_CHECK=1`.
./kernel/arch/aarch64/boot.S:85:     * MMU stays OFF (SCTLR_EL1.M untouched). bit31 is RES1 on cortex-a72, so we
./kernel/arch/aarch64/boot.S:92:    /* ---- SCTLR_EL1 explicit init (MP1 review gap): several SCTLR_EL1 bits reset
./kernel/arch/aarch64/boot.S:100:     * ARMv8.0 SCTLR_EL1 (bits 11, 20, 22, 23, 28, 29) so we don't write a
./kernel/arch/aarch64/boot.S:104:     *   total                                                     = 0x30D00808 */
./kernel/arch/aarch64/boot.S:107:    msr     SCTLR_EL1, x0

codex
The source confirms the two high-risk areas are real review points to resolve: the vector index transport is separate from full register preservation, and the slot macro’s first `.balign` needs byte-level confirmation rather than intuition. I’m now checking the linked ELF/disassembly and the compiler-generated double-fault path.
exec
/usr/bin/bash -lc "aarch64-linux-gnu-readelf -SWs build/aarch64-vectest/jeios-arm.elf | sed -n '1,220p'; aarch64-linux-gnu-objdump -drwC -j .text.vectors -j .text build/aarch64-vectest/jeios-arm.elf | sed -n '1,300p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "aarch64-linux-gnu-nm -n build/aarch64-vectest/jeios-arm.elf | grep -E 'je_arm_vectors|je_arm_vec_common|je_arm_brk_site|je_arm_exception|je_arm_double_fault|__stack_(bottom|top)|__bss_(start|end)|_text|_end'; aarch64-linux-gnu-objdump -t build/aarch64-vectest/jeios-arm.elf | grep -E 'je_arm_vectors|je_arm_brk_site|je_arm_vec_common|je_arm_double_fault'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "aarch64-linux-gnu-objdump -d -j .text build/aarch64-vectest/jeios-arm.elf | grep -A100 -B10 '<je_arm_double_fault>'; aarch64-linux-gnu-objdump -d -j .text build/aarch64-vectest/jeios-arm.elf | grep -A40 -B8 '<trigger_brk>'; aarch64-linux-gnu-objdump -s -j .text.vectors build/aarch64-vectest/jeios-arm.elf | sed -n '1,180p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
exec
/usr/bin/bash -lc "git diff --check; git diff -- kernel/arch/aarch64/vectors.S kernel/arch/aarch64/main.c kernel/arch/aarch64/boot.S kernel/arch/aarch64/linker.ld mk/aarch64.mk | sed -n '1,360p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
0000000040080000 T _text
0000000040080800 T je_arm_vectors
0000000040081030 T je_arm_exception
00000000400811b0 T je_arm_double_fault
0000000040081338 t je_arm_brk_site
0000000040081354 T je_arm_vec_common
0000000040081790 B __bss_start
0000000040081790 B _file_end
00000000400817a0 B __bss_end
00000000400817a0 B __stack_bottom
00000000400857a0 B __stack_top
00000000400857a0 B _end
0000000040081338 l       .text	0000000000000000 je_arm_brk_site
00000000400811b0 g     F .text	0000000000000030 je_arm_double_fault
0000000040081354 g       .text	0000000000000000 je_arm_vec_common
0000000040080800 g       .text.vectors	0000000000000000 je_arm_vectors

 succeeded in 0ms:
There are 20 section headers, starting at offset 0x13968:

Section Headers:
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            0000000000000000 000000 000000 00      0   0  0
  [ 1] .text.boot        PROGBITS        0000000040080000 010000 000184 00  AX  0   0  4
  [ 2] .text.vectors     PROGBITS        0000000040080800 010800 000788 00  AX  0   0 2048
  [ 3] .text             PROGBITS        0000000040080f90 010f90 000460 00  AX  0   0 16
  [ 4] .rodata           PROGBITS        00000000400813f0 0113f0 00038c 00   A  0   0  8
  [ 5] .data             PROGBITS        0000000040081780 011780 000009 00  WA  0   0  8
  [ 6] .bss              NOBITS          0000000040081790 011789 000010 00  WA  0   0  8
  [ 7] .stack            NOBITS          00000000400817a0 011789 004000 00  WA  0   0  1
  [ 8] .debug_info       PROGBITS        0000000000000000 011789 000c51 00      0   0  1
  [ 9] .debug_abbrev     PROGBITS        0000000000000000 0123da 000300 00      0   0  1
  [10] .debug_line       PROGBITS        0000000000000000 0126da 000454 00      0   0  1
  [11] .debug_str        PROGBITS        0000000000000000 012b2e 0002b0 01  MS  0   0  1
  [12] .debug_line_str   PROGBITS        0000000000000000 012dde 0000ae 01  MS  0   0  1
  [13] .debug_loclists   PROGBITS        0000000000000000 012e8c 0001e6 00      0   0  1
  [14] .debug_rnglists   PROGBITS        0000000000000000 013072 00007f 00      0   0  1
  [15] .debug_aranges    PROGBITS        0000000000000000 0130f1 000030 00      0   0  1
  [16] .comment          PROGBITS        0000000000000000 013121 00002d 01  MS  0   0  1
  [17] .symtab           SYMTAB          0000000000000000 013150 0005d0 18     18  44  8
  [18] .strtab           STRTAB          0000000000000000 013720 000174 00      0   0  1
  [19] .shstrtab         STRTAB          0000000000000000 013894 0000cd 00      0   0  1
Key to Flags:
  W (write), A (alloc), X (execute), M (merge), S (strings), I (info),
  L (link order), O (extra OS processing required), G (group), T (TLS),
  C (compressed), x (unknown), o (OS specific), E (exclude),
  D (mbind), p (processor specific)

Symbol table '.symtab' contains 62 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND 
     1: 0000000040080000     0 SECTION LOCAL  DEFAULT    1 .text.boot
     2: 0000000040080800     0 SECTION LOCAL  DEFAULT    2 .text.vectors
     3: 0000000040080f90     0 SECTION LOCAL  DEFAULT    3 .text
     4: 00000000400813f0     0 SECTION LOCAL  DEFAULT    4 .rodata
     5: 0000000040081780     0 SECTION LOCAL  DEFAULT    5 .data
     6: 0000000040081790     0 SECTION LOCAL  DEFAULT    6 .bss
     7: 00000000400817a0     0 SECTION LOCAL  DEFAULT    7 .stack
     8: 0000000000000000     0 SECTION LOCAL  DEFAULT    8 .debug_info
     9: 0000000000000000     0 SECTION LOCAL  DEFAULT    9 .debug_abbrev
    10: 0000000000000000     0 SECTION LOCAL  DEFAULT   10 .debug_line
    11: 0000000000000000     0 SECTION LOCAL  DEFAULT   11 .debug_str
    12: 0000000000000000     0 SECTION LOCAL  DEFAULT   12 .debug_line_str
    13: 0000000000000000     0 SECTION LOCAL  DEFAULT   13 .debug_loclists
    14: 0000000000000000     0 SECTION LOCAL  DEFAULT   14 .debug_rnglists
    15: 0000000000000000     0 SECTION LOCAL  DEFAULT   15 .debug_aranges
    16: 0000000000000000     0 SECTION LOCAL  DEFAULT   16 .comment
    17: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS boot.o
    18: 0000000040080000     0 NOTYPE  LOCAL  DEFAULT    1 $x
    19: 0000000040080040     0 NOTYPE  LOCAL  DEFAULT    1 real_start
    20: 0000000040080004     0 NOTYPE  LOCAL  DEFAULT    1 $d
    21: 0000000040080040     0 NOTYPE  LOCAL  DEFAULT    1 $x
    22: 00000000400800d8     0 NOTYPE  LOCAL  DEFAULT    1 el_fatal
    23: 0000000040080128     0 NOTYPE  LOCAL  DEFAULT    1 load_fatal
    24: 00000000400800d0     0 NOTYPE  LOCAL  DEFAULT    1 el_hang
    25: 00000000400800f8     0 NOTYPE  LOCAL  DEFAULT    1 el_fatal_msg
    26: 00000000400800f8     0 NOTYPE  LOCAL  DEFAULT    1 $d
    27: 0000000040080128     0 NOTYPE  LOCAL  DEFAULT    1 $x
    28: 0000000040080148     0 NOTYPE  LOCAL  DEFAULT    1 load_fatal_msg
    29: 0000000040080148     0 NOTYPE  LOCAL  DEFAULT    1 $d
    30: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS vectors.o
    31: 0000000040080800     0 NOTYPE  LOCAL  DEFAULT    2 $x
    32: 0000000040081354     0 NOTYPE  LOCAL  DEFAULT    3 $x
    33: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS main.c
    34: 0000000040080f90     0 NOTYPE  LOCAL  DEFAULT    3 $x
    35: 0000000040080f90    72 FUNC    LOCAL  DEFAULT    3 console_write
    36: 0000000040080fe0    80 FUNC    LOCAL  DEFAULT    3 console_hex
    37: 00000000400813f0     0 NOTYPE  LOCAL  DEFAULT    4 $d
    38: 0000000040081338     0 NOTYPE  LOCAL  DEFAULT    3 je_arm_brk_site
    39: 0000000040081738     0 NOTYPE  LOCAL  DEFAULT    4 $d
    40: 0000000040081738    17 OBJECT  LOCAL  DEFAULT    4 d.1
    41: 0000000040081750    44 OBJECT  LOCAL  DEFAULT    4 msg.0
    42: 0000000040081790     0 NOTYPE  LOCAL  DEFAULT    6 $d
    43: 0000000040081780     0 NOTYPE  LOCAL  DEFAULT    5 $d
    44: 0000000000004000     0 NOTYPE  GLOBAL DEFAULT  ABS STACK_SIZE
    45: 00000000400811b0    48 FUNC    GLOBAL DEFAULT    3 je_arm_double_fault
    46: 0000000040081790     8 OBJECT  GLOBAL DEFAULT    6 je_arm_bss_probe
    47: 0000000040081788     1 OBJECT  GLOBAL DEFAULT    5 je_arm_in_handler
    48: 00000000000057a0     0 NOTYPE  GLOBAL DEFAULT  ABS __image_size
    49: 00000000400817a0     0 NOTYPE  GLOBAL DEFAULT    6 __bss_end
    50: 00000000400857a0     0 NOTYPE  GLOBAL DEFAULT    7 __stack_top
    51: 0000000040080000     0 NOTYPE  GLOBAL DEFAULT    1 _start
    52: 0000000040081780     8 OBJECT  GLOBAL DEFAULT    5 je_arm_dtb_phys
    53: 00000000400811e0   372 FUNC    GLOBAL DEFAULT    3 je_arm_main
    54: 0000000040081354     0 NOTYPE  GLOBAL DEFAULT    3 je_arm_vec_common
    55: 0000000040080000     0 NOTYPE  GLOBAL DEFAULT    1 _text
    56: 0000000040081790     0 NOTYPE  GLOBAL DEFAULT    6 _file_end
    57: 0000000040081790     0 NOTYPE  GLOBAL DEFAULT    6 __bss_start
    58: 0000000040081030   380 FUNC    GLOBAL DEFAULT    3 je_arm_exception
    59: 00000000400817a0     0 NOTYPE  GLOBAL DEFAULT    7 __stack_bottom
    60: 00000000400857a0     0 NOTYPE  GLOBAL DEFAULT    7 _end
    61: 0000000040080800     0 NOTYPE  GLOBAL DEFAULT    2 je_arm_vectors

build/aarch64-vectest/jeios-arm.elf:     file format elf64-littleaarch64


Disassembly of section .text.vectors:

0000000040080800 <je_arm_vectors>:
    40080800:	52800010 	mov	w16, #0x0                   	// #0
    40080804:	140002d4 	b	40081354 <je_arm_vec_common>
    40080808:	d503201f 	nop
    4008080c:	d503201f 	nop
    40080810:	d503201f 	nop
    40080814:	d503201f 	nop
    40080818:	d503201f 	nop
    4008081c:	d503201f 	nop
    40080820:	d503201f 	nop
    40080824:	d503201f 	nop
    40080828:	d503201f 	nop
    4008082c:	d503201f 	nop
    40080830:	d503201f 	nop
    40080834:	d503201f 	nop
    40080838:	d503201f 	nop
    4008083c:	d503201f 	nop
    40080840:	d503201f 	nop
    40080844:	d503201f 	nop
    40080848:	d503201f 	nop
    4008084c:	d503201f 	nop
    40080850:	d503201f 	nop
    40080854:	d503201f 	nop
    40080858:	d503201f 	nop
    4008085c:	d503201f 	nop
    40080860:	d503201f 	nop
    40080864:	d503201f 	nop
    40080868:	d503201f 	nop
    4008086c:	d503201f 	nop
    40080870:	d503201f 	nop
    40080874:	d503201f 	nop
    40080878:	d503201f 	nop
    4008087c:	d503201f 	nop
    40080880:	52800030 	mov	w16, #0x1                   	// #1
    40080884:	140002b4 	b	40081354 <je_arm_vec_common>
    40080888:	d503201f 	nop
    4008088c:	d503201f 	nop
    40080890:	d503201f 	nop
    40080894:	d503201f 	nop
    40080898:	d503201f 	nop
    4008089c:	d503201f 	nop
    400808a0:	d503201f 	nop
    400808a4:	d503201f 	nop
    400808a8:	d503201f 	nop
    400808ac:	d503201f 	nop
    400808b0:	d503201f 	nop
    400808b4:	d503201f 	nop
    400808b8:	d503201f 	nop
    400808bc:	d503201f 	nop
    400808c0:	d503201f 	nop
    400808c4:	d503201f 	nop
    400808c8:	d503201f 	nop
    400808cc:	d503201f 	nop
    400808d0:	d503201f 	nop
    400808d4:	d503201f 	nop
    400808d8:	d503201f 	nop
    400808dc:	d503201f 	nop
    400808e0:	d503201f 	nop
    400808e4:	d503201f 	nop
    400808e8:	d503201f 	nop
    400808ec:	d503201f 	nop
    400808f0:	d503201f 	nop
    400808f4:	d503201f 	nop
    400808f8:	d503201f 	nop
    400808fc:	d503201f 	nop
    40080900:	52800050 	mov	w16, #0x2                   	// #2
    40080904:	14000294 	b	40081354 <je_arm_vec_common>
    40080908:	d503201f 	nop
    4008090c:	d503201f 	nop
    40080910:	d503201f 	nop
    40080914:	d503201f 	nop
    40080918:	d503201f 	nop
    4008091c:	d503201f 	nop
    40080920:	d503201f 	nop
    40080924:	d503201f 	nop
    40080928:	d503201f 	nop
    4008092c:	d503201f 	nop
    40080930:	d503201f 	nop
    40080934:	d503201f 	nop
    40080938:	d503201f 	nop
    4008093c:	d503201f 	nop
    40080940:	d503201f 	nop
    40080944:	d503201f 	nop
    40080948:	d503201f 	nop
    4008094c:	d503201f 	nop
    40080950:	d503201f 	nop
    40080954:	d503201f 	nop
    40080958:	d503201f 	nop
    4008095c:	d503201f 	nop
    40080960:	d503201f 	nop
    40080964:	d503201f 	nop
    40080968:	d503201f 	nop
    4008096c:	d503201f 	nop
    40080970:	d503201f 	nop
    40080974:	d503201f 	nop
    40080978:	d503201f 	nop
    4008097c:	d503201f 	nop
    40080980:	52800070 	mov	w16, #0x3                   	// #3
    40080984:	14000274 	b	40081354 <je_arm_vec_common>
    40080988:	d503201f 	nop
    4008098c:	d503201f 	nop
    40080990:	d503201f 	nop
    40080994:	d503201f 	nop
    40080998:	d503201f 	nop
    4008099c:	d503201f 	nop
    400809a0:	d503201f 	nop
    400809a4:	d503201f 	nop
    400809a8:	d503201f 	nop
    400809ac:	d503201f 	nop
    400809b0:	d503201f 	nop
    400809b4:	d503201f 	nop
    400809b8:	d503201f 	nop
    400809bc:	d503201f 	nop
    400809c0:	d503201f 	nop
    400809c4:	d503201f 	nop
    400809c8:	d503201f 	nop
    400809cc:	d503201f 	nop
    400809d0:	d503201f 	nop
    400809d4:	d503201f 	nop
    400809d8:	d503201f 	nop
    400809dc:	d503201f 	nop
    400809e0:	d503201f 	nop
    400809e4:	d503201f 	nop
    400809e8:	d503201f 	nop
    400809ec:	d503201f 	nop
    400809f0:	d503201f 	nop
    400809f4:	d503201f 	nop
    400809f8:	d503201f 	nop
    400809fc:	d503201f 	nop
    40080a00:	52800090 	mov	w16, #0x4                   	// #4
    40080a04:	14000254 	b	40081354 <je_arm_vec_common>
    40080a08:	d503201f 	nop
    40080a0c:	d503201f 	nop
    40080a10:	d503201f 	nop
    40080a14:	d503201f 	nop
    40080a18:	d503201f 	nop
    40080a1c:	d503201f 	nop
    40080a20:	d503201f 	nop
    40080a24:	d503201f 	nop
    40080a28:	d503201f 	nop
    40080a2c:	d503201f 	nop
    40080a30:	d503201f 	nop
    40080a34:	d503201f 	nop
    40080a38:	d503201f 	nop
    40080a3c:	d503201f 	nop
    40080a40:	d503201f 	nop
    40080a44:	d503201f 	nop
    40080a48:	d503201f 	nop
    40080a4c:	d503201f 	nop
    40080a50:	d503201f 	nop
    40080a54:	d503201f 	nop
    40080a58:	d503201f 	nop
    40080a5c:	d503201f 	nop
    40080a60:	d503201f 	nop
    40080a64:	d503201f 	nop
    40080a68:	d503201f 	nop
    40080a6c:	d503201f 	nop
    40080a70:	d503201f 	nop
    40080a74:	d503201f 	nop
    40080a78:	d503201f 	nop
    40080a7c:	d503201f 	nop
    40080a80:	528000b0 	mov	w16, #0x5                   	// #5
    40080a84:	14000234 	b	40081354 <je_arm_vec_common>
    40080a88:	d503201f 	nop
    40080a8c:	d503201f 	nop
    40080a90:	d503201f 	nop
    40080a94:	d503201f 	nop
    40080a98:	d503201f 	nop
    40080a9c:	d503201f 	nop
    40080aa0:	d503201f 	nop
    40080aa4:	d503201f 	nop
    40080aa8:	d503201f 	nop
    40080aac:	d503201f 	nop
    40080ab0:	d503201f 	nop
    40080ab4:	d503201f 	nop
    40080ab8:	d503201f 	nop
    40080abc:	d503201f 	nop
    40080ac0:	d503201f 	nop
    40080ac4:	d503201f 	nop
    40080ac8:	d503201f 	nop
    40080acc:	d503201f 	nop
    40080ad0:	d503201f 	nop
    40080ad4:	d503201f 	nop
    40080ad8:	d503201f 	nop
    40080adc:	d503201f 	nop
    40080ae0:	d503201f 	nop
    40080ae4:	d503201f 	nop
    40080ae8:	d503201f 	nop
    40080aec:	d503201f 	nop
    40080af0:	d503201f 	nop
    40080af4:	d503201f 	nop
    40080af8:	d503201f 	nop
    40080afc:	d503201f 	nop
    40080b00:	528000d0 	mov	w16, #0x6                   	// #6
    40080b04:	14000214 	b	40081354 <je_arm_vec_common>
    40080b08:	d503201f 	nop
    40080b0c:	d503201f 	nop
    40080b10:	d503201f 	nop
    40080b14:	d503201f 	nop
    40080b18:	d503201f 	nop
    40080b1c:	d503201f 	nop
    40080b20:	d503201f 	nop
    40080b24:	d503201f 	nop
    40080b28:	d503201f 	nop
    40080b2c:	d503201f 	nop
    40080b30:	d503201f 	nop
    40080b34:	d503201f 	nop
    40080b38:	d503201f 	nop
    40080b3c:	d503201f 	nop
    40080b40:	d503201f 	nop
    40080b44:	d503201f 	nop
    40080b48:	d503201f 	nop
    40080b4c:	d503201f 	nop
    40080b50:	d503201f 	nop
    40080b54:	d503201f 	nop
    40080b58:	d503201f 	nop
    40080b5c:	d503201f 	nop
    40080b60:	d503201f 	nop
    40080b64:	d503201f 	nop
    40080b68:	d503201f 	nop
    40080b6c:	d503201f 	nop
    40080b70:	d503201f 	nop
    40080b74:	d503201f 	nop
    40080b78:	d503201f 	nop
    40080b7c:	d503201f 	nop
    40080b80:	528000f0 	mov	w16, #0x7                   	// #7
    40080b84:	140001f4 	b	40081354 <je_arm_vec_common>
    40080b88:	d503201f 	nop
    40080b8c:	d503201f 	nop
    40080b90:	d503201f 	nop
    40080b94:	d503201f 	nop
    40080b98:	d503201f 	nop
    40080b9c:	d503201f 	nop
    40080ba0:	d503201f 	nop
    40080ba4:	d503201f 	nop
    40080ba8:	d503201f 	nop
    40080bac:	d503201f 	nop
    40080bb0:	d503201f 	nop
    40080bb4:	d503201f 	nop
    40080bb8:	d503201f 	nop
    40080bbc:	d503201f 	nop
    40080bc0:	d503201f 	nop
    40080bc4:	d503201f 	nop
    40080bc8:	d503201f 	nop
    40080bcc:	d503201f 	nop
    40080bd0:	d503201f 	nop
    40080bd4:	d503201f 	nop
    40080bd8:	d503201f 	nop
    40080bdc:	d503201f 	nop
    40080be0:	d503201f 	nop
    40080be4:	d503201f 	nop
    40080be8:	d503201f 	nop
    40080bec:	d503201f 	nop
    40080bf0:	d503201f 	nop
    40080bf4:	d503201f 	nop
    40080bf8:	d503201f 	nop
    40080bfc:	d503201f 	nop
    40080c00:	52800110 	mov	w16, #0x8                   	// #8
    40080c04:	140001d4 	b	40081354 <je_arm_vec_common>
    40080c08:	d503201f 	nop
    40080c0c:	d503201f 	nop
    40080c10:	d503201f 	nop
    40080c14:	d503201f 	nop
    40080c18:	d503201f 	nop
    40080c1c:	d503201f 	nop
    40080c20:	d503201f 	nop
    40080c24:	d503201f 	nop
    40080c28:	d503201f 	nop
    40080c2c:	d503201f 	nop
    40080c30:	d503201f 	nop
    40080c34:	d503201f 	nop
    40080c38:	d503201f 	nop
    40080c3c:	d503201f 	nop
    40080c40:	d503201f 	nop
    40080c44:	d503201f 	nop
    40080c48:	d503201f 	nop
    40080c4c:	d503201f 	nop
    40080c50:	d503201f 	nop
    40080c54:	d503201f 	nop
    40080c58:	d503201f 	nop
    40080c5c:	d503201f 	nop
    40080c60:	d503201f 	nop
    40080c64:	d503201f 	nop
    40080c68:	d503201f 	nop
    40080c6c:	d503201f 	nop
    40080c70:	d503201f 	nop
    40080c74:	d503201f 	nop
    40080c78:	d503201f 	nop
    40080c7c:	d503201f 	nop
    40080c80:	52800130 	mov	w16, #0x9                   	// #9
    40080c84:	140001b4 	b	40081354 <je_arm_vec_common>
    40080c88:	d503201f 	nop
    40080c8c:	d503201f 	nop
    40080c90:	d503201f 	nop

 succeeded in 0ms:
 succeeded in 0ms:
    4008118c:	9240118c 	and	x12, x12, #0x1f
    40081190:	f10f001f 	cmp	x0, #0x3c0
    40081194:	fa450980 	ccmp	x12, #0x5, #0x0, eq	// eq = none
    40081198:	54fffd61 	b.ne	40081144 <je_arm_exception+0x114>  // b.any
    4008119c:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400811a0:	91120000 	add	x0, x0, #0x480
    400811a4:	97ffff7b 	bl	40080f90 <console_write>
    400811a8:	17ffffea 	b	40081150 <je_arm_exception+0x120>
    400811ac:	d503201f 	nop

00000000400811b0 <je_arm_double_fault>:
    400811b0:	90000001 	adrp	x1, 40081000 <console_hex+0x20>
    400811b4:	911ce021 	add	x1, x1, #0x738
    400811b8:	91006021 	add	x1, x1, #0x18
    400811bc:	52800940 	mov	w0, #0x4a                  	// #74
    400811c0:	d2a12002 	mov	x2, #0x9000000             	// #150994944
    400811c4:	d503201f 	nop
    400811c8:	b9000040 	str	w0, [x2]
    400811cc:	38401c20 	ldrb	w0, [x1, #1]!
    400811d0:	35ffffc0 	cbnz	w0, 400811c8 <je_arm_double_fault+0x18>
    400811d4:	d503205f 	wfe
    400811d8:	d503205f 	wfe
    400811dc:	17fffffe 	b	400811d4 <je_arm_double_fault+0x24>

00000000400811e0 <je_arm_main>:
    400811e0:	a9bf7bfd 	stp	x29, x30, [sp, #-16]!
    400811e4:	910003fd 	mov	x29, sp
    400811e8:	d5384241 	mrs	x1, currentel
    400811ec:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400811f0:	91162000 	add	x0, x0, #0x588
    400811f4:	d3420c21 	ubfx	x1, x1, #2, #2
    400811f8:	1100c028 	add	w8, w1, #0x30
    400811fc:	97ffff65 	bl	40080f90 <console_write>
    40081200:	5290d400 	mov	w0, #0x86a0                	// #34464
    40081204:	72a00020 	movk	w0, #0x1, lsl #16
    40081208:	d2800302 	mov	x2, #0x18                  	// #24
    4008120c:	f2a12002 	movk	x2, #0x900, lsl #16
    40081210:	14000003 	b	4008121c <je_arm_main+0x3c>
    40081214:	71000400 	subs	w0, w0, #0x1
    40081218:	54000060 	b.eq	40081224 <je_arm_main+0x44>  // b.none
    4008121c:	b9400041 	ldr	w1, [x2]
    40081220:	372fffa1 	tbnz	w1, #5, 40081214 <je_arm_main+0x34>
    40081224:	d2a12001 	mov	x1, #0x9000000             	// #150994944
    40081228:	90000007 	adrp	x7, 40081000 <console_hex+0x20>
    4008122c:	9110c0e7 	add	x7, x7, #0x430
    40081230:	aa0703e0 	mov	x0, x7
    40081234:	b9000028 	str	w8, [x1]
    40081238:	97ffff56 	bl	40080f90 <console_write>
    4008123c:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    40081240:	91168000 	add	x0, x0, #0x5a0
    40081244:	97ffff53 	bl	40080f90 <console_write>
    40081248:	910003e0 	mov	x0, sp
    4008124c:	97ffff65 	bl	40080fe0 <console_hex>
    40081250:	aa0703e0 	mov	x0, x7
    40081254:	97ffff4f 	bl	40080f90 <console_write>
    40081258:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    4008125c:	91170000 	add	x0, x0, #0x5c0
    40081260:	97ffff4c 	bl	40080f90 <console_write>
    40081264:	d5382040 	mrs	x0, tcr_el1
    40081268:	97ffff5e 	bl	40080fe0 <console_hex>
    4008126c:	aa0703e0 	mov	x0, x7
    40081270:	97ffff48 	bl	40080f90 <console_write>
    40081274:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    40081278:	9117e000 	add	x0, x0, #0x5f8
    4008127c:	97ffff45 	bl	40080f90 <console_write>
    40081280:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    40081284:	911e4000 	add	x0, x0, #0x790
    40081288:	97ffff56 	bl	40080fe0 <console_hex>
    4008128c:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    40081290:	91186000 	add	x0, x0, #0x618
    40081294:	97ffff3f 	bl	40080f90 <console_write>
    40081298:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    4008129c:	911e8000 	add	x0, x0, #0x7a0
    400812a0:	97ffff50 	bl	40080fe0 <console_hex>
    400812a4:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400812a8:	91188000 	add	x0, x0, #0x620
    400812ac:	97ffff39 	bl	40080f90 <console_write>
    400812b0:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400812b4:	f943c800 	ldr	x0, [x0, #1936]
    400812b8:	b40000e0 	cbz	x0, 400812d4 <je_arm_main+0xf4>
    400812bc:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400812c0:	9118a000 	add	x0, x0, #0x628
    400812c4:	97ffff33 	bl	40080f90 <console_write>
    400812c8:	d503205f 	wfe
    400812cc:	d503205f 	wfe
    400812d0:	17fffffe 	b	400812c8 <je_arm_main+0xe8>
    400812d4:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400812d8:	91196000 	add	x0, x0, #0x658
    400812dc:	97ffff2d 	bl	40080f90 <console_write>
    400812e0:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400812e4:	9119e000 	add	x0, x0, #0x678
    400812e8:	97ffff2a 	bl	40080f90 <console_write>
    400812ec:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    400812f0:	f943c000 	ldr	x0, [x0, #1920]
    400812f4:	97ffff3b 	bl	40080fe0 <console_hex>
    400812f8:	aa0703e0 	mov	x0, x7
    400812fc:	97ffff25 	bl	40080f90 <console_write>
    40081300:	f0ffffe0 	adrp	x0, 40080000 <_start>
    40081304:	91200000 	add	x0, x0, #0x800
    40081308:	d518c000 	msr	vbar_el1, x0
    4008130c:	d5033fdf 	isb
    40081310:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    40081314:	911aa000 	add	x0, x0, #0x6a8
    40081318:	97ffff1e 	bl	40080f90 <console_write>
    4008131c:	d538c000 	mrs	x0, vbar_el1
    40081320:	97ffff30 	bl	40080fe0 <console_hex>
    40081324:	aa0703e0 	mov	x0, x7
    40081328:	97ffff1a 	bl	40080f90 <console_write>
    4008132c:	90000000 	adrp	x0, 40081000 <console_hex+0x20>
    40081330:	911b4000 	add	x0, x0, #0x6d0
    40081334:	97ffff17 	bl	40080f90 <console_write>
--
    40081340:	911c2000 	add	x0, x0, #0x708
    40081344:	97ffff13 	bl	40080f90 <console_write>
    40081348:	d503205f 	wfe
    4008134c:	d503205f 	wfe
    40081350:	17fffffe 	b	40081348 <je_arm_brk_site+0x10>

0000000040081354 <je_arm_vec_common>:
    40081354:	90000011 	adrp	x17, 40081000 <console_hex+0x20>
    40081358:	911e2231 	add	x17, x17, #0x788
    4008135c:	39400232 	ldrb	w18, [x17]
    40081360:	35fff292 	cbnz	w18, 400811b0 <je_arm_double_fault>
    40081364:	52800032 	mov	w18, #0x1                   	// #1
    40081368:	39000232 	strb	w18, [x17]
    4008136c:	d104c3ff 	sub	sp, sp, #0x130
    40081370:	a90007e0 	stp	x0, x1, [sp]
    40081374:	a9010fe2 	stp	x2, x3, [sp, #16]
    40081378:	a90217e4 	stp	x4, x5, [sp, #32]
    4008137c:	a9031fe6 	stp	x6, x7, [sp, #48]
    40081380:	a90427e8 	stp	x8, x9, [sp, #64]
    40081384:	a9052fea 	stp	x10, x11, [sp, #80]
    40081388:	a90637ec 	stp	x12, x13, [sp, #96]
    4008138c:	a9073fee 	stp	x14, x15, [sp, #112]
    40081390:	a90847f0 	stp	x16, x17, [sp, #128]
    40081394:	a9094ff2 	stp	x18, x19, [sp, #144]
    40081398:	a90a57f4 	stp	x20, x21, [sp, #160]
    4008139c:	a90b5ff6 	stp	x22, x23, [sp, #176]
    400813a0:	a90c67f8 	stp	x24, x25, [sp, #192]
    400813a4:	a90d6ffa 	stp	x26, x27, [sp, #208]
    400813a8:	a90e77fc 	stp	x28, x29, [sp, #224]
    400813ac:	f9007bfe 	str	x30, [sp, #240]
    400813b0:	9104c3e0 	add	x0, sp, #0x130
    400813b4:	f9007fe0 	str	x0, [sp, #248]
    400813b8:	d5384020 	mrs	x0, elr_el1
    400813bc:	d5384001 	mrs	x1, spsr_el1
    400813c0:	a91007e0 	stp	x0, x1, [sp, #256]
    400813c4:	d5385200 	mrs	x0, esr_el1
    400813c8:	d5386001 	mrs	x1, far_el1
    400813cc:	a91107e0 	stp	x0, x1, [sp, #272]
    400813d0:	f94043e0 	ldr	x0, [sp, #128]
    400813d4:	92407c00 	and	x0, x0, #0xffffffff
    400813d8:	f90093e0 	str	x0, [sp, #288]
    400813dc:	f90097ff 	str	xzr, [sp, #296]
    400813e0:	910003e0 	mov	x0, sp
    400813e4:	97ffff13 	bl	40081030 <je_arm_exception>
    400813e8:	d503205f 	wfe
    400813ec:	17ffffff 	b	400813e8 <je_arm_vec_common+0x94>

build/aarch64-vectest/jeios-arm.elf:     file format elf64-littleaarch64

Contents of section .text.vectors:
 40080800 10008052 d4020014 1f2003d5 1f2003d5  ...R..... ... ..
 40080810 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080820 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080830 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080840 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080850 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080860 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080870 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080880 30008052 b4020014 1f2003d5 1f2003d5  0..R..... ... ..
 40080890 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400808a0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400808b0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400808c0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400808d0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400808e0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400808f0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080900 50008052 94020014 1f2003d5 1f2003d5  P..R..... ... ..
 40080910 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080920 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080930 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080940 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080950 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080960 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080970 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080980 70008052 74020014 1f2003d5 1f2003d5  p..Rt.... ... ..
 40080990 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400809a0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400809b0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400809c0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400809d0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400809e0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 400809f0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a00 90008052 54020014 1f2003d5 1f2003d5  ...RT.... ... ..
 40080a10 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a20 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a30 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a40 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a50 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a60 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a70 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080a80 b0008052 34020014 1f2003d5 1f2003d5  ...R4.... ... ..
 40080a90 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080aa0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ab0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ac0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ad0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ae0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080af0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b00 d0008052 14020014 1f2003d5 1f2003d5  ...R..... ... ..
 40080b10 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b20 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b30 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b40 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b50 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b60 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b70 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080b80 f0008052 f4010014 1f2003d5 1f2003d5  ...R..... ... ..
 40080b90 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ba0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080bb0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080bc0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080bd0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080be0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080bf0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c00 10018052 d4010014 1f2003d5 1f2003d5  ...R..... ... ..
 40080c10 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c20 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c30 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c40 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c50 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c60 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c70 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080c80 30018052 b4010014 1f2003d5 1f2003d5  0..R..... ... ..
 40080c90 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ca0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080cb0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080cc0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080cd0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ce0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080cf0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d00 50018052 94010014 1f2003d5 1f2003d5  P..R..... ... ..
 40080d10 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d20 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d30 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d40 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d50 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d60 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d70 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080d80 70018052 74010014 1f2003d5 1f2003d5  p..Rt.... ... ..
 40080d90 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080da0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080db0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080dc0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080dd0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080de0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080df0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e00 90018052 54010014 1f2003d5 1f2003d5  ...RT.... ... ..
 40080e10 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e20 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e30 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e40 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e50 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e60 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e70 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080e80 b0018052 34010014 1f2003d5 1f2003d5  ...R4.... ... ..
 40080e90 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ea0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080eb0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ec0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ed0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ee0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080ef0 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f00 d0018052 14010014 1f2003d5 1f2003d5  ...R..... ... ..
 40080f10 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f20 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f30 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f40 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f50 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f60 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f70 1f2003d5 1f2003d5 1f2003d5 1f2003d5  . ... ... ... ..
 40080f80 f0018052 f4000014                    ...R....        

exec
/usr/bin/bash -lc "nl -ba tools/smoke_arm.py | sed -n '1,180p'; nl -ba docs/IMPLEMENTATION.md | sed -n '490,575p'; nl -ba docs/HANDOFF.md | sed -n '1,70p'" in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
     1	#!/usr/bin/env python3
     2	# SPDX-License-Identifier: Apache-2.0
     3	"""
     4	MP1 step 1 (ADR 0002): boot the ARM64 Image on the pinned QEMU virt-8.2 /
     5	cortex-a72 target and assert the ordered boot markers.
     6	
     7	Discipline (mirrors the x86 smoke harness + the ADR 0002 D6 evidence rule):
     8	  - capture serial to a FILE, never stdout (stdout races the timeout kill and
     9	    looks empty even when the UART produced output);
    10	  - require the exact ordered markers and the terminal line; a halted emulator
    11	    with no markers is a FAILURE, not success;
    12	  - reject any "FATAL" line;
    13	  - -no-reboot so a triple-fault/reset can't masquerade as a hang;
    14	  - record the image SHA-256, tool versions, and the exact QEMU command;
    15	  - SKIP (exit 0, clearly labelled) if the aarch64 toolchain / qemu-system-aarch64
    16	    is unavailable — never report PASS when nothing ran.
    17	"""
    18	import argparse
    19	import hashlib
    20	import os
    21	import shutil
    22	import subprocess
    23	import sys
    24	import tempfile
    25	
    26	QEMU = "qemu-system-aarch64"
    27	MACHINE = "virt-8.2"
    28	CPU = "cortex-a72"
    29	
    30	# Ordered step-1 markers. "bss cleared" (default) vs "bss clear verified"
    31	# (patterned-BSS build) — accept either, but if the verified form is present it
    32	# must be the verified one; see --bss-verified.
    33	BASE_MARKERS = [
    34	    b"JeiOS/aarch64: entry EL1",
    35	    b"JeiOS/aarch64: stack ready",
    36	    b"JeiOS/aarch64: tcr_el1 initialized (tbi0=0)",
    37	    None,  # bss line, filled below
    38	    b"JeiOS/aarch64: dtb pointer recorded",
    39	    b"JeiOS/aarch64: vectors installed",
    40	]
    41	# Default build terminal marker (no vector test).
    42	TERM_DEFAULT = b"JeiOS/aarch64: initialization complete; halted"
    43	# Vector-test build: the planted BRK must vector to the EL1h sync slot and the
    44	# handler must assert the exact syndrome. These ordered markers replace the
    45	# default terminal and prove the deliberate exception end-to-end.
    46	VECTOR_TEST_MARKERS = [
    47	    b"JeiOS/aarch64: triggering deliberate BRK",
    48	    b"JeiOS/aarch64: EXCEPTION vector=0x0000000000000004 ec=0x000000000000003c",
    49	    b"JeiOS/aarch64: vector self-test PASS",
    50	    b"JeiOS/aarch64: exception handled; halted",
    51	]
    52	
    53	
    54	def skip(msg):
    55	    print(f"QEMU aarch64 smoke: SKIP ({msg})")
    56	    sys.exit(0)
    57	
    58	
    59	def main():
    60	    ap = argparse.ArgumentParser(description=__doc__)
    61	    ap.add_argument("image")
    62	    ap.add_argument("--memory", default="256M")
    63	    ap.add_argument("--timeout", type=int, default=15)
    64	    ap.add_argument("--bss-verified", action="store_true",
    65	                    help="expect the patterned-BSS 'bss clear verified' marker")
    66	    ap.add_argument("--vector-test", action="store_true",
    67	                    help="expect the deliberate-BRK vector self-test markers")
    68	    args = ap.parse_args()
    69	
    70	    if shutil.which(QEMU) is None:
    71	        skip(f"{QEMU} not installed")
    72	    if not os.path.exists(args.image):
    73	        skip(f"image {args.image} not built")
    74	
    75	    markers = list(BASE_MARKERS)
    76	    markers[3] = (b"JeiOS/aarch64: bss clear verified" if args.bss_verified
    77	                  else b"JeiOS/aarch64: bss cleared")
    78	    if args.vector_test:
    79	        markers += VECTOR_TEST_MARKERS
    80	    else:
    81	        markers.append(TERM_DEFAULT)
    82	
    83	    # Evidence: image hash + tool versions.
    84	    with open(args.image, "rb") as f:
    85	        sha = hashlib.sha256(f.read()).hexdigest()
    86	    try:
    87	        qver = subprocess.check_output([QEMU, "--version"], text=True).splitlines()[0]
    88	    except Exception:
    89	        qver = "(unknown)"
    90	    print(f"image: {args.image}  sha256={sha}")
    91	    print(f"qemu:  {qver}")
    92	
    93	    with tempfile.NamedTemporaryFile(prefix="jeios-arm-serial-", suffix=".log",
    94	                                     delete=False) as tf:
    95	        serial_path = tf.name
    96	
    97	    cmd = [QEMU, "-M", MACHINE, "-cpu", CPU, "-smp", "1", "-m", args.memory,
    98	           "-display", "none", "-serial", f"file:{serial_path}",
    99	           "-no-reboot", "-kernel", args.image]
   100	    print("cmd:   " + " ".join(cmd))
   101	
   102	    try:
   103	        subprocess.run(cmd, timeout=args.timeout, stdout=subprocess.DEVNULL,
   104	                       stderr=subprocess.DEVNULL)
   105	    except subprocess.TimeoutExpired:
   106	        pass  # expected: the kernel halts in WFE; the timeout stops QEMU.
   107	
   108	    with open(serial_path, "rb") as f:
   109	        out = f.read()
   110	    os.unlink(serial_path)
   111	
   112	    sys.stdout.write(out.decode("ascii", "replace"))
   113	    sys.stdout.write("\n")
   114	
   115	    if b"FATAL" in out:
   116	        print("QEMU aarch64 smoke: FAIL (FATAL marker present)")
   117	        sys.exit(1)
   118	
   119	    pos = 0
   120	    for m in markers:
   121	        idx = out.find(m, pos)
   122	        if idx < 0:
   123	            print(f"QEMU aarch64 smoke: FAIL (missing/out-of-order marker: "
   124	                  f"{m.decode()})")
   125	            sys.exit(1)
   126	        pos = idx + len(m)
   127	
   128	    print("QEMU aarch64 smoke: PASS")
   129	
   130	
   131	if __name__ == "__main__":
   132	    main()
   490	unchanged.
   491	
   492	Scope honesty: the `M3b-s1` run exercises ONE process's table; it does not by
   493	itself prove tables are *per-process* (that holds by construction + the hosted
   494	vm/table tests), and the marker/comments say so rather than overclaiming.
   495	
   496	REVISIT (owner-approved for this PR, recorded here): (1) `je_vm_process` grew by
   497	an inline handle table — watch the struct-size budget as object kinds and the
   498	handle count grow. (2) `handle_query` returns **type only**; rights are carried
   499	internally but **NOT enforced and NOT exposed** in M3b — exposing a frozen rights
   500	layout waits until a real rights-enforcing operation exists. (3) `NOSLOTS` is
   501	reachable from ring 3 only via generation saturation of every slot (a bounded
   502	self-DoS of the caller's own table over ~1M syscalls), not truly "unreachable";
   503	it is covered exhaustively by the reduced-width hosted test.
   504	
   505	Peer cross-review (two independent reads) found no identity / slot-reuse / ABI-
   506	mapping / decode defect, and drove the evidence hardening applied here (the
   507	re-stale-after-reuse assertion, the measured quota count, the softened per-process
   508	wording, the ADR wide-handle amendment, and the pinned ABI static-asserts).
   509	AI-assisted review only; not independent human qualification.
   510	
   511	## MP1 step 1 — ARM64 entry + bounded console (QEMU virt-8.2) — 2026-10-17
   512	
   513	First *executable* ARM64 work (ADR 0002, jeios-mobile). The shared kernel core
   514	stays in this repo per ADR 0001; this adds a second architecture under
   515	`kernel/arch/aarch64/` (`boot.S`, `main.c`, `linker.ld`), built by a separate
   516	`mk/aarch64.mk` (targets `make aarch64`, `make smoke-aarch64`) that leaves the
   517	x86-64 build/check/smoke/integration flow completely untouched. Scope is entry
   518	only: **no VBAR/vectors, no DTB parse, no MMU** (those are later MP PRs).
   519	
   520	On the QEMU-validated `virt-8.2` / `cortex-a72` target (direct `-kernel`, no
   521	firmware), `boot.S`: parks x0(DTB) in x19, masks DAIF, asserts **EL1** (FATAL
   522	marker + halt otherwise), **self-checks the load address == link address**
   523	(text_offset=0x80000 → 0x40080000; FATAL if QEMU placed us elsewhere, so a future
   524	absolute reference can't silently be wrong), selects SP_EL1, writes **TCR_EL1 =
   525	0x80000000** (RES1 bit31 set for cortex-a72, TBI0=0 interim per ADR 0002 D5 — a
   526	field init, NOT a tag-rejection claim), clears BSS with aligned stores (no DC
   527	ZVA/atomics — pre-MMU Device-nGnRnE memory), stashes the DTB pointer into `.data`,
   528	and enters C. `main.c` drives a bounded PL011 (DR-write + bounded TXFF poll, no
   529	init — a documented virt-8.2 support-tier assumption) and emits ordered markers.
   530	
   531	Built `-mstrict-align -mgeneral-regs-only -ffreestanding -fno-builtin` so the
   532	compiler emits neither unaligned nor FP/SIMD code (a stray SIMD op would trap
   533	into absent vectors and hang). Image header: text_offset 0x80000, flags **0x1**
   534	(4 KiB pages, LE), magic, `__image_size` from the linker. Linker asserts keep the
   535	stack+BSS 16-byte aligned, after the file-backed image, within `__image_size`,
   536	non-overlapping; `--orphan-handling=error` forbids any unplaced section landing
   537	in the image.
   538	
   539	Verified in this env: `make aarch64` + `make smoke-aarch64` PASS with the ordered
   540	markers — `entry EL1` / `stack ready sp=0x400844e0` (in the linked range, proving
   541	load==link) / `tcr_el1 initialized (tbi0=0) tcr=0x80000000` / `bss range [...]` +
   542	`bss cleared` / `dtb pointer recorded dtb=0x48000000` / `initialization complete;
   543	halted`. The `ARM_BSS_CHECK=1` build (separate build dir) pattern-fills BSS before
   544	the production clear and prints **`bss clear verified`**, so the clear is proven
   545	to zero rather than relying on QEMU's already-zero RAM. The smoke harness
   546	(`tools/smoke_arm.py`) captures serial to a FILE (stdout races the timeout kill),
   547	asserts exact ordered markers, rejects `FATAL`, records image SHA-256 + tool
   548	versions + the exact command, uses `-no-reboot`, and is NOT wired into
   549	`make check`. The x86-64 side is unchanged — `make check` + smoke + 11 integration
   550	modes still pass.
   551	
   552	Not yet enforced (handoff): the ADR 0002 D5 trap-frame fields (ESR/FAR/ELR/SPSR),
   553	VBAR exception vectors + the deliberate-fault test, the TBI behavioral (tag
   554	rejection) test, DTB *validation* (the pointer is recorded, never dereferenced),
   555	and the MMU. Peer cross-review (two independent reads) found and drove real fixes
   556	the smoke had passed over: the Image flags (0x2→0x1), TCR_EL1 RES1 (0→0x80000000),
   557	the load≠link latent hazard (now a boot-time self-check), the vacuous empty-BSS
   558	marker (now a real probe + range), `_file_end` placement, `--orphan-handling`,
   559	and the DAIF-by-construction mask. AI-assisted review only; not independent human
   560	qualification.
   561	
   562	## M3b third PR — rights ABI exposure + enforcement policy hook — 2026-10-18
   563	
   564	Freezes the handle **rights ABI** (ADR 0008's deferred revisit) and wires rights
   565	enforcement on a live syscall path, WITHOUT attenuation/references (next PR) and
   566	without touching M3a's fault-death path. Rights are a fixed `uint32_t` mask;
   567	`JE_ABI_RIGHT_INSPECT = 0x1`, `JE_ABI_RIGHTS_KNOWN = INSPECT`, defined in `abi.h`
   568	and pinned with `_Static_assert` (the glue maps them; `abi.h` stays independent of
   569	`status.h` and `handle.h`). SELF is now minted with INSPECT (a kernel constant,
   570	compile-asserted to contain only known bits). New op **`handle_rights` (op 8)**:
   571	`lookup` requiring INSPECT, returns the slot's rights mask in RDX **masked to
   572	`JE_ABI_RIGHTS_KNOWN`** so no internal bit can leak. `handle_query` (op 7) stays
   573	type-only; `handle_close` requires no right.
   574	
   575	A pure accessor `je_handle_rights_of()` was added to the handle API (same validity
     1	# Agent handoff: M1/M2 kernel foundation
     2	
     3	## Current status — observed 2026-10-18 (read this first)
     4	
     5	Implemented and merged beyond M3a: **M3b** (typed handle table, ADR 0008) is in
     6	progress on x86-64, and a second architecture, **ARM64 (MP1)**, now boots.
     7	
     8	- **M3b** (issue #7, ADR 0008 accepted, Option A): the pure handle table, the
     9	  handle syscalls (`handle_self`/`close`/`query`/`rights`), the rights ABI, AND
    10	  **attenuation** (`handle_dup`, op 9) are all merged. Attenuation made rights
    11	  enforcement genuinely ring-3-observable: a process derives a rights-reduced
    12	  handle and `handle_rights` on it returns DENIED (QEMU `M3b-s1` proof). SELF is
    13	  the only object kind; dup is SELF-only (structurally type-gated — a destroyable
    14	  type would be a latent UAF without a retain, which M3c must add). **#7 stays
    15	  OPEN** (landed "Refs #7"): its remaining acceptance — bounded per-object
    16	  **reference count + overflow**, destruction-on-last-reference, and the
    17	  **service-instance vs slot generation** split — needs a destroyable object and
    18	  is M3c work.
    19	- **ARM64 MP1 step 1** (jeios-mobile#1, ADR 0002): boots on QEMU `virt-8.2` /
    20	  `cortex-a72` (direct `-kernel`, no firmware) — entry EL1, stack, TCR_EL1 interim,
    21	  BSS clear, DTB pointer recorded, bounded PL011 console, ordered markers. Separate
    22	  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
    23	  the x86-64 flow is untouched. **Not yet enforced (deferred to MP2+):** VBAR
    24	  vectors + the D5 trap frame (ESR/FAR/ELR/SPSR) + the deliberate-fault test, the
    25	  TBI behavioral test, DTB *validation*, and the MMU.
    26	- **M3c** (issue #8, **ADR 0009 accepted**): in progress. The contract is frozen
    27	  (endpoint/channel object, split refcount + destruction, bounded data-only
    28	  messages, waits, transactional attenuated transfer, bootstrap fixture; two
    29	  owner-gated decisions — terminal-destruction/no-incarnation-counter, and the
    30	  process-death release walk as its own owner-signed PR). **D-SEQ step 2
    31	  (lifecycle)** is implemented: `kernel/core/endpoint.c` + `endpoint.h` — the
    32	  endpoint pool, channel record, **split refcount** (`handle_refs` vs
    33	  `inflight_refs`), retain/release, destruction-on-last-ref, `REFOVERFLOW`, the
    34	  endpoint-aware alloc/dup/close seam (table stays pure), and retain-before-commit
    35	  endpoint dup (while `dup_self` stays SELF-only). Step 2 kept create kernel-only;
    36	  **step 3 (owner-signed) is now done**: the process-death **release walk** (via a
    37	  generic `je_vm` teardown hook run inside an **IRQ critical section** in
    38	  `je_vm_destroy` — A3-1), the **typed close dispatcher** (ring-3 HANDLE_CLOSE
    39	  routes endpoint handles through the endpoint layer — A3-3), and **ring-3
    40	  `endpoint_create` (op 10)** returning both handles **packed in RDX** (A3-2)
    41	  shipped together. Proven in QEMU: `M3c-s3a` (kernel self-test: survivor-peer
    42	  PEER_CLOSED, record freed on survivor close) and `M3c-s3b` (ring-3 create →
    43	  typed close → fault → reap reclaims the channel; pool baseline held across
    44	  >POOL_CAP cycles). ADR 0009 was amended (A3-1/A3-2/A3-3, owner-approved) for the
    45	  IRQ-save death-path change and the packed-RDX ABI shape. Earlier, ADR 0009 D4a
    46	  appended endpoint rights (`SEND=0x2`…), an owner-approved ABI change that flipped
    47	  the tested `dup(self,0x2)` result INVALID→DENIED (Option A; see IMPLEMENTATION.md
    48	  "M3c rights-bit discrepancy"). **Step 4 (non-blocking data IPC) is now done**:
    49	  `try_send` (op 11) / `try_recv` (op 12) move bounded, copied, **data-only**
    50	  messages through per-end inbound ring queues (Model A: send on end i → peer
    51	  inbound), honoring the D6 copy ordering (send: preflight → copy_from_user into
    52	  stack staging → enqueue; recv: peek → copy_to_user → infallible commit, so a
    53	  copy-out fault leaves the message queued). Statuses QUEUE_FULL/SHORT_BUFFER/
    54	  PEER_CLOSED/EMPTY (10–13) added; `JE_ABI_MSG_MAX=64`, DEPTH=4 (policy bounds,
    55	  ~2.1 KB pool). D9 data-only is enforced (R10 cap_count must be 0). An
    56	  owner-approved touch to the tested lifecycle path discards/zeroes inbound
    57	  queues on close/free (free condition unchanged). Proven ring-3 by the `M3c-s4`
    58	  self-loopback marker (direction, FIFO, SHORT_BUFFER, copy-out-fault rollback →
    59	  delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED) — which
    60	  does NOT prove cross-address-space delivery (one process holds both ends; that
    61	  needs transfer). **ABI forward note:** the single RDX scalar can't return
    62	  received capability handles, so **step 5 MUST add a new op / out-register
    63	  extension, not overload `try_recv`** (R8/R9 and RDX[63:32] reserved now).
    64	  **Step 5 (capability transfer) is now done** (frozen by ADR 0009 A5): `try_send`
    65	  carries 0..2 caps (descriptors queued + inflight-retained on the target, D3.2);
    66	  `try_recv` mints fresh attenuated handles in the receiver's table (D8). D9 =
    67	  cycles allowed but reclaimed on last-handle removal (A5-1); stale cap → handle 0
    68	  (A5-2); `maybe_free_record` got the `allocated` guard + detach-before-release
    69	  discard order (A5-3); ABI uses the pre-reserved R8/R9 path (A5-4). Proven: hosted
    70	  cap mint/dead-cap/cycle-reclaim + the `M3c-s5` kernel self-test (cross-TABLE

exec
/usr/bin/bash -lc 'git rev-parse --show-toplevel; git branch --show-current; git rev-parse HEAD; git diff --cached --stat; git status --short' in /home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
 succeeded in 0ms:
/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios
feat/mp1-arm64-vectors
f46f4f9094152e3fc1f3308a222003275c992b45
 docs/adr/debates/mp1-vectors.md | 291 ++++++++++++++++++++++++++++++++++++++++
 kernel/arch/aarch64/boot.S      |  18 +++
 kernel/arch/aarch64/linker.ld   |   2 +
 kernel/arch/aarch64/main.c      | 133 ++++++++++++++++++
 kernel/arch/aarch64/trapframe.h |  62 +++++++++
 kernel/arch/aarch64/vectors.S   | 134 ++++++++++++++++++
 mk/aarch64.mk                   |  23 +++-
 tools/smoke_arm.py              |  19 ++-
 8 files changed, 677 insertions(+), 5 deletions(-)
A  docs/adr/debates/mp1-vectors.md
M  kernel/arch/aarch64/boot.S
M  kernel/arch/aarch64/linker.ld
M  kernel/arch/aarch64/main.c
A  kernel/arch/aarch64/trapframe.h
A  kernel/arch/aarch64/vectors.S
M  mk/aarch64.mk
M  tools/smoke_arm.py

codex
I found three real defects/gaps; the BRK/QEMU result does not cover them.

1. **P1 — Trap-frame “complete GPR state” is false.** `vectors.S:77-82` clobbers `x17/x18`, and every stub intentionally clobbers `x16`. Thus `gpr[16] = vector index`, `gpr[17] = &je_arm_in_handler`, and `gpr[18] = 1`, not the interrupted values (`vectors.S:96-97`, `117-124`). Vector recovery itself is correct: nothing clobbers `x16` between `mov w16,#idx` and its save, and `mov w16` zero-extends the index. Either preserve x16–x18 or amend the frozen ABI to mark them invalid/scratch; the current `trapframe.h:10-18,37-46` claim is inconsistent.

2. **P2 — SP alignment/range is assumed, not enforced.** The current path is aligned: boot initializes an aligned `__stack_top`, the frame size is 304 bytes, and the generated `je_arm_double_fault` has no stack accesses. But `SA=1` does not guarantee SP is aligned at exception entry; it only faults on applicable SP-based accesses. A corrupted/misaligned/out-of-range SP can reach `vectors.S:86-103`, causing frame stores to corrupt memory or fault. Add a bounded SP/range check or explicitly narrow the MP1 guarantee.

3. **P2 — FAR validity test is incomplete.** `main.c:126-130` omits instruction/data aborts from the lower EL (`EC 0x20/0x24`) and watchpoints (`0x34/0x35`), all of which architecturally define FAR. It also does not check `FnV` for aborts, so an `EC 0x21/0x25` exception with `FnV=1` can print an architecturally UNKNOWN FAR as valid. Current-EL ordinary data aborts (`0x25`, `FnV=0`) are handled correctly. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/far_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/b/aarch64-system-registers/far_el1))

4. **P2 — The smoke test is weaker than the documented evidence claim.** `tools/smoke_arm.py:46-50` only requires the vector/EC prefix and `PASS`; it does not independently assert raw ESR, ELR, SPSR, or FAR (`tools/smoke_arm.py:119-128`). The in-kernel comparator currently checks them, but a bug in that comparator could produce a false PASS. Add exact serial assertions for the raw fields.

5. **P2/latent — Non-group-1 vectors do not have a correct SP ABI.** The common saver assumes the interrupted SP is `sp + 304` (`vectors.S:105-107`). That is valid for Current EL using SP_ELx, but not for lower-EL vectors, where the interrupted SP is SP_EL0; nor is it generally valid for Current EL using SP_EL0. Those vectors are unreachable in MP1, but the implementation currently presents all 16 as defined handlers. Arm documents the distinct SP selection for these vector groups. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table), [developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling))

No defect found in the specific questions below:

- **Guard race:** correct for single CPU with DAIF masked. The expected `adrp/add/ldrb` path cannot fault with MMU off and the image/data present; after `strb`, nested faults see `1`. It is not SMP-safe, and the never-clear behavior is appropriate only because handlers are non-returning (`vectors.S:73-82`).
- **SCTLR value:** `0x30D00808` decodes as SA plus RES1 bits 11, 20, 22, 23, 28, 29; M/A/C/I are clear. No accidental enable is present. ([developer.arm.com](https://developer.arm.com/docs/ddi0488/d/system-control/aarch64-register-descriptions/system-control-register-el1), [developer.arm.com](https://developer.arm.com/docs/ddi0488/d/system-control/aarch64-register-descriptions/system-control-register-el1))
- **Vector offsets:** verified from `build/aarch64-vectest/jeios-arm.elf`: base `0x40080800`; slot N is exactly base + `N*0x80`. The first `.balign` adds no padding because the table is already aligned. Arm specifies 16 entries at 128-byte offsets. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table))
- **BRK label:** `.globl` is not required here; the local symbol resolves within the same object. It is compiler-emission-fragile, so a dedicated assembly label would be more robust than merely adding `.globl` (`main.c:197-205`).
- **Conversions/cache:** no evident `-Wconversion` issue; no cache maintenance is needed because the code is static and boot explicitly disables I/D caches before installation (`boot.S:92-108`, `main.c:104-112`).
- **Documentation:** stale status is itself a defect: `main.c:8-13`, `docs/HANDOFF.md:23-25`, and `docs/IMPLEMENTATION.md:518,552-555` still say vectors/trap frames are unimplemented.
I found three real defects/gaps; the BRK/QEMU result does not cover them.

1. **P1 — Trap-frame “complete GPR state” is false.** `vectors.S:77-82` clobbers `x17/x18`, and every stub intentionally clobbers `x16`. Thus `gpr[16] = vector index`, `gpr[17] = &je_arm_in_handler`, and `gpr[18] = 1`, not the interrupted values (`vectors.S:96-97`, `117-124`). Vector recovery itself is correct: nothing clobbers `x16` between `mov w16,#idx` and its save, and `mov w16` zero-extends the index. Either preserve x16–x18 or amend the frozen ABI to mark them invalid/scratch; the current `trapframe.h:10-18,37-46` claim is inconsistent.

2. **P2 — SP alignment/range is assumed, not enforced.** The current path is aligned: boot initializes an aligned `__stack_top`, the frame size is 304 bytes, and the generated `je_arm_double_fault` has no stack accesses. But `SA=1` does not guarantee SP is aligned at exception entry; it only faults on applicable SP-based accesses. A corrupted/misaligned/out-of-range SP can reach `vectors.S:86-103`, causing frame stores to corrupt memory or fault. Add a bounded SP/range check or explicitly narrow the MP1 guarantee.

3. **P2 — FAR validity test is incomplete.** `main.c:126-130` omits instruction/data aborts from the lower EL (`EC 0x20/0x24`) and watchpoints (`0x34/0x35`), all of which architecturally define FAR. It also does not check `FnV` for aborts, so an `EC 0x21/0x25` exception with `FnV=1` can print an architecturally UNKNOWN FAR as valid. Current-EL ordinary data aborts (`0x25`, `FnV=0`) are handled correctly. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/far_el1), [developer.arm.com](https://developer.arm.com/docs/ddi0595/b/aarch64-system-registers/far_el1))

4. **P2 — The smoke test is weaker than the documented evidence claim.** `tools/smoke_arm.py:46-50` only requires the vector/EC prefix and `PASS`; it does not independently assert raw ESR, ELR, SPSR, or FAR (`tools/smoke_arm.py:119-128`). The in-kernel comparator currently checks them, but a bug in that comparator could produce a false PASS. Add exact serial assertions for the raw fields.

5. **P2/latent — Non-group-1 vectors do not have a correct SP ABI.** The common saver assumes the interrupted SP is `sp + 304` (`vectors.S:105-107`). That is valid for Current EL using SP_ELx, but not for lower-EL vectors, where the interrupted SP is SP_EL0; nor is it generally valid for Current EL using SP_EL0. Those vectors are unreachable in MP1, but the implementation currently presents all 16 as defined handlers. Arm documents the distinct SP selection for these vector groups. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table), [developer.arm.com](https://developer.arm.com/docs/100933/latest/processor-state-in-exception-handling))

No defect found in the specific questions below:

- **Guard race:** correct for single CPU with DAIF masked. The expected `adrp/add/ldrb` path cannot fault with MMU off and the image/data present; after `strb`, nested faults see `1`. It is not SMP-safe, and the never-clear behavior is appropriate only because handlers are non-returning (`vectors.S:73-82`).
- **SCTLR value:** `0x30D00808` decodes as SA plus RES1 bits 11, 20, 22, 23, 28, 29; M/A/C/I are clear. No accidental enable is present. ([developer.arm.com](https://developer.arm.com/docs/ddi0488/d/system-control/aarch64-register-descriptions/system-control-register-el1), [developer.arm.com](https://developer.arm.com/docs/ddi0488/d/system-control/aarch64-register-descriptions/system-control-register-el1))
- **Vector offsets:** verified from `build/aarch64-vectest/jeios-arm.elf`: base `0x40080800`; slot N is exactly base + `N*0x80`. The first `.balign` adds no padding because the table is already aligned. Arm specifies 16 entries at 128-byte offsets. ([developer.arm.com](https://developer.arm.com/docs/100933/latest/aarch64-exception-vector-table))
- **BRK label:** `.globl` is not required here; the local symbol resolves within the same object. It is compiler-emission-fragile, so a dedicated assembly label would be more robust than merely adding `.globl` (`main.c:197-205`).
- **Conversions/cache:** no evident `-Wconversion` issue; no cache maintenance is needed because the code is static and boot explicitly disables I/D caches before installation (`boot.S:92-108`, `main.c:104-112`).
- **Documentation:** stale status is itself a defect: `main.c:8-13`, `docs/HANDOFF.md:23-25`, and `docs/IMPLEMENTATION.md:518,552-555` still say vectors/trap frames are unimplemented.
