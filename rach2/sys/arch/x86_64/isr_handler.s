# Interrupt and system call entry for x86-64.
#
# Every entry builds one frame, struct isr_params in cpu.h, reading from the
# stack pointer toward higher addresses: the fifteen general registers, the
# vector, the error code, and the processor's five words (RIP, CS, RFLAGS,
# RSP, SS). An interrupt or exception gets the five from the processor and
# the vector and error code from its stub; the stubs are written out in full,
# as on i686, since TinyCC's assembler has no .macro. A `syscall` gets all
# of them from syscall_entry, which writes the five the processor did not
# push. The frame is 176 bytes, and the processor aligns the stack to 16
# bytes before it pushes its five words, so the frame starts on a 16-byte
# boundary and every call below meets the ABI's alignment.
#
# GS. In the kernel, GS's base is this processor's struct cpu (percpu.h); in
# user mode it is the program's. An entry from user mode, the saved CS at
# RPL 3, runs `swapgs` before anything reads %gs, and the return to user
# mode runs it again as its last step before the `iretq` or `sysretq`.
#
# Four exceptions come on interrupt stacks of their own and may arrive
# anywhere, between the `swapgs` of an exit and its `iretq` among other
# places, where the saved CS says kernel mode while GS has the user's base.
# The NMI, the double fault and the machine check therefore take the
# paranoid entry, which asks the GS base MSR itself whose base is loaded,
# swaps only when it is the user's, and swaps back only if it swapped. The
# debug exception makes the same GS check in debug_entry, which moves a
# user frame to the thread's stack or defers a kernel exception.
#
# Vectors with an error code from the processor (8, 10 to 14, 17, 21, 29 and
# 30) push only their vector; the others push a zero first, so the frame has
# one shape. There is no gate for int $0x80 (idt.c).

.set F_R11,    32
.set F_RCX,    96
.set F_RIP,    136
.set F_CS,     144
.set F_RFLAGS, 152
.set F_SS,     168

.set CPU_KERNEL_RSP, 8
.set CPU_USER_RSP,   16

.set GDT_USER_DATA,  0x2B
.set GDT_USER_CODE,  0x33
.set SYSCALL_VECTOR, 128

# The flags only `iretq` restores: TF, and RF, which `sysret` clears.
.set RFLAGS_TF_RF, 0x10100

# The bottom of the last user page. A return address at or above it is not
# one `sysretq` may take: an address past the lower half makes it fault in
# ring 0, on the user's stack, and an instruction at the end of the last
# page returns to the first address past it.
.set USER_ADDR_MAX, 0x00007FFFFFFFF000

.set MSR_GS_BASE, 0xC0000101

.text

# Exception stubs, vectors 0 through 31.

.globl isr0
isr0:
    pushq $0
    pushq $0
    jmp isr_common_handler

.globl isr1
isr1:
    pushq $0
    pushq $1
    jmp debug_entry

# The NMI, on its own stack.
.globl isr2
isr2:
    pushq $0
    pushq $2
    jmp paranoid_entry

.globl isr3
isr3:
    pushq $0
    pushq $3
    jmp isr_common_handler

.globl isr4
isr4:
    pushq $0
    pushq $4
    jmp isr_common_handler

.globl isr5
isr5:
    pushq $0
    pushq $5
    jmp isr_common_handler

.globl isr6
isr6:
    pushq $0
    pushq $6
    jmp isr_common_handler

.globl isr7
isr7:
    pushq $0
    pushq $7
    jmp isr_common_handler

# #DF double fault: the processor pushes the error code.
.globl isr8
isr8:
    pushq $8
    jmp paranoid_entry

.globl isr9
isr9:
    pushq $0
    pushq $9
    jmp isr_common_handler

# #TS invalid TSS: the processor pushes the error code.
.globl isr10
isr10:
    pushq $10
    jmp isr_common_handler

# #NP segment not present: the processor pushes the error code.
.globl isr11
isr11:
    pushq $11
    jmp isr_common_handler

