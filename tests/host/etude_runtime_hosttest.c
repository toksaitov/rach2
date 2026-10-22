/**
 * Check etude's runtime on Linux with its parts together: workers of several
 * accounts that run plans through the repetition driver
 * (tools/etude/posix/repeat.c) on one ledger, under watchdogs, into history
 * streams and one store; and a controller that runs a case on the responder
 * of remote execution through a relay, beside a host case under one
 * supervisor. This program is the cases, the workers, the controller, the
 * responder, the relay, and a service that keeps the output a case gives it.
 *
 * host-deps: tools/etude/repeat.c tools/etude/posix/repeat.c
 * host-deps: tools/etude/posix/load.c tools/etude/ledger.c
 * host-deps: tools/etude/watchdog.c tools/etude/supervise.c
 * host-deps: tools/etude/capture.c tools/etude/qemu_profile.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/qemu.c
 * host-deps: tools/etude/processor.c tools/etude/posix/serve.c
 * host-deps: tools/etude/posix/remote.c tools/etude/guest/wire.c
 * host-deps: tools/etude/json.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -Wall -Wextra -Werror
 * host-first: it runs for minutes
 *
 * Three accounts compete for a machine of four mebibytes: four cases hold
 * memory at once while the other requests wait, no case ever finds more held
 * than the machine has, and every case of every account then passes, with
 * the ledger empty and each history whole. A worker killed while its cases
 * run leaves them to their keepers: a case that ignores SIGTERM is ended with
 * its descendants and its claim freed, and a case whose output a service
 * outside its tree keeps leaves its claim uncertain, the keeper's verdict
 * unconfirmed and no attempt in the history. While that claim stays, a job
 * of the whole machine is refused and a smaller one passes; once a person has
 * ended the service and dropped the claim, the large job passes. A disk that
 * fills, as a limit on the size of a file makes it, ends a run whose store or
 * history it cuts: the case still running is ended, the claims are released,
 * no partial blob is left, and the history keeps no attempt or run that was
 * not written whole; the next run of the same case, with room, keeps its
 * evidence. A protected request of the whole machine runs before the next
 * small job of another account, and one whose worker died gives way to the
 * next. A case blocked forever runs out of its gap, and so does its retry,
 * beside a case that passes. A remote case whose transport is lost and cannot
 * be made again ends lost, and its claim stays uncertain after its controller
 * has gone, though the responder ended the case; one whose transport is made
 * again ends as the responder recorded it, and its claim is released.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../tools/etude/guest/wire.h"
#include "../../utils/sha256.h"
#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "budget.h"
#include "compare.h"
#include "etude/remote.h"
#include "etude/repeat.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("etude-runtime-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

#define MS     UINT64_C(1000)
#define SECOND UINT64_C(1000000)
#define MIB    UINT64_C(1048576)
#define SCHEMA "tools/etude/schemas/history.schema.json"

/* The bound of each wait of the test for a witness or a worker's end. */
#define WITNESS_US (30 * SECOND)

/* The boot the responders run in. */
#define BOOT "0123456789abcdef"

/* The lines of the output a noisy case writes, 43 KiB in all. */
#define NOISY_LINES 1024

/* The bytes a run may write to one file: those its store needs for the noisy
 * case's evidence, which is larger, and those of a stream of 40 attempts. */
#define STORE_ROOM  16384
#define STREAM_ROOM 32768

/* This program, by an absolute path. */
static char self[PATH_MAX];

/** Give the monotonic clock in microseconds. */
static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * SECOND + (uint64_t)ts.tv_nsec / 1000;
}

/** Write the SHA-256 of `n` bytes as hexadecimal into `hex`. */
static void sha_hex(const void *p, size_t n, char hex[65])
{
    struct sha256 h;
    unsigned char d[32];

    sha256_init(&h);
    sha256_update(&h, p, n);
    sha256_final(&h, d);
    sha256_hex(d, hex);
}

/* ---- The cases' program. ---- */

/** Give the path of `name` in the directory the cases share. */
static const char *shared(const char *name)
{
    static char path[PATH_MAX];
    const char *dir = getenv("ETUDE_TEST_SHARED");

    snprintf(path, sizeof(path), "%s/%s", dir != NULL ? dir : ".", name);
    return path;
}

/** Append one line to the shared log with one write(2). */
static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int fd, n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line) - 1)
        _exit(120);
    line[n++] = '\n';
    fd = open(shared("log"), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0 || write(fd, line, (size_t)n) != n)
        _exit(121);
    close(fd);
}

/** Take a lock of `type` on the whole file `path`, waiting for it; the
 * descriptor, which keeps the lock until it is closed, or -1. */
