/*
 * serve.c: the responder of remote execution. It reads commands as frames on
 * one connection, stores the files they send after checking them, runs
 * attempts through the executor, and keeps a ledger of each command and the
 * events it caused, so that a command repeated on this connection or the
 * next one is answered again and not acted on twice.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

#include "../../../utils/sha256.h"
#include "../guest/wire.h"
#include "../json.h"
#include "etude/remote.h"
#include "posix.h"

/* The bytes of a frame besides an `out` or `data` frame's base64. */
#define FRAME_OVERHEAD 160

/* A command number, the decimal text of at most 19 digits. */
#define CMD_MAX 20

/* How long the loop waits for the executor while attempts run. */
#define SLICE_MS 20

/* The bytes a session's paths take after the state directory's: the
 * session, `work`, an attempt and a file's name of 128 bytes. */
#define STATE_ROOM 256

/* One attempt that runs, from its `started` to its `cleaned`. */
struct attempt {
    struct attempt *next;
    char token[ETUDE_FRAME_TOKEN_LEN + 1];
    char cmd[CMD_MAX + 1]; /* the run's command */
    int ledger;            /* the run's command record, open to append */
    struct etude_proc *proc;
    uint64_t deadline_at; /* when the responder cancels it, monotonic */
    int cancelled;
    size_t sent[2];        /* bytes of standard output and error sent */
    struct sha256 hash[2]; /* of the bytes sent */
    char **argv, **envp;
};

/* The put whose data are arriving. */
struct transfer {
    int open;
    int draining; /* a repeated put: its data are taken and not acted on */
    char cmd[CMD_MAX + 1];
    char text[512];  /* the put command, recorded when it is sealed */
    char digest[65]; /* its digest */
    char name[129];
    char sha[65];
    uint64_t size, got;
    struct sha256 h;
    int fd;
    char tmp[PATH_MAX];
};

struct server {
    const struct etude_serve_conf *conf;
    struct etude_exec *ex;
    struct etude_pid self;
    char session[ETUDE_FRAME_TOKEN_LEN + 1];
    int ready;
    size_t own_max; /* the largest frame this responder takes */
    size_t max;     /* the largest frame of the session */
    uint64_t file_max;
    uint64_t seq_out;   /* the SEQ of the next frame sent */
    int has_ack;        /* a frame of the controller's arrived */
    uint64_t seq_in;    /* the SEQ of the last one */
    uint64_t ack_in;    /* the highest ACK the controller gave */
    char dir[PATH_MAX]; /* STATE/SESSION */
    int lock;           /* the session's lock, -1 before the hello */
    char *in;           /* the line being read */
    size_t inlen;
    struct attempt *attempts;
    struct transfer put;
    int lost; /* the connection is lost or broken */
    int done; /* the controller said bye */
};

/** Write a diagnostic as `etude serve: WHAT`. */
static void say(struct server *s, const char *fmt, ...)
{
    va_list ap;

    if (s->conf->log == NULL)
        return;
    fputs("etude serve: ", s->conf->log);
    va_start(ap, fmt);
    vfprintf(s->conf->log, fmt, ap);
    va_end(ap);
    fputc('\n', s->conf->log);
    fflush(s->conf->log);
}

/** Append `line` and why it was not acted on to STATE/evidence. */
static void evidence(struct server *s, const char *why, const char *line, size_t len)
{
    char path[PATH_MAX];
    FILE *f;

    snprintf(path, sizeof(path), "%s/evidence", s->conf->state);
    f = fopen(path, "a");
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

/** Send one frame; after a failed write the connection is lost. */
static void send_frame(struct server *s, const char *type, const char *const *field, int n)
{
    char *buf;
    size_t len, max = s->max;

    if (s->lost)
        return;
    buf = malloc(max + 1);
    if (buf == NULL) {
        s->lost = 1;
        return;
    }
    len = wire_encode(buf, max + 1, s->session[0] != '\0' ? s->session : ETUDE_FRAME_NO_SESSION,
                      s->conf->boot, s->seq_out, s->has_ack, s->seq_in, type, n, field);
    if (len == 0 || write_all(s->conf->out, buf, len) != 0) {
        say(s, "cannot send %s%s", type, len == 0 ? ": it does not fit a frame" : "");
        s->lost = 1;
    }
    s->seq_out++;
    free(buf);
}

/** Join a type and its fields with spaces into `out`; -1 when too long. */
static int join(char *out, size_t cap, const char *type, const char *const *field, int n)
{
    size_t len = strlen(type), flen;
    int i;

    if (len >= cap)
        return -1;
    memcpy(out, type, len + 1);
    for (i = 0; i < n; i++) {
        flen = strlen(field[i]);
        if (len + 1 + flen >= cap)
            return -1;
        out[len++] = ' ';
        memcpy(out + len, field[i], flen + 1);
        len += flen;
    }
    return 0;
}

/**
 * Record an event in a command's record, when `ledger` is open, and send it.
 * `sync` makes the record durable before the frame leaves.
 */
static void emit(struct server *s, int ledger, int sync, const char *type, const char *const *field,
                 int n)
{
    char *line;
    size_t cap = s->max + 2;

    if (ledger >= 0) {
        line = malloc(cap);
        if (line == NULL || join(line, cap - 1, type, field, n) != 0) {
            free(line);
            s->lost = 1;
            return;
        }
        strcat(line, "\n");
        if (write_all(ledger, line, strlen(line)) != 0 || (sync && fsync(ledger) != 0)) {
            say(s, "cannot record %s", type);
            free(line);
            s->lost = 1;
            return;
        }
        free(line);
    }
    send_frame(s, type, field, n);
}

/** Send `error CMD CODE`, recorded when `ledger` is open. */
static void emit_error(struct server *s, int ledger, const char *cmd, const char *code)
{
    const char *f[2];

    f[0] = cmd;
    f[1] = code;
    emit(s, ledger, 1, ETUDE_EV_ERROR, f, 2);
}

/** Say whether `s` is a command number: decimal, no leading zero. */
static int is_cmd(const char *s)
{
    size_t n = strlen(s), i;

    if (n == 0 || n > CMD_MAX - 1 || (s[0] == '0' && n > 1))
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

/** Say whether `s` is `n` lowercase hexadecimal digits. */
static int is_hex(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return s[n] == '\0';
}

/** Say whether `s` is a file's name, as an invocation's names are. */
static int is_name(const char *s)
{
    size_t n = strlen(s), i;

    if (n == 0 || n > 128 ||
        !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') ||
          (s[0] >= '0' && s[0] <= '9')))
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '+' ||
              s[i] == '-'))
            return 0;
    return 1;
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

/** Give a file's SHA-256 in `hex` and its size in `*size`; 0 on success. */
static int file_digest(const char *path, char hex[65], uint64_t *size)
{
    unsigned char buf[65536], d[32];
    struct sha256 h;
    uint64_t total = 0;
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    sha256_init(&h);
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        sha256_update(&h, buf, (size_t)n);
        total += (uint64_t)n;
    }
    close(fd);
    if (n < 0)
        return -1;
    sha256_final(&h, d);
    sha256_hex(d, hex);
    *size = total;
    return 0;
}

/** Make a directory with mode 0700 unless it exists; 0 on success. */
static int make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

/** Format a path into `out`, of PATH_MAX bytes; -1 when it does not fit. */
static int pathf(char *out, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out, PATH_MAX, fmt, ap);
    va_end(ap);
    return n < 0 || n >= PATH_MAX ? -1 : 0;
}

/** Give the path of the session's file `a/b` in `out`; -1 if it is too long. */
static int session_path(struct server *s, char *out, const char *a, const char *b)
{
    return pathf(out, "%s/%s/%s", s->dir, a, b);
}

/** Read a file of at most `max` bytes, with a NUL after it; NULL on failure. */
static char *read_file(const char *path, size_t max, size_t *len)
{
    struct stat st;
    char *buf;
    ssize_t n;
    size_t got = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > max) {
        close(fd);
        return NULL;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    while (got < (size_t)st.st_size && (n = read(fd, buf + got, (size_t)st.st_size - got)) > 0)
        got += (size_t)n;
    close(fd);
    buf[got] = '\0';
    if (len != NULL)
        *len = got;
    return buf;
}

