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

# The nine test-build toggles below are MUTUALLY EXCLUSIVE: each selects its own
# build dir, and with more than one set the last assignment would silently win
# and reuse that dir for a different flag combination (stale objects / a smoke
# run asserting the wrong markers). Refuse the combination outright.
# ARM_MMU_FAULT (a NAME, not 0/1) joins the group as a "1" when non-empty.
ARM_MMU_FAULT_SET := $(if $(strip $(ARM_MMU_FAULT)),1,)
ARM_SCHED_MUTATION_SET := $(if $(strip $(ARM_SCHED_MUTATION)),1,)
ARM_SCHED_TEST_SET := $(if $(filter 1,$(ARM_SCHED_TEST)),1,)
ARM_USER_TEST_SET := $(if $(filter 1,$(ARM_USER_TEST)),1,)
ARM_EL0_TEST_SET := $(if $(filter 1,$(ARM_EL0_TEST)),1,)
ifneq ($(ARM_SCHED_MUTATION_SET),)
ifeq ($(ARM_SCHED_TEST),1)
else
$(error ARM_SCHED_MUTATION requires ARM_SCHED_TEST=1)
endif
ARM_SCHED_TEST_SET := 1
endif
ifneq ($(words $(filter 1,$(ARM_BSS_CHECK) $(ARM_VECTOR_TEST) $(ARM_DTB_BAD) $(ARM_MMU_FAULT_SET) $(ARM_TIMER_POLL) $(ARM_TIMER_TEST) $(ARM_SCHED_TEST_SET) $(ARM_USER_TEST_SET) $(ARM_EL0_TEST_SET))),0)
ifneq ($(words $(filter 1,$(ARM_BSS_CHECK) $(ARM_VECTOR_TEST) $(ARM_DTB_BAD) $(ARM_MMU_FAULT_SET) $(ARM_TIMER_POLL) $(ARM_TIMER_TEST) $(ARM_SCHED_TEST_SET) $(ARM_USER_TEST_SET) $(ARM_EL0_TEST_SET))),1)
$(error ARM_BSS_CHECK, ARM_VECTOR_TEST, ARM_DTB_BAD, ARM_MMU_FAULT, ARM_TIMER_POLL, ARM_TIMER_TEST, ARM_SCHED_TEST, ARM_USER_TEST and ARM_EL0_TEST are mutually exclusive; set at most one)
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
# Optional deliberate MMU-fault build (MP2 PR-1b, ADR 0011 D4/D7): after the MMU
# markers, perform ONE access the W^X/guard contract must refuse and assert the
# exact syndrome in je_arm_exception. ARM_MMU_FAULT selects which (one build per
# mode, each in its OWN build dir so modes never share stale objects):
#   text   write-to-text        DABT perm L3, WnR=1
#   data   execute-from-data    IABT perm L3
#   rodata execute-from-rodata  IABT perm L3 (WXN does not cover rodata)
#   guard  stack-guard          DABT translation L3, WnR=1
# `make aarch64 ARM_MMU_FAULT=text && make smoke-aarch64 ARM_MMU_FAULT=text`.
ARM_MMU_FAULT_text   := 1
ARM_MMU_FAULT_data   := 2
ARM_MMU_FAULT_rodata := 3
ARM_MMU_FAULT_guard  := 4
ARM_MMU_FAULT_SMOKE_text   := write-to-text
ARM_MMU_FAULT_SMOKE_data   := execute-from-data
ARM_MMU_FAULT_SMOKE_rodata := execute-from-rodata
ARM_MMU_FAULT_SMOKE_guard  := stack-guard
ifneq ($(ARM_MMU_FAULT_SET),)
ifeq ($(ARM_MMU_FAULT_$(ARM_MMU_FAULT)),)
$(error ARM_MMU_FAULT=$(ARM_MMU_FAULT) is not one of: text data rodata guard)
endif
ARM_BUILD   := build/aarch64-mmufault-$(ARM_MMU_FAULT)
endif
# Optional polled GIC/timer proof (MP2 PR-2a, ADR 0012 D8): after the default
# GIC/timer markers, poll the CNTP PPI 4 times with DAIF.I still MASKED and run
# the full ACK/handle/EOI chain each time. `make aarch64 ARM_TIMER_POLL=1 &&
# make smoke-aarch64 ARM_TIMER_POLL=1`. Separate build dir (no stale objects).
ifeq ($(ARM_TIMER_POLL),1)
ARM_BUILD   := build/aarch64-timerpoll
endif
# Optional returning IRQ/timer proof (MP2 PR-2b, ADR 0012 D5/D8): unmask only
# PSTATE.I in the hand-written register-canary loop, collect N=4 returning
# timer frames, then run the BRK negative control. Separate build dir.
ifeq ($(ARM_TIMER_TEST),1)
ARM_BUILD   := build/aarch64-timertest
endif
# Optional MP2 preemptive scheduler proof (ADR 0013 D2/D3/D5/D6/D7). The
# mutation profiles deliberately share the scheduler source but use distinct
# build directories so a stale normal object can never masquerade as a proof.
ifeq ($(ARM_SCHED_TEST),1)
ARM_BUILD   := build/aarch64-schedtest
endif
ifneq ($(ARM_SCHED_MUTATION_SET),)
ARM_BUILD   := build/aarch64-schedtest-$(ARM_SCHED_MUTATION)
endif
# MP3a-1 EL1-only TTBR0/AT S1E0 proof. It builds two process roots and halts
# after the serial oracle sees the complete permission/isolation proof.
ifeq ($(ARM_USER_TEST),1)
ARM_BUILD   := build/aarch64-usertest
endif
# MP3a-2 actual EL0 entry/SVC/fault-death proof.
ifeq ($(ARM_EL0_TEST),1)
ARM_BUILD   := build/aarch64-el0test
endif