static int lock_file(const char *path, short type, int wait)
{
    struct flock fl;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0)
        return -1;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    while (fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl) != 0) {
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

/** Read the number in file `path`; 0 when it is missing. */
static long number_in(const char *path)
{
    FILE *f = fopen(path, "r");
    long n = 0;

    if (f == NULL)
        return 0;
    if (fscanf(f, "%ld", &n) != 1)
        n = 0;
    fclose(f);
    return n;
}

/** Write `n` as file `path`; 0, or -1. */
static int number_out(const char *path, long n)
{
    FILE *f = fopen(path, "w");
    int bad;

    if (f == NULL)
        return -1;
    bad = fprintf(f, "%ld\n", n) < 0;
    return fclose(f) != 0 || bad ? -1 : 0;
}

/**
 * Add `delta` to the units the running cases hold, an account the cases
 * keep themselves under a lock of their own, apart from the ledger, and
 * raise its peak; give the new count, or -1.
 */
static long occupy(long delta)
{
    int fd = lock_file(shared("held.lock"), F_WRLCK, 1);
    long n;

    if (fd < 0)
        return -1;
    n = number_in(shared("held")) + delta;
    if (number_out(shared("held"), n) != 0 ||
        (n > number_in(shared("peak")) && number_out(shared("peak"), n) != 0))
        n = -1;
    close(fd);
    return n;
}

/** Wait for signals forever. */
static void idle(void)
{
    for (;;)
        pause();
}

/** Start a child in a session of its own that waits forever; its PID. */
static pid_t escape(void)
{
    int sync[2];
    pid_t pid;
    char c;

    if (pipe(sync) != 0 || (pid = fork()) < 0)
        _exit(122);
    if (pid == 0) {
        setsid();
        if (write(sync[1], "s", 1) != 1)
            _exit(123);
        idle();
    }
    close(sync[1]);
    if (read(sync[0], &c, 1) != 1)
        _exit(124);
    close(sync[0]);
    return pid;
}

/** Write `text` to standard output with one write(2). */
static void say(const char *text)
{
    if (write(1, text, strlen(text)) < 0)
        _exit(125);
}

/**
 * Give standard output and error to the service, as a case might hand them
 * to a daemon that a service manager starts, outside the case's tree.
 */
static int hand_over(void)
{
    struct sockaddr_un sa;
    struct msghdr m;
    struct iovec iov;
    struct cmsghdr *cm;
    union {
        char buf[CMSG_SPACE(2 * sizeof(int))];
        struct cmsghdr align;
    } u;
    int fd, fds[2] = {1, 2};
    char c = 'f';

    if (chdir(shared(".")) != 0 || (fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "service.sock");
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        return -1;
    memset(&m, 0, sizeof(m));
    memset(&u, 0, sizeof(u));
    iov.iov_base = &c;
    iov.iov_len = 1;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof(u.buf);
    cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(cm), fds, sizeof(fds));
    if (sendmsg(fd, &m, 0) != 1 || read(fd, &c, 1) != 1)
        return -1;
    close(fd);
    return 0;
}

/**
 * Be case `mode` with its arguments `arg`; give its exit status.
 *
 * hold GATE TAG UNITS CAP takes UNITS of the held account, fails past CAP,
 * and waits for GATE; quick TAG ends at once; stuck GATE TAG waits for a
 * gate never opened; deaf TAG ignores SIGTERM beside a reported descendant
 * and, on Linux, one it does not report; leak TAG gives its output to the
 * service; noisy TAG writes 32 KiB and fails; hang waits to be ended.
 */
static int case_main(const char *mode, char **arg, int n)
{
    long held, cap;
    int i;

    if (strcmp(mode, "hold") == 0 && n == 4) {
        cap = atol(arg[3]);
        note("start %s %ld", arg[1], (long)getpid());
        held = occupy(atol(arg[2]));
        if (held < 0 || held > cap)
            note("over %s %ld", arg[1], held);
        if (lock_file(shared(arg[0]), F_RDLCK, 1) < 0 || occupy(-atol(arg[2])) < 0)
            return 3;
        return held >= 0 && held <= cap ? 0 : 1;
    }
    if (strcmp(mode, "quick") == 0 && n == 1) {
        note("start %s %ld", arg[0], (long)getpid());
        return 0;
    }
    if (strcmp(mode, "stuck") == 0 && n == 2) {
        note("start %s %ld", arg[1], (long)getpid());
        lock_file(shared(arg[0]), F_RDLCK, 1);
        return 3;
    }
    if (strcmp(mode, "deaf") == 0 && n == 1) {
        char line[64];
        pid_t reported, unreported = 0;

        signal(SIGTERM, SIG_IGN);
        reported = escape();
        snprintf(line, sizeof(line), "@etude 1 child %ld\n", (long)reported);
        say(line);
#if defined(__linux__)
        unreported = escape();
#endif
        note("deaf %s %ld %ld %ld", arg[0], (long)getpid(), (long)reported, (long)unreported);
        idle();
    }
    if (strcmp(mode, "leak") == 0 && n == 1) {
        if (hand_over() != 0)
            return 3;
        note("leak %s %ld", arg[0], (long)getpid());
        idle();
    }
    if (strcmp(mode, "noisy") == 0 && n == 1) {
        note("start %s %ld", arg[0], (long)getpid());
        for (i = 0; i < NOISY_LINES; i++)
            printf("line %04d of the evidence a full disk cuts\n", i);
        return 1;
    }
    if (strcmp(mode, "hang") == 0)
        idle();
    return 3;
}

/* ---- The service, outside every worker's tree. ---- */

/**
 * Be the service: accept connections on `service.sock` in the shared
 * directory and keep each descriptor a client sends, until killed. It says
 * it is ready by making `service.ready`.
 */
static void service_main(void)
{
    struct sockaddr_un sa;
    int fd, c;

    if (chdir(shared(".")) != 0 || (fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        _exit(1);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "service.sock");
    unlink(sa.sun_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 8) != 0 ||
        (c = open("service.ready", O_WRONLY | O_CREAT, 0644)) < 0)
        _exit(1);
    close(c);
    for (;;) {
        union {
            char buf[CMSG_SPACE(2 * sizeof(int))];
            struct cmsghdr align;
        } u;
        struct msghdr m;
        struct iovec iov;
        char b;

        c = accept(fd, NULL, NULL);
        if (c < 0)
            continue;
        memset(&m, 0, sizeof(m));
        iov.iov_base = &b;
        iov.iov_len = 1;
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = u.buf;
        m.msg_controllen = sizeof(u.buf);
        if (recvmsg(c, &m, 0) == 1 && write(c, "k", 1) == 1) {
            /* The descriptors received stay open: they are what it keeps. */
        }
        close(c);
    }
}

/* ---- The relay of remote execution. ---- */

/** Write all of `n` bytes; 0, or -1. */
static int write_all(int fd, const char *p, size_t n)
{
    ssize_t w;

    while (n > 0) {
        w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/**
 * Pass the whole lines of the responder's output in `line`, of `*len` bytes,
 * to the controller, keeping a partial one; 1 when the connection is to be
 * cut, after a `started` on the first connection or a failed write.
 */
static int pass_lines(int first, char *line, size_t *len)
{
    static char copy[1 << 17];
    struct wire_frame f;
    char *nl;
    size_t k;
    int cut = 0;

    while (!cut && (nl = memchr(line, '\n', *len)) != NULL) {
        k = (size_t)(nl - line) + 1;
        if (write_all(1, line, k) != 0)
            cut = 1;
        memcpy(copy, line, k - 1);
        if (first && wire_parse(copy, k - 1, &f) == 0 && strcmp(f.type, ETUDE_EV_STARTED) == 0)
            cut = 1;
        memmove(line, line + k, *len - k);
        *len -= k;
    }
    return cut;
}

/**
 * Be the relay between the controller, on standard input and output, and
 * the responder that `argv` starts. On the first connection, which `marker`
 * not yet existing shows, it passes the responder's `started` and then ends
 * the connection, after the responder has ended. In mode `down` a later
 * connection ends before the responder starts, as an SSH connection to a
 * machine no longer reachable does; in mode `once` it passes everything.
 */
static int relay_main(const char *mode, const char *marker, char **argv)
{
    static char line[1 << 17], copy[1 << 17];
    struct pollfd pfd[2];
    int to[2], from[2], status, fd, first, cut = 0, open_c = 1;
    size_t len = 0;
    ssize_t n;
    pid_t pid;

    signal(SIGPIPE, SIG_IGN);
    first = access(marker, F_OK) != 0;
    if ((fd = open(marker, O_WRONLY | O_CREAT, 0600)) >= 0)
        close(fd);
    if (!first && strcmp(mode, "down") == 0)
        return 1;
    if (pipe(to) != 0 || pipe(from) != 0 || (pid = fork()) < 0)
        return 1;
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        close(to[0]);
        close(to[1]);
        close(from[0]);
        close(from[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    while (!cut) {
        pfd[0].fd = open_c ? 0 : -1;
        pfd[0].events = POLLIN;
        pfd[1].fd = from[0];
        pfd[1].events = POLLIN;
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (open_c && (pfd[0].revents & (POLLIN | POLLHUP)) != 0) {
            n = read(0, copy, sizeof(copy));
            if (n <= 0) {
                open_c = 0;
                close(to[1]);
            } else if (write_all(to[1], copy, (size_t)n) != 0) {
                break;
            }
        }
        if ((pfd[1].revents & (POLLIN | POLLHUP)) == 0)
            continue;
        n = read(from[0], line + len, sizeof(line) - len);
        if (n <= 0)
            break;
        len += (size_t)n;
        cut = pass_lines(first, line, &len);
    }
    close(1);
    if (open_c)
        close(to[1]);
    close(from[0]);
    waitpid(pid, &status, 0);
    return 0;
}

/* ---- Fixtures the rows share. ---- */

/* The blob reference every plan and case shares, and the plan's members. */
static char ref[200], plan_fixed[4096];

/* The limits of the plans' class, which their budgets record. */
static const struct etude_budget_limits FOCUSED = {3600 * SECOND, 3600 * SECOND, 14400 * SECOND,
                                                   600 * SECOND};

/* The environment of the cases, which names the directory they share. */
static char **case_env;

/** Write into `out` a blob reference to the bytes `bytes`. */
static void blob(char *out, size_t size, const char *bytes)
{
    struct sha256 sha;
    unsigned char digest[32];
    char hex[65];

    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, strlen(bytes));
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    snprintf(out, size, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"test://blob\"]}",
             hex, (unsigned long)strlen(bytes));
}

/** Make the fixtures the rows share; 0, or -1. */
static int fixtures(void)
{
    blob(ref, sizeof(ref), "fixed");
    snprintf(plan_fixed, sizeof(plan_fixed),
             "\"purpose\":\"focused\",\"commit\":\"1111111111111111111111111111111111111111\","
             "\"source\":%s,\"runner\":%s,\"artifacts\":[],\"environment\":{\"machine_id\":\"m1\","
             "\"boot_id\":\"b1\",\"os\":\"linux\",\"os_version\":\"26.04\",\"accelerator\":"
             "\"native\",\"site_profile\":%s,\"tools\":%s,\"capacity\":%s,\"cache_state\":%s,"
             "\"load\":%s},\"policies\":%s,\"exemption\":null,\"reproduce\":%s",
             ref, ref, ref, ref, ref, ref, ref, ref, ref);
    if (setenv("ETUDE_TEST_SHARED", fix_path("shared"), 1) != 0)
        return -1;
    case_env = environ;
    return mkdir(fix_path("shared"), 0700) == 0 && mkdir(fix_path("store"), 0700) == 0 &&
                   mkdir(fix_path("scratch"), 0700) == 0
               ? 0
               : -1;
}

/** Empty the cases' log and their account of what they hold. */
static void fresh_shared(void)
{
    static const char *const names[] = {"log", "held", "peak"};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unlink(shared(names[i]));
}

/** Close gate `name`, which cases wait for; the descriptor that holds it. */
static int gate_close(const char *name)
{
    return lock_file(shared(name), F_WRLCK, 0);
}

/** Open the gate that `fd` holds. */
static void gate_open(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* The load program, which no plan here runs, since no item has a load. */
static char *no_load[] = {self, "--case", "quick", "load", NULL};

/* The argument vectors and identities of the cases, in slots used in turn;
 * a row makes fewer cases than there are. */
#define SLOTS 16
static char *argv_of[SLOTS][9];
static char ids[SLOTS][64];
static int next_slot;

/**
 * Make case `name` of `reps` repetitions under bounds `wd`, reserving the
 * resources `res`, processors and memory, whose program is this one in mode
 * `mode` with the arguments of `args`, ending with NULL.
 */
static struct etude_repeat_case make_case(const char *name, unsigned reps,
                                          const struct etude_watchdog_spec *wd,
                                          const struct etude_ledger_res *res, const char *mode,
                                          const char *const *args)
{
    struct etude_repeat_case c;
    int i, slot = next_slot;

    next_slot = (next_slot + 1) % SLOTS;
    memset(&c, 0, sizeof(c));
    snprintf(ids[slot], sizeof(ids[slot]), "test.runtime.%s", name);
    argv_of[slot][0] = self;
    argv_of[slot][1] = "--case";
    argv_of[slot][2] = (char *)mode;
    for (i = 0; args[i] != NULL && i < 5; i++)
        argv_of[slot][3 + i] = (char *)args[i];
    argv_of[slot][3 + i] = NULL;
    c.case_id = ids[slot];
    c.arch = "none";
    c.repetitions = reps;
    c.family_id = "test.runtime";
    c.argv = argv_of[slot];
    c.envp = case_env;
    c.output_max = 65536;
    c.grace_us = 300 * MS;
    c.wd = wd;
    c.res = res;
    c.nres = 2;
    c.workload = c.assertions = c.profile = ref;
    c.inventory_sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    return c;
}

/** Give the claim identity of attempt `try` of repetition `rep` of case `c`
 * in run `run`, into `out`. */
static void claim_of(const char *run, const struct etude_repeat_case *c, unsigned rep, unsigned try,
                     char *out, size_t size)
{
    char item[256];

    if (etude_repeat_item_id(run, c, rep, item, sizeof(item)) != 0)
        item[0] = '\0';
    snprintf(out, size, "%s:a%u", item, try);
}

/* ---- The workers. ---- */

/* A worker: a process that runs one plan through the repetition driver. */
struct worker {
    const char *name;    /* its run, and its files' names */
    const char *account; /* the account its claims are admitted for */
    const char *ledger;  /* the ledger's directory, a name of the fixtures */
    const struct etude_ledger_res *cap;
    size_t ncap;
    const struct etude_repeat_case *cases;
    size_t ncases;
    unsigned width, retries;
    uint64_t seed;
    rlim_t fsize; /* the largest file it may write; 0 for no limit */
    pid_t pid;
};

/* What a worker reported of its run. */
struct summary {
    int rc, err, outcome;
    size_t n;
    struct {
        char id[272];
        int outcome, cleanup;
        char reason[200];
    } a[48];
};

/** Run the plan of `w` and write its summary; the body of the worker. */
static void worker_main(const struct worker *w)
{
    struct etude_repeat_spec spec;
    struct etude_repeat_site site;
    struct etude_repeat_result r;
    struct etude_ledger *l;
    struct etude_history *h;
    struct rlimit lim;
    char path[64], stream[80], budget[200];
    FILE *out;
    size_t i;
    int rc, e;

    if (w->fsize != 0) {
        signal(SIGXFSZ, SIG_IGN);
        getrlimit(RLIMIT_FSIZE, &lim);
        lim.rlim_cur = w->fsize;
        if (setrlimit(RLIMIT_FSIZE, &lim) != 0)
            _exit(2);
    }
    snprintf(path, sizeof(path), "%s.jsonl", w->name);
    snprintf(stream, sizeof(stream), "%s-s", w->name);
    l = etude_ledger_open(fix_path(w->ledger));
    h = etude_history_open(fix_path(path), SCHEMA, stream, w->name, UINT64_C(1) << 24);
    if (l == NULL || h == NULL)
        _exit(2);
    memset(&spec, 0, sizeof(spec));
    spec.seed = w->seed;
    spec.width = w->width;
    spec.retries = w->retries;
    spec.cases = w->cases;
    spec.ncases = w->ncases;
    blob(budget, sizeof(budget), "budget");
    memset(&site, 0, sizeof(site));
    site.run_id = w->name;
    site.ledger = l;
    site.account = w->account;
    site.cap = w->cap;
    site.ncap = w->ncap;
    site.load_argv = no_load;
    site.load_envp = case_env;
    site.scratch = fix_path("scratch");
    site.load_ready_us = 10 * SECOND;
    site.load_grace_us = SECOND;
    site.history = h;
    site.store = fix_path("store");
    site.plan_fixed = plan_fixed;
    site.machine_id = "m1";
    site.boot_id = "b1";
    site.budget_policy = budget;
    site.budget_limits = &FOCUSED;
    memset(&r, 0, sizeof(r));
    rc = etude_repeat_run(&spec, &site, &r);
    e = errno;
    etude_history_close(h);
    etude_ledger_close(l);
    if (w->fsize != 0) {
        lim.rlim_cur = lim.rlim_max;
        setrlimit(RLIMIT_FSIZE, &lim);
    }
    snprintf(path, sizeof(path), "%s.summary", w->name);
    out = fopen(fix_path(path), "w");
    if (out == NULL)
        _exit(2);
    fprintf(out, "%d %d %d\n", rc, rc == 0 ? 0 : e, rc == 0 ? (int)r.outcome : -1);
    for (i = 0; rc == 0 && i < r.nattempts; i++)
        fprintf(out, "%s %d %d %s\n", r.attempts[i].attempt_id, (int)r.attempts[i].outcome,
                (int)r.attempts[i].cleanup, r.attempts[i].reason);
    _exit(fclose(out) == 0 ? 0 : 2);
}

/** Start worker `w`; 0, or -1. */
static int start(struct worker *w)
{
    fflush(stdout);
    w->pid = fork();
    if (w->pid < 0)
        return -1;
    if (w->pid == 0)
        worker_main(w);
    return 0;
}

/**
 * Wait for process `pid`, for at most WITNESS_US, killing it then; its wait
 * status, or -1 when it had to be killed.
 */
static int reap_within(pid_t pid)
{
    uint64_t until = now_us() + WITNESS_US;
    int status;
    pid_t got;

    while ((got = waitpid(pid, &status, WNOHANG)) == 0 && now_us() < until)
        poll(NULL, 0, 10);
    if (got == pid)
        return status;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

/** Wait for worker `w` and read its summary into `s`; 0, or -1. */
static int finish(struct worker *w, struct summary *s)
{
    char path[64], line[600];
    FILE *f;
    int status = reap_within(w->pid);

    memset(s, 0, sizeof(*s));
    s->rc = -2;
    if (status == -1)
        printf("etude-runtime-test: worker %s did not end and was killed\n", w->name);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s.summary", w->name);
    if ((f = fopen(fix_path(path), "r")) == NULL)
        return -1;
    if (fgets(line, sizeof(line), f) == NULL ||
        sscanf(line, "%d %d %d", &s->rc, &s->err, &s->outcome) != 3) {
        fclose(f);
        return -1;
    }
    while (s->n < sizeof(s->a) / sizeof(s->a[0]) && fgets(line, sizeof(line), f) != NULL) {
        int used = 0;

        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%271s %d %d %n", s->a[s->n].id, &s->a[s->n].outcome, &s->a[s->n].cleanup,
                   &used) < 3)
            break;
        snprintf(s->a[s->n].reason, sizeof(s->a[s->n].reason), "%s", line + used);
        s->n++;
    }
    fclose(f);
    return 0;
}

/** Say whether every attempt of `s` passed with its cleanup confirmed. */
static int all_passed(const struct summary *s, size_t n)
{
    size_t i;

    if (s->rc != 0 || s->outcome != ETUDE_REPEAT_PASS || s->n != n)
        return 0;
    for (i = 0; i < s->n; i++)
        if (s->a[i].outcome != ETUDE_REPEAT_PASS || s->a[i].cleanup != ETUDE_CLEANUP_CONFIRMED)
            return 0;
    return 1;
}

/** Give the index of the attempt of `s` whose identity has `part` in it,
 * or -1. */
static int attempt_of(const struct summary *s, const char *part)
{
    size_t i;

    for (i = 0; i < s->n; i++)
        if (strstr(s->a[i].id, part) != NULL)
            return (int)i;
    return -1;
}

/* ---- What the rows look at. ---- */

/* What a reading of a ledger found. */
struct look {
    unsigned running, uncertain, requests, blocked;
    uint64_t memory; /* held by the claims that are not requests */
    char blocked_account[32];
    char accounts[128]; /* the requests' accounts, each followed by a space */
};

/** Note one claim of a ledger in the look `ctx`. */
static int look_visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct look *lk = ctx;
    size_t i;

    if (strcmp(c->state, "waiting") == 0 || strcmp(c->state, "blocked") == 0) {
        lk->requests++;
        if (strlen(lk->accounts) + strlen(c->account) + 2 < sizeof(lk->accounts)) {
            strcat(lk->accounts, c->account);
            strcat(lk->accounts, " ");
        }
        if (strcmp(c->state, "blocked") == 0) {
            lk->blocked++;
            snprintf(lk->blocked_account, sizeof(lk->blocked_account), "%s", c->account);
        }
        return 0;
    }
    if (strcmp(c->state, "running") == 0)
        lk->running++;
    if (c->status == ETUDE_CLAIM_UNCERTAIN)
        lk->uncertain++;
    for (i = 0; i < c->nres; i++)
        if (strcmp(c->res[i].name, "memory_bytes") == 0)
            lk->memory += c->res[i].amount;
    return 0;
}

/** Read ledger `name` into `lk`; 0, or -1. */
static int look(const char *name, struct look *lk)
{
    struct etude_ledger *l = etude_ledger_open(fix_path(name));
    int rc;

    memset(lk, 0, sizeof(*lk));
    if (l == NULL)
        return -1;
    rc = etude_ledger_list(l, look_visit, lk);
    etude_ledger_close(l);
    return rc;
}

/** Say whether ledger `name` keeps nothing. */
static int empty(const char *name)
{
    struct look lk;

    return look(name, &lk) == 0 && lk.running == 0 && lk.uncertain == 0 && lk.requests == 0 &&
           lk.memory == 0;
}

/* What a reading of a ledger must find, for await. */
struct want {
    const char *ledger;
    unsigned running;    /* running claims at least */
    const char *request; /* an account with a request, or NULL */
    const char *blocked; /* the account of the protected request, or NULL */
    const char *gone;    /* an account with no request, or NULL */
    long held;           /* the cases' own count, or -1 */
    unsigned log_lines;  /* lines of the log at least */
    const char *claim;   /* a claim that must be absent, or NULL */
};

/** Count the lines of the cases' log that begin with `prefix`. */
static unsigned logged(const char *prefix)
{
    char line[256];
    unsigned n = 0;
    FILE *f = fopen(shared("log"), "r");

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL)
        n += strncmp(line, prefix, strlen(prefix)) == 0;
    fclose(f);
    return n;
}

/* The claims a reading saw, for a claim that must be absent. */
static int claim_seen;
static const char *claim_sought;

/** Note whether the claim sought is in the ledger. */
static int seek_visit(const struct etude_ledger_claim *c, void *ctx)
{
    (void)ctx;
    claim_seen |= strcmp(c->id, claim_sought) == 0;
    return 0;
}

/** Say whether ledger `name` keeps claim `id`. */
static int kept(const char *name, const char *id)
{
    struct etude_ledger *l = etude_ledger_open(fix_path(name));

    claim_seen = 0;
    claim_sought = id;
    if (l == NULL)
        return 1;
    if (etude_ledger_list(l, seek_visit, NULL) != 0)
        claim_seen = 1;
    etude_ledger_close(l);
    return claim_seen;
}

/* The processes registered in the claim sought, for nproc_visit. */
static size_t claim_nproc;

/** Note the processes of the claim sought. */
static int nproc_visit(const struct etude_ledger_claim *c, void *ctx)
{
    (void)ctx;
    if (strcmp(c->id, claim_sought) == 0)
        claim_nproc = c->nproc;
    return 0;
}

/**
 * Wait, for at most WITNESS_US, until claim `id` of ledger `name` has `n`
 * processes registered; 1 once it has.
 */
static int await_nproc(const char *name, const char *id, size_t n)
{
    uint64_t until = now_us() + WITNESS_US;
    struct etude_ledger *l;

    do {
        claim_nproc = 0;
        claim_sought = id;
        if ((l = etude_ledger_open(fix_path(name))) != NULL) {
            etude_ledger_list(l, nproc_visit, NULL);
            etude_ledger_close(l);
        }
        if (claim_nproc >= n)
            return 1;
        poll(NULL, 0, 10);
    } while (now_us() < until);
    return 0;
}

/** Say whether the ledger and the log show what `w` wants now. */
static int seen(const struct want *w)
{
    struct look lk;
    char word[40];

    if (look(w->ledger, &lk) != 0 || lk.running < w->running)
        return 0;
    snprintf(word, sizeof(word), "%s ", w->request != NULL ? w->request : "");
    if (w->request != NULL && strstr(lk.accounts, word) == NULL)
        return 0;
    if (w->blocked != NULL && strcmp(lk.blocked_account, w->blocked) != 0)
        return 0;
    snprintf(word, sizeof(word), "%s ", w->gone != NULL ? w->gone : "");
    if (w->gone != NULL && strstr(lk.accounts, word) != NULL)
        return 0;
    if (w->held >= 0 && number_in(shared("held")) != w->held)
        return 0;
    if (logged("") < w->log_lines)
        return 0;
    return w->claim == NULL || !kept(w->ledger, w->claim);
}

/** Wait for what `w` wants, for at most WITNESS_US; 1 once it is seen. */
static int await(const struct want *w)
{
    uint64_t until = now_us() + WITNESS_US;

    while (!seen(w)) {
        if (now_us() >= until)
            return seen(w);
        poll(NULL, 0, 10);
    }
    return 1;
}

/** Say whether process `pid` runs, a zombie being ended. */
static int runs(long pid)
{
    struct etude_pid id;

    return pid > 0 && etude_process_state(pid, &id) == 1;
}

/* The records of one history stream, for count_visit. */
struct records {
    unsigned plans, attempts, runs;
};

/** Count one record of a stream by its kind. */
static int count_visit(const char *line, void *ctx)
{
    struct records *r = ctx;

    r->plans += strstr(line, "\"kind\":\"plan\"") != NULL;
    r->attempts += strstr(line, "\"kind\":\"attempt\"") != NULL;
    r->runs += strstr(line, "\"kind\":\"run\"") != NULL;
    return 0;
}

/** Read the stream of run `name` into `r`; etude_history_read's result. */
static int records(const char *name, struct records *r)
{
    char path[64];
    uint64_t bad = 0;

    memset(r, 0, sizeof(*r));
    snprintf(path, sizeof(path), "%s.jsonl", name);
    return etude_history_read(fix_path(path), SCHEMA, count_visit, r, &bad);
}

/**
 * Say whether every file of the store is a whole blob: named by the SHA-256
 * of its bytes, and nothing else in the directory.
 */
static int store_whole(void)
{
    DIR *d = opendir(fix_path("store"));
    struct dirent *e;
    int ok = d != NULL;

    while (ok && (e = readdir(d)) != NULL) {
        char path[PATH_MAX], hex[65], *bytes;
        size_t n;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", fix_path("store"), e->d_name);
        if (strlen(e->d_name) != 64 || cmp_read_file(path, 1 << 24, &bytes, &n) != 0) {
            printf("etude-runtime-test: the store has %s, which is no blob\n", e->d_name);
            ok = 0;
            continue;
        }
        sha_hex(bytes, n, hex);
        free(bytes);
        if (strcmp(hex, e->d_name) != 0) {
            printf("etude-runtime-test: the store's %s has %lu bytes of other content\n", e->d_name,
                   (unsigned long)n);
            ok = 0;
        }
    }
    if (d != NULL)
        closedir(d);
    return ok;
}

/** Say whether the store keeps the noisy case's output whole. */
static int noisy_kept(void)
{
    struct sha256 sha;
    unsigned char digest[32];
    char hex[65], line[64], path[PATH_MAX], *bytes;
    size_t n, total = 0;
    int i, ok;

    sha256_init(&sha);
    for (i = 0; i < NOISY_LINES; i++) {
        snprintf(line, sizeof(line), "line %04d of the evidence a full disk cuts\n", i);
        sha256_update(&sha, (const unsigned char *)line, strlen(line));
        total += strlen(line);
    }
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    snprintf(path, sizeof(path), "%s/%s", fix_path("store"), hex);
    if (cmp_read_file(path, 1 << 20, &bytes, &n) != 0)
        return 0;
    ok = n == total;
    free(bytes);
    return ok;
}

/* ---- The rows. ---- */

/* The bounds of an ordinary case, and of one that must run out of its gap. */
static const struct etude_watchdog_spec WD = {120 * SECOND, 60 * SECOND, {0}, NULL, 0};
static const struct etude_watchdog_spec STUCK_WD = {120 * SECOND, 300 * MS, {0}, NULL, 0};

/* A machine of two mebibytes, and claims of one and of two. */
static const struct etude_ledger_res CAP2[] = {{"cpu_millis", 8000}, {"memory_bytes", 2 * MIB}};
static const struct etude_ledger_res ONE[] = {{"cpu_millis", 100}, {"memory_bytes", MIB}};
static const struct etude_ledger_res TWO[] = {{"cpu_millis", 100}, {"memory_bytes", 2 * MIB}};

/** Fill worker `w` with what every row's workers have in common: one case
 * at a time, on a machine of two mebibytes. */
static void worker_of(struct worker *w, const char *name, const char *account, const char *ledger,
                      const struct etude_repeat_case *cases, size_t ncases)
{
    memset(w, 0, sizeof(*w));
    w->name = name;
    w->account = account;
    w->ledger = ledger;
    w->cap = CAP2;
    w->ncap = 2;
    w->cases = cases;
    w->ncases = ncases;
    w->width = 1;
    w->seed = 1;
}

/**
 * Three accounts each run three cases of one mebibyte, three at a time, on a
 * machine of four: four hold memory while the others wait, the cases' own
 * account agrees, and once their gate opens every case passes, no case ever
 * having found more held than the machine has.
 */
static void test_accounts(void)
{
    static const struct etude_ledger_res cap[] = {{"cpu_millis", 9000}, {"memory_bytes", 4 * MIB}};
    static const char *const names[3] = {"alpha", "beta", "gamma"};
    static const char *const args[3][5] = {{"accounts.gate", "alpha", "1", "4", NULL},
                                           {"accounts.gate", "beta", "1", "4", NULL},
                                           {"accounts.gate", "gamma", "1", "4", NULL}};
    struct etude_repeat_case c[3];
    struct worker w[3];
    struct summary s;
    struct records r;
    struct want want;
    int gate, i;

    fresh_shared();
    gate = gate_close("accounts.gate");
    CHECK(gate >= 0, "accounts");
    for (i = 0; i < 3; i++) {
        c[i] = make_case(names[i], 3, &WD, ONE, "hold", args[i]);
        worker_of(&w[i], names[i], names[i], "accounts", &c[i], 1);
        w[i].width = 3;
        w[i].cap = cap;
        CHECK(start(&w[i]) == 0, "accounts");
    }
    memset(&want, 0, sizeof(want));
    want.ledger = "accounts";
    want.running = 4;
    want.held = 4;
    want.log_lines = 4;
    CHECK(await(&want), "accounts");
    {
        struct look lk;

        CHECK(look("accounts", &lk) == 0 && lk.running == 4 && lk.memory == 4 * MIB &&
                  lk.requests >= 1 && lk.blocked == 1,
              "accounts");
    }
    CHECK(logged("start ") == 4 && number_in(shared("held")) == 4, "accounts");
    gate_open(gate);
    for (i = 0; i < 3; i++) {
        CHECK(finish(&w[i], &s) == 0 && all_passed(&s, 3), "accounts");
        CHECK(records(names[i], &r) == 0 && r.plans == 1 && r.attempts == 3 && r.runs == 1,
              "accounts");
    }
    CHECK(logged("start ") == 9 && logged("over ") == 0, "accounts");
    CHECK(number_in(shared("peak")) == 4 && number_in(shared("held")) == 0, "accounts");
    CHECK(empty("accounts"), "accounts");
}

/** Start the service; its PID, or -1 once it could not be made ready. */
static pid_t service_start(void)
{
    uint64_t until = now_us() + WITNESS_US;
    pid_t pid;

    unlink(shared("service.ready"));
    fflush(stdout);
    pid = fork();
    if (pid == 0)
        service_main();
    while (pid > 0 && access(shared("service.ready"), F_OK) != 0 && now_us() < until)
        poll(NULL, 0, 10);
    return pid > 0 && access(shared("service.ready"), F_OK) == 0 ? pid : -1;
}

/** Read the PIDs of the log's line that begins `prefix` into `pid`. */
static int pids_of(const char *prefix, long pid[3])
{
    char line[256];
    int n = 0;
    FILE *f = fopen(shared("log"), "r");

    pid[0] = pid[1] = pid[2] = 0;
    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            n = sscanf(line + strlen(prefix), "%ld %ld %ld", &pid[0], &pid[1], &pid[2]);
            break;
        }
    fclose(f);
    return n;
}

/* The reason sought of the claim sought, and whether it was found. */
static const char *why_sought;
static int why_found;

/** Note whether the claim sought is uncertain for the reason sought. */
static int why_visit(const struct etude_ledger_claim *c, void *ctx)
{
    (void)ctx;
    if (strcmp(c->id, claim_sought) == 0)
        why_found = c->status == ETUDE_CLAIM_UNCERTAIN && strstr(c->why, why_sought) != NULL;
    return 0;
}

/** Say whether claim `id` of ledger `name` is uncertain for reason `why`. */
static int uncertain(const char *name, const char *id, const char *why)
{
    struct etude_ledger *l = etude_ledger_open(fix_path(name));

    why_found = 0;
    claim_sought = id;
    why_sought = why;
    if (l == NULL)
        return 0;
    if (etude_ledger_list(l, why_visit, NULL) != 0)
        why_found = 0;
    etude_ledger_close(l);
    return why_found;
}

/** Read the verdict file of claim `id` of ledger `name`; 0, or -1. */
static int verdict_of(const char *name, const char *id, char *buf, size_t size)
{
    struct etude_ledger *l = etude_ledger_open(fix_path(name));
    char path[PATH_MAX];
    FILE *f;
    size_t n;

    buf[0] = '\0';
    if (l == NULL || etude_ledger_verdict_path(l, id, path, sizeof(path)) != 0) {
        if (l != NULL)
            etude_ledger_close(l);
        return -1;
    }
    etude_ledger_close(l);
    if ((f = fopen(path, "r")) == NULL)
        return -1;
    n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

/**
 * Kill a worker while two of its cases run: one that ignores SIGTERM beside
 * its descendants, and one that gave its output to a service. The first's
 * keeper ends its tree and its claim is freed; the second's claim stays
 * uncertain, its keeper's verdict unconfirmed, with no attempt or run in the
 * history. While it stays, another account's job of the whole machine is
 * refused and not retried, and a job of half passes; after a person has ended
 * the service, the claim still stays until dropped, and then the large job
 * passes.
 */
static void test_lost_worker(void)
{
    static const char *const deaf_args[] = {"deaf", NULL};
    static const char *const leak_args[] = {"leak", NULL};
    static const char *const big_args[] = {"big", NULL};
    static const char *const small_args[] = {"small", NULL};
    struct etude_repeat_case lost[2], second[2], third[1];
    struct worker a, b, c;
    struct summary s;
    struct records r;
    struct want want;
    char deaf_id[300], leak_id[300], buf[64];
    long deaf[3], leak[3];
    pid_t service;
    int i, big, small;

    fresh_shared();
    service = service_start();
    CHECK(service > 0, "lost-worker");
    lost[0] = make_case("deaf", 1, &WD, ONE, "deaf", deaf_args);
    lost[1] = make_case("leak", 1, &WD, ONE, "leak", leak_args);
    worker_of(&a, "lost-a", "alpha", "lost", lost, 2);
    a.width = 2;
    claim_of("lost-a", &lost[0], 0, 0, deaf_id, sizeof(deaf_id));
    claim_of("lost-a", &lost[1], 0, 0, leak_id, sizeof(leak_id));
    CHECK(start(&a) == 0, "lost-worker");
    memset(&want, 0, sizeof(want));
    want.ledger = "lost";
    want.running = 2;
    want.held = -1;
    want.log_lines = 2;
    CHECK(await(&want), "lost-worker");
    CHECK(await_nproc("lost", deaf_id, 3), "lost-worker");
    CHECK(pids_of("deaf deaf ", deaf) == 3 && pids_of("leak leak ", leak) == 1, "lost-worker");
    kill(a.pid, SIGKILL);
    CHECK(reap_within(a.pid) != -1, "lost-worker");

    memset(&want, 0, sizeof(want));
    want.ledger = "lost";
    want.held = -1;
    want.claim = deaf_id;
    CHECK(await(&want), "lost-worker");
    for (i = 0; i < 3; i++)
        CHECK(!runs(deaf[i]), "lost-worker");
    {
        uint64_t until = now_us() + WITNESS_US;

        while ((verdict_of("lost", leak_id, buf, sizeof(buf)) != 0 || buf[0] == '\0') &&
               now_us() < until)
            poll(NULL, 0, 10);
    }
    CHECK(strcmp(buf, ETUDE_VERDICT_UNCONFIRMED) == 0, "lost-worker");
    CHECK(!runs(leak[0]), "lost-worker");
    CHECK(uncertain("lost", leak_id, "keeper"), "lost-worker");
    CHECK(records("lost-a", &r) == 0 && r.plans == 1 && r.attempts == 0 && r.runs == 0,
          "lost-worker");

    second[0] = make_case("big", 1, &WD, TWO, "quick", big_args);
    second[1] = make_case("small", 1, &WD, ONE, "quick", small_args);
    worker_of(&b, "lost-b", "beta", "lost", second, 2);
    b.retries = 1;
    CHECK(start(&b) == 0 && finish(&b, &s) == 0, "lost-worker");
    big = attempt_of(&s, "big");
    small = attempt_of(&s, "small");
    CHECK(s.rc == 0 && s.outcome == ETUDE_REPEAT_INCOMPLETE && s.n == 2, "lost-worker");
    CHECK(big >= 0 && s.a[big].outcome == ETUDE_REPEAT_INCOMPLETE &&
              strstr(s.a[big].reason, "only a person can release") != NULL,
          "lost-worker");
    CHECK(small >= 0 && s.a[small].outcome == ETUDE_REPEAT_PASS, "lost-worker");
    CHECK(logged("start big") == 0 && logged("start small") == 1, "lost-worker");

    if (service > 0) {
        kill(service, SIGKILL);
        waitpid(service, NULL, 0);
    }
    CHECK(uncertain("lost", leak_id, "keeper"), "lost-worker");
    {
        struct etude_ledger *l = etude_ledger_open(fix_path("lost"));

        CHECK(l != NULL && etude_ledger_drop(l, leak_id) == 0, "lost-worker");
        if (l != NULL)
            etude_ledger_close(l);
    }
    third[0] = make_case("big", 1, &WD, TWO, "quick", big_args);
    worker_of(&c, "lost-c", "gamma", "lost", third, 1);
    CHECK(start(&c) == 0 && finish(&c, &s) == 0 && all_passed(&s, 1), "lost-worker");
    CHECK(empty("lost"), "lost-worker");
}

/**
 * Fill the disk under a run, with a limit on the size of a file. A case's
 * evidence too large for the room left ends the run with the store's
 * failure: the claim is released, no partial blob is left, and the history
 * has no attempt and no run. The same case run again with room keeps its
 * evidence whole.
 */
static void test_full_store(void)
{
    static const char *const noisy_args[] = {"noisy", NULL};
    struct etude_repeat_case noisy[1], again[1];
    struct worker w;
    struct summary s;
    struct records r;

    fresh_shared();
    noisy[0] = make_case("noisy", 1, &WD, ONE, "noisy", noisy_args);
    worker_of(&w, "disk-store", "alpha", "disk", noisy, 1);
    w.fsize = STORE_ROOM;
    CHECK(start(&w) == 0 && finish(&w, &s) == 0, "full-disk");
    CHECK(s.rc == -1 && s.err != 0 && logged("start noisy") == 1, "full-disk");
    CHECK(empty("disk"), "full-disk");
    CHECK(store_whole(), "full-disk");
    CHECK(records("disk-store", &r) == 0 && r.plans == 1 && r.attempts == 0 && r.runs == 0,
          "full-disk");

    again[0] = make_case("noisy", 1, &WD, ONE, "noisy", noisy_args);
    worker_of(&w, "disk-again", "alpha", "disk", again, 1);
    CHECK(start(&w) == 0 && finish(&w, &s) == 0, "full-disk");
    CHECK(s.rc == 0 && s.outcome == ETUDE_REPEAT_FAILURE && s.n == 1, "full-disk");
    CHECK(noisy_kept() && store_whole(), "full-disk");
    CHECK(records("disk-again", &r) == 0 && r.attempts == 1 && r.runs == 1, "full-disk");
}

/**
 * Fill a run's history stream while a case waits on its gate: the run ends,
 * the waiting case is ended, its claim released, and the stream keeps no
 * partial record and no run.
 */
static void test_full_stream(void)
{
    static const char *const quick_args[] = {"quick", NULL};
    static const char *const hold_args[] = {"disk.gate", "held", "1", "2", NULL};
    struct etude_repeat_case stream[2];
    struct etude_repeat_spec spec;
    struct etude_repeat_item *it;
    struct worker w;
    struct summary s;
    struct records r;
    struct stat st;
    long held[3];
    size_t n;
    int gate, read;

    /* A seed whose plan runs the waiting case first, so it runs throughout. */
    stream[0] = make_case("held", 1, &WD, ONE, "hold", hold_args);
    stream[1] = make_case("quick", 40, &WD, ONE, "quick", quick_args);
    memset(&spec, 0, sizeof(spec));
    spec.width = 2;
    spec.cases = stream;
    spec.ncases = 2;
    for (spec.seed = 1; spec.seed < 1000; spec.seed++) {
        if (etude_repeat_plan(&spec, &it, &n) != 0)
            break;
        read = it[0].case_index == 0;
        free(it);
        if (read)
            break;
    }
    fresh_shared();
    gate = gate_close("disk.gate");
    worker_of(&w, "disk-stream", "alpha", "disk", stream, 2);
    w.width = 2;
    w.seed = spec.seed;
    w.fsize = STREAM_ROOM;
    CHECK(gate >= 0 && start(&w) == 0 && finish(&w, &s) == 0, "full-disk");
    CHECK(s.rc == -1 && s.err != 0, "full-disk");
    CHECK(pids_of("start held ", held) == 1 && !runs(held[0]), "full-disk");
    CHECK(logged("start quick") > 0 && logged("start quick") < 40, "full-disk");
    CHECK(empty("disk"), "full-disk");
    read = records("disk-stream", &r);
    CHECK((read == 0 || read == 1) && r.plans == 1 && r.attempts > 0 && r.runs == 0, "full-disk");
    CHECK(stat(fix_path("disk-stream.jsonl"), &st) == 0 && st.st_size == STREAM_ROOM, "full-disk");
    gate_open(gate);
}

/**
 * Protect a request of the whole machine: while one account's cases hold it,
 * another account's job of all its memory waits, and once the gate opens it
 * runs after the first account's request already protected and before that
 * account's next. A protected request whose worker is killed is removed, and
 * the next account's request is protected in its place.
 */
static void test_blocked(void)
{
    static const char *const a_args[] = {"blocked.gate", "a", "1", "2", NULL};
    static const char *const b_args[] = {"blocked.gate", "b", "2", "2", NULL};
    static const char *const d_args[] = {"dead.gate", "d", "1", "2", NULL};
    static const char *const e_args[] = {"dead.gate", "e", "2", "2", NULL};
    static const char *const f_args[] = {"f", NULL};
    struct etude_repeat_case ca[1], cb[1], cd[1], ce[1], cf[1];
    struct worker a, b, d, e, f;
    struct summary s;
    struct want want;
    char line[64];
    unsigned before = 0, seen_b = 0;
    FILE *log;
    int gate;

    fresh_shared();
    gate = gate_close("blocked.gate");
    ca[0] = make_case("a", 4, &WD, ONE, "hold", a_args);
    cb[0] = make_case("b", 1, &WD, TWO, "hold", b_args);
    worker_of(&a, "blocked-a", "alpha", "blocked", ca, 1);
    a.width = 3;
    worker_of(&b, "blocked-b", "beta", "blocked", cb, 1);
    CHECK(gate >= 0 && start(&a) == 0, "blocked");
    memset(&want, 0, sizeof(want));
    want.ledger = "blocked";
    want.running = 2;
    want.blocked = "alpha";
    want.held = 2;
    CHECK(await(&want), "blocked");
    CHECK(start(&b) == 0, "blocked");
    want.request = "beta";
    CHECK(await(&want), "blocked");
    gate_open(gate);
    CHECK(finish(&a, &s) == 0 && all_passed(&s, 4), "blocked");
    CHECK(finish(&b, &s) == 0 && all_passed(&s, 1), "blocked");
    if ((log = fopen(shared("log"), "r")) != NULL) {
        while (fgets(line, sizeof(line), log) != NULL) {
            if (strncmp(line, "start b ", 8) == 0)
                seen_b = 1;
            if (strncmp(line, "start a ", 8) == 0 && !seen_b)
                before++;
        }
        fclose(log);
    }
    CHECK(seen_b && before == 3, "blocked");
    CHECK(logged("over ") == 0 && empty("blocked"), "blocked");

    fresh_shared();
    gate = gate_close("dead.gate");
    cd[0] = make_case("d", 2, &WD, ONE, "hold", d_args);
    ce[0] = make_case("e", 1, &WD, TWO, "hold", e_args);
    cf[0] = make_case("f", 1, &WD, ONE, "quick", f_args);
    worker_of(&d, "dead-d", "alpha", "dead", cd, 1);
    d.width = 2;
    worker_of(&e, "dead-e", "beta", "dead", ce, 1);
    worker_of(&f, "dead-f", "gamma", "dead", cf, 1);
    CHECK(gate >= 0 && start(&d) == 0, "dead-request");
    memset(&want, 0, sizeof(want));
    want.ledger = "dead";
    want.running = 2;
    want.held = 2;
    CHECK(await(&want), "dead-request");
    CHECK(start(&e) == 0, "dead-request");
    want.blocked = "beta";
    CHECK(await(&want), "dead-request");
    kill(e.pid, SIGKILL);
    CHECK(reap_within(e.pid) != -1, "dead-request");
    want.blocked = NULL;
    want.gone = "beta";
    CHECK(await(&want), "dead-request");
    CHECK(start(&f) == 0, "dead-request");
    want.blocked = "gamma";
    CHECK(await(&want), "dead-request");
    gate_open(gate);
    CHECK(finish(&d, &s) == 0 && all_passed(&s, 2), "dead-request");
    CHECK(finish(&f, &s) == 0 && all_passed(&s, 1), "dead-request");
    CHECK(logged("start e") == 0 && empty("dead"), "dead-request");
}

/**
 * Run a case blocked forever beside one that passes: it runs out of its gap,
 * and so does its retry, each ended with its cleanup confirmed and its claim
 * released, and the history keeps every attempt.
 */
static void test_stuck(void)
{
    static const char *const stuck_args[] = {"stuck.gate", "stuck", NULL};
    static const char *const quick_args[] = {"fine", NULL};
    struct etude_repeat_case c[2];
    struct worker w;
    struct summary s;
    struct records r;
    size_t i, stuck = 0;
    int gate;

    fresh_shared();
    gate = gate_close("stuck.gate");
    c[0] = make_case("stuck", 1, &STUCK_WD, ONE, "stuck", stuck_args);
    c[1] = make_case("fine", 1, &WD, ONE, "quick", quick_args);
    worker_of(&w, "stuck", "alpha", "stuck", c, 2);
    w.width = 2;
    w.retries = 1;
    CHECK(gate >= 0 && start(&w) == 0 && finish(&w, &s) == 0, "stuck");
    CHECK(s.rc == 0 && s.outcome == ETUDE_REPEAT_INCOMPLETE && s.n == 3, "stuck");
    for (i = 0; i < s.n; i++) {
        if (strstr(s.a[i].id, "runtime.stuck") == NULL) {
            CHECK(s.a[i].outcome == ETUDE_REPEAT_PASS, "stuck");
            continue;
        }
        stuck++;
        CHECK(s.a[i].outcome == ETUDE_REPEAT_INCOMPLETE && strstr(s.a[i].reason, "gap") != NULL &&
                  s.a[i].cleanup == ETUDE_CLEANUP_CONFIRMED,
              "stuck");
    }
    CHECK(stuck == 2 && logged("start stuck") == 2, "stuck");
    CHECK(records("stuck", &r) == 0 && r.attempts == 3 && r.runs == 1, "stuck");
    CHECK(empty("stuck"), "stuck");
    gate_open(gate);
}

/* ---- The remote case. ---- */

/* The remote case's program: this one, ended only by its responder. */
static const char prog[] = "#!/bin/sh\nexec \"$1\" --case hang\n";

/** Make the invocation of attempt `attempt` of run `run` into `out`. */
static void invocation(char *out, size_t size, const char *run, const char *attempt)
{
    char sha[65], tok[65];

    sha_hex(prog, strlen(prog), sha);
    sha_hex(attempt, strlen(attempt), tok);
    tok[16] = '\0';
    fix_format(out, size,
               "{\"schema_version\": 1, \"run_id\": \"%s\", \"attempt_id\": \"%s\", "
               "\"attempt_token\": \"%s\", \"case_id\": \"host.etude.runtime\", "
               "\"program\": \"prog.sh\", \"arguments\": [\"%s\"], "
               "\"environment\": {\"PATH\": \"/bin:/usr/bin\", \"HOME\": \"${WORKSPACE}\"}, "
               "\"stdin\": null, \"role\": \"ordinary\", "
               "\"files\": [{\"name\": \"prog.sh\", \"sha256\": \"%s\", \"size_bytes\": %lu, "
               "\"mode\": \"0755\"}], "
               "\"fixtures\": [{\"fixture\": \"workspace\", \"given\": []}], "
               "\"deadline_us\": 60000000, \"grace_us\": 1000000, "
               "\"output_max_bytes\": 65536}",
               run, attempt, tok, self, sha, (unsigned long)strlen(prog));
}

/** Ask a remote case to stop; the transport here is the controller's own. */
static int no_ask(void *ctx, const char *attempt_id)
{
    (void)ctx;
    (void)attempt_id;
    return ENOTCONN;
}

/**
 * Be the controller of run `run`: admit a claim for a host case and one for
 * a remote case through relay mode `mode`, run both under one supervisor,
 * end each claim as its result says, and write what happened to
 * RUN.summary.
 */
static void controller_main(const char *run, const char *mode)
{
    static const struct etude_ledger_res host_res[] = {{"cpu_millis", 100}};
    static char *host_argv[] = {self, "--case", "quick", "host", NULL};
    static char *argv[9];
    struct etude_remote_conf conf;
    struct etude_remote_result res;
    struct etude_remote *conn = NULL;
    struct etude_supervisor *sup = etude_supervisor_new();
    struct etude_supervised *h = NULL, *rm = NULL, *f;
    const struct etude_supervised_result *hr = NULL, *rr = NULL;
    struct etude_exec_spec spec;
    struct etude_ask ask = {no_ask, NULL};
    struct etude_ledger *l = etude_ledger_open(fix_path("transport"));
    enum etude_remote_status st = ETUDE_REMOTE_SYSTEM;
    char host_id[64], remote_id[64], marker[64], journal[64], inv[4096], why[256];
    char verdict[PATH_MAX], path[64];
    FILE *out;
    int left = 2;

    snprintf(host_id, sizeof(host_id), "%s-host", run);
    snprintf(remote_id, sizeof(remote_id), "%s-remote", run);
    snprintf(marker, sizeof(marker), "%s.marker", run);
    snprintf(journal, sizeof(journal), "%s.journal", run);
    if (sup == NULL || l == NULL ||
        etude_ledger_admit(l, host_id, "alpha", host_res, 1, CAP2, 2, 0) != 0 ||
        etude_ledger_admit(l, remote_id, "alpha", ONE, 2, CAP2, 2, 0) != 0 ||
        etude_ledger_verdict_path(l, host_id, verdict, sizeof(verdict)) != 0)
        _exit(2);
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = host_id;
    spec.argv = host_argv;
    spec.envp = case_env;
    spec.output_max = 4096;
    spec.grace_us = 300 * MS;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = l;
    spec.verdict = verdict;
    if (etude_supervise_host(sup, &spec, &WD, &h) != 0 ||
        etude_supervise_remote(sup, remote_id, &WD, SECOND, &ask, &rm) != 0)
        _exit(2);
    argv[0] = self;
    argv[1] = "--relay";
    argv[2] = (char *)mode;
    argv[3] = (char *)fix_path(marker);
    argv[4] = self;
    argv[5] = "--serve";
    argv[6] = (char *)fix_path("remote");
    argv[7] = BOOT;
    argv[8] = NULL;
    memset(&conf, 0, sizeof(conf));
    conf.argv = argv;
    conf.run_id = run;
    conf.journal = fix_path(journal);
    conf.stage_us = 10 * SECOND;
    conf.reconnects = 2;
    memset(&res, 0, sizeof(res));
    res.end = ETUDE_END_LOST;
    if (etude_remote_open(&conf, &conn, why, sizeof(why)) == ETUDE_REMOTE_OK &&
        etude_remote_put(conn, "prog.sh", prog, strlen(prog), "0755") == ETUDE_REMOTE_OK) {
        invocation(inv, sizeof(inv), run, remote_id);
        st = etude_remote_run(conn, inv, strlen(inv), 0, &res);
    }
    if (st == ETUDE_REMOTE_OK && res.complete)
        etude_supervised_end(rm, res.end, res.value, res.cleanup);
    else
        etude_supervised_lost(rm);
    while (left > 0) {
        f = etude_supervise_wait(sup, -1);
        if (f == NULL)
            continue;
        left--;
        if (f == h)
            hr = etude_supervised_result(f);
        else
            rr = etude_supervised_result(f);
        etude_ledger_finish(l, f == h ? host_id : remote_id,
                            etude_supervised_result(f)->claim_kept ? ETUDE_CLEANUP_UNCONFIRMED
                                                                   : ETUDE_CLEANUP_CONFIRMED);
    }
    snprintf(path, sizeof(path), "%s.summary", run);
    if ((out = fopen(fix_path(path), "w")) == NULL)
        _exit(2);
    fprintf(out, "%d %d %d %d %ld %u %d %d %d %d %d %d\n", (int)st, res.complete, (int)res.end,
            (int)res.cleanup, res.started ? res.process.pid : 0L, res.reconnects, (int)rr->end,
            (int)rr->cleanup, rr->claim_kept, (int)hr->end, hr->value, (int)hr->cleanup);
    if (fclose(out) != 0)
        _exit(2);
    etude_remote_result_free(&res);
    if (conn != NULL)
        etude_remote_close(conn);
    _exit(0);
}

/* What a controller reported. */
struct remote_summary {
    int status, complete, end, cleanup;
    long pid;
    unsigned reconnects;
    int sup_end, sup_cleanup, claim_kept, host_end, host_value, host_cleanup;
};

/** Run a controller of run `run` through relay mode `mode`; 0 with its
 * report in `s`, or -1. */
static int controller(const char *run, const char *mode, struct remote_summary *s)
{
    char path[64];
    FILE *f;
    pid_t pid;
    int status, n;

    fflush(stdout);
    if ((pid = fork()) < 0)
        return -1;
    if (pid == 0)
        controller_main(run, mode);
    status = reap_within(pid);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s.summary", run);
    if ((f = fopen(fix_path(path), "r")) == NULL)
        return -1;
    n = fscanf(f, "%d %d %d %d %ld %u %d %d %d %d %d %d", &s->status, &s->complete, &s->end,
               &s->cleanup, &s->pid, &s->reconnects, &s->sup_end, &s->sup_cleanup, &s->claim_kept,
               &s->host_end, &s->host_value, &s->host_cleanup);
    fclose(f);
    return n == 12 ? 0 : -1;
}

/**
 * Say whether the responder recorded the end and the confirmed cleanup of
 * every attempt of run `run`, and ran exactly `attempts` of them.
 */
static int responder_recorded(const char *run, int attempts)
{
    char tok[65], cmd[PATH_MAX * 2];

    sha_hex(run, strlen(run), tok);
    tok[16] = '\0';
    fix_format(cmd, sizeof(cmd),
               "d='%s/%s'; test \"$(ls \"$d/att\" | wc -l)\" -eq %d && "
               "test \"$(cat \"$d\"/cmd/* | grep -c '^cleaned .* confirmed$')\" -eq %d",
               fix_path("remote"), tok, attempts, attempts);
    return system(cmd) == 0;
}

/**
 * Lose a remote case's transport for good while the case runs, beside a host
 * case that passes: the host claim is released, and the remote one ends lost
 * and stays uncertain after its controller has gone, though the responder
 * ended the case and recorded its cleanup, until a person drops it. A
 * transport lost once and made again gives the end the responder recorded,
 * the case's cancellation, and its claim is released.
 */
static void test_transport(void)
{
    struct remote_summary s;

    memset(&s, 0, sizeof(s));
    CHECK(controller("transport-down", "down", &s) == 0, "transport");
    CHECK(s.status == ETUDE_REMOTE_LOST && !s.complete && s.end == ETUDE_END_LOST, "transport");
    CHECK(s.sup_end == ETUDE_END_LOST && s.sup_cleanup == ETUDE_CLEANUP_UNCONFIRMED && s.claim_kept,
          "transport");
    CHECK(s.host_end == ETUDE_END_EXITED && s.host_value == 0 &&
              s.host_cleanup == ETUDE_CLEANUP_CONFIRMED,
          "transport");
    CHECK(!kept("transport", "transport-down-host"), "transport");
    CHECK(uncertain("transport", "transport-down-remote", "unconfirmed"), "transport");
    CHECK(s.pid > 0 && !runs(s.pid), "transport");
    if (s.pid > 0 && runs(s.pid))
        kill((pid_t)s.pid, SIGKILL);
    CHECK(responder_recorded("transport-down", 1), "transport");
    {
        struct etude_ledger *l = etude_ledger_open(fix_path("transport"));

        CHECK(l != NULL && etude_ledger_drop(l, "transport-down-remote") == 0, "transport");
        if (l != NULL)
            etude_ledger_close(l);
    }
    CHECK(empty("transport"), "transport");

    memset(&s, 0, sizeof(s));
    CHECK(controller("transport-once", "once", &s) == 0, "transport-again");
    CHECK(s.status == ETUDE_REMOTE_OK && s.complete && s.end == ETUDE_END_STOPPED &&
              s.cleanup == ETUDE_CLEANUP_CONFIRMED && s.reconnects == 1,
          "transport-again");
    CHECK(s.sup_end == ETUDE_END_STOPPED && !s.claim_kept, "transport-again");
    CHECK(s.pid > 0 && !runs(s.pid), "transport-again");
    CHECK(responder_recorded("transport-once", 1), "transport-again");
    CHECK(empty("transport"), "transport-again");
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--case") == 0)
        return case_main(argv[2], argv + 3, argc - 3);
    if (argc == 4 && strcmp(argv[1], "--serve") == 0) {
        struct etude_serve_conf conf;

        memset(&conf, 0, sizeof(conf));
        conf.in = 0;
        conf.out = 1;
        conf.state = argv[2];
        conf.boot = argv[3];
        conf.log = stderr;
        return etude_serve(&conf);
    }
    if (argc >= 5 && strcmp(argv[1], "--relay") == 0)
        return relay_main(argv[2], argv[3], argv + 4);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-runtime-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (fixtures() != 0) {
        printf("etude-runtime-test: fail: the fixtures cannot be made\n");
        return 1;
    }
    test_accounts();
    test_lost_worker();
    test_full_store();
    test_full_stream();
    test_blocked();
    test_stuck();
    test_transport();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_runtime_hosttest: pass\n");
    return 0;
}
