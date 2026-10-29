/* flow_board.c: the board product against a native, stateful GitHub fixture. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* The milestones, open ones in the order GitHub lists them. Milestone 1
 * writes its branch in backquotes with CRLF line ends; 3 names a branch with
 * a space and a semicolon, which is none, on its first `Branch:` line; 5
 * names none. */
static const struct milestone {
    const char *number, *state, *title, *description;
} milestone_table[] = {
    {"2", "open", "Second", "Branch: milestone/02-b"},
    {"3", "open", "Third", "Branch: milestone/03-c; true\nBranch: milestone/03-d\n"},
    {"1", "open", "First", "The first milestone.\r\nBranch: `milestone/01-a`\r\nMore.\r\n"},
    {"5", "open", "Fifth", "No branch is named."},
    {"4", "closed", "Fourth", "Branch: milestone/04-d\n"}};

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

/** Write S to O as a JSON string. */
static void json_string(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(o, "\\%c", c);
        else if (c < 0x20)
            fprintf(o, "\\u%04x", c);
        else
            fputc(c, o);
    }
    fputc('"', o);
}

/** Write the comma-joined LIST to O as GitHub's `{nodes: [{KEY: ...}]}`. */
static void json_nodes(FILE *o, const char *list, const char *key)
{
    char copy[4096], *p, *next;
    int first = 1;
    snprintf(copy, sizeof(copy), "%s", list);
    fputs("{\"nodes\":[", o);
    for (p = copy; *p; p = next) {
        next = strchr(p, ',');
        if (next)
            *next++ = 0;
        else
            next = p + strlen(p);
        if (!*p)
            continue;
        fprintf(o, "%s{\"%s\":", first ? "" : ",", key);
        json_string(o, p);
        fputc('}', o);
        first = 0;
    }
    fputs("]}", o);
}

/** Write milestone N to O, as GraphQL gives it with UPPER set, or null. */
static void json_milestone(FILE *o, const char *n, int upper)
{
    const struct milestone *m = milestone_of(n);
    if (!m) {
        fputs("null", o);
        return;
    }
    fprintf(o, "{\"number\":%s,\"state\":\"%s\",\"title\":", m->number,
            upper ? (!strcmp(m->state, "open") ? "OPEN" : "CLOSED") : m->state);
    json_string(o, m->title);
    fputs(",\"description\":", o);
    json_string(o, m->description);
    fputc('}', o);
}

/** Write the Status of R to O as `fieldValueByName`. */
static void json_status(FILE *o, const struct issue *r)
{
    const char *s = clean_field(r, F_STATUS);
    fputs("\"fieldValueByName\":", o);
    if (!*s) {
        fputs("null", o);
        return;
    }
    fputs("{\"name\":\"", o);
    for (; *s; s++)
        fputc(*s == '_' ? ' ' : *s, o);
    fputs("\"}", o);
}

/** Write issue R to O as board.sh's ISSUE asks for it, without the closing
 * brace, so that a caller can add fields. */
static void json_issue(FILE *o, const struct issue *r)
{
    const char *parent = clean_field(r, F_PARENT), *blockers = clean_field(r, F_BLOCKERS);
    fprintf(o, "{\"number\":%s,\"title\":", field(r, F_NUMBER));
    json_string(o, field(r, F_TITLE));
    fprintf(o, ",\"state\":\"%s\",\"assignees\":", field(r, F_STATE));
    json_nodes(o, clean_field(r, F_ASSIGNEES), "login");
    fputs(",\"labels\":", o);
    json_nodes(o, clean_field(r, F_LABELS), "name");
    if (*parent)
        fprintf(o, ",\"parent\":{\"number\":%s}", parent);
    else
        fputs(",\"parent\":null", o);
    fputs(",\"milestone\":", o);
    json_milestone(o, clean_field(r, F_MILESTONE), 1);
    fprintf(o, ",\"issueDependenciesSummary\":{\"blockedBy\":%s}", *blockers ? blockers : "0");
}

/** Write R to O as a node of the board's items, or of the repository's
 * issues when FORM is not `board`. */
static void json_node(FILE *o, const struct issue *r, const char *form)
{
    int archived = !strcmp(field(r, F_ARCHIVED), "archived");
    if (!strcmp(form, "board")) {
        fprintf(o, "{\"id\":\"I%s\",\"isArchived\":%s,", field(r, F_NUMBER),
                archived ? "true" : "false");
        json_status(o, r);
        fputs(",\"content\":", o);
        json_issue(o, r);
        fputs("}}", o);
        return;
    }
    json_issue(o, r);
    fprintf(o,
            ",\"projectItems\":{\"nodes\":[{\"id\":\"I%s\",\"project\":{\"number\":6,"
            "\"owner\":{\"login\":\"toksaitov\"}},",
            field(r, F_NUMBER));
    json_status(o, r);
    fputs("}]}}", o);
}

