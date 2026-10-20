/**
 * Check etude's admission and schedule: the requests of the ledger
 * (tools/etude/ledger.c), a worker's schedule (tools/etude/sched.c) and the
 * costs it takes from history (tools/etude/cost.c).
 *
 * host-deps: tools/etude/ledger.c tools/etude/sched.c tools/etude/cost.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c tools/etude/posix/process.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 *
 * A processor reservation is the median ratio of processor time to elapsed
 * time with its headroom, within the ceiling, or the declared value without
 * history; a cost comes from the samples measured where the case will run,
 * and not from failed runs, another architecture or another accelerator.
 * Jobs of both architectures share one order, by preparation and cost.
 *
 * A blocked request is protected: a claim that would take what it needs is
 * refused, though it fits now, and one that takes none of it is admitted.
 * The protected request is chosen fairly between accounts and stays chosen.
 * A request whose owner has died is removed; one that claims left uncertain
 * by a dead worker leave no room for is refused, and one too large for the
 * machine as well. A worker keeps one request waiting. Six workers of three
 * accounts then run 48 jobs on one machine: every job runs once, and no
 * reading of the ledger or of the jobs' own account of what they hold finds
 * more taken than the machine has.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "../../utils/sha256.h"
#include "cost.h"
#include "etude/history.h"
#include "etude/sched.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            printf("etude-sched-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                             \
        }                                                                                        \
    } while (0)

#define CPU    "cpu_millis"
#define MEM    "memory_bytes"
#define SECOND UINT64_C(1000000)

/* The machine of every row: four processors and 8 KiB of memory. */
static const struct etude_ledger_res CAP[] = {{CPU, 4000}, {MEM, 8192}};
#define NCAP (sizeof(CAP) / sizeof(CAP[0]))

/* The process that runs the rows, which alone removes the fixtures. */
static pid_t parent;

/** Leave through the fixtures' cleanup after a signal, in the parent. */
static void stopped(int sig)
{
    if (getpid() != parent)
        _exit(128 + sig);
    printf("etude-sched-test: fail: stopped by signal %d\n", sig);
    exit(1);
}

/* The resources of one claim. */
struct need {
    struct etude_ledger_res r[2];
};

/** Fill `n` with `cpu` thousandths of a processor and `mem` bytes. */
static const struct etude_ledger_res *need(struct need *n, uint64_t cpu, uint64_t mem)
{
    n->r[0].name = CPU;
    n->r[0].amount = cpu;
    n->r[1].name = MEM;
    n->r[1].amount = mem;
    return n->r;
}

/** Ask for claim `id` of `cpu` and `mem` for `account`; the call's result. */
static int ask(struct etude_ledger *l, const char *id, const char *account, uint64_t cpu,
               uint64_t mem, int wait)
{
    struct need n;

    return etude_ledger_admit(l, id, account, need(&n, cpu, mem), 2, CAP, NCAP, wait);
}

/** Say whether `ask` was refused with `err`. */
static int refused(int rc, int err)
{
    return rc == -1 && errno == err;
}

/* What a visit of the ledger looks for, and what it finds. */
struct look {
    const char *id;
    char state[16];
    uint64_t cpu, mem; /* the sums of the claims, requests left out */
    size_t entries;
};

/** Note the state of the claim looked for, and add up the claims. */
static int look_visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct look *k = ctx;
    size_t i;

    k->entries++;
    if (k->id != NULL && strcmp(c->id, k->id) == 0)
        snprintf(k->state, sizeof(k->state), "%s", c->state);
    if (strcmp(c->state, "waiting") == 0 || strcmp(c->state, "blocked") == 0)
        return 0;
    for (i = 0; i < c->nres; i++) {
        if (strcmp(c->res[i].name, CPU) == 0)
            k->cpu += c->res[i].amount;
        if (strcmp(c->res[i].name, MEM) == 0)
            k->mem += c->res[i].amount;
    }
    return 0;
}

/** Read the ledger into `k`, which names the claim looked for; 0 or -1. */
static int look(struct etude_ledger *l, const char *id, struct look *k)
{
    memset(k, 0, sizeof(*k));
    k->id = id;
    return etude_ledger_list(l, look_visit, k);
}

/** Say whether claim or request `id` is in state `state`, "" for none. */
static int state_is(struct etude_ledger *l, const char *id, const char *state)
{
    struct look k;

    return look(l, id, &k) == 0 && strcmp(k.state, state) == 0;
}

/** Say whether the ledger keeps nothing. */
static int empty(struct etude_ledger *l)
{
    struct look k;

    return look(l, NULL, &k) == 0 && k.entries == 0;
}

