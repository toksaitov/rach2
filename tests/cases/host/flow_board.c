/* flow_board.c: the board product against a native, stateful GitHub fixture. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define BOARD_BASE  "1111111111111111111111111111111111111111"
#define BOARD_HEAD  "2222222222222222222222222222222222222222"
#define BOARD_LATER "3333333333333333333333333333333333333333"

static const char *env(const char *name)
{
    const char *s = getenv(name);
    return s != NULL ? s : "";
}

static const char *state_path(const char *name)
{
    static char paths[16][8192];
    static unsigned next;
    char *out = paths[next++ % 16];
    snprintf(out, 8192, "%s/%s", env("BOARD_TEST_WORK"), name);
    return out;
}

static int append_line(const char *path, const char *text)
{
    FILE *f = fopen(path, "a");
    int ok;
    if (!f)
        return 0;
    ok = fputs(text, f) >= 0 && fputc('\n', f) != EOF;
    return fclose(f) == 0 && ok;
}

static char *words(int argc, char **argv)
{
    size_t size = 1;
    int i;
    char *s;
    for (i = 1; i < argc; i++)
        size += strlen(argv[i]) + 1;
    s = calloc(1, size);
    if (s)
        for (i = 1; i < argc; i++) {
            if (i > 1)
                strcat(s, " ");
            strcat(s, argv[i]);
        }
    return s;
}
struct issue {
    char *field[12];
    unsigned fields;
};
struct board {
    struct issue issues[128];
    unsigned count;
};

static void board_free(struct board *b)
{
    unsigned i, j;
    for (i = 0; i < b->count; i++)
        for (j = 0; j < b->issues[i].fields; j++)
            free(b->issues[i].field[j]);
}

static int board_read(struct board *b)
{
    char *text = fx_read(env("BOARD_TEST_DATA")), *line, *next;
    memset(b, 0, sizeof(*b));
    if (!text)
        return 0;
    line = text;
    while (*line && b->count < 128) {
        struct issue *i = &b->issues[b->count++];
        char *field, *end;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        field = line;
        do {
            end = strchr(field, '|');
            if (end)
                *end++ = 0;
            i->field[i->fields++] = strdup(field);
            if (!i->field[i->fields - 1]) {
                free(text);
                return 0;
            }
            field = end;
        } while (field && i->fields < 12);
        if (!next)
            break;
        line = next;
    }
    free(text);
    return 1;
}

static const char *field(const struct issue *i, unsigned n)
{
    return n > 0 && n <= i->fields ? i->field[n - 1] : "";
}

static const char *clean_field(const struct issue *i, unsigned n)
{
    const char *s = field(i, n);
    return !strcmp(s, "-") ? "" : s;
}

static struct issue *issue(struct board *b, const char *n)
{
    unsigned i;
    for (i = 0; i < b->count; i++)
        if (!strcmp(field(&b->issues[i], 1), n))
            return &b->issues[i];
    return NULL;
}

static int board_write(struct board *b)
{
    unsigned i, j;
    FILE *f = fopen(env("BOARD_TEST_DATA"), "w");
    int ok = f != NULL;
    for (i = 0; ok && i < b->count; i++) {
        for (j = 0; j < b->issues[i].fields; j++) {
            if (j)
                fputc('|', f);
            fputs(b->issues[i].field[j], f);
        }
        ok = fputc('\n', f) != EOF;
    }
    return f != NULL && fclose(f) == 0 && ok;
}

static int change(struct board *b, const char *n, unsigned k, const char *value)
{
    struct issue *i = issue(b, n);
    if (!i)
        return 1;
    while (i->fields < k)
        i->field[i->fields++] = strdup("");
    free(i->field[k - 1]);
    i->field[k - 1] = strdup(value);
    return i->field[k - 1] && board_write(b);
}

static unsigned count_csv(const char *s)
{
    unsigned count = *s ? 1 : 0;
    for (; *s; s++)
        count += *s == ',';
    return count;
}

static void print_named(const char *body)
{
    unsigned count = 0;
    while ((body = strstr(body, "Part of #")) != NULL) {
        body += 9;
        if (*body < '0' || *body > '9')
            continue;
        if (count++)
            putchar(',');
        while (*body >= '0' && *body <= '9')
            putchar(*body++);
    }
}

static void translated(const char *s, char from, char to)
{
    for (; *s; s++)
        putchar(*s == from ? to : *s);
}

static int listing(struct board *b, const char *form)
{
    unsigned i, count = 0;
    int board = !strcmp(form, "board"), repo = !strcmp(form, "repo"),
        openrepo = !strcmp(form, "repo-open");
    const char *lost = board ? "board" : repo || openrepo ? "repo" : "issues";
    for (i = 0; i < b->count; i++) {
        const struct issue *r = &b->issues[i];
        int isopen = !strcmp(field(r, 2), "OPEN");
        if ((board || repo || openrepo) && !strcmp(field(r, 11), "archived"))
            continue;
        if (board && !strcmp(field(r, 12), "unlisted"))
            continue;
        if ((openrepo || !strcmp(form, "open")) && !isopen)
            continue;
        if (count++ == 2 && !strcmp(env("BOARD_TEST_LOST"), lost))
            return 1;
        if (board || repo || openrepo) {
            printf("I%s\t", field(r, 1));
            translated(clean_field(r, 3), '_', ' ');
            printf("\t%s\t%s\t%s\t%u\t%s\t%s\t", field(r, 1), field(r, 2), clean_field(r, 4),
                   count_csv(clean_field(r, 5)), clean_field(r, 7), clean_field(r, 6));
        } else
            printf("%s\t%s\t%s\t%s\t%s\t", field(r, 1), field(r, 2),
                   isopen ? "-" : "2026-10-01T00:00:00Z", clean_field(r, 4), clean_field(r, 6));
        print_named(clean_field(r, 8));
        printf("\t%s\n", clean_field(r, 9));
    }
    return 0;
}

static char *assignees(void)
{
    char *s, *p;
    if (fx_exists(state_path("assignees")))
        return fx_read(state_path("assignees"));
    s = strdup(env("BOARD_TEST_NOW"));
    if (s)
        for (p = s; *p; p++)
            if (*p == ',')
                *p = '\n';
    return s;
}

static int reassign(const char *list, int add, const char *login)
{
    char *copy = strdup(list), *p, *next;
    FILE *f = fopen(state_path("assignees"), "w");
    int found = 0, ok = copy && f;
    for (p = copy; ok && p && *p; p = next) {
        next = strpbrk(p, ",\n");
        if (next)
            *next++ = 0;
        if (!strcmp(p, login)) {
            found = 1;
            if (!add)
                continue;
        }
        if (*p)
            ok = fprintf(f, "%s\n", p) > 0;
    }
    if (ok && add && !found)
        ok = fprintf(f, "%s\n", login) > 0;
    free(copy);
    return f && fclose(f) == 0 && ok;
}
struct request {
    const char *query, *body, *item, *login, *n, *id;
};

static struct request request(int argc, char **argv)
{
    struct request r = {"", "", "", "", "", ""};
    int i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i - 1], "--body"))
            r.body = argv[i];
        if (!strcmp(argv[i - 1], "--id"))
            r.item = argv[i];
        if (!strcmp(argv[i - 1], "--add-assignee") || !strcmp(argv[i - 1], "--remove-assignee"))
            r.login = argv[i];
        if (!strcmp(argv[i - 1], "-f") || !strcmp(argv[i - 1], "-F")) {
            if (!strncmp(argv[i], "query=", 6))
                r.query = argv[i] + 6;
            if (!strncmp(argv[i], "sub_issue_id=", 13))
                r.id = argv[i] + 13;
            if (!strncmp(argv[i], "assignees[]=", 12))
                r.login = argv[i] + 12;
            if (!strncmp(argv[i], "n=", 2))
                r.n = argv[i] + 2;
        }
    }
    return r;
}

static int graphql(struct board *b, const struct request *r)
{
    const char *fail = env("BOARD_TEST_FAIL");
    if (strstr(r->query, "field(name: \"Status\")"))
        puts("project=project\nfield=field\nBacklog=backlog\nReady=ready\nIn_progress="
             "progress\nDone=done");
    else if (strstr(r->query, "projectItems(first: 10)")) {
        if (!strcmp(fail, "item"))
            return 1;
        if (strcmp(fail, "project") && strcmp(fail, "remove") && strcmp(fail, "close"))
            printf("I%s\n", r->n);
    } else if (strstr(r->query, "includeArchived: false"))
        return listing(b, strstr(r->query, "states: [OPEN]") ? "repo-open" : "repo");
    else if (strstr(r->query, "items(first: 100"))
        return listing(b, "board");
    else if (strstr(r->query, "states: [OPEN]"))
        return listing(b, "open");
    else if (strstr(r->query, "issues(first: 100"))
        return listing(b, "all");
    else
        return 2;
    return 0;
}

static int issue_view(struct board *b, int argc, char **argv, const char *all)
{
    const char *fail = env("BOARD_TEST_FAIL");
    struct issue *r = argc > 3 ? issue(b, argv[3]) : NULL;
    if (strstr(all, "--json state --jq .state"))
        puts(env("BOARD_TEST_STATE"));
    else if (strstr(all, "--json body --jq .body")) {
        const char *p = r ? clean_field(r, 8) : "";
        while (*p) {
            if (p[0] == '\\' && p[1] == 'n') {
                putchar('\n');
                p += 2;
            } else
                putchar(*p++);
        }
        putchar('\n');
    } else if (*env("BOARD_TEST_ME")) {
        if (strstr(all, ".assignees[].login")) {
            char *s = assignees();
            if (s) {
                puts(s);
                free(s);
            }
        } else
            puts("no");
    } else if (!strcmp(fail, "validate"))
        return 1;
    else if (fx_exists(state_path("grouped-2"))) {
        if (!strcmp(fail, "recheck"))
            puts("assigned");
        if (!strcmp(fail, "recheck-read"))
            return 1;
    }
    return 0;
}

static char *encoded(const char *text, int semicolon)
{
    size_t n = strlen(text), i, at = 0;
    char *s = malloc(n * 2 + 1);
    if (!s)
        return NULL;
    for (i = 0; i < n; i++) {
        if (text[i] == '\n') {
            s[at++] = semicolon ? ';' : '\\';
            if (!semicolon)
                s[at++] = 'n';
        } else
            s[at++] = text[i];
    }
    s[at] = 0;
    return s;
}

static int issue_edit(struct board *b, int argc, char **argv, const char *all,
                      const struct request *r)
{
    const char *fail = env("BOARD_TEST_FAIL"), *n = argv[3];
    char name[80];
    if (strstr(all, " --body ")) {
        char *s = encoded(r->body, 0);
        int ok = s && change(b, n, 8, s);
        free(s);
        return !ok;
    }
    if (strstr(all, "--add-label grouped")) {
        snprintf(name, sizeof(name), "label%s", n);
        if (!strcmp(fail, name))
            return 1;
        if (!strcmp(n, "2") && !strcmp(fail, "term")) {
            kill(getppid(), SIGTERM);
            return 1;
        }
        snprintf(name, sizeof(name), "grouped-%s", n);
        if (!fx_text(state_path(name), ""))
            return 1;
        snprintf(name, sizeof(name), "applied%s", n);
        return !strcmp(fail, name);
    }
    if (strstr(all, "--remove-label grouped")) {
        if (!strcmp(fail, "remove") && !strcmp(n, "1"))
            return 1;
        snprintf(name, sizeof(name), "grouped-%s", n);
        unlink(state_path(name));
        if (strstr(all, " --add-label ")) {
            struct issue *i = issue(b, n);
            char labels[4096];
            snprintf(labels, sizeof(labels), "%s,%s", i ? field(i, 4) : "", argv[argc - 1]);
            return !change(b, n, 4, labels);
        }
        return 0;
    }
    if (strstr(all, "--add-assignee ") || strstr(all, "--remove-assignee ")) {
        char *s = assignees();
        int ok = reassign(*env("BOARD_TEST_STALE") ? env("BOARD_TEST_STALE")
                          : s                      ? s
                                                   : "",
                          strstr(all, "--add-assignee ") != NULL, r->login);
        free(s);
        return !ok;
    }
    return 2;
}

static int paginated(struct board *b, const char *endpoint)
{
    const char *last = strrchr(endpoint, '/');
    if (last && !strcmp(last, "/timeline"))
        translated(env("BOARD_TEST_TIMELINE"), ',', '\n');
    else if (last && !strcmp(last, "/comments")) {
        char n[64];
        const char *before = last;
        struct issue *i;
        while (before > endpoint && before[-1] != '/')
            before--;
        snprintf(n, sizeof(n), "%.*s", (int)(last - before), before);
        i = issue(b, n);
        if (i)
            translated(clean_field(i, 10), ';', '\n');
    } else
        return 2;
    putchar('\n');

    return 0;
}

static int api(struct board *b, int argc, char **argv, const struct request *r)
{
    const char *target = argc > 2 ? argv[2] : "", *fail = env("BOARD_TEST_FAIL");
    if (!strcmp(target, "user"))
        puts(env("BOARD_TEST_ME"));
    else if (!strcmp(target, "graphql"))
        return graphql(b, r);
    else if (!strcmp(target, "--paginate") && argc > 3)
        return paginated(b, argv[3]);
    else if (!strcmp(target, "-X") && argc > 4) {
        const char *url = argv[4];
        if (strstr(url, "/assignees")) {
            char *s = assignees();
            int ok = reassign(s ? s : "", !strcmp(argv[3], "POST"), r->login);
            free(s);
            return !ok;
        }
        if (!strcmp(env("BOARD_TEST_FULL"), "yes")) {
            fputs("gh: the parent has the most sub-issues it may have (HTTP 422)\n", stderr);
            return 1;
        }
        {
            char n[64], parent[64];
            const char *last = strrchr(url, '/'), *p = last;
            snprintf(n, sizeof(n), "%ld", strtol(r->id, NULL, 10) - 1000);
            while (p > url && p[-1] != '/')
                p--;
            /* The URL ends /issues/P/sub_issues. */
            if (last) {
                p = last;
                while (p > url && p[-1] != '/')
                    p--;
                snprintf(parent, sizeof(parent), "%.*s", (int)(last - p), p);
            } else
                parent[0] = 0;
            return !change(b, n, 6, parent);
        }
    } else if (strstr(target, "/commits/master"))
        puts(env("BOARD_TEST_HEAD"));
    else if (strstr(target, "/status"))
        puts(env("BOARD_TEST_CORE"));
    else if (strstr(target, "/issues/")) {
        const char *last = strrchr(target, '/');
        printf("%ld\n", 1000 + strtol(last ? last + 1 : "0", NULL, 10));
    } else {
        (void)fail;
        return 2;
    }
    return 0;
}

