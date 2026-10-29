/* flow_worker.c: the real worker loop's claims, resumes and hand-overs. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A fixture task is one line of WORKER_TEST_DATA: the number, the state, the
 * assignee or `-`, the milestone's branch or `-`, the title, and what its
 * session does: `close` closes it, and `limit` ends on the usage limit while
 * the manager gives the task to the account `zed`. */
enum { T_NUMBER, T_STATE, T_ASSIGNEE, T_BRANCH, T_TITLE, T_ACTION, T_FIELDS };
struct task {
    char *field[T_FIELDS];
};
static const char *env(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static void append(const char *path, const char *text)
{
    FILE *f = fopen(path, "ab");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
static size_t load(struct task rows[100], char **storage)
{
    char *line, *save = NULL;
    size_t n = 0;
    *storage = fx_read(env("WORKER_TEST_DATA"));
    if (!*storage)
        return 0;
    for (line = strtok_r(*storage, "\n", &save); line && n < 100;
         line = strtok_r(NULL, "\n", &save)) {
        char *at = line;
        unsigned k;
        for (k = 0; k < T_FIELDS; k++) {
            rows[n].field[k] = at;
            at = strchr(at, '|');
            if (!at) {
                if (k != T_FIELDS - 1)
                    return 0;
                break;
            }
            *at++ = 0;
        }
        n++;
    }
    return n;
}
static void save(struct task rows[100], size_t n)
{
    FILE *f = fopen(env("WORKER_TEST_DATA"), "wb");
    size_t i;
    unsigned k;
    if (!f)
        return;
    for (i = 0; i < n; i++)
        for (k = 0; k < T_FIELDS; k++)
            fprintf(f, "%s%c", rows[i].field[k], k == T_FIELDS - 1 ? '\n' : '|');
    fclose(f);
}
static struct task *find(struct task *rows, size_t n, const char *number)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(rows[i].field[T_NUMBER], number))
            return &rows[i];
    return NULL;
}
static int open_task(const struct task *t)
{
    return !strcmp(t->field[T_STATE], "OPEN");
}
static int board_mock(int argc, char **argv, struct task *rows, size_t n)
{
    struct task *t;
    size_t i;
    if (argc == 2 && !strcmp(argv[1], "next")) {
        for (i = 0; i < n; i++)
            if (open_task(&rows[i]) && !strcmp(rows[i].field[T_ASSIGNEE], "-"))
                printf("%s\t%s\n", rows[i].field[T_NUMBER], rows[i].field[T_TITLE]);
        return 0;
    }
    if (argc != 3)
        return 2;
    t = find(rows, n, argv[2]);
    if (!t)
        return 1;
    if (!strcmp(argv[1], "take")) {
        if (!open_task(t) || strcmp(t->field[T_ASSIGNEE], "-"))
            return 1;
        t->field[T_ASSIGNEE] = "me";
        save(rows, n);
        return 0;
    }
    if (!strcmp(argv[1], "mine")) {
        puts(open_task(t) && !strcmp(t->field[T_ASSIGNEE], "me") ? "yes" : "no");
        return 0;
    }
    if (!strcmp(argv[1], "branch")) {
        if (!strcmp(t->field[T_BRANCH], "-")) {
            fprintf(stderr, "board: #%s: it has no milestone, or its milestone names no branch\n",
                    argv[2]);
            return 1;
        }
        puts(t->field[T_BRANCH]);
        return 0;
    }
    return 2;
}
static int gh_mock(int argc, char **argv, struct task *rows, size_t n)
{
    struct task *t;
    if (argc < 4 || strcmp(argv[1], "issue") || strcmp(argv[2], "view"))
        return 2;
    t = find(rows, n, argv[3]);
    puts(t ? t->field[T_STATE] : "OPEN");
    return 0;
}
/* Record the task and branch the prompt names, and do the task's action. */
static int claude_mock(int argc, char **argv, struct task *rows, size_t n)
{
    char number[50], branch[200], line[300];
    const char *prompt = NULL;
    struct task *t;
    int i;
    for (i = 1; i < argc; i++)
        if (strstr(argv[i], "Work on issue "))
            prompt = strstr(argv[i], "Work on issue ");
    if (!prompt || sscanf(prompt, "Work on issue %49s on %199[^.].", number, branch) != 2)
        return 2;
    snprintf(line, sizeof(line), "%s %s\n", number, branch);
    append(env("WORKER_TEST_SESSIONS"), line);
    t = find(rows, n, number);
    if (t && !strcmp(t->field[T_ACTION], "close"))
        t->field[T_STATE] = "CLOSED";
    if (t && !strcmp(t->field[T_ACTION], "limit")) {
        t->field[T_ASSIGNEE] = "zed";
        puts("{\"type\":\"result\",\"is_error\":true,\"result\":\"You've hit your session "
             "limit, resets 3am\"}");
    }
    save(rows, n);
    return 0;
}
static int sleep_mock(int argc, char **argv)
{
    char line[100], path[4096], *s;
    unsigned count = 0;
    const char *p;
    snprintf(line, sizeof(line), "%s\n", argc > 1 ? argv[1] : "");
    append(env("WORKER_TEST_SLEEPS"), line);
    s = fx_read(env("WORKER_TEST_SLEEPS"));
    for (p = s; p && *p; p++)
        if (*p == '\n')
            count++;
    free(s);
    if (count >= (unsigned)atoi(env("WORKER_TEST_LAST"))) {
        snprintf(path, sizeof(path), "%s/stop", env("WORKER_TEST_STATE"));
        if (!fx_text(path, ""))
            return 1;
    }
    return 0;
}
int flow_worker_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    char log[10000], *storage;
    struct task rows[100];
    size_t n, i;
    int result;
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "sh")) {
        if (argc < 2 || !strstr(argv[1], "/board.sh")) {
            argv[0] = "/bin/sh";
            execv(argv[0], argv);
            return 127;
        }
        name = "board";
        argv++;
        argc--;
    }
    snprintf(log, sizeof(log), "%s", name);
    for (i = 1; i < (size_t)argc; i++) {
        strncat(log, " ", sizeof(log) - strlen(log) - 1);
        strncat(log, argv[i], sizeof(log) - strlen(log) - 1);
    }
    strncat(log, "\n", sizeof(log) - strlen(log) - 1);
    if (!strcmp(name, "board") || !strcmp(name, "gh"))
        append(env("WORKER_TEST_CALLS"), log);
    n = load(rows, &storage);
    if (!storage)
        return 2;
    if (!strcmp(name, "board"))
        result = board_mock(argc, argv, rows, n);
    else if (!strcmp(name, "gh"))
        result = gh_mock(argc, argv, rows, n);
    else if (!strcmp(name, "claude"))
        result = claude_mock(argc, argv, rows, n);
    else if (!strcmp(name, "sleep"))
        result = sleep_mock(argc, argv);
    else
        result = 97;
    free(storage);
    return result;
}

