/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/arch.h>
#include <jeios/kheap.h>
#include <jeios/paging.h>
#include <jeios/sched.h>
#include <jeios/uframe.h>
#include <jeios/vm.h>
#define STACK_PAGES 5u
typedef enum { EMPTY, RUNNING, READY, SLEEPING, ZOMBIE, DYING } state;
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
    /* M3a #4 step 4 / #5: death record for a DYING user thread. `reason`
     * distinguishes a synchronous fault, a clean exit(code), and an invalid
     * return state (read by the bootstrap reaper and the proofs). */
    uint64_t death_vector, death_error, death_rip, death_cs, death_cr2;
    uint64_t death_reason, exit_code;
} thread;
/* Death reasons (M3a #5). FAULT: synchronous user fault. EXIT: clean exit(code).
 * BAD_RETURN: a user frame failed je_uframe_check (invalid return state). */
typedef enum { DEATH_FAULT = 0, DEATH_EXIT = 1, DEATH_BAD_RETURN = 2 } death_reason;
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
    threads[slot]=(thread){READY,f,entry,arg,base,0,0,THREAD_KERNEL,0,0,0,0,0,0,0,0,0,0,0};
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
    threads[slot]=(thread){READY,f,0,0,0,0,0,THREAD_USER,p->root,rsp0_top,vm,index,0,0,0,0,0,0,0};
    return JE_OK;
}
/* M3a #4 step 4: mark a user thread DYING with a death record (never requeued;
 * reclaimed later by je_sched_reap_dead from bootstrap). Kernel threads never
 * take this path. */
static void mark_dying(unsigned slot, death_reason reason, uint64_t code,
                       uint64_t vec, uint64_t err, uint64_t rip, uint64_t cs, uint64_t cr2)
{
    threads[slot].state=DYING;
    threads[slot].frame=0;
    threads[slot].death_reason=(uint64_t)reason; threads[slot].exit_code=code;
    threads[slot].death_vector=vec; threads[slot].death_error=err;
    threads[slot].death_rip=rip; threads[slot].death_cs=cs; threads[slot].death_cr2=cr2;
}

/* Install the chosen next thread's address space and return its frame. For a
 * user next thread, validate+sanitize its saved frame first; if that FAILS the
 * thread is terminated (DYING) and we retry with another candidate — a user
 * task must never be able to halt the kernel via a bad saved frame (fault
 * locality; fixes the pre-step-4 abort-on-bad-return-state hole). Bootstrap
 * (slot 0) is always READY, so a live candidate always exists. Returns the
 * frame to iretq to; never requeues a DYING/ZOMBIE thread. */
