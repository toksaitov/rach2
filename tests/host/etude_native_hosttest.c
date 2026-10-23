/**
 * Check `etude run native` (tools/etude/posix/run_native.c) running the
 * TinyCC build of tests/native/tccbuild.sh through `etude-guest --stdio`
 * (tools/etude/guest/guest.c) over the controller's side of remote
 * execution (tools/etude/posix/remote.c). This program is `etude run
 * native`, run with `--native`; etude-guest, run with `--guest`; and the
 * transport between them, run with `--relay`, which passes the frames on
 * and, on the first connection of a row, cuts the connection, holds it open
 * on the machine's side, or kills etude-guest. The build's compiler is the
 * host's cc, which tccbuild.sh takes from TCC.
 *
 * host-deps: tools/etude/posix/run_native.c tools/etude/posix/remote.c
 * host-deps: tools/etude/posix/process.c tools/etude/json.c
 * host-deps: tools/etude/guest/guest.c tools/etude/guest/fixture.c
 * host-deps: tools/etude/guest/wire.c tools/etude/history_json.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/guest
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-first: it runs for minutes
 *
 * A build that passes must pass with its output whole and its workspace
 * removed; a build that fails on purpose must be a failure with the
 * compiler's status and message; a build cancelled at the controller's
 * deadline must be cancelled; a build whose connection is cut while it runs
 * must be stopped and reported, after a new connection, as stopped when its
 * connection was lost; a connection cut after the build's end must be
 * resumed with the pass; a connection that the machine's side holds open,
 * its etude-guest still running the build, must be taken over by the next
 * connection's etude-guest, which ends the first and reports the build
 * stopped; and an etude-guest killed during the build must leave the build's
 * end recorded as lost, with its cleanup confirmed by the next etude-guest
 * once the build's processes have ended. In every row the cleanup is
 * confirmed, and the test waits until every process of the case, of
 * etude-guest and of the transport has ended after `etude run native`.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../tools/etude/guest/wire.h"
#include "../../utils/sha256.h"
#include "compat/fixdir_host.c"
#include "etude/frame.h"

int etude_guest_main(int argc, char **argv);
int etude_native_main(int argc, char **argv);

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            printf("etude-run-native-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                    \
            fails++;                                                                          \
        }                                                                                     \
    } while (0)

/* The case's script, from the root of the tree. */
#define CASE_SCRIPT "tests/native/tccbuild.sh"

/* The bytes of a path, as many as realpath(3) may write on Linux; the
 * freestanding <limits.h> of the host tests' build has no PATH_MAX. */
#define PATH_CAP 4096

/* This program, by an absolute path. */
static char self[PATH_CAP];

/** Sleep `ms` milliseconds. */
static void pause_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /* interval: a process or a group is looked at again every 20 ms */
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/** Write all of `n` bytes. */
static void write_all(int fd, const char *p, size_t n)
{
    ssize_t w;

    while (n > 0) {
        w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/** Say whether `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/** Read at most `cap - 1` bytes of `path` into `buf` with a NUL; "" when it
 * cannot be read. */
static void slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f != NULL) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
}

/** Give the first ETUDE_FRAME_TOKEN_LEN digits of the SHA-256 of `text`. */
static void token(const char *text, char out[ETUDE_FRAME_TOKEN_LEN + 1])
{
    struct sha256 h;
    unsigned char d[32];
    char hex[65];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)text, strlen(text));
    sha256_final(&h, d);
    sha256_hex(d, hex);
    memcpy(out, hex, ETUDE_FRAME_TOKEN_LEN);
    out[ETUDE_FRAME_TOKEN_LEN] = '\0';
}

/* ---- The transport, `--relay MODE MARKER COMMAND...`. ---- */

/* The relay of one connection. Its fault applies on the first connection of
 * a row, which the marker file's absence shows. */
struct relay {
    const char *mode;
    int first;
    int to, from; /* etude-guest's input and output */
    pid_t guest;
    int cut_next; /* cut the connection inside the next frame */
};

/** End the connection: write `n` bytes of `line`, then close both ways. */
static void cut(struct relay *rl, const char *line, size_t n)
{
    write_all(1, line, n);
    close(1);
    close(rl->to);
    close(rl->from);
    _exit(0);
}

/**
 * Leave etude-guest running as a connection that the machine has not seen
 * end: a process of its own keeps etude-guest's input open and reads its
 * output until etude-guest ends, while the controller's side of the
 * connection is closed.
 */
static void hold(struct relay *rl)
{
    char buf[4096];
    pid_t pid = fork();

    if (pid == 0) {
        close(0);
        close(1);
        while (read(rl->from, buf, sizeof(buf)) > 0) {
        }
        _exit(0);
    }
    close(1);
    _exit(0);
}

