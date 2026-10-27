/*
 * repeat.c: the driver of shuffled and loaded repetitions on POSIX systems.
 * It runs a plan's attempts through the machine's ledger under their
 * watchdogs, each beside its load, retries those that did not pass, and
 * writes their results, the plan and the run to the history.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "budget.h"
#include "etude/repeat.h"
#include "posix.h"
#include "repeat_text.h"

/* How often a worker whose next attempt waits for room asks again, since
 * the ledger gives no notice when a claim ends. */
#define ASK_MS 100

/* The output kept of a load fixture. */
#define LOAD_OUTPUT 4096

/* The bytes of a JSON fragment the driver writes in one piece. */
#define PIECE 4096

static const char *const kind_names[] = {"cpu", "memory", "disk"};
static const char *const resource_names[] = {"cpu_millis", "memory_bytes", "disk_bytes"};
static const char *const outcome_names[] = {"pass", "failure", "incomplete", "cancelled"};

/* Where an attempt is. */
enum stage {
    QUEUED,  /* not yet admitted whole */
    LOADING, /* its load runs and has not said it is ready */
    RUNNING, /* its case runs */
    ENDING,  /* its case has ended and its load is being ended */
    DONE
};

/* The driver's own account of one attempt. */
struct slot {
    enum stage stage;
    struct etude_supervised *load, *cas;
    int load_claim, case_claim; /* a claim, or a request, is in the ledger */
    int load_admitted;          /* the load's is a claim, not a request */
    int load_ran, load_cancelled, load_done, ran, refused;
    char load_id[288];
    char amount[24];
    char **load_args;
    struct etude_watchdog_spec load_wd;
    struct etude_reading queued, admitted, done;
    int was_admitted;
    struct etude_pid keeper, process; /* the case's, for the scope */
};

/* One run of the driver. */
struct run {
    const struct etude_repeat_spec *spec;
    const struct etude_repeat_site *site;
    struct etude_repeat_result *r;
    struct etude_supervisor *sup;
    struct slot *slots; /* beside r->attempts */
    size_t *queue, nqueue, next;
    unsigned active, running, peak;
    uint64_t busy_us, last_us; /* the integral of running cases, and its last change */
    struct etude_reading start, first;
    int granted;
};

/** Give the item of attempt `a`'s case. */
static const struct etude_repeat_case *case_of(const struct run *x,
                                               const struct etude_repeat_attempt *a)
{
    return &x->spec->cases[x->r->items[a->item].case_index];
}

/** Give the load of attempt `a`, or NULL. */
static const struct etude_load *load_of(const struct run *x, const struct etude_repeat_attempt *a)
{
    size_t l = x->r->items[a->item].load;

    return l == ETUDE_REPEAT_NO_LOAD ? NULL : &x->spec->loads[l];
}

/** Give the reservation of load `l` in `res`. */
static void load_res(const struct etude_load *l, struct etude_ledger_res *res)
{
    res->name = resource_names[l->kind];
    res->amount = l->kind == ETUDE_LOAD_CPU ? l->amount * 1000u : l->amount;
}

/** Note a change of the count of running cases, for the useful concurrency. */
static void running_changed(struct run *x, int delta)
{
    uint64_t now = posix_now_us();

    if (x->last_us != 0)
        x->busy_us += (uint64_t)x->running * (now - x->last_us);
    x->last_us = now;
    x->running = (unsigned)((int)x->running + delta);
    if (x->running > x->peak)
        x->peak = x->running;
}

/** Note the first grant of capacity, where the run's time starts. */
static void granted(struct run *x, struct slot *s)
{
    if (!s->was_admitted) {
        etude_history_now(&s->admitted);
        s->was_admitted = 1;
    }
    if (!x->granted) {
        x->first = s->admitted;
        x->granted = 1;
    }
}

