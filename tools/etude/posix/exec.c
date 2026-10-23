/*
 * exec.c: the controller side of the POSIX executor. It forks a keeper for
 * each attempt, registers the case's process the keeper reports, reads the
 * case's output and the keeper's messages for every attempt at once, and
 * keeps what it observes in the attempt's result.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../capture.h"
#include "posix.h"

/* The method identities of the measurements, as the history records them. */
#define METHOD_CPU_EXACT "posix-subreaper-rusage-v1"
#define METHOD_CPU_LOWER "posix-rusage-children-v1"
#define METHOD_PEAK      "posix-maxrss-v1"

struct etude_proc {
    struct etude_exec *ex;
    struct etude_proc *next;
    char *attempt_id;
    size_t output_max;
    uint64_t grace_us;
    etude_child_fn child;
    void *reg_ctx;
    pid_t keeper;
    int ctl, ev, out, err; /* -1 once closed */
    struct keeper_msg msg; /* a message of the keeper's being read */
    size_t msg_len;
    struct capture_lines lines;
    struct capture_files files;
    int cancelled;
    int unverified;       /* a `child` record named a process of unknown start */
    int exited;           /* the keeper reported the end of the case's process */
    int cleaned;          /* the keeper reported its cleanup */
    int reaped;           /* the keeper has ended and been reaped */
    uint64_t drain_until; /* after the keeper's end, when open output
                             proves a process of the case still runs */
    int noticed;          /* the keeper reported a change of the suspension */
    int finished;
    int returned;
    struct etude_exec_result r;
};

struct etude_exec {
    struct etude_proc *procs;
};

struct etude_exec *etude_exec_new(void)
{
    signal(SIGPIPE, SIG_IGN);
    return calloc(1, sizeof(struct etude_exec));
}

/** Make an attempt with no processes and no descriptors. */
static struct etude_proc *proc_new(const struct etude_exec_spec *spec)
{
    struct etude_proc *p = calloc(1, sizeof(*p));

    if (p == NULL)
        return NULL;
    p->ctl = p->ev = p->out = p->err = -1;
    p->keeper = -1;
    p->output_max = spec->output_max;
    p->grace_us = spec->grace_us;
    p->child = spec->child;
    p->reg_ctx = spec->reg_ctx;
    p->attempt_id = malloc(strlen(spec->attempt_id) + 1);
    if (p->attempt_id == NULL || capture_init(&p->r.out, spec->output_max) != 0 ||
        capture_init(&p->r.err, spec->output_max) != 0) {
        free(p->attempt_id);
        capture_free(&p->r.out);
        free(p);
        return NULL;
    }
    if (capture_files_open(&p->files, spec) != 0) {
        free(p->attempt_id);
        capture_free(&p->r.out);
        capture_free(&p->r.err);
        free(p);
        return NULL;
    }
    strcpy(p->attempt_id, spec->attempt_id);
    p->r.attempt_id = p->attempt_id;
    p->r.end = p->r.exit_end = ETUDE_END_LOST;
    p->r.value = p->r.exit_value = -1;
    p->r.cpu_us.method = p->r.peak_bytes.method = "the keeper ended before it measured the tree";
    p->r.started_us = posix_now_us();
    return p;
}

/** Close descriptor `*fd` if it is open, and mark it closed. */
static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

