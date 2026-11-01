/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <jeios/kheap.h>
#include <jeios/sched.h>
#include <jeios/arch.h>
#include <jeios/trap.h>
#include <jeios/boot.h>
#include <jeios/paging.h>
#include <jeios/vm.h>
#include <jeios/abi.h>
#include <jeios/endpoint.h>
#include <jeios/endpoint_kern.h>
#include <jeios/handle.h>
#include <jeios/proc.h>
#include <jeios/image.h>
#include <jeios/spawn.h>
#include <jeios/svc.h>
#include <jeios/broker_test.h>

extern void je_sched_set_proc_pool(je_proc_pool *pool);
extern je_status je_sched_register_process(je_vm *vm, uint32_t index, je_proc_id id);
extern je_status je_sched_unregister_process(je_vm *vm, uint32_t index);
extern bool je_sched_has_process(je_vm *vm, uint32_t index);
extern unsigned je_sched_user_thread_count(void);
extern void je_kernel_idle_step(void);
extern void je_endpoint_kern_set_proc_pool(je_proc_pool *pool);
extern je_status je_process_handle_mint(je_vm *vm, uint32_t index, je_proc_id id,
                                        uint32_t rights, je_handle *out,
                                        je_handle_status *out_alloc_status);
extern const char je_c_hello_blob_sha256[];
static je_boot_context boot;
static _Noreturn void fail(const char *message)
{
    (void)je_arch_irq_save();
    je_arch_console_write(message); je_arch_halt();
}
static atomic_uint completed;
static atomic_uint capacity_completed;
static je_proc_pool process_pool;
static je_proc_id vm_s3_proc_ids[JE_VM_MAX_PROCESSES];
/*
 * M4f identity evidence is kernel-owned and scoped to one seeded supervisor
 * run. The active entries are cleared when the reaper destroys their VM slot;
 * terminal facts are retained in fixed per-image counters so corroboration can
 * still distinguish a reaped predecessor from its replacement.
 */
#define M4F_IMAGE_COUNT 9u
typedef struct {
    bool active;
    bool terminal_seen;
    je_proc_id id;
    uint32_t image_id;
    uint32_t terminal_reason;
    uint64_t terminal_code;
    uint32_t terminal_ep_charge;
    bool terminal_endpoint_shape;
} m4f_child_identity;
static bool m4f_ledger_enabled;
static bool m4f_unexpected_image;
static m4f_child_identity m4f_children[JE_PROC_REC_MAX];
static bool m4f_run_proc_slots[JE_PROC_REC_MAX];
static uint32_t m4f_spawn_count[M4F_IMAGE_COUNT];
static uint32_t m4f_terminal_count[M4F_IMAGE_COUNT];
static uint32_t m4f_death_count[M4F_IMAGE_COUNT][JE_PROC_REASON_MAX + 1u];
static uint32_t m4f_spoofer_shape_count;
static uint64_t m4f_spoofer_code;
static uint32_t m4f_spoofer_ep_charge;
static uint32_t m4f_progress_serial;
static uint32_t vm_s3_live_count(void);

typedef struct {
    bool active;
    je_proc_id receiver;
    uint32_t endpoint_generation;
    uint16_t endpoint_index;
    uint8_t endpoint_side;
} m4g_delivery;
static m4g_delivery m4g_deliveries[JE_BROKER_MAX];
static uint32_t m4g_delivery_used;
static uint32_t m4g_delivery_count[JE_PROC_REC_MAX];
static je_proc_id m4g_delivery_identity[JE_PROC_REC_MAX];
static bool m4g_delivery_bad;
#if JE_TEST_MODE == 13
typedef struct {
    bool captured;
    je_proc_id client;
    uint32_t brokered_count;
    uint32_t service_peer_count;
} m4g_client_audit;
static m4g_client_audit m4g_client_audits[JE_PROC_REC_MAX];
#endif
static bool m4f_id_equal(je_proc_id left, je_proc_id right);
#if JE_TEST_MODE == 13
static bool m4g_table_holds_object(uint32_t vm_index, const void *object);
#endif

static void m4g_delivery_reset(void)
{
    for (uint32_t i = 0u; i < JE_BROKER_MAX; ++i)
        m4g_deliveries[i] = (m4g_delivery){0};
    m4g_delivery_used = 0u;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        m4g_delivery_count[i] = 0u;
        m4g_delivery_identity[i] = JE_PROC_ID_NONE;
#if JE_TEST_MODE == 13
        m4g_client_audits[i] = (m4g_client_audit){0};
#endif
    }
    m4g_delivery_bad = false;
}

/*
 * Called after the endpoint layer has minted a received cap. Spawn grants are
 * deliberately ignored because je_proc_record_image() is published only after
 * the spawn transaction commits. In mode 13, the only later SEND|RECV cap
 * deliveries into a broker client are the supervisor's broker grants.
 */
void je_m4g_record_cap_delivery(je_vm *vm, uint32_t index,
                                const je_ep_capdesc *descriptor)
{
#if JE_TEST_MODE == 13
    if (!vm || !descriptor || index >= JE_VM_MAX_PROCESSES
        || !m4f_ledger_enabled || descriptor->rights
            != (JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV))
        return;
    (void)vm;
    je_proc_id receiver = JE_PROC_ID_NONE;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const je_proc_rec *record = &process_pool.rec[i];
        if ((record->state == JE_PROC_LIVE || record->state == JE_PROC_DYING)
            && record->run_linked && record->vm_index == index) {
            receiver = (je_proc_id){i, record->gen};
            break;
        }
    }
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        m4f_child_identity *child = &m4f_children[i];
        if (!child->active || child->image_id != JE_IMAGE_BROKER_CLI
            || !m4f_id_equal(child->id, receiver))
            continue;
        if (m4g_delivery_count[index] == 0u)
            m4g_delivery_identity[index] = child->id;
        if (!m4f_id_equal(m4g_delivery_identity[index], child->id)
            || m4g_delivery_count[index] >= JE_BROKER_MAX) {
            m4g_delivery_bad = true;
            return;
        }
        uint32_t slot = m4g_delivery_used;
        m4g_delivery_count[index]++;
        if (slot < JE_BROKER_MAX) {
            m4g_deliveries[slot] = (m4g_delivery){
                true, child->id, descriptor->generation,
                descriptor->rec_index, descriptor->side
            };
            m4g_delivery_used++;
        }
        return;
    }
#else
    (void)vm;
    (void)index;
    (void)descriptor;
#endif
}

static bool m4f_id_equal(je_proc_id left, je_proc_id right)
{
    return left.index == right.index && left.gen == right.gen;
}

static void m4f_ledger_reset(void)
{
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        m4f_children[i] = (m4f_child_identity){0};
        m4f_run_proc_slots[i] = false;
    }
    for (uint32_t image = 0u; image < M4F_IMAGE_COUNT; ++image) {
        m4f_spawn_count[image] = 0u;
        m4f_terminal_count[image] = 0u;
        for (uint32_t reason = 0u; reason <= JE_PROC_REASON_MAX; ++reason)
            m4f_death_count[image][reason] = 0u;
    }
    m4f_spoofer_shape_count = 0u;
    m4f_spoofer_code = 0u;
    m4f_spoofer_ep_charge = UINT32_MAX;
    m4f_unexpected_image = false;
    m4f_progress_serial = 0u;
    m4f_ledger_enabled = true;
    m4g_delivery_reset();
}

/* Called only after a spawn is committed, so a failed journal never enters the
 * run ledger. It is intentionally inert before the M4f seed point. */