/** Give a blob reference to `bytes`, kept in the store, in `out`. */
static int keep(const struct run *x, const char *bytes, size_t n, struct cmp_text *out)
{
    char path[4200], hex[65];

    if (cmp_store(x->site->store, bytes, n, path, sizeof(path), hex) != 0)
        return -1;
    cmp_printf(out, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[", hex, (unsigned long)n);
    rep_string(out, path);
    cmp_printf(out, "]}");
    return out->failed ? -1 : 0;
}

/** Append measurement `m` of the executor as the history writes one. */
static void measure(struct cmp_text *t, const struct etude_measure *m)
{
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        cmp_printf(t, "{\"value\":null,\"reason\":");
        rep_string(t, m->method != NULL ? m->method : "it was not measured");
        cmp_printf(t, "}");
        return;
    }
    cmp_printf(t, "{\"value\":%llu,\"method\":", (unsigned long long)m->value);
    rep_string(t, m->method);
    cmp_printf(t, ",\"quality\":\"%s\",\"interval_us\":null,\"error_bound\":null}",
               m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound");
}

/** Append the time from `a` to `b` as a measurement. */
static int elapsed(struct cmp_text *t, const struct etude_reading *a, const struct etude_reading *b)
{
    struct etude_measurement m = etude_history_elapsed(a, b);
    char json[PIECE];

    if (etude_measurement_json(&m, json, sizeof(json)) != 0)
        return -1;
    cmp_printf(t, "%s", json);
    return 0;
}

/** Append an interval from `a` to `b`, or one with no end when `b` is NULL. */
static int interval(struct cmp_text *t, const struct run *x, const struct etude_reading *a,
                    const struct etude_reading *b, const char *why_open)
{
    struct etude_measurement m;
    char json[PIECE];

    if (b != NULL)
        m = etude_history_elapsed(a, b);
    else {
        memset(&m, 0, sizeof(m));
        m.reason = why_open;
    }
    if (etude_measurement_json(&m, json, sizeof(json)) != 0)
        return -1;
    cmp_printf(t, "{\"start_utc\":\"%s\",\"end_utc\":", a->utc);
    if (b != NULL && m.reason == NULL)
        cmp_printf(t, "\"%s\"", b->utc);
    else
        cmp_printf(t, "null");
    cmp_printf(t, ",\"clock\":{\"machine_id\":");
    rep_string(t, x->site->machine_id);
    cmp_printf(t, ",\"boot_id\":");
    rep_string(t, x->site->boot_id);
    cmp_printf(t, ",\"clock_id\":\"monotonic\",\"resolution_us\":%llu},\"start_us\":%llu,",
               (unsigned long long)a->resolution_us, (unsigned long long)a->monotonic_us);
    if (b != NULL && m.reason == NULL)
        cmp_printf(t, "\"end_us\":%llu", (unsigned long long)b->monotonic_us);
    else
        cmp_printf(t, "\"end_us\":null");
    cmp_printf(t, ",\"elapsed_us\":%s}", json);
    return 0;
}

/** Append record `data` of `kind` to the history; its identity in `id`. */
static int append(const struct run *x, const char *kind, const struct cmp_text *data, char *id,
                  size_t size)
{
    struct etude_reading now;

    if (data->failed || etude_history_now(&now) != 0 ||
        etude_history_record_id(x->site->history, id, size) != 0)
        return -1;
    return etude_history_append(x->site->history, id, now.utc, kind, data->data);
}

/** Write the repetition record and the plan record. */
static int write_plan(struct run *x)
{
    struct cmp_text t = {NULL, 0, 0, 0}, ref = {NULL, 0, 0, 0};
    char id[ETUDE_ID_MAX * 4];
    char *record;
    size_t i;
    unsigned k;
    int rc = -1;

    record = etude_repeat_record(x->spec, x->site->run_id, x->r->items, x->r->nitems);
    if (record == NULL || keep(x, record, strlen(record), &ref) != 0)
        goto out;
    cmp_printf(&t, "{%s,\"selection\":%s,\"concurrency\":%s,\"items\":[", x->site->plan_fixed,
               ref.data, ref.data);
    for (i = 0; i < x->r->nitems; i++) {
        const struct etude_repeat_item *it = &x->r->items[i];
        const struct etude_repeat_case *c = &x->spec->cases[it->case_index];

        if (etude_repeat_item_id(x->site->run_id, c, it->repetition, id, sizeof(id)) != 0)
            goto out;
        cmp_printf(&t, "%s{\"item_id\":", i ? "," : "");
        rep_string(&t, id);
        cmp_printf(&t, ",\"case_id\":");
        rep_string(&t, c->case_id);
        cmp_printf(&t, ",\"family_id\":");
        rep_string(&t, c->family_id);
        cmp_printf(&t, ",\"architecture\":");
        rep_string(&t, c->arch);
        cmp_printf(&t,
                   ",\"domain\":\"host\",\"workload\":%s,\"assertions\":%s,\"profile\":%s,"
                   "\"repetition\":%u,\"attempt_ids\":[",
                   c->workload, c->assertions, c->profile, it->repetition);
        for (k = 0; k <= x->spec->retries; k++)
            cmp_printf(&t, "%s\"%s:a%u\"", k ? "," : "", id, k);
        cmp_printf(&t, "]}");
    }
    cmp_printf(&t, "]}");
    rc = append(x, "plan", &t, x->r->plan_record, sizeof(x->r->plan_record));
out:
    free(record);
    free(t.data);
    free(ref.data);
    return rc;
}

/** Add an attempt of item `item`, the `try`th, to the end of the queue. */
static int enqueue(struct run *x, size_t item, unsigned try)
{
    struct etude_repeat_attempt *a = &x->r->attempts[x->r->nattempts];
    struct slot *s = &x->slots[x->r->nattempts];
    char id[256];
    const struct etude_repeat_case *c = &x->spec->cases[x->r->items[item].case_index];

    if (etude_repeat_item_id(x->site->run_id, c, x->r->items[item].repetition, id, sizeof(id)))
        return -1;
    memset(a, 0, sizeof(*a));
    memset(s, 0, sizeof(*s));
    a->item = item;
    a->try = try;
    snprintf(a->attempt_id, sizeof(a->attempt_id), "%s:a%u", id, try);
    snprintf(s->load_id, sizeof(s->load_id), "%s:load", a->attempt_id);
    if (strlen(s->load_id) > 255) {
        errno = ERANGE;
        return -1;
    }
    a->end = ETUDE_END_LOST;
    a->value = -1;
    a->cleanup = a->load_cleanup = ETUDE_CLEANUP_CONFIRMED;
    a->cpu_us.method = "the case did not run";
    a->load_cpu_us.method = "no load ran";
    s->stage = QUEUED;
    x->queue[x->nqueue++] = x->r->nattempts++;
    return 0;
}

/** End attempt `ai` before its case ran, as incomplete for `reason`. */
static void not_run(struct run *x, size_t ai, const char *reason)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];

    a->outcome = ETUDE_REPEAT_INCOMPLETE;
    snprintf(a->reason, sizeof(a->reason), "%s", reason);
    snprintf(a->disruption, sizeof(a->disruption), "%s", reason);
    a->end = ETUDE_END_STOPPED;
}

/** Release the claims of a case that never ran, and an unstarted load's. */
static void release_unrun(struct run *x, size_t ai)
{
    struct slot *s = &x->slots[ai];

    if (s->case_claim)
        etude_ledger_finish(x->site->ledger, x->r->attempts[ai].attempt_id,
                            ETUDE_CLEANUP_CONFIRMED);
    if (s->load_claim && s->load == NULL)
        etude_ledger_finish(x->site->ledger, s->load_id, ETUDE_CLEANUP_CONFIRMED);
    s->case_claim = 0;
    if (s->load == NULL)
        s->load_claim = 0;
}

/** Give the verdict path of claim `id` in `buf`, or NULL. */
static const char *verdict(const struct run *x, const char *id, char *buf, size_t size)
{
    return etude_ledger_verdict_path(x->site->ledger, id, buf, size) == 0 ? buf : NULL;
}

/** Start the case of attempt `ai`, whose claims are admitted. */
static void start_case(struct run *x, size_t ai)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];
    struct slot *s = &x->slots[ai];
    const struct etude_repeat_case *c = case_of(x, a);
    struct etude_exec_spec spec;
    char path[4200], reason[160];

    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = a->attempt_id;
    spec.argv = c->argv;
    spec.envp = c->envp;
    spec.cwd = c->cwd;
    spec.output_max = c->output_max;
    spec.grace_us = c->grace_us;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = x->site->ledger;
    spec.verdict = verdict(x, a->attempt_id, path, sizeof(path));
    if (etude_supervise_host(x->sup, &spec, c->wd, &s->cas) != 0) {
        snprintf(reason, sizeof(reason), "the case could not start: %s", strerror(errno));
        not_run(x, ai, reason);
        s->cas = NULL;
        release_unrun(x, ai);
        if (s->load != NULL) {
            etude_supervised_cancel(s->load);
            s->load_cancelled = 1;
            s->stage = ENDING;
        } else {
            s->stage = DONE;
        }
        return;
    }
    s->ran = 1;
    s->keeper = etude_supervised_result(s->cas)->proc->keeper;
    s->process = etude_supervised_result(s->cas)->proc->process;
    a->started_us = etude_supervised_result(s->cas)->proc->started_us;
    running_changed(x, 1);
    s->stage = RUNNING;
}

