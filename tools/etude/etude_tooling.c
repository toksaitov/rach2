/*
 * etude_tooling.c: etude-tooling, the command that makes the local Linux
 * tooling machine, says whether it is made, and runs a job in it.
 * tools/etude/Readme.md gives its usage.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/tooling.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#define environ _environ
#else
extern char **environ;
#endif

/* The most files, disks and outputs a job names. */
#define LIST_MAX 64

/* The signal that asked the command to stop, or 0. */
static volatile sig_atomic_t stop_signal;

/* etude-volume beside this program, or its name alone. */
static char volume[4096];

/* The history's options. */
struct record {
    const char *path, *stream, *run, *schema;
    struct etude_tooling_ids ids;
};

static int usage(void)
{
    fprintf(
        stderr,
        "usage: etude-tooling [-r RECIPE] [-C CACHE] [-p ARCH] [-a ACCEL] prepare [-c CPUS] "
        "[-t SECONDS] [HISTORY]\n"
        "       etude-tooling [-r RECIPE] [-C CACHE] [-p ARCH] check\n"
        "       etude-tooling [-r RECIPE] [-C CACHE] [-p ARCH] [-a ACCEL] run [-n] [-k] [-c CPUS] "
        "[-m MIB] [-s MIB] [-t SECONDS]\n"
        "                     [-i PATH]... [-d DISK]... [-o NAME]... [-O DIR] [HISTORY] SCRIPT\n"
        "HISTORY: -H STREAM -j STREAM_ID -R RUN -P PLAN [-I ITEM] [-A ATTEMPT] -M MACHINE "
        "-B BOOT -b STORE [-S SCHEMA]\n");
    return 2;
}

/** Note the signal that asks the command to stop. */
static void on_signal(int sig)
{
#ifdef _WIN32
    /* Windows sets the handler back to its default before it calls it. */
    signal(sig, on_signal);
#endif
    stop_signal = sig;
}

/**
 * Catch the signals that stop a boot, so that its QEMU is stopped and its
 * files are removed before the command exits.
 */
static void catch_signals(void)
{
#ifdef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#else
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif
}

/** Read a count of at least 1 and at most `max`; 0 when it is not one. */
static unsigned long count(const char *s, unsigned long max)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    return errno != 0 || end == s || *end != '\0' || v == 0 || v > max ? 0 : v;
}

/** Take option `o` with value `v` into the history's options, or return -1. */
static int record_option(struct record *r, char o, const char *v)
{
    switch (o) {
    case 'H':
        r->path = v;
        break;
    case 'j':
        r->stream = v;
        break;
    case 'R':
        r->run = v;
        break;
    case 'P':
        r->ids.plan = v;
        break;
    case 'I':
        r->ids.item = v;
        break;
    case 'A':
        r->ids.attempt = v;
        break;
    case 'M':
        r->ids.machine = v;
        break;
    case 'B':
        r->ids.boot = v;
        break;
    case 'b':
        r->ids.store = v;
        break;
    case 'S':
        r->schema = v;
        break;
    default:
        return -1;
    }
    return 0;
}

/** Say whether the history's options are none, or whole. */
static int record_valid(const struct record *r)
{
    int any = r->path != NULL || r->stream != NULL || r->run != NULL || r->ids.plan != NULL ||
              r->ids.item != NULL || r->ids.attempt != NULL || r->ids.machine != NULL ||
              r->ids.boot != NULL || r->ids.store != NULL || r->schema != NULL;

    return !any || (r->path != NULL && r->stream != NULL && r->run != NULL && r->ids.plan != NULL &&
                    r->ids.machine != NULL && r->ids.boot != NULL && r->ids.store != NULL);
}

/** Write a boot's steps to a new history stream, when one is asked for. */
static int write_record(struct record *r, const struct etude_tooling_machine *m,
                        const struct etude_tooling_result *res, const char *recipe)
{
    char schema[4096];
    struct etude_history *h;
    int rc;

    if (r->path == NULL || res->cost.nsteps == 0)
        return 0;
    if (r->schema != NULL)
        snprintf(schema, sizeof(schema), "%s", r->schema);
    else
        snprintf(schema, sizeof(schema), "%s/../schemas/history.schema.json", recipe);
    r->ids.key = m->key;
    h = etude_history_open(r->path, schema, r->stream, r->run, (uint64_t)1 << 24);
    if (h == NULL) {
        fprintf(stderr, "etude-tooling: %s: %s\n", r->path, strerror(errno));
        return -1;
    }
    rc = etude_tooling_record(h, &r->ids, res);
    if (etude_history_close(h) != 0 || rc != 0) {
        fprintf(stderr, "etude-tooling: %s: the steps could not be recorded\n", r->path);
        return -1;
    }
    return 0;
}

