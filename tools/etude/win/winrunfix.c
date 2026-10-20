/*
 * winrunfix.c: the runtime fixtures of etude on Windows. Each runs a case of
 * wintarget.exe as a worker runs one: its claim reserved in a ledger, the
 * case started under the supervisor's watchdog with the ledger as its
 * registration, and the claim ended with the cleanup the supervisor found.
 * The cases crash, hang past their bound, and leave children and
 * grandchildren behind; a controller in a process of its own is killed, or
 * loses the pipes that stand for its SSH connection, while its case runs.
 * Each fixture requires every process of the case to have ended and the
 * claim to be released.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/ledger.h"
#include "etude/watchdog.h"
#include "winexec.h"
#include "winfixture.h"
#include "winfmt.h"

/* The most processes of a case a fixture follows. */
#define MAX_TREE 16

/* The bytes of each output stream an attempt keeps. */
#define KEEP 65536

/* The case bound of every attempt, within the fixtures' completion bound. */
#define CASE_MS (BOUND / 2)

/* Gap bounds in milliseconds: one no case here reaches, that of the case
 * that hangs, and that of the controller whose connection is cut. */
#define LONG_GAP_MS 600000
#define HANG_GAP_MS 5000
#define CUT_GAP_MS  10000

/* The ways of run_case: print what a controller's fixture reads, and hold
 * the case at its `ready` line until its processes are held. */
#define RELAY 1
#define HOLD  2

/* What each claim reserves. */
static const struct etude_ledger_res claim_res[] = {
    {"cpu_millis", 1000},
    {"memory_bytes", 64ull << 20},
};

/* The processes of a case, held open so that a reused ID cannot mislead. */
struct tree {
    DWORD pid[MAX_TREE];
    HANDLE h[MAX_TREE]; /* NULL for a process that could not be opened */
    int n;
};

