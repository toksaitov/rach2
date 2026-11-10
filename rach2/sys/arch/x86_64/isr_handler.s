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
# one shape. The gate of `int $0x80`, the i386 system call, pushes the
# error code SC_ENTRY_IA32 (cpu.h) and the vector SYSCALL_VECTOR, which
# x86_64_route (idt.c) hands to the system call's handler. The entries of
# `sysenter` and of `syscall` from 32-bit code, which the 32-bit vDSO makes,
# build that same frame, so that the call runs as the `int $0x80` it stands
# for.
#
# Two instructions of the kernel fault on what a program set: the `iretq` of
# a return to user mode, on a CS or SS whose LDT entry the program changed
# or a return address past a segment's limit, and the load of a selector
# into a segment register (switch.s). The common entry turns the first into
# the program's own fault, and the second resumes where the selector's
# register gets the null one (`x86_64_fault_fixup`, context.c), as Linux's
# fault fixups do.

# Byte offsets in the frame, struct isr_params (cpu.h).
.set F_R11,    32
.set F_RCX,    96
.set F_RIP,    136
.set F_CS,     144
.set F_RFLAGS, 152
.set F_RSP,    160
.set F_SS,     168

# Byte offsets in struct cpu (percpu.h).
.set CPU_KERNEL_RSP, 8
.set CPU_USER_RSP,   16

# The user's selectors (percpu.h) and the system call's vector (sysnr.h).
.set GDT_USER_CODE32, 0x23
.set GDT_USER_DATA,  0x2B
.set GDT_USER_CODE,  0x33
.set SYSCALL_VECTOR, 128
.set SC_ENTRY_IA32,  0x80

# The flags only `iretq` restores: TF, and RF, which `sysret` clears.
.set RFLAGS_TF_RF, 0x10100

# IF; the flags `sysenter` leaves as the caller had them that the kernel
# must not run with, NT, AC and TF; and bit 1, which is always set.
.set RFLAGS_IF,       0x200
.set RFLAGS_NT_AC_TF, 0x44100
.set RFLAGS_FIXED,    0x2

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


# The common entry of the exceptions and the interrupts, all but the four
# exceptions on interrupt stacks and the local vectors: swap GS for an entry
# from user mode, save the registers, route the vector (x86_64_route,
# idt.c), deliver any signal pending for a return to user mode and give back
# the big lock the router took (isr_leave, isr.c), and return.
.globl isr_common_handler
isr_common_handler:
    testb $3, 24(%rsp)
    jz .common_from_kernel
    swapgs
.common_save:
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

# The return of the common entry and of a debug exception from user mode,
# and where a forked child's first switch lands (context.c), with its frame
# on the stack.
.globl isr_return
isr_return:
    movq %rsp, %rdi
    call isr_leave

# Restore the frame and return with `iretq`, interrupts off until the flags
# it loads. A return to user mode at an address `iretq` would fault on, in
# ring 0 with the user's GS, gets the address 0 instead, below
# USER_ADDR_MIN, where the program takes its SIGSEGV in user mode: in 64-bit
# code a non-canonical address, one whose bits 47 to 63 are not all equal,
# and in 32-bit code one past the code segment's 4 GiB limit. Only a frame a
# tracer or a signal return wrote can have one. Shifted right by 47 with its
# sign, a canonical address is 0 or -1, and one more than that is at most 1.
.globl isr_exit
isr_exit:
    testb $3, F_CS(%rsp)
    jz .exit_restore
    movq F_RIP(%rsp), %rax
    cmpq $GDT_USER_CODE32, F_CS(%rsp)
    jne .exit_canonical
    shrq $32, %rax
    jz .exit_restore
    movq $0, F_RIP(%rsp)
    jmp .exit_restore
.exit_canonical:
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
    jz isr_iret
    swapgs
.globl isr_iret
isr_iret:
    iretq

# An exception from kernel mode finds the kernel's GS base loaded, but for
# the two faulting instructions that run with the user's: the `iretq` of a
# return to user mode (isr_iret) and the load into GS (switch.s). Their
# faults swap here, and the one of the `iretq` is taken as the program's
# (.common_bad_iret). Interrupts are off at both, so only the instruction's
# own fault comes from there.
.common_from_kernel:
    pushq %rax
    leaq isr_iret(%rip), %rax
    cmpq %rax, 24(%rsp)
    je .common_bad_iret
    leaq x86_64_gs_load_insn(%rip), %rax
    cmpq %rax, 24(%rsp)
    popq %rax
    jne .common_save
    swapgs
    jmp .common_save

