/**
 * Check etude's per-case watchdog (tools/etude/watchdog.c) and its
 * supervisor (tools/etude/posix/supervise.c), with this program as the
 * cases' program, as a stand-in for QEMU when ETUDE_FAKE_QEMU is set, and
 * with the real QEMU on the kernel and test image of the architecture ARCH
 * names.
 *
 * host-deps: tools/etude/watchdog.c tools/etude/posix/supervise.c
 * host-deps: tools/etude/capture.c tools/etude/history_json.c
 * host-deps: tools/etude/qemu_profile.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-deps: tools/etude/posix/qemu.c tools/etude/processor.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 * host-seconds: 240
 * host-alone: a stand-in QEMU must answer three queries within 500 ms
 *
 * Bounds are fixed from samples by their policy. The watchdog alone then
 * reads report records at times the test gives: only a witness that
 * advances is progress, a quiet witness's announcement replaces the gap
 * bound, and each stage has its bound. The supervisor runs the rest at once:
 * a busy loop and a stream of heartbeats run out of their gap and are
 * stopped, one of them past SIGTERM; quiet valid work passes though it
 * prints nothing for longer than the gap; endless progress runs out of the
 * case's bound and quiet work of its own; a stand-in QEMU whose QMP hangs is
 * stopped within its bounds with the capture's failure recorded, one that
 * answers gives its state, and one whose QMP never greets runs out of its
 * boot stage; and remote cases end lost when their transport is lost or
 * silent, with their claims kept, or stopped when the case confirms its
 * end. With the real QEMU, a kernel idle at its debug shell runs out of its
 * gap, and QMP gives its status and its registers before it is stopped.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "etude/watchdog.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            printf("etude-watchdog-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                  \
            fails++;                                                                        \
        }                                                                                   \
    } while (0)

/* The completion bounds of the stand-in rows and of the real QEMU's, past
 * which the test ends. */
#define BOUND_MS      60000
#define REAL_BOUND_MS 150000

/* Microseconds in a millisecond and in a second. */
#define MS 1000u
#define S  1000000u

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* ---- The cases' program. ---- */

/** Write `text` with one write(2), as a report record must be written. */
static void say(const char *text)
{
    if (write(1, text, strlen(text)) < 0)
        _exit(120);
}

/** Use processor time until this process has used `us` microseconds. */
static void burn(uint64_t us)
{
    volatile unsigned long spin = 0;
    struct rusage ru;
    uint64_t used = 0;

    while (used < us) {
        for (spin = 0; spin < 100000; spin++) {
        }
        getrusage(RUSAGE_SELF, &ru);
        used = (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec +
               (uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
    }
}

/** Spin forever, with SIGTERM ignored when `deaf` is set. */
static void busy(int deaf)
{
    volatile unsigned long spin = 0;

    if (deaf)
        signal(SIGTERM, SIG_IGN);
    say("@etude 1 begin host.watchdog.busy\n");
    for (;;)
        spin++;
}

/** Print heartbeats forever: the witness at its units, records that go
 * back, name no witness or do not parse, and other output. */
static void heartbeat(void)
{
    struct timespec t = {0, 20000000};

    say("@etude 1 begin host.watchdog.heartbeat\n@etude 1 progress tick 3\n");
    for (;;) {
        say("@etude 1 progress tick 3\n");
        say("@etude 1 progress tick 2\n");
        say("@etude 1 progress other 9\n");
        say("@etude 1 progress tick x\n");
        say("still alive\n");
        /* interval: a heartbeat every 20 ms, which is no progress */
        nanosleep(&t, NULL);
    }
}

/** Announce quiet work, do it in a child that prints nothing, and report
 * it done; with `forever`, the child never ends. */
static int quiet(int forever)
{
    pid_t pid;
    int status;

    say("@etude 1 begin host.watchdog.quiet\n@etude 1 progress compile 0\n");
    pid = fork();
    if (pid < 0)
        _exit(121);
    if (pid == 0) {
        if (forever)
            for (;;)
                pause();
        burn(2 * S);
        _exit(0);
    }
    if (waitpid(pid, &status, 0) != pid || status != 0)
        return 1;
    say("@etude 1 progress compile 1\n@etude 1 progress steps 1\n");
    return 0;
}

/** Advance a witness without limit forever. */
static void endless(void)
{
    struct timespec t = {0, 20000000};
    char line[64];
    unsigned long n;

    for (n = 1;; n++) {
        snprintf(line, sizeof(line), "@etude 1 progress steps %lu\n", n);
        say(line);
        /* interval: progress every 20 ms, which the case's bound must end */
        nanosleep(&t, NULL);
    }
}

/** Be the case `role` names. */
static int case_main(const char *role)
{
    if (strcmp(role, "busy") == 0 || strcmp(role, "busy-deaf") == 0)
        busy(strcmp(role, "busy-deaf") == 0);
    if (strcmp(role, "heartbeat") == 0)
        heartbeat();
    if (strcmp(role, "quiet") == 0 || strcmp(role, "quiet-forever") == 0)
        return quiet(strcmp(role, "quiet-forever") == 0);
    if (strcmp(role, "endless") == 0)
        endless();
    return 125;
}

/* ---- The stand-in for QEMU. ---- */

/** Write all of `text` to `fd`, or end the stand-in. */
static void put(int fd, const char *text)
{
    size_t len = strlen(text), done = 0;
    ssize_t n;

    while (done < len) {
        n = write(fd, text + done, len - done);
        if (n <= 0)
            _exit(120);
        done += (size_t)n;
    }
}

/** Connect to the Unix socket at `path`, as QEMU's client chardev does. */
static int fake_connect(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
        _exit(121);
    return fd;
}

/** Read one line of QMP into `line`, without its newline; -1 at its end. */
static int fake_line(int fd, char *line, size_t size)
{
    size_t n = 0;
    char c;

    for (;;) {
        if (read(fd, &c, 1) != 1)
            return -1;
        if (c == '\n')
            break;
        if (n + 1 < size)
            line[n++] = c;
    }
    line[n] = '\0';
    return 0;
}

/** Give the command a QMP line executes, in `name`, and its id. */
static long fake_command(const char *line, char *name, size_t size)
{
    const char *at = strstr(line, "\"execute\":\""), *id = strstr(line, "\"id\":");
    size_t n = 0;

    name[0] = '\0';
    if (at == NULL)
        return -1;
    for (at += 11; *at != '\0' && *at != '"' && n + 1 < size; at++)
        name[n++] = *at;
    name[n] = '\0';
    return id != NULL ? strtol(id + 5, NULL, 10) : -1;
}

/** Find the path of the `-chardev` socket of id `id` in QEMU's arguments. */
static const char *fake_socket(int argc, char **argv, const char *id)
{
    char head[64];
    int i;

    snprintf(head, sizeof(head), "socket,id=%s,path=", id);
    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "-chardev") == 0 && strncmp(argv[i + 1], head, strlen(head)) == 0)
            return argv[i + 1] + strlen(head);
    _exit(124);
}

