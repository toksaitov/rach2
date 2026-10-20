/*
 * proof.c: the driver of etude's mutation proofs on POSIX systems. It saves
 * the sources, runs the base, fault and restored phases through the executor,
 * keeps each phase's artifacts in the store and restores the sources on every
 * path out.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "etude/artifact.h"
#include "etude/exec.h"
#include "etude/proof.h"
#include "macho.h"
#include "posix.h"
#include "proof_file.h"

extern char **environ;

/* The bytes kept of each of a build's or a test's two output streams. */
#define PROOF_OUTPUT_MAX (16u << 20)

/* From a cancellation's SIGTERM to its SIGKILL. */
#define PROOF_GRACE_US 2000000u

/* How often a running command is looked at for its bound and a signal. */
#define PROOF_TICK_MS 100

/* The sanitizers' settings of every command, whatever the driver's
 * environment says: a report ends the process with status 1 on every
 * platform, where macOS would otherwise abort it with SIGABRT. */
static const char *const sanitizer_env[] = {
    "ASAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=1",
    "UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=1",
};

/* The inherited variables a command does not get: its scratch directory, and
 * the sanitizers' settings, which sanitizer_env replaces. */
static const char *const dropped_env[] = {
    "TMPDIR=", "ASAN_OPTIONS=", "UBSAN_OPTIONS=", "LSAN_OPTIONS="};

/* The kinds of phase, each judged by its own rule. */
enum kind { BASE, FAULT, RESTORED };

/* One source of the proof. */
struct src {
    const char *rel; /* relative to the tree */
    char *path;      /* where the faults go */
    char *tree_path; /* the tree's own copy */
    char *saved;     /* the unmodified text */
    size_t len;
    const char *cur; /* the text that `path` must have now */
    size_t curlen;
    mode_t mode;
    int dirty; /* nonzero while `path` may differ from `saved` */
};

/* What a build or a test gave. */
struct ran {
    enum etude_end end;
    int value;
    int confirmed; /* every process it started is known to have ended */
    int bound;     /* nonzero: it ran past its bound and was cancelled */
    int truncated; /* nonzero: an output stream passed PROOF_OUTPUT_MAX */
    char *out, *err;
};

/* One run of a proof. */
struct run {
    const struct etude_proof *p;
    const struct etude_proof_opts *o;
    volatile sig_atomic_t *stop;
    struct etude_exec *ex;
    struct etude_store *store;
    char *work;
    char *srcroot;
    struct src *src;
    char (*base)[ETUDE_DIGEST_HEX + 1]; /* the unmodified artifacts', as compared */
    char (*dig)[ETUDE_DIGEST_HEX + 1];  /* the phase's, in the store */
    char (*cmp)[ETUDE_DIGEST_HEX + 1];  /* the phase's, as compared */
    char phase[96];
};

/* The stop flag of a caller that gives none. */
static volatile sig_atomic_t never;

/* ---- Text and files. ---- */

/** Give `a` and `b` joined by `/`, from malloc, or NULL. */
static char *join(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    char *s = malloc(n + m + 2);

    if (s != NULL) {
        memcpy(s, a, n);
        s[n] = '/';
        memcpy(s + n + 1, b, m + 1);
    }
    return s;
}

/** Give a copy of `s` from malloc, or NULL. */
static char *text_copy(const char *s)
{
    size_t n = strlen(s) + 1;
    char *c = malloc(n);

    if (c != NULL)
        memcpy(c, s, n);
    return c;
}

/** Report the proof's failure in the phase that runs; return 1. */
static int fail(const struct run *r, const char *fmt, ...)
{
    va_list ap;

    fprintf(r->o->log, "etude-proof: result: fail: %s: %s: ", r->p->name, r->phase);
    va_start(ap, fmt);
    vfprintf(r->o->log, fmt, ap);
    va_end(ap);
    fputc('\n', r->o->log);
    return 1;
}

/** Report an error of the system with `what`; return 2. */
static int sys_error(const struct run *r, const char *what, int e)
{
    fprintf(r->o->log, "etude-proof: %s: %s: %s: %s\n", r->p->name, r->phase, what, strerror(e));
    return 2;
}

