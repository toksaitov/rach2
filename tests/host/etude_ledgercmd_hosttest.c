/**
 * Check `etude status` and `etude release` (tools/etude/posix/claims.c),
 * which list the claims of a ledger and drop one, against a ledger of real
 * processes.
 *
 * host-deps: tools/etude/history_collect.c
 * host-deps: tools/etude/history_json.c
 * host-deps: tools/etude/authority.c tools/etude/authority_parse.c
 * host-deps: tools/etude/posix/authority_platform.c
 * host-deps: tools/etude/win/authority_platform.c
 * host-deps: tools/etude/ledger.c tools/etude/posix/process.c utils/sha256.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 *
 * This process holds a claim and a request that waits; workers it starts
 * leave a claim their owner found unconfirmed, a claim whose keeper never
 * confirmed the cleanup, and a claim whose process still runs. The command,
 * run in a child of this process, must list each with its status and
 * reason; refuse to drop the claim whose owner runs, naming the owner, the
 * claim whose process runs, naming the process, and a claim the ledger
 * lacks; and drop an uncertain claim. A path that is not a ledger and a
 * ledger whose file does not parse are refused, and neither is changed.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "etude/ledger.h"

#include "../../tools/etude/verbs.h"
#include "../../tools/etude/posix/claims.c"

static int fails;
#define CHECK(c, row)                                             \
    do {                                                          \
        if (!(c)) {                                               \
            printf("etude-claims-test: fail: %s: %s\n", row, #c); \
            fails++;                                              \
        }                                                         \
    } while (0)

#if defined(__linux__)
#define SYSTEM "linux"
#elif defined(__APPLE__)
#define SYSTEM "darwin"
#else
#define SYSTEM "posix"
#endif

/* The process whose claim stays live, and the pipe whose end ends it. */
static pid_t holder;
static int holder_pipe = -1;

/* What one run of the command gave. */
struct ran {
    int status; /* its exit status, or -1 when it did not exit */
    char out[8192];
    char err[2048];
};

