/**
 * Check etude's remote execution (tools/etude/posix/serve.c and
 * posix/remote.c, over the frames of guest/wire.c) with this program as the
 * responder, run with `--serve`, and as a relay between the controller and
 * the responder that damages, repeats or cuts frames on the first connection
 * of a row, run with `--relay`.
 *
 * host-deps: tools/etude/guest/wire.c tools/etude/json.c tools/etude/capture.c
 * host-deps: tools/etude/posix/serve.c tools/etude/posix/remote.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c utils/sha256.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 *
 * Speaking frames itself, the test requires the responder to refuse a hello of
 * another version; to store a file only with the size and digest its put gave;
 * to answer a repeated command from its ledger, so that a run repeated on the
 * same connection runs once, and to refuse a command number reused with other
 * fields and a second run of one attempt; to give an attempt's state and events
 * again, a file of its workspace checked by its digest, and its workspace's
 * removal; to refuse an invocation of another run, of the init role and of a
 * fixture it does not make; to end the connection, acting on nothing more, at a
 * damaged, misordered, foreign or truncated frame; and, after a responder was
 * killed, to record its attempt as lost with its cleanup unconfirmed. Through
 * the controller's side, a run must pass with its output whole; a run repeated
 * by the relay must run once; a run repeated with other fields, content damaged
 * in every transfer, and output damaged on the way must give no complete run; a
 * damaged frame and a cut after the end must be recovered by a new connection
 * that reads the same attempt, with the same process, from the responder's
 * ledger; a cut while the program runs must give its cancellation, and a cut
 * followed by a new boot a lost end; a cut transfer must leave no partial file;
 * a deadline must cancel the run; and a hello of another version, a ready with
 * a version that was not offered, and frames of another format must refuse the
 * connection before anything runs.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../tools/etude/guest/wire.h"
#include "../../utils/sha256.h"
#include "compat/fixdir_host.c"
#include "etude/remote.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-remote-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

/* The boot the responders of the test run in, and another. */
#define BOOT       "0123456789abcdef"
#define OTHER_BOOT "ffffffffffffffff"

/* The bound of a connection and of each answer. */
#define STAGE_US 10000000u

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* The case's program: it says its PID, counts its runs in the file its
 * first argument names, writes a file in its workspace, and exits with its
 * second argument, or waits to be ended when that is `wait`. */
static const char prog[] = "#!/bin/sh\n"
                           "echo \"pid=$$\"\n"
                           "echo run >> \"$1\"\n"
                           "echo 'to stderr' >&2\n"
                           "echo made > out.txt\n"
                           "case $2 in\n"
                           "wait) echo waiting; exec sleep 60 ;;\n"
                           "esac\n"
                           "i=0\n"
                           "while [ $i -lt 100 ]; do echo \"line $i of the output\"; i=$((i + 1)); "
                           "done\n"
                           "exit \"$2\"\n";

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

/** Give the first sixteen digits of the SHA-256 of `text`. */
static void token(const char *text, char out[17])
{
    char hex[65];

    sha_hex(text, strlen(text), hex);
    memcpy(out, hex, 16);
    out[16] = '\0';
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

/** Count the lines of a file; 0 when it is missing. */
static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int c, n = 0;

    if (f == NULL)
        return 0;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n')
            n++;
    fclose(f);
    return n;
}

/** Say whether a file exists. */
static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/**
 * Make the invocation of attempt `attempt` of run `run`: the program with
 * the counter and the code as its arguments. It is kept until the next call.
 */
static const char *invocation(const char *run, const char *attempt, const char *role,
                              const char *fixture, const char *counter, const char *code)
{
    static char out[4096];
    char sha[65], tok[17];

    sha_hex(prog, strlen(prog), sha);
    token(attempt, tok);
    fix_format(out, sizeof(out),
               "{\"schema_version\": 1, \"run_id\": \"%s\", \"attempt_id\": \"%s\", "
               "\"attempt_token\": \"%s\", \"case_id\": \"host.etude.remote\", "
               "\"program\": \"prog.sh\", \"arguments\": [\"%s\", \"%s\"], "
               "\"environment\": {\"PATH\": \"/bin:/usr/bin\", \"HOME\": \"${WORKSPACE}\"}, "
               "\"stdin\": null, \"role\": \"%s\", "
               "\"files\": [{\"name\": \"prog.sh\", \"sha256\": \"%s\", \"size_bytes\": %lu, "
               "\"mode\": \"0755\"}], "
               "\"fixtures\": [{\"fixture\": \"%s\", \"given\": []}], "
               "\"deadline_us\": 30000000, \"grace_us\": 1000000, "
               "\"output_max_bytes\": 65536}",
               run, attempt, tok, counter, code, role, sha, (unsigned long)strlen(prog), fixture);
    return out;
}

/* ---- A peer that speaks frames to a responder itself. ---- */

struct peer {
    pid_t pid;
    int to, from;
    char buf[1 << 17];
    size_t len, used;
    uint64_t seq;
    int has_in;
    uint64_t seq_in;
    char session[17];
    char line[1 << 17];
    struct wire_frame f;
};

