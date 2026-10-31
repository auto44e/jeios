/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PUSHBACK / minimally different fake hook:
 * the supplied je_vm_ops read interface has no read-only view of a published
 * USER_CODE or USER_RODATA backing page, and map() must reject those pages
 * after their aliases become RO_NX.  This test therefore also requires:
 *
 *     const unsigned char *je_vm_fake_peek(je_vm_ref frame);
 *
 * It returns a read-only pointer to one fake backing page without changing
 * fake state.  Without that hook, the poisoned-backing assertion can cover
 * only USER_DATA and stack pages, not every user page.
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <jeios/alloc.h>
#include <jeios/vm.h>

void je_vm_fake_init(je_vm_ops *ops, je_pages *pages);
void je_vm_fake_fail_at(unsigned n);
size_t je_vm_fake_fallible_count(void);
const unsigned char *je_vm_fake_peek(je_vm_ref frame);

static je_pages pages;
static je_vm_ops ops;
static je_vm vm;
static je_pages pages_before;
static je_vm_process procs_before[JE_VM_MAX_PROCESSES];
static unsigned char blob[256];

static void init_pages(je_pages *out)
{
    je_boot_context boot = {
        .valid = true,
        .map_count = 1u,
        .reserved_count = 1u
    };
    boot.map[0] = (je_memory_range){
        UINT64_C(0x100000), UINT64_C(0x100000), 1u
    };
    boot.reserved[0] = (je_reservation){
        UINT64_C(0x100000), UINT64_C(0x103000)
    };
    assert(je_pages_init(out, &boot) == JE_OK);
    assert(out->free_count == 253u);
}

static void reset_vm(size_t reserve_pages)
{
    init_pages(&pages);
    je_vm_fake_init(&ops, &pages);
    assert(je_vm_init(&vm, &ops, &pages, reserve_pages) == JE_OK);
}

static je_vm_image image_fixture(void)
{
    je_vm_image image = {0};

    image.version = JE_VM_IMAGE_VERSION;
    image.segment_count = 3u;
    image.entry_offset = 0u;
    image.segments[0] = (je_vm_segment){
        .source_offset = 0u,
        .source_size = 16u,
        .target_offset = 0u,
        .memory_size = JE_PAGE_SIZE,
        .perm = JE_VM_R | JE_VM_X,
        .reserved = 0u
    };
    image.segments[1] = (je_vm_segment){
        .source_offset = 32u,
        .source_size = 12u,
        .target_offset = 2u * JE_PAGE_SIZE,
        .memory_size = JE_PAGE_SIZE,
        .perm = JE_VM_R,
        .reserved = 0u
    };
    image.segments[2] = (je_vm_segment){
        .source_offset = 64u,
        .source_size = 8u,
        .target_offset = 4u * JE_PAGE_SIZE,
        .memory_size = JE_PAGE_SIZE,
        .perm = JE_VM_R | JE_VM_W,
        .reserved = 0u
    };
    return image;
}

static void init_blob(void)
{
    size_t i;

    memset(blob, 0, sizeof blob);
    for (i = 0u; i < 16u; ++i)
        blob[i] = (unsigned char)(0x10u + (unsigned)i);
    for (i = 0u; i < 12u; ++i)
        blob[32u + i] = (unsigned char)(0x80u + (unsigned)i);
    for (i = 0u; i < 8u; ++i)
        blob[64u + i] = (unsigned char)(0xe0u + (unsigned)i);
}

static size_t build_need(void)
{
    const size_t image_pages = 3u;
    const size_t stack_pages = 4u;

    return (size_t)JE_VM_TABLE_PAGES + image_pages + stack_pages +
           (size_t)JE_VM_KSTACK_PAGES;
}

