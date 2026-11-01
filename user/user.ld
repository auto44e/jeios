/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40000000;
    .text ALIGN(0x1000) :
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text
    .rodata ALIGN(0x1000) :
    {
        *(.rodata .rodata.*)
    } :rodata
    .data ALIGN(0x1000) :
    {
        *(.data .data.*)
    } :data
    /* ADR 0010 D12: the supervisor report is a dedicated RW section at the
     * fixed user virtual address. The linker keeps it below the guard; the
     * kernel re-validates that the containing LOAD is RW and NX. */
    .je_report 0x40010000 : ALIGN(0x8)
    {
        KEEP(*(.je_report.report))
        KEEP(*(.je_report.padding))
        KEEP(*(.je_report.go))
    } :data
    .bss ALIGN(0x1000) (NOLOAD) :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    /DISCARD/ :
    {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
        *(.gnu.property)
        /* Some toolchains emit empty GOT/PLT/IRELATIVE sections even for a
         * -static -no-pie -fno-pic freestanding link. They carry no entries
         * here (verified: no dynamic relocations in the image), so discarding
         * them keeps the three-segment W^X layout and avoids an unplaced-orphan
         * link error on stricter linkers. If real GOT/PLT entries ever appear
         * the discard would drop needed relocations -- check_user_image.py and
         * the kernel validator re-verify segment flags, and a missing symbol
         * would fault loudly at ring-3 entry. */
        *(.got) *(.got.plt) *(.igot.plt)
        *(.plt) *(.plt.*) *(.iplt)
        *(.rela.*) *(.rel.*)
    }
}

ASSERT(. <= 0x401fb000, "ring-3 image reaches the guard page")