/** Remove a directory and everything in it, following no link; 0 on success. */
static int remove_tree(const char *path)
{
    char sub[PATH_MAX];
    struct dirent *e;
    struct stat st;
    DIR *d;
    int rc = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    d = opendir(path);
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub) ||
            remove_tree(sub) != 0)
            rc = -1;
    }
    closedir(d);
    return rmdir(path) == 0 ? rc : -1;
}

/*
 * The ledger. A command's record, `cmd/CMD`, has the SHA-256 of the command
 * on its first line, the command on its second, and the events it caused,
 * one on each line after them. An attempt's record, `att/ATTEMPT`, has the
 * line `run CMD` and, once the process is registered, `process PID START`.
 */

/**
 * Look up a command's record.
 *
 * @return 0 when there is none, 1 when it has the same digest, 2 when it
 *         has another, and -1 when it cannot be read.
 */
static int ledger_find(struct server *s, const char *cmd, const char *digest)
{
    char path[PATH_MAX], line[80];
    FILE *f;
    int rc;

    session_path(s, path, "cmd", cmd);
    f = fopen(path, "r");
    if (f == NULL)
        return errno == ENOENT ? 0 : -1;
    rc = fgets(line, sizeof(line), f) == NULL ? -1 : strncmp(line, digest, 64) == 0 ? 1 : 2;
    fclose(f);
    return rc;
}

/**
 * Make a command's record before the command acts.
 *
 * @return the record, open to append, or -1.
 */
static int ledger_begin(struct server *s, const char *cmd, const char *digest, const char *text)
{
    char path[PATH_MAX];
    int fd;

    session_path(s, path, "cmd", cmd);
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (write_all(fd, digest, 64) != 0 || write_all(fd, "\n", 1) != 0 ||
        write_all(fd, text, strlen(text)) != 0 || write_all(fd, "\n", 1) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

/**
 * Split an event line of a record into its type and fields, in place.
 *
 * @return the count of fields, or -1 when the line is not an event.
 */
static int split_event(char *line, char **type, char **field)
{
    char *p;
    int n = 0;

    p = strchr(line, '\n');
    if (p == NULL)
        return -1;
    *p = '\0';
    *type = line;
    for (p = strchr(line, ' '); p != NULL; p = strchr(p, ' ')) {
        if (n == WIRE_FIELDS_MAX)
            return -1;
        *p++ = '\0';
        field[n++] = p;
    }
    return n;
}

/**
 * Read the events of a command's record.
 *
 * Calls `each` with every event in order; stops early when it returns
 * nonzero.
 *
 * @return 0, or -1 when the record cannot be read.
 */
static int ledger_events(struct server *s, const char *cmd,
                         int (*each)(void *ctx, const char *type, char **field, int n), void *ctx)
{
    char path[PATH_MAX], *line, *type, *field[WIRE_FIELDS_MAX];
    size_t cap = s->own_max + 2;
    FILE *f;
    int i = 0, n;

    session_path(s, path, "cmd", cmd);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    line = malloc(cap);
    if (line == NULL) {
        fclose(f);
        return -1;
    }
    while (fgets(line, (int)cap, f) != NULL) {
        if (i++ < 2)
            continue;
        n = split_event(line, &type, field);
        if (n >= 0 && each(ctx, type, field, n) != 0)
            break;
    }
    free(line);
    fclose(f);
    return 0;
}

/** Send one recorded event again. */
static int replay_one(void *ctx, const char *type, char **field, int n)
{
    struct server *s = ctx;

    send_frame(s, type, (const char *const *)field, n);
    return s->lost;
}

/* What a run's record says of its attempt. */
struct run_state {
    int started, ended, stream[2], usage, cleaned, confirmed, error;
};

/** Note one event of a run's record. */
static int note_run(void *ctx, const char *type, char **field, int n)
{
    struct run_state *st = ctx;

    (void)field;
    if (strcmp(type, ETUDE_EV_STARTED) == 0)
        st->started = 1;
    else if (strcmp(type, ETUDE_EV_EXIT) == 0)
        st->ended = 1;
    else if (strcmp(type, ETUDE_EV_STREAM) == 0 && n == 6 &&
             (strcmp(field[2], "1") == 0 || strcmp(field[2], "2") == 0))
        st->stream[field[2][0] - '1'] = 1;
    else if (strcmp(type, ETUDE_EV_USAGE) == 0)
        st->usage = 1;
    else if (strcmp(type, ETUDE_EV_ERROR) == 0)
        st->error = 1;
    else if (strcmp(type, ETUDE_EV_CLEANED) == 0 && n == 3) {
        st->cleaned = 1;
        st->confirmed = strcmp(field[2], "confirmed") == 0;
    }
    return 0;
}

/** Read the run command of an attempt's record into `cmd`; 0 when found. */
static int attempt_run(struct server *s, const char *token, char cmd[CMD_MAX + 1])
{
    char path[PATH_MAX], line[64];
    FILE *f;
    int rc = -1;

    session_path(s, path, "att", token);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    if (fgets(line, sizeof(line), f) != NULL && strncmp(line, "run ", 4) == 0) {
        line[strcspn(line, "\n")] = '\0';
        if (is_cmd(line + 4)) {
            strcpy(cmd, line + 4);
            rc = 0;
        }
    }
    fclose(f);
    return rc;
}

/**
 * Give an attempt's state from the ledger: unknown, preparing, running,
 * ended or cleaned, with its run command in `cmd`.
 */
static const char *attempt_state(struct server *s, const char *token, char cmd[CMD_MAX + 1])
{
    struct run_state st;
    struct attempt *a;

    if (attempt_run(s, token, cmd) != 0)
        return "unknown";
    for (a = s->attempts; a != NULL; a = a->next)
        if (strcmp(a->token, token) == 0)
            return a->proc == NULL ? "preparing" : "running";
    memset(&st, 0, sizeof(st));
    if (ledger_events(s, cmd, note_run, &st) != 0)
        return "unknown";
    if (st.cleaned)
        return st.confirmed ? "cleaned" : "ended";
    if (st.error || st.ended)
        return "ended";
    return st.started ? "running" : "preparing";
}

/** Append an event to a command's record; -1 when too long or not written. */
static int record_event(int fd, const char *type, const char *const *field, int n)
{
    char line[256];
    size_t len;

    if (join(line, sizeof(line) - 1, type, field, n) != 0)
        return -1;
    len = strlen(line);
    line[len++] = '\n';
    return write_all(fd, line, len);
}

/**
 * Record the end of an attempt that no responder supervises: its end lost,
 * its output's sizes and its usage unknown and its cleanup unconfirmed, or
 * the run failed when the attempt never started.
 */
static void record_lost(int fd, const char *cmd, const char *token, const struct run_state *st)
{
    const char *f[6] = {cmd, token, "lost", "-", "-", "-"};

    if (!st->started) {
        f[1] = ETUDE_ERR_FAILED;
        record_event(fd, ETUDE_EV_ERROR, f, 2);
        return;
    }
    if (!st->ended)
        record_event(fd, ETUDE_EV_EXIT, f, 4);
    f[2] = "1";
    if (!st->stream[0])
        record_event(fd, ETUDE_EV_STREAM, f, 6);
    f[2] = "2";
    if (!st->stream[1])
        record_event(fd, ETUDE_EV_STREAM, f, 6);
    f[2] = "-";
    if (!st->usage)
        record_event(fd, ETUDE_EV_USAGE, f, 6);
    f[2] = "unconfirmed";
    record_event(fd, ETUDE_EV_CLEANED, f, 3);
}

/**
 * Finish the attempts of the session that no responder supervises: those
 * whose run has no end recorded, left by a responder that ended without
 * recording it.
 */
static void recover(struct server *s)
{
    char path[PATH_MAX], cmd[CMD_MAX + 1];
    struct run_state st;
    struct dirent *e;
    DIR *d;
    int fd;

    session_path(s, path, "att", "");
    d = opendir(path);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (!is_hex(e->d_name, ETUDE_FRAME_TOKEN_LEN) || attempt_run(s, e->d_name, cmd) != 0)
            continue;
        memset(&st, 0, sizeof(st));
        if (ledger_events(s, cmd, note_run, &st) != 0 || st.cleaned || st.error)
            continue;
        session_path(s, path, "cmd", cmd);
        fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0)
            continue;
        say(s, "attempt %s: its responder ended before its end; recorded as lost", e->d_name);
        record_lost(fd, cmd, e->d_name, &st);
        fsync(fd);
        close(fd);
    }
    closedir(d);
}

