/*
 * exec.c: the executor of host processes on Windows, as etude/exec.h
 * declares it, on the Job Objects of winexec.c. The job takes the part of the
 * POSIX keeper: it holds the case before it runs, holds every descendant,
 * and ends them all with the controller.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../capture.h"
#include "winexec.h"

/* The method identity of the processor time, as the history records it. */
#define METHOD_CPU "win32-job-accounting-v1"

/* Why the peak memory has no value on Windows. */
#define NO_PEAK "a job reports its peak commit, not its resident memory"

/* How long etude_exec_start waits for a refused case to end, in ms. */
#define REFUSED_BOUND 60000

struct etude_proc {
    struct etude_exec *ex;
    struct etude_proc *next;
    char *attempt_id;
    size_t output_max;
    struct etude_win_child *child;
    struct capture_lines lines;
    etude_register_fn reg;
    void *reg_ctx;
    int reg_error; /* the error the registration returned, or 0 */
    int cancelled;
    int finished;
    int returned;
    struct etude_exec_result r;
};

struct etude_exec {
    struct etude_win_sup sup;
    struct etude_proc *procs;
    int output; /* output arrived since etude_exec_wait last returned */
};

/** Give the errno value nearest the Win32 error ERR. */
static int win_errno(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_BAD_EXE_FORMAT:
        return ENOEXEC;
    case ERROR_FILENAME_EXCED_RANGE:
        return E2BIG;
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    default:
        return EIO;
    }
}

/** Fill `id` with process H's identity: its ID and its creation time. */
static int identify(HANDLE h, struct etude_pid *id)
{
    FILETIME ft[4];

    if (!GetProcessTimes(h, &ft[0], &ft[1], &ft[2], &ft[3]))
        return -1;
    id->pid = (long)GetProcessId(h);
    id->start = (uint64_t)ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime;
    id->start_known = 1;
    return 0;
}

int etude_process_state(long pid, struct etude_pid *id)
{
    HANDLE h;
    int state;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (h == NULL)
        return GetLastError() == ERROR_INVALID_PARAMETER ? 0 : -1;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
        state = 0;
    else
        state = identify(h, id) == 0 ? 1 : -1;
    CloseHandle(h);
    return state;
}

struct etude_exec *etude_exec_new(void)
{
    struct etude_exec *ex = calloc(1, sizeof(*ex));

    if (ex == NULL)
        return NULL;
    if (etude_win_sup_open(&ex->sup) != 0) {
        free(ex);
        return NULL;
    }
    return ex;
}

/** Make an attempt with no process. */
static struct etude_proc *proc_new(const struct etude_exec_spec *spec)
{
    struct etude_proc *p = calloc(1, sizeof(*p));

    if (p == NULL)
        return NULL;
    p->output_max = spec->output_max;
    p->reg = spec->reg;
    p->reg_ctx = spec->reg_ctx;
    p->attempt_id = malloc(strlen(spec->attempt_id) + 1);
    if (p->attempt_id == NULL || capture_init(&p->r.out, spec->output_max) != 0 ||
        capture_init(&p->r.err, spec->output_max) != 0) {
        free(p->attempt_id);
        capture_free(&p->r.out);
        free(p);
        return NULL;
    }
    strcpy(p->attempt_id, spec->attempt_id);
    p->r.attempt_id = p->attempt_id;
    p->r.end = p->r.exit_end = ETUDE_END_LOST;
    p->r.value = p->r.exit_value = -1;
    p->r.cpu_us.method = "the job ended before it was measured";
    p->r.peak_bytes.method = NO_PEAK;
    identify(GetCurrentProcess(), &p->r.keeper);
    p->r.started_us = etude_win_mono_us();
    return p;
}

/** Free an attempt's memory. */
static void proc_release(struct etude_proc *p)
{
    capture_free(&p->r.out);
    capture_free(&p->r.err);
    free(p->attempt_id);
    free(p);
}

/**
 * Take a `child` record. Every descendant of the case is in its job and ends
 * with it; the record is counted as a child when it names a process of the
 * job, and as foreign otherwise.
 */
static void child_found(void *ctx, long pid)
{
    struct etude_proc *p = ctx;
    HANDLE h;
    BOOL in = FALSE;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (h == NULL)
        return;
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
        if (IsProcessInJob(h, p->child->job, &in) && in)
            p->r.children++;
        else
            p->r.foreign++;
    }
    CloseHandle(h);
}

/** The adapter's output function: keep the stream and scan the output. */
static void on_output(void *ctx, struct etude_win_child *c, enum etude_win_stream s,
                      const char *data, size_t n)
{
    struct etude_proc *p = ctx;

    (void)c;
    p->ex->output = 1;
    if (s == ETUDE_WIN_STDERR) {
        capture_add(&p->r.err, p->output_max, data, n);
        return;
    }
    capture_add(&p->r.out, p->output_max, data, n);
    capture_scan(&p->lines, data, n, child_found, p);
}

