/* flow_cleanup.c: preserve unlanded work, evidence and active files. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "flow_check.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int g(struct fixture *f, const char *cwd, ...)
{
    const char *a[80] = {"/usr/bin/git",
                         "-c",
                         "user.name=cleanup-test",
                         "-c",
                         "user.email=cleanup-test@example.invalid",
                         "-c",
                         "commit.gpgsign=false",
                         "-c",
                         "core.hooksPath=/dev/null"},
               *v;
    unsigned n = 9;
    va_list ap;
    va_start(ap, cwd);
    while ((v = va_arg(ap, const char *)) && n < 79)
        a[n++] = v;
    va_end(ap);
    a[n] = NULL;
    return !fx_run(f, a, cwd, NULL, NULL);
}
#define G(...)                             \
    do {                                   \
        if (!g(f, cwd, __VA_ARGS__, NULL)) \
            return 0;                      \
    } while (0)
static int age_tree(const char *path, time_t when)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char p[4096];
    struct timespec t[2] = {{0, 0}, {0, 0}};
    int ok = 1;
    if (lstat(path, &st))
        return 0;
    if (S_ISDIR(st.st_mode)) {
        d = opendir(path);
        if (!d)
            return 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
            if (!age_tree(p, when))
                ok = 0;
        }
        closedir(d);
    }
    t[0].tv_sec = t[1].tv_sec = when;
    return !utimensat(AT_FDCWD, path, t, AT_SYMLINK_NOFOLLOW) && ok;
}
static int newer(const char *path, struct timespec stamp)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char p[4096];
    int found = 0;
    if (lstat(path, &st))
        return 1;
#ifdef __APPLE__
    struct timespec modified = st.st_mtimespec;
#else
    struct timespec modified = st.st_mtim;
#endif
    if (modified.tv_sec > stamp.tv_sec ||
        (modified.tv_sec == stamp.tv_sec && modified.tv_nsec > stamp.tv_nsec))
        return 1;
    if (!S_ISDIR(st.st_mode))
        return 0;
    d = opendir(path);
    if (!d)
        return 1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        if (newer(p, stamp)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}
static int link_ready(struct fx_child *child, const char *which, const char *wanted)
{
    char path[100], target[4096];
    ssize_t n;
    snprintf(path, sizeof(path), "/proc/%ld/%s", (long)child->pid, which);
    for (;;) {
        n = readlink(path, target, sizeof(target) - 1);
        if (n >= 0) {
            target[n] = 0;
            if (!strcmp(target, wanted))
                return 1;
        }
        if (fx_poll(child))
            return 0;
        fx_pause();
    }
}
static int copy_clone(struct fixture *f, const char *name, const char *ref)
{
    char target[4096], origin[4096];
    snprintf(target, sizeof(target), "%s/home.noindex/%s", f->work, name);
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    return g(f, f->work, "clone", "-q", "--no-hardlinks", origin, target, NULL) &&
           g(f, target, "checkout", "-q", "--detach", ref ? ref : "origin/master", NULL);
}
/**
 * Make the fixture's origin, a bare repository whose master has the commit
 * of branch 1-picked picked onto it, from the repository `seed`, and the
 * clone rach2-stale of that branch.
 */
