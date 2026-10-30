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

# The three test-build toggles below are MUTUALLY EXCLUSIVE: each selects its own
# build dir, and with more than one set the last assignment would silently win
# and reuse that dir for a different flag combination (stale objects / a smoke
# run asserting the wrong markers). Refuse the combination outright.
ifneq ($(words $(filter 1,$(ARM_BSS_CHECK) $(ARM_VECTOR_TEST) $(ARM_DTB_BAD))),0)
ifneq ($(words $(filter 1,$(ARM_BSS_CHECK) $(ARM_VECTOR_TEST) $(ARM_DTB_BAD))),1)
$(error ARM_BSS_CHECK, ARM_VECTOR_TEST and ARM_DTB_BAD are mutually exclusive; set at most one to 1)
endif
endif

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
# Optional parser fault-injection build (issue #52): after the DTB copy and
# before the parse, overwrite the first struct token IN THE COPY with the unknown
# token 0x0000000A; the parser must reject with BAD_TOKEN and the boot must halt
# before "boot context validated". `make aarch64 ARM_DTB_BAD=1`. Separate build
# dir so the toggle never reuses stale objects.
ifeq ($(ARM_DTB_BAD),1)
ARM_BUILD   := build/aarch64-dtbbad
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
ifeq ($(ARM_DTB_BAD),1)
ARM_CFLAGS  += -DJE_ARM_DTB_BAD
ARM_ASFLAGS += -DJE_ARM_DTB_BAD
endif

ARM_OBJS := $(ARM_BUILD)/boot.o $(ARM_BUILD)/main.o $(ARM_BUILD)/vectors.o $(ARM_BUILD)/fdt.o

# Shared arch-neutral bounded DTB parser (issue #52), compiled with the ARM
# freestanding/strict-align flags.
$(ARM_BUILD)/fdt.o: kernel/core/fdt.c include/jeios/fdt.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

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
# harness captures serial to a FILE (stdout races the timeout kill) — always
# $(ARM_BUILD)/serial.log so CI can upload it — rejects any unexpected FATAL
# line, requires the DTB markers (incl. a CRC32 match against the committed
# fixture) and the terminal marker, records image SHA-256 + tool versions + the
# exact command, and SKIPs (never PASSes) if the toolchain or
# qemu-system-aarch64 is absent — unless ARM_REQUIRE=1, which turns every SKIP
# into a FAIL (remote CI). Not wired into `make check`.
# ARM_SMOKE_TIMEOUT (seconds, finite and > 0, default 15; 0/nan/inf are a parse
# error) bounds the whole QEMU run INCLUDING the 0.5 s post-terminal settle
# window; the harness stops QEMU once the mode's final marker is seen as a
# complete line and the settle elapses, so a PASS normally takes well under it.
# Expiry without that marker, or with less than the settle window left, is a FAIL.
# The ELF the Image was objcopied from is passed via --elf: under
# ARM_VECTOR_TEST=1 the harness resolves je_arm_brk_site from it and requires the
# reported ELR to equal that address exactly (missing ELF is a FAIL).
# Variable names ARM_DTB_BAD / ARM_REQUIRE / ARM_SMOKE_TIMEOUT are frozen (CI
# uses them).
ARM_SMOKE_TIMEOUT ?= 15
smoke-aarch64: $(ARM_BUILD)/Image
	$(PYTHON) tools/smoke_arm.py $(ARM_BUILD)/Image --elf $(ARM_BUILD)/jeios-arm.elf --log $(ARM_BUILD)/serial.log --timeout $(ARM_SMOKE_TIMEOUT) $(if $(filter 1,$(ARM_BSS_CHECK)),--bss-verified,) $(if $(filter 1,$(ARM_VECTOR_TEST)),--vector-test,) $(if $(filter 1,$(ARM_DTB_BAD)),--dtb-bad,) $(if $(filter 1,$(ARM_REQUIRE)),--require,)