/** Say whether a frame is output of the build loop's that says it built. */
static int says_built(const struct wire_frame *f)
{
    unsigned char bytes[1 << 12];
    long n;

    if (strcmp(f->type, ETUDE_EV_OUT) != 0 || f->nfields != 5 || strcmp(f->field[2], "1") != 0 ||
        strlen(f->field[4]) / 4 * 3 >= sizeof(bytes))
        return 0;
    n = wire_unbase64(f->field[4], bytes);
    if (n <= 0)
        return 0;
    bytes[n] = '\0';
    return strstr((const char *)bytes, "tccbuild: build ") != NULL;
}

/** Pass a line from etude-guest to the controller, applying the fault. */
static void toward_controller(struct relay *rl, char *line, size_t len)
{
    static char copy[1 << 17];
    struct wire_frame f;
    const char *m = rl->mode;
    int status;

    if (rl->cut_next)
        cut(rl, line, len / 2);
    write_all(1, line, len);
    write_all(1, "\n", 1);
    if (!rl->first || len >= sizeof(copy))
        return;
    memcpy(copy, line, len);
    copy[len] = '\0';
    if (wire_parse(copy, len, &f) != 0)
        return;
    if (strcmp(m, "cut-exit") == 0 && strcmp(f.type, ETUDE_EV_EXIT) == 0)
        rl->cut_next = 1;
    if (!says_built(&f))
        return;
    if (strcmp(m, "cut-running") == 0)
        cut(rl, "", 0);
    if (strcmp(m, "hold") == 0)
        hold(rl);
    if (strcmp(m, "kill") == 0) {
        kill(rl->guest, SIGKILL);
        waitpid(rl->guest, &status, 0);
        cut(rl, "", 0);
    }
}

/** Pass on the whole lines of `buf`, keeping a partial one; `toward` is 0
 * for the lines to etude-guest and 1 for those to the controller. */
static void pass_lines(struct relay *rl, char *buf, size_t *len, int toward)
{
    size_t start = 0, i;

    for (i = 0; i < *len; i++) {
        if (buf[i] != '\n')
            continue;
        if (toward == 0) {
            write_all(rl->to, buf + start, i - start + 1);
        } else {
            toward_controller(rl, buf + start, i - start);
        }
        start = i + 1;
    }
    memmove(buf, buf + start, *len - start);
    *len -= start;
}