static int git(struct fixture *f, const char *const command[], const char *cwd)
{
    const char *args[40] = {"/usr/bin/git",
                            "-c",
                            "user.name=worker-test",
                            "-c",
                            "user.email=worker-test@example.invalid",
                            "-c",
                            "commit.gpgsign=false",
                            "-c",
                            "core.hooksPath=/dev/null",
                            NULL};
    unsigned n = 9, i;
    for (i = 0; command[i] && n < 39; i++)
        args[n++] = command[i];
    args[n] = NULL;
    return !fx_run(f, args, cwd, NULL, NULL);
}
#define G(...)                                 \
    do {                                       \
        const char *a[] = {__VA_ARGS__, NULL}; \
        if (!git(f, a, cwd))                   \
            return 0;                          \
    } while (0)
struct worker_test {
    const char *name, *last, *issue, *data, *sessions, *sleeps, *logged;
};
static const struct worker_test tests[] = {
    {"claim", "2", NULL, "70|OPEN|-|milestone/01-a|a task|close\n", "70 milestone/01-a\n",
     "20\n300\n", " took #70\n"},
    {"resume", "2", "50",
     "50|OPEN|me|milestone/02-b|a task begun|close\n70|OPEN|zed|milestone/01-a|taken|close\n",
     "50 milestone/02-b\n", "20\n300\n", " #50 is closed or no longer this account's\n"},
    {"lost", "2", "50",
     "50|OPEN|zed|milestone/01-a|given away|close\n70|OPEN|-|milestone/01-a|a task|close\n",
     "70 milestone/01-a\n", "20\n300\n", " #50 is closed or no longer this account's\n"},
    {"limit", "2", "50",
     "50|OPEN|me|milestone/01-a|a task begun|limit\n71|OPEN|-|milestone/01-a|another|close\n",
     "50 milestone/01-a\n71 milestone/01-a\n", "900\n20\n",
     " limit: hit your session limit, resets 3am\n"},
    {"no-branch", "1", NULL, "72|OPEN|-|-|a task of no milestone|close\n", "", "300\n",
     " #72: no usable milestone branch\n"}};