# Freestanding, no PIC/stack-protector, STRICT alignment (pre-MMU RAM is
# Device-nGnRnE so unaligned accesses fault), and general-regs-only (a stray
# FP/SIMD op would trap into absent vectors and hang). No native tuning.
ARM_CFLAGS  := -Iinclude -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion \
               -Wshadow -Wstrict-prototypes -ffreestanding -fno-builtin \
               -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
               -mstrict-align -mgeneral-regs-only -mcpu=cortex-a72 \
               -DJE_ARCH_AARCH64
ARM_ASFLAGS := -Iinclude -mstrict-align -mgeneral-regs-only -mcpu=cortex-a72 \
               -DJE_ARCH_AARCH64
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
ifeq ($(ARM_TIMER_POLL),1)
ARM_CFLAGS  += -DJE_ARM_TIMER_POLL
ARM_ASFLAGS += -DJE_ARM_TIMER_POLL
endif
ifeq ($(ARM_TIMER_TEST),1)
ARM_CFLAGS  += -DJE_ARM_TIMER_TEST
ARM_ASFLAGS += -DJE_ARM_TIMER_TEST
endif
ifeq ($(ARM_SCHED_TEST),1)
ARM_CFLAGS  += -DJE_ARM_SCHED_TEST
ARM_ASFLAGS += -DJE_ARM_SCHED_TEST
endif
ifeq ($(ARM_USER_TEST),1)
ARM_CFLAGS  += -DJE_ARM_USER_TEST
ARM_ASFLAGS += -DJE_ARM_USER_TEST
endif
ifeq ($(ARM_EL0_TEST),1)
ARM_CFLAGS  += -DJE_ARM_EL0_TEST -DJE_ARM_NO_DEVICE_HOOKS \
               -DJE_ARM_NO_DEVICE_HOOKS_ACK
