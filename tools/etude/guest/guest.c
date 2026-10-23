/*
 * etude-guest: etude's helper inside Rach2. It takes the controller's
 * commands as frames on the serial console under QEMU, or on its standard
 * input and output through SSH, stores files, runs the attempts of guest
 * cases and reports their output and ends. tools/etude/Readme.md gives the
 * protocol; this file implements it.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "wire.h"
#include "fixture.h"
#include "history_json.h"
#include "sha256.h"
#include <etude/frame.h>
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
#include <sys/resource.h>
#if defined(__rach2__)
#include <sys/random.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#define GUEST_VERSIONS "1"
#define PATH_CAP       1024
#define LINE_CAP       512
#define STAGE_MAX      8
#define KIDS_MAX       16
#define ARGS_MAX       256

/* The bytes of a file a put may store, and of an invocation. */
#define FILE_MAX       ((uint64_t)1 << 30)
#define INVOCATION_MAX (1u << 20)

/* Unwritten or unacknowledged frames past which no more output is read from
 * the children or from a file being sent. */
#define QUEUE_FRAMES 32
#define QUEUE_BYTES  (1u << 20)

/* How long a finished attempt's pipes may stay open, and its processes stay,
 * when its grace period is shorter. */
#define DRAIN_US 2000000

/* How long the processes of an orphaned attempt may take to end once they
 * are killed, before its cleanup is recorded as unconfirmed. */
#define SETTLE_US 2000000

/* The methods of usage's measurements: the processor time and the largest
 * resident size that wait4 gives for the child and the descendants it
 * reaped, each a lower bound of the attempt's. */
#define METHOD_CPU  "wait-reaped-descendants-v1"
#define METHOD_PEAK "posix-maxrss-v1"

enum state { ST_PREPARING, ST_RUNNING, ST_ENDED, ST_CLEANED };
static const char *const state_name[] = {"preparing", "running", "ended", "cleaned"};

/* A frame waiting to be written, or over the serial console to be
 * acknowledged. */
struct pending {
    char *text;
    size_t len;
    size_t done;
    int sequenced; /* has a SEQ that the peer acknowledges */
    uint64_t seq;
    struct pending *next;
};

/* An attempt this process runs. */
struct attempt {
    char token[ETUDE_FRAME_TOKEN_LEN + 1];
    char cmd[24];       /* the run's CMD */
    char dir[PATH_CAP]; /* STATE/a/TOKEN */
    pid_t pid;          /* the child, the leader of its own session and group */
    unsigned long long start;
    int fd[3];                   /* the read ends of the child's 1 and 2, or -1 */
    unsigned long long off[3];   /* the bytes of each kept and sent so far */
    unsigned long long total[3]; /* the bytes of each read, kept or not */
    struct sha256 sent[3];       /* of the bytes of each sent */
    unsigned long long kept;     /* of both */
    unsigned long long keep_max;
    char line[256]; /* the child's standard output since its last newline */
    size_t line_len;
    int line_long; /* the current line is longer than a record */
    pid_t kid[KIDS_MAX];
    unsigned long long kid_start[KIDS_MAX];
    int nkids;
    int64_t deadline; /* monotonic microseconds */
    int64_t grace_us;
    int64_t kill_at; /* when SIGKILL follows a SIGTERM, or 0 */
    int stopped;     /* a cancellation, the deadline or a lost connection ended it */
    int exited;
    int status;
    struct rusage ru;
    int64_t drain_until; /* the end of the wait for the pipes and the processes */
    struct fixtures fx;
    struct attempt *next;
};

/* A file being received, of a put not yet sealed. */
struct receiving {
    int active;
    int present; /* the store has the file already, and data is not needed */
    char cmd[24];
    char name[160];
    char sha[65];
    mode_t mode;
    uint64_t size;
    uint64_t got;
    int fd;
    char part[PATH_CAP];
    struct sha256 digest;
};

/* A file being sent, for a get. */
struct sending {
    int active;
    char cmd[24];
    char name[160];
    int fd;
    uint64_t off;
    struct sha256 digest;
};

static struct {
    int stdio;
    const char *state;
    const char *stage[STAGE_MAX];
    int nstage;
    int64_t resend_us;
} opt;

static int in_fd = 0;
static int out_fd = 1;
static int log_fd = -1;
static struct termios saved_tty;
static int tty_saved;
static int sig_pipe[2] = {-1, -1};
static volatile sig_atomic_t got_end;

static char boot[ETUDE_FRAME_TOKEN_LEN + 1];
static char session[ETUDE_FRAME_TOKEN_LEN + 1];
static int in_session;
static int lock_fd = -1; /* the session's lock, over standard I/O */
static uint64_t next_seq;
static uint64_t recv_next;
static int need_ack;
static int sent_since_input;
static size_t max_frame;
static int64_t last_alive;
static int64_t last_send;

static struct pending *queue_head;
static struct pending *queue_tail;
static size_t queue_frames;
static size_t queue_bytes;
static int input_closed;
static int output_broken;
static int closing; /* a bye was answered */

static char in_buf[2 * ETUDE_FRAME_STDIO_MAX + 2];
static size_t in_len;
static int in_skip; /* discarding the rest of a line over the bound */

static struct attempt *attempts;

/* The program an exec runs in this process's place once its started frame is
 * acknowledged. */
static struct {
    int pending;
    int64_t until;
    char *argv[ARGS_MAX + 1];
    char *envp[ARGS_MAX + 1];
    char work[PATH_CAP];
    char prog[PATH_CAP];
} exec_next;

static struct receiving recv_file;
static struct sending send_file;
static pid_t self_pid;
static unsigned long long self_start;

/** Monotonic microseconds. */
static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/**
 * Append a line to STATE/guest.log, and over standard I/O to stderr as well.
 * Over the serial console nothing but frames may be written to the console,
 * so the log is the only place a diagnostic goes.
 */
static void note(const char *fmt, ...)
{
    char line[LINE_CAP + 64];
    va_list ap;
    int n;

    n = snprintf(line, sizeof(line), "etude-guest: ");
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    n = (int)strlen(line);
    line[n++] = '\n';
    if (log_fd >= 0 && write(log_fd, line, (size_t)n) < 0)
        log_fd = -1;
    if (opt.stdio && write(2, line, (size_t)n) < 0)
        return;
}

/** Write all `n` bytes at `p` to `fd`; 0 or -1. */
static int write_all(int fd, const void *p, size_t n)
{
    const char *b = p;

    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/** Format a path into `out`; 0, or -1 when it does not fit. */
static int path_of(char *out, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out, PATH_CAP, fmt, ap);
    va_end(ap);
    return n > 0 && n < PATH_CAP ? 0 : -1;
}

/**
 * Write `n` bytes as the whole of the file `path`: a temporary file is
 * written and synchronized, then renamed over `path`, so that a reader sees
 * the old content or the new and never a part.
 */
static int write_file(const char *path, const char *text, size_t n)
{
    char tmp[PATH_CAP];
    int fd;

    if (path_of(tmp, "%s.new", path) < 0)
        return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (write_all(fd, text, n) < 0 || fsync(fd) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/** Read at most `cap - 1` bytes of `path` into `buf` with a terminator; the
 * length, or -1. */
static long read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t n = 0;

    if (fd < 0)
        return -1;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    return (long)n;
}

#if defined(__APPLE__)
/** The start of process `pid` in microseconds since the epoch,
 * kp_proc.p_starttime of the sysctl KERN_PROC_PID, which exec leaves as it
 * is and a zombie keeps until it is reaped; 0 when the process is gone. */
static unsigned long long proc_start(pid_t pid)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, 0};
    struct kinfo_proc kp;
    size_t len = sizeof(kp);

    mib[3] = (int)pid;
    if (sysctl(mib, 4, &kp, &len, NULL, 0) < 0 || len < sizeof(kp))
        return 0;
    return (unsigned long long)kp.kp_proc.p_starttime.tv_sec * 1000000u +
           (unsigned long long)kp.kp_proc.p_starttime.tv_usec;
}
#else
/** The start of process `pid` in the kernel's ticks since boot, field 22 of
 * /proc/PID/stat; 0 when the process is gone. */
static unsigned long long proc_start(pid_t pid)
{
    char path[64];
    char buf[1024];
    char *p;
    int field;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (read_file(path, buf, sizeof(buf)) <= 0)
        return 0;
    p = strrchr(buf, ')');
    if (!p)
        return 0;
    /* The text after the command's ')' begins with field 3, the state. */
    for (field = 2; field < 22 && p; field++)
        p = strchr(p + 1, ' ');
    return p ? strtoull(p + 1, NULL, 10) : 0;
}
#endif

/** Whether `pid` still is the process that started at `start`. */
static int same_process(pid_t pid, unsigned long long start)
{
    return pid > 0 && proc_start(pid) == start && start != 0;
}

/** Hash `n` bytes to the first ETUDE_FRAME_TOKEN_LEN hexadecimal digits of
 * their SHA-256. */
static void token_of(const void *p, size_t n, char out[ETUDE_FRAME_TOKEN_LEN + 1])
{
    struct sha256 s;
    unsigned char d[32];
    char hex[65];

    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, d);
    sha256_hex(d, hex);
    memcpy(out, hex, ETUDE_FRAME_TOKEN_LEN);
    out[ETUDE_FRAME_TOKEN_LEN] = '\0';
}

/**
 * Find the boot's token. Linux has a boot identity in /proc, and macOS, where
 * the host tests run etude-guest too, has one in the sysctl
 * kern.bootsessionuuid. Rach2 has none, and the first etude-guest of a boot
 * writes random bytes to a file in /dev/shm, a tmpfs that every boot mounts
 * empty, for every later one to read. These bytes distinguish boots and
 * need no secure random source; GRND_INSECURE supplies them when Rach2
 * cannot read /dev/urandom. The file is published whole by
 * link(2), so a reader never sees a part, and it is used only when this user
 * owns it and nobody else may write it.
 */
