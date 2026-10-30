/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP1 step 1 (ADR 0002): ARM64 linker script for the QEMU virt-8.2 target.
 * RAM base is 0x40000000; with text_offset=0x80000 in the Image header QEMU
 * places us at 0x40080000, so we link there and the load address equals the
 * link address (a boot.S self-check enforces this). The whole in-memory extent
 * [_text, _end) is published as __image_size in the Image header, and the linker
 * asserts below keep the stack and BSS inside that extent, 16-byte aligned, and
 * non-overlapping. NOTE: QEMU does NOT use image_size to place the DTB/initrd —
 * on virt it puts them at RAM base + min(ram/2, 128 MiB) (hence dtb=0x48000000
 * at -m 256M), well clear of our image; image_size is nonetheless set correctly
 * per the boot protocol for loaders that DO honor it.
 */
ENTRY(_start)

STACK_SIZE = 0x4000;   /* 16 KiB boot stack, in-image (NOLOAD) */

SECTIONS
{
    . = 0x40080000;
    _text = .;

    .text.boot : { KEEP(*(.text.boot)) }
    . = ALIGN(0x800);                       /* VBAR_EL1 requires 2 KiB alignment */
    .text.vectors : { KEEP(*(.text.vectors)) }
    .text      : { *(.text*) }
    .rodata    : { *(.rodata*) }
    .data      : { *(.data*) }
    . = ALIGN(16);
    _file_end = .;          /* end of file-backed (loaded) content, incl. .data */

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    .stack (NOLOAD) : {
        . = ALIGN(16);
        __stack_bottom = .;
        . += STACK_SIZE;
        . = ALIGN(16);
        __stack_top = .;
    }

    . = ALIGN(16);
    /* _end is IMAGE_END: the exclusive end of the WHOLE in-memory extent
     * (file-backed + NOLOAD .bss incl. the 64 KiB DTB copy buffer + boot stack).
     * boot.S rejects a DTB pointer whose conservative extent intersects
     * [_text, _end), and main.c passes [_text, _end) to je_boot_ctx_check as the
     * kernel_image fact (issue #52). */
    _end = .;
    __image_size = _end - _text;

    /* Build metadata kept in the ELF but outside the loaded image. Debug and
     * DWARF sections are placed (not discarded) so debuggers still work; they
     * carry no VMA in the binary image. */
    .debug_info    0 : { *(.debug_info) }
    .debug_abbrev  0 : { *(.debug_abbrev) }
    .debug_line    0 : { *(.debug_line) }
    .debug_str     0 : { *(.debug_str) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_addr    0 : { *(.debug_addr) }
    .debug_loc     0 : { *(.debug_loc) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_ranges  0 : { *(.debug_ranges) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_frame   0 : { *(.debug_frame) }
    .comment       0 : { *(.comment) }

    /* A correct static freestanding link should resolve all relocations and
     * emit no PLT/GOT; discard any such orphans so they can never land in the
     * image. (--orphan-handling=error makes an UNlisted section a build error.) */
    /DISCARD/ : {
        *(.note*) *(.eh_frame*)
        *(.iplt) *(.rela.iplt) *(.igot.plt) *(.got) *(.got.plt) *(.plt)
        *(.rela*)
    }
}

/* --- Placement invariants (design-pass requirements). --- */
ASSERT((__bss_start   & 0xf) == 0, "BSS start must be 16-byte aligned");
ASSERT((__bss_end     & 0xf) == 0, "BSS end must be 16-byte aligned");
ASSERT((__stack_top   & 0xf) == 0, "stack top must be 16-byte aligned");
ASSERT((__stack_bottom & 0xf) == 0, "stack bottom must be 16-byte aligned");
/* BSS and stack are after the file-backed image... */
ASSERT(__bss_start   >= _file_end, "BSS must follow the file-backed image");
ASSERT(__stack_bottom >= __bss_end, "stack must follow BSS (no overlap)");
/* ...and inside the published in-memory image extent. */
ASSERT(__stack_top <= _end, "stack must lie within the image extent");
ASSERT(__bss_end   <= _end, "BSS must lie within the image extent");
/* The DTB copy buffer (main.c, issue #52) must lie in the cleared BSS, hence
 * inside [_text, _end) — the extent the boot.S guard and the kernel_image fact
 * cover — and be 16-byte aligned. */
ASSERT(je_arm_dtb_copy >= __bss_start, "DTB copy buffer must be in BSS");
ASSERT(je_arm_dtb_copy + 0x10000 <= __bss_end, "DTB copy buffer (64 KiB) must be in BSS");
ASSERT((je_arm_dtb_copy & 0xf) == 0, "DTB copy buffer must be 16-byte aligned");