/** Add process PID to T, holding it open while it runs, unless T has it. */
static void tree_add(struct tree *t, DWORD pid)
{
    int i;

    for (i = 0; i < t->n; i++) {
        if (t->pid[i] == pid)
            return;
    }
    if (t->n == MAX_TREE || pid == 0)
        return;
    t->pid[t->n] = pid;
    t->h[t->n++] = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

/** Add each process that a line `child PID` of OUT names; give their count. */
static int tree_scan(struct tree *t, const char *out)
{
    const char *p = out;
    int count = 0;

    while (p != NULL && *p != '\0') {
        if (strncmp(p, "child ", 6) == 0) {
            tree_add(t, (DWORD)strtoul(p + 6, NULL, 10));
            count++;
        }
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    return count;
}

/** Count the processes of T that are held open. */
static int tree_held(const struct tree *t)
{
    int held = 0;
    int i;

    for (i = 0; i < t->n; i++)
        held += t->h[i] != NULL;
    return held;
}

/** Count the processes of T that have not ended. */
static int tree_alive(const struct tree *t)
{
    int alive = 0;
    int i;

    for (i = 0; i < t->n; i++)
        alive += t->h[i] != NULL && WaitForSingleObject(t->h[i], 0) != WAIT_OBJECT_0;
    return alive;
}

/** Wait up to MS milliseconds for every process of T to end. */
static int tree_ended(const struct tree *t, DWORD ms)
{
    ULONGLONG t0 = GetTickCount64();

    while (tree_alive(t) != 0) {
        if (GetTickCount64() - t0 > ms)
            return 0;
        Sleep(10);
    }
    return 1;
}

/** Close the handles of T. */
static void tree_close(struct tree *t)
{
    int i;

    for (i = 0; i < t->n; i++) {
        if (t->h[i] != NULL)
            CloseHandle(t->h[i]);
    }
    t->n = 0;
}

/* What a listing found of one claim. */
struct found {
    const char *id;
    int status; /* enum etude_claim_status; -1 when the ledger lacks it, -2
                   when it cannot be read */
    size_t nproc;
    long owner;
};

/** Note the claim a lookup asks for. */
static int find(const struct etude_ledger_claim *c, void *ctx)
{
    struct found *f = ctx;

    if (strcmp(c->id, f->id) != 0)
        return 0;
    f->status = (int)c->status;
    f->nproc = c->nproc;
    f->owner = c->owner.pid;
    return 1;
}

/** Look claim ID up in ledger L, after the recovery a listing makes. */
static struct found lookup(struct etude_ledger *l, const char *id)
{
    struct found f;

    memset(&f, 0, sizeof f);
    f.id = id;
    f.status = -1;
    if (etude_ledger_list(l, find, &f) < 0)
        f.status = -2;
    return f;
}

/** Remove the ledger in DIR: its files, its verdicts and the directory. */
static void ledger_remove(const char *dir)
{
    static const char *const files[] = {"claims", "claims.new", "lock", "verdicts", ""};
    char path[MAX_PATH * 4 + 96];
    wchar_t *w;
    size_t i;

    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(path, sizeof path, "%s%s%s", dir, files[i][0] != '\0' ? "\\" : "", files[i]);
        w = etude_win_long_path(path);
        if (w != NULL && !DeleteFileW(w))
            RemoveDirectoryW(w);
        free(w);
    }
}

/* One case run under a supervisor of this process, and what it showed. */
struct run {
    const char *name; /* the attempt's and the claim's identity */
    char dir[MAX_PATH * 4 + 64];
    int made; /* this run made the ledger, and removes it */
    struct etude_ledger *l;
    struct etude_supervisor *s;
    struct etude_supervised *a;
    struct etude_watchdog_spec wd;
    HANDLE go; /* the event that releases a held case, or NULL */
    struct tree t;
    int named;               /* `child` lines in the case's output */
    struct found when_ready; /* the claim once the case was ready */
};

/**
 * Open the ledger of run NAME, beside this program; with MAKE, remove what
 * an earlier run left first, and remove the ledger when the run is freed.
 */
static int run_open(struct run *r, const char *name, int make)
{
    memset(r, 0, sizeof *r);
    r->name = name;
    r->made = make;
    snprintf(r->dir, sizeof r->dir, "%s\\ledger-%s", scratch, name);
    if (make)
        ledger_remove(r->dir);
    r->l = etude_ledger_open(r->dir);
    CHECK(r->l != NULL, "ledger %s: %s", r->dir, strerror(errno));
    return 0;
}

/** Say whether attempt A's output has its `ready` line. */
static int ready(const struct etude_supervised *a)
{
    const char *out = etude_supervised_result(a)->proc->out.data;

    return out != NULL && strstr(out, "ready\n") != NULL;
}

/**
 * Reserve the claim of R and run wintarget.exe with ARGS as its attempt,
 * under a gap bound of GAP_MS, until it finishes or BOUND passes. Once it is
 * ready, hold its processes and note its claim. With HOLD among WAYS, the
 * case waits at its `ready` line until then, so that none of its processes
 * ends before it is held; with RELAY, the case's process, its output so far
 * and `controlled` are printed.
 */
static int run_case(struct run *r, const char *const *args, unsigned gap_ms, int ways)
{
    const struct etude_exec_result *p;
    struct etude_exec_spec spec;
    struct etude_supervised *done = NULL;
    char *argv[8];
    char go[64];
    ULONGLONG t0 = GetTickCount64();
    int seen = 0;
    int i;

    argv[0] = target;
    for (i = 0; i < 5 && args[i] != NULL; i++)
        argv[i + 1] = (char *)args[i];
    if (ways & HOLD) {
        snprintf(go, sizeof go, "Local\\etude-go-%lu-%s", (unsigned long)GetCurrentProcessId(),
                 r->name);
        r->go = CreateEventA(NULL, TRUE, FALSE, go);
        CHECK(r->go != NULL, "event: error %lu", (unsigned long)GetLastError());
        argv[++i] = go;
    }
    argv[i + 1] = NULL;
    r->wd.case_us = (uint64_t)CASE_MS * 1000u;
    r->wd.gap_us = (uint64_t)gap_ms * 1000u;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = r->name;
    spec.argv = argv;
    spec.output_max = KEEP;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = r->l;
    CHECK(etude_ledger_reserve(r->l, r->name, claim_res, 2, NULL, 0) == 0, "reserve: %s",
          strerror(errno));
    r->s = etude_supervisor_new();
    CHECK(r->s != NULL, "no supervisor");
    CHECK(etude_supervise_host(r->s, &spec, &r->wd, &r->a) == 0, "start: %s", strerror(errno));
    p = etude_supervised_result(r->a)->proc;
    while (done != r->a) {
        CHECK(GetTickCount64() - t0 <= BOUND, "the attempt did not finish within %d ms", BOUND);
        done = etude_supervise_wait(r->s, 1000);
        if (seen || !ready(r->a))
            continue;
        seen = 1;
        tree_add(&r->t, (DWORD)p->process.pid);
        r->named = tree_scan(&r->t, p->out.data);
        r->when_ready = lookup(r->l, r->name);
        if (r->go != NULL)
            SetEvent(r->go);
        if (ways & RELAY) {
            printf("case %ld %llu\n%scontrolled\n", p->process.pid,
                   (unsigned long long)p->process.start, p->out.data);
            fflush(stdout);
        }
    }
    CHECK(seen, "the case never said it was ready: %s", p->out.data);
    return 0;
}

/**
 * Judge what a run that finished left: its claim owned once the case was
 * ready, with the keeper, the case and the descendant it reported; NAMED
 * descendants in its output, each held open with the case while they ran;
 * the cleanup confirmed; every process of the case ended; and the claim
 * released once the cleanup is given to the ledger.
 *
 * A process leaves its job's count of active processes and closes its
 * handles before its own handle is signaled, so a process the executor has
 * seen end may show as running for a moment; each is given BOUND to end.
 */
static int judge_end(struct run *r, int named)
{
    const struct etude_supervised_result *res = etude_supervised_result(r->a);
    struct found f;

    CHECK(r->when_ready.status == ETUDE_CLAIM_OWNED && r->when_ready.nproc == 3,
          "the claim of the ready case was %d with %lu processes, not owned with 3",
          r->when_ready.status, (unsigned long)r->when_ready.nproc);
    CHECK(r->named == named, "%d descendants named, not %d", r->named, named);
    CHECK(tree_held(&r->t) == named + 1, "%d of the case's %d processes held", tree_held(&r->t),
          named + 1);
    CHECK(res->cleanup == ETUDE_CLEANUP_CONFIRMED && !res->claim_kept, "cleanup %d, claim kept %d",
          (int)res->cleanup, res->claim_kept);
    CHECK(tree_ended(&r->t, BOUND), "%d processes of the case outlived its attempt",
          tree_alive(&r->t));
    CHECK(etude_ledger_finish(r->l, r->name, res->cleanup) == 0, "finish: %s", strerror(errno));
    f = lookup(r->l, r->name);
    CHECK(f.status == -1, "the claim stays, as %d", f.status);
    return 0;
}

/** Free what R holds, removing its ledger when it made it; give RESULT. */
static int run_free(struct run *r, int result)
{
    if (r->s != NULL)
        etude_supervisor_free(r->s);
    tree_close(&r->t);
    if (r->l != NULL)
        etude_ledger_close(r->l);
    if (r->go != NULL)
        CloseHandle(r->go);
    if (r->made)
        ledger_remove(r->dir);
    return result;
}

int fx_runtime_crash(void)
{
    static const char *const args[] = {"family", "1", "crash", NULL};
    const struct etude_supervised_result *res;
    struct run r;
    int rc;

    rc = run_open(&r, "crash", 1);
    if (rc == 0)
        rc = run_case(&r, args, LONG_GAP_MS, HOLD);
    if (rc != 0)
        return run_free(&r, rc);
    res = etude_supervised_result(r.a);
    if (res->end != ETUDE_END_EXITED || res->value != (int)EXCEPTION_ACCESS_VIOLATION ||
        res->expiry != ETUDE_EXPIRY_NONE || res->incomplete)
        rc = failed("end %d with %#x, expiry %s, incomplete %d", (int)res->end,
                    (unsigned)res->value, etude_expiry_name(res->expiry), res->incomplete);
    if (rc == 0)
        rc = judge_end(&r, 2);
    return run_free(&r, rc);
}

int fx_runtime_hang(void)
{
    static const char *const args[] = {"family", "2", "hang", NULL};
    const struct etude_supervised_result *res;
    struct run r;
    int rc;

    rc = run_open(&r, "hang", 1);
    if (rc == 0)
        rc = run_case(&r, args, HANG_GAP_MS, 0);
    if (rc != 0)
        return run_free(&r, rc);
    res = etude_supervised_result(r.a);
    if (res->expiry != ETUDE_EXPIRY_GAP || !res->incomplete || !res->asked ||
        res->end != ETUDE_END_STOPPED || res->bound_us != (uint64_t)HANG_GAP_MS * 1000u)
        rc = failed("expiry %s of %llu us, incomplete %d, asked %d, end %d",
                    etude_expiry_name(res->expiry), (unsigned long long)res->bound_us,
                    res->incomplete, res->asked, (int)res->end);
    else if (res->owned_count != 2 || res->owned[1].pid != res->proc->process.pid)
        rc = failed("%lu processes owned at the expiry", (unsigned long)res->owned_count);
    else if (res->sample_cpu_us.quality != ETUDE_QUALITY_EXACT ||
             res->sample_rss_bytes.quality != ETUDE_QUALITY_LOWER_BOUND ||
             res->sample_rss_bytes.value == 0)
        rc = failed("the sample at the expiry: %s, %s", res->sample_cpu_us.method,
                    res->sample_rss_bytes.method);
    if (rc == 0)
        rc = judge_end(&r, 4);
    return run_free(&r, rc);
}

int fx_runtime_orphans(void)
{
    static const char *const args[] = {"family", "2", "exit", NULL};
    const struct etude_supervised_result *res;
    struct run r;
    int rc;

    rc = run_open(&r, "orphans", 1);
    if (rc == 0)
        rc = run_case(&r, args, LONG_GAP_MS, HOLD);
    if (rc != 0)
        return run_free(&r, rc);
    res = etude_supervised_result(r.a);
    if (res->end != ETUDE_END_EXITED || res->value != 0 || res->incomplete ||
        res->proc->children != 1 || res->proc->foreign != 0)
        rc = failed("end %d with %d, incomplete %d, %u children and %u foreign", (int)res->end,
                    res->value, res->incomplete, res->proc->children, res->proc->foreign);
    if (rc == 0)
        rc = judge_end(&r, 4);
    return run_free(&r, rc);
}

int runtime_controller(const char *id, const char *gap)
{
    static const char *const args[] = {"family", "1", "hang", NULL};
    const struct etude_supervised_result *res;
    struct etude_pid me;
    struct found f;
    struct run r;
    int ok;
    int rc;

    if (etude_process_state((long)GetCurrentProcessId(), &me) != 1)
        return failed("this process has no identity");
    printf("controller %ld %llu\n", me.pid, (unsigned long long)me.start);
    fflush(stdout);
    rc = run_open(&r, id, 0);
    if (rc == 0)
        rc = run_case(&r, args, (unsigned)strtoul(gap, NULL, 10), RELAY);
    if (rc != 0)
        return run_free(&r, rc);
    res = etude_supervised_result(r.a);
    if (etude_ledger_finish(r.l, id, res->cleanup) != 0)
        rc = failed("finish: %s", strerror(errno));
    f = lookup(r.l, id);
    ok = res->expiry == ETUDE_EXPIRY_GAP && res->cleanup == ETUDE_CLEANUP_CONFIRMED &&
         f.status == -1;
    printf("winfixture: result: %s: expiry %s, cleanup %s, claim %s\n", ok ? "pass" : "fail",
           etude_expiry_name(res->expiry),
           res->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed",
           f.status == -1 ? "released" : "kept");
    fflush(stdout);
    if (rc == 0 && !ok)
        rc = failed("the case was not ended at its gap bound with its claim released");
    return run_free(&r, rc);
}

/* A controller in a process of its own, in a job of this program's that
 * ends whatever is left of it, with pipes for its standard streams. */
struct controller {
    HANDLE job;
    HANDLE process;
    HANDLE in;  /* the write end of its standard input */
    HANDLE out; /* the read end of its standard output and error */
    char text[8192];
    size_t len;
};

/** End whatever is left of C's job, and close what C holds. */
static void controller_close(struct controller *c)
{
    if (c->job != NULL) {
        TerminateJobObject(c->job, 1);
        CloseHandle(c->job);
    }
    if (c->process != NULL) {
        WaitForSingleObject(c->process, BOUND);
        CloseHandle(c->process);
    }
    if (c->in != NULL)
        CloseHandle(c->in);
    if (c->out != NULL)
        CloseHandle(c->out);
    memset(c, 0, sizeof *c);
}

/**
 * Make C's job, which ends what is in it when its last handle closes, and
 * the pipes of the controller's standard streams, whose ends for the
 * controller, its input and its output, go in `inherit`.
 *
 * @return 0, or the error of the system.
 */
static DWORD controller_pipes(struct controller *c, HANDLE *inherit)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;

    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if ((c->job = CreateJobObjectW(NULL, NULL)) == NULL ||
        !SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &lim, sizeof lim) ||
        !CreatePipe(&inherit[0], &c->in, &sa, 0) || !CreatePipe(&c->out, &inherit[1], &sa, 0) ||
        !SetHandleInformation(c->in, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(c->out, HANDLE_FLAG_INHERIT, 0))
        return GetLastError();
    return 0;
}

