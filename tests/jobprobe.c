/**
 * Exercises the session and process-group identity model from user space.
 *
 * The probe establishes, through raw `int 0x80`, that:
 *   * a process is its own session and group leader by default (getpgrp ==
 *     getsid == getpid for the init process);
 *   * fork inherits the parent's session and group;
 *   * setpgid(0,0) moves a non-leader child into its own group;
 *   * setsid on a non-group-leader child opens a new session (sid == pgid ==
 *     pid);
 *   * the controlling terminal, stop/continue/kill cycle, group signals, and
 *     background-read SIGTTIN all behave;
 *   * the error contract has: setsid by a group leader is -EPERM, getpgid of
 *     a missing pid is -ESRCH, setpgid with a negative pgid is -EINVAL.
 *
 * Children report each check via exit status; the parent decodes WEXITSTATUS.
 * Prints "JOBPROBE OK" or "JOBPROBE FAIL". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_WAITPID   7
#define SYS_GETPID    20
#define SYS_KILL      37
#define SYS_PAUSE     29
#define SYS_SETPGID   57
#define SYS_NANOSLEEP 162
#define SYS_GETPGRP   65
#define SYS_SETSID    66
#define SYS_GETPGID   132
#define SYS_GETSID    147
#define SYS_IOCTL     54

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

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static int getpid(void)
{
    return syscall1(SYS_GETPID, 0);
}
static int getpgrp(void)
{
    return syscall1(SYS_GETPGRP, 0);
}
static int getpgid(int pid)
{
    return syscall1(SYS_GETPGID, pid);
}
static int getsid(int pid)
{
    return syscall1(SYS_GETSID, pid);
}
static int setpgid(int pid, int pgid)
{
    return syscall3(SYS_SETPGID, pid, pgid, 0);
}
static int setsid(void)
{
    return syscall1(SYS_SETSID, 0);
}
static int fork(void)
{
    return syscall1(SYS_FORK, 0);
}
static int waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, pid, (int)(size_t)status, options);
}
/* Long enough for a child that does nothing but exit to have done so. */
struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};
static void sleep_ms(long ms)
{
    struct timespec_probe req;
    int r;

    req.tv_sec = ms / 1000L;
    req.tv_nsec = (ms % 1000L) * 1000000L;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_NANOSLEEP), "b"(&req), "c"(0) : "memory");
    (void)r;
}
static int ioctl(int fd, int req, void *arg)
{
    return syscall3(SYS_IOCTL, fd, req, (int)(size_t)arg);
}
static int kill(int pid, int sig)
{
    return syscall3(SYS_KILL, pid, sig, 0);
}
static int pause(void)
{
    return syscall1(SYS_PAUSE, 0);
}
static int read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, n);
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

/**
 * Runs a forked child's body; its return value becomes the exit status, which
 * the parent decodes. A return of 0 means every in-child check passed.
 */
static int child_setpgid(void)
{
    int me = getpid();
    int parent_pgrp;

    /* Inherited the parent's group + session (parent is the leader). */
    parent_pgrp = getpgrp();
    if (parent_pgrp == me) {
        return 10; /* should still be the parent's group, not our own */
    }
    if (getsid(0) != getsid(me)) {
        return 11;
    }
    /* Move into our own new group. */
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

static int child_setsid(void)
{
    int me = getpid();

    /* Not a group leader yet (inherited the parent's group), so setsid is
     * allowed and opens a new session led by us. */
    if (setsid() != me) {
        return 20;
    }
    if (getsid(0) != me) {
        return 21;
    }
    if (getpgrp() != me) {
        return 22; /* setsid also makes us our group's leader */
    }
    return 0;
}

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
    return (status >> 8) & 0xff; /* WEXITSTATUS */
}

void _start(void)
{
    int me = getpid();

    puts("JOBPROBE START");

    /* The init process leads its own session and group. */
    check(getpgrp() == me, "init getpgrp == pid");
    check(getpgid(0) == me, "init getpgid(0) == pid");
    check(getsid(0) == me, "init getsid(0) == pid");

    /* fork inheritance + setpgid, checked inside the child. */
    check(run_child(child_setpgid) == 0, "child: inherit group, setpgid(0,0) -> own group");

    /* setsid from a non-leader child opens a new session. */
    check(run_child(child_setsid) == 0, "child: setsid -> new session (sid==pgid==pid)");

    /* Controlling terminal on fd 0. We are a session leader, so we may claim
     * the console; the foreground group round-trips through TIOCG/SPGRP. */
    {
        int pgrp = -1;
        check(ioctl(0, TIOCSCTTY, 0) == 0, "TIOCSCTTY claims the console");
        check(ioctl(0, TIOCGPGRP, &pgrp) == 0 && pgrp == getpgrp(),
              "TIOCGPGRP == our process group");
        /* Set our own group as foreground (it is in our session). */
        pgrp = getpgrp();
        check(ioctl(0, TIOCSPGRP, &pgrp) == 0, "TIOCSPGRP to our own group");
        /* A group not in the session is rejected; a bad pgrp is -EINVAL. */
        {
            int bad = 0x6ffffff0;
            check(ioctl(0, TIOCSPGRP, &bad) == -EPERM, "TIOCSPGRP foreign group -> -EPERM");
            bad = -7;
            check(ioctl(0, TIOCSPGRP, &bad) == -EINVAL, "TIOCSPGRP negative pgrp -> -EINVAL");
        }
        /* A group whose only member has exited but has not been reaped is
         * still a group, and a shell putting a pipeline in the foreground
         * depends on it: the first member can exit before the shell claims the
         * terminal for the job, and a rule that counted only running processes
         * would refuse the claim and leave the shell complaining that it cannot
         * set the terminal's process group. */
        {
            int child = fork();
            int status = 0;

            if (child == 0) {
                setpgid(0, 0);
                sys_exit(0);
            }
            check(child > 0, "fork for the zombie group");
            setpgid(child, child);
            /* Let it run to its exit. It is not reaped until after the claim,
             * so what the kernel sees is a group holding one zombie. */
            sleep_ms(50);
            check(ioctl(0, TIOCSPGRP, &child) == 0,
                  "TIOCSPGRP to a group holding only an unreaped zombie");
            check(waitpid(child, &status, 0) == child, "the zombie is reaped");
            /* And once reaped the group is gone, so the same claim is
             * refused. */
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

    /* The stop / continue / kill cycle. A child loops in pause(); the parent
     * stops it, observes the stop, continues it, observes the continue, then
     * kills it; each transition reported through wait4. */
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
            setpgid(0, 0); /* own group == my pid */
            for (;;) {
                pause();
            }
            sys_exit(0);
        }
        if (cpid > 0) {
            /* Set the child's group from the parent too, so the signal below
             * cannot race the child's own setpgid (the fork/setpgid idiom). */
            setpgid(cpid, cpid);
            /* The child's group id equals its pid; signal the whole group. */
            check(kill(-cpid, SIGTERM) == 0, "kill(-pgid, SIGTERM) reaches the group");
            check(waitpid(cpid, &st, 0) == cpid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
                  "the grouped child dies of SIGTERM");
        } else {
            check(0, "fork for kill(-pgid)");
        }
    }

    /* A background process reading its controlling terminal is stopped with
     * SIGTTIN. Re-claim the console (detached above) so our group is in front,
     * then a child that backgrounds itself and reads gets SIGTTIN. */
    {
        int cpid, st, r;

        ioctl(0, TIOCSCTTY, 0); /* our group becomes foreground again */
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
