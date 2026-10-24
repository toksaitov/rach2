/* Exercise native collection selection through its public entry. */
#include "build_support.h"

static char *replaced(const char *text, const char *old, const char *value)
{
    size_t n = strlen(old), z = strlen(value), count = 0;
    const char *at = text, *p;
    while ((p = strstr(at, old))) {
        count++;
        at = p + n;
    }
    char *out = malloc(strlen(text) + count * z + 1), *to = out;
    if (!out)
        abort();
    at = text;
    while ((p = strstr(at, old))) {
        size_t k = (size_t)(p - at);
        memcpy(to, at, k);
        to += k;
        memcpy(to, value, z);
        to += z;
        at = p + n;
    }
    strcpy(to, at);
    return out;
}
static char *fixture_case(struct fixture *f)
{
    char *text = fx_read(bp(f->tree, "tools/etude/fixtures/verdict/cases/fixture.host.json"));
    char *at = text ? strstr(text, "\"cases\"") : NULL, *start = at ? strchr(at, '{') : NULL;
    if (!start) {
        free(text);
        return NULL;
    }
    int depth = 0, string = 0, escaped = 0;
    for (at = start; *at; at++) {
        if (string) {
            if (escaped)
                escaped = 0;
            else if (*at == '\\')
                escaped = 1;
            else if (*at == '"')
                string = 0;
            continue;
        }
        if (*at == '"')
            string = 1;
        else if (*at == '{')
            depth++;
        else if (*at == '}' && !--depth) {
            at[1] = 0;
            char *out = strdup(start);
            free(text);
            return out;
        }
    }
    free(text);
    return NULL;
}
static char *architectures(char *object, const char *value)
{
    char *field = strstr(object, "\"architectures\""), *at = field ? strchr(field, ':') : NULL;
    if (!at)
        return NULL;
    at++;
    while (*at == ' ' || *at == '\n' || *at == '\t')
        at++;
    if (*at != '"')
        return NULL;
    char *end = strchr(at + 1, '"');
    if (!end)
        return NULL;
    size_t before = (size_t)(at - object), n = strlen(value), after = strlen(end + 1);
    char *out = malloc(before + n + after + 1);
    if (!out)
        abort();
    memcpy(out, object, before);
    memcpy(out + before, value, n);
    memcpy(out + before + n, end + 1, after + 1);
    return out;
}
static int manifest(struct fixture *f, const char *tree, const char *family, const char *names,
                    const char *const arches[])
{
    char file[256];
    snprintf(file, sizeof(file), "tests/cases/%s.json", family);
    const char *path = bp(tree, file);
    char *base = fixture_case(f);
    if (!base)
        return 0;
    fx_text(path, "");
    FILE *out = fopen(path, "w");
    if (!out) {
        free(base);
        return 0;
    }
    fprintf(out,
            "{\"schema_version\":1,\"family_id\":\"%s\",\"summary\":\"Native case "
            "selection\",\"cases\":[",
            family);
    for (size_t i = 0; names[i]; i++) {
        char identity[256];
        snprintf(identity, sizeof(identity), "%s.%c", family, names[i]);
        char *object = replaced(base, "fixture.host.vectors", identity),
             *changed = architectures(object, arches[i]);
        free(object);
        if (!changed) {
            free(base);
            fclose(out);
            return 0;
        }
        fprintf(out, "%s%s", i ? "," : "", changed);
        free(changed);
    }
    fputs("]}\n", out);
    free(base);
    return fclose(out) == 0;
}
static int selection_tree(struct fixture *f, const char *tree)
{
    const char *both = "[\"i686\",\"x86_64\"]", *single = "[\"i686\"]";
    const char *alpha[] = {both, single, "\"none\""}, *beta[] = {both},
               *agents[] = {both, both, both};
    return fx_mkdir(bp(tree, "tests/cases")) &&
           fx_text(bp(tree, "tests/registry"), "alpha|-|full\nbeta|-|full\nagents|-|full\n") &&
           fx_tree(bp(f->tree, "tools/etude/schemas"), bp(tree, "tools/etude/schemas")) &&
           manifest(f, tree, "host.alpha", "abc", alpha) &&
           manifest(f, tree, "host.beta", "d", beta) &&
           manifest(f, tree, "machine.agents", "xyz", agents);
}
struct select_row {
    const char *name, *arch, *cases, *rows, *suites, *want;
    int status;
};

static const struct select_row selections[] = {
    {"manifest-order", "i686", "host.alpha.c host.alpha.a", NULL, "alpha",
     "alpha i686 host.alpha.a\nalpha none host.alpha.c\n", 0},
    {"rows-order", "i686", NULL, "z x", "agents",
     "agents i686 machine.agents.z\nagents i686 machine.agents.x\n", 0},
    {"both", "i686", "host.alpha.a", "y", "alpha agents",
     "alpha i686 host.alpha.a\nagents i686 machine.agents.y\n", 0},
    {"rows-elsewhere", "i686", NULL, "x", "alpha", NULL, 0},
    {"unknown", "i686", "host.alpha.nope", NULL, "alpha", NULL, 2},
    {"repeated", "i686", "host.alpha.a host.alpha.a", NULL, "alpha", NULL, 2},
    {"empty-suite", "i686", "host.alpha.a", NULL, "alpha beta", NULL, 2},
    {"other-suite", "i686", "host.alpha.a machine.agents.x", NULL, "alpha", NULL, 2},
    {"architecture", "x86_64", "host.alpha.b", NULL, "alpha", NULL, 2},
    {"twice", "i686", "machine.agents.x", "y", "agents", NULL, 2},
    {"rows-empty", "i686", NULL, " ", "agents", NULL, 2},
    {"rows-repeated", "i686", NULL, "x x", "agents", NULL, 2},
    {"rows-unknown", "i686", NULL, "x nope", "agents", NULL, 2}};

