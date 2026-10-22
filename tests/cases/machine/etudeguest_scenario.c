/*
 * etudeguest_scenario.c: the scenario of the etudeguest case
 * (../machine.etudeguest.json): etude-guest started at the test image's debug
 * shell as the boot's designated init, with no debug-exit port, and driven
 * over the serial console one frame at a time, as etude's controller drives it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/frame.h"
#include "lifecycle.h"
#include "sha256.h"
#include "wire.h"

/* The session of every attempt, and the bound of the frames sent. */
static const char session[] = "e7de5e5510000001";
#define FRAME_MAX ETUDE_FRAME_SERIAL_MAX

/* The bytes each data frame carries, whose base64 fits a frame. */
#define DATA_BYTES 72

/* The record by which the kernel hands its debug shell back. */
static const char handback[] = ", returning to kernel shell";

/* One event of etude-guest's, in the order it came, not yet taken. */
struct event {
    char type[16];
    int nfields;
    char field[WIRE_FIELDS_MAX][256];
    struct event *next;
};

/* A program the scenario stores on Rach2: its name there, the artifact of
 * the host's copy, and that copy's digest and size once it is read. */
struct program {
    const char *name;
    const char *artifact;
    char hex[65];
    long size;
};

/* The session with etude-guest on the console of machine `guest`. */
struct eg {
    struct lc l;
    struct etude_case_machine *m;
    uint64_t read;                        /* console offset of the next line to take */
    char boot[ETUDE_FRAME_TOKEN_LEN + 1]; /* the boot token of etude-guest's alive */
    long guest_pid;                       /* etude-guest's PID, from its alive */
    int alive_seen;
    unsigned long long tx_seq;  /* the SEQ of the next sequenced frame sent */
    long long peer_ack;         /* the last SEQ etude-guest acknowledged; -1 for none */
    unsigned long long rx_next; /* the SEQ of the next frame taken in order */
    int rx_any;                 /* a sequenced frame has been taken */
    unsigned long cmd_no;       /* the number of the next command */
    struct event *events;
    struct event **tail;
    char why[200]; /* why the exchange under way failed; empty while none did */
    struct program hello, ish, yes;
};

/* What an attempt reported, from started to cleaned. */
struct ending {
    char pid[24];
    char start[24];
    char out[4097];
    char kind[16];
    char value[16];
    char cleaned[16];
    int sizes_ok; /* stream 1 gives the size and digest of the output that came */
    int usage_ok; /* usage gives each value, or -, with a method */
};

/**
 * Say why the exchange under way failed: the reason the scenario cannot go
 * on, when it cannot, and otherwise the text `fmt` gives. The first reason
 * stays. @return -1.
 */
static int fault(struct eg *g, const char *fmt, ...)
{
    va_list ap;

    if (g->why[0] != '\0')
        return -1;
    if (!lc_ok(&g->l)) {
        snprintf(g->why, sizeof(g->why), "%s", g->l.why);
        return -1;
    }
    va_start(ap, fmt);
    vsnprintf(g->why, sizeof(g->why), fmt, ap);
    va_end(ap);
    return -1;
}

/**
 * Report assertion `id` as `held`, with the reason fault() kept, and clear
 * it for the next exchange. Once the executor has stopped the case nothing is
 * reported, and lc_finish() gives what is left as not run.
 */
static void report(struct eg *g, const char *id, int held)
{
    if (!held && g->why[0] == '\0')
        fault(g, "etude-guest's answer was not the one due");
    if (held || !g->l.stopped)
        lc_assert(&g->l, id, held, g->why);
    g->why[0] = '\0';
}

/** Type one frame at the console. @return 1 when it was typed. */
static int put_frame(struct eg *g, unsigned long long seq, const char *type, int n,
                     const char *const *field)
{
    char text[FRAME_MAX + 1];
    size_t len = wire_encode(text, sizeof(text), session, g->boot, seq, g->rx_any,
                             g->rx_any ? g->rx_next - 1 : 0, type, n, field);

    if (len == 0) {
        lc_halt(&g->l, ETUDE_FAILED, "a frame past the serial bound");
        return 0;
    }
    return lc_type(&g->l, g->m, text);
}

