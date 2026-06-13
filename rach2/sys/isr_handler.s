.text

# Macro for ISR without error code (pushes dummy error code 0)
.macro ISR_WITHOUT_ERROR_CODE num
    .globl isr\num
    isr\num:
        cli
        pushl $0
        pushl $\num
        jmp isr_common_handler
.endm

# Macro for ISR with error code (CPU pushes error code)
.macro ISR_WITH_ERROR_CODE num
    .globl isr\num
    isr\num:
        cli
        pushl $\num
        jmp isr_common_handler
.endm

# Generate ISR stubs (exceptions 0-31)
ISR_WITHOUT_ERROR_CODE 0
ISR_WITHOUT_ERROR_CODE 1
ISR_WITHOUT_ERROR_CODE 2
ISR_WITHOUT_ERROR_CODE 3
ISR_WITHOUT_ERROR_CODE 4
ISR_WITHOUT_ERROR_CODE 5
ISR_WITHOUT_ERROR_CODE 6
ISR_WITHOUT_ERROR_CODE 7

ISR_WITH_ERROR_CODE 8

ISR_WITHOUT_ERROR_CODE 9

ISR_WITH_ERROR_CODE 10
ISR_WITH_ERROR_CODE 11
ISR_WITH_ERROR_CODE 12
ISR_WITH_ERROR_CODE 13
ISR_WITH_ERROR_CODE 14

ISR_WITHOUT_ERROR_CODE 15
ISR_WITHOUT_ERROR_CODE 16

ISR_WITHOUT_ERROR_CODE 17

ISR_WITHOUT_ERROR_CODE 18
ISR_WITHOUT_ERROR_CODE 19
ISR_WITHOUT_ERROR_CODE 20
ISR_WITHOUT_ERROR_CODE 21
ISR_WITHOUT_ERROR_CODE 22
ISR_WITHOUT_ERROR_CODE 23
ISR_WITHOUT_ERROR_CODE 24
ISR_WITHOUT_ERROR_CODE 25
ISR_WITHOUT_ERROR_CODE 26
ISR_WITHOUT_ERROR_CODE 27
ISR_WITHOUT_ERROR_CODE 28
ISR_WITHOUT_ERROR_CODE 29
ISR_WITHOUT_ERROR_CODE 30
ISR_WITHOUT_ERROR_CODE 31

# Syscall interrupt (int 0x80 = vector 128)
ISR_WITHOUT_ERROR_CODE 128

# Common ISR handler
.globl isr_common_handler
isr_common_handler:
    pushal

    # Save the interrupted segment registers individually so the per-process
    # %gs TLS pointer survives this interrupt/syscall. Pushed ds, es, fs, gs so
    # gs lands at the lowest frame address (struct isr_params: gs, fs, es, ds).
    movw %ds, %ax
    pushl %eax
    movw %es, %ax
    pushl %eax
    movw %fs, %ax
    pushl %eax
    movw %gs, %ax
    pushl %eax

    # Run the kernel with the kernel data selector in ds/es/fs/gs.
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Enter the router with IF=0. The syscall handler (_syscall_handler) sets
    # the per-process in_syscall flag and only THEN does sti, closing the window
    # in which a timer IRQ could preempt a half-built trapframe; it cli's again
    # before returning here, so the epilogue restores segments/%gs atomically and
    # iret restores the caller's IF. IRQ stubs run with IF=0 throughout.
    pushl %esp          # Push pointer to isr_params_t structure
    call _isr_router
    addl $4, %esp       # Clean up pushed pointer

# Fork child return point - child processes jump here after switch_context
# Stack must contain: isr_params_t (gs, fs, es, ds, pushal regs, vector,
# error, iret frame)
.globl isr_return
isr_return:
    # Restore each saved segment register from its own slot (popped gs, fs, es,
    # ds), so the interrupted context's %gs TLS pointer is put back exactly.
    popl %eax
    movw %ax, %gs
    popl %eax
    movw %ax, %fs
    popl %eax
    movw %ax, %es
    popl %eax
    movw %ax, %ds

    popal
    addl $8, %esp

    # iret restores EFLAGS (including IF) from the saved frame; no explicit sti,
    # so no timer IRQ can preempt between sti and iret.
    iret

# Macro for IRQ handlers
.macro IRQ num, vector
    .globl irq\num
    irq\num:
        cli
        pushl $0
        pushl $\vector
        jmp irq_common_handler
.endm

# Generate IRQ stubs (IRQ 0-15 mapped to vectors 32-47)
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

# Common IRQ handler
.globl irq_common_handler
irq_common_handler:
    pushal

    # Save the interrupted segment registers individually (see isr_common_handler)
    # so the per-process %gs TLS pointer survives the IRQ. gs at the lowest slot.
    movw %ds, %ax
    pushl %eax
    movw %es, %ax
    pushl %eax
    movw %fs, %ax
    pushl %eax
    movw %gs, %ax
    pushl %eax

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp          # Push pointer to isr_params_t structure
    call _irq_router
    addl $4, %esp       # Clean up pushed pointer

    # Restore each saved segment register from its own slot (gs, fs, es, ds).
    popl %eax
    movw %ax, %gs
    popl %eax
    movw %ax, %fs
    popl %eax
    movw %ax, %es
    popl %eax
    movw %ax, %ds

    popal
    addl $8, %esp

    # iret restores EFLAGS (including IF); no explicit sti before iret.
    iret
