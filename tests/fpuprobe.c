/**
 * The floating-point state of a process, proved from user space.
 *
 * `fpuprobe`: the parent and a forked child each keep their own values in
 * `st(0)`, `st(1)` and `xmm0` to `xmm7`, or to `xmm15` on x86-64, a control
 * word with the rounding mode set to round down, and an MXCSR with
 * flush-to-zero set, and each checks them again and again for half a second
 * while yielding to the other and being preempted by the timer. Then the parent
 * raises a signal whose handler must start with the initial state, clobbers
 * everything it can reach, raises a second signal from inside the first, and
 * returns; the values must be as they were before the signal. A child then sets
 * the same state and executes this program with `reset`, which must find the
 * initial state: control word 0x37F, every register tagged empty, MXCSR 0x1F80.
 * Last, a child's handler sets a reserved bit of MXCSR in the frame the kernel
 * built on its stack and returns through it; the kernel must refuse the
 * frame and the child must die of SIGSEGV.
 *
 * `fpuprobe x87`, for a boot with `rach2.nofxsr`, which only the i686 kernel
 * takes: the same without the XMM registers and MXCSR, since an SSE
 * instruction is an invalid opcode there, and without the damaged frame,
 * since the x87 image has no bit a load can fault on.
 *
 * On x86-64 the probe's own code is compiled to use the general registers
 * alone, so that nothing but the rows' assembly touches an XMM register.
 *
 * Prints "FPUPROBE OK" or "FPUPROBE FAIL: reason". Freestanding; direct system
 * calls (probe.h).
 */

#include "probe.h"

#if defined __x86_64__
#pragma GCC target("general-regs-only")
#define NXMM 16
#else
#define NXMM 8
#endif

#define SIGSEGV     11
#define SIGUSR1     10
#define SIGUSR2     12
#define SA_SIGINFO  0x00000004u
#define SA_RESTORER 0x04000000u

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
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

static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"FPUPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

struct timeval {
    long sec;
    long usec;
};

static unsigned now_ms(void)
{
    struct timeval tv;
    syscall2(SYS_GETTIMEOFDAY, (long)&tv, 0);
    return (unsigned)tv.sec * 1000u + (unsigned)tv.usec / 1000u;
}

static int with_sse = 1;

static unsigned read_cw(void)
{
    unsigned short w = 0;

    __asm__ __volatile__("fnstcw %0" : "=m"(w));
    return w;
}

static unsigned read_mxcsr(void)
{
    unsigned m = 0;

    __asm__ __volatile__("stmxcsr %0" : "=m"(m));
    return m;
}

/* One process's values: two doubles for the x87 stack and a row of four
 * words for each XMM register, all derived from a seed. */
struct values {
    unsigned st0[2];
    unsigned st1[2];
    unsigned xmm[NXMM][4];
};

static void make_values(struct values *v, unsigned seed)
{
    int i, j;

    v->st0[0] = seed * 0x9E3779B1u;
    v->st0[1] = 0x40000000u | (seed & 0xFFFFu);
    v->st1[0] = ~seed;
    v->st1[1] = 0x40100000u | ((seed * 7u) & 0xFFFFu);
    for (i = 0; i < NXMM; i++) {
        for (j = 0; j < 4; j++) {
            v->xmm[i][j] = seed * 0x01000193u + (unsigned)(i * 4 + j) * 0x100001B3u;
        }
    }
}

#define CW_ROUND_DOWN 0x077Fu /* the initial 0x37F with RC = 01 */
#define CW_INITIAL    0x037Fu
#define MXCSR_INITIAL 0x1F80u
#define MXCSR_FLUSH   0x9F80u /* the initial with FZ set */

static void load_values(const struct values *v, unsigned cw, unsigned mxcsr)
{
    unsigned short w = (unsigned short)cw;

    __asm__ __volatile__("fninit; fldcw %0" : : "m"(w));
    __asm__ __volatile__("fldl %0; fldl %1" : : "m"(v->st1), "m"(v->st0));
    if (with_sse) {
        __asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));
        __asm__ __volatile__(
            "movups %0, %%xmm0; movups %1, %%xmm1; movups %2, %%xmm2; movups %3, %%xmm3"
            :
            : "m"(v->xmm[0]), "m"(v->xmm[1]), "m"(v->xmm[2]), "m"(v->xmm[3]));
        __asm__ __volatile__(
            "movups %0, %%xmm4; movups %1, %%xmm5; movups %2, %%xmm6; movups %3, %%xmm7"
            :
            : "m"(v->xmm[4]), "m"(v->xmm[5]), "m"(v->xmm[6]), "m"(v->xmm[7]));