static int worker_setup(struct fixture *f)
{
    char cwd[4096], origin[4096], source[4096], target[4096];
    snprintf(cwd, sizeof(cwd), "%s/seed", f->work);
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    fx_mkdir(cwd);
    G("init", "-q", "--bare", "-b", "master", origin);
    G("init", "-q", "-b", "master");
    snprintf(source, sizeof(source), "%s/tools/flow", f->tree);
    snprintf(target, sizeof(target), "%s/tools/flow", cwd);
    if (!fx_tree(source, target))
        return 0;
    snprintf(target, sizeof(target), "%s/tools/flow/board.sh", cwd);
    if (!fx_text(target, "# board operations are native command fixtures\n"))
        return 0;
    snprintf(target, sizeof(target), "%s/tools/flow/worker.txt", cwd);
    fx_text(target, "Work on issue @ISSUE@ on @BRANCH@.\n");
    snprintf(target, sizeof(target), "%s/.gitignore", cwd);
    fx_text(target, "/build/\n");
    G("add", "-A");
    G("commit", "-q", "-m", "Add the flow scripts");
    G("push", "-q", origin, "master");
    if (!fx_mock(f, "sh") || !fx_mock(f, "gh") || !fx_mock(f, "claude") || !fx_mock(f, "sleep"))
        return 0;
    fx_env("FLOW_WORKER", "1");
    return 1;
}
static int worker_row(struct fixture *f, const struct worker_test *t)
{
    char cwd[4096], origin[4096], clone[4096], source[4096];
    const char *a[5];
    int held;
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    snprintf(clone, sizeof(clone), "%s/clone", f->work);
    fx_remove(clone);
    fx_remove(flow_path(f, "state"));
    fx_mkdir(flow_path(f, "state"));
    flow_put(f, "sessions", "");
    flow_put(f, "sleeps", "");
    flow_put(f, "calls", "");
    flow_put(f, "data", t->data);
    if (t->issue) {
        char text[64];
        snprintf(text, sizeof(text), "%s\n", t->issue);
        flow_put(f, "state/issue", text);
    }
    snprintf(cwd, sizeof(cwd), "%s", f->work);
    G("clone", "-q", origin, clone);
    fx_env("WORKER_TEST_DATA", flow_path(f, "data"));
    fx_env("WORKER_TEST_CALLS", flow_path(f, "calls"));
    fx_env("WORKER_TEST_SESSIONS", flow_path(f, "sessions"));
    fx_env("WORKER_TEST_SLEEPS", flow_path(f, "sleeps"));
    fx_env("WORKER_TEST_STATE", flow_path(f, "state"));
    fx_env("WORKER_TEST_LAST", t->last);
    snprintf(source, sizeof(source), "%s/tools/flow/worker.sh", clone);
    a[0] = "/bin/sh";
    a[1] = source;
    a[2] = "-s";
    a[3] = flow_path(f, "state");
    a[4] = NULL;
    held = !fx_run(f, a, clone, NULL, NULL) && fx_equal(flow_path(f, "sessions"), t->sessions) &&
           fx_equal(flow_path(f, "sleeps"), t->sleeps) &&
           fx_has(flow_path(f, "state/loop.log"), t->logged);
    if (t->issue)
        held = held && !fx_has(flow_path(f, "calls"), "board take 50");
    return fx_check(f, held, t->name);
}
int flow_worker(struct fixture *f)
{
    unsigned i;
    int ok = 1;
    if (!worker_setup(f))
        return 0;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        ok = worker_row(f, &tests[i]) && ok;
    fx_env("FLOW_WORKER", NULL);
    return ok;
}
