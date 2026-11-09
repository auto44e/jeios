/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_USERCOPY_H
#define JEIOS_USERCOPY_H
/* M3a #5 step (b): bounded recoverable user copies (see kernel/core/usercopy.c). */
#include <stdint.h>
#include <jeios/abi.h>
#include <jeios/trap.h>
#if defined(JE_ARCH_AARCH64)
struct je_arm_trapframe;
typedef struct je_arm_trapframe je_arm_trapframe;
#endif

/* Copy between kernel storage and a user arena span. Prevalidate presence +
 * permission for every page, copy through the approved assembly site, and
 * recover a not-present fault there to JE_ABI_INVALID. A nonzero-length call
 * rejects a NULL kernel buffer before scheduler/scope access. The caller must
 * not nest copies while another copy scope is active; a nested call is
 * rejected and leaves the outer scope intact. The copy assumes interrupts are
 * disabled and no scheduling occurs for its duration on the single CPU; this
 * contract does not promise SMP or per-CPU behavior. Zero length is OK and
 * touches neither pointer. No user pointer is retained. */
je_abi_status je_copy_from_user(void *kdst, uint64_t usrc, uint64_t len);
je_abi_status je_copy_to_user(uint64_t udst, const void *ksrc, uint64_t len);

/* Development-only copy boundary probe (ABI op 3): stage <=256 user bytes from
 * src through kernel scratch, then out to dst. Accepts overlap. */
je_abi_status je_copy_probe(uint64_t usrc, uint64_t udst, uint64_t len);

/* Exception-fixup hook: called from je_trap_dispatch on a CPL0 #PF. Returns 1
 * and rewrites frame->rip to the fixup entry iff this is a recoverable copy-site
 * not-present fault under an active, owner-matched, direction-matched scope with
 * CR2 in the declared span; else 0 (caller keeps the fatal path). */
int je_usercopy_try_fixup(je_trap_frame *frame, uint64_t cr2, uint64_t pfec);

#if defined(JE_ARCH_AARCH64)
/* ARM direction-specific leaf sites and the returnable slot-4 predicate. */
int je_copy_from_user_run(void *kdst, const void *usrc, uint64_t len);
int je_copy_to_user_run(void *udst, const void *ksrc, uint64_t len);
int je_arm_copy_fault(je_arm_trapframe *frame);
#ifdef JE_ARM_EL0_TEST
uint32_t je_arm_copy_fixup_count(void);
int je_arm_copy_fixup_inject(void);
#endif
#endif

#ifdef JE_DEV_COPY_INJECT
/* Dev-only (compile-guarded): raw-copy an unmapped arena page from the current
 * syscall context to prove the exception fixup recovers a not-present copy-site
 * #PF. Returns 1 if recovered. Excluded from the default build. */
int je_copy_fixup_inject(uint64_t uaddr);
#endif

#endif
