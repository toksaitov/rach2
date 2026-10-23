/* Check that `etude show failures` lists each attempt of a run that did not
 * pass, keeps a failure that a passing retry follows, and refuses a history
 * it cannot read whole.
 * host-deps: tools/etude/failures.c tools/etude/history.c
 * host-deps: tools/etude/history_json.c tools/etude/history_collect.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude
 */
#include <etude/history.h>
#include "failures.h"
#include "../../utils/sha256.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECOND UINT64_C(1000000)
#define TRIES  3 /* the attempts the plan gives each item */

static int failures;
static char directory[1024];
static const char schema[] = "tools/etude/schemas/history.schema.json";

/* One attempt of an item, and the results document the store keeps of it. */
struct attempt_spec {
    const char *outcome;    /* pass, failure, incomplete or cancelled */
    const char *reason;     /* NULL for a pass */
    const char *disruption; /* NULL when none */
    const char *results;    /* NULL when the store keeps none */
};

/* One item of the plan: its case, its attempts and the run record's outcome. */
struct item_spec {
    const char *case_id;
    const char *summary; /* the outcome the run record gives it */
    int count;           /* the attempts written */
    struct attempt_spec attempts[TRIES];
};

/* One run: its items, the run record's outcome, and the faults written. */
struct run_spec {
    const char *run;
    const char *outcome; /* NULL writes no run record */
    const char *reason;
    const struct item_spec *items;
    int count;
    int stray;    /* write an attempt the plan does not give its item */
    int dangling; /* give an item a record that is not an attempt of it */
};

#define CHECK(test, detail)                                                   \
    do {                                                                      \
        if (!(test)) {                                                        \
            fprintf(stderr, "etude_failures_hosttest: failed: %s\n", detail); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/** Remove the fixtures, even after a failure. */
static void cleanup(void)
{
    char command[1200];
    if (*directory) {
        snprintf(command, sizeof(command), "rm -rf '%s'", directory);
        if (system(command))
            fprintf(stderr, "etude_failures_hosttest: cannot remove %s\n", directory);
    }
}

/** Exit through the fixture cleanup after a signal. */
static void stopped(int number)
{
    exit(128 + number);
}

/** Write a path under the fixture directory. */
static const char *path(const char *name)
{
    static char paths[64][1200];
    static unsigned next;
    char *out = paths[next++ % 64];
    snprintf(out, sizeof(paths[0]), "%s/%s", directory, name);
    return out;
}

/** Write the hexadecimal SHA-256 of a text. */
static void digest_of(const char *bytes, char hex[65])
{
    struct sha256 sha;
    unsigned char digest[32];
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, strlen(bytes));
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
}

/** Write a blob reference to a text, and when `keep` is set, keep the blob. */
static void blob(char *out, size_t size, const char *bytes, int keep)
{
    char hex[65], name[80];
    FILE *file;
    digest_of(bytes, hex);
    snprintf(out, size, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"test://blob\"]}",
             hex, (unsigned long)strlen(bytes));
    if (!keep)
        return;
    snprintf(name, sizeof(name), "store/%s", hex);
    file = fopen(path(name), "wb");
    if (!file || fputs(bytes, file) < 0 || fclose(file)) {
        fprintf(stderr, "etude_failures_hosttest: cannot keep a blob\n");
        exit(1);
    }
}

/** Write a UTC time some microseconds after the start of the test's day. */
static void utc_at(uint64_t us, char out[40])
{
    uint64_t s = us / SECOND;
    snprintf(out, 40, "2026-10-20T%02u:%02u:%02u.%06uZ", (unsigned)(s / 3600 % 24),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60), (unsigned)(us % SECOND));
}

/** Write a time interval of the test's boot, in microseconds from its start. */
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

/** Write a result with its outcome, reason and disruption. */
static void result(char *out, size_t size, const char *outcome, const char *reason,
                   const char *disruption)
{
    snprintf(out, size,
             "{\"outcome\":\"%s\",\"reason\":%s%s%s,\"cleanup\":\"confirmed\",\"evidence\":[],"
             "\"capture_errors\":[],\"disruptions\":[%s%s%s]}",
             outcome, reason ? "\"" : "", reason ? reason : "null", reason ? "\"" : "",
             disruption ? "\"" : "", disruption ? disruption : "", disruption ? "\"" : "");
}

