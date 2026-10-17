/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ABI_H
#define JEIOS_ABI_H
/*
 * M3a #5: experimental native syscall ABI v0 (ADR 0007 D8/D10). This is the
 * ONLY status/op numbering exposed to ring 3. It is explicitly numbered and is
 * DELIBERATELY independent of the internal <jeios/status.h> enum — the kernel
 * must never pass internal je_status numbering across the user boundary (M3A.md
 * rule 5). This header includes <stdint.h> only; a make-check grep enforces that
 * it never includes status.h.
 *
 * Call convention (int 0x80 gate, DPL3, IST=0):
 *   RAX  = operation number in; je_abi_status out.
 *   RDI,RSI,RDX,R10,R8,R9 = up to six argument registers in.
 *   RDX  = optional single u64 scalar result out (0 when no result).
 * Unknown operations return JE_ABI_UNSUPPORTED regardless of arguments. A
 * recognized operation with a nonzero reserved argument returns JE_ABI_INVALID
 * and has no side effect. No stable ABI promise; v0 is experimental.
 */
#include <stdint.h>

/* Explicitly numbered status. NOT related to je_status in status.h. */
typedef enum {
    JE_ABI_OK          = 0,
    JE_ABI_UNSUPPORTED = 1,
    JE_ABI_INVALID     = 2
} je_abi_status;
_Static_assert(JE_ABI_OK == 0, "abi ok");
_Static_assert(JE_ABI_UNSUPPORTED == 1, "abi unsupported");
_Static_assert(JE_ABI_INVALID == 2, "abi invalid");

/* Operation numbers (full 64-bit RAX; any value not listed is UNSUPPORTED). */
typedef enum {
    JE_SYS_ABI_QUERY = 0,  /* abi_query(which): RDI 0=version,1=profile -> RDX */
    JE_SYS_YIELD     = 1,  /* yield(): reschedule caller; no authority grant */
    JE_SYS_EXIT      = 2   /* exit(code): RDI=code; terminate process, no return */
    /* op 3 (copy_probe) is reserved for M3a #5 step (b); until then it is
     * UNSUPPORTED like any unknown op — no stub that reports success. */
} je_abi_op;

/* abi_query selectors (RDI). */
#define JE_ABI_QUERY_VERSION 0u
#define JE_ABI_QUERY_PROFILE 1u

/* Values abi_query returns in RDX. */
#define JE_ABI_VERSION 0u      /* experimental v0 */
#define JE_ABI_PROFILE 0u      /* M3a baseline profile */

#endif
