/**
 * Check etude's shuffled and loaded repetitions: the plan a seed gives and
 * its record (tools/etude/repeat.c), the driver (tools/etude/posix/repeat.c)
 * and the load fixture (tools/etude/posix/load.c), with this program as the
 * cases' program and as the load.
 *
 * host-inputs: tools/etude/schemas/history.schema.json
 * host-inputs: tools/etude/schemas/repeat.schema.json
 * host-inputs: tools/etude/schemas/repeat.example.json
 * host-inputs: tools/etude/schemas/budget.schema.json
 * host-deps: tools/etude/routine_charge.c
 * host-deps: tools/etude/authority.c tools/etude/authority_parse.c
 * host-deps: tools/etude/posix/authority_platform.c
 * host-deps: tools/etude/win/authority_platform.c
 * host-deps: tools/etude/repeat.c tools/etude/posix/repeat.c
 * host-deps: tools/etude/compare_load.c tools/etude/observe_import.c
 * host-deps: tools/etude/json.c
 * host-deps: tools/etude/posix/load.c tools/etude/ledger.c
 * host-deps: tools/etude/watchdog.c tools/etude/supervise.c
 * host-deps: tools/etude/capture.c tools/etude/qemu_profile.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/qemu.c
 * host-deps: tools/etude/processor.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-deps: tools/etude/budget.c tools/etude/digest.c tools/etude/cost.c
 * host-deps: tools/etude/budget_prepare.c
 * host-deps: tools/etude/sched.c tools/etude/json.c tools/etude/schema.c
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 * host-first: it runs for minutes
 * host-alone: gap expiry and load reservations require an idle fixture host
 *
 * The generator gives splitmix64's published outputs, and a number below a
 * bound without bias. A seed gives one plan, the same each time, and the
 * plan of a pinned seed is pinned; a record keeps the seed and its plan, and
 * a record whose items, seed or loads were changed is refused.
 *
 * Two cases race over a file one makes and the other reads, and pass in the
 * order of their manifest. A seed that shuffles a reader first exposes the
 * race, the same seed exposes it again, and a seed that does not passes.
 * Each failure is retried after the planned items, where the retry passes,
 * and the history keeps both attempts, the item's failure, and the case
 * possibly flaky. Two cases that meet at a FIFO pass two at a time and run
 * out of their gap one at a time. Loads of processors, memory and disk run
 * beside their cases, reserved in the ledger while the case runs, and end
 * with their cleanup confirmed; a load that is never ready, one that ends
 * before its case and one that cannot fit beside its case are told apart.
 * Modelled supervisor results cover lost and stopped ends and unconfirmed
 * cleanup; modelled admission covers a ledger with no recoverable room.
 * Each records a disruption in the history, while a case's timeout does not.
 * A run's budget records the limits of its class under the site's policy,
 * and the check of the budget passes a run that passed.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "compat/directory_host.h"
#include "../../utils/sha256.h"
#include "budget.h"
#include "compare.h"
#include "etude/repeat.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-repeat-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

#define MS     UINT64_C(1000)
#define SECOND UINT64_C(1000000)
#define MIB    UINT64_C(1048576)
#define SCHEMA "tools/etude/schemas/history.schema.json"
#define RECORD "tools/etude/schemas/repeat.schema.json"

/* The plan of seed 42 in test_plan, as plan_text writes it. */
#define PINNED_42 "x86_64/1/- x86_64/0/disk1m i686/0/disk1m i686/2/mem1m none/0/- i686/1/-"

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* Results supplied to the driver after the real supervisor has finished. */
enum result_fault {
    ORDINARY,
    LOST,
    LOST_GAP,
    STOPPED_END,
    CASE_CLEANUP,
    LOAD_CLEANUP,
    FAILURE_CLEANUP
};
static enum result_fault result_fault;
static int admission_errno;

/* Observe real FIFO events before applying the execution gap. Startup and
 * the width-two rendezvous retain the ordinary whole-case bound. */
static unsigned width_mode;
static struct {
    struct etude_supervised *attempt;
    unsigned starts, ready, met, armed;
} width_peers[2];

/** Count complete event lines in a captured child stream. */
static unsigned width_events(const char *text, const char *event)
{
    unsigned n = 0;
    size_t len = strlen(event);

    while (text != NULL && (text = strstr(text, event)) != NULL) {
        n++;
        text += len;
    }
    return n;
}

/** Read each live child's real output and arm its execution gap. */
static void width_observe(void)
{
    const struct etude_supervised_result *r;
    unsigned i;

    for (i = 0; i < 2; i++) {
        if (width_peers[i].attempt == NULL)
            continue;
        r = etude_supervised_result(width_peers[i].attempt);
        if (r->proc == NULL)
            continue;
        width_peers[i].ready = width_events(r->proc->out.data, "pair ready\n");
        width_peers[i].met = width_events(r->proc->out.data, "pair met\n");
        if (!width_peers[i].armed &&
            (width_mode == 1 ? width_peers[i].ready : width_peers[i].met)) {
            etude_supervised_stage(width_peers[i].attempt, ETUDE_STAGE_EXECUTE);
            width_peers[i].armed = 1;
        }
    }
}

/** Put only the real pair cases in their event-driven preparation stage. */
static int width_start(struct etude_supervisor *s, const struct etude_exec_spec *spec,
                       const struct etude_watchdog_spec *wd, struct etude_supervised **out)
{
    int rc = etude_supervise_host(s, spec, wd, out);
    unsigned i;

    if (rc != 0 || width_mode == 0)
        return rc;
    for (i = 0; i < 2; i++) {
        const char *id = i == 0 ? ":test.repeat.pair-r:" : ":test.repeat.pair-w:";
        if (strstr(spec->attempt_id, id) != NULL) {
            CHECK(width_peers[i].attempt == NULL, "width ownership");
            width_peers[i].attempt = *out;
            width_peers[i].starts++;
            etude_supervised_stage(*out, ETUDE_STAGE_PREPARE);
        }
    }
    return rc;
}

/** Serve real executor events, then inspect the captured readiness bytes. */
static struct etude_supervised *width_wait(struct etude_supervisor *s, int timeout_ms)
{
    struct etude_supervised *done;

    width_observe();
    done = etude_supervise_wait(s, timeout_ms);
    width_observe();
    return done;
}

/** Keep the final events before the real supervisor frees its capture. */
static void width_free(struct etude_supervised *a)
{
    unsigned i;

    width_observe();
    for (i = 0; i < 2; i++)
        if (width_peers[i].attempt == a)
            width_peers[i].attempt = NULL;
    etude_supervised_free(a);
}

/** Supply one finished result with a lost end or unconfirmed cleanup. */
static const struct etude_supervised_result *driver_result(const struct etude_supervised *a)
{
    const struct etude_supervised_result *r = etude_supervised_result(a);
    static struct etude_supervised_result changed;
    size_t n = strlen(r->attempt_id);
    int load = n >= 5 && strcmp(r->attempt_id + n - 5, ":load") == 0;

    if (!r->finished || result_fault == ORDINARY || load != (result_fault == LOAD_CLEANUP))
        return r;
    changed = *r;
    if (result_fault == LOST || result_fault == LOST_GAP || result_fault == STOPPED_END) {
        changed.end = result_fault == STOPPED_END ? ETUDE_END_STOPPED : ETUDE_END_LOST;
        changed.value = -1;
        changed.incomplete = 1;
        if (result_fault == LOST_GAP) {
            changed.expiry = ETUDE_EXPIRY_GAP;
            changed.bound_us = 300 * MS;
        }
    } else {
        changed.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
        if (result_fault == FAILURE_CLEANUP)
            changed.value = 1;
    }
    return &changed;
}