/** Start a responder on `state` in boot `boot`; 0 on success. */
static int peer_start(struct peer *p, const char *state, const char *boot)
{
    int to[2], from[2];

    memset(p, 0, sizeof(*p));
    if (pipe(to) != 0 || pipe(from) != 0)
        return -1;
    p->pid = fork();
    if (p->pid < 0)
        return -1;
    if (p->pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        close(to[0]);
        close(to[1]);
        close(from[0]);
        close(from[1]);
        execl(self, self, "--serve", state, boot, (char *)NULL);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    p->to = to[1];
    p->from = from[0];
    return 0;
}

/** Read the next frame within the stage's bound; 0 on success. */
static int peer_recv(struct peer *p)
{
    struct pollfd pfd;
    char *nl;
    ssize_t n;
    size_t ll;

    for (;;) {
        if (p->used > 0) {
            memmove(p->buf, p->buf + p->used, p->len - p->used);
            p->len -= p->used;
            p->used = 0;
        }
        nl = memchr(p->buf, '\n', p->len);
        if (nl != NULL) {
            ll = (size_t)(nl - p->buf);
            p->used = ll + 1;
            memcpy(p->line, p->buf, ll);
            if (wire_parse(p->line, ll, &p->f) != 0)
                return -1;
            p->has_in = 1;
            p->seq_in = p->f.seq;
            return 0;
        }
        pfd.fd = p->from;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, (int)(STAGE_US / 1000)) <= 0)
            return -1;
        n = read(p->from, p->buf + p->len, sizeof(p->buf) - p->len - 1);
        if (n <= 0)
            return -1;
        p->len += (size_t)n;
    }
}

/** Format a frame of the session with the given fields, ending with NULL. */
static size_t peer_format(struct peer *p, char *out, size_t cap, const char *boot, const char *type,
                          const char *const *fields)
{
    int n = 0;

    while (fields[n] != NULL)
        n++;
    return wire_encode(out, cap, p->session, boot, p->seq++, p->has_in, p->seq_in, type, n, fields);
}

/** Send a frame of the session and boot; the fields end with NULL. */
static void peer_send(struct peer *p, const char *type, const char *const *fields)
{
    static char out[1 << 17];
    size_t len = peer_format(p, out, sizeof(out), BOOT, type, fields);

    write_all(p->to, out, len);
}

/** Send a frame and expect the next to be `type` with `field0` first. */
static int peer_call(struct peer *p, const char *type, const char *const *fields, const char *want)
{
    peer_send(p, type, fields);
    return peer_recv(p) == 0 && strcmp(p->f.type, want) == 0;
}

/** Close the connection and give the responder's exit status. */
static int peer_end(struct peer *p)
{
    int status;

    if (p->to >= 0)
        close(p->to);
    close(p->from);
    if (waitpid(p->pid, &status, 0) != p->pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/** Read `alive` and say hello for run `run`; 1 when `ready` came. */
static int peer_hello(struct peer *p, const char *run)
{
    const char *f[] = {"1", "1", "65536", NULL};

    token(run, p->session);
    if (peer_recv(p) != 0 || strcmp(p->f.type, ETUDE_EV_ALIVE) != 0)
        return 0;
    return peer_call(p, ETUDE_CMD_HELLO, f, ETUDE_EV_READY);
}

/** Put `n` bytes as `name` under command `cmd`, with `sha` as their digest. */
static void peer_put(struct peer *p, const char *cmd, const char *name, const char *data, size_t n,
                     const char *sha)
{
    static char b64[1 << 16];
    char size[24];
    const char *put[] = {cmd, name, size, sha, "0755", NULL};
    const char *data_f[] = {cmd, "0", b64, NULL};
    const char *seal[] = {cmd, NULL};

    snprintf(size, sizeof(size), "%lu", (unsigned long)n);
    peer_send(p, ETUDE_CMD_PUT, put);
    if (n > 0) {
        wire_base64((const unsigned char *)data, n, b64);
        peer_send(p, ETUDE_CMD_DATA, data_f);
    }
    peer_send(p, ETUDE_CMD_SEAL, seal);
}

/**
 * Read the events of a run until its `cleaned`, joining each as `TYPE
 * FIELD...` lines into `out`; 1 when `cleaned` came.
 */
static int peer_events(struct peer *p, char *out, size_t cap)
{
    size_t len = 0;
    int i;

    out[0] = '\0';
    while (peer_recv(p) == 0) {
        len += (size_t)snprintf(out + len, cap - len, "%s", p->f.type);
        for (i = 0; i < p->f.nfields && len < cap; i++)
            len += (size_t)snprintf(out + len, cap - len, " %s", p->f.field[i]);
        if (len < cap)
            len += (size_t)snprintf(out + len, cap - len, "\n");
        if (len >= cap)
            return 0;
        if (strcmp(p->f.type, ETUDE_EV_CLEANED) == 0 || strcmp(p->f.type, ETUDE_EV_ERROR) == 0)
            return strcmp(p->f.type, ETUDE_EV_CLEANED) == 0;
    }
    return 0;
}

/** Give the path of `name` in the session directory of run `run`. */
static void session_file(char *out, size_t cap, const char *run, const char *name)
{
    char tok[17];

    token(run, tok);
    fix_format(out, cap, "%s/%s/%s", fix_path("state"), tok, name);
}

static void test_raw_version(void)
{
    const char *f[] = {"1", "2", "65536", NULL};
    struct peer *p = malloc(sizeof(*p));

    CHECK(p != NULL && peer_start(p, fix_path("state"), BOOT) == 0, "version");
    if (p == NULL)
        return;
    token("raw-version", p->session);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_ALIVE) == 0 && p->f.nfields == 3 &&
              strcmp(p->f.field[0], "1") == 0 && strcmp(p->f.boot, BOOT) == 0,
          "alive gives the versions and the boot");
    CHECK(peer_call(p, ETUDE_CMD_HELLO, f, ETUDE_EV_ERROR) &&
              strcmp(p->f.field[1], ETUDE_ERR_VERSION) == 0,
          "a hello of another version is refused");
    CHECK(peer_end(p) == 1, "the connection ends after a refused hello");
    free(p);
}

