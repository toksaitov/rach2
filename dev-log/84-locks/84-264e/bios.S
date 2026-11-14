/* A 64 KiB firmware image that performs one operation N times and then
 * writes 0 to isa-debug-exit at port 0xf4: KIND 0 reads PORT, 1 reads MSR
 * 0x1b, 2 runs CPUID, 3 does nothing. */
    .code16
    .text
    .org 0
start:
    cli
    movl $N, %ecx
    movw $PORT, %dx
    testl %ecx, %ecx
    jz done
1:
.if KIND == 0
    inb %dx, %al
.elseif KIND == 1
    pushl %ecx
    movl $0x1b, %ecx
    rdmsr
    popl %ecx
.elseif KIND == 2
    pushl %ecx
    xorl %eax, %eax
    cpuid
    popl %ecx
.else
    nop
.endif
    decl %ecx
    jnz 1b
done:
    movw $0xf4, %dx
    xorb %al, %al
    outb %al, %dx
    hlt
    jmp done
    .org 0xfff0
    ljmp $0xf000, $start
    .org 0x10000
