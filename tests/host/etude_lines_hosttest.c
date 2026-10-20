/* Check that etude's readers keep a suite's failure apart from the runner's,
 * and tell useful work from admission waits, stalled cases and uncertain
 * claims.
 * host-deps: tools/etude/lines.c tools/etude/history.c
 * host-deps: tools/etude/history_json.c tools/etude/history_collect.c
 * host-deps: utils/sha256.c tools/etude/posix/status.c tools/etude/watchdog.c
 * host-deps: tools/etude/ledger.c tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 *
 * Each row writes a run's history through the history store and runs this
 * program as the compatible formatter on it, `--lines` its first argument:
 * a pass, a failure, a failure that a retry passed, a stalled case, an
 * attempt that something outside its case disrupted, a run cut short, a case
 * of no suite, a suite of no case, a cancelled run, and run records that
 * disagree with their attempts. The real tools/ci/run.sh then reads the
 * lines, through a tests/run that runs the formatter, and the real suites
 * function of tools/ci/bisect.sh judges them.
 *
 * The status writer then records a run's attempts from real watchdogs, and
 * the ledger gets real claims and requests: a claim of this process, one
 * whose owner ended with its cleanup unconfirmed, one whose owner ended while
 * its process runs, the protected request and one behind it. On Linux,
 * tools/flow/test_status.py, or the reader the environment's
 * ETUDE_LINES_READER names, must report each as it is; elsewhere it must say
 * that it needs the Linux testing host.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../utils/sha256.h"
#include "etude/history.h"
#include "etude/ledger.h"
#include "etude/status.h"
#include "etude/watchdog.h"
#include "lines.h"

#define SECOND UINT64_C(1000000)

/* The room for a path of the tree or of this program. */
#define PATH_ROOM 4096

static int failures;
static char directory[1024]; /* the scratch directory */
static char self[PATH_ROOM]; /* this program */
static char repo[PATH_ROOM]; /* the tree, where the test runs */
static pid_t holder;         /* a process a claim registers, or 0 */
static int holder_pipe = -1; /* the write end of its standard input */
static const char schema[] = "tools/etude/schemas/history.schema.json";

#define CHECK(test, detail)                                                \
    do {                                                                   \
        if (!(test)) {                                                     \
            fprintf(stderr, "etude_lines_hosttest: failed: %s\n", detail); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* One attempt of an item: its outcome, reason and disruption. */
struct try_spec {
    const char *outcome, *reason, *disruption;
};

/* One item of a run's plan, and its attempts. */
struct item_spec {
    const char *case_id, *family, *arch;
    int tries;
    struct try_spec t[2];
    const char *summary; /* the run record's outcome of it, if not its worst */
};

/* A run's history, as one stream named after the run. */
struct run_spec {
    const char *name;
    int nitems;
    struct item_spec items[2];
    int run_record;
    /* The run's outcome and reason, when they are not its items'. */
    const char *run_outcome, *run_reason;
    int cut; /* the last record is cut short */
};

/* A row: a run, the suite map, and what the formatter, ci and bisection
 * must make of it. */
struct row {
    const char *detail;
    struct run_spec run;
    const char *map;
    int status;
    const char *wants;  /* lines: `1 TEXT` on stdout, `2 TEXT` on stderr,
                           `-TEXT` on neither, `_TEXT` not on stdout */
    const char *failed; /* what ci's run.sh records */
    int bisect;         /* 0 passes, 1 fails, 2 untestable */
};

/** Remove the scratch directory and end the holder, even after a failure. */
static void cleanup(void)
{
    char command[1200];
    if (holder_pipe >= 0)
        close(holder_pipe);
    if (holder > 0) {
        kill(holder, SIGKILL);
        waitpid(holder, NULL, 0);
    }
    if (*directory) {
        snprintf(command, sizeof(command), "rm -rf '%s'", directory);
        if (system(command))
            fprintf(stderr, "etude_lines_hosttest: cannot remove %s\n", directory);
    }
}

/** Exit through the cleanup after a signal. */
static void stopped(int number)
{
    exit(128 + number);
}

/** Give a path under the scratch directory. */
static const char *path(const char *name)
{
    static char paths[32][1200];
    static unsigned next;
    char *out = paths[next++ % 32];
    snprintf(out, sizeof(paths[0]), "%s/%s", directory, name);
    return out;
}

/** Write `text` to the scratch file `name`, with `mode`; exit on failure. */
static void put(const char *name, const char *text, mode_t mode)
{
    FILE *f = fopen(path(name), "w");
    if (!f || fputs(text, f) < 0 || fclose(f) || chmod(path(name), mode)) {
        fprintf(stderr, "etude_lines_hosttest: cannot write %s\n", path(name));
        exit(1);
    }
}

/** Read the scratch file `name` into `out`; "" when it cannot be read. */
static void slurp(const char *name, char *out, size_t size)
{
    FILE *f = fopen(path(name), "r");
    size_t n = f ? fread(out, 1, size - 1, f) : 0;
    out[n] = 0;
    if (f)
        fclose(f);
}

/** Write a blob reference whose digest is that of the bytes given. */
static void blob(char *out, size_t size, const char *bytes)
{
    struct sha256 sha;
    unsigned char digest[32];
    char hex[65];
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, strlen(bytes));
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    snprintf(out, size, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"test://blob\"]}",
             hex, (unsigned long)strlen(bytes));
}

/** Write a UTC time some microseconds after the start of the test's day. */
static void utc_at(uint64_t us, char out[40])
{
    uint64_t s = us / SECOND;
    snprintf(out, 40, "2026-10-20T%02u:%02u:%02u.%06uZ", (unsigned)(s / 3600 % 24),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60), (unsigned)(us % SECOND));
}

