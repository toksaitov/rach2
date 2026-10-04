/**
 * The signal behaviour GNU libc and debuggers rely on, past what sigmusl
 * covers.
 *
 * A write to a read-only page reaches a SIGSEGV handler with SEGV_ACCERR and
 * the address, and runs again once the handler makes the page writable; a
 * read of an unmapped page gives SEGV_MAPERR. A read blocked on a pipe, as a
 * child sees before it sends SIGUSR1, is restarted after a handler with
 * SA_RESTART and ends with EINTR after one without. Each handler runs once
 * and answers the child, which writes the byte the restarted read returns
 * only after the answer. A handler with SA_ONSTACK runs on the alternate
 * stack, where sigaltstack reports SS_ONSTACK, and its frame saves the
 * flags of the normal stack the signal came on, 0. `int3` reaches a SIGTRAP
 * handler and the program goes on after it. rt_sigpending reports a blocked
 * signal, and the old sigprocmask call blocks one, or on x86-64, which has no
 * old call, rt_sigprocmask of one word. sigsuspend returns -1 with EINTR once
 * the pending signal's handler has run under the suspend's mask, and the old
 * mask comes back after it. A child's exit reaches a SIGCHLD handler as
 * CLD_EXITED with its pid and status. A handler that nests on a small alternate
 * stack until its frames no longer fit ends the process with SIGSEGV and writes
 * nothing below the stack.
 *
 * Prints "SIGFRAMEMUSL OK" on success and "SIGFRAMEMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * A mask of one word, which blocks signals by number through BLOCK_RAW: the
 * old sigprocmask's 32 bits on i386, and on x86-64, which has no old call,
 * the 64 bits of rt_sigprocmask's whole mask.
 */
#if defined SYS_sigprocmask
typedef uint32_t word_mask_t;
#define BLOCK_RAW(set, old) syscall(SYS_sigprocmask, SIG_BLOCK, set, old)
#else
typedef uint64_t word_mask_t;
#define BLOCK_RAW(set, old) syscall(SYS_rt_sigprocmask, SIG_BLOCK, set, old, sizeof(word_mask_t))
#endif

static volatile sig_atomic_t usr1_runs;
static int usr1_answer = -1;
static volatile sig_atomic_t got_code;
static volatile uintptr_t got_addr;
static volatile sig_atomic_t on_alt;
static volatile int saved_ss_flags;
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

/** Count a SIGUSR1, and answer it with a byte on `usr1_answer` when that is
 * open. */
static void on_usr1(int sig)
{
    int saved = errno;

    (void)sig;
    usr1_runs++;
    if (usr1_answer >= 0) {
        (void)write(usr1_answer, "h", 1);
    }
    errno = saved;
}

