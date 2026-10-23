/*
 * serial.c: the controller's side of etude-guest on a serial console. It takes
 * etude-guest's frames from the console's lines, acts on each once and in
 * order, and sends its own until they are acknowledged; serial.h gives the
 * contract and tools/etude/Readme.md the protocol.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../utils/sha256.h"
#include "capture.h"
#include "guest/wire.h"
#include "serial.h"

/*
 * The sequenced frames in flight at once. Four frames of 255 bytes and their
 * acknowledgements fit with room in the 4096-byte receive ring of Rach2's
 * serial driver, so no frame is lost to a full ring.
 */
#define WINDOW 4

/* The frames queued, in flight or not yet sent: a window and the commands
 * that may follow it before it drains. */
#define QUEUE_MAX (2 * WINDOW)

/* The bytes of a line kept: a frame, which ends the line, is never longer
 * than ETUDE_FRAME_SERIAL_MAX, and what precedes it is the kernel's text. */
#define LINE_KEEP (2 * ETUDE_FRAME_SERIAL_MAX)

/* The operations, as serial.h describes them. */
enum op { OP_NONE, OP_OPEN, OP_PUT, OP_GET, OP_RUN, OP_BYE };

/* A sequenced frame of this side's, sent and not yet acknowledged, or queued
 * behind a full window. */
struct out_frame {
    char text[ETUDE_FRAME_SERIAL_MAX + 1];
    size_t len;
    uint64_t seq;
    uint64_t sent_us; /* when it was last written */
};

struct etude_serial {
    struct etude_serial_conf conf;
    char session[ETUDE_FRAME_TOKEN_LEN + 1];
    int closed;

    /* etude-guest, from its last `alive` */
    int guest_seen;
    char boot[ETUDE_FRAME_TOKEN_LEN + 1];
    struct etude_pid guest;
    char versions[64];
    int alive_fresh; /* an alive came since the last hello, which may follow */

    /* the session */
    int live;          /* a hello was sent: frames of the session are taken */
    int open;          /* ready came, and neither bye nor a loss since */
    int hello_sent;    /* the open going on has sent its hello */
    uint64_t seq_out;  /* the SEQ of the next frame queued */
    uint64_t seq_wire; /* one past the highest SEQ written */
    uint64_t recv_next;
    int need_ack;
    struct out_frame q[QUEUE_MAX];
    size_t nq;    /* frames queued */
    size_t nsent; /* of them, the first ones, written */
    uint64_t next_cmd;

    /* the operation begun last */
    enum op op;
    enum etude_serial_status status;
    uint64_t cmd;
    uint64_t cancel_cmd; /* the run's cancel, or 0 */
    char why[256];

    /* a put */
    const unsigned char *data;
    int (*file_read)(void *, uint64_t, void *, size_t);
    void *read_ctx;
    uint64_t size;
    uint64_t at; /* the bytes queued in data frames */
    int sealed;  /* the seal is queued */
    char name[ETUDE_FRAME_SERIAL_MAX];
    char sha[65];

    /* a get */
    int (*file_write)(void *, const void *, size_t);
    void *file_ctx;

    /* a run */
    char attempt[ETUDE_FRAME_TOKEN_LEN + 1];
    int have_exit, have_stream[2], have_usage, have_cleaned;
    int sizes_known[2]; /* the stream's sizes and digest were given */
    int damaged;
    uint64_t sent[2];   /* the bytes of each stream `stream` says were sent */
    char digest[2][65]; /* and their SHA-256 */
    uint64_t got[2];    /* the bytes of each stream received */
    struct sha256 got_sha[2];
    struct etude_serial_result result;

    /* the console's line being read */
    char line[LINE_KEEP + 1];
    size_t line_len;
};

/** Set why the operation stands as it does. */
static void set_why(struct etude_serial *s, const char *fmt, va_list ap)
{
    vsnprintf(s->why, sizeof(s->why), fmt, ap);
}

/** Give the first ETUDE_FRAME_TOKEN_LEN digits of the SHA-256 of `text`. */
static void token_of(const char *text, char token[ETUDE_FRAME_TOKEN_LEN + 1])
{
    struct sha256 h;
    unsigned char d[32];
    char hex[65];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)text, strlen(text));
    sha256_final(&h, d);
    sha256_hex(d, hex);
    memcpy(token, hex, ETUDE_FRAME_TOKEN_LEN);
    token[ETUDE_FRAME_TOKEN_LEN] = '\0';
}

/** Say whether `s` is one or more bytes from '!' to '~', as a field is. */
static int printable(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s != '\0'; s++)
        if (*s < '!' || *s > '~')
            return 0;
    return 1;
}

/** Say whether the comma-separated `list` of versions has version 1. */
static int offers_one(const char *list)
{
    const char *p = list;

    for (;;) {
        if (p[0] == '1' && (p[1] == ',' || p[1] == '\0'))
            return 1;
        p = strchr(p, ',');
        if (p == NULL)
            return 0;
        p++;
    }
}

/**
 * End the operation going on with `st`, and say why unless it is DONE. A run
 * that ends before exit, usage and cleaned have all arrived keeps no end but
 * LOST, and no cleanup but an unconfirmed one.
 */
