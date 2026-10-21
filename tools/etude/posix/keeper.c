/*
 * keeper.c: the keeper of one attempt, a process forked from the controller
 * that makes the case's process, holds it until the controller has
 * registered it, reaps it, ends what it leaves, and measures the tree.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "posix.h"

/* The most descendants a keeper names at once. */
#define KEEPER_MAX 256

/* The interval at which a keeper looks again for processes it cannot be
 * told of: members of the case's group that are not its children. */
#define KEEPER_LOOK_MS 10

/* The longest single wait of the keeper's poll, in milliseconds. */
#define KEEPER_WAIT_MAX_MS 60000

/* One attempt, as its keeper sees it. */
struct keeper {
    const struct keeper_args *args;
    pid_t pid;      /* the case's process, its session and its group */
    int alive;      /* the case's process has not been reaped */
    int reaper;     /* this process reaps the orphans below it */
    int controller; /* the controller's pipe is open */
    int wake;       /* the read end of the pipe SIGCHLD writes to */
    int cancelled;
    int held;         /* 1 once SIGSTOP is sent, 2 once the stop is seen */
    int resuming;     /* SIGCONT sent after a stop seen; not yet seen to act */
    uint64_t kill_at; /* when a cancellation sends SIGKILL; 0 for none */
    struct etude_pid reported[KEEPER_MAX];
    int nreported;
    int lost_reports; /* reported descendants beyond KEEPER_MAX */
};

/* The write end of the pipe that SIGCHLD wakes the keeper through. */
static int keeper_wake = -1;

/** Wake the keeper's poll on SIGCHLD. */
static void keeper_sigchld(int sig)
{
    int saved = errno;
    ssize_t n;
    char c = 0;

    (void)sig;
    n = write(keeper_wake, &c, 1);
    (void)n;
    errno = saved;
}

/**
 * Set every signal to its default and clear the mask, as the case's process
 * starts its program.
 */
