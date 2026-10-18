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
#include <jeios/abi.h>
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
extern unsigned char je_user_exit_start[], je_user_exit_end[];
extern unsigned char je_user_syscall_start[], je_user_syscall_end[];
extern unsigned char je_user_unknownop_start[], je_user_unknownop_end[];
extern unsigned char je_user_badret_start[], je_user_badret_end[];
extern unsigned char je_user_copyprobe_start[], je_user_copyprobe_end[];
extern unsigned char je_user_handle_start[], je_user_handle_end[];
#ifdef JE_DEV_COPY_INJECT
extern unsigned char je_user_inject_start[], je_user_inject_end[];
#endif
extern unsigned char je_user_badrsp_start[], je_user_badrsp_end[];
static je_vm vm_s3;
static unsigned char vm_blob_s3[1024];
typedef enum { PROG_HEARTBEAT=0, PROG_FP=1, PROG_FAULT=2, PROG_EXIT=3,
               PROG_SYSCALL=4, PROG_UNKNOWNOP=5, PROG_BADRSP=6, PROG_BADRET=7,
               PROG_COPYPROBE=8, PROG_INJECT=9, PROG_HANDLE=10 } user_prog;
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
    case PROG_FP:        start=je_user_fp_start;        end=je_user_fp_end;        break;
    case PROG_FAULT:     start=je_user_fault_start;     end=je_user_fault_end;     break;
    case PROG_EXIT:      start=je_user_exit_start;      end=je_user_exit_end;      break;
    case PROG_SYSCALL:   start=je_user_syscall_start;   end=je_user_syscall_end;   break;
    case PROG_UNKNOWNOP: start=je_user_unknownop_start; end=je_user_unknownop_end; break;
    case PROG_BADRSP:    start=je_user_badrsp_start;    end=je_user_badrsp_end;    break;
    case PROG_BADRET:    start=je_user_badret_start;    end=je_user_badret_end;    break;
    case PROG_COPYPROBE: start=je_user_copyprobe_start; end=je_user_copyprobe_end; break;
    case PROG_HANDLE:    start=je_user_handle_start;    end=je_user_handle_end;    break;
#ifdef JE_DEV_COPY_INJECT
    case PROG_INJECT:    start=je_user_inject_start;    end=je_user_inject_end;    break;
#endif
    default:             start=je_user_demo_start;      end=je_user_demo_end;      break;
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
/* Read a u64 at arena offset `off` under process idx's root (CPL0). Used to read
 * back syscall results the user stub stored in its data page. */
