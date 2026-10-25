/* Source capture reads a real committed tree and preserves failed outputs. */
#include "../../../tools/etude/measurement_prepare.c"

static void source_fixture_put(const char *name, const char *text)
{
    FILE *file = fopen(fix_path(name), "wb");
    CHECK(file, "source fixture opens");
    if (file) {
        CHECK(fputs(text, file) >= 0, "source fixture writes");
        CHECK(!fclose(file), "source fixture closes");
    }
}

static int source_git(const char *tree, char *const argv[])
{
    size_t size = 0;
    char *bytes = source_query(tree, argv, &size);
    int ok = bytes != NULL;
    free(bytes);
    return ok;
}

static void source_repository(char tree[4096], char current[41])
{
    snprintf(tree, 4096, "%s", fix_path("source-repo"));
    CHECK(!mkdir(tree, 0700), "source repository directory");
    source_fixture_put("source-repo/.gitignore", "build/\n");
    source_fixture_put("source-repo/source file.c", "tracked source\n");
    source_fixture_put("source-repo/empty", "");
    char *init[] = {"git", "-c", "init.defaultBranch=fixture", "init", "-q", NULL};
    char *add[] = {"git", "add", "--", ".gitignore", "source file.c", "empty", NULL};
    char *save[] = {"git",
                    "-c",
                    "user.name=Etude fixture",
                    "-c",
                    "user.email=fixture@example.invalid",
                    "-c",
                    "core.hooksPath=/dev/null",
                    "commit",
                    "-qm",
                    "fixture",
                    NULL};
    CHECK(source_git(tree, init) && source_git(tree, add) && source_git(tree, save),
          "source repository commits actual fixture bytes");
    char *head[] = {"git", "rev-parse", "HEAD", NULL};
    size_t size = 0;
    char *bytes = source_query(tree, head, &size);
    CHECK(bytes && size >= 40, "source repository commit is read");
    if (bytes && size >= 40) {
        memcpy(current, bytes, 40);
        current[40] = 0;
    }
    free(bytes);
}

static void check_source_snapshot(const char *output, const char *current, const char *prepared)
{
    struct json *document = NULL;
    CHECK(!routine_json(output, &document), "native source snapshot parses");
    if (!document)
        return;
    CHECK(json_text(document, "commit") && !strcmp(json_text(document, "commit"), current),
          "native source snapshot records actual commit");
    const struct json *inputs = json_member(document, "inputs");
    static const char *const paths[] = {".gitignore", "build/prepared.h", "empty", "source file.c"};
    const char *contents[] = {"build/\n", prepared, "", "tracked source\n"};
    CHECK(inputs && inputs->type == JSON_ARRAY && inputs->count == 4,
          "native source snapshot includes tracked and unique prepared inputs");
    if (inputs && inputs->count == 4)
        for (size_t i = 0; i < 4; i++) {
            const char *name = json_text(inputs->item[i], "path");
            const char *digest = json_text(inputs->item[i], "sha256");
            const struct json *size = json_member(inputs->item[i], "size_bytes");
            char expected[65];
            sha_text(contents[i], expected);
            CHECK(name && !strcmp(name, paths[i]) && size && size->type == JSON_INTEGER &&
                      size->integer == (long long)strlen(contents[i]) && digest &&
                      !strcmp(digest, expected),
                  "native source snapshot preserves sorted paths, lengths and actual bytes");
        }
    json_free(document);
}

static void source_memory_snapshot(const char *tree, const char *current, const char *const extra[],
                                   const char *output)
{
    struct jbuf bytes = {0};
    struct json *memory = NULL, *written = NULL;
    char error[160];
    CHECK(!etude_context_source_build(tree, current, extra, &bytes),
          "source capture builds in memory without a temporary file");
    CHECK(!json_parse(bytes.data, bytes.len, 1, &memory, error, sizeof error) &&
              !routine_json(output, &written) && json_equal(memory, written),
          "in-memory and published snapshots describe identical actual inputs");
    CHECK(etude_context_source_build(tree, current, extra, &bytes),
          "source capture refuses a nonempty destination buffer");
    json_free(memory);
    json_free(written);
    jbuf_free(&bytes);
}

static void test_source_capture(void)
{
    char tree[4096], current[41] = {0}, output[4096], refused[4096];
    source_repository(tree, current);
    CHECK(!mkdir(fix_path("source-repo/build"), 0700), "prepared source directory");
    source_fixture_put("source-repo/build/prepared.h", "prepared\n");
    snprintf(output, sizeof output, "%s", fix_path("source-snapshot"));
    snprintf(refused, sizeof refused, "%s", fix_path("source-refused"));
    const char *extra[] = {"build/prepared.h", "build/prepared.h", NULL};
    char *args[] = {"source",           "--tree", tree,      "--commit",         current,
                    "--output",         output,   "--extra", "build/prepared.h", "--extra",
                    "build/prepared.h", NULL};
    CHECK(!etude_prepare_source_main((int)(sizeof args / sizeof(*args)) - 1, args),
          "native committed source entry captures");
    check_source_snapshot(output, current, "prepared\n");
    source_memory_snapshot(tree, current, extra, output);
    CHECK(etude_context_source(tree, current, output, extra),
          "native source never replaces output");
    check_source_snapshot(output, current, "prepared\n");
    source_fixture_put("source-repo/source file.c", "modified source\n");
    CHECK(etude_context_source(tree, current, refused, extra) && access(refused, F_OK),
          "dirty source refuses before output publication");
    source_fixture_put("source-repo/source file.c", "tracked source\n");
    const char *bad[] = {"../outside", NULL};
    CHECK(etude_context_source(tree, current, refused, bad) && access(refused, F_OK),
          "source capture refuses a path outside the tree");
    CHECK(etude_context_source(tree, "1111111111111111111111111111111111111111", refused, extra) &&
              access(refused, F_OK),
          "source capture refuses a different commit");
    source_fixture_put("source-repo/build/prepared.h", "replaced\n");
    CHECK(!etude_context_source(tree, current, refused, extra),
          "changed prepared bytes produce a new actual source snapshot");
    check_source_snapshot(refused, current, "replaced\n");
    CHECK(!unlink(refused) && !unlink(fix_path("source-repo/build/prepared.h")) &&
              !symlink("../empty", fix_path("source-repo/build/prepared.h")),
          "source link fixture is created");
    CHECK(etude_context_source(tree, current, refused, extra) && access(refused, F_OK),
          "source capture refuses a linked input without publishing output");
}
