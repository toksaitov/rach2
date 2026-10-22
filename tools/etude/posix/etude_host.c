/*
 * etude_host.c: etude-host, which runs the host checks of `tests/run host`
 * several at once on the host executor, each with a scratch directory of its
 * own, and prints one result for every check of its selection.
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include "../win/files.h"
#else
#define ETUDE_SHELL "/bin/sh"
#endif

#include "etude/exec.h"
#include "posix.h"

/* The bound of a check's compile, in seconds; a compile declares none. */
#define COMPILE_SECONDS 600

/* The longest bound a selection may give a check, in seconds. */
#define SECONDS_MAX 86400

/* The most checks run at once. */
#define JOBS_MAX 1024

/* The time from the termination signal to the kill. */
#define GRACE_US 2000000u

/* The lines of a failed step's log printed under its result, and the bytes
 * of the log's end read to find them. */
#define TAIL_LINES 20
#define TAIL_BYTES 8192

/* The most lines of sanitizers' reports printed under a failed step. */
#define SANITIZER_LINES 6

/* The sanitizers' settings every check runs with, whatever the caller's
 * environment says, so that no inherited setting lets a report pass. */
static const char *const sanitizer_env[] = {
    "ASAN_OPTIONS=halt_on_error=1:exitcode=1",
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=1",
};

/* The inherited variables a check does not get: its scratch directory, and
 * the sanitizers' settings, which sanitizer_env replaces. */
static const char *const dropped_env[] = {
    "TMPDIR=", "ASAN_OPTIONS=", "UBSAN_OPTIONS=", "LSAN_OPTIONS="};

enum step { STEP_COMPILE, STEP_RUN };

/* One check of the selection. */
struct check {
    char *name;    /* its identity, which names its logs and scratch */
    char *label;   /* what its result line says it checked */
    char *compile; /* the command that builds it, or an empty string */
    char *run;     /* the command whose exit status 0 is its pass */
    unsigned seconds;
    int alone; /* the check runs with no other beside it */
    enum step step;
    struct etude_proc *proc; /* the step running, or NULL */
    uint64_t began_us;       /* when the step started */
    int hung;                /* the step ran past its bound and was ended */
    int stopped;             /* the step was ended by a stop signal */
    enum { WAITING, PASSED, FAILED, STOPPED } state;
};

/* A run of the selection. */
struct run {
    struct etude_exec *ex;
    struct check *checks;
    size_t n;
    const char *logs;    /* the directory of the logs */
    const char *scratch; /* the directory of the checks' scratch directories */
    char **env;          /* the checks' environment, with a slot for TMPDIR */
    size_t env_tmpdir;   /* the index of that slot */
    size_t failed;
    size_t reported;
};

/* The stop signal received, or 0. */
static volatile sig_atomic_t stop_signal;

/** Note a stop signal; the loop then ends the running checks. */
static void note_stop(int sig)
{
    stop_signal = sig;
}

/** Copy a string; NULL when memory ran out. */
static char *copy_text(const char *s)
{
    char *t = malloc(strlen(s) + 1);

    if (t != NULL)
        strcpy(t, s);
    return t;
}

/** Join three strings into a new one; NULL when memory ran out. */
static char *join(const char *a, const char *b, const char *c)
{
    size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
    char *p = malloc(n);

    if (p != NULL)
        snprintf(p, n, "%s%s%s", a, b, c);
    return p;
}

