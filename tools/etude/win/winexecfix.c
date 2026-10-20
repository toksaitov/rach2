/*
 * winexecfix.c: the fixtures of etude's executor on Windows, exec.c, which
 * run wintarget.exe through the interface of etude/exec.h, as the controller
 * will.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/exec.h"
#include "winexec.h"
#include "winfixture.h"

/* The attempts that run at once. */
enum { A_EXIT, A_CANCEL, A_BOUND, A_ORPHAN, A_TREE, ATTEMPTS };

/* The bytes of output an attempt keeps, but for A_BOUND. */
#define KEEP 65536

/* What a registration saw of one attempt. */
struct seen {
    char attempt[16];
    struct etude_pid keeper;
    struct etude_pid process;
    HANDLE held; /* the refused process, kept so that its status can be read */
    int calls;
};

/* The registrations, one for each attempt started. */
static struct seen seen[ATTEMPTS + 3];
static int nseen;

/** The registration: record what it saw, and refuse the attempt `refuse`. */
static int record(void *ctx, const char *attempt_id, const struct etude_pid *keeper,
                  const struct etude_pid *process)
{
    struct seen *s;

    (void)ctx;
    if (nseen == (int)(sizeof seen / sizeof seen[0]))
        return EIO;
    s = &seen[nseen++];
    snprintf(s->attempt, sizeof s->attempt, "%s", attempt_id);
    s->keeper = *keeper;
    s->process = *process;
    s->calls++;
    if (strcmp(attempt_id, "refuse") != 0)
        return 0;
    s->held =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)process->pid);
    return EPERM;
}

/** The registration of ATTEMPT, or NULL. */
static const struct seen *seen_of(const char *attempt)
{
    int i;

    for (i = 0; i < nseen; i++) {
        if (strcmp(seen[i].attempt, attempt) == 0)
            return &seen[i];
    }
    return NULL;
}

/** Whether process ID, as it was, has ended. */
static int gone(const struct etude_pid *id)
{
    struct etude_pid now;
    int state = etude_process_state(id->pid, &now);

    return state == 0 || (state == 1 && now.start != id->start);
}

/**
 * Whether every process that the lines `child PID` of OUT name has ended; the
 * target writes one for each copy it starts.
 */
static int children_gone(const char *out, int *count)
{
    struct etude_pid id;
    const char *p;
    long pid;

    *count = 0;
    for (p = out; p != NULL && (p = strstr(p, "child ")) != NULL; p++) {
        if (p != out && p[-1] != '\n')
            continue;
        pid = strtol(p + 6, NULL, 10);
        if (pid <= 0)
            continue;
        (*count)++;
        if (etude_process_state(pid, &id) == 1)
            return 0;
    }
    return 1;
}

/** Fill SPEC to run the target with ARGV as ATTEMPT. */
static void spec_of(struct etude_exec_spec *spec, const char *attempt, char **argv)
{
    memset(spec, 0, sizeof *spec);
    spec->attempt_id = attempt;
    spec->argv = argv;
    spec->output_max = KEEP;
    spec->reg = record;
}

