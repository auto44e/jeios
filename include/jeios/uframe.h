/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_UFRAME_H
#define JEIOS_UFRAME_H
/*
 * M3a #4 step 3 (ADR 0007 D6): pure validator + sanitizer for a user-return
 * trap frame, called on EVERY dispatch to a user thread (first entry included)
 * before IRETQ. No allocation, no globals — hostable so make check unit-tests
 * the edge cases. The CPU writes a fresh frame (with user-chosen RIP/RSP/RFLAGS)
 * on each trap, so re-validation on resume is required, not optional.
 *
 * Checks (reject => terminate the process, never iretq a bad frame):
 *   - cs == JE_UFRAME_USER_CS (0x33), ss == JE_UFRAME_USER_SS (0x2b)
 *   - RIP canonical and within [base, base+size)         (half-open)
 *   - RSP canonical and within (base, base+size]          (top inclusive: a
 *     full-descending stack's initial RSP legitimately equals arena top)
 * Sanitize (force, do not merely check — ring 3 can flip bits via popf):
 *   - rflags := (user & SAFE_FLAGS) | IF | bit1; clears IOPL, NT, TF, RF, VM,
 *     AC, ID, DF and any reserved bits. IF=1 (gate re-enables), IOPL=0.
 */
#include <stdint.h>
#include <stdbool.h>
#include <jeios/trap.h>

#define JE_UFRAME_USER_CS 0x33u
#define JE_UFRAME_USER_SS 0x2bu

/* Validate frame against the arena [base, base+size). On success, write the
 * sanitized RFLAGS to *out_rflags and return true. On any violation return
 * false and leave *out_rflags untouched. Pure; safe to unit-test. */
bool je_uframe_check(const je_trap_frame *f, uint64_t base, uint64_t size,
                     uint64_t *out_rflags);

#endif
