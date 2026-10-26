/*
 * Check tools/etude/serial.c, the controller of etude-guest on a serial
 * console, with frames the test writes itself and with etude-guest on pipes.
 *
 * host-tools: etude-workload
 * host-deps: tools/etude/serial.c tools/etude/capture.c
 * host-deps: tools/etude/guest/guest.c tools/etude/guest/fixture.c
 * host-deps: tools/etude/guest/owner.c tools/etude/posix/process.c
 * host-deps: tools/etude/boot_identity.c
 * host-deps: tools/etude/guest/wire.c
 * host-deps: tools/etude/history_json.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/guest
 * host-cflags: -I utils -Wall -Wextra -Werror
 *
 * Speaking as etude-guest, the test requires the controller to wait for an
 * alive before its hello; to send an unacknowledged frame again, with its SEQ
 * and bytes, when it has waited the resend interval, and not once it is
 * acknowledged; to acknowledge every sequenced frame, a repeat and one past a
 * gap included; to act on no damaged frame, repeated frame, frame past a gap,
 * frame of another session or boot, or frame that acknowledges what was not
 * sent; to send a staged put as put and seal alone, and the data of another
 * in frames of at most 255 bytes, four frames in flight at most; to take a
 * frame that follows kernel text on its line; to give output to its caller
 * once and in order, keeping what the bound allows and counting the rest; to
 * end an operation PROTOCOL at an event of a command never sent, of another
 * attempt, of the old ten-field `exit` or out of order, the streams 1 and 2
 * included, and REFUSED at an error, passing by an event of an earlier
 * operation; to end it DAMAGED when a stored digest or a stream's digest
 * disagrees; to take a stream's total from its `stream` frame, and to leave a
 * run whose streams' sizes are not known DONE but not complete; and to keep a
 * run's end LOST when the console closes, or etude-guest starts again, before
 * its whole end arrived.
 *
 * With etude-guest compiled in and run in a child process on pipes, as the
 * serial transport, the test opens a session; stores a staged file without
 * data and a program with data, checked by digest, and is refused a staged
 * file that is not staged; runs a pass, whose output arrives whole and in
 * order, and an intended failure with its status; ignores a frame of another
 * session and one of another boot during a run; cancels a program that runs
 * until it is stopped, and finds its processes gone; ends the session with
 * bye and opens another. Through a relay that drops, repeats, damages and
 * reorders frames both ways, the same operations complete, with frames sent
 * again by both sides. A console closed during a run, and an etude-guest
 * killed and started again during one, end the run LOST and never EXITED.
 */
#include "../../tools/etude/serial.h"
#include "../../tools/etude/guest/wire.h"
#include "../../utils/sha256.h"
#include <etude/frame.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "compat/workload_host.h"

static char *workload;
static size_t workload_size;

int etude_guest_main(int argc, char **argv);

#include "compat/tree_host.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-serial-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fflush(stdout);                                                                       \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

/* The resend interval of both sides, and the most output a controller
 * keeps. */
#define RESEND_US 50000u
#define KEEP      65536

/* How long the relay holds a frame that nothing has followed. */
#define HELD_US 5000u

/* The boot and process of the scripted etude-guest, and a token of neither
 * this session nor its boot. */
#define FAKE_BOOT "0123456789abcdef"
#define OTHER     "ffffffffffffffff"

#define LINES_MAX 512
#define LINE_CAP  (2 * ETUDE_FRAME_SERIAL_MAX)

/* A console between a controller and etude-guest, real or scripted. */
struct harness {
    struct etude_serial *s;
    char session[ETUDE_FRAME_TOKEN_LEN + 1];
    int scripted; /* the controller's frames are kept, and none goes on */
    char lines[LINES_MAX][LINE_CAP];
    size_t line_len[LINES_MAX];
    int nlines;
    char wline[LINE_CAP];
    size_t wlen;
    uint64_t gseq; /* the scripted etude-guest's next SEQ */

    pid_t pid; /* etude-guest's process, or -1 */
    int to, from;
    char state[600];
    char rline[4096];
    size_t rlen;

    int relay;
    unsigned long count[2]; /* frames on their way: 0 to etude-guest, 1 back */
    char held[2][LINE_CAP];
    size_t held_len[2];
    unsigned drops[2], repeats[2], damages[2], swaps[2];
    unsigned char seen[2][4096]; /* the SEQs each side has sent */
    unsigned resent[2];
    unsigned lost_bytes; /* writes to an etude-guest that had gone */
    uint64_t guest_next; /* one past etude-guest's highest SEQ seen */
    char gboot[ETUDE_FRAME_TOKEN_LEN + 1];
    char run_cmd[24];

    char out[2][KEEP + 1]; /* what the output function took */
    size_t outlen[2];
    int out_busy; /* each piece came while the run went on */
};

static struct harness H;
static char work[512];
static char stage[600];
static char alias_dir[700];

/* Processes to end at exit: etude-guests, and the groups of their attempts. */
static pid_t guests[32];
static int nguests;
static long groups[32];
static int ngroups;

/* The owners of attempts whose etude-guest was killed: each writes its proof
 * into the attempt's directory once it has reaped the command. */
static struct etude_pid keepers[8];
static int nkeepers;

/** Give the monotonic time in microseconds. */
static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/** Write the SHA-256 of `n` bytes as hexadecimal into `hex`. */
static void digest_hex(const void *p, size_t n, char hex[65])
{
    struct sha256 h;
    unsigned char d[32];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)p, n);
    sha256_final(&h, d);
    sha256_hex(d, hex);
}

/** End every etude-guest and attempt the test started, wait for the owners
 * of the attempts of killed etude-guests, and remove the scratch, on every
 * exit path. */
static void cleanup(void)
{
    int i;

    free(workload);
    workload = NULL;
    for (i = 0; i < nguests; i++)
        if (guests[i] > 0) {
            kill(guests[i], SIGKILL);
            waitpid(guests[i], NULL, 0);
        }
    for (i = 0; i < ngroups; i++)
        kill(-(pid_t)groups[i], SIGKILL);
    for (i = 0; i < nkeepers; i++) {
        struct etude_pid now;

        while (etude_process_state(keepers[i].pid, &now) == 1 && now.start == keepers[i].start) {
            /* interval: the attempt's owner is looked at again every 10 ms */
            poll(NULL, 0, 10);
        }
    }
    if (*alias_dir) {
        char path[720];
        snprintf(path, sizeof(path), "%s/w", alias_dir);
        if ((unlink(path) != 0 && errno != ENOENT) || rmdir(alias_dir) != 0)
            fprintf(stderr, "etude-serial-test: could not remove alias %s\n", alias_dir);
    }
    if (*work) {
        if (host_remove_tree(work) != 0)
            fprintf(stderr, "etude-serial-test: could not remove %s\n", work);
    }
}

/** Exit through the cleanup after a signal. */
static void stopped(int sig)
{
    exit(128 + sig);
}

/** Parse a copy of the `len` bytes of `line` into `f`; 0, or -1. */
static int parse(const char *line, size_t len, struct wire_frame *f, char copy[LINE_CAP])
{
    if (len >= LINE_CAP)
        return -1;
    memcpy(copy, line, len);
    copy[len] = '\0';
    return wire_parse(copy, len, f);
}

/** Note a frame on its way, `dir` 0 from the controller and 1 from
 * etude-guest: a SEQ seen before is a frame sent again. */
static void track(struct harness *h, int dir, const char *line, size_t len)
{
    char copy[LINE_CAP];
    struct wire_frame f;

    if (parse(line, len, &f, copy) != 0)
        return;
    if (dir == 0 && strcmp(f.type, ETUDE_CMD_HELLO) == 0 && f.seq == 0)
        memset(h->seen[0], 0, sizeof(h->seen[0]));
    if (dir == 1 && strcmp(f.type, ETUDE_EV_ALIVE) == 0) {
        memset(h->seen[1], 0, sizeof(h->seen[1]));
        h->guest_next = 0;
        snprintf(h->gboot, sizeof(h->gboot), "%s", f.boot);
        return;
    }
    if (dir == 0 && strcmp(f.type, ETUDE_CMD_RUN) == 0 && f.nfields == 3)
        snprintf(h->run_cmd, sizeof(h->run_cmd), "%s", f.field[0]);
    if (strcmp(f.type, ETUDE_CMD_ACK) == 0 || f.seq >= sizeof(h->seen[0]))
        return;
    if (h->seen[dir][f.seq])
        h->resent[dir]++;
    h->seen[dir][f.seq] = 1;
    if (dir == 1 && f.seq + 1 > h->guest_next)
        h->guest_next = f.seq + 1;
}

/** Deliver a frame: to etude-guest's input, or to the controller. */
static void emit(struct harness *h, int dir, const char *line, size_t len)
{
    char text[LINE_CAP + 1];

    memcpy(text, line, len);
    text[len] = '\n';
    if (dir == 1) {
        etude_serial_feed(h->s, text, len + 1, now_us());
        return;
    }
    /* A console stays open when etude-guest has gone; the bytes are lost. */
    if (h->to >= 0 && write(h->to, text, len + 1) != (ssize_t)(len + 1))
        h->lost_bytes++;
}

/**
 * Pass a frame through the relay. By its count on its way, a frame is
 * dropped, held until the next frame passes it, damaged, or sent twice;
 * the others pass unchanged.
 */
