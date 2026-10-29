/* flow_release.c: resume records, selective reruns and acceptance ordering. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define H "1111111111111111111111111111111111111111"
#define C "2222222222222222222222222222222222222222"
#define T "2026-10-10T00:00:00Z"
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static void append(const char *file, const char *text)
{
    FILE *f = fopen(file, "ab");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
static const char *rp(const char *suffix)
{
    static char p[16][4096];
    static unsigned i;
    char *v = p[i++ % 16];
    snprintf(v, 4096, "%s%s", ev("RELEASE_TEST_CALLS"), suffix);
    return v;
}
static char *joined(int argc, char **argv, int first)
{
    size_t n = 2;
    int i;
    char *s;
    for (i = first; i < argc; i++)
        n += strlen(argv[i]) + 1;
    s = malloc(n);
    if (!s)
        return NULL;
    s[0] = 0;
    for (i = first; i < argc; i++) {
        if (i > first)
            strcat(s, " ");
        strcat(s, argv[i]);
    }
    strcat(s, "\n");
    return s;
}
static void call(const char *prefix, int argc, char **argv, int first)
{
    char *s = joined(argc, argv, first), *v;
    if (!s)
        return;
    v = malloc(strlen(prefix) + strlen(s) + 1);
    if (v) {
        strcpy(v, prefix);
        strcat(v, s);
        append(ev("RELEASE_TEST_CALLS"), v);
        free(v);
    }
    free(s);
}
static int sh_mock(int argc, char **argv)
{
    const char *name = argc > 1 ? strrchr(argv[1], '/') : NULL;
    name = name ? name + 1 : argc > 1 ? argv[1] : "";
    if (!strcmp(name, "board.sh")) {
        call("board ", argc, argv, 2);
        if (argc > 2 && !strcmp(argv[2], "phase"))
            puts(ev("RELEASE_TEST_BOARD"));
        return 0;
    }
    if (!strcmp(name, "notify.sh")) {
        call("notify ", argc, argv, 2);
        return 0;
    }
    if (!strcmp(name, "testing.sh")) {
        char prefix[5000];
        snprintf(prefix, sizeof(prefix), "testing %s ", ev("RACH2_TESTING_COPY"));
        call(prefix, argc, argv, 2);
        puts("testing: the host did not answer");
        return 1;
    }
    if (!strcmp(name, "metal_accept.sh")) {
        call("accept ", argc, argv, 2);
        if (!strcmp(ev("RELEASE_TEST_ACCEPT"), "pass")) {
            puts("metal-accept: PASS");
            return 0;
        }
        puts("  FAIL: the hub section found no hub\nmetal-accept: FAIL (1)");
        return 1;
    }
    argv[0] = "/bin/sh";
    execv(argv[0], argv);
    return 127;
}
/**
 * Answer `gh issue view N` with the arguments `all`: the state and title that
 * RELEASE_TEST_ISSUES gives issue N, or the state RELEASE_TEST_STATE.
 */
static int gh_view(const char *number, const char *all)
{
    if (!strstr(all, "--json state,title")) {
        if (!strstr(all, "--json state"))
            return 1;
        puts(ev("RELEASE_TEST_STATE"));
        return 0;
    }
    char *data = strdup(ev("RELEASE_TEST_ISSUES")), *line, *save = NULL;
    for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *end = strchr(line, ' ');
        if (end) {
            *end = 0;
            if (!strcmp(line, number))
                puts(end + 1);
        }
    }
    free(data);
    return 0;
}
static int gh_mock(int argc, char **argv)
{
    char log[10000], *all = joined(argc, argv, 1);
    int status = 0;
    if (argc < 3 || !all) {
        free(all);
        return 1;
    }
    snprintf(log, sizeof(log), "gh %s %s %s\n", argv[1], argv[2], argc > 3 ? argv[3] : "");
    append(ev("RELEASE_TEST_CALLS"), log);
    if (!strcmp(argv[1], "issue") && !strcmp(argv[2], "comment"))
        append(rp(".comments"), all);
    else if (!strcmp(argv[1], "issue") && !strcmp(argv[2], "create")) {
        append(rp(".created"), all);
        puts("https://github.com/toksaitov/rach2/issues/40");
    } else if (!strcmp(argv[1], "issue") && !strcmp(argv[2], "close")) {
    } else if (!strcmp(argv[1], "issue") && !strcmp(argv[2], "view"))
        status = gh_view(argc > 3 ? argv[3] : "", all);
    else if (!strcmp(argv[1], "api") && strstr(argv[2], "/statuses"))
        puts(ev("RELEASE_TEST_STATUSES"));
    else if (!strcmp(argv[1], "run") && !strcmp(argv[2], "list"))
        status = strcmp(ev("RELEASE_TEST_RUNS"), "none") ||
                 fx_has(ev("RELEASE_TEST_CALLS"), "dispatch ");
    else if (!strcmp(argv[1], "workflow") && !strcmp(argv[2], "run")) {
        char *dispatch = malloc(strlen(all) + 10);
        if (!dispatch)
            status = 1;
        else {
            sprintf(dispatch, "dispatch %s", all);
            append(ev("RELEASE_TEST_CALLS"), dispatch);
            free(dispatch);
        }
    } else
        status = 1;
    free(all);
    return status;
}
static int ssh_mock(int argc, char **argv)
{
    const char *host = argc > 2 ? argv[argc - 2] : "", *command = argc > 1 ? argv[argc - 1] : "";
    char line[20000];
    if (!strcmp(host, "testing")) {
        int windows = strstr(command, "bash.exe -l -s") != NULL;
        if (windows || strstr(command, "wsl.exe -- bash -s")) {
            char b[4096];
            while (fgets(b, sizeof(b), stdin)) {
                snprintf(line, sizeof(line), "%s %s", windows ? "windows" : "wsl", b);
                append(ev("RELEASE_TEST_CALLS"), line);
            }
            if (windows)
                puts(ev("RELEASE_TEST_WINDOWS"));
            return 0;
        }
        snprintf(line, sizeof(line), "ssh testing %s\n", command);
        append(ev("RELEASE_TEST_CALLS"), line);
        return 0;
    }
    if (!strncmp(command, "cat /sys/class/net/eth0/address;",
                 strlen("cat /sys/class/net/eth0/address;"))) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        puts("28:d2:44:7e:e0:af\nroot=/dev/disk0p3 rach2.slot=candidate\n5000.00 9000.00\nidentity "
             "0 nvme S6P2NX0T381638Z");
        return 0;
    }
    if (!strcmp(command, "bootctl status")) {
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        puts("bootctl: next_entry: empty");
        return 0;
    }
    if (!strcmp(command, "bootctl commit")) {
        append(ev("RELEASE_TEST_CALLS"), "machine write bootctl commit\n");
        puts("bootctl: committed: candidate");
        return 0;
    }
    if (!strcmp(command, "cat /etc/rach2/release 2> /dev/null; echo answered")) {
        char *s = fx_read(rp(".release"));
        append(ev("RELEASE_TEST_CALLS"), "machine read\n");
        fputs(s, stdout);
        free(s);
        puts("answered");
        return 0;
    }
    if (!strncmp(command, "{ mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&",
                 strlen("{ mkdir -p /etc/rach2 && cat > /etc/rach2/release.new &&"))) {
        FILE *f;
        char b[4096];
        size_t n;
        const char *how = ev("RELEASE_TEST_WRITES");
        if (!strcmp(how, "no")) {
            append(ev("RELEASE_TEST_CALLS"), "machine write refused\n");
            return 1;
        }
        append(ev("RELEASE_TEST_CALLS"), "machine write release\n");
        f = !strcmp(how, "lost") ? NULL : fopen(rp(".release"), "wb");
        while ((n = fread(b, 1, sizeof(b), stdin)))
            if (f && fwrite(b, 1, n, f) != n) {
                fclose(f);
                return 1;
            }
        if (f)
            fclose(f);
        if (!strcmp(how, "unsynced"))
            puts("sync: error syncing '/etc/rach2/release': Input/output error\nstatus 1");
        else
            puts("status 0");
        return 0;
    }
    snprintf(line, sizeof(line), "machine write %s\n", command);
    append(ev("RELEASE_TEST_CALLS"), line);
    return 1;
}
/**
 * Answer `git show OBJECT`: the registry of three suites, or the site that
 * RELEASE_TEST_SITES gives OBJECT's commit.
 */
