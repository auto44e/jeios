/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted contract and failure tests for kernel/core/usercopy.c.
 *
 * The fake scheduler and one-page PTE backend model only the symbols that
 * usercopy.c consumes. The copy-run fake records its arguments, optionally
 * probes the active scope through je_usercopy_try_fixup(), and either copies
 * bytes or reports a fault. No static copy scope is exposed directly.
 */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <jeios/status.h>
#include <jeios/usercopy.h>
#include <jeios/vm.h>

/* Addresses are intentionally separated so "nearby RIP" cannot equal another
 * exact stub address when the linker lays these test symbols out. */
unsigned char je_copy_user_load_rip[1] __attribute__((aligned(16)));
unsigned char je_copy_user_store_rip[1] __attribute__((aligned(16)));
unsigned char je_copy_user_fixup[1] __attribute__((aligned(16)));

typedef struct {
    int present;
    je_vm_perm perm;
} fake_pte;

static fake_pte g_pages[JE_VM_ARENA_PAGES];
static je_vm_ops g_ops;
static je_vm g_vm;
static je_vm g_other_vm;
static je_vm g_no_ops_vm;

static je_status g_sched_status;
static je_vm *g_sched_vm;
static uint32_t g_sched_index;
static size_t g_sched_calls;

typedef struct {
    void *dst;
    const void *src;
    uint64_t len;
} copy_record;

#define COPY_RECORD_MAX 8u
static copy_record g_copy_records[COPY_RECORD_MAX];
static size_t g_copy_record_count;
static int g_copy_fault;
static void (*g_copy_hook)(void);

static void *g_arena;

static je_status fake_read_user_pte(void *ctx, uint32_t process,
                                    uint64_t offset, uint64_t *out_phys,
                                    je_vm_perm *out_perm, int *out_present)
{
    size_t page;

    (void)process;
    if (ctx != (void *)g_pages || out_phys == NULL || out_perm == NULL ||
        out_present == NULL || offset >= JE_VM_ARENA_SIZE ||
        (offset & (JE_PAGE_SIZE - 1u)) != 0u) {
        return JE_ERR_INVALID;
    }
    page = (size_t)(offset / JE_PAGE_SIZE);
    if (page >= (size_t)JE_VM_ARENA_PAGES) return JE_ERR_INVALID;
    *out_phys = JE_VM_ARENA_BASE + offset;
    *out_perm = g_pages[page].perm;
    *out_present = g_pages[page].present;
    return JE_OK;
}

static void reset_run(void)
{
    memset(g_copy_records, 0, sizeof(g_copy_records));
    g_copy_record_count = 0u;
    g_copy_fault = 0;
    g_copy_hook = NULL;
}

int je_copy_user_run(void *dst, const void *src, uint64_t len)
{
    assert(g_copy_record_count < (size_t)COPY_RECORD_MAX);
    g_copy_records[g_copy_record_count] = (copy_record){ dst, src, len };
    ++g_copy_record_count;
    if (g_copy_hook != NULL) g_copy_hook();
    if (g_copy_fault != 0) return 1;
    assert(len <= (uint64_t)SIZE_MAX);
    memcpy(dst, src, (size_t)len);
    return 0;
}

je_status je_sched_current_user(je_vm **out_vm, uint32_t *out_index)
{
    ++g_sched_calls;
    if (g_sched_status != JE_OK) return g_sched_status;
    *out_vm = g_sched_vm;
    *out_index = g_sched_index;
    return JE_OK;
}

static void select_current(je_status status, je_vm *vm, uint32_t index)
{
    g_sched_status = status;
    g_sched_vm = vm;
    g_sched_index = index;
}

static void reset_fixture(void)
{
    size_t page;

    for (page = 0u; page < (size_t)JE_VM_ARENA_PAGES; ++page) {
        g_pages[page].present = 0;
        g_pages[page].perm = JE_VM_PERM_R_NX;
    }
    g_ops = (je_vm_ops){0};
    g_ops.context = (void *)g_pages;
    g_ops.read_user_pte = fake_read_user_pte;
    g_vm = (je_vm){0};
    g_vm.ops = &g_ops;
    g_other_vm = (je_vm){0};
    g_other_vm.ops = &g_ops;
    g_no_ops_vm = (je_vm){0};
    select_current(JE_OK, &g_vm, 7u);
    g_sched_calls = 0u;
    reset_run();
}

