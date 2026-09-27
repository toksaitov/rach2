/**
 * waitid(2) and the siginfo it fills, read through musl.
 *
 * Each row forks the children it waits for and judges the fields the kernel
 * wrote: an exit reports CLD_EXITED with the exit status, a death by a
 * signal CLD_KILLED with the signal, a stop CLD_STOPPED with the stop signal
 * and a continue CLD_CONTINUED with SIGCONT, each with SIGCHLD, the child's
 * pid and its real uid. WNOHANG with nothing to report returns 0 with si_pid
 * 0 and leaves the resource usage alone; WNOWAIT reports an exit and leaves
 * the zombie for `waitpid` to reap, and a stop it left reports no more once
 * the child has been killed. A child is selected by pid, by its process
 * group and by the caller's, each beside an exited child of another group,
 * whose exit a wait with WNOHANG passes over; once the selected child is
 * killed, the wait reports it and reaps it alone. The raw call's fifth
 * argument receives the CPU time of a child that spun. No event, an unknown
 * option or idtype, a pid of 0 and a pidfd are refused, and no child at all
 * is ECHILD.
 *
 * Prints "WAITIDMUSL OK" on success and "WAITIDMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>

/* The i386 kernel's struct rusage, which the raw call writes: two timevals
 * of 32-bit seconds and microseconds, then fourteen counters. */
struct rusage32 {
    long utime_sec;
    long utime_usec;
    long stime_sec;
    long stime_usec;
    long counters[14];
};

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("WAITIDMUSL FAIL (%s)\n", what);

    return 1;
}

/** A child that waits for signals until it is killed. */
static pid_t idle_child(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    return pid;
}

/** A child that exits with `code` at once. */
static pid_t exiting_child(int code)
{
    pid_t pid = fork();

    if (pid == 0) {
        _exit(code);
    }
    return pid;
}

/** Spend a while in user mode without a system call; the sum is what the
 * loop computed, returned so the compiler keeps the loop. */
static unsigned int spin(void)
{
    volatile unsigned int x = 0;
    unsigned int i;

    for (i = 0; i < 200000u; i++) {
        x += i;
    }
    return x;
}

/** A child that spins until the kernel has charged it `ticks` of user time,
 * or for a thousand ticks, then exits with status 0. */
static pid_t spinning_child(long ticks)
{
    pid_t pid = fork();

    if (pid == 0) {
        struct tms tm;
        clock_t start = times(&tm);

        while ((long)tm.tms_utime < ticks && (long)(times(&tm) - start) < 1000) {
            spin();
        }
        _exit(0);
    }
    return pid;
}

/** Whether `si` reports `code` and `status` for child `pid`. */
static int reports(const siginfo_t *si, pid_t pid, int code, int status)
{
    return si->si_signo == SIGCHLD && si->si_code == code && si->si_pid == pid &&
           si->si_uid == getuid() && si->si_status == status;
}

/** A stop that WNOWAIT left, then a SIGKILL: once the child has died, a wait
 * for a stop finds none. */
static int dead_stop(void)
{
    siginfo_t si;
    pid_t pid = idle_child();
    int status;

    kill(pid, SIGSTOP);
    if (waitid(P_PID, (id_t)pid, &si, WSTOPPED | WNOWAIT) != 0 ||
        !reports(&si, pid, CLD_STOPPED, SIGSTOP)) {
        kill(pid, SIGKILL);
        return fail("WNOWAIT did not report the stop");
    }
    kill(pid, SIGKILL);
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT) != 0 ||
        !reports(&si, pid, CLD_KILLED, SIGKILL)) {
        return fail("WNOWAIT did not report the death");
    }
    memset(&si, 0xFF, sizeof(si));
    if (waitid(P_PID, (id_t)pid, &si, WSTOPPED | WNOHANG) != 0 || si.si_pid != 0) {
        return fail("a child that died was reported stopped");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return fail("the killed child was not reaped");
    }
    return 0;
}

/** Exits, deaths, stops and continues, and the fields each fills. */
static int events(void)
{
    siginfo_t si;
    pid_t pid;

    pid = exiting_child(7);
    memset(&si, 0xFF, sizeof(si));
    if (waitid(P_PID, (id_t)pid, &si, WEXITED) != 0 || !reports(&si, pid, CLD_EXITED, 7)) {
        return fail("an exit did not report CLD_EXITED with its status");
    }
    pid = idle_child();
    kill(pid, SIGTERM);
    if (waitid(P_PID, (id_t)pid, &si, WEXITED) != 0 || !reports(&si, pid, CLD_KILLED, SIGTERM)) {
        return fail("a death by SIGTERM did not report CLD_KILLED with the signal");
    }

    pid = idle_child();
    memset(&si, 0xFF, sizeof(si));
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG) != 0 || si.si_pid != 0 ||
        si.si_signo != 0) {
        kill(pid, SIGKILL);
        return fail("WNOHANG with nothing to report did not return 0 with si_pid 0");
    }
    kill(pid, SIGSTOP);
    if (waitid(P_PID, (id_t)pid, &si, WSTOPPED) != 0 || !reports(&si, pid, CLD_STOPPED, SIGSTOP)) {
        kill(pid, SIGKILL);
        return fail("a stop did not report CLD_STOPPED with the signal");
    }
    kill(pid, SIGCONT);
    if (waitid(P_PID, (id_t)pid, &si, WCONTINUED) != 0 ||
        !reports(&si, pid, CLD_CONTINUED, SIGCONT)) {
        kill(pid, SIGKILL);
        return fail("a continue did not report CLD_CONTINUED with SIGCONT");
    }
    kill(pid, SIGKILL);
    if (waitid(P_PID, (id_t)pid, &si, WEXITED) != 0 || !reports(&si, pid, CLD_KILLED, SIGKILL)) {
        return fail("a death by SIGKILL was not reported");
    }
    return dead_stop();
}