static int project(struct board *b, char **argv, const char *all, const struct request *r)
{
    const char *fail = env("BOARD_TEST_FAIL");
    if (!strcmp(argv[2], "item-add")) {
        if (!strcmp(fail, "project") || !strcmp(fail, "remove") || !strcmp(fail, "close"))
            return 1;
        if (!strcmp(fail, "added")) {
            fputs("GraphQL: Content already exists in this project\n", stderr);
            return 1;
        }
    } else if (!strcmp(argv[2], "item-edit")) {
        const char *p = strstr(all, "--single-select-option-id "), *status;
        if (!strstr(all, "--id I") || !strcmp(fail, "status") || !p)
            return 1;
        p += 26;
        if (!fx_text(state_path("status"), p))
            return 1;
        status = !strcmp(p, "backlog")    ? "Backlog"
                 : !strcmp(p, "ready")    ? "Ready"
                 : !strcmp(p, "progress") ? "In_progress"
                                          : "Done";
        return !change(b, r->item[0] == 'I' ? r->item + 1 : r->item, 3, status);
    } else if (!strcmp(argv[2], "item-archive")) {
        char failure[80];
        snprintf(failure, sizeof(failure), "archive%s", r->item[0] == 'I' ? r->item + 1 : r->item);
        if (!strcmp(fail, failure))
            return 1;
        return !change(b, r->item[0] == 'I' ? r->item + 1 : r->item, 11, "archived");
    } else
        return 2;
    return 0;
}

