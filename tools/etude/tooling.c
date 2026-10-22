/*
 * tooling.c: the local Linux tooling machine. It makes the machine from the
 * recipe in tools/etude/tooling, runs jobs in private copies of it under
 * QEMU, measures each boot, and writes the boot's steps to the history.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "../../utils/sha256.h"
#include "etude/tooling.h"

/* How often a boot's channel is read and QEMU sampled, in milliseconds. */
#define POLL_MS 50

/* The bytes kept of a program's standard output and error. */
#define OUTPUT_MAX 65536

/* The largest console or channel kept as evidence, and the largest output
 * of a job read back. */
#define EVIDENCE_MAX   (16u << 20)
#define JOB_OUTPUT_MAX (256u << 20)

/* The size of the made machine's disk, which the first boot grows its root
 * file system to fill. */
#define DISK_SIZE "16G"

#define PATH_LEN 4096

#ifdef _WIN32
#define PATH_SEP  ';'
#define getpid    _getpid
#define is_sep(c) ((c) == '/' || (c) == '\\')
#define name_cmp  _strnicmp
#else
#define PATH_SEP  ':'
#define is_sep(c) ((c) == '/')
#define name_cmp  strncmp
#endif

/* The methods of a step's processor time and memory, which are QEMU's: on
 * Windows its job's time and its working set, elsewhere its process's time
 * and resident set; and what the scope of a step says of them. */
#ifdef _WIN32
#define METHOD_CPU    "tooling-qemu-job-cpu-win32-v1"
#define METHOD_MEMORY "tooling-qemu-working-set-win32-v1"
#define ACCOUNTING    "the step's processor time is QEMU's job's and its memory QEMU's working set"
#else
#define METHOD_CPU    "tooling-qemu-cpu-v1"
#define METHOD_MEMORY "tooling-qemu-rss-v1"
#define ACCOUNTING    "the step's processor time and memory are QEMU's process"
#endif

/* A growing string. */
struct text {
    char *data;
    size_t len, cap;
    int failed;
};

/** Append formatted text to `t`; a failure is kept in `t->failed`. */
static void tprintf(struct text *t, const char *format, ...)
{
    va_list ap;
    int n;

    if (t->failed)
        return;
    va_start(ap, format);
    n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (n < 0) {
        t->failed = 1;
        return;
    }
    if (t->len + (size_t)n + 1 > t->cap) {
        size_t cap = 2 * (t->len + (size_t)n + 1);
        char *d = realloc(t->data, cap);

        if (d == NULL) {
            t->failed = 1;
            return;
        }
        t->data = d;
        t->cap = cap;
    }
    va_start(ap, format);
    vsnprintf(t->data + t->len, t->cap - t->len, format, ap);
    va_end(ap);
    t->len += (size_t)n;
}

/** Append `s` to `t` as a JSON string. */
static void tstring(struct text *t, const char *s)
{
    const unsigned char *c;

    tprintf(t, "\"");
    for (c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\')
            tprintf(t, "\\%c", *c);
        else if (*c < 0x20 || *c > 0x7e)
            tprintf(t, "\\u%04x", *c);
        else
            tprintf(t, "%c", *c);
    }
    tprintf(t, "\"");
}

