/* ADR 0010 D5 built-in image table. Pre-wiring STUB (lead): empty table until PR-C. */
#include <jeios/image.h>
je_status je_builtin_images_init(void) { return JE_OK; }
const je_builtin_image *je_builtin_image_get(uint32_t image_id) { (void)image_id; return (const je_builtin_image *)0; }
uint32_t je_builtin_image_count(void) { return 0u; }