static int find_boot(void)
{
    static const char path[] = "/dev/shm/etude-guest-boot";
    char buf[128];
    char tmp[64];
    unsigned char rnd[32];
    char hex[65];
    struct stat st;
    long n;
    int fd;

    n = read_file("/proc/sys/kernel/random/boot_id", buf, sizeof(buf));
#if defined(__APPLE__)
    if (n <= 0) {
        size_t len = sizeof(buf) - 1;
        if (sysctlbyname("kern.bootsessionuuid", buf, &len, NULL, 0) == 0) {
            buf[len] = '\0';
            n = (long)strlen(buf);
        }
    }
#endif
    if (n > 0) {
        token_of(buf, (size_t)n, boot);
        return 0;
    }
    if (stat(path, &st) < 0) {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        n = fd < 0 ? -1 : read(fd, rnd, sizeof(rnd));
        if (fd >= 0)
            close(fd);
#if defined(__rach2__)
        if (n != (long)sizeof(rnd))
            n = getrandom(rnd, sizeof(rnd), GRND_INSECURE);
#endif
        if (n != (long)sizeof(rnd))
            return -1;
        sha256_hex(rnd, hex);
        snprintf(tmp, sizeof(tmp), "%s.%ld", path, (long)getpid());
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            return -1;
        if (write_all(fd, hex, 64) < 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        close(fd);
        if (link(tmp, path) < 0 && errno != EEXIST) {
            unlink(tmp);
            return -1;
        }
        unlink(tmp);
    }
    if (stat(path, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 022) != 0 ||
        !S_ISREG(st.st_mode)) {
        note("boot: %s is not this user's own file", path);
        return -1;
    }
    n = read_file(path, buf, sizeof(buf));
    if (n != 64)
        return -1;
    token_of(buf, (size_t)n, boot);
    return 0;
}

/* ------------------------------------------------------------------ frames */

/** Put a frame on the queue. */
static void enqueue(char *text, size_t len, int sequenced, uint64_t seq)
{
    struct pending *p = calloc(1, sizeof(*p));

    if (!p) {
        free(text);
        output_broken = 1;
        return;
    }
    p->text = text;
    p->len = len;
    p->sequenced = sequenced;
    p->seq = seq;
    if (queue_tail)
        queue_tail->next = p;
    else
        queue_head = p;
    queue_tail = p;
    queue_frames++;
    queue_bytes += len;
}

/** Remove the frame at the head of the queue. */
static void dequeue(void)
{
    struct pending *p = queue_head;

    queue_head = p->next;
    if (!queue_head)
        queue_tail = NULL;
    queue_frames--;
    queue_bytes -= p->len;
    free(p->text);
    free(p);
}

/**
 * Write what the queue has not written. Over standard I/O a written frame is
 * done with; over the serial console it stays until acknowledged, and the
 * whole queue is written again when the oldest has waited past the resend
 * interval.
 */
static void flush(void)
{
    struct pending *p;

    if (output_broken)
        return;
    if (!opt.stdio && queue_head && queue_head->done == queue_head->len &&
        now_us() - last_send >= opt.resend_us)
        for (p = queue_head; p; p = p->next)
            p->done = 0;
    for (p = queue_head; p;) {
        ssize_t w;

        if (p->done == p->len) {
            p = p->next;
            continue;
        }
        w = write(out_fd, p->text + p->done, p->len - p->done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN)
            return;
        if (w <= 0) {
            output_broken = 1;
            return;
        }
        p->done += (size_t)w;
        if (p->done < p->len)
            return;
        last_send = now_us();
        sent_since_input = 1;
        if (opt.stdio || !p->sequenced) {
            struct pending *next = p->next;
            if (p == queue_head) {
                dequeue();
            } else {
                /* Only the head can be unsequenced over the serial console
                 * once a later frame waits, since an ack or alive frame goes
                 * out at once. */
                struct pending *q = queue_head;
                while (q->next != p)
                    q = q->next;
                q->next = next;
                if (queue_tail == p)
                    queue_tail = q;
                queue_frames--;
                queue_bytes -= p->len;
                free(p->text);
                free(p);
            }
            p = next;
        } else {
            p = p->next;
        }
    }
}

/** Whether the queue has room for more output of the children or of a file. */
static int queue_room(void)
{
    return queue_frames < QUEUE_FRAMES && queue_bytes < QUEUE_BYTES;
}

/**
 * Encode and queue a frame of `type` and `nfields` fields. An event takes the
 * next SEQ. An alive frame has no session: on the serial console its SEQ is 0
 * and takes nothing, and over standard I/O it is the connection's first
 * frame and takes SEQ 0, so that the events follow it from 1. An ack frame
 * carries the next SEQ without taking it.
 *
 * @return 0, or -1 when the frame does not fit the frame bound.
 */
static int send_frame(const char *type, int nfields, const char *const *field)
{
    int alive = strcmp(type, ETUDE_EV_ALIVE) == 0;
    int ack = strcmp(type, ETUDE_CMD_ACK) == 0;
    int counted = !ack && (!alive || opt.stdio);
    size_t cap = alive ? ETUDE_FRAME_SERIAL_MAX : max_frame;
    char *text = malloc(cap + 1);
    size_t len;

    if (!text)
        return -1;
    len = wire_encode(text, cap + 1, alive ? ETUDE_FRAME_NO_SESSION : session, boot,
                      alive && !opt.stdio ? 0 : next_seq, !alive && recv_next > 0, recv_next - 1,
                      type, nfields, field);
    if (len == 0) {
        free(text);
        return -1;
    }
    enqueue(text, len, !alive && !ack, next_seq);
    if (counted)
        next_seq++;
    if (ack)
        need_ack = 0;
    flush();
    return 0;
}

/**
 * Split `copy`, a line of words separated by single spaces, "TYPE FIELD...",
 * in place into its type, which stays at `copy`, and its fields.
 *
 * @return the number of fields, or -1 when there are more than a frame has.
 */
static int split_line(char *copy, const char **field)
{
    char *p = strchr(copy, ' ');
    int n = 0;

    if (p) {
        *p++ = '\0';
        while (p && n < WIRE_FIELDS_MAX) {
            field[n++] = p;
            p = strchr(p, ' ');
            if (p)
                *p++ = '\0';
        }
    }
    return p ? -1 : n;
}

/** Send a frame whose fields are given as one line of words separated by
 * single spaces: "TYPE FIELD...". */
static int send_line(const char *line)
{
    const char *field[WIRE_FIELDS_MAX] = {0};
    char *copy = strdup(line);
    int n;
    int rc;

    if (!copy)
        return -1;
    n = split_line(copy, field);
    rc = n < 0 ? -1 : send_frame(copy, n, field);
    free(copy);
    return rc;
}

/** Whether send_line could send the event `line` as the frame `ahead` events
 * after the next: whether it has few enough fields and fits the frame bound. */
static int event_fits(const char *line, uint64_t ahead)
{
    const char *field[WIRE_FIELDS_MAX] = {0};
    char *copy = strdup(line);
    char *text = malloc(max_frame + 1);
    int n = copy ? split_line(copy, field) : -1;
    int fits = n >= 0 && text &&
               wire_encode(text, max_frame + 1, session, boot, next_seq + ahead, recv_next > 0,
                           recv_next - 1, copy, n, field) > 0;

    free(text);
    free(copy);
    return fits;
}

/** The bytes of output one out frame has room for, a multiple of 3 so that
 * its base64 has no padding but at its end. */
static size_t out_chunk(void)
{
    /* The header with every number at its widest, the CRC and the newline. */
    size_t overhead = 4 + 17 + 17 + 19 + 19 + sizeof(ETUDE_EV_OUT) + 19 + 17 + 2 + 19 + 11;
    size_t chunk = max_frame > overhead ? (max_frame - overhead) / 4 * 3 : 0;

    return chunk > 4095 ? 4095 : chunk < 3 ? 3 : chunk;
}

/** Release the frames the peer has acknowledged, up to SEQ `ack`. */
static void acknowledged(uint64_t ack)
{
    while (queue_head && queue_head->sequenced && queue_head->seq <= ack &&
           queue_head->done == queue_head->len)
        dequeue();
}

/* ----------------------------------------------------------------- records */

/** The path of the record of command `cmd` in this session. */
static int cmd_path(char *out, const char *cmd)
{
    return path_of(out, "%s/s/%s/%s", opt.state, session, cmd);
}

/** Append `line` and a newline to the record of command `cmd`. */
static void cmd_append(const char *cmd, const char *line)
{
    char path[PATH_CAP];
    int fd;

    if (cmd_path(path, cmd) < 0)
        return;
    fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        return;
    if (write_all(fd, line, strlen(line)) < 0 || write_all(fd, "\n", 1) < 0 || fsync(fd) < 0)
        note("record: %s: %s", path, strerror(errno));
    close(fd);
}

#ifdef RACH2_HOSTTEST
/* In the host test, the type of event that etude-guest is killed at, once
 * the event is recorded and before it is sent. */
const char *etude_guest_fault;

/** Kill this process when the event `line` is of the type
 * `etude_guest_fault` gives, as a kill between two writes would. */
static void fault_point(const char *line)
{
    size_t n = etude_guest_fault ? strlen(etude_guest_fault) : 0;

    if (n > 0 && strncmp(line, etude_guest_fault, n) == 0 && line[n] == ' ')
        kill(getpid(), SIGKILL);
}
#else
#define fault_point(line) ((void)0)
#endif

/**
 * Record the event `line` of command `cmd` and then send it, so that a
 * repeat of the command is answered with every event that was sent. An
 * event that cannot be sent is recorded and sent as `error CMD limit` in its
 * place, so that the record keeps one form of it.
 */
static void cmd_event(const char *cmd, const char *line)
{
    char err[64];

    if (!event_fits(line, 0)) {
        snprintf(err, sizeof(err), "%s %s %s", ETUDE_EV_ERROR, cmd, ETUDE_ERR_LIMIT);
        line = err;
    }
    cmd_append(cmd, line);
    fault_point(line);
    send_line(line);
}

/** Send `error CMD CODE` for command `cmd`, kept in its record. */
static void cmd_error(const char *cmd, const char *code)
{
    char line[64];

    snprintf(line, sizeof(line), "%s %s %s", ETUDE_EV_ERROR, cmd, code);
    cmd_event(cmd, line);
}

static void replay_output(const char *token, const char *cmd);

/**
 * Begin command `cmd`, whose type and fields are `text`. A new command is
 * recorded before it acts. A repeat with the same text is answered by the
 * events its record keeps, and a repeat with other text by `error CMD
 * conflict`.
 *
 * @return 1 for a new command, 0 for one answered here.
 */
static int cmd_begin(const char *cmd, const char *text)
{
    char path[PATH_CAP];
    static char buf[64 * 1024];
    char *line;
    char *end;

    if (cmd_path(path, cmd) < 0) {
        send_line("error - limit");
        return 0;
    }
    if (read_file(path, buf, sizeof(buf)) < 0) {
        char line0[LINE_CAP + 2];
        snprintf(line0, sizeof(line0), "%s\n", text);
        if (write_file(path, line0, strlen(line0)) < 0) {
            note("record: %s: %s", path, strerror(errno));
            snprintf(line0, sizeof(line0), "%s %s %s", ETUDE_EV_ERROR, cmd, ETUDE_ERR_FAILED);
            send_line(line0);
            return 0;
        }
        return 1;
    }
    end = strchr(buf, '\n');
    if (!end || (size_t)(end - buf) != strlen(text) || memcmp(buf, text, strlen(text)) != 0) {
        char err[64];
        snprintf(err, sizeof(err), "%s %s %s", ETUDE_EV_ERROR, cmd, ETUDE_ERR_CONFLICT);
        send_line(err);
        return 0;
    }
    for (line = end + 1; *line; line = end + 1) {
        end = strchr(line, '\n');
        if (!end)
            break;
        *end = '\0';
        send_line(line);
        if (strncmp(line, ETUDE_EV_STARTED " ", 8) == 0) {
            char token[ETUDE_FRAME_TOKEN_LEN + 1];
            const char *p = strchr(line + 8, ' ');
            if (p && strlen(p + 1) > ETUDE_FRAME_TOKEN_LEN) {
                memcpy(token, p + 1, ETUDE_FRAME_TOKEN_LEN);
                token[ETUDE_FRAME_TOKEN_LEN] = '\0';
                replay_output(token, cmd);
            }
        }
    }
    return 0;
}

/**
 * Write the record of an attempt, STATE/a/TOKEN/record: its command, its
 * owner, its child, the boot they run in, its state and its final events. A
 * later etude-guest reads it to answer `status`.
 */
static void attempt_record(const struct attempt *a, enum state st, const char *finals)
{
    char path[PATH_CAP];
    char text[2048];

    if (path_of(path, "%s/record", a->dir) < 0)
        return;
    snprintf(text, sizeof(text),
             "cmd %s\nsession %s\nowner %ld %llu\nchild %ld %llu\nboot %s\nstate %s\n%s", a->cmd,
             session, (long)self_pid, self_start, (long)a->pid, a->start, boot, state_name[st],
             finals ? finals : "");
    if (write_file(path, text, strlen(text)) < 0)
        note("record: %s: %s", path, strerror(errno));
}

/* ------------------------------------------------------------------- files */

/** Whether `s` is a file name of the invocation's pattern. */
static int good_name(const char *s)
{
    size_t i;
    size_t n = strlen(s);

    if (n == 0 || n > 128 ||
        !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') ||
          (s[0] >= '0' && s[0] <= '9')))
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || strchr("._+-", s[i])))
            return 0;
    return 1;
}

/** Whether `s` is an absolute file path without empty, dot or parent
 * components. Its bytes must fit one protocol field. */
static int absolute_file(const char *s)
{
    const char *p;

    if (s[0] != '/' || s[1] == '\0' || strlen(s) >= 160)
        return 0;
    for (p = s; *p; p++)
        if (*p <= ' ' || *p > '~')
            return 0;
    for (p = s; p; p = strchr(p + 1, '/'))
        if (p[1] == '/' || p[1] == '\0' || strncmp(p + 1, "./", 2) == 0 ||
            strncmp(p + 1, "../", 3) == 0 || strcmp(p + 1, ".") == 0 || strcmp(p + 1, "..") == 0)
            return 0;
    return 1;
}

/**
 * Copy the file `from` to `to` with mode `mode`, and check on the way that it
 * has `size` bytes and the digest `sha`. A copy that does not match is
 * removed.
 *
 * @return 0, -1 for a file that cannot be read or written, -2 for one that
 * does not match.
 */
static int copy_checked(const char *from, const char *to, uint64_t size, const char *sha,
                        mode_t mode)
{
    char buf[8192];
    struct sha256 s;
    unsigned char d[32];
    char hex[65];
    uint64_t n = 0;
    int in;
    int out;
    int rc = 0;

    in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        return -1;
    }
    sha256_init(&s);
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            rc = -1;
            break;
        }
        if (r == 0)
            break;
        sha256_update(&s, (unsigned char *)buf, (size_t)r);
        n += (uint64_t)r;
        if (n > size || write_all(out, buf, (size_t)r) < 0) {
            rc = n > size ? -2 : -1;
            break;
        }
    }
    sha256_final(&s, d);
    sha256_hex(d, hex);
    if (rc == 0 && (n != size || strcmp(hex, sha) != 0))
        rc = -2;
    if (rc == 0 && (fchmod(out, mode) < 0 || fsync(out) < 0))
        rc = -1;
    close(in);
    close(out);
    if (rc != 0)
        unlink(to);
    return rc;
}

/** The path of the stored file of digest `sha`. */
static int store_path(char *out, const char *sha)
{
    return path_of(out, "%s/f/%s", opt.state, sha);
}

/** Copy a staged file of `name`, size and digest into the store; 0 when one
 * of the stage directories has it. */