/** Format into `buf`, and say whether it fitted. */
static int fmt(char *buf, size_t size, const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(buf, size, format, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

/** Set `why` from a format, and give -1. */
static int failure(char *why, size_t size, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsnprintf(why, size, format, ap);
    va_end(ap);
    return -1;
}

/** Read a whole file of at most `max` bytes; NULL when it cannot be. */
static char *slurp(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *d = NULL, *nd;
    size_t n = 0, cap = 0, got;

    if (f == NULL)
        return NULL;
    for (;;) {
        if (n + 65536 + 1 > cap) {
            cap = 2 * cap + 65536 + 1;
            nd = realloc(d, cap);
            if (nd == NULL)
                break;
            d = nd;
        }
        got = fread(d + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0 || n > max)
            break;
    }
    if (ferror(f) || n > max || d == NULL || n + 1 > cap) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    d[n] = '\0';
    *len = n;
    return d;
}

/** Give the SHA-256 of a file in `hex`; -1 when it cannot be read. */
static int file_sha256(const char *path, char hex[65])
{
    unsigned char buf[65536], digest[32];
    struct sha256 s;
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
        return -1;
    sha256_init(&s);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&s, buf, n);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    return 0;
}

/** Say whether `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/** Give the bytes of storage `path` uses, or its size where that is unknown. */
static uint64_t used_bytes(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (uint64_t)st.st_size;
#else
    return (uint64_t)st.st_blocks * 512u;
#endif
}

/** Make directory `path`; 0 when it was made, -1 with errno otherwise. */
static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

/** Make `path` and the directories above it that are missing. */
static int make_dirs(const char *path)
{
    char buf[PATH_LEN], *p;

    if (fmt(buf, sizeof(buf), "%s", path) != 0)
        return -1;
    for (p = buf + 1; *p; p++) {
        char sep = *p;

        if (!is_sep(sep))
            continue;
        *p = '\0';
        if (make_dir(buf) != 0 && errno != EEXIST && !exists(buf))
            return -1;
        *p = sep;
    }
    return make_dir(buf) != 0 && errno != EEXIST ? -1 : 0;
}

/** Make a new directory of this process's, CACHE/PREFIX-PID-N, in `out`. */
static int private_dir(const char *cache, const char *prefix, char *out, size_t size)
{
    unsigned n;

    for (n = 0; n < 1000; n++) {
        if (fmt(out, size, "%s/%s-%ld-%u", cache, prefix, (long)getpid(), n) != 0)
            return -1;
        if (make_dir(out) == 0)
            return 0;
        if (errno != EEXIST)
            return -1;
    }
    return -1;
}

/** Remove the files named `names`, then directory `dir` itself. */
static int remove_dir(const char *dir, const char *const *names)
{
    char path[PATH_LEN];
    int rc = 0;

    for (; *names != NULL; names++) {
        if (fmt(path, sizeof(path), "%s/%s", dir, *names) != 0)
            return -1;
        if (remove(path) != 0 && errno != ENOENT)
            rc = -1;
    }
#ifdef _WIN32
    if (_rmdir(dir) != 0)
#else
    if (rmdir(dir) != 0)
#endif
        rc = -1;
    return rc;
}

/** Make `path` read-only. */
static int read_only(const char *path)
{
#ifdef _WIN32
    return _chmod(path, _S_IREAD);
#else
    return chmod(path, 0444);
#endif
}

/**
 * Find the value of environment variable `name` in `envp`, whose names
 * Windows compares without regard to case, as its `Path` shows.
 */
static const char *env_get(char *const *envp, const char *name)
{
    size_t n = strlen(name);

    for (; envp != NULL && *envp != NULL; envp++)
        if (name_cmp(*envp, name, n) == 0 && (*envp)[n] == '=')
            return *envp + n + 1;
    return NULL;
}

/**
 * Find program `name` on the PATH of `envp`, as the executor needs a path:
 * a name with a slash is taken as it is.
 */
static int find_program(char *const *envp, const char *name, char *out, size_t size)
{
    const char *path = env_get(envp, "PATH"), *p, *end;
    char dir[PATH_LEN];

    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
        return fmt(out, size, "%s", name);
    for (p = path; p != NULL && *p != '\0'; p = *end ? end + 1 : end) {
        end = strchr(p, PATH_SEP);
        if (end == NULL)
            end = p + strlen(p);
        if (end == p || (size_t)(end - p) >= sizeof(dir))
            continue;
        memcpy(dir, p, (size_t)(end - p));
        dir[end - p] = '\0';
#ifdef _WIN32
        if (fmt(out, size, "%s/%s.exe", dir, name) == 0 && exists(out))
            return 0;
#else
        if (fmt(out, size, "%s/%s", dir, name) == 0 && access(out, X_OK) == 0)
            return 0;
#endif
    }
    return -1;
}

/** Say whether a signal has asked the boot to stop. */
static int stopped(const struct etude_tooling_site *site)
{
    return site->stop != NULL && *site->stop != 0;
}

/**
 * Run a program to its end, or until a signal asks the boot to stop.
 * `argv[0]` is a path with a directory, or a name found on the PATH of
 * `site->envp`.
 *
 * @return its exit status, or -1 when it did not run, did not exit or was
 *         stopped, with `why` saying so; a status other than 0 also sets
 *         `why`, with the program's standard error.
 */
static int run_program(const struct etude_tooling_site *site, char *const *argv, char *why,
                       size_t whylen)
{
    struct etude_exec *ex;
    struct etude_exec_spec spec;
    struct etude_proc *p, *done;
    const struct etude_exec_result *r;
    char path[PATH_LEN], **args;
    const char *name = argv[0];
    size_t n;
    int status = -1;

    if (find_program(site->envp, name, path, sizeof(path)) != 0)
        return failure(why, whylen, "%s is not found on the PATH", name);
    for (n = 0; argv[n] != NULL; n++)
        ;
    if ((args = malloc((n + 1) * sizeof(*args))) == NULL)
        return failure(why, whylen, "out of memory");
    memcpy(args, argv, (n + 1) * sizeof(*args));
    if ((ex = etude_exec_new()) == NULL) {
        free(args);
        return failure(why, whylen, "out of memory");
    }
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = name;
    args[0] = path;
    spec.argv = args;
    spec.envp = site->envp;
    spec.output_max = OUTPUT_MAX;
    spec.grace_us = 5000000;
    if (etude_exec_start(ex, &spec, &p) != 0) {
        failure(why, whylen, "%s does not start: %s", name, strerror(errno));
        etude_exec_free(ex);
        free(args);
        return -1;
    }
    while ((done = etude_exec_wait(ex, POLL_MS)) != p)
        if (done == NULL && stopped(site))
            etude_exec_cancel(p);
    r = etude_proc_result(p);
    if (r->end == ETUDE_END_EXITED)
        status = r->value;
    if (r->end == ETUDE_END_STOPPED)
        failure(why, whylen, "%s was stopped by a signal", name);
    else if (status != 0)
        failure(why, whylen, "%s %s %d: %.300s", name,
                r->end == ETUDE_END_EXITED ? "exited with status" : "ended by signal", r->value,
                r->err.data != NULL ? r->err.data : "");
    etude_proc_free(p);
    etude_exec_free(ex);
    free(args);
    return status;
}

/* The arguments of a program being made, each a string of its own. */
struct args {
    char **v; /* ended by NULL */
    size_t n, cap;
    int failed;
};

/** Append a formatted argument to `a`; a failure is kept in `a->failed`. */
static void add_arg(struct args *a, const char *format, ...)
{
    va_list ap;
    char *s;
    int n;

    if (a->failed)
        return;
    if (a->n + 2 > a->cap) {
        size_t cap = 2 * a->cap + 16;
        char **v = realloc(a->v, cap * sizeof(*v));

        if (v == NULL) {
            a->failed = 1;
            return;
        }
        a->v = v;
        a->cap = cap;
    }
    va_start(ap, format);
    n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (n < 0 || (s = malloc((size_t)n + 1)) == NULL) {
        a->failed = 1;
        return;
    }
    va_start(ap, format);
    vsnprintf(s, (size_t)n + 1, format, ap);
    va_end(ap);
    a->v[a->n++] = s;
    a->v[a->n] = NULL;
}

/** Run the program of arguments `a`, as run_program does, and free them. */
static int run_args(const struct etude_tooling_site *site, struct args *a, char *why, size_t whylen)
{
    int rc =
        a->failed ? failure(why, whylen, "out of memory") : run_program(site, a->v, why, whylen);

    while (a->n > 0)
        free(a->v[--a->n]);
    free(a->v);
    return rc;
}

/** Double each comma of `s` into `out`, as QEMU's option values need. */
static int qemu_value(const char *s, char *out, size_t size)
{
    size_t n = 0;

    for (; *s; s++) {
        if (n + 3 > size)
            return -1;
        out[n++] = *s;
        if (*s == ',')
            out[n++] = ',';
    }
    out[n] = '\0';
    return 0;
}

int etude_tooling_machine(const struct etude_tooling_site *site, struct etude_tooling_machine *m,
                          char *why, size_t whylen)
{
    static const char *const files[] = ETUDE_TOOLING_RECIPE;
    const char *arch = site->arch != NULL && *site->arch != '\0' ? site->arch : "amd64";
    char path[PATH_LEN], line[2048], *bytes;
    unsigned char digest[32];
    struct sha256 s;
    size_t i, len;
    FILE *f;
    int found = 0;

    memset(m, 0, sizeof(*m));
    if (strcmp(arch, "amd64") != 0 && strcmp(arch, "arm64") != 0)
        return failure(why, whylen, "the machine's architecture is %s, not amd64 or arm64", arch);
    if (fmt(path, sizeof(path), "%s/image", site->recipe) != 0 || (f = fopen(path, "r")) == NULL)
        return failure(why, whylen, "the recipe has no image file: %s", path);
    while (fgets(line, sizeof(line), f) != NULL) {
        struct etude_tooling_machine l;

        memset(&l, 0, sizeof(l));
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%15s %1023s %64s", l.arch, l.url, l.image_sha256) != 3 ||
            strlen(l.image_sha256) != 64 || strspn(l.image_sha256, "0123456789abcdef") != 64) {
            fclose(f);
            return failure(why, whylen, "%s: the image line is not ARCH URL SHA256", path);
        }
        if (strcmp(l.arch, arch) != 0)
            continue;
        if (found) {
            fclose(f);
            return failure(why, whylen, "%s names two images for %s", path, arch);
        }
        memcpy(m->arch, l.arch, sizeof(m->arch));
        memcpy(m->url, l.url, sizeof(m->url));
        memcpy(m->image_sha256, l.image_sha256, sizeof(m->image_sha256));
        found = 1;
    }
    fclose(f);
    if (!found)
        return failure(why, whylen, "%s names no image for %s", path, arch);
    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)"etude-tooling 2\n", 16);
    snprintf(line, sizeof(line), "arch %s\n", m->arch);
    sha256_update(&s, (const unsigned char *)line, strlen(line));
    for (i = 0; files[i] != NULL; i++) {
        if (fmt(path, sizeof(path), "%s/%s", site->recipe, files[i]) != 0 ||
            (bytes = slurp(path, 1u << 20, &len)) == NULL)
            return failure(why, whylen, "the recipe's %s cannot be read", files[i]);
        snprintf(line, sizeof(line), "%s %lu\n", files[i], (unsigned long)len);
        sha256_update(&s, (const unsigned char *)line, strlen(line));
        sha256_update(&s, (const unsigned char *)bytes, len);
        free(bytes);
    }
    sha256_final(&s, digest);
    sha256_hex(digest, m->key);
    if (fmt(m->image, sizeof(m->image), "%s/image-%s.img", site->cache, m->image_sha256) != 0 ||
        fmt(m->disk, sizeof(m->disk), "%s/tooling-%s.qcow2", site->cache, m->key) != 0 ||
        fmt(m->tools, sizeof(m->tools), "%s/tooling-%s.tools", site->cache, m->key) != 0)
        return failure(why, whylen, "the cache's path is too long");
    return 0;
}

