/*
 * winholdfix.c: the fixtures of `etude lock tree` on Windows. Each runs
 * etude.exe, beside winfixture.exe, on a tree under scratch\hold with
 * wintarget.exe as its command, and judges the helper's status and
 * messages, the lock's directory and owner record, and the command's
 * processes.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winexec.h"
#include "winfixture.h"
#include "winfmt.h"
#include "files.h"

/* The longest path a fixture makes. */
#define PATH 2048

/* The most processes a fixture starts or opens. */
#define LIVE 16

/* How long a command may outlive the helper killed under it, in ms. */
#define KILLED_BOUND 10000

/* The most arguments the fixtures give `etude lock tree`. */
#define ARGS 16

/* The fixture's directory, the tree and its lock, and the command's files. */
static char base[PATH], tree[PATH], build[PATH], lock[PATH], owner[PATH], guard[PATH], helper[PATH],
    ready[PATH], go[PATH];

/* The environment the fixture changes, as it was, from malloc or NULL. */
static char *old_path, *old_held;

/* The processes the fixture started or opened, ended when it ends. */
static HANDLE live[LIVE];
static int nlive;

/* A process the fixture started, with the files of its output. */
struct run {
    HANDLE process;
    DWORD pid;
    char out[PATH];
    char err[PATH];
};

/* What a command under the hold wrote to the file `ready`. */
struct gate {
    unsigned long pid;   /* the command's process */
    unsigned long child; /* the copy it started, which hangs */
    char held[64];       /* its RACH2_TREE_HELD, or `unset` */
    HANDLE process[2];   /* the command and its copy, open to wait for */
};

/** Give `dir\name` in a buffer of the caller's, or "" when it is too long. */
static char *join(char *buf, const char *dir, const char *name)
{
    int n = snprintf(buf, PATH, "%s\\%s", dir, name);

    if (n < 0 || n >= PATH)
        buf[0] = '\0';
    return buf;
}

/** Say whether `path` names a file or a directory. */
static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/** Remove `path`, a file or a tree, read-only files among them. */
static void remove_tree(const char *path)
{
    char child[PATH];
    struct stat st;
    DIR *dir;
    struct dirent *entry;

    if (lstat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    dir = opendir(path);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
                remove_tree(join(child, path, entry->d_name));
        }
        closedir(dir);
    }
    rmdir(path);
}

/** Write `text` to `path`; 0 on success. */
static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    size_t n = strlen(text);

    if (f == NULL)
        return -1;
    if (fwrite(text, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) != 0 ? -1 : 0;
}

/**
 * Give the contents of `path` without carriage returns and with a NUL after
 * them, or NULL. The helper writes its record in text mode.
 */
static char *get(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *d = NULL;
    size_t n = 0, i, j;
    int c;

    if (f == NULL)
        return NULL;
    while ((c = getc(f)) != EOF) {
        if ((n & 1023) == 0) {
            char *more = realloc(d, n + 1025);

            if (more == NULL) {
                free(d);
                fclose(f);
                return NULL;
            }
            d = more;
        }
        d[n++] = (char)c;
    }
    fclose(f);
    if (d == NULL && (d = malloc(1)) == NULL)
        return NULL;
    for (i = j = 0; i < n; i++)
        if (d[i] != '\r')
            d[j++] = d[i];
    d[j] = '\0';
    return d;
}

/** Say whether the file `path` contains `text`. */
static int contains(const char *path, const char *text)
{
    char *d = get(path);
    int in = d != NULL && strstr(d, text) != NULL;

    free(d);
    return in;
}

/** Say whether the file `path` has the line `line`. */
static int has_line(const char *path, const char *line)
{
    char *d = get(path), *p;
    size_t n = strlen(line);
    int found = 0;

    for (p = d; p != NULL && !found && (p = strstr(p, line)) != NULL; p++)
        found = (p == d || p[-1] == '\n') && (p[n] == '\n' || p[n] == '\0');
    free(d);
    return found;
}

/** Say whether the file `path` has exactly `text`. */
static int has(const char *path, const char *text)
{
    char *d = get(path);
    int same = d != NULL && strcmp(d, text) == 0;

    free(d);
    return same;
}

/** Keep `h` to be ended and closed when the fixture ends. */
static HANDLE keep(HANDLE h)
{
    if (h != NULL && nlive < LIVE)
        live[nlive++] = h;
    return h;
}