/** Print a measurement of the executor's in `unit`, divided by `per`. */
static void print_measure(const char *what, const struct etude_measure *m, const char *unit,
                          double per)
{
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE)
        fprintf(stderr, "; %s not measured", what);
    else
        fprintf(stderr, "; %s %.2f %s (%s)", what, (double)m->value / per, unit,
                m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower bound");
}

/** Print what a boot cost, one line on standard error. */
static void print_cost(const struct etude_tooling_result *res)
{
    static const char *const names[] = {"start", "job", "end"};
    const struct etude_tooling_cost *c = &res->cost;
    unsigned i;

    if (c->nsteps == 0)
        return;
    fprintf(stderr, "etude-tooling: cost:");
    for (i = 0; i < c->nsteps; i++) {
        const struct etude_tooling_step *s = &c->steps[i];

        if (s->ended)
            fprintf(stderr, "%s %s %.2f s", i > 0 ? "," : "", res->making ? "prepare" : names[i],
                    (double)(s->to.monotonic_us - s->from.monotonic_us) / 1e6);
        else
            fprintf(stderr, "%s %s unended", i > 0 ? "," : "", names[i]);
    }
    print_measure("processor", &c->cpu_us, "s", 1e6);
    print_measure("peak memory", &c->peak_bytes, "MiB", 1048576.0);
    fprintf(stderr, "; disk %.2f MiB\n", (double)c->disk_bytes / 1048576.0);
}

/** Write `n` bytes to `f`. */
static void relay(FILE *f, const char *data, size_t n)
{
    if (n > 0)
        fwrite(data, 1, n, f);
    fflush(f);
}

/* The command's options. */
struct options {
    struct etude_tooling_site site;
    struct etude_tooling_job job;
    struct record rec;
    const char *inputs[LIST_MAX + 1], *disks[LIST_MAX + 1], *outputs[LIST_MAX + 1];
    size_t ni, nd, no;
    unsigned long seconds, cpus;
};

/** Append `v` to a list of at most LIST_MAX names; -1 when it is full. */
static int list_add(const char **list, size_t *n, const char *v)
{
    if (*n == LIST_MAX)
        return -1;
    list[(*n)++] = v;
    list[*n] = NULL;
    return 0;
}

/** Take a command's option `o` with value `v`, or return -1. */
static int job_option(struct options *op, char o, const char *v)
{
    switch (o) {
    case 'c':
        return (op->cpus = count(v, 64)) == 0 ? -1 : 0;
    case 'm':
        return (op->job.memory_mib = (unsigned)count(v, 1u << 20)) == 0 ? -1 : 0;
    case 's':
        return (op->job.volume_mib = (unsigned)count(v, 1u << 16)) == 0 ? -1 : 0;
    case 't':
        return (op->seconds = count(v, 1ul << 30)) == 0 ? -1 : 0;
    case 'i':
        return list_add(op->inputs, &op->ni, v);
    case 'd':
        return list_add(op->disks, &op->nd, v);
    case 'o':
        return list_add(op->outputs, &op->no, v);
    case 'O':
        op->job.outdir = v;
        return 0;
    default:
        return record_option(&op->rec, o, v);
    }
}

/**
 * Give the path of etude-volume, which is built and deployed beside this
 * program: in the directory of `self`, this program's `argv[0]`, or, when
 * that has none, on the PATH as this program was.
 */
static const char *find_volume(const char *self)
{
    size_t n = strlen(self);

#ifdef _WIN32
    while (n > 0 && self[n - 1] != '/' && self[n - 1] != '\\' && self[n - 1] != ':')
        n--;
    snprintf(volume, sizeof(volume), "%.*setude-volume%s", (int)n, self, n > 0 ? ".exe" : "");
    if (n > 0 && _access(volume, 0) != 0)
        snprintf(volume, sizeof(volume), "%.*setude-volume", (int)n, self);
#else
    while (n > 0 && self[n - 1] != '/')
        n--;
    snprintf(volume, sizeof(volume), "%.*setude-volume", (int)n, self);
#endif
    return volume;
}

/**
 * Read the options before the command and after it.
 *
 * @return the index of the command, with `*rest` the index of its first
 *         operand, or -1 for a wrong usage.
 */
static int read_options(int argc, char **argv, struct options *op, int *rest)
{
    const char *cache = getenv("ETUDE_TOOLING_CACHE");
    int i, command;

    memset(op, 0, sizeof(*op));
    op->site.recipe = "tools/etude/tooling";
    op->site.volume = find_volume(argv[0]);
    op->site.cache = cache != NULL && *cache != '\0' ? cache : "build/tooling";
    op->site.arch = getenv("ETUDE_TOOLING_ARCH");
    op->site.accel = getenv("RACH2_QEMU_ACCEL");
    op->site.envp = environ;
    op->job.outdir = ".";
    for (i = 1; i < argc && argv[i][0] == '-'; i += 2) {
        if (i + 1 >= argc || argv[i][1] == '\0' || argv[i][2] != '\0')
            return -1;
        if (argv[i][1] == 'r')
            op->site.recipe = argv[i + 1];
        else if (argv[i][1] == 'C')
            op->site.cache = argv[i + 1];
        else if (argv[i][1] == 'p')
            op->site.arch = argv[i + 1];
        else if (argv[i][1] == 'a')
            op->site.accel = argv[i + 1];
        else
            return -1;
    }
    if (i >= argc)
        return -1;
    command = i++;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0' && argv[i][2] == '\0'; i++) {
        if (argv[i][1] == 'n' || argv[i][1] == 'k')
            *(argv[i][1] == 'n' ? &op->job.network : &op->job.keep) = 1;
        else if (i + 1 == argc || job_option(op, argv[i][1], argv[i + 1]) != 0)
            return -1;
        else
            i++;
    }
    *rest = i;
    return record_valid(&op->rec) ? command : -1;
}