static void end_op(struct etude_serial *s, enum etude_serial_status st, const char *fmt, ...)
{
    struct etude_serial_result *r = &s->result;
    va_list ap;

    if (s->status != ETUDE_SERIAL_BUSY)
        return;
    s->status = st;
    s->why[0] = '\0';
    if (st != ETUDE_SERIAL_DONE) {
        va_start(ap, fmt);
        set_why(s, fmt, ap);
        va_end(ap);
    }
    if (s->op == OP_RUN && !(s->have_exit && s->have_usage && s->have_cleaned)) {
        r->end = ETUDE_END_LOST;
        r->value = -1;
        r->complete = 0;
        if (r->cleanup == ETUDE_CLEANUP_PENDING)
            r->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    }
}

/** Lose the session: the operation going on ends LOST, and no frame of the
 * session is taken or sent again. */
static void lose(struct etude_serial *s, const char *why)
{
    end_op(s, ETUDE_SERIAL_LOST, "%s", why);
    s->live = 0;
    s->open = 0;
    s->need_ack = 0;
    s->nq = 0;
    s->nsent = 0;
}

/** Write `n` bytes to the console; 0, or -1 when it has closed. */
static int send_raw(struct etude_serial *s, const char *text, size_t n)
{
    if (s->closed)
        return -1;
    if (s->conf.write(s->conf.ctx, text, n) == 0)
        return 0;
    s->closed = 1;
    lose(s, "the console closed");
    return -1;
}

/** Encode a frame of the session into `out`; its length, or 0 when it does
 * not fit the serial bound. */
static size_t encode(struct etude_serial *s, char *out, uint64_t seq, const char *type, int n,
                     const char *const *field)
{
    return wire_encode(out, ETUDE_FRAME_SERIAL_MAX + 1, s->session, s->boot, seq, s->recv_next > 0,
                       s->recv_next - 1, type, n, field);
}

/**
 * Queue a sequenced frame behind those queued before it.
 *
 * @return 0, or -1 with errno EINVAL when it does not fit the serial bound or
 *         EBUSY when the queue is full.
 */
static int queue_frame(struct etude_serial *s, const char *type, int n, const char *const *field)
{
    struct out_frame *f;

    if (s->nq == QUEUE_MAX) {
        errno = EBUSY;
        return -1;
    }
    f = &s->q[s->nq];
    f->len = encode(s, f->text, s->seq_out, type, n, field);
    if (f->len == 0) {
        errno = EINVAL;
        return -1;
    }
    f->seq = s->seq_out++;
    s->nq++;
    return 0;
}

/**
 * Queue the next frame of the put going on: a data frame of as many bytes as
 * fit the serial bound, and the seal after the last.
 *
 * @return 1 when a frame was queued, 0 when none is left or an error ended
 *         the put.
 */
static int put_next(struct etude_serial *s)
{
    char cmd[24], off[24], probe[ETUDE_FRAME_SERIAL_MAX + 1];
    char b64[ETUDE_FRAME_SERIAL_MAX + 1];
    const char *a[3];
    size_t room, chunk;
    unsigned char bytes[ETUDE_FRAME_SERIAL_MAX];

    if (s->op != OP_PUT || s->status != ETUDE_SERIAL_BUSY || s->sealed)
        return 0;
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->cmd);
    a[0] = cmd;
    if ((s->data == NULL && s->file_read == NULL) || s->at >= s->size) {
        if (queue_frame(s, ETUDE_CMD_SEAL, 1, a) != 0) {
            end_op(s, ETUDE_SERIAL_PROTOCOL, "the seal of %s cannot be queued", s->name);
            return 0;
        }
        s->sealed = 1;
        return 1;
    }
    snprintf(off, sizeof(off), "%llu", (unsigned long long)s->at);
    a[1] = off;
    a[2] = "A";
    /* The frame with one byte of base64 gives the room the rest may have. */
    room = encode(s, probe, s->seq_out, ETUDE_CMD_DATA, 3, a);
    room = room == 0 ? 0 : ETUDE_FRAME_SERIAL_MAX - (room - 1);
    chunk = room / 4 * 3;
    if (chunk > s->size - s->at)
        chunk = (size_t)(s->size - s->at);
    if (s->file_read != NULL) {
        if (s->file_read(s->read_ctx, s->at, bytes, chunk) != 0) {
            lose(s, "the file being sent could not be read");
            return 0;
        }
        wire_base64(bytes, chunk, b64);
    } else
        wire_base64(s->data + s->at, chunk, b64);
    a[2] = b64;
    if (chunk == 0 || queue_frame(s, ETUDE_CMD_DATA, 3, a) != 0) {
        end_op(s, ETUDE_SERIAL_PROTOCOL, "a data frame of %s does not fit", s->name);
        return 0;
    }
    s->at += chunk;
    return 1;
}

/** Write the queued frames the window has room for, and queue the put's next
 * frames as it drains. */