/** Write the SIZE bytes at P to the descriptor FD; fail when it takes no more. */
static int send_all(int fd, const char *p, size_t size)
{
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0)
            return 0;
        p += n;
        size -= (size_t)n;
    }
    return 1;
}

/** Wait for the gh process PID to accept a connection on the listening socket
 * S, or for it to end first; return the connection, or -1 with its status
 * in STATUS. */
static int connection(int s, pid_t pid, int *status)
{
    struct pollfd p;
    p.fd = s;
    p.events = POLLIN;
    while (poll(&p, 1, 100) <= 0)
        if (waitpid(pid, status, WNOHANG) == pid)
            return -1;
    return accept(s, NULL, NULL);
}

/** Answer the request of the gh process PID on connection C with the SIZE
 * bytes of JSON, and return gh's status. */
static int serve(int c, pid_t pid, const char *json, size_t size)
{
    char request[8192], head[200];
    size_t got = 0;
    ssize_t n;
    int status;
    while (got + 1 < sizeof(request) &&
           (n = read(c, request + got, sizeof(request) - 1 - got)) > 0) {
        got += (size_t)n;
        request[got] = 0;
        if (strstr(request, "\r\n\r\n"))
            break;
    }
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n"
             "Connection: close\r\n\r\n",
             (unsigned long)size);
    send_all(c, head, strlen(head));
    send_all(c, json, size);
    close(c);
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

/**
 * Print what gh's own --jq FILTER makes of the SIZE bytes of JSON, so that
 * board.sh's filters run in the jq that gh carries: the gh that BOARD_TEST_GH
 * names asks for it through the Unix socket that its configuration's
 * `http_unix_socket` names, in BOARD_TEST_WORK, and this process answers.
 * Return gh's status.
 */
static int gh_jq(const char *json, size_t size, const char *filter)
{
    struct sockaddr_un a;
    int s, c, status = 2;
    pid_t pid;
    if (chdir(env("BOARD_TEST_WORK")) || !fx_text("gh/config.yml", "http_unix_socket: gh.sock\n"))
        return 2;
    unlink("gh.sock");
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "gh.sock");
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1))
        return 2;
    fflush(stdout);
    signal(SIGPIPE, SIG_IGN);
    pid = fork();
    if (pid < 0)
        return 2;
    if (!pid) {
        close(s);
        setenv("GH_CONFIG_DIR", "gh", 1);
        setenv("GH_TOKEN", "fixture", 1);
        setenv("GH_NO_UPDATE_NOTIFIER", "1", 1);
        execl(env("BOARD_TEST_GH"), "gh", "api", "answer", "--jq", filter, (char *)NULL);
        _exit(127);
    }
    c = connection(s, pid, &status);
    close(s);
    if (c >= 0)
        status = serve(c, pid, json, size);
    else
        status = WIFEXITED(status) ? WEXITSTATUS(status) : 2;
    unlink("gh.sock");
    return status;
}

/** Answer the JSON that the stream O holds, through FILTER when it is given. */
static int answer(FILE *o, char **json, size_t *size, const char *filter)
{
    int status;
    if (fclose(o))
        return 2;
    if (*filter)
        status = gh_jq(*json, *size, filter);
    else
        status = fwrite(*json, 1, *size, stdout) != *size;
    free(*json);
    return status;
}

static int listed(const struct issue *r, const char *form)
{
    int archived = !strcmp(field(r, F_ARCHIVED), "archived");
    if (!strcmp(form, "board"))
        return strcmp(field(r, F_UNLISTED), "unlisted") != 0;
    if (archived)
        return 0;
    if (!strcmp(form, "repo-open"))
        return !strcmp(field(r, F_STATE), "OPEN");
    return 1;
}
struct request {
    const char *query, *item, *login, *n, *after, *jq;
};

/* Answer one page, two issues long, that starts after AFTER issues, as
 * GitHub's JSON put through the request's filter; the board's list gives its
 * archived items too. With BOARD_TEST_LOST naming the list, a page after the
 * first is lost, and with BOARD_TEST_FAIL `flaky`, each page fails once in a
 * run of board.sh. */