static bool all_processes_free(const je_vm *target)
{
    size_t i;

    for (i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
        if (target->procs[i].state != JE_VM_FREE)
            return false;
    return true;
}

static uint32_t find_extent(const je_vm_process *process, je_vm_kind kind,
                            size_t extent_pages)
{
    size_t i;

    for (i = 0u; i < process->extent_count; ++i)
        if (process->extents[i].kind == kind &&
            process->extents[i].pages == extent_pages)
            return (uint32_t)i;
    assert(false);
    return 0u;
}

static bool extents_overlap(const je_vm_extent *left, const je_vm_extent *right)
{
    const uint64_t left_bytes = (uint64_t)left->pages * JE_PAGE_SIZE;
    const uint64_t right_bytes = (uint64_t)right->pages * JE_PAGE_SIZE;
    const uint64_t left_end = left->physical + left_bytes;
    const uint64_t right_end = right->physical + right_bytes;

    return left->physical < right_end && right->physical < left_end;
}

static void assert_pte(uint32_t process, uint64_t varena_offset,
                       uint32_t extent, uint32_t page, je_vm_perm expected)
{
    uint64_t physical;
    uint64_t expected_physical;
    je_vm_perm permission;
    int present;

    assert(ops.read_user_pte(ops.context, process, varena_offset, &physical,
                             &permission, &present) == JE_OK);
    assert(present != 0);
    assert(permission == expected);
    assert(ops.extent_frame_phys(ops.context, process, extent, page,
                                 &expected_physical) == JE_OK);
    assert(physical == expected_physical);
}

static void assert_page_bytes(je_vm_ref frame, const unsigned char *source,
                              uint64_t source_size)
{
    const unsigned char *page = je_vm_fake_peek(frame);
    const size_t page_size = (size_t)JE_PAGE_SIZE;
    const size_t initialized = (size_t)source_size;
    size_t i;

    assert(source_size == (uint64_t)initialized);
    assert(initialized <= page_size);
    assert(page != NULL);
    assert(memcmp(page, source, initialized) == 0);
    for (i = initialized; i < page_size; ++i)
        assert(page[i] == 0u);
}

static void test_image_validation(void)
{
    je_vm_image image = image_fixture();

    init_blob();
    assert(je_vm_image_validate(&image, sizeof blob) == JE_OK);

    image.version = JE_VM_IMAGE_VERSION + 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_UNSUPPORTED);

    image = image_fixture();
    image.segment_count = 0u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image.segment_count = JE_VM_MAX_SEGMENTS + 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.reserved = 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].reserved = 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].perm = 0u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image.segments[0].perm = JE_VM_R | 0x8u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image.segments[0].perm = JE_VM_W | JE_VM_X;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image.segments[0].perm = JE_VM_X;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image.segments[0].perm = JE_VM_W;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].target_offset += 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].memory_size += 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].memory_size = 0u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].source_size = JE_PAGE_SIZE + 1u;
    assert(je_vm_image_validate(&image, SIZE_MAX) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].source_offset = UINT64_MAX - 1u;
    image.segments[0].source_size = 2u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].source_offset = UINT64_MAX - 1u;
    image.segments[0].source_size = 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].source_offset = (uint64_t)sizeof blob;
    image.segments[0].source_size = 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].target_offset = JE_VM_ARENA_SIZE;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image.segments[0].target_offset = 0u;
    image.segments[0].memory_size = UINT64_MAX - UINT64_C(4095);
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[1].target_offset = image.segments[0].target_offset;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].target_offset = JE_VM_GUARD_OFFSET;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].target_offset = JE_VM_STACK_OFFSET;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.entry_offset = image.segments[1].target_offset;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].source_size = 1u;
    image.entry_offset = 1u;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.segments[0].memory_size =
        (uint64_t)JE_VM_MAX_USER_PAGES * JE_PAGE_SIZE;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);

    image = image_fixture();
    image.entry_offset = UINT64_MAX;
    assert(je_vm_image_validate(&image, sizeof blob) == JE_ERR_INVALID);
}

