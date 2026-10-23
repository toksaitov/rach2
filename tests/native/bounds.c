/* Fix native bounds from matching complete successes before work starts. */
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *fields[] = {"case", "gap", "transfer", "capture", "cleanup"};
static enum etude_stage stages[] = {ETUDE_STAGE_EXECUTE, ETUDE_STAGE_TRANSFER,
                                     ETUDE_STAGE_CAPTURE, ETUDE_STAGE_CLEANUP};
static const char *names[] = {"execute", "transfer", "capture", "cleanup"};
static uint64_t number(const struct eh_json *o, const char *name)
{
    const struct eh_json *v = eh_json_get(o, name);
    return v && v->type == 'n' && v->number > 0 ? (uint64_t)v->number : 0;
}
static int declarations(struct nc_history *h)
{
    char path[4096], *bytes = NULL;
    size_t length;
    snprintf(path, sizeof(path), "%s/tests/native/bounds.json", h->conf->tree);
    if (cmp_read_file(path, 65536, &bytes, &length))
        return 0;
    struct eh_json *doc = eh_json_parse(bytes);
    free(bytes);
    int ok = doc && cmp_text(doc, "policy") &&
             !strcmp(cmp_text(doc, "policy"), ETUDE_BOUND_POLICY);
    for (size_t i = 0; ok && i < 5; i++) {
        const struct eh_json *d = eh_json_get(doc, fields[i]);
        struct etude_bound *b = &h->declared[i];
        b->floor_us = number(d, "floor_us");
        b->initial_us = number(d, "initial_us");
        b->ceiling_us = number(d, "ceiling_us");
        struct etude_bound_fixed fixed;
        ok = etude_bound_fix(b, NULL, 0, &fixed) == 0;
    }
    eh_json_free(doc);
    return ok;
}
static int step_value(const struct cmp_history *h, const struct cmp_sample *s,
                       const char *stage, int gap, uint64_t *value)
{
    const struct eh_json *steps = eh_json_get(s->attempt->data, "steps");
    for (const struct eh_json *id = steps ? steps->child : NULL; id; id = id->next) {
        const struct cmp_record *r = cmp_find(h, id->string);
        if (!r || strcmp(r->kind, "step") || strcmp(r->run, s->attempt->run) ||
            strcmp(cmp_text(r->data, "plan_id"), s->plan->id) ||
            strcmp(cmp_text(r->data, "attempt_id"), cmp_text(s->attempt->data, "attempt_id")) ||
            strcmp(cmp_text(r->data, "stage"), stage))
            continue;
        const struct eh_json *result = eh_json_get(r->data, "result");
        const struct eh_json *d = eh_json_get(result, "disruptions");
        const struct eh_json *metric = gap ? eh_json_get(r->data, "max_progress_gap_us") :
            eh_json_get(eh_json_get(r->data, "time"), "elapsed_us");
        const struct eh_json *v = eh_json_get(metric, "value");
        if (strcmp(cmp_text(result, "outcome"), "pass") ||
            strcmp(cmp_text(result, "cleanup"), "confirmed") || (d && d->child) ||
            !v || v->type != 'n' || v->number < 0)
            continue;
        *value = (uint64_t)v->number;
        return 1;
    }
    return 0;
}
static int matches(struct nc_history *h, struct nc_measure *m, const struct cmp_sample *s)
{
    const struct eh_json *env = eh_json_get(s->plan->data, "environment");
    return h->conf->machine && h->conf->kernel_sha256 && s->watchdog_eligible && !strcmp(s->key.value[0], m->case_id) &&
           !strcmp(s->key.value[1], h->conf->arch) && !strcmp(s->key.value[2], "native") &&
           !strcmp(s->key.value[3], h->machine) && !strcmp(s->key.value[4], "native") &&
           !strcmp(s->key.value[5], m->workload) && !strcmp(s->key.value[6], m->assertions) &&
           !strcmp(s->key.value[7], m->profile) && !strcmp(cmp_text(env, "os_version"), h->version);
}
static int learn(struct nc_history *h, struct nc_measure *m, unsigned field,
                  struct etude_bound_fixed *fixed)
{
    uint64_t *samples = calloc(h->prior.sample_count + 1, sizeof(*samples));
    const char **runs = calloc(h->prior.sample_count + 1, sizeof(*runs));
    size_t count = 0;
    if (!samples || !runs) {
        free(samples);
        free(runs);
        return 0;
    }
    for (size_t i = 0; i < h->prior.sample_count; i++) {
        const struct cmp_sample *s = &h->prior.samples[i];
        if (!matches(h, m, s))
            continue;
        size_t j;
        for (j = 0; j < count && strcmp(runs[j], s->attempt->run); j++) {}
        if (j < count)
            continue;
        uint64_t value = s->elapsed_us;
        if (field && !step_value(&h->prior, s, field == 1 ? "execute" : fields[field],
                                  field == 1, &value))
            continue;
        runs[count] = s->attempt->run;
        samples[count++] = value;
    }
    int ok = etude_bound_fix(&h->declared[field], samples, count, fixed) == 0;
    free(samples);
    free(runs);
    return ok;
}
int nc_bounds(struct nc_history *h)
{
    if (!declarations(h))
        return 0;
    for (size_t n = 0; n < h->count; n++) {
        struct nc_measure *m = &h->items[n];
        struct etude_bound_fixed *b[] = {&m->whole, &m->gap, &m->transfer, &m->capture, &m->cleanup};
        for (unsigned i = 0; i < 5; i++)
            if (!learn(h, m, i, b[i]))
                return 0;
        m->spec.case_us = m->whole.us;
        m->spec.gap_us = m->gap.us;
        m->spec.stage_us[ETUDE_STAGE_TRANSFER] = m->transfer.us;
        m->spec.stage_us[ETUDE_STAGE_CAPTURE] = m->capture.us;
        m->spec.stage_us[ETUDE_STAGE_CLEANUP] = m->cleanup.us;
        m->spec.witnesses = m->witnesses;
        m->spec.witness_count = m->nwitnesses;
    }
    return 1;
}
static void stage(void *context, const char *name)
{
    struct nc_measure *m = context;
    for (size_t i = 0; i < 4; i++) {
        if (strcmp(name, names[i]) || m->stage == stages[i])
            continue;
        struct etude_reading now;
        if (etude_history_now(&now))
            return;
        m->stage_end[m->stage] = now;
        m->stages[m->stage] += now.monotonic_us - m->stage_begin[m->stage].monotonic_us;
        m->stage = stages[i];
        m->stage_begin[m->stage] = now;
        etude_watchdog_stage(m->watchdog, m->stage, now.monotonic_us);
        return;
    }
}
static void output(void *context, const char *bytes, size_t length)
{
    struct nc_measure *m = context;
    struct etude_reading now;
    if (!etude_history_now(&now))
        etude_watchdog_feed(m->watchdog, bytes, length, now.monotonic_us);
}
static uint64_t deadline(void *context)
{
    return etude_watchdog_deadline(((struct nc_measure *)context)->watchdog);
}
static int expired(void *context)
{
    struct nc_measure *m = context;
    struct etude_reading now;
    return etude_history_now(&now) || etude_watchdog_check(m->watchdog, now.monotonic_us);
}
int nc_measure_start(struct nc_history *h, struct nc_measure *m)
{
    (void)h;
    if (etude_history_now(&m->start))
        return 0;
    m->stage = ETUDE_STAGE_TRANSFER;
    m->stage_begin[m->stage] = m->start;
    m->watchdog = etude_watchdog_new(&m->spec, m->stage, m->start.monotonic_us);
    m->observer.context = m;
    m->observer.stage = stage;
    m->observer.output = output;
    m->observer.deadline = deadline;
    m->observer.expired = expired;
    return m->watchdog != NULL;
}
void nc_measure_end(struct nc_measure *m)
{
    if (!etude_history_now(&m->end)) {
        m->stage_end[m->stage] = m->end;
        m->stages[m->stage] += m->end.monotonic_us - m->stage_begin[m->stage].monotonic_us;
    }
}
