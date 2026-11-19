# The code of the 32-bit vDSO an i386 program is given (vdso32.c), as
# Linux's vdso32 has it: __kernel_vsyscall, __kernel_sigreturn and
# __kernel_rt_sigreturn, and clock_gettime, clock_gettime64,
# gettimeofday, time, clock_getres and getcpu as a program calls them, by
# the i386 C convention. vdso32.c copies it into the image's page at
# VDSO_TEXT_OFFSET; it is 32-bit code, which the kernel never runs.
#
# The clock functions read the data page the 64-bit vDSO reads
# (../../vdso.h), mapped one page below the image, and KVM's clock page for
# the boot processor, one page below that, and find them from their own
# address, since 32-bit code has no addressing relative to the instruction
# pointer. Where the page says the clock does not run from the TSC, where
# KVM's page has withdrawn its stable bit, fine time takes the system call.
# Coarse time and time() read the data page in every mode. For a clock or
# an argument it does not serve, the vDSO makes the system call through
# __kernel_vsyscall and returns what the kernel returned, -errno for an
# error, as Linux's vDSO does.

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
.set D_RAW_MULT,   80
.set D_RAW_SEC,    88
.set D_TZ_MW,      104
.set D_TZ_DST,     108
.set D_COARSE_MONO_SEC, 112
.set D_COARSE_REAL_SEC, 128
# From the data page back to KVM's clock page, and that page's fields,
# pvclock_vcpu_time_info's (../x86/tsc.c), with PVCLOCK_TSC_STABLE_BIT.
.set KVM_BACK,         0x1000
.set PVTI_VERSION,     0
.set PVTI_TSC,         8
.set PVTI_SYSTEM,      16
.set PVTI_MUL,         24
.set PVTI_SHIFT,       28
.set PVTI_FLAGS,       29
.set PVCLOCK_TSC_STABLE_BIT, 1
# Each clock's rate is 8 bytes before its seconds: D_MULT before D_MONO_SEC,
# D_REAL_MULT before D_REAL_SEC and D_RAW_MULT before D_RAW_SEC.
.set D_RATE_BACK,  8

# VCLOCK_NONE and VCLOCK_PVCLOCK, and the i386 system calls the code
# makes.
.set VCLOCK_NONE,          0
.set VCLOCK_PVCLOCK,       2
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
# page at ECX, the nanoseconds after them, from the page's count: the
# seconds in EDX:EAX and the nanoseconds, below 10^9, in ECX, as the 64-bit
# vDSO's vdso_fine reads them. The count since the base, nought when it is
# behind, as another processor's can be by a few counts, is multiplied by
# the clock's rate, 8 bytes before its seconds, into 96 bits and shifted
# right by the page's shift, at most 32; the base's nanoseconds are added
# and the sum divided by 10^9 into seconds and nanoseconds. The sequence
# count is read before and after, and the reading made again while it is
# odd or has moved. Each pass checks the page's mode after the count, as
# Linux's do_hres does (lib/vdso/gettimeofday.c:141-169), and returns with
# the carry set and nothing read when the clock has stopped, so that a
# reading the stop overtakes makes the system call; the carry is clear
# otherwise. The count is the TSC in VCLOCK_TSC, and in VCLOCK_PVCLOCK
# KVM's clock, read as Linux's vread_pvclock reads it (arch/x86/include/
# asm/vdso/gettimeofday.h:197-232) and as the 64-bit vDSO's vdso_fine
# reads it: the TSC converted through KVM's page by the 32-bit steps of
# pvclock_scale_delta (asm/pvclock.h:44-81), the count shifted, then each
# half times the page's rate, the low half's product shifted down 32 and
# added to the high's, then the system time added. A clear stable bit
# returns with the carry set.
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
    movl D_MODE(%ebp), %eax
    cmpl $VCLOCK_NONE, %eax
    je 6f
    cmpl $VCLOCK_PVCLOCK, %eax
    je 7f
    lfence
    rdtsc
    lfence
8:
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
    clc
    ret
5:
    pause
    jmp 1b
6:
    addl $4, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    stc
    ret
