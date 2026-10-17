/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/arch.h>
#include <jeios/kheap.h>
#include <jeios/paging.h>
#include <jeios/sched.h>
#include <jeios/uframe.h>
#include <jeios/vm.h>
#define STACK_PAGES 5u
typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE } state;
typedef enum { THREAD_KERNEL = 0, THREAD_USER = 1 } thread_kind;
typedef struct {
    state state;
    je_trap_frame *frame;
    void (*entry)(void *);
    void *argument;
    uint64_t stack, wake, dispatches;
    /* M3a #4 step 3: user-thread fields. For THREAD_KERNEL these are 0/unused.
     * For THREAD_USER: root is the process CR3; rsp0_top is the per-process
     * kernel-stack top loaded into TSS.RSP0 on dispatch; vm/vm_index name the
     * owning process so teardown (not reap) frees its held kstack. */
    thread_kind kind;
    uint64_t root, rsp0_top;
    je_vm *vm;
    uint32_t vm_index;
} thread;
static thread threads[JE_THREAD_MAX];
static unsigned current;
static bool ready;
static uint64_t preemptions;
static uint64_t user_cpl3_dispatches; /* M3a #4 step 3: times a THREAD_USER frame (cs&3==3) was seen */
volatile uint64_t je_timer_ticks;
static _Noreturn void abort_scheduler(void)
{
    (void)je_arch_irq_save();
    je_arch_console_write("JeiOS fatal: scheduler invariant\r\n");je_arch_halt();
}
static _Noreturn void trampoline(void)
{
    threads[current].entry(threads[current].argument);
    je_thread_exit();
}
void je_sched_init(void)
{
    if (ready) abort_scheduler();
    threads[0].state=RUNNING;current=0;ready=true;
}
je_status je_thread_create(void (*entry)(void *),void *arg)
{
    if (!ready || !entry) return JE_ERR_INVALID;
    unsigned slot=1;
    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
    uint64_t base;
    je_status s=je_pages_alloc(&je_kernel_pages,STACK_PAGES,&base);
    if (s!=JE_OK) return s;
    unsigned char *memory=(void *)(uintptr_t)base;
    for (size_t i=0;i<STACK_PAGES*4096;++i) memory[i]=0;
    s=je_arch_page_guard((uintptr_t)base);
    if (s!=JE_OK) {
        je_status cleanup=je_pages_free(&je_kernel_pages,base,STACK_PAGES);
        return cleanup==JE_OK ? s : cleanup;
    }
    uintptr_t top=(uintptr_t)base+STACK_PAGES*4096;
    uintptr_t rsp=top-8; /* SysV C entry: RSP % 16 == 8, with fake return address. */
    *(uintptr_t *)rsp=(uintptr_t)je_thread_exit;
    je_trap_frame *f=(void *)(rsp-sizeof(*f));
    *f=(je_trap_frame){.rip=(uintptr_t)trampoline,.cs=8,.rflags=0x202,.rsp=rsp,.ss=16};
    threads[slot]=(thread){READY,f,entry,arg,base,0,0,THREAD_KERNEL,0,0,0,0};
    return JE_OK;
}

/* M3a #4 step 3: create a user thread for a READY vm process. Synthesizes the
 * initial ring-3 trap frame on the process's held kernel stack, so the first
 * scheduler dispatch iretqs into CPL3 at the validated entry. The kstack is the
 * process's concrete 5-page run (held by #3); its top is TSS.RSP0. Requires NX
 * (ADR 0007 D2). Does NOT allocate from je_kernel_pages — the kstack belongs to
 * the vm process and is freed by je_vm_destroy, never by je_sched_reap. */
