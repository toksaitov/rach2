/* Check that etude's comparison tells noise, too little history and a
 * slowdown apart.
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude
 */
#include <etude/history.h>
#include "compare.h"
#include "../../utils/sha256.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECOND UINT64_C(1000000)

static int failures;
static char directory[1024];
static char commits[3][41];
static const char schema[] = "tools/etude/schemas/history.schema.json";
static const char reproduce_doc[] =
    "{\"schema_version\":1,\"items\":[{\"item_id\":\"slow-i0\",\"directory\":\"/src/rach2\","
    "\"environment\":[{\"name\":\"ARCH\",\"value\":\"x86_64\"}],"
    "\"preparation\":[[\"make\",\"build/tests/c.boot\"]],"
    "\"case\":[\"etude\",\"run\",\"c.boot\"],"
    "\"profile\":[\"perf\",\"record\",\"-g\",\"--\",\"etude\",\"run\",\"c.boot\"]}]}";

/* One run of the synthetic history: its case, timing and faults. */
struct spec {
    const char *run, *commit, *architecture, *accelerator, *workload, *case_id, *outcome;
    int hour, items, disrupted, no_run_record, retry;
    uint64_t boot_us, execute_us;
    const char *reproduce;
};

#define CHECK(test, detail)                                                  \
    do {                                                                     \
        if (!(test)) {                                                       \
            fprintf(stderr, "etude_compare_hosttest: failed: %s\n", detail); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/** Remove the fixtures, even after a failure. */
static void cleanup(void)
{
    char command[1200];
    if (*directory) {
        snprintf(command, sizeof(command), "rm -rf '%s'", directory);
        if (system(command))
            fprintf(stderr, "etude_compare_hosttest: cannot remove %s\n", directory);
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

/** Write a UTC time some microseconds after the start of the test's month. */
static void utc_at(uint64_t us, char out[40])
{
    uint64_t s = us / SECOND;
    snprintf(out, 40, "2026-10-%02uT%02u:%02u:%02u.%06uZ", (unsigned)(1 + s / 86400),
             (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60),
             (unsigned)(us % SECOND));
}

/** Write a time interval on the run's boot, from start to end microseconds. */
static void interval(char *out, size_t size, const struct spec *r, uint64_t start, uint64_t end)
{
    char a[40], b[40];
    uint64_t base = (uint64_t)r->hour * 3600 * SECOND;
    utc_at(base + start, a);
    utc_at(base + end, b);
    snprintf(out, size,
             "{\"start_utc\":\"%s\",\"end_utc\":\"%s\",\"clock\":{\"machine_id\":\"m1\","
             "\"boot_id\":\"%s-boot\",\"clock_id\":\"monotonic\",\"resolution_us\":1},"
             "\"start_us\":%llu,\"end_us\":%llu,\"elapsed_us\":{\"value\":%llu,"
             "\"method\":\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null,"
             "\"error_bound\":1}}",
             a, b, r->run, (unsigned long long)start, (unsigned long long)end,
             (unsigned long long)(end - start));
}

/** Write a result: a pass, the spec's outcome, or a disruption. */
static void result(char *out, size_t size, const char *outcome, int disrupted)
{
    snprintf(out, size,
             "{\"outcome\":\"%s\",\"reason\":%s,\"cleanup\":\"confirmed\",\"evidence\":[],"
             "\"capture_errors\":[],\"disruptions\":[%s]}",
             outcome ? outcome : "pass", outcome ? "\"an assertion failed\"" : "null",
             disrupted ? "\"a backup job ran during the run\"" : "");
}

/** Write the resources of an interval, with a processor time. */
static void resources(char *out, size_t size, uint64_t cpu)
{
    char scope[200];
    blob(scope, sizeof(scope), "scope");
    snprintf(out, size,
             "{\"scope\":%s,\"tree_cpu_us\":{\"value\":%llu,\"method\":\"rusage-v1\","
             "\"quality\":\"exact\",\"interval_us\":null,\"error_bound\":1},"
             "\"combined_peak_bytes\":{\"value\":null,\"reason\":\"not sampled in the test\"},"
             "\"samples\":[]}",
             scope, (unsigned long long)cpu);
}

/** Append one record, checking that the writer takes it. */
static void append(struct etude_history *h, const char *kind, const char *data, char *id)
{
    char utc[40];
    utc_at(0, utc);
    if (etude_history_record_id(h, id, 300) || etude_history_append(h, id, utc, kind, data)) {
        fprintf(stderr, "etude_compare_hosttest: the writer refused a %s record: %s\n", kind, data);
        exit(1);
    }
}

/** Write the plan of a run, with one item for each repetition. */
static void write_plan(struct etude_history *h, const struct spec *r, char *plan_id)
{
    static char data[65536];
    char items[16384], b[200], w[200], env[2400], rep[200];
    size_t used = 0;
    int i;
    blob(b, sizeof(b), "fixed");
    blob(w, sizeof(w), r->workload ? r->workload : "w1");
    blob(rep, sizeof(rep), r->reproduce ? r->reproduce : "none");
    for (i = 0; i < r->items; i++)
        used += (size_t)snprintf(
            items + used, sizeof(items) - used,
            "%s{\"item_id\":\"%s-i%d\",\"case_id\":\"%s\",\"family_id\":\"f\","
            "\"architecture\":\"%s\",\"domain\":\"qemu\",\"workload\":%s,\"assertions\":%s,"
            "\"profile\":%s,\"repetition\":%d,\"attempt_ids\":[\"%s-a%d\"%s%s%s]}",
            i ? "," : "", r->run, i, r->case_id ? r->case_id : "c.boot",
            r->architecture ? r->architecture : "x86_64", w, b, b, i, r->run, i,
            r->retry ? ",\"" : "", r->retry ? r->run : "", r->retry ? "-retry\"" : "");
    snprintf(env, sizeof(env),
             "{\"machine_id\":\"m1\",\"boot_id\":\"%s-boot\",\"os\":\"linux\","
             "\"os_version\":\"26.04\",\"accelerator\":\"%s\",\"site_profile\":%s,\"tools\":%s,"
             "\"capacity\":%s,\"cache_state\":%s,\"load\":%s}",
             r->run, r->accelerator ? r->accelerator : "kvm", b, b, b, b, b);
    snprintf(data, sizeof(data),
             "{\"purpose\":\"focused\",\"selection\":%s,\"items\":[%s],\"commit\":\"%s\","
             "\"source\":%s,\"runner\":%s,\"artifacts\":[%s],\"environment\":%s,"
             "\"concurrency\":%s,\"policies\":%s,\"exemption\":null,\"reproduce\":%s}",
             b, items, r->commit ? r->commit : commits[0], b, b, b, env, b, b, rep);
    append(h, "plan", data, plan_id);
}

/* The stream an attempt is written to, its run, plan and item. */
struct place {
    struct etude_history *h;
    const struct spec *r;
    const char *plan_id;
    int item;
};

/** Write a step of an attempt. */
static void write_step(const struct place *p, const char *attempt, const char *step, uint64_t start,
                       uint64_t end, char *id)
{
    static char data[16384];
    char time[1200], res[600], ok[400];
    interval(time, sizeof(time), p->r, start, end);
    resources(res, sizeof(res), (end - start) / 2);
    result(ok, sizeof(ok), NULL, 0);
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"item_id\":\"%s-i%d\",\"attempt_id\":\"%s\",\"step_id\":\"%s\","
             "\"stage\":\"%s\",\"time\":%s,\"resources\":%s,\"witnesses\":[],"
             "\"max_progress_gap_us\":{\"value\":null,\"reason\":\"not traced in the test\"},"
             "\"result\":%s}",
             p->plan_id, p->r->run, p->item, attempt, step,
             strcmp(step, "boot") ? "execute" : "boot", time, res, ok);
    append(p->h, "step", data, id);
}

/** Write an attempt with its boot and execute steps; return its end. */
static uint64_t write_attempt(const struct place *p, int retry, uint64_t start, char *id)
{
    static char data[16384];
    char attempt[200], previous[220], boot[300], execute[300], time[1200], res[600], ok[400];
    char b[200];
    const struct spec *r = p->r;
    uint64_t middle = start + r->boot_us, end = middle + r->execute_us;
    snprintf(attempt, sizeof(attempt), retry ? "%s-retry" : "%s-a%d", r->run, p->item);
    snprintf(previous, sizeof(previous), retry ? "\"%s-a%d\"" : "null", r->run, p->item);
    write_step(p, attempt, "boot", start, middle, boot);
    write_step(p, attempt, "execute", middle, end, execute);
    interval(time, sizeof(time), r, start, end);
    resources(res, sizeof(res), r->boot_us / 2);
    result(ok, sizeof(ok), retry ? NULL : r->outcome, 0);
    blob(b, sizeof(b), "results");
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"item_id\":\"%s-i%d\",\"attempt_id\":\"%s\","
             "\"previous_attempt_id\":%s,\"steps\":[\"%s\",\"%s\"],\"time\":%s,"
             "\"queue_us\":{\"value\":0,\"method\":\"monotonic-v1\",\"quality\":\"exact\","
             "\"interval_us\":null,\"error_bound\":1},\"resources\":%s,"
             "\"assertion_results\":%s,\"result\":%s}",
             p->plan_id, r->run, p->item, attempt, previous, boot, execute, time, res, b, ok);
    append(p->h, "attempt", data, id);
    return end;
}

/** Write the run record that sums up the attempts. */
static void write_run(struct etude_history *h, const struct spec *r, const char *plan_id,
                      char attempts[][300], uint64_t end)
{
    static char data[65536];
    char items[16384], time[1200], res[600], ok[400], b[200], m[200], id[300];
    size_t used = 0;
    int i;
    for (i = 0; i < r->items; i++)
        used += (size_t)snprintf(items + used, sizeof(items) - used,
                                 "%s{\"item_id\":\"%s-i%d\",\"attempt_records\":[\"%s\"%s%s%s],"
                                 "\"outcome\":\"%s\"}",
                                 i ? "," : "", r->run, i, attempts[2 * i], r->retry ? ",\"" : "",
                                 r->retry ? attempts[2 * i + 1] : "", r->retry ? "\"" : "",
                                 r->outcome ? r->outcome : "pass");
    interval(time, sizeof(time), r, 0, end);
    resources(res, sizeof(res), end / 2);
    result(ok, sizeof(ok), r->outcome, r->disrupted);
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
             "\"verdict\":\"pass\"},\"result\":%s}",
             plan_id, items, time, res, b, b, b, b, b, m, m, m, ok);
    append(h, "run", data, id);
}

/** Write a run's stream, named for the run. */
static void write_spec(const struct spec *given)
{
    struct spec r = *given;
    struct etude_history *h;
    char plan_id[300], attempts[32][300], stream[300];
    uint64_t at = 0;
    int i;
    if (!r.items)
        r.items = 1;
    snprintf(stream, sizeof(stream), "%s-s", r.run);
    h = etude_history_open(path(r.run), schema, stream, r.run, UINT64_C(1) << 24);
    if (!h) {
        fprintf(stderr, "etude_compare_hosttest: cannot open the stream of %s\n", r.run);
        exit(1);
    }
    write_plan(h, &r, plan_id);
    for (i = 0; i < r.items; i++) {
        struct place p;
        p.h = h;
        p.r = &r;
        p.plan_id = plan_id;
        p.item = i;
        at = write_attempt(&p, 0, at, attempts[2 * i]);
        if (r.retry)
            at = write_attempt(&p, 1, at, attempts[2 * i + 1]);
    }
    if (!r.no_run_record)
        write_run(h, &r, plan_id, attempts, at);
    if (etude_history_close(h))
        exit(1);
}

/** Run a command of the shell in the fixture directory; exit when it fails. */
static void shell(const char *command)
{
    char line[2400];
    snprintf(line, sizeof(line), "cd '%s' && %s", directory, command);
    if (system(line)) {
        fprintf(stderr, "etude_compare_hosttest: failed: %s\n", command);
        exit(1);
    }
}

/** Make a repository of three commits, whose hashes the runs name. */
static void make_repository(void)
{
    static const char *const subjects[] = {"Measure the baseline", "Slow down the boot",
                                           "Add a check"};
    char command[1200];
    FILE *pipe;
    int i;
    unsetenv("GIT_DIR");
    unsetenv("GIT_WORK_TREE");
    shell("git init -q repo");
    for (i = 0; i < 3; i++) {
        snprintf(command, sizeof(command),
                 "git -C repo -c user.name=etude -c user.email=etude@example.com "
                 "-c commit.gpgsign=false -c core.hooksPath=/dev/null commit -q --allow-empty "
                 "-m '%s'",
                 subjects[i]);
        shell(command);
        snprintf(command, sizeof(command), "git -C '%s/repo' rev-parse HEAD", directory);
        pipe = popen(command, "r");
        if (!pipe || !fgets(commits[i], sizeof(commits[i]), pipe) || pclose(pipe)) {
            fprintf(stderr, "etude_compare_hosttest: cannot read commit %d\n", i);
            exit(1);
        }
    }
}

/** Run `etude compare` with stdout in a file; return its status and output. */
static int compare(char *output, size_t size, const char *arguments)
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
    status = cmp_main(argc, argv);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    file = open(path("output"), O_RDONLY);
    got = read(file, output, size - 1);
    close(file);
    output[got > 0 ? got : 0] = 0;
    return status;
}

