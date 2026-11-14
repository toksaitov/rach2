/**
 * jobprobe: sessions and process groups, tested from user space.
 *
 * The probe establishes, through raw system calls, that:
 *   * a process is its own session and group leader by default (getpgrp ==
 *     getsid == getpid for the init process);
 *   * a forked child is in its parent's group and session but does not
 *     lead the group, and setpgid(0,0) moves it into a group of its own;
 *   * setsid on a non-group-leader child opens a new session (sid == pgid ==
 *     pid);
 *   * the session leader claims the console as its controlling terminal,
 *     and TIOCSPGRP moves the foreground to a group of the session, one of
 *     a single unreaped zombie included, or a thread's number in that
 *     session even when it names no group, and refuses any other;
 *   * the TIOCNOTTY of a member that does not lead the session takes the
 *     terminal from its process alone: /dev/tty is then -ENXIO to it and to
 *     a child it forks, and its TIOCGPGRP -ENOTTY, while the session keeps
 *     the terminal;
 *   * the leader's TIOCNOTTY sends SIGHUP to its foreground group and gives
 *     the terminal up, after which TIOCGPGRP, TIOCNOTTY and TIOCSPGRP are
 *     -ENOTTY;
 *   * the end of a session's leader sends its foreground group SIGHUP and,
 *     on the console, hangs up the descriptions of /dev/tty a member opened,
 *     whose non-blocking read then gives 0 and whose write is -EIO, while a
 *     description of /dev/console still writes; on a pty it hangs up none;
 *   * the probe's own group, which is orphaned because it leads its session,
 *     gets -ENOTTY from TIOCSPGRP once it is in the background, and goes
 *     ahead while it ignores SIGTTOU, as a shell does;
 *   * the stop, continue and kill cycle, a signal to a group, and the
 *     SIGTTIN stop of a background read behave as on Linux;
 *   * the error contract has: setsid by a group leader is -EPERM, getpgid of
 *     a missing pid is -ESRCH, setpgid with a negative pgid is -EINVAL,
 *     TIOCSPGRP of a missing group and thread is -ESRCH, and of a negative
 *     number -EINVAL.
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

#define SIGHUP  1
#define SIGKILL 9
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTTIN 21
#define SIGTTOU 22

#define SIG_DFL 0ul
#define SIG_IGN 1ul

#define WNOHANG    1
#define WUNTRACED  2
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000
#define P_PID      1

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
#define KDGETMODE 0x4B3B

#define O_RDWR     2
#define O_NOCTTY   0400
#define O_NONBLOCK 04000

#define EPERM  1
#define ESRCH  3
#define EIO    5
#define ENXIO  6
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
/** The caller's parent's pid. */
static int getppid(void)
{
    return syscall1(SYS_GETPPID, 0);
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
/**
 * Wait until the child `pid` has exited, and leave it unreaped: waitid with
 * WEXITED and WNOWAIT. Whether the call reported the child's exit.
 */
static int wait_exited(int pid)
{
    int info[32]; /* siginfo_t, 128 bytes on both architectures */

    info[0] = 0;
    return syscall5(SYS_WAITID, P_PID, pid, (long)info, WEXITED | WNOWAIT, 0) == 0 &&
           info[0] == SIGCHLD;
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
/* The sigaction rt_sigaction takes, with a 64-bit mask; the handler is a
 * disposition, SIG_DFL or SIG_IGN, held in a word the size of a pointer. */
struct k_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long long mask;
};

/** Set the disposition of `sig` to SIG_DFL or SIG_IGN: 0, or a negated
 * errno. */
static int signal_set(int sig, unsigned long handler)
{
    struct k_sigaction ka;

    ka.handler = handler;
    ka.flags = 0;
    ka.restorer = 0;
    ka.mask = 0;
    return syscall4(SYS_RT_SIGACTION, sig, (long)&ka, 0, 8);
}
/** Block the signals of `mask` (SIG_BLOCK) when `block`, else unblock them
 * (SIG_UNBLOCK): 0, or a negated errno. */
static int sigmask_change(int block, unsigned long long mask)
{
    return syscall4(SYS_RT_SIGPROCMASK, block ? 0 : 1, (long)&mask, 0, 8);
}
/** The caller's pending signals, as rt_sigpending gives them, or 0. */
static unsigned long long sigpending(void)
{
    unsigned long long set = 0;

    syscall2(SYS_RT_SIGPENDING, (long)&set, 8);
    return set;
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}
/** The write call: the count written, or a negated errno. */
static int write(int fd, const void *buf, int n)
{
    return syscall3(SYS_WRITE, fd, (long)buf, n);
}
/** The open call with no mode: a descriptor, or a negated errno. */
static int open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, flags, 0);
}
/** The close call: 0, or a negated errno. */
static int close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}
/** The pipe call: 0 with the read end in fds[0] and the write end in
 * fds[1], or a negated errno. */