static void signals_default(void)
{
    sigset_t none;
    int sig;

    for (sig = 1; sig < NSIG; sig++)
        if (sig != SIGKILL && sig != SIGSTOP)
            signal(sig, SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

/**
 * Become the case's process: wait for the keeper's go, then execute the
 * program. A failure writes its errno to `status`, which closes on a
 * successful exec.
 */
static void case_child(const struct keeper_args *k, int go, int status)
{
    const struct etude_exec_spec *s = k->spec;
    ssize_t n;
    char c;
    int e;

    setsid();
    if (dup2(k->in, 0) < 0 || dup2(k->out, 1) < 0 || dup2(k->err, 2) < 0)
        goto fail;
    signals_default();
    if (read(go, &c, 1) != 1)
        _exit(127);
    if (s->cwd != NULL && chdir(s->cwd) != 0)
        goto fail;
    execve(s->argv[0], s->argv, s->envp);
fail:
    e = errno;
    n = write(status, &e, sizeof(e));
    (void)n;
    _exit(127);
}

/**
 * Reap every child that has ended, the case's process among them, and tell
 * the controller how the case's process ended; tell it too when that process
 * has stopped after a KEEP_STOP, or continued after a KEEP_CONT.
 */
static void reap(struct keeper *kp)
{
    int st;
    pid_t w;

    while ((w = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        if (w != kp->pid)
            continue;
        if (WIFSTOPPED(st) && kp->held == 1) {
            kp->held = 2;
            keeper_send(kp->args->ev, KEEP_STOPPED, (int64_t)posix_now_us(), 0, 0);
        }
        if (WIFCONTINUED(st) && kp->resuming) {
            kp->resuming = 0;
            keeper_send(kp->args->ev, KEEP_RESUMED, (int64_t)posix_now_us(), 0, 0);
        }
        if (WIFSTOPPED(st) || WIFCONTINUED(st))
            continue;
        kp->alive = 0;
        if (WIFSIGNALED(st))
            keeper_send(kp->args->ev, KEEP_EXIT, ETUDE_END_SIGNALED, WTERMSIG(st), 0);
        else
            keeper_send(kp->args->ev, KEEP_EXIT, ETUDE_END_EXITED, WEXITSTATUS(st), 0);
    }
}

/** Say whether reported descendant `r` still runs as the same process. */
static int reported_runs(const struct etude_pid *r)
{
    struct etude_pid now;

    if (etude_process_state(r->pid, &now) != 1)
        return 0;
    return now.start == r->start;
}

/**
 * Send `sig` to every process of the case the keeper knows of: the case's
 * group, each reported descendant still the same process, and, where the
 * keeper reaps orphans, each of its children and their groups.
 */
static void signal_all(struct keeper *kp, int sig)
{
    long kids[KEEPER_MAX];
    int i, n;

    kill(-kp->pid, sig);
    for (i = 0; i < kp->nreported; i++)
        if (reported_runs(&kp->reported[i]))
            kill((pid_t)kp->reported[i].pid, sig);
    if (!kp->reaper)
        return;
    n = posix_children(kids, KEEPER_MAX);
    for (i = 0; i < n; i++) {
        kill((pid_t)kids[i], sig);
        kill((pid_t)-kids[i], sig);
    }
}

/**
 * Say whether any process of the case may still run: a member of its
 * group, a reported descendant, or, where the keeper reaps orphans, a child.
 */
static int anything_left(struct keeper *kp)
{
    int i;

    reap(kp);
    if (kp->alive || kill(-kp->pid, 0) == 0 || errno != ESRCH)
        return 1;
    for (i = 0; i < kp->nreported; i++)
        if (reported_runs(&kp->reported[i]))
            return 1;
    if (kp->reaper && (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD))
        return 1;
    return kp->lost_reports;
}

/** Wait for a SIGCHLD for at most `ms` milliseconds, and empty the pipe. */
static void wait_wake(struct keeper *kp, int ms)
{
    struct pollfd p;
    char buf[64];

    p.fd = kp->wake;
    p.events = POLLIN;
    if (poll(&p, 1, ms) > 0)
        while (read(kp->wake, buf, sizeof(buf)) > 0) {
        }
}

/**
 * Wait until nothing of the case is left or `bound_us` passes, sending
 * `again`, when it is not 0, to what is left at each look, so that an orphan
 * that comes to the keeper after the first signal gets it too.
 */
static int drain(struct keeper *kp, uint64_t bound_us, int again)
{
    uint64_t until = posix_now_us() + bound_us;

    while (anything_left(kp)) {
        if (posix_now_us() >= until)
            return 0;
        if (again != 0)
            signal_all(kp, again);
        wait_wake(kp, KEEPER_LOOK_MS);
    }
    return 1;
}

/** Read and discard what pipe `*fd` holds, and at its end mark it -1. */
static void discard(int *fd)
{
    char buf[4096];
    ssize_t n;

    while (*fd >= 0) {
        n = read(*fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            *fd = -1;
        if (n <= 0)
            return;
    }
}

/**
 * Read and discard the case's output until both of its pipes reach their
 * end, or `bound_us` passes.
 *
 * @return 1 when no process holds either pipe open for writing, else 0.
 */
static int outputs_closed(struct keeper *kp, uint64_t bound_us)
{
    uint64_t until = posix_now_us() + bound_us, now, ms;
    struct pollfd p[2];
    int i;

    p[0].fd = kp->args->out_r;
    p[1].fd = kp->args->err_r;
    for (i = 0; i < 2; i++) {
        fcntl(p[i].fd, F_SETFL, O_NONBLOCK);
        p[i].events = POLLIN;
    }
    for (;;) {
        discard(&p[0].fd);
        discard(&p[1].fd);
        now = posix_now_us();
        if (p[0].fd < 0 && p[1].fd < 0)
            return 1;
        if (now >= until)
            return 0;
        ms = (until - now + 999) / 1000;
        poll(p, 2, ms < KEEPER_WAIT_MAX_MS ? (int)ms : KEEPER_WAIT_MAX_MS);
    }
}

/** Record the cleanup in the attempt's verdict file, when it has one. */
static void write_verdict(const struct keeper_args *a, int confirmed)
{
    const char *text = confirmed ? ETUDE_VERDICT_CONFIRMED : ETUDE_VERDICT_UNCONFIRMED;
    ssize_t n;
    int fd;

    if (a->spec->verdict == NULL)
        return;
    fd = open(a->spec->verdict, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
        return;
    n = write(fd, text, strlen(text));
    (void)n;
    close(fd);
}

/**
 * End what the case left, report the usage and the cleanup, and exit. A
 * keeper whose controller has gone also requires the case's output to have
 * closed, as the controller would, and records its finding in the verdict
 * file, which the ledger reads in place of the controller's report.
 */
static void keeper_finish(struct keeper *kp)
{
    uint64_t grace = kp->args->spec->grace_us;
    struct rusage ru;
    uint64_t cpu, peak;
    int confirmed = 1;

    if (anything_left(kp)) {
        signal_all(kp, SIGTERM);
        signal_all(kp, SIGCONT);
        if (!drain(kp, grace, 0)) {
            signal_all(kp, SIGKILL);
            confirmed = drain(kp, grace, SIGKILL);
        }
    }
    getrusage(RUSAGE_CHILDREN, &ru);
    cpu = (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec +
          (uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
#if defined(__APPLE__)
    peak = (uint64_t)ru.ru_maxrss;
#else
    peak = (uint64_t)ru.ru_maxrss * 1024u;
#endif
    if (kp->controller &&
        keeper_send(kp->args->ev, KEEP_USAGE, (int64_t)cpu, kp->reaper && confirmed,
                    (int64_t)peak) == 0 &&
        keeper_send(kp->args->ev, KEEP_CLEANED, confirmed, 0, 0) == 0)
        _exit(0);
    if (kp->args->spec->verdict != NULL)
        write_verdict(kp->args, confirmed && outputs_closed(kp, grace));
    _exit(0);
}

/**
 * Stop the case's process group at the controller's KEEP_STOP, or let it go
 * at its KEEP_CONT. A stop is reported once waitpid(2) has seen it, and a
 * continuation after a stop seen once waitpid(2) has seen that; a SIGCONT
 * with no stop seen is reported at once, since the stop may never have
 * happened. A cancelled or ended case is not stopped.
 */
static void keeper_hold(struct keeper *kp, int type)
{
    if (type == KEEP_STOP && kp->alive && !kp->cancelled && kp->held == 0) {
        kp->held = 1;
        kill(-kp->pid, SIGSTOP);
        return;
    }
    if (type != KEEP_CONT)
        return;
    if (kp->alive)
        kill(-kp->pid, SIGCONT);
    kp->resuming = kp->alive && kp->held == 2;
    kp->held = 0;
    if (!kp->resuming)
        keeper_send(kp->args->ev, KEEP_RESUMED, (int64_t)posix_now_us(), 0, 0);
}

/**
 * Act on one message of the controller's. The end of the controller's pipe
 * cancels the case, since nobody is left to read its results; the SIGCONT
 * of a cancellation lets a stopped case take its SIGTERM.
 */
static void keeper_command(struct keeper *kp)
{
    struct keeper_msg m;

    if (keeper_recv(kp->args->ctl, &m) != 0) {
        kp->controller = 0;
        m.type = KEEP_CANCEL;
    }
    if (m.type == KEEP_CANCEL && !kp->cancelled) {
        kp->cancelled = 1;
        kp->held = 0;
        kp->kill_at = posix_now_us() + kp->args->spec->grace_us;
        signal_all(kp, SIGTERM);
        signal_all(kp, SIGCONT);
    } else if (m.type == KEEP_STOP || m.type == KEEP_CONT) {
        keeper_hold(kp, m.type);
    } else if (m.type == KEEP_CHILD && kp->nreported < KEEPER_MAX) {
        kp->reported[kp->nreported].pid = (long)m.a;
        kp->reported[kp->nreported].start = (uint64_t)m.b;
        kp->reported[kp->nreported++].start_known = 1;
    } else if (m.type == KEEP_CHILD) {
        kp->lost_reports = 1;
    } else if (m.type == KEEP_CLEAN && !kp->alive) {
        keeper_finish(kp);
    }
}

/** Give the milliseconds until a cancellation's kill, or -1 for none due. */
static int kill_timeout(const struct keeper *kp)
{
    uint64_t now = posix_now_us();

    if (kp->kill_at == 0 || !kp->alive)
        return -1;
    if (now >= kp->kill_at)
        return 0;
    if (kp->kill_at - now > KEEPER_WAIT_MAX_MS * 1000u)
        return KEEPER_WAIT_MAX_MS;
    return (int)((kp->kill_at - now + 999) / 1000);
}

/**
 * Give the milliseconds the keeper may wait for an event: until a
 * cancellation's kill, and KEEPER_LOOK_MS at most while a stop or a
 * continuation is still to be seen, since not every system sends SIGCHLD
 * when a child continues.
 */
static int wait_ms(const struct keeper *kp)
{
    int ms = kill_timeout(kp);

    if ((kp->held == 1 || kp->resuming) && kp->alive && (ms < 0 || ms > KEEPER_LOOK_MS))
        return KEEPER_LOOK_MS;
    return ms;
}

/**
 * Serve the case until it has ended and been cleaned up: reap on each
 * SIGCHLD, act on the controller's messages, and kill a cancelled case whose
 * grace has passed.
 */
static void supervise(struct keeper *kp)
{
    struct pollfd p[2];
    int n;

    for (;;) {
        p[0].fd = kp->wake;
        p[0].events = POLLIN;
        p[1].fd = kp->controller ? kp->args->ctl : -1;
        p[1].events = POLLIN;
        p[0].revents = p[1].revents = 0;
        n = poll(p, 2, wait_ms(kp));
        if (n > 0 && p[0].revents != 0)
            wait_wake(kp, 0);
        reap(kp);
        if (n > 0 && p[1].revents != 0)
            keeper_command(kp);
        if (kp->alive && kill_timeout(kp) == 0) {
            signal_all(kp, SIGKILL);
            kp->kill_at = 0;
        }
        if (!kp->alive && !kp->controller)
            keeper_finish(kp);
    }
}

/**
 * Give the keeper its descriptors, each numbered 3 or above and closed on
 * exec, and close every other. A pipe made afterward cannot take 0, 1 or 2
 * from under the case's dup2.
 */
static int keeper_fds(struct keeper_args *a, const struct keeper_args *k)
{
    int keep[7];

    keep[0] = k->ctl;
    keep[1] = k->ev;
    keep[2] = k->in;
    keep[3] = k->out;
    keep[4] = k->err;
    keep[5] = k->out_r;
    keep[6] = k->err_r;
    posix_close_others(keep, 7);
    *a = *k;
    a->ctl = posix_fd_high(k->ctl);
    a->ev = posix_fd_high(k->ev);
    a->in = posix_fd_high(k->in);
    a->out = posix_fd_high(k->out);
    a->err = posix_fd_high(k->err);
    a->out_r = posix_fd_high(k->out_r);
    a->err_r = posix_fd_high(k->err_r);
    return a->ctl < 0 || a->ev < 0 || a->in < 0 || a->out < 0 || a->err < 0 || a->out_r < 0 ||
                   a->err_r < 0
               ? -1
               : 0;
}

/** Make a pipe whose ends are numbered 3 or above and closed on exec. */
static int keeper_pipe(int fds[2])
{
    if (pipe(fds) != 0)
        return -1;
    fds[0] = posix_fd_high(fds[0]);
    fds[1] = posix_fd_high(fds[1]);
    return fds[0] < 0 || fds[1] < 0 ? -1 : 0;
}

/**
 * Give the keeper its own signal dispositions, which it does first, while
 * every signal is still blocked from the controller's fork: ignore SIGPIPE
 * and the stop signals, and set every other signal the controller caught to
 * its default.
 *
 * A handler of the controller's would run in the keeper on the controller's
 * data, writing, say, to a descriptor number the keeper has since given to a
 * pipe of its own. The stop signals are ignored because the keeper must
 * outlive the case to end it: a stop reaches it as the end of the
 * controller's pipe, and a signal sent to every process of a run must not
 * leave the case without its keeper.
 */
static void keeper_dispositions(void)
{
    struct sigaction old;
    int sig;

    for (sig = 1; sig < NSIG; sig++)
        if (sig != SIGKILL && sig != SIGSTOP && sigaction(sig, NULL, &old) == 0 &&
            old.sa_handler != SIG_IGN && old.sa_handler != SIG_DFL)
            signal(sig, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
}

/** Wake the keeper on SIGCHLD through a pipe of its own, and unblock the
 * signals. */
static int keeper_signals(struct keeper *kp)
{
    struct sigaction sa;
    sigset_t none;
    int wake[2];

    if (keeper_pipe(wake) != 0)
        return -1;
    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    fcntl(wake[1], F_SETFL, O_NONBLOCK);
    keeper_wake = wake[1];
    kp->wake = wake[0];
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = keeper_sigchld;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigemptyset(&none);
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        return -1;
    return sigprocmask(SIG_SETMASK, &none, NULL);
}

/**
 * Make the case's process and hold it until the controller says go; then
 * tell the controller whether its exec succeeded.
 *
 * @return 0 when the program runs, or -1 when the attempt ended before it.
 */
static int keeper_launch(struct keeper *kp)
{
    const struct keeper_args *a = kp->args;
    struct etude_pid id;
    struct keeper_msg m;
    int go[2], status[2], e = 0;
    ssize_t n;

    if (keeper_pipe(go) != 0 || keeper_pipe(status) != 0 || (kp->pid = fork()) < 0) {
        keeper_send(a->ev, KEEP_EXEC, errno, 0, 0);
        return -1;
    }
    if (kp->pid == 0) {
        close(go[1]);
        close(status[0]);
        case_child(a, go[0], status[1]);
    }
    posix_untrack_runner();
    kp->alive = 1;
    close(go[0]);
    close(status[1]);
    memset(&id, 0, sizeof(id));
    if (etude_process_state(kp->pid, &id) != 1)
        id.start_known = 0;
    keeper_send(a->ev, KEEP_STARTED, kp->pid, (int64_t)id.start, id.start_known);
    if (keeper_recv(a->ctl, &m) != 0 || m.type != KEEP_GO || write(go[1], "g", 1) != 1) {
        kill(kp->pid, SIGKILL);
        return -1;
    }
    close(go[1]);
    do {
        n = read(status[0], &e, sizeof(e));
    } while (n < 0 && errno == EINTR);
    close(status[0]);
    keeper_send(a->ev, KEEP_EXEC, n == (ssize_t)sizeof(e) ? e : 0, 0, 0);
    return n == (ssize_t)sizeof(e) ? -1 : 0;
}

void keeper_main(const struct keeper_args *k)
{
    struct keeper_args a;
    struct keeper kp;

    keeper_dispositions();
    setsid();
    if (keeper_fds(&a, k) != 0)
        _exit(1);
    memset(&kp, 0, sizeof(kp));
    kp.args = &a;
    kp.controller = 1;
    if (keeper_signals(&kp) != 0) {
        keeper_send(a.ev, KEEP_EXEC, errno, 0, 0);
        _exit(1);
    }
    kp.reaper = !a.spec->posix_only && posix_subreaper() == 0;
    if (keeper_launch(&kp) != 0) {
        while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
        }
        write_verdict(&a, 1);
        _exit(1);
    }
    close(a.in);
    close(a.out);
    close(a.err);
    supervise(&kp);
}
