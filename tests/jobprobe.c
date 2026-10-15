/**
 * jobprobe: sessions and process groups, tested from user space.
 *
 * The probe establishes, through raw system calls, that:
 *   * a process is its own session and group leader by default (getpgrp ==
 *     getsid == getpid for the init process);
 *   * a forked child does not lead its group, and setpgid(0,0) moves it
 *     into a group of its own;
 *   * setsid on a non-group-leader child opens a new session (sid == pgid ==
 *     pid);
 *   * the session leader claims the console as its controlling terminal,
 *     and TIOCSPGRP moves the foreground to a group of the session, one of
 *     a single unreaped zombie included, and refuses any other;
 *   * the stop, continue and kill cycle, a signal to a group, and the
 *     SIGTTIN stop of a background read behave as on Linux;
 *   * the error contract has: setsid by a group leader is -EPERM, getpgid of
 *     a missing pid is -ESRCH, setpgid with a negative pgid is -EINVAL.
 *
 * Children report each check via exit status; the parent decodes WEXITSTATUS,
 * and a child that a signal ended fails its row, whatever its status reads.
 * Prints "JOBPROBE OK" and exits 0 when all pass, and "JOBPROBE FAIL" with
 * the count of failed checks as its status otherwise.
 *
 * Needs: a shell with job control, which makes each command its own
 * process group with the terminal's foreground group set. A run from a
 * non-interactive shell, which is what an SSH command line is, leaves the
 * probe in its parent's group with no terminal of its own, and the rows
 * for the foreground group, a background read and `setsid` by a group
 * leader all read the arrangement rather than the kernel.
 */

#include "probe.h"

#define SIGKILL 9
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTTIN 21

#define WUNTRACED  2
#define WCONTINUED 8

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)

#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCNOTTY 0x5422

#define EPERM  1
#define ESRCH  3
#define EINVAL 22
#define ENOTTY 25

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The caller's pid. */
static int getpid(void)
{
    return syscall1(SYS_GETPID, 0);
}
/** The caller's process group. */
static int getpgrp(void)
{
    return syscall1(SYS_GETPGRP, 0);
}
/** The process group of `pid`, 0 for the caller, or a negated errno. */
static int getpgid(int pid)
{
    return syscall1(SYS_GETPGID, pid);
}
/** The session of `pid`, 0 for the caller, or a negated errno. */
static int getsid(int pid)
{
    return syscall1(SYS_GETSID, pid);
}
/** The setpgid call: 0, or a negated errno. */
static int setpgid(int pid, int pgid)
{
    return syscall3(SYS_SETPGID, pid, pgid, 0);
}
/** The setsid call: the new session, or a negated errno. */
static int setsid(void)
{
    return syscall1(SYS_SETSID, 0);
}
/** The fork call: the child's pid, 0 in the child, or a negated errno. */
static int fork(void)
{
    return syscall1(SYS_FORK, 0);
}
/** Wait for the child `pid`: the pid reaped, or a negated errno. */
static int waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, pid, (long)status, options);
}
/* The kernel's timespec, of longs. */
struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};
/** Sleep for `ms` milliseconds. */
static void sleep_ms(long ms)
{
    struct timespec_probe req;
    int r;

    req.tv_sec = ms / 1000L;
    req.tv_nsec = (ms % 1000L) * 1000000L;
    r = syscall2(SYS_NANOSLEEP, (long)(&req), (long)0);
    (void)r;
}
/** The ioctl call: its result, or a negated errno. */
static int ioctl(int fd, int req, void *arg)
{
    return syscall3(SYS_IOCTL, fd, req, (long)arg);
}
/** The kill call: 0, or a negated errno. */
static int kill(int pid, int sig)
{
    return syscall3(SYS_KILL, pid, sig, 0);
}
/** Wait for a signal; -EINTR when a handler ran. */
static int pause(void)
{
    return syscall1(SYS_PAUSE, 0);
}
/** The read call: the count read, 0 at the end, or a negated errno. */
static int read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static int fails = 0;

/** Count a failure and print `msg` when `cond` is false. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

/**
 * A child's body for `run_child`: it checks that it does not lead its group
 * and that getsid(0) and getsid of its own pid agree, then moves to a group
 * of its own with setpgid(0, 0) and checks getpgrp and getpgid(0). Returns 0
 * when every check passes, or 10 to 14 for the first that fails.
 */