/**
 * Make in `*attrs` an attribute list by which a child inherits the two
 * handles of `inherit` and no other; the caller deletes and frees it.
 *
 * @return 0, or the error of the system with `*attrs` NULL.
 */
static DWORD inherit_only(HANDLE *inherit, LPPROC_THREAD_ATTRIBUTE_LIST *attrs)
{
    SIZE_T size = 0;
    DWORD err = 0;

    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    *attrs = malloc(size);
    if (*attrs == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!InitializeProcThreadAttributeList(*attrs, 1, 0, &size)) {
        err = GetLastError();
        free(*attrs);
        *attrs = NULL;
        return err;
    }
    if (!UpdateProcThreadAttribute(*attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   2 * sizeof inherit[0], NULL, NULL)) {
        err = GetLastError();
        DeleteProcThreadAttributeList(*attrs);
        free(*attrs);
        *attrs = NULL;
    }
    return err;
}

/**
 * Start this program as the controller of claim ID with a gap bound of
 * GAP_MS, in a new job, with pipes for its standard input, output and error,
 * which are all it inherits.
 *
 * @return 0, or the error of the system.
 */
static DWORD controller_start(struct controller *c, const char *id, unsigned gap_ms)
{
    char gap[16];
    const char *const argv[] = {"runtime-controller", id, gap, NULL};
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    PROCESS_INFORMATION pi;
    STARTUPINFOEXW si;
    HANDLE inherit[2] = {NULL, NULL};
    wchar_t *prog = NULL, *line = NULL;
    char *text = NULL;
    DWORD err;

    memset(c, 0, sizeof *c);
    snprintf(gap, sizeof gap, "%u", gap_ms);
    err = controller_pipes(c, inherit);
    if (err == 0 && etude_win_cmdline(fixture_path, argv, &text) != 0)
        err = ERROR_INVALID_PARAMETER;
    if (err == 0 &&
        ((prog = etude_win_wide(fixture_path)) == NULL || (line = etude_win_wide(text)) == NULL))
        err = ERROR_NOT_ENOUGH_MEMORY;
    if (err == 0)
        err = inherit_only(inherit, &attrs);
    if (err == 0) {
        memset(&si, 0, sizeof si);
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = inherit[0];
        si.StartupInfo.hStdOutput = inherit[1];
        si.StartupInfo.hStdError = inherit[1];
        si.lpAttributeList = attrs;
        if (!CreateProcessW(prog, line, NULL, NULL, TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            NULL, NULL, &si.StartupInfo, &pi))
            err = GetLastError();
    }
    if (err == 0) {
        c->process = pi.hProcess;
        if (!AssignProcessToJobObject(c->job, pi.hProcess)) {
            err = GetLastError();
            TerminateProcess(pi.hProcess, 1);
        } else {
            ResumeThread(pi.hThread);
        }
        CloseHandle(pi.hThread);
    }
    if (attrs != NULL) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    free(prog);
    free(line);
    free(text);
    if (inherit[0] != NULL)
        CloseHandle(inherit[0]);
    if (inherit[1] != NULL)
        CloseHandle(inherit[1]);
    if (err != 0)
        controller_close(c);
    return err;
}