/* A boot of the machine: QEMU under the executor, its channel, and the
 * samples of its usage. */
struct boot {
    struct etude_exec *ex;
    struct etude_proc *p;
    char channel[PATH_LEN];
    long offset;     /* channel bytes read */
    char line[1024]; /* the channel's line not yet whole */
    size_t linelen;
    struct etude_tooling_cost *cost;
    unsigned step;      /* the step in progress */
    uint64_t step_cpu0; /* QEMU's processor time when it began */
    int step_cpu_ok;    /* every sample of the step was taken */
    uint64_t last_us, gap_us, cpu, rss;
    int sampled;
    /* What the channel said. */
    int started, ran, ended, status, provisioned;
    char failed[512];
};

/** Give a sampled measurement, or an unavailable one with `reason`. */
static struct etude_measurement sampled(int ok, uint64_t value, uint64_t interval_us,
                                        const char *method, const char *reason)
{
    struct etude_measurement m;

    memset(&m, 0, sizeof(m));
    if (!ok) {
        m.reason = reason;
        return m;
    }
    m.value = value;
    m.method = method;
    m.quality = "sampled";
    m.interval_us = interval_us > 0 ? interval_us : 1;
    return m;
}

/** Sample QEMU's processor time and resident memory, where the platform can. */
static void sample(struct boot *b)
{
    struct etude_reading now;
    uint64_t cpu, rss;
    int rc = etude_proc_sample(b->p, &cpu, &rss);

    if (etude_history_now(&now) != 0)
        return;
    if (b->sampled && now.monotonic_us - b->last_us > b->gap_us)
        b->gap_us = now.monotonic_us - b->last_us;
    b->last_us = now.monotonic_us;
    if (rc != 1) {
        b->step_cpu_ok = 0;
        return;
    }
    b->rss = rss;
    b->cpu = cpu;
    b->sampled = 1;
}

/** Note a sample's peak in the step in progress. */
static void step_peak(struct boot *b)
{
    struct etude_tooling_step *s = &b->cost->steps[b->step];

    if (!b->sampled)
        return;
    if (s->peak_bytes.reason != NULL || s->peak_bytes.method == NULL ||
        b->rss > s->peak_bytes.value)
        s->peak_bytes = sampled(1, b->rss, b->gap_us, METHOD_MEMORY, NULL);
}

/** Begin step `i` now, at QEMU's processor time of the last sample. */
static void step_begin(struct boot *b, unsigned i, const struct etude_reading *now)
{
    struct etude_tooling_step *s = &b->cost->steps[i];

    b->step = i;
    s->begun = 1;
    s->from = *now;
    s->peak_bytes = sampled(0, 0, 0, NULL, "QEMU's resident memory was not sampled in the step");
    s->cpu_us = sampled(0, 0, 0, NULL,
                        "this platform gives no reading of a running process's processor time");
    b->step_cpu0 = b->sampled ? b->cpu : 0;
    b->step_cpu_ok = b->sampled || i == 0;
    b->gap_us = 0;
    if (b->cost->nsteps < i + 1)
        b->cost->nsteps = i + 1;
}

/**
 * End the step in progress now, with QEMU's processor time `cpu`; `done`
 * says that its witness was seen.
 */
static void step_end(struct boot *b, const struct etude_reading *now, int cpu_ok, uint64_t cpu,
                     int done)
{
    struct etude_tooling_step *s = &b->cost->steps[b->step];

    s->ended = 1;
    s->done = done;
    s->to = *now;
    step_peak(b);
    if (cpu_ok && b->step_cpu_ok && cpu >= b->step_cpu0)
        s->cpu_us = sampled(1, cpu - b->step_cpu0, b->gap_us > 0 ? b->gap_us : POLL_MS * 1000u,
                            METHOD_CPU, NULL);
}

/** Act on one whole line of the channel. */
static void channel_line(struct boot *b, const char *line, const struct etude_reading *now)
{
    int n;

    if (strcmp(line, "etude-tooling: provision: ok") == 0)
        b->provisioned = 1;
    else if (strncmp(line, "etude-tooling: provision: failed: ", 34) == 0)
        failure(b->failed, sizeof(b->failed), "provision.sh: %s", line + 34);
    else if (strcmp(line, "etude-job: start") == 0 && !b->started) {
        b->started = 1;
        step_end(b, now, b->sampled, b->cpu, 1);
        step_begin(b, ETUDE_TOOLING_JOB, now);
    } else if (sscanf(line, "etude-job: ran %d", &n) == 1 && b->started && !b->ran) {
        b->ran = 1;
        b->status = n;
        step_end(b, now, b->sampled, b->cpu, 1);
        step_begin(b, ETUDE_TOOLING_END, now);
    } else if (sscanf(line, "etude-job: end %d", &n) == 1 && b->ran && n == b->status)
        b->ended = 1;
    else if (strncmp(line, "etude-job: failed: ", 19) == 0)
        failure(b->failed, sizeof(b->failed), "the job runner: %s", line + 19);
}

