/**
 * The extended floating-point state, proved from user space.
 *
 * `xsaveprobe` checks first what `cpuid` and XCR0 say: OSXSAVE is set
 * exactly where XSAVE is reported, and XGETBV's XCR0 names x87 and SSE, AVX
 * where `cpuid` reports it, AVX-512's three components where `cpuid`
 * reports AVX512F and leaf 0xD lists them, and nothing else. Then, where
 * the kernel enabled AVX, the YMM registers, YMM0 to YMM7 or to YMM15 on
 * x86-64, hold values whose upper halves are not zero, and:
 *
 * * a forked child finds the parent's values, and parent and child each
 *   keep their own across 300 ms of yields, sleeps and preemption;
 * * a signal handler starts with every YMM register zero and finds the
 *   interrupted values in its frame, in Linux's layout: UC_FP_XSTATE in
 *   the ucontext's flags, the image 64-byte aligned, its software bytes
 *   FP_XSTATE_MAGIC1, the sizes and XCR0, FP_XSTATE_MAGIC2 after the
 *   image, and XSTATE_BV marking x87, SSE and AVX in use; it clobbers
 *   every register, and the interrupted code finds its values intact;
 * * a value a handler writes into the frame's AVX part is the register's
 *   after sigreturn;
 * * a frame whose FP_XSTATE_MAGIC1 a handler cleared gives the XMM halves
 *   back and the upper halves zero, as Linux restores such a frame;
 * * a frame whose XSAVE header a handler damaged kills its process with
 *   SIGSEGV;
 * * an exec starts with every YMM register zero;
 * * a tracer reads a stopped child's registers through PTRACE_GETREGSET
 *   with NT_X86_XSTATE, at the offsets leaf 0xD gives, with XCR0 at byte
 *   464, and their XMM halves in the `fxsave` image's set, writes one upper
 *   half through PTRACE_SETREGSET, and the child finds it;
 * * a child that dumps core leaves an NT_X86_XSTATE note named "LINUX" in
 *   its core file with the same image of its registers.
 *
 * Without AVX each of those rows prints "xsaveprobe: ROW: not run: no AVX",
 * which is no failure; without XSAVE, PTRACE_GETREGSET with NT_X86_XSTATE
 * must fail with ENODEV. `xsaveprobe noxsave`, for a boot with
 * `rach2.noxsave` or `rach2.nofxsr`, expects OSXSAVE clear where `cpuid`
 * reports XSAVE.
 *
 * On x86-64 the probe's own code is compiled to use the general registers
 * alone, so that nothing but the rows' assembly touches a vector register.
 *
 * Prints "XSAVEPROBE OK" or "XSAVEPROBE FAIL: reason". Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"

#if defined __x86_64__
#pragma GCC target("general-regs-only")
#define NYMM 16
#else
#define NYMM 8
#endif

#define SIGUSR1     10
#define SIGSEGV     11
#define SIGQUIT     3
#define SIGSTOP     19
#define SA_SIGINFO  0x00000004u
#define SA_RESTORER 0x04000000u
#define ENODEV      19
#define EINVAL      22

#define PTRACE_TRACEME   0
#define PTRACE_CONT      7
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SETREGSET 0x4205
#define NT_X86_XSTATE    0x202
/* The `fxsave` image's register set: NT_PRFPREG on x86-64, NT_PRXFPREG on
 * i386, whose NT_PRFPREG is the x87 part alone. */
#if defined __x86_64__
#define NT_FXSAVE 2
#else
#define NT_FXSAVE 0x46E62B7F
#endif

/* Linux's marks of an XSAVE image in a signal frame. */
#define FP_XSTATE_MAGIC1 0x46505853u
#define FP_XSTATE_MAGIC2 0x46505845u

/* The ucontext's flag that announces an XSAVE image at `fpstate`. */
#define UC_FP_XSTATE 0x1ul

/* Offsets in the XSAVE image: the XMM registers, the software bytes,
 * XSTATE_BV and XCOMP_BV. */
#define X_XMM  160
#define X_SW   464
#define X_BV   512
#define X_COMP 520

/* Where a frame's ucontext has the `fpstate` pointer, the offset of
 * `uc_mcontext` and of `fpstate` in the sigcontext; and the bytes of the
 * frame's state before its image, the i386 x87 part. */
