# The system instructions of 64-bit kernel code, and a few of the forms
# the kernel's entry and switch use.
    iretq
    iretl
    iretw
    lretq
    lretl
    lretw
    lretq $16
    lretl $8
    lretw $6
    sysretq
    syscall
    swapgs
    rdmsr
    wrmsr
    rdtsc
    rdtscp
    cpuid
    xgetbv
    xsetbv
    xsave (%rdi)
    xsave64 (%rdi)
    xrstor (%rdi)
    xrstor64 (%rdi)
    xsaveopt (%rdi)
    xsaveopt64 (%rdi)
    xsavec (%rdi)
    xsavec64 8(%rsi)
    fxsave (%rdi)
    fxsave64 (%rdi)
    fxrstor (%rdi)
    fxrstor64 (%rdi)
    fxsaveq (%rdi)
    fxrstorq (%rdi)
    movq %cr0, %rax
    movq %rax, %cr0
    movq %cr3, %rax
    movq %rax, %cr3
    movq %cr4, %rdx
    movq %rdx, %cr4
    movq %cr2, %rax
    lgdt (%rdi)
    lgdtq (%rdi)
    lidt (%rdi)
    lidtq (%rdi)
    sgdt (%rdi)
    sidtq (%rdi)
    ltr %ax
    ljmp *(%rax)
    ljmpl *(%rax)
    ljmpw *(%rax)
    lcall *(%rax)
    lcalll *(%rax)
    lcallw *(%rax)
    pushfq
    popfq
    pushf
    popf
    pushfw
    popfw
    ret
    retq
    retw
    ret $16
    retq $16
    retw $4
    cqto
    cwtl
    cbtw
    invlpg (%rax)
    wbinvd
    hlt
    cli
    sti
    pause
    pushq $0x10
    pushq %rax
    popq %r15
    movl %eax, %ds
    movq %rsp, %gs:8
    movq %gs:16, %rsp
    jmp *%rax
    call *%rax
back:  nop
    jecxz back
