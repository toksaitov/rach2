/**
 * Check the bridge against the POSIX record locks used by old runners.
 * host-deps: tools/etude/posix/bridge.c tools/etude/portlock.c
 * host-deps: tools/etude/ledger.c
 * host-deps: tools/etude/measurement_context.c tools/etude/json.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c
 * host-deps: tools/etude/posix/process.c tools/etude/capture.c utils/sha256.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include "compat/fixdir_host.c"
#include "etude/bridge.h"
#include "etude/ledger.h"

#include "../../tools/etude/verbs.h"
#include "../../tools/etude/posix/admit.c"

static int fails;
static char executable[4096];
#define CHECK(c, row)                                                     \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "etude-admit-test: fail: %s: %s\n", row, #c); \
            fails++;                                                      \
        }                                                                 \
    } while (0)

/* Only these fixture processes belong to this test. */
static pid_t active_worker;
static long holders[32];
static size_t nholders;

/** Announce the child, then wait on the pipe owned by the test. */
static int fixture_block(char **argv)
{
    FILE *f;
    char line[4096];
    int fd;
    ssize_t n;
    const char *marker = getenv("RUNNER_TRACKING_ID");
    if (strcmp(argv[5], "tracked") == 0 &&
        (marker == NULL || strcmp(marker, "etude-admit-fixture") != 0))
        return 90;
    f = fopen(argv[3], "w");
    if (f == NULL || fputc('.', f) == EOF || fclose(f) != 0)
        return 92;
    fd = open(argv[4], O_RDONLY);
    if (fd < 0)
        return 93;
    while ((n = read(fd, line, sizeof(line))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0 && fwrite(line, 1, (size_t)n, stdout) != (size_t)n) {
            close(fd);
            return 94;
        }
    }
    close(fd);
    return n < 0;
}

/** Run a compiled command whose process the admission checks observe. */
static int fixture_command(int argc, char **argv)
{
    FILE *f;
    char path[4096], line[4096];
    int found = 0;

    if (argc < 3)
        return 99;
    if (strcmp(argv[2], "pass") == 0)
        return 0;
    if (strcmp(argv[2], "kill") == 0) {
        raise(SIGKILL);
        return 99;
    }
    if (strcmp(argv[2], "block") == 0 && argc == 6)
        return fixture_block(argv);
    if (strcmp(argv[2], "touch") == 0 && argc == 4) {
        f = fopen(argv[3], "w");
        return f != NULL && fclose(f) == 0 ? 2 : 95;
    }
    if (strcmp(argv[2], "registered") != 0 || argc != 4)
        return 99;
    if (getenv("RACH2_PORT_OFFSET") == NULL || strcmp(getenv("RACH2_PORT_OFFSET"), "0") != 0)
        return 90;
    if (snprintf(path, sizeof(path), "%s/claims", argv[3]) >= (int)sizeof(path))
        return 91;
    f = fopen(path, "r");
    if (f == NULL)
        return 91;
    while (fgets(line, sizeof(line), f))
        found |= strstr(line, "state running") != NULL;
    fclose(f);
    return found ? 7 : 91;
}

/** Keep the identity of a fixture's lock holder for interrupted cleanup. */
static void remember(long pid)
{
    if (nholders < sizeof(holders) / sizeof(holders[0]))
        holders[nholders++] = pid;
}

/** Forget a fixture's holder after reaping it. */
static void forget(long pid)
{
    size_t i;
    for (i = 0; i < nholders; i++)
        if (holders[i] == pid)
            holders[i] = 0;
}

/** Stop fixture workers, then their private lock holders. */
static void cleanup_test(void)
{
    size_t i;
    if (active_worker > 0) {
        kill(active_worker, SIGTERM);
        while (waitpid(active_worker, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    for (i = 0; i < nholders; i++) {
        if (holders[i] <= 0)
            continue;
        kill((pid_t)holders[i], SIGKILL);
        while (waitpid((pid_t)holders[i], NULL, 0) < 0 && errno == EINTR) {
        }
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

/** Run the exit cleanup and retain named evidence after an interruption. */
static void interrupted(int sig)
{
    exit(128 + sig);
}

/** Acquire a fixture's locks for the claim its row reserved. */
static struct etude_bridge *start(const char *ledger, const char *id, const char *dir, int alone,
                                  int legacy, unsigned wait_ms)
{
    struct etude_bridge_spec s = {ledger, id, dir, alone, legacy, wait_ms};
    struct etude_bridge *b = etude_bridge_start(&s);
    if (b != NULL)
        remember(etude_bridge_pid(b));
    return b;
}

/** Open one of the old runner's lock files in the private directory. */
static int lock_file(const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", fix_dir(), name);
    return open(path, O_RDWR | O_CREAT, 0666);
}

/** Try a whole-file lock, as `etude lock ports` does. */
static int lock_try(int fd, short type)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLK, &fl);
}

/** Give the process whose lock conflicts with `type`, or zero. */
static long locked_by(int fd, short type)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_GETLK, &fl) != 0)
        return -1;
    return fl.l_type == F_UNLCK ? 0 : (long)fl.l_pid;
}

