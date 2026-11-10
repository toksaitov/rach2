# The entry to an SVM guest, for svm.c. TinyCC's assembler names none of
# the SVM instructions, so each is written in its bytes.

.text

# The offsets of struct kvm_regs, kvm_abi.h's.
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

# The frame kvm_svm_vmrun keeps on the stack while the guest runs.
.set F_REGS, 0
.set F_VMCB, 8
.set F_HOST, 16
.set F_SIZE, 24

# int kvm_svm_vmrun(struct kvm_regs *regs, uint64_t vmcb_pa, uint64_t host_pa)
#
# Run the guest of the VMCB at vmcb_pa until its next exit, with its
# general registers but RAX and RSP, which the VMCB holds, from regs, and
# back into regs at the exit; the result is 0. VMSAVE keeps the host's FS,
# GS, TR, LDTR and system-call MSRs in the page at host_pa and VMLOAD puts
# them back after; VMLOAD and VMSAVE of the VMCB move the guest's. GIF is
# clear from before the guest's state is loaded until the host's is back,
# and RFLAGS.IF set across VMRUN, so that a physical interrupt or NMI ends
# the run with an exit and stays pending until the caller turns interrupts
# on; VMRUN keeps the host's RFLAGS, which the exit restores. Called with
# interrupts off and GIF set; returns with interrupts off and GIF set.
.globl kvm_svm_vmrun
kvm_svm_vmrun:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rdx
    pushq %rsi
    pushq %rdi

    movq %rdx, %rax
    .byte 0x0f, 0x01, 0xdb              # vmsave (%rax)
    .byte 0x0f, 0x01, 0xdd              # clgi
    sti

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
    movq F_VMCB(%rsp), %rax
    .byte 0x0f, 0x01, 0xda              # vmload (%rax)
    .byte 0x0f, 0x01, 0xd8              # vmrun (%rax)
    .byte 0x0f, 0x01, 0xdb              # vmsave (%rax)

    # The exit: RAX and RSP are the host's again; every other general
    # register is the guest's.
    pushq %rdi
    movq F_REGS+8(%rsp), %rdi
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

    movq F_HOST(%rsp), %rax
    .byte 0x0f, 0x01, 0xda              # vmload (%rax)
    cli
    .byte 0x0f, 0x01, 0xdc              # stgi

    addq $F_SIZE, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    xorl %eax, %eax
    ret