/**
 * Answer QMP: the start of a machine on accelerator `accel`, and with
 * `answer` the commands that read its state and the quit. Without it QMP
 * stops answering after the start, and SIGTERM is ignored.
 */
static void fake_qmp(int qmp, const char *accel, int answer)
{
    char line[512], name[64], text[512];
    long id;

    put(qmp, "{\"QMP\": {\"version\": {\"qemu\": {\"micro\": 0, \"minor\": 0, \"major\": 9}, "
             "\"package\": \"\"}, \"capabilities\": [\"oob\"]}}\r\n");
    while (fake_line(qmp, line, sizeof(line)) == 0) {
        id = fake_command(line, name, sizeof(name));
        if (strcmp(name, "query-accelerators") == 0)
            snprintf(text, sizeof(text),
                     "{\"return\": {\"enabled\": \"%s\", \"present\": [\"%s\"]}, \"id\": %ld}\r\n",
                     accel, accel, id);
        else if (strcmp(name, "query-status") == 0)
            snprintf(text, sizeof(text),
                     "{\"return\": {\"status\": \"running\", \"running\": true}, \"id\": %ld}\r\n",
                     id);
        else if (strcmp(name, "query-cpus-fast") == 0)
            snprintf(text, sizeof(text),
                     "{\"return\": [{\"cpu-index\": 0, \"thread-id\": 7, \"target\": \"i386\"}], "
                     "\"id\": %ld}\r\n",
                     id);
        else if (strcmp(name, "human-monitor-command") == 0)
            snprintf(text, sizeof(text),
                     "{\"return\": \"CPU#0\\r\\nEIP=00100000\\r\\n\", \"id\": %ld}\r\n", id);
        else
            snprintf(text, sizeof(text), "{\"return\": {}, \"id\": %ld}\r\n", id);
        put(qmp, text);
        if (strcmp(name, "quit") == 0)
            _exit(0);
        if (strcmp(name, "cont") == 0 && !answer) {
            signal(SIGTERM, SIG_IGN);
            for (;;)
                pause();
        }
    }
    _exit(0);
}

/** Be QEMU, as ETUDE_FAKE_QEMU's `mode` says: "hung", "answer" or "mute". */
static int fake_qemu(const char *mode, int argc, char **argv)
{
    const char *accel = NULL;
    int i, con, qmp;

    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "-accel") == 0)
            accel = argv[i + 1];
    if (accel == NULL)
        return 125;
    con = fake_connect(fake_socket(argc, argv, "etude-console"));
    qmp = fake_connect(fake_socket(argc, argv, "etude-qmp"));
    put(con, "fake: up\r\n");
    if (strcmp(mode, "mute") == 0)
        for (;;)
            pause();
    fake_qmp(qmp, accel, strcmp(mode, "answer") == 0);
    return 0;
}

/* ---- The bounds and the watchdog alone. ---- */

/** Fix `b` from `n` samples and require `want`, or an error when 0. */
static void fix_row(const char *row, const struct etude_bound *b, const uint64_t *v, size_t n,
                    uint64_t want, int clamped)
{
    struct etude_bound_fixed f;
    int rc = etude_bound_fix(b, v, n, &f);

    CHECK(rc == 0, row);
    if (rc != 0)
        return;
    if (f.us != want) {
        printf("etude-watchdog-test: fail: %s: fixed %llu us, not %llu\n", row,
               (unsigned long long)f.us, (unsigned long long)want);
        fails++;
    }
    CHECK(f.clamped == clamped, row);
    CHECK(f.samples == (n >= ETUDE_BOUND_SAMPLES ? n : 0), row);
}

