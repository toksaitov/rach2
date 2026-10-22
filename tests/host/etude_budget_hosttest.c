/**
 * Check etude's budget (tools/etude/budget.c): its policy, the digests by
 * which a plan's item names its case, the schedule a forecast assumes, the
 * forecast of a selection's charge from recorded costs, and the check of a
 * run's budget from its records.
 *
 * host-deps: tools/etude/budget.c tools/etude/budget_run.c tools/etude/digest.c
 * host-deps: tools/etude/cost.c tools/etude/sched.c tools/etude/ledger.c
 * host-deps: tools/etude/posix/process.c tools/etude/compare.c
 * host-deps: tools/etude/compare_report.c tools/etude/history.c
 * host-deps: tools/etude/history_json.c tools/etude/history_collect.c
 * host-deps: tools/etude/json.c tools/etude/schema.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 *
 * A policy whose limits do not nest is refused. The schedule keeps a job
 * from passing a costlier one that waits. A forecast charges every
 * repetition of every selected case and every node its cases prepare, at
 * the costs recorded on the policy's site; a case or a node with no cost
 * recorded there is unknown, and costs recorded past the deadline fail,
 * even beside unknown ones. A run's check leaves unknown a charge that was
 * not observed and work that was not done; it fails attempts taken from
 * another run, preparation another run did or the run's record leaves out,
 * an exemption that does not hold, and limits that are not the policy's;
 * it charges work outside the run's interval, and one-time setup that was
 * not the first. An unevaluated budget accepts the computed verdict,
 * while a different charge, setup time or evaluated verdict is told.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "../../utils/sha256.h"
#include "budget.h"
#include "digest.h"
#include "etude/history.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-budget-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

#define SECOND  INT64_C(1000000)
#define SCHEMAS "tools/etude/schemas"
#define HISTORY SCHEMAS "/history.schema.json"
#define CASES   SCHEMAS "/case.host.example.json"
#define NODE    "build/tests/host/sha256_hosttest"

/* The process that runs the rows, which alone removes the fixtures. */
static pid_t parent;

/* The host example's manifest, with three repetitions of its case. */
static struct json *manifest;
static const struct json *the_case;

/* The policy of the rows that judge runs. */
static struct etude_budget_policy policy;

/** Leave through the fixtures' cleanup after a signal, in the parent. */
static void stopped(int sig)
{
    if (getpid() != parent) {
        _exit(128 + sig);
    }
    printf("etude-budget-test: fail: stopped by signal %d\n", sig);
    exit(1);
}

/** Say whether report `r` has a line that contains `text`. */
static int says(const struct etude_budget_report *r, const char *text)
{
    return r->lines.data != NULL && strstr(r->lines.data, text) != NULL;
}

/** Write `text` to the fixture `name`; give its path, or NULL. */
static const char *put(const char *name, const char *text)
{
    const char *path = fix_path(name);
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return NULL;
    }
    if (fputs(text, f) == EOF) {
        fclose(f);
        return NULL;
    }
    return fclose(f) == 0 ? path : NULL;
}

/* ---- The policy. ---- */

/* The parts of a policy document a row changes. */
struct policy_spec {
    uint64_t cpu_millis, memory_bytes;
    int64_t ci_goal, ci_growth, ci_ceiling, ci_reserve; /* seconds */
    int no_focused;                                     /* leave the class out */
};

/* The policy of most rows: a machine of 16 processors, and ci's limits. */
static const struct policy_spec usual = {16000, 1073741824, 600, 0, 600, 60, 0};

/** Write the policy of `s` to fixture `name`; give its path, or NULL. */
static const char *write_policy(const char *name, const struct policy_spec *s)
{
    static const char release[] =
        "{\"goal_us\":3600000000,\"growth_us\":3600000000,\"ceiling_us\":14400000000,"
        "\"cleanup_reserve_us\":600000000}";
    char text[4096];

    fix_format(text, sizeof(text),
               "{\"schema_version\":1,\"policy\":\"budget-v1\",\"site\":{\"machine_id\":\"m1\","
               "\"os\":\"linux\",\"accelerator\":\"kvm\",\"capacity\":{\"cpu_millis\":%llu,"
               "\"memory_bytes\":%llu,\"disk_bytes\":1073741824}},\"classes\":{\"push\":"
               "{\"goal_us\":300000000,\"growth_us\":0,\"ceiling_us\":300000000,"
               "\"cleanup_reserve_us\":30000000},\"ci\":{\"goal_us\":%lld,\"growth_us\":%lld,"
               "\"ceiling_us\":%lld,\"cleanup_reserve_us\":%lld},\"release\":%s,%s%s%s"
               "\"phase_first_run\":%s},\"one_time_setup\":[\"setup.toolchain\"]}\n",
               (unsigned long long)s->cpu_millis, (unsigned long long)s->memory_bytes,
               (long long)(s->ci_goal * SECOND), (long long)(s->ci_growth * SECOND),
               (long long)(s->ci_ceiling * SECOND), (long long)(s->ci_reserve * SECOND), release,
               s->no_focused ? "" : "\"focused\":", s->no_focused ? "" : release,
               s->no_focused ? "" : ",", release);
    return put(name, text);
}

/**
 * Read policy `s` into `p` from fixture `name`; give the result of
 * etude_budget_policy_read.
 */
static int read_policy(const char *name, const struct policy_spec *s, struct etude_budget_policy *p,
                       char *err, size_t errlen)
{
    const char *path = write_policy(name, s);

    if (path == NULL) {
        snprintf(err, errlen, "%s: cannot write", name);
        memset(p, 0, sizeof(*p));
        return -1;
    }
    return etude_budget_policy_read(SCHEMAS, path, p, err, errlen);
}

/** Give into `hex` the SHA-256 of the file at `path`; 0 or -1. */
static int file_digest(const char *path, char hex[65])
{
    char *bytes = NULL;
    size_t length = 0;
    struct sha256 sha;
    unsigned char digest[32];

    if (cmp_read_file(path, 1 << 20, &bytes, &length) != 0) {
        return -1;
    }
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, length);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    free(bytes);
    return 0;
}

/**
 * A policy is read whole and checked: its digest is its file's, its classes
 * and capacity are taken, and limits that do not nest, or a class left out,
 * are refused.
 */
static void test_policy(void)
{
    struct policy_spec s = usual;
    struct etude_budget_policy p;
    char err[1200], hex[65];
    int rc;

    CHECK(read_policy("policy-good.json", &s, &p, err, sizeof(err)) == 0, "policy");
    CHECK(file_digest(fix_path("policy-good.json"), hex) == 0 && strcmp(hex, p.sha256) == 0,
          "policy");
    CHECK(p.ncapacity == 3 && p.capacity[0].amount == 16000, "policy");
    CHECK(p.limits[ETUDE_BUDGET_CI].ceiling_us == 600 * (uint64_t)SECOND &&
              p.limits[ETUDE_BUDGET_CI].reserve_us == 60 * (uint64_t)SECOND,
          "policy");
    CHECK(p.one_time != NULL && p.one_time->count == 1, "policy");
    etude_budget_policy_free(&p);
    rc = etude_budget_policy_read(SCHEMAS, SCHEMAS "/budget.example.json", &p, err, sizeof(err));
    CHECK(rc == 0, "policy");
    etude_budget_policy_free(&p);
    s.ci_goal = 700;
    CHECK(read_policy("policy-goal.json", &s, &p, err, sizeof(err)) != 0 &&
              strstr(err, "do not nest") != NULL,
          "policy");
    s = usual;
    s.ci_goal = 400;
    s.ci_growth = 300;
    CHECK(read_policy("policy-growth.json", &s, &p, err, sizeof(err)) != 0, "policy");
    s = usual;
    s.ci_reserve = 600;
    CHECK(read_policy("policy-reserve.json", &s, &p, err, sizeof(err)) != 0, "policy");
    s = usual;
    s.no_focused = 1;
    CHECK(read_policy("policy-class.json", &s, &p, err, sizeof(err)) != 0 &&
              strstr(err, "focused") != NULL,
          "policy");
}