/** Whether a name is letters, digits, `_`, `.` and `-`, and not `.` or `..`. */
static int name_ok(const char *s)
{
    static const char allowed[] = "abcdefghijklmnopqrstuvwxyz"
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-";

    if (*s == '\0' || strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
        return 0;
    return strspn(s, allowed) == strlen(s);
}

/** Split a line at its tabs into exactly `n` fields; 0 on success. */
static int split_fields(char *line, char **field, int n)
{
    int i;

    for (i = 0; i < n - 1; i++) {
        char *tab = strchr(line, '\t');

        if (tab == NULL)
            return -1;
        field[i] = line;
        *tab = '\0';
        line = tab + 1;
    }
    field[n - 1] = line;
    return strchr(line, '\t') == NULL ? 0 : -1;
}

/** Read a whole number from `lo` to `hi`; 0 when the text is not one. */
static unsigned long number_of(const char *s, unsigned long lo, unsigned long hi)
{
    unsigned long v = 0;

    if (*s == '\0' || strlen(s) > 9 || strspn(s, "0123456789") != strlen(s))
        return 0;
    for (; *s != '\0'; s++)
        v = v * 10 + (unsigned long)(*s - '0');
    return v >= lo && v <= hi ? v : 0;
}

/**
 * Take one line of the selection as a check: NAME, SECONDS, MODE, LABEL,
 * COMPILE and RUN, separated by tabs.
 *
 * @return NULL, or the reason the line is refused.
 */
static const char *take_check(struct run *r, char *line)
{
    struct check *c = &r->checks[r->n];
    char *f[6];
    size_t i;

    if (split_fields(line, f, 6) != 0)
        return "a check is six fields separated by tabs";
    if (!name_ok(f[0]))
        return "a name is letters, digits, '_', '.' and '-'";
    for (i = 0; i < r->n; i++) {
        if (strcmp(r->checks[i].name, f[0]) == 0)
            return "a name is given twice";
    }
    c->seconds = (unsigned)number_of(f[1], 1, SECONDS_MAX);
    if (c->seconds == 0)
        return "a bound is a whole number of seconds from 1 to 86400";
    if (strcmp(f[2], "shared") != 0 && strcmp(f[2], "alone") != 0)
        return "a mode is shared or alone";
    c->alone = strcmp(f[2], "alone") == 0;
    if (*f[3] == '\0' || *f[5] == '\0')
        return "a check has a label and a command that runs it";
    c->name = copy_text(f[0]);
    c->label = copy_text(f[3]);
    c->compile = copy_text(f[4]);
    c->run = copy_text(f[5]);
    if (c->name == NULL || c->label == NULL || c->compile == NULL || c->run == NULL)
        return "memory ran out";
    r->n++;
    return NULL;
}

/** Make room for one more check; 0, or -1 when memory ran out. */
static int grow(struct run *r, size_t *max)
{
    struct check *more;

    if (r->n < *max)
        return 0;
    more = realloc(r->checks, (*max * 2 + 64) * sizeof(*more));
    if (more == NULL)
        return -1;
    *max = *max * 2 + 64;
    r->checks = more;
    return 0;
}

/**
 * Read the selection, one check a line. A line that is not a check refuses
 * the whole selection, so that no check is left out unseen.
 *
 * @return 0, or -1 after a diagnostic.
 */
static int read_selection(struct run *r, const char *path)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0, max = 0;
    unsigned long lineno = 0;
    ssize_t len;
    const char *why = NULL;

    if (f == NULL) {
        fprintf(stderr, "etude-host: %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (why == NULL && (len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if (line[len - 1] != '\n') {
            why = "the last line has no newline";
        } else if (grow(r, &max) != 0) {
            why = "memory ran out";
        } else {
            line[len - 1] = '\0';
            memset(&r->checks[r->n], 0, sizeof(r->checks[r->n]));
            why = take_check(r, line);
        }
    }
    if (why == NULL && ferror(f))
        why = "the selection could not be read";
    free(line);
    fclose(f);
    if (why == NULL && r->n == 0)
        why = "the selection has no check";
    if (why != NULL) {
        fprintf(stderr, "etude-host: %s:%lu: %s\n", path, lineno, why);
        return -1;
    }
    return 0;
}

/** Whether `var` is one of the inherited variables a check does not get. */
static int dropped(const char *var)
{
    size_t i;

    for (i = 0; i < sizeof(dropped_env) / sizeof(dropped_env[0]); i++) {
        if (strncmp(var, dropped_env[i], strlen(dropped_env[i])) == 0)
            return 1;
    }
    return 0;
}

/**
 * Make the checks' environment: this process's without the dropped
 * variables, the sanitizers' settings, and a slot for each check's TMPDIR.
 *
 * @return 0, or -1 when memory ran out.
 */