/** Check that the output has a line with this text. */
static void has(const char *output, const char *text, const char *detail)
{
    if (!strstr(output, text)) {
        fprintf(stderr, "etude_compare_hosttest: output lacks \"%s\":\n%s\n", text, output);
        CHECK(0, detail);
    }
}

/** Write the history: five samples and the samples to be left out. */
static void write_history(void)
{
    static const uint64_t seconds[] = {99, 100, 100, 100, 101};
    static const char *const names[] = {"b1", "b2", "b3", "b4", "b5"};
    struct spec r;
    int i;
    for (i = 0; i < 5; i++) {
        memset(&r, 0, sizeof(r));
        r.run = names[i];
        r.hour = i + 1;
        r.boot_us = (seconds[i] - 10) * SECOND;
        r.execute_us = 10 * SECOND;
        write_spec(&r);
    }
    memset(&r, 0, sizeof(r));
    r.boot_us = 290 * SECOND;
    r.execute_us = 10 * SECOND;
    r.run = "f1", r.hour = 6, r.outcome = "failure";
    write_spec(&r);
    r.run = "d1", r.hour = 7, r.outcome = NULL, r.disrupted = 1;
    write_spec(&r);
    r.run = "n1", r.hour = 8, r.disrupted = 0, r.no_run_record = 1;
    write_spec(&r);
    r.run = "r1", r.hour = 9, r.no_run_record = 0, r.retry = 1, r.outcome = "failure";
    write_spec(&r);
    r.run = "x1", r.hour = 10, r.retry = 0, r.outcome = NULL, r.architecture = "i686";
    write_spec(&r);
    r.run = "t1", r.hour = 11, r.architecture = NULL, r.accelerator = "tcg";
    write_spec(&r);
}