/* An invocation as do_run reads it. */
struct invocation {
    struct json *doc;
    const char *program;
    const char *stdin_name; /* NULL for /dev/null */
    const struct json *arguments, *environment, *files;
    uint64_t deadline_us, grace_us, output_max;
};

/** Give the first ETUDE_FRAME_TOKEN_LEN digits of the SHA-256 of `text`. */
static void token_of(const char *text, char token[ETUDE_FRAME_TOKEN_LEN + 1])
{
    char hex[65];

    digest_hex(text, strlen(text), hex);
    memcpy(token, hex, ETUDE_FRAME_TOKEN_LEN);
    token[ETUDE_FRAME_TOKEN_LEN] = '\0';
}

/** Give a positive integer member of `o`, or 0 when it is not one. */
static uint64_t positive(const struct json *o, const char *name)
{
    const struct json *v = json_member(o, name);

    return v != NULL && v->type == JSON_INTEGER && v->integer > 0 ? (uint64_t)v->integer : 0;
}

/** Find the file `name` among an invocation's files, or NULL. */
static const struct json *find_file(const struct json *files, const char *name)
{
    size_t i;

    for (i = 0; i < files->count; i++)
        if (strcmp(json_text(files->item[i], "name"), name) == 0)
            return files->item[i];
    return NULL;
}

/* The members of an invocation of version 1. */
static const char *const members[] = {
    "schema_version", "run_id",    "attempt_id",  "attempt_token", "case_id",
    "program",        "arguments", "environment", "stdin",         "role",
    "files",          "fixtures",  "deadline_us", "grace_us",      "output_max_bytes"};

/** Check that an invocation has the members of version 1 and no other. */
static const char *check_members(const struct json *d)
{
    const struct json *v;
    size_t i, k, n = sizeof(members) / sizeof(members[0]);

    if (d->type != JSON_OBJECT || d->count != n)
        return "the invocation does not have the members of version 1";
    for (i = 0; i < d->count; i++) {
        for (k = 0; k < n; k++)
            if (strcmp(d->item[i]->name, members[k]) == 0)
                break;
        if (k == n)
            return "the invocation has a member version 1 does not define";
    }
    v = json_member(d, "schema_version");
    if (v == NULL || v->type != JSON_INTEGER || v->integer != 1)
        return "the invocation is not of version 1";
    return NULL;
}

/** Check that an invocation is of this session's run and of attempt `token`. */
static const char *check_owner(struct server *s, const struct json *d, const char *token)
{
    char tok[ETUDE_FRAME_TOKEN_LEN + 1];
    const struct json *v;
    const char *t;
    size_t i;

    t = json_text(d, "run_id");
    if (t == NULL)
        return "the invocation has no run";
    token_of(t, tok);
    if (strcmp(tok, s->session) != 0)
        return "the invocation belongs to another run";
    t = json_text(d, "attempt_id");
    if (t == NULL || json_text(d, "attempt_token") == NULL)
        return "the invocation has no attempt";
    token_of(t, tok);
    if (strcmp(tok, token) != 0 || strcmp(json_text(d, "attempt_token"), token) != 0)
        return "the invocation is of another attempt";
    t = json_text(d, "role");
    if (t == NULL || strcmp(t, "ordinary") != 0)
        return "a remote invocation must have the ordinary role";
    v = json_member(d, "fixtures");
    if (v == NULL || v->type != JSON_ARRAY)
        return "the invocation has no fixtures";
    for (i = 0; i < v->count; i++) {
        t = json_text(v->item[i], "fixture");
        if (t == NULL || strcmp(t, "workspace") != 0)
            return "the invocation asks for a fixture this responder does not make";
    }
    return NULL;
}

/** Say whether an entry of an invocation's files is of the contract. */
static int file_entry(const struct json *f)
{
    const struct json *size = json_member(f, "size_bytes");
    const char *name = json_text(f, "name"), *sha = json_text(f, "sha256");
    const char *mode = json_text(f, "mode");

    return f->type == JSON_OBJECT && name != NULL && is_name(name) && sha != NULL &&
           is_hex(sha, 64) && mode != NULL && strlen(mode) == 4 && strspn(mode, "01234567") == 4 &&
           size != NULL && size->type == JSON_INTEGER && size->integer >= 0;
}

/** Check an invocation's files, each of the contract and each named once. */
static const char *check_files(const struct json *files)
{
    size_t i, j;

    if (files == NULL || files->type != JSON_ARRAY)
        return "the invocation has no files";
    for (i = 0; i < files->count; i++) {
        if (!file_entry(files->item[i]))
            return "a file of the invocation is not of the contract";
        for (j = 0; j < i; j++)
            if (strcmp(json_text(files->item[j], "name"), json_text(files->item[i], "name")) == 0)
                return "the invocation names a file twice";
    }
    return NULL;
}

/** Check an invocation's program, input, arguments, environment and bounds. */
static const char *check_program(const struct json *d, struct invocation *inv)
{
    const struct json *v;
    size_t i;

    inv->program = json_text(d, "program");
    if (inv->program == NULL || find_file(inv->files, inv->program) == NULL)
        return "the invocation's program is not among its files";
    v = json_member(d, "stdin");
    if (v == NULL || (v->type != JSON_NULL && v->type != JSON_STRING))
        return "the invocation's standard input is not of the contract";
    if (v->type == JSON_STRING) {
        inv->stdin_name = v->string;
        if (find_file(inv->files, inv->stdin_name) == NULL)
            return "the invocation's standard input is not among its files";
    }
    inv->arguments = json_member(d, "arguments");
    if (inv->arguments == NULL || inv->arguments->type != JSON_ARRAY)
        return "the invocation has no arguments";
    for (i = 0; i < inv->arguments->count; i++)
        if (inv->arguments->item[i]->type != JSON_STRING)
            return "an argument of the invocation is not a string";
    inv->environment = json_member(d, "environment");
    if (inv->environment == NULL || inv->environment->type != JSON_OBJECT)
        return "the invocation has no environment";
    for (i = 0; i < inv->environment->count; i++)
        if (inv->environment->item[i]->type != JSON_STRING)
            return "a value of the invocation's environment is not a string";
    inv->deadline_us = positive(d, "deadline_us");
    inv->grace_us = positive(d, "grace_us");
    inv->output_max = positive(d, "output_max_bytes");
    if (inv->deadline_us == 0 || inv->grace_us == 0 || inv->output_max == 0 ||
        inv->output_max > SIZE_MAX / 2)
        return "the invocation's bounds are not of the contract";
    return NULL;
}

/**
 * Read and check an invocation for this session and attempt.
 *
 * @return NULL with `inv` filled, or what makes it one this responder
 *         refuses. The caller frees `inv->doc` in either case.
 */
static const char *load_invocation(struct server *s, const char *text, size_t len,
                                   const char *token, struct invocation *inv)
{
    char err[200];
    const char *why;

    memset(inv, 0, sizeof(*inv));
    if (json_parse(text, len, 1, &inv->doc, err, sizeof(err)) != 0)
        return "the invocation is not a JSON document of the contract";
    if ((why = check_members(inv->doc)) != NULL || (why = check_owner(s, inv->doc, token)) != NULL)
        return why;
    inv->files = json_member(inv->doc, "files");
    if ((why = check_files(inv->files)) != NULL)
        return why;
    return check_program(inv->doc, inv);
}

/** Free a vector of strings and the strings. */
static void free_vector(char **v)
{
    size_t i;

    if (v == NULL)
        return;
    for (i = 0; v[i] != NULL; i++)
        free(v[i]);
    free(v);
}

/** Give a copy of `value` with each `${WORKSPACE}` replaced, or NULL. */
static char *expand(const char *value, const char *workspace)
{
    static const char word[] = "${WORKSPACE}";
    size_t n = 1, wl = strlen(workspace);
    const char *p, *q;
    char *out, *o;

    for (p = value; (q = strstr(p, word)) != NULL; p = q + sizeof(word) - 1)
        n += (size_t)(q - p) + wl;
    n += strlen(p);
    out = malloc(n);
    if (out == NULL)
        return NULL;
    o = out;
    for (p = value; (q = strstr(p, word)) != NULL; p = q + sizeof(word) - 1) {
        memcpy(o, p, (size_t)(q - p));
        o += q - p;
        memcpy(o, workspace, wl);
        o += wl;
    }
    strcpy(o, p);
    return out;
}

