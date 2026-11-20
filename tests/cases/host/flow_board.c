/* flow_board.c: the board product against a native, stateful GitHub fixture.
 * Its gh answers each call with what board.sh's jq filter would print; the
 * filters themselves run in gh and are not evaluated here, because the
 * suite's sandbox has no jq engine. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fields of a fixture issue, one line of BOARD_TEST_DATA split at `|`;
 * `-` is an empty field. */
enum {
    F_NUMBER = 1,
    F_STATE,
    F_STATUS,
    F_LABELS,
    F_ASSIGNEES,
    F_BLOCKERS,
    F_PARENT,
    F_MILESTONE,
    F_TITLE,
    F_ARCHIVED,
    F_UNLISTED,
    F_COUNT = F_UNLISTED
};

/* The milestones, open ones in the order GitHub lists them, each with the
 * branch that board.sh's BRANCH filter prints for its description; 3 and 5
 * name none. */
static const struct milestone {
    const char *number, *state, *title, *branch;
} milestone_table[] = {{"2", "OPEN", "Second", "milestone/02-b"},
                       {"3", "OPEN", "Third", ""},
                       {"1", "OPEN", "First", "milestone/01-a"},
                       {"5", "OPEN", "Fifth", ""},
                       {"4", "CLOSED", "Fourth", "milestone/04-d"}};

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
    char *field[F_COUNT];
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
        } while (field && i->fields < F_COUNT);
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
        if (!strcmp(field(&b->issues[i], F_NUMBER), n))
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
        return 0;
    while (i->fields < k)
        i->field[i->fields++] = strdup("-");
    free(i->field[k - 1]);
    i->field[k - 1] = strdup(*value ? value : "-");
    return i->field[k - 1] && board_write(b);
}

/* Add LOGIN to issue N's assignees, or remove it. */
static int reassign(struct board *b, const char *n, int add, const char *login)
{
    struct issue *i = issue(b, n);
    char list[1024] = "", copy[1024], *p, *next;
    int found = 0;
    if (!i)
        return 0;
    snprintf(copy, sizeof(copy), "%s", clean_field(i, F_ASSIGNEES));
    for (p = copy; *p; p = next) {
        next = strchr(p, ',');
        if (next)
            *next++ = 0;
        else
            next = p + strlen(p);
        if (!strcmp(p, login)) {
            found = 1;
            if (!add)
                continue;
        }
        if (*p)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", *list ? "," : "", p);
    }
    if (add && !found)
        snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", *list ? "," : "", login);
    return change(b, n, F_ASSIGNEES, list);
}

static void translated(const char *s, char from, char to)
{
    for (; *s; s++)
        putchar(*s == from ? to : *s);
}

static const struct milestone *milestone_of(const char *n)
{
    unsigned i;
    for (i = 0; i < sizeof(milestone_table) / sizeof(milestone_table[0]); i++)
        if (!strcmp(milestone_table[i].number, n))
            return &milestone_table[i];
    return NULL;
}

static int listed(const struct issue *r, const char *form)
{
    if (!strcmp(field(r, F_ARCHIVED), "archived"))
        return 0;
    if (!strcmp(form, "board"))
        return strcmp(field(r, F_UNLISTED), "unlisted") != 0;
    if (!strcmp(form, "repo-open"))
        return !strcmp(field(r, F_STATE), "OPEN");
    return 1;
}

/* Print issue R as board.sh's FIELDS prints it. */
static void row(const struct issue *r)
{
    const struct milestone *m = milestone_of(clean_field(r, F_MILESTONE));
    const char *blockers = clean_field(r, F_BLOCKERS);
    printf("I%s\t", field(r, F_NUMBER));
    translated(clean_field(r, F_STATUS), '_', ' ');
    printf("\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t", field(r, F_NUMBER), field(r, F_STATE),
           clean_field(r, F_LABELS), clean_field(r, F_ASSIGNEES), *blockers ? blockers : "0",
           clean_field(r, F_PARENT), m ? m->number : "", m ? m->state : "", m ? m->branch : "");
    translated(field(r, F_TITLE), '\t', ' ');
    putchar('\n');
}

/* Answer one page, two issues long, that starts after AFTER issues; with
 * BOARD_TEST_LOST naming the list, a page after the first is lost, and with
 * BOARD_TEST_FAIL `flaky`, each page fails once in a run of board.sh. */
