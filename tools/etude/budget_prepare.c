/* Match aggregate preparation costs to their complete declared node set. */
#include "budget.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

struct preparation_sample {
    const struct eh_json *environment;
    uint64_t elapsed;
    const char *end;
};

static int same(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static struct eh_json *document(const struct eh_json *ref)
{
    const char *hex = cmp_text(ref, "sha256");
    const struct eh_json *size = eh_json_get(ref, "size_bytes");
    const struct eh_json *locations = eh_json_get(ref, "locations");
    if (!hex || strlen(hex) != 64 || !size || size->type != 'n' || size->number < 0 ||
        size->number > 16 * 1024 * 1024 || !locations || locations->type != 'a')
        return NULL;
    for (const struct eh_json *at = locations->child; at; at = at->next) {
        char *bytes;
        size_t length;
        if (at->type != 's' || cmp_read_file(at->string, (size_t)size->number, &bytes, &length))
            continue;
        struct sha256 hash;
        unsigned char digest[32];
        char actual[65];
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)bytes, length);
        sha256_final(&hash, digest);
        sha256_hex(digest, actual);
        struct eh_json *doc =
            length == (size_t)size->number && !strcmp(actual, hex) ? eh_json_parse(bytes) : NULL;
        free(bytes);
        if (doc)
            return doc;
    }
    return NULL;
}

static int completed_clean(const struct eh_json *receipt)
{
    const struct eh_json *c = eh_json_get(receipt, "conditioning");
    const char *commit = cmp_text(receipt, "commit"), *record = cmp_text(receipt, "record_sha256");
    return same(cmp_text(receipt, "format"), "etude-preparation-receipt") &&
           same(cmp_text(receipt, "outcome"), "pass") &&
           same(cmp_text(receipt, "cleanup"), "confirmed") && commit && strlen(commit) == 40 &&
           record && strlen(record) == 64 && same(cmp_text(c, "inhouse"), "clean");
}

/* Native goals are rebuilt locally. Portable goals retain their producer. */
static int imported_goal(const struct eh_json *proof, const char *node)
{
    const char *role = cmp_text(eh_json_get(proof, "roles"), node);
    if (same(role, "native") || same(role, "inventory"))
        return 1;
    const struct eh_json *producer = eh_json_get(proof, "ordinary_production");
    const struct eh_json *c = eh_json_get(producer, "conditioning");
    const struct eh_json *input = eh_json_get(eh_json_get(proof, "imported"), node);
    const struct eh_json *artifact = eh_json_get(eh_json_get(proof, "artifacts"), node);
    const char *hash = cmp_text(input, "sha256");
    return same(role, "portable") && input && artifact && hash && strlen(hash) == 64 &&
           same(hash, cmp_text(artifact, "sha256")) && completed_clean(producer) &&
           same(cmp_text(c, "backend"), "ordinary") && eh_json_get(c, "provenance") &&
           eh_json_get(c, "provenance")->type == 'z' &&
           same(cmp_text(producer, "commit"), cmp_text(proof, "commit"));
}

static int conditioned(const struct eh_json *receipt, const char *const *nodes, size_t count,
                       int aggregate)
{
    const struct eh_json *c = eh_json_get(receipt, "conditioning");
    if (!completed_clean(receipt))
        return 0;
    if (same(cmp_text(c, "backend"), "ordinary"))
        return eh_json_get(c, "provenance") && eh_json_get(c, "provenance")->type == 'z';
    if (!same(cmp_text(c, "backend"), "import"))
        return 0;
    struct eh_json *proof = document(eh_json_get(c, "provenance"));
    int ok = proof && same(cmp_text(proof, "format"), "etude-preparation-import-provenance") &&
             same(cmp_text(proof, "commit"), cmp_text(receipt, "commit"));
    for (size_t i = 0; ok && i < count; i++)
        ok = imported_goal(proof, nodes[i]);
    const struct eh_json *roles = eh_json_get(proof, "roles");
    for (const struct eh_json *r = aggregate && roles ? roles->child : NULL; ok && r; r = r->next)
        ok = imported_goal(proof, r->key);
    eh_json_free(proof);
    return ok;
}

static int nodes_match(const struct eh_json *doc, const struct etude_preparation_selection *s)
{
    const struct eh_json *nodes = eh_json_get(doc, "nodes");
    const struct eh_json *arches = eh_json_get(doc, "architectures");
    const struct eh_json *kernel = eh_json_get(doc, "kernel");
    const struct eh_json *version = eh_json_get(doc, "schema_version");
    unsigned mask = 0;
    if (!version || version->type != 'n' || version->number != 1 ||
        !same(cmp_text(doc, "format"), "etude-preparation-nodes") || !nodes || nodes->type != 'a' ||
        !arches || arches->type != 'a' || !kernel || kernel->type != 'z')
        return 0;
    for (const struct eh_json *a = arches->child; a; a = a->next)
        mask |= same(a->string, "i686") ? 1 : same(a->string, "x86_64") ? 2 : 4;
    if (mask != s->architectures)
        return 0;
    size_t count = 0;
    for (const struct eh_json *n = nodes->child; n; n = n->next) {
        if (n->type != 's')
            return 0;
        size_t matches = 0;
        for (size_t i = 0; i < s->count; i++)
            matches += !strcmp(n->string, s->nodes[i]);
        for (const struct eh_json *p = nodes->child; p != n; p = p->next)
            if (same(n->string, p->string))
                return 0;
        if (matches != 1)
            return 0;
        count++;
    }
    return count == s->count &&
           (s->cls != ETUDE_BUDGET_RELEASE ||
            conditioned(eh_json_get(doc, "preparation"), s->nodes, s->count, 1));
}