static int git_show(const char *object)
{
    if (strstr(object, ":tests/registry")) {
        puts("# the "
             "suites\nagents|qemu|full||4096|x86_64\nfs|qemu|full\nbigmem|qemu|full||73728|x86_"
             "64");
        return 0;
    }
    if (!strstr(object, ":tools/ci/site"))
        return 1;
    char *data = strdup(ev("RELEASE_TEST_SITES")), *line, *save = NULL;
    int found = 0;
    for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char commit[100], label[100], accel[100];
        if (sscanf(line, "%99s %99s %99s", commit, label, accel) == 3 &&
            !strncmp(object, commit, strlen(commit)) && object[strlen(commit)] == ':') {
            printf("runner %s\naccelerator %s\n", label, accel);
            found = 1;
        }
    }
    free(data);
    return found ? 0 : 1;
}
static int git_mock(int argc, char **argv)
{
    call("git ", argc, argv, 1);
    if (!*ev("RELEASE_TEST_GIT"))
        return 1;
    if (argc > 2 && !strcmp(argv[1], "-C")) {
        argv += 2;
        argc -= 2;
    }
    if (argc < 2)
        return 1;
    if (!strcmp(argv[1], "fetch") || !strcmp(argv[1], "merge-base") || !strcmp(argv[1], "bundle"))
        return 0;
    if (!strcmp(argv[1], "rev-list")) {
        char *range = argc > 3 ? strstr(argv[3], "..") : NULL;
        if (range && strncmp(argv[3], range + 2, (size_t)(range - argv[3])))
            puts("3333333333333333333333333333333333333333");
        return 0;
    }
    if (!strcmp(argv[1], "log")) {
        puts(ev("RELEASE_TEST_LOG"));
        return 0;
    }
    if (!strcmp(argv[1], "show") && argc > 2)
        return git_show(argv[2]);
    if (!strcmp(argv[1], "worktree")) {
        if (argc > 5 && !strcmp(argv[2], "add")) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/tools/dev/testing.sh", argv[5]);
            return fx_text(path, "# native testing command fixture\n") ? 0 : 1;
        }
        return 0;
    }
    return 1;
}
int flow_release_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "sh"))
        return sh_mock(argc, argv);
    if (!strcmp(name, "gh"))
        return gh_mock(argc, argv);
    if (!strcmp(name, "ssh"))
        return ssh_mock(argc, argv);
    if (!strcmp(name, "git"))
        return git_mock(argc, argv);
    if (!strcmp(name, "make") || !strcmp(name, "scp")) {
        call(!strcmp(name, "make") ? "make " : "scp ", argc, argv, 1);
        return 0;
    }
    if (!strcmp(name, "native-deploy")) {
        char logs[4200];
        call("native-deploy ", argc, argv, 1);
        if (!strcmp(ev("RELEASE_TEST_NATIVE"), "pass"))
            return 0;
        if (argc > 9) {
            snprintf(logs, sizeof(logs), "%s/collection-2.out", argv[9]);
            mkdir(argv[9], 0700);
            fx_text(logs,
                    "native: credprobe: pass; evidence e\nnative: dirprobe.ext2: failed: exited "
                    "1\nnative: result: 2 cases, 1 failed or incomplete\n");
        }
        return 1;
    }
    if (!strcmp(name, "sleep")) {
        char *line = joined(argc, argv, 1), *s, *p;
        unsigned n = 0;
        if (!line)
            return 1;
        append(rp(".sleeps"), line);
        free(line);
        s = fx_read(rp(".sleeps"));
        for (p = s; *p; p++)
            if (*p == '\n')
                n++;
        free(s);
        if (n > 20)
            kill(getppid(), SIGTERM);
        return 0;
    }
    return 97;
}
struct release {
    struct fixture *f;
    char tree[4096], script[4096], retired[4096];
    char head[2000], built[3000], installing[4000], tried[5000], accepted[6000], finished[7000],
        ended[2000];
};
struct release_row {
    const char *record, *board, *state, *writes, *issues, *statuses, *runs, *log, *git, *sites,
        *accept, *machines, *windows, *native;
};