static void test_admission(void)
{
    const size_t need = build_need();
    const je_vm_image image = image_fixture();
    const size_t initial_free = 253u;
    uint32_t index;
    size_t before;
    size_t i;
    bool seen[JE_VM_MAX_PROCESSES] = {false};

    /* The fixture pool must hold P full builds, or BUSY would be masked by NOMEM. */
    assert((size_t)JE_VM_MAX_PROCESSES * need <= initial_free);
    init_blob();

    reset_vm(0u);
    before = pages.free_count;
    index = UINT32_MAX;
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 0, &index) ==
           JE_ERR_UNSUPPORTED);
    assert(pages.free_count == before);
    assert(all_processes_free(&vm));

    /* ADR 0010 D2: exactly JE_VM_MAX_PROCESSES builds are admitted, each in a
     * distinct slot; the (P+1)-th is BUSY with no page or process-table change. */
    reset_vm(0u);
    for (i = 0u; i < JE_VM_MAX_PROCESSES; ++i) {
        index = UINT32_MAX;
        assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
        assert(index < JE_VM_MAX_PROCESSES);
        assert(!seen[index]);
        seen[index] = true;
    }
    assert(pages.free_count == initial_free - (size_t)JE_VM_MAX_PROCESSES * need);
    before = pages.free_count;
    memcpy(procs_before, vm.procs, sizeof procs_before);
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) ==
           JE_ERR_BUSY);
    assert(pages.free_count == before);
    assert(memcmp(vm.procs, procs_before, sizeof procs_before) == 0);
    for (i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
        assert(je_vm_destroy(&vm, (uint32_t)i) == JE_OK);
    assert(pages.free_count == initial_free);
    assert(all_processes_free(&vm));

    reset_vm(initial_free - need + 1u);
    before = pages.free_count;
    index = UINT32_MAX;
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) ==
           JE_ERR_NOMEM);
    assert(pages.free_count == before);
    assert(all_processes_free(&vm));
}

static void test_build_audit_and_teardown(void)
{
    const je_vm_image image = image_fixture();
    const size_t initial_free = 253u;
    const size_t expected_need = build_need();
    uint32_t first;
    uint32_t second;
    uint32_t code_extent;
    uint32_t rodata_extent;
    uint32_t data_extent;
    uint32_t stack_extent;
    size_t i;
    size_t j;

    init_blob();
    reset_vm(0u);
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &first) == JE_OK);
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &second) == JE_OK);
    assert(first != second);
    assert(vm.procs[first].root != vm.procs[second].root);
    assert(pages.free_count == initial_free - 2u * expected_need);
    assert(je_vm_audit(&vm, first) == JE_OK);
    assert(je_vm_audit(&vm, second) == JE_OK);

    for (i = 0u; i < vm.procs[first].extent_count; ++i)
        for (j = 0u; j < vm.procs[second].extent_count; ++j)
            assert(!extents_overlap(&vm.procs[first].extents[i],
                                    &vm.procs[second].extents[j]));

    code_extent = find_extent(&vm.procs[first], JE_VM_USER_CODE, 1u);
    rodata_extent = find_extent(&vm.procs[first], JE_VM_USER_RODATA, 1u);
    data_extent = find_extent(&vm.procs[first], JE_VM_USER_DATA, 1u);
    stack_extent = find_extent(&vm.procs[first], JE_VM_USER_DATA,
                               (size_t)JE_VM_STACK_PAGES);

    assert_pte(first, image.segments[0].target_offset, code_extent, 0u,
               JE_VM_PERM_RX);
    assert_pte(first, image.segments[1].target_offset, rodata_extent, 0u,
               JE_VM_PERM_R_NX);
    assert_pte(first, image.segments[2].target_offset, data_extent, 0u,
               JE_VM_PERM_RW_NX);

    for (i = 0u; i < JE_VM_MAX_SEGMENTS; ++i) {
        uint32_t flags;

        assert(ops.read_upper(ops.context, first, 4, (uint32_t)i, &flags) ==
               JE_OK);
        if (i == 0u)
            assert(flags == 7u);
        else
            assert(flags == 0u);
    }
    {
        uint32_t flags;

        assert(ops.read_upper(ops.context, first, 3, 0u, &flags) == JE_OK);
        assert(flags == 3u);
        assert(ops.read_upper(ops.context, first, 3, 1u, &flags) == JE_OK);
        assert(flags == 7u);
        assert(ops.read_upper(ops.context, first, 2, 0u, &flags) == JE_OK);
        assert(flags == 7u);
    }

    assert_page_bytes((je_vm_ref){first, code_extent, 0u},
                      blob, image.segments[0].source_size);
    assert_page_bytes((je_vm_ref){first, rodata_extent, 0u},
                      blob + image.segments[1].source_offset,
                      image.segments[1].source_size);
    assert_page_bytes((je_vm_ref){first, data_extent, 0u},
                      blob + image.segments[2].source_offset,
                      image.segments[2].source_size);
    for (i = 0u; i < (size_t)JE_VM_STACK_PAGES; ++i)
        assert_page_bytes((je_vm_ref){first, stack_extent, (uint32_t)i},
                          blob, 0u);

    {
        je_vm_alias alias;

        assert(ops.read_alias(ops.context,
                              (je_vm_ref){first, code_extent, 0u},
                              &alias) == JE_OK);
        assert(alias == JE_VM_ALIAS_RO_NX);
        assert(ops.read_alias(ops.context,
                              (je_vm_ref){first, rodata_extent, 0u},
                              &alias) == JE_OK);
        assert(alias == JE_VM_ALIAS_RO_NX);
        assert(ops.read_alias(ops.context,
                              (je_vm_ref){first, data_extent, 0u},
                              &alias) == JE_OK);
        assert(alias == JE_VM_ALIAS_RW);
    }

    assert(je_vm_destroy(&vm, first) == JE_OK);
    assert(je_vm_destroy(&vm, second) == JE_OK);
    assert(pages.free_count == initial_free);
    assert(all_processes_free(&vm));
    assert(je_vm_destroy(&vm, first) == JE_ERR_INVALID);
}

