/**
 * Check etude's set driver (etude_legacy_set in tools/etude/posix/legacy_set.c)
 * on the suites of a fixture tree: a registry, a Makefile whose recipes run a
 * script of the tree's, and a stand-in for QEMU first on the path. This
 * program holds the tree's run lock and is the driver's etude-legacy: run as
 * `prepare`, `run` or `check`, it does what etude-legacy does. Each set runs
 * in a child of its own, with a private ledger, and with `locks` a private
 * directory of the old runner's locks.
 *
 * host-deps: tools/etude/posix/legacy.c tools/etude/posix/legacy_set.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/capture.c
 * host-deps: tools/etude/watchdog.c tools/etude/supervise.c
 * host-deps: tools/etude/qemu_profile.c tools/etude/posix/qemu.c
 * host-deps: tools/etude/processor.c tools/etude/ledger.c tools/etude/sched.c
 * host-deps: tools/etude/posix/bridge.c tools/etude/posix/status.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-seconds: 240
 *
 * A set of two suites that pass and one that fails exits 1, with a history
 * the schema accepts of one plan, one step, an attempt for each suite and a
 * run record, the failed suite's work kept and the others' removed, no claim
 * left in the ledger, each command's output kept, and the driver's own line
 * of each suite's start in place of etude-legacy's lines of its start and
 * result. With `locks`, two suites that run at once have port blocks of their
 * own; without, both have the caller's. A width of one keeps two suites
 * apart, and one of two lets them meet. A serial member runs first, with
 * nothing beside it. A failure under `stop_on_fail`, and one of a member that
 * `stops`, cancel the suite that runs and the one not started, and say so. A
 * stop signal exits 143 with the running suite cancelled and its output
 * printed, the history whole and the ledger empty; while the suite runs, the
 * status record has a line of its attempt, and the record is removed at the
 * end. A failed preparation exits 3 with every item cancelled and no attempt.
 * A suite that asks for more memory than the machine has runs. A set of no
 * suite, a bad name, a suite named twice, no port block without `locks`, a
 * bad accelerator and a relative ledger are refused, with nothing run. The
 * driver leaks no memory.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "etude/history.h"
#include "etude/ledger.h"
#include "etude/legacy.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            printf("etude-legacyset-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                   \
            fails++;                                                                         \
        }                                                                                    \
    } while (0)

/* The room for a path: a fixture's are under a $TMPDIR of at most 100
 * characters (fixdir_host.h). */
#define PATH_LEN 1024

/* The room for what a set printed, and for a status record. */
#define OUT_LEN 131072

/* The bound of a wait for a recipe to start, and for the status record to
 * name an attempt, in milliseconds. */
#define START_BOUND  60000
#define STATUS_BOUND 30000

/* The seconds a set may take before it is ended as hung. */
#define SET_BOUND 120

/* The memory of the fixture machine's ledger, in MiB. */
#define MEMORY_MIB 8192u

/* LeakSanitizer's check, when the test is built with it; the reference is
 * weak, so a test built without it links. */
extern int __lsan_do_recoverable_leak_check(void) __attribute__((weak));

/* This program, the fixture tree, etude's schemas, and the fixtures' ledger
 * and lock directory. */
static char self[PATH_MAX];
static char tree[PATH_LEN];
static char schemas[PATH_MAX];
static char ledger[PATH_LEN];
static char locks[PATH_LEN];

/* The suites of the fixture tree. */
static const char registry[] = "# The suites of the fixture tree.\n"
                               "boot|qemu|core\n"
                               "plain|-|core\n"
                               "fail|-|core\n"
                               "meet-a|-|core\n"
                               "meet-b|-|core\n"
                               "serial|-|core\n"
                               "side-x|-|core\n"
                               "side-y|-|core\n"
                               "long|-|core\n"
                               "hold|-|core\n";

/* The fixture's Makefile. Each suite's target needs the one image, and its
 * recipe runs the tree's script, which says what each does; the components
 * fail when FIXTURE_BREAK is set. */
static const char makefile[] =
    "ARCH ?= i686\n"
    "ifeq ($(ARCH),x86_64)\n"
    "ARCH_BUILD := build/x86_64\n"
    "else\n"
    "ARCH_BUILD := build\n"
    "endif\n"
    "IMAGES := $(ARCH_BUILD)/images\n"
    ".PHONY: components prepare-all prepare-tools run-boot run-plain run-fail run-meet-a \\\n"
    "        run-meet-b run-serial run-side-x run-side-y run-long run-hold\n"
    "components: $(ARCH_BUILD)/rach2/kernel\n"
    "\t@if [ -n \"$$FIXTURE_BREAK\" ]; then echo broken components; exit 4; fi\n"
    "$(ARCH_BUILD)/rach2/kernel:\n"
    "\t@mkdir -p $(@D) && echo kernel > $@\n"
    "prepare-all: components\n"
    "prepare-tools:\n"
    "\t@echo sh\n"
    "$(IMAGES)/disk.img:\n"
    "\t@mkdir -p $(@D) && echo disk > $@\n"
    "run-boot: $(IMAGES)/disk.img\n"
    "\t@qemu-system-i386 -m 64 pass\n"
    "run-plain: $(IMAGES)/disk.img\n"
    "\t@echo plain ran\n"
    "run-fail: $(IMAGES)/disk.img\n"
    "\t@echo failing; exit 3\n"
    "run-meet-a: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh meet meet-a meet-b\n"
    "run-meet-b: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh meet meet-b meet-a\n"
    "run-serial: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh mark serial 1\n"
    "run-side-x: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh mark side-x 0.2\n"
    "run-side-y: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh mark side-y 0.2\n"
    "run-long: $(IMAGES)/disk.img\n"
    "\t@echo long running; sleep 8\n"
    "run-hold: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh hold\n";