static int make_env(struct run *r)
{
#ifndef _WIN32
    extern char **environ;
#endif
    size_t n = 0, k = 0, i;

    while (environ[n] != NULL)
        n++;
    r->env = calloc(n + 4, sizeof(char *));
    if (r->env == NULL)
        return -1;
    for (i = 0; i < n; i++) {
        if (!dropped(environ[i]))
            r->env[k++] = environ[i];
    }
    for (i = 0; i < sizeof(sanitizer_env) / sizeof(sanitizer_env[0]); i++)
        r->env[k++] = (char *)sanitizer_env[i];
    r->env_tmpdir = k;
    return 0;
}

/**
 * Remove a tree, with directories its check left unwritable; 0 once gone.
 *
 * Each directory is made writable and searchable before it is read, and its
 * entries are removed before it is; a symbolic link is removed, not followed.
 */
static int remove_tree(const char *path)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    int bad = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return remove(path) == 0 || errno == ENOENT ? 0 : -1;
    chmod(path, (st.st_mode & 07777) | S_IRWXU);
    if ((d = opendir(path)) == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char *sub;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if ((sub = join(path, "/", e->d_name)) == NULL || remove_tree(sub) != 0)
            bad = -1;
        free(sub);
    }
    closedir(d);
    return rmdir(path) == 0 || errno == ENOENT ? bad : -1;
}

/** Whether a directory has an entry, or cannot be read. */
static int has_entries(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int any = 0;

    if (d == NULL)
        return errno != ENOENT;
    while (!any && (e = readdir(d)) != NULL)
        any = strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    closedir(d);
    return any;
}

/** Give the path of a check's log of one step; NULL when memory ran out. */
static char *log_of(const struct run *r, const struct check *c, enum step s)
{
    size_t n = strlen(r->logs) + strlen(c->name) + 7;
    char *p = malloc(n);

    if (p != NULL)
        snprintf(p, n, "%s/%s.%s", r->logs, c->name, s == STEP_COMPILE ? "clog" : "rlog");
    return p;
}

/**
 * Start the current step of a check. Its command runs as
 * `sh -c 'exec >LOG 2>&1; eval "$2"'`, so that its output and that of
 * everything it starts go to the step's log in the order written.
 *
 * @return 0, or an error number.
 */
static int start_step(struct run *r, struct check *c)
{
    char *log = log_of(r, c, c->step);
    char *tmpdir = join("TMPDIR=", r->scratch, "/");
    char *tmpvar = tmpdir == NULL ? NULL : join(tmpdir, c->name, "");
    char *argv[] = {(char *)ETUDE_SHELL,
                    "-c",
                    "exec >\"$1\" 2>&1; eval \"$2\"",
                    "etude-host",
                    log,
                    c->step == STEP_COMPILE ? c->compile : c->run,
                    NULL};
    struct etude_exec_spec spec;
    int err = 0;

    if (log == NULL || tmpvar == NULL) {
        err = ENOMEM;
    } else {
        r->env[r->env_tmpdir] = tmpvar;
        memset(&spec, 0, sizeof(spec));
        spec.attempt_id = c->name;
        spec.argv = argv;
        spec.envp = r->env;
        spec.output_max = 4096;
        spec.grace_us = GRACE_US;
        fflush(stdout);
        c->began_us = posix_now_us();
        if (etude_exec_start(r->ex, &spec, &c->proc) != 0)
            err = errno != 0 ? errno : EIO;
        r->env[r->env_tmpdir] = NULL;
    }
    free(log);
    free(tmpdir);
    free(tmpvar);
    return err;
}

/** Whether a line is a check's report of something it could not run:
 * `NAME: not run: WHAT: WHY`. */
static int not_run_line(const char *s)
{
    size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-");

    return n > 0 && strncmp(s + n, ": not run: ", 11) == 0;
}

/** Print each line of a log that says a check was not run, indented. */
static void print_not_run(const char *path)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;

    if (f == NULL)
        return;
    while (getline(&line, &cap, f) >= 0) {
        if (not_run_line(line))
            printf("    %s%s", line, strchr(line, '\n') == NULL ? "\n" : "");
    }
    free(line);
    fclose(f);
}

