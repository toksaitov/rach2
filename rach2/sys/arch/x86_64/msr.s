# Optional MSR accesses. context.c resumes a #GP at the failure return.
.section .text
.code64

# int kcpu_rdmsr_safe(uint32_t index, uint64_t *value)
.global kcpu_rdmsr_safe
.type kcpu_rdmsr_safe, @function
kcpu_rdmsr_safe:
    movl %edi, %ecx
.global x86_64_rdmsr_insn
x86_64_rdmsr_insn:
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
.global x86_64_wrmsr_insn
x86_64_wrmsr_insn:
    wrmsr
    xorl %eax, %eax
    ret
.size kcpu_wrmsr_safe, .-kcpu_wrmsr_safe

.global x86_64_msr_fault
x86_64_msr_fault:
    movl $-1, %eax
    ret

.section .note.GNU-stack,"",@progbits
