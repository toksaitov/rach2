/**
 * Check etude's suite adapter (etude_legacy_run in tools/etude/posix/legacy.c)
 * and runutil's `record` mode (tests/system/runner/runutil.c) on fixture
 * suites: a tree whose Makefile gives each suite a recipe, and a stand-in for
 * QEMU that the adapter's shim records and relays. This program holds the
 * tree's run lock. The tree has a copy of runutil's source, the file
 * ETUDE_LEGACYRUN_RUNUTIL names or this tree's, which the adapter compiles.
 *
 * host-deps: tools/etude/posix/legacy.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-deps: tools/etude/capture.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-seconds: 120
 *
 * A suite that boots the stand-in and exits 0 passes, with the accelerator,
 * the processors and the port block reaching it and its work removed. A suite
 * that exits 0 with the stand-in left running fails, with the stand-in ended
 * and its work kept with the console and the logs the suite wrote. A suite
 * whose make is killed fails. One whose supervisor is killed is incomplete,
 * and its stand-in is ended through the `child` records with the POSIX
 * interfaces alone. One whose machine's console is cut, one idle past its
 * limit and one past its bound are incomplete, and one that stops its machine
 * and the relay with SIGTERM passes; one under a stop signal is stopped, with
 * its work removed. A target that fails fails. A record that changed, a suite
 * it does not list, no port block, a tree not held, and a wrong count of
 * processors or accelerator are refused with no work made.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

#include "compat/fixdir_host.c"
#include "etude/legacy.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            printf("etude-legacyrun-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                   \
            fails++;                                                                         \
        }                                                                                    \
    } while (0)

/* The room for a path: a fixture's are under a $TMPDIR of at most 100
 * characters (fixdir_host.h). */
#define PATH_LEN 1024

/* The bound of a wait for a recipe to start, in milliseconds. */
#define START_BOUND 60000

/* The fixture tree. */
static char tree[PATH_LEN];

/* The suites of the fixture tree. */
static const char registry[] = "# The suites of the fixture tree.\n"
                               "pass|qemu|core\n"
                               "left|qemu|core\n"
                               "killed|qemu|core\n"
                               "lost|qemu|core\n"
                               "cut|qemu|core\n"
                               "term|qemu|core\n"
                               "quiet|-|core\n"
                               "busy|-|core\n"
                               "fail|-|core\n"
                               "wait|-|core\n";

/* The fixture's Makefile. Each suite's target needs the one image; a recipe
 * that leaves its machine behind waits until the machine has written to
 * its console. */
static const char makefile[] =
    "ARCH ?= i686\n"
    "ifeq ($(ARCH),x86_64)\n"
    "ARCH_BUILD := build/x86_64\n"
    "else\n"
    "ARCH_BUILD := build\n"
    "endif\n"
    "IMAGES := $(ARCH_BUILD)/images\n"
    "booted = i=0; until grep -q lingering \"$$RACH2_QEMU_LEDGER.console\" || \\\n"
    "         [ $$i -ge 200 ]; do sleep 0.1; i=$$((i + 1)); done\n"
    ".PHONY: components prepare-all prepare-tools run-pass run-left run-killed run-lost \\\n"
    "        run-cut run-term run-quiet run-busy run-fail run-wait\n"
    "components: $(ARCH_BUILD)/rach2/kernel\n"
    "$(ARCH_BUILD)/rach2/kernel:\n"
    "\t@mkdir -p $(@D) && echo kernel > $@\n"
    "prepare-all: components\n"
    "prepare-tools:\n"
    "\t@echo sh\n"
    "$(IMAGES)/disk.img:\n"
    "\t@mkdir -p $(@D) && echo disk > $@\n"
    "run-pass: $(IMAGES)/disk.img\n"
    "\t@echo \"offset $$RACH2_PORT_OFFSET lc $$LC_ALL\"; qemu-system-i386 -m 64 pass\n"
    "run-left: $(IMAGES)/disk.img\n"
    "\t@echo own > $(IMAGES)/left-test.log; echo other > $(IMAGES)/pass-test.log\n"
    "\t@qemu-system-i386 linger-left & $(booted); echo left behind\n"
    "run-killed: $(IMAGES)/disk.img\n"
    "\t@qemu-system-i386 pass; kill -KILL $$PPID\n"
    "run-lost: $(IMAGES)/disk.img\n"
    "\t@qemu-system-i386 linger-lost & $(booted); kill -KILL $$(ps -o ppid= -p $$PPID)\n"
    "run-cut: $(IMAGES)/disk.img\n"
    "\t@qemu-system-i386 cut; echo after the cut\n"
    "run-term: $(IMAGES)/disk.img\n"
    "\t@qemu-system-i386 linger-term & $(booted); kill -TERM $$(cat \"$$RACH2_QEMU_LEDGER\"); "
    "wait\n"
    "run-quiet: $(IMAGES)/disk.img\n"
    "\t@sleep 30\n"
    "run-busy: $(IMAGES)/disk.img\n"
    "\t@i=0; while [ $$i -lt 300 ]; do echo tick $$i; sleep 0.1; i=$$((i + 1)); done\n"
    "run-fail: $(IMAGES)/disk.img\n"
    "\t@echo failing; exit 3\n"
    "run-wait: $(IMAGES)/disk.img\n"
    "\t@echo started > started.fifo; read x < hang.fifo\n";

