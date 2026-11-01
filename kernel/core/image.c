#include <jeios/image.h>
#include <jeios/svc.h>

extern unsigned char je_user_spawn_child_start[];
extern unsigned char je_user_spawn_child_end[];
extern const unsigned char je_img_c_hello_blob[];
extern const unsigned long je_img_c_hello_len;
extern const je_vm_image je_img_c_hello_desc;
extern const unsigned char je_img_supervisor_blob[];
extern const unsigned long je_img_supervisor_len;
extern const je_vm_image je_img_supervisor_desc;
extern const unsigned char je_img_restart_blob[];
extern const unsigned long je_img_restart_len;
extern const je_vm_image je_img_restart_desc;
extern const unsigned char je_img_survivor_blob[];
extern const unsigned long je_img_survivor_len;
extern const je_vm_image je_img_survivor_desc;
extern const unsigned char je_img_spoofer_blob[];
extern const unsigned long je_img_spoofer_len;
extern const je_vm_image je_img_spoofer_desc;
extern const unsigned char je_img_crashloop_blob[];
extern const unsigned long je_img_crashloop_len;
extern const je_vm_image je_img_crashloop_desc;
extern const unsigned char je_img_broker_service_blob[];
extern const unsigned long je_img_broker_service_len;
extern const je_vm_image je_img_broker_service_desc;
extern const unsigned char je_img_broker_client_blob[];
extern const unsigned long je_img_broker_client_len;
extern const je_vm_image je_img_broker_client_desc;

#ifndef JE_C_HELLO_BLOB_SHA256
#define JE_C_HELLO_BLOB_SHA256 "unavailable"
#endif
const char je_c_hello_blob_sha256[] = JE_C_HELLO_BLOB_SHA256;

#define JE_BUILTIN_IMAGE_COUNT 9u

static je_builtin_image builtin_images[JE_BUILTIN_IMAGE_COUNT] = {
    { .name = "spawn-child" },
    { .name = "c-hello" },
    { .name = "supervisor" },
    { .name = "restart-svc" },
    { .name = "survivor" },
    { .name = "spoofer" },
    { .name = "crashloop" },
    { .name = "broker-service" },
    { .name = "broker-client" }
};
static bool builtin_images_ready;

/*
 * Stage 2 image hook: the supervisor descriptor must reserve one complete,
 * non-executable RW segment for the fixed service report. Keep this predicate
 * pure so it can be exercised independently of image-table initialization.
 */
static bool report_vaddr_in_rw_segment(const je_vm_image *desc)
{
    _Static_assert(sizeof(je_svc_report) == JE_SVC_REPORT_LEN,
                   "service report size");
    _Static_assert((JE_SVC_REPORT_VADDR & (UINT64_C(8) - 1u)) == 0u,
                   "service report alignment");
    _Static_assert((JE_SVC_REPORT_VADDR & (JE_PAGE_SIZE - 1u))
                   + sizeof(je_svc_report) <= JE_PAGE_SIZE,
                   "service report must not straddle a page");

    if (!desc || desc->segment_count == 0u
        || desc->segment_count > JE_VM_MAX_SEGMENTS
        || JE_SVC_REPORT_VADDR < JE_VM_ARENA_BASE
        || JE_SVC_REPORT_VADDR - JE_VM_ARENA_BASE >= JE_VM_ARENA_SIZE)
        return false;

    uint64_t report_offset = JE_SVC_REPORT_VADDR - JE_VM_ARENA_BASE;
    uint64_t page_offset = report_offset & (JE_PAGE_SIZE - 1u);
    if (page_offset > JE_PAGE_SIZE - sizeof(je_svc_report))
        return false;
    if (report_offset > JE_VM_ARENA_SIZE - sizeof(je_svc_report))
        return false;
    if (report_offset > JE_VM_GUARD_OFFSET - sizeof(je_svc_report))
        return false;
    uint64_t report_end = report_offset + sizeof(je_svc_report);
    uint32_t matches = 0u;

    for (uint32_t i = 0u; i < desc->segment_count; ++i) {
        const je_vm_segment *seg = &desc->segments[i];
        if ((seg->perm & (JE_VM_R | JE_VM_W)) != (JE_VM_R | JE_VM_W)
            || (seg->perm & JE_VM_X) != 0u
            || seg->memory_size == 0u
            || seg->target_offset > JE_VM_ARENA_SIZE
            || seg->memory_size > JE_VM_ARENA_SIZE - seg->target_offset)
            continue;
        uint64_t seg_end = seg->target_offset + seg->memory_size;
        if (report_offset >= seg->target_offset && report_end <= seg_end)
            ++matches;
    }
    return matches == 1u;
}

