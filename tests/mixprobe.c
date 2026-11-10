/**
 * mixprobe: calls between an i386 and an x86-64 program on the x86-64
 * kernel, which must give each the other as Linux does.
 *
 * The x86-64 build, /mixprobe, is the driver. It starts the i686 build,
 * which the x86-64 test image stages at /ia32/mixprobe, in a role that its
 * first argument names, and judges the role by its exit status and by what
 * it sees itself. Each role is a few calls, made in either build:
 *
 * An execve of the i386 program must run it in 32-bit mode, CS 0x23, with
 * the argument and environment vectors whole, and the i386 execve and
 * execveat, the second through a descriptor of the root, must run the
 * x86-64 program so, in CS 0x33. A descriptor that SCM_RIGHTS passes over a
 * UNIX socket from the i386 program must reach the x86-64 one, its control
 * message in x86-64's 8-byte layout, and one passed the other way must reach
 * the i386 program in i386's 4-byte layout, msg_controllen 16; each must
 * read the pipe it names. A futex word of a memfd page that both map shared
 * must be one futex for both: an x86-64 FUTEX_WAKE must end the i386
 * program's FUTEX_WAIT, with its 32-bit timespec, and an i386 futex_time64
 * wake must end the x86-64 program's wait; the i386 waits must end with
 * ETIMEDOUT at their deadlines, futex_time64's taking the low half of its
 * 64-bit tv_nsec, as Linux's compat path does.
 *
 * An x86-64 tracer must see an i386 tracee stop at its exec with CS 0x23
 * and its EIP in the i386 text, and at its getpid with orig_rax 20 and
 * AUDIT_ARCH_I386 from PTRACE_GET_SYSCALL_INFO; an i386 tracer, whose
 * tracee clone3 makes, must see an x86-64 tracee in i386's 17-word
 * registers, CS 0x33 and EIP the low half of RIP, and at its getpid
 * orig_eax 39 and AUDIT_ARCH_X86_64. A seccomp filter that an x86-64
 * process installs must judge the i386 program it execs by i386 numbers,
 * and one an i386 process installs, in a child that i386 clone makes, by
 * x86-64 numbers in the x86-64 program it execs: each returns its own errno
 * for getpid of the other ABI. A signal that rt_sigqueueinfo sends from
 * either program must reach an SA_SIGINFO handler of the other with the
 * sender's pid, SI_QUEUE and its value, each siginfo in its reader's layout.
 *
 * The driver writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. A role writes no record. Prints "MIXPROBE OK" and exits 0, or
 * "MIXPROBE FAIL: " and the reason and exits 1. The i686 build run with no
 * role, as on the i686 kernel, has no driver and fails.
 */

#include "probe.h"
#include "probe_report.h"

#if defined __x86_64__
#define SYS_RT_SIGQUEUEINFO 129
#define SYS_RT_SIGSUSPEND   130
#define SYS_PRCTL           157
#define SYS_SECCOMP         317
#define SYS_MEMFD_CREATE    319
#define SYS_EXECVEAT        322
#define SYS_CLONE3          435
#define OWN_ARCH            "x86_64"
#define OWN_CS              0x33
#define OTHER_GETPID        20 /* i386's, which a filter of this ABI judges */
#define AUDIT_ARCH_OWN      AUDIT_ARCH_X86_64
#define AUDIT_ARCH_OTHER    AUDIT_ARCH_I386
/* siginfo's si_pid, si_uid and si_value, after a 4-byte hole. */
#define SI_PID              16
#define SI_UID              20
#define SI_VALUE            24
#else
#define SYS_RT_SIGQUEUEINFO 178
#define SYS_RT_SIGSUSPEND   179
#define SYS_PRCTL           172
#define SYS_SECCOMP         354
#define SYS_MEMFD_CREATE    356
#define SYS_EXECVEAT        358
#define SYS_FUTEX_TIME64    422
#define SYS_CLONE3          435
#define OWN_ARCH            "i386"
#define OWN_CS              0x23
#define OTHER_GETPID        39 /* x86-64's */
#define AUDIT_ARCH_OWN      AUDIT_ARCH_I386
#define AUDIT_ARCH_OTHER    AUDIT_ARCH_X86_64
#define SI_PID              12
#define SI_UID              16
#define SI_VALUE            20
#endif

#define I386_PATH   "/ia32/mixprobe"
#define X86_64_PATH "/mixprobe"

#define EINTR     4
#define ETIMEDOUT 110

#define O_RDONLY    0
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

#define SIGUSR1     10
#define SIGTRAP     5
#define SIGCHLD     17
#define SIG_BLOCK   0
#define SIG_SETMASK 2
#define SA_SIGINFO  0x00000004ul
#define SA_RESTORER 0x04000000ul
#define SI_QUEUE    (-1)

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x01
#define PAGE       4096

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define AF_UNIX      1
#define SOCK_STREAM  1
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET   1
#define SCM_RIGHTS   1