static int from_stage(const char *name, uint64_t size, const char *sha)
{
    char from[PATH_CAP];
    char to[PATH_CAP];
    char part[PATH_CAP];
    struct stat st;
    int i;

    if (store_path(to, sha) < 0 || path_of(part, "%s.stage", to) < 0)
        return -1;
    for (i = 0; i < opt.nstage; i++) {
        if (path_of(from, "%s/%s", opt.stage[i], name) < 0 || stat(from, &st) < 0 ||
            !S_ISREG(st.st_mode) || (uint64_t)st.st_size != size)
            continue;
        unlink(part);
        if (copy_checked(from, part, size, sha, 0600) == 0) {
            if (rename(part, to) == 0)
                return 0;
            unlink(part);
        }
    }
    return -1;
}

/** Abandon the file being received. */
static void receive_end(void)
{
    if (!recv_file.active)
        return;
    if (recv_file.fd >= 0) {
        close(recv_file.fd);
        unlink(recv_file.part);
    }
    recv_file.active = 0;
}

/** put CMD NAME SIZE SHA256 MODE */
static void do_put(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    char path[PATH_CAP];
    uint64_t size;
    struct stat st;

    if (f->nfields != 5) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    if ((!good_name(f->field[1]) && !absolute_file(f->field[1])) ||
        !wire_number(f->field[2], &size) || !wire_hex(f->field[3], 64) ||
        strlen(f->field[4]) != 4 || strspn(f->field[4], "01234567") != 4) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (size > FILE_MAX) {
        cmd_error(cmd, ETUDE_ERR_LIMIT);
        return;
    }
    if (store_path(path, f->field[3]) < 0) {
        cmd_error(cmd, ETUDE_ERR_LIMIT);
        return;
    }
    receive_end();
    memset(&recv_file, 0, sizeof(recv_file));
    snprintf(recv_file.cmd, sizeof(recv_file.cmd), "%s", cmd);
    snprintf(recv_file.name, sizeof(recv_file.name), "%s", f->field[1]);
    snprintf(recv_file.sha, sizeof(recv_file.sha), "%s", f->field[3]);
    recv_file.size = size;
    recv_file.mode = (mode_t)strtoul(f->field[4], NULL, 8);
    recv_file.fd = -1;
    if ((stat(path, &st) == 0 && (uint64_t)st.st_size == size) ||
        from_stage(f->field[1], size, f->field[3]) == 0) {
        recv_file.present = 1;
        recv_file.active = 1;
        return;
    }
    if (path_of(recv_file.part, "%s/f/.part-%s", opt.state, cmd) < 0) {
        cmd_error(cmd, ETUDE_ERR_LIMIT);
        return;
    }
    unlink(recv_file.part);
    recv_file.fd = open(recv_file.part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (recv_file.fd < 0) {
        note("put: %s: %s", recv_file.part, strerror(errno));
        cmd_error(cmd, ETUDE_ERR_FAILED);
        return;
    }
    sha256_init(&recv_file.digest);
    recv_file.active = 1;
}

/** Whether command `cmd` has a record with events, which a data or seal of a
 * finished put is answered from. */
static void replay_record(const char *cmd)
{
    char path[PATH_CAP];
    static char buf[4096];
    char *line;
    char *end;

    if (cmd_path(path, cmd) < 0 || read_file(path, buf, sizeof(buf)) < 0) {
        char err[64];
        snprintf(err, sizeof(err), "%s %s %s", ETUDE_EV_ERROR, cmd, ETUDE_ERR_UNKNOWN);
        send_line(err);
        return;
    }
    end = strchr(buf, '\n');
    for (line = end ? end + 1 : buf + strlen(buf); *line; line = end + 1) {
        end = strchr(line, '\n');
        if (!end)
            break;
        *end = '\0';
        send_line(line);
    }
}

/** data CMD OFFSET BASE64, of the put CMD */
static void do_data(const struct wire_frame *f)
{
    const char *cmd = f->field[0];
    static unsigned char bytes[ETUDE_FRAME_STDIO_MAX];
    uint64_t off;
    long n;

    if (!recv_file.active || strcmp(recv_file.cmd, cmd) != 0) {
        char path[PATH_CAP];
        struct stat st;
        /* A data frame of a put already sealed is a repeat; it changes
         * nothing. */
        if (cmd_path(path, cmd) < 0 || stat(path, &st) < 0)
            cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (recv_file.present)
        return;
    if (f->nfields != 3 || !wire_number(f->field[1], &off) ||
        strlen(f->field[2]) > sizeof(bytes) / 3 * 4 ||
        (n = wire_unbase64(f->field[2], bytes)) < 0 || off > recv_file.got) {
        receive_end();
        cmd_error(cmd, ETUDE_ERR_DAMAGED);
        return;
    }
    if (off + (uint64_t)n <= recv_file.got)
        return;
    if (off != recv_file.got || recv_file.got + (uint64_t)n > recv_file.size) {
        receive_end();
        cmd_error(cmd, ETUDE_ERR_DAMAGED);
        return;
    }
    if (write_all(recv_file.fd, bytes, (size_t)n) < 0) {
        note("put: %s: %s", recv_file.part, strerror(errno));
        receive_end();
        cmd_error(cmd, ETUDE_ERR_FAILED);
        return;
    }
    sha256_update(&recv_file.digest, bytes, (size_t)n);
    recv_file.got += (uint64_t)n;
}

/** Publish an absolute-path put only after the stored object's size and
 * digest match. The temporary file is beside its destination, so rename
 * replaces the destination whole; failures remove the temporary file. */
static int publish_received(void)
{
    char from[PATH_CAP], part[PATH_CAP];
    int rc;

    if (recv_file.name[0] != '/')
        return 0;
    if (store_path(from, recv_file.sha) < 0 ||
        path_of(part, "%s.etude-%ld-%s", recv_file.name, (long)self_pid, recv_file.cmd) < 0)
        return -1;
    rc = copy_checked(from, part, recv_file.size, recv_file.sha, recv_file.mode);
    if (rc == 0)
        rc = rename(part, recv_file.name);
    if (rc != 0)
        unlink(part);
    return rc;
}

/** seal CMD, of the put CMD */
static void do_seal(const struct wire_frame *f)
{
    const char *cmd = f->field[0];
    unsigned char d[32];
    char hex[65];
    char path[PATH_CAP];
    char line[LINE_CAP];

    if (!recv_file.active || strcmp(recv_file.cmd, cmd) != 0) {
        replay_record(cmd);
        return;
    }
    if (recv_file.present) {
        recv_file.active = 0;
        if (publish_received() < 0) {
            cmd_error(cmd, ETUDE_ERR_FAILED);
            return;
        }
        snprintf(line, sizeof(line), "%s %s %s %s", ETUDE_EV_STORED, cmd, recv_file.name,
                 recv_file.sha);
        cmd_event(cmd, line);
        return;
    }
    sha256_final(&recv_file.digest, d);
    sha256_hex(d, hex);
    if (recv_file.got != recv_file.size || strcmp(hex, recv_file.sha) != 0) {
        receive_end();
        cmd_error(cmd, ETUDE_ERR_DAMAGED);
        return;
    }
    if (fsync(recv_file.fd) < 0 || store_path(path, recv_file.sha) < 0 ||
        rename(recv_file.part, path) < 0) {
        note("put: %s: %s", recv_file.part, strerror(errno));
        receive_end();
        cmd_error(cmd, ETUDE_ERR_FAILED);
        return;
    }
    close(recv_file.fd);
    recv_file.active = 0;
    if (publish_received() < 0) {
        cmd_error(cmd, ETUDE_ERR_FAILED);
        return;
    }
    snprintf(line, sizeof(line), "%s %s %s %s", ETUDE_EV_STORED, cmd, recv_file.name,
             recv_file.sha);
    cmd_event(cmd, line);
}

/* ------------------------------------------------------------- invocations */

/* An invocation, as run and exec read it. */
struct invocation {
    struct eh_json *root;
    const char *token;
    const char *program;
    const char *role;
    const char *stdin_name;
    int64_t deadline_us;
    int64_t grace_us;
    int64_t output_max;
    struct fixtures fx;
};

/** The integer member `key` of `o`, or -1. */
static int64_t int_member(const struct eh_json *o, const char *key)
{
    const struct eh_json *v = eh_json_get(o, key);

    return v && v->type == 'n' ? v->number : -1;
}

/** The string member `key` of `o`, or NULL. */
static const char *str_member(const struct eh_json *o, const char *key)
{
    const struct eh_json *v = eh_json_get(o, key);

    return v && v->type == 's' ? v->string : NULL;
}

/** Whether `name` is one of the invocation's files. */
static const struct eh_json *find_file(const struct invocation *inv, const char *name)
{
    const struct eh_json *f;

    for (f = eh_json_get(inv->root, "files")->child; f; f = f->next)
        if (strcmp(str_member(f, "name"), name) == 0)
            return f;
    return NULL;
}

/** Whether the invocation `ctx` has a file called `name`, for the fixtures. */
static int has_file(const void *ctx, const char *name)
{
    return find_file(ctx, name) != NULL;
}

/**
 * Read and check the invocation of digest `sha` for the attempt `token`.
 *
 * @return 0, or the error code to answer with: `unknown` when it is not
 * stored, `refused` when it is not a valid invocation of this attempt or
 * asks for what this etude-guest does not provide.
 */
static const char *load_invocation(struct invocation *inv, const char *sha, const char *token)
{
    static const char *const fields[] = {"run_id",  "attempt_id", "attempt_token",
                                         "case_id", "program",    "role"};
    char path[PATH_CAP];
    char *text;
    const struct eh_json *v;
    const struct eh_json *f;
    const char *why;
    long n;
    size_t i;

    memset(inv, 0, sizeof(*inv));
    text = malloc(INVOCATION_MAX + 1);
    if (!text || store_path(path, sha) < 0 || (n = read_file(path, text, INVOCATION_MAX + 1)) < 0) {
        free(text);
        return ETUDE_ERR_UNKNOWN;
    }
    inv->root = n < (long)INVOCATION_MAX ? eh_json_parse(text) : NULL;
    free(text);
    if (!inv->root || inv->root->type != 'o' || int_member(inv->root, "schema_version") != 1) {
        note("invocation %s: not an invocation of version 1", sha);
        return ETUDE_ERR_REFUSED;
    }
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (!str_member(inv->root, fields[i])) {
            note("invocation %s: no %s", sha, fields[i]);
            return ETUDE_ERR_REFUSED;
        }
    inv->token = str_member(inv->root, "attempt_token");
    inv->program = str_member(inv->root, "program");
    inv->role = str_member(inv->root, "role");
    inv->deadline_us = int_member(inv->root, "deadline_us");
    inv->grace_us = int_member(inv->root, "grace_us");
    inv->output_max = int_member(inv->root, "output_max_bytes");
    v = eh_json_get(inv->root, "stdin");
    inv->stdin_name = v && v->type == 's' ? v->string : NULL;
    if (strcmp(inv->token, token) != 0 ||
        (!good_name(inv->program) && !absolute_file(inv->program)) || inv->deadline_us < 1 ||
        inv->grace_us < 1 || inv->output_max < 1 || !v || (v->type != 'z' && v->type != 's') ||
        (strcmp(inv->role, "ordinary") != 0 && strcmp(inv->role, "init") != 0)) {
        note("invocation %s: a field has no valid value for attempt %s", sha, token);
        return ETUDE_ERR_REFUSED;
    }
    v = eh_json_get(inv->root, "files");
    if (!v || v->type != 'a')
        return ETUDE_ERR_REFUSED;
    for (f = v->child; f; f = f->next) {
        const char *mode = str_member(f, "mode");
        if (f->type != 'o' || !str_member(f, "name") || !good_name(str_member(f, "name")) ||
            !str_member(f, "sha256") || !wire_hex(str_member(f, "sha256"), 64) ||
            int_member(f, "size_bytes") < 0 || !mode || strlen(mode) != 4 ||
            strspn(mode, "01234567") != 4) {
            note("invocation %s: a file is not valid", sha);
            return ETUDE_ERR_REFUSED;
        }
    }
    if ((inv->program[0] != '/' && !find_file(inv, inv->program)) ||
        (inv->stdin_name && !absolute_file(inv->stdin_name) && !find_file(inv, inv->stdin_name))) {
        note("invocation %s: the program or the input is not one of its files", sha);
        return ETUDE_ERR_REFUSED;
    }
    v = eh_json_get(inv->root, "arguments");
    if (!v || v->type != 'a')
        return ETUDE_ERR_REFUSED;
    for (f = v->child, n = 0; f; f = f->next, n++)
        if (f->type != 's' || n >= ARGS_MAX - 1)
            return ETUDE_ERR_REFUSED;
    v = eh_json_get(inv->root, "environment");
    if (!v || v->type != 'o')
        return ETUDE_ERR_REFUSED;
    for (f = v->child, n = 0; f; f = f->next, n++)
        if (f->type != 's' || n >= ARGS_MAX - 1)
            return ETUDE_ERR_REFUSED;
    v = eh_json_get(inv->root, "fixtures");
    if (!v || v->type != 'a')
        return ETUDE_ERR_REFUSED;
    if (fixture_parse(&inv->fx, v, has_file, inv, &why)) {
        note("invocation %s: fixture %s is not valid", sha, why);
        return ETUDE_ERR_REFUSED;
    }
    return NULL;
}

/** Make the attempt's directory and workspace and copy its files into the
 * workspace; NULL, or the error code. */
static const char *prepare(const struct invocation *inv, const char *dir)
{
    char from[PATH_CAP];
    char to[PATH_CAP];
    const struct eh_json *f;
    int rc;

    if (mkdir(dir, 0711) < 0) {
        note("prepare: %s: %s", dir, strerror(errno));
        return errno == EEXIST ? ETUDE_ERR_CONFLICT : ETUDE_ERR_FAILED;
    }
    if (chmod(dir, 0711) < 0 || path_of(to, "%s/work", dir) < 0 || mkdir(to, 0755) < 0 ||
        chmod(to, 0755) < 0)
        return ETUDE_ERR_FAILED;
    for (f = eh_json_get(inv->root, "files")->child; f; f = f->next) {
        const char *sha = str_member(f, "sha256");
        if (store_path(from, sha) < 0 || path_of(to, "%s/work/%s", dir, str_member(f, "name")) < 0)
            return ETUDE_ERR_LIMIT;
        rc = copy_checked(from, to, (uint64_t)int_member(f, "size_bytes"), sha,
                          (mode_t)strtoul(str_member(f, "mode"), NULL, 8));
        if (rc != 0) {
            note("prepare: %s: %s", to,
                 rc == -2 ? "the stored file does not match" : strerror(errno));
            return rc == -2 ? ETUDE_ERR_DAMAGED : ETUDE_ERR_UNKNOWN;
        }
    }
    return NULL;
}

/** Build the argument and environment vectors of an invocation in `argv`
 * and `envp`, each of ARGS_MAX entries, with `self` as argv[0];
 * `${WORKSPACE}` in a value of the environment becomes `work`. The strings
 * are allocated. */
static int build_vectors(const struct invocation *inv, const char *work, const char *self,
                         char **argv, char **envp)
{
    const struct eh_json *v;
    int n = 0;

    argv[n++] = strdup(self);
    for (v = eh_json_get(inv->root, "arguments")->child; v; v = v->next)
        argv[n++] = strdup(v->string);
    argv[n] = NULL;
    n = 0;
    for (v = eh_json_get(inv->root, "environment")->child; v; v = v->next) {
        const char *s = v->string;
        size_t len = strlen(v->key) + 2 + strlen(s);
        const char *p;
        char *e;
        for (p = strstr(s, "${WORKSPACE}"); p; p = strstr(p + 12, "${WORKSPACE}"))
            len += strlen(work);
        e = malloc(len);
        if (!e)
            return -1;
        snprintf(e, len, "%s=", v->key);
        for (p = s; *p;) {
            if (strncmp(p, "${WORKSPACE}", 12) == 0) {
                strcat(e, work);
                p += 12;
            } else {
                size_t l = strlen(e);
                e[l] = *p++;
                e[l + 1] = '\0';
            }
        }
        envp[n++] = e;
    }
    envp[n] = NULL;
    return 0;
}

/** Put every signal's action back to the default and empty the mask, as a
 * case's program starts. */
static void default_signals(void)
{
    sigset_t none;
    int s;

    for (s = 1; s < NSIG; s++)
        signal(s, SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

/** Close every descriptor from 3 up but `keep`. */
static void close_from_3(int keep)
{
    long max = sysconf(_SC_OPEN_MAX);
    int fd;

    if (max < 0 || max > 4096)
        max = 4096;
    for (fd = 3; fd < max; fd++)
        if (fd != keep)
            close(fd);
}

/* ---------------------------------------------------------------- attempts */

/** The attempt of `token` this process runs, or NULL. */
static struct attempt *find_attempt(const char *token)
{
    struct attempt *a;

    for (a = attempts; a; a = a->next)
        if (strcmp(a->token, token) == 0)
            return a;
    return NULL;
}

/** Send what an attempt's output files hold, for a repeated run. */
static void replay_output(const char *token, const char *cmd)
{
    char path[PATH_CAP];
    static char line[8192];
    unsigned char buf[4095];
    size_t chunk = out_chunk();
    int fdn;

    for (fdn = 1; fdn <= 2; fdn++) {
        unsigned long long off = 0;
        int fd;
        if (path_of(path, "%s/a/%s/out.%d", opt.state, token, fdn) < 0)
            return;
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        for (;;) {
            ssize_t r = read(fd, buf, chunk);
            int k;
            if (r <= 0)
                break;
            k = snprintf(line, sizeof(line), "%s %s %s %d %llu ", ETUDE_EV_OUT, cmd, token, fdn,
                         off);
            wire_base64(buf, (size_t)r, line + k);
            send_line(line);
            off += (unsigned long long)r;
        }
        close(fd);
    }
}

/** Signal every process of an attempt: its group, and each child it
 * reported that is still the process it reported. */
static void signal_attempt(struct attempt *a, int sig)
{
    int i;

    if (a->pid > 0)
        kill(-a->pid, sig);
    for (i = 0; i < a->nkids; i++)
        if (same_process(a->kid[i], a->kid_start[i]))
            kill(a->kid[i], sig);
}

/** Whether no process of an attempt is left: its group is empty and every
 * child it reported is gone, or its PID now names another process. */
static int attempt_gone(const struct attempt *a)
{
    int i;

    if (a->pid > 0 && (kill(-a->pid, 0) == 0 || errno != ESRCH))
        return 0;
    for (i = 0; i < a->nkids; i++)
        if (same_process(a->kid[i], a->kid_start[i]))
            return 0;
    return 1;
}

/** Ask an attempt to stop: SIGTERM now, SIGKILL after its grace period. */
static void stop_attempt(struct attempt *a)
{
    if (a->exited || a->stopped)
        return;
    a->stopped = 1;
    signal_attempt(a, SIGTERM);
    a->kill_at = now_us() + a->grace_us;
}

/** Read a child report record from the case's standard output: a `child PID`
 * record names a process the executor ends with the case. */
static void scan_output(struct attempt *a, const char *p, size_t n)
{
    static const char rec[] = "@etude 1 child ";
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != '\n') {
            if (a->line_len < sizeof(a->line) - 1)
                a->line[a->line_len++] = p[i];
            else
                a->line_long = 1;
            continue;
        }
        a->line[a->line_len] = '\0';
        if (!a->line_long && strncmp(a->line, rec, sizeof(rec) - 1) == 0 && a->nkids < KIDS_MAX) {
            long pid = strtol(a->line + sizeof(rec) - 1, NULL, 10);
            unsigned long long st = pid > 0 ? proc_start((pid_t)pid) : 0;
            if (st) {
                a->kid[a->nkids] = (pid_t)pid;
                a->kid_start[a->nkids++] = st;
            }
        }
        a->line_len = 0;
        a->line_long = 0;
    }
}

/** Read what the child wrote on descriptor `fdn` (1 or 2), keep it, and
 * send it in out frames. */
static void read_output(struct attempt *a, int fdn)
{
    unsigned char buf[4096];
    char path[PATH_CAP];
    size_t chunk;
    ssize_t r;
    size_t i;
    int fd;

    r = read(a->fd[fdn], buf, sizeof(buf));
    if (r < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (r <= 0) {
        close(a->fd[fdn]);
        a->fd[fdn] = -1;
        return;
    }
    a->total[fdn] += (unsigned long long)r;
    if (fdn == 1)
        scan_output(a, (char *)buf, (size_t)r);
    if (a->kept >= a->keep_max)
        return;
    if ((unsigned long long)r > a->keep_max - a->kept)
        r = (ssize_t)(a->keep_max - a->kept);
    if (path_of(path, "%s/out.%d", a->dir, fdn) == 0 &&
        (fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)) >= 0) {
        if (write_all(fd, buf, (size_t)r) < 0)
            note("output: %s: %s", path, strerror(errno));
        close(fd);
    }
    chunk = out_chunk();
    for (i = 0; i < (size_t)r; i += chunk) {
        size_t m = (size_t)r - i < chunk ? (size_t)r - i : chunk;
        static char line[8192];
        int k = snprintf(line, sizeof(line), "%s %s %s %d %llu ", ETUDE_EV_OUT, a->cmd, a->token,
                         fdn, a->off[fdn] + i);
        wire_base64(buf + i, m, line + k);
        send_line(line);
    }
    sha256_update(&a->sent[fdn], buf, (size_t)r);
    a->off[fdn] += (unsigned long long)r;
    a->kept += (unsigned long long)r;
}

/** Free an attempt's vectors. */
static void free_vectors(char **argv, char **envp)
{
    int i;

    for (i = 0; argv[i]; i++)
        free(argv[i]);
    for (i = 0; envp[i]; i++)
        free(envp[i]);
}

/** The pipes between etude-guest and an attempt's child. */
struct pipes {
    int out[2];  /* the child's standard output */
    int err[2];  /* the child's standard error */
    int sync[2]; /* one byte once the child is in the attempt's record */
    int exe[2];  /* closed on exec, or the exec's errno */
};

/* What the child of an attempt executes, and where. */
struct launch {
    const char *input; /* the file of its standard input, or NULL */
    const char *work;  /* its workspace */
    const char *prog;  /* the program's path in the workspace */
    char **argv, **envp;
};

/** Close both ends of every pipe of `pp` that is open. */
static void close_pipes(struct pipes *pp)
{
    int *pair[4];
    int i;

    pair[0] = pp->out;
    pair[1] = pp->err;
    pair[2] = pp->sync;
    pair[3] = pp->exe;
    for (i = 0; i < 4; i++) {
        if (pair[i][0] >= 0)
            close(pair[i][0]);
        if (pair[i][1] >= 0)
            close(pair[i][1]);
        pair[i][0] = pair[i][1] = -1;
    }
}

/**
 * The child of an attempt, after `fork`: lead a session and a group of its
 * own, wait for etude-guest's byte on the sync pipe, take the launch's
 * input (or /dev/null) and the output pipes as descriptors 0 to 2, make the
 * fixtures of its own process, and execute the program in its workspace, or
 * in its private root. A fixture or an exec that fails writes its errno on
 * the exec pipe. Never returns.
 */
static void run_child(const struct pipes *pp, const struct fixtures *fx, const struct launch *l)
{
    char path[PATH_CAP];
    const char *dir = l->work;
    char byte;
    int code = 0;
    int in;

    setsid();
    close(pp->out[0]);
    close(pp->err[0]);
    close(pp->sync[1]);
    close(pp->exe[0]);
    if (read(pp->sync[0], &byte, 1) != 1)
        _exit(127);
    in = open(l->input ? l->input : "/dev/null", O_RDONLY);
    if (in < 0 || dup2(in, 0) < 0 || dup2(pp->out[1], 1) < 0 || dup2(pp->err[1], 2) < 0)
        code = errno;
    close_from_3(pp->exe[1]);
    snprintf(path, sizeof(path), "%s", l->prog);
    if (code == 0)
        code = fixture_enter(fx, l->work, path, sizeof(path), &dir);
    if (code == 0 && chdir(dir) < 0)
        code = errno;
    if (code == 0) {
        umask(022);
        default_signals();
        execve(path, l->argv, l->envp);
        code = errno;
    }
    if (write(pp->exe[1], &code, sizeof(code)) < 0)
        _exit(127);
    _exit(127);
}

/**
 * Read a case's output from the master side of its pseudo-terminal, as
 * standard output, in place of the pipe its slave side replaced, and write
 * the file `input`, when there is one, to the master side as the case's
 * input.
 */
static void take_terminal(struct attempt *a, const char *input)
{
    char buf[4096];
    ssize_t r;
    int fd;

    close(a->fd[1]);
    a->fd[1] = a->fx.pty_master;
    a->fx.pty_master = -1;
    fd = input ? open(input, O_RDONLY | O_CLOEXEC) : -1;
    while (fd >= 0 && (r = read(fd, buf, sizeof(buf))) > 0)
        if (write_all(a->fd[1], buf, (size_t)r) < 0) {
            note("spawn: the terminal's input: %s", strerror(errno));
            break;
        }
    if (fd >= 0)
        close(fd);
    fcntl(a->fd[1], F_SETFL, O_NONBLOCK);
}

/**
 * Start the child of attempt `a` from `inv`. The child leads a session and
 * a group of its own and waits until its identity is in the attempt's
 * record, so that no process of the attempt runs unregistered; then it
 * takes its descriptors, workspace, umask and signal state and executes the
 * program.
 *
 * @return NULL once the program runs, or the error code.
 */
static const char *spawn(struct attempt *a, const struct invocation *inv)
{
    char *argv[ARGS_MAX + 1];
    char *envp[ARGS_MAX + 1];
    char dir[PATH_MAX];
    char work[PATH_CAP];
    char prog[PATH_CAP];
    char input[PATH_CAP];
    struct pipes pp = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
    int code = 0;
    int n;
    ssize_t r;

    if (!realpath(a->dir, dir) || path_of(work, "%s/work", dir) < 0 ||
        path_of(prog, inv->program[0] == '/' ? "%s%s" : "%s/%s", inv->program[0] == '/' ? "" : work,
                inv->program) < 0 ||
        path_of(input, inv->stdin_name && inv->stdin_name[0] == '/' ? "%s%s" : "%s/%s",
                inv->stdin_name && inv->stdin_name[0] == '/' ? "" : work,
                inv->stdin_name ? inv->stdin_name : "") < 0)
        return ETUDE_ERR_LIMIT;
    argv[0] = NULL;
    envp[0] = NULL;
    if (build_vectors(inv, work, a->fx.has_root ? a->fx.root_path : prog, argv, envp) < 0)
        return ETUDE_ERR_FAILED;
    for (n = 0; argv[n]; n++) {
    }
    if (fixture_args(&a->fx, work, argv, n, ARGS_MAX) < 0) {
        free_vectors(argv, envp);
        return ETUDE_ERR_LIMIT;
    }
    if (pipe(pp.out) < 0 || pipe(pp.err) < 0 || pipe(pp.sync) < 0 || pipe(pp.exe) < 0 ||
        fcntl(pp.exe[1], F_SETFD, FD_CLOEXEC) < 0 || (a->pid = fork()) < 0) {
        note("spawn: %s", strerror(errno));
        free_vectors(argv, envp);
        close_pipes(&pp);
        a->pid = 0;
        return ETUDE_ERR_FAILED;
    }
    if (a->pid == 0) {
        struct launch l;
        l.input = inv->stdin_name ? input : NULL;
        l.work = work;
        l.prog = prog;
        l.argv = argv;
        l.envp = envp;
        run_child(&pp, &a->fx, &l);
    }
    close(pp.out[1]);
    close(pp.err[1]);
    close(pp.sync[0]);
    close(pp.exe[1]);
    a->start = proc_start(a->pid);
    attempt_record(a, ST_RUNNING, NULL);
    if (write(pp.sync[1], "x", 1) != 1)
        note("spawn: the child did not take its start");
    close(pp.sync[1]);
    do
        r = read(pp.exe[0], &code, sizeof(code));
    while (r < 0 && errno == EINTR);
    close(pp.exe[0]);
    free_vectors(argv, envp);
    a->fd[1] = pp.out[0];
    a->fd[2] = pp.err[0];
    fcntl(pp.out[0], F_SETFL, O_NONBLOCK);
    fcntl(pp.err[0], F_SETFL, O_NONBLOCK);
    fcntl(pp.out[0], F_SETFD, FD_CLOEXEC);
    fcntl(pp.err[0], F_SETFD, FD_CLOEXEC);
    if (r == (ssize_t)sizeof(code)) {
        note("spawn: %s: %s", prog, strerror(code));
        return ETUDE_ERR_FAILED;
    }
    if (a->fx.pty)
        take_terminal(a, inv->stdin_name ? input : NULL);
    return NULL;
}

/** Remove an attempt from the list and free it. */
static void drop_attempt(struct attempt *a)
{
    struct attempt **p;

    for (p = &attempts; *p; p = &(*p)->next)
        if (*p == a) {
            *p = a->next;
            break;
        }
    if (a->fd[1] >= 0)
        close(a->fd[1]);
    if (a->fd[2] >= 0)
        close(a->fd[2]);
    free(a);
}

/** Write in `out` the `stream` event of stream `fdn` (1 or 2): the bytes
 * sent, the bytes read and the SHA-256 of those sent. */
static void stream_event(char *out, size_t cap, struct attempt *a, int fdn)
{
    unsigned char d[32];
    char hex[65];

    sha256_final(&a->sent[fdn], d);
    sha256_hex(d, hex);
    snprintf(out, cap, "%s %s %s %d %llu %llu %s", ETUDE_EV_STREAM, a->cmd, a->token, fdn,
             a->off[fdn], a->total[fdn], hex);
}

/**
 * Record and send an attempt's final events once its child has ended and
 * its pipes are closed, or its time to close them has passed: how it ended,
 * the sizes and digest of each stream of its output, the processor time and
 * peak memory of the child and the descendants it reaped, and whether every
 * process of the attempt is gone and its fixtures removed. An event that
 * cannot be sent is `error CMD limit` in the attempt's record as in the
 * command's.
 */
static void finish(struct attempt *a)
{
    char line[5][LINE_CAP];
    char finals[5 * LINE_CAP + 6];
    char end[24];
    char peak[64];
    long long cpu;
    long long rss = (long long)a->ru.ru_maxrss;
    int clean = attempt_gone(a);
    int i;

    if (fixture_cleanup(&a->fx, a->dir) < 0) {
        note("attempt %s: a fixture was left", a->token);
        clean = 0;
    }
    if (a->stopped)
        snprintf(end, sizeof(end), "stopped -");
    else if (WIFSIGNALED(a->status))
        snprintf(end, sizeof(end), "signaled %d", WTERMSIG(a->status));
    else
        snprintf(end, sizeof(end), "exited %d", WEXITSTATUS(a->status));
    snprintf(line[0], LINE_CAP, "%s %s %s %s", ETUDE_EV_EXIT, a->cmd, a->token, end);
    stream_event(line[1], LINE_CAP, a, 1);
    stream_event(line[2], LINE_CAP, a, 2);
    cpu = (long long)a->ru.ru_utime.tv_sec * 1000000 + a->ru.ru_utime.tv_usec +
          (long long)a->ru.ru_stime.tv_sec * 1000000 + a->ru.ru_stime.tv_usec;
#if !defined(__APPLE__)
    rss *= 1024; /* ru_maxrss is in kilobytes, and in bytes on macOS */
#endif
    if (rss > 0)
        snprintf(peak, sizeof(peak), "%lld+ %s", rss, METHOD_PEAK);
    else
        snprintf(peak, sizeof(peak), "- -");
    snprintf(line[3], LINE_CAP, "%s %s %s %lld+ %s %s", ETUDE_EV_USAGE, a->cmd, a->token, cpu,
             METHOD_CPU, peak);
    snprintf(line[4], LINE_CAP, "%s %s %s %s", ETUDE_EV_CLEANED, a->cmd, a->token,
             clean ? "confirmed" : "unconfirmed");
    for (i = 0; i < 5; i++)
        if (!event_fits(line[i], (uint64_t)i))
            snprintf(line[i], LINE_CAP, "%s %s %s", ETUDE_EV_ERROR, a->cmd, ETUDE_ERR_LIMIT);
    snprintf(finals, sizeof(finals), "%s\n%s\n%s\n%s\n%s\n", line[0], line[1], line[2], line[3],
             line[4]);
    attempt_record(a, clean ? ST_CLEANED : ST_ENDED, finals);
    for (i = 0; i < 5; i++)
        cmd_event(a->cmd, line[i]);
    drop_attempt(a);
}

/** Take the end of every child that has ended: an attempt's, or an orphan's
 * when this process is the init they were given to. */
static void reap(void)
{
    struct rusage ru;
    struct attempt *a;
    int status;
    pid_t pid;

    while ((pid = wait4(-1, &status, WNOHANG, &ru)) > 0)
        for (a = attempts; a; a = a->next)
            if (a->pid == pid && !a->exited) {
                a->exited = 1;
                a->status = status;
                a->ru = ru;
                /* What the program left in its group, and the children it
                 * reported, end with it. */
                signal_attempt(a, SIGKILL);
                a->drain_until = now_us() + (a->grace_us > DRAIN_US ? a->grace_us : DRAIN_US);
            }
}

/** Move each attempt on by the clock: its deadline, the SIGKILL after a
 * SIGTERM, and its end. */
static void tick(void)
{
    int64_t t = now_us();
    struct attempt *a;
    struct attempt *next;

    for (a = attempts; a; a = next) {
        next = a->next;
        if (!a->exited && !a->stopped && t >= a->deadline) {
            note("attempt %s: past its deadline", a->token);
            stop_attempt(a);
        }
        if (!a->exited && a->kill_at && t >= a->kill_at) {
            signal_attempt(a, SIGKILL);
            a->kill_at = 0;
        }
        if (a->exited && ((a->fd[1] < 0 && a->fd[2] < 0 && attempt_gone(a)) || t >= a->drain_until))
            finish(a);
    }
}

/** run CMD ATTEMPT INVOCATION-SHA256 */
static void do_run(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    struct invocation inv;
    struct attempt *a;
    const char *code;
    const char *why;
    char line[LINE_CAP];
    int made = 0;

    if (f->nfields != 3) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    if (!wire_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN) || !wire_hex(f->field[2], 64)) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    code = load_invocation(&inv, f->field[2], f->field[1]);
    if (!code && strcmp(inv.role, "ordinary") != 0) {
        note("run %s: an invocation of the init role needs exec", f->field[1]);
        code = ETUDE_ERR_REFUSED;
    }
    a = code ? NULL : calloc(1, sizeof(*a));
    if (!code && !a)
        code = ETUDE_ERR_FAILED;
    if (!code) {
        snprintf(a->token, sizeof(a->token), "%s", f->field[1]);
        snprintf(a->cmd, sizeof(a->cmd), "%s", cmd);
        a->fd[1] = a->fd[2] = -1;
        sha256_init(&a->sent[1]);
        sha256_init(&a->sent[2]);
        a->keep_max = (unsigned long long)inv.output_max;
        a->grace_us = inv.grace_us;
        a->fx = inv.fx;
        if (path_of(a->dir, "%s/a/%s", opt.state, a->token) < 0)
            code = ETUDE_ERR_LIMIT;
    }
    if (!code)
        code = prepare(&inv, a->dir);
    if (!code) {
        made = 1;
        attempt_record(a, ST_PREPARING, NULL);
        code = fixture_prepare(&a->fx, a->dir, &why);
        if (code)
            note("run %s: %s", a->token, why);
    }
    if (!code) {
        a->next = attempts;
        attempts = a;
        a->deadline = now_us() + inv.deadline_us;
        code = spawn(a, &inv);
        if (code) {
            attempts = a->next;
            if (a->pid > 0) {
                waitpid(a->pid, NULL, 0);
                a->pid = 0;
            }
        }
    }
    if (code && made) {
        if (fixture_cleanup(&a->fx, a->dir) < 0)
            note("run %s: a fixture was left", a->token);
        attempt_record(a, ST_CLEANED, NULL);
    }
    eh_json_free(inv.root);
    if (code) {
        if (a && (!attempts || attempts != a))
            free(a);
        cmd_error(cmd, code);
        return;
    }
    snprintf(line, sizeof(line), "%s %s %s %ld %llu", ETUDE_EV_STARTED, cmd, a->token, (long)a->pid,
             a->start);
    cmd_event(cmd, line);
}

/** Put the console back as it was found. */
static void restore_tty(void)
{
    if (tty_saved)
        tcsetattr(in_fd, TCSANOW, &saved_tty);
}

/**
 * exec CMD ATTEMPT INVOCATION-SHA256: run a case of the init role in this
 * process's place, so that the program becomes the boot's designated init.
 * Its end is the kernel's record, which the controller reads from the
 * console, and no frame follows `started`.
 */
static void do_exec(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    char *argv[ARGS_MAX + 1];
    char *envp[ARGS_MAX + 1];
    char work[PATH_CAP];
    char prog[PATH_CAP];
    char line[LINE_CAP];
    struct invocation inv;
    struct attempt a;
    const char *code;

    if (f->nfields != 3) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    if (opt.stdio || attempts || send_file.active ||
        !wire_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN) || !wire_hex(f->field[2], 64)) {
        note("exec: refused: %s", opt.stdio ? "not on the serial console" : "an attempt runs");
        cmd_error(cmd, ETUDE_ERR_REFUSED);
        return;
    }
    code = load_invocation(&inv, f->field[2], f->field[1]);
    if (!code && (strcmp(inv.role, "init") != 0 || !fixture_simple(&inv.fx)))
        code = ETUDE_ERR_REFUSED;
    memset(&a, 0, sizeof(a));
    snprintf(a.token, sizeof(a.token), "%s", f->field[1]);
    snprintf(a.cmd, sizeof(a.cmd), "%s", cmd);
    a.pid = self_pid;
    a.start = self_start;
    if (!code && path_of(a.dir, "%s/a/%s", opt.state, a.token) < 0)
        code = ETUDE_ERR_LIMIT;
    if (!code)
        code = prepare(&inv, a.dir);
    if (!code && (path_of(work, "%s/work", a.dir) < 0 ||
                  path_of(prog, inv.program[0] == '/' ? "%s%s" : "%s/%s",
                          inv.program[0] == '/' ? "" : work, inv.program) < 0 ||
                  build_vectors(&inv, work, inv.program, argv, envp) < 0))
        code = ETUDE_ERR_LIMIT;
    if (code) {
        eh_json_free(inv.root);
        cmd_error(cmd, code);
        return;
    }
    attempt_record(&a, ST_RUNNING, NULL);
    snprintf(line, sizeof(line), "%s %s %s %ld %llu", ETUDE_EV_STARTED, cmd, a.token,
             (long)self_pid, self_start);
    cmd_event(cmd, line);
    /* The started frame and every frame before it must be acknowledged before
     * the program takes this process, since nothing can send them again. */
    exec_next.pending = 1;
    exec_next.until = now_us() + inv.deadline_us;
    memcpy(exec_next.argv, argv, sizeof(argv));
    memcpy(exec_next.envp, envp, sizeof(envp));
    snprintf(exec_next.work, sizeof(exec_next.work), "%s", work);
    snprintf(exec_next.prog, sizeof(exec_next.prog), "%s", prog);
    eh_json_free(inv.root);
}

