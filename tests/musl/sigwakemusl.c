/**
 * sigwakemusl: check that a wait woken by a signal ignored at its default
 * drops that signal before it sleeps again, so that a later fatal signal
 * still ends the process, that a child wait drops it while a POSIX timer's
 * signal is posted on another processor, and that kill(-1, 0) from an
 * unprivileged process succeeds when it selects processes it may not
 * signal, as on Linux.
 *
 * First, a child gives up root, if it has it, for user and group 65534, and
 * calls kill(-1, 0). The call selects every process but its own and init,
 * which here are its parent and the system's, all root's, so that none may
 * be signalled; Linux 6.12.115's kill_something_info counts each process it
 * selects and returns 0 when its only errors are EPERM, ESRCH only when it
 * selects none (kernel/signal.c:1631-1645).
 *
 * Second, a process of two threads: its leader blocks SIGCHLD and SIGTERM
 * and sleeps in pause, and its other thread blocks neither and sleeps in a
 * read of a pipe that nobody writes. The leader's child exits. Its SIGCHLD
 * goes to the leader, which blocks it, so it is kept, pending for the
 * process (kernel/signal.c:104-112), and the reader is woken to take it
 * (kernel/signal.c:977-1029). The reader finds it ignored, discards it and
 * goes back to its read, as get_signal discards an ignored signal it
 * dequeues (kernel/signal.c:2838-2839, 2853-2854). The probe sees the
 * reader asleep again, by the voluntary switches /proc counts, with SIGCHLD
 * in neither its pending set nor the process's. Then SIGTERM to the
 * process, which the leader blocks, must be taken by the reader and end the
 * process with that signal. A reader that went back to sleep still marked
 * for the discarded signal would be passed over, and SIGTERM would end
 * nothing; the probe then waits for the process without end, and the run's
 * own bound reports it.
 *
 * Third, on more than one processor, a process with a POSIX timer that
 * sends SIGUSR1 every 50 us: its leader blocks SIGWINCH, takes SIGUSR1 and
 * sends SIGWINCH to the process without pause, while its other thread,
 * which blocks SIGUSR1 and leaves SIGWINCH unblocked, waits for each of a
 * series of children that sleep a millisecond. The leader's block keeps
 * each SIGWINCH, which is ignored at its default, from being dropped as it
 * is sent (kernel/signal.c:104-112), so it goes to the waiting thread and
 * wakes its wait, which discards it before it sleeps again; the discard
 * rearms the process's timers under timerfd's lock, while another
 * processor's timer interrupt posts the timer's SIGUSR1. A wait that
 * discarded the signal under the signal lock without the big lock met the
 * post, which holds the timer lock and takes the signal lock, and the two
 * processors spun for ever; the run's own bound reports such a hang.
 *
 * The probe makes itself a child subreaper, so that the child of the
 * process it ends is reaped here. Writes etude's report records
 * (../probe_report.h) for the case ETUDE_CASE names,
 * guest.default.sigwakemusl without it, and beside them prints
 * "SIGWAKEMUSL OK" with status 0, or "SIGWAKEMUSL FAIL (detail)" with
 * status 1. A failed check ends the program: its assertion fails and those
 * after it are not run. Linked statically against the ported musl.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_report.h"

/* The user and group the kill(-1) child takes, nobody's. */
#define NOBODY 65534

/* The children the third part waits for, each of which sleeps
 * DRAIN_SLEEP_NS, and the interval of its timer. */
#define DRAIN_ROUNDS 2000

#define DRAIN_SLEEP_NS 1000000L

#define DRAIN_TIMER_NS 50000L

extern char **environ;

/* The process under test, the reader's thread ID and the leader's child,
 * for the cleanup of a failure. */
static pid_t victim, reader, orphan;

/* The pipe the reader sleeps on, written by nobody, and the one whose
 * write or close tells the leader's child to exit. */
static int idle[2], go[2] = {-1, -1};