#define PR_SET_NO_NEW_PRIVS     38
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_RET_ERRNO       0x00050000u
#define SECCOMP_RET_ALLOW       0x7fff0000u
#define AUDIT_ARCH_I386         0x40000003u
#define AUDIT_ARCH_X86_64       0xC000003Eu
#define BPF_LD_W_ABS            0x20
#define BPF_JEQ_K               0x15
#define BPF_RET_K               0x06
#define SECCOMP_DATA_NR         0
#define SECCOMP_DATA_ARCH       4

#define PTRACE_TRACEME            0
#define PTRACE_PEEKUSER           3
#define PTRACE_CONT               7
#define PTRACE_KILL               8
#define PTRACE_GETREGS            12
#define PTRACE_SYSCALL            24
#define PTRACE_SETOPTIONS         0x4200
#define PTRACE_O_TRACESYSGOOD     1
#define PTRACE_GET_SYSCALL_INFO   0x420E
#define PTRACE_SYSCALL_INFO_ENTRY 1

/* The words of x86-64's user_regs_struct and i386's that the tracers read:
 * orig_rax, rip and cs; orig_eax, eip and xcs. */
#define REG64_ORIG_RAX 15
#define REG64_RIP      16
#define REG64_CS       17
#define REG64_COUNT    27
#define REG32_ORIG_EAX 11
#define REG32_EIP      12
#define REG32_CS       13
#define REG32_COUNT    17

/* Where each build's text lies: its link address and the next 16 MiB. */
#define I386_TEXT   0x08048000ul
#define X86_64_TEXT 0x400000ul
#define TEXT_SPAN   0x1000000ul

/* How many times a waiter looks before it gives up on the other side. */
#define SPIN 200000

/* The i386 errno and the x86-64 errno the two filters return. */
#define ERRNO_OF_X86_64_FILTER 77
#define ERRNO_OF_I386_FILTER   78

/* The value each sender's signal carries. */
#define VALUE_TO_I386   0x1234
#define VALUE_TO_X86_64 0x5678

typedef unsigned int u32;

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT_GROUP, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether the `n` bytes at `a` and `b` are equal. */
static int same_bytes(const char *a, const char *b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/** Set the `n` bytes at `p` to 0. */
static void zero(void *p, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        ((char *)p)[i] = 0;
    }
}

/** The decimal number `s`, which must be one, or -1. */
static long number(const char *s)
{
    long v = 0;

    if (s == 0 || *s == '\0') {
        return -1;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        v = v * 10 + (*s - '0');
    }
    return v;
}

/** Write `s` to standard output. */
static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (long)strlen(s));
}

/** The CS this program runs in. */
static unsigned code_segment(void)
{
    unsigned short cs;

    __asm__ __volatile__("movw %%cs, %0" : "=r"(cs));
    return cs;
}

/** waitpid of `pid`; its exit status, 128 and the signal when a signal
 * ended it, or -1 when it cannot be reaped. */
static int reap(long pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return -1;
    }
    if ((status & 0x7F) == 0) {
        return (status >> 8) & 0xFF;
    }
    return 128 + (status & 0x7F);
}

/** Write `n`, which is not negative, in decimal into `buf` of 12 bytes. */
static char *decimal(char *buf, long n)
{
    return report_decimal(buf, (int)n);
}

/* A classic BPF instruction, and the program seccomp takes, in each ABI's
 * own layout. */
struct sock_filter {
    unsigned short code;
    unsigned char jt;
    unsigned char jf;
    unsigned int k;
};

struct sock_fprog {
    unsigned short len;
    struct sock_filter *filter;
};

struct k_sigaction {
    void (*handler)(int, void *, void *);
    unsigned long flags;
    void (*restorer)(void);
    u32 mask[2];
};

struct timespec {
    long sec;
    long nsec;
};

struct iovec {
    void *base;
    unsigned long len;
};

struct msghdr {
    void *name;
    unsigned int namelen;
    struct iovec *iov;
    unsigned long iovlen;
    void *control;
    unsigned long controllen;
    int flags;
};

/* struct cmsghdr: cmsg_len, a size_t, then the level and type; the data
 * follows at CMSG_HDR, the header's size, 16 bytes on x86-64 and 12 on
 * i386. */
#define CMSG_HDR (sizeof(unsigned long) + 8)

/** The filter that returns `err` for getpid of the other ABI, by that ABI's
 * number, and allows every other call. */
