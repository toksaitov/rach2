ENTRY(_start)

SECTIONS {
    . = 0x100000;

    /* The .x.* forms are what -ffunction-sections and -fdata-sections
       produce; the dead-code report links that way. A plain build has
       only the bare names. */
    .text : {
        *(.text .text.*)
    }

    .rodata ALIGN(0x1000) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(0x1000) : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
    }

    _end = .;
}