/** Whether a line begins a sanitizer's report or gives its summary. */
static int sanitizer_line(const char *s)
{
    return strstr(s, "ERROR: AddressSanitizer") != NULL ||
           strstr(s, "ERROR: LeakSanitizer") != NULL || strstr(s, "runtime error: ") != NULL ||
           strncmp(s, "SUMMARY: ", 9) == 0;
}

/** Print the first few lines of a sanitizer's reports in a log, indented,
 * since the end of an AddressSanitizer report is a legend. */
static void print_sanitizer(const char *path)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    int shown = 0;

    if (f == NULL)
        return;
    while (shown < SANITIZER_LINES && getline(&line, &cap, f) >= 0) {
        if (sanitizer_line(line)) {
            printf("    ! %s%s", line, strchr(line, '\n') == NULL ? "\n" : "");
            shown++;
        }
    }
    free(line);
    fclose(f);
}

/** Print the last lines of a log, indented, so a failure reads without it. */
static void print_tail(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[TAIL_BYTES + 1];
    size_t n, start, lines = 0;
    long size;

    if (f == NULL)
        return;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > TAIL_BYTES)
        fseek(f, size - TAIL_BYTES, SEEK_SET);
    else
        rewind(f);
    n = fread(buf, 1, TAIL_BYTES, f);
    fclose(f);
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n')
        buf[--n] = '\0';
    for (start = n; start > 0; start--) {
        if (buf[start - 1] == '\n' && ++lines == TAIL_LINES)
            break;
    }
    while (start < n) {
        size_t len = strcspn(buf + start, "\n");

        printf("    | %.*s\n", (int)len, buf + start);
        start += len + 1;
    }
}

/** Describe how a step failed, into `buf`. */
static void describe(const struct check *c, const struct etude_exec_result *res, char *buf,
                     size_t n)
{
    const char *what = c->step == STEP_COMPILE ? "compile" : "run";
    unsigned bound = c->step == STEP_COMPILE ? COMPILE_SECONDS : c->seconds;

    if (c->hung)
        snprintf(buf, n, "%s hung; killed after %us", what, bound);
    else if (res == NULL)
        snprintf(buf, n, "%s could not start", what);
    else if (res->end == ETUDE_END_SIGNALED)
        snprintf(buf, n, "%s, signal %d", what, res->value);
    else if (res->end == ETUDE_END_EXITED && res->value != 0)
        snprintf(buf, n, "%s, status %d", what, res->value);
    else if (res->end == ETUDE_END_EXITED)
        snprintf(buf, n, "%s left processes running", what);
    else
        snprintf(buf, n, "%s, its keeper was lost", what);
}

/** Remove a check's scratch directory, or name it when it is kept or stays. */
static void settle_scratch(const struct run *r, const struct check *c, int keep)
{
    char *dir = join(r->scratch, "/", c->name);

    if (dir == NULL)
        return;
    if (keep && has_entries(dir))
        printf("    scratch kept: %s\n", dir);
    else if (remove_tree(dir) != 0)
        printf("    scratch not removed: %s\n", dir);
    free(dir);
}

/** Print a check's result, and free its step. */
static void report(struct run *r, struct check *c, const struct etude_exec_result *res, int ok)
{
    char *log = log_of(r, c, c->step);
    char why[160];

    if (c->stopped) {
        printf("  STOPPED: %s\n", c->label);
    } else if (ok) {
        printf("  PASS: %s\n", c->label);
        if (log != NULL)
            print_not_run(log);
    } else {
        describe(c, res, why, sizeof(why));
        printf("  FAIL (%s; see %s): %s\n", why, log != NULL ? log : c->name, c->label);
        if (log != NULL) {
            print_sanitizer(log);
            print_tail(log);
        }
        r->failed++;
    }
    settle_scratch(r, c, !ok && !c->stopped);
    fflush(stdout);
    free(log);
    if (c->proc != NULL)
        etude_proc_free(c->proc);
    c->proc = NULL;
    c->state = c->stopped ? STOPPED : ok ? PASSED : FAILED;
    r->reported++;
}

