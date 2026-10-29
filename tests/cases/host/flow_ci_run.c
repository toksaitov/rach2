/* flow_ci_run.c: C fixtures for the release tests' run.sh and killed steps. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *env(const char *key)
{
    const char *s = getenv(key);
    return s ? s : "";
}

static const char *path(const char *name)
{
    static char paths[24][8192];
    static unsigned next;
    char *s = paths[next++ % 24];
    snprintf(s, 8192, "%s/%s", env("CI_RUN_WORK"), name);
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

static pid_t parent(pid_t pid)
{
    char filename[80], *s, *last, state;
    long p = 0;
    snprintf(filename, sizeof(filename), "/proc/%ld/stat", (long)pid);
    s = fx_read(filename);
    last = s ? strrchr(s, ')') : NULL;
    if (last && sscanf(last + 1, " %c %ld", &state, &p) != 2)
        p = 0;
    free(s);
    return (pid_t)p;
}

static int run_shell(pid_t pid)
{
    char filename[80], command[8192];
    FILE *f;
    size_t n, i;
    snprintf(filename, sizeof(filename), "/proc/%ld/cmdline", (long)pid);
    f = fopen(filename, "rb");
    if (!f)
        return 0;
    n = fread(command, 1, sizeof(command) - 1, f);
    fclose(f);
    for (i = 0; i < n; i++)
        if (!command[i])
            command[i] = ' ';
    command[n] = 0;
    return !strncmp(command, "sh tools/ci/run.sh ", strlen("sh tools/ci/run.sh "));
}
/** Match the original cancellation: kill inner step shells, the command
 * itself, or the run shell's parent. The outer monitor remains to collect
 * every byte and observes EOF only after the run's descendants close it. */

static void cut(const char *where)
{
    const char *request = env("CI_RUN_TEST_CUT"), *how = strchr(request, ' ');
    pid_t chain[64], pid;
    unsigned n = 0, i;
    if (!how || (size_t)(how - request) != strlen(where) || strncmp(request, where, strlen(where)))
        return;
    pid = parent(getpid());
    while (pid > 1 && n < 64 && run_shell(pid)) {
        chain[n++] = pid;
        pid = parent(pid);
    }
    if (!n) {
        fputs("cut: no shell of run.sh above command\n", stderr);
        _exit(2);
    }
    how++;
    if (!strcmp(how, "parent")) {
        kill(pid, SIGKILL);
        return;
    }
    if (!strcmp(how, "step"))
        for (i = 0; i + 1 < n; i++)
            kill(chain[i], SIGKILL);
    kill(getpid(), SIGKILL);
    _exit(2);
}