/** Execute the program an exec started, once the queue is empty, or give it
 * up when its deadline passes first. */
static void exec_when_acknowledged(void)
{
    if (!exec_next.pending || (queue_head && now_us() < exec_next.until))
        return;
    if (queue_head) {
        note("exec: the started frame was not acknowledged");
        exec_next.pending = 0;
        free_vectors(exec_next.argv, exec_next.envp);
        return;
    }
    restore_tty();
    if (log_fd >= 0)
        close(log_fd);
    close_from_3(-1);
    if (chdir(exec_next.work) < 0)
        _exit(127);
    umask(022);
    default_signals();
    execve(exec_next.prog, exec_next.argv, exec_next.envp);
    _exit(127);
}

/** cancel CMD ATTEMPT */
static void do_cancel(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    struct attempt *a;
    char line[LINE_CAP];

    if (f->nfields != 2) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    a = find_attempt(f->field[1]);
    if (!a) {
        char path[PATH_CAP];
        static char buf[4096];
        const char *p;
        if (!wire_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN) ||
            path_of(path, "%s/a/%s/record", opt.state, f->field[1]) < 0 ||
            read_file(path, buf, sizeof(buf)) < 0 || !(p = strstr(buf, "\nstate "))) {
            cmd_error(cmd, ETUDE_ERR_UNKNOWN);
            return;
        }
        snprintf(line, sizeof(line), "%s %s %s %.*s", ETUDE_EV_STATE, cmd, f->field[1],
                 (int)strcspn(p + 7, "\n"), p + 7);
        cmd_event(cmd, line);
        return;
    }
    stop_attempt(a);
    snprintf(line, sizeof(line), "%s %s %s %s", ETUDE_EV_STATE, cmd, a->token,
             a->exited ? "ended" : "running");
    cmd_event(cmd, line);
}

#if defined(__APPLE__)
/**
 * Count the processes of group `pgid` that the sysctl KERN_PROC_PGRP lists:
 * each one in `*seen`, and in `*live` each one that is not a zombie and is
 * in session `pgid` as getsid(2) gives it.
 *
 * @return 0, or -1 when the list cannot be read.
 */
static int count_group(pid_t pgid, int *live, int *seen)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, 0};
    struct kinfo_proc *kp = NULL;
    size_t len = 0;
    size_t i;
    int tries;

    mib[3] = (int)pgid;
    for (tries = 0; tries < 3 && !kp; tries++) {
        if (sysctl(mib, 4, NULL, &len, NULL, 0) < 0)
            return -1;
        len += len / 4 + sizeof(*kp);
        kp = malloc(len);
        if (kp && sysctl(mib, 4, kp, &len, NULL, 0) < 0) {
            free(kp);
            kp = NULL;
            if (errno != ENOMEM)
                return -1;
        }
    }
    if (!kp)
        return -1;
    for (i = 0; i < len / sizeof(*kp); i++) {
        (*seen)++;
        if (kp[i].kp_proc.p_stat != SZOMB && getsid(kp[i].kp_proc.p_pid) == pgid)
            (*live)++;
    }
    free(kp);
    return 0;
}
#else
/**
 * Count the processes of group `pgid` that /proc shows: each one in
 * `*seen`, and in `*live` each one that is not a zombie and is in session
 * `pgid`.
 *
 * @return 0, or -1 when /proc cannot be read.
 */
static int count_group(pid_t pgid, int *live, int *seen)
{
    static char buf[1024];
    char path[64];
    struct dirent *e;
    const char *p;
    size_t n;
    long group;
    long sid;
    char state;
    DIR *d = opendir("/proc");

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        n = strlen(e->d_name);
        if (n == 0 || n > 20 || strspn(e->d_name, "0123456789") != n)
            continue;
        snprintf(path, sizeof(path), "/proc/%.20s/stat", e->d_name);
        /* After the command's ')' come the state, parent, group and session. */
        if (read_file(path, buf, sizeof(buf)) <= 0 || !(p = strrchr(buf, ')')) ||
            sscanf(p + 1, " %c %*d %ld %ld", &state, &group, &sid) != 3 || group != (long)pgid)
            continue;
        (*seen)++;
        if (state != 'Z' && state != 'X' && sid == (long)pgid)
            (*live)++;
    }
    closedir(d);
    return 0;
}
#endif

/**
 * Count the processes left running of the group that an attempt's child
 * made, the child that `child` and `start` name: the processes of group
 * `child` that are in session `child` too, the session the child made with
 * setsid(2), while `child` names that process or none at all. A zombie is
 * not counted, since it has ended and waits only for its parent to reap it,
 * perhaps this process as the boot's init.
 *
 * A group's number is the PID of the process that made it. Linux and macOS
 * give a new process no PID that a process or a process group still has, as
 * POSIX requires, and Rach2 gives its PIDs out in order and comes back to
 * the first only after two thousand million. So while a process of the
 * attempt's group is left, no other process or group has its number, and
 * when `child` names another process, the attempt's group has ended. A group
 * of that number and session is another's only when, in the same boot, the
 * attempt's group ended, the PIDs came round to the number again, and the
 * process given it made a session of its own and ended before this look.
 * The caller rules out a record of another boot, whose PIDs began again.
 *
 * @return the count, or -1 when it cannot be told: the processes cannot be
 * listed, or none of the group is listed and kill(2) finds one.
 */
static int attempt_left(pid_t child, unsigned long long start)
{
    unsigned long long now = proc_start(child);
    int live = 0;
    int seen = 0;

    if (now != 0 && now != start)
        return 0;
    if (count_group(child, &live, &seen) < 0 || seen == 0)
        return kill(-child, 0) < 0 && errno == ESRCH ? 0 : -1;
    return live;
}

/**
 * Whether no process of the group that an attempt's child made is left
 * running, as attempt_left counts them, within `bound_us` microseconds. It
 * is looked at every 10 ms until none is left or the bound has passed.
 */
static int attempt_ended(pid_t child, unsigned long long start, int64_t bound_us)
{
    int64_t until = now_us() + bound_us;
    struct timespec ts;

    for (;;) {
        if (attempt_left(child, start) == 0)
            return 1;
        if (now_us() >= until)
            return 0;
        ts.tv_sec = 0;
        ts.tv_nsec = 10000000;
        nanosleep(&ts, NULL);
    }
}

/**
 * Settle the record of an attempt whose owner is gone without writing its
 * end, as when an etude-guest was killed: what is left of the group that its
 * child made is killed, whether or not the child itself still runs, and the
 * attempt is recorded as lost, since neither its end nor its output's sizes
 * were seen. Nothing of an attempt of another boot runs, and nothing is
 * killed for it; a record that gives no boot is taken as this boot's, and
 * its boot is written back as `-`. Its cleanup is confirmed once nothing of
 * the group is left, which a killed group is given SETTLE_US to do.
 * `record` is given the settled record's text.
 *
 * @return 1 when the record was settled, 0 when it needed nothing.
 */
static int settle_orphan(const char *token, char *record)
{
    char cmd[24];
    char sess[ETUDE_FRAME_TOKEN_LEN + 1];
    char was[ETUDE_FRAME_TOKEN_LEN + 1] = "-";
    long owner = 0;
    long child = 0;
    unsigned long long owner_start = 0;
    unsigned long long child_start = 0;
    char *p;
    char finals[5 * LINE_CAP];
    char path[PATH_CAP];
    char text[4096];
    int here;
    int left;
    int killed;
    int gone;

    if (sscanf(record, "cmd %23s\nsession %16s\nowner %ld %llu\nchild %ld %llu", cmd, sess, &owner,
               &owner_start, &child, &child_start) != 6)
        return 0;
    p = strstr(record, "\nstate ");
    if (!p || (strncmp(p + 7, "running", 7) != 0 && strncmp(p + 7, "preparing", 9) != 0) ||
        same_process((pid_t)owner, owner_start))
        return 0;
    p = strstr(record, "\nboot ");
    here = !p || (sscanf(p + 6, "%16s", was) == 1 && strcmp(was, boot) == 0);
    left = child > 0 && here ? attempt_left((pid_t)child, child_start) : 0;
    killed = left > 0 && kill(-(pid_t)child, SIGKILL) == 0;
    gone = left == 0 || attempt_ended((pid_t)child, child_start, killed ? SETTLE_US : 0);
    if (path_of(path, "%s/a/%s", opt.state, token) < 0 || fixture_settle(path) < 0)
        gone = 0;
    snprintf(finals, sizeof(finals),
             "%s %s %s lost -\n%s %s %s 1 - - -\n%s %s %s 2 - - -\n%s %s %s - - - -\n%s %s %s %s\n",
             ETUDE_EV_EXIT, cmd, token, ETUDE_EV_STREAM, cmd, token, ETUDE_EV_STREAM, cmd, token,
             ETUDE_EV_USAGE, cmd, token, ETUDE_EV_CLEANED, cmd, token,
             gone ? "confirmed" : "unconfirmed");
    snprintf(text, sizeof(text),
             "cmd %s\nsession %s\nowner %ld %llu\nchild %ld %llu\nboot %s\nstate %s\n%s", cmd, sess,
             (long)self_pid, self_start, child, child_start, was, gone ? "cleaned" : "ended",
             finals);
    if (path_of(path, "%s/a/%s/record", opt.state, token) == 0)
        write_file(path, text, strlen(text));
    snprintf(record, 4096, "%s", text);
    note("attempt %s: its etude-guest was gone; recorded as lost", token);
    return 1;
}

/** status CMD ATTEMPT: the attempt's state, and its final events again. */
static void do_status(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    struct attempt *a;
    char line[LINE_CAP];
    char path[PATH_CAP];
    static char buf[4096];
    char *p;
    char *end;
    int n;

    if (f->nfields != 2) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    /* A status changes nothing, so a repeat is answered from the present
     * state; the record still refuses the number with other fields. */
    if (!cmd_begin(cmd, text)) {
        char rec[PATH_CAP];
        static char first[1024];
        if (cmd_path(rec, cmd) < 0 || read_file(rec, first, sizeof(first)) < 0 ||
            strncmp(first, text, strlen(text)) != 0 || first[strlen(text)] != '\n')
            return;
    }
    a = find_attempt(f->field[1]);
    if (a) {
        snprintf(line, sizeof(line), "%s %s %s %s", ETUDE_EV_STATE, cmd, a->token,
                 a->exited ? "ended" : state_name[ST_RUNNING]);
        send_line(line);
        return;
    }
    if (!wire_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN) ||
        path_of(path, "%s/a/%s/record", opt.state, f->field[1]) < 0 ||
        read_file(path, buf, sizeof(buf)) < 0) {
        snprintf(line, sizeof(line), "%s %s %s unknown", ETUDE_EV_STATE, cmd, f->field[1]);
        send_line(line);
        return;
    }
    settle_orphan(f->field[1], buf);
    p = strstr(buf, "\nstate ");
    if (!p)
        return;
    n = (int)strcspn(p + 7, "\n");
    snprintf(line, sizeof(line), "%s %s %s %.*s", ETUDE_EV_STATE, cmd, f->field[1], n, p + 7);
    send_line(line);
    for (p = strchr(p + 1, '\n'); p && p[1]; p = end) {
        end = strchr(p + 1, '\n');
        if (!end)
            break;
        *end = '\0';
        send_line(p + 1);
        *end = '\n';
    }
}

/** Remove the tree at `path`, without following a symbolic link; 0 or -1. */
static int remove_tree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char sub[PATH_CAP];
    int rc = 0;

    if (lstat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    d = opendir(path);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (path_of(sub, "%s/%s", path, e->d_name) < 0 || remove_tree(sub) < 0)
            rc = -1;
    }
    closedir(d);
    if (rmdir(path) < 0)
        rc = -1;
    return rc;
}

/** remove CMD ATTEMPT: remove an ended attempt's workspace and output; its
 * record stays for status. */
