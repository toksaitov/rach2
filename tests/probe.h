#ifndef PROBE_H
#define PROBE_H

/*
 * probe.h: the system call interface of the freestanding probes, for the
 * architecture tests/Makefile builds them for.
 *
 * A probe makes its system calls itself, with no C library between it and
 * the kernel, so that what it tests is the kernel's interface and nothing
 * else. syscall0 to syscall6 make call `n` with up to six arguments, each a
 * long, and return the kernel's result, a negated errno on failure. On i386
 * they enter the kernel through `int $0x80`, with the number in EAX and the
 * arguments in EBX, ECX, EDX, ESI, EDI and EBP. On x86-64 they enter through
 * `syscall`, with the number in RAX and the arguments in RDI, RSI, RDX, R10,
 * R8 and R9, and the instruction overwrites RCX with the return address and
 * R11 with the flags; a wrapper of fewer arguments sets the others to zero,
 * so that wait4 made with three arguments is waitpid.
 *
 * SYS_ names each call by its number on the architecture. A call whose
 * arguments are longs and pointers has the same form on both, and its name
 * is the same: SYS_LSEEK is lseek, with a 32-bit offset on i386 and a 64-bit
 * one on x86-64. Where i386 added a call of wider ids or a wider struct
 * beside an older one, as stat64 beside stat and setuid32 beside setuid, the
 * name is the wider call's, which is the one x86-64 has: SYS_STAT is stat64
 * on i386 and stat on x86-64, and SYS_SETUID is setuid32 and setuid.
 * SYS_SELECT is i386's _newselect, and SYS_WAITPID is wait4 on x86-64, which
 * has no waitpid. The calls only i386 has, and the narrower calls of the old
 * names, have names of their own (SYS_OLDSTAT, SYS_LLSEEK, SYS_MMAP2,
 * SYS_SOCKETCALL, SYS_SET_THREAD_AREA and the like) defined for i386 alone,
 * so a probe or a row that makes one is built for i686 alone; arch_prctl,
 * which x86-64 alone has, is SYS_ARCH_PRCTL, defined for it alone.
 *
 * A probe that reads a struct the kernel writes reads it by the offsets
 * below, which are Linux's for the architecture: KSTAT_ for the stat that
 * SYS_STAT, SYS_LSTAT, SYS_FSTAT and SYS_FSTATAT fill, i386's stat64 or
 * x86-64's stat. The other structs a probe passes, timespec, timeval,
 * sigaction, sysinfo and the rest, are declared with longs and pointers,
 * which give Linux's layout on both.
 */

typedef __SIZE_TYPE__ size_t;

#if defined __x86_64__

