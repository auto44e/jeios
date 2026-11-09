/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP3a-1 (ADR 0015 D1): AArch64 je_vm_ops backend.
 *
 * Caller context: EL1, one CPU, interrupts masked by the boot proof harness.
 * The arch-neutral builder owns admission, image validation, publication order,
 * and rollback. This backend owns the physical extents and the AArch64 table
 * topology:
 *
 *   process root L0[0..31]  = the kernel root's shared descriptors
 *   process root L0[32]     = private arena L1
 *   arena L1[0]             = private arena L2
 *   arena L2[0]             = private arena L3
 *   arena L3                = private EL0 leaves
 *
 * The backend uses the same physical ownership model before and after enable.
 * Once the MMU is on, each process table or transient user-frame access is
 * exposed through one bounded identity alias, then removed before publication
 * or reuse. The page-table and cache/TLBI ordering is therefore explicit at
 * every MMU-on boundary.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jeios/alloc.h>
#include <jeios/vm.h>

#include "paging.h"

#define ARM_DESC_VALID UINT64_C(1)
#define ARM_DESC_TABLE UINT64_C(3)
#define ARM_DESC_ADDR_MASK UINT64_C(0x0000fffffffff000)
#define ARM_DESC_AP1 (UINT64_C(1) << 6)
#define ARM_DESC_AP2 (UINT64_C(1) << 7)
#define ARM_DESC_NG (UINT64_C(1) << 11)
#define ARM_DESC_PXN (UINT64_C(1) << 53)
#define ARM_DESC_UXN (UINT64_C(1) << 54)
#define ARM_DESC_TABLE_AP_MASK (UINT64_C(3) << 61)
#define ARM_DESC_TABLE_PXN (UINT64_C(1) << 59)
#define ARM_DESC_TABLE_UXN (UINT64_C(1) << 60)
#define ARM_DESC_TABLE_NS (UINT64_C(1) << 63)
#define ARM_PA_LIMIT (UINT64_C(1) << 44)
#define ARM_L0_KERNEL_APTABLE (UINT64_C(1) << 61)
#define ARM_L0_KERNEL_UXNTABLE (UINT64_C(1) << 60)
#define ARM_L0_ARENA_PXNTABLE (UINT64_C(1) << 59)

extern je_arm_mmu je_arm_mmu_context;

_Static_assert(JE_PHYSICAL_LIMIT <= (UINT64_C(1) << 32),
               "ARM process roots are truncated to uint32_t");
_Static_assert(((JE_VM_ARENA_BASE >> 39) & UINT64_C(0x1ff)) == 32u,
               "AArch64 arena must occupy L0[32]");
_Static_assert(((JE_VM_ARENA_BASE >> 30) & UINT64_C(0x1ff)) == 0u,
               "AArch64 arena must occupy L1[0]");
_Static_assert(((JE_VM_ARENA_BASE >> 21) & UINT64_C(0x1ff)) == 0u,
               "AArch64 arena must occupy L2[0]");
_Static_assert((JE_ARM_USER_CODE_BITS & ARM_DESC_AP1) != 0u &&
               (JE_ARM_USER_RODATA_BITS & ARM_DESC_AP1) != 0u &&
               (JE_ARM_USER_DATA_BITS & ARM_DESC_AP1) != 0u,
               "all ARM user classes must set AP[1]");
_Static_assert((JE_ARM_USER_CODE_BITS & ARM_DESC_UXN) == 0u &&
               (JE_ARM_USER_RODATA_BITS & ARM_DESC_UXN) != 0u &&
               (JE_ARM_USER_DATA_BITS & ARM_DESC_UXN) != 0u,
               "USER_CODE is the only EL0-executable class");
_Static_assert((JE_ARM_USER_CODE_BITS & ARM_DESC_AP2) != 0u &&
               (JE_ARM_USER_DATA_BITS & ARM_DESC_AP2) == 0u,
               "USER_CODE is EL0 read-only and USER_DATA is writable");

typedef struct {
    bool used;
    uint32_t process;
    uint64_t physical;
    size_t pages;
    je_vm_kind kind;
} arm_extent;

typedef struct {
    bool used;
    uint64_t root;
    uint32_t table_extent;
} arm_process;

typedef struct {
    je_pages *pages;
    uint64_t kernel_root;
    arm_process process[JE_VM_MAX_PROCESSES];
    arm_extent extent[JE_VM_MAX_PROCESSES * JE_VM_MAX_EXTENTS];
    uint8_t alias_state[JE_PAGE_COUNT]; /* audit mirror of the paging ledger */
    bool kernel_root_decorated;
} arm_vm_state;

static arm_vm_state arm_vm;
static bool arm_kernel_root_decorated;

static void *arm_map(void *ctx, je_vm_ref frame);