/** Reserve one unit of memory for a test's claim. */
static void reserve(struct etude_ledger *l, const char *id)
{
    struct etude_ledger_res res = {"memory_bytes", 1};
    struct etude_ledger_res cap = {"memory_bytes", 20};
    CHECK(etude_ledger_admit(l, id, "test", &res, 1, &cap, 1, 0) == 0, "reserve");
}

/** Release a claim and reap the process that kept its locks. */
static void finish(struct etude_ledger *l, const char *id, struct etude_bridge *b)
{
    CHECK(etude_ledger_finish(l, id, ETUDE_CLEANUP_CONFIRMED) == 0, "finish");
    long pid = b == NULL ? 0 : etude_bridge_pid(b);
    etude_bridge_join(b);
    forget(pid);
}

/** A collection's child claim retains the block after its root is free. */
static void collection(struct etude_ledger *l, const char *ledger)
{
    struct etude_bridge_spec s = {ledger, "collection:ports", fix_dir(), 0, 1, 0};
    struct etude_bridge *b, *blocked;
    int port = lock_file("rach2-ports.0");
    reserve(l, s.id);
    b = etude_bridge_collection_start(&s, "collection");
    CHECK(b != NULL, "collection holder starts");
    if (b == NULL) {
        finish(l, s.id, NULL);
        close(port);
        return;
    }
    long pid = etude_bridge_pid(b);
    remember(pid);
    reserve(l, "collection:case");
    CHECK(etude_ledger_finish(l, "collection:case", ETUDE_CLEANUP_UNCONFIRMED) == 0,
          "collection case cleanup is uncertain");
    CHECK(etude_ledger_finish(l, s.id, ETUDE_CLEANUP_CONFIRMED) == 0, "collection root has ended");
    reserve(l, "collection-other");
    blocked = start(ledger, "collection-other", fix_dir(), 1, 0, 200);
    CHECK(blocked == NULL && errno == ETIMEDOUT,
          "a surviving collection claim retains the host and port block");
    finish(l, "collection-other", blocked);
    CHECK(locked_by(port, F_WRLCK) == pid, "uncertain collection case retains its port block");
    CHECK(etude_ledger_finish(l, "collection:case", ETUDE_CLEANUP_CONFIRMED) == 0,
          "collection case cleanup is confirmed");
    etude_bridge_join(b);
    forget(pid);
    CHECK(lock_try(port, F_WRLCK) == 0, "collection block is free after every claim ends");
    close(port);
}

/** Shared old runs and new claims agree on host and distinct port locks. */
static void shared(struct etude_ledger *l, const char *ledger)
{
    struct etude_bridge *a, *b, *c;
    int host = lock_file("rach2-host.lock");
    int port = lock_file("rach2-ports.0");
    int gate = lock_file("rach2-host.gate");

    CHECK(lock_try(host, F_RDLCK) == 0 && lock_try(port, F_WRLCK) == 0, "old shared");
    reserve(l, "first");
    a = start(ledger, "first", fix_dir(), 0, 1, 0);
    CHECK(a != NULL, "shared admits");
    if (a != NULL) {
        CHECK(etude_bridge_offset(a) == 1000, "old port avoided");
        CHECK(locked_by(host, F_WRLCK) == etude_bridge_pid(a), "shared host held");
        CHECK(locked_by(gate, F_WRLCK) == 0, "shared gate released");
    }
    reserve(l, "second");
    b = start(ledger, "second", fix_dir(), 0, 1, 0);
    CHECK(b != NULL, "second admits");
    if (b != NULL)
        CHECK(etude_bridge_offset(b) == 2000, "distinct blocks");
    reserve(l, "large");
    c = start(ledger, "large", fix_dir(), 1, 0, 0);
    CHECK(c == NULL && errno == EAGAIN, "exclusive waits for old and new");
    finish(l, "large", c);
    finish(l, "second", b);
    finish(l, "first", a);
    close(port);
    close(host);
    close(gate);
}

