/* Check that etude's readers keep a suite's failure apart from the runner's,
 * and tell useful work from admission waits, stalled cases and uncertain
 * claims.
 * host-inputs: tools/etude/schemas/history.schema.json
 * host-deps: tools/etude/lines.c tools/etude/history.c
 * host-deps: tools/etude/history_json.c tools/etude/history_collect.c
 * host-deps: utils/sha256.c tools/etude/posix/status.c tools/etude/watchdog.c
 * host-deps: tools/etude/ledger.c tools/etude/posix/process.c
 * host-deps: tests/cases/host/fixture.c tests/host/compat/status_proc_host.c
 * host-tools: python3
 * host-inputs: tools/ci/run.sh
 * host-inputs: tools/ci/bisect.sh
 * host-inputs: tools/flow/test_status.py
 * host-inputs: tests/registry
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 *
 * Each row writes a run's history through the history store and runs this
 * program as the compatible formatter on it, `--lines` its first argument:
 * a pass, a failure, a failure that a retry passed, a stalled case, an
 * attempt that something outside its case disrupted, a run cut short, a case
 * of no suite, a suite of no case, a cancelled run, and run records that
 * disagree with their attempts. The retained lines branch of tools/ci/run.sh
 * reads them through a tests/run that runs the formatter. Only its entry
 * selector changes in a private copy. The real suites
 * function of tools/ci/bisect.sh judges them, with a registry whose core
 * tier is the map's suites, as the tested commit's would be: a failure
 * beside a fault of the runner leaves bisection unresolved. Given both
 * architectures, `-a i686,x86_64`, the formatter makes each suite of the map
 * a suite `ARCH:SUITE` of each, leaves out one with no case beside one with
 * cases, and gives an item of no architecture to i686; given one, it prints
 * what it prints without `-a`.
 *
 * The status writer then records a run's attempts from real watchdogs, and
 * the ledger gets real claims and requests: a claim of this process, one
 * whose owner ended with its cleanup unconfirmed, one whose owner ended while
 * its process runs, the protected request and one behind it. The reader
 * tools/flow/test_status.py, or ETUDE_LINES_READER, reads proc-format copies
 * of the actual process identities and a kernel-observed lock holder. An
 * empty proc directory also checks the unavailable-host report.
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
#include "../cases/host/fixture.h"
#include "compat/status_proc_host.h"

#define SECOND UINT64_C(1000000)

/* The room for a path of the tree or of this program. */
#define PATH_ROOM 4096

static int failures;
static char directory[1024]; /* the scratch directory */
static char self[PATH_ROOM]; /* this program */
static char repo[PATH_ROOM]; /* the tree, where the test runs */
static pid_t holder;         /* a process a claim registers, or 0 */
static struct fixture commands;
static int commands_ready;
static pid_t observed[16];
static size_t nobserved;
static const char *proc_root;
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
    if (commands_ready)
        fx_destroy(&commands);
    if (holder_pipe >= 0)
        close(holder_pipe);
    if (holder > 0) {
        kill(holder, SIGKILL);
        waitpid(holder, NULL, 0);
    }
    if (*directory && !fx_remove(directory))
        fprintf(stderr, "etude_lines_hosttest: cannot remove %s\n", directory);
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

struct format_output {
    char *out, *err;
    size_t size;
};

/** Run this program as the formatter, with the failures `named` outside the
 * suites, at most two, given by `-f`, and the architectures `arches` by `-a`
 * unless it is NULL; give its status and its two outputs. */
static int format(const char *const *named, const char *arches, const char *map, const char *stream,
                  struct format_output output)
{
    char schemas[PATH_ROOM + 32];
    char *argv[13];
    int status, o, e, n = 4;
    pid_t pid;
    snprintf(schemas, sizeof(schemas), "%s/tools/etude/schemas", repo);
    argv[0] = self, argv[1] = "--lines", argv[2] = "-s", argv[3] = schemas;
    for (; named && *named && n < 8; named++) {
        argv[n++] = "-f";
        argv[n++] = (char *)*named;
    }
    if (arches) {
        argv[n++] = "-a";
        argv[n++] = (char *)arches;
    }
    argv[n++] = (char *)map, argv[n++] = (char *)stream, argv[n] = NULL;
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
    slurp("out", output.out, output.size);
    slurp("err", output.err, output.size);
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

/** Copy only the actual product functions that the old harness sourced. */
static int bisect_functions(void)
{
    static const char *const names[] = {"members() ", "admitted() ", "historic() ", "suites() "};
    FILE *source = fopen("tools/ci/bisect.sh", "r"), *out = fopen(path("bisect-functions"), "w");
    char *line = NULL;
    size_t capacity = 0, i;
    unsigned found = 0;
    int copying = 0, ok = source != NULL && out != NULL;
    while (ok && getline(&line, &capacity, source) >= 0) {
        if (!copying)
            for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!strncmp(line, names[i], strlen(names[i]))) {
                    copying = 1;
                    found |= 1u << i;
                }
        if (copying) {
            if (fputs(line, out) == EOF)
                ok = 0;
            if (line[0] == '}')
                copying = 0;
        }
    }
    free(line);
    if (source && (ferror(source) || fclose(source)))
        ok = 0;
    if (out && fclose(out))
        ok = 0;
    return ok && !copying && found == 15;
}