static int listing(struct board *b, const char *form, const struct request *r)
{
    unsigned i, count = 0, after = (unsigned)strtoul(r->after, NULL, 10);
    const char *lost = !strcmp(form, "board") ? "board" : "repo";
    char once[96], *json = NULL;
    size_t size = 0;
    FILE *o;
    if (after && !strcmp(env("BOARD_TEST_LOST"), lost))
        return 1;
    snprintf(once, sizeof(once), "flaky-%s-%u", form, after);
    if (!strcmp(env("BOARD_TEST_FAIL"), "flaky") && !fx_exists(state_path(once))) {
        fx_text(state_path(once), "");
        return 1;
    }
    o = open_memstream(&json, &size);
    if (!o)
        return 2;
    fputs(!strcmp(form, "board") ? "{\"data\":{\"user\":{\"projectV2\":{\"items\":{\"nodes\":["
                                 : "{\"data\":{\"repository\":{\"issues\":{\"nodes\":[",
          o);
    for (i = 0; i < b->count; i++) {
        if (!listed(&b->issues[i], form) || count++ < after)
            continue;
        if (count > after + 2)
            break;
        if (count > after + 1)
            fputc(',', o);
        json_node(o, &b->issues[i], form);
    }
    if (count > after + 2)
        fprintf(o, "],\"pageInfo\":{\"hasNextPage\":true,\"endCursor\":\"%u\"}}", after + 2);
    else
        fputs("],\"pageInfo\":{\"hasNextPage\":false,\"endCursor\":null}}", o);
    fputs(!strcmp(form, "board") ? "}}}}" : "}}}", o);
    return answer(o, &json, &size, r->jq);
}

static struct request request(int argc, char **argv)
{
    struct request r = {"", "", "", "", "", ""};
    int i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i - 1], "--id"))
            r.item = argv[i][0] == 'I' ? argv[i] + 1 : argv[i];
        if (!strcmp(argv[i - 1], "--jq"))
            r.jq = argv[i];
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
        return listing(b, strstr(r->query, "states: [OPEN]") ? "repo-open" : "repo", r);
    else if (strstr(r->query, "projectItems(first: 10)")) {
        if (!strcmp(env("BOARD_TEST_FAIL"), "item"))
            return 1;
        printf("I%s\n", r->n);
    } else if (strstr(r->query, "items(first: 100"))
        return listing(b, "board", r);
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
    } else
        return 2;
    return 0;
}

/** Answer the REST reads of milestones: `milestones?state=open`, the open
 * ones, or `milestones/M`, milestone M, which fails when there is none. */
static int milestones_read(const char *endpoint, const struct request *r)
{
    const char *one = strstr(endpoint, "/milestones/");
    char *json = NULL;
    size_t size = 0;
    unsigned i;
    FILE *o;
    if (!strcmp(env("BOARD_TEST_FAIL"), "milestones") || (one && !milestone_of(one + 12)))
        return 1;
    o = open_memstream(&json, &size);
    if (!o)
        return 2;
    if (one)
        json_milestone(o, one + 12, 0);
    else {
        fputc('[', o);
        for (i = 0; i < sizeof(milestone_table) / sizeof(milestone_table[0]); i++)
            if (!strcmp(milestone_table[i].state, "open")) {
                fputs(i ? "," : "", o);
                json_milestone(o, milestone_table[i].number, 0);
            }
        fputc(']', o);
    }
    return answer(o, &json, &size, r->jq);
}

/** Answer `repos/O/R/issues/N` as REST gives the issue: its number and its
 * milestone. */
static int issue_read(struct board *b, const char *endpoint, const struct request *r)
{
    struct issue *i = issue(b, number(endpoint));
    char *json = NULL;
    size_t size = 0;
    FILE *o;
    if (!strcmp(env("BOARD_TEST_FAIL"), "branch") || !i)
        return 1;
    o = open_memstream(&json, &size);
    if (!o)
        return 2;
    fprintf(o, "{\"number\":%s,\"milestone\":", field(i, F_NUMBER));
    json_milestone(o, clean_field(i, F_MILESTONE), 0);
    fputc('}', o);
    return answer(o, &json, &size, r->jq);
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
        return milestones_read(argv[3], r);
    else if (strstr(target, "/milestones/"))
        return milestones_read(target, r);
    else if (!strcmp(target, "-X") && argc > 4 && strstr(argv[4], "/assignees")) {
        if (!strcmp(fail, "assign") && !strcmp(argv[3], "POST"))
            return 1;
        return !reassign(b, number(argv[4]), !strcmp(argv[3], "POST"), r->login);
    } else if (strstr(target, "/issues/"))
        return issue_read(b, target, r);
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
    else if (!strcmp(argv[1], "issue") && argc > 3) {
        if (!strcmp(argv[2], "view"))
            status = issue_view(&b, argv[3], all);
        else if (!strcmp(argv[2], "edit") && strstr(all, " --add-label ")) {
            struct issue *i = issue(&b, argv[3]);
            char labels[4096];
            snprintf(labels, sizeof(labels), "%s,%s", i ? field(i, F_LABELS) : "", argv[argc - 1]);
            status = !change(&b, argv[3], F_LABELS, labels);
        }
    }
    board_free(&b);
    free(all);
    return status;
}