/* The script of the recipes. `meet SELF OTHER` waits two seconds at most
 * for the other suite to be present, and notes whether it met it and its
 * port block; `mark NAME SECONDS` notes its start and its end; `hold` says it
 * has started through a FIFO and waits fifteen seconds at most. */
static const char script[] =
    "mode=$1\n"
    "shift\n"
    "case $mode in\n"
    "meet)\n"
    "    touch \"meet/$1.here\"\n"
    "    i=0\n"
    "    until [ -e \"meet/$2.here\" ] || [ $i -ge 20 ]; do sleep 0.1; i=$((i + 1)); done\n"
    "    if [ -e \"meet/$2.here\" ]; then w=met; else w=alone; fi\n"
    "    sleep 0.3\n"
    "    rm -f \"meet/$1.here\"\n"
    "    echo \"$1 $w $RACH2_PORT_OFFSET\" >> meet/log ;;\n"
    "mark)\n"
    "    echo \"start $1\" >> marks\n"
    "    sleep \"$2\"\n"
    "    echo \"end $1\" >> marks ;;\n"
    "hold)\n"
    "    echo holding\n"
    "    echo started > started.fifo\n"
    "    i=0\n"
    "    until [ -e release ] || [ $i -ge 150 ]; do sleep 0.1; i=$((i + 1)); done ;;\n"
    "esac\n";

/* The stand-in for QEMU, which the shim makes a machine of. */
static const char standin[] = "#!/bin/sh\n"
                              "echo FIXTURE-BOOT-OK\n"
                              "sleep 0.2\n";

/* The records of a history, each a line of the stream. */
struct history {
    char **line;
    size_t n, cap;
};

/* One run of the set driver, as a row asks for it, and what it gave. */
struct run {
    const char *name; /* the row's, after which its files are named */
    const struct etude_legacy_member *members;
    size_t n;
    unsigned width;
    int locks, stop_on_fail;
    const char *accel;  /* the accelerator, or NULL for tcg */
    const char *ledger; /* the ledger, or NULL for the fixtures' */
    const char *unset;  /* a variable the driver runs without, or NULL */
    const char *var;    /* a variable the driver runs with, or NULL */
    pid_t pid;
    int status;  /* the driver's, or -1 when it did not end by itself */
    long claims; /* the claims the ledger kept after it, or -1 */
    int leaks;   /* LeakSanitizer found a leak of the driver */
    char results[PATH_LEN], evidence[PATH_LEN], status_file[PATH_LEN];
    char out_file[PATH_LEN], side[PATH_LEN];
    char run_id[256];
    char out[OUT_LEN]; /* what it printed */
    int read;          /* etude_history_read's result, or -2 for no stream */
    struct history h;
};

/* ---- Files and text. ---- */

/** Give the path of `name` in the fixture tree in `buf`. */
static const char *in(char *buf, size_t size, const char *name)
{
    fix_format(buf, size, "%s/%s", tree, name);
    return buf;
}

/** Make file `path` hold `text` with mode `mode`; 0 or -1. */
static int write_text(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    int r;

    if (f == NULL)
        return -1;
    r = fputs(text, f) < 0 ? -1 : 0;
    if (fclose(f) != 0)
        r = -1;
    return r == 0 ? chmod(path, mode) : -1;
}

/** Read up to `max - 1` bytes of `path` into `buf`, ended by NUL; the
 * length, or -1 with `buf` empty. */
static long read_text(const char *path, char *buf, size_t max)
{
    FILE *f = fopen(path, "r");
    size_t n;

    buf[0] = '\0';
    if (f == NULL)
        return -1;
    n = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/** Say whether `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/** Give the first line of `text` that begins with `start`, or NULL. */
static const char *line_start(const char *text, const char *start)
{
    const char *at;

    for (at = text; (at = strstr(at, start)) != NULL; at++) {
        if (at == text || at[-1] == '\n')
            return at;
    }
    return NULL;
}

/** Say whether `text` has a line that begins with `start`. */
static int has_start(const char *text, const char *start)
{
    return line_start(text, start) != NULL;
}

/** Count the lines of `text` that are `line` whole. */
static int count_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at;
    int count = 0;

    for (at = text; (at = strstr(at, line)) != NULL; at++) {
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\0'))
            count++;
    }
    return count;
}

/** Say whether `text` has the whole line `line`. */
static int has_line(const char *text, const char *line)
{
    return count_line(text, line) > 0;
}

/** Give the line number of `line` in `text`, from 1, or 0 when it has none. */
static int line_no(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at = text;
    int no = 1;

    while (*at != '\0') {
        if (strncmp(at, line, n) == 0 && (at[n] == '\n' || at[n] == '\0'))
            return no;
        at = strchr(at, '\n');
        if (at == NULL)
            break;
        at++;
        no++;
    }
    return 0;
}

/* ---- Histories. ---- */