/** Make a pipe whose ends are closed on exec. */
static int cloexec_pipe(int fds[2])
{
    if (pipe(fds) != 0)
        return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

/* The descriptors of a keeper's start, in the order of `fds` below. */
enum { FD_IN, FD_CTL_R, FD_CTL_W, FD_EV_R, FD_EV_W, FD_OUT_R, FD_OUT_W, FD_ERR_R, FD_ERR_W, FD_N };

/**
 * Fork the attempt's keeper, with the pipes between them and the case's
 * standard input open. Every signal is blocked across the fork, so that no
 * handler of the controller's runs in the keeper before the keeper has set
 * its own.
 */
static int proc_spawn(struct etude_proc *p, const struct etude_exec_spec *spec)
{
    const char *in = spec->stdin_path != NULL ? spec->stdin_path : "/dev/null";
    int fds[FD_N], i, e;
    struct keeper_args k;
    sigset_t all, old;

    for (i = 0; i < FD_N; i++)
        fds[i] = -1;
    fds[FD_IN] = strcmp(in, "-") == 0 ? dup(STDIN_FILENO) : open(in, O_RDONLY | O_CLOEXEC);
    if (fds[FD_IN] >= 0)
        fcntl(fds[FD_IN], F_SETFD, FD_CLOEXEC);
    if (fds[FD_IN] < 0 || cloexec_pipe(&fds[FD_CTL_R]) != 0 || cloexec_pipe(&fds[FD_EV_R]) != 0 ||
        cloexec_pipe(&fds[FD_OUT_R]) != 0 || cloexec_pipe(&fds[FD_ERR_R]) != 0) {
        e = errno;
        for (i = 0; i < FD_N; i++)
            close_fd(&fds[i]);
        errno = e;
        return -1;
    }
    sigfillset(&all);
    sigprocmask(SIG_SETMASK, &all, &old);
    p->keeper = fork();
    if (p->keeper < 0) {
        e = errno;
        sigprocmask(SIG_SETMASK, &old, NULL);
        for (i = 0; i < FD_N; i++)
            close_fd(&fds[i]);
        errno = e;
        return -1;
    }
    if (p->keeper == 0) {
        k.spec = spec;
        k.ctl = fds[FD_CTL_R];
        k.ev = fds[FD_EV_W];
        k.in = fds[FD_IN];
        k.out = fds[FD_OUT_W];
        k.err = fds[FD_ERR_W];
        k.out_r = fds[FD_OUT_R];
        k.err_r = fds[FD_ERR_R];
        keeper_main(&k);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    p->ctl = fds[FD_CTL_W];
    p->ev = fds[FD_EV_R];
    p->out = fds[FD_OUT_R];
    p->err = fds[FD_ERR_R];
    close(fds[FD_IN]);
    close(fds[FD_CTL_R]);
    close(fds[FD_EV_W]);
    close(fds[FD_OUT_W]);
    close(fds[FD_ERR_W]);
    if (etude_process_state(p->keeper, &p->r.keeper) != 1)
        p->r.keeper.pid = p->keeper;
    return 0;
}

/**
 * Take the case's process the keeper made, register it, let it run, and
 * learn whether its exec succeeded.
 */
static int proc_handshake(struct etude_proc *p, const struct etude_exec_spec *spec)
{
    struct keeper_msg m;
    int rc;

    if (keeper_recv(p->ev, &m) != 0 || (m.type != KEEP_STARTED && m.type != KEEP_EXEC)) {
        errno = EIO;
        return -1;
    }
    if (m.type == KEEP_EXEC) {
        errno = (int)m.a;
        return -1;
    }
    p->r.process.pid = (long)m.a;
    p->r.process.start = (uint64_t)m.b;
    p->r.process.start_known = (int)m.c;
    rc = spec->reg == NULL ? 0
                           : spec->reg(spec->reg_ctx, p->attempt_id, &p->r.keeper, &p->r.process);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    if (keeper_send(p->ctl, KEEP_GO, 0, 0, 0) != 0 || keeper_recv(p->ev, &m) != 0 ||
        m.type != KEEP_EXEC) {
        errno = EIO;
        return -1;
    }
    if (m.a != 0) {
        errno = (int)m.a;
        return -1;
    }
    fcntl(p->ev, F_SETFL, O_NONBLOCK);
    fcntl(p->out, F_SETFL, O_NONBLOCK);
    fcntl(p->err, F_SETFL, O_NONBLOCK);
    return 0;
}

/** Free an attempt and close what it still has open. */
static void proc_release(struct etude_proc *p)
{
    close_fd(&p->ctl);
    close_fd(&p->ev);
    close_fd(&p->out);
    close_fd(&p->err);
    capture_free(&p->r.out);
    capture_free(&p->r.err);
    capture_files_close(&p->files);
    free(p->attempt_id);
    free(p);
}

/**
 * Give up an attempt that did not start: closing the controller's pipe makes
 * the keeper kill the case's process, and the keeper is reaped.
 */
static void proc_abandon(struct etude_proc *p)
{
    int e = errno;

    close_fd(&p->ctl);
    close_fd(&p->ev);
    if (p->keeper > 0)
        while (waitpid(p->keeper, NULL, 0) < 0 && errno == EINTR) {
        }
    proc_release(p);
    errno = e;
}

int etude_exec_start(struct etude_exec *ex, const struct etude_exec_spec *spec,
                     struct etude_proc **pp)
{
    struct etude_proc *p = proc_new(spec);

    if (p == NULL) {
        if (errno == 0)
            errno = ENOMEM;
        return -1;
    }
    if (proc_spawn(p, spec) != 0 || proc_handshake(p, spec) != 0) {
        proc_abandon(p);
        return -1;
    }
    p->ex = ex;
    p->next = ex->procs;
    ex->procs = p;
    *pp = p;
    return 0;
}

/**
 * Take a `child` record: a running process that started no earlier than the
 * case's process is registered and goes to the keeper to end; one that
 * started earlier cannot be its descendant. A descendant the registration
 * refused leaves the cleanup unconfirmed.
 */
static void child_found(void *ctx, long pid)
{
    struct etude_proc *p = ctx;
    struct etude_pid id;
    int state = etude_process_state(pid, &id);

    if (state == 0 || p->cleaned)
        return;
    if (state < 0) {
        p->unverified = 1;
        p->r.foreign++;
    } else if (p->r.process.start_known && id.start < p->r.process.start) {
        p->r.foreign++;
    } else {
        p->r.children++;
        if (p->child != NULL && p->child(p->reg_ctx, p->attempt_id, &id) != 0)
            p->unverified = 1;
        keeper_send(p->ctl, KEEP_CHILD, pid, (int64_t)id.start, 0);
    }
}

/**
 * Read what output stream `*fd` holds now, until it would wait or ends.
 *
 * @return the count of bytes read.
 */
static size_t read_output(struct etude_proc *p, int *fd, struct etude_output *o, int scan)
{
    size_t total = 0;
    char buf[4096];
    ssize_t n;

    while (*fd >= 0) {
        n = read(*fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return total;
        if (n == 0) {
            o->closed = 1;
            close_fd(fd);
            return total;
        }
        total += (size_t)n;
        if (p->r.output_error == 0)
            p->r.output_error =
                capture_files_add(scan ? p->files.out : p->files.err, buf, (size_t)n);
        capture_add(o, p->output_max, buf, (size_t)n);
        if (scan)
            capture_scan(&p->lines, buf, (size_t)n, child_found, p);
    }
    return total;
}

/** Act on one message of the keeper's. */
static void keeper_message(struct etude_proc *p, const struct keeper_msg *m)
{
    if (m->type == KEEP_EXIT && !p->exited) {
        p->exited = 1;
        p->r.ended_us = posix_now_us();
        p->r.exit_end = (enum etude_end)m->a;
        p->r.exit_value = (int)m->b;
        p->r.end = p->cancelled ? ETUDE_END_STOPPED : p->r.exit_end;
        p->r.value = p->cancelled ? -1 : p->r.exit_value;
        p->r.suspension = ETUDE_SUSPEND_NONE;
        read_output(p, &p->out, &p->r.out, 1);
        read_output(p, &p->err, &p->r.err, 0);
        keeper_send(p->ctl, KEEP_CLEAN, 0, 0, 0);
    } else if (m->type == KEEP_USAGE) {
        p->r.cpu_us.quality = m->b ? ETUDE_QUALITY_EXACT : ETUDE_QUALITY_LOWER_BOUND;
        p->r.cpu_us.value = (uint64_t)m->a;
        p->r.cpu_us.method = m->b ? METHOD_CPU_EXACT : METHOD_CPU_LOWER;
        p->r.peak_bytes.quality = ETUDE_QUALITY_LOWER_BOUND;
        p->r.peak_bytes.value = (uint64_t)m->c;
        p->r.peak_bytes.method = METHOD_PEAK;
    } else if (m->type == KEEP_CLEANED) {
        p->cleaned = 1;
        p->r.cleanup = m->a && !p->unverified ? ETUDE_CLEANUP_CONFIRMED : ETUDE_CLEANUP_UNCONFIRMED;
    } else if (m->type == KEEP_STOPPED && p->r.suspension == ETUDE_SUSPEND_ASKED) {
        p->r.suspension = ETUDE_SUSPEND_STOPPED;
        p->r.suspensions++;
        p->r.suspended_us = (uint64_t)m->a;
        p->noticed = 1;
    } else if (m->type == KEEP_RESUMED && p->r.suspension == ETUDE_SUSPEND_RESUMING) {
        p->r.suspension = ETUDE_SUSPEND_NONE;
        p->r.resumed_us = (uint64_t)m->a;
        p->noticed = 1;
    }
}

/**
 * Read the keeper's messages. At the end of its pipe the keeper is reaped;
 * an end of the case it did not report is lost, and a cleanup it did not
 * report is unconfirmed.
 */
static void read_keeper(struct etude_proc *p)
{
    ssize_t n;

    while (p->ev >= 0) {
        n = read(p->ev, (char *)&p->msg + p->msg_len, sizeof(p->msg) - p->msg_len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return;
        if (n == 0)
            break;
        p->msg_len += (size_t)n;
        if (p->msg_len == sizeof(p->msg)) {
            p->msg_len = 0;
            keeper_message(p, &p->msg);
        }
    }
    close_fd(&p->ev);
    close_fd(&p->ctl);
    while (waitpid(p->keeper, NULL, 0) < 0 && errno == EINTR) {
    }
    p->reaped = 1;
    if (!p->cleaned)
        p->r.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    p->drain_until = posix_now_us() + p->grace_us;
}

/**
 * Finish an attempt whose keeper has been reaped, once its output has ended.
 * Output still open after the grace period has a writer that outlived the
 * cleanup, so the cleanup is unconfirmed; once it is, nothing is waited for.
 */
static void proc_settle(struct etude_proc *p)
{
    int open_output;

    if (!p->reaped || p->finished)
        return;
    read_output(p, &p->out, &p->r.out, 1);
    read_output(p, &p->err, &p->r.err, 0);
    open_output = p->out >= 0 || p->err >= 0;
    if (open_output && p->r.cleanup == ETUDE_CLEANUP_CONFIRMED && posix_now_us() < p->drain_until)
        return;
    if (open_output)
        p->r.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    close_fd(&p->out);
    close_fd(&p->err);
    p->r.finished_us = posix_now_us();
    p->finished = 1;
}

/** Give a finished attempt not yet returned, marking it returned. */
static struct etude_proc *take_finished(struct etude_exec *ex)
{
    struct etude_proc *p;

    for (p = ex->procs; p != NULL; p = p->next) {
        if (p->finished && !p->returned) {
            p->returned = 1;
            return p;
        }
    }
    return NULL;
}

/**
 * Fill `fds` with the open descriptors of every attempt, and give their
 * count and in `*soonest` the earliest time an attempt's output wait ends.
 */
static size_t poll_set(struct etude_exec *ex, struct pollfd *fds, uint64_t *soonest)
{
    struct etude_proc *p;
    size_t n = 0;

    *soonest = 0;
    for (p = ex->procs; p != NULL; p = p->next) {
        if (p->finished)
            continue;
        if (p->reaped && (*soonest == 0 || p->drain_until < *soonest))
            *soonest = p->drain_until;
        fds[n].fd = p->ev;
        fds[n + 1].fd = p->out;
        fds[n + 2].fd = p->err;
        fds[n].events = fds[n + 1].events = fds[n + 2].events = POLLIN;
        fds[n].revents = fds[n + 1].revents = fds[n + 2].revents = 0;
        n += 3;
    }
    return n;
}

/** Give the milliseconds poll may wait for: until `until`, or forever at 0. */
static int poll_timeout(uint64_t until)
{
    uint64_t now = posix_now_us();

    if (until == 0)
        return -1;
    if (until <= now)
        return 0;
    if (until - now > 60000000u)
        return 60000;
    return (int)((until - now + 999) / 1000);
}

/**
 * Serve every unfinished attempt once, after a poll of their descriptors.
 *
 * @return the count of output bytes read, and of suspensions that changed.
 */
static size_t serve(struct etude_exec *ex)
{
    struct etude_proc *p;
    size_t n = 0;

    for (p = ex->procs; p != NULL; p = p->next) {
        if (p->finished)
            continue;
        n += read_output(p, &p->out, &p->r.out, 1);
        n += read_output(p, &p->err, &p->r.err, 0);
        if (!p->reaped)
            read_keeper(p);
        n += (size_t)p->noticed;
        p->noticed = 0;
        proc_settle(p);
    }
    return n;
}

/** Count the attempts the executor runs that have not finished. */
static size_t unfinished(const struct etude_exec *ex)
{
    const struct etude_proc *p;
    size_t n = 0;

    for (p = ex->procs; p != NULL; p = p->next)
        n += !p->finished;
    return n;
}

/**
 * Copy into `w` what poll found of the caller's descriptors, at `fds`.
 *
 * @return the count of them that are ready.
 */
static size_t watch_found(struct etude_watch *w, const struct pollfd *fds, size_t n)
{
    size_t i, ready = 0;

    for (i = 0; i < n; i++) {
        w[i].revents = ((fds[i].revents & POLLIN) ? ETUDE_WATCH_IN : 0) |
                       ((fds[i].revents & POLLOUT) ? ETUDE_WATCH_OUT : 0) |
                       ((fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) ? ETUDE_WATCH_END : 0);
        ready += w[i].revents != 0;
    }
    return ready;
}

struct etude_proc *etude_exec_wait(struct etude_exec *ex, int timeout_ms)
{
    return etude_exec_watch(ex, timeout_ms, NULL, 0);
}

struct etude_proc *etude_exec_watch(struct etude_exec *ex, int timeout_ms, struct etude_watch *w,
                                    size_t nw)
{
    uint64_t until = timeout_ms < 0 ? 0 : posix_now_us() + (uint64_t)timeout_ms * 1000u;
    struct etude_proc *done;
    struct pollfd *fds;
    uint64_t soonest;
    size_t i, n, output = 0, ready = 0;
    int polled = 0;

    for (i = 0; i < nw; i++)
        w[i].revents = 0;
    for (;;) {
        done = take_finished(ex);
        if (done != NULL || output || ready || (unfinished(ex) == 0 && nw == 0))
            return done;
        if (polled && timeout_ms >= 0 && posix_now_us() >= until)
            return NULL;
        fds = malloc((3 * unfinished(ex) + nw + 1) * sizeof(*fds));
        if (fds == NULL)
            return NULL;
        n = poll_set(ex, fds, &soonest);
        for (i = 0; i < nw; i++) {
            fds[n + i].fd = w[i].fd;
            fds[n + i].events = (short)(((w[i].events & ETUDE_WATCH_IN) ? POLLIN : 0) |
                                        ((w[i].events & ETUDE_WATCH_OUT) ? POLLOUT : 0));
            fds[n + i].revents = 0;
        }
        if (timeout_ms >= 0 && (soonest == 0 || until < soonest))
            soonest = until;
        if (poll(fds, (nfds_t)(n + nw), poll_timeout(soonest)) > 0)
            ready = watch_found(w, fds + n, nw);
        free(fds);
        polled = 1;
        output = serve(ex);
    }
}

void etude_exec_cancel(struct etude_proc *p)
{
    if (p->finished || p->cancelled)
        return;
    p->cancelled = 1;
    if (!p->exited)
        keeper_send(p->ctl, KEEP_CANCEL, 0, 0, 0);
}

int etude_exec_suspend(struct etude_proc *p)
{
    if (p->exited || p->cancelled || p->finished) {
        errno = ESRCH;
        return -1;
    }
    if (p->r.suspension != ETUDE_SUSPEND_NONE) {
        errno = EBUSY;
        return -1;
    }
    if (keeper_send(p->ctl, KEEP_STOP, 0, 0, 0) != 0) {
        errno = EPIPE;
        return -1;
    }
    p->r.suspension = ETUDE_SUSPEND_ASKED;
    return 0;
}

int etude_exec_resume(struct etude_proc *p)
{
    if (p->r.suspension != ETUDE_SUSPEND_ASKED && p->r.suspension != ETUDE_SUSPEND_STOPPED)
        return 0;
    if (p->exited || p->finished) {
        p->r.suspension = ETUDE_SUSPEND_NONE;
        return 0;
    }
    p->r.suspension = ETUDE_SUSPEND_RESUMING;
    if (keeper_send(p->ctl, KEEP_CONT, 0, 0, 0) != 0) {
        errno = EPIPE;
        return -1;
    }
    return 0;
}

const struct etude_exec_result *etude_proc_result(const struct etude_proc *p)
{
    return &p->r;
}

int etude_proc_sample(const struct etude_proc *p, uint64_t *cpu_us, uint64_t *rss_bytes)
{
    if (p->exited)
        return 0;
    return posix_usage(&p->r.process, cpu_us, rss_bytes);
}

void etude_proc_free(struct etude_proc *p)
{
    struct etude_proc **pp;

    for (pp = &p->ex->procs; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    }
    proc_release(p);
}

void etude_exec_free(struct etude_exec *ex)
{
    struct etude_proc *p;

    for (p = ex->procs; p != NULL; p = p->next)
        etude_exec_cancel(p);
    while (unfinished(ex) > 0)
        etude_exec_wait(ex, -1);
    while (ex->procs != NULL)
        etude_proc_free(ex->procs);
    free(ex);
}