/** End claim `id` of this process with its cleanup confirmed. */
static int done(struct etude_ledger *l, const char *id)
{
    return etude_ledger_finish(l, id, ETUDE_CLEANUP_CONFIRMED);
}

/* ---- Processor reservations and costs. ---- */

/** The median ratio with its headroom, the declared value, and the bounds. */
static void test_cpu(void)
{
    static const uint64_t c3[] = {5 * SECOND, 6 * SECOND, 22 * SECOND};
    static const uint64_t e3[] = {10 * SECOND, 12 * SECOND, 11 * SECOND};
    static const uint64_t c2[] = {1, 3}, e2[] = {1, 1};
    static const uint64_t c1[] = {1}, e1[] = {3}, z[] = {0}, nine[] = {9}, ten[] = {10};

    CHECK(etude_sched_cpu(NULL, NULL, 0, 1500, 4000) == 1500, "cpu");
    CHECK(etude_sched_cpu(NULL, NULL, 0, 5000, 4000) == 4000, "cpu");
    CHECK(etude_sched_cpu(c3, e3, 3, 9999, 8000) == 625, "cpu");
    CHECK(etude_sched_cpu(c2, e2, 2, 9999, 8000) == 2500, "cpu");
    CHECK(etude_sched_cpu(c2, e2, 2, 9999, 2000) == 2000, "cpu");
    CHECK(etude_sched_cpu(c1, e1, 1, 9999, 8000) == 418, "cpu");
    CHECK(etude_sched_cpu(nine, z, 1, 1500, 8000) == 1500, "cpu");
    CHECK(etude_sched_cpu(z, ten, 1, 1500, 8000) == 1, "cpu");
}

/* One run of the synthetic history, with one attempt of one case. */
struct run_spec {
    const char *run, *architecture, *accelerator, *load, *outcome;
    uint64_t elapsed_us;
    int64_t cpu_us; /* -1 for a processor time that is unavailable */
};

/** Write into `out` a blob reference to the bytes `bytes`. */
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

/** Write into `out` an interval of `us` microseconds on the run's boot. */
static void interval(char *out, size_t size, const char *run, uint64_t us)
{
    snprintf(out, size,
             "{\"start_utc\":\"2026-10-01T00:00:00.000000Z\",\"end_utc\":"
             "\"2026-10-01T00:%02u:%02u.%06uZ\",\"clock\":{\"machine_id\":\"m1\",\"boot_id\":"
             "\"%s-boot\",\"clock_id\":\"monotonic\",\"resolution_us\":1},\"start_us\":0,"
             "\"end_us\":%llu,\"elapsed_us\":{\"value\":%llu,\"method\":\"monotonic-v1\","
             "\"quality\":\"exact\",\"interval_us\":null,\"error_bound\":1}}",
             (unsigned)(us / SECOND / 60), (unsigned)(us / SECOND % 60), (unsigned)(us % SECOND),
             run, (unsigned long long)us, (unsigned long long)us);
}

/** Write into `out` the resources of an interval with processor time `cpu`. */
static void resources(char *out, size_t size, int64_t cpu)
{
    char scope[200], value[200];

    blob(scope, sizeof(scope), "scope");
    if (cpu < 0)
        snprintf(value, sizeof(value), "{\"value\":null,\"reason\":\"not measured\"}");
    else
        snprintf(value, sizeof(value),
                 "{\"value\":%lld,\"method\":\"rusage-v1\",\"quality\":\"lower_bound\","
                 "\"interval_us\":null,\"error_bound\":null}",
                 (long long)cpu);
    snprintf(out, size,
             "{\"scope\":%s,\"tree_cpu_us\":%s,\"combined_peak_bytes\":{\"value\":null,"
             "\"reason\":\"not sampled\"},\"samples\":[]}",
             scope, value);
}

/** Write into `out` a result of `outcome`, a pass when it is NULL. */
static void result(char *out, size_t size, const char *outcome)
{
    snprintf(out, size,
             "{\"outcome\":\"%s\",\"reason\":%s,\"cleanup\":\"confirmed\",\"evidence\":[],"
             "\"capture_errors\":[],\"disruptions\":[]}",
             outcome ? outcome : "pass", outcome ? "\"an assertion failed\"" : "null");
}

/** Append a record of `kind` to `h`, its identity in `id`; 0 or -1. */
static int append(struct etude_history *h, const char *kind, const char *data, char *id)
{
    if (etude_history_record_id(h, id, 300) != 0 ||
        etude_history_append(h, id, "2026-10-01T00:01:00.000000Z", kind, data) != 0) {
        printf("etude-sched-test: the history refused a %s record: %s\n", kind, data);
        return -1;
    }
    return 0;
}

