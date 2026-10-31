/*
 * main.c: term's command line, its settings, and its loop, which owns every
 * dispatch: the child's output, the keys and the framebuffer's events.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* sigaction, kill */
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <rach2/fb.h>

#include "term.h"

/* The signals term catches; the first is SIGCHLD, the others end it. */
static const int caught[] = {SIGCHLD, SIGTERM, SIGHUP, SIGINT, SIGQUIT};

/* The pipe the signal handler writes each caught signal's number to. */
static int sig_pipe[2] = {-1, -1};

/** The handler of every caught signal: one byte to the pipe. */
static void on_signal(int sig)
{
    int saved = errno;
    unsigned char b = (unsigned char)sig;

    (void)!write(sig_pipe[1], &b, 1);
    errno = saved;
}

int term_signals_open(void)
{
    struct sigaction sa;
    size_t i;
    int k;

    if (pipe(sig_pipe) < 0)
        return -errno;
    for (k = 0; k < 2; k++) {
        if (fcntl(sig_pipe[k], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(sig_pipe[k], F_SETFL, fcntl(sig_pipe[k], F_GETFL) | O_NONBLOCK) < 0) {
            k = -errno;
            term_signals_close();
            return k;
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(caught) / sizeof(caught[0]); i++) {
        sa.sa_flags = caught[i] == SIGCHLD ? SA_NOCLDSTOP | SA_RESTART : SA_RESTART;
        sigaction(caught[i], &sa, NULL);
    }
    signal(SIGPIPE, SIG_IGN);
    return sig_pipe[0];
}

unsigned term_signals_take(void)
{
    unsigned char buf[64];
    unsigned bits = 0;
    ssize_t n;
    ssize_t i;

    while ((n = read(sig_pipe[0], buf, sizeof(buf))) > 0)
        for (i = 0; i < n; i++)
            bits |= buf[i] == SIGCHLD ? TERM_SIG_CHILD : TERM_SIG_QUIT;
    return bits;
}

void term_signals_close(void)
{
    size_t i;

    for (i = 0; i < sizeof(caught) / sizeof(caught[0]); i++)
        signal(caught[i], SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (sig_pipe[0] >= 0)
        close(sig_pipe[0]);
    if (sig_pipe[1] >= 0)
        close(sig_pipe[1]);
    sig_pipe[0] = -1;
    sig_pipe[1] = -1;
}

int term_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

void term_conf_defaults(struct term_conf *c)
{
    strcpy(c->font, TERM_FONT_PATH);
    c->scale = 1;
    c->scrollback = TERM_SCROLLBACK;
}

/** Strip blanks from both ends of `s` in place and return its first byte. */
static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        e--;
    *e = '\0';
    return s;
}

/**
 * Parse `s` as a decimal number from `lo` to `hi` into `*out`. Returns false,
 * leaving `*out` alone, when it is not one.
 */
static bool parse_number(const char *s, unsigned long lo, unsigned long hi, unsigned *out)
{
    unsigned long v = 0;

    if (*s == '\0')
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > hi)
            return false;
    }
    if (v < lo)
        return false;
    *out = (unsigned)v;
    return true;
}

/**
 * Apply one setting. Returns NULL, or what is wrong with the line, which
 * the caller prints after the setting's name.
 */
static const char *conf_set(struct term_conf *c, const char *name, const char *value)
{
    if (strcmp(name, "font") == 0) {
        if (value[0] != '/' || strlen(value) >= sizeof(c->font))
            return "not an absolute path that fits";
        strcpy(c->font, value);
    } else if (strcmp(name, "scale") == 0) {
        if (!parse_number(value, 1, 2, &c->scale))
            return "not 1 or 2";
    } else if (strcmp(name, "scrollback") == 0) {
        if (!parse_number(value, 0, TERM_SCROLLBACK_MAX, &c->scrollback))
            return "not a count of lines from 0 to 100000";
    } else {
        return "unknown setting";
    }
    return NULL;
}

