/*
 * supervise.c: the supervisor of etude's attempts on POSIX systems and on
 * Windows. It runs host attempts on an executor of its own and machines on a
 * launcher of its own, each under its watchdog, and follows remote attempts
 * whose transport the caller keeps. When a watchdog runs out it saves the
 * evidence, asks the case to stop and ends what it owns, each step within a
 * bound.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "win/winexec.h"
#else
#include <poll.h>

#include "posix/posix.h"
#endif

#include "etude/watchdog.h"

/* The console lines kept as evidence when a machine's watchdog runs out. */
#define TAIL_LINES 20
#define TAIL_BYTES 4096

/* The longest time one executor is served while the other also has work. */
#define SLICE_MS 50

/* The QMP commands that read a machine's state, in the order of the
 * result's members. */
static const char *const capture_commands[3][2] = {
    {"query-status", NULL},
    {"query-cpus-fast", NULL},
    {"human-monitor-command", "{\"command-line\":\"info registers -a\"}"},
};

/* Where an attempt is in its supervision. */
enum phase {
    PHASE_RUN,     /* its watchdog has not run out */
    PHASE_CAPTURE, /* reading QEMU's state through QMP */
    PHASE_ASK,     /* the case was asked to stop, and has its grace */
    PHASE_END,     /* ending what the supervisor owns */
    PHASE_DONE
};

struct etude_supervised {
    struct etude_supervisor *s;
    struct etude_supervised *next;
    char *attempt_id;
    struct etude_watchdog *wd;
    const struct etude_watchdog_spec *spec;
    struct etude_proc *proc;
    struct etude_machine *m;
    int proc_done;    /* the executor has returned the host attempt */
    int machine_done; /* the launcher has returned the machine */
    int console_progress;
    size_t fed;           /* bytes of the output or console fed to the watchdog */
    uint64_t seen;        /* output and console bytes seen, and the state */
    struct etude_ask ask; /* fn is NULL for none */
    uint64_t grace_us;
    enum phase phase;
    uint64_t until; /* when the capture or the grace of the ask runs out */
    long qmp_id[3];
    int reported;         /* the guest or the transport reported the case's end */
    uint64_t reported_us; /* monotonic time of the report */
    enum etude_end reported_end;
    int reported_value;
    enum etude_cleanup reported_cleanup;
    int lost;
    int changed;
    int returned;
    struct etude_supervised_result r;
};

struct etude_supervisor {
    struct etude_exec *ex;
    struct etude_qemu *q;
    struct etude_supervised *list;
};

/** Give the monotonic clock in microseconds. */
static uint64_t now_us(void)
{
#ifdef _WIN32
    return etude_win_mono_us();
#else
    return posix_now_us();
#endif
}

/** Wait `ms` milliseconds with nothing to serve. */
static void pause_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    poll(NULL, 0, ms);
#endif
}

struct etude_supervisor *etude_supervisor_new(void)
{
    struct etude_supervisor *s = calloc(1, sizeof(*s));

    if (s == NULL)
        return NULL;
    s->ex = etude_exec_new();
    s->q = etude_qemu_new();
    if (s->ex == NULL || s->q == NULL) {
        if (s->ex != NULL)
            etude_exec_free(s->ex);
        if (s->q != NULL)
            etude_qemu_free(s->q);
        free(s);
        return NULL;
    }
    return s;
}

/** Make an attempt of `kind` under watchdog `wd`, starting in `stage`. */
static struct etude_supervised *attempt_new(struct etude_supervisor *s, const char *id,
                                            enum etude_supervised_kind kind,
                                            const struct etude_watchdog_spec *wd,
                                            enum etude_stage stage)
{
    struct etude_supervised *a = calloc(1, sizeof(*a));
    uint64_t now = now_us();

    if (a == NULL)
        return NULL;
    a->attempt_id = malloc(strlen(id) + 1);
    a->wd = etude_watchdog_new(wd, stage, now);
    if (a->attempt_id == NULL || a->wd == NULL) {
        free(a->attempt_id);
        etude_watchdog_free(a->wd);
        free(a);
        return NULL;
    }
    strcpy(a->attempt_id, id);
    a->s = s;
    a->spec = wd;
    a->r.attempt_id = a->attempt_id;
    a->r.kind = kind;
    a->r.end = ETUDE_END_LOST;
    a->r.value = -1;
    a->r.progress = etude_watchdog_progress(a->wd);
    a->r.sample_cpu_us.method = a->r.sample_rss_bytes.method = "no watchdog ran out";
    a->r.cleanup = ETUDE_CLEANUP_PENDING;
    a->r.claim_kept = 1;
    return a;
}

