/**
 * The signal behaviour GNU libc and debuggers rely on, past what sigmusl
 * covers.
 *
 * A write to a read-only page reaches a SIGSEGV handler with SEGV_ACCERR and
 * the address, and runs again once the handler makes the page writable; a
 * read of an unmapped page gives SEGV_MAPERR. A read blocked on a pipe is
 * restarted after a handler with SA_RESTART and ends with EINTR after one
 * without. A handler with SA_ONSTACK runs on the alternate stack, where
 * sigaltstack reports SS_ONSTACK. `int3` reaches a SIGTRAP handler and the
 * program goes on after it. rt_sigpending reports a blocked signal, the old
 * sigprocmask call blocks one, sigsuspend's handler runs under the
 * suspend's mask and the old mask comes back after it, and a child's exit
 * reaches a SIGCHLD handler as CLD_EXITED with its pid and status. A handler
 * that nests on a small alternate stack until its frames no longer fit ends
 * the process with SIGSEGV and writes nothing below the stack.
 *
 * Prints "SIGFRAMEMUSL OK" on success and "SIGFRAMEMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got_code;
static volatile uintptr_t got_addr;
static volatile sig_atomic_t on_alt;
static volatile sig_atomic_t trapped;
static volatile sig_atomic_t handler_mask_had_usr1;
static volatile sig_atomic_t chld_code;
static volatile sig_atomic_t chld_pid;
static volatile sig_atomic_t chld_status;
static char *page;
static sigjmp_buf escape;
static char altstack[16384];

static int fail(const char *what)
{
    printf("SIGFRAMEMUSL FAIL (%s)\n", what);
    return 1;
}

static void on_segv(int sig, siginfo_t *info, void *uc)
{
    (void)sig;
    (void)uc;
    got_code = info->si_code;
    got_addr = (uintptr_t)info->si_addr;
    if (info->si_code == SEGV_ACCERR) {
        mprotect(page, 4096, PROT_READ | PROT_WRITE);
    } else {
        siglongjmp(escape, 1);
    }
}

static void on_usr1(int sig)
{
    (void)sig;
}

static void on_alt_handler(int sig)
{
    stack_t st;
    char here;

    (void)sig;
    on_alt = (uintptr_t)&here - (uintptr_t)altstack < sizeof(altstack) &&
             sigaltstack(NULL, &st) == 0 && (st.ss_flags & SS_ONSTACK);
}

static void on_trap(int sig, siginfo_t *info, void *uc)
{
    (void)sig;
    (void)uc;
    trapped = info->si_code;
}

static void on_usr2(int sig, siginfo_t *info, void *context)
{
    sigset_t now;

    (void)sig;
    (void)info;
    (void)context;
    sigprocmask(SIG_BLOCK, NULL, &now);
    handler_mask_had_usr1 = sigismember(&now, SIGUSR1);
}

static void on_chld(int sig, siginfo_t *info, void *uc)
{
    (void)sig;
    (void)uc;
    chld_code = info->si_code;
    chld_pid = info->si_pid;
    chld_status = info->si_status;
}

static void nest(int sig)
{
    raise(sig);
}

/** A child whose SIGUSR1 handler raises SIGUSR1 again, SA_NODEFER, on an
 * alternate stack of MINSIGSTKSZ bytes at the start of a shared page, with
 * the page below it filled with a mark the kernel must leave. */
static int overflow(void)
{
    char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int status, i;
    pid_t pid;

    if (m == MAP_FAILED) {
        return fail("the pages for the alternate stack");
    }
    memset(m, 0x5a, 4096);
    pid = fork();
    if (pid == 0) {
        stack_t ss;
        struct sigaction sa;

        ss.ss_sp = m + 4096;
        ss.ss_size = MINSIGSTKSZ;
        ss.ss_flags = 0;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = nest;
        sa.sa_flags = SA_ONSTACK | SA_NODEFER;
        if (sigaltstack(&ss, NULL) != 0 || sigaction(SIGUSR1, &sa, NULL) != 0) {
            _exit(2);
        }
        raise(SIGUSR1);
        _exit(3);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGSEGV) {
        return fail("a handler that overflows its alternate stack");
    }
    for (i = 0; i < 4096; i++) {
        if (m[i] != 0x5a) {
            return fail("a frame written below the alternate stack");
        }
    }
    munmap(m, 8192);
    return 0;
}

static int install(int sig, void (*fn)(int, siginfo_t *, void *), int flags)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

/* A child that sends SIGUSR1 to its parent and, a moment later, writes one
 * byte to `fd`; the parent's read of the other end is what the signal cuts. */