/** Write at `path` the stream of run `r`: plan, attempt and run; 0 or -1. */
static int write_run(const char *path, const struct run_spec *r)
{
    static char data[16384];
    char b[200], ld[200], env[2400], time[1200], res[600], ok[400], stream[64];
    char plan_id[300], attempt_id[300], run_id[300];
    struct etude_history *h;
    int rc = -1;

    snprintf(stream, sizeof(stream), "%s-s", r->run);
    h = etude_history_open(path, "tools/etude/schemas/history.schema.json", stream, r->run,
                           UINT64_C(1) << 22);
    if (h == NULL)
        return -1;
    blob(b, sizeof(b), "fixed");
    blob(ld, sizeof(ld), r->load);
    snprintf(env, sizeof(env),
             "{\"machine_id\":\"m1\",\"boot_id\":\"%s-boot\",\"os\":\"linux\","
             "\"os_version\":\"26.04\",\"accelerator\":\"%s\",\"site_profile\":%s,\"tools\":%s,"
             "\"capacity\":%s,\"cache_state\":%s,\"load\":%s}",
             r->run, r->accelerator, b, b, b, b, ld);
    snprintf(data, sizeof(data),
             "{\"purpose\":\"focused\",\"selection\":%s,\"items\":[{\"item_id\":\"%s-i\","
             "\"case_id\":\"c.boot\",\"family_id\":\"f\",\"architecture\":\"%s\","
             "\"domain\":\"qemu\",\"workload\":%s,\"assertions\":%s,\"profile\":%s,"
             "\"repetition\":0,\"attempt_ids\":[\"%s-a\"]}],\"commit\":"
             "\"1111111111111111111111111111111111111111\",\"source\":%s,\"runner\":%s,"
             "\"artifacts\":[],\"environment\":%s,\"concurrency\":%s,\"policies\":%s,"
             "\"exemption\":null,\"reproduce\":%s}",
             b, r->run, r->architecture, b, b, b, r->run, b, b, env, ld, b, b);
    if (append(h, "plan", data, plan_id) != 0)
        goto out;
    interval(time, sizeof(time), r->run, r->elapsed_us);
    resources(res, sizeof(res), r->cpu_us);
    result(ok, sizeof(ok), r->outcome);
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"item_id\":\"%s-i\",\"attempt_id\":\"%s-a\","
             "\"previous_attempt_id\":null,\"steps\":[],\"time\":%s,\"queue_us\":{\"value\":0,"
             "\"method\":\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null,"
             "\"error_bound\":1},\"resources\":%s,\"assertion_results\":%s,\"result\":%s}",
             plan_id, r->run, r->run, time, res, b, ok);
    if (append(h, "attempt", data, attempt_id) != 0)
        goto out;
    snprintf(data, sizeof(data),
             "{\"plan_id\":\"%s\",\"items\":[{\"item_id\":\"%s-i\",\"attempt_records\":[\"%s\"],"
             "\"outcome\":\"%s\"}],\"preparation_steps\":[],\"time\":%s,\"resources\":%s,"
             "\"stage_totals\":%s,\"family_totals\":%s,\"useful_concurrency\":%s,\"load\":%s,"
             "\"budget\":{\"policy\":%s,\"charged_us\":{\"value\":%llu,\"method\":"
             "\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null,\"error_bound\":1},"
             "\"outside_queue_us\":{\"value\":0,\"method\":\"monotonic-v1\",\"quality\":"
             "\"exact\",\"interval_us\":null,\"error_bound\":1},\"one_time_setup_us\":{\"value\":"
             "0,\"method\":\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null,"
             "\"error_bound\":1},\"goal_us\":3600000000,\"growth_us\":0,\"ceiling_us\":"
             "3600000000,\"cleanup_reserve_us\":1000000,\"verdict\":\"pass\"},\"result\":%s}",
             plan_id, r->run, attempt_id, r->outcome ? r->outcome : "pass", time, res, b, b, b, b,
             b, (unsigned long long)r->elapsed_us, ok);
    if (append(h, "run", data, run_id) != 0)
        goto out;
    rc = 0;
out:
    if (etude_history_close(h) != 0)
        rc = -1;
    return rc;
}

/**
 * The cost of a case from its history: the samples of the same machine,
 * architecture and accelerator under any load, and none of a failed run,
 * of i686 or of TCG.
 */
