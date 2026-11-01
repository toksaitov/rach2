/**
 * Host test of term's keys, through term -H and its pty: xterm's bytes for
 * the cases of the key table, the Kitty keyboard protocol's, and term's own
 * keys for the scrollback, the selection and paste.
 *
 * host-deps: term/pty.c term/screen.c term/host.c term/draw.c term/font.c
 * host-deps: term/unifont.c term/keys.c term/keymap.c term/select.c
 * host-deps: tests/host/keytable.c tests/host/compat/fixdir_host.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-render.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-screen.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-selection.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-unicode.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte-charsets.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/shared/shl-htable.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.c
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/tsm
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/shared
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external -I rach2/include
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external/wcwidth
 * host-cflags: -include vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.h
 * host-cflags: -Dwcwidth=tsm_wcwidth -Dtypeof=__typeof__
 * host-inputs: tests/host/keytable.txt
 *
 * main.c is included whole, its main renamed, and term_main runs in a child
 * of the test with pipes for its standard input and output, as `term -H -c
 * CONF sh -c SCRIPT`. The script puts its pty in raw mode, writes the modes a
 * group of cases needs, which libtsm's parser takes from its output, and
 * then reads its input away. term -H's `key` command gives term the key
 * events and answers with the bytes term wrote to the pty for them; the
 * `keymap` command changes term -H's keymap as `loadkeys` changes the
 * console's.
 *
 * Every case of tests/host/keytable.txt runs but those of its `tty` modes,
 * which are the kernel's line discipline's, not term's: under DECCKM and
 * DECKPAM as the case's modes say, with Num Lock and Caps Lock set by the
 * `locks` command first. The Kitty cases: Shift+Enter with flag 1, Russian
 * Ctrl+C on a two-layout map with flag 1 and with flags 1 and 4, Ctrl+Shift+C
 * there with its shifted key, and the repeat and release events of flag 2.
 * The keymap's entries and strings are read as each key is pressed, a press
 * of a key already down is a repeat, and Caps Lock changes the locks. One
 * child reads the bytes of a Cyrillic letter and of Shift+Enter from its pty
 * and prints them, so that the bytes `key` reports are those the program
 * gets. Shift with Page Up and Page Down scrolls the scrollback a page, and
 * a key the child gets shows the screen again; on the alternate screen the
 * child gets them. Print Screen's selection copies one line and two, and
 * Shift+Print Screen pastes, bracketed under mode 2004, with CR for LF.
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
#include "keytable.h"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* Linux's KEY_* codes of the keys the cases below press by number. */
#define K_ESC       1
#define K_ENTER     28
#define K_LEFTCTRL  29
#define K_A         30
#define K_LEFTSHIFT 42
#define K_C         46
#define K_SPACE     57
#define K_CAPSLOCK  58
#define K_F1        59
#define K_SYSRQ     99
#define K_HOME      102
#define K_UP        103
#define K_PAGEUP    104
#define K_RIGHT     106
#define K_END       107
#define K_DOWN      108
#define K_PAGEDOWN  109
#define K_RIGHTMETA 126

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
        perror("term_keys_hosttest: pipe");
        exit(1);
    }
    fflush(NULL);
    r->pid = fork();
    if (r->pid < 0) {
        perror("term_keys_hosttest: fork");
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
        perror("term_keys_hosttest: fdopen");
        exit(1);
    }
}

/**
 * Start term -H with settings `conf` on a child that runs `script` after
 * putting its pty in raw mode and that prints `ready` once it has, and wait
 * for it.
 */
