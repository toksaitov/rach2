/*
 * The x86-64 kernel's layout: linked at KVA_IMAGE plus its physical address
 * and loaded at that physical address, from 1 MiB (cpu.h, loader.s). Each
 * section's load address is its address less KVA_IMAGE, and the sections
 * follow one another in the file as in memory, so a loader that copies the
 * file from the Multiboot header to _load_end, as the a.out kludge asks,
 * puts every byte where the kernel expects it. _load_end and _end are
 * physical addresses: the end of the file's contents and the end of .bss.
 */

ENTRY(_start)

KVA_IMAGE = 0xFFFFFFFF80000000;

SECTIONS {
    . = KVA_IMAGE + 0x100000;

    /* The .x.* forms are what -ffunction-sections and -fdata-sections
       produce; the dead-code report links that way. */
    .text : AT(ADDR(.text) - KVA_IMAGE) {
        _text = .;
        *(.text .text.*)
        _etext = .;
    }

    .rodata ALIGN(0x1000) : AT(ADDR(.rodata) - KVA_IMAGE) {
        _rodata = .;
        *(.rodata .rodata.*)
    }

    .data ALIGN(0x1000) : AT(ADDR(.data) - KVA_IMAGE) {
        _data = .;
        *(.data .data.*)
    }

    _load_end = . - KVA_IMAGE;

    .bss : AT(ADDR(.bss) - KVA_IMAGE) {
        *(.bss .bss.* COMMON)
    }

    _end = . - KVA_IMAGE;

    /* The unwinding tables GCC writes for x86-64 by default: the kernel
       unwinds nothing. */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .note.GNU-stack)
    }
}