static void test_rollback(void)
{
    const je_vm_image image = image_fixture();
    size_t fallible_count;
    unsigned failure;

    init_blob();
    reset_vm(0u);
    {
        uint32_t index;

        assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) ==
               JE_OK);
    }
    fallible_count = je_vm_fake_fallible_count();
    assert(fallible_count > 0u);
    assert(fallible_count <= (size_t)UINT_MAX);
    assert(je_vm_destroy(&vm, 0u) == JE_OK);

    for (failure = 1u; failure <= (unsigned)fallible_count; ++failure) {
        uint32_t index = UINT32_MAX;
        je_status status;

        reset_vm(0u);
        pages_before = pages;
        memcpy(procs_before, vm.procs, sizeof procs_before);
        je_vm_fake_fail_at(failure);
        status = je_vm_build(&vm, &image, blob, sizeof blob, 1, &index);
        assert(status != JE_OK);
        assert(all_processes_free(&vm));
        assert(memcmp(&pages, &pages_before, sizeof pages) == 0);
        assert(memcmp(vm.procs, procs_before, sizeof procs_before) == 0);
    }
}

/* M3c (ADR 0009 A3-1): prove je_vm_destroy runs the teardown hook BEFORE the
 * table is zeroed, wrapped in a paired irq_save/irq_restore, and that NULL hooks
 * (the default) preserve the prior behaviour. Uses fakes — no endpoint state. */
static int hook_calls, irq_saves, irq_restores, hook_saw_live_table;
static uint64_t fake_irq_save(void) { irq_saves++; return 0x1234u; }
static void fake_irq_restore(uint64_t f) { assert(f == 0x1234u); irq_restores++; }
static void fake_teardown(void *ctx, je_handle_table *t)
{
    (void)ctx;
    hook_calls++;
    /* The hook must run while the process is still intact: its handle table is
     * still initialized (capacity nonzero), i.e. unwind()'s *p={0} has NOT run
     * yet. And irq_save must already have happened, irq_restore not yet. */
    if (t && t->capacity != 0u) hook_saw_live_table = 1;
    assert(irq_saves == 1 && irq_restores == 0);
}

static void test_teardown_hook(void)
{
    const je_vm_image image = image_fixture();
    uint32_t index = UINT32_MAX;
    init_blob();

    /* With the hook + irq pair installed: destroy runs hook before zeroing, and
     * pairs save/restore exactly once. */
    reset_vm(0u);
    vm.teardown = fake_teardown;
    vm.teardown_ctx = (void *)0;
    vm.irq_save = fake_irq_save;
    vm.irq_restore = fake_irq_restore;
    hook_calls = irq_saves = irq_restores = hook_saw_live_table = 0;
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
    assert(je_vm_destroy(&vm, index) == JE_OK);
    assert(hook_calls == 1);               /* hook ran exactly once */
    assert(hook_saw_live_table == 1);      /* BEFORE the table was zeroed */
    assert(irq_saves == 1 && irq_restores == 1);  /* paired */
    assert(all_processes_free(&vm));       /* and the zeroing still happened */

    /* Early return (bad state) must NOT run the hook or touch IRQs. */
    hook_calls = irq_saves = irq_restores = 0;
    assert(je_vm_destroy(&vm, index) == JE_ERR_INVALID);  /* already FREE */
    assert(hook_calls == 0 && irq_saves == 0 && irq_restores == 0);

    /* NULL hooks (the unit-test default elsewhere): destroy still works, no hook,
     * no IRQ calls — the pre-M3c behaviour. */
    reset_vm(0u);   /* zeroes vm, so all four fields are NULL again */
    index = UINT32_MAX;
    hook_calls = irq_saves = irq_restores = 0;
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
    assert(je_vm_destroy(&vm, index) == JE_OK);
    assert(hook_calls == 0 && irq_saves == 0 && irq_restores == 0);
    assert(all_processes_free(&vm));
}

