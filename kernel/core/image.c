#include <jeios/image.h>

extern unsigned char je_user_spawn_child_start[];
extern unsigned char je_user_spawn_child_end[];

#define JE_BUILTIN_IMAGE_COUNT 1u

static je_builtin_image builtin_images[JE_BUILTIN_IMAGE_COUNT] = {
    { .name = "spawn-child" }
};
static bool builtin_images_ready;

je_status je_builtin_images_init(void)
{
    if (builtin_images_ready) return JE_OK;
    unsigned char *start = je_user_spawn_child_start;
    unsigned char *end = je_user_spawn_child_end;
    if (end <= start) return JE_ERR_INVALID;

    je_builtin_image *image = &builtin_images[0];
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
