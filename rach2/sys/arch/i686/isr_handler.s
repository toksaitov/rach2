# Interrupt entry stubs, one per vector, written out in full.
#
# Every stub was once produced by a GAS .macro. The stubs are explicit now so
# that one source assembles under both toolchains that build this kernel:
# GNU as, and tcc's assembler, whose directive set has no .macro. Each stub
# disables interrupts, pushes an error code slot and its vector number, and
# jumps to the common handler, so the handler always finds the same frame,
# reading from the stack pointer toward higher addresses:
# [vector][error code][iret frame].
#
# Two shapes exist because the CPU pushes an error code for some exceptions
# and not for others. Vectors 10 (#TS), 11 (#NP), 12 (#SS), 13 (#GP), 14
# (#PF), 17 (#AC) and 21 (#CP) arrive with the CPU's error code already on
# the stack, so their stubs push only the vector. Every other stub pushes a
# zero first, so both shapes leave the frame identical. A stub given the wrong
# shape leaves the vector slot intact and shifts everything above it: the
# error code reads as the stub's zero, and the saved iret fields sit one slot
# off, so the interrupt return consumes a malformed frame.
#
# Vector 8 (#DF), the double fault, has no stub: its gate is a task gate,
# and the processor starts isr_double_fault (isr.c) on a stack of its own.

.text

# Exception stubs, vectors 0 through 31.

.globl isr0
isr0:
    cli
    pushl $0
    pushl $0
    jmp isr_common_handler

.globl isr1
isr1:
    cli
    pushl $0
    pushl $1
    jmp isr_common_handler

.globl isr2
isr2:
    cli
    pushl $0
    pushl $2
    jmp isr_common_handler

.globl isr3
isr3:
    cli
    pushl $0
    pushl $3
    jmp isr_common_handler

.globl isr4
isr4:
    cli
    pushl $0
    pushl $4
    jmp isr_common_handler

.globl isr5
isr5:
    cli
    pushl $0
    pushl $5
    jmp isr_common_handler

.globl isr6
isr6:
    cli
    pushl $0
    pushl $6
    jmp isr_common_handler

.globl isr7
isr7:
    cli
    pushl $0
    pushl $7
    jmp isr_common_handler

.globl isr9
isr9:
    cli
    pushl $0
    pushl $9
    jmp isr_common_handler

# #TS invalid TSS: the CPU pushes the error code.
.globl isr10
isr10:
    cli
    pushl $10
    jmp isr_common_handler

# #NP segment not present: the CPU pushes the error code.
.globl isr11
isr11:
    cli
    pushl $11
    jmp isr_common_handler

# #SS stack fault: the CPU pushes the error code.
.globl isr12
isr12:
    cli
    pushl $12
    jmp isr_common_handler

# #GP general protection: the CPU pushes the error code.
.globl isr13
isr13:
    cli
    pushl $13
    jmp isr_common_handler

# #PF page fault: the CPU pushes the error code.
.globl isr14
isr14:
    cli
    pushl $14
    jmp isr_common_handler

.globl isr15
isr15:
    cli
    pushl $0
    pushl $15
    jmp isr_common_handler

.globl isr16
isr16:
    cli
    pushl $0
    pushl $16
    jmp isr_common_handler

# #AC alignment check: the CPU pushes the error code.
.globl isr17
isr17:
    cli
    pushl $17
    jmp isr_common_handler

.globl isr18
isr18:
    cli
    pushl $0
    pushl $18
    jmp isr_common_handler

.globl isr19
isr19:
    cli
    pushl $0
    pushl $19
    jmp isr_common_handler

.globl isr20
isr20:
    cli
    pushl $0
    pushl $20
    jmp isr_common_handler

# #CP control protection: the CPU pushes the error code.
.globl isr21
isr21:
    cli
    pushl $21
    jmp isr_common_handler

.globl isr22
isr22:
    cli
    pushl $0
    pushl $22
    jmp isr_common_handler

.globl isr23
isr23:
    cli
    pushl $0
    pushl $23
    jmp isr_common_handler

.globl isr24
isr24:
    cli
    pushl $0
    pushl $24
    jmp isr_common_handler

.globl isr25
isr25:
    cli
    pushl $0
    pushl $25
    jmp isr_common_handler

.globl isr26
isr26:
    cli
    pushl $0
    pushl $26
    jmp isr_common_handler

.globl isr27
isr27:
    cli
    pushl $0
    pushl $27
    jmp isr_common_handler

.globl isr28
isr28:
    cli
    pushl $0
    pushl $28
    jmp isr_common_handler

.globl isr29
isr29:
    cli
    pushl $0
    pushl $29
    jmp isr_common_handler

.globl isr30
isr30:
    cli
    pushl $0
    pushl $30
    jmp isr_common_handler

.globl isr31
isr31:
    cli
    pushl $0
    pushl $31
    jmp isr_common_handler

# The syscall gate, int 0x80 (vector 128).
.globl isr128
isr128:
    cli
    pushl $0
    pushl $128
    jmp isr_common_handler

