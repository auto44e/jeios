ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}
SECTIONS {
  . = 1M;
  .multiboot2 : { KEEP(*(.multiboot2)) } :text
  .text ALIGN(4096) : { *(.text.boot) *(.text .text.*) } :text
  .rodata ALIGN(4096) : { *(.rodata .rodata.*) } :rodata
  .data ALIGN(4096) : { *(.data .data.*) } :data
  .bss ALIGN(4096) : { *(COMMON) *(.bss .bss.*) } :data
  ASSERT(. < 0x40000000, "kernel exceeds bootstrap identity mapping")
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