/* ---- The digests. ---- */

/**
 * Each digest is the same for the same case, and changes with what it
 * covers: the workload with its architecture and repetitions, the profile
 * with its placement; the inventory does not change with the workload.
 */
static void test_digests(void)
{
    struct json *workload = (struct json *)json_member(the_case, "workload");
    struct json *reps = (struct json *)json_member(workload, "repetitions");
    char a[65], b[65], c[65];
    long long kept = reps->integer;

    workload_digest(the_case, "none", a);
    workload_digest(the_case, "none", b);
    CHECK(strcmp(a, b) == 0 && strlen(a) == 64, "digests");
    workload_digest(the_case, "x86_64", b);
    CHECK(strcmp(a, b) != 0, "digests");
    inventory_digest(the_case, c);
    reps->integer = kept + 1;
    workload_digest(the_case, "none", b);
    CHECK(strcmp(a, b) != 0, "digests");
    inventory_digest(the_case, b);
    CHECK(strcmp(c, b) == 0, "digests");
    reps->integer = kept;
    profile_digest(the_case, "host", a);
    profile_digest(the_case, "qemu", b);
    CHECK(strcmp(a, b) != 0, "digests");
}

/* A machine case with one service, whose work is `%s`. */
#define SERVICE_CASE                                                                        \
    "{\"case_id\":\"machine.net.echo\",\"workload\":{\"repetitions\":1},"                   \
    "\"execution\":{\"program\":\"echoscenario\",\"role\":\"ordinary\"},"                   \
    "\"steps\":[{\"step_id\":\"boot\"}],\"machines\":[{\"machine_id\":\"m\"}],"             \
    "\"services\":[{\"service_id\":\"echo\",\"program\":\"echod\",\"ready\":\"listening\"," \
    "\"does\":\"%s\"}]}"

/** Parse the service case whose service does `does` into `*c`; 0 or -1. */
static int service_case(const char *does, struct json **c)
{
    char text[600], why[200];

    snprintf(text, sizeof(text), SERVICE_CASE, does);
    return json_parse(text, strlen(text), 1, c, why, sizeof(why));
}

/**
 * A change of a case's services changes its workload digest, and leaves its
 * profile and inventory digests as they were.
 */
static void test_services(void)
{
    struct json *one = NULL, *other = NULL;
    char a[65], b[65];

    CHECK(service_case("echoes each line", &one) == 0 &&
              service_case("echoes each line twice", &other) == 0,
          "services");
    if (one != NULL && other != NULL) {
        workload_digest(one, "i686", a);
        workload_digest(other, "i686", b);
        CHECK(strcmp(a, b) != 0, "services");
        profile_digest(one, "qemu", a);
        profile_digest(other, "qemu", b);
        CHECK(strcmp(a, b) == 0, "services");
        inventory_digest(one, a);
        inventory_digest(other, b);
        CHECK(strcmp(a, b) == 0, "services");
    }
    json_free(one);
    json_free(other);
}

/* ---- The schedule. ---- */

/** Fill job `j` with `cost` seconds and `cpu` thousandths of a processor. */
static void job(struct etude_job *j, struct etude_ledger_res *res, const char *id, int64_t cost,
                uint64_t cpu)
{
    memset(j, 0, sizeof(*j));
    j->id = id;
    j->case_id = id;
    j->arch = "none";
    j->ready = 1;
    j->cost_us = (uint64_t)(cost * SECOND);
    res->name = "cpu_millis";
    res->amount = cpu;
    j->res = res;
    j->nres = 1;
}

/**
 * Jobs that take the machine run one after another, jobs that fit run at
 * once, a job does not pass a costlier one that waits for room, and a job
 * the machine cannot hold is refused.
 */
static void test_schedule(void)
{
    static const struct etude_ledger_res cap[] = {{"cpu_millis", 4000}};
    struct etude_ledger_res res[4];
    struct etude_job jobs[4];
    uint64_t span;

    job(&jobs[0], &res[0], "a", 10, 4000);
    job(&jobs[1], &res[1], "b", 20, 4000);
    job(&jobs[2], &res[2], "c", 30, 4000);
    CHECK(etude_budget_schedule(jobs, 3, cap, 1, &span) == 0 && span == 60 * (uint64_t)SECOND,
          "schedule");
    job(&jobs[0], &res[0], "a", 10, 1000);
    job(&jobs[1], &res[1], "b", 20, 1000);
    job(&jobs[2], &res[2], "c", 30, 1000);
    CHECK(etude_budget_schedule(jobs, 3, cap, 1, &span) == 0 && span == 30 * (uint64_t)SECOND,
          "schedule");
    job(&jobs[0], &res[0], "x", 10, 2000);
    job(&jobs[1], &res[1], "y", 9, 4000);
    job(&jobs[2], &res[2], "z", 8, 2000);
    CHECK(etude_budget_schedule(jobs, 3, cap, 1, &span) == 0 && span == 27 * (uint64_t)SECOND,
          "schedule");
    job(&jobs[0], &res[0], "a", 10, 1000);
    job(&jobs[1], &res[1], "huge", 50, 5000);
    CHECK(etude_budget_schedule(jobs, 2, cap, 1, &span) == 0 && span == 10 * (uint64_t)SECOND,
          "schedule");
    CHECK((jobs[0].state == ETUDE_JOB_REFUSED && jobs[0].error == ERANGE) ||
              (jobs[1].state == ETUDE_JOB_REFUSED && jobs[1].error == ERANGE),
          "schedule");
}

/* ---- The records of runs. ---- */

/* A step of a run, its times in seconds on the run's clock. */
struct step {
    const char *step_id, *stage;
    int64_t start, end;
    int unlisted;    /* the run's record leaves it out of its preparation */
    int other_clock; /* timed on another boot's clock */
};

/* A run to write, each item a repetition of the host example's case. */
struct run {
    const char *id, *purpose, *accelerator;
    const char *utc;       /* the time its records were recorded */
    int items, summarized; /* the plan's items, and those its summary lists */
    int64_t attempt_s;     /* each attempt's time; they run in turn from 5 s */
    int64_t start, end;    /* the run's interval; end -1 when not observed */
    struct step steps[3];
    size_t nsteps;
    const char *verdict;       /* the budget it records */
    int64_t charged, once;     /* the charge and one-time setup it records; -1 for none */
    const char *reuse;         /* an attempt record of another run, given for item 0 */
    const char *foreign;       /* a step record of another run, listed as preparation */
    int exemption;             /* 1 lists the run and its attempts, 2 leaves out one */
    int64_t ceiling;           /* the ceiling it records, in seconds; 0 for the policy's */
    const char *policy_sha256; /* the policy it names; NULL for the one read */
};

/** Write into `out` a blob reference whose digest is `hex`. */
static void blob_of(char *out, size_t size, const char *hex)
{
    fix_format(out, size, "{\"sha256\":\"%s\",\"size_bytes\":64,\"locations\":[\"test://blob\"]}",
               hex);
}