int flow_board_mock(int argc, char **argv)
{
    struct board b;
    struct request r = request(argc, argv);
    char *all = words(argc, argv);
    int status = 2;
    if (argc < 3 || !all || !board_read(&b)) {
        free(all);
        return 2;
    }
    if (!append_line(state_path("calls"), all)) {
        board_free(&b);
        free(all);
        return 2;
    }
    if (!strcmp(argv[1], "api"))
        status = api(&b, argc, argv, &r);
    else if (!strcmp(argv[1], "project"))
        status = project(&b, argv, all, &r);
    else if (!strcmp(argv[1], "issue")) {
        if (!strcmp(argv[2], "view"))
            status = issue_view(&b, argc, argv, all);
        else if (!strcmp(argv[2], "edit") && argc > 3)
            status = issue_edit(&b, argc, argv, all, &r);
        else if (!strcmp(argv[2], "create")) {
            status = !strcmp(env("BOARD_TEST_FAIL"), "create") ? 1 : 0;
            if (!status) {
                status =
                    !(fx_text(state_path("body"), all) && fx_text(state_path("group"), "open\n"));
                puts("https://example.invalid/issues/99");
            }
        } else if (!strcmp(argv[2], "close")) {
            status = !strcmp(env("BOARD_TEST_FAIL"), "close")
                         ? 1
                         : !fx_text(state_path("group"), "closed\n");
        } else if (!strcmp(argv[2], "comment") && argc > 3) {
            struct issue *i = issue(&b, argv[3]);
            char *body = encoded(r.body, 1), combined[16384];
            const char *old = i ? clean_field(i, 10) : "";
            snprintf(combined, sizeof(combined), "%s%s%s", old, *old ? ";" : "", body ? body : "");
            status = body ? !change(&b, argv[3], 10, combined) : 1;
            free(body);
        }
    }
    board_free(&b);
    free(all);
    return status;
}

