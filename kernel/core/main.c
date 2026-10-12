#include <stdint.h>
#include <jeios/arch.h>
#include <jeios/module.h>
static je_status policy_init(void) { return JE_OK; }
static void policy_stop(void) { /* No resources owned by this stub. */ }
/* The bootstrap passes the Multiboot2 magic and info address. Until a checked
 * memory-map parser exists, never dereference the info pointer or allocate. */
_Noreturn void je_kernel_main(uint32_t magic, uintptr_t boot_info)
{
    (void)boot_info;
    je_status console = je_arch_console_init();
    if (console == JE_OK) je_arch_console_write("JeiOS M0: entered C kernel\r\n");
    if (magic != 0x36d76289u) {
        je_arch_console_write("JeiOS fatal: invalid Multiboot2 magic\r\n");
        je_arch_halt();
    }
    je_arch_console_write(je_arch_has_nx() ?
        "CPU: NX present (not enabled in M0)\r\n" :
        "CPU: legacy profile, NX absent\r\n");
    const je_module modules[] = {{ "policy-placeholder", policy_init, policy_stop }};
    size_t failed;
    je_status status = je_modules_start(modules, 1, &failed);
    if (status != JE_OK) {
        je_arch_console_write("JeiOS fatal: static initialization failed\r\n");
        je_arch_halt();
    }
    je_arch_console_write("JeiOS M0: initialization complete; halted\r\n");
    /* No scheduler, interrupts, userspace or isolation exists yet. */
    je_arch_halt();
}
