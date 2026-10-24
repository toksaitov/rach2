/**
 * clonemusl: check clone in the form GNU libc's fork uses, set_tid_address,
 * and futex within one address space.
 *
 * glibc makes every child with clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID
 * | SIGCHLD), never with fork, and reads the child's id from the word the
 * kernel stores in the child's memory; a child that found the parent's id
 * there would send its raise() to the parent. glibc also calls futex in a
 * process of one thread, from pthread_once. The program checks that the
 * child's copy of the word has the child's id and the parent's copy is
 * unchanged, that the parent's CLONE_PARENT_SETTID word has the child's id,
 * that CLONE_SIGHAND without CLONE_VM fails with EINVAL, and that futex
 * compares, times out on both clocks no sooner than asked, and returns EINVAL
 * or ENOSYS for what it does not take. A
 * timed wait that a handler with SA_RESTART interrupts ends with EINTR, as
 * Linux ends one, and is not restarted with its whole timeout again.
 *
 * The futex call (240, or 202 on x86-64) takes the kernel's timespec, whose
 * fields are a long's width: on i386 that is not musl's, whose seconds are 64
 * bits wide.
 *
 * It prints "CLONEMUSL OK" and exits 0 on success, or prints "CLONEMUSL FAIL
 * (detail)" and exits 1 at the first failure. Writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, an assertion for each
 * check, as `assertion_id` lists them; a failed check fails its assertion
 * and leaves those after it not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_report.h"

extern char **environ;

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_WAIT_BITSET    9
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

/* The assertions, a check each, in the order the program makes them. */
static const char *const assertion_id[] = {"set-tid-address",
                                           "clone-fork-form",
                                           "child-exit",
                                           "child-settid",
                                           "parent-settid",
                                           "parent-word",
                                           "sighand-einval",
                                           "zero-clone",
                                           "zero-filter",
                                           "zero-reap",
                                           "zero-signal",
                                           "parent-inherit",
                                           "exec-reset",
                                           "wake-no-waiter",
                                           "wait-eagain",
                                           "relative-timeout",
                                           "relative-duration",
                                           "monotonic-timeout",
                                           "monotonic-deadline",
                                           "realtime-past",
                                           "zero-bitset",
                                           "unaligned",
                                           "requeue-enosys",
                                           "realtime-wait-enosys",
                                           "eintr"};

