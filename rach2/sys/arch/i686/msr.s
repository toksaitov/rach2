# Optional MSR accesses. isr.c resumes a #GP at the failure return
# (`msr_fixup`).
.text

# int kcpu_rdmsr_safe(uint32_t index, uint64_t *value)
.global kcpu_rdmsr_safe
.type kcpu_rdmsr_safe, @function
kcpu_rdmsr_safe:
    movl 4(%esp), %ecx
.global kcpu_rdmsr_insn
kcpu_rdmsr_insn:
    rdmsr
    movl 8(%esp), %ecx
    movl %eax, 0(%ecx)
    movl %edx, 4(%ecx)
    xorl %eax, %eax
    ret
.size kcpu_rdmsr_safe, .-kcpu_rdmsr_safe

# int kcpu_wrmsr_safe(uint32_t index, uint64_t value)
.global kcpu_wrmsr_safe
.type kcpu_wrmsr_safe, @function
kcpu_wrmsr_safe:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movl 12(%esp), %edx
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
# back after, as Linux's rdmsr_safe_regs (arch/x86/lib/msr-reg.S). The
# pointer to regs stays at 0(%esp) while EDI holds regs[7].
.global kcpu_msr_safe_regs
.type kcpu_msr_safe_regs, @function
kcpu_msr_safe_regs:
    pushl %ebx
    pushl %ebp
    pushl %esi
    pushl %edi
    movl 24(%esp), %edi
    pushl %edi
    movl 0(%edi), %eax
    movl 4(%edi), %ecx
    movl 8(%edi), %edx
    movl 12(%edi), %ebx
    movl 20(%edi), %ebp
    movl 24(%edi), %esi
    cmpl $0, 24(%esp)
    movl 28(%edi), %edi
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
    pushl %edi
    movl 4(%esp), %edi
    movl %eax, 0(%edi)
    movl %ecx, 4(%edi)
    movl %edx, 8(%edi)
    movl %ebx, 12(%edi)
    movl %ebp, 20(%edi)
    movl %esi, 24(%edi)
    popl %eax
    movl %eax, 28(%edi)
    xorl %eax, %eax
.Lmsr_regs_out:
    addl $4, %esp
    popl %edi
    popl %esi
    popl %ebp
    popl %ebx
    ret
.global kcpu_msr_regs_fault
kcpu_msr_regs_fault:
    movl $-1, %eax
    jmp .Lmsr_regs_out
.size kcpu_msr_safe_regs, .-kcpu_msr_safe_regs
