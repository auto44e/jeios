# SPDX-License-Identifier: Apache-2.0
CC = gcc
LD = ld
PYTHON ?= python3
# Current kernel is a BIOS PC target; do not autodetect EFI modules installed on the host.
GRUB_PC_DIR ?= /usr/lib/grub/i386-pc
TEST_MODE ?= 0
BUILD := build/$(TEST_MODE)
WARN := -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes
CPPFLAGS := -Iinclude -DJE_TEST_MODE=$(TEST_MODE)
CFLAGS := -std=c11 -O2 -g $(WARN) -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mno-red-zone -mgeneral-regs-only
CORE := kernel/core/status.c kernel/core/features.c kernel/core/module.c kernel/core/boot.c kernel/core/alloc.c
SOURCES := $(CORE) kernel/core/kheap.c kernel/core/sched.c kernel/core/memory.c kernel/core/vm.c kernel/core/uframe.c kernel/core/syscall.c kernel/core/main.c kernel/arch/x86_64/platform.c kernel/arch/x86_64/trap.c kernel/arch/x86_64/paging.c kernel/arch/x86_64/vm_backend.c
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES)) $(BUILD)/kernel/arch/x86_64/boot.o $(BUILD)/kernel/arch/x86_64/trap_entry.o $(BUILD)/kernel/arch/x86_64/register_probe.o $(BUILD)/kernel/arch/x86_64/user_demo.o
.PHONY: all check test iso smoke clean
all: $(BUILD)/jeios.elf
$(BUILD)/jeios.elf: $(OBJECTS) kernel/arch/x86_64/linker.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/arch/x86_64/linker.ld -o $@ $(OBJECTS)
$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
$(BUILD)/test_core: tests/test_core.c $(CORE) $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=undefined -fno-sanitize-recover=all tests/test_core.c $(CORE) -o $@
test: $(BUILD)/test_core $(BUILD)/test_boot $(BUILD)/test_alloc $(BUILD)/test_vm $(BUILD)/test_uframe $(BUILD)/test_syscall
	./$(BUILD)/test_core
	./$(BUILD)/test_boot
	./$(BUILD)/test_alloc
	./$(BUILD)/test_vm
	./$(BUILD)/test_uframe
	./$(BUILD)/test_syscall
check: all test
	$(PYTHON) tools/check_elf.py $(BUILD)/jeios.elf
	@echo "ABI isolation: abi.h must not include the internal status.h"
	@! grep -qE '^[[:space:]]*#[[:space:]]*include.*status\.h' include/jeios/abi.h
iso: all
	grub-file --is-x86-multiboot2 $(BUILD)/jeios.elf
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(BUILD)/jeios.elf $(BUILD)/iso/boot/
	cp boot/grub/grub.cfg $(BUILD)/iso/boot/grub/
	grub-mkrescue -d $(GRUB_PC_DIR) -o $(BUILD)/jeios.iso $(BUILD)/iso
smoke: iso
	$(PYTHON) tools/smoke.py $(BUILD)/jeios.iso
clean:
	rm -rf build
-include $(OBJECTS:.o=.d)
.PHONY: integration
integration:
	$(PYTHON) tools/integration.py
$(BUILD)/test_boot: tests/test_boot.c kernel/core/boot.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_boot.c kernel/core/boot.c -o $@

$(BUILD)/test_alloc: tests/test_alloc.c kernel/core/alloc.c kernel/core/boot.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_alloc.c kernel/core/alloc.c kernel/core/boot.c -o $@

$(BUILD)/test_vm: tests/test_vm.c tests/vm_fake.c kernel/core/vm.c kernel/core/alloc.c kernel/core/boot.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_vm.c tests/vm_fake.c kernel/core/vm.c kernel/core/alloc.c kernel/core/boot.c -o $@

$(BUILD)/test_uframe: tests/test_uframe.c kernel/core/uframe.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_uframe.c kernel/core/uframe.c -o $@

$(BUILD)/test_syscall: tests/test_syscall.c kernel/core/syscall.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DJE_SYSCALL_HOSTED_TEST -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_syscall.c kernel/core/syscall.c -o $@

