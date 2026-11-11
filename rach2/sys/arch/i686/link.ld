/* The i686 kernel's link: its sections from 1 MiB, where the loader puts it. */

ENTRY(_start)

/* Two segments, as the kernel check wants: without them named, the gap that
   an aligned object in .bss opens after .data starts a third. */
PHDRS {
    text PT_LOAD;
    data PT_LOAD;
}

SECTIONS {
    . = 0x100000;

    /* The .x.* forms are what -ffunction-sections and -fdata-sections
       produce; the dead-code report links that way. A plain build has
       only the bare names. */
    .text : {
        *(.text .text.*)
    } :text

    .rodata ALIGN(0x1000) : {
        *(.rodata .rodata.*)
    } :text

    /* _data to _image_end holds the kernel's static data, which the
       lock-order checker keys a lock in by its address (lib/lockdep.c). */
    .data ALIGN(0x1000) : {
        _data = .;
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
    } :data

    _end = .;
    _image_end = .;
}