je_status je_thread_create_user(je_vm *vm, uint32_t index)
{
    if (!ready || !vm || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    if (!je_arch_has_nx()) return JE_ERR_UNSUPPORTED; /* D2 */
    je_vm_process *p = &vm->procs[index];
    if (p->state != JE_VM_READY || p->held_kstack != JE_VM_KSTACK_PAGES || !p->root)
        return JE_ERR_INVALID;
    unsigned slot=1;
    while (slot<JE_THREAD_MAX && threads[slot].state!=EMPTY) ++slot;
    if (slot==JE_THREAD_MAX) return JE_ERR_NOMEM;
    uint64_t kbase = p->kstack_physical;              /* identity VA (< 1 GiB) */
    /* Guard the first page of the held kstack (ADR D4 assigns the guard to #4);
     * RSP0 is the top of the remaining usable pages. */
    je_status g = je_arch_page_guard((uintptr_t)kbase);
    if (g != JE_OK) return g;
    uint64_t rsp0_top = kbase + (uint64_t)STACK_PAGES * 4096u;
    /* Initial user frame at the very top of the kstack, GPRs zero (no kernel
     * register leak into ring 3). user RSP starts at arena top (full-descending,
     * validator allows top inclusive); RIP at validated entry. */
    je_trap_frame *f = (void *)(uintptr_t)(rsp0_top - sizeof(*f));
    *f = (je_trap_frame){0};
    f->cs = JE_UFRAME_USER_CS; f->ss = JE_UFRAME_USER_SS;
    f->rip = JE_VM_ARENA_BASE + p->entry;
    f->rsp = JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE;
    f->rflags = 0x202u; /* IF=1, bit1; validator re-sanitizes on every dispatch */
    threads[slot]=(thread){READY,f,0,0,0,0,0,THREAD_USER,p->root,rsp0_top,vm,index};
    return JE_OK;
}
je_trap_frame *je_sched_interrupt(je_trap_frame *f,bool timer)
{
    if (!ready || threads[current].state==EMPTY) abort_scheduler();
    /* Validate the INCOMING frame by the current thread's kind, not by trusting
     * the frame. A kernel thread must present a CPL0 frame (cs==8); a user
     * thread must present its exact user frame landing at rsp0_top-frame (where
     * the CPU writes it on a CPL3->CPL0 trap). Any mismatch is corruption. */
    if (threads[current].kind==THREAD_KERNEL) {
        if ((f->cs&3) != 0 || f->cs != 8) abort_scheduler();
    } else {
        if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS) abort_scheduler();
        if ((uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
        ++user_cpl3_dispatches; /* observed a genuine CPL3 (cs&3==3) frame return from a user thread */
    }
    threads[current].frame=f;
    if (timer) {
        if (je_timer_ticks==UINT64_MAX) abort_scheduler();
        ++je_timer_ticks;
    }
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].state==SLEEPING && threads[i].wake<=je_timer_ticks) threads[i].state=READY;
    if (threads[current].state==RUNNING) threads[current].state=READY;
    unsigned next=current;
    for (unsigned n=1;n<=JE_THREAD_MAX;++n) {
        unsigned candidate=(current+n)%JE_THREAD_MAX;
        if (threads[candidate].state==READY) { next=candidate;break; }
    }
    if (threads[next].state!=READY || !threads[next].frame) abort_scheduler();
    /* For a user NEXT thread: validate + sanitize its saved frame before we
     * commit any state, then set TSS.RSP0 and load its CR3. For a kernel NEXT
     * thread: restore the kernel root and poison RSP0 (fail-closed). CR3/RSP0
     * are installed while still executing on the shared kernel subtree, so the
     * subsequent GPR pops + iretq remain mapped (step 2 topology). */
    if (threads[next].kind==THREAD_USER) {
        uint64_t sane;
        if (!je_uframe_check(threads[next].frame, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane))
            abort_scheduler();
        threads[next].frame->rflags = sane;
        je_arch_tss_set_rsp0(threads[next].rsp0_top);
        je_arch_cr3_load(threads[next].root);
    } else {
        je_arch_tss_set_rsp0(0); /* poison: CPL0->CPL0 never consults RSP0 */
        je_arch_cr3_load(je_arch_kernel_root_phys());
    }
    if (timer && next!=current) ++preemptions;
    current=next;threads[current].state=RUNNING;++threads[current].dispatches;
    return threads[current].frame;
}
je_status je_thread_sleep(uint64_t duration)
{
    uint64_t flags=je_arch_irq_save();
    if (!ready || current==0 || duration>UINT64_MAX-je_timer_ticks) {
        je_arch_irq_restore(flags);return current==0 ? JE_ERR_DENIED : JE_ERR_INVALID;
    }
    if (duration) {
        threads[current].wake=je_timer_ticks+duration;
        threads[current].state=SLEEPING;
    }
    __asm__ volatile("int $0x81":::"memory");
    je_arch_irq_restore(flags);return JE_OK;
}
_Noreturn void je_thread_exit(void)
{
    (void)je_arch_irq_save();
    if (!ready || current==0) abort_scheduler();
    threads[current].state=ZOMBIE;
    __asm__ volatile("int $0x81":::"memory");
    abort_scheduler();
}
bool je_sched_finished(void)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].state!=EMPTY && threads[i].state!=ZOMBIE) return false;
    return true;
}
je_status je_sched_reap(void)
{
    if (current!=0 || !je_sched_finished()) return JE_ERR_BUSY;
    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==ZOMBIE) {
        /* User threads own no je_kernel_pages stack: their kernel stack is the
         * vm process's held run, freed by je_vm_destroy. Reaping it here would
         * double-free (M3a #4 step 3). Clear the slot only. */
        if (threads[i].kind==THREAD_USER) { threads[i]=(thread){0}; continue; }
        je_status s=je_arch_page_restore((uintptr_t)threads[i].stack);
        if (s!=JE_OK) return s;
        s=je_pages_free(&je_kernel_pages,threads[i].stack,STACK_PAGES);
        if (s!=JE_OK) return s;
        threads[i]=(thread){0};
    }
    return JE_OK;
}
uint64_t je_sched_ticks(void) { return je_timer_ticks; }
uint64_t je_sched_preemptions(void) { return preemptions; }
uint64_t je_sched_user_cpl3_dispatches(void) { return user_cpl3_dispatches; }
/* M3a #4 step 3: remove a user thread (by its vm process) from scheduling
 * before its process is destroyed, so the scheduler never dispatches a torn-down
 * CR3. Caller holds interrupts disabled and must not be the user thread. */
je_status je_sched_drop_user(je_vm *vm, uint32_t index)
{
    if (!ready || !vm) return JE_ERR_INVALID;
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index) {
            if (i==current) return JE_ERR_BUSY;
            if (threads[i].rsp0_top) { /* restore the kstack guard before the process frees the run */
                uint64_t kbase=threads[i].rsp0_top-(uint64_t)STACK_PAGES*4096u;
                je_status s=je_arch_page_restore((uintptr_t)kbase);
                if (s!=JE_OK) return s;
            }
            threads[i]=(thread){0};
            return JE_OK;
        }
    return JE_ERR_INVALID;
}
uint64_t je_sched_min_dispatches(void)
{
    uint64_t min=UINT64_MAX;
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].state!=EMPTY && threads[i].dispatches<min) min=threads[i].dispatches;
    return min;
}

