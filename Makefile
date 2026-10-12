CC = gcc
LD = ld
PYTHON ?= python3
BUILD := build
WARN := -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes
CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -O2 -g $(WARN) -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mno-red-zone -mgeneral-regs-only
CORE := kernel/core/status.c kernel/core/features.c kernel/core/module.c
SOURCES := $(CORE) kernel/core/main.c kernel/arch/x86_64/platform.c
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES)) $(BUILD)/kernel/arch/x86_64/boot.o
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
test: $(BUILD)/test_core
	./$(BUILD)/test_core
check: all test
	$(PYTHON) tools/check_elf.py $(BUILD)/jeios.elf
iso: all
	grub-file --is-x86-multiboot2 $(BUILD)/jeios.elf
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(BUILD)/jeios.elf $(BUILD)/iso/boot/
	cp boot/grub/grub.cfg $(BUILD)/iso/boot/grub/
	grub-mkrescue -o $(BUILD)/jeios.iso $(BUILD)/iso
smoke: iso
	$(PYTHON) tools/smoke.py $(BUILD)/jeios.iso
clean:
	rm -rf $(BUILD)
-include $(OBJECTS:.o=.d)