/** Run the relay: `argv` starts etude-guest. */
static int relay_main(const char *mode, const char *marker, char **argv)
{
    static char cbuf[1 << 18], gbuf[1 << 18];
    struct pollfd pfd[2];
    struct relay rl;
    size_t clen = 0, glen = 0;
    int to[2], from[2], status, fd, open_c = 1;
    ssize_t n;

    memset(&rl, 0, sizeof(rl));
    signal(SIGPIPE, SIG_IGN);
    rl.mode = mode;
    rl.first = !exists(marker);
    fd = open(marker, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0)
        close(fd);
    if (pipe(to) != 0 || pipe(from) != 0)
        return 1;
    rl.guest = fork();
    if (rl.guest == 0) {
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
    rl.to = to[1];
    rl.from = from[0];
    for (;;) {
        pfd[0].fd = open_c ? 0 : -1;
        pfd[0].events = POLLIN;
        pfd[1].fd = rl.from;
        pfd[1].events = POLLIN;
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (open_c && (pfd[0].revents & (POLLIN | POLLHUP)) != 0) {
            n = read(0, cbuf + clen, sizeof(cbuf) - clen);
            if (n <= 0) {
                open_c = 0;
                close(rl.to);
            } else {
                clen += (size_t)n;
                pass_lines(&rl, cbuf, &clen, 0);
            }
        }
        if ((pfd[1].revents & (POLLIN | POLLHUP)) != 0) {
            n = read(rl.from, gbuf + glen, sizeof(gbuf) - glen);
            if (n <= 0)
                break;
            glen += (size_t)n;
            pass_lines(&rl, gbuf, &glen, 1);
        }
    }
    close(1);
    if (open_c)
        close(rl.to);
    waitpid(rl.guest, &status, 0);
    return 0;
}

/* ---- The rows. ---- */

/* One row: a run of `etude run native` through the relay. */
struct row {
    const char *name;   /* the row, and its run's identity */
    const char *mode;   /* the relay's fault */
    const char *build;  /* tccbuild.sh's argument */
    const char *cancel; /* the command's -x, or NULL */
};

/* What a row gave. */
struct outcome {
    int status;           /* the command's exit status, or -1 */
    char out[1 << 16];    /* what it printed on its standard output */
    char err[1 << 16];    /* and on its standard error */
    char result[1024];    /* its result line */
    long process;         /* the case's process, from the result, or 0 */
    char state[PATH_CAP]; /* etude-guest's state directory */
    char attempt[ETUDE_FRAME_TOKEN_LEN + 1];
};

/** Wait for `pid` to end; its status, or -1. */
static int wait_for(pid_t pid)
{
    int status;
    pid_t w;

    while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
    }
    if (w != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/** Wait until the process group `pgid` has no process left. */
static void group_gone(long pgid)
{
    while (pgid > 0 && !(kill((pid_t)-pgid, 0) != 0 && errno == ESRCH))
        pause_ms(20);
}

/**
 * Run `etude run native` for row `w` as the leader of a process group of its
 * own, which the relays, the etude-guests and the holders join, and fill `o`;
 * then wait until neither that group nor the case's group has a process left.
 */
static void run_row(const struct row *w, struct outcome *o)
{
    static char marker[PATH_CAP], journal[PATH_CAP], evid[PATH_CAP], outp[PATH_CAP];
    static char errp[PATH_CAP], transport[4 * PATH_CAP + 64], attempt[96], case_id[96];
    char *argv[48];
    const char *p;
    pid_t pid;
    int n = 0, fd;

    memset(o, 0, sizeof(*o));
    fix_format(o->state, sizeof(o->state), "%s/%s.state", fix_dir(), w->name);
    fix_format(marker, sizeof(marker), "%s/%s.marker", fix_dir(), w->name);
    fix_format(journal, sizeof(journal), "%s/%s.journal", fix_dir(), w->name);
    fix_format(evid, sizeof(evid), "%s/%s.evidence", fix_dir(), w->name);
    fix_format(outp, sizeof(outp), "%s/%s.out", fix_dir(), w->name);
    fix_format(errp, sizeof(errp), "%s/%s.err", fix_dir(), w->name);
    fix_format(attempt, sizeof(attempt), "%s-attempt", w->name);
    fix_format(case_id, sizeof(case_id), "tccbuild.%s", w->build);
    fix_format(transport, sizeof(transport), "'%s' --relay %s '%s' '%s' --guest '%s'", self,
               w->mode, marker, self, o->state);
    token(attempt, o->attempt);
    argv[n++] = self;
    argv[n++] = "--native";
    argv[n++] = "-R";
    argv[n++] = (char *)w->name;
    argv[n++] = "-a";
    argv[n++] = attempt;
    argv[n++] = "-c";
    argv[n++] = case_id;
    argv[n++] = "-j";
    argv[n++] = journal;
    argv[n++] = "-e";
    argv[n++] = evid;
    argv[n++] = "-t";
    argv[n++] = transport;
    argv[n++] = "-f";
    argv[n++] = CASE_SCRIPT;
    argv[n++] = "-E";
    argv[n++] = "TCC=cc";
    argv[n++] = "-d";
    argv[n++] = "20000";
    argv[n++] = "-g";
    argv[n++] = "1000";
    argv[n++] = "-s";
    argv[n++] = "5000";
    argv[n++] = "-r";
    argv[n++] = "2";
    if (w->cancel != NULL) {
        argv[n++] = "-x";
        argv[n++] = (char *)w->cancel;
    }
    argv[n++] = "--";
    argv[n++] = "tccbuild.sh";
    argv[n++] = (char *)w->build;
    argv[n] = NULL;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        fd = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, 1) < 0)
            _exit(127);
        fd = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, 2) < 0)
            _exit(127);
        execv(self, argv);
        _exit(127);
    }
    if (pid < 0) {
        o->status = -1;
        return;
    }
    setpgid(pid, pid);
    o->status = wait_for(pid);
    group_gone(pid);
    slurp(outp, o->out, sizeof(o->out));
    slurp(errp, o->err, sizeof(o->err));
    p = strstr(o->out, "etude run: result: ");
    if (p != NULL)
        snprintf(o->result, sizeof(o->result), "%.*s", (int)strcspn(p, "\n"), p);
    p = strstr(o->result, "; process ");
    if (p != NULL)
        o->process = atol(p + 10);
    group_gone(o->process);
    printf("etude-run-native-test: %s: status %d: %s\n", w->name, o->status, o->result);
}

/** Say whether the row's result line has `text`. */
static int says(const struct outcome *o, const char *text)
{
    return strstr(o->result, text) != NULL;
}

static void check_pass(const struct outcome *o)
{
    char work[PATH_CAP + 64];

    fix_format(work, sizeof(work), "%s/a/%s/work", o->state, o->attempt);
    CHECK(o->status == 0 && says(o, "result: pass: case tccbuild.pass") && says(o, ": exited 0;") &&
              says(o, "cleanup confirmed, reconnections 0") &&
              strstr(o->out, "tccbuild: sum 5050\ntccbuild: pass\n") != NULL,
          "a build that passes passes with its output whole");
    CHECK(!exists(work), "a passing build's workspace is removed");
}

