/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP1 (ADR 0002 D5-3): the AArch64 EL1 exception trap-frame ABI — owner-approved
 * 2026-10-19. The vector saver (boot/vectors asm) writes this layout; the C
 * handler reads it. The field ORDER and OFFSETS are a FROZEN contract: the MP3
 * exact-site fixup table and any crash-dump tooling will bind to these offsets,
 * and D5-3 requires the fixup be keyed on the exact faulting PC (ELR_EL1), so
 * `elr` must be a stable, documented field.
 *
 * D5-3 mandates ESR_EL1, FAR_EL1, ELR_EL1, SPSR_EL1 are saved. We also save the
 * full GPR set x0-x30 and the faulting SP for a complete saved state (ARM64.md
 * MP1 "complete saved-state layout"). GPR area is gpr[31] = x0..x30 exactly
 * (AArch64 has no architectural x31 in GPR context; SP is a separate field),
 * per the owner's DECISION 2.
 *
 * KNOWN DEVIATION (owner-acknowledged, MP1): gpr[16], gpr[17], gpr[18] do NOT
 * hold the interrupted x16/x17/x18. The vector stub uses x16 to carry the vector
 * index and the common saver uses x17/x18 for the reentrancy guard BEFORE the GPR
 * save, so the saved frame records: gpr[16] = vector index (low 32), gpr[17] =
 * &je_arm_in_handler, gpr[18] = 1. This is harmless for the MP1 diagnostic
 * (fatal, non-returning — the frame is read for ESR/FAR/ELR/vector only, never
 * restored). A future RETURNING handler (MP3 fixup / IRQ) MUST preserve x16-x18
 * properly (e.g. spill them to a scratch sysreg before the guard) — restoring
 * this frame's gpr[16..18] as-is would corrupt live registers. The offsets are
 * frozen; this deviation is part of the recorded MP1 contract.
 *
 * Total size 304 bytes, 16-byte aligned (AArch64 requires SP 16-aligned on
 * exception entry and for stp). elr/spsr and esr/far form 16-aligned stp pairs.
 * The asm and C MUST agree exactly; _Static_assert(offsetof(...)) below pins it.
 */
#ifndef JEIOS_AARCH64_TRAPFRAME_H
#define JEIOS_AARCH64_TRAPFRAME_H

/* Byte offsets — shared by the asm saver (via the JE_TF_* macros) and C. */
#define JE_TF_GPR     0    /* x0..x30, 31 * 8 = 248 bytes */
#define JE_TF_SP      248  /* SP_EL1 at fault time */
#define JE_TF_ELR     256  /* ELR_EL1 — faulting PC (D5-3 fixup key) */
#define JE_TF_SPSR    264  /* SPSR_EL1 — pre-exception PSTATE */
#define JE_TF_ESR     272  /* ESR_EL1 — syndrome (EC/IL/ISS) */
#define JE_TF_FAR     280  /* FAR_EL1 — faulting VA (valid per EC) */
#define JE_TF_VECTOR  288  /* originating vector index 0..15 */
#define JE_TF_PAD     296  /* reserved; keeps size 16-aligned */
#define JE_TF_SIZE    304

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef struct {
    uint64_t gpr[31];   /* x0..x30 */
    uint64_t sp;        /* SP_EL1 at fault */
    uint64_t elr;       /* ELR_EL1 (faulting PC) */
    uint64_t spsr;      /* SPSR_EL1 */
    uint64_t esr;       /* ESR_EL1 */
    uint64_t far;       /* FAR_EL1 */
    uint64_t vector;    /* 0..15 */
    uint64_t pad;       /* reserved */
} je_arm_trapframe;

_Static_assert(sizeof(je_arm_trapframe) == JE_TF_SIZE, "arm trapframe size");
_Static_assert(__builtin_offsetof(je_arm_trapframe, gpr)    == JE_TF_GPR,    "tf gpr");
_Static_assert(__builtin_offsetof(je_arm_trapframe, sp)     == JE_TF_SP,     "tf sp");
_Static_assert(__builtin_offsetof(je_arm_trapframe, elr)    == JE_TF_ELR,    "tf elr");
_Static_assert(__builtin_offsetof(je_arm_trapframe, spsr)   == JE_TF_SPSR,   "tf spsr");
_Static_assert(__builtin_offsetof(je_arm_trapframe, esr)    == JE_TF_ESR,    "tf esr");
_Static_assert(__builtin_offsetof(je_arm_trapframe, far)    == JE_TF_FAR,    "tf far");
_Static_assert(__builtin_offsetof(je_arm_trapframe, vector) == JE_TF_VECTOR, "tf vector");
_Static_assert((JE_TF_SIZE % 16) == 0, "arm trapframe 16-aligned");

/* The common C exception handler (MP1: diagnose + halt; no recovery). */
_Noreturn void je_arm_exception(je_arm_trapframe *tf);
#endif /* __ASSEMBLER__ */

#endif /* JEIOS_AARCH64_TRAPFRAME_H */