static _Noreturn void arm_fail_stop(void)
{
    for (;;) __asm__ volatile("wfe");
}

static void *phys_ptr(uint64_t physical)
{
    return (void *)(uintptr_t)physical;
}

static bool in_phys_window(uint64_t physical)
{
    return physical >= JE_PHYSICAL_BASE && physical < JE_PHYSICAL_LIMIT;
}

static arm_extent *extent_of(arm_vm_state *state, uint32_t process,
                             uint32_t id)
{
    if (!state || process >= JE_VM_MAX_PROCESSES ||
        id >= sizeof(state->extent) / sizeof(state->extent[0]))
        return NULL;
    arm_extent *extent = &state->extent[id];
    if (!extent->used || extent->process != process) return NULL;
    return extent;
}

static const arm_extent *extent_of_const(const arm_vm_state *state,
                                         uint32_t process, uint32_t id)
{
    return extent_of((arm_vm_state *)(uintptr_t)state, process, id);
}

static uint64_t extent_frame(const arm_extent *extent, uint32_t page)
{
    return extent->physical + (uint64_t)page * JE_PAGE_SIZE;
}

static void zero_frames(uint64_t physical, size_t pages)
{
    for (size_t page = 0u; page < pages; ++page) {
        volatile uint64_t *words =
            (volatile uint64_t *)phys_ptr(physical + page * JE_PAGE_SIZE);
        for (size_t word = 0u; word < JE_PAGE_SIZE / sizeof(uint64_t); ++word)
            words[word] = 0u;
    }
}

static uint64_t *identity_page(uint64_t physical)
{
    if (!in_phys_window(physical) ||
        (physical & (JE_PAGE_SIZE - 1u)) != 0u)
        return NULL;
    if (je_arm_mmu_is_enabled(&je_arm_mmu_context) &&
        je_arm_mmu_map_page(&je_arm_mmu_context, physical, physical,
                            JE_MAP_RW) != JE_OK)
        return NULL;
    return (uint64_t *)phys_ptr(physical);
}

static void identity_page_release(uint64_t physical)
{
    if (je_arm_mmu_is_enabled(&je_arm_mmu_context) &&
        je_arm_mmu_unmap_page(&je_arm_mmu_context, physical) != JE_OK)
        arm_fail_stop();
}

static void zero_extent(arm_extent *extent)
{
    if (!extent) arm_fail_stop();
    for (size_t page = 0u; page < extent->pages; ++page) {
        const uint64_t physical = extent_frame(extent, (uint32_t)page);
        uint64_t *mapped = identity_page(physical);
        if (!mapped) arm_fail_stop();
        for (size_t word = 0u; word < JE_PAGE_SIZE / sizeof(uint64_t);
             ++word)
            mapped[word] = 0u;
        identity_page_release(physical);
    }
}

static uint64_t table_descriptor(uint64_t physical)
{
    return (physical & ARM_DESC_ADDR_MASK) | ARM_DESC_TABLE;
}

static uint64_t user_leaf_bits(je_vm_kind kind)
{
    switch (kind) {
    case JE_VM_USER_CODE: return JE_ARM_USER_CODE_BITS;
    case JE_VM_USER_RODATA: return JE_ARM_USER_RODATA_BITS;
    case JE_VM_USER_DATA: return JE_ARM_USER_DATA_BITS;
    default: return 0u;
    }
}

static bool user_kind(je_vm_kind kind)
{
    return kind == JE_VM_USER_CODE || kind == JE_VM_USER_RODATA ||
           kind == JE_VM_USER_DATA;
}

static bool physical_in_extent(const arm_extent *extent, uint64_t physical,
                               uint32_t *out_page)
{
    if (!extent || physical < extent->physical ||
        physical - extent->physical >=
            (uint64_t)extent->pages * JE_PAGE_SIZE)
        return false;
    if (out_page) {
        *out_page = (uint32_t)((physical - extent->physical) / JE_PAGE_SIZE);
    }
    return true;
}

static je_status arm_reserve_hold(void *ctx, size_t pages,
                                  uint64_t *out_physical)
{
    arm_vm_state *state = ctx;
    if (!state || !state->pages || !out_physical || pages == 0u)
        return JE_ERR_INVALID;
    return je_pages_alloc(state->pages, pages, out_physical);
}

static je_status arm_register_extent(void *ctx, uint32_t process, size_t pages,
                                     je_vm_kind kind, uint32_t *out_extent)
{
    arm_vm_state *state = ctx;
    if (!state || !state->pages || process >= JE_VM_MAX_PROCESSES ||
        pages == 0u || !out_extent ||
        (!user_kind(kind) && kind != JE_VM_TABLE))
        return JE_ERR_INVALID;

    const size_t count = sizeof(state->extent) / sizeof(state->extent[0]);
    for (uint32_t id = 0u; id < count; ++id) {
        arm_extent *extent = &state->extent[id];
        if (extent->used) continue;
        uint64_t physical = 0u;
        je_status status = je_pages_alloc(state->pages, pages, &physical);
        if (status != JE_OK) return status;
        *extent = (arm_extent){ true, process, physical, pages, kind };
        *out_extent = id;
        return JE_OK;
    }
    return JE_ERR_NOMEM;
}