/**
 * A wait that selects an idle child, `mine`, beside an exited child of a
 * group of its own: with P_PID by `mine`'s pid, and with P_PGID by `mine`'s
 * group, one of its own, or, with `callers`, by the caller's group, which
 * `mine` keeps. With WNOHANG the wait passes over the other child's exit and
 * reports nothing; once `mine` is killed the wait reports and reaps `mine`
 * alone, and a wait by pid then reaps the other. Returns 0, or 1 once the
 * failure is reported.
 */
static int selects(idtype_t idtype, int callers)
{
    siginfo_t si;
    pid_t other, mine;
    id_t id;

    other = fork();
    if (other == 0) {
        setpgid(0, 0);
        _exit(4);
    }
    setpgid(other, other);
    mine = fork();
    if (mine == 0) {
        for (;;) {
            pause();
        }
    }
    id = idtype == P_PID || !callers ? (id_t)mine : 0;
    if (other < 0 || mine < 0 || (!callers && setpgid(mine, mine) != 0) ||
        waitid(P_PID, (id_t)other, &si, WEXITED | WNOWAIT) != 0 ||
        !reports(&si, other, CLD_EXITED, 4)) {
        if (mine > 0) {
            kill(mine, SIGKILL);
        }
        return fail("the two children of a selection");
    }
    memset(&si, 0xFF, sizeof(si));
    if (waitid(idtype, id, &si, WEXITED | WNOHANG) != 0 || si.si_pid != 0) {
        kill(mine, SIGKILL);
        return fail("a wait reported the exit of a child it does not select");
    }
    kill(mine, SIGTERM);
    if (waitid(idtype, id, &si, WEXITED) != 0 || !reports(&si, mine, CLD_KILLED, SIGTERM) ||
        waitid(P_PID, (id_t)mine, &si, WEXITED | WNOHANG) != -1 || errno != ECHILD) {
        return fail("a wait did not report and reap the child it selects");
    }
    if (waitid(P_PID, (id_t)other, &si, WEXITED) != 0 || !reports(&si, other, CLD_EXITED, 4)) {
        return fail("the other child's exit was not left to a wait by its pid");
    }
    return 0;
}

/** WNOWAIT, the selections, the resource usage and the refusals. */
static int selections(void)
{
    struct rusage32 ru;
    siginfo_t si;
    pid_t pid;
    int status;

    pid = exiting_child(3);
    if (waitid(P_ALL, 0, &si, WEXITED | WNOWAIT) != 0 || !reports(&si, pid, CLD_EXITED, 3)) {
        return fail("WNOWAIT did not report the exit");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 3) {
        return fail("WNOWAIT did not leave the zombie to be reaped");
    }

    if (selects(P_PID, 0) != 0 || selects(P_PGID, 0) != 0 || selects(P_PGID, 1) != 0) {
        return 1;
    }

    pid = spinning_child(2);
    memset(&ru, 0xFF, sizeof(ru));
    if (syscall(SYS_waitid, (long)P_PID, (long)pid, &si, (long)WEXITED, &ru) != 0 ||
        !reports(&si, pid, CLD_EXITED, 0)) {
        return fail("the raw waitid did not reap the spinning child");
    }
    if (ru.utime_sec * 100 + ru.utime_usec / 10000 < 2 || ru.counters[0] != 0) {
        return fail("the raw waitid's usage is not the child's CPU time");
    }
    pid = idle_child();
    memset(&ru, 0xFF, sizeof(ru));
    if (syscall(SYS_waitid, (long)P_PID, (long)pid, &si, (long)(WEXITED | WNOHANG), &ru) != 0 ||
        ru.utime_sec != -1) {
        kill(pid, SIGKILL);
        return fail("WNOHANG with nothing to report wrote the usage");
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);

    if (waitid(P_ALL, 0, &si, 0) != -1 || errno != EINVAL) {
        return fail("a waitid with no event was not refused with EINVAL");
    }
    if (waitid(P_ALL, 0, &si, WEXITED | 0x100) != -1 || errno != EINVAL) {
        return fail("an unknown option was not refused with EINVAL");
    }
    if (waitid((idtype_t)7, 0, &si, WEXITED) != -1 || errno != EINVAL) {
        return fail("an unknown idtype was not refused with EINVAL");
    }
    if (waitid(P_PID, 0, &si, WEXITED) != -1 || errno != EINVAL) {
        return fail("a pid of 0 was not refused with EINVAL");
    }
    if (waitid(P_PIDFD, 3, &si, WEXITED) != -1 || errno != EBADF) {
        return fail("a pidfd was not refused with EBADF");
    }
    if (waitid(P_ALL, 0, &si, WEXITED) != -1 || errno != ECHILD) {
        return fail("a waitid with no children was not refused with ECHILD");
    }
    return 0;
}

int main(void)
{
    if (events() != 0 || selections() != 0) {
        return 1;
    }
    printf("WAITIDMUSL OK\n");

    return 0;
}
