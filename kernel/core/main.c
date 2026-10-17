/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <stdatomic.h>
#include <jeios/kheap.h>
#include <jeios/sched.h>
#include <jeios/arch.h>
#include <jeios/trap.h>
#include <jeios/boot.h>
#include <jeios/paging.h>
#include <jeios/vm.h>
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

/* ---- M3a #4 step 2: CPL0 real-page-table / real-CR3 boot check ------------ */
static je_vm vm_s2;
static unsigned char vm_blob_s2[128];
static je_vm_image vm_image_s2(void)
{
    je_vm_image image = {0};
    image.version = JE_VM_IMAGE_VERSION;
    image.segment_count = 3u;
    image.entry_offset = 0u;
    image.segments[0] = (je_vm_segment){ 0u, 16u, 0u,               JE_PAGE_SIZE, JE_VM_R|JE_VM_X, 0u };
    image.segments[1] = (je_vm_segment){ 32u, 12u, 2u*JE_PAGE_SIZE, JE_PAGE_SIZE, JE_VM_R,         0u };
    image.segments[2] = (je_vm_segment){ 64u, 8u,  4u*JE_PAGE_SIZE, JE_PAGE_SIZE, JE_VM_R|JE_VM_W,  0u };
    return image;
}
static void vm_blob_init_s2(void)
{
    for (size_t i=0;i<sizeof(vm_blob_s2);++i) vm_blob_s2[i]=0;
    for (size_t i=0;i<16u;++i) vm_blob_s2[i]=(unsigned char)(0x10u+i);  /* CODE page byte 0 = 0x10 */
    for (size_t i=0;i<12u;++i) vm_blob_s2[32u+i]=(unsigned char)(0x80u+i);
    for (size_t i=0;i<8u;++i)  vm_blob_s2[64u+i]=(unsigned char)(0xe0u+i);
}
/* Build one process with the real backend and audit it. Returns its root phys
 * (CR3 value) via *out_root and the arena-absolute address of a USER_DATA page.
 * Fails hard (never returns) on any build/audit error. */
static uint32_t vm_s2_build(uint64_t *out_root)
{
    int nx = je_arch_has_nx();
    const je_vm_ops *ops = je_arch_vm_ops(&je_kernel_pages, nx);
    if (je_vm_init(&vm_s2, ops, &je_kernel_pages, 0)!=JE_OK) fail("JeiOS fatal: s2 vm_init\r\n");
    je_vm_image img = vm_image_s2();
    vm_blob_init_s2();
    if (je_vm_image_validate(&img, sizeof vm_blob_s2)!=JE_OK) fail("JeiOS fatal: s2 image\r\n");
    uint32_t idx;
    if (je_vm_build(&vm_s2, &img, vm_blob_s2, sizeof vm_blob_s2, nx, &idx)!=JE_OK)
        fail("JeiOS fatal: s2 build\r\n");
    if (je_vm_audit(&vm_s2, idx)!=JE_OK) fail("JeiOS fatal: s2 audit\r\n");
    *out_root = vm_s2.procs[idx].root;
    return idx;
}

/* ---- M3a #4 step 3: ring-3 entry proof ------------------------------------ */
extern unsigned char je_user_demo_start[], je_user_demo_end[];
extern unsigned char je_user_fp_start[], je_user_fp_end[];
static je_vm vm_s3;
static unsigned char vm_blob_s3[256];
/* Build a user process whose code page holds a demo loop and whose data page is
 * a heartbeat. With fp!=0 the code is the hostile-SSE stub (for the D7 proof);
 * otherwise the heartbeat-increment loop. Returns the process index; fails hard. */
static uint32_t vm_s3_build(int fp)
{
    unsigned char *start = fp ? je_user_fp_start : je_user_demo_start;
    unsigned char *end   = fp ? je_user_fp_end   : je_user_demo_end;
    size_t code_len=(size_t)(end-start);
    if (code_len==0 || code_len>sizeof(vm_blob_s3)) fail("JeiOS fatal: s3 demo size\r\n");
    for (size_t i=0;i<sizeof(vm_blob_s3);++i) vm_blob_s3[i]=0;
    for (size_t i=0;i<code_len;++i) vm_blob_s3[i]=start[i];
    je_vm_image img={0};
    img.version=JE_VM_IMAGE_VERSION;
    img.segment_count=2u;
    img.entry_offset=0u;
    img.segments[0]=(je_vm_segment){ 0u, code_len, 0u, JE_PAGE_SIZE, JE_VM_R|JE_VM_X, 0u };
    img.segments[1]=(je_vm_segment){ 0u, 0u, JE_PAGE_SIZE, JE_PAGE_SIZE, JE_VM_R|JE_VM_W, 0u };
    int nx=je_arch_has_nx();
    const je_vm_ops *ops=je_arch_vm_ops(&je_kernel_pages, nx);
    if (je_vm_init(&vm_s3, ops, &je_kernel_pages, 0)!=JE_OK) fail("JeiOS fatal: s3 vm_init\r\n");
    if (je_vm_image_validate(&img, sizeof vm_blob_s3)!=JE_OK) fail("JeiOS fatal: s3 image\r\n");
    uint32_t idx;
    if (je_vm_build(&vm_s3, &img, vm_blob_s3, sizeof vm_blob_s3, nx, &idx)!=JE_OK)
        fail("JeiOS fatal: s3 build\r\n");
    if (je_vm_audit(&vm_s3, idx)!=JE_OK) fail("JeiOS fatal: s3 audit\r\n");
    return idx;
}
/* Read the heartbeat word by switching to the process root, reading the arena
 * data page at CPL0, and switching back. Interrupts must be disabled so the
 * timer cannot dispatch while we are on the process root from kernel context. */
