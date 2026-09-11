/**
 * ptydrive: run a command on a pseudo-terminal and answer its password
 * prompts.
 *
 * The SSH suites need a client that reads a password from a terminal, on a
 * terminal with a known size, because a session's window size is part of
 * what they prove. This driver replaces the Expect script that did that:
 * it spawns the command on a pty sized `--rows` by `--cols`, answers every
 * prompt matching `--prompt-re` with the value of the environment variable
 * named by `--password-env` (the environment, not an argument, so the
 * secret never shows in a process listing), and relays everything the
 * command writes to standard output.
 *
 * Two tokens end the transcript, and the suites read both. `TIMEDOUT`
 * appears when `--timeout` seconds pass before the command exits;
 * `PROMPTED=N` counts the prompts that were answered, so a server that let
 * a login in without asking cannot be read as a password login. The driver
 * exits 0 when it ran the command to completion (whatever the command's
 * own outcome), and nonzero when it could not run it at all, the
 * distinction the suites need between "the server said no" and "the
 * question was never asked".
 *
 * The prompt pattern is matched by the same small matcher the harness
 * uses; the default `[Pp]assword:` is a character class and a literal.
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

/* ---- the prompt matcher ----
 *
 * A literal character matches itself, `.` matches any character but a
 * newline, `[...]` matches one of its members (ranges and a leading `^` to
 * negate), `\` escapes, and `*`, `+` and `?` repeat. A pattern using a
 * construct outside that subset is refused rather than silently taken as
 * literal text, because a driver that never sees its prompt would hang
 * until the timeout and report a question that was asked as one that was
 * not.
 */

struct item {
    enum { LITERAL, ANY, CLASS } kind;
    char literal;
    unsigned char set[32];
    int negate;
    char repeat;
};

static struct item items[64];
static int n_items;

static void class_add(struct item *it, unsigned char c)
{
    it->set[c >> 3] |= (unsigned char) (1 << (c & 7));
}

static void compile_prompt(const char *pattern)
{
    const char *p;

    for (p = pattern; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            continue;
        }
        if (strchr("(){}|^$", *p) != NULL) {
            fprintf(stderr, "ptydrive: the pattern '%s' uses '%c', which "
                    "this matcher does not support\n", pattern, *p);
            exit(2);
        }
    }
    p = pattern;
    while (*p != '\0') {
        struct item *it = &items[n_items];

        if (n_items >= 64) {
            fprintf(stderr, "ptydrive: the pattern '%s' has more than 64 "
                    "items; a matcher holding a prefix of it would answer "
                    "a prompt the pattern does not name\n", pattern);
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

                    for (c = prev; c <= (unsigned char) p[1]; c++)
                        class_add(it, (unsigned char) c);
                    p += 2;
                    prev = 0;
                    continue;
                }
                if (*p == '\\' && p[1] != '\0')
                    p++;
                class_add(it, (unsigned char) *p);
                prev = (unsigned char) *p;
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

static int item_matches(const struct item *it, unsigned char c)
{
    switch (it->kind) {
    case LITERAL:
        return (unsigned char) it->literal == c;
    case ANY:
        return c != '\n';
    case CLASS:
        return it->negate
            ? !((it->set[c >> 3] >> (c & 7)) & 1)
            : ((it->set[c >> 3] >> (c & 7)) & 1);
    }
    return 0;
}

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
        if (at < len && item_matches(it, text[at]) &&
            match_here(k + 1, text, len, at + 1))
            return 1;
        return match_here(k + 1, text, len, at);
    }
    if (at < len && item_matches(it, text[at]))
        return match_here(k + 1, text, len, at + 1);
    return 0;
}

static int prompt_seen(const unsigned char *text, size_t len)
{
    size_t at;

    for (at = 0; at < len; at++)
        if (match_here(0, text, len, at))
            return 1;
    return 0;
}

static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
}

/* ---- the deadline factor ----
 *
 * A set's runner measures the emulator's rate under the load the set makes
 * and passes `RACH2_DEADLINES=scaled` with `RACH2_DEADLINE_FACTOR=F` to
 * every suite; a deadline that bounds completion is multiplied by F here.
 * A process told nothing, or told the mode without a factor, keeps its
 * numbers as written. */
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

int main(int argc, char **argv)
{
    int rows = 40, cols = 100;
    double timeout = 60.0;
    const char *prompt_re = "[Pp]assword:";
    const char *password_env = "RACH2_PASSWORD";
    const char *password;
    char **command = NULL;
    int master, i;
    pid_t pid;
    struct winsize ws;
    unsigned char tail[TAIL_BYTES];
    size_t tail_len = 0;
    int asked = 0, timed_out = 0;
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
            /* The end-of-options marker: what follows is the command, even
               when its first word begins with a dash. */
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
    if (password == NULL) {
        fprintf(stderr, "ptydrive: %s is not set\n", password_env);
        return 2;
    }
    compile_prompt(prompt_re);

    ws.ws_row = (unsigned short) rows;
    ws.ws_col = (unsigned short) cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        fprintf(stderr, "ptydrive: cannot open a pty: %s\n", strerror(errno));
        return 2;
    }
    if (pid == 0) {
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

        if (remaining <= 0) {
            timed_out = 1;
            break;
        }
        FD_ZERO(&r);
        FD_SET(master, &r);
        tv.tv_sec = (long) (remaining < 1.0 ? remaining : 1.0);
        tv.tv_usec = (long) (((remaining < 1.0 ? remaining : 1.0) -
                              (double) tv.tv_sec) * 1e6);
        if (select(master + 1, &r, NULL, NULL, &tv) <= 0)
            continue;
        n = read(master, data, sizeof(data));
        if (n <= 0) {
            /* A pty reports EIO when the last writer closes it, which is
               how the command's exit reaches here. */
            break;
        }
        fwrite(data, 1, (size_t) n, stdout);
        fflush(stdout);
        /* A bounded tail across read boundaries, so a prompt split between
           two reads is still answered. */
        {
            size_t keep = tail_len + (size_t) n > TAIL_BYTES
                ? TAIL_BYTES : tail_len + (size_t) n;
            unsigned char merged[TAIL_BYTES + 4096];

            memcpy(merged, tail, tail_len);
            memcpy(merged + tail_len, data, (size_t) n);
            memcpy(tail, merged + (tail_len + (size_t) n) - keep, keep);
            tail_len = keep;
        }
        if (prompt_seen(tail, tail_len)) {
            asked++;
            if (write(master, password, strlen(password)) < 0 ||
                write(master, "\r", 1) < 0) {
                /* The command is gone; the loop's next read reports it. */
            }
            tail_len = 0;
        }
    }

    if (timed_out) {
        printf("TIMEDOUT\n");
        /* forkpty puts the command in its own session, so the negative pid
           reaches whatever it started as well. A command that ignores the
           signal, or a descendant holding the pty, would otherwise keep
           this process waiting after its own deadline has passed. */
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
    printf("PROMPTED=%d\n", asked ? 1 : 0);
    return 0;
}