#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_LSTAT           6
#define SYS_POLL            7
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_RT_SIGRETURN    15
#define SYS_IOCTL           16
#define SYS_READV           19
#define SYS_WRITEV          20
#define SYS_ACCESS          21
#define SYS_PIPE            22
#define SYS_SELECT          23
#define SYS_SCHED_YIELD     24
#define SYS_MREMAP          25
#define SYS_DUP             32
#define SYS_DUP2            33
#define SYS_PAUSE           34
#define SYS_NANOSLEEP       35
#define SYS_GETITIMER       36
#define SYS_ALARM           37
#define SYS_SETITIMER       38
#define SYS_GETPID          39
#define SYS_SOCKET          41
#define SYS_CONNECT         42
#define SYS_ACCEPT          43
#define SYS_SENDTO          44
#define SYS_RECVFROM        45
#define SYS_SENDMSG         46
#define SYS_RECVMSG         47
#define SYS_SHUTDOWN        48
#define SYS_BIND            49
#define SYS_LISTEN          50
#define SYS_GETSOCKNAME     51
#define SYS_GETPEERNAME     52
#define SYS_SOCKETPAIR      53
#define SYS_SETSOCKOPT      54
#define SYS_GETSOCKOPT      55
#define SYS_CLONE           56
#define SYS_FORK            57
#define SYS_VFORK           58
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_WAIT4           61
#define SYS_WAITPID         61
#define SYS_KILL            62
#define SYS_UNAME           63
#define SYS_FCNTL           72
#define SYS_FLOCK           73
#define SYS_FSYNC           74
#define SYS_TRUNCATE        76
#define SYS_FTRUNCATE       77
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_FCHDIR          81
#define SYS_RENAME          82
#define SYS_MKDIR           83
#define SYS_RMDIR           84
#define SYS_CREAT           85
#define SYS_LINK            86
#define SYS_UNLINK          87
#define SYS_SYMLINK         88
#define SYS_READLINK        89
#define SYS_CHMOD           90
#define SYS_FCHMOD          91
#define SYS_CHOWN           92
#define SYS_FCHOWN          93
#define SYS_LCHOWN          94
#define SYS_UMASK           95
#define SYS_GETTIMEOFDAY    96
#define SYS_GETRLIMIT       97
#define SYS_SYSINFO         99
#define SYS_TIMES           100
#define SYS_PTRACE          101
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_SETUID          105
#define SYS_SETGID          106
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_SETPGID         109
#define SYS_GETPPID         110
#define SYS_GETPGRP         111
#define SYS_SETSID          112
#define SYS_SETREUID        113
#define SYS_SETREGID        114
#define SYS_GETGROUPS       115
#define SYS_SETGROUPS       116
#define SYS_SETRESUID       117
#define SYS_GETRESUID       118
#define SYS_SETRESGID       119
#define SYS_GETRESGID       120
#define SYS_GETPGID         121
#define SYS_GETSID          124
#define SYS_SIGALTSTACK     131
#define SYS_MKNOD           133
#define SYS_STATFS          137
#define SYS_FSTATFS         138
#define SYS_ARCH_PRCTL      158
#define SYS_SETRLIMIT       160
#define SYS_CHROOT          161
#define SYS_SYNC            162
#define SYS_MOUNT           165
#define SYS_UMOUNT2         166
#define SYS_REBOOT          169
#define SYS_GETTID          186
#define SYS_TKILL           200
#define SYS_FUTEX           202
#define SYS_SCHED_SETAFFINITY 203
#define SYS_GETDENTS64      217
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP      231
#define SYS_TGKILL          234
#define SYS_WAITID          247
#define SYS_OPENAT          257
#define SYS_MKDIRAT         258
#define SYS_FCHOWNAT        260
#define SYS_FSTATAT         262
#define SYS_UNLINKAT        263
#define SYS_RENAMEAT        264
#define SYS_LINKAT          265
#define SYS_SYMLINKAT       266
#define SYS_READLINKAT      267
#define SYS_FCHMODAT        268
#define SYS_FACCESSAT       269
#define SYS_PSELECT6        270
#define SYS_PPOLL           271
#define SYS_UTIMENSAT       280
#define SYS_ACCEPT4         288
#define SYS_DUP3            292
#define SYS_PIPE2           293
#define SYS_PRLIMIT64       302
#define SYS_SYNCFS          306
#define SYS_GETRANDOM       318

/* Offsets in x86-64's struct stat, 144 bytes. */
#define KSTAT_SIZE       144
#define KSTAT_DEV        0
#define KSTAT_INO        8
#define KSTAT_NLINK      16
#define KSTAT_MODE       24
#define KSTAT_UID        28
#define KSTAT_GID        32
#define KSTAT_RDEV       40
#define KSTAT_SIZE_AT    48
#define KSTAT_BLKSIZE    56
#define KSTAT_BLOCKS     64
#define KSTAT_ATIME      72
#define KSTAT_MTIME      88
#define KSTAT_CTIME      104
#define KSTAT_ATIME_NSEC 80
#define KSTAT_MTIME_NSEC 96
#define KSTAT_CTIME_NSEC 112

static __inline__ long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;

    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
    return r;
}

static __inline__ long syscall5(long n, long a, long b, long c, long d, long e)
{
    return syscall6(n, a, b, c, d, e, 0);
}

static __inline__ long syscall4(long n, long a, long b, long c, long d)
{
    return syscall6(n, a, b, c, d, 0, 0);
}

static __inline__ long syscall3(long n, long a, long b, long c)
{
    return syscall6(n, a, b, c, 0, 0, 0);
}

static __inline__ long syscall2(long n, long a, long b)
{
    return syscall6(n, a, b, 0, 0, 0, 0);
}

