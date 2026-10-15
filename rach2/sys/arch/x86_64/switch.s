# The x86-64's stack switch and the instructions that load the descriptor
# tables and enter user mode, for the C of context.c and percpu.c.

.set GDT_KERNEL_CODE, 0x10
.set GDT_KERNEL_DATA, 0x18
.set GDT_USER_DATA,   0x2B
.set GDT_USER_CODE,   0x33

.text

# void x86_64_switch(uint64_t *save_sp, uint64_t load_sp)
#
# Push the registers the ABI makes callee-saved, store the stack pointer at
# save_sp, take load_sp, pop the same registers from it and return to what
# that stack holds: the thread's own call of this function, or the address
# context.c put there for a thread's first switch.
.globl x86_64_switch
x86_64_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

# void x86_64_load_gdt(const struct desc_ptr *ptr)
#
# Load the GDT and reload the segment registers from it: the kernel's data
# selector in DS, ES and SS, the null one in FS and GS, whose bases come
# from their MSRs afterwards, and CS through a far return to the caller.
.globl x86_64_load_gdt
x86_64_load_gdt:
    lgdt (%rdi)
    movw $GDT_KERNEL_DATA, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    popq %rax
    pushq $GDT_KERNEL_CODE
    pushq %rax
    lretq

# unsigned int x86_64_fs_read(void)
# unsigned int x86_64_gs_read(void)
#
# The selector in FS or GS, zero-extended.
.globl x86_64_fs_read
x86_64_fs_read:
    xorl %eax, %eax
    movw %fs, %ax
    ret

.globl x86_64_gs_read
x86_64_gs_read:
    xorl %eax, %eax
    movw %gs, %ax
    ret

# void x86_64_fs_load(unsigned int sel)
# void x86_64_gs_load(unsigned int sel)
#
# Load sel into FS or GS. The load also sets the register's base, to the
# descriptor's or, for a null selector, to a value that differs between
# processors, so the caller writes the base MSR afterwards. A load into GS
# sets IA32_GS_BASE, the kernel's base, so it runs between two `swapgs`
# with interrupts off: the user's slot takes the descriptor's base, and an
# NMI between the two finds a lower-half base in IA32_GS_BASE, which the
# paranoid entry swaps as it does for any entry with the user's base loaded.
.globl x86_64_fs_load
x86_64_fs_load:
    movw %di, %fs
    ret

.globl x86_64_gs_load
x86_64_gs_load:
    pushfq
    cli
    swapgs
    movw %di, %gs
    swapgs
    popfq
    ret

# void x86_64_load_idt(const void *ptr)
#
# Load the IDTR from the operand at ptr: a 16-bit limit and a 64-bit base.
.globl x86_64_load_idt
x86_64_load_idt:
    lidt (%rdi)
    ret

# void x86_64_enter_user(uintptr_t pc, uintptr_t sp)
#
# Enter user mode at pc with the stack pointer sp, interrupts on and every
# general register cleared, through the frame an interrupt from user mode
# would have left; the `swapgs` before the `iretq` gives GS the program's
# base from IA32_KERNEL_GS_BASE.
.globl x86_64_enter_user
x86_64_enter_user:
    cli
    pushq $GDT_USER_DATA
    pushq %rsi
    pushq $0x202
    pushq $GDT_USER_CODE
    pushq %rdi
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    swapgs
    iretq
