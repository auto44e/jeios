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
