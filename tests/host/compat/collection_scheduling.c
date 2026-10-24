/* Public collection scheduling over compiled, prepared fixture cases. */
#include "../../../tools/etude/json.h"

static unsigned scheduling_initial, scheduling_ceiling;
static int scheduling_both;

/* Replace only an existing string in the fixture's parsed case. */
static void scheduling_string(struct json *object, const char *key, const char *value)
{
    struct json *field = (struct json *)json_member(object, key);
    CHECK(field && field->type == JSON_STRING, "scheduling fixture string exists");
    if (field && field->type == JSON_STRING) {
        free(field->string);
        field->string = strdup(value);
        CHECK(field->string != NULL, "scheduling fixture string allocates");
    }
}

static void scheduling_architectures(struct json *one)
{
    if (!scheduling_both)
        return;
    struct json *value = NULL;
    char error[256];
    const char text[] = "[\"i686\",\"x86_64\"]";
    CHECK(!json_parse(text, strlen(text), 1, &value, error, sizeof error),
          "both scheduling architectures parse");
    for (size_t i = 0; value && i < one->count; i++)
        if (!strcmp(one->item[i]->name, "architectures")) {
            value->name = strdup("architectures");
            json_free(one->item[i]);
            one->item[i] = value;
            return;
        }
    json_free(value);
    CHECK(0, "scheduling architectures exist");
}

/* Reuse the validated case contract, changing its family and identities only. */
static void scheduling_manifest(const char *family, const char *const *ids)
{
    char text[16384], error[256], path[128];
    FILE *f = fopen("tools/etude/fixtures/verdict/cases/fixture.host.json", "rb");
    struct json *doc = NULL;
    size_t n = f ? fread(text, 1, sizeof(text), f) : 0;
    if (f)
        fclose(f);
    CHECK(n && !json_parse(text, n, 1, &doc, error, sizeof(error)),
          "scheduling case template parses");
    if (!doc)
        return;
    const struct json *cases = json_member(doc, "cases");
    struct json *one = cases && cases->count == 1 ? cases->item[0] : NULL;
    CHECK(one != NULL, "scheduling template has one case");
    struct jbuf output = {0};
    jbuf_puts(&output, "{\"schema_version\":1,\"summary\":\"Prepared scheduling fixtures\","
                       "\"family_id\":\"");
    jbuf_puts(&output, family);
    jbuf_puts(&output, "\",\"cases\":[");
    for (size_t i = 0; one && ids[i]; i++) {
        if (i)
            jbuf_puts(&output, ",");
        scheduling_string(one, "case_id", ids[i]);
        scheduling_architectures(one);
        if (scheduling_ceiling) {
            struct json *r = (struct json *)json_member(one, "resources");
            ((struct json *)json_member(r, "cpu_millis"))->integer = scheduling_initial;
            ((struct json *)json_member(r, "cpu_ceiling_millis"))->integer = scheduling_ceiling;
        }
        json_canonical(one, &output);
    }
    jbuf_puts(&output, "]}\n");
    snprintf(path, sizeof(path), "tests/cases/%s.json", family);
    put(path, output.data);
    jbuf_free(&output);
    json_free(doc);
}

/* Inspect the real private ledger while this compiled case owns its claim. */
static int scheduling_claim(const char *root)
{
    const char *expected = getenv("ETUDE_COLLECTION_CPU");
    if (!expected)
        return 1;
    char path[PATH_MAX], line[512], wanted[80];
    snprintf(path, sizeof(path), "%s/build/tests/results/ledger/claims", root);
    snprintf(wanted, sizeof(wanted), "resource cpu_millis %s\n", expected);
    FILE *f = fopen(path, "rb");
    int found = 0;
    while (f && fgets(line, sizeof(line), f))
        found |= !strcmp(line, wanted);
    if (f)
        fclose(f);
    return found;
}

/* A FIFO rendezvous has no timing threshold: both prepared cases must start
 * before either can finish. Their ordinary case watchdog handles a deadlock. */
static int scheduling_role(const char *id)
{
    const char *root = getenv("ETUDE_COLLECTION_MEETING");
    int meeting = root != NULL;
    if (!root)
        root = getenv("ETUDE_COLLECTION_COST_ROOT");
    if (!root || (strncmp(id, "host.host.", 10) && strcmp(id, "fixture.style.ready")))
        return -1;
    printf("@etude 1 begin %s\n", id);
    fflush(stdout);
    int ok = scheduling_claim(root);
    if (meeting && (!strcmp(id, "host.host.tests") || !strcmp(id, "host.host.checkers"))) {
        int writer = !strcmp(id, "host.host.tests");
        char path[PATH_MAX], byte = 'x';
        for (int phase = 0; phase < 2; phase++) {
            snprintf(path, sizeof(path), "%s/meeting-%d", root, phase);
            int write_end = phase ? !writer : writer;
            int fd = open(path, write_end ? O_WRONLY : O_RDONLY);
            ok &= fd >= 0;
            if (fd >= 0) {
                ok &= (write_end ? write(fd, &byte, 1) : read(fd, &byte, 1)) == 1;
                close(fd);
            }
        }
    }
    printf("@etude 1 assert empty %s\n@etude 1 assert million pass\n@etude 1 end 2\n",
           ok ? "pass" : "fail");
    return !ok;
}