/** Keep the event of frame `f` after the others not yet taken. */
static void keep_event(struct eg *g, const struct wire_frame *f)
{
    struct event *e = calloc(1, sizeof(*e));
    int i;

    if (e == NULL) {
        lc_halt(&g->l, ETUDE_FAILED, "the room for an event");
        return;
    }
    snprintf(e->type, sizeof(e->type), "%s", f->type);
    e->nfields = f->nfields;
    for (i = 0; i < f->nfields; i++)
        snprintf(e->field[i], sizeof(e->field[i]), "%s", f->field[i]);
    *g->tail = e;
    g->tail = &e->next;
    g->rx_next++;
    g->rx_any = 1;
}

/**
 * Act on one line of the console: a frame of etude-guest's, or the kernel's
 * text, which is passed over. A frame can follow kernel text on one line, so
 * the last frame magic of the line is where a frame would begin.
 */
static void take_line(struct eg *g, char *text, size_t len)
{
    struct wire_frame f;
    char *at = NULL, *p;

    if (len > 0 && text[len - 1] == '\n')
        len--;
    if (len > 0 && text[len - 1] == '\r')
        len--;
    text[len] = '\0';
    for (p = strstr(text, ETUDE_FRAME_MAGIC " "); p != NULL;
         p = strstr(p + 1, ETUDE_FRAME_MAGIC " "))
        at = p;
    if (at == NULL || wire_parse(at, len - (size_t)(at - text), &f) < 0)
        return;
    if (strcmp(f.type, ETUDE_EV_ALIVE) == 0) {
        if (!g->alive_seen && f.nfields == 3) {
            snprintf(g->boot, sizeof(g->boot), "%s", f.boot);
            g->guest_pid = atol(f.field[1]);
            g->alive_seen = 1;
        }
        return;
    }
    if (strcmp(f.boot, g->boot) != 0 || strcmp(f.session, session) != 0)
        return;
    if (f.has_ack && (long long)f.ack > g->peer_ack)
        g->peer_ack = (long long)f.ack;
    if (strcmp(f.type, ETUDE_CMD_ACK) == 0)
        return;
    if (f.seq == g->rx_next)
        keep_event(g, &f);
    /* Every sequenced frame, a repeat and one past a gap included, is
     * answered with the last frame taken in order. */
    put_frame(g, g->tx_seq, ETUDE_CMD_ACK, 0, NULL);
}

/**
 * Wait for the console's next whole line and take it.
 *
 * The scenario resends nothing: it has one frame of at most 255 bytes in
 * flight at a time, which Rach2's serial receive ring of 4096 bytes takes
 * whole, and a frame etude-guest never answers fails the step at its bound.
 * @return 1 when a line was taken.
 */
static int pump(struct eg *g)
{
    uint64_t at = g->read;
    char *line;

    if (!lc_wait(&g->l, g->m, "\n", &at, NULL) ||
        (line = lc_console(&g->l, "guest", g->read, at)) == NULL)
        return 0;
    take_line(g, line, (size_t)(at - g->read));
    free(line);
    g->read = at;
    return lc_ok(&g->l);
}

/**
 * Send a sequenced frame of `type` with the fields that follow, ended by a
 * null pointer, and wait until etude-guest acknowledges it. @return 0, or -1
 * with the reason kept.
 */
static int send_frame(struct eg *g, const char *type, ...)
{
    const char *field[WIRE_FIELDS_MAX] = {NULL};
    unsigned long long seq = g->tx_seq++;
    const char *a;
    va_list ap;
    int n = 0;

    va_start(ap, type);
    while (n < WIRE_FIELDS_MAX && (a = va_arg(ap, const char *)) != NULL)
        field[n++] = a;
    va_end(ap);
    if (!put_frame(g, seq, type, n, field))
        return fault(g, "the %s frame was not typed", type);
    while (g->peer_ack < (long long)seq)
        if (!pump(g))
            return fault(g, "the %s frame was not acknowledged", type);
    return 0;
}

/** Take etude-guest's next event, or NULL when the scenario cannot go on.
 * The caller frees it. */
static struct event *next_event(struct eg *g)
{
    struct event *e;