/** Write the resources of an interval. */
static void resources(char *out, size_t size)
{
    char scope[200];
    blob(scope, sizeof(scope), "scope", 0);
    snprintf(out, size,
             "{\"scope\":%s,\"tree_cpu_us\":{\"value\":1,\"method\":\"rusage-v1\","
             "\"quality\":\"exact\",\"interval_us\":null,\"error_bound\":1},"
             "\"combined_peak_bytes\":{\"value\":null,\"reason\":\"not sampled in the test\"},"
             "\"samples\":[]}",
             scope);
}

/** Append one record, checking that the writer takes it. */
static void append(struct etude_history *h, const char *kind, const char *data, char *id)
{
    if (etude_history_record_id(h, id, 300) ||
        etude_history_append(h, id, "2026-10-20T00:00:00Z", kind, data)) {
        fprintf(stderr, "etude_failures_hosttest: the writer refused a %s record: %s\n", kind,
                data);
        exit(1);
    }
}

/* A stream being written, and the identity of its plan record. */
struct stream {
    struct etude_history *h;
    char plan_id[300];
};

/** Write the plan of a run, which gives each item TRIES attempts. */
static void write_plan(struct etude_history *h, const struct run_spec *r, char *plan_id)
{
    static char data[65536], items[49152];
    char b[200], env[2400];
    size_t used = 0;
    int i;
    blob(b, sizeof(b), "fixed", 0);
    for (i = 0; i < r->count; i++)
        used += (size_t)snprintf(
            items + used, sizeof(items) - used,
            "%s{\"item_id\":\"%s-i%d\",\"case_id\":\"%s\",\"family_id\":\"f\","
            "\"architecture\":\"x86_64\",\"domain\":\"qemu\",\"workload\":%s,\"assertions\":%s,"
            "\"profile\":%s,\"repetition\":0,\"attempt_ids\":[\"%s-i%d-a0\",\"%s-i%d-a1\","
            "\"%s-i%d-a2\"]}",
            i ? "," : "", r->run, i, r->items[i].case_id, b, b, b, r->run, i, r->run, i, r->run, i);
    snprintf(env, sizeof(env),
             "{\"machine_id\":\"m1\",\"boot_id\":\"b1\",\"os\":\"linux\",\"os_version\":\"26.04\","
             "\"accelerator\":\"kvm\",\"site_profile\":%s,\"tools\":%s,\"capacity\":%s,"
             "\"cache_state\":%s,\"load\":%s}",
             b, b, b, b, b);
    snprintf(data, sizeof(data),
             "{\"purpose\":\"ci\",\"selection\":%s,\"items\":[%s],\"commit\":"
             "\"0123456789abcdef0123456789abcdef01234567\",\"source\":%s,\"runner\":%s,"
             "\"artifacts\":[],\"environment\":%s,\"concurrency\":%s,\"policies\":%s,"
             "\"exemption\":null,\"reproduce\":%s}",
             b, items, b, b, env, b, b, b);
    append(h, "plan", data, plan_id);
}

/** Write attempt K of item I as the attempt NAME; return its end. */
static uint64_t write_attempt(const struct stream *s, const struct run_spec *r, int i, int k,
                              const char *name, char *id)
{
    static char data[16384];
    const struct attempt_spec *a = &r->items[i].attempts[k];
    char previous[220], time[1200], res[600], ok[600], results[200];
    uint64_t start = (uint64_t)(i * TRIES + k) * SECOND;
    snprintf(previous, sizeof(previous), k ? "\"%s-i%d-a%d\"" : "null", r->run, i, k - 1);
    interval(time, sizeof(time), start, start + SECOND / 2);
    resources(res, sizeof(res));
    result(ok, sizeof(ok), a->outcome, a->reason, a->disruption);
    blob(results, sizeof(results), a->results ? a->results : "no results", a->results != NULL);
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"item_id\":\"%s-i%d\",\"attempt_id\":\"%s\","
             "\"previous_attempt_id\":%s,\"steps\":[],\"time\":%s,"
             "\"queue_us\":{\"value\":0,\"method\":\"monotonic-v1\",\"quality\":\"exact\","
             "\"interval_us\":null,\"error_bound\":1},\"resources\":%s,"
             "\"assertion_results\":%s,\"result\":%s}",
             s->plan_id, r->run, i, name, previous, time, res, results, ok);
    append(s->h, "attempt", data, id);
    return start + SECOND / 2;
}

