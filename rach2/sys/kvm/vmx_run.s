# The entry to a VMX guest and the VMX instructions, for vmx.c. TinyCC's
# assembler names none of the VMX instructions, so each is written in its
# bytes. Each instruction but the entry returns 0 on success, 1 for
# VMfailInvalid (CF set) and 2 for VMfailValid (ZF set), as vmx.c's
# VMX_OK, VMX_FAIL_INVALID and VMX_FAIL_VALID.

.text

# The offsets of struct kvm_regs, kvm_abi.h's.
.set REG_RAX, 0
.set REG_RBX, 8
.set REG_RCX, 16
.set REG_RDX, 24
.set REG_RSI, 32
.set REG_RDI, 40
.set REG_RBP, 56
.set REG_R8, 64
.set REG_R9, 72
.set REG_R10, 80
.set REG_R11, 88
.set REG_R12, 96
.set REG_R13, 104
.set REG_R14, 112
.set REG_R15, 120

# The VMCS field the entry writes: HOST_RSP.
.set HOST_RSP, 0x6C14

# The frame kvm_vmx_vmrun keeps on the stack while the guest runs, from
# the stack pointer the exit restores.
.set F_LAUNCHED, 0
.set F_CR2, 8
.set F_REGS, 16
.set F_SIZE, 24

# The result of the VMX instruction just run, from its flags. A jump
# leaves the flags as they are.
.vmx_result:
    jb .vmx_fail_invalid
    je .vmx_fail_valid
    xorl %eax, %eax
    ret
.vmx_fail_invalid:
    movl $1, %eax
    ret
.vmx_fail_valid:
    movl $2, %eax
    ret

# int kvm_vmx_vmxon(uint64_t *pa)
#
# VMXON with the region at the physical address *pa.
.globl kvm_vmx_vmxon
kvm_vmx_vmxon:
    movq %rdi, %rax
    .byte 0xf3, 0x0f, 0xc7, 0x30        # vmxon (%rax)
    jmp .vmx_result

# int kvm_vmx_vmxoff(void)
.globl kvm_vmx_vmxoff
kvm_vmx_vmxoff:
    .byte 0x0f, 0x01, 0xc4              # vmxoff
    jmp .vmx_result

# int kvm_vmx_vmclear(uint64_t *pa)
.globl kvm_vmx_vmclear
kvm_vmx_vmclear:
    movq %rdi, %rax
    .byte 0x66, 0x0f, 0xc7, 0x30        # vmclear (%rax)
    jmp .vmx_result

# int kvm_vmx_vmptrld(uint64_t *pa)
.globl kvm_vmx_vmptrld
kvm_vmx_vmptrld:
    movq %rdi, %rax
    .byte 0x0f, 0xc7, 0x30              # vmptrld (%rax)
    jmp .vmx_result

# int kvm_vmx_vmread(uint64_t field, uint64_t *value)
#
# *value is written whether or not the instruction succeeds; vmx.c reads
# it only on success.
.globl kvm_vmx_vmread
kvm_vmx_vmread:
    movq %rdi, %rdx
    xorl %eax, %eax
    .byte 0x0f, 0x78, 0xd0              # vmread %rdx, %rax
    movq %rax, (%rsi)
    jmp .vmx_result

# int kvm_vmx_vmwrite(uint64_t field, uint64_t value)
.globl kvm_vmx_vmwrite
kvm_vmx_vmwrite:
    movq %rdi, %rdx
    movq %rsi, %rax
    .byte 0x0f, 0x79, 0xd0              # vmwrite %rax, %rdx
    jmp .vmx_result

# int kvm_vmx_invept(uint64_t type, const uint64_t *desc)
.globl kvm_vmx_invept
kvm_vmx_invept:
    movq %rdi, %rdx
    movq %rsi, %rax
    .byte 0x66, 0x0f, 0x38, 0x80, 0x10  # invept (%rax), %rdx
    jmp .vmx_result