static void pump(struct etude_serial *s, uint64_t now_us)
{
    struct out_frame *f;

    for (;;) {
        if (s->closed)
            return;
        if (s->nsent < s->nq && s->nsent < WINDOW) {
            f = &s->q[s->nsent];
            if (send_raw(s, f->text, f->len) != 0)
                return;
            f->sent_us = now_us;
            s->nsent++;
            if (f->seq + 1 > s->seq_wire)
                s->seq_wire = f->seq + 1;
            continue;
        }
        if (s->nq < WINDOW && put_next(s))
            continue;
        return;
    }
}

/** Release the frames that etude-guest has acknowledged, up to SEQ `ack`. */
static void release(struct etude_serial *s, uint64_t ack)
{
    size_t n = 0;

    while (n < s->nsent && s->q[n].seq <= ack)
        n++;
    if (n == 0)
        return;
    memmove(s->q, s->q + n, (s->nq - n) * sizeof(s->q[0]));
    s->nq -= n;
    s->nsent -= n;
}

/** Write an `ack` frame: the last SEQ received in order, under the next SEQ,
 * which it does not take. */
static void send_ack(struct etude_serial *s)
{
    char text[ETUDE_FRAME_SERIAL_MAX + 1];
    size_t n = encode(s, text, s->seq_out, ETUDE_CMD_ACK, 0, NULL);

    if (n != 0)
        send_raw(s, text, n);
}

/** Begin an operation of kind `op`. */
static void begin(struct etude_serial *s, enum op op)
{
    s->op = op;
    s->status = ETUDE_SERIAL_BUSY;
    s->why[0] = '\0';
    s->cancel_cmd = 0;
}

/** Say whether an operation may begin, setting errno when it may not. An open
 * needs no session; every other operation needs one. */
static int may_begin(struct etude_serial *s, int needs_session)
{
    if (s->status == ETUDE_SERIAL_BUSY) {
        errno = EBUSY;
        return 0;
    }
    if (s->closed || (needs_session && !s->open)) {
        errno = EPIPE;
        return 0;
    }
    if (!needs_session && s->open) {
        errno = EISCONN;
        return 0;
    }
    return 1;
}

/** Start the session with etude-guest's last alive: a hello under SEQ 0 that
 * offers version 1 and frames of ETUDE_FRAME_SERIAL_MAX bytes. */
static void send_hello(struct etude_serial *s, uint64_t now_us)
{
    char cmd[24], max[24];
    const char *a[3];

    if (!offers_one(s->versions)) {
        end_op(s, ETUDE_SERIAL_REFUSED, "etude-guest offers versions %s, and this controller 1",
               s->versions);
        return;
    }
    s->nq = s->nsent = 0;
    s->seq_out = s->seq_wire = 0;
    s->recv_next = 0;
    s->need_ack = 0;
    s->live = 1;
    s->hello_sent = 1;
    s->alive_fresh = 0;
    s->cmd = s->next_cmd++;
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->cmd);
    snprintf(max, sizeof(max), "%d", ETUDE_FRAME_SERIAL_MAX);
    a[0] = cmd;
    a[1] = "1";
    a[2] = max;
    if (queue_frame(s, ETUDE_CMD_HELLO, 3, a) != 0) {
        end_op(s, ETUDE_SERIAL_PROTOCOL, "the hello cannot be queued");
        return;
    }
    pump(s, now_us);
}

/**
 * Take etude-guest's `alive`. A first alive, or one of the same process after
 * its session ended, lets a hello follow. One of another process or boot is
 * an etude-guest that started again, which loses the session; one of the same
 * process while the session goes on is a late copy, and changes nothing.
 */
static void take_alive(struct etude_serial *s, const struct wire_frame *f, uint64_t now_us)
{
    uint64_t pid, start;
    int same;

    if (f->nfields != 3 || strcmp(f->session, ETUDE_FRAME_NO_SESSION) != 0 || f->seq != 0 ||
        !wire_number(f->field[1], &pid) || !wire_number(f->field[2], &start) ||
        strlen(f->field[0]) >= sizeof(s->versions))
        return;
    same = s->guest_seen && strcmp(f->boot, s->boot) == 0 && (long)pid == s->guest.pid &&
           start == s->guest.start;
    if (same && (s->open || (s->op == OP_OPEN && s->status == ETUDE_SERIAL_BUSY && s->hello_sent)))
        return;
    if (!same && s->live) {
        char why[128];
        snprintf(why, sizeof(why), "etude-guest started again, as PID %llu of boot %s",
                 (unsigned long long)pid, f->boot);
        lose(s, why);
    }
    s->live = 0;
    s->nq = s->nsent = 0;
    s->guest_seen = 1;
    memcpy(s->boot, f->boot, sizeof(s->boot));
    s->guest.pid = (long)pid;
    s->guest.start = start;
    s->guest.start_known = 1;
    snprintf(s->versions, sizeof(s->versions), "%s", f->field[0]);
    s->alive_fresh = 1;
    if (s->op == OP_OPEN && s->status == ETUDE_SERIAL_BUSY && !s->hello_sent)
        send_hello(s, now_us);
}

/** End the operation PROTOCOL for an event `f` that does not follow. */
static void protocol(struct etude_serial *s, const struct wire_frame *f, const char *what)
{
    end_op(s, ETUDE_SERIAL_PROTOCOL, "etude-guest sent %s %s, %s", f->type,
           f->nfields > 0 ? f->field[0] : "-", what);
}