static void test_cost(void)
{
    static const struct run_spec runs[] = {
        {"r1", "x86_64", "kvm", "quiet", NULL, 10 * SECOND, 5 * (int64_t)SECOND},
        {"r2", "x86_64", "kvm", "quiet", NULL, 12 * SECOND, 6 * (int64_t)SECOND},
        {"r3", "x86_64", "kvm", "quiet", NULL, 11 * SECOND, 22 * (int64_t)SECOND},
        {"l1", "x86_64", "kvm", "busy", NULL, 11 * SECOND, 11 * (int64_t)SECOND / 2},
        {"u1", "x86_64", "kvm", "quiet", NULL, 11 * SECOND, -1},
        {"f1", "x86_64", "kvm", "quiet", "failure", 50 * SECOND, 400 * (int64_t)SECOND},
        {"i1", "i686", "kvm", "quiet", NULL, 50 * SECOND, 50 * (int64_t)SECOND},
        {"t1", "x86_64", "tcg", "quiet", NULL, 90 * SECOND, 90 * (int64_t)SECOND},
    };
    enum { NRUNS = sizeof(runs) / sizeof(runs[0]) };
    char *paths[NRUNS];
    struct cmp_history h;
    struct cmp_key key;
    struct etude_cost cost;
    const struct cmp_sample *r1 = NULL;
    size_t i;

    for (i = 0; i < NRUNS; i++) {
        char name[16];

        snprintf(name, sizeof(name), "%s.jsonl", runs[i].run);
        paths[i] = (char *)fix_path(name);
        if (write_run(paths[i], &runs[i]) != 0) {
            CHECK(0, "cost");
            return;
        }
    }
    if (cmp_load(&h, "tools/etude/schemas/history.schema.json", paths, NRUNS) != 0) {
        CHECK(0, "cost");
        return;
    }
    for (i = 0; i < h.sample_count; i++)
        if (strcmp(h.samples[i].attempt->run, "r1") == 0)
            r1 = &h.samples[i];
    CHECK(r1 != NULL && r1->reason[0] == '\0', "cost");
    if (r1 != NULL) {
        key = r1->key;
        CHECK(etude_cost_of(&h, &key, 77, 1500, 8000, &cost) == 0, "cost");
        CHECK(cost.samples == 5 && cost.cpu_samples == 4, "cost");
        CHECK(cost.elapsed_us == 11 * SECOND, "cost");
        CHECK(cost.cpu_millis == 625, "cost");
        key.value[0] = "c.none";
        CHECK(etude_cost_of(&h, &key, 77, 1500, 8000, &cost) == 0, "cost");
        CHECK(cost.samples == 0 && cost.elapsed_us == 77 && cost.cpu_millis == 1500, "cost");
    }
    cmp_free(&h);
}

/* ---- The order of a schedule. ---- */

/** Both architectures in one order: preparation first, then the costlier. */
static void test_order(void)
{
    struct etude_job jobs[] = {
        {"A", "a", "i686", 1, 1, 10, NULL, 0, ETUDE_JOB_PENDING, 0},
        {"B", "b", "x86_64", 0, 1, 5, NULL, 0, ETUDE_JOB_PENDING, 0},
        {"C", "b", "i686", 0, 1, 9, NULL, 0, ETUDE_JOB_PENDING, 0},
        {"D", "a", "x86_64", 0, 1, 9, NULL, 0, ETUDE_JOB_PENDING, 0},
        {"E", "a", "i686", 0, 1, 9, NULL, 0, ETUDE_JOB_PENDING, 0},
    };
    static const char *const want[] = {"E", "D", "C", "B", "A"};
    size_t i, n = sizeof(jobs) / sizeof(jobs[0]);

    etude_sched_order(jobs, n);
    for (i = 0; i < n; i++)
        CHECK(strcmp(jobs[i].id, want[i]) == 0, "order");
}

/* ---- Admission through the ledger. ---- */

/**
 * A request blocked on the processors is protected: a claim that fits now
 * but would take what it needs is refused, by etude_ledger_reserve too, and
 * the blocked request is admitted once the claim that held it has ended.
 */
