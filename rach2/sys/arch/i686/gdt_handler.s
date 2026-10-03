.text

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

# Load Task Register with TSS selector
.globl _load_tss
_load_tss:
    movw 4(%esp), %ax
    ltr %ax
    ret