static __inline__ long syscall1(long n, long a)
{
    return syscall6(n, a, 0, 0, 0, 0, 0);
}

static __inline__ long syscall0(long n)
{
    return syscall6(n, 0, 0, 0, 0, 0, 0);
}

/*
 * The entry of a probe that reads its initial stack: the kernel enters
 * `_start` with RSP at argc, aligned to 16 bytes, and the stub passes that
 * address to `probe_main` in RDI with the stack aligned as a call expects.
 */
#define PROBE_SP_ENTRY              \
    __asm__(".text\n"               \
            ".globl _start\n"       \
            "_start:\n"             \
            "    xorq %rbp, %rbp\n" \
            "    movq %rsp, %rdi\n" \
            "    andq $-16, %rsp\n" \
            "    call probe_main\n" \
            "    hlt\n")

/*
 * The attribute of a probe's `_start` written in C: the kernel enters it with
 * RSP aligned to 16 bytes, where a called function finds it 8 bytes past
 * that, so the function realigns its frame first.
 */
#define PROBE_START __attribute__((force_align_arg_pointer, used))

/* A signal restorer for SA_RESTORER, named `name`: rt_sigreturn. */
#define PROBE_RT_RESTORER(name)              \
    __asm__(".text\n"                        \
            ".globl " #name "\n" #name ":\n" \
            "    movq $15, %rax\n"           \
            "    syscall\n")

#else

#define SYS_EXIT            1
#define SYS_FORK            2
#define SYS_READ            3
#define SYS_WRITE           4
#define SYS_OPEN            5
#define SYS_CLOSE           6
#define SYS_WAITPID         7
#define SYS_CREAT           8
#define SYS_LINK            9
#define SYS_UNLINK          10
#define SYS_EXECVE          11
#define SYS_CHDIR           12
#define SYS_MKNOD           14
#define SYS_CHMOD           15
#define SYS_LSEEK           19
#define SYS_GETPID          20
#define SYS_MOUNT           21
#define SYS_PTRACE          26
#define SYS_ALARM           27
#define SYS_PAUSE           29
#define SYS_ACCESS          33
#define SYS_SYNC            36
#define SYS_KILL            37
#define SYS_RENAME          38
#define SYS_MKDIR           39
#define SYS_RMDIR           40
#define SYS_DUP             41
#define SYS_PIPE            42
#define SYS_TIMES           43
#define SYS_BRK             45
#define SYS_UMOUNT2         52
#define SYS_IOCTL           54
#define SYS_SETPGID         57
#define SYS_UMASK           60
#define SYS_CHROOT          61
#define SYS_DUP2            63
#define SYS_GETPPID         64
#define SYS_GETPGRP         65
#define SYS_SETSID          66
#define SYS_GETTIMEOFDAY    78
#define SYS_SYMLINK         83
#define SYS_READLINK        85
#define SYS_REBOOT          88
#define SYS_MUNMAP          91
#define SYS_TRUNCATE        92
#define SYS_FTRUNCATE       93
#define SYS_FCHMOD          94
#define SYS_SETITIMER       104
#define SYS_GETITIMER       105
#define SYS_WAIT4           114
#define SYS_SYSINFO         116
#define SYS_FSYNC           118
#define SYS_CLONE           120
#define SYS_UNAME           122
#define SYS_MPROTECT        125
#define SYS_GETPGID         132
#define SYS_FCHDIR          133
#define SYS_FLOCK           143
#define SYS_SELECT          142
#define SYS_READV           145
#define SYS_WRITEV          146
#define SYS_GETSID          147
#define SYS_SCHED_YIELD     158
#define SYS_NANOSLEEP       162
#define SYS_MREMAP          163
#define SYS_POLL            168
#define SYS_RT_SIGRETURN    173
#define SYS_RT_SIGACTION    174
#define SYS_RT_SIGPROCMASK  175
#define SYS_GETCWD          183
#define SYS_SIGALTSTACK     186
#define SYS_VFORK           190
#define SYS_GETRLIMIT       191
#define SYS_STAT            195
#define SYS_LSTAT           196
#define SYS_FSTAT           197
#define SYS_LCHOWN          198
#define SYS_GETUID          199
#define SYS_GETGID          200
#define SYS_GETEUID         201
#define SYS_GETEGID         202
#define SYS_SETREUID        203
#define SYS_SETREGID        204
#define SYS_GETGROUPS       205
#define SYS_SETGROUPS       206
#define SYS_FCHOWN          207
#define SYS_SETRESUID       208
#define SYS_GETRESUID       209
#define SYS_SETRESGID       210
#define SYS_GETRESGID       211
#define SYS_CHOWN           212
#define SYS_SETUID          213
#define SYS_SETGID          214
#define SYS_GETDENTS64      220
#define SYS_FCNTL           221
#define SYS_GETTID          224
#define SYS_TKILL           238
#define SYS_FUTEX           240
#define SYS_SCHED_SETAFFINITY 241
#define SYS_EXIT_GROUP      252
#define SYS_CLOCK_GETTIME   265
#define SYS_CLOCK_NANOSLEEP 267
#define SYS_TGKILL          270
#define SYS_WAITID          284
#define SYS_OPENAT          295
#define SYS_MKDIRAT         296
#define SYS_FCHOWNAT        298
#define SYS_FSTATAT         300
#define SYS_UNLINKAT        301
#define SYS_RENAMEAT        302
#define SYS_LINKAT          303
#define SYS_SYMLINKAT       304
#define SYS_READLINKAT      305
#define SYS_FCHMODAT        306
#define SYS_FACCESSAT       307
#define SYS_PSELECT6        308
#define SYS_PPOLL           309
#define SYS_UTIMENSAT       320
#define SYS_DUP3            330
#define SYS_PIPE2           331
#define SYS_PRLIMIT64       340
#define SYS_SYNCFS          344
#define SYS_GETRANDOM       355
#define SYS_SOCKET          359
#define SYS_SOCKETPAIR      360
#define SYS_BIND            361
#define SYS_CONNECT         362
#define SYS_LISTEN          363
#define SYS_ACCEPT4         364
#define SYS_GETSOCKOPT      365
#define SYS_SETSOCKOPT      366
#define SYS_GETSOCKNAME     367
#define SYS_GETPEERNAME     368
#define SYS_SENDTO          369
#define SYS_SENDMSG         370
#define SYS_RECVFROM        371
#define SYS_RECVMSG         372
#define SYS_SHUTDOWN        373