static long install_filter(long err)
{
    static struct sock_filter code[6];
    struct sock_fprog prog;

    code[0].code = BPF_LD_W_ABS;
    code[0].k = SECCOMP_DATA_ARCH;
    code[1].code = BPF_JEQ_K;
    code[1].jf = 3;
    code[1].k = AUDIT_ARCH_OTHER;
    code[2].code = BPF_LD_W_ABS;
    code[2].k = SECCOMP_DATA_NR;
    code[3].code = BPF_JEQ_K;
    code[3].jf = 1;
    code[3].k = OTHER_GETPID;
    code[4].code = BPF_RET_K;
    code[4].k = SECCOMP_RET_ERRNO | (unsigned)err;
    code[5].code = BPF_RET_K;
    code[5].k = SECCOMP_RET_ALLOW;
    prog.len = 6;
    prog.filter = code;
    if (syscall5(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }
    return syscall3(SYS_SECCOMP, SECCOMP_SET_MODE_FILTER, 0, (long)&prog);
}

/* What the SIGUSR1 handler found in its siginfo. */
static volatile long got_signo, got_code, got_pid, got_value;

/** The SA_SIGINFO handler of SIGUSR1: records the siginfo, read in this
 * ABI's layout. */
static void on_usr1(int sig, void *info, void *uc)
{
    const unsigned char *si = info;

    (void)sig;
    (void)uc;
    got_signo = *(const int *)si;
    got_code = *(const int *)(si + 8);
    got_pid = *(const int *)(si + SI_PID);
    got_value = *(const int *)(si + SI_VALUE);
}

void mix_restorer(void);
PROBE_RT_RESTORER(mix_restorer);

/** Install `on_usr1` for SIGUSR1 and block it; 0, or a negated errno. */
static long catch_usr1(u32 *old)
{
    struct k_sigaction sa;
    u32 set[2];

    zero(&sa, sizeof(sa));
    sa.handler = on_usr1;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = mix_restorer;
    if (syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8) != 0) {
        return -1;
    }
    set[0] = 1u << (SIGUSR1 - 1);
    set[1] = 0;
    return syscall4(SYS_RT_SIGPROCMASK, SIG_BLOCK, (long)set, (long)old, 8);
}

/** Wait with every signal unblocked until a handler has run; the result of
 * rt_sigsuspend, -EINTR when one did. */
static long suspend(void)
{
    u32 none[2];

    none[0] = none[1] = 0;
    return syscall2(SYS_RT_SIGSUSPEND, (long)none, 8);
}

/** Queue SIGUSR1 with `value` to `pid` by rt_sigqueueinfo, with this
 * process's pid and SI_QUEUE in a siginfo of this ABI's layout. */
static long queue_usr1(long pid, int value)
{
    unsigned char info[128];

    zero(info, sizeof(info));
    *(int *)info = SIGUSR1;
    *(int *)(info + 8) = SI_QUEUE;
    *(int *)(info + SI_PID) = (int)syscall0(SYS_GETPID);
    *(int *)(info + SI_UID) = (int)syscall0(SYS_GETUID);
    *(int *)(info + SI_VALUE) = value;
    return syscall3(SYS_RT_SIGQUEUEINFO, pid, SIGUSR1, (long)info);
}

/*
 * The roles, each a program of either build that the other starts. Each
 * returns the status the program exits with: 0 when every call gave what
 * Linux gives, and a number of its own for the first that did not.
 */

/** `argv abc`: the vectors as the exec gave them, MIXPROBE naming this
 * build's architecture, and this build's CS. */
static int role_argv(int argc, char **argv, char **envp)
{
    static const char want[] = "MIXPROBE=" OWN_ARCH;

    if (argc != 3 || !same(argv[2], "abc")) {
        return 3;
    }
    if (envp[0] == 0 || !same(envp[0], want) || envp[1] != 0) {
        return 4;
    }
    return code_segment() == OWN_CS ? 0 : 5;
}

/** `getpid N`: getpid gives the pid when N is 0, and the errno N that a
 * filter returns otherwise. */
static int role_getpid(int argc, char **argv)
{
    long want = argc == 3 ? number(argv[2]) : -1;
    long r = syscall0(SYS_GETPID);

    if (want < 0) {
        return 3;
    }
    return (want == 0 ? r > 0 : r == -want) ? 0 : 4;
}

/**
 * Receive one descriptor over the socket `sock` and read `want`, `n` bytes,
 * from it: the control message must be this ABI's, its length CMSG_LEN(4)
 * and msg_controllen CMSG_SPACE(4). 0, or 3 to 6 for the first that was
 * not so.
 */
static int recv_fd(long sock, const char *want, int n)
{
    unsigned long control[8];
    unsigned char *c = (unsigned char *)control;
    unsigned long space = (CMSG_HDR + 4 + sizeof(unsigned long) - 1) & ~(sizeof(unsigned long) - 1);
    char byte, text[16];
    struct iovec iov;
    struct msghdr msg;
    int fd;

    zero(control, sizeof(control));
    iov.base = &byte;
    iov.len = 1;
    zero(&msg, sizeof(msg));
    msg.iov = &iov;
    msg.iovlen = 1;
    msg.control = control;
    msg.controllen = sizeof(control);
    if (syscall3(SYS_RECVMSG, sock, (long)&msg, 0) != 1) {
        return 3;
    }
    if (msg.controllen != space || *(unsigned long *)c != CMSG_HDR + 4 ||
        *(int *)(c + sizeof(unsigned long)) != SOL_SOCKET ||
        *(int *)(c + sizeof(unsigned long) + 4) != SCM_RIGHTS) {
        return 4;
    }
    fd = *(int *)(c + CMSG_HDR);
    if (fd < 0 || syscall3(SYS_READ, fd, (long)text, n) != n || !same_bytes(text, want, n)) {
        return 5;
    }
    syscall1(SYS_CLOSE, fd);
    return 0;
}

/** `recvfd SOCK`: receive a descriptor from the x86-64 program and read
 * "from-x86-64" from it. */
