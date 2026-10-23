/* Comparison requires hash-bound continuous observations of the run. */
#include <stdlib.h>
#include <string.h>
#include "compare.h"
#include "etude/observe.h"
#include "sha256.h"

static int yes(const struct eh_json *v, const char *key)
{
    const struct eh_json *n = eh_json_get(v, key);
    return n && n->type == 't';
}

static int integer(const struct eh_json *v, const char *key, uint64_t *out)
{
    const struct eh_json *n = eh_json_get(v, key);
    if (!n || n->type != 'n' || n->number < 0) return 0;
    *out = (uint64_t)n->number;
    return 1;
}

/* A location is a hint. Its bytes must satisfy both the size and digest. */
static char *blob(const struct eh_json *ref, size_t *length)
{
    uint64_t size;
    const char *expected = cmp_text(ref, "sha256");
    const struct eh_json *locations = eh_json_get(ref, "locations");
    if (!expected || strlen(expected) != 64 || !integer(ref, "size_bytes", &size) ||
        size > ETUDE_OBSERVE_BYTES || !locations || locations->type != 'a') return NULL;
    for (const struct eh_json *p = locations->child; p; p = p->next) {
        char *bytes;
        size_t count;
        if (p->type != 's' || cmp_read_file(p->string, (size_t)size, &bytes, &count)) continue;
        struct sha256 hash;
        unsigned char digest[32];
        char hex[65];
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)bytes, count);
        sha256_final(&hash, digest);
        sha256_hex(digest, hex);
        if (count == size && !strcmp(hex, expected)) { *length = count; return bytes; }
        free(bytes);
    }
    return NULL;
}

static int observation(const struct eh_json *v, const char *run, uint64_t start, uint64_t end)
{
    if (!v || !yes(eh_json_get(v, "summary"), "comparison_available")) return 0;
    size_t size;
    char *bytes = blob(eh_json_get(v, "trace"), &size);
    if (!bytes) return 0;
    char *summary = etude_observe_import(bytes, size, run, start, end);
    free(bytes);
    if (!summary) return 0;
    free(summary);
    return 1;
}

/* Evaluate once per run before indexing its attempts; a trace is not reread
 * for every case in a large collection. */
void cmp_check_load(struct cmp_record *run)
{
    const struct eh_json *ref = eh_json_get(run->data, "load");
    const struct eh_json *time = eh_json_get(run->data, "time");
    size_t size;
    char *bytes = blob(ref, &size);
    struct eh_json *load = bytes && !memchr(bytes, 0, size) ? eh_json_parse(bytes) : NULL;
    uint64_t start = etude_observe_utc(cmp_text(time, "start_utc"));
    uint64_t end = etude_observe_utc(cmp_text(time, "end_utc")), runtime;
    const char *method = cmp_text(load, "method");
    const struct eh_json *required = eh_json_get(load, "physical_required");
    run->load_eligible = load && method && !strcmp(method, "native-load-v1") &&
        yes(load, "comparison_available") && yes(load, "context_eligible") &&
        start && end >= start && integer(load, "runtime_start_utc_us", &runtime) &&
        runtime >= start && runtime <= end && required &&
        (required->type == 't' || required->type == 'f') &&
        observation(eh_json_get(load, "runtime"), run->run, runtime, end) &&
        observation(eh_json_get(load, "outer"), run->run, start, end) &&
        (required->type == 'f' || observation(eh_json_get(load, "physical"), run->run, start, end));
    eh_json_free(load);
    free(bytes);
}