static void set_page(size_t page, int present, je_vm_perm perm)
{
    assert(page < (size_t)JE_VM_ARENA_PAGES);
    g_pages[page].present = present;
    g_pages[page].perm = perm;
}

static uint64_t user_addr(size_t offset)
{
    assert(offset < (size_t)JE_VM_ARENA_SIZE);
    return JE_VM_ARENA_BASE + (uint64_t)offset;
}

static je_trap_frame make_frame(uint64_t rip, uint64_t vector, uint64_t cs)
{
    je_trap_frame frame = {0};
    frame.rip = rip;
    frame.vector = vector;
    frame.cs = cs;
    return frame;
}

static void expect_fixup_variant(uint64_t rip, uint64_t vector, uint64_t cs,
                                 uint64_t cr2, uint64_t pfec, int expected)
{
    je_trap_frame frame = make_frame(rip, vector, cs);
    const uint64_t original_rip = frame.rip;
    const int actual = je_usercopy_try_fixup(&frame, cr2, pfec);

    assert(actual == expected);
    if (expected != 0)
        assert(frame.rip == (uint64_t)(uintptr_t)je_copy_user_fixup);
    else
        assert(frame.rip == original_rip);
}

static void expect_fixup(uint64_t rip, uint64_t cr2, uint64_t pfec, int expected)
{
    expect_fixup_variant(rip, 14u, 8u, cr2, pfec, expected);
}

static void expect_inactive(void)
{
    expect_fixup((uint64_t)(uintptr_t)je_copy_user_load_rip,
                 JE_VM_ARENA_BASE, 0u, 0);
}

static void test_zero_length(void)
{
    unsigned char kernel[8] = {0};

    reset_fixture();
    assert(je_copy_from_user(kernel, JE_VM_ARENA_BASE, 0u) == JE_ABI_OK);
    assert(je_copy_to_user(JE_VM_ARENA_BASE, kernel, 0u) == JE_ABI_OK);
    assert(je_copy_from_user(NULL, JE_VM_ARENA_BASE, 0u) == JE_ABI_OK);
    assert(je_copy_to_user(JE_VM_ARENA_BASE, NULL, 0u) == JE_ABI_OK);
    assert(g_copy_record_count == 0u);
    assert(g_sched_calls == 0u);
    expect_inactive();
}

static void test_null_kernel_buffer(void)
{
    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    assert(je_copy_from_user(NULL, JE_VM_ARENA_BASE, 1u) == JE_ABI_INVALID);
    assert(g_copy_record_count == 0u);
    assert(g_sched_calls == 0u);
    expect_inactive();

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    assert(je_copy_to_user(JE_VM_ARENA_BASE, NULL, 1u) == JE_ABI_INVALID);
    assert(g_copy_record_count == 0u);
    assert(g_sched_calls == 0u);
    expect_inactive();
}

static void test_no_current_user(void)
{
    unsigned char kernel[1] = {0};

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_R_NX);
    select_current(JE_ERR_INVALID, &g_vm, 7u);
    assert(je_copy_from_user(kernel, JE_VM_ARENA_BASE, 1u) == JE_ABI_INVALID);
    assert(g_sched_calls == 1u);
    assert(g_copy_record_count == 0u);
    expect_inactive();

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_R_NX);
    select_current(JE_OK, NULL, 7u);
    assert(je_copy_from_user(kernel, JE_VM_ARENA_BASE, 1u) == JE_ABI_INVALID);
    assert(g_sched_calls == 1u);
    assert(g_copy_record_count == 0u);
    expect_inactive();

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_R_NX);
    select_current(JE_OK, &g_no_ops_vm, 7u);
    assert(je_copy_from_user(kernel, JE_VM_ARENA_BASE, 1u) == JE_ABI_INVALID);
    assert(g_sched_calls == 1u);
    assert(g_copy_record_count == 0u);
    expect_inactive();
}

