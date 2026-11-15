# Optional MSR accesses. isr.c resumes a #GP at the failure return
# (`msr_fixup`).
.section .text
.code64

# int kcpu_rdmsr_safe(uint32_t index, uint64_t *value)
.global kcpu_rdmsr_safe
.type kcpu_rdmsr_safe, @function
kcpu_rdmsr_safe:
    movl %edi, %ecx
.global kcpu_rdmsr_insn
kcpu_rdmsr_insn:
    rdmsr
    shlq $32, %rdx
    orq %rdx, %rax
    movq %rax, (%rsi)
    xorl %eax, %eax
    ret
.size kcpu_rdmsr_safe, .-kcpu_rdmsr_safe

# int kcpu_wrmsr_safe(uint32_t index, uint64_t value)
.global kcpu_wrmsr_safe
.type kcpu_wrmsr_safe, @function
kcpu_wrmsr_safe:
    movl %edi, %ecx
    movq %rsi, %rax
    movq %rsi, %rdx
    shrq $32, %rdx
.global kcpu_wrmsr_insn
kcpu_wrmsr_insn:
    wrmsr
    xorl %eax, %eax
    ret
.size kcpu_wrmsr_safe, .-kcpu_wrmsr_safe

.global kcpu_msr_fault
kcpu_msr_fault:
    movl $-1, %eax
    ret

# int kcpu_msr_safe_regs(int write, uint32_t *regs)
# RDMSR, or WRMSR when write is not 0, with EAX, ECX, EDX, EBX, EBP, ESI and
# EDI loaded from regs[0] to regs[7], skipping regs[4], ESP's, and stored
# back after, as Linux's rdmsr_safe_regs (arch/x86/lib/msr-reg.S).
.global kcpu_msr_safe_regs
.type kcpu_msr_safe_regs, @function
kcpu_msr_safe_regs:
    pushq %rbx
    pushq %rbp
    pushq %r12
    movq %rsi, %r12
    movl %edi, %r11d
    movl 0(%r12), %eax
    movl 4(%r12), %ecx
    movl 8(%r12), %edx
    movl 12(%r12), %ebx
    movl 20(%r12), %ebp
    movl 24(%r12), %esi
    movl 28(%r12), %edi
    testl %r11d, %r11d
    jnz .Lmsr_regs_write
.global kcpu_rdmsr_regs_insn
kcpu_rdmsr_regs_insn:
    rdmsr
    jmp .Lmsr_regs_store
.Lmsr_regs_write:
.global kcpu_wrmsr_regs_insn
kcpu_wrmsr_regs_insn:
    wrmsr
.Lmsr_regs_store:
    movl %eax, 0(%r12)
    movl %ecx, 4(%r12)
    movl %edx, 8(%r12)
    movl %ebx, 12(%r12)
    movl %ebp, 20(%r12)
    movl %esi, 24(%r12)
    movl %edi, 28(%r12)
    xorl %eax, %eax
.Lmsr_regs_out:
    popq %r12
    popq %rbp
    popq %rbx
    ret
.global kcpu_msr_regs_fault
kcpu_msr_regs_fault:
    movl $-1, %eax
    jmp .Lmsr_regs_out
.size kcpu_msr_safe_regs, .-kcpu_msr_safe_regs

.section .note.GNU-stack,"",@progbits