static void relay(struct harness *h, int dir, const char *line, size_t len)
{
    unsigned long k = ++h->count[dir];
    char bad[LINE_CAP];

    if (k % 5 == 1) {
        h->drops[dir]++;
        return;
    }
    if (h->held_len[dir] > 0) {
        emit(h, dir, line, len);
        emit(h, dir, h->held[dir], h->held_len[dir]);
        h->held_len[dir] = 0;
        h->swaps[dir]++;
        return;
    }
    if (k % 8 == 6) {
        memcpy(h->held[dir], line, len);
        h->held_len[dir] = len;
        return;
    }
    if (k % 6 == 4) {
        memcpy(bad, line, len);
        bad[len / 2] ^= 1;
        emit(h, dir, bad, len);
        h->damages[dir]++;
        return;
    }
    emit(h, dir, line, len);
    if (k % 7 == 3) {
        emit(h, dir, line, len);
        h->repeats[dir]++;
    }
}

/** Send on the frames the relay holds, as when nothing has followed them. */
static void flush_held(struct harness *h)
{
    int dir;

    for (dir = 0; dir < 2; dir++)
        if (h->held_len[dir] > 0) {
            size_t len = h->held_len[dir];
            h->held_len[dir] = 0;
            emit(h, dir, h->held[dir], len);
        }
}

/** Take a whole line from one side, `dir` as track gives it. */
static void take(struct harness *h, int dir, const char *line, size_t len)
{
    track(h, dir, line, len);
    if (dir == 0 && h->scripted) {
        if (h->nlines < LINES_MAX) {
            memcpy(h->lines[h->nlines], line, len);
            h->lines[h->nlines][len] = '\0';
            h->line_len[h->nlines++] = len;
        }
        return;
    }
    if (h->relay)
        relay(h, dir, line, len);
    else
        emit(h, dir, line, len);
}

/** The controller's write: the console's input, split into lines. */
static int console_write(void *ctx, const char *buf, size_t n)
{
    struct harness *h = ctx;
    size_t i;

    for (i = 0; i < n; i++) {
        if (buf[i] != '\n') {
            if (h->wlen < sizeof(h->wline) - 1)
                h->wline[h->wlen++] = buf[i];
            continue;
        }
        take(h, 0, h->wline, h->wlen);
        h->wlen = 0;
    }
    return 0;
}

/** The controller's output function: keep each stream's bytes in order. */
static void console_output(void *ctx, int fd, const char *buf, size_t n)
{
    struct harness *h = ctx;
    int i = fd - 1;

    if (etude_serial_status(h->s) != ETUDE_SERIAL_BUSY)
        h->out_busy = 0;
    if (h->outlen[i] + n > KEEP)
        n = KEEP - h->outlen[i];
    memcpy(h->out[i] + h->outlen[i], buf, n);
    h->outlen[i] += n;
    h->out[i][h->outlen[i]] = '\0';
}

/** Begin a harness for the run `run_id` whose controller keeps `keep` bytes
 * of each stream; `scripted` keeps the controller's frames for the test. */
static void begin(struct harness *h, const char *run_id, size_t keep, int scripted)
{
    struct etude_serial_conf conf;
    char hex[65];

    memset(h, 0, sizeof(*h));
    h->pid = -1;
    h->to = h->from = -1;
    h->scripted = scripted;
    h->out_busy = 1;
    digest_hex(run_id, strlen(run_id), hex);
    memcpy(h->session, hex, ETUDE_FRAME_TOKEN_LEN);
    memset(&conf, 0, sizeof(conf));
    conf.run_id = run_id;
    conf.resend_us = RESEND_US;
    conf.output_max = keep;
    conf.write = console_write;
    conf.output = console_output;
    conf.ctx = h;
    h->s = etude_serial_new(&conf);
    if (h->s == NULL) {
        fprintf(stderr, "etude-serial-test: out of memory\n");
        exit(2);
    }
}

/** Free the harness's controller. */
static void finish(struct harness *h)
{
    etude_serial_free(h->s);
    h->s = NULL;
}

/* ------------------------------------------------------- scripted frames */

/* The header of a scripted frame. */
struct hdr {
    const char *session, *boot;
    uint64_t seq;
    long long ack; /* below 0 for none */
};

/** Encode a frame of the scripted etude-guest into `out`, its fields a list
 * ending in NULL; the frame's length, or 0 when it does not encode. */
static size_t vframe(char *out, const struct hdr *k, const char *type, va_list ap)
{
    const char *field[WIRE_FIELDS_MAX] = {0};
    const char *a;
    int n = 0;

    while ((a = va_arg(ap, const char *)) != NULL && n < WIRE_FIELDS_MAX)
        field[n++] = a;
    return wire_encode(out, LINE_CAP, k->session, k->boot, k->seq, k->ack >= 0,
                       k->ack >= 0 ? (uint64_t)k->ack : 0, type, n, field);
}

/** Feed the controller at `t` the frame that vframe makes of `k`, `type`
 * and the fields in `ap`. */
static void vfeed(struct harness *h, uint64_t t, const struct hdr *k, const char *type, va_list ap)
{
    char text[LINE_CAP];
    size_t n = vframe(text, k, type, ap);

    if (n == 0) {
        printf("etude-serial-test: fail: a scripted %s frame does not encode\n", type);
        fails++;
        return;
    }
    etude_serial_feed(h->s, text, n, t);
}

/** Feed the controller a frame of the scripted etude-guest at `t`, its
 * fields a list ending in NULL. */
static void fake(struct harness *h, uint64_t t, const struct hdr *k, const char *type, ...)
{
    va_list ap;

    va_start(ap, type);
    vfeed(h, t, k, type, ap);
    va_end(ap);
}

/** Feed the scripted etude-guest's alive at `t`, of process `pid` begun at
 * `start`. */
static void alive(struct harness *h, uint64_t t, const char *pid, const char *start)
{
    const struct hdr k = {ETUDE_FRAME_NO_SESSION, FAKE_BOOT, 0, -1};

    fake(h, t, &k, ETUDE_EV_ALIVE, "1", pid, start, (char *)NULL);
}

/** Parse line `i` the controller wrote into `f`; 0, or -1. */
static int sent(struct harness *h, int i, struct wire_frame *f, char copy[LINE_CAP])
{
    if (i < 0 || i >= h->nlines)
        return -1;
    return parse(h->lines[i], h->line_len[i], f, copy);
}

/** Give the highest SEQ of a sequenced frame the controller wrote, or -1. */
static long long last_seq(struct harness *h)
{
    char copy[LINE_CAP];
    struct wire_frame f;
    long long top = -1;
    int i;

    for (i = 0; i < h->nlines; i++)
        if (sent(h, i, &f, copy) == 0 && strcmp(f.type, ETUDE_CMD_ACK) != 0 &&
            (long long)f.seq > top)
            top = (long long)f.seq;
    return top;
}

/** Feed an event of the scripted etude-guest's session under its next SEQ,
 * acknowledging every frame the controller wrote. */
static void event(struct harness *h, uint64_t t, const char *type, ...)
{
    struct hdr k;
    va_list ap;

    k.session = h->session;
    k.boot = FAKE_BOOT;
    k.seq = h->gseq++;
    k.ack = last_seq(h);
    va_start(ap, type);
    vfeed(h, t, &k, type, ap);
    va_end(ap);
}

/** Give the index of the last line the controller wrote of `type`, or -1. */
static int last_of(struct harness *h, const char *type, struct wire_frame *f, char copy[LINE_CAP])
{
    int i;

    for (i = h->nlines - 1; i >= 0; i--)
        if (sent(h, i, f, copy) == 0 && strcmp(f->type, type) == 0)
            return i;
    return -1;
}

/** Open a scripted session: alive, the controller's hello, and ready. */
static void scripted_open(struct harness *h, const char *run_id, size_t keep)
{
    char copy[LINE_CAP];
    struct wire_frame f;

    begin(h, run_id, keep, 1);
    alive(h, 1000, "4242", "77");
    CHECK(etude_serial_open(h->s, 1000) == 0 && last_of(h, ETUDE_CMD_HELLO, &f, copy) >= 0,
          "an alive seen before the open is answered by a hello at once");
    h->gseq = 0;
    if (last_of(h, ETUDE_CMD_HELLO, &f, copy) >= 0)
        event(h, 1000, ETUDE_EV_READY, f.field[0], "1", "255", "4242", "77", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE, "ready opens the session");
}

/** Write the base64 of the text `p` into `out`. */
static const char *b64(const char *p, char *out)
{
    wire_base64((const unsigned char *)p, strlen(p), out);
    return out;
}

/** Feed the scripted `stream` of descriptor `fd` of attempt `att` under
 * command `cmd`: the text `sent`, by its size and digest, of a stream of
 * `total` bytes. */
static void fake_stream(struct harness *h, const char *cmd, const char *att, int fd,
                        const char *sent, size_t total)
{
    char size[24], all[24], sha[65];

    snprintf(size, sizeof(size), "%zu", strlen(sent));
    snprintf(all, sizeof(all), "%zu", total);
    digest_hex(sent, strlen(sent), sha);
    event(h, 5000, ETUDE_EV_STREAM, cmd, att, fd == 1 ? "1" : "2", size, all, sha, (char *)NULL);
}

/** Feed the scripted end of attempt `att` under command `cmd` up to its
 * usage: exited 0, the streams `out` and `err` whole, and no measures. */
static void fake_end(struct harness *h, const char *cmd, const char *att, const char *out,
                     const char *err)
{
    event(h, 5000, ETUDE_EV_EXIT, cmd, att, "exited", "0", (char *)NULL);
    fake_stream(h, cmd, att, 1, out, strlen(out));
    fake_stream(h, cmd, att, 2, err, strlen(err));
    event(h, 5000, ETUDE_EV_USAGE, cmd, att, "-", "-", "-", "-", (char *)NULL);
}

