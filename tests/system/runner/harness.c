/**
 * harness: the tracked QEMU serial harness for probe and test runs.
 *
 * Boots qemu-system-i386 headless with the kernel and a KRFFS initrd,
 * attaches QEMU's isa-debug-exit device (iobase 0xF4), and drives the
 * guest with an expect and send script. With a HARNESS=1 kernel build the
 * kernel writes each process exit status to that port (rach2/sys/kio.c,
 * harness_exit), so QEMU stops with ((status << 1) | 1) and this reads a
 * REAL exit code: a hang or a triple fault then shows up as a timeout, not
 * as a missing serial marker.
 *
 * Usage:
 *   harness --kernel K --initrd I [--net NETVAL] [--timeout S]
 *           [--never TEXT ...] [STEP ...]
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
 *     send=TEXT      write TEXT and a newline to the guest serial
 *     sendraw=TEXT   write TEXT with no newline, for a guest terminal in
 *                    raw mode, where the newline would be read as the
 *                    byte. `\\`, `\n`, `\r`, `\t` and `\xNN` in TEXT stand
 *                    for the bytes they name
 *     wait=SECS      sleep SECS, draining serial
 *     tcp=PORT       open a TCP connection to 127.0.0.1:PORT and close it,
 *                    which a `hostfwd=tcp::PORT-:GUESTPORT` in --net
 *                    forwards to a server inside the guest. This provokes
 *                    traffic; it does not prove the guest answered,
 *                    because QEMU's user network accepts the host side of
 *                    the connection before it knows whether anything is
 *                    there. What the guest did is read from a capture.
 *     udp=PORT:TEXT  send TEXT as one UDP datagram to 127.0.0.1:PORT,
 *                    which a `hostfwd=udp::PORT-:GUESTPORT` forwards into
 *                    the guest. This is the only way a test can put a
 *                    frame on the guest's wire that the guest did not ask
 *                    for.
 *     exec=NAME|MARK run "exec NAME", re-sending if the shell drops it,
 *                    until MARK
 *     probe=NAME     boot, run "exec NAME", and let the kernel's
 *                    harness_exit stop QEMU; the process exit code is then
 *                    decoded from QEMU's.
 *
 * Exit code: 0 if the guest process exited 0 (QEMU returned an odd code
 * via isa-debug-exit); 1 if it exited non-zero; 2 if QEMU failed to
 * launch; 3 on a timeout or hang (QEMU never self-exited), an unambiguous
 * failure.
 *
 * With no --ok token the status is read off the kernel's "process: exited
 * with status N" record, which `sys_exit` emits only where the debug shell
 * is the supervisor. That is every image this harness boots; an image
 * running /sbin/init prints no such line, so drive one of those with --ok.
 *
 * `harness --selftest` checks the escape decoder without QEMU.
 *
 * `RACH2_TRACE=FILE` records each step's outcome and the final decision,
 * which is how this was proved equal to the Python harness it replaces.
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
 * The socket is opened only for a run whose steps need it, so every other
 * suite's command line is unchanged.
 */
static char monitor_path[128];
static int monitor_fd = -1;

/**
 * Read a `mouse=` step's three numbers.
 *
 * Returns 0 when all three were there and -1 otherwise. Separate from the step
 * that uses it so `--selftest` can check the parsing without a machine.
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
        return -1; /* three buttons, and nothing else is one */
    }
    return 0;
}

/** Whether any step needs the monitor. */
static int steps_need_monitor(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "key=", 4) == 0 || strncmp(argv[i], "mouse=", 6) == 0 ||
            strncmp(argv[i], "usb=", 4) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Read from the monitor until its prompt arrives, or the deadline passes.
 *
 * The prompt is what says the monitor has finished with what it was given.
 * Sleeping a fixed time instead is a race: under load a command sent before
 * the monitor was listening is taken by nobody, and the step that depended on
 * it waits out its whole limit for something that never happened.
 */