static int child_setpgid(void)
{
    int me = getpid();
    int parent_pgrp;

    parent_pgrp = getpgrp();
    if (parent_pgrp == me) {
        return 10;
    }
    if (getsid(0) != getsid(me)) {
        return 11;
    }
    if (setpgid(0, 0) != 0) {
        return 12;
    }
    if (getpgrp() != me) {
        return 13;
    }
    if (getpgid(0) != me) {
        return 14;
    }
    return 0;
}

/**
 * A child's body for `run_child`: in its parent's group it leads no group,
 * so setsid succeeds and makes it the leader of a new session and of a new
 * group. Returns 0 when every check passes, or 20 to 22 for the first that
 * fails.
 */
static int child_setsid(void)
{
    int me = getpid();

    if (setsid() != me) {
        return 20;
    }
    if (getsid(0) != me) {
        return 21;
    }
    if (getpgrp() != me) {
        return 22;
    }
    return 0;
}

/**
 * Run `body` in a forked child and return the child's exit status: -1 when
 * the fork fails, -2 when the wait does not return the child, and -3 when a
 * signal ended it, whose exit status reads 0.
 */
static int run_child(int (*body)(void))
{
    int pid = fork();
    int status = -1;

    if (pid == 0) {
        sys_exit(body());
    }
    if (pid < 0) {
        return -1;
    }
    if (waitpid(pid, &status, 0) != pid) {
        return -2;
    }
    if ((status & 0x7f) != 0) {
        return -3; /* WTERMSIG */
    }
    return (status >> 8) & 0xff; /* WEXITSTATUS */
}

/**
 * The probe: the rows of the opening comment in order, each counted by
 * `check`.
 *
 * The probe leads its session, so it may claim the console as its
 * controlling terminal. A group whose only member has exited but has not
 * been reaped is still a group, and a shell that puts a pipeline in the
 * foreground depends on it: the first member can exit before the shell
 * claims the terminal for the job. The probe sleeps 50 ms, long enough for
 * a child that does nothing but exit to have done so, claims the terminal
 * for that child's group, and only then reaps it; once it is reaped, the
 * group is gone and the same claim is -EPERM.
 *
 * For the group signal, the parent sets the child's group as the child
 * does, the usual fork and setpgid idiom, so that the signal cannot come
 * before the child's own setpgid. For SIGTTIN, the probe claims the console
 * again after TIOCNOTTY detached it, so that its group is in front, and a
 * child that moves to a group of its own and reads is stopped.
 */
