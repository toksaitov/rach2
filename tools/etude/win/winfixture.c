/*
 * winfixture.c: the fixtures of etude's Windows adapter. Each starts
 * wintarget.exe, which must be beside this program, through the adapter and
 * judges what the adapter reports against what the target and Windows say:
 * the suspended start and the registration, the accounting of a tree of
 * processes, the end of a tree by its job and by the death of its
 * controller, output, arguments, paths, the environment, sockets, the
 * reading of a reset connection, and clocks.
 * Others run the executor of etude/exec.h, the ledger, the supervisor with
 * both, and etude.exe, which must also be beside this program: `etude
 * provision tooling` with winstand.exe, `etude lock tree`, `etude lock
 * ports` and the host lock of `etude admit`.
 *
 * Usage: winfixture [NAME...] runs the fixtures named, or all of them, and
 * prints `winfixture: result: pass: NAME` or `winfixture: result: fail: NAME:
 * DETAIL` for each and a summary line last; it exits 0 when every one passed.
 * `winfixture controller` is the controller that the controller-death
 * fixture starts and kills, and `winfixture runtime-controller ID GAP` the
 * one that the runtime-killed and runtime-cut fixtures start. Each wait has a
 * completion bound of BOUND milliseconds, which no correct run comes near.
 */
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sockets.h"
#include "winexec.h"
#include "winfixture.h"
#include "winfmt.h"

#include <tlhelp32.h>

/* The most processes a fixture lists in a job. */
#define MAX_PIDS 64

/* An output stream as a fixture keeps it. */
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

/* What one child of a fixture has shown: its output and its registration. */
struct cap {
    struct buf out;
    struct buf err;
    int registered; /* the registration function ran */
    int refuse;     /* the registration function refuses the child */
    int checked;    /* the registration found the child as it must be */
    char why[256];  /* why the registration's checks failed */
};

static struct etude_win_sup sup;
char fixture_path[MAX_PATH * 4];
char scratch[MAX_PATH * 4];
char binary_dir[MAX_PATH * 4];
char target[sizeof scratch + 16];
static const char *current;
static char detail[512];

int failed(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    return 1;
}

/** Append N bytes of DATA to B. */
static void append(struct buf *b, const char *data, size_t n)
{
    char *p;

    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        p = realloc(b->data, b->cap);
        if (p == NULL) {
            fprintf(stderr, "winfixture: out of memory\n");
            exit(2);
        }
        b->data = p;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/** Free what CAP holds and empty it. */
static void cap_free(struct cap *cap)
{
    free(cap->out.data);
    free(cap->err.data);
    memset(cap, 0, sizeof *cap);
}

/** The adapter's output function: keep each stream apart. */
static void on_output(void *ctx, struct etude_win_child *c, enum etude_win_stream s,
                      const char *data, size_t n)
{
    struct cap *cap = ctx;

    (void)c;
    append(s == ETUDE_WIN_STDOUT ? &cap->out : &cap->err, data, n);
}

/**
 * Return the previous suspend count of the one thread of process PID, as
 * SuspendThread gives it, and resume the thread again; -1 when it has no
 * single thread that can be opened.
 */
static long suspend_count(DWORD pid)
{
    THREADENTRY32 te;
    HANDLE snap;
    HANDLE th;
    DWORD tid = 0;
    int threads = 0;
    DWORD prev;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                tid = te.th32ThreadID;
                threads++;
            }
            te.dwSize = sizeof te;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (threads != 1)
        return -1;
    th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    if (th == NULL)
        return -1;
    prev = SuspendThread(th);
    ResumeThread(th);
    CloseHandle(th);
    return prev == (DWORD)-1 ? -1 : (long)prev;
}

/**
 * The adapter's registration function. It checks that the child is in its
 * job, that its one thread is suspended, that it has written nothing, and
 * that its identity is the process's own, then lets it run or refuses it.
 */
static int on_register(void *ctx, struct etude_win_child *c, const struct etude_win_ident *id)
{
    struct cap *cap = ctx;
    FILETIME ft[4];
    uint64_t created;
    BOOL in = FALSE;
    long count;

    cap->registered = 1;
    if (!IsProcessInJob(c->process, c->job, &in) || !in) {
        snprintf(cap->why, sizeof cap->why, "the child is not in its job when registered");
        return cap->refuse;
    }
    count = suspend_count(id->pid);
    if (count == -1) {
        snprintf(cap->why, sizeof cap->why, "the child does not have one thread, so it has run");
        return cap->refuse;
    }
    if (count != 1) {
        snprintf(cap->why, sizeof cap->why, "the child's thread had suspend count %ld, not 1",
                 count);
        return cap->refuse;
    }
    if (cap->out.len != 0 || cap->err.len != 0) {
        snprintf(cap->why, sizeof cap->why, "the child wrote before it was registered");
        return cap->refuse;
    }
    GetProcessTimes(c->process, &ft[0], &ft[1], &ft[2], &ft[3]);
    created = (uint64_t)ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime;
    if (id->pid != GetProcessId(c->process) || id->created != created || created == 0) {
        snprintf(cap->why, sizeof cap->why, "the identity %lu/%llu is not the process's",
                 (unsigned long)id->pid, (unsigned long long)id->created);
        return cap->refuse;
    }
    cap->checked = 1;
    return cap->refuse;
}