/** Read what the channel has written since the last read. */
static void read_channel(struct boot *b)
{
    struct etude_reading now;
    char buf[4096];
    size_t n, i;
    FILE *f = fopen(b->channel, "rb");

    if (f == NULL || fseek(f, b->offset, SEEK_SET) != 0) {
        if (f != NULL)
            fclose(f);
        return;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0 || etude_history_now(&now) != 0)
        return;
    b->offset += (long)n;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\r')
            continue;
        if (buf[i] != '\n') {
            if (b->linelen + 1 < sizeof(b->line))
                b->line[b->linelen++] = buf[i];
            continue;
        }
        b->line[b->linelen] = '\0';
        channel_line(b, b->line, &now);
        b->linelen = 0;
    }
}

/* How a boot ended: by itself, at its deadline, or by a signal's request. */
enum { BOOT_ENDED, BOOT_LATE, BOOT_STOPPED };

/**
 * Boot QEMU with `argv` and serve it until it ends: read its channel, sample
 * it, and stop it when `deadline_us` passes, unless it is 0, or a signal
 * asks it to stop.
 *
 * @return BOOT_ENDED, BOOT_LATE or BOOT_STOPPED, or -1 when it did not
 *         start.
 */
static int boot_run(struct boot *b, const struct etude_tooling_site *site, char **argv,
                    uint64_t deadline_us, char *why, size_t whylen)
{
    struct etude_exec_spec spec;
    struct etude_reading now;
    const struct etude_exec_result *r;
    uint64_t begin;
    int end = BOOT_ENDED;

    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = "tooling";
    spec.argv = argv;
    spec.envp = site->envp;
    spec.output_max = OUTPUT_MAX;
    spec.grace_us = 10000000;
    if ((b->ex = etude_exec_new()) == NULL)
        return failure(why, whylen, "out of memory");
    if (etude_history_now(&now) != 0)
        return failure(why, whylen, "the clock cannot be read");
    begin = now.monotonic_us;
    if (etude_exec_start(b->ex, &spec, &b->p) != 0)
        return failure(why, whylen, "QEMU does not start: %s", strerror(errno));
    b->cost->qemu = etude_proc_result(b->p)->process;
    for (;;) {
        struct etude_proc *done = etude_exec_wait(b->ex, POLL_MS);

        if (done == NULL || done == b->p) {
            if (done == NULL)
                sample(b);
            read_channel(b);
            step_peak(b);
        }
        if (done == b->p)
            break;
        if (end != BOOT_ENDED)
            continue;
        if (stopped(site))
            end = BOOT_STOPPED;
        else if (deadline_us != 0 && etude_history_now(&now) == 0 &&
                 now.monotonic_us - begin > deadline_us)
            end = BOOT_LATE;
        else
            continue;
        etude_exec_cancel(b->p);
    }
    r = etude_proc_result(b->p);
    b->cost->cpu_us = r->cpu_us;
    b->cost->peak_bytes = r->peak_bytes;
    b->cost->cleanup = r->cleanup;
    if (end == BOOT_ENDED && r->end == ETUDE_END_EXITED && r->value != 0 && b->failed[0] == '\0')
        failure(b->failed, sizeof(b->failed), "QEMU exited with status %d: %.300s", r->value,
                r->err.data != NULL ? r->err.data : "");
    else if (end == BOOT_ENDED && r->end != ETUDE_END_EXITED && b->failed[0] == '\0')
        failure(b->failed, sizeof(b->failed), "QEMU ended by signal %d", r->value);
    return end;
}

/** Free a boot's executor. */
static void boot_free(struct boot *b)
{
    if (b->p != NULL)
        etude_proc_free(b->p);
    if (b->ex != NULL)
        etude_exec_free(b->ex);
    b->p = NULL;
    b->ex = NULL;
}

/* QEMU's command line, with its strings. */
struct line {
    char *argv[64];
    char buf[16384];
    size_t argc, used;
    int failed;
};

/** Append an argument of formatted text. */
static void arg(struct line *l, const char *format, ...)
{
    va_list ap;
    int n;

    if (l->failed || l->argc + 2 > sizeof(l->argv) / sizeof(l->argv[0])) {
        l->failed = 1;
        return;
    }
    va_start(ap, format);
    n = vsnprintf(l->buf + l->used, sizeof(l->buf) - l->used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(l->buf) - l->used) {
        l->failed = 1;
        return;
    }
    l->argv[l->argc++] = l->buf + l->used;
    l->argv[l->argc] = NULL;
    l->used += (size_t)n + 1;
}

/**
 * Append a virtio disk of image `file` in `format`, with `serial` or none.
 * `format` may be followed by the drive's options, and `extra` has the
 * device's.
 */
static void disk(struct line *l, const char *id, const char *file, const char *format,
                 const char *serial, const char *extra)
{
    char v[PATH_LEN * 2];

    if (qemu_value(file, v, sizeof(v)) != 0) {
        l->failed = 1;
        return;
    }
    arg(l, "-drive");
    arg(l, "if=none,id=%s,file=%s,format=%s", id, v, format);
    arg(l, "-device");
    arg(l, "virtio-blk-pci,drive=%s%s%s%s", id, serial != NULL ? ",serial=" : "",
        serial != NULL ? serial : "", extra);
}

/* A boot's processors, its memory in MiB, and whether it has a network. */
struct shape {
    unsigned cpus, mib;
    int network;
};

/**
 * Start QEMU's command line: the machine, its root disk and its consoles. An
 * amd64 machine is QEMU's default PC; an arm64 one is its `virt` board with
 * the UEFI firmware QEMU installs, which gives the guest a second PL011
 * serial port, ttyAMA1, for the second `-serial`.
 */
static int qemu_line(struct line *l, const struct etude_tooling_site *site,
                     const struct etude_tooling_machine *m, const char *dir, const char *root,
                     const struct shape *shape)
{
    int arm = strcmp(m->arch, "arm64") == 0;
    char path[PATH_LEN], v[PATH_LEN * 2];

    memset(l, 0, sizeof(*l));
    if (find_program(site->envp, arm ? "qemu-system-aarch64" : "qemu-system-x86_64", path,
                     sizeof(path)) != 0)
        return -1;
    arg(l, "%s", path);
    if (arm) {
        arg(l, "-M");
        arg(l, "virt");
        arg(l, "-bios");
        arg(l, "edk2-aarch64-code.fd");
    }
    arg(l, "-accel");
    arg(l, "%s", site->accel);
    arg(l, "-cpu");
    arg(l, "max");
    arg(l, "-smp");
    arg(l, "%u", shape->cpus);
    arg(l, "-m");
    arg(l, "%u", shape->mib);
    arg(l, "-nodefaults");
    arg(l, "-no-user-config");
    arg(l, "-display");
    arg(l, "none");
    arg(l, "-no-reboot");
    disk(l, "root", root, "qcow2", NULL, ",bootindex=0");
    arg(l, "-device");
    arg(l, "virtio-rng-pci");
    arg(l, "-nic");
    arg(l, "%s", shape->network ? "user,model=virtio-net-pci" : "none");
    if (qemu_value(dir, v, sizeof(v)) != 0)
        return -1;
    arg(l, "-serial");
    arg(l, "file:%s/console.log", v);
    arg(l, "-serial");
    arg(l, "file:%s/channel.log", v);
    return l->failed ? -1 : 0;
}