/* The stand-in for QEMU, which the shim makes a machine of: it is the
 * process the ledger records, and its output goes through the relay the
 * ledger records after it. It notes its arguments, and its last argument
 * says what it does: boot and end, linger with a FIFO open, or kill its
 * relay and go on writing. */
static const char standin[] =
    "#!/bin/sh\n"
    "for mode in \"$@\"; do :; done\n"
    "echo \"$*\" >> qemu.args\n"
    "case $mode in\n"
    "pass) echo FIXTURE-BOOT-OK; sleep 0.3 ;;\n"
    "linger-*) echo lingering; exec sleep 60 <>\"$mode.fifo\" ;;\n"
    "cut)\n"
    "    echo before the cut\n"
    "    relay=$(awk -v m=$$ 'f { print; exit } $1 == m { f = 1 }' \"$RACH2_QEMU_LEDGER\")\n"
    "    trap '' PIPE\n"
    "    kill -KILL \"$relay\"\n"
    "    sleep 0.3\n"
    "    echo after the relay 2>/dev/null\n"
    "    exit 0 ;;\n"
    "esac\n";

/* What one run gave. */
struct outcome {
    int status;
    char out[65536]; /* what it printed */
};

/* ---- Files. ---- */

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

/** Say whether `text` has a line that begins with `start`. */
static int has_start(const char *text, const char *start)
{
    const char *at;

    for (at = text; (at = strstr(at, start)) != NULL; at++) {
        if (at == text || at[-1] == '\n')
            return 1;
    }
    return 0;
}

/** Say whether `text` has the whole line `line`. */
static int has_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at;

    for (at = text; (at = strstr(at, line)) != NULL; at++) {
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\0'))
            return 1;
    }
    return 0;
}

/** Say whether a line of `text` ends with `tail`. */
static int has_end(const char *text, const char *tail)
{
    size_t n = strlen(tail);
    const char *at;

    for (at = text; (at = strstr(at, tail)) != NULL; at++) {
        if (at[n] == '\n' || at[n] == '\0')
            return 1;
    }
    return 0;
}

/** Say whether nothing has the FIFO `path` open for reading. */
static int no_reader(const char *path)
{
    int fd = open(path, O_WRONLY | O_NONBLOCK);

    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == ENXIO;
}

/** Say whether nothing has the FIFO `path` open for reading within two
 * seconds, the time an ended process takes to be reaped on a busy host. */
static int closed_soon(const char *path)
{
    int i;

    for (i = 0; i < 40 && !no_reader(path); i++)
        /* interval: a FIFO is looked at again every 50 ms */
        usleep(50000);
    return no_reader(path);
}

/**
 * Make the fixture tree, with its run lock held by this process, runutil's
 * source and the stand-in first on the path, and prepare its suites into
 * the record `record`.
 */
