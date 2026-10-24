/* Synthetic eligible histories exercise public ordering and actual claims.
 * Their counters and cache/load declarations are fixtures, not measurements. */
#include "load_history_host.h"

struct cost_sample {
    const char *name;
    unsigned seconds, excluded;
};

/* Replace one existing member with parsed fixture bytes. */
static void cost_field(struct json *object, const char *name, const char *bytes)
{
    struct json *value = NULL;
    char error[256];
    int parsed = !json_parse(bytes, strlen(bytes), 1, &value, error, sizeof error);
    CHECK(parsed, "cost fixture replacement parses");
    if (!parsed)
        return;
    for (size_t i = 0; object && i < object->count; i++)
        if (object->item[i]->name && !strcmp(object->item[i]->name, name)) {
            value->name = strdup(name);
            json_free(object->item[i]);
            object->item[i] = value;
            return;
        }
    CHECK(0, "cost fixture replacement exists");
    json_free(value);
}

/* Change stream/item/attempt identities without changing absolute blob paths. */
static void cost_identity(struct json *doc, const char *old, const char *id)
{
    if (doc->type == JSON_STRING && !strncmp(doc->string, old, strlen(old))) {
        size_t n = strlen(id) + strlen(doc->string + strlen(old)) + 1;
        char *text = malloc(n);
        CHECK(text != NULL, "cost fixture identity allocates");
        if (text) {
            snprintf(text, n, "%s%s", id, doc->string + strlen(old));
            free(doc->string);
            doc->string = text;
        }
    }
    for (size_t i = 0; i < doc->count; i++)
        cost_identity(doc->item[i], old, id);
}

/* Use exact synthetic intervals. The trace below covers the complete run. */
static void cost_interval(char *out, size_t size, unsigned seconds)
{
    snprintf(out, size,
             "{\"start_utc\":\"2026-10-01T00:00:00.000000Z\",\"end_utc\":"
             "\"2026-10-01T00:%02u:%02u.000000Z\",\"clock\":{\"machine_id\":\"fixture\","
             "\"boot_id\":\"fixture-boot\",\"clock_id\":\"monotonic\",\"resolution_us\":1},"
             "\"start_us\":0,\"end_us\":%u,\"elapsed_us\":{\"value\":%u,"
             "\"method\":\"monotonic-v1\",\"quality\":\"exact\",\"interval_us\":null,"
             "\"error_bound\":1}}",
             seconds / 60, seconds % 60, seconds * 1000000, seconds * 1000000);
}

/* Mutate only the values under test, retaining the real fixture plan's key. */
static void cost_record(struct json *doc, const struct cost_sample *sample)
{
    const char *kind = json_text(doc, "kind");
    struct json *data = (struct json *)json_member(doc, "data");
    char id[100], old[256], interval[1200], value[256];
    snprintf(id, sizeof id, "cost-%s", sample->name);
    snprintf(old, sizeof old, "%s", json_text(doc, "run_id"));
    cost_identity(doc, old, id);
    if (!strcmp(kind, "plan")) {
        if (sample->excluded == 3)
            scheduling_string((struct json *)json_member(data, "environment"), "accelerator",
                              "whpx");
        return;
    }
    if (strcmp(kind, "attempt") && strcmp(kind, "run"))
        return;
    unsigned seconds = sample->seconds;
    if (!strcmp(kind, "attempt") && strstr(json_text(data, "item_id"), "checkers"))
        seconds = seconds > 6 ? seconds - 6 : 1;
    cost_interval(interval, sizeof interval, seconds);
    cost_field(data, "time", interval);
    struct json *result = (struct json *)json_member(data, "result");
    if (sample->excluded == 1) {
        scheduling_string(result, "outcome", "failure");
        cost_field(result, "reason", "\"synthetic failed sample\"");
    }
    if (sample->excluded == 2)
        cost_field(result, "disruptions", "[\"synthetic disruption\"]");
    if (!strcmp(kind, "run")) {
        char load[1800];
        CHECK(!load_fixture(load, sizeof load, fix_dir(), id, interval),
              "synthetic complete observation writes");
        cost_field(data, "load", load);
    } else {
        struct json *resources = (struct json *)json_member(data, "resources");
        snprintf(value, sizeof value,
                 "{\"value\":%u,\"method\":\"rusage-v1\",\"quality\":\"lower_bound\","
                 "\"interval_us\":null,\"error_bound\":null}",
                 seconds * 500000);
        cost_field(resources, "tree_cpu_us", value);
    }
}

