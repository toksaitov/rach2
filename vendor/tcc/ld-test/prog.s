# The program script.ld lays out: a .text aligned to 32 bytes, so that it
# lies past the headers, an address of a script symbol taken PC-relative,
# and a byte of .rodata, a word of .data and space in .bss.
.text
.balign 32
.globl _start
_start:
    leaq data_base(%rip), %rax
    hlt
.section .rodata
    .byte 1
.data
    .quad 1
.bss
    .skip 16