/** Write an interval of the test's clock, from start to end microseconds. */
static void interval(char *out, size_t size, uint64_t start, uint64_t end)
{
    char a[40], b[40];
    utc_at(start, a);
    utc_at(end, b);
    snprintf(out, size,
             "{\"start_utc\":\"%s\",\"end_utc\":\"%s\",\"clock\":{\"machine_id\":\"m1\","
             "\"boot_id\":\"b1\",\"clock_id\":\"monotonic\",\"resolution_us\":1},"
             "\"start_us\":%llu,\"end_us\":%llu,\"elapsed_us\":{\"value\":%llu,"
             "\"method\":\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null,"
             "\"error_bound\":1}}",
             a, b, (unsigned long long)start, (unsigned long long)end,
             (unsigned long long)(end - start));
}

/** Write the resources of an interval. */
static void resources(char *out, size_t size)
{
    char scope[200];
    blob(scope, sizeof(scope), "scope");
    snprintf(out, size,
             "{\"scope\":%s,\"tree_cpu_us\":{\"value\":1000,\"method\":\"rusage-v1\","
             "\"quality\":\"exact\",\"interval_us\":null,\"error_bound\":1},"
             "\"combined_peak_bytes\":{\"value\":null,\"reason\":\"not sampled in the test\"},"
             "\"samples\":[]}",
             scope);
}

/** Write a result of an outcome, with its reason, disruption and cleanup. */
static void result(char *out, size_t size, const char *outcome, const char *reason,
                   const char *disruption, int confirmed)
{
    snprintf(out, size,
             "{\"outcome\":\"%s\",\"reason\":%s%s%s,\"cleanup\":\"%s\",\"evidence\":[],"
             "\"capture_errors\":[],\"disruptions\":[%s%s%s]}",
             outcome, reason ? "\"" : "null", reason ? reason : "", reason ? "\"" : "",
             confirmed ? "confirmed" : "unconfirmed", disruption ? "\"" : "",
             disruption ? disruption : "", disruption ? "\"" : "");
}

/** Append one record, checking that the history store takes it. */
static void append(struct etude_history *h, const char *kind, const char *data, char *id)
{
    char utc[40];
    utc_at(0, utc);
    if (etude_history_record_id(h, id, 300) || etude_history_append(h, id, utc, kind, data)) {
        fprintf(stderr, "etude_lines_hosttest: the store refused a %s record: %s\n", kind, data);
        exit(1);
    }
}

/** Write the plan of a run, each item with two attempt identities. */
static void write_plan(struct etude_history *h, const struct run_spec *r, char *plan_id)
{
    static char data[32768];
    char items[8192], b[200], env[2400];
    size_t used = 0;
    int i;
    blob(b, sizeof(b), "fixed");
    for (i = 0; i < r->nitems; i++)
        used += (size_t)snprintf(
            items + used, sizeof(items) - used,
            "%s{\"item_id\":\"%s-i%d\",\"case_id\":\"%s\",\"family_id\":\"%s\","
            "\"architecture\":\"%s\",\"domain\":\"qemu\",\"workload\":%s,\"assertions\":%s,"
            "\"profile\":%s,\"repetition\":0,\"attempt_ids\":[\"%s-i%d-a0\",\"%s-i%d-a1\"]}",
            i ? "," : "", r->name, i, r->items[i].case_id, r->items[i].family, r->items[i].arch, b,
            b, b, r->name, i, r->name, i);
    snprintf(env, sizeof(env),
             "{\"machine_id\":\"m1\",\"boot_id\":\"b1\",\"os\":\"linux\",\"os_version\":\"26.04\","
             "\"accelerator\":\"kvm\",\"site_profile\":%s,\"tools\":%s,\"capacity\":%s,"
             "\"cache_state\":%s,\"load\":%s}",
             b, b, b, b, b);
    snprintf(data, sizeof(data),
             "{\"purpose\":\"ci\",\"selection\":%s,\"items\":[%s],\"commit\":"
             "\"0123456789abcdef0123456789abcdef01234567\",\"source\":%s,\"runner\":%s,"
             "\"artifacts\":[%s],\"environment\":%s,\"concurrency\":%s,\"policies\":%s,"
             "\"exemption\":null,\"reproduce\":%s}",
             b, items, b, b, b, env, b, b, b);
    append(h, "plan", data, plan_id);
}

/** Give when attempt `j` of item `i` starts: three seconds after the one
 * before it. */
static uint64_t start_of(const struct run_spec *r, int i, int j)
{
    uint64_t n = (uint64_t)j;
    int k;
    for (k = 0; k < i; k++)
        n += (uint64_t)r->items[k].tries;
    return 3 * SECOND * n;
}

