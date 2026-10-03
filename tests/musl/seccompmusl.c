/**
 * seccomp filters, through musl's syscall(), prctl() and pthreads.
 *
 * The probe starts with no filter and mode 0. It refuses an empty program,
 * a jump past the end, a load past struct seccomp_data, and a filter from
 * an unprivileged caller without no_new_privs; it reports the actions it
 * has, and not SECCOMP_RET_USER_NOTIF.
 *
 * In a child with no_new_privs, a filter fails getppid with errno 77 and
 * lets the rest run, and /proc/self/status shows mode 2 and one filter. A
 * second filter traps getuid32 with data 5: the handler's siginfo has
 * SYS_SECCOMP, the call, the instruction after it, AUDIT_ARCH_I386 and the
 * data in si_errno, and EAX holds the call's number, as Linux's rollback
 * leaves it. Both filters judge getppid, and the trap, of higher
 * precedence, wins over the errno. A grandchild inherits both and adds a
 * third that kills the process at getgid32, and dies by SIGSYS. On x86-64
 * the calls are getuid and getgid, the architecture AUDIT_ARCH_X86_64 and the
 * register RAX. TSYNC gives
 * a waiting thread the caller's new filter, and an exec keeps the four.
 * The strict mode lets write through and kills the process with SIGKILL at
 * getpid.
 *
 * Prints "SECCOMPMUSL OK" on success and "SECCOMPMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define SET_MODE_STRICT  0
#define SET_MODE_FILTER  1
#define GET_ACTION_AVAIL 2
#define FLAG_TSYNC       1

#define RET_KILL_PROCESS 0x80000000u
#define RET_TRAP         0x00030000u
#define RET_ERRNO        0x00050000u
#define RET_USER_NOTIF   0x7fc00000u
#define RET_ALLOW        0x7fff0000u

#define LD_W_ABS 0x20
#define JEQ_K    0x15
#define RET_K    0x06

/*
 * The calls the filters judge, the architecture a trap's siginfo names, and
 * the saved register a trap leaves the call's number in: i386's calls of
 * 32-bit ids, getuid32 and getgid32, are x86-64's getuid and getgid.
 */
#define NR_SECCOMP SYS_seccomp
#define NR_GETPPID SYS_getppid
#define NR_GETTID  SYS_gettid
#if defined __x86_64__
#define NR_GETUID       SYS_getuid
#define NR_GETGID       SYS_getgid
#define AUDIT_ARCH_SELF 0xC000003Eu /* AUDIT_ARCH_X86_64 */
#define REG_NR          REG_RAX
#else
#define NR_GETUID       SYS_getuid32
#define NR_GETGID       SYS_getgid32
#define AUDIT_ARCH_SELF 0x40000003u /* AUDIT_ARCH_I386 */
#define REG_NR          REG_EAX
#endif

struct insn {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};

struct fprog {
    uint16_t len;
    struct insn *filter;
};

static volatile int trapped;
static volatile int trap_ok;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("SECCOMPMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Install the filter `n` instructions at `prog` describe, with `flags`. */
static long install(struct insn *prog, unsigned short n, unsigned flags)
{
    struct fprog fp;

    fp.len = n;
    fp.filter = prog;
    return syscall(NR_SECCOMP, SET_MODE_FILTER, flags, &fp);
}

/** Install a filter that answers the call `nr` with `action` and lets the
 * rest run. */
static long answer(int nr, uint32_t action, unsigned flags)
{
    struct insn prog[4] = {
        {LD_W_ABS, 0, 0, 0},
        {JEQ_K, 0, 1, (uint32_t)nr},
        {RET_K, 0, 0, action},
        {RET_K, 0, 0, RET_ALLOW},
    };

    return install(prog, 4, flags);
}

/** Whether /proc/self/status has the line `line`. */
static int status_has(const char *line)
{
    char buf[4096];
    FILE *f = fopen("/proc/self/status", "r");
    size_t n;

    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, line) != NULL;
}

/** The handler of the trap: the siginfo and EAX must be the call's. */
static void on_sigsys(int signo, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;

    (void)signo;
    trapped++;
    trap_ok = si->si_code == 1 && si->si_errno == 5 && si->si_syscall == NR_GETUID &&
              si->si_arch == AUDIT_ARCH_SELF && si->si_call_addr != NULL &&
              uc->uc_mcontext.gregs[REG_NR] == NR_GETUID;
    uc->uc_mcontext.gregs[REG_NR] = 4242;
}