/** Write into `out` a measured value of `us` microseconds. */
static void measure(char *out, size_t size, int64_t us)
{
    fix_format(out, size,
               "{\"value\":%lld,\"method\":\"monotonic-v1\",\"quality\":\"exact\","
               "\"interval_us\":null,\"error_bound\":1}",
               (long long)us);
}

/** Write into `out` an interval of `start` to `end` s on `boot`'s clock. */
static void interval(char *out, size_t size, const char *utc, const char *boot, int64_t start,
                     int64_t end)
{
    char elapsed[300];

    if (end < 0) {
        fix_format(out, size,
                   "{\"start_utc\":\"%s\",\"end_utc\":null,\"clock\":{\"machine_id\":\"m1\","
                   "\"boot_id\":\"%s\",\"clock_id\":\"monotonic\",\"resolution_us\":1},"
                   "\"start_us\":%lld,\"end_us\":null,\"elapsed_us\":{\"value\":null,"
                   "\"reason\":\"the end was not observed\"}}",
                   utc, boot, (long long)(start * SECOND));
        return;
    }
    measure(elapsed, sizeof(elapsed), (end - start) * SECOND);
    fix_format(out, size,
               "{\"start_utc\":\"%s\",\"end_utc\":\"%s\",\"clock\":{\"machine_id\":\"m1\","
               "\"boot_id\":\"%s\",\"clock_id\":\"monotonic\",\"resolution_us\":1},"
               "\"start_us\":%lld,\"end_us\":%lld,\"elapsed_us\":%s}",
               utc, utc, boot, (long long)(start * SECOND), (long long)(end * SECOND), elapsed);
}

/* The resources, the result of a pass, and a zero measured, of every record. */
#define RESOURCES                                                                                  \
    "{\"scope\":{\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"," \
    "\"size_bytes\":1,\"locations\":[\"test://scope\"]},\"tree_cpu_us\":{\"value\":null,"          \
    "\"reason\":\"not measured\"},\"combined_peak_bytes\":{\"value\":null,\"reason\":"             \
    "\"not sampled\"},\"samples\":[]}"
#define PASS                                                                           \
    "{\"outcome\":\"pass\",\"reason\":null,\"cleanup\":\"confirmed\",\"evidence\":[]," \
    "\"capture_errors\":[],\"disruptions\":[]}"
#define ZERO                                                                               \
    "{\"value\":0,\"method\":\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null," \
    "\"error_bound\":1}"

/* A stream being written, and the text of its next record. */
struct writer {
    struct etude_history *h;
    const struct run *r;
    char data[32768], id[300];
};

/** Append `w->data` as a record of `kind`, its identity in `w->id`; 0 or -1. */
static int append(struct writer *w, const char *kind)
{
    if (etude_history_record_id(w->h, w->id, sizeof(w->id)) != 0 ||
        etude_history_append(w->h, w->id, w->r->utc, kind, w->data) != 0) {
        printf("etude-budget-test: the history refused a %s record: %s\n", kind, w->data);
        return -1;
    }
    return 0;
}

/** Write into `out` the plan's items, each a repetition of the case. */
static void plan_items(const struct run *r, char *out, size_t size)
{
    char digest[3][65], blobs[3][200];
    size_t used = 0;
    int k;

    workload_digest(the_case, "none", digest[0]);
    inventory_digest(the_case, digest[1]);
    profile_digest(the_case, "host", digest[2]);
    for (k = 0; k < 3; k++) {
        blob_of(blobs[k], sizeof(blobs[k]), digest[k]);
    }
    for (k = 0; k < r->items; k++) {
        fix_format(out + used, size - used,
                   "%s{\"item_id\":\"%s-i%d\",\"case_id\":\"host.sha256.vectors\","
                   "\"family_id\":\"host.sha256\",\"architecture\":\"none\",\"domain\":\"host\","
                   "\"workload\":%s,\"assertions\":%s,\"profile\":%s,\"repetition\":%d,"
                   "\"attempt_ids\":[\"%s-a%d\"]}",
                   k ? "," : "", r->id, k, blobs[0], blobs[1], blobs[2], k, r->id, k);
        used += strlen(out + used);
    }
}

/**
 * Write into `out` the exemption of a first run, or null: one that lists the
 * run and each of its attempts, or, for `r->exemption` 2, an attempt of no
 * plan in place of the first.
 */
static void exemption(const struct run *r, const char *selection, char *out, size_t size)
{
    char ids[2000];
    size_t used;
    int k;

    if (r->exemption == 0) {
        fix_format(out, size, "null");
        return;
    }
    fix_format(ids, sizeof(ids), "\"unplanned-a\"");
    used = r->exemption == 2 ? strlen(ids) : 0;
    for (k = r->exemption == 2 ? 1 : 0; k < r->items; k++) {
        fix_format(ids + used, sizeof(ids) - used, "%s\"%s-a%d\"", used ? "," : "", r->id, k);
        used += strlen(ids + used);
    }
    fix_format(out, size,
               "{\"reason\":\"milestone-62-first-runs\",\"phase_issue\":589,\"manifest\":%s,"
               "\"run_ids\":[\"%s\"],\"attempt_ids\":[%s]}",
               selection, r->id, ids);
}

/** Append the plan of run `w->r`; 0 or -1. */
static int write_plan(struct writer *w)
{
    static char items[16384];
    const struct run *r = w->r;
    char b[200], env[2400], ex[3000];

    blob_of(b, sizeof(b), "1111111111111111111111111111111111111111111111111111111111111111");
    plan_items(r, items, sizeof(items));
    exemption(r, b, ex, sizeof(ex));
    fix_format(env, sizeof(env),
               "{\"machine_id\":\"m1\",\"boot_id\":\"%s-boot\",\"os\":\"linux\","
               "\"os_version\":\"26.04\",\"accelerator\":\"%s\",\"site_profile\":%s,\"tools\":%s,"
               "\"capacity\":%s,\"cache_state\":%s,\"load\":%s}",
               r->id, r->accelerator, b, b, b, b, b);
    fix_format(w->data, sizeof(w->data),
               "{\"purpose\":\"%s\",\"selection\":%s,\"items\":[%s],\"commit\":"
               "\"1111111111111111111111111111111111111111\",\"source\":%s,\"runner\":%s,"
               "\"artifacts\":[],\"environment\":%s,\"concurrency\":%s,\"policies\":%s,"
               "\"exemption\":%s,\"reproduce\":%s}",
               r->purpose, b, items, b, b, env, b, b, ex, b);
    return append(w, "plan");
}

/** Append the steps of run `w->r`, those it lists in `prep`; 0 or -1. */
static int write_steps(struct writer *w, const char *plan_id, char *prep, size_t size)
{
    const struct run *r = w->r;
    char time[1200], boot[300];
    size_t i, used = strlen(prep);

    for (i = 0; i < r->nsteps; i++) {
        const struct step *s = &r->steps[i];

        fix_format(boot, sizeof(boot), "%s-%s", r->id, s->other_clock ? "other" : "boot");
        interval(time, sizeof(time), r->utc, boot, s->start, s->end);
        fix_format(w->data, sizeof(w->data),
                   "{\"plan_id\":\"%s\",\"item_id\":null,\"attempt_id\":null,\"step_id\":\"%s\","
                   "\"stage\":\"%s\",\"time\":%s,\"resources\":%s,\"witnesses\":[],"
                   "\"max_progress_gap_us\":%s,\"result\":%s}",
                   plan_id, s->step_id, s->stage, time, RESOURCES, ZERO, PASS);
        if (append(w, "step") != 0) {
            return -1;
        }
        if (!s->unlisted) {
            fix_format(prep + used, size - used, "%s\"%s\"", used ? "," : "", w->id);
            used += strlen(prep + used);
        }
    }
    return 0;
}