/** Fix bounds from samples by their policy. */
static void test_bounds(void)
{
    static const uint64_t ten[] = {10 * S, 10 * S, 10 * S, 10 * S, 10 * S};
    static const uint64_t spread[] = {100 * S, 90 * S, 110 * S, 120 * S, 100 * S};
    static const uint64_t even[] = {100 * S, 110 * S, 120 * S, 130 * S, 140 * S, 150 * S};
    static const uint64_t odd_us[] = {1, 2, 3, 4, 6};
    static const uint64_t half_us[] = {1, 2, 3, 4, 5, 7};
    static const uint64_t huge[] = {UINT64_MAX / 2, 1, 1, 1, 1};
    struct etude_bound b = {1 * S, 60 * S, 900 * S}, tiny = {1, 10, 100};
    struct etude_bound_fixed f;

    fix_row("no history", &b, NULL, 0, 60 * S, 0);
    fix_row("four samples", &b, ten, 4, 60 * S, 0);
    fix_row("five equal samples", &b, ten, 5, 20 * S, 0);
    fix_row("a spread of five", &b, spread, 5, 2 * (120 * S + 44478000u), 0);
    fix_row("an even count", &b, even, 6, 433434000u, 0);
    fix_row("an upward rounding", &tiny, odd_us, 5, 22, 0);
    fix_row("a median between samples", &tiny, half_us, 6, 28, 0);
    b.ceiling_us = 300 * S;
    fix_row("the ceiling", &b, even, 6, 300 * S, 1);
    b.floor_us = 30 * S;
    b.initial_us = 60 * S;
    fix_row("the floor", &b, ten, 5, 30 * S, -1);
    b.floor_us = 0;
    CHECK(etude_bound_fix(&b, ten, 5, &f) == EINVAL, "a floor of 0");
    b.floor_us = 70 * S;
    CHECK(etude_bound_fix(&b, ten, 5, &f) == EINVAL, "a floor above the initial value");
    b.floor_us = 1 * S;
    b.ceiling_us = 50 * S;
    CHECK(etude_bound_fix(&b, ten, 5, &f) == EINVAL, "an initial value above the ceiling");
    b.ceiling_us = 900 * S;
    CHECK(etude_bound_fix(&b, huge, 5, &f) == EOVERFLOW, "a sample too large");
}

/** Feed `text` to watchdog `w` at `t`. */
static void feed(struct etude_watchdog *w, const char *text, uint64_t t)
{
    etude_watchdog_feed(w, text, strlen(text), t);
}