/**
 * Read C's output until it has NEEDLE, for at most BOUND milliseconds.
 *
 * @return 0, ERROR_TIMEOUT, or the error of the system.
 */
static DWORD controller_wait(struct controller *c, const char *needle)
{
    ULONGLONG t0 = GetTickCount64();
    DWORD avail, got;

    while (strstr(c->text, needle) == NULL) {
        if (GetTickCount64() - t0 > BOUND)
            return ERROR_TIMEOUT;
        if (!PeekNamedPipe(c->out, NULL, 0, NULL, &avail, NULL))
            return GetLastError();
        if (avail == 0) {
            Sleep(10);
            continue;
        }
        if (avail > sizeof c->text - 1 - c->len)
            avail = (DWORD)(sizeof c->text - 1 - c->len);
        if (avail == 0)
            return ERROR_INSUFFICIENT_BUFFER;
        if (!ReadFile(c->out, c->text + c->len, avail, &got, NULL))
            return GetLastError();
        c->len += got;
        c->text[c->len] = '\0';
    }
    return 0;
}

/**
 * Start a controller of the run NAME with gap GAP_MS, wait until its case is
 * ready, and hold the case's processes, which its output names. The claim
 * must be owned by the controller, with the keeper, the case and its child,
 * and the case's three processes must run.
 */