static const char board_only[] = "25|OPEN|Ready|bug,owner-only|-|7|0|-|owner-only member\n";

static const char board_old[] = "1|OPEN|Ready|ci|-|-|0|-|ci: one\n"
                                "2|OPEN|Ready|ci|-|-|0|-|ci: two\n"
                                "7|OPEN|Ready|ci|-|-|0|-|ci: seven\n";

static const char board_main[] =
    "7|OPEN|In_progress|phase|-|-|0|-|phase: seven\n"
    "8|OPEN|Ready|phase|-|-|0|-|phase: eight\n"
    "10|OPEN|Ready|bug|-|7|0|-|linked member\n"
    "11|OPEN|Ready|bug|-|-|0|Found in review. Part of #7.|named member\n"
    "12|OPEN|Ready|bug|-|-|0|Part of #73.|member of 73\n"
    "13|OPEN|Ready|bug|-|8|0|-|member of eight\n"
    "14|OPEN|Backlog|ci|-|-|0|-|ci: core fails\n"
    "15|OPEN|Ready|ci,intermittent|-|-|0|-|ci: night fails once\n"
    "16|OPEN|Ready|bug,owner|-|7|0|-|owner member\n"
    "17|OPEN|Ready|bug|-|7|1|-|blocked member\n"
    "18|OPEN|Ready|bug|alpha|7|0|-|assigned member\n"
    "19|CLOSED|Done|bug|-|7|0|-|closed member\n"
    "20|CLOSED|Done|bug|-|8|0|-|closed member of eight\n"
    "21|CLOSED|Done|bug|-|-|0|-|closed issue of no phase\n"
    "22|OPEN|Ready|ci|-|-|0|-|ci: style fails\n"
    "23|OPEN|Ready|bug,grouped|-|7|0|-|grouped member\n"
    "24|OPEN|Ready|phase|-|7|0|-|phase under seven\n";

static const char board_quiet[] = "7|OPEN|In_progress|phase|-|-|0|-|phase: seven\n"
                                  "14|OPEN|Backlog|ci|-|-|0|-|ci: core fails\n"
                                  "15|OPEN|Ready|ci,intermittent|-|-|0|-|ci: night fails once\n"
                                  "19|CLOSED|Done|bug|-|7|0|-|closed member\n";

static const char board_begin[] =
    "5|OPEN|Ready|phase|-|-|0|-|phase: five\n"
    "6|OPEN|Ready|phase|-|-|0|-|phase: six\n"
    "30|OPEN|Backlog|bug|-|5|0|-|backlog member\n"
    "31|OPEN|-|bug|-|-|0|Part of #5.|member with no Status\n"
    "32|OPEN|In_progress|bug|alpha|5|0|-|member in progress\n"
    "33|OPEN|Backlog|bug,owner|-|5|0|-|owner member\n"
    "34|OPEN|Backlog|bug,triaged|-|-|0|Seen in a log.|triaged orphan\n"
    "35|OPEN|Backlog|ci,triaged|-|-|0|-|ci: triaged\n"
    "36|OPEN|Backlog|bug|-|-|0|-|untriaged orphan\n"
    "37|OPEN|Backlog|bug,triaged|-|6|0|-|member of six\n"
    "38|OPEN|Backlog|bug,grouped|-|5|0|-|grouped member\n"
    "39|OPEN|Ready|bug|-|5|0|-|ready member\n"
    "40|OPEN|Backlog|day-report,triaged|-|-|0|-|day report\n"
    "41|OPEN|Backlog|incident,triaged|-|-|0|-|incident\n"
    "42|OPEN|Backlog|hold,triaged|-|-|0|-|hold\n";

static const char board_review[] =
    "7|OPEN|In_progress|phase|-|-|0|-|phase: seven|phase: started at "
    "1111111111111111111111111111111111111111;Looks right.;review: "
    "1111111111111111111111111111111111111111..2222222222222222222222222222222222222222 "
    "complete;phase: started at 3333333333333333333333333333333333333333\n"
    "10|CLOSED|Done|bug|-|7|0|-|done member\n"
    "11|CLOSED|Done|bug|-|-|0|Part of #7.|named done member\n"
    "12|CLOSED|Done|ci|-|-|0|-|ci: fixed\n"
    "13|OPEN|Ready|bug|-|-|0|-|issue of no phase\n";

static int use(struct fixture *f, const char *data)
{
    static const char *const names[] = {"FAIL", "LOST",  "FULL",     "CORE",  "ME",
                                        "NOW",  "STALE", "TIMELINE", "STATE", NULL};
    static const char *const values[] = {"none", "", "no", "success", "", "", "", "", "OPEN"};
    unsigned i;
    char name[80];
    for (i = 0; names[i]; i++) {
        snprintf(name, sizeof(name), "BOARD_TEST_%s", names[i]);
        fx_env(name, values[i]);
    }
    fx_env("BOARD_TEST_HEAD", BOARD_HEAD);
    return flow_put(f, "data", data);
}

static int run(struct fixture *f, const char *const args[])
{
    const char *argv[16] = {"/bin/sh", flow_path(f, "board.sh")};
    unsigned i;
    if (!fx_remove(flow_path(f, "state")) || !fx_mkdir(flow_path(f, "state")))
        return 0;
    for (i = 0; args[i] && i < 13; i++)
        argv[i + 2] = args[i];
    argv[i + 2] = NULL;
    return fx_run(f, argv, f->work, NULL, NULL);
}

static int call(struct fixture *f, const char *a, const char *b, const char *c, const char *d)
{
    const char *args[] = {a, b, c, d, NULL};
    return run(f, args);
}