static je_status decorate_kernel_root(uint64_t root_pa)
{
    if (!in_phys_window(root_pa)) return JE_ERR_INVALID;
    uint64_t *root = phys_ptr(root_pa);
    for (uint32_t index = 0u; index < 512u; ++index) {
        if ((root[index] & ARM_DESC_VALID) == 0u) continue;
        if (index >= 32u || (root[index] & UINT64_C(3)) != ARM_DESC_TABLE)
            return JE_ERR_INVALID;
        root[index] |= ARM_L0_KERNEL_APTABLE | ARM_L0_KERNEL_UXNTABLE;
    }
    return JE_OK;
}

je_status je_arch_vm_decorate_kernel_root(void)
{
    if (arm_kernel_root_decorated) return JE_OK;
    je_status status = decorate_kernel_root(
        je_arm_mmu_root(&je_arm_mmu_context));
    if (status == JE_OK) arm_kernel_root_decorated = true;
    return status;
}

static je_status arm_link_tables(void *ctx, uint32_t process,
                                 uint32_t table_extent, uint32_t *out_root)
{
    arm_vm_state *state = ctx;
    if (!state || !out_root || process >= JE_VM_MAX_PROCESSES ||
        state->kernel_root == 0u)
        return JE_ERR_INVALID;
    arm_extent *extent = extent_of(state, process, table_extent);
    if (!extent || extent->kind != JE_VM_TABLE ||
        extent->pages != JE_VM_TABLE_PAGES)
        return JE_ERR_INVALID;

    if (!state->kernel_root_decorated) return JE_ERR_INVALID;
    uint64_t root_pa = extent_frame(extent, 0u);
    uint64_t l1_pa = extent_frame(extent, 1u);
    uint64_t l2_pa = extent_frame(extent, 2u);
    uint64_t l3_pa = extent_frame(extent, 3u);
    uint64_t *root = identity_page(root_pa);
    uint64_t *l1 = identity_page(l1_pa);
    uint64_t *l2 = identity_page(l2_pa);
    uint64_t *l3 = identity_page(l3_pa);
    if (!root || !l1 || !l2 || !l3) arm_fail_stop();
    zero_frames(root_pa, JE_VM_TABLE_PAGES);
    uint64_t *kernel_root = (uint64_t *)phys_ptr(state->kernel_root);

    /* The process root shares the complete kernel L0 array byte-for-byte.
     * The kernel root was decorated once, after the unchanged MP2 audit, so
     * those shared descriptors carry hierarchical EL0 denial. */
    for (uint32_t index = 0u; index < 512u; ++index)
        root[index] = kernel_root[index];
    if ((root[32] & ARM_DESC_VALID) != 0u) return JE_ERR_INVALID;

    root[32] = table_descriptor(l1_pa) | ARM_L0_ARENA_PXNTABLE;
    l1[0] = table_descriptor(l2_pa);
    l2[0] = table_descriptor(l3_pa);
    identity_page_release(l3_pa);
    identity_page_release(l2_pa);
    identity_page_release(l1_pa);
    identity_page_release(root_pa);
    state->process[process] = (arm_process){ true, root_pa, table_extent };
    *out_root = (uint32_t)root_pa;
    return JE_OK;
}

static je_status arm_install_pte(void *ctx, je_vm_ref frame,
                                 uint64_t arena_offset)
{
    arm_vm_state *state = ctx;
    if (!state || frame.process >= JE_VM_MAX_PROCESSES ||
        arena_offset >= JE_VM_ARENA_SIZE ||
        (arena_offset & (JE_PAGE_SIZE - 1u)) != 0u)
        return JE_ERR_INVALID;
    arm_extent *extent = extent_of(state, frame.process, frame.extent);
    arm_process *process = &state->process[frame.process];
    if (!extent || !process->used || !user_kind(extent->kind) ||
        frame.page >= extent->pages)
        return JE_ERR_INVALID;
    arm_extent *tables = extent_of(state, frame.process, process->table_extent);
    if (!tables) return JE_ERR_INVALID;
    const uint64_t l3_pa = extent_frame(tables, 3u);
    uint64_t *l3 = identity_page(l3_pa);
    if (!l3) arm_fail_stop();
    size_t index = (size_t)(arena_offset / JE_PAGE_SIZE);
    if ((l3[index] & ARM_DESC_VALID) != 0u) {
        identity_page_release(l3_pa);
        return JE_ERR_INVALID;
    }
    const uint64_t physical = extent_frame(extent, frame.page);
    if (extent->kind != JE_VM_USER_DATA) {
        const je_map_class cls = extent->kind == JE_VM_USER_CODE
                                     ? JE_MAP_USER_CODE
                                     : JE_MAP_USER_RODATA;
        if (je_arm_mmu_publish_user_frame(&je_arm_mmu_context, physical, cls) !=
            JE_OK) {
            identity_page_release(l3_pa);
            return JE_ERR_DENIED;
        }
    }
    l3[index] = physical | user_leaf_bits(extent->kind);
    identity_page_release(l3_pa);
    return JE_OK;
}