static int driver_repeat_run(const struct etude_repeat_spec *spec,
                             const struct etude_repeat_site *site, struct etude_repeat_result *r);
static void driver_repeat_free(struct etude_repeat_result *r);

#define etude_repeat_run           driver_repeat_run
#define etude_repeat_result_free   driver_repeat_free
#define etude_supervised_result(a) driver_result(a)
#define etude_supervise_host       width_start
#define etude_supervise_wait       width_wait
#define etude_supervised_free      width_free
#define etude_ledger_admit(...) \
    (admission_errno ? (errno = admission_errno, -1) : etude_ledger_admit(__VA_ARGS__))
#include "posix/repeat.c"
#undef etude_ledger_admit
#undef etude_supervised_free
#undef etude_supervise_wait
#undef etude_supervise_host
#undef etude_supervised_result
#undef etude_repeat_result_free
#undef etude_repeat_run

/* ---- The cases' program and the loads. ---- */

/** Give the path of `name` in the cases' shared directory. */
static const char *shared(const char *name)
{
    static char path[PATH_MAX];
    const char *dir = getenv("ETUDE_TEST_SHARED");

    snprintf(path, sizeof(path), "%s/%s", dir != NULL ? dir : ".", name);
    return path;
}

/* What a reading of the ledger finds of the loads and the cases. */
struct seen {
    unsigned loads, cases; /* claims running */
    int load_ok;           /* the one load's reservation is one the test declares */
};

/** Count the running claims of loads and of cases. */
static int seen_visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct seen *s = ctx;
    size_t n = strlen(c->id);

    if (strcmp(c->state, "running") != 0)
        return 0;
    if (n > 5 && strcmp(c->id + n - 5, ":load") == 0) {
        s->loads++;
        s->load_ok = c->nres == 1 && c->nproc >= 2 &&
                     ((strcmp(c->res[0].name, "cpu_millis") == 0 && c->res[0].amount == 1000) ||
                      (strcmp(c->res[0].name, "memory_bytes") == 0 && c->res[0].amount == MIB) ||
                      (strcmp(c->res[0].name, "disk_bytes") == 0 && c->res[0].amount == MIB));
    } else {
        s->cases++;
    }
    return 0;
}

/** Read the ledger the cases share into `s`; 0, or -1. */
static int look(struct seen *s)
{
    struct etude_ledger *l = etude_ledger_open(getenv("ETUDE_TEST_LEDGER"));
    int rc;

    memset(s, 0, sizeof(*s));
    if (l == NULL)
        return -1;
    rc = etude_ledger_list(l, seen_visit, s);
    etude_ledger_close(l);
    return rc;
}

/** Create a shared file for a case; give its exit status. */
static int create_shared(const char *name)
{
    int fd = open(shared(name), O_WRONLY | O_CREAT, 0644);

    return fd >= 0 && close(fd) == 0 ? 0 : 3;
}

/** Be fixture case `name`; give its exit status. */
static int case_main(const char *name)
{
    struct seen s;
    char c = 0;
    int fd;

    if (strcmp(name, "seed") == 0)
        return create_shared("state");
    if (strcmp(name, "read") == 0) {
        if (access(shared("state"), F_OK) == 0)
            return 0;
        printf("read: the state that seed makes is missing\n");
        return 1;
    }
    if (strcmp(name, "mark") == 0)
        return create_shared("marked");
    if (strcmp(name, "pair-w") == 0 || strcmp(name, "pair-r") == 0) {
        if (mkfifo(shared("fifo"), 0600) != 0 && errno != EEXIST)
            return 3;
        if (printf("pair ready\n") < 0 || fflush(stdout) != 0)
            return 3;
        if (name[5] == 'w') {
            fd = open(shared("fifo"), O_WRONLY);
            if (fd < 0 || write(fd, "x", 1) != 1 || close(fd) != 0)
                return 1;
        } else {
            fd = open(shared("fifo"), O_RDONLY);
            if (fd < 0 || read(fd, &c, 1) != 1 || c != 'x' || close(fd) != 0)
                return 1;
        }
        return printf("pair met\n") >= 0 && fflush(stdout) == 0 ? 0 : 3;
    }
    if (strcmp(name, "witness") == 0)
        return look(&s) == 0 && s.loads == 1 && s.load_ok && s.cases == 1 ? 0 : 1;
    if (strcmp(name, "wait-load") == 0) {
        do
            if (look(&s) != 0)
                return 3;
        while (s.loads > 0);
        return 0;
    }
    return 3;
}

/** Be a load that is ready only once its case runs, and then ends. */
static int short_load(void)
{
    struct seen s;

    if (write(1, ETUDE_LOAD_READY, strlen(ETUDE_LOAD_READY)) < 0)
        return 3;
    do
        if (look(&s) != 0)
            return 3;
    while (s.cases == 0);
    return 0;
}

/* ---- Fixtures of the driver. ---- */

/* The machine of every run: four processors, 64 MiB of memory and of disk. */
static const struct etude_ledger_res CAP[] = {
    {"cpu_millis", 4000}, {"memory_bytes", 64 * MIB}, {"disk_bytes", 64 * MIB}};
static const struct etude_ledger_res SMALL[] = {{"cpu_millis", 1500}};
static const struct etude_ledger_res CASE_RES[] = {{"cpu_millis", 100}};
static const struct etude_ledger_res WIDE_RES[] = {{"cpu_millis", 1000}};

/* The bounds of an ordinary case, and of one that must run out of its gap. */
static const struct etude_watchdog_spec WD = {20 * SECOND, 20 * SECOND, {0}, NULL, 0};
static const struct etude_watchdog_spec SHORT_WD = {20 * SECOND, 300 * MS, {0}, NULL, 0};

static const struct etude_load LOADS[] = {{"cpu1", ETUDE_LOAD_CPU, 1},
                                          {"mem1m", ETUDE_LOAD_MEMORY, MIB},
                                          {"disk1m", ETUDE_LOAD_DISK, MIB}};

/* The blob references every plan and case shares, and the plan's members. */
static char ref[200], plan_fixed[4096];

/* The site's budget policy, whose focused class has limits of its own, and
 * its blob reference. */
static struct etude_budget_policy policy;
static char policy_ref[200];

/* The environment of the cases: this one's, the shared directory's and the
 * ledger's. */
static char **case_env;
static char env_shared[PATH_MAX + 32], env_ledger[PATH_MAX + 32];

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