static pid_t signal_then_write(int fd)
{
    struct timespec pause = {0, 100000000};
    pid_t pid = fork();

    if (pid == 0) {
        nanosleep(&pause, NULL);
        kill(getppid(), SIGUSR1);
        nanosleep(&pause, NULL);
        (void)write(fd, "x", 1);
        _exit(0);
    }
    return pid;
}

/* A blocking read on a pipe cut by SIGUSR1: 1 when the kernel restarted the
 * read, -1 with EINTR when it did not. */
static int interrupted_read(int restart)
{
    struct sigaction sa;
    int fds[2];
    char c;
    int n;
    pid_t pid;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sa.sa_flags = restart ? SA_RESTART : 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    if (pipe(fds) != 0) {
        return -2;
    }
    pid = signal_then_write(fds[1]);
    n = (int)read(fds[0], &c, 1);
    if (n < 0 && errno != EINTR) {
        n = -2;
    }
    waitpid(pid, NULL, 0);
    close(fds[0]);
    close(fds[1]);
    return n;
}

int main(void)
{
    struct sigaction sa;
    sigset_t block, pending, empty, now;
    stack_t st;
    volatile char *unmapped = (volatile char *)0x00001000;
    uint32_t old = 0;
    uint32_t set;
    pid_t pid;

    page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || install(SIGSEGV, on_segv, 0) != 0) {
        return fail("setup");
    }
    /* Through a volatile pointer, so the compiler cannot read got_code before
     * the store that faults. */
    ((volatile char *)page)[10] = 'w';
    if (got_code != SEGV_ACCERR || got_addr != (uintptr_t)page + 10 ||
        ((volatile char *)page)[10] != 'w') {
        return fail("the write to a read-only page was not handled and run again");
    }
    if (sigsetjmp(escape, 1) == 0) {
        (void)*unmapped;
        return fail("the read of an unmapped page went through");
    }
    if (got_code != SEGV_MAPERR || got_addr != (uintptr_t)unmapped) {
        return fail("SEGV_MAPERR or its address");
    }

    if (interrupted_read(1) != 1) {
        return fail("SA_RESTART did not restart the read");
    }
    if (interrupted_read(0) != -1) {
        return fail("a read cut by a handler without SA_RESTART did not end with EINTR");
    }

    st.ss_sp = altstack;
    st.ss_size = sizeof(altstack);
    st.ss_flags = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alt_handler;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaltstack(&st, NULL) != 0 || sigaction(SIGUSR1, &sa, NULL) != 0) {
        return fail("sigaltstack");
    }
    raise(SIGUSR1);
    if (!on_alt) {
        return fail("the SA_ONSTACK handler was not on the alternate stack");
    }

    if (install(SIGTRAP, on_trap, 0) != 0) {
        return fail("sigaction SIGTRAP");
    }
    __asm__ __volatile__("int3" : : : "memory");
    if (trapped != SI_KERNEL) {
        return fail("int3 did not reach the SIGTRAP handler as SI_KERNEL");
    }

    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, NULL);
    raise(SIGUSR2);
    if (syscall(SYS_rt_sigpending, &pending, 8) != 0 || !sigismember(&pending, SIGUSR2)) {
        return fail("rt_sigpending did not report the blocked signal");
    }
    if (install(SIGUSR2, on_usr2, 0) != 0) {
        return fail("sigaction SIGUSR2");
    }
    set = 1u << (SIGUSR1 - 1);
    if (syscall(SYS_sigprocmask, SIG_BLOCK, &set, &old) != 0) {
        return fail("the old sigprocmask call");
    }
    sigprocmask(SIG_BLOCK, NULL, &now);
    if (!sigismember(&now, SIGUSR1)) {
        return fail("the old sigprocmask call did not block the signal");
    }
    sigemptyset(&empty);
    sigsuspend(&empty);
    if (handler_mask_had_usr1 != 0) {
        return fail("the handler did not run under the suspend's mask");
    }
    sigprocmask(SIG_BLOCK, NULL, &now);
    if (!sigismember(&now, SIGUSR1) || !sigismember(&now, SIGUSR2)) {
        return fail("the mask from before sigsuspend did not come back");
    }

    if (install(SIGCHLD, on_chld, 0) != 0) {
        return fail("sigaction SIGCHLD");
    }
    pid = fork();
    if (pid == 0) {
        _exit(7);
    }
    waitpid(pid, NULL, 0);
    if (chld_code != CLD_EXITED || chld_pid != pid || chld_status != 7) {
        return fail("SIGCHLD's siginfo");
    }
    if (overflow() != 0) {
        return 1;
    }

    printf("SIGFRAMEMUSL OK\n");
    return 0;
}