# The fault of the `iretq` to user mode, which Linux's `fixup_bad_iret`
# makes a fault of the program at the place it was returning to: the five
# words the `iretq` was loading take the place of the kernel's in the frame,
# and the frame moves up to where the frame the `iretq` restored began, 136
# bytes below those five words, so it is a frame of an entry from user mode
# like any other. The frame is below that place, so it is copied from its
# last word down. The program then takes the signal of the fault, or, for a
# selector of an LDT this processor had not loaded, runs on with it loaded
# (`ldt_fault`, ldt.c).
.common_bad_iret:
    popq %rax
    swapgs
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
    movq F_RSP(%rsp), %rsi
    movq (%rsi), %rax
    movq %rax, F_RIP(%rsp)
    movq 8(%rsi), %rax
    movq %rax, F_CS(%rsp)
    movq 16(%rsi), %rax
    movq %rax, F_RFLAGS(%rsp)
    movq 24(%rsi), %rax
    movq %rax, F_RSP(%rsp)
    movq 32(%rsi), %rax
    movq %rax, F_SS(%rsp)
    leaq 32(%rsi), %rdi
    leaq 168(%rsp), %rsi
    movl $22, %ecx
    std
    rep movsq
    cld
    leaq 8(%rdi), %rsp
    movq %rsp, %rdi
    call x86_64_route
    jmp isr_return

# The debug exception's entry. MOV SS can delay one past a `syscall` to the
# first instruction of syscall_entry, before it has changed the stack or GS,
# so the exception comes on an interrupt stack of its own, and the GS base
# MSR, not the saved CS, decides the swap, as in the paranoid entry. An
# exception from user mode has its frame copied to the thread's kernel
# stack, before any C code can stop or switch the thread, and is routed and
# returned from there. One from kernel mode is only recorded for this
# processor (debugreg_defer_user, debugreg_defer, x86/debugreg.c), with TF
# cleared in the saved flags; its signal is posted later on the thread's own
# stack, at the return to user mode (isr_leave) or before a signal test
# (arch_debug_deliver). The exception of a single step through the
# `sysenter` entry, whose instruction leaves TF set, is let through with TF
# kept (debugreg_defer_sysenter, x86/debugreg.c), so that the entry's frame
# has the program's TF.
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
    call debugreg_defer_sysenter
    testl %eax, %eax
    jnz .debug_stepping
    movq F_RIP(%rsp), %rdi
    call debugreg_defer_user
    testl %eax, %eax
    jnz .debug_deferred
    call debugreg_defer
.debug_deferred:
    andq $-257, F_RFLAGS(%rsp)
.debug_stepping:
    testl %ebx, %ebx
    jz .paranoid_restore
    swapgs
    jmp .paranoid_restore

# The paranoid entry of the NMI, the double fault and the machine check,
# on their interrupt stacks. RBX, which the C call preserves, is 1 when this
# entry swapped GS. A kernel GS base is an upper-half address, whose high
# half has its top bit set. Nothing here delivers a signal or switches
# threads: the stack is the processor's, not the thread's.
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
# every register as the frame has it. From the load of the user's stack
# pointer to `sysretq` the kernel runs on the user's stack, with interrupts
# off, and touches no memory.
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
    # The user's RSP, past the vector, the error code, RIP, CS and RFLAGS.
    movq 40(%rsp), %rsp
    swapgs
    sysretq

# The entry of `sysenter`, IA32_SYSENTER_EIP's, which 32-bit code on
# Intel's processors makes from the vDSO's __kernel_vsyscall after
# `movl %esp, %ebp` (vdso32code.s), as Linux's `entry_SYSENTER_compat`. The
# processor has loaded the kernel's CS and SS, and RSP from
# IA32_SYSENTER_ESP, and cleared IF, and kept no address to return to: the
# frame has the caller's flags as `pushfq` reads them, with IF set again,
# and its RIP and RSP come from ia32_fast_syscall (vdso32.c). The
# instruction clears no other flag, so NT, AC and TF are cleared here, after
# the frame has them. With TF set the processor steps through each
# instruction up to the `popfq`, and the debug entry lets it, up to
# sysenter_stepped.
.globl sysenter_entry
sysenter_entry:
    swapgs
    movq %gs:CPU_KERNEL_RSP, %rsp
    pushq $GDT_USER_DATA
    pushq $0
    pushfq
    pushq $GDT_USER_CODE32
    pushq $0
    pushq $SC_ENTRY_IA32
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
    cld
    testl $RFLAGS_NT_AC_TF, F_RFLAGS(%rsp)
    jnz .sysenter_flags
.sysenter_flags_clear:
    orq $RFLAGS_IF, F_RFLAGS(%rsp)
    movl $1, %esi
    jmp .fast32_call
.sysenter_flags:
    pushq $RFLAGS_FIXED
    popfq
.globl sysenter_stepped
sysenter_stepped:
    jmp .sysenter_flags_clear

