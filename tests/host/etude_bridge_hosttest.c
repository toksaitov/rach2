/**
 * Check the bridge against the POSIX record locks used by old runners.
 * host-deps: tools/etude/posix/bridge.c tools/etude/portlock.c
 * host-deps: tools/etude/ledger.c
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

#define main bridge_main
#include "../../tools/etude/posix/etude_bridge.c"
#undef main

static int fails;
#define CHECK(c, row)                                                      \
    do {                                                                   \
        if (!(c)) {                                                        \
            fprintf(stderr, "etude-bridge-test: fail: %s: %s\n", row, #c); \
            fails++;                                                       \
        }                                                                  \
    } while (0)

/* Only these fixture processes belong to this test. */
static pid_t active_worker;
static long holders[32];
static size_t nholders;

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

/** Try a whole-file lock, as runutil does. */
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
};

/** Note whether the fixture job still has an admission claim. */
static int runner_visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct runner_claim *q = ctx;

    if (strcmp(c->id, "runner-cleanup") == 0)
        q->found = 1;
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
        if (strcmp(entry, "RUNNER_TRACKING_ID=etude-bridge-fixture") == 0)
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
        setenv("RUNNER_TRACKING_ID", "etude-bridge-fixture", 1);
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        execl(self, self, "--runner-worker", "-p", "-d", fix_dir(), ledger, "runner-cleanup",
              "test", "shared", "1,15,1", "10,20,10", "20", "/bin/sh", "-c",
              "test \"$RUNNER_TRACKING_ID\" = etude-bridge-fixture || exit 90; "
              "printf . > \"$1\"; cat \"$2\"",
              "sh", ready, blocked, (char *)NULL);
        _exit(99);
    }
    active_worker = worker;
    return worker;
}

/** Apply the runner's kill rule before the controller's loss starts cleanup. */
static void runner_sweep(pid_t worker, pid_t holder, const struct runner_claim *q)
{
    long targets[4] = {q->keeper, holder, q->process, worker};
    int i;

    for (i = 0; i < 4; i++)
        if (targets[i] > 0 && runner_tracks(targets[i]) == 1)
            kill((pid_t)targets[i], SIGKILL);
}

/** End fixture processes and remove its private claim after a failed sweep. */
static void runner_dispose(struct etude_ledger *l, const struct runner_claim *q)
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
}

/**
 * Sweep a job's marked processes after its case is ready. Its keeper and
 * lock holder must survive, end the case and release the admission claim.
 */
static void runner_cleanup(const char *self, const char *ledger)
{
    const char *ready = fix_path("runner-ready");
    const char *blocked = fix_path("runner-blocked");
    struct runner_claim q = {0, 0, 0};
    struct etude_ledger *l;
    struct etude_pid id;
    struct pollfd p;
    pid_t worker, holder, reaped = 0;
    int fd, host, st, i;
    char c;

    CHECK(mkfifo(ready, 0600) == 0 && mkfifo(blocked, 0600) == 0, "runner fixtures");
    fd = open(ready, O_RDWR | O_NONBLOCK);
    worker = runner_worker(self, ledger, ready, blocked, fd);
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 10000) == 1 && read(fd, &c, 1) == 1, "runner case ready");
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
    runner_sweep(worker, holder, &q);
    CHECK(waitpid(worker, &st, 0) == worker, "runner controller reaped");
    active_worker = 0;
    for (i = 0; holder > 0 && i < 1000; i++) {
        reaped = waitpid(holder, &st, WNOHANG);
        if (reaped != 0)
            break;
        poll(NULL, 0, 10);
    }
    CHECK(reaped == holder && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "runner holder confirms cleanup");
    if (reaped == holder)
        forget(holder);
    q.found = 0;
    CHECK(l != NULL && etude_ledger_list(l, runner_visit, &q) == 0 && !q.found,
          "runner sweep releases claim");
    CHECK(q.keeper > 0 && q.process > 0 && etude_process_state(q.keeper, &id) == 0 &&
              etude_process_state(q.process, &id) == 0,
          "runner sweep leaves no case");
    runner_dispose(l, &q);
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
    struct pollfd p;
    char c;

    CHECK(mkfifo(ready, 0600) == 0 && mkfifo(blocked, 0600) == 0, "controller fixtures");
    fd = open(ready, O_RDWR | O_NONBLOCK);
    worker = fork();
    if (worker == 0) {
        char *args[] = {"etude-bridge",
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
                        "/bin/sh",
                        "-c",
                        "printf . > \"$1\"; cat \"$2\"",
                        "sh",
                        (char *)ready,
                        (char *)blocked,
                        NULL};
        close(fd);
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        optind = 1;
        _exit(bridge_main(17, args));
    }
    active_worker = worker;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 10000) == 1 && read(fd, &c, 1) == 1, "case ready before loss");
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
        _exit(bridge_main(n, args));
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

