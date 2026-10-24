/*
 * budget.c: the budget policy, and the forecast of a selection's charged
 * time from the costs its history records, on the site's reference
 * capacity under the schedule's own rule of admission.
 */
#include "budget.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../utils/sha256.h"
#include "cost.h"
#include "digest.h"
#include "schema.h"
#ifdef _WIN32
#include "win/files.h"
#endif

/* The largest policy, schema or case manifest read. */
#define DOCUMENT_LIMIT (UINT64_C(16) << 20)

/* The leading fields of a sample's key that a selected item fixes: the
 * case, architecture, placement, machine, accelerator, workload, inventory,
 * profile and system. The rest come from the newest sample that has them. */
#define ITEM_FIELDS 9

/* The longest item identity, CASE:ARCH:rN, and node target. */
#define NAME_MAX_BYTES 600

static const char *const class_names[ETUDE_BUDGET_CLASSES] = {"push", "ci", "release", "focused",
                                                              "phase_first_run"};
static const char *const verdict_names[] = {"pass", "failure", "exempt", "unknown"};
static const char *const resource_names[ETUDE_BUDGET_RESOURCES] = {"cpu_millis", "memory_bytes",
                                                                   "disk_bytes"};

/** Give the class of the purpose `name`, or -1 when it names none. */
int etude_budget_class_of(const char *name)
{
    int i;

    for (i = 0; name != NULL && i < ETUDE_BUDGET_CLASSES; i++) {
        if (strcmp(name, class_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/** Give the name of class `cls`, the purpose of its plans. */
const char *etude_budget_class_name(enum etude_budget_class cls)
{
    return class_names[cls];
}

/** Give the name of `verdict`, as the history's budget records it. */
const char *etude_budget_verdict_name(enum etude_budget_verdict verdict)
{
    return verdict_names[verdict];
}

/** Free the lines of a report. */
void etude_budget_report_free(struct etude_budget_report *r)
{
    free(r->lines.data);
    memset(&r->lines, 0, sizeof(r->lines));
}

/* ---- The policy. ---- */

/* The first fault a schema check reports. */
struct first_fault {
    char text[400];
    int seen;
};

/** Keep the first fault of a schema check. */
static void keep_fault(void *ctx, int line, const char *path, const char *what)
{
    struct first_fault *f = ctx;

    if (!f->seen) {
        snprintf(f->text, sizeof(f->text), "line %d: %s: %s", line, path, what);
        f->seen = 1;
    }
}

/** Read the JSON document of `path` into `*doc`; its bytes stay in `*bytes`. */
static int read_document(const char *path, char **bytes, size_t *length, struct json **doc,
                         char *err, size_t errlen)
{
    char why[200];

    *doc = NULL;
    if (cmp_read_file(path, DOCUMENT_LIMIT, bytes, length) != 0) {
        snprintf(err, errlen, "%s: cannot read", path);
        return -1;
    }
    if (json_parse(*bytes, *length, 1, doc, why, sizeof(why)) != 0) {
        snprintf(err, errlen, "%s: %s", path, why);
        return -1;
    }
    return 0;
}

/** Check `doc` of `path` against the schema `NAME.schema.json` of `schemas`. */
static int check_shape(const char *schemas, const char *name, const struct json *doc,
                       const char *path, char *err, size_t errlen)
{
    char file[1200], *bytes = NULL;
    struct json *schema = NULL;
    struct first_fault f = {{0}, 0};
    size_t length;
    int rc = -1;

    snprintf(file, sizeof(file), "%s/%s.schema.json", schemas, name);
    if (read_document(file, &bytes, &length, &schema, err, errlen) == 0) {
        schema_check(schema, doc, keep_fault, &f);
        if (f.seen) {
            snprintf(err, errlen, "%s: %s", path, f.text);
        } else {
            rc = 0;
        }
    }
    json_free(schema);
    free(bytes);
    return rc;
}

/** Give the integer member `name` of `object`, which the schema requires. */
static uint64_t integer_of(const struct json *object, const char *name)
{
    const struct json *v = json_member(object, name);

    return v != NULL && v->type == JSON_INTEGER && v->integer > 0 ? (uint64_t)v->integer : 0;
}

/**
 * Take the site and the limits of each class from the checked document, and
 * refuse limits that do not nest: a goal past its ceiling, a goal and growth
 * past it, or a cleanup reserve that leaves no time before it.
 */
static int take_policy(struct etude_budget_policy *p, const char *path, char *err, size_t errlen)
{
    const struct json *site = json_member(p->doc, "site");
    const struct json *capacity = json_member(site, "capacity");
    const struct json *classes = json_member(p->doc, "classes");
    size_t i;

    p->machine_id = json_text(site, "machine_id");
    p->os = json_text(site, "os");
    p->accelerator = json_text(site, "accelerator");
    for (i = 0; i < ETUDE_BUDGET_RESOURCES; i++) {
        if (json_member(capacity, resource_names[i]) != NULL) {
            p->capacity[p->ncapacity].name = resource_names[i];
            p->capacity[p->ncapacity++].amount = integer_of(capacity, resource_names[i]);
        }
    }
    p->one_time = json_member(p->doc, "one_time_setup");
    for (i = 0; i < ETUDE_BUDGET_CLASSES; i++) {
        const struct json *c = json_member(classes, class_names[i]);
        struct etude_budget_limits *l = &p->limits[i];

        l->goal_us = integer_of(c, "goal_us");
        l->growth_us = (uint64_t)json_member(c, "growth_us")->integer;
        l->ceiling_us = integer_of(c, "ceiling_us");
        l->reserve_us = integer_of(c, "cleanup_reserve_us");
        if (l->goal_us > l->ceiling_us || l->growth_us > l->ceiling_us - l->goal_us ||
            l->reserve_us >= l->ceiling_us) {
            snprintf(err, errlen,
                     "%s: the limits of %s do not nest: the goal and growth must be within the "
                     "ceiling, and the cleanup reserve below it",
                     path, class_names[i]);
            return -1;
        }
    }
    return 0;
}

/**
 * Read the budget policy at `path` into `p`, checked against
 * `SCHEMAS/budget.schema.json` and by the rules of take_policy, with the
 * SHA-256 and size of its bytes, which a run's budget names.
 *
 * @return 0, or -1 with the reason in `err`; `p` is then empty.
 */
int etude_budget_policy_read(const char *schemas, const char *path, struct etude_budget_policy *p,
                             char *err, size_t errlen)
{
    char *bytes = NULL;
    size_t length = 0;
    struct sha256 sha;
    unsigned char digest[32];
    int rc = -1;

    memset(p, 0, sizeof(*p));
    if (read_document(path, &bytes, &length, &p->doc, err, errlen) == 0 &&
        check_shape(schemas, "budget", p->doc, path, err, errlen) == 0 &&
        take_policy(p, path, err, errlen) == 0) {
        sha256_init(&sha);
        sha256_update(&sha, (const unsigned char *)bytes, length);
        sha256_final(&sha, digest);
        sha256_hex(digest, p->sha256);
        p->size_bytes = length;
        rc = 0;
    }
    free(bytes);
    if (rc != 0) {
        etude_budget_policy_free(p);
    }
    return rc;
}

/**
 * Read the case manifest at `path` into `*doc`, checked against
 * `SCHEMAS/case.schema.json`.
 *
 * @return 0, or -1 with the reason in `err` and `*doc` NULL.
 */
int etude_budget_manifest_read(const char *schemas, const char *path, struct json **doc, char *err,
                               size_t errlen)
{
    char *bytes = NULL;
    size_t length = 0;
    int rc = -1;

    if (read_document(path, &bytes, &length, doc, err, errlen) == 0 &&
        check_shape(schemas, "case", *doc, path, err, errlen) == 0) {
        rc = 0;
    }
    free(bytes);
    if (rc != 0) {
        json_free(*doc);
        *doc = NULL;
    }
    return rc;
}

/** Free a policy that etude_budget_policy_read filled. */
void etude_budget_policy_free(struct etude_budget_policy *p)
{
    json_free(p->doc);
    memset(p, 0, sizeof(*p));
}

/* ---- The schedule. ---- */

/** Give job `j`'s amount of resource `name`, 0 when it takes none. */
static uint64_t amount_of(const struct etude_job *j, const char *name)
{
    size_t i;

    for (i = 0; j != NULL && i < j->nres; i++) {
        if (strcmp(j->res[i].name, name) == 0) {
            return j->res[i].amount;
        }
    }
    return 0;
}

/**
 * Say whether job `j` fits in what is `left` of each resource of `cap`, and
 * leaves free, of each resource it takes, what the protected job `keep`
 * needs of it, as the ledger admits a claim beside its protected request.
 */
static int fits(const struct etude_job *j, const uint64_t *left, const struct etude_job *keep,
                const struct etude_ledger_res *cap, size_t ncap)
{
    size_t k;

    for (k = 0; k < ncap; k++) {
        uint64_t want = amount_of(j, cap[k].name), kept = amount_of(keep, cap[k].name);

        if (want > left[k] || (want > 0 && kept > left[k] - want)) {
            return 0;
        }
    }
    return 1;
}

/** Mark each job too large for the capacity refused, and the rest pending. */
static void refuse_large(struct etude_job *jobs, size_t n, const struct etude_ledger_res *cap,
                         size_t ncap)
{
    size_t i, k;

    for (i = 0; i < n; i++) {
        jobs[i].state = ETUDE_JOB_PENDING;
        jobs[i].error = 0;
        for (k = 0; k < ncap; k++) {
            if (amount_of(&jobs[i], cap[k].name) > cap[k].amount) {
                jobs[i].state = ETUDE_JOB_REFUSED;
                jobs[i].error = ERANGE;
            }
        }
    }
}

/* A schedule as etude_budget_schedule runs it. */
struct timeline {
    struct etude_job *jobs;
    size_t n;
    const struct etude_ledger_res *cap;
    size_t ncap;
    uint64_t *left; /* what is free of each resource of `cap` */
    uint64_t *end;  /* when each admitted job ends */
    char *ended;    /* each admitted job that has ended */
    uint64_t now;
};

/** Admit each pending job that fits now, in order; 0, or -1 with EOVERFLOW. */
static int admit_all(struct timeline *tl)
{
    const struct etude_job *keep = NULL;
    size_t i, k;

    for (i = 0; i < tl->n; i++) {
        struct etude_job *j = &tl->jobs[i];

        if (j->state != ETUDE_JOB_PENDING) {
            continue;
        }
        if (!fits(j, tl->left, keep, tl->cap, tl->ncap)) {
            keep = keep != NULL ? keep : j;
            continue;
        }
        if (j->cost_us > UINT64_MAX - tl->now) {
            errno = EOVERFLOW;
            return -1;
        }
        j->state = ETUDE_JOB_ADMITTED;
        tl->end[i] = tl->now + j->cost_us;
        for (k = 0; k < tl->ncap; k++) {
            tl->left[k] -= amount_of(j, tl->cap[k].name);
        }
    }
    return 0;
}

/**
 * Move the time now to the first end of a running job, and end each job
 * that ends then, giving back what it took. 0 when no job runs.
 */
static int end_next(struct timeline *tl)
{
    size_t i, k;
    int running = 0;

    for (i = 0; i < tl->n; i++) {
        if (tl->jobs[i].state == ETUDE_JOB_ADMITTED && !tl->ended[i] &&
            (!running || tl->end[i] < tl->now)) {
            tl->now = tl->end[i];
            running = 1;
        }
    }
    for (i = 0; running && i < tl->n; i++) {
        if (tl->jobs[i].state == ETUDE_JOB_ADMITTED && !tl->ended[i] && tl->end[i] == tl->now) {
            tl->ended[i] = 1;
            for (k = 0; k < tl->ncap; k++) {
                tl->left[k] += amount_of(&tl->jobs[i], tl->cap[k].name);
            }
        }
    }
    return running;
}

/**
 * Forecast the time the `n` jobs take on a machine of the capacity `cap`, as
 * one worker's schedule runs them: in the order etude_sched_order gives,
 * which sorts `jobs` in place, each admitted when it fits beside those
 * running and leaves free what the first job refused needs, so that no
 * cheaper job passes a costlier one that waits, and each running for its
 * `cost_us`. A job the capacity alone cannot hold is marked
 * ETUDE_JOB_REFUSED with the error ERANGE and does not run; the others end
 * ETUDE_JOB_ADMITTED.
 *
 * @return 0 with the end of the last job in `*makespan_us`, or -1 with errno
 *         ENOMEM or EOVERFLOW.
 */
int etude_budget_schedule(struct etude_job *jobs, size_t n, const struct etude_ledger_res *cap,
                          size_t ncap, uint64_t *makespan_us)
{
    struct timeline tl;
    size_t k;
    int rc = -1;

    memset(&tl, 0, sizeof(tl));
    tl.jobs = jobs;
    tl.n = n;
    tl.cap = cap;
    tl.ncap = ncap;
    tl.left = calloc(ncap + 1, sizeof(*tl.left));
    tl.end = calloc(n + 1, sizeof(*tl.end));
    tl.ended = calloc(n + 1, 1);
    *makespan_us = 0;
    if (tl.left == NULL || tl.end == NULL || tl.ended == NULL) {
        errno = ENOMEM;
        goto out;
    }
    etude_sched_order(jobs, n);
    refuse_large(jobs, n, cap, ncap);
    for (k = 0; k < ncap; k++) {
        tl.left[k] = cap[k].amount;
    }
    do {
        if (admit_all(&tl) != 0) {
            goto out;
        }
    } while (end_next(&tl));
    *makespan_us = tl.now;
    rc = 0;
out:
    free(tl.left);
    free(tl.end);
    free(tl.ended);
    return rc;
}

/* ---- The forecast. ---- */

/* The items and the preparation nodes of a selection. */
struct selection {
    struct etude_job *jobs; /* the items whose cost is recorded */
    struct etude_ledger_res (*res)[ETUDE_BUDGET_RESOURCES];
    char **ids;
    size_t n, cap;
    char **nodes;
    size_t nnodes, nodecap;
    size_t items;
    unsigned architectures;
};

/** Add a line of `kind` to report `r`, counting the reasons it gives. */
static void say(struct etude_budget_report *r, const char *kind, const char *format, ...)
{
    char text[1200];
    va_list ap;

    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    cmp_printf(&r->lines, "%s: %s\n", kind, text);
    r->unknown += strcmp(kind, "unknown") == 0;
    r->failed += strcmp(kind, "failure") == 0;
}

/** Copy `text` to a new string, or NULL when memory ran out. */
static char *copy_of(const char *text)
{
    size_t length = strlen(text) + 1;
    char *s = malloc(length);

    if (s != NULL) {
        memcpy(s, text, length);
    }
    return s;
}

/** Add node `target` to `s` once; 0 or -1. */
static int add_node(struct selection *s, const char *target)
{
    size_t i;

    for (i = 0; i < s->nnodes; i++) {
        if (strcmp(s->nodes[i], target) == 0) {
            return 0;
        }
    }
    if (s->nnodes == s->nodecap) {
        size_t cap = s->nodecap ? 2 * s->nodecap : 32;
        char **nodes = realloc(s->nodes, cap * sizeof(*nodes));

        if (nodes == NULL) {
            return -1;
        }
        s->nodes = nodes;
        s->nodecap = cap;
    }
    s->nodes[s->nnodes] = copy_of(target);
    return s->nodes[s->nnodes++] == NULL ? -1 : 0;
}

/** Make room in `s` for one more job; 0 or -1. */
static int grow(struct selection *s)
{
    size_t cap = s->cap ? 2 * s->cap : 64, i;
    struct etude_job *jobs;
    struct etude_ledger_res(*res)[ETUDE_BUDGET_RESOURCES];
    char **ids;

    if (s->n < s->cap) {
        return 0;
    }
    jobs = realloc(s->jobs, cap * sizeof(*jobs));
    if (jobs != NULL) {
        s->jobs = jobs;
    }
    res = realloc(s->res, cap * sizeof(*res));
    if (res != NULL) {
        s->res = res;
    }
    ids = realloc(s->ids, cap * sizeof(*ids));
    if (ids != NULL) {
        s->ids = ids;
    }
    if (jobs == NULL || res == NULL || ids == NULL) {
        return -1;
    }
    for (i = 0; i < s->n; i++) {
        s->jobs[i].res = s->res[i];
    }
    s->cap = cap;
    return 0;
}

/** Free the selection's items and nodes. */
static void selection_free(struct selection *s)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        free(s->ids[i]);
    }
    for (i = 0; i < s->nnodes; i++) {
        free(s->nodes[i]);
    }
    free(s->jobs);
    free(s->res);
    free(s->ids);
    free(s->nodes);
}

/**
 * Find the cost of the item whose key gives the first ITEM_FIELDS values in
 * `want`: of the eligible samples with those values, the newest gives the
 * rest of the key, the site profile, tools and cache state, and etude_cost_of
 * takes the cost from the samples of that whole key. With no such sample,
 * `cost->samples` is 0.
 */
static int item_cost(const struct cmp_history *h, const struct cmp_key *want, uint64_t cpu,
                     uint64_t ceiling, struct etude_cost *cost)
{
    const struct cmp_sample *newest = NULL;
    size_t i;

    for (i = 0; i < h->sample_count; i++) {
        const struct cmp_sample *s = &h->samples[i];

        if (s->reason[0] || s->plan == NULL ||
            cmp_key_differs(&s->key, want, 0, ITEM_FIELDS, NULL, 0) != 0) {
            continue;
        }
        if (newest == NULL || strcmp(s->end, newest->end) > 0) {
            newest = s;
        }
    }
    if (newest == NULL) {
        memset(cost, 0, sizeof(*cost));
        return 0;
    }
    return etude_cost_of(h, &newest->key, 0, cpu, ceiling, cost);
}

/**
 * Give where case `c` runs on the policy's site: a host case on the host; a
 * guest or machine case in QEMU, or, on a site whose accelerator is
 * `native`, on the machine itself when its native plan is `native`. NULL
 * when the site cannot run it.
 */
static const char *placement_of(const struct etude_budget_policy *p, const struct json *c)
{
    if (strcmp(json_text(c, "domain"), "host") == 0) {
        return "host";
    }
    if (strcmp(p->accelerator, "native") != 0) {
        return "qemu";
    }
    return strcmp(json_text(json_member(c, "native"), "plan"), "native") == 0 ? "native" : NULL;
}

/**
 * Write into `target` the node that makes artifact `a` of a `prepare` list
 * for `arch`: its target in `build/x86_64` when it is made for each
 * architecture and `arch` is x86_64, and in `build` otherwise. A
 * preparation step records the node's work under this name as its step ID.
 */
static void node_target(const struct json *a, const char *arch, char target[NAME_MAX_BYTES])
{
    int own = json_member(a, "per_architecture")->integer && strcmp(arch, "x86_64") == 0;

    snprintf(target, NAME_MAX_BYTES, "%s/%s", own ? "build/x86_64" : "build", json_text(a, "node"));
}

/** Add the nodes that case `c` on `arch` prepares to `s`; 0 or -1. */
static int add_prepare(struct selection *s, const struct json *c, const char *arch)
{
    const struct json *prepare = json_member(c, "prepare");
    char target[NAME_MAX_BYTES];
    size_t i;

    for (i = 0; prepare != NULL && i < prepare->count; i++) {
        node_target(prepare->item[i], arch, target);
        if (add_node(s, target) != 0) {
            return -1;
        }
    }
    return 0;
}

/** Add the item `id` of `case_id` on `arch` to `s` with its cost; 0 or -1. */
static int add_job(struct selection *s, const char *id, const char *case_id, const char *arch,
                   const struct json *resources, const struct etude_cost *cost)
{
    struct etude_job *j;
    size_t k;

    if (grow(s) != 0 || (s->ids[s->n] = copy_of(id)) == NULL) {
        return -1;
    }
    j = &s->jobs[s->n];
    memset(j, 0, sizeof(*j));
    j->id = s->ids[s->n];
    j->case_id = case_id;
    j->arch = arch;
    j->ready = 1;
    j->cost_us = cost->elapsed_us;
    j->res = s->res[s->n];
    j->nres = ETUDE_BUDGET_RESOURCES;
    for (k = 0; k < ETUDE_BUDGET_RESOURCES; k++) {
        s->res[s->n][k].name = resource_names[k];
        s->res[s->n][k].amount =
            k == 0 ? cost->cpu_millis : integer_of(resources, resource_names[k]);
    }
    s->n++;
    return 0;
}

/**
 * Add the items of case `c` on `arch` to `s`, one for each repetition its
 * workload gives, each with the cost its history records on the policy's
 * site, and its nodes; an item whose cost is not recorded is reported
 * unknown and not scheduled. 0 or -1.
 */
static int select_arch(const struct etude_budget_policy *p, const struct cmp_history *h,
                       const struct json *c, const char *arch, struct selection *s,
                       struct etude_budget_report *r)
{
    const struct json *resources = json_member(c, "resources");
    const char *case_id = json_text(c, "case_id"), *placement = placement_of(p, c);
    char digests[3][65], id[NAME_MAX_BYTES];
    struct cmp_key key;
    struct etude_cost cost;
    long long rep, reps = json_member(json_member(c, "workload"), "repetitions")->integer;

    memset(&cost, 0, sizeof(cost));
    s->architectures |= !strcmp(arch, "i686") ? 1 : !strcmp(arch, "x86_64") ? 2 : 0;
    s->items += (size_t)reps;
    if (placement == NULL) {
        say(r, "unknown", "%s on %s cannot run on %s, whose guests run natively", case_id, arch,
            p->machine_id);
    } else {
        workload_digest(c, arch, digests[0]);
        inventory_digest(c, digests[1]);
        profile_digest(c, placement, digests[2]);
        memset(&key, 0, sizeof(key));
        key.value[0] = case_id;
        key.value[1] = arch;
        key.value[2] = placement;
        key.value[3] = p->machine_id;
        key.value[4] = p->accelerator;
        key.value[5] = digests[0];
        key.value[6] = digests[1];
        key.value[7] = digests[2];
        key.value[8] = p->os;
        if (item_cost(h, &key, integer_of(resources, "cpu_millis"),
                      integer_of(resources, "cpu_ceiling_millis"), &cost) != 0) {
            return -1;
        }
        if (cost.samples == 0) {
            say(r, "unknown", "%s on %s has no recorded cost on %s under %s", case_id, arch,
                p->machine_id, p->accelerator);
        }
    }
    for (rep = 0; cost.samples > 0 && rep < reps; rep++) {
        snprintf(id, sizeof(id), "%s:%s:r%lld", case_id, arch, rep);
        if (add_job(s, id, case_id, arch, resources, &cost) != 0) {
            return -1;
        }
    }
    return add_prepare(s, c, arch);
}

/** Add the items of every case of `manifest` to `s`; 0 or -1. */
static int select_manifest(const struct etude_budget_policy *p, const struct cmp_history *h,
                           const struct json *manifest, struct selection *s,
                           struct etude_budget_report *r)
{
    const struct json *cases = json_member(manifest, "cases");
    size_t i, a;

    for (i = 0; cases != NULL && i < cases->count; i++) {
        const struct json *c = cases->item[i], *archs = json_member(c, "architectures");

        if (archs->type == JSON_STRING) {
            if (select_arch(p, h, c, "none", s, r) != 0) {
                return -1;
            }
            continue;
        }
        for (a = 0; a < archs->count; a++) {
            if (select_arch(p, h, c, archs->item[a]->string, s, r) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* One recorded cost of a preparation node. */
struct node_sample {
    const struct eh_json *env; /* the environment of its plan */
    uint64_t elapsed_us;
    char end[CMP_UTC];
};

/** Give the first run record of run `run` in `h`, or NULL. */
static const struct cmp_record *run_of(const struct cmp_history *h, const char *run)
{
    size_t low = 0, high = h->runs;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (strcmp(run, h->by_run[middle]->run) <= 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return low < h->runs && strcmp(h->by_run[low]->run, run) == 0 ? h->by_run[low] : NULL;
}

/** Say whether the array of strings `array` has `text`. */
static int listed(const struct eh_json *array, const char *text)
{
    const struct eh_json *m;

    for (m = array != NULL ? array->child : NULL; text != NULL && m != NULL; m = m->next) {
        if (m->type == 's' && strcmp(m->string, text) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Say whether two texts that may be NULL are equal. */
static int same(const char *a, const char *b)
{
    return a == b || (a != NULL && b != NULL && strcmp(a, b) == 0);
}

/** Say whether a record's `result` is a pass that nothing disrupted. */
static int clean_pass(const struct eh_json *result)
{
    const struct eh_json *disruptions = eh_json_get(result, "disruptions");

    return same(cmp_text(result, "outcome"), "pass") && disruptions != NULL &&
           disruptions->child == NULL;
}

/**
 * Give the measured value of the elapsed time in `time`, or -1 when it is
 * unavailable, only a lower bound, or has no error bound.
 */
static int64_t measured(const struct eh_json *time)
{
    const struct eh_json *elapsed = eh_json_get(time, "elapsed_us");
    const struct eh_json *value = eh_json_get(elapsed, "value");
    const struct eh_json *error = eh_json_get(elapsed, "error_bound");

    if (value == NULL || value->type != 'n' || error == NULL || error->type != 'n' ||
        same(cmp_text(elapsed, "quality"), "lower_bound")) {
        return -1;
    }
    return value->number;
}

/** Say whether record `r` is a shared step, of no item. */
static int shared_step(const struct cmp_record *r)
{
    const struct eh_json *item = eh_json_get(r->data, "item_id");

    return strcmp(r->kind, "step") == 0 && item != NULL && item->type == 'z';
}

/** Say whether record `r` is shared preparation of node `node`. */
static int prepares(const struct cmp_record *r, const char *node)
{
    const char *stage = cmp_text(r->data, "stage");

    return shared_step(r) && same(cmp_text(r->data, "step_id"), node) &&
           (same(stage, "build") || same(stage, "prepare"));
}

/** Say whether environment `env` is the policy's site. */
static int on_site(const struct etude_budget_policy *p, const struct eh_json *env)
{
    return same(cmp_text(env, "machine_id"), p->machine_id) && same(cmp_text(env, "os"), p->os) &&
           same(cmp_text(env, "accelerator"), p->accelerator);
}

/** Give the plan of record `r`, in its run, or NULL. */
static const struct cmp_record *plan_of(const struct cmp_history *h, const struct cmp_record *r)
{
    const struct cmp_record *plan = cmp_find(h, cmp_text(r->data, "plan_id"));

    if (plan == NULL || strcmp(plan->kind, "plan") != 0 || strcmp(plan->run, r->run) != 0) {
        return NULL;
    }
    return plan;
}

/**
 * Say whether the preparation of a plan of `purpose` is a sample for class
 * `cls`: a plan of the class, or, for a release, a first run, which prepares
 * from clean as a release does.
 */
static int teaches(enum etude_budget_class cls, const char *purpose)
{
    return same(purpose, class_names[cls]) ||
           (cls == ETUDE_BUDGET_RELEASE && same(purpose, class_names[ETUDE_BUDGET_FIRST_RUN]));
}

/**
 * Say whether step record `r` is a recorded cost of node `node` for class
 * `cls` on the policy's site, and fill `x` when it is: shared preparation of
 * the node, by prepares, that passed undisrupted with a measured time, of a
 * plan that teaches the class on the site, whose run's record lists it as
 * preparation and was not disrupted.
 */
static int node_sample(const struct cmp_history *h, const struct etude_budget_policy *p,
                       enum etude_budget_class cls, const char *node, const struct cmp_record *r,
                       struct node_sample *x)
{
    const struct cmp_record *plan, *run;
    int64_t elapsed;

    if (!prepares(r, node) || !clean_pass(eh_json_get(r->data, "result")) ||
        (elapsed = measured(eh_json_get(r->data, "time"))) < 0) {
        return 0;
    }
    plan = plan_of(h, r);
    run = run_of(h, r->run);
    /* A physical aggregate has no measured per-node allocation. */
    if (plan == NULL || run == NULL || run->routine_load ||
        !same(cmp_text(run->data, "plan_id"), plan->id) ||
        !listed(eh_json_get(run->data, "preparation_steps"), r->id) ||
        !clean_pass(eh_json_get(run->data, "result")) ||
        !teaches(cls, cmp_text(plan->data, "purpose")) ||
        !etude_budget_preparation_eligible(plan, cls, node)) {
        return 0;
    }
    x->env = eh_json_get(plan->data, "environment");
    x->elapsed_us = (uint64_t)elapsed;
    cmp_utc(cmp_text(eh_json_get(r->data, "time"), "end_utc"), x->end);
    return on_site(p, x->env);
}

/** Say whether two environments share their version, site, tools and caches. */
static int same_series(const struct eh_json *a, const struct eh_json *b)
{
    return same(cmp_text(a, "os_version"), cmp_text(b, "os_version")) &&
           same(cmp_digest(a, "site_profile"), cmp_digest(b, "site_profile")) &&
           same(cmp_digest(a, "tools"), cmp_digest(b, "tools")) &&
           same(cmp_digest(a, "cache_state"), cmp_digest(b, "cache_state"));
}

/**
 * Find the cost of preparation node `node` for class `cls` on the policy's
 * site: the median elapsed time of its samples, by node_sample, that share
 * the newest sample's system version, site profile, tools and cache state.
 * `*count` is the samples taken, 0 when there is none.
 *
 * @return 0, or -1 when memory ran out.
 */
static int node_cost(const struct cmp_history *h, const struct etude_budget_policy *p,
                     enum etude_budget_class cls, const char *node, uint64_t *us, size_t *count)
{
    struct node_sample *x = calloc(h->unique + 1, sizeof(*x));
    uint64_t *values = calloc(h->unique + 1, sizeof(*values));
    struct cmp_stats stats;
    size_t i, n = 0, newest = 0;
    int rc = -1;

    *us = 0;
    *count = 0;
    if (x == NULL || values == NULL) {
        goto out;
    }
    for (i = 0; i < h->unique; i++) {
        if (node_sample(h, p, cls, node, h->by_id[i], &x[n])) {
            newest = strcmp(x[n].end, x[newest].end) > 0 ? n : newest;
            n++;
        }
    }
    for (i = 0; i < n; i++) {
        if (same_series(x[i].env, x[newest].env)) {
            values[(*count)++] = x[i].elapsed_us;
        }
    }
    memset(&stats, 0, sizeof(stats));
    if (*count > 0 && cmp_statistics(values, *count, &stats) != 0) {
        goto out;
    }
    *us = stats.median_us;
    rc = 0;
out:
    free(x);
    free(values);
    return rc;
}

/** Charge the preparation of the selection's nodes, one at a time; 0 or -1. */
static int charge_nodes(const struct etude_budget_policy *p, enum etude_budget_class cls,
                        const struct cmp_history *h, const struct selection *s,
                        struct etude_budget_forecast *f)
{
    size_t i, count;
    uint64_t us;

    struct etude_preparation_selection selected = {(const char *const *)s->nodes, s->nnodes,
                                                   s->architectures, cls};
    if (etude_budget_preparation(h, p, &selected, &us, &count))
        return -1;
    if (count) {
        f->prep_us = us;
        say(&f->report, "note", "aggregate preparation: %lu matching complete samples",
            (unsigned long)count);
        return 0;
    }
    for (i = 0; i < s->nnodes; i++) {
        if (node_cost(h, p, cls, s->nodes[i], &us, &count) != 0) {
            return -1;
        }
        if (count == 0) {
            say(&f->report, "unknown", "node %s has no recorded cost in %s runs on %s", s->nodes[i],
                class_names[cls], p->machine_id);
        } else if (us > UINT64_MAX - f->prep_us) {
            errno = EOVERFLOW;
            return -1;
        } else {
            f->prep_us += us;
        }
    }
    return 0;
}

/** Report each item that the capacity alone cannot hold as a failure. */
static void report_large(const struct etude_budget_policy *p, const struct selection *s,
                         struct etude_budget_report *r)
{
    size_t i, k;

    for (i = 0; i < s->n; i++) {
        for (k = 0; s->jobs[i].state == ETUDE_JOB_REFUSED && k < p->ncapacity; k++) {
            uint64_t want = amount_of(&s->jobs[i], p->capacity[k].name);

            if (want > p->capacity[k].amount) {
                say(r, "failure", "%s needs %llu %s, more than the %llu of %s", s->jobs[i].id,
                    (unsigned long long)want, p->capacity[k].name,
                    (unsigned long long)p->capacity[k].amount, p->machine_id);
            }
        }
    }
}

/**
 * Report a charge of `charged` microseconds against the limits `l`: a note
 * when it passes the goal, and how much of the growth it uses, or when it
 * passes the goal and the growth together; and a failure when it passes
 * `limit`, the deadline of a forecast or the ceiling of a run. `what` names
 * the charge.
 */
static void report_charge(const struct etude_budget_limits *l, uint64_t charged, uint64_t limit,
                          const char *what, struct etude_budget_report *r)
{
    char a[32], b[32], c[32];

    cmp_seconds(charged, a);
    cmp_seconds(charged > l->goal_us ? charged - l->goal_us : 0, b);
    cmp_seconds(l->growth_us, c);
    if (charged > l->goal_us + l->growth_us) {
        say(r, "note", "%s, %s, passes the goal and the %s kept for growth", what, a, c);
    } else if (charged > l->goal_us) {
        say(r, "note", "%s, %s, uses %s of the %s kept for growth", what, a, b, c);
    }
    if (charged > limit) {
        cmp_seconds(limit, b);
        say(r, "failure", "%s, %s, passes the %s of %s", what, a,
            limit == l->ceiling_us ? "ceiling" : "deadline", b);
    }
}

/**
 * Decide the forecast's verdict: a failure when the costs recorded pass the
 * class's deadline, even with other costs unknown, or an item cannot fit;
 * unknown when a cost is not recorded; and a pass otherwise.
 */
static void forecast_verdict(const struct etude_budget_limits *l, struct etude_budget_forecast *f)
{
    report_charge(l, f->prep_us + f->cases_us, l->ceiling_us - l->reserve_us,
                  f->report.unknown > 0 ? "the costs recorded" : "the forecast", &f->report);
    if (f->report.failed > 0) {
        f->report.verdict = ETUDE_BUDGET_FAILURE;
    } else if (f->report.unknown > 0) {
        f->report.verdict = ETUDE_BUDGET_UNKNOWN;
    } else {
        f->report.verdict = ETUDE_BUDGET_PASS;
    }
}

/**
 * Forecast the charged time of a run of class `cls` that selects every case
 * of the `n` case manifests, on the site of policy `p`, from the costs that
 * the history `h` records there.
 *
 * Each case gives one item for each of its architectures, or one for `none`,
 * and each repetition of its workload. An item costs the median elapsed time
 * of the samples of its case, workload, inventory and profile on the site's
 * machine, system and accelerator, by item_cost; each node its cases prepare
 * costs the median of its preparation in runs of the class, by node_cost. An
 * item or node with no recorded cost is unknown, never free. Preparation is
 * charged as its nodes run one after another before any case; the items then
 * run as etude_budget_schedule gives on the site's capacity. The verdict is
 * a failure when an item needs more than the capacity has or the costs
 * recorded pass the class's deadline, the ceiling less the cleanup reserve,
 * whether or not other costs are unknown; unknown when a cost is not
 * recorded; and otherwise a pass.
 *
 * @return 0, or -1 with errno set when memory ran out or a charge overflowed.
 */
int etude_budget_forecast(const struct etude_budget_policy *p, enum etude_budget_class cls,
                          const struct cmp_history *h, struct json *const *manifests, size_t n,
                          struct etude_budget_forecast *f)
{
    struct selection s;
    size_t i;
    int rc = -1;

    memset(f, 0, sizeof(*f));
    memset(&s, 0, sizeof(s));
    for (i = 0; i < n; i++) {
        if (select_manifest(p, h, manifests[i], &s, &f->report) != 0) {
            goto out;
        }
    }
    f->items = s.items;
    f->nodes = s.nnodes;
    if (charge_nodes(p, cls, h, &s, f) != 0 ||
        etude_budget_schedule(s.jobs, s.n, p->capacity, p->ncapacity, &f->cases_us) != 0) {
        goto out;
    }
    report_large(p, &s, &f->report);
    if (f->cases_us > UINT64_MAX - f->prep_us) {
        errno = EOVERFLOW;
        goto out;
    }
    forecast_verdict(&p->limits[cls], f);
    rc = f->report.lines.failed ? -1 : 0;
    if (rc != 0) {
        errno = ENOMEM;
    }
out:
    selection_free(&s);
    return rc;
}

/* ---- The judgement of a run. ---- */

/* An interval of a run's clock, in microseconds. */
struct interval {
    uint64_t start, end;
};

/* The run a judgement reads, and what it finds of the charged interval. */
struct run_view {
    const struct cmp_history *h;
    const struct etude_budget_policy *p;
    const char *run_id;
    const struct cmp_record *run, *plan;
    const struct eh_json *clock; /* the clock of the run's own time */
    int known;                   /* the interval has a measured start and end */
    int routine;                 /* verified terminal charge replaces local time */
    uint64_t lo, hi;             /* the charged interval on that clock */
    uint64_t work_start;         /* the first start of the run's charged work */
    uint64_t error_us;           /* the error bound of the run's elapsed time */
    struct interval *once;       /* the one-time setup the charge leaves out */
    size_t nonce;
};

/** Give `text`, or `none` when it is NULL. */
static const char *shown(const char *text)
{
    return text != NULL ? text : "none";
}

/** Give the first element of `array`, or NULL. */
static const struct eh_json *child_of(const struct eh_json *array)
{
    return array != NULL ? array->child : NULL;
}

/** Give the element of `array` whose member `field` is `value`, or NULL. */
static const struct eh_json *member_with(const struct eh_json *array, const char *field,
                                         const char *value)
{
    const struct eh_json *m;

    for (m = child_of(array); m != NULL; m = m->next) {
        if (same(cmp_text(m, field), value)) {
            return m;
        }
    }
    return NULL;
}

/** Give the integer member `name` of `object`, or -1 when it has none. */
static int64_t number_of(const struct eh_json *object, const char *name)
{
    const struct eh_json *v = eh_json_get(object, name);

    return v != NULL && v->type == 'n' ? v->number : -1;
}

/** Say whether two clocks are one: the same machine, boot and clock. */
static int same_clock(const struct eh_json *a, const struct eh_json *b)
{
    return same(cmp_text(a, "machine_id"), cmp_text(b, "machine_id")) &&
           same(cmp_text(a, "boot_id"), cmp_text(b, "boot_id")) &&
           same(cmp_text(a, "clock_id"), cmp_text(b, "clock_id"));
}

/** Say whether record `r` is a step of the stage `stage`. */
static int of_stage(const struct cmp_record *r, const char *stage)
{
    return strcmp(r->kind, "step") == 0 && same(cmp_text(r->data, "stage"), stage);
}

/**
 * Find the record of run `v->run_id` and its plan, or say in `j->error` why
 * the run cannot be judged: it has no run record or more than one, or its
 * plan is not in the history.
 */
static int find_run(struct run_view *v, struct etude_budget_judgement *j)
{
    size_t i, count = 0;

    for (i = 0; i < v->h->runs; i++) {
        if (strcmp(v->h->by_run[i]->run, v->run_id) == 0 && count++ == 0) {
            v->run = v->h->by_run[i];
        }
    }
    if (count != 1) {
        snprintf(j->error, sizeof(j->error), "run %s has %s run record", v->run_id,
                 count == 0 ? "no" : "more than one");
        return -1;
    }
    v->plan = plan_of(v->h, v->run);
    if (v->plan == NULL) {
        snprintf(j->error, sizeof(j->error), "the plan %s of run %s is not in the history",
                 shown(cmp_text(v->run->data, "plan_id")), v->run_id);
        return -1;
    }
    j->cls = (enum etude_budget_class)etude_budget_class_of(cmp_text(v->plan->data, "purpose"));
    j->plan_id = v->plan->id;
    v->clock = eh_json_get(eh_json_get(v->run->data, "time"), "clock");
    return 0;
}

/**
 * Fail a run whose recorded budget does not agree with the policy: one that
 * names another policy document, or records other limits for its class.
 */
static void check_policy(const struct run_view *v, struct etude_budget_judgement *j)
{
    const struct eh_json *b = eh_json_get(v->run->data, "budget");
    const struct etude_budget_limits *l = &v->p->limits[j->cls];

    if (!same(cmp_digest(b, "policy"), v->p->sha256) ||
        number_of(eh_json_get(b, "policy"), "size_bytes") != (int64_t)v->p->size_bytes) {
        say(&j->report, "failure", "the run's budget names the policy %s, not this one, %s",
            shown(cmp_digest(b, "policy")), v->p->sha256);
    }
    if (number_of(b, "goal_us") != (int64_t)l->goal_us ||
        number_of(b, "growth_us") != (int64_t)l->growth_us ||
        number_of(b, "ceiling_us") != (int64_t)l->ceiling_us ||
        number_of(b, "cleanup_reserve_us") != (int64_t)l->reserve_us) {
        say(&j->report, "failure", "the run's budget records limits other than the policy's for %s",
            class_names[j->cls]);
    }
}

/** Leave the charge of a run made on another site unknown. */
static void check_site(const struct run_view *v, struct etude_budget_judgement *j)
{
    const struct eh_json *env = eh_json_get(v->plan->data, "environment");

    if (!on_site(v->p, env)) {
        say(&j->report, "unknown",
            "the run was on %s, %s, %s, not on the policy's site, %s, %s, %s, so its charge "
            "there is unknown",
            shown(cmp_text(env, "machine_id")), shown(cmp_text(env, "os")),
            shown(cmp_text(env, "accelerator")), v->p->machine_id, v->p->os, v->p->accelerator);
    }
}

/**
 * Give 1 when the run is a first run whose exemption holds: it names the
 * plan's selection as its manifest, this run, and every attempt the plan
 * lists. An exemption that does not hold is a failure, never leave to pass
 * a limit. Another class of run has no exemption, and gives 0.
 */
static int check_exemption(const struct run_view *v, struct etude_budget_judgement *j)
{
    const struct eh_json *ex = eh_json_get(v->plan->data, "exemption"), *item, *id;
    size_t before = j->report.failed;

    if (j->cls != ETUDE_BUDGET_FIRST_RUN) {
        return 0;
    }
    if (!same(cmp_digest(ex, "manifest"), cmp_digest(v->plan->data, "selection"))) {
        say(&j->report, "failure",
            "the exemption names the manifest %s, not the plan's selection, %s",
            shown(cmp_digest(ex, "manifest")), shown(cmp_digest(v->plan->data, "selection")));
    }
    if (!listed(eh_json_get(ex, "run_ids"), v->run_id)) {
        say(&j->report, "failure", "the exemption does not list run %s", v->run_id);
    }
    for (item = child_of(eh_json_get(v->plan->data, "items")); item != NULL; item = item->next) {
        for (id = child_of(eh_json_get(item, "attempt_ids")); id != NULL; id = id->next) {
            if (!listed(eh_json_get(ex, "attempt_ids"), id->string)) {
                say(&j->report, "failure", "the exemption does not list attempt %s", id->string);
            }
        }
    }
    return j->report.failed == before;
}

/**
 * Check attempt record `record`, which the run's summary gives for `item`:
 * one that is not in the history leaves the item's time unobserved, and one
 * of another run, such as an exempt first run, or of another plan or item,
 * is work whose time this run did not charge, and fails.
 */
static void check_attempt(const struct run_view *v, const struct eh_json *item, const char *record,
                          struct etude_budget_judgement *j)
{
    const struct cmp_record *a = cmp_find(v->h, record), *plan;
    const char *id = cmp_text(item, "item_id");

    if (a == NULL || strcmp(a->kind, "attempt") != 0) {
        say(&j->report, "unknown", "attempt record %s of item %s is not in the history", record,
            id);
        return;
    }
    if (strcmp(a->run, v->run_id) != 0) {
        plan = plan_of(v->h, a);
        say(&j->report, "failure",
            "item %s reuses attempt record %s of run %s%s; a run's budget charges only its own "
            "work",
            id, record, a->run,
            plan != NULL && same(cmp_text(plan->data, "purpose"), "phase_first_run")
                ? ", an exempt first run"
                : "");
        return;
    }
    if (!same(cmp_text(a->data, "plan_id"), v->plan->id) ||
        !same(cmp_text(a->data, "item_id"), id)) {
        say(&j->report, "failure", "attempt record %s is not of item %s of plan %s", record, id,
            v->plan->id);
    }
}

/**
 * Check that the run did each item of its plan in attempts of its own. An
 * item the summary leaves out, that did not finish, or that has no attempt
 * leaves its time unobserved and the charge unknown; check_attempt judges
 * each attempt the summary gives.
 */
static void check_work(const struct run_view *v, struct etude_budget_judgement *j)
{
    const struct eh_json *summaries = eh_json_get(v->run->data, "items"), *item, *a;

    for (item = child_of(eh_json_get(v->plan->data, "items")); item != NULL; item = item->next) {
        const char *id = cmp_text(item, "item_id");
        const struct eh_json *summary = member_with(summaries, "item_id", id);
        const char *outcome = cmp_text(summary, "outcome");

        if (summary == NULL) {
            say(&j->report, "unknown",
                "item %s is planned but not in the run's summary, so its time is not observed", id);
            continue;
        }
        if (same(outcome, "incomplete") || same(outcome, "cancelled")) {
            say(&j->report, "unknown", "item %s is %s, so its work did not finish", id, outcome);
        }
        if (child_of(eh_json_get(summary, "attempt_records")) == NULL) {
            say(&j->report, "unknown", "item %s has no attempt, so its time is not observed", id);
        }
        for (a = child_of(eh_json_get(summary, "attempt_records")); a != NULL; a = a->next) {
            check_attempt(v, item, a->string, j);
        }
    }
}

/** Say whether the run lists a shared step of its own with step ID `target`. */
static int prepared(const struct run_view *v, const char *target)
{
    const struct eh_json *id;

    for (id = child_of(eh_json_get(v->run->data, "preparation_steps")); id != NULL; id = id->next) {
        const struct cmp_record *r = cmp_find(v->h, id->string);

        if (r != NULL && strcmp(r->run, v->run_id) == 0 && shared_step(r) &&
            same(cmp_text(r->data, "step_id"), target)) {
            return 1;
        }
    }
    return 0;
}

/** Fail each node that case `c` prepares on `arch` and the run did not. */
static void check_case_nodes(const struct run_view *v, const struct json *c, const char *arch,
                             struct etude_budget_judgement *j)
{
    const struct json *prepare = json_member(c, "prepare");
    char target[NAME_MAX_BYTES];
    size_t k;

    for (k = 0; prepare != NULL && k < prepare->count; k++) {
        node_target(prepare->item[k], arch, target);
        if (!prepared(v, target)) {
            say(&j->report, "failure", "node %s, which %s prepares on %s, has no preparation step",
                target, json_text(c, "case_id"), arch);
        }
    }
}

/** Check the nodes of each case of `manifest` whose first item is planned. */
static void check_nodes(const struct run_view *v, const struct json *manifest,
                        struct etude_budget_judgement *j)
{
    const struct json *cases = json_member(manifest, "cases");
    const struct eh_json *item;
    size_t i;

    for (item = child_of(eh_json_get(v->plan->data, "items")); item != NULL; item = item->next) {
        if (number_of(item, "repetition") != 0) {
            continue;
        }
        for (i = 0; cases != NULL && i < cases->count; i++) {
            if (same(json_text(cases->item[i], "case_id"), cmp_text(item, "case_id"))) {
                check_case_nodes(v, cases->item[i], cmp_text(item, "architecture"), j);
            }
        }
    }
}

/**
 * Check that the run's preparation is its own and on its record: each step
 * it lists as preparation is a shared step of this run, each shared step of
 * this run is listed, and, with the manifests of its cases, each node that a
 * planned case prepares has a preparation step. Preparation that is not on
 * the record, or that another run did, is hidden from the charge, and fails.
 */
static void check_preparation(const struct run_view *v, struct json *const *manifests, size_t n,
                              struct etude_budget_judgement *j)
{
    const struct eh_json *prep = eh_json_get(v->run->data, "preparation_steps"), *id;
    size_t i;

    for (id = child_of(prep); id != NULL; id = id->next) {
        const struct cmp_record *r = cmp_find(v->h, id->string);

        if (r == NULL) {
            say(&j->report, "unknown", "preparation step %s is not in the history", id->string);
        } else if (strcmp(r->run, v->run_id) != 0) {
            say(&j->report, "failure",
                "preparation step %s was recorded in run %s, so this run does not charge it",
                id->string, r->run);
        } else if (!shared_step(r)) {
            say(&j->report, "failure", "preparation step %s is a step of an item", id->string);
        }
    }
    for (i = 0; i < v->h->unique; i++) {
        const struct cmp_record *r = v->h->by_id[i];

        if (strcmp(r->run, v->run_id) == 0 && shared_step(r) && !listed(prep, r->id)) {
            say(&j->report, "failure", "shared step %s is not among the run's preparation steps",
                r->id);
        }
    }
    for (i = 0; i < n; i++) {
        check_nodes(v, manifests[i], j);
    }
}

/**
 * Take the run's own interval on its clock, with the error bound of its
 * elapsed time. Unconfirmed cleanup, an end not observed, or an elapsed
 * time that is only a lower bound or has no error bound leaves the charge
 * unknown; work the run did not finish leaves the verdict unknown.
 */
static void run_interval(struct run_view *v, struct etude_budget_judgement *j)
{
    const struct eh_json *time = eh_json_get(v->run->data, "time");
    const struct eh_json *result = eh_json_get(v->run->data, "result");
    const char *outcome = cmp_text(result, "outcome");

    if (same(outcome, "incomplete") || same(outcome, "cancelled")) {
        say(&j->report, "unknown", "the run is %s: %s", outcome, shown(cmp_text(result, "reason")));
    }
    if (same(cmp_text(result, "cleanup"), "unconfirmed")) {
        say(&j->report, "unknown", "the run's cleanup is unconfirmed, so its charge has no end");
    } else if (number_of(time, "end_us") < 0) {
        say(&j->report, "unknown", "the run's end was not observed: %s",
            shown(cmp_text(eh_json_get(time, "elapsed_us"), "reason")));
    } else if (measured(time) < 0) {
        say(&j->report, "unknown",
            "the run's elapsed time is only a lower bound or has no error bound");
    } else {
        v->known = 1;
        v->lo = (uint64_t)number_of(time, "start_us");
        v->hi = (uint64_t)number_of(time, "end_us");
        v->work_start = v->hi;
        v->error_us = (uint64_t)number_of(eh_json_get(time, "elapsed_us"), "error_bound");
    }
}

/**
 * Extend the charged interval to cover record `r` of the run, an attempt or
 * a step: work outside the run's own interval is charged, never hidden. A
 * shared step or an attempt on another clock, or one whose end was not
 * observed, leaves the charge unknown; a step of an item on another clock is
 * inside its attempt.
 */
static void cover(struct run_view *v, const struct cmp_record *r, struct etude_budget_judgement *j)
{
    const struct eh_json *time = eh_json_get(r->data, "time");
    int64_t start = number_of(time, "start_us"), end = number_of(time, "end_us");
    char gap[32];

    if (!same_clock(eh_json_get(time, "clock"), v->clock)) {
        if (shared_step(r) || strcmp(r->kind, "attempt") == 0) {
            say(&j->report, "unknown", "%s %s is timed on another clock than the run", r->kind,
                r->id);
            v->known = 0;
        }
        return;
    }
    if (start < 0 || end < 0) {
        say(&j->report, "unknown", "%s %s has no observed end", r->kind, r->id);
        v->known = 0;
        return;
    }
    if ((uint64_t)start < v->lo) {
        cmp_seconds(v->lo - (uint64_t)start, gap);
        say(&j->report, "note", "%s %s began %s before the run; the charge starts with it", r->kind,
            r->id, gap);
        v->lo = (uint64_t)start;
    }
    if ((uint64_t)end > v->hi) {
        cmp_seconds((uint64_t)end - v->hi, gap);
        say(&j->report, "note", "%s %s ended %s after the run; the charge ends with it", r->kind,
            r->id, gap);
        v->hi = (uint64_t)end;
    }
    v->work_start = (uint64_t)start < v->work_start ? (uint64_t)start : v->work_start;
}

/** Say whether the policy lists step ID `id` as one-time setup. */
static int one_time_listed(const struct etude_budget_policy *p, const char *id)
{
    size_t i;

    for (i = 0; p->one_time != NULL && i < p->one_time->count; i++) {
        if (same(p->one_time->item[i]->string, id)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Give a record of an earlier run on the run's machine that passed the
 * one-time setup step of `r`'s step ID, or NULL when `r` is the first.
 */
static const struct cmp_record *done_before(const struct run_view *v, const struct cmp_record *r)
{
    const char *machine = cmp_text(eh_json_get(v->plan->data, "environment"), "machine_id");
    size_t i;

    for (i = 0; i < v->h->unique; i++) {
        const struct cmp_record *o = v->h->by_id[i], *plan;

        if (!of_stage(o, "one_time_setup") || strcmp(o->run, v->run_id) == 0 ||
            strcmp(o->utc, r->utc) >= 0 ||
            !same(cmp_text(o->data, "step_id"), cmp_text(r->data, "step_id")) ||
            !clean_pass(eh_json_get(o->data, "result"))) {
            continue;
        }
        plan = plan_of(v->h, o);
        if (plan != NULL &&
            same(cmp_text(eh_json_get(plan->data, "environment"), "machine_id"), machine)) {
            return o;
        }
    }
    return NULL;
}

/**
 * Leave one-time setup step `r` out of the charge when it may be: the policy
 * lists it, no earlier run on the machine did it, it is on the run's clock
 * with an observed end, and it ended before the run's other work began.
 * Otherwise it is charged as ordinary preparation, and a note says why.
 *
 * @return 0, or -1 when memory ran out.
 */
static int one_time(struct run_view *v, const struct cmp_record *r,
                    struct etude_budget_judgement *j)
{
    const struct eh_json *time = eh_json_get(r->data, "time");
    int64_t start = number_of(time, "start_us"), end = number_of(time, "end_us");
    const struct cmp_record *before = done_before(v, r);
    struct interval *once;
    char why[400];

    why[0] = 0;
    if (!one_time_listed(v->p, cmp_text(r->data, "step_id"))) {
        snprintf(why, sizeof(why), "the policy does not list it as one-time setup");
    } else if (before != NULL) {
        snprintf(why, sizeof(why), "run %s did it before on this machine", before->run);
    } else if (!same_clock(eh_json_get(time, "clock"), v->clock) || start < 0 || end < 0) {
        snprintf(why, sizeof(why), "its time cannot be placed in the run's interval");
    } else if ((uint64_t)end > v->work_start) {
        snprintf(why, sizeof(why), "it overlaps the run's other work");
    }
    if (why[0] != 0) {
        say(&j->report, "note", "one-time setup step %s is charged: %s", r->id, why);
        cover(v, r, j);
        return 0;
    }
    once = realloc(v->once, (v->nonce + 1) * sizeof(*once));
    if (once == NULL) {
        return -1;
    }
    v->once = once;
    v->once[v->nonce].start = (uint64_t)start < v->lo ? v->lo : (uint64_t)start;
    v->once[v->nonce++].end = (uint64_t)end;
    return 0;
}

/** Order intervals by their start. */
static int by_start(const void *a, const void *b)
{
    const struct interval *x = a, *y = b;

    return (x->start > y->start) - (x->start < y->start);
}

/** Give the time that the intervals of `v->once` cover, each moment once. */
static uint64_t once_total(struct run_view *v)
{
    uint64_t total = 0, end = 0;
    size_t i;

    if (v->nonce > 0) {
        qsort(v->once, v->nonce, sizeof(*v->once), by_start);
    }
    for (i = 0; i < v->nonce; i++) {
        uint64_t from = v->once[i].start > end ? v->once[i].start : end;

        total += v->once[i].end > from ? v->once[i].end - from : 0;
        end = v->once[i].end > end ? v->once[i].end : end;
    }
    return total;
}

/**
 * Charge the run: its elapsed time, extended to cover each attempt and step
 * of it outside its interval, less the first one-time setup that one_time
 * leaves out. Outside queue wait is not charged. The charge is known only
 * when every part of it was observed.
 *
 * @return 0, or -1 when memory ran out.
 */
static int charge(struct run_view *v, struct etude_budget_judgement *j)
{
    uint64_t start, end, total;
    size_t i;

    run_interval(v, j);
    start = v->lo;
    end = v->hi;
    for (i = 0; v->known && i < v->h->unique; i++) {
        const struct cmp_record *r = v->h->by_id[i];

        if (strcmp(r->run, v->run_id) == 0 && !of_stage(r, "queue") &&
            !of_stage(r, "one_time_setup") &&
            (strcmp(r->kind, "step") == 0 || strcmp(r->kind, "attempt") == 0)) {
            cover(v, r, j);
        }
    }
    for (i = 0; v->known && i < v->h->unique; i++) {
        const struct cmp_record *r = v->h->by_id[i];

        if (strcmp(r->run, v->run_id) == 0 && of_stage(r, "one_time_setup") &&
            one_time(v, r, j) != 0) {
            return -1;
        }
    }
    if (v->known) {
        j->one_time_us = once_total(v);
        total =
            (uint64_t)measured(eh_json_get(v->run->data, "time")) + (start - v->lo) + (v->hi - end);
        j->charged_us = total > j->one_time_us ? total - j->one_time_us : 0;
        j->charged_known = 1;
    }
    struct etude_routine_cost routine;
    int status = etude_budget_routine(v->plan, v->run, &routine);
    if (status < 0) {
        j->charged_known = 0;
        say(&j->report, "unknown", "the whole-PC owner has no matching completed charge proof");
    } else if (status) {
        v->routine = 1;
        j->charged_us = routine.charged_us;
        j->one_time_us = 0;
        j->charged_known = 1;
        v->error_us = routine.error_us;
    }
    return 0;
}

/**
 * Decide the run's verdict: a failure when its records hide work or do not
 * agree with the policy, whatever its exemption; exempt for a first run
 * whose exemption holds; a failure when the charge passes the ceiling;
 * unknown when the charge or the work is unknown, or the charge is within
 * the ceiling by less than its error bound; and a pass otherwise.
 */
static void judge_verdict(const struct run_view *v, int exempt, struct etude_budget_judgement *j)
{
    const struct etude_budget_limits *l = &v->p->limits[j->cls];
    char a[32], b[32];

    if (j->report.failed > 0 || exempt) {
        j->report.verdict = j->report.failed > 0 ? ETUDE_BUDGET_FAILURE : ETUDE_BUDGET_EXEMPT;
        return;
    }
    if (j->charged_known) {
        report_charge(l, j->charged_us, l->ceiling_us, "the charge", &j->report);
        if (j->report.failed == 0 && v->error_us > l->ceiling_us - j->charged_us) {
            cmp_seconds(j->charged_us, a);
            cmp_seconds(v->error_us, b);
            say(&j->report, "unknown",
                "the charge, %s, is within the ceiling by less than its error bound, %s", a, b);
        }
    }
    if (j->report.failed > 0) {
        j->report.verdict = ETUDE_BUDGET_FAILURE;
    } else if (j->report.unknown > 0 || !j->charged_known) {
        j->report.verdict = ETUDE_BUDGET_UNKNOWN;
    } else {
        j->report.verdict = ETUDE_BUDGET_PASS;
    }
}

/** Write measurement `value` in seconds into `out`, or `none` when it is -1. */
static void value_text(int64_t value, char out[32])
{
    if (value < 0) {
        snprintf(out, 32, "none");
    } else {
        cmp_seconds((uint64_t)value, out);
    }
}

/**
 * Say whether the run's recorded budget is the judgement's: the same
 * verdict or an unevaluated verdict (`unknown`), and, when the judgement
 * has a charge, the same charge and one-time setup. A line of kind
 * `record` says how it differs.
 */
static int agrees(const struct run_view *v, struct etude_budget_judgement *j)
{
    const struct eh_json *b = eh_json_get(v->run->data, "budget");
    int64_t charged = number_of(eh_json_get(b, "charged_us"), "value");
    int64_t once = number_of(eh_json_get(b, "one_time_setup_us"), "value");
    const char *verdict = cmp_text(b, "verdict");
    char x[32], y[32];
    int ok = same(verdict, "unknown") || same(verdict, verdict_names[j->report.verdict]);

    if (j->charged_known && !(v->routine && same(verdict, "unknown") && charged < 0)) {
        ok = ok && charged == (int64_t)j->charged_us && once == (int64_t)j->one_time_us;
    }
    if (!ok) {
        value_text(charged, x);
        value_text(once, y);
        say(&j->report, "record",
            "the run records the verdict %s, a charge of %s and one-time setup of %s; this "
            "judgement differs",
            shown(verdict), x, y);
    }
    return ok;
}

/**
 * Judge the budget of run `run_id` from the records of history `h`, by
 * policy `p`, as tools/etude/Readme.md describes: whether the run agrees
 * with the policy and ran on its site, whether a first run's exemption
 * holds, whether every planned item was done in the run's own attempts and
 * all its preparation is on its record, and what the run charged. With the
 * `n` case manifests of its selection, each node a planned case prepares
 * must have a preparation step of the run. `j->agrees` says whether the
 * budget the run records agrees, accepting `unknown` as unevaluated.
 *
 * @return 0 with the judgement in `j`; -1 with the reason in `j->error`
 *         when the run cannot be judged or memory ran out.
 */
int etude_budget_judge(const struct cmp_history *h, const struct etude_budget_policy *p,
                       const char *run_id, struct json *const *manifests, size_t n,
                       struct etude_budget_judgement *j)
{
    struct run_view v;
    size_t i;
    int exempt, rc = -1;

    memset(j, 0, sizeof(*j));
    memset(&v, 0, sizeof(v));
    v.h = h;
    v.p = p;
    v.run_id = run_id;
    if (find_run(&v, j) != 0) {
        return -1;
    }
    for (i = 0; i < h->note_count; i++) {
        say(&j->report, "unknown", "a record may be missing: %s", h->notes[i]);
    }
    check_policy(&v, j);
    check_site(&v, j);
    exempt = check_exemption(&v, j);
    check_work(&v, j);
    check_preparation(&v, manifests, n, j);
    if (charge(&v, j) == 0) {
        judge_verdict(&v, exempt, j);
        j->agrees = agrees(&v, j);
        rc = j->report.lines.failed ? -1 : 0;
    }
    if (rc != 0) {
        snprintf(j->error, sizeof(j->error), "memory ran out");
    }
    free(v.once);
    return rc;
}
