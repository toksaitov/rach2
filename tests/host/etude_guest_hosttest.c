/* Drive etude-guest through pipes, as the controller drives it through SSH
 * and the serial console, and check the contracts of its commands. Small
 * prepared native workloads exercise each invocation without an interpreter.
 * host-deps: tools/etude/guest/guest.c tools/etude/guest/fixture.c
 * host-deps: tools/etude/boot_identity.c
 * host-deps: tools/etude/guest/wire.c
 * host-tools: etude-workload
 * host-deps: tools/etude/history_json.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/guest
 * host-cflags: -I utils
 */
#include "wire.h"
#include "sha256.h"
#include <etude/frame.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <grp.h>
#include <pwd.h>
#endif

int etude_guest_main(int argc, char **argv);
extern const char *etude_guest_fault;

/* The bytes of a path, as many as realpath(3) may write on Linux; the
 * freestanding <limits.h> of the host tests' build has no PATH_MAX. */
#define PATH_CAP 4096

#include "compat/tree_host.h"
#include "compat/workload_host.h"

static int failures;
static char work[512];
/* The scratch directory by its resolved path, with no symbolic link and no
 * doubled slash, which is how etude-guest gives a workspace to a program. */
static char resolved[PATH_CAP];
/* Prepared workload bytes, transferred with their explicit length. */
static char *workload;
static size_t workload_size;
static pid_t guest = -1;
static int to_guest = -1;
static int from_guest = -1;

/* The session of the run these tests make, and the boot the guest gave. */
static const char session[] = "5e551011a0000001";
static char boot[ETUDE_FRAME_TOKEN_LEN + 1];
static unsigned long seq;
static unsigned long cmd_no = 1;

/* Over the serial console the test acknowledges each frame it reads, as a
 * controller does, and drops a frame sent again below `seen`. */
static int serial_acks;
static unsigned long long seen;

#define CHECK(test, detail)                                                \
    do {                                                                   \
        if (!(test)) {                                                     \
            fprintf(stderr, "etude_guest_hosttest: failed: %s\n", detail); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* One frame read back, with copies of its type and fields. */
struct got {
    char line[70000];
    char type[16];
    int nfields;
    char field[WIRE_FIELDS_MAX][8192];
    unsigned long long seq;
    int has_ack;
    unsigned long long ack;
};

static char in_buf[200000];
static size_t in_len;

/** Remove the tree at `path` (the test's own scratch). */
static void remove_tree(const char *path)
{
    if (host_remove_tree(path) != 0)
        fprintf(stderr, "etude_guest_hosttest: could not remove %s\n", path);
}

/** Kill and reap the guest before closing its pipes. Closing them first
 * lets it cancel the attempts whose orphaned state the tests need. */
static void end_guest(void)
{
    if (guest > 0) {
        kill(guest, SIGKILL);
        waitpid(guest, NULL, 0);
    }
    guest = -1;
    if (to_guest >= 0)
        close(to_guest);
    if (from_guest >= 0)
        close(from_guest);
    to_guest = from_guest = -1;
}

/* The processes that hold the groups of check_group_records, or -1. */
static long holders[3] = {-1, -1, -1};

/** Kill the processes that hold the groups made by make_group(). */
static void end_groups(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (holders[i] > 0)
            kill((pid_t)holders[i], SIGKILL);
        holders[i] = -1;
    }
}

/** Reap the guest, end the groups made here and remove every fixture, on
 * every exit path. */
static void cleanup(void)
{
    end_guest();
    end_groups();
    free(workload);
    if (*work)
        remove_tree(work);
}

/** Exit through the cleanup after a signal. */
static void stopped(int number)
{
    exit(128 + number);
}

/** Start etude-guest with the arguments given, its input and output on
 * pipes; the state directory is the test's. */
static void start_guest(const char *mode, ...)
{
    int in[2];
    int out[2];
    char *argv[16];
    char state[600];
    va_list ap;
    int n = 0;

    snprintf(state, sizeof(state), "%s/state", work);
    if (pipe(in) < 0 || pipe(out) < 0) {
        perror("pipe");
        exit(2);
    }
    argv[n++] = "etude-guest";
    if (mode)
        argv[n++] = (char *)mode;
    argv[n++] = "--state";
    argv[n++] = state;
    va_start(ap, mode);
    for (;;) {
        char *a = va_arg(ap, char *);
        if (!a)
            break;
        argv[n++] = a;
    }
    va_end(ap);
    argv[n] = NULL;
    guest = fork();
    if (guest == 0) {
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
    to_guest = in[1];
    from_guest = out[0];
    in_len = 0;
    seq = 0;
}

/** Monotonic milliseconds. */
static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/** The time `ms` milliseconds from now, or -1 for no limit when `ms` is
 * negative. */
static long long deadline(int ms)
{
    return ms < 0 ? -1 : now_ms() + ms;
}

/** The milliseconds left before `until`, at least 0, or -1 when `until` is
 * -1, as poll takes them. */
static int left(long long until)
{
    long long now = now_ms();

    return until < 0 ? -1 : until > now ? (int)(until - now) : 0;
}

/** Read one line from the guest within `ms`, or with no limit when `ms` is
 * negative; 0, or -1 at the end or on time. */
static int read_line(char *out, size_t cap, int ms)
{
    long long until = deadline(ms);

    for (;;) {
        char *nl = memchr(in_buf, '\n', in_len);
        struct pollfd p = {from_guest, POLLIN, 0};
        ssize_t r;
        if (nl) {
            size_t n = (size_t)(nl - in_buf);
            if (n >= cap)
                n = cap - 1;
            memcpy(out, in_buf, n);
            out[n] = '\0';
            in_len -= (size_t)(nl - in_buf) + 1;
            memmove(in_buf, nl + 1, in_len);
            return 0;
        }
        if (poll(&p, 1, left(until)) <= 0)
            return -1;
        r = read(from_guest, in_buf + in_len, sizeof(in_buf) - in_len);
        if (r <= 0)
            return -1;
        in_len += (size_t)r;
    }
}

/** Read the next frame within `ms`, or with no limit when `ms` is negative;
 * 0, or -1. A line that is not a frame is an error here, since nothing but
 * frames may come from etude-guest. */
static int next_frame(struct got *g, int ms)
{
    static char copy[sizeof(g->line)];
    struct wire_frame f;
    long long until = deadline(ms);
    int i;

again:
    if (read_line(g->line, sizeof(g->line), left(until)) < 0)
        return -1;
    strcpy(copy, g->line);
    if (wire_parse(copy, strlen(copy), &f) < 0) {
        fprintf(stderr, "etude_guest_hosttest: not a frame: %s\n", g->line);
        failures++;
        return -1;
    }
    snprintf(g->type, sizeof(g->type), "%s", f.type);
    g->nfields = f.nfields;
    for (i = 0; i < f.nfields; i++)
        snprintf(g->field[i], sizeof(g->field[i]), "%s", f.field[i]);
    g->seq = f.seq;
    g->has_ack = f.has_ack;
    g->ack = f.ack;
    if (strcmp(f.type, ETUDE_EV_ALIVE) == 0)
        snprintf(boot, sizeof(boot), "%s", f.boot);
    if (serial_acks && strcmp(f.type, ETUDE_EV_ALIVE) != 0 && strcmp(f.type, ETUDE_CMD_ACK) != 0) {
        char ack[300];
        size_t n =
            wire_encode(ack, sizeof(ack), session, boot, seq, 1, f.seq, ETUDE_CMD_ACK, 0, NULL);
        if (write(to_guest, ack, n) != (ssize_t)n)
            failures++;
        if (f.seq < seen)
            goto again;
        seen = f.seq + 1;
    }
    return 0;
}

/** Read frames until one of `type`, skipping acks; 0, or -1 when another
 * frame or the end comes first. */
static int expect(struct got *g, const char *type)
{
    for (;;) {
        if (next_frame(g, -1) < 0)
            return -1;
        if (strcmp(g->type, ETUDE_CMD_ACK) == 0)
            continue;
        if (strcmp(g->type, type) == 0)
            return 0;
        fprintf(stderr, "etude_guest_hosttest: %s where %s was due: %s\n", g->type, type, g->line);
        return -1;
    }
}

/** Write a frame of the given SEQ, which need not be the next. */
static void send_seq(unsigned long s, const char *type, int n, const char *const *field)
{
    char line[70000];
    size_t len = wire_encode(line, sizeof(line), session, boot, s, 0, 0, type, n, field);

    if (len == 0 || write(to_guest, line, len) != (ssize_t)len) {
        fprintf(stderr, "etude_guest_hosttest: could not send %s\n", type);
        failures++;
    }
}

/** Send a frame with the next SEQ; the fields are a list ending in NULL. */
static void send_cmd(const char *type, ...)
{
    const char *field[WIRE_FIELDS_MAX];
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
    send_seq(seq++, type, n, field);
}

/** The next command number, as text. */
static const char *new_cmd(char out[24])
{
    snprintf(out, 24, "%lu", cmd_no++);
    return out;
}

/** The SHA-256 of `n` bytes, in hexadecimal. */
static void digest(const void *p, size_t n, char hex[65])
{
    struct sha256 s;
    unsigned char d[32];

    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, d);
    sha256_hex(d, hex);
}

/** Take the alive frame and open the session with a hello. */
static void hello(int maxframe)
{
    struct got g;
    char c[24];
    char max[24];

    CHECK(next_frame(&g, -1) == 0 && strcmp(g.type, ETUDE_EV_ALIVE) == 0, "alive comes first");
    snprintf(max, sizeof(max), "%d", maxframe);
    send_cmd(ETUDE_CMD_HELLO, new_cmd(c), "1", max, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_READY) == 0 && strcmp(g.field[0], c) == 0 &&
              strcmp(g.field[1], "1") == 0,
          "hello is answered by ready, version 1");
}