    while (g->events == NULL)
        if (!pump(g))
            return NULL;
    e = g->events;
    g->events = e->next;
    if (g->events == NULL)
        g->tail = &g->events;
    return e;
}

/** The next event, required to be of `type` and for command `cmd`; NULL
 * with the reason kept otherwise. */
static struct event *expect(struct eg *g, const char *type, const char *cmd)
{
    struct event *e = next_event(g);

    if (e == NULL) {
        fault(g, "no %s event", type);
        return NULL;
    }
    if (strcmp(e->type, type) != 0 || strcmp(e->field[0], cmd) != 0) {
        fault(g, "%s %s where %s %s was due", e->type, e->nfields > 0 ? e->field[0] : "", type,
              cmd);
        free(e);
        return NULL;
    }
    return e;
}

/** Write the SHA-256 of `n` bytes at `p` in hexadecimal to `hex`. */
static void digest(const void *p, size_t n, char hex[65])
{
    unsigned char d[32];
    struct sha256 s;

    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, d);
    sha256_hex(d, hex);
}

/** Give the next command number as text in `out`. */
static const char *new_cmd(struct eg *g, char out[24])
{
    snprintf(out, 24, "%lu", g->cmd_no++);
    return out;
}

/**
 * Store `n` bytes as `name`: put, data frames unless `staged`, and seal,
 * which `stored` answers with the digest `hex`. @return 0, or -1 with the
 * reason kept.
 */
static int store(struct eg *g, const char *name, const unsigned char *bytes, long n,
                 const char *hex, int staged)
{
    char c[24], size[24], off[24], b64[128];
    struct event *e;
    long i;

    snprintf(size, sizeof(size), "%ld", n);
    if (send_frame(g, ETUDE_CMD_PUT, new_cmd(g, c), name, size, hex, "0755", (char *)NULL) < 0)
        return -1;
    for (i = 0; !staged && i < n; i += DATA_BYTES) {
        wire_base64(bytes + i, (size_t)(n - i < DATA_BYTES ? n - i : DATA_BYTES), b64);
        snprintf(off, sizeof(off), "%ld", i);
        if (send_frame(g, ETUDE_CMD_DATA, c, off, b64, (char *)NULL) < 0)
            return -1;
    }
    if (send_frame(g, ETUDE_CMD_SEAL, c, (char *)NULL) < 0 ||
        (e = expect(g, ETUDE_EV_STORED, c)) == NULL)
        return -1;
    if (strcmp(e->field[2], hex) != 0)
        fault(g, "%s was stored as %s, not %s", name, e->field[2], hex);
    free(e);
    return g->why[0] == '\0' ? 0 : -1;
}

/** Read the whole of file `path` into `*bytes`, which the caller frees.
 * @return its size, or -1. */
static long read_whole(const char *path, unsigned char **bytes)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    long n = -1;

    *bytes = NULL;
    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (*bytes = malloc((size_t)n + 1)) != NULL && fread(*bytes, 1, (size_t)n, f) == (size_t)n) {
        fclose(f);
        return n;
    }
    fclose(f);
    free(*bytes);
    *bytes = NULL;
    return -1;
}

/** Store the host's copy of `p` under its name, from the image's own copy
 * when `staged`; its digest and size are kept in `p`. @return 0 or -1. */
static int store_file(struct eg *g, struct program *p, int staged)
{
    unsigned char *bytes;
    int rc;

    p->size = read_whole(etude_artifact(g->l.c, p->artifact), &bytes);
    if (p->size < 0)
        return fault(g, "artifact %s cannot be read", p->artifact);
    digest(bytes, (size_t)p->size, p->hex);
    rc = store(g, p->name, bytes, p->size, p->hex, staged);
    free(bytes);
    return rc;
}

/**
 * Store an invocation of the stored program `p` with the arguments `args`,
 * JSON text, in the role `role` for the attempt `token`; its digest goes to
 * `inv`. @return 0 or -1.
 */