/* The calls only i386 has, and the narrower calls of the old names. */
#define SYS_OLDSTAT         106
#define SYS_OLDLSTAT        107
#define SYS_SOCKETCALL      102
#define SYS_LLSEEK          140
#define SYS_MMAP2           192
#define SYS_TRUNCATE64      193
#define SYS_FTRUNCATE64     194
#define SYS_SET_THREAD_AREA 243
#define SYS_STATFS64        268
#define SYS_FSTATFS64       269

/* Offsets in i386's struct stat64, 96 bytes. */
#define KSTAT_SIZE          96
#define KSTAT_DEV           0
#define KSTAT_INO           88
#define KSTAT_NLINK         20
#define KSTAT_MODE          16
#define KSTAT_UID           24
#define KSTAT_GID           28
#define KSTAT_RDEV          32
#define KSTAT_SIZE_AT       44
#define KSTAT_BLKSIZE       52
#define KSTAT_BLOCKS        56
#define KSTAT_ATIME         64
#define KSTAT_MTIME         72
#define KSTAT_CTIME         80
#define KSTAT_ATIME_NSEC    68
#define KSTAT_MTIME_NSEC    76
#define KSTAT_CTIME_NSEC    84

static __inline__ long syscall0(long n)
{
    long r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n) : "memory");
    return r;
}