/** Judge what the attempts that ran at once report. */
static int judge(struct etude_proc **p)
{
    const struct etude_exec_result *r[ATTEMPTS];
    struct etude_pid me;
    const struct seen *s;
    int n;
    int i;

    for (i = 0; i < ATTEMPTS; i++) {
        r[i] = etude_proc_result(p[i]);
        s = seen_of(r[i]->attempt_id);
        CHECK(s != NULL && s->calls == 1, "%s was not registered once", r[i]->attempt_id);
        CHECK(s->process.pid == r[i]->process.pid && s->process.start == r[i]->process.start,
              "%s: the registered process is not the result's", r[i]->attempt_id);
        CHECK(etude_process_state((long)GetCurrentProcessId(), &me) == 1 &&
                  me.start == r[i]->keeper.start && me.pid == r[i]->keeper.pid,
              "%s: the keeper is not the controller", r[i]->attempt_id);
        CHECK(r[i]->cleanup == ETUDE_CLEANUP_CONFIRMED, "%s: cleanup %d", r[i]->attempt_id,
              (int)r[i]->cleanup);
        CHECK(r[i]->out.closed && r[i]->err.closed, "%s: open output", r[i]->attempt_id);
        CHECK(r[i]->started_us <= r[i]->ended_us && r[i]->ended_us <= r[i]->finished_us,
              "%s: times out of order", r[i]->attempt_id);
        CHECK(gone(&r[i]->process), "%s: the process outlived its attempt", r[i]->attempt_id);
        CHECK(r[i]->cpu_us.quality == ETUDE_QUALITY_EXACT &&
                  r[i]->peak_bytes.quality == ETUDE_QUALITY_UNAVAILABLE,
              "%s: measurements of quality %d and %d", r[i]->attempt_id, (int)r[i]->cpu_us.quality,
              (int)r[i]->peak_bytes.quality);
    }
    CHECK(r[A_EXIT]->end == ETUDE_END_EXITED && r[A_EXIT]->value == 3 &&
              r[A_EXIT]->exit_end == ETUDE_END_EXITED && r[A_EXIT]->exit_value == 3,
          "exit: end %d value %d", (int)r[A_EXIT]->end, r[A_EXIT]->value);
    CHECK(r[A_CANCEL]->end == ETUDE_END_STOPPED && r[A_CANCEL]->value == -1,
          "cancel: end %d value %d", (int)r[A_CANCEL]->end, r[A_CANCEL]->value);
    CHECK(children_gone(r[A_CANCEL]->out.data, &n) && n == 2,
          "cancel: %d children named, not all gone", n);
    CHECK(r[A_BOUND]->out.kept == 4096 && r[A_BOUND]->out.total == 100000 &&
              memcmp(r[A_BOUND]->out.data, "abcdefghijklmnopqrstuvw", 23) == 0 &&
              strcmp(r[A_BOUND]->err.data, "big done\n") == 0,
          "bound: kept %lu of %llu", (unsigned long)r[A_BOUND]->out.kept,
          (unsigned long long)r[A_BOUND]->out.total);
    CHECK(r[A_ORPHAN]->end == ETUDE_END_EXITED && r[A_ORPHAN]->value == 0 &&
              r[A_ORPHAN]->children == 1 && r[A_ORPHAN]->foreign == 0,
          "orphan: end %d, %u children and %u foreign", (int)r[A_ORPHAN]->end,
          r[A_ORPHAN]->children, r[A_ORPHAN]->foreign);
    CHECK(children_gone(r[A_ORPHAN]->out.data, &n) && n == 1, "orphan: the descendant lives");
    CHECK(r[A_TREE]->end == ETUDE_END_EXITED && r[A_TREE]->cpu_us.value >= 300000,
          "tree: %llu us of processor time", (unsigned long long)r[A_TREE]->cpu_us.value);
    return 0;
}

int fx_exec_attempts(void)
{
    static const char *const names[ATTEMPTS] = {"exit", "cancel", "bound", "orphan", "tree"};
    char *argv[ATTEMPTS][5] = {{target, "exit", "3", NULL},
                               {target, "hangtree", "2", NULL},
                               {target, "big", "100000", NULL},
                               {target, "orphan", NULL},
                               {target, "tree", "2", "100", NULL}};
    struct etude_proc *p[ATTEMPTS] = {NULL};
    struct etude_exec_spec spec;
    struct etude_proc *done;
    struct etude_exec *ex;
    ULONGLONG t0 = GetTickCount64();
    int returned = 0;
    int cancelled = 0;
    int i;
    int r = 0;

    ex = etude_exec_new();
    CHECK(ex != NULL, "no executor");
    for (i = 0; i < ATTEMPTS && r == 0; i++) {
        spec_of(&spec, names[i], argv[i]);
        if (i == A_BOUND)
            spec.output_max = 4096;
        if (etude_exec_start(ex, &spec, &p[i]) != 0)
            r = failed("%s: start: %s", names[i], strerror(errno));
    }
    while (r == 0 && returned < ATTEMPTS) {
        if (GetTickCount64() - t0 > BOUND)
            r = failed("%d of %d attempts finished", returned, ATTEMPTS);
        done = etude_exec_wait(ex, 1000);
        returned += done != NULL;
        if (!cancelled && strstr(etude_proc_result(p[A_CANCEL])->out.data, "ready\n") != NULL) {
            etude_exec_cancel(p[A_CANCEL]);
            cancelled = 1;
        }
    }
    /* A late attempt may never end; its job ends with this process. */
    if (r != 0 && returned < ATTEMPTS)
        return r;
    if (r == 0)
        r = judge(p);
    etude_exec_free(ex);
    return r;
}

