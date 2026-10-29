/* flow_ci.c: C fixtures and assertions for the report of the release tests. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *env(const char *key)
{
    const char *s = getenv(key);
    return s ? s : "";
}

static const char *path(const char *name)
{
    static char paths[32][8192];
    static unsigned next;
    char *s = paths[next++ % 32];
    snprintf(s, 8192, "%s/%s", env("CI_TEST_WORK"), name);
    return s;
}

static int append(const char *file, const char *text)
{
    FILE *f = fopen(file, "a");
    int ok;
    if (!f)
        return 0;
    ok = fputs(text, f) >= 0 && fputc('\n', f) != EOF;
    return fclose(f) == 0 && ok;
}

static char *join(int argc, char **argv, int first)
{
    int i;
    size_t n = 1;
    char *s;
    for (i = first; i < argc; i++)
        n += strlen(argv[i]) + 1;
    s = calloc(1, n);
    if (s)
        for (i = first; i < argc; i++) {
            if (i > first)
                strcat(s, " ");
            strcat(s, argv[i]);
        }
    return s;
}

/* Stands for GitHub, which the report must not call. */
static int mock_gh(int argc, char **argv)
{
    char *all = join(argc, argv, 1);
    if (all)
        append(path("gh.log"), all);
    free(all);
    return 1;
}

static int mock_native_reader(int argc, char **argv)
{
    char *all = join(argc, argv, 3);
    int ok = all && strstr(all, "--project-architecture") && strstr(all, "--require-budget") &&
             append(path("reader.log"), all);
    free(all);
    if (!ok)
        return 2;
    int failed = !strcmp(env("CI_TEST_NATIVE_GUARD"), "budget");
    puts(failed ? "etude check: result: failed: runner" : "etude check: result: passed");
    return failed;
}

static int mock_reader(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "prepare") && !strcmp(argv[2], "reader")) {
        char *args[] = {(char *)path("etude"), "prepare", "reader", argv[3], argv[4], NULL};
        execv(args[0], args);
        return 127;
    }
    if (argc > 2 && !strcmp(argv[1], "check") && !strcmp(argv[2], "record"))
        return 0;
    if (argc > 2 && !strcmp(argv[1], "check") && !strcmp(argv[2], "verdict"))
        return mock_native_reader(argc, argv);
    return 2;
}

int flow_ci_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "gh"))
        return mock_gh(argc, argv);
    if (!strcmp(name, "etude-reader"))
        return mock_reader(argc, argv);
    return 2;
}

static int runsh(struct fixture *f, const char *results)
{
    char text[256];
    snprintf(text, sizeof(text), "#!/bin/sh\nresults=%s\n", results);
    return flow_put(f, "tree/tools/ci/run.sh", text);
}

/** Run report.sh with WORDS in the tree; return its status. */
static int report(struct fixture *f, const char *const words[])
{
    const char *argv[8] = {"/bin/sh", flow_path(f, "report.sh")};
    unsigned i;
    for (i = 0; words[i] && i < 5; i++)
        argv[i + 2] = words[i];
    argv[i + 2] = NULL;
    fx_run(f, argv, flow_path(f, "tree"), NULL, NULL);
    return f->status;
}

static int full(struct fixture *f)
{
    const char *words[] = {"bad", path("results"), path("failures"), NULL};
    return report(f, words);
}

/**
 * Give the architecture ARCH a result: `pass`, `fail` with FAILED as its
 * `failed`, `missing` for no directory, `unfinished` for one without `done`,
 * `unlisted` for `done` without `failed`, or `skipped`.
 */
static int result(struct fixture *f, const char *arch, const char *state, const char *failed)
{
    char dir[160], file[200];
    snprintf(dir, sizeof(dir), "results/full-%s", arch);
    if (!fx_remove(path(dir)))
        return 0;
    if (!strcmp(state, "missing"))
        return 1;
    if (!fx_mkdir(path(dir)))
        return 0;
    snprintf(file, sizeof(file), "%s/done", dir);
    if (strcmp(state, "unfinished") && !flow_put(f, file, ""))
        return 0;
    snprintf(file, sizeof(file), "%s/failed", dir);
    if (strcmp(state, "unlisted") &&
        !flow_put(f, file, !strcmp(state, "fail") || !strcmp(state, "unfinished") ? failed : ""))
        return 0;
    snprintf(file, sizeof(file), "%s/%s", dir, state);
    return strcmp(state, "skipped") || flow_put(f, file, "");
}

static int start(struct fixture *f)
{
    fx_env("SUITES", NULL);
    return fx_remove(path("results")) && fx_mkdir(path("results")) && flow_put(f, "gh.log", "") &&
           flow_put(f, "reader.log", "") && flow_put(f, "summary", "") && runsh(f, "lines");
}

static int pass_row(struct fixture *f)
{
    int ok = start(f) && result(f, "i686", "pass", "") && result(f, "x86_64", "pass", "") &&
             full(f) == 0 && fx_equal(path("failures"), "") &&
             strstr(f->out, "ci: report: result: passed on i686 and x86_64") &&
             fx_has(path("summary"), "## Release tests of bad\n") &&
             fx_has(path("summary"), "i686 passed.") && fx_has(path("summary"), "x86_64 passed.") &&
             fx_equal(path("gh.log"), "");
    return fx_check(f, ok, "both architectures pass");
}

