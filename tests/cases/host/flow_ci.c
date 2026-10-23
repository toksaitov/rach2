/* flow_ci.c: C fixtures and assertions for CI reporting and bisection. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <dirent.h>
#include <errno.h>
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

static char *read_trim(const char *file)
{
    char *s = fx_read(file);
    size_t n;
    if (!s)
        return strdup("");
    n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
    return s;
}

static int event(const char *text)
{
    return append(path("events"), text);
}

static void admitted(const char *kind)
{
    char line[8192], *claims = fx_read(path("ledger/claims")), memory[80] = "";
    const char *p = claims;
    if (p && (p = strstr(p, "resource memory_bytes ")))
        sscanf(p, "resource memory_bytes %79s", memory);
    snprintf(line, sizeof(line), "%s offset=%s workers=%s jobs=%s makeflags=%s memory=%s claim=%s",
             kind, env("RACH2_PORT_OFFSET"), env("RACH2_WORKERS"), env("JOBS"), env("MAKEFLAGS"),
             env("RACH2_MEMORY_MIB"), memory);
    append(path("admitted"), line);
    free(claims);
}

static int mock_git(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (!strcmp(argv[1], "rev-parse")) {
        puts(argc > 2 && !strcmp(argv[2], "HEAD") ? "0123456789abcdef0123456789abcdef01234567"
             : argc > 3                           ? argv[3]
                                                  : "");
        return 0;
    }
    if (!strcmp(argv[1], "rev-list") && argc > 2) {
        if (!strcmp(argv[2], "--first-parent"))
            puts("good");
        else if (!strcmp(argv[2], "--reverse"))
            puts("bad");
        else if (!strcmp(argv[2], "--count"))
            puts("1");
        else
            return 2;
        return 0;
    }
    if (!strcmp(argv[1], "checkout") && argc > 4) {
        char value[256];
        snprintf(value, sizeof(value), "%s\n", argv[4]);
        return !fx_text(path("current"), value);
    }
    if (!strcmp(argv[1], "log")) {
        puts("bad test commit");
        return 0;
    }
    return 2;
}

static const char *argument(int argc, char **argv, const char *prefix)
{
    int i;
    size_t n = strlen(prefix);
    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], prefix, n))
            return argv[i] + n;
    return "";
}

static void status_errors(void)
{
    char *s = fx_read(path("statuses")), *p = s, *next;
    char contexts[32][256], states[32][40];
    unsigned n = 0, i;
    while (p && *p) {
        char context[256], state[40];
        next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        if (sscanf(p, "%255s %39s", context, state) == 2) {
            for (i = 0; i < n; i++)
                if (!strcmp(context, contexts[i]))
                    break;
            if (i < 32) {
                if (i == n)
                    n++;
                strcpy(contexts[i], context);
                strcpy(states[i], state);
            }
        }
        p = next;
    }
    for (i = 0; i < n; i++)
        if (!strcmp(states[i], "error"))
            printf("%s ", contexts[i]);
    putchar('\n');
    free(s);
}

static int gh_api(int argc, char **argv, const char *sub)
{
    int result = 0;
    if (!strcmp(sub, "repos/test/rach2/commits/good/status"))
        puts(!strcmp(env("CI_TEST_BASE"), "night")
                 ? "ci/night/i686\nci/night/x86_64"
                 : "ci/core\nci/core/i686\nci/core/x86_64\nci/full/i686\nci/full/x86_64");
    else if (!strcmp(sub, "repos/test/rach2/actions/runs/1/attempts/1/jobs"))
        puts(env("CI_TEST_UNRUN"));
    else if (!strcmp(sub, "repos/test/rach2/statuses/bad")) {
        char line[512];
        snprintf(line, sizeof(line), "%s %s", argument(argc, argv, "context="),
                 argument(argc, argv, "state="));
        result = !append(path("statuses"), line);
    } else if (!strcmp(sub, "repos/test/rach2/commits/bad/status"))
        status_errors();
    return result;
}

static int gh_list(const char *all)
{
    if (strstr(all, "--label hold ") && strstr(all, "--json number --jq ")) {
        char *s = strdup(env("CI_TEST_HOLD")), *p = strtok(s ? s : "", " ");
        while (p) {
            puts(p);
            p = strtok(NULL, " ");
        }
        free(s);
    } else if (strstr(all, "--label bug ") && strstr(all, "--json number,title,labels ") &&
               *env("CI_TEST_OPEN_TITLE") && strstr(all, env("CI_TEST_OPEN_TITLE")))
        puts(env("CI_TEST_OPEN"));
    else if (strstr(all, " --json number --jq ")) {
        const char *p = strstr(all, "--label ");
        char label[80], number[80], labels[1024];
        if (p && sscanf(p, "--label %79s", label) == 1 &&
            sscanf(env("CI_TEST_OPEN"), "%79s %1023[^\n]", number, labels) == 2 &&
            strstr(labels, label))
            puts(number);
    }
    return 0;
}

static int gh_view(const char *all)
{
    int result = 0;
    if (strstr(all, " --json labels "))
        puts(env("CI_TEST_LABELS"));
    else if (strstr(all, " --json state "))
        puts(*env("CI_TEST_STATE") ? env("CI_TEST_STATE") : "OPEN");
    else if (strstr(all, " --json body,comments ")) {
        const char *runs = env("CI_TEST_RUNS");
        char *s, *p;
        if (fx_has(path("gh.log"), "--add-label intermittent") && *env("CI_TEST_RUNS_LABELLED"))
            runs = env("CI_TEST_RUNS_LABELLED");
        s = strdup(runs);
        p = strtok(s ? s : "", " ");
        while (p) {
            printf("A failure.\n\nRun: "
                   "https://example.invalid/test/rach2/actions/runs/%s\nAttempt: 1\n",
                   p);
            p = strtok(NULL, " ");
        }
        free(s);
    } else
        result = 2;
    return result;
}

static int mock_gh(int argc, char **argv)
{
    char *all = join(argc, argv, 1);
    const char *command = argc > 1 ? argv[1] : "", *sub = argc > 2 ? argv[2] : "";
    int result = 0;
    if (!all || !append(path("gh.log"), all)) {
        free(all);
        return 2;
    }
    if (!strcmp(command, "api"))
        result = gh_api(argc, argv, sub);
    else if (!strcmp(command, "issue") && !strcmp(sub, "list"))
        result = gh_list(all);
    else if (!strcmp(command, "issue") && !strcmp(sub, "view"))
        result = gh_view(all);
    else if (!strcmp(command, "issue") && !strcmp(sub, "create"))
        puts("https://example.invalid/issues/7");
    else if (!strcmp(command, "issue") && !strcmp(sub, "comment"))
        result = !strcmp(env("CI_TEST_GH_FAIL"), "comment");
    else if (!strcmp(command, "issue") && !strcmp(sub, "edit"))
        result = !strcmp(env("CI_TEST_GH_FAIL"), "edit");
    else if (!strcmp(command, "issue") && !strcmp(sub, "close")) {
    } else if (!strcmp(command, "workflow") && !strcmp(sub, "run")) {
    } else
        result = 2;
    free(all);
    return result;
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
    char *s;
    int i;
    if (argc > 2 && !strcmp(argv[1], "check") && !strcmp(argv[2], "verdict"))
        return mock_native_reader(argc, argv);
    if (argc < 3 || strcmp(argv[1], "show") || strcmp(argv[2], "failures"))
        return 2;
    s = join(argc, argv, 3);
    if (!s || !append(path("reader.log"), s)) {
        free(s);
        return 2;
    }
    free(s);
    for (i = 3; i < argc; i++)
        if (strcmp(argv[i], "-b") && !fx_exists(argv[i])) {
            fprintf(stderr, "etude show: %s: cannot be opened\n", argv[i]);
            return 2;
        }
    if (*env("CI_TEST_HISTORY_OUT"))
        puts(env("CI_TEST_HISTORY_OUT"));
    return atoi(env("CI_TEST_HISTORY_STATUS"));
}

static int copy_streams(const char *destination, const char *streams)
{
    char *s = strdup(streams), *p = strtok(s ? s : "", " ");
    int ok = s != NULL;
    while (p && ok) {
        char from[8192], to[8192];
        const char *name = strrchr(p, '/');
        snprintf(from, sizeof(from), "%s/%s", env("CI_TEST_FIXTURES"), p);
        snprintf(to, sizeof(to), "%s/history/%s", destination, name ? name + 1 : p);
        ok = fx_copy(from, to);
        p = strtok(NULL, " ");
    }
    free(s);
    if (ok) {
        char from[8192], to[8192];
        snprintf(from, sizeof(from), "%s/store", env("CI_TEST_FIXTURES"));
        snprintf(to, sizeof(to), "%s/store", destination);
        ok = fx_tree(from, to);
    }
    return ok;
}

static int lock_tests(int argc, char **argv)
{
    char **args = calloc((size_t)argc + 9, sizeof(*args));
    int i;
    if (!args)
        return 2;
    fx_env("RACH2_HOST_LOCK", argc > 2 && !strcmp(argv[2], "bigmem") ? "alone" : "shared");
    args[0] = (char *)path("etude");
    args[1] = "lock";
    args[2] = "ports";
    args[3] = (char *)(*env("RACH2_PORT_DIR") ? env("RACH2_PORT_DIR") : "/tmp");
    args[4] = "10";
    args[5] = "sh";
    for (i = 1; i < argc; i++)
        args[i + 5] = argv[i];
    args[argc + 5] = NULL;
    execv(args[0], args);
    free(args);
    return 127;
}

static int mock_test_result(int argc, char **argv, const char *mode, int bad)
{
    int status;
    if (bad && (!strcmp(mode, "fail") || !strcmp(mode, "refuse") || !strcmp(mode, "silent") ||
                !strcmp(mode, "lost") || !strncmp(mode, "late-", 5))) {
        if (!strcmp(mode, "fail"))
            printf("run: %s: FAIL in 1s\nrun: result: failed\n", argc > 2 ? argv[2] : "");
        else if (!strcmp(mode, "refuse"))
            fprintf(stderr, "run: result: refused: unknown suite '%s'\n", argc > 2 ? argv[2] : "");
        else if (!strcmp(mode, "silent"))
            puts("run: result: all 1 suites passed");
        else if (!strcmp(mode, "lost"))
            printf("run: %s: FAIL in 1s\nrun: runner: the run record is missing\nrun: runner: FAIL "
                   "in 9s\nrun: result: failed: %s runner\nrun: result: failed\n",
                   argv[2], argv[2]);
        else {
            int incomplete = !strcmp(mode, "late-incomplete");
            printf("run: result: %s\n", incomplete ? "incomplete" : "failed");
            fflush(stdout);
            fprintf(stderr, "run: %s: %s in %s\nrun: result: failed: %s\n", argv[2],
                    incomplete ? "INCOMPLETE" : "FAIL", incomplete ? "?s" : "1s", argv[2]);
        }
        status = !strcmp(mode, "refuse") ? 2 : !strcmp(mode, "silent") ? 0 : 1;
        return status;
    }
    return -1;
}

static int mock_tests(int argc, char **argv)
{
    char *current = read_trim(path("current")), *all = join(argc, argv, 2), line[8192];
    const char *mode = env("CI_TEST_RUN");
    int bad = current && !strcmp(current, "bad"), status = 0, i;
    if (!*env("RACH2_PORT_OFFSET")) {
        free(current);
        free(all);
        return lock_tests(argc, argv);
    }
    snprintf(line, sizeof(line), "tests %s %s", all ? all : "", current ? current : "");
    event(line);
    admitted("tests");
    free(all);
    status = mock_test_result(argc, argv, mode, bad);
    if (status >= 0) {
        free(current);
        return status;
    }
    status = 0;
    if (*env("RACH2_RESULTS")) {
        if (!copy_streams(env("RACH2_RESULTS"),
                          bad ? env("CI_TEST_STREAMS") : env("CI_TEST_PASS"))) {
            free(current);
            return 2;
        }
        if (bad)
            status = atoi(env("CI_TEST_STATUS"));
    }
    for (i = 2; i < argc; i++) {
        const char *names = !strcmp(argv[i], "core") ? "default style host" : argv[i];
        char *s = strdup(names), *p = strtok(s ? s : "", " ");
        while (p) {
            if (!(bad && !strcmp(mode, "partial") && argc > 2 && !strcmp(argv[2], "core") &&
                  !strcmp(p, "default")))
                printf("run: %s: PASS in 1s\n", p);
            p = strtok(NULL, " ");
        }
        free(s);
    }
    puts("run: result: all suites passed");
    free(current);
    return status;
}

static int mock_native_run(int argc, char **argv)
{
    char *current = read_trim(path("current")), file[16384];
    int bad = current && !strcmp(current, "bad");
    const char *failed = bad ? env("CI_TEST_FAILED") : "";
    const char *kind = argc > 2 ? argv[2] : "";
    const char *out = argc > 3 ? argv[3] : "";
    int ok = *kind && *out;
    snprintf(file, sizeof(file), "%s/%s-%s/native", out, kind, env("ARCH"));
    ok = ok && fx_text(file, "etude-native-1\n");
    snprintf(file, sizeof(file), "%s/%s-%s/failed", out, kind, env("ARCH"));
    char text[256];
    snprintf(text, sizeof(text), "%s%s", failed, *failed ? "\n" : "");
    ok = ok && fx_text(file, text);
    snprintf(file, sizeof(file), "%s/%s-%s/done", out, kind, env("ARCH"));
    ok = ok && fx_text(file, "");
    event(bad ? "native bad" : "native good");
    free(current);
    return !ok ? 2 : *failed ? 1 : 0;
}

static int mock_run(int argc, char **argv)
{
    if (fx_has("tools/ci/run.sh", "results=native\n"))
        return mock_native_run(argc, argv);
    char *current = read_trim(path("current")), *clock = read_trim(path("clock")), line[8192];
    const char *kind = argc > 2 ? argv[2] : "";
    int result = 0;
    if (strcmp(kind, "core") && strcmp(kind, env("CI_TEST_FULL_KIND"))) {
        free(current);
        free(clock);
        return 2;
    }
    snprintf(line, sizeof(line), "run %s %s", kind, current ? current : "");
    event(line);
    admitted("run");
    snprintf(line, sizeof(line), "%ld\n",
             strtol(clock ? clock : "0", NULL, 10) + strtol(env("CI_TEST_STEP"), NULL, 10));
    fx_text(path("clock"), line);
    if (strcmp(env("CI_TEST_FAIL"), "0") && current && !strcmp(current, "bad")) {
        char failed[8192];
        snprintf(failed, sizeof(failed), "%s/failed", argc > 3 ? argv[3] : "");
        snprintf(line, sizeof(line), "%s\n", env("CI_TEST_FAILED"));
        result = !fx_text(failed, line) ? 2 : 1;
    }
    free(current);
    free(clock);
    return result;
}

static int mock_casecheck(int argc, char **argv)
{
    struct fixture f;
    const char *args[32];
    int i;
    char *p;
    memset(&f, 0, sizeof(f));
    snprintf(f.work, sizeof(f.work), "%s", env("CI_TEST_WORK"));
    f.command = (unsigned)getpid() * 10;
    args[0] = "/host-products/etude";
    args[1] = "check";
    for (i = 1; i < argc && i < 29; i++)
        args[i + 1] = argv[i];
    args[i + 1] = NULL;
    fx_run(&f, args, NULL, NULL, NULL);
    p = f.out;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        if (!strncmp(p, "etude check: ", 13))
            printf("casecheck: %.*s", end ? (int)(end - p - 13) : (int)strlen(p + 13), p + 13);
        else
            printf("%.*s", end ? (int)(end - p) : (int)strlen(p), p);
        if (!end)
            break;
        putchar('\n');
        p = (char *)end + 1;
    }
    i = f.status;
    free(f.out);
    free(f.err);
    return i;
}

int flow_ci_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "git"))
        return mock_git(argc, argv);
    if (!strcmp(name, "gh"))
        return mock_gh(argc, argv);
    if (!strcmp(name, "etude-reader"))
        return mock_reader(argc, argv);
    if (!strcmp(name, "make")) {
        event("make");
        admitted("make");
        return *env("CI_TEST_MAKE_FAIL") ? 1 : 0;
    }
    if (!strcmp(name, "date")) {
        char *s;
        if (argc != 2 || strcmp(argv[1], "+%s"))
            return 2;
        s = fx_read(path("clock"));
        if (s)
            fputs(s, stdout);
        free(s);
        return 0;
    }
    if (!strcmp(name, "timeout")) {
        if (argc < 5)
            return 2;
        append(path("limits"), argv[3]);
        execvp(argv[4], argv + 4);
        return 127;
    }
    if (!strcmp(name, "casecheck"))
        return mock_casecheck(argc, argv);
    if (!strcmp(name, "etude") && !strcmp(env("CI_TEST_JUDGE"), "unjudged")) {
        puts("item item-d0: fixture.default.errno on i686: failure, in default");
        return 2;
    }
    if (!strcmp(name, "sh") && argc > 1) {
        const char *p = argv[1], *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        if (!strcmp(base, "run") && strstr(p, "tests/"))
            return mock_tests(argc, argv);
        if (!strcmp(base, "run.sh") && strstr(p, "tools/ci/"))
            return mock_run(argc, argv);
        execv("/bin/sh", argv);
        return 127;
    }
    return 2;
}

static const char pass_streams[] =
    "pass/plan.jsonl pass/item-d0.jsonl pass/item-d1.jsonl pass/item-h.jsonl pass/run.jsonl";

static const char failure_streams[] =
    "pass/plan.jsonl item-d0-failure.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-failure.jsonl";

static const char unfinished_streams[] =
    "pass/plan.jsonl item-d0-unfinished.jsonl pass/item-d1.jsonl pass/item-h.jsonl "
    "run-unfinished.jsonl";

static const char cut_streams[] =
    "pass/plan.jsonl item-d0-failure.jsonl item-d1-lost.jsonl pass/item-h.jsonl run-failure.jsonl";

static int log_has(const char *name, const char *text)
{
    return fx_has(path(name), text);
}

static int log_regex(const char *name, const char *pattern)
{
    char *s = fx_read(path(name));
    int ok = s && fx_regex(s, pattern);
    free(s);
    return ok;
}

static int log_equal(const char *name, const char *text)
{
    return fx_equal(path(name), text);
}

static int clear(void)
{
    const char *names[] = {"BASE", "OPEN",          "OPEN_TITLE", "LABELS",
                           "RUNS", "RUNS_LABELLED", "STATE",      NULL};
    unsigned i;
    char key[100];
    unlink(path("statuses"));
    for (i = 0; names[i]; i++) {
        snprintf(key, sizeof(key), "CI_TEST_%s", names[i]);
        fx_env(key, "");
    }
    return 1;
}

static int start(struct fixture *f)
{
    clear();
    fx_env("RUNNER_TEMP", path("logs"));
    return fx_remove(path("logs")) && flow_put(f, "events", "") && flow_put(f, "gh.log", "") &&
           flow_put(f, "limits", "") && flow_put(f, "clock", "0\n");
}

static int runsh(struct fixture *f, const char *kind, const char *results)
{
    char text[256];
    fx_env("CI_TEST_FULL_KIND", kind);
    snprintf(text, sizeof(text),
             "#!/bin/sh\ncase $1 in\n    core|%s) ;;\n    *) exit 2 ;;\nesac\nresults=%s\n", kind,
             results);
    return flow_put(f, "tree/tools/ci/run.sh", text);
}

static int ci_command(struct fixture *f, const char *product, const char *const words[],
                      const char *log)
{
    const char *argv[24] = {"/bin/sh", flow_path(f, product)};
    unsigned i;
    char *combined;
    size_t a, b;
    int status;
    for (i = 0; words[i] && i < 21; i++)
        argv[i + 2] = words[i];
    argv[i + 2] = NULL;
    fx_run(f, argv, flow_path(f, "tree"), NULL, NULL);
    status = f->status;
    a = f->out ? strlen(f->out) : 0;
    b = f->err ? strlen(f->err) : 0;
    combined = malloc(a + b + 1);
    if (!combined)
        return -1;
    if (a)
        memcpy(combined, f->out, a);
    if (b)
        memcpy(combined + a, f->err, b);
    combined[a + b] = 0;
    if (!flow_put(f, log, combined))
        status = -1;
    free(combined);
    return status;
}

static int bisect(struct fixture *f, const char *const words[])
{
    return ci_command(f, "bisect.sh", words, "bisect.log");
}

static int report(struct fixture *f, const char *kind)
{
    const char *argv[] = {kind, "bad", path("results"), NULL};
    return ci_command(f, "report.sh", argv, "report.log");
}

static int results(struct fixture *f, const char *kind, const char *state)
{
    char base[128], other[128];
    const char *verdict = "";
    fx_env("CI_TEST_GH_FAIL", "");
    if (!fx_remove(path("results")))
        return 0;
    snprintf(base, sizeof(base), "results/%s-i686", kind);
    snprintf(other, sizeof(other), "results/%s-x86_64", kind);
    if (!fx_mkdir(path(base)) || !fx_mkdir(path(other)))
        return 0;
    {
        char p[160];
        snprintf(p, sizeof(p), "%s/done", other);
        if (!flow_put(f, p, ""))
            return 0;
        snprintf(p, sizeof(p), "%s/failed", other);
        if (!flow_put(f, p, ""))
            return 0;
    }
    if (!strcmp(state, "missing") || !strcmp(state, "lost") || !strcmp(state, "lost-runner")) {
        if (!fx_remove(path(base)))
            return 0;
        if (!strcmp(state, "lost"))
            verdict = "done ";
        if (!strcmp(state, "lost-runner"))
            verdict = "done runner default";
    } else if (!strcmp(state, "unfinished")) {
        char p[160];
        snprintf(p, sizeof(p), "%s/failed", base);
        if (!flow_put(f, p, "default\n"))
            return 0;
    } else if (!strcmp(state, "missing-failed")) {
        char p[160];
        snprintf(p, sizeof(p), "%s/done", base);
        if (!flow_put(f, p, ""))
            return 0;
    } else if (!strcmp(state, "runner") || !strcmp(state, "swapped")) {
        char p[160];
        snprintf(p, sizeof(p), "%s/done", base);
        if (!flow_put(f, p, ""))
            return 0;
        snprintf(p, sizeof(p), "%s/failed", base);
        if (!flow_put(f, p, !strcmp(state, "runner") ? "runner\n" : ""))
            return 0;
        if (!strcmp(state, "swapped") && !fx_remove(path(other)))
            return 0;
    }
    fx_env("VERDICT_i686", verdict);
    flow_put(f, "gh.log", "");
    return report(f, kind) == 0;
}

static int report_row(struct fixture *f, const char *kind, const char *state, const char *item)
{
    char pattern[400], label[160];
    int ok;
    clear();
    ok = results(f, kind, state) &&
         log_regex("gh.log", "^issue create .* --label bug --label ci --body ") &&
         log_has("gh.log", "\nAttempt: 1\n") && log_has("gh.log", "\nCommit: bad\n");
    snprintf(pattern, sizeof(pattern), "workflow run bisect.yml .* -f items=%s -f issue=7 -f run=1",
             item);
    ok = ok && log_regex("gh.log", pattern) &&
         (log_regex("gh.log", "issue create .* --label hold ") == !strcmp(kind, "core"));
    snprintf(label, sizeof(label), "report %s %s", kind, state);
    return fx_check(f, ok, label);
}

static int limits_match(const char *kind)
{
    int ok = 1;
    char pattern[128];
    {
        char *limits = fx_read(path("limits")), *events = fx_read(path("events"));
        unsigned a = 0, b = 0;
        char *p;
        for (p = limits; p && *p; p++)
            a += *p == '\n';
        for (p = events; p && *p; p++)
            b += *p == '\n';
        ok = ok && a == b;
        free(limits);
        free(events);
    }
    snprintf(pattern, sizeof(pattern), "^%ss$", !strcmp(kind, "core") ? "10200" : "20400");
    {
        char *s = fx_read(path("limits")), *p = s, *next;
        while (p && *p) {
            next = strchr(p, '\n');
            if (next)
                *next++ = 0;
            if (!fx_regex(p, pattern))
                ok = 0;
            p = next;
        }
        free(s);
    }
    return ok;
}

static int bisect_row(struct fixture *f, const char *item, const char *command, int fail,
                      const char *ghfail)
{
    const char *kind = !strcmp(item, "unfinished-full")    ? "full"
                       : !strcmp(item, "unfinished-night") ? "night"
                                                           : "core";
    const char *args[] = {kind, "i686", "good", "bad", "7", item, NULL};
    char expected[2048], label[240];
    int status, ok;
    fx_env("CI_TEST_FAIL", fail ? "1" : "0");
    fx_env("CI_TEST_GH_FAIL", ghfail);
    fx_env("CI_TEST_RUN", "pass");
    if (!start(f))
        return 0;
    if (strchr(item, ' ')) {
        args[5] = "style";
        args[6] = "core";
    }
    {
        const char *full[9];
        unsigned i;
        for (i = 0; i < 7; i++)
            full[i] = args[i];
        full[7] = NULL;
        status = bisect(f, full);
    }
    ok = status == (*ghfail ? 1 : 0) && log_has("gh.log", "issue comment 7 ");
    if (*ghfail) {
        char text[120];
        snprintf(text, sizeof(text), "the %s on #7 failed",
                 !strcmp(ghfail, "edit") ? "label" : "comment");
        ok = ok && log_has("bisect.log", text);
    }
    if (!strncmp(item, "unfinished-", 11))
        snprintf(expected, sizeof(expected),
                 fail ? "run %s bad\nrun %s bad\nrun %s good\n" : "run %s bad\n", command, command,
                 command);
    else
        snprintf(expected, sizeof(expected), "make\n%s\n", command);
    ok = ok && log_equal("events", expected);
    ok = ok && limits_match(kind);
    ok = ok &&
         log_has("gh.log", fail ? "is the first commit that fails" : "--add-label intermittent");
    snprintf(label, sizeof(label), "bisect %s fail=%d %s", item, fail, ghfail);
    return fx_check(f, ok, label);
}

static int lost_row(struct fixture *f)
{
    int ok = report_row(f, "core", "lost-runner", "default runner") &&
             log_has("gh.log", "--title Core tests fail on i686: default, runner ") &&
             log_has("gh.log", "The run's output was not kept;");
    clear();
    ok = results(f, "core", "lost") && ok;
    return fx_check(
        f,
        ok && log_has("gh.log", "statuses/bad -f state=success -f context=ci/core/i686 ") &&
            log_has("gh.log", "statuses/bad -f state=success -f context=ci/core ") &&
            !log_has("gh.log", "issue create "),
        "lost uploaded result");
}

static int unrun_row(struct fixture *f)
{
    int ok;
    clear();
    fx_env("CI_TEST_UNRUN", "Choose the commit,Test on i686");
    ok = results(f, "core", "missing") &&
         log_has("gh.log", "statuses/bad -f state=error -f context=ci/core/i686 ") &&
         log_has("gh.log", "statuses/bad -f state=error -f context=ci/lost/i686 ") &&
         log_has("gh.log", "workflow run ci.yml --repo test/rach2 --ref master") &&
         !log_has("gh.log", "issue create ") && !log_has("gh.log", "context=ci/core ");
    fx_env("CI_TEST_UNRUN", "Choose the commit,Test on x86_64");
    ok = results(f, "core", "swapped") && ok &&
         log_has("gh.log", "statuses/bad -f state=success -f context=ci/core/i686 ") &&
         log_has("gh.log", "statuses/bad -f state=error -f context=ci/lost/x86_64 ") &&
         log_has("gh.log", "workflow run ci.yml ") && !log_has("gh.log", "issue create ");
    fx_env("CI_TEST_UNRUN", "Choose the commit,Test on i686");
    ok = results(f, "core", "missing") && ok &&
         log_has("gh.log",
                 "issue create --repo test/rach2 --title Core tests did not finish on i686 ") &&
         !log_has("gh.log", "context=ci/lost/") && !log_has("gh.log", "workflow run ci.yml");
    return fx_check(f, ok, "jobs no runner took");
}

static int base_row(struct fixture *f)
{
    int ok;
    clear();
    fx_env("CI_TEST_BASE", "night");
    ok = results(f, "full", "runner") &&
         log_has("gh.log", "-f kind=full -f arch=i686 -f good=good -f bad=bad -f items=runner -f "
                           "issue=7 -f run=1") &&
         log_has("gh.log", "--title Release tests fail on i686: runner ");
    ok = results(f, "core", "runner") && ok && !log_has("gh.log", "workflow run") &&
         log_has("gh.log", "are unknown: no commit in the 50 before bad");
    return fx_check(f, ok, "last nightly pass of a full run");
}

static int again_row(struct fixture *f, const char *labels, const char *later)
{
    char open[256];
    int flaky = strstr(labels, "intermittent") || strstr(later, "intermittent"), ok;
    clear();
    snprintf(open, sizeof(open), "12 %s", labels);
    fx_env("CI_TEST_OPEN_TITLE", "Core tests fail on i686: runner");
    fx_env("CI_TEST_OPEN", open);
    fx_env("CI_TEST_LABELS", later);
    ok = results(f, "core", "runner") && log_has("gh.log", "issue comment 12 ") &&
         !log_regex("gh.log", "issue create .* --title Core tests ");
    ok = ok && (flaky ? log_has("gh.log",
                                "issue edit 12 --repo test/rach2 --remove-label intermittent") &&
                            log_has("gh.log", "-f items=runner -f issue=12 -f run=1")
                      : !log_has("gh.log", "issue edit ") && !log_has("gh.log", "workflow run"));
    return fx_check(f, ok, "repeat failure rechecks intermittent label");
}

static int clean_results(struct fixture *f, const char *kind)
{
    char name[160];
    unsigned i;
    const char *arches[] = {"i686", "x86_64"};
    if (!fx_remove(path("results")))
        return 0;
    for (i = 0; i < 2; i++) {
        snprintf(name, sizeof(name), "results/%s-%s/done", kind, arches[i]);
        if (!flow_put(f, name, ""))
            return 0;
        snprintf(name, sizeof(name), "results/%s-%s/failed", kind, arches[i]);
        if (!flow_put(f, name, ""))
            return 0;
    }
    return flow_put(f, "gh.log", "");
}

static int hold_row(struct fixture *f, int pass)
{
    int ok;
    clear();
    if (!clean_results(f, "core") ||
        (!pass && !flow_put(f, "results/core-i686/failed", "default\n")))
        return 0;
    fx_env("CI_TEST_HOLD", "5 9");
    ok = report(f, "core") == 0 && !log_regex("gh.log", "issue create .* --label hold ");
    ok = ok &&
         (pass ? log_has("gh.log", "issue close 5 ") && log_has("gh.log", "issue close 9 ")
               : log_has("gh.log", "issue comment 5 ") && !log_has("gh.log", "issue comment 9 "));
    return fx_check(
        f, ok, pass ? "close both holds after a pass" : "comment only oldest hold after failure");
}

static int chosen_row(struct fixture *f)
{
    char *s, *p;
    char got[1024] = "";
    int ok;
    clear();
    if (!clean_results(f, "full") || !flow_put(f, "results/full-i686/skipped", ""))
        return 0;
    unlink(path("results/full-i686/failed"));
    fx_env("SUITES", "x86_64:agents");
    ok = report(f, "full") == 0;
    s = fx_read(path("gh.log"));
    p = s;
    while (p && (p = strstr(p, "state="))) {
        size_t n = strcspn(p, "\n");
        const char *end = strstr(p, "context=");
        if (!end || end > p + n) {
            ok = 0;
            break;
        }
        end += strcspn(end, " \n");
        if (strlen(got) + (size_t)(end - p) + 2 >= sizeof(got)) {
            ok = 0;
            break;
        }
        strncat(got, p, (size_t)(end - p));
        strcat(got, ",");
        p = (char *)end;
    }
    free(s);
    return fx_check(
        f,
        ok &&
            !strcmp(got,
                    "state=success -f context=ci/rerun/x86_64,state=success -f context=ci/rerun,"),
        "chosen suites have rerun statuses only");
}

static int latest_row(struct fixture *f, const char *runs, const char *labelled)
{
    const char *args[] = {"-r", "1", "core", "i686", "good", "bad", "7", "unfinished-core", NULL};
    int ok;
    fx_env("CI_TEST_FAIL", "0");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "pass");
    if (!start(f))
        return 0;
    fx_env("CI_TEST_RUNS", runs);
    fx_env("CI_TEST_RUNS_LABELLED", labelled);
    ok = bisect(f, args) == 0 &&
         log_has("gh.log", "issue view 7 --repo test/rach2 --json body,comments ");
    if (!strcmp(runs, "1") && !strcmp(labelled, "1"))
        ok = ok && log_has("gh.log", "--add-label intermittent") &&
             !log_has("gh.log", "--remove-label intermittent");
    else if (!strcmp(runs, "1"))
        ok = ok && log_has("gh.log", "--add-label intermittent") &&
             log_has("gh.log", "--remove-label intermittent") &&
             log_has("gh.log", "run 2 has reported the failure since run 1");
    else
        ok = ok && !log_has("gh.log", "--add-label intermittent") &&
             log_has("gh.log", "run 2 has reported the failure since run 1");
    return fx_check(f, ok, "newest failure run before and after intermittent label");
}

static int handoff_row(struct fixture *f)
{
    const char *first[] = {
        "-r", "1", "-m", "700", "core", "i686", "good", "bad", "7", "unfinished-core", NULL};
    const char *second[] = {
        "-c", "-m", "700", "core", "i686", "good", "bad", "7", "unfinished-core", NULL};
    const char *shorter[] = {"-m", "100", "core", "i686", "good", "bad", "7", "unfinished-core",
                             NULL};
    int ok;
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_STEP", "24000");
    if (!start(f))
        return 0;
    ok = bisect(f, first) == 0 && log_equal("events", "run core bad\n") &&
         log_has("gh.log", "workflow run bisect.yml ") &&
         log_has("gh.log", "-f kind=core -f arch=i686 -f good=good -f bad=bad -f "
                           "items=unfinished-core -f issue=7 -f confirmed=yes -f run=1") &&
         log_has("gh.log", "good passes and bad fails");
    if (!start(f))
        return 0;
    ok = bisect(f, second) == 0 && ok && log_equal("events", "run core bad\nrun core good\n") &&
         !log_has("gh.log", "workflow run") &&
         log_has("gh.log", "is the first commit that fails") &&
         log_has("gh.log", "its parent, good, passes");
    if (!start(f))
        return 0;
    ok = bisect(f, shorter) == 1 && ok && log_equal("events", "") &&
         log_has("gh.log", "unresolved: this run has 100 minutes");
    return fx_check(f, ok, "bisection hands remaining confirmations to next run");
}

static int closed_row(struct fixture *f)
{
    const char *args[] = {"core", "i686", "good", "bad", "7", "unfinished-core", NULL};
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "pass");
    if (!start(f))
        return 0;
    fx_env("CI_TEST_STATE", "CLOSED");
    return fx_check(f,
                    bisect(f, args) == 0 && log_has("bisect.log", "stopped: #7 was closed") &&
                        log_equal("events", "") && !log_has("gh.log", "issue comment 7 "),
                    "closed issue stops before testing or commenting");
}

static int suite_row(struct fixture *f, const char *mode)
{
    const char *args[] = {"core", "i686", "good", "bad", "7", "runner", NULL};
    const char *expected =
        !strcmp(mode, "fail")
            ? "make\ntests runner bad\nmake\ntests runner bad\nmake\ntests runner good\n"
            : "make\ntests runner bad\n";
    int ok, untested = !strcmp(mode, "refuse") || !strcmp(mode, "silent");
    fx_env("CI_TEST_RUN", mode);
    fx_env("CI_TEST_GH_FAIL", "");
    if (!start(f))
        return 0;
    ok = bisect(f, args) == untested && log_equal("events", expected) &&
         log_has("gh.log", untested                ? "unresolved: bad could not be tested again"
                           : !strcmp(mode, "fail") ? "is the first commit that fails"
                                                   : "--add-label intermittent");
    return fx_check(f, ok, mode);
}

static int enroll(struct fixture *f, const char *ledger)
{
    char record[8192];
    if (!fx_remove(path("ledger")) || !fx_mkdir(path("ledger")))
        return 0;
    unlink(path("admitted"));
    snprintf(record, sizeof(record),
             "etude-machine 1\nhost test\nplatform linux x86_64\ncpus 8\nmemory_bytes "
             "8589934592\nledger %s\n",
             ledger ? ledger : path("ledger"));
    return flow_put(f, "state/machine", record);
}

static unsigned matching_lines(const char *text, const char *pattern)
{
    char *copy = strdup(text ? text : ""), *p = copy, *next;
    unsigned count = 0;
    while (p && *p) {
        next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        count += fx_regex(p, pattern);
        p = next;
    }
    free(copy);
    return count;
}

static int admits_row(struct fixture *f, const char *name)
{
    const char *args[12] = {"-e",  path("etude"), "core",   "i686", "good",
                            "bad", "7",           "bigmem", NULL};
    int status, ok;
    if (!start(f) || !enroll(f, NULL))
        return 0;
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "fail");
    if (!strcmp(name, "refused")) {
        if (!flow_put(f, "file", "") || !enroll(f, path("file")))
            return 0;
        args[7] = "build";
    }
    if (!strcmp(name, "ports") && rename(path("tree/tests/system/suites.mk"), path("suites.mk")))
        return 0;
    if (!strcmp(name, "noetude"))
        status = bisect(f, args + 2);
    else
        status = bisect(f, args);
    if (!strcmp(name, "bigmem")) {
        char *s = fx_read(path("admitted"));
        ok = status == 0 &&
             log_equal(
                 "events",
                 "make\ntests bigmem bad\nmake\ntests bigmem bad\nmake\ntests bigmem good\n") &&
             log_has("gh.log", "is the first commit that fails") &&
             matching_lines(s, "^make offset= workers=2 jobs=3 makeflags=-j3 --output-sync "
                               "memory=1536 claim=2147483648$") == 3 &&
             matching_lines(s, "^tests offset=[0-9]+ workers=2 jobs=3 makeflags=-j3 --output-sync "
                               "memory=6144 claim=8589934592$") == 3;
        free(s);
    } else if (!strcmp(name, "ports"))
        ok = status == 1 && log_equal("events", "make\n") &&
             log_has("gh.log", "unresolved: bad could not be tested again");
    else
        ok = status == 1 && log_equal("events", "") &&
             log_has("gh.log", !strcmp(name, "noetude")
                                   ? "unresolved: this machine admits tests through etude"
                                   : "unresolved: bad could not be tested again");
    unlink(path("state/machine"));
    unlink(path("file"));
    if (fx_exists(path("suites.mk")) &&
        rename(path("suites.mk"), path("tree/tests/system/suites.mk")))
        ok = 0;
    return fx_check(f, ok, name);
}

static int environ_row(struct fixture *f)
{
    const char *items[] = {"runner", "unfinished-core", "unfinished-full"};
    unsigned i;
    int ok = 1;
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "fail");
    for (i = 0; i < 3; i++) {
        const char *args[] = {i == 2 ? "full" : "core", "i686", "good", "bad", "7", items[i], NULL};
        char *s, *p, *next;
        const char *want = i == 2 ? " workers= jobs= makeflags= memory="
                                  : " workers=5 jobs=3 makeflags=-j3 --output-sync memory=";
        if (!start(f))
            return 0;
        unlink(path("admitted"));
        ok = bisect(f, args) == 0 && ok && log_has("gh.log", "is the first commit that fails");
        s = fx_read(path("admitted"));
        if (!s || !*s)
            ok = 0;
        p = s;
        while (p && *p) {
            next = strchr(p, '\n');
            if (next)
                *next++ = 0;
            if (!strstr(p, want))
                ok = 0;
            p = next;
        }
        free(s);
    }
    if (rename(path("tree/tools/ci/core.env"), path("core.env")))
        return 0;
    if (!start(f))
        return 0;
    {
        const char *args[] = {"core", "i686", "good", "bad", "7", "runner", NULL};
        ok = bisect(f, args) == 1 && ok && log_equal("events", "") &&
             log_has("gh.log",
                     "unresolved: master's tools/ci/core.env gives no environment for a core run");
    }
    if (rename(path("core.env"), path("tree/tools/ci/core.env")))
        ok = 0;
    return fx_check(f, ok, "core bisections inherit exact core environment");
}

static int structured_prepare(struct fixture *f, const char *name, const char *streams)
{
    if (!start(f) || !runsh(f, "full", "history") ||
        !fx_tree(flow_source(f, !strcmp(name, "missing") ? "tools/etude/fixtures/verdict/cases-more"
                                                         : "tools/etude/fixtures/verdict/cases"),
                 path("tree/tests/cases")))
        return 0;
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "pass");
    fx_env("CI_TEST_STATUS",
           !strcmp(name, "pass") || !strcmp(name, "missing") || !strcmp(name, "hidden") ? "0"
                                                                                        : "1");
    fx_env("CI_TEST_STREAMS", streams);
    if (!strcmp(name, "run"))
        fx_env("CI_TEST_FAILED", "runner");
    if (!fx_mkdir(path("tree/build/tools/etude")))
        return 0;
    if (!strcmp(name, "older")) {
        if (!fx_copy(f->self, path("tree/build/tools/etude/casecheck")))
            return 0;
    } else {
        if (!flow_put(f, "tree/tools/etude/etude.c", ""))
            return 0;
        if (!strcmp(name, "unjudged")) {
            fx_env("CI_TEST_JUDGE", "unjudged");
            if (!fx_copy(f->self, path("tree/build/tools/etude/etude")))
                return 0;
        } else if (!fx_copy(path("etude"), path("tree/build/tools/etude/etude")))
            return 0;
    }
    return 1;
}

static const char *structured_streams(const char *name)
{
    return !strcmp(name, "pass") || !strcmp(name, "missing") || !strcmp(name, "status")
               ? pass_streams
           : !strcmp(name, "unfinished") ? unfinished_streams
           : !strcmp(name, "cut")        ? cut_streams
                                         : failure_streams;
}

static int structured_row(struct fixture *f, const char *name)
{
    const char *item = !strcmp(name, "elsewhere") ? "host"
                       : !strcmp(name, "run")     ? "unfinished-core"
                                                  : "default";
    const char *streams = structured_streams(name);
    const char *args[] = {"core", "i686", "good", "bad", "7", item, NULL};
    char expected[512];
    int status, ok;
    unsigned i;
    if (!structured_prepare(f, name, streams))
        return 0;
    status = bisect(f, args);
    if (!strcmp(name, "failure") || !strcmp(name, "older")) {
        const char *boots[] = {"1-bad", "2-bad", "3-good"};
        ok = status == 0 &&
             log_equal(
                 "events",
                 "make\ntests default bad\nmake\ntests default bad\nmake\ntests default good\n") &&
             log_has("gh.log", "is the first commit that fails");
        for (i = 0; i < 3; i++) {
            char p[256];
            snprintf(p, sizeof(p), "logs/bisect/%s.history.1/history/plan.jsonl", boots[i]);
            ok = fx_exists(path(p)) && ok;
            snprintf(p, sizeof(p), "logs/bisect/%s.history.1/verdict", boots[i]);
            ok = log_has(p, !strcmp(name, "older") ? "casecheck: result: "
                                                   : "etude check: result: ") &&
                 ok;
        }
    } else if (!strcmp(name, "pass"))
        ok = status == 0 && log_has("gh.log", "--add-label intermittent");
    else {
        snprintf(expected, sizeof(expected),
                 !strcmp(name, "run") ? "run core bad\n" : "make\ntests %s bad\n", item);
        ok = status == 1 && log_equal("events", expected) &&
             log_has("gh.log", "unresolved: bad could not be tested again");
    }
    if (!runsh(f, "full", "lines") || !fx_remove(path("tree/build")) ||
        !fx_remove(path("tree/tests/cases")) || !fx_mkdir(path("tree/tests/cases")))
        ok = 0;
    unlink(path("tree/tools/etude/etude.c"));
    fx_env("CI_TEST_JUDGE", NULL);
    return fx_check(f, ok, name);
}

static int faulted_row(struct fixture *f, const char *item)
{
    const char *args[] = {"core", "i686", "good", "bad", "7", item, NULL};
    int runner = !strcmp(item, "runner"), ok;
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "lost");
    if (!start(f))
        return 0;
    ok = bisect(f, args) == !runner &&
         log_has("gh.log", runner ? "is the first commit that fails"
                                  : "unresolved: bad could not be tested again");
    return fx_check(f, ok,
                    runner ? "runner failure is bisected"
                           : "suite plus runner fault leaves range unresolved");
}

static int late_row(struct fixture *f, const char *result)
{
    const char *args[] = {"core", "i686", "good", "bad", "7", "default", NULL};
    char mode[80];
    int failed = !strcmp(result, "failed"), ok;
    snprintf(mode, sizeof(mode), "late-%s", result);
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", mode);
    if (!start(f))
        return 0;
    ok = bisect(f, args) == !failed &&
         log_equal(
             "events",
             failed ? "make\ntests default bad\nmake\ntests default bad\nmake\ntests default good\n"
                    : "make\ntests default bad\n") &&
         log_has("gh.log", failed ? "is the first commit that fails"
                                  : "unresolved: bad could not be tested again");
    return fx_check(f, ok, mode);
}

static int scoped_row(struct fixture *f)
{
    const char *args[] = {"core", "i686", "good", "bad", "7", "default", "core", NULL};
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "partial");
    if (!start(f))
        return 0;
    return fx_check(
        f, bisect(f, args) == 1 && log_has("gh.log", "unresolved: bad could not be tested again"),
        "suite results belong to their own tests/run invocation");
}

static const char attempt_item[] = "guest.default.rngprobe on i686, item r1-i0: failed, and passed "
                                   "on another attempt, so it may be flaky";

static const char attempt_assertion[] = "    trusted-monobit: fail: 61 of 512 bits set";

static const char attempt_intro[] =
    "The attempts that did not pass, from etude's history of the run; a failure stays listed when "
    "a later attempt of its case passed:";

static int attempts_prepare(struct fixture *f, const char *name)
{
    char history[4096];
    clear();
    if (!clean_results(f, "core") || !flow_put(f, "results/core-i686/history/run.jsonl", "") ||
        !fx_mkdir(path("results/core-i686/store")))
        return 0;
    snprintf(history, sizeof(history),
             "run r1: failure: of 2 attempts, 1 failed, 0 did not finish\n%s\n  attempt r1-i0-a0: "
             "failure: it exited 1\n%s\n  attempt r1-i0-a1, a retry of r1-i0-a0: pass",
             attempt_item, attempt_assertion);
    fx_env("CI_TEST_HISTORY_STATUS", "1");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_MAKE_FAIL", "");
    fx_env("CI_TEST_HISTORY_OUT", history);
    if (!strcmp(name, "flaky") || !strcmp(name, "repeated")) {
        if (!flow_put(f, "results/core-i686/failed", "default\n"))
            return 0;
    }
    if (!strcmp(name, "unread")) {
        fx_env("CI_TEST_HISTORY_STATUS", "2");
        fx_env("CI_TEST_HISTORY_OUT",
               "etude show: run.jsonl:3: the record of kind `run` is incomplete");
    }
    if (!strcmp(name, "unbuilt")) {
        fx_env("ETUDE", NULL);
        fx_env("CI_TEST_MAKE_FAIL", "1");
    }
    if (!strcmp(name, "kept")) {
        fx_env("CI_TEST_HISTORY_STATUS", "0");
        fx_env("CI_TEST_HISTORY_OUT", "");
    }
    if (!strcmp(name, "repeated") || !strcmp(name, "kept")) {
        fx_env("CI_TEST_OPEN_TITLE", "Core tests fail on i686: default");
        fx_env("CI_TEST_OPEN", "12 bug ci intermittent");
        fx_env("CI_TEST_LABELS", "bug ci intermittent");
    }
    if (!flow_put(f, "gh.log", "") || !flow_put(f, "reader.log", "") || !flow_put(f, "summary", ""))
        return 0;
    fx_env("GITHUB_STEP_SUMMARY", path("summary"));
    return 1;
}

static int attempts_detail(const char *name)
{
    int ok = 1;
    if (!strcmp(name, "flaky"))
        ok = ok &&
             log_has("gh.log", "issue create --repo test/rach2 --title Core tests fail on i686: "
                               "default --label bug --label ci ") &&
             log_has("gh.log", "-f items=default -f issue=7 -f run=1");
    if (!strcmp(name, "repeated"))
        ok = ok && log_has("gh.log", "issue comment 12 ") &&
             !log_regex("gh.log", "issue create .* --title Core tests ") &&
             log_has("gh.log", "issue edit 12 --repo test/rach2 --remove-label intermittent") &&
             log_has("gh.log", "-f items=default -f issue=12 -f run=1");
    if (!strcmp(name, "hidden"))
        ok = ok &&
             log_has("gh.log",
                     "issue create --repo test/rach2 --title Core tests fail on i686: runner "
                     "--label bug --label ci --body The core tests of commit bad failed on i686: "
                     "`runner`. The run listed no failure, but etude's history of the run has "
                     "attempts that did not pass, so the failure is the test runner's.") &&
             log_has("gh.log", "-f items=runner -f issue=7 -f run=1");
    if (!strcmp(name, "unread"))
        ok = ok &&
             log_has("gh.log", "failed on i686: `runner`. The run listed no failure, but etude's "
                               "history of the run could not be read") &&
             log_has("gh.log", "\nEtude's history of the run could not be read:\n") &&
             log_has("gh.log",
                     "\netude show: run.jsonl:3: the record of kind 'run' is incomplete\n");
    if (!strcmp(name, "unbuilt"))
        ok = ok && log_equal("reader.log", "") &&
             log_has("gh.log", "\nci: report: build/tools/etude/etude could not be built\n");
    return ok;
}

static int attempts_row(struct fixture *f, const char *name)
{
    char reader[8192];
    int ok;
    if (!attempts_prepare(f, name))
        return 0;
    ok = report(f, "core") == 0 && !log_regex("gh.log", "^issue close ") &&
         !log_regex("gh.log", "^issue edit .* --body");
    if (!strcmp(name, "kept"))
        return fx_check(
            f,
            ok && log_has("gh.log", "statuses/bad -f state=success -f context=ci/core/i686 ") &&
                log_has("gh.log", "statuses/bad -f state=success -f context=ci/core ") &&
                !log_regex("gh.log", "^issue (create|comment|edit) "),
            "passing history keeps intermittent issue open");
    ok = ok && log_has("gh.log", "statuses/bad -f state=failure -f context=ci/core/i686 ") &&
         log_regex("gh.log", "issue create .* --label hold ");
    if (!strcmp(name, "flaky") || !strcmp(name, "repeated") || !strcmp(name, "hidden")) {
        snprintf(reader, sizeof(reader), "-b %s %s\n", path("results/core-i686/store"),
                 path("results/core-i686/history/run.jsonl"));
        ok = ok && log_equal("reader.log", reader) && log_has("gh.log", attempt_intro) &&
             log_has("summary", attempt_intro) && log_has("gh.log", attempt_item) &&
             log_has("summary", attempt_item) && log_has("gh.log", attempt_assertion) &&
             log_has("summary", attempt_assertion);
    }
    ok = ok && attempts_detail(name);
    return fx_check(f, ok, name);
}

static char *fence(const char *text, const char *after)
{
    const char *p = after ? strstr(text, after) : text, *begin, *end;
    if (!p || (begin = strstr(p, "```\n")) == NULL)
        return NULL;
    begin += 4;
    end = strstr(begin, "\n```");
    return end ? strndup(begin, (size_t)(end - begin)) : NULL;
}

static int quoted_row(struct fixture *f)
{
    const char *own = "run: default: guest.default.loginmusl on i686: incomplete: it ran past its "
                      "stage bound of 60000 ms: root is missing: nothing reported it\nrun: "
                      "default: INCOMPLETE in 119s\nrun: result: failed: default";
    char input[4096], *issue, *summary, *a, *b;
    int ok;
    clear();
    if (!clean_results(f, "core") || !flow_put(f, "results/core-i686/failed", "default\n"))
        return 0;
    snprintf(input, sizeof(input),
             "run: starting: default runner\nrun: starting: failing\nrun: failing: FAIL in "
             "1s\nrun: result: failed: failing\nrun: runner: PASS in 9s\n%s\n",
             own);
    if (!flow_put(f, "results/core-i686/log", input))
        return 0;
    snprintf(input, sizeof(input), "%s\n", own);
    if (!flow_put(f, "results/core-i686/err", input) || !flow_put(f, "gh.log", "") ||
        !flow_put(f, "summary", ""))
        return 0;
    fx_env("GITHUB_STEP_SUMMARY", path("summary"));
    ok = report(f, "core") == 0 &&
         log_has("gh.log",
                 "issue create --repo test/rach2 --title Core tests fail on i686: default ");
    issue = fx_read(path("gh.log"));
    summary = fx_read(path("summary"));
    a = issue ? fence(issue, "The first lines of tests/run's standard error that name a failure:")
              : NULL;
    b = summary ? fence(summary, NULL) : NULL;
    ok = ok && a && b && !strcmp(a, own) && !strcmp(b, own);
    free(a);
    free(b);
    free(issue);
    free(summary);
    return fx_check(f, ok, "report quotes its own stderr instead of a nested run");
}

static int reset_row(struct fixture *f)
{
    static const char *const unset[] = {"CI_TEST_UNRUN",
                                        "CI_TEST_STEP",
                                        "CI_TEST_FAILED",
                                        "CI_TEST_MAKE_FAIL",
                                        "CI_TEST_HISTORY_OUT",
                                        "CI_TEST_HISTORY_STATUS",
                                        "CI_TEST_JUDGE",
                                        "CI_TEST_HOLD",
                                        "RACH2_PORT_OFFSET",
                                        "SSH_TEST_PORT",
                                        "RACH2_WORKERS",
                                        "RACH2_MEMORY_MIB",
                                        "RACH2_CPUS",
                                        "GITHUB_RUN_ATTEMPT",
                                        "RACH2_RESULTS",
                                        "RACH2_FAILED",
                                        "JOBS",
                                        "MAKEFLAGS",
                                        "ETUDE_SITE",
                                        "ETUDE_ADMITTED",
                                        "GITHUB_STEP_SUMMARY",
                                        "SUITES",
                                        NULL};
    unsigned i;
    for (i = 0; unset[i]; i++)
        fx_env(unset[i], NULL);
    fx_env("CI_TEST_FAIL", "0");
    fx_env("CI_TEST_GH_FAIL", "");
    fx_env("CI_TEST_RUN", "pass");
    fx_env("CI_TEST_STATUS", "0");
    fx_env("CI_TEST_STREAMS", pass_streams);
    fx_env("ETUDE", path("bin/etude-reader"));
    clear();
    unlink(path("state/machine"));
    return runsh(f, "full", "lines");
}

static int prepare(struct fixture *f)
{
    const char *mocks[] = {"git", "gh", "make", "timeout", "date", "etude-reader", "sh", NULL};
    const char *schemas[] = {"case", "results", "history"};
    unsigned i;
    char source[160], target[160];
    fx_env("FLOW_CI", "1");
    fx_env("CI_TEST_WORK", f->work);
    fx_env("ETUDE_STATE", path("state"));
    fx_env("RACH2_PORT_DIR", path("locks"));
    fx_env("GITHUB_REPOSITORY", "test/rach2");
    fx_env("GITHUB_SERVER_URL", "https://example.invalid");
    fx_env("GITHUB_RUN_ID", "1");
    fx_env("CI_TEST_FIXTURES", flow_source(f, "tools/etude/fixtures/verdict"));
    fx_env("CI_TEST_PASS", pass_streams);
    if (!fx_mkdir(path("state")) || !fx_mkdir(path("locks")) ||
        !flow_copy(f, "tools/ci/report.sh", "report.sh") ||
        !fx_replace(flow_source(f, "tools/ci/bisect.sh"), path("bisect.sh"),
                    "/etc/rach2/etude-site", path("no-installed-site")) ||
        !fx_copy("/host-products/etude", path("etude")))
        return 0;
    for (i = 0; mocks[i]; i++)
        if (!fx_mock(f, mocks[i]))
            return 0;
    if (!flow_put(f, "tree/tests/registry",
                  "default|qemu|core\nstyle|-|core\nhost|-|core\nrunner|-|full\nbigmem|qemu|full||"
                  "6144|\n") ||
        !flow_put(f, "tree/tests/system/suites.mk",
                  "PORT = $(shell echo $$((2222 + $${RACH2_PORT_OFFSET:-0})))\n") ||
        !flow_put(f, "tree/tools/ci/core.env",
                  "# The environment of a core run.\nJOBS=3\nMAKEFLAGS=-j3 "
                  "--output-sync\nRACH2_WORKERS=5\n") ||
        !flow_put(f, "tree/tests/run",
                  "#!/bin/sh\n# Native command fixture reads RACH2_PORT_OFFSET.\n") ||
        !fx_mkdir(path("tree/tests/cases")))
        return 0;
    for (i = 0; i < 3; i++) {
        snprintf(source, sizeof(source), "tools/etude/schemas/%s.schema.json", schemas[i]);
        snprintf(target, sizeof(target), "tree/tools/etude/schemas/%s.schema.json", schemas[i]);
        if (!flow_copy(f, source, target))
            return 0;
    }
    return reset_row(f);
}
/* Each original top-level row had its own subshell. Reset its environment
 * here, while allowing the several actions within one row to share state. */