/* Milestones 1 and 2 are open with a branch, 3 open with none, and 4
 * closed, as milestone_table says. The tasks a worker may take are 30, 31, 33 and 36, and in that
 * order: milestone 1's before milestone 2's, each in the board's order. */
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
                                 "60|CLOSED|Done|bug|-|0|-|4|closed task of four\n";

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
    } rows[] = {{"next-order", "alpha", "", "30 31 36 40 33"},
                {"next-owner", "toksaitov", "", "30 31 47 36 40 33"},
                {"next-flaky", "alpha", "flaky", "30 31 36 40 33"},
                {"next-lost", "alpha", "board", NULL},
                {"next-unlisted", "alpha", "unlisted", "30 36 31 40 33"},
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
        const char *name, *n, *how, *status, *labels;
    } rows[] = {{"release-open", "49", NULL, "Ready", "bug"},
                {"release-owner", "49", "owner", "Backlog", "bug,owner"},
                {"release-only", "49", "owner-only", "Ready", "bug,owner-only"},
                {"release-closed", "51", NULL, "Done", "bug"}};
    unsigned i;
    int ok = 1, held;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!use(f, board_main) || !alter("51", F_ASSIGNEES, "alpha") ||
            !alter("51", F_STATUS, "In_progress"))
            return 0;
        call(f, "release", rows[i].n, rows[i].how);
        held = !f->status && is_field(rows[i].n, F_STATUS, rows[i].status) &&
               is_field(rows[i].n, F_LABELS, rows[i].labels) &&
               is_field(rows[i].n, F_ASSIGNEES, i == 3 ? "alpha" : "-");
        ok = fx_check(f, held, rows[i].name) && ok;
    }
    if (!use(f, board_main))
        return 0;
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

/* `milestone M` prints the branch of an open milestone, in backquotes and
 * with CRLF line ends or not, exits 3 for one that is closed or names no
 * branch that can be used, and 1 when the milestone cannot be read. */
static int milestone_rows(struct fixture *f)
{
    static const struct {
        const char *name, *m, *fail, *out;
        int status;
    } rows[] = {{"milestone-quoted", "1", "none", "milestone/01-a\n", 0},
                {"milestone-plain", "2", "none", "milestone/02-b\n", 0},
                {"milestone-unusable", "3", "none", "", 3},
                {"milestone-closed", "4", "none", "", 3},
                {"milestone-none", "5", "none", "", 3},
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
                                      "  tasks: 9 open, 1 closed\n"
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

/** Set BOARD_TEST_GH to the first gh on the PATH that is not the mock. */
static int real_gh(struct fixture *f)
{
    char *path = strdup(env("PATH")), *dir, *save = NULL, gh[4200];
    int found = 0;
    for (dir = path ? strtok_r(path, ":", &save) : NULL; dir && !found;
         dir = strtok_r(NULL, ":", &save)) {
        snprintf(gh, sizeof(gh), "%s/gh", dir);
        found = strcmp(dir, f->bin) && !access(gh, X_OK);
    }
    free(path);
    if (found)
        fx_env("BOARD_TEST_GH", gh);
    else
        fprintf(stderr, "fixture: board: no gh on the PATH to run board.sh's filters\n");
    return found;
}

int flow_board(struct fixture *f)
{
    int ok = 1;
    if (!real_gh(f))
        return 0;
    fx_env("FLOW_BOARD", "1");
    fx_env("BOARD_TEST_WORK", flow_path(f, "state"));
    fx_env("BOARD_TEST_DATA", flow_path(f, "data"));
    fx_env("BOARD_WAIT", "0");
    if (!flow_copy(f, "tools/flow/board.sh", "board.sh") || !fx_mock(f, "gh") ||
        !fx_mkdir(flow_path(f, "state")))
        return 0;
    ok = next_rows(f) && ok;
    ok = take_refusals(f) && ok;
    ok = take_rows(f) && ok;
    ok = release_rows(f) && ok;
    ok = small_rows(f) && ok;
    ok = milestones_rows(f) && ok;
    ok = archive_rows(f) && ok;
    ok = large_row(f) && ok;
    return ok;
}