/* ADR 0010 D2: the two-process isolation property above, at the full P. Every
 * pair of the P concurrently READY processes has distinct roots and disjoint
 * extents, each passes the audit, and destroying all returns every page. */
static void test_all_processes_isolated(void)
{
    const je_vm_image image = image_fixture();
    const size_t initial_free = 253u;
    uint32_t idx[JE_VM_MAX_PROCESSES];
    size_t a, b, i, j;

    init_blob();
    reset_vm(0u);
    for (a = 0u; a < JE_VM_MAX_PROCESSES; ++a) {
        assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &idx[a]) == JE_OK);
        assert(idx[a] < JE_VM_MAX_PROCESSES);
    }
    assert(pages.free_count == initial_free - (size_t)JE_VM_MAX_PROCESSES * build_need());
    for (a = 0u; a < JE_VM_MAX_PROCESSES; ++a) {
        assert(je_vm_audit(&vm, idx[a]) == JE_OK);
        for (b = a + 1u; b < JE_VM_MAX_PROCESSES; ++b) {
            const je_vm_process *pa = &vm.procs[idx[a]];
            const je_vm_process *pb = &vm.procs[idx[b]];

            assert(idx[a] != idx[b]);
            assert(pa->root != pb->root);
            assert(pa->kstack_physical != pb->kstack_physical);
            for (i = 0u; i < pa->extent_count; ++i)
                for (j = 0u; j < pb->extent_count; ++j)
                    assert(!extents_overlap(&pa->extents[i], &pb->extents[j]));
        }
    }
    for (a = 0u; a < JE_VM_MAX_PROCESSES; ++a)
        assert(je_vm_destroy(&vm, idx[a]) == JE_OK);
    assert(pages.free_count == initial_free);
    assert(all_processes_free(&vm));
}

/* Fill `t` with SELF handles and prove it admits exactly `quota` LIVE handles:
 * the table reports the quota, allocation succeeds until live == quota, the
 * next one is QUOTA (not NOSLOTS: quota < capacity) with *out 0 and live
 * unchanged, and closing one makes exactly one more admissible. Does not assume
 * how many handles the builder seeds (it may seed none). */
static void assert_admits_exactly(je_handle_table *t, uint32_t quota)
{
    je_handle h = 0u;
    je_handle last = 0u;
    uint32_t start;
    uint32_t k;

    assert(t->capacity == JE_VM_HANDLE_CAP);
    assert(t->quota == quota);
    start = t->live;
    assert(start <= quota);
    for (k = start; k < quota; ++k) {
        assert(je_handle_alloc(t, t, JE_HANDLE_OBJ_SELF, 0x1u, &h) == JE_HANDLE_OK);
        assert(h != 0u);
        last = h;
    }
    assert(t->live == quota);
    h = 1u;
    assert(je_handle_alloc(t, t, JE_HANDLE_OBJ_SELF, 0x1u, &h) == JE_HANDLE_QUOTA);
    assert(h == 0u);
    assert(t->live == quota);
    if (last != 0u) {
        assert(je_handle_close(t, last) == JE_HANDLE_OK);
        assert(t->live == quota - 1u);
        assert(je_handle_alloc(t, t, JE_HANDLE_OBJ_SELF, 0x1u, &h) == JE_HANDLE_OK);
        assert(je_handle_alloc(t, t, JE_HANDLE_OBJ_SELF, 0x1u, &h) == JE_HANDLE_QUOTA);
        assert(t->live == quota);
    }
}