/** Write the site's budget policy and read it into `policy`; 0, or -1. */
static int budget_policy(void)
{
    static const char limits[] =
        "{\"goal_us\":3600000000,\"growth_us\":3600000000,\"ceiling_us\":14400000000,"
        "\"cleanup_reserve_us\":600000000}";
    const char *path = fix_path("budget.json");
    char text[2048], err[1200];
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return -1;
    snprintf(text, sizeof(text),
             "{\"schema_version\":1,\"policy\":\"budget-v1\",\"site\":{\"machine_id\":\"m1\","
             "\"os\":\"linux\",\"accelerator\":\"native\",\"capacity\":{\"cpu_millis\":3000,"
             "\"memory_bytes\":1073741824,\"disk_bytes\":1073741824}},\"classes\":{\"push\":%s,"
             "\"ci\":%s,\"release\":%s,\"focused\":{\"goal_us\":600000000,\"growth_us\":"
             "60000000,\"ceiling_us\":1200000000,\"cleanup_reserve_us\":30000000},"
             "\"phase_first_run\":%s},\"one_time_setup\":[]}\n",
             limits, limits, limits, limits);
    if (fputs(text, f) == EOF) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    if (etude_budget_policy_read("tools/etude/schemas", path, &policy, err, sizeof(err)) != 0) {
        printf("etude-repeat-test: %s\n", err);
        return -1;
    }
    snprintf(policy_ref, sizeof(policy_ref),
             "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"test://budget\"]}",
             policy.sha256, (unsigned long)policy.size_bytes);
    return 0;
}

/** Make the fixtures the runs share; 0, or -1. */
static int fixtures(void)
{
    size_t n = 0, i;

    if (budget_policy() != 0)
        return -1;
    blob(ref, sizeof(ref), "fixed");
    snprintf(plan_fixed, sizeof(plan_fixed),
             "\"purpose\":\"focused\",\"commit\":\"1111111111111111111111111111111111111111\","
             "\"source\":%s,\"runner\":%s,\"artifacts\":[],\"environment\":{\"machine_id\":\"m1\","
             "\"boot_id\":\"b1\",\"os\":\"linux\",\"os_version\":\"26.04\",\"accelerator\":"
             "\"native\",\"site_profile\":%s,\"tools\":%s,\"capacity\":%s,\"cache_state\":%s,"
             "\"load\":%s},\"policies\":%s,\"exemption\":null,\"reproduce\":%s",
             ref, ref, ref, ref, ref, ref, ref, ref, ref);
    while (environ[n] != NULL)
        n++;
    case_env = calloc(n + 3, sizeof(*case_env));
    if (case_env == NULL)
        return -1;
    for (i = 0; i < n; i++)
        case_env[i] = environ[i];
    snprintf(env_shared, sizeof(env_shared), "ETUDE_TEST_SHARED=%s", fix_path("shared"));
    snprintf(env_ledger, sizeof(env_ledger), "ETUDE_TEST_LEDGER=%s", fix_path("ledger"));
    case_env[n] = env_shared;
    case_env[n + 1] = env_ledger;
    return mkdir(fix_path("store"), 0700) == 0 && mkdir(fix_path("scratch"), 0700) == 0 ? 0 : -1;
}

/** Empty the cases' shared directory, making it when it is missing. */
static void fresh_shared(void)
{
    static const char *const names[] = {"state", "marked", "fifo"};
    size_t i;

    mkdir(fix_path("shared"), 0700);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s/%s", fix_path("shared"), names[i]);
        unlink(path);
    }
}

/* The argument vectors of the fixture cases and of the loads. */
static char *argv_of[8][4];
static char *load_argv[] = {self, "--load", NULL};
static char *broken_argv[] = {self, "--broken-load", NULL};
static char *short_argv[] = {self, "--short-load", NULL};

/** Make case `name` on `arch` of `reps` repetitions, with argument vector
 * slot `slot`, bounds `wd` and reservation `res`. */
static struct etude_repeat_case make_case(int slot, const char *name, const char *arch,
                                          unsigned reps, const struct etude_watchdog_spec *wd,
                                          const struct etude_ledger_res *res)
{
    static char ids[8][64];
    struct etude_repeat_case c;

    memset(&c, 0, sizeof(c));
    snprintf(ids[slot], sizeof(ids[slot]), "test.repeat.%s", name);
    argv_of[slot][0] = self;
    argv_of[slot][1] = "--case";
    argv_of[slot][2] = (char *)name;
    argv_of[slot][3] = NULL;
    c.case_id = ids[slot];
    c.arch = arch;
    c.repetitions = reps;
    c.family_id = "test.repeat";
    c.argv = argv_of[slot];
    c.envp = case_env;
    c.output_max = 4096;
    c.grace_us = 500 * MS;
    c.wd = wd;
    c.res = res;
    c.nres = 1;
    c.workload = c.assertions = c.profile = ref;
    c.inventory_sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    return c;
}

/** Say whether the ledger of the runs keeps nothing. */
static int count_visit(const struct etude_ledger_claim *c, void *ctx)
{
    (void)c;
    ++*(size_t *)ctx;
    return 0;
}

/**
 * Run `spec` as run `run`, on the machine of `cap` with the loads of
 * `load`, into the stream `run`.jsonl; 0 with the result in `r`, or -1.
 * The ledger must be empty afterward.
 */
static int run(const char *run_id, const struct etude_repeat_spec *spec,
               const struct etude_ledger_res *cap, size_t ncap, char **load,
               struct etude_repeat_result *r)
{
    struct etude_repeat_site site;
    struct etude_ledger *l = etude_ledger_open(fix_path("ledger"));
    struct etude_history *h;
    char name[64], stream[80];
    size_t entries = 0;
    int rc;

    snprintf(name, sizeof(name), "%s.jsonl", run_id);
    snprintf(stream, sizeof(stream), "%s-s", run_id);
    h = etude_history_open(fix_path(name), SCHEMA, stream, run_id, UINT64_C(1) << 24);
    if (l == NULL || h == NULL) {
        if (l != NULL)
            etude_ledger_close(l);
        if (h != NULL)
            etude_history_close(h);
        return -1;
    }
    memset(&site, 0, sizeof(site));
    site.run_id = run_id;
    site.ledger = l;
    site.account = "test";
    site.cap = cap;
    site.ncap = ncap;
    site.load_argv = load;
    site.load_envp = case_env;
    site.scratch = fix_path("scratch");
    site.load_ready_us = 10 * SECOND;
    site.load_grace_us = SECOND;
    site.history = h;
    site.store = fix_path("store");
    site.plan_fixed = plan_fixed;
    site.machine_id = "m1";
    site.boot_id = "b1";
    site.budget_policy = policy_ref;
    site.budget_limits = &policy.limits[ETUDE_BUDGET_FOCUSED];
    rc = result_fault != ORDINARY || admission_errno || width_mode
             ? driver_repeat_run(spec, &site, r)
             : etude_repeat_run(spec, &site, r);
    if (rc != 0)
        printf("etude-repeat-test: run %s: %s\n", run_id, strerror(errno));
    if (etude_history_close(h) != 0)
        rc = -1;
    if (result_fault == CASE_CLEANUP || result_fault == LOAD_CLEANUP ||
        result_fault == FAILURE_CLEANUP) {
        size_t i;

        CHECK(etude_ledger_list(l, count_visit, &entries) == 0 && entries == 1, "cleanup-claim");
        for (i = 0; i < r->nattempts; i++) {
            char id[288];

            snprintf(id, sizeof(id), "%s%s", r->attempts[i].attempt_id,
                     result_fault == LOAD_CLEANUP ? ":load" : "");
            CHECK(etude_ledger_finish(l, id, ETUDE_CLEANUP_CONFIRMED) == 0, "cleanup-claim");
        }
        entries = 0;
    }
    if (etude_ledger_list(l, count_visit, &entries) != 0 || entries != 0) {
        printf("etude-repeat-test: run %s left %lu claims in the ledger\n", run_id,
               (unsigned long)entries);
        rc = -1;
    }
    etude_ledger_close(l);
    return rc;
}