/** End the holder, at exit and after a signal. */
static void end_holder(void)
{
    if (holder_pipe >= 0)
        close(holder_pipe);
    holder_pipe = -1;
    if (holder > 0) {
        kill(holder, SIGKILL);
        while (waitpid(holder, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    holder = 0;
}

/** Run the exit cleanup after a signal. */
static void interrupted(int sig)
{
    exit(128 + sig);
}

/** Start a process that runs until its pipe closes; give its pid. */
static pid_t start_holder(void)
{
    int p[2];
    char c;
    pid_t pid;

    if (pipe(p) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        _exit(read(p[0], &c, 1) < 0);
    }
    close(p[0]);
    holder_pipe = p[1];
    return pid;
}

/**
 * Be a worker that claims `cpu` thousandths of a processor as `id`, registers
 * `process`, or itself when it is NULL, and exits: with the cleanup found
 * unconfirmed when `unconfirmed` is set, and with the claim left running
 * otherwise. Give the worker's pid, or -1 when it failed.
 */
static long worker(const char *ledger, const char *id, uint64_t cpu,
                   const struct etude_pid *process, int unconfirmed)
{
    struct etude_ledger_res res = {"cpu_millis", 0};
    struct etude_ledger *l;
    struct etude_pid me;
    int status;
    pid_t pid = fork();

    res.amount = cpu;
    if (pid == 0) {
        l = etude_ledger_open(ledger);
        if (l == NULL || etude_process_state((long)getpid(), &me) != 1 ||
            etude_ledger_reserve(l, id, &res, 1, NULL, 0) != 0 ||
            etude_ledger_register(l, id, process ? process : &me, process ? process : &me) != 0 ||
            (unconfirmed && etude_ledger_finish(l, id, ETUDE_CLEANUP_UNCONFIRMED) != 0))
            _exit(1);
        etude_ledger_close(l);
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return -1;
    return (long)pid;
}

/** Read the file `path` into `buf` of `size` bytes, as a string. */
static void slurp(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, size - 1, f) : 0;

    buf[n] = '\0';
    if (f)
        fclose(f);
}

/**
 * Run `etude VERB`, `status` or `release`, with the arguments `a1` and
 * `a2`, of which the last ones may be NULL, in a child of this process, and
 * give what it did in `r`.
 */
static void run(struct ran *r, const char *verb, const char *a1, const char *a2)
{
    char *argv[] = {"etude", (char *)a1, (char *)a2, NULL};
    int argc = a1 == NULL ? 1 : a2 == NULL ? 2 : 3, out, err, st;
    pid_t pid;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        out = open(fix_path("out"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        err = open(fix_path("err"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out < 0 || err < 0 || dup2(out, 1) < 0 || dup2(err, 2) < 0)
            _exit(99);
        st = strcmp(verb, "status") == 0 ? etude_status_main(argc, argv)
                                         : etude_release_main(argc, argv);
        fflush(stdout);
        fflush(stderr);
        _exit(st);
    }
    r->status = -1;
    if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st))
        r->status = WEXITSTATUS(st);
    slurp(fix_path("out"), r->out, sizeof(r->out));
    slurp(fix_path("err"), r->err, sizeof(r->err));
}

/** Say whether `text` has the line `line`, given without its newline. */
static int has_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *p = text;

    while ((p = strstr(p, line)) != NULL) {
        if ((p == text || p[-1] == '\n') && p[n] == '\n')
            return 1;
        p += n;
    }
    return 0;
}

/** Stop a listing at the claim or request whose identity is `ctx`. */
static int keeps_one(const struct etude_ledger_claim *c, void *ctx)
{
    return strcmp(c->id, ctx) == 0;
}

/** Say whether the ledger `l` keeps the claim `id`. */
static int kept(struct etude_ledger *l, const char *id)
{
    return etude_ledger_list(l, keeps_one, (void *)id) == 1;
}

/**
 * Fill the ledger `dir`, whose handle in this process is `l`, and give the
 * pids of the workers that own its claims `lost`, `silent` and `held`.
 */
static void fill(struct etude_ledger *l, const char *dir, long *lost, long *silent, long *held)
{
    struct etude_ledger_res mine[2] = {{"cpu_millis", 100}, {"memory_bytes", 4096}};
    struct etude_ledger_res ask = {"cpu_millis", 500}, cap = {"cpu_millis", 1000};
    struct etude_pid running;

    CHECK(etude_ledger_reserve(l, "owned", mine, 2, NULL, 0) == 0, "the ledger is filled");
    *lost = worker(dir, "lost", 200, NULL, 1);
    *silent = worker(dir, "silent", 300, NULL, 0);
    holder = start_holder();
    CHECK(holder > 0 && etude_process_state((long)holder, &running) == 1, "the ledger is filled");
    *held = worker(dir, "held", 400, &running, 0);
    CHECK(*lost > 0 && *silent > 0 && *held > 0, "the ledger is filled");
    errno = 0;
    CHECK(etude_ledger_admit(l, "asking", "tester", &ask, 1, &cap, 1, 1) == -1 && errno == EAGAIN,
          "the ledger is filled");
}

/** Check the listing of the ledger `dir` as it was filled. */
static void check_list(const char *dir, long lost, long silent, long held)
{
    struct ran r;
    char line[512];
    long me = (long)getpid();

    run(&r, "status", dir, NULL);
    CHECK(r.status == 0 && r.err[0] == '\0', "the listing");
    snprintf(line, sizeof(line),
             "claim owned: owned; state reserved, system " SYSTEM
             ", owner %ld, processes 0; holds cpu_millis 100, memory_bytes 4096",
             me);
    CHECK(has_line(r.out, line), "an owned claim is listed");
    snprintf(line, sizeof(line),
             "claim lost: uncertain: its owner found the cleanup unconfirmed; state uncertain, "
             "system " SYSTEM ", owner %ld, processes 2; holds cpu_millis 200",
             lost);
    CHECK(has_line(r.out, line), "an uncertain claim is listed with its reason");
    snprintf(line, sizeof(line),
             "claim silent: uncertain: its keeper did not confirm the cleanup; state running, "
             "system " SYSTEM ", owner %ld, processes 2; holds cpu_millis 300",
             silent);
    CHECK(has_line(r.out, line), "an uncertain claim is listed with its reason");
    snprintf(line, sizeof(line),
             "claim held: live: a process of it runs; state running, system " SYSTEM
             ", owner %ld, processes 2; holds cpu_millis 400",
             held);
    CHECK(has_line(r.out, line), "a live claim is listed with its reason");
    snprintf(line, sizeof(line),
             "request asking: owned; state blocked, system " SYSTEM
             ", owner %ld, account tester; asks cpu_millis 500",
             me);
    CHECK(has_line(r.out, line), "a request is listed");
    CHECK(has_line(r.out, "etude status: result: owned 2, live 1, uncertain 2"),
          "the listing counts each status");
    if (fails != 0)
        printf("etude-claims-test: the listing:\n%s%s", r.out, r.err);
}

/** Check the drops of an owned, a live, a missing and an uncertain claim. */
static void check_drop(struct etude_ledger *l, const char *dir)
{
    struct ran r;
    char line[512];

    run(&r, "release", dir, "owned");
    snprintf(line, sizeof(line),
             "etude release: result: refused: owned: its owner, process %ld, runs", (long)getpid());
    CHECK(r.status == 1 && r.out[0] == '\0' && has_line(r.err, line), "an owned claim is refused");
    CHECK(kept(l, "owned"), "an owned claim is refused");
    run(&r, "release", dir, "held");
    snprintf(line, sizeof(line), "etude release: result: refused: held: its process %ld runs",
             (long)holder);
    CHECK(r.status == 1 && r.out[0] == '\0' && has_line(r.err, line), "a live claim is refused");
    CHECK(kept(l, "held"), "a live claim is refused");
    run(&r, "release", dir, "absent");
    snprintf(line, sizeof(line), "etude release: result: refused: absent: %s",
             "the ledger has no such claim");
    CHECK(r.status == 1 && r.out[0] == '\0' && has_line(r.err, line), "a missing claim is refused");
    run(&r, "release", dir, "lost");
    CHECK(r.status == 0 && r.err[0] == '\0' &&
              has_line(r.out, "etude release: result: dropped: lost"),
          "an uncertain claim is dropped");
    CHECK(!kept(l, "lost") && kept(l, "silent") && kept(l, "held") && kept(l, "asking"),
          "an uncertain claim is dropped");
    run(&r, "status", dir, NULL);
    CHECK(r.status == 0 && has_line(r.out, "etude status: result: owned 2, live 1, uncertain 1"),
          "an uncertain claim is dropped");
}

/** Write `text` as the whole file `path`; give 0, or -1 when it failed. */
static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    int ok = f != NULL && fputs(text, f) >= 0;

    if (f != NULL && fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/**
 * Check that a wrong usage, a path that is no ledger and a ledger whose file
 * does not parse are refused, and that neither path is changed.
 */
static void check_refused(void)
{
    static const char damaged[] = "etude-ledger 1\nclaim cut\n";
    struct ran r;
    char text[64];
    struct stat st;

    run(&r, "release", fix_path("ledger"), NULL);
    CHECK(r.status == 2 && has_line(r.err, "usage: etude release LEDGER ID"),
          "a wrong usage is refused");
    run(&r, "status", fix_path("nothing"), NULL);
    CHECK(r.status == 2 && strstr(r.err, ": not a ledger: it has no verdicts directory\n") &&
              stat(fix_path("nothing"), &st) != 0,
          "a path that is not a ledger is refused");
    CHECK(mkdir(fix_path("damaged"), 0700) == 0 && mkdir(fix_path("damaged/verdicts"), 0700) == 0 &&
              put(fix_path("damaged/claims"), damaged) == 0,
          "a damaged ledger is refused");
    run(&r, "status", fix_path("damaged"), NULL);
    CHECK(r.status == 2 && strstr(r.err, ": its claims file does not parse; nothing was changed\n"),
          "a damaged ledger is refused");
    slurp(fix_path("damaged/claims"), text, sizeof(text));
    CHECK(strcmp(text, damaged) == 0, "a damaged ledger is refused");
}

/** Fill a ledger, then run the command on it and on paths it must refuse. */
int main(void)
{
    struct etude_ledger *l;
    const char *dir;
    long lost = 0, silent = 0, held = 0;

    atexit(end_holder);
    signal(SIGTERM, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGHUP, interrupted);
    dir = fix_path("ledger");
    l = etude_ledger_open(dir);
    CHECK(l != NULL, "the ledger is filled");
    if (l == NULL)
        return 1;
    fill(l, dir, &lost, &silent, &held);
    check_list(dir, lost, silent, held);
    check_drop(l, dir);
    check_refused();
    etude_ledger_close(l);
    end_holder();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_ledgercmd_hosttest: pass\n");
    return 0;
}