static void expect_structural_reject(uint64_t address, uint64_t len)
{
    unsigned char kernel[8] = {0};

    reset_fixture();
    assert(je_copy_from_user(kernel, address, len) == JE_ABI_INVALID);
    assert(g_sched_calls == 1u);
    assert(g_copy_record_count == 0u);
    expect_inactive();
}

static void test_span_structure(void)
{
    expect_structural_reject(JE_VM_ARENA_BASE - UINT64_C(1), 1u);
    expect_structural_reject(JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE, 1u);
    expect_structural_reject(UINT64_MAX - UINT64_C(3), 8u);
    expect_structural_reject(UINT64_C(0x0000800000000000), 1u);
}

static void test_span_pages(void)
{
    unsigned char kernel[8] = {0};
    const unsigned char source[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RX);
    set_page(1u, 1, JE_VM_PERM_R_NX);
    set_page(2u, 1, JE_VM_PERM_RW_NX);
    assert(je_copy_from_user(kernel, user_addr(0u), 1u) == JE_ABI_OK);
    assert(je_copy_from_user(kernel, user_addr((size_t)JE_PAGE_SIZE), 1u) == JE_ABI_OK);
    assert(je_copy_from_user(kernel, user_addr(2u * (size_t)JE_PAGE_SIZE), 1u) == JE_ABI_OK);
    assert(g_copy_record_count == 3u);

    assert(je_copy_to_user(user_addr(0u), source, 1u) == JE_ABI_INVALID);
    assert(je_copy_to_user(user_addr((size_t)JE_PAGE_SIZE), source, 1u) == JE_ABI_INVALID);
    assert(je_copy_to_user(user_addr(2u * (size_t)JE_PAGE_SIZE), source, 1u) == JE_ABI_OK);
    assert(g_copy_record_count == 4u);

    reset_fixture();
    set_page(3u, 0, JE_VM_PERM_RW_NX);
    assert(je_copy_from_user(kernel, user_addr(3u * (size_t)JE_PAGE_SIZE), 1u) ==
           JE_ABI_INVALID);
    assert(g_copy_record_count == 0u);
    expect_inactive();
}

static void test_page_crossing(void)
{
    unsigned char kernel[8] = {0};
    const unsigned char source[8] = {9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u};
    const size_t crossing = (size_t)JE_PAGE_SIZE - 2u;

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    set_page(1u, 1, JE_VM_PERM_RW_NX);
    assert(je_copy_from_user(kernel, user_addr(crossing), 4u) == JE_ABI_OK);
    assert(je_copy_to_user(user_addr(crossing), source, 4u) == JE_ABI_OK);
    assert(g_copy_record_count == 2u);

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    set_page(1u, 0, JE_VM_PERM_RW_NX);
    assert(je_copy_from_user(kernel, user_addr(crossing), 4u) == JE_ABI_INVALID);
    assert(g_copy_record_count == 0u);
    expect_inactive();
}

static void assert_scope_cleared(uint64_t rip, uint64_t address)
{
    expect_fixup(rip, address, 0u, 0);
}