/* The digest of no content the test stores. */
static const char other_sha[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

/** Store files whole, again, under a reused number, damaged and with a gap. */
static void ledger_store(struct peer *p, const char *run)
{
    const char *put[] = {"4", "x", "5", other_sha, "0644", NULL};
    const char *d1[] = {"4", "0", "Zm9v", NULL};
    const char *d2[] = {"4", "5", "Zm8=", NULL};
    const char *seal[] = {"4", NULL};
    char sha[65], path[PATH_MAX];

    sha_hex(prog, strlen(prog), sha);
    peer_put(p, "2", "prog.sh", prog, strlen(prog), sha);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_STORED) == 0 &&
              strcmp(p->f.field[2], sha) == 0,
          "a whole file is stored");
    peer_put(p, "2", "prog.sh", prog, strlen(prog), sha);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_STORED) == 0 &&
              strcmp(p->f.field[0], "2") == 0,
          "a repeated put is answered from the ledger");
    peer_put(p, "2", "prog.sh", prog, strlen(prog), other_sha);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_ERROR) == 0 &&
              strcmp(p->f.field[1], ETUDE_ERR_CONFLICT) == 0,
          "a command number reused with other fields is refused");
    peer_put(p, "3", "bad.sh", prog, strlen(prog), other_sha);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_ERROR) == 0 &&
              strcmp(p->f.field[0], "3") == 0 && strcmp(p->f.field[1], ETUDE_ERR_DAMAGED) == 0,
          "content whose digest is not the put's is refused");
    session_file(path, sizeof(path), run, "blob/");
    strcat(path, other_sha);
    CHECK(!exists(path), "damaged content is not stored");
    peer_send(p, ETUDE_CMD_PUT, put);
    peer_send(p, ETUDE_CMD_DATA, d1);
    peer_send(p, ETUDE_CMD_DATA, d2);
    peer_send(p, ETUDE_CMD_SEAL, seal);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_ERROR) == 0 &&
              strcmp(p->f.field[0], "4") == 0 && strcmp(p->f.field[1], ETUDE_ERR_DAMAGED) == 0,
          "data with a gap are refused");
}

/** Run an attempt, repeat it, read its state, fetch a file and remove it. */
static void ledger_run(struct peer *p, const char *run, const char *counter)
{
    static char ev1[1 << 16], ev2[1 << 16];
    char isha[65], tok[17], path[PATH_MAX], hex[65], name[64];
    const char *inv = invocation(run, "raw-ledger-1", "ordinary", "workspace", counter, "0");
    const char *r6[] = {"6", tok, isha, NULL}, *r7[] = {"7", tok, isha, NULL};
    const char *st[] = {"8", tok, NULL}, *get[] = {"9", name, NULL};
    const char *rm[] = {"10", tok, NULL}, *st2[] = {"11", tok, NULL};
    unsigned char got[256];

    sha_hex(inv, strlen(inv), isha);
    peer_put(p, "5", ETUDE_REMOTE_INVOCATION, inv, strlen(inv), isha);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_STORED) == 0, "an invocation is stored");
    token("raw-ledger-1", tok);
    snprintf(name, sizeof(name), "%s/work/out.txt", tok);
    peer_send(p, ETUDE_CMD_RUN, r6);
    CHECK(peer_events(p, ev1, sizeof(ev1)) && strncmp(ev1, "started 6 ", 10) == 0 &&
              strstr(ev1, "\nexit 6 ") != NULL && strstr(ev1, " exited 0\nstream 6 ") != NULL &&
              strstr(ev1, "\ncleaned 6 ") != NULL && strstr(ev1, " confirmed\n") != NULL,
          "a run gives started, its output, exit, its streams, usage and cleaned");
    peer_send(p, ETUDE_CMD_RUN, r6);
    CHECK(peer_events(p, ev2, sizeof(ev2)) && strcmp(ev1, ev2) == 0,
          "a repeated run is answered by the same events");
    CHECK(count_lines(counter) == 1, "a repeated run does not run again");
    CHECK(peer_call(p, ETUDE_CMD_RUN, r7, ETUDE_EV_ERROR) &&
              strcmp(p->f.field[1], ETUDE_ERR_CONFLICT) == 0,
          "a second run of one attempt is refused");
    CHECK(count_lines(counter) == 1, "a second run of one attempt does not run");
    CHECK(peer_call(p, ETUDE_CMD_STATUS, st, ETUDE_EV_STATE) &&
              strcmp(p->f.field[2], "cleaned") == 0,
          "status gives the attempt's state");
    CHECK(peer_events(p, ev2, sizeof(ev2)) && strcmp(ev1, ev2) == 0,
          "status gives the run's events again");
    peer_send(p, ETUDE_CMD_GET, get);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_CMD_DATA) == 0 &&
              wire_unbase64(p->f.field[2], got) == 5 && memcmp(got, "made\n", 5) == 0,
          "get sends a file of the workspace");
    sha_hex("made\n", 5, hex);
    CHECK(peer_recv(p) == 0 && strcmp(p->f.type, ETUDE_EV_SENT) == 0 &&
              strcmp(p->f.field[1], name) == 0 && strcmp(p->f.field[2], "5") == 0 &&
              strcmp(p->f.field[3], hex) == 0,
          "get ends with the file's size and digest");
    CHECK(peer_call(p, ETUDE_CMD_REMOVE, rm, ETUDE_EV_REMOVED), "remove removes a workspace");
    session_file(path, sizeof(path), run, "work/");
    strcat(path, tok);
    CHECK(!exists(path), "a removed workspace is gone");
    CHECK(peer_call(p, ETUDE_CMD_STATUS, st2, ETUDE_EV_STATE) &&
              strcmp(p->f.field[2], "cleaned") == 0,
          "the ledger keeps a removed attempt");
    peer_events(p, ev2, sizeof(ev2));
}

