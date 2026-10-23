/* The routine entry requires an observed charge and a verified policy. */
#include "check.h"
#include "schema.h"
#include "sha256.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static int number(const struct json *v, const char *key, uint64_t *out)
{
    const struct json *n = json_member(v, key);
    if (!n || n->type != JSON_INTEGER || n->integer < 0)
        return 0;
    *out = (uint64_t)n->integer;
    return 1;
}

static int metric(const struct json *v, uint64_t *value, uint64_t *error)
{
    const char *quality = json_text(v, "quality");
    return (same(quality, "exact") || same(quality, "sampled")) && number(v, "value", value) &&
           number(v, "error_bound", error);
}

static struct json *policy_read(const struct cases *set, const struct json *ref, const char *store)
{
    const char *expected = json_text(ref, "sha256");
    char path[4096], hex[65], error[200];
    uint64_t size;
    if (!store || !expected || strlen(expected) != 64 || !number(ref, "size_bytes", &size) ||
        size > 1048576)
        return NULL;
    int n = snprintf(path, sizeof path, "%s/%s", store, expected);
    if (n < 0 || n >= (int)sizeof path)
        return NULL;
    FILE *file = fopen(path, "rb");
    char *bytes = malloc((size_t)size + 1);
    if (!file || !bytes) {
        if (file)
            fclose(file);
        free(bytes);
        return NULL;
    }
    size_t length = fread(bytes, 1, (size_t)size + 1, file);
    int failed = ferror(file) || length != size;
    failed |= fclose(file) != 0;
    if (failed) {
        free(bytes);
        return NULL;
    }
    struct sha256 hash;
    unsigned char digest[32];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)bytes, length);
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
    struct json *doc = NULL;
    if (length != size || strcmp(hex, expected) ||
        json_parse(bytes, length, 1, &doc, error, sizeof error)) {
        free(bytes);
        return NULL;
    }
    free(bytes);
    struct json *schema = read_schema(set->schemas, "budget");
    if (!schema || schema_findings(schema, doc, path)) {
        json_free(doc);
        doc = NULL;
    }
    json_free(schema);
    return doc;
}

static int limits(const struct json *budget, const struct json *policy, const char *purpose)
{
    static const char *const names[] = {"goal_us", "growth_us", "ceiling_us", "cleanup_reserve_us"};

    const struct json *cls = json_member(json_member(policy, "classes"), purpose);
    uint64_t recorded[4], expected;
    for (unsigned i = 0; i < 4; i++)
        if (!number(budget, names[i], &recorded[i]) || !number(cls, names[i], &expected) ||
            recorded[i] != expected)
            return 0;
    return recorded[0] <= recorded[2] && recorded[1] <= recorded[2] - recorded[0] &&
           recorded[3] < recorded[2];
}

static int charge(const struct json *run)
{
    const struct json *time = json_member(run, "time");
    const struct json *budget = json_member(run, "budget");
    uint64_t start, end, elapsed, error, charged, charge_error, ceiling, queue, once;
    if (!number(time, "start_us", &start) || !number(time, "end_us", &end) || end < start ||
        !metric(json_member(time, "elapsed_us"), &elapsed, &error) || elapsed != end - start ||
        !metric(json_member(budget, "charged_us"), &charged, &charge_error) || charged != elapsed ||
        !number(budget, "ceiling_us", &ceiling) || charged > ceiling)
        return 0;
    if (charge_error > ceiling - charged || error > ceiling - elapsed)
        return 0;
    /* This entry charges the whole interval. Subtracted queue/setup costs
     * need their own record-level judgement, not a claimed zero-cost gap. */
    return number(json_member(budget, "outside_queue_us"), "value", &queue) && !queue &&
           number(json_member(budget, "one_time_setup_us"), "value", &once) && !once;
}

int judge_run_budget(const struct cases *set, const struct json *plan, const struct json *run,
                     const char *store)
{
    const char *purpose = json_text(plan, "purpose");
    const struct json *budget = json_member(run, "budget");
    const struct json *ref = json_member(budget, "policy");
    const struct json *planned = json_member(plan, "policies");
    uint64_t planned_size, recorded_size;
    struct json *policy = policy_read(set, ref, store);
    const struct json *site = json_member(policy, "site");
    const struct json *env = json_member(plan, "environment");
    const struct json *clock = json_member(json_member(run, "time"), "clock");
    int ok = (same(purpose, "push") || same(purpose, "ci") || same(purpose, "release")) &&
             same(json_text(budget, "verdict"), "pass") && policy &&
             same(json_text(planned, "sha256"), json_text(ref, "sha256")) &&
             number(planned, "size_bytes", &planned_size) &&
             number(ref, "size_bytes", &recorded_size) && planned_size == recorded_size &&
             limits(budget, policy, purpose) && charge(run);
    static const char *const fields[] = {"machine_id", "os", "accelerator"};

    for (unsigned i = 0; i < 3; i++)
        ok = ok && same(json_text(site, fields[i]), json_text(env, fields[i]));
    ok = ok && same(json_text(clock, "machine_id"), json_text(env, "machine_id")) &&
         same(json_text(clock, "boot_id"), json_text(env, "boot_id"));
    json_free(policy);
    if (!ok)
        finding(NULL, 0,
                "routine testing needs a passing purpose-bound budget with a verified policy and "
                "full observed charge");
    return ok;
}
