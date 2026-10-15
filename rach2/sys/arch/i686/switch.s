# switch_context(process_t *old, process_t *new): the i686 context switch.
#
# It pushes the callee-saved registers on old's kernel stack and keeps the
# stack pointer in old's record, gives the TSS new's kernel stack for the
# next entry from user mode, writes new's TLS base into this processor's TLS
# entry and reloads %gs, has switch_space (context.c) load new's page
# directory when this processor has another, and pops new's registers from
# new's stack. The ret returns where new last called switch_context, or
# where arch_context_init or arch_fork_frame (context.c) left its first
# return. The segment registers user mode runs with come back from the
# frame of the entry; reloading %gs here refreshes the processor's copy of
# the TLS descriptor, which the write to the GDT does not change.

.text

# The process record's offsets; context.c checks them against the
# compiler's at build time, and arch_context_check at boot.
.set PROC_REGS_ESP, 48     # regs.esp
.set PROC_KERNEL_STACK, 56 # kernel_stack
.set PROC_TLS_BASE, 80     # tls_base

# The TLS selector (GDT_TLS_SELECTOR in gdt.h): entry 6 at RPL 3, 0x33.
.set GDT_TLS_SELECTOR, 0x33

.globl switch_context
switch_context:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    movl 20(%esp), %eax         # old, above the four registers and the return
    movl %esp, PROC_REGS_ESP(%eax)

    movl 24(%esp), %eax         # new

    pushl PROC_KERNEL_STACK(%eax)
    call tss_set_kernel_stack
    addl $4, %esp

    movl 20(%esp), %ebx         # old
    movl 24(%esp), %eax         # new

    # With interrupts off, so that none finds the TLS entry half written.
    pushfl
    cli
    pushl PROC_TLS_BASE(%eax)
    call gdt_set_tls
    addl $4, %esp
    movw $GDT_TLS_SELECTOR, %cx
    movw %cx, %gs
    popfl
    movl 20(%esp), %ebx         # old
    movl 24(%esp), %eax         # new

    pushl %eax
    call switch_space
    addl $4, %esp
    movl 24(%esp), %eax         # new

    movl PROC_REGS_ESP(%eax), %esp

    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    ret