static char *data_field(const char *n, unsigned k)
{
    struct board b;
    struct issue *i;
    char *s;
    if (!board_read(&b))
        return NULL;
    i = issue(&b, n);
    s = strdup(i ? field(i, k) : "");
    board_free(&b);
    return s;
}

static int is_field(const char *n, unsigned k, const char *wanted)
{
    char *s = data_field(n, k);
    int ok = s && !strcmp(s, wanted);
    free(s);
    return ok;
}

static int phase_record(const char *n, const char *wanted)
{
    char *s = data_field(n, 10), *p = s, *end;
    unsigned found = 0;
    int ok = s != NULL;
    while (p && *p) {
        end = strchr(p, ';');
        if (end)
            *end++ = 0;
        if (!strncmp(p, "phase: ", 7)) {
            found++;
            ok = ok && !strcmp(p, wanted);
        }
        p = end;
    }
    free(s);
    return ok && found == 1;
}

static int calls(const char *text)
{
    return fx_has(state_path("calls"), text);
}

static int left(const char *wanted)
{
    char *s = assignees(), *p;
    size_t n;
    int ok;
    if (!s)
        return 0;
    n = strlen(s);
    while (n && s[n - 1] == '\n')
        s[--n] = 0;
    for (p = s; *p; p++)
        if (*p == '\n')
            *p = ',';
    ok = !strcmp(s, wanted);
    free(s);
    return ok;
}