/** Start the load of attempt `ai`, whose claims are admitted. */
static void start_load(struct run *x, size_t ai)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];
    struct slot *s = &x->slots[ai];
    const struct etude_repeat_case *c = case_of(x, a);
    const struct etude_load *l = load_of(x, a);
    struct etude_exec_spec spec;
    char path[4200], reason[160];
    size_t n = 0, i;

    while (x->site->load_argv[n] != NULL)
        n++;
    s->load_args = calloc(n + 3, sizeof(*s->load_args));
    if (s->load_args == NULL) {
        not_run(x, ai, "the load could not start: memory ran out");
        release_unrun(x, ai);
        s->stage = DONE;
        return;
    }
    for (i = 0; i < n; i++)
        s->load_args[i] = x->site->load_argv[i];
    snprintf(s->amount, sizeof(s->amount), "%llu", (unsigned long long)l->amount);
    s->load_args[n] = (char *)kind_names[l->kind];
    s->load_args[n + 1] = s->amount;
    s->load_wd.case_us =
        x->site->load_ready_us + c->wd->case_us + c->grace_us + x->site->load_grace_us;
    s->load_wd.gap_us = s->load_wd.case_us;
    s->load_wd.stage_us[ETUDE_STAGE_PREPARE] = x->site->load_ready_us;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = s->load_id;
    spec.argv = s->load_args;
    spec.envp = x->site->load_envp;
    spec.cwd = x->site->scratch;
    spec.output_max = LOAD_OUTPUT;
    spec.grace_us = x->site->load_grace_us;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = x->site->ledger;
    spec.verdict = verdict(x, s->load_id, path, sizeof(path));
    if (etude_supervise_host(x->sup, &spec, &s->load_wd, &s->load) != 0) {
        snprintf(reason, sizeof(reason), "the load %s could not start: %s", l->load_id,
                 strerror(errno));
        s->load = NULL;
        not_run(x, ai, reason);
        release_unrun(x, ai);
        s->stage = DONE;
        return;
    }
    etude_supervised_stage(s->load, ETUDE_STAGE_PREPARE);
    s->load_ran = 1;
    s->stage = LOADING;
}

/** Say whether the load and the case of attempt `a` fit the machine. */
static int fits(const struct run *x, const struct etude_repeat_attempt *a)
{
    const struct etude_repeat_case *c = case_of(x, a);
    const struct etude_load *l = load_of(x, a);
    struct etude_ledger_res lr;
    size_t i, j;

    if (l == NULL)
        return 1;
    load_res(l, &lr);
    for (i = 0; i < x->site->ncap; i++) {
        uint64_t sum = strcmp(x->site->cap[i].name, lr.name) == 0 ? lr.amount : 0;

        for (j = 0; j < c->nres; j++)
            if (strcmp(x->site->cap[i].name, c->res[j].name) == 0)
                sum += c->res[j].amount;
        if (sum > x->site->cap[i].amount)
            return 0;
    }
    return 1;
}

/**
 * Ask the ledger for one claim of attempt `ai`.
 *
 * @return 1 when admitted, 0 when it waits, 2 when it is refused, which ends
 *         the attempt, and -1 with errno set for a ledger that failed.
 */
static int ask(struct run *x, size_t ai, const char *id, const struct etude_ledger_res *res,
               size_t nres, int *held)
{
    const struct etude_repeat_site *site = x->site;
    char reason[200];

    *held = 1;
    if (etude_ledger_admit(site->ledger, id, site->account, res, nres, site->cap, site->ncap, 1) ==
        0) {
        granted(x, &x->slots[ai]);
        return 1;
    }
    if (errno == EAGAIN)
        return 0;
    *held = 0;
    if (errno != ERANGE && errno != ENOSPC)
        return -1;
    snprintf(reason, sizeof(reason), "admission refused %s: %s", id,
             errno == ERANGE ? "the machine's limits cannot hold it"
                             : "it cannot fit beside claims only a person can release");
    not_run(x, ai, reason);
    x->slots[ai].refused = 1;
    release_unrun(x, ai);
    x->slots[ai].stage = DONE;
    return 2;
}

/**
 * Admit attempt `ai` at the head of the queue, its load first and then its
 * case, and start it.
 *
 * @return 1 when it started or ended, 0 when it waits for room, and -1 with
 *         errno set for a ledger that failed.
 */
static int admit(struct run *x, size_t ai)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];
    struct slot *s = &x->slots[ai];
    const struct etude_repeat_case *c = case_of(x, a);
    const struct etude_load *l = load_of(x, a);
    struct etude_ledger_res lr;
    int rc;

    if (s->queued.utc[0] == '\0')
        etude_history_now(&s->queued);
    if (!fits(x, a)) {
        not_run(x, ai, "the load and its case together exceed the machine's limits");
        s->refused = 1;
        s->stage = DONE;
        return 1;
    }
    if (l != NULL && !s->load_admitted) {
        load_res(l, &lr);
        rc = ask(x, ai, s->load_id, &lr, 1, &s->load_claim);
        if (rc != 1)
            return rc == 2 ? 1 : rc;
        s->load_admitted = 1;
    }
    rc = ask(x, ai, a->attempt_id, c->res, c->nres, &s->case_claim);
    if (rc != 1)
        return rc == 2 ? 1 : rc;
    if (l != NULL)
        start_load(x, ai);
    else
        start_case(x, ai);
    return 1;
}

