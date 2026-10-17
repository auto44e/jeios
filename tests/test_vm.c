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

    init_blob();

    reset_vm(0u);
    before = pages.free_count;
    index = UINT32_MAX;
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 0, &index) ==
           JE_ERR_UNSUPPORTED);
    assert(pages.free_count == before);
    assert(all_processes_free(&vm));

    reset_vm(0u);
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) == JE_OK);
    before = pages.free_count;
    assert(je_vm_build(&vm, &image, blob, sizeof blob, 1, &index) ==
           JE_ERR_BUSY);
    assert(pages.free_count == before);
    assert(je_vm_destroy(&vm, 0u) == JE_OK);
    assert(je_vm_destroy(&vm, 1u) == JE_OK);

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

int main(void)
{
    test_image_validation();
    test_admission();
    test_build_audit_and_teardown();
    test_rollback();
    puts("vm: PASS (validation, admission, audit, permissions, zeroing, rollback, teardown)");
}