#if defined __x86_64__
#define UC_FPSTATE   (40 + 184)
#define FRAME_PREFIX 0
#else
#define UC_FPSTATE   (20 + 76)
#define FRAME_PREFIX 112
#endif

/* The largest image the probe reads; the kernel keeps no larger. */
#define IMAGE_MAX 4096

/* The directory a child dumps core in, on tmpfs, and its core file. */
#define CORE_DIR    "/dev/shm/xsaveprobe.d"
#define CORE_PATH   CORE_DIR "/core"
#define RLIMIT_CORE 4
#define PT_NOTE     4

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

struct timespec {
    long sec;
    long nsec;
};

struct timeval {
    long sec;
    long usec;
};

struct iovec {
    void *base;
    unsigned long len;
};

/* One YMM register: its XMM half in w[0] to w[3], its upper half in w[4]
 * to w[7]. */
struct ymm {
    unsigned w[8];
};

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (long)strlen(s));
}

static void puts(const char *s)
{
    say(s);
    say("\n");
}

static void fail(const char *why)
{
    say("XSAVEPROBE FAIL: ");
    puts(why);
    sys_exit(1);
}

static void fail2(const char *what, const char *why)
{
    say("XSAVEPROBE FAIL: ");
    say(what);
    say(": ");
    puts(why);
    sys_exit(1);
}

static void not_run(const char *row)
{
    say("xsaveprobe: ");
    say(row);
    puts(": not run: no AVX");
}

static unsigned now_ms(void)
{
    struct timeval tv;

    syscall2(SYS_GETTIMEOFDAY, (long)&tv, 0);
    return (unsigned)tv.sec * 1000u + (unsigned)tv.usec / 1000u;
}

static int getpid(void)
{
    return (int)syscall1(SYS_GETPID, 0);
}

static long ptrace(long request, long pid, long addr, long data)
{
    return syscall4(SYS_PTRACE, request, pid, addr, data);
}

static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    __asm__ __volatile__("cpuid"
                         : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                         : "a"(leaf), "c"(sub));
}

/* What `cpuid` and XCR0 said: whether AVX is usable, XCR0, the bytes of the
 * XSAVE image for it, and the offset of the AVX component in it. */
static int avx_usable;
static unsigned xcr0_lo, xcr0_hi;
static unsigned xstate_size;
static unsigned avx_offset;

#define LOAD(n)  __asm__ __volatile__("vmovdqu %0, %%ymm" #n : : "m"(v[n]))
#define STORE(n) __asm__ __volatile__("vmovdqu %%ymm" #n ", %0" : "=m"(v[n]))

static void ymm_load(const struct ymm *v)
{
    LOAD(0);
    LOAD(1);
    LOAD(2);
    LOAD(3);
    LOAD(4);
    LOAD(5);
    LOAD(6);
    LOAD(7);
#if defined __x86_64__
    LOAD(8);
    LOAD(9);
    LOAD(10);
    LOAD(11);
    LOAD(12);
    LOAD(13);
    LOAD(14);
    LOAD(15);
#endif
}

static void ymm_store(struct ymm *v)
{
    STORE(0);
    STORE(1);
    STORE(2);
    STORE(3);
    STORE(4);
    STORE(5);
    STORE(6);
    STORE(7);
#if defined __x86_64__
    STORE(8);
    STORE(9);
    STORE(10);
    STORE(11);
    STORE(12);
    STORE(13);
    STORE(14);
    STORE(15);
#endif
}

/* Values derived from a seed, every word of them not zero. */
static void make_values(struct ymm *v, unsigned seed)
{
    int i, j;

    for (i = 0; i < NYMM; i++) {
        for (j = 0; j < 8; j++) {
            v[i].w[j] = (seed * 0x9E3779B1u + (unsigned)(i * 8 + j) * 0x01000193u) | 1u;
        }
    }
}

static void zero_upper(struct ymm *v)
{
    int i, j;

    for (i = 0; i < NYMM; i++) {
        for (j = 4; j < 8; j++) {
            v[i].w[j] = 0;
        }
    }
}

/* "ymmN: the XMM half" or "ymmN: the upper half". */
static char why_text[40];