/** Take `ready CMD VERSION MAXFRAME PID START`, which opens the session. */
static void take_ready(struct etude_serial *s, const struct wire_frame *f)
{
    uint64_t max, pid, start;

    if (strcmp(f->type, ETUDE_EV_READY) != 0 || f->nfields != 5) {
        protocol(s, f, "where ready was due");
        return;
    }
    if (strcmp(f->field[1], "1") != 0 || !wire_number(f->field[2], &max) ||
        max != ETUDE_FRAME_SERIAL_MAX || !wire_number(f->field[3], &pid) ||
        !wire_number(f->field[4], &start) || (long)pid != s->guest.pid || start != s->guest.start) {
        protocol(s, f, "with a version, frame bound or process that was not offered");
        return;
    }
    s->open = 1;
    end_op(s, ETUDE_SERIAL_DONE, "");
}

/** Take `stored CMD NAME SHA256`, which ends a put. */
static void take_stored(struct etude_serial *s, const struct wire_frame *f)
{
    if (strcmp(f->type, ETUDE_EV_STORED) != 0 || f->nfields != 3 ||
        strcmp(f->field[1], s->name) != 0 || !s->sealed) {
        protocol(s, f, "where the stored of the put was due");
        return;
    }
    if (strcmp(f->field[2], s->sha) != 0) {
        end_op(s, ETUDE_SERIAL_DAMAGED, "etude-guest stored %s with digest %s, not %s", s->name,
               f->field[2], s->sha);
        return;
    }
    end_op(s, ETUDE_SERIAL_DONE, "");
}

/** Take data parts followed by sent, checking offsets, size and digest. */
static void take_get(struct etude_serial *s, const struct wire_frame *f)
{
    unsigned char bytes[ETUDE_FRAME_SERIAL_MAX], d[32];
    char hex[65];
    uint64_t off;
    long n;

    if (strcmp(f->type, ETUDE_CMD_DATA) == 0) {
        if (f->nfields != 3 || !wire_number(f->field[1], &off) || off != s->at ||
            strlen(f->field[2]) / 4 * 3 > sizeof(bytes) ||
            (n = wire_unbase64(f->field[2], bytes)) <= 0) {
            protocol(s, f, "with a damaged file part");
            return;
        }
        if (s->file_write(s->file_ctx, bytes, (size_t)n) != 0) {
            lose(s, "the received file could not be written");
            return;
        }
        s->at += (uint64_t)n;
        sha256_update(&s->got_sha[0], bytes, (size_t)n);
        return;
    }
    if (strcmp(f->type, ETUDE_EV_SENT) != 0 || f->nfields != 4 ||
        strcmp(f->field[1], s->name) != 0 || !wire_number(f->field[2], &off) ||
        !wire_hex(f->field[3], 64)) {
        protocol(s, f, "where the sent of the get was due");
        return;
    }
    sha256_final(&s->got_sha[0], d);
    sha256_hex(d, hex);
    if (off != s->at || strcmp(hex, f->field[3]) != 0)
        end_op(s, ETUDE_SERIAL_DAMAGED, "the received file has another size or digest");
    else
        end_op(s, ETUDE_SERIAL_DONE, "");
}

/** Take `bye CMD`, which ends the session. */
static void take_bye(struct etude_serial *s, const struct wire_frame *f)
{
    if (strcmp(f->type, ETUDE_CMD_BYE) != 0 || f->nfields != 1) {
        protocol(s, f, "where bye was due");
        return;
    }
    s->open = 0;
    end_op(s, ETUDE_SERIAL_DONE, "");
}

/** Read a usage value, N, N+ for a lower bound or -, into `m`. */
static void take_measure(struct etude_measure *m, char *method, size_t cap, const char *value,
                         const char *name)
{
    char digits[24];
    size_t len = strlen(value);
    uint64_t v;

    m->quality = ETUDE_QUALITY_UNAVAILABLE;
    m->value = 0;
    m->method = "etude-guest could not measure it";
    if (len == 0 || len >= sizeof(digits) || strcmp(value, "-") == 0)
        return;
    memcpy(digits, value, len + 1);
    if (digits[len - 1] == '+')
        digits[len - 1] = '\0';
    if (!wire_number(digits, &v))
        return;
    m->value = v;
    m->quality = value[len - 1] == '+' ? ETUDE_QUALITY_LOWER_BOUND : ETUDE_QUALITY_EXACT;
    snprintf(method, cap, "%s", strcmp(name, "-") == 0 ? "etude-guest" : name);
    m->method = method;
}