/* Produce independent schema-checked histories from one real fixture stream.
 * The production reader validates every rewritten record before learning. */
static void cost_stream(const char *source, const char *history, const struct cost_sample *sample)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s/%s.jsonl", tree, history, sample->name);
    FILE *in = fopen(source, "rb"), *out = fopen(path, "wb");
    CHECK(in && out, "cost history files open");
    char *line = NULL, error[256];
    size_t cap = 0;
    ssize_t n;
    while (in && out && (n = getline(&line, &cap, in)) > 0) {
        struct json *doc = NULL;
        if (json_parse(line, (size_t)n, 1, &doc, error, sizeof error)) {
            CHECK(0, "source cost history parses");
            break;
        }
        cost_record(doc, sample);
        struct jbuf bytes = {0};
        json_canonical(doc, &bytes);
        CHECK(bytes.data && fputs(bytes.data, out) >= 0 && fputc('\n', out) != EOF,
              "cost history record writes");
        jbuf_free(&bytes);
        json_free(doc);
    }
    free(line);
    if (in)
        fclose(in);
    if (out)
        CHECK(!fclose(out), "cost history closes");
}

static void cost_histories(const char *template, const char *history)
{
    static const struct cost_sample samples[] = {{"first", 8, 0},       {"middle", 10, 0},
                                                 {"last", 12, 0},       {"failed", 100, 1},
                                                 {"disrupted", 100, 2}, {"foreign", 100, 3}};
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s/history", tree, template);
    DIR *dir = opendir(path);
    struct dirent *entry;
    char source[PATH_MAX] = "";
    while (dir && (entry = readdir(dir)))
        if (strstr(entry->d_name, ".jsonl")) {
            int n = snprintf(source, sizeof source, "%s/%s", path, entry->d_name);
            CHECK(n > 0 && n < (int)sizeof source, "cost source path fits");
            break;
        }
    if (dir)
        closedir(dir);
    CHECK(source[0], "the template collection wrote a history");
    CHECK(!mkdir(fix_path(history), 0700), "cost history directory is made");
    for (size_t i = 0; source[0] && i < sizeof samples / sizeof samples[0]; i++)
        cost_stream(source, history, &samples[i]);
}

static void cost_capacity(char *out, size_t room)
{
    char *learned[] = {"-A",   "tcg", "-j", "1", "-H", (char *)fix_path("cost-history"),
                       "host", NULL};
    fixture_policy(800);
    setenv("ETUDE_COLLECTION_CPU", "625", 1);
    CHECK(!run(learned, NULL, NULL, NULL, out, room),
          "eligible625 fits site800 despite initial1000");
    CHECK(!mkdir(fix_path("cost-empty"), 0700), "the empty history directory is made");
    char *initial[] = {"-A", "tcg", "-j", "1", "-H", (char *)fix_path("cost-empty"), "host", NULL};
    CHECK(run(initial, NULL, NULL, NULL, out, room) == 2 && !strstr(out, ": starting"),
          "unlearned initial1000 cannot fit site800");
    fixture_policy(2000);
    unsetenv("ETUDE_COLLECTION_CPU");
}

/* Match the actual seeded load identity before exercising its reservation. */
static void cost_loaded(char *out, size_t room)
{
    char *template[] = {"-A",   "tcg", "-j",    "1",  "-S",
                        "42",   "-L",  "cpu:1", "-o", (char *)fix_path("cost-loaded-template"),
                        "host", NULL};
    CHECK(!run(template, NULL, NULL, NULL, out, room), "the loaded identity template passes");
    cost_histories("cost-loaded-template", "cost-loaded-history");
    char *loaded[] = {"-A",   "tcg", "-j",    "1",  "-S",
                      "42",   "-L",  "cpu:1", "-H", (char *)fix_path("cost-loaded-history"),
                      "host", NULL};
    setenv("ETUDE_COLLECTION_CPU", "1625", 1);
    fixture_policy(1800);
    CHECK(!run(loaded, NULL, NULL, NULL, out, room),
          "learned625 plus declared load1000 fits site1800");
    fixture_policy(1500);
    CHECK(run(loaded, NULL, NULL, NULL, out, room) == 2 && !strstr(out, ": starting") &&
              strstr(out, "CPU 625 (3 samples)"),
          "learned CPU plus load is refused instead of clamped to site1500");
    unsetenv("ETUDE_COLLECTION_CPU");
    fixture_policy(2000);
}

