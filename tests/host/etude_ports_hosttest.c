/**
 * Check `etude lock ports` against the host lock and the port blocks of the
 * old runner's protocol, and against a stand-in for an older checkout's
 * claim.
 * host-deps: tools/etude/portlock.c
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "compat/fixdir_host.c"

#include "verbs.h"
#include "../../tools/etude/lock_ports.c"

/* The stand-in for an older checkout's claim, from the root of the tree. */
#define STAND_IN "tests/system/fixtures/old_ports.py"

/* How long a run that must wait is watched for a start, in milliseconds. */
#define WATCH_MS 600

static int fails;
#define CHECK(c, row)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "etude-lock-ports-test: fail: %s: %s\n", row, #c); \
            fails++;                                                           \
        }                                                                      \
    } while (0)

/* The fixture processes this test has started and not yet reaped. */
static pid_t children[64];

/** Keep `pid` for the cleanup of an interrupted test. */
static void remember(pid_t pid)
{
    size_t i;

    for (i = 0; i < sizeof(children) / sizeof(children[0]); i++)
        if (children[i] == 0) {
            children[i] = pid;
            return;
        }
}

/** Forget `pid`, which has been reaped. */
static void forget(pid_t pid)
{
    size_t i;

    for (i = 0; i < sizeof(children) / sizeof(children[0]); i++)
        if (children[i] == pid)
            children[i] = 0;
}

/** Wait for `pid` and forget it; give its wait status, or -1. */
static int reap(pid_t pid)
{
    int st = -1;

    if (pid <= 0)
        return -1;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    forget(pid);
    return st;
}

/** Kill and reap every fixture process still running. */
static void cleanup_test(void)
{
    size_t i;

    for (i = 0; i < sizeof(children) / sizeof(children[0]); i++)
        if (children[i] > 0) {
            kill(children[i], SIGKILL);
            reap(children[i]);
        }
}

/** End the test through its exit cleanup. */
static void interrupted(int sig)
{
    exit(128 + sig);
}