/** Download the image to its place in the cache, checked by its digest. */
static int download(const struct etude_tooling_site *site, const struct etude_tooling_machine *m,
                    char *why, size_t whylen)
{
    char part[PATH_LEN], hex[65];
    char *argv[] = {"curl", "-fsSL", "--retry", "3", "-o", part, NULL, NULL};

    argv[6] = (char *)m->url;
    if (exists(m->image)) {
        if (file_sha256(m->image, hex) != 0 || strcmp(hex, m->image_sha256) != 0)
            return failure(why, whylen, "%s is not the image the recipe names; remove it",
                           m->image);
        return 0;
    }
    if (fmt(part, sizeof(part), "%s.%ld.part", m->image, (long)getpid()) != 0)
        return failure(why, whylen, "the cache's path is too long");
    fprintf(stderr, "etude-tooling: prepare: downloading %s\n", m->url);
    if (run_program(site, argv, why, whylen) != 0) {
        remove(part);
        return -1;
    }
    if (file_sha256(part, hex) != 0 || strcmp(hex, m->image_sha256) != 0) {
        remove(part);
        return failure(why, whylen, "%s has the digest %s, not %s", m->url, hex, m->image_sha256);
    }
    read_only(part);
    if (!exists(m->image) && rename(part, m->image) != 0) {
        remove(part);
        return failure(why, whylen, "%s cannot be published: %s", m->image, strerror(errno));
    }
    remove(part);
    return 0;
}

/**
 * Make the seed volume of the first boot, DIR/seed.img, from the recipe: the
 * FAT volume labelled CIDATA that cloud-init's NoCloud source looks for.
 */
static int make_seed(const struct etude_tooling_site *site, const char *dir, char *why,
                     size_t whylen)
{
    static const char *const files[] = ETUDE_TOOLING_RECIPE;
    struct args a = {NULL, 0, 0, 0};
    size_t i;

    add_arg(&a, "%s", site->volume);
    add_arg(&a, "seed");
    add_arg(&a, "%s/seed.img", dir);
    add_arg(&a, "CIDATA");
    add_arg(&a, "%s", site->recipe);
    for (i = 0; files[i] != NULL; i++)
        add_arg(&a, "%s", files[i]);
    return run_args(site, &a, why, whylen);
}

/** Give an executor's measurement as a history measurement. */
static struct etude_measurement from_exec(const struct etude_measure *m, int whole)
{
    struct etude_measurement r;

    memset(&r, 0, sizeof(r));
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        r.reason = m->method != NULL ? m->method : "it was not measured";
        return r;
    }
    r.value = m->value;
    r.method = m->method;
    r.quality = whole && m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound";
    return r;
}

/* A boot in the making: the machine, QEMU's command line and its files. */
struct work {
    struct etude_tooling_machine m;
    struct boot b;
    struct line l;
    char dir[PATH_LEN];  /* the boot's private directory, or "" */
    char root[PATH_LEN]; /* the disk it boots: the part being made, or the job's copy */
    char img[PATH_LEN];  /* the job volume, or "" */
};

/** Download the image; make the seed, the new disk and QEMU's command line. */
static int prepare_line(const struct etude_tooling_site *site, struct work *w, unsigned cpus,
                        char *why, size_t whylen)
{
    struct shape shape;
    char seed[PATH_LEN], base[PATH_LEN];
    char *create[] = {"qemu-img", "create", "-q",    "-f",    "qcow2",   "-b",
                      base,       "-F",     "qcow2", w->root, DISK_SIZE, NULL};

    shape.cpus = cpus > 0 ? cpus : 4;
    shape.mib = 2048;
    shape.network = 1;
    if (download(site, &w->m, why, whylen) != 0)
        return -1;
    if (private_dir(site->cache, "prepare", w->dir, sizeof(w->dir)) != 0) {
        w->dir[0] = '\0';
        return failure(why, whylen, "a directory in %s cannot be made: %s", site->cache,
                       strerror(errno));
    }
    /* The made disk is beside its backing image, which it names relatively. */
    if (fmt(w->root, sizeof(w->root), "%s/tooling-%s.%ld.part", site->cache, w->m.key,
            (long)getpid()) != 0 ||
        fmt(base, sizeof(base), "image-%s.img", w->m.image_sha256) != 0 ||
        fmt(seed, sizeof(seed), "%s/seed.img", w->dir) != 0 ||
        fmt(w->b.channel, sizeof(w->b.channel), "%s/channel.log", w->dir) != 0)
        return failure(why, whylen, "the path is too long");
    if (make_seed(site, w->dir, why, whylen) != 0 || run_program(site, create, why, whylen) != 0)
        return -1;
    if (qemu_line(&w->l, site, &w->m, w->dir, w->root, &shape) != 0 ||
        (disk(&w->l, "seed", seed, "raw", NULL, ""), w->l.failed))
        return failure(why, whylen, "QEMU's command line cannot be made");
    return 0;
}

/**
 * Make the outcome of a boot that had not ended STOPPED when a signal asked
 * it to stop, with `what` the part that the signal stopped.
 */
static void note_stop(const struct etude_tooling_site *site, struct etude_tooling_result *res,
                      const char *what)
{
    if (res->outcome == ETUDE_TOOLING_ENDED || !stopped(site))
        return;
    res->outcome = ETUDE_TOOLING_STOPPED;
    failure(res->why, sizeof(res->why), "a signal stopped %s", what);
}

/** Judge a provisioning boot, and publish its disk and its report. */
static int prepare_publish(struct work *w, int late, struct etude_tooling_result *res)
{
    const struct boot *b = &w->b;
    char *log;
    size_t len;

    if (late) {
        res->outcome = ETUDE_TOOLING_LATE;
        return failure(res->why, sizeof(res->why), "the deadline passed before provision.sh ended");
    }
    if (!b->provisioned || b->failed[0] != '\0')
        return failure(res->why, sizeof(res->why), "%s",
                       b->failed[0] != '\0' ? b->failed : "provision.sh did not report");
    if ((log = slurp(b->channel, EVIDENCE_MAX, &len)) != NULL) {
        FILE *f = fopen(w->m.tools, "wb");

        if (f == NULL || fwrite(log, 1, len, f) != len || fclose(f) != 0)
            failure(res->why, sizeof(res->why), "%s cannot be written", w->m.tools);
        free(log);
    }
    if (res->why[0] != '\0')
        return -1;
    read_only(w->root);
    if (!exists(w->m.disk) && rename(w->root, w->m.disk) != 0)
        return failure(res->why, sizeof(res->why), "%s cannot be published: %s", w->m.disk,
                       strerror(errno));
    res->outcome = ETUDE_TOOLING_ENDED;
    return 0;
}

