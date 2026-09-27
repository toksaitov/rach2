/**
 * How the kernel names and schedules a process, read through musl.
 *
 * `gettid` returns the process id, in the process and in a forked child
 * alike, since each process is one thread.
 *
 * The nice value starts at 0, is kept as set, clamped to 19, inherited by a
 * child, and read raw as 20 less itself. Set for a process group it reaches
 * every member, and read for one it is the lowest. A caller of another user
 * may raise its own nice value but not lower it, and may not change root's
 * process; it may set its own user's. No process of a user, a pid or an
 * unknown `which` is refused.
 *
 * `sched_yield` returns 0. Every process has SCHED_OTHER with priority 0, and
 * asking for it, with SCHED_RESET_ON_FORK or not, succeeds; SCHED_FIFO and
 * SCHED_RR are refused with EPERM, and the other policies, a priority that
 * does not suit its policy, and an unknown policy with EINVAL. The priority
 * ranges are Linux's, and the time slice is one 10 ms tick. The affinity is
 * CPU 0 alone, read raw as the four bytes written; a mask with CPU 0 is
 * accepted and one without it refused. A caller of another user may not
 * change root's process, and a pid of no process is refused with ESRCH. musl
 * answers the policy and parameter calls with ENOSYS itself, so those rows
 * call the kernel through `syscall`.
 *
 * Prints "SCHEDMUSL OK" on success and "SCHEDMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("SCHEDMUSL FAIL (%s)\n", what);

    return 1;
}

/** The thread id: the process id, here and in a child. */
static int thread_ids(void)
{
    pid_t child;
    int status;

    if (gettid() != getpid()) {
        return fail("gettid is not the process id");
    }
    child = fork();
    if (child < 0) {
        return fail("fork failed");
    }
    if (child == 0) {
        _exit(gettid() == getpid() ? 0 : 1);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a child's gettid is not its process id");
    }
    return 0;
}

/** Run `test` in a child and return its exit status, or -1. */
static int in_child(int (*test)(void))
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        _exit(test());
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/** A child's view: the nice value it inherited, -5. */
static int inherited(void)
{
    return getpriority(PRIO_PROCESS, 0) == -5 ? 0 : 1;
}

/** A group of two: set for the group, both have it, and the group reads as
 * its lowest. */
static int group(void)
{
    pid_t member;
    int rc = 0;

    if (setpgid(0, 0) != 0) {
        return 1;
    }
    member = fork();
    if (member < 0) {
        return 2;
    }
    if (member == 0) {
        pause();
        _exit(0);
    }
    if (setpriority(PRIO_PGRP, 0, 3) != 0 || getpriority(PRIO_PROCESS, member) != 3) {
        rc = 3;
    } else if (setpriority(PRIO_PROCESS, member, 7) != 0 || getpriority(PRIO_PGRP, 0) != 3) {
        rc = 4;
    }
    kill(member, SIGKILL);
    waitpid(member, NULL, 0);
    return rc;
}

/** A caller of user 1000: it may raise its own nice value and set its
 * user's, and may neither lower its own nor change root's process. */
static int unprivileged(void)
{
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 1;
    }
    if (setpriority(PRIO_PROCESS, 0, 10) != 0) {
        return 2;
    }
    if (setpriority(PRIO_PROCESS, 0, 9) != -1 || errno != EACCES) {
        return 3;
    }
    if (setpriority(PRIO_PROCESS, getppid(), 10) != -1 || errno != EPERM) {
        return 4;
    }
    if (setpriority(PRIO_USER, 0, 12) != 0 || getpriority(PRIO_USER, 1000) != 12) {
        return 5;
    }
    return 0;
}

/** The nice value: kept, clamped, inherited, raw, by group and by user, and
 * the refusals. */
static int priorities(void)
{
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0) {
        return fail("the nice value does not start at 0");
    }
    if (setpriority(PRIO_PROCESS, 0, 5) != 0 || getpriority(PRIO_PROCESS, 0) != 5) {
        return fail("a nice value of 5 was not kept");
    }
    if (syscall(SYS_getpriority, PRIO_PROCESS, 0) != 15) {
        return fail("the raw getpriority does not return 20 less the nice value");
    }
    if (setpriority(PRIO_PROCESS, getpid(), 100) != 0 || getpriority(PRIO_PROCESS, 0) != 19) {
        return fail("a nice value past 19 was not clamped to 19");
    }
    if (setpriority(PRIO_PROCESS, 0, -5) != 0 || getpriority(PRIO_PROCESS, 0) != -5) {
        return fail("root could not lower the nice value");
    }
    if (in_child(inherited) != 0) {
        return fail("a child did not inherit the nice value");
    }
    if (in_child(group) != 0) {
        return fail("a process group's nice value was not set or read as one");
    }
    switch (in_child(unprivileged)) {
    case 0:
        break;
    case 2:
        return fail("user 1000 could not raise its own nice value");
    case 3:
        return fail("user 1000 lowered its own nice value");
    case 4:
        return fail("user 1000 changed root's process");
    case 5:
        return fail("user 1000 could not set its own user's nice value");
    default:
        return fail("the unprivileged child did not run");
    }
    if (getpriority(PRIO_USER, 4242) != -1 || errno != ESRCH ||
        getpriority(PRIO_PROCESS, 999999) != -1 || errno != ESRCH) {
        return fail("a priority of no process was not refused with ESRCH");
    }
    if (getpriority(3, 0) != -1 || errno != EINVAL) {
        return fail("an unknown which was not refused with EINVAL");
    }
    return 0;
}

