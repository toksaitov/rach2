# The code of the 32-bit vDSO an i386 program is given (vdso32.c), as
# Linux's vdso32 has it: __kernel_vsyscall, __kernel_sigreturn and
# __kernel_rt_sigreturn, and clock_gettime, clock_gettime64,
# gettimeofday, time, clock_getres and getcpu as a program calls them, by
# the i386 C convention. vdso32.c copies it into the image's page at
# VDSO_TEXT_OFFSET; it is 32-bit code, which the kernel never runs.
#
# The clock functions read the data page the 64-bit vDSO reads
# (../../vdso.h), mapped one page below the image, and find it from their
# own address, since 32-bit code has no addressing relative to the
# instruction pointer. Where the page says the clock does not run from the
# TSC, or for a clock or an argument they do not serve, they make the
# system call they stand for through __kernel_vsyscall and return what the
# kernel returned, -errno for an error, as Linux's vDSO does.

# From the code's first byte back to the data page: the image's bytes
# before the code, VDSO_TEXT_OFFSET in vdsoimg.h, and the page.
.set VDSO_BACK, 0x1800

# The data page's offsets, struct vdso_data's (../../vdso.h). A field of 64
# bits is read as two words, the low first.
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
# Each clock's rate is 8 bytes before its seconds: D_MULT before D_MONO_SEC
# and D_REAL_MULT before D_REAL_SEC.
.set D_RATE_BACK,  8

# VCLOCK_TSC, and the i386 system calls the code makes.
.set VCLOCK_TSC,           1
.set NR_time,              13
.set NR_gettimeofday,      78
.set NR_sigreturn,         119
.set NR_rt_sigreturn,      173
.set NR_clock_gettime,     265
.set NR_clock_getres,      266
.set NR_getcpu,            318
.set NR_clock_gettime64,   403

.set NSEC_PER_SEC, 1000000000

.text
.code32
.p2align 4
.globl vdso32_text_start
vdso32_text_start:

# __kernel_vsyscall: the system call in EAX with the arguments in EBX,
# ECX, EDX, ESI, EDI and EBP, as `int $0x80` takes them, which AT_SYSINFO
# names, as Linux's. ECX, EDX and EBP are saved around the entry. The four
# bytes at vdso32_vsyscall_entry are nops here, which vdso32.c replaces at
# boot, as Linux's ALTERNATIVE_2 does: with `movl %esp, %ebp; sysenter`
# where the processors are Intel's, and with `movl %ecx, %ebp; syscall`
# where they are AMD's. The kernel returns from either to the landing pad,
# vdso32_landing, with EBP restored and ECX and EDX as the pops leave them;
# a call it restarts goes back to the `int $0x80` before it.
.globl vdso32_vsyscall
vdso32_vsyscall:
    pushl %ecx
    pushl %edx
    pushl %ebp
.globl vdso32_vsyscall_entry
vdso32_vsyscall_entry:
    nop
    nop
    nop
    nop
    int $0x80
.globl vdso32_landing
vdso32_landing:
    popl %ebp
    popl %edx
    popl %ecx
    ret
.globl vdso32_vsyscall_end
vdso32_vsyscall_end:

# __kernel_sigreturn and __kernel_rt_sigreturn, the restorers of the i386
# signal frames, in Linux's bytes, which debuggers and unwinders know: a
# frame's handler returns to them, and sigreturn pops the signal number
# first. A nop comes before the pair, as in Linux's, and each is aligned
# with nops, so that an unwinder that looks one byte back from a frame's
# return address finds an instruction.
    nop
.p2align 4, 0x90
.globl vdso32_sigreturn
vdso32_sigreturn:
    popl %eax
    movl $NR_sigreturn, %eax
    int $0x80
    nop
.globl vdso32_sigreturn_end
vdso32_sigreturn_end:

.p2align 4, 0x90
.globl vdso32_rt_sigreturn
vdso32_rt_sigreturn:
    movl $NR_rt_sigreturn, %eax
    int $0x80
    nop
.globl vdso32_rt_sigreturn_end
vdso32_rt_sigreturn_end:

# The data page's address, in ECX: the return address of the call in the
# routine, less its distance from the code's start, less VDSO_BACK.
# Changes the flags alone besides.
vdso32_page:
    call vdso32_page_at
vdso32_page_at:
    popl %ecx
    subl $(vdso32_page_at - vdso32_text_start + VDSO_BACK), %ecx
    ret

# A reading of the clock whose base seconds are at offset EAX of the data
# page at ECX, the nanoseconds after them, from the TSC: the seconds in
# EDX:EAX and the nanoseconds, below 10^9, in ECX, as the 64-bit vDSO's
# vdso_fine reads them. The TSC's count since the base, nought when it is
# behind, as another processor's can be by a few counts, is multiplied by
# the clock's rate, 8 bytes before its seconds, into 96 bits and shifted
# right by the page's shift, at most 32; the base's nanoseconds are added
# and the sum divided by 10^9 into seconds and nanoseconds. The sequence
# count is read before and after, and the reading made again while it is
# odd or has moved.
vdso32_fine:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %ecx, %ebp
    movl %eax, %edi
    pushl $0
