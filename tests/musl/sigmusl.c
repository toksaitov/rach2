/**
 * Exercises the signal path through the ported musl, end to end.
 *
 * An ordinary hosted C program that drives four contracts. First, a plain
 * SIGUSR1 handler installed with sigaction (sa_flags=0, so musl wires up
 * SA_RESTORER -> __restore -> the legacy sigreturn(119) trampoline, the common
 * path for a handler that takes no siginfo) runs when the signal is raised to
 * self, and control resumes after it. Second, a signal blocked with sigprocmask
 * stays pending instead of running, and is delivered once unblocked. Third,
 * the default action terminates: a child that abort()s with no handler is
 * reaped as killed by SIGABRT (WIFSIGNALED / WTERMSIG). Fourth, an SA_SIGINFO
 * handler finds the interrupted context in Linux's layout: the user code
 * selector and a stack pointer in the saved registers, the floating-point
 * state's address, the mask from before the handler, and a siginfo that names
 * raise's tkill and its sender. A handler that finds the user code selector,
 * the stack pointer or the floating-point state missing reports the context
 * and ends the program itself, since a return through that frame would
 * restore no registers and fault before any report.
 *
 * "SIGMUSL OK" means the whole path worked through libc: sigaction, the signal
 * mask, synchronous delivery on syscall return, musl's restorer trampoline,
 * raise and abort, and the wait-status signal encoding. Any "SIGMUSL FAIL"
 * line names the step that failed.
 *
 * Linked statically against the ported musl; see tests/musl/build.sh.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <ucontext.h>

static volatile sig_atomic_t handler_ran;
static volatile sig_atomic_t handler_sig;
static volatile sig_atomic_t info_signo;
static volatile sig_atomic_t info_code;
static volatile sig_atomic_t info_pid;
static volatile unsigned long context_cs;
static volatile unsigned long context_esp;
static volatile unsigned long context_fpregs;
static volatile int context_mask_has_usr1;

/** Record delivery to the plain handler. */
static void usr1_handler(int sig)
{
    handler_ran = 1;
    handler_sig = sig;
}

/** Write the context's report, as `run_context` prints it, and end the
 * program: what a handler does with a frame it cannot return through. */
static void context_fail(void)
{
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "SIGMUSL FAIL: ucontext cs=0x%lx esp=0x%lx fpregs=0x%lx usr1-masked=%d\n",
                     context_cs, context_esp, context_fpregs, context_mask_has_usr1);

    if (n > 0) {
        (void)write(1, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    _exit(1);
}

/** Record the siginfo and the parts of the context the kernel fills, and end
 * the program with the report when the registers a return restores are not
 * there. */
static void info_handler(int sig, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;

    handler_ran = 1;
    handler_sig = sig;
    info_signo = info->si_signo;
    info_code = info->si_code;
    info_pid = info->si_pid;
    context_cs = (unsigned long)uc->uc_mcontext.gregs[REG_CS] & 0xFFFFu;
    context_esp = (unsigned long)uc->uc_mcontext.gregs[REG_ESP];
    context_fpregs = (unsigned long)uc->uc_mcontext.fpregs;
    context_mask_has_usr1 = sigismember(&uc->uc_sigmask, SIGUSR1);
    if (context_cs != 0x1B || context_esp == 0 || context_fpregs == 0) {
        context_fail();
    }
}

/** Check musl's SA_SIGINFO arguments and return through __restore_rt. */
static int run_context(void)
{
    struct sigaction sa = {0};

    sa.sa_sigaction = info_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        printf("SIGMUSL FAIL: sigaction siginfo\n");
        return 1;
    }
    handler_ran = 0;
    handler_sig = 0;
    if (raise(SIGUSR1) != 0 || handler_ran != 1 || handler_sig != SIGUSR1 ||
        info_signo != SIGUSR1) {
        printf("SIGMUSL FAIL: siginfo delivery\n");
        return 1;
    }
    /* raise() is tkill on the caller, so the siginfo says SI_TKILL (-6). */
    if (info_code != -6 || info_pid != getpid()) {
        printf("SIGMUSL FAIL: siginfo code=%d pid=%d\n", (int)info_code, (int)info_pid);
        return 1;
    }
    if (context_cs != 0x1B || context_esp == 0 || context_fpregs == 0 || context_mask_has_usr1) {
        printf("SIGMUSL FAIL: ucontext cs=0x%lx esp=0x%lx fpregs=0x%lx usr1-masked=%d\n",
               context_cs, context_esp, context_fpregs, context_mask_has_usr1);
        return 1;
    }
    return 0;
}

/** Check delivery, masking, termination and the full context argument. */
int main(void)
{
    struct sigaction sa;
    sigset_t mask;
    pid_t pid;
    int status = 0;

    /* Handler runs on a raised signal, and control resumes after it. */
    sa.sa_handler = usr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        printf("SIGMUSL FAIL: sigaction\n");
        return 1;
    }

    handler_ran = 0;
    handler_sig = 0;
    if (raise(SIGUSR1) != 0) {
        printf("SIGMUSL FAIL: raise\n");
        return 1;
    }
    if (handler_ran != 1 || handler_sig != SIGUSR1) {
        printf("SIGMUSL FAIL: handler did not run (ran=%d sig=%d)\n", (int)handler_ran,
               (int)handler_sig);
        return 1;
    }

    /* Blocked signal stays pending until unblocked. */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        printf("SIGMUSL FAIL: sigprocmask block\n");
        return 1;
    }
    handler_ran = 0;
    handler_sig = 0;
    raise(SIGUSR1);
    if (handler_ran != 0) {
        printf("SIGMUSL FAIL: blocked signal delivered\n");
        return 1;
    }
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
        printf("SIGMUSL FAIL: sigprocmask unblock\n");
        return 1;
    }
    if (handler_ran != 1 || handler_sig != SIGUSR1) {
        printf("SIGMUSL FAIL: pending signal not delivered on unblock (ran=%d sig=%d)\n",
               (int)handler_ran, (int)handler_sig);
        return 1;
    }

    /* Default action terminates: an abort()ing child is killed by SIGABRT. */
    pid = fork();
    if (pid < 0) {
        printf("SIGMUSL FAIL: fork\n");
        return 1;
    }
    if (pid == 0) {
        abort();
        _exit(0); /* not reached */
    }

    if (waitpid(pid, &status, 0) != pid) {
        printf("SIGMUSL FAIL: waitpid\n");
        return 1;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        printf("SIGMUSL FAIL: child not killed by SIGABRT (status=0x%x)\n", status);
        return 1;
    }

    if (run_context() != 0) {
        return 1;
    }
    printf("SIGMUSL OK (handler ran for sig %d, child killed by SIGABRT %d, context %u bytes)\n",
           handler_sig, SIGABRT, (unsigned)sizeof(ucontext_t));
    return 0;
}
