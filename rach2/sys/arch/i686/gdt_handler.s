# Loading the GDT and the task register, for gdt.c and tss.c.

.text

# void _load_gdt(gdt_ptr_t *p): lgdt, then the flat kernel selectors in CS,
# DS, ES, GS and SS, and the processor's own segment in FS.
.globl _load_gdt
_load_gdt:
    movl 4(%esp), %eax
    lgdt (%eax)

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %gs
    movw %ax, %ss
    # The processor's own segment (GDT_PERCPU_SELECTOR in gdt.h).
    movw $0x40, %ax
    movw %ax, %fs
    ljmp $0x08, $.set_cs

.set_cs:
    ret

# void _load_tss(uint16_t selector): ltr.
.globl _load_tss
_load_tss:
    movw 4(%esp), %ax
    ltr %ax
    ret