/** Give the creation time of process `h` as one count of 100 ns units. */
static unsigned long long created(HANDLE h)
{
    FILETIME ft[4];

    if (!GetProcessTimes(h, &ft[0], &ft[1], &ft[2], &ft[3]))
        return 0;
    return (unsigned long long)ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime;
}

/** Open a file of the fixture for a child to inherit as a standard stream. */
static HANDLE stream(const char *path, int out)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    wchar_t *wide = strcmp(path, "NUL") == 0 ? etude_win_wide(path) : etude_win_long_path(path);
    HANDLE h = wide != NULL
                   ? CreateFileW(wide, out ? GENERIC_WRITE : GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                 out ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                   : INVALID_HANDLE_VALUE;
    free(wide);
    return h;
}

/**
 * Start `program` with the arguments `argv`, which end in NULL, in this
 * process's console and environment, with no job, its input the NUL device
 * and its output and errors the files base\TAG.out and base\TAG.err, which
 * are the only handles it inherits.
 *
 * @return 0, or 1 when it did not start.
 */
static int start(struct run *r, const char *tag, const char *program, const char *const *argv)
{
    char name[64], *text = NULL;
    wchar_t *prog = NULL, *line = NULL;
    HANDLE std[3];
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T size = 0;
    int i, ok = 0;

    memset(r, 0, sizeof *r);
    snprintf(name, sizeof name, "%s.out", tag);
    join(r->out, base, name);
    snprintf(name, sizeof name, "%s.err", tag);
    join(r->err, base, name);
    std[0] = stream("NUL", 0);
    std[1] = stream(r->out, 1);
    std[2] = stream(r->err, 1);
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = std[0];
    si.StartupInfo.hStdOutput = std[1];
    si.StartupInfo.hStdError = std[2];
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    si.lpAttributeList = malloc(size);
    if (std[0] != INVALID_HANDLE_VALUE && std[1] != INVALID_HANDLE_VALUE &&
        std[2] != INVALID_HANDLE_VALUE && si.lpAttributeList != NULL &&
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) {
        ok = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       std, sizeof std, NULL, NULL) &&
             etude_win_cmdline(program, argv, &text) == 0 &&
             (prog = etude_win_wide(program)) != NULL && (line = etude_win_wide(text)) != NULL &&
             CreateProcessW(prog, line, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                            &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    free(si.lpAttributeList);
    for (i = 0; i < 3; i++)
        if (std[i] != INVALID_HANDLE_VALUE)
            CloseHandle(std[i]);
    free(text);
    free(prog);
    free(line);
    if (!ok)
        return failed("%s does not start: error %lu", program, (unsigned long)GetLastError());
    CloseHandle(pi.hThread);
    r->process = keep(pi.hProcess);
    r->pid = pi.dwProcessId;
    return 0;
}

/** Start `etude lock tree` with `argv`, as start() does. */
static int hold(struct run *r, const char *tag, const char *const *argv)
{
    const char *args[ARGS];
    int n = 0;

    args[n++] = "lock";
    args[n++] = "tree";
    while (*argv != NULL && n < ARGS - 1)
        args[n++] = *argv++;
    if (*argv != NULL)
        return failed("more than %d arguments for etude lock tree", ARGS - 3);
    args[n] = NULL;
    return start(r, tag, helper, args);
}

/**
 * Wait up to BOUND for the process of `r` to end, and give its exit status
 * in `*status`.
 *
 * @return 0, or 1 when it did not end.
 */
static int finish(struct run *r, DWORD *status)
{
    if (WaitForSingleObject(r->process, BOUND) != WAIT_OBJECT_0)
        return failed("the process %lu did not end", (unsigned long)r->pid);
    if (!GetExitCodeProcess(r->process, status))
        return failed("no exit status of %lu", (unsigned long)r->pid);
    return 0;
}

/**
 * Wait up to BOUND for the command of the helper `r`, a `wintarget gate`, to
 * write the file `ready`, and read it into `*g`. A virus scanner may have the
 * file open just after its rename, so a failed read is tried again.
 *
 * @return 0, or 1 when it did not, with the helper's errors.
 */
static int gated(struct run *r, struct gate *g)
{
    ULONGLONG t0 = GetTickCount64();
    char *d;
    int n;

    for (;;) {
        d = exists(ready) ? get(ready) : NULL;
        n = d != NULL && sscanf(d, "%lu %lu %63s", &g->pid, &g->child, g->held) == 3;
        free(d);
        if (n)
            break;
        if (WaitForSingleObject(r->process, 10) == WAIT_OBJECT_0) {
            d = get(r->err);
            n = failed("the helper ended before its command ran: %s", d != NULL ? d : "");
            free(d);
            return n;
        }
        if (GetTickCount64() - t0 > BOUND)
            return failed(exists(ready) ? "the command's report cannot be read"
                                        : "the command did not run");
    }
    g->process[0] = keep(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, g->pid));
    g->process[1] = keep(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, g->child));
    if (g->process[0] == NULL || g->process[1] == NULL)
        return failed("the command's processes cannot be opened");
    return 0;
}