#if defined __x86_64__
        __asm__ __volatile__(
            "movups %0, %%xmm8; movups %1, %%xmm9; movups %2, %%xmm10; movups %3, %%xmm11"
            :
            : "m"(v->xmm[8]), "m"(v->xmm[9]), "m"(v->xmm[10]), "m"(v->xmm[11]));
        __asm__ __volatile__(
            "movups %0, %%xmm12; movups %1, %%xmm13; movups %2, %%xmm14; movups %3, %%xmm15"
            :
            : "m"(v->xmm[12]), "m"(v->xmm[13]), "m"(v->xmm[14]), "m"(v->xmm[15]));
#endif
    }
}

/* What the registers hold now, read without changing them: the x87 stack is
 * stored and loaded back, so the tags stay as they were. */
static void read_values(struct values *v, unsigned *cw, unsigned *mxcsr)
{
    unsigned short w = 0;
    unsigned m = 0;

    __asm__ __volatile__("fnstcw %0" : "=m"(w));
    __asm__ __volatile__("fstpl %0; fstpl %1; fldl %1; fldl %0" : "=m"(v->st0), "=m"(v->st1));
    *cw = w;
    if (with_sse) {
        __asm__ __volatile__("stmxcsr %0" : "=m"(m));
        __asm__ __volatile__(
            "movups %%xmm0, %0; movups %%xmm1, %1; movups %%xmm2, %2; movups %%xmm3, %3"
            : "=m"(v->xmm[0]), "=m"(v->xmm[1]), "=m"(v->xmm[2]), "=m"(v->xmm[3]));
        __asm__ __volatile__(
            "movups %%xmm4, %0; movups %%xmm5, %1; movups %%xmm6, %2; movups %%xmm7, %3"
            : "=m"(v->xmm[4]), "=m"(v->xmm[5]), "=m"(v->xmm[6]), "=m"(v->xmm[7]));
#if defined __x86_64__
        __asm__ __volatile__(
            "movups %%xmm8, %0; movups %%xmm9, %1; movups %%xmm10, %2; movups %%xmm11, %3"
            : "=m"(v->xmm[8]), "=m"(v->xmm[9]), "=m"(v->xmm[10]), "=m"(v->xmm[11]));
        __asm__ __volatile__(
            "movups %%xmm12, %0; movups %%xmm13, %1; movups %%xmm14, %2; movups %%xmm15, %3"
            : "=m"(v->xmm[12]), "=m"(v->xmm[13]), "=m"(v->xmm[14]), "=m"(v->xmm[15]));
#endif
    }
    *mxcsr = m;
}

/* The values against what was loaded: NULL when they agree, else why not. */
static const char *compare(const struct values *want, unsigned cw, unsigned mxcsr)
{
    struct values got;
    unsigned got_cw, got_mxcsr;
    int i, j;

    read_values(&got, &got_cw, &got_mxcsr);
    if (got_cw != cw) {
        return "the control word";
    }
    if (got.st0[0] != want->st0[0] || got.st0[1] != want->st0[1]) {
        return "st(0)";
    }
    if (got.st1[0] != want->st1[0] || got.st1[1] != want->st1[1]) {
        return "st(1)";
    }
    if (with_sse) {
        if (got_mxcsr != mxcsr) {
            return "MXCSR";
        }
        for (i = 0; i < NXMM; i++) {
            for (j = 0; j < 4; j++) {
                if (got.xmm[i][j] != want->xmm[i][j]) {
                    return "an XMM register";
                }
            }
        }
    }
    return 0;
}

/* Check for `ms` milliseconds while yielding, so the other process runs
 * between checks and the timer preempts the spins. Returns the first
 * failure, or NULL. */
static const char *hold(const struct values *v, unsigned cw, unsigned mxcsr, unsigned ms)
{
    unsigned start = now_ms();
    const char *why;
    volatile unsigned spin;

    while (now_ms() - start < ms) {
        why = compare(v, cw, mxcsr);
        if (why) {
            return why;
        }
        syscall1(SYS_SCHED_YIELD, 0);
        for (spin = 0; spin < 20000u; spin++) {
        }
    }
    return 0;
}

extern void sig_restorer(void);
PROBE_RT_RESTORER(sig_restorer);

