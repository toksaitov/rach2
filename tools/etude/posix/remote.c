/*
 * remote.c: the controller's side of remote execution. It starts the
 * responder through a command, such as ssh(1), whose standard input and
 * output carry the frames; agrees on a version and the session; stores files
 * and fetches them checked by their sizes and digests; and runs an attempt,
 * sending the run again under the same command after a lost connection, so
 * that the responder answers it from its ledger and does not run it twice.
 */
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>

#include "../../../utils/sha256.h"
#include "../guest/wire.h"
#include "../json.h"
#include "etude/remote.h"
#ifdef _WIN32
#include "../win/files.h"
#include <wchar.h>
#define posix_now_us etude_win_mono_us
#else
#include "posix.h"
#endif

/* The bytes of a frame besides a `data` frame's base64. */
#define FRAME_OVERHEAD 160

/* Results of reading a frame. */
#define READ_FRAME 0
#define READ_LOST  (-1) /* the end of the connection, or the time passed */
#define READ_FAULT (-2) /* a damaged, foreign or misordered frame */
#define READ_OTHER (-3) /* a frame of another version of the format */
#define READ_TIME  (-4) /* the time passed; the connection stays */
#define READ_AGAIN (-5) /* within read_frame: read on */

struct etude_remote {
    struct etude_remote_conf conf;
    char session[ETUDE_FRAME_TOKEN_LEN + 1];
    char boot[ETUDE_FRAME_TOKEN_LEN + 1]; /* the responder's, or "" */
#ifdef _WIN32
    struct etude_win_sup supervisor;
    struct etude_win_child *child;
    HANDLE input;
    struct jbuf incoming;
    int supervisor_open, transport_closing, transport_error;
#else
    pid_t child; /* the transport, or -1 */
#endif
    int transport_ok, transport_done;
    FILE *transcript;
    int to, from;
    int connected, bounded_writes;
    int cancelling;   /* a CANCEL is sent past the caller's cancellation */
    uint64_t seq_out; /* the SEQ of the next frame sent */
    int has_peer;     /* a frame of the responder's arrived */
    uint64_t seq_in;  /* the SEQ of the last one */
    size_t own_max;   /* the largest frame this side offers */
    size_t max;       /* the largest frame of the connection */
    char *in;         /* what arrived and is not yet read */
    size_t inlen, used;
    uint64_t next_cmd;
    int journal;
    char why[256];
};

/** Set the description of the last failure. */
static void fail(struct etude_remote *r, const char *fmt, const char *a)
{
    snprintf(r->why, sizeof(r->why), fmt, a);
}

/** Append a line not acted on, and why, to the evidence file. */
static void evidence(struct etude_remote *r, const char *why, const char *line, size_t len)
{
    FILE *f;

    if (r->conf.evidence == NULL)
        return;
    f = fopen(r->conf.evidence, "a");
    if (f == NULL)
        return;
    fprintf(f, "%s: ", why);
    fwrite(line, 1, len, f);
    fputc('\n', f);
    fclose(f);
}

/** Write all of `n` bytes; 0 on success. */
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

/** Say whether the caller's cancellation stops a write other than a CANCEL. */
static int cancel_asked(const struct etude_remote *r)
{
    return !r->cancelling && r->conf.cancel_requested && *r->conf.cancel_requested;
}

/** The observed deadline also bounds a pipe whose reader has stopped. */
static uint64_t write_limit(struct etude_remote *r, uint64_t until)
{
    const struct etude_remote_observer *o = r->conf.observer;
    uint64_t observed = o && o->deadline ? o->deadline(o->context) : 0;
    return observed && observed < until ? observed : until;
}

/** Write a frame without blocking observation or cancellation. */
#ifndef _WIN32
static int write_observed(struct etude_remote *r, const char *p, size_t n)
{
    uint64_t now = posix_now_us(), until = now + r->conf.stage_us;
    struct pollfd fd = {r->to, POLLOUT, 0};
    while (n) {
        uint64_t limit = write_limit(r, until);
        now = posix_now_us();
        if (now >= limit || cancel_asked(r)) {
            errno = ETIMEDOUT;
            return -1;
        }
        int ms = limit - now > 50000 ? 50 : (int)((limit - now + 999) / 1000);
        int ready = poll(&fd, 1, ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return -1;
        if (!ready)
            continue;
        ssize_t written = write(r->to, p, n);
        if (written < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (written <= 0)
            return -1;
        p += written;
        n -= (size_t)written;
    }
    return 0;
}

#endif

/** Write the SHA-256 of `n` bytes as hexadecimal into `hex`. */
static void digest_hex(const void *p, size_t n, char hex[65])
{
    struct sha256 h;
    unsigned char d[32];

    sha256_init(&h);
    sha256_update(&h, p, n);
    sha256_final(&h, d);
    sha256_hex(d, hex);
}

/** Give the first ETUDE_FRAME_TOKEN_LEN digits of the SHA-256 of `text`. */
static void token_of(const char *text, char token[ETUDE_FRAME_TOKEN_LEN + 1])
{
    char hex[65];

    digest_hex(text, strlen(text), hex);
    memcpy(token, hex, ETUDE_FRAME_TOKEN_LEN);
    token[ETUDE_FRAME_TOKEN_LEN] = '\0';
}

/** Read a decimal number into `*v`; 0 on success. */
static int decimal(const char *s, uint64_t *v)
{
    uint64_t n = 0;

    if (*s == '\0' || (s[0] == '0' && s[1] != '\0'))
        return -1;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - 9) / 10)
            return -1;
        n = n * 10 + (uint64_t)(*s - '0');
    }
    *v = n;
    return 0;
}

/** Say whether the comma-separated `list` of versions has `version`. */
static int offers(const char *list, unsigned long version)
{
    const char *p = list;
    char *end;

    for (;;) {
        if (*p < '0' || *p > '9')
            return 0;
        if (strtoul(p, &end, 10) == version)
            return 1;
        if (*end != ',')
            return 0;
        p = end + 1;
    }
}

/** Start the transport command, with pipes for its I/O; 0 on success. */
#ifndef _WIN32
static int transport_start(struct etude_remote *r)
{
    static const int keep[3] = {0, 1, 2};
    int to[2], from[2];
    pid_t pid;

    if (pipe(to) != 0)
        return -1;
    if (pipe(from) != 0) {
        close(to[0]);
        close(to[1]);
        return -1;
    }
    r->bounded_writes = r->conf.observer != NULL;
    if (r->bounded_writes && fcntl(to[1], F_SETFL, O_NONBLOCK) < 0) {
        close(to[0]);
        close(to[1]);
        close(from[0]);
        close(from[1]);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(to[0]);
        close(to[1]);
        close(from[0]);
        close(from[1]);
        return -1;
    }
    if (pid == 0) {
        if (dup2(to[0], 0) < 0 || dup2(from[1], 1) < 0)
            _exit(127);
        posix_close_others(keep, 3);
        if (r->conf.envp) {
            extern char **environ;
            environ = (char **)r->conf.envp;
        }
        execvp(r->conf.argv[0], r->conf.argv);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    fcntl(to[1], F_SETFD, FD_CLOEXEC);
    fcntl(from[0], F_SETFD, FD_CLOEXEC);
    r->transport_ok = r->transport_done = 0;
    r->child = pid;
    r->to = to[1];
    r->from = from[0];
    r->seq_out = 0;
    r->has_peer = 0;
    r->inlen = r->used = 0;
    r->max = r->own_max;
    return 0;
}

static pid_t transport_wait(pid_t child, int *status, int flags)
{
    pid_t result;
    do {
        result = waitpid(child, status, flags);
    } while (result < 0 && errno == EINTR);
    return result;
}

/**
 * Close the transport and wait for its command to end: by itself within the
 * stage's bound once its input has ended, by SIGTERM within another, and by
 * SIGKILL after that.
 */
static void transport_stop(struct etude_remote *r)
{
    uint64_t until;
    int sig = 0, status = -1;
    pid_t waited;
    struct timespec ts = {0, 10000000};

    if (r->to >= 0)
        close(r->to);
    if (r->from >= 0)
        close(r->from);
    r->to = r->from = -1;
    r->connected = 0;
    if (r->child < 0)
        return;
    until = write_limit(r, posix_now_us() + r->conf.stage_us);
    while ((waited = transport_wait(r->child, &status, WNOHANG)) == 0) {
        if (posix_now_us() >= until) {
            if (sig == SIGKILL) {
                waited = transport_wait(r->child, &status, 0);
                break;
            }
            sig = sig == 0 ? SIGTERM : SIGKILL;
            kill(r->child, sig);
            until = posix_now_us() + r->conf.stage_us;
        }
        nanosleep(&ts, NULL);
    }
    r->transport_done = waited == r->child;
    r->transport_ok = r->transport_done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    r->child = -1;
}
#endif

#ifdef _WIN32
/* The supervisor owns the transport Job and delivers both complete streams. */
static void transport_output(void *context, struct etude_win_child *child,
                             enum etude_win_stream stream, const char *bytes, size_t size)
{
    struct etude_remote *r = context;
    (void)child;
    if (stream == ETUDE_WIN_STDERR) {
        DWORD written = 0;
        if (size > UINT32_MAX ||
            !WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes, (DWORD)size, &written, NULL) ||
            written != size)
            r->transport_error = 1;
    } else if (!r->transport_closing)
        jbuf_put(&r->incoming, bytes, size);
}

/* A noninherited overlapped writer and a borrowed synchronous child reader. */
static int transport_input(struct etude_remote *r, HANDLE *reader)
{
    static LONG serial;
    wchar_t name[128];
    swprintf(name, sizeof name / sizeof *name, L"\\\\.\\pipe\\etude-remote-%lu-%lu",
             (unsigned long)GetCurrentProcessId(), (unsigned long)InterlockedIncrement(&serial));
    r->input = CreateNamedPipeW(
        name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        ETUDE_FRAME_STDIO_MAX, 0, 0, NULL);
    if (r->input == INVALID_HANDLE_VALUE)
        return -1;
    *reader = CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    return *reader == INVALID_HANDLE_VALUE ? -1 : 0;
}

static void transport_stop(struct etude_remote *r)
{
    if (r->input != INVALID_HANDLE_VALUE) {
        CloseHandle(r->input);
        r->input = INVALID_HANDLE_VALUE;
    }
    r->to = r->from = -1;
    r->connected = 0;
    r->transport_closing = 1;
    if (r->child) {
        uint64_t until = write_limit(r, posix_now_us() + r->conf.stage_us);
        while (!r->child->ended && posix_now_us() < until &&
               !(r->conf.cancel_requested && *r->conf.cancel_requested))
            if (etude_win_poll(&r->supervisor, 50) < 0)
                break;
        if (!r->child->ended)
            etude_win_stop(r->child);
        r->transport_ok = r->child->ended && !r->child->stopped && r->child->status == 0 &&
                          !r->child->out.error && !r->child->err.error;
        DWORD error = etude_win_release(r->child, 2000);
        if (error)
            r->transport_error = 1;
        else
            r->child = NULL;
    }
    if (r->supervisor_open) {
        DWORD error = etude_win_sup_close(&r->supervisor, 2000);
        if (error)
            r->transport_error = 1;
        else {
            r->supervisor_open = 0;
            r->child = NULL;
        }
    }
    r->transport_done = !r->supervisor_open && !r->transport_error;
    jbuf_free(&r->incoming);
}

static int transport_start(struct etude_remote *r)
{
    if (r->supervisor_open)
        return -1;
    struct etude_win_spawn spawn = {0};
    HANDLE reader = INVALID_HANDLE_VALUE;
    r->transport_error = r->transport_closing = r->transport_ok = r->transport_done = 0;
    if (etude_win_sup_open(&r->supervisor))
        return -1;
    r->supervisor_open = 1;
    if (transport_input(r, &reader)) {
        transport_stop(r);
        return -1;
    }
    char executable[32768];
    spawn.program = r->conf.argv[0];
    if (!etude_win_absolute(spawn.program)) {
        if (etude_win_program(spawn.program, executable, sizeof executable)) {
            CloseHandle(reader);
            transport_stop(r);
            return -1;
        }
        spawn.program = executable;
    }
    spawn.argv = (const char *const *)(r->conf.argv + 1);
    spawn.envp = (const char *const *)r->conf.envp;
    spawn.stdin_handle = reader;
    spawn.output = transport_output;
    spawn.ctx = r;
    DWORD error = etude_win_start(&r->supervisor, &spawn, &r->child);
    CloseHandle(reader);
    if (error) {
        transport_stop(r);
        return -1;
    }
    r->to = r->from = 0;
    r->bounded_writes = 1;
    r->seq_out = 0;
    r->has_peer = 0;
    r->inlen = r->used = 0;
    r->max = r->own_max;
    return 0;
}

/* Keep the OVERLAPPED and its bytes alive until cancellation is completed. */
static int transport_write(struct etude_remote *r, const char *bytes, DWORD size, uint64_t until,
                           DWORD *written)
{
    OVERLAPPED operation = {0};
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!operation.hEvent)
        return -1;
    int rc = 0;
    if (!WriteFile(r->input, bytes, size, written, &operation)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(operation.hEvent);
            return -1;
        }
        DWORD ready = WAIT_TIMEOUT;
        while (!rc && (ready = WaitForSingleObject(operation.hEvent, 0)) == WAIT_TIMEOUT) {
            if (posix_now_us() >= write_limit(r, until) || r->transport_error || cancel_asked(r) ||
                etude_win_poll(&r->supervisor, 50) < 0)
                rc = -1;
        }
        if (ready != WAIT_OBJECT_0)
            rc = -1;
        if (rc) {
            CancelIoEx(r->input, &operation);
            GetOverlappedResult(r->input, &operation, written, TRUE);
        } else if (!GetOverlappedResult(r->input, &operation, written, TRUE))
            rc = -1;
    }
    CloseHandle(operation.hEvent);
    return rc || !*written ? -1 : 0;
}

static int write_observed(struct etude_remote *r, const char *bytes, size_t size)
{
    uint64_t until = posix_now_us() + r->conf.stage_us;
    while (size) {
        if (cancel_asked(r) || posix_now_us() >= write_limit(r, until))
            return -1;
        DWORD written = 0, part = size > UINT32_MAX ? UINT32_MAX : (DWORD)size;
        if (transport_write(r, bytes, part, until, &written))
            return -1;
        bytes += written;
        size -= written;
    }
    return 0;
}
#endif

static int transcript(struct etude_remote *r, const char *direction, const char *bytes, size_t size)
{
    if (!r->transcript)
        return 0;
    return fputs(direction, r->transcript) < 0 || fwrite(bytes, 1, size, r->transcript) != size ||
                   fflush(r->transcript)
               ? -1
               : 0;
}

/** Send one frame; a failed write loses the connection. */
static int send_frame(struct etude_remote *r, const char *type, const char *const *field, int n)
{
    char *buf = malloc(r->max + 1);
    size_t len;

    if (buf == NULL)
        return -1;
    len = wire_encode(buf, r->max + 1, r->session, r->boot, r->seq_out, r->has_peer, r->seq_in,
                      type, n, field);
    if (len == 0) {
        free(buf);
        fail(r, "a %s frame does not fit the connection's bound", type);
        transport_stop(r);
        return -1;
    }
    r->seq_out++;
    if (transcript(r, "> ", buf, len) ||
        (r->bounded_writes ? write_observed(r, buf, len) : write_all(r->to, buf, len)) != 0) {
        free(buf);
        fail(r, "the connection was lost while a %s was sent", type);
        transport_stop(r);
        return -1;
    }
    free(buf);
    return 0;
}

/** Say why the responder's frame does not follow on the connection, or NULL. */
static const char *frame_fault(struct etude_remote *r, const struct wire_frame *f)
{
    if (!r->has_peer &&
        (strcmp(f->type, ETUDE_EV_ALIVE) != 0 || strcmp(f->session, ETUDE_FRAME_NO_SESSION) != 0))
        return "a first frame that is not the responder's alive";
    if (r->has_peer && strcmp(f->session, r->session) != 0)
        return "a frame of another session";
    if (r->has_peer && strcmp(f->boot, r->boot) != 0)
        return "a frame of another boot";
    if (f->seq != (r->has_peer ? r->seq_in + 1 : 0))
        return "a frame out of order";
    if (f->has_ack && f->ack >= r->seq_out)
        return "a frame that acknowledges what was not sent";
    return NULL;
}

/**
 * Take the line of `len` bytes at the start of the connection's buffer.
 *
 * @return READ_FRAME with `f` filled; READ_AGAIN for a line kept as evidence
 *         or an `ack`; or READ_FAULT or READ_OTHER with the connection
 *         stopped.
 */
static int take_line(struct etude_remote *r, size_t len, struct wire_frame *f)
{
    char *line = r->in, *copy;
    const char *why;
    int other;

    r->used = len + 1;
    if (transcript(r, "< ", line, len + 1)) {
        fail(r, "cannot retain %s", "the transport transcript");
        transport_stop(r);
        return READ_FAULT;
    }
    if (len < 2 || line[0] != '#' || line[1] != 'E') {
        evidence(r, "not a frame", line, len);
        return READ_AGAIN;
    }
    copy = malloc(len + 1);
    if (copy == NULL) {
        fail(r, "out of %s", "memory");
        transport_stop(r);
        return READ_FAULT;
    }
    memcpy(copy, line, len);
    other = len < 4 || memcmp(line, ETUDE_FRAME_MAGIC " ", 4) != 0;
    if (other)
        why = "a frame of another version of the format";
    else if (len + 1 > r->max)
        why = "a frame longer than the bound";
    else if (wire_parse(line, len, f) != 0)
        why = "a damaged or malformed frame";
    else
        why = frame_fault(r, f);
    if (why != NULL) {
        evidence(r, why, copy, len);
        free(copy);
        fail(r, "the responder sent %s; the connection ends", why);
        transport_stop(r);
        return other ? READ_OTHER : READ_FAULT;
    }
    free(copy);
    r->has_peer = 1;
    r->seq_in = f->seq;
    return strcmp(f->type, ETUDE_CMD_ACK) == 0 ? READ_AGAIN : READ_FRAME;
}

/**
 * Wait for more of the connection until monotonic time `until`, 0 setting
 * none.
 *
 * @return READ_AGAIN when bytes arrived or the wait ended early; READ_TIME
 *         when `until` passed; or READ_LOST with the connection stopped.
 */
#ifndef _WIN32
static int fill(struct etude_remote *r, uint64_t until)
{
    struct pollfd pfd;
    uint64_t now = posix_now_us();
    ssize_t n;
    int rc;

    if (until != 0 && now >= until) {
        fail(r, "no answer came within %s", "the time allowed");
        return READ_TIME;
    }
    pfd.fd = r->from;
    pfd.events = POLLIN;
    rc = poll(&pfd, 1, until == 0 ? -1 : (int)((until - now + 999) / 1000));
    if (rc == 0 || (rc < 0 && errno == EINTR))
        return READ_AGAIN;
    n = rc < 0 ? -1 : read(r->from, r->in + r->inlen, r->own_max - r->inlen);
    if (n < 0 && errno == EINTR)
        return READ_AGAIN;
    if (n <= 0) {
        if (r->inlen > 0)
            evidence(r, "a line cut short by the end of the connection", r->in, r->inlen);
        fail(r, "the connection %s", "ended");
        transport_stop(r);
        return READ_LOST;
    }
    r->inlen += (size_t)n;
    return READ_AGAIN;
}

#endif

#ifdef _WIN32
static int fill(struct etude_remote *r, uint64_t until)
{
    uint64_t now = posix_now_us();
    if (until && now >= until) {
        fail(r, "no answer came within %s", "the time allowed");
        return READ_TIME;
    }
    if (!r->incoming.len && r->child && !r->child->out.eof) {
        DWORD wait = !until || until - now > 50000 ? 50 : (DWORD)((until - now + 999) / 1000);
        if (etude_win_poll(&r->supervisor, wait) < 0)
            r->transport_error = 1;
    }
    if (r->transport_error || (!r->incoming.len && (!r->child || r->child->out.eof))) {
        fail(r, "the connection %s", "ended");
        transport_stop(r);
        return READ_LOST;
    }
    size_t size = r->own_max - r->inlen;
    if (size > r->incoming.len)
        size = r->incoming.len;
    if (size) {
        memcpy(r->in + r->inlen, r->incoming.data, size);
        r->inlen += size;
        memmove(r->incoming.data, r->incoming.data + size, r->incoming.len - size);
        r->incoming.len -= size;
    }
    return READ_AGAIN;
}
#endif

/**
 * Read the next frame, until monotonic time `until`, 0 setting none.
 *
 * Lines that are not frames, such as what a shell's startup files print,
 * are kept as evidence. The first frame of a connection must be the
 * responder's `alive`, without a session; every later one must have the
 * session and the boot, and follow the one before it. The frame's fields
 * point into the connection's buffer until the next read.
 *
 * @return READ_FRAME; READ_TIME when `until` passed; or READ_LOST,
 *         READ_FAULT or READ_OTHER with the connection stopped.
 */
static int read_frame(struct etude_remote *r, uint64_t until, struct wire_frame *f)
{
    char *nl;
    int rc;

    for (;;) {
        if (r->conf.observer && r->conf.observer->deadline) {
            uint64_t bound = r->conf.observer->deadline(r->conf.observer->context);
            if (bound && bound < until)
                until = bound;
        }
        if (r->from < 0)
            return READ_LOST;
        if (r->used > 0) {
            memmove(r->in, r->in + r->used, r->inlen - r->used);
            r->inlen -= r->used;
            r->used = 0;
        }
        nl = memchr(r->in, '\n', r->inlen);
        if (nl != NULL) {
            rc = take_line(r, (size_t)(nl - r->in), f);
        } else if (r->inlen >= r->max) {
            evidence(r, "a line longer than a frame", r->in, r->inlen);
            fail(r, "the responder sent %s; the connection ends", "a line longer than a frame");
            transport_stop(r);
            rc = READ_FAULT;
        } else {
            rc = fill(r, until);
        }
        if (rc != READ_AGAIN)
            return rc;
    }
}

/** Map a failed read to the status it gives. */
static enum etude_remote_status read_status(int rc)
{
    return rc == READ_OTHER   ? ETUDE_REMOTE_VERSION
           : rc == READ_FAULT ? ETUDE_REMOTE_PROTOCOL
                              : ETUDE_REMOTE_LOST;
}

/**
 * Allocate the next command number, recording the command in the journal
 * before it can be sent.
 *
 * @return 0 with the number's text in `cmd`, or -1.
 */
static int new_cmd(struct etude_remote *r, char cmd[24], const char *type, const char *const *field,
                   int n)
{
    char line[1024];
    size_t len;
    int i;

    snprintf(cmd, 24, "%llu", (unsigned long long)r->next_cmd++);
    if (r->journal < 0)
        return 0;
    len = (size_t)snprintf(line, sizeof(line), "%s %s", cmd, type);
    for (i = 0; i < n && len < sizeof(line); i++)
        len += (size_t)snprintf(line + len, sizeof(line) - len, " %s", field[i]);
    if (len + 1 >= sizeof(line)) {
        fail(r, "a %s command is too long for the journal", type);
        return -1;
    }
    line[len++] = '\n';
    if (write_all(r->journal, line, len) != 0 || fsync(r->journal) != 0) {
        fail(r, "cannot record a command in %s", r->conf.journal);
        return -1;
    }
    return 0;
}

/**
 * Wait for the first event of command `cmd`, skipping those of others,
 * until `until`, after which the connection is stopped as lost.
 */
static int wait_answer(struct etude_remote *r, const char *cmd, uint64_t until,
                       struct wire_frame *f)
{
    int rc;

    for (;;) {
        rc = read_frame(r, until, f);
        if (rc == READ_TIME) {
            transport_stop(r);
            return READ_LOST;
        }
        if (rc != READ_FRAME)
            return rc;
        if (f->nfields >= 1 && strcmp(f->field[0], cmd) == 0)
            return READ_FRAME;
        evidence(r, "an event of another command", f->type, strlen(f->type));
    }
}

/** Connect once: start the transport, read `alive`, say hello, read `ready`. */
static enum etude_remote_status connect_once(struct etude_remote *r)
{
    char cmd[24], max[24];
    const char *a[3];
    struct wire_frame f;
    uint64_t v;
    int rc;

    if (transport_start(r) != 0) {
        fail(r, "cannot start the transport: %s", strerror(errno));
        return ETUDE_REMOTE_SYSTEM;
    }
    rc = read_frame(r, posix_now_us() + r->conf.stage_us, &f);
    if (rc == READ_TIME) {
        transport_stop(r);
        rc = READ_LOST;
    }
    if (rc != READ_FRAME) {
        if (rc == READ_OTHER)
            fail(r, "the responder speaks %s", "another version of the frames");
        return read_status(rc);
    }
    if (f.nfields != 3 || !offers(f.field[0], ETUDE_FRAME_VERSION)) {
        fail(r, "the responder speaks versions %s, and this controller 1",
             f.nfields >= 1 ? f.field[0] : "-");
        transport_stop(r);
        return ETUDE_REMOTE_VERSION;
    }
    memcpy(r->boot, f.boot, sizeof(r->boot));
    snprintf(max, sizeof(max), "%lu", (unsigned long)r->own_max);
    a[0] = ETUDE_REMOTE_VERSIONS;
    a[1] = max;
    if (new_cmd(r, cmd, ETUDE_CMD_HELLO, a, 2) != 0) {
        transport_stop(r);
        return ETUDE_REMOTE_SYSTEM;
    }
    a[0] = cmd;
    a[1] = ETUDE_REMOTE_VERSIONS;
    a[2] = max;
    if (send_frame(r, ETUDE_CMD_HELLO, a, 3) != 0)
        return ETUDE_REMOTE_LOST;
    rc = wait_answer(r, cmd, posix_now_us() + r->conf.stage_us, &f);
    if (rc != READ_FRAME)
        return read_status(rc);
    if (strcmp(f.type, ETUDE_EV_ERROR) == 0 && f.nfields == 2) {
        fail(r, "the responder refused the hello: %s", f.field[1]);
        transport_stop(r);
        return strcmp(f.field[1], ETUDE_ERR_VERSION) == 0 ? ETUDE_REMOTE_VERSION
                                                          : ETUDE_REMOTE_REFUSED;
    }
    if (strcmp(f.type, ETUDE_EV_READY) != 0 || f.nfields != 5) {
        fail(r, "the responder answered the hello with %s", f.type);
        transport_stop(r);
        return ETUDE_REMOTE_PROTOCOL;
    }
    if (strcmp(f.field[1], "1") != 0) {
        fail(r, "the responder chose version %s, which this controller did not offer", f.field[1]);
        transport_stop(r);
        return ETUDE_REMOTE_VERSION;
    }
    if (decimal(f.field[2], &v) != 0 || v < ETUDE_FRAME_MIN || v > r->own_max) {
        fail(r, "the responder chose a largest frame of %s", f.field[2]);
        transport_stop(r);
        return ETUDE_REMOTE_PROTOCOL;
    }
    r->max = (size_t)v;
    r->connected = 1;
    return ETUDE_REMOTE_OK;
}

static void remote_free(struct etude_remote *r);

enum etude_remote_status etude_remote_open(const struct etude_remote_conf *conf,
                                           struct etude_remote **out, char *why, size_t whylen)
{
    struct etude_remote *r = calloc(1, sizeof(*r));
    enum etude_remote_status st;
    char line[1024];
    uint64_t v, top = 0;
    FILE *f;

    *out = NULL;
    if (r == NULL) {
        snprintf(why, whylen, "out of memory");
        return ETUDE_REMOTE_SYSTEM;
    }
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    r->child = -1;
#else
    r->input = INVALID_HANDLE_VALUE;
#endif
    r->conf = *conf;
    r->to = r->from = r->journal = -1;
    r->own_max = conf->max_frame != 0 ? conf->max_frame : ETUDE_FRAME_STDIO_MAX;
    r->in = malloc(r->own_max + 1);
    token_of(conf->session_id ? conf->session_id : conf->run_id, r->session);
    if (r->in == NULL || r->own_max < ETUDE_FRAME_MIN) {
        snprintf(why, whylen, "cannot make a connection with frames of %lu bytes",
                 (unsigned long)r->own_max);
        free(r->in);
        free(r);
        return ETUDE_REMOTE_SYSTEM;
    }
    if (conf->journal != NULL) {
        f = fopen(conf->journal, "r");
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, " \n")] = '\0';
            if (decimal(line, &v) == 0 && v > top)
                top = v;
        }
        if (f != NULL)
            fclose(f);
        r->journal = open(conf->journal, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (r->journal < 0) {
            snprintf(why, whylen, "cannot open %s: %s", conf->journal, strerror(errno));
            free(r->in);
            free(r);
            return ETUDE_REMOTE_SYSTEM;
        }
    }
    if (conf->transcript) {
        r->transcript = fopen(conf->transcript, "wbx");
        if (!r->transcript) {
            snprintf(why, whylen, "cannot create transcript: %s", conf->transcript);
            if (r->journal >= 0)
                close(r->journal);
            free(r->in);
            free(r);
            return ETUDE_REMOTE_SYSTEM;
        }
    }
    r->next_cmd = top + 1;
    st = connect_once(r);
    if (st != ETUDE_REMOTE_OK) {
        snprintf(why, whylen, "%s", r->why);
        transport_stop(r);
        remote_free(r);
        return st;
    }
    *out = r;
    return ETUDE_REMOTE_OK;
}

/**
 * Make the connection again after a loss.
 *
 * @return ETUDE_REMOTE_OK, with `r->boot` the responder's boot now, or the
 *         failure.
 */
static enum etude_remote_status reconnect(struct etude_remote *r)
{
    transport_stop(r);
    return connect_once(r);
}

/** Say whether a status is one that a new connection may cure. */
static int transient(enum etude_remote_status st)
{
    return st == ETUDE_REMOTE_LOST || st == ETUDE_REMOTE_PROTOCOL;
}

/** Map an error event's code to a status. */
static enum etude_remote_status error_status(const char *code)
{
    if (strcmp(code, ETUDE_ERR_DAMAGED) == 0)
        return ETUDE_REMOTE_DAMAGED;
    if (strcmp(code, ETUDE_ERR_VERSION) == 0)
        return ETUDE_REMOTE_VERSION;
    return ETUDE_REMOTE_REFUSED;
}

/* A file to store, with the size and digest its put gives. */
struct outgoing {
    const char *name, *mode;
    const unsigned char *data;
    size_t size;
    char sha[65], sizetext[24];
};

/** Send a put, its data and its seal, and read its answer. */
static enum etude_remote_status put_once(struct etude_remote *r, const struct outgoing *o)
{
    size_t chunk = (r->max - FRAME_OVERHEAD) / 4 * 3, at, n;
    char cmd[24], offset[24], *b64;
    const char *a[5];
    struct wire_frame f;
    int rc;

    a[0] = o->name;
    a[1] = o->sizetext;
    a[2] = o->sha;
    a[3] = o->mode;
    if (new_cmd(r, cmd, ETUDE_CMD_PUT, a, 4) != 0)
        return ETUDE_REMOTE_SYSTEM;
    a[0] = cmd;
    a[1] = o->name;
    a[2] = o->sizetext;
    a[3] = o->sha;
    a[4] = o->mode;
    if (send_frame(r, ETUDE_CMD_PUT, a, 5) != 0)
        return ETUDE_REMOTE_LOST;
    b64 = malloc(r->max);
    if (b64 == NULL)
        return ETUDE_REMOTE_SYSTEM;
    for (at = 0; o->data != NULL && at < o->size; at += n) {
        n = o->size - at < chunk ? o->size - at : chunk;
        wire_base64(o->data + at, n, b64);
        snprintf(offset, sizeof(offset), "%llu", (unsigned long long)at);
        a[0] = cmd;
        a[1] = offset;
        a[2] = b64;
        if (send_frame(r, ETUDE_CMD_DATA, a, 3) != 0) {
            free(b64);
            return ETUDE_REMOTE_LOST;
        }
    }
    free(b64);
    a[0] = cmd;
    if (send_frame(r, ETUDE_CMD_SEAL, a, 1) != 0)
        return ETUDE_REMOTE_LOST;
    rc = wait_answer(r, cmd, posix_now_us() + r->conf.stage_us, &f);
    if (rc != READ_FRAME)
        return read_status(rc);
    if (strcmp(f.type, ETUDE_EV_ERROR) == 0 && f.nfields == 2) {
        fail(r, "the responder refused the file: %s", f.field[1]);
        return error_status(f.field[1]);
    }
    if (strcmp(f.type, ETUDE_EV_STORED) != 0 || f.nfields != 3 ||
        strcmp(f.field[1], o->name) != 0 || strcmp(f.field[2], o->sha) != 0) {
        fail(r, "the responder answered a put with %s", f.type);
        transport_stop(r);
        return ETUDE_REMOTE_PROTOCOL;
    }
    return ETUDE_REMOTE_OK;
}

static enum etude_remote_status store_outgoing(struct etude_remote *r, struct outgoing *o)
{
    enum etude_remote_status st = ETUDE_REMOTE_LOST;
    unsigned tries;

    for (tries = 0; tries <= r->conf.reconnects; tries++) {
        if (!r->connected) {
            st = reconnect(r);
            if (st != ETUDE_REMOTE_OK) {
                if (transient(st))
                    continue;
                return st;
            }
        }
        st = put_once(r, o);
        if (st == ETUDE_REMOTE_OK || (!transient(st) && st != ETUDE_REMOTE_DAMAGED))
            return st;
    }
    return st;
}

enum etude_remote_status etude_remote_put(struct etude_remote *r, const char *name,
                                          const void *data, size_t size, const char *mode)
{
    struct outgoing o;
    memset(&o, 0, sizeof(o));
    o.name = name;
    o.mode = mode;
    o.data = data;
    o.size = size;
    digest_hex(data, size, o.sha);
    snprintf(o.sizetext, sizeof(o.sizetext), "%llu", (unsigned long long)size);
    return store_outgoing(r, &o);
}

enum etude_remote_status etude_remote_reference(struct etude_remote *r, const char *name,
                                                size_t size, const char *sha, const char *mode)
{
    struct outgoing o;
    if (strlen(sha) != 64 || strspn(sha, "0123456789abcdef") != 64)
        return ETUDE_REMOTE_REFUSED;
    memset(&o, 0, sizeof(o));
    o.name = name;
    o.mode = mode;
    o.size = size;
    memcpy(o.sha, sha, sizeof(o.sha));
    snprintf(o.sizetext, sizeof(o.sizetext), "%llu", (unsigned long long)size);
    return store_outgoing(r, &o);
}

/* What has arrived of one attempt, with the text of each event, so that an
 * event sent again can be compared with the first. */
struct collect {
    struct etude_remote_result *res;
    const char *cmd;
    size_t output_max;
    size_t cap[2];
    char started[96], exit[64], stream[2][160], usage[256], cleaned[64];
    int have_exit, have_stream[2], have_usage, have_cleaned;
    int sizes_known[2];
    uint64_t sent[2];
    char sha[2][65];
};

/** Join fields from `from` on with single spaces into `out`. */
static void join_fields(char *out, size_t cap, const char *const *field, int from, int n)
{
    size_t len = 0;
    int i;

    out[0] = '\0';
    for (i = from; i < n && len < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s", i > from ? " " : "", field[i]);
}

/**
 * Take an event sent again, or for the first time: 0 when it agrees with
 * the one before, -1 when it does not.
 */
static int same_or_set(char *kept, size_t cap, int *have, struct wire_frame *f)
{
    char text[512];

    join_fields(text, sizeof(text), f->field, 2, f->nfields);
    if (*have)
        return strcmp(kept, text) == 0 ? 0 : -1;
    snprintf(kept, cap, "%s", text);
    *have = 1;
    return 0;
}

/** Take `out CMD ATTEMPT FD OFFSET BASE64`; 0 when it agrees with the rest. */
static int take_output(struct collect *c, struct wire_frame *f)
{
    struct etude_output *o;
    uint64_t offset;
    unsigned char *buf;
    size_t end;
    long n;
    int fd;

    if (f->nfields != 5 || (strcmp(f->field[2], "1") != 0 && strcmp(f->field[2], "2") != 0) ||
        decimal(f->field[3], &offset) != 0)
        return -1;
    fd = f->field[2][0] == '1' ? 0 : 1;
    o = fd == 0 ? &c->res->out : &c->res->err;
    buf = malloc(strlen(f->field[4]) / 4 * 3 + 3);
    if (buf == NULL)
        return -1;
    n = wire_unbase64(f->field[4], buf);
    if (n <= 0 || offset > o->kept || offset + (uint64_t)n > c->output_max) {
        free(buf);
        return -1;
    }
    end = (size_t)offset + (size_t)n;
    if (end > c->cap[fd]) {
        size_t want = c->cap[fd] == 0 ? 4096 : c->cap[fd];
        char *p;
        while (want < end)
            want *= 2;
        if (want > c->output_max)
            want = c->output_max;
        p = realloc(o->data, want + 1);
        if (p == NULL) {
            free(buf);
            return -1;
        }
        o->data = p;
        c->cap[fd] = want;
    }
    if (memcmp(o->data + offset, buf,
               (size_t)(o->kept - offset < (size_t)n ? o->kept - offset : (size_t)n)) != 0) {
        free(buf);
        return -1;
    }
    if (end > o->kept) {
        memcpy(o->data + o->kept, buf + (o->kept - offset), end - o->kept);
        o->kept = end;
        o->data[end] = '\0';
    }
    free(buf);
    return 0;
}

/** Read a `usage` value: N, N+ for a lower bound, or -. */
static void take_measure(struct etude_measure *m, char *method, size_t cap, const char *value,
                         const char *name)
{
    char digits[24];
    size_t len = strlen(value);
    uint64_t v;

    m->quality = ETUDE_QUALITY_UNAVAILABLE;
    m->value = 0;
    m->method = "the responder could not measure it";
    if (strcmp(value, "-") == 0 || len == 0 || len >= sizeof(digits))
        return;
    memcpy(digits, value, len + 1);
    if (digits[len - 1] == '+')
        digits[len - 1] = '\0';
    if (decimal(digits, &v) != 0)
        return;
    m->value = v;
    m->quality = value[len - 1] == '+' ? ETUDE_QUALITY_LOWER_BOUND : ETUDE_QUALITY_EXACT;
    snprintf(method, cap, "%s", strcmp(name, "-") == 0 ? "remote" : name);
    m->method = method;
}

/** Take `exit CMD ATTEMPT KIND VALUE`, the end; 0 when it agrees. */
static int take_exit(struct collect *c, struct wire_frame *f)
{
    struct etude_remote_result *res = c->res;
    uint64_t v;
    const char *k;

    if (f->nfields != 4 || same_or_set(c->exit, sizeof(c->exit), &c->have_exit, f) != 0)
        return -1;
    k = f->field[2];
    res->end = strcmp(k, "exited") == 0     ? ETUDE_END_EXITED
               : strcmp(k, "signaled") == 0 ? ETUDE_END_SIGNALED
               : strcmp(k, "stopped") == 0  ? ETUDE_END_STOPPED
                                            : ETUDE_END_LOST;
    res->value = -1;
    if (decimal(f->field[3], &v) == 0 && v < 256)
        res->value = (int)v;
    if ((res->end == ETUDE_END_EXITED || res->end == ETUDE_END_SIGNALED) && res->value < 0)
        return -1;
    return 0;
}

/**
 * Take `stream CMD ATTEMPT FD SIZE TOTAL SHA256`; 0 when it agrees with what
 * arrived. A stream whose fields are "-" has sizes that are not known.
 */
static int take_stream(struct collect *c, struct wire_frame *f)
{
    uint64_t total;
    int fd;

    if (f->nfields != 6 || (strcmp(f->field[2], "1") != 0 && strcmp(f->field[2], "2") != 0))
        return -1;
    fd = f->field[2][0] == '1' ? 0 : 1;
    if (same_or_set(c->stream[fd], sizeof(c->stream[fd]), &c->have_stream[fd], f) != 0)
        return -1;
    c->sizes_known[fd] = decimal(f->field[3], &c->sent[fd]) == 0 &&
                         decimal(f->field[4], &total) == 0 && strlen(f->field[5]) == 64;
    if (c->sizes_known[fd]) {
        (fd == 0 ? &c->res->out : &c->res->err)->total = total;
        memcpy(c->sha[fd], f->field[5], 65);
    }
    return 0;
}

/** Take `started CMD ATTEMPT PID START`; 0 when it agrees with what arrived. */
static int take_started(struct collect *c, struct wire_frame *f)
{
    struct etude_remote_result *res = c->res;
    uint64_t pid = 0, start = 0;
    int had = res->started;

    if (f->nfields != 4 || decimal(f->field[2], &pid) != 0 ||
        (strcmp(f->field[3], "-") != 0 && decimal(f->field[3], &start) != 0) ||
        same_or_set(c->started, sizeof(c->started), &res->started, f) != 0)
        return -1;
    if (!had) {
        res->process.pid = (long)pid;
        res->process.start_known = strcmp(f->field[3], "-") != 0;
        res->process.start = start;
    }
    return 0;
}

/** Take `usage CMD ATTEMPT CPU-US METHOD PEAK METHOD`; 0 when it agrees. */
static int take_usage(struct collect *c, struct wire_frame *f)
{
    struct etude_remote_result *res = c->res;

    if (f->nfields != 6 || same_or_set(c->usage, sizeof(c->usage), &c->have_usage, f) != 0)
        return -1;
    take_measure(&res->cpu_us, res->cpu_method, sizeof(res->cpu_method), f->field[2], f->field[3]);
    take_measure(&res->peak_bytes, res->peak_method, sizeof(res->peak_method), f->field[4],
                 f->field[5]);
    return 0;
}

/** Take `cleaned CMD ATTEMPT confirmed|unconfirmed`; 0 when it agrees. */
static int take_cleaned(struct collect *c, struct wire_frame *f)
{
    int confirmed;

    if (f->nfields != 3)
        return -1;
    confirmed = strcmp(f->field[2], "confirmed") == 0;
    if ((!confirmed && strcmp(f->field[2], "unconfirmed") != 0) ||
        same_or_set(c->cleaned, sizeof(c->cleaned), &c->have_cleaned, f) != 0)
        return -1;
    c->res->cleanup = confirmed ? ETUDE_CLEANUP_CONFIRMED : ETUDE_CLEANUP_UNCONFIRMED;
    return 0;
}

static int take_observed_output(struct etude_remote *r, struct collect *c, struct wire_frame *frame)
{
    const size_t before[2] = {c->res->out.kept, c->res->err.kept};
    int status = c->res->started ? take_output(c, frame) : -1;
    const struct etude_remote_observer *observer = r->conf.observer;
    if (status || !observer)
        return status;
    const struct etude_output *streams[] = {&c->res->out, &c->res->err};
    for (size_t i = 0; i < 2; i++)
        if (observer->stream && streams[i]->kept > before[i])
            observer->stream(observer->context, (int)i + 1, streams[i]->data + before[i],
                             streams[i]->kept - before[i]);
    if (observer->output && streams[0]->kept > before[0])
        observer->output(observer->context, streams[0]->data + before[0],
                         streams[0]->kept - before[0]);
    return 0;
}

/**
 * Take one event of the run's command.
 *
 * @return 1 when the attempt's events are all there, 0 when more are to
 *         come, or a negative READ_FAULT when an event disagrees with the
 *         contract or with what arrived before; an error event's status is
 *         in `*st`.
 */
static int take_event(struct etude_remote *r, struct collect *c, struct wire_frame *f,
                      enum etude_remote_status *st)
{
    const char *t = f->type;
    int ok;

    if (strcmp(t, ETUDE_EV_ERROR) == 0 && f->nfields == 2) {
        fail(r, "the responder refused the run: %s", f->field[1]);
        *st = error_status(f->field[1]);
        return 1;
    }
    if (f->nfields < 2 || strcmp(f->field[1], c->res->attempt) != 0)
        ok = -1;
    else if (strcmp(t, ETUDE_EV_STARTED) == 0)
        ok = take_started(c, f);
    else if (strcmp(t, ETUDE_EV_OUT) == 0)
        ok = take_observed_output(r, c, f);
    else if (strcmp(t, ETUDE_EV_EXIT) == 0) {
        int had = c->have_exit;
        ok = take_exit(c, f);
        if (!ok && !had && r->conf.observer && r->conf.observer->stage)
            r->conf.observer->stage(r->conf.observer->context, "capture");
    } else if (strcmp(t, ETUDE_EV_STREAM) == 0)
        ok = take_stream(c, f);
    else if (strcmp(t, ETUDE_EV_USAGE) == 0)
        ok = take_usage(c, f);
    else if (strcmp(t, ETUDE_EV_CLEANED) == 0)
        ok = take_cleaned(c, f);
    else
        ok = -1;
    if (ok != 0) {
        fail(r, "the responder sent a %s that disagrees with the attempt", t);
        return READ_FAULT;
    }
    return c->have_exit && c->have_stream[0] && c->have_stream[1] && c->have_usage &&
           c->have_cleaned;
}

/** Check the output that arrived against the sizes and digests of its
 * streams. */
static int output_matches(struct collect *c)
{
    struct etude_output *o;
    char hex[65];
    int fd;

    if (!c->sizes_known[0] || !c->sizes_known[1])
        return c->res->end == ETUDE_END_LOST;
    for (fd = 0; fd < 2; fd++) {
        o = fd == 0 ? &c->res->out : &c->res->err;
        digest_hex(o->data != NULL ? o->data : "", o->kept, hex);
        if (o->kept != c->sent[fd] || strcmp(hex, c->sha[fd]) != 0)
            return 0;
    }
    return 1;
}

/** Give a positive integer member of `o`, or 0 when it is not one. */
static uint64_t positive(const struct json *o, const char *name)
{
    const struct json *v = json_member(o, name);

    return v != NULL && v->type == JSON_INTEGER && v->integer > 0 ? (uint64_t)v->integer : 0;
}

/* The times that bound a run, in monotonic microseconds. */
struct run_clock {
    uint64_t deadline; /* the invocation's deadline, from its start */
    uint64_t grace;    /* the invocation's grace period */
    uint64_t until;    /* when the caller's deadline passes, or 0 */
    uint64_t limit;    /* when the controller gives the attempt up */
    int cancelled;
};

/**
 * Check that an invocation is of this run, and take its attempt and bounds.
 *
 * @return ETUDE_REMOTE_OK, or ETUDE_REMOTE_REFUSED for an invocation that
 *         is not one this controller sends.
 */
static enum etude_remote_status check_invocation(struct etude_remote *r, const char *invocation,
                                                 size_t len, struct collect *c, struct run_clock *k)
{
    char err[200], tok[ETUDE_FRAME_TOKEN_LEN + 1] = "";
    struct json *doc = NULL;
    const char *id, *run, *token;

    err[0] = '\0';
    if (json_parse(invocation, len, 1, &doc, err, sizeof(err)) != 0 || doc->type != JSON_OBJECT) {
        json_free(doc);
        fail(r, "the invocation is not a JSON object: %s", err);
        return ETUDE_REMOTE_REFUSED;
    }
    id = json_text(doc, "attempt_id");
    run = json_text(doc, "run_id");
    token = json_text(doc, "attempt_token");
    c->output_max = (size_t)positive(doc, "output_max_bytes");
    k->deadline = positive(doc, "deadline_us");
    k->grace = positive(doc, "grace_us");
    if (id != NULL)
        token_of(id, tok);
    if (run == NULL || strcmp(run, r->conf.run_id) != 0 || id == NULL || token == NULL ||
        strcmp(token, tok) != 0 || c->output_max == 0 || k->deadline == 0 || k->grace == 0) {
        json_free(doc);
        fail(r, "the invocation is not of %s, or lacks its attempt or bounds", r->conf.run_id);
        return ETUDE_REMOTE_REFUSED;
    }
    json_free(doc);
    memcpy(c->res->attempt, tok, sizeof(tok));
    return ETUDE_REMOTE_OK;
}

/**
 * Make the connection again during a run, in the boot the attempt ran in.
 *
 * @return ETUDE_REMOTE_OK, ETUDE_REMOTE_BOOT when the responder's machine
 *         booted again, or the failure once the reconnections are spent.
 */
static enum etude_remote_status rejoin(struct etude_remote *r, struct etude_remote_result *res)
{
    enum etude_remote_status st;

    do {
        if (res->reconnects >= r->conf.reconnects)
            return ETUDE_REMOTE_LOST;
        res->reconnects++;
        st = reconnect(r);
    } while (transient(st));
    if (st != ETUDE_REMOTE_OK)
        return st;
    if (strcmp(r->boot, res->boot) != 0) {
        fail(r, "the responder's machine booted again; the end of attempt %s is lost",
             res->attempt);
        return ETUDE_REMOTE_BOOT;
    }
    return ETUDE_REMOTE_OK;
}

/**
 * Cancel the attempt, its caller's deadline having passed, and bring the
 * limit to the grace period and two stages from now. The CANCEL is written
 * within a stage of its own, past the cancellation and the observer that
 * asked for it.
 *
 * @return 0 when sent, 1 when the connection was lost, or -1 when the
 *         journal could not record the cancel.
 */
static int send_cancel(struct etude_remote *r, struct collect *c, struct run_clock *k)
{
    char cmd[24];
    const char *a[2];
    const struct etude_remote_observer *observer = r->conf.observer;
    uint64_t end;
    int lost;

    a[0] = c->res->attempt;
    if (new_cmd(r, cmd, ETUDE_CMD_CANCEL, a, 1) != 0)
        return -1;
    a[0] = cmd;
    a[1] = c->res->attempt;
    r->conf.observer = NULL;
    r->cancelling = 1;
    lost = send_frame(r, ETUDE_CMD_CANCEL, a, 2) != 0;
    r->cancelling = 0;
    if (lost) {
        r->conf.observer = observer;
        return 1;
    }
    k->cancelled = 1;
    c->res->cancelled = 1;
    end = posix_now_us() + k->grace + 2 * r->conf.stage_us;
    if (end < k->limit)
        k->limit = end;
    return 0;
}

/** Give the status of a run whose events are all there. */
static enum etude_remote_status end_run(struct etude_remote *r, struct collect *c)
{
    if (!output_matches(c)) {
        fail(r, "the output of attempt %s does not match its sizes and digests", c->res->attempt);
        return ETUDE_REMOTE_DAMAGED;
    }
    c->res->complete = 1;
    return ETUDE_REMOTE_OK;
}

/**
 * Read the events of the run on this connection, cancelling the attempt at
 * the caller's deadline.
 *
 * @return 1 with the run's status in `*st`, or 0 when the connection was
 *         lost and the run is to be sent again on the next.
 */
static int cancel_due(struct etude_remote *r, const struct run_clock *k)
{
    return !k->cancelled && ((k->until && posix_now_us() >= k->until) ||
                             (r->conf.cancel_requested && *r->conf.cancel_requested) ||
                             (r->conf.observer && r->conf.observer->expired &&
                              r->conf.observer->expired(r->conf.observer->context)));
}

static int follow(struct etude_remote *r, struct collect *c, struct run_clock *k,
                  enum etude_remote_status *st)
{
    struct wire_frame f;
    uint64_t wake;
    int rc;

    for (;;) {
        if (cancel_due(r, k)) {
            rc = send_cancel(r, c, k);
            if (rc != 0) {
                *st = ETUDE_REMOTE_SYSTEM;
                return rc < 0;
            }
        }
        wake = !k->cancelled && k->until != 0 && k->until < k->limit ? k->until : k->limit;
        if (!k->cancelled && r->conf.cancel_requested && wake > posix_now_us() + 50000)
            wake = posix_now_us() + 50000;
        rc = read_frame(r, wake, &f);
        if (rc == READ_TIME && wake == k->limit) {
            fail(r, "attempt %s did not end within its deadline and grace", c->res->attempt);
            transport_stop(r);
            *st = ETUDE_REMOTE_LOST;
            return 1;
        }
        if (rc == READ_TIME)
            continue;
        if (rc != READ_FRAME)
            return 0;
        if (f.nfields < 1 || strcmp(f.field[0], c->cmd) != 0) {
            evidence(r, "an event of another command", f.type, strlen(f.type));
            continue;
        }
        *st = ETUDE_REMOTE_OK;
        rc = take_event(r, c, &f, st);
        if (rc == READ_FAULT) {
            transport_stop(r);
            *st = ETUDE_REMOTE_PROTOCOL;
        } else if (rc == 1 && *st == ETUDE_REMOTE_OK) {
            *st = end_run(r, c);
        }
        if (rc != 0 || *st != ETUDE_REMOTE_OK)
            return 1;
    }
}

enum etude_remote_status etude_remote_run(struct etude_remote *r, const char *invocation,
                                          size_t len, uint64_t deadline_us,
                                          struct etude_remote_result *res)
{
    enum etude_remote_status st;
    struct run_clock k;
    struct collect c;
    char sha[65], cmd[24];
    const char *a[3];
    uint64_t now;

    memset(res, 0, sizeof(*res));
    res->end = ETUDE_END_LOST;
    res->value = -1;
    res->cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    res->cpu_us.method = res->peak_bytes.method = "the end did not arrive";
    memset(&c, 0, sizeof(c));
    memset(&k, 0, sizeof(k));
    c.res = res;
    st = check_invocation(r, invocation, len, &c, &k);
    if (st == ETUDE_REMOTE_OK)
        st = etude_remote_put(r, ETUDE_REMOTE_INVOCATION, invocation, len, "0600");
    if (st != ETUDE_REMOTE_OK)
        return st;
    memcpy(res->boot, r->boot, sizeof(res->boot));
    digest_hex(invocation, len, sha);
    a[0] = res->attempt;
    a[1] = sha;
    if (new_cmd(r, cmd, ETUDE_CMD_RUN, a, 2) != 0)
        return ETUDE_REMOTE_SYSTEM;
    c.cmd = cmd;
    now = posix_now_us();
    k.until = deadline_us != 0 ? now + deadline_us : 0;
    k.limit = now + k.deadline + k.grace + 2 * r->conf.stage_us;
    for (;;) {
        if (!r->connected && (st = rejoin(r, res)) != ETUDE_REMOTE_OK)
            return st;
        a[0] = cmd;
        a[1] = res->attempt;
        a[2] = sha;
        if (send_frame(r, ETUDE_CMD_RUN, a, 3) == 0 && follow(r, &c, &k, &st))
            return st;
    }
}

/**
 * Take a get's `data CMD OFFSET BASE64` into `buf`, which has `*got` of at
 * most `max` bytes.
 *
 * @return 0, or -1 when the data are out of order, not base64 or too long.
 */
static int take_data(const struct wire_frame *f, unsigned char *buf, size_t max, size_t *got)
{
    unsigned char *part;
    uint64_t offset;
    long n;

    if (decimal(f->field[1], &offset) != 0 || offset != *got)
        return -1;
    part = malloc(strlen(f->field[2]) / 4 * 3 + 3);
    if (part == NULL)
        return -1;
    n = wire_unbase64(f->field[2], part);
    if (n <= 0 || (size_t)n > max - *got) {
        free(part);
        return -1;
    }
    memcpy(buf + *got, part, (size_t)n);
    *got += (size_t)n;
    free(part);
    return 0;
}

enum etude_remote_status etude_remote_get(struct etude_remote *r, const char *attempt,
                                          const char *name, size_t max, char **data, size_t *size)
{
    enum etude_remote_status st = ETUDE_REMOTE_LOST;
    struct wire_frame f;
    char cmd[24], hex[65], full[ETUDE_FRAME_TOKEN_LEN + 6 + 130];
    const char *a[2];
    unsigned char *buf = NULL;
    uint64_t want;
    size_t got = 0;
    unsigned tries;
    int rc;

    *data = NULL;
    *size = 0;
    if (strlen(attempt) != ETUDE_FRAME_TOKEN_LEN || strlen(name) > 128 || strchr(name, '/')) {
        fail(r, "no file of a workspace is named %s", name);
        return ETUDE_REMOTE_REFUSED;
    }
    snprintf(full, sizeof(full), "%s/work/%s", attempt, name);
    for (tries = 0; tries <= r->conf.reconnects; tries++) {
        if (!r->connected) {
            st = reconnect(r);
            if (st != ETUDE_REMOTE_OK) {
                if (transient(st))
                    continue;
                return st;
            }
        }
        a[0] = full;
        if (new_cmd(r, cmd, ETUDE_CMD_GET, a, 1) != 0)
            return ETUDE_REMOTE_SYSTEM;
        a[0] = cmd;
        a[1] = full;
        if (send_frame(r, ETUDE_CMD_GET, a, 2) != 0)
            continue;
        free(buf);
        buf = malloc(max + 1);
        if (buf == NULL)
            return ETUDE_REMOTE_SYSTEM;
        got = 0;
        for (;;) {
            rc = wait_answer(r, cmd, posix_now_us() + r->conf.stage_us, &f);
            if (rc != READ_FRAME) {
                st = read_status(rc);
                break;
            }
            if (strcmp(f.type, ETUDE_EV_ERROR) == 0 && f.nfields == 2) {
                free(buf);
                fail(r, "the responder refused the get: %s", f.field[1]);
                return error_status(f.field[1]);
            }
            if (strcmp(f.type, ETUDE_CMD_DATA) == 0 && f.nfields == 3) {
                if (take_data(&f, buf, max, &got) == 0)
                    continue;
                free(buf);
                fail(r, "the data of %s arrived out of order, damaged or too long", name);
                return ETUDE_REMOTE_DAMAGED;
            }
            if (strcmp(f.type, ETUDE_EV_SENT) == 0 && f.nfields == 4) {
                digest_hex(buf, got, hex);
                if (strcmp(f.field[1], full) != 0 || decimal(f.field[2], &want) != 0 ||
                    want != got || strcmp(f.field[3], hex) != 0) {
                    free(buf);
                    fail(r, "%s arrived with another size or digest than it was sent with", name);
                    return ETUDE_REMOTE_DAMAGED;
                }
                buf[got] = '\0';
                *data = (char *)buf;
                *size = got;
                return ETUDE_REMOTE_OK;
            }
            free(buf);
            fail(r, "the responder answered a get with %s", f.type);
            transport_stop(r);
            return ETUDE_REMOTE_PROTOCOL;
        }
    }
    free(buf);
    return st;
}

enum etude_remote_status etude_remote_remove(struct etude_remote *r, const char *attempt)
{
    enum etude_remote_status st = ETUDE_REMOTE_LOST;
    struct wire_frame f;
    char cmd[24];
    const char *a[2];
    unsigned tries;
    int rc;

    a[0] = attempt;
    if (new_cmd(r, cmd, ETUDE_CMD_REMOVE, a, 1) != 0)
        return ETUDE_REMOTE_SYSTEM;
    for (tries = 0; tries <= r->conf.reconnects; tries++) {
        if (!r->connected) {
            st = reconnect(r);
            if (st != ETUDE_REMOTE_OK) {
                if (transient(st))
                    continue;
                return st;
            }
        }
        a[0] = cmd;
        a[1] = attempt;
        if (send_frame(r, ETUDE_CMD_REMOVE, a, 2) != 0)
            continue;
        rc = wait_answer(r, cmd, posix_now_us() + r->conf.stage_us, &f);
        if (rc != READ_FRAME) {
            st = read_status(rc);
            continue;
        }
        if (strcmp(f.type, ETUDE_EV_REMOVED) == 0 && f.nfields == 2 &&
            strcmp(f.field[1], attempt) == 0)
            return ETUDE_REMOTE_OK;
        if (strcmp(f.type, ETUDE_EV_ERROR) == 0 && f.nfields == 2) {
            fail(r, "the responder refused the remove: %s", f.field[1]);
            return error_status(f.field[1]);
        }
        fail(r, "the responder answered a remove with %s", f.type);
        transport_stop(r);
        return ETUDE_REMOTE_PROTOCOL;
    }
    return st;
}

/* Do not discard an unconfirmed native Job's pending overlapped buffers. */
static void remote_free(struct etude_remote *r)
{
    if (r->journal >= 0)
        close(r->journal);
    if (r->transcript)
        fclose(r->transcript);
#ifdef _WIN32
    if (r->supervisor_open)
        return;
#endif
    free(r->in);
    free(r);
}

enum etude_remote_status etude_remote_abandon(struct etude_remote *r)
{
    transport_stop(r);
    enum etude_remote_status status = r->transport_done ? ETUDE_REMOTE_OK : ETUDE_REMOTE_SYSTEM;
    remote_free(r);
    return status;
}

enum etude_remote_status etude_remote_close(struct etude_remote *r)
{
    enum etude_remote_status st = ETUDE_REMOTE_LOST;
    struct wire_frame f;
    char cmd[24];
    const char *a[1] = {NULL};

    if (r->connected && new_cmd(r, cmd, ETUDE_CMD_BYE, a, 0) == 0) {
        a[0] = cmd;
        if (send_frame(r, ETUDE_CMD_BYE, a, 1) == 0 &&
            wait_answer(r, cmd, posix_now_us() + r->conf.stage_us, &f) == READ_FRAME &&
            strcmp(f.type, ETUDE_CMD_BYE) == 0)
            st = ETUDE_REMOTE_OK;
    }
    transport_stop(r);
    if (r->conf.require_transport_cleanup && (!r->transport_ok || !r->transport_done))
        st = ETUDE_REMOTE_SYSTEM;
    if (r->transcript && fflush(r->transcript))
        st = ETUDE_REMOTE_SYSTEM;
    remote_free(r);
    return st;
}

const char *etude_remote_why(const struct etude_remote *r)
{
    return r->why;
}

void etude_remote_result_free(struct etude_remote_result *res)
{
    free(res->out.data);
    free(res->err.data);
    res->out.data = res->err.data = NULL;
}

const char *etude_remote_status_text(enum etude_remote_status status)
{
    switch (status) {
    case ETUDE_REMOTE_OK:
        return "ok";
    case ETUDE_REMOTE_LOST:
        return "lost";
    case ETUDE_REMOTE_PROTOCOL:
        return "protocol";
    case ETUDE_REMOTE_VERSION:
        return "version";
    case ETUDE_REMOTE_BOOT:
        return "boot";
    case ETUDE_REMOTE_REFUSED:
        return "refused";
    case ETUDE_REMOTE_DAMAGED:
        return "damaged";
    case ETUDE_REMOTE_SYSTEM:
        return "system";
    }
    return "unknown";
}
