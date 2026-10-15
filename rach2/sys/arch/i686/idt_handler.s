# void _load_idt(idt_ptr_t *p): lidt, for idt.c.

.text

.globl _load_idt
_load_idt:
    movl 4(%esp), %eax
    lidt (%eax)

    ret