/** Write a candidate run of the series, at its hour and commit. */
static void candidate(const char *run, int hour, uint64_t boot_seconds, const char *workload)
{
    struct spec r;
    memset(&r, 0, sizeof(r));
    r.run = run;
    r.hour = hour;
    r.commit = commits[2];
    r.workload = workload;
    r.boot_us = boot_seconds * SECOND;
    r.execute_us = 10 * SECOND;
    r.reproduce = reproduce_doc;
    write_spec(&r);
}

#define HISTORY "@b1 @b2 @b3 @b4 @b5 @f1 @d1 @n1 @r1 @x1 @t1"

/** A candidate within the noise, with the samples left out named. */
static void check_noise(char *out, size_t size)
{
    CHECK(compare(out, size, "noise " HISTORY " @noise") == 0, "within noise exits 0");
    has(out, "verdict: within_noise", "101 s against [99,100,100,100,101] is noise");
    has(out, "5 samples, median 100.000000 s, deviation 0.000000 s, band 1.000000 s",
        "the baseline's median, deviation and one-percent band");
    has(out, "band 1.010000 s", "the candidate's band is one percent of 101 s");
    has(out, "left out f1-s/3: it ended in failure", "a failed attempt is left out");
    has(out, "left out d1-s/3: its run was disrupted", "a disrupted run is left out");
    has(out, "left out n1-s/3: its run has no run record", "a run without its record is left out");
    has(out, "left out r1-s/6: it retries attempt r1-a0", "a retry is not independent");
    CHECK(!strstr(out, "x1-s/") && !strstr(out, "t1-s/"),
          "another architecture or accelerator is another series");
    has(out, "commits in between: git log --reverse", "without a repository, the command");
}