static const char *differs(int reg, int upper)
{
    const char *tail = upper ? ": the upper half" : ": the XMM half";
    char *p = why_text;
    size_t i;

    *p++ = 'y';
    *p++ = 'm';
    *p++ = 'm';
    if (reg >= 10) {
        *p++ = (char)('0' + reg / 10);
    }
    *p++ = (char)('0' + reg % 10);
    for (i = 0; i <= strlen(tail); i++) {
        *p++ = tail[i];
    }
    return why_text;
}

/* The registers against `want`: NULL when they agree, else which differs. */
static const char *ymm_check(const struct ymm *want)
{
    static struct ymm got[NYMM];
    int i, j;

    ymm_store(got);
    for (i = 0; i < NYMM; i++) {
        for (j = 0; j < 8; j++) {
            if (got[i].w[j] != want[i].w[j]) {
                return differs(i, j >= 4);
            }
        }
    }
    return 0;
}

/* Check for `ms` milliseconds while yielding and sleeping, so the other
 * process runs between checks and the timer preempts the spins. */
static const char *hold(const struct ymm *v, unsigned ms)
{
    unsigned start = now_ms();
    struct timespec nap;
    const char *why;
    volatile unsigned spin;

    nap.sec = 0;
    nap.nsec = 2000000;
    while (now_ms() - start < ms) {
        why = ymm_check(v);
        if (why) {
            return why;
        }
        syscall1(SYS_SCHED_YIELD, 0);
        syscall2(SYS_NANOSLEEP, (long)&nap, 0);
        for (spin = 0; spin < 20000u; spin++) {
        }
    }
    return 0;
}

/* The XSAVE image `img` against the registers `want`: the XMM halves at
 * byte 160 and the upper halves at the AVX component's offset. */
static const char *image_check(const unsigned char *img, const struct ymm *want)
{
    int i, j;

    for (i = 0; i < NYMM; i++) {
        for (j = 0; j < 4; j++) {
            if (get32(img + X_XMM + i * 16 + j * 4) != want[i].w[j]) {
                return differs(i, 0);
            }
            if (get32(img + avx_offset + i * 16 + j * 4) != want[i].w[4 + j]) {
                return differs(i, 1);
            }
        }
    }
    return 0;
}

/* The rows the handler serves. */
enum { ROW_FRAME, ROW_WRITE, ROW_MAGIC, ROW_HEADER };

static volatile int mode;
static volatile const char *handler_fault;
static struct ymm want[NYMM];
static int header_pipe[2];

/* The frame's state at `fp` against Linux's layout and the interrupted
 * registers `want`. */
static const char *frame_check(const unsigned char *fp)
{
    const unsigned char *img = fp + FRAME_PREFIX;
    const char *why;

    if ((unsigned long)img & 63u) {
        return "the frame's image is not 64-byte aligned";
    }
#if !defined __x86_64__
    if (fp[110] != 0 || fp[111] != 0) {
        return "the frame's magic word is not 0, the FXSR one";
    }
#endif
    if (get32(img + X_SW) != FP_XSTATE_MAGIC1) {
        return "the frame's software bytes lack FP_XSTATE_MAGIC1";
    }
    if (get32(img + X_SW + 4) != xstate_size + 4u + FRAME_PREFIX) {
        return "the frame's extended size is not the image's and the magic's";
    }
    if (get32(img + X_SW + 8) != xcr0_lo || get32(img + X_SW + 12) != xcr0_hi) {
        return "the frame's software bytes do not have XCR0";
    }
    if (get32(img + X_SW + 16) != xstate_size) {
        return "the frame's image size is not leaf 0xD's";
    }
    if (get32(img + xstate_size) != FP_XSTATE_MAGIC2) {
        return "FP_XSTATE_MAGIC2 does not follow the frame's image";
    }
    if ((get32(img + X_BV) & 7u) != 7u) {
        return "the frame's XSTATE_BV does not mark x87, SSE and AVX in use";
    }
    why = image_check(img, want);
    if (why) {
        return why;
    }
    return 0;
}

