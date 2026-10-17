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

/* ---- M3a #4 step 3/4: ring-3 entry + fault-local death proof -------------- */
extern unsigned char je_user_demo_start[], je_user_demo_end[];
extern unsigned char je_user_fp_start[], je_user_fp_end[];
extern unsigned char je_user_fault_start[], je_user_fault_end[];
extern unsigned char je_user_int80_start[], je_user_int80_end[];
extern unsigned char je_user_badrsp_start[], je_user_badrsp_end[];
static je_vm vm_s3;
static unsigned char vm_blob_s3[256];
typedef enum { PROG_HEARTBEAT=0, PROG_FP=1, PROG_FAULT=2, PROG_INT80=3, PROG_BADRSP=4 } user_prog;
static int vm_s3_inited;
/* Initialize the shared step-3/4 vm once (it holds up to JE_VM_MAX_PROCESSES
 * processes — A and B share it, matching the 2-process admission quota). */
static void vm_s3_init_once(void)
{
    if (vm_s3_inited) return;
    const je_vm_ops *ops=je_arch_vm_ops(&je_kernel_pages, je_arch_has_nx());
    if (je_vm_init(&vm_s3, ops, &je_kernel_pages, 0)!=JE_OK) fail("JeiOS fatal: s3 vm_init\r\n");
    vm_s3_inited=1;
}
/* Build one user process into the shared vm from the selected program (code at
 * arena 0, a heartbeat/scratch data page at 0x1000). Returns its index. */
static uint32_t vm_s3_build(user_prog prog)
{
    vm_s3_init_once();
    unsigned char *start, *end;
    switch (prog) {
    case PROG_FP:     start=je_user_fp_start;     end=je_user_fp_end;     break;
    case PROG_FAULT:  start=je_user_fault_start;  end=je_user_fault_end;  break;
    case PROG_INT80:  start=je_user_int80_start;  end=je_user_int80_end;  break;
    case PROG_BADRSP: start=je_user_badrsp_start; end=je_user_badrsp_end; break;
    default:          start=je_user_demo_start;   end=je_user_demo_end;   break;
    }
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
    if (je_vm_image_validate(&img, sizeof vm_blob_s3)!=JE_OK) fail("JeiOS fatal: s3 image\r\n");
    uint32_t idx;
    if (je_vm_build(&vm_s3, &img, vm_blob_s3, sizeof vm_blob_s3, je_arch_has_nx(), &idx)!=JE_OK)
        fail("JeiOS fatal: s3 build\r\n");
    if (je_vm_audit(&vm_s3, idx)!=JE_OK) fail("JeiOS fatal: s3 audit\r\n");
    return idx;
}
/* Read the heartbeat word of process idx by switching to its root at CPL0.
 * Interrupts must be disabled so the timer cannot dispatch mid-switch. */
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
#if JE_TEST_MODE == 10
        /* D7 enforcement proof: launch a user task that executes SSE at CPL3.
         * With CR0.EM=1 the CPU raises #UD (vector 6) from a cs=0x33 frame. In
         * step 4 this no longer PANICs — it terminates only that process; the
         * kernel survives and halts normally. */
        uint32_t fpidx=vm_s3_build(PROG_FP);
        if (je_thread_create_user(&vm_s3, fpidx)!=JE_OK) fail("JeiOS fatal: s3 fp user thread\r\n");
        je_arch_console_write("M3a-s3: hostile-FP user thread created; entering ring 3\r\n");
        for (uint64_t sp=0; !je_sched_has_dying(); ) {
            (void)je_arch_irq_save();
            if (++sp>100000000ull) fail("JeiOS fatal: s3 fp did not fault\r\n");
            __asm__ volatile("sti; hlt":::"memory");
        }
        { uint64_t v,e,r,c,cr2; (void)je_arch_irq_save();
          if (je_sched_death_record(&vm_s3,fpidx,&v,&e,&r,&c,&cr2)!=JE_OK) fail("JeiOS fatal: s3 fp no record\r\n");
          if (v!=6u || c!=0x33u) fail("JeiOS fatal: s3 fp wrong fault\r\n");
          if (je_sched_reap_dead()<0) fail("JeiOS fatal: s3 fp reap\r\n"); }
        je_arch_console_write("M3a-s3: hostile-FP task #UD terminated (fault-local); kernel alive\r\n");
        je_arch_console_write("JeiOS: initialization complete; halted\r\n");
        je_arch_halt();
