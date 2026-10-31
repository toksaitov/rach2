/*
 * host.c: term -H, the child on a host pty with no screen, driven by
 * commands on standard input that send it bytes or keys, wait for text on
 * its screen and dump the screen's cells, for tests.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* kill */
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "term.h"

#define HOST_COLS     80
#define HOST_ROWS     24
#define HOST_LINE_MAX 65536 /* bytes of a command line, its newline included */

/* What a command that waits is waiting for. */
enum host_block {
    BLOCK_NONE,
    BLOCK_WAIT, /* `wait`: the text on the screen, or the output's end */
    BLOCK_EXIT, /* `exit`: the child's exit and the output's end */
};

struct host {
    struct term_screen screen;
    struct term_pty pty;
    struct term_keys keys;         /* the keys, through term -H's own keymap */
    struct term_select sel;        /* the selection made with them */
    char *tapped;                  /* the bytes the last `key` sent the child */
    size_t tapped_len;             /* bytes at `tapped` */
    size_t tapped_size;            /* bytes allocated */
    char *in;                      /* standard input not yet taken as commands */
    size_t in_len;                 /* bytes at `in` */
    size_t in_size;                /* bytes allocated */
    bool in_eof;                   /* standard input has ended */
    bool child_done;               /* the child has been reaped */
    int status;                    /* its wait status, once reaped */
    bool output_ended;             /* the master read end of file */
    enum host_block block;         /* the command that waits, or BLOCK_NONE */
    char wait_text[HOST_LINE_MAX]; /* what BLOCK_WAIT waits for */
};

/** The screen's tap: keep a copy of what the keys send the child. */
static void tap(void *data, const char *buf, size_t len)
{
    struct host *h = data;
    size_t size;
    char *p;

    if (h->tapped_len + len > h->tapped_size) {
        size = h->tapped_size ? h->tapped_size : 256;
        while (size < h->tapped_len + len)
            size *= 2;
        p = realloc(h->tapped, size);
        if (!p)
            return;
        h->tapped = p;
        h->tapped_size = size;
    }
    memcpy(h->tapped + h->tapped_len, buf, len);
    h->tapped_len += len;
}