/** Let a `wintarget gate` command end. */
static int open_gate(void)
{
    return put(go, "go\n") != 0 ? failed("%s cannot be written", go) : 0;
}

/**
 * Write the owner record of process `h` to the lock's directory, with the
 * line `held script` when `held` is set.
 */
static int record(HANDLE h, int held)
{
    char text[512];
    unsigned long pid = GetProcessId(h);

    snprintf(text, sizeof text,
             "pid %lu\npgid 0\nidentity %lu 0 %llu 0\njob 0\nsince 2026-10-23 00:00:00\n"
             "args fixture\n%s",
             pid, pid, created(h), held ? "held script\n" : "");
    if (!exists(build) && mkdir(build, 0700) != 0)
        return failed("%s cannot be made", build);
    if (!exists(lock) && mkdir(lock, 0700) != 0)
        return failed("%s cannot be made", lock);
    return put(owner, text) != 0 ? failed("%s cannot be written", owner) : 0;
}

/** Say whether the helper's errors in `r` contain `text`. */
static int said(const struct run *r, const char *text)
{
    return contains(r->err, text);
}

/** Give the helper's errors in `r`, from a static buffer, for a failure. */
static const char *errors(const struct run *r)
{
    static char buf[1024];
    char *d = get(r->err);

    snprintf(buf, sizeof buf, "%s", d != NULL ? d : "(none)");
    free(d);
    return buf;
}

/**
 * Run `etude lock tree` with `argv` to its end; its status, or -1 on a
 * failure.
 */
static long run_hold(struct run *r, const char *tag, const char *const *argv)
{
    DWORD status;

    if (hold(r, tag, argv) != 0 || finish(r, &status) != 0)
        return -1;
    return (long)status;
}

/** Set the variable `name` of this process's environment, or unset it. */
static void setvar(const char *name, const char *value)
{
    SetEnvironmentVariableA(name, value);
}

/** Give the variable `name` from malloc, or NULL when it is unset. */
static char *getvar(const char *name)
{
    DWORD n = GetEnvironmentVariableA(name, NULL, 0);
    char *v;

    if (n == 0 || (v = malloc(n)) == NULL)
        return NULL;
    if (GetEnvironmentVariableA(name, v, n) >= n) {
        free(v);
        return NULL;
    }
    return v;
}

/**
 * Make the fixture's directory afresh, put wintarget.exe's directory first
 * on the path, so that a command named `wintarget` is found there, and unset
 * RACH2_TREE_HELD.
 */
static int fresh(void)
{
    char *path;

    join(base, scratch, "hold");
    remove_tree(base);
    join(tree, base, "tree");
    join(build, tree, "build");
    join(lock, build, ".run.lock");
    join(owner, lock, "owner");
    join(guard, build, ".run.guard");
    join(helper, binary_dir, "etude.exe");
    join(ready, base, "ready");
    join(go, base, "go");
    nlive = 0;
    old_path = getvar("PATH");
    old_held = getvar("RACH2_TREE_HELD");
    path = malloc(strlen(binary_dir) + (old_path != NULL ? strlen(old_path) : 0) + 2);
    if (path == NULL)
        return failed("out of memory");
    sprintf(path, "%s;%s", binary_dir, old_path != NULL ? old_path : "");
    setvar("PATH", path);
    free(path);
    setvar("RACH2_TREE_HELD", NULL);
    if (mkdir(base, 0700) != 0 || mkdir(tree, 0700) != 0)
        return failed("%s cannot be made", tree);
    return 0;
}

/** End every process the fixture kept, restore its environment and remove
 * its files. */