static int pipe(int *fds)
{
    return syscall1(SYS_PIPE, (long)fds);
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
 * TIOCNOTTY from the session's leader with its own group in the foreground:
 * the call succeeds, the group, the probe, gets SIGHUP, which the probe
 * blocks and finds pending, and a foreground query then finds the terminal
 * is no longer the caller's controlling terminal (-ENOTTY), as on Linux,
 * where disassociate_ctty sends SIGHUP and SIGCONT to the foreground group
 * and clears the terminal's session. SIGHUP stays ignored after, so the
 * later TIOCNOTTY of `background_read` cannot end the probe.
 */
static void leader_notty(void)
{
    unsigned long long hup = 1ull << (SIGHUP - 1);
    int pgrp = -1;

    check(sigmask_change(1, hup) == 0, "block SIGHUP for TIOCNOTTY");
    check(ioctl(0, TIOCNOTTY, 0) == 0, "TIOCNOTTY detaches");
    check((sigpending() & hup) != 0, "the leader's TIOCNOTTY sends SIGHUP to the foreground group");
    check(signal_set(SIGHUP, SIG_IGN) == 0 && sigmask_change(0, hup) == 0,
          "ignore SIGHUP after TIOCNOTTY");
    check(ioctl(0, TIOCGPGRP, &pgrp) == -ENOTTY, "after NOTTY, TIOCGPGRP -> -ENOTTY");
    check(ioctl(0, TIOCNOTTY, 0) == -ENOTTY, "TIOCNOTTY with no ctty -> -ENOTTY");
    pgrp = getpgrp(); /* a negative number is EINVAL before the check */
    check(ioctl(0, TIOCSPGRP, &pgrp) == -ENOTTY, "TIOCSPGRP with no ctty -> -ENOTTY");
}

/**
 * A child's body for `run_child`: it checks that it is in its parent's group
 * and session and does not lead its group, then moves to a group of its own
 * with setpgid(0, 0) and checks getpgrp and getpgid(0). The parent waits for
 * it, so getpgid and getsid of the parent's pid answer for a live process.
 * Returns 0 when every check passes, or 10 to 15 for the first that fails.
 */
static int child_setpgid(void)
{
    int me = getpid();
    int parent = getppid();

    if (getpgrp() != getpgid(parent)) {
        return 10;
    }
    if (getpgrp() == me) {
        return 11;
    }
    if (getsid(0) != getsid(parent) || getsid(me) != getsid(parent)) {
        return 12;
    }
    if (setpgid(0, 0) != 0) {
        return 13;
    }
    if (getpgrp() != me) {
        return 14;
    }
    if (getpgid(0) != me) {
        return 15;
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

/** Whether /dev/tty is -ENXIO to the caller, which has no controlling
 * terminal; a descriptor it gives is closed. */
static int no_dev_tty(void)
{
    int fd = open("/dev/tty", O_RDWR);

    if (fd >= 0) {
        close(fd);
    }
    return fd == -ENXIO;
}

/**
 * A child's body for `run_child`, in the session whose controlling terminal
 * is on fd 0, which it does not lead: its TIOCNOTTY succeeds and takes the
 * terminal from its process alone, as Linux's no_tty, so /dev/tty is
 * -ENXIO, its TIOCGPGRP -ENOTTY, and a child it forks has no terminal
 * either. Returns 0, or 50 to 54 for the first check that fails.
 */
static int child_notty(void)
{
    int pgrp = -1, pid, status = -1;

    if (ioctl(0, TIOCNOTTY, 0) != 0) {
        return 50;
    }
    if (!no_dev_tty()) {
        return 51;
    }
    if (ioctl(0, TIOCGPGRP, &pgrp) != -ENOTTY) {
        return 52;
    }
    pid = fork();
    if (pid == 0) {
        sys_exit(no_dev_tty() ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return 53;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 54;
}

/**
 * A member's TIOCNOTTY, while the probe's session controls fd 0: the member
 * loses the terminal (`child_notty`), and the session keeps it, with the
 * probe's group in front.
 */
static void member_notty(void)
{
    int pgrp = -1;

    check(run_child(child_notty) == 0, "a member's TIOCNOTTY takes the terminal from it alone");
    check(ioctl(0, TIOCGPGRP, &pgrp) == 0 && pgrp == getpgrp(),
          "the session keeps its terminal after a member's TIOCNOTTY");
}

/**
 * The member of `leader_hangup`, in the session of the leader that forked
 * it and in its group: with SIGHUP blocked, it opens /dev/tty, the
 * terminal on fd 0, non-blocking, writes 'r' to `up` and waits for a byte
 * on `go`, which comes once the leader has ended. It then writes to `up`
 * '0' when SIGHUP is pending and the /dev/tty description is as Linux
 * leaves it: on the console, which answers KDGETMODE, hung up, its read
 * giving 0 and its write -EIO, while a description of /dev/console still
 * writes; on a pty, not hung up. Otherwise it writes a letter for the first
 * check that fails. It does not return.
 */
static void hangup_member(int up, int go)
{
    unsigned long long hup = 1ull << (SIGHUP - 1);
    int tty, cons = -1, mode = 0, console;
    char byte;

    console = ioctl(0, KDGETMODE, &mode) == 0;
    if (sigmask_change(1, hup) != 0) {
        sys_exit(1);
    }
    tty = open("/dev/tty", O_RDWR | O_NONBLOCK);
    if (console) {
        cons = open("/dev/console", O_RDWR | O_NOCTTY | O_NONBLOCK);
    }
    if (tty < 0 || (console && cons < 0) || write(up, "r", 1) != 1 || read(go, &byte, 1) != 1) {
        sys_exit(1);
    }
    if ((sigpending() & hup) == 0) {
        byte = 'a';
    } else if (!console) {
        byte = write(tty, "", 0) == 0 ? '0' : 'b';
    } else if (read(tty, &byte, 1) != 0) {
        byte = 'c';
    } else {
        byte = write(tty, "", 0) != -EIO ? 'd' : write(cons, "", 0) != 0 ? 'e' : '0';
    }
    write(up, &byte, 1);
    sys_exit(0);
}

/**
 * The leader of `leader_hangup`: a new session's leader, it makes the
 * terminal on fd 0, which no session holds, its controlling terminal, forks
 * `hangup_member`, closes the pipe ends it does not use, waits for the
 * member's 'r' on `up` and exits 0, or 3 for a step that failed. It does
 * not return.
 */
static void hangup_leader(int *up, int *go)
{
    int member = -1;
    char byte;

    if (setsid() >= 0 && ioctl(0, TIOCSCTTY, 0) == 0) {
        member = fork();
    }
    if (member == 0) {
        close(up[0]);
        close(go[1]);
        hangup_member(up[1], go[0]);
    }
    close(up[1]);
    close(go[0]);
    close(go[1]);
    sys_exit(member > 0 && read(up[0], &byte, 1) == 1 && byte == 'r' ? 0 : 3);
}

/**
 * The end of a session's leader, while the probe holds no terminal: a
 * leader takes the terminal on fd 0 and ends, and the member it leaves,
 * the foreground group, finds SIGHUP pending. On the console, as in the
 * default suite, the description of /dev/tty that the member opened is
 * hung up and a description of /dev/console is not, as Linux's
 * disassociate_ctty calls tty_vhangup_session for any terminal but a pty,
 * whose hangup passes over /dev/console; on a pty, as in a native run, the
 * description is not hung up (`hangup_member`). Every process closes the
 * pipe ends it does not use, so a reader sees the end of a process that
 * failed; SIGPIPE is ignored for the write to a member that ended. The
 * member, orphaned by the leader's end, is the probe's to reap when the
 * probe is init, as in the default suite, and otherwise init's, when the
 * wait for any child finds none.
 */
static void leader_hangup(void)
{
    int up[2] = {-1, -1}, go[2] = {-1, -1};
    int leader, status = -1;
    char byte = 0;

    check(signal_set(SIGPIPE, SIG_IGN) == 0 && pipe(up) == 0 && pipe(go) == 0,
          "pipes for the leader's end");
    leader = fork();
    if (leader == 0) {
        hangup_leader(up, go);
    }
    close(up[1]);
    close(go[0]);
    check(leader > 0 && waitpid(leader, &status, 0) == leader && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "a leader takes the terminal and forks a member");
    check(write(go[1], "g", 1) == 1 && read(up[0], &byte, 1) == 1 && byte == '0',
          "the leader's end hangs up the console's /dev/tty, but not /dev/console or a pty");
    close(go[1]);
    waitpid(-1, &status, 0);
    close(up[0]);
    check(signal_set(SIGPIPE, SIG_DFL) == 0, "SIGPIPE back to its default");
}

/**
 * The rows of a zombie's group, run while the probe's group is the
 * foreground of the console it controls, which it is again at the end.
 *
 * A group whose only member has exited but has not been reaped is still a
 * group, and a shell that puts a pipeline in the foreground depends on it:
 * the first member can exit before the shell claims the terminal for the
 * job. The probe waits until a child that does nothing but exit has done
 * so, leaving it unreaped, claims the terminal for the child's group, and
 * only then reaps it; once it is reaped, the group is gone and the same
 * claim is -ESRCH.
 *
 * Once the zombie's group is in front, the probe is a background process,
 * and its own group is orphaned: the probe leads its session, so no member's
 * parent is in the session outside the group. Its TIOCSPGRP is therefore
 * -ENOTTY, as on Linux, and it ignores SIGTTOU for the claims that follow,
 * as a shell does when it takes the terminal back from a job.
 */
static void zombie_group(void)
{
    int child = fork();
    int status = 0;
    int pgrp = getpgrp();

    if (child == 0) {
        setpgid(0, 0);
        sys_exit(0);
    }
    check(child > 0, "fork for the zombie group");
    setpgid(child, child);
    /* The child is seen exited, and is reaped after the claim. */
    check(wait_exited(child), "the child of the zombie group exits");
    check(ioctl(0, TIOCSPGRP, &child) == 0, "TIOCSPGRP to a group holding only an unreaped zombie");
    check(ioctl(0, TIOCSPGRP, &pgrp) == -ENOTTY,
          "TIOCSPGRP from the orphaned background group -> -ENOTTY");
    check(signal_set(SIGTTOU, SIG_IGN) == 0, "ignore SIGTTOU");
    check(waitpid(child, &status, 0) == child, "the zombie is reaped");
    /* Once reaped, the group is gone and the same claim is refused. */
    check(ioctl(0, TIOCSPGRP, &child) == -ESRCH,
          "TIOCSPGRP to a group whose zombie was reaped -> -ESRCH");
    check(ioctl(0, TIOCSPGRP, &pgrp) == 0, "the terminal comes back to us");
    check(signal_set(SIGTTOU, SIG_DFL) == 0, "SIGTTOU back to its default");
}

/**
 * A PID in this session is accepted even when it names no group, as Linux's
 * session_of_pgrp falls back to the thread; an existing group in another
 * session is EPERM. Keep both children unreaped through the lookup, then
 * reclaim the foreground and reap them before restoring SIGTTOU.
 */
static void group_numbers(void)
{
    int other, child, status, pgrp = getpgrp();

    check(signal_set(SIGTTOU, SIG_IGN) == 0, "ignore SIGTTOU for PID foreground claims");
    for (other = 0; other <= 1; other++) {
        child = fork();
        if (child == 0) {
            if (other && setsid() < 0) {
                sys_exit(1);
            }
            sys_exit(0);
        }
        check(child > 0, "fork for foreground number lookup");
        if (child <= 0) {
            continue;
        }
        check(wait_exited(child), "keep the foreground number's child unreaped");
        check(ioctl(0, TIOCSPGRP, &child) == (other ? -EPERM : 0),
              other ? "TIOCSPGRP existing foreign group -> -EPERM"
                    : "TIOCSPGRP PID in this session without a group succeeds");
        check(ioctl(0, TIOCSPGRP, &pgrp) == 0, "restore the probe's foreground group");
        status = -1;
        check(waitpid(child, &status, 0) == child && status == 0,
              "reap the foreground number's child after its lookup");
    }
    check(signal_set(SIGTTOU, SIG_DFL) == 0, "restore SIGTTOU after PID foreground claims");
}

/**
 * The reader of `background_read`: in a group of its own, a read of the
 * terminal stops it with SIGTTIN. It does not return.
 */
static void background_reader(void)
{
    char b[4];

    setpgid(0, 0);              /* background: not the fg group */
    read(0, b, (int)sizeof(b)); /* background read -> SIGTTIN stop */
    sys_exit(0);
}

/**
 * Check that a background read of the controlling terminal stops with
 * SIGTTIN. The reader's group must not be orphaned, or the read is EIO, as
 * Linux's __tty_check_change has it; a member whose parent is init does not
 * keep a group from being orphaned (will_become_orphaned_pgrp), and the probe
 * is init in the default suite. So a helper in the probe's group forks the
 * reader, whose parent is then in another group of the session, and reports
 * through its status: 0 for the stop, 1 for no stop, 2 for a failed fork.
 * The probe's group is made the foreground first, and the terminal is given
 * up after.
 */
static void background_read(void)
{
    int hpid, st;

    ioctl(0, TIOCSCTTY, 0);
    hpid = fork();
    if (hpid == 0) {
        int cpid, r, ok;

        cpid = fork();
        if (cpid == 0) {
            background_reader();
        }
        if (cpid < 0) {
            sys_exit(2);
        }
        setpgid(cpid, cpid);
        r = waitpid(cpid, &st, WUNTRACED);
        ok = r == cpid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN;
        kill(cpid, SIGKILL);
        waitpid(cpid, &st, 0);
        sys_exit(ok ? 0 : 1);
    }
    if (hpid > 0) {
        st = -1;
        check(waitpid(hpid, &st, 0) == hpid && WIFEXITED(st) && WEXITSTATUS(st) != 2,
              "fork for SIGTTIN");
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "background read -> SIGTTIN stop");
    } else {
        check(0, "fork for SIGTTIN");
    }
    ioctl(0, TIOCNOTTY, 0);
}

/**
 * The probe: the rows of the opening comment in order, each counted by
 * `check`.
 *
 * The probe leads its session, so it may claim the console as its
 * controlling terminal, and gives it to a zombie's group (`zombie_group`).
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
        /* A missing number is ESRCH; a negative number is EINVAL. */
        {
            int bad = 0x6ffffff0;
            check(ioctl(0, TIOCSPGRP, &bad) == -ESRCH, "TIOCSPGRP missing group -> -ESRCH");
            bad = 0;
            check(ioctl(0, TIOCSPGRP, &bad) == -ESRCH, "TIOCSPGRP zero pgrp -> -ESRCH");
            bad = -7;
            check(ioctl(0, TIOCSPGRP, &bad) == -EINVAL, "TIOCSPGRP negative pgrp -> -EINVAL");
        }
        group_numbers();
        zombie_group();
        member_notty();

        leader_notty();
    }
    leader_hangup();

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

    background_read();

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