/** Exclusive old runs and new claims exclude one another in both orders. */
static void exclusive(struct etude_ledger *l, const char *ledger)
{
    struct etude_bridge *b;
    int host = lock_file("rach2-host.lock");
    int gate = lock_file("rach2-host.gate");

    CHECK(lock_try(gate, F_WRLCK) == 0 && lock_try(host, F_WRLCK) == 0, "old exclusive");
    reserve(l, "new-shared");
    b = start(ledger, "new-shared", fix_dir(), 0, 0, 0);
    CHECK(b == NULL && errno == EAGAIN, "old exclusive excludes new");
    finish(l, "new-shared", b);
    close(host);
    close(gate);
    reserve(l, "new-exclusive");
    b = start(ledger, "new-exclusive", fix_dir(), 1, 0, 0);
    CHECK(b != NULL, "exclusive admits");
    host = lock_file("rach2-host.lock");
    gate = lock_file("rach2-host.gate");
    if (b != NULL) {
        CHECK(etude_bridge_offset(b) == -1, "native needs no legacy block");
        CHECK(lock_try(host, F_RDLCK) != 0, "new exclusive excludes old");
        CHECK(lock_try(gate, F_RDLCK) != 0, "exclusive gate held");
    }
    finish(l, "new-exclusive", b);
    CHECK(lock_try(host, F_WRLCK) == 0, "released after cleanup");
    close(host);
    close(gate);
}

/** Refuse a caller's offset until a real block is owned, and exhaust blocks. */
static void ports(struct etude_ledger *l, const char *ledger)
{
    struct etude_bridge *b;
    int fd[10], i;
    char name[64];

    for (i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "rach2-ports.%d", i);
        fd[i] = lock_file(name);
        CHECK(lock_try(fd[i], F_WRLCK) == 0, "old blocks taken");
    }
    reserve(l, "no-block");
    b = start(ledger, "no-block", fix_dir(), 0, 1, 30);
    CHECK(b == NULL && errno == ETIMEDOUT, "all blocks occupied");
    finish(l, "no-block", b);
    for (i = 0; i < 10; i++)
        close(fd[i]);
    CHECK(!etude_bridge_native_port(24999), "native avoids legacy range");
    CHECK(etude_bridge_native_port(25000) && etude_bridge_native_port(65535), "native range");
    CHECK(!etude_bridge_native_port(65536), "native upper bound");
}

#if defined(__linux__)
/* The running processes of the fixture job's admission claim. */
struct runner_claim {
    long keeper, process;
    int found;
    enum etude_claim_status status;
    char why[96]; /* what keeps the claim, when it is kept */
};

/** Record the fixture job's admission claim and status. */
static int runner_visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct runner_claim *q = ctx;

    if (strcmp(c->id, "runner-cleanup") == 0) {
        q->found = 1;
        q->status = c->status;
        snprintf(q->why, sizeof(q->why), "%s", c->why);
    }
    return 0;
}

/** Read the fixture's registered processes from the ledger's durable file. */
static void runner_processes(const char *ledger, struct runner_claim *q)
{
    char path[4096], line[1024], role[16];
    int own = 0;
    long pid;
    FILE *f;

    snprintf(path, sizeof(path), "%s/claims", ledger);
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "claim ", 6) == 0)
            own = strcmp(line, "claim runner-cleanup\n") == 0;
        if (!own || sscanf(line, "process %15s %ld", role, &pid) != 2)
            continue;
        if (strcmp(role, "keeper") == 0)
            q->keeper = pid;
        if (strcmp(role, "case") == 0)
            q->process = pid;
    }
    fclose(f);
}

/** Read the fixture's marker as the runner does, from /proc/PID/environ. */
static int runner_tracks(long pid)
{
    char path[64], *entry = NULL;
    size_t cap = 0;
    FILE *f;
    int found = 0;

    snprintf(path, sizeof(path), "/proc/%ld/environ", pid);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    while (getdelim(&entry, &cap, '\0', f) >= 0)
        if (strcmp(entry, "RUNNER_TRACKING_ID=etude-admit-fixture") == 0)
            found = 1;
    if (ferror(f))
        found = -1;
    free(entry);
    fclose(f);
    return found;
}

/** Execute a fixture job with the runner marker in its initial environment. */
static pid_t runner_worker(const char *self, const char *ledger, const char *ready,
                           const char *blocked, int fd)
{
    pid_t worker = fork();

    if (worker == 0) {
        close(fd);
        setenv("RUNNER_TRACKING_ID", "etude-admit-fixture", 1);
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        execl(self, self, "--runner-worker", "-p", "-d", fix_dir(), ledger, "runner-cleanup",
              "test", "shared", "1,15,1", "10,20,10", "20", executable, "--bridge-command", "block",
              ready, blocked, "tracked", (char *)NULL);
        _exit(99);
    }
    active_worker = worker;
    return worker;
}