static int selection_row(struct fixture *f, const char *tree, const struct select_row *r,
                         int prepare)
{
    char cases[4096], rows[4096];
    snprintf(cases, sizeof(cases), "RACH2_CASES%s%s", r->cases ? "=" : "",
             r->cases ? r->cases : "");
    snprintf(rows, sizeof(rows), "RACH2_AGENTS_ROWS%s%s", r->rows ? "=" : "",
             r->rows ? r->rows : "");
    const char *env[] = {cases,
                         rows,
                         "RACH2_ACCEPT_AGENTS",
                         "ETUDE_SITE",
                         "ETUDE_ADMITTED",
                         "RACH2_TREE_HELD",
                         "RACH2_PORT_OFFSET",
                         "ARCH=i686",
                         NULL};
    const char *args[32];
    size_t n = 0;
    args[n++] = product_path(f, "etude");
    args[n++] = prepare ? "prepare" : "run";
    if (prepare)
        args[n++] = "collection";
    args[n++] = "-t";
    args[n++] = tree;
    args[n++] = "-a";
    args[n++] = r->arch;
    if (!prepare)
        args[n++] = "-N";
    char *text = strdup(r->suites), *save;
    for (char *suite = strtok_r(text, " ", &save); suite; suite = strtok_r(NULL, " ", &save))
        args[n++] = suite;
    args[n] = NULL;
    int status = fx_run(f, args, f->work, env, NULL);
    free(text);
    int ok = status == r->status;
    if (r->status == 2)
        ok &= f->out && !*f->out && !fx_exists(bp(tree, "build")) && output_has(f, "refused:");
    else if (r->want)
        ok &= f->out && !strcmp(f->out, r->want);
    else
        ok &= f->out && strstr(f->out, "host.alpha.a") && strstr(f->out, "host.alpha.b") &&
              strstr(f->out, "host.alpha.c");
    return fx_check(f, ok, r->name);
}
static int select_run_rows(struct fixture *f, const char *tree)
{
    const struct select_row refused[] = {
        {"run refuses unknown", "i686", "host.alpha.a host.beta.nope", NULL, "alpha beta", NULL, 2},
        {"run refuses repeated", "i686", "host.alpha.a host.alpha.a host.beta.d", NULL,
         "alpha beta", NULL, 2},
        {"run refuses empty-suite", "i686", "host.alpha.a", NULL, "alpha beta", NULL, 2}};
    for (size_t i = 0; i < 3; i++)
        selection_row(f, tree, &refused[i], 1);
    /* Dry selection is the public preparation boundary of the native runner.
     * It validates cases without invoking make or a fixture compiler. */
    const char *env[] = {"RACH2_CASES=host.beta.d host.alpha.c host.alpha.a",
                         "RACH2_AGENTS_ROWS",
                         "RACH2_ACCEPT_AGENTS",
                         "ETUDE_SITE",
                         "ETUDE_ADMITTED",
                         "RACH2_TREE_HELD",
                         NULL};
    const char *args[] = {
        product_path(f, "etude"), "run", "-N", "-t", tree, "-A", "tcg", "alpha", "beta", NULL};
    int status = fx_run(f, args, f->work, env, NULL);
    fx_check(
        f,
        status == 0 && f->out &&
            !strcmp(f->out,
                    "alpha i686 host.alpha.a\nalpha none host.alpha.c\nbeta i686 host.beta.d\n") &&
            !fx_exists(bp(tree, "build")),
        "accepted explicit selection preserves manifest order without build activity");
    const char *every[] = {"RACH2_CASES", "RACH2_AGENTS_ROWS", "RACH2_ACCEPT_AGENTS", NULL};
    status = fx_run(f, args, f->work, every, NULL);
    fx_check(f,
             status == 0 && f->out && strstr(f->out, "host.alpha.b") &&
                 strstr(f->out, "host.beta.d") && !fx_exists(bp(tree, "build")),
             "unselected run retains every case without build activity");

    return !f->failed;
}
int build_runner(struct fixture *f, const char *id)
{
    if (strcmp(id, "case_select"))
        return 0;
    char tree[4096];
    snprintf(tree, sizeof(tree), "%s", bp(f->work, "selection"));
    if (!selection_tree(f, tree))
        return 0;
    for (size_t i = 0; i < sizeof(selections) / sizeof(*selections); i++)
        selection_row(f, tree, &selections[i], 0);
    return select_run_rows(f, tree);
}