/** Take `out CMD ATTEMPT FD OFFSET BASE64`; 0, or -1 when out of order. */
static int take_out(struct etude_serial *s, const struct wire_frame *f)
{
    unsigned char buf[ETUDE_FRAME_SERIAL_MAX];
    uint64_t off;
    long n;
    int fd;

    if (!s->result.started || s->have_exit || f->nfields != 5 ||
        (strcmp(f->field[2], "1") != 0 && strcmp(f->field[2], "2") != 0) ||
        !wire_number(f->field[3], &off) || strlen(f->field[4]) / 4 * 3 > sizeof(buf))
        return -1;
    fd = f->field[2][0] - '1';
    n = wire_unbase64(f->field[4], buf);
    if (n <= 0 || off != s->got[fd])
        return -1;
    s->got[fd] += (uint64_t)n;
    sha256_update(&s->got_sha[fd], buf, (size_t)n);
    capture_add(fd == 0 ? &s->result.out : &s->result.err, s->conf.output_max, (const char *)buf,
                (size_t)n);
    if (s->conf.output != NULL)
        s->conf.output(s->conf.ctx, fd + 1, (const char *)buf, (size_t)n);
    return 0;
}

/** Take `started CMD ATTEMPT PID START`; 0, or -1 when it does not follow. */
static int take_started(struct etude_serial *s, const struct wire_frame *f)
{
    struct etude_serial_result *r = &s->result;
    uint64_t pid, start = 0;

    if (r->started || f->nfields != 4 || !wire_number(f->field[2], &pid) ||
        (strcmp(f->field[3], "-") != 0 && !wire_number(f->field[3], &start)))
        return -1;
    r->started = 1;
    r->process.pid = (long)pid;
    r->process.start = start;
    r->process.start_known = strcmp(f->field[3], "-") != 0;
    return 0;
}

/** Take `exit CMD ATTEMPT KIND VALUE`; 0, or -1 when it does not follow. */
static int take_exit(struct etude_serial *s, const struct wire_frame *f)
{
    struct etude_serial_result *r = &s->result;
    const char *k;
    uint64_t v;

    if (!r->started || s->have_exit || f->nfields != 4)
        return -1;
    k = f->field[2];
    if (strcmp(k, "exited") == 0)
        r->end = ETUDE_END_EXITED;
    else if (strcmp(k, "signaled") == 0)
        r->end = ETUDE_END_SIGNALED;
    else if (strcmp(k, "stopped") == 0)
        r->end = ETUDE_END_STOPPED;
    else if (strcmp(k, "lost") == 0)
        r->end = ETUDE_END_LOST;
    else
        return -1;
    r->value = -1;
    if (wire_number(f->field[3], &v) && v < 256)
        r->value = (int)v;
    else if (strcmp(f->field[3], "-") != 0)
        return -1;
    if ((r->end == ETUDE_END_EXITED || r->end == ETUDE_END_SIGNALED) && r->value < 0)
        return -1;
    s->have_exit = 1;
    return 0;
}

/**
 * Take `stream CMD ATTEMPT FD SIZE TOTAL SHA256`, of descriptor 1 and then 2
 * after the exit, or `- - -` for sizes that are not known; 0, or -1 when it
 * does not follow. A size or digest that disagrees with the output received
 * marks the run damaged.
 */
static int take_stream(struct etude_serial *s, const struct wire_frame *f)
{
    struct etude_output *o;
    unsigned char d[32];
    char hex[65];
    uint64_t total;
    int fd = s->have_stream[0];

    if (!s->have_exit || s->have_stream[1] || f->nfields != 6 ||
        strcmp(f->field[2], fd == 0 ? "1" : "2") != 0)
        return -1;
    s->have_stream[fd] = 1;
    o = fd == 0 ? &s->result.out : &s->result.err;
    o->closed = 1;
    if (strcmp(f->field[3], "-") == 0 && strcmp(f->field[4], "-") == 0 &&
        strcmp(f->field[5], "-") == 0)
        return 0;
    if (!wire_number(f->field[3], &s->sent[fd]) || !wire_number(f->field[4], &total) ||
        !wire_hex(f->field[5], 64) || total < s->sent[fd])
        return -1;
    memcpy(s->digest[fd], f->field[5], 65);
    o->total = total;
    sha256_final(&s->got_sha[fd], d);
    sha256_hex(d, hex);
    if (s->sent[fd] != s->got[fd] || strcmp(hex, s->digest[fd]) != 0)
        s->damaged = 1;
    s->sizes_known[fd] = 1;
    return 0;
}

/** Take `usage CMD ATTEMPT CPU-US METHOD PEAK METHOD`; 0, or -1 when it does
 * not follow. */
static int take_usage(struct etude_serial *s, const struct wire_frame *f)
{
    struct etude_serial_result *r = &s->result;

    if (!s->have_stream[1] || s->have_usage || f->nfields != 6)
        return -1;
    take_measure(&r->cpu_us, r->cpu_method, sizeof(r->cpu_method), f->field[2], f->field[3]);
    take_measure(&r->peak_bytes, r->peak_method, sizeof(r->peak_method), f->field[4], f->field[5]);
    s->have_usage = 1;
    return 0;
}

/**
 * Take `cleaned CMD ATTEMPT confirmed|unconfirmed`, which ends the run:
 * DAMAGED when the output disagreed with its streams, and DONE otherwise,
 * complete when both streams' sizes were known. 0, or -1 when it does not
 * follow.
 */