static void do_remove(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    char path[PATH_CAP];
    char line[LINE_CAP];
    static char buf[4096];
    struct stat st;
    int fdn;

    if (f->nfields != 2) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    if (find_attempt(f->field[1])) {
        cmd_error(cmd, ETUDE_ERR_REFUSED);
        return;
    }
    if (!wire_hex(f->field[1], ETUDE_FRAME_TOKEN_LEN) ||
        path_of(path, "%s/a/%s/record", opt.state, f->field[1]) < 0 || stat(path, &st) < 0) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        settle_orphan(f->field[1], buf);
        if (!strstr(buf, "\nstate ended\n") && !strstr(buf, "\nstate cleaned\n")) {
            cmd_error(cmd, ETUDE_ERR_REFUSED);
            return;
        }
    }
    if (path_of(path, "%s/a/%s/work", opt.state, f->field[1]) < 0 || remove_tree(path) < 0) {
        note("remove: %s: %s", path, strerror(errno));
        cmd_error(cmd, ETUDE_ERR_FAILED);
        return;
    }
    for (fdn = 1; fdn <= 2; fdn++)
        if (path_of(path, "%s/a/%s/out.%d", opt.state, f->field[1], fdn) == 0)
            unlink(path);
    snprintf(line, sizeof(line), "%s %s %s", ETUDE_EV_REMOVED, cmd, f->field[1]);
    cmd_event(cmd, line);
}

/**
 * get CMD NAME: send a file of an attempt's directory back. NAME is the
 * attempt's token, a slash and a path in its directory, such as
 * `TOKEN/out.1` or `TOKEN/work/core`.
 */
static void do_get(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    const char *name;
    const char *p;
    char path[PATH_CAP];

    if (f->nfields != 2) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    name = f->field[1];
    if ((!absolute_file(name) &&
         (strlen(name) <= ETUDE_FRAME_TOKEN_LEN + 1 || name[ETUDE_FRAME_TOKEN_LEN] != '/')) ||
        strlen(name) >= sizeof(send_file.name) || send_file.active) {
        cmd_error(cmd, send_file.active ? ETUDE_ERR_REFUSED : ETUDE_ERR_UNKNOWN);
        return;
    }
    for (p = name[0] == '/' ? name : name + ETUDE_FRAME_TOKEN_LEN; p; p = strchr(p + 1, '/'))
        if (p[1] == '/' || p[1] == '\0' || strncmp(p + 1, "./", 2) == 0 ||
            strncmp(p + 1, "../", 3) == 0 || strcmp(p + 1, ".") == 0 || strcmp(p + 1, "..") == 0) {
            cmd_error(cmd, ETUDE_ERR_UNKNOWN);
            return;
        }
    if (path_of(path, name[0] == '/' ? "%s%s" : "%s/a/%s", name[0] == '/' ? "" : opt.state, name) <
        0) {
        cmd_error(cmd, ETUDE_ERR_LIMIT);
        return;
    }
    memset(&send_file, 0, sizeof(send_file));
    send_file.fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (send_file.fd < 0) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    snprintf(send_file.cmd, sizeof(send_file.cmd), "%s", cmd);
    snprintf(send_file.name, sizeof(send_file.name), "%s", name);
    sha256_init(&send_file.digest);
    send_file.active = 1;
}

/** Send the next part of the file a get asked for, while the queue has
 * room, and `sent` at its end. */
static void pump_get(void)
{
    unsigned char buf[4095];
    static char line[8192];
    unsigned char d[32];
    char hex[65];

    while (send_file.active && queue_room() && !output_broken) {
        ssize_t r = read(send_file.fd, buf, out_chunk());
        int k;
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            close(send_file.fd);
            send_file.active = 0;
            if (r < 0) {
                cmd_error(send_file.cmd, ETUDE_ERR_FAILED);
                return;
            }
            sha256_final(&send_file.digest, d);
            sha256_hex(d, hex);
            snprintf(line, sizeof(line), "%s %s %s %llu %s", ETUDE_EV_SENT, send_file.cmd,
                     send_file.name, (unsigned long long)send_file.off, hex);
            cmd_event(send_file.cmd, line);
            return;
        }
        sha256_update(&send_file.digest, buf, (size_t)r);
        k = snprintf(line, sizeof(line), "%s %s %llu ", ETUDE_CMD_DATA, send_file.cmd,
                     (unsigned long long)send_file.off);
        wire_base64(buf, (size_t)r, line + k);
        send_line(line);
        send_file.off += (uint64_t)r;
    }
}

/** hello CMD VERSIONS MAXFRAME */
static void do_hello(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    char line[LINE_CAP];
    char versions[64];
    uint64_t theirs;
    size_t ours = opt.stdio ? ETUDE_FRAME_STDIO_MAX : ETUDE_FRAME_SERIAL_MAX;
    char *v;
    int common = 0;

    if (f->nfields != 3 || !wire_number(f->field[2], &theirs) || theirs < 128 ||
        strlen(f->field[1]) >= sizeof(versions)) {
        cmd_error(cmd, ETUDE_ERR_UNKNOWN);
        return;
    }
    if (!cmd_begin(cmd, text))
        return;
    strcpy(versions, f->field[1]);
    for (v = strtok(versions, ","); v; v = strtok(NULL, ","))
        if (strcmp(v, GUEST_VERSIONS) == 0)
            common = 1;
    if (!common) {
        cmd_error(cmd, ETUDE_ERR_VERSION);
        return;
    }
    if (theirs < ours)
        ours = (size_t)theirs;
    snprintf(line, sizeof(line), "%s %s %s %lu %ld %llu", ETUDE_EV_READY, cmd, GUEST_VERSIONS,
             (unsigned long)ours, (long)self_pid, self_start);
    /* The ready frame itself still fits the bound in force before it. */
    cmd_event(cmd, line);
    max_frame = ours;
}

/** bye CMD: end the session once every attempt has ended. */
static void do_bye(const struct wire_frame *f, const char *text)
{
    const char *cmd = f->field[0];
    char line[64];
    struct attempt *a;

    if (!cmd_begin(cmd, text))
        return;
    for (a = attempts; a; a = a->next)
        stop_attempt(a);
    snprintf(line, sizeof(line), "%s %s", ETUDE_CMD_BYE, cmd);
    cmd_event(cmd, line);
    closing = 1;
}

/* ------------------------------------------------------------------- input */

/** Rebuild the command text "TYPE FIELD..." that a command's record keeps. */
static void command_text(const struct wire_frame *f, char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "%s", f->type);
    int i;

    for (i = 0; i < f->nfields && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, " %s", f->field[i]);
}

/**
 * Take the lock of the session, STATE/s/SESSION/lock, over standard I/O. An
 * etude-guest of an earlier connection of the session holds it until it
 * ends, which it does once it sees its connection lost. The one that the
 * session's owner file names, when its start shows it is still that process,
 * is sent SIGTERM, which ends its attempts as a lost connection does, and the
 * lock is taken once it has recorded their ends and exited. The wait ends
 * early when this process is signalled or its input is closed.
 *
 * @return 0 with the lock held and this process in the owner file, or -1.
 */
static int lock_session(void)
{
    char path[PATH_CAP];
    char text[64];
    char drain[64];
    struct pollfd p[2];
    unsigned long long start;
    long pid;
    long signalled = 0;
    int fd;
    int n;

    if (path_of(path, "%s/s/%s/lock", opt.state, session) < 0)
        return -1;
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    while (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK && errno != EINTR) {
            close(fd);
            return -1;
        }
        if (path_of(path, "%s/s/%s/owner", opt.state, session) == 0 &&
            read_file(path, text, sizeof(text)) > 0 &&
            sscanf(text, "%ld %llu", &pid, &start) == 2 && pid != signalled &&
            pid != (long)self_pid && same_process((pid_t)pid, start)) {
            note("session: etude-guest %ld of an earlier connection is told to end", pid);
            kill((pid_t)pid, SIGTERM);
            signalled = pid;
        }
        p[0].fd = sig_pipe[0];
        p[0].events = POLLIN;
        p[1].fd = in_fd;
        p[1].events = 0;
        n = poll(p, 2, 50);
        if (n > 0 && (p[0].revents & POLLIN))
            while (read(sig_pipe[0], drain, sizeof(drain)) > 0)
                ;
        if (got_end || (n > 0 && (p[1].revents & (POLLHUP | POLLERR | POLLNVAL)))) {
            close(fd);
            return -1;
        }
    }
    snprintf(text, sizeof(text), "%ld %llu\n", (long)self_pid, self_start);
    if (path_of(path, "%s/s/%s/owner", opt.state, session) < 0 ||
        write_file(path, text, strlen(text)) < 0) {
        close(fd);
        return -1;
    }
    lock_fd = fd;
    return 0;
}

/**
 * Append to the record of run `cmd` each final event of its attempt's
 * record, the text `record`, that the run's record lacks. An attempt's end is
 * written whole to its own record and then appended to its run's one event
 * at a time, so an etude-guest killed between the two leaves the run's
 * record with none or some of the events, the last perhaps cut short. A
 * last line cut short, which has no newline, is removed first.
 */
static void complete_run(const char *cmd, const char *record)
{
    static char have[64 * 1024];
    char path[PATH_CAP];
    char line[LINE_CAP + 2];
    const char *p;
    const char *end;
    char *last;
    long n;
    int fd;

    p = strstr(record, "\nstate ");
    p = p ? strchr(p + 1, '\n') : NULL;
    if (!p || !p[1] || cmd_path(path, cmd) < 0 || (n = read_file(path, have, sizeof(have))) <= 0 ||
        (size_t)n == sizeof(have) - 1 || !(last = strrchr(have, '\n')))
        return;
    if (last[1] != '\0') {
        last[1] = '\0';
        fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)(last + 1 - have)) < 0 || fsync(fd) < 0) {
            note("record: %s: %s", path, strerror(errno));
            if (fd >= 0)
                close(fd);
            return;
        }
        close(fd);
    }
    for (; p[1]; p = end) {
        end = strchr(p + 1, '\n');
        if (!end || end - p > LINE_CAP)
            break;
        snprintf(line, sizeof(line), "%.*s", (int)(end - p + 1), p);
        if (!strstr(have, line)) {
            line[end - p] = '\0';
            cmd_append(cmd, line + 1);
        }
    }
}

/**
 * Settle the attempts of this session whose etude-guest is gone without
 * having recorded their ends, as settle_orphan does, and give the record of
 * each attempt's run the final events it lacks, as complete_run does, so
 * that a run sent again is answered with a whole end.
 */
static void settle_session(void)
{
    static char buf[4096];
    char path[PATH_CAP];
    char sess[ETUDE_FRAME_TOKEN_LEN + 1];
    char cmd[24];
    struct dirent *e;
    DIR *d;

    if (path_of(path, "%s/a", opt.state) < 0 || !(d = opendir(path)))
        return;
    while ((e = readdir(d)) != NULL) {
        if (!wire_hex(e->d_name, ETUDE_FRAME_TOKEN_LEN) ||
            path_of(path, "%s/a/%s/record", opt.state, e->d_name) < 0 ||
            read_file(path, buf, sizeof(buf)) < 0 ||
            sscanf(buf, "cmd %23s\nsession %16s", cmd, sess) != 2 || strcmp(sess, session) != 0)
            continue;
        settle_orphan(e->d_name, buf);
        complete_run(cmd, buf);
    }
    closedir(d);
}

/**
 * Start a session from a hello frame. Over standard I/O the session's lock
 * is taken first, and the attempts that an etude-guest of the session left
 * unsettled are settled.
 */
static int open_session(const struct wire_frame *f)
{
    char path[PATH_CAP];

    if (strcmp(f->type, ETUDE_CMD_HELLO) != 0 || f->seq != 0 ||
        strcmp(f->session, ETUDE_FRAME_NO_SESSION) == 0)
        return -1;
    snprintf(session, sizeof(session), "%s", f->session);
    if (path_of(path, "%s/s/%s", opt.state, session) < 0 ||
        (mkdir(path, 0700) < 0 && errno != EEXIST)) {
        note("session: %s: %s", path, strerror(errno));
        return -1;
    }
    if (opt.stdio) {
        if (lock_session() < 0) {
            note("session: %s: the lock was not taken", session);
            input_closed = 1;
            return -1;
        }
        settle_session();
    }
    in_session = 1;
    if (!opt.stdio)
        next_seq = 0;
    recv_next = 0;
    max_frame = ETUDE_FRAME_SERIAL_MAX;
    return 0;
}

