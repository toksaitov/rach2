/* Comparison requires hash-bound continuous observations of the run. */
#include <stdlib.h>
#include <string.h>
#include "compare.h"
#include "etude/observe.h"
#include "sha256.h"
#include "routine_charge.h"

static int yes(const struct eh_json *v, const char *key)
{
    const struct eh_json *n = eh_json_get(v, key);
    return n && n->type == 't';
}

static int integer(const struct eh_json *v, const char *key, uint64_t *out)
{
    const struct eh_json *n = eh_json_get(v, key);
    if (!n || n->type != 'n' || n->number < 0)
        return 0;
    *out = (uint64_t)n->number;
    return 1;
}

/* A location is a hint. Its bytes must satisfy both the size and digest. */
char *cmp_blob_bytes(const struct eh_json *ref, size_t *length)
{
    uint64_t size;
    const char *expected = cmp_text(ref, "sha256");
    const struct eh_json *locations = eh_json_get(ref, "locations");
    if (!expected || strlen(expected) != 64 || !integer(ref, "size_bytes", &size) ||
        size > ETUDE_OBSERVE_BYTES || !locations || locations->type != 'a')
        return NULL;
    for (const struct eh_json *p = locations->child; p; p = p->next) {
        char *bytes;
        size_t count;
        if (p->type != 's' || cmp_read_file(p->string, (size_t)size, &bytes, &count))
            continue;
        struct sha256 hash;
        unsigned char digest[32];
        char hex[65];
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)bytes, count);
        sha256_final(&hash, digest);
        sha256_hex(digest, hex);
        if (count == size && !strcmp(hex, expected)) {
            *length = count;
            return bytes;
        }
        free(bytes);
    }
    return NULL;
}

int cmp_routine_cost(const struct cmp_record *plan, const struct cmp_record *run,
                     struct etude_routine_cost *cost)
{
    const struct eh_json *refs = eh_json_get(plan->data, "artifacts");
    for (const struct eh_json *r = refs ? refs->child : NULL; r; r = r->next) {
        const struct eh_json *size = eh_json_get(r, "size_bytes");
        const struct eh_json *paths = eh_json_get(r, "locations");
        struct json *doc =
            size && size->type == 'n' && size->number >= 0 && paths && paths->child &&
                    paths->child->type == 's'
                ? etude_routine_document(cmp_text(r, "sha256"), (uint64_t)size->number,
                                         paths->child->string)
                : NULL;
        int rc = 0;
        if (doc && json_text(doc, "format") &&
            !strcmp(json_text(doc, "format"), "etude-preparation-nodes")) {
            const struct etude_routine_input in = {
                doc,          run->run,   cmp_text(plan->data, "commit"), NULL, NULL,
                plan->digest, run->digest};
            rc = etude_routine_charge(&in, cost);
        }
        json_free(doc);
        if (rc)
            return rc;
    }
    const struct eh_json *budget = eh_json_get(run->data, "budget");
    const char *reason = cmp_text(eh_json_get(budget, "charged_us"), "reason");
    return reason && !strcmp(reason, "whole-PC owner cleanup is pending") ? -1 : 0;
}

static int observation(const struct eh_json *v, const char *run, uint64_t start, uint64_t end)
{
    if (!v || !yes(eh_json_get(v, "summary"), "comparison_available"))
        return 0;
    size_t size;
    char *bytes = cmp_blob_bytes(eh_json_get(v, "trace"), &size);
    if (!bytes)
        return 0;
    char *summary = etude_observe_import(bytes, size, run, start, end);
    free(bytes);
    if (!summary)
        return 0;
    free(summary);
    return 1;
}

/* Evaluate once per run before indexing its attempts; a trace is not reread
 * for every case in a large collection. */
void cmp_check_load(struct cmp_record *run, const struct cmp_record *plan)
{
    const struct eh_json *ref = eh_json_get(run->data, "load");
    const struct eh_json *time = eh_json_get(run->data, "time");
    size_t size;
    char *bytes = cmp_blob_bytes(ref, &size);
    struct eh_json *load = bytes && !memchr(bytes, 0, size) ? eh_json_parse(bytes) : NULL;
    uint64_t start = etude_observe_utc(cmp_text(time, "start_utc"));
    uint64_t end = etude_observe_utc(cmp_text(time, "end_utc")), runtime;
    const char *method = cmp_text(load, "method");
    const struct eh_json *required = eh_json_get(load, "physical_required");
    run->local_load_eligible = load && method && !strcmp(method, "native-load-v1") &&
                               yes(load, "context_eligible") && start && end >= start &&
                               integer(load, "runtime_start_utc_us", &runtime) &&
                               runtime >= start && runtime <= end && required &&
                               (required->type == 't' || required->type == 'f') &&
                               observation(eh_json_get(load, "runtime"), run->run, runtime, end) &&
                               observation(eh_json_get(load, "outer"), run->run, start, end);
    run->load_eligible =
        run->local_load_eligible && yes(load, "comparison_available") &&
        (required->type == 'f' || observation(eh_json_get(load, "physical"), run->run, start, end));
    eh_json_free(load);
    free(bytes);
    if (plan) {
        struct etude_routine_cost cost;
        int route = cmp_routine_cost(plan, run, &cost);
        run->routine_load = route < 0 ? -1 : route && !cost.load_available ? 2 : route;
    }
}