/** Give the attempt of item `item`, try `try`, of `r`, or NULL. */
static const struct etude_repeat_attempt *attempt(const struct etude_repeat_result *r, size_t item,
                                                  unsigned try)
{
    size_t i;

    for (i = 0; i < r->nattempts; i++)
        if (r->attempts[i].item == item && r->attempts[i].try == try)
            return &r->attempts[i];
    return NULL;
}

/* ---- The generator and the plan. ---- */

/** splitmix64's published outputs, and numbers below a bound. */
static void test_rng(void)
{
    static const uint64_t want[] = {UINT64_C(6457827717110365317), UINT64_C(3203168211198807973),
                                    UINT64_C(9817491932198370423), UINT64_C(4593380528125082431),
                                    UINT64_C(16408922859458223821)};
    struct etude_rng r, q;
    unsigned seen[6] = {0};
    size_t i;

    etude_rng_seed(&r, 1234567);
    for (i = 0; i < 5; i++)
        CHECK(etude_rng_next(&r) == want[i], "rng");
    etude_rng_seed(&r, 99);
    q = r;
    CHECK(etude_rng_below(&r, 0) == 0 && r.state == q.state, "rng");
    CHECK(etude_rng_below(&r, 1) == 0, "rng");
    for (i = 0; i < 6000; i++) {
        uint64_t v = etude_rng_below(&r, 6);

        if (v < 6)
            seen[v]++;
        else
            CHECK(v < 6, "rng");
    }
    for (i = 0; i < 6; i++)
        CHECK(seen[i] > 800 && seen[i] < 1200, "rng");
}

/** Write the plan's items as text, CASE/REP/LOAD each, into `out`. */
static void plan_text(const struct etude_repeat_spec *spec, const struct etude_repeat_item *it,
                      size_t n, char *out, size_t size)
{
    size_t i, used = 0;

    out[0] = '\0';
    for (i = 0; i < n && used < size; i++)
        used += (size_t)snprintf(
            out + used, size - used, "%s%s/%u/%s", i ? " " : "", spec->cases[it[i].case_index].arch,
            it[i].repetition,
            it[i].load == ETUDE_REPEAT_NO_LOAD ? "-" : spec->loads[it[i].load].load_id);
}

/**
 * A seed gives one plan each time, and the plan of a pinned seed is pinned;
 * a selection keeps the spec's order; the share of loaded items follows
 * the percentage; and a plan that cannot be drawn is refused.
 */
static void test_plan(void)
{
    struct etude_repeat_case cases[3];
    struct etude_repeat_spec spec;
    struct etude_repeat_item *a = NULL, *b = NULL;
    size_t na = 0, nb = 0, i, loaded;
    char ta[512], tb[512];

    memset(cases, 0, sizeof(cases));
    cases[0].case_id = "a";
    cases[0].arch = "i686";
    cases[0].repetitions = 3;
    cases[1].case_id = "b";
    cases[1].arch = "x86_64";
    cases[1].repetitions = 2;
    cases[2].case_id = "c";
    cases[2].arch = "none";
    cases[2].repetitions = 1;
    memset(&spec, 0, sizeof(spec));
    spec.seed = 42;
    spec.load_percent = 50;
    spec.width = 1;
    spec.cases = cases;
    spec.ncases = 3;
    spec.loads = LOADS;
    spec.nloads = 3;
    CHECK(etude_repeat_plan(&spec, &a, &na) == 0 && na == 6, "plan");
    CHECK(etude_repeat_plan(&spec, &b, &nb) == 0 && nb == 6, "plan");
    plan_text(&spec, a, na, ta, sizeof(ta));
    plan_text(&spec, b, nb, tb, sizeof(tb));
    CHECK(strcmp(ta, tb) == 0, "plan");
    CHECK(strcmp(ta, PINNED_42) == 0, "plan");
    if (strcmp(ta, PINNED_42) != 0)
        printf("etude-repeat-test: the plan of seed 42 is %s\n", ta);
    free(b);
    spec.seed = 43;
    CHECK(etude_repeat_plan(&spec, &b, &nb) == 0, "plan");
    plan_text(&spec, b, nb, tb, sizeof(tb));
    CHECK(strcmp(ta, tb) != 0, "plan");
    free(a);
    free(b);

    spec.pick = 2;
    spec.load_percent = 0;
    CHECK(etude_repeat_plan(&spec, &a, &na) == 0, "pick");
    CHECK(etude_repeat_plan(&spec, &b, &nb) == 0 && na == nb, "pick");
    for (i = 0; i < na && i < nb; i++)
        CHECK(a[i].case_index == b[i].case_index && a[i].load == ETUDE_REPEAT_NO_LOAD, "pick");
    {
        int has[3] = {0, 0, 0};

        for (i = 0; i < na; i++)
            has[a[i].case_index] = 1;
        CHECK(has[0] + has[1] + has[2] == 2, "pick");
        CHECK(na == (size_t)(has[0] * 3 + has[1] * 2 + has[2]), "pick");
    }
    free(a);
    free(b);

    spec.pick = 0;
    spec.load_percent = 100;
    cases[0].repetitions = 200;
    CHECK(etude_repeat_plan(&spec, &a, &na) == 0 && na == 203, "loads");
    for (i = 0, loaded = 0; i < na; i++)
        loaded += a[i].load != ETUDE_REPEAT_NO_LOAD;
    CHECK(loaded == na, "loads");
    free(a);
    spec.load_percent = 30;
    CHECK(etude_repeat_plan(&spec, &a, &na) == 0, "loads");
    for (i = 0, loaded = 0; i < na; i++)
        loaded += a[i].load != ETUDE_REPEAT_NO_LOAD;
    CHECK(loaded > 30 && loaded < 100, "loads");
    free(a);

    spec.seed = ETUDE_REPEAT_SEED_MAX + 1;
    CHECK(etude_repeat_plan(&spec, &a, &na) == -1 && errno == EINVAL, "plan");
    spec.seed = 1;
    spec.load_percent = 101;
    CHECK(etude_repeat_plan(&spec, &a, &na) == -1 && errno == EINVAL, "plan");
    spec.load_percent = 0;
    cases[1].repetitions = 0;
    CHECK(etude_repeat_plan(&spec, &a, &na) == -1 && errno == EINVAL, "plan");
}