static int monitor(int argc, char **argv)
{
    int pipefd[2], status;
    pid_t outer;
    char bytes[32768];
    ssize_t n;
    if (argc != 5 || pipe(pipefd))
        return 2;
    outer = fork();
    if (outer < 0)
        return 2;
    if (!outer) {
        pid_t child;
        char value[64];
        close(pipefd[0]);
        if (dup2(pipefd[1], 1) < 0 || dup2(pipefd[1], 2) < 0)
            _exit(2);
        close(pipefd[1]);
        child = fork();
        if (child < 0)
            _exit(2);
        if (!child) {
            if (chdir(argv[1]))
                _exit(2);
            execl("/bin/sh", "sh", "tools/ci/run.sh", argv[2], argv[3], (char *)NULL);
            _exit(127);
        }
        while (waitpid(child, &status, 0) < 0)
            if (errno != EINTR)
                _exit(2);
        snprintf(value, sizeof(value), "%d\n",
                 WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        if (!fx_text(argv[4], value))
            _exit(2);
        _exit(0);
    }
    close(pipefd[1]);
    while ((n = read(pipefd[0], bytes, sizeof(bytes))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(pipefd[0]);
            return 2;
        }
        if (fwrite(bytes, 1, (size_t)n, stdout) != (size_t)n) {
            close(pipefd[0]);
            return 2;
        }
    }
    close(pipefd[0]);
    while (waitpid(outer, &status, 0) < 0)
        if (errno != EINTR)
            return 2;
    return 0;
}

static void escaped(FILE *f, const char *text)
{
    while (*text) {
        if (text[0] == '\\' && text[1]) {
            text++;
            if (*text == 'n')
                fputc('\n', f);
            else if (*text == 't')
                fputc('\t', f);
            else if (*text == 'r')
                fputc('\r', f);
            else
                fputc(*text, f);
            text++;
        } else
            fputc(*text++, f);
    }
}

static int tests(int argc, char **argv)
{
    char line[16384] = "tests/run ";
    char *keep, *p;
    const char *failed = *env("RACH2_FAILED") ? env("RACH2_FAILED") : "build/tests/failed";
    int i;
    for (i = 1; i < argc; i++) {
        if (i > 1)
            strcat(line, " ");
        if (strlen(line) + strlen(argv[i]) + 1 >= sizeof(line))
            return 2;
        strcat(line, argv[i]);
    }
    if (strlen(line) + strlen(env("RACH2_RESULTS")) + 20 >= sizeof(line))
        return 2;
    strcat(line, " with ");
    strcat(line, *env("RACH2_RESULTS") ? env("RACH2_RESULTS") : "no results");
    if (!append(path("calls"), line) || !append(path("calls.arch"), env("ARCH")))
        return 2;
    keep = strdup(env("CI_RUN_TEST_KEEP"));
    p = strtok(keep ? keep : "", " ");
    while (p) {
        char file[8192];
        snprintf(file, sizeof(file), "%s/%s", failed, p);
        if (!fx_mkdir(file)) {
            free(keep);
            return 2;
        }
        p = strtok(NULL, " ");
    }
    free(keep);
    cut(argc > 1 ? argv[1] : "");
    if (argc < 2 || strcmp(argv[1], env("CI_RUN_TEST_SET"))) {
        printf("run: %s: PASS in 1s\nrun: result: passed\n", argc > 1 ? argv[1] : "");
        return 0;
    }
    escaped(stdout, env("CI_RUN_TEST_PRINT"));
    fflush(stdout);
    escaped(stderr, env("CI_RUN_TEST_ERR"));
    fflush(stderr);
    escaped(stdout, env("CI_RUN_TEST_LATE"));
    return atoi(env("CI_RUN_TEST_STATUS"));
}

int flow_ci_run_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "uname")) {
        puts("Linux");
        return 0;
    }
    if (!strcmp(name, "git")) {
        if (argc == 5 && !strcmp(argv[1], "-C") && !strcmp(argv[3], "rev-parse") &&
            !strcmp(argv[4], "HEAD")) {
            puts("0123456789abcdef0123456789abcdef01234567");
            return 0;
        }
        if (argc >= 3 && !strcmp(argv[1], "diff"))
            return 0;
        if (argc == 4 && !strcmp(argv[1], "rev-parse") && !strcmp(argv[2], "--short") &&
            !strcmp(argv[3], "HEAD")) {
            puts("0123456");
            return 0;
        }
        if (argc == 3 && !strcmp(argv[1], "rev-parse") && !strcmp(argv[2], "HEAD")) {
            puts("0123456789abcdef0123456789abcdef01234567");
            return 0;
        }
        return 2;
    }
    if (!strcmp(name, "make")) {
        if (argc == 1)
            cut("make");
        if (!strcmp(env("ARCH"), env("CI_RUN_TEST_BROKEN")))
            return 1;
        return atoi(env("CI_RUN_TEST_MAKE"));
    }
    if (!strcmp(name, "run"))
        return tests(argc, argv);
    if (!strcmp(name, "ci-run-watch"))
        return monitor(argc, argv);
    return 2;
}
struct run_case {
    const char *name, *set, *failed;
    int status, make_status;
    const char *out, *err, *late;
};
struct run_fixture {
    struct fixture *f;
    unsigned rows;
    char output[8192];
    const char *arch, *suites, *keep, *broken;
};

static void settings(struct run_fixture *r, const struct run_case *c)
{
    char value[80];
    fx_env("ARCH", r->arch);
    fx_env("SUITES", r->suites);
    fx_env("CI_RUN_TEST_SET", c->set);
    snprintf(value, sizeof(value), "%d", c->status);
    fx_env("CI_RUN_TEST_STATUS", value);
    snprintf(value, sizeof(value), "%d", c->make_status);
    fx_env("CI_RUN_TEST_MAKE", value);
    fx_env("CI_RUN_TEST_PRINT", c->out ? c->out : "");
    fx_env("CI_RUN_TEST_ERR", c->err ? c->err : "");
    fx_env("CI_RUN_TEST_LATE", c->late ? c->late : "");
    fx_env("CI_RUN_TEST_KEEP", r->keep);
    fx_env("CI_RUN_TEST_BROKEN", r->broken);
    fx_env("CI_RUN_TEST_CUT", "");
}

