/*
 * cost.c: the expected time and the processor reservation of a planned case,
 * from the samples of its history that were measured where it will run.
 */
#include "cost.h"
#include "etude/sched.h"
#include <stdlib.h>
#include <string.h>

static int text_is(const struct eh_json *object, const char *key, const char *want)
{
    const char *value = cmp_text(object, key);
    return value && !strcmp(value, want);
}

static int64_t number_of(const struct eh_json *object, const char *key)
{
    const struct eh_json *value = eh_json_get(object, key);
    return value && value->type == 'n' && value->number >= 0 ? value->number : -1;
}

static struct eh_json *cost_blob(const struct eh_json *ref)
{
    size_t size;
    char *bytes = cmp_blob_bytes(ref, &size);
    struct eh_json *doc = bytes && !memchr(bytes, 0, size) ? eh_json_parse(bytes) : NULL;
    free(bytes);
    return doc;
}

/* Each binding has one observed load for this exact planned item. */
static const struct eh_json *item_load(const struct eh_json *run, const char *item)
{
    const struct eh_json *applied = eh_json_get(run, "applied"), *found = NULL;
    if (!applied || applied->type != 'a' || !item)
        return NULL;
    for (const struct eh_json *p = applied->child; p; p = p->next)
        if (text_is(p, "item_id", item)) {
            if (found)
                return NULL;
            found = eh_json_get(p, "load");
            if (!found || found->type != 'o')
                return NULL;
        }
    return found;
}

static int same_load(const struct eh_json *declared, const struct eh_json *observed)
{
    const char *id = cmp_text(declared, "load_id"), *kind = cmp_text(declared, "kind");
    const char *method = cmp_text(observed, "cpu_method");
    int64_t amount = number_of(declared, "amount");
    const struct eh_json *failed = eh_json_get(observed, "failed");
    return id && *id && kind &&
           (!strcmp(kind, "cpu") || !strcmp(kind, "memory") || !strcmp(kind, "disk")) &&
           amount > 0 && text_is(observed, "load_id", id) && text_is(observed, "kind", kind) &&
           number_of(observed, "amount") == amount && text_is(observed, "cleanup", "confirmed") &&
           failed && failed->type == 'f' && text_is(observed, "cpu_quality", "exact") && method &&
           *method;
}

struct cost_identity {
    int64_t pid;
    uint64_t creation;
};

/* Process creation counters use decimal strings on Windows, where they
 * exceed JSON's exact integer range. Keep older exact numeric records. */
static uint64_t creation_of(const struct eh_json *doc, const char *key)
{
    const struct eh_json *v = eh_json_get(doc, key);
    if (v && v->type == 'n')
        return v->number > 0 ? (uint64_t)v->number : 0;
    if (!v || v->type != 's' || v->string[0] < '1' || v->string[0] > '9')
        return 0;
    uint64_t n = 0;
    for (const char *p = v->string; *p; p++) {
        unsigned digit = (unsigned char)*p - '0';
        if (digit > 9 || n > (UINT64_MAX - digit) / 10)
            return 0;
        n = n * 10 + digit;
    }
    return n;
}

static int identity(const struct eh_json *doc, const char *birth, struct cost_identity *out)
{
    out->pid = number_of(doc, "pid");
    out->creation = creation_of(doc, birth);
    return out->pid > 0 && out->creation > 0;
}

/* The case scope is bound to its attempt, not a load or another attempt. */
static int case_scope(const struct cmp_sample *s, const struct eh_json *scope,
                      struct cost_identity ids[2])
{
    const char *attempt = cmp_text(s->attempt->data, "attempt_id");
    const struct eh_json *processes = eh_json_get(scope, "processes");
    unsigned roles = 0;
    if (!attempt || !text_is(scope, "attempt_id", attempt) || !processes || processes->type != 'a')
        return 0;
    for (const struct eh_json *p = processes->child; p; p = p->next) {
        int at = text_is(p, "role", "keeper") ? 0 : text_is(p, "role", "case") ? 1 : -1;
        if (at < 0 || (roles & (1u << at)) || !identity(p, "start", &ids[at]))
            return 0;
        roles |= 1u << at;
    }
    return roles == 3 && ids[0].pid != ids[1].pid;
}

/* A Windows keeper may be common to both jobs. Their process identities
 * must differ, and a reused keeper PID must have the same creation value. */