void je_proc_record_image(je_proc_id id, uint32_t image_id)
{
    if (!m4f_ledger_enabled) return;
    if (id.gen == 0u || id.index >= JE_PROC_REC_MAX
        || m4f_children[id.index].active)
        fail("JeiOS fatal: M4f child ledger identity\r\n");
    m4f_children[id.index] = (m4f_child_identity){true, false, id, image_id,
                                                   0u, 0u, 0u, false};
    m4f_run_proc_slots[id.index] = true;
    if (image_id >= M4F_IMAGE_COUNT || image_id < JE_IMAGE_SUPERVISOR)
        m4f_unexpected_image = true;
    else
        ++m4f_spawn_count[image_id];
    ++m4f_progress_serial;
}
_Static_assert((1u + JE_M2_WORKERS) * 2u == 16u,
               "M2 preemption floor remains the T=8 floor");
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
static void capacity_probe(void *argument)
{
    (void)argument;
    if (je_thread_sleep(0u)!=JE_OK) fail("JeiOS fatal: capacity probe yield\r\n");
    if (je_thread_sleep(0u)!=JE_OK) fail("JeiOS fatal: capacity probe yield\r\n");
    atomic_fetch_add_explicit(&capacity_completed,1,memory_order_release);
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
extern unsigned char je_user_proc_exit7_start[], je_user_proc_exit7_end[];
extern unsigned char je_user_m4b_start[], je_user_m4b_end[];
extern unsigned char je_user_m4c_start[], je_user_m4c_end[];
extern unsigned char je_user_m4e_start[], je_user_m4e_end[];
extern unsigned char je_user_m4d_start[], je_user_m4d_end[];
extern unsigned char je_user_m4d_ready_start[], je_user_m4d_ready_end[];
extern unsigned char je_user_m4d_blocked_start[], je_user_m4d_blocked_end[];
extern unsigned char je_user_m4d_woken_start[], je_user_m4d_woken_end[];
extern unsigned char je_user_fp_start[], je_user_fp_end[];
extern unsigned char je_user_fault_start[], je_user_fault_end[];
extern unsigned char je_user_exit_start[], je_user_exit_end[];
extern unsigned char je_user_syscall_start[], je_user_syscall_end[];
extern unsigned char je_user_unknownop_start[], je_user_unknownop_end[];
extern unsigned char je_user_badret_start[], je_user_badret_end[];
extern unsigned char je_user_copyprobe_start[], je_user_copyprobe_end[];
extern unsigned char je_user_handle_start[], je_user_handle_end[];
extern unsigned char je_user_epfault_start[], je_user_epfault_end[];
extern unsigned char je_user_epipc_start[], je_user_epipc_end[];
extern unsigned char je_user_eprecv_to_start[], je_user_eprecv_to_end[];
extern unsigned char je_user_ping_start[], je_user_ping_end[];
extern unsigned char je_user_pong_start[], je_user_pong_end[];
#ifdef JE_DEV_COPY_INJECT
extern unsigned char je_user_inject_start[], je_user_inject_end[];
#endif
extern unsigned char je_user_badrsp_start[], je_user_badrsp_end[];
static je_vm vm_s3;
static unsigned char vm_blob_s3[JE_PAGE_SIZE];
typedef enum { PROG_HEARTBEAT=0, PROG_FP=1, PROG_FAULT=2, PROG_EXIT=3,
               PROG_SYSCALL=4, PROG_UNKNOWNOP=5, PROG_BADRSP=6, PROG_BADRET=7,
               PROG_COPYPROBE=8, PROG_INJECT=9, PROG_HANDLE=10,
               PROG_EPFAULT=11, PROG_EPIPC=12, PROG_EPRECV_TO=13,
               PROG_PING=14, PROG_PONG=15, PROG_PROC_EXIT7=16,
               PROG_M4B=17, PROG_M4C=18, PROG_M4D=19, PROG_M4D_READY=20,
               PROG_M4D_BLOCKED=21, PROG_M4D_WOKEN=22, PROG_M4E=23 } user_prog;
static int vm_s3_inited;
/* Initialize the shared step-3/4 vm once. The M3 fixture still uses two
 * processes, while the backing pool now has the M4 D2 capacity for later PRs. */
static void vm_s3_init_once(void)
{
    if (vm_s3_inited) return;
    const je_vm_ops *ops=je_arch_vm_ops(&je_kernel_pages, je_arch_has_nx());
    if (je_vm_init(&vm_s3, ops, &je_kernel_pages, 0)!=JE_OK) fail("JeiOS fatal: s3 vm_init\r\n");
    /* M3c (A3-1): install the endpoint teardown hook + the IRQ critical-section
     * pair so je_vm_destroy reclaims any endpoint this vm's processes hold and
     * does so atomically vs a timer. SELF-only processes see a no-op walk; the
     * s3 endpoint fault-death test (below) relies on this being installed. */
    je_endpoint_kern_install(&vm_s3);
    vm_s3_inited=1;
}
/* Build one user process into the shared vm from the selected program (code at
 * arena 0, a heartbeat/scratch data page at 0x1000). Returns its index. */
static uint32_t vm_s3_build_quota_kind(user_prog prog, uint32_t ep_limit,
                                       bool supervisor)
{
    vm_s3_init_once();
    unsigned char *start, *end;
    switch (prog) {
    case PROG_PROC_EXIT7: start=je_user_proc_exit7_start; end=je_user_proc_exit7_end; break;
    case PROG_M4B:        start=je_user_m4b_start;        end=je_user_m4b_end;        break;
    case PROG_M4C:        start=je_user_m4c_start;        end=je_user_m4c_end;        break;
    case PROG_M4E:        start=je_user_m4e_start;        end=je_user_m4e_end;        break;
    case PROG_M4D:        start=je_user_m4d_start;        end=je_user_m4d_end;        break;
    case PROG_M4D_READY:  start=je_user_m4d_ready_start;  end=je_user_m4d_ready_end;  break;
    case PROG_M4D_BLOCKED:start=je_user_m4d_blocked_start;end=je_user_m4d_blocked_end;break;
    case PROG_M4D_WOKEN:  start=je_user_m4d_woken_start;  end=je_user_m4d_woken_end;  break;
    case PROG_FP:        start=je_user_fp_start;        end=je_user_fp_end;        break;
    case PROG_FAULT:     start=je_user_fault_start;     end=je_user_fault_end;     break;
    case PROG_EXIT:      start=je_user_exit_start;      end=je_user_exit_end;      break;
    case PROG_SYSCALL:   start=je_user_syscall_start;   end=je_user_syscall_end;   break;
    case PROG_UNKNOWNOP: start=je_user_unknownop_start; end=je_user_unknownop_end; break;
    case PROG_BADRSP:    start=je_user_badrsp_start;    end=je_user_badrsp_end;    break;
    case PROG_BADRET:    start=je_user_badret_start;    end=je_user_badret_end;    break;
    case PROG_COPYPROBE: start=je_user_copyprobe_start; end=je_user_copyprobe_end; break;
    case PROG_HANDLE:    start=je_user_handle_start;    end=je_user_handle_end;    break;
    case PROG_EPFAULT:   start=je_user_epfault_start;   end=je_user_epfault_end;   break;
    case PROG_EPIPC:     start=je_user_epipc_start;     end=je_user_epipc_end;     break;
    case PROG_EPRECV_TO: start=je_user_eprecv_to_start; end=je_user_eprecv_to_end; break;
    case PROG_PING:      start=je_user_ping_start;      end=je_user_ping_end;      break;
    case PROG_PONG:      start=je_user_pong_start;      end=je_user_pong_end;      break;
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
    je_proc_id id = JE_PROC_ID_NONE;
    uint64_t flags = je_arch_irq_save();
    je_status proc_alloc = je_proc_alloc(&process_pool, ep_limit, supervisor, &id);
    if (proc_alloc != JE_OK) {
        je_arch_irq_restore(flags);
        fail("JeiOS fatal: s3 process record\r\n");
    }
    vm_s3_proc_ids[idx] = id;
    je_status register_status = je_sched_register_process(&vm_s3, idx, id);
    je_arch_irq_restore(flags);
    if (register_status != JE_OK)
        fail("JeiOS fatal: s3 process register\r\n");
    return idx;
}

static uint32_t vm_s3_build(user_prog prog)
{
    return vm_s3_build_quota_kind(prog, JE_EP_POOL_CAP, false);
}

static uint32_t vm_s3_build_quota(user_prog prog, uint32_t ep_limit)
{
    return vm_s3_build_quota_kind(prog, ep_limit, false);
}

/* PR-F kernel-seeded image path. PROC_SPAWN cannot create the first supervisor
 * because no user LAUNCHER exists yet, so this is the same journal order with
 * the supervisor flag set on the PROCESS identity. */
static uint32_t vm_s3_build_builtin(uint32_t image_id, uint32_t ep_limit,
                                    bool supervisor, je_proc_id *out_id)
{
    const je_builtin_image *image = je_builtin_image_get(image_id);
    if (!image || !out_id) fail("JeiOS fatal: M4f image lookup\r\n");
    vm_s3_init_once();
    je_proc_id id = JE_PROC_ID_NONE;
    uint64_t flags = je_arch_irq_save();
    if (je_proc_alloc(&process_pool, ep_limit, supervisor, &id) != JE_OK) {
        je_arch_irq_restore(flags);
        fail("JeiOS fatal: M4f process allocation\r\n");
    }
    uint32_t index = JE_VM_MAX_PROCESSES;
    je_status status = je_vm_build_quota(
        &vm_s3, &image->desc, image->blob, image->blob_len,
        je_arch_has_nx(), JE_VM_HANDLE_QUOTA_MAX, &index);
    if (status != JE_OK) {
        (void)je_proc_abandon(&process_pool, id);
        je_arch_irq_restore(flags);
        fail("JeiOS fatal: M4f supervisor VM build\r\n");
    }
    if (je_sched_register_process(&vm_s3, index, id) != JE_OK) {
        (void)je_vm_destroy(&vm_s3, index);
        (void)je_proc_abandon(&process_pool, id);
        je_arch_irq_restore(flags);
        fail("JeiOS fatal: M4f supervisor registration\r\n");
    }
    vm_s3_proc_ids[index] = id;
    je_proc_record_image(id, image_id);
    *out_id = id;
    je_arch_irq_restore(flags);
    return index;
}

static uint32_t m4f_seed_supervisor(je_proc_id *out_id)
{
    m4f_ledger_reset();
    uint32_t index = vm_s3_build_builtin(
        JE_IMAGE_SUPERVISOR, JE_SVC_EP_LIMIT, true, out_id);
    je_handle launcher = 0u;
    uint64_t flags = je_arch_irq_save();
    je_handle_status alloc = je_handle_alloc(
        &vm_s3.procs[index].htable, (void *)0, JE_ABI_OBJ_LAUNCHER,
        JE_ABI_RIGHT_SPAWN, &launcher);
    if (alloc != JE_HANDLE_OK) {
        je_arch_irq_restore(flags);
        fail("JeiOS fatal: M4f launcher allocation\r\n");
    }
    je_user_startup startup = {
        JE_ABI_STARTUP_V1, 1u, launcher, 0u, 0u, 0u
    };
    if (je_thread_create_user_start(&vm_s3, index, &startup, (uint32_t *)0)
        != JE_OK) {
        je_arch_irq_restore(flags);
        fail("JeiOS fatal: M4f supervisor thread\r\n");
    }
    je_arch_irq_restore(flags);
    return index;
}

static je_status vm_s3_destroy(uint32_t idx)
{
    if (idx >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    je_proc_id id = vm_s3_proc_ids[idx];
    uint64_t flags = je_arch_irq_save();
    if (je_sched_has_process(&vm_s3, idx)) {
        je_arch_console_write("JeiOS fatal: s3 destroy has scheduler reference\r\n");
        je_arch_irq_restore(flags);
        return JE_ERR_INVALID;
    }
    if (id.gen != 0u && je_proc_on_dying(&process_pool, id, JE_DEATH_KILLED, 0u) != JE_OK) {
        je_arch_irq_restore(flags);
        return JE_ERR_INVALID;
    }
    if (je_vm_destroy(&vm_s3, idx) != JE_OK) {
        je_arch_irq_restore(flags);
        return JE_ERR_INVALID;
    }
    if (id.gen != 0u && je_proc_on_reaped(&process_pool, id) != JE_OK) {
        je_arch_irq_restore(flags);
        return JE_ERR_INVALID;
    }
    if (je_sched_unregister_process(&vm_s3, idx) != JE_OK) {
        je_arch_irq_restore(flags);
        return JE_ERR_INVALID;
    }
    je_arch_irq_restore(flags);
    vm_s3_proc_ids[idx] = JE_PROC_ID_NONE;
    return JE_OK;
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

static bool svc_phase_known(uint32_t phase)
{
    return phase <= JE_SVC_PHASE_FAILED;
}

static bool svc_fail_code_known(uint32_t fail_code)
{
    return fail_code <= JE_SVC_FAIL_CRASHLOOP;
}

/*
 * The slot-0 monitor consumes this reader. The report is untrusted user memory,
 * so the PTE probe completes before CR3 changes and no field is used as an
 * address or bound. The rollback word has a separate fixed-address writer
 * below; it uses the same PTE/RW/NX precondition.
 */
static bool svc_report_read(uint32_t vm_index, je_svc_report *out)
{
    _Static_assert(sizeof(je_svc_report) == JE_SVC_REPORT_LEN,
                   "service report size");
    _Static_assert((JE_SVC_REPORT_VADDR & (UINT64_C(8) - 1u)) == 0u,
                   "service report alignment");
    _Static_assert((JE_SVC_REPORT_VADDR & (JE_PAGE_SIZE - 1u))
                   + sizeof(je_svc_report) <= JE_PAGE_SIZE,
                   "service report must not straddle a page");

    if (!out || vm_index >= JE_VM_MAX_PROCESSES || !vm_s3.ops
        || JE_SVC_REPORT_VADDR < JE_VM_ARENA_BASE
        || JE_SVC_REPORT_VADDR - JE_VM_ARENA_BASE >= JE_VM_ARENA_SIZE)
        return false;

    uint64_t off = JE_SVC_REPORT_VADDR - JE_VM_ARENA_BASE;
    uint64_t page_off = off & (JE_PAGE_SIZE - 1u);
    if (page_off > JE_PAGE_SIZE - sizeof(je_svc_report))
        return false;

    /* The PTE probe AND the raw read must happen in the same IF=0 section so no
     * future unmap/mprotect syscall (none exists today) can invalidate the
     * mapping between the check and the dereference, and so a non-slot-0 caller
     * cannot race the supervisor. We assert IF=0 rather than relying on the
     * absence of those syscalls. (Claude Stage-1 review #2.) */
    uint64_t flags = je_arch_irq_save();
    if ((flags & (UINT64_C(1) << 9)) != 0u)
        fail("JeiOS fatal: svc_report_read IF set\r\n"); /* IF must be 0 */

    uint64_t physical = 0u;
    je_vm_perm perm = (je_vm_perm)0;
    int present = 0;
    if (vm_s3.ops->read_user_pte(vm_s3.ops->context, vm_index,
                                 off & ~(uint64_t)(JE_PAGE_SIZE - 1u),
                                 &physical, &perm, &present) != JE_OK
        || !present || perm != JE_VM_PERM_RW_NX) {
        je_arch_irq_restore(flags);
        return false;
    }
    (void)physical;

    uint64_t kroot = je_arch_kernel_root_phys();
    je_arch_cr3_load(vm_s3.procs[vm_index].root);

    volatile const uint32_t *src =
        (volatile const uint32_t *)(uintptr_t)(JE_VM_ARENA_BASE + off);
    je_svc_report snapshot = {0};
    uint32_t seq_first = src[2];
    snapshot.magic = src[0];
    snapshot.version = src[1];
    snapshot.seq = seq_first;
    snapshot.phase = src[3];
    snapshot.fail_code = src[4];
    snapshot.restarts = src[5];
    snapshot.heartbeats = src[6];
    snapshot.spoofer_bits = src[7];
    snapshot.crash_attempts = src[8];
    snapshot.reserved = src[9];
    uint32_t seq_second = src[2];

    je_arch_cr3_load(kroot);
    je_arch_irq_restore(flags);

    if (seq_first != seq_second || (seq_first & 1u) != 0u)
        return false;
    if (snapshot.magic != JE_SVC_REPORT_MAGIC
        || snapshot.version != JE_SVC_REPORT_V0
        || !svc_phase_known(snapshot.phase)
        || !svc_fail_code_known(snapshot.fail_code)
        || snapshot.reserved != 0u)
        return false;
    /* Phase and fail_code must agree: FAILED requires a nonzero fail_code, and
     * any non-FAILED phase (including DONE, which the success path keys on)
     * requires fail_code == NONE. Reject a self-contradictory report here rather
     * than leaving the Stage-2 caller to interpret it. (Claude Stage-1 review #3.) */
    if ((snapshot.phase == JE_SVC_PHASE_FAILED)
            != (snapshot.fail_code != JE_SVC_FAIL_NONE))
        return false;
    *out = snapshot;
    return true;
}

/* Write a 64-bit word into proc idx's arena at offset off (CPL0, its CR3).
 * (Currently unused — kept for a future ring-3 fixture that hands a seeded
 * handle to a stub.) First used by the M3c-s7 bootstrap fixture to inject each
 * process's seeded endpoint handle into its arena before the thread starts. */
static void vm_s3_scratch_write(uint32_t idx, uint64_t off, uint64_t val)
{
    uint64_t flags=je_arch_irq_save();
    uint64_t kroot=je_arch_kernel_root_phys();
    je_arch_cr3_load(vm_s3.procs[idx].root);
    volatile uint64_t *p=(volatile uint64_t *)(uintptr_t)(JE_VM_ARENA_BASE+off);
    *p=val;
    je_arch_cr3_load(kroot);
    je_arch_irq_restore(flags);
}

static bool svc_report_word_write(uint32_t vm_index, uint64_t address,
                                  uint32_t value)
{
    if (vm_index >= JE_VM_MAX_PROCESSES || !vm_s3.ops
        || address < JE_VM_ARENA_BASE
        || address - JE_VM_ARENA_BASE >= JE_VM_ARENA_SIZE
        || (address & 3u) != 0u)
        return false;
    uint64_t off = address - JE_VM_ARENA_BASE;
    uint64_t flags = je_arch_irq_save();
    uint64_t physical = 0u;
    je_vm_perm perm = (je_vm_perm)0;
    int present = 0;
    if (vm_s3.ops->read_user_pte(vm_s3.ops->context, vm_index,
                                 off & ~(uint64_t)(JE_PAGE_SIZE - 1u),
                                 &physical, &perm, &present) != JE_OK
        || !present || perm != JE_VM_PERM_RW_NX) {
        je_arch_irq_restore(flags);
        return false;
    }
    (void)physical;
    uint64_t kroot = je_arch_kernel_root_phys();
    je_arch_cr3_load(vm_s3.procs[vm_index].root);
    *(volatile uint32_t *)(uintptr_t)address = value;
    je_arch_cr3_load(kroot);
    je_arch_irq_restore(flags);
    return true;
}

typedef struct {
    uint32_t state;
    uint32_t gen;
    bool retired;
    uint32_t reason;
    uint64_t code;
    bool run_linked;
    uint32_t thread_slot;
    uint32_t vm_index;
    bool supervisor;
    uint32_t handle_refs;
    uint32_t ep_charge;
    uint32_t ep_limit;
} m4f_proc_identity;

typedef struct {
    uint32_t state;
    uint64_t root;
} m4f_vm_identity;

typedef struct {
    size_t free_pages;
    uint16_t page_state[JE_PAGE_COUNT];
    uint32_t endpoints;
    uint32_t process_used_count;
    unsigned user_threads;
    m4f_proc_identity processes[JE_PROC_REC_MAX];
    m4f_vm_identity vms[JE_VM_MAX_PROCESSES];
} m4f_baseline;

static void m4f_take_baseline(m4f_baseline *out)
{
    if (!out) fail("JeiOS fatal: M4f baseline output\r\n");
    out->free_pages = je_kernel_pages.free_count;
    for (uint32_t i = 0u; i < JE_PAGE_COUNT; ++i)
        out->page_state[i] = je_kernel_pages.state[i];
    out->endpoints = je_endpoint_kernel_allocated();
    out->process_used_count = process_pool.used_count;
    out->user_threads = je_sched_user_thread_count();
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const je_proc_rec *rec = &process_pool.rec[i];
        out->processes[i] = (m4f_proc_identity){
            rec->state, rec->gen, rec->retired, rec->reason, rec->code,
            rec->run_linked, rec->thread_slot, rec->vm_index, rec->supervisor,
            rec->handle_refs, rec->ep_charge, rec->ep_limit
        };
    }
    for (uint32_t i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
        out->vms[i] = (m4f_vm_identity){ vm_s3.procs[i].state,
                                         vm_s3.procs[i].root };
}

static bool m4f_baseline_matches(const m4f_baseline *baseline)
{
    if (!baseline || je_kernel_pages.free_count != baseline->free_pages
        || je_endpoint_kernel_allocated() != baseline->endpoints
        || process_pool.used_count != baseline->process_used_count
        || !je_endpoint_kern_quiescent()
        || je_sched_user_thread_count() != baseline->user_threads)
        return false;
    for (uint32_t i = 0u; i < JE_PAGE_COUNT; ++i)
        if (je_kernel_pages.state[i] != baseline->page_state[i])
            return false;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const je_proc_rec *rec = &process_pool.rec[i];
        const m4f_proc_identity *old = &baseline->processes[i];
        if (old->state != JE_PROC_FREE) {
            if (rec->state != old->state || rec->gen != old->gen
                || rec->retired != old->retired
                || rec->reason != old->reason || rec->code != old->code
                || rec->run_linked != old->run_linked
                || rec->thread_slot != old->thread_slot
                || rec->vm_index != old->vm_index
                || rec->supervisor != old->supervisor
                || rec->handle_refs != old->handle_refs
                || rec->ep_charge != old->ep_charge
                || rec->ep_limit != old->ep_limit)
                return false;
        } else if (rec->state != JE_PROC_FREE || rec->gen < old->gen
                   || rec->retired != old->retired
                   || (rec->gen != old->gen && !m4f_run_proc_slots[i])
                   || rec->handle_refs != 0u || rec->ep_charge != 0u
                   || rec->run_linked || rec->supervisor)
            return false;
    }
    for (uint32_t i = 0u; i < JE_VM_MAX_PROCESSES; ++i) {
        const m4f_vm_identity *old = &baseline->vms[i];
        if (old->state != JE_VM_FREE
            && (vm_s3.procs[i].state != old->state
                || vm_s3.procs[i].root != old->root))
            return false;
        if (old->state == JE_VM_FREE && vm_s3.procs[i].state != JE_VM_FREE)
            return false;
    }
    return true;
}

static bool m4f_spoofer_endpoint_shape(const je_proc_rec *rec)
{
    if (!rec || !rec->run_linked || rec->vm_index >= JE_VM_MAX_PROCESSES)
        return false;
    const je_handle_table *table = &vm_s3.procs[rec->vm_index].htable;
    uint32_t live = 0u;
    uint32_t endpoint = 0u;
    for (uint32_t h = 0u; h < table->capacity; ++h) {
        je_handle handle = 0u;
        uint32_t type = 0u;
        if (!je_handle_live_at(table, h, &handle, &type)) continue;
        ++live;
        uint32_t rights = 0u;
        void *object = (void *)0;
        if (type != JE_ABI_OBJ_ENDPOINT
            || je_handle_resolve_any(table, handle, &type, &rights,
                                     &object) != JE_HANDLE_OK
            || rights != JE_ABI_RIGHT_SEND || object == (void *)0)
            return false;
        ++endpoint;
    }
    return live == 1u && endpoint == 1u;
}

#if JE_TEST_MODE == 13
static uint32_t m4g_live_service_vm(void)
{
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const m4f_child_identity *child = &m4f_children[i];
        const je_proc_rec *record = &process_pool.rec[i];
        if (child->active && child->image_id == JE_IMAGE_BROKER_SVC
            && !child->terminal_seen && record->state == JE_PROC_LIVE
            && record->run_linked)
            return record->vm_index;
    }
    return JE_VM_MAX_PROCESSES;
}

static void m4g_capture_client_handles(const m4f_child_identity *client,
                                        const je_proc_rec *record)
{
    if (!client || !record || client->id.index >= JE_PROC_REC_MAX
        || !record->run_linked || record->vm_index >= JE_VM_MAX_PROCESSES)
        return;
    je_proc_id supervisor = JE_PROC_ID_NONE;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i)
        if (m4f_children[i].active
            && m4f_children[i].image_id == JE_IMAGE_SUPERVISOR) {
            supervisor = m4f_children[i].id;
            break;
        }
    uint32_t service_vm = m4g_live_service_vm();
    uint32_t supervisor_vm = JE_VM_MAX_PROCESSES;
    if (supervisor.index < JE_PROC_REC_MAX
        && process_pool.rec[supervisor.index].run_linked)
        supervisor_vm = process_pool.rec[supervisor.index].vm_index;
    const je_handle_table *table = &vm_s3.procs[record->vm_index].htable;
    uint32_t brokered_count = 0u;
    uint32_t service_peer_count = 0u;
    for (uint32_t slot = 0u; slot < table->capacity; ++slot) {
        je_handle handle = 0u;
        uint32_t type = 0u;
        uint32_t rights = 0u;
        void *object = (void *)0;
        if (!je_handle_live_at(table, slot, &handle, &type)
            || type != JE_ABI_OBJ_ENDPOINT
            || je_handle_resolve_any(table, handle, &type, &rights,
                                     &object) != JE_HANDLE_OK
            || rights != (JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV))
            continue;
        je_proc_id creator = JE_PROC_ID_NONE;
        const je_ep_end *peer = (const je_ep_end *)0;
        je_ep_end_state state = JE_EP_END_FREE;
        if (!je_endpoint_kern_inspect_end(object, &creator, &peer, &state)
            || !m4f_id_equal(creator, supervisor)
            || state == JE_EP_END_CLOSED
            || m4g_table_holds_object(supervisor_vm, peer))
            continue;
        ++brokered_count;
        if (service_vm < JE_VM_MAX_PROCESSES
            && m4g_table_holds_object(service_vm, peer))
            ++service_peer_count;
    }
    m4g_client_audit *audit = &m4g_client_audits[client->id.index];
    audit->captured = true;
    audit->client = client->id;
    if (brokered_count > audit->brokered_count)
        audit->brokered_count = brokered_count;
    if (service_peer_count > audit->service_peer_count)
        audit->service_peer_count = service_peer_count;
}

static void m4g_capture_live_clients(void)
{
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const m4f_child_identity *child = &m4f_children[i];
        const je_proc_rec *record = &process_pool.rec[i];
        if (!child->active || child->image_id != JE_IMAGE_BROKER_CLI
            || child->terminal_seen || record->state != JE_PROC_LIVE)
            continue;
        m4g_capture_client_handles(child, record);
    }
}
#endif

/* Snapshot terminal facts before the monitor reaper destroys the VM slot. */
static void m4f_capture_terminal_children(void)
{
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        m4f_child_identity *child = &m4f_children[i];
        if (!child->active || child->terminal_seen) continue;
        const je_proc_rec *rec = &process_pool.rec[i];
        if (!m4f_id_equal(child->id, (je_proc_id){i, rec->gen}))
            fail("JeiOS fatal: M4f child ledger generation\r\n");
        if (rec->state != JE_PROC_DYING || rec->supervisor) continue;
        child->terminal_seen = true;
        child->terminal_reason = rec->reason;
        child->terminal_code = rec->code;
        child->terminal_ep_charge = rec->ep_charge;
#if JE_TEST_MODE == 13
        if (child->image_id == JE_IMAGE_BROKER_CLI) {
            m4g_capture_client_handles(child, rec);
        }
#endif
        if (child->image_id >= M4F_IMAGE_COUNT
            || child->terminal_reason > JE_PROC_REASON_MAX) {
            m4f_unexpected_image = true;
        } else {
            ++m4f_terminal_count[child->image_id];
            ++m4f_death_count[child->image_id][child->terminal_reason];
        }
        if (child->image_id == JE_IMAGE_SPOOFER) {
            child->terminal_endpoint_shape = m4f_spoofer_endpoint_shape(rec);
            if (child->terminal_endpoint_shape) ++m4f_spoofer_shape_count;
            m4f_spoofer_code = child->terminal_code;
            m4f_spoofer_ep_charge = child->terminal_ep_charge;
        }
        ++m4f_progress_serial;
    }
}

static void m4f_clear_reaped_children(void)
{
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        m4f_child_identity *child = &m4f_children[i];
        if (!child->active || child->image_id == JE_IMAGE_SUPERVISOR)
            continue;
        const je_proc_rec *rec = &process_pool.rec[i];
        if (!m4f_id_equal(child->id, (je_proc_id){i, rec->gen}))
            continue;
        if (rec->state != JE_PROC_LIVE && rec->state != JE_PROC_DYING) {
            child->active = false;
            ++m4f_progress_serial;
        }
    }
}

