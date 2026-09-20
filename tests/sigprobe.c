/**
 * Exercises signal delivery from user space via direct int 0x80 syscalls.
 *
 * Three checks:
 *   * install a SIGUSR1 handler (with a tiny rt_sigreturn restorer), raise it
 *     to ourselves, and confirm the handler runs and resumes after it;
 *   * block SIGUSR1, raise it (stays pending, handler must NOT run), then
 *     unblock and confirm the now-pending signal is delivered;
 *   * fork a child that spins; SIGTERM it and confirm the default action
 *     terminates it (parent reaps WTERMSIG == SIGTERM);
 *   * a handler that writes a selector naming no descriptor into the saved
 *     `gs` of its own frame, which the kernel loads at ring 0 on the way
 *     back: the machine must survive it, since a process may not choose
 *     what the kernel loads.
 *
 * Prints "SIGPROBE OK" on success. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT           1
#define SYS_FORK           2
#define SYS_WRITE          4
#define SYS_WAITPID        7
#define SYS_GETPID         20
#define SYS_KILL           37
#define SYS_RT_SIGACTION   174
#define SYS_RT_SIGPROCMASK 175

#define SIGUSR1 10
#define SIGTERM 15

#define SA_RESTORER 0x04000000u

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int syscall4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

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
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

/**
 * The restorer trampoline, musl-style: when the handler returns to it, invoke
 * rt_sigreturn (173) to restore the interrupted context.
 */
extern void sig_restorer(void);
__asm__(".globl sig_restorer\n"
        "sig_restorer:\n"
        "    movl $173, %eax\n"
        "    int $0x80\n");

static volatile int handler_ran;
static volatile int handler_sig;
static volatile int forge_ran;

/* The saved trapframe inside the signal frame the kernel built: its first
 * word is the saved `gs` (struct isr_params in rach2/sys/isr.h), and the
 * frame begins four words before the handler's own return address, which is
 * what `&signo` stands one word past. */
#define SAVED_GS_FROM_ARG(argp) ((volatile unsigned *)((char *)(argp) + 3 * 4))

/* A selector with a ring-3 RPL whose index names no descriptor: the GDT has
 * seven entries, so index 8191 is past its limit. A kernel that loaded it
 * with `mov %ax, %gs` at ring 0 would take a general-protection fault in
 * kernel mode and panic. */
#define BAD_SELECTOR 0xFFFFu

static void usr1_handler(int sig)
{
    handler_ran = 1;
    handler_sig = sig;
}

static void forge_handler(int sig)
{
    forge_ran = 1;
    (void)sig;
    *SAVED_GS_FROM_ARG(&sig) = BAD_SELECTOR;
}

static void install(int signo, void (*h)(int))
{
    struct k_sigaction ka;

    ka.handler = h;
    ka.flags = SA_RESTORER;
    ka.restorer = sig_restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    check(syscall4(SYS_RT_SIGACTION, signo, (int)(size_t)&ka, 0, 8) == 0, "rt_sigaction");
}

void _start(void)
{
    int pid, status;
    unsigned set[2];

    /* 1. A handler runs on a raised signal, and execution resumes after it. */
    install(SIGUSR1, usr1_handler);
    handler_ran = 0;
    handler_sig = 0;
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(handler_ran == 1 && handler_sig == SIGUSR1, "SIGUSR1 handler ran and resumed");

    /* 2. A blocked signal stays pending until unblocked. */
    set[0] = 1u << (SIGUSR1 - 1);
    set[1] = 0;
    handler_ran = 0;
    check(syscall4(SYS_RT_SIGPROCMASK, SIG_BLOCK, (int)(size_t)set, 0, 8) == 0,
          "sigprocmask block");
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(handler_ran == 0, "blocked signal does not deliver");
    check(syscall4(SYS_RT_SIGPROCMASK, SIG_UNBLOCK, (int)(size_t)set, 0, 8) == 0,
          "sigprocmask unblock");
    check(handler_ran == 1, "unblocking delivers the pending signal");

    /* 3. Default action terminates a process that does not handle it. */
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        /* Child: spin so the parent's SIGTERM is delivered on a timer tick. */
        for (;;) {
            __asm__ __volatile__("" ::: "memory"); /* a spin, meant to last */
        }
    }
    syscall2(SYS_KILL, pid, SIGTERM);
    check(syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) == pid, "reap killed child");
    check((status & 0x7f) == SIGTERM, "child terminated by SIGTERM (WTERMSIG)");

    /* 4. A forged selector in the frame the kernel restores. The return path
       loads gs, fs, es and ds with `mov` while still at ring 0, so a
       selector past the GDT's limit would fault the kernel and not the
       process; the kernel must coerce it. Reaching the next statement at
       all is the proof, since the alternative is a machine that stops. */
    install(SIGUSR1, forge_handler);
    forge_ran = 0;
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(forge_ran == 1, "the forging handler ran");
    check(syscall1(SYS_GETPID, 0) > 0, "the machine survives a forged selector in the frame");

    if (fails == 0) {
        puts("SIGPROBE OK");
    } else {
        puts("SIGPROBE FAIL");
    }
    sys_exit(fails);
}