static int listing(struct board *b, const char *form, const char *cursor)
{
    unsigned i, count = 0, after = (unsigned)strtoul(cursor, NULL, 10);
    const char *lost = !strcmp(form, "board") ? "board" : "repo";
    char once[96];
    if (after && !strcmp(env("BOARD_TEST_LOST"), lost))
        return 1;
    snprintf(once, sizeof(once), "flaky-%s-%u", form, after);
    if (!strcmp(env("BOARD_TEST_FAIL"), "flaky") && !fx_exists(state_path(once))) {
        fx_text(state_path(once), "");
        return 1;
    }
    for (i = 0; i < b->count; i++) {
        if (!listed(&b->issues[i], form) || count++ < after)
            continue;
        if (count > after + 2) {
            printf("@page true %u\n", after + 2);
            return 0;
        }
        row(&b->issues[i]);
    }
    puts("@page false null");
    return 0;
}
struct request {
    const char *query, *item, *login, *n, *after;
};

static struct request request(int argc, char **argv)
{
    struct request r = {"", "", "", "", ""};
    int i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i - 1], "--id"))
            r.item = argv[i][0] == 'I' ? argv[i] + 1 : argv[i];
        if (!strcmp(argv[i - 1], "-f") || !strcmp(argv[i - 1], "-F")) {
            if (!strncmp(argv[i], "query=", 6))
                r.query = argv[i] + 6;
            if (!strncmp(argv[i], "assignees[]=", 12))
                r.login = argv[i] + 12;
            if (!strncmp(argv[i], "n=", 2))
                r.n = argv[i] + 2;
            if (!strncmp(argv[i], "endCursor=", 10))
                r.after = argv[i] + 10;
        }
    }
    return r;
}

static int graphql(struct board *b, const struct request *r)
{
    if (strstr(r->query, "field(name: \"Status\")"))
        puts("project=project\nfield=field\nBacklog=backlog\nReady=ready\nIn_progress="
             "progress\nDone=done");
    else if (strstr(r->query, "includeArchived: false"))
        return listing(b, strstr(r->query, "states: [OPEN]") ? "repo-open" : "repo", r->after);
    else if (strstr(r->query, "projectItems(first: 10)")) {
        if (!strcmp(env("BOARD_TEST_FAIL"), "item"))
            return 1;
        printf("I%s\n", r->n);
    } else if (strstr(r->query, "items(first: 100"))
        return listing(b, "board", r->after);
    else
        return 2;
    return 0;
}

/* The number in an endpoint `repos/O/R/issues/N[/...]`. */
static const char *number(const char *endpoint)
{
    static char n[64];
    const char *p = strstr(endpoint, "/issues/");
    if (!p)
        return "";
    p += 8;
    snprintf(n, sizeof(n), "%.*s", (int)strcspn(p, "/"), p);
    return n;
}

static int issue_view(struct board *b, const char *n, const char *all)
{
    struct issue *r = issue(b, n);
    const char *me = env("BOARD_TEST_ME");
    char padded[1100], wanted[100];
    if (!r)
        return 1;
    snprintf(padded, sizeof(padded), ",%s,", clean_field(r, F_ASSIGNEES));
    snprintf(wanted, sizeof(wanted), ",%s,", me);
    if (strstr(all, "--json state,assignees ")) {
        if (!strcmp(field(r, F_STATE), "OPEN") && *clean_field(r, F_ASSIGNEES)) {
            translated(clean_field(r, F_ASSIGNEES), ',', '\n');
            putchar('\n');
        }
    } else if (strstr(all, "--json state "))
        puts(field(r, F_STATE));
    else if (strstr(all, "--json assignees --jq if any"))
        puts(strstr(padded, wanted) ? "yes" : "no");
    else if (strstr(all, "--json assignees ")) {
        translated(clean_field(r, F_ASSIGNEES), ',', '\n');
        putchar('\n');
    } else if (strstr(all, "--json labels ")) {
        translated(clean_field(r, F_LABELS), ',', '\n');
        putchar('\n');
    } else
        return 2;
    return 0;
}

/* Remove LABEL from issue N's labels. */
static int unlabel(struct board *b, const char *n, const char *label)
{
    struct issue *i = issue(b, n);
    char list[4096] = "", copy[4096], *p, *next;
    if (!i)
        return 0;
    snprintf(copy, sizeof(copy), "%s", clean_field(i, F_LABELS));
    for (p = copy; *p; p = next) {
        next = strchr(p, ',');
        if (next)
            *next++ = 0;
        else
            next = p + strlen(p);
        if (*p && strcmp(p, label))
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", *list ? "," : "", p);
    }
    return change(b, n, F_LABELS, list);
}

