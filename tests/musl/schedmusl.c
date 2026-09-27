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
 * Prints "SCHEDMUSL OK" on success and "SCHEDMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
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

int main(void)
{
    if (thread_ids() != 0 || priorities() != 0) {
        return 1;
    }
    printf("SCHEDMUSL OK\n");

    return 0;
}