/* The chunk in which the monitor complained since the last command, if it
 * did, so a refused command can be reported with its reason. The monitor
 * echoes a command with a redraw of the line per character, which is far
 * more text than the reply itself, so only a chunk that carries an error
 * word is kept and every chunk is scanned for the prompt on its own. */
static char monitor_reply[512];
static int monitor_refused;

static int monitor_wait_prompt(int seconds)
{
    time_t deadline = time(NULL) + seconds;
    char buf[512];

    monitor_reply[0] = '\0';
    monitor_refused = 0;
    while (time(NULL) <= deadline) {
        ssize_t n = recv(monitor_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);

        if (n > 0) {
            buf[n] = '\0';
            if (!monitor_refused &&
                (strstr(buf, "Error") != NULL || strstr(buf, "error:") != NULL)) {
                memcpy(monitor_reply, buf, (size_t)n + 1);
                monitor_refused = 1;
            }
            if (strstr(buf, "(qemu)") != NULL) {
                return 0;
            }
            continue;
        }
        if (n == 0) {
            return -1; /* the monitor closed: the machine has gone */
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        usleep(10000);
    }
    return -1;
}

/** Connect to the monitor, once, and read past its greeting. */
static int monitor_open(void)
{
    struct sockaddr_un addr;
    int tries;

    if (monitor_fd >= 0) {
        return 0;
    }
    for (tries = 0; tries < 100; tries++) {
        monitor_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (monitor_fd < 0) {
            return -1;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, monitor_path, sizeof(addr.sun_path) - 1);
        if (connect(monitor_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }
        close(monitor_fd);
        monitor_fd = -1;
        usleep(50000); /* QEMU may not have made the socket yet */
    }
    if (monitor_fd < 0) {
        printf("HARNESS: no monitor at %s\n", monitor_path);
        return -1;
    }
    /* Its greeting ends in a prompt, and until that arrives it is not
     * listening. */
    if (monitor_wait_prompt(60) != 0) {
        printf("HARNESS: the monitor never gave a prompt\n");
        close(monitor_fd);
        monitor_fd = -1;
        return -1;
    }
    return 0;
}

/** Send one monitor command and let it be acted on. */
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
    /* The monitor acts on a command and then prompts again. Waiting for that
     * prompt is what makes the step ordered with respect to the machine: the
     * device is plugged in before the next step looks for it. */
    /* Sixty seconds, because a monitor starved by a loaded host answers
     * late and a command that was acted on late must not be counted as one
     * that was never acted on. */
    if (monitor_wait_prompt(60) != 0) {
        printf("HARNESS: the monitor did not answer '%s'\n", command);
        return -1;
    }
    /* A refused command answers with an error line before the prompt; the
     * step that sent it fails with that line, not with a later expect that
     * waits for a device that was never added or removed. */
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

static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
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
 * inside addresses, but a pattern is a pattern: this supports the subset
 * the Python harness's patterns could use and no more. A literal
 * character matches itself, `.` matches any character but a newline, a
 * `[...]` class matches one of its members (with ranges and a leading `^`
 * to negate), `\` escapes the next character, and `*`, `+` and `?` repeat
 * the item before them. `^` and `$` anchor. Anything else is literal.
 *
 * Backtracking, because a serial log is small and the patterns are short.
 *
 * A pattern using a construct outside this subset (a group, an
 * alternation, a counted repeat) is REFUSED, not silently taken as
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
            p++;
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

/* Match `re->item[k..]` at `text[at]`; returns the end offset or -1. */
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
            /* The longest run first, backing off one at a time, which is
               what a greedy repeat means. */
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

/* The end offset of the first match at or after `from`, or -1. */
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
static int child_out = -1;
static int child_in = -1;
static int child_status;
static int child_reaped;
/* Whether the machine was ended by a signal rather than by exiting. A run
   that requires an exit does not accept one. */
static int child_by_signal;
static FILE *logf;
static double deadline;

/* The child's exit code, or -1 while it runs. */
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

/* Read what the guest has said, until `until` or the deadline. */
static void pump(double until)
{
    while (now() < until && now() < deadline) {
        fd_set r;
        struct timeval tv;
        unsigned char chunk[BUF_GROW];
        ssize_t n;

        FD_ZERO(&r);
        FD_SET(child_out, &r);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
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
            /* Drain what is left in the pipe before giving up. */
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

static double steptmo = 15.0;

static int expect_pattern(const char *pattern)
{
    struct regex re;
    double stop;

    regex_compile(&re, pattern);
    stop = now() + steptmo;
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
    /* One last look at what arrived while the child was stopping. */
    {
        long end = regex_search(&re, buf, buf_len, consumed);

        if (end >= 0) {
            consumed = (size_t)end;
            return 1;
        }
    }
    return 0;
}

/* `newline` is what separates `send=` from `sendraw=`. A terminal in raw
   mode delivers each byte as it arrives, so a probe reading one byte in
   that mode is handed the newline of a `send=` as its byte, and the byte
   the test meant to deliver arrives after the read it was written for. */
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

static void send_text(const char *text, int newline)
{
    size_t n = strlen(text);

    if (child_in < 0)
        return;
    if (write(child_in, text, n) < 0) {
        /* A closed pipe means the guest is gone; the caller notices. */
        return;
    }
    if (newline && write(child_in, "\n", 1) < 0) {
        /* Same. */
    }
}

static void send_line(const char *text)
{
    send_text(text, 1);
}

/* Decode `\\`, `\n`, `\r`, `\t` and `\xNN` in place of the bytes they name,
   so a step can deliver a control byte the shell would not carry. Only
   `sendraw=` is decoded: `send=` has always written its text as typed, and
   a step that contains a backslash today means the backslash.

   An escape this does not know is an error rather than the letter after
   the backslash: a step that meant a byte and got a letter sends the wrong
   input and reads as a kernel fault. Returns the length, or -1. */
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

static int contains(const unsigned char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;

    if (n == 0 || n > len)
        return 0;
    for (i = 0; i + n <= len; i++)
        if (memcmp(hay + i, needle, n) == 0)
            return 1;
    return 0;
}

/* The escape decoder, against the cases the suites depend on and the ones
   they do not: a decoder that quietly passed a bad escape through would send
   the letter after the backslash, and a probe reading one byte would be given
   the wrong one and read as a kernel fault. */
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
    /* A text longer than the buffer is refused rather than cut: a step that
       sent half of what it was given would be checked against the whole. */
    {
        char out[4];

        if (unescape("abcdef", out, sizeof(out)) >= 0) {
            printf("harness: selftest: a text past the buffer was accepted\n");
            bad++;
        }
    }

    /* The mouse step's three numbers. A step whose numbers were misread would
       move the mouse somewhere else and the check that follows would fail for
       a reason nobody could see. */
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

/* `--match PATTERN` prints the end offset of the first match in the text
   on standard input, or -1. The matcher is proved against Python's `re`
   through this. */
static int match_mode(const char *pattern)
{
    struct regex re;
    unsigned char text[1 << 16];
    size_t n = fread(text, 1, sizeof(text), stdin);

    regex_compile(&re, pattern);
    printf("%ld\n", regex_search(&re, text, n, 0));
    return 0;
}

/* A run that asks for more than the harness holds is stopped, not
   trimmed: a dropped step is a check that never ran under a report that
   says the run passed. */
static int too_many(const char *what, int limit)
{
    fprintf(stderr,
            "harness: more than %d %s arguments; this harness "
            "would run only the first %d\n",
            limit, what, limit);
    return 2;
}

int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL, *append = NULL;
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
        return unescape_selftest();
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
#undef OPT
        if (strcmp(arg, "--never") == 0 && i + 1 < argc) {
            if (n_never >= MAX_ARGS)
                return too_many("--never", MAX_ARGS);
            /* An empty text is in every stream and in none, depending on how
               you read it; either way the run could not fail on it, and a
               check that cannot fail is worse than no check. */
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
            i++; /* accepted and unused, as in the Python harness */
        } else if (strcmp(arg, "--must-exit") == 0) {
            must_exit = 1;
        } else if (strcmp(arg, "--no-debug-exit") == 0) {
            no_debug_exit = 1;
        } else if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "harness: unknown option %s\n", arg);
            return 2;
        } else {
            /* A probe=NAME step expands to a wait for the shell prompt and
               a repeated exec, the way the Python harness expanded it. */
            if (strncmp(arg, "probe=", 6) == 0) {
                static char execq[MAX_STEPS][256];
                static int n_execq;

                if (n_steps + 2 > MAX_STEPS)
                    return too_many("step", MAX_STEPS);
                if ((size_t)snprintf(execq[n_execq], sizeof(execq[0]), "execq=%s", arg + 6) >=
                    sizeof(execq[0])) {
                    fprintf(stderr,
                            "harness: the probe name in %s is "
                            "longer than %lu bytes; a shortened name runs "
                            "a different program\n",
                            arg, (unsigned long)sizeof(execq[0]) - 7);
                    return 2;
                }
                steps[n_steps++] = "expect=Type 'help'";
                steps[n_steps++] = execq[n_execq++];
            } else {
                if (n_steps >= MAX_STEPS)
                    return too_many("step", MAX_STEPS);
                steps[n_steps++] = arg;
            }
        }
    }
    if (kernel == NULL) {
        fprintf(stderr, "harness: --kernel is required\n");
        return 2;
    }

    /* -no-reboot so a triple fault exits QEMU (read here as a failure)
       instead of looping; NOT -no-shutdown, which would stop the
       isa-debug-exit device from stopping the machine. */
    if (steps_need_monitor(argc, argv)) {
        snprintf(monitor_path, sizeof(monitor_path), "/tmp/rach2-monitor-%d", (int)getpid());
        unlink(monitor_path);
    }

    qemu[nq++] = (char *)"qemu-system-i386";
    qemu[nq++] = (char *)"-kernel";
    qemu[nq++] = (char *)kernel;
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
    if (initrd != NULL) {
        qemu[nq++] = (char *)"-initrd";
        qemu[nq++] = (char *)initrd;
    }
    if (append != NULL) {
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
    child_out = out_pipe[0];
    child_in = in_pipe[1];
    fcntl(child_out, F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);

    trace_path = getenv("RACH2_TRACE");
    if (trace_path != NULL)
        tracef = fopen(trace_path, "w");
    if (log != NULL)
        logf = fopen(log, "wb");
    deadline = now() + timeout;
    buf_add((const unsigned char *)"", 0);

    for (i = 0; i < n_steps; i++) {
        const char *step = steps[i];

        /* The machine is gone, so every step left is a check that will not
           run. They are counted and named: a run that stopped early and
           reported a pass over the steps it never reached is the report
           this harness exists to prevent. */
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
        if (strncmp(step, "expect=", 7) == 0) {
            int got = expect_pattern(step + 7);

            if (!got)
                step_failures++;
            trace("step %s %s consumed=%lu", step, got ? "ok" : "fail", (unsigned long)consumed);
        } else if (strncmp(step, "send=", 5) == 0) {
            send_line(step + 5);
        } else if (strncmp(step, "sendraw=", 8) == 0) {
            char raw[256];
            int n = unescape(step + 8, raw, sizeof(raw));

            if (n < 0 || child_in < 0 || write(child_in, raw, (size_t)n) != n) {
                fprintf(stderr,
                        "HARNESS: step '%s' did not reach the "
                        "guest\n",
                        step);
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
                /* Only when a button is asked for: pressing none is not a
                 * thing that happens, and a machine that has already exited
                 * would refuse the command and fail a step that had nothing
                 * to do. */
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
        } else if (strncmp(step, "wait=", 5) == 0) {
            pump(now() + atof(step + 5));
        } else if (strncmp(step, "tcp=", 4) == 0) {
            int port = atoi(step + 4);
            int sk = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            struct timeval tv;

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons((unsigned short)port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            tv.tv_sec = 15;
            tv.tv_usec = 0;
            setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (sk >= 0 && connect(sk, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                printf("HARNESS: knocked on port %d\n", port);
            } else {
                printf("HARNESS: nothing answered on port %d (%s)\n", port, strerror(errno));
                step_failures++;
            }
            if (sk >= 0)
                close(sk);
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
                send_line(line);
                if (expect_pattern(marker)) {
                    seen = 1;
                    break;
                }
                tries++;
            }
            if (!seen)
                step_failures++;
            trace("step %s %s consumed=%lu", step, seen ? "ok" : "fail", (unsigned long)consumed);
        } else if (strncmp(step, "execq=", 6) == 0) {
            /* Run a probe, re-sending if the kernel shell drops the
               command, and stop as soon as harness_exit stops QEMU. */
            int tries = 0;

            while (tries < 6 && child_poll() < 0) {
                char line[512];

                snprintf(line, sizeof(line), "exec %s", step + 6);
                send_line(line);
                pump(now() + 3.0);
                tries++;
            }
        } else {
            fprintf(stderr, "unknown step: %s\n", step);
            step_failures++;
        }
    }

    pump(now() + grace);
    /* `pump` returns as soon as the serial pipe reaches its end, which is
       before the machine that closed it has finished going away, so the exit
       is waited for here rather than read off that instant. Bounded by the
       same grace period, so a machine that is not leaving still costs only
       that. */
    if (must_exit) {
        /* To the deadline, which is what the caller asked to wait: a machine
           that is on its way out has as long as the run has. Only where an
           exit is required, because a run whose machine is meant to stay up
           would otherwise wait out its whole timeout. */
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
    /* Whether the machine ended itself. A suite that boots a machine to make
       it restart or halt has no other way to tell that from a machine that
       printed the right line and then sat there: both leave the same log, and
       the harness kills the second one at the deadline. */
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
    pump(now() + 0.5);
    if (logf != NULL)
        fclose(logf);

    /* A forbidden text fails the run whatever else happened. A probe whose
       whole claim is that the kernel killed it prints one line if it was
       not killed, and that line is the failure: the probe's own status
       cannot report it, because a probe that was not killed goes on to
       exit however it likes. */
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

    /* A serial-token check passes on the printed token alone, for a probe
       run in the debug shell that never stops QEMU itself. */
    if (ok != NULL) {
        int seen = contains(buf, buf_len, ok);

        trace("decide ok_seen=%d step_failures=%d exited=%d rc=%d", seen, step_failures,
              self_exited, qemu_rc);
        if (self_exited && qemu_rc != 0) {
            fprintf(stderr, "HARNESS: the machine exited with status %d\n", qemu_rc);
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
    /* The result comes from the kernel's SERIAL status print, not from
       QEMU's exit code: the host truncates exit codes to 8 bits, so
       ((status << 1) | 1) aliases (status 128 gives host 1) and a QEMU
       launch error also gives 1; either would pass a broken run. The
       serial line is the true, untruncated status and also proves the
       guest ran. The exit code is used only to confirm that QEMU stopped
       through isa-debug-exit (an odd code) instead of hanging. */
    {
        const char *marker = "process: exited with status ";
        size_t mlen = strlen(marker);
        long status = -1;
        int have_status = 0;
        size_t at;
        int via_debug_exit = (qemu_rc & 1) == 1;

        /* The status must be digits, as the Python harness's `(-?\d+)`
           required: text that is not a number would otherwise read as
           status 0 and pass a run that never reported one. */
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
                /* A number too large for a long is reported rather than
                   read as the prefix that fit. Python compared the whole
                   value; a prefix would be a different status. */
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
        /* A step that failed fails the run even when the guest went on to
           exit cleanly. The Python harness counted step failures only on
           the --ok path, so a run driven by steps alone could report a
           pass over a step that never matched. */
        if (step_failures) {
            fprintf(stderr, "HARNESS: %d step(s) failed\n", step_failures);
            return 1;
        }
        return status == 0 ? 0 : 1;
    }
}