/** Sleep `ms` milliseconds. */
static void nap_ms(long ms)
{
    struct timespec t;

    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    /* interval: a run is looked at again, or watched while it must wait */
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/** Give `dir/name` in `buf`, of 4096 bytes. */
static char *in(char *buf, const char *dir, const char *name)
{
    snprintf(buf, 4096, "%s/%s", dir, name);
    return buf;
}

/** Whether the fixture file `name` exists. */
static int exists(const char *name)
{
    char path[4096];

    return access(in(path, fix_dir(), name), F_OK) == 0;
}

/** Wait for the fixture file `name` to appear. */
static void await(const char *name)
{
    while (!exists(name))
        nap_ms(20);
}

/** Read the fixture file `name` into `buf`; "" when it cannot be read. */
static char *slurp(const char *name, char *buf, size_t size)
{
    char path[4096];
    FILE *f = fopen(in(path, fix_dir(), name), "r");
    size_t n = 0;

    if (f != NULL) {
        n = fread(buf, 1, size - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

/** Make the fixture file `name`. */
static void touch(const char *name)
{
    char path[4096];
    int fd = open(in(path, fix_dir(), name), O_WRONLY | O_CREAT, 0644);

    if (fd >= 0)
        close(fd);
}

/** Give the process whose lock on `dir/name` conflicts with `type`, or 0. */
static long locked_by(const char *dir, const char *name, short type)
{
    char path[4096];
    struct flock fl;
    int fd = open(in(path, dir, name), O_RDWR);
    long pid = -1;

    if (fd < 0)
        return -1;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_GETLK, &fl) == 0)
        pid = fl.l_type == F_UNLCK ? 0 : (long)fl.l_pid;
    close(fd);
    return pid;
}

/* A lock that a fixture process holds until it is let go. */
struct holder {
    pid_t pid;
    int release; /* closing it lets the process go */
};

/* The release ends of the holders that have not been let go, or -1. */
static int releases[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

/** Keep or forget the release end `fd`, which no other fixture may have. */
static void track(int fd, int keep)
{
    size_t i;

    for (i = 0; i < sizeof(releases) / sizeof(releases[0]); i++)
        if (releases[i] == (keep ? -1 : fd)) {
            releases[i] = keep ? fd : -1;
            return;
        }
}

/** Close, in a new fixture process, the release ends of the holders. */
static void close_releases(void)
{
    size_t i;

    for (i = 0; i < sizeof(releases) / sizeof(releases[0]); i++)
        if (releases[i] >= 0)
            close(releases[i]);
}

/**
 * Lock the fixture's `name` with `type` in a process of its own, as another
 * run would, and return once it holds the lock. A record lock belongs to a
 * process, and any close of the file in that process gives it back, so the
 * test itself holds none.
 */
static struct holder hold(const char *name, short type)
{
    struct holder h = {-1, -1};
    char path[4096], c;
    int ready[2], release[2];
    struct flock fl;

    if (pipe(ready) != 0 || pipe(release) != 0)
        return h;
    h.pid = fork();
    if (h.pid == 0) {
        int fd = open(in(path, fix_dir(), name), O_RDWR | O_CREAT, 0666);
        close_releases();
        memset(&fl, 0, sizeof(fl));
        fl.l_type = type;
        fl.l_whence = SEEK_SET;
        if (fd < 0 || fcntl(fd, F_SETLKW, &fl) != 0 || write(ready[1], "r", 1) != 1)
            _exit(1);
        close(release[1]);
        while (read(release[0], &c, 1) < 0 && errno == EINTR) {
        }
        _exit(0);
    }
    close(ready[1]);
    close(release[0]);
    if (h.pid > 0)
        remember(h.pid);
    if (h.pid < 0 || read(ready[0], &c, 1) != 1) {
        close(release[1]);
        reap(h.pid);
        h.pid = -1;
        h.release = -1;
    } else {
        h.release = release[1];
        track(h.release, 1);
    }
    close(ready[0]);
    return h;
}

/** Let the holder `h` go, which gives back its lock. */
static void let_go(struct holder *h)
{
    if (h->release >= 0) {
        track(h->release, 0);
        close(h->release);
    }
    reap(h->pid);
    h->pid = -1;
    h->release = -1;
}

/* One run of the helper, or of the stand-in, and its command. */
struct run {
    const char *name;   /* the run's files are NAME.on, NAME.go and NAME.err */
    int count;          /* the blocks it may take */
    const char *host;   /* RACH2_HOST_LOCK, or NULL for none */
    const char *script; /* the command, or NULL for one that waits for NAME.go */
    const char *dir;    /* the directory of the locks, or NULL for the fixture's */
    const char *offset; /* a RACH2_PORT_OFFSET already set, or NULL */
    int work;           /* RACH2_RUN_WORK names the fixture's directory */
    int old;            /* run the stand-in in place of the helper */
};

/**
 * Start `r` with standard error in NAME.err. The default command writes
 * the offset and RACH2_HOST_LOCK, or `unset`, to NAME.on, and waits for
 * NAME.go.
 */
static pid_t launch(const struct run *r)
{
    char script[8192], count[16], err[4096], name[256];
    const char *dir = r->dir != NULL ? r->dir : fix_dir();
    pid_t pid;
    int fd;

    if (r->script != NULL)
        snprintf(script, sizeof(script), "%s", r->script);
    else
        snprintf(script, sizeof(script),
                 "printf '%%s %%s\\n' \"$RACH2_PORT_OFFSET\" \"${RACH2_HOST_LOCK-unset}\" > "
                 "%s/%s.on.new && mv %s/%s.on.new %s/%s.on; "
                 "while [ ! -e %s/%s.go ]; do sleep 0.02; done",
                 fix_dir(), r->name, fix_dir(), r->name, fix_dir(), r->name, fix_dir(), r->name);
    snprintf(count, sizeof(count), "%d", r->count);
    snprintf(name, sizeof(name), "%s.err", r->name);
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"etude", (char *)dir, count, "/bin/sh", "-c", script, NULL};
        close_releases();
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        unsetenv("RACH2_HOST_LOCK");
        unsetenv("RACH2_RUN_WORK");
        unsetenv("RACH2_PORT_OFFSET");
        if (r->host != NULL)
            setenv("RACH2_HOST_LOCK", r->host, 1);
        if (r->offset != NULL)
            setenv("RACH2_PORT_OFFSET", r->offset, 1);
        if (r->work)
            setenv("RACH2_RUN_WORK", fix_dir(), 1);
        fd = open(in(err, fix_dir(), name), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        if (r->old) {
            execlp("python3", "python3", STAND_IN, dir, count, "/bin/sh", "-c", script,
                   (char *)NULL);
            _exit(126);
        }
        _exit(etude_lock_ports_main(6, argv));
    }
    if (pid > 0)
        remember(pid);
    return pid;
}

/** Let the run `name` end, and give its exit status, or -1 for a death by a
 * signal. */
static int finish(pid_t pid, const char *name)
{
    char go[256];
    int st;

    snprintf(go, sizeof(go), "%s.go", name);
    touch(go);
    st = reap(pid);
    return st >= 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/** Whether run `name` has started; its command has written NAME.on. */
static int started(const char *name)
{
    char on[256];

    snprintf(on, sizeof(on), "%s.on", name);
    return exists(on);
}

/** Wait for run `name` to start. */
static void starts(const char *name)
{
    char on[256];

    snprintf(on, sizeof(on), "%s.on", name);
    await(on);
}

/** Whether run `name` stays unstarted while it is watched. */
static int waits(const char *name)
{
    nap_ms(WATCH_MS);
    return !started(name);
}

/** Give what run `name` wrote to NAME.on. */
static char *seen(const char *name, char *buf, size_t size)
{
    char on[256];

    snprintf(on, sizeof(on), "%s.on", name);
    return slurp(on, buf, size);
}

/** Whether run `name` wrote `text` to its standard error. */
static int said(const char *name, const char *text)
{
    char err[256], buf[4096];

    snprintf(err, sizeof(err), "%s.err", name);
    return strstr(slurp(err, buf, sizeof(buf)), text) != NULL;
}

/** Wait for run `name` to start or to say that it waits; whether it started. */
static int runs(const char *name)
{
    while (!started(name) && !said(name, "waiting"))
        nap_ms(20);
    return started(name);
}

/** The lowest free block is taken, and kept while the command runs. */
static void blocks(void)
{
    struct holder h = hold("rach2-ports.0", F_WRLCK);
    struct run a = {"a", 3, NULL, NULL, NULL, NULL, 0, 0};
    struct run b = {"b", 3, NULL, NULL, NULL, NULL, 0, 0};
    char buf[256];
    pid_t pa, pb;

    CHECK(h.pid > 0, "lowest free block");
    pa = launch(&a);
    CHECK(runs("a") && strcmp(seen("a", buf, sizeof(buf)), "1000 unset\n") == 0,
          "lowest free block");
    CHECK(locked_by(fix_dir(), "rach2-ports.1", F_WRLCK) == (long)pa, "block kept while running");
    CHECK(said("a", "etude lock: ports: block 1, 1000 above the table's"), "lowest free block");
    let_go(&h);
    pb = launch(&b);
    CHECK(runs("b") && strcmp(seen("b", buf, sizeof(buf)), "0 unset\n") == 0, "lowest free block");
    CHECK(finish(pa, "a") == 0 && finish(pb, "b") == 0, "lowest free block");
    CHECK(locked_by(fix_dir(), "rach2-ports.1", F_WRLCK) == 0, "block given back at the end");
}

/** The command gets the offset of its block and not RACH2_HOST_LOCK. */
static void variables(void)
{
    struct run c = {"c", 1, "shared", NULL, NULL, "5000", 0, 0};
    struct run d = {"d", 1, NULL, "exit 7", NULL, NULL, 0, 0};
    char buf[256];
    pid_t pc = launch(&c);

    CHECK(runs("c") && strcmp(seen("c", buf, sizeof(buf)), "0 unset\n") == 0, "variables");
    CHECK(finish(pc, "c") == 0, "variables");
    CHECK(finish(launch(&d), "d") == 7, "command status");
}

/** A run that needs the host alone waits for a shared one, and the reverse. */
static void alone(void)
{
    struct holder h = hold("rach2-host.lock", F_RDLCK);
    struct run e = {"e", 2, "alone", NULL, NULL, NULL, 1, 0};
    struct run f = {"f", 2, "shared", NULL, NULL, NULL, 0, 0};
    struct run g = {"g", 2, "alone", NULL, NULL, NULL, 0, 0};
    char buf[8192], want[4200];
    pid_t pe, pf, pg;

    CHECK(h.pid > 0, "alone excludes shared");
    pe = launch(&e);
    CHECK(!runs("e") && waits("e"), "alone excludes shared");
    snprintf(want, sizeof(want), "lock WRITE %s/rach2-host.lock\n", fix_dir());
    slurp("claim.wait", buf, sizeof(buf));
    CHECK(strncmp(buf, "reason host\nsince ", 18) == 0 && strstr(buf, want) != NULL,
          "wait recorded");
    CHECK(said("e", "etude lock: host: waiting for the runs in progress to end"),
          "alone excludes shared");
    let_go(&h);
    starts("e");
    CHECK(!exists("claim.wait"), "wait recorded");
    CHECK(locked_by(fix_dir(), "rach2-host.lock", F_RDLCK) == (long)pe &&
              locked_by(fix_dir(), "rach2-host.gate", F_RDLCK) == (long)pe,
          "alone excludes shared");
    pf = launch(&f);
    CHECK(waits("f"), "alone excludes shared");
    CHECK(finish(pe, "e") == 0, "alone excludes shared");
    starts("f");
    pg = launch(&g);
    CHECK(waits("g"), "shared excludes alone");
    CHECK(finish(pf, "f") == 0, "shared excludes alone");
    starts("g");
    CHECK(finish(pg, "g") == 0, "shared excludes alone");
}

/**
 * A run that waits for the host alone keeps a shared run that starts after
 * it waiting, though the host lock has only shared holders.
 */
static void order(void)
{
    struct holder h = hold("rach2-host.lock", F_RDLCK);
    struct run i = {"i", 2, "alone", NULL, NULL, NULL, 0, 0};
    struct run j = {"j", 2, "shared", NULL, NULL, NULL, 0, 0};
    pid_t pi, pj;

    CHECK(h.pid > 0, "alone waits ahead of later shared");
    pi = launch(&i);
    nap_ms(WATCH_MS / 2);
    pj = launch(&j);
    CHECK(waits("i") && !runs("j"), "alone waits ahead of later shared");
    CHECK(said("j", "etude lock: host: waiting for a run that needs the host alone"),
          "alone waits ahead of later shared");
    /* A j that went ahead holds the host lock that i needs. */
    if (started("j"))
        (void)finish(pj, "j");
    let_go(&h);
    starts("i");
    CHECK(waits("j"), "alone waits ahead of later shared");
    CHECK(finish(pi, "i") == 0, "alone waits ahead of later shared");
    starts("j");
    CHECK(finish(pj, "j") == 0, "alone waits ahead of later shared");
}

/** Shared runs run at once, on blocks of their own, and keep no gate. */
static void together(void)
{
    struct run k = {"k", 2, "shared", NULL, NULL, NULL, 0, 0};
    struct run m = {"m", 2, "shared", NULL, NULL, NULL, 0, 0};
    char a[256], b[256];
    pid_t pk = launch(&k), pm = launch(&m);

    CHECK(runs("k") && runs("m"), "shared runs together");
    CHECK(strcmp(seen("k", a, sizeof(a)), seen("m", b, sizeof(b))) != 0, "shared runs together");
    CHECK(locked_by(fix_dir(), "rach2-host.gate", F_WRLCK) == 0, "shared gives back the gate");
    CHECK(finish(pk, "k") == 0 && finish(pm, "m") == 0, "shared runs together");
}

/** A stop during either wait ends the helper and starts no command. */
static void stops_wait(void)
{
    struct holder h = hold("rach2-ports.0", F_WRLCK);
    struct run n = {"n", 1, NULL, NULL, NULL, NULL, 1, 0};
    struct run p = {"p", 1, "shared", NULL, NULL, NULL, 1, 0};
    char want[4200], first[8192], again[8192];
    pid_t pn, pp;
    int st;

    CHECK(h.pid > 0, "stop during the wait");
    pn = launch(&n);
    CHECK(!runs("n"), "stop during the wait");
    slurp("claim.wait", first, sizeof(first));
    snprintf(want, sizeof(want), "lock WRITE %s/rach2-ports.0\n", fix_dir());
    CHECK(strncmp(first, "reason ports\nsince ", 19) == 0 && strstr(first, want) != NULL,
          "wait recorded");
    nap_ms(1200);
    CHECK(strcmp(slurp("claim.wait", again, sizeof(again)), first) == 0, "wait recorded");
    CHECK(said("n", "etude lock: ports: every block is held by another run; waiting"),
          "stop during the wait");
    kill(pn, SIGTERM);
    st = reap(pn);
    CHECK(st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGTERM, "stop during the wait");
    CHECK(!started("n") && !exists("claim.wait"), "stop during the wait");
    let_go(&h);
    h = hold("rach2-host.gate", F_WRLCK);
    pp = launch(&p);
    CHECK(!runs("p"), "stop during the host wait");
    kill(pp, SIGINT);
    st = reap(pp);
    CHECK(st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGINT, "stop during the host wait");
    CHECK(!started("p") && !exists("claim.wait"), "stop during the host wait");
    let_go(&h);
}

/** Give the exit status of the helper run with `argv`, its output dropped. */
static int status_of(int argc, char **argv, const char *host)
{
    pid_t pid = fork();
    int st, fd;

    if (pid == 0) {
        close_releases();
        fd = open("/dev/null", O_WRONLY);
        if (fd >= 0)
            dup2(fd, 2);
        if (host != NULL)
            setenv("RACH2_HOST_LOCK", host, 1);
        else
            unsetenv("RACH2_HOST_LOCK");
        _exit(etude_lock_ports_main(argc, argv));
    }
    if (pid > 0)
        remember(pid);
    st = reap(pid);
    return st >= 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/** A wrong usage, a lock file that is not one and a missing command. */
static void refusals(void)
{
    char dir[4096], bad[4096], path[4096], target[4096];
    char *good[] = {"etude", dir, "1", "true", NULL};
    char *none[] = {"etude", dir, "0", "true", NULL};
    char *many[] = {"etude", dir, "11", "true", NULL};
    char *word[] = {"etude", dir, "1x", "true", NULL};
    char *lost[] = {"etude", dir, "1", "/nonexistent/command", NULL};
    char *odd[] = {"etude", bad, "1", "true", NULL};

    in(dir, fix_dir(), "refusals");
    in(bad, fix_dir(), "bad");
    CHECK(mkdir(dir, 0700) == 0 && mkdir(bad, 0700) == 0, "refusals");
    CHECK(status_of(4, good, "both") == 2, "refusals");
    CHECK(status_of(4, none, NULL) == 2 && status_of(4, many, NULL) == 2 &&
              status_of(4, word, NULL) == 2 && status_of(3, good, NULL) == 2,
          "refusals");
    CHECK(status_of(4, lost, NULL) == 127, "refusals");
    CHECK(status_of(4, good, NULL) == 0, "refusals");
    CHECK(mkdir(in(path, bad, "rach2-ports.0"), 0700) == 0, "refusals");
    CHECK(status_of(4, odd, NULL) == 2, "refusals");
    in(target, bad, "elsewhere");
    CHECK(symlink(target, in(path, bad, "rach2-host.lock")) == 0, "refusals");
    CHECK(status_of(4, odd, "shared") == 2 && access(target, F_OK) != 0, "refusals");
}

/**
 * The stand-in for an older checkout and the helper keep each other out of
 * a block and of the host, whichever claims first.
 */
static void old_checkout(void)
{
    struct run o1 = {"o1", 2, NULL, NULL, NULL, NULL, 0, 1};
    struct run q1 = {"q1", 2, NULL, NULL, NULL, NULL, 0, 0};
    struct run q2 = {"q2", 2, NULL, NULL, NULL, NULL, 0, 0};
    struct run o2 = {"o2", 2, NULL, NULL, NULL, NULL, 0, 1};
    struct run o3 = {"o3", 2, "alone", NULL, NULL, NULL, 0, 1};
    struct run q3 = {"q3", 2, "shared", NULL, NULL, NULL, 0, 0};
    struct run q4 = {"q4", 2, "alone", NULL, NULL, NULL, 0, 0};
    struct run o4 = {"o4", 2, "shared", NULL, NULL, NULL, 0, 1};
    char buf[256];
    pid_t a, b;

    CHECK(access(STAND_IN, R_OK) == 0, "old checkout");
    a = launch(&o1);
    CHECK(runs("o1") && strcmp(seen("o1", buf, sizeof(buf)), "0 unset\n") == 0, "old checkout");
    b = launch(&q1);
    CHECK(runs("q1") && strcmp(seen("q1", buf, sizeof(buf)), "1000 unset\n") == 0, "old checkout");
    CHECK(finish(a, "o1") == 0 && finish(b, "q1") == 0, "old checkout");
    a = launch(&q2);
    CHECK(runs("q2") && strcmp(seen("q2", buf, sizeof(buf)), "0 unset\n") == 0, "old checkout");
    b = launch(&o2);
    CHECK(runs("o2") && strcmp(seen("o2", buf, sizeof(buf)), "1000 unset\n") == 0, "old checkout");
    CHECK(finish(a, "q2") == 0 && finish(b, "o2") == 0, "old checkout");
    a = launch(&o3);
    CHECK(runs("o3"), "old checkout");
    b = launch(&q3);
    CHECK(waits("q3"), "old checkout alone");
    CHECK(finish(a, "o3") == 0, "old checkout alone");
    starts("q3");
    CHECK(finish(b, "q3") == 0, "old checkout alone");
    a = launch(&q4);
    CHECK(runs("q4"), "old checkout");
    b = launch(&o4);
    CHECK(waits("o4"), "old checkout alone");
    CHECK(finish(a, "q4") == 0, "old checkout alone");
    starts("o4");
    CHECK(finish(b, "o4") == 0, "old checkout alone");
}

/** Run the rows in the fixture's directory, which holds every lock file. */
int main(void)
{
    (void)fix_dir();
    atexit(cleanup_test);
    signal(SIGTERM, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGHUP, interrupted);
    blocks();
    variables();
    alone();
    order();
    together();
    stops_wait();
    refusals();
    old_checkout();
    if (!fails) {
        fix_passed();
        printf("etude-lock-ports-test: pass\n");
    }
    return fails != 0;
}