static void make_tree(const char *record)
{
    static const char *const dirs[] = {
        "",      "tests",           "tests/system", "tests/system/runner",
        "build", "build/.run.lock", "build/images", "qemu"};
    static const char *const fifos[] = {"hang.fifo", "started.fifo", "linger-left.fifo",
                                        "linger-lost.fifo", "linger-term.fifo"};
    static char *suites[] = {"pass", "left",  "killed", "lost", "cut",
                             "term", "quiet", "busy",   "fail", "wait"};
    static char source[1 << 20], newpath[8192];
    const char *runutil = getenv("ETUDE_LEGACYRUN_RUNUTIL"), *oldpath = getenv("PATH");
    struct etude_legacy_opts o;
    struct utimbuf old;
    char path[PATH_LEN], owner[64];
    size_t i;
    int bad = 0;

    fix_format(tree, sizeof(tree), "%s/tree", fix_dir());
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        bad |= mkdir(in(path, sizeof(path), dirs[i]), 0755) != 0;
    for (i = 0; i < sizeof(fifos) / sizeof(fifos[0]); i++)
        bad |= mkfifo(in(path, sizeof(path), fifos[i]), 0644) != 0;
    snprintf(owner, sizeof(owner), "pid %ld\n", (long)getpid());
    bad |= write_text(in(path, sizeof(path), "tests/registry"), registry, 0644);
    bad |= write_text(in(path, sizeof(path), "Makefile"), makefile, 0644);
    bad |= write_text(in(path, sizeof(path), "qemu/qemu-system-i386"), standin, 0755);
    bad |= write_text(in(path, sizeof(path), "build/.run.lock/owner"), owner, 0644);
    bad |= write_text(in(path, sizeof(path), "build/images/left-old.log"), "old\n", 0644);
    old.actime = old.modtime = 1000000000;
    bad |= utime(path, &old) != 0;
    bad |= read_text(runutil != NULL ? runutil : "tests/system/runner/runutil.c", source,
                     sizeof(source)) <= 0;
    bad |= write_text(in(path, sizeof(path), "tests/system/runner/runutil.c"), source, 0644);
    fix_format(newpath, sizeof(newpath), "%s/qemu:%s", tree,
               oldpath != NULL ? oldpath : "/usr/bin:/bin");
    bad |= setenv("PATH", newpath, 1) != 0;
    if (bad) {
        printf("etude-legacyrun-test: fail: the fixture tree %s was not made\n", tree);
        exit(1);
    }
    memset(&o, 0, sizeof(o));
    o.tree = tree;
    o.arch = "i686";
    o.record = record;
    o.seconds = 60;
    o.suites = suites;
    o.nsuites = sizeof(suites) / sizeof(suites[0]);
    o.out = stdout;
    if (etude_legacy_prepare(&o) != 0) {
        printf("etude-legacyrun-test: fail: the fixture's suites were not prepared\n");
        exit(1);
    }
}

/* ---- Runs. ---- */

/** Fill `o` for a run of `suite` with the record `record`, its work in the
 * directory `work` of the fixtures, and bounds no row reaches. */
static void opts_for(struct etude_legacy_run_opts *o, const char *record, const char *suite,
                     const char *work)
{
    memset(o, 0, sizeof(*o));
    o->record = record;
    o->suite = suite;
    o->work = fix_path(work);
    o->idle = 30;
    o->seconds = 60;
}

/** Run `o` and take what it printed into `r`. */
static void run(struct etude_legacy_run_opts *o, struct outcome *r)
{
    FILE *f = tmpfile();
    size_t n;

    r->out[0] = '\0';
    if (f == NULL) {
        printf("etude-legacyrun-test: fail: no file for the output: %s\n", strerror(errno));
        exit(1);
    }
    o->out = f;
    r->status = etude_legacy_run(o);
    rewind(f);
    n = fread(r->out, 1, sizeof(r->out) - 1, f);
    r->out[n] = '\0';
    fclose(f);
}

/** Print what a run printed when a check of its row failed. */
static void show(const struct outcome *r, int before)
{
    if (fails != before)
        printf("etude-legacyrun-test: its output:\n%s", r->out);
}

/* ---- Rows. ---- */

/** A suite that boots its machine and exits 0 passes. */
static void test_pass(const char *record)
{
    char path[PATH_LEN], args[4096];
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "pass", "pass.work");
    run(&o, &r);
    read_text(in(path, sizeof(path), "qemu.args"), args, sizeof(args));
    CHECK(r.status == 0, "pass: passed");
    CHECK(has_line(r.out, "etude-legacy: result: passed: pass"), "pass: the result");
    CHECK(has_start(r.out, "run: pass: PASS in "), "pass: the old runner's line");
    CHECK(has_line(r.out, "FIXTURE-BOOT-OK"), "pass: the console relayed");
    CHECK(has_line(r.out, "offset 7000 lc C"), "pass: the port block and the locale");
    CHECK(has_end(args, "-accel tcg -smp 2 -m 64 pass"),
          "pass: the shim's accelerator and processors");
    CHECK(!exists(o.work), "pass: the work removed");
    show(&r, before);
}

/** A suite that exits 0 with its machine still running fails, and the
 * machine is ended; the work keeps the console and the suite's own logs. */