static int output_words(struct fixture *f, const char *wanted)
{
    const char *p = f->out;
    char out[2048] = "";
    size_t n = 0;
    while (p && *p) {
        const char *end = p + strcspn(p, "\t\n");
        while (p < end) {
            const char *word;
            size_t len;
            while (p < end && *p == ' ')
                p++;
            word = p;
            while (p < end && *p != ' ')
                p++;
            len = (size_t)(p - word);
            if (!len)
                break;
            if (n + len + 2 >= sizeof(out))
                return 0;
            if (n)
                out[n++] = ' ';
            memcpy(out + n, word, len);
            n += len;
        }
        out[n] = 0;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return !strcmp(out, wanted);
}

static int first_words(struct fixture *f, const char *wanted)
{
    return f->status == 0 && output_words(f, wanted);
}

static int said(struct fixture *f, const char *line)
{
    const char *p = f->out;
    size_t n = strlen(line);
    while (p && *p) {
        if (!strncmp(p, line, n) && (p[n] == '\n' || p[n] == 0))
            return 1;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static int tail(struct fixture *f, const char *line)
{
    const char *p = f->out, *last = p;
    size_t n;
    if (!p)
        return 0;
    while ((p = strchr(p, '\n')) != NULL) {
        if (p[1])
            last = p + 1;
        p++;
    }
    n = strcspn(last, "\n");
    return strlen(line) == n && !memcmp(last, line, n);
}

static int setup_data(struct fixture *f, const char *base, const char *extra)
{
    char *s = malloc(strlen(base) + strlen(extra) + 1);
    int ok;
    if (!s)
        return 0;
    strcpy(s, base);
    strcat(s, extra);
    ok = use(f, s);
    free(s);
    return ok;
}

static int alter(const char *n, unsigned k, const char *text)
{
    struct board b;
    int ok;
    if (!board_read(&b))
        return 0;
    ok = change(&b, n, k, text);
    board_free(&b);
    return ok;
}

static int group_success(struct fixture *f)
{
    return !f->status && !strcmp(f->out, "99\n") && !*f->err &&
           fx_equal(state_path("group"), "open\n") && fx_equal(state_path("status"), "ready") &&
           fx_exists(state_path("grouped-1")) && fx_exists(state_path("grouped-2")) &&
           fx_has(state_path("body"), "\n* #1\n") && fx_has(state_path("body"), "\n* #2") &&
           !calls("issue close") && !calls("remove-label");
}

static int group_partial(struct fixture *f, const char *failure)
{
    if (!strcmp(failure, "remove"))
        return fx_exists(state_path("grouped-1")) && fx_equal(state_path("group"), "open\n") &&
               strstr(f->err, "issues/99: could not remove grouped from #1") &&
               strstr(f->err, "issues/99: incomplete group kept open") && !calls("issue close");
    if (!strcmp(failure, "close"))
        return !fx_exists(state_path("grouped-1")) && fx_equal(state_path("group"), "open\n") &&
               strstr(f->err, "issues/99: could not close the incomplete group");
    return !fx_exists(state_path("grouped-1")) && !fx_exists(state_path("grouped-2")) &&
           fx_equal(state_path("group"), "closed\n");
}

static int group_row(struct fixture *f, const char *failure)
{
    static const char *const gone[] = {"create", "label1",  "label2",      "applied2",
                                       "term",   "recheck", "recheck-read"};
    unsigned i;
    int ok;
    if (!use(f, board_old))
        return 0;
    fx_env("BOARD_TEST_FAIL", failure);
    call(f, "group", "fixtures", "1", "2");
    if (!strcmp(failure, "success") || !strcmp(failure, "added"))
        return fx_check(f, group_success(f), failure);
    ok = f->status && !*f->out;
    if (!strcmp(failure, "validate"))
        return fx_check(f,
                        ok && !fx_exists(state_path("group")) && !calls("issue edit") &&
                            !calls("issue close") && !calls("project item-"),
                        failure);
    for (i = 0; i < sizeof(gone) / sizeof(gone[0]); i++)
        if (!strcmp(failure, gone[i]))
            return fx_check(f,
                            ok && !fx_exists(state_path("grouped-1")) &&
                                !fx_exists(state_path("grouped-2")) &&
                                !fx_exists(state_path("group")),
                            failure);
    return fx_check(f, ok && group_partial(f, failure), failure);
}
struct take_row {
    const char *name, *me, *now, *timeline, *stale, *after;
    int keep;
};

static int take_row(struct fixture *f, const struct take_row *r)
{
    int ok;
    if (!use(f, board_old))
        return 0;
    fx_env("BOARD_TEST_ME", r->me);
    fx_env("BOARD_TEST_NOW", r->now);
    fx_env("BOARD_TEST_TIMELINE", r->timeline);
    fx_env("BOARD_TEST_STALE", r->stale);
    call(f, "take", "7", NULL, NULL);
    ok = r->keep ? !f->status && fx_exists(state_path("status")) && left(r->after)
                 : f->status && !fx_exists(state_path("status")) && left(r->after) &&
                       strstr(f->err, "board: #7: taken by zed first\n");
    return fx_check(f, ok, r->name);
}

static int state_row(struct fixture *f, const char *name)
{
    int closed = strstr(name, "-closed") != NULL, take = !strncmp(name, "take", 4),
        only = !strcmp(name, "release-only");
    int ok;
    if (!use(f, board_old))
        return 0;
    fx_env("BOARD_TEST_ME", "alpha");
    fx_env("BOARD_TEST_NOW", "alpha");
    fx_env("BOARD_TEST_TIMELINE", "alpha");
    fx_env("BOARD_TEST_STALE", "alpha,zed");
    fx_env("BOARD_TEST_STATE", closed ? "CLOSED" : "OPEN");
    call(f, take ? "take" : "release", "7", only ? "owner-only" : NULL, NULL);
    if (closed)
        ok = f->status == (take ? 1 : 0) && fx_equal(state_path("status"), "done") &&
             !calls("issue edit") && (!take || strstr(f->err, "board: #7: closed: set Done\n"));
    else
        ok = !f->status && fx_equal(state_path("status"), "ready") && left("") &&
             (!only || is_field("7", 4, "ci,owner-only"));
    return fx_check(f, ok, name);
}

struct simple_row {
    const char *name, *data, *command, *arg, *extra, *words;
};

static int select_prepare(struct fixture *f, const struct simple_row *r)
{
    if (!setup_data(f, r->data,
                    r->extra && (!strcmp(r->extra, "only") || !strcmp(r->extra, "owner"))
                        ? board_only
                        : ""))
        return 0;
    if (r->extra && strchr(r->extra, ':')) {
        char n[32], value[80];
        unsigned k;
        if (sscanf(r->extra, "%31[^:]:%u:%79s", n, &k, value) != 3 || !alter(n, k, value))
            return 0;
    }
    if (r->extra && (!strcmp(r->extra, "unlisted") || !strcmp(r->extra, "repo"))) {
        if (!alter("10", 12, "unlisted") || !alter("14", 12, "unlisted"))
            return 0;
    }
    if (r->extra && !strcmp(r->extra, "owner"))
        fx_env("BOARD_TEST_ME", "toksaitov");
    if (r->extra &&
        (!strcmp(r->extra, "issues") || !strcmp(r->extra, "board") || !strcmp(r->extra, "repo")))
        fx_env("BOARD_TEST_LOST", r->extra);
    return 1;
}

static int select_rows(struct fixture *f)
{
    static const struct simple_row rows[] = {
        {"current-none", board_main, "current", NULL, "7:3:Ready", ""},
        {"current-one", board_main, "current", NULL, NULL, "7"},
        {"current-two", board_main, "current", NULL, "8:3:In_progress", NULL},
        {"members-all", board_main, "members", "7", NULL, "10 11 16 17 18 19 23 24"},
        {"members-open", board_main, "members", "7", "open", "10 11 16 17 18 23 24"},
        {"members-lost", board_main, "members", "7", "issues", NULL},
        {"next-tiers", board_main, "next", NULL, NULL, "14 22 10 11 15"},
        {"next-only", board_main, "next", NULL, "only", "14 22 10 11 15"},
        {"next-owner", board_main, "next", NULL, "owner", "14 22 10 11 25 15"},
        {"next-idle", board_main, "next", NULL, "7:3:Ready", "14 22"},
        {"next-done", board_quiet, "next", NULL, NULL, "14"},
        {"next-lost", board_main, "next", NULL, "board", NULL},
        {"next-unlisted", board_main, "next", NULL, "unlisted", "22 14 11 10 15"},
        {"next-unlisted-lost", board_main, "next", NULL, "repo", NULL}};
    unsigned i;
    int ok = 1;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const struct simple_row *r = &rows[i];
        int held;
        if (!select_prepare(f, r))
            return 0;
        call(f, r->command, r->arg, r->extra && !strcmp(r->extra, "open") ? "open" : NULL, NULL);
        held = r->words ? first_words(f, r->words) : f->status && !*f->out && *f->err;
        if (!strcmp(r->name, "current-two"))
            held = held && strstr(f->err, "#7 #8 are In progress");
        if (!strcmp(r->name, "members-all"))
            held = held && said(f, "11\tOPEN\t-\tbug\tnamed member") &&
                   said(f, "19\tCLOSED\t2026-10-01T00:00:00Z\tbug\tclosed member");
        if (!strcmp(r->name, "next-tiers"))
            held = held && said(f, "14\tci: core fails");
        ok = fx_check(f, held, r->name) && ok;
    }
    return ok;
}

static void identity(struct fixture *f, const char *who)
{
    fx_env("BOARD_TEST_ME", who);
    fx_env("BOARD_TEST_NOW", who);
    fx_env("BOARD_TEST_TIMELINE", who);
    (void)f;
}

static int take_phase_rows(struct fixture *f)
{
    static const struct {
        const char *name, *n, *why;
    } rows[] = {{"take-unlisted", "10", NULL},
                {"take-member", "10", NULL},
                {"take-ci", "14", NULL},
                {"take-owner-only", "25", NULL},
                {"take-stranger", "13", "neither labelled ci nor a member of #7"},
                {"take-blocked", "17", "it has an open blocker"},
                {"take-owner", "16", "it waits for the owner"},
                {"take-only", "25", "only the owner's accounts can do its work"},
                {"take-phase", "8", "it is a phase"},
                {"take-backlog", "10", "its Status is Backlog, not Ready"}};
    unsigned i;
    int ok = 1;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int held;
        if (!setup_data(f, board_main, board_only))
            return 0;
        identity(f, !strcmp(rows[i].name, "take-owner-only") ? "toksaitov" : "alpha");
        if (rows[i].why && !alter("10", 3, "Backlog"))
            return 0;
        if (!strcmp(rows[i].name, "take-unlisted") && !alter("10", 12, "unlisted"))
            return 0;
        call(f, "take", rows[i].n, NULL, NULL);
        held = rows[i].why ? f->status && strstr(f->err, rows[i].why) && !calls("issue edit") &&
                                 !calls("item-edit")
                           : !f->status && is_field(rows[i].n, 3, "In_progress");
        ok = fx_check(f, held, rows[i].name) && ok;
    }
    return ok;
}

static int group_phase_rows(struct fixture *f)
{
    int ok = 1;
    if (!use(f, board_main))
        return 0;
    call(f, "group", "fixtures", "13", NULL);
    ok = fx_check(f, f->status && !*f->out && !calls("issue edit") && !calls("issue create"),
                  "group-stranger") &&
         ok;
    if (!setup_data(f, board_main, board_only))
        return 0;
    call(f, "group", "fixtures", "10", "25");
    ok = fx_check(f,
                  f->status && !*f->out && strstr(f->err, "board: #25 is labelled owner-only") &&
                      !calls("issue edit") && !calls("issue create"),
                  "group-only") &&
         ok;
    if (!use(f, board_main))
        return 0;
    call(f, "group", "fixtures", "14", "10");
    ok = fx_check(f,
                  !f->status && !strcmp(f->out, "99\n") &&
                      fx_has(state_path("body"), "--label group,ci ") &&
                      fx_has(state_path("body"), "\n* #14\n") &&
                      fx_has(state_path("body"), "\n* #10\n") &&
                      fx_has(state_path("body"), "\nPart of #7.") &&
                      calls("issues/7/sub_issues -F sub_issue_id=1099\n"),
                  "group-ci") &&
         ok;
    return ok;
}

static int archive_prepare(struct fixture *f, unsigned i)
{
    if (!use(f, board_main))
        return 0;
    if (i == 1 && !alter("7", 2, "CLOSED"))
        return 0;
    if (i == 1 && !alter("7", 3, "Done"))
        return 0;
    if (i == 2)
        fx_env("BOARD_TEST_FAIL", "archive19");
    if (i == 3 && !alter("21", 12, "unlisted"))
        return 0;
    if (i == 4 && (!alter("19", 3, "Ready") || !alter("19", 12, "unlisted")))
        return 0;
    return 1;
}

static int archive_rows(struct fixture *f)
{
    const char *names[] = {"archive-other", "archive-closed", "archive-fail", "archive-unlisted",
                           "done-unlisted"};
    unsigned i;
    int ok = 1;
    for (i = 0; i < 5; i++) {
        int held;
        if (!archive_prepare(f, i))
            return 0;
        call(f, i == 4 ? "done" : "archive", i == 4 ? NULL : "7", NULL, NULL);
        if (i == 2)
            held = f->status && strstr(f->err, "board: #19: cannot archive its card\n") && f->out &&
                   output_words(f, "21");
        else
            held = first_words(f, i == 1 ? "7 19 21" : i == 4 ? "19" : "19 21");
        if (i == 0)
            held = held && is_field("19", 11, "archived") && is_field("21", 11, "archived") &&
                   is_field("20", 11, "");
        if (i == 3)
            held = held && is_field("21", 11, "archived");
        if (i == 4)
            held = held && is_field("19", 3, "Done");
        ok = fx_check(f, held, names[i]) && ok;
    }
    return ok;
}

static int statuses(const char *wanted)
{
    struct board b;
    char text[4096] = "";
    unsigned i;
    int ok;
    if (!board_read(&b))
        return 0;
    for (i = 0; i < b.count; i++) {
        size_t used = strlen(text);
        if (snprintf(text + used, sizeof(text) - used, "%s%s:%s", i ? " " : "",
                     field(&b.issues[i], 1),
                     field(&b.issues[i], 3)) >= (int)(sizeof(text) - used)) {
            board_free(&b);
            return 0;
        }
    }
    ok = !strcmp(text, wanted);
    board_free(&b);
    return ok;
}

static int start_early(struct fixture *f, unsigned i, const char *first, int before)
{
    int held = !f->status;
    if (i == 0)
        held = held && !strcmp(f->out, "5\n") &&
               statuses("5:In_progress 6:Ready 30:Ready 31:Ready 32:In_progress 33:Backlog "
                        "34:Ready 35:Backlog 36:Backlog 37:Backlog 38:Backlog 39:Ready "
                        "40:Backlog 41:Backlog 42:Backlog") &&
               is_field("34", 6, "5") && is_field("40", 6, "-") && is_field("41", 6, "-") &&
               is_field("42", 6, "-");
    if (i == 1) {
        char *after = fx_read(env("BOARD_TEST_DATA"));
        held = held && !strcmp(f->out, "5\n") && first && after && !strcmp(first, after) &&
               !calls("item-edit") && !calls("issue comment") && !calls("-X POST");
        free(after);
    }
    if (i == 2)
        held = held && before && is_field("5", 3, "In_progress");
    if (i < 3)
        held = held && phase_record("5", "phase: started at " BOARD_HEAD);
    return held;
}

static int start_later(struct fixture *f, unsigned i)
{
    int held = !f->status;
    if (i == 3)
        held = held && is_field("34", 3, "Ready") && is_field("34", 6, "-") &&
               is_field("34", 8, "Seen in a log.\\n\\nPart of #5.") &&
               strstr(f->err, "board: #34: not linked to #5");
    if (i == 4)
        held = f->status && !*f->out && strstr(f->err, "#5 is In progress") &&
               !calls("item-edit") && !calls("issue comment");
    if (i == 5)
        held = held && !*f->out && !calls("item-edit");
    return held;
}

static int start_rows(struct fixture *f)
{
    const char *names[] = {"start-first", "start-again", "start-retry",
                           "start-full",  "start-other", "start-idle"};
    unsigned i;
    int ok = 1;
    for (i = 0; i < 6; i++) {
        char *first = NULL;
        int before = 0, held;
        if (!use(f, board_begin))
            return 0;
        if (i == 2)
            fx_env("BOARD_TEST_FAIL", "status");
        if (i == 3)
            fx_env("BOARD_TEST_FULL", "yes");
        if (i == 4 && !alter("5", 3, "In_progress"))
            return 0;
        if (i == 5 && (!alter("5", 3, "Backlog") || !alter("6", 3, "Backlog")))
            return 0;
        call(f, "start", i == 4 ? "6" : NULL, NULL, NULL);
        if (i == 1) {
            first = fx_read(env("BOARD_TEST_DATA"));
            call(f, "start", NULL, NULL, NULL);
        }
        if (i == 2) {
            before = f->status;
            fx_env("BOARD_TEST_FAIL", "none");
            fx_env("BOARD_TEST_HEAD", BOARD_LATER);
            call(f, "start", NULL, NULL, NULL);
        }
        held = i < 3 ? start_early(f, i, first, before) : start_later(f, i);
        free(first);
        ok = fx_check(f, held, names[i]) && ok;
    }
    return ok;
}

static int phase_rows(struct fixture *f)
{
    static const struct {
        const char *name, *extra, *line, *last;
    } rows[] = {{"phase-ready", "", NULL, "ready: yes " BOARD_HEAD},
                {"phase-none", "", "phase: none", "ready: no: no phase is In progress"},
                {"phase-work",
                 "14|OPEN|Ready|bug|-|7|0|-|open member\n15|OPEN|-|ci|-|-|0|-|ci: core fails\n",
                 "work: #14 #15", "ready: no: work: #14 #15"},
                {"phase-flaky", "16|OPEN|-|ci,intermittent|-|-|0|-|ci: night fails once\n",
                 "intermittent: #16", "ready: yes " BOARD_HEAD},
                {"phase-hold", "17|OPEN|-|hold|-|-|0|-|hold: core fails\n", "hold: #17",
                 "ready: no: hold #17"},
                {"phase-failed", "", NULL, "ready: no: review failed"},
                {"phase-review", "", "review: " BOARD_BASE ".." BOARD_LATER, "ready: no: review"},
                {"phase-core", "", "core: failure", "ready: no: core failure"},
                {"phase-lost", "", NULL, "ready: no: unreadable: issues"}};
    unsigned i;
    int ok = 1;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int held;
        if (!setup_data(f, board_review, rows[i].extra))
            return 0;
        if (i == 1 && !alter("7", 3, "Ready"))
            return 0;
        if (i == 5 && !alter("7", 4, "phase,review-failed"))
            return 0;
        if (i == 6 && !alter("7", 10,
                             "phase: started at " BOARD_BASE ";Looks right.;review: " BOARD_BASE
                             ".." BOARD_HEAD " complete;review: " BOARD_BASE ".." BOARD_LATER
                             " complete;phase: started at " BOARD_LATER))
            return 0;
        if (i == 7)
            fx_env("BOARD_TEST_CORE", "failure");
        if (i == 8)
            fx_env("BOARD_TEST_LOST", "issues");
        call(f, "phase", NULL, NULL, NULL);
        held = f->status == (i == 8 ? 1 : 0) && tail(f, rows[i].last) &&
               (!rows[i].line || said(f, rows[i].line));
        if (i == 0)
            held = held &&
                   !strcmp(f->out, "phase: #7 phase: seven\nbase: " BOARD_BASE "\nhead: " BOARD_HEAD
                                   "\nmembers: 0 open, 2 closed\nwork: none\nintermittent: "
                                   "none\nhold: none\nreview: " BOARD_BASE ".." BOARD_HEAD
                                   "\ncore: success\nready: yes " BOARD_HEAD "\n");
        if (i == 1)
            held = held && said(f, "base: unknown");
        if (i == 2)
            held = held && said(f, "members: 1 open, 2 closed");
        ok = fx_check(f, held, rows[i].name) && ok;
    }
    return ok;
}