static uint64_t vm_s3_scratch(uint32_t idx, uint64_t off)
{
    uint64_t flags=je_arch_irq_save();
    uint64_t kroot=je_arch_kernel_root_phys();
    je_arch_cr3_load(vm_s3.procs[idx].root);
    volatile uint64_t *p=(volatile uint64_t *)(uintptr_t)(JE_VM_ARENA_BASE+off);
    uint64_t v=*p;
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
         * Each expects the faulting process DYING with the right reason, then
         * reaps it, all while peer B keeps running. */
        static const user_prog variants[2] = { PROG_BADRSP, PROG_FP };
        static const uint64_t vreason[2] = { JE_DEATH_BAD_RETURN, JE_DEATH_FAULT };
        static const char *const vmsg[2] = {
            "M3a-s4: bad-RSP (H1) user task terminated (BAD_RETURN), not kernel; B alive\r\n",
            "M3a-s4: hostile-FP user task terminated (#UD FAULT); B alive\r\n" };
        for (unsigned vi=0; vi<2; ++vi) {
            uint32_t C=vm_s3_build(variants[vi]);
            uint64_t bdisp=je_sched_dispatches_of(&vm_s3,B);
            if (je_thread_create_user(&vm_s3, C)!=JE_OK) fail("JeiOS fatal: s4 variant thread\r\n");
            for (uint64_t sp=0; !je_sched_has_dying(); ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s4 variant did not die\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            uint64_t creason,ccode; (void)je_arch_irq_save();
            if (je_sched_death_reason(&vm_s3,C,&creason,&ccode)!=JE_OK) fail("JeiOS fatal: s4 variant no record\r\n");
            if (creason!=vreason[vi]) fail("JeiOS fatal: s4 variant wrong death reason\r\n");
            if (je_sched_reap_dead()<0) fail("JeiOS fatal: s4 variant reap\r\n");
            /* B must still be scheduled (progressed past the pre-variant snapshot). */
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,B)<=bdisp; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s4 B stalled after variant\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            je_arch_console_write(vmsg[vi]);
        }

        /* --- step-(a) syscall proof, with B still the live peer --- */
        /* exit(42) via the int 0x80 gate: a CLEAN controlled termination
         * (DEATH_EXIT, code 42), distinct from a fault. */
        {
            uint32_t E=vm_s3_build(PROG_EXIT);
            if (je_thread_create_user(&vm_s3, E)!=JE_OK) fail("JeiOS fatal: s5a exit thread\r\n");
            for (uint64_t sp=0; !je_sched_has_dying(); ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5a exit did not exit\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            uint64_t er,ec; (void)je_arch_irq_save();
            if (je_sched_death_reason(&vm_s3,E,&er,&ec)!=JE_OK) fail("JeiOS fatal: s5a exit no record\r\n");
            if (er!=JE_DEATH_EXIT || ec!=42u) fail("JeiOS fatal: s5a exit wrong reason/code\r\n");
            if (je_sched_reap_dead()<0) fail("JeiOS fatal: s5a exit reap\r\n");
            je_arch_console_write("M3a-s5a: user exit(42) -> clean EXIT (code 42) via int 0x80; B alive\r\n");
        }
        /* abi_query + yield: a returning-syscall task. It records abi_query's
         * status/version and a yield status into its data page; run it a while,
         * then read those back and confirm it keeps looping (syscalls returned). */
        {
            uint32_t S=vm_s3_build(PROG_SYSCALL);
            uint64_t sdisp0;
            if (je_thread_create_user(&vm_s3, S)!=JE_OK) fail("JeiOS fatal: s5a syscall thread\r\n");
            sdisp0=je_sched_dispatches_of(&vm_s3,S);
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,S)-sdisp0<JE_THREAD_MAX*2; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5a syscall no progress\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            uint64_t qstatus=vm_s3_scratch(S,JE_PAGE_SIZE+8);   /* abi_query status */
            uint64_t qver=vm_s3_scratch(S,JE_PAGE_SIZE+16);     /* abi_query version */
            uint64_t ystatus=vm_s3_scratch(S,JE_PAGE_SIZE+24);  /* yield status */
            if (qstatus!=JE_ABI_OK || qver!=JE_ABI_VERSION) fail("JeiOS fatal: s5a abi_query result\r\n");
            if (ystatus!=JE_ABI_OK) fail("JeiOS fatal: s5a yield result\r\n");
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, S)!=JE_OK) fail("JeiOS fatal: s5a syscall drop\r\n");
            if (je_vm_destroy(&vm_s3, S)!=JE_OK) fail("JeiOS fatal: s5a syscall destroy\r\n");
            je_arch_console_write("M3a-s5a: abi_query returned OK+version; yield returned OK; B alive\r\n");
        }
        /* Unknown op must NOT terminate: the task records UNSUPPORTED and keeps
         * running. */
        {
            uint32_t U=vm_s3_build(PROG_UNKNOWNOP);
            uint64_t udisp0;
            if (je_thread_create_user(&vm_s3, U)!=JE_OK) fail("JeiOS fatal: s5a unknown thread\r\n");
            udisp0=je_sched_dispatches_of(&vm_s3,U);
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,U)-udisp0<JE_THREAD_MAX*2; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5a unknown no progress\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            if (je_sched_has_dying()) fail("JeiOS fatal: s5a unknown op terminated the task\r\n");
            if (vm_s3_scratch(U,JE_PAGE_SIZE+8)!=JE_ABI_UNSUPPORTED) fail("JeiOS fatal: s5a unknown status\r\n");
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, U)!=JE_OK) fail("JeiOS fatal: s5a unknown drop\r\n");
            if (je_vm_destroy(&vm_s3, U)!=JE_OK) fail("JeiOS fatal: s5a unknown destroy\r\n");
            je_arch_console_write("M3a-s5a: unknown op -> UNSUPPORTED, task survived; B alive\r\n");
        }
        /* Bad-return THROUGH the syscall return path: corrupt RSP then make a
         * returning syscall; je_sched_return_to_user must validate and terminate
         * (BAD_RETURN) rather than iretq the bad frame. */
        {
            uint32_t R=vm_s3_build(PROG_BADRET);
            if (je_thread_create_user(&vm_s3, R)!=JE_OK) fail("JeiOS fatal: s5a badret thread\r\n");
            for (uint64_t sp=0; !je_sched_has_dying(); ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5a badret did not die\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            uint64_t rr,rc; (void)je_arch_irq_save();
            if (je_sched_death_reason(&vm_s3,R,&rr,&rc)!=JE_OK) fail("JeiOS fatal: s5a badret no record\r\n");
            if (rr!=JE_DEATH_BAD_RETURN) fail("JeiOS fatal: s5a badret wrong reason\r\n");
            if (je_sched_reap_dead()<0) fail("JeiOS fatal: s5a badret reap\r\n");
            je_arch_console_write("M3a-s5a: bad frame on syscall return -> BAD_RETURN death (validated); B alive\r\n");
        }
        /* --- step-(b): copy_probe battery + fixup recovery --- */
        /* The user program seeds a source pattern, runs copy_probe across valid
         * and adversarial cases, and records each returned status; we read them
         * back and also verify the valid copy's destination content. */
        {
            uint32_t C=vm_s3_build(PROG_COPYPROBE);
            /* Seed the in-arena source (0x1100) with a known pattern at CPL0. */
            {
                uint64_t flags=je_arch_irq_save();
                uint64_t kroot=je_arch_kernel_root_phys();
                je_arch_cr3_load(vm_s3.procs[C].root);
                volatile unsigned char *srcp=(volatile unsigned char *)(uintptr_t)(JE_VM_ARENA_BASE+0x1100);
                for (unsigned i=0;i<16;++i) srcp[i]=(unsigned char)(0xA0u+i);
                je_arch_cr3_load(kroot);
                je_arch_irq_restore(flags);
            }
            uint64_t cdisp0;
            if (je_thread_create_user(&vm_s3, C)!=JE_OK) fail("JeiOS fatal: s5b copyprobe thread\r\n");
            cdisp0=je_sched_dispatches_of(&vm_s3,C);
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,C)-cdisp0<JE_THREAD_MAX*3; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5b copyprobe no progress\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            /* Read back the 5 recorded statuses. */
            if (vm_s3_scratch(C,0x1020)!=JE_ABI_OK)      fail("JeiOS fatal: s5b valid copy not OK\r\n");
            if (vm_s3_scratch(C,0x1028)!=JE_ABI_OK)      fail("JeiOS fatal: s5b zero-len not OK\r\n");
            if (vm_s3_scratch(C,0x1030)!=JE_ABI_INVALID) fail("JeiOS fatal: s5b null src not INVALID\r\n");
            if (vm_s3_scratch(C,0x1038)!=JE_ABI_INVALID) fail("JeiOS fatal: s5b kernel src not INVALID\r\n");
            if (vm_s3_scratch(C,0x1040)!=JE_ABI_INVALID) fail("JeiOS fatal: s5b len>256 not INVALID\r\n");
            /* Verify the valid copy actually moved the bytes (dst at 0x1180). */
            {
                uint64_t flags=je_arch_irq_save();
                uint64_t kroot=je_arch_kernel_root_phys();
                je_arch_cr3_load(vm_s3.procs[C].root);
                volatile unsigned char *dstp=(volatile unsigned char *)(uintptr_t)(JE_VM_ARENA_BASE+0x1180);
                int ok=1; for (unsigned i=0;i<16;++i) if (dstp[i]!=(unsigned char)(0xA0u+i)) ok=0;
                je_arch_cr3_load(kroot);
                je_arch_irq_restore(flags);
                if (!ok) fail("JeiOS fatal: s5b valid copy content wrong\r\n");
            }
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, C)!=JE_OK) fail("JeiOS fatal: s5b copyprobe drop\r\n");
            if (je_vm_destroy(&vm_s3, C)!=JE_OK) fail("JeiOS fatal: s5b copyprobe destroy\r\n");
            je_arch_console_write("M3a-s5b: copy_probe valid+zero OK, null/kernel/len>256 INVALID, bytes moved; B alive\r\n");
        }