# #SS stack fault: the processor pushes the error code.
.globl isr12
isr12:
    pushq $12
    jmp isr_common_handler

# #GP general protection: the processor pushes the error code.
.globl isr13
isr13:
    pushq $13
    jmp isr_common_handler

# #PF page fault: the processor pushes the error code.
.globl isr14
isr14:
    pushq $14
    jmp isr_common_handler

.globl isr15
isr15:
    pushq $0
    pushq $15
    jmp isr_common_handler

.globl isr16
isr16:
    pushq $0
    pushq $16
    jmp isr_common_handler

# #AC alignment check: the processor pushes the error code.
.globl isr17
isr17:
    pushq $17
    jmp isr_common_handler

# #MC machine check, on its own stack.
.globl isr18
isr18:
    pushq $0
    pushq $18
    jmp paranoid_entry

.globl isr19
isr19:
    pushq $0
    pushq $19
    jmp isr_common_handler

.globl isr20
isr20:
    pushq $0
    pushq $20
    jmp isr_common_handler

# #CP control protection: the processor pushes the error code.
.globl isr21
isr21:
    pushq $21
    jmp isr_common_handler

.globl isr22
isr22:
    pushq $0
    pushq $22
    jmp isr_common_handler

.globl isr23
isr23:
    pushq $0
    pushq $23
    jmp isr_common_handler

.globl isr24
isr24:
    pushq $0
    pushq $24
    jmp isr_common_handler

.globl isr25
isr25:
    pushq $0
    pushq $25
    jmp isr_common_handler

.globl isr26
isr26:
    pushq $0
    pushq $26
    jmp isr_common_handler

.globl isr27
isr27:
    pushq $0
    pushq $27
    jmp isr_common_handler

.globl isr28
isr28:
    pushq $0
    pushq $28
    jmp isr_common_handler

# #VC VMM communication: the processor pushes the error code.
.globl isr29
isr29:
    pushq $29
    jmp isr_common_handler

# #SX security: the processor pushes the error code.
.globl isr30
isr30:
    pushq $30
    jmp isr_common_handler

.globl isr31
isr31:
    pushq $0
    pushq $31
    jmp isr_common_handler


# The common entry of the exceptions and of the interrupts: save the
# registers, route the vector (x86_64_route, idt.c), deliver any signal
# pending for a return to user mode and give back the big lock (isr_leave,
# isr.c), and return.
.globl isr_common_handler
isr_common_handler:
    testb $3, 24(%rsp)
    jz .common_from_kernel
    swapgs
.common_from_kernel:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi
    call x86_64_route

# Where a forked child's first switch lands (context.c), with its frame on
# the stack: the same return as every other entry's.
.globl isr_return
isr_return:
    movq %rsp, %rdi
    call isr_leave

# Restore the frame and return with `iretq`, interrupts off until the flags
# it loads. A return to user mode at a non-canonical address, one whose bits
# 47 to 63 are not all equal, would fault `iretq` itself, in ring 0 with the
# user's GS; the address becomes 0 instead, below USER_ADDR_MIN, where the
# program takes its SIGSEGV in user mode. Only a frame a tracer wrote can
# have one. Shifted right by 47 with its sign, a canonical address is 0 or
# -1, and one more than that is at most 1.
.globl isr_exit
isr_exit:
    testb $3, F_CS(%rsp)
    jz .exit_restore
    movq F_RIP(%rsp), %rax
    sarq $47, %rax
    incq %rax
    cmpq $1, %rax
    jbe .exit_restore
    movq $0, F_RIP(%rsp)
.exit_restore:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    testb $3, 8(%rsp)
    jz .exit_iret
    swapgs
.exit_iret:
    iretq

# A debug exception may follow MOV SS and SYSCALL before syscall_entry
# changes stacks or GS. Its IST stack is always usable, and the GS MSR
# determines whether to swap. A user exception moves to the thread's stack
# before any C code can stop or switch it. A kernel exception only saves
# DR6 on this processor; isr_leave posts the signal on the thread's stack.
debug_entry:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    xorl %ebx, %ebx
    movl $MSR_GS_BASE, %ecx
    rdmsr
    testl %edx, %edx
    js .debug_kernel_gs
    swapgs
    movl $1, %ebx