static void test_happy_and_fault_paths(void)
{
    unsigned char kernel[8] = {0};
    const unsigned char source[8] = {0xa1u, 0xb2u, 0xc3u, 0xd4u,
                                     0xe5u, 0xf6u, 0x17u, 0x28u};
    const uint64_t address = user_addr(64u);

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    memcpy((void *)(uintptr_t)address, source, sizeof(source));
    assert(je_copy_from_user(kernel, address, sizeof(kernel)) == JE_ABI_OK);
    assert(memcmp(kernel, source, sizeof(kernel)) == 0);
    assert(g_copy_record_count == 1u);
    assert(g_copy_records[0].dst == kernel);
    assert(g_copy_records[0].src == (const void *)(uintptr_t)address);
    assert(g_copy_records[0].len == (uint64_t)sizeof(kernel));
    assert_scope_cleared((uint64_t)(uintptr_t)je_copy_user_load_rip, address);

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    memset((void *)(uintptr_t)address, 0, sizeof(source));
    assert(je_copy_to_user(address, source, sizeof(source)) == JE_ABI_OK);
    assert(memcmp((void *)(uintptr_t)address, source, sizeof(source)) == 0);
    assert(g_copy_record_count == 1u);
    assert(g_copy_records[0].dst == (void *)(uintptr_t)address);
    assert(g_copy_records[0].src == source);
    assert(g_copy_records[0].len == (uint64_t)sizeof(source));
    assert_scope_cleared((uint64_t)(uintptr_t)je_copy_user_store_rip, address);

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_R_NX);
    g_copy_fault = 1;
    assert(je_copy_from_user(kernel, address, sizeof(kernel)) == JE_ABI_INVALID);
    assert(g_copy_record_count == 1u);
    assert_scope_cleared((uint64_t)(uintptr_t)je_copy_user_load_rip, address);

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    g_copy_fault = 1;
    assert(je_copy_to_user(address, source, sizeof(source)) == JE_ABI_INVALID);
    assert(g_copy_record_count == 1u);
    assert_scope_cleared((uint64_t)(uintptr_t)je_copy_user_store_rip, address);
}

static uint64_t g_outer_address;
static unsigned char g_nested_kernel[8];
static je_abi_status g_nested_status;
static int g_outer_probe;
static int g_hook_depth;

static void nested_copy_hook(void)
{
    je_trap_frame frame;

    if (g_hook_depth != 0) return;
    g_hook_depth = 1;
    /* Nest a 1-byte copy at the OUTER base. If the reject wrongly clobbered the
     * scope, it would now describe [outer, outer+1). We later probe at
     * outer+7 — inside the outer 8-byte span but OUTSIDE any clobbered 1-byte
     * span — so the probe distinguishes an intact outer scope (returns 1) from a
     * clobbered one (would return 0). */
    g_nested_status = je_copy_from_user(g_nested_kernel, g_outer_address, 1u);
    frame = make_frame((uint64_t)(uintptr_t)je_copy_user_load_rip, 14u, 8u);
    g_outer_probe = je_usercopy_try_fixup(&frame, g_outer_address + 7u, 0u);
    assert(frame.rip == (uint64_t)(uintptr_t)je_copy_user_fixup);
    /* And a probe at the nested base+... that would be OUTSIDE the outer span is
     * not available here (outer starts at g_outer_address); instead confirm a
     * clobbered 1-byte span would NOT cover outer+7: an address one past a
     * clobbered [outer,outer+1) is outer+1, already < outer+7, so the +7 probe
     * above is the discriminator. */
    g_hook_depth = 0;
}

static void test_nonnesting(void)
{
    unsigned char outer_kernel[8] = {0};

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_R_NX);
    g_outer_address = user_addr(96u);
    memset(g_nested_kernel, 0, sizeof(g_nested_kernel));
    g_nested_status = JE_ABI_OK;
    g_outer_probe = 0;
    g_hook_depth = 0;
    g_copy_hook = nested_copy_hook;
    assert(je_copy_from_user(outer_kernel, g_outer_address, sizeof(outer_kernel)) ==
           JE_ABI_OK);
    assert(g_nested_status == JE_ABI_INVALID);
    assert(g_outer_probe == 1);
    assert(g_copy_record_count == 1u);
    assert_scope_cleared((uint64_t)(uintptr_t)je_copy_user_load_rip, g_outer_address);
}

static uint64_t g_matrix_lo;
static uint64_t g_matrix_hi;