/** Append the attempts of run `w->r`, their identities in `ids`; 0 or -1. */
static int write_attempts(struct writer *w, const char *plan_id, char ids[][300])
{
    const struct run *r = w->r;
    char time[1200], boot[300];
    int k;

    fix_format(boot, sizeof(boot), "%s-boot", r->id);
    for (k = 0; k < r->items; k++) {
        interval(time, sizeof(time), r->utc, boot, 5 + k * r->attempt_s,
                 5 + (k + 1) * r->attempt_s);
        fix_format(
            w->data, sizeof(w->data),
            "{\"plan_id\":\"%s\",\"item_id\":\"%s-i%d\",\"attempt_id\":\"%s-a%d\","
            "\"previous_attempt_id\":null,\"steps\":[],\"time\":%s,\"queue_us\":%s,"
            "\"resources\":%s,\"assertion_results\":%s,\"result\":%s}",
            plan_id, r->id, k, r->id, k, time, ZERO, RESOURCES,
            "{\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\","
            "\"size_bytes\":1,\"locations\":[\"test://results\"]}",
            PASS);
        if (append(w, "attempt") != 0) {
            return -1;
        }
        snprintf(ids[k], 300, "%s", w->id);
    }
    return 0;
}

/** Write into `out` the budget run `r` records. */
static void budget_of(const struct run *r, char *out, size_t size)
{
    const struct etude_budget_limits *l = &policy.limits[etude_budget_class_of(r->purpose)];
    char charged[300], once[300];

    if (r->charged < 0) {
        fix_format(charged, sizeof(charged), "{\"value\":null,\"reason\":\"the run did not end\"}");
    } else {
        measure(charged, sizeof(charged), r->charged * SECOND);
    }
    measure(once, sizeof(once), r->once * SECOND);
    fix_format(
        out, size,
        "{\"policy\":{\"sha256\":\"%s\",\"size_bytes\":%llu,\"locations\":"
        "[\"test://policy\"]},\"charged_us\":%s,\"outside_queue_us\":%s,"
        "\"one_time_setup_us\":%s,\"goal_us\":%llu,\"growth_us\":%llu,\"ceiling_us\":%llu,"
        "\"cleanup_reserve_us\":%llu,\"verdict\":\"%s\"}",
        r->policy_sha256 != NULL ? r->policy_sha256 : policy.sha256,
        (unsigned long long)policy.size_bytes, charged, ZERO, once, (unsigned long long)l->goal_us,
        (unsigned long long)l->growth_us,
        (unsigned long long)(r->ceiling > 0 ? (uint64_t)(r->ceiling * SECOND) : l->ceiling_us),
        (unsigned long long)l->reserve_us, r->verdict);
}

/** Append the run record of `w->r`; 0 or -1. */
static int write_summary(struct writer *w, const char *plan_id, char ids[][300], const char *prep)
{
    static char items[16384];
    const struct run *r = w->r;
    char time[1200], boot[300], budget[2000], b[200];
    size_t used = 0;
    int k;

    items[0] = 0;
    for (k = 0; k < r->summarized; k++) {
        fix_format(items + used, sizeof(items) - used,
                   "%s{\"item_id\":\"%s-i%d\",\"attempt_records\":[\"%s\"],\"outcome\":\"pass\"}",
                   k ? "," : "", r->id, k, k == 0 && r->reuse != NULL ? r->reuse : ids[k]);
        used += strlen(items + used);
    }
    fix_format(boot, sizeof(boot), "%s-boot", r->id);
    interval(time, sizeof(time), r->utc, boot, r->start, r->end);
    budget_of(r, budget, sizeof(budget));
    blob_of(b, sizeof(b), "3333333333333333333333333333333333333333333333333333333333333333");
    fix_format(w->data, sizeof(w->data),
               "{\"plan_id\":\"%s\",\"items\":[%s],\"preparation_steps\":[%s],\"time\":%s,"
               "\"resources\":%s,\"stage_totals\":%s,\"family_totals\":%s,"
               "\"useful_concurrency\":%s,\"load\":%s,\"budget\":%s,\"result\":%s}",
               plan_id, items, prep, time, RESOURCES, b, b, b, b, budget,
               r->end < 0 ? "{\"outcome\":\"incomplete\",\"reason\":\"the executor was lost\","
                            "\"cleanup\":\"unconfirmed\",\"evidence\":[],\"capture_errors\":[],"
                            "\"disruptions\":[]}"
                          : PASS);
    return append(w, "run");
}

/** Write run `r` to the fixture `RUN.jsonl`; give its path, or NULL. */
static const char *write_run(const struct run *r)
{
    static struct writer w;
    char name[64], stream[300], plan_id[300], prep[4000], ids[8][300];
    const char *path;
    int rc = -1;

    fix_format(name, sizeof(name), "%s.jsonl", r->id);
    fix_format(stream, sizeof(stream), "%s-s", r->id);
    path = fix_path(name);
    w.r = r;
    w.h = etude_history_open(path, HISTORY, stream, r->id, UINT64_C(1) << 22);
    if (w.h == NULL) {
        printf("etude-budget-test: %s: cannot open: %s\n", path, strerror(errno));
        return NULL;
    }
    prep[0] = 0;
    if (r->foreign != NULL) {
        fix_format(prep, sizeof(prep), "\"%s\"", r->foreign);
    }
    if (write_plan(&w) == 0) {
        snprintf(plan_id, sizeof(plan_id), "%s", w.id);
        if (write_steps(&w, plan_id, prep, sizeof(prep)) == 0 &&
            write_attempts(&w, plan_id, ids) == 0 && write_summary(&w, plan_id, ids, prep) == 0) {
            rc = 0;
        }
    }
    if (etude_history_close(w.h) != 0) {
        rc = -1;
    }
    return rc == 0 ? path : NULL;
}

/** Load the histories of runs `runs` into `h`; 0, or -1 with a failed check. */
static int load(const struct run *const *runs, size_t n, struct cmp_history *h, const char *row)
{
    const char *paths[16];
    size_t i;

    for (i = 0; i < n; i++) {
        paths[i] = write_run(runs[i]);
        if (paths[i] == NULL) {
            CHECK(0, row);
            return -1;
        }
    }
    if (cmp_load(h, HISTORY, (char **)paths, (int)n) != 0) {
        CHECK(0, row);
        return -1;
    }
    return 0;
}

/* A ci run of three items on the site, which prepares the node in 4 s and
 * spends 10 s on each attempt; its record agrees with its judgement. */
#define CI_RUN(name, utc)                                                             \
    {name, "ci",   "kvm", utc, 3,    3,    10, 0, 40,  {{NODE, "build", 0, 4, 0, 0}}, \
     1,    "pass", 40,    0,   NULL, NULL, 0,  0, NULL}

/* ---- The forecast. ---- */

/** Forecast class `cls` of the case on `p` from `h` into `f`; 0 or -1. */
static int forecast(const struct etude_budget_policy *p, enum etude_budget_class cls,
                    const struct cmp_history *h, struct etude_budget_forecast *f)
{
    return etude_budget_forecast(p, cls, h, &manifest, 1, f);
}

/**
 * A forecast charges each repetition at the median recorded on the site
 * and the node at its recorded preparation, on the site's capacity: all at
 * once on sixteen processors, one at a time on one.
 */