int etude_tooling_prepare(const struct etude_tooling_site *site, unsigned cpus,
                          uint64_t deadline_us, struct etude_tooling_result *res)
{
    static const char *const scratch[] = {"seed.img", "console.log", "channel.log", NULL};
    struct work w;
    struct etude_reading now;
    int end;

    memset(res, 0, sizeof(*res));
    memset(&w, 0, sizeof(w));
    res->outcome = ETUDE_TOOLING_FAILED;
    w.b.cost = &res->cost;
    if (etude_tooling_machine(site, &w.m, res->why, sizeof(res->why)) != 0)
        return -1;
    if (exists(w.m.disk)) {
        res->outcome = ETUDE_TOOLING_ENDED;
        res->reused = 1;
        return 0;
    }
    if (make_dirs(site->cache) != 0)
        return failure(res->why, sizeof(res->why), "%s cannot be made: %s", site->cache,
                       strerror(errno));
    if (etude_history_now(&now) != 0)
        return failure(res->why, sizeof(res->why), "the clock cannot be read");
    res->making = 1;
    step_begin(&w.b, ETUDE_TOOLING_START, &now);
    if (prepare_line(site, &w, cpus, res->why, sizeof(res->why)) == 0) {
        fprintf(stderr, "etude-tooling: prepare: provisioning %s\n", w.m.key);
        end = boot_run(&w.b, site, w.l.argv, deadline_us > 0 ? deadline_us : 7200000000u, res->why,
                       sizeof(res->why));
        if (end == BOOT_ENDED || end == BOOT_LATE)
            prepare_publish(&w, end == BOOT_LATE, res);
    }
    note_stop(site, res, "the provisioning");
    res->cost.disk_bytes = used_bytes(w.m.disk) + used_bytes(w.m.image);
    if (w.root[0] != '\0' && exists(w.root)) {
        res->cost.disk_bytes += used_bytes(w.root);
        remove(w.root);
    }
    boot_free(&w.b);
    if (w.dir[0] != '\0' && res->outcome != ETUDE_TOOLING_ENDED &&
        res->outcome != ETUDE_TOOLING_STOPPED)
        snprintf(res->dir, sizeof(res->dir), "%s", w.dir);
    else if (w.dir[0] != '\0' && remove_dir(w.dir, scratch) != 0)
        res->cost.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    if (etude_history_now(&now) == 0)
        step_end(&w.b, &now, 0, 0, res->outcome == ETUDE_TOOLING_ENDED);
    res->cost.steps[0].cpu_us = from_exec(&res->cost.cpu_us, 0);
    return res->outcome == ETUDE_TOOLING_ENDED ? 0 : -1;
}

/** Give the last name of `path`, which may end in separators. */
static const char *last_name(const char *path, char *out, size_t size)
{
    size_t end = strlen(path), start;

    while (end > 0 && is_sep(path[end - 1]))
        end--;
    for (start = end; start > 0 && !is_sep(path[start - 1]); start--)
        ;
    snprintf(out, size, "%.*s", (int)(end - start), path + start);
    return out;
}

/**
 * Make the job volume DIR/job.img: an archive of the script as job.sh, the
 * list of outputs, DIR/outputs, as outputs, and in/ with each input under
 * its last name.
 */
static int make_volume(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                       const char *dir, char *why, size_t whylen)
{
    struct args a = {NULL, 0, 0, 0};
    char names[PATH_LEN], base[PATH_LEN];
    const char *const *p;
    FILE *f;

    if (fmt(names, sizeof(names), "%s/outputs", dir) != 0)
        return failure(why, whylen, "the path is too long");
    if ((f = fopen(names, "wb")) == NULL)
        return failure(why, whylen, "%s cannot be written", names);
    for (p = job->outputs; p != NULL && *p != NULL; p++)
        fprintf(f, "%s\n", *p);
    if (fclose(f) != 0)
        return failure(why, whylen, "%s cannot be written", names);
    add_arg(&a, "%s", site->volume);
    add_arg(&a, "job");
    add_arg(&a, "%s/job.img", dir);
    add_arg(&a, "%u", job->volume_mib > 0 ? job->volume_mib : 64);
    add_arg(&a, "job.sh");
    add_arg(&a, "%s", job->script);
    add_arg(&a, "outputs");
    add_arg(&a, "%s", names);
    add_arg(&a, "-d");
    add_arg(&a, "in");
    for (p = job->inputs; p != NULL && *p != NULL; p++) {
        add_arg(&a, "in/%s", last_name(*p, base, sizeof(base)));
        add_arg(&a, "%s", *p);
    }
    return run_args(site, &a, why, whylen);
}

/**
 * Read the job's volume back: its status, its output, and its outputs, each
 * of which it must have, into the job's directory of outputs.
 */
static int read_volume(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                       const char *dir, struct etude_tooling_result *res)
{
    static const char *const names[] = {"status", "stdout", "stderr"};
    struct args a = {NULL, 0, 0, 0};
    char from[PATH_LEN], base[PATH_LEN], *data;
    size_t i, len;
    int status;

    add_arg(&a, "%s", site->volume);
    add_arg(&a, "read");
    add_arg(&a, "%s/job.img", dir);
    for (i = 0; i < 3; i++) {
        add_arg(&a, "%s", names[i]);
        add_arg(&a, "%s/%s", dir, names[i]);
    }
    for (i = 0; job->outputs != NULL && job->outputs[i] != NULL; i++) {
        last_name(job->outputs[i], base, sizeof(base));
        add_arg(&a, "out/%s", base);
        add_arg(&a, "%s/%s", job->outdir, base);
    }
    add_arg(&a, "out");
    add_arg(&a, "%s", job->outdir);
    if (run_args(site, &a, res->why, sizeof(res->why)) != 0)
        return -1;
    for (i = 0; i < 3; i++) {
        if (fmt(from, sizeof(from), "%s/%s", dir, names[i]) != 0)
            return failure(res->why, sizeof(res->why), "the path is too long");
        if ((data = slurp(from, JOB_OUTPUT_MAX, &len)) == NULL)
            return failure(res->why, sizeof(res->why), "%s cannot be read", from);
        if (i == 0) {
            if (sscanf(data, "%d", &status) != 1 || status != res->status) {
                free(data);
                return failure(res->why, sizeof(res->why),
                               "the volume's status is not the one the runner reported");
            }
            free(data);
        } else if (i == 1) {
            res->out = data;
            res->out_len = len;
        } else {
            res->err = data;
            res->err_len = len;
        }
    }
    return 0;
}