/** Free an attempt's own memory, after its process or machine. */
static void attempt_release(struct etude_supervised *a)
{
    etude_watchdog_free(a->wd);
    free(a->r.console_tail);
    free(a->r.qmp_status);
    free(a->r.qmp_cpus);
    free(a->r.qmp_registers);
    free(a->attempt_id);
    free(a);
}

/** Put attempt `a` on the supervisor's list and give it to the caller. */
static int attempt_add(struct etude_supervisor *s, struct etude_supervised *a,
                       struct etude_supervised **out)
{
    a->next = s->list;
    s->list = a;
    *out = a;
    return 0;
}

int etude_supervise_host(struct etude_supervisor *s, const struct etude_exec_spec *spec,
                         const struct etude_watchdog_spec *wd, struct etude_supervised **out)
{
    struct etude_supervised *a =
        attempt_new(s, spec->attempt_id, ETUDE_SUPERVISED_HOST, wd, ETUDE_STAGE_EXECUTE);
    int e;

    if (a == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (etude_exec_start(s->ex, spec, &a->proc) != 0) {
        e = errno;
        attempt_release(a);
        errno = e;
        return -1;
    }
    a->grace_us = spec->grace_us;
    a->r.proc = etude_proc_result(a->proc);
    return attempt_add(s, a, out);
}

int etude_supervise_machine(struct etude_supervisor *s, const struct etude_machine_spec *spec,
                            const struct etude_watchdog_spec *wd, int console_progress,
                            const struct etude_ask *ask, struct etude_supervised **out)
{
    struct etude_supervised *a =
        attempt_new(s, spec->attempt_id, ETUDE_SUPERVISED_MACHINE, wd, ETUDE_STAGE_BOOT);
    int e;

    if (a == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (etude_qemu_start(s->q, spec, &a->m) != 0) {
        e = errno;
        attempt_release(a);
        errno = e;
        return -1;
    }
    a->grace_us = spec->grace_us;
    a->console_progress = console_progress;
    if (ask != NULL)
        a->ask = *ask;
    a->r.machine = etude_machine_result(a->m);
    return attempt_add(s, a, out);
}

int etude_supervise_remote(struct etude_supervisor *s, const char *attempt_id,
                           const struct etude_watchdog_spec *wd, uint64_t grace_us,
                           const struct etude_ask *ask, struct etude_supervised **out)
{
    struct etude_supervised *a =
        attempt_new(s, attempt_id, ETUDE_SUPERVISED_REMOTE, wd, ETUDE_STAGE_EXECUTE);

    if (a == NULL) {
        errno = ENOMEM;
        return -1;
    }
    a->grace_us = grace_us;
    if (ask != NULL)
        a->ask = *ask;
    return attempt_add(s, a, out);
}

/** Copy `n` bytes at `text` into a new string; NULL when memory ran out. */
static char *copy_of(const char *text, size_t n)
{
    char *c = malloc(n + 1);

    if (c != NULL) {
        memcpy(c, text, n);
        c[n] = '\0';
    }
    return c;
}

/** Add `text` to the attempt's account of what could not be saved. */
static void capture_failed(struct etude_supervised *a, const char *text)
{
    size_t used = strlen(a->r.capture_error);

    snprintf(a->r.capture_error + used, sizeof(a->r.capture_error) - used, "%s%s",
             used > 0 ? "; " : "", text);
}

/** Keep the last lines of what the console kept. */
static void keep_console_tail(struct etude_supervised *a)
{
    const struct etude_output *c = &a->r.machine->console;
    size_t from = c->kept > TAIL_BYTES ? c->kept - TAIL_BYTES : 0, at = c->kept;
    int lines = 0;

    while (at > from) {
        if (c->data[at - 1] == '\n' && at < c->kept && ++lines == TAIL_LINES)
            break;
        at--;
    }
    a->r.console_tail = copy_of(c->data + at, c->kept - at);
    if (c->total > c->kept)
        capture_failed(a, "the console passed its bound, so its last lines are of what was kept");
}

/**
 * Sample the case's process, or QEMU's, as the watchdog runs out. On POSIX
 * systems the sample is of the process and the children it has reaped, read
 * from /proc on Linux. On Windows it is of a host attempt's job, whose
 * processor time counts every process the job has held, and of the working
 * set of the case's process.
 */
static void sample(struct etude_supervised *a, const struct etude_exec_result *p)
{
    uint64_t cpu, rss;
    int rc;
#ifdef _WIN32
    const enum etude_quality cpu_quality = ETUDE_QUALITY_EXACT;
    const char *cpu_method = "win32-job-accounting-v1", *rss_method = "win32-working-set-v1";

    rc = a->proc != NULL ? etude_proc_sample(a->proc, &cpu, &rss) : -1;
#else
    const enum etude_quality cpu_quality = ETUDE_QUALITY_LOWER_BOUND;
    const char *cpu_method = "linux-proc-stat-v1", *rss_method = "linux-proc-stat-v1";

    rc = posix_usage(&p->process, &cpu, &rss);
#endif
    a->r.owned[0] = p->keeper;
    a->r.owned[1] = p->process;
    a->r.owned_count = 2;
    if (rc == 1) {
        a->r.sample_cpu_us.quality = cpu_quality;
        a->r.sample_rss_bytes.quality = ETUDE_QUALITY_LOWER_BOUND;
        a->r.sample_cpu_us.value = cpu;
        a->r.sample_rss_bytes.value = rss;
        a->r.sample_cpu_us.method = cpu_method;
        a->r.sample_rss_bytes.method = rss_method;
    } else {
        a->r.sample_cpu_us.quality = a->r.sample_rss_bytes.quality = ETUDE_QUALITY_UNAVAILABLE;
        a->r.sample_cpu_us.method = a->r.sample_rss_bytes.method =
            rc == 0 ? "the process had ended" : "the system gives no sample of a process";
    }
}

/** Give the capture's bound: the capture stage's, or the grace when the
 * case declares none. */
static uint64_t capture_bound(const struct etude_supervised *a)
{
    uint64_t us = a->spec->stage_us[ETUDE_STAGE_CAPTURE];

    return us != 0 ? us : a->grace_us;
}

/** End what the supervisor owns of the attempt. */
static void end_owned(struct etude_supervised *a)
{
    if (a->r.kind == ETUDE_SUPERVISED_MACHINE)
        etude_machine_cancel(a->m);
    a->phase = PHASE_END;
}

/** Ask the case to stop through its transport, when the caller gave one. */
static void ask_case(struct etude_supervised *a, uint64_t now)
{
    if (a->ask.fn == NULL || a->reported || a->lost) {
        end_owned(a);
        return;
    }
    a->r.asked = 1;
    a->r.ask_error = a->ask.fn(a->ask.ctx, a->attempt_id);
    if (a->r.ask_error != 0) {
        end_owned(a);
        return;
    }
    a->phase = PHASE_ASK;
    a->until = now + a->grace_us;
}

/** Send the commands that read the machine's state. */
static void capture_start(struct etude_supervised *a, uint64_t now)
{
    size_t i;

    if (a->r.machine->state != ETUDE_MACHINE_RUNNING) {
        capture_failed(a, "QMP had not started the machine, so its state was not read");
        ask_case(a, now);
        return;
    }
    for (i = 0; i < 3; i++)
        a->qmp_id[i] = etude_machine_qmp(a->m, capture_commands[i][0], capture_commands[i][1]);
    a->phase = PHASE_CAPTURE;
    a->until = now + capture_bound(a);
}

/**
 * Take the replies that have come; at the bound, or when QMP is gone, give
 * up on the rest and go on to the ask.
 */
static void capture_serve(struct etude_supervised *a, uint64_t now)
{
    char **slot[3];
    char text[96];
    const char *reply;
    int i, missing = 0, rc[3] = {1, 1, 1};

    slot[0] = &a->r.qmp_status;
    slot[1] = &a->r.qmp_cpus;
    slot[2] = &a->r.qmp_registers;
    for (i = 0; i < 3; i++) {
        if (*slot[i] != NULL)
            continue;
        rc[i] = a->qmp_id[i] < 0 ? -1 : etude_machine_reply(a->m, a->qmp_id[i], &reply);
        if (rc[i] == 1)
            *slot[i] = copy_of(reply, strlen(reply));
        else if (rc[i] == 0)
            missing++;
    }
    if (missing > 0 && now < a->until)
        return;
    for (i = 0; i < 3; i++) {
        if (*slot[i] != NULL)
            continue;
        snprintf(text, sizeof(text), "%s: %s", capture_commands[i][0],
                 rc[i] == 0 ? "QMP did not answer within the capture bound"
                            : "QMP could not take or answer it");
        capture_failed(a, text);
    }
    ask_case(a, now);
}

/** Keep in the result the expiry of the watchdog, seen at `now`. */
static void keep_expiry(struct etude_supervised *a, uint64_t now)
{
    a->r.expiry = etude_watchdog_check(a->wd, now);
    a->r.expired = etude_watchdog_expired(a->wd);
    a->r.bound_us = etude_watchdog_bound(a->wd);
    a->r.expired_us = now;
    a->r.incomplete = 1;
    a->changed = 1;
}

/** Start the end of an attempt whose watchdog has run out. */
static void expire(struct etude_supervised *a, uint64_t now)
{
    keep_expiry(a, now);
    if (a->r.kind == ETUDE_SUPERVISED_HOST) {
        sample(a, a->r.proc);
        a->r.asked = 1;
        etude_exec_cancel(a->proc);
        a->phase = PHASE_END;
    } else if (a->r.kind == ETUDE_SUPERVISED_MACHINE) {
        sample(a, a->r.machine->qemu);
        keep_console_tail(a);
        capture_start(a, now);
    } else {
        a->r.sample_cpu_us.method = a->r.sample_rss_bytes.method =
            "the case runs on another machine";
        ask_case(a, now);
    }
}

/** Finish an attempt: its end, its cleanup and what follows from them. */
static void finish(struct etude_supervised *a, uint64_t now)
{
    etude_watchdog_stage(a->wd, ETUDE_STAGE_CLEANUP, now);
    if (a->r.kind == ETUDE_SUPERVISED_HOST) {
        a->r.end = a->r.proc->end;
        a->r.value = a->r.proc->value;
        a->r.cleanup = a->r.proc->cleanup;
    } else if (a->r.kind == ETUDE_SUPERVISED_MACHINE) {
        a->r.end = a->r.machine->end;
        a->r.cleanup = a->r.machine->qemu->cleanup;
    } else if (a->reported) {
        a->r.end = a->reported_end;
        a->r.value = a->reported_value;
        a->r.cleanup = a->reported_cleanup;
    } else {
        a->r.end = ETUDE_END_LOST;
        a->r.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    }
    if (a->r.end == ETUDE_END_LOST || a->lost)
        a->r.incomplete = 1;
    if (a->r.cleanup != ETUDE_CLEANUP_CONFIRMED)
        a->r.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    a->r.claim_kept = a->r.cleanup != ETUDE_CLEANUP_CONFIRMED;
    a->r.max_gap_us = etude_watchdog_max_gap(a->wd, now);
    a->r.finished_us = now;
    a->r.finished = 1;
    a->phase = PHASE_DONE;
    a->changed = 1;
}

/** Feed the watchdog what the case's output, or the console, has added. */
static void feed_new(struct etude_supervised *a, uint64_t now)
{
    const struct etude_output *o = NULL;
    uint64_t seen;

    if (a->r.kind == ETUDE_SUPERVISED_HOST) {
        o = &a->r.proc->out;
        seen = a->r.proc->out.total + a->r.proc->err.total;
    } else if (a->r.kind == ETUDE_SUPERVISED_MACHINE) {
        o = a->console_progress ? &a->r.machine->console : NULL;
        seen = a->r.machine->console.total * 4 + (uint64_t)a->r.machine->state;
    } else {
        return;
    }
    if (seen != a->seen)
        a->changed = 1;
    a->seen = seen;
    if (o != NULL && o->kept > a->fed) {
        etude_watchdog_feed(a->wd, o->data + a->fed, o->kept - a->fed, now);
        a->fed = o->kept;
    }
}

/** Say whether what the supervisor owns of the attempt has ended. */
static int owned_ended(const struct etude_supervised *a)
{
    if (a->r.kind == ETUDE_SUPERVISED_HOST)
        return a->proc_done;
    if (a->r.kind == ETUDE_SUPERVISED_MACHINE)
        return a->machine_done;
    return a->reported || a->lost || a->phase == PHASE_END;
}

/**
 * Give when the case of attempt `a` ended, as its keeper or launcher saw it
 * or its transport reported it, or 0 while it runs.
 */
static uint64_t case_ended(const struct etude_supervised *a, uint64_t now)
{
    if (a->r.kind == ETUDE_SUPERVISED_HOST && a->r.proc->ended_us != 0)
        return a->r.proc->ended_us;
    if (a->r.kind == ETUDE_SUPERVISED_MACHINE && a->r.machine->state == ETUDE_MACHINE_ENDED)
        return a->r.machine->ended_us != 0 ? a->r.machine->ended_us : now;
    if (a->r.kind == ETUDE_SUPERVISED_REMOTE && a->reported)
        return a->reported_us;
    return 0;
}

/**
 * Serve one attempt at `now`: read, check its watchdog, move its end on. The
 * case's end is judged at the time its keeper or launcher saw it, which may
 * be before `now` when the caller was busy elsewhere.
 */
static void serve(struct etude_supervised *a, uint64_t now)
{
    uint64_t ended;

    if (a->phase == PHASE_DONE)
        return;
    feed_new(a, now);
    if ((ended = case_ended(a, now)) != 0)
        etude_watchdog_end(a->wd, ended, now);
    if (owned_ended(a)) {
        if (a->phase == PHASE_RUN && etude_watchdog_expired(a->wd) != NULL)
            keep_expiry(a, now);
        finish(a, now);
        return;
    }
    if (a->phase == PHASE_RUN && etude_watchdog_check(a->wd, now) != ETUDE_EXPIRY_NONE)
        expire(a, now);
    if (a->phase == PHASE_CAPTURE)
        capture_serve(a, now);
    if (a->phase == PHASE_ASK && (now >= a->until || a->reported))
        end_owned(a);
    if (owned_ended(a))
        finish(a, now);
}

/** Give a finished attempt not yet returned, marking it returned. */
static struct etude_supervised *take_finished(struct etude_supervisor *s)
{
    struct etude_supervised *a;

    for (a = s->list; a != NULL; a = a->next) {
        if (a->r.finished && !a->returned) {
            a->returned = 1;
            return a;
        }
    }
    return NULL;
}

/**
 * Give the soonest time an attempt needs serving: a watchdog's deadline or
 * the end of a capture or an ask; 0 for none. Count the host attempts and
 * the machines that have not ended in `*procs` and `*machines`, and say in
 * the result's place whether any attempt is unfinished.
 */
static uint64_t soonest(struct etude_supervisor *s, size_t *procs, size_t *machines, int *any)
{
    struct etude_supervised *a;
    uint64_t t, next = 0;

    *procs = *machines = 0;
    *any = 0;
    for (a = s->list; a != NULL; a = a->next) {
        if (a->phase == PHASE_DONE)
            continue;
        *any = 1;
        *procs += a->proc != NULL && !a->proc_done;
        *machines += a->m != NULL && !a->machine_done;
        if (a->phase == PHASE_RUN)
            t = etude_watchdog_deadline(a->wd);
        else if (a->phase == PHASE_CAPTURE || a->phase == PHASE_ASK)
            t = a->until;
        else
            continue;
        if (next == 0 || t < next)
            next = t;
    }
    return next;
}

/** Mark the host attempt of process `p`, which the executor returned. */
static void proc_returned(struct etude_supervisor *s, struct etude_proc *p)
{
    struct etude_supervised *a;

    for (a = s->list; p != NULL && a != NULL; a = a->next)
        if (a->proc == p)
            a->proc_done = 1;
}

/** Mark the attempt of machine `m`, which the launcher returned. */
static void machine_returned(struct etude_supervisor *s, struct etude_machine *m)
{
    struct etude_supervised *a;

    for (a = s->list; m != NULL && a != NULL; a = a->next)
        if (a->m == m)
            a->machine_done = 1;
}

/** Give the milliseconds from `now` until `until`, -1 for 0. */
static int ms_until(uint64_t until, uint64_t now)
{
    if (until == 0)
        return -1;
    if (until <= now)
        return 0;
    if (until - now > 60000000u)
        return 60000;
    return (int)((until - now + 999) / 1000);
}

/**
 * Take what the executor and the launcher have ready, without waiting: the
 * output, the ends and the cleanups that came while the caller was not
 * waiting, so that no watchdog is checked before it has seen them.
 */
static void take_ready(struct etude_supervisor *s)
{
    struct etude_proc *p;
    struct etude_machine *m;

    while ((p = etude_exec_wait(s->ex, 0)) != NULL)
        proc_returned(s, p);
    while ((m = etude_qemu_wait(s->q, 0)) != NULL)
        machine_returned(s, m);
}

/**
 * Wait up to `ms` milliseconds, -1 for no limit, for the executor and the
 * launcher; when both have work, each in turn for a short slice.
 */
static void wait_both(struct etude_supervisor *s, size_t procs, size_t machines, int ms)
{
    int slice = ms < 0 || ms > SLICE_MS ? SLICE_MS : ms;

    if (procs > 0 && machines > 0) {
        proc_returned(s, etude_exec_wait(s->ex, slice));
        machine_returned(s, etude_qemu_wait(s->q, 0));
    } else if (procs > 0) {
        proc_returned(s, etude_exec_wait(s->ex, ms));
    } else if (machines > 0) {
        machine_returned(s, etude_qemu_wait(s->q, ms));
    } else if (ms != 0) {
        pause_ms(ms < 0 ? SLICE_MS : ms);
    }
}

struct etude_supervised *etude_supervise_wait(struct etude_supervisor *s, int timeout_ms)
{
    uint64_t now = now_us(), next, until = timeout_ms < 0 ? 0 : now + (uint64_t)timeout_ms * 1000u;
    struct etude_supervised *a, *done;
    size_t procs, machines;
    int polled = 0, changed, any;

    for (;;) {
        if (!polled)
            take_ready(s);
        now = now_us();
        changed = 0;
        for (a = s->list; a != NULL; a = a->next) {
            serve(a, now);
            changed |= a->changed;
            a->changed = 0;
        }
        done = take_finished(s);
        if (done != NULL || changed)
            return done;
        next = soonest(s, &procs, &machines, &any);
        if (!any || (polled && timeout_ms >= 0 && now >= until))
            return NULL;
        if (timeout_ms >= 0 && (next == 0 || until < next))
            next = until;
        wait_both(s, procs, machines, ms_until(next, now));
        polled = 1;
    }
}

void etude_supervised_feed(struct etude_supervised *a, const char *buf, size_t n)
{
    etude_watchdog_feed(a->wd, buf, n, now_us());
}

void etude_supervised_stage(struct etude_supervised *a, enum etude_stage stage)
{
    if (a->phase == PHASE_RUN)
        etude_watchdog_stage(a->wd, stage, now_us());
}

void etude_supervised_end(struct etude_supervised *a, enum etude_end end, int value,
                          enum etude_cleanup cleanup)
{
    if (a->reported || a->r.finished)
        return;
    a->reported = 1;
    a->reported_us = now_us();
    a->reported_end = end;
    a->reported_value = value;
    a->reported_cleanup = cleanup;
}

void etude_supervised_lost(struct etude_supervised *a)
{
    if (a->r.kind == ETUDE_SUPERVISED_REMOTE)
        a->lost = 1;
}

void etude_supervised_cancel(struct etude_supervised *a)
{
    if (a->r.kind != ETUDE_SUPERVISED_HOST || a->phase != PHASE_RUN || a->proc_done)
        return;
    etude_exec_cancel(a->proc);
    a->phase = PHASE_END;
}

const struct etude_supervised_result *etude_supervised_result(const struct etude_supervised *a)
{
    return &a->r;
}

const struct etude_watchdog *etude_supervised_watchdog(const struct etude_supervised *a)
{
    return a->wd;
}

struct etude_machine *etude_supervised_machine(struct etude_supervised *a)
{
    return a->m;
}

void etude_supervised_free(struct etude_supervised *a)
{
    struct etude_supervised **ap;

    for (ap = &a->s->list; *ap != NULL; ap = &(*ap)->next) {
        if (*ap == a) {
            *ap = a->next;
            break;
        }
    }
    if (a->proc != NULL)
        etude_proc_free(a->proc);
    if (a->m != NULL)
        etude_machine_free(a->m);
    attempt_release(a);
}

void etude_supervisor_free(struct etude_supervisor *s)
{
    struct etude_supervised *a;
    size_t procs, machines;
    int any;

    for (a = s->list; a != NULL; a = a->next) {
        if (a->r.finished)
            continue;
        if (a->proc != NULL)
            etude_exec_cancel(a->proc);
        if (a->m != NULL)
            etude_machine_cancel(a->m);
        a->lost |= a->r.kind == ETUDE_SUPERVISED_REMOTE;
    }
    soonest(s, &procs, &machines, &any);
    while (any) {
        etude_supervise_wait(s, -1);
        soonest(s, &procs, &machines, &any);
    }
    while (s->list != NULL)
        etude_supervised_free(s->list);
    etude_exec_free(s->ex);
    etude_qemu_free(s->q);
    free(s);
}