static int role_recvfd(int argc, char **argv)
{
    long sock = argc == 3 ? number(argv[2]) : -1;

    return sock < 0 ? 6 : recv_fd(sock, "from-x86-64", 11);
}

#if !defined __x86_64__
/** `hop execve` or `hop execveat`: exec the x86-64 build's `argv` role by
 * the i386 call named, execveat through a descriptor of the root. */
static int role_hop(int argc, char **argv, char **envp)
{
    static char *next[] = {"mixprobe", "argv", "abc", 0};
    long dir;

    if (argc != 3) {
        return 3;
    }
    if (same(argv[2], "execve")) {
        syscall3(SYS_EXECVE, (long)X86_64_PATH, (long)next, (long)envp);
        return 4;
    }
    dir = syscall3(SYS_OPEN, (long)"/", O_RDONLY | O_DIRECTORY, 0);
    if (dir < 0) {
        return 5;
    }
    syscall5(SYS_EXECVEAT, dir, (long)(X86_64_PATH + 1), (long)next, (long)envp, 0);
    return 6;
}

/** `sendfd SOCK`: send the read end of a pipe that holds "from-i386" over
 * the socket SOCK with SCM_RIGHTS. */
static int role_sendfd(int argc, char **argv)
{
    long sock = argc == 3 ? number(argv[2]) : -1;
    unsigned long control[4];
    unsigned char *c = (unsigned char *)control;
    struct iovec iov;
    struct msghdr msg;
    int p[2];

    if (sock < 0 || syscall1(SYS_PIPE, (long)p) != 0) {
        return 3;
    }
    if (syscall3(SYS_WRITE, p[1], (long)"from-i386", 9) != 9) {
        return 4;
    }
    zero(control, sizeof(control));
    *(unsigned long *)c = CMSG_HDR + 4;
    *(int *)(c + sizeof(unsigned long)) = SOL_SOCKET;
    *(int *)(c + sizeof(unsigned long) + 4) = SCM_RIGHTS;
    *(int *)(c + CMSG_HDR) = p[0];
    iov.base = "m";
    iov.len = 1;
    zero(&msg, sizeof(msg));
    msg.iov = &iov;
    msg.iovlen = 1;
    msg.control = control;
    msg.controllen = (CMSG_HDR + 4 + sizeof(unsigned long) - 1) & ~(sizeof(unsigned long) - 1);
    return syscall3(SYS_SENDMSG, sock, (long)&msg, 0) == 1 ? 0 : 5;
}

/**
 * `futex FD`: map the memfd FD shared and, on its words 0 to 3: say it is
 * ready in word 1 and sleep on word 0 until the x86-64 program wakes it,
 * then wake that program's sleep on word 2 by futex_time64, then let a
 * wait on word 3 by each call reach its deadline.
 */
static int role_futex(int argc, char **argv)
{
    long fd = argc == 3 ? number(argv[2]) : -1;
    struct timespec ts;
    u32 ts64[4];
    volatile u32 *w;
    long r;
    int i;

    w = (volatile u32 *)probe_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fd < 0 || sys_failed((long)w)) {
        return 3;
    }
    w[1] = 1;
    syscall3(SYS_FUTEX, (long)(w + 1), FUTEX_WAKE, 1);
    ts.sec = 10;
    ts.nsec = 0;
    if (syscall4(SYS_FUTEX, (long)w, FUTEX_WAIT, 0, (long)&ts) != 0) {
        return 4;
    }
    for (i = 0; i < SPIN; i++) {
        r = syscall4(SYS_FUTEX_TIME64, (long)(w + 2), FUTEX_WAKE, 1, 0);
        if (r == 1) {
            break;
        }
        syscall0(SYS_SCHED_YIELD);
    }
    if (i == SPIN) {
        return 5;
    }
    ts.sec = 0;
    ts.nsec = 20000000;
    if (syscall4(SYS_FUTEX, (long)(w + 3), FUTEX_WAIT, 0, (long)&ts) != -ETIMEDOUT) {
        return 6;
    }
    /* The upper half of an i386 tv_nsec is padding, which Linux clears. */
    ts64[0] = 0;
    ts64[1] = 0;
    ts64[2] = 20000000;
    ts64[3] = 0xFFFFFFFFu;
    if (syscall4(SYS_FUTEX_TIME64, (long)(w + 3), FUTEX_WAIT, 0, (long)ts64) != -ETIMEDOUT) {
        return 7;
    }
    return 0;
}

/** The i386 tracer's PTRACE_PEEKUSER of the word at `off`, or 0x5A5A. */
static u32 peek32(long pid, long off)
{
    u32 word = 0x5A5A;

    if (syscall4(SYS_PTRACE, PTRACE_PEEKUSER, pid, off, (long)&word) != 0) {
        return 0x5A5A;
    }
    return word;
}

/**
 * `tracer`: make a child by clone3 that traces itself and execs the x86-64
 * build's `getpid 0`, and read it as an i386 tracer: CS 0x33 and its RIP's
 * low half in i386's registers at the exec's stop, and its getpid by
 * PTRACE_GET_SYSCALL_INFO and orig_eax.
 */