/** Give `s` with the placeholders of etude_proof_run replaced, or NULL. */
static char *expand(const struct run *r, const char *s)
{
    const char *names[] = {"${TREE}", "${SRC}", "${WORK}", "${PHASE}"};
    const char *values[4];
    char *out = malloc(1), *more;
    size_t n = 0, i, k, skip;

    values[0] = r->o->tree;
    values[1] = r->srcroot;
    values[2] = r->work;
    values[3] = r->phase;
    while (out != NULL && *s != '\0') {
        const char *v = s;

        k = skip = 1;
        for (i = 0; i < 4; i++)
            if (strncmp(s, names[i], strlen(names[i])) == 0) {
                v = values[i];
                k = strlen(v);
                skip = strlen(names[i]);
                break;
            }
        more = realloc(out, n + k + 1);
        if (more == NULL)
            free(out);
        out = more;
        if (out != NULL)
            memcpy(out + n, v, k);
        n += k;
        s += skip;
    }
    if (out != NULL)
        out[n] = '\0';
    return out;
}

/** Free a list ended by NULL. */
static void list_free(char **list)
{
    size_t i;

    for (i = 0; list != NULL && list[i] != NULL; i++)
        free(list[i]);
    free(list);
}

/** Give list `argv` with its placeholders replaced, or NULL. */
static char **expand_list(const struct run *r, char *const *argv)
{
    size_t n = 0, i;
    char **out;

    while (argv[n] != NULL)
        n++;
    out = calloc(n + 1, sizeof(*out));
    for (i = 0; out != NULL && i < n; i++)
        if ((out[i] = expand(r, argv[i])) == NULL) {
            list_free(out);
            return NULL;
        }
    return out;
}

/** Write `len` bytes of `data` to the new file `path`; 0, or -1. */
static int write_new(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    size_t done = 0;
    ssize_t n;
    int e;

    if (fd < 0)
        return -1;
    while (done < len) {
        n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            e = n < 0 ? errno : EIO;
            close(fd);
            errno = e;
            return -1;
        }
        done += (size_t)n;
    }
    return close(fd);
}

/** Make each missing directory of `path` but its last part; 0, or -1. */
static int make_parents(char *path)
{
    char *slash;

    for (slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            *slash = '/';
            return -1;
        }
        *slash = '/';
    }
    return 0;
}

/**
 * Remove the file or the directory tree at `path`; 0, or -1 with `errno` set
 * by the first removal that failed.
 *
 * Each directory is made readable, writable and searchable before it is
 * read, since a command's scratch may have one that is not; a symbolic link
 * is removed, not followed.
 */