/** Begin a scripted run of attempt `att`, and give its command in `cmd`. */
static void fake_run(struct harness *h, const char *att, char cmd[24])
{
    static const char inv[] = "00000000000000000000000000000000"
                              "00000000000000000000000000000000";
    char copy[LINE_CAP];
    struct wire_frame f;

    memset(h->out, 0, sizeof(h->out));
    h->outlen[0] = h->outlen[1] = 0;
    cmd[0] = '\0';
    if (etude_serial_run(h->s, att, inv, 5000) == 0 && last_of(h, ETUDE_CMD_RUN, &f, copy) >= 0)
        snprintf(cmd, 24, "%s", f.field[0]);
}

/** The open of a scripted session, the resending of a frame, and the
 * acknowledgements. */
static void check_open(struct harness *h)
{
    char copy[LINE_CAP];
    struct wire_frame f;
    char hello[24];

    CHECK(etude_serial_open(h->s, 1000) == 0 && etude_serial_status(h->s) == ETUDE_SERIAL_BUSY &&
              h->nlines == 0 && etude_serial_deadline(h->s) == 0,
          "open waits for etude-guest's alive");
    CHECK(etude_serial_open(h->s, 1000) == -1 && errno == EBUSY,
          "an operation waits for the one going on");
    alive(h, 1000, "4242", "77");
    CHECK(h->nlines == 1 && sent(h, 0, &f, copy) == 0 && strcmp(f.type, ETUDE_CMD_HELLO) == 0 &&
              f.seq == 0 && !f.has_ack && f.nfields == 3 && strcmp(f.field[1], "1") == 0 &&
              strcmp(f.field[2], "255") == 0 && strcmp(f.boot, FAKE_BOOT) == 0 &&
              strcmp(f.session, h->session) == 0,
          "an alive is answered by a hello under SEQ 0 for frames of 255 bytes");
    snprintf(hello, sizeof(hello), "%s", h->nlines == 1 ? f.field[0] : "1");
    CHECK(etude_serial_deadline(h->s) == 1000 + RESEND_US,
          "a frame is due again a resend interval after it was sent");
    etude_serial_tick(h->s, 1000 + RESEND_US - 1);
    CHECK(h->nlines == 1, "a frame is not sent again before the resend interval");
    etude_serial_tick(h->s, 1000 + RESEND_US);
    CHECK(h->nlines == 2 && strcmp(h->lines[0], h->lines[1]) == 0,
          "an unacknowledged frame is sent again with the same SEQ and bytes");
    CHECK(etude_serial_deadline(h->s) == 1000 + 2 * RESEND_US,
          "a frame is due again a resend interval after it was sent again");

    h->gseq = 0;
    event(h, 2000, ETUDE_EV_READY, hello, "1", "255", "4242", "77", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE && etude_serial_why(h->s)[0] == '\0',
          "ready opens the session");
    CHECK(h->nlines == 3 && sent(h, 2, &f, copy) == 0 && strcmp(f.type, ETUDE_CMD_ACK) == 0 &&
              f.has_ack && f.ack == 0 && f.seq == 1,
          "every sequenced frame is acknowledged");
    CHECK(etude_serial_deadline(h->s) == 0, "an acknowledged frame is not sent again");
    etude_serial_tick(h->s, 1000 + 9 * RESEND_US);
    CHECK(h->nlines == 3, "an acknowledged frame is not sent again");
}

/** A repeated ready, and the open and alive that follow an open session. */
static void check_reopen(struct harness *h)
{
    char copy[LINE_CAP];
    struct wire_frame f;

    h->gseq = 0;
    if (last_of(h, ETUDE_CMD_HELLO, &f, copy) >= 0)
        event(h, 2000, ETUDE_EV_READY, f.field[0], "1", "255", "4242", "77", (char *)NULL);
    CHECK(h->nlines == 4 && sent(h, 3, &f, copy) == 0 && strcmp(f.type, ETUDE_CMD_ACK) == 0 &&
              f.ack == 0 && etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "a repeated frame is acknowledged with the last frame in order");
    CHECK(etude_serial_open(h->s, 2000) == -1 && errno == EISCONN,
          "an open session is not opened again");
    alive(h, 2000, "4242", "77");
    CHECK(h->nlines == 4 && etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "a late copy of the alive changes nothing");
    h->gseq = 1;
}

/** A staged put, and the frames that must not end it. */
static void check_staged_put(struct harness *h)
{
    static const char text[] = "staged\n";
    char copy[LINE_CAP], copy2[LINE_CAP], sha[65], frame[LINE_CAP];
    struct wire_frame f, g;
    int first = h->nlines, i, data = 0;
    long long seal = -1;
    size_t n;

    digest_hex(text, strlen(text), sha);
    CHECK(etude_serial_put(h->s, "staged.txt", strlen(text), sha, "0644", NULL, 3000) == 0,
          "a staged put begins");
    for (i = first; i < h->nlines; i++)
        if (sent(h, i, &f, copy) == 0) {
            data += strcmp(f.type, ETUDE_CMD_DATA) == 0;
            if (strcmp(f.type, ETUDE_CMD_SEAL) == 0)
                seal = (long long)f.seq;
        }
    CHECK(h->nlines == first + 2 && sent(h, first, &f, copy) == 0 &&
              strcmp(f.type, ETUDE_CMD_PUT) == 0 && f.nfields == 5 &&
              strcmp(f.field[1], "staged.txt") == 0 && strcmp(f.field[2], "7") == 0 &&
              strcmp(f.field[3], sha) == 0 && strcmp(f.field[4], "0644") == 0 &&
              sent(h, first + 1, &g, copy2) == 0 && strcmp(g.type, ETUDE_CMD_SEAL) == 0 &&
              strcmp(g.field[0], f.field[0]) == 0 && data == 0,
          "a staged put sends put and seal alone");
    n = wire_encode(frame, sizeof(frame), h->session, FAKE_BOOT, h->gseq, 1, (uint64_t)seal,
                    ETUDE_EV_STORED, 3, (const char *const[]){f.field[0], "staged.txt", sha});
    first = h->nlines;
    frame[n / 2] ^= 1;
    etude_serial_feed(h->s, frame, n, 3000);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY && h->nlines == first,
          "a damaged frame is not acted on");
    fake(h, 3000, &(struct hdr){OTHER, FAKE_BOOT, h->gseq, seal}, ETUDE_EV_STORED, f.field[0],
         "staged.txt", sha, (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "a frame of another session is not acted on");
    fake(h, 3000, &(struct hdr){h->session, OTHER, h->gseq, seal}, ETUDE_EV_STORED, f.field[0],
         "staged.txt", sha, (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "a frame of another boot is not acted on");
    fake(h, 3000, &(struct hdr){h->session, FAKE_BOOT, h->gseq, 99}, ETUDE_EV_STORED, f.field[0],
         "staged.txt", sha, (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "a frame that acknowledges what was not sent is not acted on");
    event(h, 3000, ETUDE_EV_STORED, f.field[0], "staged.txt", sha, (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE && etude_serial_deadline(h->s) == 0,
          "stored with the put's digest completes the put");
}

/**
 * Say whether the frames the controller wrote from line `first` all fit the
 * serial bound and whose data frames carry `data` whole, in order, under the
 * put's command, which goes to `put_cmd`.
 */
static int data_frames(struct harness *h, int first, const unsigned char *data, size_t n,
                       char put_cmd[24])
{
    static unsigned char back[2000];
    char copy[LINE_CAP];
    struct wire_frame f;
    uint64_t off = 0;
    long got;
    int i, ok = 1;

    for (i = first; i < h->nlines; i++) {
        if (sent(h, i, &f, copy) != 0 || h->line_len[i] + 1 > ETUDE_FRAME_SERIAL_MAX) {
            ok = 0;
            continue;
        }
        if (strcmp(f.type, ETUDE_CMD_PUT) == 0)
            snprintf(put_cmd, 24, "%s", f.field[0]);
        if (strcmp(f.type, ETUDE_CMD_DATA) != 0)
            continue;
        got = wire_unbase64(f.field[2], back + off);
        if (got <= 0 || strtoull(f.field[1], NULL, 10) != off || strcmp(f.field[0], put_cmd) != 0)
            ok = 0;
        else
            off += (uint64_t)got;
    }
    return ok && off == n && memcmp(back, data, n) == 0;
}

/** A put with data, in frames of the serial bound, four in flight. */
static void check_data_put(struct harness *h)
{
    static unsigned char data[1000];
    char copy[LINE_CAP], sha[65], put_cmd[24] = "";
    struct wire_frame f;
    int first = h->nlines, i, inflight = 0, rounds, sealed = 0;

    for (i = 0; i < (int)sizeof(data); i++)
        data[i] = (unsigned char)(i * 7 % 251);
    digest_hex(data, sizeof(data), sha);
    CHECK(etude_serial_put(h->s, "data.bin", sizeof(data), sha, "0644", data, 4000) == 0,
          "a put with data begins");
    for (i = first; i < h->nlines; i++)
        if (sent(h, i, &f, copy) == 0 && strcmp(f.type, ETUDE_CMD_ACK) != 0)
            inflight++;
    CHECK(inflight == 4, "no more than four frames are in flight");
    for (rounds = 0; rounds < 100 && !sealed; rounds++) {
        fake(h, 4000, &(struct hdr){h->session, FAKE_BOOT, h->gseq, last_seq(h)}, ETUDE_CMD_ACK,
             (char *)NULL);
        sealed = last_of(h, ETUDE_CMD_SEAL, &f, copy) >= first;
    }
    CHECK(sealed && data_frames(h, first, data, sizeof(data), put_cmd),
          "a put with data sends data frames that fit the serial bound");
    event(h, 4000, ETUDE_EV_STORED, put_cmd, "data.bin",
          "1111111111111111111111111111111111111111111111111111111111111111", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DAMAGED &&
              strstr(etude_serial_why(h->s), "digest") != NULL,
          "a stored with another digest is damaged");
}

/** The controller's session, its open, the resending of a frame, the
 * acknowledgements, and its puts. */
static void check_scripted_session(void)
{
    struct harness *h = &H;

    begin(h, "etude-serial-test scripted", KEEP, 1);
    check_open(h);
    check_reopen(h);
    check_staged_put(h);
    check_data_put(h);
    finish(h);
}

/** A run's started and output: a frame after kernel text, a repeat and a
 * gap, and output given once and in order. The run's command goes to `cmd`. */
static void check_run_output(struct harness *h, char cmd[24])
{
    const struct etude_serial_result *r = etude_serial_result(h->s);
    char copy[LINE_CAP], b1[64], b2[64], b3[64];
    char text[LINE_CAP], line[LINE_CAP + 64];
    struct wire_frame f;
    size_t i, n;

    fake_run(h, "00000000000000a1", cmd);
    n = wire_encode(text, sizeof(text), h->session, FAKE_BOOT, h->gseq++, 1, (uint64_t)last_seq(h),
                    ETUDE_EV_STARTED, 4,
                    (const char *const[]){cmd, "00000000000000a1", "321", "654"});
    snprintf(line, sizeof(line), "rach2: kernel text before a frame %.*s", (int)n, text);
    for (i = 0; i < strlen(line); i++)
        etude_serial_feed(h->s, line + i, 1, 5000);
    CHECK(r->started && r->process.pid == 321 && r->process.start == 654,
          "a frame after kernel text, fed a byte at a time, is acted on");
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000a1", "1", "0", b64("hello ", b1),
          (char *)NULL);
    h->gseq--;
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000a1", "1", "0", b64("hello ", b1),
          (char *)NULL);
    CHECK(strcmp(h->out[0], "hello ") == 0 && etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "a repeated frame is not acted on again");
    h->gseq++;
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000a1", "1", "6", b64("wor", b2), (char *)NULL);
    CHECK(strcmp(h->out[0], "hello ") == 0, "a frame past a gap is not acted on");
    CHECK(last_of(h, ETUDE_CMD_ACK, &f, copy) == h->nlines - 1 && f.has_ack && f.ack == h->gseq - 3,
          "a frame past a gap is acknowledged with the last frame in order");
    h->gseq -= 2;
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000a1", "1", "6", b64("world\n", b2),
          (char *)NULL);
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000a1", "2", "0", b64("e\n", b3), (char *)NULL);
    CHECK(strcmp(h->out[0], "hello world\n") == 0 && strcmp(h->out[1], "e\n") == 0 && h->out_busy,
          "output reaches the caller once and in order while the run goes on");
}