/** Read records, heartbeats, quiet work and stages at times the test gives. */
static void test_watchdog(void)
{
    static const struct etude_witness_bound witnesses[] = {
        {"steps", ETUDE_WITNESS_STEP, 3, 0},
        {"bytes", ETUDE_WITNESS_COUNTER, 1000, 0},
        {"compile", ETUDE_WITNESS_QUIET, 0, 50 * S},
    };
    struct etude_watchdog_spec spec;
    const struct etude_progress *p;
    struct etude_watchdog *w;
    char line[400];

    memset(&spec, 0, sizeof(spec));
    spec.case_us = 600 * S;
    spec.gap_us = 10 * S;
    spec.stage_us[ETUDE_STAGE_BOOT] = 30 * S;
    spec.witnesses = witnesses;
    spec.witness_count = 3;
    w = etude_watchdog_new(&spec, ETUDE_STAGE_BOOT, 1000 * S);
    CHECK(w != NULL, "watchdog");
    if (w == NULL)
        return;
    p = etude_watchdog_progress(w);
    CHECK(etude_watchdog_deadline(w) == 1030 * S, "the boot stage's bound");
    CHECK(etude_watchdog_check(w, 1029 * S) == ETUDE_EXPIRY_NONE, "the boot stage's bound");
    etude_watchdog_stage(w, ETUDE_STAGE_EXECUTE, 1020 * S);
    CHECK(etude_watchdog_deadline(w) == 1030 * S, "the gap starts with execution");
    feed(w, "@etude 1 progress steps 1\n", 1022 * S);
    CHECK(p->advances == 1 && etude_watchdog_deadline(w) == 1032 * S, "a step advances");
    feed(w, "@etude 1 progress steps 1\nother output\n", 1025 * S);
    feed(w, "@etude 1 progress steps 0\n@etude 1 progress steps 4\n", 1026 * S);
    feed(w, "@etude 1 progress nothing 5\n@etude 1 progress steps\n", 1027 * S);
    feed(w, "@etude 1 progress steps 2 more\n@etude 1 progress steps -2\n", 1027 * S);
    feed(w, "@etude 1  progress steps 2\n@etude 1 assert x pass\n", 1027 * S);
    CHECK(p->advances == 1 && p->repeats == 1 && p->refused == 6, "records that are not progress");
    CHECK(etude_watchdog_deadline(w) == 1032 * S, "heartbeats do not move the gap");
    CHECK(p->bytes_since > 0 && strcmp(p->last_witness, "steps") == 0, "output since progress");
    memset(line, 'x', sizeof(line));
    memcpy(line, "@etude 1 progress steps 2 ", 26);
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = '\0';
    feed(w, line, 1028 * S);
    CHECK(p->advances == 1, "a line longer than a record");
    feed(w, "@etude 1 progress by", 1029 * S);
    feed(w, "tes 400\r\n", 1030 * S);
    CHECK(p->advances == 2 && p->last_units == 400, "a record in two pieces");
    CHECK(p->max_gap_us == 8 * S, "the longest gap");
    feed(w, "@etude 1 progress compile 0\n", 1035 * S);
    CHECK(p->quiet != NULL && etude_watchdog_deadline(w) == 1085 * S, "quiet work announced");
    feed(w, "@etude 1 progress compile 0\n", 1060 * S);
    CHECK(etude_watchdog_deadline(w) == 1085 * S, "an announcement repeated");
    CHECK(etude_watchdog_check(w, 1084 * S) == ETUDE_EXPIRY_NONE, "quiet work in its bound");
    feed(w, "@etude 1 progress compile 1\n", 1084 * S);
    CHECK(p->quiet == NULL && etude_watchdog_deadline(w) == 1094 * S, "quiet work done");
    CHECK(etude_watchdog_max_gap(w, 1090 * S) == 54 * S, "a quiet gap");
    feed(w, "@etude 1 progress compile 1\n", 1090 * S);
    CHECK(etude_watchdog_deadline(w) == 1140 * S, "quiet work announced again");
    feed(w, "@etude 1 progress steps 3\n", 1100 * S);
    CHECK(etude_watchdog_deadline(w) == 1140 * S, "progress beside quiet work");
    CHECK(etude_watchdog_check(w, 1140 * S) == ETUDE_EXPIRY_QUIET, "quiet work past its bound");
    CHECK(etude_watchdog_bound(w) == 50 * S, "the quiet bound");
    feed(w, "@etude 1 progress compile 2\n", 1141 * S);
    CHECK(etude_watchdog_check(w, 2000 * S) == ETUDE_EXPIRY_QUIET, "the first expiry is kept");
    CHECK(etude_watchdog_expired(w)->advances == 4, "the progress at the expiry");
    etude_watchdog_stage(w, ETUDE_STAGE_CLEANUP, 1220 * S);
    CHECK(etude_watchdog_max_gap(w, 1500 * S) == 79 * S, "the last gap");
    etude_watchdog_free(w);

    spec.gap_us = 700 * S;
    w = etude_watchdog_new(&spec, ETUDE_STAGE_EXECUTE, 0);
    CHECK(w != NULL, "watchdog");
    if (w == NULL)
        return;
    CHECK(etude_watchdog_check(w, 599 * S) == ETUDE_EXPIRY_NONE, "the case's bound");
    CHECK(etude_watchdog_check(w, 600 * S) == ETUDE_EXPIRY_CASE, "the case's bound");
    etude_watchdog_free(w);
    spec.gap_us = 10 * S;
    w = etude_watchdog_new(&spec, ETUDE_STAGE_BOOT, 0);
    CHECK(w != NULL, "watchdog");
    if (w == NULL)
        return;
    CHECK(etude_watchdog_check(w, 30 * S) == ETUDE_EXPIRY_STAGE, "the boot stage runs out");
    etude_watchdog_free(w);
}

/* ---- The supervisor. ---- */

/* One attempt the supervisor runs, and what the test does with it. */
struct row {
    const char *name;
    enum etude_supervised_kind kind;
    const char *mode; /* the case's role or the stand-in's mode */
    struct etude_watchdog_spec spec;
    struct etude_supervised *a;
    int done;
    int staged; /* a machine was moved to execution */
    int acted;  /* the test acted on a remote row */
    int asked;  /* the ask function was called */
    uint64_t seen_us;
    char env[64];
    char **envp;
    struct etude_qemu_profile profile;
    struct etude_qemu_site site;
    char kernel[PATH_MAX], initrd[PATH_MAX];
};

static const struct etude_witness_bound host_witnesses[] = {
    {"tick", ETUDE_WITNESS_STEP, 0, 0},
    {"steps", ETUDE_WITNESS_STEP, 0, 0},
    {"compile", ETUDE_WITNESS_QUIET, 0, 60 * S},
};

static const struct etude_witness_bound short_quiet[] = {
    {"compile", ETUDE_WITNESS_QUIET, 0, 500 * MS},
};

/** Fill the bounds of row `r`, its case and its gap, with host witnesses. */
static void row_bounds(struct row *r, uint64_t whole, uint64_t gap)
{
    memset(&r->spec, 0, sizeof(r->spec));
    r->spec.case_us = whole;
    r->spec.gap_us = gap;
    r->spec.witnesses = host_witnesses;
    r->spec.witness_count = 3;
}

/** Give a copy of this program's environment with NAME=VALUE added. */
static char **env_with(struct row *r, const char *name, const char *value)
{
    size_t n = 0;

    while (environ[n] != NULL)
        n++;
    r->envp = calloc(n + 2, sizeof(*r->envp));
    if (r->envp == NULL)
        exit(1);
    memcpy(r->envp, environ, n * sizeof(*r->envp));
    snprintf(r->env, sizeof(r->env), "%s=%s", name, value);
    r->envp[n] = r->env;
    return r->envp;
}