/* Answer the REST reads of milestones as board.sh's filters print them:
 * `milestones?state=open`, a line for each open milestone, or `milestones/M`,
 * milestone M's branch while it is open, which fails when there is no M. */
static int milestones_read(const char *endpoint)
{
    const char *one = strstr(endpoint, "/milestones/");
    const struct milestone *m = one ? milestone_of(one + 12) : NULL;
    unsigned i;
    if (!strcmp(env("BOARD_TEST_FAIL"), "milestones") || (one && !m))
        return 1;
    if (m)
        puts(!strcmp(m->state, "OPEN") ? m->branch : "");
    for (i = 0; !one && i < sizeof(milestone_table) / sizeof(milestone_table[0]); i++)
        if (!strcmp(milestone_table[i].state, "OPEN"))
            printf("%s\t%s\t%s\n", milestone_table[i].number, milestone_table[i].branch,
                   milestone_table[i].title);
    return 0;
}

/* Answer `repos/O/R/issues/N` with the branch its milestone names. */
static int issue_read(struct board *b, const char *endpoint)
{
    struct issue *i = issue(b, number(endpoint));
    const struct milestone *m;
    if (!strcmp(env("BOARD_TEST_FAIL"), "branch") || !i)
        return 1;
    m = milestone_of(clean_field(i, F_MILESTONE));
    puts(m ? m->branch : "");
    return 0;
}

static int api(struct board *b, int argc, char **argv, const struct request *r)
{
    const char *target = argc > 2 ? argv[2] : "", *fail = env("BOARD_TEST_FAIL");
    if (!strcmp(target, "user"))
        puts(env("BOARD_TEST_ME"));
    else if (!strcmp(target, "graphql"))
        return graphql(b, r);
    else if (!strcmp(target, "--paginate") && argc > 3 && strstr(argv[3], "/timeline")) {
        translated(env("BOARD_TEST_TIMELINE"), ',', '\n');
        putchar('\n');
    } else if (!strcmp(target, "--paginate") && argc > 3 && strstr(argv[3], "/milestones?"))
        return milestones_read(argv[3]);
    else if (strstr(target, "/milestones/"))
        return milestones_read(target);
    else if (!strcmp(target, "-X") && argc > 4 && strstr(argv[4], "/assignees")) {
        if (!strcmp(fail, "assign") && !strcmp(argv[3], "POST"))
            return 1;
        return !reassign(b, number(argv[4]), !strcmp(argv[3], "POST"), r->login);
    } else if (strstr(target, "/issues/"))
        return issue_read(b, target);
    else
        return 2;
    return 0;
}

static int project(struct board *b, char **argv, const char *all, const struct request *r)
{
    const char *fail = env("BOARD_TEST_FAIL");
    if (!strcmp(argv[2], "item-edit")) {
        const char *p = strstr(all, "--single-select-option-id "), *status;
        if (!*r->item || !strcmp(fail, "status") || !p)
            return 1;
        p += 26;
        status = !strcmp(p, "backlog")    ? "Backlog"
                 : !strcmp(p, "ready")    ? "Ready"
                 : !strcmp(p, "progress") ? "In_progress"
                                          : "Done";
        return !change(b, r->item, F_STATUS, status);
    }
    if (!strcmp(argv[2], "item-archive")) {
        char failure[80];
        snprintf(failure, sizeof(failure), "archive%s", r->item);
        if (!strcmp(fail, failure))
            return 1;
        return !change(b, r->item, F_ARCHIVED, "archived");
    }
    return 2;
}