/** Write the run record, which lists each item's attempt records. */
static void write_run(struct etude_history *h, const struct run_spec *r, const char *plan_id,
                      char ids[][TRIES][300], uint64_t end)
{
    static char data[65536], items[49152];
    char time[1200], res[600], ok[600], b[200], m[200], id[300];
    size_t used = 0;
    int i, k;
    for (i = 0; i < r->count; i++) {
        used += (size_t)snprintf(items + used, sizeof(items) - used,
                                 "%s{\"item_id\":\"%s-i%d\",\"attempt_records\":[", i ? "," : "",
                                 r->run, i);
        for (k = 0; k < r->items[i].count; k++)
            used += (size_t)snprintf(items + used, sizeof(items) - used, "%s\"%s\"", k ? "," : "",
                                     ids[i][k]);
        if (r->dangling && i == 0)
            used += (size_t)snprintf(items + used, sizeof(items) - used, "%s\"%s\"",
                                     r->items[i].count ? "," : "", plan_id);
        used += (size_t)snprintf(items + used, sizeof(items) - used, "],\"outcome\":\"%s\"}",
                                 r->items[i].summary);
    }
    interval(time, sizeof(time), 0, end);
    resources(res, sizeof(res));
    result(ok, sizeof(ok), r->outcome, r->reason, NULL);
    blob(b, sizeof(b), "fixed", 0);
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

/** Write a run's stream to the fixture file FILE. */
static void write_spec(const struct run_spec *r, const char *file)
{
    static char ids[64][TRIES][300];
    struct stream s;
    char stream[300], name[300], stray[300];
    uint64_t end = SECOND;
    int i, k;
    snprintf(stream, sizeof(stream), "%s-s", r->run);
    s.h = etude_history_open(path(file), schema, stream, r->run, UINT64_C(1) << 24);
    if (!s.h) {
        fprintf(stderr, "etude_failures_hosttest: cannot open the stream of %s\n", r->run);
        exit(1);
    }
    write_plan(s.h, r, s.plan_id);
    for (i = 0; i < r->count; i++)
        for (k = 0; k < r->items[i].count; k++) {
            snprintf(name, sizeof(name), "%s-i%d-a%d", r->run, i, k);
            end = write_attempt(&s, r, i, k, name, ids[i][k]);
        }
    if (r->stray) {
        snprintf(name, sizeof(name), "%s-i0-a9", r->run);
        write_attempt(&s, r, 0, 0, name, stray);
    }
    if (r->outcome)
        write_run(s.h, r, s.plan_id, ids, end);
    if (etude_history_close(s.h))
        exit(1);
}

/**
 * Run `etude show failures` with stdout in a file; return its status and
 * output. A word of ARGUMENTS that begins with `@` names a fixture file.
 */
static int run(char *output, size_t size, const char *arguments)
{
    char *argv[64], copy[4096], *word;
    int argc = 0, saved, file, status;
    ssize_t got;
    snprintf(copy, sizeof(copy), "etude %s", arguments);
    for (word = strtok(copy, " "); word && argc < 63; word = strtok(NULL, " "))
        argv[argc++] = word[0] == '@' ? (char *)path(word + 1) : word;
    argv[argc] = NULL;
    fflush(stdout);
    saved = dup(1);
    file = open(path("output"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(file, 1);
    close(file);
    status = etude_failures_main(argc, argv);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    file = open(path("output"), O_RDONLY);
    got = read(file, output, size - 1);
    close(file);
    output[got > 0 ? got : 0] = 0;
    return status;
}

/** Check that the output has a line that is TEXT. */
static void has(const char *output, const char *text, const char *detail)
{
    const char *at = output;
    size_t length = strlen(text);
    while ((at = strstr(at, text)) && !((at == output || at[-1] == '\n') && at[length] == '\n'))
        at++;
    if (!at) {
        fprintf(stderr, "etude_failures_hosttest: output lacks the line \"%s\":\n%s\n", text,
                output);
        CHECK(0, detail);
    }
}

/** Count the lines of an output. */
static int lines_of(const char *output)
{
    int n = 0;
    for (; *output; output++)
        n += *output == '\n';
    return n;
}

/** Write a results document whose report is in STATE, with BAD failures. */
static void results_doc(char *out, size_t size, const char *state, int bad)
{
    size_t used;
    int i;
    used = (size_t)snprintf(out, size,
                            "{\"schema_version\":1,\"report\":{\"state\":\"%s\",\"records\":4,"
                            "\"errors\":[%s]},\"assertions\":[{\"assertion_id\":\"cpuid-leaf7\","
                            "\"source\":\"program\",\"outcome\":\"pass\",\"detail\":null}",
                            state, strcmp(state, "complete") ? "\"no end record\"" : "");
    for (i = 0; i < bad; i++) {
        char id[32];
        snprintf(id, sizeof(id), i ? "check-%d" : "trusted-monobit", i);
        used += (size_t)snprintf(out + used, size - used,
                                 ",{\"assertion_id\":\"%s\",\"source\":\"program\","
                                 "\"outcome\":\"fail\",\"detail\":\"%s\"}",
                                 id, i ? "a wrong value" : "61 of 512 bits set");
    }
    snprintf(out + used, size - used, "],\"undeclared\":[]}\n");
}

static char flaky_doc[2048], six_doc[4096], damaged_doc[2048];

/* An attempt that passed, one that failed, and one whose bound ran out. */
#define PASSED           {"pass", NULL, NULL, NULL}
#define FAILED(why, doc) {"failure", why, NULL, doc}
#define HUNG             {"incomplete", "its execute bound of 30000000 us ran out", NULL, NULL}
#define RNG              "guest.default.rngprobe"
#define ERRNO            "guest.default.errnoprobe"

/* The run of a case whose failure a passing retry follows, and its twins. */
static const struct item_spec flaky_items[] = {
    {RNG, "failure", 2, {FAILED("it exited 1", flaky_doc), PASSED}},
    {ERRNO, "pass", 1, {PASSED}},
};
static const struct item_spec hidden_items[] = {
    {RNG, "pass", 2, {FAILED("it exited 1", flaky_doc), PASSED}},
    {ERRNO, "pass", 1, {PASSED}},
};
static const struct item_spec pass_items[] = {
    {RNG, "pass", 1, {PASSED}},
    {ERRNO, "pass", 1, {PASSED}},
};
/* The run of pass_items with one attempt record of another content. */
static const struct item_spec twin_items[] = {
    {RNG, "pass", 1, {FAILED("it exited 1", NULL)}},
    {ERRNO, "pass", 1, {PASSED}},
};
static const struct item_spec cut_items[] = {
    {RNG, "pass", 1, {PASSED}},
    {ERRNO, "pass", 0, {PASSED}},
};
static const struct item_spec hung_items[] = {
    {RNG, "incomplete", 2, {HUNG, HUNG}},
};
static const struct item_spec disrupted_items[] = {
    {RNG,
     "failure",
     2,
     {{"failure", "it exited 1", "a backup job ran beside the case", NULL}, PASSED}},
};
static const struct item_spec kept_items[] = {
    {RNG, "failure", 1, {FAILED("it exited 1", damaged_doc)}},
    {ERRNO, "failure", 1, {FAILED("it exited 2", NULL)}},
    {"guest.default.auxvprobe", "failure", 1, {FAILED("it exited 3", six_doc)}},
};
static struct item_spec many_items[40];

/** Write the streams of every run the rows read. */
static void write_runs(void)
{
    static const struct run_spec runs[] = {
        {"flaky", "failure", "of 3 attempts, 1 failed, 0 did not finish", flaky_items, 2, 0, 0},
        {"hidden", "pass", NULL, hidden_items, 2, 0, 0},
        {"pass", "pass", NULL, pass_items, 2, 0, 0},
        {"cut", NULL, NULL, cut_items, 2, 0, 0},
        {"hung", "incomplete", "of 2 attempts, 0 failed, 2 did not finish", hung_items, 1, 0, 0},
        {"disrupted", "failure", "of 2 attempts, 1 failed", disrupted_items, 1, 0, 0},
        {"kept", "failure", "of 3 attempts, 3 failed", kept_items, 3, 0, 0},
        {"many", "failure", "of 80 attempts, 80 failed", many_items, 40, 0, 0},
        {"stray", "failure", "of 3 attempts, 1 failed", flaky_items, 2, 1, 0},
        {"dangling", "failure", "of 3 attempts, 1 failed", flaky_items, 2, 0, 1},
    };
    struct run_spec twin = {"twin", "pass", NULL, pass_items, 2, 0, 0};
    size_t i;
    results_doc(flaky_doc, sizeof(flaky_doc), "complete", 1);
    results_doc(six_doc, sizeof(six_doc), "truncated", 6);
    results_doc(damaged_doc, sizeof(damaged_doc), "complete", 2);
    for (i = 0; i < 40; i++) {
        many_items[i].case_id = "guest.default.rngprobe";
        many_items[i].summary = "failure";
        many_items[i].count = 2;
        many_items[i].attempts[0] = flaky_items[0].attempts[0];
        many_items[i].attempts[1] = flaky_items[0].attempts[0];
    }
    for (i = 0; i < sizeof(runs) / sizeof(runs[0]); i++)
        write_spec(&runs[i], runs[i].run);
    write_spec(&twin, "twin-a");
    twin.items = twin_items;
    write_spec(&twin, "twin-b");
}

/** A failure that a passing retry follows stays listed, with its results. */
static void check_flaky(char *out, size_t size)
{
    CHECK(run(out, size, "-b @store @flaky") == 1, "a run with a failed attempt gives status 1");
    has(out, "run flaky: failure: of 3 attempts, 1 failed, 0 did not finish",
        "the run's outcome and reason are given");
    has(out,
        "guest.default.rngprobe on x86_64, item flaky-i0: failed, and passed on another attempt, "
        "so it may be flaky",
        "a failure that a passing retry follows is listed as possibly flaky");
    has(out, "  attempt flaky-i0-a0: failure: it exited 1", "the original failure stays listed");
    has(out, "    trusted-monobit: fail: 61 of 512 bits set",
        "the failed assertion of the original attempt is listed");
    has(out, "  attempt flaky-i0-a1, a retry of flaky-i0-a0: pass", "the passing retry is listed");
    CHECK(!strstr(out, "errnoprobe") && !strstr(out, "cpuid-leaf7"), "what passed is not listed");
    CHECK(run(out, size, "@flaky") == 1 && !strstr(out, "trusted-monobit"),
          "without a store no results are read");
    CHECK(run(out, size, "-b @store @flaky @flaky") == 1 && lines_of(out) == 5,
          "a stream given twice is read once");
}

/** A run record that calls a failed item passed does not hide the failure. */
static void check_hidden(char *out, size_t size)
{
    CHECK(run(out, size, "-b @store @hidden") == 1,
          "the failure stays listed when the run record calls the item passed");
    has(out, "  attempt hidden-i0-a0: failure: it exited 1",
        "the failure stays listed when the run record calls the item passed");
    CHECK(!strstr(out, "run hidden:"), "a run whose record passes has no run line");
}

/** A pass, a run cut short, a case that did not finish and a disrupted one. */
static void check_outcomes(char *out, size_t size)
{
    CHECK(run(out, size, "-b @store @pass") == 0 && !*out,
          "a run whose every attempt passed lists nothing");
    CHECK(run(out, size, "@cut") == 1, "a run without its run record gives status 1");
    has(out, "run cut: no run record, so the run did not finish", "a cut run is named");
    has(out, "guest.default.errnoprobe on x86_64, item cut-i1: no attempt was recorded",
        "an item without attempts is listed");
    CHECK(!strstr(out, "cut-i0"), "an item whose attempt passed is not listed");
    CHECK(run(out, size, "@hung") == 1, "a run that did not finish gives status 1");
    has(out, "guest.default.rngprobe on x86_64, item hung-i0: did not finish",
        "an item whose bound ran out did not finish");
    has(out,
        "  attempt hung-i0-a1, a retry of hung-i0-a0: incomplete: its execute bound of 30000000 "
        "us ran out",
        "each incomplete attempt is listed with its reason");
    CHECK(run(out, size, "@disrupted") == 1, "a disrupted failure gives status 1");
    has(out, "guest.default.rngprobe on x86_64, item disrupted-i0: failed",
        "a disrupted failure does not make its case possibly flaky");
    has(out,
        "  attempt disrupted-i0-a0: failure: it exited 1; disrupted: a backup job ran beside the "
        "case",
        "the disruption is listed");
}

/** Results damaged or not kept in the store, and too many to list. */
static void check_results(char *out, size_t size)
{
    char hex[65], name[80];
    FILE *file;
    digest_of(damaged_doc, hex);
    snprintf(name, sizeof(name), "store/%s", hex);
    file = fopen(path(name), "r+b");
    if (!file || fputc('X', file) == EOF || fclose(file))
        exit(1);
    CHECK(run(out, size, "-b @store @kept") == 1, "the run with damaged results gives status 1");
    has(out, "    results: the store's copy does not match the history's digest",
        "a damaged results document is not read");
    has(out, "    results: not in the store", "a results document not kept is named");
    has(out, "    report: truncated: no end record", "a truncated report is named");
    has(out, "    check-3: fail: a wrong value", "the fourth failed assertion is listed");
    has(out, "    and 2 more assertions that did not pass", "assertions past four are counted");
    CHECK(run(out, size, "-b @store @many") == 1, "a run of forty failed items gives status 1");
    CHECK(lines_of(out) <= 80, "the listing stops at 80 lines");
    CHECK(strstr(out, "more items that did not pass, which the history lists") != NULL,
          "the items not listed are counted");
    has(out, "guest.default.rngprobe on x86_64, item many-i0: failed",
        "the first items are listed whole");
}

/** Streams that cannot be read whole, and records naming what is not there. */
static void check_damage(char *out, size_t size)
{
    FILE *file = fopen(path("bad"), "wb");
    if (!file || fputs("not a record\n", file) < 0 || fclose(file))
        exit(1);
    file = fopen(path("flaky"), "ab");
    if (!file || fputs("{\"schema_version\":1", file) < 0 || fclose(file))
        exit(1);
    CHECK(run(out, size, "@flaky") == 2, "a stream whose last record is incomplete is refused");
    CHECK(run(out, size, "@pass @bad") == 2, "an invalid stream is refused");
    CHECK(run(out, size, "@pass @hung") == 2, "a history of two plans is refused");
    CHECK(run(out, size, "@twin-a @twin-b") == 2, "a record with two contents is refused");
    CHECK(run(out, size, "@stray") == 2, "an attempt the plan does not give is refused");
    CHECK(run(out, size, "@dangling") == 2,
          "a run record that gives an item what is not its attempt is refused");
    CHECK(run(out, size, "") == 2 && run(out, size, "-x @pass") == 2, "a wrong usage is refused");
    CHECK(run(out, size, "-s @nowhere @pass") == 2, "a missing schema is refused");
}

int main(void)
{
    static char out[1 << 18];
    const char *base = getenv("TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    atexit(cleanup);
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGHUP, stopped);
    if (snprintf(directory, sizeof(directory), "%s/rach2-etude-failures.XXXXXX", base) >=
            (int)sizeof(directory) ||
        !mkdtemp(directory))
        return 1;
    if (mkdir(path("store"), 0700)) {
        perror("etude_failures_hosttest: mkdir");
        return 1;
    }
    write_runs();
    check_flaky(out, sizeof(out));
    check_hidden(out, sizeof(out));
    check_outcomes(out, sizeof(out));
    check_results(out, sizeof(out));
    check_damage(out, sizeof(out));
    if (failures)
        fprintf(stderr, "etude_failures_hosttest: %d checks failed\n", failures);
    else
        printf("etude_failures_hosttest: result: pass\n");
    return failures != 0;
}