static void start_script(struct run *r, const char *conf, const char *script)
{
    char *argv[] = {"term", "-H", "-c", (char *)conf, "/bin/sh", "-c", (char *)script, NULL};
    char got[256];

    start(r, argv);
    fprintf(r->to, "wait ready\n");
    fflush(r->to);
    if (!fgets(got, sizeof(got), r->from) || strncmp(got, "found ", 6) != 0) {
        fprintf(stderr, "term_keys_hosttest: the child did not start: %s", got);
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

/** Read the answer to `dump` and keep row `y`'s text in `row`. */
static void dump_row(struct run *r, unsigned y, char *row, size_t size)
{
    char line[4096];
    unsigned rows = 0;
    unsigned i;

    say(r, "dump");
    hear(r, line, sizeof(line));
    if (sscanf(line, "screen %*u %u", &rows) != 1) {
        CHECK(0, "dump: no screen line");
        row[0] = '\0';
        return;
    }
    for (i = 0; i < rows; i++) {
        hear(r, line, sizeof(line));
        if (i == y)
            snprintf(row, size, "%s", line);
    }
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
            perror("term_keys_hosttest: waitpid");
            exit(1);
        }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/** Write `n` bytes into `out` as term -H's answers escape them. */
static void escape(const unsigned char *bytes, int n, char *out, size_t size)
{
    size_t len = 0;
    int i;

    out[0] = '\0';
    for (i = 0; i < n && len + 5 < size; i++) {
        if (bytes[i] == '\\')
            len += (size_t)snprintf(out + len, size - len, "\\\\");
        else if (bytes[i] >= 0x20 && bytes[i] < 0x7F)
            out[len++] = (char)bytes[i];
        else
            len += (size_t)snprintf(out + len, size - len, "\\x%02x", bytes[i]);
    }
    out[len] = '\0';
}

/** The key table cases' child: modes `modes` printed, input read away. */
static void table_script(char *buf, size_t size, unsigned modes)
{
    fix_format(buf, size, "stty raw -echo; printf '%s%sready'; exec cat >/dev/null",
               modes & KEYTABLE_CKM ? "\\033[?1h" : "", modes & KEYTABLE_KPAM ? "\\033=" : "");
}

/**
 * Every case of the key table but the line discipline's, a run of term -H
 * for each change of DECCKM and DECKPAM.
 */
static void test_table(const char *conf)
{
    struct keytable t;
    struct keytable_case c;
    struct run r;
    char script[256];
    char line[1024];
    char want[1024];
    char got[1024];
    char what[2048];
    unsigned screen_modes = 0;
    bool running = false;
    size_t len;
    int cases = 0;
    int n;
    int i;

    fprintf(stderr, "key table\n");
    if (keytable_open(&t, KEYTABLE_PATH) < 0) {
        fprintf(stderr, "term_keys_hosttest: %s\n", t.error);
        exit(1);
    }
    while ((n = keytable_next(&t, &c)) == 1) {
        if (c.modes & KEYTABLE_TTY)
            continue;
        if (!running || (c.modes & (KEYTABLE_CKM | KEYTABLE_KPAM)) != screen_modes) {
            if (running)
                CHECK(finish(&r) == 129, "the table's child ends by its hang-up");
            screen_modes = c.modes & (KEYTABLE_CKM | KEYTABLE_KPAM);
            table_script(script, sizeof(script), screen_modes);
            start_script(&r, conf, script);
            running = true;
        }
        snprintf(line, sizeof(line), "locks %u",
                 ((c.modes & KEYTABLE_NUM) ? 2u : 0u) | ((c.modes & KEYTABLE_CAPS) ? 4u : 0u));
        say(&r, line);
        hear(&r, got, sizeof(got));
        len = (size_t)snprintf(line, sizeof(line), "key");
        for (i = 0; i < c.nevents; i++)
            len += (size_t)snprintf(line + len, sizeof(line) - len, " %c%u",
                                    c.events[i].press ? '+' : '-', c.events[i].code);
        escape(c.bytes, c.kind == KEYTABLE_BYTES ? c.nbytes : 0, got, sizeof(got));
        snprintf(want, sizeof(want), "sent %s", got);
        say(&r, line);
        hear(&r, got, sizeof(got));
        if (strcmp(got, want) != 0) {
            keytable_describe(&c, line, sizeof(line));
            snprintf(what, sizeof(what), "%s: answered \"%s\"", line, got);
            CHECK(0, what);
        }
        cases++;
    }
    if (n < 0) {
        fprintf(stderr, "term_keys_hosttest: %s\n", t.error);
        fails++;
    }
    keytable_close(&t);
    if (running)
        CHECK(finish(&r) == 129, "the table's child ends by its hang-up");
    CHECK(cases > 10000, "the table gives its cases");
    fprintf(stderr, "  %d cases\n", cases);
}

/**
 * Give term -H's keymap a second layout, as ckbcomp writes a Russian one
 * beside US: C's Cyrillic letters on the ShiftL tables, 16 and 17, Ctrl+C's
 * ETX on 20, and the switch, ShiftL_Lock, on Right Meta.
 */
static void russian(struct run *r)
{
    ask(r, "keymap 0 126 0x0A04", "ok");
    ask(r, "keymap 16 46 0xF441", "ok");
    ask(r, "keymap 17 46 0xF421", "ok");
    ask(r, "keymap 20 46 0x0003", "ok");
}

/** The Kitty keyboard protocol's cases, each set of flags a run of its own. */
static void test_kitty(const char *conf)
{
    struct run r;

    fprintf(stderr, "kitty\n");
    /* Flag 1, disambiguate. */
    start_script(&r, conf, "stty raw -echo; printf '\\033[>1uready'; exec cat >/dev/null");
    russian(&r);
    ask(&r, "key +42 28 -42", "sent \\x1b[13;2u");
    ask(&r, "key 28", "sent \\x0d");
    ask(&r, "key +29 46 -29", "sent \\x1b[99;5u");
    ask(&r, "key +29 +42 30 -42 -29", "sent \\x1b[97;6u");
    ask(&r, "key 1", "sent \\x1b[27u");
    ask(&r, "key 126", "sent ");
    ask(&r, "key 46", "sent \\xd1\\x81");
    ask(&r, "key +29 46 -29", "sent \\x1b[1089;5u");
    ask(&r, "key +30", "sent a");
    ask(&r, "key -30", "sent ");
    CHECK(finish(&r) == 129, "the flag 1 child ends by its hang-up");

    /* Flags 1 and 4: the shifted and the base layout's keys. */
    start_script(&r, conf, "stty raw -echo; printf '\\033[=5uready'; exec cat >/dev/null");
    russian(&r);
    ask(&r, "key 126 +29 46 -29 126", "sent \\x1b[1089::99;5u");
    ask(&r, "key 126 +42 +29 46 -29 -42 126", "sent \\x1b[1089:1057:99;6u");
    ask(&r, "key +29 46 -29", "sent \\x1b[99;5u");
    CHECK(finish(&r) == 129, "the flags 5 child ends by its hang-up");

    /* Flags 1 and 2: repeats, a second press of a key down, and releases. */
    start_script(&r, conf, "stty raw -echo; printf '\\033[=3uready'; exec cat >/dev/null");
    ask(&r, "key +30", "sent a");
    ask(&r, "key +30", "sent a");
    ask(&r, "key -30", "sent \\x1b[97;1:3u");
    ask(&r, "key +29 +46", "sent \\x1b[99;5u");
    ask(&r, "key +46", "sent \\x1b[99;5:2u");
    ask(&r, "key -46 -29", "sent \\x1b[99;5:3u");
    ask(&r, "key +42 +28", "sent \\x1b[13;2u");
    ask(&r, "key -28 -42", "sent \\x1b[13;2:3u");
    ask(&r, "key 28", "sent \\x0d");
    ask(&r, "key 103", "sent \\x1b[A\\x1b[1;1:3A");
    CHECK(finish(&r) == 129, "the flags 3 child ends by its hang-up");
}

/**
 * The keymap read as keys are pressed, repeats, Caps Lock's lock, and the
 * bytes of a Cyrillic letter and of Shift+Enter as the child reads them.
 */
static void test_keymap(const char *conf)
{
    struct run r;

    fprintf(stderr, "keymap\n");
    start_script(&r, conf, "stty raw -echo; printf ready; exec cat >/dev/null");
    ask(&r, "key +30 +30 +30 -30", "sent aaa");
    ask(&r, "keymap 0 30 0x0062", "ok");
    ask(&r, "key 30", "sent b");
    ask(&r, "string 0 hello", "ok");
    ask(&r, "key 59", "sent hello");
    ask(&r, "string 0 \\eOP", "ok");
    ask(&r, "key +42 59 -42", "sent \\x1b[1;2P");
    ask(&r, "locks", "locks 0");
    ask(&r, "key 58", "sent ");
    ask(&r, "locks", "locks 4");
    ask(&r, "key 31", "sent S");
    ask(&r, "keymap 300 1 1", "error keymap: needs a table, a key code and an entry");
    ask(&r, "key +999", "error key: needs events +CODE, -CODE or CODE");
    CHECK(finish(&r) == 129, "the keymap's child ends by its hang-up");

    /* The child reads nine bytes and prints them in hex, then exits. */
    start_script(&r, conf,
                 "stty raw -echo; printf '\\033[>1uready\\r\\n'; "
                 "dd bs=1 count=9 2>/dev/null | od -An -tx1 -v | tr -d ' \\n'; exit 3");
    russian(&r);
    ask(&r, "key 126 46 126", "sent \\xd1\\x81");
    ask(&r, "key +42 28 -42", "sent \\x1b[13;2u");
    say(&r, "wait d1811b5b31333b3275");
    {
        char got[256];

        hear(&r, got, sizeof(got));
        CHECK(strncmp(got, "found ", 6) == 0, "the child reads the bytes the keys sent");
    }
    ask(&r, "exit", "exit 3");
    CHECK(finish(&r) == 3, "the reading child's status");
}

/** Shift with Page Up and Page Down, and the alternate screen's. */
static void test_scrollback(const char *conf)
{
    struct run r;
    char row[4096];

    fprintf(stderr, "scrollback\n");
    start_script(&r, conf,
                 "stty raw -echo; i=1; while [ $i -le 40 ]; do printf 'line %d\\r\\n' $i; "
                 "i=$((i + 1)); done; printf ready; exec cat >/dev/null");
    dump_row(&r, 0, row, sizeof(row));
    CHECK(strcmp(row, "line 18") == 0, "the screen's first row before the scrollback");
    ask(&r, "key +42 104 -42", "sent ");
    dump_row(&r, 0, row, sizeof(row));
    CHECK(strcmp(row, "line 1") == 0, "Shift+Page Up shows the scrollback");
    dump_row(&r, 17, row, sizeof(row));
    CHECK(strcmp(row, "line 18") == 0, "the scrollback above the screen");
    ask(&r, "key +42 109 -42", "sent ");
    dump_row(&r, 0, row, sizeof(row));
    CHECK(strcmp(row, "line 18") == 0, "Shift+Page Down shows the screen");
    ask(&r, "key +42 104 -42", "sent ");
    ask(&r, "key 30", "sent a");
    dump_row(&r, 0, row, sizeof(row));
    CHECK(strcmp(row, "line 18") == 0, "a key for the child shows the screen");
    CHECK(finish(&r) == 129, "the scrollback's child ends by its hang-up");

    start_script(&r, conf,
                 "stty raw -echo; printf 'a\\r\\nb\\r\\n\\033[?1049hready'; exec cat >/dev/null");
    ask(&r, "key +42 104 -42", "sent \\x1b[5;2~");
    ask(&r, "key +42 109 -42", "sent \\x1b[6;2~");
    CHECK(finish(&r) == 129, "the alternate screen's child ends by its hang-up");
}

/** Print Screen's selection, the clipboard and paste. */
static void test_selection(const char *conf)
{
    struct run r;

    fprintf(stderr, "selection\n");
    start_script(&r, conf, "stty raw -echo; printf 'hello world\\r\\nready'; exec cat >/dev/null");
    /* On, up a row, to its start, mark, four to the right, copy. */
    ask(&r, "key 99 103 102 57 106 106 106 106 28", "sent ");
    ask(&r, "clipboard", "clipboard hello");
    ask(&r, "key +42 99 -42", "sent hello");
    /* Two lines, the second to its end; Escape leaves with no copy. */
    ask(&r, "key 99 103 102 57 108 107 28", "sent ");
    ask(&r, "clipboard", "clipboard hello world\\x0aready");
    ask(&r, "key 99 103 57 1", "sent ");
    ask(&r, "clipboard", "clipboard hello world\\x0aready");
    /* While the mode is on, the keys are its own; Print Screen leaves it. */
    ask(&r, "key 99 30 31 99", "sent ");
    ask(&r, "key 30", "sent a");
    ask(&r, "key +42 99 -42", "sent hello world\\x0dready");
    CHECK(finish(&r) == 129, "the selection's child ends by its hang-up");

    start_script(&r, conf,
                 "stty raw -echo; printf '\\033[?2004hhello\\r\\nready'; exec cat >/dev/null");
    ask(&r, "key 99 103 102 57 106 106 106 106 28", "sent ");
    ask(&r, "key +42 99 -42", "sent \\x1b[200~hello\\x1b[201~");
    CHECK(finish(&r) == 129, "the bracketed paste's child ends by its hang-up");
}

int main(void)
{
    const char *conf = fix_path("term.conf");
    FILE *f = fopen(conf, "w");

    /* A term that has exited must not end the test as it writes. */
    signal(SIGPIPE, SIG_IGN);
    if (!f) {
        perror("term_keys_hosttest: term.conf");
        return 1;
    }
    fputs("scrollback = 100\n", f);
    fclose(f);

    test_table(conf);
    test_kitty(conf);
    test_keymap(conf);
    test_scrollback(conf);
    test_selection(conf);

    if (fails) {
        fprintf(stderr, "term_keys_hosttest: %d failed\n", fails);
        return fails > 255 ? 255 : fails;
    }
    fix_passed();
    fprintf(stderr, "term_keys_hosttest: passed\n");
    return 0;
}