static void test_backfill(void)
{
    struct etude_ledger *l = etude_ledger_open(fix_path("backfill"));
    struct need n;

    if (l == NULL) {
        CHECK(0, "backfill");
        return;
    }
    CHECK(ask(l, "hold", "a", 3000, 0, 1) == 0 && state_is(l, "hold", "reserved"), "backfill");
    CHECK(refused(ask(l, "big", "b", 4000, 0, 1), EAGAIN) && state_is(l, "big", "blocked"),
          "backfill");
    CHECK(refused(ask(l, "small", "c", 1000, 0, 0), EAGAIN) && state_is(l, "small", ""),
          "backfill");
    CHECK(refused(ask(l, "small", "c", 1000, 0, 1), EAGAIN) && state_is(l, "small", "waiting"),
          "backfill");
    CHECK(refused(etude_ledger_reserve(l, "plain", need(&n, 1000, 0), 2, CAP, NCAP), EAGAIN),
          "backfill");
    CHECK(done(l, "hold") == 0, "backfill");
    CHECK(refused(ask(l, "small", "c", 1000, 0, 1), EAGAIN), "backfill");
    CHECK(ask(l, "big", "b", 4000, 0, 1) == 0 && state_is(l, "big", "reserved"), "backfill");
    CHECK(refused(ask(l, "small", "c", 1000, 0, 1), EAGAIN) && state_is(l, "small", "blocked"),
          "backfill");
    CHECK(done(l, "big") == 0 && ask(l, "small", "c", 1000, 0, 1) == 0, "backfill");
    CHECK(done(l, "small") == 0 && empty(l), "backfill");
    etude_ledger_close(l);
}

/**
 * A request blocked on memory keeps the memory it needs, and no more: a claim
 * of processors alone is admitted beside it, and one that takes memory is
 * refused, though the memory is free now.
 */
static void test_beside(void)
{
    struct etude_ledger *l = etude_ledger_open(fix_path("beside"));

    if (l == NULL) {
        CHECK(0, "beside");
        return;
    }
    CHECK(ask(l, "hold", "a", 1000, 6144, 1) == 0, "beside");
    CHECK(refused(ask(l, "mem", "b", 1000, 4096, 1), EAGAIN) && state_is(l, "mem", "blocked"),
          "beside");
    CHECK(ask(l, "cpu-only", "c", 1000, 0, 0) == 0, "beside");
    CHECK(refused(ask(l, "takes-mem", "c", 1000, 1024, 0), EAGAIN), "beside");
    CHECK(done(l, "hold") == 0 && ask(l, "mem", "b", 1000, 4096, 1) == 0, "beside");
    CHECK(done(l, "mem") == 0 && done(l, "cpu-only") == 0 && empty(l), "beside");
    etude_ledger_close(l);
}

/**
 * The protected request is the one whose account holds the fewest claims,
 * before one that has waited longer, and stays protected when an account
 * that came later would now come first.
 */
static void test_fair(void)
{
    struct etude_ledger *l = etude_ledger_open(fix_path("fair"));

    if (l == NULL) {
        CHECK(0, "fair");
        return;
    }
    CHECK(ask(l, "a-1", "a", 4000, 0, 1) == 0, "fair");
    CHECK(refused(ask(l, "a-2", "a", 1000, 0, 1), EAGAIN) && state_is(l, "a-2", "blocked"), "fair");
    CHECK(refused(ask(l, "a-3", "a", 1000, 0, 1), EAGAIN), "fair");
    CHECK(refused(ask(l, "b-1", "b", 3000, 0, 1), EAGAIN) && state_is(l, "b-1", "waiting"), "fair");
    CHECK(done(l, "a-1") == 0 && ask(l, "a-2", "a", 1000, 0, 1) == 0, "fair");
    CHECK(refused(ask(l, "a-3", "a", 1000, 0, 1), EAGAIN), "fair");
    CHECK(state_is(l, "b-1", "blocked") && state_is(l, "a-3", "waiting"), "fair");

    CHECK(ask(l, "b-2", "b", 0, 1024, 0) == 0, "sticky");
    CHECK(refused(ask(l, "a-3", "a", 1000, 0, 1), EAGAIN), "sticky");
    CHECK(state_is(l, "b-1", "blocked") && state_is(l, "a-3", "waiting"), "sticky");
    CHECK(done(l, "a-2") == 0 && done(l, "b-2") == 0, "sticky");
    CHECK(ask(l, "b-1", "b", 3000, 0, 1) == 0 && ask(l, "a-3", "a", 1000, 0, 1) == 0, "sticky");
    CHECK(done(l, "b-1") == 0 && done(l, "a-3") == 0 && empty(l), "sticky");
    etude_ledger_close(l);
}

/*
 * A worker of another process, which opens the ledger `dir` itself, runs
 * `fn` with phase 0, says so on `ready`, waits for a byte on `go`, runs `fn`
 * with phase 1, and exits 0 when both returned 0.
 */
struct other {
    const char *dir;
    int ready, go; /* this process's ends of the two pipes */
    int (*fn)(struct etude_ledger *l, int phase);
};