/** Judge the case of attempt `ai`, which has finished. */
static void judge(struct run *x, size_t ai)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];
    const struct etude_supervised_result *res = etude_supervised_result(x->slots[ai].cas);

    a->end = res->end;
    a->value = res->value;
    a->cleanup = res->cleanup;
    a->cpu_us = res->proc->cpu_us;
    a->ended_us = res->finished_us;
    if (res->incomplete && res->expiry != ETUDE_EXPIRY_NONE) {
        a->outcome = ETUDE_REPEAT_INCOMPLETE;
        snprintf(a->reason, sizeof(a->reason), "its %s bound of %llu us ran out",
                 etude_expiry_name(res->expiry), (unsigned long long)res->bound_us);
    } else if (res->incomplete || res->end == ETUDE_END_LOST || res->end == ETUDE_END_STOPPED) {
        a->outcome = ETUDE_REPEAT_INCOMPLETE;
        snprintf(a->reason, sizeof(a->reason), "its end was %s",
                 res->end == ETUDE_END_STOPPED ? "a stop" : "not observed");
    } else if (res->end == ETUDE_END_EXITED && res->value == 0) {
        a->outcome =
            res->cleanup == ETUDE_CLEANUP_CONFIRMED ? ETUDE_REPEAT_PASS : ETUDE_REPEAT_INCOMPLETE;
        if (a->outcome != ETUDE_REPEAT_PASS)
            snprintf(a->reason, sizeof(a->reason), "its cleanup is unconfirmed");
    } else {
        a->outcome = ETUDE_REPEAT_FAILURE;
        snprintf(a->reason, sizeof(a->reason), "%s %d",
                 res->end == ETUDE_END_EXITED ? "it exited" : "it was killed by signal",
                 res->value);
    }
    if (a->disruption[0] == '\0' &&
        (res->end == ETUDE_END_LOST || (res->incomplete && res->expiry == ETUDE_EXPIRY_NONE) ||
         (res->end == ETUDE_END_STOPPED && res->expiry == ETUDE_EXPIRY_NONE)))
        snprintf(a->disruption, sizeof(a->disruption), "its end was %s",
                 res->end == ETUDE_END_STOPPED ? "a stop" : "not observed");
}

/** Append attempt `a`'s results document, kept in the store, to `t`. */
static int results(struct run *x, size_t ai, struct cmp_text *t)
{
    const struct etude_repeat_attempt *a = &x->r->attempts[ai];
    const struct etude_repeat_case *c = case_of(x, a);
    const struct slot *s = &x->slots[ai];
    struct cmp_text d = {NULL, 0, 0, 0};
    char item[ETUDE_ID_MAX * 4];
    const char *end, *by = "host_process", *check;
    int value = a->value, rc;

    etude_repeat_item_id(x->site->run_id, c, x->r->items[a->item].repetition, item, sizeof(item));
    end = a->end == ETUDE_END_EXITED     ? "exited"
          : a->end == ETUDE_END_SIGNALED ? "signaled"
          : a->end == ETUDE_END_STOPPED  ? "stopped"
                                         : "lost";
    if (a->end == ETUDE_END_STOPPED || a->end == ETUDE_END_LOST || !s->ran) {
        by = "executor";
        value = -1;
    }
    check = a->outcome == ETUDE_REPEAT_PASS      ? "pass"
            : a->outcome == ETUDE_REPEAT_FAILURE ? "fail"
                                                 : "missing";
    cmp_printf(&d, "{\"schema_version\":1,\"run_id\":");
    rep_string(&d, x->site->run_id);
    cmp_printf(&d, ",\"attempt_id\":");
    rep_string(&d, a->attempt_id);
    cmp_printf(&d, ",\"item_id\":");
    rep_string(&d, item);
    cmp_printf(&d, ",\"case_id\":");
    rep_string(&d, c->case_id);
    cmp_printf(&d,
               ",\"inventory_sha256\":\"%s\",\"placement\":\"host\",\"termination\":{\"end\":"
               "\"%s\",\"value\":",
               c->inventory_sha256, end);
    if (value >= 0 && value <= 255)
        cmp_printf(&d, "%d", value);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d, ",\"observed_by\":\"%s\",\"detail\":", by);
    if (a->reason[0] != '\0')
        rep_string(&d, a->reason);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d,
               "},\"report\":{\"state\":\"not_used\",\"records\":0,\"errors\":[]},"
               "\"branches\":[],\"assertions\":[{\"assertion_id\":\"%s\",\"source\":"
               "\"executor\",\"outcome\":\"%s\",\"detail\":",
               ETUDE_ASSERT_TERMINATION, check);
    if (a->outcome == ETUDE_REPEAT_PASS)
        cmp_printf(&d, "null");
    else
        rep_string(&d, a->reason);
    cmp_printf(&d, "}],\"undeclared\":[]}\n");
    rc = d.failed ? -1 : keep(x, d.data, d.length, t);
    free(d.data);
    return rc;
}

/** Append the resources of attempt `ai`: its scope, kept, and its time. */
static int attempt_resources(struct run *x, size_t ai, struct cmp_text *t)
{
    const struct etude_repeat_attempt *a = &x->r->attempts[ai];
    const struct slot *s = &x->slots[ai];
    struct cmp_text d = {NULL, 0, 0, 0};
    int rc;

    cmp_printf(&d, "{\"attempt_id\":");
    rep_string(&d, a->attempt_id);
    cmp_printf(&d, ",\"processes\":[");
    if (s->ran)
        cmp_printf(&d,
                   "{\"role\":\"keeper\",\"pid\":%ld,\"start\":%llu},{\"role\":\"case\","
                   "\"pid\":%ld,\"start\":%llu}",
                   s->keeper.pid, (unsigned long long)s->keeper.start, s->process.pid,
                   (unsigned long long)s->process.start);
    cmp_printf(&d, "],\"load\":");
    if (s->load_ran)
        rep_string(&d, s->load_id);
    else
        cmp_printf(&d, "null");
    cmp_printf(&d, "}\n");
    cmp_printf(t, "{\"scope\":");
    rc = d.failed ? -1 : keep(x, d.data, d.length, t);
    free(d.data);
    cmp_printf(t, ",\"tree_cpu_us\":");
    measure(t, &a->cpu_us);
    cmp_printf(t, ",\"combined_peak_bytes\":{\"value\":null,\"reason\":\"the driver samples no "
                  "combined memory\"},\"samples\":[]}");
    return rc;
}

