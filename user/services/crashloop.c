/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

static const char crashloop_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS crashloop";
static volatile uint32_t crashloop_data __attribute__((used, section(".data")))
    = UINT32_C(0x55667788);

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    (void)crashloop_rodata;
    (void)crashloop_data;
    (void)version; (void)n; (void)h0; (void)h1; (void)h2; (void)h3;
    __asm__ volatile("ud2");
    for (;;) { }
}