static int take_cleaned(struct etude_serial *s, const struct wire_frame *f)
{
    struct etude_serial_result *r = &s->result;
    int confirmed;

    if (!s->have_usage || f->nfields != 3)
        return -1;
    confirmed = strcmp(f->field[2], "confirmed") == 0;
    if (!confirmed && strcmp(f->field[2], "unconfirmed") != 0)
        return -1;
    r->cleanup = confirmed ? ETUDE_CLEANUP_CONFIRMED : ETUDE_CLEANUP_UNCONFIRMED;
    s->have_cleaned = 1;
    if (s->damaged) {
        end_op(s, ETUDE_SERIAL_DAMAGED,
               "the output of attempt %s does not match the sizes and digests of its streams",
               s->attempt);
        return 0;
    }
    r->complete = s->sizes_known[0] && s->sizes_known[1];
    end_op(s, ETUDE_SERIAL_DONE, "");
    return 0;
}

/** Take one event of the run going on, or the state that answers its cancel. */
static void take_run(struct etude_serial *s, const struct wire_frame *f, uint64_t cmd)
{
    const char *t = f->type;
    int ok = -1;

    if (f->nfields < 2 || strcmp(f->field[1], s->attempt) != 0) {
        protocol(s, f, "of another attempt");
        return;
    }
    if (cmd == s->cancel_cmd) {
        if (strcmp(t, ETUDE_EV_STATE) != 0 || f->nfields != 3)
            protocol(s, f, "where the state of the cancel was due");
        return;
    }
    if (strcmp(t, ETUDE_EV_STARTED) == 0)
        ok = take_started(s, f);
    else if (strcmp(t, ETUDE_EV_OUT) == 0)
        ok = take_out(s, f);
    else if (strcmp(t, ETUDE_EV_EXIT) == 0)
        ok = take_exit(s, f);
    else if (strcmp(t, ETUDE_EV_STREAM) == 0)
        ok = take_stream(s, f);
    else if (strcmp(t, ETUDE_EV_USAGE) == 0)
        ok = take_usage(s, f);
    else if (strcmp(t, ETUDE_EV_CLEANED) == 0)
        ok = take_cleaned(s, f);
    if (ok != 0)
        protocol(s, f, "out of the order of an attempt's events");
}

/**
 * Act on an event received in order. An event of an earlier operation's
 * command is passed by; one of a command that was never sent, and one that
 * does not follow in the operation going on, end it PROTOCOL.
 */
static void take_event(struct etude_serial *s, const struct wire_frame *f)
{
    uint64_t cmd;

    if (s->status != ETUDE_SERIAL_BUSY)
        return;
    if (f->nfields < 1 || !wire_number(f->field[0], &cmd) || cmd == 0 || cmd >= s->next_cmd) {
        protocol(s, f, "for no command that was sent");
        return;
    }
    if (cmd != s->cmd && cmd != s->cancel_cmd)
        return;
    if (strcmp(f->type, ETUDE_EV_ERROR) == 0) {
        if (f->nfields != 2)
            protocol(s, f, "with no code");
        else if (cmd != s->cancel_cmd)
            end_op(s, ETUDE_SERIAL_REFUSED, "%s %s %s", f->type, f->field[0], f->field[1]);
        return;
    }
    switch (s->op) {
    case OP_OPEN:
        take_ready(s, f);
        break;
    case OP_PUT:
        take_stored(s, f);
        break;
    case OP_GET:
        take_get(s, f);
        break;
    case OP_RUN:
        take_run(s, f, cmd);
        break;
    case OP_BYE:
        take_bye(s, f);
        break;
    case OP_NONE:
        break;
    }
}

/**
 * Say whether the `len` bytes at `p` are a frame of the session as
 * etude-guest wrote it: its session and boot are this session's and its CRC
 * is right, whether or not its fields can be read.
 */
static int whole_frame(const struct etude_serial *s, const char *p, size_t len)
{
    const size_t tok = ETUDE_FRAME_TOKEN_LEN;
    char crc[9];

    if (len < 4 + 2 * (tok + 1) + 10 || p[len - 10] != ' ' || p[len - 9] != '*' ||
        memcmp(p + 4, s->session, tok) != 0 || p[4 + tok] != ' ' ||
        memcmp(p + 5 + tok, s->boot, tok) != 0 || p[5 + 2 * tok] != ' ')
        return 0;
    memcpy(crc, p + len - 8, 8);
    crc[8] = '\0';
    return wire_hex(crc, 8) && strtoul(crc, NULL, 16) == wire_crc32(p, len - 9);
}

/**
 * Take one line of the console. Its frame, if it has one, begins at its last
 * `#E1 `, since kernel text may precede it. A frame of the session from the
 * boot of etude-guest's alive is taken: its acknowledgement releases this
 * side's frames, and its event is acted on when it is the next in order. A
 * frame of the session with a right CRC whose fields do not parse, such as
 * an `exit` of more fields than a frame may have, ends the operation
 * PROTOCOL; a damaged frame is passed by.
 */