/**
 * Store the invocation of `attempt` under command `put` and run it under
 * command `run`; 1 when the run is refused as an invocation the responder
 * cannot run.
 */
static int refused(struct peer *p, const char *put, const char *run, const char *inv,
                   const char *attempt)
{
    char isha[65], tok[17];
    const char *r[] = {run, tok, isha, NULL};

    sha_hex(inv, strlen(inv), isha);
    peer_put(p, put, ETUDE_REMOTE_INVOCATION, inv, strlen(inv), isha);
    peer_recv(p);
    token(attempt, tok);
    return peer_call(p, ETUDE_CMD_RUN, r, ETUDE_EV_ERROR) &&
           strcmp(p->f.field[1], ETUDE_ERR_REFUSED) == 0;
}

/** Refuse what the responder must not run, and end the session. */
static void ledger_refusals(struct peer *p, const char *run, const char *counter)
{
    const char *r12[] = {"12", "1111111111111111", other_sha, NULL};
    const char *ex[] = {"13", "1111111111111111", other_sha, NULL};
    const char *bye[] = {"18", NULL};
    char path[PATH_MAX];

    CHECK(peer_call(p, ETUDE_CMD_RUN, r12, ETUDE_EV_ERROR) &&
              strcmp(p->f.field[1], ETUDE_ERR_UNKNOWN) == 0,
          "a run of an invocation never stored is refused");
    CHECK(peer_call(p, ETUDE_CMD_EXEC, ex, ETUDE_EV_ERROR) &&
              strcmp(p->f.field[1], ETUDE_ERR_REFUSED) == 0,
          "exec is refused over standard input and output");
    CHECK(refused(p, "14", "15", invocation(run, "raw-ledger-2", "init", "workspace", counter, "0"),
                  "raw-ledger-2"),
          "an invocation of the init role is refused");
    CHECK(refused(p, "16", "17",
                  invocation("another-run", "raw-ledger-3", "ordinary", "workspace", counter, "0"),
                  "raw-ledger-3"),
          "an invocation of another run is refused");
    CHECK(refused(p, "19", "20", invocation(run, "raw-ledger-4", "ordinary", "tmpfs", counter, "0"),
                  "raw-ledger-4"),
          "an invocation of a fixture the responder does not make is refused");
    CHECK(count_lines(counter) == 1, "no refused invocation ran");
    CHECK(peer_call(p, ETUDE_CMD_BYE, bye, ETUDE_CMD_BYE), "bye is answered");
    CHECK(peer_end(p) == 0, "the responder ends after bye");
    session_file(path, sizeof(path), run, "blob");
    CHECK(!exists(path), "bye removes the session's files");
}

static void test_raw_ledger(void)
{
    const char *run = "raw-ledger", *counter = fix_path("raw-ledger-runs");
    struct peer *p = malloc(sizeof(*p));

    if (p == NULL || peer_start(p, fix_path("state"), BOOT) != 0) {
        CHECK(0, "ledger");
        free(p);
        return;
    }
    CHECK(peer_hello(p, run), "a hello of version 1 is answered by ready");
    ledger_store(p, run);
    ledger_run(p, run, counter);
    ledger_refusals(p, run, counter);
    free(p);
}

/* The frames that must end a connection, each sent after a hello. */
enum fault { BAD_CRC, SEQ_GAP, OTHER_BOOT_FRAME, OTHER_SESSION, CUT_SHORT, BEFORE_HELLO };

