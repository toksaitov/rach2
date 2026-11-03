/**
 * Host test of term's pty, screen and settings, through term -H: a child's
 * round trip from its output on a host pty, through libtsm's screen, to the
 * commands that read the screen back, and from the commands to its input.
 *
 * host-deps: term/pty.c term/screen.c term/host.c term/draw.c term/font.c
 * host-deps: term/unifont.c term/keys.c term/keymap.c term/select.c term/bell.c
 * host-deps: tests/host/compat/fixdir_host.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-render.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-screen.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-selection.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-unicode.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-vte-charsets.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-vte.c
 * host-deps: build/host/libtsm-4.8.0/src/shared/shl-htable.c
 * host-deps: build/host/libtsm-4.8.0/external/wcwidth/wcwidth.c
 * host-cflags: -I build/host/libtsm-4.8.0/src/tsm
 * host-cflags: -I build/host/libtsm-4.8.0/src/shared
 * host-cflags: -I build/host/libtsm-4.8.0/external -I rach2/include
 * host-cflags: -I build/host/libtsm-4.8.0/external/wcwidth
 * host-cflags: -include build/host/libtsm-4.8.0/external/wcwidth/wcwidth.h
 * host-cflags: -Dwcwidth=tsm_wcwidth -Dtypeof=__typeof__
 *
 * main.c is included whole, its main renamed, and term_main runs in a
 * child of the test with pipes for its standard input and output, as
 * `term -H -c CONF sh -c SCRIPT`. The test writes commands and reads each
 * answer; every wait is for text on the screen, never for time.
 *
 * The scripts check that the child starts on a pty of 80 by 24 cells with
 * TERM=xterm-256color, that bytes sent reach its input, that its output
 * lands on the screen with its colors, cursor and wide characters, that the
 * parser's reply to a cursor position report reaches it, that a resize
 * reaches it as `stty size`, that its exit status comes back, that the end
 * of standard input hangs it up, and that a program that cannot be run
 * fails term. The settings file is read with each kind of bad line.
 *
 * libtsm is compiled from the i686 tree the build extracted and patched, as
 * build_libtsm.sh compiles it: wcwidth renamed, and typeof, a GNU keyword
 * its list header uses, spelled as the strict dialect accepts it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define main term_main
#include "../../term/main.c"
#undef main

#include "fixdir_host.h"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* A term -H running in a child of the test. */
struct run {
    pid_t pid;
    FILE *to;   /* its standard input */
    FILE *from; /* its standard output */
};

/**
 * Start term_main(argv) in a child with pipes on its standard input and
 * output. Ends the test when the child cannot be started.
 */
static void start(struct run *r, char **argv)
{
    int in[2];
    int out[2];
    int argc = 0;

    while (argv[argc])
        argc++;
    if (pipe(in) < 0 || pipe(out) < 0) {
        perror("term_hosttest: pipe");
        exit(1);
    }
    fflush(NULL);
    r->pid = fork();
    if (r->pid < 0) {
        perror("term_hosttest: fork");
        exit(1);
    }
    if (r->pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        /* _exit: the test's exit handlers, which judge its fixtures, are
           not the child's. */
        argc = term_main(argc, argv);
        fflush(NULL);
        _exit(argc);
    }
    close(in[0]);
    close(out[1]);
    r->to = fdopen(in[1], "w");
    r->from = fdopen(out[0], "r");
    if (!r->to || !r->from) {
        perror("term_hosttest: fdopen");
        exit(1);
    }
}

/** Send one command line. */
static void say(struct run *r, const char *line)
{
    fprintf(r->to, "%s\n", line);
    fflush(r->to);
}

/** Read one answer line into `buf`, without its newline; "" at the end. */
static void hear(struct run *r, char *buf, size_t size)
{
    size_t n;

    if (!fgets(buf, (int)size, r->from)) {
        buf[0] = '\0';
        return;
    }
    n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
}

/** Send `line` and check that its answer is `want`. */
static void ask(struct run *r, const char *line, const char *want)
{
    char got[4096];
    char what[8192];

    say(r, line);
    hear(r, got, sizeof(got));
    snprintf(what, sizeof(what), "%s: answered \"%s\", not \"%s\"", line, got, want);
    CHECK(strcmp(got, want) == 0, what);
}