static void test_forecast(void)
{
    static const struct run a = CI_RUN("fa", "2026-10-01T01:00:00.000000Z");
    static const struct run b = CI_RUN("fb", "2026-10-01T02:00:00.000000Z");
    static const struct run c = {"fc",  "ci",
                                 "kvm", "2026-10-01T03:00:00.000000Z",
                                 3,     3,
                                 13,    0,
                                 50,    {{NODE, "build", 0, 5, 0, 0}},
                                 1,     "pass",
                                 50,    0,
                                 NULL,  NULL,
                                 0,     0,
                                 NULL};
    static const struct run *const runs[] = {&a, &b, &c};
    struct policy_spec one = usual;
    struct etude_budget_policy small;
    struct etude_budget_forecast f;
    struct cmp_history h;
    char err[1200];

    if (load(runs, 3, &h, "forecast") != 0) {
        return;
    }
    CHECK(forecast(&policy, ETUDE_BUDGET_CI, &h, &f) == 0, "forecast");
    CHECK(f.report.verdict == ETUDE_BUDGET_PASS && f.items == 3 && f.nodes == 1, "forecast");
    CHECK(f.prep_us == 4 * (uint64_t)SECOND && f.cases_us == 10 * (uint64_t)SECOND, "forecast");
    etude_budget_report_free(&f.report);
    one.cpu_millis = 1000;
    CHECK(read_policy("policy-one.json", &one, &small, err, sizeof(err)) == 0, "forecast");
    CHECK(forecast(&small, ETUDE_BUDGET_CI, &h, &f) == 0, "forecast");
    CHECK(f.report.verdict == ETUDE_BUDGET_PASS && f.cases_us == 30 * (uint64_t)SECOND, "forecast");
    etude_budget_report_free(&f.report);
    etude_budget_policy_free(&small);
    cmp_free(&h);
}

/**
 * A case with no cost recorded on the site, under its accelerator, is
 * unknown, and so is a node whose preparation no run of the class records;
 * neither is charged as nothing.
 */
static void test_unknown_cost(void)
{
    static const struct run tcg = {"ut",  "ci",
                                   "tcg", "2026-10-01T01:00:00.000000Z",
                                   3,     3,
                                   10,    0,
                                   40,    {{NODE, "build", 0, 4, 0, 0}},
                                   1,     "pass",
                                   40,    0,
                                   NULL,  NULL,
                                   0,     0,
                                   NULL};
    static const struct run bare = {"ub", "ci",  "kvm", "2026-10-01T01:00:00.000000Z",
                                    3,    3,     10,    0,
                                    40,   {{0}}, 0,     "pass",
                                    40,   0,     NULL,  NULL,
                                    0,    0,     NULL};
    static const struct run push = {"up",  "push",
                                    "kvm", "2026-10-01T02:00:00.000000Z",
                                    3,     3,
                                    10,    0,
                                    40,    {{NODE, "build", 0, 4, 0, 0}},
                                    1,     "pass",
                                    40,    0,
                                    NULL,  NULL,
                                    0,     0,
                                    NULL};
    static const struct run *const first[] = {&tcg};
    static const struct run *const second[] = {&bare, &push};
    struct etude_budget_forecast f;
    struct cmp_history h;

    if (load(first, 1, &h, "unknown-cost") != 0) {
        return;
    }
    CHECK(forecast(&policy, ETUDE_BUDGET_CI, &h, &f) == 0, "unknown-cost");
    CHECK(f.report.verdict == ETUDE_BUDGET_UNKNOWN && f.cases_us == 0 && f.prep_us == 0,
          "unknown-cost");
    CHECK(says(&f.report, "unknown: host.sha256.vectors on none has no recorded cost"),
          "unknown-cost");
    etude_budget_report_free(&f.report);
    cmp_free(&h);
    if (load(second, 2, &h, "unknown-node") != 0) {
        return;
    }
    CHECK(forecast(&policy, ETUDE_BUDGET_CI, &h, &f) == 0, "unknown-node");
    CHECK(f.report.verdict == ETUDE_BUDGET_UNKNOWN && f.prep_us == 0, "unknown-node");
    CHECK(f.cases_us == 10 * (uint64_t)SECOND, "unknown-node");
    CHECK(says(&f.report, "unknown: node " NODE " has no recorded cost in ci runs"),
          "unknown-node");
    etude_budget_report_free(&f.report);
    CHECK(forecast(&policy, ETUDE_BUDGET_PUSH, &h, &f) == 0, "unknown-node");
    CHECK(f.report.verdict == ETUDE_BUDGET_PASS && f.prep_us == 4 * (uint64_t)SECOND,
          "unknown-node");
    etude_budget_report_free(&f.report);
    cmp_free(&h);
}

/**
 * Costs recorded past the deadline fail the forecast, though a node's cost
 * is unknown beside them, and an item the machine cannot hold fails it.
 */
static void test_deadline(void)
{
    static const struct run bare = {"db", "ci",  "kvm", "2026-10-01T01:00:00.000000Z",
                                    3,    3,     10,    0,
                                    40,   {{0}}, 0,     "pass",
                                    40,   0,     NULL,  NULL,
                                    0,    0,     NULL};
    static const struct run *const runs[] = {&bare};
    struct policy_spec tight = usual;
    struct etude_budget_policy p;
    struct etude_budget_forecast f;
    struct cmp_history h;
    char err[1200];

    if (load(runs, 1, &h, "deadline") != 0) {
        return;
    }
    tight.cpu_millis = 1000;
    tight.ci_goal = 20;
    tight.ci_ceiling = 30;
    tight.ci_reserve = 5;
    tight.ci_growth = 10;
    CHECK(read_policy("policy-tight.json", &tight, &p, err, sizeof(err)) == 0, "deadline");
    CHECK(forecast(&p, ETUDE_BUDGET_CI, &h, &f) == 0, "deadline");
    CHECK(f.report.verdict == ETUDE_BUDGET_FAILURE, "deadline");
    CHECK(says(&f.report, "failure: the costs recorded, 30.000000 s, passes the deadline"),
          "deadline");
    CHECK(says(&f.report, "note: the costs recorded, 30.000000 s, uses 10.000000 s"), "deadline");
    etude_budget_report_free(&f.report);
    etude_budget_policy_free(&p);
    tight = usual;
    tight.memory_bytes = 1048576;
    CHECK(read_policy("policy-memory.json", &tight, &p, err, sizeof(err)) == 0, "deadline");
    CHECK(forecast(&p, ETUDE_BUDGET_CI, &h, &f) == 0, "deadline");
    CHECK(f.report.verdict == ETUDE_BUDGET_FAILURE && f.cases_us == 0, "deadline");
    CHECK(says(&f.report, "needs 16777216 memory_bytes, more than the 1048576 of m1"), "deadline");
    etude_budget_report_free(&f.report);
    etude_budget_policy_free(&p);
    cmp_free(&h);
}

/* ---- The check of a run. ---- */

/** Judge run `id` of `h`, with the case's manifest when `nodes`; 0 or -1. */
static int judge(const struct cmp_history *h, const char *id, int nodes,
                 struct etude_budget_judgement *j)
{
    return etude_budget_judge(h, &policy, id, &manifest, nodes ? 1 : 0, j);
}

/**
 * A run whose records account for its work and its preparation passes,
 * with the charge it records; one past the ceiling fails, and one at the
 * ceiling, within less than its error bound, is unknown.
 */