/** Keep a copy of line `line` of a stream in the history `ctx`. */
static int keep_line(const char *line, void *ctx)
{
    struct history *h = ctx;
    char **grown;

    if (h->n == h->cap) {
        h->cap = h->cap == 0 ? 16 : h->cap * 2;
        if ((grown = realloc(h->line, h->cap * sizeof(*grown))) == NULL)
            return -1;
        h->line = grown;
    }
    if ((h->line[h->n] = strdup(line)) == NULL)
        return -1;
    h->n++;
    return 0;
}

/** Free the lines of history `h`. */
static void history_free(struct history *h)
{
    size_t i;

    for (i = 0; i < h->n; i++)
        free(h->line[i]);
    free(h->line);
    memset(h, 0, sizeof(*h));
}

/** Read the one stream under RESULTS/history of run `r`, with the run's
 * identity from its name. */
static void history_read(struct run *r)
{
    char dir[PATH_LEN], path[PATH_LEN * 2], schema[PATH_MAX + 32];
    struct dirent *e;
    size_t n;
    uint64_t bad = 0;
    DIR *d;

    r->read = -2;
    r->run_id[0] = '\0';
    fix_format(dir, sizeof(dir), "%s/history", r->results);
    if ((d = opendir(dir)) == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        n = strlen(e->d_name);
        if (n > 6 && strcmp(e->d_name + n - 6, ".jsonl") == 0 && n - 6 < sizeof(r->run_id)) {
            snprintf(r->run_id, sizeof(r->run_id), "%.*s", (int)(n - 6), e->d_name);
            break;
        }
    }
    closedir(d);
    if (r->run_id[0] == '\0')
        return;
    fix_format(path, sizeof(path), "%s/%s.jsonl", dir, r->run_id);
    fix_format(schema, sizeof(schema), "%s/history.schema.json", schemas);
    r->read = etude_history_read(path, schema, keep_line, &r->h, &bad);
}

/** Count the records of kind `kind` in history `h`. */
static int count_kind(const struct history *h, const char *kind)
{
    char want[64];
    size_t i;
    int n = 0;

    fix_format(want, sizeof(want), "\"kind\":\"%s\"", kind);
    for (i = 0; i < h->n; i++)
        n += strstr(h->line[i], want) != NULL;
    return n;
}

/** Copy into `out` the outcome that follows `key` after `from` in `line`, or
 * "" when there is none. */
static void outcome_after(const char *line, const char *from, const char *key, char *out,
                          size_t size)
{
    const char *at = strstr(line, from);

    out[0] = '\0';
    if (at == NULL || (at = strstr(at, key)) == NULL)
        return;
    at += strlen(key);
    snprintf(out, size, "%.*s", (int)strcspn(at, "\""), at);
}

/** Give the outcome of the attempt record of suite `suite` in `out`, or ""
 * when the history has no attempt of it. */
static const char *attempt_of(const struct run *r, const char *suite, char *out, size_t size)
{
    char id[128];
    size_t i;

    out[0] = '\0';
    fix_format(id, sizeof(id), ":legacy.%s:i686:r0:a0\"", suite);
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"attempt\"") != NULL && strstr(r->h.line[i], id))
            outcome_after(r->h.line[i], "\"kind\":\"attempt\"", "\"result\":{\"outcome\":\"", out,
                          size);
    return out;
}

/** Give the outcome of the item of suite `suite` in the run record in
 * `out`, or "" when there is none. */
static const char *item_of(const struct run *r, const char *suite, char *out, size_t size)
{
    char id[128];
    size_t i;

    out[0] = '\0';
    fix_format(id, sizeof(id), ":legacy.%s:i686:r0\",\"attempt_records\":[", suite);
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"run\"") != NULL)
            outcome_after(r->h.line[i], id, "\"outcome\":\"", out, size);
    return out;
}

/** Give the outcome of the preparation's step record in `out`, or "". */
static const char *step_of(const struct run *r, char *out, size_t size)
{
    size_t i;

    out[0] = '\0';
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"step\"") != NULL)
            outcome_after(r->h.line[i], "\"kind\":\"step\"", "\"result\":{\"outcome\":\"", out,
                          size);
    return out;
}

/** Say whether the attempt of `suite` has outcome `want`. */
static int attempt_is(const struct run *r, const char *suite, const char *want)
{
    char got[32];

    return strcmp(attempt_of(r, suite, got, sizeof(got)), want) == 0;
}

/** Say whether the run record's item of `suite` has outcome `want`. */
static int item_is(const struct run *r, const char *suite, const char *want)
{
    char got[32];

    return strcmp(item_of(r, suite, got, sizeof(got)), want) == 0;
}

/* ---- This program as etude-legacy. ---- */

/* The stop signal this process received, as etude-legacy or as a set's
 * driver. */
static volatile sig_atomic_t stop_signal;

/** Note a stop signal. */
static void note_stop(int sig)
{
    stop_signal = sig;
}

/** Catch the stop signals without restarting the executor's waits, as
 * etude-legacy does. */
