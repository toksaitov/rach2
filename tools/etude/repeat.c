/*
 * repeat.c: the plan of a run of repetitions drawn from its seed, its
 * repetition record and the check of one, and the tally of attempts by case
 * that finds a result possibly flaky. It is plain C99.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compare.h"
#include "etude/repeat.h"
#include "repeat_text.h"

/* The longest identity of the history. */
#define ID_LIMIT 255

static const char *const kind_names[] = {"cpu", "memory", "disk"};

void etude_rng_seed(struct etude_rng *r, uint64_t seed)
{
    r->state = seed;
}

uint64_t etude_rng_next(struct etude_rng *r)
{
    uint64_t z = (r->state += UINT64_C(0x9e3779b97f4a7c15));

    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

uint64_t etude_rng_below(struct etude_rng *r, uint64_t n)
{
    uint64_t least, x;

    if (n == 0)
        return 0;
    least = (UINT64_C(0) - n) % n;
    do
        x = etude_rng_next(r);
    while (x < least);
    return x % n;
}

int etude_repeat_plan(const struct etude_repeat_spec *spec, struct etude_repeat_item **items,
                      size_t *n)
{
    struct etude_rng rng;
    struct etude_repeat_item *it, t;
    size_t *pool, *chosen, i, j, k, count = 0, picked;

    *items = NULL;
    *n = 0;
    if (spec->seed > ETUDE_REPEAT_SEED_MAX || spec->load_percent > 100 || spec->ncases == 0) {
        errno = EINVAL;
        return -1;
    }
    pool = malloc(spec->ncases * sizeof(*pool));
    chosen = calloc(spec->ncases, sizeof(*chosen));
    if (pool == NULL || chosen == NULL) {
        free(pool);
        free(chosen);
        errno = ENOMEM;
        return -1;
    }
    etude_rng_seed(&rng, spec->seed);
    for (i = 0; i < spec->ncases; i++)
        pool[i] = i;
    picked = spec->pick > 0 && spec->pick < spec->ncases ? spec->pick : spec->ncases;
    for (i = 0; picked < spec->ncases && i < picked; i++) {
        j = i + (size_t)etude_rng_below(&rng, spec->ncases - i);
        k = pool[i];
        pool[i] = pool[j];
        pool[j] = k;
    }
    for (i = 0; i < picked; i++)
        chosen[pool[i]] = 1;
    free(pool);
    for (i = 0; i < spec->ncases; i++) {
        if (!chosen[i])
            continue;
        if (spec->cases[i].repetitions == 0 ||
            spec->cases[i].repetitions > SIZE_MAX / sizeof(*it) - count) {
            free(chosen);
            errno = spec->cases[i].repetitions == 0 ? EINVAL : EOVERFLOW;
            return -1;
        }
        count += spec->cases[i].repetitions;
    }
    it = malloc(count * sizeof(*it));
    if (it == NULL) {
        free(chosen);
        errno = ENOMEM;
        return -1;
    }
    for (i = 0, k = 0; i < spec->ncases; i++) {
        unsigned rep;

        for (rep = 0; chosen[i] && rep < spec->cases[i].repetitions; rep++) {
            it[k].case_index = i;
            it[k].repetition = rep;
            it[k++].load = ETUDE_REPEAT_NO_LOAD;
        }
    }
    free(chosen);
    for (i = count; i > 1; i--) {
        j = (size_t)etude_rng_below(&rng, i);
        t = it[i - 1];
        it[i - 1] = it[j];
        it[j] = t;
    }
    for (i = 0; i < count; i++)
        if (etude_rng_below(&rng, 100) < spec->load_percent && spec->nloads > 0)
            it[i].load = (size_t)etude_rng_below(&rng, spec->nloads);
    *items = it;
    *n = count;
    return 0;
}

int etude_repeat_item_id(const char *run_id, const struct etude_repeat_case *c, unsigned rep,
                         char *buf, size_t size)
{
    int w = snprintf(buf, size, "%s:%s:%s:r%u", run_id, c->case_id, c->arch, rep);

    if (w < 0 || (size_t)w >= size || w > ID_LIMIT) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

void rep_string(struct cmp_text *t, const char *text)
{
    const unsigned char *c;

    cmp_printf(t, "\"");
    for (c = (const unsigned char *)text; *c; c++) {
        if (*c == '"' || *c == '\\')
            cmp_printf(t, "\\%c", *c);
        else if (*c < 0x20 || *c > 0x7e)
            cmp_printf(t, "\\u%04x", *c);
        else
            cmp_printf(t, "%c", *c);
    }
    cmp_printf(t, "\"");
}

char *etude_repeat_record(const struct etude_repeat_spec *spec, const char *run_id,
                          const struct etude_repeat_item *items, size_t n)
{
    struct cmp_text t = {NULL, 0, 0, 0};
    char id[ID_LIMIT + 1];
    size_t i;

    cmp_printf(&t, "{\"schema_version\":1,\"policy\":\"%s\",\"run_id\":", ETUDE_REPEAT_POLICY);
    rep_string(&t, run_id);
    cmp_printf(&t, ",\"seed\":%llu,\"pick\":%llu,\"load_percent\":%u,\"width\":%u,\"retries\":%u",
               (unsigned long long)spec->seed, (unsigned long long)spec->pick, spec->load_percent,
               spec->width, spec->retries);
    cmp_printf(&t, ",\"cases\":[");
    for (i = 0; i < spec->ncases; i++) {
        cmp_printf(&t, "%s{\"case_id\":", i ? "," : "");
        rep_string(&t, spec->cases[i].case_id);
        cmp_printf(&t, ",\"architecture\":");
        rep_string(&t, spec->cases[i].arch);
        cmp_printf(&t, ",\"repetitions\":%u}", spec->cases[i].repetitions);
    }
    cmp_printf(&t, "],\"loads\":[");
    for (i = 0; i < spec->nloads; i++) {
        cmp_printf(&t, "%s{\"load_id\":", i ? "," : "");
        rep_string(&t, spec->loads[i].load_id);
        cmp_printf(&t, ",\"kind\":\"%s\",\"amount\":%llu}", kind_names[spec->loads[i].kind],
                   (unsigned long long)spec->loads[i].amount);
    }
    cmp_printf(&t, "],\"items\":[");
    for (i = 0; i < n; i++) {
        const struct etude_repeat_case *c = &spec->cases[items[i].case_index];

        if (etude_repeat_item_id(run_id, c, items[i].repetition, id, sizeof(id)) != 0) {
            free(t.data);
            return NULL;
        }
        cmp_printf(&t, "%s{\"item_id\":", i ? "," : "");
        rep_string(&t, id);
        cmp_printf(&t, ",\"case_id\":");
        rep_string(&t, c->case_id);
        cmp_printf(&t, ",\"architecture\":");
        rep_string(&t, c->arch);
        cmp_printf(&t, ",\"repetition\":%u,\"load_id\":", items[i].repetition);
        if (items[i].load == ETUDE_REPEAT_NO_LOAD)
            cmp_printf(&t, "null");
        else
            rep_string(&t, spec->loads[items[i].load].load_id);
        cmp_printf(&t, "}");
    }
    cmp_printf(&t, "]}\n");
    if (t.failed) {
        free(t.data);
        errno = ENOMEM;
        return NULL;
    }
    return t.data;
}

/** Give the count of the members of array `a`. */
static size_t length_of(const struct eh_json *a)
{
    const struct eh_json *e;
    size_t n = 0;

    for (e = a != NULL ? a->child : NULL; e != NULL; e = e->next)
        n++;
    return n;
}

/** Give the integer member `name` of `o`, which the schema has checked. */
static uint64_t number_of(const struct eh_json *o, const char *name)
{
    return (uint64_t)eh_json_get(o, name)->number;
}

/** Compare the record's items with those its plan gives; 0 when equal. */
static int same_items(const struct eh_json *record, const struct etude_repeat_spec *spec,
                      const struct etude_repeat_item *items, size_t n, char *why, size_t size)
{
    const struct eh_json *e = eh_json_get(record, "items")->child, *load;
    const char *run = cmp_text(record, "run_id");
    char id[ID_LIMIT + 1];
    size_t i;

    if (length_of(eh_json_get(record, "items")) != n) {
        snprintf(why, size, "the record has %lu items, and its seed gives %lu",
                 (unsigned long)length_of(eh_json_get(record, "items")), (unsigned long)n);
        return -1;
    }
    for (i = 0; i < n; i++, e = e->next) {
        const struct etude_repeat_case *c = &spec->cases[items[i].case_index];
        const char *want =
            items[i].load == ETUDE_REPEAT_NO_LOAD ? NULL : spec->loads[items[i].load].load_id;

        load = eh_json_get(e, "load_id");
        if (etude_repeat_item_id(run, c, items[i].repetition, id, sizeof(id)) != 0 ||
            strcmp(cmp_text(e, "item_id"), id) != 0 || strcmp(cmp_text(e, "case_id"), c->case_id) ||
            strcmp(cmp_text(e, "architecture"), c->arch) ||
            number_of(e, "repetition") != items[i].repetition ||
            (want == NULL) != (load->type == 'z') || (want && strcmp(load->string, want))) {
            snprintf(why, size, "item %lu is %s, and the seed gives %s with load %s",
                     (unsigned long)i, cmp_text(e, "item_id"), id, want ? want : "none");
            return -1;
        }
    }
    return 0;
}

/** Draw the plan the record's inputs give, and compare its items. */
static int replan(const struct eh_json *record, char *why, size_t size)
{
    struct etude_repeat_spec spec;
    struct etude_repeat_case *cases;
    struct etude_load *loads;
    struct etude_repeat_item *items = NULL;
    const struct eh_json *e;
    size_t i, k, n = 0;
    int rc = -1;

    memset(&spec, 0, sizeof(spec));
    spec.ncases = length_of(eh_json_get(record, "cases"));
    spec.nloads = length_of(eh_json_get(record, "loads"));
    cases = calloc(spec.ncases + 1, sizeof(*cases));
    loads = calloc(spec.nloads + 1, sizeof(*loads));
    if (cases == NULL || loads == NULL) {
        snprintf(why, size, "memory ran out");
        goto out;
    }
    for (e = eh_json_get(record, "cases")->child, i = 0; e != NULL; e = e->next, i++) {
        cases[i].case_id = cmp_text(e, "case_id");
        cases[i].arch = cmp_text(e, "architecture");
        cases[i].repetitions = (unsigned)number_of(e, "repetitions");
        for (k = 0; k < i; k++)
            if (!strcmp(cases[k].case_id, cases[i].case_id) &&
                !strcmp(cases[k].arch, cases[i].arch)) {
                snprintf(why, size, "case %s on %s is listed twice", cases[i].case_id,
                         cases[i].arch);
                goto out;
            }
    }
    for (e = eh_json_get(record, "loads")->child, i = 0; e != NULL; e = e->next, i++) {
        loads[i].load_id = cmp_text(e, "load_id");
        for (k = 0; k < 3 && strcmp(cmp_text(e, "kind"), kind_names[k]); k++) {
        }
        loads[i].kind = (enum etude_load_kind)k;
        loads[i].amount = number_of(e, "amount");
    }
    spec.seed = number_of(record, "seed");
    spec.pick = (size_t)number_of(record, "pick");
    spec.load_percent = (unsigned)number_of(record, "load_percent");
    spec.width = (unsigned)number_of(record, "width");
    spec.retries = (unsigned)number_of(record, "retries");
    spec.cases = cases;
    spec.loads = loads;
    if (etude_repeat_plan(&spec, &items, &n) != 0) {
        snprintf(why, size, "its plan cannot be drawn: %s", strerror(errno));
        goto out;
    }
    rc = same_items(record, &spec, items, n, why, size);
out:
    free(items);
    free(cases);
    free(loads);
    return rc;
}

int etude_repeat_check(const char *json, const char *schema_path, char *why, size_t size)
{
    struct eh_json *record = NULL, *schema = NULL;
    char *text = NULL;
    size_t length;
    int rc = -1;

    if (cmp_read_file(schema_path, EH_MAX_LINE, &text, &length) != 0 ||
        (schema = eh_json_parse(text)) == NULL) {
        snprintf(why, size, "%s: the schema cannot be read", schema_path);
        goto out;
    }
    record = eh_json_parse(json);
    if (record == NULL) {
        snprintf(why, size, "the record is not one JSON value of these contracts");
        goto out;
    }
    if (!eh_json_validate(record, schema)) {
        snprintf(why, size, "the record does not have the shape of its schema");
        goto out;
    }
    rc = replan(record, why, size);
out:
    eh_json_free(record);
    eh_json_free(schema);
    free(text);
    return rc;
}

/** Order two tallies by case, architecture and workload. */
static int tally_cmp(const void *pa, const void *pb)
{
    const struct etude_repeat_tally *a = pa, *b = pb;
    int c;

    if ((c = strcmp(a->case_id, b->case_id)) != 0 || (c = strcmp(a->arch, b->arch)) != 0)
        return c;
    return strcmp(a->workload, b->workload);
}

/** Find the tally of `key` in `t`, or add it; NULL when memory ran out. */
static struct etude_repeat_tally *tally_of(struct etude_repeat_tally **t, size_t *n,
                                           const struct etude_repeat_tally *key)
{
    struct etude_repeat_tally *grown;
    size_t i;

    for (i = 0; i < *n; i++)
        if (tally_cmp(&(*t)[i], key) == 0)
            return &(*t)[i];
    grown = realloc(*t, (*n + 1) * sizeof(*grown));
    if (grown == NULL)
        return NULL;
    *t = grown;
    grown[*n] = *key;
    return &grown[(*n)++];
}

int etude_repeat_tally(const struct cmp_history *h, const char *run_id,
                       struct etude_repeat_tally **out, size_t *n)
{
    struct etude_repeat_tally key, *t;
    const struct eh_json *result, *disruptions;
    const char *outcome, *digest;
    size_t i;

    *out = NULL;
    *n = 0;
    for (i = 0; i < h->sample_count; i++) {
        const struct cmp_sample *s = &h->samples[i];

        if (s->item == NULL || (run_id != NULL && strcmp(s->attempt->run, run_id) != 0))
            continue;
        memset(&key, 0, sizeof(key));
        snprintf(key.case_id, sizeof(key.case_id), "%s", cmp_text(s->item, "case_id"));
        snprintf(key.arch, sizeof(key.arch), "%s", cmp_text(s->item, "architecture"));
        digest = cmp_digest(s->item, "workload");
        snprintf(key.workload, sizeof(key.workload), "%s", digest ? digest : "");
        if ((t = tally_of(out, n, &key)) == NULL) {
            free(*out);
            *out = NULL;
            *n = 0;
            return -1;
        }
        result = eh_json_get(s->attempt->data, "result");
        outcome = cmp_text(result, "outcome");
        disruptions = eh_json_get(result, "disruptions");
        t->retries += cmp_text(s->attempt->data, "previous_attempt_id") != NULL;
        if (disruptions != NULL && disruptions->child != NULL)
            t->disrupted++;
        else if (!strcmp(outcome, "pass"))
            t->pass++;
        else if (!strcmp(outcome, "failure"))
            t->failure++;
        else if (!strcmp(outcome, "incomplete"))
            t->incomplete++;
        else
            t->cancelled++;
    }
    for (i = 0; i < *n; i++)
        (*out)[i].flaky = (*out)[i].pass > 0 && (*out)[i].failure + (*out)[i].incomplete > 0;
    if (*n > 1)
        qsort(*out, *n, sizeof(**out), tally_cmp);
    return 0;
}