static void install(int signo, void (*h)(int))
{
    struct k_sigaction ka;

    ka.handler = h;
    ka.flags = SA_SIGINFO | SA_RESTORER; /* the rt frame its restorer returns from */
    ka.restorer = sig_restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    if (syscall4(SYS_RT_SIGACTION, signo, (long)&ka, 0, 8) != 0) {
        fail("rt_sigaction");
    }
}

static volatile int handlers_ran;
static volatile const char *handler_fault;

/* Every handler must start with the initial state, and it leaves behind
 * values the interrupted code must not see. */
static void clobber_handler(int sig)
{
    struct values junk;

    if (read_cw() != CW_INITIAL) {
        handler_fault = "the handler did not start with the initial control word";
    }
    if (with_sse && read_mxcsr() != MXCSR_INITIAL) {
        handler_fault = "the handler did not start with the initial MXCSR";
    }
    make_values(&junk, 0xDEAD0000u + (unsigned)sig);
    load_values(&junk, 0x0F7Fu, 0x7F80u);
    handlers_ran++;
    if (sig == SIGUSR1) {
        syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR2);
        /* The inner handler clobbered everything and returned: the outer
         * one must find its own values back. */
        if (compare(&junk, 0x0F7Fu, 0x7F80u)) {
            handler_fault = "the outer handler did not get its state back from the inner one";
        }
    }
}

static int damage_pipe[2];

#if defined __x86_64__
/**
 * Damage MXCSR in the saved floating-point image.
 *
 * The handler of an SA_SIGINFO signal is given the ucontext as its third
 * argument. Its `uc_mcontext` starts 40 bytes in, and its `fpstate` word,
 * 184 bytes further, holds the address of the `_fpstate_64`, the `fxsave64`
 * image, with MXCSR 24 bytes into it. A byte goes down the pipe after the
 * write, so the parent knows the frame was damaged before the kernel refused
 * it.
 */
static void damage_siginfo_handler(int sig, void *info, void *ucontext)
{
    unsigned char *uc = ucontext;
    unsigned char *fpstate = *(unsigned char **)(uc + 40 + 184);
    unsigned mxcsr;
    int k;

    (void)sig;
    (void)info;
    for (k = 0; k < 4; k++) {
        ((unsigned char *)&mxcsr)[k] = fpstate[24 + k];
    }
    mxcsr |= 0x80000000u;
    for (k = 0; k < 4; k++) {
        fpstate[24 + k] = ((unsigned char *)&mxcsr)[k];
    }
    syscall3(SYS_WRITE, damage_pipe[1], (long)"d", 1);
}

/* The handler as install takes it; its SA_SIGINFO call passes the other two
 * arguments in RSI and RDX. */
#define damage_handler ((void (*)(int))(void (*)(void))damage_siginfo_handler)
#else
/**
 * Damage MXCSR in the saved floating-point image.
 *
 * The frame the kernel built starts where the handler's stack pointer was
 * at entry, which is one word above the saved frame pointer. It is Linux's
 * rt frame: the return address, the signal number, the siginfo and ucontext
 * pointers, the 128-byte siginfo, and the ucontext at byte 144. The
 * ucontext's `uc_mcontext` starts 20 bytes in, and its `fpstate` word, 76
 * bytes further, holds the address of the `_fpstate_32`, whose `fxsave`
 * image starts at byte 112 with MXCSR 24 bytes into it. A byte goes down the
 * pipe after the write, so the parent knows the frame was damaged before the
 * kernel refused it.
 */
static void damage_handler(int sig)
{
    unsigned char *frame = (unsigned char *)((unsigned *)__builtin_frame_address(0) + 1);
    unsigned char *uc = *(unsigned char **)(frame + 12);
    unsigned char *fpstate;
    unsigned mxcsr;
    int k;

    (void)sig;
    if (uc != frame + 144) {
        puts("FPUPROBE FAIL: the ucontext is not where Linux's rt frame puts it");
        sys_exit(1);
    }
    fpstate = *(unsigned char **)(uc + 20 + 76);
    for (k = 0; k < 4; k++) {
        ((unsigned char *)&mxcsr)[k] = fpstate[112 + 24 + k];
    }
    mxcsr |= 0x80000000u;
    for (k = 0; k < 4; k++) {
        fpstate[112 + 24 + k] = ((unsigned char *)&mxcsr)[k];
    }
    syscall3(SYS_WRITE, damage_pipe[1], (long)"d", 1);
}
#endif