/** Make the job's disk, its volume and QEMU's command line with its disks. */
static int job_line(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                    struct work *w, char *why, size_t whylen)
{
    struct shape shape;
    char base[PATH_LEN], id[16], serial[24];
    char *create[] = {"qemu-img", "create", "-q",    "-f",    "qcow2", "-b",
                      base,       "-F",     "qcow2", w->root, NULL};
    size_t i;

    shape.cpus = job->cpus > 0 ? job->cpus : 2;
    shape.mib = job->memory_mib > 0 ? job->memory_mib : 2048;
    shape.network = job->network;
    if (fmt(w->root, sizeof(w->root), "%s/root.qcow2", w->dir) != 0 ||
        fmt(base, sizeof(base), "../tooling-%s.qcow2", w->m.key) != 0 ||
        fmt(w->img, sizeof(w->img), "%s/job.img", w->dir) != 0 ||
        fmt(w->b.channel, sizeof(w->b.channel), "%s/channel.log", w->dir) != 0)
        return failure(why, whylen, "the path is too long");
    if (run_program(site, create, why, whylen) != 0 ||
        make_volume(site, job, w->dir, why, whylen) != 0)
        return -1;
    if (qemu_line(&w->l, site, &w->m, w->dir, w->root, &shape) != 0)
        return failure(why, whylen, "QEMU's command line cannot be made");
    disk(&w->l, "job", w->img, "raw", "etude-job", "");
    for (i = 0; job->disks != NULL && job->disks[i] != NULL; i++) {
        snprintf(id, sizeof(id), "d%lu", (unsigned long)i + 1);
        snprintf(serial, sizeof(serial), "etude-d%lu", (unsigned long)i + 1);
        disk(&w->l, id, job->disks[i], "raw,discard=unmap", serial, "");
    }
    return w->l.failed ? failure(why, whylen, "QEMU's command line cannot be made") : 0;
}

/** Judge a job's boot; read its status, output and outputs from its volume. */
static int job_judge(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                     const struct work *w, int late, struct etude_tooling_result *res)
{
    if (late) {
        res->outcome = ETUDE_TOOLING_LATE;
        return failure(res->why, sizeof(res->why), "the deadline passed before the job %s",
                       w->b.started ? "ended" : "started");
    }
    if (w->b.failed[0] != '\0' || !w->b.ended)
        return failure(res->why, sizeof(res->why), "%s",
                       w->b.failed[0] != '\0' ? w->b.failed
                                              : "the job runner did not report the job's end");
    res->status = w->b.status;
    if (read_volume(site, job, w->dir, res) != 0)
        return -1;
    res->outcome = ETUDE_TOOLING_ENDED;
    return 0;
}

int etude_tooling_run(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                      struct etude_tooling_result *res)
{
    static const char *const scratch[] = {"root.qcow2",  "job.img", "console.log",
                                          "channel.log", "outputs", "status",
                                          "stdout",      "stderr",  NULL};
    struct work w;
    struct etude_reading now;
    size_t i;
    int end, whole;

    memset(res, 0, sizeof(*res));
    memset(&w, 0, sizeof(w));
    res->outcome = ETUDE_TOOLING_FAILED;
    w.b.cost = &res->cost;
    if (etude_tooling_machine(site, &w.m, res->why, sizeof(res->why)) != 0)
        return -1;
    if (!exists(w.m.disk))
        return failure(res->why, sizeof(res->why),
                       "the tooling machine %s is not made; run `etude-tooling prepare`", w.m.key);
    if (etude_history_now(&now) != 0)
        return failure(res->why, sizeof(res->why), "the clock cannot be read");
    step_begin(&w.b, ETUDE_TOOLING_START, &now);
    if (private_dir(site->cache, "job", w.dir, sizeof(w.dir)) != 0)
        return failure(res->why, sizeof(res->why), "a directory in %s cannot be made: %s",
                       site->cache, strerror(errno));
    if (job_line(site, job, &w, res->why, sizeof(res->why)) == 0) {
        end = boot_run(&w.b, site, w.l.argv, job->deadline_us, res->why, sizeof(res->why));
        if (end == BOOT_ENDED || end == BOOT_LATE)
            job_judge(site, job, &w, end == BOOT_LATE, res);
    }
    note_stop(site, res, w.b.started ? "the job" : "the job before it started");
    res->cost.disk_bytes = used_bytes(w.root) + used_bytes(w.img);
    for (i = 0; job->disks != NULL && job->disks[i] != NULL; i++)
        res->cost.disk_bytes += used_bytes(job->disks[i]);
    boot_free(&w.b);
    if ((res->outcome != ETUDE_TOOLING_ENDED && res->outcome != ETUDE_TOOLING_STOPPED) || job->keep)
        snprintf(res->dir, sizeof(res->dir), "%s", w.dir);
    else if (remove_dir(w.dir, scratch) != 0 && res->cost.cleanup == ETUDE_CLEANUP_CONFIRMED)
        res->cost.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    if (etude_history_now(&now) != 0)
        return res->outcome == ETUDE_TOOLING_ENDED ? 0 : -1;
    whole = res->outcome == ETUDE_TOOLING_ENDED && res->cost.cleanup == ETUDE_CLEANUP_CONFIRMED;
    if (w.b.step != ETUDE_TOOLING_END || !w.b.ran)
        step_end(&w.b, &now, 0, 0, 0);
    else
        step_end(&w.b, &now, res->cost.cpu_us.quality != ETUDE_QUALITY_UNAVAILABLE && w.b.sampled,
                 res->cost.cpu_us.value, whole);
    return res->outcome == ETUDE_TOOLING_ENDED ? 0 : -1;
}

void etude_tooling_result_free(struct etude_tooling_result *res)
{
    free(res->out);
    free(res->err);
    res->out = res->err = NULL;
}

