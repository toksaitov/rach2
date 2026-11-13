/* flow_testing.c: command quoting, worktree sync and remote run reports. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static int ssh_input(void)
{
    char source[4096], target[4096], *all;
    FILE *f;
    char bytes[4096];
    size_t n;
    int fd;
    snprintf(source, sizeof(source), "%s/ssh-source.%ld", ev("FLOW_WORK"), (long)getpid());
    snprintf(target, sizeof(target), "%s/ssh-input.%ld", ev("FLOW_WORK"), (long)getpid());
    f = fopen(source, "wb");
    if (!f)
        return 0;
    while ((n = fread(bytes, 1, sizeof(bytes), stdin)))
        if (fwrite(bytes, 1, n, f) != n) {
            fclose(f);
            return 0;
        }
    if (ferror(stdin) || fclose(f))
        return 0;
    all = malloc(strlen(ev("HOSTHOME")) + 2);
    if (!all)
        return 0;
    sprintf(all, "%s/", ev("HOSTHOME"));
    if (!fx_replace(source, target, "/home/testing/", all)) {
        free(all);
        return 0;
    }
    free(all);
    unlink(source);
    fd = open(target, O_RDONLY);
    unlink(target);
    if (fd < 0 || dup2(fd, 0) < 0) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    close(fd);
    return 1;
}
static int ssh_mock(int argc, char **argv)
{
    const char *command;
    if (argc == 4 && !strcmp(argv[1], "-t") && !strcmp(argv[2], "testing")) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/../unreachable", ev("HOSTHOME"));
        if (fx_exists(path))
            return 255;
        if (!ssh_input())
            return 255;
        command = argv[3];
    } else {
        if (argc != 5 || strcmp(argv[1], "-o") || strcmp(argv[3], "testing"))
            return 255;
        command = argv[4];
        if (strlen(command) > 8191) {
            fprintf(stderr, "ssh fixture: cmd.exe refuses a line of %zu characters\n",
                    strlen(command));
            return 255;
        }
        if (strstr(command, "bash -s -- ")) {
            char b[1024];
            puts("held");
            fflush(stdout);
            while (read(0, b, sizeof(b)) > 0) {
            }
            return 0;
        }
        if (strpbrk(command, "\"%!")) {
            fprintf(stderr, "ssh fixture: cmd.exe misreads: %s\n", command);
            return 255;
        }
    }
    {
        char *a[] = {"/bin/bash", "-c", (char *)command, NULL};
        execv(a[0], a);
    }
    return 127;
}
static int rsync_mock(int argc, char **argv)
{
    char **a = calloc((size_t)argc + 1, sizeof(*a));
    int i, n = 1;
    if (!a)
        return 1;
    a[0] = "/usr/bin/rsync";
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("rsync  version 3.2.7  protocol version 31");
        free(a);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--rsync-path=", 13))
            continue;
        if (!strcmp(argv[i], "-e")) {
            i++;
            continue;
        }
        a[n++] = !strncmp(argv[i], "testing:", 8) ? argv[i] + 8 : argv[i];
    }
    a[n] = NULL;
    execv(a[0], a);
    free(a);
    return 127;
}
int flow_testing_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "ssh"))
        return ssh_mock(argc, argv);
    if (!strcmp(name, "rsync"))
        return rsync_mock(argc, argv);
    if (!strcmp(name, "wsl.exe")) {
        if (argc < 3 || (strcmp(argv[1], "-e") && strcmp(argv[1], "--")))
            return 2;
        execvp(argv[2], argv + 2);
        return 127;
    }
    if (!strcmp(name, "sh")) {
        argv[0] = "/bin/sh";
        execv(argv[0], argv);
        return 127;
    }
    if (!strcmp(name, "sleep")) {
        char path[4096];
        FILE *f;
        snprintf(path, sizeof(path), "%s/rach2/build/runs/%s.out", ev("HOSTHOME"),
                 ev("FLOW_WAIT_NAME"));
        f = fopen(path, "ab");
        if (!f)
            return 1;
        fprintf(f, "%s: first\n", ev("FLOW_WAIT_NAME"));
        return fclose(f) ? 1 : 0;
    }
    return 97;
}
struct testing {
    struct fixture *f;
    char script[4096], host[4096], name[100];
};
static int run(struct testing *t, const char *verb, const char *const extra[], const char *input)
{
    const char *a[40] = {"/bin/sh", t->script, verb};
    unsigned n = 3, i;
    for (i = 0; extra && extra[i] && n < 39; i++)
        a[n++] = extra[i];
    a[n] = NULL;
    return fx_run(t->f, a, t->f->work, NULL, input);
}
static int row(struct testing *t, const char *label, int status, const char *expected,
               const char *input, const char *const words[])
{
    struct fixture *f = t->f;
    if (!flow_put(f, "input", input))
        return 0;
    run(t, "run", words, flow_path(f, "input"));
    return fx_check(f, f->status == status && !strcmp(f->out, expected), label);
}
static int quoting(struct testing *t)
{
    struct fixture *f = t->f;
    char expected[5000], *command;
    int ok = 1;
    {
        const char *a[] = {"echo", "a", "b", NULL};
        ok = row(t, "words", 0, "a b\n", "", a) && ok;
    }
    {
        const char *a[] = {"printf '%s\\n' ok", NULL};
        ok = row(t, "single shell command", 0, "ok\n", "", a) && ok;
    }
    {
        const char *a[] = {"echo '%OS% !x! ^'", NULL};
        ok = row(t, "cmd.exe metacharacters", 0, "%OS% !x! ^\n", "", a) && ok;
    }
    {
        const char *a[] = {"printf '%s\\n' \"it's\" 'say \"hi\"' '(x)' 'a\\b' | tr -d '()'", NULL};
        ok = row(t, "quotes", 0, "it's\nsay \"hi\"\nx\na\\b\n", "", a) && ok;
    }
    {
        const char *a[] = {"echo \"$RACH2_QEMU_ACCEL $(pwd)\"", NULL};
        snprintf(expected, sizeof(expected), "tcg %s/copy\n", f->work);
        ok = row(t, "remote shell expansion", 0, expected, "", a) && ok;
    }
    {
        const char *a[] = {"read -r v; echo \"got $v\"", NULL};
        ok = row(t, "standard input", 0, "got line\n", "line\n", a) && ok;
    }
    {
        const char *a[] = {"(exit 7)", NULL};
        ok = row(t, "exit status", 7, "", "", a) && ok;
    }
    command = malloc(9100);
    if (!command)
        return 0;
    strcpy(command, "echo ");
    memset(command + 5, 'x', 9000);
    strcpy(command + 9005, " | awk '{ print length($0) }'");
    {
        const char *a[] = {command, NULL};
        ok = row(t, "long command crosses cmd.exe limit safely", 0, "9000\n", "", a) && ok;
    }
    free(command);
    return ok;
}
static int git(struct testing *t, const char *cwd, const char *const command[])
{
    const char *a[50] = {"/usr/bin/git", "-c", "init.defaultBranch=master", "-c",
                         "commit.gpgsign=false"};
    unsigned i, n = 5;
    for (i = 0; command[i] && n < 49; i++)
        a[n++] = command[i];
    a[n] = NULL;
    return !fx_run(t->f, a, cwd, NULL, NULL);
}
static int worktree(struct testing *t)
{
    struct fixture *f = t->f;
    char repo[4096], wt[4096], copy[4096], away[4096], script[4096], expected[300], *commit;
    int ok;
    snprintf(repo, sizeof(repo), "%s/repo", f->work);
    snprintf(wt, sizeof(wt), "%s/wt", f->work);
    snprintf(copy, sizeof(copy), "%s/wtcopy", f->work);
    snprintf(away, sizeof(away), "%s/away", f->work);
    fx_mkdir(repo);
    flow_put(f, "repo/.gitignore", "/tools/\n");
    flow_put(f, "repo/first", "one\n");
    {
        const char *a[] = {"init", "-q", NULL}, *b[] = {"add", ".gitignore", "first", NULL},
                   *c[] = {"commit", "-qm", "one", NULL},
                   *d[] = {"worktree", "add", "-q", "-b", "topic", wt, NULL};
        if (!git(t, repo, a) || !git(t, repo, b) || !git(t, repo, c) || !git(t, repo, d))
            return 0;
    }
    flow_put(f, "wt/second", "two\n");
    {
        const char *a[] = {"add", "second", NULL}, *b[] = {"commit", "-qm", "two", NULL},
                   *c[] = {"rev-parse", "HEAD", NULL};
        if (!git(t, wt, a) || !git(t, wt, b) || !git(t, wt, c))
            return 0;
    }
    commit = strdup(f->out);
    snprintf(expected, sizeof(expected), "%sHEAD:second\n", commit);
    free(commit);
    snprintf(script, sizeof(script), "%s/tools/dev/testing.sh", wt);
    /* testing.sh holds its background processes with these. */
    if (!fx_copy(t->script, script) ||
        !flow_copy(f, "tools/flow/hold.pl", "wt/tools/flow/hold.pl") ||
        !flow_copy(f, "tools/flow/bound.pl", "wt/tools/flow/bound.pl") ||
        !fx_copy(flow_path(f, "wt/.git"), flow_path(f, "wtcopy/.git")))
        return 0;
    fx_env("RACH2_TESTING_COPY", copy);
    {
        const char *a[] = {"/bin/sh", script, "sync", NULL};
        ok = !fx_run(f, a, wt, NULL, NULL);
    }
    if (rename(repo, away))
        ok = 0;
    if (ok) {
        char output[1000];
        const char *a[] = {"rev-parse", "HEAD", NULL}, *b[] = {"grep", "-l", "two", "HEAD", NULL},
                   *c[] = {"status", "--porcelain", NULL};
        if (!git(t, copy, a))
            return 0;
        snprintf(output, sizeof(output), "%s", f->out);
        if (!git(t, copy, b))
            return 0;
        strncat(output, f->out, sizeof(output) - strlen(output) - 1);
        if (!git(t, copy, c))
            return 0;
        strncat(output, f->out, sizeof(output) - strlen(output) - 1);
        ok = !strcmp(output, expected);
    }
    fx_env("RACH2_TESTING_COPY", flow_path(f, "copy"));
    return fx_check(f, ok, "synced worktree retains private repository after original moves");
}
struct waiting {
    const char *label, *expected, *results, *seen;
    int status, running, fresh;
};
static int wait_row(struct testing *t, const struct waiting *spec)
{
    const char *label = spec->label, *expected = spec->expected, *results = spec->results,
               *seen = spec->seen;
    int status = spec->status, running = spec->running, fresh = spec->fresh;
    struct fixture *f = t->f;
    char path[4096], env[5000], *script;
    int ok;
    struct fx_child process;
    snprintf(path, sizeof(path), "%s/rach2/build/runs/%s", t->host, t->name);
    fx_remove(path);
    snprintf(path, sizeof(path), "%s/rach2/build/runs/%s.out", t->host, t->name);
    fx_remove(path);
    if (running) {
        char file[200];
        const char *a[] = {"sh", file, "--hold", NULL};
        snprintf(file, sizeof(file), "%s.sh", t->name);
        snprintf(env, sizeof(env), "FLOW_WAIT_READY=%s", flow_path(f, "wait-ready"));
        fx_remove(flow_path(f, "wait-ready"));
        const char *overrides[] = {env, NULL};
        if (!fx_start(f, &process, a, f->work, overrides, NULL) ||
            !fx_until(&process, flow_path(f, "wait-ready")))
            return 0;
    }
    if (fresh) {
        snprintf(path, sizeof(path), "%s/rach2/build/runs/%s/log", t->host, t->name);
        fx_text(path, "");
    }
    if (results) {
        snprintf(path, sizeof(path), "%s/rach2/build/runs/%s.out", t->host, t->name);
        fx_text(path, results);
    }
    {
        const char *a[] = {t->name, seen, NULL};
        run(t, "wait", a, NULL);
    }
    script = strdup(f->out);
    ok = f->status == status && !strcmp(script, expected);
    free(script);
    if (running) {
        fx_stop(&process, SIGTERM);
        fx_wait(f, &process);
    }
    return fx_check(f, ok, label);
}
static int waits(struct testing *t)
{
    struct fixture *f = t->f;
    char expected[1000], results[1000], path[9000];
    int ok = 1;
    fx_env("FLOW_WAIT_NAME", t->name);
    fx_mkdir(flow_path(f, "waitbin"));
    if (!fx_copy(f->self, flow_path(f, "waitbin/sleep")) || !fx_mock(f, "sh"))
        return 0;
    snprintf(path, sizeof(path), "%s/waitbin:%s:/usr/bin:/bin:/usr/sbin:/sbin", f->work, f->bin);
    fx_env("PATH", path);
    snprintf(expected, sizeof(expected), "%s: first\ntesting: %s: running, 1 lines\n", t->name,
             t->name);
    ok = wait_row(t, &(struct waiting){"wait first result", expected, NULL, NULL, 0, 1, 1}) && ok;
    snprintf(expected, sizeof(expected),
             "testing: %s: stalled, nothing under %s/rach2/build/runs/%s "
             "changed for 15 minutes, 0 lines\n",
             t->name, FLOW_OWNER_HOME, t->name);
    ok = wait_row(t, &(struct waiting){"wait stalled", expected, NULL, NULL, 0, 1, 0}) && ok;
    snprintf(expected, sizeof(expected), "%s: two\ntesting: %s: ended, 2 lines\n", t->name,
             t->name);
    snprintf(results, sizeof(results), "%s: one\n%s: two\n", t->name, t->name);
    ok = wait_row(t, &(struct waiting){"wait ended with unseen lines", expected, results, "1", 0, 0,
                                       0}) &&
         ok;
    flow_put(f, "host/unreachable", "");
    snprintf(results, sizeof(results), "%s: one\n", t->name);
    ok = wait_row(t, &(struct waiting){"wait unreachable", "", results, NULL, 1, 1, 1}) && ok;
    return ok;
}
int flow_testing(struct fixture *f)
{
    struct testing t;
    char runs[4200];
    int ok = 1;
    memset(&t, 0, sizeof(t));
    t.f = f;
    snprintf(t.script, sizeof(t.script), "%s/tree/tools/dev/testing.sh", f->work);
    snprintf(t.host, sizeof(t.host), "%s/host/home", f->work);
    snprintf(runs, sizeof(runs), "%s/rach2/build/runs", t.host);
    snprintf(t.name, sizeof(t.name), "testingwait%ld", (long)getpid());
    /* testing.sh holds its background processes with hold.pl and bounds
     * its stops with bound.pl, here and, through the sync, in the copy. */
    if (!fx_copy(flow_source(f, "tools/dev/testing.sh"), t.script) ||
        !flow_copy(f, "tools/flow/hold.pl", "tree/tools/flow/hold.pl") ||
        !flow_copy(f, "tools/flow/bound.pl", "tree/tools/flow/bound.pl") || !fx_mock(f, "ssh") ||
        !fx_mock(f, "rsync") || !fx_mock(f, "wsl.exe") || !fx_mkdir(runs) ||
        !fx_mkdir(flow_path(f, "copy")))
        return 0;
    fx_env("FLOW_TESTING", "1");
    fx_env("HOSTHOME", t.host);
    fx_env("RACH2_TESTING_COPY", flow_path(f, "copy"));
    fx_env("GIT_AUTHOR_NAME", "test");
    fx_env("GIT_AUTHOR_EMAIL", "test@example.com");
    fx_env("GIT_COMMITTER_NAME", "test");
    fx_env("GIT_COMMITTER_EMAIL", "test@example.com");
    fx_env("GIT_DIR", NULL);
    fx_env("GIT_WORK_TREE", NULL);
    fx_env("GIT_INDEX_FILE", NULL);
    fx_env("GIT_COMMON_DIR", NULL);
    ok = quoting(&t) && ok;
    ok = worktree(&t) && ok;
    ok = waits(&t) && ok;
    fx_env("FLOW_TESTING", NULL);
    return ok;
}