/** Make a check's empty scratch directory and start its first step. */
static void start_check(struct run *r, struct check *c)
{
    char *dir = join(r->scratch, "/", c->name);
    int err = ENOMEM;

    c->step = *c->compile != '\0' ? STEP_COMPILE : STEP_RUN;
    if (dir != NULL) {
        err = remove_tree(dir) == 0 && mkdir(dir, 0700) == 0 ? 0 : errno;
        if (err == 0)
            err = start_step(r, c);
        free(dir);
    }
    if (err != 0) {
        printf("etude-host: %s: %s\n", c->name, strerror(err));
        report(r, c, NULL, 0);
    }
}

/**
 * Take a step that finished: a compile that passed starts the run, and any
 * other end is the check's result.
 *
 * @return 1 when the check is done, 0 when its run started.
 */
static int finish(struct run *r, struct check *c)
{
    const struct etude_exec_result *res = etude_proc_result(c->proc);
    int ok = !c->hung && res->end == ETUDE_END_EXITED && res->value == 0 &&
             res->cleanup == ETUDE_CLEANUP_CONFIRMED;
    int err;

    if (!ok || c->step == STEP_RUN || c->stopped) {
        report(r, c, res, ok);
        return 1;
    }
    etude_proc_free(c->proc);
    c->proc = NULL;
    c->step = STEP_RUN;
    err = start_step(r, c);
    if (err != 0) {
        printf("etude-host: %s: %s\n", c->name, strerror(err));
        report(r, c, NULL, 0);
        return 1;
    }
    return 0;
}

/** Give the check whose step `p` is. */
static struct check *owner(struct run *r, const struct etude_proc *p)
{
    size_t i;

    for (i = 0; i < r->n; i++) {
        if (r->checks[i].proc == p)
            return &r->checks[i];
    }
    return NULL;
}

/**
 * End each running step that has passed its bound, and give the time until
 * the next bound, in milliseconds, between 10 and 1000.
 */
static int expire(struct run *r)
{
    uint64_t now = posix_now_us(), next = 1000000u;
    size_t i;

    for (i = 0; i < r->n; i++) {
        struct check *c = &r->checks[i];
        uint64_t bound, end;

        if (c->proc == NULL || c->hung || c->stopped)
            continue;
        bound = (uint64_t)(c->step == STEP_COMPILE ? COMPILE_SECONDS : c->seconds) * 1000000u;
        end = c->began_us + bound;
        if (stop_signal || now >= end) {
            c->stopped = stop_signal != 0;
            c->hung = !c->stopped;
            etude_exec_cancel(c->proc);
        } else if (end - now < next) {
            next = end - now;
        }
    }
    return next < 10000u ? 10 : (int)(next / 1000u);
}

/** Whether check `a` starts before check `b`: a shared check before an alone
 * one, and of two of one mode the one with the longer bound. */
static int before(const struct check *a, const struct check *b)
{
    if (a->alone != b->alone)
        return !a->alone;
    return a->seconds > b->seconds;
}

/** Order the checks as `before` says, ties as selected. */
static size_t *order_of(const struct run *r)
{
    size_t *order = malloc(r->n * sizeof(*order));
    size_t i, j;

    if (order == NULL)
        return NULL;
    for (i = 0; i < r->n; i++) {
        size_t k = i;

        for (j = i; j > 0 && before(&r->checks[k], &r->checks[order[j - 1]]); j--)
            order[j] = order[j - 1];
        order[j] = k;
    }
    return order;
}

/**
 * Run every check, at most `jobs` at once and an alone check with no other,
 * until each has a result or a stop signal came; after one, the running
 * checks are ended and reported.
 *
 * @return 0, or -1 when memory ran out.
 */