/** Begin the report and plan its assertions. */
static void report_start(void)
{
    unsigned int i;

    report_begin(report_case(environ, "guest.default.clonemusl"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
}

/** Report one failed check, end the report at the assertion that `what`
 * fails, and return 1, the probe's failure status. */
static int fail(const char *what)
{
    report_stop(what);
    printf("CLONEMUSL FAIL (%s)\n", what);
    return 1;
}

/** Make system call `n` with six arguments. Returns its value, or -errno. */
static long raw(long n, long a, long b, long c, long d, long e, long f)
{
    long r = syscall(n, a, b, c, d, e, f);

    return r == -1 ? -errno : r;
}

/* The kernel's timespec, of a long's width: 32 bits on i386, 64 on x86-64. */
struct kts {
    long sec;
    long nsec;
};

/* The last two arguments of a raw clone, the TLS and the address the child's
 * id goes to: i386 takes the TLS first, and x86-64 the address. */
#if defined __x86_64__
#define CLONE_TLS_CTID(tls, ctid) (long)(ctid), (long)(tls)
#else
#define CLONE_TLS_CTID(tls, ctid) (long)(tls), (long)(ctid)
#endif

/**
 * Make the futex call `op` on `word` with `val`, the timeout `ts` (relative
 * or absolute, as `op` says, or null) and `val3`. Returns its value, or
 * -errno.
 */
static long futex(volatile uint32_t *word, int op, uint32_t val, const struct kts *ts,
                  uint32_t val3)
{
    return raw(SYS_futex, (long)word, op, (long)val, (long)ts, 0, (long)val3);
}

/** Return the reading of the clock `clk` in nanoseconds. */
static long long ns_of(clockid_t clk)
{
    struct timespec ts;

    clock_gettime(clk, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** Convert a time in nanoseconds to a kernel timespec. */
static void to_kts(long long ns, struct kts *ts)
{
    ts->sec = (long)(ns / 1000000000LL);
    ts->nsec = (long)(ns % 1000000000LL);
}

/* The count of SIGALRM deliveries to on_alarm. */
static volatile sig_atomic_t alarms;

/* The word of the interrupted wait, which on_alarm changes from 7 to 8. */
static volatile uint32_t *alarm_word;

/** Count one SIGALRM in `alarms` and set the wait's word to 8. */
static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
    *alarm_word = 8;
}

/**
 * Wait five seconds on `word` for 7, with a SIGALRM handler installed with
 * SA_RESTART and a timer that delivers the signal 20 ms in, until the signal
 * comes while the call waits. The handler sets the word to 8, so a signal
 * that comes before the call compares the word ends the call at once with
 * EAGAIN, and the wait is tried again, at most 20 times; so does a wait the
 * kernel restarts after the handler. Returns the last futex call's value, or
 * -errno; `word` is 7 again and SIGALRM back at its default action on return.
 */
static long interrupted_wait(volatile uint32_t *word)
{
    struct sigaction sa;
    struct itimerval it;
    struct kts ts;
    long r = -EAGAIN;
    int tries;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    alarm_word = word;
    sigaction(SIGALRM, &sa, NULL);
    for (tries = 0; tries < 20 && r == -EAGAIN; tries++) {
        *word = 7;
        alarms = 0;
        memset(&it, 0, sizeof(it));
        it.it_value.tv_usec = 20000;
        setitimer(ITIMER_REAL, &it, NULL);
        ts.sec = 5;
        ts.nsec = 0;
        r = futex(word, FUTEX_WAIT, 7, &ts, 0);
    }
    signal(SIGALRM, SIG_DFL);
    *word = 7;
    return r;
}

/**
 * The futex rows on `word`, which holds 7: a wake with no waiter, a wait on
 * another value, the timeouts on both clocks, the refusals, and a wait that
 * a handler interrupts. Returns 0, or 1 from `fail` at the first failed
 * check.
 */
static int futex_rows(volatile uint32_t *word)
{
    struct kts ts;
    long long start;
    long long deadline;

    if (futex(word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0) != 0) {
        return fail("a wake with no waiter did not return 0");
    }
    report_held();
    if (futex(word, FUTEX_WAIT, 8, NULL, 0) != -EAGAIN) {
        return fail("a wait on another value did not return EAGAIN");
    }
    report_held();
    start = ns_of(CLOCK_MONOTONIC);
    to_kts(20000000, &ts);
    if (futex(word, FUTEX_WAIT, 7, &ts, 0) != -ETIMEDOUT) {
        return fail("a relative wait did not time out");
    }
    report_held();
    if (ns_of(CLOCK_MONOTONIC) - start < 20000000) {
        return fail("a relative wait of 20 ms ended sooner");
    }
    report_held();
    deadline = ns_of(CLOCK_MONOTONIC) + 20000000;
    to_kts(deadline, &ts);
    if (futex(word, FUTEX_WAIT_BITSET, 7, &ts, 0xFFFFFFFFu) != -ETIMEDOUT) {
        return fail("an absolute monotonic wait did not time out");
    }
    report_held();
    if (ns_of(CLOCK_MONOTONIC) < deadline) {
        return fail("an absolute monotonic wait ended before its deadline");
    }
    report_held();
    to_kts(ns_of(CLOCK_REALTIME), &ts);
    if (futex(word, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 7, &ts, 0xFFFFFFFFu) != -ETIMEDOUT) {
        return fail("an absolute real-time wait in the past did not time out");
    }
    report_held();
    if (futex(word, FUTEX_WAIT_BITSET, 7, NULL, 0) != -EINVAL) {
        return fail("a zero bitset was not refused");
    }
    report_held();
    if (futex((volatile uint32_t *)((uintptr_t)word + 1), FUTEX_WAKE, 1, NULL, 0) != -EINVAL) {
        return fail("an unaligned address was not refused");
    }
    report_held();
    if (futex(word, FUTEX_REQUEUE, 1, NULL, 0) != -ENOSYS) {
        return fail("requeue did not answer ENOSYS");
    }
    report_held();
    if (futex(word, FUTEX_WAIT | FUTEX_CLOCK_REALTIME, 7, NULL, 0) != -ENOSYS) {
        return fail("the real-time clock was accepted for FUTEX_WAIT");
    }
    report_held();
    if (interrupted_wait(word) != -EINTR || alarms != 1) {
        return fail("a timed wait a handler with SA_RESTART interrupted did not end with EINTR");
    }
    report_held();
    report_end();
    return 0;
}

static unsigned char clone_stacks[3][65536] __attribute__((aligned(16)));

static volatile int clone_shared;
static volatile pid_t clone_grandchild;
static const char *clone_executable;

static int clone_leaf(void *arg)
{
    (void)arg;
    return 74;
}

/* Match the sanitizer's shared resources, but keep distinct thread groups. */
static int clone_shared_child(void *arg)
{
    (void)arg;
    clone_shared = 123;
    clone_grandchild = clone(clone_leaf, clone_stacks[1] + sizeof(clone_stacks[1]),
                             CLONE_VM | CLONE_PARENT | SIGCHLD, NULL);
    return clone_grandchild > 0 ? 73 : 72;
}

static int clone_exec_child(void *arg)
{
    char *args[] = {(char *)clone_executable, "--clone-exec", NULL};
    (void)arg;
    execve(clone_executable, args, environ);
    return 71;
}

/* Drain an earlier, already-reaped fork's notification before this test. */
static int clone_block_children(sigset_t *old)
{
    sigset_t mask;
    struct timespec zero = {0, 0};
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, old))
        return fail("cannot block child notifications");
    if (sigtimedwait(&mask, NULL, &zero) < 0 && errno != EAGAIN)
        return fail("cannot inspect earlier child notification");
    return 0;
}

static int clone_exec_reset(void)
{
    int status;
    sigset_t pending;
    siginfo_t info;
    pid_t pid = clone(clone_exec_child, clone_stacks[2] + sizeof(clone_stacks[2]),
                      CLONE_VM | CLONE_UNTRACED, NULL);
    memset(&info, 0, sizeof(info));
    if (pid < 0 || waitid(P_PID, pid, &info, WEXITED | WNOWAIT | __WALL) || info.si_pid != pid ||
        waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 75 ||
        sigpending(&pending) || !sigismember(&pending, SIGCHLD))
        return fail("exec did not reset zero-signal child notification and wait selection");
    report_held();
    return 0;
}

static int zero_signal_rows(void)
{
    sigset_t old, pending;
    int status;
    pid_t pid;
    if (clone_block_children(&old))
        return 1;
    pid = clone(clone_shared_child, clone_stacks[0] + sizeof(clone_stacks[0]),
                CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_UNTRACED, NULL);
    if (pid < 0)
        return fail("sanitizer zero-signal clone was refused");
    report_held();
    if (waitpid(pid, &status, WNOHANG) != -1 || errno != ECHILD)
        return fail("ordinary wait selected a zero-signal child");
    report_held();
    if (waitpid(pid, &status, __WCLONE) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 73 ||
        clone_shared != 123)
        return fail("zero-signal child did not share memory and wake its clone wait");
    report_held();
    if (sigpending(&pending) || sigismember(&pending, SIGCHLD))
        return fail("a zero-signal child posted SIGCHLD");
    report_held();
    if (clone_grandchild <= 0 || waitpid(clone_grandchild, &status, WNOHANG) != -1 ||
        errno != ECHILD || waitpid(clone_grandchild, &status, __WCLONE) != clone_grandchild ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 74)
        return fail("CLONE_PARENT did not inherit the caller's zero exit signal");
    report_held();
    if (clone_exec_reset())
        return 1;
    sigprocmask(SIG_SETMASK, &old, NULL);
    return 0;
}

int main(int argc, char **argv)
{
    static volatile int32_t ctid;
    static volatile int32_t ptid;
    static volatile uint32_t word = 7;
    long pid;
    int status;

    if (argc == 2 && !strcmp(argv[1], "--clone-exec"))
        return 75;
    clone_executable = argv[0];
    report_start();
    if (raw(SYS_set_tid_address, (long)&ctid, 0, 0, 0, 0, 0) != getpid()) {
        return fail("set_tid_address did not return the pid");
    }
    report_held();

    pid = raw(SYS_clone, CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_PARENT_SETTID | SIGCHLD,
              0, (long)&ptid, CLONE_TLS_CTID(0, &ctid), 0);
    if (pid < 0) {
        return fail("clone refused the fork form");
    }
    if (pid == 0) {
        _exit(ctid == getpid() ? 0 : 1);
    }
    report_held();
    if (waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("the child did not exit");
    }
    report_held();
    if (WEXITSTATUS(status) != 0) {
        return fail("the child's CLONE_CHILD_SETTID word was not its id");
    }
    report_held();
    if (ptid != pid) {
        return fail("CLONE_PARENT_SETTID did not store the child's id");
    }
    report_held();
    if (ctid != 0) {
        return fail("the store in the child changed the parent's word");
    }
    report_held();
    pid = raw(SYS_clone, CLONE_SIGHAND | SIGCHLD, 0, 0, 0, 0, 0);
    if (pid == 0) {
        _exit(1);
    }
    if (pid > 0) {
        waitpid((pid_t)pid, &status, 0);
    }
    if (pid != -EINVAL) {
        return fail("CLONE_SIGHAND without CLONE_VM was not refused");
    }
    report_held();

    if (zero_signal_rows() != 0)
        return 1;
    if (futex_rows(&word) != 0) {
        return 1;
    }

    printf("CLONEMUSL OK\n");
    return 0;
}