/**
 * Copy the stored file `sha` to `path` with `mode`, and check the copy's
 * size and digest.
 *
 * @return NULL, or the error code to answer.
 */
static const char *stage_file(struct server *s, const char *sha, uint64_t size, const char *mode,
                              const char *path)
{
    char from[PATH_MAX], hex[65], buf[65536];
    uint64_t got;
    ssize_t n;
    int in, out, rc = 0;

    session_path(s, from, "blob", sha);
    in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return ETUDE_ERR_UNKNOWN;
    out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        return ETUDE_ERR_FAILED;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write_all(out, buf, (size_t)n) != 0)
            rc = -1;
    if (n < 0 || fchmod(out, (mode_t)strtoul(mode, NULL, 8)) != 0)
        rc = -1;
    close(in);
    if (close(out) != 0 || rc != 0)
        return ETUDE_ERR_FAILED;
    if (file_digest(path, hex, &got) != 0)
        return ETUDE_ERR_FAILED;
    return strcmp(hex, sha) != 0 || got != size ? ETUDE_ERR_DAMAGED : NULL;
}

/* The attempt's record, which the registration completes. */
struct registration {
    int fd;
};

/** Record the case's process before it runs. */
static int register_process(void *ctx, const char *attempt_id, const struct etude_pid *keeper,
                            const struct etude_pid *process)
{
    struct registration *reg = ctx;
    char line[96];

    (void)attempt_id;
    (void)keeper;
    snprintf(line, sizeof(line), "process %ld %llu\n", process->pid,
             (unsigned long long)process->start);
    if (write_all(reg->fd, line, strlen(line)) != 0 || fsync(reg->fd) != 0)
        return errno != 0 ? errno : EIO;
    return 0;
}

/** Make the vectors of an attempt's program; 0 on success. */
static int make_vectors(const struct invocation *inv, const char *workspace, struct attempt *a)
{
    const struct json *env = inv->environment, *args = inv->arguments;
    size_t i, n;

    a->argv = calloc(args->count + 2, sizeof(char *));
    a->envp = calloc(env->count + 1, sizeof(char *));
    if (a->argv == NULL || a->envp == NULL)
        return -1;
    n = strlen(workspace) + strlen(inv->program) + 2;
    a->argv[0] = malloc(n);
    if (a->argv[0] == NULL)
        return -1;
    snprintf(a->argv[0], n, "%s/%s", workspace, inv->program);
    for (i = 0; i < args->count; i++)
        if ((a->argv[i + 1] = expand(args->item[i]->string, workspace)) == NULL)
            return -1;
    for (i = 0; i < env->count; i++) {
        char *value = expand(env->item[i]->string, workspace);
        if (value == NULL)
            return -1;
        n = strlen(env->item[i]->name) + strlen(value) + 2;
        a->envp[i] = malloc(n);
        if (a->envp[i] == NULL) {
            free(value);
            return -1;
        }
        snprintf(a->envp[i], n, "%s=%s", env->item[i]->name, value);
        free(value);
    }
    return 0;
}

/** Free an attempt that has ended, or that never started. */
static void attempt_free(struct attempt *a)
{
    if (a->ledger >= 0)
        close(a->ledger);
    if (a->proc != NULL)
        etude_proc_free(a->proc);
    free_vector(a->argv);
    free_vector(a->envp);
    free(a);
}

/**
 * Read the stored invocation `sha` and check it for the attempt `token`.
 *
 * @return NULL with `inv` filled, or the error code to answer. The caller
 *         frees `inv->doc` in either case.
 */
static const char *read_invocation(struct server *s, const char *cmd, const char *token,
                                   const char *sha, struct invocation *inv)
{
    char path[PATH_MAX], hex[65];
    const char *why;
    char *text;
    size_t len;

    memset(inv, 0, sizeof(*inv));
    session_path(s, path, "blob", sha);
    text = read_file(path, s->file_max, &len);
    if (text == NULL)
        return ETUDE_ERR_UNKNOWN;
    digest_hex(text, len, hex);
    if (strcmp(hex, sha) != 0) {
        free(text);
        return ETUDE_ERR_DAMAGED;
    }
    why = load_invocation(s, text, len, token, inv);
    free(text);
    if (why != NULL) {
        say(s, "run %s: %s", cmd, why);
        return ETUDE_ERR_REFUSED;
    }
    return NULL;
}

/**
 * Make an attempt's record, `att/ATTEMPT`, with its run command.
 *
 * @return the record, open to append, or -1 with `*code` the error code to
 *         answer: a conflict when the attempt has a record already.
 */
static int record_attempt(struct server *s, const char *cmd, const char *token, const char **code)
{
    char path[PATH_MAX], line[64];
    int rec;

    session_path(s, path, "att", token);
    rec = open(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (rec < 0) {
        *code = errno == EEXIST ? ETUDE_ERR_CONFLICT : ETUDE_ERR_FAILED;
        return -1;
    }
    snprintf(line, sizeof(line), "run %s\n", cmd);
    if (write_all(rec, line, strlen(line)) != 0 || fsync(rec) != 0)
        *code = ETUDE_ERR_FAILED;
    return rec;
}

/** Make an attempt's workspace anew with its files; NULL or the error code. */
static const char *stage_files(struct server *s, const struct invocation *inv,
                               const char *workspace)
{
    char file[PATH_MAX];
    const char *code = NULL;
    const struct json *fl;
    size_t i;

    if (remove_tree(workspace) != 0 || mkdir(workspace, 0700) != 0)
        return ETUDE_ERR_FAILED;
    for (i = 0; code == NULL && i < inv->files->count; i++) {
        fl = inv->files->item[i];
        if (pathf(file, "%s/%s", workspace, json_text(fl, "name")) != 0)
            return ETUDE_ERR_FAILED;
        code =
            stage_file(s, json_text(fl, "sha256"), (uint64_t)json_member(fl, "size_bytes")->integer,
                       json_text(fl, "mode"), file);
    }
    return code;
}

/**
 * Start the program of an attempt, registered in its record `rec` before it
 * runs.
 *
 * @return NULL with `a->proc` set, or the error code to answer.
 */
static const char *start_program(struct server *s, const struct invocation *inv,
                                 const char *workspace, int rec, struct attempt *a)
{
    char file[PATH_MAX];
    struct registration reg;
    struct etude_exec_spec spec;

    if (make_vectors(inv, workspace, a) != 0)
        return ETUDE_ERR_FAILED;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = a->token;
    spec.argv = a->argv;
    spec.envp = a->envp;
    spec.cwd = workspace;
    if (inv->stdin_name != NULL) {
        if (pathf(file, "%s/%s", workspace, inv->stdin_name) != 0)
            return ETUDE_ERR_FAILED;
        spec.stdin_path = file;
    }
    spec.output_max = (size_t)inv->output_max;
    spec.grace_us = inv->grace_us;
    reg.fd = rec;
    spec.reg = register_process;
    spec.reg_ctx = &reg;
    if (etude_exec_start(s->ex, &spec, &a->proc) != 0) {
        say(s, "run %s: cannot start %s: %s", a->cmd, a->argv[0], strerror(errno));
        a->proc = NULL;
        return ETUDE_ERR_FAILED;
    }
    return NULL;
}

/** Run the stored invocation's attempt, as `run CMD ATTEMPT SHA256` asks. */
static void do_run(struct server *s, int ledger, const char *cmd, const char *token,
                   const char *sha)
{
    char workspace[PATH_MAX], pid[24], start[24];
    const char *code, *f[4];
    struct invocation inv;
    struct attempt *a = NULL;
    int rec = -1;

    code = read_invocation(s, cmd, token, sha, &inv);
    if (code == NULL)
        rec = record_attempt(s, cmd, token, &code);
    session_path(s, workspace, "work", token);
    if (code == NULL)
        code = stage_files(s, &inv, workspace);
    if (code == NULL && (a = calloc(1, sizeof(*a))) == NULL)
        code = ETUDE_ERR_FAILED;
    if (code == NULL) {
        strcpy(a->token, token);
        strcpy(a->cmd, cmd);
        a->ledger = -1;
        code = start_program(s, &inv, workspace, rec, a);
    }
    if (rec >= 0)
        close(rec);
    json_free(inv.doc);
    if (code != NULL) {
        if (a != NULL)
            attempt_free(a);
        if (rec >= 0)
            remove_tree(workspace);
        emit_error(s, ledger, cmd, code);
        close(ledger);
        return;
    }
    a->ledger = ledger;
    a->deadline_at = posix_now_us() + inv.deadline_us;
    sha256_init(&a->hash[0]);
    sha256_init(&a->hash[1]);
    a->next = s->attempts;
    s->attempts = a;
    snprintf(pid, sizeof(pid), "%ld", etude_proc_result(a->proc)->process.pid);
    snprintf(start, sizeof(start), "%llu",
             (unsigned long long)etude_proc_result(a->proc)->process.start);
    f[0] = cmd;
    f[1] = token;
    f[2] = pid;
    f[3] = start;
    emit(s, ledger, 1, ETUDE_EV_STARTED, f, 4);
}

/** Send the output of an attempt that has not been sent yet. */
static void stream_output(struct server *s, struct attempt *a)
{
    const struct etude_exec_result *r = etude_proc_result(a->proc);
    size_t chunk = (s->max - FRAME_OVERHEAD) / 4 * 3, n;
    const struct etude_output *o;
    char *b64, offset[24];
    const char *f[5];
    int fd;

    b64 = malloc(s->max);
    if (b64 == NULL) {
        s->lost = 1;
        return;
    }
    for (fd = 0; fd < 2; fd++) {
        o = fd == 0 ? &r->out : &r->err;
        while (a->sent[fd] < o->kept && !s->lost) {
            n = o->kept - a->sent[fd] < chunk ? o->kept - a->sent[fd] : chunk;
            wire_base64((const unsigned char *)o->data + a->sent[fd], n, b64);
            snprintf(offset, sizeof(offset), "%llu", (unsigned long long)a->sent[fd]);
            f[0] = a->cmd;
            f[1] = a->token;
            f[2] = fd == 0 ? "1" : "2";
            f[3] = offset;
            f[4] = b64;
            emit(s, a->ledger, 0, ETUDE_EV_OUT, f, 5);
            sha256_update(&a->hash[fd], (const unsigned char *)o->data + a->sent[fd], n);
            a->sent[fd] += n;
        }
    }
    free(b64);
}

/** Give a measurement as a `usage` field: N, N+ for a lower bound, or -. */
static void measure_field(const struct etude_measure *m, char *value, size_t cap,
                          const char **method)
{
    const char *p;

    *method = "-";
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE) {
        snprintf(value, cap, "-");
        return;
    }
    snprintf(value, cap, "%llu%s", (unsigned long long)m->value,
             m->quality == ETUDE_QUALITY_LOWER_BOUND ? "+" : "");
    for (p = m->method; p != NULL && *p > ' ' && *p <= '~'; p++)
        ;
    if (m->method != NULL && *m->method != '\0' && *p == '\0')
        *method = m->method;
}