#define ROW(action)          \
    do {                     \
        if (!reset_row(f))   \
            return 0;        \
        ok = (action) && ok; \
    } while (0)

static int native_report_row(struct fixture *f, const char *mode)
{
    if (!clean_results(f, "core") || !runsh(f, "full", "native"))
        return 0;
    for (unsigned i = 0; i < 2; i++) {
        const char *arch = i ? "x86_64" : "i686";
        char file[160];
        snprintf(file, sizeof(file), "results/core-%s/native", arch);
        if (!flow_put(f, file, "etude-native-1\n"))
            return 0;
        snprintf(file, sizeof(file), "results/core-%s/suites", arch);
        if (!flow_put(f, file, "default\n"))
            return 0;
    }
    if (strcmp(mode, "missing"))
        if (!flow_put(f, "results/history/run.jsonl", "fixture\n") ||
            !fx_mkdir(path("results/store")))
            return 0;
    fx_env("CI_TEST_NATIVE_GUARD", mode);
    int ok = report(f, "core") == 0;
    int fail = strcmp(mode, "pass") != 0;
    ok = ok && log_has("gh.log", fail ? "-f state=failure -f context=ci/core/i686 "
                                      : "-f state=success -f context=ci/core/i686 ");
    ok = ok && log_has("gh.log", fail ? "-f state=failure -f context=ci/core/x86_64 "
                                      : "-f state=success -f context=ci/core/x86_64 ");
    fx_env("CI_TEST_NATIVE_GUARD", NULL);
    return fx_check(f, ok, "native reporting validates complete history and budget");
}

