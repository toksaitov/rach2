/*
 * legacy.c: the preparation adapter on POSIX systems. It runs the
 * preparation of tests/run one step at a time through the executor, builds
 * the goals it is given, checks that they exist, and records them. The
 * common record checker is in preparation_record.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#ifdef _WIN32
#include "../win/files.h"
#else
#define ETUDE_SHELL "/bin/sh"
#endif

#include "etude/artifact.h"
#include "etude/exec.h"
#include "etude/legacy.h"
#include "../measurement_context.h"
#include "../preparation_record.h"
#include "posix.h"

#ifndef _WIN32
extern char **environ;
#endif

/* The time from a step's termination signal to its kill. make removes the
 * target it was writing when it is stopped, and tests/run gave it as long. */
#define GRACE_US 5000000u

/* The bytes kept of a step's standard output and of its standard error
 * through the executor: the path, and whatever the shell printed before its
 * output went to the log. */
#define OUTPUT_MAX 65536

/* The lines of a failed step's log printed under its result, and the bytes
 * of the log's end read to find them. */
#define TAIL_LINES 20
#define TAIL_BYTES 8192

/* A build step's output, and its errors, go to the log $1. */
static const char script_log[] = "exec >\"$1\" 2>&1; shift; exec \"$@\"";

/* The path the steps search: the caller's, with e2fsprogs's directories
 * added where a package manager keeps them off, as tests/run adds them. */
static const char script_path[] = "exec 2>\"$1\"; [ ! -f tools/setup/e2fsprogs_path.sh ] || "
                                  ". tools/setup/e2fsprogs_path.sh; printf '%s\\n' \"$PATH\"";

/* The variables of make that a step does not inherit: a jobserver's
 * descriptors do not reach it, so each step's make is a make of its own. */
static const char *const dropped_env[] = {"MAKEFLAGS=",
                                          "MFLAGS=",
                                          "MAKELEVEL=",
                                          "LC_ALL=",
                                          "PATH=",
                                          "ETUDE_PREPARATION_RUN_ID=",
                                          "ETUDE_PREPARATION_RECEIPT=",
                                          "ETUDE_PREPARATION_COMMIT=",
                                          "ETUDE_PREPARATION_GRANT=",
                                          "ETUDE_PREPARATION_IMPORT=",
                                          "ETUDE_PREPARATION_CLEAN="};

/* A list of strings from malloc. */
struct list {
    char **v;
    size_t n, cap;
};

/* One step: a command run under sh(1) from the tree's root. */
struct step {
    const char *name;   /* names its log, WORK/N-NAME.log */
    const char *script; /* where its output goes */
    char **cmd;         /* the command, ended by NULL */
    int capture;        /* nonzero: its standard output is its answer */
    char *answer;       /* that answer, its first line, once it passed */
};

/* One architecture of a preparation. */
struct target {
    const char *arch;  /* i686 or x86_64 */
    const char *build; /* its build tree */
    char *var;         /* ARCH=..., given to each of its makes */
    char *kernel;      /* the kernel the suites boot */
    char *kernel_var;  /* BOOT_KERNEL=..., or NULL for the default */
};

/* One preparation under way. */
struct prep {
    const struct etude_legacy_opts *o;
    FILE *out;
    char *tree;
    struct target t[2]; /* the architectures, prepared in this order */
    size_t nt;
    const struct target *at; /* the one the steps build for now */
    char name[64];           /* a build step's name, with `at`'s architecture
                                in a preparation of both */
    char *work;
    char **env; /* the steps' environment */
    struct etude_exec *ex;
    unsigned steps;       /* the steps begun, which number their logs */
    char *why;            /* what failed, for the result line */
    struct list goals;    /* the artifacts the last step builds */
    struct list required; /* every artifact recorded */
    size_t files;         /* the regular files of `required` */
    const char *receipt_path;
    struct etude_preparation_receipt receipt;
};

/* ---- Strings and lists. ---- */

/** Give a string from malloc made as printf makes it, or NULL. */
static char *format(const char *fmt, ...)
{
    va_list ap;
    char *s;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || (s = malloc((size_t)n + 1)) == NULL)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/** Add a copy of `s` to list `l`; 0, or -1 when memory ran out. */
static int list_add(struct list *l, const char *s)
{
    char **more;

    if (l->n + 1 >= l->cap) {
        more = realloc(l->v, (l->cap * 2 + 16) * sizeof(*more));
        if (more == NULL)
            return -1;
        l->v = more;
        l->cap = l->cap * 2 + 16;
    }
    if ((l->v[l->n] = strdup(s)) == NULL)
        return -1;
    l->v[++l->n] = NULL;
    return 0;
}