/** Start the child that `o` describes; its PID, or -1. */
static pid_t other_start(struct other *o)
{
    int to[2], from[2];
    pid_t pid;
    char c = 0;

    if (pipe(to) != 0 || pipe(from) != 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        struct etude_ledger *l = etude_ledger_open(o->dir);
        int s;

        close(to[1]);
        close(from[0]);
        s = l == NULL || o->fn(l, 0) != 0;
        if (write(from[1], "r", 1) != 1 || (!s && read(to[0], &c, 1) != 1))
            _exit(3);
        if (!s && o->fn(l, 1) != 0)
            s = 1;
        _exit(s);
    }
    close(to[0]);
    close(from[1]);
    o->ready = from[0];
    o->go = to[1];
    if (read(o->ready, &c, 1) != 1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

/** Let the child of `o` go on, reap it, and give its exit status. */
static int other_end(struct other *o, pid_t pid)
{
    int status = 0;

    if (write(o->go, "g", 1) != 1)
        kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(o->ready);
    close(o->go);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/** The child of the dead row: a request that waits, and an end. */
static int orphan(struct etude_ledger *l, int phase)
{
    if (phase == 0)
        return refused(ask(l, "orphan", "z", 2000, 0, 1), EAGAIN) &&
                       state_is(l, "orphan", "blocked")
                   ? 0
                   : 1;
    return 0;
}

/** A protected request whose worker died is removed, and the next chosen. */
static void test_dead(void)
{
    const char *dir = fix_path("dead");
    struct etude_ledger *l = etude_ledger_open(dir);
    struct other o = {dir, -1, -1, orphan};
    pid_t pid;

    if (l == NULL || ask(l, "hold", "a", 4000, 0, 1) != 0 || (pid = other_start(&o)) < 0) {
        CHECK(0, "dead");
        etude_ledger_close(l);
        return;
    }
    CHECK(other_end(&o, pid) == 0, "dead");
    CHECK(state_is(l, "orphan", ""), "dead");
    CHECK(refused(ask(l, "next", "b", 2000, 0, 1), EAGAIN) && state_is(l, "next", "blocked"),
          "dead");
    CHECK(done(l, "hold") == 0 && ask(l, "next", "b", 2000, 0, 1) == 0, "dead");
    CHECK(done(l, "next") == 0 && empty(l), "dead");
    etude_ledger_close(l);
}

/** The child of the uncertain row: a claim, then its cleanup unconfirmed. */
static int lost(struct etude_ledger *l, int phase)
{
    if (phase == 0)
        return ask(l, "lost", "z", 3000, 0, 1);
    return etude_ledger_finish(l, "lost", ETUDE_CLEANUP_UNCONFIRMED);
}

/**
 * A claim left uncertain by a worker that has died holds its resources
 * until a person drops it: the protected request that cannot fit beside it
 * is no longer protected, and is refused when it asks again, while the rest
 * go on; a request larger than the machine is refused.
 */
static void test_uncertain(void)
{
    const char *dir = fix_path("uncertain");
    struct etude_ledger *l = etude_ledger_open(dir);
    struct other o = {dir, -1, -1, lost};
    pid_t pid;

    if (l == NULL || (pid = other_start(&o)) < 0) {
        CHECK(0, "uncertain");
        etude_ledger_close(l);
        return;
    }
    CHECK(refused(ask(l, "wide", "a", 2000, 0, 1), EAGAIN) && state_is(l, "wide", "blocked"),
          "uncertain");
    CHECK(other_end(&o, pid) == 0 && state_is(l, "lost", "uncertain"), "uncertain");
    CHECK(ask(l, "narrow", "b", 1000, 0, 0) == 0 && state_is(l, "wide", "waiting"), "uncertain");
    CHECK(refused(ask(l, "wide", "a", 2000, 0, 1), ENOSPC) && state_is(l, "wide", ""), "uncertain");
    CHECK(refused(ask(l, "huge", "a", 5000, 0, 1), ERANGE) && state_is(l, "huge", ""), "uncertain");
    CHECK(done(l, "narrow") == 0 && etude_ledger_drop(l, "lost") == 0 && empty(l), "uncertain");
    etude_ledger_close(l);
}

/**
 * A worker's schedule keeps one request waiting: the first job refused for
 * now, which stays its request until it is admitted; a job too large for the
 * machine is refused and the rest go on; a job not yet prepared is not
 * asked for; and a worker that stops withdraws its request.
 */
static void test_sched(void)
{
    struct etude_ledger *l = etude_ledger_open(fix_path("sched"));
    struct need n1, n2, n3, n4, n5;
    struct etude_job jobs[] = {
        {"j1", "c1", "i686", 0, 1, 30, need(&n1, 2000, 0), 2, ETUDE_JOB_PENDING, 0},
        {"j2", "c2", "x86_64", 0, 1, 20, need(&n2, 1000, 0), 2, ETUDE_JOB_PENDING, 0},
        {"j3", "c3", "i686", 0, 0, 10, need(&n3, 500, 0), 2, ETUDE_JOB_PENDING, 0},
        {"j4", "c4", "x86_64", 0, 1, 40, need(&n4, 9000, 0), 2, ETUDE_JOB_PENDING, 0},
    };
    struct etude_job late = {"j5", "c5", "i686", 0, 1, 1, need(&n5, 4000, 0), 2, ETUDE_JOB_PENDING,
                             0};
    size_t i, got = 9, nj = sizeof(jobs) / sizeof(jobs[0]);

    if (l == NULL || ask(l, "hold", "x", 3000, 0, 1) != 0) {
        CHECK(0, "sched");
        etude_ledger_close(l);
        return;
    }
    etude_sched_order(jobs, nj);
    CHECK(strcmp(jobs[0].id, "j4") == 0 && strcmp(jobs[1].id, "j1") == 0, "sched");
    CHECK(etude_sched_admit(l, "w", jobs, nj, CAP, NCAP, &got) == 0 && got == 0, "sched");
    CHECK(jobs[0].state == ETUDE_JOB_REFUSED && jobs[0].error == ERANGE, "sched");
    CHECK(jobs[1].state == ETUDE_JOB_WAITING && state_is(l, "j1", "blocked"), "sched");
    CHECK(jobs[2].state == ETUDE_JOB_PENDING && state_is(l, "j2", ""), "sched");
    CHECK(jobs[3].state == ETUDE_JOB_PENDING && state_is(l, "j3", ""), "sched");
    CHECK(etude_sched_admit(l, "w", jobs, nj, CAP, NCAP, &got) == 0 && got == 0, "sched");
    CHECK(jobs[1].state == ETUDE_JOB_WAITING && state_is(l, "j2", ""), "sched");
    CHECK(done(l, "hold") == 0, "sched");
    CHECK(etude_sched_admit(l, "w", jobs, nj, CAP, NCAP, &got) == 0 && got == 2, "sched");
    CHECK(jobs[1].state == ETUDE_JOB_ADMITTED && jobs[2].state == ETUDE_JOB_ADMITTED, "sched");
    jobs[3].ready = 1;
    CHECK(etude_sched_admit(l, "w", jobs, nj, CAP, NCAP, &got) == 0 && got == 1, "sched");
    CHECK(jobs[3].state == ETUDE_JOB_ADMITTED && state_is(l, "j3", "reserved"), "sched");
    CHECK(etude_sched_admit(l, "w", &late, 1, CAP, NCAP, &got) == 0 && got == 0, "sched");
    CHECK(late.state == ETUDE_JOB_WAITING && state_is(l, "j5", "blocked"), "sched");
    CHECK(etude_sched_withdraw(l, &late, 1) == 0 && late.state == ETUDE_JOB_PENDING, "sched");
    CHECK(state_is(l, "j5", ""), "sched");
    for (i = 1; i < nj; i++)
        CHECK(done(l, jobs[i].id) == 0, "sched");
    CHECK(empty(l), "sched");
    etude_ledger_close(l);
}

/* ---- Workers under contention. ---- */

#define WORKERS     6
#define JOBS        8
#define ACCOUNTS    3
#define USAGE_BYTES (2 * sizeof(uint64_t))

/**
 * Add `sign` times the resources of job `j` to what the jobs say they hold,
 * kept in file `usage` under its lock, and say whether the sum stays within
 * the machine.
 */
static int usage_add(const char *usage, const struct etude_job *j, int sign)
{
    uint64_t held[2] = {0, 0};
    FILE *f = fopen(usage, "r+");
    int ok = 1;
    size_t i;

    if (f == NULL || flock(fileno(f), LOCK_EX) != 0) {
        if (f != NULL)
            fclose(f);
        return 0;
    }
    if (fread(held, 1, USAGE_BYTES, f) != USAGE_BYTES)
        held[0] = held[1] = 0;
    for (i = 0; i < 2; i++)
        held[i] = sign > 0 ? held[i] + j->res[i].amount : held[i] - j->res[i].amount;
    ok = held[0] <= CAP[0].amount && held[1] <= CAP[1].amount;
    rewind(f);
    if (fwrite(held, 1, USAGE_BYTES, f) != USAGE_BYTES || fflush(f) != 0)
        ok = 0;
    fclose(f);
    return ok;
}

/** Wait half a millisecond: a job's work, or a pause before asking again. */
static void pause_briefly(void)
{
    struct timespec ts = {0, 500000};

    /* interval: a job holds its claims so the workers' claims overlap */
    nanosleep(&ts, NULL);
}

/**
 * Be worker `w` of the ledger in `dir`: run its jobs through the schedule,
 * each between an addition to `usage` and a subtraction, and check the
 * ledger after each admission. Exit 0 when every job ran once and one of
 * them waited for another worker's, 1 when every job ran once and none
 * waited, 2 when either reading found more taken than the machine has, and
 * 3 for an error.
 */
static void worker(const char *dir, const char *usage, int w)
{
    static const uint64_t cpus[] = {500, 1000, 1500, 2000, 4000, 1000, 2500, 500};
    static const uint64_t mems[] = {1024, 4096, 512, 8192, 2048, 0, 3072, 6144};
    struct etude_ledger *l = etude_ledger_open(dir);
    struct etude_job jobs[JOBS];
    struct need needs[JOBS];
    char ids[JOBS][32], account[16];
    int ran[JOBS], left = JOBS, bad = 0, waited = 0, k;
    size_t got;
    struct look look_now;

    if (l == NULL)
        _exit(3);
    snprintf(account, sizeof(account), "acct%d", w % ACCOUNTS);
    for (k = 0; k < JOBS; k++) {
        int s = (k + w) % JOBS;

        snprintf(ids[k], sizeof(ids[k]), "w%d-j%d", w, k);
        jobs[k].id = ids[k];
        jobs[k].case_id = ids[k];
        jobs[k].arch = k % 2 ? "x86_64" : "i686";
        jobs[k].prep = (unsigned)(k / 4);
        jobs[k].ready = 1;
        jobs[k].cost_us = cpus[s];
        jobs[k].res = need(&needs[k], cpus[s], mems[s]);
        jobs[k].nres = 2;
        jobs[k].state = ETUDE_JOB_PENDING;
        jobs[k].error = 0;
        ran[k] = 0;
    }
    etude_sched_order(jobs, JOBS);
    while (left > 0) {
        if (etude_sched_admit(l, account, jobs, JOBS, CAP, NCAP, &got) != 0)
            _exit(3);
        if (got > 0 && (look(l, NULL, &look_now) != 0 || look_now.cpu > CAP[0].amount ||
                        look_now.mem > CAP[1].amount))
            bad = 1;
        for (k = 0; k < JOBS; k++) {
            if (jobs[k].state == ETUDE_JOB_WAITING)
                waited = 1;
            if (jobs[k].state == ETUDE_JOB_ADMITTED && !ran[k] && !usage_add(usage, &jobs[k], 1))
                bad = 1;
        }
        pause_briefly();
        for (k = 0; k < JOBS; k++) {
            if (jobs[k].state == ETUDE_JOB_REFUSED)
                _exit(3);
            if (jobs[k].state != ETUDE_JOB_ADMITTED || ran[k])
                continue;
            if (!usage_add(usage, &jobs[k], -1))
                bad = 1;
            if (done(l, jobs[k].id) != 0)
                _exit(3);
            ran[k] = 1;
            left--;
        }
    }
    etude_ledger_close(l);
    _exit(bad ? 2 : waited ? 0 : 1);
}

/**
 * Six workers of three accounts, each with eight jobs of both architectures,
 * one of which takes every processor and one all the memory: every job runs
 * once, the machine is never over its limits, a job of some worker waited
 * for another's, and the ledger ends empty.
 */
static void test_contention(void)
{
    const char *dir = fix_path("contention"), *usage = fix_path("usage");
    struct etude_ledger *l;
    pid_t pids[WORKERS];
    int i, status, ok = 1, waited = 0;
    FILE *f = fopen(usage, "w");

    CHECK(f != NULL && fclose(f) == 0, "contention");
    for (i = 0; i < WORKERS; i++) {
        pids[i] = fork();
        if (pids[i] == 0)
            worker(dir, usage, i);
        CHECK(pids[i] > 0, "contention");
    }
    for (i = 0; i < WORKERS; i++) {
        if (pids[i] <= 0)
            continue;
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR) {
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            waited = 1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) > 1) {
            printf("etude-sched-test: worker %d: status %d\n", i,
                   WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
            ok = 0;
        }
    }
    CHECK(ok && waited, "contention");
    l = etude_ledger_open(dir);
    CHECK(l != NULL && empty(l), "contention");
    etude_ledger_close(l);
}

int main(void)
{
    parent = getpid();
    signal(SIGALRM, stopped);
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    alarm(120);
    test_cpu();
    test_cost();
    test_order();
    test_backfill();
    test_beside();
    test_fair();
    test_dead();
    test_uncertain();
    test_sched();
    test_contention();
    alarm(0);
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_sched_hosttest: pass\n");
    return 0;
}