/** A slowdown, with its commits, its steps and the commands to profile it. */
static void check_slowdown(char *out, size_t size)
{
    char store[1200], line[200];
    FILE *file;
    struct sha256 sha;
    unsigned char digest[32];
    char hex[65];
    snprintf(store, sizeof(store), "%s", path("store"));
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)reproduce_doc, strlen(reproduce_doc));
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    snprintf(line, sizeof(line), "store/%s", hex);
    file = fopen(path(line), "wb");
    if (!file || fputs(reproduce_doc, file) < 0 || fclose(file))
        exit(1);
    CHECK(compare(out, size, "-g @repo -b @store slow " HISTORY " @slow") == 0,
          "a regression is not a failure");
    has(out, "verdict: regression", "110 s against 100 s is a regression");
    has(out, "is 10.000000 s slower than the baseline's, 100.000000 s",
        "the reason gives the difference");
    snprintf(line, sizeof(line), "  %s Slow down the boot", commits[1]);
    has(out, line, "the commits in between are listed");
    snprintf(line, sizeof(line), "  %s Add a check", commits[2]);
    has(out, line, "the candidate's commit is listed");
    CHECK(!strstr(out, "Measure the baseline"), "the baseline's commit is not in between");
    has(out, "boot (boot): baseline 90.000000 s, candidate 100.000000 s",
        "the boot step is compared");
    has(out, "combined band 1.900000 s: regression", "the boot step changed");
    has(out,
        "execute (execute): baseline 10.000000 s, candidate 10.000000 s, combined band "
        "0.200000 s: within_noise",
        "the execute step did not change");
    has(out, "processor time of the tree: baseline median 45.000000 s of 5",
        "processor time is apart from the elapsed time");
    has(out, "  profile: perf record -g -- etude run c.boot", "the profiler's command");
    has(out, "  case: etude run c.boot", "the case's command");
}

