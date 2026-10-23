/* flow_status.c: real lock holders, stable waits and stale process records. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int flow_status_lock(int argc, char **argv)
{
    struct flock lock;
    int fd;
    if (argc != 4)
        return 2;
    fd = open(argv[2], O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return 1;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = !strcmp(argv[3], "read") ? F_RDLCK : F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &lock))
        return 1;
    puts("ready");
    fflush(stdout);
    for (;;)
        pause();
}
static int identity(pid_t pid, char out[200], int reused)
{
    char file[100], *text, *at, *save = NULL, *word;
    unsigned i = 0;
    long group = 0;
    unsigned long long start = 0;
    snprintf(file, sizeof(file), "/proc/%ld/stat", (long)pid);
    text = fx_read(file);
    if (!text)
        return 0;
    at = strrchr(text, ')');
    if (!at) {
        free(text);
        return 0;
    }
    for (word = strtok_r(at + 2, " \t\n", &save); word;
         word = strtok_r(NULL, " \t\n", &save), i++) {
        if (i == 2)
            group = strtol(word, NULL, 10);
        if (i == 19) {
            start = strtoull(word, NULL, 10);
            break;
        }
    }
    free(text);
    if (i != 19)
        return 0;
    snprintf(out, 200, "%ld %ld %llu 0", (long)pid, group, start + (reused ? 1 : 0));
    return 1;
}
static int report(struct fixture *f)
{
    const char *a[] = {"python3", flow_source(f, "tools/flow/test_status.py"), f->work, NULL};
    return !fx_run(f, a, f->work, NULL, NULL);
}
static int ready(struct fx_child *p)
{
    while (!fx_has(p->out, "ready\n")) {
        if (fx_poll(p))
            return 0;
        fx_pause();
    }
    return 1;
}
static int holder(struct fixture *f, struct fx_child *p, const char *path, const char *mode)
{
    const char *a[] = {f->self, "--status-lock", path, mode, NULL};
    return fx_start(f, p, a, f->work, NULL, NULL) && ready(p);
}
static int waiter(struct fixture *f, struct fx_child *p, const char *mode)
{
    const char *a[] = {
        "/host-products/etude", "lock", "ports", flow_path(f, "ports"), "1", "true", NULL};
    fx_env("RACH2_HOST_LOCK", mode);
    return fx_start(f, p, a, f->work, NULL, NULL) && fx_until(p, flow_path(f, "work/claim.wait"));
}
static int claim_row(struct fixture *f, const char *filename, const char *mode,
                     const char *lockmode, int stable)
{
    struct fx_child held, waiting;
    char path[4096], text[300], *before;
    int ok = 1;
    snprintf(path, sizeof(path), "%s/ports/%s", f->work, filename);
    if (!holder(f, &held, path, lockmode) || !waiter(f, &waiting, mode))
        return 0;
    before = fx_read(flow_path(f, "work/claim.wait"));
    if (!report(f)) {
        free(before);
        return 0;
    }
    snprintf(text, sizeof(text), "holder %ld alive", (long)held.pid);
    ok = fx_check(f,
                  strstr(f->out, stable ? "waits for ports since " : "waits for host since ") &&
                      strstr(f->out, " UTC") && strstr(f->out, text) &&
                      (!stable ? strstr(f->out, path) != NULL : 1),
                  "conflicting holder is reported") &&
         ok;
    if (stable) {
        struct timespec delay = {1, 100000000};
        nanosleep(&delay, NULL);
        ok = fx_check(f, fx_equal(flow_path(f, "work/claim.wait"), before),
                      "wait start unchanged across a retry") &&
             ok;
    }
    free(before);
    fx_stop(&held, SIGTERM);
    fx_wait(f, &held);
    fx_wait(f, &waiting);
    return fx_check(f, !f->status && !fx_exists(flow_path(f, "work/claim.wait")),
                    "admitted waiter removes claim") &&
           ok;
}
static int two_holders(struct fixture *f)
{
    struct fx_child a, b, waiting;
    char wanted[100];
    int ok;
    const char *path = flow_path(f, "ports/rach2-host.lock");
    if (!holder(f, &a, path, "read") || !holder(f, &b, path, "read") ||
        !waiter(f, &waiting, "alone") || !report(f))
        return 0;
    snprintf(wanted, sizeof(wanted), "holder %ld alive", (long)a.pid);
    ok = strstr(f->out, wanted) != NULL;
    snprintf(wanted, sizeof(wanted), "holder %ld alive", (long)b.pid);
    ok = ok && strstr(f->out, wanted);
    fx_stop(&waiting, SIGTERM);
    fx_wait(f, &waiting);
    fx_stop(&a, SIGTERM);
    fx_wait(f, &a);
    fx_stop(&b, SIGTERM);
    fx_wait(f, &b);
    fx_remove(flow_path(f, "work/claim.wait"));
    return fx_check(f, ok, "both shared host lock holders are listed");
}
static int pool_rows(struct fixture *f)
{
    struct fx_child p;
    const char *a[] = {f->self, "--hold", NULL};
    char id[200], text[1000], want[100];
    int ok;
    if (!fx_start(f, &p, a, f->work, NULL, NULL) || !identity(p.pid, id, 0))
        return 0;
    snprintf(text, sizeof(text),
             "reason memory\nsince 1\nbudget 4000\npending hungry2 3000\nholder hungry1 3000 %s\n",
             id);
    flow_put(f, "work/pool.wait", text);
    if (!report(f))
        return 0;
    snprintf(want, sizeof(want), "holder hungry1 pid %ld alive", (long)p.pid);
    ok = fx_check(f, strstr(f->out, want) != NULL, "live pool holder");
    fx_stop(&p, SIGTERM);
    fx_wait(f, &p);
    if (!report(f))
        return 0;
    snprintf(want, sizeof(want), "holder hungry1 pid %ld dead", (long)p.pid);
    return fx_check(f, strstr(f->out, want) && strstr(f->out, "fault:"), "dead pool holder") && ok;
}
static int owner_rows(struct fixture *f)
{
    struct fx_child p;
    char id[200], text[1000], wanted[100];
    const char *a[] = {f->self, "--hold", NULL};
    int ok = 1;
    if (!identity(getpid(), id, 1))
        return 0;
    snprintf(text, sizeof(text), "identity %s\nwork %s/work\nargs reused\n", id, f->work);
    flow_put(f, "build/.run.status.fixture", text);
    if (!report(f))
        return 0;
    ok = fx_check(f, strstr(f->out, "dead (pid reused)") != NULL, "reused run identity") && ok;
    snprintf(text, sizeof(text), "pid 99999999\npgid 1\nsince old\nargs fixture\n");
    flow_put(f, "build/.run.lock/owner", text);
    if (!report(f))
        return 0;
    ok =
        fx_check(f, strstr(f->out, "fault: inspect the work and clear the stale tree lock") != NULL,
                 "stale tree lock") &&
        ok;
    if (!fx_start(f, &p, a, f->work, NULL, NULL) || !identity(p.pid, id, 0))
        return 0;
    snprintf(text, sizeof(text), "pid %ld\npgid 1\nidentity %s\nsince now\nargs held\n",
             (long)p.pid, id);
    flow_put(f, "build/.run.lock/owner", text);
    if (!report(f))
        return 0;
    snprintf(wanted, sizeof(wanted), "tree lock: holder %ld alive", (long)p.pid);
    ok = fx_check(f, strstr(f->out, wanted) != NULL, "live tree lock identity") && ok;
    if (!identity(p.pid, id, 1))
        return 0;
    snprintf(text, sizeof(text), "pid %ld\nidentity %s\nsince now\nargs held\n", (long)p.pid, id);
    flow_put(f, "build/.run.lock/owner", text);
    if (!report(f))
        return 0;
    ok = fx_check(f, strstr(f->out, "dead (pid reused)") && strstr(f->out, "stale tree lock"),
                  "reused tree lock identity") &&
         ok;
    fx_stop(&p, SIGTERM);
    fx_wait(f, &p);
    return ok;
}
static int zombie_row(struct fixture *f)
{
    struct fx_child p;
    char path[100], *stat, *at, id[200], text[1000], wanted[100];
    const char *a[] = {"true", NULL};
    int ok;
    if (!fx_start(f, &p, a, f->work, NULL, NULL))
        return 0;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)p.pid);
    /* Keep the exited child unreaped while the product reads its identity. */
    for (;;) {
        stat = fx_read(path);
        at = strrchr(stat, ')');
        if (at && at[2] == 'Z') {
            free(stat);
            break;
        }
        free(stat);
        fx_pause();
    }
    if (!identity(p.pid, id, 0))
        return 0;
    snprintf(text, sizeof(text), "identity %s\nwork %s/work\nargs zombie\n", id, f->work);
    flow_put(f, "build/.run.status.fixture", text);
    if (!report(f))
        return 0;
    snprintf(wanted, sizeof(wanted), "run %ld dead:", (long)p.pid);
    ok = fx_check(f, strstr(f->out, wanted) != NULL, "zombie is dead before reap");
    fx_wait(f, &p);
    return ok;
}
int flow_status(struct fixture *f)
{
    char id[200], text[1000];
    int ok = 1;
    if (!fx_mkdir(flow_path(f, "build")) || !fx_mkdir(flow_path(f, "work")) ||
        !fx_mkdir(flow_path(f, "ports")) || !identity(getpid(), id, 0))
        return 0;
    snprintf(text, sizeof(text), "identity %s\nwork %s/work\nargs fixture\n", id, f->work);
    flow_put(f, "build/.run.status.fixture", text);
    fx_env("RACH2_RUN_WORK", flow_path(f, "work"));
    fx_env("RACH2_RUN_STATUS", NULL);
    fx_env("RACH2_HOST_LOCK", NULL);
    ok = claim_row(f, "rach2-ports.0", NULL, "write", 1) && ok;
    ok = claim_row(f, "rach2-host.gate", "shared", "write", 0) && ok;
    ok = claim_row(f, "rach2-host.lock", "alone", "read", 0) && ok;
    ok = two_holders(f) && ok;
    ok = pool_rows(f) && ok;
    ok = owner_rows(f) && ok;
    ok = zombie_row(f) && ok;
    fx_remove(flow_path(f, "build/.run.lock"));
    fx_remove(flow_path(f, "work/pool.wait"));
    fx_remove(flow_path(f, "build/.run.status.fixture"));
    if (!report(f))
        return 0;
    return fx_check(f, strstr(f->out, "no recorded test run") != NULL, "no recorded run") && ok;
}
