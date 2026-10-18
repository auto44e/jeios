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
    JE_ABI_INVALID     = 2,
    /* M3b handle statuses (ADR 0008 D2): appended, DISTINCT values exposed to
     * ring 3 (not collapsed to INVALID). These are the v0 ABI mapping of the
     * internal JE_HANDLE_* results; abi.h stays independent of handle.h (the
     * glue maps between them). Information leak is not a concern: a handle names
     * only the caller's own process-local table. */
    JE_ABI_H_STALE     = 3,  /* slot FREE/RETIRED or generation mismatch */
    JE_ABI_H_BADINDEX  = 4,  /* handle 0, or index >= table capacity */
    JE_ABI_H_WRONGTYPE = 5,  /* live slot, wrong object type (hosted-only in M3b) */
    JE_ABI_H_DENIED    = 6,  /* live slot, insufficient rights (hosted-only in M3b) */
    JE_ABI_H_QUOTA     = 7,  /* allocate: live-slot count at the process quota */
    JE_ABI_H_NOSLOTS   = 8   /* allocate: no FREE slot remains */
} je_abi_status;
_Static_assert(JE_ABI_OK == 0, "abi ok");
_Static_assert(JE_ABI_UNSUPPORTED == 1, "abi unsupported");
_Static_assert(JE_ABI_INVALID == 2, "abi invalid");
/* M3b handle statuses are ABI too — pin their numbering so a careless reorder
 * can't silently renumber a status ring 3 already depends on. */
_Static_assert(JE_ABI_H_STALE == 3, "abi h_stale");
_Static_assert(JE_ABI_H_BADINDEX == 4, "abi h_badindex");
_Static_assert(JE_ABI_H_WRONGTYPE == 5, "abi h_wrongtype");
_Static_assert(JE_ABI_H_DENIED == 6, "abi h_denied");
_Static_assert(JE_ABI_H_QUOTA == 7, "abi h_quota");
_Static_assert(JE_ABI_H_NOSLOTS == 8, "abi h_noslots");

/* Operation numbers (full 64-bit RAX; any value not listed is UNSUPPORTED). */
typedef enum {
    JE_SYS_ABI_QUERY = 0,  /* abi_query(which): RDI 0=version,1=profile -> RDX */
    JE_SYS_YIELD     = 1,  /* yield(): reschedule caller; no authority grant */
    JE_SYS_EXIT      = 2,  /* exit(code): RDI=code; terminate process, no return */
    JE_SYS_COPY_PROBE = 3, /* dev-only copy_probe(src=RDI, dst=RSI, len=RDX), <=256 */
    /* M3b handle ops (ADR 0008). (vm,index) is resolved ONLY via the current
     * user process; a non-user caller gets JE_ABI_INVALID. */
    JE_SYS_HANDLE_SELF  = 5, /* handle_self(): mint a handle to this process's
                              * SELF object -> handle in RDX, status in RAX */
    JE_SYS_HANDLE_CLOSE = 6, /* handle_close(h=RDI): close; status in RAX */
    JE_SYS_HANDLE_QUERY = 7, /* handle_query(h=RDI): -> RDX = object type (rights
                              * NOT exposed here; reserved out bits zero) */
    JE_SYS_HANDLE_RIGHTS = 8 /* handle_rights(h=RDI): -> RDX = rights mask of the
                              * handle, masked to JE_ABI_RIGHTS_KNOWN. Requires
                              * the INSPECT right (policy hook; see note below). */
#ifdef JE_DEV_COPY_INJECT
    , JE_SYS_COPY_FIXUP_INJECT = 4  /* dev-only: raw-copy an unmapped page to test the fixup */
#endif
} je_abi_op;

/* M3b handle object types (RDX from handle_query). Only SELF exists in M3b: a
 * kernel-minted, immortal, resource-free process-identity capability. Type 0 is
 * reserved/invalid so a zeroed slot never reads as a valid type. */
#define JE_ABI_OBJ_SELF 1u
_Static_assert(JE_SYS_HANDLE_SELF == 5, "abi op handle_self");
_Static_assert(JE_SYS_HANDLE_CLOSE == 6, "abi op handle_close");
_Static_assert(JE_SYS_HANDLE_QUERY == 7, "abi op handle_query");
_Static_assert(JE_SYS_HANDLE_RIGHTS == 8, "abi op handle_rights");

/* M3b rights ABI (ADR 0008 revisit, frozen here). Rights are a fixed uint32_t
 * bitmask. handle_rights masks its output to JE_ABI_RIGHTS_KNOWN so no internal
 * bit can leak to ring 3. Unknown bits crossing the boundary are rejected when a
 * caller-supplied mask first exists (M3c attenuation); in M3b every mint uses a
 * kernel constant, asserted known at compile time.
 *
 * HONESTY (recorded in docs): this is a rights ABI + enforcement POLICY HOOK, not
 * end-to-end ring-3 enforcement. Because the only ring-3 minting path
 * (handle_self) always grants INSPECT, ring 3 can observe success and the mask,
 * but CANNOT observe DENIED/WRONGTYPE yet. Genuine ring-3-observable denial
 * arrives with M3c attenuation (which can mint a rights-reduced handle) — planned
 * as the (B)/A+B follow-up. The denial branch is exercised by hosted tests now. */
#define JE_ABI_RIGHT_INSPECT 0x1u              /* may read the object's metadata */
#define JE_ABI_RIGHTS_KNOWN  (JE_ABI_RIGHT_INSPECT)
_Static_assert(JE_ABI_RIGHT_INSPECT == 0x1u, "abi right inspect");
_Static_assert((JE_ABI_RIGHTS_KNOWN & ~UINT64_C(0xffffffff)) == 0u, "rights fit u32");

/* abi_query selectors (RDI). */
#define JE_ABI_QUERY_VERSION 0u
#define JE_ABI_QUERY_PROFILE 1u

/* Values abi_query returns in RDX. */
#define JE_ABI_VERSION 0u      /* experimental v0 */
#define JE_ABI_PROFILE 0u      /* M3a baseline profile */

#endif
