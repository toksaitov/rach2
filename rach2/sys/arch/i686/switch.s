# Context switch implementation for i386
#
# void switch_context(process_t *old, process_t *new)
#
# Saves current CPU state to old process, restores from new process.
# Uses stack-based context switching: saves callee-saved registers on
# the kernel stack, then swaps stack pointers.
# Also switches page directories (CR3) when the new process's is not the one
# the processor has loaded.

.text

# Offsets within process_t structure. These mirror struct process in
# proc/process.h; proc/process.c checks each against the compiler's offset
# at build time, so a field added before one of these stops the build.
.set PROC_REGS_ESP, 48     # offset of regs.esp (pid=4 + state=4 + regs offset 40)
.set PROC_KERNEL_STACK, 56 # offset of kernel_stack
.set PROC_TLS_BASE, 80     # offset of tls_base, after the descriptor table's
                           # pointer, the fs record's pointer and in_syscall

# The TLS selector (GDT_TLS_SELECTOR in gdt.h): entry 6 at RPL 3, 0x33.
.set GDT_TLS_SELECTOR, 0x33

.globl switch_context
switch_context:
    # Arguments: old at 4(%esp), new at 8(%esp)
    # At entry: stack has [ret_addr][old][new]

    # Save callee-saved registers on current (old) stack
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    # Get old process pointer and save ESP
    movl 20(%esp), %eax         # old (offset 16 for pushed regs + 4 for ret addr)
    movl %esp, PROC_REGS_ESP(%eax)

    # Get new process pointer
    movl 24(%esp), %eax         # new

    # Update TSS ESP0 with new process's kernel stack
    # This ensures interrupts in user mode use the correct kernel stack
    pushl PROC_KERNEL_STACK(%eax)
    call tss_set_kernel_stack
    addl $4, %esp

    # Reload pointers (stack changed after call)
    movl 20(%esp), %ebx         # old
    movl 24(%esp), %eax         # new

    # Program the incoming process's TLS base into the running processor's TLS
    # descriptor (gdt_set_tls) and refresh %gs, so %gs-relative reads resolve to
    # this process's TCB, the i386 equivalent of Linux load_TLS() on
    # __switch_to. Done with interrupts disabled (the switch can be entered from
    # syscall context with IF=1) so a nested IRQ never observes a half-written
    # descriptor. The actual gs/fs/es/ds the user runs with are restored from
    # the trapframe by the IRQ/syscall epilogue; reloading %gs here keeps the
    # CPU's hidden descriptor cache coherent with the just-rewritten base. The
    # call clobbers eax, ecx and edx, so old and new are reloaded after it.
    pushfl
    cli
    pushl PROC_TLS_BASE(%eax)               # new->tls_base
    call gdt_set_tls
    addl $4, %esp
    movw $GDT_TLS_SELECTOR, %cx
    movw %cx, %gs
    popfl
    movl 20(%esp), %ebx         # old
    movl 24(%esp), %eax         # new

    # Translate through the incoming process's page directory, the kernel's
    # for a record with none (switch_space, context.c), which compares it
    # with the one this processor has loaded. The call clobbers eax, ecx and
    # edx, so new is reloaded after it.
    pushl %eax
    call switch_space
    addl $4, %esp
    movl 24(%esp), %eax         # new

    # Switch to new process's stack
    movl PROC_REGS_ESP(%eax), %esp

    # Restore callee-saved registers from new stack
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    # Return to new process (ret addr is on new stack)
    ret
