/**
 * clone in the form GNU libc's fork uses, set_tid_address, and futex within
 * one address space.
 *
 * glibc makes every child with clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID
 * | SIGCHLD), never with fork, and reads the child's id from the word the
 * kernel stores in the child's memory; a child that finds the parent's id
 * there sends raise() to the parent. glibc also calls futex in a process of
 * one thread, from pthread_once. The probe checks that the child's copy of the
 * word holds the child's id and the parent's copy is untouched, that the
 * parent's CLONE_PARENT_SETTID word holds the child's id, that CLONE_SIGHAND
 * without CLONE_VM is refused, and that futex compares, times out on both
 * clocks no sooner than asked, and refuses what it does not implement. A
 * timed wait that a handler with SA_RESTART interrupts ends with EINTR, as
 * Linux ends one, and is not restarted with its whole timeout again.
 *
 * The futex call (240, or 202 on x86-64) takes the kernel's timespec, whose
 * fields are a long's width: on i386 that is not musl's, whose seconds are 64
 * bits wide.
 *
 * Prints "CLONEMUSL OK" on success and "CLONEMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CLONE_SIGHAND        0x00000800
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_WAIT_BITSET    9
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

static int fail(const char *what)
{
    printf("CLONEMUSL FAIL (%s)\n", what);
    return 1;
}

/* The raw call's result: the value, or -errno. */
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

static long futex(volatile uint32_t *word, int op, uint32_t val, const struct kts *ts,
                  uint32_t val3)
{
    return raw(SYS_futex, (long)word, op, (long)val, (long)ts, 0, (long)val3);
}

/** A clock's reading in nanoseconds. */
static long long ns_of(clockid_t clk)
{
    struct timespec ts;

    clock_gettime(clk, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** A time in nanoseconds as a kernel timespec. */
static void to_kts(long long ns, struct kts *ts)
{
    ts->sec = (long)(ns / 1000000000LL);
    ts->nsec = (long)(ns % 1000000000LL);
}

static volatile sig_atomic_t alarms;

static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

/* A wait of five seconds on `word`, which holds 7, that a SIGALRM with
 * SA_RESTART interrupts 20 ms in; the raw call's result. */
static long interrupted_wait(volatile uint32_t *word)
{
    struct sigaction sa;
    struct itimerval it;
    struct kts ts;
    long r;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    memset(&it, 0, sizeof(it));
    it.it_value.tv_usec = 20000;
    setitimer(ITIMER_REAL, &it, NULL);
    ts.sec = 5;
    ts.nsec = 0;
    r = futex(word, FUTEX_WAIT, 7, &ts, 0);
    signal(SIGALRM, SIG_DFL);
    return r;
}

int main(void)
{
    static volatile int32_t ctid;
    static volatile int32_t ptid;
    static volatile uint32_t word = 7;
    struct kts ts;
    long long start;
    long long deadline;
    long pid;
    int status;

    if (raw(SYS_set_tid_address, (long)&ctid, 0, 0, 0, 0, 0) != getpid()) {
        return fail("set_tid_address did not return the pid");
    }

    pid = raw(SYS_clone, CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_PARENT_SETTID | SIGCHLD,
              0, (long)&ptid, CLONE_TLS_CTID(0, &ctid), 0);
    if (pid < 0) {
        return fail("clone refused the fork form");
    }
    if (pid == 0) {
        _exit(ctid == getpid() ? 0 : 1);
    }
    if (waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("the child did not exit");
    }
    if (WEXITSTATUS(status) != 0) {
        return fail("the child's CLONE_CHILD_SETTID word was not its id");
    }
    if (ptid != pid) {
        return fail("CLONE_PARENT_SETTID did not store the child's id");
    }
    if (ctid != 0) {
        return fail("the store in the child changed the parent's word");
    }
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

    if (futex(&word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0) != 0) {
        return fail("a wake with no waiter did not return 0");
    }
    if (futex(&word, FUTEX_WAIT, 8, NULL, 0) != -EAGAIN) {
        return fail("a wait on another value did not return EAGAIN");
    }
    start = ns_of(CLOCK_MONOTONIC);
    to_kts(20000000, &ts);
    if (futex(&word, FUTEX_WAIT, 7, &ts, 0) != -ETIMEDOUT) {
        return fail("a relative wait did not time out");
    }
    if (ns_of(CLOCK_MONOTONIC) - start < 20000000) {
        return fail("a relative wait of 20 ms ended sooner");
    }
    deadline = ns_of(CLOCK_MONOTONIC) + 20000000;
    to_kts(deadline, &ts);
    if (futex(&word, FUTEX_WAIT_BITSET, 7, &ts, 0xFFFFFFFFu) != -ETIMEDOUT) {
        return fail("an absolute monotonic wait did not time out");
    }
    if (ns_of(CLOCK_MONOTONIC) < deadline) {
        return fail("an absolute monotonic wait ended before its deadline");
    }
    to_kts(ns_of(CLOCK_REALTIME), &ts);
    if (futex(&word, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 7, &ts, 0xFFFFFFFFu) != -ETIMEDOUT) {
        return fail("an absolute real-time wait in the past did not time out");
    }
    if (futex(&word, FUTEX_WAIT_BITSET, 7, NULL, 0) != -EINVAL) {
        return fail("a zero bitset was not refused");
    }
    if (futex((volatile uint32_t *)((uintptr_t)&word + 1), FUTEX_WAKE, 1, NULL, 0) != -EINVAL) {
        return fail("an unaligned address was not refused");
    }
    if (futex(&word, FUTEX_REQUEUE, 1, NULL, 0) != -ENOSYS) {
        return fail("requeue did not answer ENOSYS");
    }
    if (futex(&word, FUTEX_WAIT | FUTEX_CLOCK_REALTIME, 7, NULL, 0) != -ENOSYS) {
        return fail("the real-time clock was accepted for FUTEX_WAIT");
    }
    if (interrupted_wait(&word) != -EINTR || alarms != 1) {
        return fail("a timed wait a handler with SA_RESTART interrupted did not end with EINTR");
    }

    printf("CLONEMUSL OK\n");
    return 0;
}