/** Append the evidence of an attempt that did not pass: its output, kept. */
static int evidence(struct run *x, size_t ai, struct cmp_text *t)
{
    const struct slot *s = &x->slots[ai];
    const struct etude_exec_result *p;
    int rc = 0;

    cmp_printf(t, "[");
    if (x->r->attempts[ai].outcome != ETUDE_REPEAT_PASS && s->cas != NULL) {
        p = etude_supervised_result(s->cas)->proc;
        if (p->out.kept > 0)
            rc |= keep(x, p->out.data, p->out.kept, t);
        if (p->err.kept > 0) {
            cmp_printf(t, p->out.kept > 0 ? "," : "");
            rc |= keep(x, p->err.data, p->err.kept, t);
        }
    }
    cmp_printf(t, "]");
    return rc;
}

/** Write the attempt record of attempt `ai`, which is done. */
static int write_attempt(struct run *x, size_t ai)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];
    struct slot *s = &x->slots[ai];
    struct cmp_text t = {NULL, 0, 0, 0};
    char item[ETUDE_ID_MAX * 4];
    const struct etude_reading *from = s->was_admitted ? &s->admitted : &s->queued;
    int rc = 0;

    etude_repeat_item_id(x->site->run_id, case_of(x, a), x->r->items[a->item].repetition, item,
                         sizeof(item));
    cmp_printf(&t, "{\"plan_id\":");
    rep_string(&t, x->r->plan_record);
    cmp_printf(&t, ",\"item_id\":");
    rep_string(&t, item);
    cmp_printf(&t, ",\"attempt_id\":");
    rep_string(&t, a->attempt_id);
    cmp_printf(&t, ",\"previous_attempt_id\":");
    if (a->try > 0)
        cmp_printf(&t, "\"%s:a%u\"", item, a->try - 1);
    else
        cmp_printf(&t, "null");
    cmp_printf(&t, ",\"steps\":[],\"time\":");
    rc |= interval(&t, x, from, &s->done, NULL);
    cmp_printf(&t, ",\"queue_us\":");
    rc |= elapsed(&t, &s->queued, from);
    cmp_printf(&t, ",\"resources\":");
    rc |= attempt_resources(x, ai, &t);
    cmp_printf(&t, ",\"assertion_results\":");
    rc |= results(x, ai, &t);
    cmp_printf(&t, ",\"result\":{\"outcome\":\"%s\",\"reason\":", outcome_names[a->outcome]);
    if (a->outcome == ETUDE_REPEAT_PASS)
        cmp_printf(&t, "null");
    else
        rep_string(&t, a->reason);
    cmp_printf(&t, ",\"cleanup\":\"%s\",\"evidence\":",
               a->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed");
    rc |= evidence(x, ai, &t);
    cmp_printf(&t, ",\"capture_errors\":[],\"disruptions\":[");
    if (a->disruption[0] != '\0')
        rep_string(&t, a->disruption);
    cmp_printf(&t, "]}}");
    if (rc == 0)
        rc = append(x, "attempt", &t, a->record_id, sizeof(a->record_id));
    free(t.data);
    return rc;
}

/**
 * Finish attempt `ai`: write its record, and queue its retry when it did not
 * pass.
 */
static int complete(struct run *x, size_t ai)
{
    struct etude_repeat_attempt *a = &x->r->attempts[ai];
    struct slot *s = &x->slots[ai];
    int rc;

    if (a->load_cleanup != ETUDE_CLEANUP_CONFIRMED) {
        a->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
        if (a->disruption[0] == '\0')
            snprintf(a->disruption, sizeof(a->disruption),
                     "the cleanup of its load is unconfirmed");
        if (a->outcome == ETUDE_REPEAT_PASS) {
            a->outcome = ETUDE_REPEAT_INCOMPLETE;
            snprintf(a->reason, sizeof(a->reason), "the cleanup of its load is unconfirmed");
        }
    }
    if (a->cleanup != ETUDE_CLEANUP_CONFIRMED && a->disruption[0] == '\0')
        snprintf(a->disruption, sizeof(a->disruption), "its cleanup is unconfirmed");
    etude_history_now(&s->done);
    rc = write_attempt(x, ai);
    if (s->cas != NULL)
        etude_supervised_free(s->cas);
    if (s->load != NULL)
        etude_supervised_free(s->load);
    s->cas = s->load = NULL;
    x->active--;
    if (rc == 0 && a->outcome != ETUDE_REPEAT_PASS && !s->refused && a->try < x->spec->retries)
        rc = enqueue(x, a->item, a->try + 1);
    return rc;
}

/** Take the end of a load or a case that the supervisor gave. */
static int finished(struct run *x, struct etude_supervised *f)
{
    const struct etude_supervised_result *res = etude_supervised_result(f);
    struct etude_repeat_attempt *a;
    struct slot *s;
    char reason[96];
    size_t ai;

    for (ai = 0; ai < x->r->nattempts; ai++)
        if (x->slots[ai].load == f || x->slots[ai].cas == f)
            break;
    if (ai == x->r->nattempts) {
        etude_supervised_free(f);
        return 0;
    }
    a = &x->r->attempts[ai];
    s = &x->slots[ai];
    if (f == s->load) {
        s->load_done = 1;
        a->load_ended_us = res->finished_us;
        a->load_cleanup = res->cleanup;
        a->load_cpu_us = res->proc->cpu_us;
        etude_ledger_finish(x->site->ledger, s->load_id, res->cleanup);
        s->load_claim = 0;
        if (res->expiry != ETUDE_EXPIRY_NONE)
            snprintf(reason, sizeof(reason), "its %s bound of %llu us ran out",
                     etude_expiry_name(res->expiry), (unsigned long long)res->bound_us);
        else
            snprintf(reason, sizeof(reason), "it ended, %s %d",
                     res->end == ETUDE_END_EXITED ? "exit status" : "end", res->value);
        if (s->stage == LOADING) {
            char why[200];

            snprintf(why, sizeof(why), "the load %s was not ready: %s", load_of(x, a)->load_id,
                     reason);
            not_run(x, ai, why);
            release_unrun(x, ai);
            s->stage = DONE;
        } else if (s->stage == RUNNING && !s->load_cancelled) {
            snprintf(a->disruption, sizeof(a->disruption), "the load %s ended before its case: %s",
                     load_of(x, a)->load_id, reason);
        } else if (s->stage == ENDING) {
            s->stage = DONE;
        }
    } else {
        judge(x, ai);
        etude_ledger_finish(x->site->ledger, a->attempt_id, res->cleanup);
        s->case_claim = 0;
        running_changed(x, -1);
        if (s->load != NULL && !s->load_done) {
            etude_supervised_cancel(s->load);
            s->load_cancelled = 1;
            s->stage = ENDING;
        } else {
            s->stage = DONE;
        }
    }
    return s->stage == DONE ? complete(x, ai) : 0;
}