static int role_tracer(char **envp)
{
    static char *next[] = {"mixprobe", "getpid", "0", 0};
    unsigned long long args[8];
    u32 regs[REG32_COUNT];
    unsigned char info[88];
    int status = 0, saw = 0, i;
    long pid;

    zero(args, sizeof(args));
    args[4] = SIGCHLD;
    pid = syscall2(SYS_CLONE3, (long)args, sizeof(args));
    if (pid < 0) {
        return 3;
    }
    if (pid == 0) {
        syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0);
        syscall3(SYS_EXECVE, (long)X86_64_PATH, (long)next, (long)envp);
        sys_exit(127);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F ||
        ((status >> 8) & 0xFF) != SIGTRAP) {
        return 4;
    }
    if (syscall4(SYS_PTRACE, PTRACE_GETREGS, pid, 0, (long)regs) != 0 || regs[REG32_CS] != 0x33 ||
        regs[REG32_EIP] < X86_64_TEXT || regs[REG32_EIP] >= X86_64_TEXT + TEXT_SPAN) {
        syscall4(SYS_PTRACE, PTRACE_KILL, pid, 0, 0);
        reap(pid);
        return 5;
    }
    syscall4(SYS_PTRACE, PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);
    for (i = 0; i < 16 && !saw; i++) {
        if (syscall4(SYS_PTRACE, PTRACE_SYSCALL, pid, 0, 0) != 0 ||
            syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F) {
            break;
        }
        if (((status >> 8) & 0xFF) != (SIGTRAP | 0x80) ||
            syscall4(SYS_PTRACE, PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), (long)info) <= 0 ||
            info[0] != PTRACE_SYSCALL_INFO_ENTRY) {
            continue;
        }
        saw = *(u32 *)(info + 4) == AUDIT_ARCH_X86_64 && *(u32 *)(info + 24) == 39 &&
              *(u32 *)(info + 28) == 0 && peek32(pid, REG32_ORIG_EAX * 4) == 39;
    }
    if (!saw) {
        if ((status & 0xFF) == 0x7F) {
            syscall4(SYS_PTRACE, PTRACE_KILL, pid, 0, 0);
        }
        reap(pid);
        return 6;
    }
    if (syscall4(SYS_PTRACE, PTRACE_CONT, pid, 0, 0) != 0) {
        return 7;
    }
    return reap(pid) == 0 ? 0 : 8;
}

/**
 * `filter`: make a child by the i386 clone that installs the filter of
 * ERRNO_OF_I386_FILTER and execs the x86-64 build's getpid role, which must
 * see that errno; the child's status.
 */
static int role_filter(char **envp)
{
    static char *next[] = {"mixprobe", "getpid", "78", 0};
    long pid;
    int status;

    /* i386's clone takes the flags, the stack, the parent's tid pointer, the
     * TLS and the child's tid pointer, in that order. */
    pid = syscall5(SYS_CLONE, SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) {
        return 3;
    }
    if (pid == 0) {
        if (install_filter(ERRNO_OF_I386_FILTER) != 0) {
            sys_exit(4);
        }
        syscall3(SYS_EXECVE, (long)X86_64_PATH, (long)next, (long)envp);
        sys_exit(5);
    }
    status = reap(pid);
    return status < 0 ? 6 : status;
}

/** `sigwait FD`: catch SIGUSR1, say so with a byte on FD, and wait for the
 * x86-64 parent's signal, whose siginfo must carry its pid, SI_QUEUE and
 * VALUE_TO_I386. */
static int role_sigwait(int argc, char **argv)
{
    long fd = argc == 3 ? number(argv[2]) : -1;

    if (fd < 0 || catch_usr1(0) != 0) {
        return 3;
    }
    if (syscall3(SYS_WRITE, fd, (long)"r", 1) != 1) {
        return 4;
    }
    if (suspend() != -EINTR) {
        return 5;
    }
    if (got_signo != SIGUSR1 || got_code != SI_QUEUE || got_pid != syscall0(SYS_GETPPID) ||
        got_value != VALUE_TO_I386) {
        return 6;
    }
    return 0;
}

/** `sigsend`: queue SIGUSR1 with VALUE_TO_X86_64 to the x86-64 parent. */
static int role_sigsend(void)
{
    return queue_usr1(syscall0(SYS_GETPPID), VALUE_TO_X86_64) == 0 ? 0 : 3;
}
#endif

/** Run the role `argv[1]`; its status, or 2 for no such role. */
static int role(int argc, char **argv, char **envp)
{
    const char *name = argv[1];

    if (same(name, "argv")) {
        return role_argv(argc, argv, envp);
    }
    if (same(name, "getpid")) {
        return role_getpid(argc, argv);
    }
    if (same(name, "recvfd")) {
        return role_recvfd(argc, argv);
    }
#if !defined __x86_64__
    if (same(name, "hop")) {
        return role_hop(argc, argv, envp);
    }
    if (same(name, "sendfd")) {
        return role_sendfd(argc, argv);
    }
    if (same(name, "futex")) {
        return role_futex(argc, argv);
    }
    if (same(name, "tracer")) {
        return role_tracer(envp);
    }
    if (same(name, "filter")) {
        return role_filter(envp);
    }
    if (same(name, "sigwait")) {
        return role_sigwait(argc, argv);
    }
    if (same(name, "sigsend")) {
        return role_sigsend();
    }
#endif
    return 2;
}