/** Too little history, a faster candidate and a changed workload. */
static void check_other_verdicts(char *out, size_t size)
{
    CHECK(compare(out, size, "slow @b2 @b3 @b4 @f1 @slow") == 0, "too few samples exits 0");
    has(out, "verdict: insufficient_history", "three equal samples are too few");
    has(out, "3 eligible baseline samples, and median-mad-v1 requires 5", "the count is given");
    CHECK(compare(out, size, "fast " HISTORY " @fast") == 0, "an improvement exits 0");
    has(out, "verdict: improved", "90 s against 100 s is an improvement");
    CHECK(compare(out, size, "changed " HISTORY " @changed") == 0, "a new workload exits 0");
    has(out, "verdict: not_comparable", "a changed workload is not compared");
    has(out, "its workload differs from the candidate's", "the differing field is named");
    CHECK(compare(out, size, "missing " HISTORY) == 2, "a run without a plan is an error");
    CHECK(compare(out, size, "-o @x slow @slow") == 2, "-o needs -i and a store");
}

/** Return the verdict line of a comparison of slow7 with its options. */
static const char *verdict_of(char *out, size_t size, const char *options)
{
    char arguments[1200];
    snprintf(arguments, sizeof(arguments),
             "%s slow7 " HISTORY " @slow @accepted @slow2 @slow3 @slow4 @slow5 @slow6 @slow7",
             options);
    CHECK(compare(out, size, arguments) == 0, "the comparison of slow7 exits 0");
    return strstr(out, "verdict: ");
}

/** Count a stream's records; return -1 when it is not a valid closed stream. */
static int count_records(const char *line, void *context)
{
    (void)line;
    ++*(int *)context;
    return 0;
}