static void clean(void)
{
    int i;

    put(go, "go\n");
    for (i = 0; i < nlive; i++) {
        TerminateProcess(live[i], 1);
        WaitForSingleObject(live[i], BOUND);
        CloseHandle(live[i]);
    }
    nlive = 0;
    setvar("PATH", old_path);
    setvar("RACH2_TREE_HELD", old_held);
    free(old_path);
    free(old_held);
    old_path = old_held = NULL;
    remove_tree(base);
}

/** Run `row` with the fixture's files made afresh, and clean up after it. */
static int fixture(int (*row)(void))
{
    int rc = fresh() != 0 ? 1 : row();

    clean();
    return rc;
}

/**
 * Hold the tree for a `wintarget gate` command, as a run's hold when `run`
 * is set, and judge it while it runs and after: RACH2_TREE_HELD and the
 * record name the helper, the record says `held` only for a script's
 * hold, and once the command has ended its copy has ended too, the lock's
 * directory is gone and the guard's file stays.
 */
static int gate_run(int run)
{
    const char *const script[] = {tree, "fixture", "wintarget", "gate", ready, go, NULL};
    const char *const runs[] = {"-r", tree, "fixture", "wintarget", "gate", ready, go, NULL};
    char line[128];
    struct run r;
    struct gate g;
    DWORD status;

    unlink(ready);
    unlink(go);
    if (hold(&r, run ? "run" : "script", run ? runs : script) != 0 || gated(&r, &g) != 0)
        return 1;
    snprintf(line, sizeof line, "%lu", (unsigned long)r.pid);
    if (strcmp(g.held, line) != 0)
        return failed("the command has RACH2_TREE_HELD %s, not the helper's %s", g.held, line);
    snprintf(line, sizeof line, "pid %lu", (unsigned long)r.pid);
    if (!has_line(owner, line))
        return failed("the owner record has no line %s", line);
    snprintf(line, sizeof line, "identity %lu 0 %llu 0", (unsigned long)r.pid, created(r.process));
    if (!has_line(owner, line))
        return failed("the owner record has no line %s", line);
    if (has_line(owner, "held script") == run)
        return failed("a %s hold's record %s `held`", run ? "run's" : "script's",
                      run ? "says" : "does not say");
    if (open_gate() != 0 || finish(&r, &status) != 0)
        return 1;
    if (status != 0)
        return failed("the helper exits %lu: %s", (unsigned long)status, errors(&r));
    if (WaitForSingleObject(g.process[1], 0) != WAIT_OBJECT_0)
        return failed("the command's copy outlived the hold");
    if (exists(lock))
        return failed("%s was left", lock);
    if (!exists(guard))
        return failed("%s is gone", guard);
    return 0;
}

/** A command's status passes through the hold, which names its holder. */
static int row_run(void)
{
    const char *const argv[] = {tree, "fixture", "wintarget", "exit", "7", NULL};
    struct run r;
    long status = run_hold(&r, "exit", argv);

    if (status < 0)
        return 1;
    if (status != 7)
        return failed("the helper exits %ld, not 7: %s", status, errors(&r));
    if (exists(lock))
        return failed("%s was left", lock);
    if (!exists(guard))
        return failed("%s was not made", guard);
    return gate_run(0) != 0 || gate_run(1) != 0;
}

/** A process outside the holder's job cannot query or reuse its hold. */
static int row_busy(void)
{
    const char *const first[] = {tree, "first", "wintarget", "gate", ready, go, NULL};
    const char *const second[] = {tree, "second", "wintarget", "exit", "0", NULL};
    const char *const query[] = {"-q", tree, NULL};
    char line[64];
    char *before;
    struct run a, b;
    struct gate g;
    DWORD status;
    long st;

    if (hold(&a, "first", first) != 0 || gated(&a, &g) != 0)
        return 1;
    before = get(owner);
    setvar("RACH2_TREE_HELD", g.held);
    st = run_hold(&b, "query-stranger", query);
    if (st != 1 || before == NULL || !has(owner, before)) {
        free(before);
        return failed("an unrelated query exits %ld or changes the owner record", st);
    }
    free(before);
    if ((st = run_hold(&b, "second", second)) < 0)
        return 1;
    if (st != 1 || !said(&b, "second: result: refused: the tree is not free\n"))
        return failed("the second helper exits %ld: %s", st, errors(&b));
    snprintf(line, sizeof line, "pid %lu", (unsigned long)a.pid);
    if (WaitForSingleObject(a.process, 0) != WAIT_TIMEOUT || !has_line(owner, line))
        return failed("the first hold was disturbed");
    if (open_gate() != 0 || finish(&a, &status) != 0)
        return 1;
    if (status != 0 || exists(lock))
        return failed("the first helper exits %lu and leaves the lock %d: %s",
                      (unsigned long)status, exists(lock), errors(&a));
    return 0;
}