7:
    movl -KVM_BACK+PVTI_VERSION(%ebp), %esi
    andl $~1, %esi
    testb $PVCLOCK_TSC_STABLE_BIT, -KVM_BACK+PVTI_FLAGS(%ebp)
    jz 6b
    pushl %esi
    lfence
    rdtsc
    lfence
    subl -KVM_BACK+PVTI_TSC(%ebp), %eax
    sbbl -KVM_BACK+PVTI_TSC+4(%ebp), %edx
    jae 9f
    xorl %eax, %eax
    xorl %edx, %edx
9:
    movsbl -KVM_BACK+PVTI_SHIFT(%ebp), %ecx
    testl %ecx, %ecx
    js 10f
    testb $32, %cl
    jz 11f
    movl %eax, %edx
    xorl %eax, %eax
11:
    shldl %cl, %eax, %edx
    shll %cl, %eax
    jmp 12f
10:
    negl %ecx
    testb $32, %cl
    jz 13f
    movl %edx, %eax
    xorl %edx, %edx
13:
    shrdl %cl, %edx, %eax
    shrl %cl, %edx
12:
    movl %edx, %ebx
    mull -KVM_BACK+PVTI_MUL(%ebp)
    movl %edx, %esi
    movl %ebx, %eax
    mull -KVM_BACK+PVTI_MUL(%ebp)
    addl %esi, %eax
    adcl $0, %edx
    addl -KVM_BACK+PVTI_SYSTEM(%ebp), %eax
    adcl -KVM_BACK+PVTI_SYSTEM+4(%ebp), %edx
    popl %esi
    cmpl -KVM_BACK+PVTI_VERSION(%ebp), %esi
    jne 7b
    andl $0x7FFFFFFF, %edx
    jmp 8b

# A coarse clock, the seconds at the offset in EAX and the nanoseconds 8
# bytes after them, the time at the tick's last edge, without the TSC:
# EDX:EAX and ECX as `vdso32_fine` gives them, with the carry clear.
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
    clc
    ret
2:
    pause
    jmp 1b

# The clock whose id is in EAX, read into EDX:EAX and ECX as `vdso32_fine`
# gives it, with the carry flag clear: CLOCK_REALTIME and its coarse form,
# CLOCK_MONOTONIC, whose reading CLOCK_BOOTTIME shares, as in the kernel's
# clock_read, and its coarse form, and CLOCK_MONOTONIC_RAW.
# The carry flag is set, with nothing read, for any other clock or when
# a fine reading finds no vDSO clock.
vdso32_clock:
    call vdso32_page
    testl %eax, %eax
    je 1f
    cmpl $1, %eax
    je 2f
    cmpl $4, %eax
    je 5f
    cmpl $7, %eax
    je 2f
    cmpl $5, %eax
    je 3f
    cmpl $6, %eax
    je 4f
    stc
    ret
5:
    movl $D_RAW_SEC, %eax
    jmp vdso32_fine
1:
    movl $D_REAL_SEC, %eax
    jmp vdso32_fine
2:
    movl $D_MONO_SEC, %eax
    jmp vdso32_fine
3:
    movl $D_COARSE_REAL_SEC, %eax
    jmp vdso32_coarse
4:
    movl $D_COARSE_MONO_SEC, %eax
    jmp vdso32_coarse

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
# Real time to the microsecond at tv, then the kernel's time zone from the
# data page at tz, each skipped when null, as the 64-bit vDSO's
# gettimeofday; a time the vDSO cannot read makes the system call, which
# writes both.
.globl vdso32_gettimeofday
vdso32_gettimeofday:
    cmpl $0, 4(%esp)
    je 2f
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
2:
    movl 8(%esp), %edx
    testl %edx, %edx
    je 1f
    call vdso32_page
    movl D_TZ_MW(%ecx), %eax
    movl %eax, (%edx)
    movl D_TZ_DST(%ecx), %eax
    movl %eax, 4(%edx)
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
# Real time's seconds at the last tick, cut to 32 bits, also stored at t
# when it is not null.
.globl vdso32_time
vdso32_time:
    call vdso32_page
    movl $D_COARSE_REAL_SEC, %eax
    call vdso32_coarse
    movl 4(%esp), %edx
    testl %edx, %edx
    je 1f
    movl %eax, (%edx)
1:
    ret
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
    cmpl $VCLOCK_NONE, D_MODE(%ecx)
    je 9f
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