#endif
        /* --- step-3 sanity: one task reaches CPL3 and makes progress --- */
        uint32_t uidx=vm_s3_build(PROG_HEARTBEAT);
        uint64_t before_disp=je_sched_user_cpl3_dispatches();
        if (je_thread_create_user(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 user thread\r\n");
        je_arch_console_write("M3a-s3: user thread created; entering ring 3 via timer\r\n");
        uint64_t hb0=vm_s3_heartbeat(uidx);
        uint64_t spins=0;
        for (;;) {
            (void)je_arch_irq_save();
            if (je_sched_user_cpl3_dispatches()-before_disp>=JE_THREAD_MAX*2) break;
            if (++spins>100000000ull) fail("JeiOS fatal: s3 no CPL3 progress\r\n");
            __asm__ volatile("sti; hlt":::"memory");
        }
        if (vm_s3_heartbeat(uidx)<=hb0) fail("JeiOS fatal: s3 heartbeat did not advance\r\n");
        je_arch_console_write("M3a-s3: CPL3 reached; survived preemption; heartbeat advanced\r\n");
        (void)je_arch_irq_save();
        if (je_sched_drop_user(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 drop_user\r\n");
        if (je_vm_destroy(&vm_s3, uidx)!=JE_OK) fail("JeiOS fatal: s3 destroy\r\n");

        /* --- step-4 acceptance: fault-local death, peer survives --- */
        /* Build A (faults after a few heartbeats) and B (loops forever). */
        uint32_t A=vm_s3_build(PROG_FAULT);
        uint32_t B=vm_s3_build(PROG_HEARTBEAT);
        if (vm_s3.procs[A].root==vm_s3.procs[B].root) fail("JeiOS fatal: s4 roots not distinct\r\n");
        if (je_thread_create_user(&vm_s3, A)!=JE_OK) fail("JeiOS fatal: s4 A thread\r\n");
        if (je_thread_create_user(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 B thread\r\n");
        je_arch_console_write("M3a-s4: two CPL3 tasks A(fault)+B(loop) created; distinct roots\r\n");
        /* Run until A has died (its slot is DYING). */
        for (uint64_t sp=0; !je_sched_has_dying(); ) {
            (void)je_arch_irq_save();
            if (++sp>100000000ull) fail("JeiOS fatal: s4 A did not fault\r\n");
            __asm__ volatile("sti; hlt":::"memory");
        }
        /* Observe A's death record (irq off): must be a user #PF on A's arena. */
        uint64_t Av,Ae,Ar,Ac,Acr2;
        (void)je_arch_irq_save();
        if (je_sched_death_record(&vm_s3,A,&Av,&Ae,&Ar,&Ac,&Acr2)!=JE_OK) fail("JeiOS fatal: s4 no A record\r\n");
        if (Av!=14u || Ac!=0x33u || Acr2!=(JE_VM_ARENA_BASE+3u*JE_PAGE_SIZE))
            fail("JeiOS fatal: s4 A wrong fault\r\n");
        uint64_t hbB0=vm_s3_heartbeat(B), dB0=je_sched_dispatches_of(&vm_s3,B), p0=je_sched_preemptions();
        je_arch_console_write("M3a-s4: A terminated by user #PF (vector 14, cs=0x33); B still scheduled\r\n");
        /* Prove B progressed AFTER A's death: resume, require B heartbeat +
         * dispatch + preemption all strictly increased from the post-death snapshot. */
        for (uint64_t sp=0;;) {
            (void)je_arch_irq_save();
            if (je_sched_dispatches_of(&vm_s3,B)-dB0>=4 && je_sched_preemptions()>p0) break;
            if (++sp>100000000ull) fail("JeiOS fatal: s4 B no post-death progress\r\n");
            __asm__ volatile("sti; hlt":::"memory");
        }
        if (vm_s3_heartbeat(B)<=hbB0) fail("JeiOS fatal: s4 B heartbeat flat after A death\r\n");
        je_arch_console_write("M3a-s4: B advanced AFTER A's death (heartbeat+dispatch+preempt up)\r\n");
        /* Reap A from bootstrap while B is still live; A's quota returns. */
        (void)je_arch_irq_save();
        if (je_sched_reap_dead()<0) fail("JeiOS fatal: s4 reap A\r\n");
        je_arch_console_write("M3a-s4: dead process A reaped while peer B runs\r\n");

        /* Additional death causes, each terminated fault-locally while B keeps
         * running, reusing the freed second process slot: the D6 int 0x80 gate
         * (unsupported op -> death) and bad-return-state (H1: a user task that
         * corrupts its RSP must be killed at next dispatch, not halt the kernel).
         * Each expects the faulting process DYING, then reaps it. */
        static const user_prog variants[2] = { PROG_INT80, PROG_BADRSP };
        static const char *const vmsg[2] = {
            "M3a-s4: user int 0x80 (D6 gate) terminated offender; B alive\r\n",
            "M3a-s4: bad-RSP (H1) user task terminated, not kernel; B alive\r\n" };
        for (unsigned vi=0; vi<2; ++vi) {
            uint32_t C=vm_s3_build(variants[vi]);
            uint64_t bdisp=je_sched_dispatches_of(&vm_s3,B);
            if (je_thread_create_user(&vm_s3, C)!=JE_OK) fail("JeiOS fatal: s4 variant thread\r\n");
            for (uint64_t sp=0; !je_sched_has_dying(); ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s4 variant did not die\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            uint64_t cv,ce,cr,cc,ccr2; (void)je_arch_irq_save();
            if (je_sched_death_record(&vm_s3,C,&cv,&ce,&cr,&cc,&ccr2)!=JE_OK) fail("JeiOS fatal: s4 variant no record\r\n");
            if (cc!=0x33u) fail("JeiOS fatal: s4 variant not user origin\r\n");
            if (je_sched_reap_dead()<0) fail("JeiOS fatal: s4 variant reap\r\n");
            /* B must still be scheduled (progressed past the pre-variant snapshot). */
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,B)<=bdisp; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s4 B stalled after variant\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            je_arch_console_write(vmsg[vi]);
        }
        /* Tear down B; all step-3/4 pages must be back to the pre-s4 baseline. */
        (void)je_arch_irq_save();
        if (je_sched_drop_user(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 drop B\r\n");
        if (je_vm_destroy(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 destroy B\r\n");
        if (je_kernel_pages.free_count!=s3_free_before) fail("JeiOS fatal: s4 page leak\r\n");
        je_arch_console_write("M3a-s4: peer B torn down; page accounting restored\r\n");
    }

    je_arch_console_write("JeiOS: initialization complete; halted\r\n");
    je_arch_halt();
}