/** Poll until C ends, for at most BOUND milliseconds. */
static DWORD wait_end(struct etude_win_child *c)
{
    ULONGLONG start = GetTickCount64();

    while (!c->ended) {
        if (GetTickCount64() - start > BOUND)
            return ERROR_TIMEOUT;
        if (etude_win_poll(&sup, 1000) < 0)
            return GetLastError();
    }
    return 0;
}

/** Poll until the standard output of CAP has NEEDLE, for at most BOUND ms. */
static DWORD wait_text(struct cap *cap, const char *needle)
{
    ULONGLONG start = GetTickCount64();

    while (cap->out.data == NULL || strstr(cap->out.data, needle) == NULL) {
        if (GetTickCount64() - start > BOUND)
            return ERROR_TIMEOUT;
        if (etude_win_poll(&sup, 1000) < 0)
            return GetLastError();
    }
    return 0;
}

/** The spawn of PROGRAM with ARGV, reporting to CAP, with nothing else set. */
static struct etude_win_spawn spawn_of(const char *program, const char *const *argv,
                                       struct cap *cap)
{
    struct etude_win_spawn sp;

    memset(&sp, 0, sizeof sp);
    sp.program = program;
    sp.argv = argv;
    sp.registered = on_register;
    sp.output = on_output;
    sp.ctx = cap;
    return sp;
}

/** Start PROGRAM with ARGV through the adapter, reporting to CAP. */
static DWORD start(const char *program, const char *const *argv, struct cap *cap,
                   struct etude_win_child **c)
{
    struct etude_win_spawn sp = spawn_of(program, argv, cap);

    return etude_win_start(&sup, &sp, c);
}

/** Start the target with ARGV and wait until it ends. */
static DWORD run(const char *const *argv, struct cap *cap, struct etude_win_child **c)
{
    DWORD err = start(target, argv, cap, c);

    return err != 0 ? err : wait_end(*c);
}

/** Give back a child's job, reporting a child that would not end. */
static int done(struct etude_win_child *c, int result)
{
    if (c != NULL && etude_win_release(c, BOUND) != 0 && result == 0)
        return failed("the child did not end when released");
    return result;
}

/** Return the text of the Win32 error ERR, from a static buffer. */
static const char *errtext(DWORD err)
{
    static char buf[256];

    etude_win_strerror(err, buf, sizeof buf);
    return buf;
}

/** Count the lines of S that begin with PREFIX, and sum the numbers after. */
static int count_lines(const char *s, const char *prefix, unsigned long long *sum)
{
    size_t n = strlen(prefix);
    int count = 0;

    if (sum != NULL)
        *sum = 0;
    while (s != NULL && *s != '\0') {
        if (strncmp(s, prefix, n) == 0) {
            count++;
            if (sum != NULL)
                *sum += strtoull(s + n, NULL, 10);
        }
        s = strchr(s, '\n');
        if (s != NULL)
            s++;
    }
    return count;
}