static int controlled(struct controller *c, struct run *r, const char *name, unsigned gap_ms)
{
    const char *p;
    DWORD err;

    memset(c, 0, sizeof *c);
    if (run_open(r, name, 1) != 0)
        return 1;
    err = controller_start(c, name, gap_ms);
    CHECK(err == 0, "controller: error %lu", (unsigned long)err);
    err = controller_wait(c, "controlled\n");
    CHECK(err == 0, "the controller's case was not ready (error %lu): %s", (unsigned long)err,
          c->text);
    p = strstr(c->text, "\ncase ");
    CHECK(p != NULL, "the controller named no case: %s", c->text);
    tree_add(&r->t, (DWORD)strtoul(p + 6, NULL, 10));
    r->named = tree_scan(&r->t, c->text);
    r->when_ready = lookup(r->l, name);
    CHECK(r->when_ready.status == ETUDE_CLAIM_OWNED && r->when_ready.nproc == 3 &&
              r->when_ready.owner == (long)GetProcessId(c->process),
          "the controller's claim is %d with %lu processes and owner %ld", r->when_ready.status,
          (unsigned long)r->when_ready.nproc, r->when_ready.owner);
    CHECK(r->named == 2 && tree_alive(&r->t) == 3, "%d processes named and %d running, not 2 and 3",
          r->named, tree_alive(&r->t));
    return 0;
}