/** Send fault `which`; the responder must end and act on nothing more. */
static void raw_fault(enum fault which, const char *row, const char *why)
{
    char run[64], sha[65], path[PATH_MAX], line[1024], *ev;
    const char *put[] = {"2", "prog.sh", "1", "", "0755", NULL};
    struct peer *p = malloc(sizeof(*p));
    size_t len;
    FILE *f;

    if (p == NULL || peer_start(p, fix_path("state"), BOOT) != 0) {
        CHECK(0, row);
        free(p);
        return;
    }
    snprintf(run, sizeof(run), "raw-fault-%d", (int)which);
    sha_hex("x", 1, sha);
    put[3] = sha;
    if (which == BEFORE_HELLO) {
        token(run, p->session);
        CHECK(peer_recv(p) == 0, row);
        p->seq = 0;
    } else {
        CHECK(peer_hello(p, run), row);
    }
    len = peer_format(p, line, sizeof(line), which == OTHER_BOOT_FRAME ? OTHER_BOOT : BOOT,
                      ETUDE_CMD_PUT, put);
    if (which == BAD_CRC)
        line[10] ^= 1;
    if (which == SEQ_GAP) {
        p->seq++;
        len = peer_format(p, line, sizeof(line), BOOT, ETUDE_CMD_PUT, put);
    }
    if (which == OTHER_SESSION) {
        token("someone-else", p->session);
        p->seq--;
        len = peer_format(p, line, sizeof(line), BOOT, ETUDE_CMD_PUT, put);
    }
    write_all(p->to, line, which == CUT_SHORT ? len / 2 : len);
    if (which != CUT_SHORT) {
        const char *d[] = {"2", "0", "eA==", NULL};
        const char *s[] = {"2", NULL};
        peer_send(p, ETUDE_CMD_DATA, d);
        peer_send(p, ETUDE_CMD_SEAL, s);
    } else {
        close(p->to);
        p->to = -1;
    }
    CHECK(peer_end(p) == 1, row);
    session_file(path, sizeof(path), run, "cmd/2");
    CHECK(!exists(path), row);
    f = fopen(fix_path("state/evidence"), "r");
    ev = NULL;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, why, strlen(why)) == 0)
            ev = line;
    if (f != NULL)
        fclose(f);
    CHECK(ev != NULL, row);
    free(p);
}

static void test_raw_faults(void)
{
    raw_fault(BAD_CRC, "a frame with a bad CRC ends the connection",
              "a damaged or malformed frame");
    raw_fault(SEQ_GAP, "a gap in the sequence ends the connection", "a frame out of order");
    raw_fault(OTHER_BOOT_FRAME, "a frame for another boot ends the connection",
              "a frame for another boot");
    raw_fault(OTHER_SESSION, "a frame of another session ends the connection",
              "a frame of another session");
    raw_fault(CUT_SHORT, "a frame cut short ends the connection",
              "a line cut short by the end of the connection");
    raw_fault(BEFORE_HELLO, "a command before the hello ends the connection",
              "a command before the hello");
}

static void test_raw_recovery(void)
{
    static char ev[1 << 16];
    char sha[65], isha[65], tok[17];
    const char *run = "raw-recovery", *counter = fix_path("raw-recovery-runs"), *inv;
    struct peer *p = malloc(sizeof(*p));
    int status;

    if (p == NULL || peer_start(p, fix_path("state"), BOOT) != 0) {
        CHECK(0, "recovery");
        free(p);
        return;
    }
    CHECK(peer_hello(p, run), "recovery");
    sha_hex(prog, strlen(prog), sha);
    peer_put(p, "2", "prog.sh", prog, strlen(prog), sha);
    peer_recv(p);
    inv = invocation(run, "raw-recovery-1", "ordinary", "workspace", counter, "wait");
    sha_hex(inv, strlen(inv), isha);
    peer_put(p, "3", ETUDE_REMOTE_INVOCATION, inv, strlen(inv), isha);
    peer_recv(p);
    token("raw-recovery-1", tok);
    {
        const char *r4[] = {"4", tok, isha, NULL};
        CHECK(peer_call(p, ETUDE_CMD_RUN, r4, ETUDE_EV_STARTED), "recovery: the run starts");
    }
    kill(p->pid, SIGKILL);
    waitpid(p->pid, &status, 0);
    close(p->to);
    close(p->from);
    CHECK(peer_start(p, fix_path("state"), BOOT) == 0 && peer_hello(p, run),
          "a new responder takes the session of one that was killed");
    {
        const char *st[] = {"5", tok, NULL};
        const char *bye[] = {"6", NULL};
        CHECK(peer_call(p, ETUDE_CMD_STATUS, st, ETUDE_EV_STATE) &&
                  strcmp(p->f.field[2], "ended") == 0,
              "an attempt whose responder was killed has ended");
        CHECK(peer_events(p, ev, sizeof(ev)) && strstr(ev, "\nexit 4 ") != NULL &&
                  strstr(ev, " lost -\nstream 4 ") != NULL && strstr(ev, " 2 - - -\n") != NULL &&
                  strstr(ev, " unconfirmed\n") != NULL,
              "its end is lost, its streams unknown and its cleanup unconfirmed");
        peer_call(p, ETUDE_CMD_BYE, bye, ETUDE_CMD_BYE);
    }
    peer_end(p);
    free(p);
}

/* ---- The controller's side, through the relay. ---- */

/* The faults of the relay. A fault applies on a row's first connection. */
struct relay {
    const char *mode;
    int first;
    int to, from;     /* the responder's input and output */
    uint64_t extra;   /* frames the relay added toward the responder */
    uint64_t dup_seq; /* the SEQ the added frame was given */
    int dupped;
    char put_cmd[24]; /* the put whose data the fault damages */
    int done;         /* the fault has been applied */
    int cut_next;     /* cut the connection inside the next frame */
};

/** Write a frame again, with its fields as they are now. */
static void reemit(int fd, struct wire_frame *f)
{
    static char out[1 << 17];
    size_t len = wire_encode(out, sizeof(out), f->session, f->boot, f->seq, f->has_ack, f->ack,
                             f->type, f->nfields, f->field);

    write_all(fd, out, len);
}