static int store_invocation(struct eg *g, const char *token, const char *role,
                            const struct program *p, const char *args, char inv[65])
{
    char text[2048];
    int n;

    n = snprintf(text, sizeof(text),
                 "{\"schema_version\": 1, \"run_id\": \"etudeguest\", "
                 "\"attempt_id\": \"etudeguest-%s\", \"attempt_token\": \"%s\", "
                 "\"case_id\": \"%s\", \"program\": \"%s\", \"arguments\": %s, "
                 "\"environment\": {\"PATH\": \"/bin:/\", \"HOME\": \"${WORKSPACE}\"}, "
                 "\"stdin\": null, \"role\": \"%s\", \"files\": [{\"name\": \"%s\", "
                 "\"sha256\": \"%s\", \"size_bytes\": %ld, \"mode\": \"0755\"}], "
                 "\"fixtures\": [{\"fixture\": \"workspace\", \"given\": []}], "
                 "\"deadline_us\": 120000000, \"grace_us\": 5000000, "
                 "\"output_max_bytes\": 4096}",
                 token, token, etude_case_id(g->l.c), p->name, args, role, p->name, p->hex,
                 p->size);
    if (n < 0 || (size_t)n >= sizeof(text))
        return fault(g, "the invocation of %s does not fit", p->name);
    digest(text, (size_t)n, inv);
    return store(g, "invocation.json", (const unsigned char *)text, n, inv, 0);
}

/** Say whether `value` and `method` are a measurement of `usage`: a number, a
 * number and `+`, or `-`, and a method. */
static int measure_ok(const char *value, const char *method)
{
    size_t n = strspn(value, "0123456789");

    return (strcmp(value, "-") == 0 ||
            (n > 0 && (value[n] == '\0' || strcmp(value + n, "+") == 0))) &&
           method[0] != '\0';
}

/** Add the output of an `out` event `e` of descriptor 1 to `r`, in order. */
static void add_output(struct ending *r, const struct event *e)
{
    unsigned char bytes[256];
    long n = wire_unbase64(e->field[4], bytes);
    size_t off = strtoul(e->field[3], NULL, 10);

    if (n > 0 && off == strlen(r->out) && off + (size_t)n < sizeof(r->out)) {
        memcpy(r->out + off, bytes, (size_t)n);
        r->out[off + (size_t)n] = '\0';
    }
}

/** Keep in `r` what event `e` of an attempt says. */
static void take_event(struct ending *r, const struct event *e)
{
    char hex[65];

    if (strcmp(e->type, ETUDE_EV_STARTED) == 0) {
        snprintf(r->pid, sizeof(r->pid), "%.23s", e->field[2]);
        snprintf(r->start, sizeof(r->start), "%.23s", e->field[3]);
    } else if (strcmp(e->type, ETUDE_EV_OUT) == 0 && strcmp(e->field[2], "1") == 0) {
        add_output(r, e);
    } else if (strcmp(e->type, ETUDE_EV_EXIT) == 0) {
        snprintf(r->kind, sizeof(r->kind), "%.15s", e->field[2]);
        snprintf(r->value, sizeof(r->value), "%.15s", e->field[3]);
    } else if (strcmp(e->type, ETUDE_EV_STREAM) == 0 && e->nfields == 6 &&
               strcmp(e->field[2], "1") == 0) {
        digest(r->out, strlen(r->out), hex);
        r->sizes_ok =
            strtoul(e->field[3], NULL, 10) == strlen(r->out) && strcmp(e->field[5], hex) == 0;
    } else if (strcmp(e->type, ETUDE_EV_USAGE) == 0) {
        r->usage_ok = e->nfields == 6 && measure_ok(e->field[2], e->field[3]) &&
                      measure_ok(e->field[4], e->field[5]);
    } else if (strcmp(e->type, ETUDE_EV_CLEANED) == 0) {
        snprintf(r->cleaned, sizeof(r->cleaned), "%.15s", e->field[2]);
    }
}

/** Read the events of the attempt of command `cmd` into `r` until its
 * cleaned. @return 0, or -1 with the reason kept. */
static int collect(struct eg *g, const char *cmd, struct ending *r)
{
    struct event *e;

    memset(r, 0, sizeof(*r));
    while (r->cleaned[0] == '\0') {
        if ((e = next_event(g)) == NULL)
            return fault(g, "the attempt of command %s did not end", cmd);
        if (strcmp(e->field[0], cmd) != 0 || strcmp(e->type, ETUDE_EV_ERROR) == 0) {
            fault(g, "%s %s %s while command %s ran", e->type, e->field[0], e->field[1], cmd);
            free(e);
            return -1;
        }
        take_event(r, e);
        free(e);
    }
    return 0;
}

