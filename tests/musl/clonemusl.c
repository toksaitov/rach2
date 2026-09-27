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
 * parent's CLONE_PARENT_SETTID word holds the child's id, that a sharing flag
 * is refused, and that futex compares, times out on both clocks and refuses
 * what it does not implement.
 *
 * Prints "CLONEMUSL OK" on success and "CLONEMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CLONE_VM             0x00000100
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

static long futex(volatile uint32_t *word, int op, uint32_t val, const struct timespec *ts,
                  uint32_t val3)
{
    return raw(SYS_futex, (long)word, op, (long)val, (long)ts, 0, (long)val3);
}

int main(void)
{
    static volatile int32_t ctid;
    static volatile int32_t ptid;
    static volatile uint32_t word = 7;
    struct timespec ts;
    long pid;
    int status;

    if (raw(SYS_set_tid_address, (long)&ctid, 0, 0, 0, 0, 0) != getpid()) {
        return fail("set_tid_address did not return the pid");
    }

    pid = raw(SYS_clone, CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_PARENT_SETTID | SIGCHLD,
              0, (long)&ptid, 0, (long)&ctid, 0);
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
    if (raw(SYS_clone, CLONE_VM | SIGCHLD, 0, 0, 0, 0, 0) != -EINVAL) {
        return fail("a sharing flag was not refused");
    }

    if (futex(&word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0) != 0) {
        return fail("a wake with no waiter did not return 0");
    }
    if (futex(&word, FUTEX_WAIT, 8, NULL, 0) != -EAGAIN) {
        return fail("a wait on another value did not return EAGAIN");
    }
    ts.tv_sec = 0;
    ts.tv_nsec = 20000000;
    if (futex(&word, FUTEX_WAIT, 7, &ts, 0) != -ETIMEDOUT) {
        return fail("a relative wait did not time out");
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 20000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_nsec -= 1000000000;
        ts.tv_sec++;
    }
    if (futex(&word, FUTEX_WAIT_BITSET, 7, &ts, 0xFFFFFFFFu) != -ETIMEDOUT) {
        return fail("an absolute monotonic wait did not time out");
    }
    clock_gettime(CLOCK_REALTIME, &ts);
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

    printf("CLONEMUSL OK\n");
    return 0;
}