/** Replace the first `from` in `text` with `to`; 0, or -1 when it is absent. */
static int replace(char *text, size_t size, const char *from, const char *to)
{
    char *at = strstr(text, from);
    size_t n = strlen(text), f = strlen(from), t = strlen(to);

    if (at == NULL || n - f + t >= size)
        return -1;
    memmove(at + t, at + f, n - (size_t)(at - text) - f + 1);
    memcpy(at, to, t);
    return 0;
}

/**
 * A record has the seed and the plan it gives, and passes its check; one
 * whose items are in another order, whose seed or load was changed, or that
 * has a member its schema lacks, is refused; the example passes.
 */
static void test_record(void)
{
    struct etude_repeat_case cases[2];
    struct etude_repeat_spec spec;
    struct etude_repeat_item *it = NULL, t;
    size_t n = 0;
    char *json, why[300], bad[8192], *example = NULL;
    size_t length;

    memset(cases, 0, sizeof(cases));
    cases[0].case_id = "host.a";
    cases[0].arch = "none";
    cases[0].repetitions = 3;
    cases[1].case_id = "guest.b";
    cases[1].arch = "x86_64";
    cases[1].repetitions = 2;
    memset(&spec, 0, sizeof(spec));
    spec.seed = 7;
    spec.load_percent = 60;
    spec.width = 2;
    spec.retries = 1;
    spec.cases = cases;
    spec.ncases = 2;
    spec.loads = LOADS;
    spec.nloads = 3;
    if (etude_repeat_plan(&spec, &it, &n) != 0 || n != 5) {
        CHECK(0, "record");
        free(it);
        return;
    }
    json = etude_repeat_record(&spec, "run-1", it, n);
    CHECK(json != NULL && etude_repeat_check(json, RECORD, why, sizeof(why)) == 0, "record");
    if (json != NULL) {
        snprintf(bad, sizeof(bad), "%s", json);
        CHECK(replace(bad, sizeof(bad), "\"seed\":7", "\"seed\":8") == 0 &&
                  etude_repeat_check(bad, RECORD, why, sizeof(why)) != 0,
              "record");
        snprintf(bad, sizeof(bad), "%s", json);
        CHECK(replace(bad, sizeof(bad), "\"width\":2", "\"width\":2,\"extra\":1") == 0 &&
                  etude_repeat_check(bad, RECORD, why, sizeof(why)) != 0,
              "record");
        free(json);
    }
    t = it[0];
    it[0] = it[1];
    it[1] = t;
    json = etude_repeat_record(&spec, "run-1", it, n);
    CHECK(json != NULL && etude_repeat_check(json, RECORD, why, sizeof(why)) != 0 &&
              strstr(why, "item 0") != NULL,
          "record");
    free(json);
    it[1] = it[0];
    it[0] = t;
    it[n - 1].load = it[n - 1].load == 0 ? 1 : 0;
    json = etude_repeat_record(&spec, "run-1", it, n);
    CHECK(json != NULL && etude_repeat_check(json, RECORD, why, sizeof(why)) != 0, "record");
    free(json);
    free(it);
    CHECK(cmp_read_file("tools/etude/schemas/repeat.example.json", 1 << 20, &example, &length) ==
                  0 &&
              etude_repeat_check(example, RECORD, why, sizeof(why)) == 0,
          "record-example");
    free(example);
}

/* ---- The seeded race. ---- */

/** Find in `seeds` from 1 the first seed whose plan of `spec` runs a reader
 * first (`first_read`) or a seeder first; the count of readers before the
 * first seeder in `*early`. */
static uint64_t find_seed(struct etude_repeat_spec *spec, int first_read, size_t *early)
{
    struct etude_repeat_item *it;
    size_t n, i;
    uint64_t s;

    for (s = 1; s < 10000; s++) {
        spec->seed = s;
        if (etude_repeat_plan(spec, &it, &n) != 0)
            return 0;
        for (i = 0; i < n && it[i].case_index == 1; i++) {
        }
        free(it);
        if ((i > 0) == first_read) {
            *early = i;
            return s;
        }
    }
    return 0;
}

/* The results of the race's runs, which the history row reads. */
static struct etude_repeat_result bad_run, again_run, good_run;
static size_t early;

/**
 * Check run `r` of the race: the readers shuffled before the first seeder
 * fail, each once, and their retries, after the planned items, pass.
 */
static void race_result(const struct etude_repeat_result *r, size_t readers_early, const char *row)
{
    size_t i, failed = 0;
    const struct etude_repeat_attempt *a, *b;

    CHECK(r->nitems == 4 && r->nattempts == 4 + readers_early, row);
    CHECK(r->outcome == (readers_early ? ETUDE_REPEAT_FAILURE : ETUDE_REPEAT_PASS), row);
    CHECK(r->cleanup == ETUDE_CLEANUP_CONFIRMED, row);
    for (i = 0; i < r->nitems; i++) {
        a = attempt(r, i, 0);
        b = attempt(r, i, 1);
        if (a == NULL) {
            CHECK(a != NULL, row);
            continue;
        }
        if (i < readers_early) {
            CHECK(r->items[i].case_index == 1, row);
            CHECK(a->outcome == ETUDE_REPEAT_FAILURE && a->end == ETUDE_END_EXITED &&
                      a->value == 1 && strstr(a->reason, "exited 1") != NULL,
                  row);
            CHECK(b != NULL && b->outcome == ETUDE_REPEAT_PASS, row);
            CHECK(b != NULL && (size_t)(b - r->attempts) >= r->nitems, row);
            failed++;
        } else {
            CHECK(a->outcome == ETUDE_REPEAT_PASS && b == NULL, row);
        }
    }
    CHECK(failed == readers_early, row);
}

/**
 * Two cases race over a file: seed makes it and read requires it. A seed
 * that shuffles a reader before every seeder exposes the race, the same
 * seed in another run exposes it again on the same items, and a seed that
 * runs a seeder first passes.
 */
static void test_race(void)
{
    struct etude_repeat_case cases[2];
    struct etude_repeat_spec spec;
    size_t good_early = 0, i;
    uint64_t bad, good;

    cases[0] = make_case(0, "seed", "none", 2, &WD, CASE_RES);
    cases[1] = make_case(1, "read", "none", 2, &WD, CASE_RES);
    memset(&spec, 0, sizeof(spec));
    spec.width = 1;
    spec.retries = 1;
    spec.cases = cases;
    spec.ncases = 2;
    bad = find_seed(&spec, 1, &early);
    good = find_seed(&spec, 0, &good_early);
    CHECK(bad != 0 && good != 0 && early > 0, "race");
    if (bad == 0 || good == 0)
        return;
    spec.seed = bad;
    fresh_shared();
    CHECK(run("race-bad", &spec, CAP, 3, load_argv, &bad_run) == 0, "race");
    race_result(&bad_run, early, "race");
    fresh_shared();
    CHECK(run("race-again", &spec, CAP, 3, load_argv, &again_run) == 0, "race-replay");
    race_result(&again_run, early, "race-replay");
    for (i = 0; i < bad_run.nitems && i < again_run.nitems; i++)
        CHECK(bad_run.items[i].case_index == again_run.items[i].case_index &&
                  bad_run.items[i].repetition == again_run.items[i].repetition,
              "race-replay");
    spec.seed = good;
    fresh_shared();
    CHECK(run("race-good", &spec, CAP, 3, load_argv, &good_run) == 0, "race-good");
    race_result(&good_run, 0, "race-good");
}