static void test_judge(void)
{
    static const struct run ok = CI_RUN("jok", "2026-10-01T01:00:00.000000Z");
    static const struct run over = {"jover", "ci",
                                    "kvm",   "2026-10-01T01:00:00.000000Z",
                                    3,       3,
                                    10,      0,
                                    700,     {{NODE, "build", 0, 4, 0, 0}},
                                    1,       "failure",
                                    700,     0,
                                    NULL,    NULL,
                                    0,       0,
                                    NULL};
    static const struct run edge = {"jedge", "ci",
                                    "kvm",   "2026-10-01T01:00:00.000000Z",
                                    3,       3,
                                    10,      0,
                                    600,     {{NODE, "build", 0, 4, 0, 0}},
                                    1,       "unknown",
                                    600,     0,
                                    NULL,    NULL,
                                    0,       0,
                                    NULL};
    static const struct run *const runs[] = {&ok, &over, &edge};
    struct etude_budget_judgement j;
    struct cmp_history h;

    if (load(runs, 3, &h, "judge") != 0) {
        return;
    }
    CHECK(judge(&h, "jok", 1, &j) == 0, "judge");
    CHECK(j.report.verdict == ETUDE_BUDGET_PASS && j.agrees && j.cls == ETUDE_BUDGET_CI, "judge");
    CHECK(j.charged_known && j.charged_us == 40 * (uint64_t)SECOND, "judge");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "jover", 0, &j) == 0, "ceiling");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE && j.agrees, "ceiling");
    CHECK(says(&j.report, "failure: the charge, 700.000000 s, passes the ceiling"), "ceiling");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "jedge", 0, &j) == 0, "ceiling");
    CHECK(j.report.verdict == ETUDE_BUDGET_UNKNOWN && j.agrees, "ceiling");
    CHECK(says(&j.report, "less than its error bound"), "ceiling");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "absent", 0, &j) != 0 && strstr(j.error, "no run record") != NULL, "judge");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
}

/**
 * A run whose end was not observed, an item left out of the summary, a run
 * on another site, or a history that could not be read whole leaves the
 * charge unknown, and a record that says pass then differs.
 */
static void test_unobserved(void)
{
    static const struct run open = {"jopen", "ci",
                                    "kvm",   "2026-10-01T01:00:00.000000Z",
                                    3,       3,
                                    10,      0,
                                    -1,      {{NODE, "build", 0, 4, 0, 0}},
                                    1,       "pass",
                                    -1,      0,
                                    NULL,    NULL,
                                    0,       0,
                                    NULL};
    static const struct run left = {"jleft", "ci",
                                    "kvm",   "2026-10-01T01:00:00.000000Z",
                                    3,       2,
                                    10,      0,
                                    40,      {{NODE, "build", 0, 4, 0, 0}},
                                    1,       "pass",
                                    40,      0,
                                    NULL,    NULL,
                                    0,       0,
                                    NULL};
    static const struct run away = {"jaway", "ci",
                                    "tcg",   "2026-10-01T01:00:00.000000Z",
                                    3,       3,
                                    10,      0,
                                    40,      {{NODE, "build", 0, 4, 0, 0}},
                                    1,       "pass",
                                    40,      0,
                                    NULL,    NULL,
                                    0,       0,
                                    NULL};
    static const struct run *const runs[] = {&open, &left, &away};
    struct etude_budget_judgement j;
    struct cmp_history h;
    const char *paths[2];

    if (load(runs, 3, &h, "unobserved") != 0) {
        return;
    }
    CHECK(judge(&h, "jopen", 0, &j) == 0, "unobserved");
    CHECK(j.report.verdict == ETUDE_BUDGET_UNKNOWN && !j.charged_known && !j.agrees, "unobserved");
    CHECK(says(&j.report, "unknown: the run's cleanup is unconfirmed"), "unobserved");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "jleft", 0, &j) == 0, "omitted");
    CHECK(j.report.verdict == ETUDE_BUDGET_UNKNOWN && !j.agrees, "omitted");
    CHECK(says(&j.report, "unknown: item jleft-i2 is planned but not in the run's summary"),
          "omitted");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "jaway", 0, &j) == 0, "site");
    CHECK(j.report.verdict == ETUDE_BUDGET_UNKNOWN, "site");
    CHECK(says(&j.report, "not on the policy's site"), "site");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
    paths[0] = fix_path("jleft.jsonl");
    paths[1] = put("damaged.jsonl", "{\"schema_version\":1,\"record_id\":\n");
    CHECK(paths[1] != NULL && cmp_load(&h, HISTORY, (char **)paths, 2) == 0, "unobserved");
    CHECK(judge(&h, "jleft", 0, &j) == 0 && says(&j.report, "unknown: a record may be missing"),
          "unobserved");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
}

/**
 * Attempts of another run do not count as this run's work: an exempt first
 * run's least of all. A first run's exemption holds only when it lists the
 * run and every attempt, and limits other than the policy's fail.
 */
static void test_reuse(void)
{
    static const struct run first = {"rfirst", "phase_first_run",
                                     "kvm",    "2026-10-01T01:00:00.000000Z",
                                     3,        3,
                                     10,       0,
                                     40,       {{NODE, "build", 0, 4, 0, 0}},
                                     1,        "exempt",
                                     40,       0,
                                     NULL,     NULL,
                                     1,        0,
                                     NULL};
    static const struct run loose = {"rloose", "phase_first_run",
                                     "kvm",    "2026-10-01T01:00:00.000000Z",
                                     3,        3,
                                     10,       0,
                                     40,       {{NODE, "build", 0, 4, 0, 0}},
                                     1,        "exempt",
                                     40,       0,
                                     NULL,     NULL,
                                     2,        0,
                                     NULL};
    static const struct run reuse = {"rreuse",
                                     "ci",
                                     "kvm",
                                     "2026-10-01T02:00:00.000000Z",
                                     3,
                                     3,
                                     10,
                                     0,
                                     40,
                                     {{NODE, "build", 0, 4, 0, 0}},
                                     1,
                                     "pass",
                                     40,
                                     0,
                                     "rfirst-s/2",
                                     NULL,
                                     0,
                                     0,
                                     NULL};
    static const struct run limits = {"rlimits", "ci",
                                      "kvm",     "2026-10-01T02:00:00.000000Z",
                                      3,         3,
                                      10,        0,
                                      40,        {{NODE, "build", 0, 4, 0, 0}},
                                      1,         "pass",
                                      40,        0,
                                      NULL,      NULL,
                                      0,         6000,
                                      NULL};
    static const struct run other = {
        "rother",
        "ci",
        "kvm",
        "2026-10-01T02:00:00.000000Z",
        3,
        3,
        10,
        0,
        40,
        {{NODE, "build", 0, 4, 0, 0}},
        1,
        "pass",
        40,
        0,
        NULL,
        NULL,
        0,
        0,
        "4444444444444444444444444444444444444444444444444444444444444444"};
    static const struct run *const runs[] = {&first, &loose, &reuse, &limits, &other};
    struct etude_budget_judgement j;
    struct cmp_history h;

    if (load(runs, 5, &h, "reuse") != 0) {
        return;
    }
    CHECK(judge(&h, "rfirst", 0, &j) == 0, "exempt");
    CHECK(j.report.verdict == ETUDE_BUDGET_EXEMPT && j.agrees, "exempt");
    CHECK(j.charged_known && j.charged_us == 40 * (uint64_t)SECOND, "exempt");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "rloose", 0, &j) == 0, "exempt");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE, "exempt");
    CHECK(says(&j.report, "failure: the exemption does not list attempt rloose-a0"), "exempt");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "rreuse", 0, &j) == 0, "reuse");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE, "reuse");
    CHECK(says(&j.report, "reuses attempt record rfirst-s/2 of run rfirst, an exempt first run"),
          "reuse");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "rlimits", 0, &j) == 0, "policy-record");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE, "policy-record");
    CHECK(says(&j.report, "records limits other than the policy's for ci"), "policy-record");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "rother", 0, &j) == 0, "policy-record");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE, "policy-record");
    CHECK(says(&j.report, "names the policy 4444"), "policy-record");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
}

