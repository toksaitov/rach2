/*
 * The rows of etude_legacy_hosttest.c that run etude's host preparation,
 * `etude prepare -H`, on fixture trees: how it partitions the etude checks
 * and the scripts cases, how it reuses a predecessor's programs and tools,
 * how it records inputs and prepared external tools, and how it builds and
 * reuses the Windows companion programs.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixdir_host.h"
#include "prepare_fixture.h"

/* The modification time of `path` in nanoseconds, or -1. */
static long long mtime_ns(const char *path)
{
    struct stat st;

    if (stat(path, &st))
        return -1;
#ifdef __APPLE__
    return st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

/* The checks of an inventory. */
static const struct json *checks_of(const struct json *record)
{
    const struct json *checks = json_member(record, "checks");

    return checks && checks->type == JSON_ARRAY ? checks : NULL;
}

/* The program of the inventory's check `i`, in `tree`. */
static const char *program_of(const char *tree, const struct json *record, size_t i)
{
    const struct json *checks = checks_of(record);

    return pf_at(tree, checks && i < checks->count ? json_text(checks->item[i], "program") : "");
}

/* Add the assertions of the etude case `c` to `owners`, each true when the
 * case is the quiet one, whose checks run alone. */
static void case_owners(const struct json *c, struct json *owners)
{
    const struct json *progress = json_member(c, "progress");
    const struct json *assertions = json_member(c, "assertions");
    int quiet = !strcmp(json_text(c, "case_id"), "host.host.etude_quiet");
    long long limit = -1;
    size_t j;

    for (j = 0; progress && j < progress->count; j++) {
        const char *witness = json_text(progress->item[j], "witness_id");
        if (witness && !strcmp(witness, "checks"))
            limit = json_member(progress->item[j], "limit")->integer;
    }
    PF_CHECK(assertions && limit == (long long)assertions->count,
             "partition: a case's progress limit counts its assertions");
    for (j = 0; assertions && j < assertions->count; j++) {
        const char *assertion = json_text(assertions->item[j], "assertion_id");
        if (!assertion || !strcmp(assertion, "listed"))
            continue;
        PF_CHECK(!json_member(owners, assertion), "partition: one owner for each check");
        pf_jset(owners, assertion, pf_jbool(quiet));
    }
}

/* The owners of the etude host checks in the source tree's manifest, and a
 * check that only the three that need it run alone. */
static struct json *etude_owners(void)
{
    static const char *const alone[] = {"etude_platform", "etude_remote", "etude_repeat"};
    struct json *manifest = pf_jload(pf_at(pf_repo, "tests/cases/host.host.json"));
    const struct json *cases = json_member(manifest, "cases");
    struct json *owners = pf_jobj();
    size_t i, nalone = 0;

    for (i = 0; cases && i < cases->count; i++) {
        const char *id = json_text(cases->item[i], "case_id");
        if (id && (!strcmp(id, "host.host.etude") || !strcmp(id, "host.host.etude_quiet")))
            case_owners(cases->item[i], owners);
    }
    for (i = 0; i < owners->count; i++)
        nalone += owners->item[i]->integer != 0;
    for (i = 0; i < sizeof(alone) / sizeof(*alone); i++)
        PF_CHECK(json_member(owners, alone[i]) && json_member(owners, alone[i])->integer,
                 "partition: the checks that run alone");
    PF_CHECK(nalone == sizeof(alone) / sizeof(*alone), "partition: no other check runs alone");
    json_free(manifest);
    return owners;
}

/* Give `tree` a stand-in for each etude host test of the source tree that
 * boots no kernel, alone when the test is, and check that each has an owner. */
static void etude_sources(const char *tree, const struct json *owners)
{
    struct dirent **names = NULL;
    int n = scandir(pf_at(pf_repo, "tests/host"), &names, NULL, alphasort), i;

    for (i = 0; i < n; i++) {
        const char *name = names[i]->d_name;
        size_t len = strlen(name);
        char identity[256], *text = NULL;
        if (!strncmp(name, "etude_", 6) && len > 11 && !strcmp(name + len - 11, "_hosttest.c"))
            text = pf_read(pf_at(pf_at(pf_repo, "tests/host"), name), NULL);
        if (text && !strstr(text, "host-arch:")) {
            fix_format(identity, sizeof(identity), "%.*s", (int)(len - 11), name);
            PF_CHECK(json_member(owners, identity) != NULL, "partition: each test has an owner");
            if (!json_member(owners, identity))
                printf("etude-prepare-test: %s has no owner\n", identity);
            pf_native_source(tree, name,
                             strstr(text, "host-alone:") ? "host-alone: native isolation\n" : "",
                             NULL);
        }
        free(text);
        free(names[i]);
    }
    free(names);
}

/** Check etude's partition of its own host checks into shared and alone. */
static void partition_modes(void)
{
    struct json *owners = etude_owners();
    char work[PF_PATH], tree[PF_PATH];
    size_t i;
    int windows;

    pf_scratch("partition", work);
    fix_format(tree, sizeof(tree), "%s/tree", work);
    pf_native_tree(tree);
    etude_sources(tree, owners);
    for (windows = 0; windows < 2; windows++) {
        const char *env[] = {windows ? "HOST_TEST_PLATFORM=windows" : NULL, NULL};
        struct json *record;
        const struct json *checks;
        if (windows)
            pf_windows_tree(tree);
        record = pf_native_prepare(tree, "etude", NULL, env, 0, NULL);
        checks = checks_of(record);
        PF_CHECK(checks && checks->count == owners->count, "partition: every owner prepared");
        for (i = 0; checks && i < checks->count; i++) {
            const char *id = json_text(checks->item[i], "assertion_id");
            const struct json *owner = id ? json_member(owners, id) : NULL;
            const char *mode = json_text(checks->item[i], "mode");
            PF_CHECK(owner && mode && !strcmp(mode, owner->integer ? "alone" : "shared"),
                     "partition: each check's mode");
        }
        json_free(record);
    }
    json_free(owners);
    pf_cleanup(work);
}

/*
 * Copy into `tree` each file of the source tree that the recipe `text`
 * names, a path that begins `rach2/` or `tests/` and ends `.c`, `.h` or
 * `.py`, so that the scripts group finds its fixtures' sources.
 */
static void copy_named(const char *tree, const char *text)
{
    static const char allowed[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./-";
    const char *p = text;

    while (*p) {
        size_t start = !strncmp(p, "rach2/", 6) ? 6 : !strncmp(p, "tests/", 6) ? 6 : 0;
        size_t run, end, k;
        char name[1024];
        struct stat st;
        if (!start) {
            p++;
            continue;
        }
        run = start + strspn(p + start, allowed);
        for (end = 0, k = run; k > start + 1 && !end; k--) {
            if ((k >= start + 3 && !strncmp(p + k - 2, ".c", 2)) ||
                (k >= start + 3 && !strncmp(p + k - 2, ".h", 2)) ||
                (k >= start + 4 && !strncmp(p + k - 3, ".py", 3)))
                end = k;
        }
        if (!end) {
            p++;
            continue;
        }
        fix_format(name, sizeof(name), "%.*s", (int)end, p);
        if (!stat(pf_at(pf_repo, name), &st) && S_ISREG(st.st_mode))
            PF_CHECK(!pf_copy(pf_at(pf_repo, name), pf_at(tree, name)), "scripts: a fixture");
        p += end;
    }
}

/** Check the scripts group: its 44 checks, and the refusal of a manifest
 * that lacks one of them or gives one twice. */
static void script_partition_routes(void)
{
    static const char *const print[] = {"--print-commands", NULL};
    char tree[PF_PATH];
    struct json *manifest, *assertion = NULL, *assertions = NULL;
    struct jbuf out = {0};
    char *recipe, *list, *line, *next;
    size_t names = 0, unique = 0, i, extra;
    struct json *seen = pf_jobj();
    const struct json *cases;

    pf_scratch("scripts", tree);
    pf_native_tree(tree);
    PF_CHECK(!pf_copy(pf_at(pf_repo, "tests/cases/host/host.list"),
                      pf_at(tree, "tests/cases/host/host.list")) &&
                 !pf_copy(pf_at(pf_repo, "tests/cases/host/adapters.json"),
                          pf_at(tree, "tests/cases/host/adapters.json")) &&
                 !pf_copy(pf_at(pf_repo, "tests/cases/host.host.json"),
                          pf_at(tree, "tests/cases/host.host.json")),
             "scripts: the manifests");
    recipe = pf_read(pf_at(pf_repo, "mk/host-fixtures.mk"), NULL);
    copy_named(tree, recipe ? recipe : "");
    free(recipe);
    PF_CHECK(!pf_copy(pf_at(pf_repo, "mk/host-fixtures.mk"), pf_at(tree, "mk/host-fixtures.mk")),
             "scripts: the fixtures' recipe");
    json_free(pf_native_prepare(tree, "scripts", print, NULL, 0, &out));
    PF_CHECK(out.data && strstr(out.data, "fixtures/chown_plan.c"),
             "scripts: the kernel fixture's command");
    jbuf_free(&out);
    list = pf_read(pf_at(tree, "tests/cases/host/host.list"), NULL);
    for (line = list; line && *line; line = next) {
        next = line + strcspn(line, "\n");
        if (*next)
            *next++ = 0;
        if (!*line || *line == '#')
            continue;
        line[strcspn(line, "|")] = 0;
        names++;
        if (!json_member(seen, line)) {
            unique++;
            pf_jset(seen, line, pf_jnull());
        }
    }
    free(list);
    json_free(seen);
    PF_CHECK(names == 44 && unique == 44, "scripts: 44 checks, each named once");
    manifest = pf_jload(pf_at(tree, "tests/cases/host.host.json"));
    cases = json_member(manifest, "cases");
    for (i = 0; cases && i < cases->count; i++) {
        const char *id = json_text(cases->item[i], "case_id");
        if (id && !strcmp(id, "host.host.scripts_images"))
            assertions = pf_jget(cases->item[i], "assertions");
    }
    for (i = 0; assertions && i < assertions->count && !assertion; i++) {
        const char *id = json_text(assertions->item[i], "assertion_id");
        if (id && !strcmp(id, "mkext2_overlay")) {
            assertion = assertions->item[i];
            memmove(assertions->item + i, assertions->item + i + 1,
                    (assertions->count - i - 1) * sizeof(*assertions->item));
            assertions->count--;
        }
    }
    PF_CHECK(assertion != NULL, "scripts: the overlay check is declared");
    for (extra = 0; assertion && extra <= 2; extra += 2) {
        for (i = 0; i < extra; i++)
            pf_jadd(assertions, pf_jclone(assertion));
        PF_CHECK(!pf_jsave(pf_at(tree, "tests/cases/host.host.json"), manifest),
                 "scripts: the changed manifest");
        json_free(pf_native_prepare(tree, "scripts", print, NULL, 1, NULL));
    }
    json_free(assertion);
    json_free(manifest);
    pf_cleanup(tree);
}

/* A GNU preparation of `next` that may reuse `old`'s programs, from scratch;
 * check how many programs it compiled and each one's exit status. */
static struct json *retry(const char *old, const char *next, size_t compiled, int first, int second)
{
    const char *args[] = {"--gnu", "--previous", old, NULL};
    struct json *record = NULL;
    struct jbuf out = {0};

    unlink(pf_at(next, "build/tests/host-suite/tests.inventory.json"));
    record = pf_native_prepare(next, "tests", args, NULL, 0, &out);
    PF_CHECK(pf_count(out.data, " -o ") == compiled, "predecessor: the programs compiled");
    PF_CHECK(pf_status(program_of(next, record, 0)) == first &&
                 pf_status(program_of(next, record, 1)) == second,
             "predecessor: each program's status");
    if (pf_count(out.data, " -o ") != compiled)
        printf("etude-prepare-test: expected %zu compiles:\n%s", compiled, out.data);
    jbuf_free(&out);
    return record;
}

/** Check that a GNU preparation reuses a predecessor's programs exactly when
 * their compiler inputs are unchanged, and never trusts a corrupt one. */
static void gnu_inventory_predecessor(void)
{
    const char *args[] = {"--gnu", NULL};
    const char *trees[2];
    struct json *record = NULL, *current;
    const struct json *inputs;
    char *original[2] = {0};
    size_t sizes[2] = {0}, i;
    char work[PF_PATH], old_tree[PF_PATH], new_tree[PF_PATH];

    pf_scratch("gnu-reuse", work);
    fix_format(old_tree, sizeof(old_tree), "%s/old", work);
    fix_format(new_tree, sizeof(new_tree), "%s/new", work);
    trees[0] = old_tree;
    trees[1] = new_tree;
    for (i = 0; i < 2; i++) {
        pf_native_tree(trees[i]);
        PF_CHECK(!pf_write_text(pf_at(trees[i], "include/choice.h"), "#define VALUE 0\n") &&
                     !pf_write_text(pf_at(trees[i], "runtime.txt"), "initial runtime bytes\n"),
                 "predecessor: tree");
        pf_native_source(trees[i], "first_hosttest.c",
                         "host-cflags: -Iinclude\nhost-inputs: runtime.txt\n",
                         "#include \"choice.h\"\nint main(void) { return VALUE; }\n");
        pf_native_source(trees[i], "second_hosttest.c", "", NULL);
    }
    record = pf_native_prepare(old_tree, "tests", args, NULL, 0, NULL);
    for (i = 0; i < 2; i++)
        original[i] = pf_read(program_of(old_tree, record, i), &sizes[i]);
    json_free(retry(old_tree, new_tree, 0, 0, 0));
    pf_write_text(pf_at(new_tree, "runtime.txt"), "new runtime bytes\n");
    current = retry(old_tree, new_tree, 0, 0, 0);
    inputs = checks_of(current) ? json_member(checks_of(current)->item[0], "compile_inputs") : NULL;
    PF_CHECK(inputs != NULL, "predecessor: compile inputs recorded");
    for (i = 0; inputs && i < inputs->count; i++) {
        const char *path = json_text(inputs->item[i], "path");
        PF_CHECK(!path || strcmp(path, "runtime.txt"), "predecessor: runtime data is no input");
    }
    json_free(current);
    pf_write_text(pf_at(new_tree, "include/choice.h"), "#define VALUE 1\n");
    json_free(retry(old_tree, new_tree, 1, 1, 0));
    pf_write_text(pf_at(new_tree, "include/choice.h"), "#define VALUE 0\n");
    pf_write_text(pf_at(new_tree, "tests/host/choice.h"), "#define VALUE 2\n");
    json_free(retry(old_tree, new_tree, 1, 2, 0));
    unlink(pf_at(new_tree, "tests/host/choice.h"));
    pf_native_source(new_tree, "second_hosttest.c", "", "int main(void) { return 3; }\n");
    json_free(retry(old_tree, new_tree, 1, 0, 3));
    pf_native_source(new_tree, "first_hosttest.c", "host-cflags: -Iinclude -DCHANGED=1\n",
                     "#include \"choice.h\"\nint main(void) { return VALUE; }\n");
    json_free(retry(old_tree, new_tree, 2, 0, 3));
    for (i = 0; i < 2; i++) {
        size_t size;
        char *now = pf_read(program_of(old_tree, record, i), &size);
        PF_CHECK(now && original[i] && size == sizes[i] && !memcmp(now, original[i], size),
                 "predecessor: its programs unchanged");
        free(now);
        free(original[i]);
    }
    PF_CHECK(!pf_write_text(program_of(old_tree, record, 0), "corrupt predecessor"),
             "predecessor: corrupted");
    json_free(retry(old_tree, new_tree, 2, 0, 3));
    json_free(record);
    pf_cleanup(work);
}

/* Make the trees `old` and `new` with a check that declares the tool
 * `mkfatfixture`, whose source is `tool`. */
static void tool_trees(const char *old, const char *next, const char *tool, int header)
{
    const char *trees[] = {old, next};
    size_t i;

    for (i = 0; i < 2; i++) {
        pf_native_tree(trees[i]);
        pf_native_source(trees[i], "probe_hosttest.c", "host-tools: mkfatfixture\n", NULL);
        if (header)
            PF_CHECK(!pf_write_text(pf_at(trees[i], "tests/host/value.h"), "#define VALUE 0\n"),
                     "tool: header");
        PF_CHECK(!pf_write_text(pf_at(trees[i], "tests/host/mkfatfixture.c"), tool),
                 "tool: source");
    }
}

/** Check that a GNU preparation copies a predecessor's tool and its record
 * when they are current, and builds its own, alone, when they are not. */
static void shared_tool_predecessor(void)
{
    static const char program[] = "build/tests/host-suite/tools/mkfatfixture";
    static const char marker[] = "build/tests/host-suite/tools/mkfatfixture.build.json";
    char work[PF_PATH], old[PF_PATH], next[PF_PATH];
    const char *gnu[] = {"--gnu", NULL};
    const char *previous[] = {"--gnu", "--previous", old, NULL};
    struct jbuf out = {0};
    struct stat a, b;
    char *before_program, *before_marker;
    size_t program_size = 0;

    pf_scratch("tool-predecessor", work);
    fix_format(old, sizeof(old), "%s/old", work);
    fix_format(next, sizeof(next), "%s/new", work);
    tool_trees(old, next, "#include \"value.h\"\nint main(void) { return VALUE; }\n", 1);
    json_free(pf_native_prepare(old, "tests", gnu, NULL, 0, NULL));
    before_program = pf_read(pf_at(old, program), &program_size);
    before_marker = pf_read(pf_at(old, marker), NULL);
    json_free(pf_native_prepare(next, "tests", previous, NULL, 0, &out));
    PF_CHECK(pf_count(out.data, " -o ") == 0, "tool predecessor: no compile");
    jbuf_free(&out);
    PF_CHECK(pf_same_file(pf_at(old, program), pf_at(next, program)) &&
                 pf_same_file(pf_at(old, marker), pf_at(next, marker)),
             "tool predecessor: the tool and its record copied");
    PF_CHECK(!stat(pf_at(old, program), &a) && !stat(pf_at(next, program), &b) &&
                 a.st_ino != b.st_ino,
             "tool predecessor: a copy of its own");
    pf_write_text(pf_at(next, "tests/host/value.h"), "#define VALUE 1\n");
    json_free(pf_native_prepare(next, "tests", gnu, NULL, 0, &out));
    PF_CHECK(pf_count(out.data, " -o ") == 1, "tool predecessor: one compile");
    jbuf_free(&out);
    PF_CHECK(pf_status(pf_at(next, program)) == 1, "tool predecessor: the new tool");
    {
        size_t size;
        char *now_program = pf_read(pf_at(old, program), &size);
        char *now_marker = pf_read(pf_at(old, marker), NULL);
        PF_CHECK(now_program && before_program && size == program_size &&
                     !memcmp(now_program, before_program, size) && now_marker && before_marker &&
                     !strcmp(now_marker, before_marker),
                 "tool predecessor: the predecessor unchanged");
        free(now_program);
        free(now_marker);
    }
    free(before_program);
    free(before_marker);
    pf_cleanup(work);
}

/** Check that a predecessor's tool record of the older form, without its
 * sealed entry, does not stop a preparation. */
static void legacy_tool_predecessor(void)
{
    static const char program[] = "build/tests/host-suite/tools/mkfatfixture";
    char work[PF_PATH], old[PF_PATH], next[PF_PATH], marker[PF_PATH];
    const char *gnu[] = {"--gnu", NULL};
    const char *previous[] = {"--gnu", "--previous", old, NULL};
    struct json *record;

    pf_scratch("tool-legacy", work);
    fix_format(old, sizeof(old), "%s/old", work);
    fix_format(next, sizeof(next), "%s/new", work);
    tool_trees(old, next, "int main(void) { return 4; }\n", 0);
    json_free(pf_native_prepare(old, "tests", gnu, NULL, 0, NULL));
    fix_format(marker, sizeof(marker), "%s/%s.build.json", old, program);
    record = pf_jload(marker);
    PF_CHECK(record && json_member(record, "inventory_entry"), "legacy tool: a sealed entry");
    if (record) {
        pf_jdel(record, "inventory_entry");
        pf_jsave(marker, record);
    }
    json_free(record);
    json_free(pf_native_prepare(next, "tests", previous, NULL, 0, NULL));
    PF_CHECK(pf_status(pf_at(next, program)) == 4, "legacy tool: the tool built");
    pf_cleanup(work);
}

/* Append `name` to `out` as Python's json.dumps writes a string, each
 * character past ASCII as a \u escape. */
static void dumps_string(struct jbuf *out, const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    char escape[16];

    jbuf_puts(out, "\"");
    while (*p) {
        unsigned code = *p++;
        if (code >= 0xc0 && code < 0xe0 && *p) {
            code = ((code & 0x1f) << 6) | (*p++ & 0x3f);
        } else if (code >= 0xe0 && code < 0xf0 && p[0] && p[1]) {
            code = ((code & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
            p += 2;
        }
        if (code == '"' || code == '\\') {
            snprintf(escape, sizeof(escape), "\\%c", code);
        } else if (code < 0x20 || code >= 0x7f) {
            snprintf(escape, sizeof(escape), "\\u%04x", code);
        } else {
            escape[0] = (char)code;
            escape[1] = 0;
        }
        jbuf_puts(out, escape);
    }
    jbuf_puts(out, "\"");
}

/* Order two strings, for qsort. */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/** Check that a current exFAT fixture's marker is kept as it is, and that an
 * input whose name is not ASCII is recorded by its bytes in hex. */
static void native_exfat_filename(void)
{
    static const char *const paths[] = {
        "exfat_external.img",
        "exfat_external.manifest",
        "writer",
        "exfat_src/hello.txt",
        "exfat_src/pattern.bin",
        "exfat_src/Sub Dir/inner.txt",
        "exfat_src/empty",
        "exfat_src/e\xc3\xa9t\xc3\xa9.txt",
        "exfat_src/A name of some length that takes more than one name entry.txt"};
    static const char name[] = "tests/host/e\xc3\xa9t\xc3\xa9.txt";
    enum { N = sizeof(paths) / sizeof(*paths) };
    const char *sorted[N];
    char tree[PF_PATH], root[PF_PATH], marker[PF_PATH], hex[65], encoded[256], *original;
    const char *gnu[] = {"--gnu", NULL};
    struct jbuf text = {0};
    struct json *record = NULL;
    const struct json *inputs;
    size_t i, found = 0, plain = 1;

    pf_scratch("exfat-record", tree);
    fix_format(root, sizeof(root), "%s/build/tests/host-suite/tools/exfat_external", tree);
    pf_native_tree(tree);
    PF_CHECK(!pf_write_text(pf_at(tree, "tests/host/mkexfat_external.sh"), "exit 91\n"),
             "exfat: its producer");
    for (i = 0; i < N; i++) {
        PF_CHECK(!pf_write_text(pf_at(root, paths[i]), "fixture bytes\n"), "exfat: a file");
        sorted[i] = paths[i];
    }
    qsort(sorted, N, sizeof(*sorted), by_name);
    fix_format(marker, sizeof(marker), "%s/build/tests/host/exfat_writer", tree);
    jbuf_puts(&text, "{\"files\": {");
    for (i = 0; i < N; i++) {
        jbuf_puts(&text, i ? ", " : "");
        dumps_string(&text, sorted[i]);
        jbuf_puts(&text, ": ");
        pf_sha_file(pf_at(root, sorted[i]), hex);
        dumps_string(&text, hex);
    }
    pf_sha_file(pf_at(tree, "tests/host/mkexfat_external.sh"), hex);
    jbuf_puts(&text, "}, \"producer_sha256\": ");
    dumps_string(&text, hex);
    jbuf_puts(&text, "}\n");
    PF_CHECK(!pf_write_text(marker, text.data), "exfat: the marker");
    original = text.data;
    {
        const char *argv[] = {pf_etude,         "prepare", "-H",   "--only",
                              "external-exfat", "tools",   marker, NULL};
        struct jbuf out = {0};
        int status = pf_run(tree, NULL, argv, &out);
        char *now = pf_read(marker, NULL);
        PF_CHECK(status == 0, "exfat: a current fixture is reused");
        PF_CHECK(now && !strcmp(now, original), "exfat: its marker unchanged");
        if (status)
            printf("etude-prepare-test: external-exfat gave %d:\n%s", status, out.data);
        free(now);
        jbuf_free(&out);
    }
    jbuf_free(&text);
    PF_CHECK(!pf_write_text(pf_at(tree, name), "input bytes\n"), "exfat: the input");
    pf_native_source(tree, "probe_hosttest.c", "host-inputs: tests/host/e\xc3\xa9t\xc3\xa9.txt\n",
                     NULL);
    record = pf_native_prepare(tree, "tests", gnu, NULL, 0, NULL);
    for (i = 0; i < sizeof(name) - 1; i++)
        snprintf(encoded + 2 * i, 3, "%02x", (unsigned char)name[i]);
    pf_sha_file(pf_at(tree, name), hex);
    inputs = checks_of(record) ? json_member(checks_of(record)->item[0], "inputs") : NULL;
    for (i = 0; inputs && i < inputs->count; i++) {
        const char *row_hex = json_text(inputs->item[i], "path_hex");
        const char *path = json_text(inputs->item[i], "path");
        const char *sha = json_text(inputs->item[i], "sha256");
        found += row_hex && sha && inputs->item[i]->count == 2 && !strcmp(row_hex, encoded) &&
                 !strcmp(sha, hex);
        for (; path && *path; path++)
            plain &= (unsigned char)*path < 0x80;
    }
    PF_CHECK(found == 1, "exfat: the input recorded by path_hex");
    PF_CHECK(plain, "exfat: every path is ASCII");
    json_free(record);
    pf_cleanup(tree);
}

/** Check that a Windows preparation publishes a platform program by the name
 * the inventory gives, without the suffix the compiler added. */
static void native_program_publication(void)
{
    const char *env[] = {"HOST_TEST_PLATFORM=windows", NULL};
    char work[PF_PATH], tree_path[PF_PATH], suffixed[PF_PATH];
    struct json *record = NULL;
    struct stat st;
    const char *program;

    pf_scratch("native-publication", work);
    fix_format(tree_path, sizeof(tree_path), "%s/tree", work);
    pf_native_tree(tree_path);
    pf_native_source(tree_path, "etude_platform_hosttest.c", "host-alone: native isolation\n",
                     NULL);
    pf_windows_tree(tree_path);
    record = pf_native_prepare(tree_path, "etude", NULL, env, 0, NULL);
    program = program_of(tree_path, record, 0);
    fix_format(suffixed, sizeof(suffixed), "%s.exe", program);
    PF_CHECK(!stat(program, &st) && S_ISREG(st.st_mode), "publication: the program");
    PF_CHECK(!pf_exists(suffixed), "publication: no suffixed copy");
    json_free(record);
    pf_cleanup(work);
}

/* Check the prepared copies of the tools in `names`, each from the file
 * `sources[i]`, and the inputs that record them. */
static void check_external_tools(const char *tree, const struct json *record,
                                 const char *const names[], const char *const sources[])
{
    const struct json *inputs =
        checks_of(record) ? json_member(checks_of(record)->item[0], "inputs") : NULL;
    size_t i, j;

    PF_CHECK(inputs != NULL, "external tools: the inputs");
    for (i = 0; names[i]; i++) {
        char relative[256], provenance[300], digest[65], proof[65], source[PF_PATH];
        const char *tool_digest = NULL, *proof_digest = NULL;
        struct json *document;
        fix_format(relative, sizeof(relative), "build/tests/host-suite/tools/%s", names[i]);
        fix_format(provenance, sizeof(provenance), "%s.provenance.json", relative);
        pf_sha_file(sources[i], digest);
        pf_sha_file(pf_at(tree, provenance), proof);
        PF_CHECK(pf_same_file(pf_at(tree, relative), sources[i]), "external tools: the copy");
        document = pf_jload(pf_at(tree, provenance));
        PF_CHECK(
            realpath(sources[i], source) != NULL && document && document->count == 2 &&
                json_text(document, "source") && !strcmp(json_text(document, "source"), source) &&
                json_text(document, "sha256") && !strcmp(json_text(document, "sha256"), digest),
            "external tools: the provenance");
        json_free(document);
        for (j = 0; inputs && j < inputs->count; j++) {
            const char *path = json_text(inputs->item[j], "path");
            if (path && !strcmp(path, relative))
                tool_digest = json_text(inputs->item[j], "sha256");
            if (path && !strcmp(path, provenance))
                proof_digest = json_text(inputs->item[j], "sha256");
        }
        PF_CHECK(tool_digest && !strcmp(tool_digest, digest), "external tools: the tool's input");
        PF_CHECK(proof_digest && !strcmp(proof_digest, proof),
                 "external tools: the provenance's input");
    }
}

/** Check that a declared external tool must be a prepared native program,
 * and that its copy and provenance are recorded. */
static void external_git_preparation(void)
{
    static const char *const names[] = {"git", "tar", NULL};
    char tree[PF_PATH];
    const char *unset[] = {"HOST_TEST_PREPARED_TOOLS", NULL};
    char prepared[PF_PATH], setting[PF_PATH + 32], found[2][PF_PATH];
    char copies[2][PF_PATH], suffixed[2][PF_PATH];
    const char *env[] = {setting, NULL};
    const char *plain[] = {copies[0], copies[1], NULL}, *exe[] = {suffixed[0], suffixed[1], NULL};
    struct json *record = NULL;
    size_t i;

    pf_scratch("native-git", tree);
    pf_native_tree(tree);
    pf_native_source(tree, "probe_hosttest.c", "host-tools: git tar\n", NULL);
    fix_format(prepared, sizeof(prepared), "%s/prepared", tree);
    fix_format(setting, sizeof(setting), "HOST_TEST_PREPARED_TOOLS=%s", prepared);
    PF_CHECK(!pf_mkdirs(prepared), "external tools: the directory");
    json_free(pf_native_prepare(tree, "tests", NULL, unset, 1, NULL));
    for (i = 0; names[i]; i++) {
        PF_CHECK(!pf_which(names[i], found[i]), "external tools: found on PATH");
        fix_format(copies[i], sizeof(copies[i]), "%s/%s", prepared, names[i]);
        fix_format(suffixed[i], sizeof(suffixed[i]), "%s/%s.exe", prepared, names[i]);
        PF_CHECK(!pf_copy(found[i], copies[i]), "external tools: copied");
    }
    record = pf_native_prepare(tree, "tests", NULL, env, 0, NULL);
    check_external_tools(tree, record, names, plain);
    json_free(record);
    for (i = 0; names[i]; i++)
        PF_CHECK(!rename(copies[i], suffixed[i]), "external tools: renamed");
    record = pf_native_prepare(tree, "tests", NULL, env, 0, NULL);
    check_external_tools(tree, record, names, exe);
    json_free(record);
    unlink(suffixed[0]);
    json_free(pf_native_prepare(tree, "tests", NULL, env, 1, NULL));
    PF_CHECK(!pf_write_text(suffixed[0], "not a native executable\n"),
             "external tools: not a program");
    json_free(pf_native_prepare(tree, "tests", NULL, env, 1, NULL));
    pf_native_source(tree, "probe_hosttest.c", "host-tools: mdir\n", NULL);
    json_free(pf_native_prepare(tree, "tests", NULL, unset, 1, NULL));
    pf_cleanup(tree);
}

/** Check that a GNU preparation reuses a current shared tool untouched, and
 * refuses a changed tool, an invalid record or a source that does not
 * compile without replacing the tool. */
static void shared_native_tool_reuse(void)
{
    const char *gnu[] = {"--gnu", NULL};
    char tree[PF_PATH], program[PF_PATH], marker[PF_PATH], hex[65], invalid[256];
    struct json *first = NULL, *second = NULL;
    struct jbuf out = {0};
    char *bytes, *record;
    size_t size;
    long long when;

    pf_scratch("native-tool", tree);
    pf_native_tree(tree);
    pf_native_source(tree, "probe_hosttest.c", "host-tools: mkfatfixture\n", NULL);
    PF_CHECK(
        !pf_write_text(pf_at(tree, "tests/host/mkfatfixture.c"), "int main(void) { return 0; }\n"),
        "tool reuse: source");
    first = pf_native_prepare(tree, "tests", gnu, NULL, 0, NULL);
    fix_format(program, sizeof(program), "%s/build/tests/host-suite/tools/mkfatfixture", tree);
    fix_format(marker, sizeof(marker), "%s.build.json", program);
    bytes = pf_read(program, &size);
    record = pf_read(marker, NULL);
    when = mtime_ns(program);
    second = pf_native_prepare(tree, "tests", gnu, NULL, 0, &out);
    PF_CHECK(first && second && json_equal(first, second), "tool reuse: the same inventory");
    PF_CHECK(pf_count(out.data, " -o ") == 0, "tool reuse: no compile");
    jbuf_free(&out);
    {
        size_t now_size;
        char *now = pf_read(program, &now_size), *now_record = pf_read(marker, NULL);
        PF_CHECK(now && bytes && now_size == size && !memcmp(now, bytes, size) && now_record &&
                     record && !strcmp(now_record, record) && mtime_ns(program) == when,
                 "tool reuse: the tool untouched");
        free(now);
        free(now_record);
    }
    pf_write(program, bytes, size);
    pf_append_text(program, "changed");
    json_free(pf_native_prepare(tree, "tests", gnu, NULL, 1, NULL));
    pf_write(program, bytes, size);
    pf_sha_bytes(bytes, size, hex);
    fix_format(invalid, sizeof(invalid), "{\"schema_version\":1,\"recipe\":{},\"sha256\":\"%s\"}",
               hex);
    pf_write_text(marker, "{}");
    json_free(pf_native_prepare(tree, "tests", gnu, NULL, 1, NULL));
    pf_write_text(marker, invalid);
    json_free(pf_native_prepare(tree, "tests", gnu, NULL, 1, NULL));
    pf_write_text(marker, record);
    pf_write_text(pf_at(tree, "tests/host/mkfatfixture.c"), "invalid C syntax\n");
    json_free(pf_native_prepare(tree, "tests", gnu, NULL, 1, NULL));
    {
        size_t now_size;
        char *now = pf_read(program, &now_size), *now_record = pf_read(marker, NULL);
        PF_CHECK(now && now_size == size && !memcmp(now, bytes, size) && now_record &&
                     !strcmp(now_record, record),
                 "tool reuse: a failed preparation keeps the tool");
        free(now);
        free(now_record);
    }
    free(bytes);
    free(record);
    json_free(first);
    json_free(second);
    pf_cleanup(tree);
}

/* One Windows preparation of `tree` that compiles `compiled` programs. */
static void windows_again(const char *tree, size_t compiled)
{
    const char *env[] = {"HOST_TEST_PLATFORM=windows", NULL};
    struct jbuf out = {0};

    json_free(pf_native_prepare(tree, "etude", NULL, env, 0, &out));
    PF_CHECK(pf_count(out.data, " -o ") == compiled, "companions: the programs compiled");
    jbuf_free(&out);
}

/** Check that the Windows companion programs are reused while their
 * sources, the tree's headers and the SDK's are unchanged, and rebuilt
 * alone when one changes or when a program or its record is changed,
 * damaged or missing; and that a source that does not compile fails the
 * preparation and keeps them. */
static void windows_companion_reuse(void)
{
    static const char *const names[] = {"wintarget.exe", "winstand.exe", "etude.exe",
                                        "etude-volume.exe", "image-command.exe"};
    enum { N = sizeof(names) / sizeof(*names) };
    const char *env[] = {"HOST_TEST_PLATFORM=windows", NULL};
    char work[PF_PATH], tree[PF_PATH], root[PF_PATH], programs[N][PF_PATH], marker[PF_PATH];
    char *bytes[N];
    size_t sizes[N], i;
    long long times[N];
    struct json *first = NULL, *second = NULL;
    struct jbuf out = {0};
    char *target, *record, *source, *edit;
    size_t target_size;

    pf_scratch("companion-cache", work);
    fix_format(tree, sizeof(tree), "%s/tree", work);
    fix_format(root, sizeof(root), "%s/build/tests/host-suite/etude/platform", tree);
    pf_native_tree(tree);
    pf_native_source(tree, "etude_platform_hosttest.c", "host-alone: native isolation\n", NULL);
    pf_windows_tree(tree);
    first = pf_native_prepare(tree, "etude", NULL, env, 0, NULL);
    for (i = 0; i < N; i++) {
        fix_format(programs[i], sizeof(programs[i]), "%s/%s", root, names[i]);
        bytes[i] = pf_read(programs[i], &sizes[i]);
        times[i] = mtime_ns(programs[i]);
    }
    second = pf_native_prepare(tree, "etude", NULL, env, 0, &out);
    PF_CHECK(first && second && json_equal(first, second), "companions: the same inventory");
    PF_CHECK(pf_count(out.data, " -o ") == 0, "companions: no compile");
    jbuf_free(&out);
    for (i = 0; i < N; i++) {
        size_t size;
        char *now = pf_read(programs[i], &size);
        PF_CHECK(now && bytes[i] && size == sizes[i] && !memcmp(now, bytes[i], size) &&
                     mtime_ns(programs[i]) == times[i],
                 "companions: each program untouched");
        free(now);
        free(bytes[i]);
    }
    pf_write_text(pf_at(tree, "tools/etude/win/value.h"), "#define VALUE 1\n");
    windows_again(tree, 1);
    PF_CHECK(pf_status(programs[0]) == 1, "companions: a changed header of the tree");
    pf_write_text(pf_at(tree, "tools/etude/win/value.h"), "#define VALUE 0\n");
    pf_write_text(pf_at(tree, "tools/etude/win/shadow.h"), "#define SHADOW 2\n");
    windows_again(tree, 1);
    PF_CHECK(pf_status(programs[0]) == 2, "companions: a new header that shadows another");
    pf_write_text(pf_at(work, "sdk/external.h"), "#define EXTERNAL 3\n");
    windows_again(tree, 1);
    PF_CHECK(pf_status(programs[0]) == 5, "companions: a changed header of the SDK");
    source = pf_read(pf_at(tree, "tools/etude/win/wintarget.c"), NULL);
    edit = source ? strstr(source, "EXTERNAL;") : NULL;
    PF_CHECK(edit != NULL, "companions: the fixture's source");
    if (edit) {
        *edit = '\0';
        pf_write_text(pf_at(tree, "tools/etude/win/wintarget.c"), source);
        pf_append_text(pf_at(tree, "tools/etude/win/wintarget.c"), "EXTERNAL + 1;");
        pf_append_text(pf_at(tree, "tools/etude/win/wintarget.c"), edit + strlen("EXTERNAL;"));
    }
    free(source);
    windows_again(tree, 1);
    PF_CHECK(pf_status(programs[0]) == 6, "companions: a changed source");
    fix_format(marker, sizeof(marker), "%s.build.json", programs[0]);
    for (i = 0; i < 3; i++) {
        char sha[65];
        struct json *built;

        if (i == 0)
            pf_append_text(programs[0], "changed");
        else if (i == 1)
            pf_write_text(marker, "{}");
        else
            unlink(marker);
        windows_again(tree, 1);
        built = pf_jload(marker);
        PF_CHECK(pf_status(programs[0]) == 6 && built && !pf_sha_file(programs[0], sha) &&
                     json_text(built, "sha256") && !strcmp(json_text(built, "sha256"), sha),
                 "companions: a changed program or a damaged or missing record is rebuilt");
        json_free(built);
    }
    target = pf_read(programs[0], &target_size);
    record = pf_read(marker, NULL);
    pf_write_text(pf_at(tree, "tools/etude/win/wintarget.c"), "invalid C syntax\n");
    json_free(pf_native_prepare(tree, "etude", NULL, env, 1, NULL));
    {
        size_t size;
        char *now = pf_read(programs[0], &size), *now_record = pf_read(marker, NULL);
        PF_CHECK(now && target && size == target_size && !memcmp(now, target, size) && now_record &&
                     record && !strcmp(now_record, record),
                 "companions: a failed preparation keeps the program");
        free(now);
        free(now_record);
    }
    free(target);
    free(record);
    json_free(first);
    json_free(second);
    pf_cleanup(work);
}

/** Run the host preparation rows. */
void pf_host_rows(void)
{
    partition_modes();
    script_partition_routes();
    gnu_inventory_predecessor();
    shared_tool_predecessor();
    legacy_tool_predecessor();
    native_exfat_filename();
    native_program_publication();
    external_git_preparation();
    shared_native_tool_reuse();
    windows_companion_reuse();
}
