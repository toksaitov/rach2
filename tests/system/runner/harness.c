/**
 * harness: boot one machine under QEMU and drive its serial console, for
 * probe and test runs.
 *
 * Boots qemu-system-i386 headless with the kernel and a KRFFS initrd,
 * attaches QEMU's isa-debug-exit device (iobase 0xF4), and drives the
 * guest with an expect and send script. With a HARNESS=1 kernel build the
 * kernel writes each process exit status to that port (rach2/sys/kio.c,
 * harness_exit), so QEMU stops with ((status << 1) | 1) and this reads a
 * real exit code: a hang or a triple fault then shows up as a timeout, not
 * as a missing serial marker.
 *
 * Usage:
 *   harness --kernel K [--initrd I] [--append LINE] [--net NETVAL]
 *           [--device DEV] [--drive SPEC ...] [--qemu-arg ARG ...]
 *           [--timeout S] [--steptmo S] [--grace S] [--ok TOKEN]
 *           [--never TEXT ...] [--must-exit] [--log FILE] [--stamp FILE]
 *           [--no-debug-exit] [STEP ...]
 *   harness --bios [OPTION ...] [STEP ...]
 *   harness --selftest
 *   harness --match PATTERN
 *
 *   --bios gives the emulator no kernel, so its firmware boots whatever the
 *   drives and devices hold, in the order their `bootindex` says, which is
 *   how a machine boots from a stick.
 *
 *   --timeout bounds the run, 60 seconds unless given, and --steptmo each
 *   step that waits for output, 15 unless given; both are multiplied by the
 *   set's deadline factor. --grace is how long the output is read after the
 *   last step, 3 seconds unless given. --ok TOKEN decides the run by that
 *   serial token instead of the guest's exit status. --log keeps the console
 *   in FILE, --stamp appends the times the runner calibrates with, and
 *   --no-debug-exit leaves the isa-debug-exit device out. --net and --device
 *   default to QEMU's user network on an rtl8139.
 *
 *   --must-exit fails the run unless QEMU ended by itself, by exiting,
 *   before the deadline. It is what tells a machine that restarted or halted
 *   from one that printed the line for it and then sat there; a machine that
 *   was killed, by this harness or by anything else, does not pass it.
 *
 *   --never TEXT fails the run if TEXT ever appears on the serial console,
 *   however the run ends. It is how a probe that must be killed is checked:
 *   the line it prints when it was not killed is forbidden. It may be given
 *   more than once.
 *
 *   STEP forms:
 *     expect=REGEX   wait until REGEX appears (per-step cap = --steptmo)
 *     boot=REGEX     internal prompt wait bounded by the run's --timeout
 *     seen=REGEX     wait until REGEX has appeared since the last send,
 *                    wherever it lies: two seen= steps are satisfied in
 *                    either order, two expect= steps only in theirs
 *     send=TEXT      write TEXT and a newline to the guest serial
 *     sendraw=TEXT   write TEXT with no newline, for a guest terminal in
 *                    raw mode, where the newline would be read as the
 *                    byte. `\\`, `\n`, `\r`, `\t` and `\xNN` in TEXT stand
 *                    for the bytes they name
 *     sendfile=PATH  write the bytes of the file at PATH to the guest
 *                    serial as they are, in one stream, which is how a
 *                    flood of lines is typed back to back
 *     key=KEYS       press KEYS on the machine's keyboard, as QEMU's
 *                    `sendkey` takes them
 *     mouse=DX,DY,B  move the mouse by DX and DY and, when the button mask
 *                    B is not 0, press those of its three buttons; a mask
 *                    of 0 sends no button command, which a machine that has
 *                    already exited would refuse
 *     usb=add:ARGS   plug in a device, as QEMU's `device_add ARGS`
 *     usb=del:ID     pull the device with that id out
 *     drive=add:ARGS make a medium, as `drive_add 0 ARGS`. A stick plugged
 *                    in after the boot is a drive=add: and a usb=add:
 *                    naming that drive; pulling the device leaves the
 *                    medium, so a replug is a usb=add: alone
 *     drive=del:ID   remove the medium with that id
 *     barrier=FILE   wait until FILE exists: the calibration barrier, which
 *                    the runner makes when every machine of the width has
 *                    reached it, so the measured work starts together. A
 *                    barrier not released before the deadline fails
 *     until=SECS     drain serial for at most SECS, ending half a second
 *                    after the run's --ok token appears in unconsumed
 *                    output from the current command, or when the machine
 *                    exits: for the wait after `send=exec /probe` whose
 *                    only purpose is that token, so a probe done in ten
 *                    seconds does not cost sixty. Never past --timeout
 *     tcp=PORT       open a TCP connection to 127.0.0.1:PORT, which a
 *                    `hostfwd=tcp::PORT-:GUESTPORT` in --net forwards to
 *                    a server inside the guest, read the first bytes the
 *                    server sends, an SSH banner, and close. The step
 *                    fails when nothing arrives within thirty seconds,
 *                    scaled as a deadline is: QEMU's user network accepts
 *                    the host side of a connection before it knows
 *                    whether anything is there, so only bytes from the
 *                    guest prove it answered, and they are on the wire by
 *                    then.
 *     udp=PORT:TEXT  send TEXT as one UDP datagram to 127.0.0.1:PORT,
 *                    which a `hostfwd=udp::PORT-:GUESTPORT` forwards into
 *                    the guest. This is the only way a test can put a
 *                    frame on the guest's wire that the guest did not ask
 *                    for.
 *     exec=NAME|MARK run "exec NAME" and wait for MARK, typing the line
 *                    again when MARK does not come within the step's
 *                    limit, four times in all
 *     probe=NAME     wait for boot under the run's timeout, run "exec NAME",
 *                    and let the kernel's harness_exit stop QEMU under
 *                    the step's limit. The line is typed again only while
 *                    the shell answers it with nothing, which is the shell
 *                    dropping it, or with "Unknown command", which is a
 *                    line typed before the shell was reading and missing
 *                    its first bytes. With --ok the step ends when the
 *                    token has arrived. It is expanded into boot= and
 *                    execq=NAME, the typed half
 *
 * Exit status: 0 when the run passed; 1 when it failed, which is a step
 * that failed, a --never text seen, a --must-exit machine that did not end
 * itself, an --ok token not seen, or a guest status other than 0; 2 for a
 * usage error or when the pipes or the fork for QEMU could not be made; 3
 * when, with no --ok, QEMU never exited by itself, which is a hang.
 *
 * With no --ok token the status is read off the kernel's "process: exited
 * with status N" record, which `sys_exit` emits only where the debug shell
 * is the supervisor; an image running /sbin/init prints no such line, so a
 * run of one of those is decided with --ok. QEMU's own exit code only
 * confirms that it stopped through isa-debug-exit.
 *
 * `harness --selftest` checks the escape decoder, the mouse step's parsing,
 * the probe's wait bounds and the bound on a write the machine does not
 * read, without QEMU. `harness --match PATTERN` prints where PATTERN first
 * ends in the text on standard input, or -1, which checks the matcher by
 * hand. `RACH2_TRACE=FILE` records each step's outcome and the final
 * decision.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS  128
#define MAX_STEPS 128
#define BUF_GROW  (1 << 16)

/* ---- the monitor ---- */

/*
 * QEMU's own command channel, on a unix socket beside the serial line. The
 * serial line is where the guest talks; this is where the machine is told what
 * a person at it would have done: a key pressed, the mouse moved, a device
 * plugged in or pulled out. Nothing else can produce those events, because
 * they come from the emulated hardware rather than from anything the guest
 * could be asked to do.
 *
 * The socket is made only for a run whose steps need it, so a run with no
 * such step starts QEMU with no -monitor at all.
 */
static char monitor_path[128];
static int monitor_fd = -1;

/**
 * Read a `mouse=` step's three numbers.
 *
 * Returns 0 when all three were there and the button mask is 0 through 7,
 * one bit for each of three buttons, and -1 otherwise. Separate from the
 * step that uses it so `--selftest` can check the parsing without a machine.
 */
static int parse_mouse(const char *text, int *dx, int *dy, int *buttons)
{
    if (text == NULL) {
        return -1;
    }
    if (sscanf(text, "%d,%d,%d", dx, dy, buttons) != 3) {
        return -1;
    }
    if (*buttons < 0 || *buttons > 7) {
        return -1;
    }
    return 0;
}