/**
 * Preparation is charged, never hidden: a shared step the run's record
 * leaves out, one another run did, and a node of the case with no step all
 * fail; a step before the run's interval extends the charge, and one on
 * another clock leaves it unknown.
 */
static void test_hidden(void)
{
    static const struct run donor = CI_RUN("hdonor", "2026-10-01T01:00:00.000000Z");
    static const struct run unlisted = {"hunlisted", "ci",
                                        "kvm",       "2026-10-01T02:00:00.000000Z",
                                        3,           3,
                                        10,          0,
                                        40,          {{NODE, "build", 0, 4, 1, 0}},
                                        1,           "pass",
                                        40,          0,
                                        NULL,        NULL,
                                        0,           0,
                                        NULL};
    static const struct run foreign = {"hforeign", "ci",  "kvm", "2026-10-01T02:00:00.000000Z",
                                       3,          3,     10,    0,
                                       40,         {{0}}, 0,     "pass",
                                       40,         0,     NULL,  "hdonor-s/1",
                                       0,          0,     NULL};
    static const struct run early = {"hearly", "ci",
                                     "kvm",    "2026-10-01T02:00:00.000000Z",
                                     3,        3,
                                     10,       5,
                                     40,       {{NODE, "build", 0, 4, 0, 0}},
                                     1,        "pass",
                                     35,       0,
                                     NULL,     NULL,
                                     0,        0,
                                     NULL};
    static const struct run clock = {"hclock", "ci",
                                     "kvm",    "2026-10-01T02:00:00.000000Z",
                                     3,        3,
                                     10,       0,
                                     40,       {{NODE, "build", 0, 4, 0, 1}},
                                     1,        "pass",
                                     40,       0,
                                     NULL,     NULL,
                                     0,        0,
                                     NULL};
    static const struct run *const runs[] = {&donor, &unlisted, &foreign, &early, &clock};
    struct etude_budget_judgement j;
    struct cmp_history h;

    if (load(runs, 5, &h, "hidden") != 0) {
        return;
    }
    CHECK(judge(&h, "hunlisted", 0, &j) == 0, "hidden-unlisted");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE, "hidden-unlisted");
    CHECK(says(&j.report, "shared step hunlisted-s/1 is not among the run's preparation steps"),
          "hidden-unlisted");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "hforeign", 0, &j) == 0, "hidden-foreign");
    CHECK(j.report.verdict == ETUDE_BUDGET_FAILURE, "hidden-foreign");
    CHECK(says(&j.report, "preparation step hdonor-s/1 was recorded in run hdonor"),
          "hidden-foreign");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "hforeign", 1, &j) == 0, "hidden-node");
    CHECK(says(&j.report, "failure: node " NODE ", which host.sha256.vectors prepares on none, "
                          "has no preparation step"),
          "hidden-node");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "hearly", 1, &j) == 0, "hidden-early");
    CHECK(j.report.verdict == ETUDE_BUDGET_PASS && !j.agrees, "hidden-early");
    CHECK(j.charged_known && j.charged_us == 40 * (uint64_t)SECOND, "hidden-early");
    CHECK(says(&j.report, "began 5.000000 s before the run"), "hidden-early");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "hclock", 0, &j) == 0, "hidden-clock");
    CHECK(j.report.verdict == ETUDE_BUDGET_UNKNOWN && !j.charged_known, "hidden-clock");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
}

/**
 * The first one-time setup the policy lists is left out of the charge when
 * it ends before the run's other work; a second one on the machine, and
 * a step the policy does not list, are charged.
 */
static void test_one_time(void)
{
    static const struct run once = {
        "oonce", "ci",
        "kvm",   "2026-10-01T01:00:00.000000Z",
        3,       3,
        10,      0,
        40,      {{"setup.toolchain", "one_time_setup", 0, 3, 0, 0}, {NODE, "build", 3, 4, 0, 0}},
        2,       "pass",
        37,      3,
        NULL,    NULL,
        0,       0,
        NULL};
    static const struct run again = {
        "oagain", "ci",
        "kvm",    "2026-10-01T02:00:00.000000Z",
        3,        3,
        10,       0,
        40,       {{"setup.toolchain", "one_time_setup", 0, 3, 0, 0}, {NODE, "build", 3, 4, 0, 0}},
        2,        "pass",
        37,       3,
        NULL,     NULL,
        0,        0,
        NULL};
    static const struct run unlisted = {
        "ounlisted", "ci",
        "kvm",       "2026-10-01T03:00:00.000000Z",
        3,           3,
        10,          0,
        40,          {{"setup.cache", "one_time_setup", 0, 3, 0, 0}, {NODE, "build", 3, 4, 0, 0}},
        2,           "pass",
        37,          3,
        NULL,        NULL,
        0,           0,
        NULL};
    static const struct run overlap = {
        "ooverlap",
        "ci",
        "kvm",
        "2026-10-01T00:30:00.000000Z",
        3,
        3,
        10,
        0,
        40,
        {{"setup.toolchain", "one_time_setup", 0, 4, 0, 0}, {NODE, "build", 3, 4, 0, 0}},
        2,
        "pass",
        36,
        4,
        NULL,
        NULL,
        0,
        0,
        NULL};
    static const struct run *const runs[] = {&once, &again, &unlisted};
    static const struct run *const alone[] = {&overlap};
    struct etude_budget_judgement j;
    struct cmp_history h;

    if (load(alone, 1, &h, "one-time-overlap") != 0) {
        return;
    }
    CHECK(judge(&h, "ooverlap", 0, &j) == 0, "one-time-overlap");
    CHECK(j.report.verdict == ETUDE_BUDGET_PASS && !j.agrees, "one-time-overlap");
    CHECK(j.charged_us == 40 * (uint64_t)SECOND && j.one_time_us == 0, "one-time-overlap");
    CHECK(says(&j.report, "it overlaps the run's other work"), "one-time-overlap");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
    if (load(runs, 3, &h, "one-time") != 0) {
        return;
    }
    CHECK(judge(&h, "oonce", 0, &j) == 0, "one-time");
    CHECK(j.report.verdict == ETUDE_BUDGET_PASS && j.agrees, "one-time");
    CHECK(j.charged_us == 37 * (uint64_t)SECOND && j.one_time_us == 3 * (uint64_t)SECOND,
          "one-time");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "oagain", 0, &j) == 0, "one-time-again");
    CHECK(j.charged_us == 40 * (uint64_t)SECOND && j.one_time_us == 0 && !j.agrees,
          "one-time-again");
    CHECK(says(&j.report, "run oonce did it before on this machine"), "one-time-again");
    etude_budget_report_free(&j.report);
    CHECK(judge(&h, "ounlisted", 0, &j) == 0, "one-time-unlisted");
    CHECK(j.charged_us == 40 * (uint64_t)SECOND && !j.agrees, "one-time-unlisted");
    CHECK(says(&j.report, "the policy does not list it as one-time setup"), "one-time-unlisted");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
}

/* ---- The program. ---- */