static void test_left(const char *record)
{
    char path[PATH_LEN], console[4096];
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "left", "left.work");
    run(&o, &r);
    fix_format(path, sizeof(path), "%s/machines.console", o.work);
    read_text(path, console, sizeof(console));
    CHECK(r.status == 1, "left: failed");
    CHECK(has_start(r.out, "etude-legacy: result: failed: left: a machine was left running: pid "),
          "left: the result");
    CHECK(has_start(r.out, "run: left: FAIL in "), "left: the old runner's line");
    CHECK(has_start(r.out, "etude-legacy: left: left running: pid "), "left: the machine named");
    CHECK(closed_soon(in(path, sizeof(path), "linger-left.fifo")), "left: the machine ended");
    CHECK(has_line(console, "lingering"), "left: the console kept");
    fix_format(path, sizeof(path), "%s/images/left-test.log", o.work);
    CHECK(exists(path), "left: its log kept");
    fix_format(path, sizeof(path), "%s/images/pass-test.log", o.work);
    CHECK(!exists(path), "left: another suite's log not kept");
    fix_format(path, sizeof(path), "%s/images/left-old.log", o.work);
    CHECK(!exists(path), "left: an older log not kept");
    show(&r, before);
}

/** A suite whose make is killed fails. */
static void test_killed(const char *record)
{
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "killed", "killed.work");
    run(&o, &r);
    CHECK(r.status == 1, "killed: failed");
    CHECK(has_line(r.out, "etude-legacy: result: failed: killed: it was killed by signal 9"),
          "killed: the result");
    show(&r, before);
}

/** A suite whose supervisor is killed is incomplete, and with the POSIX
 * interfaces alone its machine is ended through its `child` records. */
static void test_lost(const char *record)
{
    char path[PATH_LEN];
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "lost", "lost.work");
    o.posix_only = 1;
    run(&o, &r);
    CHECK(r.status == 1, "lost: incomplete");
    CHECK(has_line(r.out, "etude-legacy: result: incomplete: lost: runutil left no record of its "
                          "end: it was killed by signal 9"),
          "lost: the result");
    CHECK(has_start(r.out, "run: lost: INCOMPLETE in "), "lost: the old runner's line");
    CHECK(closed_soon(in(path, sizeof(path), "linger-lost.fifo")), "lost: the machine ended");
    show(&r, before);
}

/** A suite whose machine's console was cut is incomplete. */
static void test_cut(const char *record)
{
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "cut", "cut.work");
    run(&o, &r);
    CHECK(r.status == 1, "cut: incomplete");
    CHECK(has_start(r.out, "etude-legacy: result: incomplete: cut: the console of machine "),
          "cut: the result");
    CHECK(has_start(r.out, "etude-legacy: cut: console cut: machine "), "cut: the machine named");
    show(&r, before);
}

/** A suite that stops its machine and the relay together with SIGTERM, as
 * `pkill -f` does, passes: the relay copies the console to its end. */
static void test_term(const char *record)
{
    char path[PATH_LEN];
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "term", "term.work");
    run(&o, &r);
    CHECK(r.status == 0, "term: passed");
    CHECK(has_line(r.out, "etude-legacy: result: passed: term"), "term: the result");
    CHECK(closed_soon(in(path, sizeof(path), "linger-term.fifo")), "term: the machine ended");
    show(&r, before);
}

/** A suite that does nothing past its idle limit is incomplete. */
static void test_quiet(const char *record)
{
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "quiet", "quiet.work");
    o.idle = 1;
    run(&o, &r);
    CHECK(r.status == 1, "quiet: incomplete");
    CHECK(has_line(r.out, "etude-legacy: result: incomplete: quiet: no output, console or "
                          "processor time for 1 s"),
          "quiet: the result");
    show(&r, before);
}

/** A suite past its bound is incomplete. */
static void test_busy(const char *record)
{
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "busy", "busy.work");
    o.seconds = 2;
    run(&o, &r);
    CHECK(r.status == 1, "busy: incomplete");
    CHECK(has_line(r.out, "etude-legacy: result: incomplete: busy: it ran past its bound of 2 s"),
          "busy: the result");
    CHECK(has_line(r.out, "tick 1"), "busy: its output copied as it ran");
    show(&r, before);
}

/** A target that fails fails. */
static void test_fail(const char *record)
{
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, "fail", "fail.work");
    run(&o, &r);
    CHECK(r.status == 1, "fail: failed");
    CHECK(has_line(r.out, "etude-legacy: result: failed: fail: it exited 2"), "fail: the result");
    CHECK(has_line(r.out, "failing"), "fail: its output");
    show(&r, before);
}

