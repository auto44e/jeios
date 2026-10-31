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
    }
}

ASSERT(. <= 0x401fb000, "ring-3 image reaches the guard page")