/** A child's exit status reaches the executor, with no output. */
static int fx_exit_status(void)
{
    static const char *const argv[] = {"exit", "7", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = c->status != 7 ? failed("status %lu, not 7", (unsigned long)c->status)
        : c->stopped   ? failed("the child is marked stopped")
        : cap.out.len != 0 || cap.err.len != 0 ? failed("output from a child that wrote none")
        : !cap.checked                         ? failed("%s", cap.why)
                                               : 0;
    cap_free(&cap);
    return done(c, r);
}

/** A crash ends the child with its exception's status and no dialog. */
static int fx_crash(void)
{
    static const char *const argv[] = {"crash", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = c->status != (DWORD)EXCEPTION_ACCESS_VIOLATION
            ? failed("status %#lx, not an access violation", (unsigned long)c->status)
            : 0;
    cap_free(&cap);
    return done(c, r);
}

/** A mebibyte of output arrives whole and in order, apart from the errors. */
static int fx_output(void)
{
    static const char *const argv[] = {"big", "1048576", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    DWORD err;
    size_t i;
    int r = 0;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    if (cap.out.len != 1048576 || c->out.bytes != 1048576)
        r = failed("%lu bytes of output, the adapter counted %llu, not 1048576",
                   (unsigned long)cap.out.len, (unsigned long long)c->out.bytes);
    for (i = 0; r == 0 && i < cap.out.len; i++) {
        if (cap.out.data[i] != (char)('a' + i % 23))
            r = failed("byte %lu of the output is wrong", (unsigned long)i);
    }
    if (r == 0 && (cap.err.len != 9 || memcmp(cap.err.data, "big done\n", 9) != 0))
        r = failed("the error stream is not the one line written to it");
    if (r == 0 && (c->out.error != 0 || c->err.error != 0))
        r = failed("a read failed: %s", errtext(c->out.error ? c->out.error : c->err.error));
    cap_free(&cap);
    return done(c, r);
}

/** Write TEXT to the file PATH. */
static int write_file(const char *path, const char *text)
{
    wchar_t *w = etude_win_long_path(path);
    HANDLE h;
    DWORD n;
    BOOL ok;

    if (w == NULL)
        return -1;
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    ok = WriteFile(h, text, (DWORD)strlen(text), &n, NULL) && n == strlen(text);
    CloseHandle(h);
    return ok ? 0 : -1;
}

/** Remove the file PATH. */
static void remove_file(const char *path)
{
    wchar_t *w = etude_win_long_path(path);

    if (w != NULL)
        DeleteFileW(w);
    free(w);
}

/** Standard input comes from the file named, or is empty. */
static int fx_stdin(void)
{
    struct etude_win_spawn sp;
    static const char *const argv[] = {"cat", NULL};
    static const char text[] = "line one\nline two\n";
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    char path[sizeof scratch + 32];
    DWORD err;
    int r;

    snprintf(path, sizeof path, "%s\\stdin.txt", scratch);
    if (write_file(path, text) != 0)
        return failed("cannot write %s", path);
    sp = spawn_of(target, argv, &cap);
    sp.stdin_path = path;
    err = etude_win_start(&sup, &sp, &c);
    if (err == 0)
        err = wait_end(c);
    remove_file(path);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = cap.out.len != strlen(text) || memcmp(cap.out.data, text, cap.out.len) != 0
            ? failed("the child read other than the file")
            : 0;
    cap_free(&cap);
    r = done(c, r);
    if (r != 0)
        return r;
    c = NULL;
    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = cap.out.len != 0 ? failed("the NUL device gave %lu bytes", (unsigned long)cap.out.len) : 0;
    cap_free(&cap);
    return done(c, r);
}

/** The child is suspended in its job when registered, and runs in it. */
static int fx_suspended_start(void)
{
    static const char *const argv[] = {"injob", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = !cap.registered ? failed("the registration function was not called")
        : !cap.checked  ? failed("%s", cap.why)
        : cap.out.data == NULL || strcmp(cap.out.data, "injob 1\n") != 0
            ? failed("the child said %s", cap.out.data ? cap.out.data : "nothing")
        : c->status != 0 ? failed("status %lu", (unsigned long)c->status)
                         : 0;
    cap_free(&cap);
    return done(c, r);
}

/** A child whose registration fails never runs. */
static int fx_refused(void)
{
    static const char *const argv[] = {"args", "never", NULL};
    struct etude_win_child *c = NULL;
    struct etude_win_usage u;
    struct cap cap = {0};
    DWORD err;
    int r;

    cap.refuse = 1;
    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    err = etude_win_usage(c, &u);
    r = err != 0       ? failed("usage: %s", errtext(err))
        : !cap.checked ? failed("%s", cap.why)
        : c->status != ETUDE_WIN_REFUSED_STATUS || !c->stopped
            ? failed("status %#lx, stopped %d", (unsigned long)c->status, c->stopped)
        : cap.out.len != 0 ? failed("the refused child wrote %s", cap.out.data)
        : u.processes != 1 ? failed("%lu processes entered the job", (unsigned long)u.processes)
                           : 0;
    cap_free(&cap);
    return done(c, r);
}

/** A process of the job cannot start a process outside it. */
static int fx_breakaway(void)
{
    static const char *const argv[] = {"breakaway", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = cap.out.data == NULL || strcmp(cap.out.data, "breakaway denied 5\n") != 0
            ? failed("the child said %s", cap.out.data ? cap.out.data : "nothing")
            : 0;
    cap_free(&cap);
    return done(c, r);
}

/** The job's processor time covers every process of the tree, ended or not. */
static int fx_accounting(void)
{
    static const char *const argv[] = {"tree", "3", "200", NULL};
    struct etude_win_child *c = NULL;
    struct etude_win_usage u;
    struct cap cap = {0};
    unsigned long long burned;
    int burners;
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    burners = count_lines(cap.out.data, "burned ", &burned);
    err = etude_win_usage(c, &u);
    r = err != 0 ? failed("usage: %s", errtext(err))
        : burners != 4 || count_lines(cap.out.data, "child ", NULL) != 3
            ? failed("the tree reported %d of 4 processes", burners)
        : u.user_us + u.kernel_us < burned
            ? failed("the job counted %llu us of processor time, its processes %llu",
                     (unsigned long long)(u.user_us + u.kernel_us), burned)
        : burned < 4 * 200000u ? failed("the processes burned %llu us, not 800000", burned)
        : u.processes < 4 || u.active != 0
            ? failed("%lu processes entered the job, %lu are active", (unsigned long)u.processes,
                     (unsigned long)u.active)
            : 0;
    cap_free(&cap);
    return done(c, r);
}

/** The job's peak is of the memory its processes held at once. */
static int fx_memory(void)
{
    static const char *const argv[] = {"memory", "64", "32", NULL};
    struct etude_win_child *c = NULL;
    struct etude_win_usage u;
    struct cap cap = {0};
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    err = etude_win_usage(c, &u);
    r = err != 0         ? failed("usage: %s", errtext(err))
        : c->status != 0 ? failed("status %lu", (unsigned long)c->status)
        : u.peak_job_commit < 96ull << 20 || u.peak_process_commit < 64ull << 20
            ? failed("peaks of %llu bytes for the job and %llu for one process",
                     (unsigned long long)u.peak_job_commit,
                     (unsigned long long)u.peak_process_commit)
            : 0;
    cap_free(&cap);
    return done(c, r);
}

/** A child has not ended while a process of its tree still writes. */
static int fx_late_output(void)
{
    static const char *const argv[] = {"late", "late-text", NULL};
    struct etude_win_child *c = NULL;
    struct etude_win_usage u;
    struct cap cap = {0};
    DWORD err;
    int r;

    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    err = etude_win_usage(c, &u);
    r = err != 0        ? failed("usage: %s", errtext(err))
        : u.active != 0 ? failed("ended with %lu processes in the job", (unsigned long)u.active)
        : cap.out.data == NULL || strstr(cap.out.data, "late-text\n") == NULL
            ? failed("the late output is missing")
        : c->status != 0 ? failed("status %lu", (unsigned long)c->status)
                         : 0;
    cap_free(&cap);
    return done(c, r);
}

/**
 * Open every process of LIST for waiting, checking that each is still the
 * process listed; return how many were opened.
 */
static int open_tree(const struct etude_win_ident *list, size_t n, HANDLE *h)
{
    FILETIME ft[4];
    size_t i;
    int k = 0;

    for (i = 0; i < n; i++) {
        h[k] = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, list[i].pid);
        if (h[k] == NULL)
            continue;
        if (!GetProcessTimes(h[k], &ft[0], &ft[1], &ft[2], &ft[3]) ||
            ((uint64_t)ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime) != list[i].created) {
            CloseHandle(h[k]);
            continue;
        }
        k++;
    }
    return k;
}

/** Whether every one of the N processes of H ended within MS milliseconds. */
static int tree_ended(HANDLE *h, int n, DWORD ms)
{
    DWORD r = WaitForMultipleObjects((DWORD)n, h, TRUE, ms);

    return r < WAIT_OBJECT_0 + (DWORD)n;
}

/** Close the N handles of H. */
static void close_all(HANDLE *h, int n)
{
    int i;

    for (i = 0; i < n; i++)
        CloseHandle(h[i]);
}

/** Stopping a child ends every process of its tree. */
static int fx_stop(void)
{
    static const char *const argv[] = {"hangtree", "3", NULL};
    struct etude_win_ident list[MAX_PIDS];
    HANDLE h[MAX_PIDS];
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    DWORD status;
    size_t n;
    DWORD err;
    int k;
    int i;
    int r = 0;

    err = start(target, argv, &cap, &c);
    if (err == 0)
        err = wait_text(&cap, "ready\n");
    if (err != 0)
        return done(c, failed("start: %s", errtext(err)));
    err = etude_win_pids(c, list, MAX_PIDS, &n);
    if (err != 0)
        return done(c, failed("pids: %s", errtext(err)));
    k = open_tree(list, n, h);
    if (k < 4)
        return close_all(h, k), done(c, failed("%d processes in the tree, not 4", k));
    err = etude_win_stop(c);
    if (err == 0)
        err = wait_end(c);
    if (err != 0)
        r = failed("stop: %s", errtext(err));
    else if (!tree_ended(h, k, BOUND))
        r = failed("a process of the tree outlived its job's end");
    else if (!c->stopped || c->status != ETUDE_WIN_STOP_STATUS)
        r = failed("status %#lx, stopped %d", (unsigned long)c->status, c->stopped);
    for (i = 0; r == 0 && i < k; i++) {
        if (GetExitCodeProcess(h[i], &status) && status != ETUDE_WIN_STOP_STATUS)
            r = failed("a process of the tree ended with %#lx", (unsigned long)status);
    }
    close_all(h, k);
    cap_free(&cap);
    return done(c, r);
}

/**
 * The controller of the controller-death fixture: start a tree in a job of
 * its own, print each of its processes, and wait to be killed.
 */
static int controller(void)
{
    static const char *const argv[] = {"hangtree", "2", NULL};
    struct etude_win_ident list[MAX_PIDS];
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    size_t n;
    size_t i;

    if (etude_win_sup_open(&sup) != 0 || start(target, argv, &cap, &c) != 0 ||
        wait_text(&cap, "ready\n") != 0 || etude_win_pids(c, list, MAX_PIDS, &n) != 0)
        return 3;
    for (i = 0; i < n; i++)
        printf("tree %lu %llu\n", (unsigned long)list[i].pid, (unsigned long long)list[i].created);
    printf("controlled\n");
    fflush(stdout);
    Sleep(INFINITE);
    return 0;
}

/** When the controller dies, the tree in its job dies with it. */
static int fx_controller_death(void)
{
    static const char *const argv[] = {"controller", NULL};
    struct etude_win_ident list[MAX_PIDS];
    HANDLE h[MAX_PIDS];
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    const char *p;
    size_t n = 0;
    DWORD err;
    int k;
    int r = 0;

    err = start(fixture_path, argv, &cap, &c);
    if (err == 0)
        err = wait_text(&cap, "controlled\n");
    if (err != 0)
        return done(c, failed("start: %s", errtext(err)));
    for (p = cap.out.data; p != NULL && n < MAX_PIDS; p = strchr(p, '\n')) {
        if (*p == '\n')
            p++;
        if (sscanf(p, "tree %lu %llu", (unsigned long *)&list[n].pid,
                   (unsigned long long *)&list[n].created) == 2)
            n++;
    }
    k = open_tree(list, n, h);
    if (k < 3)
        return close_all(h, k), done(c, failed("%d processes in the tree, not 3", k));
    if (!TerminateProcess(c->process, 1))
        r = failed("cannot kill the controller: %s", errtext(GetLastError()));
    else if (!tree_ended(h, k, BOUND))
        r = failed("the tree outlived its controller");
    else if ((err = wait_end(c)) != 0)
        r = failed("the controller's job did not end: %s", errtext(err));
    close_all(h, k);
    cap_free(&cap);
    return done(c, r);
}

/** The child inherits its three standard handles and no other. */
static int fx_handles(void)
{
    static const char *const argv[] = {"hang", NULL};
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    HANDLE r_end;
    HANDLE w_end;
    DWORD avail;
    DWORD err;
    int r;

    if (!CreatePipe(&r_end, &w_end, &sa, 0))
        return failed("pipe: %s", errtext(GetLastError()));
    err = start(target, argv, &cap, &c);
    CloseHandle(w_end);
    if (err != 0) {
        CloseHandle(r_end);
        return done(c, failed("start: %s", errtext(err)));
    }
    r = PeekNamedPipe(r_end, NULL, 0, NULL, &avail, NULL)
            ? failed("the child holds an inheritable handle it was not given")
        : GetLastError() != ERROR_BROKEN_PIPE ? failed("peek: %s", errtext(GetLastError()))
                                              : 0;
    CloseHandle(r_end);
    cap_free(&cap);
    return done(c, r);
}

/** Arguments reach the child byte for byte, however they must be quoted. */
static int fx_arguments(void)
{
    static const char *const argv[] = {"args",
                                       "plain",
                                       "two words",
                                       "",
                                       "quote\"in",
                                       "back\\slash",
                                       "trail\\",
                                       "trail space\\",
                                       "\\\\\"",
                                       "tab\there",
                                       "\xc3\xa9 \xd0\xb6 \xe4\xb8\xad",
                                       "a\\\\b\\\"c",
                                       "\"",
                                       " ",
                                       "\\\\server\\share\\",
                                       NULL};
    struct etude_win_child *c = NULL;
    struct buf want = {0};
    struct cap cap = {0};
    char line[256];
    DWORD err;
    int i;
    int r;

    for (i = 1; argv[i] != NULL; i++) {
        snprintf(line, sizeof line, "arg %d %s\n", i - 1, argv[i]);
        append(&want, line, strlen(line));
    }
    snprintf(line, sizeof line, "args %d\n", i - 1);
    append(&want, line, strlen(line));
    err = run(argv, &cap, &c);
    if (err != 0) {
        free(want.data);
        return done(c, failed("run: %s", errtext(err)));
    }
    r = cap.out.data == NULL || strcmp(cap.out.data, want.data) != 0
            ? failed("the child received other arguments:\n%s", cap.out.data ? cap.out.data : "")
            : 0;
    free(want.data);
    cap_free(&cap);
    return done(c, r);
}

/** Make the directory PATH, which may be longer than 260 characters. */
static int make_dir(const char *path)
{
    wchar_t *w = etude_win_long_path(path);
    BOOL ok;

    if (w == NULL)
        return -1;
    ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok ? 0 : -1;
}

/** Remove the empty directory PATH. */
static void remove_dir(const char *path)
{
    wchar_t *w = etude_win_long_path(path);

    if (w != NULL)
        RemoveDirectoryW(w);
    free(w);
}

/** Whether the long form of PATH is WANT. */
static int long_is(const char *path, const wchar_t *want)
{
    wchar_t *w = etude_win_long_path(path);
    int same = w != NULL && wcscmp(w, want) == 0;

    free(w);
    return same;
}

/** A path of any length, with spaces and letters beyond ASCII, works. */
static int fx_paths(void)
{
    struct etude_win_spawn sp;
    static const char *const argv[] = {"cwd", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    char dir[sizeof scratch + 64];
    char prog[sizeof dir + 16];
    char want[sizeof dir + 16];
    char *slash;
    char deep[sizeof scratch + 1024];
    char file[sizeof deep + 16];
    wchar_t *wt;
    wchar_t *wp;
    size_t base;
    DWORD err;
    int r;

    CHECK(long_is("\\\\server\\share\\a/b", L"\\\\?\\UNC\\server\\share\\a\\b"),
          "a share's long form is wrong");
    CHECK(long_is("\\\\?\\C:\\x", L"\\\\?\\C:\\x"), "a long path was changed");
    snprintf(dir, sizeof dir, "%s\\etude \xc3\xa9 dir", scratch);
    snprintf(prog, sizeof prog, "%s\\x y.exe", dir);
    CHECK(make_dir(dir) == 0, "cannot make %s", dir);
    wt = etude_win_long_path(target);
    wp = etude_win_long_path(prog);
    if (wt == NULL || wp == NULL || !CopyFileW(wt, wp, FALSE)) {
        free(wt);
        free(wp);
        remove_dir(dir);
        return failed("cannot copy the target to %s", prog);
    }
    free(wt);
    free(wp);
    sp = spawn_of(prog, argv, &cap);
    sp.cwd = dir;
    err = etude_win_start(&sup, &sp, &c);
    if (err == 0)
        err = wait_end(c);
    snprintf(want, sizeof want, "cwd %s\n", dir);
    for (slash = want; *slash != '\0'; slash++)
        if (*slash == '/')
            *slash = '\\';
    r = err != 0 ? failed("run: %s", errtext(err))
        : cap.out.data == NULL || strcmp(cap.out.data, want) != 0
            ? failed("the child said %s", cap.out.data ? cap.out.data : "nothing")
            : 0;
    cap_free(&cap);
    r = done(c, r);
    remove_file(prog);
    remove_dir(dir);
    if (r != 0)
        return r;

    snprintf(deep, sizeof deep, "%s\\deep", scratch);
    base = strlen(deep);
    CHECK(make_dir(deep) == 0, "cannot make %s", deep);
    while (strlen(deep) < 400) {
        strcat(deep, "\\a-directory-name-of-forty-characters-0");
        if (make_dir(deep) != 0)
            break;
    }
    snprintf(file, sizeof file, "%s\\f.txt", deep);
    r = strlen(deep) < 400
            ? failed("cannot make a directory %lu characters long", (unsigned long)strlen(deep))
        : write_file(file, "deep") != 0
            ? failed("cannot write a file %lu characters deep", (unsigned long)strlen(file))
            : 0;
    remove_file(file);
    while (strlen(deep) >= base) {
        remove_dir(deep);
        *strrchr(deep, '\\') = '\0';
    }
    return r;
}

/** An environment given whole replaces the executor's; none inherits it. */
static int fx_environment(void)
{
    struct etude_win_spawn sp;
    static const char *const argv[] = {"env", "ETUDE_A", "ETUDE_B", "ETUDE_INHERITED", NULL};
    static const char *const dup[] = {"ETUDE_X=1", "etude_x=2", NULL};
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    char root[MAX_PATH + 16];
    const char *envp[4];
    DWORD err;
    int r;

    strcpy(root, "SystemRoot=");
    if (GetEnvironmentVariableA("SystemRoot", root + 11, MAX_PATH) == 0)
        return failed("SystemRoot is not set");
    envp[0] = "ETUDE_A=one \xc3\xa9";
    envp[1] = root;
    envp[2] = "etude_b=two";
    envp[3] = NULL;
    SetEnvironmentVariableW(L"ETUDE_INHERITED", L"yes");
    sp = spawn_of(target, argv, &cap);
    sp.envp = envp;
    err = etude_win_start(&sup, &sp, &c);
    if (err == 0)
        err = wait_end(c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = cap.out.data == NULL || strcmp(cap.out.data, "env ETUDE_A=one \xc3\xa9\nenv ETUDE_B=two\n"
                                                     "env ETUDE_INHERITED unset\n") != 0
            ? failed("the child saw %s", cap.out.data ? cap.out.data : "nothing")
            : 0;
    cap_free(&cap);
    r = done(c, r);
    if (r != 0)
        return r;
    c = NULL;
    err = run(argv, &cap, &c);
    if (err != 0)
        return done(c, failed("run: %s", errtext(err)));
    r = cap.out.data == NULL || strstr(cap.out.data, "env ETUDE_INHERITED=yes\n") == NULL
            ? failed("the child did not inherit: %s", cap.out.data ? cap.out.data : "nothing")
            : 0;
    cap_free(&cap);
    r = done(c, r);
    if (r != 0)
        return r;
    c = NULL;
    sp = spawn_of(target, argv, &cap);
    sp.envp = dup;
    err = etude_win_start(&sup, &sp, &c);
    if (err != ERROR_INVALID_PARAMETER)
        return done(c, failed("two names that differ in case gave %s", errtext(err)));
    return 0;
}

/** A child connects to the executor's listener, which it cannot inherit. */
static int fx_sockets(void)
{
    struct etude_win_child *c = NULL;
    struct cap cap = {0};
    const char *argv[4];
    char port_text[16];
    char got[64];
    unsigned short port;
    unsigned short closed;
    SOCKET l;
    SOCKET a;
    SOCKET b;
    DWORD flags;
    DWORD err;
    int n = 0;
    int k;
    int r;

    CHECK(etude_win_net_start() == 0, "Winsock did not start");
    l = etude_win_listen(&port);
    CHECK(l != INVALID_SOCKET, "listen: %d", WSAGetLastError());
    if (!GetHandleInformation((HANDLE)l, &flags) || (flags & HANDLE_FLAG_INHERIT) != 0) {
        closesocket(l);
        return failed("the listener can be inherited");
    }
    snprintf(port_text, sizeof port_text, "%u", port);
    argv[0] = "connect";
    argv[1] = port_text;
    argv[2] = "hello-socket";
    argv[3] = NULL;
    err = start(target, argv, &cap, &c);
    if (err != 0) {
        closesocket(l);
        return done(c, failed("start: %s", errtext(err)));
    }
    a = etude_win_accept(l, BOUND);
    if (a == INVALID_SOCKET) {
        closesocket(l);
        return done(c, failed("accept: %d", WSAGetLastError()));
    }
    while (n < (int)sizeof got - 1 && (k = recv(a, got + n, (int)sizeof got - 1 - n, 0)) > 0)
        n += k;
    got[n] = '\0';
    closesocket(a);
    err = wait_end(c);
    r = err != 0                           ? failed("run: %s", errtext(err))
        : strcmp(got, "hello-socket") != 0 ? failed("the listener read %s", got)
        : c->status != 0                   ? failed("status %lu", (unsigned long)c->status)
                                           : 0;
    cap_free(&cap);
    r = done(c, r);
    if (r != 0) {
        closesocket(l);
        return r;
    }
    b = etude_win_connect(port, BOUND);
    a = b == INVALID_SOCKET ? INVALID_SOCKET : etude_win_accept(l, BOUND);
    r = b == INVALID_SOCKET   ? failed("connect: %d", WSAGetLastError())
        : a == INVALID_SOCKET ? failed("accept: %d", WSAGetLastError())
        : send(b, "x", 1, 0) != 1 || recv(a, got, 1, 0) != 1 || got[0] != 'x'
            ? failed("a byte did not cross the connection")
            : 0;
    if (a != INVALID_SOCKET)
        closesocket(a);
    if (b != INVALID_SOCKET)
        closesocket(b);
    closesocket(l);
    if (r != 0)
        return r;
    l = etude_win_listen(&closed);
    CHECK(l != INVALID_SOCKET, "listen: %d", WSAGetLastError());
    closesocket(l);
    b = etude_win_connect(closed, BOUND);
    if (b != INVALID_SOCKET) {
        closesocket(b);
        return failed("a connection to a closed port succeeded");
    }
    CHECK(WSAGetLastError() == WSAECONNREFUSED, "a closed port gave %d", WSAGetLastError());
    return 0;
}

/**
 * The QEMU launcher's posted receive keeps what a child sent before it exited
 * with the connection open, which resets it, although the launcher reads only
 * after the exit, and then reads the connection's end. The child exits only
 * after a byte that the fixture sends once its receive is posted.
 */
static int fx_reset(void)
{
    static const char text[] = "{\"return\": {}, \"id\": 100}";
    struct etude_win_child *c = NULL;
    struct socket_in *in;
    struct cap cap = {0};
    const char *argv[4];
    char port_text[16], got[64];
    ULONGLONG began;
    unsigned short port;
    SOCKET l, a;
    DWORD err;
    int n = 0, k, ended = 0, unreleased = 0, r;

    CHECK(etude_win_net_start() == 0, "Winsock did not start");
    in = malloc(sizeof(*in));
    CHECK(in != NULL, "no memory");
    l = etude_win_listen(&port);
    if (l == INVALID_SOCKET) {
        free(in);
        return failed("listen: %d", WSAGetLastError());
    }
    snprintf(port_text, sizeof port_text, "%u", port);
    argv[0] = "reset";
    argv[1] = port_text;
    argv[2] = text;
    argv[3] = NULL;
    err = start(target, argv, &cap, &c);
    a = err != 0 ? INVALID_SOCKET : etude_win_accept(l, BOUND);
    closesocket(l);
    if (a == INVALID_SOCKET) {
        free(in);
        cap_free(&cap);
        return done(c, err != 0 ? failed("start: %s", errtext(err))
                                : failed("accept: %d", WSAGetLastError()));
    }
    socket_in_start(in, (etude_socket)a);
    if (send(a, "r", 1, 0) != 1)
        unreleased = WSAGetLastError();
    err = unreleased != 0 ? 0 : wait_end(c);
    began = GetTickCount64();
    while (!unreleased && n < (int)sizeof got - 1 && GetTickCount64() - began <= BOUND) {
        k = socket_read(in, (etude_socket)a, got + n, sizeof got - 1 - (size_t)n);
        if (k > 0) {
            n += k;
        } else if (k == 0 || errno != EAGAIN) {
            ended = 1;
            break;
        } else {
            Sleep(1);
        }
    }
    got[n] = '\0';
    socket_in_stop(in, (etude_socket)a);
    closesocket(a);
    free(in);
    r = unreleased != 0          ? failed("release: %d", unreleased)
        : err != 0               ? failed("run: %s", errtext(err))
        : c->status != 0         ? failed("status %lu", (unsigned long)c->status)
        : strcmp(got, text) != 0 ? failed("the launcher read '%s' of the reset connection", got)
        : !ended                 ? failed("the reset connection did not end")
                                 : 0;
    cap_free(&cap);
    return done(c, r);
}

/** The monotonic clock does not go back, and UTC agrees with the C library. */
static int fx_clocks(void)
{
    uint64_t prev;
    uint64_t now;
    int64_t utc;
    time_t t;
    long i;

    prev = etude_win_mono_us();
    for (i = 0; i < 100000; i++) {
        now = etude_win_mono_us();
        CHECK(now >= prev, "the monotonic clock went from %llu to %llu", (unsigned long long)prev,
              (unsigned long long)now);
        prev = now;
    }
    CHECK(etude_win_mono_resolution_ns() >= 1 && etude_win_mono_resolution_ns() <= 1000000,
          "a resolution of %llu ns", (unsigned long long)etude_win_mono_resolution_ns());
    t = time(NULL);
    utc = etude_win_utc_us();
    CHECK(utc / 1000000 - (int64_t)t <= 2 && (int64_t)t - utc / 1000000 <= 2,
          "UTC is %lld us, time() %lld s", (long long)utc, (long long)t);
    return 0;
}

/** Children run at once keep their own output and status. */
static int fx_concurrent(void)
{
    enum { N = 8 };
    struct etude_win_child *c[N] = {NULL};
    struct cap cap[N];
    char text[N][16];
    char want[32];
    const char *argv[N][3];
    DWORD err = 0;
    int i;
    int r = 0;
    ULONGLONG t0 = GetTickCount64();
    int left;

    memset(cap, 0, sizeof cap);
    for (i = 0; i < N && err == 0; i++) {
        snprintf(text[i], sizeof text[i], i % 2 ? "%d" : "id-%d", i);
        argv[i][0] = i % 2 ? "exit" : "args";
        argv[i][1] = text[i];
        argv[i][2] = NULL;
        err = start(target, argv[i], &cap[i], &c[i]);
    }
    for (left = N; err == 0 && left > 0;) {
        for (left = 0, i = 0; i < N; i++)
            left += !c[i]->ended;
        if (left > 0 && GetTickCount64() - t0 > BOUND)
            err = ERROR_TIMEOUT;
        else if (left > 0 && etude_win_poll(&sup, 1000) < 0)
            err = GetLastError();
    }
    if (err != 0)
        r = failed("run: %s", errtext(err));
    for (i = 0; r == 0 && i < N; i++) {
        snprintf(want, sizeof want, "arg 0 id-%d\nargs 1\n", i);
        if (i % 2 ? c[i]->status != (DWORD)i || cap[i].out.len != 0
                  : c[i]->status != 0 || cap[i].out.data == NULL ||
                        strcmp(cap[i].out.data, want) != 0)
            r = failed("child %d ended with %lu and %s", i, (unsigned long)c[i]->status,
                       cap[i].out.data ? cap[i].out.data : "no output");
    }
    for (i = 0; i < N; i++) {
        cap_free(&cap[i]);
        if (c[i] != NULL)
            r = done(c[i], r);
    }
    return r;
}

/* The fixtures, in the order they run. */
static const struct {
    const char *name;
    int (*run)(void);
} fixtures[] = {
    {"exit-status", fx_exit_status},
    {"crash", fx_crash},
    {"output", fx_output},
    {"stdin", fx_stdin},
    {"suspended-start", fx_suspended_start},
    {"refused", fx_refused},
    {"breakaway", fx_breakaway},
    {"accounting", fx_accounting},
    {"memory", fx_memory},
    {"late-output", fx_late_output},
    {"stop", fx_stop},
    {"controller-death", fx_controller_death},
    {"handles", fx_handles},
    {"arguments", fx_arguments},
    {"paths", fx_paths},
    {"environment", fx_environment},
    {"sockets", fx_sockets},
    {"reset", fx_reset},
    {"clocks", fx_clocks},
    {"concurrent", fx_concurrent},
    {"exec-attempts", fx_exec_attempts},
    {"exec-unrun", fx_exec_unrun},
    {"exec-sample", fx_exec_sample},
    {"exec-poll", fx_exec_poll},
    {"exec-late", fx_exec_late},
    {"exec-sinks", fx_exec_sinks},
    {"collection-files", fx_collection_files},
    {"collection-qemu", fx_collection_qemu},
    {"ledger", fx_ledger},
    {"runtime-crash", fx_runtime_crash},
    {"runtime-hang", fx_runtime_hang},
    {"runtime-orphans", fx_runtime_orphans},
    {"runtime-killed", fx_runtime_killed},
    {"runtime-cut", fx_runtime_cut},
    {"tooling-made", fx_tooling_made},
    {"tooling-digest", fx_tooling_digest},
    {"tooling-provision", fx_tooling_provision},
    {"tooling-job", fx_tooling_job},
    {"tooling-status", fx_tooling_status},
    {"tooling-unmade", fx_tooling_unmade},
    {"tooling-late", fx_tooling_late},
    {"tooling-crash", fx_tooling_crash},
    {"tooling-lie", fx_tooling_lie},
    {"tooling-line", fx_tooling_line},
    {"tooling-sample", fx_tooling_sample},
    {"hold-run", fx_hold_run},
    {"hold-busy", fx_hold_busy},
    {"hold-left", fx_hold_left},
    {"hold-ancestor", fx_hold_ancestor},
    {"hold-killed", fx_hold_killed},
    {"hold-unicode", fx_hold_unicode},
    {"ports", fx_ports},
    {"admit-locks", fx_admit_locks},
};

/** Find this program's path and directory, and the target's path beside it. */
static int locate(void)
{
    wchar_t w[MAX_PATH * 2];
    char *u;
    char *slash;
    DWORD n;

    n = GetModuleFileNameW(NULL, w, sizeof w / sizeof w[0]);
    if (n == 0 || n >= sizeof w / sizeof w[0])
        return -1;
    u = etude_win_utf8(w);
    if (u == NULL || strlen(u) >= sizeof fixture_path) {
        free(u);
        return -1;
    }
    strcpy(fixture_path, u);
    free(u);
    strcpy(binary_dir, fixture_path);
    slash = strrchr(binary_dir, '\\');
    if (slash == NULL)
        return -1;
    *slash = '\0';
    strcpy(scratch, binary_dir);
    if (getenv("TMPDIR") != NULL && getenv("TMPDIR")[0] != '\0') {
        if (strlen(getenv("TMPDIR")) >= sizeof scratch)
            return -1;
        strcpy(scratch, getenv("TMPDIR"));
    }
    snprintf(target, sizeof target, "%s\\wintarget.exe", binary_dir);
    return 0;
}

int main(int argc, char **argv)
{
    size_t i;
    int k;
    int ran = 0;
    int bad = 0;
    int want;

    _setmode(_fileno(stdout), _O_BINARY);
    if (locate() != 0) {
        fprintf(stderr, "winfixture: cannot find this program's directory\n");
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "controller") == 0)
        return controller();
    if (argc == 4 && strcmp(argv[1], "runtime-controller") == 0) {
        if (runtime_controller(argv[2], argv[3]) == 0)
            return 0;
        fprintf(stderr, "winfixture: controller: %s\n", detail);
        return 1;
    }
    if (etude_win_sup_open(&sup) != 0) {
        fprintf(stderr, "winfixture: cannot open a completion port\n");
        return 2;
    }
    for (k = 1; k < argc; k++) {
        for (i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
            if (strcmp(argv[k], fixtures[i].name) == 0)
                break;
        }
        if (i == sizeof fixtures / sizeof fixtures[0]) {
            fprintf(stderr, "winfixture: no fixture %s\n", argv[k]);
            return 2;
        }
    }
    for (i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        want = argc == 1;
        for (k = 1; k < argc; k++)
            want |= strcmp(argv[k], fixtures[i].name) == 0;
        if (!want)
            continue;
        current = fixtures[i].name;
        detail[0] = '\0';
        ran++;
        if (fixtures[i].run() == 0) {
            printf("winfixture: result: pass: %s\n", current);
        } else {
            bad++;
            printf("winfixture: result: fail: %s: %s\n", current, detail);
        }
        fflush(stdout);
    }
    if (etude_win_sup_close(&sup, BOUND) != 0) {
        printf("winfixture: result: fail: a child did not end at the close\n");
        bad++;
    }
    if (bad != 0) {
        printf("winfixture: result: fail: %d of %d fixtures\n", bad, ran);
        return 1;
    }
    printf("winfixture: result: pass: %d fixtures\n", ran);
    return 0;
}
