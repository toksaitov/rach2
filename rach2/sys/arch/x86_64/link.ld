/*
 * The x86-64 kernel's layout: linked at KVA_IMAGE plus its physical address
 * and loaded at that physical address, from 1 MiB (cpu.h, loader.s). Each
 * section's load address is its address less KVA_IMAGE.
 *
 * A loader that takes the a.out kludge copies the file from the Multiboot
 * header on, byte for byte, to the header's physical address, up to
 * _load_end, so every byte must lie in the file as far from the header as
 * it lies in memory. The first segment therefore begins at file offset 0
 * with the ELF headers, at physical 1 MiB, and the second follows it: a
 * file offset is then its address less 1 MiB, which is congruent to the
 * address modulo any alignment up to 1 MiB, so the linker leaves no gap a
 * section's alignment would otherwise open between the file and memory.
 * The header, first in .text, lies within the first 8 KiB of the file,
 * where the loaders search. _load_end and _end are physical addresses: the
 * end of the file's contents and the end of .bss.
 */

ENTRY(_start)

KVA_IMAGE = 0xFFFFFFFF80000000;

PHDRS {
    text PT_LOAD FILEHDR PHDRS;
    data PT_LOAD;
}

SECTIONS {
    . = KVA_IMAGE + 0x100000 + SIZEOF_HEADERS;

    /* The .x.* forms are what -ffunction-sections and -fdata-sections
       produce; the dead-code report links that way. */
    .text : AT(ADDR(.text) - KVA_IMAGE) {
        _text = .;
        *(.text .text.*)
        _etext = .;
    } :text

    .rodata ALIGN(0x1000) : AT(ADDR(.rodata) - KVA_IMAGE) {
        _rodata = .;
        *(.rodata .rodata.*)
    } :text

    .data ALIGN(0x1000) : AT(ADDR(.data) - KVA_IMAGE) {
        _data = .;
        *(.data .data.*)
    } :data

    _load_end = . - KVA_IMAGE;

    .bss : AT(ADDR(.bss) - KVA_IMAGE) {
        *(.bss .bss.* COMMON)
    } :data

    _end = . - KVA_IMAGE;

    /* The unwinding tables GCC writes for x86-64 by default: the kernel
       unwinds nothing. */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .note.GNU-stack)
    }
}
