/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_MEMORY_H
#define JEIOS_MEMORY_H

#include <stddef.h>

/* Freestanding, non-overlapping byte helpers supplied by kernel/core/memory.c. */
void *memset(void *destination, int value, size_t count);
void *memcpy(void *destination, const void *source, size_t count);

#endif /* JEIOS_MEMORY_H */