/** Start the host row `r` on supervisor `s`. */
static void start_host(struct etude_supervisor *s, struct row *r)
{
    char *argv[] = {self, NULL};
    struct etude_exec_spec es;

    memset(&es, 0, sizeof(es));
    es.attempt_id = r->name;
    es.argv = argv;
    es.envp = env_with(r, "ETUDE_WATCHDOG_ROLE", r->mode);
    es.output_max = 1u << 20;
    es.grace_us = 300 * MS;
    if (etude_supervise_host(s, &es, &r->spec, &r->a) != 0) {
        printf("etude-watchdog-test: fail: %s: the start failed: %s\n", r->name, strerror(errno));
        fails++;
        r->a = NULL;
    }
}

/** Start the machine row `r` on supervisor `s`, with QEMU `q32` and `q64`. */
static void start_machine(struct etude_supervisor *s, struct row *r, const char *q32,
                          const char *q64, uint64_t grace, int console_progress)
{
    struct etude_machine_spec ms;

    r->site.accel = ETUDE_ACCEL_TCG;
    r->site.qemu_i686 = q32;
    r->site.qemu_x86_64 = q64;
    if (r->profile.architecture == NULL)
        r->profile.architecture = "i686";
    if (r->profile.kernel == NULL)
        r->profile.kernel = "kernel";
    r->profile.cpu = "max";
    r->profile.processors = 1;
    r->profile.memory_bytes = 128u << 20;
    r->profile.firmware = ETUDE_FIRMWARE_DIRECT;
    memset(&ms, 0, sizeof(ms));
    ms.attempt_id = r->name;
    ms.profile = &r->profile;
    ms.site = &r->site;
    ms.envp = env_with(r, "ETUDE_FAKE_QEMU", r->mode != NULL ? r->mode : "");
    ms.console_max = 1u << 20;
    ms.grace_us = grace;
    if (etude_supervise_machine(s, &ms, &r->spec, console_progress, NULL, &r->a) != 0) {
        printf("etude-watchdog-test: fail: %s: the start failed: %s\n", r->name, strerror(errno));
        fails++;
        r->a = NULL;
    }
}

/** Ask a remote row's case to stop, as a cancel frame would. */
static int ask_remote(void *ctx, const char *attempt_id)
{
    struct row *r = ctx;

    CHECK(strcmp(attempt_id, r->name) == 0, r->name);
    r->asked++;
    return 0;
}

/** Start the remote row `r` on supervisor `s`. */
static void start_remote(struct etude_supervisor *s, struct row *r)
{
    struct etude_ask ask = {ask_remote, NULL};

    ask.ctx = r;
    if (etude_supervise_remote(s, r->name, &r->spec, 300 * MS, &ask, &r->a) != 0) {
        printf("etude-watchdog-test: fail: %s: the start failed: %s\n", r->name, strerror(errno));
        fails++;
        r->a = NULL;
    }
}

/** Give the monotonic clock in microseconds. */
static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/** Say whether the console of `res` has `text` after `from`. */
static int console_has(const struct etude_machine_result *res, const char *text, size_t from)
{
    size_t len = strlen(text), i;

    for (i = from; i + len <= res->console.kept; i++)
        if (memcmp(res->console.data + i, text, len) == 0)
            return 1;
    return 0;
}

/* What a remote case sends before its transport is lost. */
static const char lost_report[] = "@etude 1 begin guest.x\n@etude 1 progress steps 1\n";

/**
 * Act on row `r` as the test's transport and scenario: move a machine to
 * execution once it runs, or for the real QEMU at its shell's prompt; feed a
 * remote case's progress and then lose its transport, or confirm the end of
 * a case asked to stop.
 */
static void row_act(struct row *r)
{
    const struct etude_supervised_result *res = etude_supervised_result(r->a);

    if (res->finished)
        return;
    if (r->kind == ETUDE_SUPERVISED_MACHINE && !r->staged &&
        res->machine->state == ETUDE_MACHINE_RUNNING &&
        (r->mode != NULL || console_has(res->machine, "Kernel debug shell active.", 0))) {
        r->staged = 1;
        etude_supervised_stage(r->a, ETUDE_STAGE_EXECUTE);
    }
    if (r->kind != ETUDE_SUPERVISED_REMOTE || r->acted)
        return;
    if (strcmp(r->mode, "lost") == 0) {
        r->acted = 1;
        etude_supervised_feed(r->a, lost_report, strlen(lost_report));
        etude_supervised_lost(r->a);
    } else if (strcmp(r->mode, "confirm") == 0 && r->asked) {
        r->acted = 1;
        etude_supervised_end(r->a, ETUDE_END_STOPPED, -1, ETUDE_CLEANUP_CONFIRMED);
    }
}

/**
 * Run the `n` rows at once until every one has finished, acting on each;
 * past `bound_ms` the test ends at once, and its keepers end what the rows
 * left.
 */
static void run_rows(struct etude_supervisor *s, struct row *rows, int n, int bound_ms)
{
    uint64_t bound = now_us() + (uint64_t)bound_ms * MS;
    struct etude_supervised *a;
    int i, left = 0;

    for (i = 0; i < n; i++)
        left += rows[i].a != NULL;
    while (left > 0) {
        a = etude_supervise_wait(s, 200);
        for (i = 0; i < n; i++) {
            if (rows[i].a == NULL)
                continue;
            if (rows[i].a == a) {
                rows[i].done = 1;
                rows[i].seen_us = now_us();
                left--;
            }
            row_act(&rows[i]);
        }
        if (now_us() > bound) {
            for (i = 0; i < n; i++)
                if (rows[i].a != NULL && !rows[i].done)
                    printf("etude-watchdog-test: fail: %s: did not finish within %d ms\n",
                           rows[i].name, bound_ms);
            exit(1);
        }
    }
}