static void handler(int sig, void *info, void *ucontext)
{
    unsigned char *uc = ucontext;
    unsigned char *fp = *(unsigned char **)(uc + UC_FPSTATE);
    unsigned char *img = fp + FRAME_PREFIX;
    static struct ymm seen[NYMM], junk[NYMM];
    int i, j;

    (void)sig;
    (void)info;
    switch (mode) {
    case ROW_FRAME:
        ymm_store(seen);
        for (i = 0; i < NYMM; i++) {
            for (j = 0; j < 8; j++) {
                if (seen[i].w[j] != 0) {
                    handler_fault = "the handler did not start with the YMM registers zero";
                }
            }
        }
        if (!handler_fault && !(*(unsigned long *)uc & UC_FP_XSTATE)) {
            handler_fault = "the frame's uc_flags lack UC_FP_XSTATE";
        }
        if (!handler_fault) {
            handler_fault = frame_check(fp);
        }
        make_values(junk, 0xDEAD0000u);
        ymm_load(junk);
        break;
    case ROW_WRITE:
        for (j = 0; j < 4; j++) {
            put32(img + avx_offset + 16 + j * 4, 0xA5A50000u + (unsigned)j);
        }
        break;
    case ROW_MAGIC:
        put32(img + X_SW, 0);
        break;
    default:
        img[X_COMP] = 1;
        syscall3(SYS_WRITE, header_pipe[1], (long)"d", 1);
        break;
    }
}

extern void sig_restorer(void);
PROBE_RT_RESTORER(sig_restorer);

static void install(int signo)
{
    struct k_sigaction ka;

    ka.handler = (void (*)(int))(void (*)(void))handler;
    ka.flags = SA_SIGINFO | SA_RESTORER;
    ka.restorer = sig_restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    if (syscall4(SYS_RT_SIGACTION, signo, (long)&ka, 0, 8) != 0) {
        fail("rt_sigaction");
    }
}

/**
 * The checks of `cpuid` and XCR0. With `noxsave`, the kernel was told not
 * to use XSAVE, and OSXSAVE must be clear wherever XSAVE is reported.
 */
static void run_cpuid(int noxsave)
{
    unsigned r1[4], r7[4], rd[4], r[4];
    int xsave, osxsave, avx, avx512;
    unsigned want_xcr0;

    cpuid(0, 0, r);
    if (r[0] < 1) {
        fail("cpuid has no leaf 1");
    }
    cpuid(1, 0, r1);
    xsave = (r1[2] >> 26) & 1u;
    osxsave = (r1[2] >> 27) & 1u;
    avx = (r1[2] >> 28) & 1u;
    if (osxsave != (xsave && !noxsave)) {
        fail(osxsave ? "cpuid reports OSXSAVE where the kernel saves no XSAVE state"
                     : "cpuid reports XSAVE but not OSXSAVE");
    }
    if (!osxsave) {
        puts("xsaveprobe: cpuid: no OSXSAVE, as the kernel keeps no XSAVE state");
        return;
    }
    if (r[0] < 0xD) {
        fail("cpuid reports XSAVE without leaf 0xD");
    }
    __asm__ __volatile__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    cpuid(7, 0, r7);
    cpuid(0xD, 0, rd);
    avx512 = (r7[1] >> 16) & 1u;
    want_xcr0 = 3u;
    if (avx && (rd[0] & 4u)) {
        want_xcr0 |= 4u;
        if (avx512 && (rd[0] & 0xE0u) == 0xE0u) {
            want_xcr0 |= 0xE0u;
        }
    }
    if (xcr0_lo != want_xcr0 || xcr0_hi != 0) {
        fail("XCR0 is not x87, SSE, and AVX and AVX-512 where the processor has them");
    }
    xstate_size = rd[1];
    if (xstate_size < 576u || xstate_size > IMAGE_MAX) {
        fail("leaf 0xD gives an image size out of range");
    }
    avx_usable = (xcr0_lo & 6u) == 6u;
    if (avx_usable) {
        cpuid(0xD, 2, r);
        avx_offset = r[1];
    }
    puts(xcr0_lo & 0xE0u ? "xsaveprobe: cpuid: OSXSAVE, XCR0 has x87, SSE, AVX and AVX-512"
                         : (avx_usable ? "xsaveprobe: cpuid: OSXSAVE, XCR0 has x87, SSE and AVX"
                                       : "xsaveprobe: cpuid: OSXSAVE, XCR0 has x87 and SSE"));
}

