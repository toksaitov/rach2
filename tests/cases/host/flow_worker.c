/* flow_worker.c: issue parking and intermittent hand-back in the real loop. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
struct issue {
    char *field[6];
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
static size_t load(struct issue rows[100], char **storage)
{
    char *line, *save = NULL;
    size_t n = 0;
    *storage = fx_read(env("WORKER_TEST_DATA"));
    for (line = strtok_r(*storage, "\n", &save); line && n < 100;
         line = strtok_r(NULL, "\n", &save)) {
        char *at = line;
        unsigned k;
        for (k = 0; k < 6; k++) {
            rows[n].field[k] = at;
            at = strchr(at, '|');
            if (!at) {
                if (k != 5)
                    return 0;
                break;
            }
            *at++ = 0;
        }
        n++;
    }
    return n;
}
static int tagged(const char *labels, const char *tag)
{
    char padded[500], wanted[100];
    snprintf(padded, sizeof(padded), ",%s,", labels);
    snprintf(wanted, sizeof(wanted), ",%s,", tag);
    return strstr(padded, wanted) != NULL;
}
static void save(struct issue rows[100], size_t n)
{
    FILE *f = fopen(env("WORKER_TEST_DATA"), "wb");
    size_t i;
    unsigned k;
    if (!f)
        return;
    for (i = 0; i < n; i++)
        for (k = 0; k < 6; k++)
            fprintf(f, "%s%c", rows[i].field[k], k == 5 ? '\n' : '|');
    fclose(f);
}
static int board_mock(int argc, char **argv, struct issue *rows, size_t n)
{
    size_t i;
    int result = 0;

    if (argc < 2)
        result = 2;
    else if (!strcmp(argv[1], "next")) {
        for (i = 0; i < n; i++)
            if (!strcmp(rows[i].field[1], "OPEN") && !strcmp(rows[i].field[2], "-"))
                printf("%s\t%s\n", rows[i].field[0], rows[i].field[4]);
    } else if (argc != 3)
        result = 2;
    else {
        for (i = 0; i < n && strcmp(rows[i].field[0], argv[2]); i++) {
        }
        if (i == n)
            result = 1;
        else if (!strcmp(argv[1], "take")) {
            if (strcmp(rows[i].field[1], "OPEN") || strcmp(rows[i].field[2], "-"))
                result = 1;
            else {
                rows[i].field[2] = "me";
                save(rows, n);
            }
        } else if (!strcmp(argv[1], "mine"))
            puts(!strcmp(rows[i].field[1], "OPEN") && !strcmp(rows[i].field[2], "me") ? "yes"
                                                                                      : "no");
        else
            result = 2;
    }
    return result;
}
static int timeline_mock(int argc, char **argv)
{
    char *events = fx_read(env("WORKER_TEST_EVENTS")), *line, *ss = NULL;
    const char *id = NULL;
    char number[50];
    unsigned k;
    for (k = 2; k < (unsigned)argc; k++) {
        const char *end = strstr(argv[k], "/timeline");
        if (end) {
            const char *p = end;
            while (p > argv[k] && p[-1] != '/')
                p--;
            snprintf(number, sizeof(number), "%.*s", (int)(end - p), p);
            id = number;
        }
    }
    for (line = strtok_r(events, "\n", &ss); line; line = strtok_r(NULL, "\n", &ss)) {
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL;
        if (a && b) {
            *a++ = 0;
            *b++ = 0;
            if (id && !strcmp(line, id))
                printf("%s %s\n", a, b);
        }
    }
    free(events);
    return 0;
}
static int issue_view(const struct issue *row, const char *log)
{
    int result = 0;
    int ci = tagged(row->field[3], "ci"),
        core = ci && (!strncmp(row->field[4], "Core tests fail on ", 19) ||
                      !strncmp(row->field[4], "Core tests did not finish on ", 29));
    if (strstr(log, "\"intermittent\""))
        puts(tagged(row->field[3], "intermittent") ? "true" : "false");
    else if (strstr(log, "--json labels "))
        puts(ci ? "true" : "false");
    else if (strstr(log, "--json title,labels,body ")) {
        if (core)
            puts("core");
        else if (ci && tagged(row->field[3], "group")) {
            const char *p = row->field[5];
            while ((p = strstr(p, "* #"))) {
                p += 3;
                printf("%ld\n", strtol(p, NULL, 10));
            }
        }
    } else if (strstr(log, "--json title,labels "))
        puts(core ? "true" : "false");
    else if (strstr(log, "--json state "))
        puts(row->field[1]);
    else
        result = 2;
    return result;
}
static int gh_mock(int argc, char **argv, struct issue *rows, size_t n, const char *log)
{
    size_t i;
    if (argc > 2 && !strcmp(argv[1], "issue") && !strcmp(argv[2], "list") &&
        strstr(log, "--label hold")) {
        for (i = 0; i < n; i++)
            if (!strcmp(rows[i].field[1], "OPEN") && tagged(rows[i].field[3], "hold")) {
                puts(rows[i].field[0]);
                break;
            }
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "api") && strstr(log, "/timeline "))
        return timeline_mock(argc, argv);
    if (argc > 3 && !strcmp(argv[1], "issue") && !strcmp(argv[2], "view")) {
        for (i = 0; i < n && strcmp(rows[i].field[0], argv[3]); i++) {
        }
        return i < n ? issue_view(&rows[i], log) : 0;
    }
    return 2;
}
static int claude_mock(int argc, char **argv, struct issue *rows, size_t n)
{
    size_t i;
    int result = 0;
    char log[10000];

    char number[50] = "", *head = fx_read(".git/HEAD"), *branch,
         *parked = fx_read(env("WORKER_TEST_STATE")), path[4096];
    const char *prompt = NULL;
    for (i = 1; i < (size_t)argc; i++)
        if (strstr(argv[i], "Work on issue "))
            prompt = strstr(argv[i], "Work on issue ") + 14;
    if (!prompt)
        result = 2;
    else {
        snprintf(number, sizeof(number), "%ld", strtol(prompt, NULL, 10));
        snprintf(path, sizeof(path), "%s/parked", env("WORKER_TEST_STATE"));
        free(parked);
        parked = fx_read(path);
        if (!*parked) {
            free(parked);
            parked = strdup("-");
        }
        parked[strcspn(parked, "\r\n")] = 0;
        branch = strrchr(head, '/');
        branch = branch ? branch + 1 : head;
        branch[strcspn(branch, "\r\n")] = 0;
        snprintf(log, sizeof(log), "%s %s %s\n", number, branch, parked);
        append(env("WORKER_TEST_SESSIONS"), log);
        for (i = 0; i < n; i++) {
            int flaky = tagged(rows[i].field[3], "intermittent");
            if (!strcmp(rows[i].field[0], number) && flaky) {
                rows[i].field[2] = "-";
                snprintf(log, sizeof(log), "%s|back|%lld\n", number, (long long)time(NULL));
                append(env("WORKER_TEST_EVENTS"), log);
            }
            if ((!strcmp(rows[i].field[0], number) && !flaky) || tagged(rows[i].field[3], "hold"))
                rows[i].field[1] = "CLOSED";
        }
        save(rows, n);
    }
    free(head);
    free(parked);
    return result;
}
static int sleep_mock(int argc, char **argv)
{
    int result = 0;
    char log[10000];

    char *s;
    unsigned count = 0;
    const char *p;
    snprintf(log, sizeof(log), "%s\n", argc > 1 ? argv[1] : "");
    append(env("WORKER_TEST_SLEEPS"), log);
    s = fx_read(env("WORKER_TEST_SLEEPS"));
    for (p = s; *p; p++)
        if (*p == '\n')
            count++;
    free(s);
    if (count >= (unsigned)atoi(env("WORKER_TEST_LAST"))) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/stop", env("WORKER_TEST_STATE"));
        if (!fx_text(path, ""))
            result = 1;
    }
    return result;
}
int flow_worker_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    char log[10000], *storage;
    struct issue rows[100];
    size_t n, i;
    int result = 0;
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
        result = gh_mock(argc, argv, rows, n, log);
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
    const char *name, *last, *data, *sessions;
};
static const struct worker_test tests[] = {
    {"park", "2",
     "60|OPEN|-|bug|A bug in the tests|-\n62|OPEN|-|ci|Release tests fail on i686: "
     "style|-\n61|OPEN|-|ci|Core tests fail on i686: default|-\n",
     "61 master 50\n50 master -\n"},
    {"group", "1",
     "65|OPEN|-|ci,group|Fix the style of the release tests|* #62\n62|OPEN|-|ci|Release tests "
     "fail on i686: style|-\n63|OPEN|-|ci,group|Make the test image boot again|* #66\\n* "
     "#64\n66|OPEN|-|ci|Release tests fail on i686: host|-\n64|OPEN|-|ci|Core tests fail on "
     "i686: default|-\n",
     "63 master 50\n"},
    {"sixth", "1",
     "70|OPEN|-|bug|a|-\n71|OPEN|-|bug|b|-\n72|OPEN|-|ci|Release tests fail on i686: "
     "c|-\n73|OPEN|-|bug|d|-\n74|OPEN|-|bug|e|-\n61|OPEN|-|ci|Core tests did not finish on "
     "i686|-\n",
     "61 master 50\n"},
    {"none", "1",
     "60|OPEN|-|bug|A bug in the tests|-\n62|OPEN|-|ci|Release tests fail on i686: style|-\n", ""},
    {"dirty", "1", "61|OPEN|-|ci|Core tests fail on i686: default|-\n", ""},
    {"rest", "2", "80|OPEN|-|ci,intermittent|Release tests fail on i686: default|-\n",
     "80 master -\n"},
    {"fresh", "2", "80|OPEN|-|ci,intermittent|Release tests fail on i686: default|-\n",
     "80 master -\n"},
    {"stale", "2", "80|OPEN|-|ci,intermittent|Release tests fail on i686: default|-\n",
     "80 master -\n"},
    {"plain", "2", "81|OPEN|-|ci|Release tests fail on i686: default|-\n", "81 master -\n"}};

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
    fx_text(target, "Work on issue @ISSUE@.\n");
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
static int worker_row(struct fixture *f, unsigned i)
{
    char cwd[4096], origin[4096], clone[4096], data[4000], events[200], source[4096], target[4096];
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    snprintf(clone, sizeof(clone), "%s/clone", f->work);

    const struct worker_test *t = &tests[i];
    time_t now = time(NULL);
    int handback = i >= 5, held;
    const char *a[6];
    fx_remove(clone);
    fx_remove(flow_path(f, "state"));
    fx_mkdir(flow_path(f, "state"));
    flow_put(f, "sessions", "");
    flow_put(f, "sleeps", "");
    flow_put(f, "calls", "");
    events[0] = 0;
    if (i == 5)
        snprintf(events, sizeof(events), "80|ci|%lld\n", (long long)now - 7200);
    if (i == 6)
        snprintf(events, sizeof(events), "80|back|%lld\n80|ci|%lld\n", (long long)now - 7200,
                 (long long)now - 3600);
    if (i == 7)
        snprintf(events, sizeof(events), "80|back|%lld\n", (long long)now - 172800);
    if (i == 8)
        snprintf(events, sizeof(events), "81|back|%lld\n", (long long)now - 60);
    flow_put(f, "events", events);
    snprintf(data, sizeof(data), "%s%s",
             handback ? ""
                      : "50|OPEN|me|bug|A fix to the tools|-\n77|OPEN|-|hold|Pushes are held: "
                        "master does not build, pass style or boot|-\n",
             t->data);
    flow_put(f, "data", data);
    snprintf(cwd, sizeof(cwd), "%s", f->work);
    G("clone", "-q", origin, clone);
    snprintf(cwd, sizeof(cwd), "%s", clone);
    if (!handback) {
        G("switch", "-q", "-c", "50-fix");
        snprintf(target, sizeof(target), "%s/fix.txt", clone);
        fx_text(target, "fix\n");
        G("add", "fix.txt");
        G("commit", "-q", "-m", "Fix the tool");
        flow_put(f, "state/issue", "50\n");
    }
    if (i == 4) {
        snprintf(target, sizeof(target), "%s/notes.txt", clone);
        fx_text(target, "notes\n");
    }
    fx_env("WORKER_TEST_DATA", flow_path(f, "data"));
    fx_env("WORKER_TEST_EVENTS", flow_path(f, "events"));
    fx_env("WORKER_TEST_CALLS", flow_path(f, "calls"));
    fx_env("WORKER_TEST_SESSIONS", flow_path(f, "sessions"));
    fx_env("WORKER_TEST_SLEEPS", flow_path(f, "sleeps"));
    fx_env("WORKER_TEST_STATE", flow_path(f, "state"));
    fx_env("WORKER_TEST_LAST", t->last);
    snprintf(source, sizeof(source), "%s/tools/flow/worker.sh", clone);
    a[0] = "/bin/sh";
    a[1] = source;
    a[2] = "-s";
    a[3] = getenv("WORKER_TEST_STATE");
    a[4] = NULL;
    held = !fx_run(f, a, clone, NULL, NULL) && fx_equal(flow_path(f, "sessions"), t->sessions);
    if (i == 0) {
        held = held && fx_has(flow_path(f, "state/loop.log"), " #50 parked for #61\n") &&
               fx_has(flow_path(f, "state/loop.log"), " #50 taken back from parking\n");
        {
            const char *g[] = {"rev-list", "-n", "1", "origin/master..50-fix", NULL};
            git(f, g, clone);
            held = held && *f->out;
        }
    }
    if (i == 3 || i == 4) {
        held =
            held && fx_equal(flow_path(f, "state/issue"), "50\n") &&
            !fx_exists(flow_path(f, "state/parked")) &&
            fx_has(flow_path(f, "state/loop.log"), " #50 is committed and waits for hold #77\n") &&
            !fx_has(flow_path(f, "calls"), "board take");
        {
            const char *g[] = {"rev-parse", "--abbrev-ref", "HEAD", NULL};
            git(f, g, clone);
            held = held && !strcmp(f->out, "50-fix\n");
        }
    }
    if (i >= 5 && i <= 7)
        held = held && fx_has(flow_path(f, "state/loop.log"),
                              " #80 rests: handed back with nothing new from ci\n");
    return fx_check(f, held, t->name);
}
int flow_worker(struct fixture *f)
{
    unsigned i;
    int ok = 1;
    if (!worker_setup(f))
        return 0;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        ok = worker_row(f, i) && ok;
    fx_env("FLOW_WORKER", NULL);
    return ok;
}
