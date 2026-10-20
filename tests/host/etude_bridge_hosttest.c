/**
 * Check the bridge against the POSIX record locks used by old runners.
 * host-deps: tools/etude/posix/bridge.c tools/etude/ledger.c
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

/** Run the external wrapper on a fixture old checkout, preserving its exit. */
static void wrapper(const char *ledger)
{
    pid_t pid = fork();
    int st;

    if (pid == 0) {
        char *args[] = {"etude-bridge",
                        "-p",
                        "-d",
                        (char *)fix_dir(),
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
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        optind = 1;
        _exit(bridge_main(16, args));
    }
    active_worker = pid;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7,
          "old checkout offset registration and status");
    active_worker = 0;
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
int main(void)
{
    struct etude_ledger *l;
    const char *ledger;
    struct etude_bridge *b;

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
    etude_ledger_close(l);
    wrapper(ledger);
    missing_program(ledger);
#if defined(__linux__)
    lost_controller(ledger);
    abandoned(ledger);
#endif
    if (!fails)
        fix_passed();
    return fails != 0;
}