static int run_all(struct run *r, unsigned jobs)
{
    size_t *order = order_of(r);
    size_t next = 0;
    unsigned running = 0;
    int alone = 0; /* an alone check runs */

    if (order == NULL)
        return -1;
    while (r->reported < r->n) {
        struct etude_proc *p;
        int wait_ms;

        while (!stop_signal && next < r->n) {
            struct check *c = &r->checks[order[next]];

            if (c->alone ? running > 0 : alone || running >= jobs)
                break;
            next++;
            start_check(r, c);
            running += c->state == WAITING;
            alone = c->alone && c->state == WAITING;
        }
        if (running == 0)
            break;
        wait_ms = expire(r);
        p = etude_exec_wait(r->ex, wait_ms);
        if (p != NULL)
            running -= (unsigned)finish(r, owner(r, p));
        alone = alone && running > 0;
    }
    free(order);
    return 0;
}

/** Give the processors online, at least 1 and at most JOBS_MAX. */
static unsigned processors(void)
{
#ifdef _WIN32
    long n = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif

    return n < 1 ? 1 : n > JOBS_MAX ? JOBS_MAX : (unsigned)n;
}

/**
 * Give the absolute path of a directory, since a check's TMPDIR must name its
 * scratch from any working directory.
 *
 * @return the path, or NULL after a diagnostic.
 */
static const char *dir_of(const char *path)
{
    struct stat st;
    char *abs;

    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "etude-host: %s: not a directory\n", path);
        return NULL;
    }
    abs = realpath(path, NULL);
    if (abs == NULL)
        fprintf(stderr, "etude-host: %s: %s\n", path, strerror(errno));
    return abs;
}

/** Catch the stop signals without restarting the executor's waits. */
static void catch_stops(void)
{
#ifdef _WIN32
    signal(SIGTERM, note_stop);
    signal(SIGINT, note_stop);
#else
    static const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_stop;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
#endif
}

/** Print the run's last line and give its exit status. */
static int conclude(const struct run *r)
{
    size_t i;

    if (stop_signal) {
        size_t done = 0;

        for (i = 0; i < r->n; i++)
            done += r->checks[i].state == PASSED || r->checks[i].state == FAILED;
        printf("etude-host: result: stopped by signal %d: %lu of %lu checks finished\n",
               (int)stop_signal, (unsigned long)done, (unsigned long)r->n);
        return 128 + stop_signal;
    }
    if (r->reported != r->n) {
        printf("etude-host: result: fail: %lu of %lu checks gave no result\n",
               (unsigned long)(r->n - r->reported), (unsigned long)r->n);
        return 1;
    }
    if (r->failed == 0) {
        printf("etude-host: result: pass: %lu checks\n", (unsigned long)r->n);
        return 0;
    }
    printf("etude-host: result: fail: %lu of %lu checks:", (unsigned long)r->failed,
           (unsigned long)r->n);
    for (i = 0; i < r->n; i++) {
        if (r->checks[i].state == FAILED)
            printf(" %s", r->checks[i].name);
    }
    printf("\n");
    return 1;
}

int main(int argc, char **argv)
{
    struct run r;
    unsigned jobs = processors();
    int opt, status;

    memset(&r, 0, sizeof(r));
    while ((opt = getopt(argc, argv, "j:l:s:")) != -1) {
        if (opt == 'j' && (jobs = (unsigned)number_of(optarg, 1, JOBS_MAX)) == 0)
            opt = '?';
        if (opt == 'l')
            r.logs = optarg;
        else if (opt == 's')
            r.scratch = optarg;
        else if (opt == '?')
            break;
    }
    if (opt == '?' || optind != argc - 1 || r.logs == NULL || r.scratch == NULL) {
        fprintf(stderr, "usage: etude-host [-j JOBS] -l LOGS -s SCRATCH SELECTION\n");
        return 2;
    }
    r.logs = dir_of(r.logs);
    r.scratch = r.logs == NULL ? NULL : dir_of(r.scratch);
    if (r.scratch == NULL || read_selection(&r, argv[optind]) != 0)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    if (make_env(&r) != 0 || (r.ex = etude_exec_new()) == NULL) {
        fprintf(stderr, "etude-host: memory ran out\n");
        return 2;
    }
    printf("etude-host: running %lu checks, %u at a time\n", (unsigned long)r.n, jobs);
    if (run_all(&r, jobs) != 0)
        fprintf(stderr, "etude-host: memory ran out\n");
    etude_exec_free(r.ex);
    status = conclude(&r);
    return status;
}