static void from_fixup_matrix(void)
{
    const uint64_t load = (uint64_t)(uintptr_t)je_copy_user_load_rip;
    const uint64_t store = (uint64_t)(uintptr_t)je_copy_user_store_rip;

    expect_fixup(load, g_matrix_lo, 0u, 1);
    expect_fixup(load, g_matrix_hi - UINT64_C(1), 0u, 1);
    expect_fixup(load, g_matrix_hi, 0u, 0);
    expect_fixup(load, g_matrix_lo - UINT64_C(1), 0u, 0);
    expect_fixup(load, g_matrix_lo, 0x1u, 0);
    expect_fixup(load, g_matrix_lo, 0x2u, 0);
    expect_fixup(load, g_matrix_lo, 0x8u, 0);
    expect_fixup(load, g_matrix_lo, 0x10u, 0);
    expect_fixup(store, g_matrix_lo, 0x2u, 0);
    expect_fixup(load + UINT64_C(1), g_matrix_lo, 0u, 0);
    expect_fixup_variant(load, 13u, 8u, g_matrix_lo, 0u, 0);
    expect_fixup_variant(load, 14u, 0x33u, g_matrix_lo, 0u, 0);

    g_sched_vm = &g_other_vm;
    expect_fixup(load, g_matrix_lo, 0u, 0);
    g_sched_vm = &g_vm;
    g_sched_index = 8u;
    expect_fixup(load, g_matrix_lo, 0u, 0);
    g_sched_index = 7u;
    g_sched_status = JE_ERR_INVALID;
    expect_fixup(load, g_matrix_lo, 0u, 0);
    g_sched_status = JE_OK;
}

static void to_fixup_matrix(void)
{
    const uint64_t load = (uint64_t)(uintptr_t)je_copy_user_load_rip;
    const uint64_t store = (uint64_t)(uintptr_t)je_copy_user_store_rip;

    expect_fixup(store, g_matrix_lo, 0x2u, 1);
    expect_fixup(store, g_matrix_hi - UINT64_C(1), 0x2u, 1);
    expect_fixup(store, g_matrix_hi, 0x2u, 0);
    expect_fixup(store, g_matrix_lo - UINT64_C(1), 0x2u, 0);
    expect_fixup(store, g_matrix_lo, 0u, 0);
    expect_fixup(load, g_matrix_lo, 0u, 0);
    expect_fixup(load, g_matrix_lo, 0x2u, 0);
    expect_fixup(store + UINT64_C(1), g_matrix_lo, 0x2u, 0);
}

static void test_fixup_predicate(void)
{
    unsigned char kernel[8] = {0};
    const unsigned char source[8] = {0x31u, 0x41u, 0x59u, 0x26u,
                                     0x53u, 0x58u, 0x97u, 0x93u};

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_R_NX);
    g_matrix_lo = user_addr(128u);
    g_matrix_hi = g_matrix_lo + (uint64_t)sizeof(kernel);
    g_copy_hook = from_fixup_matrix;
    assert(je_copy_from_user(kernel, g_matrix_lo, sizeof(kernel)) == JE_ABI_OK);
    assert(g_copy_record_count == 1u);
    expect_inactive();
    assert(je_usercopy_try_fixup(NULL, g_matrix_lo, 0u) == 0);

    reset_fixture();
    set_page(0u, 1, JE_VM_PERM_RW_NX);
    g_matrix_lo = user_addr(160u);
    g_matrix_hi = g_matrix_lo + (uint64_t)sizeof(source);
    g_copy_hook = to_fixup_matrix;
    assert(je_copy_to_user(g_matrix_lo, source, sizeof(source)) == JE_ABI_OK);
    assert(g_copy_record_count == 1u);
    expect_inactive();
}

static void map_arena(void)
{
    const size_t bytes = (size_t)JE_VM_ARENA_SIZE;
    void *wanted = (void *)(uintptr_t)JE_VM_ARENA_BASE;

    g_arena = mmap(wanted, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(g_arena != MAP_FAILED);
    assert(g_arena == wanted);
    memset(g_arena, 0, bytes);
}

static void unmap_arena(void)
{
    assert(munmap(g_arena, (size_t)JE_VM_ARENA_SIZE) == 0);
    g_arena = NULL;
}

int main(void)
{
    map_arena();
    test_zero_length();
    test_null_kernel_buffer();
    test_no_current_user();
    test_span_structure();
    test_span_pages();
    test_page_crossing();
    test_happy_and_fault_paths();
    test_nonnesting();
    test_fixup_predicate();
    unmap_arena();
    puts("test_usercopy: PASS");
    return 0;
}