/** Run history_budget with `argv` in a child, output to `out`; its status. */
static int program(char **argv, const char *out)
{
    pid_t pid;
    int status, argc;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        for (argc = 0; argv[argc] != NULL; argc++) {
        }
        if (freopen(out, "w", stdout) == NULL) {
            _exit(99);
        }
        _exit(etude_budget_main(argc, argv));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/** Say whether the file at `path` contains `text`. */
static int contains(const char *path, const char *text)
{
    char *bytes = NULL;
    size_t length = 0;
    int found;

    if (cmp_read_file(path, 1 << 20, &bytes, &length) != 0) {
        return 0;
    }
    found = strstr(bytes, text) != NULL;
    free(bytes);
    return found;
}

/**
 * history_budget's status is 0 for a pass, 1 for a verdict that is not one
 * or a record that differs, and 2 for a run it cannot judge or a wrong
 * usage; a forecast reads its manifests from files.
 */
static void test_program(void)
{
    static const struct run ok = CI_RUN("pok", "2026-10-01T01:00:00.000000Z");
    static const struct run left = {"pleft", "ci",
                                    "kvm",   "2026-10-01T01:00:00.000000Z",
                                    3,       2,
                                    10,      0,
                                    40,      {{NODE, "build", 0, 4, 0, 0}},
                                    1,       "pass",
                                    40,      0,
                                    NULL,    NULL,
                                    0,       0,
                                    NULL};
    static const struct run short_charge = {"pshort", "ci",
                                            "kvm",    "2026-10-01T01:00:00.000000Z",
                                            3,        3,
                                            10,       0,
                                            40,       {{NODE, "build", 0, 4, 0, 0}},
                                            1,        "pass",
                                            30,       0,
                                            NULL,     NULL,
                                            0,        0,
                                            NULL};
    const char *policy_path = fix_path("policy.json"), *out = fix_path("program.out");
    const char *a = write_run(&ok), *b = write_run(&left), *c = write_run(&short_charge), *cases;
    struct jbuf text = {0};
    char *check_ok[] = {"history_budget",    "-s",      SCHEMAS, "check",
                        (char *)policy_path, "pok",     CASES,   "--",
                        (char *)a,           (char *)b, NULL};
    char *check_left[] = {"history_budget", "check", (char *)policy_path, "pleft", "--", (char *)a,
                          (char *)b,        NULL};
    char *check_short[] = {"history_budget", "check", (char *)policy_path, "pshort", "--",
                           (char *)c,        NULL};
    char *check_absent[] = {"history_budget", "check", (char *)policy_path, "none", "--",
                            (char *)a,        NULL};
    char *usage[] = {"history_budget", "check", (char *)policy_path, "pok", NULL};
    char *fc[] = {"history_budget", "forecast", (char *)policy_path, "ci", NULL, "--",
                  (char *)a,        NULL};

    json_canonical(manifest, &text);
    cases = put("cases.json", text.data);
    jbuf_free(&text);
    fc[4] = (char *)cases;
    CHECK(a != NULL && b != NULL && c != NULL && cases != NULL, "program");
    if (a == NULL || b == NULL || c == NULL || cases == NULL) {
        return;
    }
    CHECK(program(check_ok, out) == 0 && contains(out, "history_budget: result: pass"), "program");
    CHECK(contains(out, "charge: 40.000000 s"), "program");
    CHECK(program(check_left, out) == 1 &&
              contains(out, "history_budget: result: unknown; the run records another budget"),
          "program");
    CHECK(program(check_short, out) == 1 &&
              contains(out, "history_budget: result: pass; the run records another budget"),
          "program");
    CHECK(program(check_absent, out) == 2, "program");
    CHECK(program(usage, out) == 2, "program");
    CHECK(program(fc, out) == 0 && contains(out, "forecast of ci on m1, linux, kvm: 3 items"),
          "program");
}

/**
 * An unevaluated budget accepts the checker's verdict, but still has to
 * account for the charge and setup time. The command's status follows
 * its judgement, including an overrun, an exemption and incomplete work.
 */
static void test_unjudged(void)
{
    static const struct {
        int end, summarized, charged, once, exemption;
        const char *recorded;
        enum etude_budget_verdict verdict;
        int agrees, status;
    } rows[] = {{40, 3, 40, 0, 0, "unknown", ETUDE_BUDGET_PASS, 1, 0},
                {700, 3, 700, 0, 0, "unknown", ETUDE_BUDGET_FAILURE, 1, 1},
                {40, 3, 40, 0, 1, "unknown", ETUDE_BUDGET_EXEMPT, 1, 0},
                {40, 2, 40, 0, 0, "unknown", ETUDE_BUDGET_UNKNOWN, 1, 1},
                {40, 3, 30, 0, 0, "unknown", ETUDE_BUDGET_PASS, 0, 1},
                {40, 3, 40, 1, 0, "unknown", ETUDE_BUDGET_PASS, 0, 1},
                {40, 3, 40, 0, 0, "failure", ETUDE_BUDGET_PASS, 0, 1}};
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct run r = CI_RUN("unjudged", "2026-10-01T01:00:00.000000Z");
        struct etude_budget_judgement j;
        struct cmp_history h;
        const char *out = fix_path("unjudged.out"), *path;
        char result[100], id[40];
        char *args[] = {
            "history_budget", "check", (char *)fix_path("policy.json"), id, "--", NULL, NULL};

        snprintf(id, sizeof(id), "unjudged%zu", i);
        r.id = id;
        r.end = rows[i].end;
        r.summarized = rows[i].summarized;
        r.charged = rows[i].charged;
        r.once = rows[i].once;
        r.exemption = rows[i].exemption;
        r.purpose = r.exemption ? "phase_first_run" : "ci";
        r.verdict = rows[i].recorded;
        path = write_run(&r);
        CHECK(path != NULL, "unjudged");
        if (path == NULL) {
            return;
        }
        args[5] = (char *)path;
        if (cmp_load(&h, HISTORY, args + 5, 1) != 0) {
            CHECK(0, "unjudged");
            return;
        }
        CHECK(judge(&h, r.id, 0, &j) == 0, "unjudged");
        CHECK(j.report.verdict == rows[i].verdict && j.agrees == rows[i].agrees, "unjudged");
        etude_budget_report_free(&j.report);
        cmp_free(&h);
        snprintf(result, sizeof(result), "history_budget: result: %s",
                 etude_budget_verdict_name(rows[i].verdict));
        CHECK(program(args, out) == rows[i].status && contains(out, result), "unjudged");
        CHECK(contains(out, "the run records another budget") == !rows[i].agrees, "unjudged");
    }
}

int main(void)
{
    char err[1200];
    struct json *workload;

    parent = getpid();
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    if (etude_budget_manifest_read(SCHEMAS, CASES, &manifest, err, sizeof(err)) != 0 ||
        read_policy("policy.json", &usual, &policy, err, sizeof(err)) != 0) {
        printf("etude-budget-test: fail: %s\n", err);
        return 1;
    }
    the_case = json_member(manifest, "cases")->item[0];
    workload = (struct json *)json_member(the_case, "workload");
    ((struct json *)json_member(workload, "repetitions"))->integer = 3;
    test_policy();
    test_digests();
    test_services();
    test_schedule();
    test_forecast();
    test_unknown_cost();
    test_deadline();
    test_judge();
    test_unobserved();
    test_reuse();
    test_hidden();
    test_one_time();
    test_program();
    test_unjudged();
    etude_budget_policy_free(&policy);
    json_free(manifest);
    if (fails != 0) {
        return 1;
    }
    fix_passed();
    printf("etude_budget_hosttest: pass\n");
    return 0;
}
