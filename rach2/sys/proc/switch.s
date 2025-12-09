# Context switch implementation for i386
#
# void switch_context(process_t *old, process_t *new)
#
# Saves current CPU state to old process, restores from new process.
# Uses stack-based context switching: saves callee-saved registers on
# the kernel stack, then swaps stack pointers.
# Also switches page directories (CR3) if processes have different ones.

.text

# Offsets within process_t structure
.set PROC_REGS_ESP, 48     # offset of regs.esp (pid=4 + state=4 + regs offset 40)
.set PROC_CR3, 52          # offset of cr3 (page directory physical address)
.set PROC_KERNEL_STACK, 56 # offset of kernel_stack

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

    # Check if we need to switch page directories
    movl PROC_CR3(%ebx), %ecx   # old->cr3
    movl PROC_CR3(%eax), %edx   # new->cr3

    # Skip CR3 switch if same directory (or both 0)
    cmpl %ecx, %edx
    je .no_cr3_switch

    # Only switch if new->cr3 != 0 (0 means use kernel directory)
    testl %edx, %edx
    jz .no_cr3_switch

    # Switch to new page directory
    movl %edx, %cr3

.no_cr3_switch:
    # Switch to new process's stack
    movl PROC_REGS_ESP(%eax), %esp

    # Restore callee-saved registers from new stack
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    # Return to new process (ret addr is on new stack)
    ret