static je_status arm_demote_alias(void *ctx, je_vm_ref frame)
{
    arm_vm_state *state = ctx;
    arm_extent *extent = extent_of(state, frame.process, frame.extent);
    if (!extent || !user_kind(extent->kind) ||
        extent->kind == JE_VM_USER_DATA || frame.page >= extent->pages)
        return JE_ERR_INVALID;
    uint64_t physical = extent_frame(extent, frame.page);
    if (!in_phys_window(physical)) return JE_ERR_INVALID;
    state->alias_state[(physical - JE_PHYSICAL_BASE) / JE_PAGE_SIZE] = 1u;
    return JE_OK;
}

static void arm_unmap(void *ctx, je_vm_ref frame)
{
    arm_vm_state *state = ctx;
    const arm_extent *extent = extent_of_const(state, frame.process,
                                               frame.extent);
    if (!extent || !user_kind(extent->kind) || frame.page >= extent->pages)
        arm_fail_stop();
    const uint64_t physical = extent_frame(extent, frame.page);
    if (extent->kind == JE_VM_USER_CODE &&
        je_arm_mmu_sync_user_code(physical) != JE_OK)
        arm_fail_stop();
    if (je_arm_mmu_is_enabled(&je_arm_mmu_context) &&
        je_arm_mmu_unmap_page(&je_arm_mmu_context, physical) != JE_OK)
        arm_fail_stop();
}

static void arm_clear_pte(void *ctx, uint32_t process, uint64_t arena_offset)
{
    arm_vm_state *state = ctx;
    if (state == NULL) arm_fail_stop();
    arm_process *proc = process < JE_VM_MAX_PROCESSES
                            ? &state->process[process] : NULL;
    if (!proc || !proc->used || arena_offset >= JE_VM_ARENA_SIZE ||
        (arena_offset & (JE_PAGE_SIZE - 1u)) != 0u)
        for (;;) __asm__ volatile("wfe");
    arm_extent *tables = extent_of(state, process, proc->table_extent);
    if (!tables) for (;;) __asm__ volatile("wfe");
    const uint64_t l3_pa = extent_frame(tables, 3u);
    uint64_t *l3 = identity_page(l3_pa);
    if (!l3) arm_fail_stop();
    uint64_t *slot = &l3[arena_offset / JE_PAGE_SIZE];
    const uint64_t descriptor = *slot;
    if ((descriptor & ARM_DESC_VALID) == 0u) arm_fail_stop();
    *slot = 0u;
    if (je_arm_mmu_is_enabled(&je_arm_mmu_context) &&
        je_arm_mmu_flush_va_asid(JE_VM_ARENA_BASE + arena_offset, 1u) !=
            JE_OK)
        arm_fail_stop();
    const uint64_t bits = descriptor & ~ARM_DESC_ADDR_MASK;
    if (bits == JE_ARM_USER_CODE_BITS ||
        bits == JE_ARM_USER_RODATA_BITS) {
        if (je_arm_mmu_unpublish_user_frame(
                &je_arm_mmu_context, descriptor & ARM_DESC_ADDR_MASK) != JE_OK)
            arm_fail_stop();
    }
    identity_page_release(l3_pa);
}

static void arm_restore_alias(void *ctx, je_vm_ref frame)
{
    arm_vm_state *state = ctx;
    arm_extent *extent = extent_of(state, frame.process, frame.extent);
    if (!extent || !user_kind(extent->kind) ||
        extent->kind == JE_VM_USER_DATA || frame.page >= extent->pages)
        for (;;) __asm__ volatile("wfe");
    uint64_t physical = extent_frame(extent, frame.page);
    if (!in_phys_window(physical)) for (;;) __asm__ volatile("wfe");
    state->alias_state[(physical - JE_PHYSICAL_BASE) / JE_PAGE_SIZE] = 0u;
}