/** An accepted baseline stays with slow samples until -n asks for a new one. */
static void check_accepted(char *out, size_t size)
{
    char name[16];
    uint64_t bad;
    int i, records = 0;
    const char *verdict;
    CHECK(compare(out, size, "-b @store -o @accepted -i cmp-1 slow " HISTORY " @slow") == 0,
          "a recorded regression exits 0");
    CHECK(etude_history_read(path("accepted"), schema, count_records, &records, &bad) == 0 &&
              records == 1,
          "the comparison stream is valid and has one record");
    for (i = 2; i <= 7; i++) {
        snprintf(name, sizeof(name), "slow%d", i);
        candidate(name, 20 + i, 100, NULL);
    }
    verdict = verdict_of(out, size, "");
    CHECK(verdict && !strncmp(verdict, "verdict: regression", 19),
          "five slow runs do not move the accepted baseline");
    has(out, "accepted by comparison cmp-1/0", "the accepted baseline is named");
    verdict = verdict_of(out, size, "-n");
    CHECK(verdict && !strncmp(verdict, "verdict: within_noise", 21),
          "a new baseline takes the slow runs since the accepted one");
    has(out, "6 samples, median 110.000000 s", "the new baseline has the six slow runs");
}

/** Damaged streams are noted, and the others still compared. */
static void check_damage(char *out, size_t size)
{
    struct spec r;
    FILE *file;
    memset(&r, 0, sizeof(r));
    r.run = "lost", r.hour = 12, r.case_id = "c.other", r.boot_us = r.execute_us = SECOND;
    write_spec(&r);
    file = fopen(path("lost"), "ab");
    if (!file || fputs("{\"schema_version\":1", file) < 0 || fclose(file))
        exit(1);
    file = fopen(path("bad"), "wb");
    if (!file || fputs("not a record\n", file) < 0 || fclose(file))
        exit(1);
    CHECK(compare(out, size, "noise " HISTORY " @lost @bad @noise") == 0,
          "damaged streams do not stop the comparison");
    has(out, "verdict: within_noise", "the other streams are still compared");
    has(out, "ends in an incomplete line", "a lost last line is noted");
    has(out, "bad is invalid at line 1, and none of its records is used",
        "an invalid stream is noted");
}

/** Check the statistics against the examples of the measurement record. */
static void check_statistics(void)
{
    static const uint64_t base[] = {99 * SECOND, 100 * SECOND, 100 * SECOND, 100 * SECOND,
                                    101 * SECOND};
    static const uint64_t even[] = {1, 2, 3, 4};
    uint64_t one = 101 * SECOND, ten = 110 * SECOND;
    struct cmp_stats b, c, e;
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    memset(&e, 0, sizeof(e));
    CHECK(!cmp_statistics(base, 5, &b) && b.median_us == 100 * SECOND && b.mad_us == 0 &&
              b.band_us == SECOND,
          "the baseline of the example has median 100 s, MAD 0 and band 1 s");
    CHECK(!cmp_statistics(&one, 1, &c) && c.band_us == 1010000 &&
              !strcmp(cmp_verdict(&b, &c), "within_noise"),
          "101 s is within the combined band of 2.01 s");
    CHECK(!cmp_statistics(&ten, 1, &c) && !strcmp(cmp_verdict(&b, &c), "regression"),
          "110 s is beyond the combined band of 2.1 s");
    CHECK(!cmp_statistics(even, 4, &e) && e.median4 == 10 && e.median_us == 3 && e.mad_us == 1 &&
              e.band_us == 5,
          "an even count takes the midpoints, and the band uses 3 * 1.4826 * MAD");
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
    if (snprintf(directory, sizeof(directory), "%s/rach2-etude-compare.XXXXXX", base) >=
            (int)sizeof(directory) ||
        !mkdtemp(directory))
        return 1;
    shell("mkdir store");
    make_repository();
    check_statistics();
    write_history();
    candidate("noise", 20, 91, NULL);
    candidate("slow", 21, 100, NULL);
    candidate("fast", 22, 80, NULL);
    candidate("changed", 23, 90, "w2");
    check_noise(out, sizeof(out));
    check_slowdown(out, sizeof(out));
    check_other_verdicts(out, sizeof(out));
    check_accepted(out, sizeof(out));
    check_damage(out, sizeof(out));
    if (failures)
        fprintf(stderr, "etude_compare_hosttest: %d checks failed\n", failures);
    else
        printf("etude_compare_hosttest: result: pass\n");
    return failures != 0;
}