/** Start the case of each attempt whose load has said it is ready. */
static void check_ready(struct run *x)
{
    const struct etude_exec_result *p;
    size_t ai;

    for (ai = 0; ai < x->r->nattempts; ai++) {
        struct slot *s = &x->slots[ai];

        if (s->stage != LOADING)
            continue;
        p = etude_supervised_result(s->load)->proc;
        if (p->out.data == NULL || strstr(p->out.data, ETUDE_LOAD_READY) == NULL)
            continue;
        x->r->attempts[ai].load_ready_us = posix_now_us();
        etude_supervised_stage(s->load, ETUDE_STAGE_EXECUTE);
        start_case(x, ai);
    }
}

/**
 * Run the queue: admit the attempt at its head while fewer than `width` run,
 * start each case whose load is ready, and take each end.
 *
 * @return 0 once every attempt is done, or -1 with errno set.
 */
static int drive(struct run *x)
{
    struct etude_supervised *f;
    int rc, waiting;

    for (;;) {
        waiting = 0;
        while (x->active < x->spec->width && x->next < x->nqueue) {
            size_t ai = x->queue[x->next];

            rc = admit(x, ai);
            if (rc < 0)
                return -1;
            if (rc == 0) {
                waiting = 1;
                break;
            }
            x->next++;
            x->active++;
            if (x->slots[ai].stage == DONE && complete(x, ai) != 0)
                return -1;
        }
        if (x->next >= x->nqueue && x->active == 0)
            return 0;
        check_ready(x);
        if (x->active == 0) {
            poll(NULL, 0, ASK_MS);
            continue;
        }
        f = etude_supervise_wait(x->sup, waiting ? ASK_MS : -1);
        if (f != NULL && finished(x, f) != 0)
            return -1;
    }
}

/** End what runs after a failure, release or keep its claims, and withdraw
 * the requests that wait. */
static void abandon(struct run *x)
{
    struct etude_supervised *f;
    size_t ai, left = 0;

    for (ai = 0; ai < x->r->nattempts; ai++) {
        struct slot *s = &x->slots[ai];

        if (s->cas != NULL && s->stage == RUNNING) {
            etude_supervised_cancel(s->cas);
            left++;
        }
        if (s->load != NULL && !s->load_done) {
            etude_supervised_cancel(s->load);
            left++;
        }
    }
    while (left > 0) {
        f = etude_supervise_wait(x->sup, -1);
        if (f == NULL)
            continue;
        left--;
        for (ai = 0; ai < x->r->nattempts; ai++) {
            struct slot *s = &x->slots[ai];
            const char *id = f == s->load ? s->load_id : x->r->attempts[ai].attempt_id;

            if (f != s->load && f != s->cas)
                continue;
            etude_ledger_finish(x->site->ledger, id, etude_supervised_result(f)->cleanup);
            if (f == s->load)
                s->load_claim = 0;
            else
                s->case_claim = 0;
        }
    }
    for (ai = 0; ai < x->r->nattempts; ai++) {
        struct slot *s = &x->slots[ai];

        if (s->load_claim && etude_ledger_withdraw(x->site->ledger, s->load_id) != 0)
            etude_ledger_finish(x->site->ledger, s->load_id, ETUDE_CLEANUP_CONFIRMED);
        if (s->case_claim &&
            etude_ledger_withdraw(x->site->ledger, x->r->attempts[ai].attempt_id) != 0)
            etude_ledger_finish(x->site->ledger, x->r->attempts[ai].attempt_id,
                                ETUDE_CLEANUP_CONFIRMED);
    }
}

/** Give the rank of an outcome: pass, cancelled, incomplete, failure. */
static int rank(enum etude_repeat_outcome o)
{
    static const int ranks[] = {0, 3, 2, 1};

    return ranks[o];
}

/** Give a measurement of the history for one of the executor. */
static struct etude_measurement as_history(const struct etude_measure *m)
{
    struct etude_measurement h;

    memset(&h, 0, sizeof(h));
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        h.reason = m->method != NULL ? m->method : "it was not measured";
        return h;
    }
    h.value = m->value;
    h.method = m->method;
    h.quality = m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound";
    return h;
}

/** Append the run's totals of the execution stage and of each family. */
static int totals(struct run *x, struct cmp_text *t)
{
    struct cmp_text d = {NULL, 0, 0, 0}, f = {NULL, 0, 0, 0};
    uint64_t sum = 0;
    size_t i, c, n;
    int rc;

    cmp_printf(&d, "{\"schema_version\":1,\"stages\":[{\"stage\":\"execute\",\"members\":[");
    for (i = 0, n = 0; i < x->r->nattempts; i++)
        if (x->slots[i].ran) {
            cmp_printf(&d, n++ ? "," : "");
            rep_string(&d, x->r->attempts[i].record_id);
            sum += x->r->attempts[i].ended_us - x->r->attempts[i].started_us;
        }
    cmp_printf(&d, "],\"sum_us\":%llu,\"missing\":[", (unsigned long long)sum);
    for (i = 0, n = 0; i < x->r->nattempts; i++)
        if (!x->slots[i].ran) {
            cmp_printf(&d, n++ ? "," : "");
            cmp_printf(&d, "{\"record_id\":");
            rep_string(&d, x->r->attempts[i].record_id);
            cmp_printf(&d, ",\"reason\":\"the case did not run\"}");
        }
    cmp_printf(&d, "]}]}\n");
    cmp_printf(&f, "{\"schema_version\":1,\"families\":[");
    for (c = 0; c < x->spec->ncases; c++) {
        const char *family = x->spec->cases[c].family_id;

        for (i = 0; i < c && strcmp(x->spec->cases[i].family_id, family) != 0; i++) {
        }
        if (i < c)
            continue;
        cmp_printf(&f, c ? ",{\"family_id\":" : "{\"family_id\":");
        rep_string(&f, family);
        cmp_printf(&f, ",\"members\":[");
        for (i = 0, n = 0, sum = 0; i < x->r->nattempts; i++) {
            if (!x->slots[i].ran || strcmp(case_of(x, &x->r->attempts[i])->family_id, family))
                continue;
            cmp_printf(&f, n++ ? "," : "");
            rep_string(&f, x->r->attempts[i].record_id);
            sum += x->r->attempts[i].ended_us - x->r->attempts[i].started_us;
        }
        cmp_printf(&f, "],\"sum_us\":%llu}", (unsigned long long)sum);
    }
    cmp_printf(&f, "]}\n");
    cmp_printf(t, ",\"stage_totals\":");
    rc = d.failed ? -1 : keep(x, d.data, d.length, t);
    cmp_printf(t, ",\"family_totals\":");
    rc |= f.failed ? -1 : keep(x, f.data, f.length, t);
    free(d.data);
    free(f.data);
    return rc;
}