int fx_runtime_killed(void)
{
    struct controller c;
    struct run r;
    struct found f;
    size_t freed = 9;
    int rc;

    rc = controlled(&c, &r, "killed", LONG_GAP_MS);
    if (rc == 0 && !TerminateProcess(c.process, 1))
        rc = failed("cannot kill the controller: error %lu", (unsigned long)GetLastError());
    if (rc == 0 && WaitForSingleObject(c.process, BOUND) != WAIT_OBJECT_0)
        rc = failed("the controller did not end");
    if (rc == 0 && !tree_ended(&r.t, BOUND))
        rc = failed("%d processes of the case outlived the controller", tree_alive(&r.t));
    if (rc == 0 && (etude_ledger_recover(r.l, &freed) != 0 || freed != 1))
        rc = failed("recovery freed %lu claims, not 1", (unsigned long)freed);
    if (rc == 0 && (f = lookup(r.l, r.name)).status != -1)
        rc = failed("the dead controller's claim stays, as %d", f.status);
    controller_close(&c);
    return run_free(&r, rc);
}

int fx_runtime_cut(void)
{
    struct controller c;
    struct run r;
    struct found f;
    size_t freed = 9;
    DWORD status = 0;
    int rc;

    rc = controlled(&c, &r, "cut", CUT_GAP_MS);
    if (rc == 0) {
        CloseHandle(c.in);
        CloseHandle(c.out);
        c.in = c.out = NULL;
        if (tree_alive(&r.t) != 3)
            rc = failed("the case ended at the cut");
    }
    if (rc == 0 && WaitForSingleObject(c.process, BOUND) != WAIT_OBJECT_0)
        rc = failed("the controller did not end after its case's gap bound");
    if (rc == 0 && (!GetExitCodeProcess(c.process, &status) || status != 0))
        rc = failed("the controller ended with %lu", (unsigned long)status);
    if (rc == 0 && !tree_ended(&r.t, BOUND))
        rc = failed("%d processes of the case outlived the controller", tree_alive(&r.t));
    if (rc == 0 && (etude_ledger_recover(r.l, &freed) != 0 || freed != 0))
        rc = failed("recovery freed %lu claims, not 0", (unsigned long)freed);
    if (rc == 0 && (f = lookup(r.l, r.name)).status != -1)
        rc = failed("the controller's claim stays, as %d", f.status);
    controller_close(&c);
    return run_free(&r, rc);
}