#if defined __x86_64__
/*
 * The driver, the x86-64 build run with no role.
 */

/* The assertions, a check each, in the order the driver makes them. */
static const char *const assertion_id[] = {
    "exec-to-i386",       "exec-to-x86-64",       "execveat-to-x86-64",    "rights-from-i386",
    "rights-to-i386",     "futex-wake-i386",      "futex-wake-x86-64",     "futex-timeouts-i386",
    "ptrace-i386-tracee", "ptrace-x86-64-tracee", "seccomp-x86-64-filter", "seccomp-i386-filter",
    "signal-to-i386",     "signal-to-x86-64"};

/** Print "MIXPROBE FAIL: " and `why`, end the report at the assertion that
 * `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("MIXPROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    sys_exit(1);
}

/** Fail with `why` and the status `status` of the role. */
static void fail_status(const char *why, int status)
{
    static char line[ETUDE_REPORT_LINE_MAX];
    char digits[12];
    int n = 0;

    report_put(line, &n, why);
    report_put(line, &n, ": status ");
    report_put(line, &n, status < 0 ? "unknown" : decimal(digits, status));
    line[n] = '\0';
    fail(line);
}

/** Fork a child that keeps the descriptor `keep`, closes `drop` (each -1
 * for none) and execs `path` with `argv` and the environment
 * MIXPROBE=`arch`; its pid. */
static long start(const char *path, char **argv, const char *arch, int drop)
{
    static char env[32];
    char *envp[2];
    long pid;
    int n = 0;

    report_put(env, &n, "MIXPROBE=");
    report_put(env, &n, arch);
    env[n] = '\0';
    envp[0] = env;
    envp[1] = 0;
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        if (drop >= 0) {
            syscall1(SYS_CLOSE, drop);
        }
        syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
        sys_exit(127);
    }
    return pid;
}

/** Run the i386 build in the role `argv[1]`, with MIXPROBE=`arch`, and give
 * its status. */
static int run_i386(char **argv, const char *arch)
{
    return reap(start(I386_PATH, argv, arch, -1));
}

/** The execs between the builds. */
static void run_exec(void)
{
    static char *argv_role[] = {"mixprobe", "argv", "abc", 0};
    static char *hop_execve[] = {"mixprobe", "hop", "execve", 0};
    static char *hop_execveat[] = {"mixprobe", "hop", "execveat", 0};
    int status;

    status = run_i386(argv_role, "i386");
    if (status != 0) {
        fail_status("the i386 build exec'd by execve did not run in CS 0x23 with its vectors whole",
                    status);
    }
    report_held();
    status = run_i386(hop_execve, "x86_64");
    if (status != 0) {
        fail_status("the x86-64 build exec'd by the i386 execve did not run in CS 0x33 with its "
                    "vectors whole",
                    status);
    }
    report_held();
    status = run_i386(hop_execveat, "x86_64");
    if (status != 0) {
        fail_status("the x86-64 build exec'd by the i386 execveat through a descriptor of the "
                    "root did not run in CS 0x33 with its vectors whole",
                    status);
    }
    report_held();
}

/** A UNIX stream socket pair, `sv[0]` closed on exec; fails when it cannot
 * be made. */
static void pair(int *sv)
{
    unsigned long args[4];

    args[0] = AF_UNIX;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    args[3] = (unsigned long)sv;
    if (sock_call(SC_SOCKETPAIR, args) != 0) {
        fail("a UNIX socket pair could not be made");
    }
    if (syscall3(SYS_FCNTL, sv[0], 2 /* F_SETFD */, 1 /* FD_CLOEXEC */) != 0) {
        fail("fcntl F_SETFD");
    }
}

/** SCM_RIGHTS from the i386 build to the x86-64 one, and back. */
static void run_rights(void)
{
    static char sock[12];
    static char *sendfd[] = {"mixprobe", "sendfd", sock, 0};
    static char *recvfd[] = {"mixprobe", "recvfd", sock, 0};
    unsigned long control[4];
    unsigned char *c = (unsigned char *)control;
    struct iovec iov;
    struct msghdr msg;
    int sv[2], p[2], r, status;
    long pid;

    pair(sv);
    decimal(sock, sv[1]);
    pid = start(I386_PATH, sendfd, "i386", -1);
    syscall1(SYS_CLOSE, sv[1]);
    r = recv_fd(sv[0], "from-i386", 9);
    status = reap(pid);
    syscall1(SYS_CLOSE, sv[0]);
    if (status != 0) {
        fail_status("the i386 build could not send a descriptor by SCM_RIGHTS", status);
    }
    if (r != 0) {
        fail_status("the descriptor the i386 build sent did not arrive in x86-64's control "
                    "message, or did not read its pipe",
                    r);
    }
    report_held();

    pair(sv);
    decimal(sock, sv[1]);
    if (syscall2(SYS_PIPE2, (long)p, O_CLOEXEC) != 0 ||
        syscall3(SYS_WRITE, p[1], (long)"from-x86-64", 11) != 11) {
        fail("a pipe could not be made and filled");
    }
    pid = start(I386_PATH, recvfd, "i386", -1);
    syscall1(SYS_CLOSE, sv[1]);
    zero(control, sizeof(control));
    *(unsigned long *)c = CMSG_HDR + 4;
    *(int *)(c + 8) = SOL_SOCKET;
    *(int *)(c + 12) = SCM_RIGHTS;
    *(int *)(c + CMSG_HDR) = p[0];
    iov.base = "m";
    iov.len = 1;
    zero(&msg, sizeof(msg));
    msg.iov = &iov;
    msg.iovlen = 1;
    msg.control = control;
    msg.controllen = 24;
    r = (int)syscall3(SYS_SENDMSG, sv[0], (long)&msg, 0);
    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, p[1]);
    status = reap(pid);
    syscall1(SYS_CLOSE, sv[0]);
    if (r != 1) {
        fail("sendmsg with SCM_RIGHTS to the i386 build failed");
    }
    if (status != 0) {
        fail_status("the descriptor sent to the i386 build did not arrive in i386's control "
                    "message, or did not read its pipe",
                    status);
    }
    report_held();
}