ARM_ASFLAGS += -DJE_ARM_EL0_TEST
endif
ARM_SCHED_MUTATION_DEFINE_drop-mov-sp := JE_ARM_SCHED_MUTANT_DROP_MOV_SP
ARM_SCHED_MUTATION_DEFINE_pick-skips-b := JE_ARM_SCHED_MUTANT_PICK_SKIP_B
ARM_SCHED_MUTATION_DEFINE_rebuild-fake-frame := JE_ARM_SCHED_MUTANT_REBUILD_FAKE
ARM_SCHED_MUTATION_DEFINE_unconditional-cur-ready := JE_ARM_SCHED_MUTANT_UNCONDITIONAL_CUR_READY
ARM_SCHED_MUTATION_DEFINE_wfi-i-masked := JE_ARM_SCHED_MUTANT_WFI_MASKED
ARM_SCHED_MUTATION_DEFINE_corrupt-spsr-nzcv := JE_ARM_SCHED_MUTANT_CORRUPT_SPSR_NZCV
ARM_SCHED_MUTATION_DEFINE_spsr-i := JE_ARM_SCHED_MUTANT_SET_SPSR_I
ARM_SCHED_MUTATION_DEFINE_early-fake-swap := JE_ARM_SCHED_MUTANT_EARLY_FAKE_SWAP
ARM_SCHED_MUTATION_DEFINE_live-pointer-swap := JE_ARM_SCHED_MUTANT_LIVE_POINTER_SWAP
ARM_SCHED_MUTATION_DEFINE_live-content-swap := JE_ARM_SCHED_MUTANT_LIVE_CONTENT_SWAP
ARM_SCHED_MUTATION_DEFINE_drop-high-reg-restore := JE_ARM_SCHED_MUTANT_DROP_HIGH_REG_RESTORE
ARM_SCHED_MUTATION_DEFINE_drop-consume-on-resume := JE_ARM_SCHED_MUTANT_DROP_CONSUME
ARM_SCHED_MUTATION_DEFINE_replay-stale-frame := JE_ARM_SCHED_MUTANT_REPLAY_STALE_FRAME
ifneq ($(ARM_SCHED_MUTATION_SET),)
ifeq ($(ARM_SCHED_MUTATION_DEFINE_$(ARM_SCHED_MUTATION)),)
$(error ARM_SCHED_MUTATION=$(ARM_SCHED_MUTATION) is not one of: drop-mov-sp pick-skips-b rebuild-fake-frame unconditional-cur-ready wfi-i-masked corrupt-spsr-nzcv spsr-i early-fake-swap live-pointer-swap live-content-swap drop-high-reg-restore drop-consume-on-resume replay-stale-frame)
endif
ARM_CFLAGS  += -D$(ARM_SCHED_MUTATION_DEFINE_$(ARM_SCHED_MUTATION))
ARM_ASFLAGS += -D$(ARM_SCHED_MUTATION_DEFINE_$(ARM_SCHED_MUTATION))
endif
ifneq ($(ARM_MMU_FAULT_SET),)
ARM_CFLAGS  += -DJE_ARM_MMU_FAULT=$(ARM_MMU_FAULT_$(ARM_MMU_FAULT))
ARM_ASFLAGS += -DJE_ARM_MMU_FAULT=$(ARM_MMU_FAULT_$(ARM_MMU_FAULT))
endif

ARM_OBJS := $(ARM_BUILD)/boot.o $(ARM_BUILD)/main.o $(ARM_BUILD)/gic.o $(ARM_BUILD)/physmem.o $(ARM_BUILD)/paging.o $(ARM_BUILD)/paging_enable.o $(ARM_BUILD)/alloc.o $(ARM_BUILD)/bootctx.o $(ARM_BUILD)/memory.o $(ARM_BUILD)/vectors.o $(ARM_BUILD)/fdt.o $(ARM_BUILD)/sched_rr.o $(ARM_BUILD)/sched_arm.o $(ARM_BUILD)/vm.o $(ARM_BUILD)/handle.o $(ARM_BUILD)/vm_backend.o
ifeq ($(ARM_EL0_TEST),1)
ARM_OBJS += $(ARM_BUILD)/proc.o $(ARM_BUILD)/lifecycle.o $(ARM_BUILD)/syscall.o $(ARM_BUILD)/el0_test.o
endif

