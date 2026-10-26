/* flow_progress.c: complete and failed board reads of the progress product. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static void data(struct fixture *f, const char *name, const char *text)
{
    if (!flow_put(f, name, text))
        f->failed = 1;
}
static void issues(struct fixture *f, int blocked, int assigned, int extra, int ciassigned,
                   int intermittent)
{
    char v[6000];
    snprintf(v, sizeof(v),
             "[[{\"number\":1,\"title\":\"task "
             "1\",\"created_at\":\"2026-10-21T04:00:00Z\",\"labels\":[],\"assignees\":%s,\"issue_"
             "dependencies_summary\":{\"blocking\":2,\"blocked_by\":%d}}],[{\"number\":2,\"title\":"
             "\"task "
             "2\",\"created_at\":\"2026-10-21T05:00:00Z\",\"labels\":[],\"assignees\":%s,\"issue_"
             "dependencies_summary\":{\"blocking\":5,\"blocked_by\":0}}%s]]",
             assigned ? "[{\"login\":\"worker\"}]" : "[]", blocked,
             assigned ? "[{\"login\":\"worker\"}]" : "[]",
             extra == 90 ? ",{\"number\":90,\"title\":\"task "
                           "90\",\"created_at\":\"2026-10-21T00:00:00Z\",\"labels\":[],"
                           "\"assignees\":[{\"login\":\"worker\"}],\"issue_dependencies_summary\":{"
                           "\"blocking\":20,\"blocked_by\":0}}"
                         : "");
    if (extra == 4) {
        char *end = strrchr(v, ']');
        if (end && end > v)
            end[-1] = 0;
        snprintf(
            v + strlen(v), sizeof(v) - strlen(v),
            ",{\"number\":4,\"title\":\"task "
            "4\",\"created_at\":\"2026-10-21T11:00:00Z\",\"labels\":[{\"name\":\"ci\"}%s],"
            "\"assignees\":%s,\"issue_dependencies_summary\":{\"blocking\":0,\"blocked_by\":0}}]]",
            intermittent ? ",{\"name\":\"intermittent\"}" : "",
            ciassigned ? "[{\"login\":\"worker\"}]" : "[]");
    }
    data(f, "opened", v);
}
static void base(struct fixture *f)
{
    data(f, "phase", "589\n");
    data(f, "members", "1\tOPEN\t-\tbug\ttask 1\n2\tOPEN\t-\tbug\ttask 2\n");
    issues(f, 0, 1, 0, 0, 0);
    data(
        f, "comments",
        "[[{\"body\":\"phase: started at "
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created_at\":\"2026-10-21T06:00:00Z\"}],[]]");
    data(f, "timeline", "[[],[]]");
    fx_remove(flow_path(f, "fail-opened"));
    fx_remove(flow_path(f, "fail-timeline"));
}
static int report(struct fixture *f, const char *label, const char *want, const char *absent,
                  int error)
{
    const char *a[] = {"python3", flow_path(f, "progress.py"), NULL};
    fx_run(f, a, f->work, NULL, NULL);
    return fx_check(f,
                    (f->status != 0) == error && (!want || strstr(f->out, want)) &&
                        (!absent || !strstr(f->out, absent)) &&
                        (!error || strstr(f->err, "progress: unknown: ")),
                    label);
}
static int member_rows(struct fixture *f)
{
    int ok = 1;
    base(f);
    ok = report(f, "phase-idle", "incident: phase #589", NULL, 0) && ok;
    ok = report(f, "dependency", "incident: member #1", NULL, 0) && ok;
    ok = report(f, "dependency2", "incident: member #2", NULL, 0) && ok;
    ok = report(f, "order",
                "member #2: blocks 5 open issues; worker; 21600s since assignment or phase start; "
                "task 2\nprogress: member #1",
                NULL, 0) &&
         ok;
    base(f);
    issues(f, 1, 1, 0, 0, 0);
    ok = report(f, "prerequisite", "waits on 1 open prerequisites", "incident: member #1", 0) && ok;
    base(f);
    data(f, "timeline",
         "[[{\"event\":\"assigned\",\"assignee\":{\"login\":\"worker\"},\"created_at\":\"2026-10-"
         "21T11:59:00Z\"}],[]]");
    ok = report(f, "new-assignment", "60s since assignment", "incident: member", 0) && ok;
    base(f);
    issues(f, 0, 0, 0, 0, 0);
    ok = report(f, "unclaimed-member", "unassigned; no open prerequisite", "incident: member", 0) &&
         ok;
    base(f);
    data(f, "members",
         "1\tOPEN\t-\tbug\ttask 1\n2\tOPEN\t-\tbug\ttask "
         "2\n3\tCLOSED\t2026-10-21T11:59:00Z\tbug\ttask 3\n");
    ok = report(f, "closure", "last closed #3; 60s since a member", "incident: phase", 0) && ok;
    data(
        f, "comments",
        "[[{\"body\":\"phase: started at "
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"created_at\":\"2026-10-21T11:59:00Z\"}],[]]");
    data(f, "members",
         "1\tOPEN\t-\tbug\ttask 1\n2\tOPEN\t-\tbug\ttask "
         "2\n3\tCLOSED\t2026-10-21T04:00:00Z\tbug\ttask 3\n");
    ok = report(f, "new-phase", "60s since a member", "incident:", 0) && ok;
    base(f);
    data(f, "members", "3\tCLOSED\t2026-10-21T04:00:00Z\tbug\ttask 3\n");
    ok = report(f, "nothing-left", NULL, "incident:", 0) && ok;
    base(f);
    issues(f, 0, 1, 90, 0, 0);
    ok = report(f, "nonmember", NULL, "member #90", 0) && ok;
    return ok;
}
static int failure_rows(struct fixture *f)
{
    int ok = 1;
    base(f);
    issues(f, 0, 1, 4, 0, 0);
    ok = report(f, "unassigned", "unassigned for 3600s", NULL, 0) && ok;
    ok = report(f, "unassigned-incident", "incident: failure #4", NULL, 0) && ok;
    data(f, "timeline",
         "[[{\"event\":\"unassigned\",\"created_at\":\"2026-10-21T11:30:00Z\"}],[]]");
    ok = report(f, "half-hour", "unassigned for 1800s", NULL, 0) && ok;
    ok = report(f, "half-hour-incident", "incident: failure #4", NULL, 0) && ok;
    data(f, "timeline",
         "[[{\"event\":\"unassigned\",\"created_at\":\"2026-10-21T11:30:01Z\"}],[]]");
    ok = report(f, "recent-handback", "unassigned for 1799s", "incident: failure", 0) && ok;
    base(f);
    issues(f, 0, 1, 4, 0, 1);
    ok = report(f, "intermittent", NULL, "failure #4", 0) && ok;
    base(f);
    issues(f, 0, 1, 4, 1, 0);
    ok = report(f, "assigned", NULL, "failure #4", 0) && ok;
    base(f);
    issues(f, 0, 1, 4, 0, 0);
    data(f, "phase", "");
    ok = report(f, "no-phase", "no phase is In progress", NULL, 0) && ok;
    ok = report(f, "no-phase-ci", "incident: failure #4", NULL, 0) && ok;
    base(f);
    data(f, "fail-opened", "");
    ok = report(f, "failed-read", NULL, NULL, 1) && ok;
    base(f);
    issues(f, 0, 1, 4, 0, 0);
    data(f, "fail-timeline", "");
    ok = report(f, "failed-timeline", NULL, NULL, 1) && ok;
    base(f);
    data(f, "comments", "[[],[]]");
    ok = report(f, "missing-start", NULL, NULL, 1) && ok;
    base(f);
    data(f, "opened", "[[],[]]");
    ok = report(f, "changed-member", NULL, NULL, 1) && ok;
    return ok;
}
int flow_progress(struct fixture *f)
{
    char old[300], replacement[100];
    struct tm tm;
    time_t fixed;
    int ok;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 126;
    tm.tm_mon = 9;
    tm.tm_mday = 21;
    tm.tm_hour = 12;
    fx_env("TZ", "UTC0");
    tzset();
    fixed = mktime(&tm);
    strcpy(old, "report(datetime.datetime.now(datetime.timezone.utc).timestamp())");
    snprintf(replacement, sizeof(replacement), "report(%lld)", (long long)fixed);
    if (!fx_replace(flow_source(f, "tools/flow/progress.py"), flow_path(f, "progress.py"), old,
                    replacement) ||
        !fx_mock(f, "gh") || !fx_mock(f, "sh"))
        return 0;
    fx_env("FLOW_PROGRESS", "1");
    ok = member_rows(f);
    ok = failure_rows(f) && ok;
    fx_env("FLOW_PROGRESS", NULL);
    return ok;
}
int flow_progress_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/'), *file = NULL, *work = getenv("FLOW_WORK");
    char p[4096], *s;
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "sh") && argc == 3 && !strcmp(argv[2], "current"))
        file = "phase";
    if (!strcmp(name, "sh") && argc == 4 && !strcmp(argv[2], "members") && !strcmp(argv[3], "589"))
        file = "members";
    if (!strcmp(name, "gh") && argc == 5 && !strcmp(argv[1], "api") &&
        !strcmp(argv[2], "--paginate") && !strcmp(argv[3], "--slurp")) {
        const char *issues = "repos/toksaitov/rach2/issues", *timeline = "/timeline?per_page=100";
        size_t n = strlen(argv[4]);
        if (!strcmp(argv[4], "repos/toksaitov/rach2/issues?state=open&per_page=100"))
            file = "opened";
        else if (!strcmp(argv[4], "repos/toksaitov/rach2/issues/589/comments?per_page=100"))
            file = "comments";
        else if (!strncmp(argv[4], issues, strlen(issues)) && n > strlen(timeline) &&
                 !strcmp(argv[4] + n - strlen(timeline), timeline))
            file = "timeline";
    }
    if (!file || !work) {
        fprintf(stderr, "progress mock: unexpected command\n");
        return 97;
    }
    snprintf(p, sizeof(p), "%s/fail-%s", work, file);
    if (fx_exists(p))
        return 1;
    snprintf(p, sizeof(p), "%s/%s", work, file);
    s = fx_read(p);
    if (!s)
        return 1;
    fputs(s, stdout);
    free(s);
    return 0;
}