/** Wait up to ten seconds for request `id` to wait in the ledger. */
static int waits_in(struct etude_ledger *l, const char *id)
{
    int i, found = 0;

    sought = id;
    for (i = 0; i < 1000 && !found; i++) {
        if (etude_ledger_list(l, waiting_visit, &found) != 0)
            return 0;
        if (!found)
            poll(NULL, 0, 10);
    }
    return found;
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
    char *args[] = {"etude-bridge",
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
                    "/bin/sh",
                    "-c",
                    ": > \"$1\"; exit 2",
                    "sh",
                    (char *)ran,
                    NULL};
    pid_t pid;
    int st;

    hold(l);
    pid = bridge_child(args);
    CHECK(waits_in(l, "waiter"), "waits for admission");
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
    CHECK(waits_in(l, "stopped"), "stop waits first");
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
    char *args[] = {"etude-bridge", "-d",   (char *)fix_dir(), (char *)ledger,
                    "beside-busy",  "test", "shared",          "5,1,5",
                    "10,20,10",     "5",    "/bin/sh",         "-c",
                    "exit 0",       NULL};

    busy(l);
    CHECK(bridge_status(bridge_child(args)) == 0, "processors and disk not judged");
    CHECK(etude_ledger_finish(l, "busy", ETUDE_CLEANUP_CONFIRMED) == 0, "busy ended");
}

/** A command killed by a signal fails, and its result is incomplete. */
static void incomplete(const char *ledger)
{
    const char *rec = fix_path("result");
    char *args[] = {"etude-bridge", "-d",   (char *)fix_dir(), "-r",    (char *)rec, (char *)ledger,
                    "killed",       "test", "shared",          "1,1,1", "10,20,10",  "5",
                    "/bin/sh",      "-c",   "kill -KILL $$",   NULL};

    CHECK(bridge_status(bridge_child(args)) == 2, "killed command fails");
    CHECK(recorded(rec, "incomplete"), "incomplete recorded");
}

/** Run the external wrapper on a fixture old checkout, preserving its exit. */
static void wrapper(const char *ledger)
{
    const char *rec = fix_path("result");
    char *args[] = {"etude-bridge",
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
                    "/bin/sh",
                    "-c",
                    "test \"$RACH2_PORT_OFFSET\" = 0 || exit 90; "
                    "grep -q 'state running' \"$1/claims\" || exit 91; exit 7",
                    "sh",
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
        char *args[] = {"etude-bridge",    "-d",   (char *)fix_dir(), (char *)ledger,
                        "missing-program", "test", "shared",          "1,1,1",
                        "10,20,10",        "5",    (char *)program,   NULL};
        if (dup2(fileno(err), STDERR_FILENO) < 0)
            _exit(99);
        fclose(err);
        optind = 1;
        _exit(bridge_main(11, args));
    }
    if (pid > 0) {
        active_worker = pid;
        CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 2,
              "missing program status");
        active_worker = 0;
        rewind(err);
        n = fread(actual, 1, sizeof(actual) - 1, err);
        actual[n] = '\0';
        snprintf(expected, sizeof(expected), "etude-bridge: start: %s: %s\n", program,
                 strerror(ENOENT));
        CHECK(!ferror(err) && strcmp(actual, expected) == 0, "missing program diagnostic");
    }
    fclose(err);
}

/** Exercise the bridge with private locks, without using host admission. */
int main(int argc, char **argv)
{
    struct etude_ledger *l;
    const char *ledger;
    struct etude_bridge *b;

    if (argc > 1 && strcmp(argv[1], "--runner-worker") == 0)
        return bridge_main(argc - 1, argv + 1);
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
    b = start(ledger, "absent", fix_dir(), 0, 0, 0);
    CHECK(b == NULL && errno == ENOENT, "absent claim refused");
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