je_status je_builtin_images_init(void)
{
    if (builtin_images_ready) return JE_OK;
    unsigned char *start = je_user_spawn_child_start;
    unsigned char *end = je_user_spawn_child_end;
    if (end <= start) return JE_ERR_INVALID;

    je_builtin_image *image = &builtin_images[JE_IMAGE_SPAWN_CHILD];
    size_t length = (size_t)(end - start);
    image->blob = start;
    image->blob_len = length;
    image->desc = (je_vm_image){0};
    image->desc.version = JE_VM_IMAGE_VERSION;
    image->desc.segment_count = 2u;
    image->desc.entry_offset = 0u;
    image->desc.segments[0] = (je_vm_segment){
        0u, length, 0u, JE_PAGE_SIZE, JE_VM_R | JE_VM_X, 0u
    };
    image->desc.segments[1] = (je_vm_segment){
        0u, 0u, JE_PAGE_SIZE, JE_PAGE_SIZE, JE_VM_R | JE_VM_W, 0u
    };
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_C_HELLO];
    image->blob = je_img_c_hello_blob;
    image->blob_len = (size_t)je_img_c_hello_len;
    image->desc = je_img_c_hello_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_SUPERVISOR];
    image->blob = je_img_supervisor_blob;
    image->blob_len = (size_t)je_img_supervisor_len;
    image->desc = je_img_supervisor_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK
        || !report_vaddr_in_rw_segment(&image->desc))
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_RESTART_SVC];
    image->blob = je_img_restart_blob;
    image->blob_len = (size_t)je_img_restart_len;
    image->desc = je_img_restart_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_SURVIVOR];
    image->blob = je_img_survivor_blob;
    image->blob_len = (size_t)je_img_survivor_len;
    image->desc = je_img_survivor_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_SPOOFER];
    image->blob = je_img_spoofer_blob;
    image->blob_len = (size_t)je_img_spoofer_len;
    image->desc = je_img_spoofer_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_CRASHLOOP];
    image->blob = je_img_crashloop_blob;
    image->blob_len = (size_t)je_img_crashloop_len;
    image->desc = je_img_crashloop_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_BROKER_SVC];
    image->blob = je_img_broker_service_blob;
    image->blob_len = (size_t)je_img_broker_service_len;
    image->desc = je_img_broker_service_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    image = &builtin_images[JE_IMAGE_BROKER_CLI];
    image->blob = je_img_broker_client_blob;
    image->blob_len = (size_t)je_img_broker_client_len;
    image->desc = je_img_broker_client_desc;
    if (je_vm_image_validate(&image->desc, image->blob_len) != JE_OK)
        return JE_ERR_INVALID;
    builtin_images_ready = true;
    return JE_OK;
}

const je_builtin_image *je_builtin_image_get(uint32_t image_id)
{
    if (!builtin_images_ready || image_id >= JE_BUILTIN_IMAGE_COUNT)
        return (const je_builtin_image *)0;
    return &builtin_images[image_id];
}

uint32_t je_builtin_image_count(void)
{
    return builtin_images_ready ? JE_BUILTIN_IMAGE_COUNT : 0u;
}