static je_trap_frame *select_and_install_next(void)
{
    for (unsigned attempt=0; attempt<JE_THREAD_MAX; ++attempt) {
        unsigned next=current;
        for (unsigned n=1;n<=JE_THREAD_MAX;++n) {
            unsigned candidate=(current+n)%JE_THREAD_MAX;
            if (threads[candidate].state==READY) { next=candidate;break; }
        }
        if (threads[next].state!=READY || !threads[next].frame) abort_scheduler();
        if (threads[next].kind==THREAD_USER) {
            uint64_t sane;
            if (!je_uframe_check(threads[next].frame, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
                /* Bad saved return state -> terminate THIS task, not the kernel. */
                mark_dying(next, DEATH_BAD_RETURN, 0, 0, 0, threads[next].frame->rip,
                           threads[next].frame->cs, 0);
                continue; /* pick another candidate */
            }
            threads[next].frame->rflags = sane;
            je_arch_tss_set_rsp0(threads[next].rsp0_top);
            je_arch_cr3_load(threads[next].root);
        } else {
            je_arch_tss_set_rsp0(0); /* poison: CPL0->CPL0 never consults RSP0 */
            je_arch_cr3_load(je_arch_kernel_root_phys());
        }
        current=next;threads[current].state=RUNNING;++threads[current].dispatches;
        return threads[current].frame;
    }
    abort_scheduler(); /* unreachable: bootstrap is always a valid kernel candidate */
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
    unsigned before=current;
    je_trap_frame *nf=select_and_install_next();
    if (timer && current!=before) ++preemptions;
    return nf;
}

/* M3a #4 step 4: a SYNCHRONOUS user fault (or user int 0x80) terminates ONLY the
 * current user process; the kernel and peers keep running. Called from
 * je_trap_dispatch with interrupts disabled, on the dying task's own per-process
 * RSP0 kstack — so we must NOT free that kstack here (the C frame + the iretq
 * epilogue still run on it). We mark the task DYING and return a LIVE successor's
 * frame; the bootstrap reaper reclaims the dead process later. Never requeues
 * the dying task. */
je_trap_frame *je_sched_fault_terminate(je_trap_frame *f, uint64_t cr2)
{
    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler(); /* only user tasks die here */
    /* Confirm the frame really is the current user task's (defense in depth). */
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
    mark_dying(current, DEATH_FAULT, 0, f->vector, f->error, f->rip, f->cs, cr2);
    return select_and_install_next();
}

/* M3a #5: clean exit(code) — same mechanics as fault_terminate (mark the current
 * user thread DYING on its own kstack, return a live successor; the reaper frees
 * it later) but records a DEATH_EXIT reason and the exit code rather than a
 * fault. Separate from je_sched_fault_terminate (tested step-4 code) by design. */
je_trap_frame *je_sched_exit_current(je_trap_frame *f, uint64_t code)
{
    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
    mark_dying(current, DEATH_EXIT, code, 0, 0, f->rip, f->cs, 0);
    return select_and_install_next();
}

/* M3a #5: return path for a syscall that resumes the CURRENT user task (no
 * context switch) — e.g. abi_query, UNSUPPORTED, INVALID. ADR 0007 D6/M3A.md §4
 * require validating the user frame on EVERY return, so a non-switching syscall
 * return must NOT skip je_uframe_check (a user can corrupt its own RSP/RFLAGS
 * before int 0x80). Validate+sanitize here; on failure terminate (BAD_RETURN)
 * and switch to a live successor instead of iretq'ing a bad frame. The caller
 * has already written the result registers (rax/rdx) into f. */
je_trap_frame *je_sched_return_to_user(je_trap_frame *f)
{
    if (!ready || threads[current].kind!=THREAD_USER) abort_scheduler();
    if (f->cs != JE_UFRAME_USER_CS || f->ss != JE_UFRAME_USER_SS ||
        (uintptr_t)f != (uintptr_t)(threads[current].rsp0_top - sizeof(*f))) abort_scheduler();
    uint64_t sane;
    if (!je_uframe_check(f, JE_VM_ARENA_BASE, JE_VM_ARENA_SIZE, &sane)) {
        mark_dying(current, DEATH_BAD_RETURN, 0, 0, 0, f->rip, f->cs, 0);
        return select_and_install_next();
    }
    f->rflags = sane;
    return f; /* resume the caller; CR3/RSP0 already correct for the current task */
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

/* M3a #4 step 4: reclaim DYING user processes while peers keep running. Runs in
 * bootstrap (slot 0) with interrupts disabled (caller's responsibility). For
 * each DYING user slot: assert it is not current, not the active CR3, and not
 * the active TSS.RSP0 (the task has been iretq'd away from), then restore its
 * kstack guard and destroy its vm process (freeing the held run + tables). The
 * vm record stays READY until destroy (D5's DYING/REAPABLE are modeled in the
 * scheduler slot, not vm.c). Returns the number reaped, or a negative je_status
 * on an invariant violation. Single-CPU/PCID-off invariant: after one iretq away
 * from the dead task, nothing else runs on its kstack. */
int je_sched_reap_dead(void)
{
    if (!ready || current!=0) return (int)JE_ERR_BUSY;
    /* Must run with interrupts disabled: the three liveness guards below and the
     * destroy must be atomic vs a timer dispatch. Enforce it here rather than
     * trusting the caller (defense in depth). */
    uint64_t rflags; __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    if (rflags & (UINT64_C(1)<<9)) abort_scheduler(); /* IF set -> misuse */
    uint64_t active_cr3=je_arch_cr3_read(), active_rsp0=je_arch_tss_get_rsp0();
    int reaped=0;
    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) {
        if (i==current) return (int)JE_ERR_BUSY;
        if (threads[i].root==active_cr3) return (int)JE_ERR_BUSY;    /* still the live address space */
        if (threads[i].rsp0_top==active_rsp0) return (int)JE_ERR_BUSY; /* still the live kernel stack */
        je_vm *vm=threads[i].vm; uint32_t idx=threads[i].vm_index;
        je_status s=je_sched_drop_user(vm, idx); /* restores the kstack guard, clears the slot */
        if (s!=JE_OK) return (int)s;
        s=je_vm_destroy(vm, idx);                /* frees the held kstack run + page tables */
        if (s!=JE_OK) return (int)s;
        ++reaped;
    }
    return reaped;
}

/* M3a #4 step 4 proof accessors: whether a DYING user slot exists and its death
 * record, plus a single-slot dispatch counter so the proof can show a peer
 * progressed AFTER a specific death. Slot identity is by vm process. */
bool je_sched_has_dying(void)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i) if (threads[i].state==DYING) return true;
    return false;
}
je_status je_sched_death_record(je_vm *vm, uint32_t index, uint64_t *vec, uint64_t *err,
                                uint64_t *rip, uint64_t *cs, uint64_t *cr2)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index &&
            threads[i].state==DYING) {
            if (vec) *vec=threads[i].death_vector;
            if (err) *err=threads[i].death_error;
            if (rip) *rip=threads[i].death_rip;
            if (cs)  *cs=threads[i].death_cs;
            if (cr2) *cr2=threads[i].death_cr2;
            return JE_OK;
        }
    return JE_ERR_INVALID;
}
/* M3a #5: the death reason + exit code for a DYING user slot (proof accessor). */
je_status je_sched_death_reason(je_vm *vm, uint32_t index, uint64_t *reason, uint64_t *code)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index &&
            threads[i].state==DYING) {
            if (reason) *reason=threads[i].death_reason;
            if (code)   *code=threads[i].exit_code;
            return JE_OK;
        }
    return JE_ERR_INVALID;
}
uint64_t je_sched_dispatches_of(je_vm *vm, uint32_t index)
{
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].kind==THREAD_USER && threads[i].vm==vm && threads[i].vm_index==index)
            return threads[i].dispatches;
    return 0;
}
uint64_t je_sched_min_dispatches(void)
{
    uint64_t min=UINT64_MAX;
    for (unsigned i=1;i<JE_THREAD_MAX;++i)
        if (threads[i].state!=EMPTY && threads[i].dispatches<min) min=threads[i].dispatches;
    return min;
}

