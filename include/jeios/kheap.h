/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_KHEAP_H
#define JEIOS_KHEAP_H
#include <jeios/alloc.h>
extern je_pages je_kernel_pages;
je_status je_kheap_init(const je_boot_context *boot);
je_status je_kalloc(size_t bytes,void **out);
je_status je_kfree(void *pointer);
#endif