static void catch_stops(void)
{
    static const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_stop;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

/** Read a whole number of seconds from 1 to a week; 0 when `s` is not one. */
static unsigned seconds_of(const char *s)
{
    char *end;
    unsigned long v;

    if (*s < '0' || *s > '9')
        return 0;
    v = strtoul(s, &end, 10);
    return *end == '\0' && v >= 1 && v <= 604800ul ? (unsigned)v : 0;
}

/**
 * Do what etude-legacy does with `argv`, its arguments from `prepare`, `run`
 * or `check` on: the options the set driver gives it, parsed as
 * tools/etude/posix/etude_legacy.c parses them.
 *
 * @return its status, or 2 for a wrong usage.
 */
static int as_legacy(int argc, char **argv)
{
    struct etude_legacy_opts p;
    struct etude_legacy_run_opts r;
    int opt;

    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    if (strcmp(argv[0], "check") == 0)
        return argc == 2 ? etude_legacy_check(argv[1], 0, stdout) : 2;
    memset(&p, 0, sizeof(p));
    memset(&r, 0, sizeof(r));
    p.tree = ".";
    p.seconds = r.seconds = ETUDE_LEGACY_SECONDS;
    r.idle = ETUDE_LEGACY_IDLE;
    p.out = r.out = stdout;
    p.stop = r.stop = &stop_signal;
    while ((opt = getopt(argc, argv, "t:a:n:w:o:s:i:")) != -1) {
        if (opt == 't')
            p.tree = optarg;
        else if (opt == 'a')
            p.arch = optarg;
        else if (opt == 'n')
            p.kernel = optarg;
        else if (opt == 'w')
            p.work = r.work = optarg;
        else if (opt == 'o')
            p.record = optarg;
        else if (opt == 's' && (p.seconds = r.seconds = seconds_of(optarg)) != 0)
            continue;
        else if (opt == 'i' && (r.idle = seconds_of(optarg)) != 0)
            continue;
        else
            return 2;
    }
    if (strcmp(argv[0], "prepare") == 0 && optind < argc) {
        p.suites = argv + optind;
        p.nsuites = (size_t)(argc - optind);
        return etude_legacy_prepare(&p);
    }
    if (strcmp(argv[0], "run") == 0 && optind == argc - 2) {
        r.record = argv[optind];
        r.suite = argv[optind + 1];
        return etude_legacy_run(&r);
    }
    return 2;
}

/* ---- The fixture tree. ---- */

/** Make the fixture tree, with its run lock held by this process, runutil's
 * source, the recipes' script and the stand-in first on the path. */
static void make_tree(void)
{
    static const char *const dirs[] = {
        "",      "tests",           "tests/system", "tests/system/runner",
        "build", "build/.run.lock", "meet",         "qemu"};
    static char source[1 << 20], newpath[8192];
    const char *oldpath = getenv("PATH");
    char path[PATH_LEN], owner[64];
    size_t i;
    int bad = 0;

    fix_format(tree, sizeof(tree), "%s/tree", fix_dir());
    fix_format(ledger, sizeof(ledger), "%s/ledger", fix_dir());
    fix_format(locks, sizeof(locks), "%s/locks", fix_dir());
    fix_format(path, sizeof(path), "%s/tmp", fix_dir());
    bad |= mkdir(path, 0755) != 0 || setenv("TMPDIR", path, 1) != 0;
    bad |= mkdir(locks, 0755) != 0;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        bad |= mkdir(in(path, sizeof(path), dirs[i]), 0755) != 0;
    bad |= mkfifo(in(path, sizeof(path), "started.fifo"), 0644) != 0;
    snprintf(owner, sizeof(owner), "pid %ld\n", (long)getpid());
    bad |= write_text(in(path, sizeof(path), "tests/registry"), registry, 0644);
    bad |= write_text(in(path, sizeof(path), "Makefile"), makefile, 0644);
    bad |= write_text(in(path, sizeof(path), "fixture.sh"), script, 0644);
    bad |= write_text(in(path, sizeof(path), "qemu/qemu-system-i386"), standin, 0755);
    bad |= write_text(in(path, sizeof(path), "build/.run.lock/owner"), owner, 0644);
    bad |= read_text("tests/system/runner/runutil.c", source, sizeof(source)) <= 0;
    bad |= write_text(in(path, sizeof(path), "tests/system/runner/runutil.c"), source, 0644);
    fix_format(newpath, sizeof(newpath), "%s/qemu:%s", tree,
               oldpath != NULL ? oldpath : "/usr/bin:/bin");
    bad |= setenv("PATH", newpath, 1) != 0;
    if (bad) {
        printf("etude-legacyset-test: fail: the fixture tree %s was not made\n", tree);
        exit(1);
    }
}

/** Remove the files of the recipes that meet and that mark their times. */
static void clear_marks(void)
{
    static const char *const names[] = {"marks", "meet/log", "meet/meet-a.here",
                                        "meet/meet-b.here"};
    char path[PATH_LEN];
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unlink(in(path, sizeof(path), names[i]));
}

/* ---- Sets. ---- */

/** Count claim `c` into the count `ctx`. */
static int count_claim(const struct etude_ledger_claim *c, void *ctx)
{
    (void)c;
    (*(long *)ctx)++;
    return 0;
}

/** Fill `o` for run `r`, with bounds no row reaches. */
static void opts_for(struct etude_legacy_set_opts *o, const struct run *r)
{
    memset(o, 0, sizeof(*o));
    o->tree = tree;
    o->arch = "i686";
    o->program = self;
    o->members = r->members;
    o->nmembers = r->n;
    o->ledger = r->ledger != NULL ? r->ledger : ledger;
    o->account = "fixture";
    o->cap[ETUDE_LEGACY_CPU] = 8000;
    o->cap[ETUDE_LEGACY_MEMORY] = (uint64_t)MEMORY_MIB << 20;
    o->cap[ETUDE_LEGACY_DISK] = UINT64_C(64) << 30;
    o->locks = r->locks ? locks : NULL;
    o->wait = 30;
    o->seconds = 60;
    o->idle = 30;
    o->width = r->width;
    o->cpus = 1;
    o->accel = r->accel != NULL ? r->accel : "tcg";
    o->stop_on_fail = r->stop_on_fail;
    o->results = r->results;
    o->schemas = schemas;
    o->evidence = r->evidence;
    o->status = r->status_file;
    o->args = r->name;
}

/**
 * Run the set of `r` as a set's driver, in this child of the test: write
 * what it printed to `r->out_file`, then the claims the ledger keeps while
 * this process, their owner, still runs and whether LeakSanitizer finds a
 * leak to `r->side`, and exit with the driver's status.
 */
static void driver(const struct run *r)
{
    struct etude_legacy_set_opts o;
    struct etude_ledger *l;
    long claims = -1;
    int status, leaks;
    FILE *f;

    catch_stops();
    if (r->unset != NULL)
        unsetenv(r->unset);
    if (r->var != NULL)
        putenv((char *)r->var);
    opts_for(&o, r);
    o.stop = &stop_signal;
    if ((o.out = fopen(r->out_file, "w")) == NULL)
        _exit(120);
    status = etude_legacy_set(&o);
    fclose(o.out);
    if ((l = etude_ledger_open(ledger)) != NULL) {
        claims = 0;
        if (etude_ledger_list(l, count_claim, &claims) != 0)
            claims = -1;
        etude_ledger_close(l);
    }
    leaks = __lsan_do_recoverable_leak_check != NULL && __lsan_do_recoverable_leak_check() != 0;
    if ((f = fopen(r->side, "w")) != NULL) {
        fprintf(f, "%ld %d\n", claims, leaks);
        fclose(f);
    }
    _exit(status & 0xff);
}

/** Start the set of `r` in a child, with its files named for the row. */
static void set_start(struct run *r)
{
    const char *dir = fix_dir();

    fix_format(r->results, sizeof(r->results), "%s/%s.results", dir, r->name);
    fix_format(r->evidence, sizeof(r->evidence), "%s/%s.evidence", dir, r->name);
    fix_format(r->status_file, sizeof(r->status_file), "%s/%s.status", dir, r->name);
    fix_format(r->out_file, sizeof(r->out_file), "%s/%s.out", dir, r->name);
    fix_format(r->side, sizeof(r->side), "%s/%s.side", dir, r->name);
    r->status = -1;
    r->claims = -1;
    fflush(stdout);
    r->pid = fork();
    if (r->pid == 0)
        driver(r);
    if (r->pid < 0)
        printf("etude-legacyset-test: fail: %s: no child: %s\n", r->name, strerror(errno));
}

/** Wait for the set of `r` to end within SET_BOUND seconds, or end it, and
 * take what it gave. */
static void set_finish(struct run *r)
{
    char side[64];
    int status, i;
    pid_t got = 0;

    for (i = 0; r->pid > 0 && i < SET_BOUND * 20; i++) {
        if ((got = waitpid(r->pid, &status, WNOHANG)) != 0)
            break;
        /* interval: a set is looked at again every 50 ms */
        usleep(50000);
    }
    if (r->pid > 0 && got == 0) {
        printf("etude-legacyset-test: fail: %s: the set did not end in %d s\n", r->name, SET_BOUND);
        fails++;
        kill(r->pid, SIGKILL);
        got = waitpid(r->pid, &status, 0);
    } else if (got == r->pid && WIFEXITED(status)) {
        r->status = WEXITSTATUS(status);
    }
    read_text(r->out_file, r->out, sizeof(r->out));
    if (read_text(r->side, side, sizeof(side)) > 0 &&
        sscanf(side, "%ld %d", &r->claims, &r->leaks) != 2)
        r->claims = -1;
    history_read(r);
}

/** Run the set of `r` and take what it gave. */
static void set_run(struct run *r)
{
    set_start(r);
    set_finish(r);
}

/** Print what a set printed when a check of its row failed, and free its
 * history. */
static void show(struct run *r, int before)
{
    if (fails != before)
        printf("etude-legacyset-test: %s: its output:\n%s", r->name, r->out);
    history_free(&r->h);
}

/** Say whether the work of suite `suite` of run `r` was kept as evidence. */
static int kept(const struct run *r, const char *suite)
{
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/%s.%s", r->evidence, r->run_id, suite);
    return exists(path);
}

/** Say whether the output of command `name` of run `r` was kept, with a line
 * `line` in it when `line` is not NULL. */
static int output_has(const struct run *r, const char *name, const char *line)
{
    static char text[OUT_LEN];
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/output/%s.log", r->results, name);
    return read_text(path, text, sizeof(text)) >= 0 && (line == NULL || has_line(text, line));
}

/* ---- Rows. ---- */

/** A set of suites that pass and one that fails. */
static void test_mixed(void)
{
    static const struct etude_legacy_member m[] = {
        {"boot", 64, 0, 0}, {"plain", 64, 0, 0}, {"fail", 64, 0, 0}};
    static struct run r;
    static const char *const names[] = {"boot", "plain", "fail"};
    char line[128];
    size_t i;
    int before = fails;

    r.name = "mixed";
    r.members = m;
    r.n = 3;
    set_run(&r);
    CHECK(r.status == 1, "mixed: failed");
    CHECK(r.read == 0, "mixed: the history is whole");
    CHECK(count_kind(&r.h, "plan") == 1, "mixed: one plan");
    CHECK(count_kind(&r.h, "step") == 1, "mixed: one step");
    CHECK(count_kind(&r.h, "attempt") == 3, "mixed: an attempt of each suite");
    CHECK(count_kind(&r.h, "run") == 1, "mixed: one run record");
    CHECK(attempt_is(&r, "boot", "pass"), "mixed: boot passed");
    CHECK(attempt_is(&r, "plain", "pass"), "mixed: plain passed");
    CHECK(attempt_is(&r, "fail", "failure"), "mixed: fail failed");
    CHECK(item_is(&r, "fail", "failure"), "mixed: the run record's item of fail");
    CHECK(kept(&r, "fail"), "mixed: the failed suite's work kept");
    CHECK(!kept(&r, "plain") && !kept(&r, "boot"), "mixed: a passed suite's work removed");
    CHECK(r.claims == 0, "mixed: the ledger has no claim");
    CHECK(!r.leaks, "mixed: the driver leaks nothing");
    CHECK(output_has(&r, "prepare", NULL), "mixed: the preparation's output kept");
    CHECK(output_has(&r, "boot", "FIXTURE-BOOT-OK"), "mixed: the output of boot kept");
    CHECK(output_has(&r, "fail", "failing"), "mixed: the output of fail kept");
    CHECK(has_line(r.out, "failing") && has_line(r.out, "plain ran"),
          "mixed: the suites' output printed");
    for (i = 0; i < 3; i++) {
        fix_format(line, sizeof(line), "run: %s: starting (idle limit 30 s)", names[i]);
        CHECK(count_line(r.out, line) == 1, "mixed: one line of each suite's start");
        fix_format(line, sizeof(line), "run: %s: PASS in ", names[i]);
        CHECK(!has_start(r.out, line), "mixed: no line of etude-legacy's result");
        fix_format(line, sizeof(line), "run: %s: FAIL in ", names[i]);
        CHECK(!has_start(r.out, line), "mixed: no line of etude-legacy's result");
    }
    CHECK(has_line(r.out, "etude-legacy: result: ran: 3 suites, 2 passed"), "mixed: the result");
    show(&r, before);
}

/** Read the meeting suites' notes into `log`, and say whether both have the
 * note `what`. */
static int both(char *log, size_t size, const char *a, const char *b)
{
    char path[PATH_LEN];

    read_text(in(path, sizeof(path), "meet/log"), log, size);
    return has_start(log, a) && has_start(log, b);
}

/** Give the port offset of the meeting suite whose note begins with
 * `start` in `log`, or -1 when it has none. */
static long offset_of(const char *log, const char *start)
{
    const char *at = line_start(log, start);
    long offset;

    return at != NULL && sscanf(at, "%*s %*s %ld", &offset) == 1 ? offset : -1;
}

/** Two suites that run at once each have a port block of their own with
 * `locks`, and width 2 lets them meet. */
static void test_ports(void)
{
    static const struct etude_legacy_member m[] = {{"meet-a", 64, 0, 0}, {"meet-b", 64, 0, 0}};
    static struct run r;
    char log[1024];
    int before = fails;

    clear_marks();
    r.name = "ports";
    r.members = m;
    r.n = 2;
    r.width = 2;
    r.locks = 1;
    set_run(&r);
    CHECK(r.status == 0, "ports: passed");
    CHECK(both(log, sizeof(log), "meet-a met ", "meet-b met "), "width: two at once with width 2");
    CHECK(offset_of(log, "meet-a ") >= 0 && offset_of(log, "meet-a ") != offset_of(log, "meet-b "),
          "ports: blocks of their own with locks");
    CHECK(strstr(log, " 7000\n") == NULL, "ports: not the caller's block with locks");
    CHECK(r.claims == 0, "ports: the ledger has no claim");
    if (fails != before)
        printf("etude-legacyset-test: ports: the notes:\n%s", log);
    show(&r, before);
}

/** Without `locks` both suites have the caller's port block, and width 1
 * keeps them apart. */
static void test_width(void)
{
    static const struct etude_legacy_member m[] = {{"meet-a", 64, 0, 0}, {"meet-b", 64, 0, 0}};
    static struct run r;
    char log[1024];
    int before = fails;

    clear_marks();
    r.name = "width";
    r.members = m;
    r.n = 2;
    r.width = 1;
    set_run(&r);
    CHECK(r.status == 0, "width: passed");
    CHECK(both(log, sizeof(log), "meet-a alone ", "meet-b alone "),
          "width: one at a time with width 1");
    CHECK(has_line(log, "meet-a alone 7000") || has_line(log, "meet-a met 7000"),
          "ports: the caller's block without locks");
    CHECK(has_line(log, "meet-b alone 7000") || has_line(log, "meet-b met 7000"),
          "ports: the caller's block without locks");
    if (fails != before)
        printf("etude-legacyset-test: width: the notes:\n%s", log);
    show(&r, before);
}

/** A serial member, though named second, runs first with nothing beside
 * it. */
static void test_serial(void)
{
    static const struct etude_legacy_member m[] = {
        {"side-x", 64, 0, 0}, {"serial", 64, 1, 0}, {"side-y", 64, 0, 0}};
    static struct run r;
    char path[PATH_LEN], marks[1024];
    int before = fails;

    clear_marks();
    r.name = "serial";
    r.members = m;
    r.n = 3;
    set_run(&r);
    read_text(in(path, sizeof(path), "marks"), marks, sizeof(marks));
    CHECK(r.status == 0, "serial: passed");
    CHECK(line_no(marks, "start serial") == 1, "serial: it runs first");
    CHECK(line_no(marks, "end serial") == 2, "serial: nothing beside it");
    CHECK(line_no(r.out, "run: serial: starting (idle limit 30 s)") <
                  line_no(r.out, "run: side-x: starting (idle limit 30 s)") &&
              line_no(r.out, "run: serial: starting (idle limit 30 s)") > 0,
          "serial: it starts first");
    if (fails != before)
        printf("etude-legacyset-test: serial: the marks:\n%s", marks);
    show(&r, before);
}

/** A failure under `stop_on_fail` cancels the suite that runs and the one
 * not started. */
static void test_first(void)
{
    static const struct etude_legacy_member m[] = {
        {"fail", 64, 0, 0}, {"long", 64, 0, 0}, {"plain", 64, 0, 0}};
    static struct run r;
    int before = fails;

    r.name = "first";
    r.members = m;
    r.n = 3;
    r.width = 2;
    r.stop_on_fail = 1;
    set_run(&r);
    CHECK(r.status == 1, "first: failed");
    CHECK(r.read == 0, "first: the history is whole");
    CHECK(attempt_is(&r, "fail", "failure"), "first: fail failed");
    CHECK(attempt_is(&r, "long", "cancelled"), "first: the running suite cancelled");
    CHECK(attempt_is(&r, "plain", ""), "first: no attempt of the suite not started");
    CHECK(item_is(&r, "plain", "cancelled"), "first: the suite not started cancelled");
    CHECK(has_line(r.out, "run: stopped on the first failure; not started: plain"),
          "first: the suites not started named");
    CHECK(has_line(r.out, "run: stopped on the first failure; stopped part way: long"),
          "first: the suites stopped part way named");
    CHECK(has_line(r.out, "run: long: stopped before its end; its output so far:"),
          "first: the stopped suite's output");
    CHECK(r.claims == 0, "first: the ledger has no claim");
    show(&r, before);
}

/** A failure of a member that `stops` cancels the rest. */
static void test_stops(void)
{
    static const struct etude_legacy_member m[] = {
        {"fail", 64, 0, 1}, {"long", 64, 0, 0}, {"plain", 64, 0, 0}};
    static struct run r;
    int before = fails;

    r.name = "stops";
    r.members = m;
    r.n = 3;
    r.width = 2;
    set_run(&r);
    CHECK(r.status == 1, "stops: failed");
    CHECK(attempt_is(&r, "long", "cancelled"), "stops: the running suite cancelled");
    CHECK(item_is(&r, "plain", "cancelled"), "stops: the suite not started cancelled");
    CHECK(has_line(r.out, "run: stopped: fail failed; not started: plain"),
          "stops: the suites not started named");
    CHECK(has_line(r.out, "run: stopped: fail failed; stopped part way: long"),
          "stops: the suites stopped part way named");
    show(&r, before);
}

/** A stop signal while a suite runs: the status record has a line of its
 * attempt first, and the set then ends with the suite cancelled. */
static void test_signal(void)
{
    static const struct etude_legacy_member m[] = {{"hold", 64, 0, 0}, {"plain", 64, 0, 0}};
    static struct run r;
    static char status[OUT_LEN];
    char path[PATH_LEN], got[16], want[512];
    const char *at;
    struct pollfd pfd;
    int fd, i, named = 0, before = fails;

    fd = open(in(path, sizeof(path), "started.fifo"), O_RDWR | O_CLOEXEC);
    r.name = "signal";
    r.members = m;
    r.n = 2;
    set_start(&r);
    pfd.fd = fd;
    pfd.events = POLLIN;
    CHECK(fd >= 0 && r.pid > 0 && poll(&pfd, 1, START_BOUND) == 1 && read(fd, got, sizeof(got)) > 0,
          "signal: the suite started");
    for (i = 0; i < STATUS_BOUND / 50 && !named; i++) {
        read_text(r.status_file, status, sizeof(status));
        named = (at = line_start(status, "attempt ")) != NULL &&
                strstr(at, ":legacy.hold:i686:r0:a0 legacy.hold i686 ") != NULL;
        if (!named)
            /* interval: the record is read again every 50 ms */
            usleep(50000);
    }
    CHECK(named, "status: the running suite's attempt");
    CHECK(strncmp(status, "etude-status 1\n", 15) == 0, "status: the record's form");
    fix_format(want, sizeof(want), "ledger %s", ledger);
    CHECK(has_line(status, want), "status: the run's ledger");
    if (r.pid > 0)
        kill(r.pid, SIGTERM);
    set_finish(&r);
    if (fd >= 0)
        close(fd);
    fix_format(want, sizeof(want), "run %s", r.run_id);
    CHECK(has_line(status, want), "status: the run's identity");
    CHECK(!exists(r.status_file), "status: removed at the end");
    CHECK(r.status == 128 + SIGTERM, "signal: stopped");
    CHECK(r.read == 0 && count_kind(&r.h, "run") == 1, "signal: the history whole");
    CHECK(attempt_is(&r, "hold", "cancelled"), "signal: the running suite cancelled");
    CHECK(item_is(&r, "hold", "cancelled"), "signal: the run record's item cancelled");
    at = line_start(r.out, "run: hold: stopped before its end; its output so far:");
    CHECK(at != NULL && has_line(at, "holding"), "signal: its output so far printed");
    CHECK(has_line(r.out, "etude-legacy: result: stopped by signal 15"), "signal: the result");
    CHECK(r.claims == 0, "signal: the ledger has no claim");
    if (fails != before)
        printf("etude-legacyset-test: signal: the status record:\n%s", status);
    show(&r, before);
}

/** A preparation that fails cancels every item, and no suite starts. */
static void test_prepare(void)
{
    static const struct etude_legacy_member m[] = {{"plain", 64, 0, 0}, {"fail", 64, 0, 0}};
    static char var[] = "FIXTURE_BREAK=1";
    static struct run r;
    char step[32];
    int before = fails;

    r.name = "prepare";
    r.members = m;
    r.n = 2;
    r.var = var;
    set_run(&r);
    CHECK(r.status == 3, "prepare: failed");
    CHECK(r.read == 0, "prepare: the history is whole");
    CHECK(strcmp(step_of(&r, step, sizeof(step)), "failure") == 0, "prepare: the step failed");
    CHECK(count_kind(&r.h, "attempt") == 0, "prepare: no attempt");
    CHECK(item_is(&r, "plain", "cancelled") && item_is(&r, "fail", "cancelled"),
          "prepare: every item cancelled");
    CHECK(has_start(r.out, "etude-legacy: result: preparation failed: "), "prepare: the result");
    CHECK(r.claims == 0, "prepare: the ledger has no claim");
    show(&r, before);
}

/** A suite that asks for more memory than the machine has runs, its claim
 * held to the limit. */
static void test_large(void)
{
    static const struct etude_legacy_member m[] = {{"plain", UINT64_C(1) << 20, 0, 0}};
    static struct run r;
    int before = fails;

    r.name = "large";
    r.members = m;
    r.n = 1;
    r.locks = 1;
    set_run(&r);
    CHECK(r.status == 0, "large: passed");
    CHECK(attempt_is(&r, "plain", "pass"), "large: it ran");
    CHECK(r.claims == 0, "large: the ledger has no claim");
    show(&r, before);
}

/** Require of the set `r`, named `name`, a refusal with nothing run. */
static void refused(struct run *r, const char *name, const char *row)
{
    int before = fails;

    r->name = name;
    set_run(r);
    CHECK(r->status == 2, row);
    CHECK(has_start(r->out, "etude-legacy: result: refused: "), row);
    CHECK(!exists(r->results), row);
    show(r, before);
}

/** The requests refused before anything runs. */
static void test_refused(void)
{
    static const struct etude_legacy_member bad[] = {{"Plain", 64, 0, 0}};
    static const struct etude_legacy_member twice[] = {{"plain", 64, 0, 0}, {"plain", 64, 0, 0}};
    static const struct etude_legacy_member one[] = {{"plain", 64, 0, 0}};
    static struct run r[6];

    r[0].members = one;
    refused(&r[0], "none", "refused: no suite");
    r[1].members = bad;
    r[1].n = 1;
    refused(&r[1], "badname", "refused: a bad name");
    r[2].members = twice;
    r[2].n = 2;
    refused(&r[2], "twice", "refused: a suite named twice");
    r[3].members = one;
    r[3].n = 1;
    r[3].unset = "RACH2_PORT_OFFSET";
    refused(&r[3], "nooffset", "refused: no port block without locks");
    r[4].members = one;
    r[4].n = 1;
    r[4].accel = "fast";
    refused(&r[4], "accel", "refused: a bad accelerator");
    r[5].members = one;
    r[5].n = 1;
    r[5].ledger = "ledger";
    refused(&r[5], "relative", "refused: a relative ledger");
}

int main(int argc, char **argv)
{
    char held[32];

    if (argc >= 2 && (strcmp(argv[1], "prepare") == 0 || strcmp(argv[1], "run") == 0 ||
                      strcmp(argv[1], "check") == 0))
        return as_legacy(argc - 1, argv + 1);
    signal(SIGPIPE, SIG_IGN);
    if (realpath(argv[0], self) == NULL || realpath("tools/etude/schemas", schemas) == NULL) {
        printf("etude-legacyset-test: fail: this program or etude's schemas cannot be found\n");
        return 1;
    }
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    setenv("RACH2_PORT_OFFSET", "7000", 1);
    setenv("RACH2_QEMU_ACCEL", "tcg", 1);
    setenv("RACH2_CPUS", "1", 1);
    unsetenv("RACH2_DEADLINES");
    unsetenv("FIXTURE_BREAK");
    make_tree();
    test_mixed();
    test_ports();
    test_width();
    test_serial();
    test_first();
    test_stops();
    test_signal();
    test_prepare();
    test_large();
    test_refused();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_legacyset_hosttest: pass\n");
    return 0;
}