int etude_budget_preparation_eligible(const struct cmp_record *plan, enum etude_budget_class cls,
                                      const char *node)
{
    if (cls != ETUDE_BUDGET_RELEASE)
        return 1;
    const struct eh_json *refs = eh_json_get(plan->data, "artifacts");
    for (const struct eh_json *r = refs ? refs->child : NULL; r; r = r->next) {
        struct eh_json *doc = document(r);
        const struct eh_json *nodes = eh_json_get(doc, "nodes");
        int found = 0;
        for (const struct eh_json *n = nodes ? nodes->child : NULL; n; n = n->next)
            found |= n->type == 's' && same(n->string, node);
        int ok = found && same(cmp_text(doc, "format"), "etude-preparation-nodes") &&
                 conditioned(eh_json_get(doc, "preparation"), &node, 1, 0);
        eh_json_free(doc);
        if (ok)
            return 1;
    }
    return 0;
}

static int declares(const struct cmp_record *plan, const struct etude_preparation_selection *s)
{
    const struct eh_json *refs = eh_json_get(plan->data, "artifacts");
    for (const struct eh_json *r = refs ? refs->child : NULL; r; r = r->next) {
        struct eh_json *doc = document(r);
        int match = doc && nodes_match(doc, s);
        eh_json_free(doc);
        if (match)
            return 1;
    }
    return 0;
}

static int passed(const struct eh_json *result)
{
    const struct eh_json *disruptions = eh_json_get(result, "disruptions");
    return same(cmp_text(result, "outcome"), "pass") &&
           same(cmp_text(result, "cleanup"), "confirmed") && disruptions && !disruptions->child;
}

static int step_sample(const struct cmp_record *step, const struct eh_json *environment,
                       struct preparation_sample *out)
{
    const struct eh_json *item = eh_json_get(step->data, "item_id");
    const struct eh_json *time = eh_json_get(step->data, "time");
    const struct eh_json *elapsed = eh_json_get(time, "elapsed_us");
    const struct eh_json *value = eh_json_get(elapsed, "value"),
                         *error = eh_json_get(elapsed, "error_bound");
    if (!item || item->type != 'z' || !value || value->type != 'n' || value->number < 0 || !error ||
        error->type != 'n' || same(cmp_text(elapsed, "quality"), "lower_bound"))
        return 0;
    out->environment = environment;
    out->elapsed = (uint64_t)value->number;
    out->end = cmp_text(time, "end_utc");
    return out->end != NULL;
}

static int sample(const struct cmp_history *h, const struct etude_budget_policy *p,
                  const struct etude_preparation_selection *s, const struct cmp_record *run,
                  struct preparation_sample *out)
{
    const struct cmp_record *plan = cmp_find(h, cmp_text(run->data, "plan_id"));
    const struct eh_json *refs = eh_json_get(run->data, "preparation_steps");
    if (!plan || strcmp(plan->run, run->run) || !run->load_eligible ||
        !passed(eh_json_get(run->data, "result")) || !refs || !refs->child || refs->child->next)
        return 0;
    const char *purpose = cmp_text(plan->data, "purpose");
    if (!same(purpose, etude_budget_class_name(s->cls)) &&
        !(s->cls == ETUDE_BUDGET_RELEASE && same(purpose, "phase_first_run")))
        return 0;
    const struct eh_json *env = eh_json_get(plan->data, "environment");
    if (!same(cmp_text(env, "machine_id"), p->machine_id) || !same(cmp_text(env, "os"), p->os) ||
        !same(cmp_text(env, "accelerator"), p->accelerator) || !declares(plan, s))
        return 0;
    const struct cmp_record *step = cmp_find(h, refs->child->string);
    if (!step || strcmp(step->kind, "step") || strcmp(step->run, run->run) ||
        (!same(cmp_text(step->data, "stage"), "build") &&
         !same(cmp_text(step->data, "stage"), "prepare")) ||
        !same(cmp_text(step->data, "plan_id"), plan->id) ||
        !passed(eh_json_get(step->data, "result")))
        return 0;
    return step_sample(step, env, out);
}

static int same_environment(const struct eh_json *a, const struct eh_json *b)
{
    return same(cmp_text(a, "os_version"), cmp_text(b, "os_version")) &&
           same(cmp_digest(a, "site_profile"), cmp_digest(b, "site_profile")) &&
           same(cmp_digest(a, "tools"), cmp_digest(b, "tools")) &&
           same(cmp_digest(a, "cache_state"), cmp_digest(b, "cache_state"));
}

int etude_budget_preparation(const struct cmp_history *h, const struct etude_budget_policy *p,
                             const struct etude_preparation_selection *selection, uint64_t *us,
                             size_t *count)
{
    struct preparation_sample *samples = calloc(h->runs + 1, sizeof(*samples));
    uint64_t *values = calloc(h->runs + 1, sizeof(*values));
    struct cmp_stats stats;
    size_t n = 0, newest = 0;
    int rc = -1;
    *count = 0;
    *us = 0;
    if (!samples || !values)
        goto done;
    for (size_t i = 0; i < h->runs; i++)
        if (sample(h, p, selection, h->by_run[i], &samples[n])) {
            if (strcmp(samples[n].end, samples[newest].end) > 0)
                newest = n;
            n++;
        }
    for (size_t i = 0; i < n; i++)
        if (same_environment(samples[i].environment, samples[newest].environment))
            values[(*count)++] = samples[i].elapsed;
    memset(&stats, 0, sizeof(stats));
    if (*count && cmp_statistics(values, *count, &stats))
        goto done;
    *us = stats.median_us;
    rc = 0;
done:
    free(samples);
    free(values);
    return rc;
}