int fx_exec_unrun(void)
{
    char *refused[] = {target, "hang", NULL};
    char *missing[] = {"C:\\etude-missing\\none.exe", NULL};
    struct etude_exec_spec spec;
    const struct seen *s;
    struct etude_proc *p = NULL;
    struct etude_exec *ex;
    DWORD status = 0;
    int rc;
    int err;
    int r = 0;

    ex = etude_exec_new();
    CHECK(ex != NULL, "no executor");
    spec_of(&spec, "refuse", refused);
    rc = etude_exec_start(ex, &spec, &p);
    err = errno;
    s = seen_of("refuse");
    if (s != NULL && s->held != NULL) {
        if (WaitForSingleObject(s->held, 0) == WAIT_OBJECT_0)
            GetExitCodeProcess(s->held, &status);
        CloseHandle(s->held);
    }
    if (rc != -1 || err != EPERM)
        r = failed("a refused registration gave %d, %s", rc, strerror(err));
    else if (status != ETUDE_WIN_REFUSED_STATUS)
        r = failed("the refused process ended with %#lx, not as refused", (unsigned long)status);
    else if (etude_exec_wait(ex, 0) != NULL)
        r = failed("the refused attempt was returned");
    spec_of(&spec, "missing", missing);
    if (r == 0 && (etude_exec_start(ex, &spec, &p) != -1 || errno != ENOENT))
        r = failed("a missing program gave %s", strerror(errno));
    etude_exec_free(ex);
    return r;
}

int fx_exec_sample(void)
{
    char *argv[] = {target, "hold", "64", "300", NULL};
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL;
    struct etude_proc *done = NULL;
    struct etude_exec *ex;
    ULONGLONG t0 = GetTickCount64();
    uint64_t cpu = 0, rss = 0, cpu_end, rss_end;
    int running, ended;
    int r = 0;

    ex = etude_exec_new();
    CHECK(ex != NULL, "no executor");
    spec_of(&spec, "sample", argv);
    if (etude_exec_start(ex, &spec, &p) != 0) {
        r = failed("start: %s", strerror(errno));
        etude_exec_free(ex);
        return r;
    }
    while (strstr(etude_proc_result(p)->out.data, "ready\n") == NULL &&
           GetTickCount64() - t0 < BOUND)
        etude_exec_wait(ex, 1000);
    running = etude_proc_sample(p, &cpu, &rss);
    etude_exec_cancel(p);
    while (done != p && GetTickCount64() - t0 < BOUND)
        done = etude_exec_wait(ex, 1000);
    ended = etude_proc_sample(p, &cpu_end, &rss_end);
    if (running != 1 || cpu < 300000 || rss < 64ull << 20)
        r = failed("the running case sampled %d, with %llu us and %llu bytes", running,
                   (unsigned long long)cpu, (unsigned long long)rss);
    else if (done != p || ended != 0)
        r = failed("the ended case sampled %d", ended);
    etude_exec_free(ex);
    return r;
}

int fx_exec_poll(void)
{
    char *argv[] = {target, "args", "polled", NULL};
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL;
    struct etude_proc *done = NULL;
    struct etude_exec *ex;
    ULONGLONG t0 = GetTickCount64();
    int waits = 0;
    int r = 0;

    ex = etude_exec_new();
    CHECK(ex != NULL, "no executor");
    spec_of(&spec, "poll", argv);
    if (etude_exec_start(ex, &spec, &p) != 0) {
        r = failed("start: %s", strerror(errno));
        etude_exec_free(ex);
        return r;
    }
    while (done != p && GetTickCount64() - t0 < BOUND) {
        Sleep(10);
        done = etude_exec_wait(ex, 0);
        waits++;
    }
    if (done != p)
        r = failed("%d waits of no time did not finish the attempt, which has %lu bytes", waits,
                   (unsigned long)etude_proc_result(p)->out.kept);
    else if (strcmp(etude_proc_result(p)->out.data, "arg 0 polled\nargs 1\n") != 0)
        r = failed("the attempt gave other output: %s", etude_proc_result(p)->out.data);
    etude_exec_free(ex);
    return r;
}
