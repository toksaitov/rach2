/* A layout of the shape the x86-64 kernel's link.ld has, with what TinyCC's
   reader of it must also take: the headers in the first segment, a .text
   aligned past them, a symbol a reference reaches PC-relative, space a
   section reserves after its contents, and assignments after SECTIONS. */
ENTRY(_start)
BASE = 0x400000;
PHDRS {
    text PT_LOAD FILEHDR PHDRS;
    data PT_LOAD;
}
HEADERS = SIZEOF_HEADERS;
SECTIONS {
    . = BASE + SIZEOF_HEADERS;
    .text : { *(.text) } :text
    .rodata : { *(.rodata) } :text
    .data ALIGN(0x1000) : { *(.data) } :data
    data_base = ADDR(.data);
    .bss : {
        *(.bss)
        . = ALIGN(0x1000);
        stack_bottom = .;
        . = . + 0x1000;
        stack_top = .;
    } :data
}
image_end = .;
image_marker = 123;
