/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <stdatomic.h>
#include <jeios/kheap.h>
#include <jeios/sched.h>
#include <jeios/arch.h>
#include <jeios/trap.h>
#include <jeios/boot.h>
#include <jeios/paging.h>
static je_boot_context boot;
static _Noreturn void fail(const char *message)
{
    (void)je_arch_irq_save();
    je_arch_console_write(message); je_arch_halt();
}
static atomic_uint completed;
static void worker(void *argument)
{
    uintptr_t id=(uintptr_t)argument;
    void *object;
    size_t bytes=5000+id*17;
    if (je_kalloc(bytes,&object)!=JE_OK) fail("JeiOS fatal: worker allocation\r\n");
    unsigned char *p=object;
    for (size_t i=0;i<bytes;++i) {
        if (p[i]) fail("JeiOS fatal: allocation not zeroed\r\n");
        p[i]=(unsigned char)(i^id);
    }
    if (je_arch_register_probe()) fail("JeiOS fatal: register corruption\r\n");
    if (je_thread_sleep(UINT64_MAX)!=JE_ERR_INVALID) fail("JeiOS fatal: sleep overflow\r\n");
    if (je_thread_sleep(0)!=JE_OK) fail("JeiOS fatal: yield\r\n");
    uint64_t before=je_sched_ticks();
    if (je_thread_sleep(3+id)!=JE_OK || je_sched_ticks()-before<3+id)
        fail("JeiOS fatal: early wake\r\n");
    for (size_t i=0;i<bytes;++i)
        if (p[i]!=(unsigned char)(i^id)) fail("JeiOS fatal: heap corruption\r\n");
    if (je_kfree(p)!=JE_OK) fail("JeiOS fatal: worker free\r\n");
    atomic_fetch_add_explicit(&completed,1,memory_order_release);
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
    if (je_kheap_init(&boot)!=JE_OK) fail("JeiOS fatal: allocator init\r\n");
    size_t free_before=je_kernel_pages.free_count;
    je_arch_console_write("M2: allocator ready; free pages=");
    je_arch_console_hex(free_before);je_arch_console_write("\r\n");
    je_sched_init();
    if (je_thread_sleep(1)!=JE_ERR_DENIED) fail("JeiOS fatal: idle slept\r\n");
    for (uintptr_t i=1;i<JE_THREAD_MAX;++i)
        if (je_thread_create(worker,(void *)i)!=JE_OK) fail("JeiOS fatal: thread creation\r\n");
    size_t free_after_create=je_kernel_pages.free_count;
    if (je_thread_create(worker,0)!=JE_ERR_NOMEM || je_kernel_pages.free_count!=free_after_create)
        fail("JeiOS fatal: thread capacity rollback\r\n");
    je_arch_console_write("M2: seven guarded threads ready; enabling PIT\r\n");
    je_arch_timer_init();
    for (;;) {
        (void)je_arch_irq_save();
        if (atomic_load_explicit(&completed,memory_order_acquire)==JE_THREAD_MAX-1 && je_sched_finished()) break;
        __asm__ volatile("sti; hlt":::"memory");
    }
    if (je_sched_preemptions()<JE_THREAD_MAX*2 || je_sched_min_dispatches()<3)
        fail("JeiOS fatal: insufficient preemption\r\n");
    je_arch_console_write("M2: preemption/GPR/sleep/heap checks passed; ticks=");
    je_arch_console_hex(je_sched_ticks());je_arch_console_write("\r\n");
    if (je_sched_reap()!=JE_OK || je_kernel_pages.free_count!=free_before)
        fail("JeiOS fatal: stack or heap resource leak\r\n");
    je_arch_console_write("M2: exited threads reaped; page accounting restored\r\n");
    je_arch_console_write("JeiOS: initialization complete; halted\r\n");
    je_arch_halt();
}