int flow_board_mock(int argc, char **argv)
{
    struct board b;
    struct request r = request(argc, argv);
    char *all = words(argc, argv);
    const char *name = strrchr(argv[0], '/');
    int status = 2;
    /* git answers only board.sh's question for the main working tree's
     * common directory, BOARD_TEST_GIT, and fails when that is empty, as
     * outside a clone. */
    if (!strcmp(name ? name + 1 : argv[0], "git")) {
        status = all && strstr(all, "rev-parse") && strstr(all, "--git-common-dir") &&
                         *env("BOARD_TEST_GIT")
                     ? puts(env("BOARD_TEST_GIT")) < 0
                     : 128;
        free(all);
        return status;
    }
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
    else if (!strcmp(argv[1], "issue") && argc > 3) {
        if (!strcmp(argv[2], "view"))
            status = issue_view(&b, argv[3], all);
        else if (!strcmp(argv[2], "edit") && strstr(all, " --add-label ")) {
            struct issue *i = issue(&b, argv[3]);
            char labels[4096];
            snprintf(labels, sizeof(labels), "%s,%s", i ? field(i, F_LABELS) : "", argv[argc - 1]);
            status = !change(&b, argv[3], F_LABELS, labels);
        } else if (!strcmp(argv[2], "edit") && strstr(all, " --remove-label "))
            status = !unlabel(&b, argv[3], argv[argc - 1]);
    }
    board_free(&b);
    free(all);
    return status;
}

/* Milestones 1 and 2 are open with a branch, 3 open with none, and 4
 * closed, as milestone_table says. The tasks a worker may take are 30, 31,
 * 33, 36 and, on Linux, 55, in the board's order whatever their milestones:
 * 40, 30, 31, 33, 36 and 55. */
static const char board_main[] = "40|OPEN|Ready|bug|-|0|-|2|second milestone, first\n"
                                 "30|OPEN|Ready|bug|-|0|-|1|first milestone, first\n"
                                 "41|OPEN|Ready|bug|-|0|30|1|subtask of 30\n"
                                 "31|OPEN|Ready|bug|-|0|-|1|first milestone, second\n"
                                 "42|OPEN|Ready|bug|-|0|-|3|milestone with no branch\n"
                                 "43|OPEN|Ready|bug|-|0|-|4|closed milestone\n"
                                 "44|OPEN|Ready|bug|-|0|-|-|no milestone\n"
                                 "45|OPEN|Ready|bug|-|1|-|1|blocked\n"
                                 "46|OPEN|Ready|bug,owner|-|0|-|1|waits for the owner\n"
                                 "47|OPEN|Ready|bug,owner-only|-|0|-|1|owner-only\n"
                                 "48|OPEN|Backlog|bug|-|0|-|1|in Backlog\n"
                                 "49|OPEN|In_progress|bug|alpha|0|-|1|In progress\n"
                                 "50|OPEN|Ready|bug|zed|0|-|1|assigned\n"
                                 "51|CLOSED|Done|bug|-|0|-|1|closed task\n"
                                 "52|CLOSED|Ready|bug|-|0|30|1|closed subtask\n"
                                 "33|OPEN|Ready|bug|-|0|-|2|second milestone, second\n"
                                 "36|OPEN|Ready|bug|-|0|-|1|first milestone, third\n"
                                 "53|CLOSED|Done|bug|-|0|-|2|closed task of two\n"
                                 "54|CLOSED|Done|bug|-|0|60|-|closed subtask of a task of four\n"
                                 "60|CLOSED|Done|bug|-|0|-|4|closed task of four\n"
                                 "55|OPEN|Ready|bug,linux-host|-|0|-|1|builds the GNU layer\n";