static void check_fail(const struct outcome *o)
{
    CHECK(o->status == 1 && says(o, "result: failure: case tccbuild.fail") &&
              says(o, ": exited 1;") && says(o, "cleanup confirmed, reconnections 0") &&
              strstr(o->err, "this build fails on purpose") != NULL,
          "a build that fails on purpose is a failure with the compiler's status and message");
}

static void check_cancel(const struct outcome *o)
{
    CHECK(o->status == 3 && says(o, "result: cancelled: case tccbuild.loop") &&
              says(o, ": stopped at the controller's deadline;") &&
              says(o, "cleanup confirmed, reconnections 0") &&
              strstr(o->out, "tccbuild: build 1\n") != NULL,
          "a build cancelled at the controller's deadline is cancelled");
}

static void check_cut_running(const struct outcome *o)
{
    CHECK(o->status == 4 && says(o, "result: incomplete: case tccbuild.loop") &&
              says(o, ": stopped when its connection was lost;") &&
              says(o, "cleanup confirmed, reconnections 1"),
          "a build whose connection is cut is reported stopped after a new connection");
}

static void check_cut_exit(const struct outcome *o)
{
    CHECK(o->status == 0 && says(o, "result: pass: case tccbuild.pass") && says(o, ": exited 0;") &&
              says(o, "cleanup confirmed, reconnections 1") &&
              strstr(o->out, "tccbuild: pass\n") != NULL,
          "a connection cut after the build's end is resumed with the pass");
}

static void check_hold(const struct outcome *o)
{
    char log[PATH_CAP + 16];
    static char text[1 << 16];

    fix_format(log, sizeof(log), "%s/guest.log", o->state);
    slurp(log, text, sizeof(text));
    CHECK(o->status == 4 && says(o, "result: incomplete: case tccbuild.loop") &&
              says(o, ": stopped when its connection was lost;") &&
              says(o, "cleanup confirmed, reconnections 1"),
          "a connection held open on the machine's side is taken over by the next");
    CHECK(strstr(text, "of an earlier connection is told to end") != NULL,
          "the next connection's etude-guest ends the one that holds the session");
}

static void check_kill(const struct outcome *o)
{
    CHECK(o->status == 4 && says(o, "result: incomplete: case tccbuild.loop") &&
              says(o, ": its end was lost with its etude-guest;") && says(o, "reconnections 1"),
          "a build whose etude-guest was killed has its end recorded as lost");
    CHECK(says(o, "cleanup confirmed, reconnections 1"),
          "the next etude-guest confirms the cleanup once the build's processes have ended");
}

static void test_rows(void)
{
    static const struct {
        struct row w;
        void (*check)(const struct outcome *o);
    } rows[] = {
        {{"native-pass", "none", "pass", NULL}, check_pass},
        {{"native-fail", "none", "fail", NULL}, check_fail},
        {{"native-cancel", "none", "loop", "3000"}, check_cancel},
        {{"native-cut-running", "cut-running", "loop", NULL}, check_cut_running},
        {{"native-cut-exit", "cut-exit", "pass", NULL}, check_cut_exit},
        {{"native-hold", "hold", "loop", NULL}, check_hold},
        {{"native-kill", "kill", "loop", NULL}, check_kill},
    };
    static struct outcome o;
    size_t i;
    int before;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        before = fails;
        run_row(&rows[i].w, &o);
        rows[i].check(&o);
        CHECK(o.process > 0, "the result names the case's process");
        if (fails != before) {
            printf("etude-run-native-test: %s: its output:\n%s", rows[i].w.name, o.out);
            printf("etude-run-native-test: %s: its error output:\n%s", rows[i].w.name, o.err);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--guest") == 0) {
        char *gargv[5];
        gargv[0] = "etude-guest";
        gargv[1] = "--stdio";
        gargv[2] = "--state";
        gargv[3] = argv[2];
        gargv[4] = NULL;
        return etude_guest_main(4, gargv);
    }
    if (argc >= 5 && strcmp(argv[1], "--relay") == 0)
        return relay_main(argv[2], argv[3], argv + 4);
    if (argc >= 2 && strcmp(argv[1], "--native") == 0)
        return etude_native_main(argc - 1, argv + 1);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-run-native-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    if (!exists(CASE_SCRIPT)) {
        printf("etude-run-native-test: fail: %s is not here; run from the root of the tree\n",
               CASE_SCRIPT);
        return 1;
    }
    test_rows();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_native_hosttest: pass\n");
    return 0;
}