static void take_line(struct etude_serial *s, uint64_t now_us)
{
    struct wire_frame f;
    size_t len = s->line_len, i, at = 0;
    int found = 0, intact;

    if (len > 0 && s->line[len - 1] == '\r')
        len--;
    for (i = 0; i + 4 <= len; i++)
        if (memcmp(s->line + i, ETUDE_FRAME_MAGIC " ", 4) == 0) {
            at = i;
            found = 1;
        }
    if (!found || len - at >= ETUDE_FRAME_SERIAL_MAX)
        return;
    intact = s->live && whole_frame(s, s->line + at, len - at);
    if (wire_parse(s->line + at, len - at, &f) != 0) {
        if (intact)
            end_op(s, ETUDE_SERIAL_PROTOCOL,
                   "etude-guest sent a frame of a form this controller does not read");
        return;
    }
    if (strcmp(f.type, ETUDE_EV_ALIVE) == 0) {
        take_alive(s, &f, now_us);
        return;
    }
    if (!s->live || strcmp(f.boot, s->boot) != 0 || strcmp(f.session, s->session) != 0 ||
        (f.has_ack && f.ack >= s->seq_wire))
        return;
    if (f.has_ack)
        release(s, f.ack);
    if (strcmp(f.type, ETUDE_CMD_ACK) == 0)
        return;
    s->need_ack = 1;
    if (f.seq != s->recv_next)
        return;
    s->recv_next++;
    take_event(s, &f);
}

struct etude_serial *etude_serial_new(const struct etude_serial_conf *conf)
{
    struct etude_serial *s = calloc(1, sizeof(*s));

    if (s == NULL)
        return NULL;
    s->conf = *conf;
    token_of(conf->run_id, s->session);
    memcpy(s->boot, ETUDE_FRAME_NO_SESSION, sizeof(s->boot));
    s->next_cmd = 1;
    s->status = ETUDE_SERIAL_IDLE;
    s->result.end = ETUDE_END_LOST;
    s->result.value = -1;
    s->result.cpu_us.method = s->result.peak_bytes.method = "no run was made";
    if (capture_init(&s->result.out, conf->output_max) != 0 ||
        capture_init(&s->result.err, conf->output_max) != 0) {
        capture_free(&s->result.out);
        free(s);
        return NULL;
    }
    return s;
}

void etude_serial_free(struct etude_serial *s)
{
    if (s == NULL)
        return;
    capture_free(&s->result.out);
    capture_free(&s->result.err);
    free(s);
}

void etude_serial_feed(struct etude_serial *s, const char *buf, size_t n, uint64_t now_us)
{
    size_t i;

    for (i = 0; i < n && !s->closed; i++) {
        if (buf[i] == '\n') {
            take_line(s, now_us);
            s->line_len = 0;
            continue;
        }
        if (s->line_len == LINE_KEEP) {
            memmove(s->line, s->line + LINE_KEEP - (ETUDE_FRAME_SERIAL_MAX - 1),
                    ETUDE_FRAME_SERIAL_MAX - 1);
            s->line_len = ETUDE_FRAME_SERIAL_MAX - 1;
        }
        s->line[s->line_len++] = buf[i];
    }
    if (s->closed)
        return;
    pump(s, now_us);
    if (s->need_ack && s->live)
        send_ack(s);
    s->need_ack = 0;
}

void etude_serial_closed(struct etude_serial *s)
{
    s->closed = 1;
    lose(s, "the console closed");
}

void etude_serial_tick(struct etude_serial *s, uint64_t now_us)
{
    size_t i;

    if (s->closed || s->nsent == 0 || now_us < s->q[0].sent_us + s->conf.resend_us)
        return;
    for (i = 0; i < s->nsent; i++) {
        if (send_raw(s, s->q[i].text, s->q[i].len) != 0)
            return;
        s->q[i].sent_us = now_us;
    }
}

uint64_t etude_serial_deadline(const struct etude_serial *s)
{
    if (s->closed || s->nsent == 0)
        return 0;
    return s->q[0].sent_us + s->conf.resend_us;
}

int etude_serial_open(struct etude_serial *s, uint64_t now_us)
{
    if (!may_begin(s, 0))
        return -1;
    begin(s, OP_OPEN);
    s->hello_sent = 0;
    if (s->alive_fresh)
        send_hello(s, now_us);
    return 0;
}

static int begin_put(struct etude_serial *s, const char *name, uint64_t size, const char *sha256,
                     const char *mode, const void *data,
                     int (*read)(void *, uint64_t, void *, size_t), void *ctx, uint64_t now_us)
{
    char cmd[24], sz[24];
    const char *a[5];

    if (!may_begin(s, 1))
        return -1;
    if (strlen(name) >= sizeof(s->name) || !printable(name) || !wire_hex(sha256, 64) ||
        !printable(mode)) {
        errno = EINVAL;
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->next_cmd);
    snprintf(sz, sizeof(sz), "%llu", (unsigned long long)size);
    a[0] = cmd;
    a[1] = name;
    a[2] = sz;
    a[3] = sha256;
    a[4] = mode;
    if (queue_frame(s, ETUDE_CMD_PUT, 5, a) != 0)
        return -1;
    begin(s, OP_PUT);
    s->cmd = s->next_cmd++;
    snprintf(s->name, sizeof(s->name), "%s", name);
    memcpy(s->sha, sha256, sizeof(s->sha));
    s->data = data;
    s->file_read = read;
    s->read_ctx = ctx;
    s->size = size;
    s->at = 0;
    s->sealed = 0;
    pump(s, now_us);
    return 0;
}