static const char *const ends[] = {"exited",      "signaled",     "init_exited", "init_killed",
                                   "panic",       "reset",        "shutdown",    "halted",
                                   "qemu_exited", "accel_failed", "stopped",     "lost"};

/** Require row `r` to have ended `end` after expiry `expiry`. */
static int row_ended(const struct row *r, enum etude_end end, enum etude_expiry expiry)
{
    const struct etude_supervised_result *res;

    if (r->a == NULL)
        return 0;
    res = etude_supervised_result(r->a);
    CHECK(res->finished && strcmp(res->attempt_id, r->name) == 0, r->name);
    if (res->end != end || res->expiry != expiry) {
        printf("etude-watchdog-test: fail: %s: ended %s after expiry %s, not %s after %s\n",
               r->name, ends[res->end], etude_expiry_name(res->expiry), ends[end],
               etude_expiry_name(expiry));
        fails++;
        return 0;
    }
    if (expiry != ETUDE_EXPIRY_NONE) {
        CHECK(res->incomplete && res->expired != NULL, r->name);
        CHECK(res->expired_us != 0 && res->finished_us >= res->expired_us, r->name);
    }
    return 1;
}

/** Check a host row stopped by its watchdog: its tree ended, the evidence. */
static void check_stopped_host(const struct row *r, int deaf)
{
    const struct etude_supervised_result *res = etude_supervised_result(r->a);

    CHECK(res->asked && res->cleanup == ETUDE_CLEANUP_CONFIRMED && !res->claim_kept, r->name);
    CHECK(res->owned_count == 2 && res->owned[1].pid == res->proc->process.pid, r->name);
    CHECK(res->proc->exit_end == ETUDE_END_SIGNALED &&
              res->proc->exit_value == (deaf ? SIGKILL : SIGTERM),
          r->name);
    CHECK(res->finished_us - res->expired_us < 10 * S, r->name);
}