static int remove_tree(const char *path)
{
    struct dirent *d;
    struct stat st;
    char *sub;
    DIR *dir;
    int status = 0, e = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    chmod(path, (st.st_mode & 07777) | S_IRWXU);
    dir = opendir(path);
    if (dir == NULL)
        return -1;
    while ((d = readdir(dir)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        sub = join(path, d->d_name);
        if ((sub == NULL || remove_tree(sub) != 0) && status == 0) {
            e = sub == NULL ? ENOMEM : errno;
            status = -1;
        }
        free(sub);
    }
    closedir(dir);
    if (rmdir(path) != 0 && status == 0)
        return -1;
    errno = e;
    return status;
}

/** Say whether the file at `path` has exactly `len` bytes of `text`. */
static int has_text(const char *path, const char *text, size_t len)
{
    char *data;
    size_t n;
    int same;

    if (proof_slurp(path, &data, &n) != 0)
        return 0;
    same = n == len && memcmp(data, text, len) == 0;
    free(data);
    return same;
}

/* ---- The sources. ---- */

/**
 * Make the file at `path` newer by its seconds than every artifact that
 * exists, so that a build that judges by time stamps makes them again;
 * 0, or -1.
 */
static int make_newer(const struct run *r, const char *path)
{
    struct timespec times[2];
    struct stat st;
    time_t newest = 0;
    size_t i;

    for (i = 0; i < r->p->nartifacts; i++) {
        char *a = expand(r, r->p->artifacts[i]);

        if (a != NULL && stat(a, &st) == 0 && st.st_mtime > newest)
            newest = st.st_mtime;
        free(a);
    }
    if (stat(path, &st) != 0)
        return -1;
    if (st.st_mtime > newest)
        return 0;
    times[0].tv_sec = times[1].tv_sec = newest + 1;
    times[0].tv_nsec = times[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path, times, 0);
}

/**
 * Replace source `s`'s file whole with `len` bytes of `text`, with its mode
 * and newer than every artifact, and make that its expected text.
 *
 * @return 0; 1 when the file read back differs; -1 for an error of the system.
 */
static int put_source(const struct run *r, struct src *s, const char *text, size_t len)
{
    size_t n = strlen(s->path) + sizeof(".etude-proof");
    char *part = malloc(n);
    int e;

    if (part == NULL)
        return -1;
    snprintf(part, n, "%s.etude-proof", s->path);
    unlink(part);
    if (write_new(part, text, len) != 0 || chmod(part, s->mode) != 0 ||
        rename(part, s->path) != 0) {
        e = errno;
        unlink(part);
        free(part);
        errno = e;
        return -1;
    }
    free(part);
    s->cur = text;
    s->curlen = len;
    if (make_newer(r, s->path) != 0)
        return -1;
    return has_text(s->path, text, len) ? 0 : 1;
}

/** Put every source back as it was saved; 0, 1 for one that differs, or 2. */
static int restore(const struct run *r)
{
    size_t i;
    int status = 0, w;

    for (i = 0; i < r->p->nsources; i++) {
        struct src *s = &r->src[i];

        if (!s->dirty)
            continue;
        w = put_source(r, s, s->saved, s->len);
        if (w < 0)
            status = sys_error(r, s->path, errno);
        else if (w > 0 && status == 0)
            status = fail(r, "the restoration of %s is incomplete", s->rel);
        else if (w == 0)
            s->dirty = 0;
        s->cur = s->saved;
        s->curlen = s->len;
    }
    return status;
}

/** Say whether every source has the text it must have now; 0, or 1. */
static int sources_kept(const struct run *r)
{
    size_t i;

    for (i = 0; i < r->p->nsources; i++)
        if (!has_text(r->src[i].path, r->src[i].cur, r->src[i].curlen))
            return fail(r, "%s changed while the build and the test ran", r->src[i].rel);
    return 0;
}

/** Save source `s` and, for a private proof, make its copy; 0, or -1. */
static int save_source(const struct run *r, struct src *s)
{
    char *saved = join(r->work, "saved");
    char *copy = saved != NULL ? join(saved, s->rel) : NULL;
    struct stat st;
    int status = -1;

    if (copy != NULL && stat(s->tree_path, &st) == 0 &&
        proof_slurp(s->tree_path, &s->saved, &s->len) == 0 && make_parents(copy) == 0 &&
        write_new(copy, s->saved, s->len) == 0) {
        s->mode = st.st_mode & 07777;
        s->cur = s->saved;
        s->curlen = s->len;
        status = 0;
        if (!r->p->shared)
            status = make_parents(s->path) == 0 && write_new(s->path, s->saved, s->len) == 0 &&
                             chmod(s->path, s->mode) == 0
                         ? 0
                         : -1;
    }
    free(copy);
    free(saved);
    return status;
}

/** Give each source its paths, save it and make its copy; 0, or 2. */
static int set_sources(struct run *r)
{
    size_t i;

    r->src = calloc(r->p->nsources, sizeof(*r->src));
    if (r->src == NULL)
        return sys_error(r, "the sources", ENOMEM);
    for (i = 0; i < r->p->nsources; i++) {
        struct src *s = &r->src[i];

        s->rel = r->p->sources[i];
        s->tree_path = join(r->o->tree, s->rel);
        s->path = join(r->srcroot, s->rel);
        if (s->tree_path == NULL || s->path == NULL || save_source(r, s) != 0)
            return sys_error(r, s->rel, s->tree_path == NULL || s->path == NULL ? ENOMEM : errno);
    }
    return 0;
}

/* ---- Commands. ---- */

/** Keep output `o` of command `what` in the work as PHASE.WHAT. */
static void keep_output(const struct run *r, const char *what, const struct etude_output *o)
{
    char name[160];
    char *path;

    snprintf(name, sizeof name, "%s.%s", r->phase, what);
    path = join(r->work, name);
    if (path != NULL) {
        unlink(path);
        write_new(path, o->data != NULL ? o->data : "", o->kept);
    }
    free(path);
}

/** Give a copy of output `o` from malloc, or NULL. */
static char *output_copy(const struct etude_output *o)
{
    char *c = malloc(o->kept + 1);

    if (c != NULL) {
        memcpy(c, o->data != NULL ? o->data : "", o->kept);
        c[o->kept] = '\0';
    }
    return c;
}

/** Fill `x` with what attempt `p` gave, and keep its output in the work. */
static void collect(const struct run *r, struct etude_proc *p, const char *what, struct ran *x)
{
    const struct etude_exec_result *res = etude_proc_result(p);
    char name[64];

    snprintf(name, sizeof name, "%s.out", what);
    keep_output(r, name, &res->out);
    snprintf(name, sizeof name, "%s.err", what);
    keep_output(r, name, &res->err);
    x->end = res->end;
    x->value = res->value;
    x->confirmed = res->cleanup == ETUDE_CLEANUP_CONFIRMED;
    x->truncated = res->out.total > res->out.kept || res->err.total > res->err.kept;
    x->out = output_copy(&res->out);
    x->err = output_copy(&res->err);
}

/**
 * Wait until attempt `p` finishes, cancelling it once it runs past
 * `bound_us` or a signal asks the proof to stop.
 */
static void await(const struct run *r, struct etude_proc *p, uint64_t bound_us, struct ran *x)
{
    uint64_t until = posix_now_us() + bound_us;
    int cancelled = 0;

    for (;;) {
        if (!cancelled && (*r->stop || posix_now_us() > until)) {
            x->bound = !*r->stop;
            etude_exec_cancel(p);
            cancelled = 1;
        }
        if (etude_exec_wait(r->ex, PROOF_TICK_MS) == p)
            return;
    }
}

/** Say whether `var` is one of the inherited variables dropped_env lists. */
static int dropped(const char *var)
{
    size_t i;

    for (i = 0; i < sizeof(dropped_env) / sizeof(dropped_env[0]); i++)
        if (strncmp(var, dropped_env[i], strlen(dropped_env[i])) == 0)
            return 1;
    return 0;
}

/**
 * Give the environment of a command: the driver's, with `TMPDIR` naming
 * `tmp` and the sanitizers' settings replaced by sanitizer_env, or NULL when
 * memory ran out. The array and its first entry, the `TMPDIR`, are from
 * malloc; env_free frees them.
 */
static char **command_env(const char *tmp)
{
    size_t n = 0, k = 1, i, len = strlen(tmp) + sizeof("TMPDIR=");
    size_t fixed = sizeof(sanitizer_env) / sizeof(sanitizer_env[0]);
    char **env;

    while (environ[n] != NULL)
        n++;
    env = calloc(n + fixed + 2, sizeof(*env));
    if (env == NULL || (env[0] = malloc(len)) == NULL) {
        free(env);
        return NULL;
    }
    snprintf(env[0], len, "TMPDIR=%s", tmp);
    for (i = 0; i < n; i++)
        if (!dropped(environ[i]))
            env[k++] = environ[i];
    for (i = 0; i < fixed; i++)
        env[k++] = (char *)sanitizer_env[i];
    return env;
}

/** Free an environment that command_env gave. */
static void env_free(char **env)
{
    if (env != NULL)
        free(env[0]);
    free(env);
}

/**
 * Run command `argv`, named `what`, in the tree's root under `bound_us`, with
 * a new directory as its `TMPDIR`, and say in `*x` how it ended.
 *
 * The directory is `WORK/PHASE.WHAT.tmp`. A build runs in `WORK/build.tmp`,
 * the same path in every phase, and the directory is renamed when the build
 * ends: a compiler may write the path of its scratch into what it makes, as
 * Apple's linker does in a debug map.
 *
 * @return 0, -1 with errno set when it did not start or its directory could
 *         not be renamed, or ETUDE_PROOF_STOPPED.
 */
static int run_command(const struct run *r, char *const *argv, uint64_t bound_us, const char *what,
                       struct ran *x)
{
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL;
    char **args = expand_list(r, argv), **env = NULL;
    char id[160], name[160], *tmp = NULL, *kept;
    int build = strcmp(what, "build") == 0, e = 0;

    memset(x, 0, sizeof(*x));
    snprintf(name, sizeof name, "%s.%s.tmp", r->phase, what);
    kept = join(r->work, name);
    if (kept != NULL)
        tmp = build ? join(r->work, "build.tmp") : text_copy(kept);
    if (tmp != NULL)
        env = command_env(tmp);
    if (args == NULL || env == NULL || kept == NULL)
        e = ENOMEM;
    else if (mkdir(tmp, 0700) != 0)
        e = errno;
    if (e == 0) {
        snprintf(id, sizeof id, "proof/%s/%s/%s", r->p->name, r->phase, what);
        memset(&spec, 0, sizeof(spec));
        spec.attempt_id = id;
        spec.argv = args;
        spec.envp = env;
        spec.cwd = r->o->tree;
        spec.output_max = PROOF_OUTPUT_MAX;
        spec.grace_us = PROOF_GRACE_US;
        if (etude_exec_start(r->ex, &spec, &p) != 0)
            e = errno != 0 ? errno : EIO;
    }
    list_free(args);
    env_free(env);
    if (e == 0) {
        await(r, p, bound_us, x);
        collect(r, p, what, x);
        etude_proc_free(p);
        if (build && rename(tmp, kept) != 0)
            e = errno;
    }
    free(tmp);
    free(kept);
    if (*r->stop && e == 0)
        return ETUDE_PROOF_STOPPED;
    errno = e;
    return e != 0 ? -1 : 0;
}

/** Free what a command gave. */
static void ran_free(struct ran *x)
{
    free(x->out);
    free(x->err);
}

/** Say why command `what` cannot be judged by its status, or give 0. */
static int ended_badly(const struct run *r, const struct ran *x, const char *what)
{
    if (x->bound)
        return fail(r, "the %s ran past its bound", what);
    if (!x->confirmed)
        return fail(r, "the %s's processes were not all seen to end", what);
    if (x->truncated)
        return fail(r, "the %s's output passed the %u bytes kept", what, PROOF_OUTPUT_MAX);
    if (x->end == ETUDE_END_SIGNALED)
        return fail(r, "the %s was killed by signal %d", what, x->value);
    if (x->end != ETUDE_END_EXITED || !x->out || !x->err)
        return fail(r, "the %s's end was not seen", what);
    return 0;
}

/* ---- Judging a phase. ---- */

/** Give the first line of `text` that begins with `prefix` and contains none
 * of `lines`, or that begins with it at all when `lines` is NULL. */
static const char *line_unlisted(const char *text, const char *prefix, char *const *lines, size_t n,
                                 size_t *len)
{
    size_t plen = strlen(prefix), i;
    const char *at, *end;

    for (at = text; *at != '\0'; at = *end != '\0' ? end + 1 : end) {
        int listed = 0;

        end = strchr(at, '\n');
        if (end == NULL)
            end = at + strlen(at);
        *len = (size_t)(end - at);
        if (*len < plen || strncmp(at, prefix, plen) != 0)
            continue;
        for (i = 0; i < n && !listed; i++)
            listed = strstr(at, lines[i]) != NULL && strstr(at, lines[i]) < end;
        if (!listed)
            return at;
    }
    return NULL;
}

/** Say whether the test's output `x` has a line that begins with the
 * failure prefix and is not one of `lines`, and report it; 0, or 1. */
static int unlisted(const struct run *r, const struct ran *x, char *const *lines, size_t n,
                    const char *why)
{
    const char *line;
    size_t len;

    if (r->p->fail_prefix == NULL)
        return 0;
    line = line_unlisted(x->out, r->p->fail_prefix, lines, n, &len);
    if (line == NULL)
        line = line_unlisted(x->err, r->p->fail_prefix, lines, n, &len);
    return line != NULL ? fail(r, "%s: %.*s", why, (int)len, line) : 0;
}

/** Judge the test of a base or restored phase, which must pass; 0, or 1. */
static int judge_pass(const struct run *r, const struct ran *x)
{
    if (ended_badly(r, x, "test"))
        return 1;
    if (x->value != 0)
        return fail(r, "the test exited %d", x->value);
    return unlisted(r, x, NULL, 0, "the test passed and printed a failure");
}

/** Judge the test of fault `f`, which must fail as `f` declares; 0, or 1. */
static int judge_fault(const struct run *r, const struct etude_fault *f, const struct ran *x)
{
    size_t i;

    if (ended_badly(r, x, "test"))
        return 1;
    if (x->value == 0)
        return fail(r, "the test passed with the fault");
    if (f->status != 0 && x->value != f->status)
        return fail(r, "the test exited %d, not %d", x->value, f->status);
    for (i = 0; i < f->nlines; i++)
        if (strstr(x->out, f->lines[i]) == NULL && strstr(x->err, f->lines[i]) == NULL)
            return fail(r, "the test's output has no line with: %s", f->lines[i]);
    if (f->only)
        return unlisted(r, x, f->lines, f->nlines,
                        "the test failed a check the fault does not name");
    return 0;
}

/**
 * Keep the phase's artifacts in the store and their digests, with the digest
 * each is compared by: macho_digest's for a Mach-O file, the store's for any
 * other; 0, 1 or 2.
 */
static int keep_artifacts(const struct run *r)
{
    size_t i;

    for (i = 0; i < r->p->nartifacts; i++) {
        char *path = expand(r, r->p->artifacts[i]);
        int e, m;

        if (path == NULL)
            return sys_error(r, "an artifact's path", ENOMEM);
        if (etude_store_put(r->store, path, NULL, r->dig[i], NULL) != 0) {
            e = errno;
            if (e == ENOENT)
                fail(r, "the build made no %s", path);
            else
                sys_error(r, path, e);
            free(path);
            return e == ENOENT ? 1 : 2;
        }
        m = macho_digest(path, r->cmp[i]);
        if (m < 0) {
            sys_error(r, path, errno);
            free(path);
            return 2;
        }
        if (m == 1)
            memcpy(r->cmp[i], r->dig[i], sizeof(r->cmp[i]));
        free(path);
    }
    return 0;
}

/** Judge the phase's artifacts against the base's by `kind`; 0, or 1. */
static int judge_artifacts(const struct run *r, enum kind kind)
{
    size_t i, same = 0;

    for (i = 0; i < r->p->nartifacts; i++) {
        if (kind == BASE)
            memcpy(r->base[i], r->cmp[i], sizeof(r->base[i]));
        else if (strcmp(r->base[i], r->cmp[i]) == 0)
            same++;
        else if (kind == RESTORED)
            return fail(r, "the restored %s differs from the base's", r->p->artifacts[i]);
    }
    if (kind == FAULT && same == r->p->nartifacts)
        return fail(r, "the artifacts are the base's, so the fault did not reach them");
    return 0;
}

/** Print the phase's outcome with its artifacts' digests, shortened. */
static void report(const struct run *r, const char *what)
{
    size_t i;

    fprintf(r->o->log, "etude-proof: %s: %s: artifacts", r->p->name, r->phase);
    for (i = 0; i < r->p->nartifacts; i++)
        fprintf(r->o->log, " %.12s", r->dig[i]);
    fprintf(r->o->log, "; %s\n", what);
}

/**
 * Run one phase: build, keep and judge the artifacts, run and judge the
 * test, and require the sources to be as the phase left them.
 *
 * @return 0, 1 for a proof that fails, 2, or ETUDE_PROOF_STOPPED.
 */
static int phase(const struct run *r, enum kind kind, const struct etude_fault *f)
{
    struct ran x;
    int status;

    status = run_command(r, r->p->build, r->p->build_bound_us, "build", &x);
    if (status == 0 && (status = ended_badly(r, &x, "build")) == 0 && x.value != 0)
        status = fail(r, "the build exited %d", x.value);
    ran_free(&x);
    if (status == 0 && (status = keep_artifacts(r)) == 0)
        status = judge_artifacts(r, kind);
    if (status == 0) {
        status = run_command(r, r->p->test, r->p->test_bound_us, "test", &x);
        if (status == 0)
            status = kind == FAULT ? judge_fault(r, f, &x) : judge_pass(r, &x);
        ran_free(&x);
    }
    if (status == 0)
        status = sources_kept(r);
    if (status < 0)
        return sys_error(r, "a command did not start, or its TMPDIR was not renamed", errno);
    if (status == 0)
        report(r, kind == FAULT ? "the test failed as the fault declares" : "the test passed");
    return status;
}

/* ---- Faults. ---- */

/** Give the index of source `rel`. */
static size_t source_index(const struct run *r, const char *rel)
{
    size_t i;

    for (i = 0; i < r->p->nsources; i++)
        if (strcmp(r->p->sources[i], rel) == 0)
            break;
    return i;
}

/** Make the texts of fault `f` in `texts` and `lens`; 0, 1 or 2. */
static int fault_texts(const struct run *r, const struct etude_fault *f, char **texts, size_t *lens)
{
    size_t i, j, count, len;
    char *next;

    for (i = 0; i < f->nedits; i++) {
        const struct etude_edit *e = &f->edit[i];

        j = source_index(r, e->file);
        count = etude_edit_apply(texts[j] != NULL ? texts[j] : r->src[j].saved,
                                 texts[j] != NULL ? lens[j] : r->src[j].len, e, &next, &len);
        if (count == 1 && next == NULL)
            return sys_error(r, e->file, ENOMEM);
        if (count != 1)
            return fail(r, "edit %zu finds its text %zu times in %s", i + 1, count, e->file);
        free(texts[j]);
        texts[j] = next;
        lens[j] = len;
    }
    return 0;
}

/** Write the faulty texts into the sources; 0, or 2. */
static int put_fault(const struct run *r, char **texts, const size_t *lens)
{
    size_t i;

    for (i = 0; i < r->p->nsources; i++) {
        if (texts[i] == NULL)
            continue;
        r->src[i].dirty = 1;
        if (put_source(r, &r->src[i], texts[i], lens[i]) != 0)
            return sys_error(r, r->src[i].rel, errno);
    }
    return 0;
}

/** Prove fault `f`, then restore the sources and prove them restored. */
static int prove_fault(struct run *r, const struct etude_fault *f)
{
    char **texts = calloc(r->p->nsources, sizeof(*texts));
    size_t *lens = calloc(r->p->nsources, sizeof(*lens)), i;
    int status, back;

    snprintf(r->phase, sizeof r->phase, "fault-%s", f->name);
    if (texts == NULL || lens == NULL)
        status = sys_error(r, "the fault's texts", ENOMEM);
    else if ((status = fault_texts(r, f, texts, lens)) == 0 &&
             (status = put_fault(r, texts, lens)) == 0)
        status = phase(r, FAULT, f);
    snprintf(r->phase, sizeof r->phase, "restored-%s", f->name);
    back = restore(r);
    if (status == 0 && back == 0 && !*r->stop)
        status = phase(r, RESTORED, NULL);
    for (i = 0; texts != NULL && i < r->p->nsources; i++)
        free(texts[i]);
    free(texts);
    free(lens);
    if (*r->stop)
        return ETUDE_PROOF_STOPPED;
    return status != 0 ? status : back;
}

/** Say whether fault `name` is among those `o` asks for. */
static int selected(const struct etude_proof_opts *o, const char *name)
{
    size_t i;

    for (i = 0; o->only != NULL && o->only[i] != NULL; i++)
        if (strcmp(o->only[i], name) == 0)
            return 1;
    return o->only == NULL;
}

/** Say whether each fault `o` asks for is the proof's; 0, or 2. */
static int check_selection(const struct run *r)
{
    size_t i, j;

    for (i = 0; r->o->only != NULL && r->o->only[i] != NULL; i++) {
        for (j = 0; j < r->p->nfaults; j++)
            if (strcmp(r->p->faults[j].name, r->o->only[i]) == 0)
                break;
        if (j == r->p->nfaults) {
            fprintf(r->o->log, "etude-proof: %s: no fault %s\n", r->p->name, r->o->only[i]);
            return 2;
        }
    }
    return 0;
}

/* ---- The run. ---- */

/**
 * Say whether the tree's run lock is held for this process: the lock's owner
 * file names the process RACH2_TREE_HELD names, and that process runs.
 */
static int tree_held(const struct run *r)
{
    const char *held = getenv("RACH2_TREE_HELD");
    char *path = join(r->o->tree, "build/.run.lock/owner"), *text = NULL, *line;
    long pid = held != NULL ? strtol(held, NULL, 10) : 0;
    size_t len;
    int ok = 0;

    if (pid > 0 && path != NULL && proof_slurp(path, &text, &len) == 0)
        for (line = text; line != NULL && !ok; line = strchr(line, '\n')) {
            line += *line == '\n';
            ok = strncmp(line, "pid ", 4) == 0 && strtol(line + 4, NULL, 10) == pid;
        }
    free(text);
    free(path);
    return ok && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
}

/**
 * Make the work directory, open the store and the executor; 0, or 2.
 *
 * The work's path is made absolute, since `${WORK}` and each command's
 * `TMPDIR` must name it from the tree's root and from any directory a command
 * moves to.
 */
static int open_run(struct run *r)
{
    const char *tmp = getenv("TMPDIR");
    char *store;
    size_t n;

    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
    n = strlen(tmp) + strlen(r->p->name) + sizeof("/rach2-proof-.XXXXXX");
    if (r->o->work != NULL)
        r->work = mkdir(r->o->work, 0700) == 0 ? text_copy(r->o->work) : NULL;
    else if ((r->work = malloc(n)) != NULL) {
        snprintf(r->work, n, "%s/rach2-proof-%s.XXXXXX", tmp, r->p->name);
        if (mkdtemp(r->work) == NULL) {
            free(r->work);
            r->work = NULL;
        }
    }
    if (r->work != NULL && r->work[0] != '/') {
        char *made = r->work;

        r->work = realpath(made, NULL);
        if (r->work == NULL) {
            int e = errno;

            rmdir(made);
            errno = e;
        }
        free(made);
    }
    if (r->work == NULL)
        return sys_error(r, "the work directory", errno);
    r->srcroot = r->p->shared ? text_copy(r->o->tree) : join(r->work, "src");
    store = r->o->store != NULL ? text_copy(r->o->store) : join(r->work, "store");
    r->store = store != NULL ? etude_store_open(store) : NULL;
    free(store);
    r->ex = etude_exec_new();
    r->base = calloc(r->p->nartifacts, sizeof(*r->base));
    r->dig = calloc(r->p->nartifacts, sizeof(*r->dig));
    r->cmp = calloc(r->p->nartifacts, sizeof(*r->cmp));
    if (r->srcroot == NULL || r->store == NULL || r->ex == NULL || r->base == NULL ||
        r->dig == NULL || r->cmp == NULL)
        return sys_error(r, "the run", errno != 0 ? errno : ENOMEM);
    fprintf(r->o->log, "etude-proof: %s: saved: %s/saved\n", r->p->name, r->work);
    return 0;
}

/** Require a private proof's tree to be as it was saved; 0, or 1. */
static int tree_kept(const struct run *r)
{
    size_t i;

    for (i = 0; !r->p->shared && i < r->p->nsources; i++)
        if (!has_text(r->src[i].tree_path, r->src[i].saved, r->src[i].len))
            return fail(r, "the tree's %s changed while the proof ran", r->src[i].rel);
    return 0;
}

/** Free what the run made, and remove or keep its work after `status`. */
static void close_run(struct run *r, int status)
{
    size_t i;

    for (i = 0; r->src != NULL && i < r->p->nsources; i++) {
        free(r->src[i].path);
        free(r->src[i].tree_path);
        free(r->src[i].saved);
    }
    free(r->src);
    if (r->ex != NULL)
        etude_exec_free(r->ex);
    if (r->store != NULL)
        etude_store_close(r->store);
    if (r->work != NULL && ((status == 0 && !r->o->keep) || status == ETUDE_PROOF_STOPPED)) {
        if (remove_tree(r->work) != 0)
            fprintf(r->o->log, "etude-proof: not removed: %s: %s\n", r->work, strerror(errno));
    } else if (r->work != NULL)
        fprintf(r->o->log, "etude-proof: evidence: %s\n", r->work);
    free(r->work);
    free(r->srcroot);
    free(r->base);
    free(r->dig);
    free(r->cmp);
}

/** Run the base phase and each fault asked for; 0, 1, 2 or stopped. */
static int run_phases(struct run *r)
{
    size_t i, proved = 0;
    int status;

    snprintf(r->phase, sizeof r->phase, "base");
    status = phase(r, BASE, NULL);
    for (i = 0; status == 0 && !r->o->base && i < r->p->nfaults; i++)
        if (selected(r->o, r->p->faults[i].name)) {
            status = prove_fault(r, &r->p->faults[i]);
            proved++;
        }
    if (status == 0) {
        snprintf(r->phase, sizeof r->phase, "end");
        status = tree_kept(r);
    }
    if (status == 0 && r->o->base)
        fprintf(r->o->log, "etude-proof: result: pass: %s: the base passed; no fault was run\n",
                r->p->name);
    else if (status == 0)
        fprintf(r->o->log,
                "etude-proof: result: pass: %s: %zu of %zu faults found; the sources and "
                "artifacts restored\n",
                r->p->name, proved, r->p->nfaults);
    return status;
}

int etude_proof_run(const struct etude_proof *p, const struct etude_proof_opts *o)
{
    struct run r;
    int status, back;

    memset(&r, 0, sizeof(r));
    r.p = p;
    r.o = o;
    r.stop = o->stop != NULL ? o->stop : &never;
    snprintf(r.phase, sizeof r.phase, "start");
    if (check_selection(&r) != 0)
        return 2;
    if (p->shared && !tree_held(&r)) {
        fprintf(o->log, "etude-proof: %s: the tree's run lock is not held for the proof\n",
                p->name);
        return 2;
    }
    status = open_run(&r);
    if (status == 0)
        status = set_sources(&r);
    if (status == 0)
        status = run_phases(&r);
    if (r.src != NULL && (back = restore(&r)) != 0 && status != ETUDE_PROOF_STOPPED)
        status = back;
    if (*r.stop)
        status = ETUDE_PROOF_STOPPED;
    close_run(&r, status);
    return status;
}