static void arm_free_extent(void *ctx, uint32_t process, uint32_t id)
{
    arm_vm_state *state = ctx;
    arm_extent *extent = extent_of(state, process, id);
    if (!extent) for (;;) __asm__ volatile("wfe");
    zero_extent(extent);
    if (je_pages_free(state->pages, extent->physical, extent->pages) != JE_OK)
        for (;;) __asm__ volatile("wfe");
    extent->used = false;
}

static void arm_release_hold(void *ctx, uint64_t physical, size_t pages)
{
    arm_vm_state *state = ctx;
    if (!state || !in_phys_window(physical) ||
        pages == 0u || pages > (JE_PHYSICAL_LIMIT - physical) / JE_PAGE_SIZE)
        for (;;) __asm__ volatile("wfe");
    for (size_t page = 0u; page < pages; ++page) {
        const uint64_t pa = physical + (uint64_t)page * JE_PAGE_SIZE;
        uint64_t *mapped = identity_page(pa);
        if (!mapped) arm_fail_stop();
        for (size_t word = 0u; word < JE_PAGE_SIZE / sizeof(uint64_t);
             ++word)
            mapped[word] = 0u;
        identity_page_release(pa);
    }
    if (je_pages_free(state->pages, physical, pages) != JE_OK)
        for (;;) __asm__ volatile("wfe");
}

static void arm_drop_tables(void *ctx, uint32_t process)
{
    arm_vm_state *state = ctx;
    if (!state || process >= JE_VM_MAX_PROCESSES) return;
    state->process[process].used = false;
    state->process[process].root = 0u;
    state->process[process].table_extent = UINT32_MAX;
}

static void arm_flush(void *ctx)
{
    (void)ctx;
    if (je_arm_mmu_is_enabled(&je_arm_mmu_context))
        __asm__ volatile("dsb ish\n\t"
                         "tlbi vmalle1is\n\t"
                         "dsb ish\n\t"
                         "isb" ::: "memory");
}

static je_status arm_read_user_pte(void *ctx, uint32_t process,
                                    uint64_t arena_offset, uint64_t *out_phys,
                                    je_vm_perm *out_perm, int *out_present)
{
    arm_vm_state *state = ctx;
    if (!out_phys || !out_perm || !out_present ||
        process >= JE_VM_MAX_PROCESSES ||
        arena_offset >= JE_VM_ARENA_SIZE ||
        (arena_offset & (JE_PAGE_SIZE - 1u)) != 0u)
        return JE_ERR_INVALID;
    const arm_process *proc = &state->process[process];
    const arm_extent *tables =
        extent_of_const(state, process, proc->table_extent);
    if (!proc->used || !tables) return JE_ERR_INVALID;
    const uint64_t l3_pa = extent_frame(tables, 3u);
    uint64_t *l3 = identity_page(l3_pa);
    if (!l3) return JE_ERR_INVALID;
    uint64_t *slot = &l3[arena_offset / JE_PAGE_SIZE];
    uint64_t descriptor = *slot;
    if ((descriptor & ARM_DESC_VALID) == 0u) {
        *out_phys = 0u;
        *out_perm = (je_vm_perm)0;
        *out_present = 0;
        identity_page_release(l3_pa);
        return JE_OK;
    }
    if ((descriptor & UINT64_C(3)) != ARM_DESC_TABLE ||
        (descriptor & ARM_DESC_AP1) == 0u) {
        identity_page_release(l3_pa);
        return JE_ERR_INVALID;
    }
    *out_phys = descriptor & ARM_DESC_ADDR_MASK;
    *out_present = 1;
    const bool writable = (descriptor & ARM_DESC_AP2) == 0u;
    const bool executable = (descriptor & ARM_DESC_UXN) == 0u;
    if (writable && executable) {
        identity_page_release(l3_pa);
        return JE_ERR_INVALID;
    }
    *out_perm = executable ? JE_VM_PERM_RX
                           : writable ? JE_VM_PERM_RW_NX : JE_VM_PERM_R_NX;
    identity_page_release(l3_pa);
    return JE_OK;
}

static je_status arm_read_upper(void *ctx, uint32_t process, int level,
                                uint32_t index, uint32_t *out_flags)
{
    arm_vm_state *state = ctx;
    if (!state || !out_flags || process >= JE_VM_MAX_PROCESSES ||
        index >= 512u || !state->process[process].used)
        return JE_ERR_INVALID;
    const arm_process *proc = &state->process[process];
    const arm_extent *tables =
        extent_of_const(state, process, proc->table_extent);
    if (!tables) return JE_ERR_INVALID;
    uint32_t table_page;
    switch (level) {
    case 4: table_page = 0u; break;
    case 3: table_page = 1u; break;
    case 2: table_page = 2u; break;
    case 1: table_page = 3u; break;
    default: return JE_ERR_INVALID;
    }
    const uint64_t table_pa = extent_frame(tables, table_page);
    uint64_t *table = identity_page(table_pa);
    if (!table) return JE_ERR_INVALID;
    uint64_t descriptor = table[index];
    /* The arch-neutral flags contract is x86-shaped. ARM table descriptors
     * do not carry leaf AP/XN permissions, and decoding those bits here would
     * report meaningless values. The ARM topology audit bypasses this seam;
     * expose only presence for any legacy caller. */
    *out_flags = (descriptor & ARM_DESC_VALID) != 0u ? 1u : 0u;
    identity_page_release(table_pa);
    return JE_OK;
}