/**
 * Apply the runner's kill rule before the controller's loss starts cleanup.
 *
 * @return 1 when the rule killed the controller, `worker`, and 0 otherwise.
 */
static int runner_sweep(pid_t worker, pid_t holder, const struct runner_claim *q)
{
    long targets[4] = {q->keeper, holder, q->process, worker};
    int i, killed = 0;

    for (i = 0; i < 4; i++)
        if (targets[i] > 0 && runner_tracks(targets[i]) == 1) {
            kill((pid_t)targets[i], SIGKILL);
            killed |= targets[i] == worker;
        }
    return killed;
}

/**
 * Wait for `holder`, the fixture's lock holder and this process's child once
 * the controller has died, while ledger `l` may yet free the claim it waits
 * for. `*gone` is set once the holder is reaped, or is no child to reap.
 *
 * @return 1 when the holder exited with status 0; 0 when it ended otherwise,
 *         or when the ledger keeps the claim uncertain, which recovery never
 *         frees, with the reason on standard error.
 */
static int runner_released(struct etude_ledger *l, pid_t holder, int *gone)
{
    struct runner_claim q;
    pid_t r;
    int st;

    *gone = 0;
    for (;;) {
        r = waitpid(holder, &st, WNOHANG);
        if (r < 0 && errno == EINTR)
            continue;
        if (r != 0) {
            *gone = 1;
            if (r == holder)
                forget(holder);
            return r == holder && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
        memset(&q, 0, sizeof(q));
        if (l == NULL || etude_ledger_list(l, runner_visit, &q) != 0)
            return 0;
        if (q.found && q.status == ETUDE_CLAIM_UNCERTAIN) {
            fprintf(stderr, "etude-bridge-test: runner: the claim is left uncertain: %s\n", q.why);
            return 0;
        }
        /* interval: the holder and ledger are read again every 10 ms */
        poll(NULL, 0, 10);
    }
}

/**
 * End fixture processes and remove its private claim after a failed sweep,
 * and then end and reap its lock holder, `holder`, unless it is `gone`.
 */
static void runner_dispose(struct etude_ledger *l, const struct runner_claim *q, pid_t holder,
                           int gone)
{
    if (q->keeper > 0)
        kill((pid_t)q->keeper, SIGKILL);
    if (q->process > 0)
        kill((pid_t)-q->process, SIGKILL);
    if (l != NULL) {
        if (q->found)
            etude_ledger_drop(l, "runner-cleanup");
        etude_ledger_close(l);
    }
    if (holder > 0 && !gone) {
        kill(holder, SIGKILL);
        while (waitpid(holder, NULL, 0) < 0 && errno == EINTR) {
        }
        forget(holder);
    }
}

/**
 * Wait for the fixture's case to write its byte to `fd`, its ready FIFO,
 * while `worker` runs; 1 once it has, 0 when the worker ended first.
 */
static int case_ready(int fd, pid_t worker)
{
    struct pollfd p;
    siginfo_t si;
    char c;

    for (;;) {
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        /* interval: the worker is looked at again every 10 ms */
        if (poll(&p, 1, 10) == 1)
            return read(fd, &c, 1) == 1;
        memset(&si, 0, sizeof(si));
        if (waitid(P_PID, (id_t)worker, &si, WEXITED | WNOHANG | WNOWAIT) != 0 || si.si_pid != 0)
            return 0;
    }
}

/**
 * Sweep a job's marked processes after its case is ready. Its keeper and
 * lock holder must survive, end the case and release the admission claim.
 * The case ends whatever the bridge does: a controller the rule misses is
 * killed too, and a claim the ledger keeps uncertain, with its holder, is
 * reported and then ended.
 */
static void runner_cleanup(const char *self, const char *ledger)
{
    const char *ready = fix_path("runner-ready");
    const char *blocked = fix_path("runner-blocked");
    struct runner_claim q;
    struct etude_ledger *l;
    struct etude_pid id;
    pid_t worker, holder;
    int fd, host, st, gone = 1;

    memset(&q, 0, sizeof(q));
    CHECK(mkfifo(ready, 0600) == 0 && mkfifo(blocked, 0600) == 0, "runner fixtures");
    fd = open(ready, O_RDWR | O_NONBLOCK);
    worker = runner_worker(self, ledger, ready, blocked, fd);
    CHECK(case_ready(fd, worker), "runner case ready");
    l = etude_ledger_open(ledger);
    runner_processes(ledger, &q);
    CHECK(l != NULL && etude_ledger_list(l, runner_visit, &q) == 0 && q.found && q.keeper > 0 &&
              q.process > 0,
          "runner processes registered");
    host = lock_file("rach2-host.lock");
    holder = (pid_t)locked_by(host, F_RDLCK);
    if (holder > 0)
        remember(holder);
    CHECK(holder > 0 && runner_tracks(holder) == 0, "runner holder unmarked");
    CHECK(q.keeper > 0 && runner_tracks(q.keeper) == 0, "runner keeper unmarked");
    CHECK(worker > 0 && runner_tracks(worker) == 1 && q.process > 0 &&
              runner_tracks(q.process) == 1,
          "runner controller and case marked");
    if (!runner_sweep(worker, holder, &q))
        kill(worker, SIGKILL);
    CHECK(waitpid(worker, &st, 0) == worker, "runner controller reaped");
    active_worker = 0;
    CHECK(holder > 0 && runner_released(l, holder, &gone), "runner holder confirms cleanup");
    q.found = 0;
    CHECK(l != NULL && etude_ledger_list(l, runner_visit, &q) == 0 && !q.found,
          "runner sweep releases claim");
    CHECK(q.keeper > 0 && q.process > 0 && etude_process_state(q.keeper, &id) == 0 &&
              etude_process_state(q.process, &id) == 0,
          "runner sweep leaves no case");
    runner_dispose(l, &q, holder, gone);
    CHECK(lock_try(host, F_WRLCK) == 0, "runner sweep releases locks");
    close(host);
    close(fd);
}

/** A killed controller keeps the locks until its keeper has ended the case. */
static void lost_controller(const char *ledger)
{
    const char *ready = fix_path("ready");
    const char *blocked = fix_path("blocked");
    int fd, host, st;
    pid_t worker, holder;

    CHECK(mkfifo(ready, 0600) == 0 && mkfifo(blocked, 0600) == 0, "controller fixtures");
    fd = open(ready, O_RDWR | O_NONBLOCK);
    worker = fork();
    if (worker == 0) {
        char *args[] = {"etude",
                        "-p",
                        "-d",
                        (char *)fix_dir(),
                        (char *)ledger,
                        "lost-controller",
                        "test",
                        "shared",
                        "1,15,1",
                        "10,20,10",
                        "20",
                        executable,
                        "--bridge-command",
                        "block",
                        (char *)ready,
                        (char *)blocked,
                        "ordinary",
                        NULL};
        close(fd);
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        optind = 1;
        _exit(etude_admit_main((int)(sizeof(args) / sizeof(args[0])) - 1, args));
    }
    active_worker = worker;
    CHECK(case_ready(fd, worker), "case ready before loss");
    host = lock_file("rach2-host.lock");
    holder = (pid_t)locked_by(host, F_RDLCK);
    if (holder > 0)
        remember(holder);
    CHECK(holder > 0, "large declaration forces exclusive host");
    kill(worker, SIGKILL);
    CHECK(waitpid(worker, &st, 0) == worker, "controller killed");
    active_worker = 0;
    if (holder > 0)
        CHECK(waitpid(holder, &st, 0) == holder, "confirmed cleanup releases orphan holder");
    forget(holder);
    CHECK(lock_try(host, F_WRLCK) == 0, "old run admitted after lost controller cleanup");
    close(host);
    close(fd);
}

/** A dead worker leaves an uncertain claim and its old locks reserved. */
static void abandoned(const char *ledger)
{
    int p[2], st, host;
    long keeper = 0;
    pid_t worker;
    struct etude_ledger *l;
    struct etude_bridge *b;

    CHECK(pipe(p) == 0, "worker pipe");
    worker = fork();
    if (worker == 0) {
        close(p[0]);
        l = etude_ledger_open(ledger);
        reserve(l, "abandoned");
        b = start(ledger, "abandoned", fix_dir(), 1, 1, 0);
        if (b == NULL)
            _exit(2);
        keeper = etude_bridge_pid(b);
        etude_ledger_finish(l, "abandoned", ETUDE_CLEANUP_UNCONFIRMED);
        if (write(p[1], &keeper, sizeof(keeper)) != sizeof(keeper))
            _exit(3);
        _exit(0);
    }
    close(p[1]);
    active_worker = worker;
    CHECK(read(p[0], &keeper, sizeof(keeper)) == sizeof(keeper), "worker ready");
    close(p[0]);
    if (keeper > 0)
        remember(keeper);
    CHECK(waitpid(worker, &st, 0) == worker && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "worker ended");
    active_worker = 0;
    l = etude_ledger_open(ledger);
    reserve(l, "blocked-by-abandoned");
    b = start(ledger, "blocked-by-abandoned", fix_dir(), 0, 0, 500);
    CHECK(b == NULL && errno == ETIMEDOUT, "uncertain retains locks after death");
    finish(l, "blocked-by-abandoned", b);
    host = lock_file("rach2-host.lock");
    CHECK(locked_by(host, F_RDLCK) == keeper, "orphan holder alive");
    CHECK(etude_ledger_drop(l, "abandoned") == 0, "operator confirms cleanup");
    CHECK(waitpid((pid_t)keeper, &st, 0) == keeper, "orphan holder reaped");
    forget(keeper);
    CHECK(lock_try(host, F_WRLCK) == 0, "operator releases locks");
    close(host);
    etude_ledger_close(l);
}
#endif

/** Start the bridge's main in a child with ARGS, a NULL-terminated list. */
static pid_t bridge_child(char **args)
{
    pid_t pid = fork();
    int n = 0;

    if (pid == 0) {
        while (args[n] != NULL)
            n++;
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        optind = 1;
        _exit(etude_admit_main(n, args));
    }
    active_worker = pid;
    return pid;
}

/** Wait for the bridge's child and give its exit status, or -1. */
static int bridge_status(pid_t pid)
{
    int st;

    active_worker = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

/** Say whether the result file at `path` is the one line `want`. */
static int recorded(const char *path, const char *want)
{
    char line[64] = "";
    FILE *f = fopen(path, "r");

    if (f == NULL)
        return 0;
    if (fgets(line, sizeof(line), f) == NULL)
        line[0] = '\0';
    fclose(f);
    line[strcspn(line, "\n")] = '\0';
    return strcmp(line, want) == 0;
}

/* The claim a row looks for in the ledger. */
static const char *sought;

/** Note whether the sought request waits in the ledger. */
static int waiting_visit(const struct etude_ledger_claim *c, void *ctx)
{
    int *found = ctx;

    if (strcmp(c->id, sought) == 0 &&
        (strcmp(c->state, "waiting") == 0 || strcmp(c->state, "blocked") == 0))
        *found = 1;
    return 0;
}

/** Wait for request `id` to wait in the ledger while its bridge `pid` runs;
 * 1 when it does, 0 when the bridge ended first. */
static int waits_in(struct etude_ledger *l, const char *id, pid_t pid)
{
    siginfo_t si;
    int found = 0;

    sought = id;
    for (;;) {
        if (etude_ledger_list(l, waiting_visit, &found) != 0)
            return 0;
        memset(&si, 0, sizeof(si));
        if (found || waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0 ||
            si.si_pid != 0)
            return found;
        /* interval: the ledger is read again every 10 ms */
        poll(NULL, 0, 10);
    }
}

/** Hold fifteen of twenty units of memory, which a claim of ten cannot join. */
static void hold(struct etude_ledger *l)
{
    struct etude_ledger_res res = {"memory_bytes", 15};
    struct etude_ledger_res cap = {"memory_bytes", 20};
    CHECK(etude_ledger_admit(l, "holding", "test", &res, 1, &cap, 1, 0) == 0, "hold");
}

/**
 * A command waits with -w until a claim ends and room is free, and then the
 * result file gives its own exit status; a wait past its bound and a stop
 * during it are recorded as such and run nothing.
 */
static void waiting(struct etude_ledger *l, const char *ledger)
{
    const char *rec = fix_path("result");
    const char *ran = fix_path("ran");
    char *args[] = {"etude",
                    "-d",
                    (char *)fix_dir(),
                    "-w",
                    "10",
                    "-r",
                    (char *)rec,
                    (char *)ledger,
                    "waiter",
                    "test",
                    "shared",
                    "1,10,1",
                    "10,20,10",
                    "5",
                    executable,
                    "--bridge-command",
                    "touch",
                    (char *)ran,
                    NULL};
    pid_t pid;
    int st;

    hold(l);
    pid = bridge_child(args);
    CHECK(waits_in(l, "waiter", pid), "waits for admission");
    CHECK(access(ran, F_OK) != 0, "nothing runs while it waits");
    CHECK(etude_ledger_finish(l, "holding", ETUDE_CLEANUP_CONFIRMED) == 0, "room freed");
    st = bridge_status(pid);
    CHECK(st == 2 && access(ran, F_OK) == 0, "admitted once room is free");
    CHECK(recorded(rec, "exited 2"), "command status kept apart");

    hold(l);
    args[4] = "1";
    args[8] = "expires";
    unlink(ran);
    pid = bridge_child(args);
    st = bridge_status(pid);
    CHECK(st == 2 && access(ran, F_OK) != 0, "admission wait bounded");
    CHECK(recorded(rec, "refused"), "refusal recorded");

    args[4] = "30";
    args[8] = "stopped";
    pid = bridge_child(args);
    CHECK(waits_in(l, "stopped", pid), "stop waits first");
    kill(pid, SIGTERM);
    st = bridge_status(pid);
    CHECK(st == 128 + SIGTERM && access(ran, F_OK) != 0, "stop during wait");
    CHECK(recorded(rec, "stopped 15"), "stop recorded");
    CHECK(etude_ledger_finish(l, "holding", ETUDE_CLEANUP_CONFIRMED) == 0, "hold ended");
}

/** Hold nine of ten processors, one unit of memory and nine of ten of disk. */
static void busy(struct etude_ledger *l)
{
    struct etude_ledger_res res[] = {{"cpu_millis", 9}, {"memory_bytes", 1}, {"disk_bytes", 9}};
    struct etude_ledger_res cap[] = {{"cpu_millis", 10}, {"memory_bytes", 20}, {"disk_bytes", 10}};
    CHECK(etude_ledger_admit(l, "busy", "test", res, 3, cap, 3, 0) == 0, "busy");
}

/**
 * A claim takes memory alone, so a command runs beside claims that hold more
 * of the processors and the disk than the capacity has left.
 */
static void processors(struct etude_ledger *l, const char *ledger)
{
    char *args[] = {"etude",       "-d",   (char *)fix_dir(), (char *)ledger,
                    "beside-busy", "test", "shared",          "5,1,5",
                    "10,20,10",    "5",    executable,        "--bridge-command",
                    "pass",        NULL};

    busy(l);
    CHECK(bridge_status(bridge_child(args)) == 0, "processors and disk not judged");
    CHECK(etude_ledger_finish(l, "busy", ETUDE_CLEANUP_CONFIRMED) == 0, "busy ended");
}

/** A command killed by a signal fails, and its result is incomplete. */
static void incomplete(const char *ledger)
{
    const char *rec = fix_path("result");
    char *args[] = {"etude",
                    "-d",
                    (char *)fix_dir(),
                    "-r",
                    (char *)rec,
                    (char *)ledger,
                    "killed",
                    "test",
                    "shared",
                    "1,1,1",
                    "10,20,10",
                    "5",
                    executable,
                    "--bridge-command",
                    "kill",
                    NULL};

    CHECK(bridge_status(bridge_child(args)) == 2, "killed command fails");
    CHECK(recorded(rec, "incomplete"), "incomplete recorded");
}

/** Run the external wrapper on a fixture old checkout, preserving its exit. */
static void wrapper(const char *ledger)
{
    const char *rec = fix_path("result");
    char *args[] = {"etude",
                    "-p",
                    "-d",
                    (char *)fix_dir(),
                    "-r",
                    (char *)rec,
                    (char *)ledger,
                    "old-checkout",
                    "test",
                    "shared",
                    "1,15,1",
                    "10,20,10",
                    "5",
                    executable,
                    "--bridge-command",
                    "registered",
                    (char *)ledger,
                    NULL};

    CHECK(bridge_status(bridge_child(args)) == 7, "old checkout offset registration and status");
    CHECK(recorded(rec, "exited 7"), "exit recorded");
}

/** A missing program gives its path and the system error on stderr. */
static void missing_program(const char *ledger)
{
    const char *program = fix_path("no-program");
    FILE *err = fopen(fix_path("missing-stderr"), "w+");
    char actual[4096], expected[4096];
    size_t n;
    pid_t pid;
    int st;

    CHECK(err != NULL, "missing program stderr file");
    if (err == NULL)
        return;
    pid = fork();
    CHECK(pid >= 0, "missing program fork");
    if (pid == 0) {
        char *args[] = {"etude",           "-d",   (char *)fix_dir(), (char *)ledger,
                        "missing-program", "test", "shared",          "1,1,1",
                        "10,20,10",        "5",    (char *)program,   NULL};
        if (dup2(fileno(err), STDERR_FILENO) < 0)
            _exit(99);
        fclose(err);
        optind = 1;
        _exit(etude_admit_main(11, args));
    }
    if (pid > 0) {
        active_worker = pid;
        CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 2,
              "missing program status");
        active_worker = 0;
        rewind(err);
        n = fread(actual, 1, sizeof(actual) - 1, err);
        actual[n] = '\0';
        snprintf(expected, sizeof(expected), "etude admit: start: %s: %s\n", program,
                 strerror(ENOENT));
        CHECK(!ferror(err) && strcmp(actual, expected) == 0, "missing program diagnostic");
    }
    fclose(err);
}

static int grant_resources(const struct etude_ledger_claim *claim, void *context)
{
    static const char *const names[] = {"cpu_millis", "memory_bytes", "disk_bytes"};
    if (strcmp(claim->id, "grant:prepare"))
        return 0;
    unsigned found = 0;
    for (size_t i = 0; i < claim->nres; i++)
        for (size_t j = 0; j < 3; j++)
            if (!strcmp(claim->res[i].name, names[j]) && claim->res[i].amount == 1)
                found |= 1u << j;
    *(int *)context = claim->nres == 3 && found == 7;
    return 0;
}

/* A failed grant publication releases the claim before any command starts. */
static void grant_seed(struct etude_ledger *l, const char *ledger)
{
    struct request q = {0};
    struct etude_preparation_grant grant;
    struct etude_reading before;
    q.ledger = ledger;
    q.dir = fix_dir();
    q.id = "grant:prepare";
    q.account = "fixture";
    static const char *const names[] = {"cpu_millis", "memory_bytes", "disk_bytes"};
    for (size_t i = 0; i < 3; i++) {
        q.res[i].name = q.cap[i].name = names[i];
        q.res[i].amount = 1;
        q.cap[i].amount = 20;
    }
    CHECK(!etude_history_now(&before), "grant clock reads");
    setenv("ETUDE_PREPARATION_GRANT", fix_path("admission.grant"), 1);
    struct etude_bridge *b = take(l, &q);
    CHECK(b && !etude_context_grant_read(fix_path("admission.grant"), q.id, &grant) &&
              grant.start.monotonic_us >= before.monotonic_us,
          "admission records its grant before locks");
    int resources = 0;
    CHECK(!etude_ledger_list(l, grant_resources, &resources) && resources,
          "measured preparation reserves CPU, memory and disk");
    CHECK(!etude_ledger_finish(l, q.id, ETUDE_CLEANUP_CONFIRMED), "grant fixture claim ends");
    if (b)
        etude_bridge_join(b);
    b = take(l, &q);
    CHECK(!b, "existing grant refuses admission before a child");
    if (b) {
        etude_ledger_finish(l, q.id, ETUDE_CLEANUP_CONFIRMED);
        etude_bridge_join(b);
    }
    CHECK(etude_ledger_finish(l, q.id, ETUDE_CLEANUP_CONFIRMED) != 0 && errno == ENOENT,
          "grant write failure leaves no claim");
    q.res[0].amount = 21;
    CHECK(admit(l, "grant-cpu", q.account, q.res, q.cap, 0) != 0 && errno == ERANGE,
          "measured preparation cannot exceed CPU capacity");
    q.res[0].amount = 1;
    q.res[2].amount = 21;
    CHECK(admit(l, "grant-disk", q.account, q.res, q.cap, 0) != 0 && errno == ERANGE,
          "measured preparation cannot exceed disk capacity");
    unsetenv("ETUDE_PREPARATION_GRANT");
    q.res[0].amount = 21;
    CHECK(!admit(l, "compatibility", q.account, q.res, q.cap, 0),
          "unmeasured compatibility admission still reserves only memory");
    CHECK(!etude_ledger_finish(l, "compatibility", ETUDE_CLEANUP_CONFIRMED),
          "compatibility fixture claim ends");
}

/** Exercise the bridge with private locks, without using host admission. */
int main(int argc, char **argv)
{
    struct etude_ledger *l;
    const char *ledger;
    struct etude_bridge *b;

    if (argc > 1 && strcmp(argv[1], "--bridge-command") == 0)
        return fixture_command(argc, argv);
    if (realpath(argv[0], executable) == NULL)
        return 1;
    if (argc > 1 && strcmp(argv[1], "--runner-worker") == 0)
        return etude_admit_main(argc - 1, argv + 1);
    atexit(cleanup_test);
    signal(SIGTERM, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGHUP, interrupted);
    ledger = fix_path("ledger");
#if defined(__linux__)
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0, "adopt holders");
#endif
    l = etude_ledger_open(ledger);
    CHECK(l != NULL, "open ledger");
    if (l == NULL)
        return 1;
    grant_seed(l, ledger);
    if (argc == 2 && !strcmp(argv[1], "--grant-only")) {
        etude_ledger_close(l);
        if (!fails)
            fix_passed();
        return fails != 0;
    }
    b = start(ledger, "absent", fix_dir(), 0, 0, 0);
    CHECK(b == NULL && errno == ENOENT, "absent claim refused");
    collection(l, ledger);
    shared(l, ledger);
    exclusive(l, ledger);
    ports(l, ledger);
    waiting(l, ledger);
    processors(l, ledger);
    etude_ledger_close(l);
    incomplete(ledger);
    wrapper(ledger);
    missing_program(ledger);
#if defined(__linux__)
    runner_cleanup(argv[0], ledger);
    lost_controller(ledger);
    abandoned(ledger);
#endif
    if (!fails)
        fix_passed();
    return fails != 0;
}