static char *flat(const char *file, int calls)
{
    char *s = fx_read(file), *p = s, *next;
    size_t used = 0;
    if (!s)
        return NULL;
    while (p && *p) {
        size_t n;
        next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        if (calls) {
            char *at = strstr(p, " with ");
            if (at)
                *at = 0;
        }
        n = strlen(p);
        if (used)
            s[used++] = ' ';
        memmove(s + used, p, n);
        used += n;
        p = next;
    }
    s[used] = 0;
    return s;
}

static const char *output_path(struct run_fixture *r, const char *name)
{
    static char files[16][8192];
    static unsigned next;
    char *s = files[next++ % 16];
    if (snprintf(s, 8192, "%s/%s", r->output, name) >= 8192)
        exit(2);
    return s;
}

/** Run run.sh with KIND and check its status, and what it failed unless
 * `failed` is NULL, for a refusal that writes no result. */
static int start(struct run_fixture *r, const struct run_case *c, const char *kind)
{
    struct fixture *f = r->f;
    const char *argv[] = {"/bin/sh", "tools/ci/run.sh", kind, r->output, NULL};
    char *got;
    int ok;
    snprintf(r->output, sizeof(r->output), "%s/out.%u", f->work, ++r->rows);
    if (!flow_put(f, "calls", "") || !flow_put(f, "calls.arch", ""))
        return 0;
    settings(r, c);
    fx_run(f, argv, path("tree"), NULL, NULL);
    if (!c->failed)
        return fx_check(f, f->status == c->status && !fx_exists(output_path(r, "done")), c->name);
    got = flat(output_path(r, "failed"), 0);
    ok = got && !strcmp(got, c->failed) && fx_exists(output_path(r, "done")) &&
         f->status == (*c->failed ? 1 : 0);
    if (!ok)
        fprintf(stderr, "ci_run: %s: failed '%s', status %d; expected '%s'\n", c->name,
                got ? got : "(missing)", f->status, c->failed);
    free(got);
    return fx_check(f, ok, c->name);
}

static int row(struct run_fixture *r, const struct run_case *c)
{
    return start(r, c, "full");
}

static int words(struct run_fixture *r, const char *file, const char *wanted)
{
    char *got = flat(file, 1);
    int ok = fx_exists(file) && got && !strcmp(got, wanted);
    free(got);
    return fx_check(r->f, ok, wanted);
}

static int kept(struct run_fixture *r, const char *err)
{
    return fx_check(r->f, fx_equal(output_path(r, "err"), err),
                    "keeps only tests/run standard error");
}

static int lines_rows(struct run_fixture *r)
{
    static const struct run_case rows[] = {
        {"lines: pass", "none", "", 0, 0, NULL, NULL, NULL},
        {"lines: failed suite", "style", "i686:default", 1, 0, "run: style: PASS in 3s\n",
         "run: i686:default: FAIL in 3s\nrun: result: failed: i686:default\n", NULL},
        {"lines: no suite named", "style", "i686:all x86_64:all", 1, 0,
         "run: default: PASS in 3s\n", "run: result: failed: nothing-such\n", NULL},
        {"lines: runner's own lines", "style", "i686:all x86_64:all", 1, 0,
         "run: runner: PASS in 9s\n",
         "run: result: failed: i686:default\nrun: shutdown: INCOMPLETE in ?s\n", NULL},
        {"lines: stderr before stdout", "style", "i686:default x86_64:shutdown", 1, 0, NULL,
         "run: i686:default: INCOMPLETE in 121s\nrun: x86_64:shutdown: INCOMPLETE in 301s\nrun: "
         "result: failed: i686:default x86_64:shutdown\n",
         "run: runner: PASS in 9s\nrun: result: incomplete\n"},
        {"lines: bigmem alone", "bigmem", "x86_64:bigmem", 1, 0, NULL,
         "run: result: failed: bigmem\n", NULL},
        {"lines: build", "none", "i686:build x86_64:build", 0, 1, NULL, NULL, NULL}};
    unsigned i;
    int ok = 1;
    r->suites = "";
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        ok = row(r, &rows[i]) && ok;
        if (i == 4)
            ok = kept(r, rows[i].err) && ok;
    }
    return ok;
}