/** Whether any step needs the monitor. */
static int steps_need_monitor(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "key=", 4) == 0 || strncmp(argv[i], "mouse=", 6) == 0 ||
            strncmp(argv[i], "usb=", 4) == 0 || strncmp(argv[i], "drive=", 6) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The chunk in which the monitor complained since the last command, if it
 * did, so a refused command can be reported with its reason. The monitor
 * echoes a command with a redraw of the line per character, which is far
 * more text than the reply itself, so only a chunk that has an error word
 * is kept and every chunk is scanned for the prompt on its own. */
static char monitor_reply[512];
static int monitor_refused;

static double now(void);
static double scaled(double seconds);

/**
 * Read from the monitor until its prompt arrives, or `seconds`, scaled as a
 * deadline is, have passed.
 *
 * The prompt is what says the monitor has finished with what it was given.
 * Sleeping a fixed time instead is a race: under load a command sent before
 * the monitor was listening is taken by nobody, and the step that depended on
 * it waits out its whole limit for something that never happened. Returns 0
 * at the prompt, or -1 when the time runs out, the monitor closes because the
 * machine has gone, or a read fails.
 */
static int monitor_wait_prompt(int seconds)
{
    double until = now() + scaled((double)seconds);
    char chunk[512];

    monitor_reply[0] = '\0';
    monitor_refused = 0;
    while (now() <= until) {
        ssize_t n = recv(monitor_fd, chunk, sizeof(chunk) - 1, MSG_DONTWAIT);

        if (n > 0) {
            chunk[n] = '\0';
            if (!monitor_refused &&
                (strstr(chunk, "Error") != NULL || strstr(chunk, "error:") != NULL)) {
                memcpy(monitor_reply, chunk, (size_t)n + 1);
                monitor_refused = 1;
            }
            if (strstr(chunk, "(qemu)") != NULL) {
                return 0;
            }
            continue;
        }
        if (n == 0) {
            return -1;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        usleep(10000);
    }
    return -1;
}

/**
 * Connect to the monitor, once, and read past its greeting.
 *
 * QEMU may not have made the socket yet, so the connection is tried a
 * hundred times, scaled as a deadline is, 50 ms apart. The greeting ends in
 * a prompt, and until that arrives the monitor is not listening.
 */
static int monitor_open(void)
{
    struct sockaddr_un addr;
    int tries;

    if (monitor_fd >= 0) {
        return 0;
    }
    if (strlen(monitor_path) >= sizeof(addr.sun_path)) {
        printf("HARNESS: monitor path too long: %s\n", monitor_path);
        return -1;
    }
    for (tries = 0; tries < (int)scaled(100.0); tries++) {
        monitor_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (monitor_fd < 0) {
            return -1;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, monitor_path, strlen(monitor_path) + 1);
        if (connect(monitor_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }
        close(monitor_fd);
        monitor_fd = -1;
        usleep(50000);
    }
    if (monitor_fd < 0) {
        printf("HARNESS: no monitor at %s\n", monitor_path);
        return -1;
    }
    if (monitor_wait_prompt(60) != 0) {
        printf("HARNESS: the monitor never gave a prompt\n");
        close(monitor_fd);
        monitor_fd = -1;
        return -1;
    }
    return 0;
}

/**
 * Send one monitor command and let it be acted on.
 *
 * The monitor acts on a command and then prompts again. Waiting for that
 * prompt is what makes the step ordered with respect to the machine: the
 * device is plugged in before the next step looks for it. The wait is sixty
 * seconds, because a monitor starved by a loaded host answers late and a
 * command that was acted on late must not be counted as one that was never
 * acted on. A refused command answers with an error line before the prompt;
 * the step that sent it fails with that line, not with a later expect that
 * waits for a device that was never added or removed.
 */
static int monitor_send(const char *command)
{
    char line[256];
    size_t len;

    if (monitor_open() != 0) {
        return -1;
    }
    len = (size_t)snprintf(line, sizeof(line), "%s\n", command);
    if (write(monitor_fd, line, len) != (ssize_t)len) {
        printf("HARNESS: the monitor would not take '%s'\n", command);
        return -1;
    }
    if (monitor_wait_prompt(60) != 0) {
        printf("HARNESS: the monitor did not answer '%s'\n", command);
        return -1;
    }
    if (monitor_refused) {
        printf("HARNESS: the monitor refused '%s': %s\n", command, monitor_reply);
        return -1;
    }
    return 0;
}

/* ---- the serial buffer ---- */

static unsigned char *buf;
static size_t buf_len, buf_cap;
static size_t consumed;
/* Where the last send began, which a `seen=` step searches from. */
static size_t exchange;

static void buf_add(const unsigned char *data, size_t n)
{
    if (buf_len + n + 1 > buf_cap) {
        buf_cap = (buf_len + n + 1) * 2;
        buf = realloc(buf, buf_cap);
        if (buf == NULL) {
            fprintf(stderr, "harness: out of memory\n");
            exit(2);
        }
    }
    memcpy(buf + buf_len, data, n);
    buf_len += n;
    buf[buf_len] = '\0';
}

/* ---- the clock ---- */

/**
 * Seconds on the monotonic clock: a deadline is a duration, and the wall
 * clock can be set while a run goes on.
 */
static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- the deadline factor ----
 *
 * A set's runner measures the emulator's rate under the load the set makes
 * and passes `RACH2_DEADLINES=scaled` with `RACH2_DEADLINE_FACTOR=F` to
 * every suite; a deadline that bounds completion is multiplied by F here.
 * A process told nothing, or told the mode without a factor, keeps its
 * numbers as written, and so does one given a factor outside 1 to 64. */
static double deadline_factor = 1.0;

static void deadline_factor_read(void)
{
    const char *mode = getenv("RACH2_DEADLINES");
    const char *factor = getenv("RACH2_DEADLINE_FACTOR");
    double f;

    if (mode == NULL || strcmp(mode, "scaled") != 0 || factor == NULL)
        return;
    f = atof(factor);
    if (f >= 1.0 && f <= 64.0)
        deadline_factor = f;
}

static double scaled(double seconds)
{
    return seconds * deadline_factor;
}

/* ---- the trace ---- */

static FILE *tracef;

static void trace(const char *fmt, ...)
{
    va_list ap;

    if (tracef == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(tracef, fmt, ap);
    va_end(ap);
    fputc('\n', tracef);
    fflush(tracef);
}

/* ---- a small regular expression matcher ----
 *
 * The patterns these suites use are literal text, with `.` appearing only
 * inside addresses; they are still read as patterns, in a small subset of
 * regular expressions and no more. A literal character matches itself, `.`
 * matches any character but a newline, a `[...]` class matches one of its
 * members (with ranges and a leading `^` to negate), `\` escapes the next
 * character, and `*`, `+` and `?` repeat the item before them. `^` and `$`
 * anchor. Anything else is literal.
 *
 * The matcher backtracks, since a serial log is small and the patterns are
 * short.
 *
 * A pattern using a construct outside this subset (a group, an
 * alternation, a counted repeat) is refused, not silently taken as
 * literal text: a matcher that quietly never matches would turn a
 * passing suite into a timeout nobody could read. No pattern in this tree
 * uses one.
 */

struct item {
    enum { LITERAL, ANY, CLASS } kind;
    char literal;
    unsigned char set[32]; /* a bitmap for CLASS */
    int negate;
    char repeat; /* 0, '*', '+' or '?' */
};

struct regex {
    struct item item[256];
    int n;
    int anchor_start, anchor_end;
};

static void class_add(struct item *it, unsigned char c)
{
    it->set[c >> 3] |= (unsigned char)(1 << (c & 7));
}

static int class_has(const struct item *it, unsigned char c)
{
    int in = (it->set[c >> 3] >> (c & 7)) & 1;

    return it->negate ? !in : in;
}

/**
 * Compile `pattern` into `re`.
 *
 * A pattern with a construct outside the subset, or with more items than
 * `re` holds, ends the run with status 2 and a message that names it.
 */
static int regex_compile(struct regex *re, const char *pattern)
{
    const char *p = pattern;

    memset(re, 0, sizeof(*re));
    for (; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            continue;
        }
        if (strchr("(){}|", *p) != NULL) {
            fprintf(stderr,
                    "harness: the pattern '%s' uses '%c', which "
                    "this matcher does not support; write it as literal "
                    "text or extend the matcher\n",
                    pattern, *p);
            exit(2);
        }
    }
    p = pattern;
    if (*p == '^') {
        re->anchor_start = 1;
        p++;
    }
    while (*p != '\0') {
        struct item *it = &re->item[re->n];

        if (re->n >= (int)(sizeof(re->item) / sizeof(re->item[0]))) {
            fprintf(stderr,
                    "harness: the pattern '%s' has more than %lu "
                    "items; a matcher holding a prefix of it would match "
                    "text the pattern does not\n",
                    pattern, (unsigned long)(sizeof(re->item) / sizeof(re->item[0])));
            exit(2);
        }

        if (*p == '$' && p[1] == '\0') {
            re->anchor_end = 1;
            break;
        }
        memset(it, 0, sizeof(*it));
        if (*p == '\\' && p[1] != '\0') {
            it->kind = LITERAL;
            it->literal = p[1];
            p += 2;
        } else if (*p == '.') {
            it->kind = ANY;
            p++;
        } else if (*p == '[') {
            unsigned char prev = 0;

            it->kind = CLASS;
            p++;
            if (*p == '^') {
                it->negate = 1;
                p++;
            }
            if (*p == ']') {
                class_add(it, (unsigned char)*p);
                prev = (unsigned char)*p;
                p++;
            }
            while (*p != '\0' && *p != ']') {
                if (*p == '-' && p[1] != '\0' && p[1] != ']' && prev != 0) {
                    unsigned char hi = (unsigned char)p[1];
                    unsigned c;

                    for (c = prev; c <= hi; c++)
                        class_add(it, (unsigned char)c);
                    p += 2;
                    prev = 0;
                    continue;
                }
                if (*p == '\\' && p[1] != '\0')
                    p++;
                class_add(it, (unsigned char)*p);
                prev = (unsigned char)*p;
                p++;
            }
            if (*p == ']')
                p++;
        } else {
            it->kind = LITERAL;
            it->literal = *p;
            p++;
        }
        if (*p == '*' || *p == '+' || *p == '?') {
            it->repeat = *p;
            p++;
        }
        re->n++;
    }
    return 1;
}

static int item_matches(const struct item *it, unsigned char c)
{
    switch (it->kind) {
    case LITERAL:
        return (unsigned char)it->literal == c;
    case ANY:
        return c != '\n';
    case CLASS:
        return class_has(it, c);
    }
    return 0;
}

/**
 * Match `re->item[k..]` at `text[at]`; returns the end offset or -1.
 *
 * A `*` or `+` item takes the longest run first and backs off one at a time,
 * which is what a greedy repeat means.
 */
static long match_here(const struct regex *re, int k, const unsigned char *text, size_t len,
                       size_t at)
{
    if (k == re->n)
        return (re->anchor_end && at != len) ? -1 : (long)at;
    {
        const struct item *it = &re->item[k];

        if (it->repeat == '*' || it->repeat == '+') {
            size_t count = 0, i = at;

            while (i < len && item_matches(it, text[i])) {
                i++;
                count++;
            }
            for (;;) {
                long got;

                if (it->repeat == '+' && count == 0)
                    return -1;
                got = match_here(re, k + 1, text, len, at + count);
                if (got >= 0)
                    return got;
                if (count == 0)
                    return -1;
                count--;
            }
        }
        if (it->repeat == '?') {
            if (at < len && item_matches(it, text[at])) {
                long got = match_here(re, k + 1, text, len, at + 1);

                if (got >= 0)
                    return got;
            }
            return match_here(re, k + 1, text, len, at);
        }
        if (at < len && item_matches(it, text[at]))
            return match_here(re, k + 1, text, len, at + 1);
        return -1;
    }
}

/** The end offset of the first match at or after `from`, or -1. */
static long regex_search(const struct regex *re, const unsigned char *text, size_t len, size_t from)
{
    size_t at;

    for (at = from; at <= len; at++) {
        long got = match_here(re, 0, text, len, at);

        if (got >= 0)
            return got;
        if (re->anchor_start)
            break;
    }
    return -1;
}

/* ---- the guest process ---- */

static pid_t child_pid = -1;

/**
 * Stop the machine with the harness, and exit 143.
 *
 * A suite that stops this harness stops its machine with it: the machine is
 * the harness's child, and one left running would hold its disk images and
 * its forwarded port through the rows after.
 */
static void on_term(int sig)
{
    (void)sig;
    if (child_pid > 0)
        kill(child_pid, SIGTERM);
    _exit(143);
}
static int child_out = -1;
static int child_in = -1;
static int child_status;
static int child_reaped;
/* Whether the machine was ended by a signal rather than by exiting. A run
   that requires an exit does not accept one. */
static int child_by_signal;
static FILE *logf;
static double deadline;

/** The machine's exit status, 128 + N after signal N, or -1 while it runs. */
static int child_poll(void)
{
    pid_t got;

    if (child_reaped)
        return child_status;
    got = waitpid(child_pid, &child_status, WNOHANG);
    if (got == child_pid) {
        child_reaped = 1;
        child_by_signal = !WIFEXITED(child_status);
        child_status =
            WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
        return child_status;
    }
    return -1;
}

/**
 * Read what the guest has said into `buf` and the log, until `until` or the
 * deadline.
 *
 * It returns early at the end of the pipe, and when the machine has exited,
 * once what the pipe still holds has been read.
 */
static void pump(double until)
{
    while (now() < until && now() < deadline) {
        fd_set r;
        struct timeval tv;
        unsigned char chunk[BUF_GROW];
        ssize_t n;
        double left = (until < deadline ? until : deadline) - now();

        FD_ZERO(&r);
        FD_SET(child_out, &r);
        tv.tv_sec = 0;
        tv.tv_usec = left < 0.2 ? (long)(left * 1e6) : 200000;
        if (select(child_out + 1, &r, NULL, NULL, &tv) > 0) {
            n = read(child_out, chunk, sizeof(chunk));
            if (n > 0) {
                buf_add(chunk, (size_t)n);
                if (logf != NULL) {
                    fwrite(chunk, 1, (size_t)n, logf);
                    fflush(logf);
                }
            } else if (n == 0) {
                break;
            }
        }
        if (child_poll() >= 0) {
            for (;;) {
                n = read(child_out, chunk, sizeof(chunk));
                if (n <= 0)
                    break;
                buf_add(chunk, (size_t)n);
                if (logf != NULL) {
                    fwrite(chunk, 1, (size_t)n, logf);
                    fflush(logf);
                }
            }
            break;
        }
    }
}

/**
 * Read what the machine's output pipe still holds once the machine has been
 * stopped, to its end, whatever the run's deadline says: output written just
 * before a timeout is searched like any other, a `--never` text among it.
 * The pipe is non-blocking, so a writer that is still alive ends the read
 * rather than holding it.
 */
static void drain_rest(void)
{
    unsigned char chunk[BUF_GROW];
    ssize_t n;

    for (;;) {
        n = read(child_out, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        buf_add(chunk, (size_t)n);
        if (logf != NULL) {
            fwrite(chunk, 1, (size_t)n, logf);
            fflush(logf);
        }
    }
}

/** Read bytes already available without waiting for new output. */
static void drain_ready(void)
{
    unsigned char chunk[BUF_GROW];
    ssize_t n;

    while (now() < deadline) {
        n = read(child_out, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        buf_add(chunk, (size_t)n);
        if (logf != NULL) {
            fwrite(chunk, 1, (size_t)n, logf);
            fflush(logf);
        }
    }
}

/** Start a new exchange before publishing input, preserving fast replies. */
static void begin_input(void)
{
    drain_ready();
    consumed = buf_len;
    exchange = buf_len;
}

static double steptmo = 15.0;

/* The stamp file: `MATCH N SECONDS` for each `expect=` matched and
   `SEND N SECONDS` for each `send=`, N the step's number and SECONDS the
   host's monotonic clock, for the runner's calibration. */
static FILE *stampf;
static int step_number;

static void stamp(const char *what)
{
    if (stampf != NULL) {
        fprintf(stampf, "%s %d %.3f\n", what, step_number, now());
        fflush(stampf);
    }
}

/**
 * Wait until `pattern` appears in the guest's output or `stop` passes.
 *
 * A child that has exited can still have output in the pipe, since reaping it
 * reads nothing; that output is drained before the last search.
 */
static int expect_pattern(const char *pattern, double stop)
{
    struct regex re;

    regex_compile(&re, pattern);
    if (stop > deadline)
        stop = deadline;
    while (now() < stop && child_poll() < 0) {
        long end = regex_search(&re, buf, buf_len, consumed);

        if (end >= 0) {
            consumed = (size_t)end;
            return 1;
        }
        pump(now() + 0.3);
    }
    if (child_poll() >= 0)
        drain_ready();
    {
        long end = regex_search(&re, buf, buf_len, consumed);

        if (end >= 0) {
            consumed = (size_t)end;
            return 1;
        }
    }
    return 0;
}

/** Whether `step` waits for output: an `expect=` or the probe's `boot=`. */
static int is_expect_step(const char *step)
{
    return strncmp(step, "expect=", 7) == 0 || strncmp(step, "boot=", 5) == 0;
}

/**
 * Wait for an `expect=` step under the step's limit, or for a `boot=` step
 * under the run's deadline, since a boot may take longer than a command when
 * many emulators run at once.
 */
static int expect_step(const char *step)
{
    if (strncmp(step, "boot=", 5) == 0)
        return expect_pattern(step + 5, deadline);
    return expect_pattern(step + 7, now() + steptmo);
}

/**
 * Wait until `pattern` has appeared since the last send, wherever it lies:
 * two `seen=` steps are satisfied in either order, where two `expect=`
 * steps are satisfied in theirs. The cursor moves past the match only
 * when the match is beyond it, so a later `expect=` still searches on.
 */
static int seen_pattern(const char *pattern)
{
    struct regex re;
    double stop;

    regex_compile(&re, pattern);
    stop = now() + steptmo;
    if (stop > deadline)
        stop = deadline;
    for (;;) {
        long end = regex_search(&re, buf, buf_len, exchange);

        if (end >= 0) {
            if ((size_t)end > consumed)
                consumed = (size_t)end;
            return 1;
        }
        if (now() >= stop || child_poll() >= 0)
            break;
        pump(now() + 0.3);
    }
    return 0;
}

/** The value of the hexadecimal digit `c`, or -1. */
static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * Keep the machine's output pipe and input pipe, both non-blocking.
 *
 * `out_read` becomes `child_out` and `in_write` becomes `child_in`. The input
 * side is what `send_bytes` depends on: a write to a machine that has stopped
 * reading its serial line fills the pipe, and a blocking write would then
 * wait for a reader past the run's deadline. Returns 0, or -1 when either
 * could not be made non-blocking.
 */
static int serial_ends(int out_read, int in_write)
{
    child_out = out_read;
    child_in = in_write;
    if (fcntl(child_out, F_SETFL, O_NONBLOCK) < 0 || fcntl(child_in, F_SETFL, O_NONBLOCK) < 0)
        return -1;
    return 0;
}

/**
 * Write `n` bytes whole, across short writes, and give up at the deadline.
 *
 * The guest's output is read between the writes, so a guest that answers
 * while it takes input is not blocked on a full pipe. The pipe is
 * non-blocking (`serial_ends` makes it so), so a guest that has stopped
 * taking input does not hold the harness past its deadline. Returns 1, or 0
 * when the pipe is gone or the bytes could not be sent in time.
 */
static int send_bytes(const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w;

        if (now() >= deadline)
            return 0;
        w = write(child_in, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (now() >= deadline || child_poll() >= 0)
                return 0;
            pump(now() + 0.05);
            continue;
        }
        if (w <= 0)
            return 0;
        p += w;
        n -= (size_t)w;
    }
    return 1;
}

/**
 * Start a new exchange and write `text`, then a newline when `newline` is set.
 *
 * Returns 1 when every byte reached the pipe and 0 when the guest is gone or
 * stopped reading until the run's deadline; a step counts that as a failure,
 * since a line the guest never got cannot have been answered.
 */
static int send_text(const char *text, int newline)
{
    if (child_in < 0)
        return 0;
    begin_input();
    if (!send_bytes(text, strlen(text)))
        return 0;
    return !newline || send_bytes("\n", 1);
}

static int send_line(const char *text)
{
    return send_text(text, 1);
}

/**
 * Decode `\\`, `\n`, `\r`, `\t` and `\xNN` into the bytes they name, so a
 * step can deliver a control byte the shell would not carry.
 *
 * Only `sendraw=` is decoded: `send=` writes its text as typed, so a
 * backslash in a `send=` step is a backslash. An escape this does not know
 * is an error rather than the letter after the backslash: a step that meant
 * a byte and got a letter sends the wrong input and reads as a kernel fault.
 * A text longer than `cap` is refused rather than cut. Returns the length,
 * or -1.
 */
static int unescape(const char *in, char *out, size_t cap)
{
    size_t n = 0;

    while (*in != '\0') {
        int c = (unsigned char)*in++;

        if (c == '\\') {
            int h, l;

            switch (*in) {
            case '\\':
                c = '\\';
                in++;
                break;
            case 'n':
                c = '\n';
                in++;
                break;
            case 'r':
                c = '\r';
                in++;
                break;
            case 't':
                c = '\t';
                in++;
                break;
            case 'x':
                h = hexval(in[1]);
                l = h < 0 ? -1 : hexval(in[2]);
                if (l < 0) {
                    fprintf(stderr, "harness: \\x must be followed by two "
                                    "hexadecimal digits\n");
                    return -1;
                }
                c = h * 16 + l;
                in += 3;
                break;
            default:
                fprintf(stderr, "harness: unknown escape \\%c\n", *in);
                return -1;
            }
        }
        if (n + 1 > cap)
            return -1;
        out[n++] = (char)c;
    }
    return (int)n;
}

/* ---- main ---- */

/** End of the first literal token at or after `from`, or -1. */
static long token_end(const unsigned char *hay, size_t len, const char *needle, size_t from)
{
    size_t n = strlen(needle), i;

    if (n == 0 || n > len)
        return -1;
    for (i = from; i <= len - n; i++)
        if (memcmp(hay + i, needle, n) == 0)
            return (long)(i + n);
    return -1;
}

static int contains(const unsigned char *hay, size_t len, const char *needle)
{
    return token_end(hay, len, needle, 0) >= 0;
}

/** Split a probe into its boot wait, under the run's timeout, and its exec. */
static int expand_probe(const char *arg, const char **steps, char *execq, size_t size)
{
    if ((size_t)snprintf(execq, size, "execq=%s", arg + 6) >= size) {
        fprintf(stderr,
                "harness: the probe name in %s is "
                "longer than %lu bytes; a shortened name runs "
                "a different program\n",
                arg, (unsigned long)size - 7);
        return 0;
    }
    steps[0] = "boot=Type 'help'";
    steps[1] = execq;
    return 1;
}

/**
 * Use a pipe as the serial source because the harness has no fake clock.
 * The prompt arrives after the step cap; a short run must still reject it.
 * The exited case reaps before reading, so buffered output cannot be missed.
 */
static int prompt_wait_selftest(const char *step, double timeout, int want, int exited)
{
    int serial[2], got;

    if (pipe(serial) != 0) {
        perror("harness: selftest pipe");
        return 1;
    }
    child_pid = fork();
    if (child_pid < 0) {
        perror("harness: selftest fork");
        close(serial[0]);
        close(serial[1]);
        return 1;
    }
    if (child_pid == 0) {
        const char prompt[] = "Type 'help'";
        struct timespec delay = {0, 0};

        delay.tv_sec = exited ? 0 : 1;
        close(serial[0]);
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
        _exit(write(serial[1], prompt, sizeof(prompt) - 1) == (ssize_t)sizeof(prompt) - 1 ? 0 : 1);
    }
    close(serial[1]);
    child_out = serial[0];
    if (fcntl(child_out, F_SETFL, O_NONBLOCK) < 0) {
        perror("harness: selftest nonblocking pipe");
        got = -1;
    } else {
        child_reaped = 0;
        buf_len = consumed = 0;
        steptmo = 0.05;
        deadline = now() + timeout;
        if (exited) {
            while (waitpid(child_pid, &child_status, 0) < 0 && errno == EINTR)
                ;
            child_reaped = 1;
            child_status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 1;
        }
        got = expect_step(step);
    }
    if (!child_reaped) {
        kill(child_pid, SIGKILL);
        while (waitpid(child_pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close(child_out);
    child_out = -1;
    child_pid = -1;
    child_reaped = 0;
    if (got != want) {
        fprintf(stderr, "harness: selftest: %s with timeout %.2f returned %d, wanted %d\n", step,
                timeout, got, want);
        return 1;
    }
    return 0;
}

/** Test the actual probe expansion so restoring its old expect step fails. */
static int probe_wait_selftest(void)
{
    const char *steps[2];
    char execq[256];
    int bad;

    if (!expand_probe("probe=/selftest", steps, execq, sizeof(execq)))
        return 1;
    bad = prompt_wait_selftest(steps[0], 10.0, 1, 0);
    bad += prompt_wait_selftest("expect=Type 'help'", 10.0, 0, 0);
    bad += prompt_wait_selftest(steps[0], 0.05, 0, 0);
    bad += prompt_wait_selftest(steps[0], 10.0, 1, 1);
    if (strcmp(steps[1], "execq=/selftest") != 0) {
        fprintf(stderr, "harness: selftest: probe command was misread\n");
        bad++;
    }
    free(buf);
    buf = NULL;
    buf_len = buf_cap = consumed = 0;
    if (!bad)
        printf("harness: selftest: probe wait bounds pass\n");
    return bad != 0;
}

/**
 * Require a write to a machine that has stopped reading to end at the run's
 * deadline.
 *
 * A child process takes the pipe ends through `serial_ends`, as a run does,
 * and sends a megabyte with the deadline a fifth of a second away; nothing
 * reads the input pipe, so it fills. The child must give up and exit within
 * five seconds. A blocking input end would hold its write until a reader
 * came, and the child is killed then and the case fails.
 */
static int input_deadline_selftest(void)
{
    struct timespec nap = {0, 50000000};
    int in[2], out[2], status = 0, killed = 0;
    pid_t pid, got = 0;
    double start;

    if (pipe(in) != 0) {
        perror("harness: selftest pipe");
        return 1;
    }
    if (pipe(out) != 0) {
        perror("harness: selftest pipe");
        close(in[0]);
        close(in[1]);
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        perror("harness: selftest fork");
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        return 1;
    }
    if (pid == 0) {
        static char flood[1 << 20];

        child_pid = getppid();
        child_reaped = 0;
        deadline = now() + 0.2;
        if (serial_ends(out[0], in[1]) != 0)
            _exit(2);
        _exit(send_bytes(flood, sizeof(flood)) == 0 ? 0 : 1);
    }
    close(in[1]);
    close(out[1]);
    start = now();
    for (;;) {
        got = waitpid(pid, &status, WNOHANG);
        if (got == pid || (got < 0 && errno != EINTR) || now() - start >= 5.0)
            break;
        nanosleep(&nap, NULL);
    }
    if (got != pid) {
        killed = 1;
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close(in[0]);
    close(out[0]);
    if (killed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "harness: selftest: a write the machine does not read "
                        "was not ended by the deadline\n");
        return 1;
    }
    printf("harness: selftest: a write the machine does not read ends at the deadline\n");
    return 0;
}

/**
 * Require output the machine wrote before a timeout to be read after it.
 *
 * The pipe holds a line and its writer is gone, and the run's deadline has
 * passed. `pump` reads nothing then, since it checks the deadline before it
 * reads; `drain_rest` must read the line all the same, as the judgement of a
 * run that timed out needs the `--never` texts written just before the end.
 */
static int late_output_selftest(void)
{
    static const char line[] = "late output before the deadline\n";
    int out[2];
    int saved_out = child_out;
    double saved_deadline = deadline;
    size_t start = buf_len;
    int found;

    if (pipe(out) != 0) {
        perror("harness: selftest pipe");
        return 1;
    }
    if (write(out[1], line, sizeof(line) - 1) != (ssize_t)(sizeof(line) - 1)) {
        perror("harness: selftest write");
        close(out[0]);
        close(out[1]);
        return 1;
    }
    close(out[1]);
    child_out = out[0];
    deadline = now() - 1.0;
    pump(now() + 1.0);
    drain_rest();
    found =
        buf != NULL && strstr((const char *)buf + start, "late output before the deadline") != NULL;
    close(out[0]);
    child_out = saved_out;
    deadline = saved_deadline;
    if (!found) {
        fprintf(stderr, "harness: selftest: output written before a timeout was not "
                        "read after it\n");
        return 1;
    }
    printf("harness: selftest: output written before a timeout is read after it\n");
    return 0;
}

/**
 * Check the escape decoder and the mouse step's parsing.
 *
 * The escapes are the cases the suites depend on and the ones they do not: a
 * decoder that quietly passed a bad escape through would send the letter
 * after the backslash, and a probe reading one byte would be given the wrong
 * one and read as a kernel fault. A text longer than the buffer must be
 * refused rather than cut, since a step that sent half of what it was given
 * would be checked against the whole. A mouse step whose numbers were misread
 * would move the mouse somewhere else, and the check that follows would fail
 * for a reason nobody could see.
 */
static int unescape_selftest(void)
{
    static const struct {
        const char *in;
        const char *want; /* NULL for "must be refused" */
        int len;
    } cases[] = {{"abc", "abc", 3},
                 {"a\\x7fb",
                  "a\x7f"
                  "b",
                  3},
                 {"\\x00", "\0", 1},
                 {"\\xff", "\xff", 1},
                 {"\\n", "\n", 1},
                 {"\\r\\t", "\r\t", 2},
                 {"\\\\", "\\", 1},
                 {"a\\x4A", "aJ", 2},
                 {"\\x", NULL, 0},
                 {"\\x4", NULL, 0},
                 {"\\xg0", NULL, 0},
                 {"\\q", NULL, 0}};
    int n = (int)(sizeof(cases) / sizeof(cases[0])), i, bad = 0;

    printf("harness: selftest: the refused cases print their own reason\n");
    for (i = 0; i < n; i++) {
        char out[64];
        int got = unescape(cases[i].in, out, sizeof(out));

        if (cases[i].want == NULL) {
            if (got >= 0) {
                printf("harness: selftest: '%s' was accepted and must not "
                       "be\n",
                       cases[i].in);
                bad++;
            }
            continue;
        }
        if (got != cases[i].len || memcmp(out, cases[i].want, (size_t)cases[i].len) != 0) {
            printf("harness: selftest: '%s' decoded to %d byte(s), wanted "
                   "%d\n",
                   cases[i].in, got, cases[i].len);
            bad++;
        }
    }
    {
        char out[4];

        if (unescape("abcdef", out, sizeof(out)) >= 0) {
            printf("harness: selftest: a text past the buffer was accepted\n");
            bad++;
        }
    }

    {
        int dx = 0;
        int dy = 0;
        int buttons = 0;

        if (parse_mouse("5,-7,1", &dx, &dy, &buttons) != 0 || dx != 5 || dy != -7 || buttons != 1) {
            printf("harness: selftest: a mouse step was misread\n");
            bad++;
        }
        if (parse_mouse("5,-7", &dx, &dy, &buttons) == 0) {
            printf("harness: selftest: a mouse step with two numbers was taken\n");
            bad++;
        }
        if (parse_mouse("5,-7,9", &dx, &dy, &buttons) == 0) {
            printf("harness: selftest: a mouse step with a ninth button was taken\n");
            bad++;
        }
        if (parse_mouse("", &dx, &dy, &buttons) == 0) {
            printf("harness: selftest: an empty mouse step was taken\n");
            bad++;
        }
    }
    if (bad != 0) {
        printf("harness: selftest: %d case(s) failed\n", bad);
        return 1;
    }
    printf("harness: selftest: %d escape cases and the mouse step PASS\n", n + 1);
    return 0;
}

/**
 * `--match PATTERN`: print the end offset of the first match in the text on
 * standard input, or -1, so the matcher can be checked by hand.
 */
static int match_mode(const char *pattern)
{
    struct regex re;
    unsigned char text[1 << 16];
    size_t n = fread(text, 1, sizeof(text), stdin);

    regex_compile(&re, pattern);
    printf("%ld\n", regex_search(&re, text, n, 0));
    return 0;
}

/**
 * Refuse a run that asks for more than the harness holds, with status 2.
 *
 * The run is stopped, not trimmed: a dropped step is a check that never ran
 * under a report that says the run passed.
 */
static int too_many(const char *what, int limit)
{
    fprintf(stderr,
            "harness: more than %d %s arguments; this harness "
            "would run only the first %d\n",
            limit, what, limit);
    return 2;
}

/**
 * The `tcp=PORT` step: connect to the port on the loopback, which a
 * `hostfwd` forwards into the guest, and read the first bytes its server
 * sends. QEMU's user network accepts the host's side of a connection
 * before the guest's side exists, so a connection proves nothing; bytes
 * from the guest, an SSH server's banner, prove its server answered. A
 * server slow to listen under load, or one whose connection the guest
 * resets, is connected to again until thirty seconds, scaled as a deadline
 * is, have passed, and never past the run's deadline. Returns 1 when the
 * guest answered, 0 when nothing did.
 */
static int step_tcp(int port)
{
    int sk = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    struct timeval tv;
    double knock_until;
    int connected, answered = 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    knock_until = now() + scaled(30.0);
    if (knock_until > deadline)
        knock_until = deadline;
    connected = sk >= 0 && connect(sk, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    while (!connected && now() < knock_until && child_poll() < 0) {
        if (sk >= 0)
            close(sk);
        pump(now() + 1.0);
        if (child_poll() >= 0)
            break;
        sk = socket(AF_INET, SOCK_STREAM, 0);
        if (sk >= 0)
            setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        connected = sk >= 0 && connect(sk, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    }
    while (connected && now() < knock_until && child_poll() < 0) {
        char answer[64];
        ssize_t got;
        struct timeval rv;

        rv.tv_sec = 1;
        rv.tv_usec = 0;
        setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof(rv));
        got = recv(sk, answer, sizeof(answer), 0);
        if (got > 0) {
            answered = 1;
            break;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pump(now() + 0.1);
            continue;
        }
        close(sk);
        pump(now() + 1.0);
        sk = socket(AF_INET, SOCK_STREAM, 0);
        if (sk >= 0)
            setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        connected = sk >= 0 && connect(sk, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    }
    if (answered) {
        printf("HARNESS: port %d answered\n", port);
    } else {
        printf("HARNESS: nothing answered on port %d (%s)\n", port, strerror(errno));
    }
    if (sk >= 0)
        close(sk);
    return answered;
}

/**
 * The `execq=NAME` step: type `exec NAME` at the shell's prompt, which the
 * step before it has waited for, and stop as soon as `harness_exit` stops
 * QEMU, under the step's limit. The line is typed again when the shell has
 * printed nothing past its echo of it for three seconds, which is the
 * shell dropping the line, or when it answered that it does not know the
 * command, which is a line typed before the shell was reading and missing
 * its first bytes; a probe that is running is not typed at twice, since
 * the shell would run it again when it ends. The echo is one line ending,
 * and a second is the program's output or the shell's answer. Under `ok`,
 * the run's token, the step ends when the token has arrived since the line
 * was typed: a machine that stays at its prompt afterwards is not waited for
 * to the limit, which a set's factor can put past the runner's idle limit. A
 * `--never` text ends the step too, with or without `ok`, since the run fails
 * on it afterwards and a probe that failed at once is not waited for to the
 * step's limit.
 */
static void step_execq(const char *name, const char *ok, const char *const *nevers, int n_never)
{
    double step_end = now() + steptmo;
    double sent = 0;
    size_t mark = 0;
    char line[512];

    snprintf(line, sizeof(line), "exec %s", name);
    while (child_poll() < 0 && now() < step_end && now() < deadline) {
        int lines = 0, refused;
        size_t i;

        for (i = mark; i < buf_len; i++) {
            if (buf[i] == '\n')
                lines++;
        }
        refused = contains(buf + mark, buf_len - mark, "Unknown command");
        if (sent == 0 || refused || (lines < 2 && now() - sent >= 3.0)) {
            (void)send_line(line);
            sent = now();
            mark = buf_len;
        }
        pump(now() + 0.5);
        if (ok != NULL && contains(buf + mark, buf_len - mark, ok))
            break;
        for (i = 0; i < (size_t)n_never; i++)
            if (contains(buf + mark, buf_len - mark, nevers[i]))
                return;
    }
}

/**
 * Run one machine through its steps and decide the run.
 *
 * An empty `--never` text is refused: it is in every stream and in none,
 * depending on how it is read, and a check that cannot fail is worse than
 * none. QEMU starts with -no-reboot, so a triple fault ends it, which is read
 * as a failure, instead of looping; not with -no-shutdown, which would keep
 * the isa-debug-exit device from stopping the machine. The monitor socket is
 * made only when a step needs it.
 *
 * Once the machine has gone, a step that waits for output is still satisfied
 * by what the machine printed on its way out, read from the pipe first: a
 * probe's last lines and the exit its kernel makes for it arrive together,
 * and the output is what the check is about. An `until=` step is satisfied
 * by a machine that has left. Every other step left is counted as failed and
 * named, since a run that stopped early and reported a pass over the steps it
 * never reached is the report this harness exists to prevent.
 *
 * After the last step the output is read for the grace period, and then the
 * machine's exit is waited for, since `pump` returns when the pipe reaches its
 * end, before the machine that closed it has finished going away. Under
 * --must-exit the wait goes to the deadline, which is what the caller asked
 * to wait; otherwise only for the grace period, so a machine that is meant to
 * stay up does not cost the whole timeout. A machine still running then is
 * stopped.
 *
 * The run is then decided in this order. A --never text fails it, whatever
 * else happened: a probe whose claim is that the kernel killed it prints one
 * line when it was not killed, and its own status cannot report that. Under
 * --must-exit a machine that did not end itself by exiting fails it, which is
 * what tells a machine that restarted or halted from one that printed the
 * right line and then sat there. With --ok, which is for a machine that
 * never stops QEMU itself, the token and no failed step pass it, unless the
 * machine stopped on its own with an even status: codes from isa-debug-exit
 * are odd, so QEMU ended by itself and the token alone does not pass. Without
 * --ok a QEMU that never exited is status 3, and otherwise the result is the
 * kernel's `process: exited with status N` line, not QEMU's exit code. The
 * host keeps 8 bits of that code, so ((status << 1) | 1) aliases (status 128
 * gives 1) and a QEMU launch error also gives 1; the serial line is the whole
 * status and also proves the guest ran, and the code only confirms that QEMU
 * stopped through isa-debug-exit. The status must be
 * digits, since text that is not a number would read as status 0, and a
 * number too large for a long is reported rather than read as the prefix
 * that fit. No status line, or a stop that was not through isa-debug-exit,
 * fails the run, and so does a failed step even when the guest then exited
 * cleanly.
 */
int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL, *append = NULL;
    int bios = 0;
    const char *net = "user,id=net0", *device = "rtl8139,netdev=net0";
    const char *log = NULL, *ok = NULL;
    const char *nevers[MAX_ARGS];
    int n_never = 0;
    const char *drives[MAX_ARGS];
    const char *qemu_args[MAX_ARGS];
    const char *steps[MAX_STEPS];
    int n_drives = 0, n_qemu = 0, n_steps = 0;
    double timeout = 60.0, grace = 3.0;
    int no_debug_exit = 0, must_exit = 0;
    const char *stamp_path = NULL;
    /* Every --drive costs two words, so the command line is wider than
       the option arrays it is built from. */
    char *qemu[MAX_ARGS * 4];
    int nq = 0, i;
    int out_pipe[2], in_pipe[2];
    int step_failures = 0;
    int qemu_rc;
    int self_exited = 0;
    const char *trace_path;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return unescape_selftest() | probe_wait_selftest() | input_deadline_selftest() |
               late_output_selftest();
    if (argc == 3 && strcmp(argv[1], "--match") == 0)
        return match_mode(argv[2]);

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

#define OPT(name, target)                         \
    if (strcmp(arg, name) == 0 && i + 1 < argc) { \
        target = argv[++i];                       \
        continue;                                 \
    }
        OPT("--kernel", kernel)
        OPT("--initrd", initrd)
        OPT("--append", append)
        OPT("--net", net)
        OPT("--device", device)
        OPT("--log", log)
        OPT("--ok", ok)
        OPT("--stamp", stamp_path)
#undef OPT
        if (strcmp(arg, "--never") == 0 && i + 1 < argc) {
            if (n_never >= MAX_ARGS)
                return too_many("--never", MAX_ARGS);
            if (argv[i + 1][0] == '\0') {
                fprintf(stderr, "harness: --never needs a text\n");
                return 2;
            }
            nevers[n_never++] = argv[++i];
        } else if (strcmp(arg, "--drive") == 0 && i + 1 < argc) {
            if (n_drives >= MAX_ARGS)
                return too_many("--drive", MAX_ARGS);
            drives[n_drives++] = argv[++i];
        } else if (strncmp(arg, "--qemu-arg=", 11) == 0) {
            if (n_qemu >= MAX_ARGS)
                return too_many("--qemu-arg", MAX_ARGS);
            qemu_args[n_qemu++] = arg + 11;
        } else if (strcmp(arg, "--qemu-arg") == 0 && i + 1 < argc) {
            if (n_qemu >= MAX_ARGS)
                return too_many("--qemu-arg", MAX_ARGS);
            qemu_args[n_qemu++] = argv[++i];
        } else if (strcmp(arg, "--timeout") == 0 && i + 1 < argc) {
            timeout = atof(argv[++i]);
        } else if (strcmp(arg, "--steptmo") == 0 && i + 1 < argc) {
            steptmo = atof(argv[++i]);
        } else if (strcmp(arg, "--grace") == 0 && i + 1 < argc) {
            grace = atof(argv[++i]);
        } else if (strcmp(arg, "--bootwait") == 0 && i + 1 < argc) {
            i++; /* accepted and ignored */
        } else if (strcmp(arg, "--must-exit") == 0) {
            must_exit = 1;
        } else if (strcmp(arg, "--bios") == 0) {
            bios = 1;
        } else if (strcmp(arg, "--no-debug-exit") == 0) {
            no_debug_exit = 1;
        } else if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "harness: unknown option %s\n", arg);
            return 2;
        } else {
            if (strncmp(arg, "probe=", 6) == 0) {
                static char execq[MAX_STEPS][256];
                static int n_execq;

                if (n_steps + 2 > MAX_STEPS)
                    return too_many("step", MAX_STEPS);
                if (!expand_probe(arg, steps + n_steps, execq[n_execq], sizeof(execq[0])))
                    return 2;
                n_steps += 2;
                n_execq++;
            } else {
                if (n_steps >= MAX_STEPS)
                    return too_many("step", MAX_STEPS);
                steps[n_steps++] = arg;
            }
        }
    }
    if (kernel == NULL && !bios) {
        fprintf(stderr, "harness: --kernel is required, or --bios to boot from the drives\n");
        return 2;
    }

    if (steps_need_monitor(argc, argv)) {
        snprintf(monitor_path, sizeof(monitor_path), "/tmp/rach2-monitor-%d", (int)getpid());
        unlink(monitor_path);
    }

    qemu[nq++] = (char *)"qemu-system-i386";
    if (!bios) {
        qemu[nq++] = (char *)"-kernel";
        qemu[nq++] = (char *)kernel;
    }
    qemu[nq++] = (char *)"-serial";
    qemu[nq++] = (char *)"stdio";
    qemu[nq++] = (char *)"-display";
    qemu[nq++] = (char *)"none";
    qemu[nq++] = (char *)"-no-reboot";
    if (monitor_path[0] != '\0') {
        static char monitor_arg[192];

        snprintf(monitor_arg, sizeof(monitor_arg), "unix:%s,server,nowait", monitor_path);
        qemu[nq++] = (char *)"-monitor";
        qemu[nq++] = monitor_arg;
    }
    qemu[nq++] = (char *)"-netdev";
    qemu[nq++] = (char *)net;
    qemu[nq++] = (char *)"-device";
    qemu[nq++] = (char *)device;
    if (initrd != NULL && !bios) {
        qemu[nq++] = (char *)"-initrd";
        qemu[nq++] = (char *)initrd;
    }
    if (append != NULL && !bios) {
        qemu[nq++] = (char *)"-append";
        qemu[nq++] = (char *)append;
    }
    for (i = 0; i < n_drives; i++) {
        qemu[nq++] = (char *)"-drive";
        qemu[nq++] = (char *)drives[i];
    }
    for (i = 0; i < n_qemu; i++)
        qemu[nq++] = (char *)qemu_args[i];
    if (!no_debug_exit) {
        qemu[nq++] = (char *)"-device";
        qemu[nq++] = (char *)"isa-debug-exit,iobase=0xf4,iosize=0x04";
    }
    qemu[nq] = NULL;

    if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
        fprintf(stderr, "harness: cannot make a pipe\n");
        return 2;
    }
    child_pid = fork();
    if (child_pid < 0) {
        fprintf(stderr, "qemu launch failed: %s\n", strerror(errno));
        return 2;
    }
    if (child_pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        close(in_pipe[1]);
        close(out_pipe[0]);
        execvp(qemu[0], qemu);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (serial_ends(out_pipe[0], in_pipe[1]) != 0)
        fprintf(stderr, "HARNESS: the serial pipes stay blocking: %s\n", strerror(errno));
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);

    trace_path = getenv("RACH2_TRACE");
    if (trace_path != NULL)
        tracef = fopen(trace_path, "w");
    if (log != NULL)
        logf = fopen(log, "wb");
    if (stamp_path != NULL) {
        stampf = fopen(stamp_path, "a");
        if (stampf == NULL) {
            fprintf(stderr, "HARNESS: cannot write the stamp file %s: %s\n", stamp_path,
                    strerror(errno));
            return 2;
        }
    }
    deadline_factor_read();
    timeout = scaled(timeout);
    steptmo = scaled(steptmo);
    deadline = now() + timeout;
    buf_add((const unsigned char *)"", 0);

    for (i = 0; i < n_steps; i++) {
        const char *step = steps[i];

        step_number = i + 1;
        if (child_poll() >= 0 && strncmp(step, "until=", 6) == 0)
            continue;
        if (child_poll() >= 0 && (is_expect_step(step) || strncmp(step, "seen=", 5) == 0)) {
            struct regex re;
            long end;
            int seen = step[0] == 's';

            pump(now() + 0.05);
            regex_compile(&re, step + (step[0] == 'e' ? 7 : 5));
            end = regex_search(&re, buf, buf_len, seen ? exchange : consumed);
            if (end >= 0) {
                if (!seen || (size_t)end > consumed)
                    consumed = (size_t)end;
                continue;
            }
        }
        if (child_poll() >= 0) {
            int left;

            for (left = i; left < n_steps; left++) {
                fprintf(stderr,
                        "HARNESS: step '%s' never ran: qemu had "
                        "already exited\n",
                        steps[left]);
                step_failures++;
            }
            break;
        }
        if (is_expect_step(step)) {
            int got = expect_step(step);

            if (!got)
                step_failures++;
            else
                stamp("MATCH");
            trace("step %s %s consumed=%lu", step, got ? "ok" : "fail", (unsigned long)consumed);
        } else if (strncmp(step, "seen=", 5) == 0) {
            int got = seen_pattern(step + 5);

            if (!got)
                step_failures++;
            else
                stamp("MATCH");
            trace("step %s %s consumed=%lu", step, got ? "ok" : "fail", (unsigned long)consumed);
        } else if (strncmp(step, "send=", 5) == 0) {
            stamp("SEND");
            if (!send_line(step + 5)) {
                fprintf(stderr, "HARNESS: step '%s' did not reach the guest\n", step);
                step_failures++;
            }
        } else if (strncmp(step, "sendraw=", 8) == 0) {
            char raw[256];
            int n = unescape(step + 8, raw, sizeof(raw));

            begin_input();
            if (n < 0 || child_in < 0 || !send_bytes(raw, (size_t)n)) {
                fprintf(stderr,
                        "HARNESS: step '%s' did not reach the "
                        "guest\n",
                        step);
                step_failures++;
            }
        } else if (strncmp(step, "sendfile=", 9) == 0) {
            FILE *sf = fopen(step + 9, "rb");
            char chunk[4096];
            size_t got;
            int sent = sf != NULL;

            begin_input();
            while (sent && (got = fread(chunk, 1, sizeof(chunk), sf)) > 0) {
                if (child_in < 0 || !send_bytes(chunk, got))
                    sent = 0;
            }
            if (sf)
                fclose(sf);
            if (!sent) {
                fprintf(stderr, "HARNESS: step '%s' did not reach the guest\n", step);
                step_failures++;
            }
        } else if (strncmp(step, "key=", 4) == 0) {
            char command[128];

            snprintf(command, sizeof(command), "sendkey %s", step + 4);
            if (monitor_send(command) != 0) {
                step_failures++;
            }
        } else if (strncmp(step, "mouse=", 6) == 0) {
            char command[128];
            int dx = 0;
            int dy = 0;
            int buttons = 0;

            if (parse_mouse(step + 6, &dx, &dy, &buttons) != 0) {
                fprintf(stderr, "HARNESS: mouse= wants DX,DY,BUTTONS\n");
                step_failures++;
                continue;
            }
            snprintf(command, sizeof(command), "mouse_move %d %d", dx, dy);
            if (monitor_send(command) != 0) {
                step_failures++;
            }
            if (buttons != 0) {
                snprintf(command, sizeof(command), "mouse_button %d", buttons);
                if (monitor_send(command) != 0) {
                    step_failures++;
                }
            }
        } else if (strncmp(step, "usb=add:", 8) == 0) {
            char command[192];

            snprintf(command, sizeof(command), "device_add %s", step + 8);
            if (monitor_send(command) != 0) {
                step_failures++;
            }
        } else if (strncmp(step, "usb=del:", 8) == 0) {
            char command[192];

            snprintf(command, sizeof(command), "device_del %s", step + 8);
            if (monitor_send(command) != 0) {
                step_failures++;
            }
        } else if (strncmp(step, "drive=add:", 10) == 0) {
            char command[192];

            snprintf(command, sizeof(command), "drive_add 0 %s", step + 10);
            if (monitor_send(command) != 0) {
                step_failures++;
            }
        } else if (strncmp(step, "drive=del:", 10) == 0) {
            char command[192];

            snprintf(command, sizeof(command), "drive_del %s", step + 10);
            if (monitor_send(command) != 0) {
                step_failures++;
            }
        } else if (strncmp(step, "barrier=", 8) == 0) {
            while (now() < deadline && child_poll() < 0 && access(step + 8, F_OK) != 0)
                pump(now() + 0.05);
            if (access(step + 8, F_OK) != 0)
                step_failures++;
        } else if (strncmp(step, "until=", 6) == 0) {
            double until = now() + scaled(atof(step + 6));
            size_t from = consumed;

            drain_ready();
            if (until > deadline)
                until = deadline;
            while (now() < until && child_poll() < 0) {
                pump(now() + 0.3);
                long end = ok ? token_end(buf, buf_len, ok, from) : -1;

                if (end >= 0) {
                    consumed = (size_t)end;
                    pump(now() + 0.5);
                    break;
                }
            }
        } else if (strncmp(step, "tcp=", 4) == 0) {
            if (!step_tcp(atoi(step + 4)))
                step_failures++;
        } else if (strncmp(step, "udp=", 4) == 0) {
            const char *colon = strchr(step + 4, ':');
            int port = atoi(step + 4);
            const char *text = colon != NULL ? colon + 1 : "";
            int sk = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in addr;

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons((unsigned short)port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (sk < 0) {
                printf("HARNESS: no socket for port %d (%s)\n", port, strerror(errno));
                step_failures++;
            } else {
                if (sendto(sk, text, strlen(text), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                    printf("HARNESS: nothing sent to port %d (%s)\n", port, strerror(errno));
                    step_failures++;
                }
                close(sk);
            }
        } else if (strncmp(step, "exec=", 5) == 0) {
            const char *bar = strchr(step + 5, '|');
            char name[256], marker[256];
            int tries = 0, seen = 0, cut;

            if (bar != NULL) {
                cut = snprintf(name, sizeof(name), "%.*s", (int)(bar - step - 5), step + 5) >=
                      (int)sizeof(name);
                cut |= snprintf(marker, sizeof(marker), "%s", bar + 1) >= (int)sizeof(marker);
            } else {
                cut = snprintf(name, sizeof(name), "%s", step + 5) >= (int)sizeof(name);
                marker[0] = '\0';
            }
            if (cut) {
                fprintf(stderr,
                        "harness: the step '%s' is longer than "
                        "this harness holds; a shortened name runs a "
                        "different program\n",
                        step);
                step_failures++;
                continue;
            }
            while (tries < 4 && child_poll() < 0) {
                char line[512];

                snprintf(line, sizeof(line), "exec %s", name);
                (void)send_line(line);
                if (expect_pattern(marker, now() + steptmo)) {
                    seen = 1;
                    break;
                }
                tries++;
            }
            if (!seen)
                step_failures++;
            trace("step %s %s consumed=%lu", step, seen ? "ok" : "fail", (unsigned long)consumed);
        } else if (strncmp(step, "execq=", 6) == 0) {
            step_execq(step + 6, ok, nevers, n_never);
        } else {
            fprintf(stderr, "unknown step: %s\n", step);
            step_failures++;
        }
    }

    pump(now() + grace);
    if (must_exit) {
        while (child_poll() < 0 && now() < deadline) {
            usleep(20000);
        }
    } else {
        double until = now() + grace;

        while (child_poll() < 0 && now() < until && now() < deadline) {
            usleep(20000);
        }
    }
    qemu_rc = child_poll();
    self_exited = qemu_rc >= 0 && !child_by_signal;
    if (qemu_rc < 0) {
        kill(child_pid, SIGTERM);
        usleep(400000);
        if (child_poll() < 0) {
            kill(child_pid, SIGKILL);
            usleep(400000);
            child_poll();
        }
    }
    drain_rest();
    if (logf != NULL)
        fclose(logf);

    for (i = 0; i < n_never; i++) {
        if (contains(buf, buf_len, nevers[i])) {
            trace("decide forbidden=%s", nevers[i]);
            fprintf(stderr, "HARNESS: saw '%s', which must not appear\n", nevers[i]);
            return 1;
        }
    }

    if (must_exit && !self_exited) {
        trace("decide must_exit self_exited=0 by_signal=%d", child_by_signal);
        if (child_by_signal) {
            fprintf(stderr, "HARNESS: the machine was ended by a signal, "
                            "not by exiting\n");
        } else {
            fprintf(stderr, "HARNESS: the machine was still running at the "
                            "deadline and had to be killed\n");
        }
        return 1;
    }

    if (ok != NULL) {
        int seen = contains(buf, buf_len, ok);

        trace("decide ok_seen=%d step_failures=%d exited=%d rc=%d", seen, step_failures,
              self_exited, qemu_rc);
        if (self_exited && qemu_rc != 0 && (qemu_rc & 1) == 0) {
            fprintf(stderr,
                    "HARNESS: the machine exited with status %d, not through "
                    "isa-debug-exit\n",
                    qemu_rc);
            return 1;
        }
        if (seen && step_failures == 0) {
            fprintf(stderr, "HARNESS: saw '%s'\n", ok);
            return 0;
        }
        if (step_failures)
            fprintf(stderr, "HARNESS: %d step(s) failed\n", step_failures);
        else
            fprintf(stderr, "HARNESS: pattern '%s' not seen\n", ok);
        return 1;
    }

    if (qemu_rc < 0) {
        trace("decide timeout step_failures=%d", step_failures);
        fprintf(stderr, "HARNESS: timeout/hang: QEMU never self-exited\n");
        return 3;
    }
    {
        const char *marker = "process: exited with status ";
        size_t mlen = strlen(marker);
        long status = -1;
        int have_status = 0;
        size_t at;
        int via_debug_exit = (qemu_rc & 1) == 1;

        for (at = 0; at + mlen < buf_len; at++) {
            if (memcmp(buf + at, marker, mlen) == 0) {
                size_t p = at + mlen;
                long sign = 1, n = 0;
                int digits = 0, over = 0;

                if (p < buf_len && buf[p] == '-') {
                    sign = -1;
                    p++;
                }
                while (p < buf_len && buf[p] >= '0' && buf[p] <= '9') {
                    if (n > (LONG_MAX - 9) / 10)
                        over = 1;
                    else
                        n = n * 10 + (buf[p] - '0');
                    p++;
                    digits++;
                }
                if (over) {
                    fprintf(stderr,
                            "HARNESS: a guest status of %d digits "
                            "is past what this host reads\n",
                            digits);
                    have_status = 0;
                } else if (digits > 0) {
                    status = sign * n;
                    have_status = 1;
                }
            }
        }
        trace("decide qemu_rc=%d via_debug_exit=%d status=%s "
              "step_failures=%d",
              qemu_rc, via_debug_exit, have_status ? "set" : "none", step_failures);
        fprintf(stderr,
                "HARNESS: qemu_exit=%d via_debug_exit=%s "
                "guest_status=",
                qemu_rc, via_debug_exit ? "True" : "False");
        if (have_status)
            fprintf(stderr, "%ld\n", status);
        else
            fprintf(stderr, "?\n");
        if (!have_status || !via_debug_exit) {
            fprintf(stderr, "HARNESS: no clean guest exit reported, "
                            "failing\n");
            return 1;
        }
        if (step_failures) {
            fprintf(stderr, "HARNESS: %d step(s) failed\n", step_failures);
            return 1;
        }
        return status == 0 ? 0 : 1;
    }
}