/** The refusals and the queries, in the probe itself. */
static int refusals(void)
{
    struct insn past[2] = {{0x05, 0, 0, 5}, {RET_K, 0, 0, RET_ALLOW}};
    struct insn wide[2] = {{LD_W_ABS, 0, 0, 64}, {RET_K, 0, 0, RET_ALLOW}};
    uint32_t action = RET_ALLOW;
    pid_t pid;
    int status;

    if (prctl(PR_GET_SECCOMP) != 0 || !status_has("Seccomp:\t0\n")) {
        return fail("a process starts with no filter");
    }
    if (install(past, 0, 0) != -1 || errno != EINVAL || install(past, 2, 0) != -1 ||
        errno != EINVAL || install(wide, 2, 0) != -1 || errno != EINVAL) {
        return fail("an empty program, a jump past the end and a wide load are refused");
    }
    if (syscall(NR_SECCOMP, GET_ACTION_AVAIL, 0, &action) != 0) {
        return fail("SECCOMP_RET_ALLOW is available");
    }
    action = RET_USER_NOTIF;
    if (syscall(NR_SECCOMP, GET_ACTION_AVAIL, 0, &action) != -1 || errno != EOPNOTSUPP) {
        return fail("SECCOMP_RET_USER_NOTIF is not available");
    }
    pid = fork();
    if (pid == 0) {
        _exit(setuid(1000) == 0 && answer(NR_GETPPID, RET_ERRNO | 77, 0) == -1 && errno == EACCES
                  ? 0
                  : 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("an unprivileged caller without no_new_privs is refused");
    }
    return 0;
}

/** The thread TSYNC reaches: it waits for the filter, then makes the call. */
static void *tsync_thread(void *arg)
{
    volatile int *go = arg;

    while (*go == 0) {
        sched_yield();
    }
    return (void *)(long)(syscall(NR_GETTID) == -1 && errno == 33);
}

/** The filters, in a child with no_new_privs; returns its exit status. */
static int filters(const char *self)
{
    struct sigaction sa;
    volatile int go = 0;
    pthread_t th;
    void *res;
    pid_t pid;
    int status;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || answer(NR_GETPPID, RET_ERRNO | 77, 0) != 0) {
        return fail("a filter installs under no_new_privs");
    }
    if (syscall(NR_GETPPID) != -1 || errno != 77 || getpid() <= 0) {
        return fail("the filter fails getppid with its errno and lets getpid run");
    }
    if (prctl(PR_GET_SECCOMP) != 2 || !status_has("Seccomp:\t2\nSeccomp_filters:\t1\n")) {
        return fail("the status file shows the mode and one filter");
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    if (answer(NR_GETUID, RET_TRAP | 5, 0) != 0 || syscall(NR_GETUID) != 4242 || trapped != 1 ||
        !trap_ok) {
        return fail("a trap posts SIGSYS with the call's siginfo and its number in EAX");
    }
    if (answer(NR_GETPPID, RET_TRAP | 5, 0) != 0 || syscall(NR_GETPPID) == -1 || trapped != 2) {
        return fail("of a trap and an errno for one call, the trap wins");
    }
    pid = fork();
    if (pid == 0) {
        if (answer(NR_GETGID, RET_KILL_PROCESS, 0) != 0) {
            _exit(4);
        }
        syscall(NR_GETGID);
        _exit(3);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSYS) {
        return fail("a grandchild's filter kills it with SIGSYS");
    }
    if (pthread_create(&th, NULL, tsync_thread, (void *)&go) != 0 ||
        answer(NR_GETTID, RET_ERRNO | 33, FLAG_TSYNC) != 0) {
        return fail("TSYNC installs over a second thread");
    }
    go = 1;
    if (pthread_join(th, &res) != 0 || res == NULL) {
        return fail("the second thread has the filter TSYNC gave it");
    }
    execl(self, self, "exec", (char *)NULL);
    return fail("execl");
}

/** After the exec: the filters are kept. */
static int after_exec(void)
{
    if (prctl(PR_GET_SECCOMP) != 2 || syscall(NR_GETTID) != -1 || errno != 33 ||
        !status_has("Seccomp_filters:\t4\n")) {
        return fail("an exec keeps the filters");
    }
    return 0;
}

/** The strict mode, in a child: write runs, getpid kills. */
static int strict(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if (syscall(NR_SECCOMP, SET_MODE_STRICT, 0, NULL) != 0) {
            _exit(5);
        }
        if (write(1, "", 0) != 0) {
            syscall(SYS_exit, 6);
        }
        syscall(SYS_getpid);
        syscall(SYS_exit, 7);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return fail("the strict mode kills at getpid");
    }
    return 0;
}

int main(int argc, char **argv)
{
    pid_t pid;
    int status;

    if (argc > 1) {
        return after_exec();
    }
    if (refusals() || strict()) {
        return 1;
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        _exit(filters(argv[0]));
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 1;
    }
    printf("SECCOMPMUSL OK\n");
    return 0;
}