static int refused_rows(struct run_fixture *r)
{
    struct run_case c = {"refused: core", "none", NULL, 2, 0, NULL, NULL, NULL};
    int ok = start(r, &c, "core");
    c.name = "refused: one architecture";
    r->arch = "i686";
    ok = row(r, &c) && ok;
    r->arch = "i686 x86_64";
    c.name = "refused: a suite the architecture does not run";
    r->suites = "i686:fpu";
    ok = row(r, &c) && ok;
    r->suites = "";
    return ok;
}

struct cut_case {
    const char *name, *request, *sets, *wanted;
};

static int cut_row(struct run_fixture *r, const struct cut_case *test)
{
    const struct run_case c = {test->name, "none", "", 0, 0, NULL, NULL, NULL};
    const char *args[] = {path("bin/ci-run-watch"), path("tree"), "full", r->output,
                          path("status"),           NULL};
    struct fixture *f = r->f;
    char *got, *status, *calls, *p, *next;
    char names[1024] = "";
    int ok;
    snprintf(r->output, sizeof(r->output), "%s/out.%u", f->work, ++r->rows);
    if (!flow_put(f, "calls", "") || !flow_put(f, "calls.arch", ""))
        return 0;
    unlink(path("status"));
    settings(r, &c);
    fx_env("CI_RUN_TEST_CUT", test->request);
    fx_run(f, args, path("tree"), NULL, NULL);
    calls = fx_read(path("calls"));
    p = calls;
    while (p && *p) {
        char set[128];
        next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        if (sscanf(p, "tests/run %127s", set) != 1 ||
            strlen(names) + strlen(set) + 2 >= sizeof(names)) {
            free(calls);
            return 0;
        }
        if (*names)
            strcat(names, " ");
        strcat(names, set);
        p = next;
    }
    free(calls);
    status = fx_exists(path("status")) ? fx_read(path("status")) : strdup("none");
    if (status) {
        size_t n = strlen(status);
        while (n && status[n - 1] == '\n')
            status[--n] = 0;
    }
    got = fx_read(output_path(r, "log"));
    ok = !strcmp(names, test->sets) && !fx_exists(output_path(r, "done")) && status &&
         !strcmp(status, test->wanted) && got && strstr(got, "ci: full: stopped on i686 x86_64\n");
    free(status);
    free(got);
    return fx_check(f, ok, test->name);
}

static int cut_rows(struct run_fixture *r)
{
    int ok = 1;
    r->suites = "";
    ok = cut_row(r, &(const struct cut_case){"killed set shells", "style step", "style", "143"}) &&
         ok;
    ok = cut_row(r, &(const struct cut_case){"killed set alone", "style self", "style", "143"}) &&
         fx_exists(output_path(r, "full-i686/log")) &&
         !fx_exists(output_path(r, "full-i686/done")) && ok;
    ok = cut_row(r,
                 &(const struct cut_case){"killed run parent", "style parent", "style", "none"}) &&
         ok;
    ok = cut_row(r, &(const struct cut_case){"killed build shell after clean", "make step", "",
                                             "143"}) &&
         ok;
    return ok;
}

static int directory_words(struct run_fixture *r, const char *directory, const char *wanted)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    char *names[64], got[8192] = "";
    unsigned n = 0, i, j;
    int ok = dir != NULL;
    if (!dir)
        return fx_check(r->f, 0, wanted);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        if (n == 64) {
            ok = 0;
            break;
        }
        names[n++] = strdup(entry->d_name);
    }
    closedir(dir);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char *swap = names[i];
                names[i] = names[j];
                names[j] = swap;
            }
    for (i = 0; i < n; i++) {
        if (strlen(got) + strlen(names[i]) + 2 >= sizeof(got))
            ok = 0;
        else {
            if (i)
                strcat(got, " ");
            strcat(got, names[i]);
        }
        free(names[i]);
    }
    return fx_check(r->f, ok && !strcmp(got, wanted), wanted);
}

static int both_kept(struct run_fixture *r)
{
    int ok = 1;
    ok = directory_words(r, output_path(r, "full-i686/kept"),
                         "tests.i686-x86_64.T.1.guest.default.boot.i686.r0 "
                         "tests.i686-x86_64.T.1.host.none.r0") &&
         ok;
    ok =
        directory_words(
            r, output_path(r, "full-x86_64/kept"),
            "tests.i686-x86_64.T.1.guest.fpu.xsave.x86_64.r0 tests.i686-x86_64.T.1.host.none.r0") &&
        ok;
    return ok;
}