/** The whole end of the run of `cmd` that check_run_output began, its
 * standard output's stream longer than what etude-guest sent. */
static void check_run_end(struct harness *h, const char *cmd)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);

    event(h, 5000, ETUDE_EV_EXIT, cmd, "00000000000000a1", "exited", "0", (char *)NULL);
    fake_stream(h, cmd, "00000000000000a1", 1, "hello world\n", 20);
    fake_stream(h, cmd, "00000000000000a1", 2, "e\n", 2);
    event(h, 5000, ETUDE_EV_USAGE, cmd, "00000000000000a1", "1234+", "cpu-method", "4096+",
          "peak-method", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY, "a run goes on until cleaned");
    event(h, 5000, ETUDE_EV_CLEANED, cmd, "00000000000000a1", "confirmed", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE && r->complete &&
              r->end == ETUDE_END_EXITED && r->value == 0 &&
              r->cleanup == ETUDE_CLEANUP_CONFIRMED && r->cpu_us.value == 1234 &&
              r->cpu_us.quality == ETUDE_QUALITY_LOWER_BOUND &&
              strcmp(r->cpu_us.method, "cpu-method") == 0 && r->peak_bytes.value == 4096,
          "a whole end completes the run");
    CHECK(r->out.kept == 8 && strcmp(r->out.data, "hello wo") == 0 && r->out.total == 20 &&
              r->err.kept == 2 && r->err.total == 2,
          "output is kept to the bound, and its total is its stream's");
}

/** Runs whose streams disagree with their output, or are not known. */
static void check_run_streams(struct harness *h)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);
    char cmd[24], b1[64], sha[65];

    fake_run(h, "00000000000000a2", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000a2", "1", "2", (char *)NULL);
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000a2", "1", "0", b64("abc", b1), (char *)NULL);
    event(h, 5000, ETUDE_EV_EXIT, cmd, "00000000000000a2", "exited", "0", (char *)NULL);
    digest_hex("abd", 3, sha);
    event(h, 5000, ETUDE_EV_STREAM, cmd, "00000000000000a2", "1", "3", "3", sha, (char *)NULL);
    fake_stream(h, cmd, "00000000000000a2", 2, "", 0);
    event(h, 5000, ETUDE_EV_USAGE, cmd, "00000000000000a2", "-", "-", "-", "-", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY, "a damaged run goes on until cleaned");
    event(h, 5000, ETUDE_EV_CLEANED, cmd, "00000000000000a2", "confirmed", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DAMAGED && !r->complete &&
              r->end == ETUDE_END_EXITED,
          "output that disagrees with its stream's digest is damaged");

    fake_run(h, "00000000000000a3", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000a3", "1", "2", (char *)NULL);
    event(h, 5000, ETUDE_EV_EXIT, cmd, "00000000000000a3", "exited", "0", (char *)NULL);
    event(h, 5000, ETUDE_EV_STREAM, cmd, "00000000000000a3", "1", "-", "-", "-", (char *)NULL);
    event(h, 5000, ETUDE_EV_STREAM, cmd, "00000000000000a3", "2", "-", "-", "-", (char *)NULL);
    event(h, 5000, ETUDE_EV_USAGE, cmd, "00000000000000a3", "-", "-", "-", "-", (char *)NULL);
    event(h, 5000, ETUDE_EV_CLEANED, cmd, "00000000000000a3", "confirmed", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE && !r->complete &&
              r->end == ETUDE_END_EXITED && r->value == 0,
          "a run whose streams' sizes are not known ends DONE and not complete");
}

/** Feed the `exit` of attempt `att` under command `cmd` in its old form,
 * with ten fields, under the scripted etude-guest's next SEQ, which it
 * leaves for the next frame. */
static void old_exit(struct harness *h, const char *cmd, const char *att)
{
    char text[LINE_CAP], empty[65];
    size_t n;

    digest_hex("", 0, empty);
    n = wire_encode(
        text, sizeof(text), h->session, FAKE_BOOT, h->gseq, 1, (uint64_t)last_seq(h), ETUDE_EV_EXIT,
        10, (const char *const[]){cmd, att, "exited", "0", "0", "0", empty, "0", "0", empty});
    etude_serial_feed(h->s, text, n, 5000);
}

/** Events of the wrong command, attempt or form, and events out of order. */
static void check_run_protocol(struct harness *h)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);
    char cmd[24], b1[64];

    fake_run(h, "00000000000000b1", cmd);
    event(h, 5000, ETUDE_EV_STARTED, "999", "00000000000000b1", "1", "2", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL && !r->complete &&
              r->end == ETUDE_END_LOST,
          "an event of a command never sent ends the run PROTOCOL");
    fake_run(h, "00000000000000b2", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000c2", "1", "2", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL,
          "an event of another attempt ends the run PROTOCOL");
    fake_run(h, "00000000000000b3", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000b3", "1", "2", (char *)NULL);
    old_exit(h, cmd, "00000000000000b3");
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL && r->end == ETUDE_END_LOST,
          "an exit of ten fields, the old form, ends the run PROTOCOL");
    fake_run(h, "00000000000000b4", cmd);
    event(h, 5000, ETUDE_EV_OUT, cmd, "00000000000000b4", "1", "0", b64("abc", b1), (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL && h->outlen[0] == 0,
          "output before started ends the run PROTOCOL");
    fake_run(h, "00000000000000b5", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000b5", "1", "2", (char *)NULL);
    event(h, 5000, ETUDE_EV_USAGE, cmd, "00000000000000b5", "-", "-", "-", "-", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL,
          "usage before exit ends the run PROTOCOL");
    fake_run(h, "00000000000000b6", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000b6", "1", "2", (char *)NULL);
    event(h, 5000, ETUDE_EV_EXIT, cmd, "00000000000000b6", "exited", "0", (char *)NULL);
    fake_stream(h, cmd, "00000000000000b6", 2, "", 0);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL,
          "the stream of descriptor 2 before that of 1 ends the run PROTOCOL");
    fake_run(h, "00000000000000b7", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000b7", "1", "2", (char *)NULL);
    event(h, 5000, ETUDE_EV_EXIT, cmd, "00000000000000b7", "exited", "0", (char *)NULL);
    fake_stream(h, cmd, "00000000000000b7", 1, "", 0);
    event(h, 5000, ETUDE_EV_USAGE, cmd, "00000000000000b7", "-", "-", "-", "-", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_PROTOCOL,
          "usage before the stream of descriptor 2 ends the run PROTOCOL");
}

/** An event of an earlier operation, and an error. */
static void check_run_refused(struct harness *h)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);
    char cmd[24], old[24], line[64];

    fake_run(h, "00000000000000b8", cmd);
    snprintf(old, sizeof(old), "%s", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000b8", "1", "2", (char *)NULL);
    fake_end(h, cmd, "00000000000000b8", "", "");
    event(h, 5000, ETUDE_EV_CLEANED, cmd, "00000000000000b8", "confirmed", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE && r->complete,
          "a run of empty streams completes");
    fake_run(h, "00000000000000b9", cmd);
    event(h, 5000, ETUDE_EV_CLEANED, old, "00000000000000b8", "confirmed", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "an event of an earlier operation is passed by");
    event(h, 5000, ETUDE_EV_ERROR, cmd, ETUDE_ERR_LIMIT, (char *)NULL);
    snprintf(line, sizeof(line), "error %s limit", cmd);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_REFUSED &&
              strcmp(etude_serial_why(h->s), line) == 0 && r->end == ETUDE_END_LOST,
          "an error ends the run REFUSED with its code");
}