/** Ask the kernel to set `pid` to `policy` at `priority`; 0, or -1 with
 * errno set. */
static long set_policy(pid_t pid, int policy, int priority)
{
    struct sched_param sp;

    sp.sched_priority = priority;
    return syscall(SYS_sched_setscheduler, (long)pid, (long)policy, &sp);
}

/** A caller of user 1000 may change neither the policy nor the affinity of
 * root's process. */
static int not_root(void)
{
    cpu_set_t set;

    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 1;
    }
    if (set_policy(getppid(), SCHED_OTHER, 0) != -1 || errno != EPERM) {
        return 2;
    }
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(getppid(), sizeof(set), &set) != -1 || errno != EPERM) {
        return 3;
    }
    return 0;
}

/** Yielding, the policy calls, the ranges, the time slice and the
 * affinity. */
static int scheduling(void)
{
    static const int refused[] = {SCHED_BATCH, SCHED_IDLE, SCHED_DEADLINE, 4, 42};
    struct sched_param sp;
    struct timespec slice;
    cpu_set_t set;
    unsigned int i;

    if (sched_yield() != 0) {
        return fail("sched_yield did not return 0");
    }
    if (syscall(SYS_sched_getscheduler, 0L) != SCHED_OTHER) {
        return fail("the policy is not SCHED_OTHER");
    }
    sp.sched_priority = -1;
    if (syscall(SYS_sched_getparam, 0L, &sp) != 0 || sp.sched_priority != 0) {
        return fail("the priority is not 0");
    }
    if (set_policy(0, SCHED_OTHER, 0) != 0 ||
        set_policy(0, SCHED_OTHER | SCHED_RESET_ON_FORK, 0) != 0) {
        return fail("SCHED_OTHER at priority 0 was refused");
    }
    sp.sched_priority = 0;
    if (syscall(SYS_sched_setparam, 0L, &sp) != 0) {
        return fail("sched_setparam of priority 0 was refused");
    }
    if (set_policy(0, SCHED_FIFO, 10) != -1 || errno != EPERM ||
        set_policy(0, SCHED_RR, 99) != -1 || errno != EPERM) {
        return fail("a real-time policy was not refused with EPERM");
    }
    if (set_policy(0, SCHED_FIFO, 0) != -1 || errno != EINVAL ||
        set_policy(0, SCHED_OTHER, 10) != -1 || errno != EINVAL ||
        set_policy(0, SCHED_RR, 100) != -1 || errno != EINVAL) {
        return fail("a priority that does not suit its policy was not refused with EINVAL");
    }
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (set_policy(0, refused[i], 0) != -1 || errno != EINVAL) {
            return fail("a policy this kernel does not run was not refused with EINVAL");
        }
    }
    sp.sched_priority = 5;
    if (syscall(SYS_sched_setparam, 0L, &sp) != -1 || errno != EINVAL) {
        return fail("sched_setparam of priority 5 was not refused with EINVAL");
    }
    if (sched_get_priority_max(SCHED_FIFO) != 99 || sched_get_priority_min(SCHED_FIFO) != 1 ||
        sched_get_priority_max(SCHED_RR) != 99 || sched_get_priority_min(SCHED_RR) != 1 ||
        sched_get_priority_max(SCHED_OTHER) != 0 || sched_get_priority_min(SCHED_OTHER) != 0 ||
        sched_get_priority_max(SCHED_IDLE) != 0 || sched_get_priority_min(SCHED_BATCH) != 0) {
        return fail("a priority range is not Linux's");
    }
    if (sched_get_priority_max(42) != -1 || errno != EINVAL) {
        return fail("the range of an unknown policy was not refused with EINVAL");
    }
    if (sched_rr_get_interval(0, &slice) != 0 || slice.tv_sec != 0 ||
        slice.tv_nsec != 10000000L) {
        return fail("the time slice is not one 10 ms tick");
    }
    memset(&set, 0xFF, sizeof(set));
    if (sched_getaffinity(0, sizeof(set), &set) != 0 || CPU_COUNT(&set) != 1 ||
        !CPU_ISSET(0, &set)) {
        return fail("the affinity is not CPU 0 alone");
    }
    if (syscall(SYS_sched_getaffinity, 0L, (long)sizeof(set), &set) != 4) {
        return fail("the raw sched_getaffinity did not return the four bytes it wrote");
    }
    if (syscall(SYS_sched_getaffinity, 0L, 3L, &set) != -1 || errno != EINVAL) {
        return fail("an affinity of three bytes was not refused with EINVAL");
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return fail("an affinity with CPU 0 was refused");
    }
    CPU_ZERO(&set);
    CPU_SET(1, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != -1 || errno != EINVAL) {
        return fail("an affinity without CPU 0 was not refused with EINVAL");
    }
    if (in_child(not_root) != 0) {
        return fail("user 1000 changed the scheduling of root's process");
    }
    if (syscall(SYS_sched_getscheduler, 999999L) != -1 || errno != ESRCH ||
        sched_rr_get_interval(999999, &slice) != -1 || errno != ESRCH ||
        sched_getaffinity(999999, sizeof(set), &set) != -1 || errno != ESRCH) {
        return fail("a pid of no process was not refused with ESRCH");
    }
    if (syscall(SYS_sched_getscheduler, -1L) != -1 || errno != EINVAL) {
        return fail("a negative pid was not refused with EINVAL");
    }
    return 0;
}

int main(void)
{
    if (thread_ids() != 0 || priorities() != 0 || scheduling() != 0) {
        return 1;
    }
    printf("SCHEDMUSL OK\n");

    return 0;
}