/* One rejected je_vm_build_quota call: INVALID, and NO state change — pages,
 * every process slot, and zero backend (fallible-op) attempts. */
static void assert_quota_rejected(const je_vm_image *image, int nx,
                                  uint32_t quota)
{
    uint32_t index = UINT32_MAX;

    pages_before = pages;
    memcpy(procs_before, vm.procs, sizeof procs_before);
    je_vm_fake_fail_at(0u);   /* reset the attempt counter, inject nothing */
    assert(je_vm_build_quota(&vm, image, blob, sizeof blob, nx, quota, &index) ==
           JE_ERR_INVALID);
    assert(je_vm_fake_fallible_count() == 0u);
    assert(memcmp(&pages, &pages_before, sizeof pages) == 0);
    assert(memcmp(vm.procs, procs_before, sizeof procs_before) == 0);
}

/* ADR 0010 D2 / vm.h: handle_quota outside 1..JE_VM_HANDLE_QUOTA_MAX is INVALID,
 * checked FIRST. Negative cases run first so they also hold against the
 * fail-closed stub; the positive cases need the real implementation. */
static void test_build_quota_range(void)
{
    const je_vm_image good = image_fixture();
    je_vm_image bad_version = image_fixture();
    je_vm_image bad_segments = image_fixture();
    const uint32_t bad[] = { 0u, JE_VM_HANDLE_QUOTA_MAX + 1u, JE_VM_HANDLE_CAP,
                             UINT32_MAX };
    size_t i;

    bad_version.version = JE_VM_IMAGE_VERSION + 1u;   /* would be UNSUPPORTED */
    bad_segments.segment_count = 0u;                  /* would be INVALID */
    _Static_assert(JE_VM_HANDLE_QUOTA_MAX == JE_VM_HANDLE_CAP - 1u, "quota max");
    _Static_assert(JE_VM_HANDLE_QUOTA_DEFAULT >= 1u &&
                   JE_VM_HANDLE_QUOTA_DEFAULT <= JE_VM_HANDLE_QUOTA_MAX, "quota default");
    init_blob();
    reset_vm(0u);
    /* Control: these inputs really do take the other error paths with a valid
     * quota through je_vm_build, so INVALID below is the range check winning. */
    {
        uint32_t index = UINT32_MAX;
        assert(je_vm_build(&vm, &good, blob, sizeof blob, 0, &index) == JE_ERR_UNSUPPORTED);
        assert(je_vm_build(&vm, &bad_version, blob, sizeof blob, 1, &index) ==
               JE_ERR_UNSUPPORTED);
        assert(all_processes_free(&vm));
    }
    for (i = 0u; i < sizeof bad / sizeof bad[0]; ++i) {
        assert_quota_rejected(&good, 1, bad[i]);
        assert_quota_rejected(&good, 0, bad[i]);          /* beats no-NX */
        assert_quota_rejected(&bad_version, 1, bad[i]);   /* beats UNSUPPORTED */
        assert_quota_rejected(&bad_segments, 1, bad[i]);  /* beats image INVALID */
        assert(all_processes_free(&vm));
    }
    /* With every slot READY, an out-of-range quota is still INVALID (not BUSY). */
    {
        uint32_t index;
        for (i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
            assert(je_vm_build(&vm, &good, blob, sizeof blob, 1, &index) == JE_OK);
        assert_quota_rejected(&good, 1, 0u);
        assert_quota_rejected(&good, 1, JE_VM_HANDLE_QUOTA_MAX + 1u);
        for (i = 0u; i < JE_VM_MAX_PROCESSES; ++i)
            assert(je_vm_destroy(&vm, (uint32_t)i) == JE_OK);
    }

    /* In-range boundary quotas are NOT rejected by the range check: they reach
     * the ordinary checks (no NX -> UNSUPPORTED, full table -> BUSY). */
    {
        uint32_t index = UINT32_MAX;
        size_t before = pages.free_count;
        assert(je_vm_build_quota(&vm, &good, blob, sizeof blob, 0, 1u, &index) ==
               JE_ERR_UNSUPPORTED);
        assert(je_vm_build_quota(&vm, &good, blob, sizeof blob, 0,
                                 JE_VM_HANDLE_QUOTA_MAX, &index) == JE_ERR_UNSUPPORTED);
        assert(pages.free_count == before);
        assert(all_processes_free(&vm));
    }
}

static void test_build_quota_admits(void)
{
    const je_vm_image image = image_fixture();
    const size_t initial_free = 253u;
    uint32_t index;

    init_blob();

    /* je_vm_build == je_vm_build_quota(DEFAULT): exactly 15 live handles. */
    _Static_assert(JE_VM_HANDLE_QUOTA_DEFAULT == 15u, "M3b default quota unchanged");
    reset_vm(0u);
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
    assert_admits_exactly(&vm.procs[index].htable, JE_VM_HANDLE_QUOTA_DEFAULT);
    assert(je_vm_destroy(&vm, index) == JE_OK);
    assert(pages.free_count == initial_free);

    /* Same build through the quota entry point at DEFAULT: same observable
     * result (state, entry, pages charged, table shape). */
    {
        je_vm_process via_build;
        uint32_t i2;

        reset_vm(0u);
        assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
        via_build = vm.procs[index];
        reset_vm(0u);
        assert(je_vm_build_quota(&vm, &image, blob, sizeof blob, 1,
                                 JE_VM_HANDLE_QUOTA_DEFAULT, &i2) == JE_OK);
        assert(i2 == index);
        assert(vm.procs[i2].state == via_build.state);
        assert(vm.procs[i2].entry == via_build.entry);
        assert(vm.procs[i2].extent_count == via_build.extent_count);
        assert(vm.procs[i2].journal_count == via_build.journal_count);
        assert(vm.procs[i2].held_kstack == via_build.held_kstack);
        assert(vm.procs[i2].htable.capacity == via_build.htable.capacity);
        assert(vm.procs[i2].htable.quota == via_build.htable.quota);
        assert(vm.procs[i2].htable.live == via_build.htable.live);
        assert(pages.free_count == initial_free - build_need());
        assert(je_vm_audit(&vm, i2) == JE_OK);
        assert_admits_exactly(&vm.procs[i2].htable, JE_VM_HANDLE_QUOTA_DEFAULT);
        assert(je_vm_destroy(&vm, i2) == JE_OK);
    }

    /* Boundaries: quota 1 and quota MAX each build normally and admit exactly
     * that many live handles. */
    {
        const uint32_t q[] = { 1u, JE_VM_HANDLE_QUOTA_MAX };
        size_t i;

        for (i = 0u; i < sizeof q / sizeof q[0]; ++i) {
            reset_vm(0u);
            index = UINT32_MAX;
            assert(je_vm_build_quota(&vm, &image, blob, sizeof blob, 1, q[i],
                                     &index) == JE_OK);
            assert(index < JE_VM_MAX_PROCESSES);
            assert(vm.procs[index].state == JE_VM_READY);
            assert(pages.free_count == initial_free - build_need());
            assert(je_vm_audit(&vm, index) == JE_OK);
            assert_admits_exactly(&vm.procs[index].htable, q[i]);
            assert(je_vm_destroy(&vm, index) == JE_OK);
            assert(pages.free_count == initial_free);
            assert(all_processes_free(&vm));
        }
    }
}

/* P concurrent builds, each with a different quota spanning 1..MAX: every table
 * gets its own quota, filling one table does not change any other, and a slot
 * reused after destroy takes the NEW build's quota (no stale quota). */
static void test_build_quota_independent(void)
{
    const je_vm_image image = image_fixture();
    uint32_t idx[JE_VM_MAX_PROCESSES];
    uint32_t q[JE_VM_MAX_PROCESSES];
    uint32_t live_before[JE_VM_MAX_PROCESSES];
    size_t a, b;

    _Static_assert(JE_VM_MAX_PROCESSES >= 2u, "independence needs two processes");
    _Static_assert(JE_VM_MAX_PROCESSES <= JE_VM_HANDLE_QUOTA_MAX, "distinct quotas");
    init_blob();
    reset_vm(0u);
    for (a = 0u; a < JE_VM_MAX_PROCESSES; ++a) {
        /* q[0] = 1, q[P-1] = MAX, strictly increasing in between. */
        q[a] = 1u + (uint32_t)(a * (JE_VM_HANDLE_QUOTA_MAX - 1u) /
                               (JE_VM_MAX_PROCESSES - 1u));
        if (a > 0u) assert(q[a] > q[a - 1u]);
        assert(je_vm_build_quota(&vm, &image, blob, sizeof blob, 1, q[a],
                                 &idx[a]) == JE_OK);
    }
    assert(q[0] == 1u && q[JE_VM_MAX_PROCESSES - 1u] == JE_VM_HANDLE_QUOTA_MAX);
    for (a = 0u; a < JE_VM_MAX_PROCESSES; ++a)
        assert(vm.procs[idx[a]].htable.quota == q[a]);
    /* Fill in reverse so a later (smaller) quota is checked after a larger one
     * was filled. */
    for (a = JE_VM_MAX_PROCESSES; a-- > 0u;) {
        for (b = 0u; b < JE_VM_MAX_PROCESSES; ++b)
            live_before[b] = vm.procs[idx[b]].htable.live;
        assert_admits_exactly(&vm.procs[idx[a]].htable, q[a]);
        for (b = 0u; b < JE_VM_MAX_PROCESSES; ++b) {
            if (b == a) continue;
            assert(vm.procs[idx[b]].htable.live == live_before[b]);
            assert(vm.procs[idx[b]].htable.quota == q[b]);
        }
    }
    /* Reuse the quota-MAX slot with quota 1 and the quota-1 slot via je_vm_build. */
    {
        const size_t hi = JE_VM_MAX_PROCESSES - 1u;
        uint32_t re;

        assert(je_vm_destroy(&vm, idx[hi]) == JE_OK);
        assert(je_vm_build_quota(&vm, &image, blob, sizeof blob, 1, 1u, &re) == JE_OK);
        assert(re == idx[hi]);
        assert_admits_exactly(&vm.procs[re].htable, 1u);
        assert(je_vm_destroy(&vm, idx[0]) == JE_OK);
        assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &re) == JE_OK);
        assert(re == idx[0]);
        assert_admits_exactly(&vm.procs[re].htable, JE_VM_HANDLE_QUOTA_DEFAULT);
    }
    for (a = 0u; a < JE_VM_MAX_PROCESSES; ++a)
        assert(je_vm_destroy(&vm, idx[a]) == JE_OK);
    assert(pages.free_count == 253u);
    assert(all_processes_free(&vm));
}