/* A cost history must not change the seed allocator's exact order. */
static void cost_seed(char *out, size_t room)
{
    char *allocate[] = {"-N", "-S", "42", "-J", (char *)fix_path("cost-seed.json"), "host", NULL};
    CHECK(!run(allocate, NULL, NULL, NULL, out, room), "the measured seed order exports");
    char text[32768], error[256];
    FILE *f = fopen(fix_path("cost-seed.json"), "rb");
    size_t n = f ? fread(text, 1, sizeof text, f) : 0;
    if (f)
        fclose(f);
    struct json *doc = NULL;
    CHECK(n && !json_parse(text, n, 1, &doc, error, sizeof error), "the seed export parses");
    char *execute[] = {"-A",   "tcg", "-j", "1", "-S", "42", "-H", (char *)fix_path("cost-history"),
                       "host", NULL};
    setenv("ETUDE_COLLECTION_CPU", "1000", 1);
    CHECK(!run(execute, NULL, NULL, NULL, out, room),
          "the distinct seeded profile keeps its initial CPU claim");
    const struct json *items = json_member(doc, "items");
    const char *cursor = out;
    CHECK(items && items->count == 2, "both seeded cost cases are exported");
    for (size_t i = 0; items && i < items->count; i++) {
        char start[256];
        snprintf(start, sizeof start, "%s: starting", json_text(items->item[i], "case_id"));
        const char *next = cursor ? strstr(cursor, start) : NULL;
        CHECK(next != NULL, "compatible history never reorders a seeded selection");
        cursor = next ? next + strlen(start) : NULL;
    }
    json_free(doc);
}

static void scheduling_history(char *out, size_t room)
{
    static const char *const ids[] = {"host.host.checkers", "host.host.tests", NULL};
    scheduling_initial = 1000;
    scheduling_ceiling = 16000;
    scheduling_both = 1;
    scheduling_manifest("host.host", ids);
    scheduling_both = 0;
    scheduling_initial = scheduling_ceiling = 0;
    setenv("ETUDE_COLLECTION_COST_ROOT", tree, 1);
    fixture_policy(2000);
    char *template[] = {"-A",   "tcg",  "-j", "1",
                        "-a",   "both", "-o", (char *)fix_path("cost-template"),
                        "host", NULL};
    CHECK(!run(template, NULL, NULL, NULL, out, room), "the cost identity template passes");
    cost_histories("cost-template", "cost-history");
    char *measured[] = {"-A",   "tcg", "-j", "1", "-H", (char *)fix_path("cost-history"),
                        "host", NULL};
    setenv("ETUDE_COLLECTION_CPU", "625", 1);
    CHECK(!run(measured, NULL, NULL, NULL, out, room), "history teaches actual CPU claims");
    const char *tests = strstr(out, "host.host.tests: starting");
    const char *checkers = strstr(out, "host.host.checkers: starting");
    CHECK(tests && checkers && tests < checkers,
          "one architecture starts the costlier compatible median first");
    CHECK(strstr(out, "host.host.tests i686: 10000000 us (3 samples), CPU 625 (3 samples)") &&
              strstr(out, "host.host.checkers i686: 4000000 us (3 samples), CPU 625 (3 samples)"),
          "failed, disrupted and incompatible samples do not teach costs");
    char *both[] = {"-A",   "tcg", "-j", "1", "-a", "both", "-H", (char *)fix_path("cost-history"),
                    "host", NULL};
    CHECK(!run(both, NULL, NULL, NULL, out, room), "both architectures use learned claims");
    const char *cursor = out;
    const char *order[] = {
        "host.host.tests on i686: starting", "host.host.tests on x86_64: starting",
        "host.host.checkers on i686: starting", "host.host.checkers on x86_64: starting"};
    for (size_t i = 0; i < 4; i++) {
        const char *next = cursor ? strstr(cursor, order[i]) : NULL;
        CHECK(next != NULL, "both architectures share measured order with stable ties");
        cursor = next ? next + strlen(order[i]) : NULL;
    }
    cost_capacity(out, room);
    cost_seed(out, room);
    unsetenv("ETUDE_COLLECTION_CPU");
    cost_loaded(out, room);
    unsetenv("ETUDE_COLLECTION_CPU");
    unsetenv("ETUDE_COLLECTION_COST_ROOT");
    fixture_policy(4000);
}