static int setup(struct release *r, struct fixture *f)
{
    char path[4096];
    unsigned i;
    const char *mock[] = {"sh", "gh", "ssh", "git", "make", "sleep", "scp"};
    memset(r, 0, sizeof(*r));
    r->f = f;
    snprintf(r->tree, sizeof(r->tree), "%s/tree", f->work);
    snprintf(r->script, sizeof(r->script), "%s/tools/flow/release.sh", r->tree);
    if (!fx_copy(flow_source(f, "tools/flow/release.sh"), r->script))
        return 0;
    snprintf(path, sizeof(path), "%s/tools/ci/site.sh", r->tree);
    if (!fx_copy(flow_source(f, "tools/ci/site.sh"), path))
        return 0;
    flow_put(f, "tree/krffs/rach2_demo_ed25519", "");
    flow_put(f, "tree/tools/flow/board.sh", "# native board fixture\n");
    flow_put(f, "tree/tools/flow/notify.sh", "# native notification fixture\n");
    for (i = 0; i < 7; i++)
        if (!fx_mock(f, mock[i]))
            return 0;
    fx_env("FLOW_RELEASE", "1");
    snprintf(r->head, sizeof(r->head),
             "phase 7\nsha %s\nchosen %s\ndispatched 1760054400\nrun 101 success\nfull done %s run "
             "101\ntouched %s\n",
             H, T, T, T);
    snprintf(r->built, sizeof(r->built),
             "%sbuild done %s kernel k root r\nrecovery done %s kernel k image i\ntransfer done %s "
             "kernel and root in /dev/shm\nkernel done %s candidate slot\n",
             r->head, T, T, T, T);
    snprintf(r->installing, sizeof(r->installing), "%sinstall started %s\n", r->built, T);
    snprintf(
        r->tried, sizeof(r->tried),
        "%sinstall done %s root r\ntrial done %s the new kernel runs from the candidate slot\n",
        r->installing, T, T);
    snprintf(r->accepted, sizeof(r->accepted), "%saccept done %s metal-accept: PASS; committed\n",
             r->tried, T);
    snprintf(r->finished, sizeof(r->finished), "%srecord done %s /etc/rach2/release names %s\n",
             r->accepted, T, H);
    snprintf(r->ended, sizeof(r->ended),
             "phase 7\nsha %s\nchosen %s\ntouched %s\nvoid: ready: no: work: #14\n", H, T, T);
    return 1;
}
static void run(struct release *r, const struct release_row *spec)
{
    struct fixture *f = r->f;
    char board[1000];
    const char *files[] = {"calls", "calls.sleeps", "calls.release", "calls.comments",
                           "calls.created"};
    unsigned i;
    fx_remove(flow_path(f, "state"));
    for (i = 0; i < 5; i++)
        fx_remove(flow_path(f, files[i]));
    flow_put(f, "state/wt/krffs/rach2_demo_ed25519", "");
    flow_put(f, "state/wt/build/x86_64/images/release.sha256", "");
    flow_put(f, "state/wt/tests/system/accept/metal_accept.sh", "# native acceptance fixture\n");
    flow_put(f, "state/wt/tools/ci/site", "runner rach2\naccelerator tcg\n");
    flow_put(f, "calls", "");
    flow_put(f, "tree/tools/flow/machines", spec->machines ? spec->machines : "# none\n");
    flow_put(f, "state/record", spec->record);
    flow_put(f, "before", spec->record);
    snprintf(board, sizeof(board), "phase: #7 phase: seven\n%s", spec->board);
    fx_env("RELEASE_TEST_CALLS", flow_path(f, "calls"));
    fx_env("RELEASE_TEST_BOARD", board);
    fx_env("RELEASE_TEST_STATE", spec->state ? spec->state : "OPEN");
    fx_env("RELEASE_TEST_WRITES", spec->writes ? spec->writes : "no");
    fx_env("RELEASE_TEST_ISSUES", spec->issues ? spec->issues : "");
    fx_env("RELEASE_TEST_STATUSES", spec->statuses ? spec->statuses : "");
    fx_env("RELEASE_TEST_RUNS", spec->runs ? spec->runs : "");
    fx_env("RELEASE_TEST_LOG", spec->log ? spec->log : "");
    fx_env("RELEASE_TEST_GIT", spec->git ? spec->git : "");
    fx_env("RELEASE_TEST_SITES", spec->sites ? spec->sites : H " rach2 tcg\n" C " rach2 tcg\n");
    fx_env("RELEASE_TEST_ACCEPT", spec->accept ? spec->accept : "");
    fx_env("RELEASE_TEST_WINDOWS", spec->windows ? spec->windows : "");
    fx_env("RELEASE_TEST_NATIVE", spec->native ? spec->native : "");
    if (spec->native) {
        flow_put(f, "state/wt/build/tools/dev/native-hostpeer", "");
        fx_copy(f->self, flow_path(f, "state/wt/build/tools/dev/native-deploy"));
    }
    {
        const char *a[] = {"/bin/sh", r->script, "-s", flow_path(f, "state"), "7", NULL};
        fx_run(f, a, r->tree, NULL, NULL);
    }
}
static int called(struct release *r, const char *pattern)
{
    char *s = fx_read(flow_path(r->f, "calls"));
    int held = fx_regex(s, pattern);
    free(s);
    return held;
}
static int archived(struct release *r, const char *prefix)
{
    DIR *d = opendir(flow_path(r->f, "state"));
    struct dirent *e;
    unsigned count = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, prefix, strlen(prefix)) && !strstr(e->d_name, ".sha256")) {
            snprintf(r->retired, sizeof(r->retired), "%s/state/%s", r->f->work, e->d_name);
            count++;
        }
    closedir(d);
    return count == 1;
}
static char *last_line(const char *path)
{
    char *s = fx_read(path), *end = s + strlen(s), *at;
    while (end > s && end[-1] == '\n')
        *--end = 0;
    at = strrchr(s, '\n');
    at = strdup(at ? at + 1 : s);
    free(s);
    return at;
}
static int last_is(const char *path, const char *text)
{
    char *line = last_line(path);
    int held = !strcmp(line, text);
    free(line);
    return held;
}
static int fourth_is(const char *path, const char *text)
{
    char *s = fx_read(path), *at = s;
    unsigned n;
    int held = 0;
    for (n = 0; n < 3 && at; n++) {
        at = strchr(at, '\n');
        if (at)
            at++;
    }
    if (at) {
        char *end = strchr(at, '\n');
        if (end)
            *end = 0;
        held = !strcmp(at, text);
    }
    free(s);
    return held;
}
static unsigned count(const char *path, const char *pattern)
{
    char *s = fx_read(path), *line, *save = NULL;
    unsigned n = 0;
    for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (fx_regex(line, pattern))
            n++;
    free(s);
    return n;
}
static int boards(struct release *r, const char *wanted)
{
    char *s = fx_read(flow_path(r->f, "calls")), *line, *save = NULL, got[1000] = "";
    for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, "board ", 6) && strlen(got) + strlen(line) + 2 < sizeof(got)) {
            strcat(got, line);
            strcat(got, ",");
        }
    free(s);
    return !strcmp(got, wanted);
}
static int check(struct release *r, int held, const char *label)
{
    return fx_check(r->f, held, label);
}
static int closed_records(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    char *line;
    run(r, &(struct release_row){.record = r->finished, .board = "ready: yes " H});
    {
        char sum[4096];
        int held = !f->status && archived(r, "7-11111111-");
        snprintf(sum, sizeof(sum), "%s.sha256", r->retired);
        line = last_line(r->retired);
        held = held && !strncmp(line, "closed ", 7) && count(r->retired, " done ") == 9 &&
               called(r, "^gh issue close 7$") && called(r, "^board archive 7$") &&
               fx_exists(sum) && called(r, "^git worktree remove --force .*/state/wt$") &&
               !called(r, "^(machine|ssh|git -C|gh run|gh workflow)") &&
               !fx_exists(flow_path(f, "state/record"));
        free(line);
        ok = check(r, held, "all done only closes and reports") && ok;
    }
    run(r, &(struct release_row){.record = r->head, .board = "ready: yes " H});
    ok = check(r,
               f->status == 75 && count(flow_path(f, "state/record"), "^full done ") == 1 &&
                   called(r, "^git -C .* fetch") &&
                   !called(r, "^(machine write|gh run|gh workflow)"),
               "full run not repeated") &&
         ok;
    run(r, &(struct release_row){.record = r->ended, .board = "ready: no: work: #14"});
    ok = check(r,
               f->status == 75 && fx_same(flow_path(f, "before"), flow_path(f, "state/record")) &&
                   !archived(r, "7-") && !called(r, "^(machine|gh)"),
               "void record waits unchanged") &&
         ok;
    return ok;
}
static int closing_rows(struct release *r)
{
    struct fixture *f = r->f;
    char record[9000], *line;
    int ok = closed_records(r);
    snprintf(record, sizeof(record), "%sreported %s\nclosing %s\n", r->finished, T, T);
    run(r, &(struct release_row){.record = record, .board = "ready: yes " H});
    ok = check(r,
               !f->status && archived(r, "7-11111111-") && count(r->retired, "^closing ") == 2 &&
                   boards(r, "board phase,board done,board archive 7,") &&
                   called(r, "^gh issue close 7$"),
               "open closing phase checked again") &&
         ok;
    run(r, &(struct release_row){.record = record, .board = "ready: no: work: #14"});
    ok = check(r,
               !f->status && last_is(flow_path(f, "state/record"), "void: ready: no: work: #14") &&
                   boards(r, "board phase,") && called(r, "^gh issue comment 7$") &&
                   !called(r, "^gh issue close"),
               "unready closing phase remains open") &&
         ok;
    {
        char changed[9000];
        snprintf(changed, sizeof(changed), "phase 6\n%s", strchr(record, '\n') + 1);
        run(r,
            &(struct release_row){.record = changed, .board = "ready: yes " C, .state = "CLOSED"});
        archived(r, "6-11111111-");
        line = last_line(r->retired);
        ok = check(r,
                   !f->status && archived(r, "6-11111111-") &&
                       !fx_exists(flow_path(f, "state/record")) && !strncmp(line, "closed ", 7) &&
                       boards(r, "board done,board archive 6,") &&
                       !called(r, "^(gh issue close|gh issue comment|machine)"),
                   "closing previous phase resumes its record") &&
             ok;
        free(line);
    }
    return ok;
}
static int new_candidate_rows(struct release *r)
{
    struct fixture *f = r->f;
    char record[9000], expected[1000];
    unsigned i;
    int ok = 1;
    for (i = 0; i < 2; i++) {
        if (i)
            snprintf(record, sizeof(record),
                     "phase 6\nsha %s\nchosen %s\ntouched %s\nfailed full: run 101 failed\n", H, T,
                     T);
        else
            snprintf(record, sizeof(record), "%s", r->ended);
        run(r, &(struct release_row){.record = record, .board = "ready: yes " C});
        ok =
            check(r,
                  f->status == 75 && archived(r, i ? "6-11111111-" : "7-11111111-") &&
                      fx_same(flow_path(f, "before"), r->retired) && !called(r, "^machine write") &&
                      count(flow_path(f, "state/record"), ".") == 4 &&
                      fx_has(flow_path(f, "state/record"), "phase 7\nsha " C "\n") &&
                      last_is(flow_path(f, "state/record"), "touched " T),
                  i ? "failed record copied aside" : "void record copied aside") &&
            ok;
    }
    snprintf(record, sizeof(record), "phase 7\nsha %s\nchosen %s\ntouched %s\n", C, T, T);
    run(r, &(struct release_row){.record = record, .board = "ready: yes " C, .runs = "none"});
    snprintf(expected, sizeof(expected),
             "^dispatch workflow run full.yml --repo toksaitov/rach2 --ref master -f sha=%s -f "
             "title=Release tests of 22222222 for phase #7$",
             C);
    return check(r,
                 f->status == 75 && called(r, expected) &&
                     count(flow_path(f, "calls"), "^dispatch ") == 1,
                 "new candidate runs every suite") &&
           ok;
}
static int interrupted_install_records(struct release *r)
{
    struct fixture *f = r->f;
    char record[9000];
    int ok = 1;
    snprintf(record, sizeof(record), "%sfailed install: rach2-install ended 1: no space\n",
             r->installing);
    run(r, &(struct release_row){.record = record, .board = "ready: yes " C});
    ok = check(r,
               f->status == 1 && fx_same(flow_path(f, "before"), flow_path(f, "state/record")) &&
                   !archived(r, "7-") && fx_equal(flow_path(f, "calls"), "") &&
                   (strstr(f->out, "delete the record's `install started` line") ||
                    strstr(f->err, "delete the record's `install started` line")),
               "failed install requires manual record repair") &&
         ok;
    run(r, &(struct release_row){.record = r->installing, .board = "ready: yes " H});
    ok = check(r,
               f->status == 1 &&
                   last_is(flow_path(f, "state/record"),
                           "failed install: the machine runs system candidate after the installer "
                           "was started, so a reboot came before the release saw its result; check "
                           "the root by hand") &&
                   !called(r, "^(machine write|board)"),
               "install reboot does not write machine") &&
         ok;
    return ok;
}
static int install_record_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = interrupted_install_records(r);
    unsigned i;
    const char *writes[] = {"yes", "lost", "unsynced"};
    for (i = 0; i < 3; i++) {
        run(r, &(struct release_row){
                   .record = r->accepted, .board = "ready: yes " H, .writes = writes[i]});
        if (!i) {
            char *calls = fx_read(flow_path(f, "calls")),
                 *write = strstr(calls, "machine write release\n"),
                 *close = strstr(calls, "gh issue close 7\n");
            ok = check(r,
                       !f->status && archived(r, "7-11111111-") &&
                           fx_equal(flow_path(f, "calls.release"),
                                    "commit " H "\nphase 7\nfull 101\nroot \n") &&
                           count(r->retired, "^record done ") == 1 && write && close &&
                           write < close,
                       "release record written before phase closes") &&
                 ok;
            free(calls);
        } else {
            const char *last =
                i == 1 ? "failed record: /etc/rach2/release on the machine does not name " H ": "
                       : "failed record: writing /etc/rach2/release failed: sync: error syncing "
                         "'/etc/rach2/release': Input/output error status 1";
            ok = check(r,
                       f->status == 1 && last_is(flow_path(f, "state/record"), last) &&
                           !fx_has(flow_path(f, "state/record"), "reported ") &&
                           !fx_has(flow_path(f, "state/record"), "record done ") &&
                           !called(r, "^gh issue close"),
                       i == 1 ? "lost read-back blocks closing" : "failed flush blocks closing") &&
                 ok;
        }
    }
    return ok;
}
static int rerun_rows(struct release *r)
{
    struct fixture *f = r->f;
    char record[4000], expected[1000];
    unsigned i;
    int ok = 1;
    const char *logs[] = {"Fix agents\n\nCloses #20",
                          "Fix agents",
                          "Fix agents\n\nCloses #20\nCloses #30",
                          "Fix agents\n\nCloses #20",
                          "Fix agents\n\nCloses #20",
                          "Fix agents\n\nCloses #20"};
    snprintf(record, sizeof(record),
             "phase 7\nsha %s\nchosen %s\ntouched %s\nfiled 101 #20\nfailed full: run 101 failed; "
             "ci filed its failures as #20\n",
             H, T, T);
    for (i = 0; i < 6; i++) {
        const char *issues = i == 4 ? "20 OPEN Release tests fail on x86_64: agents, bigmem"
                                    : "20 CLOSED Release tests fail on x86_64: agents, bigmem";
        const char *statuses = i == 3 ? "ci/full/i686 failure\nci/full/x86_64 failure"
                                      : "ci/full/i686 success\nci/full/x86_64 failure";
        run(r, &(struct release_row){.record = record,
                                     .board = "ready: yes " C,
                                     .issues = issues,
                                     .statuses = statuses,
                                     .log = logs[i],
                                     .git = "yes",
                                     .sites = i == 5 ? H " rach2 tcg\n" C " rach2-mac kvm" : NULL});
        ok = check(r,
                   f->status == 75 && fx_has(flow_path(f, "state/record"), "sha " C "\n") &&
                       (i ? !fx_has(flow_path(f, "state/record"), "rerun ")
                          : fourth_is(flow_path(f, "state/record"),
                                      "rerun 101 " H " x86_64:agents x86_64:bigmem")),
                   i == 0   ? "only failure repairs select rerun"
                   : i == 1 ? "unrelated commit selects full run"
                   : i == 2 ? "additional issue selects full run"
                   : i == 3 ? "unfiled architecture selects full run"
                   : i == 4 ? "open issue selects full run"
                            : "changed machine selects full run") &&
             ok;
    }
    snprintf(record, sizeof(record),
             "phase 7\nsha %s\nchosen %s\nrerun 101 %s x86_64:agents\ntouched %s\n", C, T, H, T);
    run(r, &(struct release_row){.record = record, .board = "ready: yes " C, .runs = "none"});
    snprintf(expected, sizeof(expected),
             "^dispatch workflow run full.yml --repo toksaitov/rach2 --ref master -f sha=%s -f "
             "title=Release tests of 22222222 for phase #7: again only agents on x86_64 -f "
             "suites=x86_64:agents$",
             C);
    return check(r,
                 f->status == 75 && called(r, expected) &&
                     count(flow_path(f, "calls"), "^dispatch ") == 1,
                 "selective rerun dispatch") &&
           ok;
}
static int intermittent_build_rows(struct release *r)
{
    struct fixture *f = r->f;
    char record[4000], expected[1000];
    int ok = 1;
    snprintf(record, sizeof(record),
             "phase 7\nsha %s\nchosen %s\ntouched %s\ndispatched 1760054400\nrun 101 "
             "failure\nfiled 101 #20\nfailed full: run 101 failed; ci filed its failures as #20\n",
             H, T, T);
    run(r, &(struct release_row){.record = record,
                                 .board = "ready: yes " H,
                                 .issues = "20 OPEN Release tests fail on x86_64: agents, bigmem",
                                 .git = "yes",
                                 .runs = "none"});
    snprintf(expected, sizeof(expected),
             "^dispatch workflow run full.yml --repo toksaitov/rach2 --ref master -f sha=%s -f "
             "title=Release tests of 11111111 for phase #7$",
             H);
    ok = check(r,
               f->status == 75 && archived(r, "7-11111111-") &&
                   fx_same(flow_path(f, "before"), r->retired) &&
                   fx_has(flow_path(f, "state/record"), "phase 7\nsha " H "\n") &&
                   fourth_is(flow_path(f, "state/record"), "touched " T) &&
                   !count(flow_path(f, "state/record"), "^(rerun|run) ") && called(r, expected) &&
                   count(flow_path(f, "calls"), "^dispatch ") == 1 &&
                   !called(r, "^gh issue (close|edit|reopen)"),
               "intermittent failure repeats full run without changing issue") &&
         ok;
    run(r, &(struct release_row){.record = r->head, .board = "ready: yes " H, .git = "yes"});
    snprintf(expected, sizeof(expected),
             "^testing %s/rach2/build/worktrees/release run ARCH=x86_64 make clean && "
             "ARCH=x86_64 make machine GNU_AGENTS=all && ",
             FLOW_OWNER_HOME);
    return check(r,
                 f->status == 75 && !fx_has(flow_path(f, "state/record"), "build done ") &&
                     called(r, expected),
                 "machine build starts clean") &&
           ok;
}
static int acceptance_rows(struct release *r)
{
    struct fixture *f = r->f;
    int ok = 1;
    char *calls, *accept, *commit, *write, *close;
    run(r, &(struct release_row){
               .record = r->tried, .board = "ready: yes " H, .writes = "yes", .accept = "pass"});
    calls = fx_read(flow_path(f, "calls"));
    accept = strstr(calls, "accept 192.168.0.106\n");
    commit = strstr(calls, "machine write bootctl commit\n");
    write = strstr(calls, "machine write release\n");
    close = strstr(calls, "gh issue close 7\n");
    ok = check(r,
               !f->status && archived(r, "7-11111111-") && accept && commit && write && close &&
                   accept < commit && commit < write && write < close &&
                   count(flow_path(f, "calls"), "^(accept|machine write|gh issue close)") == 4 &&
                   count(r->retired, "^accept done .* metal-accept: PASS; committed$") == 1 &&
                   fx_has(flow_path(f, "calls.comments"),
                          "* The release tests ran on the one machine that the commit's "
                          "`tools/ci/site` selects, whose runners have the label `rach2`, with "
                          "QEMU's accelerator `tcg`."),
               "acceptance commits trial then records and closes phase") &&
         ok;
    free(calls);
    run(r, &(struct release_row){
               .record = r->tried, .board = "ready: yes " H, .writes = "yes", .accept = "fail"});
    calls = last_line(flow_path(f, "state/record"));
    ok = check(r,
               f->status == 1 &&
                   fx_regex(calls, "^failed accept: metal_accept.sh failed: .*FAIL: the hub "
                                   "section found no hub") &&
                   called(r, "^accept 192.168.0.106$") &&
                   !called(r, "^(machine write|gh issue close)"),
               "failed acceptance does not commit trial") &&
         ok;
    free(calls);
    return ok;
}
#define MAC "mac accel tcg\nmac env JOBS=8\n"
#define WINDOWS                                                                                \
    "windows accel whpx\nwindows tree D:/w\nwindows out D:/o\nwindows site C:/s\nwindows env " \
    "JOBS=30\nwindows path /ucrt64/bin\n"