/** A cancel, and a console closed before the run's cleaned. */
static void check_run_closed(struct harness *h)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);
    char copy[LINE_CAP], cmd[24];
    struct wire_frame f;

    fake_run(h, "00000000000000a8", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000a8", "1", "2", (char *)NULL);
    CHECK(etude_serial_cancel(h->s, 5000) == 0 && last_of(h, ETUDE_CMD_CANCEL, &f, copy) >= 0 &&
              f.nfields == 2 && strcmp(f.field[1], "00000000000000a8") == 0 &&
              etude_serial_cancel(h->s, 5000) == 0,
          "cancel is sent for the attempt of the run");
    fake_end(h, cmd, "00000000000000a8", "", "");
    etude_serial_closed(h->s);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_LOST && r->end == ETUDE_END_LOST &&
              r->value == -1 && !r->complete && r->cleanup == ETUDE_CLEANUP_UNCONFIRMED,
          "a closed console leaves the run's end lost");
    event(h, 5000, ETUDE_EV_CLEANED, cmd, "00000000000000a8", "confirmed", (char *)NULL);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_LOST && r->end == ETUDE_END_LOST &&
              etude_serial_deadline(h->s) == 0,
          "a closed console leaves the run's end lost");
    CHECK(etude_serial_bye(h->s, 5000) == -1 && errno == EPIPE &&
              etude_serial_open(h->s, 5000) == -1 && errno == EPIPE,
          "nothing begins on a closed console");
}

/** A scripted run: its events in order, output kept to the bound, and the
 * events that end it otherwise. */
static void check_scripted_run(void)
{
    struct harness *h = &H;
    char cmd[24];

    scripted_open(h, "etude-serial-test scripted run", 8);
    check_run_output(h, cmd);
    check_run_end(h, cmd);
    check_run_streams(h);
    check_run_protocol(h);
    check_run_refused(h);
    check_run_closed(h);
    finish(h);
}

/* End a scripted ordinary or service attempt with its complete records. */
static void service_end(struct harness *h, const char *cmd, const char *token, const char *kind,
                        const char *value, const char *text)
{
    event(h, 5000, ETUDE_EV_EXIT, cmd, token, kind, value, (char *)NULL);
    fake_stream(h, cmd, token, 1, text, strlen(text));
    fake_stream(h, cmd, token, 2, "", 0);
    event(h, 5000, ETUDE_EV_USAGE, cmd, token, "12", "service-cpu", "4096", "service-peak",
          (char *)NULL);
    event(h, 5000, ETUDE_EV_CLEANED, cmd, token, "confirmed", (char *)NULL);
}

/* Service events share wire ordering, but never another operation's result. */
static void check_scripted_service(void)
{
    struct harness *h = &H;
    const struct etude_serial_result *r;
    const char *service = "00000000000000b1", *ordinary = "00000000000000b2";
    char first[24], next[24], text[64], copy[LINE_CAP];
    struct wire_frame f;

    scripted_open(h, "etude-serial-test services", KEEP);
    CHECK(etude_serial_background(h->s) < 0, "a service must be a started run");
    fake_run(h, service, first);
    event(h, 5000, ETUDE_EV_STARTED, first, service, "321", "654", (char *)NULL);
    CHECK(etude_serial_background(h->s) == 0 && etude_serial_background_running(h->s),
          "a started run remains owned as a service");
    CHECK(etude_serial_background(h->s) < 0 && etude_serial_bye(h->s, 5000) < 0,
          "a second service and bye before its finish are refused");
    check_staged_put(h);
    fake_run(h, ordinary, next);
    event(h, 5000, ETUDE_EV_STARTED, next, ordinary, "322", "655", (char *)NULL);
    event(h, 5000, ETUDE_EV_OUT, first, service, "1", "0", b64("service", text), (char *)NULL);
    CHECK(etude_serial_result(h->s)->out.kept == 0 && etude_serial_background_running(h->s),
          "service output is not ordinary command output");
    CHECK(etude_serial_foreground(h->s) < 0, "a busy ordinary operation cannot be replaced");
    service_end(h, first, service, "exited", "0", "service");
    CHECK(!etude_serial_background_running(h->s) && etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "service completion does not complete the ordinary command");
    service_end(h, next, ordinary, "exited", "7", "");
    CHECK(etude_serial_result(h->s)->value == 7 && etude_serial_result(h->s)->complete,
          "the ordinary command keeps its own complete result");
    CHECK(etude_serial_foreground(h->s) == 0 && !etude_serial_has_background(h->s),
          "a completed service returns through normal result processing");
    r = etude_serial_result(h->s);
    CHECK(r->complete && r->value == 0 && r->cleanup == ETUDE_CLEANUP_CONFIRMED &&
              !strcmp(r->out.data, "service") && !strcmp(r->cpu_us.method, "service-cpu"),
          "service output, cleanup and measurement strings survive later operations");
    fake_run(h, service, first);
    event(h, 5000, ETUDE_EV_STARTED, first, service, "323", "656", (char *)NULL);
    CHECK(etude_serial_background(h->s) == 0 && etude_serial_foreground(h->s) == 0 &&
              etude_serial_cancel(h->s, 5000) == 0 && last_of(h, ETUDE_CMD_CANCEL, &f, copy) >= 0 &&
              !strcmp(f.field[1], service),
          "service cancellation addresses its original attempt");
    service_end(h, first, service, "stopped", "15", "");
    CHECK(etude_serial_result(h->s)->complete &&
              etude_serial_result(h->s)->cleanup == ETUDE_CLEANUP_CONFIRMED,
          "a cancelled service still requires its complete cleanup records");
    fake_run(h, service, first);
    event(h, 5000, ETUDE_EV_STARTED, first, service, "324", "657", (char *)NULL);
    CHECK(etude_serial_background(h->s) == 0, "a new service starts after prior cleanup");
    etude_serial_closed(h->s);
    CHECK(!etude_serial_background_running(h->s) && etude_serial_foreground(h->s) == 0 &&
              etude_serial_result(h->s)->end == ETUDE_END_LOST &&
              etude_serial_result(h->s)->cleanup == ETUDE_CLEANUP_UNCONFIRMED,
          "console loss cannot confirm a background service cleanup");
    finish(h);
}

/** A scripted etude-guest that starts again during a run, and the session
 * opened with it. */
static void check_scripted_restart(void)
{
    struct harness *h = &H;
    const struct etude_serial_result *r;
    char copy[LINE_CAP], cmd[24], boot[ETUDE_FRAME_TOKEN_LEN + 1];
    struct wire_frame f;
    struct etude_pid pid;
    int before;

    scripted_open(h, "etude-serial-test scripted restart", KEEP);
    r = etude_serial_result(h->s);
    fake_run(h, "00000000000000c1", cmd);
    event(h, 5000, ETUDE_EV_STARTED, cmd, "00000000000000c1", "1", "2", (char *)NULL);
    fake_end(h, cmd, "00000000000000c1", "", "");
    alive(h, 5000, "4243", "78");
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_LOST && r->end == ETUDE_END_LOST &&
              !r->complete && strstr(etude_serial_why(h->s), "started again") != NULL &&
              etude_serial_guest(h->s, &pid, boot) == 0 && pid.pid == 4243,
          "an etude-guest started again loses the run");
    before = h->nlines;
    CHECK(etude_serial_open(h->s, 6000) == 0 && h->nlines == before + 1 &&
              last_of(h, ETUDE_CMD_HELLO, &f, copy) == before && f.seq == 0,
          "a session opens with the etude-guest that started again");
    finish(h);
}

/* ------------------------------------------------------------ etude-guest */

/** Both startup forms fit the debug shell at the largest interval. */
static void check_guest_line(void)
{
    char line[65];
    const uint64_t max = UINT64_C(9007199254740991);
    CHECK(etude_serial_guest_line(line, sizeof(line), 1, 1, max) == 0 &&
              !strcmp(line, "exec /etude-guest -s /dev/shm/etude-guest -t / -r 9007199254741\n") &&
              strlen(line) == 64 && line[63] == '\n',
          "the maximum schema interval fits exactly 63 shell characters before LF");
    CHECK(etude_serial_guest_line(line, sizeof(line), 0, 1, max) == 0 &&
              !strcmp(line, "/etude-guest -s /dev/shm/etude-guest -t / -r 9007199254741\n"),
          "explicit guest open preserves state, stage and interval");
    CHECK(etude_serial_guest_line(line, sizeof(line), 1, 0, 1001) == 0 &&
              !strcmp(line, "exec /etude-guest -t / -r 2\n"),
          "a submillisecond remainder rounds upward");
    CHECK(etude_serial_guest_line(line, 64, 1, 1, max) < 0 && !line[0] &&
              etude_serial_guest_line(line, sizeof(line), 1, 1, UINT64_MAX) < 0 && !line[0] &&
              etude_serial_guest_line(line, sizeof(line), 1, 1, 0) < 0 && !line[0],
          "a short buffer, overflowing interval or zero interval sends no partial line");
}