1:
    movl D_SEQ(%ebp), %eax
    testl $1, %eax
    jnz 5f
    movl %eax, (%esp)
    lfence
    rdtsc
    lfence
    subl D_CYCLE_BASE(%ebp), %eax
    sbbl D_CYCLE_BASE+4(%ebp), %edx
    jae 2f
    xorl %eax, %eax
    xorl %edx, %edx
2:
    movl %edx, %ebx
    mull -D_RATE_BACK(%ebp,%edi)
    movl %eax, %esi
    movl %edx, %ecx
    movl %ebx, %eax
    mull -D_RATE_BACK(%ebp,%edi)
    addl %ecx, %eax
    adcl $0, %edx
    movl D_SHIFT(%ebp), %ecx
    cmpl $32, %ecx
    jb 3f
    movl %eax, %esi
    movl %edx, %eax
    jmp 4f
3:
    shrdl %cl, %eax, %esi
    shrdl %cl, %edx, %eax
4:
    addl 8(%ebp,%edi), %esi
    adcl 12(%ebp,%edi), %eax
    movl (%ebp,%edi), %ebx
    movl 4(%ebp,%edi), %ecx
    movl (%esp), %edx
    cmpl D_SEQ(%ebp), %edx
    jne 1b
    movl $NSEC_PER_SEC, %edi
    xorl %edx, %edx
    divl %edi
    movl %eax, %ebp
    movl %esi, %eax
    divl %edi
    addl %eax, %ebx
    adcl %ebp, %ecx
    movl %ebx, %eax
    movl %edx, %ebx
    movl %ecx, %edx
    movl %ebx, %ecx
    addl $4, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
5:
    pause
    jmp 1b

# The same clock as of the last tick, without the TSC: EDX:EAX and ECX as
# `vdso32_fine` gives them.
vdso32_coarse:
    pushl %ebx
    pushl %esi
    pushl %edi
1:
    movl D_SEQ(%ecx), %edi
    testl $1, %edi
    jnz 2f
    movl (%ecx,%eax), %ebx
    movl 4(%ecx,%eax), %esi
    movl 8(%ecx,%eax), %edx
    cmpl D_SEQ(%ecx), %edi
    jne 1b
    movl %edx, %ecx
    movl %ebx, %eax
    movl %esi, %edx
    popl %edi
    popl %esi
    popl %ebx
    ret
2:
    pause
    jmp 1b

# The clock whose id is in EAX, read into EDX:EAX and ECX as `vdso32_fine`
# gives it, with the carry flag clear: CLOCK_REALTIME and its coarse form,
# and CLOCK_MONOTONIC, whose reading CLOCK_MONOTONIC_RAW and
# CLOCK_BOOTTIME share, as in the kernel's clock_read, and its coarse form.
# The carry flag is set, with nothing read, for any other clock and while
# the clock does not run from the TSC.
vdso32_clock:
    call vdso32_page
    cmpl $VCLOCK_TSC, D_MODE(%ecx)
    jne 9f
    testl %eax, %eax
    je 1f
    cmpl $1, %eax
    je 2f
    cmpl $4, %eax
    je 2f
    cmpl $7, %eax
    je 2f
    cmpl $5, %eax
    je 3f
    cmpl $6, %eax
    je 4f
9:
    stc
    ret
1:
    movl $D_REAL_SEC, %eax
    call vdso32_fine
    clc
    ret
2:
    movl $D_MONO_SEC, %eax
    call vdso32_fine
    clc
    ret
3:
    movl $D_REAL_SEC, %eax
    call vdso32_coarse
    clc
    ret
4:
    movl $D_MONO_SEC, %eax
    call vdso32_coarse
    clc
    ret

# The system call in EAX with the first two arguments of the function that
# jumps here, made through __kernel_vsyscall, whose result the function
# returns. EBX is the caller's, and is kept.
vdso32_fallback2:
    pushl %ebx
    movl 8(%esp), %ebx
    movl 12(%esp), %ecx
    call vdso32_vsyscall
    popl %ebx
    ret

# int clock_gettime(clockid_t clk, struct old_timespec32 *ts)
#
# The seconds are cut to 32 bits, as Linux's vDSO cuts them.
.globl vdso32_clock_gettime
vdso32_clock_gettime:
    movl 4(%esp), %eax
    call vdso32_clock
    jc 9f
    movl 8(%esp), %edx
    movl %eax, (%edx)
    movl %ecx, 4(%edx)
    xorl %eax, %eax
    ret