static __inline__ long syscall1(long n, long a)
{
    long r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static __inline__ long syscall2(long n, long a, long b)
{
    long r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static __inline__ long syscall3(long n, long a, long b, long c)
{
    long r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static __inline__ long syscall4(long n, long a, long b, long c, long d)
{
    long r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static __inline__ long syscall5(long n, long a, long b, long c, long d, long e)
{
    long r;

    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

/*
 * The sixth argument goes in EBP, which the compiler may keep the frame in
 * and gives no constraint: it is saved, loaded from memory, and put back
 * around the call.
 */
static __inline__ long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    long args[2];

    args[0] = f;
    args[1] = n;
    __asm__ __volatile__("pushl %%ebp\n\t"
                         "movl (%%eax), %%ebp\n\t"
                         "movl 4(%%eax), %%eax\n\t"
                         "int $0x80\n\t"
                         "popl %%ebp"
                         : "=a"(r)
                         : "0"(args), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

/*
 * The entry of a probe that reads its initial stack: the kernel enters
 * `_start` with ESP at argc, and the stub passes that address to
 * `probe_main` as its argument on the stack.
 */
#define PROBE_SP_ENTRY             \
    __asm__(".globl _start\n"      \
            "_start:\n"            \
            "    mov %esp, %eax\n" \
            "    push %eax\n"      \
            "    call probe_main\n")

/* The attribute of a probe's `_start` written in C, which i386 enters with
 * the alignment it needs. */
#define PROBE_START

/* A signal restorer for SA_RESTORER, named `name`: rt_sigreturn. */
#define PROBE_RT_RESTORER(name)              \
    __asm__(".globl " #name "\n" #name ":\n" \
            "    movl $173, %eax\n"          \
            "    int $0x80\n")

#endif

/** Whether a system call's result is a negated errno, as an address such as
 * mmap's can be anything else. */
static __inline__ int sys_failed(long r)
{
    return (unsigned long)r > -4096UL;
}

/**
 * mmap(2) at the file offset `off` in bytes, which must be a multiple of the
 * page: on i386 through mmap2, which takes the offset in pages, and on
 * x86-64 through mmap. The mapping's address, or a negated errno.
 */
static __inline__ long probe_mmap(long addr, long len, long prot, long flags, long fd, long off)
{
#if defined __x86_64__
    return syscall6(SYS_MMAP, addr, len, prot, flags, fd, off);
#else
    return syscall6(SYS_MMAP2, addr, len, prot, flags, fd, (long)((unsigned long)off >> 12));
#endif
}

/*
 * lseek to the 64-bit offset `off` from `whence`, with the offset it reaches
 * in `*result`: on i386 through _llseek, which takes the offset in two words
 * and writes the result, and on x86-64 through lseek, whose offset and result
 * are 64 bits wide. 0, or a negated errno. LLSEEK_NAME names the call in a
 * row's text.
 */
#if defined __x86_64__
#define LLSEEK_NAME "lseek"
#else
#define LLSEEK_NAME "_llseek"
#endif
static __inline__ long probe_llseek(int fd, long long off, int whence, long long *result)
{
#if defined __x86_64__
    long r = syscall3(SYS_LSEEK, fd, (long)off, whence);

    if (r < 0) {
        return r;
    }
    *result = r;
    return 0;
#else
    return syscall5(SYS_LLSEEK, fd, (long)((unsigned long long)off >> 32), (long)off, (long)result,
                    whence);
#endif
}

/**
 * ftruncate to the 64-bit length `len`: on i386 through ftruncate64, which
 * takes the length in two words, the low first, and on x86-64 through
 * ftruncate. 0, or a negated errno.
 */
static __inline__ long probe_ftruncate64(int fd, long long len)
{
#if defined __x86_64__
    return syscall2(SYS_FTRUNCATE, fd, (long)len);
#else
    return syscall3(SYS_FTRUNCATE64, fd, (long)len, (long)((unsigned long long)len >> 32));
#endif
}

/*
 * statfs of `path` into `buf`: on i386 through statfs64, which takes the
 * size of its 84-byte struct, and on x86-64 through statfs, whose struct of
 * longs is 120 bytes. KSTATFS_SIZE is the larger, and f_type, the magic, is
 * the 32-bit word at the start of either; KSTATFS_FILES and KSTATFS_FFREE
 * are the offsets of the 64-bit inode counts. 0, or a negated errno.
 */
#define KSTATFS_SIZE 120
#if defined __x86_64__
#define KSTATFS_FILES 40
#define KSTATFS_FFREE 48
#else
#define KSTATFS_FILES 32
#define KSTATFS_FFREE 40
#endif
static __inline__ long probe_statfs(const char *path, void *buf)
{
#if defined __x86_64__
    return syscall2(SYS_STATFS, (long)path, (long)buf);
#else
    return syscall3(SYS_STATFS64, (long)path, 84, (long)buf);
#endif
}

/* The socket operations of socketcall(2), by its numbers, and Rach2's ICMP
 * echo, SC_PING and SC_PING_DATA, with its call (pingcall.h). */
#include "../utils/pingcall.h"

#define SC_SOCKET      1
#define SC_BIND        2
#define SC_CONNECT     3
#define SC_LISTEN      4
#define SC_ACCEPT      5
#define SC_GETSOCKNAME 6
#define SC_GETPEERNAME 7
#define SC_SOCKETPAIR  8
#define SC_SEND        9
#define SC_RECV        10
#define SC_SENDTO      11
#define SC_RECVFROM    12
#define SC_SHUTDOWN    13
#define SC_SETSOCKOPT  14
#define SC_GETSOCKOPT  15
#define SC_SENDMSG     16
#define SC_RECVMSG     17
#define SC_ACCEPT4     18

/**
 * The socket operation `call` with the arguments `args`, as socketcall(2)
 * takes them: on i386 through socketcall itself, and on x86-64, which has no
 * socketcall, through the call of the operation's own. SC_SEND and SC_RECV
 * are sendto and recvfrom with no address there, SC_ACCEPT is accept4 with
 * no flags, and the echo is Rach2's call for it. The operation's result, or a
 * negated errno.
 */
static __inline__ long sock_call(int call, const unsigned long *args)
{
#if defined __x86_64__
    const long *a = (const long *)args;

    switch (call) {
    case SC_SOCKET:
        return syscall3(SYS_SOCKET, a[0], a[1], a[2]);
    case SC_BIND:
        return syscall3(SYS_BIND, a[0], a[1], a[2]);
    case SC_CONNECT:
        return syscall3(SYS_CONNECT, a[0], a[1], a[2]);
    case SC_LISTEN:
        return syscall2(SYS_LISTEN, a[0], a[1]);
    case SC_ACCEPT:
        return syscall4(SYS_ACCEPT4, a[0], a[1], a[2], 0);
    case SC_GETSOCKNAME:
        return syscall3(SYS_GETSOCKNAME, a[0], a[1], a[2]);
    case SC_GETPEERNAME:
        return syscall3(SYS_GETPEERNAME, a[0], a[1], a[2]);
    case SC_SOCKETPAIR:
        return syscall4(SYS_SOCKETPAIR, a[0], a[1], a[2], a[3]);
    case SC_SEND:
        return syscall6(SYS_SENDTO, a[0], a[1], a[2], a[3], 0, 0);
    case SC_RECV:
        return syscall6(SYS_RECVFROM, a[0], a[1], a[2], a[3], 0, 0);
    case SC_SENDTO:
        return syscall6(SYS_SENDTO, a[0], a[1], a[2], a[3], a[4], a[5]);
    case SC_RECVFROM:
        return syscall6(SYS_RECVFROM, a[0], a[1], a[2], a[3], a[4], a[5]);
    case SC_SHUTDOWN:
        return syscall2(SYS_SHUTDOWN, a[0], a[1]);
    case SC_SETSOCKOPT:
        return syscall5(SYS_SETSOCKOPT, a[0], a[1], a[2], a[3], a[4]);
    case SC_GETSOCKOPT:
        return syscall5(SYS_GETSOCKOPT, a[0], a[1], a[2], a[3], a[4]);
    case SC_SENDMSG:
        return syscall3(SYS_SENDMSG, a[0], a[1], a[2]);
    case SC_RECVMSG:
        return syscall3(SYS_RECVMSG, a[0], a[1], a[2]);
    case SC_ACCEPT4:
        return syscall4(SYS_ACCEPT4, a[0], a[1], a[2], a[3]);
    case SC_PING:
    case SC_PING_DATA:
        return syscall2(SYS_PINGCALL, call, (long)args);
    default:
        return -22; /* EINVAL, as socketcall answers an operation it lacks */
    }
#else
    return syscall2(SYS_SOCKETCALL, call, (long)args);
#endif
}

#endif /* PROBE_H */
