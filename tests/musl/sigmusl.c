/**
 * sigmusl: check signal delivery through the ported musl, end to end.
 *
 * It checks four things. First, a plain SIGUSR1 handler installed with
 * sigaction and no flags runs when the signal is raised to the process, and
 * control resumes after it. musl gives such a handler the restorer
 * `__restore`, which makes the legacy sigreturn (119) on i386; x86-64 has no
 * legacy sigreturn, and there musl's `__restore_rt` makes rt_sigreturn (15).
 * Second, a signal blocked with sigprocmask stays pending instead of running,
 * and is delivered once unblocked. Third, the default action terminates: a
 * child that calls abort with no handler is reaped as killed by SIGABRT.
 * Fourth, an SA_SIGINFO handler finds the interrupted context in Linux's
 * layout: the user code selector and a stack pointer in the saved registers,
 * the floating-point state's address, the mask from before the handler, and a
 * siginfo with the code of raise's tkill and the sender's pid. A handler that
 * finds the user code selector, the stack pointer or the floating-point state
 * missing prints the context and ends the program itself, since a return
 * through that frame would restore no registers and fault before any report.
 *
 * "SIGMUSL OK" means each of these worked through libc: sigaction, the signal
 * mask, delivery on the return from a system call, musl's restorer, raise and
 * abort, and the signal encoding of the wait status. A "SIGMUSL FAIL" line
 * names the step that failed, and the program exits 1. It is linked
 * statically against the ported musl; see `tests/musl/build.sh`.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <ucontext.h>

/*
 * The saved registers a handler checks, by each architecture's index into
 * gregs: the code segment, whose user selector is 0x1B on i386 and 0x33 on
 * x86-64, where it is the low 16 bits of the word that also has gs and fs,
 * and the stack pointer.
 */
#if defined __x86_64__
#define CTX_CS  REG_CSGSFS
#define CTX_SP  REG_RSP
#define SP_NAME "rsp"
#define USER_CS 0x33
#else
#define CTX_CS  REG_CS
#define CTX_SP  REG_ESP
#define SP_NAME "esp"
#define USER_CS 0x1B
#endif

/* What the handlers saw; context_cs is the low 16 bits of the saved CS. */
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

/**
 * Write the context's report with write(2), as `run_context` prints it, and
 * end the program with status 1. The handler calls it for a frame it cannot
 * return through.
 */
static void context_fail(void)
{
    char line[128];
    int n =
        snprintf(line, sizeof(line),
                 "SIGMUSL FAIL: ucontext cs=0x%lx " SP_NAME "=0x%lx fpregs=0x%lx usr1-masked=%d\n",
                 context_cs, context_esp, context_fpregs, context_mask_has_usr1);

    if (n > 0) {
        (void)write(1, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    _exit(1);
}

/**
 * Record the siginfo and the parts of the context the kernel fills, and end
 * the program with the report when the registers a return restores are not
 * there.
 */
static void info_handler(int sig, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;

    handler_ran = 1;
    handler_sig = sig;
    info_signo = info->si_signo;
    info_code = info->si_code;
    info_pid = info->si_pid;
    context_cs = (unsigned long)uc->uc_mcontext.gregs[CTX_CS] & 0xFFFFu;
    context_esp = (unsigned long)uc->uc_mcontext.gregs[CTX_SP];
    context_fpregs = (unsigned long)uc->uc_mcontext.fpregs;
    context_mask_has_usr1 = sigismember(&uc->uc_sigmask, SIGUSR1);
    if (context_cs != USER_CS || context_esp == 0 || context_fpregs == 0) {
        context_fail();
    }
}

/**
 * Check musl's SA_SIGINFO arguments and the return through `__restore_rt`.
 * Returns 0, or 1 after the failure is printed.
 */
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
    if (context_cs != USER_CS || context_esp == 0 || context_fpregs == 0 || context_mask_has_usr1) {
        printf("SIGMUSL FAIL: ucontext cs=0x%lx " SP_NAME "=0x%lx fpregs=0x%lx usr1-masked=%d\n",
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
