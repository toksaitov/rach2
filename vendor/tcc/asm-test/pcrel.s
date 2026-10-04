# Fields whose value is a constant less a label of the section, which an
# assembler writes as a PC-relative relocation against no symbol, with the
# constant in its addend: a 16-bit one, a 32-bit one and a 64-bit one. Linked
# at 1 MiB by GNU ld, the TinyCC object must have GNU's bytes.

.text
.globl pcrel_start
pcrel_start:
    .word 0x1234
pcrel_word:
    .word 0x100000 - pcrel_word
pcrel_long:
    .long 0x100000 - pcrel_long
pcrel_quad:
    .quad 0x100000 - pcrel_quad