static je_status arm_read_alias(void *ctx, je_vm_ref frame, je_vm_alias *out)
{
    arm_vm_state *state = ctx;
    const arm_extent *extent = extent_of_const(state, frame.process, frame.extent);
    if (!state || !out || !extent || !user_kind(extent->kind) ||
        frame.page >= extent->pages)
        return JE_ERR_INVALID;
    uint64_t physical = extent_frame(extent, frame.page);
    if (!in_phys_window(physical)) return JE_ERR_INVALID;
    *out = state->alias_state[(physical - JE_PHYSICAL_BASE) / JE_PAGE_SIZE] != 0u
               ? JE_VM_ALIAS_RO_NX : JE_VM_ALIAS_RW;
    return JE_OK;
}

static je_status arm_extent_frame_phys(void *ctx, uint32_t process,
                                       uint32_t extent_id, uint32_t page,
                                       uint64_t *out_phys)
{
    arm_vm_state *state = ctx;
    const arm_extent *extent = extent_of_const(state, process, extent_id);
    if (!out_phys || !extent || page >= extent->pages) return JE_ERR_INVALID;
    *out_phys = extent_frame(extent, page);
    return JE_OK;
}

static void arm_backend_panic(void *ctx, const char *why)
{
    (void)ctx;
    (void)why;
    arm_fail_stop();
}

static const je_vm_ops arm_ops = {
    .context = &arm_vm,
    .reserve_hold = arm_reserve_hold,
    .register_extent = arm_register_extent,
    .map = arm_map,
    .link_tables = arm_link_tables,
    .install_pte = arm_install_pte,
    .demote_alias = arm_demote_alias,
    .unmap = arm_unmap,
    .clear_pte = arm_clear_pte,
    .restore_alias = arm_restore_alias,
    .free_extent = arm_free_extent,
    .release_hold = arm_release_hold,
    .drop_tables = arm_drop_tables,
    .flush = arm_flush,
    .read_user_pte = arm_read_user_pte,
    .read_upper = arm_read_upper,
    .read_alias = arm_read_alias,
    .extent_frame_phys = arm_extent_frame_phys,
    .panic = arm_backend_panic,
};

static void *arm_map(void *ctx, je_vm_ref frame)
{
    arm_vm_state *state = ctx;
    const arm_extent *extent = extent_of_const(state, frame.process, frame.extent);
    if (!extent || !user_kind(extent->kind) || frame.page >= extent->pages)
        return NULL;
    const uint64_t physical = extent_frame(extent, frame.page);
    if (je_arm_mmu_is_enabled(&je_arm_mmu_context) &&
        je_arm_mmu_map_page(&je_arm_mmu_context, physical, physical,
                            JE_MAP_RW) != JE_OK)
        return NULL;
    return phys_ptr(physical);
}

/*
 * Validate a shared kernel subtree without recursion. The L0 descriptor is
 * allowed to carry the MP3a hierarchical deny bits; all child table
 * descriptors remain ordinary table descriptors. Every shared leaf must keep
 * AP[1]=0, so a stale user bit in any kernel mapping fails closed.
 */
typedef struct {
    uint64_t physical;
    unsigned level;
} arm_audit_node;

static bool audit_kernel_subtree(uint64_t root_descriptor)
{
    arm_audit_node work[256];
    size_t head = 0u;
    size_t count = 0u;
    if ((root_descriptor & ARM_DESC_VALID) == 0u ||
        (root_descriptor & UINT64_C(3)) != ARM_DESC_TABLE)
        return false;
    work[count++] = (arm_audit_node){
        root_descriptor & ARM_DESC_ADDR_MASK, 1u
    };
    while (head < count) {
        arm_audit_node node = work[head++];
        if (!in_phys_window(node.physical) || node.level > 3u) return false;
        uint64_t *table = phys_ptr(node.physical);
        for (uint32_t index = 0u; index < 512u; ++index) {
            uint64_t descriptor = table[index];
            if ((descriptor & ARM_DESC_VALID) == 0u) continue;
            if (node.level < 3u) {
                if ((descriptor & UINT64_C(3)) != ARM_DESC_TABLE ||
                    (descriptor & (ARM_DESC_TABLE_AP_MASK |
                                   ARM_DESC_TABLE_PXN |
                                   ARM_DESC_TABLE_UXN |
                                   ARM_DESC_TABLE_NS)) != 0u ||
                    count >= sizeof(work) / sizeof(work[0]))
                    return false;
                work[count++] = (arm_audit_node){
                    descriptor & ARM_DESC_ADDR_MASK, node.level + 1u
                };
            } else if ((descriptor & UINT64_C(3)) != ARM_DESC_TABLE ||
                       (descriptor & ARM_DESC_AP1) != 0u) {
                return false;
            }
        }
    }
    return true;
}