/** Send `line` and check that its answer begins with `want`. */
static void ask_prefix(struct run *r, const char *line, const char *want)
{
    char got[4096];
    char what[8192];

    say(r, line);
    hear(r, got, sizeof(got));
    snprintf(what, sizeof(what), "%s: answered \"%s\", not \"%s...\"", line, got, want);
    CHECK(strncmp(got, want, strlen(want)) == 0, what);
}

/**
 * Close the child's standard input and return term's exit status, after
 * checking that it wrote nothing more.
 */
static int finish(struct run *r)
{
    char rest[4096];
    int status;

    fclose(r->to);
    hear(r, rest, sizeof(rest));
    CHECK(rest[0] == '\0', "term -H wrote after its last answer");
    fclose(r->from);
    while (waitpid(r->pid, &status, 0) < 0)
        if (errno != EINTR) {
            perror("term_hosttest: waitpid");
            exit(1);
        }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/**
 * A child that reads a line, shows colors, a wide character, the cursor, a
 * cursor position report and its size, and exits 7.
 */
static void test_round_trip(const char *conf)
{
    static const char script[] = "printf 'term=%s\\n' \"$TERM\"; "
                                 "read line; printf 'got:%s\\n' \"$line\"; "
                                 "printf '\\033[1;31mX\\033[0m\\n'; "
                                 "printf '\\344\\270\\255.\\n'; "
                                 "stty -icanon -echo min 1; "
                                 "printf '\\033[20;7H\\033[6n'; "
                                 "reply=$(dd bs=1 count=7 2>/dev/null | tr '\\033' E); "
                                 "stty icanon echo; "
                                 "printf '\\033[22;1Hcpr:%s\\n' \"$reply\"; "
                                 "read line; "
                                 "printf 'size:%s\\n' \"$(stty size)\"; "
                                 "read line; exit 7";
    char *argv[] = {"term", "-H", "-c", (char *)conf, "/bin/sh", "-c", (char *)script, NULL};
    char screen[256];
    char row[4096];
    struct run r;
    unsigned y;

    fprintf(stderr, "round trip\n");
    start(&r, argv);
    ask(&r, "wait term=xterm-256color", "found 0");
    ask(&r, "send hello there\\r", "ok");
    ask(&r, "wait got:hello there", "found 2");
    ask(&r, "wait cpr:E[20;7R", "found 21");

    /* The bold red X, and the wide character's two cells. */
    ask_prefix(&r, "cell 0 3", "cell 0 3 1 U+0058 fg=");
    say(&r, "cell 0 3");
    hear(&r, row, sizeof(row));
    CHECK(strstr(row, " bold") != NULL, "the X is not bold");
    CHECK(strstr(row, "fg=e5e5e5") == NULL, "the X has the default foreground");
    ask_prefix(&r, "cell 0 4", "cell 0 4 2 U+4E2D ");
    ask_prefix(&r, "cell 1 4", "cell 1 4 0 - ");
    ask_prefix(&r, "cell 2 4", "cell 2 4 1 U+002E ");
    ask(&r, "cell 80 0", "error cell: off the screen");

    /* The dump: its header, the rows that hold text, and no others. */
    say(&r, "dump");
    hear(&r, screen, sizeof(screen));
    CHECK(strncmp(screen, "screen 80 24 0 22 1", 19) == 0, "the dump's header");
    for (y = 0; y < 24; y++) {
        hear(&r, row, sizeof(row));
        if (y == 4)
            CHECK(strcmp(row, "\344\270\255.") == 0, "row 4 holds the wide character");
        if (y == 21)
            CHECK(strcmp(row, "cpr:E[20;7R") == 0, "row 21 holds the reply");
        if (y > 4 && y != 21 && y < 24)
            CHECK(row[0] == '\0' || y == 22, "a row past the output is blank");
    }

    ask(&r, "resize 100 30", "ok");
    ask(&r, "send \\r", "ok");
    ask(&r, "wait size:30 100", "found 23");
    ask(&r, "send \\r", "ok");
    ask(&r, "exit", "exit 7");
    ask(&r, "wait nowhere", "missing");
    ask(&r, "bogus", "error bogus: unknown command");
    ask(&r, "send \\q", "error send: bad escape");
    ask(&r, "resize 0 5", "error resize: needs columns and rows from 1 to 1000");
    ask(&r, "clipboard", "clipboard ");
    CHECK(finish(&r) == 7, "term -H exits with the child's status");
}

/** The end of standard input hangs up the child; term returns its status. */
static void test_hangup(const char *conf)
{
    static const char script[] = "trap '' HUP; echo ready; read line; exit 5";
    char *argv[] = {"term", "-H", "-c", (char *)conf, "/bin/sh", "-c", (char *)script, NULL};
    struct run r;

    fprintf(stderr, "hang up\n");
    start(&r, argv);
    ask(&r, "wait ready", "found 0");
    CHECK(finish(&r) == 5, "the hung-up child's status");
}

/** A signal that ends term also reaps the program that owned its slave pty. */
static void test_terminate(const char *conf)
{
    static const char script[] = "printf 'child:%s\\n' $$; read line";
    char *argv[] = {"term", "-H", "-c", (char *)conf, "/bin/sh", "-c", (char *)script, NULL};
    struct run r;
    char text[4096];
    long child = -1;

    start(&r, argv);
    ask(&r, "wait child:", "found 0");
    say(&r, "dump");
    hear(&r, text, sizeof(text));
    for (unsigned y = 0; y < 24; y++) {
        hear(&r, text, sizeof(text));
        if (y == 0)
            CHECK(sscanf(text, "child:%ld", &child) == 1 && child > 1, "the child's pid");
    }
    CHECK(kill(r.pid, SIGTERM) == 0, "terminate term");
    CHECK(finish(&r) == 1, "term reports its interruption");
    if (child > 1)
        CHECK(kill((pid_t)child, 0) < 0 && errno == ESRCH, "term reaped its pty child");
}

/** A program that cannot be run fails term, which answers nothing. */
static void test_no_program(const char *conf)
{
    char *argv[] = {"term", "-H", "-c", (char *)conf, "/nonexistent/program", NULL};
    struct run r;

    fprintf(stderr, "no program\n");
    start(&r, argv);
    say(&r, "dump");
    CHECK(finish(&r) == 1, "a program that cannot be run fails term");
}

/** The settings file: good lines, each kind of bad line, and no file. */
static void test_conf(void)
{
    struct term_conf c;
    const char *path = fix_path("bad.conf");
    FILE *f;

    fprintf(stderr, "settings\n");
    term_conf_defaults(&c);
    CHECK(strcmp(c.font, TERM_FONT_PATH) == 0 && c.scale == 1 && c.scrollback == TERM_SCROLLBACK &&
              c.bell == TERM_BELL_AUDIBLE,
          "the defaults");
    CHECK(term_conf_read(&c, fix_path("absent.conf")) == 0, "a missing file is no error");
    CHECK(c.scale == 1, "a missing file changes nothing");

    f = fopen(path, "w");
    if (!f) {
        perror("term_hosttest: bad.conf");
        exit(1);
    }
    fputs("# term's settings\n"
          "\n"
          "  scale = 2  \r\n"
          "scrollback=250\n"
          "font = /fonts/u.font\n"
          "scale = 3\n"
          "scrollback = 100001\n"
          "scrollback = 12x\n"
          "font = relative.font\n"
          "bell = both\n"
          "bell = loud\n"
          "colour = blue\n"
          "no equals sign\n"
          "scale =\n",
          f);
    fclose(f);
    fprintf(stderr, "  (eight diagnostics follow)\n");
    CHECK(term_conf_read(&c, path) == 0, "a file with bad lines is read");
    CHECK(c.scale == 2, "scale = 2, and bad scales are skipped");
    CHECK(c.scrollback == 250, "scrollback=250, and bad counts are skipped");
    CHECK(strcmp(c.font, "/fonts/u.font") == 0, "font, and a relative path is skipped");
    CHECK(c.bell == (TERM_BELL_AUDIBLE | TERM_BELL_VISUAL),
          "bell = both, and a bad bell is skipped");
}

int main(void)
{
    const char *conf = fix_path("term.conf");
    FILE *f = fopen(conf, "w");

    /* A term that has exited must not end the test as it writes. */
    signal(SIGPIPE, SIG_IGN);
    if (!f) {
        perror("term_hosttest: term.conf");
        return 1;
    }
    fputs("scrollback = 10\n", f);
    fclose(f);

    test_conf();
    test_round_trip(conf);
    test_hangup(conf);
    test_terminate(conf);
    test_no_program(conf);

    if (fails) {
        fprintf(stderr, "term_hosttest: %d failed\n", fails);
        return 1;
    }
    fix_passed();
    fprintf(stderr, "term_hosttest: passed\n");
    return 0;
}