/** Append the run's useful concurrency and the loads it applied. */
static int observed(struct run *x, struct cmp_text *t)
{
    struct cmp_text u = {NULL, 0, 0, 0}, l = {NULL, 0, 0, 0};
    uint64_t now = posix_now_us();
    size_t i, n;
    int rc;

    cmp_printf(&u,
               "{\"schema_version\":1,\"method\":\"driver-transitions-v1\",\"integral_us\":"
               "%llu,\"peak\":%u,\"elapsed_us\":%llu}\n",
               (unsigned long long)x->busy_us, x->peak,
               (unsigned long long)(x->granted ? now - x->first.monotonic_us : 0));
    cmp_printf(&l, "{\"schema_version\":1,\"outside\":{\"value\":null,\"reason\":\"the driver "
                   "does not observe other work\"},\"loads\":[");
    for (i = 0, n = 0; i < x->r->nattempts; i++) {
        const struct etude_repeat_attempt *a = &x->r->attempts[i];
        const struct etude_load *ld = load_of(x, a);

        if (!x->slots[i].load_ran)
            continue;
        cmp_printf(&l, n++ ? ",{\"attempt_id\":" : "{\"attempt_id\":");
        rep_string(&l, a->attempt_id);
        cmp_printf(&l, ",\"load_id\":");
        rep_string(&l, ld->load_id);
        cmp_printf(&l,
                   ",\"kind\":\"%s\",\"amount\":%llu,\"ready_us\":%llu,\"ended_us\":%llu,"
                   "\"cleanup\":\"%s\",\"tree_cpu_us\":",
                   kind_names[ld->kind], (unsigned long long)ld->amount,
                   (unsigned long long)a->load_ready_us, (unsigned long long)a->load_ended_us,
                   a->load_cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed");
        measure(&l, &a->load_cpu_us);
        cmp_printf(&l, "}");
    }
    cmp_printf(&l, "]}\n");
    cmp_printf(t, ",\"useful_concurrency\":");
    rc = u.failed ? -1 : keep(x, u.data, u.length, t);
    cmp_printf(t, ",\"load\":");
    rc |= l.failed ? -1 : keep(x, l.data, l.length, t);
    free(u.data);
    free(l.data);
    return rc;
}

/** Append the run's resources: its scope, kept, and its processor time. */
static int run_resources(struct run *x, struct cmp_text *t)
{
    struct cmp_text d = {NULL, 0, 0, 0};
    struct etude_measurement *m, sum;
    char json[PIECE];
    size_t i, n = 0;
    int rc;

    m = calloc(2 * x->r->nattempts + 1, sizeof(*m));
    if (m == NULL)
        return -1;
    cmp_printf(&d, "{\"run_id\":");
    rep_string(&d, x->site->run_id);
    cmp_printf(&d, ",\"attempts\":[");
    for (i = 0; i < x->r->nattempts; i++) {
        cmp_printf(&d, i ? "," : "");
        rep_string(&d, x->r->attempts[i].record_id);
        if (x->slots[i].ran)
            m[n++] = as_history(&x->r->attempts[i].cpu_us);
        if (x->slots[i].load_ran)
            m[n++] = as_history(&x->r->attempts[i].load_cpu_us);
    }
    cmp_printf(&d, "]}\n");
    sum = etude_history_cpu_sum(m, n);
    free(m);
    cmp_printf(t, "{\"scope\":");
    rc = d.failed ? -1 : keep(x, d.data, d.length, t);
    free(d.data);
    if (etude_measurement_json(&sum, json, sizeof(json)) != 0)
        return -1;
    cmp_printf(t,
               ",\"tree_cpu_us\":%s,\"combined_peak_bytes\":{\"value\":null,\"reason\":\"the "
               "driver samples no combined memory\"},\"samples\":[]}",
               json);
    return rc;
}

/** Write into `out` why the run did not pass: its counts and flaky cases. */
static void run_reason(const struct run *x, char *out, size_t size)
{
    unsigned counts[4] = {0, 0, 0, 0};
    size_t i, j, used;

    for (i = 0; i < x->r->nattempts; i++)
        counts[x->r->attempts[i].outcome]++;
    used = (size_t)snprintf(out, size, "of %lu attempts, %u failed, %u did not finish",
                            (unsigned long)x->r->nattempts, counts[ETUDE_REPEAT_FAILURE],
                            counts[ETUDE_REPEAT_INCOMPLETE] + counts[ETUDE_REPEAT_CANCELLED]);
    for (i = 0; i < x->spec->ncases && used < size; i++) {
        int pass = 0, other = 0;

        for (j = 0; j < x->r->nattempts; j++) {
            const struct etude_repeat_attempt *a = &x->r->attempts[j];

            if (x->r->items[a->item].case_index != i || a->disruption[0] != '\0')
                continue;
            pass |= a->outcome == ETUDE_REPEAT_PASS;
            other |= a->outcome == ETUDE_REPEAT_FAILURE || a->outcome == ETUDE_REPEAT_INCOMPLETE;
        }
        if (pass && other)
            used += (size_t)snprintf(out + used, size - used, "; possibly flaky: %s on %s",
                                     x->spec->cases[i].case_id, x->spec->cases[i].arch);
    }
}

/** Write the run record, with each item's attempts and the worst outcome. */
static int write_run(struct run *x)
{
    struct cmp_text t = {NULL, 0, 0, 0};
    struct etude_reading end;
    const struct etude_reading *from = x->granted ? &x->first : &x->start;
    char id[ETUDE_ID_MAX * 4], reason[1024];
    enum etude_repeat_outcome worst = ETUDE_REPEAT_PASS, o;
    int rc = 0, open;
    size_t i, j, n;

    running_changed(x, 0);
    x->r->cleanup = ETUDE_CLEANUP_CONFIRMED;
    for (j = 0; j < x->r->nattempts; j++)
        if (x->r->attempts[j].cleanup != ETUDE_CLEANUP_CONFIRMED)
            x->r->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    open = x->r->cleanup != ETUDE_CLEANUP_CONFIRMED;
    cmp_printf(&t, "{\"plan_id\":");
    rep_string(&t, x->r->plan_record);
    cmp_printf(&t, ",\"items\":[");
    for (i = 0; i < x->r->nitems; i++) {
        const struct etude_repeat_item *it = &x->r->items[i];

        etude_repeat_item_id(x->site->run_id, &x->spec->cases[it->case_index], it->repetition, id,
                             sizeof(id));
        cmp_printf(&t, "%s{\"item_id\":", i ? "," : "");
        rep_string(&t, id);
        cmp_printf(&t, ",\"attempt_records\":[");
        o = ETUDE_REPEAT_PASS;
        for (j = 0, n = 0; j < x->r->nattempts; j++) {
            if (x->r->attempts[j].item != i)
                continue;
            cmp_printf(&t, n++ ? "," : "");
            rep_string(&t, x->r->attempts[j].record_id);
            if (rank(x->r->attempts[j].outcome) > rank(o))
                o = x->r->attempts[j].outcome;
        }
        if (rank(o) > rank(worst))
            worst = o;
        cmp_printf(&t, "],\"outcome\":\"%s\"}", outcome_names[o]);
    }
    x->r->outcome = worst;
    cmp_printf(&t, "],\"preparation_steps\":[],\"time\":");
    if (etude_history_now(&end) != 0)
        return -1;
    rc |= interval(&t, x, from, open ? NULL : &end,
                   "a cleanup is unconfirmed, so the run has not ended");
    cmp_printf(&t, ",\"resources\":");
    rc |= run_resources(x, &t);
    rc |= totals(x, &t);
    rc |= observed(x, &t);
    cmp_printf(&t, ",\"budget\":{\"policy\":%s,\"charged_us\":", x->site->budget_policy);
    if (open)
        cmp_printf(&t, "{\"value\":null,\"reason\":\"the run has not ended\"}");
    else
        rc |= elapsed(&t, from, &end);
    cmp_printf(&t,
               ",\"outside_queue_us\":{\"value\":0,\"method\":\"repeat-driver-v1\",\"quality\":"
               "\"exact\",\"interval_us\":null,\"error_bound\":0},\"one_time_setup_us\":"
               "{\"value\":0,\"method\":\"repeat-driver-v1\",\"quality\":\"exact\","
               "\"interval_us\":null,\"error_bound\":0},\"goal_us\":%llu,\"growth_us\":%llu,"
               "\"ceiling_us\":%llu,\"cleanup_reserve_us\":%llu,\"verdict\":\"unknown\"}",
               (unsigned long long)x->site->budget_limits->goal_us,
               (unsigned long long)x->site->budget_limits->growth_us,
               (unsigned long long)x->site->budget_limits->ceiling_us,
               (unsigned long long)x->site->budget_limits->reserve_us);
    cmp_printf(&t, ",\"result\":{\"outcome\":\"%s\",\"reason\":", outcome_names[worst]);
    run_reason(x, reason, sizeof(reason));
    if (worst == ETUDE_REPEAT_PASS)
        cmp_printf(&t, "null");
    else
        rep_string(&t, reason);
    cmp_printf(&t, ",\"cleanup\":\"%s\",\"evidence\":[],\"capture_errors\":[],\"disruptions\":[",
               open ? "unconfirmed" : "confirmed");
    for (j = 0, n = 0; j < x->r->nattempts; j++) {
        char text[ETUDE_ID_MAX * 4 + 256];

        if (x->r->attempts[j].disruption[0] == '\0')
            continue;
        snprintf(text, sizeof(text), "%s: %s", x->r->attempts[j].attempt_id,
                 x->r->attempts[j].disruption);
        cmp_printf(&t, n++ ? "," : "");
        rep_string(&t, text);
    }
    cmp_printf(&t, "]}}");
    if (rc == 0)
        rc = append(x, "run", &t, x->r->run_record, sizeof(x->r->run_record));
    free(t.data);
    return rc;
}

int etude_repeat_run(const struct etude_repeat_spec *spec, const struct etude_repeat_site *site,
                     struct etude_repeat_result *r)
{
    struct run x;
    size_t i, max;
    int rc = -1, e = 0;

    memset(&x, 0, sizeof(x));
    memset(r, 0, sizeof(*r));
    if (spec->width == 0 || site->budget_limits == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (etude_repeat_plan(spec, &r->items, &r->nitems) != 0)
        return -1;
    x.spec = spec;
    x.site = site;
    x.r = r;
    if (r->nitems > SIZE_MAX / sizeof(struct slot) / ((size_t)spec->retries + 1)) {
        etude_repeat_result_free(r);
        errno = EOVERFLOW;
        return -1;
    }
    max = r->nitems * ((size_t)spec->retries + 1);
    r->attempts = calloc(max, sizeof(*r->attempts));
    x.slots = calloc(max, sizeof(*x.slots));
    x.queue = calloc(max, sizeof(*x.queue));
    x.sup = etude_supervisor_new();
    if (r->attempts == NULL || x.slots == NULL || x.queue == NULL || x.sup == NULL ||
        etude_history_now(&x.start) != 0) {
        e = ENOMEM;
        goto out;
    }
    if (write_plan(&x) != 0) {
        e = errno != 0 ? errno : EIO;
        goto out;
    }
    for (i = 0; i < r->nitems; i++)
        if (enqueue(&x, i, 0) != 0) {
            e = errno;
            goto out;
        }
    if (drive(&x) != 0) {
        e = errno != 0 ? errno : EIO;
        abandon(&x);
        goto out;
    }
    if (write_run(&x) != 0) {
        e = errno != 0 ? errno : EIO;
        goto out;
    }
    rc = 0;
out:
    for (i = 0; x.slots != NULL && i < r->nattempts; i++)
        free(x.slots[i].load_args);
    if (x.sup != NULL)
        etude_supervisor_free(x.sup);
    free(x.slots);
    free(x.queue);
    if (rc != 0) {
        etude_repeat_result_free(r);
        errno = e;
    }
    return rc;
}

void etude_repeat_result_free(struct etude_repeat_result *r)
{
    free(r->items);
    free(r->attempts);
    memset(r, 0, sizeof(*r));
}