int term_conf_read(struct term_conf *c, const char *path)
{
    char line[512];
    unsigned number = 0;
    const char *bad;
    char *name;
    char *eq;
    FILE *f;
    int r = 0;
    size_t n;

    f = fopen(path, "r");
    if (!f)
        return errno == ENOENT ? 0 : -errno;
    while (fgets(line, sizeof(line), f)) {
        number++;
        n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            fprintf(stderr, "term: %s:%u: line too long\n", path, number);
            while (fgets(line, sizeof(line), f) && line[strlen(line) - 1] != '\n')
                ;
            continue;
        }
        name = trim(line);
        if (*name == '\0' || *name == '#')
            continue;
        eq = strchr(name, '=');
        if (!eq) {
            fprintf(stderr, "term: %s:%u: no `=`\n", path, number);
            continue;
        }
        *eq = '\0';
        name = trim(name);
        bad = conf_set(c, name, trim(eq + 1));
        if (bad)
            fprintf(stderr, "term: %s:%u: %s: %s\n", path, number, name, bad);
    }
    if (ferror(f))
        r = -EIO;
    fclose(f);
    return r;
}

/* Everything the loop works with. */
struct term {
    struct term_font font;
    struct term_draw draw;
    struct term_keys keys;
    struct term_select sel;
    struct term_screen screen;
    struct term_pty pty;
    const char *conf; /* the settings file, where a change of scale is kept */
    unsigned cols;    /* the screen's size in cells */
    unsigned rows;
    bool suspended;    /* a descendant has the screen and the keyboard */
    bool dirty;        /* the screen changed since the last frame */
    bool output_ended; /* the master read end of file */
    bool ended;        /* the child has exited and been reaped */
};

/**
 * Read a burst of the child's output, at most TERM_BURST bytes, onto the
 * screen. Returns 0, or a negative errno value; sets `output_ended` at the
 * end of the child's output.
 */
static int read_output(struct term *t)
{
    char buf[4096];
    size_t total = 0;
    ssize_t n;

    while (total < TERM_BURST) {
        n = term_pty_read(&t->pty, buf, sizeof(buf));
        if (n == -EAGAIN || n == -EINTR)
            return 0;
        if (n < 0)
            return (int)n;
        if (n == 0) {
            t->output_ended = true;
            return 0;
        }
        term_screen_input(&t->screen, buf, (size_t)n);
        t->dirty = true;
        total += (size_t)n;
    }
    return 0;
}

/**
 * Read one event from /fb: a descendant took the screen, or gave it back,
 * when term draws everything again and forgets the keys it saw held.
 */