static int failed_row(struct fixture *f)
{
    int ok =
        start(f) && result(f, "i686", "fail", "fpu\ndefault\nfpu\n") &&
        result(f, "x86_64", "fail", "build\n") &&
        flow_put(f, "results/full-i686/err",
                 "run: i686:default: FAIL in 3s\nrun: starting: fpu\nrun: result: failed: "
                 "i686:default i686:fpu\n") &&
        full(f) == 1 && fx_equal(path("failures"), "i686:default\ni686:fpu\nx86_64:build\n") &&
        strstr(f->out, "ci: report: result: failed: i686:default i686:fpu x86_64:build") &&
        fx_has(path("summary"), "i686 failed: default fpu.") &&
        fx_has(path("summary"), "```\nrun: i686:default: FAIL in 3s\nrun: result: failed: "
                                "i686:default i686:fpu\n```") &&
        !fx_has(path("summary"), "run: starting: fpu") &&
        fx_has(path("summary"), "Logs: `gh run download 1 --repo test/rach2 --name full-i686`") &&
        fx_equal(path("gh.log"), "");
    return fx_check(f, ok, "failures are written as ARCH:ITEM and summarized");
}

static int unfinished_row(struct fixture *f, const char *state)
{
    char label[160];
    int ok = start(f) && result(f, "i686", state, "default\n") && result(f, "x86_64", "pass", "") &&
             full(f) == 1 && fx_equal(path("failures"), "i686:unfinished\n");
    snprintf(label, sizeof(label), "a %s result is unfinished", state);
    return fx_check(f, ok, label);
}

static int skipped_row(struct fixture *f)
{
    int ok = start(f) && result(f, "i686", "skipped", "") && result(f, "x86_64", "pass", "") &&
             full(f) == 0 && fx_equal(path("failures"), "") &&
             fx_has(path("summary"), "i686 had no suite to test.");
    if (ok) {
        fx_env("SUITES", "x86_64:fpu");
        ok = full(f) == 0 && fx_has(path("summary"), "only these suites: x86_64:fpu.");
    }
    return fx_check(f, ok, "a skipped architecture passes");
}

static int usage_row(struct fixture *f)
{
    const char *words[] = {"bad", path("results"), NULL};
    int ok = start(f) && report(f, words) == 2 && strstr(f->err, "usage: tools/ci/report.sh");
    return fx_check(f, ok, "report refuses missing arguments");
}

static int native_row(struct fixture *f, const char *mode)
{
    unsigned i;
    int ok, fail = strcmp(mode, "pass") != 0;
    if (!start(f) || !runsh(f, "native"))
        return 0;
    for (i = 0; i < 2; i++) {
        const char *arch = i ? "x86_64" : "i686";
        char file[160];
        if (!result(f, arch, "pass", ""))
            return 0;
        snprintf(file, sizeof(file), "results/full-%s/native", arch);
        if (!flow_put(f, file, "etude-native-1\n"))
            return 0;
        snprintf(file, sizeof(file), "results/full-%s/suites", arch);
        if (!flow_put(f, file, "default\n"))
            return 0;
    }
    if (strcmp(mode, "missing"))
        if (!flow_put(f, "results/history/run.jsonl", "fixture\n") ||
            !fx_mkdir(path("results/store")))
            return 0;
    if (!flow_ci_reader_record(f, path("bin/etude-reader")))
        return 0;
    fx_env("CI_TEST_NATIVE_GUARD", mode);
    ok = full(f) == (fail ? 1 : 0) &&
         fx_equal(path("failures"), fail ? "i686:runner\nx86_64:runner\n" : "") &&
         fx_has(path("reader.log"), !strcmp(mode, "missing") ? "" : " -c bad i686 full default ");
    fx_env("CI_TEST_NATIVE_GUARD", NULL);
    fx_env("ETUDE_REPORT_READER_RECORD", NULL);
    return fx_check(f, ok,
                    !strcmp(mode, "pass")     ? "native verdict passes"
                    : !strcmp(mode, "budget") ? "native budget failure is the runner's"
                                              : "missing native history is the runner's");
}

static int prepare(struct fixture *f)
{
    const char *root = getenv("HOST_PRODUCTS");
    char etude[4096];
    if (!root || !*root)
        root = "/host-products";
    snprintf(etude, sizeof(etude), "%s/etude", root);
    fx_env("FLOW_CI", "1");
    fx_env("CI_TEST_WORK", f->work);
    fx_env("GITHUB_REPOSITORY", "test/rach2");
    fx_env("GITHUB_RUN_ID", "1");
    fx_env("GITHUB_STEP_SUMMARY", path("summary"));
    fx_env("ETUDE", path("bin/etude-reader"));
    fx_env("ETUDE_REPORT_READER", NULL);
    fx_env("ETUDE_REPORT_READER_RECORD", NULL);
    fx_env("ETUDE_SITE", NULL);
    return flow_copy(f, "tools/ci/report.sh", "report.sh") && fx_copy(etude, path("etude")) &&
           fx_mock(f, "gh") && fx_mock(f, "etude-reader") && fx_mkdir(path("tree/tools/ci"));
}

int flow_ci(struct fixture *f)
{
    int ok = 1;
    if (!prepare(f))
        return 0;
    ok = pass_row(f) && ok;
    ok = failed_row(f) && ok;
    ok = unfinished_row(f, "missing") && ok;
    ok = unfinished_row(f, "unfinished") && ok;
    ok = unfinished_row(f, "unlisted") && ok;
    ok = skipped_row(f) && ok;
    ok = usage_row(f) && ok;
    ok = native_row(f, "pass") && ok;
    ok = native_row(f, "budget") && ok;
    ok = native_row(f, "missing") && ok;
    ok = flow_ci_preparation(f) && ok;
    return ok;
}
