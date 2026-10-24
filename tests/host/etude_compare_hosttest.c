/* Check that etude's comparison tells noise, too little history and a
 * slowdown apart.
 * host-inputs: tools/etude/schemas/history.schema.json
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_load.c tools/etude/observe_import.c
 * host-deps: tools/etude/json.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-deps: tools/etude/capture.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include -I tools/etude
 */
#include <etude/history.h>
#include <etude/exec.h>
#include "compat/tree_host.h"
#include "compare.h"
#include "compat/load_history_host.h"
#include "../../utils/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECOND UINT64_C(1000000)

static int failures;
static int passed;
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
    int unknown_load;
};

#define CHECK(test, detail)                                                  \
    do {                                                                     \
        if (!(test)) {                                                       \
            fprintf(stderr, "etude_compare_hosttest: failed: %s\n", detail); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/** Remove successful fixtures; retain failures with their exact Git results. */
static void cleanup(void)
{
    if (*directory) {
        if (!passed) {
            fprintf(stderr, "etude_compare_hosttest: fixtures kept in %s\n", directory);
            return;
        }
        if (host_remove_tree(directory) != 0)
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
    char items[16384], time[1200], res[600], ok[400], b[200], m[200], id[300], load[1800];
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
    if (load_fixture(load, sizeof load, directory, r->run, time))
        exit(1);
    if (r->unknown_load)
        strcpy(load, b);
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
             plan_id, items, time, res, b, b, b, load, b, m, m, m, ok);
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

/* The executor uses execve. Resolve Git once without a shell or execvp. */
static const char *git_program(void)
{
    static char resolved[4096];
    const char *env = getenv("PATH");
    if (*resolved)
        return resolved;
    if (!env)
        return NULL;
    char *paths = strdup(env), *part, *next = paths;
    if (!paths)
        return NULL;
    while (next) {
        part = next;
        next = strchr(part, ':');
        if (next)
            *next++ = 0;
        char candidate[4096];
        int n = snprintf(candidate, sizeof candidate, "%s/git", *part ? part : ".");
        if (n > 0 && n < (int)sizeof candidate && access(candidate, X_OK) == 0 &&
            realpath(candidate, resolved))
            break;
    }
    free(paths);
    return *resolved ? resolved : NULL;
}

/** Record the command and every condition used to accept its result. */
static void git_details(FILE *file, const struct etude_exec_spec *spec,
                        const struct etude_exec_result *r, size_t size, int error)
{
    size_t i;

    fprintf(file, "cwd=%s\nstdout=%s\nstderr=%s\noutput_max=%lu result_buffer=%lu\n",
            spec->cwd, spec->stdout_path, spec->stderr_path, (unsigned long)spec->output_max,
            (unsigned long)size);
    fprintf(file, "PATH=%s\nHOME=%s\n", getenv("PATH") ? getenv("PATH") : "",
            getenv("HOME") ? getenv("HOME") : "");
    for (i = 0; spec->argv[i]; i++)
        fprintf(file, "argv[%lu]=%s\n", (unsigned long)i, spec->argv[i]);
    if (!r) {
        fprintf(file, "start_error=%d: %s\n", error, strerror(error));
        return;
    }
    fprintf(file,
            "end=%d value=%d exit_end=%d exit_value=%d cleanup=%d output_error=%d\n"
            "end constants: exited=%d signaled=%d stopped=%d lost=%d; confirmed_cleanup=%d\n"
            "stdout kept=%lu total=%llu closed=%d; stderr kept=%lu total=%llu closed=%d\n"
            "keeper=%ld:%llu:%d process=%ld:%llu:%d children=%u foreign=%u\n",
            r->end, r->value, r->exit_end, r->exit_value, r->cleanup, r->output_error,
            ETUDE_END_EXITED, ETUDE_END_SIGNALED, ETUDE_END_STOPPED, ETUDE_END_LOST,
            ETUDE_CLEANUP_CONFIRMED, (unsigned long)r->out.kept,
            (unsigned long long)r->out.total, r->out.closed, (unsigned long)r->err.kept,
            (unsigned long long)r->err.total, r->err.closed, r->keeper.pid,
            (unsigned long long)r->keeper.start, r->keeper.start_known, r->process.pid,
            (unsigned long long)r->process.start, r->process.start_known, r->children,
            r->foreign);
}

/** Keep the existing exit, cleanup and capture requirements for real Git. */
static int git_succeeded(const struct etude_exec_result *r, size_t size)
{
    return r->end == ETUDE_END_EXITED && r->value == 0 &&
           r->cleanup == ETUDE_CLEANUP_CONFIRMED && !r->output_error &&
           r->out.total == r->out.kept && r->err.total == r->err.kept && r->out.kept < size;
}

/** Run real Git, keeping full streams and status for each distinct command. */
static void git(char *const argv[], char *out, size_t size)
{
    static unsigned sequence;
    struct etude_exec_spec spec;
    struct etude_exec *ex = etude_exec_new();
    struct etude_proc *p = NULL;
    const struct etude_exec_result *r = NULL;
    char name[80];
    FILE *details;
    int ok = 0, error = 0;

    size_t count = 0;
    while (argv[count])
        count++;
    char **arguments = calloc(count + 1, sizeof *arguments);
    const char *program = git_program();
    if (!ex || !arguments || !program) {
        if (!program)
            errno = ENOENT;
        perror("etude_compare_hosttest: Git");
        if (ex)
            etude_exec_free(ex);
        free(arguments);
        exit(1);
    }
    memcpy(arguments, argv, count * sizeof *arguments);
    arguments[0] = (char *)program;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "compare-git";
    spec.argv = arguments;
    extern char **environ;
    spec.envp = environ;
    spec.cwd = directory;
    spec.output_max = 4096;
    snprintf(name, sizeof(name), "git-%u.out", ++sequence);
    spec.stdout_path = path(name);
    snprintf(name, sizeof(name), "git-%u.err", sequence);
    spec.stderr_path = path(name);
    snprintf(name, sizeof(name), "git-%u.result", sequence);
    details = fopen(path(name), "w");
    if (!details) {
        perror("etude_compare_hosttest: Git result file");
        etude_exec_free(ex);
        free(arguments);
        exit(1);
    }
    if (etude_exec_start(ex, &spec, &p) == 0) {
        while (etude_exec_wait(ex, -1) != p)
            ;
        r = etude_proc_result(p);
        ok = git_succeeded(r, size);
        if (ok && out != NULL)
            memcpy(out, r->out.data, r->out.kept + 1);
    } else
        error = errno;
    git_details(details, &spec, r, size, error);
    if (ferror(details))
        ok = 0;
    if (fclose(details))
        ok = 0;
    if (!ok) {
        fputs("etude_compare_hosttest: Git failed:\n", stderr);
        git_details(stderr, &spec, r, size, error);
        if (r) {
            fwrite(r->out.data, 1, r->out.kept, stderr);
            fwrite(r->err.data, 1, r->err.kept, stderr);
        }
    }
    if (ex != NULL)
        etude_exec_free(ex);
    free(arguments);
    if (!ok)
        exit(1);
}

/** Make a repository of three commits, whose hashes the runs name. */
static void make_repository(void)
{
    static const char *const subjects[] = {"Measure the baseline", "Slow down the boot",
                                           "Add a check"};
    char *init[] = {"git", "init", "-q", "repo", NULL};
    char *commit[] = {"git",
                      "-C",
                      "repo",
                      "-c",
                      "user.name=etude",
                      "-c",
                      "user.email=etude@example.com",
                      "-c",
                      "commit.gpgsign=false",
                      "-c",
                      "core.hooksPath=/dev/null",
                      "commit",
                      "-q",
                      "--allow-empty",
                      "-m",
                      NULL,
                      NULL};
    char *head[] = {"git", "-C", "repo", "rev-parse", "HEAD", NULL};
    char hash[64];
    int i;

    unsetenv("GIT_DIR");
    unsetenv("GIT_WORK_TREE");
    git(init, NULL, 4097);
    for (i = 0; i < 3; i++) {
        commit[15] = (char *)subjects[i];
        git(commit, NULL, 4097);
        git(head, hash, sizeof hash);
        if (strlen(hash) != 41 || hash[40] != '\n') {
            fprintf(stderr, "etude_compare_hosttest: cannot read commit %d\n", i);
            exit(1);
        }
        memcpy(commits[i], hash, 40);
        commits[i][40] = '\0';
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

/* A passing result without whole-run evidence is not a baseline sample. */
static void check_load_evidence(char *out, size_t size)
{
    struct spec r;
    memset(&r, 0, sizeof r);
    r.run = "unknown-load";
    r.hour = 30;
    r.boot_us = 91 * SECOND;
    r.execute_us = 10 * SECOND;
    r.unknown_load = 1;
    write_spec(&r);
    struct cmp_history observed;
    char *history = (char *)path(r.run);
    CHECK(cmp_load(&observed, schema, &history, 1) == 0 && observed.sample_count == 1,
          "load-only exclusion preserves a successful timing observation");
    if (observed.sample_count == 1)
        CHECK(observed.samples[0].reason[0] && observed.samples[0].watchdog_eligible,
              "unknown outside load excludes performance but still teaches a successful watchdog "
              "bound");
    cmp_free(&observed);
    history = (char *)path("d1");
    CHECK(cmp_load(&observed, schema, &history, 1) == 0 && observed.sample_count == 1,
          "read a run with an actual recorded disruption");
    if (observed.sample_count == 1)
        CHECK(!observed.samples[0].watchdog_eligible,
              "an actual disruption cannot teach a watchdog bound");
    cmp_free(&observed);
    CHECK(compare(out, size, "unknown-load " HISTORY " @unknown-load") == 0,
          "unknown load is reported without failing the comparison command");
    has(out, "verdict: not_comparable", "a run with unknown load is excluded");
    has(out, "no verified continuous load observation", "the missing observation is named");
    r.run = "changed-trace";
    r.unknown_load = 0;
    write_spec(&r);
    /* Read the newly published run and change its trace after publication. */
    struct cmp_history h;
    char *stream = (char *)path(r.run);
    CHECK(cmp_load(&h, schema, &stream, 1) == 0 && h.runs == 1,
          "the synthetic load record is eligible before its trace changes");
    if (h.runs == 1) {
        const struct eh_json *ref = eh_json_get(h.by_run[0]->data, "load");
        const char *file = eh_json_get(ref, "locations")->child->string;
        char *bytes = NULL;
        size_t length;
        CHECK(cmp_read_file(file, 65536, &bytes, &length) == 0, "read the load fixture");
        struct eh_json *load = bytes ? eh_json_parse(bytes) : NULL;
        ref = eh_json_get(eh_json_get(load, "outer"), "trace");
        const struct eh_json *locations = eh_json_get(ref, "locations");
        FILE *f = locations ? fopen(locations->child->string, "ab") : NULL;
        CHECK(f != NULL && fputs(" ", f) >= 0, "change published trace bytes");
        if (f)
            CHECK(fclose(f) == 0, "close changed trace");
        eh_json_free(load);
        free(bytes);
    }
    cmp_free(&h);
    CHECK(compare(out, size, "changed-trace " HISTORY " @changed-trace") == 0,
          "a changed trace is reported without failing the comparison command");
    has(out, "verdict: not_comparable", "a trace digest mismatch excludes the run");
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
    if (mkdir(path("store"), 0700) != 0)
        exit(1);
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
    check_load_evidence(out, sizeof(out));
    if (failures)
        fprintf(stderr, "etude_compare_hosttest: %d checks failed\n", failures);
    else
        printf("etude_compare_hosttest: result: pass\n");
    passed = failures == 0;
    return failures != 0;
}