static int use(struct fixture *f, const char *data)
{
    static const char *const names[] = {"FAIL", "LOST", "ME", "TIMELINE", NULL};
    static const char *const values[] = {"none", "", "alpha", "alpha"};
    unsigned i;
    char name[80];
    for (i = 0; names[i]; i++) {
        snprintf(name, sizeof(name), "BOARD_TEST_%s", names[i]);
        fx_env(name, values[i]);
    }
    /* The main working tree of the clone, whose Lenovo lock is free. */
    fx_env("BOARD_TEST_GIT", flow_path(f, "main/.git"));
    return fx_remove(flow_path(f, "main")) && fx_mkdir(flow_path(f, "main/.git")) &&
           flow_put(f, "data", data);
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

static int call(struct fixture *f, const char *a, const char *b, const char *c)
{
    const char *args[] = {a, b, c, NULL};
    return run(f, args);
}

static int is_field(const char *n, unsigned k, const char *wanted)
{
    struct board b;
    struct issue *i;
    int ok;
    if (!board_read(&b))
        return 0;
    i = issue(&b, n);
    ok = i && !strcmp(field(i, k), wanted);
    board_free(&b);
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

static int calls(const char *text)
{
    return fx_has(state_path("calls"), text);
}

/* Succeed when the run passed and the first words of its lines, joined by
 * spaces, are WANTED. */
static int first_words(struct fixture *f, const char *wanted)
{
    const char *p = f->out;
    char out[4096] = "";
    size_t n = 0;
    if (f->status)
        return 0;
    while (p && *p) {
        size_t len = strcspn(p, " \t\n");
        if (len) {
            if (n + len + 2 >= sizeof(out))
                return 0;
            if (n)
                out[n++] = ' ';
            memcpy(out + n, p, len);
            n += len;
            out[n] = 0;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return !strcmp(out, wanted);
}

/* Succeed when the run failed with nothing on stdout and REFUSAL on stderr. */
static int refused(struct fixture *f, const char *refusal)
{
    return f->status && !*f->out && fx_regex(f->err, refusal);
}

static int next_rows(struct fixture *f)
{
    static const struct {
        const char *name, *me, *setup, *words;
    } rows[] = {{"next-order", "alpha", "", "40 30 31 33 36 55"},
                {"next-owner", "toksaitov", "", "40 30 31 47 33 36 55"},
                {"next-owner-lenovo", "toksaitov", "lenovo", "40 30 31 33 36 55"},
                {"next-owner-busy", "toksaitov", "busy", "40 30 31 33 36 55"},
                {"next-owner-no-clone", "toksaitov", "no-clone", "40 30 31 33 36 55"},
                {"next-flaky", "alpha", "flaky", "40 30 31 33 36 55"},
                {"next-lost", "alpha", "board", NULL},
                {"next-unlisted", "alpha", "unlisted", "40 30 33 36 55 31"},
                {"next-unlisted-lost", "alpha", "repo", NULL}};
    unsigned i;
    int ok = 1, held;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!use(f, board_main))
            return 0;
        fx_env("BOARD_TEST_ME", rows[i].me);
        if (!strcmp(rows[i].setup, "flaky"))
            fx_env("BOARD_TEST_FAIL", "flaky");
        if (!strcmp(rows[i].setup, "board") || !strcmp(rows[i].setup, "repo"))
            fx_env("BOARD_TEST_LOST", rows[i].setup);
        if ((!strcmp(rows[i].setup, "unlisted") || !strcmp(rows[i].setup, "repo")) &&
            !alter("31", F_UNLISTED, "unlisted"))
            return 0;
        /* The Lenovo's lock is held, another owner-only task is In
         * progress, or no main working tree is found: each offers no step
         * on the Lenovo. */
        if (!strcmp(rows[i].setup, "lenovo") && !fx_mkdir(flow_path(f, "main/build/flow/lenovo")))
            return 0;
        if (!strcmp(rows[i].setup, "busy") && !alter("49", F_LABELS, "bug,owner-only"))
            return 0;
        if (!strcmp(rows[i].setup, "no-clone"))
            fx_env("BOARD_TEST_GIT", "");
        call(f, "next", NULL, NULL);
        held = rows[i].words ? first_words(f, rows[i].words) : refused(f, "cannot read the board");
        if (i == 0)
            held = held && strstr(f->out, "30\tfirst milestone, first\n") != NULL;
        ok = fx_check(f, held, rows[i].name) && ok;
    }
    return ok;
}

static int take_refusals(struct fixture *f)
{
    static const struct {
        const char *name, *n, *why;
    } rows[] = {{"take-subtask", "41", "it is a subtask of #30; take its task"},
                {"take-closed-milestone", "43", "it is in no open milestone"},
                {"take-no-milestone", "44", "it is in no open milestone"},
                {"take-no-branch", "42", "its milestone names no branch"},
                {"take-blocked", "45", "it has an open blocker"},
                {"take-owner", "46", "it waits for the owner"},
                {"take-only", "47", "only the owner's accounts can do its work"},
                {"take-backlog", "48", "its Status is Backlog, not Ready"},
                {"take-absent", "99", "it is not on the board"}};
    unsigned i;
    int ok = 1;
    char refusal[200];
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!use(f, board_main) ||
            (!strcmp(rows[i].n, "99") &&
             !flow_put(f, "data", "99|OPEN|Ready|bug|-|0|-|1|gone|archived\n")))
            return 0;
        call(f, "take", rows[i].n, NULL);
        snprintf(refusal, sizeof(refusal), "^board: #%s: %s\n$", rows[i].n, rows[i].why);
        ok = fx_check(f,
                      f->status && fx_regex(f->err, refusal) && !calls("-X POST") &&
                          !calls("item-edit"),
                      rows[i].name) &&
             ok;
    }
    if (!use(f, board_main))
        return 0;
    call(f, "take", "51", NULL);
    ok = fx_check(f,
                  f->status && strstr(f->err, "board: #51: closed: set Done\n") &&
                      is_field("51", F_STATUS, "Done") && !calls("-X POST"),
                  "take-closed") &&
         ok;
    return ok;
}