static void m4f_clear_free_vm_ids(void)
{
    for (uint32_t i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
        if (vm_s3.procs[i].state == JE_VM_FREE)
            vm_s3_proc_ids[i] = JE_PROC_ID_NONE;
}

#if JE_TEST_MODE == 13
static bool m4g_table_holds_object(uint32_t vm_index, const void *object)
{
    if (!object || vm_index >= JE_VM_MAX_PROCESSES
        || vm_s3.procs[vm_index].state == JE_VM_FREE)
        return false;
    const je_handle_table *table = &vm_s3.procs[vm_index].htable;
    for (uint32_t slot = 0u; slot < table->capacity; ++slot) {
        je_handle handle = 0u;
        uint32_t type = 0u;
        uint32_t rights = 0u;
        void *candidate = (void *)0;
        if (!je_handle_live_at(table, slot, &handle, &type)
            || type != JE_ABI_OBJ_ENDPOINT
            || je_handle_resolve_any(table, handle, &type, &rights,
                                     &candidate) != JE_HANDLE_OK)
            continue;
        if (candidate == object) return true;
    }
    return false;
}

static uint32_t m4g_endpoint_count(const m4f_child_identity *client,
                                   je_proc_id supervisor,
                                   uint32_t supervisor_vm,
                                   uint32_t service_vm,
                                   bool require_service_peer)
{
    (void)supervisor;
    (void)supervisor_vm;
    (void)service_vm;
    if (!client || !client->terminal_seen
        || client->id.index >= JE_PROC_REC_MAX)
        return 0u;
    const m4g_client_audit *audit = &m4g_client_audits[client->id.index];
    if (!audit->captured || !m4f_id_equal(audit->client, client->id))
        return 0u;
    return require_service_peer ? audit->service_peer_count
                                : audit->brokered_count;
}

static const m4f_child_identity *m4g_find_client(uint32_t required_bits,
                                                 uint32_t forbidden_bits)
{
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const m4f_child_identity *child = &m4f_children[i];
        if (child->image_id != JE_IMAGE_BROKER_CLI || !child->terminal_seen)
            continue;
        uint32_t code = (uint32_t)child->terminal_code;
        if ((code & required_bits) == required_bits
            && (code & forbidden_bits) == 0u)
            return child;
    }
    return (const m4f_child_identity *)0;
}

static bool m4g_corroborate(je_proc_id supervisor,
                            const je_svc_report *report)
{
    (void)report;
    if (m4f_unexpected_image
        || m4f_spawn_count[JE_IMAGE_SPAWN_CHILD] != 0u
        || m4f_spawn_count[JE_IMAGE_C_HELLO] != 0u
        || m4f_spawn_count[JE_IMAGE_SUPERVISOR] != 1u
        || m4f_spawn_count[JE_IMAGE_BROKER_SVC] != 2u
        || m4f_spawn_count[JE_IMAGE_BROKER_CLI] != 2u
        || m4f_spawn_count[JE_IMAGE_RESTART_SVC] != 0u
        || m4f_spawn_count[JE_IMAGE_SURVIVOR] != 0u
        || m4f_spawn_count[JE_IMAGE_SPOOFER] != 0u
        || m4f_spawn_count[JE_IMAGE_CRASHLOOP] != 0u
        || m4f_terminal_count[JE_IMAGE_BROKER_SVC] != 1u
        || m4f_terminal_count[JE_IMAGE_BROKER_CLI] != 2u
        || m4f_death_count[JE_IMAGE_BROKER_SVC][JE_DEATH_FAULT] != 1u
        || m4f_death_count[JE_IMAGE_BROKER_CLI][JE_DEATH_EXIT] != 2u
        || m4g_delivery_bad || m4g_delivery_used != 3u)
        return false;
    const m4f_child_identity *client_a = m4g_find_client(
        JE_BROKER_TEST_BIT_READY | JE_BROKER_TEST_BIT_GRANTS
            | JE_BROKER_TEST_BIT_ECHO | JE_BROKER_TEST_BIT_NO_TRANSFER
            | JE_BROKER_TEST_BIT_PEER_CLOSED | JE_BROKER_TEST_BIT_RESTARTED
            | JE_BROKER_TEST_BIT_EXHAUSTED,
        JE_BROKER_TEST_BIT_ROLE_B);
    const m4f_child_identity *client_b = m4g_find_client(
        JE_BROKER_TEST_BIT_READY | JE_BROKER_TEST_BIT_GRANTS
            | JE_BROKER_TEST_BIT_ECHO | JE_BROKER_TEST_BIT_NO_TRANSFER
            | JE_BROKER_TEST_BIT_ROLE_B,
        JE_BROKER_TEST_BIT_EXHAUSTED);
    if (!client_a || !client_b || client_a == client_b
        || m4g_delivery_count[client_a->id.index] != 2u
        || m4g_delivery_count[client_b->id.index] != 1u
        || !m4f_id_equal(m4g_delivery_identity[client_a->id.index],
                          client_a->id)
        || !m4f_id_equal(m4g_delivery_identity[client_b->id.index],
                          client_b->id))
        return false;
    for (uint32_t i = 0u; i < m4g_delivery_used; ++i)
        if (!m4g_deliveries[i].active
            || m4g_deliveries[i].endpoint_index >= JE_EP_POOL_CAP
            || m4g_deliveries[i].endpoint_side > 1u
            || m4g_deliveries[i].endpoint_generation == 0u)
            return false;

    const je_proc_rec *sup = &process_pool.rec[supervisor.index];
    if (!m4f_id_equal(supervisor, (je_proc_id){supervisor.index, sup->gen})
        || sup->state != JE_PROC_DYING
        || sup->reason != JE_DEATH_EXIT
        || sup->code != JE_SVC_DONE_EXIT_G
        || !sup->run_linked)
        return false;
    uint32_t supervisor_vm = sup->vm_index;
    if (m4g_endpoint_count(client_a, supervisor, supervisor_vm,
                           JE_VM_MAX_PROCESSES, true) != 1u
        || m4g_endpoint_count(client_b, supervisor, supervisor_vm,
                              JE_VM_MAX_PROCESSES, false) != 1u)
        return false;
    je_arch_console_write("M4g: EXHAUSTED observed and kernel-delivery count corroborated\r\n");
    return true;
}
#endif

static bool m4f_corroborate(je_proc_id supervisor,
                            const je_svc_report *report)
{
#if JE_TEST_MODE == 13
    return m4g_corroborate(supervisor, report);
#else
    (void)report; /* report counters are diagnostics, never proof */
    if (m4f_unexpected_image
        || m4f_spawn_count[JE_IMAGE_SPAWN_CHILD] != 0u
        || m4f_spawn_count[JE_IMAGE_C_HELLO] != 0u
        || m4f_spawn_count[JE_IMAGE_SUPERVISOR] != 1u
        || m4f_spawn_count[JE_IMAGE_RESTART_SVC]
            != JE_SVC_RESTART_LIMIT + 1u
        || m4f_spawn_count[JE_IMAGE_SURVIVOR] != 1u
        || m4f_spawn_count[JE_IMAGE_SPOOFER] != 1u
        || m4f_spawn_count[JE_IMAGE_CRASHLOOP]
            != JE_SVC_CRASHLOOP_RESTART_LIMIT + 1u
        || m4f_terminal_count[JE_IMAGE_RESTART_SVC] != 1u
        || m4f_terminal_count[JE_IMAGE_SPOOFER] != 1u
        || m4f_terminal_count[JE_IMAGE_CRASHLOOP]
            != JE_SVC_CRASHLOOP_RESTART_LIMIT + 1u
        || m4f_death_count[JE_IMAGE_RESTART_SVC][JE_DEATH_FAULT]
            != JE_SVC_RESTART_LIMIT
        || m4f_death_count[JE_IMAGE_SPOOFER][JE_DEATH_EXIT] != 1u
        || m4f_death_count[JE_IMAGE_CRASHLOOP][JE_DEATH_FAULT]
            != JE_SVC_CRASHLOOP_RESTART_LIMIT + 1u
        || m4f_spoofer_shape_count != 1u
        || m4f_spoofer_ep_charge != 0u
        || m4f_spoofer_code
            != (((uint64_t)JE_SVC_SPOOF_MAGIC << 32)
                | ((uint64_t)((~JE_SVC_SPOOF_EXPECTED_MASK)
                              & UINT32_C(0xffff)) << 16)
                | JE_SVC_SPOOF_EXPECTED_MASK)
        || je_spawn_failpoint_fired() != 1u)
        return false;

    uint32_t live_restart = 0u;
    uint32_t live_survivor = 0u;
    bool survivor_dispatched = false;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        const m4f_child_identity *child = &m4f_children[i];
        if (!child->active || child->image_id == JE_IMAGE_SUPERVISOR)
            continue;
        const je_proc_rec *rec = &process_pool.rec[i];
        if (!m4f_id_equal(child->id, (je_proc_id){i, rec->gen}))
            return false;
        if (rec->state == JE_PROC_LIVE) {
            if (!rec->run_linked || rec->vm_index >= JE_VM_MAX_PROCESSES)
                return false;
            if (child->image_id == JE_IMAGE_RESTART_SVC) ++live_restart;
            else if (child->image_id == JE_IMAGE_SURVIVOR) {
                ++live_survivor;
                survivor_dispatched =
                    je_sched_dispatches_of(&vm_s3, rec->vm_index) > 0u;
            } else {
                return false;
            }
        } else if (rec->state == JE_PROC_DYING) {
            return false;
        }
    }
    const je_proc_rec *sup = &process_pool.rec[supervisor.index];
    if (!m4f_id_equal(supervisor, (je_proc_id){supervisor.index, sup->gen})
        || sup->state != JE_PROC_DYING || sup->reason != JE_DEATH_EXIT
        || sup->code != JE_SVC_DONE_EXIT)
        return false;
    return live_restart == 1u && live_survivor == 1u && survivor_dispatched;
#endif
}

static _Noreturn void m4f_teardown(const m4f_baseline *baseline,
                                   je_proc_id supervisor, bool success,
                                   uint32_t reason, uint32_t last_phase)
{
    uint64_t terminal_code = 0u;
    if (supervisor.index < JE_PROC_REC_MAX
        && m4f_id_equal(supervisor,
                        (je_proc_id){supervisor.index,
                                     process_pool.rec[supervisor.index].gen}))
        terminal_code = process_pool.rec[supervisor.index].code;
    if (success) {
#if JE_TEST_MODE == 13
        je_arch_console_write("M4g-s1: broker scenario completed\r\n");
#else
        je_arch_console_write("M4f-s1: supervisor scenario completed\r\n");
#endif
    } else {
#if JE_TEST_MODE == 13
        je_arch_console_write("M4g: supervisor lost (last_phase=");
#else
        je_arch_console_write("M4f: supervisor lost (last_phase=");
#endif
        je_arch_console_hex(last_phase);
        je_arch_console_write("; reason=");
        je_arch_console_hex(reason);
        je_arch_console_write("; code=");
        je_arch_console_hex(terminal_code);
        je_arch_console_write("); no recovery path in this profile\r\n");
    }
    uint32_t killed = 0u;
    if (je_sched_kill_all_user(JE_PROC_ID_NONE, &killed) != JE_OK)
        fail("JeiOS fatal: M4f kill-all teardown\r\n");
    (void)supervisor;
    int first_reap = je_sched_reap_dead();
    if (first_reap < 0 || je_sched_reap_dead() != 0
        || je_sched_has_dying())
        fail("JeiOS fatal: M4f identity baseline teardown\r\n");
    m4f_clear_free_vm_ids();
    if (!m4f_baseline_matches(baseline))
        fail("JeiOS fatal: M4f identity baseline teardown\r\n");
#if JE_TEST_MODE == 13
    if (je_endpoint_kernel_allocated() != 0u || !je_endpoint_kern_quiescent()
        || process_pool.used_count != 0u)
        fail("JeiOS fatal: M4g endpoint quiescence\r\n");
    je_arch_console_write("M4g: zero-leak identity baseline verified\r\n");
#else
    je_arch_console_write("M4f: zero-leak identity baseline verified\r\n");
#endif
    je_arch_halt();
}

static _Noreturn void m4f_monitor_deadline(uint32_t phase)
{
    je_arch_console_write("JeiOS fatal: M4f monitor deadline phase=");
    je_arch_console_hex(phase);
    fail("\r\n");
}

static void m4f_reap_monitor_children(void)
{
    m4f_capture_terminal_children();
#if JE_TEST_MODE == 13
    m4g_capture_live_clients();
#endif
    if (je_sched_reap_dead() < 0)
        fail("JeiOS fatal: M4f monitor reap\r\n");
    m4f_clear_reaped_children();
    m4f_clear_free_vm_ids();
}

static _Noreturn void m4f_monitor(const m4f_baseline *baseline,
                                  je_proc_id supervisor, uint32_t supervisor_vm)
{
    uint32_t last_phase = UINT32_MAX;
    uint32_t observed_progress = m4f_progress_serial;
    uint64_t deadline_start = je_sched_ticks();
    bool rollback_armed = false;
    for (;;) {
        uint64_t flags = je_arch_irq_save();
        uint32_t state = JE_PROC_FREE;
        uint32_t reason = JE_DEATH_FAULT;
        uint64_t code = 0u;
        je_status status = je_proc_status(
            &process_pool, supervisor, &state, &reason, &code);
        je_svc_report report = {0};
        bool readable = status == JE_OK
            && svc_report_read(supervisor_vm, &report);
        if (status == JE_OK && state == JE_PROC_LIVE) {
            bool phase_progress = readable && report.phase != last_phase;
            if (readable && report.phase != last_phase) {
                last_phase = report.phase;
#if JE_TEST_MODE == 13
                je_arch_console_write("M4g: supervisor phase=");
#else
                je_arch_console_write("M4f: supervisor phase=");
#endif
                je_arch_console_hex(report.phase);
                je_arch_console_write("\r\n");
            }
            if (readable && report.phase == JE_SVC_PHASE_ARM
                && !rollback_armed) {
                if (vm_s3_live_count() >= JE_VM_MAX_PROCESSES)
                    fail("JeiOS fatal: M4f rollback no VM headroom\r\n");
                je_spawn_failpoint_set(4u, 0u);
                if (!svc_report_word_write(supervisor_vm,
                                           JE_SVC_ROLLBACK_GO_VADDR,
                                           JE_SVC_ROLLBACK_GO))
                    fail("JeiOS fatal: M4f rollback arm write\r\n");
                rollback_armed = true;
            }
            uint32_t verify_state = JE_PROC_FREE;
            uint32_t verify_reason = JE_DEATH_FAULT;
            uint64_t verify_code = 0u;
            if (je_proc_status(&process_pool, supervisor, &verify_state,
                               &verify_reason, &verify_code) != JE_OK
                || verify_state != JE_PROC_LIVE) {
                /*
                 * The supervisor may die after the first scan but before the
                 * child reaper. Do not let that reaper destroy the supervisor
                 * record before D12 has made its decision.
                 */
                m4f_capture_terminal_children();
                je_svc_report terminal_report = report;
                bool terminal_readable =
                    svc_report_read(supervisor_vm, &terminal_report);
                if (terminal_readable
                    && terminal_report.phase != last_phase) {
                    last_phase = terminal_report.phase;
#if JE_TEST_MODE == 13
                    je_arch_console_write("M4g: supervisor phase=");
#else
                    je_arch_console_write("M4f: supervisor phase=");
#endif
                    je_arch_console_hex(last_phase);
                    je_arch_console_write("\r\n");
                }
                bool terminal_success =
                    terminal_readable
                    && terminal_report.phase == JE_SVC_PHASE_DONE
                    && m4f_corroborate(supervisor, &terminal_report);
                m4f_teardown(baseline, supervisor, terminal_success,
                             verify_reason, last_phase);
            }
            m4f_reap_monitor_children();
            if (phase_progress || m4f_progress_serial != observed_progress) {
                observed_progress = m4f_progress_serial;
                deadline_start = je_sched_ticks();
            } else if (je_sched_ticks() - deadline_start
                       >= JE_SVC_MONITOR_DEADLINE) {
                m4f_monitor_deadline(last_phase);
            }
            je_arch_irq_restore(flags);
            __asm__ volatile("sti; hlt" ::: "memory");
            continue;
        }
        m4f_capture_terminal_children();
        bool success = readable && report.phase == JE_SVC_PHASE_DONE
            && m4f_corroborate(supervisor, &report);
        m4f_teardown(baseline, supervisor, success, reason, last_phase);
    }
}

/* M4d coordinator wait: permit one interrupt, but do not reap. The coordinator
 * uses this only while a victim or the killer is expected to make progress.
 * Reaping remains explicit at the later je_kernel_idle_step() checkpoints. */
static void m4d_wait_interrupt(void)
{
    (void)je_arch_irq_save();
    __asm__ volatile("sti; hlt" ::: "memory");
}

typedef struct {
    size_t pages;
    uint32_t endpoints;
    uint32_t records;
    uint32_t parent_live;
    unsigned threads;
    uint32_t vms;
    uint16_t grant_handle_refs[2];
    uint16_t grant_inflight_refs[2];
} m4c_baseline;

static uint32_t vm_s3_live_count(void)
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
        if (vm_s3.procs[i].state != JE_VM_FREE) ++count;
    return count;
}

/* The grant handles name endpoint ends. This test-only read is valid while IF=0:
 * handle slots and endpoint refcounts are kernel-owned mutable state, and no
 * endpoint can be freed while the parent still owns its source handle. */
static bool m4c_grant_refs(uint32_t spawner, const je_handle grants[2],
                           uint16_t handle_refs[2], uint16_t inflight_refs[2])
{
    if (spawner >= JE_VM_MAX_PROCESSES) return false;
    je_handle_table *table = &vm_s3.procs[spawner].htable;
    for (uint32_t i = 0u; i < 2u; ++i) {
        uint32_t slot_index = 0u;
        uint32_t generation = 0u;
        je_handle_decode(grants[i], &slot_index, &generation);
        if (slot_index >= table->capacity) return false;
        je_handle_slot *slot = &table->slots[slot_index];
        if (slot->state != JE_HSLOT_LIVE
            || slot->generation != generation
            || slot->type != JE_ABI_OBJ_ENDPOINT
            || !slot->object)
            return false;
        je_ep_end *end = (je_ep_end *)slot->object;
        handle_refs[i] = end->handle_refs;
        inflight_refs[i] = end->inflight_refs;
    }
    return true;
}

/* Called with IF=0. All fields are read as one scheduler-observation point. */
static void m4c_take_baseline(uint32_t spawner, const je_handle grants[2],
                              m4c_baseline *out)
{
    out->pages = je_kernel_pages.free_count;
    out->endpoints = je_endpoint_kernel_allocated();
    out->records = process_pool.used_count;
    out->parent_live = vm_s3.procs[spawner].htable.live;
    out->threads = je_sched_user_thread_count();
    out->vms = vm_s3_live_count();
    if (!m4c_grant_refs(spawner, grants, out->grant_handle_refs,
                        out->grant_inflight_refs))
        fail("JeiOS fatal: M4c grant baseline handle\r\n");
}

/* Called with IF=0. A failed journal operation must leave every count and both
 * endpoint refcount pairs unchanged, not merely the pool occupancy totals. */
static bool m4c_baseline_matches(uint32_t spawner, const je_handle grants[2],
                                 const m4c_baseline *baseline)
{
    uint16_t handle_refs[2] = {0u, 0u};
    uint16_t inflight_refs[2] = {0u, 0u};
    if (je_kernel_pages.free_count != baseline->pages
        || je_endpoint_kernel_allocated() != baseline->endpoints
        || process_pool.used_count != baseline->records
        || vm_s3.procs[spawner].htable.live != baseline->parent_live
        || je_sched_user_thread_count() != baseline->threads
        || vm_s3_live_count() != baseline->vms
        || !m4c_grant_refs(spawner, grants, handle_refs, inflight_refs))
        return false;
    for (uint32_t i = 0u; i < 2u; ++i)
        if (handle_refs[i] != baseline->grant_handle_refs[i]
            || inflight_refs[i] != baseline->grant_inflight_refs[i])
            return false;
    return true;
}