# Common ISR handler
.globl isr_common_handler
isr_common_handler:
    pusha

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
# Stack must contain: isr_params_t (gs, fs, es, ds, pusha regs, vector,
# error, iret frame)
.globl isr_return
isr_return:
    # Before returning to user mode, deliver any pending signal: this may rewrite
    # the trapframe (run a handler) or not return at all (default-terminate). It
    # is a no-op when returning to kernel mode (it checks the saved CS RPL). Runs
    # with IF=0, so it cannot be preempted. esp still points at the trapframe.
    pushl %esp
    call deliver_signals
    addl $4, %esp

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

    popa
    addl $8, %esp

    # iret restores EFLAGS (including IF) from the saved frame; no explicit sti,
    # so no timer IRQ can preempt between sti and iret.
    iret

# IRQ stubs: IRQ 0 through 15, remapped to vectors 32 through 47.

.globl irq0
irq0:
    cli
    pushl $0
    pushl $32
    jmp irq_common_handler

.globl irq1
irq1:
    cli
    pushl $0
    pushl $33
    jmp irq_common_handler

.globl irq2
irq2:
    cli
    pushl $0
    pushl $34
    jmp irq_common_handler

.globl irq3
irq3:
    cli
    pushl $0
    pushl $35
    jmp irq_common_handler

.globl irq4
irq4:
    cli
    pushl $0
    pushl $36
    jmp irq_common_handler

.globl irq5
irq5:
    cli
    pushl $0
    pushl $37
    jmp irq_common_handler

.globl irq6
irq6:
    cli
    pushl $0
    pushl $38
    jmp irq_common_handler

.globl irq7
irq7:
    cli
    pushl $0
    pushl $39
    jmp irq_common_handler

.globl irq8
irq8:
    cli
    pushl $0
    pushl $40
    jmp irq_common_handler

.globl irq9
irq9:
    cli
    pushl $0
    pushl $41
    jmp irq_common_handler

.globl irq10
irq10:
    cli
    pushl $0
    pushl $42
    jmp irq_common_handler

.globl irq11
irq11:
    cli
    pushl $0
    pushl $43
    jmp irq_common_handler

.globl irq12
irq12:
    cli
    pushl $0
    pushl $44
    jmp irq_common_handler

.globl irq13
irq13:
    cli
    pushl $0
    pushl $45
    jmp irq_common_handler

.globl irq14
irq14:
    cli
    pushl $0
    pushl $46
    jmp irq_common_handler

.globl irq15
irq15:
    cli
    pushl $0
    pushl $47
    jmp irq_common_handler


# Message-signalled interrupt stubs, vectors 48 through 63. A message has no
# line to acknowledge at the 8259; the router sends the local APIC its
# end-of-interrupt instead, after the handler chain.

.globl msi0
msi0:
    cli
    pushl $0
    pushl $48
    jmp irq_common_handler

.globl msi1
msi1:
    cli
    pushl $0
    pushl $49
    jmp irq_common_handler

.globl msi2
msi2:
    cli
    pushl $0
    pushl $50
    jmp irq_common_handler

.globl msi3
msi3:
    cli
    pushl $0
    pushl $51
    jmp irq_common_handler

.globl msi4
msi4:
    cli
    pushl $0
    pushl $52
    jmp irq_common_handler

.globl msi5
msi5:
    cli
    pushl $0
    pushl $53
    jmp irq_common_handler

.globl msi6
msi6:
    cli
    pushl $0
    pushl $54
    jmp irq_common_handler

.globl msi7
msi7:
    cli
    pushl $0
    pushl $55
    jmp irq_common_handler

.globl msi8
msi8:
    cli
    pushl $0
    pushl $56
    jmp irq_common_handler

.globl msi9
msi9:
    cli
    pushl $0
    pushl $57
    jmp irq_common_handler

.globl msi10
msi10:
    cli
    pushl $0
    pushl $58
    jmp irq_common_handler

.globl msi11
msi11:
    cli
    pushl $0
    pushl $59
    jmp irq_common_handler

.globl msi12
msi12:
    cli
    pushl $0
    pushl $60
    jmp irq_common_handler

.globl msi13
msi13:
    cli
    pushl $0
    pushl $61
    jmp irq_common_handler

.globl msi14
msi14:
    cli
    pushl $0
    pushl $62
    jmp irq_common_handler

.globl msi15
msi15:
    cli
    pushl $0
    pushl $63
    jmp irq_common_handler


# The APIC's spurious vector, 255. It is raised when an interrupt the APIC was
# about to deliver went away, needs no end-of-interrupt, and has no device
# behind it; the router counts it and returns.

.globl spurious_isr
spurious_isr:
    cli
    pushl $0
    pushl $255
    jmp irq_common_handler

# Common IRQ handler
.globl irq_common_handler
irq_common_handler:
    pusha

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

    # Deliver any pending signal before returning to user (no-op in kernel mode;
    # the IRQ may have switched processes, so esp/current_proc are the resumed
    # process's). IF is still 0.
    pushl %esp
    call deliver_signals
    addl $4, %esp

    # Restore each saved segment register from its own slot (gs, fs, es, ds).
    popl %eax
    movw %ax, %gs
    popl %eax
    movw %ax, %fs
    popl %eax
    movw %ax, %es
    popl %eax
    movw %ax, %ds

    popa
    addl $8, %esp

    # iret restores EFLAGS (including IF); no explicit sti before iret.
    iret