/** Run the stored program `p` with `args` as attempt `token`, by command
 * `c`, and collect its ending in `r`. @return 0 or -1. */
static int run_case(struct eg *g, const char *token, const struct program *p, const char *args,
                    char c[24], struct ending *r)
{
    char inv[65];

    if (store_invocation(g, token, "ordinary", p, args, inv) < 0 ||
        send_frame(g, ETUDE_CMD_RUN, new_cmd(g, c), token, inv, (char *)NULL) < 0)
        return -1;
    return collect(g, c, r);
}

/** Start etude-guest at the debug shell and open the session: assertions
 * `alive` and `ready`. @return 1 when the session is open. */
static int open_session(struct eg *g)
{
    struct event *e;
    char c[24];
    int ok;

    if (!lc_type(&g->l, g->m, "exec /etude-guest --stage / --resend-ms 1000\n"))
        return 0;
    while (!g->alive_seen)
        if (!pump(g))
            break;
    if (!g->alive_seen)
        fault(g, "etude-guest sent no alive frame");
    report(g, "alive", g->alive_seen);
    if (!g->alive_seen ||
        send_frame(g, ETUDE_CMD_HELLO, new_cmd(g, c), "1", "65536", (char *)NULL) < 0 ||
        (e = expect(g, ETUDE_EV_READY, c)) == NULL) {
        report(g, "ready", 0);
        return 0;
    }
    ok = strcmp(e->field[2], "255") == 0 && atol(e->field[3]) == g->guest_pid;
    if (!ok)
        fault(g, "ready gives the bound %s and the PID %s, where alive gave %ld", e->field[2],
              e->field[3], g->guest_pid);
    free(e);
    report(g, "ready", ok);
    return ok;
}

/** Store the programs: two the image stages, and one transferred. */
static void step_files(struct eg *g)
{
    report(g, "staged-hello", store_file(g, &g->hello, 1) == 0);
    report(g, "staged-ish", store_file(g, &g->ish, 1) == 0);
    report(g, "transfer", store_file(g, &g->yes, 0) == 0);
}

/** A pass of the invocation `inv`, NULL when it was not stored, and its
 * stream and usage: assertions `pass`, `stream` and `usage`. @return 0, or
 * -1 when the pass did not run. */
static int pass(struct eg *g, const char *inv, char c[24], struct ending *r)
{
    int ok;

    if (inv == NULL ||
        send_frame(g, ETUDE_CMD_RUN, new_cmd(g, c), "00000000000000a1", inv, (char *)NULL) < 0 ||
        collect(g, c, r) < 0) {
        report(g, "pass", 0);
        fault(g, "the pass did not run");
        report(g, "stream", 0);
        fault(g, "the pass did not run");
        report(g, "usage", 0);
        return -1;
    }
    ok = strstr(r->out, "Hello from ELF!") != NULL && strcmp(r->kind, "exited") == 0 &&
         strcmp(r->value, "0") == 0 && strcmp(r->cleaned, "confirmed") == 0;
    if (!ok)
        fault(g, "hello ended %s %s, cleaned %s, with the output %.80s", r->kind, r->value,
              r->cleaned, r->out);
    report(g, "pass", ok);
    if (!r->sizes_ok)
        fault(g, "the stream of descriptor 1 does not give the size and digest of %.80s", r->out);
    report(g, "stream", r->sizes_ok);
    if (!r->usage_ok)
        fault(g, "usage does not give each value with its method");
    report(g, "usage", r->usage_ok);
    return 0;
}

/** A pass, the same command again, and its number with other fields:
 * assertions `pass`, `stream`, `usage`, `repeat` and `conflict`. */