/** Write attempt `j` of item `i`, which takes two seconds. */
static void write_attempt(struct etude_history *h, const struct run_spec *r, const char *plan_id,
                          int i, int j, char *id)
{
    static char data[16384];
    char time[1200], res[600], ok[600], b[200], previous[300];
    const struct try_spec *t = &r->items[i].t[j];
    uint64_t start = start_of(r, i, j);
    interval(time, sizeof(time), start, start + 2 * SECOND);
    resources(res, sizeof(res));
    result(ok, sizeof(ok), t->outcome, t->reason, t->disruption,
           strcmp(t->outcome, "incomplete") != 0);
    blob(b, sizeof(b), "results");
    snprintf(previous, sizeof(previous), j ? "\"%s-i%d-a0\"" : "null", r->name, i);
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"item_id\":\"%s-i%d\",\"attempt_id\":\"%s-i%d-a%d\","
             "\"previous_attempt_id\":%s,\"steps\":[],\"time\":%s,"
             "\"queue_us\":{\"value\":0,\"method\":\"monotonic-v1\",\"quality\":\"exact\","
             "\"interval_us\":null,\"error_bound\":1},\"resources\":%s,"
             "\"assertion_results\":%s,\"result\":%s}",
             plan_id, r->name, i, r->name, i, j, previous, time, res, b, ok);
    append(h, "attempt", data, id);
}

/** Give the weight of an outcome: pass, cancelled, incomplete, failure. */
static int weight(const char *outcome)
{
    if (!strcmp(outcome, "failure"))
        return 3;
    if (!strcmp(outcome, "incomplete"))
        return 2;
    return !strcmp(outcome, "cancelled");
}

/** Give the heaviest outcome of an item's attempts, as a run record does. */
static const char *worst(const struct item_spec *it)
{
    static const char *const order[] = {"failure", "incomplete", "cancelled", "pass"};
    int k, j;
    if (it->summary)
        return it->summary;
    for (k = 0; k < 4; k++)
        for (j = 0; j < it->tries; j++)
            if (!strcmp(it->t[j].outcome, order[k]))
                return order[k];
    return "incomplete";
}

/** Write the run record, listing each item's attempt records. */
static void write_run(struct etude_history *h, const struct run_spec *r, const char *plan_id,
                      char records[2][2][300], uint64_t end)
{
    static char data[32768];
    char items[8192], time[1200], res[600], ok[600], b[200], m[200], id[300];
    const char *outcome = "pass";
    size_t used = 0;
    int i;
    for (i = 0; i < r->nitems; i++) {
        const struct item_spec *it = &r->items[i];
        if (weight(worst(it)) > weight(outcome))
            outcome = worst(it);
        used +=
            (size_t)snprintf(items + used, sizeof(items) - used,
                             "%s{\"item_id\":\"%s-i%d\",\"attempt_records\":[%s%s%s%s%s],"
                             "\"outcome\":\"%s\"}",
                             i ? "," : "", r->name, i, it->tries ? "\"" : "",
                             it->tries ? records[i][0] : "", it->tries > 1 ? "\",\"" : "",
                             it->tries > 1 ? records[i][1] : "", it->tries ? "\"" : "", worst(it));
    }
    if (r->run_outcome)
        outcome = r->run_outcome;
    interval(time, sizeof(time), 0, end);
    resources(res, sizeof(res));
    result(ok, sizeof(ok), outcome,
           r->run_reason             ? r->run_reason
           : strcmp(outcome, "pass") ? "an item did not pass"
                                     : NULL,
           NULL, 1);
    blob(b, sizeof(b), "fixed");
    snprintf(m, sizeof(m),
             "{\"value\":%llu,\"method\":\"monotonic-v1\",\"quality\":\"exact\","
             "\"interval_us\":null,\"error_bound\":1}",
             (unsigned long long)end);
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"items\":[%s],\"preparation_steps\":[],\"time\":%s,"
             "\"resources\":%s,\"stage_totals\":%s,\"family_totals\":%s,"
             "\"useful_concurrency\":%s,\"load\":%s,\"budget\":{\"policy\":%s,\"charged_us\":%s,"
             "\"outside_queue_us\":%s,\"one_time_setup_us\":%s,\"goal_us\":3600000000,"
             "\"growth_us\":0,\"ceiling_us\":3600000000,\"cleanup_reserve_us\":1000000,"
             "\"verdict\":\"unknown\"},\"result\":%s}",
             plan_id, items, time, res, b, b, b, b, b, m, m, m, ok);
    append(h, "run", data, id);
}

/** Write a run's stream, `NAME.jsonl` in the scratch directory. */
static void write_stream(const struct run_spec *r)
{
    struct etude_history *h;
    char plan_id[300], records[2][2][300], name[300], stream[300];
    uint64_t end = start_of(r, r->nitems, 0);
    struct stat st;
    int i, j;
    snprintf(name, sizeof(name), "%s.jsonl", r->name);
    snprintf(stream, sizeof(stream), "%s-s", r->name);
    h = etude_history_open(path(name), schema, stream, r->name, UINT64_C(1) << 24);
    if (!h) {
        fprintf(stderr, "etude_lines_hosttest: cannot open the stream of %s\n", r->name);
        exit(1);
    }
    write_plan(h, r, plan_id);
    for (i = 0; i < r->nitems; i++)
        for (j = 0; j < r->items[i].tries; j++)
            write_attempt(h, r, plan_id, i, j, records[i][j]);
    if (r->run_record)
        write_run(h, r, plan_id, records, end ? end : SECOND);
    if (etude_history_close(h))
        exit(1);
    if (r->cut && (stat(path(name), &st) || truncate(path(name), st.st_size - 10)))
        exit(1);
}