9:
    movl $NR_clock_gettime, %eax
    jmp vdso32_fallback2
.globl vdso32_clock_gettime_end
vdso32_clock_gettime_end:

# int clock_gettime64(clockid_t clk, struct __kernel_timespec *ts)
.globl vdso32_clock_gettime64
vdso32_clock_gettime64:
    movl 4(%esp), %eax
    call vdso32_clock
    jc 9f
    pushl %ebx
    movl 12(%esp), %ebx
    movl %eax, (%ebx)
    movl %edx, 4(%ebx)
    movl %ecx, 8(%ebx)
    movl $0, 12(%ebx)
    popl %ebx
    xorl %eax, %eax
    ret
9:
    movl $NR_clock_gettime64, %eax
    jmp vdso32_fallback2
.globl vdso32_clock_gettime64_end
vdso32_clock_gettime64_end:

# int gettimeofday(struct old_timeval32 *tv, struct timezone *tz)
#
# Real time to the microsecond; a null tv reads nothing, as the kernel's
# call does. A time zone asked for is the kernel's to answer.
.globl vdso32_gettimeofday
vdso32_gettimeofday:
    cmpl $0, 8(%esp)
    jne 9f
    cmpl $0, 4(%esp)
    je 1f
    xorl %eax, %eax
    call vdso32_clock
    jc 9f
    pushl %ebx
    movl 8(%esp), %ebx
    movl %eax, (%ebx)
    movl %ecx, %eax
    xorl %edx, %edx
    movl $1000, %ecx
    divl %ecx
    movl %eax, 4(%ebx)
    popl %ebx
1:
    xorl %eax, %eax
    ret
9:
    movl $NR_gettimeofday, %eax
    jmp vdso32_fallback2
.globl vdso32_gettimeofday_end
vdso32_gettimeofday_end:

# old_time32_t time(old_time32_t *t)
#
# Real time's seconds, cut to 32 bits, as the kernel's time reads them,
# also stored at t when it is not null.
.globl vdso32_time
vdso32_time:
    xorl %eax, %eax
    call vdso32_clock
    jc 9f
    movl 4(%esp), %edx
    testl %edx, %edx
    je 1f
    movl %eax, (%edx)
1:
    ret
9:
    movl $NR_time, %eax
    jmp vdso32_fallback2
.globl vdso32_time_end
vdso32_time_end:

# int clock_getres(clockid_t clk, struct old_timespec32 *res)
#
# The resolution the kernel's clock_getres gives the clocks clock_gettime
# serves here: a count of the TSC, rounded up, for the fine clocks and the
# tick for the coarse; a null res reads nothing. Any other clock is the
# kernel's.
.globl vdso32_clock_getres
vdso32_clock_getres:
    call vdso32_page
    cmpl $VCLOCK_TSC, D_MODE(%ecx)
    jne 9f
    movl 4(%esp), %edx
    movl D_RES_NS(%ecx), %eax
    testl %edx, %edx
    je 1f
    cmpl $1, %edx
    je 1f
    cmpl $4, %edx
    je 1f
    cmpl $7, %edx
    je 1f
    movl D_COARSE_NS(%ecx), %eax
    cmpl $5, %edx
    je 1f
    cmpl $6, %edx
    je 1f
    jmp 9f
1:
    movl 8(%esp), %edx
    testl %edx, %edx
    je 2f
    movl $0, (%edx)
    movl %eax, 4(%edx)
2:
    xorl %eax, %eax
    ret
9:
    movl $NR_clock_getres, %eax
    jmp vdso32_fallback2
.globl vdso32_clock_getres_end
vdso32_clock_getres_end:

# int getcpu(unsigned *cpu, unsigned *node, void *cache)
#
# The processor and node from IA32_TSC_AUX, which the kernel sets on each
# processor to the node above bit 12 and the processor below; either
# pointer may be null, and the cache is ignored, as the kernel ignores it.
.globl vdso32_getcpu
vdso32_getcpu:
    call vdso32_page
    cmpl $1, D_GETCPU(%ecx)
    jne 9f
    rdtscp
    movl 4(%esp), %edx
    testl %edx, %edx
    je 1f
    movl %ecx, %eax
    andl $0xFFF, %eax
    movl %eax, (%edx)
1:
    movl 8(%esp), %edx
    testl %edx, %edx
    je 2f
    shrl $12, %ecx
    movl %ecx, (%edx)
2:
    xorl %eax, %eax
    ret
9:
    pushl %ebx
    movl 8(%esp), %ebx
    movl 12(%esp), %ecx
    movl 16(%esp), %edx
    movl $NR_getcpu, %eax
    call vdso32_vsyscall
    popl %ebx
    ret
.globl vdso32_getcpu_end
vdso32_getcpu_end:

.globl vdso32_text_end
vdso32_text_end:

.code64
