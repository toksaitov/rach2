/* style.c: verify source-bound evidence from the build's source checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "source_snapshot.h"
#ifdef _WIN32
#include "win/files.h"
#endif

static struct json *read_json(const char *path)
{
    struct jbuf bytes = {0};
    struct json *value = NULL;
    char block[8192], why[256];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    while ((n = fread(block, 1, sizeof(block), f))) {
        if (bytes.len + n > 4u * 1024 * 1024)
            break;
        jbuf_put(&bytes, block, n);
    }
    if (!ferror(f) && feof(f) && json_parse(bytes.data, bytes.len, 1, &value, why, sizeof(why)))
        fprintf(stderr, "style: %s: %s\n", path, why);
    fclose(f);
    jbuf_free(&bytes);
    return value;
}

static const struct json *find_case(const struct json *manifest, const char *id)
{
    const struct json *cases = json_member(manifest, "cases");
    for (size_t i = 0; cases && i < cases->count; i++) {
        const char *name = json_text(cases->item[i], "case_id");
        if (name && !strcmp(name, id))
            return cases->item[i];
    }
    return NULL;
}

static int same(const struct json *value, const char *key, const char *expected)
{
    const char *text = json_text(value, key);
    return text && !strcmp(text, expected);
}

static int assertions_match(const struct json *record, const struct json *definition)
{
    const struct json *got = json_member(record, "assertions");
    const struct json *want = json_member(definition, "assertions");
    if (!got || got->type != JSON_ARRAY || !want || got->count != want->count)
        return 0;
    for (size_t i = 0; i < want->count; i++) {
        const char *id = json_text(want->item[i], "assertion_id");
        int found = 0;
        if (!id)
            return 0;
        for (size_t j = 0; j < got->count; j++) {
            if (got->item[j]->type != JSON_STRING)
                return 0;
            found += !strcmp(got->item[j]->string, id);
        }
        if (found != 1)
            return 0;
    }
    return 1;
}

static int branches_match(const struct json *record, const struct json *definition)
{
    const struct json *got = json_member(record, "branches");
    const struct json *want = json_member(definition, "branches");
    if (!got || got->type != JSON_ARRAY || !want || got->count != want->count)
        return 0;
    for (size_t i = 0; i < want->count; i++) {
        const char *group = json_text(want->item[i], "group_id");
        int found = 0;
        if (!group)
            return 0;
        for (size_t j = 0; j < got->count; j++) {
            const char *alternative = json_text(got->item[j], "alternative");
            if (!alternative)
                return 0;
            if (same(got->item[j], "group", group)) {
                const struct json *alternatives = json_member(want->item[i], "alternatives");
                for (size_t k = 0; alternatives && k < alternatives->count; k++)
                    if (same(alternatives->item[k], "alternative_id", alternative) &&
                        (!strcmp(alternative, "recorded") || !strcmp(alternative, "present") ||
                         !strcmp(alternative, "judged")))
                        found++;
            }
        }
        if (found != 1)
            return 0;
    }
    return 1;
}

static int record_matches(const struct json *record, const struct json *definition, const char *id,
                          const char *arch, const char *snapshot)
{
    const struct json *version = json_member(record, "version");
    return version && version->type == JSON_INTEGER && version->integer == 1 &&
           same(record, "case_id", id) && same(record, "architecture", arch) &&
           same(record, "source_sha256", snapshot) && assertions_match(record, definition) &&
           branches_match(record, definition);
}

static void report(const struct json *record, const char *id)
{
    const struct json *assertions = json_member(record, "assertions");
    const struct json *branches = json_member(record, "branches");
    printf("@etude 1 begin %s\n", id);
    printf("style: source checks ran during preparation; this run verified their complete source "
           "digest\n");
    for (size_t i = 0; i < branches->count; i++)
        printf("@etude 1 branch %s %s\n", json_text(branches->item[i], "group"),
               json_text(branches->item[i], "alternative"));
    for (size_t i = 0; i < assertions->count; i++)
        printf("@etude 1 assert %s pass\n", assertions->item[i]->string);
    printf("@etude 1 end %lu\n", (unsigned long)(assertions->count + branches->count));
}

/** Verify prepared source evidence or print its exact source fingerprint. */
int main(int argc, char **argv)
{
    const char *tree = getenv("ETUDE_TREE"), *arch = getenv("ARCH"),
               *expected = getenv("ETUDE_CASE");
    char path[4096], id[128], snapshot[65];
    struct json *manifest = NULL, *record = NULL;
    const struct json *definition, *architectures;
    int rc = 2;
    if (!tree)
        tree = ".";
    if (argc == 3 && !strcmp(argv[1], "--fingerprint")) {
#ifdef _WIN32
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            fputs("style: cannot set binary fingerprint output\n", stderr);
            return 2;
        }
#endif
        if (source_snapshot(argv[2], snapshot))
            return 2;
        puts(snapshot);
        return 0;
    }
    if (argc != 2 || strlen(argv[1]) > 80 ||
        strspn(argv[1], "abcdefghijklmnopqrstuvwxyz-") != strlen(argv[1]))
        return 2;
    snprintf(id, sizeof(id), "host.style.%s", argv[1]);
    if (expected && strcmp(expected, id))
        return 2;
    if (snprintf(path, sizeof(path), "%s/tests/cases/host.style.json", tree) >= (int)sizeof(path))
        return 2;
    manifest = read_json(path);
    definition = find_case(manifest, id);
    if (!definition)
        goto done;
    architectures = json_member(definition, "architectures");
    if (architectures && architectures->type == JSON_STRING)
        arch = "none";
    else if (!arch)
        arch = "i686";
    if (strcmp(arch, "none") && strcmp(arch, "i686") && strcmp(arch, "x86_64"))
        goto done;
    if (snprintf(path, sizeof(path), "%s/build%s/tests/style/%s.json", tree,
                 !strcmp(arch, "x86_64") ? "/x86_64" : "", argv[1]) >= (int)sizeof(path))
        goto done;
    record = read_json(path);
    if (!record || source_snapshot(tree, snapshot) ||
        !record_matches(record, definition, id, arch, snapshot)) {
        fprintf(stderr,
                "style: %s: source-check evidence is absent, incomplete or for different sources; "
                "prepare it on the checking host\n",
                id);
        goto done;
    }
    report(record, id);
    rc = 0;
done:
    json_free(record);
    json_free(manifest);
    return rc;
}
