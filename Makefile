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
# Dev-only fixup-injection path is compiled ONLY in the dedicated test mode 11,
# never in the default (mode 0) kernel. A make-check assertion proves its symbols
# are absent from the default ELF.
ifeq ($(TEST_MODE),11)
CPPFLAGS += -DJE_DEV_COPY_INJECT
endif
CFLAGS := -std=c11 -O2 -g $(WARN) -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mno-red-zone -mgeneral-regs-only
CORE := kernel/core/status.c kernel/core/features.c kernel/core/module.c kernel/core/boot.c kernel/core/alloc.c
SOURCES := $(CORE) kernel/core/kheap.c kernel/core/sched.c kernel/core/memory.c kernel/core/vm.c kernel/core/uframe.c kernel/core/syscall.c kernel/core/usercopy.c kernel/core/handle.c kernel/core/endpoint.c kernel/core/endpoint_kern.c kernel/core/device.c kernel/core/proc.c kernel/core/image.c kernel/core/spawn.c kernel/core/main.c kernel/arch/x86_64/platform.c kernel/arch/x86_64/trap.c kernel/arch/x86_64/paging.c kernel/arch/x86_64/vm_backend.c
USER_WARN := -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes
USER_CFLAGS := -std=c11 -O2 $(USER_WARN) -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fcf-protection=none -m64 -march=x86-64 -mcmodel=small -mno-red-zone -mgeneral-regs-only -fno-tree-loop-distribute-patterns -DJE_TEST_MODE=$(TEST_MODE) -Iuser/include -Iinclude
USER_LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -Wl,--build-id=none
USER_COMMON_OBJECTS := $(BUILD)/user/crt0.o $(BUILD)/user/lib/mem.o
USER_SERVICE_NAMES := supervisor restart survivor spoofer crashloop broker_service broker_client uart_driver uart_service uart_client
USER_SERVICE_OBJECTS := $(patsubst %,$(BUILD)/user/services/%.o,$(USER_SERVICE_NAMES))
USER_IMAGES := $(patsubst %,$(BUILD)/user/%.elf,$(USER_SERVICE_NAMES))
USER_GENERATED_IMAGES := $(patsubst %,$(BUILD)/gen/image_%.c,$(USER_SERVICE_NAMES))
USER_IMAGE_OBJECTS := $(patsubst %,$(BUILD)/gen/image_%.o,$(USER_SERVICE_NAMES))
USER_OBJECTS := $(USER_COMMON_OBJECTS) $(BUILD)/user/services/hello.o $(USER_SERVICE_OBJECTS)
USER_IMAGE := $(BUILD)/user/hello.elf
USER_GENERATED := $(BUILD)/gen/image_c_hello.c
USER_IMAGE_OBJECT := $(BUILD)/gen/image_c_hello.o
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES)) $(BUILD)/kernel/arch/x86_64/boot.o $(BUILD)/kernel/arch/x86_64/trap_entry.o $(BUILD)/kernel/arch/x86_64/register_probe.o $(BUILD)/kernel/arch/x86_64/user_demo.o $(BUILD)/kernel/arch/x86_64/usercopy.o $(USER_IMAGE_OBJECT) $(USER_IMAGE_OBJECTS)
.PHONY: all check test iso smoke clean
all: $(BUILD)/jeios.elf
$(BUILD)/jeios.elf: $(OBJECTS) kernel/arch/x86_64/linker.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/arch/x86_64/linker.ld -o $@ $(OBJECTS)
$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -m64 -ffreestanding -fno-pie -c $< -o $@
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user/%.o: user/%.S
	@mkdir -p $(@D)
	$(CC) -Iuser/include -Iinclude -m64 -ffreestanding -fno-pie -fcf-protection=none -MMD -MP -c $< -o $@
$(USER_IMAGE): $(USER_COMMON_OBJECTS) $(BUILD)/user/services/hello.o user/user.ld
	@mkdir -p $(@D)
	$(CC) $(USER_LDFLAGS) -T user/user.ld -o $@ $(USER_COMMON_OBJECTS) $(BUILD)/user/services/hello.o
$(USER_GENERATED): $(USER_IMAGE) tools/mkimage.py
	@mkdir -p $(@D)
	$(PYTHON) tools/mkimage.py --name c_hello --elf $(USER_IMAGE) --out $@