static void run_switches(void)
{
    static struct ymm before[NYMM], mine[NYMM];
    int pid, status;
    const char *why;

    make_values(before, 0x0F0Fu);
    ymm_load(before);
    pid = (int)syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    why = ymm_check(before);
    if (why) {
        fail2(pid == 0 ? "the child across fork" : "the parent across fork", why);
    }
    make_values(mine, pid == 0 ? 0x1111u : 0x2222u);
    ymm_load(mine);
    why = hold(mine, 300u);
    if (pid == 0) {
        if (why) {
            fail2("the child across switches", why);
        }
        sys_exit(0);
    }
    if (why) {
        fail2("the parent across switches", why);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("the child's registers across switches");
    }
}

/* Load `want` from `seed`, take SIGUSR1 in `row`, and return what the
 * registers then differ in from `after`, or NULL. */
static const char *signal_row(int row, unsigned seed, const struct ymm *after)
{
    mode = row;
    make_values(want, seed);
    ymm_load(want);
    syscall2(SYS_KILL, getpid(), SIGUSR1);
    return ymm_check(after ? after : want);
}

static void run_signals(void)
{
    static struct ymm after[NYMM];
    const char *why;
    int j;

    install(SIGUSR1);
    why = signal_row(ROW_FRAME, 0x3333u, 0);
    if (handler_fault) {
        fail2("the handler", (const char *)handler_fault);
    }
    if (why) {
        fail2("after a handler that clobbered the registers", why);
    }

    make_values(after, 0x4444u);
    for (j = 0; j < 4; j++) {
        after[1].w[4 + j] = 0xA5A50000u + (unsigned)j;
    }
    why = signal_row(ROW_WRITE, 0x4444u, after);
    if (why) {
        fail2("after a handler that wrote ymm1's upper half in the frame", why);
    }

    make_values(after, 0x5555u);
    zero_upper(after);
    why = signal_row(ROW_MAGIC, 0x5555u, after);
    if (why) {
        fail2("after a handler that cleared FP_XSTATE_MAGIC1", why);
    }
}

static void run_header(void)
{
    int pid, status;
    char byte = 0;

    if (syscall1(SYS_PIPE, (long)header_pipe) != 0) {
        fail("pipe for the damaged header");
    }
    pid = (int)syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for the damaged header");
    }
    if (pid == 0) {
        install(SIGUSR1);
        signal_row(ROW_HEADER, 0x7777u, 0);
        puts("XSAVEPROBE FAIL: a frame with a damaged XSAVE header was restored");
        sys_exit(6);
    }
    if (syscall3(SYS_READ, header_pipe[0], (long)&byte, 1) != 1 || byte != 'd') {
        fail("the handler did not report the header damaged");
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0x7F) != SIGSEGV) {
        fail("a damaged XSAVE header did not kill the child with SIGSEGV");
    }
}

static void run_exec(void)
{
    int pid, status;

    pid = (int)syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for exec");
    }
    if (pid == 0) {
        char *argv[3];

        make_values(want, 0x8888u);
        ymm_load(want);
        argv[0] = (char *)"xsaveprobe";
        argv[1] = (char *)"reset";
        argv[2] = 0;
        syscall3(SYS_EXECVE, (long)"/xsaveprobe", (long)argv, 0);
        sys_exit(2);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("an exec did not start with the YMM registers zero");
    }
}

/* What a fresh image sees: every YMM register zero. */
static void run_reset(void)
{
    static const struct ymm zero[NYMM];

    sys_exit(ymm_check(zero) ? 3 : 0);
}

/* Fork a child that traces itself, loads the values of `seed` when `avx`,
 * and stops; the parent returns with the child in its stop. */
static int traced_child(int avx, unsigned seed)
{
    int pid, status;

    pid = (int)syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for ptrace");
    }
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) {
            sys_exit(8);
        }
        if (avx) {
            make_values(want, seed);
            ymm_load(want);
        }
        syscall2(SYS_KILL, getpid(), SIGSTOP);
        if (avx) {
            /* The tracer wrote ymm2's upper half. */
            int j;

            for (j = 0; j < 4; j++) {
                want[2].w[4 + j] = 0x5A5A0000u + (unsigned)j;
            }
            sys_exit(ymm_check(want) ? 7 : 0);
        }
        sys_exit(0);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F ||
        ((status >> 8) & 0xFF) != SIGSTOP) {
        fail("the traced child did not stop");
    }
    return pid;
}