/** Create the fixture's files and compiled command roles without a shell. */
static void make_tree(void)
{
    char search[32768];
    const char *old = getenv("PATH");
    int n = snprintf(search, sizeof(search), "%s/tree/bin:%s", directory, old ? old : "");
    if (!fx_mkdir(path("tree/tests")) || !fx_mkdir(path("tree/bin")) ||
        !fx_copy("tests/registry", path("tree/tests/registry")) ||
        !fx_replace("tools/ci/run.sh", path("run.sh"), "results=native\n", "results=lines\n") ||
        !fx_copy(self, path("tree/tests/run")) || symlink(self, path("tree/bin/make")) ||
        symlink(self, path("tree/bin/git")) || !bisect_functions() || n < 0 ||
        n >= (int)sizeof(search) || setenv("PATH", search, 1))
        exit(1);
}

/** Pass a row's formatter inputs to the compiled tests/run fixture. */
static int reader_environment(const char *map, const char *stream)
{
    char schemas[PATH_ROOM + 32];
    int n = snprintf(schemas, sizeof(schemas), "%s/tools/etude/schemas", repo);
    return n < 0 || n >= (int)sizeof(schemas) || setenv("ETUDE_LINES_SELF", self, 1) ||
                   setenv("ETUDE_LINES_SCHEMAS", schemas, 1) || setenv("ETUDE_LINES_MAP", map, 1) ||
                   setenv("ETUDE_LINES_STREAM", stream, 1) || setenv("ETUDE_LINES_REPO", repo, 1)
               ? -1
               : 0;
}

/** Give bisection the row's registry and initially empty log from C. */
static int bisect_tree(const char *log, const char *map, char *work, size_t size)
{
    char destination[4096], name[256], *line = NULL;
    size_t capacity = 0;
    FILE *input = NULL, *registry = NULL;
    int n = snprintf(work, size, "%s.tree", log), ok;
    if (n < 0 || n >= (int)size)
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/tests", work);
    if (n < 0 || n >= (int)sizeof(destination) || !fx_mkdir(destination))
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/tests/run", work);
    if (n < 0 || n >= (int)sizeof(destination) || symlink(path("tree/tests/run"), destination))
        return 0;
    n = snprintf(destination, sizeof(destination), "%s/tests/registry", work);
    if (n < 0 || n >= (int)sizeof(destination))
        return 0;
    input = fopen(map, "r");
    registry = fopen(destination, "w");
    ok = input != NULL && registry != NULL;
    while (ok && getline(&line, &capacity, input) >= 0)
        if (line[0] != '#' && sscanf(line, "%255s", name) == 1 &&
            fprintf(registry, "%s|qemu|core\n", name) < 0)
            ok = 0;
    free(line);
    if (input && (ferror(input) || fclose(input)))
        ok = 0;
    if (registry && fclose(registry))
        ok = 0;
    n = snprintf(destination, sizeof(destination), "%s.log", log);
    return ok && n >= 0 && n < (int)sizeof(destination) && fx_text(destination, "");
}

