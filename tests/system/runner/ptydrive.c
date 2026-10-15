/**
 * ptydrive: run a command on a pseudo-terminal of a given size and answer
 * one password prompt.
 *
 * Usage: ptydrive [--rows N] [--cols N] [--timeout S] [--prompt-re RE]
 *                 [--password-env NAME] [--] COMMAND [ARG ...]
 *
 * The terminal is 40 rows by 100 columns unless given. The password is the
 * value of the environment variable NAME, RACH2_PASSWORD unless given; it is
 * removed from the command's environment before the exec, and an empty
 * password or one with a control byte is refused. When the command's output
 * matches RE, `[Pp]assword:` unless given, the password and a carriage
 * return are typed, and a second prompt stops the command unanswered. A
 * prompt is matched byte by byte against the last 256 bytes since the one
 * before it, so it is found across reads, and each of several prompts in one
 * read is counted.
 *
 * The command's output is copied to standard output without its carriage
 * returns and with each whole password written as `[redacted]`. A prefix of
 * the password is held back until it is completed or broken, and one still
 * held at the end of the output, a timeout or a stop is written as
 * `[redacted]` too.
 *
 * The run ends when the command closes the terminal, or after S seconds, 60
 * unless given and multiplied by the set's deadline factor, which prints
 * `TIMEDOUT`. `PROMPTED=N` then counts the prompts seen and `ANSWERED=N`
 * the answers sent. The exit status is 0, after a timeout too, 1 after a
 * second prompt or an answer that could not be written, and 2 for a usage
 * error or a failure to set up. The command's own status is not reported;
 * the suites judge its output and the two counts.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

#define TAIL_BYTES 256

/* The output filter: the `length` bytes of the password, and the `used`
 * bytes of output held in `pending`, which are a prefix of the password. */
struct secret_filter {
    const unsigned char *password;
    unsigned char *pending;
    size_t length;
    size_t used;
};

/**
 * Pass one byte of the command's output through `filter` to standard output.
 *
 * A carriage return is dropped. A byte that extends a prefix of the password
 * is held; a held run that stops being a prefix is written from its first
 * byte until what is left is one again, and a run that completes the
 * password is written as `[redacted]`. Carriage returns are dropped before
 * the match, so output that has the password with one inside it is still
 * redacted; the password itself has no control bytes.
 */
static void secret_byte(struct secret_filter *filter, unsigned char byte)
{
    if (byte == '\r')
        return;
    filter->pending[filter->used++] = byte;
    while (filter->used != 0 && memcmp(filter->pending, filter->password, filter->used) != 0) {
        putchar(filter->pending[0]);
        filter->used--;
        memmove(filter->pending, filter->pending + 1, filter->used);
    }
    if (filter->used == filter->length) {
        fputs("[redacted]", stdout);
        filter->used = 0;
    }
}

/**
 * Write a held prefix of the password as `[redacted]`, at the end of the
 * output, a timeout or a stop, and flush standard output.
 */
static void secret_finish(struct secret_filter *filter)
{
    if (filter->used != 0) {
        fputs("[redacted]", stdout);
        filter->used = 0;
    }
    fflush(stdout);
}

/**
 * Write the `length` bytes of `text` to `master`, across interrupted and
 * short writes. Returns 0, or -1 when a write fails.
 */
static int answer_write(int master, const char *text, size_t length)
{
    while (length != 0) {
        ssize_t written = write(master, text, length);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        text += written;
        length -= (size_t)written;
    }
    return 0;
}

/* An item of the prompt's pattern: a literal byte, any byte but a newline,
 * or one of a class of bytes in a map of 256 bits, `negate` inverting it,
 * with `repeat` the '*', '+' or '?' that follows the item, or 0. */
struct item {
    enum { LITERAL, ANY, CLASS } kind;
    char literal;
    unsigned char set[32];
    int negate;
    char repeat;
};

/* The prompt's pattern, compiled. */
static struct item items[64];
static int n_items;

/** Add the byte `c` to the class of `it`. */
static void class_add(struct item *it, unsigned char c)
{
    it->set[c >> 3] |= (unsigned char)(1 << (c & 7));
}

/**
 * End the program with status 2 and a message when `pattern` has an
 * unescaped `(`, `)`, `{`, `}`, `|`, `^` or `$` anywhere, inside a class
 * too, but for a `^` that is the first character of a class.
 */