#ifdef JE_DEV_COPY_INJECT
        /* DEV-ONLY fixup-recovery proof: a user task invokes the inject op, which
         * raw-copies an unmapped arena page from its syscall context; the fixup
         * must RECOVER the not-present copy-site #PF (RDX=1) and the kernel must
         * stay alive. Compile-guarded; absent from the default build. */
        {
            uint32_t I=vm_s3_build(PROG_INJECT);
            uint64_t idisp0;
            if (je_thread_create_user(&vm_s3, I)!=JE_OK) fail("JeiOS fatal: s5b inject thread\r\n");
            idisp0=je_sched_dispatches_of(&vm_s3,I);
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,I)-idisp0<JE_THREAD_MAX*2; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5b inject no progress\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            if (vm_s3_scratch(I,0x1020)!=1u) fail("JeiOS fatal: s5b fixup did not recover\r\n");
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, I)!=JE_OK) fail("JeiOS fatal: s5b inject drop\r\n");
            if (je_vm_destroy(&vm_s3, I)!=JE_OK) fail("JeiOS fatal: s5b inject destroy\r\n");
            je_arch_console_write("M3a-s5b: exception-fixup recovered load+store copy-site #PF (dev inject); kernel alive\r\n");
        }
#endif

        /* --- M3b second PR: per-process handle table + handle syscalls --- */
        /* Process H mints a SELF handle, queries it (type==SELF), closes it, and
         * probes the stale/forged/bound/wide/quota/reuse cases, recording each
         * status for us to assert. Proves the handle table is wired to a live
         * process and the JE_HANDLE_* -> JE_ABI_* mapping is distinct (not
         * collapsed to INVALID). (This exercises ONE process's table; it does not
         * by itself prove tables are PER-process — that distinction is covered by
         * construction + the hosted vm/table tests, not claimed from this run.) */
        {
            uint32_t H=vm_s3_build(PROG_HANDLE);
            uint64_t hdisp0;
            if (je_thread_create_user(&vm_s3, H)!=JE_OK) fail("JeiOS fatal: s5c handle thread\r\n");
            hdisp0=je_sched_dispatches_of(&vm_s3,H);
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,H)-hdisp0<JE_THREAD_MAX*3; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s5c handle no progress\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            /* self#1: OK + a nonzero handle. */
            if (vm_s3_scratch(H,0x1020)!=JE_ABI_OK)        fail("JeiOS fatal: s5c self not OK\r\n");
            if (vm_s3_scratch(H,0x1028)==0u)               fail("JeiOS fatal: s5c self handle zero\r\n");
            /* query(live): OK + type == SELF. */
            if (vm_s3_scratch(H,0x1030)!=JE_ABI_OK)        fail("JeiOS fatal: s5c query(live) not OK\r\n");
            if (vm_s3_scratch(H,0x1038)!=JE_ABI_OBJ_SELF)  fail("JeiOS fatal: s5c query(live) wrong type\r\n");
            /* rights(live) op8: OK + mask == INSPECT (0x1). A freshly minted SELF
             * holds INSPECT; the DENIED case is proven below via handle_dup
             * dropping the right (ring-3-observable, not hosted-only). */
            if (vm_s3_scratch(H,0x10a0)!=JE_ABI_OK)        fail("JeiOS fatal: s5c rights not OK\r\n");
            if (vm_s3_scratch(H,0x10a8)!=JE_ABI_RIGHT_INSPECT) fail("JeiOS fatal: s5c rights mask != INSPECT\r\n");
            /* attenuation (op 9): full-rights alias OK + a DIFFERENT handle. */
            if (vm_s3_scratch(H,0x10b0)!=JE_ABI_OK)        fail("JeiOS fatal: s5c dup(full) not OK\r\n");
            if (vm_s3_scratch(H,0x10b8)==vm_s3_scratch(H,0x1028)) fail("JeiOS fatal: s5c dup handle == source\r\n");
            /* dropping INSPECT: OK, and a nonzero derived handle. */
            if (vm_s3_scratch(H,0x10c0)!=JE_ABI_OK)        fail("JeiOS fatal: s5c dup(weak) not OK\r\n");
            if (vm_s3_scratch(H,0x10c8)==0u)               fail("JeiOS fatal: s5c dup(weak) handle zero\r\n");
            /* THE ENFORCEMENT PROOF: handle_rights on the rights-less derived
             * handle is DENIED, from ring 3 — attenuation made DENIED real. */
            if (vm_s3_scratch(H,0x10d0)!=JE_ABI_H_DENIED)  fail("JeiOS fatal: s5c rights(weak) not DENIED\r\n");
            /* superset request (weak lacks INSPECT) -> DENIED, never escalates. */
            if (vm_s3_scratch(H,0x10d8)!=JE_ABI_H_DENIED)  fail("JeiOS fatal: s5c dup superset not DENIED\r\n");
            /* still-unknown rights bit (0x80000000) -> INVALID (malformed), not
             * DENIED. 0x2 is now the KNOWN right SEND (ADR 0009 D4a), so the
             * unknown-bit contract moved to bit 31; see user_demo.S +0xe0. */
            if (vm_s3_scratch(H,0x10e0)!=JE_ABI_INVALID)   fail("JeiOS fatal: s5c dup unknown-right not INVALID\r\n");
            /* a rights-0 handle still closes (no slot leak). */
            if (vm_s3_scratch(H,0x10e8)!=JE_ABI_OK)        fail("JeiOS fatal: s5c close(weak) not OK\r\n");
            if (vm_s3_scratch(H,0x10f0)!=JE_ABI_OK)        fail("JeiOS fatal: s5c close(dup full) not OK\r\n");
            /* close(live): OK. */
            if (vm_s3_scratch(H,0x1040)!=JE_ABI_OK)        fail("JeiOS fatal: s5c close(live) not OK\r\n");
            /* query/close of the now-closed handle: STALE (distinct status). */
            if (vm_s3_scratch(H,0x1048)!=JE_ABI_H_STALE)   fail("JeiOS fatal: s5c query(closed) not STALE\r\n");
            if (vm_s3_scratch(H,0x1050)!=JE_ABI_H_STALE)   fail("JeiOS fatal: s5c close(closed) not STALE\r\n");
            /* handle 0 and index==capacity: BADINDEX. */
            if (vm_s3_scratch(H,0x1058)!=JE_ABI_H_BADINDEX) fail("JeiOS fatal: s5c close(0) not BADINDEX\r\n");
            if (vm_s3_scratch(H,0x1060)!=JE_ABI_H_BADINDEX) fail("JeiOS fatal: s5c bound-index not BADINDEX\r\n");
            /* slot reuse: self#2 took slot 0 at a new generation. */
            if (vm_s3_scratch(H,0x1088)!=JE_ABI_OK)        fail("JeiOS fatal: s5c self#2 not OK\r\n");
            if (vm_s3_scratch(H,0x1078)==0u)               fail("JeiOS fatal: s5c self#2 handle zero\r\n");
            if (vm_s3_scratch(H,0x1078)==vm_s3_scratch(H,0x1028))
                fail("JeiOS fatal: s5c reused handle equals the stale one\r\n");
            /* RE-STALE: the OLD self#1 handle, presented AFTER slot 0 was reused,
             * is STALE — the real proof that reuse invalidates old names. */
            if (vm_s3_scratch(H,0x1090)!=JE_ABI_H_STALE)   fail("JeiOS fatal: s5c old handle not re-staled after reuse\r\n");
            /* wrong generation on a live slot: STALE. */
            if (vm_s3_scratch(H,0x1068)!=JE_ABI_H_STALE)   fail("JeiOS fatal: s5c wrong-gen not STALE\r\n");
            /* wide handle (bits 63:32 set): INVALID at decode, before narrowing. */
            if (vm_s3_scratch(H,0x1070)!=JE_ABI_INVALID)   fail("JeiOS fatal: s5c wide-handle not INVALID\r\n");
            /* quota (distinct from NOSLOTS): the mint loop ends at QUOTA after
             * EXACTLY 14 more successful mints (quota 15 minus the live self#2),
             * a measured fact — not merely "ended at QUOTA". */
            if (vm_s3_scratch(H,0x1080)!=JE_ABI_H_QUOTA)   fail("JeiOS fatal: s5c quota not QUOTA\r\n");
            if (vm_s3_scratch(H,0x1098)!=14u)              fail("JeiOS fatal: s5c quota count != 14\r\n");
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, H)!=JE_OK) fail("JeiOS fatal: s5c handle drop\r\n");
            if (je_vm_destroy(&vm_s3, H)!=JE_OK) fail("JeiOS fatal: s5c handle destroy\r\n");
            je_arch_console_write("M3b-s1: handle self/query/rights/dup/close OK; attenuation drops INSPECT -> handle_rights DENIED (enforcement real); superset DENIED; unknown-right INVALID; reuse re-stales; QUOTA at CAP-1; B alive\r\n");
        }

        /* Tear down B; all step-3/4/5a pages must be back to the pre-s4 baseline. */
        (void)je_arch_irq_save();
        if (je_sched_drop_user(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 drop B\r\n");
        if (je_vm_destroy(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 destroy B\r\n");
        if (je_kernel_pages.free_count!=s3_free_before) fail("JeiOS fatal: s4 page leak\r\n");
        je_arch_console_write("M3a-s4: peer B torn down; page accounting restored\r\n");
    }

    je_arch_console_write("JeiOS: initialization complete; halted\r\n");
    je_arch_halt();
}