/** A futex word shared by the x86-64 driver and the i386 build. */
static void run_futex(void)
{
    static char fdtext[12];
    static char *futex[] = {"mixprobe", "futex", fdtext, 0};
    struct timespec ts;
    volatile u32 *w;
    long fd, pid, r;
    int i, woke, status;

    fd = syscall2(SYS_MEMFD_CREATE, (long)"mixprobe", 0);
    if (fd < 0 || syscall2(SYS_FTRUNCATE, fd, PAGE) != 0) {
        fail("a memfd of a page could not be made");
    }
    w = (volatile u32 *)probe_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sys_failed((long)w)) {
        fail("the memfd could not be mapped shared");
    }
    decimal(fdtext, fd);
    pid = start(I386_PATH, futex, "i386", -1);
    for (i = 0; i < 20 && w[1] == 0; i++) {
        ts.sec = 1;
        ts.nsec = 0;
        syscall4(SYS_FUTEX, (long)(w + 1), FUTEX_WAIT, 0, (long)&ts);
    }
    woke = 0;
    for (i = 0; i < SPIN && w[1] != 0; i++) {
        if (syscall3(SYS_FUTEX, (long)w, FUTEX_WAKE, 1) == 1) {
            woke = 1;
            break;
        }
        syscall0(SYS_SCHED_YIELD);
    }
    ts.sec = 10;
    ts.nsec = 0;
    r = woke ? syscall4(SYS_FUTEX, (long)(w + 2), FUTEX_WAIT, 0, (long)&ts) : -1;
    if (!woke) {
        syscall2(SYS_KILL, pid, 9);
    }
    status = reap(pid);
    syscall2(SYS_MUNMAP, (long)w, PAGE);
    syscall1(SYS_CLOSE, fd);
    if (!woke || status == 3 || status == 4) {
        fail_status("an x86-64 FUTEX_WAKE did not end the i386 FUTEX_WAIT on the shared word",
                    status);
    }
    report_held();
    if (r != 0 || status == 5) {
        fail_status("the i386 futex_time64 wake did not end the x86-64 wait on the shared word",
                    status);
    }
    report_held();
    if (status != 0) {
        fail_status("an i386 futex or futex_time64 wait did not end with ETIMEDOUT at its "
                    "deadline",
                    status);
    }
    report_held();
}

/** The x86-64 tracer's PTRACE_PEEKUSER of the word at `off`, or 0x5A5A. */
static long peek64(long pid, long off)
{
    long word = 0x5A5A;

    if (syscall4(SYS_PTRACE, PTRACE_PEEKUSER, pid, off, (long)&word) != 0) {
        return 0x5A5A;
    }
    return word;
}

/** An x86-64 tracer of the i386 build, and an i386 tracer of the x86-64
 * build. */