/* The stop signal the child of the stop row received. */
static volatile sig_atomic_t child_stop;

/** Note the stop signal in the child of the stop row. */
static void note_stop(int sig)
{
    child_stop = sig;
}

/** A stop signal ends the suite and removes the work. */
static void test_stop(const char *record)
{
    char path[PATH_LEN], got[16];
    const char *out = fix_path("stop.out");
    struct etude_legacy_run_opts o;
    struct outcome r;
    struct sigaction sa;
    struct pollfd pfd;
    int status = -1, fd, before = fails;
    pid_t pid;

    opts_for(&o, record, "wait", "stop.work");
    o.seconds = 30;
    fd = open(in(path, sizeof(path), "started.fifo"), O_RDWR);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = note_stop;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);
        o.stop = &child_stop;
        if ((o.out = fopen(out, "w")) == NULL)
            _exit(1);
        status = etude_legacy_run(&o);
        fclose(o.out);
        _exit(status & 0xff);
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    CHECK(fd >= 0 && pid > 0 && poll(&pfd, 1, START_BOUND) == 1 && read(fd, got, sizeof(got)) > 0,
          "stop: the suite started");
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM, "stop: stopped");
    CHECK(closed_soon(in(path, sizeof(path), "hang.fifo")), "stop: its recipe ended");
    CHECK(!exists(o.work), "stop: the work removed");
    if (fd >= 0)
        close(fd);
    read_text(out, r.out, sizeof(r.out));
    CHECK(has_line(r.out, "etude-legacy: result: stopped by signal 15"), "stop: the result");
    show(&r, before);
}

/** Run suite `suite` with `record` and require a refusal whose reason
 * begins with `why`, and no work made. */
static void refused(const char *record, const char *suite, const char *row, const char *why)
{
    char want[512];
    struct etude_legacy_run_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, record, suite, "refused.work");
    run(&o, &r);
    fix_format(want, sizeof(want), "etude-legacy: result: refused: %s", why);
    CHECK(r.status == 2, row);
    CHECK(has_start(r.out, want), row);
    CHECK(!exists(o.work), row);
    show(&r, before);
}

/** The requests refused before anything runs. */
static void test_refused(const char *record)
{
    char path[PATH_LEN], owner[64];

    unsetenv("RACH2_PORT_OFFSET");
    refused(record, "pass", "refused: no port block", "RACH2_PORT_OFFSET must name");
    setenv("RACH2_PORT_OFFSET", "7000", 1);
    setenv("RACH2_CPUS", "65", 1);
    refused(record, "pass", "refused: processors", "RACH2_CPUS must be");
    unsetenv("RACH2_CPUS");
    setenv("RACH2_QEMU_ACCEL", "fast", 1);
    refused(record, "pass", "refused: accelerator", "RACH2_QEMU_ACCEL must be");
    setenv("RACH2_QEMU_ACCEL", "tcg", 1);
    refused(record, "nosuch", "refused: a suite not prepared", "nosuch is no suite of");
    write_text(in(path, sizeof(path), "build/.run.lock/owner"), "pid 1\n", 0644);
    refused(record, "pass", "refused: held", "the tree is not held");
    snprintf(owner, sizeof(owner), "pid %ld\n", (long)getpid());
    write_text(in(path, sizeof(path), "build/.run.lock/owner"), owner, 0644);
    write_text(in(path, sizeof(path), "build/images/disk.img"), "changed\n", 0644);
    refused(record, "pass", "refused: changed", "the preparation changed: ");
}

int main(void)
{
    char held[32];
    const char *record;

    signal(SIGPIPE, SIG_IGN);
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    setenv("LC_ALL", "POSIX", 1);
    setenv("MAKEFLAGS", "n", 1);
    setenv("RACH2_PORT_OFFSET", "7000", 1);
    setenv("RACH2_QEMU_ACCEL", "tcg", 1);
    unsetenv("RACH2_CPUS");
    unsetenv("RACH2_DEADLINES");
    record = fix_path("record");
    make_tree(record);
    test_pass(record);
    test_left(record);
    test_killed(record);
    test_lost(record);
    test_cut(record);
    test_term(record);
    test_quiet(record);
    test_busy(record);
    test_fail(record);
    test_stop(record);
    test_refused(record);
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_legacyrun_hosttest: pass\n");
    return 0;
}