# int kvm_vmx_invvpid(uint64_t type, const uint64_t *desc)
.globl kvm_vmx_invvpid
kvm_vmx_invvpid:
    movq %rdi, %rdx
    movq %rsi, %rax
    .byte 0x66, 0x0f, 0x38, 0x81, 0x10  # invvpid (%rax), %rdx
    jmp .vmx_result

# int kvm_vmx_vmrun(struct kvm_regs *regs, uint64_t *cr2, int launched)
#
# Enter the guest of the current VMCS with its general registers from
# regs and its CR2 from *cr2: VMRESUME when launched, else VMLAUNCH. The
# guest's RSP, RIP and RFLAGS are the VMCS's. At the exit, which resumes
# at kvm_vmx_exit on the stack HOST_RSP names, the guest's registers go
# back into regs and its CR2 into *cr2, and the result is 0. When the
# entry instruction itself fails, nothing of regs changes and the result
# is 1 or 2, as for the other instructions. Called with interrupts off;
# the exit leaves them off, with RFLAGS 0x2.
.globl kvm_vmx_vmrun
kvm_vmx_vmrun:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rdi
    pushq %rsi
    pushq %rdx

    movq $HOST_RSP, %rax
    .byte 0x0f, 0x79, 0xc4              # vmwrite %rsp, %rax
    jb .vmrun_fail_invalid
    je .vmrun_fail_valid

    movq (%rsi), %rax
    .byte 0x0f, 0x22, 0xd0              # mov %rax, %cr2

    # The choice of instruction is made before the guest's registers are
    # loaded, since none of the moves below changes the flags.
    cmpl $0, F_LAUNCHED(%rsp)
    movq REG_RAX(%rdi), %rax
    movq REG_RBX(%rdi), %rbx
    movq REG_RCX(%rdi), %rcx
    movq REG_RDX(%rdi), %rdx
    movq REG_RSI(%rdi), %rsi
    movq REG_RBP(%rdi), %rbp
    movq REG_R8(%rdi), %r8
    movq REG_R9(%rdi), %r9
    movq REG_R10(%rdi), %r10
    movq REG_R11(%rdi), %r11
    movq REG_R12(%rdi), %r12
    movq REG_R13(%rdi), %r13
    movq REG_R14(%rdi), %r14
    movq REG_R15(%rdi), %r15
    movq REG_RDI(%rdi), %rdi
    jne .vmrun_resume
    .byte 0x0f, 0x01, 0xc2              # vmlaunch
    jmp .vmrun_failed
.vmrun_resume:
    .byte 0x0f, 0x01, 0xc3              # vmresume

    # The entry failed: the guest's registers were only loaded, and regs
    # still holds them.
.vmrun_failed:
    jb .vmrun_fail_invalid
.vmrun_fail_valid:
    movl $2, %eax
    jmp .vmrun_out
.vmrun_fail_invalid:
    movl $1, %eax
    jmp .vmrun_out

# The VM exit: HOST_RIP. The stack is the frame above; every general
# register but RSP is the guest's.
.globl kvm_vmx_exit
kvm_vmx_exit:
    pushq %rdi
    movq F_REGS+8(%rsp), %rdi
    movq %rax, REG_RAX(%rdi)
    movq %rbx, REG_RBX(%rdi)
    movq %rcx, REG_RCX(%rdi)
    movq %rdx, REG_RDX(%rdi)
    movq %rsi, REG_RSI(%rdi)
    movq %rbp, REG_RBP(%rdi)
    movq %r8, REG_R8(%rdi)
    movq %r9, REG_R9(%rdi)
    movq %r10, REG_R10(%rdi)
    movq %r11, REG_R11(%rdi)
    movq %r12, REG_R12(%rdi)
    movq %r13, REG_R13(%rdi)
    movq %r14, REG_R14(%rdi)
    movq %r15, REG_R15(%rdi)
    popq %rax
    movq %rax, REG_RDI(%rdi)
    .byte 0x0f, 0x20, 0xd0              # mov %cr2, %rax
    movq F_CR2(%rsp), %rsi
    movq %rax, (%rsi)
    xorl %eax, %eax

.vmrun_out:
    addq $F_SIZE, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