/* LFENCE/RDTSC brackets one direct spawn. This is a diagnostic
 * cycle count, not a timing bound or a substitute for the CLOCK_TICKS ABI. */
static uint64_t m4c_rdtsc_start(void)
{
    uint32_t low = 0u;
    uint32_t high = 0u;
    __asm__ volatile("lfence\n\trdtsc"
                     : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | (uint64_t)low;
}

static uint64_t m4c_rdtsc_end(void)
{
    uint32_t low = 0u;
    uint32_t high = 0u;
    __asm__ volatile("lfence\n\trdtsc"
                     : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | (uint64_t)low;
}

/* Called with IF=0 immediately after PROC_SPAWN. The new thread cannot have
 * dispatched yet, so the fixture can exercise the published PROCESS handle and
 * then use the same ordered teardown as the journal's published-thread path. */
static void m4c_discard_spawn(uint32_t spawner, je_handle process_handle)
{
    uint32_t slot = 0u;
    uint32_t generation = 0u;
    je_handle_decode(process_handle, &slot, &generation);
    if (slot >= vm_s3.procs[spawner].htable.capacity)
        fail("JeiOS fatal: M4c spawned handle slot\r\n");
    je_handle_slot *handle_slot = &vm_s3.procs[spawner].htable.slots[slot];
    if (handle_slot->state != JE_HSLOT_LIVE
        || handle_slot->generation != generation
        || handle_slot->type != JE_ABI_OBJ_PROCESS
        || !handle_slot->object)
        fail("JeiOS fatal: M4c spawned handle binding\r\n");
    je_proc_rec *rec = (je_proc_rec *)handle_slot->object;
    je_proc_id id = {
        (uint32_t)(rec - &process_pool.rec[0]), rec->gen
    };
    uint32_t child_index = rec->vm_index;
    if (!rec->run_linked || child_index >= JE_VM_MAX_PROCESSES)
        fail("JeiOS fatal: M4c spawned record link\r\n");
    if (je_sched_drop_user(&vm_s3, child_index) != JE_OK
        || je_vm_destroy(&vm_s3, child_index) != JE_OK
        || je_proc_on_dying(&process_pool, id, JE_DEATH_KILLED, 0u) != JE_OK
        || je_proc_on_reaped(&process_pool, id) != JE_OK)
        fail("JeiOS fatal: M4c spawned teardown\r\n");
    if (je_handle_close(&vm_s3.procs[spawner].htable, process_handle)
        != JE_HANDLE_OK)
        fail("JeiOS fatal: M4c spawned handle close\r\n");
    je_proc_release(&process_pool, id);
}
static _Noreturn void fail_m4c_phase(const char *message, uint32_t idx,
                                     uint64_t phase_offset)
{
    (void)je_arch_irq_save();
    je_arch_console_write(message);
    je_arch_console_hex(vm_s3_scratch(idx, phase_offset));
    je_arch_console_write("\r\n");
    je_arch_halt();
}
static void vm_s3_assert_only_ready(uint32_t survivor, const char *message)
{
    if (survivor >= JE_VM_MAX_PROCESSES ||
        vm_s3.procs[survivor].state != JE_VM_READY)
        fail(message);
    for (uint32_t i=0; i<JE_VM_MAX_PROCESSES; ++i)
        if (i != survivor && vm_s3.procs[i].state != JE_VM_FREE)
            fail(message);
}
static void vm_s3_assert_all_free(const char *message)
{
    for (uint32_t i=0; i<JE_VM_MAX_PROCESSES; ++i)
        if (vm_s3.procs[i].state != JE_VM_FREE)
            fail(message);
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
    je_proc_pool_init(&process_pool);
    je_sched_set_proc_pool(&process_pool);
    je_endpoint_kern_set_proc_pool(&process_pool);
    if (je_builtin_images_init() != JE_OK)
        fail("JeiOS fatal: built-in image table\r\n");
    if (je_thread_sleep(1)!=JE_ERR_DENIED) fail("JeiOS fatal: idle slept\r\n");
    for (uintptr_t i=1;i<=JE_M2_WORKERS;++i)
        if (je_thread_create(worker,(void *)i)!=JE_OK) fail("JeiOS fatal: thread creation\r\n");
    je_arch_console_write("M2: seven guarded threads ready; enabling PIT\r\n");
    je_arch_timer_init();
    for (;;) {
        (void)je_arch_irq_save();
        if (atomic_load_explicit(&completed,memory_order_acquire)==JE_M2_WORKERS
            && je_sched_finished()) break;
        __asm__ volatile("sti; hlt":::"memory");
    }
    if (je_sched_preemptions()<(1u+JE_M2_WORKERS)*2u || je_sched_min_dispatches()<3)
        fail("JeiOS fatal: insufficient preemption\r\n");
    je_arch_console_write("M2: preemption/GPR/sleep/heap checks passed; ticks=");
    je_arch_console_hex(je_sched_ticks());je_arch_console_write("; preemptions=");
    je_arch_console_hex(je_sched_preemptions());je_arch_console_write("\r\n");
    if (je_sched_reap()!=JE_OK || je_kernel_pages.free_count!=free_before)
        fail("JeiOS fatal: stack or heap resource leak\r\n");
    je_arch_console_write("M2: exited threads reaped; page accounting restored\r\n");
    /* M4 D2 capacity phase: after the original M2 workload has been reaped,
     * create probes whose BODY allocates nothing (each je_thread_create still
     * allocates its guarded stack) with interrupts disabled until every
     * non-idle scheduler slot is occupied. The failed next create must not
     * allocate or change page accounting. */
    atomic_store_explicit(&capacity_completed,0,memory_order_release);
    const unsigned capacity_fill = JE_THREAD_MAX - 1u;
    const size_t capacity_free_before=je_kernel_pages.free_count;
    const uint64_t capacity_flags=je_arch_irq_save();
    for (unsigned i=0u;i<capacity_fill;++i)
        if (je_thread_create(capacity_probe,(void *)0)!=JE_OK)
            fail("JeiOS fatal: thread capacity fill\r\n");
    const size_t free_after_create=je_kernel_pages.free_count;
    if (je_thread_create(capacity_probe,(void *)0)!=JE_ERR_NOMEM
        || je_kernel_pages.free_count!=free_after_create)
        fail("JeiOS fatal: thread capacity rollback\r\n");
    je_arch_irq_restore(capacity_flags);
    for (;;) {
        (void)je_arch_irq_save();
        if (atomic_load_explicit(&capacity_completed,memory_order_acquire)==capacity_fill
            && je_sched_finished()) break;
        __asm__ volatile("sti; hlt":::"memory");
    }
    if (je_sched_reap()!=JE_OK || je_kernel_pages.free_count!=capacity_free_before)
        fail("JeiOS fatal: thread capacity reap/page accounting\r\n");
    je_arch_console_write("M2: thread capacity exhausted at T=");
    je_arch_console_hex(JE_THREAD_MAX);
    je_arch_console_write("; NOMEM rollback and reap OK\r\n");

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
        /* M3 progress waits retain JE_THREAD_MAX*2 (and the existing
         * JE_THREAD_MAX*3 and JE_THREAD_MAX*4
         * waits below): these are conservative whole-scheduler-table budgets,
         * so T=16 means two, three, or four full slot passes. M2 is different:
         * its eight capacity probes are not workload iterations, so its
         * preemption floor is derived from JE_M2_WORKERS above. */
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
        if (vm_s3_destroy(uidx)!=JE_OK) fail("JeiOS fatal: s3 destroy\r\n");

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
            if (vm_s3_destroy(S)!=JE_OK) fail("JeiOS fatal: s5a syscall destroy\r\n");
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
            if (vm_s3_destroy(U)!=JE_OK) fail("JeiOS fatal: s5a unknown destroy\r\n");
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
            if (vm_s3_destroy(C)!=JE_OK) fail("JeiOS fatal: s5b copyprobe destroy\r\n");
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
            if (vm_s3_destroy(I)!=JE_OK) fail("JeiOS fatal: s5b inject destroy\r\n");
            je_arch_console_write("M3a-s5b: exception-fixup recovered load+store copy-site #PF (dev inject); kernel alive\r\n");
        }
#endif

        /* M3a intentionally leaves only the fault-survivor B live for the
         * following M3b/M3c proofs. Every other VM slot must already be FREE. */
        vm_s3_assert_only_ready(B, "JeiOS fatal: M3a vm process leak\r\n");

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
            /* handle 0 is BADINDEX. Index==capacity is also BADINDEX, while
             * the separate in-range FREE-slot probe is STALE. */
            if (vm_s3_scratch(H,0x1058)!=JE_ABI_H_BADINDEX) fail("JeiOS fatal: s5c close(0) not BADINDEX\r\n");
            if (vm_s3_scratch(H,0x1060)!=JE_ABI_H_BADINDEX) fail("JeiOS fatal: s5c capacity bound not BADINDEX\r\n");
            if (vm_s3_scratch(H,0x1018)!=JE_ABI_H_STALE)   fail("JeiOS fatal: s5c in-range-free not STALE\r\n");
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
            /* Quota (distinct from NOSLOTS): the default build quota remains 15,
             * so the mint loop ends after EXACTLY 14 more successful mints
             * (quota 15 minus the live self#2). This is a measured fact, not
             * merely "ended at QUOTA"; a custom build quota is covered by PR-A's
             * hosted API contract and is not this legacy ring-3 fixture. */
            if (vm_s3_scratch(H,0x1080)!=JE_ABI_H_QUOTA)   fail("JeiOS fatal: s5c quota not QUOTA\r\n");
            if (vm_s3_scratch(H,0x1098)!=14u)              fail("JeiOS fatal: s5c quota count != 14\r\n");
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, H)!=JE_OK) fail("JeiOS fatal: s5c handle drop\r\n");
            if (vm_s3_destroy(H)!=JE_OK) fail("JeiOS fatal: s5c handle destroy\r\n");
            je_arch_console_write("M3b-s1: handle self/query/rights/dup/close OK; attenuation drops INSPECT -> handle_rights DENIED (enforcement real); superset DENIED; unknown-right INVALID; reuse re-stales; BADINDEX at capacity restored; in-range-free STALE; default quota 15; B alive\r\n");
        }
        /* M3b intentionally preserves only the M3a survivor B. */
        vm_s3_assert_only_ready(B, "JeiOS fatal: M3b vm process leak\r\n");

        /* --- M3c s3: endpoint lifecycle wired to ring 3 + process-death reclaim.
         * Two proofs:
         *   (A) a KERNEL-ONLY self-test of the survivor-peer case — one process
         *       table holds end0, the OTHER holds end1; destroying the holder of
         *       end0 must leave end1 PEER_CLOSED and the record still allocated
         *       (survivor keeps it), and closing end1 then frees it. (A ring-3
         *       cross-process peer needs transfer (step 5); this proves the
         *       survivor semantics honestly without claiming the ring-3 path does.)
         *   (B) a RING-3 fault-death loop: a process calls endpoint_create, closes
         *       one end via the typed dispatch, then faults; after reap the pool
         *       MUST return to baseline. Looped > JE_EP_POOL_CAP times to prove no
         *       leak exhausts the pool. */
        {
            /* (A) survivor-peer, kernel-only, using two live process tables. The
             * two s4 processes A/B are gone by here; build two fresh SELF-less
             * holders is overkill — reuse the raw channel-create against two
             * distinct process htables we construct via short-lived vms is also
             * heavy. Simplest honest proof: two tables are &vm_s3.procs[0/1]
             * htables, which are FREE here; init two throwaway tables on the stack
             * is cleaner and needs no vm. */
            je_handle_slot sa[4], sb[4];
            je_handle_table ta, tb;
            if (!je_handle_table_init(&ta, sa, 4u, 3u)
                || !je_handle_table_init(&tb, sb, 4u, 3u))
                fail("JeiOS fatal: s3 survivor tables\r\n");
            /* Use a local pool for this structural proof (the kernel pool is for
             * the ring-3 path). */
            static je_endpoint_pool sp;
            je_endpoint_pool_init(&sp);
            je_handle ea=0, eb=0;
            if (je_endpoint_channel_create(&sp, &ta, JE_EP_RIGHTS_ALL,
                                           &tb, JE_EP_RIGHTS_ALL, &ea, &eb)!=JE_EP_OK)
                fail("JeiOS fatal: s3 survivor create\r\n");
            if (je_endpoint_pool_allocated(&sp)!=1u) fail("JeiOS fatal: s3 survivor not 1\r\n");
            /* Holder of end0 (table ta) 'dies': release its endpoints. */
            if (je_endpoint_release_table(&sp, &ta)!=1) fail("JeiOS fatal: s3 survivor walk!=1\r\n");
            /* Survivor (tb) still holds end1: record stays allocated, end1 is
             * PEER_CLOSED. */
            if (je_endpoint_pool_allocated(&sp)!=1u) fail("JeiOS fatal: s3 survivor freed early\r\n");
            je_ep_channel *rec; uint8_t side;
            if (je_endpoint_resolve(&sp, &tb, eb, &rec, &side)!=JE_EP_OK) fail("JeiOS fatal: s3 survivor resolve\r\n");
            if (je_endpoint_end_state(rec, side)!=JE_EP_END_PEER_CLOSED) fail("JeiOS fatal: s3 survivor not PEER_CLOSED\r\n");
            /* Survivor closes its end -> record freed. */
            if (je_endpoint_release_table(&sp, &tb)!=1) fail("JeiOS fatal: s3 survivor close\r\n");
            if (je_endpoint_pool_allocated(&sp)!=0u) fail("JeiOS fatal: s3 survivor not reclaimed\r\n");
            je_arch_console_write("M3c-s3a: survivor-peer PEER_CLOSED; record held by survivor; reclaimed on its close (kernel self-test)\r\n");

            /* (B) ring-3 endpoint fault-death loop. Pool must start and end at 0,
             * and never climb despite N>POOL_CAP fault/reap cycles. */
            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s3 kpool not 0 at start\r\n");
            for (unsigned iter=0; iter<JE_EP_POOL_CAP+3u; ++iter) {
                uint32_t E=vm_s3_build(PROG_EPFAULT);
                if (je_thread_create_user(&vm_s3, E)!=JE_OK) fail("JeiOS fatal: s3 ep thread\r\n");
                /* Run until E has faulted (its slot is DYING). It creates an
                 * endpoint, closes one end, then reads an unmapped page -> #PF. */
                for (uint64_t sp2=0; !je_sched_has_dying(); ) {
                    (void)je_arch_irq_save();
                    if (++sp2>100000000ull) fail("JeiOS fatal: s3 ep did not fault\r\n");
                    __asm__ volatile("sti; hlt":::"memory");
                }
                /* With IRQs off, read what E recorded before it faulted: create OK,
                 * both packed handle halves nonzero AND distinct (proves the A3-2
                 * pack order/distinctness, not just "nonzero"), and the typed
                 * close of h0 OK (h1 kept the channel alive -> pool is 1). */
                (void)je_arch_irq_save();
                if (vm_s3_scratch(E,0x1200)!=JE_ABI_OK)   fail("JeiOS fatal: s3 ep create not OK\r\n");
                {
                    uint64_t packed=vm_s3_scratch(E,0x1208);
                    uint32_t eh0=(uint32_t)(packed & 0xffffffffu);
                    uint32_t eh1=(uint32_t)(packed >> 32);
                    if (eh0==0u || eh1==0u) fail("JeiOS fatal: s3 ep a packed handle is zero\r\n");
                    if (eh0==eh1)           fail("JeiOS fatal: s3 ep packed handles not distinct\r\n");
                }
                if (vm_s3_scratch(E,0x1210)!=JE_ABI_OK)   fail("JeiOS fatal: s3 ep typed close(h0) not OK\r\n");
                if (je_endpoint_kernel_allocated()!=1u)   fail("JeiOS fatal: s3 ep pool not 1 at fault\r\n");
                /* E died by a user #PF on the unmapped page, not some other kill. */
                {
                    uint64_t Ev,Ee,Er,Ec,Ecr2;
                    if (je_sched_death_record(&vm_s3,E,&Ev,&Ee,&Er,&Ec,&Ecr2)!=JE_OK)
                        fail("JeiOS fatal: s3 ep no death record\r\n");
                    if (Ev!=14u || Ec!=0x33u) fail("JeiOS fatal: s3 ep not a user #PF\r\n");
                }
                /* Reap the faulted process; the release walk (under the IRQ
                 * critical section in je_vm_destroy) must drop the surviving h1
                 * and free the channel -> pool back to baseline 0. Exactly ONE
                 * process (E) is DYING here, so reap_dead must report 1 — proving
                 * the live peer B was not touched. */
                if (je_sched_reap_dead()!=1) fail("JeiOS fatal: s3 ep reap != 1\r\n");
                if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s3 ep pool leak after reap\r\n");
            }
            je_arch_console_write("M3c-s3b: ring-3 endpoint_create + typed close; fault-death reap reclaims the channel; pool baseline held across >POOL_CAP cycles\r\n");
        }

        /* --- M3c s4: non-blocking data IPC (try_send/try_recv), ring-3
         * self-loopback (both ends in one process's table). Proves copy-in/queue/
         * direction/copy-out rollback/SHORT_BUFFER/QUEUE_FULL/PEER_CLOSED/rights.
         * NOT cross-address-space delivery (one process holds both ends). */
        {
            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s4 kpool not 0\r\n");
            uint32_t P=vm_s3_build(PROG_EPIPC);
            uint64_t pd0=je_sched_dispatches_of(&vm_s3,P);
            if (je_thread_create_user(&vm_s3, P)!=JE_OK) fail("JeiOS fatal: s4 ipc thread\r\n");
            /* It runs the whole IPC script then loops on the heartbeat (no fault),
             * so wait for enough dispatches to guarantee completion. */
            for (uint64_t sp=0; je_sched_dispatches_of(&vm_s3,P)-pd0<JE_THREAD_MAX*4; ) {
                (void)je_arch_irq_save();
                if (++sp>100000000ull) fail("JeiOS fatal: s4 ipc no progress\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            (void)je_arch_irq_save();
            /* Verify the recorded results (offsets match the stub). */
            if (vm_s3_scratch(P,0x1380)!=JE_ABI_OK)          fail("JeiOS fatal: s4 create not OK\r\n");
            if (vm_s3_scratch(P,0x1388)!=JE_ABI_OK)          fail("JeiOS fatal: s4 send not OK\r\n");
            if (vm_s3_scratch(P,0x1390)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 recv(h0) not EMPTY (direction)\r\n");
            if (vm_s3_scratch(P,0x1398)!=JE_ABI_H_SHORT_BUFFER) fail("JeiOS fatal: s4 not SHORT_BUFFER\r\n");
            if (vm_s3_scratch(P,0x13a0)!=8u)                 fail("JeiOS fatal: s4 short-buffer wrong len\r\n");
            if (vm_s3_scratch(P,0x13a8)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 bad-dst recv not INVALID\r\n");
            if (vm_s3_scratch(P,0x13b0)!=JE_ABI_OK)          fail("JeiOS fatal: s4 recv not OK\r\n");
            if (vm_s3_scratch(P,0x13b8)!=8u)                 fail("JeiOS fatal: s4 recv wrong len\r\n");
            if (vm_s3_scratch(P,0x13c0)!=0xdeadbeef11223344ull) fail("JeiOS fatal: s4 payload mismatch (survived faulted copy-out)\r\n");
            if (vm_s3_scratch(P,0x13c8)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 not delivered exactly once\r\n");
            if (vm_s3_scratch(P,0x13d0)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 bad-src send not INVALID\r\n");
            if (vm_s3_scratch(P,0x13d8)!=JE_ABI_H_EMPTY)     fail("JeiOS fatal: s4 bad send enqueued something\r\n");
            if (vm_s3_scratch(P,0x13e0)!=JE_ABI_OK)          fail("JeiOS fatal: s4 fill send not OK\r\n");
            if (vm_s3_scratch(P,0x13e8)!=JE_ABI_H_QUEUE_FULL) fail("JeiOS fatal: s4 not QUEUE_FULL\r\n");
            if (vm_s3_scratch(P,0x13f0)!=JE_ABI_INVALID)     fail("JeiOS fatal: s4 cap_count send not INVALID (data-only)\r\n");
            if (vm_s3_scratch(P,0x13f8)!=JE_ABI_OK)          fail("JeiOS fatal: s4 close(h0) not OK\r\n");
            if (vm_s3_scratch(P,0x1400)!=JE_ABI_OK)          fail("JeiOS fatal: s4 drain-after-peer-close not OK\r\n");
            if (vm_s3_scratch(P,0x1408)!=JE_ABI_H_PEER_CLOSED) fail("JeiOS fatal: s4 drained recv not PEER_CLOSED\r\n");
            if (vm_s3_scratch(P,0x1410)!=JE_ABI_H_PEER_CLOSED) fail("JeiOS fatal: s4 send to closed peer not PEER_CLOSED\r\n");
            /* Tear down P; its channel (h1 still live) must be reclaimed. */
            if (je_sched_drop_user(&vm_s3, P)!=JE_OK) fail("JeiOS fatal: s4 ipc drop\r\n");
            if (vm_s3_destroy(P)!=JE_OK) fail("JeiOS fatal: s4 ipc destroy\r\n");
            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s4 ipc pool leak\r\n");
            je_arch_console_write("M3c-s4: ring-3 try_send/try_recv loopback OK (direction, SHORT_BUFFER, copy-out failure leaves msg queued -> delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED); channel reclaimed on exit\r\n");
        }

        /* Tear down B; all step-3/4/5a pages must be back to the pre-s4 baseline. */
        (void)je_arch_irq_save();
        if (je_sched_drop_user(&vm_s3, B)!=JE_OK) fail("JeiOS fatal: s4 drop B\r\n");
        if (vm_s3_destroy(B)!=JE_OK) fail("JeiOS fatal: s4 destroy B\r\n");
        if (je_kernel_pages.free_count!=s3_free_before) fail("JeiOS fatal: s4 page leak\r\n");
        je_arch_console_write("M3a-s4: peer B torn down; page accounting restored\r\n");

        /* --- M3c s5: cross-TABLE capability transfer (ADR 0009 A5), proven as a
         * KERNEL self-test over two distinct process tables. A full ring-3
         * two-process transfer needs cooperative scheduling of two user tasks
         * exchanging over a seeded channel; that multi-process choreography is the
         * explicit subject of step 7 (the bootstrap fixture), so the ring-3
         * cross-process proof is deferred there (recorded decision — see the s5
         * ledger note / ADR A5-4 flex). Here we prove the transfer MECHANISM
         * deterministically: seed S across procs[XA] and procs[XB]'s tables, mint
         * a second channel C in XA, transfer C.e1 as a cap over S, and mint it
         * into XB's OWN table via the real send/recv cap path — asserting the
         * minted handle lives in the DISTINCT XB table and names C.e1. */
        {
            uint32_t XA=vm_s3_build(PROG_HEARTBEAT);   /* just need the two tables */
            uint32_t XB=vm_s3_build(PROG_HEARTBEAT);
            je_handle sA=0, sB=0;
            if (je_endpoint_kern_seed(&vm_s3, XA, XB, &sA, &sB)!=JE_EP_OK)
                fail("JeiOS fatal: s5 seed\r\n");
            if (sA==0u || sB==0u) fail("JeiOS fatal: s5 seed zero handle\r\n");
            if (je_endpoint_kernel_allocated()!=1u) fail("JeiOS fatal: s5 seed pool != 1\r\n");
            /* XA mints a second channel C (both ends in XA's table). */
            je_handle c0=0, c1=0;
            if (je_endpoint_kern_create(&vm_s3, XA, &c0, &c1)!=JE_EP_OK)
                fail("JeiOS fatal: s5 create C\r\n");
            if (je_endpoint_kernel_allocated()!=2u) fail("JeiOS fatal: s5 pool != 2\r\n");
            /* XA resolves c1 -> a send-cap descriptor (TRANSFER + full rights). */
            je_ep_capdesc d;
            if (je_endpoint_kern_resolve_send_cap(&vm_s3, XA, c1, JE_EP_RIGHTS_ALL, &d)!=JE_EP_OK)
                fail("JeiOS fatal: s5 resolve send cap\r\n");
            /* XA sends the cap over sA (into XB's inbound on sB). XA KEEPS c1 for
             * now: under the frozen D2, closing the last handle to C.e1 would mark
             * it CLOSED and make the queued cap dead (A5-2) — so, exactly as the
             * review requires, the sender retains its end until the receiver has
             * minted. (The stale-cap/dead path is proven separately in hosted test
             * 23.) */
            if (je_endpoint_kern_send_caps(&vm_s3, XA, sA, (void*)0, 0, &d, 1)!=JE_EP_OK)
                fail("JeiOS fatal: s5 send cap\r\n");
            /* XB receives on sB: peek the cap, mint it into XB's table. */
            je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; uint8_t nc=0; size_t rl=0;
            je_ep_recv_lease lz;
            if (je_endpoint_kern_recv_peek_caps(&vm_s3, XB, sB, (void*)0, 0, &rl,
                                                JE_EP_MSG_CAPS_MAX, rc, &nc, &lz)!=JE_EP_OK)
                fail("JeiOS fatal: s5 B peek\r\n");
            if (nc!=1u) fail("JeiOS fatal: s5 B cap count != 1\r\n");
            if (!je_endpoint_kern_capdesc_live(&rc[0])) fail("JeiOS fatal: s5 B cap not live\r\n");
            je_handle mintedB=0;
            if (je_endpoint_kern_mint_recv_cap(&vm_s3, XB, &rc[0], &mintedB)!=JE_EP_OK || mintedB==0u)
                fail("JeiOS fatal: s5 B mint\r\n");
            if (je_endpoint_kern_recv_commit(&lz)!=JE_EP_OK) fail("JeiOS fatal: s5 B commit\r\n");
            /* NOW XA closes its own c1 (B has minted its own handle to C.e1, so
             * the end stays live via B's handle_ref — a legitimate co-owner). */
            if (je_endpoint_kern_close(&vm_s3, XA, c1)!=JE_EP_OK)
                fail("JeiOS fatal: s5 close c1\r\n");
            /* The minted handle must resolve in XB's OWN table (cross-table!) and
             * name the SAME end as the original c1. XB sends a byte on it; XA
             * receives it on c0 — a genuine cross-address-space message. */
            {
                uint8_t msg[4]={0x42,0x42,0x42,0x42};
                if (je_endpoint_kern_send(&vm_s3, XB, mintedB, msg, 4)!=JE_EP_OK)
                    fail("JeiOS fatal: s5 B send on minted\r\n");
                uint8_t rb[JE_EP_MSG_MAX]; size_t bl=0; je_ep_recv_lease lz2;
                if (je_endpoint_kern_recv_peek(&vm_s3, XA, c0, rb, sizeof rb, &bl, &lz2)!=JE_EP_OK)
                    fail("JeiOS fatal: s5 A recv on c0\r\n");
                if (bl!=4u || rb[0]!=0x42) fail("JeiOS fatal: s5 A wrong payload across tables\r\n");
                if (je_endpoint_kern_recv_commit(&lz2)!=JE_EP_OK) fail("JeiOS fatal: s5 A commit\r\n");
            }
            /* Tear down both procs (no user thread was created — built only for
             * their tables). S + C must reclaim to baseline 0 via the release
             * walk in je_vm_destroy. */
            if (vm_s3_destroy(XA)!=JE_OK) fail("JeiOS fatal: s5 XA destroy\r\n");
            if (vm_s3_destroy(XB)!=JE_OK) fail("JeiOS fatal: s5 XB destroy\r\n");
            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s5 pool leak after teardown\r\n");
            je_arch_console_write("M3c-s5: cross-table capability transfer OK (XA mints C, transfers C.e1 to XB via the kernel send/recv cap FORWARDERS over seeded S; XB mints it in its OWN table and messages XA on c0 across tables); S+C reclaimed. [syscall-decode/copy layer exercised by test_syscall decode + hosted tests; ring-3 two-process transfer deferred to step 7]\r\n");
        }

        /* --- M3c step 6 (ADR 0009 A6): blocking recv that TIMES OUT (end-to-end
         * through the real scheduler, ring 3). A single user thread calls
         * recv(h0, timeout) on an end nobody sends to -> it PARKS (BLOCKED_IPC);
         * the timer naturally advances je_timer_ticks past the deadline; the ISR
         * stamps TIMED_OUT via the single arbitrate CAS and makes it READY; the
         * continuation prologue re-enters the recv and returns 14. Deterministic
         * with ONE ring-3 thread + the natural timer tick (no second spinning
         * process — the step-5 spin-hang lesson). Proves: park, timer-ISR terminal
         * transition, restart-site resume, continuation delivery, waiter release. */
        {
            uint32_t R=vm_s3_build(PROG_EPRECV_TO);
            if (je_thread_create_user(&vm_s3, R)!=JE_OK) fail("JeiOS fatal: s6 recv thread\r\n");
            je_arch_console_write("M3c-s6: ring-3 thread blocks on recv(timeout); waiting for timer-driven TIMED_OUT...\r\n");
            /* Run until the thread has written its post-wake recv status (0x1388)
             * AND advanced its liveness counter (0x1390) — i.e. it parked, the
             * timer woke it TIMED_OUT, and the continuation resumed it. */
            uint64_t sp=0;
            for (;;) {
                (void)je_arch_irq_save();
                uint64_t beat=vm_s3_scratch(R, 0x1390);
                if (beat>=2u) break;    /* resumed and looping past the recv */
                if (++sp>200000000ull) fail("JeiOS fatal: s6 no TIMED_OUT wake\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            (void)je_arch_irq_save();
            uint64_t cstat=vm_s3_scratch(R, 0x1380);
            uint64_t rstat=vm_s3_scratch(R, 0x1388);
            if (cstat!=0u) fail("JeiOS fatal: s6 create status != 0\r\n");
            if (rstat!=14u) fail("JeiOS fatal: s6 recv status != TIMED_OUT(14)\r\n");
            if (je_sched_drop_user(&vm_s3, R)!=JE_OK) fail("JeiOS fatal: s6 drop_user\r\n");
            if (vm_s3_destroy(R)!=JE_OK) fail("JeiOS fatal: s6 destroy\r\n");
            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s6 pool leak after teardown\r\n");
            je_arch_console_write("M3c-s6: blocking recv TIMED_OUT end-to-end (ring-3 thread parked, timer ISR stamped the terminal transition, continuation resumed with status 14, waiter released, channel reclaimed). [WOKEN/PEER_CLOSED/CANCELLED proven in the pure hosted tests + arbitrate CAS; the full two-process ring-3 WOKEN handshake rides on step 7's bootstrap fixture]\r\n");
        }

        /* --- M3c step 7 (ADR 0009 D-SEQ step 7): the bootstrap fixture. Two ring-3
         * processes A(PINGER)+B(PONGER) share a kernel-SEEDED channel S; the kernel
         * injects each process's S handle into its arena before its thread starts
         * ("fixture not supervisor"). Discharges the two honest deferrals in ONE
         * end-to-end run: (1) the two-process blocking WOKEN handshake (B parks on
         * recv, A's send wakes it — causally forced: B is created + confirmed
         * parked BEFORE A exists), and (2) the two-process CAPABILITY TRANSFER over
         * the WOKEN continuation (A creates C, sends c1 as a cap; B mints it on
         * wake; A closes c1 BEFORE B uses the minted handle — proving retained/copy
         * ownership; B's post-close send on the minted handle reaches A's c0). Then
         * a bounded 4-round ping-pong and an explicit DONE teardown; both exit 0;
         * the pool reclaims to 0. Deterministic because blocking recv YIELDS (no
         * spin) and the entry-probe+park is one IF=0 path (no lost wakeup). */
        {
            uint32_t pPing=vm_s3_build(PROG_PING);
            uint32_t pPong=vm_s3_build(PROG_PONG);
            if (vm_s3.procs[pPing].root==vm_s3.procs[pPong].root) fail("JeiOS fatal: s7 roots not distinct\r\n");
            /* Seed S across A and B; inject each end's handle at arena 0x2000. */
            je_handle hA=0u, hB=0u;
            if (je_endpoint_kern_seed(&vm_s3, pPing, pPong, &hA, &hB)!=JE_EP_OK)
                fail("JeiOS fatal: s7 seed\r\n");
            if (hA==0u || hB==0u) fail("JeiOS fatal: s7 seed zero handle\r\n");
            if (je_endpoint_kernel_allocated()!=1u) fail("JeiOS fatal: s7 seed pool != 1\r\n");
            vm_s3_scratch_write(pPing, 0x1500, hA);
            vm_s3_scratch_write(pPong, 0x1500, hB);
            /* Create B FIRST and confirm it PARKED on its blocking recv (progress
             * reaches 1 = pre-recv marker, and the final-status slot stays unwritten
             * across K ticks far below the 0x4000 timeout). A non-parking return
             * would write progress>=2 immediately. This makes A's later send a
             * CAUSALLY-FORCED WOKEN (B was blocked before A ever ran). */
            if (je_thread_create_user(&vm_s3, pPong)!=JE_OK) fail("JeiOS fatal: s7 B thread\r\n");
            je_arch_console_write("M3c-s7: B(ponger) created; confirming it parks on blocking recv...\r\n");
            uint64_t t0=je_sched_ticks(); uint64_t sp=0;
            for (;;) {
                (void)je_arch_irq_save();
                if (vm_s3_scratch(pPong, 0x1508)>=1u) break;   /* reached the pre-recv marker */
                if (++sp>200000000ull) fail("JeiOS fatal: s7 B never started\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            /* let >=0x40 ticks pass; B must still be parked (progress stays 1).
             * 0x40 is 32x below the 0x4000 recv timeout and ample (B runs ~3
             * instructions between progress=1 and int 0x80) while staying well
             * inside the smoke time budget. */
            for (;;) {
                (void)je_arch_irq_save();
                if (je_sched_ticks()-t0>=0x40ull) break;
                if (vm_s3_scratch(pPong, 0x1508)!=1u) break;   /* advanced early? checked below */
                __asm__ volatile("sti; hlt":::"memory");
            }
            (void)je_arch_irq_save();
            if (vm_s3_scratch(pPong, 0x1508)!=1u) fail("JeiOS fatal: s7 B did not park (progress advanced pre-A)\r\n");
            /* B is at the pre-recv marker AND still alive (not DYING): it is parked
             * in the blocking recv, not dead-at-progress-1. (je_sched_has_dying is
             * read-only; this is the kernel-state check the review asked for so a
             * faulted B can't masquerade as "parked".) */
            if (je_sched_has_dying()) fail("JeiOS fatal: s7 B died before A (not parked)\r\n");
            je_arch_console_write("M3c-s7: B is parked + alive (progress==1 held, not DYING); creating A(pinger) -> its send is a causally-forced WOKEN\r\n");
            if (je_thread_create_user(&vm_s3, pPing)!=JE_OK) fail("JeiOS fatal: s7 A thread\r\n");
            /* Run until BOTH reach progress 6 (done), or one fails (progress 99), or
             * the watchdog trips (a hang = a real lost-wakeup/deadlock bug -> fail). */
            uint64_t s7t0=je_sched_ticks();
            for (;;) {
                (void)je_arch_irq_save();
                uint64_t pa=vm_s3_scratch(pPing, 0x1508), pb=vm_s3_scratch(pPong, 0x1508);
                if (pa==99u || pb==99u) {
                    uint64_t sa=vm_s3_scratch(pPing, 0x1510), sb=vm_s3_scratch(pPong, 0x1510);
                    je_arch_console_write("JeiOS fatal: s7 stub failed; A.status="); je_arch_console_hex(sa);
                    je_arch_console_write(" B.status="); je_arch_console_hex(sb); je_arch_console_write("\r\n");
                    je_arch_halt();
                }
                if (pa>=6u && pb>=6u) break;
                if (je_sched_ticks()-s7t0>4000u) fail("JeiOS fatal: s7 watchdog (hang: lost wakeup or deadlock)\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            (void)je_arch_irq_save();
            if (vm_s3_scratch(pPing, 0x1510)!=0u) fail("JeiOS fatal: s7 A final status != 0\r\n");
            if (vm_s3_scratch(pPong, 0x1510)!=0u) fail("JeiOS fatal: s7 B final status != 0\r\n");
            /* Both must have reached progress EXACTLY 6 (done), not a stub that
             * fell through past its exit into a hang loop. */
            if (vm_s3_scratch(pPing, 0x1508)!=6u) fail("JeiOS fatal: s7 A progress != 6\r\n");
            if (vm_s3_scratch(pPong, 0x1508)!=6u) fail("JeiOS fatal: s7 B progress != 6\r\n");
            /* progress==6 is written just BEFORE exit(0); let both actually execute
             * the exit syscall (-> DYING) before checking the death record. */
            for (uint64_t ew0=je_sched_ticks(); ; ) {
                (void)je_arch_irq_save();
                uint64_t ra0=99u, ca0=99u, rb0=99u, cb0=99u;
                bool ad = (je_sched_death_reason(&vm_s3, pPing, &ra0, &ca0)==JE_OK);
                bool bd = (je_sched_death_reason(&vm_s3, pPong, &rb0, &cb0)==JE_OK);
                if (ad && bd) break;
                if (je_sched_ticks()-ew0>4000u) fail("JeiOS fatal: s7 processes did not exit\r\n");
                __asm__ volatile("sti; hlt":::"memory");
            }
            /* Both exited cleanly via exit(0): DYING with DEATH_EXIT, code 0 (not a
             * fault or bad-return). Confirms the clean teardown, not a crash. */
            (void)je_arch_irq_save();
            uint64_t ra=99u, ca=99u, rb=99u, cb=99u;
            if (je_sched_death_reason(&vm_s3, pPing, &ra, &ca)!=JE_OK || ra!=JE_DEATH_EXIT || ca!=0u)
                fail("JeiOS fatal: s7 A not a clean exit(0)\r\n");
            if (je_sched_death_reason(&vm_s3, pPong, &rb, &cb)!=JE_OK || rb!=JE_DEATH_EXIT || cb!=0u)
                fail("JeiOS fatal: s7 B not a clean exit(0)\r\n");
            /* Both exited 0. Reap the DYING processes; pool must return to 0. */
            for (uint64_t r=0; je_sched_has_dying(); ) {
                (void)je_arch_irq_save();
                if (je_sched_reap_dead()<0) fail("JeiOS fatal: s7 reap\r\n");
                if (++r>1000000ull) fail("JeiOS fatal: s7 reap stuck\r\n");
                if (!je_sched_has_dying()) break;
                __asm__ volatile("sti; hlt":::"memory");
            }
            (void)je_arch_irq_save();
            if (je_endpoint_kernel_allocated()!=0u) fail("JeiOS fatal: s7 pool leak after teardown\r\n");
            je_arch_console_write("M3c-s7: bootstrap fixture OK (two ring-3 processes, kernel-seeded channel; B parked -> A's cap-carrying send WOKE it via the continuation; B minted the transferred end; A closed c1 BEFORE B used the mint; B's post-close send reached A's c0 (retained ownership); 4-round ping-pong; DONE teardown; both exited 0; pool reclaimed). The step-5 two-process transfer AND step-6 two-process WOKEN deferrals are now discharged end-to-end.\r\n");
        }
        /* --- M4b-s1: PROCESS record status and typed dispatch. B exits first;
         * A owns two PROCESS handles with different rights. The bootstrap reaps
         * B between A's two status calls, proving the record outlives its VM slot. */
        {
            if (process_pool.used_count != 0u)
                fail("JeiOS fatal: M4b process baseline\r\n");
            uint32_t child = vm_s3_build_quota(PROG_PROC_EXIT7, JE_EP_CHILD_QUOTA_MAX);
            uint32_t parent = vm_s3_build_quota(PROG_M4B, 1u);
            je_handle inspect_kill = 0u, kill_only = 0u;
            uint64_t mint_flags = je_arch_irq_save();
            je_status mint_a = je_process_handle_mint(
                &vm_s3, parent, vm_s3_proc_ids[child],
                JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL, &inspect_kill,
                (je_handle_status *)0);
            je_status mint_b = je_process_handle_mint(
                &vm_s3, parent, vm_s3_proc_ids[child], JE_ABI_RIGHT_KILL,
                &kill_only, (je_handle_status *)0);
            je_arch_irq_restore(mint_flags);
            if (mint_a != JE_OK || mint_b != JE_OK)
                fail("JeiOS fatal: M4b process handle mint\r\n");
            vm_s3_scratch_write(parent, 0x1600u, inspect_kill);
            vm_s3_scratch_write(parent, 0x1608u, kill_only);
            /* Baseline sampled BEFORE the child can run (round-4 review nit). */
            uint64_t child_pool_flags = je_arch_irq_save();
            uint32_t child_pool_before = je_endpoint_kernel_allocated();
            je_arch_irq_restore(child_pool_flags);
            if (je_thread_create_user(&vm_s3, child) != JE_OK)
                fail("JeiOS fatal: M4b child thread\r\n");
            bool child_quota_checked = false;
            for (uint64_t child_spins = 0u; ; ) {
                uint32_t child_state = JE_PROC_FREE;
                uint32_t child_reason = 0u;
                uint64_t child_code = 0u;
                uint64_t child_flags = je_arch_irq_save();
                je_status child_status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[child], &child_state,
                    &child_reason, &child_code);
                je_arch_irq_restore(child_flags);
                if (child_status != JE_OK)
                    fail("JeiOS fatal: M4b child status\r\n");
                if (child_state == JE_PROC_DYING && !child_quota_checked) {
                    if (vm_s3_scratch(child, 0x1768u) != 7u)
                        fail("JeiOS fatal: M4b child endpoint quota not H_QUOTA\r\n");
                    child_pool_flags = je_arch_irq_save();
                    uint32_t child_pool_after = je_endpoint_kernel_allocated();
                    je_arch_irq_restore(child_pool_flags);
                    if (child_pool_after != child_pool_before)
                        fail("JeiOS fatal: M4b child quota changed endpoint pool\r\n");
                    child_quota_checked = true;
                }
                if (child_state == JE_PROC_DEAD) {
                    if (!child_quota_checked)
                        fail("JeiOS fatal: M4b child reaped before quota probe\r\n");
                    break;
                }
                if (child_state != JE_PROC_LIVE && child_state != JE_PROC_DYING)
                    fail("JeiOS fatal: M4b child reached invalid state\r\n");
                if (++child_spins > 100000000ull)
                    fail("JeiOS fatal: M4b child did not exit\r\n");
                je_kernel_idle_step();
            }
            if (je_thread_create_user(&vm_s3, parent) != JE_OK)
                fail("JeiOS fatal: M4b parent thread\r\n");
            for (uint64_t parent_progress_spins = 0u; ; ) {
                /* Lead fix (round-4 review, Claude P3): read progress and the record
                 * status in ONE IF=0 section. Otherwise the parent can write progress
                 * 1 and exit between the two reads, and a correct run is reported as
                 * "died before progress 1". */
                uint32_t parent_state = JE_PROC_FREE;
                uint32_t parent_reason = 0u;
                uint64_t parent_code = 0u;
                uint64_t parent_flags = je_arch_irq_save();
                uint64_t progress = vm_s3_scratch(parent, 0x1610u);
                je_status parent_status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[parent], &parent_state,
                    &parent_reason, &parent_code);
                je_arch_irq_restore(parent_flags);
                if (progress == 99u) fail("JeiOS fatal: M4b parent assertion\r\n");
                if (progress >= 1u) break;
                (void)parent_reason;
                (void)parent_code;
                if (parent_status != JE_OK)
                    fail("JeiOS fatal: M4b parent record invalid before progress 1\r\n");
                if (parent_state == JE_PROC_DYING || parent_state == JE_PROC_DEAD)
                    fail("JeiOS fatal: M4b parent died before progress 1\r\n");
                if (++parent_progress_spins > 100000000ull)
                    fail("JeiOS fatal: M4b parent did not reach progress 1\r\n");
                je_kernel_idle_step();
            }
            for (uint64_t parent_progress_spins = 0u; ; ) {
                /* Lead fix (round-4 review, Claude P3): read progress and the record
                 * status in ONE IF=0 section. Otherwise the parent can write progress
                 * 2 and exit between the two reads, and a correct run is reported as
                 * "died before progress 2". */
                uint32_t parent_state = JE_PROC_FREE;
                uint32_t parent_reason = 0u;
                uint64_t parent_code = 0u;
                uint64_t parent_flags = je_arch_irq_save();
                uint64_t progress = vm_s3_scratch(parent, 0x1610u);
                je_status parent_status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[parent], &parent_state,
                    &parent_reason, &parent_code);
                je_arch_irq_restore(parent_flags);
                if (progress == 99u) fail("JeiOS fatal: M4b post-reap assertion\r\n");
                if (progress >= 2u) break;
                (void)parent_reason;
                (void)parent_code;
                if (parent_status != JE_OK)
                    fail("JeiOS fatal: M4b parent record invalid before progress 2\r\n");
                if (parent_state == JE_PROC_DYING || parent_state == JE_PROC_DEAD)
                    fail("JeiOS fatal: M4b parent died before progress 2\r\n");
                if (++parent_progress_spins > 100000000ull)
                    fail("JeiOS fatal: M4b parent did not reach progress 2\r\n");
                je_kernel_idle_step();
            }
            uint32_t parent_death_state = JE_PROC_FREE;
            uint32_t parent_death_reason = 0u;
            uint64_t parent_death_code = 0u;
            for (uint64_t parent_death_spins = 0u; ; ) {
                uint64_t parent_flags = je_arch_irq_save();
                je_status parent_status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[parent], &parent_death_state,
                    &parent_death_reason, &parent_death_code);
                je_arch_irq_restore(parent_flags);
                if (parent_status != JE_OK)
                    fail("JeiOS fatal: M4b parent record unavailable before reap\r\n");
                if (parent_death_state == JE_PROC_DYING) break;
                if (parent_death_state == JE_PROC_DEAD)
                    fail("JeiOS fatal: M4b parent reaped before death snapshot\r\n");
                if (parent_death_state != JE_PROC_LIVE)
                    fail("JeiOS fatal: M4b parent reached invalid death state\r\n");
                if (++parent_death_spins > 100000000ull)
                    fail("JeiOS fatal: M4b parent did not reach DYING\r\n");
                je_kernel_idle_step();
            }
            if (parent_death_reason != JE_DEATH_EXIT || parent_death_code != 0u)
                fail("JeiOS fatal: M4b parent death was not EXIT(0)\r\n");
            uint64_t parent_spins = 0u;
            for (;;) {
                uint64_t used_flags = je_arch_irq_save();
                uint32_t used = process_pool.used_count;
                je_arch_irq_restore(used_flags);
                if (used == 0u) break;
                if (++parent_spins > 100000000ull)
                    fail("JeiOS fatal: M4b parent did not reap\r\n");
                je_kernel_idle_step();
            }
            if (je_endpoint_kernel_allocated() != 0u || process_pool.used_count != 0u)
                fail("JeiOS fatal: M4b pool baseline not restored\r\n");
            je_arch_console_write("M4b-s1: endpoint creation quota/charge release OK\r\n");
            je_arch_console_write("M4b-s1: PROCESS record status/typed dispatch OK (Dying/Dead, rights, attenuation, stale close, endpoint query/dup)\r\n");
        }
        /* --- M4c-s1: startup v1, launcher-gated spawn, clock, and journal
         * rollback. The parent publishes explicit phase/ack handshakes so each
         * baseline is observed while the parent is parked and IF=0. */
        {
            if (process_pool.used_count != 0u || je_endpoint_kernel_allocated() != 0u)
                fail("JeiOS fatal: M4c baseline\r\n");
            uint32_t spawner = vm_s3_build_quota(PROG_M4C, 1u);
            je_handle launcher = 0u;
            uint64_t launcher_flags = je_arch_irq_save();
            je_handle_status launcher_alloc = je_handle_alloc(
                &vm_s3.procs[spawner].htable, (void *)0, JE_ABI_OBJ_LAUNCHER,
                JE_ABI_RIGHT_SPAWN, &launcher);
            je_arch_irq_restore(launcher_flags);
            if (launcher_alloc != JE_HANDLE_OK || launcher == 0u)
                fail("JeiOS fatal: M4c launcher mint\r\n");
            je_user_startup startup = {
                JE_ABI_STARTUP_V1, 1u, launcher, 0u, 0u, 0u
            };
            if (je_thread_create_user_start(&vm_s3, spawner, &startup,
                                            (uint32_t *)0) != JE_OK)
                fail("JeiOS fatal: M4c spawner thread\r\n");

            bool child_audited = false;
            uint32_t audited_child = JE_VM_MAX_PROCESSES;
            uint64_t m4c_wait_start = je_sched_ticks();
            for (;;) {
                uint64_t m4c_progress_flags = je_arch_irq_save();
                for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
                    je_proc_rec *rec = &process_pool.rec[i];
                    if ((rec->state == JE_PROC_LIVE || rec->state == JE_PROC_DYING)
                        && rec->run_linked
                        && rec->vm_index < JE_VM_MAX_PROCESSES
                        && i != vm_s3_proc_ids[spawner].index) {
                        if (!child_audited) {
                            if (vm_s3_scratch(rec->vm_index, 0x1b18u) == 0u)
                                continue;
                            if (je_vm_audit(&vm_s3, rec->vm_index) != JE_OK)
                                fail("JeiOS fatal: M4c spawned child audit\r\n");
                            if (vm_s3_scratch(rec->vm_index, 0x1830u) == 99u)
                                fail_m4c_phase("JeiOS fatal: M4c child startup/rights phase=",
                                               rec->vm_index, 0x1b18u);
                            vm_s3_scratch_write(rec->vm_index, 0x1b08u, 2u);
                            audited_child = rec->vm_index;
                            child_audited = true;
                        }
                    }
                }
                uint64_t progress = vm_s3_scratch(spawner, 0x1b00u);
                uint64_t death_reason = 0u;
                uint64_t death_code = 0u;
                bool spawner_died = je_sched_death_reason(
                    &vm_s3, spawner, &death_reason, &death_code) == JE_OK;
                je_arch_irq_restore(m4c_progress_flags);
                if (progress == 99u)
                    fail("JeiOS fatal: M4c ring-3 fixture assertion\r\n");
                if (child_audited) break;
                if (spawner_died)
                    fail_m4c_phase("JeiOS fatal: M4c spawner died before progress phase=",
                                   spawner, 0x1b10u);
                if (je_sched_ticks() - m4c_wait_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c spawner progress timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }
            if (!child_audited)
                fail("JeiOS fatal: M4c child was not audited before reap\r\n");
            if (audited_child >= JE_VM_MAX_PROCESSES)
                fail("JeiOS fatal: M4c child index missing\r\n");
            uint64_t child_reap_start = je_sched_ticks();
            for (;;) {
                uint64_t child_reap_flags = je_arch_irq_save();
                bool child_present = je_sched_has_process(&vm_s3, audited_child);
                je_arch_irq_restore(child_reap_flags);
                if (!child_present) break;
                if (je_sched_ticks() - child_reap_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c audited child reap timeout phase=",
                                   audited_child, 0x1b18u);
                je_kernel_idle_step();
            }

            uint64_t packed = vm_s3_scratch(spawner, 0x1820u);
            je_handle source_a = (je_handle)(packed & UINT64_C(0xffffffff));
            je_handle source_b = (je_handle)(packed >> 32);
            je_handle grant_handles[2] = {source_a, source_b};
            je_abi_spawn_grant valid_grants[2] = {
                { (uint32_t)source_a, JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_INSPECT },
                { (uint32_t)source_b, JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV
                                     | JE_ABI_RIGHT_INSPECT }
            };
            m4c_baseline negative_baseline;

            /* Phase 6 is before every ring-3 negative spawn. The parent waits
             * for the kernel to take this baseline and acknowledge it; phase 7
             * is the reciprocal point after all those calls and before the
             * parent restores the valid grants. */
            uint64_t negative_wait_start = je_sched_ticks();
            for (;;) {
                uint64_t phase_flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(spawner, 0x1b10u);
                if (phase == 6u) {
                    m4c_take_baseline(spawner, grant_handles, &negative_baseline);
                    vm_s3_scratch_write(spawner, 0x1b20u, 1u);
                    je_arch_irq_restore(phase_flags);
                    break;
                }
                bool phase_died = je_sched_death_reason(
                    &vm_s3, spawner, (uint64_t[1]){0u},
                    (uint64_t[1]){0u}) == JE_OK;
                je_arch_irq_restore(phase_flags);
                if (phase == 99u)
                    fail("JeiOS fatal: M4c negative fixture assertion\r\n");
                if (phase_died)
                    fail_m4c_phase("JeiOS fatal: M4c spawner died before negatives phase=",
                                   spawner, 0x1b10u);
                if (je_sched_ticks() - negative_wait_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c negative probe timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }
            uint64_t negative_done_start = je_sched_ticks();
            for (;;) {
                uint64_t phase_flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(spawner, 0x1b10u);
                if (phase == 7u) {
                    bool unchanged = m4c_baseline_matches(
                        spawner, grant_handles, &negative_baseline);
                    vm_s3_scratch_write(spawner, 0x1b20u, 2u);
                    je_arch_irq_restore(phase_flags);
                    if (!unchanged)
                        fail("JeiOS fatal: M4c negative spawn changed baseline\r\n");
                    break;
                }
                bool phase_died = je_sched_death_reason(
                    &vm_s3, spawner, (uint64_t[1]){0u},
                    (uint64_t[1]){0u}) == JE_OK;
                je_arch_irq_restore(phase_flags);
                if (phase == 99u)
                    fail("JeiOS fatal: M4c negative fixture assertion\r\n");
                if (phase_died)
                    fail_m4c_phase("JeiOS fatal: M4c spawner died after negatives phase=",
                                   spawner, 0x1b10u);
                if (je_sched_ticks() - negative_done_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c negative completion timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }

            /* Phase 9 is the quota-invalid call. Arm step 1 before the
             * acknowledgment: INVALID must be returned before the journal and
             * therefore the fired counter must remain zero. */
            m4c_baseline quota_baseline;
            uint64_t quota_wait_start = je_sched_ticks();
            for (;;) {
                uint64_t phase_flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(spawner, 0x1b10u);
                if (phase == 9u) {
                    m4c_take_baseline(spawner, grant_handles, &quota_baseline);
                    je_spawn_failpoint_set(1u, 0u);
                    vm_s3_scratch_write(spawner, 0x1b20u, 1u);
                    je_arch_irq_restore(phase_flags);
                    break;
                }
                if (phase == 99u) {
                    je_arch_irq_restore(phase_flags);
                    fail("JeiOS fatal: M4c quota fixture assertion\r\n");
                }
                je_arch_irq_restore(phase_flags);
                if (je_sched_ticks() - quota_wait_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c quota probe timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }
            uint64_t quota_done_start = je_sched_ticks();
            for (;;) {
                uint64_t phase_flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(spawner, 0x1b10u);
                if (phase == 10u) {
                    uint64_t quota_status = vm_s3_scratch(spawner, 0x18d0u);
                    bool quota_unchanged = m4c_baseline_matches(
                        spawner, grant_handles, &quota_baseline);
                    uint32_t quota_fired = je_spawn_failpoint_fired();
                    vm_s3_scratch_write(spawner, 0x1b20u, 2u);
                    je_arch_irq_restore(phase_flags);
                    if (quota_status != JE_ABI_INVALID || quota_fired != 0u
                        || !quota_unchanged)
                        fail("JeiOS fatal: M4c quota validation order\r\n");
                    break;
                }
                if (phase == 99u) {
                    je_arch_irq_restore(phase_flags);
                    fail("JeiOS fatal: M4c quota fixture assertion\r\n");
                }
                je_arch_irq_restore(phase_flags);
                if (je_sched_ticks() - quota_done_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c quota completion timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }

            /* The parent now parks in phase 8/hold. Keep IF=0 from the
             * synchronized hold observation through every fail-Nth case and
             * both unfired-arm success probes. */
            uint64_t hold_wait_start = je_sched_ticks();
            uint64_t fail_matrix_flags = 0u;
            m4c_baseline baseline;
            for (;;) {
                uint64_t phase_flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(spawner, 0x1b10u);
                uint64_t hold = vm_s3_scratch(spawner, 0x1b00u);
                if (phase == 8u && hold == 1u) {
                    m4c_take_baseline(spawner, grant_handles, &baseline);
                    fail_matrix_flags = phase_flags;
                    break;
                }
                je_arch_irq_restore(phase_flags);
                if (phase == 99u)
                    fail("JeiOS fatal: M4c hold fixture assertion\r\n");
                if (je_sched_ticks() - hold_wait_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c parent hold timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }

            for (uint32_t step = 1u; step <= 5u; ++step) {
                je_spawn_failpoint_set(step, 0u);
                je_handle failed = 0u;
                je_abi_status status = je_proc_spawn(
                    &vm_s3, spawner, launcher, 0u, valid_grants, 2u, 8u,
                    JE_EP_CHILD_QUOTA_MAX, &failed);
                if (status != JE_ABI_H_NOSLOTS || failed != 0u
                    || je_spawn_failpoint_fired() != 1u
                    || !m4c_baseline_matches(spawner, grant_handles, &baseline))
                    fail("JeiOS fatal: M4c fail-Nth rollback\r\n");
            }
            for (uint32_t grant = 0u; grant < 2u; ++grant) {
                je_spawn_failpoint_set(3u, grant);
                je_handle failed = 0u;
                je_abi_status status = je_proc_spawn(
                    &vm_s3, spawner, launcher, 0u, valid_grants, 2u, 8u,
                    JE_EP_CHILD_QUOTA_MAX, &failed);
                if (status != JE_ABI_H_NOSLOTS || failed != 0u
                    || je_spawn_failpoint_fired() != 1u
                    || !m4c_baseline_matches(spawner, grant_handles, &baseline))
                    fail("JeiOS fatal: M4c grant rollback\r\n");
            }

            je_spawn_failpoint_set(3u, 2u);
            je_handle unfired_grant = 0u;
            je_abi_status unfired_grant_status = je_proc_spawn(
                &vm_s3, spawner, launcher, 0u, valid_grants, 2u, 8u,
                JE_EP_CHILD_QUOTA_MAX, &unfired_grant);
            if (unfired_grant_status != JE_ABI_OK || unfired_grant == 0u
                || je_spawn_failpoint_fired() != 0u)
                fail("JeiOS fatal: M4c unfired grant failpoint\r\n");
            m4c_discard_spawn(spawner, unfired_grant);
            if (!m4c_baseline_matches(spawner, grant_handles, &baseline))
                fail("JeiOS fatal: M4c unfired grant baseline\r\n");

            /* Lead fix (Codex mutant #6): prove spawn DISARMS an unfired failpoint at the
             * end of every call. Arm grant index 1, spawn with only ONE grant (so it
             * cannot fire), then spawn with TWO grants WITHOUT re-arming. If the
             * end-of-call disarm were missing, the second spawn would fail at grant 1. */
            je_spawn_failpoint_set(3u, 1u);
            je_handle disarm_a = 0u;
            if (je_proc_spawn(&vm_s3, spawner, launcher, 0u, valid_grants, 1u, 8u,
                              JE_EP_CHILD_QUOTA_MAX, &disarm_a) != JE_ABI_OK
                || disarm_a == 0u || je_spawn_failpoint_fired() != 0u)
                fail("JeiOS fatal: M4c disarm probe (1 grant)\r\n");
            m4c_discard_spawn(spawner, disarm_a);
            je_handle disarm_b = 0u;
            if (je_proc_spawn(&vm_s3, spawner, launcher, 0u, valid_grants, 2u, 8u,
                              JE_EP_CHILD_QUOTA_MAX, &disarm_b) != JE_ABI_OK
                || disarm_b == 0u || je_spawn_failpoint_fired() != 0u)
                fail("JeiOS fatal: M4c failpoint not disarmed after spawn\r\n");
            m4c_discard_spawn(spawner, disarm_b);
            if (!m4c_baseline_matches(spawner, grant_handles, &baseline))
                fail("JeiOS fatal: M4c disarm probe baseline\r\n");

            je_spawn_failpoint_set(9u, 0u);
            je_handle unfired_step = 0u;
            je_abi_status unfired_step_status = je_proc_spawn(
                &vm_s3, spawner, launcher, 0u, valid_grants, 2u, 8u,
                JE_EP_CHILD_QUOTA_MAX, &unfired_step);
            if (unfired_step_status != JE_ABI_OK || unfired_step == 0u
                || je_spawn_failpoint_fired() != 0u)
                fail("JeiOS fatal: M4c unfired step failpoint\r\n");
            m4c_discard_spawn(spawner, unfired_step);
            if (!m4c_baseline_matches(spawner, grant_handles, &baseline))
                fail("JeiOS fatal: M4c unfired step baseline\r\n");

            je_spawn_failpoint_set(0u, 0u);
            uint64_t spawn_cycles_start = m4c_rdtsc_start();
            je_handle measured_handle = 0u;
            je_abi_status measured_status = je_proc_spawn(
                &vm_s3, spawner, launcher, 0u, valid_grants, 2u, 8u,
                JE_EP_CHILD_QUOTA_MAX, &measured_handle);
            uint64_t spawn_cycles_end = m4c_rdtsc_end();
            if (measured_status != JE_ABI_OK || measured_handle == 0u)
                fail("JeiOS fatal: M4c measured spawn\r\n");
            uint32_t measured_rights = 0u;
            if (je_handle_rights_of(&vm_s3.procs[spawner].htable,
                                    measured_handle, JE_ABI_OBJ_PROCESS,
                                    JE_ABI_RIGHT_INSPECT, &measured_rights)
                    != JE_HANDLE_OK
                || measured_rights != (JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL))
                fail("JeiOS fatal: M4c measured PROCESS rights\r\n");
            m4c_discard_spawn(spawner, measured_handle);
            if (!m4c_baseline_matches(spawner, grant_handles, &baseline))
                fail("JeiOS fatal: M4c measured spawn baseline\r\n");

            vm_s3_scratch_write(spawner, 0x1b20u, 3u);
            je_arch_irq_restore(fail_matrix_flags);
            uint64_t m4c_reap_start = je_sched_ticks();
            for (;;) {
                uint64_t m4c_reap_flags = je_arch_irq_save();
                bool present = je_sched_has_process(&vm_s3, spawner);
                je_arch_irq_restore(m4c_reap_flags);
                if (!present) break;
                if (je_sched_ticks() - m4c_reap_start > 200u)
                    fail_m4c_phase("JeiOS fatal: M4c parent reap timeout phase=",
                                   spawner, 0x1b10u);
                je_kernel_idle_step();
            }
            if (je_endpoint_kernel_allocated() != 0u || process_pool.used_count != 0u)
                fail("JeiOS fatal: M4c final pool baseline\r\n");
            je_arch_console_write("M4c-s1: spawn/LAUNCHER/startup-v1/CLOCK_TICKS OK; negative baselines OK; quota validation order OK; fail-Nth rollback OK (steps 1-5); spawn cycles=");
            je_arch_console_hex(spawn_cycles_end - spawn_cycles_start);
            je_arch_console_write("\r\n");
        }
        /* --- M4d-s1: synchronous PROC_KILL. The bootstrap is the fixture
         * coordinator; all ring-3 authority is held by K in PROCESS and endpoint
         * handles minted through the existing kernel-only helpers. Reaping is
         * performed only by je_kernel_idle_step. */
        {
            if (process_pool.used_count != 0u || je_endpoint_kernel_allocated() != 0u)
                fail("JeiOS fatal: M4d baseline\r\n");
            uint32_t killer = vm_s3_build_quota(PROG_M4D, JE_EP_POOL_CAP);
            uint32_t ready_victim = vm_s3_build_quota(PROG_M4D_READY, JE_EP_POOL_CAP);
            uint32_t blocked_victim = vm_s3_build_quota(PROG_M4D_BLOCKED, JE_EP_POOL_CAP);
            uint32_t woken_victim = vm_s3_build_quota(PROG_M4D_WOKEN, JE_EP_POOL_CAP);
            uint32_t supervisor = vm_s3_build_quota_kind(
                PROG_M4D_READY, JE_EP_POOL_CAP, true);
            /* The unlinked negative case needs only a LIVE PROCESS record; keeping
             * it out of vm_s3 preserves the bounded five-VM fixture peak. */
            je_proc_id unlinked_id = JE_PROC_ID_NONE;
            uint32_t exit_victim = JE_VM_MAX_PROCESSES;
            /* PR-F must revisit this fixture's teardown when D12 supervisor-loss
             * handling exists. PR-D deliberately leaves the supervisor LIVE so
             * the guard is tested against a real run-linked thread. */
            je_proc_id supervisor_id = vm_s3_proc_ids[supervisor];
            je_handle h_ready_kill = 0u, h_ready_inspect = 0u, h_ready_stale = 0u;
            je_handle h_blocked_kill = 0u, h_woken_kill = 0u, h_self = 0u;
            je_handle h_unlinked = 0u, h_blocked_peer = 0u, h_woken_peer = 0u;
            je_handle h_blocked_victim_end = 0u, h_woken_victim_end = 0u;
            je_handle h_exit_kill = 0u, h_supervisor = 0u;
            je_handle_status alloc_status = JE_HANDLE_OK;
            uint64_t setup_flags = je_arch_irq_save();
            if (je_proc_alloc(&process_pool, JE_EP_POOL_CAP, false, &unlinked_id) != JE_OK
                || unlinked_id.gen == 0u
                || je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[ready_victim],
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_ready_kill, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[ready_victim],
                    JE_ABI_RIGHT_INSPECT, &h_ready_inspect, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[ready_victim],
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_ready_stale, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[blocked_victim],
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_blocked_kill, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[woken_victim],
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_woken_kill, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[killer],
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_self, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, unlinked_id,
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_unlinked, &alloc_status) != JE_OK
                || je_process_handle_mint(
                    &vm_s3, killer, supervisor_id,
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_supervisor, &alloc_status) != JE_OK
                || je_endpoint_kern_seed(&vm_s3, killer, blocked_victim,
                                         &h_blocked_peer, &h_blocked_victim_end) != JE_EP_OK
                || je_endpoint_kern_seed(&vm_s3, killer, woken_victim,
                                         &h_woken_peer, &h_woken_victim_end) != JE_EP_OK) {
                je_arch_irq_restore(setup_flags);
                fail("JeiOS fatal: M4d capability setup\r\n");
            }
            je_arch_irq_restore(setup_flags);
            vm_s3_scratch_write(killer, 0x1c08u, h_ready_kill);
            vm_s3_scratch_write(killer, 0x1c10u, h_ready_inspect);
            vm_s3_scratch_write(killer, 0x1c18u, h_blocked_kill);
            vm_s3_scratch_write(killer, 0x1c20u, h_woken_kill);
            vm_s3_scratch_write(killer, 0x1c28u, h_self);
            vm_s3_scratch_write(killer, 0x1c30u, h_ready_stale);
            vm_s3_scratch_write(killer, 0x1c38u, h_unlinked);
            vm_s3_scratch_write(killer, 0x1c48u, h_blocked_peer);
            vm_s3_scratch_write(killer, 0x1c40u, h_woken_peer);
            vm_s3_scratch_write(killer, 0x1c50u, h_supervisor);
            vm_s3_scratch_write(blocked_victim, 0x1e88u, h_blocked_victim_end);
            vm_s3_scratch_write(woken_victim, 0x1eb8u, h_woken_victim_end);
            vm_s3_scratch_write(blocked_victim, 0x1e98u, 0xdead);
            vm_s3_scratch_write(woken_victim, 0x1ec8u, 0xdead);
            uint16_t blocked_inflight_before = 0u;
            uint32_t blocked_type = 0u, blocked_rights = 0u;
            void *blocked_object = (void *)0;
            if (je_handle_resolve_any(
                    &vm_s3.procs[blocked_victim].htable,
                    h_blocked_victim_end, &blocked_type, &blocked_rights,
                    &blocked_object) != JE_HANDLE_OK
                || blocked_type != JE_ABI_OBJ_ENDPOINT
                || blocked_object == (void *)0) {
                fail("JeiOS fatal: M4d blocked endpoint setup\r\n");
            }
            blocked_inflight_before = ((je_ep_end *)blocked_object)->inflight_refs;
            if (je_thread_create_user(&vm_s3, ready_victim) != JE_OK
                || je_thread_create_user(&vm_s3, blocked_victim) != JE_OK
                || je_thread_create_user(&vm_s3, woken_victim) != JE_OK
                || je_thread_create_user(&vm_s3, killer) != JE_OK
                || je_thread_create_user(&vm_s3, supervisor) != JE_OK) {
                fail("JeiOS fatal: M4d thread setup\r\n");
            }
            uint32_t supervisor_thread_slot = 0u;
            uint32_t supervisor_vm_index = 0u;
            if (je_proc_run_link(&process_pool, supervisor_id,
                                 &supervisor_thread_slot,
                                 &supervisor_vm_index) != JE_OK
                || supervisor_thread_slot == 0u
                || supervisor_vm_index != supervisor) {
                fail("JeiOS fatal: M4d supervisor run link\r\n");
            }
            __asm__ volatile("sti" ::: "memory");

            uint64_t park_start = je_sched_ticks();
            uint64_t park_spins = 0u;
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint64_t blocked_progress = vm_s3_scratch(blocked_victim, 0x1e80u);
                uint64_t woken_progress = vm_s3_scratch(woken_victim, 0x1eb0u);
                je_arch_irq_restore(flags);
                if (phase == 99u) {
                    fail("JeiOS fatal: M4d killer assertion before park\r\n");
                }
                if (blocked_progress == 1u && woken_progress == 1u) break;
                ++park_spins;
                if (je_sched_ticks() - park_start > 300u || park_spins > 100000u)
                    fail("JeiOS fatal: M4d victims did not reach recv\r\n");
                m4d_wait_interrupt();
            }
            /* A parked receiver cannot be dispatched or write its completion
             * status. Hold the killer at its pre-READY-kill gate while the
             * bootstrap lets at least 0x40 timer ticks pass without invoking
             * the reaper. */
            uint64_t blocked_dispatches = je_sched_dispatches_of(&vm_s3, blocked_victim);
            uint64_t woken_dispatches = je_sched_dispatches_of(&vm_s3, woken_victim);
            uint64_t parked_start = je_sched_ticks();
            while (je_sched_ticks() - parked_start < 0x40u) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint64_t blocked_progress = vm_s3_scratch(blocked_victim, 0x1e80u);
                uint64_t woken_progress = vm_s3_scratch(woken_victim, 0x1eb0u);
                uint64_t blocked_status = vm_s3_scratch(blocked_victim, 0x1e98u);
                uint64_t woken_status = vm_s3_scratch(woken_victim, 0x1ec8u);
                uint64_t marker = vm_s3_scratch(killer, 0x1ca0u);
                uint64_t blocked_dispatches_now = je_sched_dispatches_of(
                    &vm_s3, blocked_victim);
                uint64_t woken_dispatches_now = je_sched_dispatches_of(
                    &vm_s3, woken_victim);
                je_arch_irq_restore(flags);
                if (phase != 1u || marker != 1u || blocked_progress != 1u
                    || woken_progress != 1u || blocked_status != 0xdead
                    || woken_status != 0xdead
                    || blocked_dispatches_now != blocked_dispatches
                    || woken_dispatches_now != woken_dispatches)
                    fail("JeiOS fatal: M4d receiver was not parked\r\n");
                m4d_wait_interrupt();
            }
            uint64_t ready_heartbeat_before = 0u;
            uint64_t ready_heartbeat_after = 0u;
            uint64_t ready_gate_start = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint64_t marker = vm_s3_scratch(killer, 0x1ca0u);
                ready_heartbeat_before = vm_s3_scratch(ready_victim, 0x1e00u);
                je_arch_irq_restore(flags);
                if (phase != 1u || marker != 1u)
                    fail("JeiOS fatal: M4d READY kill gate changed\r\n");
                if (ready_heartbeat_before != 0u) break;
                if (je_sched_ticks() - ready_gate_start > 300u)
                    fail("JeiOS fatal: M4d READY victim did not run\r\n");
                m4d_wait_interrupt();
            }
            vm_s3_scratch_write(killer, 0x1bf8u, 1u);
            uint64_t ready_dispatches_after = 0u;
            uint64_t ready_kill_wait = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t marker = vm_s3_scratch(killer, 0x1ca0u);
                uint64_t heartbeat = vm_s3_scratch(ready_victim, 0x1e00u);
                ready_dispatches_after = je_sched_dispatches_of(&vm_s3, ready_victim);
                if (marker == 2u) {
                    if (heartbeat == 0u)
                        fail("JeiOS fatal: M4d READY kill transition\r\n");
                    ready_heartbeat_after = heartbeat;
                    je_arch_irq_restore(flags);
                    vm_s3_scratch_write(killer, 0x1bf8u, 2u);
                    break;
                }
                je_arch_irq_restore(flags);
                if (marker == 99u || je_sched_ticks() - ready_kill_wait > 300u)
                    fail("JeiOS fatal: M4d READY kill timeout\r\n");
                m4d_wait_interrupt();
            }
            uint64_t ready_hold_start = je_sched_ticks();
            while (je_sched_ticks() - ready_hold_start < 0x40u) {
                uint64_t flags = je_arch_irq_save();
                uint64_t heartbeat = vm_s3_scratch(ready_victim, 0x1e00u);
                uint64_t marker = vm_s3_scratch(killer, 0x1ca0u);
                uint64_t dispatches = je_sched_dispatches_of(&vm_s3, ready_victim);
                je_arch_irq_restore(flags);
                if (marker != 2u || heartbeat != ready_heartbeat_after
                    || dispatches != ready_dispatches_after)
                    fail("JeiOS fatal: M4d READY victim resumed after kill\r\n");
                m4d_wait_interrupt();
            }

            uint64_t blocked_death_start = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint64_t blocked_progress = vm_s3_scratch(blocked_victim, 0x1e80u);
                uint64_t recv_status = vm_s3_scratch(blocked_victim, 0x1e98u);
                uint32_t state = JE_PROC_FREE, reason = 0u; uint64_t code = 0u;
                je_status status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[blocked_victim],
                    &state, &reason, &code);
                uint32_t type = 0u, rights = 0u; void *object = (void *)0;
                je_handle_status resolved = je_handle_resolve_any(
                    &vm_s3.procs[blocked_victim].htable,
                    h_blocked_victim_end, &type, &rights, &object);
                bool waiter_released = false;
                if (resolved == JE_HANDLE_OK && type == JE_ABI_OBJ_ENDPOINT
                    && object != (void *)0) {
                    je_ep_end *end = (je_ep_end *)object;
                    waiter_released = !end->waiter.present
                        && end->waiter.cause == JE_EP_WAIT_NONE
                        && end->inflight_refs == blocked_inflight_before;
                }
                uint64_t blocked_dispatches_now = je_sched_dispatches_of(
                    &vm_s3, blocked_victim);
                je_arch_irq_restore(flags);
                if (phase == 99u)
                    fail("JeiOS fatal: M4d blocked-kill assertion\r\n");
                if (phase == 3u) {
                    if (status != JE_OK || state != JE_PROC_DYING
                        || reason != JE_DEATH_KILLED
                        || blocked_progress != 1u
                        || blocked_dispatches_now != blocked_dispatches
                        || recv_status != 0xdead
                        || !waiter_released)
                        fail("JeiOS fatal: M4d blocked waiter completion\r\n");
                    break;
                }
                if (je_sched_ticks() - blocked_death_start > 300u)
                    fail("JeiOS fatal: M4d blocked kill timeout\r\n");
                m4d_wait_interrupt();
            }
            while (je_sched_has_process(&vm_s3, blocked_victim)) {
                if (je_sched_ticks() - blocked_death_start > 400u)
                    fail("JeiOS fatal: M4d blocked victim reap timeout\r\n");
                je_kernel_idle_step();
            }
            if (je_endpoint_kernel_allocated() != 1u)
                fail("JeiOS fatal: M4d blocked endpoint was not reclaimed\r\n");
            /* Release the killer's phase-3 gate only after all BLOCKED_IPC
             * checks and the intended reap have completed. */
            vm_s3_scratch_write(killer, 0x1bf8u, 3u);

            uint64_t woken_park_start = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint64_t progress = vm_s3_scratch(woken_victim, 0x1eb0u);
                uint64_t recv_status = vm_s3_scratch(woken_victim, 0x1ec8u);
                je_arch_irq_restore(flags);
                if (phase == 99u)
                    fail("JeiOS fatal: M4d woken preparation assertion\r\n");
                if (phase == 4u && progress == 1u
                    && recv_status != JE_ABI_H_TIMED_OUT
                    && recv_status != JE_ABI_H_CANCELLED)
                    break;
                if (je_sched_ticks() - woken_park_start > 300u)
                    fail("JeiOS fatal: M4d WOKEN victim did not park\r\n");
                m4d_wait_interrupt();
            }
            woken_dispatches = je_sched_dispatches_of(&vm_s3, woken_victim);
            uint64_t woken_start = je_sched_ticks();
            while (je_sched_ticks() - woken_start < 0x40u) {
                uint64_t flags = je_arch_irq_save();
                uint64_t progress = vm_s3_scratch(woken_victim, 0x1eb0u);
                uint64_t recv_status = vm_s3_scratch(woken_victim, 0x1ec8u);
                je_arch_irq_restore(flags);
                if (progress != 1u || recv_status == JE_ABI_H_TIMED_OUT
                    || recv_status == JE_ABI_H_CANCELLED
                    || je_sched_dispatches_of(&vm_s3, woken_victim) != woken_dispatches)
                    fail("JeiOS fatal: M4d WOKEN victim was not parked\r\n");
                m4d_wait_interrupt();
            }
            uint32_t woken_released_before =
                je_endpoint_kern_woken_reservations_released();
            /* Release phase 4 only after the parked/WOKEN checks are complete,
             * so the adjacent send+kill pair cannot outrun the coordinator. */
            vm_s3_scratch_write(killer, 0x1bf8u, 4u);

            uint64_t woken_death_start = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint32_t state = JE_PROC_FREE, reason = 0u; uint64_t code = 0u;
                je_status status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[woken_victim],
                    &state, &reason, &code);
                uint32_t type = 0u, rights = 0u; void *object = (void *)0;
                je_handle_status resolved = je_handle_resolve_any(
                    &vm_s3.procs[killer].htable, h_woken_peer,
                    &type, &rights, &object);
                bool queue_ok = false;
                if (resolved == JE_HANDLE_OK && type == JE_ABI_OBJ_ENDPOINT
                    && object != (void *)0) {
                    je_ep_end *sender = (je_ep_end *)object;
                    uintptr_t channel_address = (uintptr_t)sender
                        - offsetof(je_ep_channel, end)
                        - (uintptr_t)sender->side * sizeof(je_ep_end);
                    je_ep_channel *channel = (je_ep_channel *)channel_address;
                    je_ep_end *receiver = &channel->end[sender->side ^ 1u];
                    queue_ok = !receiver->waiter.present
                        && !receiver->waiter.head_reserved
                        && receiver->inbound.count == 1u
                        && receiver->state == JE_EP_END_OPEN;
                }
                je_arch_irq_restore(flags);
                if (phase == 99u)
                    fail("JeiOS fatal: M4d WOKEN kill assertion\r\n");
                if (phase == 5u) {
                    if (status != JE_OK || state != JE_PROC_DYING
                        || reason != JE_DEATH_KILLED || !queue_ok
                        || je_endpoint_kern_woken_reservations_released()
                               != woken_released_before + 1u
                        || je_sched_dispatches_of(&vm_s3, woken_victim)
                               != woken_dispatches)
                        fail("JeiOS fatal: M4d reserved-head release\r\n");
                    break;
                }
                if (je_sched_ticks() - woken_death_start > 300u)
                    fail("JeiOS fatal: M4d WOKEN kill timeout\r\n");
                m4d_wait_interrupt();
            }
            uint64_t woken_close_flags = je_arch_irq_save();
            je_ep_status woken_close = je_endpoint_kern_close(
                &vm_s3, killer, h_woken_peer);
            je_arch_irq_restore(woken_close_flags);
            if (woken_close != JE_EP_OK)
                fail("JeiOS fatal: M4d WOKEN peer close\r\n");
            while (je_sched_has_process(&vm_s3, woken_victim)) {
                if (je_sched_ticks() - woken_death_start > 400u)
                    fail("JeiOS fatal: M4d WOKEN victim reap timeout\r\n");
                je_kernel_idle_step();
            }
            if (je_endpoint_kernel_allocated() != 0u)
                fail("JeiOS fatal: M4d WOKEN endpoint was not reclaimed\r\n");
            /* The EXIT victim is built only after the three live victim VMs
             * have been reclaimed, keeping the peak real-VM fixture bounded. */
            uint64_t exit_setup_flags = je_arch_irq_save();
            exit_victim = vm_s3_build(PROG_EXIT);
            if (je_process_handle_mint(
                    &vm_s3, killer, vm_s3_proc_ids[exit_victim],
                    JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
                    &h_exit_kill, &alloc_status) != JE_OK
                || je_thread_create_user(&vm_s3, exit_victim) != JE_OK) {
                je_arch_irq_restore(exit_setup_flags);
                fail("JeiOS fatal: M4d EXIT victim setup\r\n");
            }
            je_arch_irq_restore(exit_setup_flags);
            vm_s3_scratch_write(killer, 0x1c58u, h_exit_kill);
            /* Phase 5 is acknowledged only after WOKEN teardown and EXIT
             * setup, so the killer cannot inspect an uninitialized handle. */
            vm_s3_scratch_write(killer, 0x1bf8u, 5u);
            uint64_t exit_wait_start = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint32_t state = JE_PROC_FREE, reason = 0u; uint64_t code = 0u;
                je_status status = je_proc_status(
                    &process_pool, vm_s3_proc_ids[exit_victim],
                    &state, &reason, &code);
                je_arch_irq_restore(flags);
                if (status == JE_OK && state == JE_PROC_DYING
                    && reason == JE_DEATH_EXIT) {
                    if (code != 42u)
                        fail("JeiOS fatal: M4d EXIT code changed\r\n");
                    vm_s3_scratch_write(killer, 0x1bf8u, 6u);
                    break;
                }
                if (je_sched_ticks() - exit_wait_start > 300u)
                    fail("JeiOS fatal: M4d EXIT victim timeout\r\n");
                m4d_wait_interrupt();
            }
            uint64_t supervisor_heartbeat_before =
                vm_s3_scratch(supervisor, 0x1e00u);
            uint64_t supervisor_dispatches_before =
                je_sched_dispatches_of(&vm_s3, supervisor);
            uint64_t done_start = je_sched_ticks();
            for (;;) {
                uint64_t flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(killer, 0x1c00u);
                uint32_t state = JE_PROC_FREE, reason = 0u; uint64_t code = 0u;
                je_status status = je_proc_status(
                    &process_pool, unlinked_id,
                    &state, &reason, &code);
                je_arch_irq_restore(flags);
                if (phase == 99u) {
                    fail("JeiOS fatal: M4d negative assertion\r\n");
                }
                if (phase == 7u) {
                    if (status != JE_OK || state != JE_PROC_LIVE)
                        fail("JeiOS fatal: M4d unlinked record changed\r\n");
                    break;
                }
                if (je_sched_ticks() - done_start > 300u)
                    fail("JeiOS fatal: M4d completion timeout\r\n");
                m4d_wait_interrupt();
            }
            uint64_t supervisor_progress_start = je_sched_ticks();
            for (;;) {
                uint64_t supervisor_heartbeat =
                    vm_s3_scratch(supervisor, 0x1e00u);
                uint64_t supervisor_dispatches =
                    je_sched_dispatches_of(&vm_s3, supervisor);
                uint32_t supervisor_state = JE_PROC_FREE;
                uint32_t supervisor_reason = 0u;
                uint64_t supervisor_code = 0u;
                uint64_t flags = je_arch_irq_save();
                je_status supervisor_status = je_proc_status(
                    &process_pool, supervisor_id, &supervisor_state,
                    &supervisor_reason, &supervisor_code);
                je_arch_irq_restore(flags);
                if (supervisor_status != JE_OK
                    || supervisor_state != JE_PROC_LIVE
                    || supervisor_reason != 0u || supervisor_code != 0u)
                    fail("JeiOS fatal: M4d supervisor guard changed record\r\n");
                if (supervisor_heartbeat > supervisor_heartbeat_before
                    && supervisor_dispatches > supervisor_dispatches_before)
                    break;
                if (je_sched_ticks() - supervisor_progress_start > 300u)
                    fail("JeiOS fatal: M4d supervisor stopped dispatching\r\n");
                m4d_wait_interrupt();
            }
            while (je_sched_has_process(&vm_s3, killer)) {
                if (je_sched_ticks() - done_start > 400u)
                    fail("JeiOS fatal: M4d killer reap timeout\r\n");
                je_kernel_idle_step();
            }
            while (je_sched_has_process(&vm_s3, exit_victim)) {
                if (je_sched_ticks() - done_start > 500u)
                    fail("JeiOS fatal: M4d EXIT victim reap timeout\r\n");
                je_kernel_idle_step();
            }
            (void)je_arch_irq_save();
            if (je_sched_drop_user(&vm_s3, supervisor) != JE_OK
                || vm_s3_destroy(supervisor) != JE_OK)
                fail("JeiOS fatal: M4d supervisor cleanup\r\n");
            if (je_proc_abandon(&process_pool, unlinked_id) != JE_OK)
                fail("JeiOS fatal: M4d unlinked cleanup\r\n");
            if (je_endpoint_kernel_allocated() != 0u || process_pool.used_count != 0u)
                fail("JeiOS fatal: M4d final pool baseline\r\n");
            je_arch_console_write(
                "M4d-s1: PROC_KILL OK (READY, BLOCKED_IPC, WOKEN-reserved, EXIT first-reason, supervisor guard, self/after-death/unlinked/rights, next-syscall PROC_STATUS)\r\n");
        }
        /* --- M4e-s1: build-time ELF conversion plus a real ring-3 C image.
         * The assembly parent owns the LAUNCHER and creates the channel. It
         * grants only SEND on one end, then waits for READY and child DEAD.
         * The coordinator audits before granting the parent permission to
         * inspect/reap the child, keeping the lifecycle observations ordered. */
        {
            if (process_pool.used_count != 0u || je_endpoint_kernel_allocated() != 0u)
                fail("JeiOS fatal: M4e baseline\r\n");
            uint32_t spawner = vm_s3_build_quota(PROG_M4E, JE_EP_POOL_CAP);
            je_handle launcher = 0u;
            uint64_t setup_flags = je_arch_irq_save();
            je_handle_status launcher_status = je_handle_alloc(
                &vm_s3.procs[spawner].htable, (void *)0, JE_ABI_OBJ_LAUNCHER,
                JE_ABI_RIGHT_SPAWN, &launcher);
            je_arch_irq_restore(setup_flags);
            if (launcher_status != JE_HANDLE_OK || launcher == 0u)
                fail("JeiOS fatal: M4e launcher\r\n");
            je_user_startup startup = {
                JE_ABI_STARTUP_V1, 1u, launcher, 0u, 0u, 0u
            };
            if (je_thread_create_user_start(&vm_s3, spawner, &startup,
                                            (uint32_t *)0) != JE_OK)
                fail("JeiOS fatal: M4e spawner thread\r\n");

            bool child_audited = false;
            bool child_reaped = false;
            uint32_t child_vm = JE_VM_MAX_PROCESSES;
            je_proc_id child_id = JE_PROC_ID_NONE;
            uint64_t phase_start = je_sched_ticks();
            uint64_t phase_spins = 0u;
            for (;;) {
                uint64_t phase_flags = je_arch_irq_save();
                uint64_t phase = vm_s3_scratch(spawner, 0x1f00u);
                if (phase == 99u) {
                    je_arch_irq_restore(phase_flags);
                    fail("JeiOS fatal: M4e ring-3 assertion\r\n");
                }
                for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
                    je_proc_rec *rec = &process_pool.rec[i];
                    if (rec->state != JE_PROC_LIVE && rec->state != JE_PROC_DYING)
                        continue;
                    if (!rec->run_linked || rec->vm_index >= JE_VM_MAX_PROCESSES
                        || i == vm_s3_proc_ids[spawner].index)
                        continue;
                    if (!child_audited) {
                        if (je_vm_audit(&vm_s3, rec->vm_index) != JE_OK) {
                            je_arch_irq_restore(phase_flags);
                            fail("JeiOS fatal: M4e child audit\r\n");
                        }
                        child_audited = true;
                        child_vm = rec->vm_index;
                        child_id = (je_proc_id){ i, rec->gen };
                        vm_s3_scratch_write(spawner, 0x1f28u, 1u);
                    }
                }
                if (child_audited && phase >= 2u && !child_reaped) {
                    uint32_t state = JE_PROC_FREE;
                    uint32_t reason = 0u;
                    uint64_t code = 0u;
                    if (je_proc_status(&process_pool, child_id, &state,
                                       &reason, &code) != JE_OK) {
                        je_arch_irq_restore(phase_flags);
                        fail("JeiOS fatal: M4e child status\r\n");
                    }
                    if (state == JE_PROC_DYING) {
                        if (reason != JE_DEATH_EXIT || code != 0u) {
                            je_arch_irq_restore(phase_flags);
                            fail("JeiOS fatal: M4e child exit status\r\n");
                        }
                        if (je_sched_reap_dead() < 0) {
                            je_arch_irq_restore(phase_flags);
                            fail("JeiOS fatal: M4e child reap\r\n");
                        }
                        child_reaped = true;
                    }
                }
                if (child_audited && phase < 2u && !child_reaped) {
                    uint32_t state = JE_PROC_FREE;
                    uint32_t reason = 0u;
                    uint64_t code = 0u;
                    if (je_proc_status(&process_pool, child_id, &state,
                                       &reason, &code) == JE_OK
                        && state == JE_PROC_DYING
                        && (reason != JE_DEATH_EXIT || code != 0u)) {
                        je_arch_irq_restore(phase_flags);
                        fail("JeiOS fatal: M4e child failed before READY\r\n");
                    }
                }
                if (child_reaped) {
                    uint32_t state = JE_PROC_FREE;
                    uint32_t reason = 0u;
                    uint64_t code = 0u;
                    if (je_proc_status(&process_pool, child_id, &state,
                                       &reason, &code) != JE_OK
                        || state != JE_PROC_DEAD || reason != JE_DEATH_EXIT
                        || code != 0u) {
                        je_arch_irq_restore(phase_flags);
                        fail("JeiOS fatal: M4e child DEAD status\r\n");
                    }
                }
                je_arch_irq_restore(phase_flags);
                if (child_audited && child_reaped && phase == 3u)
                    break;
                if (je_sched_ticks() - phase_start > 500u || ++phase_spins > 1000000u) {
                    fail("JeiOS fatal: M4e bounded wait\r\n");
                }
                if (child_audited && phase >= 2u)
                    je_kernel_idle_step();
                else
                    m4d_wait_interrupt();
            }
            uint64_t parent_reap_start = je_sched_ticks();
            while (je_sched_has_process(&vm_s3, spawner)) {
                if (je_sched_ticks() - parent_reap_start > 200u)
                    fail("JeiOS fatal: M4e parent reap timeout\r\n");
                je_kernel_idle_step();
            }
            if (child_vm >= JE_VM_MAX_PROCESSES
                || je_sched_has_process(&vm_s3, child_vm)
                || je_endpoint_kernel_allocated() != 0u
                || process_pool.used_count != 0u)
                fail("JeiOS fatal: M4e pool baseline\r\n");
            je_arch_console_write(
                "M4e-s1: ring-3 C image OK (startup v1, rodata/data/bss, CLOCK_TICKS, READY, exit 0); image sha256=");
            je_arch_console_write(je_c_hello_blob_sha256);
            je_arch_console_write("\r\n");
        }
        /* M3c leaves no VM process intentionally live: B was torn down before
         * the cross-table, timeout, and bootstrap proofs. */
        vm_s3_assert_all_free("JeiOS fatal: M3c vm process leak\r\n");
        /* --- M4f: seed the real supervisor only after every earlier fixture
         * has returned the pools to its identity baseline. From this point
         * forward slot 0 is owned exclusively by m4f_monitor; it does not call
         * je_kernel_idle_step, because that would reap before the identity scan. */
        {
            static m4f_baseline baseline;
            uint64_t baseline_flags = je_arch_irq_save();
            m4f_take_baseline(&baseline);
            je_arch_irq_restore(baseline_flags);
            if (baseline.endpoints != 0u || baseline.user_threads != 0u
                || process_pool.used_count != 0u || vm_s3_live_count() != 0u)
                fail("JeiOS fatal: M4f nonempty pre-seed baseline\r\n");
            je_proc_id supervisor_id = JE_PROC_ID_NONE;
            uint32_t supervisor_vm = m4f_seed_supervisor(&supervisor_id);
#if JE_TEST_MODE == 12
            if (!svc_report_word_write(supervisor_vm, JE_SVC_ROLLBACK_GO_VADDR,
                                       JE_SVC_LOSS_GO))
                fail("JeiOS fatal: M4f loss configuration write\r\n");
#endif
            m4f_monitor(&baseline, supervisor_id, supervisor_vm);
        }
    }

    je_arch_console_write("JeiOS: initialization complete; halted\r\n");
    for (;;) je_kernel_idle_step();
}