/** End the relay inside a frame: half of `line`, then nothing. */
static void cut(struct relay *rl, int fd, const char *line, size_t len)
{
    int status;

    write_all(fd, line, len / 2);
    close(1);
    close(rl->to);
    while (wait(&status) > 0)
        ;
    _exit(0);
}

/** Damage the first byte that a base64 field carries, keeping it base64. */
static const char *damage(const char *b64)
{
    static unsigned char bytes[1 << 16];
    static char out[1 << 17];
    long n = wire_unbase64(b64, bytes);

    if (n <= 0)
        return b64;
    bytes[0] ^= 0x20;
    wire_base64(bytes, (size_t)n, out);
    return out;
}

/** Pass a line from the controller to the responder. */
static void toward_responder(struct relay *rl, char *line, size_t len)
{
    static char copy[1 << 17];
    struct wire_frame f;
    int active = rl->first;
    const char *m = rl->mode;

    memcpy(copy, line, len);
    copy[len] = '\0';
    if (wire_parse(copy, len, &f) != 0) {
        write_all(rl->to, line, len);
        write_all(rl->to, "\n", 1);
        return;
    }
    f.seq += rl->extra;
    if (active && strcmp(m, "hello-v2") == 0 && strcmp(f.type, ETUDE_CMD_HELLO) == 0)
        f.field[1] = "2";
    if (active && (strcmp(m, "flip-data-once") == 0 || strcmp(m, "flip-data-all") == 0 ||
                   strcmp(m, "cut-put") == 0)) {
        const char *target = strcmp(m, "cut-put") == 0 ? ETUDE_REMOTE_INVOCATION : "prog.sh";
        if (strcmp(f.type, ETUDE_CMD_PUT) == 0 && strcmp(f.field[1], target) == 0)
            snprintf(rl->put_cmd, sizeof(rl->put_cmd), "%s", f.field[0]);
        if (strcmp(f.type, ETUDE_CMD_DATA) == 0 && strcmp(f.field[0], rl->put_cmd) == 0) {
            if (strcmp(m, "cut-put") == 0)
                cut(rl, rl->to, line, len);
            if (!rl->done) {
                f.field[2] = damage(f.field[2]);
                rl->done = strcmp(m, "flip-data-once") == 0;
            }
        }
    }
    reemit(rl->to, &f);
    if (active && !rl->dupped && (strcmp(m, "dup-run") == 0 || strcmp(m, "conflict") == 0) &&
        strcmp(f.type, ETUDE_CMD_RUN) == 0) {
        rl->dupped = 1;
        rl->extra++;
        f.seq++;
        rl->dup_seq = f.seq;
        if (strcmp(m, "conflict") == 0)
            f.field[2] = other_sha;
        reemit(rl->to, &f);
    }
}

/** Change an event toward the controller as the relay's mode says. */
static void alter_event(struct relay *rl, struct wire_frame *f)
{
    if (strcmp(rl->mode, "ready-v2") == 0 && strcmp(f->type, ETUDE_EV_READY) == 0)
        f->field[1] = "2";
    if (strcmp(rl->mode, "flip-out") == 0 && strcmp(f->type, ETUDE_EV_OUT) == 0) {
        f->field[4] = damage(f->field[4]);
        rl->done = 1;
    }
}

/** Write an event with a damaged CRC. */
static void send_damaged(const struct wire_frame *f)
{
    char out[1 << 12];
    size_t n = wire_encode(out, sizeof(out), f->session, f->boot, f->seq, f->has_ack, f->ack,
                           f->type, f->nfields, f->field);

    out[n - 14] = out[n - 14] == 'A' ? 'B' : 'A';
    write_all(1, out, n);
}

/** Say whether an event is output of the program's that says it waits. */
static int says_waiting(const struct wire_frame *f)
{
    unsigned char bytes[1 << 12];
    long n;

    if (strcmp(f->type, ETUDE_EV_OUT) != 0 || strcmp(f->field[2], "1") != 0 ||
        strlen(f->field[4]) / 4 * 3 >= sizeof(bytes))
        return 0;
    n = wire_unbase64(f->field[4], bytes);
    if (n <= 0)
        return 0;
    bytes[n] = '\0';
    return strstr((const char *)bytes, "waiting") != NULL;
}

/** Pass a line from the responder to the controller. */
static void toward_controller(struct relay *rl, char *line, size_t len)
{
    static char copy[1 << 17];
    struct wire_frame f;
    int active = rl->first && !rl->done;
    const char *m = rl->mode;

    if (rl->cut_next)
        cut(rl, 1, line, len + 1);
    memcpy(copy, line, len);
    copy[len] = '\0';
    if (active && strcmp(m, "magic") == 0 && len > 3 && memcmp(line, "#E1", 3) == 0) {
        line[2] = '2';
        rl->done = 1;
        write_all(1, line, len);
        write_all(1, "\n", 1);
        return;
    }
    if (wire_parse(copy, len, &f) != 0) {
        write_all(1, line, len);
        write_all(1, "\n", 1);
        return;
    }
    if (rl->dupped && f.has_ack && f.ack >= rl->dup_seq)
        f.ack--;
    if (active && strcmp(m, "crc") == 0 && strcmp(f.type, ETUDE_EV_EXIT) == 0) {
        send_damaged(&f);
        rl->done = 1;
        return;
    }
    if (active)
        alter_event(rl, &f);
    reemit(1, &f);
    if (active && strcmp(m, "cut-exit") == 0 && strcmp(f.type, ETUDE_EV_EXIT) == 0)
        rl->cut_next = 1;
    if (active && (strcmp(m, "cut-running") == 0 || strcmp(m, "reboot") == 0) && says_waiting(&f))
        cut(rl, 1, "", 0);
}