static uint64_t vm_s3_heartbeat(uint32_t idx)
{
    uint64_t flags=je_arch_irq_save();
    uint64_t kroot=je_arch_kernel_root_phys();
    je_arch_cr3_load(vm_s3.procs[idx].root);
    volatile uint64_t *hb=(volatile uint64_t *)(uintptr_t)(JE_VM_ARENA_BASE+JE_PAGE_SIZE);
    uint64_t v=*hb;
    je_arch_cr3_load(kroot);
    je_arch_irq_restore(flags);
    return v;
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
    /* #GP from loading a selector outside the GDT limit. The GDT now has 7
     * entries (…, user data 0x28, user code 0x30), so 0x28 is a valid DPL3
     * selector and would NOT fault. Use 0x38 (index 7), still past the limit,
     * so the CPU raises #GP with error code = selector = 0x38. */
    __asm__ volatile("movw %0,%%ds" : : "r"((uint16_t)0x38));
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
    /* M3a #4 step 3 (ADR 0007 D7): deny x87/MMX/SSE/AVX before any user thread
     * can exist, so hostile ring-3 FP/SIMD traps rather than executes/leaks. */
    je_arch_cpu_policy_init();
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

    /* M3a #4 step 2: real per-process page tables + real CR3, exercised at CPL0
     * only (no ring-3, no sched CR3 switch). The happy path runs every boot so
     * `make smoke` covers it; modes 8/9 provoke the single-shot fault probes.
     * ADR 0007 D2 gates user launch on NX: with NX absent je_vm_build refuses
     * (JE_ERR_UNSUPPORTED), so this check is skipped on the legacy CPU profile. */
    if (!je_arch_has_nx()) {
        je_arch_console_write("M3a-s2: skipped; D2 requires NX (legacy CPU profile)\r\n");
    } else {
        size_t vm_free_before=je_kernel_pages.free_count;
        uint64_t proc_root=0;
        uint32_t vm_idx=vm_s2_build(&proc_root);
        uint64_t kroot=je_arch_kernel_root_phys();
        if (je_arch_cr3_read()!=kroot) fail("JeiOS fatal: s2 kernel root mismatch\r\n");
        if (proc_root==kroot || proc_root==0) fail("JeiOS fatal: s2 root not distinct\r\n");
        je_arch_console_write("M3a-s2: process built; audit ok; root distinct\r\n");
#if JE_TEST_MODE == 8
        /* Under the KERNEL root, the arena is unmapped -> #PF not-present (err 0). */
        volatile unsigned char *p=(volatile unsigned char *)(uintptr_t)JE_VM_ARENA_BASE;
        unsigned char sink=*p; (void)sink;
        fail("JeiOS fatal: s2 arena readable under kernel root\r\n");
#endif
        je_arch_cr3_load(proc_root);
        if (je_arch_cr3_read()!=proc_root) fail("JeiOS fatal: s2 cr3 load\r\n");
        /* Entry byte of the CODE page is the first blob byte (0x10). */
        volatile unsigned char *entry=(volatile unsigned char *)(uintptr_t)JE_VM_ARENA_BASE;
        if (*entry!=0x10u) { je_arch_cr3_load(kroot); fail("JeiOS fatal: s2 arena content\r\n"); }
        /* USER_DATA page at arena offset 4*PAGE is writable at CPL0 (W=1). */
        volatile unsigned char *data=(volatile unsigned char *)(uintptr_t)(JE_VM_ARENA_BASE+4u*JE_PAGE_SIZE);
        *data=0x5au; if (*data!=0x5au) { je_arch_cr3_load(kroot); fail("JeiOS fatal: s2 data rw\r\n"); }
#if JE_TEST_MODE == 9
        /* CODE page leaf is RX (W=0); a CPL0 write faults #PF present|write (err 3)
         * because CR0.WP makes the supervisor honour the read-only user leaf. */
        volatile unsigned char *code=(volatile unsigned char *)(uintptr_t)JE_VM_ARENA_BASE;
        *code=0xffu;
        je_arch_cr3_load(kroot);
        fail("JeiOS fatal: s2 code page writable\r\n");
#endif
        je_arch_cr3_load(kroot);
        if (je_arch_cr3_read()!=kroot) fail("JeiOS fatal: s2 cr3 restore\r\n");
        je_arch_console_write("M3a-s2: arena mapped under proc root; data RW; kernel root restored\r\n");
        if (je_vm_destroy(&vm_s2, vm_idx)!=JE_OK) fail("JeiOS fatal: s2 destroy\r\n");
        if (je_kernel_pages.free_count!=vm_free_before) fail("JeiOS fatal: s2 page leak\r\n");
        je_arch_console_write("M3a-s2: process destroyed; page accounting restored\r\n");
    }

    /* M3a #4 step 3: real CPL3 execution under timer preemption (one user task,
     * no fault-locality yet). Gated on NX (D2). Proves: D7 policy took; a user
     * thread entered ring 3 (cs&3==3 frames observed); it survived preemption;
     * its heartbeat strictly increased (real ring-3 progress). */
    if (!je_arch_has_nx()) {
        je_arch_console_write("M3a-s3: skipped; D2 requires NX (legacy CPU profile)\r\n");
    } else {
        /* D7 readback: EM(2)=1, MP(1)=1, TS(3)=1 in CR0; OSFXSR(9)=OSXMMEXCPT(10)
         * =OSXSAVE(18)=FSGSBASE(16)=0 in CR4. Mode 10 additionally proves denial
         * by executing SSE at CPL3 and observing #UD. */
        uint64_t cr0=je_arch_cpu_policy_cr0(), cr4=je_arch_cpu_policy_cr4();
        if (!(cr0&(1u<<2)) || !(cr0&(1u<<1)) || !(cr0&(1u<<3)) ||
            (cr4&(1u<<9)) || (cr4&(1u<<10)) || (cr4&(1u<<18)) || (cr4&(1u<<16)))
            fail("JeiOS fatal: s3 D7 policy not enforced\r\n");
        je_arch_console_write("M3a-s3: D7 FPU/SIMD denial enforced (CR0.EM/MP/TS, CR4 clear)\r\n");

        size_t s3_free_before=je_kernel_pages.free_count;
        (void)s3_free_before;
#if JE_TEST_MODE == 10
        /* D7 enforcement proof: launch a user task that executes SSE at CPL3.
         * With CR0.EM=1 the CPU raises #UD (vector 6) from a cs=0x33 frame,
         * proving hardware denial (not just a compiler flag). The fault reaches
         * the fatal PANIC path (fault-locality is step 4). */
        uint32_t fpidx=vm_s3_build(1);
        if (je_thread_create_user(&vm_s3, fpidx)!=JE_OK) fail("JeiOS fatal: s3 fp user thread\r\n");
        je_arch_console_write("M3a-s3: hostile-FP user thread created; entering ring 3\r\n");
        for (;;) { (void)je_arch_irq_save(); __asm__ volatile("sti; hlt":::"memory"); }
#endif
        uint32_t uidx=vm_s3_build(0);
        uint64_t before_disp=je_sched_user_cpl3_dispatches();
        if (je_thread_create_user(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 user thread\r\n");
        je_arch_console_write("M3a-s3: user thread created; entering ring 3 via timer\r\n");
        uint64_t hb0=vm_s3_heartbeat(uidx);
        /* Run until we have observed enough CPL3 dispatches, then confirm
         * progress. The PIT is already live from M2; bootstrap (slot 0) cannot
         * sleep, so it hlt-loops and the timer preempts into the user thread. */
        uint64_t spins=0;
        for (;;) {
            (void)je_arch_irq_save();
            uint64_t disp=je_sched_user_cpl3_dispatches()-before_disp;
            if (disp>=JE_THREAD_MAX*2) break;
            if (++spins>100000000ull) fail("JeiOS fatal: s3 no CPL3 progress\r\n");
            __asm__ volatile("sti; hlt":::"memory");
        }
        uint64_t hb1=vm_s3_heartbeat(uidx);
        if (hb1<=hb0) fail("JeiOS fatal: s3 heartbeat did not advance\r\n");
        je_arch_console_write("M3a-s3: CPL3 reached; survived preemption; heartbeat advanced ticks=");
        je_arch_console_hex(hb1);je_arch_console_write("\r\n");
        /* Tear down: the user thread never exits (infinite loop), so mark its
         * slot and destroy the process (frees its held kstack). We stop the task
         * by clearing it from scheduling first. */
        (void)je_arch_irq_save(); /* keep interrupts off through teardown */
        if (je_sched_drop_user(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 drop_user\r\n");
        if (je_vm_destroy(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 destroy\r\n");
        if (je_kernel_pages.free_count!=s3_free_before) fail("JeiOS fatal: s3 page leak\r\n");
        je_arch_console_write("M3a-s3: user process torn down; page accounting restored\r\n");
    }

    je_arch_console_write("JeiOS: initialization complete; halted\r\n");
    je_arch_halt();
}