static void on_alt_handler(int sig, siginfo_t *info, void *ctx)
{
    stack_t st;
    char here;

    (void)sig;
    (void)info;
    on_alt = (uintptr_t)&here - (uintptr_t)altstack < sizeof(altstack) &&
             sigaltstack(NULL, &st) == 0 && (st.ss_flags & SS_ONSTACK);
    saved_ss_flags = ((ucontext_t *)ctx)->uc_stack.ss_flags;
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

/**
 * sigsuspend with an empty mask while SIGUSR1 is blocked and SIGUSR2 blocked
 * and pending: it must return -1 with EINTR once `on_usr2` has run, under
 * the suspend's mask, which lacks SIGUSR1. The handler's report starts at -1,
 * which only the handler overwrites. Returns 0, or 1 once the failure is
 * reported.
 */
static int suspend_row(void)
{
    sigset_t empty;

    sigemptyset(&empty);
    handler_mask_had_usr1 = -1;
    if (sigsuspend(&empty) != -1 || errno != EINTR) {
        return fail("sigsuspend did not return -1 with EINTR");
    }
    if (handler_mask_had_usr1 == -1) {
        return fail("sigsuspend's handler did not run");
    }
    if (handler_mask_had_usr1 != 0) {
        return fail("the handler did not run under the suspend's mask");
    }
    return 0;
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
 * the page below it filled with a mark the kernel must leave. The child
 * clears the mask it inherits, in which the rows before block SIGUSR1. */
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
        sigset_t none;

        sigemptyset(&none);
        ss.ss_sp = m + 4096;
        ss.ss_size = MINSIGSTKSZ;
        ss.ss_flags = 0;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = nest;
        sa.sa_flags = SA_ONSTACK | SA_NODEFER;
        if (sigaltstack(&ss, NULL) != 0 || sigaction(SIGUSR1, &sa, NULL) != 0 ||
            sigprocmask(SIG_SETMASK, &none, NULL) != 0) {
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

/** Whether process `pid` sleeps, by its state in /proc/<pid>/stat. */
static int sleeping(pid_t pid)
{
    char path[64], line[256], *state;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    state = strrchr(line, ')');
    return state && state[1] == ' ' && state[2] == 'S';
}

/**
 * Fork a child that waits up to 3 s to see this process asleep, sends it
 * SIGUSR1, waits up to 3 s for the handler's byte on `answer`, and only then
 * writes one byte to `fd`, the pipe the parent reads. The child exits 0 when
 * all of that happened, 2 when the parent was never seen asleep and 3 when
 * the handler's byte did not come; it writes the byte in every case, so the
 * read ends.
 */
static pid_t signal_then_write(int fd, int answer)
{
    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        struct pollfd pfd;
        int tries, code = 0;
        char c;

        for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
            usleep(10000);
        }
        if (tries == 300) {
            code = 2;
        } else {
            pfd.fd = answer;
            pfd.events = POLLIN;
            if (kill(parent, SIGUSR1) != 0 || poll(&pfd, 1, 3000) != 1 ||
                read(answer, &c, 1) != 1) {
                code = 3;
            }
        }
        (void)write(fd, "x", 1);
        _exit(code);
    }
    return pid;
}

/**
 * A blocking read on a pipe cut by SIGUSR1, whose handler has SA_RESTART
 * when `restart` is set. The handler must run once; the read must then go on
 * to return the byte the child writes after the handler answered, or end
 * with EINTR without SA_RESTART. Returns 0, or 1 once the failure is
 * reported.
 */
static int interrupted_read(int restart)
{
    struct sigaction sa;
    int fds[2], answer[2];
    int n, err, status;
    char c;
    pid_t pid;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sa.sa_flags = restart ? SA_RESTART : 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || pipe(fds) != 0 || pipe(answer) != 0) {
        return fail("setup for the interrupted read");
    }
    usr1_runs = 0;
    usr1_answer = answer[1];
    pid = signal_then_write(fds[1], answer[0]);
    if (pid < 0) {
        return fail("fork for the interrupted read");
    }
    n = (int)read(fds[0], &c, 1);
    err = errno;
    usr1_answer = -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("the child that signals and writes");
    }
    close(fds[0]);
    close(fds[1]);
    close(answer[0]);
    close(answer[1]);
    if (WEXITSTATUS(status) == 2) {
        return fail("the read was never seen asleep");
    }
    if (usr1_runs != 1 || WEXITSTATUS(status) != 0) {
        return fail(restart ? "the handler with SA_RESTART did not run once"
                            : "the handler without SA_RESTART did not run once");
    }
    if (restart && n != 1) {
        return fail("SA_RESTART did not restart the read");
    }
    if (!restart && (n != -1 || err != EINTR)) {
        return fail("a read cut by a handler without SA_RESTART did not end with EINTR");
    }
    return 0;
}

/** A write to a read-only page is handled as SEGV_ACCERR at its address
 * and run again; a read of an unmapped page is SEGV_MAPERR at its own. */
static int fault_rows(void)
{
    volatile char *unmapped = (volatile char *)0x00001000;

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
    return 0;
}

/** A handler installed with SA_ONSTACK runs on the alternate stack, and its
 * frame saves the flags of the stack the signal came on. */
static int alt_stack_row(void)
{
    struct sigaction sa;
    stack_t st;

    st.ss_sp = altstack;
    st.ss_size = sizeof(altstack);
    st.ss_flags = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_alt_handler;
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaltstack(&st, NULL) != 0 || sigaction(SIGUSR1, &sa, NULL) != 0) {
        return fail("sigaltstack");
    }
    saved_ss_flags = -1;
    raise(SIGUSR1);
    if (!on_alt) {
        return fail("the SA_ONSTACK handler was not on the alternate stack");
    }
    if (saved_ss_flags != 0) {
        return fail("the frame saved SS_ONSTACK for a signal that came on the normal stack");
    }
    return 0;
}

int main(void)
{
    sigset_t block, pending, now;
    word_mask_t old = 0;
    word_mask_t set;
    pid_t pid;

    if (fault_rows() != 0 || interrupted_read(1) != 0 || interrupted_read(0) != 0 ||
        alt_stack_row() != 0) {
        return 1;
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
    if (BLOCK_RAW(&set, &old) != 0) {
        return fail("the old sigprocmask call");
    }
    sigprocmask(SIG_BLOCK, NULL, &now);
    if (!sigismember(&now, SIGUSR1)) {
        return fail("the old sigprocmask call did not block the signal");
    }
    if (suspend_row() != 0) {
        return 1;
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