int etude_serial_put(struct etude_serial *s, const char *name, uint64_t size, const char *sha256,
                     const char *mode, const void *data, uint64_t now_us)
{
    return begin_put(s, name, size, sha256, mode, data, NULL, NULL, now_us);
}

int etude_serial_put_reader(struct etude_serial *s, const char *name, uint64_t size,
                            const char *sha256, const char *mode,
                            int (*read)(void *, uint64_t, void *, size_t), void *ctx,
                            uint64_t now_us)
{
    if (read == NULL) {
        errno = EINVAL;
        return -1;
    }
    return begin_put(s, name, size, sha256, mode, NULL, read, ctx, now_us);
}

int etude_serial_get(struct etude_serial *s, const char *name,
                     int (*write)(void *, const void *, size_t), void *ctx, uint64_t now_us)
{
    char cmd[24];
    const char *a[2];

    if (!may_begin(s, 1))
        return -1;
    if (strlen(name) >= sizeof(s->name) || !printable(name) || write == NULL) {
        errno = EINVAL;
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->next_cmd);
    a[0] = cmd;
    a[1] = name;
    if (queue_frame(s, ETUDE_CMD_GET, 2, a) != 0)
        return -1;
    begin(s, OP_GET);
    s->cmd = s->next_cmd++;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->file_write = write;
    s->file_ctx = ctx;
    s->at = 0;
    sha256_init(&s->got_sha[0]);
    pump(s, now_us);
    return 0;
}

int etude_serial_run(struct etude_serial *s, const char *attempt, const char *invocation,
                     uint64_t now_us)
{
    struct etude_serial_result *r = &s->result;
    char cmd[24];
    const char *a[3];
    int fd;

    if (!may_begin(s, 1))
        return -1;
    if (!wire_hex(attempt, ETUDE_FRAME_TOKEN_LEN) || !wire_hex(invocation, 64)) {
        errno = EINVAL;
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->next_cmd);
    a[0] = cmd;
    a[1] = attempt;
    a[2] = invocation;
    if (queue_frame(s, ETUDE_CMD_RUN, 3, a) != 0)
        return -1;
    capture_free(&r->out);
    capture_free(&r->err);
    memset(r, 0, sizeof(*r));
    if (capture_init(&r->out, s->conf.output_max) != 0 ||
        capture_init(&r->err, s->conf.output_max) != 0) {
        capture_free(&r->out);
        s->nq--;
        s->seq_out--;
        errno = ENOMEM;
        return -1;
    }
    r->end = ETUDE_END_LOST;
    r->value = -1;
    r->cleanup = ETUDE_CLEANUP_PENDING;
    r->cpu_us.method = r->peak_bytes.method = "the end did not arrive";
    begin(s, OP_RUN);
    s->cmd = s->next_cmd++;
    memcpy(s->attempt, attempt, sizeof(s->attempt));
    s->have_exit = s->have_usage = s->have_cleaned = 0;
    s->damaged = 0;
    for (fd = 0; fd < 2; fd++) {
        s->have_stream[fd] = s->sizes_known[fd] = 0;
        s->got[fd] = 0;
        s->sent[fd] = 0;
        sha256_init(&s->got_sha[fd]);
    }
    pump(s, now_us);
    return 0;
}

int etude_serial_cancel(struct etude_serial *s, uint64_t now_us)
{
    char cmd[24];
    const char *a[2];

    if (s->closed) {
        errno = EPIPE;
        return -1;
    }
    if (s->op != OP_RUN || s->status != ETUDE_SERIAL_BUSY) {
        errno = EINVAL;
        return -1;
    }
    if (s->cancel_cmd != 0)
        return 0;
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->next_cmd);
    a[0] = cmd;
    a[1] = s->attempt;
    if (queue_frame(s, ETUDE_CMD_CANCEL, 2, a) != 0)
        return -1;
    s->cancel_cmd = s->next_cmd++;
    pump(s, now_us);
    return 0;
}

int etude_serial_bye(struct etude_serial *s, uint64_t now_us)
{
    char cmd[24];
    const char *a[1];

    if (!may_begin(s, 1))
        return -1;
    snprintf(cmd, sizeof(cmd), "%llu", (unsigned long long)s->next_cmd);
    a[0] = cmd;
    if (queue_frame(s, ETUDE_CMD_BYE, 1, a) != 0)
        return -1;
    begin(s, OP_BYE);
    s->cmd = s->next_cmd++;
    pump(s, now_us);
    return 0;
}

enum etude_serial_status etude_serial_status(const struct etude_serial *s)
{
    return s->status;
}

const char *etude_serial_why(const struct etude_serial *s)
{
    return s->why;
}

const struct etude_serial_result *etude_serial_result(const struct etude_serial *s)
{
    return &s->result;
}

int etude_serial_guest(const struct etude_serial *s, struct etude_pid *pid,
                       char boot[ETUDE_FRAME_TOKEN_LEN + 1])
{
    if (!s->guest_seen)
        return -1;
    *pid = s->guest;
    memcpy(boot, s->boot, ETUDE_FRAME_TOKEN_LEN + 1);
    return 0;
}
