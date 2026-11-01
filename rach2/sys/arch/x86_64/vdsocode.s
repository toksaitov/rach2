# The code of the x86-64's vDSO: clock_gettime, gettimeofday, time, getcpu
# and clock_getres as a program calls them, which vdso.c copies into the
# image's page at VDSO_TEXT_OFFSET. The kernel never runs it.
#
# The code reads the data page (../../vdso.h), which is mapped one page
# below the image, through %rip, so it runs wherever the image is placed.
# Where the data page says the clock does not run from the TSC, or for a
# clock or an argument it does not serve, each function makes the system
# call it stands for and returns what the kernel returned, -errno for an
# error, as Linux's vDSO does; the C library turns that into errno.

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
.set D_REAL_SEC,   40
.set D_RES_NS,     56
.set D_GETCPU,     60
.set D_COARSE_NS,  64

# VCLOCK_TSC, and the system calls each function stands for.
.set VCLOCK_TSC,         1
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
# at %r8, the nanoseconds after them, from the TSC: the seconds in %rax and
# the nanoseconds, below 10^9, in %rdx. It reads the page's sequence count
# before and after, and again while it is odd or has moved; the TSC is
# read between two lfences, so that it is taken after the loads before it
# and before the count's second load. A count behind the base, as another
# processor's can be by a few counts, reads as the base. Changes %rcx and
# %r9 to %r11.
vdso_fine:
    movl D_SEQ(%r8), %r11d
    testl $1, %r11d
    jnz 3f
    movq (%r8,%rdi), %r9
    movq 8(%r8,%rdi), %r10
    lfence
    rdtsc
    lfence
    shlq $32, %rdx
    orq %rdx, %rax
    subq D_CYCLE_BASE(%r8), %rax
    jae 1f
    xorl %eax, %eax
1:
    movl D_MULT(%r8), %edx
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
    ret
3:
    pause
    jmp vdso_fine

# The same clock as of the last tick, without the TSC: %rax and %rdx as
# `vdso_fine` gives them. Changes %r11.
vdso_coarse:
    movl D_SEQ(%r8), %r11d
    testl $1, %r11d
    jnz 1f
    movq (%r8,%rdi), %rax
    movq 8(%r8,%rdi), %rdx
    cmpl D_SEQ(%r8), %r11d
    jne vdso_coarse
    ret
1:
    pause
    jmp vdso_coarse

# int clock_gettime(clockid_t clk, struct timespec *ts)
#
# CLOCK_REALTIME and its coarse form, and CLOCK_MONOTONIC, whose reading
# CLOCK_MONOTONIC_RAW and CLOCK_BOOTTIME share, as in the kernel's
# clock_read, and its coarse form; any other clock is the kernel's.
.globl vdso_clock_gettime
vdso_clock_gettime:
    leaq vdso_text_start-VDSO_BACK(%rip), %r8
    cmpl $VCLOCK_TSC, D_MODE(%r8)
    jne 9f
    testl %edi, %edi
    je 1f
    cmpl $1, %edi
    je 2f
    cmpl $4, %edi
    je 2f
    cmpl $7, %edi
    je 2f
    cmpl $5, %edi
    je 3f
    cmpl $6, %edi
    je 4f
    jmp 9f
1:
    movl $D_REAL_SEC, %edi
    call vdso_fine
    jmp 5f
2:
    movl $D_MONO_SEC, %edi
    call vdso_fine
    jmp 5f
3:
    movl $D_REAL_SEC, %edi
    call vdso_coarse
    jmp 5f
4:
    movl $D_MONO_SEC, %edi
    call vdso_coarse
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
    cmpl $VCLOCK_TSC, D_MODE(%r8)
    jne 9f
    movq %rdi, %rsi
    movl $D_REAL_SEC, %edi
    call vdso_fine
    movq %rax, (%rsi)
    movq %rdx, %rax
    xorl %edx, %edx
    movl $1000, %ecx
    divq %rcx
    movq %rax, 8(%rsi)
1:
    xorl %eax, %eax
    ret
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
    cmpl $VCLOCK_TSC, D_MODE(%r8)
    jne 9f
    movq %rdi, %rsi
    movl $D_REAL_SEC, %edi
    call vdso_fine
    testq %rsi, %rsi
    je 1f
    movq %rax, (%rsi)
1:
    ret
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
    cmpl $VCLOCK_TSC, D_MODE(%r8)
    jne 9f
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