static void run_ptrace(void)
{
    static char *getpid_role[] = {"mixprobe", "getpid", "0", 0};
    static char *tracer[] = {"mixprobe", "tracer", 0};
    static char *envp[] = {"MIXPROBE=i386", 0};
    unsigned long regs[REG64_COUNT];
    unsigned char info[88];
    int status = 0, saw = 0, i;
    long pid;

    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0);
        syscall3(SYS_EXECVE, (long)I386_PATH, (long)getpid_role, (long)envp);
        sys_exit(127);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F ||
        ((status >> 8) & 0xFF) != SIGTRAP) {
        fail("the traced i386 build did not stop at its exec with SIGTRAP");
    }
    if (syscall4(SYS_PTRACE, PTRACE_GETREGS, pid, 0, (long)regs) != 0 || regs[REG64_CS] != 0x23 ||
        regs[REG64_RIP] < I386_TEXT || regs[REG64_RIP] >= I386_TEXT + TEXT_SPAN) {
        syscall4(SYS_PTRACE, PTRACE_KILL, pid, 0, 0);
        reap(pid);
        fail("the x86-64 tracer did not read CS 0x23 and an RIP in the i386 text at the exec's "
             "stop");
    }
    syscall4(SYS_PTRACE, PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);
    for (i = 0; i < 16 && !saw; i++) {
        if (syscall4(SYS_PTRACE, PTRACE_SYSCALL, pid, 0, 0) != 0 ||
            syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F) {
            break;
        }
        if (((status >> 8) & 0xFF) != (SIGTRAP | 0x80) ||
            syscall4(SYS_PTRACE, PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), (long)info) <= 0 ||
            info[0] != PTRACE_SYSCALL_INFO_ENTRY) {
            continue;
        }
        saw = *(u32 *)(info + 4) == AUDIT_ARCH_I386 && *(unsigned long *)(info + 24) == 20 &&
              peek64(pid, REG64_ORIG_RAX * 8) == 20;
    }
    if (!saw) {
        if ((status & 0xFF) == 0x7F) {
            syscall4(SYS_PTRACE, PTRACE_KILL, pid, 0, 0);
        }
        reap(pid);
        fail("the x86-64 tracer did not see the i386 getpid as AUDIT_ARCH_I386 with orig_rax 20");
    }
    if (syscall4(SYS_PTRACE, PTRACE_CONT, pid, 0, 0) != 0 || reap(pid) != 0) {
        fail("the traced i386 build did not run to its end");
    }
    report_held();
    status = run_i386(tracer, "i386");
    if (status != 0) {
        fail_status("the i386 tracer did not read the x86-64 build as Linux gives it to an i386 "
                    "tracer",
                    status);
    }
    report_held();
}

/** A seccomp filter of each ABI judging a program of the other. */
static void run_seccomp(void)
{
    static char *getpid_role[] = {"mixprobe", "getpid", "77", 0};
    static char *filter[] = {"mixprobe", "filter", 0};
    static char *envp[] = {"MIXPROBE=i386", 0};
    long pid;
    int status;

    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        if (install_filter(ERRNO_OF_X86_64_FILTER) != 0) {
            sys_exit(9);
        }
        syscall3(SYS_EXECVE, (long)I386_PATH, (long)getpid_role, (long)envp);
        sys_exit(10);
    }
    status = reap(pid);
    if (status != 0) {
        fail_status("an x86-64 filter did not return its errno for the i386 getpid of the "
                    "program exec'd under it",
                    status);
    }
    report_held();
    status = run_i386(filter, "i386");
    if (status != 0) {
        fail_status("an i386 filter did not return its errno for the x86-64 getpid of the "
                    "program exec'd under it",
                    status);
    }
    report_held();
}

/** A queued signal from each build to the other. */
static void run_signal(void)
{
    static char fdtext[12];
    static char *sigwait[] = {"mixprobe", "sigwait", fdtext, 0};
    static char *sigsend[] = {"mixprobe", "sigsend", 0};
    u32 old[2];
    char byte;
    int p[2], status;
    long pid, r;

    if (syscall1(SYS_PIPE, (long)p) != 0) {
        fail("pipe");
    }
    decimal(fdtext, p[1]);
    pid = start(I386_PATH, sigwait, "i386", p[0]);
    syscall1(SYS_CLOSE, p[1]);
    r = syscall3(SYS_READ, p[0], (long)&byte, 1);
    syscall1(SYS_CLOSE, p[0]);
    if (r == 1) {
        r = queue_usr1(pid, VALUE_TO_I386);
    }
    if (r != 0) {
        syscall2(SYS_KILL, pid, 9);
    }
    status = reap(pid);
    if (r != 0 || status != 0) {
        fail_status("a signal queued by the x86-64 build did not reach the i386 handler with its "
                    "sender, SI_QUEUE and value",
                    status);
    }
    report_held();

    if (catch_usr1(old) != 0) {
        fail("SIGUSR1 could not be caught and blocked");
    }
    got_signo = 0;
    pid = start(I386_PATH, sigsend, "i386", -1);
    status = reap(pid);
    r = status == 0 ? suspend() : 0;
    syscall4(SYS_RT_SIGPROCMASK, SIG_SETMASK, (long)old, 0, 8);
    if (status != 0) {
        fail_status("the i386 rt_sigqueueinfo to the x86-64 build failed", status);
    }
    if (r != -EINTR || got_signo != SIGUSR1 || got_code != SI_QUEUE || got_pid != pid ||
        got_value != VALUE_TO_X86_64) {
        fail("a signal queued by the i386 build did not reach the x86-64 handler with its "
             "sender, SI_QUEUE and value");
    }
    report_held();
}
#endif

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** A role when an argument names one, and otherwise the driver. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
#if defined __x86_64__
    unsigned int i;
#endif

    if (argc >= 2) {
        sys_exit(role(argc, argv, envp));
    }
#if defined __x86_64__
    report_begin(report_case(envp, "guest.ia32.mixprobe"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_exec();
    run_rights();
    run_futex();
    run_ptrace();
    run_seccomp();
    run_signal();
    report_end();
    say("MIXPROBE OK\n");
    sys_exit(0);
#else
    say("MIXPROBE FAIL: the driver is the x86-64 build; run /mixprobe on the x86-64 kernel\n");
    sys_exit(1);
#endif
}