/** Check the busy loops and the heartbeats, which run out of their gap. */
static void check_gap_rows(struct row *rows)
{
    const struct etude_supervised_result *res;

    if (row_ended(&rows[0], ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(rows[0].a);
        check_stopped_host(&rows[0], 0);
        CHECK(res->expired->advances == 0 && res->bound_us == 1 * S, "busy");
#if defined(__linux__)
        CHECK(res->sample_cpu_us.quality == ETUDE_QUALITY_LOWER_BOUND &&
                  res->sample_cpu_us.value > 0 && res->sample_rss_bytes.value > 0,
              "busy: processor use is no progress");
#endif
    }
    if (row_ended(&rows[1], ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP))
        check_stopped_host(&rows[1], 1);
    if (row_ended(&rows[2], ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(rows[2].a);
        check_stopped_host(&rows[2], 0);
        CHECK(res->expired->advances == 1 && res->expired->repeats > 1, "heartbeat");
        CHECK(res->expired->refused > 1 && res->expired->bytes_since > 0, "heartbeat");
    }
}

/** Check quiet work, quiet work that never ends, and endless progress. */
static void check_work_rows(struct row *rows)
{
    const struct etude_supervised_result *res;

    if (row_ended(&rows[3], ETUDE_END_EXITED, ETUDE_EXPIRY_NONE)) {
        res = etude_supervised_result(rows[3].a);
        CHECK(res->value == 0 && !res->incomplete && res->cleanup == ETUDE_CLEANUP_CONFIRMED,
              "quiet");
        CHECK(res->progress->advances == 2 && res->max_gap_us > rows[3].spec.gap_us,
              "quiet: its work outlasted the gap");
        CHECK(res->owned_count == 0 && res->console_tail == NULL, "quiet: nothing was saved");
    }
    if (row_ended(&rows[4], ETUDE_END_STOPPED, ETUDE_EXPIRY_QUIET)) {
        res = etude_supervised_result(rows[4].a);
        CHECK(res->bound_us == 500 * MS && strcmp(res->expired->quiet, "compile") == 0,
              "quiet-forever");
        CHECK(res->cleanup == ETUDE_CLEANUP_CONFIRMED, "quiet-forever: its child was ended");
    }
    if (row_ended(&rows[5], ETUDE_END_STOPPED, ETUDE_EXPIRY_CASE)) {
        res = etude_supervised_result(rows[5].a);
        CHECK(res->expired->advances > 0 && res->bound_us == 1500 * MS, "endless");
    }
}

/** Check the remote cases whose transport was lost or went silent. */
static void check_remote_rows(struct row *rows)
{
    const struct etude_supervised_result *res;

    if (row_ended(&rows[6], ETUDE_END_LOST, ETUDE_EXPIRY_NONE)) {
        res = etude_supervised_result(rows[6].a);
        CHECK(res->incomplete && res->cleanup == ETUDE_CLEANUP_UNCONFIRMED && res->claim_kept,
              "lost: the claims stay");
        CHECK(!res->asked && rows[6].asked == 0 && res->progress->advances == 1, "lost");
        CHECK(rows[6].seen_us < rows[3].seen_us, "lost: the others went on");
    }
    if (row_ended(&rows[7], ETUDE_END_LOST, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(rows[7].a);
        CHECK(res->asked && rows[7].asked == 1 && res->ask_error == 0, "silent");
        CHECK(res->cleanup == ETUDE_CLEANUP_UNCONFIRMED && res->claim_kept, "silent");
        CHECK(res->owned_count == 0, "silent");
    }
}

/** Free the attempts of the `n` rows, the supervisor, and the rows' copies. */
static void rows_free(struct etude_supervisor *s, struct row *rows, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (rows[i].a != NULL)
            etude_supervised_free(rows[i].a);
    etude_supervisor_free(s);
    for (i = 0; i < n; i++)
        free(rows[i].envp);
}

/** Run the host rows and the remote rows at once. */
static void test_host_and_remote(void)
{
    static const char *const names[] = {"busy",          "busy-deaf", "heartbeat", "quiet",
                                        "quiet-forever", "endless",   "lost",      "silent"};
    static const uint64_t bounds[][2] = {{30 * S, 1 * S},    {30 * S, 1 * S},   {5 * S, 300 * MS},
                                         {60 * S, 1 * S},    {30 * S, 30 * S},  {1500 * MS, 2 * S},
                                         {30 * S, 300 * MS}, {30 * S, 300 * MS}};
    static struct row rows[8];
    struct etude_supervisor *s = etude_supervisor_new();
    int i, n = 8;

    CHECK(s != NULL, "supervisor");
    if (s == NULL)
        return;
    memset(rows, 0, sizeof(rows));
    for (i = 0; i < n; i++) {
        rows[i].name = rows[i].mode = names[i];
        rows[i].kind = i < 6 ? ETUDE_SUPERVISED_HOST : ETUDE_SUPERVISED_REMOTE;
        row_bounds(&rows[i], bounds[i][0], bounds[i][1]);
    }
    rows[4].spec.witnesses = short_quiet;
    rows[4].spec.witness_count = 1;
    for (i = 0; i < n; i++)
        if (rows[i].kind == ETUDE_SUPERVISED_HOST)
            start_host(s, &rows[i]);
        else
            start_remote(s, &rows[i]);
    run_rows(s, rows, n, BOUND_MS);
    check_gap_rows(rows);
    check_work_rows(rows);
    check_remote_rows(rows);
    rows_free(s, rows, n);
}

/** Run the stand-in machines and a remote case that confirms its end. */
static void test_machines(void)
{
    static struct row rows[4];
    struct etude_supervisor *s = etude_supervisor_new();
    const struct etude_supervised_result *res;
    int i, n = 4;

    CHECK(s != NULL, "supervisor");
    if (s == NULL)
        return;
    memset(rows, 0, sizeof(rows));
    rows[0].name = rows[0].mode = "hung";
    rows[1].name = rows[1].mode = "answer";
    rows[2].name = rows[2].mode = "mute";
    for (i = 0; i < 3; i++) {
        rows[i].kind = ETUDE_SUPERVISED_MACHINE;
        row_bounds(&rows[i], 60 * S, 300 * MS);
        rows[i].spec.stage_us[ETUDE_STAGE_CAPTURE] = 500 * MS;
        rows[i].spec.stage_us[ETUDE_STAGE_BOOT] = 30 * S;
    }
    rows[2].spec.stage_us[ETUDE_STAGE_BOOT] = 300 * MS;
    rows[3].name = rows[3].mode = "confirm";
    rows[3].kind = ETUDE_SUPERVISED_REMOTE;
    row_bounds(&rows[3], 30 * S, 300 * MS);
    for (i = 0; i < 3; i++)
        start_machine(s, &rows[i], self, self, 300 * MS, 0);
    start_remote(s, &rows[3]);
    run_rows(s, rows, n, BOUND_MS);

    if (row_ended(&rows[0], ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(rows[0].a);
        CHECK(res->qmp_status == NULL && res->qmp_cpus == NULL && res->qmp_registers == NULL,
              "hung");
        CHECK(strstr(res->capture_error, "query-status: QMP did not answer") != NULL, "hung");
        CHECK(strstr(res->capture_error, "human-monitor-command: QMP did not") != NULL, "hung");
        CHECK(res->machine->qemu->exit_end == ETUDE_END_SIGNALED &&
                  res->machine->qemu->exit_value == SIGKILL,
              "hung: QEMU was killed");
        CHECK(res->cleanup == ETUDE_CLEANUP_CONFIRMED && !res->claim_kept, "hung");
        CHECK(res->owned_count == 2 && res->owned[1].pid == res->machine->qemu->process.pid,
              "hung");
        CHECK(res->console_tail != NULL && strcmp(res->console_tail, "fake: up\r\n") == 0, "hung");
        CHECK(res->finished_us - res->expired_us < 10 * S, "hung: QMP did not delay the end");
    }
    if (row_ended(&rows[1], ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(rows[1].a);
        CHECK(res->capture_error[0] == '\0', "answer");
        CHECK(res->qmp_status != NULL && strstr(res->qmp_status, "\"running\"") != NULL, "answer");
        CHECK(res->qmp_cpus != NULL && strstr(res->qmp_cpus, "cpu-index") != NULL, "answer");
        CHECK(res->qmp_registers != NULL && strstr(res->qmp_registers, "EIP=") != NULL, "answer");
        CHECK(res->machine->qemu->exit_end == ETUDE_END_EXITED, "answer: QEMU obeyed its quit");
    }
    if (row_ended(&rows[2], ETUDE_END_STOPPED, ETUDE_EXPIRY_STAGE)) {
        res = etude_supervised_result(rows[2].a);
        CHECK(res->expired->stage == ETUDE_STAGE_BOOT && res->bound_us == 300 * MS, "mute");
        CHECK(strstr(res->capture_error, "QMP had not started the machine") != NULL, "mute");
    }
    if (row_ended(&rows[3], ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(rows[3].a);
        CHECK(res->asked && res->cleanup == ETUDE_CLEANUP_CONFIRMED && !res->claim_kept, "confirm");
        CHECK(res->incomplete, "confirm");
    }
    rows_free(s, rows, n);
}

/**
 * Give the path of `name` on PATH in `out`, or NULL when it is not there,
 * past the directory of tests/run's QEMU wrappers, which add an accelerator
 * of their own.
 */
static const char *on_path(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH"), *shims = getenv("RACH2_QEMU_SHIMS"), *end;
    size_t len;

    for (; path != NULL && *path != '\0'; path = *end == ':' ? end + 1 : end) {
        end = strchr(path, ':');
        if (end == NULL)
            end = path + strlen(path);
        len = (size_t)(end - path);
        if (len == 0 || (shims != NULL && len == strlen(shims) && strncmp(path, shims, len) == 0))
            continue;
        snprintf(out, size, "%.*s/%s", (int)len, path, name);
        if (access(out, X_OK) == 0)
            return out;
    }
    return NULL;
}

/**
 * Boot the kernel of the architecture ARCH names in the real QEMU under TCG
 * and leave it idle at its debug shell until its gap runs out. A host
 * without QEMU or a tree without the kernel fails the row.
 */
static void test_real(void)
{
    const char *arch = getenv("ARCH") != NULL && *getenv("ARCH") != '\0' ? getenv("ARCH") : "i686";
    const char *dir = strcmp(arch, "x86_64") == 0 ? "build/x86_64" : "build";
    const char *reg = strcmp(arch, "x86_64") == 0 ? "RIP=" : "EIP=";
    const struct etude_supervised_result *res;
    char q32[PATH_MAX], q64[PATH_MAX];
    struct etude_supervisor *s;
    static struct row r;

    memset(&r, 0, sizeof(r));
    r.name = "real";
    r.kind = ETUDE_SUPERVISED_MACHINE;
    row_bounds(&r, 200 * S, 2 * S);
    r.spec.stage_us[ETUDE_STAGE_BOOT] = 150 * S;
    r.spec.stage_us[ETUDE_STAGE_CAPTURE] = 30 * S;
    if (strcmp(arch, "i686") != 0 && strcmp(arch, "x86_64") != 0) {
        printf("etude-watchdog-test: fail: ARCH is i686 or x86_64, not %s\n", arch);
        fails++;
        return;
    }
    snprintf(r.kernel, sizeof(r.kernel), "%s/rach2/kernel", dir);
    snprintf(r.initrd, sizeof(r.initrd), "%s/images/test.krffs", dir);
    if (on_path("qemu-system-i386", q32, sizeof(q32)) == NULL ||
        on_path("qemu-system-x86_64", q64, sizeof(q64)) == NULL || access(r.kernel, R_OK) != 0 ||
        access(r.initrd, R_OK) != 0) {
        printf("etude-watchdog-test: fail: cannot boot: QEMU is not on PATH, or %s or %s is "
               "not built\n",
               r.kernel, r.initrd);
        fails++;
        return;
    }
    r.profile.architecture = arch;
    r.profile.kernel = r.kernel;
    r.profile.initrd = r.initrd;
    s = etude_supervisor_new();
    CHECK(s != NULL, "supervisor");
    if (s == NULL)
        return;
    start_machine(s, &r, q32, q64, 5 * S, 1);
    run_rows(s, &r, 1, REAL_BOUND_MS);
    if (row_ended(&r, ETUDE_END_STOPPED, ETUDE_EXPIRY_GAP)) {
        res = etude_supervised_result(r.a);
        CHECK(r.staged && res->capture_error[0] == '\0', "real");
        CHECK(res->qmp_status != NULL && strstr(res->qmp_status, "\"running\"") != NULL, "real");
        CHECK(res->qmp_cpus != NULL && strstr(res->qmp_cpus, "\"cpu-index\": 0") != NULL, "real");
        CHECK(res->qmp_registers != NULL && strstr(res->qmp_registers, reg) != NULL, "real");
        CHECK(res->expired->advances == 0 && res->expired->bytes > 0, "real: output, no progress");
        CHECK(res->cleanup == ETUDE_CLEANUP_CONFIRMED, "real");
    }
    rows_free(s, &r, 1);
}

int main(int argc, char **argv)
{
    const char *role = getenv("ETUDE_WATCHDOG_ROLE"), *fake = getenv("ETUDE_FAKE_QEMU");

    if (role != NULL && *role != '\0')
        return case_main(role);
    if (fake != NULL && *fake != '\0')
        return fake_qemu(fake, argc, argv);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-watchdog-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    if (setenv("TMPDIR", fix_dir(), 1) != 0)
        return 1;
    test_bounds();
    test_watchdog();
    test_host_and_remote();
    test_machines();
    test_real();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_watchdog_hosttest: pass\n");
    return 0;
}
