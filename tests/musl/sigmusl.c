/**
 * Exercises the signal path through the ported musl, end to end.
 *
 * An ordinary hosted C program that drives three contracts. First, a plain
 * SIGUSR1 handler installed with sigaction (sa_flags=0, so musl wires up
 * SA_RESTORER -> __restore -> the legacy sigreturn(119) trampoline, the common
 * path for a handler that takes no siginfo) runs when the signal is raised to
 * self, and control resumes after it. Second, a signal blocked with sigprocmask
 * stays pending instead of running, and is delivered once unblocked. Third,
 * the default action terminates: a child that abort()s with no handler is
 * reaped as killed by SIGABRT (WIFSIGNALED / WTERMSIG).
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

    printf("SIGMUSL OK (handler ran for sig %d, child killed by SIGABRT %d)\n", handler_sig,
           SIGABRT);
    return 0;
}