.debug_kernel_gs:
    testb $3, F_CS(%rsp)
    jz .debug_from_kernel
    movq %rsp, %rsi
    movq %gs:CPU_KERNEL_RSP, %rdi
    subq $176, %rdi
    movq %rdi, %rdx
    movl $22, %ecx
    rep movsq
    movq %rdx, %rsp
    movq %rsp, %rdi
    call x86_64_route
    jmp isr_return
.debug_from_kernel:
    movq F_RIP(%rsp), %rdi
    call debugreg_defer_user
    testl %eax, %eax
    jnz .debug_deferred
    call debugreg_defer
.debug_deferred:
    andq $-257, F_RFLAGS(%rsp)
    testl %ebx, %ebx
    jz .paranoid_restore
    swapgs
    jmp .paranoid_restore

# The paranoid entry of the NMI, the double fault and the machine check,
# on their interrupt stacks. RBX, which the C call preserves, remembers
# whether this entry swapped GS. A kernel GS base is an upper-half address,
# whose high half has its top bit set. Nothing here delivers a signal or
# switches threads: the stack is the processor's, not the thread's.
.globl paranoid_entry
paranoid_entry:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    xorl %ebx, %ebx
    movl $MSR_GS_BASE, %ecx
    rdmsr
    testl %edx, %edx
    js .paranoid_kernel_gs
    swapgs
    movl $1, %ebx
.paranoid_kernel_gs:
    movq %rsp, %rdi
    call isr_paranoid_router
    testl %ebx, %ebx
    jz .paranoid_restore
    swapgs
.paranoid_restore:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

# The system call entry, IA32_LSTAR's. The processor has put the user's RIP
# in RCX and RFLAGS in R11, loaded the kernel's CS and SS, and cleared the
# flags IA32_FMASK names, IF and DF among them; it has not changed RSP. The
# entry swaps GS, moves to the thread's kernel stack (struct cpu's
# kernel_rsp), and builds the frame an interrupt from user mode would have,
# with the vector SYSCALL_VECTOR, so the router finds the system call
# handler as it would find a gate's.
#
# The return takes `sysretq` when the frame still describes the return the
# call made: RCX and R11 equal to RIP and RFLAGS, the user's selectors, no
# TF or RF, and an address below USER_ADDR_MAX. Anything a signal, an exec,
# a signal return or a tracer changed returns by `iretq`, which restores
# every register as the frame has it.
.globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KERNEL_RSP, %rsp
    pushq $GDT_USER_DATA
    pushq %gs:CPU_USER_RSP
    pushq %r11
    pushq $GDT_USER_CODE
    pushq %rcx
    pushq $0
    pushq $SYSCALL_VECTOR
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    call _isr_router
    movq %rsp, %rdi
    call isr_leave
    movq F_RCX(%rsp), %rcx
    cmpq F_RIP(%rsp), %rcx
    jne isr_exit
    movq F_R11(%rsp), %r11
    cmpq F_RFLAGS(%rsp), %r11
    jne isr_exit
    testq $RFLAGS_TF_RF, %r11
    jnz isr_exit
    cmpq $GDT_USER_CODE, F_CS(%rsp)
    jne isr_exit
    cmpq $GDT_USER_DATA, F_SS(%rsp)
    jne isr_exit
    movq $USER_ADDR_MAX, %rax
    cmpq %rax, %rcx
    jae isr_exit
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    # The user's stack pointer, past the vector, the error code, RIP, CS and
    # RFLAGS; from here to `sysretq` the kernel runs on the user's stack,
    # with interrupts off, and touches no memory.
    movq 40(%rsp), %rsp
    swapgs
    sysretq

# Interrupt stubs: the 8259's lines 0 through 15 on vectors 32 through 47.
# They enter as the exceptions do; x86_64_route tells the two apart by the
# vector.