/** Set the last write time of the directory `path` an hour back. */
static int age(const char *path)
{
    wchar_t *wide = etude_win_long_path(path);
    HANDLE h = wide != NULL ? CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL)
                            : INVALID_HANDLE_VALUE;
    unsigned long long t;
    FILETIME ft;
    BOOL ok;

    free(wide);
    if (h == INVALID_HANDLE_VALUE)
        return failed("%s cannot be opened", path);
    GetSystemTimeAsFileTime(&ft);
    t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 3600ull * 10000000ull;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok ? 0 : failed("%s cannot be aged", path);
}

/** Say whether a directory moved aside by a takeover is left in the build. */
static int moved_left(void)
{
    char pattern[PATH];
    WIN32_FIND_DATAW fd;
    wchar_t *wide = etude_win_long_path(join(pattern, build, ".run.lock.old.*"));
    HANDLE h = wide != NULL ? FindFirstFileW(wide, &fd) : INVALID_HANDLE_VALUE;

    free(wide);

    if (h == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(h);
    return 1;
}

/** The run's command line of row_left, which `-r` makes a run's hold. */
static const char *const left_run[] = {"-r", tree, "fixture", "wintarget", "exit", "0", NULL};

/**
 * Locks left with no holder that runs: a script's is refused, as is
 * one with no record that a run may be taking now; one with no record that
 * is old is taken over.
 */
static int left_unheld(void)
{
    struct run r;
    long st;

    if (record(GetCurrentProcess(), 1) != 0)
        return 1;
    if ((st = run_hold(&r, "script", left_run)) < 0)
        return 1;
    if (st != 1 || !said(&r, "a script's hold on this tree was left") ||
        !has_line(owner, "held script"))
        return failed("a script's hold left: the helper exits %ld: %s", st, errors(&r));
    if (unlink(owner) != 0)
        return failed("%s cannot be removed", owner);
    if ((st = run_hold(&r, "taking", left_run)) < 0)
        return 1;
    if (st != 1 || !said(&r, "another run is taking this tree") || !exists(lock))
        return failed("a young lock with no record: the helper exits %ld: %s", st, errors(&r));
    if (age(lock) != 0 || (st = run_hold(&r, "norecord", left_run)) < 0)
        return 1;
    if (st != 0 || !said(&r, "taking over the lock a run left with no owner record") ||
        exists(lock) || moved_left())
        return failed("an old lock with no record: the helper exits %ld: %s", st, errors(&r));
    return 0;
}

/**
 * Locks that were left: those of left_unheld; a run's whose holder has
 * ended, which is taken over; and a run's whose holder runs, which is
 * refused.
 */
static int row_left(void)
{
    const char *const exit0[] = {"exit", "0", NULL};
    const char *const hang[] = {"hang", NULL};
    struct run r, t;
    DWORD status;
    long st;

    if (left_unheld() != 0)
        return 1;
    if (start(&t, "ended", target, exit0) != 0 || finish(&t, &status) != 0)
        return 1;
    if (record(t.process, 0) != 0 || (st = run_hold(&r, "ended", left_run)) < 0)
        return 1;
    if (st != 0 || !said(&r, "taking over the lock a run left: ") || exists(lock) || moved_left())
        return failed("a run's lock whose holder ended: the helper exits %ld: %s", st, errors(&r));
    if (start(&t, "hang", target, hang) != 0 || record(t.process, 0) != 0 ||
        (st = run_hold(&r, "runs", left_run)) < 0)
        return 1;
    if (st != 1 || !said(&r, "another run holds this tree: ") || !exists(owner))
        return failed("a run's lock whose holder runs: the helper exits %ld: %s", st, errors(&r));
    return 0;
}

/**
 * The hold of an ancestor: with RACH2_TREE_HELD and the record naming this
 * process, the command runs under it, its status passes through, and the
 * lock and the guard are not touched; with them naming a process that is not
 * an ancestor, the helper is refused.
 */
static int row_ancestor(void)
{
    const char *const exit5[] = {tree, "fixture", "wintarget", "exit", "5", NULL};
    const char *const env[] = {tree, "fixture", "wintarget", "env", "RACH2_TREE_HELD", NULL};
    const char *const hang[] = {"hang", NULL};
    char pid[32], want[64], *before;
    struct run r, t;
    long st;
    int rc = 0;

    if (record(GetCurrentProcess(), 1) != 0)
        return 1;
    before = get(owner);
    snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
    snprintf(want, sizeof want, "env RACH2_TREE_HELD=%s\n", pid);
    setvar("RACH2_TREE_HELD", pid);
    if ((st = run_hold(&r, "exit", exit5)) < 0)
        rc = 1;
    else if (st != 5)
        rc = failed("the helper exits %ld, not 5: %s", st, errors(&r));
    else if (before == NULL || !has(owner, before))
        rc = failed("the owner record was changed");
    else if (exists(guard))
        rc = failed("the helper took the guard");
    else if ((st = run_hold(&r, "env", env)) < 0)
        rc = 1;
    else if (st != 0 || !contains(r.out, want))
        rc = failed("the command does not have RACH2_TREE_HELD %s: %ld", pid, st);
    else if (start(&t, "hang", target, hang) != 0 || record(t.process, 1) != 0)
        rc = 1;
    free(before);
    if (rc != 0)
        return rc;
    before = get(owner);
    snprintf(pid, sizeof pid, "%lu", (unsigned long)t.pid);
    setvar("RACH2_TREE_HELD", pid);
    if ((st = run_hold(&r, "stranger", exit5)) < 0)
        rc = 1;
    else if (st != 1 || !said(&r, "result: refused: the tree is not free"))
        rc = failed("the hold of a process that is not an ancestor: the helper exits %ld: %s", st,
                    errors(&r));
    else if (before == NULL || !has(owner, before))
        rc = failed("the owner record was changed");
    free(before);
    return rc;
}

/** A helper killed under its command takes the command's tree with it, and
 * leaves the lock. */
static int row_killed(void)
{
    const char *const argv[] = {tree, "fixture", "wintarget", "gate", ready, go, NULL};
    struct run r;
    struct gate g;

    if (hold(&r, "killed", argv) != 0 || gated(&r, &g) != 0)
        return 1;
    if (!TerminateProcess(r.process, 9) || WaitForSingleObject(r.process, BOUND) != WAIT_OBJECT_0)
        return failed("the helper cannot be killed");
    if (WaitForMultipleObjects(2, g.process, TRUE, KILLED_BOUND) != WAIT_OBJECT_0)
        return failed("the command's tree outlived the helper");
    if (!exists(owner))
        return failed("the lock of a killed hold is gone");
    return 0;
}

/**
 * Arguments outside the legacy code page reach the helper and its command
 * whole: the helper holds a tree whose name has them, and the command reports
 * the same UTF-8 bytes it was given.
 */
static int row_unicode(void)
{
    static const char name[] = "\xc3\xa9t\xc3\xa9 \xd0\xb6 \xe4\xb8\xad";
    char named[PATH], built[PATH], want[64];
    const char *const argv[] = {named, "fixture", "wintarget", "args", name, NULL};
    struct run r;
    long status;

    if (mkdir(join(named, base, name), 0700) != 0)
        return failed("%s cannot be made", named);
    if ((status = run_hold(&r, "unicode", argv)) < 0)
        return 1;
    if (status != 0)
        return failed("the helper exits %ld: %s", status, errors(&r));
    snprintf(want, sizeof want, "arg 0 %s\nargs 1\n", name);
    if (!has(r.out, want)) {
        char *got = get(r.out);

        status = failed("the command reported other arguments:\n%s", got != NULL ? got : "");
        free(got);
        return (int)status;
    }
    if (!exists(join(built, named, "build\\.run.guard")))
        return failed("%s was not made", built);
    return 0;
}

int fx_hold_run(void)
{
    return fixture(row_run);
}

int fx_hold_busy(void)
{
    return fixture(row_busy);
}

int fx_hold_left(void)
{
    return fixture(row_left);
}

int fx_hold_ancestor(void)
{
    return fixture(row_ancestor);
}

int fx_hold_killed(void)
{
    return fixture(row_killed);
}

int fx_hold_unicode(void)
{
    return fixture(row_unicode);
}
