# The i686 interrupt entry: a stub for each vector, and the common entries
# and returns the stubs jump to.
#
# The stubs are written out in full because tcc's assembler, one of the two
# that assemble this kernel, has no .macro. Each stub disables interrupts,
# pushes an error code slot and its vector number, and jumps to the common
# handler, so the handler always finds the same frame, reading from the stack
# pointer toward higher addresses:
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
# Vectors 2 (NMI) and 18 (#MC) take isr_paranoid_handler, which enters the
# kernel without the big lock: they arrive at any instruction, even between
# the exchange that takes the lock and the count of its depth.

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
    jmp isr_paranoid_handler

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
    jmp isr_paranoid_handler

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

# The common entry of the exceptions and the system call: it completes the
# frame of struct isr_params (cpu.h) with the general and segment registers,
# loads the kernel's selectors, and calls _isr_router (isr.c) with
# interrupts off. A system call's handler (_syscall_handler, syscall.c) sets
# in_syscall before it enables interrupts, so a timer interrupt never finds a
# frame half built, and disables them again before it returns here.
.globl isr_common_handler
isr_common_handler:
    pusha

    # ds, es, fs, gs, so that gs has the lowest address, as isr_params has it.
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
    movw %ax, %gs
    # The processor's own segment, which this_cpu() reads (GDT_PERCPU_SELECTOR).
    movw $0x40, %ax
    movw %ax, %fs

    pushl %esp          # the frame, an isr_params_t *
    call _isr_router
    addl $4, %esp

# The return through the frame at the stack pointer, where a forked child's
# first switch also lands (arch_fork_frame, context.c). isr_leave (isr.c)
# delivers a pending signal on a return to user mode, which may rewrite the
# frame or end the thread, and gives back the big lock the entry took; then
# the segment registers the entry saved, the TLS selector in gs among them,
# are loaded again, and iret restores IF from the saved EFLAGS.
.globl isr_return
isr_return:
    pushl %esp
    call isr_leave
    addl $4, %esp

    popl %eax
    movw %ax, %gs
    popl %eax
    movw %ax, %fs
    popl %eax
    movw %ax, %es
    popl %eax
    movw %ax, %ds

    popa
    addl $8, %esp          # the vector and the error code
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


# Message-signalled interrupt stubs, vectors 48 through 239. A message has
# no line to acknowledge at the 8259; the router sends the local APIC its
# end-of-interrupt instead, after the handler chain. Each stub is 16 bytes
# from isr_msi_stubs, so that idt.c finds vector V's at isr_msi_stubs +
# (V - 48) * 16; a stub is at most 13 bytes, and idt.c checks that the run
# ends where 192 of them would. Vector 128's is never installed, since
# 0x80 is the system call's gate.

    .align 16
.globl isr_msi_stubs
isr_msi_stubs:
    cli
    pushl $0
    pushl $48
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $49
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $50
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $51
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $52
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $53
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $54
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $55
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $56
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $57
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $58
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $59
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $60
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $61
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $62
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $63
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $64
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $65
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $66
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $67
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $68
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $69
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $70
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $71
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $72
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $73
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $74
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $75
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $76
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $77
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $78
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $79
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $80
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $81
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $82
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $83
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $84
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $85
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $86
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $87
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $88
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $89
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $90
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $91
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $92
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $93
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $94
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $95
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $96
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $97
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $98
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $99
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $100
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $101
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $102
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $103
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $104
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $105
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $106
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $107
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $108
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $109
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $110
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $111
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $112
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $113
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $114
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $115
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $116
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $117
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $118
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $119
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $120
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $121
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $122
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $123
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $124
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $125
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $126
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $127
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $128
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $129
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $130
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $131
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $132
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $133
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $134
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $135
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $136
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $137
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $138
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $139
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $140
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $141
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $142
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $143
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $144
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $145
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $146
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $147
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $148
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $149
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $150
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $151
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $152
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $153
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $154
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $155
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $156
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $157
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $158
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $159
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $160
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $161
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $162
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $163
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $164
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $165
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $166
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $167
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $168
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $169
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $170
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $171
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $172
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $173
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $174
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $175
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $176
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $177
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $178
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $179
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $180
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $181
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $182
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $183
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $184
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $185
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $186
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $187
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $188
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $189
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $190
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $191
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $192
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $193
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $194
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $195
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $196
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $197
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $198
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $199
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $200
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $201
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $202
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $203
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $204
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $205
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $206
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $207
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $208
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $209
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $210
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $211
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $212
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $213
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $214
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $215
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $216
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $217
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $218
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $219
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $220
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $221
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $222
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $223
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $224
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $225
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $226
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $227
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $228
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $229
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $230
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $231
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $232
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $233
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $234
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $235
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $236
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $237
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $238
    jmp irq_common_handler
    .align 16
    cli
    pushl $0
    pushl $239
    jmp irq_common_handler
    .align 16
.globl isr_msi_stubs_end
isr_msi_stubs_end:


# The local vectors, which the processor's own APIC raises and which run
# without the big lock (isr_local_router, isr.c): the interprocessor interrupt,
# IPI_VECTOR (lapic.h), and the spurious vector, 255, raised when an interrupt
# the APIC was about to deliver went away, which needs no end-of-interrupt and
# has no device behind it.

.globl ipi_isr
ipi_isr:
    cli
    pushl $0
    pushl $0xF0
    jmp local_common_handler

.globl spurious_isr
spurious_isr:
    cli
    pushl $0
    pushl $255
    jmp local_common_handler

# The local vectors' common entry: the frame of the others, the router, and
# the return, with no signal delivered and no switch, since no lock is held.
local_common_handler:
    pusha
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
    movw %ax, %gs
    movw $0x40, %ax
    movw %ax, %fs

    pushl %esp
    call isr_local_router
    addl $4, %esp

local_return:
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
    iret

# The NMI's and the machine check's entry: the same frame, the paranoid
# router (isr.c), which takes no lock, and the same return.
isr_paranoid_handler:
    pusha
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
    movw %ax, %gs
    movw $0x40, %ax
    movw %ax, %fs

    pushl %esp
    call isr_paranoid_router
    addl $4, %esp
    jmp local_return

# The common entry of the device interrupts: the frame and selectors of
# isr_common_handler, _irq_router (isr.c), and the same return, all with
# interrupts off. The router may have switched threads, so the frame
# isr_leave and the pops find is the resumed thread's.
.globl irq_common_handler
irq_common_handler:
    pusha

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
    movw %ax, %gs
    # The processor's own segment, which this_cpu() reads (GDT_PERCPU_SELECTOR).
    movw $0x40, %ax
    movw %ax, %fs

    pushl %esp          # the frame, an isr_params_t *
    call _irq_router
    addl $4, %esp

    pushl %esp
    call isr_leave
    addl $4, %esp

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
    iret