static int take_rows(struct fixture *f)
{
    static const struct {
        const char *name, *me, *n, *now, *timeline, *after;
        int keep;
    } rows[] = {{"take-ready", "alpha", "30", "-", "alpha", "alpha", 1},
                {"take-resume", "alpha", "49", "alpha", "alpha", "alpha", 1},
                {"take-owner-only", "toksaitov", "47", "-", "toksaitov", "toksaitov", 1},
                {"yield-later", "alpha", "30", "zed", "zed,alpha", "zed", 0},
                {"keep-earlier", "zed", "30", "zed,alpha", "zed,alpha", "zed,alpha", 1},
                {"yield-again", "alpha", "30", "zed", "alpha,zed,alpha", "zed", 0},
                {"yield-stale", "alpha", "30", "zed", "zed,beta,alpha", "zed", 0}};
    unsigned i;
    int ok = 1, held;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!use(f, board_main) || !alter(rows[i].n, F_ASSIGNEES, rows[i].now))
            return 0;
        fx_env("BOARD_TEST_ME", rows[i].me);
        fx_env("BOARD_TEST_TIMELINE", rows[i].timeline);
        call(f, "take", rows[i].n, NULL);
        if (rows[i].keep)
            held = !f->status && is_field(rows[i].n, F_STATUS, "In_progress") &&
                   is_field(rows[i].n, F_ASSIGNEES, rows[i].after);
        else
            held = f->status && !calls("item-edit") &&
                   is_field(rows[i].n, F_ASSIGNEES, rows[i].after) &&
                   strstr(f->err, "taken by ") != NULL;
        ok = fx_check(f, held, rows[i].name) && ok;
    }
    /* A take that fails after it assigned leaves the issue as it found it. */
    if (!use(f, board_main))
        return 0;
    fx_env("BOARD_TEST_FAIL", "status");
    call(f, "take", "30", NULL);
    ok = fx_check(f,
                  f->status && is_field("30", F_ASSIGNEES, "-") &&
                      is_field("30", F_STATUS, "Ready") &&
                      strstr(f->err, "board: #30: cannot set it In progress\n"),
                  "take-undo") &&
         ok;
    return ok;
}

static int release_rows(struct fixture *f)
{
    static const struct {
        const char *name, *n, *how, *status, *labels, *assignees;
    } rows[] = {{"release-open", "49", NULL, "Ready", "bug", "-"},
                {"release-owner", "49", "owner", "Backlog", "bug,owner", "-"},
                {"release-only", "49", "owner-only", "Ready", "bug,owner-only", "-"},
                {"release-trial", "49", "trial", "Backlog", "bug,owner-only", "-"},
                {"release-only-linux", "55", "owner-only", "Ready", "bug,owner-only", "-"},
                {"release-closed", "51", NULL, "Done", "bug", "alpha"}};
    unsigned i;
    int ok = 1, held;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!use(f, board_main) || !alter("51", F_ASSIGNEES, "alpha") ||
            !alter("51", F_STATUS, "In_progress"))
            return 0;
        call(f, "release", rows[i].n, rows[i].how);
        held = !f->status && is_field(rows[i].n, F_STATUS, rows[i].status) &&
               is_field(rows[i].n, F_LABELS, rows[i].labels) &&
               is_field(rows[i].n, F_ASSIGNEES, rows[i].assignees);
        ok = fx_check(f, held, rows[i].name) && ok;
    }
    if (!use(f, board_main))
        return 0;
    fx_env("BOARD_TEST_ME", "zed");
    call(f, "release", "49", "owner-only");
    ok =
        fx_check(f,
                 f->status && is_field("49", F_STATUS, "In_progress") &&
                     is_field("49", F_ASSIGNEES, "alpha") && is_field("49", F_LABELS, "bug") &&
                     strstr(f->err, "alpha holds it") && !calls("item-edit") && !calls("-X DELETE"),
                 "release-other-account") &&
        ok;
    fx_env("BOARD_TEST_ME", "alpha");
    call(f, "release", "49", "Backlog");
    ok = fx_check(f, f->status == 2 && !calls("item-edit"), "release-unknown") && ok;
    fx_env("BOARD_TEST_FAIL", "status");
    call(f, "release", "49", NULL);
    ok = fx_check(f,
                  f->status && is_field("49", F_ASSIGNEES, "alpha") &&
                      strstr(f->err, "it is still assigned to alpha"),
                  "release-kept") &&
         ok;
    return ok;
}