static void step_pass(struct eg *g)
{
    struct ending r, again;
    struct event *e = NULL;
    char inv[65], c[24];
    int ok;

    ok = store_invocation(g, "00000000000000a1", "ordinary", &g->hello, "[]", inv) == 0;
    if (pass(g, ok ? inv : NULL, c, &r) < 0) {
        fault(g, "the pass did not run");
        report(g, "repeat", 0);
        fault(g, "the pass did not run");
        report(g, "conflict", 0);
        return;
    }
    ok = send_frame(g, ETUDE_CMD_RUN, c, "00000000000000a1", inv, (char *)NULL) == 0 &&
         collect(g, c, &again) == 0;
    if (ok && !(strcmp(again.pid, r.pid) == 0 && strcmp(again.start, r.start) == 0 &&
                strcmp(again.out, r.out) == 0 && strcmp(again.kind, r.kind) == 0)) {
        fault(g, "started %s %s, then %s %s", r.pid, r.start, again.pid, again.start);
        ok = 0;
    }
    report(g, "repeat", ok);
    ok = send_frame(g, ETUDE_CMD_RUN, c, "00000000000000a2", inv, (char *)NULL) == 0 &&
         (e = expect(g, ETUDE_EV_ERROR, c)) != NULL;
    if (ok && strcmp(e->field[1], ETUDE_ERR_CONFLICT) != 0) {
        fault(g, "error %s, not %s", e->field[1], ETUDE_ERR_CONFLICT);
        ok = 0;
    }
    free(e);
    report(g, "conflict", ok);
}

/** An intended failure: assertion `failure`. */
static void step_failure(struct eg *g)
{
    struct ending r;
    char c[24];
    int ok = run_case(g, "00000000000000b1", &g->ish, "[\"-c\", \"exit 3\"]", c, &r) == 0;

    if (ok && !(strcmp(r.kind, "exited") == 0 && strcmp(r.value, "3") == 0 &&
                strcmp(r.cleaned, "confirmed") == 0)) {
        fault(g, "ish ended %s %s, cleaned %s", r.kind, r.value, r.cleaned);
        ok = 0;
    }
    report(g, "failure", ok);
}

/** Cancel attempt `token`, started by command `c`, as command `c2`, and read
 * its end among its own frames: assertion `cancel`. */
static void cancel(struct eg *g, const char *token, const char *c, char c2[24])
{
    char kind[16] = "", cleaned[16] = "";
    struct event *e;
    int state_seen = 0, ok;

    if (send_frame(g, ETUDE_CMD_CANCEL, new_cmd(g, c2), token, (char *)NULL) < 0) {
        report(g, "cancel", 0);
        return;
    }
    while (cleaned[0] == '\0' && (e = next_event(g)) != NULL) {
        if (strcmp(e->type, ETUDE_EV_STATE) == 0 && strcmp(e->field[0], c2) == 0)
            state_seen = strcmp(e->field[2], "running") == 0;
        else if (strcmp(e->type, ETUDE_EV_EXIT) == 0 && strcmp(e->field[0], c) == 0)
            snprintf(kind, sizeof(kind), "%.15s", e->field[2]);
        else if (strcmp(e->type, ETUDE_EV_CLEANED) == 0 && strcmp(e->field[0], c) == 0)
            snprintf(cleaned, sizeof(cleaned), "%.15s", e->field[2]);
        free(e);
    }
    ok = state_seen && strcmp(kind, "stopped") == 0 && strcmp(cleaned, "confirmed") == 0;
    if (!ok)
        fault(g, "state running %s, ended %s, cleaned %s", state_seen ? "seen" : "not seen",
              kind[0] != '\0' ? kind : "unknown", cleaned[0] != '\0' ? cleaned : "unknown");
    report(g, "cancel", ok);
}

/** Read status's answer for the attempt of command `c`, which status command
 * `c2` asked for. @return 1 when it gives the attempt cleaned and stopped. */
static int status_end(struct eg *g, const char *c, const char *c2)
{
    static const char *const rest[] = {ETUDE_EV_STREAM, ETUDE_EV_STREAM, ETUDE_EV_USAGE,
                                       ETUDE_EV_CLEANED};
    struct event *e;
    size_t i;
    int ok;

    if ((e = expect(g, ETUDE_EV_STATE, c2)) == NULL)
        return 0;
    ok = strcmp(e->field[2], "cleaned") == 0;
    free(e);
    if ((e = expect(g, ETUDE_EV_EXIT, c)) == NULL)
        return 0;
    ok = ok && strcmp(e->field[2], "stopped") == 0;
    free(e);
    for (i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
        if ((e = expect(g, rest[i], c)) == NULL)
            return 0;
        free(e);
    }
    if (!ok)
        fault(g, "status does not give the attempt cleaned and stopped");
    return ok;
}