/** Make the machine; 0 when it is made or reused. */
static int prepare(struct options *op, const struct etude_tooling_machine *m)
{
    struct etude_tooling_result res;
    int rc;

    if (op->ni + op->nd + op->no > 0)
        return usage();
    rc = etude_tooling_prepare(&op->site, (unsigned)op->cpus, (uint64_t)op->seconds * 1000000u,
                               &res);
    print_cost(&res);
    if (write_record(&op->rec, m, &res, op->site.recipe) != 0)
        rc = -1;
    if (res.outcome == ETUDE_TOOLING_ENDED)
        printf("etude-tooling: result: %s %s %s\n", res.reused ? "reused" : "made", m->key,
               m->disk);
    else
        printf("etude-tooling: result: %s: %s%s%s\n",
               res.outcome == ETUDE_TOOLING_STOPPED ? "stopped" : "failed", res.why,
               res.dir[0] != '\0' ? "; its logs are in " : "", res.dir);
    return rc == 0 ? 0 : 1;
}

/** Run the job `script`; the job's status, or the command's own. */
static int run(struct options *op, const struct etude_tooling_machine *m, const char *script)
{
    struct etude_tooling_result res;
    int rc;

    op->job.script = script;
    op->job.inputs = op->inputs;
    op->job.disks = op->disks;
    op->job.outputs = op->outputs;
    op->job.cpus = (unsigned)op->cpus;
    op->job.deadline_us = (uint64_t)op->seconds * 1000000u;
    rc = etude_tooling_run(&op->site, &op->job, &res);
    relay(stdout, res.out, res.out_len);
    relay(stderr, res.err, res.err_len);
    print_cost(&res);
    if (write_record(&op->rec, m, &res, op->site.recipe) != 0 && rc == 0)
        rc = -1, res.outcome = ETUDE_TOOLING_FAILED;
    if (res.outcome != ETUDE_TOOLING_ENDED)
        fprintf(stderr, "etude-tooling: result: %s: %s%s%s\n",
                res.outcome == ETUDE_TOOLING_LATE      ? "late"
                : res.outcome == ETUDE_TOOLING_STOPPED ? "stopped"
                                                       : "failed",
                res.why, res.dir[0] != '\0' ? "; its files are in " : "", res.dir);
    else if (res.dir[0] != '\0')
        fprintf(stderr, "etude-tooling: the job's files are in %s\n", res.dir);
    etude_tooling_result_free(&res);
    if (rc == 0)
        return res.status;
    if (res.outcome == ETUDE_TOOLING_LATE)
        return 124;
    return strstr(res.why, "is not made") != NULL ? 126 : 125;
}

int main(int argc, char **argv)
{
    static struct options op;
    struct etude_tooling_machine m;
    const char *command;
    char why[512];
    int i, rest, made, rc;
    FILE *f;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if ((i = read_options(argc, argv, &op, &rest)) < 0)
        return usage();
    command = argv[i];
    if (etude_tooling_machine(&op.site, &m, why, sizeof(why)) != 0) {
        fprintf(stderr, "etude-tooling: %s\n", why);
        return strcmp(command, "run") == 0 ? 125 : 1;
    }
    if (strcmp(command, "check") == 0) {
        if (rest != argc)
            return usage();
        f = fopen(m.disk, "rb");
        made = f != NULL;
        if (made)
            fclose(f);
        printf("etude-tooling: result: %s %s %s\n", made ? "made" : "not made", m.key, m.disk);
        return made ? 0 : 1;
    }
    if (op.site.accel == NULL || *op.site.accel == '\0') {
        fprintf(stderr, "etude-tooling: name QEMU's accelerator with -a or RACH2_QEMU_ACCEL\n");
        return usage();
    }
    catch_signals();
    op.site.stop = &stop_signal;
    if (strcmp(command, "prepare") == 0 && rest == argc)
        rc = prepare(&op, &m);
    else if (strcmp(command, "run") == 0 && rest + 1 == argc)
        rc = run(&op, &m, argv[rest]);
    else
        return usage();
    return stop_signal != 0 ? 128 + stop_signal : rc;
}