/** End what the probe made, report the path's next assertion failed with
 * `what`, print the failure line and return 1, the failing status. */
static int fail(const char *what)
{
    int status;

    if (go[1] >= 0) {
        close(go[1]);
    }
    if (victim > 0) {
        kill(victim, SIGKILL);
        waitpid(victim, &status, 0);
    }
    if (orphan > 0) {
        waitpid(orphan, &status, 0);
    }
    report_stop(what);
    printf("SIGWAKEMUSL FAIL (%s)\n", what);
    return 1;
}

/** Read the whole of `path` into `buf`, NUL-terminated: the bytes read, or
 * -1 when it cannot be read. */
static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    size_t got = 0;

    if (fd < 0) {
        return -1;
    }
    while (got < size - 1 && (n = read(fd, buf + got, size - 1 - got)) > 0) {
        got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    return (int)got;
}

/** The state letter of thread `tid` of `pid` in its stat file, or 0 when it
 * has none. */
static int task_state(pid_t pid, pid_t tid)
{
    char path[64], line[512], *state;

    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", (int)pid, (int)tid);
    if (read_file(path, line, sizeof(line)) <= 0) {
        return 0;
    }
    state = strrchr(line, ')');
    return state && state[1] == ' ' ? state[2] : 0;
}

/** The hexadecimal field `key` of thread `tid` of `pid`'s status file, in
 * `*value`, or the decimal one with `decimal`: 0, or -1 without it. */
static int task_field(pid_t pid, pid_t tid, const char *key, int decimal, unsigned long long *value)
{
    char path[64], text[4096], *at;

    snprintf(path, sizeof(path), "/proc/%d/task/%d/status", (int)pid, (int)tid);
    if (read_file(path, text, sizeof(text)) <= 0 || (at = strstr(text, key)) == NULL) {
        return -1;
    }
    at += strlen(key);
    while (*at == ':' || *at == '\t' || *at == ' ') {
        at++;
    }
    *value = strtoull(at, NULL, decimal ? 10 : 16);
    return 0;
}

/**
 * Wait until thread `tid` of `pid` is asleep, state S, with more than
 * `after` voluntary switches, looking every 10 ms. Returns 1 then, and 0
 * when the thread ends first.
 */
static int asleep_after(pid_t pid, pid_t tid, unsigned long long after)
{
    unsigned long long switches;
    int state;

    for (;;) {
        state = task_state(pid, tid);
        if (state == 0 || state == 'Z') {
            return 0;
        }
        if (state == 'S' && task_field(pid, tid, "voluntary_ctxt_switches", 1, &switches) == 0 &&
            switches > after) {
            return 1;
        }
        /* interval: a thread's state is looked at again every 10 ms */
        usleep(10000);
    }
}

/** kill(-1, 0) as nobody, in a child: 0 when it returns 0, else 1. */
static int kill_all_unprivileged(char *why, size_t size)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        snprintf(why, size, "fork: %s", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        if (geteuid() == 0 && (setgroups(0, NULL) != 0 || setresgid(NOBODY, NOBODY, NOBODY) != 0 ||
                               setresuid(NOBODY, NOBODY, NOBODY) != 0)) {
            _exit(2);
        }
        if (kill(-1, 0) == 0) {
            _exit(0);
        }
        _exit(errno == EPERM ? 3 : errno == ESRCH ? 4 : 5);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        snprintf(why, size, "the kill(-1) child did not exit");
        return 1;
    }
    switch (WEXITSTATUS(status)) {
    case 0:
        return 0;
    case 2:
        snprintf(why, size, "the child could not give up root");
        return 1;
    case 3:
        snprintf(why, size, "kill(-1, 0) as nobody failed with EPERM");
        return 1;
    case 4:
        snprintf(why, size, "kill(-1, 0) as nobody failed with ESRCH");
        return 1;
    default:
        snprintf(why, size, "kill(-1, 0) as nobody failed with another error");
        return 1;
    }
}

