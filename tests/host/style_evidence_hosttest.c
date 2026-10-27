/* Source evidence must cover the current files and every required assertion.
 * host-deps: tests/cases/host/source_snapshot.c
 * host-deps: tools/etude/json.c
 * host-deps: utils/sha256.c
 * host-cflags: -Itools/etude -Iutils
 */
#include <unistd.h>
#include <sys/stat.h>
#include "compat/tree_host.h"
#define main style_entry
#include "../cases/host/style.c"
#undef main

static int failures;
static char work[4096];

static void cleanup(void)
{
    if (*work)
        host_remove_tree(work);
}

static void check(int held, const char *label)
{
    if (!held) {
        fprintf(stderr, "style evidence: %s\n", label);
        failures++;
    }
}

static int file(const char *name, const char *value)
{
    char path[4096];
    FILE *f;
    if (snprintf(path, sizeof(path), "%s/%s", work, name) >= (int)sizeof(path) ||
        !(f = fopen(path, "wb")))
        return -1;
    int bad = fwrite(value, 1, strlen(value), f) != strlen(value);
    if (fclose(f))
        bad = 1;
    return bad ? -1 : 0;
}

static void snapshot_cases(void)
{
    char before[65], after[65], path[4096];
    snprintf(path, sizeof(path), "%s/tests", work);
    check(!mkdir(path, 0700), "create source directory");
    check(!file("tests/example.c", "int value = 1;\n"), "write source");
    check(!source_snapshot(work, before), "read source snapshot");
    check(!file("tests/example.c", "int value = 2;\n") && !source_snapshot(work, after) &&
              strcmp(before, after),
          "a changed source invalidates evidence");
    check(!file("tests/example.c", "int value = 1;\n") && !source_snapshot(work, after) &&
              !strcmp(before, after),
          "restored bytes have the same digest");
    check(!file("tests/new.c", "int another;\n") && !source_snapshot(work, after) &&
              strcmp(before, after),
          "an added source invalidates evidence");
    snprintf(path, sizeof(path), "%s/tests/new.c", work);
    check(!unlink(path) && !source_snapshot(work, after) && !strcmp(before, after),
          "deleting the added source restores the inventory");
    check(!file("tests/example.o", "compiled output") && !source_snapshot(work, after) &&
              !strcmp(before, after),
          "a compiler output is outside the source inventory");
    snprintf(path, sizeof(path), "%s/tests/example.c", work);
    check(!unlink(path) && !source_snapshot(work, after) && strcmp(before, after),
          "a removed source invalidates evidence");
    check(!source_snapshot(work, before) && !file("tests/probe-suites", "hello default\n") &&
              !source_snapshot(work, after) && strcmp(before, after),
          "a probe assignment list invalidates evidence");
    check(!file("tests/install", "#!/bin/sh\nexit 0\n") && !source_snapshot(work, before) &&
              !file("tests/install", "#!/bin/sh\nexit 1\n") && !source_snapshot(work, after) &&
              strcmp(before, after),
          "an extensionless script invalidates evidence");
    check(!file("tests/blob", "data 0\n") && !source_snapshot(work, before) &&
              !file("tests/blob", "data 1\n") && !source_snapshot(work, after) &&
              !strcmp(before, after),
          "an extensionless file without a script line is outside the inventory");
    snprintf(path, sizeof(path), "%s/tests/native", work);
    check(!mkdir(path, 0700) && !file("tests/native/probes.cases", "hello\n") &&
              !source_snapshot(work, before) && !file("tests/native/probes.cases", "other\n") &&
              !source_snapshot(work, after) && strcmp(before, after),
          "a native case list invalidates evidence");
    check(!file("tests/native/machine", "hello\n") && !source_snapshot(work, after) &&
              strcmp(before, after),
          "an added native inventory invalidates evidence");
}

static struct json *parse(const char *text)
{
    struct json *result = NULL;
    char why[256];
    check(!json_parse(text, strlen(text), 1, &result, why, sizeof(why)), "parse fixture");
    return result;
}

static void record_cases(void)
{
    const char *base =
        "{\"version\":1,\"case_id\":\"host.style.clang-format\","
        "\"architecture\":\"none\",\"source_sha256\":\"abc\",\"assertions\":[\"formatted\"],"
        "\"branches\":[{\"group\":\"version\",\"alternative\":\"recorded\"}]}";
    struct json *definition =
        parse("{\"assertions\":[{\"assertion_id\":\"formatted\"}],"
              "\"branches\":[{\"group_id\":\"version\",\"alternatives\":["
              "{\"alternative_id\":\"recorded\"},{\"alternative_id\":\"other\"}]}]}");
    struct json *record = parse(base), *part;
    check(record_matches(record, definition, "host.style.clang-format", "none", "abc"),
          "complete recorded evidence passes");
    check(!record_matches(record, definition, "host.style.clang-format", "none", "changed"),
          "a different source digest is rejected");
    check(!record_matches(record, definition, "host.style.clang-format", "x86_64", "abc"),
          "a different architecture is rejected");
    part = (struct json *)json_member(record, "assertions");
    part->count = 0;
    check(!record_matches(record, definition, "host.style.clang-format", "none", "abc"),
          "an omitted assertion is rejected");
    part->count = 1;
    part = (struct json *)json_member(json_member(record, "branches")->item[0], "alternative");
    free(part->string);
    part->string = strdup("other");
    check(!record_matches(record, definition, "host.style.clang-format", "none", "abc"),
          "an unrecorded judge is rejected");
    free(part->string);
    part->string = strdup("present");
    check(!record_matches(record, definition, "host.style.clang-format", "none", "abc"),
          "an alternative of another group is rejected");
    json_free(record);
    json_free(definition);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp)
        tmp = "/tmp";
    atexit(cleanup);
    if (snprintf(work, sizeof(work), "%s/style-evidence.XXXXXX", tmp) >= (int)sizeof(work) ||
        !mkdtemp(work))
        return 1;
    snapshot_cases();
    record_cases();
    return failures != 0;
}