/* ---- The history of retries. ---- */

/** Give the record of `id` in `h` as its data, or NULL. */
static const struct eh_json *data_of(const struct cmp_history *h, const char *id)
{
    const struct cmp_record *rec = cmp_find(h, id);

    return rec != NULL ? rec->data : NULL;
}

/** Give the tally of `case_id` in `t`, or NULL. */
static const struct etude_repeat_tally *tally(const struct etude_repeat_tally *t, size_t n,
                                              const char *case_id)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (strcmp(t[i].case_id, case_id) == 0)
            return &t[i];
    return NULL;
}

/** Read file `path` of the store into a string; NULL when it cannot be. */
static char *stored(const struct eh_json *blob_ref)
{
    const struct eh_json *loc = eh_json_get(blob_ref, "locations");
    char *bytes = NULL;
    size_t n;

    if (loc == NULL || loc->child == NULL ||
        cmp_read_file(loc->child->string, 1 << 22, &bytes, &n) != 0)
        return NULL;
    return bytes;
}

/** Require each attempt's disruption in the history and in its tally. */
static void disrupted_history(const char *run_id, const struct etude_repeat_result *r,
                              const char *why, int disrupted)
{
    struct cmp_history h;
    struct etude_repeat_tally *t = NULL;
    char name[64], *path;
    size_t i, n = 0;

    snprintf(name, sizeof(name), "%s.jsonl", run_id);
    path = (char *)fix_path(name);
    if (cmp_load(&h, SCHEMA, &path, 1) != 0) {
        CHECK(0, run_id);
        return;
    }
    CHECK(h.note_count == 0, run_id);
    for (i = 0; i < r->nattempts; i++) {
        const struct eh_json *d = data_of(&h, r->attempts[i].record_id);
        const struct eh_json *events = eh_json_get(eh_json_get(d, "result"), "disruptions");

        CHECK(events != NULL && (events->child != NULL) == disrupted, run_id);
        if (disrupted)
            CHECK(events != NULL && events->child != NULL &&
                      strstr(events->child->string, why) != NULL,
                  run_id);
    }
    CHECK(etude_repeat_tally(&h, run_id, &t, &n) == 0 && n >= 1, run_id);
    for (i = 0; i < n; i++)
        CHECK(t[i].disrupted == (disrupted ? r->nattempts : 0) && !t[i].flaky, run_id);
    free(t);
    cmp_free(&h);
}

/**
 * The history of the race keeps each failure beside the retry that passed:
 * the retry names the attempt it repeats, the run's item has both and is a
 * failure, the comparison takes no retry as a sample, and the tally of the
 * runs finds the reader possibly flaky and the seeder not. The run's
 * repetition record reproduces its plan.
 */
static void test_history(void)
{
    char *paths[3] = {(char *)fix_path("race-bad.jsonl"), (char *)fix_path("race-again.jsonl"),
                      (char *)fix_path("race-good.jsonl")};
    struct cmp_history h;
    struct etude_repeat_tally *t = NULL;
    const struct etude_repeat_tally *rd, *sd;
    const struct etude_repeat_attempt *a, *b;
    const struct eh_json *d, *item, *plan, *ev;
    char *text, why[300];
    size_t n = 0, i;
    int found = 0;

    if (bad_run.nitems == 0 || cmp_load(&h, SCHEMA, paths, 3) != 0) {
        CHECK(0, "history");
        return;
    }
    CHECK(h.note_count == 0, "history");
    a = attempt(&bad_run, 0, 0);
    b = attempt(&bad_run, 0, 1);
    if (a != NULL && b != NULL) {
        d = data_of(&h, a->record_id);
        CHECK(d != NULL && strcmp(cmp_text(eh_json_get(d, "result"), "outcome"), "failure") == 0 &&
                  cmp_text(d, "previous_attempt_id") == NULL,
              "history");
        text = d != NULL ? stored(eh_json_get(d, "assertion_results")) : NULL;
        CHECK(text != NULL && strstr(text, "\"end\":\"exited\",\"value\":1") != NULL &&
                  strstr(text, "\"outcome\":\"fail\"") != NULL,
              "history");
        free(text);
        ev = eh_json_get(eh_json_get(d, "result"), "evidence");
        text = ev != NULL && ev->child != NULL ? stored(ev->child) : NULL;
        CHECK(text != NULL && strstr(text, "the state that seed makes is missing") != NULL,
              "history");
        free(text);
        d = data_of(&h, b->record_id);
        CHECK(d != NULL && cmp_text(d, "previous_attempt_id") != NULL &&
                  strcmp(cmp_text(d, "previous_attempt_id"), a->attempt_id) == 0 &&
                  strcmp(cmp_text(eh_json_get(d, "result"), "outcome"), "pass") == 0,
              "history");
        d = data_of(&h, bad_run.run_record);
        for (item = d != NULL ? eh_json_get(d, "items")->child : NULL; item; item = item->next) {
            const struct eh_json *recs = eh_json_get(item, "attempt_records");

            if (strncmp(cmp_text(item, "item_id"), a->attempt_id,
                        strlen(cmp_text(item, "item_id"))) != 0)
                continue;
            found = 1;
            CHECK(strcmp(cmp_text(item, "outcome"), "failure") == 0, "history");
            CHECK(recs->child != NULL && strcmp(recs->child->string, a->record_id) == 0 &&
                      recs->child->next != NULL &&
                      strcmp(recs->child->next->string, b->record_id) == 0,
                  "history");
        }
        CHECK(found, "history");
        CHECK(d != NULL && strcmp(cmp_text(eh_json_get(d, "result"), "outcome"), "failure") == 0 &&
                  strstr(cmp_text(eh_json_get(d, "result"), "reason"),
                         "possibly flaky: test.repeat.read on none") != NULL,
              "history");
        for (i = 0; i < h.sample_count; i++)
            if (strcmp(h.samples[i].attempt->id, b->record_id) == 0)
                CHECK(strstr(h.samples[i].reason, "retries attempt") != NULL, "history");
    }
    CHECK(etude_repeat_tally(&h, NULL, &t, &n) == 0 && n == 2, "tally");
    rd = tally(t, n, "test.repeat.read");
    sd = tally(t, n, "test.repeat.seed");
    CHECK(rd != NULL && rd->flaky && rd->failure == 2 * early && rd->retries == 2 * early &&
              rd->pass == 6,
          "tally");
    CHECK(sd != NULL && !sd->flaky && sd->pass == 6 && sd->failure == 0, "tally");
    free(t);
    CHECK(etude_repeat_tally(&h, "race-good", &t, &n) == 0 && n == 2, "tally");
    rd = tally(t, n, "test.repeat.read");
    CHECK(rd != NULL && !rd->flaky && rd->pass == 2 && rd->retries == 0, "tally");
    free(t);
    plan = data_of(&h, bad_run.plan_record);
    text = plan != NULL ? stored(eh_json_get(plan, "concurrency")) : NULL;
    CHECK(text != NULL && etude_repeat_check(text, RECORD, why, sizeof(why)) == 0 &&
              strstr(text, "\"run_id\":\"race-bad\"") != NULL,
          "history-record");
    free(text);
    cmp_free(&h);
}