/** The reader: send its thread ID through `arg`'s pipe, then sleep in a
 * read that no write ends. */
static void *read_idle(void *arg)
{
    int *ready = arg;
    pid_t self = (pid_t)syscall(SYS_gettid);
    char c;

    if (write(ready[1], &self, sizeof(self)) != (ssize_t)sizeof(self)) {
        _exit(10);
    }
    for (;;) {
        if (read(idle[0], &c, 1) >= 0 || errno != EINTR) {
            _exit(11);
        }
    }
    return NULL;
}

/**
 * The process under test: start the reader, fork the child that exits when
 * `go` is written or closed, block SIGCHLD and SIGTERM in the leader, send the
 * reader's ID and the child's through `ready`, and sleep. Does not return.
 */
static void run_victim(int ready[2])
{
    pthread_t thread;
    int inner[2];
    pid_t ids[2];
    sigset_t set;
    char c;

    close(ready[0]);
    close(go[1]);
    if (pipe(idle) != 0 || pipe(inner) != 0 ||
        pthread_create(&thread, NULL, read_idle, inner) != 0 ||
        read(inner[0], &ids[0], sizeof(ids[0])) != (ssize_t)sizeof(ids[0])) {
        _exit(12);
    }
    ids[1] = fork();
    if (ids[1] < 0) {
        _exit(13);
    }
    if (ids[1] == 0) {
        while (read(go[0], &c, 1) < 0 && errno == EINTR) {
        }
        _exit(0);
    }
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0 ||
        write(ready[1], ids, sizeof(ids)) != (ssize_t)sizeof(ids)) {
        _exit(14);
    }
    for (;;) {
        pause();
    }
}

/* The third part's count of the timer's signals taken, and the waiter's
 * word to the leader that its waits are over. */
static volatile sig_atomic_t ticks, drained;

/** Count one of the timer's SIGUSR1. */
static void on_tick(int signo)
{
    (void)signo;
    ticks++;
}

/** The waiter: with SIGWINCH unblocked and SIGUSR1 blocked, as the leader
 * left it, wait for DRAIN_ROUNDS children in turn, each of which sleeps
 * DRAIN_SLEEP_NS. Returns NULL when every child exited 0, else the exit
 * status for the process, as a pointer. */
static void *wait_children(void *arg)
{
    struct timespec nap = {0, DRAIN_SLEEP_NS};
    sigset_t set;
    pid_t child;
    int i, status;

    (void)arg;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        drained = 1;
        return (void *)33;
    }
    for (i = 0; i < DRAIN_ROUNDS; i++) {
        child = fork();
        if (child < 0) {
            drained = 1;
            return (void *)34;
        }
        if (child == 0) {
            /* interval: the child lives a millisecond so the wait sleeps */
            while (nanosleep(&nap, &nap) != 0 && errno == EINTR) {
            }
            _exit(0);
        }
        while (waitpid(child, &status, 0) != child) {
            if (errno != EINTR) {
                drained = 1;
                return (void *)35;
            }
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            drained = 1;
            return (void *)36;
        }
    }
    drained = 1;
    return NULL;
}

/**
 * The third part's process: block SIGWINCH and SIGUSR1, arm a timer that
 * sends SIGUSR1 every DRAIN_TIMER_NS, start the waiter, then take SIGUSR1
 * in the leader and send SIGWINCH to the process until the waiter is done.
 * Exits 0 when every child was reaped and the leader took at least one of
 * the timer's signals.
 */