PROBE_START void _start(void)
{
    int me = getpid();

    puts("JOBPROBE START");

    /* The init process leads its own session and group. */
    check(getpgrp() == me, "init getpgrp == pid");
    check(getpgid(0) == me, "init getpgid(0) == pid");
    check(getsid(0) == me, "init getsid(0) == pid");

    /* A forked child and its setpgid(0,0), checked inside the child. */
    check(run_child(child_setpgid) == 0, "child: inherit group, setpgid(0,0) -> own group");

    /* setsid from a non-leader child opens a new session. */
    check(run_child(child_setsid) == 0, "child: setsid -> new session (sid==pgid==pid)");

    /* The controlling terminal on fd 0, claimed by the session leader. */
    {
        int pgrp = -1;
        check(ioctl(0, TIOCSCTTY, 0) == 0, "TIOCSCTTY claims the console");
        check(ioctl(0, TIOCGPGRP, &pgrp) == 0 && pgrp == getpgrp(),
              "TIOCGPGRP == our process group");
        /* The probe's own group, in its session, may be the foreground. */
        pgrp = getpgrp();
        check(ioctl(0, TIOCSPGRP, &pgrp) == 0, "TIOCSPGRP to our own group");
        /* A group not in the session is rejected; a bad pgrp is -EINVAL. */
        {
            int bad = 0x6ffffff0;
            check(ioctl(0, TIOCSPGRP, &bad) == -EPERM, "TIOCSPGRP foreign group -> -EPERM");
            bad = -7;
            check(ioctl(0, TIOCSPGRP, &bad) == -EINVAL, "TIOCSPGRP negative pgrp -> -EINVAL");
        }
        /* A group of one unreaped zombie may still be the foreground. */
        {
            int child = fork();
            int status = 0;

            if (child == 0) {
                setpgid(0, 0);
                sys_exit(0);
            }
            check(child > 0, "fork for the zombie group");
            setpgid(child, child);
            /* The child exits in the sleep, and is reaped after the claim. */
            sleep_ms(50);
            check(ioctl(0, TIOCSPGRP, &child) == 0,
                  "TIOCSPGRP to a group holding only an unreaped zombie");
            check(waitpid(child, &status, 0) == child, "the zombie is reaped");
            /* Once reaped, the group is gone and the same claim is refused. */
            check(ioctl(0, TIOCSPGRP, &child) == -EPERM,
                  "TIOCSPGRP to a group whose zombie was reaped -> -EPERM");
            pgrp = getpgrp();
            check(ioctl(0, TIOCSPGRP, &pgrp) == 0, "the terminal comes back to us");
        }

        /* Detach, then a foreground query finds no controlling session. */
        check(ioctl(0, TIOCNOTTY, 0) == 0, "TIOCNOTTY detaches");
        check(ioctl(0, TIOCGPGRP, &pgrp) == 0 && pgrp == 0, "after NOTTY, fg group is 0");
        check(ioctl(0, TIOCSPGRP, &pgrp) == -ENOTTY, "TIOCSPGRP with no ctty -> -ENOTTY");
    }

    /* The stop, continue and kill cycle, each reported through wait4. */
    {
        int cpid = fork();
        int st;
        int r;

        if (cpid == 0) {
            for (;;) {
                pause();
            }
            sys_exit(0); /* unreachable */
        }
        if (cpid > 0) {
            st = -1;
            check(kill(cpid, SIGSTOP) == 0, "kill SIGSTOP");
            r = waitpid(cpid, &st, WUNTRACED);
            check(r == cpid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP,
                  "wait4(WUNTRACED) reports the SIGSTOP stop");

            check(kill(cpid, SIGCONT) == 0, "kill SIGCONT");
            r = waitpid(cpid, &st, WCONTINUED);
            check(r == cpid && WIFCONTINUED(st), "wait4(WCONTINUED) reports the continue");

            check(kill(cpid, SIGKILL) == 0, "kill SIGKILL");
            r = waitpid(cpid, &st, 0);
            check(r == cpid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
                  "wait4 reports the SIGKILL death");
        } else {
            check(0, "fork for stop/cont");
        }
    }

    /* kill(-pgid): a child in its own group is reached by a group signal. */
    {
        int cpid = fork();
        int st;

        if (cpid == 0) {
            setpgid(0, 0); /* a group of its own, numbered by its pid */
            for (;;) {
                pause();
            }
            sys_exit(0);
        }
        if (cpid > 0) {
            /* The fork and setpgid idiom: the parent sets the group too. */
            setpgid(cpid, cpid);
            /* The child's group id equals its pid; signal the whole group. */
            check(kill(-cpid, SIGTERM) == 0, "kill(-pgid, SIGTERM) reaches the group");
            check(waitpid(cpid, &st, 0) == cpid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
                  "the grouped child dies of SIGTERM");
        } else {
            check(0, "fork for kill(-pgid)");
        }
    }

    /* A background read of the controlling terminal stops with SIGTTIN. */
    {
        int cpid, st, r;

        ioctl(0, TIOCSCTTY, 0); /* the probe's group is the foreground again */
        cpid = fork();
        if (cpid == 0) {
            char b[4];
            setpgid(0, 0);              /* background: not the fg group */
            read(0, b, (int)sizeof(b)); /* background read -> SIGTTIN stop */
            sys_exit(0);
        }
        if (cpid > 0) {
            r = waitpid(cpid, &st, WUNTRACED);
            check(r == cpid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN,
                  "background read -> SIGTTIN stop");
            kill(cpid, SIGKILL);
            waitpid(cpid, &st, 0);
        } else {
            check(0, "fork for SIGTTIN");
        }
        ioctl(0, TIOCNOTTY, 0);
    }

    /* Error contract. */
    check(setsid() == -EPERM, "setsid by a group leader -> -EPERM");
    check(getpgid(0x7ffffff0) == -ESRCH, "getpgid of a missing pid -> -ESRCH");
    check(setpgid(0, -1) == -EINVAL, "setpgid with a negative pgid -> -EINVAL");

    if (fails == 0) {
        puts("JOBPROBE OK");
    } else {
        puts("JOBPROBE FAIL");
    }
    sys_exit(fails);
}