static bool own_table_page(const arm_extent *tables, uint32_t page,
                           uint64_t physical)
{
    return tables != NULL && tables->kind == JE_VM_TABLE &&
           tables->pages == JE_VM_TABLE_PAGES && page < JE_VM_TABLE_PAGES &&
           in_phys_window(physical) && physical == extent_frame(tables, page);
}

static bool audit_user_branch(const arm_vm_state *state, const je_vm *vm,
                              uint32_t process)
{
    const arm_process *proc = &state->process[process];
    const arm_extent *tables =
        extent_of_const(state, process, proc->table_extent);
    const je_vm_process *record = &vm->procs[process];
    if (!tables || !own_table_page(tables, 0u, proc->root)) return false;
    for (uint32_t page = 1u; page < JE_VM_TABLE_PAGES; ++page)
        if (!own_table_page(tables, page, extent_frame(tables, page)))
            return false;

    /* The arena walk is intentionally restricted to this process's four-page
     * table extent. Validate every child physical address and the physical
     * window before dereferencing it; a descriptor that points at a different
     * table, a user frame, or an unmapped address is never followed. */
    uint64_t *root = phys_ptr(proc->root);
    uint64_t l0 = root[32];
    if ((l0 & ARM_DESC_VALID) == 0u ||
        (l0 & UINT64_C(3)) != ARM_DESC_TABLE ||
        (l0 & ARM_L0_ARENA_PXNTABLE) == 0u ||
        (l0 & (ARM_DESC_TABLE_AP_MASK | ARM_DESC_TABLE_UXN)) != 0u ||
        !own_table_page(tables, 1u, l0 & ARM_DESC_ADDR_MASK))
        return false;
    uint64_t *l1 = phys_ptr(l0 & ARM_DESC_ADDR_MASK);
    for (uint32_t i = 1u; i < 512u; ++i)
        if ((l1[i] & ARM_DESC_VALID) != 0u) return false;
    if ((l1[0] & UINT64_C(3)) != ARM_DESC_TABLE ||
        (l1[0] & (ARM_DESC_TABLE_AP_MASK | ARM_DESC_TABLE_PXN |
                  ARM_DESC_TABLE_UXN | ARM_DESC_TABLE_NS)) != 0u ||
        !own_table_page(tables, 2u, l1[0] & ARM_DESC_ADDR_MASK))
        return false;
    uint64_t *l2 = phys_ptr(l1[0] & ARM_DESC_ADDR_MASK);
    for (uint32_t i = 1u; i < 512u; ++i)
        if ((l2[i] & ARM_DESC_VALID) != 0u) return false;
    if ((l2[0] & UINT64_C(3)) != ARM_DESC_TABLE ||
        (l2[0] & (ARM_DESC_TABLE_AP_MASK | ARM_DESC_TABLE_PXN |
                  ARM_DESC_TABLE_UXN | ARM_DESC_TABLE_NS)) != 0u ||
        !own_table_page(tables, 3u, l2[0] & ARM_DESC_ADDR_MASK))
        return false;
    uint64_t *l3 = phys_ptr(l2[0] & ARM_DESC_ADDR_MASK);

    /* The guard is a mandatory hole and the stack is exactly the declared
     * top-of-arena run. A shifted stack otherwise preserves the leaf count and
     * can evade a count-only audit. */
    if ((l3[JE_VM_GUARD_OFFSET / JE_PAGE_SIZE] & ARM_DESC_VALID) != 0u)
        return false;
    for (uint32_t page = 0u; page < JE_VM_STACK_PAGES; ++page) {
        const size_t index = (size_t)(JE_VM_STACK_OFFSET / JE_PAGE_SIZE) + page;
        if ((l3[index] & ARM_DESC_VALID) == 0u ||
            (l3[index] & ~ARM_DESC_ADDR_MASK) != JE_ARM_USER_DATA_BITS)
            return false;
    }

    uint64_t seen[JE_VM_ARENA_PAGES];
    size_t seen_count = 0u;
    for (uint64_t page = 0u; page < JE_VM_ARENA_PAGES; ++page) {
        uint64_t descriptor = l3[page];
        if ((descriptor & ARM_DESC_VALID) == 0u) continue;
        if ((descriptor & UINT64_C(3)) != ARM_DESC_TABLE ||
            (descriptor & ARM_DESC_ADDR_MASK) >= ARM_PA_LIMIT)
            return false;
        uint64_t physical = descriptor & ARM_DESC_ADDR_MASK;
        for (size_t prior = 0u; prior < seen_count; ++prior)
            if (seen[prior] == physical) return false;
        seen[seen_count++] = physical;

        bool owned = false;
        for (size_t e = 0u; e < record->extent_count; ++e) {
            const je_vm_extent *extent = &record->extents[e];
            if (!user_kind(extent->kind)) continue;
            uint32_t frame_page = 0u;
            arm_extent local = {
                true, process, extent->physical, extent->pages, extent->kind
            };
            if (!physical_in_extent(&local, physical, &frame_page)) continue;
            if ((descriptor & ~ARM_DESC_ADDR_MASK) !=
                user_leaf_bits(extent->kind))
                return false;
            je_vm_ref ref = { process, extent->backend_id, frame_page };
            je_vm_alias alias;
            if (arm_read_alias((void *)state, ref, &alias) != JE_OK ||
                (extent->kind == JE_VM_USER_DATA
                    ? alias != JE_VM_ALIAS_RW : alias != JE_VM_ALIAS_RO_NX))
                return false;
            owned = true;
            break;
        }
        if (!owned) return false;
    }

    uint64_t expected = 0u;
    for (size_t e = 0u; e < record->extent_count; ++e)
        if (user_kind(record->extents[e].kind))
            expected += record->extents[e].pages;
    return (uint64_t)seen_count == expected;
}