static int load_scope(const struct cmp_sample *s, const struct eh_json *scope,
                      const struct eh_json *load)
{
    struct cost_identity cases[2], keeper, process;
    const struct eh_json *time = eh_json_get(s->attempt->data, "time");
    int64_t start = number_of(time, "start_us"), end = number_of(time, "end_us");
    int64_t ready = number_of(load, "ready_us"), ended = number_of(load, "ended_us");
    if (!case_scope(s, scope, cases) ||
        !identity(eh_json_get(load, "keeper"), "creation", &keeper) ||
        !identity(eh_json_get(load, "process"), "creation", &process))
        return 0;
    if (keeper.pid == process.pid || process.pid == cases[0].pid || process.pid == cases[1].pid ||
        keeper.pid == cases[1].pid)
        return 0;
    if (keeper.pid == cases[0].pid && keeper.creation != cases[0].creation)
        return 0;
    return start >= 0 && ready >= start && ended >= ready && end >= ended;
}

/* Reporting retains the combined measurement. Planning subtracts a load
 * only from exact accounting with a complete, matching ownership binding. */
static int64_t base_cpu(const struct cmp_sample *s, int64_t total)
{
    const struct eh_json *resources = eh_json_get(s->attempt->data, "resources");
    struct eh_json *scope = cost_blob(eh_json_get(resources, "scope"));
    struct eh_json *workload = cost_blob(eh_json_get(s->item, "workload"));
    struct eh_json *run = s->run ? cost_blob(eh_json_get(s->run->data, "load")) : NULL;
    const struct eh_json *declared = eh_json_get(eh_json_get(workload, "repetition"), "load");
    const struct eh_json *load = item_load(run, cmp_text(s->item, "item_id"));
    int64_t value = -1, cpu = number_of(load, "cpu_us");
    if (same_load(declared, load) && load_scope(s, scope, load) && cpu >= 0 && cpu <= total)
        value = total - cpu;
    eh_json_free(scope);
    eh_json_free(workload);
    eh_json_free(run);
    return value;
}

/** Give case-only CPU, or -1 when no safe case measurement exists. */
static int64_t cpu_of(const struct cmp_sample *s)
{
    const struct eh_json *cpu =
        eh_json_get(eh_json_get(s->attempt->data, "resources"), "tree_cpu_us");
    int64_t total = number_of(cpu, "value");
    if (!text_is(cpu, "method", "case-and-reserved-load-v1"))
        return total;
    return total >= 0 && text_is(cpu, "quality", "exact") ? base_cpu(s, total) : -1;
}

/**
 * Find the cost of the case whose plan gives it `key`, from the samples of
 * `h` that the comparison finds eligible, a pass that was not disrupted nor
 * a retry, with an elapsed time, and that share the first COST_FIELDS
 * values of the key: the case, its workload, inventory and profile, the
 * machine, its system, accelerator, site profile, tools and cache state.
 *
 * The expected time is the median of their elapsed times, or `initial_us`,
 * the case's declared initial bound, when there is none. The reservation is
 * etude_sched_cpu of those with a case processor time, a standalone lower
 * bound taken as its value, from `declared_cpu` within `ceiling_cpu`. Combined
 * case/load CPU needs exact, scope-checked subtraction or supplies no ratio.
 *
 * @return 0, or -1 when memory ran out.
 */
int etude_cost_of(const struct cmp_history *h, const struct cmp_key *key, uint64_t initial_us,
                  uint64_t declared_cpu, uint64_t ceiling_cpu, struct etude_cost *cost)
{
    uint64_t *elapsed, *cpu, *cpu_elapsed;
    struct cmp_stats stats;
    size_t i;
    int64_t c;
    int rc = -1;

    memset(cost, 0, sizeof(*cost));
    elapsed = malloc((h->sample_count + 1) * sizeof(*elapsed));
    cpu = malloc((h->sample_count + 1) * sizeof(*cpu));
    cpu_elapsed = malloc((h->sample_count + 1) * sizeof(*cpu_elapsed));
    if (!elapsed || !cpu || !cpu_elapsed)
        goto out;
    for (i = 0; i < h->sample_count; i++) {
        const struct cmp_sample *s = &h->samples[i];
        if (s->reason[0] || !s->plan || cmp_key_differs(&s->key, key, 0, COST_FIELDS, NULL, 0))
            continue;
        elapsed[cost->samples++] = s->elapsed_us;
        if ((c = cpu_of(s)) >= 0) {
            cpu[cost->cpu_samples] = (uint64_t)c;
            cpu_elapsed[cost->cpu_samples++] = s->elapsed_us;
        }
    }
    cost->elapsed_us = initial_us;
    memset(&stats, 0, sizeof(stats));
    if (cost->samples) {
        if (cmp_statistics(elapsed, cost->samples, &stats))
            goto out;
        cost->elapsed_us = stats.median_us;
    }
    cost->cpu_millis =
        etude_sched_cpu(cpu, cpu_elapsed, cost->cpu_samples, declared_cpu, ceiling_cpu);
    rc = 0;
out:
    free(elapsed);
    free(cpu);
    free(cpu_elapsed);
    return rc;
}