/** Run the actual CI program and bisection's sourced product functions. */
static void readers(const struct row *w, const char *map, const char *stream)
{
    char failed[1024], name[300], program[PATH_ROOM + 32], log[4096], work[4096];
    const char *ci[] = {"/bin/sh", program, "core", log, NULL};
    const char *bisect[] = {
        "/bin/sh",
        "-c",
        ". \"$1\"; limited() { \"$@\"; }; ledger=; site_launcher=; log=$2; suites core",
        "etude-lines-bisect",
        NULL,
        log,
        NULL};
    int n, status;
    n = snprintf(program, sizeof(program), "%s/run.sh", directory);
    if (n < 0 || n >= (int)sizeof(program) || reader_environment(map, stream))
        exit(1);
    n = snprintf(log, sizeof(log), "%s/ci-%s", directory, w->run.name);
    if (n < 0 || n >= (int)sizeof(log))
        exit(1);
    status = fx_run(&commands, ci, path("tree"), NULL, NULL);
    snprintf(name, sizeof(name), "ci-%s/failed", w->run.name);
    slurp(name, failed, sizeof(failed));
    if (strcmp(failed, w->failed) || status != (*w->failed ? 1 : 0)) {
        fprintf(stderr, "etude_lines_hosttest: %s: ci recorded \"%s\", status %d\n", w->run.name,
                failed, status);
        CHECK(0, w->detail);
    }
    n = snprintf(log, sizeof(log), "%s/bisect-%s", directory, w->run.name);
    if (n < 0 || n >= (int)sizeof(log) || !bisect_tree(log, map, work, sizeof(work)))
        exit(1);
    bisect[4] = path("bisect-functions");
    status = fx_run(&commands, bisect, work, NULL, NULL);
    if (status != w->bisect) {
        fprintf(stderr, "etude_lines_hosttest: %s: bisection judged %d\n", w->run.name, status);
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
    status = format(NULL, NULL, map, stream, (struct format_output){out, err, sizeof(out)});
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
#define HOSTFPU        "guest.fpu.host", "guest.fpu", "none"

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
     2},
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
     2},
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
    static const char *const maps[] = {"fpu guest.fpu\nusb guest.fpu\n", "Fpu guest.fpu\n", "fpu\n",
                                       "fpu guest.fpu\nfpu guest.usb\n", "# no suite\n"};
    static char out[4096], err[4096];
    size_t i;
    for (i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        put("bad.map", maps[i], 0644);
        if (format(NULL, NULL, path("bad.map"), path("pass.jsonl"),
                   (struct format_output){out, err, sizeof(out)}) != 2 ||
            !strstr(err, "etude show: ")) {
            fprintf(stderr, "etude_lines_hosttest: map \"%s\": %s%s", maps[i], out, err);
            CHECK(0, "a map that names an identity twice or no case is refused");
        }
    }
}

/** Check that a failure the caller names outside the suites fails a run
 * whose suites passed, after the suites' names in the failed line. */
