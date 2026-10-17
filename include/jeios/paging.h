/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_PAGING_H
#define JEIOS_PAGING_H
#include <jeios/boot.h>
je_status je_arch_paging_init(const je_boot_context *ctx);
/* BSP only, interrupts disabled, owned kernel RAM page. No SMP TLB shootdown. */
je_status je_arch_page_guard(uintptr_t address);
je_status je_arch_page_restore(uintptr_t address);

/* M3a #4 seam: paging.c is the ONLY writer of kernel tables and of user
 * identity-alias leaves. The per-process backend (vm_backend.c) wires process
 * tables and calls these to share the kernel subtree and flip aliases.
 *
 * je_kernel_pd_phys: physical address of the kernel PD (the frame shared into a
 * process PDPT[0], U clear). The whole kernel+RAM window [0,1GiB) lives under
 * this one PD, so a process reaches kernel memory supervisor-only through it.
 *
 * je_user_alias_set: compare-and-set the identity-alias leaf of a user frame
 * (phys < JE_PHYSICAL_LIMIT) between the two legal states only:
 *   JE_PAGING_ALIAS_RW    == leaf exactly phys|P|W|NX   (allocator default)
 *   JE_PAGING_ALIAS_RO_NX == leaf exactly phys|P|NX     (published CODE/RODATA)
 * Refuses anything inside the kernel image, the guard pages, or out of range,
 * and refuses if the current leaf does not match `expect`. invlpg on success.
 * je_user_alias_get reads the current state for audit. These never touch the
 * kernel image mappings, only the identity window the allocator hands out. */
typedef enum { JE_PAGING_ALIAS_RW = 0, JE_PAGING_ALIAS_RO_NX = 1 } je_paging_alias;
uint64_t  je_kernel_pd_phys(void);
je_status je_user_alias_set(uint64_t phys, je_paging_alias expect, je_paging_alias next);
je_status je_user_alias_get(uint64_t phys, je_paging_alias *out);

/* Test-only (CPL0) CR3 helpers for the #4 step-2 boot test: read the active
 * root, load a process root, restore the kernel root. No ring transition. */
uint64_t je_arch_cr3_read(void);
void     je_arch_cr3_load(uint64_t root_phys);
uint64_t je_arch_kernel_root_phys(void);

extern unsigned char __kernel_start[], __kernel_end[], __text_end[], __rodata_end[];
extern unsigned char je_boot_guard[];
#endif