static int leftovers_origin(struct fixture *f)
{
    char cwd[4096], origin[4096], p[4096];
    fx_remove(flow_path(f, "origin.git"));
    fx_remove(flow_path(f, "seed"));
    fx_remove(flow_path(f, "home.noindex"));
    fx_mkdir(flow_path(f, "home.noindex"));
    fx_mkdir(flow_path(f, "seed"));
    snprintf(cwd, sizeof(cwd), "%s/seed", f->work);
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    G("init", "-q", "--bare", "-b", "master", origin);
    G("init", "-q", "-b", "master");
    snprintf(p, sizeof(p), "%s/.gitignore", cwd);
    fx_text(p, "build/\n*.wad\n.claude/worktrees/\n");
    snprintf(p, sizeof(p), "%s/a", cwd);
    fx_text(p, "a\n");
    G("add", ".gitignore", "a");
    G("commit", "-q", "-m", "Add a");
    G("remote", "add", "origin", origin);
    G("switch", "-q", "-c", "1-picked");
    snprintf(p, sizeof(p), "%s/x", cwd);
    fx_text(p, "x\n");
    G("add", "x");
    G("commit", "-q", "-m", "Add x");
    G("push", "-q", "origin", "master", "1-picked");
    if (!copy_clone(f, "rach2-stale", "origin/1-picked"))
        return 0;
    G("switch", "-q", "master");
    snprintf(p, sizeof(p), "%s/m", cwd);
    fx_text(p, "m\n");
    G("add", "m");
    G("commit", "-q", "-m", "Add m");
    G("cherry-pick", "1-picked");
    G("push", "-q", "origin", "master");
    return 1;
}
static int leftovers_setup(struct fixture *f)
{
    char cwd[4096], origin[4096], target[4096], p[4096];
    unsigned i;
    const char *copies[] = {"landed",   "dirty", "untracked", "unique",  "ignored",  "cited",
                            "evidence", "young", "named",     "stashed", "branched", "linked"};
    const char *failed[] = {"100.machine.a", "30.machine.d",  "200.machine.b",
                            "300.machine.c", "400.machine.e", "500.machine.f"};
    const char *files[] = {"tmp.C160l0g000",
                           "rach2-trial.Tr1al0/sub/f",
                           "flow/log",
                           ".hidden",
                           "rach2-notes",
                           "tmp.W4dW4dW4dW/doom1.wad",
                           "tmp.K3ysK3ysK3/id_ed25519",
                           "tmp.P4kgP4kgP4/pkg.tar.gz",
                           "tmp.Src0Src0Sr/.git/HEAD",
                           "tmp.Y0ungY0ung",
                           "tmp.C1tedC1ted",
                           "notes.c",
                           "plain/x.c",
                           "rach2-run.backup/f"};
    if (!leftovers_origin(f))
        return 0;
    snprintf(cwd, sizeof(cwd), "%s/seed", f->work);
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    snprintf(target, sizeof(target), "%s/home.noindex/rach2", f->work);
    G("clone", "-q", "--no-hardlinks", origin, target);
    for (i = 0; i < 12; i++) {
        char name[100];
        snprintf(name, sizeof(name), "rach2-%s", copies[i]);
        if (!copy_clone(f, name, NULL))
            return 0;
    }
    flow_put(f, "home.noindex/rach2-landed/build/out", "");
    flow_put(f, "home.noindex/rach2-landed.sync.lock", "");
    flow_put(f, "home.noindex/rach2-landed.sync.owner", "");
    flow_put(f,
             "home.noindex/rach2-dirty/build/tests/failed/"
             "tests.i686.20261020T000000Z.600.machine.g.r0/log",
             "");
    flow_put(f,
             "home.noindex/rach2-evidence/build/tests/failed/"
             "tests.i686.20261020T000000Z.700.machine.h.r0/log",
             "");
    flow_put(f, "home.noindex/rach2-dirty/a", "changed\n");
    flow_put(f, "home.noindex/rach2-untracked/new", "");
    flow_put(f, "home.noindex/rach2-unique/u", "u\n");
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2-unique", f->work);
    G("add", "u");
    G("commit", "-q", "-m", "Add u");
    flow_put(f, "home.noindex/rach2-ignored/game.wad", "");
    flow_put(f, "home.noindex/rach2-stashed/a", "stashed\n");
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2-stashed", f->work);
    G("stash", "-q");
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2-branched", f->work);
    G("switch", "-q", "-c", "side");
    flow_put(f, "home.noindex/rach2-branched/s", "s\n");
    G("add", "s");
    G("commit", "-q", "-m", "Add s");
    G("checkout", "-q", "--detach", "origin/master");
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2-linked", f->work);
    fx_remove(flow_path(f, "linked"));
    G("worktree", "add", "-q", "--detach", flow_path(f, "linked"));
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2", f->work);
    G("worktree", "add", "-q", "--detach", ".claude/worktrees/clean");
    G("worktree", "add", "-q", "--detach", ".claude/worktrees/locked");
    G("worktree", "lock", ".claude/worktrees/locked");
    for (i = 0; i < 6; i++) {
        snprintf(p, sizeof(p),
                 "home.noindex/rach2/build/tests/failed/tests.i686.20261020T000000Z.%s.r0/log",
                 failed[i]);
        flow_put(f, p, "");
    }
    for (i = 0; i < 14; i++) {
        snprintf(p, sizeof(p), "home.noindex/%s", files[i]);
        flow_put(f, p, "");
    }
    fx_mkdir(flow_path(f, "home.noindex/tmp.Bu5yBu5yBu"));
    symlink(flow_path(f, "home.noindex/rach2-trial.Tr1al0"),
            flow_path(f, "home.noindex/tmp.L1nkL1nkL1"));
    if (!age_tree(flow_path(f, "home.noindex"), time(NULL) - 172800))
        return 0;
    snprintf(p, sizeof(p), "%s/home.noindex/rach2-young/a", f->work);
    if (!age_tree(p, time(NULL)) || !flow_put(f,
                                              "home.noindex/rach2/build/tests/failed/"
                                              "tests.i686.20261020T000000Z.400.machine.e.r0/now",
                                              ""))
        return 0;
    return age_tree(flow_path(f, "home.noindex/tmp.Y0ungY0ung"), time(NULL));
}
static int leftovers_sweep(struct fixture *f, int dry, int text)
{
    const char *source = flow_source(f, "tools/flow/leftovers.pl");
    char *script = fx_read(source), *input, *master;
    int result;
    char cwd[4096];
    const char *cites =
        "\n__END__\nThe kept workspace is "
        "~toksaitov-d/rach2/build/tests/failed/tests.i686.20261020T000000Z.200.machine.b.r0 on the "
        "host.\nThe run tests.i686.20261020T000000Z.300 failed twice, and the copy "
        "/home/testing/rach2-cited has the fix.\nIts evidence is "
        "tests.i686.20261020T000000Z.700.machine.h.r0 in a copy.\nIts output is in "
        "tmp.C1tedC1ted.\n";
    if (!script)
        return 0;
    input = malloc(strlen(script) + strlen(cites) + 1);
    if (!input) {
        free(script);
        return 0;
    }
    strcpy(input, script);
    if (text)
        strcat(input, cites);
    flow_put(f, "sweep-input", input);
    free(input);
    free(script);
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2", f->work);
    if (!g(f, cwd, "rev-parse", "origin/master", NULL))
        return 0;
    master = strdup(f->out);
    master[strcspn(master, "\r\n")] = 0;
    {
        const char *a[] = {"/usr/bin/perl",
                           "-",
                           dry ? "-n" : "-l",
                           dry ? "-l" : "-m",
                           dry ? "-m" : master,
                           dry ? master : "-k",
                           dry ? "-k" : flow_path(f, "home.noindex/rach2-named"),
                           dry ? flow_path(f, "home.noindex/rach2-named") : "-k",
                           dry ? "-k" : flow_path(f, "home.noindex/flow"),
                           dry ? flow_path(f, "home.noindex/flow") : "86400",
                           dry ? "86400" : flow_path(f, "home.noindex"),
                           dry ? flow_path(f, "home.noindex") : NULL,
                           NULL};
        result = fx_run(f, a, f->work, NULL, flow_path(f, "sweep-input"));
    }
    free(master);
    return result == 0;
}
static int exists(struct fixture *f, const char *name)
{
    return fx_exists(flow_path(f, name));
}
static int is_link(struct fixture *f, const char *name)
{
    struct stat st;
    return !lstat(flow_path(f, name), &st) && S_ISLNK(st.st_mode);
}
static int leftovers_previews(struct fixture *f)
{
    int ok = 1;
    if (!leftovers_setup(f))
        return 0;
    ok = fx_check(f,
                  !leftovers_sweep(f, 0, 0) && exists(f, "home.noindex/rach2-landed") &&
                      exists(f, "home.noindex/tmp.C160l0g000") &&
                      exists(f, "home.noindex/rach2/build/tests/failed/"
                                "tests.i686.20261020T000000Z.100.machine.a.r0"),
                  "no text removes nothing") &&
         ok;
    if (!leftovers_setup(f))
        return 0;
    ok = fx_check(f,
                  leftovers_sweep(f, 1, 1) && exists(f, "home.noindex/rach2-landed") &&
                      exists(f, "home.noindex/tmp.C160l0g000") &&
                      fx_regex(f->out, "leftovers: would remove copy .*/rach2-landed: [0-9]+ KiB"),
                  "dry run removes nothing") &&
         ok;
    if (!leftovers_setup(f))
        return 0;
    return ok;
}
int flow_leftovers(struct fixture *f)
{
    unsigned i;
    int ok = 1;
    struct timespec stamp;
    struct fx_child busy[2];
    char cwd[4096];
    const char *keep[] = {
        "rach2-dirty/a",
        "rach2-untracked/new",
        "rach2-unique/u",
        "rach2-ignored/game.wad",
        "rach2-cited",
        "rach2-evidence/build/tests/failed/tests.i686.20261020T000000Z.700.machine.h.r0",
        "rach2-young",
        "rach2-named",
        "rach2-stashed",
        "rach2-branched",
        "rach2-linked",
        "rach2/.claude/worktrees/locked",
        "rach2/build/tests/failed/tests.i686.20261020T000000Z.200.machine.b.r0",
        "rach2/build/tests/failed/tests.i686.20261020T000000Z.300.machine.c.r0",
        "rach2/build/tests/failed/tests.i686.20261020T000000Z.400.machine.e.r0",
        "rach2/build/tests/failed/tests.i686.20261020T000000Z.500.machine.f.r0",
        ".hidden",
        "rach2-notes",
        "flow/log",
        "tmp.W4dW4dW4dW/doom1.wad",
        "tmp.K3ysK3ysK3/id_ed25519",
        "tmp.P4kgP4kgP4/pkg.tar.gz",
        "tmp.Src0Src0Sr/.git/HEAD",
        "tmp.Y0ungY0ung",
        "tmp.C1tedC1ted",
        "notes.c",
        "plain/x.c",
        "rach2-run.backup/f",
        "tmp.L1nkL1nkL1",
        "tmp.Bu5yBu5yBu"};
    const char *gone[] = {
        "rach2-landed",
        "rach2-landed.sync.lock",
        "rach2-landed.sync.owner",
        "rach2-stale",
        "rach2/.claude/worktrees/clean",
        "rach2/build/tests/failed/tests.i686.20261020T000000Z.100.machine.a.r0",
        "rach2/build/tests/failed/tests.i686.20261020T000000Z.30.machine.d.r0",
        "rach2-dirty/build/tests/failed/tests.i686.20261020T000000Z.600.machine.g.r0",
        "tmp.C160l0g000",
        "rach2-trial.Tr1al0"};
    if (!leftovers_previews(f))
        return 0;
    {
        const char *a[] = {f->self, "--hold", NULL};
        const char *dirs[] = {
            "home.noindex/rach2/build/tests/failed/tests.i686.20261020T000000Z.500.machine.f.r0",
            "home.noindex/tmp.Bu5yBu5yBu"};
        for (i = 0; i < 2; i++)
            if (!fx_start(f, &busy[i], a, flow_path(f, dirs[i]), NULL, NULL) ||
                !link_ready(&busy[i], "cwd", flow_path(f, dirs[i])))
                return 0;
    }
    if (clock_gettime(CLOCK_REALTIME, &stamp))
        return 0;
    ok = fx_check(f, leftovers_sweep(f, 0, 1), "sweep status") && ok;
    for (i = 0; i < 2; i++) {
        fx_stop(&busy[i], SIGTERM);
        fx_wait(f, &busy[i]);
    }
    for (i = 0; i < 30; i++) {
        char name[4096];
        snprintf(name, sizeof(name), "home.noindex/%s", keep[i]);
        ok = fx_check(f, exists(f, name), keep[i]) && ok;
    }
    for (i = 0; i < 10; i++) {
        char name[4096];
        snprintf(name, sizeof(name), "home.noindex/%s", gone[i]);
        ok = fx_check(f, !exists(f, name), gone[i]) && ok;
    }
    ok = fx_check(f,
                  !newer(flow_path(f, "home.noindex/rach2-unique"), stamp) &&
                      !newer(flow_path(f, "home.noindex/rach2-ignored"), stamp) &&
                      !newer(flow_path(f, "home.noindex/rach2-dirty/.git"), stamp),
                  "kept copies untouched") &&
         ok;
    snprintf(cwd, sizeof(cwd), "%s/home.noindex/rach2", f->work);
    if (!g(f, cwd, "worktree", "list", NULL))
        return 0;
    ok = fx_check(f, !strstr(f->out, "/clean"), "clean worktree registration removed") && ok;
    return fx_check(f, is_link(f, "home.noindex/tmp.L1nkL1nkL1"), "loose link kept as a link") &&
           ok;
}
static int file_at(const char *dir, const char *name, const char *text)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return fx_text(path, text);
}
static int sparse_at(struct fixture *f, const char *name, unsigned mib)
{
    const char *path = flow_path(f, name);
    int fd;
    char parent[4096], *slash;
    snprintf(parent, sizeof(parent), "%s", path);
    slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        fx_mkdir(parent);
    }
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return 0;
    if (ftruncate(fd, (off_t)mib * 1048576)) {
        close(fd);
        return 0;
    }
    return !close(fd);
}
static int tidy_files(struct fixture *f, const char *base)
{
    char cwd[4096], source[4096], target[4096];
    unsigned i;
    const char *worktrees[] = {"clean", "dirty", "ignored", "locked", "unique"};
    const char *scratch[] = {"rach2-old.Ab3dEf/sub/f",
                             "rach2-file.Fi1eFi",
                             "rach2-ports.0",
                             "notes",
                             "log.txt",
                             "notes.202610",
                             "report.Ab3dEf",
                             "rach2-old.Ab3dEfGhI",
                             "rach2-young.Yy3Yy3/now"};
    snprintf(cwd, sizeof(cwd), "%s/clone", f->work);
    for (i = 0; i < 5; i++) {
        char branch[100], wt[200];
        snprintf(branch, sizeof(branch), "worktree-%s", worktrees[i]);
        snprintf(wt, sizeof(wt), ".claude/worktrees/%s", worktrees[i]);
        G("worktree", "add", "-q", "-b", branch, wt, base);
    }
    snprintf(target, sizeof(target), "%s/outside", f->work);
    G("worktree", "add", "-q", "-b", "outside", target, base);
    G("worktree", "add", "-q", "-b", "worktree-helper", "build/worktrees/helper", base);
    G("worktree", "add", "-q", "-b", "worktree-busy", "build/worktrees/busy", base);
    G("worktree", "add", "-q", "-b", "worktree-runs", "build/worktrees/runs", base);
    G("worktree", "add", "-q", "-b", "worktree-flow", "build/worktrees/flow", base);
    fx_mkdir(flow_path(f, "clone/build/worktrees/busy/deep"));
    flow_put(f, "clone/build/worktrees/runs/build/runs/r.out", "");
    flow_put(f, "clone/build/worktrees/flow/build/flow/codex/loop.log", "");
    flow_put(f, "clone/.claude/worktrees/clean/build/out", "");
    flow_put(f, "clone/.claude/worktrees/dirty/untracked", "");
    flow_put(f, "clone/.claude/worktrees/ignored/game.wad", "");
    G("worktree", "lock", ".claude/worktrees/locked");
    snprintf(cwd, sizeof(cwd), "%s/clone/.claude/worktrees/unique", f->work);
    file_at(cwd, "u", "u\n");
    G("add", "u");
    G("commit", "-q", "-m", "Add u");
    snprintf(cwd, sizeof(cwd), "%s/clone", f->work);
    G("switch", "-q", "7-current");
    flow_put(f, "stateA/s-old.jsonl", "");
    flow_put(f, "stateA/s-mid.jsonl", "");
    flow_put(f, "stateA/c-old.jsonl", "");
    flow_put(f, "stateA/loop.log", "");
    age_tree(flow_path(f, "stateA/s-old.jsonl"), time(NULL) - 20 * 86400);
    age_tree(flow_path(f, "stateA/c-old.jsonl"), time(NULL) - 20 * 86400);
    age_tree(flow_path(f, "stateA/loop.log"), time(NULL) - 20 * 86400);
    age_tree(flow_path(f, "stateA/s-mid.jsonl"), time(NULL) - 5 * 86400);
    if (!sparse_at(f, "stateB/s-big1.jsonl", 600) || !sparse_at(f, "stateB/s-big2.jsonl", 600) ||
        !sparse_at(f, "stateB/s-young.jsonl", 1200))
        return 0;
    age_tree(flow_path(f, "stateB/s-big1.jsonl"), time(NULL) - 3 * 86400);
    age_tree(flow_path(f, "stateB/s-big2.jsonl"), time(NULL) - 2 * 86400);
    for (i = 0; i < 9; i++) {
        snprintf(target, sizeof(target), "tmp/%s", scratch[i]);
        flow_put(f, target, "");
    }
    flow_put(f, "tmp2/rach2-old.Tw0Tw0", "");
    fx_mkdir(flow_path(f, "tmp/rach2-busy.Bu5yBu"));
    fx_mkdir(flow_path(f, "tmp/rach2-prog.Pr0gPr"));
    fx_mkdir(flow_path(f, "tmp/rach2-py.ab_12cde"));
    fx_mkdir(flow_path(f, "target"));
    snprintf(source, sizeof(source), "%s", f->self);
    snprintf(target, sizeof(target), "%s/tmp/rach2-prog.Pr0gPr/sleep", f->work);
    if (!fx_copy(source, target))
        return 0;
    symlink(flow_path(f, "target"), flow_path(f, "tmp/rach2-link.Li9kLi"));
    if (!age_tree(flow_path(f, "tmp"), time(NULL) - 172800) ||
        !age_tree(flow_path(f, "tmp2"), time(NULL) - 172800))
        return 0;
    return age_tree(flow_path(f, "tmp/rach2-young.Yy3Yy3/now"), time(NULL));
}
static int tidy_setup(struct fixture *f)
{
    char cwd[4096], origin[4096], base[100], h[5000];
    snprintf(cwd, sizeof(cwd), "%s/clone", f->work);
    snprintf(origin, sizeof(origin), "%s/origin.git", f->work);
    fx_mkdir(cwd);
    G("init", "-q", "--bare", "-b", "master", origin);
    G("init", "-q", "-b", "master");
    file_at(cwd, ".gitignore", "build/\n*.wad\n");
    file_at(cwd, "a", "a\n");
    file_at(cwd, "r", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
    file_at(cwd, "k", "k\n");
    snprintf(h, sizeof(h),
             "f()\n{\n\ta\n\tb\n\tc\n\tx = 1\n\td\n\te\n\tf\n}\ng()\n{\n\ta\n\tb\n\tc\n\tx = "
             "1\n\td\n\te\n\tf\n}\n");
    file_at(cwd, "h", h);
    G("add", ".gitignore", "a", "r", "h", "k");
    G("commit", "-q", "-m", "Add a");
    G("remote", "add", "origin", origin);
    G("rev-parse", "HEAD");
    snprintf(base, sizeof(base), "%s", f->out);
    base[strcspn(base, "\r\n")] = 0;
    G("branch", "5-held", base);
    G("branch", "6-parked", base);
    G("branch", "7-current", base);
    G("switch", "-q", "-c", "1-merged");
    file_at(cwd, "b", "b\n");
    G("add", "b");
    G("commit", "-q", "-m", "Add b");
    G("switch", "-q", "-c", "2-rebased", base);
    file_at(cwd, "r", "1\n2\n3\n4\n5\n6\n7\n8\nnine\n10\n");
    G("commit", "-q", "-a", "-m", "Edit r");
    G("switch", "-q", "-c", "3-subject", base);
    file_at(cwd, "d", "d1\n");
    G("add", "d");
    G("commit", "-q", "-m", "Add d");
    G("switch", "-q", "-c", "4-unpushed", base);
    file_at(cwd, "e", "e\n");
    G("add", "e");
    G("commit", "-q", "-m", "Add e");
    G("switch", "-q", "-c", "8-merge", base);
    G("merge", "-q", "--no-ff", "--no-commit", "1-merged");
    file_at(cwd, "x", "x\n");
    G("add", "x");
    G("commit", "-q", "-m", "Merge 1-merged");
    G("switch", "-q", "-c", "12-delete", base);
    G("rm", "-q", "k");
    G("commit", "-q", "-m", "Remove k");
    G("switch", "-q", "-c", "11-pure", base);
    G("merge", "-q", "--no-ff", "-m", "Merge 1-merged", "1-merged");
    G("switch", "-q", "-c", "9-space", base);
    file_at(cwd, "g", "say \"a b\"\n");
    G("add", "g");
    G("commit", "-q", "-m", "Add g");
    G("switch", "-q", "-c", "10-place", base);
    h[0] = 0;
    snprintf(h, sizeof(h),
             "f()\n{\n\ta\n\tb\n\tc\n\tx = 2\n\td\n\te\n\tf\n}\ng()\n{\n\ta\n\tb\n\tc\n\tx = "
             "1\n\td\n\te\n\tf\n}\n");
    file_at(cwd, "h", h);
    G("commit", "-q", "-a", "-m", "Edit h");
    G("switch", "-q", "master");
    G("merge", "-q", "--ff-only", "1-merged");
    file_at(cwd, "r", "01\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
    G("commit", "-q", "-a", "-m", "Edit r first");
    G("cherry-pick", "-x", "2-rebased");
    file_at(cwd, "d", "d2\n");
    G("add", "d");
    G("commit", "-q", "-m", "Add d");
    file_at(cwd, "g", "say \"ab\"\n");
    G("add", "g");
    G("commit", "-q", "-m", "Add g");
    snprintf(h, sizeof(h),
             "f()\n{\n\ta\n\tb\n\tc\n\tx = 1\n\td\n\te\n\tf\n}\ng()\n{\n\ta\n\tb\n\tc\n\tx = "
             "2\n\td\n\te\n\tf\n}\n");
    file_at(cwd, "h", h);
    G("commit", "-q", "-a", "-m", "Edit h");
    file_at(cwd, "k", "k2\n");
    G("commit", "-q", "-a", "-m", "Edit k");
    G("push", "-q", "origin", "master");
    G("switch", "-q", "-c", "13-milestone", base);
    file_at(cwd, "m", "m\n");
    G("add", "m");
    G("commit", "-q", "-m", "Add m");
    G("push", "-q", "origin", "13-milestone:milestone/01-a");
    G("switch", "-q", "-c", "14-other", base);
    file_at(cwd, "o", "o\n");
    G("add", "o");
    G("commit", "-q", "-m", "Add o");
    G("push", "-q", "origin", "14-other:feature/x");
    G("switch", "-q", "master");
    G("fetch", "-q", "origin");
    return tidy_files(f, base);
}

static int lib_command(struct fixture *f, const char *cwd, const char *function, const char *state)
{
    char command[14000];
    const char *a[] = {"/bin/sh", "-c", command, NULL};
    snprintf(command, sizeof(command), "state='%s'; . '%s'; %s", flow_path(f, state),
             flow_source(f, "tools/flow/lib.sh"), function);
    return !fx_run(f, a, cwd, NULL, NULL);
}
static int tidy_checks(struct fixture *f)
{
    const char *keep_branches[] = {
        "2-rebased",      "3-subject",        "4-unpushed",      "7-current",       "8-merge",
        "12-delete",      "9-space",          "10-place",        "11-pure",         "14-other",
        "worktree-dirty", "worktree-ignored", "worktree-locked", "worktree-unique", "outside",
        "worktree-runs",  "worktree-flow"};
    const char *gone_branches[] = {"1-merged",     "5-held",         "6-parked",
                                   "13-milestone", "worktree-clean", "worktree-helper",
                                   "worktree-busy"};
    const char *kept[] = {"clone/.claude/worktrees/dirty/untracked",
                          "clone/.claude/worktrees/ignored/game.wad",
                          "clone/.claude/worktrees/locked",
                          "clone/.claude/worktrees/unique/u",
                          "outside",
                          "stateA/s-mid.jsonl",
                          "stateA/c-old.jsonl",
                          "stateA/loop.log",
                          "stateB/s-young.jsonl",
                          "tmp/rach2-young.Yy3Yy3/now",
                          "tmp/rach2-ports.0",
                          "tmp/notes",
                          "tmp/log.txt",
                          "tmp/notes.202610",
                          "tmp/report.Ab3dEf",
                          "tmp/rach2-old.Ab3dEfGhI",
                          "tmp/rach2-link.Li9kLi",
                          "target",
                          "tmp/rach2-busy.Bu5yBu",
                          "tmp/rach2-prog.Pr0gPr/sleep",
                          "clone/build/worktrees/runs/build/runs/r.out",
                          "clone/build/worktrees/flow/build/flow/codex/loop.log"};
    const char *gone[] = {"clone/.claude/worktrees/clean",
                          "clone/build/worktrees/helper",
                          "stateA/s-old.jsonl",
                          "stateB/s-big1.jsonl",
                          "stateB/s-big2.jsonl",
                          "tmp/rach2-old.Ab3dEf",
                          "tmp/rach2-py.ab_12cde",
                          "tmp/rach2-file.Fi1eFi",
                          "clone/build/worktrees/busy",
                          "tmp2/rach2-old.Tw0Tw0"};
    char cwd[4096], branch[200];
    unsigned i;
    int ok = 1;
    snprintf(cwd, sizeof(cwd), "%s/clone", f->work);
    for (i = 0; i < sizeof(keep_branches) / sizeof(keep_branches[0]); i++) {
        snprintf(branch, sizeof(branch), "refs/heads/%s", keep_branches[i]);
        ok =
            fx_check(f, g(f, cwd, "rev-parse", "-q", "--verify", branch, NULL), keep_branches[i]) &&
            ok;
    }
    for (i = 0; i < sizeof(gone_branches) / sizeof(gone_branches[0]); i++) {
        snprintf(branch, sizeof(branch), "refs/heads/%s", gone_branches[i]);
        ok = fx_check(f, !g(f, cwd, "rev-parse", "-q", "--verify", branch, NULL),
                      gone_branches[i]) &&
             ok;
    }
    for (i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        ok = fx_check(f, exists(f, kept[i]), kept[i]) && ok;
    for (i = 0; i < sizeof(gone) / sizeof(gone[0]); i++)
        ok = fx_check(f, !exists(f, gone[i]), gone[i]) && ok;
    return fx_check(f, is_link(f, "tmp/rach2-link.Li9kLi"), "scratch link kept as a link") && ok;
}
static int tidy_stems(struct fixture *f)
{
    char cwd[4096], *stems;
    int ok = 1;
    /* stems reads a commit, so index a private copy of its source files. */
    snprintf(cwd, sizeof(cwd), "%s/stems-source", f->work);
    if (!fx_tree(flow_source(f, "tools"), flow_path(f, "stems-source/tools")) ||
        !fx_tree(flow_source(f, "tests"), flow_path(f, "stems-source/tests")) ||
        !fx_text(flow_path(f, "stems-source/tests/stem_prefix.py"),
                 "work = tempfile.mkdtemp(prefix='rach2-stem-prefix.')\n") ||
        !g(f, cwd, "init", "-q", NULL) || !g(f, cwd, "add", "tools", "tests", NULL) ||
        !g(f, cwd, "commit", "-q", "-m", "Prepared source fixtures", NULL))
        return 0;
    if (!lib_command(f, cwd, "stems HEAD", "stateA"))
        return 0;
    stems = f->out;
    ok = fx_check(f,
                  fx_regex(stems, "^rach2_verify$") && fx_regex(stems, "^rach2-stem-prefix$") &&
                      fx_regex(stems, "^rach2-accept-secrets$") &&
                      !fx_regex(stems, "[^A-Za-z0-9_\n-]"),
                  "scratch stems") &&
         ok;
    {
        const char *line = stems;
        while (*line) {
            if (strncmp(line, "rach2", 5))
                ok = fx_check(f, 0, "scratch stem prefix") && ok;
            line = strchr(line, '\n');
            if (!line)
                break;
            line++;
        }
    }
    return ok;
}
int flow_tidy(struct fixture *f)
{
    char cwd[4096], program[4096];
    unsigned i;
    int ok = 1;
    struct fx_child busy[3];
    if (!tidy_setup(f))
        return 0;
    snprintf(cwd, sizeof(cwd), "%s/tmp/rach2-busy.Bu5yBu", f->work);
    {
        const char *a[] = {f->self, "--hold", NULL};
        if (!fx_start(f, &busy[0], a, cwd, NULL, NULL) || !link_ready(&busy[0], "cwd", cwd))
            return 0;
    }
    snprintf(program, sizeof(program), "%s/tmp/rach2-prog.Pr0gPr/sleep", f->work);
    {
        const char *a[] = {program, "--hold", NULL};
        if (!fx_start(f, &busy[1], a, "/", NULL, NULL) || !link_ready(&busy[1], "exe", program))
            return 0;
    }
    snprintf(cwd, sizeof(cwd), "%s/clone/build/worktrees/busy/deep", f->work);
    {
        const char *a[] = {f->self, "--hold", NULL};
        if (!fx_start(f, &busy[2], a, cwd, NULL, NULL) || !link_ready(&busy[2], "cwd", cwd))
            return 0;
    }
    snprintf(cwd, sizeof(cwd), "%s/clone", f->work);
    if (!lib_command(f, cwd, "tidy", "stateA"))
        return 0;
    ok = fx_check(f, exists(f, "clone/build/worktrees/busy/deep"), "worktree in use kept") && ok;
    fx_stop(&busy[2], SIGTERM);
    fx_wait(f, &busy[2]);
    if (!lib_command(f, cwd, "tidy", "stateA") ||
        !lib_command(f, f->work, "prune_logs 's-*'", "stateA") ||
        !lib_command(f, f->work, "prune_logs 's-*'", "stateB"))
        return 0;
    {
        const char *a[] = {"/usr/bin/perl",
                           flow_source(f, "tools/flow/scratch.pl"),
                           "-d",
                           flow_path(f, "tmp"),
                           "-d",
                           flow_path(f, "tmp2"),
                           "-d",
                           flow_path(f, "missing"),
                           "86400",
                           "rach2-old",
                           "rach2-file",
                           "rach2-py",
                           "rach2-young",
                           "rach2-busy",
                           "rach2-prog",
                           "rach2-ports",
                           "rach2-link",
                           "report",
                           NULL};
        if (fx_run(f, a, f->work, NULL, NULL))
            return 0;
    }
    for (i = 0; i < 2; i++) {
        fx_stop(&busy[i], SIGTERM);
        fx_wait(f, &busy[i]);
    }
    ok = tidy_checks(f) && ok;
    return tidy_stems(f) && ok;
}