/** An interruption, and status after it: assertions `cancel` and
 * `status`. */
static void step_interruption(struct eg *g)
{
    const char *token = "00000000000000c1";
    struct event *started = NULL, *out = NULL;
    char inv[65], c[24], c2[24];
    int ok;

    if (store_invocation(g, token, "ordinary", &g->yes, "[]", inv) == 0 &&
        send_frame(g, ETUDE_CMD_RUN, new_cmd(g, c), token, inv, (char *)NULL) == 0 &&
        (started = expect(g, ETUDE_EV_STARTED, c)) != NULL)
        out = expect(g, ETUDE_EV_OUT, c);
    ok = out != NULL;
    free(started);
    free(out);
    if (!ok) {
        report(g, "cancel", 0);
        fault(g, "yes did not run");
        report(g, "status", 0);
        return;
    }
    cancel(g, token, c, c2);
    ok = send_frame(g, ETUDE_CMD_STATUS, new_cmd(g, c2), token, (char *)NULL) == 0 &&
         status_end(g, c, c2);
    report(g, "status", ok);
}

/**
 * The designated init's exit: assertions `exec-pid` and `init-exit`. The
 * kernel's record can come before the started frame is taken, so it is
 * looked for from before the exec.
 */
static void step_init(struct eg *g)
{
    uint64_t from = g->read, at;
    struct event *e = NULL;
    char inv[65], c[24], text[128], *w;
    int ok;

    if (store_invocation(g, "00000000000000d1", "init", &g->ish, "[\"-c\", \"exit 5\"]", inv) ==
            0 &&
        send_frame(g, ETUDE_CMD_EXEC, new_cmd(g, c), "00000000000000d1", inv, (char *)NULL) == 0)
        e = expect(g, ETUDE_EV_STARTED, c);
    ok = e != NULL;
    if (ok && atol(e->field[2]) != g->guest_pid) {
        fault(g, "started gives %s, where etude-guest is %ld", e->field[2], g->guest_pid);
        ok = 0;
    }
    free(e);
    report(g, "exec-pid", ok);
    at = from;
    if (!lc_wait(&g->l, g->m, handback, &at, NULL) ||
        (w = lc_console(&g->l, "guest", from, at)) == NULL) {
        report(g, "init-exit", 0);
        return;
    }
    snprintf(text, sizeof(text), "init process %ld exited with status 5%s", g->guest_pid, handback);
    ok = strstr(w, text) != NULL;
    if (!ok)
        fault(g, "the kernel handed its shell back without \"%s\"", text);
    free(w);
    report(g, "init-exit", ok);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"alive",    "ready",    "staged-hello", "staged-ish",
                                      "transfer", "pass",     "stream",       "usage",
                                      "repeat",   "conflict", "failure",      "cancel",
                                      "status",   "exec-pid", "init-exit",    NULL};
    static struct eg g;

    lc_begin(&g.l, c, ids);
    snprintf(g.boot, sizeof(g.boot), "%s", ETUDE_FRAME_NO_SESSION);
    g.peer_ack = -1;
    g.cmd_no = 1;
    g.tail = &g.events;
    g.hello.name = g.hello.artifact = "hello";
    g.ish.name = g.ish.artifact = "ish";
    g.yes.name = g.yes.artifact = "yes";
    if ((g.m = lc_shell(&g.l, "boot", "guest", &g.read)) != NULL && lc_step(&g.l, "session") &&
        open_session(&g) && lc_step(&g.l, "files")) {
        step_files(&g);
        if (lc_step(&g.l, "runs")) {
            step_pass(&g);
            step_failure(&g);
            step_interruption(&g);
        }
        if (lc_step(&g.l, "init")) {
            step_init(&g);
            lc_quit(&g.l, g.m, NULL);
        }
    }
    return lc_finish(&g.l);
}