/** Free the strings of list `l` and the list. */
static void list_free(struct list *l)
{
    size_t i;

    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

/** Order two strings for qsort(3). */
static int by_text(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/** Sort list `l` and remove the strings it has twice. */
static void list_unique(struct list *l)
{
    size_t i, k = 0;

    if (l->n == 0)
        return;
    qsort(l->v, l->n, sizeof(*l->v), by_text);
    for (i = 1; i < l->n; i++) {
        if (strcmp(l->v[i], l->v[k]) == 0)
            free(l->v[i]);
        else
            l->v[++k] = l->v[i];
    }
    l->n = k + 1;
    l->v[l->n] = NULL;
}

/** Say whether `s` is letters, digits, `_` and `-`, and begins with neither. */
static int plain_name(const char *s)
{
    static const char allowed[] = "abcdefghijklmnopqrstuvwxyz"
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";

    return *s != '\0' && *s != '-' && *s != '_' && s[strspn(s, allowed)] == '\0';
}

/* ---- Results. ---- */

/** Print the result line of a request refused before anything was built. */
static int refuse(struct prep *p, const char *fmt, ...)
{
    va_list ap;

    fprintf(p->out, "etude prepare: result: refused: ");
    va_start(ap, fmt);
    vfprintf(p->out, fmt, ap);
    va_end(ap);
    fprintf(p->out, "\n");
    return 2;
}

/** Note what failed, for the result line, and give the status 1. */
static int failed(struct prep *p, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (p->why != NULL)
        return 1;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n >= 0 && (p->why = malloc((size_t)n + 1)) != NULL) {
        va_start(ap, fmt);
        vsnprintf(p->why, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    return 1;
}

/** Report an error of the system and give the status 2. */
static int sys_error(struct prep *p, const char *what, int err)
{
    fprintf(p->out, "etude prepare: result: error: %s: %s\n", what, strerror(err));
    return 2;
}

/** Say whether a stop signal has come. */
static int stopped(const struct prep *p)
{
    return p->o->stop != NULL && *p->o->stop != 0;
}

/** Validate optional measurement inputs before starting any preparation. */
static int begin_receipt(struct prep *p)
{
    const char *run = getenv("ETUDE_PREPARATION_RUN_ID");
    const char *path = getenv("ETUDE_PREPARATION_RECEIPT");
    const char *commit = getenv("ETUDE_PREPARATION_COMMIT");
    const char *seed = getenv("ETUDE_PREPARATION_GRANT");
    struct stat st;
    size_t n;

    if (!run && !path && !commit && !seed)
        return 0;
    if (!run || !path || !commit || !*path || !p->o->record)
        return refuse(p, "a preparation receipt needs RUN_ID, RECEIPT and COMMIT variables "
                         "and an artifact record");
    n = strlen(run);
    if (!n || n >= sizeof(p->receipt.run_id) ||
        !strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", run[0]) ||
        strspn(run, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-") != n)
        return refuse(p, "the preparation run identity is not an ASCII token");
    if (strlen(commit) != 40 || strspn(commit, "0123456789abcdef") != 40)
        return refuse(p, "the preparation commit is not a full lowercase SHA-1");
    if (!lstat(path, &st) || errno != ENOENT)
        return refuse(p, "the preparation receipt must be a new file: %s", path);
    memcpy(p->receipt.run_id, run, n + 1);
    memcpy(p->receipt.commit, commit, 41);
    if (etude_context_clock(&p->receipt.clock) || etude_history_now(&p->receipt.start))
        return refuse(p, "the preparation clock or machine identity is unavailable");
    if (seed) {
        struct etude_preparation_grant grant;
        char claim[sizeof p->receipt.run_id + 8];
        int size = snprintf(claim, sizeof claim, "%s:prepare", run);
        if (size < 0 || (size_t)size >= sizeof claim ||
            etude_context_grant_read(seed, claim, &grant))
            return refuse(p, "the preparation grant does not match this run and clock");
        p->receipt.clock = grant.clock;
        p->receipt.start = grant.start;
    }
    p->receipt_path = path;
    return 0;
}

/** Publish timing only after the artifact record and cleanup succeeded. */
static int finish_receipt(struct prep *p)
{
    struct etude_context_clock end;
    struct stat st;

    if (!p->receipt_path)
        return 0;
    if (etude_digest_file(NULL, p->o->record, &st, p->receipt.record_sha256))
        return failed(p, "the preparation record cannot be hashed");
    if (etude_context_clock(&end) || etude_history_now(&p->receipt.end) ||
        strcmp(end.machine_id, p->receipt.clock.machine_id) ||
        strcmp(end.boot_id, p->receipt.clock.boot_id))
        return failed(p, "the preparation clock identity changed or is unavailable");
    p->receipt.elapsed = etude_history_elapsed(&p->receipt.start, &p->receipt.end);
    if (p->receipt.elapsed.reason)
        return failed(p, "preparation timing: %s", p->receipt.elapsed.reason);
    if (stopped(p))
        return 128 + (int)*p->o->stop;
    p->receipt.passed = 1;
    p->receipt.cleanup_confirmed = 1;
    p->receipt.same_run = 1;
    if (etude_context_receipt_write(p->receipt_path, &p->receipt))
        return failed(p, "the preparation receipt cannot be published: %s", strerror(errno));
    return 0;
}

/* ---- Admission. ---- */

/**
 * Say whether the tree's run lock is held for this process: the lock's
 * owner file names the process RACH2_TREE_HELD names, and that process runs.
 */
static int tree_held(const struct prep *p)
{
    const char *held = getenv("RACH2_TREE_HELD");
    char *path = format("%s/build/.run.lock/owner", p->tree), *line = NULL;
    long pid = held != NULL ? strtol(held, NULL, 10) : 0;
    size_t cap = 0;
    FILE *f = pid > 0 && path != NULL ? fopen(path, "r") : NULL;
    int ok = 0;

    while (f != NULL && !ok && getline(&line, &cap, f) >= 0)
        ok = strncmp(line, "pid ", 4) == 0 && strtol(line + 4, NULL, 10) == pid;
    if (f != NULL)
        fclose(f);
    free(line);
    free(path);
    {
        struct etude_pid id;
        return ok && etude_process_state(pid, &id) == 1;
    }
}

/** Take the architectures, `i686`, `x86_64` or `i686,x86_64`, and the
 * kernel; 0, or 2 after a refusal. */
static int admit_machine(struct prep *p)
{
    const struct etude_legacy_opts *o = p->o;
    const char *arch = o->arch != NULL ? o->arch : "i686";
    struct target *t;
    size_t i;

    if (strcmp(arch, "i686,x86_64") == 0) {
        p->t[0].arch = "i686";
        p->t[1].arch = "x86_64";
        p->nt = 2;
    } else if (strcmp(arch, "i686") == 0 || strcmp(arch, "x86_64") == 0) {
        p->t[0].arch = arch;
        p->nt = 1;
        for (i = 0; strcmp(arch, "i686") == 0 && i < o->ngoals; i++)
            if (strncmp(o->goals[i], "build/x86_64/", 13) == 0) {
                p->t[1].arch = "x86_64";
                p->nt = 2;
                break;
            }
    } else {
        return refuse(p, "%s: the architectures are i686 and x86_64", arch);
    }
    if (o->kernel != NULL && !plain_name(o->kernel))
        return refuse(p, "%s: a kernel's name is letters, digits, '_' and '-'", o->kernel);
    for (i = 0; i < p->nt; i++) {
        t = &p->t[i];
        t->build = strcmp(t->arch, "x86_64") == 0 ? "build/x86_64" : "build";
        t->var = format("ARCH=%s", t->arch);
        if (o->kernel != NULL) {
            t->kernel = format("%s/rach2-%s/kernel", t->build, o->kernel);
            t->kernel_var = t->kernel != NULL ? format("BOOT_KERNEL=%s", t->kernel) : NULL;
        } else {
            t->kernel = format("%s/rach2/kernel", t->build);
        }
        if (t->var == NULL || t->kernel == NULL || (o->kernel != NULL && t->kernel_var == NULL))
            return sys_error(p, "the request", ENOMEM);
    }
    p->at = &p->t[0];
    return 0;
}

/** Assign fixed x86_64 goals to their build architecture. Shared goals stay
 * with the requested architecture, or i686 when both are prepared. */
static int goal_of(const struct prep *p, const struct target *t, const char *goal)
{
    int x86_64 = strncmp(goal, "build/x86_64/", 13) == 0;

    return p->nt == 1 || x86_64 == (strcmp(t->arch, "x86_64") == 0);
}

/** Say whether `s` names a goal inside the tree: a relative path of
 * letters, digits, `.`, `_`, `+` and `-`, whose parts are not `.` or `..`. */
static int goal_name(const char *s)
{
    const char *part;
    size_t n;

    if (s[0] == '\0' || s[0] == '-' ||
        s[strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                    "VWXYZ0123456789._+-/")] != '\0')
        return 0;
    for (part = s; *part != '\0'; part += n + (part[n] == '/')) {
        n = strcspn(part, "/");
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && strncmp(part, "..", 2) == 0))
            return 0;
    }
    return 1;
}

/** Take the goals named, of which there must be one at least; 0, or 2
 * after a refusal. */
static int admit_goals(struct prep *p)
{
    const struct etude_legacy_opts *o = p->o;
    size_t i;

    if (o->ngoals == 0)
        return refuse(p, "no goal named");
    for (i = 0; i < o->ngoals; i++)
        if (!goal_name(o->goals[i]))
            return refuse(p, "%s: a goal is a relative path inside the tree", o->goals[i]);
    return 0;
}

/** Check the request before anything runs; 0, or 2 after a refusal. */
static int admit(struct prep *p)
{
    int status = admit_machine(p);

    if (status != 0)
        return status;
    p->tree = realpath(p->o->tree != NULL ? p->o->tree : ".", NULL);
    if (p->tree == NULL)
        return refuse(p, "%s: %s", p->o->tree != NULL ? p->o->tree : ".", strerror(errno));
    if ((status = admit_goals(p)) != 0)
        return status;
    if (!tree_held(p))
        return refuse(p, "the tree is not held: RACH2_TREE_HELD must name the running process "
                         "that build/.run.lock/owner names");
    return 0;
}

/* ---- The work directory and the steps' environment. ---- */

/** Say whether `var` is one of the inherited variables a step does not get. */
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
 * Make the steps' environment: this process's without the dropped
 * variables, with LC_ALL=C and the PATH `path`; 0, or -1 when memory ran
 * out.
 */
static int make_env(struct prep *p, const char *path)
{
    size_t n = 0, k = 0, i;
    char **env;

    while (environ[n] != NULL)
        n++;
    if ((env = calloc(n + 3, sizeof(*env))) == NULL)
        return -1;
    for (i = 0; i < n; i++) {
        if (!dropped(environ[i]) && (env[k++] = strdup(environ[i])) == NULL)
            break;
    }
    env[k] = i == n ? strdup("LC_ALL=C") : NULL;
    env[k + 1] = env[k] != NULL ? format("PATH=%s", path) : NULL;
    for (i = 0; p->env != NULL && p->env[i] != NULL; i++)
        free(p->env[i]);
    free(p->env);
    p->env = env;
    return env[k + 1] != NULL ? 0 : -1;
}

/** Make the work directory and the executor; 0, or 2 after an error. */
static int open_work(struct prep *p)
{
#ifdef _WIN32
    const char *tmp = etude_win_temp();
#else
    const char *tmp = getenv("TMPDIR");
#endif
    const char *path = getenv("PATH");

    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
    if (p->o->work != NULL) {
        if (mkdir(p->o->work, 0700) != 0)
            return sys_error(p, p->o->work, errno);
        p->work = realpath(p->o->work, NULL);
    } else if ((p->work = format("%s/rach2-legacy.XXXXXX", tmp)) != NULL &&
               mkdtemp(p->work) == NULL) {
        int e = errno;

        free(p->work);
        p->work = NULL;
        return sys_error(p, "the work directory", e);
    }
    if (p->work == NULL)
        return sys_error(p, "the work directory", errno != 0 ? errno : ENOMEM);
    if (make_env(p, path != NULL ? path : "/usr/bin:/bin") != 0 ||
        (p->ex = etude_exec_new()) == NULL)
        return sys_error(p, "the steps", ENOMEM);
    return 0;
}

/**
 * Remove a tree, not following links; 0 once it is gone. A directory is
 * made writable and searchable before it is read.
 */
static int remove_tree(const char *path)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    char *sub;
    int bad = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    chmod(path, (st.st_mode & 07777) | S_IRWXU);
    if ((d = opendir(path)) == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if ((sub = format("%s/%s", path, e->d_name)) == NULL || remove_tree(sub) != 0)
            bad = -1;
        free(sub);
    }
    closedir(d);
    return rmdir(path) == 0 || errno == ENOENT ? bad : -1;
}

/* ---- Steps. ---- */

/** Print the last lines of a log, indented, so a failure reads without it. */
static void print_tail(FILE *out, const char *path)
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

        fprintf(out, "    | %.*s\n", (int)len, buf + start);
        start += len + 1;
    }
}

