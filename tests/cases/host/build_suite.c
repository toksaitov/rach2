/* Host-suite scheduling, sanitizer, reporting and cleanup regressions. */
#include "build_support.h"
#include "sha256.h"

struct suite_probe {
    const char *id, *mode;
    int alone;
};

static const struct suite_probe probes[] = {
    {"meet1", "meet", 0},          {"meet2", "meet", 0},   {"meet3", "meet", 0},
    {"meet4", "meet", 0},          {"meet5", "meet", 0},   {"meet6", "meet", 0},
    {"meet7", "meet", 0},          {"meet8", "meet", 0},   {"asan", "asan", 0},
    {"ubsan", "ubsan", 0},
#ifdef __linux__
    {"leak", "leak", 0},
#endif
    {"gone", "gone", 0},           {"keep", "keep", 0},    {"signals", "signals", 0},
    {"linger", "linger", 0},       {"alone1", "alone", 1}, {"alone2", "alone", 1},
    {"unlisted", "script_fail", 0}};

static const struct suite_probe checker_probes[] = {{"elfcheck", "--selftest", 0},
                                                    {"cfgcheck", "--selftest", 0},
                                                    {"setidcheck", "--selftest", 0},
                                                    {"prosecheck", "--selftest", 0},
                                                    {"pecheck", "--selftest", 0}};

static const struct suite_probe script_probes[] = {{"script_pass", "script_pass", 0},
                                                   {"script_fail", "script_fail", 0}};

