/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}
SECTIONS {
  . = 1M;
  __kernel_start = .;
  .multiboot2 : { KEEP(*(.multiboot2)) } :text
  .text ALIGN(4096) : { *(.text.boot) *(.text .text.*) } :text
  . = ALIGN(4096); __text_end = .;
  .rodata ALIGN(4096) : { *(.rodata .rodata.*) } :rodata
  . = ALIGN(4096); __rodata_end = .;
  .data ALIGN(4096) : { *(.data .data.*) } :data
  .bss ALIGN(4096) : { *(COMMON) *(.bss .bss.*) } :data
  . = ALIGN(4096); __kernel_end = .;
  ASSERT(. < 0x40000000, "kernel exceeds bootstrap identity mapping")
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}

