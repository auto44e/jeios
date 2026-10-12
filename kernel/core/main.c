#include <stdint.h>
#include <jeios/arch.h>
#include <jeios/trap.h>
#include <jeios/boot.h>
#include <jeios/paging.h>
static je_boot_context boot;
static _Noreturn void fail(const char *message)
{
    je_arch_console_write(message); je_arch_halt();
}
_Noreturn void je_kernel_main(uint32_t magic, uintptr_t info)
{
    (void)je_arch_console_init();
    je_arch_console_write("JeiOS: entered C kernel\r\n");
    je_arch_traps_init();
    je_arch_console_write("M1: traps ready\r\n");
#if JE_TEST_MODE == 1
    __asm__ volatile("ud2");
#elif JE_TEST_MODE == 2
    __asm__ volatile("movw %0,%%ds" : : "r"((uint16_t)0x28));
#elif JE_TEST_MODE == 3
    je_arch_test_double_fault();
#endif
    if (magic != 0x36d76289u || (info&7) || info<0x1000 || info>JE_PHYSICAL_LIMIT-16)
        fail("JeiOS fatal: invalid boot pointer\r\n");
    size_t accessible=(size_t)(JE_PHYSICAL_LIMIT-info);
    if (accessible>JE_BOOT_BYTES_MAX) accessible=JE_BOOT_BYTES_MAX;
    je_status s=je_boot_parse((const void *)info,accessible,info,(uintptr_t)__kernel_start,
                              (uintptr_t)__kernel_end,&boot);
    if (s!=JE_OK) fail("JeiOS fatal: invalid memory map\r\n");
    je_arch_console_write("M1: boot map validated; reservations ready\r\n");
    if (je_arch_paging_init(&boot)!=JE_OK) fail("JeiOS fatal: paging setup\r\n");
    je_arch_console_write(je_arch_has_nx() ? "CPU: NX enforced\r\n" :
                                            "CPU: legacy profile, NX absent\r\n");
    je_arch_console_write("M1: protected mappings and stack guards ready\r\n");
#if JE_TEST_MODE == 4
    __asm__ volatile("movq $1,(%%rax)"::"a"((uintptr_t)__kernel_start):"memory");
#elif JE_TEST_MODE == 5
    __asm__ volatile("movq (%%rax),%%rax"::"a"((uintptr_t)je_boot_guard):"memory");
#elif JE_TEST_MODE == 6
    if (!je_arch_has_nx()) fail("JeiOS fatal: NX test unsupported\r\n");
    ((void (*)(void))(uintptr_t)&boot)();
#elif JE_TEST_MODE == 7
    je_arch_test_stack_overflow((uintptr_t)je_boot_guard+4096);
#endif
    je_arch_console_write("JeiOS: initialization complete; halted\r\n");
    je_arch_halt();
}
