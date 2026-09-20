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
 * handler reads every byte of musl's ucontext_t, which Rach2 supplies as a
 * zeroed placeholder separate from the saved floating-point image.
 *
 * "SIGMUSL OK" means the whole path worked through libc: sigaction, the signal
 * mask, synchronous delivery on syscall return, musl's restorer trampoline,
 * raise and abort, and the wait-status signal encoding. Any "SIGMUSL FAIL"
 * line names the step that failed.
 *
 * Linked statically against the ported musl; see tests/musl/build.sh.
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <ucontext.h>

static volatile sig_atomic_t handler_ran;
static volatile sig_atomic_t handler_sig;
static volatile sig_atomic_t context_bytes;
static volatile sig_atomic_t context_nonzero;
static volatile sig_atomic_t info_signo;

/** Record delivery to the plain handler. */
static void usr1_handler(int sig)
{
    handler_ran = 1;
    handler_sig = sig;
}

/** Read the entire context, including its signal mask and FP storage. */
static void info_handler(int sig, siginfo_t *info, void *context)
{
    const volatile unsigned char *bytes = context;
    size_t i;

    handler_ran = 1;
    handler_sig = sig;
    info_signo = info->si_signo;
    for (i = 0; i < sizeof(ucontext_t); i++) {
        if (bytes[i] != 0 && context_nonzero == 0) {
            context_nonzero = (sig_atomic_t)(i + 1);
        }
        context_bytes++;
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
    if ((size_t)context_bytes != sizeof(ucontext_t) || context_nonzero != 0) {
        printf("SIGMUSL FAIL: ucontext bytes=%d size=%u first nonzero=%d\n", (int)context_bytes,
               (unsigned)sizeof(ucontext_t), (int)context_nonzero - 1);
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