/** Pass the whole lines of `buf` on, keeping a partial one. */
static void pass_lines(struct relay *rl, char *buf, size_t *len, int toward)
{
    size_t start = 0, i;

    for (i = 0; i < *len; i++) {
        if (buf[i] != '\n')
            continue;
        if (toward == 0)
            toward_responder(rl, buf + start, i - start);
        else
            toward_controller(rl, buf + start, i - start);
        start = i + 1;
    }
    memmove(buf, buf + start, *len - start);
    *len -= start;
}

/** Run the relay: `argv` starts the responder. */
static int relay_main(const char *mode, const char *marker, char **argv)
{
    static char cbuf[1 << 18], sbuf[1 << 18];
    struct pollfd pfd[2];
    struct relay rl;
    size_t clen = 0, slen = 0;
    int to[2], from[2], status, fd, open_c = 1;
    ssize_t n;
    pid_t pid;

    memset(&rl, 0, sizeof(rl));
    signal(SIGPIPE, SIG_IGN);
    rl.mode = mode;
    rl.first = !exists(marker) || strcmp(mode, "flip-data-all") == 0;
    fd = open(marker, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0)
        close(fd);
    if (!rl.first && strcmp(mode, "reboot") == 0 && argv[0] != NULL && argv[1] != NULL &&
        argv[2] != NULL && argv[3] != NULL)
        argv[3] = OTHER_BOOT;
    if (pipe(to) != 0 || pipe(from) != 0)
        return 1;
    pid = fork();
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
            n = read(rl.from, sbuf + slen, sizeof(sbuf) - slen);
            if (n <= 0)
                break;
            slen += (size_t)n;
            pass_lines(&rl, sbuf, &slen, 1);
        }
    }
    close(1);
    if (open_c)
        close(rl.to);
    waitpid(pid, &status, 0);
    return 0;
}

/* One row of the controller's side. */
struct row {
    const char *name; /* the row, and its run's identity */
    const char *mode; /* the relay's fault */
    const char *code; /* the program's exit status, or `wait` */
    uint64_t deadline_us;
    void (*check)(const struct row *w);
};

/* What a row's run gave. */
static struct etude_remote_result res;
static enum etude_remote_status st;
static struct etude_remote *conn;
static int runs;

/**
 * Open a connection through the relay, store the program and run it, setting
 * `res`, `st`, `conn` and `runs`. The configuration is static, since the
 * connection keeps it until it closes.
 */
static void client_row(const struct row *w)
{
    static char marker[PATH_MAX], counter[PATH_MAX], journal[PATH_MAX], evid[PATH_MAX];
    static char *argv[10];
    static struct etude_remote_conf conf;
    char attempt[96], why[256];
    const char *inv;

    fix_format(marker, sizeof(marker), "%s/%s.marker", fix_dir(), w->name);
    fix_format(counter, sizeof(counter), "%s/%s.runs", fix_dir(), w->name);
    fix_format(journal, sizeof(journal), "%s/%s.journal", fix_dir(), w->name);
    fix_format(evid, sizeof(evid), "%s/%s.evidence", fix_dir(), w->name);
    fix_format(attempt, sizeof(attempt), "%s-attempt", w->name);
    argv[0] = self;
    argv[1] = "--relay";
    argv[2] = (char *)w->mode;
    argv[3] = marker;
    argv[4] = self;
    argv[5] = "--serve";
    argv[6] = (char *)fix_path("state");
    argv[7] = BOOT;
    argv[8] = NULL;
    memset(&conf, 0, sizeof(conf));
    conf.argv = argv;
    conf.run_id = w->name;
    conf.journal = journal;
    conf.evidence = evid;
    conf.max_frame = 512;
    conf.stage_us = STAGE_US;
    conf.reconnects = 3;
    memset(&res, 0, sizeof(res));
    conn = NULL;
    runs = 0;
    st = etude_remote_open(&conf, &conn, why, sizeof(why));
    if (st != ETUDE_REMOTE_OK) {
        printf("etude-remote-test: %s: open: %s: %s\n", w->name, etude_remote_status_text(st), why);
        return;
    }
    st = etude_remote_put(conn, "prog.sh", prog, strlen(prog), "0755");
    if (st == ETUDE_REMOTE_OK) {
        inv = invocation(w->name, attempt, "ordinary", "workspace", counter, w->code);
        st = etude_remote_run(conn, inv, strlen(inv), w->deadline_us, &res);
    }
    if (st != ETUDE_REMOTE_OK)
        printf("etude-remote-test: %s: %s: %s\n", w->name, etude_remote_status_text(st),
               etude_remote_why(conn));
    runs = count_lines(counter);
}

/** Say whether the output begins with the PID that `started` named. */
static int same_process(void)
{
    char want[48];

    snprintf(want, sizeof(want), "pid=%ld\n", res.process.pid);
    return res.started && res.out.data != NULL && strncmp(res.out.data, want, strlen(want)) == 0;
}