/** Record and send the end of an attempt the executor finished, and free it. */
static void finish(struct server *s, struct attempt *a)
{
    const struct etude_exec_result *r = etude_proc_result(a->proc);
    char value[24], sizes[2][24], sum[65], cpu[24], peak[24];
    unsigned char d[32];
    struct attempt **pp;
    const char *f[6], *kind;
    int fd;

    stream_output(s, a);
    switch (r->end) {
    case ETUDE_END_EXITED:
        kind = "exited";
        break;
    case ETUDE_END_SIGNALED:
        kind = "signaled";
        break;
    case ETUDE_END_STOPPED:
        kind = "stopped";
        break;
    default:
        kind = "lost";
        break;
    }
    if (r->value >= 0 && (r->end == ETUDE_END_EXITED || r->end == ETUDE_END_SIGNALED))
        snprintf(value, sizeof(value), "%d", r->value);
    else
        strcpy(value, "-");
    f[0] = a->cmd;
    f[1] = a->token;
    f[2] = kind;
    f[3] = value;
    emit(s, a->ledger, 0, ETUDE_EV_EXIT, f, 4);
    for (fd = 0; fd < 2; fd++) {
        sha256_final(&a->hash[fd], d);
        sha256_hex(d, sum);
        snprintf(sizes[0], sizeof(sizes[0]), "%llu", (unsigned long long)a->sent[fd]);
        snprintf(sizes[1], sizeof(sizes[1]), "%llu",
                 (unsigned long long)(fd == 0 ? r->out.total : r->err.total));
        f[2] = fd == 0 ? "1" : "2";
        f[3] = sizes[0];
        f[4] = sizes[1];
        f[5] = sum;
        emit(s, a->ledger, 0, ETUDE_EV_STREAM, f, 6);
    }
    measure_field(&r->cpu_us, cpu, sizeof(cpu), &f[3]);
    measure_field(&r->peak_bytes, peak, sizeof(peak), &f[5]);
    f[2] = cpu;
    f[4] = peak;
    emit(s, a->ledger, 0, ETUDE_EV_USAGE, f, 6);
    f[2] = r->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed";
    emit(s, a->ledger, 1, ETUDE_EV_CLEANED, f, 3);
    for (pp = &s->attempts; *pp != NULL; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    attempt_free(a);
}

/** Answer `state CMD ATTEMPT STATE`, recorded when `ledger` is open. */
static void emit_state(struct server *s, int ledger, const char *cmd, const char *token,
                       const char *state)
{
    const char *f[3];

    f[0] = cmd;
    f[1] = token;
    f[2] = state;
    emit(s, ledger, 1, ETUDE_EV_STATE, f, 3);
}

/** Cancel an attempt, as `cancel CMD ATTEMPT` asks. */
static void do_cancel(struct server *s, int ledger, const char *cmd, const char *token)
{
    char run[CMD_MAX + 1];
    struct attempt *a;

    for (a = s->attempts; a != NULL; a = a->next)
        if (strcmp(a->token, token) == 0)
            break;
    if (a != NULL && !a->cancelled) {
        etude_exec_cancel(a->proc);
        a->cancelled = 1;
    }
    emit_state(s, ledger, cmd, token, attempt_state(s, token, run));
}

/** Give an attempt's state and its run's events again, as `status` asks. */
static void do_status(struct server *s, const char *cmd, const char *token)
{
    char run[CMD_MAX + 1];
    const char *state = attempt_state(s, token, run);

    emit_state(s, -1, cmd, token, state);
    if (strcmp(state, "unknown") != 0)
        ledger_events(s, run, replay_one, s);
}

/**
 * Give the path of the file that a get's NAME names: an attempt's token,
 * `/work/` and the name of a file of its workspace.
 *
 * @return NULL, or the error code to answer.
 */
static const char *get_path(struct server *s, const char *name, char *path)
{
    const char *file = name + ETUDE_FRAME_TOKEN_LEN + 6;
    char token[ETUDE_FRAME_TOKEN_LEN + 1], run[CMD_MAX + 1];
    struct stat st;

    if (strlen(name) <= ETUDE_FRAME_TOKEN_LEN + 6 ||
        strncmp(name + ETUDE_FRAME_TOKEN_LEN, "/work/", 6) != 0 || !is_name(file))
        return ETUDE_ERR_UNKNOWN;
    memcpy(token, name, ETUDE_FRAME_TOKEN_LEN);
    token[ETUDE_FRAME_TOKEN_LEN] = '\0';
    if (!is_hex(token, ETUDE_FRAME_TOKEN_LEN) ||
        strcmp(attempt_state(s, token, run), "unknown") == 0 ||
        pathf(path, "%s/work/%s/%s", s->dir, token, file) != 0 || lstat(path, &st) != 0 ||
        !S_ISREG(st.st_mode))
        return ETUDE_ERR_UNKNOWN;
    return (uint64_t)st.st_size > s->file_max ? ETUDE_ERR_LIMIT : NULL;
}

/** Send a file of an attempt's workspace, as `get CMD NAME` asks. */
static void do_get(struct server *s, const char *cmd, const char *name)
{
    char path[PATH_MAX], offset[24], size[24], hex[65], *b64;
    size_t chunk = (s->max - FRAME_OVERHEAD) / 4 * 3, len, at;
    const char *f[4], *code;
    unsigned char d[32];
    struct sha256 h;
    char *data;

    code = get_path(s, name, path);
    if (code != NULL) {
        emit_error(s, -1, cmd, code);
        return;
    }
    data = read_file(path, s->file_max, &len);
    b64 = malloc(s->max);
    if (data == NULL || b64 == NULL) {
        free(data);
        free(b64);
        emit_error(s, -1, cmd, ETUDE_ERR_FAILED);
        return;
    }
    sha256_init(&h);
    for (at = 0; at < len && !s->lost; at += chunk) {
        size_t n = len - at < chunk ? len - at : chunk;
        wire_base64((const unsigned char *)data + at, n, b64);
        sha256_update(&h, (const unsigned char *)data + at, n);
        snprintf(offset, sizeof(offset), "%llu", (unsigned long long)at);
        f[0] = cmd;
        f[1] = offset;
        f[2] = b64;
        emit(s, -1, 0, ETUDE_CMD_DATA, f, 3);
    }
    sha256_final(&h, d);
    sha256_hex(d, hex);
    snprintf(size, sizeof(size), "%llu", (unsigned long long)len);
    f[0] = cmd;
    f[1] = name;
    f[2] = size;
    f[3] = hex;
    emit(s, -1, 0, ETUDE_EV_SENT, f, 4);
    free(data);
    free(b64);
}

/** Remove an ended attempt's workspace, as `remove CMD ATTEMPT` asks. */
static void do_remove(struct server *s, int ledger, const char *cmd, const char *token)
{
    char path[PATH_MAX], run[CMD_MAX + 1];
    const char *state = attempt_state(s, token, run), *f[2];

    if (strcmp(state, "unknown") == 0) {
        emit_error(s, ledger, cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (strcmp(state, "preparing") == 0 || strcmp(state, "running") == 0) {
        emit_error(s, ledger, cmd, ETUDE_ERR_REFUSED);
        return;
    }
    session_path(s, path, "work", token);
    if (remove_tree(path) != 0) {
        emit_error(s, ledger, cmd, ETUDE_ERR_FAILED);
        return;
    }
    f[0] = cmd;
    f[1] = token;
    emit(s, ledger, 1, ETUDE_EV_REMOVED, f, 2);
}

/** Abandon the put in progress, removing what it wrote. */
static void abandon_put(struct server *s)
{
    if (s->put.open) {
        close(s->put.fd);
        unlink(s->put.tmp);
    }
    memset(&s->put, 0, sizeof(s->put));
    s->put.fd = -1;
}

/**
 * Make the put's record with its answer, `stored` or the error `code`, and
 * send the answer.
 */
static void end_put(struct server *s, const char *code)
{
    int ledger = ledger_begin(s, s->put.cmd, s->put.digest, s->put.text);
    const char *f[3];

    if (ledger < 0)
        say(s, "put %s: cannot record its answer", s->put.cmd);
    if (code != NULL) {
        emit_error(s, ledger, s->put.cmd, code);
    } else {
        f[0] = s->put.cmd;
        f[1] = s->put.name;
        f[2] = s->put.sha;
        emit(s, ledger, 1, ETUDE_EV_STORED, f, 3);
    }
    if (ledger >= 0)
        close(ledger);
}

/** Begin a put, as `put CMD NAME SIZE SHA256 MODE` asks; its data follow. */
static void do_put(struct server *s, const struct wire_frame *f, const char *text,
                   const char *digest)
{
    abandon_put(s);
    strcpy(s->put.cmd, f->field[0]);
    strcpy(s->put.text, text);
    strcpy(s->put.digest, digest);
    strcpy(s->put.name, f->field[1]);
    strcpy(s->put.sha, f->field[3]);
    decimal(f->field[2], &s->put.size);
    if (s->put.size > s->file_max) {
        s->put.draining = 1;
        end_put(s, ETUDE_ERR_LIMIT);
        return;
    }
    pathf(s->put.tmp, "%s/blob/%s.part", s->dir, s->put.cmd);
    s->put.fd = open(s->put.tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (s->put.fd < 0) {
        s->put.draining = 1;
        end_put(s, ETUDE_ERR_FAILED);
        return;
    }
    sha256_init(&s->put.h);
    s->put.open = 1;
}

/** Fail the put in progress with `code`; its later data are not acted on. */
static void fail_put(struct server *s, const char *code)
{
    close(s->put.fd);
    unlink(s->put.tmp);
    s->put.fd = -1;
    s->put.open = 0;
    s->put.draining = 1;
    end_put(s, code);
}

/**
 * Take one `data CMD OFFSET BASE64` of the put in progress.
 *
 * @return 0, or -1 when no put of that command is in progress.
 */
static int do_data(struct server *s, const struct wire_frame *f)
{
    unsigned char *buf;
    uint64_t offset;
    size_t cap;
    long n;

    if (f->nfields != 3 || (!s->put.open && !s->put.draining) ||
        strcmp(f->field[0], s->put.cmd) != 0)
        return -1;
    if (s->put.draining)
        return 0;
    cap = strlen(f->field[2]) / 4 * 3 + 3;
    buf = malloc(cap);
    if (buf == NULL) {
        fail_put(s, ETUDE_ERR_FAILED);
        return 0;
    }
    n = wire_unbase64(f->field[2], buf);
    if (n <= 0 || decimal(f->field[1], &offset) != 0 || offset != s->put.got ||
        (uint64_t)n > s->put.size - s->put.got) {
        free(buf);
        fail_put(s, ETUDE_ERR_DAMAGED);
        return 0;
    }
    if (write_all(s->put.fd, (const char *)buf, (size_t)n) != 0) {
        free(buf);
        fail_put(s, ETUDE_ERR_FAILED);
        return 0;
    }
    sha256_update(&s->put.h, buf, (size_t)n);
    s->put.got += (uint64_t)n;
    free(buf);
    return 0;
}

/**
 * End the put in progress, as `seal CMD` asks: store the file when its size
 * and digest are those the put gave, and remove it otherwise.
 *
 * @return 0, or -1 when no put of that command is in progress.
 */
static int do_seal(struct server *s, const struct wire_frame *f)
{
    char path[PATH_MAX], hex[65];
    unsigned char d[32];

    if (f->nfields != 1 || (!s->put.open && !s->put.draining) ||
        strcmp(f->field[0], s->put.cmd) != 0)
        return -1;
    if (s->put.open) {
        sha256_final(&s->put.h, d);
        sha256_hex(d, hex);
        if (s->put.got != s->put.size || strcmp(hex, s->put.sha) != 0) {
            fail_put(s, ETUDE_ERR_DAMAGED);
        } else {
            session_path(s, path, "blob", s->put.sha);
            if (fsync(s->put.fd) != 0 || close(s->put.fd) != 0 || rename(s->put.tmp, path) != 0) {
                s->put.fd = -1;
                unlink(s->put.tmp);
                end_put(s, ETUDE_ERR_FAILED);
            } else {
                s->put.fd = -1;
                end_put(s, NULL);
            }
            s->put.open = 0;
        }
    }
    memset(&s->put, 0, sizeof(s->put));
    s->put.fd = -1;
    return 0;
}

/** Cancel every running attempt and record each end as it comes. */
static void end_attempts(struct server *s)
{
    struct etude_proc *p;
    struct attempt *a;

    for (a = s->attempts; a != NULL; a = a->next)
        if (!a->cancelled) {
            etude_exec_cancel(a->proc);
            a->cancelled = 1;
        }
    while (s->attempts != NULL) {
        p = etude_exec_wait(s->ex, -1);
        if (p == NULL)
            continue;
        for (a = s->attempts; a != NULL; a = a->next)
            if (a->proc == p) {
                finish(s, a);
                break;
            }
    }
}

/** Remove the files the session stored, which only its runs read. */
static void remove_blobs(struct server *s)
{
    char path[PATH_MAX];

    pathf(path, "%s/blob", s->dir);
    remove_tree(path);
}

/** Take the session's lock, reading the connection meanwhile; 0 when taken. */
static int take_lock(struct server *s)
{
    struct pollfd pfd;
    char path[PATH_MAX];
    ssize_t n;

    pathf(path, "%s/lock", s->dir);
    s->lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (s->lock < 0)
        return -1;
    while (flock(s->lock, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EINTR)
            return -1;
        pfd.fd = s->conf->in;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 100) > 0) {
            if (s->inlen == s->own_max)
                return -1;
            n = read(s->conf->in, s->in + s->inlen, s->own_max - s->inlen);
            if (n <= 0)
                return -1;
            s->inlen += (size_t)n;
        }
    }
    return 0;
}

/** Say whether the comma-separated `list` of versions has `version`. */
static int offers(const char *list, unsigned long version)
{
    const char *p = list;
    char *end;
    unsigned long v;

    for (;;) {
        if (*p < '0' || *p > '9')
            return 0;
        v = strtoul(p, &end, 10);
        if (v == version)
            return 1;
        if (*end == '\0')
            return 0;
        if (*end != ',')
            return 0;
        p = end + 1;
    }
}

/** Remove the parts of puts that a responder left unfinished. */
static void remove_parts(struct server *s)
{
    char dir[PATH_MAX], part[PATH_MAX];
    struct dirent *e;
    DIR *d;

    pathf(dir, "%s/blob", s->dir);
    d = opendir(dir);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL)
        if (strstr(e->d_name, ".part") != NULL && pathf(part, "%s/%s", dir, e->d_name) == 0)
            unlink(part);
    closedir(d);
}

/**
 * Make the session's directories, take its lock and finish what earlier
 * responders of the session left; 0 on success.
 */
static int open_session(struct server *s)
{
    static const char *const sub[] = {"cmd", "att", "blob", "work"};
    char path[PATH_MAX];
    size_t i;

    pathf(s->dir, "%s/%s", s->conf->state, s->session);
    if (make_dir(s->dir) != 0) {
        say(s, "cannot make %s: %s", s->dir, strerror(errno));
        return -1;
    }
    for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++) {
        pathf(path, "%s/%s", s->dir, sub[i]);
        if (make_dir(path) != 0) {
            say(s, "cannot make %s: %s", path, strerror(errno));
            return -1;
        }
    }
    if (take_lock(s) != 0)
        return -1;
    recover(s);
    remove_parts(s);
    return 0;
}