int flow_board(struct fixture *f)
{
    static const char *const groups[] = {
        "success",      "added",   "validate", "create", "label1", "label2", "applied2", "recheck",
        "recheck-read", "project", "item",     "status", "term",   "remove", "close"};
    static const struct take_row takes[] = {
        {"yield-later", "alpha", "zed,alpha", "zed,alpha", "", "zed", 0},
        {"keep-earlier", "zed", "zed,alpha", "zed,alpha", "", "zed,alpha", 1},
        {"yield-again", "alpha", "zed,alpha", "alpha,zed,alpha", "", "zed", 0},
        {"yield-stale", "alpha", "zed", "zed,beta,alpha", "zed,beta", "zed", 0}};
    static const char *const states[] = {"take-closed", "release-closed", "release-open",
                                         "release-only"};
    unsigned i;
    int ok = 1;
    fx_env("FLOW_BOARD", "1");
    fx_env("BOARD_TEST_WORK", flow_path(f, "state"));
    fx_env("BOARD_TEST_DATA", flow_path(f, "data"));
    if (!flow_copy(f, "tools/flow/board.sh", "board.sh") || !fx_mock(f, "gh") ||
        !fx_mkdir(flow_path(f, "state")))
        return 0;
    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        ok = group_row(f, groups[i]) && ok;
    for (i = 0; i < sizeof(takes) / sizeof(takes[0]); i++)
        ok = take_row(f, &takes[i]) && ok;
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++)
        ok = state_row(f, states[i]) && ok;
    ok = select_rows(f) && ok;
    ok = take_phase_rows(f) && ok;
    ok = group_phase_rows(f) && ok;
    ok = archive_rows(f) && ok;
    ok = start_rows(f) && ok;
    ok = phase_rows(f) && ok;
    return ok;
}