/** Say whether the run passed whole, with status `code`. */
static int whole_pass(int code)
{
    return st == ETUDE_REMOTE_OK && res.complete && res.end == ETUDE_END_EXITED &&
           res.value == code && res.cleanup == ETUDE_CLEANUP_CONFIRMED && same_process() &&
           strstr(res.out.data, "line 99 of the output\n") != NULL && res.err.data != NULL &&
           strcmp(res.err.data, "to stderr\n") == 0 && res.out.total == res.out.kept && runs == 1;
}

static void check_pass(const struct row *w)
{
    char *data = NULL;
    size_t size;

    (void)w;
    CHECK(whole_pass(0) && res.reconnects == 0, "a remote run passes with its output whole");
    CHECK(res.cpu_us.quality != ETUDE_QUALITY_UNAVAILABLE, "a remote run's usage arrives");
    CHECK(conn != NULL &&
              etude_remote_get(conn, res.attempt, "out.txt", 64, &data, &size) == ETUDE_REMOTE_OK &&
              size == 5 && memcmp(data, "made\n", 5) == 0,
          "a file of the workspace is fetched whole");
    free(data);
    CHECK(conn != NULL && etude_remote_remove(conn, res.attempt) == ETUDE_REMOTE_OK,
          "a workspace is removed");
    CHECK(conn != NULL && etude_remote_get(conn, res.attempt, "out.txt", 64, &data, &size) ==
                              ETUDE_REMOTE_REFUSED,
          "a removed workspace has no file");
}

static void check_fail(const struct row *w)
{
    (void)w;
    CHECK(whole_pass(3), "a failing program's status arrives");
}

static void check_dup_run(const struct row *w)
{
    (void)w;
    CHECK(whole_pass(0), "a run sent twice runs once");
}

static void check_conflict(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_REFUSED && !res.complete && runs <= 1,
          "a run repeated with other fields gives no complete run");
}

static void check_flip_once(const struct row *w)
{
    (void)w;
    CHECK(whole_pass(0), "a file damaged once is sent again, and the run passes");
}

static void check_flip_all(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_DAMAGED && !res.complete && runs == 0,
          "a file damaged every time is never run");
}

static void check_flip_out(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_DAMAGED && !res.complete, "damaged output gives no complete run");
}

static void check_lost_end(const struct row *w)
{
    (void)w;
    CHECK(whole_pass(0) && res.reconnects >= 1,
          "a lost end is read again, of the same process, after a new connection");
}

static void check_cut_running(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_OK && res.complete && res.end == ETUDE_END_STOPPED && same_process() &&
              runs == 1 && res.reconnects == 1,
          "a connection lost while the program runs gives its cancellation");
}

static void check_reboot(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_BOOT && !res.complete && res.end == ETUDE_END_LOST,
          "a new boot leaves the attempt's end lost");
}

static void check_version(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_VERSION && conn == NULL && runs == 0,
          "an incompatible protocol is refused before anything runs");
}

static void check_cut_put(const struct row *w)
{
    char tok[17], cmdline[PATH_MAX + 64];

    token(w->name, tok);
    fix_format(cmdline, sizeof(cmdline), "ls '%s/%s/blob' | grep -q part", fix_path("state"), tok);
    CHECK(whole_pass(0), "a cut transfer is sent again, and the run passes");
    CHECK(system(cmdline) != 0, "a cut transfer leaves no partial file");
}

static void check_deadline(const struct row *w)
{
    (void)w;
    CHECK(st == ETUDE_REMOTE_OK && res.complete && res.end == ETUDE_END_STOPPED && runs == 1,
          "a deadline cancels the run");
}

static void test_client(void)
{
    static const struct row rows[] = {
        {"remote-pass", "none", "0", 0, check_pass},
        {"remote-fail", "none", "3", 0, check_fail},
        {"remote-dup-run", "dup-run", "0", 0, check_dup_run},
        {"remote-conflict", "conflict", "0", 0, check_conflict},
        {"remote-flip-data-once", "flip-data-once", "0", 0, check_flip_once},
        {"remote-flip-data-all", "flip-data-all", "0", 0, check_flip_all},
        {"remote-flip-out", "flip-out", "0", 0, check_flip_out},
        {"remote-crc", "crc", "0", 0, check_lost_end},
        {"remote-cut-exit", "cut-exit", "0", 0, check_lost_end},
        {"remote-cut-running", "cut-running", "wait", 0, check_cut_running},
        {"remote-reboot", "reboot", "wait", 0, check_reboot},
        {"remote-hello-v2", "hello-v2", "0", 0, check_version},
        {"remote-ready-v2", "ready-v2", "0", 0, check_version},
        {"remote-magic", "magic", "0", 0, check_version},
        {"remote-cut-put", "cut-put", "0", 0, check_cut_put},
        {"remote-deadline", "none", "wait", 300000, check_deadline},
    };
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        client_row(&rows[i]);
        rows[i].check(&rows[i]);
        etude_remote_result_free(&res);
        if (conn != NULL)
            CHECK(etude_remote_close(conn) == ETUDE_REMOTE_OK || st != ETUDE_REMOTE_OK,
                  rows[i].name);
    }
}

int main(int argc, char **argv)
{
    struct etude_serve_conf conf;

    if (argc == 4 && strcmp(argv[1], "--serve") == 0) {
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
        printf("etude-remote-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    test_raw_version();
    test_raw_ledger();
    test_raw_faults();
    test_raw_recovery();
    test_client();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_remote_hosttest: pass\n");
    return 0;
}
