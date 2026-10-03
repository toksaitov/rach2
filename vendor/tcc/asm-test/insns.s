# Instructions of the code a processor runs before long mode, which
# check_asm.sh assembles as 16-bit code and as 32-bit code.
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %cs, %ax
    movw $0x7000, %sp
    movl $0x12345678, %eax
    movw $0x1234, %ax
    movb $0x12, %al
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    movl %cr4, %eax
    movl %eax, %cr4
    movl %eax, %cr3
    movl %cr2, %edx
    movl %dr7, %eax
    movl %eax, %dr7
    lgdt 0x100
    lgdtl 0x100
    lgdtw 0x100
    lidt 0x200
    lidtl 0x200
    sgdt 0x300
    sidtl 0x300
    lgdtl (%bx)
    lgdt (%esi)
    lgdtl %cs:0x80
    ljmp $0x8, $0x1234
    ljmpw $0x8, $0x1234
    ljmpl $0x8, $0x12345678
    lcall $0x8, $0x1234
    lcalll $0x8, $0x12345678
    ljmp *(%bx)
    ljmpl *(%bx)
    lcall *4(%si)
    movw (%bx,%si), %ax
    movw (%bx,%di), %ax
    movw (%bp,%si), %ax
    movw (%bp,%di), %ax
    movw (%si), %ax
    movw (%di), %ax
    movw (%bp), %ax
    movw (%bx), %ax
    movw 4(%bp), %ax
    movw -4(%bp), %ax
    movw 0x1234(%di), %ax
    movw 0x1234(%bx,%si), %cx
    movl (%bx), %eax
    movl %eax, 8(%bx)
    movl (%esi), %eax
    movl 8(%esp), %eax
    movl (%eax,%ebx,4), %ecx
    movw 0x1234, %ax
    movb %al, 0x10
    movl 0x1234, %eax
    movw %es:(%di), %ax
    movw %fs:0x10, %ax
    addl $1, %eax
    addw $0x1234, %bx
    subl $0x12345678, %ecx
    andw $0xff00, %dx
    orb $0x80, %al
    xorl %eax, %eax
    cmpw $5, (%bx)
    cmpl $5, (%bx)
    testl $0x20000000, %edx
    testw $1, %ax
    incl %eax
    incw %ax
    incw (%bx)
    decl %ecx
    decb %al
    negw %ax
    notl %eax
    mull %ecx
    imulw %bx, %ax
    imull $10, %eax, %ebx
    divw %cx
    shll $4, %eax
    shrw $2, %ax
    sarl %cl, %edx
    rolw $3, %ax
    btl $5, %eax
    btsw $2, (%bx)
    bsfl %eax, %ecx
    bswap %eax
    movzwl %ax, %eax
    movzbl %al, %eax
    movzbw %al, %ax
    movsbl %al, %eax
    movswl %ax, %eax
    movsbw %al, %ax
    leal 4(%esi), %eax
    leaw 4(%si), %ax
    leal 4(%bx), %eax
    xchgl %eax, %ebx
    xchgw %ax, %cx
    xchgb %al, %ah
    lss (%bx), %esp
    lss (%bx), %sp
    lfs (%bx), %ax
    str %eax
    str %ax
    ltr %ax
    lldt %ax
    sldt %ax
    smsw %ax
    lmsw %ax
    push %cs
    pushw %ds
    pushl %es
    pop %ds
    popl %fs
    push %gs
    push %ax
    push %eax
    pushl $5
    pushw $5
    push $5
    push $0x1234
    pushl $0x12345678
    pushw (%bx)
    pushl (%bx)
    popw (%bx)
    pop %ax
    pop %eax
    pushf
    pushfl
    pushfw
    popf
    popfl
    popfw
    pusha
    pushal
    pushaw
    popa
    popal
    iret
    iretl
    iretw
    lret
    lretl
    lretw
    ret
    retl
    retw
    ret $4
    lret $8
    cbw
    cwde
    cwd
    cdq
    cbtw
    cwtl
    cwtd
    cltd
    inb $0x60, %al
    inw %dx, %ax
    inl %dx, %eax
    outb %al, $0x80
    outw %ax, %dx
    outl %eax, %dx
    movsb
    movsw
    movsl
    stosb
    stosw
    stosl
    lodsl
    rep movsb
    rep stosl
    cpuid
    rdmsr
    wrmsr
    rdtsc
    hlt
    nop
    pause
    sti
    clc
    stc
    int $0x10
    int $3
    jmp *%ax
    jmp *%eax
    call *%ax
    call *(%bx)
    jmp *(%bx)
    xgetbv
    xsetbv
    wbinvd
    invlpg (%bx)
    lock incw (%bx)
    cmpxchgl %ecx, (%bx)
    xaddw %ax, (%bx)
    enter $16, $0
    leave
    sete %al
    setne (%bx)
    cmovzl %eax, %ebx
    cmovnew %ax, %bx