/** Store `n` bytes as file `name` with put, data and seal; the digest goes
 * to `hex` and the put's CMD to `c`. */
static void put_file(const char *name, const char *text, size_t n, const char *mode, char hex[65],
                     char c[24])
{
    struct got g;
    char size[24];
    char off[24];
    char b64[200];
    size_t i;

    digest(text, n, hex);
    snprintf(size, sizeof(size), "%zu", n);
    send_cmd(ETUDE_CMD_PUT, new_cmd(c), name, size, hex, mode, (char *)NULL);
    for (i = 0; i < n; i += 90) {
        size_t m = n - i < 90 ? n - i : 90;
        wire_base64((const unsigned char *)text + i, m, b64);
        snprintf(off, sizeof(off), "%zu", i);
        send_cmd(ETUDE_CMD_DATA, c, off, b64, (char *)NULL);
    }
    send_cmd(ETUDE_CMD_SEAL, c, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STORED) == 0 && strcmp(g.field[0], c) == 0 &&
              strcmp(g.field[2], hex) == 0,
          "a sealed put is stored with its digest");
}

/* The most output the invocations of put_case keep. */
static long case_keep = 65536;

/* The role, deadline, fixtures and input of an invocation. */
struct shape {
    const char *role;
    long deadline_ms;
    const char *fixtures; /* a JSON array */
    const char *input;    /* the text of `input.txt`, its standard input, or NULL */
};

/** Store the compiled workload as `name` and an invocation of shape `sh` for
 * the attempt `token`; the invocation's digest goes to `inv`. */
static void put_invocation(const char *name, const char *workload_role, const char *token,
                           const struct shape *sh, char inv[65])
{
    const char *input = sh->input;
    char text[8192];
    char files[512];
    char hex[65];
    char c[24];
    int n;

    put_file(name, (const char *)workload, workload_size, "0755", hex, c);
    n = snprintf(files, sizeof(files),
                 "{\"name\": \"%s\", \"sha256\": \"%s\", \"size_bytes\": %zu, \"mode\": \"0755\"}",
                 name, hex, workload_size);
    if (input) {
        put_file("input.txt", input, strlen(input), "0644", hex, c);
        snprintf(files + n, sizeof(files) - (size_t)n,
                 ", {\"name\": \"input.txt\", \"sha256\": \"%s\", \"size_bytes\": %zu, "
                 "\"mode\": \"0644\"}",
                 hex, strlen(input));
    }
    n = snprintf(
        text, sizeof(text),
        "{\"schema_version\": 1, \"run_id\": \"etude-guest-hosttest\", "
        "\"attempt_id\": \"attempt-%s\", \"attempt_token\": \"%s\", "
        "\"case_id\": \"host.etude.guest\", \"program\": \"%s\", \"arguments\": [\"a b\"], "
        "\"environment\": {\"PATH\": \"/bin:/usr/bin\", \"HOME\": \"${WORKSPACE}\", "
        "\"MARKS\": \"%s/marks\", \"ETUDE_WORKLOAD_ROLE\": \"%s\"}, \"stdin\": %s, \"role\": "
        "\"%s\", "
        "\"files\": [%s], \"fixtures\": %s, \"deadline_us\": %ld, \"grace_us\": 500000, "
        "\"output_max_bytes\": %ld}",
        token, token, name, work, workload_role, input ? "\"input.txt\"" : "null", sh->role, files,
        sh->fixtures, sh->deadline_ms * 1000, case_keep);
    put_file("invocation.json", text, (size_t)n, "0644", inv, c);
}

/** put_invocation with the workspace alone and no input. */
static void put_case(const char *name, const char *workload_role, const char *token,
                     const char *role, long deadline_ms, char inv[65])
{
    struct shape sh;

    sh.role = role;
    sh.deadline_ms = deadline_ms;
    sh.fixtures = "[{\"fixture\": \"workspace\", \"given\": []}]";
    sh.input = NULL;
    put_invocation(name, workload_role, token, &sh, inv);
}

/** put_invocation of an ordinary case with the fixtures of the JSON array
 * `fixtures` and the standard input `input`, or none when it is NULL. */
static void put_case_with(const char *name, const char *workload_role, const char *token,
                          const char *fixtures, const char *input, char inv[65])
{
    struct shape sh;

    sh.role = "ordinary";
    sh.deadline_ms = 30000;
    sh.fixtures = fixtures;
    sh.input = input;
    put_invocation(name, workload_role, token, &sh, inv);
}

/* The output, the end and the cleanup an attempt reported. */
struct ending {
    char out[4096];
    char err[4096];
    char kind[16];
    char value[16];
    char cleaned[16];
    char total[2][24]; /* the bytes each stream carried, as its stream gives them */
    char cpu[24];
    char cpu_method[64];
    char peak[24];
    int stream_ok[2]; /* a stream's size and digest are those of its output */
    int sizes_ok;     /* both streams' are */
    long pid;
    int events;
};

/** Whether the fields of a `stream` from its size, from `field`, give the
 * size and digest of `text`, and a total no smaller than the size. */
static int stream_agrees(char field[][8192], const char *text)
{
    char hex[65];

    digest(text, strlen(text), hex);
    return strtoull(field[0], NULL, 10) == strlen(text) && strcmp(field[2], hex) == 0 &&
           strspn(field[1], "0123456789") == strlen(field[1]) &&
           strtoull(field[1], NULL, 10) >= strlen(text);
}

/** Whether the `n` fields from `field` are each "-". */
static int unknown(char field[][8192], int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(field[i], "-") != 0)
            return 0;
    return 1;
}

/** Whether the next frames are the `stream` of standard output and then
 * that of standard error, which sent `err_size` bytes. */
static int streams_follow(const char *err_size)
{
    struct got g;

    return expect(&g, ETUDE_EV_STREAM) == 0 && strcmp(g.field[2], "1") == 0 &&
           expect(&g, ETUDE_EV_STREAM) == 0 && strcmp(g.field[2], "2") == 0 &&
           strcmp(g.field[3], err_size) == 0;
}

/** Read an attempt's frames, from started to cleaned, into `e`; 0 or -1.
 * Every frame must fit `bound`. */
static int collect_within(struct ending *e, const char *c, const char *token, size_t bound)
{
    struct got g;

    memset(e, 0, sizeof(*e));
    while (next_frame(&g, -1) == 0) {
        if (strlen(g.line) + 1 > bound) {
            fprintf(stderr, "etude_guest_hosttest: a frame over %zu bytes: %s\n", bound, g.line);
            return -1;
        }
        if (strcmp(g.type, ETUDE_CMD_ACK) == 0)
            continue;
        e->events++;
        if (strcmp(g.field[0], c) != 0 || strcmp(g.field[1], token) != 0) {
            fprintf(stderr, "etude_guest_hosttest: a frame of another attempt: %s\n", g.line);
            return -1;
        }
        if (strcmp(g.type, ETUDE_EV_STARTED) == 0) {
            e->pid = atol(g.field[2]);
        } else if (strcmp(g.type, ETUDE_EV_OUT) == 0) {
            unsigned char bytes[8192];
            char *dst = strcmp(g.field[2], "1") == 0 ? e->out : e->err;
            long n = wire_unbase64(g.field[4], bytes);
            size_t off = strtoul(g.field[3], NULL, 10);
            if (n < 0 || off + (size_t)n >= sizeof(e->out) || off != strlen(dst))
                return -1;
            memcpy(dst + off, bytes, (size_t)n);
            dst[off + (size_t)n] = '\0';
        } else if (strcmp(g.type, ETUDE_EV_EXIT) == 0 && g.nfields == 4) {
            snprintf(e->kind, sizeof(e->kind), "%.15s", g.field[2]);
            snprintf(e->value, sizeof(e->value), "%.15s", g.field[3]);
        } else if (strcmp(g.type, ETUDE_EV_STREAM) == 0 && g.nfields == 6 &&
                   (strcmp(g.field[2], "1") == 0 || strcmp(g.field[2], "2") == 0)) {
            int i = g.field[2][0] - '1';
            e->stream_ok[i] = stream_agrees(g.field + 3, i == 0 ? e->out : e->err);
            snprintf(e->total[i], sizeof(e->total[i]), "%.23s", g.field[4]);
        } else if (strcmp(g.type, ETUDE_EV_USAGE) == 0 && g.nfields == 6) {
            snprintf(e->cpu, sizeof(e->cpu), "%.23s", g.field[2]);
            snprintf(e->cpu_method, sizeof(e->cpu_method), "%.63s", g.field[3]);
            snprintf(e->peak, sizeof(e->peak), "%.23s", g.field[4]);
        } else if (strcmp(g.type, ETUDE_EV_CLEANED) == 0) {
            snprintf(e->cleaned, sizeof(e->cleaned), "%.15s", g.field[2]);
            e->sizes_ok = e->stream_ok[0] && e->stream_ok[1];
            return 0;
        }
    }
    return -1;
}

/** Read an attempt's frames over standard I/O, as collect_within does. */
static int collect(struct ending *e, const char *c, const char *token)
{
    return collect_within(e, c, token, ETUDE_FRAME_STDIO_MAX);
}

/** The number of lines of the marks file, where the workloads count their
 * runs. */
static int marks(void)
{
    char path[600];
    FILE *f;
    int c;
    int n = 0;

    snprintf(path, sizeof(path), "%s/marks", work);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while ((c = fgetc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

/** Whether a process `pid` exists. */
static int alive(long pid)
{
    return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno != ESRCH);
}

/** Read a file whose first line is a PID, as a workload writes one. */
static long read_pid(const char *name)
{
    char path[600];
    FILE *f;
    long pid = 0;

    snprintf(path, sizeof(path), "%s/%s", work, name);
    f = fopen(path, "r");
    if (!f)
        return 0;
    if (fscanf(f, "%ld", &pid) != 1)
        pid = 0;
    fclose(f);
    return pid;
}

/** Open the read end of the FIFO `name` in the scratch directory, which an
 * attempt's workload opens for writing as its descendants' descriptor 9; -1 on
 * failure. Open it before the attempt runs. */
static int watch(const char *name)
{
    char path[600];

    snprintf(path, sizeof(path), "%s/%s", work, name);
    if (mkfifo(path, 0600) < 0 && errno != EEXIST)
        return -1;
    return open(path, O_RDONLY | O_NONBLOCK);
}

/** Wait until no process has the FIFO `fd` open for writing, which is when
 * every process of the attempt has exited; 1 once they have, or 0 when `fd`
 * is bad or a read fails. A read returns 0 once no writer is left. macOS's
 * poll does not report the close of a FIFO's last writer, so poll only paces
 * the reads, at 20 ms. Call it only after the attempt has run, and it closes
 * `fd`. */
static int all_gone(int fd)
{
    struct pollfd p;
    char buf[64];
    ssize_t r = -1;

    while (fd >= 0 && (r = read(fd, buf, sizeof(buf))) != 0 &&
           (r > 0 || errno == EAGAIN || errno == EINTR)) {
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        poll(&p, 1, 20);
    }
    if (fd >= 0)
        close(fd);
    return r == 0;
}

/** Wait for the guest to close its output, then reap it, with its status in
 * `st`; 0, or -1. */
static int wait_guest(int *st)
{
    struct pollfd p;
    char buf[512];
    ssize_t r = -1;

    while (r != 0) {
        p.fd = from_guest;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, -1) <= 0)
            return -1;
        r = read(from_guest, buf, sizeof(buf));
        if (r < 0)
            return -1;
    }
    if (r != 0 || waitpid(guest, st, 0) != guest)
        return -1;
    guest = -1;
    return 0;
}

/** Read an attempt's output frames until its standard output has said
 * `ready`; 0, or -1 when it ends first. */
static int wait_ready(void)
{
    static char text[8192];
    unsigned char bytes[8192];
    struct got *g = malloc(sizeof(*g));
    size_t len = 0;
    int rc = -1;

    while (g && expect(g, ETUDE_EV_OUT) == 0) {
        long n = wire_unbase64(g->field[4], bytes);
        if (n < 0 || strcmp(g->field[2], "1") != 0 || len + (size_t)n >= sizeof(text))
            continue;
        memcpy(text + len, bytes, (size_t)n);
        len += (size_t)n;
        text[len] = '\0';
        if (strstr(text, "ready\n")) {
            rc = 0;
            break;
        }
    }
    free(g);
    return rc;
}

/** End the session with bye and require the guest to exit with `status`. */
static void bye(int status)
{
    struct got g;
    char c[24];
    int st = -1;

    send_cmd(ETUDE_CMD_BYE, new_cmd(c), (char *)NULL);
    CHECK(expect(&g, ETUDE_CMD_BYE) == 0, "bye is answered by bye");
    close(to_guest);
    to_guest = -1;
    CHECK(wait_guest(&st) == 0 && WIFEXITED(st) && WEXITSTATUS(st) == status,
          "etude-guest exits after bye");
    close(from_guest);
    from_guest = -1;
}

/** A pass, an intended failure, a repeated command and a reused number,
 * over standard I/O. */
static void check_pass_fail_repeat(void)
{
    static const char pass[] = "guest-pass";
    static const char fail[] = "guest-fail";
    struct ending e;
    struct got g;
    char inv[65];
    char inv2[65];
    char c[24];
    char c2[24];
    char expected[2048];

    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    put_case("passing", pass, "00000000000000a1", "ordinary", 30000, inv);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000a1", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000a1") == 0, "a passing attempt reports its end");
    snprintf(expected, sizeof(expected),
             "@etude 1 begin host.etude.guest\na b|%s/state/a/00000000000000a1/work|%s/state/a/"
             "00000000000000a1/work\n@etude 1 end 0\n",
             resolved, resolved);
    CHECK(strcmp(e.out, expected) == 0, "the program's output comes back whole, in its workspace");
    CHECK(strcmp(e.kind, "exited") == 0 && strcmp(e.value, "0") == 0, "a pass exits 0");
    CHECK(strcmp(e.cleaned, "confirmed") == 0, "a pass is cleaned");
    CHECK(marks() == 1, "the program ran once");

    /* The same command again: the same events, and no second run. */
    send_cmd(ETUDE_CMD_RUN, c, "00000000000000a1", inv, (char *)NULL);
    {
        struct ending again;
        CHECK(collect(&again, c, "00000000000000a1") == 0 && again.pid == e.pid &&
                  strcmp(again.out, e.out) == 0 && strcmp(again.kind, e.kind) == 0 &&
                  strcmp(again.cleaned, e.cleaned) == 0,
              "a repeated run is answered by the same events");
    }
    CHECK(marks() == 1, "a repeated run does not run the program again");

    /* The number again with other fields, and another command for the same
     * attempt. */
    send_cmd(ETUDE_CMD_RUN, c, "00000000000000a2", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[0], c) == 0 &&
              strcmp(g.field[1], ETUDE_ERR_CONFLICT) == 0,
          "a command number reused with other fields is a conflict");
    send_cmd(ETUDE_CMD_RUN, new_cmd(c2), "00000000000000a1", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[1], ETUDE_ERR_CONFLICT) == 0,
          "a second command for one attempt is a conflict");
    CHECK(marks() == 1, "neither conflict runs the program");

    /* An intended failure keeps its status and its standard error. */
    put_case("failing", fail, "00000000000000b1", "ordinary", 30000, inv2);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000b1", inv2, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000b1") == 0 && strcmp(e.kind, "exited") == 0 &&
              strcmp(e.value, "3") == 0 && strcmp(e.err, "broken\n") == 0 &&
              strcmp(e.cleaned, "confirmed") == 0,
          "an intended failure reports exit 3 and its standard error");

    /* status reads the final events again; remove takes the workspace. */
    send_cmd(ETUDE_CMD_STATUS, new_cmd(c2), "00000000000000b1", (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STATE) == 0 && strcmp(g.field[2], "cleaned") == 0,
          "status of an ended attempt says cleaned");
    CHECK(expect(&g, ETUDE_EV_EXIT) == 0 && strcmp(g.field[0], c) == 0 &&
              strcmp(g.field[3], "3") == 0,
          "status sends the exit again");
    CHECK(streams_follow("7"), "status sends each stream again");
    CHECK(expect(&g, ETUDE_EV_USAGE) == 0 && expect(&g, ETUDE_EV_CLEANED) == 0,
          "status sends usage and cleaned again");
    send_cmd(ETUDE_CMD_GET, new_cmd(c2), "00000000000000b1/out.2", (char *)NULL);
    CHECK(expect(&g, ETUDE_CMD_DATA) == 0 && strcmp(g.field[2], "YnJva2VuCg==") == 0,
          "get sends the file in data frames");
    CHECK(expect(&g, ETUDE_EV_SENT) == 0 && strcmp(g.field[2], "7") == 0,
          "get ends with sent and the size");
    send_cmd(ETUDE_CMD_GET, new_cmd(c2), "00000000000000b1/../00000000000000a1/out.1",
             (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[1], ETUDE_ERR_UNKNOWN) == 0,
          "get refuses a path out of the attempt");
    send_cmd(ETUDE_CMD_REMOVE, new_cmd(c2), "00000000000000b1", (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_REMOVED) == 0, "remove answers removed");
    snprintf(expected, sizeof(expected), "%s/state/a/00000000000000b1/work", work);
    CHECK(access(expected, F_OK) != 0, "remove takes the workspace");

    /* An invocation of the init role is refused over standard I/O. */
    put_case("asinit", fail, "00000000000000c1", "init", 30000, inv2);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000c1", inv2, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[1], ETUDE_ERR_REFUSED) == 0,
          "run refuses the init role");
    send_cmd(ETUDE_CMD_EXEC, new_cmd(c), "00000000000000c1", inv2, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[1], ETUDE_ERR_REFUSED) == 0,
          "exec is refused over standard I/O");

    /* A damaged transfer is not stored. */
    send_cmd(ETUDE_CMD_PUT, new_cmd(c), "damaged", "3", inv, "0644", (char *)NULL);
    send_cmd(ETUDE_CMD_DATA, c, "0", "YWJj", (char *)NULL);
    send_cmd(ETUDE_CMD_SEAL, c, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[1], ETUDE_ERR_DAMAGED) == 0,
          "content that does not match its digest is damaged");
    bye(0);
}

/** The sizes and digests of an attempt's output in its streams, first and
 * repeated, with output past the invocation's bound, and its usage. */
static void check_sizes(void)
{
    static const char both[] = "guest-both";
    static const char flood[] = "guest-flood";
    struct ending e;
    struct ending again;
    char inv[65];
    char c[24];

    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    put_case("both", both, "00000000000000f1", "ordinary", 30000, inv);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000f1", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000f1") == 0 && strcmp(e.out, "out\n") == 0 &&
              strcmp(e.err, "err\n") == 0 && e.sizes_ok,
          "the streams give the sizes and digests of the output sent");
    CHECK(strlen(e.cpu) > 1 && e.cpu[strlen(e.cpu) - 1] == '+' &&
              strcmp(e.cpu_method, "wait-reaped-descendants-v1") == 0 && strlen(e.peak) > 1 &&
              e.peak[strlen(e.peak) - 1] == '+',
          "usage gives lower bounds of the processor time and the peak, with their methods");
    send_cmd(ETUDE_CMD_RUN, c, "00000000000000f1", inv, (char *)NULL);
    CHECK(collect(&again, c, "00000000000000f1") == 0 && again.sizes_ok,
          "a repeated run's exit agrees with the output it sends again");

    /* Output past the bound is not sent, and its stream counts it. */
    case_keep = 1000;
    put_case("flooding", flood, "00000000000000f2", "ordinary", 30000, inv);
    case_keep = 65536;
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000f2", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000f2") == 0 && strlen(e.out) == 1000 &&
              strspn(e.out, "0") == 1000 && e.err[0] == '\0' && e.sizes_ok &&
              strcmp(e.total[0], "1200") == 0 && strcmp(e.total[1], "0") == 0,
          "each stream gives the bytes it carried beyond those sent");
    bye(0);
}

/* A program that ignores SIGTERM and says when it is ready. */
static const char ignore[] = "guest-ignore";

/** A cancellation, a deadline, a child that left its group and a lost
 * connection: each ends the attempt as stopped and leaves no process. */
static void check_interruptions(void)
{
    static const char hang[] = "guest-hang";
    struct ending e;
    struct got g;
    char inv[65];
    char c[24];
    char c2[24];
    long leader;
    long member;
    long escaped;
    int open_end;
    int st;

    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);

    /* cancel: SIGTERM to the group, and the reported child ended too. */
    put_case("hanging", hang, "00000000000000d1", "ordinary", 60000, inv);
    open_end = watch("marks.open");
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d1", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STARTED) == 0, "the hanging attempt starts");
    CHECK(wait_ready() == 0, "the attempt says it is ready");
    leader = read_pid("marks.leader");
    member = read_pid("marks.member");
    escaped = read_pid("marks.escaped");
    CHECK(alive(leader) && alive(member) && alive(escaped), "the attempt's processes run");
    send_cmd(ETUDE_CMD_CANCEL, new_cmd(c2), "00000000000000d1", (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STATE) == 0 && strcmp(g.field[2], "running") == 0,
          "cancel answers the state it found");
    CHECK(collect(&e, c, "00000000000000d1") == 0 && strcmp(e.kind, "stopped") == 0 &&
              strcmp(e.cleaned, "confirmed") == 0,
          "a cancelled attempt is stopped and cleaned");
    CHECK(all_gone(open_end), "cancel leaves no process of the attempt, the reported one included");

    /* The deadline: a program that ignores SIGTERM is killed after the
     * grace period. */
    put_case("stubborn", ignore, "00000000000000d2", "ordinary", 300, inv);
    open_end = watch("marks.open");
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d2", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000d2") == 0 && strcmp(e.kind, "stopped") == 0 &&
              strcmp(e.cleaned, "confirmed") == 0,
          "an attempt past its deadline is stopped, through SIGKILL");
    CHECK(all_gone(open_end), "the stubborn program is gone");

    /* A lost connection: the guest cancels, cleans up and exits 1, and the
     * next connection's status reads the end. */
    put_case("hanging2", hang, "00000000000000d3", "ordinary", 60000, inv);
    open_end = watch("marks.open");
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d3", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STARTED) == 0, "the second hanging attempt starts");
    CHECK(wait_ready() == 0, "the attempt says it is ready");
    leader = read_pid("marks.leader");
    close(to_guest);
    to_guest = -1;
    st = -1;
    CHECK(wait_guest(&st) == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 1,
          "a lost connection ends etude-guest with status 1");
    close(from_guest);
    from_guest = -1;
    CHECK(all_gone(open_end), "a lost connection leaves no process of the attempt");
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    send_cmd(ETUDE_CMD_STATUS, new_cmd(c2), "00000000000000d3", (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STATE) == 0 && strcmp(g.field[2], "cleaned") == 0,
          "the next connection reads the cleaned state");
    CHECK(expect(&g, ETUDE_EV_EXIT) == 0 && strcmp(g.field[0], c) == 0 &&
              strcmp(g.field[2], "stopped") == 0,
          "and the stopped end of the attempt");
    CHECK(expect(&g, ETUDE_EV_STREAM) == 0 && expect(&g, ETUDE_EV_STREAM) == 0, "and its streams");
    CHECK(expect(&g, ETUDE_EV_USAGE) == 0 && expect(&g, ETUDE_EV_CLEANED) == 0 &&
              strcmp(g.field[2], "confirmed") == 0,
          "and its confirmed cleanup");
    /* The run itself, repeated on the new connection, is answered from its
     * record and starts nothing. */
    send_cmd(ETUDE_CMD_RUN, c, "00000000000000d3", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000d3") == 0 && strcmp(e.kind, "stopped") == 0 &&
              e.pid == leader && e.sizes_ok,
          "a run repeated after the connection was lost replays its events");
    bye(0);
}

/** A killed etude-guest records no end: the next one kills the attempt and
 * settles it as lost, its sizes and usage unknown, and its cleanup confirmed
 * once the attempt's processes have ended. */
static void check_orphan(void)
{
    struct got g;
    char inv[65];
    char c[24];
    char c2[24];
    int open_end;

    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    put_case("stubborn2", ignore, "00000000000000d4", "ordinary", 60000, inv);
    open_end = watch("marks.open");
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d4", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STARTED) == 0, "the attempt of a guest to be killed starts");
    CHECK(wait_ready() == 0, "the attempt says it is ready");
    end_guest();
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    send_cmd(ETUDE_CMD_STATUS, new_cmd(c2), "00000000000000d4", (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STATE) == 0, "status answers for an orphaned attempt");
    CHECK(strcmp(g.field[2], "cleaned") == 0,
          "an orphaned attempt whose processes have ended is cleaned");
    CHECK(expect(&g, ETUDE_EV_EXIT) == 0 && g.nfields == 4 && strcmp(g.field[2], "lost") == 0 &&
              unknown(g.field + 3, 1),
          "an orphaned attempt's end is lost");
    CHECK(expect(&g, ETUDE_EV_STREAM) == 0 && g.nfields == 6 && strcmp(g.field[2], "1") == 0 &&
              unknown(g.field + 3, 3) && expect(&g, ETUDE_EV_STREAM) == 0 && g.nfields == 6 &&
              strcmp(g.field[2], "2") == 0 && unknown(g.field + 3, 3),
          "an orphaned attempt's streams are unknown");
    CHECK(expect(&g, ETUDE_EV_USAGE) == 0 && g.nfields == 6 && unknown(g.field + 2, 4),
          "an orphaned attempt's usage is unknown");
    CHECK(expect(&g, ETUDE_EV_CLEANED) == 0 && strcmp(g.field[2], "confirmed") == 0,
          "an orphaned attempt's cleanup is confirmed once its processes have ended");
    CHECK(all_gone(open_end), "the next etude-guest ends an orphaned attempt's processes");
    bye(0);
}

/** Send `status` for `token` and read the six frames of its answer; whether
 * they give an orphaned attempt settled as lost and cleaned, its cleanup
 * confirmed. */
static int settled_confirmed(const char *token)
{
    struct got g;
    char c[24];
    int ok;

    send_cmd(ETUDE_CMD_STATUS, new_cmd(c), token, (char *)NULL);
    ok = expect(&g, ETUDE_EV_STATE) == 0 && strcmp(g.field[2], "cleaned") == 0;
    ok = expect(&g, ETUDE_EV_EXIT) == 0 && strcmp(g.field[2], "lost") == 0 && ok;
    ok = streams_follow("-") && ok;
    ok = expect(&g, ETUDE_EV_USAGE) == 0 && ok;
    return expect(&g, ETUDE_EV_CLEANED) == 0 && strcmp(g.field[2], "confirmed") == 0 && ok;
}

/** A killed etude-guest whose attempt's first process ends after it, and
 * leaves a process in the attempt's group: the next etude-guest kills what
 * is left of the group and confirms the cleanup. A FIFO releases the first
 * process after etude-guest ends; another, opened after the member starts,
 * reports its exit even while its new parent has not reaped it. */
static void check_orphan_group(void)
{
    static const char leaving[] = "guest-leaving";
    struct got g;
    char inv[65];
    char c[24];
    char path[600];
    long leader;
    long member;
    int open_end;
    int leader_end;
    int release;

    snprintf(path, sizeof(path), "%s/marks.release", work);
    CHECK(mkfifo(path, 0600) == 0, "the first process's release FIFO is made");
    release = open(path, O_RDWR | O_NONBLOCK);
    CHECK(release >= 0, "the first process's release FIFO is open");
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    put_case("leaving", leaving, "00000000000000d9", "ordinary", 60000, inv);
    open_end = watch("marks.open");
    leader_end = watch("marks.leader-open");
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d9", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STARTED) == 0,
          "the attempt whose first process leaves its group starts");
    CHECK(wait_ready() == 0, "the attempt says it is ready");
    leader = read_pid("marks.leader");
    member = read_pid("marks.member");
    CHECK(alive(leader) && alive(member), "the first process and its group member run");
    end_guest();
    CHECK(release >= 0 && write(release, "exit\n", 5) == 5,
          "the first process is released after etude-guest ends");
    CHECK(all_gone(leader_end), "the first process ends after etude-guest");
    CHECK(alive(member), "the ended first process leaves a process in its group");
    if (release >= 0)
        close(release);
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    CHECK(settled_confirmed("00000000000000d9"),
          "an orphaned attempt whose first process has ended has its group killed and its cleanup "
          "confirmed");
    CHECK(all_gone(open_end),
          "the next etude-guest ends the process an orphaned attempt left in its group");
    bye(0);
}

/**
 * Make a process group whose first process has ended, the shape an attempt
 * leaves: a first process makes a group of its own, and a session of its
 * own too when `own_session` is set, starts a second, and exits. The second
 * holds the FIFO `name` of the scratch directory open for writing and
 * sleeps. The group's number goes to `*group`; the second's PID, or -1.
 */
static long make_group(const char *name, int own_session, long *group)
{
    char path[600];
    long pid = -1;
    pid_t first;
    int p[2];
    int fd;
    int i;

    snprintf(path, sizeof(path), "%s/%s", work, name);
    if (pipe(p) < 0)
        return -1;
    first = fork();
    if (first == 0) {
        if ((own_session ? setsid() : setpgid(0, 0)) < 0 || fork() != 0)
            _exit(0);
        pid = (long)getpid();
        fd = open(path, O_WRONLY);
        if (fd < 0 || write(p[1], &pid, sizeof(pid)) != (ssize_t)sizeof(pid))
            _exit(1);
        i = open("/dev/null", O_RDWR);
        dup2(i, 0);
        dup2(i, 1);
        dup2(i, 2);
        for (i = 3; i < 1024; i++)
            if (i != fd)
                close(i);
        for (;;)
            pause();
    }
    close(p[1]);
    if (first < 0 || read(p[0], &pid, sizeof(pid)) != (ssize_t)sizeof(pid))
        pid = -1;
    close(p[0]);
    if (first > 0)
        waitpid(first, NULL, 0);
    *group = (long)first;
    return pid;
}

/** Whether a process still holds the FIFO `fd`, opened by watch(), open for
 * writing: a read finds neither data nor the end. */
static int still_open(int fd)
{
    char buf[64];

    return fd >= 0 && read(fd, buf, sizeof(buf)) < 0 && errno == EAGAIN;
}

/** Write the record that a killed etude-guest leaves of attempt `token`,
 * running, with its child `child` in the boot `bootid`; 0, or -1. The
 * record's session is not the test's, so that `status` settles it. */
static int orphan_record(const char *token, long child, const char *bootid)
{
    char path[700];
    FILE *f;
    int ok;

    snprintf(path, sizeof(path), "%s/state/a/%s", work, token);
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;
    snprintf(path, sizeof(path), "%s/state/a/%s/record", work, token);
    f = fopen(path, "w");
    if (!f)
        return -1;
    ok = fprintf(f,
                 "cmd 9000\nsession 5e551011a00000ff\nowner 0 0\nchild %ld 1\nboot %s\n"
                 "state running\n",
                 child, bootid) > 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/**
 * Orphaned attempts whose child's number now has a group that is not the
 * attempt's, made here by hand: a group of the shape an attempt leaves, its
 * first process ended and its second in the session the first made, named
 * by a record of another boot; and a group of this boot whose first process
 * has ended and whose second is in another session. The next etude-guest
 * settles each attempt, kills neither group, and confirms each cleanup.
 * A third group has its own session and a record of this boot: its first
 * process has been reaped, so cleanup must find and kill the group without
 * finding that process.
 */
static void check_group_records(void)
{
    char another[ETUDE_FRAME_TOKEN_LEN + 1];
    long group[3] = {0, 0, 0};
    int fd[3];

    fd[0] = watch("group.boot");
    fd[1] = watch("group.session");
    fd[2] = watch("group.reaped");
    holders[0] = make_group("group.boot", 1, &group[0]);
    holders[1] = make_group("group.session", 0, &group[1]);
    holders[2] = make_group("group.reaped", 1, &group[2]);
    CHECK(holders[0] > 0 && holders[1] > 0 && holders[2] > 0 && still_open(fd[0]) &&
              still_open(fd[1]) && still_open(fd[2]),
          "three groups whose first processes have been reaped are made");
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    snprintf(another, sizeof(another), "%s", boot);
    another[0] = another[0] == '0' ? '1' : '0';
    CHECK(orphan_record("00000000000000da", group[0], another) == 0 &&
              orphan_record("00000000000000db", group[1], boot) == 0 &&
              orphan_record("00000000000000dc", group[2], boot) == 0,
          "the records of the orphaned attempts are made");
    CHECK(settled_confirmed("00000000000000da") && still_open(fd[0]),
          "an orphaned attempt of another boot is settled with nothing killed");
    CHECK(settled_confirmed("00000000000000db") && still_open(fd[1]),
          "a group of another session is not taken for an orphaned attempt's");
    CHECK(settled_confirmed("00000000000000dc"),
          "an orphaned attempt whose first process was reaped has its group killed");
    CHECK(all_gone(fd[2]), "cleanup ends the group whose first process was reaped");
    bye(0);
    end_groups();
    CHECK(all_gone(fd[0]) && all_gone(fd[1]), "the groups made here are ended");
}

/** Read the file `path`, at most `cap - 1` bytes, into `buf` with a NUL;
 * its length, or -1. */
static long read_text(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/** The run's record that an etude-guest killed while it appended an
 * attempt's end leaves, with some of the final events and the next one cut
 * short, made here by hand: the next etude-guest makes the run's record
 * whole, and the run sent again is answered with the whole end. */
static void check_unfinished_end(void)
{
    static const char quick[] = "guest-both";
    static char whole[8192];
    static char now[8192];
    struct ending e;
    struct ending again;
    char path[700];
    char inv[65];
    char c[24];
    const char *p;
    size_t cut;
    long n;
    FILE *f;
    int ok = 0;

    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    put_case("quick", quick, "00000000000000d5", "ordinary", 30000, inv);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d5", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000d5") == 0 && strcmp(e.kind, "exited") == 0 &&
              strcmp(e.cleaned, "confirmed") == 0,
          "an attempt whose end is to be cut from its run's record ends");
    bye(0);
    snprintf(path, sizeof(path), "%s/state/s/%s/%s", work, session, c);
    n = read_text(path, whole, sizeof(whole));
    p = n > 0 ? strstr(whole, "\nstream ") : NULL;
    if (p && (f = fopen(path, "w")) != NULL) {
        cut = (size_t)(p + 12 - whole);
        ok = fwrite(whole, 1, cut, f) == cut;
        ok = fclose(f) == 0 && ok;
    }
    CHECK(ok, "the run's record is cut inside its first stream");
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    send_cmd(ETUDE_CMD_RUN, c, "00000000000000d5", inv, (char *)NULL);
    CHECK(collect(&again, c, "00000000000000d5") == 0 && again.pid == e.pid &&
              strcmp(again.kind, "exited") == 0 && strcmp(again.value, "0") == 0 &&
              strcmp(again.out, "out\n") == 0 && again.sizes_ok &&
              strcmp(again.cleaned, "confirmed") == 0,
          "a run whose record lost part of its end is answered with the whole end");
    CHECK(read_text(path, now, sizeof(now)) == n && strcmp(now, whole) == 0,
          "the next etude-guest makes the run's record whole");
    bye(0);
}

/** An etude-guest killed once it has recorded an attempt's `started` and
 * before it has sent it: the run sent again is answered with that event,
 * which only the run's record keeps. */
static void check_record_first(void)
{
    static const char quick[] = "guest-quick";
    struct ending e;
    char inv[65];
    char c[24];
    int st = -1;

    etude_guest_fault = ETUDE_EV_STARTED;
    start_guest("--stdio", (char *)NULL);
    etude_guest_fault = NULL;
    hello(ETUDE_FRAME_STDIO_MAX);
    put_case("quick", quick, "00000000000000d6", "ordinary", 30000, inv);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "00000000000000d6", inv, (char *)NULL);
    CHECK(wait_guest(&st) == 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "etude-guest is killed at its attempt's start");
    end_guest();
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    send_cmd(ETUDE_CMD_RUN, c, "00000000000000d6", inv, (char *)NULL);
    CHECK(collect(&e, c, "00000000000000d6") == 0 && e.pid > 0,
          "a run repeated after a kill is answered by each event recorded before it");
    bye(0);
}

/** A frame with a bad CRC ends a standard-I/O connection. */
static void check_stdio_damage(void)
{
    struct got g;
    char line[300];
    size_t n;
    int st = -1;

    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);
    n = wire_encode(line, sizeof(line), session, boot, seq++, 0, 0, ETUDE_CMD_STATUS, 0, NULL);
    line[n - 3] = line[n - 3] == '0' ? '1' : '0';
    CHECK(write(to_guest, line, n) == (ssize_t)n, "the damaged frame is written");
    CHECK(next_frame(&g, -1) < 0, "a damaged frame is not answered");
    CHECK(wait_guest(&st) == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 1,
          "a damaged frame ends the connection");
    end_guest();
}

/**
 * On the serial transport, the end of an attempt with the longest command
 * number, a signal and output past its bound on both streams, which in one
 * frame with the streams' sizes and digests would be over 255 bytes. The
 * bound is of both streams together, so standard output keeps it all.
 */
/** A TCP socket listening on a port of 127.0.0.1 the system chose, whose
 * number goes to `*port`; the socket, or -1. */
static int listen_any(int *port)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int s = socket(AF_INET, SOCK_STREAM, 0);

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 1) < 0 ||
        getsockname(s, (struct sockaddr *)&a, &len) < 0) {
        if (s >= 0)
            close(s);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return s;
}

#if defined(__APPLE__)
/** Whether `id` is field 3 of a line of the colon-separated file `path`. */
static int in_file(const char *path, long id)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    int found = 0;

    while (f && !found && fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        p = p ? strchr(p + 1, ':') : NULL;
        found = p && p[1] >= '0' && p[1] <= '9' && strtol(p + 1, NULL, 10) == id;
    }
    if (f)
        fclose(f);
    return found;
}

/** Whether `id` can be a NEW id that Directory Services has and neither
 * /etc/passwd nor /etc/group does. */
static int directory_only(unsigned long id)
{
    return id > 0 && id < 2147483647UL && !in_file("/etc/passwd", (long)id) &&
           !in_file("/etc/group", (long)id);
}

/** An id that Directory Services has and /etc/passwd and /etc/group do not,
 * or -1. A group's is taken before a user's: a user's id is more often a
 * running process's, and etude-guest refuses such an id for the process
 * whether or not it asks Directory Services. */
static long directory_id(void)
{
    struct group *g;
    struct passwd *u;
    long id = -1;

    setgrent();
    while (id < 0 && (g = getgrent()) != NULL)
        if (directory_only(g->gr_gid))
            id = (long)g->gr_gid;
    endgrent();
    setpwent();
    while (id < 0 && (u = getpwent()) != NULL)
        if (directory_only(u->pw_uid))
            id = (long)u->pw_uid;
    endpwent();
    return id;
}
#endif

/** Run the invocation `inv` of attempt `token` and require `error CODE`
 * for it, with nothing run. */
static void refused_run(const char *token, const char *inv, const char *code, const char *what)
{
    struct got g;
    char c[24];
    int before = marks();

    send_cmd(ETUDE_CMD_RUN, new_cmd(c), token, inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_ERROR) == 0 && strcmp(g.field[0], c) == 0 &&
              strcmp(g.field[1], code) == 0 && marks() == before,
          what);
}

/**
 * The fixtures a host can make: the ids, ports and peer given to the
 * program as arguments after its own, with its absolute path as argv[0];
 * an id that a process has, on macOS one that Directory Services alone has,
 * and a port in use refused; fixtures that do not fit their forms refused;
 * and a pseudo-terminal that is the program's terminal, written the
 * invocation's input and read as its output.
 */
static void check_fixtures(void)
{
    static const char args[] = "guest-args";
    static const char term[] = "guest-term";
    static const char *const bad[] = {
        "[{\"fixture\": \"ids\", \"given\": [\"1000\"]}]",
        "[{\"fixture\": \"ids\", \"given\": [\"1000:314159001\", \"1001:314159001\"]}]",
        "[{\"fixture\": \"ports\", \"given\": [\"7470:70000\"]}]",
        "[{\"fixture\": \"bogus\", \"given\": []}]",
        "[{\"fixture\": \"workspace\", \"given\": [\"x\"]}]",
        "[{\"fixture\": \"tmpfs\", \"given\": [\"0\"]}]",
        "[{\"fixture\": \"volume\", \"given\": [\"krffs\", \"absent.img\", \"mounted\"]}]",
        "[{\"fixture\": \"root\", \"given\": [\"ext2\", \"args\", \"musl/x\"]}]",
        "[{\"fixture\": \"mountns\", \"given\": [\"x\"]}]",
        "[{\"fixture\": \"peer\", \"given\": [\"10.0.2.2\", \"7\", \"gateway\"]}]"};
    struct ending e;
    struct stat st;
    char fixtures[1024];
    char expected[2048];
    char token[ETUDE_FRAME_TOKEN_LEN + 1];
    char inv[65];
    char c[24];
    int port = 0, busy_port = 0;
    int busy = listen_any(&busy_port);
    int s = listen_any(&port);
    size_t i;

    if (s >= 0)
        close(s);
    CHECK(busy >= 0 && s >= 0, "the test has a port in use and a free one");
    start_guest("--stdio", (char *)NULL);
    hello(ETUDE_FRAME_STDIO_MAX);

    snprintf(fixtures, sizeof(fixtures),
             "[{\"fixture\": \"workspace\", \"given\": []}, {\"fixture\": \"ids\", \"given\": "
             "[\"1000:314159001\", \"5:314159005\"]}, {\"fixture\": \"ports\", \"given\": "
             "[\"7470:%d\"]}, {\"fixture\": \"peer\", \"given\": [\"10.0.2.2\", \"7\", "
             "\"gateway=10.0.2.2\"]}, "
             "{\"fixture\": \"alone\", \"given\": []}]",
             port);
    put_case_with("args", args, "000000000000fa01", fixtures, NULL, inv);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "000000000000fa01", inv, (char *)NULL);
    CHECK(collect(&e, c, "000000000000fa01") == 0 && strcmp(e.kind, "exited") == 0 &&
              strcmp(e.value, "0") == 0 && strcmp(e.cleaned, "confirmed") == 0,
          "a case with ids, ports and a peer runs and is cleaned");
    snprintf(expected, sizeof(expected),
             "%s/state/a/000000000000fa01/work/args|a b ids=1000:314159001,5:314159005 "
             "ports=7470:%d peer=10.0.2.2:7 gateway=10.0.2.2\n",
             resolved, port);
    CHECK(strcmp(e.out, expected) == 0,
          "the program is given its ids, ports and peer, and its absolute path");
    snprintf(expected, sizeof(expected), "%s/state/a", work);
    CHECK(stat(expected, &st) == 0 && (st.st_mode & 0777) == 0711,
          "every user may search the attempts' directory to reach a workspace");

    snprintf(fixtures, sizeof(fixtures), "[{\"fixture\": \"ids\", \"given\": [\"1000:%ld\"]}]",
             (long)getuid());
    put_case_with("args", args, "000000000000fa02", fixtures, NULL, inv);
    refused_run("000000000000fa02", inv, ETUDE_ERR_REFUSED, "an id that a process has is refused");
#if defined(__APPLE__)
    {
        long id = directory_id();
        CHECK(id >= 0, "Directory Services has an id that /etc/passwd and /etc/group lack");
        snprintf(fixtures, sizeof(fixtures), "[{\"fixture\": \"ids\", \"given\": [\"1000:%ld\"]}]",
                 id);
        put_case_with("args", args, "000000000000fa10", fixtures, NULL, inv);
        refused_run("000000000000fa10", inv, ETUDE_ERR_REFUSED,
                    "an id that Directory Services alone has is refused");
    }
#endif
    snprintf(fixtures, sizeof(fixtures), "[{\"fixture\": \"ports\", \"given\": [\"7470:%d\"]}]",
             busy_port);
    put_case_with("args", args, "000000000000fa03", fixtures, NULL, inv);
    refused_run("000000000000fa03", inv, ETUDE_ERR_REFUSED, "a port in use is refused");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(token, sizeof(token), "000000000000fa%02x", 4 + (int)i);
        put_case_with("args", args, token, bad[i], NULL, inv);
        refused_run(token, inv, ETUDE_ERR_REFUSED,
                    "a fixture that does not fit its form is refused");
    }

    put_case_with("term", term, "000000000000fa20", "[{\"fixture\": \"pty\", \"given\": []}]",
                  "hello\n", inv);
    send_cmd(ETUDE_CMD_RUN, new_cmd(c), "000000000000fa20", inv, (char *)NULL);
    CHECK(collect(&e, c, "000000000000fa20") == 0 && strcmp(e.kind, "exited") == 0 &&
              strcmp(e.value, "0") == 0 && strstr(e.out, "got hello") != NULL &&
              strcmp(e.cleaned, "confirmed") == 0,
          "a case on a pseudo-terminal reads its input there and writes its output");
    bye(0);
    close(busy);
}

static void check_long_end(void)
{
    static const char flood[] = "guest-long";
    static const char long_cmd[] = "999999999999999999";
    static struct ending e;
    char inv[65];

    case_keep = 1000;
    put_case("flooding2", flood, "00000000000000e2", "ordinary", 30000, inv);
    case_keep = 65536;
    send_cmd(ETUDE_CMD_RUN, long_cmd, "00000000000000e2", inv, (char *)NULL);
    CHECK(collect_within(&e, long_cmd, "00000000000000e2", ETUDE_FRAME_SERIAL_MAX) == 0 &&
              strcmp(e.kind, "signaled") == 0 && strcmp(e.value, "15") == 0 && e.sizes_ok &&
              strlen(e.out) == 1000 && e.err[0] == '\0' && strcmp(e.total[0], "10000000") == 0 &&
              strcmp(e.total[1], "10000000") == 0,
          "the end of a long attempt comes whole on the serial console");
}

/**
 * The serial transport: an unacknowledged frame is sent again with the same
 * bytes, a damaged frame and a frame past a gap are dropped and the last
 * frame in order is acknowledged, a repeated frame is not acted on twice,
 * the end of an attempt fits the frame bound, and exec runs a case of the
 * init role in etude-guest's own place.
 */
static void check_serial(void)
{
    static const char init[] = "guest-init";
    static struct got g;
    static struct got again;
    char inv[65];
    char c[24];
    char line[300];
    char text[300];
    const char *field[3];
    size_t n;
    int before;
    int st = -1;
    long pid;

    start_guest(NULL, "--resend-ms", "200", (char *)NULL);
    CHECK(next_frame(&g, -1) == 0 && strcmp(g.type, ETUDE_EV_ALIVE) == 0,
          "alive comes first on the serial console");
    pid = atol(g.field[1]);
    CHECK(next_frame(&g, -1) == 0 && strcmp(g.type, ETUDE_EV_ALIVE) == 0,
          "alive is sent again until a hello comes");
    send_cmd(ETUDE_CMD_HELLO, new_cmd(c), "1", "65536", (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_READY) == 0 && strcmp(g.field[2], "255") == 0,
          "the serial console keeps frames to 255 bytes");
    CHECK(expect(&again, ETUDE_EV_READY) == 0 && strcmp(again.line, g.line) == 0,
          "an unacknowledged frame is sent again with the same bytes");

    /* An acknowledgement stops the resending. */
    n = wire_encode(line, sizeof(line), session, boot, seq, 1, g.seq, ETUDE_CMD_ACK, 0, NULL);
    CHECK(write(to_guest, line, n) == (ssize_t)n, "the ack is written");
    for (before = 0; before < 10 && next_frame(&g, 300) == 0; before++)
        CHECK(strcmp(g.type, ETUDE_EV_READY) == 0, "only the ready frame is pending");
    CHECK(next_frame(&g, 600) < 0, "an acknowledged frame is not sent again");
    serial_acks = 1;
    seen = 1;

    /* A damaged frame, then one past a gap: both dropped, each answered by
     * an ack of SEQ 0, the hello. */
    snprintf(text, sizeof(text), "%lu", cmd_no++);
    field[0] = text;
    field[1] = "0000000000000000";
    n = wire_encode(line, sizeof(line), session, boot, seq, 0, 0, ETUDE_CMD_STATUS, 2, field);
    line[10] ^= 1;
    CHECK(write(to_guest, line, n) == (ssize_t)n, "the damaged frame is written");
    send_seq(seq + 1, ETUDE_CMD_STATUS, 2, field);
    before = 0;
    while (before < 10 && next_frame(&g, before ? 500 : -1) == 0) {
        CHECK(strcmp(g.type, ETUDE_CMD_ACK) == 0 && g.has_ack && g.ack == 0,
              "a damaged frame and a gap are acknowledged at the last frame in order");
        before++;
    }
    CHECK(before >= 1, "a dropped frame is answered by an ack");

    /* The frame in order, and then the same frame again: one state answer. */
    send_seq(seq, ETUDE_CMD_STATUS, 2, field);
    CHECK(expect(&g, ETUDE_EV_STATE) == 0 && strcmp(g.field[2], "unknown") == 0,
          "the frame in order is acted on");
    send_seq(seq, ETUDE_CMD_STATUS, 2, field);
    seq++;
    for (before = 0; before < 10 && next_frame(&g, 600) == 0; before++)
        CHECK(strcmp(g.type, ETUDE_CMD_ACK) == 0 && g.ack == seq - 1,
              "a repeated frame is acknowledged, not acted on");

    check_long_end();

    /* exec: the program becomes this process, writes on the console itself,
     * and its exit is the process's. */
    put_case("asinit", init, "00000000000000e1", "init", 30000, inv);
    before = marks();
    send_cmd(ETUDE_CMD_EXEC, new_cmd(c), "00000000000000e1", inv, (char *)NULL);
    CHECK(expect(&g, ETUDE_EV_STARTED) == 0 && atol(g.field[2]) == pid,
          "exec's started names etude-guest's own process");
    CHECK(read_line(line, sizeof(line), -1) == 0 &&
              strcmp(line, "@etude 1 begin host.etude.guest") == 0,
          "the program writes its report on the console");
    snprintf(text, sizeof(text), "pid %ld", pid);
    CHECK(read_line(line, sizeof(line), -1) == 0 && strcmp(line, text) == 0,
          "the program runs in etude-guest's place");
    CHECK(wait_guest(&st) == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7,
          "the program's exit is the process's");
    CHECK(marks() == before + 1, "the init program ran once");
    end_guest();
}

int main(void)
{
    char *tmp = getenv("TMPDIR");

    workload = host_workload_read(&workload_size);
    if (!workload) {
        fprintf(stderr, "etude_guest_hosttest: prepared etude-workload is missing or unreadable\n");
        free(workload);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup);
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    snprintf(work, sizeof(work), "%s/rach2-etude-guest.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(work)) {
        *work = '\0';
        perror("mkdtemp");
        return 2;
    }
    if (!realpath(work, resolved)) {
        perror(work);
        return 2;
    }
    check_pass_fail_repeat();
    check_sizes();
    check_interruptions();
    check_orphan();
    check_orphan_group();
    check_group_records();
    check_unfinished_end();
    check_record_first();
    check_stdio_damage();
    check_fixtures();
    check_serial();
    if (failures) {
        fprintf(stderr, "etude_guest_hosttest: %d checks failed\n", failures);
        return 1;
    }
    printf("etude_guest_hosttest: all checks passed\n");
    return 0;
}