static int native_bisect_row(struct fixture *f, int runner)
{
    const char *args[] = {"core", "i686", "good", "bad", "7", "default", NULL};
    if (!runsh(f, "full", "native") || !start(f))
        return 0;
    fx_env("CI_TEST_FAIL", "1");
    fx_env("CI_TEST_FAILED", runner ? "runner" : "default");
    int code = bisect(f, args);
    int ok = code == (runner ? 1 : 0) && !log_has("events", "make") &&
             log_has("events", "native bad") && log_equal("limits", "");
    ok = ok && log_has("gh.log", runner ? "unresolved: bad could not be tested again"
                                        : "is the first commit that fails");
    return fx_check(f, ok, "native bisection uses the public entry without a legacy timeout");
}

int flow_ci(struct fixture *f)
{
    const char *kinds[] = {"core", "full"}, *states[] = {"missing", "unfinished", "missing-failed"};
    const char *suites[] = {"pass", "fail", "refuse", "silent"},
               *attempts[] = {"flaky", "repeated", "hidden", "unread", "unbuilt", "kept"},
               *admission[] = {"bigmem", "refused", "noetude", "ports"};
    const char *structured[] = {"failure", "older",  "pass",   "unfinished", "cut", "elsewhere",
                                "missing", "status", "hidden", "unjudged",   "run"};
    unsigned i, j;
    int ok = 1;
    if (!prepare(f))
        return 0;
    for (i = 0; i < 2; i++) {
        char item[80];
        snprintf(item, sizeof(item), "unfinished-%s", kinds[i]);
        for (j = 0; j < 3; j++)
            ROW(report_row(f, kinds[i], states[j], item));
        ROW(report_row(f, kinds[i], "runner", "runner"));
        ROW(bisect_row(f, item, kinds[i], 0, ""));
        ROW(bisect_row(f, item, kinds[i], 1, ""));
    }
    ROW(bisect_row(f, "unfinished-night", "full", 0, ""));
    if (!reset_row(f) || !runsh(f, "night", "lines"))
        return 0;
    ok = bisect_row(f, "unfinished-full", "night", 0, "") && ok;
    ROW(lost_row(f));
    ROW(unrun_row(f));
    ROW(base_row(f));
    ROW(again_row(f, "bug ci intermittent", "bug ci intermittent"));
    ROW(again_row(f, "bug ci", "bug ci"));
    ROW(again_row(f, "bug ci", "bug ci intermittent"));
    ROW(hold_row(f, 0));
    ROW(hold_row(f, 1));
    ROW(bisect_row(f, "style core", "tests style bad\ntests core bad", 0, ""));
    ROW(bisect_row(f, "unfinished-core", "core", 1, "comment"));
    ROW(bisect_row(f, "unfinished-core", "core", 0, "comment"));
    ROW(bisect_row(f, "unfinished-core", "core", 0, "edit"));
    ROW(latest_row(f, "1", "1"));
    ROW(latest_row(f, "1 2", "1 2"));
    ROW(latest_row(f, "1", "1 2"));
    ROW(handoff_row(f));
    ROW(closed_row(f));
    ROW(chosen_row(f));
    for (i = 0; i < 4; i++)
        ROW(suite_row(f, suites[i]));
    ok = fx_check(f, !fx_exists(path("reader.log")),
                  "no history reader for a result without history") &&
         ok;
    for (i = 0; i < 6; i++)
        ROW(attempts_row(f, attempts[i]));
    ROW(quoted_row(f));
    for (i = 0; i < 4; i++)
        ROW(admits_row(f, admission[i]));
    ROW(environ_row(f));
    for (i = 0; i < 11; i++)
        ROW(structured_row(f, structured[i]));
    ROW(faulted_row(f, "default"));
    ROW(faulted_row(f, "runner"));
    ROW(late_row(f, "failed"));
    ROW(late_row(f, "incomplete"));
    ROW(scoped_row(f));
    ROW(native_report_row(f, "pass"));
    ROW(native_report_row(f, "budget"));
    ROW(native_report_row(f, "missing"));
    ROW(native_bisect_row(f, 0));
    ROW(native_bisect_row(f, 1));
    return ok;
}
#undef ROW
