# An AP start trampoline in the shape the kernel's will have: copied to
# TRAMPOLINE below 1 MiB and entered in real mode at its first byte, it
# loads a GDT of its own, enters protected mode, then long mode, and jumps
# to the kernel. Every address it takes inside itself is the distance of a
# label from trampoline_start, most of them forward, plus TRAMPOLINE.
#
# The instructions are ones GNU as and TinyCC encode alike, so that the two
# objects, linked, must be equal byte for byte: an immediate goes to %edx
# rather than %eax, and a segment register is loaded from %eax with movl.

.set TRAMPOLINE, 0x8000
.set CR0_PE, 1
.set CR0_PG, 0x80000000
.set CR4_PAE, 0x20
.set MSR_EFER, 0xC0000080

.text
.globl trampoline_start
trampoline_start:
.code16
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $(trampoline_stack - trampoline_start + TRAMPOLINE), %sp
    lgdtl trampoline_gdt_ptr - trampoline_start + TRAMPOLINE
    movl %cr0, %edx
    orl $CR0_PE, %edx
    movl %edx, %cr0
    ljmpl $0x08, $(trampoline_32 - trampoline_start + TRAMPOLINE)

.code32
trampoline_32:
    movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    movl trampoline_cr3 - trampoline_start + TRAMPOLINE, %eax
    movl %eax, %cr3
    movl %cr4, %edx
    orl $CR4_PAE, %edx
    movl %edx, %cr4
    movl $MSR_EFER, %ecx
    rdmsr
    btsl $8, %eax
    wrmsr
    movl %cr0, %edx
    orl $CR0_PG, %edx
    movl %edx, %cr0
    ljmp $0x18, $(trampoline_64 - trampoline_start + TRAMPOLINE)

.code64
trampoline_64:
    movq trampoline_entry - trampoline_start + TRAMPOLINE, %rax
    movq trampoline_rsp - trampoline_start + TRAMPOLINE, %rsp
    jmp *%rax

.balign 8, 0
trampoline_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    .quad 0x00AF9A000000FFFF
trampoline_gdt_end:
trampoline_gdt_ptr:
    .word trampoline_gdt_end - trampoline_gdt - 1
    .long trampoline_gdt - trampoline_start + TRAMPOLINE
trampoline_cr3:
    .long 0
trampoline_entry:
    .quad 0
trampoline_rsp:
    .quad 0
trampoline_length:
    .long trampoline_end - trampoline_start
    .word trampoline_end - trampoline_start
    .quad trampoline_end - trampoline_start
    .skip 64
trampoline_stack:
.globl trampoline_end
trampoline_end:
