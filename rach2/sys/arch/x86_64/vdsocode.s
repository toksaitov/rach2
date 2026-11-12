# The code of the x86-64's vDSO: clock_gettime, gettimeofday, time, getcpu
# and clock_getres as a program calls them, which vdso.c copies into the
# image's page at VDSO_TEXT_OFFSET. The kernel never runs it.
#
# The code reads the data page (../../vdso.h), which is mapped one page
# below the image, and KVM's clock page for the boot processor, one page
# below that, through %rip, so it runs wherever the image is placed. Where
# the data page says the clock does not run from the TSC, where KVM's page
# has withdrawn its stable bit, or for a clock or an argument it does not
# serve, each function makes the system call it stands for and returns
# what the kernel returned, -errno for an error, as Linux's vDSO does; the
# C library turns that into errno.

# From the code's first byte back to the data page: the image's bytes
# before the code, VDSO_TEXT_OFFSET in vdso.c, and the page.
.set VDSO_BACK, 0x1800

# The data page's offsets, struct vdso_data's (../../vdso.h).
.set D_SEQ,        0
.set D_MODE,       4
.set D_CYCLE_BASE, 8
.set D_MULT,       16
.set D_SHIFT,      20
.set D_MONO_SEC,   24
.set D_REAL_MULT,  40
.set D_REAL_SEC,   48
.set D_RES_NS,     64
.set D_GETCPU,     68
.set D_COARSE_NS,  72
.set D_RAW_MULT,   80
.set D_RAW_SEC,    88
# From the data page back to KVM's clock page, and that page's version
# and flags, pvclock_vcpu_time_info's (../x86/tsc.c), with
# PVCLOCK_TSC_STABLE_BIT.
.set KVM_BACK,         0x1000
.set PVTI_VERSION,     0
.set PVTI_FLAGS,       29
.set PVCLOCK_TSC_STABLE_BIT, 1
# Each clock's rate is 8 bytes before its seconds: D_MULT before D_MONO_SEC,
# D_REAL_MULT before D_REAL_SEC and D_RAW_MULT before D_RAW_SEC.
.set D_RATE_BACK,  8

# VCLOCK_NONE and VCLOCK_PVCLOCK, and the system calls each function
# stands for.
.set VCLOCK_NONE,        0
.set VCLOCK_PVCLOCK,     2
.set SYS_gettimeofday,   96
.set SYS_time,           201
.set SYS_clock_gettime,  228
.set SYS_clock_getres,   229
.set SYS_getcpu,         309

.set NSEC_PER_SEC, 1000000000

.text
.p2align 4
.globl vdso_text_start
vdso_text_start:

# A reading of the clock whose base seconds are at %rdi in the data page
# at %r8, the nanoseconds after them and its rate before them, from the
# TSC: the seconds in %rax and the nanoseconds, below 10^9, in %rdx, with
# the carry clear. It reads the page's sequence count before and after,
# and again while it is odd or has moved; the TSC is read between two
# lfences, so that it is taken after the loads before it and before the
# count's second load. A count behind the base, as another processor's can
# be by a few counts, reads as the base. Each reading checks the page's
# mode after the count, as Linux's do_hres does (lib/vdso/gettimeofday.c:
# 141-169), and returns with the carry set and nothing read when the clock
# has stopped, so that a reading the stop overtakes makes the system call.
# In VCLOCK_PVCLOCK the TSC is read as Linux's vread_pvclock reads it
# (arch/x86/include/asm/vdso/gettimeofday.h:197-232): KVM's page's version
# made even, its stable bit checked, the TSC read, and the version read
# again, the TSC read again when it moved; a clear bit returns with the
# carry set, so that the system call reads the clock. Changes %rcx and %r9
# to %r11.
vdso_fine:
    movl D_SEQ(%r8), %r11d
    testl $1, %r11d
    jnz 3f
    movl D_MODE(%r8), %ecx
    cmpl $VCLOCK_NONE, %ecx
    je 4f
    movq (%r8,%rdi), %r9
    movq 8(%r8,%rdi), %r10
    cmpl $VCLOCK_PVCLOCK, %ecx
    je 5f
    lfence
    rdtsc
    lfence
6:
    shlq $32, %rdx
    orq %rdx, %rax
    subq D_CYCLE_BASE(%r8), %rax
    jae 1f
    xorl %eax, %eax
1:
    movl -D_RATE_BACK(%r8,%rdi), %edx
    mulq %rdx
    movl D_SHIFT(%r8), %ecx
    shrdq %cl, %rdx, %rax
    addq %rax, %r10
    cmpl D_SEQ(%r8), %r11d
    jne vdso_fine
    movq %r9, %rax
    movq %r10, %rdx
    cmpq $NSEC_PER_SEC, %rdx
    jb 2f
    movq %r10, %rax
    xorl %edx, %edx
    movl $NSEC_PER_SEC, %ecx
    divq %rcx
    addq %r9, %rax
2:
    clc
    ret
3:
    pause
    jmp vdso_fine
4:
    stc
    ret
5:
    movl -KVM_BACK+PVTI_VERSION(%r8), %ecx
    andl $~1, %ecx
    testb $PVCLOCK_TSC_STABLE_BIT, -KVM_BACK+PVTI_FLAGS(%r8)
    jz 4b
    lfence
    rdtsc
    lfence
    cmpl -KVM_BACK+PVTI_VERSION(%r8), %ecx
    jne 5b
    jmp 6b

# The same clock as of the last tick, without the TSC: %rax and %rdx and
# the carry as `vdso_fine` gives them. Changes %r11.
vdso_coarse:
    movl D_SEQ(%r8), %r11d
    testl $1, %r11d
    jnz 1f
    cmpl $VCLOCK_NONE, D_MODE(%r8)
    je 2f
    movq (%r8,%rdi), %rax
    movq 8(%r8,%rdi), %rdx
    cmpl D_SEQ(%r8), %r11d
    jne vdso_coarse
    clc
    ret
1:
    pause
    jmp vdso_coarse
2:
    stc
    ret

# int clock_gettime(clockid_t clk, struct timespec *ts)
#
# CLOCK_REALTIME and its coarse form, CLOCK_MONOTONIC, whose reading
# CLOCK_BOOTTIME shares, as in the kernel's clock_read, and its coarse
# form, and CLOCK_MONOTONIC_RAW; any other clock is the kernel's, and so is
# a reading the clock's stop overtakes, with the clock in %edi again.
.globl vdso_clock_gettime
vdso_clock_gettime:
    leaq vdso_text_start-VDSO_BACK(%rip), %r8
    cmpl $VCLOCK_NONE, D_MODE(%r8)
    je 9f
    testl %edi, %edi
    je 1f
    cmpl $1, %edi
    je 2f
    cmpl $4, %edi
    je 6f
    cmpl $7, %edi
    je 2f
    cmpl $5, %edi
    je 3f
    cmpl $6, %edi
    je 4f
    jmp 9f
6:
    movl $D_RAW_SEC, %eax
    jmp 7f
1:
    movl $D_REAL_SEC, %eax
    jmp 7f
2:
    movl $D_MONO_SEC, %eax
7:
    pushq %rdi
    movl %eax, %edi
    call vdso_fine
    popq %rdi
    jc 9f
    jmp 5f
3:
    movl $D_REAL_SEC, %eax
    jmp 8f
4:
    movl $D_MONO_SEC, %eax
8:
    pushq %rdi
    movl %eax, %edi
    call vdso_coarse
    popq %rdi
    jc 9f
5:
    movq %rax, (%rsi)
    movq %rdx, 8(%rsi)
    xorl %eax, %eax
    ret
9:
    movl $SYS_clock_gettime, %eax
    syscall
    ret
.globl vdso_clock_gettime_end
vdso_clock_gettime_end:

# int gettimeofday(struct timeval *tv, struct timezone *tz)
#
# Real time to the microsecond; a null tv reads nothing, as the kernel's
# call does. A time zone asked for is the kernel's to answer.
.globl vdso_gettimeofday
vdso_gettimeofday:
    testq %rsi, %rsi
    jne 9f
    testq %rdi, %rdi
    je 1f
    leaq vdso_text_start-VDSO_BACK(%rip), %r8
    cmpl $VCLOCK_NONE, D_MODE(%r8)
    je 9f
    movq %rdi, %rsi
    movl $D_REAL_SEC, %edi
    call vdso_fine
    jc 8f
    movq %rax, (%rsi)
    movq %rdx, %rax
    xorl %edx, %edx
    movl $1000, %ecx
    divq %rcx
    movq %rax, 8(%rsi)
1:
    xorl %eax, %eax
    ret
8:
    movq %rsi, %rdi
    xorl %esi, %esi
9:
    movl $SYS_gettimeofday, %eax
    syscall
    ret
.globl vdso_gettimeofday_end
vdso_gettimeofday_end:

# time_t time(time_t *t)
#
# Real time's seconds, as the kernel's time reads them, also stored at t
# when it is not null.
.globl vdso_time
vdso_time:
    leaq vdso_text_start-VDSO_BACK(%rip), %r8
    cmpl $VCLOCK_NONE, D_MODE(%r8)
    je 9f
    movq %rdi, %rsi
    movl $D_REAL_SEC, %edi
    call vdso_fine
    jc 8f
    testq %rsi, %rsi
    je 1f
    movq %rax, (%rsi)
1:
    ret
8:
    movq %rsi, %rdi
9:
    movl $SYS_time, %eax
    syscall
    ret
.globl vdso_time_end
vdso_time_end:

# int getcpu(unsigned *cpu, unsigned *node, void *cache)
#
# The processor and node from IA32_TSC_AUX, which the kernel sets on each
# processor to the node above bit 12 and the processor below; either
# pointer may be null, and the cache is ignored, as the kernel ignores it.
.globl vdso_getcpu
vdso_getcpu:
    leaq vdso_text_start-VDSO_BACK(%rip), %r8
    cmpl $1, D_GETCPU(%r8)
    jne 9f
    rdtscp
    testq %rdi, %rdi
    je 1f
    movl %ecx, %eax
    andl $0xFFF, %eax
    movl %eax, (%rdi)
1:
    testq %rsi, %rsi
    je 2f
    shrl $12, %ecx
    movl %ecx, (%rsi)
2:
    xorl %eax, %eax
    ret
9:
    movl $SYS_getcpu, %eax
    syscall
    ret
.globl vdso_getcpu_end
vdso_getcpu_end:

# int clock_getres(clockid_t clk, struct timespec *res)
#
# The resolution the kernel's clock_getres gives the clocks clock_gettime
# serves here: a count of the TSC, rounded up, for the fine clocks and the
# tick for the coarse; a null res reads nothing. Any other clock is the
# kernel's.
.globl vdso_clock_getres
vdso_clock_getres:
    leaq vdso_text_start-VDSO_BACK(%rip), %r8
    cmpl $VCLOCK_NONE, D_MODE(%r8)
    je 9f
    movl D_RES_NS(%r8), %eax
    testl %edi, %edi
    je 1f
    cmpl $1, %edi
    je 1f
    cmpl $4, %edi
    je 1f
    cmpl $7, %edi
    je 1f
    movl D_COARSE_NS(%r8), %eax
    cmpl $5, %edi
    je 1f
    cmpl $6, %edi
    je 1f
    jmp 9f
1:
    testq %rsi, %rsi
    je 2f
    movq $0, (%rsi)
    movq %rax, 8(%rsi)
2:
    xorl %eax, %eax
    ret
9:
    movl $SYS_clock_getres, %eax
    syscall
    ret
.globl vdso_clock_getres_end
vdso_clock_getres_end:

.globl vdso_text_end
vdso_text_end:
