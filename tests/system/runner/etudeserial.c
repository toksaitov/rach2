/**
 * etudeserial: boot the test image and drive etude-guest over Rach2's serial
 * console, as etude's controller does, without the debug-exit port.
 *
 * Usage: etudeserial --kernel K --initrd I --hello H --ish S --yes Y
 *                    [--log FILE] [--timeout S]
 *
 * It boots the architecture's QEMU, RACH2_QEMU or else qemu-system-i386,
 * headless with the kernel and the test RAM disk, waits for the kernel's
 * debug shell, and has it `exec /etude-guest`, which becomes the boot's
 * designated init and speaks frames on the console. The host files H, S and
 * Y are the test image's /hello, its /ish and utils' `yes`, which the image
 * does not stage. Then, each step a line `etudeserial: pass: WHAT` or
 * `etudeserial: fail: WHAT: DETAIL`:
 *
 *   1. hello and ready, with the serial frame bound of 255 bytes;
 *   2. /hello and /ish stored from the image without a transfer, and `yes`
 *      transferred in data frames and checked by its digest on Rach2;
 *   3. a pass: hello's output and its exit status 0;
 *   4. the same run command again, answered by the same events, and its
 *      number with other fields refused;
 *   5. an intended failure: `ish -c 'exit 3'` exits 3;
 *   6. an interruption: `yes` cancelled while it writes, ended as stopped,
 *      with every process gone, and status reading that end again;
 *   7. the designated-init exit: exec runs `ish -c 'exit 5'` in
 *      etude-guest's place, and the kernel's record says that init process,
 *      etude-guest's PID, exited with status 5.
 *
 * Lines of the console that are not frames are the kernel's, kept in the
 * transcript and searched for its records. Frames this end sends are each
 * acknowledged before the next, and sent again after two seconds without
 * an acknowledgement. --timeout bounds the run, 300 seconds unless given,
 * multiplied by the set's deadline factor. The exit status is 0 when every
 * step passed, 1 when one failed, and 2 for a usage error or a run that
 * could not start.
 */
#define _POSIX_C_SOURCE 200809L
#include "wire.h"
#include <etude/frame.h>
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The session of this run, and the bound of the frames it sends. */
static const char session[] = "e7de5e5510000001";
#define FRAME_MAX ETUDE_FRAME_SERIAL_MAX

/* QEMU, the ends of its serial pipes, and the transcript. */
static pid_t qemu = -1;
static int q_in = -1;
static int q_out = -1;
static FILE *logf;

/* The run's end, and the deadline factor every bound is multiplied by. */
static double deadline;
static double factor = 1.0;

/* Every byte of the console that was not a frame, for the kernel's records. */
static char *console;
static size_t console_len;
static size_t console_cap;

/* The line being read. */
static char line[4096];
static size_t line_len;

/* The guest's boot, PID and frame state, and this end's. */
static char boot[ETUDE_FRAME_TOKEN_LEN + 1] = "0000000000000000";
static long guest_pid;
static int alive_seen;
static unsigned long long tx_seq;
static long long peer_ack = -1;
static unsigned long long rx_next;
static int rx_any;
static unsigned long cmd_no = 1;

/* The guest's events, in order, not yet taken. */
struct event {
    char type[16];
    int nfields;
    char field[WIRE_FIELDS_MAX][256];
    struct event *next;
};
static struct event *events;
static struct event **events_tail = &events;

static int failures;

/** Seconds on the monotonic clock. */
static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** Report one step. */
static void report(int ok, const char *what, const char *fmt, ...)
{
    char detail[512] = "";
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    if (ok) {
        printf("etudeserial: pass: %s\n", what);
    } else {
        printf("etudeserial: fail: %s: %s\n", what, detail);
        failures++;
    }
    fflush(stdout);
}

/** Stop QEMU if it runs, and reap it. */
static void stop_qemu(void)
{
    int i;

    if (qemu <= 0)
        return;
    kill(qemu, SIGTERM);
    for (i = 0; i < 30 && waitpid(qemu, NULL, WNOHANG) == 0; i++) {
        struct timespec tenth = {0, 100000000};
        nanosleep(&tenth, NULL);
    }
    if (i == 30) {
        kill(qemu, SIGKILL);
        waitpid(qemu, NULL, 0);
    }
    qemu = -1;
}

/** Exit through the cleanup after a signal. */
static void stopped(int sig)
{
    exit(128 + sig);
}

/** Keep `n` bytes of console text. */
static void console_add(const char *p, size_t n)
{
    if (console_len + n + 1 > console_cap) {
        console_cap = (console_len + n + 1) * 2;
        console = realloc(console, console_cap);
        if (!console) {
            fprintf(stderr, "etudeserial: out of memory\n");
            exit(2);
        }
    }
    memcpy(console + console_len, p, n);
    console_len += n;
    console[console_len] = '\0';
}

/** Write a frame to the console. */
static void put_frame(unsigned long long seq, const char *type, int n, const char *const *field)
{
    char text[FRAME_MAX + 1];
    size_t len = wire_encode(text, sizeof(text), session, boot, seq, rx_any,
                             rx_any ? rx_next - 1 : 0, type, n, field);

    if (len == 0) {
        fprintf(stderr, "etudeserial: a %s frame does not fit\n", type);
        exit(2);
    }
    if (write(q_in, text, len) != (ssize_t)len)
        fprintf(stderr, "etudeserial: the console took no frame\n");
}

/** Act on one line of the console: a frame of the guest, or the kernel's
 * text. A frame can follow kernel text on one line, so it is looked for
 * anywhere in the line. */
static void take_line(char *text, size_t len)
{
    struct wire_frame f;
    char *at = NULL;
    char *p;
    int i;

    if (len && text[len - 1] == '\r')
        len--;
    text[len] = '\0';
    for (p = strstr(text, ETUDE_FRAME_MAGIC " "); p; p = strstr(p + 1, ETUDE_FRAME_MAGIC " "))
        at = p;
    if (at) {
        console_add(text, (size_t)(at - text));
        len -= (size_t)(at - text);
    }
    if (!at || wire_parse(at, len, &f) < 0) {
        console_add(at ? at : text, strlen(at ? at : text));
        console_add("\n", 1);
        return;
    }
    if (strcmp(f.type, ETUDE_EV_ALIVE) == 0) {
        if (!alive_seen && f.nfields == 3) {
            snprintf(boot, sizeof(boot), "%s", f.boot);
            guest_pid = atol(f.field[1]);
            alive_seen = 1;
        }
        return;
    }
    if (strcmp(f.boot, boot) != 0 || strcmp(f.session, session) != 0)
        return;
    if (f.has_ack && (long long)f.ack > peer_ack)
        peer_ack = (long long)f.ack;
    if (strcmp(f.type, ETUDE_CMD_ACK) == 0)
        return;
    if (f.seq == rx_next) {
        struct event *e = calloc(1, sizeof(*e));
        if (!e)
            exit(2);
        snprintf(e->type, sizeof(e->type), "%s", f.type);
        e->nfields = f.nfields;
        for (i = 0; i < f.nfields; i++)
            snprintf(e->field[i], sizeof(e->field[i]), "%s", f.field[i]);
        *events_tail = e;
        events_tail = &e->next;
        rx_next++;
        rx_any = 1;
    }
    /* Every sequenced frame, a repeat and one past a gap included, is
     * answered with the last frame received in order. */
    put_frame(tx_seq, ETUDE_CMD_ACK, 0, NULL);
}

/** Read the console until `until`, or for one wait of 0.2 seconds when
 * `until` has passed. */
static void pump(double until)
{
    double stop = until < deadline ? until : deadline;
    char buf[4096];

    do {
        struct pollfd p = {q_out, POLLIN, 0};
        double left = stop - now();
        ssize_t r;
        size_t i;

        if (poll(&p, 1, left > 0.2 ? 200 : left > 0 ? (int)(left * 1000) + 1 : 0) <= 0)
            continue;
        r = read(q_out, buf, sizeof(buf));
        if (r <= 0)
            return;
        if (logf)
            fwrite(buf, 1, (size_t)r, logf);
        for (i = 0; i < (size_t)r; i++) {
            if (buf[i] == '\n') {
                take_line(line, line_len);
                line_len = 0;
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = buf[i];
            }
        }
    } while (now() < stop);
}

/** Wait up to `seconds` for `text` in the console after `from`; its end, or
 * -1. Before etude-guest starts the console is plain text, and a line not
 * yet ended is looked at too. */
static long wait_text(const char *text, size_t from, double seconds)
{
    double until = now() + seconds * factor;

    for (;;) {
        char *p;
        if (from <= console_len && (p = strstr(console + from, text)))
            return (long)(p - console) + (long)strlen(text);
        line[line_len] = '\0';
        if (strstr(line, text))
            return (long)console_len;
        if (now() >= until || now() >= deadline)
            return -1;
        pump(now() + 0.2);
    }
}

/**
 * Send a sequenced frame and wait until the guest acknowledges it, sending
 * it again every two seconds; 0, or -1 when `seconds` pass first.
 */
static int send_frame(double seconds, const char *type, ...)
{
    const char *field[WIRE_FIELDS_MAX];
    unsigned long long seq = tx_seq++;
    double until = now() + seconds * factor;
    va_list ap;
    int n = 0;

    va_start(ap, type);
    for (;;) {
        const char *a = va_arg(ap, const char *);
        if (!a)
            break;
        field[n++] = a;
    }
    va_end(ap);
    while (now() < until && now() < deadline) {
        double resend = now() + 2.0;
        put_frame(seq, type, n, field);
        while (peer_ack < (long long)seq && now() < resend && now() < until)
            pump(now() + 0.2);
        if (peer_ack >= (long long)seq)
            return 0;
    }
    return -1;
}

/** Take the guest's next event within `seconds`, or NULL. The caller frees
 * it. */
static struct event *next_event(double seconds)
{
    double until = now() + seconds * factor;
    struct event *e;

    while (!events && now() < until && now() < deadline)
        pump(now() + 0.2);
    e = events;
    if (e) {
        events = e->next;
        if (!events)
            events_tail = &events;
    }
    return e;
}

/** The next event, required to be of `type` and for command `cmd`; NULL
 * with the step failed otherwise. */
static struct event *expect(const char *what, const char *type, const char *cmd)
{
    struct event *e = next_event(60);

    if (!e) {
        report(0, what, "no %s event", type);
        return NULL;
    }
    if (strcmp(e->type, type) != 0 || strcmp(e->field[0], cmd) != 0) {
        report(0, what, "%s %s where %s %s was due", e->type, e->nfields ? e->field[0] : "", type,
               cmd);
        free(e);
        return NULL;
    }
    return e;
}

/** The size and the SHA-256 of the host file `path`; 0 or -1. */
static int file_digest(const char *path, long *size, char hex[65])
{
    unsigned char buf[8192];
    unsigned char d[32];
    struct sha256 s;
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return -1;
    sha256_init(&s);
    *size = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&s, buf, n);
        *size += (long)n;
    }
    fclose(f);
    sha256_final(&s, d);
    sha256_hex(d, hex);
    return 0;
}

/** The next command number, as text in `out`. */
static const char *new_cmd(char out[24])
{
    snprintf(out, 24, "%lu", cmd_no++);
    return out;
}

/**
 * Store `n` bytes as `name`: put, data frames unless `staged`, and seal,
 * which `stored` answers with the digest. Returns 0 or -1.
 */
static int store(const char *what, const char *name, const unsigned char *bytes, long n,
                 const char *hex, int staged)
{
    char c[24];
    char size[24];
    char off[24];
    char b64[128];
    struct event *e;
    long i;

    snprintf(size, sizeof(size), "%ld", n);
    if (send_frame(30, ETUDE_CMD_PUT, new_cmd(c), name, size, hex, "0755", (char *)NULL) < 0) {
        report(0, what, "put was not acknowledged");
        return -1;
    }
    for (i = 0; !staged && i < n; i += 72) {
        long m = n - i < 72 ? n - i : 72;
        wire_base64(bytes + i, (size_t)m, b64);
        snprintf(off, sizeof(off), "%ld", i);
        if (send_frame(30, ETUDE_CMD_DATA, c, off, b64, (char *)NULL) < 0) {
            report(0, what, "data at %ld was not acknowledged", i);
            return -1;
        }
    }
    if (send_frame(30, ETUDE_CMD_SEAL, c, (char *)NULL) < 0) {
        report(0, what, "seal was not acknowledged");
        return -1;
    }
    e = expect(what, ETUDE_EV_STORED, c);
    if (!e)
        return -1;
    if (strcmp(e->field[2], hex) != 0) {
        report(0, what, "stored %s, not %s", e->field[2], hex);
        free(e);
        return -1;
    }
    free(e);
    return 0;
}

/* A program of the scenario: its name on Rach2, the host's copy, and the
 * digest and size of that copy once it is stored. */
struct program {
    const char *name;
    const char *path;
    char hex[65];
    long size;
};

/** Store the host file of `p` under its name, from the image's own copy
 * when `staged`; its digest and size are kept in `p`. */
static int store_file(const char *what, struct program *p, int staged)
{
    unsigned char *bytes;
    FILE *f;
    int rc;

    if (file_digest(p->path, &p->size, p->hex) < 0) {
        report(0, what, "cannot read %s", p->path);
        return -1;
    }
    bytes = malloc((size_t)p->size + 1);
    f = fopen(p->path, "rb");
    if (!bytes || !f || fread(bytes, 1, (size_t)p->size, f) != (size_t)p->size) {
        report(0, what, "cannot read %s", p->path);
        free(bytes);
        if (f)
            fclose(f);
        return -1;
    }
    fclose(f);
    rc = store(what, p->name, bytes, p->size, p->hex, staged);
    free(bytes);
    return rc;
}

/** Store an invocation of the stored program `p` for the attempt `token`;
 * its digest goes to `inv`. `args` is the JSON text of the arguments. */
static int store_invocation(const char *what, const char *token, const char *role,
                            const struct program *p, const char *args, char inv[65])
{
    char text[2048];
    unsigned char d[32];
    struct sha256 s;
    int n;

    n = snprintf(text, sizeof(text),
                 "{\"schema_version\": 1, \"run_id\": \"etudeserial\", "
                 "\"attempt_id\": \"etudeserial-%s\", \"attempt_token\": \"%s\", "
                 "\"case_id\": \"guest.etude.serial\", \"program\": \"%s\", \"arguments\": %s, "
                 "\"environment\": {\"PATH\": \"/bin:/\", \"HOME\": \"${WORKSPACE}\"}, "
                 "\"stdin\": null, \"role\": \"%s\", \"files\": [{\"name\": \"%s\", "
                 "\"sha256\": \"%s\", \"size_bytes\": %ld, \"mode\": \"0755\"}], "
                 "\"fixtures\": [{\"fixture\": \"workspace\", \"given\": []}], "
                 "\"deadline_us\": 120000000, \"grace_us\": 5000000, "
                 "\"output_max_bytes\": 4096}",
                 token, token, p->name, args, role, p->name, p->hex, p->size);
    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)text, (size_t)n);
    sha256_final(&s, d);
    sha256_hex(d, inv);
    return store(what, "invocation.json", (const unsigned char *)text, n, inv, 0);
}

/* What an attempt reported, from started to cleaned. */
struct ending {
    char pid[24];
    char start[24];
    char out[4097];
    char kind[16];
    char value[16];
    char cleaned[16];
};

/** Read an attempt's events after started until cleaned; 0 or -1. The
 * output of descriptor 1 is kept. */
static int collect(const char *what, const char *cmd, struct ending *r)
{
    struct event *e;

    for (;;) {
        e = next_event(60);
        if (!e) {
            report(0, what, "the attempt did not end");
            return -1;
        }
        if (strcmp(e->field[0], cmd) != 0) {
            report(0, what, "%s for command %s", e->type, e->field[0]);
            free(e);
            return -1;
        }
        if (strcmp(e->type, ETUDE_EV_STARTED) == 0) {
            snprintf(r->pid, sizeof(r->pid), "%.23s", e->field[2]);
            snprintf(r->start, sizeof(r->start), "%.23s", e->field[3]);
        } else if (strcmp(e->type, ETUDE_EV_OUT) == 0 && strcmp(e->field[2], "1") == 0) {
            unsigned char bytes[256];
            long n = wire_unbase64(e->field[4], bytes);
            size_t off = strtoul(e->field[3], NULL, 10);
            if (n > 0 && off == strlen(r->out) && off + (size_t)n < sizeof(r->out)) {
                memcpy(r->out + off, bytes, (size_t)n);
                r->out[off + (size_t)n] = '\0';
            }
        } else if (strcmp(e->type, ETUDE_EV_EXIT) == 0) {
            snprintf(r->kind, sizeof(r->kind), "%.15s", e->field[2]);
            snprintf(r->value, sizeof(r->value), "%.15s", e->field[3]);
        } else if (strcmp(e->type, ETUDE_EV_CLEANED) == 0) {
            snprintf(r->cleaned, sizeof(r->cleaned), "%.15s", e->field[2]);
            free(e);
            return 0;
        } else if (strcmp(e->type, ETUDE_EV_ERROR) == 0) {
            report(0, what, "error %s", e->field[1]);
            free(e);
            return -1;
        }
        free(e);
    }
}

/** Start QEMU with the kernel and the image, its serial console on pipes. */
static int boot_qemu(const char *kernel, const char *initrd)
{
    const char *q = getenv("RACH2_QEMU");
    char *argv[] = {(char *)(q && *q ? q : "qemu-system-i386"),
                    "-cpu",
                    "max",
                    "-kernel",
                    (char *)kernel,
                    "-initrd",
                    (char *)initrd,
                    "-serial",
                    "stdio",
                    "-display",
                    "none",
                    "-rtc",
                    "base=utc",
                    "-no-reboot",
                    "-nic",
                    "none",
                    NULL};
    int in[2];
    int out[2];

    if (pipe(in) < 0 || pipe(out) < 0)
        return -1;
    qemu = fork();
    if (qemu < 0)
        return -1;
    if (qemu == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[1]);
        close(out[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    q_in = in[1];
    q_out = out[0];
    return 0;
}

static struct program hello = {"hello", NULL, "", 0};
static struct program ish = {"ish", NULL, "", 0};
static struct program yes = {"yes", NULL, "", 0};

/** Start etude-guest from the debug shell and open the session. */
static int open_session(void)
{
    double until = now() + 60 * factor;
    struct event *e;
    char c[24];

    if (wait_text("Type 'help'", 0, 60) < 0) {
        fprintf(stderr, "etudeserial: the kernel's debug shell never appeared\n");
        return -1;
    }
    pump(now() + 0.5);
    dprintf(q_in, "exec /etude-guest --stage / --resend-ms 1000\n");
    while (!alive_seen && now() < until && now() < deadline)
        pump(now() + 0.2);
    report(alive_seen, "etude-guest says it is alive", "no alive frame");
    if (!alive_seen ||
        send_frame(30, ETUDE_CMD_HELLO, new_cmd(c), "1", "65536", (char *)NULL) < 0 ||
        !(e = expect("hello is answered by ready", ETUDE_EV_READY, c)))
        return -1;
    report(strcmp(e->field[2], "255") == 0 && atol(e->field[3]) == guest_pid,
           "ready keeps the serial frame bound", "ready %s %s", e->field[2], e->field[3]);
    free(e);
    return 0;
}

/** Store the programs: two the image stages, and one transferred. */
static void step_files(void)
{
    report(store_file("the image's /hello is stored without a transfer", &hello, 1) == 0,
           "the image's /hello is stored without a transfer", "");
    report(store_file("the image's /ish is stored without a transfer", &ish, 1) == 0,
           "the image's /ish is stored without a transfer", "");
    report(store_file("yes is transferred and checked on Rach2", &yes, 0) == 0,
           "yes is transferred and checked on Rach2", "");
}

/** Run `p` with `args` as attempt `token`, as command `c`, and collect its
 * ending in `r`; 0, or -1 after a failure is reported. */
static int run_case(const char *what, const char *token, const struct program *p, const char *args,
                    char c[24], struct ending *r)
{
    char inv[65];

    memset(r, 0, sizeof(*r));
    if (store_invocation(what, token, "ordinary", p, args, inv) < 0 ||
        send_frame(30, ETUDE_CMD_RUN, new_cmd(c), token, inv, (char *)NULL) < 0)
        return -1;
    return collect(what, c, r);
}

/** A pass, the same command again, and its number with other fields. */
static void step_pass(void)
{
    struct ending r;
    struct ending again;
    struct event *e;
    char inv[65];
    char c[24];

    memset(&r, 0, sizeof(r));
    if (store_invocation("a pass", "00000000000000a1", "ordinary", &hello, "[]", inv) < 0 ||
        send_frame(30, ETUDE_CMD_RUN, new_cmd(c), "00000000000000a1", inv, (char *)NULL) < 0 ||
        collect("a pass", c, &r) < 0)
        return;
    report(strstr(r.out, "Hello from ELF!") && strcmp(r.kind, "exited") == 0 &&
               strcmp(r.value, "0") == 0 && strcmp(r.cleaned, "confirmed") == 0,
           "a pass reports its output and exit status 0", "%s %s %s, output %s", r.kind, r.value,
           r.cleaned, r.out);
    memset(&again, 0, sizeof(again));
    if (send_frame(30, ETUDE_CMD_RUN, c, "00000000000000a1", inv, (char *)NULL) == 0 &&
        collect("a repeated run", c, &again) == 0)
        report(strcmp(again.pid, r.pid) == 0 && strcmp(again.start, r.start) == 0 &&
                   strcmp(again.out, r.out) == 0 && strcmp(again.kind, r.kind) == 0,
               "a repeated run is answered by the same events, the same process",
               "started %s %s, then %s %s", r.pid, r.start, again.pid, again.start);
    if (send_frame(30, ETUDE_CMD_RUN, c, "00000000000000a2", inv, (char *)NULL) == 0 &&
        (e = expect("a reused command number is refused", ETUDE_EV_ERROR, c))) {
        report(strcmp(e->field[1], ETUDE_ERR_CONFLICT) == 0, "a reused command number is refused",
               "error %s", e->field[1]);
        free(e);
    }
}

/** An intended failure. */
static void step_failure(void)
{
    struct ending r;
    char c[24];

    if (run_case("an intended failure", "00000000000000b1", &ish, "[\"-c\", \"exit 3\"]", c, &r) ==
        0)
        report(strcmp(r.kind, "exited") == 0 && strcmp(r.value, "3") == 0 &&
                   strcmp(r.cleaned, "confirmed") == 0,
               "an intended failure reports exit status 3", "%s %s %s", r.kind, r.value, r.cleaned);
}

/** Cancel attempt `token`, started by command `c`, as command `c2`, and
 * read its end among its own frames. */
static void cancel(const char *token, const char *c, char c2[24])
{
    struct ending r;
    struct event *e;
    int state_seen = 0;

    memset(&r, 0, sizeof(r));
    if (send_frame(30, ETUDE_CMD_CANCEL, new_cmd(c2), token, (char *)NULL) < 0)
        return;
    while ((e = next_event(60)) != NULL) {
        if (strcmp(e->type, ETUDE_EV_STATE) == 0 && strcmp(e->field[0], c2) == 0)
            state_seen = strcmp(e->field[2], "running") == 0;
        else if (strcmp(e->type, ETUDE_EV_EXIT) == 0 && strcmp(e->field[0], c) == 0)
            snprintf(r.kind, sizeof(r.kind), "%.15s", e->field[2]);
        else if (strcmp(e->type, ETUDE_EV_CLEANED) == 0 && strcmp(e->field[0], c) == 0)
            snprintf(r.cleaned, sizeof(r.cleaned), "%.15s", e->field[2]);
        free(e);
        if (r.cleaned[0])
            break;
    }
    report(state_seen && strcmp(r.kind, "stopped") == 0 && strcmp(r.cleaned, "confirmed") == 0,
           "a cancelled program is stopped and every process is gone", "state %d, %s %s",
           state_seen, r.kind, r.cleaned);
}

/** An interruption, and status after it. */
static void step_interruption(void)
{
    const char *token = "00000000000000c1";
    struct event *e;
    char inv[65];
    char c[24];
    char c2[24];
    int ok;

    if (store_invocation("an interruption", token, "ordinary", &yes, "[]", inv) < 0 ||
        send_frame(30, ETUDE_CMD_RUN, new_cmd(c), token, inv, (char *)NULL) < 0 ||
        !(e = expect("an interruption", ETUDE_EV_STARTED, c)))
        return;
    free(e);
    free(expect("an interrupted program writes", ETUDE_EV_OUT, c));
    cancel(token, c, c2);
    if (send_frame(30, ETUDE_CMD_STATUS, new_cmd(c2), token, (char *)NULL) < 0 ||
        !(e = expect("status reads the end again", ETUDE_EV_STATE, c2)))
        return;
    ok = strcmp(e->field[2], "cleaned") == 0;
    free(e);
    e = expect("status reads the end again", ETUDE_EV_EXIT, c);
    ok = ok && e && strcmp(e->field[2], "stopped") == 0;
    free(e);
    free(expect("status reads the end again", ETUDE_EV_USAGE, c));
    free(expect("status reads the end again", ETUDE_EV_CLEANED, c));
    report(ok, "status reads the end again", "");
}

/** The designated-init exit. The kernel's record can come in the same read
 * as the started frame, so it is looked for from before the exec. */
static void step_init(void)
{
    size_t from = console_len;
    struct event *e;
    char inv[65];
    char c[24];
    char text[128];

    if (store_invocation("the designated-init exit", "00000000000000d1", "init", &ish,
                         "[\"-c\", \"exit 5\"]", inv) < 0 ||
        send_frame(30, ETUDE_CMD_EXEC, new_cmd(c), "00000000000000d1", inv, (char *)NULL) < 0 ||
        !(e = expect("the designated-init exit", ETUDE_EV_STARTED, c)))
        return;
    report(atol(e->field[2]) == guest_pid, "exec runs the program in etude-guest's process",
           "started %s, etude-guest %ld", e->field[2], guest_pid);
    free(e);
    snprintf(text, sizeof(text), "init process %ld exited with status 5", guest_pid);
    report(wait_text(text, from, 60) >= 0,
           "the kernel records the designated init's exit with its status",
           "no \"%s\" on the console", text);
}

/* The command line's paths and the run's time limit in seconds. */
struct options {
    const char *kernel;
    const char *initrd;
    const char *log;
    double timeout;
};

/** Read the command line into `o` and the programs' paths; 0, or -1 after
 * a usage message. */
static int parse_options(int argc, char **argv, struct options *o)
{
    const char **path;
    int i;

    for (i = 1; i < argc; i += 2) {
        path = strcmp(argv[i], "--kernel") == 0   ? &o->kernel
               : strcmp(argv[i], "--initrd") == 0 ? &o->initrd
               : strcmp(argv[i], "--hello") == 0  ? &hello.path
               : strcmp(argv[i], "--ish") == 0    ? &ish.path
               : strcmp(argv[i], "--yes") == 0    ? &yes.path
               : strcmp(argv[i], "--log") == 0    ? &o->log
                                                  : NULL;
        if (i + 1 >= argc || (!path && strcmp(argv[i], "--timeout") != 0)) {
            fprintf(stderr, "etudeserial: %s: unknown option or no value\n", argv[i]);
            return -1;
        }
        if (path)
            *path = argv[i + 1];
        else
            o->timeout = atof(argv[i + 1]);
    }
    if (!o->kernel || !o->initrd || !hello.path || !ish.path || !yes.path) {
        fprintf(stderr, "etudeserial: --kernel, --initrd, --hello, --ish and --yes are required\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct options o = {NULL, NULL, NULL, 300};
    const char *f = getenv("RACH2_DEADLINE_FACTOR");
    const char *mode = getenv("RACH2_DEADLINES");

    if (parse_options(argc, argv, &o) < 0)
        return 2;
    if (mode && strcmp(mode, "scaled") == 0 && f && atof(f) >= 1.0 && atof(f) <= 64.0)
        factor = atof(f);
    deadline = now() + o.timeout * factor;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    atexit(stop_qemu);
    if (o.log && !(logf = fopen(o.log, "wb")))
        fprintf(stderr, "etudeserial: cannot write %s; no transcript\n", o.log);
    console_add("", 0);
    if (boot_qemu(o.kernel, o.initrd) < 0) {
        fprintf(stderr, "etudeserial: QEMU did not start: %s\n", strerror(errno));
        return 2;
    }
    if (open_session() < 0)
        return failures ? 1 : 2;
    step_files();
    step_pass();
    step_failure();
    step_interruption();
    step_init();
    if (logf)
        fclose(logf);
    stop_qemu();
    printf("etudeserial: result: %s: %d failed\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