/** Start etude-guest on pipes, with the state directory `state`. */
static void start_guest(struct harness *h, const char *state)
{
    int in[2], out[2];
    char *argv[10];
    int n = 0;
    int compact = strstr(state, "state-guest") != NULL;
    pid_t pid;

    snprintf(h->state, sizeof(h->state), "%s", state);
    if (pipe(in) < 0 || pipe(out) < 0) {
        perror("etude-serial-test: pipe");
        exit(2);
    }
    argv[n++] = "etude-guest";
    argv[n++] = compact ? "-s" : "--state";
    argv[n++] = h->state;
    argv[n++] = compact ? "-t" : "--stage";
    argv[n++] = stage;
    argv[n++] = compact ? "-r" : "--resend-ms";
    argv[n++] = "50";
    argv[n] = NULL;
    pid = fork();
    if (pid < 0) {
        perror("etude-serial-test: fork");
        exit(2);
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        _exit(etude_guest_main(n, argv));
    }
    close(in[0]);
    close(out[1]);
    h->pid = pid;
    h->to = in[1];
    h->from = out[0];
    h->rlen = 0;
    if (nguests < (int)(sizeof(guests) / sizeof(guests[0])))
        guests[nguests++] = pid;
}

/** Forget a guest the test has reaped. */
static void reaped(pid_t pid)
{
    int i;

    for (i = 0; i < nguests; i++)
        if (guests[i] == pid)
            guests[i] = -1;
}

/** End etude-guest with SIGKILL and close its pipes. */
static void end_guest(struct harness *h)
{
    if (h->to >= 0)
        close(h->to);
    if (h->from >= 0)
        close(h->from);
    h->to = h->from = -1;
    if (h->pid > 0) {
        kill(h->pid, SIGKILL);
        waitpid(h->pid, NULL, 0);
        reaped(h->pid);
    }
    h->pid = -1;
}

/** Take what etude-guest wrote, a line at a time. */
static void from_guest(struct harness *h, const char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (buf[i] != '\n') {
            if (h->rlen < sizeof(h->rline) - 1)
                h->rline[h->rlen++] = buf[i];
            continue;
        }
        if (h->rlen < LINE_CAP)
            take(h, 1, h->rline, h->rlen);
        h->rlen = 0;
    }
}

/* What a wait goes on until, besides the end of the operation. */
enum serve_until { UNTIL_END, UNTIL_STARTED, UNTIL_READY };

/** Say whether the wait is over. */
static int satisfied(struct harness *h, enum serve_until w)
{
    if (etude_serial_status(h->s) != ETUDE_SERIAL_BUSY)
        return 1;
    if (w == UNTIL_STARTED)
        return etude_serial_result(h->s)->started;
    if (w == UNTIL_READY)
        return strstr(h->out[0], "ready\n") != NULL;
    return 0;
}

/**
 * Carry the console both ways until the wait is over, sending again what the
 * controller has due; 1, or 0 when etude-guest's output has ended with
 * nothing due or held.
 */
static int serve(struct harness *h, enum serve_until w)
{
    char buf[4096];

    for (;;) {
        uint64_t t = now_us(), wake = UINT64_MAX, due;
        struct pollfd p;
        int timeout, r;
        ssize_t n;

        if (satisfied(h, w))
            return 1;
        due = etude_serial_deadline(h->s);
        if (due != 0)
            wake = due;
        if ((h->held_len[0] > 0 || h->held_len[1] > 0) && t + HELD_US < wake)
            wake = t + HELD_US;
        if (wake == UINT64_MAX && h->from < 0)
            return 0;
        timeout = wake == UINT64_MAX ? -1 : wake > t ? (int)((wake - t + 999) / 1000) : 0;
        p.fd = h->from;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, timeout);
        if (r > 0) {
            n = read(h->from, buf, sizeof(buf));
            if (n > 0) {
                from_guest(h, buf, (size_t)n);
            } else {
                close(h->from);
                h->from = -1;
            }
        } else {
            flush_held(h);
        }
        etude_serial_tick(h->s, now_us());
    }
}

/** Read etude-guest's output until it closes it, feeding the controller;
 * 1, or 0 when the wait for it fails. */
static int drain(struct harness *h)
{
    char buf[4096];
    struct pollfd p;
    ssize_t n;
    int r;

    while (h->from >= 0) {
        p.fd = h->from;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, -1);
        if (r < 0 && errno != EINTR)
            return 0;
        if (r <= 0)
            continue;
        n = read(h->from, buf, sizeof(buf));
        if (n > 0) {
            from_guest(h, buf, (size_t)n);
            continue;
        }
        close(h->from);
        h->from = -1;
    }
    return h->from < 0;
}

/** Store `n` bytes as `name` and wait for the put's end; its status, or
 * IDLE when the put could not begin. With `staged` no data is sent. */
static enum etude_serial_status store(struct harness *h, const char *name, const char *text,
                                      size_t n, int staged, char sha[65])
{
    digest_hex(text, n, sha);
    if (etude_serial_put(h->s, name, n, sha, "0755", staged ? NULL : text, now_us()) != 0)
        return ETUDE_SERIAL_IDLE;
    serve(h, UNTIL_END);
    return etude_serial_status(h->s);
}

/** Store program `name` and an invocation of it for attempt `token` with
 * the run `run_id`; the invocation's digest goes to `inv`. 0, or -1. */
static int prepare(struct harness *h, const char *run_id, const char *name, const char *role,
                   const char *token, char inv[65])
{
    char text[4096], sha[65];
    int n;

    if (store(h, name, workload, workload_size, 0, sha) != ETUDE_SERIAL_DONE)
        return -1;
    n = snprintf(text, sizeof(text),
                 "{\"schema_version\": 1, \"run_id\": \"%s\", \"attempt_id\": \"attempt-%s\", "
                 "\"attempt_token\": \"%s\", \"case_id\": \"host.etude.serial\", "
                 "\"program\": \"%s\", \"arguments\": [], "
                 "\"environment\": {\"PATH\": \"/bin:/usr/bin\", "
                 "\"ETUDE_WORKLOAD_ROLE\": \"%s\"}, \"stdin\": null, "
                 "\"role\": \"ordinary\", \"files\": [{\"name\": \"%s\", \"sha256\": \"%s\", "
                 "\"size_bytes\": %zu, \"mode\": \"0755\"}], "
                 "\"fixtures\": [{\"fixture\": \"workspace\", \"given\": []}], "
                 "\"deadline_us\": 60000000, \"grace_us\": 500000, \"output_max_bytes\": 65536}",
                 run_id, token, token, name, role, name, sha, workload_size);
    return store(h, "invocation.json", text, (size_t)n, 0, inv) == ETUDE_SERIAL_DONE ? 0 : -1;
}

/** Begin a run of `token` from the invocation `inv` and wait as `w` says;
 * whether the run began and the wait was satisfied, as serve gives it. */
static int run(struct harness *h, const char *token, const char *inv, enum serve_until w)
{
    memset(h->out, 0, sizeof(h->out));
    h->outlen[0] = h->outlen[1] = 0;
    h->out_busy = 1;
    if (etude_serial_run(h->s, token, inv, now_us()) != 0)
        return 0;
    return serve(h, w);
}

/** Note the process group of the run's attempt, to end it at exit. */
static void note_group(struct harness *h)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);

    if (r->started && r->process.pid > 0 && ngroups < (int)(sizeof(groups) / sizeof(groups[0])))
        groups[ngroups++] = r->process.pid;
}

/** Note the owner of attempt `token` from its record in the state directory
 * `state`, so that the cleanup waits for it to end before it removes the
 * scratch that the owner writes. */
static void note_keeper(const char *state, const char *token)
{
    char path[700], text[2048], *p;
    long pid;
    unsigned long long start;
    size_t n;
    FILE *f;

    snprintf(path, sizeof(path), "%s/a/%s/record", state, token);
    if ((f = fopen(path, "r")) == NULL)
        return;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    p = strstr(text, "\nkeeper ");
    if (p != NULL && sscanf(p + 8, "%ld %llu", &pid, &start) == 2 && pid > 0 &&
        nkeepers < (int)(sizeof(keepers) / sizeof(keepers[0]))) {
        keepers[nkeepers].pid = pid;
        keepers[nkeepers].start = start;
        keepers[nkeepers].start_known = 1;
        nkeepers++;
    }
}

/** Say whether the process group `pgid` has no process left. */
static int group_gone(long pgid)
{
    return pgid > 0 && kill(-(pid_t)pgid, 0) < 0 && errno == ESRCH;
}

/* The programs: a pass with output on both streams, an intended failure,
 * and a program that runs until it is stopped. */
static const char pass[] = "serial-pass";
static const char failing[] = "serial-fail";
static const char forever[] = "serial-forever";

/** Give what the pass writes on its standard output. */
static const char *pass_output(void)
{
    static char text[1024];
    size_t n = 0;
    int i;

    for (i = 0; i < 40; i++)
        n += (size_t)snprintf(text + n, sizeof(text) - n, "out line %d\n", i);
    return text;
}