/* Rollback through the quota entry point (non-default quota): every injected
 * backend failure leaves pages and every process slot byte-identical. */
static void test_build_quota_rollback(void)
{
    const je_vm_image image = image_fixture();
    size_t fallible_count;
    unsigned failure;
    uint32_t index;

    init_blob();
    reset_vm(0u);
    je_vm_fake_fail_at(0u);
    assert(je_vm_build_quota(&vm, &image, blob, sizeof blob, 1,
                             JE_VM_HANDLE_QUOTA_MAX, &index) == JE_OK);
    fallible_count = je_vm_fake_fallible_count();
    assert(fallible_count > 0u && fallible_count <= (size_t)UINT_MAX);
    assert(je_vm_destroy(&vm, index) == JE_OK);
    for (failure = 1u; failure <= (unsigned)fallible_count; ++failure) {
        reset_vm(0u);
        pages_before = pages;
        memcpy(procs_before, vm.procs, sizeof procs_before);
        je_vm_fake_fail_at(failure);
        index = UINT32_MAX;
        assert(je_vm_build_quota(&vm, &image, blob, sizeof blob, 1,
                                 JE_VM_HANDLE_QUOTA_MAX, &index) != JE_OK);
        assert(all_processes_free(&vm));
        assert(memcmp(&pages, &pages_before, sizeof pages) == 0);
        assert(memcmp(vm.procs, procs_before, sizeof procs_before) == 0);
    }
}

int main(void)
{
    test_image_validation();
    test_admission();
    test_build_audit_and_teardown();
    test_rollback();
    test_teardown_hook();
    test_all_processes_isolated();
    test_build_quota_range();
    test_build_quota_admits();
    test_build_quota_independent();
    test_build_quota_rollback();
    puts("vm: PASS (validation, admission at P, audit, permissions, zeroing, rollback, teardown, death-hook, P-way isolation, handle quota)");
}