/** The adapter's registration function: ask the controller's. */
static int on_register(void *ctx, struct etude_win_child *c, const struct etude_win_ident *id)
{
    struct etude_proc *p = ctx;

    (void)id;
    p->child = c;
    if (identify(c->process, &p->r.process) != 0) {
        p->reg_error = EIO;
        return 1;
    }
    if (p->reg != NULL)
        p->reg_error = p->reg(p->reg_ctx, p->attempt_id, &p->r.keeper, &p->r.process);
    return p->reg_error != 0;
}

/**
 * Wait for the refused or failed child of P to end, and release it; a child
 * that does not end in time stays with the supervisor, with no attempt to
 * report to.
 */
static void proc_abandon(struct etude_proc *p)
{
    p->child->output = NULL;
    p->child->ctx = NULL;
    etude_win_release(p->child, REFUSED_BOUND);
    proc_release(p);
}

int etude_exec_start(struct etude_exec *ex, const struct etude_exec_spec *spec,
                     struct etude_proc **pp)
{
    struct etude_win_spawn sp;
    struct etude_proc *p = proc_new(spec);
    DWORD err;

    if (p == NULL) {
        errno = ENOMEM;
        return -1;
    }
    p->ex = ex;
    memset(&sp, 0, sizeof(sp));
    sp.program = spec->argv[0];
    sp.argv = (const char *const *)(spec->argv + 1);
    sp.envp = (const char *const *)spec->envp;
    sp.cwd = spec->cwd;
    sp.stdin_path = spec->stdin_path;
    sp.registered = on_register;
    sp.output = on_output;
    sp.ctx = p;
    err = etude_win_start(&ex->sup, &sp, &p->child);
    if (err != 0) {
        proc_release(p);
        errno = win_errno(err);
        return -1;
    }
    if (p->reg_error != 0) {
        err = (DWORD)p->reg_error;
        proc_abandon(p);
        errno = (int)err;
        return -1;
    }
    p->next = ex->procs;
    ex->procs = p;
    *pp = p;
    return 0;
}

/**
 * Record what the adapter has seen of P's child. When its first process
 * ends, the job ends what it left, as the POSIX keeper ends the descendants
 * of a case; the attempt finishes when the job is empty and the output has
 * closed.
 */
static void proc_settle(struct etude_proc *p)
{
    struct etude_win_usage u;
    struct etude_win_child *c = p->child;

    if (c->main_ended && p->r.ended_us == 0) {
        p->r.ended_us = etude_win_mono_us();
        p->r.exit_end = ETUDE_END_EXITED;
        p->r.exit_value = (int)c->status;
        etude_win_stop(c);
    }
    if (!c->ended || p->finished)
        return;
    if (p->cancelled && c->stopped && c->status == ETUDE_WIN_STOP_STATUS) {
        p->r.end = ETUDE_END_STOPPED;
        p->r.value = -1;
    } else {
        p->r.end = ETUDE_END_EXITED;
        p->r.value = (int)c->status;
    }
    p->r.out.closed = p->r.err.closed = 1;
    if (etude_win_usage(c, &u) == 0) {
        p->r.cpu_us.quality = ETUDE_QUALITY_EXACT;
        p->r.cpu_us.value = u.user_us + u.kernel_us;
        p->r.cpu_us.method = METHOD_CPU;
    }
    p->r.cleanup = ETUDE_CLEANUP_CONFIRMED;
    p->r.finished_us = etude_win_mono_us();
    p->finished = 1;
}

/** Give a finished attempt not yet returned, marking it returned. */
static struct etude_proc *take_finished(struct etude_exec *ex)
{
    struct etude_proc *p;

    for (p = ex->procs; p != NULL; p = p->next) {
        proc_settle(p);
        if (p->finished && !p->returned) {
            p->returned = 1;
            return p;
        }
    }
    return NULL;
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

struct etude_proc *etude_exec_wait(struct etude_exec *ex, int timeout_ms)
{
    ULONGLONG start = GetTickCount64();
    ULONGLONG waited;
    struct etude_proc *done;

    ex->output = 0;
    for (;;) {
        done = take_finished(ex);
        if (done != NULL || unfinished(ex) == 0 || ex->output)
            return done;
        waited = GetTickCount64() - start;
        if (timeout_ms >= 0 && waited >= (ULONGLONG)timeout_ms)
            return NULL;
        if (etude_win_poll(&ex->sup,
                           timeout_ms < 0 ? INFINITE : (DWORD)((ULONGLONG)timeout_ms - waited)) < 0)
            return NULL;
    }
}

void etude_exec_cancel(struct etude_proc *p)
{
    if (p->finished || p->cancelled)
        return;
    p->cancelled = 1;
    etude_win_stop(p->child);
}

const struct etude_exec_result *etude_proc_result(const struct etude_proc *p)
{
    return &p->r;
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
    proc_abandon(p);
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
    etude_win_sup_close(&ex->sup, REFUSED_BOUND);
    free(ex);
}