static void run_switches(void)
{
    struct values mine, before;
    int pid, status;
    const char *why;

    /* The parent's live state before the fork is what the child must start
     * with, on both sides of the fork and before either loads its own. */
    make_values(&before, 0x0F0Fu);
    load_values(&before, CW_ROUND_DOWN, MXCSR_FLUSH);
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    why = compare(&before, CW_ROUND_DOWN, MXCSR_FLUSH);
    if (why) {
        syscall3(SYS_WRITE, 1, (long)"FPUPROBE FAIL: across fork: ", 28);
        puts(why);
        sys_exit(1);
    }
    make_values(&mine, pid == 0 ? 0x1111u : 0x2222u);
    load_values(&mine, CW_ROUND_DOWN, MXCSR_FLUSH);
    why = hold(&mine, CW_ROUND_DOWN, MXCSR_FLUSH, 500u);
    if (pid == 0) {
        if (why) {
            syscall3(SYS_WRITE, 1, (long)"FPUPROBE FAIL: the child lost ", 30);
            puts(why);
            sys_exit(1);
        }
        sys_exit(0);
    }
    if (why) {
        syscall3(SYS_WRITE, 1, (long)"FPUPROBE FAIL: the parent lost ", 31);
        puts(why);
        sys_exit(1);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("the child's state across switches");
    }
}

static void run_signals(void)
{
    struct values mine;
    const char *why;

    install(SIGUSR1, clobber_handler);
    install(SIGUSR2, clobber_handler);
    make_values(&mine, 0x3333u);
    load_values(&mine, CW_ROUND_DOWN, MXCSR_FLUSH);
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    if (handlers_ran != 2) {
        fail("the nested handler did not run");
    }
    if (handler_fault) {
        fail((const char *)handler_fault);
    }
    why = compare(&mine, CW_ROUND_DOWN, MXCSR_FLUSH);
    if (why) {
        syscall3(SYS_WRITE, 1, (long)"FPUPROBE FAIL: after the signal: ", 33);
        puts(why);
        sys_exit(1);
    }
}

static void run_exec(void)
{
    struct values mine;
    int pid, status;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for exec");
    }
    if (pid == 0) {
        char *argv[3];

        make_values(&mine, 0x4444u);
        load_values(&mine, CW_ROUND_DOWN, MXCSR_FLUSH);
        argv[0] = (char *)"fpuprobe";
        argv[1] = (char *)(with_sse ? "reset" : "reset-x87");
        argv[2] = 0;
        syscall3(SYS_EXECVE, (long)"/fpuprobe", (long)argv, 0);
        sys_exit(2);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("exec did not reset the state");
    }
}

/* The state a fresh image sees: the control word, an empty stack (the tag
 * word all ones through fnstenv), and the MXCSR. */
static unsigned char env[28]; /* what fnstenv writes: the x87 environment */

static void run_reset(void)
{
    unsigned short cw, tags;

    __asm__ __volatile__("fnstenv %0" : "=m"(env));
    cw = (unsigned short)(env[0] | (env[1] << 8));
    tags = (unsigned short)(env[8] | (env[9] << 8));
    if (cw != CW_INITIAL) {
        sys_exit(3);
    }
    if (tags != 0xFFFFu) {
        sys_exit(4);
    }
    if (with_sse && read_mxcsr() != MXCSR_INITIAL) {
        sys_exit(5);
    }
    sys_exit(0);
}

static void run_damaged(void)
{
    int pid, status;
    char byte = 0;

    if (syscall1(SYS_PIPE, (long)damage_pipe) != 0) {
        fail("pipe for the damaged frame");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for the damaged frame");
    }
    if (pid == 0) {
        install(SIGUSR1, damage_handler);
        syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
        puts("FPUPROBE FAIL: the damaged frame was restored");
        sys_exit(6);
    }
    if (syscall3(SYS_READ, damage_pipe[0], (long)&byte, 1) != 1 || byte != 'd') {
        fail("the handler did not report the frame damaged");
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid for the damaged frame");
    }
    if ((status & 0x7F) != SIGSEGV) {
        fail("the damaged frame did not kill the child with SIGSEGV");
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc >= 2 && argv[1][0] == 'r') {
        with_sse = argv[1][5] == '\0';
        run_reset();
    }
    if (argc >= 2 && argv[1][0] == 'x') {
        with_sse = 0;
    }
    run_switches();
    run_signals();
    run_exec();
    if (with_sse) {
        run_damaged();
    }
    puts("FPUPROBE OK");
    sys_exit(0);
}
