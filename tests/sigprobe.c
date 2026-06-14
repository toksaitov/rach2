/*
 * sigprobe.c - Phase 8 signal probe for Rach2 OS.
 *
 * From user space via direct int 0x80 syscalls:
 *   - install a SIGUSR1 handler (with a tiny rt_sigreturn restorer), raise it to
 *     ourselves, and confirm the handler runs and execution resumes after it;
 *   - block SIGUSR1, raise it (stays pending, handler must NOT run), then unblock
 *     and confirm the now-pending signal is delivered;
 *   - fork a child that ignores SIGTERM and spins; SIGTERM it and confirm the
 *     default action terminates it (parent reaps WTERMSIG == SIGTERM).
 * Prints "SIGPROBE OK".
 *
 * Freestanding; direct int 0x80. Build:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 \
 *                -o sigprobe sigprobe.c
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

/* The restorer trampoline musl-style: when the handler returns to it, invoke
 * rt_sigreturn (173) to restore the interrupted context. */
extern void sig_restorer(void);
__asm__(".globl sig_restorer\n"
        "sig_restorer:\n"
        "    movl $173, %eax\n"
        "    int $0x80\n");

static volatile int handler_ran;
static volatile int handler_sig;

static void usr1_handler(int sig)
{
    handler_ran = 1;
    handler_sig = sig;
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

    /* 3. Default action terminates a process that does not handle the signal. */
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        /* Child: spin so the parent's SIGTERM is delivered on a timer tick. */
        for (;;) {
        }
    }
    syscall2(SYS_KILL, pid, SIGTERM);
    check(syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) == pid, "reap killed child");
    check((status & 0x7f) == SIGTERM, "child terminated by SIGTERM (WTERMSIG)");

    if (fails == 0) {
        puts("SIGPROBE OK");
    } else {
        puts("SIGPROBE FAIL");
    }
    sys_exit(fails);
}