/* ---- The budget. ---- */

/** Give integer member `name` of `o`, or -1 when it has none. */
static int64_t member(const struct eh_json *o, const char *name)
{
    const struct eh_json *v = eh_json_get(o, name);

    return v != NULL && v->type == 'n' ? v->number : -1;
}

/**
 * The budget of a run records the policy and the limits of the plan's
 * class under it, and the check of the budget by that policy passes the
 * run of the race that passed.
 */
static void test_budget(void)
{
    char *path = (char *)fix_path("race-good.jsonl");
    struct cmp_history h;
    struct etude_budget_judgement j;
    const struct eh_json *b;
    const char *d;

    if (good_run.nitems == 0 || cmp_load(&h, SCHEMA, &path, 1) != 0) {
        CHECK(0, "budget");
        return;
    }
    b = eh_json_get(data_of(&h, good_run.run_record), "budget");
    d = cmp_digest(b, "policy");
    CHECK(d != NULL && strcmp(d, policy.sha256) == 0, "budget");
    CHECK(member(b, "goal_us") == 600000000 && member(b, "growth_us") == 60000000 &&
              member(b, "ceiling_us") == 1200000000 && member(b, "cleanup_reserve_us") == 30000000,
          "budget");
    CHECK(etude_budget_judge(&h, &policy, "race-good", NULL, 0, &j) == 0, "budget");
    if (j.report.verdict != ETUDE_BUDGET_PASS)
        printf("etude-repeat-test: budget of race-good:\n%s",
               j.report.lines.data != NULL ? j.report.lines.data : "");
    CHECK(j.report.verdict == ETUDE_BUDGET_PASS && j.agrees && j.report.failed == 0 &&
              j.charged_known && j.cls == ETUDE_BUDGET_FOCUSED,
          "budget");
    etude_budget_report_free(&j.report);
    cmp_free(&h);
}

/* ---- Width and loads. ---- */

/** Keep each width attempt's actual result beside its retained history. */
static void width_result(const char *run_id, const struct etude_repeat_result *r)
{
    printf("etude-repeat-test: %s: outcome=%d items=%lu attempts=%lu cleanup=%d\n", run_id,
           r->outcome, (unsigned long)r->nitems, (unsigned long)r->nattempts, r->cleanup);
    for (size_t i = 0; i < r->nattempts; i++) {
        const struct etude_repeat_attempt *a = &r->attempts[i];
        printf("etude-repeat-test: %s: item=%lu try=%u outcome=%d end=%d value=%d cleanup=%d "
               "start=%llu end=%llu record=%s reason=%s disruption=%s\n",
               a->attempt_id, (unsigned long)a->item, a->try, a->outcome, a->end, a->value,
               a->cleanup, (unsigned long long)a->started_us, (unsigned long long)a->ended_us,
               a->record_id, a->reason, a->disruption);
    }
    printf("etude-repeat-test: history: %s/%s.jsonl\n", fix_dir(), run_id);
}

/** Require both independently started peers and their exact FIFO events. */
static void width_peer_check(unsigned met)
{
    size_t i;

    for (i = 0; i < 2; i++)
        CHECK(width_peers[i].starts == 1 && width_peers[i].ready == 1 &&
                  width_peers[i].met == met && width_peers[i].armed == 1 &&
                  width_peers[i].attempt == NULL,
              "width FIFO events and cleanup");
}

/**
 * Two cases meet at a FIFO: two at a time both exchange the byte; one at
 * a time each runs out of its gap after reporting readiness. The pair's
 * rendezvous is preparation, not a measure of host process startup speed.
 */
static void test_width(void)
{
    struct etude_repeat_case cases[2];
    struct etude_repeat_spec spec;
    struct etude_repeat_result r;
    size_t i;

    cases[0] = make_case(2, "pair-w", "none", 1, &SHORT_WD, CASE_RES);
    cases[1] = make_case(3, "pair-r", "none", 1, &SHORT_WD, CASE_RES);
    memset(&spec, 0, sizeof(spec));
    spec.seed = 5;
    spec.width = 2;
    spec.cases = cases;
    spec.ncases = 2;
    fresh_shared();
    memset(width_peers, 0, sizeof(width_peers));
    width_mode = 2;
    CHECK(run("width-2", &spec, CAP, 3, load_argv, &r) == 0, "width");
    width_result("width-2", &r);
    CHECK(r.outcome == ETUDE_REPEAT_PASS && r.nattempts == 2, "width");
    CHECK(r.cleanup == ETUDE_CLEANUP_CONFIRMED, "width");
    width_peer_check(1);
    for (i = 0; i < r.nattempts; i++)
        CHECK(r.attempts[i].outcome == ETUDE_REPEAT_PASS &&
                  r.attempts[i].cleanup == ETUDE_CLEANUP_CONFIRMED,
              "width");
    disrupted_history("width-2", &r, "", 0);
    etude_repeat_result_free(&r);
    spec.width = 1;
    fresh_shared();
    memset(width_peers, 0, sizeof(width_peers));
    width_mode = 1;
    CHECK(run("width-1", &spec, CAP, 3, load_argv, &r) == 0, "width");
    width_result("width-1", &r);
    CHECK(r.outcome == ETUDE_REPEAT_INCOMPLETE && r.nattempts == 2, "width");
    CHECK(r.cleanup == ETUDE_CLEANUP_CONFIRMED, "width");
    width_peer_check(0);
    for (i = 0; i < r.nattempts; i++)
        CHECK(r.attempts[i].outcome == ETUDE_REPEAT_INCOMPLETE &&
                  strstr(r.attempts[i].reason, "gap bound") != NULL &&
                  r.attempts[i].cleanup == ETUDE_CLEANUP_CONFIRMED,
              "width");
    disrupted_history("width-1", &r, "", 0);
    etude_repeat_result_free(&r);
    width_mode = 0;
}

/** Say whether the loads' scratch directory is empty. */
static int scratch_empty(void)
{
    return host_directory_count(fix_path("scratch"), NULL) == 0;
}

/**
 * A load of each kind runs beside a case that finds it reserved and
 * running in the ledger: ready before its case starts, ended after, its
 * cleanup confirmed and its file gone. A load never ready, one that ends
 * before its case, and one that cannot fit beside its case are told apart.
 */