/** Write `ucs4` in UTF-8 to `out`, which has room for 4 bytes. */
static size_t utf8_put(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c > 0x10FFFF)
        c = 0xFFFD;
    out[0] = (char)(0xF0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3F));
    out[2] = (char)(0x80 | (c >> 6 & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/** The value of hex digit `c`, or -1. */
static int hex_digit(char c)
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
 * Decode the escapes of `s` in place: `\\`, `\e`, `\r`, `\n`, `\t`, `\a`,
 * `\b` and `\xHH`. Returns the decoded length, or -1 for a bad escape.
 */
static long unescape(char *s)
{
    char *start = s;
    char *out = s;
    int hi;
    int lo;

    while (*s) {
        if (*s != '\\') {
            *out++ = *s++;
            continue;
        }
        s++;
        switch (*s) {
        case '\\':
            *out++ = '\\';
            break;
        case 'e':
            *out++ = '\033';
            break;
        case 'r':
            *out++ = '\r';
            break;
        case 'n':
            *out++ = '\n';
            break;
        case 't':
            *out++ = '\t';
            break;
        case 'a':
            *out++ = '\a';
            break;
        case 'b':
            *out++ = '\b';
            break;
        case 'x':
            hi = hex_digit(s[1]);
            lo = hi < 0 ? -1 : hex_digit(s[2]);
            if (lo < 0)
                return -1;
            *out++ = (char)(hi << 4 | lo);
            s += 2;
            break;
        default:
            return -1;
        }
        s++;
    }
    *out = '\0';
    return (long)(out - start);
}

/** Print `len` bytes of `text`, escaped as unescape() reads them. */
static void print_escaped(const char *text, size_t len)
{
    size_t i;
    unsigned char c;

    for (i = 0; i < len; i++) {
        c = (unsigned char)text[i];
        if (c == '\\')
            fputs("\\\\", stdout);
        else if (c >= 0x20 && c < 0x7F)
            putchar(c);
        else
            printf("\\x%02x", c);
    }
}

/**
 * The text of row `y` of `snap` in UTF-8, its trailing blanks removed, into
 * `out`, which has room for cols * TERM_CELL_CHARS * 4 + 1 bytes.
 */
static void row_text(const struct term_snapshot *snap, unsigned y, char *out)
{
    const struct term_cell *c = &snap->cells[(size_t)y * snap->cols];
    char *p = out;
    char *end = out;
    unsigned x;
    unsigned i;

    for (x = 0; x < snap->cols; x++, c++) {
        if (c->width == 0)
            continue;
        if (c->len == 0) {
            *p++ = ' ';
            continue;
        }
        for (i = 0; i < c->len; i++)
            p += utf8_put(c->ch[i], p);
        end = p;
    }
    *end = '\0';
}

/** Allocate room for one row's text. */
static char *row_alloc(const struct term_snapshot *snap)
{
    return malloc((size_t)snap->cols * TERM_CELL_CHARS * 4 + 1);
}

/**
 * The row on which the screen shows `text`, -1 when none does, or -2 when
 * memory ran out.
 */
static int find_text(struct host *h, const char *text)
{
    struct term_snapshot snap;
    char *row;
    int found = -1;
    unsigned y;

    if (term_screen_snapshot(&h->screen, &snap) < 0)
        return -2;
    row = row_alloc(&snap);
    if (!row) {
        term_snapshot_free(&snap);
        return -2;
    }
    for (y = 0; y < snap.rows && found < 0; y++) {
        row_text(&snap, y, row);
        if (strstr(row, text))
            found = (int)y;
    }
    free(row);
    term_snapshot_free(&snap);
    return found;
}

/** `dump`: the screen's size and cursor, then each row's text. */
static void cmd_dump(struct host *h)
{
    struct term_snapshot snap;
    char *row;
    unsigned y;

    if (term_screen_snapshot(&h->screen, &snap) < 0 || !(row = row_alloc(&snap))) {
        if (snap.cells)
            term_snapshot_free(&snap);
        puts("error out of memory");
        return;
    }
    printf("screen %u %u %u %u %d\n", snap.cols, snap.rows, snap.cursor_x, snap.cursor_y,
           snap.cursor_visible ? 1 : 0);
    for (y = 0; y < snap.rows; y++) {
        row_text(&snap, y, row);
        puts(row);
    }
    free(row);
    term_snapshot_free(&snap);
}

/** `cell X Y`: one cell's characters, colors and attributes. */
static void cmd_cell(struct host *h, const char *args)
{
    struct term_snapshot snap;
    const struct term_cell *c;
    const struct tsm_screen_attr *a;
    unsigned x;
    unsigned y;
    unsigned i;

    if (sscanf(args, "%u %u", &x, &y) != 2) {
        puts("error cell: needs a column and a row");
        return;
    }
    if (term_screen_snapshot(&h->screen, &snap) < 0) {
        puts("error out of memory");
        return;
    }
    if (x >= snap.cols || y >= snap.rows) {
        puts("error cell: off the screen");
        term_snapshot_free(&snap);
        return;
    }
    c = &snap.cells[(size_t)y * snap.cols + x];
    a = &c->attr;
    printf("cell %u %u %u ", x, y, c->width);
    if (c->len == 0)
        putchar('-');
    for (i = 0; i < c->len; i++)
        printf("%sU+%04X", i ? "+" : "", (unsigned)c->ch[i]);
    printf(" fg=%02x%02x%02x bg=%02x%02x%02x", a->fr, a->fg, a->fb, a->br, a->bg, a->bb);
    printf("%s%s%s%s%s%s%s\n", a->bold ? " bold" : "", a->dim ? " dim" : "",
           a->italic ? " italic" : "", a->underline ? " underline" : "", a->blink ? " blink" : "",
           a->inverse ? " inverse" : "", a->protect ? " protect" : "");
    term_snapshot_free(&snap);
}

/**
 * `key EVENT...`: give term's keys each event, `+CODE` a press, `-CODE` a
 * release and `CODE` both, with Linux's KEY_* codes, and answer with what
 * they sent the child.
 */
static void cmd_key(struct host *h, char *args)
{
    unsigned long code;
    char *word;
    char *end;
    char what;

    h->tapped_len = 0;
    h->screen.tap = tap;
    h->screen.tap_data = h;
    for (word = strtok(args, " "); word; word = strtok(NULL, " ")) {
        what = *word == '+' || *word == '-' ? *word++ : 't';
        code = strtoul(word, &end, 10);
        if (*word == '\0' || *end != '\0' || code > TERM_KEYS_MAX) {
            h->screen.tap = NULL;
            puts("error key: needs events +CODE, -CODE or CODE");
            return;
        }
        if (what != '-')
            term_keys_event(&h->keys, &h->screen, &h->sel, (unsigned)code, true);
        if (what != '+')
            term_keys_event(&h->keys, &h->screen, &h->sel, (unsigned)code, false);
    }
    h->screen.tap = NULL;
    fputs("sent ", stdout);
    print_escaped(h->tapped ? h->tapped : "", h->tapped_len);
    putchar('\n');
}

/** `keymap TABLE CODE VALUE`: set an entry of the keymap, as KDSKBENT does. */
static void cmd_keymap(struct host *h, const char *args)
{
    unsigned long table;
    unsigned long code;
    unsigned long value;
    char *end;
    int r;

    table = strtoul(args, &end, 0);
    code = strtoul(end, &end, 0);
    value = strtoul(end, &end, 0);
    if (*end != '\0' || table >= KM_MAX_TABLES || code >= KM_NR_KEYS || value > 0xFFFF) {
        puts("error keymap: needs a table, a key code and an entry");
        return;
    }
    r = term_keys_set_entry(&h->keys, (unsigned)table, (unsigned)code, (unsigned)value);
    if (r < 0)
        printf("error keymap: %s\n", strerror(-r));
    else
        puts("ok");
}

/** `string N TEXT`: set function string N, as KDSKBSENT does. */
static void cmd_string(struct host *h, char *args)
{
    unsigned long n;
    char *end;
    long len;
    int r;

    n = strtoul(args, &end, 10);
    if (end == args || (*end != ' ' && *end != '\0')) {
        puts("error string: needs a number and text");
        return;
    }
    if (*end == ' ')
        end++;
    len = unescape(end);
    if (len < 0 || (size_t)len != strlen(end) || n >= KM_MAX_FUNC) {
        puts("error string: needs a number below 256 and text with no NUL");
        return;
    }
    r = term_keys_set_string(&h->keys, (unsigned)n, end);
    if (r < 0)
        printf("error string: %s\n", strerror(-r));
    else
        puts("ok");
}

/** `locks [N]`: set the locks, KDSKBLED's bits, and answer with them. */
static void cmd_locks(struct host *h, const char *args)
{
    unsigned long v;
    char *end;

    if (*args) {
        v = strtoul(args, &end, 0);
        if (*end != '\0' || v > 7) {
            puts("error locks: needs a value from 0 to 7");
            return;
        }
        h->keys.state.ledflags = (unsigned)v;
    }
    printf("locks %u\n", h->keys.state.ledflags);
}

/**
 * Answer the command that waits, if what it waits for has come. Returns
 * true when it was answered.
 */
static bool settle(struct host *h)
{
    int row;

    if (h->block == BLOCK_WAIT) {
        row = find_text(h, h->wait_text);
        if (row == -2)
            puts("error out of memory");
        else if (row >= 0)
            printf("found %d\n", row);
        else if (h->output_ended)
            puts("missing");
        else
            return false;
    } else if (h->block == BLOCK_EXIT) {
        if (!h->child_done || !h->output_ended)
            return false;
        printf("exit %d\n", term_status(h->status));
    } else {
        return true;
    }
    h->block = BLOCK_NONE;
    fflush(stdout);
    return true;
}

/** Run one command line, `line`, without its newline. */
static void command(struct host *h, char *line)
{
    char *args = strchr(line, ' ');
    unsigned cols;
    unsigned rows;
    long n;
    int r;

    if (args)
        *args++ = '\0';
    else
        args = line + strlen(line);

    if (strcmp(line, "send") == 0) {
        n = unescape(args);
        if (n < 0) {
            puts("error send: bad escape");
        } else {
            r = term_pty_write(&h->pty, args, (size_t)n);
            if (r < 0)
                printf("error send: %s\n", strerror(-r));
            else
                puts("ok");
        }
    } else if (strcmp(line, "wait") == 0) {
        n = unescape(args);
        if (n <= 0 || (size_t)n != strlen(args)) {
            puts("error wait: needs text, with no NUL");
        } else {
            strcpy(h->wait_text, args);
            h->block = BLOCK_WAIT;
            settle(h);
            return;
        }
    } else if (strcmp(line, "dump") == 0) {
        cmd_dump(h);
    } else if (strcmp(line, "cell") == 0) {
        cmd_cell(h, args);
    } else if (strcmp(line, "resize") == 0) {
        if (sscanf(args, "%u %u", &cols, &rows) != 2 || cols == 0 || rows == 0 || cols > 1000 ||
            rows > 1000)
            puts("error resize: needs columns and rows from 1 to 1000");
        else if ((r = term_screen_resize(&h->screen, cols, rows)) < 0 ||
                 (r = term_pty_resize(&h->pty, cols, rows)) < 0)
            printf("error resize: %s\n", strerror(-r));
        else
            puts("ok");
    } else if (strcmp(line, "key") == 0) {
        cmd_key(h, args);
    } else if (strcmp(line, "keymap") == 0) {
        cmd_keymap(h, args);
    } else if (strcmp(line, "string") == 0) {
        cmd_string(h, args);
    } else if (strcmp(line, "locks") == 0) {
        cmd_locks(h, args);
    } else if (strcmp(line, "clipboard") == 0) {
        fputs("clipboard ", stdout);
        print_escaped(h->screen.clipboard ? h->screen.clipboard : "", h->screen.clipboard_len);
        putchar('\n');
    } else if (strcmp(line, "exit") == 0) {
        h->block = BLOCK_EXIT;
        settle(h);
        return;
    } else {
        printf("error %s: unknown command\n", line);
    }
    fflush(stdout);
}

/**
 * Run the complete command lines waiting in `in`, and at the end of
 * standard input the last line without its newline, until one waits.
 */
static void run_commands(struct host *h)
{
    size_t len;
    size_t used;
    char *nl;

    while (h->block == BLOCK_NONE && h->in_len > 0) {
        nl = memchr(h->in, '\n', h->in_len);
        if (!nl && !h->in_eof)
            return;
        len = nl ? (size_t)(nl - h->in) : h->in_len;
        used = nl ? len + 1 : len;
        h->in[len] = '\0';
        if (len > 0 && h->in[len - 1] == '\r')
            h->in[--len] = '\0';
        if (strlen(h->in) != len)
            puts("error command: holds a NUL");
        else
            command(h, h->in);
        fflush(stdout);
        memmove(h->in, h->in + used, h->in_len - used);
        h->in_len -= used;
    }
}

/**
 * Read what standard input has into `in`. Returns 0, or -1 when a command
 * line passes HOST_LINE_MAX.
 */
static int read_input(struct host *h)
{
    ssize_t n;
    char *p;

    if (h->in_len + 4096 + 1 > h->in_size) {
        if (h->in_size >= HOST_LINE_MAX + 4096 + 1)
            return -1;
        p = realloc(h->in, h->in_size ? h->in_size * 2 : 8192);
        if (!p)
            return -1;
        h->in = p;
        h->in_size = h->in_size ? h->in_size * 2 : 8192;
    }
    n = read(0, h->in + h->in_len, h->in_size - h->in_len - 1);
    if (n < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : -1;
    if (n == 0)
        h->in_eof = true;
    h->in_len += (size_t)n;
    return 0;
}

/** Read what the child wrote onto the screen, until the master has no more. */
static void read_output(struct host *h)
{
    char buf[4096];
    ssize_t n;

    for (;;) {
        n = term_pty_read(&h->pty, buf, sizeof(buf));
        if (n > 0) {
            term_screen_input(&h->screen, buf, (size_t)n);
            continue;
        }
        if (n == 0 || (n != -EAGAIN && n != -EINTR))
            h->output_ended = true;
        return;
    }
}

int term_host_run(const struct term_options *o, const struct term_conf *conf)
{
    enum { P_SIG, P_IN, P_MASTER, P_COUNT };
    struct pollfd p[P_COUNT];
    struct host *h;
    unsigned bits;
    int sigfd;
    int ret = 1;
    int r;

    h = calloc(1, sizeof(*h));
    if (!h) {
        fprintf(stderr, "term: -H: out of memory\n");
        return 1;
    }
    sigfd = term_signals_open();
    if (sigfd < 0) {
        fprintf(stderr, "term: signals: %s\n", strerror(-sigfd));
        free(h);
        return 1;
    }
    r = term_screen_init(&h->screen, &h->pty, HOST_COLS, HOST_ROWS);
    if (r < 0) {
        fprintf(stderr, "term: screen: %s\n", strerror(-r));
        goto out_signals;
    }
    term_screen_scrollback(&h->screen, conf->scrollback);
    term_keys_init_host(&h->keys);
    r = term_pty_open(&h->pty, HOST_COLS, HOST_ROWS, o->argv, false);
    if (r < 0) {
        fprintf(stderr, "term: %s: %s\n", o->argv ? o->argv[0] : "shell", strerror(-r));
        goto out_screen;
    }

    for (;;) {
        run_commands(h);
        if (h->block == BLOCK_NONE && h->in_eof && h->in_len == 0)
            break;

        p[P_SIG].fd = sigfd;
        p[P_SIG].events = POLLIN;
        p[P_IN].fd = h->in_eof || h->block != BLOCK_NONE ? -1 : 0;
        p[P_IN].events = POLLIN;
        p[P_MASTER].fd = h->output_ended ? -1 : h->pty.master;
        p[P_MASTER].events = (short)(POLLIN | (h->pty.queued ? POLLOUT : 0));
        for (r = 0; r < P_COUNT; r++)
            p[r].revents = 0;
        if (poll(p, P_COUNT, -1) < 0 && errno != EINTR) {
            perror("term: poll");
            goto out_pty;
        }

        bits = term_signals_take();
        if (bits & TERM_SIG_QUIT)
            goto out_pty;
        if ((bits & TERM_SIG_CHILD) && term_pty_reap(&h->pty, &h->status) > 0)
            h->child_done = true;
        if (p[P_MASTER].revents & POLLOUT)
            term_pty_flush(&h->pty);
        if (p[P_MASTER].revents & (POLLIN | POLLHUP | POLLERR))
            read_output(h);
        if (p[P_IN].revents & (POLLIN | POLLHUP | POLLERR) && read_input(h) < 0) {
            fprintf(stderr, "term: -H: standard input: unreadable or a line too long\n");
            goto out_pty;
        }
        settle(h);
    }

    /* Standard input has ended: hang the child up and reap it. */
    if (h->pty.child > 0)
        kill(h->pty.child, SIGHUP);
    term_pty_close(&h->pty);
    if (!h->child_done) {
        while (waitpid(h->pty.child, &h->status, 0) < 0 && errno == EINTR)
            ;
        h->pty.child = -1;
    }
    ret = term_status(h->status);

out_pty:
    if (h->pty.child > 0)
        kill(h->pty.child, SIGHUP);
    term_pty_close(&h->pty);
    if (h->pty.child > 0) {
        while (waitpid(h->pty.child, &h->status, 0) < 0 && errno == EINTR)
            ;
        h->pty.child = -1;
    }
out_screen:
    term_keys_close(&h->keys);
    term_screen_free(&h->screen);
out_signals:
    term_signals_close();
    free(h->tapped);
    free(h->in);
    free(h);
    return ret;
}