$(USER_IMAGE_OBJECT): $(USER_GENERATED)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.elf: $(USER_COMMON_OBJECTS) $(BUILD)/user/services/%.o user/user.ld
	@mkdir -p $(@D)
	$(CC) $(USER_LDFLAGS) -T user/user.ld -o $@ $(USER_COMMON_OBJECTS) $(BUILD)/user/services/$*.o

$(BUILD)/gen/image_%.c: $(BUILD)/user/%.elf tools/mkimage.py
	@mkdir -p $(@D)
	$(PYTHON) tools/mkimage.py --name $* --elf $< --out $@

$(BUILD)/gen/image_%.o: $(BUILD)/gen/image_%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/core/image.o: kernel/core/image.c $(USER_GENERATED) $(USER_GENERATED_IMAGES) $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	set -e; \
	hello_sha=$$(sed -n 's/.*blob_sha256=\([0-9a-f][0-9a-f]*\).*/\1/p' $(USER_GENERATED)); \
	test "$${hello_sha#????????????????????????????????????????????????????????????????}" != "$$hello_sha"; \
	short_sha=$$(printf '%.16s' "$$hello_sha"); \
	$(CC) $(CPPFLAGS) $(CFLAGS) '-DJE_C_HELLO_BLOB_SHA256="'"$$short_sha"'"' -MMD -MP -c $< -o $@
$(BUILD)/test_core: tests/test_core.c $(CORE) $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=undefined -fno-sanitize-recover=all tests/test_core.c $(CORE) -o $@
test: $(BUILD)/test_core $(BUILD)/test_boot $(BUILD)/test_alloc $(BUILD)/test_vm $(BUILD)/test_uframe $(BUILD)/test_syscall $(BUILD)/test_handle $(BUILD)/test_handle_rw $(BUILD)/test_endpoint $(BUILD)/test_proc $(BUILD)/test_proc_retire $(BUILD)/test_fdt $(BUILD)/test_image $(BUILD)/test_sched_killall $(BUILD)/test_device
	./$(BUILD)/test_core
	./$(BUILD)/test_boot
	./$(BUILD)/test_alloc
	./$(BUILD)/test_vm
	./$(BUILD)/test_uframe
	./$(BUILD)/test_syscall
	./$(BUILD)/test_handle
	./$(BUILD)/test_handle_rw
	./$(BUILD)/test_endpoint
	./$(BUILD)/test_proc
	./$(BUILD)/test_proc_retire
	./$(BUILD)/test_fdt
	./$(BUILD)/test_image
	./$(BUILD)/test_sched_killall
	./$(BUILD)/test_device
check: all test
	$(PYTHON) tools/check_elf.py $(BUILD)/jeios.elf
	$(PYTHON) tools/mkimage.py --name c_hello --elf $(USER_IMAGE) --verify
	$(PYTHON) tools/check_user_image.py $(USER_IMAGE)
	$(PYTHON) tools/test_mkimage.py
	@echo "ABI isolation: abi.h must not include the internal status.h or handle.h"
	@! grep -qE '^[[:space:]]*#[[:space:]]*include.*(status|handle)\.h' include/jeios/abi.h
	@echo "Dev-inject isolation: the fixup-injection path must be absent from the default kernel"
	@! nm $(BUILD)/jeios.elf | grep -qiE 'copy_fixup_inject|user_inject'
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
.DELETE_ON_ERROR:
-include $(OBJECTS:.o=.d)
-include $(USER_OBJECTS:.o=.d)
.PHONY: integration
integration:
	$(PYTHON) tools/integration.py
$(BUILD)/test_boot: tests/test_boot.c kernel/core/boot.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_boot.c kernel/core/boot.c -o $@

$(BUILD)/test_alloc: tests/test_alloc.c kernel/core/alloc.c kernel/core/boot.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_alloc.c kernel/core/alloc.c kernel/core/boot.c -o $@

$(BUILD)/test_vm: tests/test_vm.c tests/vm_fake.c kernel/core/vm.c kernel/core/alloc.c kernel/core/boot.c kernel/core/handle.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_vm.c tests/vm_fake.c kernel/core/vm.c kernel/core/alloc.c kernel/core/boot.c kernel/core/handle.c -o $@

$(BUILD)/test_uframe: tests/test_uframe.c kernel/core/uframe.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_uframe.c kernel/core/uframe.c -o $@