static void read_fb_event(struct term *t)
{
    struct fb_event ev;

    if (read(t->draw.fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        return;
    if (ev.type == FB_EVENT_LOST) {
        t->suspended = true;
    } else if (ev.type == FB_EVENT_REGAINED) {
        t->suspended = false;
        term_keys_reset(&t->keys);
        t->draw.age = 0;
        t->dirty = true;
    }
}

/**
 * Follow a change of the cells the screen holds, as when the font's scale
 * changed: tell libtsm and the pty, which sends SIGWINCH to the child.
 */
static void follow_size(struct term *t)
{
    unsigned cols;
    unsigned rows;

    term_draw_cells(&t->draw, &cols, &rows);
    if (cols == 0 || rows == 0 || (cols == t->cols && rows == t->rows))
        return;
    if (term_screen_resize(&t->screen, cols, rows) < 0)
        return;
    term_pty_resize(&t->pty, cols, rows);
    t->cols = cols;
    t->rows = rows;
    t->draw.age = 0;
    t->dirty = true;
}

/**
 * The loop: poll the signal pipe, the master, the console and /fb, and
 * serve each, until the child exits or a signal ends term.
 *
 * The keys are read before the output in each round, and the output a burst
 * at a time, so that typing reaches the child while it floods the screen. A
 * frame is drawn after each round that changed the screen, and when the
 * bell's end or the hold's bound comes, unless a descendant has the screen.
 *
 * Returns the exit status term exits with.
 */
static int loop(struct term *t, int sigfd)
{
    enum { P_SIG, P_MASTER, P_KEYS, P_FB, P_COUNT };
    struct pollfd p[P_COUNT];
    unsigned bits;
    int status;
    int timeout;
    int r;

    for (;;) {
        p[P_SIG].fd = sigfd;
        p[P_SIG].events = POLLIN;
        p[P_MASTER].fd = t->output_ended ? -1 : t->pty.master;
        p[P_MASTER].events = (short)(POLLIN | (t->pty.queued ? POLLOUT : 0));
        p[P_KEYS].fd = t->suspended ? -1 : t->keys.fd;
        p[P_KEYS].events = POLLIN;
        p[P_FB].fd = t->draw.fd;
        p[P_FB].events = POLLIN;
        /* Drawing's timeout is the bell's end or the hold's bound. */
        timeout = t->suspended ? -1 : term_draw_timeout(&t->draw);
        if (t->dirty && !t->suspended && term_screen_held(&t->screen) &&
            (timeout < 0 || timeout > TERM_HOLD_POLL_MS))
            timeout = TERM_HOLD_POLL_MS;
        for (r = 0; r < P_COUNT; r++)
            p[r].revents = 0;

        r = poll(p, P_COUNT, timeout);
        if (r < 0 && errno != EINTR) {
            perror("term: poll");
            return 1;
        }

        bits = term_signals_take();
        if (bits & TERM_SIG_QUIT)
            return 1;
        if (bits & TERM_SIG_CHILD) {
            r = term_pty_reap(&t->pty, &status);
            if (r > 0) {
                t->ended = true;
                return term_status(status);
            }
        }

        if (p[P_FB].revents & POLLIN)
            read_fb_event(t);
        if (p[P_KEYS].revents & (POLLIN | POLLHUP | POLLERR)) {
            r = term_keys_read(&t->keys, &t->screen, &t->sel);
            if (r < 0 && r != -EAGAIN && r != -EINTR) {
                fprintf(stderr, "term: keys: %s\n", strerror(-r));
                return 1;
            }
            /* The scrollback and the selection change what is shown. */
            t->dirty = true;
            if (t->keys.scale) {
                term_draw_rescale(&t->draw, t->keys.scale, t->conf);
                t->keys.scale = 0;
            }
        }
        if (p[P_MASTER].revents & POLLOUT)
            term_pty_flush(&t->pty);
        if (p[P_MASTER].revents & (POLLIN | POLLHUP | POLLERR)) {
            r = read_output(t);
            if (r < 0) {
                fprintf(stderr, "term: pty: %s\n", strerror(-r));
                return 1;
            }
        }

        if (t->suspended)
            continue;
        follow_size(t);
        if (t->screen.bell) {
            term_draw_bell(&t->draw);
            t->screen.bell = false;
        }
        if (t->dirty || term_draw_timeout(&t->draw) == 0) {
            term_draw_frame(&t->draw, &t->screen);
            t->dirty = term_screen_held(&t->screen);
        }
    }
}

/**
 * Take the screen and the keyboard, start the child, run the loop, and give
 * everything back.
 *
 * Returns the exit status term exits with: the child's, as term_status()
 * gives it, or 0 under -l once the child has exited; 1 when term failed or
 * a signal ended it.
 */
static int run(const struct term_options *o, const struct term_conf *conf)
{
    struct term t;
    int status = 1;
    int sigfd;
    int r;

    memset(&t, 0, sizeof(t));
    t.draw.fd = -1;
    t.keys.fd = -1;
    t.keys.saved_mode = -1;
    t.pty.master = -1;
    t.pty.child = -1;
    t.conf = o->conf;

    sigfd = term_signals_open();
    if (sigfd < 0) {
        fprintf(stderr, "term: signals: %s\n", strerror(-sigfd));
        return 1;
    }
    r = term_font_load(&t.font, conf->font, conf->scale);
    if (r < 0) {
        fprintf(stderr, "term: font: %s: %s\n", conf->font, strerror(-r));
        goto out_signals;
    }
    r = term_draw_open(&t.draw, &t.font);
    if (r < 0) {
        fprintf(stderr, "term: /fb: %s\n", strerror(-r));
        goto out_font;
    }
    term_draw_cells(&t.draw, &t.cols, &t.rows);
    if (t.cols == 0 || t.rows == 0) {
        fprintf(stderr, "term: /fb: too small for one cell\n");
        goto out_draw;
    }
    r = term_keys_open(&t.keys, TERM_CONSOLE_PATH);
    if (r < 0) {
        fprintf(stderr, "term: %s: %s\n", TERM_CONSOLE_PATH, strerror(-r));
        goto out_draw;
    }
    r = term_screen_init(&t.screen, &t.pty, t.cols, t.rows);
    if (r < 0) {
        fprintf(stderr, "term: screen: %s\n", strerror(-r));
        goto out_keys;
    }
    term_screen_scrollback(&t.screen, conf->scrollback);
    r = term_pty_open(&t.pty, t.cols, t.rows, o->argv, o->login);
    if (r < 0) {
        fprintf(stderr, "term: %s: %s\n",
                o->argv    ? o->argv[0]
                : o->login ? "/bin/login"
                           : "shell",
                strerror(-r));
        goto out_screen;
    }
    t.dirty = true;
    status = loop(&t, sigfd);
    /* A login session's status is its shell's last command's, which says
     * nothing of term; init reads 0 as a session that ended. */
    if (o->login && t.ended)
        status = 0;
    if (t.pty.child > 0)
        kill(t.pty.child, SIGHUP);

    term_pty_close(&t.pty);
out_screen:
    term_screen_free(&t.screen);
out_keys:
    term_keys_close(&t.keys);
out_draw:
    term_draw_close(&t.draw);
out_font:
    term_font_free(&t.font);
out_signals:
    term_signals_close();
    return status;
}

/** Print the usage line to `f`. */
static void usage(FILE *f)
{
    fprintf(f, "usage: term [-l | -H] [-c FILE] [PROGRAM [ARG...]]\n"
               "       term -V\n");
}

/**
 * Read the options into `o`.
 *
 * Returns the index of the first operand, -1 after printing a usage error,
 * or -2 when -V or -h has been answered.
 */
static int parse(int argc, char **argv, struct term_options *o)
{
    int i;

    o->login = false;
    o->host = false;
    o->conf = TERM_CONF_PATH;
    o->argv = NULL;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "--") == 0)
            return i + 1;
        if (strcmp(argv[i], "-V") == 0) {
            printf("libtsm %s\n", TSM_VERSION);
            return -2;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return -2;
        } else if (strcmp(argv[i], "-l") == 0) {
            o->login = true;
        } else if (strcmp(argv[i], "-H") == 0) {
            o->host = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "term: -c: needs a file\n");
                usage(stderr);
                return -1;
            }
            o->conf = argv[++i];
        } else {
            fprintf(stderr, "term: %s: unknown option\n", argv[i]);
            usage(stderr);
            return -1;
        }
    }
    return i;
}

int main(int argc, char **argv)
{
    struct term_options o;
    struct term_conf conf;
    int first;
    int r;

    first = parse(argc, argv, &o);
    if (first == -2)
        return 0;
    if (first < 0)
        return 2;
    if (o.login && o.host) {
        fprintf(stderr, "term: -l and -H exclude each other\n");
        return 2;
    }
    if (o.login && first < argc) {
        fprintf(stderr, "term: -l takes no program\n");
        return 2;
    }
    if (first < argc)
        o.argv = argv + first;

    term_conf_defaults(&conf);
    r = term_conf_read(&conf, o.conf);
    if (r < 0)
        fprintf(stderr, "term: %s: %s\n", o.conf, strerror(-r));
    if (o.host)
        return term_host_run(&o, &conf);
    return run(&o, &conf);
}