/** Run this program as the formatter; give its status and its two outputs. */
static int format(const char *map, const char *stream, char *out, char *err, size_t size)
{
    char schemas[PATH_ROOM + 32];
    char *argv[7];
    int status, o, e;
    pid_t pid;
    snprintf(schemas, sizeof(schemas), "%s/tools/etude/schemas", repo);
    argv[0] = self, argv[1] = "--lines", argv[2] = "-s", argv[3] = schemas;
    argv[4] = (char *)map, argv[5] = (char *)stream, argv[6] = NULL;
    fflush(stdout);
    fflush(stderr);
    if ((pid = fork()) == 0) {
        o = open(path("out"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        e = open(path("err"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (o < 0 || e < 0 || dup2(o, 1) < 0 || dup2(e, 2) < 0)
            _exit(126);
        execv(self, argv);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    slurp("out", out, size);
    slurp("err", err, size);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** Check the lines a row wants; give 0 when each is where it must be. */
static int wanted(const struct row *w, const char *out, const char *err)
{
    char copy[4096], *line, *save = NULL;
    int bad = 0, ok;
    snprintf(copy, sizeof(copy), "%s", w->wants);
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '-')
            ok = !strstr(out, line + 1) && !strstr(err, line + 1);
        else if (line[0] == '_')
            ok = !strstr(out, line + 1);
        else
            ok = strstr(line[0] == '1' ? out : err, line + 2) != NULL;
        if (!ok) {
            fprintf(stderr, "etude_lines_hosttest: %s: wants: %s\n", w->run.name, line);
            bad = 1;
        }
    }
    return bad;
}

/** Run the shell command that `format` makes; exit when it is too long. */
static int shell(const char *format, ...)
{
    static char command[4 * PATH_ROOM + 4096];
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf(command, sizeof(command), format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(command)) {
        fprintf(stderr, "etude_lines_hosttest: a command is too long\n");
        exit(1);
    }
    return system(command);
}

/** Make the tree ci's run.sh runs in: the registry, a tests/run that runs
 * the formatter for `core` and passes the other sets, and stand-ins for make
 * and git; and the script that runs bisection's reading of tests/run. */
static void make_tree(void)
{
    if (shell("mkdir -p '%s/tree/tests' '%s/tree/bin' && cp '%s/tests/registry' '%s/tree/tests/'",
              directory, directory, repo, directory))
        exit(1);
    put("tree/tests/run",
        "#!/bin/sh\n"
        "case $1 in\n"
        "    core) exec \"$ETUDE_LINES_SELF\" --lines -s \"$ETUDE_LINES_SCHEMAS\" "
        "\"$ETUDE_LINES_MAP\" \"$ETUDE_LINES_STREAM\" ;;\n"
        "esac\n"
        "echo \"run: $1: PASS in 1s\"\n"
        "echo 'run: result: all 1 suites passed'\n",
        0755);
    put("tree/bin/make", "#!/bin/sh\nexit 0\n", 0755);
    put("tree/bin/git", "#!/bin/sh\necho 0000000\n", 0755);
    put("tree/bisect.sh",
        "#!/bin/sh\n"
        "limited() { \"$@\"; }\n"
        "log=$1\n"
        "eval \"$(awk '/^suites\\(\\) /,/^}/' \"$ETUDE_LINES_REPO/tools/ci/bisect.sh\")\"\n"
        "suites core\n",
        0755);
}

/** Run ci's run.sh and bisection's reading on a row's lines. */
static void readers(const struct row *w, const char *map, const char *stream)
{
    char failed[1024], name[300];
    int status;
    status =
        shell("cd '%s/tree' && ETUDE_LINES_SELF='%s' ETUDE_LINES_SCHEMAS='%s/tools/etude/schemas' "
              "ETUDE_LINES_MAP='%s' ETUDE_LINES_STREAM='%s' ETUDE_LINES_REPO='%s' "
              "PATH=\"$PWD/bin:$PATH\" sh '%s/tools/ci/run.sh' core '%s/ci-%s' >/dev/null 2>&1",
              directory, self, repo, map, stream, repo, repo, directory, w->run.name);
    snprintf(name, sizeof(name), "ci-%s/failed", w->run.name);
    slurp(name, failed, sizeof(failed));
    if (strcmp(failed, w->failed) || WEXITSTATUS(status) != (*w->failed ? 1 : 0)) {
        fprintf(stderr, "etude_lines_hosttest: %s: ci recorded \"%s\", status %d\n", w->run.name,
                failed, WEXITSTATUS(status));
        CHECK(0, w->detail);
    }
    status =
        shell("cd '%s/tree' && ETUDE_LINES_SELF='%s' ETUDE_LINES_SCHEMAS='%s/tools/etude/schemas' "
              "ETUDE_LINES_MAP='%s' ETUDE_LINES_STREAM='%s' ETUDE_LINES_REPO='%s' "
              "sh bisect.sh '%s/bisect-%s'",
              directory, self, repo, map, stream, repo, directory, w->run.name);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != w->bisect) {
        fprintf(stderr, "etude_lines_hosttest: %s: bisection judged %d\n", w->run.name,
                WEXITSTATUS(status));
        CHECK(0, w->detail);
    }
}

/** Write a row's history and map, and check what each reader makes of it. */
static void check_row(const struct row *w)
{
    static char out[65536], err[65536];
    char map[1400], stream[1400];
    int status;
    snprintf(map, sizeof(map), "%s.map", w->run.name);
    put(map, w->map, 0644);
    snprintf(map, sizeof(map), "%s", path(map));
    write_stream(&w->run);
    snprintf(stream, sizeof(stream), "%s/%s.jsonl", directory, w->run.name);
    status = format(map, stream, out, err, sizeof(out));
    if (status != w->status || wanted(w, out, err)) {
        fprintf(stderr, "etude_lines_hosttest: %s: status %d\n--- stdout\n%s--- stderr\n%s",
                w->run.name, status, out, err);
        CHECK(0, w->detail);
    }
    if (w->failed)
        readers(w, map, stream);
}

#define PASS(c)        {c, 1, {{"pass", NULL, NULL}}, NULL}
#define FAILED(c, why) {c, 1, {{"failure", why, NULL}}, NULL}
#define TWO            "fpu guest.fpu\ndefault guest.default\n"
#define XSAVE          "guest.fpu.xsave", "guest.fpu", "x86_64"
#define BOOT           "guest.default.boot", "guest.default", "i686"

/* The rows of the formatter, ci and bisection. */
static const struct row rows[] = {
    {"a run whose cases pass passes each suite",
     {"pass", 2, {PASS(XSAVE), PASS(BOOT)}, 1, NULL, NULL, 0},
     TWO,
     0,
     "1 run: fpu: PASS in 2s\n1 run: default: PASS in 2s\n"
     "1 run: result: all 2 suites passed\n-runner\n-FAIL",
     "",
     0},
    {"a failed case fails its suite alone",
     {"failure", 2, {FAILED(XSAVE, "it exited 1"), PASS(BOOT)}, 1, NULL, NULL, 0},
     TWO,
     1,
     "2 run: fpu: guest.fpu.xsave on x86_64: failure: it exited 1\n2 run: fpu: FAIL in 2s\n"
     "1 run: default: PASS in 2s\n2 run: result: failed: fpu\n1 run: result: failed\n-runner",
     "fpu\n",
     1},
    {"a retry that passes keeps the failure",
     {"retry",
      1,
      {{XSAVE, 2, {{"failure", "it exited 1", NULL}, {"pass", NULL, NULL}}, NULL}},
      1,
      NULL,
      NULL,
      0},
     "fpu guest.fpu.xsave\n",
     1,
     "2 run: fpu: guest.fpu.xsave on x86_64: failure: it exited 1\n2 run: fpu: FAIL in 5s\n"
     "2 run: result: failed: fpu\n1 run: result: failed\n-runner",
     "fpu\n",
     1},
    {"a stalled case is its suite's, and leaves bisection unresolved",
     {"stalled",
      1,
      {{XSAVE, 1, {{"incomplete", "its gap bound of 60000000 us ran out", NULL}}, NULL}},
      1,
      NULL,
      NULL,
      0},
     "fpu guest.fpu\n",
     1,
     "2 run: fpu: guest.fpu.xsave on x86_64: incomplete: its gap bound of 60000000 us ran out\n"
     "2 run: fpu: INCOMPLETE in 2s\n2 run: result: failed: fpu\n1 run: result: incomplete\n"
     "-runner\n_run: result: failed\n",
     "fpu\n",
     2},
    {"a disrupted attempt is the runner's, not its suite's",
     {"disrupted",
      2,
      {{XSAVE,
        1,
        {{"incomplete", "the executor connection was lost and cleanup is unconfirmed",
          "executor connection lost"}},
        NULL},
       PASS(BOOT)},
      1,
      NULL,
      NULL,
      0},
     TWO,
     1,
     "2 run: fpu: INCOMPLETE in 2s\n"
     "2 run: runner: guest.fpu.xsave on x86_64: the attempt disrupted-i0-a0 was disrupted: "
     "executor connection lost\n2 run: runner: FAIL in 6s\n2 run: result: failed: runner\n"
     "1 run: result: incomplete\n-failed: fpu",
     "runner\n",
     2},
    {"a run without its run record is the runner's fault",
     {"unfinished", 2, {PASS(XSAVE), {BOOT, 0, {{NULL, NULL, NULL}}, NULL}}, 0, NULL, NULL, 0},
     TWO,
     1,
     "1 run: fpu: PASS in 2s\n2 run: default: guest.default.boot on i686: incomplete: no attempt "
     "of it ran\n2 run: default: INCOMPLETE in ?s\n"
     "2 run: runner: the history has no run record: the run did not finish\n"
     "2 run: runner: guest.default.boot on i686: no attempt of it ran\n"
     "2 run: runner: FAIL in ?s\n2 run: result: failed: runner\n1 run: result: incomplete\n"
     "-failed: default",
     "runner\n",
     2},
    {"a stream cut short is the runner's fault",
     {"cut", 2, {PASS(XSAVE), PASS(BOOT)}, 1, NULL, NULL, 1},
     TWO,
     1,
     "2 cut.jsonl: its last record was cut short\n"
     "2 run: runner: the history has no run record: the run did not finish\n"
     "2 run: result: failed: runner\n1 run: result: incomplete",
     "runner\n",
     2},
    {"a case in no suite of the map is the runner's fault",
     {"orphan", 2, {PASS(XSAVE), FAILED(BOOT, "it exited 3")}, 1, NULL, NULL, 0},
     "fpu guest.fpu\n",
     1,
     "1 run: fpu: PASS in 2s\n2 run: runner: guest.default.boot on i686 is in no suite of the map\n"
     "2 run: result: failed: runner\n1 run: result: failed",
     "runner\n",
     1},
    {"a suite with no case in the plan did not finish",
     {"empty", 2, {PASS(XSAVE), PASS(BOOT)}, 1, NULL, NULL, 0},
     TWO "usb guest.usb\n",
     1,
     "1 run: fpu: PASS in 2s\n2 run: usb: INCOMPLETE in ?s\n"
     "2 run: runner: the plan has no case of the suite usb\n2 run: result: failed: runner\n"
     "1 run: result: incomplete",
     "runner\n",
     2},
    {"a cancelled run is interrupted, and nobody's failure",
     {"cancelled",
      2,
      {{XSAVE, 0, {{NULL, NULL, NULL}}, "cancelled"}, {BOOT, 0, {{NULL, NULL, NULL}}, "cancelled"}},
      1,
      "cancelled",
      "the run was cancelled",
      0},
     TWO,
     1,
     "2 run: fpu: guest.fpu.xsave on x86_64: cancelled: no attempt of it ran\n"
     "2 run: fpu: CANCELLED in ?s\n1 run: result: interrupted\n-run: result: failed\n-runner",
     "core\n",
     2},
    {"a run record that disagrees with its attempts is the runner's fault",
     {"lie", 1, {{XSAVE, 1, {{"failure", "it exited 1", NULL}}, "pass"}}, 1, "pass", NULL, 0},
     "fpu guest.fpu\n",
     1,
     "2 run: runner: the run record gives guest.fpu.xsave on x86_64 the outcome pass; its "
     "attempts give failure\n"
     "2 run: runner: the run record gives the run a pass, though an item did not pass\n"
     "2 run: fpu: FAIL in 2s\n2 run: result: failed: fpu runner\n1 run: result: failed",
     "fpu\nrunner\n",
     1},
    {"a run that failed after its cases passed is the runner's fault",
     {"prepared", 1, {PASS(XSAVE)}, 1, "failure", "a preparation step failed", 0},
     "fpu guest.fpu\n",
     1,
     "1 run: fpu: PASS in 2s\n"
     "2 run: runner: the run did not pass, though each item passed: a preparation step failed\n"
     "2 run: result: failed: runner\n1 run: result: incomplete",
     "runner\n",
     2},
};

/** Check the maps the formatter refuses before it reads a history. */
static void check_maps(void)
{
    static const char *const maps[] = {"runner guest.fpu\n",
                                       "fpu guest.fpu\nusb guest.fpu\n",
                                       "Fpu guest.fpu\n",
                                       "fpu\n",
                                       "fpu guest.fpu\nfpu guest.usb\n",
                                       "# no suite\n"};
    static char out[4096], err[4096];
    size_t i;
    for (i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        put("bad.map", maps[i], 0644);
        if (format(path("bad.map"), path("pass.jsonl"), out, err, sizeof(out)) != 2 ||
            !strstr(err, "history_lines: ")) {
            fprintf(stderr, "etude_lines_hosttest: map \"%s\": %s%s", maps[i], out, err);
            CHECK(0, "a map that names runner, an identity twice or no case is refused");
        }
    }
}

/** Give the time of the monotonic clock in microseconds. */
static uint64_t monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * SECOND + (uint64_t)t.tv_nsec / 1000;
}

/** Start a process that runs until its standard input closes; give it. */
static pid_t start_holder(int *write_end)
{
    int p[2];
    pid_t pid;
    char c;
    if (pipe(p))
        exit(1);
    if ((pid = fork()) == 0) {
        close(p[1]);
        _exit(read(p[0], &c, 1) < 0);
    }
    close(p[0]);
    *write_end = p[1];
    return pid;
}

/**
 * Be a worker that claims `res` as `id`, registers `process` in the claim,
 * and exits: with the cleanup unconfirmed when `unconfirmed`, and otherwise
 * leaving the claim running. Give the worker's exit status.
 */
static int worker(const char *ledger, const char *id, uint64_t cpu, const struct etude_pid *process,
                  int unconfirmed)
{
    struct etude_ledger_res res = {"cpu_millis", 0};
    struct etude_ledger *l;
    struct etude_pid me;
    int status;
    pid_t pid = fork();
    res.amount = cpu;
    if (pid == 0) {
        l = etude_ledger_open(ledger);
        if (!l || etude_process_state((long)getpid(), &me) != 1 ||
            etude_ledger_reserve(l, id, &res, 1, NULL, 0) ||
            etude_ledger_register(l, id, process ? process : &me, process ? process : &me) ||
            (unconfirmed && etude_ledger_finish(l, id, ETUDE_CLEANUP_UNCONFIRMED)))
            _exit(1);
        etude_ledger_close(l);
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/**
 * Fill the ledger: a claim of this process, one left uncertain, one left
 * running with a live process, the protected request and one behind it.
 * Give this process's handle, which keeps the requests.
 */
static struct etude_ledger *fill_ledger(const char *ledger)
{
    struct etude_ledger_res cap = {"cpu_millis", 2000}, res = {"cpu_millis", 1000};
    struct etude_ledger *l = etude_ledger_open(ledger);
    struct etude_pid held;
    CHECK(l && etude_ledger_admit(l, "C1", "tester", &res, 1, &cap, 1, 0) == 0,
          "this process's claim is admitted");
    CHECK(worker(ledger, "A1", 500, NULL, 1) == 0, "a worker leaves a claim uncertain");
    holder = start_holder(&holder_pipe);
    CHECK(etude_process_state((long)holder, &held) == 1, "the holder's identity is read");
    CHECK(worker(ledger, "E1", 100, &held, 0) == 0, "a worker leaves a claim running");
    errno = 0;
    CHECK(l && etude_ledger_admit(l, "B1", "tester", &res, 1, &cap, 1, 1) == -1 && errno == EAGAIN,
          "a request that does not fit waits");
    res.amount = 200;
    errno = 0;
    CHECK(l && etude_ledger_admit(l, "D1", "tester", &res, 1, &cap, 1, 1) == -1 && errno == EAGAIN,
          "a request that would take the protected request's room waits");
    return l;
}

/** Write a run's record from real watchdogs, and one whose owner ended. */
static void write_records(const char *ledger, const char *build)
{
    static struct etude_watchdog_spec roomy, tight, booting;
    struct etude_status_attempt a[6];
    struct etude_status s;
    struct etude_watchdog *w[4];
    uint64_t now = monotonic(), epoch = etude_status_now();
    char record[1400];
    int i, end, status;
    pid_t gone;
    roomy.case_us = tight.case_us = booting.case_us = 3600 * SECOND;
    roomy.gap_us = booting.gap_us = 60 * SECOND;
    tight.gap_us = SECOND;
    booting.stage_us[ETUDE_STAGE_BOOT] = 120 * SECOND;
    w[0] = etude_watchdog_new(&roomy, ETUDE_STAGE_EXECUTE, now - 5 * SECOND);
    w[1] = etude_watchdog_new(&tight, ETUDE_STAGE_EXECUTE, now - 10 * SECOND);
    w[2] = etude_watchdog_new(&tight, ETUDE_STAGE_EXECUTE, now - 10 * SECOND);
    w[3] = etude_watchdog_new(&booting, ETUDE_STAGE_BOOT, now - 3 * SECOND);
    if (!w[0] || !w[1] || !w[2] || !w[3])
        exit(1);
    CHECK(etude_watchdog_check(w[1], now) == ETUDE_EXPIRY_GAP, "the tight watchdog runs out");
    memset(a, 0, sizeof(a));
    a[0].attempt_id = "run1:xsave:a0", a[0].case_id = "guest.fpu.xsave",
    a[0].architecture = "x86_64";
    a[1].attempt_id = "run1:hang:a0", a[1].case_id = "guest.default.hang",
    a[1].architecture = "i686";
    a[2].attempt_id = "run1:late:a0", a[2].case_id = "guest.default.late",
    a[2].architecture = "i686";
    a[3].attempt_id = "run1:grub:a0", a[3].case_id = "guest.boot.grub",
    a[3].architecture = "x86_64";
    for (i = 0; i < 4; i++)
        etude_status_watch(&a[i], w[i], now, epoch);
    a[4].attempt_id = "B1", a[4].case_id = "guest.memory.big", a[4].architecture = "x86_64";
    a[5].attempt_id = "D1", a[5].case_id = "guest.disk.small", a[5].architecture = "i686";
    a[4].waiting = a[5].waiting = 1;
    a[4].since_us = epoch - 7 * SECOND, a[5].since_us = epoch - 4 * SECOND;
    memset(&s, 0, sizeof(s));
    CHECK(etude_process_state((long)getpid(), &s.owner) == 1, "this process's identity is read");
    s.run_id = "run1", s.ledger = ledger, s.args = "core", s.written_us = epoch;
    s.attempts = a, s.nattempts = 6;
    snprintf(record, sizeof(record), "%s/.run.status.1", build);
    CHECK(etude_status_write(record, &s) == 0, "the status record is written");
    s.args = "with a\nnewline";
    CHECK(etude_status_write(record, &s) == -1 && errno == EINVAL,
          "a field of two lines is refused");
    gone = start_holder(&end);
    CHECK(etude_process_state((long)gone, &s.owner) == 1, "the ended owner's identity is read");
    close(end);
    waitpid(gone, &status, 0);
    s.run_id = "run0", s.args = "stale", s.attempts = NULL, s.nattempts = 0;
    snprintf(record, sizeof(record), "%s/.run.status.2", build);
    CHECK(etude_status_write(record, &s) == 0, "the stale record is written");
    for (i = 0; i < 4; i++)
        etude_watchdog_free(w[i]);
}

/** Run the status reader on the tree `root`; give its output. */
static void read_status(const char *root, char *out, size_t size)
{
    const char *reader = getenv("ETUDE_LINES_READER");
    char command[2 * PATH_ROOM + 100];
    FILE *pipe;
    size_t n;
    snprintf(command, sizeof(command), "python3 -B '%s' '%s' 2>&1",
             reader && *reader ? reader : "tools/flow/test_status.py", root);
    pipe = popen(command, "r");
    n = pipe ? fread(out, 1, size - 1, pipe) : 0;
    out[n] = 0;
    if (pipe)
        pclose(pipe);
}

/** Check that the reader's output has `text`. */
static void has(const char *out, const char *text, const char *detail)
{
    if (!strstr(out, text)) {
        fprintf(stderr, "etude_lines_hosttest: the reader lacks \"%s\":\n%s", text, out);
        CHECK(0, detail);
    }
}

/** Check what the status reader says of the run and the ledger. */
static void check_reader(const char *root, const char *ledger, const char *owner)
{
    static char out[65536];
    char text[1400];
    read_status(root, out, sizeof(out));
    if (access("/proc/locks", F_OK)) {
        has(out, "test status unavailable: Linux testing host required", "the reader needs Linux");
        return;
    }
    snprintf(text, sizeof(text), "etude run %s alive: core (run run1, status written ", owner);
    has(out, text, "the run's owner is alive");
    has(out, "working: guest.fpu.xsave on x86_64 in execute: last progress ",
        "an attempt within its bounds is working");
    has(out, "its gap bound of 60s runs out in ", "an attempt within its bounds is working");
    has(out, "working: guest.boot.grub on x86_64 in boot: in boot for ",
        "an attempt in another stage works within that stage's bound");
    has(out, "its stage bound of 120s runs out in ",
        "an attempt in another stage works within that stage's bound");
    has(out, "its gap bound of 1s ran out, and attempt run1:hang:a0 is being ended",
        "an attempt whose watchdog ran out is stalled and being ended");
    has(out, "stalled: guest.default.late on i686 in execute: ",
        "an attempt past its deadline is stalled");
    has(out, "fault: the run has not ended attempt run1:late:a0",
        "an attempt past its deadline is stalled");
    has(out, "waits for admission: guest.memory.big on x86_64 for ", "a wait is reported");
    has(out, "it is the protected request, for cpu_millis 1000", "the protected request is named");
    has(out, "behind the protected request B1, for cpu_millis 200",
        "a request behind the protected one names it");
    has(out, "(run run0, status written ", "a stale run record is a fault");
    has(out, "fault: stale run record", "a stale run record is a fault");
    snprintf(text, sizeof(text), "ledger %s: 1 claims owned by running workers", ledger);
    has(out, text, "the ledger counts the claims of running workers");
    has(out, "request B1 of tester: protected, for cpu_millis 1000", "the ledger's requests");
    has(out, "request D1 of tester: waiting, for cpu_millis 200", "the ledger's requests");
    has(out, "claim A1: uncertain: its owner found the cleanup unconfirmed; holds cpu_millis 500",
        "a claim its owner found unconfirmed is uncertain");
    has(out, "claim E1: live: its owner has ended and a process of it runs; holds cpu_millis 100",
        "a claim whose owner ended while its process runs is live");
}

/** Check the status writer and reader against a real ledger. */
static void check_status(void)
{
    static char out[65536];
    char root[1200], build[1300], ledger[1200], machine[1400], owner[32];
    struct etude_ledger *l;
    snprintf(root, sizeof(root), "%s/root", directory);
    snprintf(build, sizeof(build), "%s/build", root);
    snprintf(ledger, sizeof(ledger), "%s/ledger", directory);
    snprintf(machine, sizeof(machine), "ledger %s\n", ledger);
    if (mkdir(root, 0700) || mkdir(build, 0700) || mkdir(path("state"), 0700) ||
        mkdir(path("state/etude"), 0700))
        exit(1);
    put("state/etude/machine", machine, 0644);
    setenv("XDG_STATE_HOME", path("state"), 1);
    l = fill_ledger(ledger);
    write_records(ledger, build);
    snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    check_reader(root, ledger, owner);
    unlink(path("root/build/.run.status.1"));
    unlink(path("root/build/.run.status.2"));
    read_status(root, out, sizeof(out));
    if (!access("/proc/locks", F_OK)) {
        has(out, "no recorded test run", "the ledger is read after the run's record is gone");
        has(out, "claim A1: uncertain: ", "the ledger is read after the run's record is gone");
    }
    etude_ledger_close(l);
}

int main(int argc, char **argv)
{
    const char *base = getenv("TMPDIR");
    size_t i;
    if (argc > 1 && !strcmp(argv[1], "--lines"))
        return etude_lines_main(argc - 1, argv + 1);
    if (!realpath(argv[0], self) || !getcwd(repo, sizeof(repo)))
        return 1;
    if (!base || !*base)
        base = "/tmp";
    atexit(cleanup);
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGHUP, stopped);
    if (snprintf(directory, sizeof(directory), "%s/rach2-etude-lines.XXXXXX", base) >=
            (int)sizeof(directory) ||
        !mkdtemp(directory))
        return 1;
    make_tree();
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        check_row(&rows[i]);
    check_maps();
    check_status();
    if (failures)
        fprintf(stderr, "etude_lines_hosttest: %d checks failed\n", failures);
    else
        printf("etude_lines_hosttest: result: pass\n");
    return failures != 0;
}