/** Say whether a run passed whole: DONE, complete, exited 0, cleaned, with
 * the pass's output given to the caller and kept. */
static int passed(struct harness *h)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);

    return etude_serial_status(h->s) == ETUDE_SERIAL_DONE && r->complete &&
           r->end == ETUDE_END_EXITED && r->value == 0 && r->cleanup == ETUDE_CLEANUP_CONFIRMED &&
           strcmp(h->out[0], pass_output()) == 0 && strcmp(h->out[1], "to stderr\n") == 0 &&
           strcmp(r->out.data, h->out[0]) == 0 && r->out.total == strlen(pass_output()) &&
           strcmp(r->err.data, "to stderr\n") == 0;
}

/** Feed the controller a frame of the session that etude-guest did not send,
 * under etude-guest's next SEQ: `session` and `boot` as given, an exit of the
 * run going on with status 9. */
static void inject_exit(struct harness *h, const char *session, const char *boot, const char *token)
{
    char text[LINE_CAP];
    size_t n;

    n = wire_encode(text, sizeof(text), session, boot, h->guest_next, 0, 0, ETUDE_EV_EXIT, 4,
                    (const char *const[]){h->run_cmd, token, "exited", "9"});
    etude_serial_feed(h->s, text, n, now_us());
}

/** Store files in etude-guest's store, with and without data, in the state
 * directory `state`. */
static void check_guest_stores(struct harness *h, const char *state)
{
    char sha[65], path[700], stored[64];
    FILE *f;
    size_t n;
    int read_ok;

    CHECK(store(h, "staged.txt", "staged\n", 7, 1, sha) == ETUDE_SERIAL_DONE,
          "a staged file is stored without data");
    CHECK(store(h, "passing", pass, strlen(pass), 0, sha) == ETUDE_SERIAL_DONE,
          "a file sent in data frames is stored");
    snprintf(path, sizeof(path), "%s/f/%s", state, sha);
    f = fopen(path, "rb");
    n = f ? fread(stored, 1, sizeof(stored), f) : 0;
    read_ok = f && feof(f) && !ferror(f);
    if (f && fclose(f) != 0)
        read_ok = 0;
    CHECK(read_ok && n == strlen(pass) && memcmp(stored, pass, n) == 0,
          "a file sent in data frames is stored");
    CHECK(store(h, "absent.txt", "abc", 3, 1, sha) == ETUDE_SERIAL_REFUSED &&
              strstr(etude_serial_why(h->s), ETUDE_ERR_DAMAGED) != NULL,
          "a staged put of a file not staged is refused as damaged");
}

/* A streamed transfer, with no whole-file buffer in the controller. */
struct file_transfer {
    const char *text;
    size_t len, at;
    char received[128];
};

/** Supply the next bytes at the exact offset requested. */
static int transfer_read(void *ctx, uint64_t offset, void *bytes, size_t n)
{
    struct file_transfer *t = ctx;

    if (offset != t->at || offset + n > t->len)
        return -1;
    memcpy(bytes, t->text + t->at, n);
    t->at += n;
    return 0;
}

/** Collect each received part exactly once. */
static int transfer_write(void *ctx, const void *bytes, size_t n)
{
    struct file_transfer *t = ctx;

    if (t->at + n >= sizeof(t->received))
        return -1;
    memcpy(t->received + t->at, bytes, n);
    t->at += n;
    t->received[t->at] = '\0';
    return 0;
}

/** Keep data in TMPDIR, with a short owned alias for bounded serial paths. */
static int installed_alias(char path[700], char program[700])
{
    char link[720];
    char *base = realpath("build/tests", NULL);
    int n;

    if (base == NULL)
        return 0;
    n = snprintf(alias_dir, sizeof(alias_dir), "%s/es.XXXXXX", base);
    free(base);
    if (n < 0 || n >= (int)sizeof(alias_dir) || mkdtemp(alias_dir) == NULL) {
        alias_dir[0] = '\0';
        return 0;
    }
    snprintf(link, sizeof(link), "%s/w", alias_dir);
    if (symlink(work, link) != 0)
        return 0;
    n = snprintf(path, 700, "%s/f", link);
    if (n < 0 || n >= 160)
        return 0;
    n = snprintf(program, 700, "%s/p", link);
    return n > 0 && n < 160;
}

/** These transfers precede large workloads; allow four-digit frame counters. */
static int installed_frames(struct harness *h, const char *path, const char *sha, size_t size)
{
    char frame[ETUDE_FRAME_SERIAL_MAX + 1], number[24];
    const char *put[] = {"9999", path, number, sha, "0600"};
    const char *sent[] = {"9999", path, number, sha};

    snprintf(number, sizeof(number), "%zu", size);
    return wire_encode(frame, sizeof(frame), h->session, h->gboot, 9999, 1, 9999, ETUDE_CMD_PUT, 5,
                       put) != 0 &&
           wire_encode(frame, sizeof(frame), h->session, h->gboot, 9999, 1, 9999, ETUDE_EV_SENT, 4,
                       sent) != 0;
}

/** Transfer to an absolute destination and back, then run an installed
 * program with no stored executable in its invocation. */