/** Keep `bytes` in the store by digest and append its blob reference to `t`. */
static int keep(const char *store, const char *bytes, size_t n, struct text *t)
{
    unsigned char digest[32];
    char hex[65], path[PATH_LEN];
    struct sha256 s;
    FILE *f;

    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)bytes, n);
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    if (fmt(path, sizeof(path), "%s/%s", store, hex) != 0)
        return -1;
    if (!exists(path)) {
        if ((f = fopen(path, "wb")) == NULL)
            return -1;
        if (fwrite(bytes, 1, n, f) != n) {
            fclose(f);
            return -1;
        }
        if (fclose(f) != 0)
            return -1;
    }
    tprintf(t, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[", hex, (unsigned long)n);
    tstring(t, path);
    tprintf(t, "]}");
    return 0;
}

/** Append an executor's measurement as the history writes one. */
static void exec_measure(struct text *t, const struct etude_measure *m)
{
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        tprintf(t, "{\"value\":null,\"reason\":");
        tstring(t, m->method != NULL ? m->method : "it was not measured");
        tprintf(t, "}");
        return;
    }
    tprintf(t, "{\"value\":%llu,\"method\":", (unsigned long long)m->value);
    tstring(t, m->method);
    tprintf(t, ",\"quality\":\"%s\",\"interval_us\":null,\"error_bound\":null}",
            m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound");
}

/** Append a history measurement. */
static int measurement(struct text *t, const struct etude_measurement *m)
{
    char json[4096];

    if (etude_measurement_json(m, json, sizeof(json)) != 0)
        return -1;
    tprintf(t, "%s", json);
    return 0;
}

/** Append a step's time. */
static int step_time(struct text *t, const struct etude_tooling_ids *ids,
                     const struct etude_tooling_step *s)
{
    struct etude_measurement m;

    if (s->ended)
        m = etude_history_elapsed(&s->from, &s->to);
    else
        m = etude_history_elapsed(&s->from, NULL);
    tprintf(t, "{\"start_utc\":\"%s\",\"end_utc\":", s->from.utc);
    if (s->ended && m.reason == NULL)
        tprintf(t, "\"%s\"", s->to.utc);
    else
        tprintf(t, "null");
    tprintf(t, ",\"clock\":{\"machine_id\":");
    tstring(t, ids->machine);
    tprintf(t, ",\"boot_id\":");
    tstring(t, ids->boot);
    tprintf(t, ",\"clock_id\":\"monotonic\",\"resolution_us\":%llu},\"start_us\":%llu,",
            (unsigned long long)s->from.resolution_us, (unsigned long long)s->from.monotonic_us);
    if (s->ended && m.reason == NULL)
        tprintf(t, "\"end_us\":%llu", (unsigned long long)s->to.monotonic_us);
    else
        tprintf(t, "\"end_us\":null");
    tprintf(t, ",\"elapsed_us\":");
    if (measurement(t, &m) != 0)
        return -1;
    tprintf(t, "}");
    return 0;
}

/** Give step `i`'s outcome, and its reason when it did not pass. */
static const char *step_outcome(const struct etude_tooling_result *res, unsigned i,
                                const char **reason)
{
    const struct etude_tooling_step *s = &res->cost.steps[i];

    *reason = NULL;
    if (s->done && !res->making && i == ETUDE_TOOLING_JOB && res->status != 0) {
        *reason = "the job exited with a status other than 0";
        return "failure";
    }
    if (s->done && res->cost.cleanup == ETUDE_CLEANUP_CONFIRMED)
        return "pass";
    *reason = res->why[0] != '\0' ? res->why
              : s->done           ? "the cleanup of QEMU's processes is not confirmed"
                                  : "the step did not reach its witness";
    return res->outcome == ETUDE_TOOLING_LATE      ? "incomplete"
           : res->outcome == ETUDE_TOOLING_STOPPED ? "cancelled"
                                                   : "failure";
}

int etude_tooling_record(struct etude_history *h, const struct etude_tooling_ids *ids,
                         const struct etude_tooling_result *res)
{
    static const char *const job_steps[] = {"tooling.start", "tooling.job", "tooling.end"};
    static const char *const job_stages[] = {"tooling_start", "execute", "cleanup"};
    static const char *const witnesses[] = {"etude-job.start", "etude-job.ran", "etude-job.end"};
    int prepare = res->making;
    struct etude_reading now;
    char id[1024], utc[32];
    unsigned i;
    int rc = 0;

    for (i = 0; i < res->cost.nsteps && rc == 0; i++) {
        const struct etude_tooling_step *s = &res->cost.steps[i];
        struct text scope = {NULL, 0, 0, 0}, t = {NULL, 0, 0, 0};
        struct etude_measurement gap;
        const char *outcome, *reason;

        tprintf(&scope, "{\"tooling\":");
        tstring(&scope, ids->key);
        tprintf(&scope,
                ",\"qemu\":{\"pid\":%ld,\"start\":%llu},\"accounting\":", res->cost.qemu.pid,
                (unsigned long long)res->cost.qemu.start);
        tstring(&scope, ACCOUNTING ", sampled at its ends; the totals are the whole tree's over "
                                   "the boot");
        tprintf(&scope, ",\"totals\":{\"tree_cpu_us\":");
        exec_measure(&scope, &res->cost.cpu_us);
        tprintf(&scope, ",\"peak_bytes\":");
        exec_measure(&scope, &res->cost.peak_bytes);
        tprintf(&scope, ",\"disk_bytes\":%llu}}\n", (unsigned long long)res->cost.disk_bytes);
        tprintf(&t, "{\"plan_id\":");
        tstring(&t, ids->plan);
        tprintf(&t, ",\"item_id\":");
        if (ids->item != NULL)
            tstring(&t, ids->item);
        else
            tprintf(&t, "null");
        tprintf(&t, ",\"attempt_id\":");
        if (ids->attempt != NULL)
            tstring(&t, ids->attempt);
        else
            tprintf(&t, "null");
        tprintf(&t, ",\"step_id\":\"%s\",\"stage\":\"%s\",\"time\":",
                prepare ? "tooling.prepare" : job_steps[i],
                prepare ? "one_time_setup" : job_stages[i]);
        rc |= step_time(&t, ids, s);
        tprintf(&t, ",\"resources\":{\"scope\":");
        rc |= scope.failed ? -1 : keep(ids->store, scope.data, scope.len, &t);
        tprintf(&t, ",\"tree_cpu_us\":");
        rc |= measurement(&t, &s->cpu_us);
        tprintf(&t, ",\"combined_peak_bytes\":");
        rc |= measurement(&t, &s->peak_bytes);
        tprintf(&t, ",\"samples\":[]},\"witnesses\":[");
        if (s->done)
            tprintf(&t,
                    "{\"witness_id\":\"%s\",\"sequence\":0,\"offset_us\":%llu,"
                    "\"completed_units\":1}",
                    prepare ? "etude-tooling.provisioned" : witnesses[i],
                    (unsigned long long)(s->to.monotonic_us - s->from.monotonic_us));
        tprintf(&t, "],\"max_progress_gap_us\":");
        gap = etude_history_elapsed(&s->from, s->done ? &s->to : NULL);
        rc |= measurement(&t, &gap);
        outcome = step_outcome(res, i, &reason);
        tprintf(&t, ",\"result\":{\"outcome\":\"%s\",\"reason\":", outcome);
        if (reason != NULL)
            tstring(&t, reason);
        else
            tprintf(&t, "null");
        tprintf(&t,
                ",\"cleanup\":\"%s\",\"evidence\":[],\"capture_errors\":[],\"disruptions\":[]}}",
                res->cost.cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed");
        if (t.failed || rc != 0 || etude_history_now(&now) != 0 ||
            etude_history_record_id(h, id, sizeof(id)) != 0)
            rc = -1;
        else {
            snprintf(utc, sizeof(utc), "%s", now.utc);
            rc = etude_history_append(h, id, utc, "step", t.data);
        }
        free(scope.data);
        free(t.data);
    }
    return rc;
}