/** Act on the command of frame `f`, which arrived in order. */
static void dispatch(struct wire_frame *f)
{
    char text[LINE_CAP];
    uint64_t cmd;

    if (f->nfields < 1 || !wire_number(f->field[0], &cmd)) {
        send_line("error - unknown");
        return;
    }
    if (exec_next.pending) {
        cmd_error(f->field[0], ETUDE_ERR_REFUSED);
        return;
    }
    command_text(f, text, sizeof(text));
    if (strcmp(f->type, ETUDE_CMD_HELLO) == 0)
        do_hello(f, text);
    else if (strcmp(f->type, ETUDE_CMD_PUT) == 0)
        do_put(f, text);
    else if (strcmp(f->type, ETUDE_CMD_DATA) == 0)
        do_data(f);
    else if (strcmp(f->type, ETUDE_CMD_SEAL) == 0)
        do_seal(f);
    else if (strcmp(f->type, ETUDE_CMD_RUN) == 0)
        do_run(f, text);
    else if (strcmp(f->type, ETUDE_CMD_EXEC) == 0)
        do_exec(f, text);
    else if (strcmp(f->type, ETUDE_CMD_CANCEL) == 0)
        do_cancel(f, text);
    else if (strcmp(f->type, ETUDE_CMD_STATUS) == 0)
        do_status(f, text);
    else if (strcmp(f->type, ETUDE_CMD_REMOVE) == 0)
        do_remove(f, text);
    else if (strcmp(f->type, ETUDE_CMD_GET) == 0)
        do_get(f, text);
    else if (strcmp(f->type, ETUDE_CMD_BYE) == 0)
        do_bye(f, text);
    else
        cmd_error(f->field[0], ETUDE_ERR_UNKNOWN);
}

/** Act on one line of input. */
static void take_line(char *line, size_t len)
{
    struct wire_frame f;

    if (len && line[len - 1] == '\r')
        len--;
    if (len == 0)
        return;
    if (wire_parse(line, len, &f) < 0) {
        note("input: not a frame: %.*s", (int)(len > 200 ? 200 : len), line);
        if (opt.stdio)
            input_closed = 1;
        else
            need_ack = in_session;
        return;
    }
    if (strcmp(f.boot, boot) != 0) {
        note("input: a frame of boot %s", f.boot);
        return;
    }
    if (!in_session && open_session(&f) < 0) {
        note("input: a frame before the session: %s", f.type);
        return;
    }
    if (strcmp(f.session, session) != 0) {
        note("input: a frame of session %s", f.session);
        return;
    }
    if (f.has_ack && !opt.stdio)
        acknowledged(f.ack);
    if (strcmp(f.type, ETUDE_CMD_ACK) == 0)
        return;
    if (f.seq != recv_next) {
        if (opt.stdio) {
            note("input: SEQ %llu where %llu was due", (unsigned long long)f.seq,
                 (unsigned long long)recv_next);
            input_closed = 1;
        }
        need_ack = 1;
        return;
    }
    recv_next++;
    need_ack = !opt.stdio;
    dispatch(&f);
}

/** Read the input and act on each whole line. A line longer than the frame
 * bound ends the connection over standard I/O and is dropped on the serial
 * console. */
static void read_input(void)
{
    size_t bound = (in_session ? max_frame : ETUDE_FRAME_SERIAL_MAX) - 1;
    ssize_t r;
    size_t i;
    size_t start;

    r = read(in_fd, in_buf + in_len, sizeof(in_buf) - 1 - in_len);
    if (r < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (r <= 0) {
        input_closed = 1;
        return;
    }
    sent_since_input = 0;
    in_len += (size_t)r;
    start = 0;
    for (i = 0; i < in_len && !input_closed; i++) {
        if (in_buf[i] != '\n')
            continue;
        if (in_skip)
            in_skip = 0;
        else if (i - start > bound + 1)
            note("input: a line over the frame bound");
        else
            take_line(in_buf + start, i - start);
        start = i + 1;
    }
    memmove(in_buf, in_buf + start, in_len - start);
    in_len -= start;
    if (in_len > bound + 1) {
        note("input: a line over the frame bound");
        if (opt.stdio)
            input_closed = 1;
        in_skip = 1;
        in_len = 0;
    }
}

/* -------------------------------------------------------------------- main */

/** Record a signal that ends the connection, and wake the loop. */
static void on_signal(int sig)
{
    int saved = errno;
    char c = (char)sig;

    if (sig != SIGCHLD)
        got_end = 1;
    if (write(sig_pipe[1], &c, 1) < 0)
        errno = saved;
    errno = saved;
}

/** Make the state directories under `opt.state`: the state directory and
 * `a`, mode 0711, which a case's other ids search to reach their workspaces,
 * and `f` and `s`, 0700. */
static int make_state(void)
{
    static const char *const sub[] = {"", "/a", "/f", "/s"};
    static const mode_t mode[] = {0711, 0711, 0700, 0700};
    char path[PATH_CAP];
    size_t i;

    for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++)
        if (path_of(path, "%s%s", opt.state, sub[i]) < 0 ||
            (mkdir(path, mode[i]) < 0 && errno != EEXIST) || chmod(path, mode[i]) < 0)
            return -1;
    if (path_of(path, "%s/guest.log", opt.state) < 0)
        return -1;
    log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    return log_fd < 0 ? -1 : 0;
}

/** Put the console in raw mode, so that a frame passes through it as bytes:
 * no echo, no line editing, no signal characters and no output processing. */
static void raw_tty(void)
{
    struct termios t;

    if (!isatty(in_fd) || tcgetattr(in_fd, &saved_tty) < 0)
        return;
    tty_saved = 1;
    t = saved_tty;
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(in_fd, TCSANOW, &t);
}

/** Print the usage and return 2. */
static int usage(void)
{
    fprintf(stderr, "usage: etude-guest [--stdio] [--state DIR] [--stage DIR]... "
                    "[--resend-ms N]\n");
    return 2;
}

/**
 * Stop the attempts once the connection is gone, and decide whether the loop
 * ends. Over the serial console a session that `bye` ended returns to
 * waiting for the next hello once its frames are acknowledged.
 *
 * @return 1 when the loop ends, 0 when it goes on.
 */
static int session_over(void)
{
    struct attempt *a;

    if (got_end && !input_closed) {
        note("ended by a signal");
        input_closed = 1;
    }
    if (input_closed || output_broken)
        for (a = attempts; a; a = a->next)
            stop_attempt(a);
    if (attempts || !(input_closed || output_broken || closing))
        return 0;
    if (input_closed || output_broken)
        return 1;
    if (opt.stdio)
        return !queue_head;
    if (!queue_head) {
        closing = 0;
        in_session = 0;
        snprintf(session, sizeof(session), "%s", ETUDE_FRAME_NO_SESSION);
        last_alive = 0;
    }
    return 0;
}

/** Send `alive` at time `t` while the serial console has no session, once
 * each resend interval. */
static void alive_when_due(int64_t t)
{
    char pid[24];
    char start[24];
    const char *fl[3];

    if (in_session || opt.stdio || t - last_alive < opt.resend_us)
        return;
    snprintf(pid, sizeof(pid), "%ld", (long)self_pid);
    snprintf(start, sizeof(start), "%llu", self_start);
    fl[0] = GUEST_VERSIONS;
    fl[1] = pid;
    fl[2] = start;
    send_frame(ETUDE_EV_ALIVE, 3, fl);
    last_alive = t;
}

/** Lower `*timeout` to the milliseconds until `when`, from `t`, both in
 * microseconds; at least 1. */
static void sooner(int *timeout, int64_t when, int64_t t)
{
    int64_t left = (when - t) / 1000 + 1;

    if (left < *timeout)
        *timeout = left < 1 ? 1 : (int)left;
}

/** The milliseconds to wait at time `t` before the next alive, resend,
 * deadline, grace period or drain ends; a second at most. */
static int next_timeout(int64_t t)
{
    struct attempt *a;
    int timeout = 1000;

    if (!opt.stdio && !in_session)
        sooner(&timeout, last_alive + opt.resend_us, t);
    if (!opt.stdio && queue_head)
        sooner(&timeout, last_send + opt.resend_us, t);
    for (a = attempts; a; a = a->next) {
        if (a->exited && a->fd[1] < 0 && a->fd[2] < 0)
            sooner(&timeout, t + 19000, t);
        else
            sooner(&timeout, a->exited ? a->drain_until : a->kill_at ? a->kill_at : a->deadline, t);
    }
    return timeout;
}

/** The descriptors the loop waits on. */
struct waits {
    struct pollfd p[64];
    struct attempt *who[64]; /* the attempt of an output pipe */
    int which[64];           /* 0 the signal pipe, -1 input, -2 output, or the FD */
    int n;
};

/** Fill `w` with the signal pipe, the input, the output while frames wait
 * over standard I/O, and the attempts' output pipes while the queue has
 * room for what they write. */
static void wait_set(struct waits *w)
{
    struct attempt *a;
    int i;

    w->n = 0;
    w->p[w->n].fd = sig_pipe[0];
    w->p[w->n].events = POLLIN;
    w->which[w->n++] = 0;
    if (!input_closed) {
        w->p[w->n].fd = in_fd;
        w->p[w->n].events = POLLIN;
        w->which[w->n++] = -1;
    }
    if (opt.stdio && queue_head && !output_broken) {
        w->p[w->n].fd = out_fd;
        w->p[w->n].events = POLLOUT;
        w->which[w->n++] = -2;
    }
    for (a = attempts; a && w->n < 62; a = a->next)
        for (i = 1; i <= 2; i++)
            if (a->fd[i] >= 0 && (queue_room() || output_broken || input_closed)) {
                w->p[w->n].fd = a->fd[i];
                w->p[w->n].events = POLLIN;
                w->who[w->n] = a;
                w->which[w->n++] = i;
            }
}

/** The loop: send what is due, wait for input, output or a deadline, and
 * handle what is ready. Returns the exit status. */
static int serve(void)
{
    static struct waits w;
    char drain[64];
    int64_t t;
    int i;

    while (!session_over()) {
        t = now_us();
        alive_when_due(t);
        pump_get();
        if (need_ack && !sent_since_input && in_session)
            send_frame(ETUDE_CMD_ACK, 0, NULL);
        need_ack = 0;
        flush();
        wait_set(&w);
        if (poll(w.p, (nfds_t)w.n, next_timeout(t)) < 0 && errno != EINTR) {
            note("poll: %s", strerror(errno));
            return 1;
        }
        for (i = 0; i < w.n; i++) {
            if (!w.p[i].revents)
                continue;
            if (w.which[i] == 0)
                while (read(sig_pipe[0], drain, sizeof(drain)) > 0)
                    ;
            else if (w.which[i] == -1)
                read_input();
            else if (w.which[i] > 0)
                read_output(w.who[i], w.which[i]);
        }
        reap();
        tick();
        flush();
        exec_when_acknowledged();
    }
    return input_closed && !closing ? 1 : 0;
}

/** etude-guest's main: the options, the state, the transport, the loop. */
int etude_guest_main(int argc, char **argv)
{
    struct sigaction sa;
    int i;

    opt.state = NULL;
    opt.resend_us = 2000000;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stdio") == 0) {
            opt.stdio = 1;
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            opt.state = argv[++i];
        } else if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc && opt.nstage < STAGE_MAX) {
            opt.stage[opt.nstage++] = argv[++i];
        } else if (strcmp(argv[i], "--resend-ms") == 0 && i + 1 < argc && atol(argv[i + 1]) > 0) {
            opt.resend_us = (int64_t)atol(argv[++i]) * 1000;
        } else {
            return usage();
        }
    }
    if (!opt.state) {
        static char path[PATH_CAP];
        const char *tmp = getenv("TMPDIR");
        snprintf(path, sizeof(path), "%s/etude-guest", tmp && *tmp ? tmp : "/tmp");
        opt.state = path;
    }
    self_pid = getpid();
    self_start = proc_start(self_pid);
    snprintf(session, sizeof(session), "%s", ETUDE_FRAME_NO_SESSION);
    max_frame = ETUDE_FRAME_SERIAL_MAX;
    if (make_state() < 0) {
        fprintf(stderr, "etude-guest: %s: %s\n", opt.state, strerror(errno));
        return 1;
    }
    if (find_boot() < 0) {
        note("boot: no token for this boot");
        if (opt.stdio)
            return 1;
        return 1;
    }
    if (pipe(sig_pipe) < 0)
        return 1;
    fcntl(sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(sig_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(sig_pipe[1], F_SETFD, FD_CLOEXEC);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (opt.stdio) {
        fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL) | O_NONBLOCK);
    } else {
        raw_tty();
    }
    fcntl(in_fd, F_SETFL, fcntl(in_fd, F_GETFL) | O_NONBLOCK);
    if (opt.stdio) {
        char pid[24];
        char start[24];
        const char *fl[3];
        snprintf(pid, sizeof(pid), "%ld", (long)self_pid);
        snprintf(start, sizeof(start), "%llu", self_start);
        fl[0] = GUEST_VERSIONS;
        fl[1] = pid;
        fl[2] = start;
        send_frame(ETUDE_EV_ALIVE, 3, fl);
    }
    i = serve();
    restore_tty();
    return i;
}

#ifndef RACH2_HOSTTEST
int main(int argc, char **argv)
{
    return etude_guest_main(argc, argv);
}
#endif
