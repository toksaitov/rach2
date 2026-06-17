/*
 * sigmusl.c - Phase 8 integration milestone: signals via musl.
 *
 * An ordinary hosted C program linked against the ported musl. It installs a
 * plain SIGUSR1 handler with sigaction (sa_flags=0, so musl wires up SA_RESTORER
 * -> __restore -> the legacy sigreturn(119) trampoline, the common path for a
 * handler that takes no siginfo), raises the signal to itself and confirms the
 * handler ran and control resumed after it; blocks SIGUSR1 with sigprocmask, raises it (it
 * must stay pending, not run), then unblocks and confirms the pending signal is
 * delivered; finally forks a child that abort()s and reaps it, checking the
 * kernel killed it with SIGABRT (WIFSIGNALED / WTERMSIG). Printing "SIGMUSL OK"
 * means the whole Phase 8 path worked through libc: sigaction, the signal mask,
 * synchronous delivery on syscall return, musl's restorer trampoline, raise and
 * abort, and the wait-status signal encoding.
 *
 * Linked statically against the ported musl; see tests/musl/build.sh.
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>

static volatile sig_atomic_t handler_ran;
static volatile sig_atomic_t handler_sig;

static void usr1_handler(int sig)
{
    handler_ran = 1;
    handler_sig = sig;
}

int main(void)
{
    struct sigaction sa;
    sigset_t mask;
    pid_t pid;
    int status = 0;

    /* 1. A handler installed via sigaction runs on a raised signal, and control
     * resumes after it (musl's __restore returns through the legacy sigreturn). */
    sa.sa_handler = usr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
    {
        printf("SIGMUSL FAIL: sigaction\n");
        return 1;
    }

    handler_ran = 0;
    handler_sig = 0;
    if (raise(SIGUSR1) != 0)
    {
        printf("SIGMUSL FAIL: raise\n");
        return 1;
    }
    if (handler_ran != 1 || handler_sig != SIGUSR1)
    {
        printf("SIGMUSL FAIL: handler did not run (ran=%d sig=%d)\n", (int)handler_ran,
               (int)handler_sig);
        return 1;
    }

    /* 2. A blocked signal stays pending until it is unblocked. */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
    {
        printf("SIGMUSL FAIL: sigprocmask block\n");
        return 1;
    }
    handler_ran = 0;
    handler_sig = 0;
    raise(SIGUSR1);
    if (handler_ran != 0)
    {
        printf("SIGMUSL FAIL: blocked signal delivered\n");
        return 1;
    }
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0)
    {
        printf("SIGMUSL FAIL: sigprocmask unblock\n");
        return 1;
    }
    if (handler_ran != 1 || handler_sig != SIGUSR1)
    {
        printf("SIGMUSL FAIL: pending signal not delivered on unblock (ran=%d sig=%d)\n",
               (int)handler_ran, (int)handler_sig);
        return 1;
    }

    /* 3. The default action terminates: a child that abort()s with no handler
     * must be reaped as killed by SIGABRT. */
    pid = fork();
    if (pid < 0)
    {
        printf("SIGMUSL FAIL: fork\n");
        return 1;
    }
    if (pid == 0)
    {
        abort();
        _exit(0); /* not reached */
    }

    if (waitpid(pid, &status, 0) != pid)
    {
        printf("SIGMUSL FAIL: waitpid\n");
        return 1;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT)
    {
        printf("SIGMUSL FAIL: child not killed by SIGABRT (status=0x%x)\n", status);
        return 1;
    }

    printf("SIGMUSL OK (handler ran for sig %d, child killed by SIGABRT %d)\n", handler_sig,
           SIGABRT);
    return 0;
}