/** Answer the controller's `hello CMD VERSIONS MAXFRAME`. */
static void do_hello(struct server *s, const struct wire_frame *f)
{
    char text[512], digest[65], max[24], pid[24], start[24];
    const char *cmd = f->field[0], *a[5];
    uint64_t offered;
    int ledger, found;

    memcpy(s->session, f->session, sizeof(s->session));
    if (!offers(f->field[1], ETUDE_FRAME_VERSION)) {
        say(s, "the controller offers versions %s, and this responder speaks %s", f->field[1],
            ETUDE_REMOTE_VERSIONS);
        emit_error(s, -1, cmd, ETUDE_ERR_VERSION);
        s->lost = 1;
        return;
    }
    if (decimal(f->field[2], &offered) != 0 || offered < ETUDE_FRAME_MIN) {
        emit_error(s, -1, cmd, ETUDE_ERR_LIMIT);
        s->lost = 1;
        return;
    }
    if (offered < s->max)
        s->max = (size_t)offered;
    if (open_session(s) != 0) {
        emit_error(s, -1, cmd, ETUDE_ERR_FAILED);
        s->lost = 1;
        return;
    }
    join(text, sizeof(text), f->type, (const char *const *)f->field, f->nfields);
    digest_hex(text, strlen(text), digest);
    found = ledger_find(s, cmd, digest);
    if (found == 2) {
        emit_error(s, -1, cmd, ETUDE_ERR_CONFLICT);
        s->lost = 1;
        return;
    }
    if (found == 0) {
        ledger = ledger_begin(s, cmd, digest, text);
        if (ledger >= 0)
            close(ledger);
    }
    s->ready = 1;
    snprintf(max, sizeof(max), "%lu", (unsigned long)s->max);
    snprintf(pid, sizeof(pid), "%ld", s->self.pid);
    if (s->self.start_known)
        snprintf(start, sizeof(start), "%llu", (unsigned long long)s->self.start);
    else
        strcpy(start, "-");
    a[0] = cmd;
    a[1] = "1";
    a[2] = max;
    a[3] = pid;
    a[4] = start;
    emit(s, -1, 0, ETUDE_EV_READY, a, 5);
}