static void resume(int pid, const char *what)
{
    int status;

    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) {
        fail("PTRACE_CONT");
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail(what);
    }
}

static void run_ptrace(void)
{
    static unsigned char img[IMAGE_MAX], fx[512];
    static struct ymm expect[NYMM];
    struct iovec iov;
    const char *why;
    long rc;
    int pid, j;

    pid = traced_child(1, 0x6666u);
    iov.base = img;
    iov.len = 12;
    if (ptrace(PTRACE_GETREGSET, pid, NT_X86_XSTATE, (long)&iov) != -EINVAL) {
        fail("PTRACE_GETREGSET took a length that is not a multiple of 8");
    }
    iov.len = sizeof(img);
    rc = ptrace(PTRACE_GETREGSET, pid, NT_X86_XSTATE, (long)&iov);
    if (rc != 0) {
        fail("PTRACE_GETREGSET with NT_X86_XSTATE");
    }
    if (iov.len != xstate_size) {
        fail("PTRACE_GETREGSET did not give leaf 0xD's image size");
    }
    if (get32(img + X_SW) != xcr0_lo || get32(img + X_SW + 4) != xcr0_hi) {
        fail("NT_X86_XSTATE does not have XCR0 at byte 464");
    }
    if ((get32(img + X_BV) & 4u) == 0) {
        fail("NT_X86_XSTATE does not mark AVX in use");
    }
    make_values(expect, 0x6666u);
    why = image_check(img, expect);
    if (why) {
        fail2("NT_X86_XSTATE of the stopped child", why);
    }
    iov.base = fx;
    iov.len = sizeof(fx);
    if (ptrace(PTRACE_GETREGSET, pid, NT_FXSAVE, (long)&iov) != 0 || iov.len != sizeof(fx)) {
        fail("PTRACE_GETREGSET of the fxsave image");
    }
    for (j = 0; j < NYMM * 4; j++) {
        if (get32(fx + X_XMM + j * 4) != expect[j / 4].w[j % 4]) {
            fail2("the fxsave image of the stopped child", differs(j / 4, 0));
        }
    }
    for (j = 0; j < 4; j++) {
        put32(img + avx_offset + 2 * 16 + j * 4, 0x5A5A0000u + (unsigned)j);
    }
    iov.base = img;
    iov.len = xstate_size;
    if (ptrace(PTRACE_SETREGSET, pid, NT_X86_XSTATE, (long)&iov) != 0) {
        fail("PTRACE_SETREGSET with NT_X86_XSTATE");
    }
    resume(pid, "the child did not find the YMM register PTRACE_SETREGSET wrote");
}

struct rlimit64 {
    unsigned long long cur;
    unsigned long long max;
};

static unsigned long long get64(const unsigned char *p)
{
    return (unsigned long long)get32(p) | ((unsigned long long)get32(p + 4) << 32);
}

/* Read `n` bytes at `off` of the file `fd` into `buf`; 0, or -1. */
static int read_at(int fd, unsigned long off, unsigned char *buf, unsigned long n)
{
    if (syscall3(SYS_LSEEK, fd, (long)off, 0) != (long)off) {
        return -1;
    }
    return syscall3(SYS_READ, fd, (long)buf, (long)n) == (long)n ? 0 : -1;
}

/**
 * The core file's NT_X86_XSTATE note, read into `notes`: its description,
 * or NULL, with its size in `*size`. The notes are the file's first program
 * header, PT_NOTE, in the ELF class of the probe.
 */