static int file_hash(const char *path, char hex[65])
{
    struct sha256 h;
    unsigned char block[8192], digest[32];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    sha256_init(&h);
    while ((n = fread(block, 1, sizeof(block), f)))
        sha256_update(&h, block, n);
    int ok = !ferror(f);
    fclose(f);
    sha256_final(&h, digest);
    sha256_hex(digest, hex);
    return ok;
}
static int suite_manifest(const char *tree, const struct suite_probe *script, size_t nscript)
{
    const char *path = bp(tree, "tests/cases/host.host.json");
    fx_text(path, "");
    FILE *out = fopen(path, "w");
    if (!out)
        return 0;
    const struct suite_probe *groups[] = {probes, checker_probes, script};
    size_t counts[] = {sizeof(probes) / sizeof(*probes), 5, nscript};
    const char *names[] = {"tests", "checkers", "scripts"};
    fputs("{\"cases\":[", out);
    for (size_t g = 0; g < 3; g++) {
        fprintf(out,
                "%s{\"case_id\":\"host.host.%s\",\"assertions\":[{\"assertion_id\":\"listed\"}",
                g ? "," : "", names[g]);
        for (size_t i = 0; i < counts[g]; i++)
            if (strcmp(groups[g][i].id, "unlisted"))
                fprintf(out, ",{\"assertion_id\":\"%s\"}", groups[g][i].id);
        if (g == 0)
            fputs(",{\"assertion_id\":\"ghost\"}", out);
        fputs("]}", out);
    }
    fputs("]}\n", out);
    return fclose(out) == 0;
}
static int suite_inventory(const char *tree, const char *group, const struct suite_probe *items,
                           size_t count)
{
    char manifest[65], program[65], source[65];
    if (!file_hash(bp(tree, "tests/cases/host.host.json"), manifest) ||
        !file_hash(bp(tree, "build/tests/host-suite/probe"), program))
        return 0;
    char relative[256];
    snprintf(relative, sizeof(relative), "build/tests/host-suite/%s.inventory.json", group);
    const char *path = bp(tree, relative);
    fx_text(path, "");
    FILE *out = fopen(path, "w");
    if (!out)
        return 0;
    fprintf(out,
            "{\"schema_version\":1,\"group\":\"%s\",\"architecture\":\"%s\",\"manifest_sha256\":\"%"
            "s\",\"checks\":[",
            group, !strcmp(group, "tests") ? "i686" : "none", manifest);
    for (size_t i = 0; i < count; i++) {
        const struct suite_probe *p = &items[i];
        char input[256];
        snprintf(input, sizeof(input), "tests/host/%s_hosttest.c", p->id);
        if (!strcmp(group, "tests")) {
            if (!fx_text(bp(tree, input), "/* prepared probe source */\n") ||
                !file_hash(bp(tree, input), source)) {
                fclose(out);
                return 0;
            }
        }
        fprintf(out,
                "%s{\"assertion_id\":\"%s\",\"label\":\"%s\",\"mode\":\"%s\",\"program\":\"build/"
                "tests/host-suite/probe\",\"sha256\":\"%s\",\"arguments\":[",
                i ? "," : "", p->id, p->id, p->alone ? "alone" : "shared", program);
        if (!strcmp(group, "checkers"))
            fputs("\"--selftest\"", out);
        else
            fprintf(out, "\"--probe\",\"%s\"", p->mode);
        fputs("],\"inputs\":[", out);
        if (!strcmp(group, "tests"))
            fprintf(out, "{\"path\":\"%s\",\"sha256\":\"%s\"}",
                    !strcmp(p->id, "gone") ? "tests/host/gone.c" : input, source);
        fputs("]}", out);
    }
    fputs("]}\n", out);
    return fclose(out) == 0;
}
static int suite_run(struct fixture *f, const char *tree, const char *group, struct fx_child *child)
{
    char tree_env[8192], case_env[256], meet_env[8192];
    snprintf(tree_env, sizeof(tree_env), "ETUDE_TREE=%s", tree);
    snprintf(case_env, sizeof(case_env), "ETUDE_CASE=host.host.%s", group);
    snprintf(meet_env, sizeof(meet_env), "HOST_SUITE_MEET=%s", bp(f->work, "meet"));
    const char *env[] = {tree_env,
                         case_env,
                         meet_env,
                         "ARCH=i686",
                         "RACH2_WORKERS=16",
                         "ASAN_OPTIONS=exitcode=0:halt_on_error=0:abort_on_error=0:detect_leaks=0",
                         "UBSAN_OPTIONS=exitcode=0:halt_on_error=0:abort_on_error=0",
                         "LSAN_OPTIONS=exitcode=0",
                         NULL};
    const char *args[] = {product_path(f, "host"), group, NULL};
    return child ? fx_start(f, child, args, f->work, env, NULL)
                 : fx_run(f, args, f->work, env, NULL);
}
static size_t occurrences(const char *text, const char *word)
{
    size_t n = 0, z = strlen(word);
    for (const char *p = text; (p = strstr(p, word)); p += z)
        n++;
    return n;
}
static char *line_value(const char *text, const char *prefix)
{
    const char *at = strstr(text, prefix);
    if (!at)
        return NULL;
    at += strlen(prefix);
    size_t n = strcspn(at, "\n");
    char *out = malloc(n + 1);
    if (!out)
        abort();
    memcpy(out, at, n);
    out[n] = 0;
    return out;
}
static int reports(const char *text, size_t expected)
{
    char *copy = strdup(text), *save;
    size_t results = 0, progress = 0, asserts = 0, end = 0;
    int pending = 0, ok = 1;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "  PASS:", 7) || !strncmp(line, "  FAIL:", 7)) {
            ok &= !pending;
            pending = 1;
            results++;
        } else if (!strncmp(line, "@etude 1 progress ", 18)) {
            unsigned long count = 0;
            char rest;
            ok &= pending && sscanf(line, "@etude 1 progress checks %lu %c", &count, &rest) == 1 &&
                  count == ++progress;
            pending = 0;
        } else if (!strncmp(line, "@etude 1 assert ", 16))
            asserts++;
        else if (!strncmp(line, "@etude 1 end ", 13))
            end = strtoul(line + 13, NULL, 10);
    }
    free(copy);
    return ok && !pending && results == expected && progress == expected && end == asserts;
}
static int tests_judge(struct fixture *f, const char *text, int status)
{
    fx_check(f, status == 1, "tests case fails for its planted faults");
    for (size_t i = 0; i < 8; i++) {
        char marker[128];
        snprintf(marker, sizeof(marker), "@etude 1 assert meet%lu pass", (unsigned long)i + 1);
        fx_check(f, occurrences(text, marker) == 1, "eight probes run together in private scratch");
    }
    fx_check(f,
             strstr(text, "assert alone1 pass") && strstr(text, "assert alone2 pass") &&
                 strstr(text, "assert linger pass"),
             "alone checks wait for shared checks");
    fx_check(f,
             strstr(text, "assert asan fail the check exited with status") &&
                 strstr(text, "assert ubsan fail the check exited with status") &&
                 strstr(text, "AddressSanitizer"),
             "sanitizers override inherited success settings");
#ifdef __linux__
    fx_check(f, strstr(text, "assert leak fail the check exited with status") != NULL,
             "LeakSanitizer overrides inherited success settings");
#endif
    fx_check(f, strstr(text, "assert gone fail") && strstr(text, "tests/host/gone.c"),
             "missing source fails and is named");
    fx_check(f,
             strstr(text, "assert signals pass") &&
                 strstr(text, "  PASS: signals\n    | signals_hosttest: not run: nothing: this "
                              "line is repeated under the pass\n"),
             "signal defaults and repeated probe diagnostic");
    fx_check(f,
             !strstr(text, "FAIL: unlisted") && strstr(text, "assert listed fail") &&
                 strstr(text, "prepared check unlisted\n") &&
                 strstr(text, "assert ghost fail the prepared inventory has no check"),
             "unlisted check stays unexecuted and missing check fails");
    fx_check(f, reports(text, sizeof(probes) / sizeof(*probes)),
             "each named check gives one result, assertion and progress");
    char *kept = line_value(text, "    scratch kept: "), *logs = line_value(text, "host: logs: ");
    fx_check(f, kept && fx_exists(bp(kept, "evidence")),
             "failed check retains and names its evidence");
    int dirs = 0;
    DIR *d = logs ? opendir(logs) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (*e->d_name == '.')
            continue;
        struct stat st;
        if (!lstat(bp(logs, e->d_name), &st) && S_ISDIR(st.st_mode)) {
            dirs++;
            fx_check(f, !strcmp(e->d_name, "keep"), "only nonempty failed scratch remains");
        }
    }
    if (d)
        closedir(d);
    fx_check(f, dirs == 1, "one failed scratch directory remains");
    free(kept);
    free(logs);
    return !f->failed;
}
static int stop_judge(struct fixture *f, const char *tree)
{
    const struct suite_probe stuck = {"script_stuck", "stuck", 0};
    if (!suite_manifest(tree, &stuck, 1) ||
        !fx_text(bp(tree, "tests/cases/host/host.list"),
                 "script_stuck|an endless native probe\n") ||
        !suite_inventory(tree, "scripts", &stuck, 1))
        return 0;
    const char *mark = bp(f->work, "meet/stuck");
    fx_remove(mark);
    struct fx_child child;
    if (!suite_run(f, tree, "scripts", &child))
        return 0;
    if (!fx_until(&child, mark)) {
        fx_stop(&child, SIGTERM);
        fx_wait(f, &child);
        return fx_check(f, 0, "endless probe starts");
    }
    char *pidtext = fx_read(mark);
    long pid = pidtext ? strtol(pidtext, NULL, 10) : 0;
    free(pidtext);
    fx_stop(&child, SIGTERM);
    int status = fx_wait(f, &child);
    const char *text = f->out ? f->out : "";
    fx_check(f,
             status == 143 && strstr(text, "  STOPPED: script_stuck") &&
                 strstr(text, "stopped by signal 15: 0 of 1 checks finished"),
             "stop is reported with exit143");
    fx_check(f, !strstr(text, "@etude 1 progress "),
             "interrupted probe contributes no completed progress");
    fx_check(f, pid > 0 && kill((pid_t)pid, 0) < 0 && errno == ESRCH,
             "interrupted probe is reaped");
    char *logs = line_value(text, "host: logs: ");
    fx_check(f, logs && !fx_exists(logs), "stopped driver removes private workspace");
    free(logs);
    return !f->failed;
}
static int duplicate_judge(struct fixture *f, const char *tree)
{
    const struct suite_probe twice[] = {{"twice", "script_pass", 0}, {"twice", "script_pass", 0}};
    if (!suite_manifest(tree, twice, 1) ||
        !fx_text(bp(tree, "tests/cases/host/host.list"), "twice|duplicate probe\n") ||
        !suite_inventory(tree, "scripts", twice, 2))
        return 0;
    int status = suite_run(f, tree, "scripts", NULL);
    return fx_check(
        f, status == 2 && !output_has(f, "  PASS: twice") && !output_has(f, "  FAIL: twice"),
        "duplicate prepared selection is refused before execution");
}
int build_suite(struct fixture *f)
{
    char tree[4096];
    snprintf(tree, sizeof(tree), "%s", bp(f->work, "suite"));
    if (!fx_mkdir(bp(tree, "tests/host")) || !fx_mkdir(bp(f->work, "meet")) ||
        !fx_copy(product_path(f, "host-suite-probe"), bp(tree, "build/tests/host-suite/probe")) ||
        !suite_manifest(tree, script_probes, 2) ||
        !fx_text(bp(tree, "tests/cases/host/host.list"),
                 "script_pass|passing native probe\nscript_fail|failing native probe\n"))
        return 0;
    if (!suite_inventory(tree, "tests", probes, sizeof(probes) / sizeof(*probes)) ||
        !suite_inventory(tree, "checkers", checker_probes, 5) ||
        !suite_inventory(tree, "scripts", script_probes, 2))
        return 0;
    int status = suite_run(f, tree, "tests", NULL);
    char *tests = strdup(f->out ? f->out : "");
    tests_judge(f, tests, status);
    free(tests);
    status = suite_run(f, tree, "checkers", NULL);
    fx_check(f,
             status == 0 && reports(f->out ? f->out : "", 5) &&
                 occurrences(f->out ? f->out : "", " pass\n") == 6,
             "five selftests and their inventory pass");
    status = suite_run(f, tree, "scripts", NULL);
    fx_check(f,
             status == 1 && reports(f->out ? f->out : "", 2) &&
                 output_has(f, "assert script_pass pass") &&
                 output_has(f, "assert script_fail fail") && output_has(f, "status 4"),
             "native adapter pass and failure keep exact outcomes");
    stop_judge(f, tree);
    duplicate_judge(f, tree);
    return !f->failed;
}