static int both_rows(struct run_fixture *r)
{
    struct run_case c = {"both: every suite", "none", "", 0, 0, NULL, NULL, NULL};
    int ok = 1;
    r->suites = "";
    ok = row(r, &c) && ok;
    ok =
        words(r, path("calls"), "tests/run style host default shutdown tcp fpu tests/run bigmem") &&
        words(r, path("calls.arch"), "i686 x86_64 x86_64") && ok;
    ok = words(r, output_path(r, "full-i686/failed"), "") &&
         fx_exists(output_path(r, "full-i686/done")) &&
         !fx_exists(output_path(r, "full-i686/skipped")) && ok;
    ok = words(r, output_path(r, "full-x86_64/failed"), "") &&
         fx_exists(output_path(r, "full-x86_64/done")) && ok;
    c.name = "both: failed suite of each";
    c.set = "style";
    c.err =
        "run: i686:default: FAIL in 3s\nrun: x86_64:fpu: guest.fpu.xsave on x86_64: failure: it "
        "exited 1\nrun: x86_64:fpu: FAIL in 3s\nrun: result: failed: i686:default x86_64:fpu\n";
    c.status = 1;
    c.failed = "i686:default x86_64:fpu";
    ok = row(r, &c) && ok;
    ok = words(r, output_path(r, "full-i686/failed"), "default") &&
         words(r, output_path(r, "full-x86_64/failed"), "fpu") && ok;
    ok = fx_check(r->f,
                  !fx_has(output_path(r, "full-i686/err"), "x86_64:fpu: FAIL") &&
                      fx_has(output_path(r, "full-x86_64/err"), "x86_64:fpu: FAIL"),
                  "an architecture keeps only its own stderr") &&
         ok;
    return ok;
}

static int both_chosen_rows(struct run_fixture *r)
{
    struct run_case c = {"both: one build failed", "none", "x86_64:build", 0, 0, NULL, NULL, NULL};
    int ok = 1;
    r->suites = "";
    r->broken = "x86_64";
    ok = row(r, &c) && ok;
    r->broken = "";
    ok = words(r, path("calls"), "tests/run style host default shutdown tcp") &&
         words(r, path("calls.arch"), "i686") &&
         words(r, output_path(r, "full-x86_64/failed"), "build") &&
         words(r, output_path(r, "full-i686/failed"), "") && ok;
    c.name = "both: chosen suites of each";
    c.failed = "";
    r->suites = "i686:default x86_64:fpu";
    r->keep = "tests.i686-x86_64.T.1.guest.default.boot.i686.r0 "
              "tests.i686-x86_64.T.1.guest.fpu.xsave.x86_64.r0 tests.i686-x86_64.T.1.host.none.r0";
    ok = row(r, &c) && ok;
    r->keep = "";
    ok = words(r, path("calls"), "tests/run i686:default x86_64:fpu") && ok;
    ok = both_kept(r) && ok;
    c.name = "both: chosen suites of one";
    r->suites = "i686:default";
    ok = row(r, &c) && ok;
    ok = words(r, path("calls"), "tests/run i686:default") &&
         words(r, path("calls.arch"), "i686") && fx_exists(output_path(r, "full-x86_64/skipped")) &&
         !fx_exists(output_path(r, "full-i686/skipped")) && ok;
    r->suites = "";
    return ok;
}

static int prepare(struct fixture *f)
{
    fx_env("FLOW_CI_RUN", "1");
    fx_env("CI_RUN_WORK", f->work);
    if (!flow_copy(f, "tools/ci/run.sh", "tree/tools/ci/run.sh") || !fx_mock(f, "git") ||
        !fx_mock(f, "make") || !fx_mock(f, "ci-run-watch") ||
        !fx_copy(f->self, path("tree/tests/run")))
        return 0;
    return flow_put(f, "tree/tests/registry",
                    "style|-|full\nhost|-|full\ndefault|qemu|core\nshutdown|qemu|core\ntcp|qemu|"
                    "full\nfpu|qemu|core|||x86_64\n");
}

int flow_ci_run(struct fixture *f)
{
    struct run_fixture r;
    int ok = 1;
    memset(&r, 0, sizeof(r));
    r.f = f;
    r.arch = "i686 x86_64";
    r.suites = r.keep = r.broken = "";
    if (!prepare(f))
        return 0;
    ok = lines_rows(&r) && ok;
    ok = refused_rows(&r) && ok;
    ok = cut_rows(&r) && ok;
    ok = both_rows(&r) && ok;
    ok = both_chosen_rows(&r) && ok;
    return ok;
}