static const unsigned char *core_xstate(unsigned char *notes, unsigned long cap, unsigned *size)
{
    unsigned char ehdr[64], phdr[56];
    unsigned long phoff, off, len, at;
    int fd;

    fd = (int)syscall2(SYS_OPEN, (long)CORE_PATH, 0);
    if (fd < 0) {
        fail("the core file cannot be opened");
    }
#if defined __x86_64__
    if (read_at(fd, 0, ehdr, 64) != 0) {
        fail("the core file's header");
    }
    phoff = (unsigned long)get64(ehdr + 32);
    if (read_at(fd, phoff, phdr, 56) != 0 || get32(phdr) != PT_NOTE) {
        fail("the core file's first program header is not PT_NOTE");
    }
    off = (unsigned long)get64(phdr + 8);
    len = (unsigned long)get64(phdr + 32);
#else
    if (read_at(fd, 0, ehdr, 52) != 0) {
        fail("the core file's header");
    }
    phoff = get32(ehdr + 28);
    if (read_at(fd, phoff, phdr, 32) != 0 || get32(phdr) != PT_NOTE) {
        fail("the core file's first program header is not PT_NOTE");
    }
    off = get32(phdr + 4);
    len = get32(phdr + 16);
#endif
    if (len > cap || read_at(fd, off, notes, len) != 0) {
        fail("the core file's notes");
    }
    syscall1(SYS_CLOSE, fd);
    for (at = 0; at + 12 <= len;) {
        unsigned namesz = get32(notes + at), descsz = get32(notes + at + 4);
        unsigned long desc = at + 12 + ((namesz + 3u) & ~3u);

        if (get32(notes + at + 8) == NT_X86_XSTATE && namesz == 6 && notes[at + 12] == 'L' &&
            notes[at + 16] == 'X') {
            *size = descsz;
            return notes + desc;
        }
        at = desc + ((descsz + 3u) & ~3u);
    }
    return 0;
}

static void run_core(void)
{
    static unsigned char notes[16384];
    static struct ymm expect[NYMM];
    const unsigned char *img;
    struct rlimit64 rl;
    unsigned size = 0;
    int pid, status;
    const char *why;

    syscall2(SYS_MKDIR, (long)CORE_DIR, 0700);
    syscall1(SYS_UNLINK, (long)CORE_PATH);
    pid = (int)syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for the core file");
    }
    if (pid == 0) {
        if (syscall1(SYS_CHDIR, (long)CORE_DIR) != 0 ||
            syscall4(SYS_PRLIMIT64, 0, RLIMIT_CORE, 0, (long)&rl) != 0) {
            sys_exit(9);
        }
        rl.cur = rl.max;
        if (syscall4(SYS_PRLIMIT64, 0, RLIMIT_CORE, (long)&rl, 0) != 0) {
            sys_exit(9);
        }
        make_values(want, 0x9999u);
        ymm_load(want);
        syscall2(SYS_KILL, getpid(), SIGQUIT);
        sys_exit(10);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0x7F) != SIGQUIT ||
        !(status & 0x80)) {
        fail("the child did not dump core on SIGQUIT");
    }
    img = core_xstate(notes, sizeof(notes), &size);
    if (!img) {
        fail("the core file has no NT_X86_XSTATE note");
    }
    if (size != xstate_size) {
        fail("the core file's NT_X86_XSTATE is not leaf 0xD's image size");
    }
    if (get32(img + X_SW) != xcr0_lo || get32(img + X_SW + 4) != xcr0_hi) {
        fail("the core file's NT_X86_XSTATE does not have XCR0 at byte 464");
    }
    make_values(expect, 0x9999u);
    why = image_check(img, expect);
    if (why) {
        fail2("the core file's NT_X86_XSTATE", why);
    }
    syscall1(SYS_UNLINK, (long)CORE_PATH);
    syscall1(SYS_RMDIR, (long)CORE_DIR);
}

static void run_ptrace_without(void)
{
    static unsigned char img[IMAGE_MAX];
    struct iovec iov;
    int pid;

    pid = traced_child(0, 0);
    iov.base = img;
    iov.len = sizeof(img);
    if (ptrace(PTRACE_GETREGSET, pid, NT_X86_XSTATE, (long)&iov) != -ENODEV) {
        fail("PTRACE_GETREGSET with NT_X86_XSTATE did not fail with ENODEV without XSAVE");
    }
    resume(pid, "the traced child did not end");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int noxsave = argc >= 2 && argv[1][0] == 'n';

    if (argc >= 2 && argv[1][0] == 'r') {
        run_reset();
    }
    run_cpuid(noxsave);
    if (!avx_usable) {
        not_run("switches");
        not_run("signals");
        not_run("header");
        not_run("exec");
        not_run("ptrace");
        not_run("core");
        if (xstate_size == 0) {
            run_ptrace_without();
        }
        puts("XSAVEPROBE OK");
        sys_exit(0);
    }
    run_switches();
    run_signals();
    run_header();
    run_exec();
    run_ptrace();
    run_core();
    puts("XSAVEPROBE OK");
    sys_exit(0);
}