je_status je_arch_vm_audit(const je_vm *vm, uint32_t index)
{
    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES ||
        vm->procs[index].state != JE_VM_READY ||
        !arm_vm.process[index].used)
        return JE_ERR_INVALID;

    const arm_process *process = &arm_vm.process[index];
    const arm_extent *tables =
        extent_of_const(&arm_vm, index, process->table_extent);
    if (!tables || !own_table_page(tables, 0u, process->root))
        return JE_ERR_INVALID;

    uint64_t *kernel_root = phys_ptr(arm_vm.kernel_root);
    uint64_t *process_root = phys_ptr(process->root);
    for (uint32_t i = 0u; i < 512u; ++i) {
        if ((kernel_root[i] & ARM_DESC_VALID) != 0u && i >= 32u) return JE_ERR_INVALID;
        if (process_root[i] != kernel_root[i] &&
            i != 32u)
            return JE_ERR_INVALID;
    }
    for (uint32_t i = 0u; i < 32u; ++i) {
        if ((kernel_root[i] & ARM_DESC_VALID) == 0u) continue;
        if ((kernel_root[i] & (ARM_L0_KERNEL_APTABLE |
                               ARM_L0_KERNEL_UXNTABLE)) !=
            (ARM_L0_KERNEL_APTABLE | ARM_L0_KERNEL_UXNTABLE) ||
            !audit_kernel_subtree(kernel_root[i]))
            return JE_ERR_INVALID;
    }
    if (!audit_user_branch(&arm_vm, vm, index)) return JE_ERR_INVALID;

    for (uint32_t other = 0u; other < JE_VM_MAX_PROCESSES; ++other) {
        if (other == index || vm->procs[other].state != JE_VM_READY) continue;
        if (vm->procs[other].root == vm->procs[index].root) return JE_ERR_INVALID;
        const je_vm_process *left = &vm->procs[index];
        const je_vm_process *right = &vm->procs[other];
        for (size_t a = 0u; a < left->extent_count; ++a) {
            for (uint32_t ap = 0u; ap < left->extents[a].pages; ++ap) {
                uint64_t lpa = left->extents[a].physical +
                               (uint64_t)ap * JE_PAGE_SIZE;
                for (size_t b = 0u; b < right->extent_count; ++b)
                    for (uint32_t bp = 0u; bp < right->extents[b].pages; ++bp)
                        if (lpa == right->extents[b].physical +
                                      (uint64_t)bp * JE_PAGE_SIZE)
                            return JE_ERR_INVALID;
            }
        }
    }
    return JE_OK;
}

const je_vm_ops *je_arch_vm_ops(je_pages *pages, int nx_available)
{
    (void)nx_available;
    arm_vm = (arm_vm_state){0};
    arm_vm.pages = pages;
    arm_vm.kernel_root = je_arm_mmu_root(&je_arm_mmu_context);
    arm_vm.kernel_root_decorated = arm_kernel_root_decorated;
    for (uint32_t i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
        arm_vm.process[i].table_extent = UINT32_MAX;
    /* The ARM slice has hardware NX in the pinned profile; keep the argument
     * in the vtable signature for parity with the x86 backend. */
    return &arm_ops;
}