/** Say whether a command's fields are of their forms. */
static int command_fields(const struct wire_frame *f)
{
    const char *t = f->type;
    uint64_t v;

    if (f->nfields < 1 || !is_cmd(f->field[0]))
        return 0;
    if (strcmp(t, ETUDE_CMD_PUT) == 0)
        return f->nfields == 5 && is_name(f->field[1]) && decimal(f->field[2], &v) == 0 &&
               is_hex(f->field[3], 64) && strlen(f->field[4]) == 4 &&
               strspn(f->field[4], "01234567") == 4;
    if (strcmp(t, ETUDE_CMD_RUN) == 0 || strcmp(t, ETUDE_CMD_EXEC) == 0)
        return f->nfields == 3 && is_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN) &&
               is_hex(f->field[2], 64);
    if (strcmp(t, ETUDE_CMD_CANCEL) == 0 || strcmp(t, ETUDE_CMD_STATUS) == 0 ||
        strcmp(t, ETUDE_CMD_REMOVE) == 0)
        return f->nfields == 2 && is_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN);
    if (strcmp(t, ETUDE_CMD_GET) == 0)
        return f->nfields == 2;
    if (strcmp(t, ETUDE_CMD_BYE) == 0)
        return f->nfields == 1;
    return 1;
}

/** End the session, as `bye CMD` asks: its attempts end and its files go. */
static void do_bye(struct server *s, const char *cmd)
{
    const char *a[1];

    end_attempts(s);
    remove_blobs(s);
    a[0] = cmd;
    emit(s, -1, 0, ETUDE_CMD_BYE, a, 1);
    s->done = 1;
}

/** Act on a command other than a put, with its record `ledger` or -1. */
static void act(struct server *s, int ledger, const struct wire_frame *f)
{
    const char *cmd = f->field[0], *t = f->type;

    if (strcmp(t, ETUDE_CMD_RUN) == 0) {
        do_run(s, ledger, cmd, f->field[1], f->field[2]);
        return;
    }
    if (strcmp(t, ETUDE_CMD_EXEC) == 0)
        emit_error(s, ledger, cmd, ETUDE_ERR_REFUSED);
    else if (strcmp(t, ETUDE_CMD_CANCEL) == 0)
        do_cancel(s, ledger, cmd, f->field[1]);
    else if (strcmp(t, ETUDE_CMD_STATUS) == 0)
        do_status(s, cmd, f->field[1]);
    else if (strcmp(t, ETUDE_CMD_GET) == 0)
        do_get(s, cmd, f->field[1]);
    else if (strcmp(t, ETUDE_CMD_REMOVE) == 0)
        do_remove(s, ledger, cmd, f->field[1]);
    else if (strcmp(t, ETUDE_CMD_BYE) == 0)
        do_bye(s, cmd);
    else
        emit_error(s, ledger, cmd, ETUDE_ERR_UNKNOWN);
    if (ledger >= 0)
        close(ledger);
}

/** Take the data of a put not acted on, and ignore them. */
static void drain_put(struct server *s, const char *t, const char *cmd)
{
    if (strcmp(t, ETUDE_CMD_PUT) != 0)
        return;
    abandon_put(s);
    strcpy(s->put.cmd, cmd);
    s->put.draining = 1;
}

/** Act on one command of the session, once. */
static void dispatch(struct server *s, struct wire_frame *f)
{
    char text[512], digest[65];
    const char *cmd, *t = f->type;
    int found, ledger, fresh;

    if (strcmp(t, ETUDE_CMD_DATA) == 0 || strcmp(t, ETUDE_CMD_SEAL) == 0) {
        if ((t[0] == 'd' ? do_data(s, f) : do_seal(s, f)) != 0) {
            say(s, "%s for no put in progress", t);
            s->lost = 1;
        }
        return;
    }
    if (strcmp(t, ETUDE_CMD_HELLO) == 0 || !command_fields(f) ||
        join(text, sizeof(text), t, (const char *const *)f->field, f->nfields) != 0) {
        say(s, "a %s command that is not of its form", t);
        s->lost = 1;
        return;
    }
    cmd = f->field[0];
    digest_hex(text, strlen(text), digest);
    found = ledger_find(s, cmd, digest);
    fresh = strcmp(t, ETUDE_CMD_STATUS) == 0 || strcmp(t, ETUDE_CMD_GET) == 0 ||
            strcmp(t, ETUDE_CMD_BYE) == 0;
    if (found == 2 || found < 0) {
        emit_error(s, -1, cmd, found == 2 ? ETUDE_ERR_CONFLICT : ETUDE_ERR_FAILED);
        drain_put(s, t, cmd);
        return;
    }
    if (found == 1 && !fresh) {
        ledger_events(s, cmd, replay_one, s);
        drain_put(s, t, cmd);
        return;
    }
    if (strcmp(t, ETUDE_CMD_PUT) == 0) {
        do_put(s, f, text, digest);
        return;
    }
    ledger = -1;
    if (found == 0) {
        ledger = ledger_begin(s, cmd, digest, text);
        if (ledger < 0) {
            emit_error(s, -1, cmd, ETUDE_ERR_FAILED);
            return;
        }
    }
    act(s, ledger, f);
}

/** Parse a line as a frame of at most `max` bytes; NULL, or why it is not. */
static const char *parse_line(char *line, size_t len, size_t max, struct wire_frame *f)
{
    if (len + 1 > max)
        return "a frame longer than the bound";
    if (len < 4 || memcmp(line, ETUDE_FRAME_MAGIC " ", 4) != 0)
        return "a line that is not a frame of this version";
    return wire_parse(line, len, f) == 0 ? NULL : "a damaged or malformed frame";
}

/** Say why a frame does not follow on this connection, or NULL. */
static const char *frame_fault(struct server *s, const struct wire_frame *f)
{
    if (strcmp(f->boot, s->conf->boot) != 0)
        return "a frame for another boot";
    if (s->ready ? strcmp(f->session, s->session) != 0
                 : strcmp(f->session, ETUDE_FRAME_NO_SESSION) == 0)
        return "a frame of another session";
    if (f->seq != (s->has_ack ? s->seq_in + 1 : 0))
        return "a frame out of order";
    if (f->has_ack && (f->ack >= s->seq_out || f->ack < s->ack_in))
        return "a frame that acknowledges what was not sent";
    if (!s->ready && strcmp(f->type, ETUDE_CMD_HELLO) != 0)
        return "a command before the hello";
    if (!s->ready && (f->nfields != 3 || !is_cmd(f->field[0])))
        return "a hello that is not of its form";
    return NULL;
}

/**
 * Act on one line of the connection. A line that is not a well-formed
 * frame of this session and boot, in order, ends the connection.
 */
static void take_line(struct server *s, char *line, size_t len)
{
    struct wire_frame f;
    char *copy;
    const char *why;

    copy = malloc(len + 1);
    if (copy == NULL) {
        s->lost = 1;
        return;
    }
    memcpy(copy, line, len);
    why = parse_line(line, len, s->max, &f);
    if (why == NULL)
        why = frame_fault(s, &f);
    if (why != NULL) {
        evidence(s, why, copy, len);
        say(s, "%s; the connection ends", why);
        free(copy);
        s->lost = 1;
        return;
    }
    free(copy);
    s->has_ack = 1;
    s->seq_in = f.seq;
    if (f.has_ack)
        s->ack_in = f.ack;
    if (strcmp(f.type, ETUDE_CMD_ACK) == 0)
        return;
    if (!s->ready)
        do_hello(s, &f);
    else
        dispatch(s, &f);
}

/** Act on every whole line that has arrived. */
static void take_input(struct server *s)
{
    size_t start = 0, i;

    for (i = 0; i < s->inlen && !s->lost && !s->done; i++) {
        if (s->in[i] != '\n')
            continue;
        take_line(s, s->in + start, i - start);
        start = i + 1;
    }
    if (s->lost || s->done)
        return;
    memmove(s->in, s->in + start, s->inlen - start);
    s->inlen -= start;
    if (s->inlen >= s->max) {
        evidence(s, "a line longer than a frame", s->in, s->inlen);
        say(s, "a line longer than a frame; the connection ends");
        s->lost = 1;
    }
}

/** Read what the connection has; at its end the connection is lost. */
static void read_input(struct server *s)
{
    ssize_t n = read(s->conf->in, s->in + s->inlen, s->own_max - s->inlen);

    if (n < 0 && errno == EINTR)
        return;
    if (n <= 0) {
        if (s->inlen > 0)
            evidence(s, "a line cut short by the end of the connection", s->in, s->inlen);
        s->lost = 1;
        return;
    }
    s->inlen += (size_t)n;
    take_input(s);
}

/** Serve the attempts: their ends, their output and their deadlines. */
static void serve_attempts(struct server *s)
{
    struct etude_proc *p;
    struct attempt *a, *next;
    uint64_t now;

    p = etude_exec_wait(s->ex, SLICE_MS);
    for (a = s->attempts; a != NULL; a = next) {
        next = a->next;
        if (a->proc == p)
            finish(s, a);
    }
    now = posix_now_us();
    for (a = s->attempts; a != NULL && !s->lost; a = a->next) {
        stream_output(s, a);
        if (!a->cancelled && now >= a->deadline_at) {
            say(s, "attempt %s: its deadline passed; it is cancelled", a->token);
            etude_exec_cancel(a->proc);
            a->cancelled = 1;
        }
    }
}

int etude_serve(const struct etude_serve_conf *conf)
{
    struct server srv, *s = &srv;
    struct pollfd pfd;
    const char *f[3];
    char pid[24], start[24];
    int rc;

    memset(s, 0, sizeof(*s));
    s->conf = conf;
    s->lock = -1;
    s->put.fd = -1;
    s->own_max = conf->max_frame != 0 ? conf->max_frame : ETUDE_FRAME_STDIO_MAX;
    s->max = s->own_max;
    s->file_max = conf->file_max != 0 ? conf->file_max : ETUDE_REMOTE_FILE_MAX;
    if (s->own_max < ETUDE_FRAME_MIN || strlen(conf->boot) != ETUDE_FRAME_TOKEN_LEN ||
        !is_hex(conf->boot, ETUDE_FRAME_TOKEN_LEN) || strlen(conf->state) > PATH_MAX - STATE_ROOM ||
        make_dir(conf->state) != 0) {
        say(s, "cannot serve in %s with boot %s", conf->state, conf->boot);
        return 2;
    }
    s->in = malloc(s->own_max + 1);
    s->ex = etude_exec_new();
    if (s->in == NULL || s->ex == NULL) {
        free(s->in);
        if (s->ex != NULL)
            etude_exec_free(s->ex);
        return 2;
    }
    if (etude_process_state((long)getpid(), &s->self) != 1) {
        s->self.pid = (long)getpid();
        s->self.start_known = 0;
    }
    snprintf(pid, sizeof(pid), "%ld", s->self.pid);
    if (s->self.start_known)
        snprintf(start, sizeof(start), "%llu", (unsigned long long)s->self.start);
    else
        strcpy(start, "-");
    f[0] = ETUDE_REMOTE_VERSIONS;
    f[1] = pid;
    f[2] = start;
    send_frame(s, ETUDE_EV_ALIVE, f, 3);
    while (!s->lost && !s->done) {
        pfd.fd = conf->in;
        pfd.events = POLLIN;
        rc = poll(&pfd, 1, s->attempts != NULL ? 0 : -1);
        if (rc < 0 && errno != EINTR)
            s->lost = 1;
        else if (rc > 0)
            read_input(s);
        if (s->attempts != NULL && !s->lost)
            serve_attempts(s);
    }
    end_attempts(s);
    abandon_put(s);
    etude_exec_free(s->ex);
    free(s->in);
    if (s->lock >= 0)
        close(s->lock);
    return s->done ? 0 : 1;
}

#if defined(__linux__) || defined(__APPLE__)
/** Read the text that names the machine's boot into `text`; 0 on success. */
static int boot_text(char *text, int cap)
{
#if defined(__linux__)
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    int rc;

    if (f == NULL)
        return -1;
    rc = fgets(text, cap, f) != NULL ? 0 : -1;
    fclose(f);
    return rc;
#else
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    struct timeval tv;
    size_t len = sizeof(tv);

    if (sysctl(mib, 2, &tv, &len, NULL, 0) != 0)
        return -1;
    snprintf(text, (size_t)cap, "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
    return 0;
#endif
}

int etude_boot_token(char token[ETUDE_FRAME_TOKEN_LEN + 1])
{
    char text[128];

    if (boot_text(text, (int)sizeof(text)) != 0)
        return -1;
    token_of(text, token);
    return 0;
}
#else
int etude_boot_token(char token[ETUDE_FRAME_TOKEN_LEN + 1])
{
    (void)token;
    return -1;
}
#endif