static void check_installed(struct harness *h)
{
    struct file_transfer t;
    const struct etude_serial_result *r;
    char path[700], program[700], sha[65], inv[65], text[2048];
    const char *token = "00000000000000a5";
    FILE *file;
    int n;

    if (!installed_alias(path, program)) {
        CHECK(0, "the installed paths have a private absolute alias");
        return;
    }
    file = fopen(program, "wb");
    if (file == NULL || fwrite(workload, 1, workload_size, file) != workload_size) {
        if (file != NULL)
            fclose(file);
        CHECK(0, "the installed program is prepared");
        return;
    }
    if (fclose(file) != 0 || chmod(program, 0755) != 0) {
        CHECK(0, "the installed program is executable");
        return;
    }
    memset(&t, 0, sizeof(t));
    t.text = "a file transferred through the guest channel\n";
    t.len = strlen(t.text);
    digest_hex(t.text, t.len, sha);
    if (!installed_frames(h, path, sha, t.len)) {
        CHECK(0, "the absolute transfer paths fit serial frames");
        return;
    }
    CHECK(etude_serial_put_reader(h->s, path, t.len, sha, "0600", transfer_read, &t, now_us()) ==
                  0 &&
              serve(h, UNTIL_END) && etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "a streamed put publishes an absolute guest destination");
    t.at = 0;
    CHECK(etude_serial_get(h->s, path, transfer_write, &t, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE && t.at == t.len &&
              strcmp(t.text, t.received) == 0,
          "get checks and returns the complete absolute file");
    n = snprintf(text, sizeof(text),
                 "{\"schema_version\":1,\"run_id\":\"installed\",\"attempt_id\":\"installed\","
                 "\"attempt_token\":\"%s\",\"case_id\":\"host.etude.serial\","
                 "\"program\":\"%s\",\"arguments\":[],"
                 "\"environment\":{\"ETUDE_WORKLOAD_ROLE\":\"serial-installed\"},\"stdin\":\"%s\","
                 "\"role\":\"ordinary\",\"files\":[],\"fixtures\":[],"
                 "\"deadline_us\":60000000,\"grace_us\":500000,\"output_max_bytes\":65536}",
                 token, program, path);
    CHECK(n > 0 && (size_t)n < sizeof(text) &&
              store(h, "invocation.json", text, (size_t)n, 0, inv) == ETUDE_SERIAL_DONE &&
              run(h, token, inv, UNTIL_END),
          "an installed absolute program runs");
    r = etude_serial_result(h->s);
    CHECK(r->complete && r->end == ETUDE_END_EXITED && r->value == 7 &&
              r->cleanup == ETUDE_CLEANUP_CONFIRMED && strcmp(h->out[0], t.text) == 0,
          "an installed program retains exit status, output and cleanup");
    unlink(path);
    unlink(program);
}

/** Run a pass, an intended failure, a pass beside frames that etude-guest
 * did not send, and a program cancelled, of the run `run_id`. */
static void check_guest_runs(struct harness *h, const char *run_id)
{
    const struct etude_serial_result *r = etude_serial_result(h->s);
    char inv[65];

    CHECK(prepare(h, run_id, "passing", pass, "00000000000000d1", inv) == 0 &&
              run(h, "00000000000000d1", inv, UNTIL_END) && passed(h) && h->out_busy &&
              r->cpu_us.quality == ETUDE_QUALITY_LOWER_BOUND,
          "a pass arrives whole and in order");
    note_group(h);
    CHECK(prepare(h, run_id, "failing", failing, "00000000000000d2", inv) == 0 &&
              run(h, "00000000000000d2", inv, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE && r->complete &&
              r->end == ETUDE_END_EXITED && r->value == 3 && strcmp(h->out[1], "broken\n") == 0,
          "an intended failure gives its exit status");
    note_group(h);

    CHECK(prepare(h, run_id, "passing", pass, "00000000000000d3", inv) == 0 &&
              run(h, "00000000000000d3", inv, UNTIL_STARTED) && r->started,
          "a run starts");
    note_group(h);
    inject_exit(h, OTHER, h->gboot, "00000000000000d3");
    inject_exit(h, h->session, OTHER, "00000000000000d3");
    CHECK(serve(h, UNTIL_END) && passed(h), "a frame of another session or boot is not acted on");

    CHECK(prepare(h, run_id, "forever", forever, "00000000000000d4", inv) == 0 &&
              run(h, "00000000000000d4", inv, UNTIL_READY) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "output arrives while the program runs");
    note_group(h);
    CHECK(etude_serial_cancel(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE && r->end == ETUDE_END_STOPPED &&
              r->cleanup == ETUDE_CLEANUP_CONFIRMED && group_gone(r->process.pid),
          "a cancelled run is stopped and its processes are gone");
}

/* The real responder keeps an owned service alive across another command. */
static void check_guest_service(struct harness *h, const char *run_id)
{
    char inv[65];
    long pid;
    const struct etude_serial_result *r;

    CHECK(prepare(h, run_id, "forever", forever, "00000000000000d5", inv) == 0 &&
              run(h, "00000000000000d5", inv, UNTIL_READY),
          "a real guest service publishes readiness");
    pid = etude_serial_result(h->s)->process.pid;
    note_group(h);
    CHECK(etude_serial_background(h->s) == 0 && etude_serial_background_running(h->s),
          "the real service remains below its command owner");
    CHECK(prepare(h, run_id, "passing", pass, "00000000000000d6", inv) == 0 &&
              run(h, "00000000000000d6", inv, UNTIL_END) && passed(h) &&
              etude_serial_background_running(h->s),
          "a second real command and its transfers complete beside the service");
    CHECK(etude_serial_foreground(h->s) == 0 && etude_serial_cancel(h->s, now_us()) == 0 &&
              serve(h, UNTIL_END),
          "the real service cancels through its original command identity");
    r = etude_serial_result(h->s);
    CHECK(r->complete && r->end == ETUDE_END_STOPPED && r->cleanup == ETUDE_CLEANUP_CONFIRMED &&
              group_gone(pid),
          "the real service ends and its owned processes are gone");
}

/** End the session with bye, and open another. */
static void check_guest_bye(struct harness *h)
{
    char sha[65];

    digest_hex("x", 1, sha);
    CHECK(etude_serial_bye(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "bye ends the session");
    CHECK(etude_serial_put(h->s, "x", 1, sha, "0644", NULL, now_us()) == -1 && errno == EPIPE,
          "nothing but an open follows bye");
    CHECK(etude_serial_open(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE &&
              etude_serial_bye(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "a session opens again after bye");
}

/** The actual compact option parser keeps the requested resend interval. */
static void check_guest_options(const char *state)
{
    char path[700], line[200];
    snprintf(path, sizeof(path), "%s/guest.log", state);
    FILE *f = fopen(path, "r");
    int ok = f && fgets(line, sizeof(line), f) && strstr(line, "options: resend_us=50000 stages=1");
    if (f)
        ok = fclose(f) == 0 && ok;
    CHECK(ok, "the actual compact parser retains 50ms and one stage root");
}

/** The serial transport with etude-guest itself, directly. */
static void check_guest(void)
{
    static const char run_id[] = "etude-serial-test guest";
    struct harness *h = &H;
    char state[600], boot[ETUDE_FRAME_TOKEN_LEN + 1];
    struct etude_pid pid;

    begin(h, run_id, KEEP, 0);
    snprintf(state, sizeof(state), "%s/state-guest", work);
    start_guest(h, state);
    CHECK(etude_serial_open(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE &&
              etude_serial_guest(h->s, &pid, boot) == 0 && pid.pid == (long)h->pid,
          "the session opens with etude-guest");
    if (etude_serial_status(h->s) == ETUDE_SERIAL_DONE) {
        check_guest_stores(h, state);
        check_installed(h);
        check_guest_runs(h, run_id);
        check_guest_service(h, run_id);
        check_guest_bye(h);
    }
    end_guest(h);
    check_guest_options(state);
    finish(h);
}

/** The serial transport through a relay that drops, repeats, damages and
 * reorders frames both ways. */
static void check_relay(void)
{
    static const char run_id[] = "etude-serial-test relay";
    struct harness *h = &H;
    char inv[65], sha[65], state[600];
    begin(h, run_id, KEEP, 0);
    h->relay = 1;
    snprintf(state, sizeof(state), "%s/state-relay", work);
    start_guest(h, state);
    CHECK(etude_serial_open(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "through a faulty relay the session opens");
    CHECK(workload_size >= 4096, "the program needs multiple transfer frames");
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE &&
              store(h, "padded", workload, workload_size, 0, sha) == ETUDE_SERIAL_DONE,
          "through a faulty relay a file is stored");
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE &&
              prepare(h, run_id, "padded", pass, "00000000000000e1", inv) == 0 &&
              run(h, "00000000000000e1", inv, UNTIL_END) && passed(h),
          "through a faulty relay a pass arrives whole and in order");
    note_group(h);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_DONE && etude_serial_bye(h->s, now_us()) == 0 &&
              serve(h, UNTIL_END) && etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "through a faulty relay bye ends the session");
    CHECK(h->drops[0] > 0 && h->drops[1] > 0 && h->repeats[0] > 0 && h->repeats[1] > 0 &&
              h->damages[0] > 0 && h->damages[1] > 0 && h->swaps[0] > 0 && h->swaps[1] > 0,
          "the relay dropped, repeated, damaged and reordered frames both ways");
    CHECK(h->resent[0] > 0 && h->resent[1] > 0, "both sides sent frames again");
    end_guest(h);
    finish(h);
}

/** A console closed during a run. */
static void check_closed(void)
{
    static const char run_id[] = "etude-serial-test lost";
    struct harness *h = &H;
    const struct etude_serial_result *r;
    char inv[65], state[600];
    long group;
    int st = -1;

    begin(h, run_id, KEEP, 0);
    r = etude_serial_result(h->s);
    snprintf(state, sizeof(state), "%s/state-closed", work);
    start_guest(h, state);
    CHECK(etude_serial_open(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              prepare(h, run_id, "forever", forever, "00000000000000f1", inv) == 0 &&
              run(h, "00000000000000f1", inv, UNTIL_READY) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "a run goes on before its console closes");
    note_group(h);
    group = r->process.pid;
    etude_serial_closed(h->s);
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_LOST && !r->complete &&
              r->end == ETUDE_END_LOST && r->value == -1 && r->cleanup == ETUDE_CLEANUP_UNCONFIRMED,
          "a console closed in a run loses it, its end unknown");
    CHECK(etude_serial_put(h->s, "x", 1, inv, "0644", NULL, now_us()) == -1 && errno == EPIPE,
          "nothing begins on a closed console");
    close(h->to);
    h->to = -1;
    CHECK(drain(h) && waitpid(h->pid, &st, 0) == h->pid && WIFEXITED(st) && WEXITSTATUS(st) == 1 &&
              group_gone(group),
          "etude-guest ends the attempt of a closed console and exits");
    reaped(h->pid);
    h->pid = -1;
    CHECK(etude_serial_status(h->s) == ETUDE_SERIAL_LOST && r->end == ETUDE_END_LOST,
          "a console closed in a run loses it, its end unknown");
    end_guest(h);
    finish(h);
}

/** An etude-guest killed and started again during a run. */
static void check_restart(void)
{
    static const char run_id[] = "etude-serial-test lost";
    struct harness *h = &H;
    const struct etude_serial_result *r;
    char inv[65], state[600], boot[ETUDE_FRAME_TOKEN_LEN + 1];
    struct etude_pid pid;

    begin(h, run_id, KEEP, 0);
    r = etude_serial_result(h->s);
    snprintf(state, sizeof(state), "%s/state-restart", work);
    start_guest(h, state);
    CHECK(etude_serial_open(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              prepare(h, run_id, "forever", forever, "00000000000000f2", inv) == 0 &&
              run(h, "00000000000000f2", inv, UNTIL_READY) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_BUSY,
          "a run goes on before its etude-guest is killed");
    note_group(h);
    note_keeper(state, "00000000000000f2");
    end_guest(h);
    if (r->process.pid > 0)
        kill(-(pid_t)r->process.pid, SIGKILL);
    start_guest(h, state);
    CHECK(serve(h, UNTIL_END) && etude_serial_status(h->s) == ETUDE_SERIAL_LOST &&
              r->end == ETUDE_END_LOST && !r->complete &&
              strstr(etude_serial_why(h->s), "started again") != NULL &&
              etude_serial_guest(h->s, &pid, boot) == 0 && pid.pid == (long)h->pid,
          "an etude-guest started again loses the run");
    CHECK(etude_serial_open(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE &&
              etude_serial_bye(h->s, now_us()) == 0 && serve(h, UNTIL_END) &&
              etude_serial_status(h->s) == ETUDE_SERIAL_DONE,
          "a session opens with the etude-guest that started again");
    end_guest(h);
    finish(h);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[700];
    FILE *f;

    check_guest_line();
    workload = host_workload_read(&workload_size);
    if (workload == NULL) {
        fputs("etude-serial-test: prepared workload is missing\n", stderr);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup);
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    snprintf(work, sizeof(work), "%s/rach2-etude-serial.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (mkdtemp(work) == NULL) {
        *work = '\0';
        perror("etude-serial-test: mkdtemp");
        return 2;
    }
    snprintf(stage, sizeof(stage), "%s/stage", work);
    snprintf(path, sizeof(path), "%s/staged.txt", stage);
    if (mkdir(stage, 0700) != 0 || (f = fopen(path, "w")) == NULL || fputs("staged\n", f) < 0 ||
        fclose(f) != 0) {
        perror("etude-serial-test: stage");
        return 2;
    }
    check_scripted_session();
    check_scripted_run();
    check_scripted_restart();
    check_scripted_service();
    check_guest();
    check_relay();
    check_closed();
    check_restart();
    if (fails != 0) {
        printf("etude-serial-test: %d checks failed\n", fails);
        return 1;
    }
    printf("etude-serial-test: result: pass\n");
    return 0;
}
