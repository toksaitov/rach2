/**
 * cputimemusl: check the CPU time the kernel charges each process, read
 * through musl's `times`, `wait4` and `getrusage`.
 *
 * The timer charges the tick it interrupts to the process it finds running:
 * as user time when the process was in its own code, as system time when it
 * was inside a system call, and to no one when the processor was idle. A
 * reap adds the child's two times to the parent's children's totals.
 *
 * The rows, each judged by what the kernel counted:
 *
 * A loop that stays in user mode gains user time, and a loop of large reads
 * from `/dev/zero`, which spends its time copying in the kernel, gains system
 * time. Neither sum is ever more than the ticks since boot that `times`
 * returns beside it.
 *
 * A child that spins for three ticks of its own user time and exits is
 * reaped by `wait4`, whose resource usage reports that child's time, and the
 * parent's children's user time then rises by exactly the ticks reported.
 *
 * `getrusage` reports the same ticks as timevals: for the process and for
 * its thread what `times` reports, read between two readings of it, and for
 * the children their totals; any other `who` fails with `EINVAL`.
 *
 * A loop runs until the time it waits for is charged: a kernel that never
 * charged it leaves the loop running, and the case's watchdog ends it.
 *
 * It prints "CPUTIMEMUSL OK" and exits 0 on success, or prints
 * "CPUTIMEMUSL FAIL (detail)" and exits 1 at the first failure.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check: `user-time`, `user-bound`,
 * `system-time`, `system-bound`, `wait4-reap`, `child-charged`,
 * `wait4-utime`, `children-times`, `getrusage-children`, `getrusage-self`
 * and `getrusage-einval`. A failed check fails its assertion and leaves
 * those after it not run.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

extern char **environ;

/* The bytes one read of /dev/zero asks for. */
#define READ_SIZE 65536

static char zeroes[READ_SIZE];

/** Report one failed check, end the report at the assertion that `what`
 * fails, and return 1, the probe's failure status. */
static int fail(const char *what)
{
    report_stop(what);
    printf("CPUTIMEMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Spend a while in user mode without a system call. Returns the sum the loop
 * computed, so that the compiler keeps the loop.
 */
static unsigned int spin(void)
{
    volatile unsigned int x = 0;
    unsigned int i;

    for (i = 0; i < 200000u; i++) {
        x += i;
    }
    return x;
}

/** Convert a timeval to the 100 Hz ticks the kernel counts. */
static long ticks_of(const struct timeval *tv)
{
    return (long)tv->tv_sec * 100 + (long)tv->tv_usec / 10000;
}

/**
 * Spin until this process has `want` more ticks of user time than it had at
 * the start. `from` receives the times read at the start. Returns the ticks
 * of user time gained.
 */
static long spin_for(long want, struct tms *from)
{
    struct tms now;

    times(from);
    now = *from;
    while ((long)(now.tms_utime - from->tms_utime) < want) {
        spin();
        times(&now);
    }
    return (long)(now.tms_utime - from->tms_utime);
}

/**
 * Check that `getrusage(who)` reports user and system times that lie between
 * two readings of `times` taken around it. Returns 0 when they do, and 1 when
 * they do not or the call fails.
 */
static int usage_between(int who)
{
    struct tms before;
    struct tms after;
    struct rusage ru;
    long user;
    long sys;

    times(&before);
    if (getrusage(who, &ru) != 0) {
        return 1;
    }
    times(&after);
    user = ticks_of(&ru.ru_utime);
    sys = ticks_of(&ru.ru_stime);
    return user < (long)before.tms_utime || user > (long)after.tms_utime ||
           sys < (long)before.tms_stime || sys > (long)after.tms_stime;
}

/**
 * Fork a child that spins for three ticks of user time, reap it by `wait4`,
 * and check what wait4 reports and the children's times in `after`, read
 * after the reap. Returns 0, or 1 from `fail` at the first failed check.
 */
static int reaped_child(struct tms *after)
{
    struct tms before;
    struct rusage ru;
    pid_t pid;
    int status;

    /* A reaped child: the time wait4 reports is added to the children's. */
    times(&before);
    pid = fork();
    if (pid < 0) {
        return fail("fork failed");
    }
    if (pid == 0) {
        struct tms mine;

        _exit(spin_for(3, &mine) >= 3 ? 0 : 1);
    }
    if (wait4(pid, &status, 0, &ru) != pid) {
        return fail("wait4 did not reap the child");
    }
    report_held();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child was not charged three ticks of user time");
    }
    report_held();
    if (ticks_of(&ru.ru_utime) < 3) {
        return fail("wait4 reported less user time than the child was charged");
    }
    report_held();
    times(after);
    if ((long)(after->tms_cutime - before.tms_cutime) != ticks_of(&ru.ru_utime) ||
        (long)(after->tms_cstime - before.tms_cstime) != ticks_of(&ru.ru_stime)) {
        return fail("the children's times did not rise by what wait4 reported");
    }
    report_held();
    return 0;
}

int main(void)
{
    struct tms before;
    struct tms after;
    struct rusage ru;
    clock_t at;
    int fd;

    report_begin(report_case(environ, "guest.default.cputimemusl"));
    report_plan("user-time");
    report_plan("user-bound");
    report_plan("system-time");
    report_plan("system-bound");
    report_plan("wait4-reap");
    report_plan("child-charged");
    report_plan("wait4-utime");
    report_plan("children-times");
    report_plan("getrusage-children");
    report_plan("getrusage-self");
    report_plan("getrusage-einval");

    /* User time: a loop in user mode is charged to utime. */
    spin_for(5, &before);
    report_held();
    at = times(&after);
    if ((long)(after.tms_utime + after.tms_stime) > (long)at) {
        return fail("the process was charged more CPU time than the ticks since boot");
    }
    report_held();

    /* System time: a loop of large reads is charged to stime. */
    fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) {
        return fail("/dev/zero did not open");
    }
    at = times(&before);
    after = before;
    while ((long)(after.tms_stime - before.tms_stime) < 2) {
        if (read(fd, zeroes, sizeof(zeroes)) != (ssize_t)sizeof(zeroes)) {
            close(fd);
            return fail("a read of /dev/zero came back short");
        }
        at = times(&after);
    }
    close(fd);
    report_held();
    if ((long)(after.tms_utime + after.tms_stime) > (long)at) {
        return fail("the process was charged more CPU time than the ticks since boot");
    }
    report_held();

    if (reaped_child(&after) != 0) {
        return 1;
    }

    /* getrusage: the same ticks, as timevals. */
    if (getrusage(RUSAGE_CHILDREN, &ru) != 0 || ticks_of(&ru.ru_utime) != (long)after.tms_cutime ||
        ticks_of(&ru.ru_stime) != (long)after.tms_cstime) {
        return fail("getrusage of the children is not their times");
    }
    report_held();
    if (usage_between(RUSAGE_SELF) != 0 || usage_between(RUSAGE_THREAD) != 0) {
        return fail("getrusage of the process or its thread is not its times");
    }
    report_held();
    if (getrusage(2, &ru) != -1 || errno != EINVAL) {
        return fail("getrusage of an unknown who was not refused with EINVAL");
    }
    report_held();
    report_end();

    printf("CPUTIMEMUSL OK\n");

    return 0;
}