$(BUILD)/test_syscall: tests/test_syscall.c kernel/core/syscall.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DJE_SYSCALL_HOSTED_TEST -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_syscall.c kernel/core/syscall.c -o $@

# M3b handle table: production 16/16 width (boundary + semantics coverage).
$(BUILD)/test_handle: tests/test_handle.c kernel/core/handle.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_handle.c kernel/core/handle.c -o $@

# Same test at a reduced generation width (gen_bits=2 -> GEN_MAX=3) so
# saturation/retirement/capacity-loss are exhaustively exercised (ADR 0008 D5).
$(BUILD)/test_handle_rw: tests/test_handle.c kernel/core/handle.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DJE_HANDLE_TEST_WIDTH -DJE_HANDLE_INDEX_BITS=30u -DJE_HANDLE_GEN_BITS=2u -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_handle.c kernel/core/handle.c -o $@

# M3c endpoint lifecycle core (ADR 0009 D-SEQ step 2): hostable, links the pure
# handle table; proves destruction-on-last-ref, split refcount, and REFOVERFLOW
# with no kernel state and no ring-3 path (create is kernel-only).
$(BUILD)/test_endpoint: tests/test_endpoint.c kernel/core/endpoint.c kernel/core/handle.c kernel/core/proc.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_endpoint.c kernel/core/endpoint.c kernel/core/handle.c kernel/core/proc.c -o $@

# ADR 0010 D4: hosted PROCESS-record pool suite (pure kernel/core/proc.c).
$(BUILD)/test_proc: tests/test_proc.c kernel/core/proc.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_proc.c kernel/core/proc.c -o $@

# ADR 0010 D4: generation retirement with a tiny JE_PROC_GEN_MAX (both TUs).
$(BUILD)/test_proc_retire: tests/test_proc_retire.c kernel/core/proc.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DJE_PROC_GEN_MAX=3u -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_proc_retire.c kernel/core/proc.c -o $@

# Issue #52: hosted bounded-FDT parser suite (pure kernel/core/fdt.c, no arch
# code), ASan+UBSan. Includes the generated real-QEMU fixture header.
$(BUILD)/test_fdt: tests/test_fdt.c kernel/core/fdt.c include/jeios/fdt.h $(wildcard tests/fixtures/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_fdt.c kernel/core/fdt.c -o $@

# ADR 0010 D10 cross-check: a mkimage.py-GENERATED descriptor must pass the real
# kernel validator (kernel/core/vm.c), and mutations of it must fail. The
# generated .c is #included (JE_TEST_IMAGE_C, absolute path so the TU is location
# independent), not linked; only the validator's own TUs are linked.
$(BUILD)/test_image: tests/test_image.c $(USER_GENERATED) kernel/core/vm.c kernel/core/handle.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -DJE_TEST_IMAGE_C='"$(abspath $(USER_GENERATED))"' -DJE_TEST_IMAGE_NAME=c_hello \
	  tests/test_image.c kernel/core/vm.c kernel/core/handle.c -o $@

# ADR 0010 D12: hosted coverage uses the production scheduler and PROCESS pool.
# Private scheduler state is exposed only by the test TU; section GC discards
# unrelated architecture-dependent scheduler entry points.
$(BUILD)/test_sched_killall: tests/test_sched_killall.c kernel/core/proc.c kernel/core/device.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -ffunction-sections -fdata-sections \
	  -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -DJE_SCHED_HOSTED_TEST tests/test_sched_killall.c kernel/core/proc.c -Wl,--gc-sections -o $@

# ADR 0010 D9: hosted DEVICE lifecycle and IRQ policy coverage. Architecture
# operations are stubbed in the test; device.c and the real handle/process cores
# remain under ASan+UBSan.
$(BUILD)/test_device: tests/test_device.c kernel/core/device.c kernel/core/handle.c kernel/core/proc.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined \
	  -fno-sanitize-recover=all tests/test_device.c kernel/core/device.c \
	  kernel/core/handle.c kernel/core/proc.c -o $@


# MP1 (ADR 0002): ARM64 (QEMU virt-8.2) build + smoke, kept fully separate from
# the x86-64 rules above so the default flow is untouched. Targets: `aarch64`,
# `smoke-aarch64`.
include mk/aarch64.mk