/** Give the arguments that run step `s` under sh(1) with its log `log`. */
static char **step_argv(const struct step *s, const char *log)
{
    size_t n = 0, k = 0, i;
    char **argv;

    while (s->cmd[n] != NULL)
        n++;
    if ((argv = calloc(n + 7, sizeof(*argv))) == NULL)
        return NULL;
    argv[k++] = (char *)ETUDE_SHELL;
    argv[k++] = "-c";
    argv[k++] = (char *)s->script;
    argv[k++] = "etude";
    argv[k++] = (char *)log;
    for (i = 0; i < n; i++)
        argv[k++] = s->cmd[i];
    return argv;
}

/* How a step ended besides by itself. */
enum ended { ENDED_ITSELF, ENDED_BOUND, ENDED_STOP };

/* A log that is copied to the output as it grows. */
struct follow {
    const char *path;
    int fd; /* -1 until the log exists */
};

/** Copy what the log `f` has gained to the output. */
static void follow_log(struct prep *p, struct follow *f)
{
    char buf[8192];
    ssize_t n;

    if (f->fd < 0 && (f->fd = open(f->path, O_RDONLY)) < 0)
        return;
    while ((n = read(f->fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, p->out);
    fflush(p->out);
}

/** Wait until the step `proc` finishes, cancelling it at its bound or at a
 * stop signal, and say which ended it; with `f`, copy that log to the
 * output meanwhile and once more at the end. */
static enum ended await_step(struct prep *p, struct etude_proc *proc, struct follow *f)
{
    uint64_t began = posix_now_us(), bound = (uint64_t)p->o->seconds * 1000000u, now;
    enum ended ended = ENDED_ITSELF;
    int wait_ms, done;

    if (bound == 0)
        bound = (uint64_t)ETUDE_LEGACY_SECONDS * 1000000u;
    for (;;) {
        now = posix_now_us();
        wait_ms = f != NULL ? 200 : 1000;
        if (ended == ENDED_ITSELF && stopped(p)) {
            ended = ENDED_STOP;
            etude_exec_cancel(proc);
        } else if (ended == ENDED_ITSELF && now - began >= bound) {
            ended = ENDED_BOUND;
            etude_exec_cancel(proc);
        } else if (ended == ENDED_ITSELF && began + bound - now < (uint64_t)wait_ms * 1000u) {
            wait_ms = (int)((began + bound - now) / 1000u) + 1;
        }
        done = etude_exec_wait(p->ex, wait_ms) == proc;
        if (f != NULL)
            follow_log(p, f);
        if (done)
            return ended;
    }
}

/** Describe how a step that did not pass ended, into `buf`. */
static void describe(const struct prep *p, const struct etude_exec_result *r, enum ended ended,
                     char *buf, size_t n)
{
    unsigned bound = p->o->seconds != 0 ? p->o->seconds : ETUDE_LEGACY_SECONDS;

    if (ended == ENDED_BOUND)
        snprintf(buf, n, "it ran past its bound of %u s", bound);
    else if (r->end == ETUDE_END_SIGNALED)
        snprintf(buf, n, "it was killed by signal %d", r->value);
    else if (r->end == ETUDE_END_EXITED && r->value != 0)
        snprintf(buf, n, "it exited %d", r->value);
    else if (r->end == ETUDE_END_EXITED && r->cleanup != ETUDE_CLEANUP_CONFIRMED)
        snprintf(buf, n, "its processes were not all seen to end");
    else if (r->end == ETUDE_END_EXITED)
        snprintf(buf, n, "its output passed %d bytes", OUTPUT_MAX);
    else
        snprintf(buf, n, "its end was not seen");
}

/** Judge a step that has finished; 0, 1 after a failure, or 128 plus the
 * signal after a stop. */
static int judge(struct prep *p, struct step *s, const struct etude_exec_result *r,
                 enum ended ended, const char *log)
{
    char why[128];
    int ok = ended == ENDED_ITSELF && r->end == ETUDE_END_EXITED && r->value == 0 &&
             r->cleanup == ETUDE_CLEANUP_CONFIRMED && r->out.total == r->out.kept;

    if (ended == ENDED_STOP) {
        fprintf(p->out, "etude prepare: step: %s: stopped by signal %d\n", s->name,
                (int)*p->o->stop);
        return 128 + (int)*p->o->stop;
    }
    if (ok && s->capture && (s->answer = strdup(r->out.data != NULL ? r->out.data : "")) == NULL)
        return failed(p, "%s: memory ran out", s->name);
    if (ok && s->capture)
        s->answer[strcspn(s->answer, "\n")] = '\0';
    if (ok)
        return 0;
    describe(p, r, ended, why, sizeof(why));
    fprintf(p->out, "etude prepare: step: %s: failed: %s; see %s\n", s->name, why, log);
    if (r->err.kept > 0)
        fprintf(p->out, "    | %s%s", r->err.data,
                r->err.data[r->err.kept - 1] == '\n' ? "" : "\n");
    print_tail(p->out, log);
    return failed(p, "%s: %s", s->name, why);
}

/**
 * Run step `s` through the executor from the tree's root, with its output
 * as its script directs and its log WORK/N-NAME.log.
 *
 * @return 0, 1 after a failure, or 128 plus the signal after a stop.
 */
static int run_step(struct prep *p, struct step *s)
{
    char *log = format("%s/%u-%s.log", p->work, ++p->steps, s->name);
    char **argv = log != NULL ? step_argv(s, log) : NULL;
    struct etude_exec_spec spec;
    struct etude_proc *proc = NULL;
    struct follow follow = {log, -1};
    enum ended ended;
    int status;

    if (argv == NULL) {
        free(log);
        return failed(p, "%s: memory ran out", s->name);
    }
    if (stopped(p)) {
        free(argv);
        free(log);
        return 128 + (int)*p->o->stop;
    }
    fprintf(p->out, "etude prepare: step: %s\n", s->name);
    fflush(p->out);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = s->name;
    spec.argv = argv;
    spec.envp = p->env;
    spec.cwd = p->tree;
    spec.output_max = OUTPUT_MAX;
    spec.grace_us = GRACE_US;
    if (etude_exec_start(p->ex, &spec, &proc) != 0) {
        status = failed(p, "%s: it did not start: %s", s->name, strerror(errno));
    } else {
        ended = await_step(p, proc, s->capture ? NULL : &follow);
        status = judge(p, s, etude_proc_result(proc), ended, log);
        etude_proc_free(proc);
    }
    if (follow.fd >= 0)
        close(follow.fd);
    free(argv);
    free(log);
    return status;
}

/**
 * Run `make -C TREE --no-print-directory ARCH=A [BOOT_KERNEL=K]` with the
 * `n` further arguments `args` as step `name` with `script`.
 */
static int run_make(struct prep *p, struct step *s, char *const *args, size_t n)
{
    size_t k = 0, i;
    int status;

    if ((s->cmd = calloc(n + 7, sizeof(*s->cmd))) == NULL)
        return failed(p, "%s: memory ran out", s->name);
    s->cmd[k++] = "make";
    s->cmd[k++] = "-C";
    s->cmd[k++] = p->tree;
    s->cmd[k++] = "--no-print-directory";
    s->cmd[k++] = p->at->var;
    if (p->at->kernel_var != NULL)
        s->cmd[k++] = p->at->kernel_var;
    for (i = 0; i < n; i++)
        s->cmd[k++] = args[i];
    status = run_step(p, s);
    free(s->cmd);
    s->cmd = NULL;
    return status;
}

/** Give the name of build step `name` for `p->at`: `name`, or in a
 * preparation of both architectures `name-ARCH`. */
static const char *step_name(struct prep *p, const char *name)
{
    if (p->nt == 1)
        return name;
    snprintf(p->name, sizeof(p->name), "%s-%s", name, p->at->arch);
    return p->name;
}

#ifndef _WIN32
/** Run make with the goals `a` and `b`, NULL for none, as a build step. */
static int build_step(struct prep *p, const char *name, const char *a, const char *b)
{
    struct step s;
    char *args[2];
    size_t n = 0;

    memset(&s, 0, sizeof(s));
    s.name = step_name(p, name);
    s.script = script_log;
    if (a != NULL)
        args[n++] = (char *)a;
    if (b != NULL)
        args[n++] = (char *)b;
    return run_make(p, &s, args, n);
}

#endif

/* ---- The steps' path. ---- */

/** Find the path the steps search, and set it in their environment. */
static int find_path(struct prep *p)
{
    struct step s;
    char *cmd[] = {NULL};
    int status;

    memset(&s, 0, sizeof(s));
    s.name = "path";
    s.script = script_path;
    s.cmd = cmd;
    s.capture = 1;
    if ((status = run_step(p, &s)) != 0)
        return status;
    status = make_env(p, s.answer) == 0 ? 0 : failed(p, "path: memory ran out");
    free(s.answer);
    return status;
}

/* ---- The artifacts. ---- */

/** Take the goals of `p->at` and the kernel its suites boot as artifacts
 * required, and those goals as the ones its last step builds; 0, or 1. */
static int find_artifacts(struct prep *p)
{
    size_t i;

    list_free(&p->goals);
    for (i = 0; i < p->o->ngoals; i++)
        if (goal_of(p, p->at, p->o->goals[i]) && (list_add(&p->required, p->o->goals[i]) != 0 ||
                                                  list_add(&p->goals, p->o->goals[i]) != 0))
            return failed(p, "artifacts: memory ran out");
    if (list_add(&p->required, p->at->kernel) != 0)
        return failed(p, "artifacts: memory ran out");
    list_unique(&p->goals);
    list_unique(&p->required);
    return 0;
}

#ifndef _WIN32
/** Build the goals in one make. */
static int build_goals(struct prep *p)
{
    struct step s;
    char **args;
    int status;

    if (p->goals.n == 0)
        return 0;
    if ((args = calloc(p->goals.n + 1, sizeof(*args))) == NULL)
        return failed(p, "artifacts: memory ran out");
    args[0] = "-j";
    memcpy(args + 1, p->goals.v, p->goals.n * sizeof(*args));
    memset(&s, 0, sizeof(s));
    s.name = step_name(p, "artifacts");
    s.script = script_log;
    status = run_make(p, &s, args, p->goals.n + 1);
    free(args);
    return status;
}

#endif

/** Write the record's head to `f`: its tree, and the architecture and the
 * kernel of each target. */
static void record_head(const struct prep *p, FILE *f)
{
    size_t i;

    fprintf(f, "%s\ntree %s\n", ETUDE_PREPARATION_HEAD, p->tree);
    for (i = 0; i < p->nt; i++)
        fprintf(f, "arch %s\nkernel %s\n", p->t[i].arch, p->t[i].kernel);
}

/**
 * Check that every artifact required exists, and write the record of each
 * regular file to `f`: `file DIGEST SIZE INODE MTIME PATH`, its time in
 * nanoseconds. A directory is accepted and not recorded. A file's digest is
 * taken from the tree's digests file when it keeps one.
 */
static int hash_artifacts(struct prep *p, FILE *f)
{
    struct stat st;
    char hex[65], *path, *cache = etude_record_digests_path(p->tree);
    size_t i, missing = 0;

    if (cache == NULL)
        return failed(p, "artifacts: memory ran out");
    for (i = 0; i < p->required.n && !stopped(p); i++) {
        if ((path = etude_record_artifact_path(p->tree, p->required.v[i])) == NULL) {
            free(cache);
            return failed(p, "artifacts: memory ran out");
        }
        if (etude_digest_file(cache, path, &st, hex) == 0) {
            p->files++;
            fprintf(f, "file %s %llu %llu %lld %s\n", hex, (unsigned long long)st.st_size,
                    (unsigned long long)st.st_ino, etude_record_mtime_ns(&st), p->required.v[i]);
        } else if (errno != EISDIR) {
            fprintf(p->out, "etude prepare: missing: %s: %s\n", p->required.v[i],
                    errno == EINVAL ? "not a regular file" : strerror(errno));
            missing++;
        }
        free(path);
    }
    free(cache);
    if (stopped(p)) {
        fprintf(p->out, "etude prepare: artifacts: stopped by signal %d\n", (int)*p->o->stop);
        return 128 + (int)*p->o->stop;
    }
    return missing == 0 ? 0
                        : failed(p, "%lu of %lu artifacts are missing", (unsigned long)missing,
                                 (unsigned long)p->required.n);
}

/**
 * Check the artifacts and write the record: to a new file beside the
 * caller's, renamed to it once whole, so that a reader finds the record
 * whole or not at all; to WORK/record when the caller asked for none.
 */
static int write_record(struct prep *p)
{
    char *dest = p->o->record != NULL ? strdup(p->o->record) : format("%s/record", p->work);
    char *tmp = dest != NULL ? format("%s.XXXXXX", dest) : NULL;
    int fd = tmp != NULL ? mkstemp(tmp) : -1, status;
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;

    if (f == NULL) {
        status = failed(p, "record: %s cannot be written: %s", dest != NULL ? dest : "",
                        strerror(errno != 0 ? errno : ENOMEM));
        if (fd >= 0) {
            close(fd);
            unlink(tmp);
        }
        free(tmp);
        free(dest);
        return status;
    }
    record_head(p, f);
    status = hash_artifacts(p, f);
    if ((fflush(f) != 0 || fsync(fd) != 0) && status == 0)
        status = failed(p, "record: %s cannot be written: %s", dest, strerror(errno));
    if (fclose(f) != 0 && status == 0)
        status = failed(p, "record: %s cannot be closed: %s", dest, strerror(errno));
    if (status == 0 && rename(tmp, dest) != 0)
        status = failed(p, "record: %s cannot be written: %s", dest, strerror(errno));
    if (status != 0)
        unlink(tmp);
    free(tmp);
    free(dest);
    return status;
}

/* ---- The preparation. ---- */

/** Make the recovery key when the tree has a recovery system and none, and
 * give the committed demo keys the mode ssh(1) requires. */
static int keys(struct prep *p)
{
    static const char *const demo[] = {"krffs/rach2_demo_ed25519", "krffs/rach2_test_ed25519"};
    char *manifest = format("%s/krffs/manifests/rach2-recovery.manifest", p->tree);
    char *key = format("%s/krffs/recovery_authorized_keys", p->tree), *path;
    char *goal[] = {"recovery-key"};
    struct step s;
    size_t i;
    int status = 0;

    memset(&s, 0, sizeof(s));
    s.name = "recovery-key";
    s.script = script_log;
    if (manifest == NULL || key == NULL)
        status = failed(p, "recovery-key: memory ran out");
    else if (access(manifest, F_OK) == 0 && access(key, F_OK) != 0)
        status = run_make(p, &s, goal, 1);
    for (i = 0; status == 0 && i < sizeof(demo) / sizeof(demo[0]); i++) {
        if ((path = format("%s/%s", p->tree, demo[i])) != NULL && access(path, F_OK) == 0 &&
            chmod(path, 0600) != 0)
            status = failed(p, "%s: chmod: %s", demo[i], strerror(errno));
        free(path);
    }
    free(manifest);
    free(key);
    return status;
}

/** Build the artifacts of `p->at` and take them as required. */
static int prepare_target(struct prep *p)
{
#ifdef _WIN32
    struct step s;
    char **args = calloc(p->o->ngoals + 7, sizeof(*args));
    size_t i, n = 6;
    int status;

    if (args == NULL)
        return failed(p, "tooling: memory ran out");
    args[0] = "python3";
    args[1] = "tools/etude/collection_tools.py";
    args[2] = "prepare";
    args[3] = p->tree;
    args[4] = (char *)p->at->arch;
    args[5] = (char *)(p->o->kernel != NULL ? p->o->kernel : "");
    for (i = 0; i < p->o->ngoals; i++)
        if (goal_of(p, p->at, p->o->goals[i]))
            args[n++] = (char *)p->o->goals[i];
    memset(&s, 0, sizeof(s));
    s.name = step_name(p, "tooling");
    s.script = "exec >\"$1\" 2>&1; shift; REPO=$PWD; "
               ". tools/etude/collection-windows.sh && "
               "collection_windows && exec \"$@\"";
    s.cmd = args;
    status = run_step(p, &s);
    free(args);
    return status == 0 ? find_artifacts(p) : status;
#else
    char *kernel_goal;
    int status = build_step(p, "components", "-j", "components");

    if (status == 0 && p->o->kernel != NULL) {
        kernel_goal = format("%s-kernel", p->o->kernel);
        status = kernel_goal != NULL ? build_step(p, "kernel", kernel_goal, NULL)
                                     : failed(p, "kernel: memory ran out");
        free(kernel_goal);
    }
    if (status == 0)
        status = find_artifacts(p);
    if (status == 0)
        status = build_goals(p);
    return status;
#endif
}

/** Run a fixed preparation-only adapter under the existing owned step. */
static int preparation_adapter(struct prep *p, const char *name, char **argv)
{
    struct step s;

    memset(&s, 0, sizeof(s));
    s.name = name;
    s.script = script_log;
    s.cmd = argv;
    return run_step(p, &s);
}

/** Clean only the declared in-house outputs before ordinary preparation. */
static int clean_inhouse(struct prep *p)
{
    const char *clean = getenv("ETUDE_PREPARATION_CLEAN");
    char *output, *slash;
    char *args[] = {"python3", "tools/ci/clean_inhouse.py", p->tree,
                    (char *)(p->o->arch && !strcmp(p->o->arch, "i686,x86_64") ? "both" :
                              p->o->arch ? p->o->arch : "i686"), NULL, NULL};
    int status;

    if (!clean)
        return 0;
    if (strcmp(clean, "1") || !p->receipt_path)
        return refuse(p, "CLEAN must be 1 and requires a preparation receipt");
    output = strdup(p->receipt_path);
    if (!output)
        return failed(p, "clean: memory ran out");
    slash = strrchr(output, '/');
#ifdef _WIN32
    if (!slash || (strrchr(output, '\\') && strrchr(output, '\\') > slash))
        slash = strrchr(output, '\\');
#endif
    if (!slash) {
        free(output);
        return refuse(p, "the preparation receipt must have an absolute directory");
    }
    *slash = '\0';
    args[4] = output;
    status = preparation_adapter(p, "clean-inhouse", args);
    free(output);
    return status;
}

/** Give the import adapter exactly the goals selected by the C allocator. */
static int import_artifacts(struct prep *p, const char *config)
{
    char *request = format("%s/import.goals", p->work);
    char *record = p->o->record ? etude_record_artifact_path(p->tree, p->o->record) : NULL;
    char *provenance = record ? format("%s.import.json", record) : NULL;
    char *config_path = etude_record_artifact_path(p->tree, config);
    char *args[] = {"python3", "tools/etude/preparation_import.py", "prepare",
                    "--tree", p->tree, "--config", (char *)config,
                    "--request", request, "--work", p->work, "--provenance", provenance, "--commit",
                    p->receipt_path ? p->receipt.commit : "", "--arches",
                    (char *)(p->o->arch ? p->o->arch : "i686"), NULL};
    FILE *f = request ? fopen(request, "wx") : NULL;
    size_t i;
    int status = 0;

    if (!f || !provenance || !config_path) {
        if (f)
            fclose(f);
        free(record);
        free(provenance);
        free(config_path);
        free(request);
        return failed(p, "import: the selected goal request cannot be written");
    }
    for (i = 0; !status && i < p->nt; i++) {
        p->at = &p->t[i];
        status = find_artifacts(p);
    }
    for (i = 0; !status && i < p->required.n; i++)
        if (fprintf(f, "%s\n", p->required.v[i]) < 0)
            status = 1;
    if (fclose(f))
        status = 1;
    p->at = &p->t[0];
    if (!status)
        status = preparation_adapter(p, "import", args);
    if (!status && (list_add(&p->required, config_path) || list_add(&p->required, provenance)))
        status = failed(p, "import: memory ran out");
    free(record);
    free(provenance);
    free(config_path);
    free(request);
    return status;
}

/** Run the steps, find and build the artifacts of each architecture in
 * turn, and record them. */
static int prepare(struct prep *p)
{
    size_t i;
    int status = find_path(p);
    const char *config = getenv("ETUDE_PREPARATION_IMPORT");

    if (status == 0)
        status = clean_inhouse(p);
    if (status == 0 && config) {
        status = import_artifacts(p, config);
        return status == 0 ? write_record(p) : status;
    }
    if (status == 0)
        status = keys(p);
    for (i = 0; status == 0 && i < p->nt; i++) {
        p->at = &p->t[i];
        status = prepare_target(p);
    }
    p->at = &p->t[0];
    if (status == 0)
        status = write_record(p);
    return status;
}

/** End the executor, keep the work and name it, or remove it, and free the
 * rest. */
static int release(struct prep *p, int keep)
{
    size_t i;
    int status = 0;

    if (p->ex != NULL)
        etude_exec_free(p->ex);
    if (p->work != NULL && keep)
        fprintf(p->out, "etude prepare: evidence: %s\n", p->work);
    else if (p->work != NULL && remove_tree(p->work) != 0) {
        fprintf(p->out, "etude prepare: %s: not removed\n", p->work);
        status = failed(p, "the preparation work directory was not removed");
    }
    fflush(p->out);
    for (i = 0; p->env != NULL && p->env[i] != NULL; i++)
        free(p->env[i]);
    free(p->env);
    return status;
}

/** Print the result after cleanup and any requested receipt publication. */
static void conclude(struct prep *p, int status)
{
    if (status == 0)
        fprintf(p->out, "etude prepare: result: prepared: %lu goals, %lu files\n",
                (unsigned long)p->o->ngoals, (unsigned long)p->files);
    else if (status == 1)
        fprintf(p->out, "etude prepare: result: failed: %s\n",
                p->why != NULL ? p->why : "memory ran out");
    else if (status > 128)
        fprintf(p->out, "etude prepare: result: stopped by signal %d\n", status - 128);
    fflush(p->out);
}

int etude_legacy_prepare(const struct etude_legacy_opts *o)
{
    struct prep p;
    size_t i;
    int status;

    memset(&p, 0, sizeof(p));
    p.o = o;
    p.out = o->out != NULL ? o->out : stdout;
    status = begin_receipt(&p);
    if (status == 0)
        status = admit(&p);
    if (status == 0) {
        fprintf(p.out, "etude prepare: preparing %lu goals for %s in %s\n",
                (unsigned long)o->ngoals, o->arch != NULL ? o->arch : "i686", p.tree);
        status = open_work(&p);
    }
    if (status == 0)
        status = prepare(&p);
    if (release(&p, status == 1) != 0 && status == 0)
        status = 1;
    if (status == 0)
        status = finish_receipt(&p);
    conclude(&p, status);
    list_free(&p.goals);
    list_free(&p.required);
    free(p.tree);
    for (i = 0; i < 2; i++) {
        free(p.t[i].var);
        free(p.t[i].kernel);
        free(p.t[i].kernel_var);
    }
    free(p.work);
    free(p.why);
    return status;
}