static int small_rows(struct fixture *f)
{
    int ok = 1;
    if (!use(f, board_main))
        return 0;
    call(f, "mine", "49", NULL);
    ok = fx_check(f, !f->status && !strcmp(f->out, "yes\n"), "mine-yes") && ok;
    call(f, "mine", "50", NULL);
    ok = fx_check(f, !f->status && !strcmp(f->out, "no\n"), "mine-other") && ok;
    if (!alter("51", F_ASSIGNEES, "alpha"))
        return 0;
    call(f, "mine", "51", NULL);
    ok = fx_check(f, !f->status && !strcmp(f->out, "no\n"), "mine-closed") && ok;
    call(f, "branch", "30", NULL);
    ok = fx_check(f, !f->status && !strcmp(f->out, "milestone/01-a\n"), "branch") && ok;
    call(f, "branch", "42", NULL);
    ok = fx_check(f, refused(f, "^board: #42: it has no milestone, or its milestone names no "),
                  "branch-none") &&
         ok;
    fx_env("BOARD_TEST_FAIL", "branch");
    call(f, "branch", "30", NULL);
    ok = fx_check(f, refused(f, "^board: #30: cannot read its milestone\n$"), "branch-lost") && ok;
    fx_env("BOARD_TEST_FAIL", "none");
    call(f, "ready", "48", "49");
    ok = fx_check(f,
                  first_words(f, "48") && is_field("48", F_STATUS, "Ready") &&
                      is_field("49", F_STATUS, "Ready"),
                  "ready") &&
         ok;
    return ok;
}

/* `milestone M` prints the branch of an open milestone, exits 3 for one that
 * is closed or names no branch, and 1 when the milestone cannot be read. */
static int milestone_rows(struct fixture *f)
{
    static const struct {
        const char *name, *m, *fail, *out;
        int status;
    } rows[] = {{"milestone", "1", "none", "milestone/01-a\n", 0},
                {"milestone-closed", "4", "none", "", 3},
                {"milestone-none", "3", "none", "", 3},
                {"milestone-absent", "9", "none", "", 1},
                {"milestone-lost", "1", "milestones", "", 1}};
    unsigned i;
    int ok = 1;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!use(f, board_main))
            return 0;
        fx_env("BOARD_TEST_FAIL", rows[i].fail);
        call(f, "milestone", rows[i].m, NULL);
        ok = fx_check(f,
                      f->status == rows[i].status && !strcmp(f->out, rows[i].out) &&
                          (!rows[i].status || *f->err),
                      rows[i].name) &&
             ok;
    }
    return ok;
}

static int milestones_rows(struct fixture *f)
{
    int ok = 1;
    if (!use(f, board_main))
        return 0;
    call(f, "milestones", NULL, NULL);
    ok = fx_check(f,
                  !f->status &&
                      !strcmp(f->out, "milestone 1: First\n"
                                      "  branch: milestone/01-a\n"
                                      "  tasks: 10 open, 1 closed\n"
                                      "  #49 In progress: alpha: In progress\n"
                                      "milestone 2: Second\n"
                                      "  branch: milestone/02-b\n"
                                      "  tasks: 2 open, 1 closed\n"
                                      "milestone 3: Third\n"
                                      "  branch: none\n"
                                      "  tasks: 1 open, 0 closed\n"
                                      "milestone 5: Fifth\n"
                                      "  branch: none\n"
                                      "  tasks: 0 open, 0 closed\n") &&
                      !calls("states: [OPEN, CLOSED]"),
                  "milestones") &&
         ok;
    fx_env("BOARD_TEST_FAIL", "milestones");
    call(f, "milestones", NULL, NULL);
    ok = fx_check(f, refused(f, "cannot read the milestones"), "milestones-lost") && ok;
    return milestone_rows(f) && ok;
}