static void refuse_unsupported(const char *pattern)
{
    const char *p;
    int in_class = 0;

    for (p = pattern; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            continue;
        }
        if (!in_class && *p == '[') {
            in_class = 1;
            if (p[1] == '^')
                p++;
            continue;
        }
        if (in_class && *p == ']') {
            in_class = 0;
            continue;
        }
        if (strchr("(){}|^$", *p) != NULL) {
            fprintf(stderr,
                    "ptydrive: the pattern '%s' uses '%c', which "
                    "this matcher does not support\n",
                    pattern, *p);
            exit(2);
        }
    }
}

/**
 * Compile `pattern` into `items`.
 *
 * A literal character matches itself, `.` any character but a newline,
 * `[...]` one of its members, with ranges, and `[^...]` any character but
 * them; `\` escapes the character after it, and `*`, `+` and `?` repeat the
 * item before them. A pattern with more than 64 items, or with a character
 * `refuse_unsupported` refuses, ends the program with status 2 and a
 * message. Such a pattern is refused rather than read as literal text,
 * since a driver that never saw its prompt would wait out its timeout and
 * report a prompt that was shown as one that was not.
 */
static void compile_prompt(const char *pattern)
{
    const char *p;

    refuse_unsupported(pattern);
    p = pattern;
    while (*p != '\0') {
        struct item *it = &items[n_items];

        if (n_items >= 64) {
            fprintf(stderr,
                    "ptydrive: the pattern '%s' has more than 64 "
                    "items; a matcher holding a prefix of it would answer "
                    "a prompt the pattern does not name\n",
                    pattern);
            exit(2);
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
            while (*p != '\0' && *p != ']') {
                if (*p == '-' && p[1] != '\0' && p[1] != ']' && prev != 0) {
                    unsigned c;

                    for (c = prev; c <= (unsigned char)p[1]; c++)
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
        n_items++;
    }
}

/** Whether the byte `c` matches `it`, its repeat aside. */
static int item_matches(const struct item *it, unsigned char c)
{
    switch (it->kind) {
    case LITERAL:
        return (unsigned char)it->literal == c;
    case ANY:
        return c != '\n';
    case CLASS:
        return it->negate ? !((it->set[c >> 3] >> (c & 7)) & 1)
                          : ((it->set[c >> 3] >> (c & 7)) & 1);
    }
    return 0;
}

/**
 * Whether the items from `k` on match the `len` bytes of `text` from `at`.
 * A `*` or `+` item takes the longest run first and gives back one byte at a
 * time.
 */
static int match_here(int k, const unsigned char *text, size_t len, size_t at)
{
    const struct item *it;

    if (k == n_items)
        return 1;
    it = &items[k];
    if (it->repeat == '*' || it->repeat == '+') {
        size_t count = 0;

        while (at + count < len && item_matches(it, text[at + count]))
            count++;
        for (;;) {
            if (it->repeat == '+' && count == 0)
                return 0;
            if (match_here(k + 1, text, len, at + count))
                return 1;
            if (count == 0)
                return 0;
            count--;
        }
    }
    if (it->repeat == '?') {
        if (at < len && item_matches(it, text[at]) && match_here(k + 1, text, len, at + 1))
            return 1;
        return match_here(k + 1, text, len, at);
    }
    if (at < len && item_matches(it, text[at]))
        return match_here(k + 1, text, len, at + 1);
    return 0;
}

/** Whether the prompt's pattern matches anywhere in the `len` bytes of
 * `text`. */
static int prompt_seen(const unsigned char *text, size_t len)
{
    size_t at;

    for (at = 0; at < len; at++)
        if (match_here(0, text, len, at))
            return 1;
    return 0;
}

/** Seconds on the calendar clock. */
static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* The set's deadline factor, which multiplies every completion bound. */
static double deadline_factor = 1.0;

/**
 * Read the deadline factor. tests/run passes its `RACH2_TIMEOUT_FACTOR` to
 * every suite as `RACH2_DEADLINES=scaled` with `RACH2_DEADLINE_FACTOR=F`. A
 * process told nothing, told the mode without a factor, or given a factor
 * outside 1 to 64 keeps the factor 1.
 */
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

/** A completion bound of `seconds` multiplied by the deadline factor. */
static double scaled(double seconds)
{
    return seconds * deadline_factor;
}

/**
 * Read the options, start the command on a terminal with `forkpty`, and
 * relay its output and answer its prompt until the end of the run.
 *
 * After a timeout or a second prompt the command's group is sent SIGTERM,
 * which reaches what the command started, since `forkpty` makes the
 * command a session leader; a command that ignores it, or a descendant that
 * holds the terminal, would otherwise keep the driver waiting past its
 * deadline. A command still running five seconds after the end is sent
 * SIGKILL, and five seconds after that the driver reports without it.
 */
int main(int argc, char **argv)
{
    int rows = 40, cols = 100;
    double timeout = 60.0;
    const char *prompt_re = "[Pp]assword:";
    const char *password_env = "RACH2_PASSWORD";
    const char *password;
    struct secret_filter filter;
    char **command = NULL;
    int master, i;
    pid_t pid;
    struct winsize ws;
    unsigned char tail[TAIL_BYTES];
    size_t tail_len = 0;
    int asked = 0, answered = 0, timed_out = 0, failed = 0;
    double deadline;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc)
            cols = atoi(argv[++i]);
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atof(argv[++i]);
        else if (strcmp(argv[i], "--prompt-re") == 0 && i + 1 < argc)
            prompt_re = argv[++i];
        else if (strcmp(argv[i], "--password-env") == 0 && i + 1 < argc)
            password_env = argv[++i];
        else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc)
                command = &argv[i + 1];
            break;
        } else {
            command = &argv[i];
            break;
        }
    }
    if (command == NULL) {
        fprintf(stderr, "usage: ptydrive [--rows N] [--cols N] "
                        "[--timeout S] [--prompt-re RE] [--password-env NAME] "
                        "COMMAND ...\n");
        return 2;
    }
    password = getenv(password_env);
    if (password == NULL || password[0] == '\0') {
        fprintf(stderr, "ptydrive: the password variable is unset or empty\n");
        return 2;
    }
    filter.password = (const unsigned char *)password;
    filter.length = strlen(password);
    filter.used = 0;
    for (i = 0; password[i] != '\0'; i++) {
        if ((unsigned char)password[i] < 32 || (unsigned char)password[i] == 127) {
            fprintf(stderr, "ptydrive: the password contains a control byte\n");
            return 2;
        }
    }
    filter.pending = malloc(filter.length);
    if (filter.pending == NULL) {
        fprintf(stderr, "ptydrive: cannot allocate the output filter\n");
        return 2;
    }
    compile_prompt(prompt_re);

    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        fprintf(stderr, "ptydrive: cannot open a pty: %s\n", strerror(errno));
        free(filter.pending);
        return 2;
    }
    if (pid == 0) {
        if (unsetenv(password_env) != 0)
            _exit(127);
        execvp(command[0], command);
        _exit(127);
    }

    deadline_factor_read();
    deadline = now() + scaled(timeout);
    for (;;) {
        double remaining = deadline - now();
        fd_set r;
        struct timeval tv;
        unsigned char data[4096];
        ssize_t n;
        size_t at;

        if (remaining <= 0) {
            timed_out = 1;
            break;
        }
        FD_ZERO(&r);
        FD_SET(master, &r);
        tv.tv_sec = (long)(remaining < 1.0 ? remaining : 1.0);
        tv.tv_usec = (long)(((remaining < 1.0 ? remaining : 1.0) - (double)tv.tv_sec) * 1e6);
        if (select(master + 1, &r, NULL, NULL, &tv) <= 0)
            continue;
        n = read(master, data, sizeof(data));
        if (n <= 0) {
            /* The master reads EIO or end of file once the slave closes. */
            break;
        }
        for (at = 0; at < (size_t)n; at++) {
            secret_byte(&filter, data[at]);
            if (tail_len == TAIL_BYTES) {
                memmove(tail, tail + 1, TAIL_BYTES - 1);
                tail_len--;
            }
            tail[tail_len++] = data[at];
            if (prompt_seen(tail, tail_len)) {
                asked++;
                tail_len = 0;
                if (asked > 1) {
                    failed = 1;
                    break;
                }
                if (answer_write(master, password, filter.length) != 0 ||
                    answer_write(master, "\r", 1) != 0) {
                    failed = 1;
                    break;
                }
                answered++;
            }
        }
        fflush(stdout);
        if (failed)
            break;
    }

    secret_finish(&filter);
    free(filter.pending);
    if (timed_out)
        printf("\nTIMEDOUT\n");
    if (timed_out || failed) {
        if (kill(-pid, SIGTERM) != 0)
            kill(pid, SIGTERM);
    }
    close(master);
    {
        double until = now() + 5.0;
        int killed = 0;

        while (waitpid(pid, NULL, WNOHANG) == 0) {
            if (now() >= until && !killed) {
                if (kill(-pid, SIGKILL) != 0)
                    kill(pid, SIGKILL);
                killed = 1;
                until = now() + 5.0;
            } else if (now() >= until) {
                fprintf(stderr, "ptydrive: the command survived SIGKILL; "
                                "leaving it and reporting\n");
                break;
            }
            usleep(50000);
        }
    }
    printf("\nPROMPTED=%d\nANSWERED=%d\n", asked, answered);
    return failed ? 1 : 0;
}
