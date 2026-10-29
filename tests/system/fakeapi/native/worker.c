/* worker.c: verify the worker loop's board, log and session evidence. */
#include "api.h"
#include "../../../../tools/etude/volume/volume.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
static char *file_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    struct buffer b = {0};
    char block[8192];
    size_t n;
    if (!f)
        return NULL;
    while ((n = fread(block, 1, sizeof(block), f)))
        append(&b, block, n);
    if (ferror(f)) {
        clear(&b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (!b.data)
        putsb(&b, "");
    return b.data;
}
static struct value *at_json(const char *dir, const char *name)
{
    struct buffer path = {0};
    struct value *v;
    putsb(&path, dir);
    putsb(&path, "/");
    putsb(&path, name);
    v = read_json(path.data);
    clear(&path);
    return v;
}
static char *at_text(const char *dir, const char *name)
{
    struct buffer path = {0};
    char *v;
    putsb(&path, dir);
    putsb(&path, "/");
    putsb(&path, name);
    v = file_text(path.data);
    clear(&path);
    return v;
}
/*
 * The board the row leaves: #901 merged and closed by its worker, who stays
 * assigned, with a comment that names the commit; #902 handed back, Ready and
 * unassigned, with the session's comment; and #903, the subtask of #901,
 * closed by that worker with a comment that names the commit and never taken.
 * The machine's git checks that both comments name the milestone's head. The
 * session deletes task/901 itself, so the loop's log has no line of `tidy`.
 */
struct wanted_issue {
    long long number;
    const char *state, *status, *assignee, *comment;
    int whole; /* the comment is all of `comment`, not a prefix of it */
};
static const struct wanted_issue wanted[] = {
    {901, "CLOSED", "Done", "rach2-test", "Merged in ", 0},
    {902, "OPEN", "Ready", NULL, "The first part is on task/902; the rest waits for its blocker.",
     1},
    {903, "CLOSED", "Done", NULL, "Done in ", 0},
};

static int board(const char *dir)
{
    struct value *b = at_json(dir, "board.json"), *calls;
    const struct value *issue, *entry;
    unsigned seen = 0, i;
    struct buffer path = {0};
    int ok = 0;
    if (!b)
        return 0;
    for (issue = get(b, "issues") ? get(b, "issues")->child : NULL; issue; issue = issue->next) {
        long long n = number(issue, "number", 0);
        const struct value *assignees = get(issue, "assignees"), *comments = get(issue, "comments");
        for (i = 0; i < sizeof(wanted) / sizeof(wanted[0]) && wanted[i].number != n; i++)
            ;
        if (i == sizeof(wanted) / sizeof(wanted[0]))
            continue;
        if (strcmp(field(issue, "state"), wanted[i].state) ||
            strcmp(field(issue, "status"), wanted[i].status) ||
            count(assignees) != (size_t)(wanted[i].assignee != NULL) ||
            (wanted[i].assignee && strcmp(text(assignees->child), wanted[i].assignee)) ||
            count(comments) != 1 ||
            (wanted[i].whole
                 ? strcmp(text(comments->child), wanted[i].comment)
                 : strncmp(text(comments->child), wanted[i].comment, strlen(wanted[i].comment))))
            goto done;
        seen |= 1U << i;
    }
    if (seen != 7)
        goto done;
    putsb(&path, dir);
    putsb(&path, "/board.json.calls");
    calls = read_json_lines(path.data);
    clear(&path);
    if (!calls || !calls->child) {
        destroy(calls);
        goto done;
    }
    for (entry = calls->child; entry; entry = entry->next)
        if (entry->type != 'a' || !entry->child || !strcmp(text(entry->child), "unexpected")) {
            destroy(calls);
            goto done;
        }
    log_line("worker: %lu calls to gh", (unsigned long)count(calls));
    destroy(calls);
    ok = 1;
done:
    destroy(b);
    return ok;
}
static int log_proof(const char *dir)
{
    char *log = at_text(dir, "state/loop.log"), *line, *save = NULL;
    unsigned at = 0, starts = 0, took = 0, tidied = 0;
    const char *fixed[] = {
        NULL,        "took #901", NULL, NULL,     "#901 is closed or no longer this account's",
        "took #902", NULL,        NULL, "stopped"};
    if (!log)
        return 0;
    for (line = strtok_r(log, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *body = strchr(line, ' '), token[128], tail;
        unsigned issue, seconds;
        int match = 0;
        if (body)
            body = strchr(body + 1, ' ');
        if (!body) {
            free(log);
            return 0;
        }
        body++;
        if (sscanf(body, "session %127[0-9-] on #%u starts%c", token, &issue, &tail) == 2)
            starts++;
        if (!strncmp(body, "took #", 6))
            took++;
        if (!strncmp(body, "tidy: ", 6))
            tidied++;
        if (at == 0) {
            size_t n = strlen(body);
            match = !strncmp(body, "loop starts: claude in ", strlen("loop starts: claude in ")) &&
                    n >= 6 && !strcmp(body + n - 6, "/clone");
        } else if (at == 2 || at == 6)
            match = sscanf(body, "session %127[0-9-] on #%u starts%c", token, &issue, &tail) == 2 &&
                    issue == (at == 2 ? 901U : 902U);
        else if (at == 3 || at == 7)
            match =
                sscanf(body, "session %127[0-9-] ended 0 after %us%c", token, &seconds, &tail) == 2;
        else if (at < 9)
            match = !strcmp(body, fixed[at]);
        if (match)
            at++;
        log_line("worker: loop: %s", body);
    }
    free(log);
    return at == 9 && starts == 2 && took == 2 && !tidied;
}
static int sessions(const char *dir)
{
    struct buffer path = {0};
    DIR *directory;
    struct dirent *entry;
    unsigned n = 0;
    int ok = 1;
    putsb(&path, dir);
    putsb(&path, "/state");
    directory = opendir(path.data);
    clear(&path);
    if (!directory)
        return 0;
    while ((entry = readdir(directory))) {
        size_t len = strlen(entry->d_name);
        struct value *lines;
        const struct value *last;
        if (len < 9 || strncmp(entry->d_name, "s-", 2) || strcmp(entry->d_name + len - 6, ".jsonl"))
            continue;
        putsb(&path, dir);
        putsb(&path, "/state/");
        putsb(&path, entry->d_name);
        lines = read_json_lines(path.data);
        clear(&path);
        last = lines ? lines->child : NULL;
        while (last && last->next)
            last = last->next;
        if (!last || strcmp(field(last, "type"), "result") || !get(last, "is_error") ||
            get(last, "is_error")->type != 'f')
            ok = 0;
        destroy(lines);
        n++;
    }
    closedir(directory);
    return ok && n == 2;
}
int verify_worker_archive(const char *archive, const char *dir)
{
    const char *names[] = {"state",    "board.json",    "board.json.calls", "origin.git",
                           "loop.out", "binary.before", "binary.after"};
    const char *map[15];
    char *paths[7], why[512];
    unsigned i;
    int ok;
    char *before, *after;
    const char *digest = "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de";
    for (i = 0; i < 7; i++) {
        struct buffer p = {0};
        putsb(&p, dir);
        putsb(&p, "/");
        putsb(&p, names[i]);
        paths[i] = p.data;
        map[i * 2] = names[i];
        map[i * 2 + 1] = paths[i];
    }
    map[14] = NULL;
    ok = etude_volume_read(archive, map, why, sizeof(why)) == 0;
    for (i = 0; i < 7; i++)
        free(paths[i]);
    if (!ok) {
        log_line("worker: %s", why);
        return 1;
    }
    before = at_text(dir, "binary.before");
    after = at_text(dir, "binary.after");
    ok = before && after && !strncmp(before, digest, 64) && !strncmp(after, digest, 64) &&
         board(dir) && log_proof(dir) && sessions(dir);
    free(before);
    free(after);
    log_line("worker evidence: %s", ok ? "pass" : "fail");
    return ok ? 0 : 1;
}