.globl irq0
irq0:
    pushq $0
    pushq $32
    jmp isr_common_handler

.globl irq1
irq1:
    pushq $0
    pushq $33
    jmp isr_common_handler

.globl irq2
irq2:
    pushq $0
    pushq $34
    jmp isr_common_handler

.globl irq3
irq3:
    pushq $0
    pushq $35
    jmp isr_common_handler

.globl irq4
irq4:
    pushq $0
    pushq $36
    jmp isr_common_handler

.globl irq5
irq5:
    pushq $0
    pushq $37
    jmp isr_common_handler

.globl irq6
irq6:
    pushq $0
    pushq $38
    jmp isr_common_handler

.globl irq7
irq7:
    pushq $0
    pushq $39
    jmp isr_common_handler

.globl irq8
irq8:
    pushq $0
    pushq $40
    jmp isr_common_handler

.globl irq9
irq9:
    pushq $0
    pushq $41
    jmp isr_common_handler

.globl irq10
irq10:
    pushq $0
    pushq $42
    jmp isr_common_handler

.globl irq11
irq11:
    pushq $0
    pushq $43
    jmp isr_common_handler

.globl irq12
irq12:
    pushq $0
    pushq $44
    jmp isr_common_handler

.globl irq13
irq13:
    pushq $0
    pushq $45
    jmp isr_common_handler

.globl irq14
irq14:
    pushq $0
    pushq $46
    jmp isr_common_handler

.globl irq15
irq15:
    pushq $0
    pushq $47
    jmp isr_common_handler

# Message-signalled interrupt stubs, vectors 48 through 63, which the
# router ends at the local APIC after the handler chain.

.globl msi0
msi0:
    pushq $0
    pushq $48
    jmp isr_common_handler

.globl msi1
msi1:
    pushq $0
    pushq $49
    jmp isr_common_handler

.globl msi2
msi2:
    pushq $0
    pushq $50
    jmp isr_common_handler

.globl msi3
msi3:
    pushq $0
    pushq $51
    jmp isr_common_handler

.globl msi4
msi4:
    pushq $0
    pushq $52
    jmp isr_common_handler

.globl msi5
msi5:
    pushq $0
    pushq $53
    jmp isr_common_handler

.globl msi6
msi6:
    pushq $0
    pushq $54
    jmp isr_common_handler

.globl msi7
msi7:
    pushq $0
    pushq $55
    jmp isr_common_handler

.globl msi8
msi8:
    pushq $0
    pushq $56
    jmp isr_common_handler

.globl msi9
msi9:
    pushq $0
    pushq $57
    jmp isr_common_handler

.globl msi10
msi10:
    pushq $0
    pushq $58
    jmp isr_common_handler

.globl msi11
msi11:
    pushq $0
    pushq $59
    jmp isr_common_handler

.globl msi12
msi12:
    pushq $0
    pushq $60
    jmp isr_common_handler

.globl msi13
msi13:
    pushq $0
    pushq $61
    jmp isr_common_handler

.globl msi14
msi14:
    pushq $0
    pushq $62
    jmp isr_common_handler

.globl msi15
msi15:
    pushq $0
    pushq $63
    jmp isr_common_handler

# The local vectors, which the processor's own APIC raises and which run
# without the big lock (isr_local_router, isr.c): the interprocessor
# interrupt, IPI_VECTOR (lapic.h), and the spurious vector, 255.

.globl ipi_isr
ipi_isr:
    pushq $0
    pushq $0xF0
    jmp local_common_handler

.globl spurious_isr
spurious_isr:
    pushq $0
    pushq $255
    jmp local_common_handler

# The local vectors' common entry: the frame of the others, the router, and
# the return through isr_exit, with no signal delivered and no switch, since
# no lock is held.
local_common_handler:
    testb $3, 24(%rsp)
    jz .local_from_kernel
    swapgs
.local_from_kernel:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi
    call isr_local_router
    jmp isr_exit