#define LENOVO                                                                              \
    "lenovo target /var/native\nlenovo peer 192.168.0.100\nlenovo echo-port 55090\nlenovo " \
    "network "                                                                              \
    "192.168.0.106/24\nlenovo interface eth0\nlenovo gateway 192.168.0.1\nlenovo far "      \
    "1.1.1.1\nlenovo empty 192.168.0.250\nlenovo silent 192.168.0.252\n"
static int machine_line(struct release *r, const char *prefix, char *rest, size_t size)
{
    char *s = fx_read(flow_path(r->f, "state/record")), *line, *save = NULL;
    int found = 0;
    for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, prefix, strlen(prefix))) {
            snprintf(rest, size, "%s", line + strlen(prefix));
            found = 1;
        }
    free(s);
    return found;
}
/**
 * Find the Mac run that the record says started, copy its identity to `id`,
 * and wait up to ten seconds for its times file to record its end, since the
 * release starts the run detached.
 */
static int mac_ran(struct release *r, char *id, size_t size)
{
    char path[4200];
    struct timespec pause = {0, 50000000};
    unsigned i;
    if (!machine_line(r, "mac started ", id, size))
        return 0;
    snprintf(path, sizeof(path), "%s/tree/build/machines/mac/runs/%s.times", r->f->work, id);
    for (i = 0; i < 200; i++) {
        if (fx_exists(path) && fx_has(path, "\nend "))
            return 1;
        /* interval: poll the Mac run's times file every 50 ms */
        nanosleep(&pause, NULL);
    }
    return 0;
}
static const char *mac_failed(struct release *r, char *record, size_t size)
{
    char path[4200];
    const char *id = "phase7-111111111111-mac-1029-000000";
    snprintf(path, sizeof(path), "tree/build/machines/mac/runs/%s.times", id);
    flow_put(r->f, path, "start 2026-10-29T00:00:00+0600\nrc=1\nend 2026-10-29T02:00:00+0600\n");
    snprintf(path, sizeof(path), "tree/build/machines/mac/runs/%s.console", id);
    flow_put(r->f, path, "run: result: failed\nrun: result: failed: i686:gnu x86_64:gnu\n");
    snprintf(record, size, "%smac started %s\n", r->built, id);
    return id;
}
static int mac_rows(struct release *r)
{
    struct fixture *f = r->f;
    char record[9000], id[200], last[400];
    const char *failed;
    int ok = 1;
    fx_remove(flow_path(f, "tree/build/machines"));
    flow_put(f, "tree/build/machines/mac/tree/Readme.md", "");
    flow_put(f, "tree/build/machines/mac/site/etude-site", "etude-site 2\n");
    snprintf(record, sizeof(record), "phase 7\nsha %s\nchosen %s\ntouched %s\n", C, T, T);
    run(r, &(struct release_row){.record = record,
                                 .board = "ready: yes " C,
                                 .runs = "none",
                                 .git = "yes",
                                 .machines = MAC});
    ok = check(r,
               f->status == 75 && count(flow_path(f, "state/record"), "^mac started ") == 1 &&
                   mac_ran(r, id, sizeof(id)) && !strncmp(id, "phase7-222222222222-mac-", 24) &&
                   fx_has(flow_path(f, "tree/build/machines/mac/run"), id) &&
                   count(flow_path(f, "calls"), "^dispatch ") == 1,
               "mac run starts beside the full run") &&
         ok;
    failed = mac_failed(r, record, sizeof(record));
    run(r, &(struct release_row){.record = record, .board = "ready: yes " H, .machines = MAC});
    snprintf(last, sizeof(last), "failed mac: run %s failed: i686:gnu, x86_64:gnu; filed as #40",
             failed);
    ok = check(r,
               f->status == 1 && last_is(flow_path(f, "state/record"), last) &&
                   fx_has(flow_path(f, "state/record"), "\nfiled mac #40 i686:gnu x86_64:gnu\n") &&
                   fx_has(flow_path(f, "calls.created"),
                          "--title Release tests fail on the Mac: i686:gnu, x86_64:gnu ") &&
                   !called(r, "^(machine write|testing )"),
               "mac failure is filed and ends the release") &&
         ok;
    snprintf(record + strlen(record), sizeof(record) - strlen(record),
             "result mac %s failure i686:gnu x86_64:gnu\nfiled mac #40 i686:gnu x86_64:gnu\n%s\n",
             failed, last);
    run(r, &(struct release_row){
               .record = record,
               .board = "ready: yes " C,
               .issues = "40 CLOSED Release tests fail on the Mac: i686:gnu, x86_64:gnu",
               .log = "Fix gnu\n\nCloses #40",
               .git = "yes",
               .machines = MAC});
    ok = check(r,
               f->status == 75 &&
                   fourth_is(flow_path(f, "state/record"), "again mac i686:gnu x86_64:gnu") &&
                   count(flow_path(f, "state/record"), "^full done ") == 1 &&
                   !fx_has(flow_path(f, "state/record"), "build done ") &&
                   mac_ran(r, id, sizeof(id)) && !called(r, "^dispatch ") &&
                   fx_has(flow_path(f, "calls.comments"),
                          "only these run again there: i686:gnu, x86_64:gnu"),
               "fixed mac failure runs only it again") &&
         ok;
    run(r, &(struct release_row){.record = record,
                                 .board = "ready: yes " C,
                                 .issues =
                                     "40 OPEN Release tests fail on the Mac: i686:gnu, x86_64:gnu",
                                 .log = "Fix gnu\n\nCloses #40",
                                 .git = "yes",
                                 .runs = "none",
                                 .machines = MAC});
    return check(r,
                 f->status == 75 && !fx_has(flow_path(f, "state/record"), "again ") &&
                     !fx_has(flow_path(f, "state/record"), "full done ") &&
                     mac_ran(r, id, sizeof(id)) && count(flow_path(f, "calls"), "^dispatch ") == 1,
                 "open mac failure runs every machine again") &&
           ok;
}
static int windows_rows(struct release *r)
{
    struct fixture *f = r->f;
    char *calls, *test, *start, *end;
    char last[400], id[200];
    int ok = 1;
    run(r, &(struct release_row){.record = r->head,
                                 .board = "ready: yes " H,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .windows = "status 0"});
    calls = fx_read(flow_path(f, "calls"));
    test = strstr(calls, "\nwsl test -e /mnt/d/o/phase7-111111111111-windows-");
    start = strstr(calls, "\nwindows cd D:/w || ");
    end = strstr(calls, "\nwsl touch /mnt/d/o/phase7-111111111111-windows-");
    ok = check(r,
               f->status == 75 && test && start && end && test < start && start < end &&
                   called(r, "^wsl exec /usr/local/libexec/rach2/etude-launch alone sh -c ") &&
                   called(r, "^scp -q -o BatchMode=yes .*/state/windows.bundle "
                             "testing:D:/o/release.bundle$") &&
                   called(r, "^windows export JOBS='30'$") &&
                   called(r, "^windows .* run routine-sequence phase_first_run D:/w "
                             "D:/o/phase7-111111111111-windows-[0-9-]+ both "
                             "phase7-111111111111-windows-[0-9-]+ C:/s .* whpx all > ") &&
                   count(flow_path(f, "state/record"),
                         "^windows done .* run phase7-111111111111-windows-") == 1 &&
                   called(r, "^testing "),
               "windows runs while WSL is held, then the build") &&
         ok;
    free(calls);
    run(r, &(struct release_row){.record = r->head,
                                 .board = "ready: yes " H,
                                 .git = "yes",
                                 .machines = WINDOWS,
                                 .windows = "run: result: failed: x86_64:agents\nstatus 1"});
    machine_line(r, "windows started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failed windows: run %s failed: x86_64:agents; filed as #40", id);
    ok = check(r,
               f->status == 1 && last_is(flow_path(f, "state/record"), last) &&
                   fx_has(flow_path(f, "state/record"), "\nfiled windows #40 x86_64:agents\n") &&
                   called(r, "^wsl touch ") && !called(r, "^testing "),
               "windows failure is filed and ends the release") &&
         ok;
    run(r, &(struct release_row){
               .record = r->head, .board = "ready: yes " H, .git = "yes", .machines = WINDOWS});
    return check(r,
                 f->status == 1 && count(flow_path(f, "calls"), "^windows cd ") == 3 &&
                     last_is(flow_path(f, "state/record"),
                             "failed windows: three runs on Windows ended without a status"),
                 "windows run without status runs three times") &&
           ok;
}
static int lenovo_rows(struct release *r)
{
    struct fixture *f = r->f;
    char *calls, *deploy, *accept, *commit, record[9000], last[400], id[200];
    int ok = 1;
    run(r, &(struct release_row){.record = r->tried,
                                 .board = "ready: yes " H,
                                 .writes = "yes",
                                 .accept = "pass",
                                 .machines = LENOVO,
                                 .native = "pass"});
    calls = fx_read(flow_path(f, "calls"));
    deploy = strstr(calls, "native-deploy run ");
    accept = strstr(calls, "accept 192.168.0.106\n");
    commit = strstr(calls, "machine write bootctl commit\n");
    ok = check(r,
               !f->status && archived(r, "7-11111111-") && deploy && accept && commit &&
                   deploy < accept && accept < commit &&
                   called(r, "^native-deploy run .*/state/wt/build/x86_64/images/../tests/native/"
                             "prepared 192.168.0.106 candidate .*/rach2/kernel .* /var/native "
                             "phase7-111111111111-lenovo-[0-9-]+ .* --hostpeer "
                             "build/tools/dev/native-hostpeer --peer 192.168.0.100 .* --purpose "
                             "phase_first_run --$") &&
                   count(r->retired, "^lenovo done ") == 1 &&
                   fx_has(flow_path(f, "calls.comments"),
                          "The release tests passed as well on the Lenovo, in the run "
                          "`phase7-111111111111-lenovo-"),
               "native cases run on the trial boot before acceptance") &&
         ok;
    free(calls);
    run(r, &(struct release_row){.record = r->tried,
                                 .board = "ready: yes " H,
                                 .accept = "pass",
                                 .machines = LENOVO,
                                 .native = "fail"});
    machine_line(r, "lenovo started ", id, sizeof(id));
    snprintf(last, sizeof(last), "failed lenovo: run %s failed: dirprobe.ext2; filed as #40", id);
    ok = check(r,
               f->status == 1 && last_is(flow_path(f, "state/record"), last) &&
                   fx_has(flow_path(f, "state/record"), "\nfiled lenovo #40 dirprobe.ext2\n") &&
                   !called(r, "^(accept|machine write)"),
               "native failure is filed and keeps the trial uncommitted") &&
         ok;
    snprintf(record, sizeof(record),
             "%slenovo started %s\nresult lenovo %s failure dirprobe.ext2\nfiled lenovo #40 "
             "dirprobe.ext2\n%s\n",
             r->tried, id, id, last);
    run(r,
        &(struct release_row){.record = record,
                              .board = "ready: yes " C,
                              .issues = "40 CLOSED Release tests fail on the Lenovo: dirprobe.ext2",
                              .log = "Fix dirprobe\n\nCloses #40",
                              .git = "yes",
                              .machines = LENOVO});
    return check(r,
                 f->status == 75 &&
                     fourth_is(flow_path(f, "state/record"), "again lenovo dirprobe.ext2") &&
                     count(flow_path(f, "state/record"), "^full done ") == 1 &&
                     !fx_has(flow_path(f, "state/record"), "trial done ") &&
                     called(r, "^testing .* ARCH=x86_64 make native-assets && "),
                 "fixed native failure runs only it again on the new install") &&
           ok;
}
int flow_release(struct fixture *f)
{
    struct release r;
    int ok = 1;
    if (!setup(&r, f))
        return 0;
    ok = closing_rows(&r) && ok;
    ok = new_candidate_rows(&r) && ok;
    ok = install_record_rows(&r) && ok;
    ok = rerun_rows(&r) && ok;
    ok = intermittent_build_rows(&r) && ok;
    ok = acceptance_rows(&r) && ok;
    ok = windows_rows(&r) && ok;
    ok = lenovo_rows(&r) && ok;
    ok = mac_rows(&r) && ok;
    fx_env("FLOW_RELEASE", NULL);
    return ok;
}