# Shared arch-neutral bounded DTB parser (issue #52), compiled with the ARM
# freestanding/strict-align flags.
$(ARM_BUILD)/fdt.o: kernel/core/fdt.c include/jeios/fdt.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/sched_rr.o: kernel/core/sched_rr.c include/jeios/sched_rr.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/sched_arm.o: $(ARM_ARCH)/sched_arm.c $(ARM_ARCH)/paging.h $(ARM_ARCH)/trapframe.h include/jeios/sched_rr.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/vm.o: kernel/core/vm.c include/jeios/vm.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/handle.o: kernel/core/handle.c include/jeios/handle.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/vm_backend.o: $(ARM_ARCH)/vm_backend.c $(ARM_ARCH)/paging.h include/jeios/vm.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/proc.o: kernel/core/proc.c include/jeios/proc.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/lifecycle.o: kernel/core/lifecycle.c include/jeios/lifecycle.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/syscall.o: kernel/core/syscall.c include/jeios/syscall.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -DJE_SYSCALL_HOSTED_TEST -c $< -o $@

$(ARM_BUILD)/el0_test.o: $(ARM_ARCH)/el0_test.c $(ARM_ARCH)/paging.h $(ARM_ARCH)/trapframe.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/boot.o: $(ARM_ARCH)/boot.S $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_ASFLAGS) -c $< -o $@

$(ARM_BUILD)/vectors.o: $(ARM_ARCH)/vectors.S $(ARM_ARCH)/trapframe.h
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/main.o: $(ARM_ARCH)/main.c $(ARM_ARCH)/trapframe.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/gic.o: $(ARM_ARCH)/gic.c $(ARM_ARCH)/gic.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/physmem.o: $(ARM_ARCH)/physmem.c $(ARM_ARCH)/physmem.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/paging.o: $(ARM_ARCH)/paging.c $(ARM_ARCH)/paging.h $(ARM_ARCH)/physmem.h $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -I$(ARM_ARCH) -c $< -o $@

$(ARM_BUILD)/paging_enable.o: $(ARM_ARCH)/paging_enable.S
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_ASFLAGS) -c $< -o $@

$(ARM_BUILD)/alloc.o: kernel/core/alloc.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/bootctx.o: kernel/core/boot.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_BUILD)/memory.o: kernel/core/memory.c
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

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
	$(PYTHON) tools/smoke_arm.py $(ARM_BUILD)/Image --elf $(ARM_BUILD)/jeios-arm.elf --log $(ARM_BUILD)/serial.log --timeout $(ARM_SMOKE_TIMEOUT) $(if $(filter 1,$(ARM_BSS_CHECK)),--bss-verified,) $(if $(filter 1,$(ARM_VECTOR_TEST)),--vector-test,) $(if $(filter 1,$(ARM_DTB_BAD)),--dtb-bad,) $(if $(filter 1,$(ARM_PHYSALLOC)),--phys-alloc,) $(if $(filter 1,$(ARM_TIMER_POLL)),--timer-poll,) $(if $(filter 1,$(ARM_TIMER_TEST)),--timer-test,) $(if $(filter 1,$(ARM_SCHED_TEST)),--sched-test,) $(if $(filter 1,$(ARM_USER_TEST)),--user-test,) $(if $(filter 1,$(ARM_EL0_TEST)),--el0-test,) $(if $(filter 1,$(ARM_SCHED_MUTATION_SET)),--sched-mutation $(ARM_SCHED_MUTATION),) $(if $(filter 1,$(ARM_REQUIRE)),--require,) $(if $(filter 1,$(ARM_MMU_FAULT_SET)),--mmu-fault $(ARM_MMU_FAULT_SMOKE_$(ARM_MMU_FAULT)),)