static void check_named(void)
{
    static const char *const named[] = {"voided", "images", NULL};
    static char out[4096], err[4096];
    int status;
    put("named.map", TWO, 0644);
    status = format(named, NULL, path("named.map"), path("pass.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    if (status != 1 || !strstr(out, "run: fpu: PASS in 2s") ||
        !strstr(err, "run: result: failed: voided images\n") ||
        !strstr(out, "run: result: failed\n") || strstr(out, "suites passed")) {
        fprintf(stderr, "etude_lines_hosttest: named: status %d\n--- stdout\n%s--- stderr\n%s",
                status, out, err);
        CHECK(0, "a failure named outside the suites fails a run whose suites passed");
    }
}

/** Check the runner's own suite, `runner`, in the map: it passes as any
 * suite does, and a fault of the runner fails it, with one result line and
 * the name once in the failed line. */
static void check_runner(void)
{
    static char out[4096], err[4096];
    int status;
    put("runner.map", "runner guest.fpu\ndefault guest.default\n", 0644);
    status = format(NULL, NULL, path("runner.map"), path("pass.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    if (status != 0 || !strstr(out, "run: runner: PASS in 2s") ||
        !strstr(out, "run: result: all 2 suites passed")) {
        fprintf(stderr, "etude_lines_hosttest: runner: status %d\n--- stdout\n%s--- stderr\n%s",
                status, out, err);
        CHECK(0, "the runner's own suite is a suite of the map");
    }
    put("runner.map", "runner guest.default\n", 0644);
    status = format(NULL, NULL, path("runner.map"), path("failure.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    if (status != 1 || !strstr(err, "run: runner: FAIL in 2s\n") ||
        strstr(err, "run: runner: FAIL in 2s\nrun: runner: FAIL") ||
        !strstr(err, "run: runner: guest.fpu.xsave on x86_64 is in no suite of the map") ||
        !strstr(err, "run: result: failed: runner\n") || strstr(out, "run: runner: PASS")) {
        fprintf(stderr,
                "etude_lines_hosttest: runner fault: status %d\n--- stdout\n%s--- stderr\n%s",
                status, out, err);
        CHECK(0, "the runner's own suite is a suite of the map");
    }
}

/** Check the lines of a run of both architectures, `-a i686,x86_64`: each
 * suite of the map is a suite `ARCH:SUITE` of each architecture, one with no
 * case beside one with cases is left out, an item of no architecture is the
 * first architecture's, and `-a` of one architecture changes nothing. */
static void check_arches(void)
{
    static const struct run_spec none = {"none", 2, {PASS(HOSTFPU), PASS(XSAVE)}, 1, NULL, NULL, 0};
    static char out[4096], err[4096], one[4096];
    int status;
    put("both.map", TWO, 0644);
    status = format(NULL, "i686,x86_64", path("both.map"), path("failure.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    if (status != 1 ||
        !strstr(err, "run: x86_64:fpu: guest.fpu.xsave on x86_64: failure: it exited 1\n") ||
        !strstr(err, "run: x86_64:fpu: FAIL in 2s\n") ||
        !strstr(out, "run: i686:default: PASS in 2s\n") ||
        !strstr(err, "run: result: failed: x86_64:fpu\n") || strstr(out, "i686:fpu") ||
        strstr(err, "i686:fpu") || strstr(out, "x86_64:default") || strstr(err, "runner")) {
        fprintf(stderr, "etude_lines_hosttest: both: status %d\n--- stdout\n%s--- stderr\n%s",
                status, out, err);
        CHECK(0, "a run of both architectures gives each suite's architecture");
    }
    write_stream(&none);
    put("none.map", "fpu guest.fpu\n", 0644);
    status = format(NULL, "i686,x86_64", path("none.map"), path("none.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    if (status != 0 || !strstr(out, "run: i686:fpu: PASS in") ||
        !strstr(out, "run: x86_64:fpu: PASS in") ||
        !strstr(out, "run: result: all 2 suites passed")) {
        fprintf(stderr, "etude_lines_hosttest: none: status %d\n--- stdout\n%s--- stderr\n%s",
                status, out, err);
        CHECK(0, "an item of no architecture is the first architecture's");
    }
    format(NULL, NULL, path("both.map"), path("failure.jsonl"),
           (struct format_output){one, err, sizeof(one)});
    status = format(NULL, "i686", path("both.map"), path("failure.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    if (status != 1 || strcmp(out, one) != 0) {
        fprintf(stderr, "etude_lines_hosttest: one: status %d\n--- stdout\n%s", status, out);
        CHECK(0, "-a of one architecture changes nothing");
    }
    status = format(NULL, "x86_64,i686", path("both.map"), path("failure.jsonl"),
                    (struct format_output){out, err, sizeof(out)});
    CHECK(status == 2, "-a of another value is a wrong usage");
}

/** Give the time of the monotonic clock in microseconds. */
static uint64_t monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * SECOND + (uint64_t)t.tv_nsec / 1000;
}

/** Hold a real write lock, then acknowledge readiness to the parent. */
static void holder_lock(int ready, const char *name)
{
    struct flock lock;
    int fd = -1;
    char byte = 'r';
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (name && ((fd = open(name, O_CREAT | O_RDWR, 0600)) < 0 || fcntl(fd, F_SETLKW, &lock)))
        _exit(1);
    if (write(ready, &byte, 1) != 1)
        _exit(1);
    close(ready);
    /* The lock descriptor remains open until this process exits. */
}

/** Start a process, optionally holding a file lock until input closes. */
static pid_t start_holder(int *write_end, const char *lock)
{
    int p[2], ready[2];
    pid_t pid;
    char c;
    if (pipe(p) || pipe(ready) || (pid = fork()) < 0)
        exit(1);
    if (!pid) {
        close(p[1]);
        close(ready[0]);
        holder_lock(ready[1], lock);
        _exit(read(p[0], &c, 1) < 0);
    }
    observed[nobserved++] = pid;
    close(p[0]);
    close(ready[1]);
    if (read(ready[0], &c, 1) != 1)
        exit(1);
    close(ready[0]);
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
    if (pid > 0)
        observed[nobserved++] = pid;
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
    holder = start_holder(&holder_pipe, path("status.lock"));
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
    gone = start_holder(&end, NULL);
    CHECK(etude_process_state((long)gone, &s.owner) == 1, "the ended owner's identity is read");
    close(end);
    waitpid(gone, &status, 0);
    s.run_id = "run0", s.args = "stale", s.attempts = NULL, s.nattempts = 0;
    snprintf(record, sizeof(record), "%s/.run.status.2", build);
    CHECK(etude_status_write(record, &s) == 0, "the stale record is written");
    for (i = 0; i < 4; i++)
        etude_watchdog_free(w[i]);
}

/** Publish a legacy wait for the file lock held by the owned child. */
static void write_lock_wait(void)
{
    struct etude_pid owner;
    char text[4096];
    if (etude_process_state(getpid(), &owner) != 1 || !owner.start_known ||
        !fx_mkdir(path("root/wait")))
        exit(1);
    snprintf(text, sizeof(text), "identity %ld %ld %llu 0\nwork %s/root/wait\nargs lock-wait\n",
             (long)getpid(), (long)getpgrp(), (unsigned long long)owner.start, directory);
    put("root/build/.run.status.lock", text, 0600);
    snprintf(text, sizeof(text), "reason ports\nsince 1\nlock WRITE %s\n", path("status.lock"));
    put("root/wait/claim.wait", text, 0600);
}

/** Run the status reader on the tree `root`; give its output. */
static void read_status(const char *root, char *out, size_t size)
{
    const char *reader = getenv("ETUDE_LINES_READER"), *tools = getenv("HOST_TEST_TOOLS");
    char python[4096], setting[4096];
    const char *environment[] = {setting, NULL};
    const char *args[] = {python, "-B", reader && *reader ? reader : "tools/flow/test_status.py",
                          root, NULL};
    int n = snprintf(python, sizeof(python), "%s/python3",
                     tools && *tools ? tools : "build/tests/host-suite/tools");
    if (n < 0 || n >= (int)sizeof(python))
        exit(1);
    n = snprintf(setting, sizeof(setting), "RACH2_STATUS_PROC_ROOT=%s",
                 proc_root ? proc_root : "/proc");
    if (n < 0 || n >= (int)sizeof(setting))
        exit(1);
    fx_run(&commands, args, NULL, environment, NULL);
    snprintf(out, size, "%s%s", commands.out ? commands.out : "", commands.err ? commands.err : "");
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
    snprintf(text, sizeof(text),
             "fault: a person must establish that nothing of it runs and drop it with "
             "build/tools/etude/etude release %s A1\n",
             ledger);
    has(out, text, "an uncertain claim names the command that drops it");
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
    observed[nobserved++] = getpid();
    l = fill_ledger(ledger);
    write_records(ledger, build);
    write_lock_wait();
    proc_root = path("proc-unavailable");
    if (!fx_mkdir(proc_root))
        exit(1);
    read_status(root, out, sizeof(out));
    has(out, "test status unavailable: Linux testing host required", "the reader needs Linux");
    proc_root = path("proc-observed");
    if (!status_proc_capture(proc_root, observed, nobserved, path("status.lock"))) {
        CHECK(0, "the fixture records actual process and lock observations");
        etude_ledger_close(l);
        return;
    }
    snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    check_reader(root, ledger, owner);
    read_status(root, out, sizeof(out));
    snprintf(machine, sizeof(machine), "holder %ld alive", (long)holder);
    has(out, "waits for ports since ", "the reader reports the actual lock wait");
    has(out, machine, "the reader identifies the kernel-observed lock holder");
    unlink(path("root/build/.run.status.1"));
    unlink(path("root/build/.run.status.2"));
    unlink(path("root/build/.run.status.lock"));
    read_status(root, out, sizeof(out));
    has(out, "no recorded test run", "the ledger is read after the run's record is gone");
    has(out, "claim A1: uncertain: ", "the ledger is read after the run's record is gone");
    etude_ledger_close(l);
}

/** Compiled tests/run, make and git models for the actual CI products. */
static int fixture_role(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "core")) {
        char *args[] = {"lines",
                        "-s",
                        getenv("ETUDE_LINES_SCHEMAS"),
                        getenv("ETUDE_LINES_MAP"),
                        getenv("ETUDE_LINES_STREAM"),
                        NULL};
        if (!args[2] || !args[3] || !args[4])
            return 125;
        return etude_lines_main(5, args);
    }
    printf("run: %s: PASS in 1s\nrun: result: all 1 suites passed\n", argc > 1 ? argv[1] : "");
    return 0;
}

int main(int argc, char **argv)
{
    const char *base = getenv("TMPDIR"), *name = strrchr(argv[0], '/');
    size_t i;
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "make"))
        return 0;
    if (!strcmp(name, "git")) {
        puts("0000000");
        return 0;
    }
    if (!strcmp(name, "run"))
        return fixture_role(argc, argv);
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
    if (!fx_init(&commands, directory, "commands", self))
        return 1;
    commands_ready = 1;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGHUP, stopped);
    make_tree();
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        check_row(&rows[i]);
    check_maps();
    check_named();
    check_runner();
    check_arches();
    check_status();
    if (failures)
        fprintf(stderr, "etude_lines_hosttest: %d checks failed\n", failures);
    else
        printf("etude_lines_hosttest: result: pass\n");
    return failures != 0;
}