static void run_drain(void)
{
    struct itimerspec every;
    struct sigaction sa;
    struct sigevent ev;
    pthread_t waiter;
    timer_t timer;
    sigset_t set;
    void *rc;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_tick;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    sigaddset(&set, SIGUSR1);
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        _exit(30);
    }
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    every.it_value.tv_sec = 0;
    every.it_value.tv_nsec = DRAIN_TIMER_NS;
    every.it_interval = every.it_value;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) != 0 ||
        timer_settime(timer, 0, &every, NULL) != 0) {
        _exit(31);
    }
    if (pthread_create(&waiter, NULL, wait_children, NULL) != 0) {
        _exit(32);
    }
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        _exit(37);
    }
    while (!drained) {
        if (kill(getpid(), SIGWINCH) != 0) {
            _exit(22);
        }
    }
    if (pthread_join(waiter, &rc) != 0) {
        _exit(38);
    }
    _exit(rc != NULL ? (int)(long)rc : ticks > 0 ? 0 : 39);
}

/** Run the third part's process to its end: 0 when it exits 0, else 1
 * with the reason in `why`. */
static int drain_waits(char *why, size_t size)
{
    int status;

    victim = fork();
    if (victim < 0) {
        snprintf(why, size, "fork of the waiting process: %s", strerror(errno));
        return 1;
    }
    if (victim == 0) {
        run_drain();
    }
    if (waitpid(victim, &status, 0) != victim) {
        snprintf(why, size, "the waiting process could not be reaped");
        return 1;
    }
    victim = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(why, size, "the waiting process ended with status 0x%x", (unsigned)status);
        return 1;
    }
    return 0;
}

int main(void)
{
    unsigned long long switches, pending;
    char why[128];
    int ready[2], status;
    pid_t ids[2];

    report_begin(report_case(environ, "guest.default.sigwakemusl"));
    report_plan("kill-all-unprivileged");
    report_plan("reader-asleep");
    report_plan("ignored-discarded");
    report_plan("sigterm-ends");
    report_plan("wait-discard-timer");

    if (kill_all_unprivileged(why, sizeof(why)) != 0) {
        return fail(why);
    }
    report_held();

    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0 || pipe(ready) != 0 || pipe(go) != 0) {
        return fail("the subreaper or the pipes could not be made");
    }
    victim = fork();
    if (victim < 0) {
        return fail("fork of the process under test");
    }
    if (victim == 0) {
        run_victim(ready);
    }
    close(ready[1]);
    close(go[0]);
    if (read(ready[0], ids, sizeof(ids)) != (ssize_t)sizeof(ids)) {
        return fail("the process under test did not start its reader and child");
    }
    reader = ids[0];
    orphan = ids[1];
    if (!asleep_after(victim, reader, 0) ||
        task_field(victim, reader, "voluntary_ctxt_switches", 1, &switches) != 0) {
        return fail("the reader was never seen asleep in its read");
    }
    report_held();

    if (write(go[1], "x", 1) != 1) {
        return fail("the child was not told to exit");
    }
    if (!asleep_after(victim, reader, switches)) {
        return fail("the reader ended instead of going back to its read");
    }
    if (task_field(victim, reader, "SigPnd", 0, &pending) != 0 ||
        (pending & (1ull << (SIGCHLD - 1))) != 0) {
        return fail("SIGCHLD is still pending for the reader");
    }
    if (task_field(victim, reader, "ShdPnd", 0, &pending) != 0 ||
        (pending & (1ull << (SIGCHLD - 1))) != 0) {
        return fail("SIGCHLD is still pending for the process after its wake");
    }
    report_held();

    if (kill(victim, SIGTERM) != 0 || waitpid(victim, &status, 0) != victim) {
        return fail("SIGTERM could not be sent or the process reaped");
    }
    victim = 0;
    if (waitpid(orphan, NULL, 0) != orphan) {
        orphan = 0;
        return fail("the leader's child was not reaped by the subreaper");
    }
    orphan = 0;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
        return fail("the process did not end by SIGTERM");
    }
    report_held();

    if (drain_waits(why, sizeof(why)) != 0) {
        return fail(why);
    }
    report_held();
    report_end();

    printf("SIGWAKEMUSL OK\n");
    return 0;
}