/** Check that `archive` with no milestone takes those of closed milestones. */
static int archive_finished(struct fixture *f)
{
    if (!use(f, board_main))
        return 0;
    call(f, "archive", NULL, NULL);
    return fx_check(f,
                    !f->status && is_field("54", F_ARCHIVED, "archived") &&
                        is_field("60", F_ARCHIVED, "archived") && is_field("51", F_ARCHIVED, ""),
                    "archive-finished");
}

static int archive_rows(struct fixture *f)
{
    int ok = 1;
    if (!use(f, board_main))
        return 0;
    call(f, "archive", "1", NULL);
    ok = fx_check(f,
                  first_words(f, "51") && strstr(f->out, "51 52 \n") &&
                      is_field("51", F_ARCHIVED, "archived") &&
                      is_field("52", F_ARCHIVED, "archived") && is_field("30", F_ARCHIVED, "") &&
                      is_field("53", F_ARCHIVED, ""),
                  "archive") &&
         ok;
    if (!use(f, board_main))
        return 0;
    call(f, "archive", "4", NULL);
    ok = fx_check(f, !f->status && !strcmp(f->out, "54 60 \n") && is_field("43", F_ARCHIVED, ""),
                  "archive-closed-milestone") &&
         ok;
    if (!use(f, board_main))
        return 0;
    fx_env("BOARD_TEST_FAIL", "archive51");
    call(f, "archive", "1", NULL);
    ok = fx_check(f,
                  f->status && !strcmp(f->out, "52 \n") &&
                      strstr(f->err, "board: #51: cannot archive its card\n") &&
                      is_field("52", F_ARCHIVED, "archived"),
                  "archive-fail") &&
         ok;
    if (!use(f, board_main) || !alter("52", F_UNLISTED, "unlisted"))
        return 0;
    call(f, "done", NULL, NULL);
    ok = fx_check(f, !f->status && !strcmp(f->out, "52 \n") && is_field("52", F_STATUS, "Done"),
                  "done-unlisted") &&
         ok;
    return ok;
}

/* A board whose issue list is longer than Linux lets one argument or
 * environment string be, 128 KiB, still moves its closed issues to Done. */
static int large_row(struct fixture *f)
{
    enum { ISSUES = 100, TITLE = 1400 };
    size_t base = strlen(board_main);
    char *data = malloc(base + ISSUES * (TITLE + 64) + 1), *p;
    char wanted[ISSUES * 5 + 1] = "";
    unsigned i;
    int ok;
    if (!data)
        return 0;
    memcpy(data, board_main, base);
    p = data + base;
    for (i = 0; i < ISSUES; i++) {
        p += sprintf(p, "%u|CLOSED|Ready|bug|-|0|-|1|", 100 + i);
        memset(p, 'x', TITLE);
        p += TITLE;
        *p++ = '\n';
        sprintf(wanted + strlen(wanted), "%s%u", i ? " " : "", 100 + i);
    }
    *p = 0;
    ok = use(f, data);
    free(data);
    if (!ok || !alter("52", F_STATUS, "Done"))
        return 0;
    call(f, "done", NULL, NULL);
    {
        char line[ISSUES * 5 + 3];
        snprintf(line, sizeof(line), "%s \n", wanted);
        return fx_check(f,
                        !f->status && !strcmp(f->out, line) && is_field("100", F_STATUS, "Done") &&
                            is_field("199", F_STATUS, "Done"),
                        "done-large");
    }
}

int flow_board(struct fixture *f)
{
    int ok = 1;
    fx_env("FLOW_BOARD", "1");
    fx_env("BOARD_TEST_WORK", flow_path(f, "state"));
    fx_env("BOARD_TEST_DATA", flow_path(f, "data"));
    fx_env("BOARD_WAIT", "0");
    /* A copy on the testing host lets this account take #55 on any system. */
    fx_env("RACH2_TESTING_COPY", "copy");
    /* release's rows are a person's, outside a worker loop. */
    fx_env("RACH2_WORKER_STATE", NULL);
    if (!flow_copy(f, "tools/flow/board.sh", "board.sh") || !fx_mock(f, "gh") ||
        !fx_mock(f, "git") || !fx_mkdir(flow_path(f, "state")))
        return 0;
    ok = next_rows(f) && ok;
    ok = take_refusals(f) && ok;
    ok = take_rows(f) && ok;
    ok = release_rows(f) && ok;
    ok = small_rows(f) && ok;
    ok = milestones_rows(f) && ok;
    ok = archive_rows(f) && ok;
    ok = archive_finished(f) && ok;
    ok = large_row(f) && ok;
    return ok;
}