static void test_load(void)
{
    struct etude_repeat_case cases[1];
    struct etude_repeat_spec spec;
    struct etude_repeat_result r;
    struct etude_repeat_item *it;
    struct stat st;
    size_t i, n;
    int kinds;

    cases[0] = make_case(4, "witness", "none", 3, &WD, CASE_RES);
    memset(&spec, 0, sizeof(spec));
    spec.width = 1;
    spec.load_percent = 100;
    spec.cases = cases;
    spec.ncases = 1;
    spec.loads = LOADS;
    spec.nloads = 3;
    for (spec.seed = 1; spec.seed < 1000; spec.seed++) {
        if (etude_repeat_plan(&spec, &it, &n) != 0)
            break;
        for (i = 0, kinds = 0; i < n; i++)
            if (it[i].load != ETUDE_REPEAT_NO_LOAD)
                kinds |= 1 << it[i].load;
        free(it);
        if (kinds == 7)
            break;
    }
    fresh_shared();
    CHECK(run("load", &spec, CAP, 3, load_argv, &r) == 0, "load");
    CHECK(r.outcome == ETUDE_REPEAT_PASS && r.nattempts == 3, "load");
    for (i = 0, kinds = 0; i < r.nattempts; i++) {
        const struct etude_repeat_attempt *a = &r.attempts[i];

        if (r.items[a->item].load == ETUDE_REPEAT_NO_LOAD) {
            CHECK(r.items[a->item].load != ETUDE_REPEAT_NO_LOAD, "load");
            continue;
        }
        kinds |= 1 << r.items[a->item].load;
        CHECK(a->outcome == ETUDE_REPEAT_PASS && a->disruption[0] == '\0', "load");
        CHECK(a->load_ready_us != 0 && a->load_ready_us <= a->started_us, "load");
        CHECK(a->load_ended_us >= a->ended_us, "load");
        CHECK(a->load_cleanup == ETUDE_CLEANUP_CONFIRMED, "load");
        if (LOADS[r.items[a->item].load].kind == ETUDE_LOAD_CPU)
            CHECK(a->load_cpu_us.quality != ETUDE_QUALITY_UNAVAILABLE && a->load_cpu_us.value > 0,
                  "load");
    }
    CHECK(kinds == 7, "load");
    CHECK(scratch_empty(), "load");
    etude_repeat_result_free(&r);

    cases[0] = make_case(5, "mark", "none", 1, &WD, CASE_RES);
    spec.retries = 1;
    fresh_shared();
    CHECK(run("load-broken", &spec, CAP, 3, broken_argv, &r) == 0, "load-broken");
    CHECK(r.outcome == ETUDE_REPEAT_INCOMPLETE && r.nattempts == 2, "load-broken");
    for (i = 0; i < r.nattempts; i++)
        CHECK(r.attempts[i].outcome == ETUDE_REPEAT_INCOMPLETE &&
                  strstr(r.attempts[i].reason, "was not ready") != NULL,
              "load-broken");
    CHECK(stat(shared("marked"), &st) != 0, "load-broken");
    disrupted_history("load-broken", &r, "was not ready", 1);
    etude_repeat_result_free(&r);

    cases[0] = make_case(6, "wait-load", "none", 1, &WD, CASE_RES);
    spec.retries = 0;
    fresh_shared();
    CHECK(run("load-short", &spec, CAP, 3, short_argv, &r) == 0, "load-short");
    CHECK(r.outcome == ETUDE_REPEAT_PASS && r.nattempts == 1, "load-short");
    CHECK(r.nattempts == 1 && strstr(r.attempts[0].disruption, "ended before its case") != NULL,
          "load-short");
    etude_repeat_result_free(&r);

    cases[0] = make_case(7, "mark", "none", 1, &WD, WIDE_RES);
    spec.retries = 1;
    spec.nloads = 1;
    fresh_shared();
    CHECK(run("load-wide", &spec, SMALL, 1, load_argv, &r) == 0, "load-wide");
    CHECK(r.outcome == ETUDE_REPEAT_INCOMPLETE && r.nattempts == 1, "load-wide");
    CHECK(r.nattempts == 1 && strstr(r.attempts[0].reason, "exceed") != NULL, "load-wide");
    CHECK(stat(shared("marked"), &st) != 0, "load-wide");
    disrupted_history("load-wide", &r, "exceed", 1);
    etude_repeat_result_free(&r);
}

/** Runner faults keep their causes as disruptions, including in history. */
static void test_disruptions(void)
{
    static const struct {
        const char *run_id, *why;
        enum result_fault fault;
    } rows[] = {{"end-lost", "not observed", LOST},
                {"end-lost-gap", "not observed", LOST_GAP},
                {"end-stopped", "a stop", STOPPED_END},
                {"case-cleanup", "its cleanup is unconfirmed", CASE_CLEANUP},
                {"load-cleanup", "the cleanup of its load is unconfirmed", LOAD_CLEANUP},
                {"failure-cleanup", "its cleanup is unconfirmed", FAILURE_CLEANUP},
                {"admit-range", "machine's limits", ORDINARY},
                {"admit-space", "only a person can release", ORDINARY},
                {"case-unstarted", "could not start", ORDINARY}};
    static const struct etude_ledger_res TOO_WIDE[] = {{"cpu_millis", 5000}};
    struct etude_repeat_case c;
    struct etude_repeat_spec spec;
    struct etude_repeat_result r;
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        c = make_case(0, "mark", "none", 1, &WD, CASE_RES);
        memset(&spec, 0, sizeof(spec));
        spec.width = 1;
        spec.cases = &c;
        spec.ncases = 1;
        spec.loads = LOADS;
        spec.nloads = 1;
        if (rows[i].fault == LOAD_CLEANUP)
            spec.load_percent = 100;
        if (strcmp(rows[i].run_id, "admit-range") == 0) {
            c.res = TOO_WIDE;
            spec.retries = 1;
        }
        if (strcmp(rows[i].run_id, "admit-space") == 0) {
            admission_errno = ENOSPC;
            spec.retries = 1;
        }
        if (strcmp(rows[i].run_id, "case-unstarted") == 0)
            argv_of[0][0] = "/nonexistent/etude-repeat-case";
        result_fault = rows[i].fault;
        fresh_shared();
        CHECK(run(rows[i].run_id, &spec, CAP, 3, load_argv, &r) == 0, rows[i].run_id);
        CHECK(r.nattempts == 1 &&
                  r.outcome == (result_fault == FAILURE_CLEANUP ? ETUDE_REPEAT_FAILURE
                                                                : ETUDE_REPEAT_INCOMPLETE),
              rows[i].run_id);
        CHECK(r.nattempts == 1 && strstr(r.attempts[0].disruption, rows[i].why) != NULL,
              rows[i].run_id);
        disrupted_history(rows[i].run_id, &r, rows[i].why, 1);
        etude_repeat_result_free(&r);
        result_fault = ORDINARY;
        admission_errno = 0;
    }
}

/** Leave through the fixtures' cleanup after a signal. */
static void stopped(int sig)
{
    printf("etude-repeat-test: fail: stopped by signal %d\n", sig);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--case") == 0)
        return case_main(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "--load") == 0)
        return etude_load_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "--broken-load") == 0)
        return 3;
    if (argc >= 2 && strcmp(argv[1], "--short-load") == 0)
        return short_load();
    if (realpath(argv[0], self) == NULL) {
        printf("etude-repeat-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    if (fixtures() != 0) {
        printf("etude-repeat-test: fail: the fixtures cannot be made\n");
        return 1;
    }
    test_rng();
    test_plan();
    test_record();
    test_race();
    test_history();
    test_budget();
    test_width();
    test_load();
    test_disruptions();
    etude_repeat_result_free(&bad_run);
    etude_repeat_result_free(&again_run);
    etude_repeat_result_free(&good_run);
    etude_budget_policy_free(&policy);
    free(case_env);
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_repeat_hosttest: pass\n");
    return 0;
}