static void scheduling_seed(char *out, size_t room)
{
    char *allocate[] = {"-N",   "-j", "1", "-S", "42", "-J", (char *)fix_path("quiet-seed.json"),
                        "host", NULL};
    char *execute[] = {"-A", "tcg", "-j", "1", "-S", "42", "host", NULL};
    CHECK(!run(allocate, "host.host.etude host.host.boots", NULL, NULL, out, room),
          "quiet seeded allocator exports");
    char text[32768], error[256];
    FILE *f = fopen(fix_path("quiet-seed.json"), "rb");
    size_t n = f ? fread(text, 1, sizeof(text), f) : 0;
    if (f)
        fclose(f);
    struct json *doc = NULL;
    CHECK(n && !json_parse(text, n, 1, &doc, error, sizeof(error)),
          "quiet seeded selection parses");
    /* No meeting cases are selected here: the allocated width is deliberately one. */
    CHECK(!run(execute, "host.host.etude host.host.boots", NULL, NULL, out, room),
          "quiet seeded cases pass");
    const struct json *items = json_member(doc, "items");
    const char *cursor = out;
    CHECK(items && items->count == 2, "seeded quiet selection has both identities");
    for (size_t i = 0; items && i < items->count; i++) {
        char start[256];
        const char *id = json_text(items->item[i], "case_id");
        snprintf(start, sizeof(start), "%s: starting", id ? id : "absent");
        const char *next = cursor ? strstr(cursor, start) : NULL;
        CHECK(next != NULL, "quiet cases retain their exported seeded execution order");
        cursor = next ? next + strlen(start) : NULL;
    }
    json_free(doc);
}

/* The public selection must describe shared host/style members and retain
 * style's stop-on-failure policy without marking either suite serial. */
static void scheduling_policy(char *out, size_t room)
{
    char *options[] = {"-N",   "-J",    (char *)fix_path("shared-selection.json"),
                       "host", "style", NULL};
    CHECK(!run(options, NULL, NULL, NULL, out, room), "shared selection exports");
    char text[32768], error[256];
    FILE *f = fopen(fix_path("shared-selection.json"), "rb");
    size_t n = f ? fread(text, 1, sizeof(text), f) : 0;
    if (f)
        fclose(f);
    struct json *doc = NULL;
    CHECK(n && !json_parse(text, n, 1, &doc, error, sizeof(error)), "shared selection parses");
    const char *selected = json_text(doc, "selection_bytes");
    CHECK(selected && !strstr(selected, "\"serial\":true") && strstr(selected, "\"stops\":true"),
          "host and style share capacity while style retains failure stopping");
    json_free(doc);
}

static void scheduling_capacity(char *out, size_t room)
{
    static const char *const ids[] = {"host.host.etude", NULL};
    scheduling_initial = 1000;
    scheduling_ceiling = 16000;
    scheduling_manifest("host.host", ids);
    fixture_policy(8000);
    setenv("ETUDE_COLLECTION_CPU", "1000", 1);
    char *fit[] = {"-A", "tcg", "-j", "4", "-C", "2", "host", NULL};
    CHECK(!run(fit, NULL, NULL, NULL, out, room),
          "the declared 1000 initial claim is used below a 16000 ceiling");
    char *load[] = {"-A", "tcg", "-j", "4", "-C", "2", "-S", "42", "-L", "cpu:1", "host", NULL};
    setenv("ETUDE_COLLECTION_CPU", "2000", 1);
    CHECK(!run(load, NULL, NULL, NULL, out, room),
          "a fitting CPU load is added to the declared initial reservation");
    char *over[] = {"-A", "tcg", "-j", "4", "-C", "2", "-S", "42", "-L", "cpu:8", "host", NULL};
    CHECK(run(over, NULL, NULL, NULL, out, room) == 2 && !strstr(out, "host.host.etude: starting"),
          "a CPU load beyond the actual site capacity refuses before execution");
    scheduling_initial = 2000;
    scheduling_manifest("host.host", ids);
    fixture_policy(1000);
    char *small[] = {"-A", "tcg", "-j", "1", "-C", "1", "host", NULL};
    CHECK(run(small, NULL, NULL, NULL, out, room) == 2 && !strstr(out, "host.host.etude: starting"),
          "capacity below the host declared initial claim refuses before execution");
    unsetenv("ETUDE_COLLECTION_CPU");
    scheduling_initial = scheduling_ceiling = 0;
    fixture_policy(4000);
}

static void scheduling_history(char *out, size_t room);

static void scheduling(char *out, size_t room)
{
    static const char *const ids[] = {"host.host.tests", "host.host.checkers", "host.host.etude",
                                      "host.host.boots", NULL};
    static const char *const style[] = {"fixture.style.ready", NULL};
    CHECK(unlink(fix_path("tests/cases/fixture.host.json")) == 0, "old host fixture is replaced");
    scheduling_initial = 1000;
    scheduling_ceiling = 16000;
    scheduling_manifest("host.host", ids);
    scheduling_initial = scheduling_ceiling = 0;
    scheduling_manifest("fixture.style", style);
    fixture_policy(2000);
    put("tests/registry", "host|-|core\nstyle|-|core\n");
    for (int i = 0; i < 2; i++) {
        char path[32];
        snprintf(path, sizeof(path), "meeting-%d", i);
        CHECK(!mkfifo(fix_path(path), 0600), "meeting FIFO is made");
    }
    scheduling_policy(out, room);
    setenv("ETUDE_COLLECTION_MEETING", tree, 1);
    char *options[] = {"-A", "tcg", "-j", "3", "host", "style", NULL};
    CHECK(!run(options, NULL, NULL, NULL, out, room),
          "prepared host cases meet while style shares collection capacity");
    const char *tests = strstr(out, "host.host.tests: starting");
    const char *etude_start = strstr(out, "host.host.etude: starting");
    const char *boots = strstr(out, "host.host.boots: starting");
    CHECK(tests && etude_start && boots && etude_start < tests && boots < tests,
          "quiet host groups precede shared prepared groups");
    scheduling_seed(out, room);
    scheduling_capacity(out, room);
    unsetenv("ETUDE_COLLECTION_MEETING");
    scheduling_history(out, room);
}