# The entry of a `syscall` from compatibility mode, IA32_CSTAR's, which
# 32-bit code on AMD's processors makes from __kernel_vsyscall after
# `movl %ecx, %ebp`, as Linux's `entry_SYSCALL_compat`. The processor has put
# the caller's EIP in RCX and its flags in R11, and cleared the flags
# IA32_FMASK names, as for 64-bit code; it has not changed RSP. The frame
# takes the call's ECX from EBP, and ia32_fast_syscall reads EBP's own value
# back from the user's stack, where __kernel_vsyscall pushed it.
#
# Both entries return by `sysretl`, the form without REX.W, to the 32-bit
# code segment STAR names, when ia32_fast_syscall says the frame still
# describes the return to the vDSO's landing pad, and otherwise by `iretq`.
# `sysretl` takes EIP from ECX, which the landing pad then pops; from the
# load of the user's stack pointer to `sysretl` the kernel runs on the
# user's stack, with interrupts off, and touches no memory.
.globl syscall32_entry
syscall32_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KERNEL_RSP, %rsp
    pushq $GDT_USER_DATA
    pushq %gs:CPU_USER_RSP
    pushq %r11
    pushq $GDT_USER_CODE32
    pushq %rcx
    pushq $SC_ENTRY_IA32
    pushq $SYSCALL_VECTOR
    pushq %rax
    pushq %rbx
    pushq %rbp
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
    xorl %esi, %esi
.fast32_call:
    movq %rsp, %rdi
    call ia32_fast_syscall
    testl %eax, %eax
    jz isr_exit
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
    # EIP, RFLAGS and the user's RSP, past the vector and the error code.
    movl 16(%rsp), %ecx
    movq 32(%rsp), %r11
    movq 40(%rsp), %rsp
    swapgs
    # sysretl, which TinyCC's assembler does not name.
    .byte 0x0f, 0x07

# IA32_CSTAR's entry where the kernel runs no i386 call (`init_tss`), as
# Linux's `entry_SYSCALL32_ignore`: -ENOSYS in EAX and back by `sysretl`
# at once, with RCX and R11 as the processor left them, on the user's
# stack and with interrupts off, touching no memory.
.globl syscall32_ignore
syscall32_ignore:
    movl $-38, %eax
    .byte 0x0f, 0x07

# The i386 system call, the gate of vector 0x80 at DPL 3, from 32-bit code
# or 64-bit: the frame of an interrupt, with SC_ENTRY_IA32 for its error
# code, so that the dispatcher takes the number and the arguments from
# i386's registers and the compat table.
.globl int80_isr
int80_isr:
    pushq $SC_ENTRY_IA32
    pushq $SYSCALL_VECTOR
    jmp isr_common_handler

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

# Message-signalled interrupt stubs, vectors 48 through 239, which the
# router ends at the local APIC after the handler chain. Each is 16 bytes
# from isr_msi_stubs, so that idt.c finds vector V's at isr_msi_stubs +
# (V - 48) * 16; a stub is at most 12 bytes, and idt.c checks that the run
# ends where 192 of them would. Vector 128's is never installed, since
# 0x80 is the i386 system call's gate.

    .align 16
.globl isr_msi_stubs
isr_msi_stubs:
    pushq $0
    pushq $48
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $49
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $50
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $51
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $52
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $53
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $54
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $55
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $56
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $57
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $58
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $59
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $60
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $61
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $62
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $63
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $64
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $65
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $66
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $67
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $68
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $69
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $70
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $71
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $72
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $73
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $74
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $75
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $76
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $77
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $78
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $79
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $80
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $81
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $82
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $83
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $84
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $85
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $86
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $87
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $88
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $89
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $90
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $91
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $92
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $93
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $94
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $95
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $96
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $97
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $98
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $99
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $100
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $101
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $102
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $103
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $104
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $105
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $106
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $107
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $108
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $109
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $110
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $111
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $112
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $113
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $114
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $115
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $116
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $117
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $118
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $119
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $120
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $121
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $122
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $123
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $124
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $125
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $126
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $127
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $128
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $129
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $130
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $131
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $132
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $133
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $134
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $135
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $136
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $137
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $138
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $139
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $140
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $141
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $142
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $143
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $144
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $145
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $146
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $147
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $148
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $149
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $150
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $151
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $152
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $153
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $154
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $155
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $156
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $157
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $158
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $159
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $160
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $161
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $162
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $163
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $164
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $165
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $166
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $167
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $168
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $169
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $170
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $171
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $172
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $173
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $174
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $175
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $176
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $177
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $178
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $179
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $180
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $181
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $182
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $183
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $184
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $185
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $186
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $187
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $188
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $189
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $190
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $191
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $192
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $193
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $194
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $195
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $196
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $197
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $198
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $199
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $200
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $201
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $202
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $203
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $204
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $205
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $206
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $207
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $208
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $209
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $210
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $211
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $212
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $213
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $214
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $215
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $216
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $217
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $218
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $219
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $220
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $221
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $222
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $223
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $224
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $225
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $226
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $227
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $228
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $229
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $230
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $231
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $232
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $233
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $234
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $235
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $236
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $237
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $238
    jmp isr_common_handler
    .align 16
    pushq $0
    pushq $239
    jmp isr_common_handler
    .align 16
.globl isr_msi_stubs_end
isr_msi_stubs_end:

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
