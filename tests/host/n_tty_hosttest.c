/**
 * n_tty_hosttest.c: the N_TTY line discipline (`tty/n_tty.c`).
 *
 * host-deps: rach2/sys/tty/n_tty.c
 *
 * The cooked line editor (ERASE, WERASE and KILL with ECHOE's rub-out, and
 * the rules by which VEOF and a newline end a line), raw mode's VMIN, the
 * recognition of ISIG's control characters, and the OPOST and ONLCR mapping
 * of output have no hardware under them, and every console read goes
 * through them, so they run natively here under ASan and UBSan with the echo
 * and the output kept in a buffer instead of a screen. Exits with the count
 * of failed checks, at most 255.
 */

#include "tty/n_tty.h"

extern int dprintf(int fd, const char *format, ...);

static int fails;

/** Count a failure when `cond` is false. */
#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* The byte sink: echo and output bytes land here instead of on the console. */
struct cap {
    unsigned char buf[1024];
    int len;
};

/**
 * The echo and output sink: append `c` to the `struct cap` `ctx`, dropping what
 * does not fit.
 */
static void sink(void *ctx, unsigned char c)
{
    struct cap *cp = (struct cap *)ctx;
    if (cp->len < (int)sizeof(cp->buf)) {
        cp->buf[cp->len++] = c;
    }
}

/** Empty the bytes kept in `cp`. */
static void cap_reset(struct cap *cp)
{
    cp->len = 0;
}

/** Whether the bytes kept are the first `n` bytes of `expect`, and no more. */
static int cap_is(const struct cap *cp, const char *expect, int n)
{
    int i;
    if (cp->len != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (cp->buf[i] != (unsigned char)expect[i]) {
            return 0;
        }
    }
    return 1;
}

/** Feed one byte; the signal the discipline recognized, or 0. */
static int feed(struct n_tty *t, struct cap *cp, unsigned char c)
{
    int signo = -1;
    n_tty_receive(t, c, sink, cp, &signo);
    return signo;
}

/** Feed the bytes of the string `s`, ignoring the signals. */
static void feed_str(struct n_tty *t, struct cap *cp, const char *s)
{
    while (*s) {
        feed(t, cp, (unsigned char)*s++);
    }
}

/** Require one read to return exactly the specified bytes and boundary. */
static void expect_line(struct n_tty *t, const char *expected, int count, const char *detail)
{
    unsigned char buf[32];
    int n = n_tty_read(t, buf, sizeof(buf));
    int i;

    if (n == count) {
        for (i = 0; i < n && buf[i] == (unsigned char)expected[i]; i++) {
        }
        if (i == n) {
            return;
        }
    }
    dprintf(2, "n-tty-test: fail: %s\n", detail);
    fails++;
}

/** Preserve VEOL bytes and queued boundaries across termios changes. */
static void veol_lines(void)
{
    struct n_tty t;
    struct cap cp;
    struct ktermios tio;
    unsigned char byte;

    n_tty_init(&t);
    cap_reset(&cp);
    tio = t.tio;
    tio.c_cc[VEOL] = ';';
    n_tty_set_termios(&t, &tio);
    feed_str(&t, &cp, "ab;");
    if (!cap_is(&cp, "ab;", 3)) {
        dprintf(2, "n-tty-test: fail: VEOL echo preserves semicolon\n");
        fails++;
    }
    tio.c_cc[VEOL] = ':';
    n_tty_set_termios(&t, &tio);
    feed_str(&t, &cp, "cd:\n");
    expect_line(&t, "ab;", 3, "VEOL byte and boundary survive termios change");
    expect_line(&t, "cd:", 3, "new VEOL terminates its own line");
    expect_line(&t, "\n", 1, "newline remains a separate line");

    feed_str(&t, &cp, "xy:z\n");
    CHECK(n_tty_read(&t, &byte, 1) == 1 && byte == 'x');
    expect_line(&t, "y:", 2, "short read retains the queued boundary");
    expect_line(&t, "z\n", 2, "short read does not consume the next line");
}

/** A VEOF-completed line ends before later input, including after wrap. */
static void eof_lines(void)
{
    struct n_tty t;
    struct cap cp;
    unsigned char byte;

    n_tty_init(&t);
    cap_reset(&cp);
    t.rq_head = t.rq_tail = ~0U - 1;
    feed_str(&t, &cp, "ab");
    feed(&t, &cp, t.tio.c_cc[VEOF]);
    feed_str(&t, &cp, "cd\n");
    expect_line(&t, "ab", 2, "VEOF boundary precedes a later newline");
    expect_line(&t, "cd\n", 3, "line after VEOF is read separately");

    feed_str(&t, &cp, "ab");
    feed(&t, &cp, t.tio.c_cc[VEOF]);
    feed_str(&t, &cp, "cd");
    feed(&t, &cp, t.tio.c_cc[VEOF]);
    CHECK(n_tty_read(&t, &byte, 1) == 1 && byte == 'a');
    expect_line(&t, "b", 1, "short read retains VEOF boundary");
    expect_line(&t, "cd", 2, "consecutive VEOF lines stay separate");
    CHECK(n_tty_readable(&t) == 0);
}

/** Reused ring slots must not retain markers from consumed or flushed lines. */
static void reused_markers(void)
{
    struct n_tty t;
    struct cap cp;
    int i;

    n_tty_init(&t);
    cap_reset(&cp);
    for (i = 0; i < N_TTY_READ_MAX; i++) {
        feed(&t, &cp, 'x');
        feed(&t, &cp, t.tio.c_cc[VEOF]);
    }
    n_tty_flush(&t);
    feed_str(&t, &cp, "ab");
    feed(&t, &cp, t.tio.c_cc[VEOF]);
    expect_line(&t, "ab", 2, "ring reuse clears old end markers");
}

/** A new terminal's control flags are Linux's: 38400 baud, eight-bit
 * characters, the receiver on and a hang-up at the last close, so that
 * `stty` does not report speed 0, which is the code for hanging up. */
static void default_cflag(void)
{
    struct n_tty t;

    n_tty_init(&t);
    if (t.tio.c_cflag != (B38400 | CS8 | CREAD | HUPCL)) {
        dprintf(2, "n-tty-test: fail: a new terminal's c_cflag is 0%o\n", (unsigned)t.tio.c_cflag);
        fails++;
    }
}

/**
 * Run the rows; the count of failures, at most 255.
 *
 * A canonical read returns one line, and a second line waits for the next read.
 * ERASE on an empty line does nothing, with no underflow and no echo. VEOF on
 * an empty line is the end of file, read as 0, and once: a following character
 * starts a line again; in the middle of a line it delivers the partial line. A
 * ^D on an empty line typed before a later line is read back first, then the
 * line; a committed line before a ^D comes first, then the end of file. ISIG
 * flushes the whole input queue, not only the edit line, so a committed line
 * that was not read does not survive the signal. With INLCR and ICRNL off, NL
 * becomes CR, an ordinary character, so a lone NL completes no line.
 *
 * A partial canonical line becomes readable when ICANON goes off: the bytes
 * typed before a program switched to raw mode are not lost, the bytes typed
 * after them follow in order, and the counters say what moved and what was
 * read. A switch that stays canonical makes nothing readable; the order typed
 * is kept across the switch, a committed line, then the partial one, then the
 * raw bytes; and FIONREAD counts what a raw read would get. A partial line the
 * full queue cannot take at the switch is kept and read after the queue.
 *
 * A control character set to 0 is disabled, the _POSIX_VDISABLE convention, so
 * a NUL is not read as that character: not as VINTR, VEOF or VERASE. In raw
 * mode a hangup, the master's close, sets the end of file, which reads as
 * readable, so that a blocked reader returns 0 rather than wait for VMIN bytes
 * that will not come; bytes still queued are read first. A canonical line
 * longer than the buffer is capped, its terminator still lands, and the read
 * returns the capped line at once.
 */
int main(void)
{
    struct n_tty t;
    struct cap cp;
    unsigned char buf[512];
    int n, i;

    default_cflag();
    veol_lines();
    eof_lines();
    reused_markers();

    /* cooked: assemble a line, ERASE rubs out, exact ECHOE feedback */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, 'a');
    feed(&t, &cp, 'b');
    feed(&t, &cp, t.tio.c_cc[VERASE]); /* rub out 'b' */
    feed(&t, &cp, t.tio.c_cc[VERASE]); /* rub out 'a' */
    feed(&t, &cp, 'c');
    feed(&t, &cp, 'd');
    CHECK(n_tty_readable(&t) == 0); /* no line terminator yet */
    feed(&t, &cp, '\n');
    CHECK(n_tty_readable(&t) == 3);
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'c' && buf[1] == 'd' && buf[2] == '\n');
    /* echo: a b, two DEL rub-outs ("\b \b" each), c d, then NL as CRLF. */
    CHECK(cap_is(&cp, "ab\b \b\b \bcd\r\n", 12));
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* queue drained */

    /* A canonical read returns one line; the second waits its turn. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "hi\nyo\n");
    CHECK(n_tty_inq(&t) == 6); /* FIONREAD: both committed lines, 6 bytes */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'y' && buf[1] == 'o' && buf[2] == '\n');

    /* WERASE eats the last word plus the run of spaces before it, no more. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "foo bar");
    feed(&t, &cp, t.tio.c_cc[VWERASE]);
    feed(&t, &cp, '\n');
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 5 && buf[0] == 'f' && buf[1] == 'o' && buf[2] == 'o' && buf[3] == ' ' &&
          buf[4] == '\n');

    /* KILL clears the whole line; what follows is all that survives. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "hello");
    feed(&t, &cp, t.tio.c_cc[VKILL]);
    feed_str(&t, &cp, "hi\n");
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');

    /* ERASE on an empty line does nothing. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, t.tio.c_cc[VERASE]);
    CHECK(cp.len == 0 && t.line_len == 0);

    /* VEOF: empty line is EOF; mid-line delivers the partial line */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, t.tio.c_cc[VEOF]); /* ^D on an empty line */
    CHECK(n_tty_readable(&t) == 1);  /* EOF reads as "readable"... */
    CHECK(n_tty_inq(&t) == 0);       /* ...but FIONREAD counts 0 bytes (read -> 0) */
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0);
    /* EOF is one-shot: a following character starts a normal line again. */
    feed_str(&t, &cp, "z\n");
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'z' && buf[1] == '\n');

    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "ab");
    feed(&t, &cp, t.tio.c_cc[VEOF]); /* ^D mid-line: deliver "ab" with no newline */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'a' && buf[1] == 'b');

    /* A ^D on an empty line, then a line: the end of file is read first. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, t.tio.c_cc[VEOF]);              /* ^D on empty line: EOF at this position */
    feed_str(&t, &cp, "x\n");                     /* ...then a line arrives before any read */
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* EOF delivered first */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'x' && buf[1] == '\n'); /* then the line */

    /* A committed line, then ^D: the line comes first, then EOF. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "foo\n");
    feed(&t, &cp, t.tio.c_cc[VEOF]); /* ^D on the now-empty line */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 4 && buf[0] == 'f' && buf[3] == '\n');
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* then EOF */

    /* ISIG: INTR/QUIT/SUSP recognized, echoed ^X, flush the line */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "junk");
    CHECK(feed(&t, &cp, t.tio.c_cc[VINTR]) == NTTY_SIGINT);
    CHECK(t.line_len == 0); /* the partial line is discarded */
    /* echo so far: j u n k ^C  -> the ^C renders as two chars '^' 'C'. */
    CHECK(cap_is(&cp, "junk^C", 6));
    feed_str(&t, &cp, "x\n"); /* the discarded "junk" must not reappear */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'x' && buf[1] == '\n');

    n_tty_init(&t);
    cap_reset(&cp);
    CHECK(feed(&t, &cp, t.tio.c_cc[VQUIT]) == NTTY_SIGQUIT);
    CHECK(cap_is(&cp, "^\\", 2)); /* 0x1c | 0x40 == '\\' */
    n_tty_init(&t);
    cap_reset(&cp);
    CHECK(feed(&t, &cp, t.tio.c_cc[VSUSP]) == NTTY_SIGTSTP);
    CHECK(cap_is(&cp, "^Z", 2)); /* 0x1a | 0x40 == 'Z' */

    /* ISIG flushes the whole input queue, its committed lines too. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "old\n"); /* committed to the read queue, unread */
    feed_str(&t, &cp, "part");  /* plus a partial edit line */
    CHECK(feed(&t, &cp, t.tio.c_cc[VINTR]) == NTTY_SIGINT);
    CHECK(n_tty_readable(&t) == 0); /* both the line and the queue are gone */
    feed_str(&t, &cp, "new\n");
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 4 && buf[0] == 'n' && buf[3] == '\n'); /* only post-signal input */

    /* input flag mapping: ICRNL, IGNCR, INLCR, ISTRIP */

    /* ICRNL (default): a bare CR terminates the line as if it were NL. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "hi");
    feed(&t, &cp, '\r');
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');

    /* IGNCR: CR is dropped outright (the line keeps assembling). */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_iflag |= IGNCR;
    feed_str(&t, &cp, "hi");
    feed(&t, &cp, '\r');
    CHECK(n_tty_readable(&t) == 0); /* CR ignored, no line yet */
    feed(&t, &cp, '\n');
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');

    /* INLCR with ICRNL off: NL becomes CR, which ends no line. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_iflag &= ~ICRNL;
    t.tio.c_iflag |= INLCR;
    feed_str(&t, &cp, "a");
    feed(&t, &cp, '\n');
    CHECK(n_tty_readable(&t) == 0);
    CHECK(t.line_len == 2 && t.line[1] == '\r');

    /* ISTRIP: the high bit is cleared before the byte is interpreted. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_iflag |= ISTRIP;
    feed(&t, &cp, 0xC1); /* 0xC1 & 0x7f == 'A' */
    feed(&t, &cp, '\n');
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'A' && buf[1] == '\n');

    /* A partial canonical line becomes readable when ICANON goes off. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "cp b");
    CHECK(n_tty_readable(&t) == 0);
    {
        struct ktermios raw = t.tio;
        raw.c_lflag &= ~(ICANON | ECHO);
        CHECK(n_tty_set_termios(&t, &raw) == 4);
    }
    feed_str(&t, &cp, "uild\n");
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 9 && buf[0] == 'c' && buf[3] == 'b' && buf[4] == 'u' && buf[8] == '\n');
    CHECK(t.counters.partial_moved == 4);
    CHECK(t.counters.read_bytes == 9);
    /* A switch that stays canonical makes nothing readable. */
    {
        struct ktermios cooked = t.tio;
        struct ktermios raw = t.tio;
        cooked.c_lflag |= ICANON;
        raw.c_lflag &= ~ICANON;
        n_tty_set_termios(&t, &cooked);
        feed_str(&t, &cp, "old\nnew");
        CHECK(n_tty_set_termios(&t, &cooked) == 4);
        CHECK(n_tty_inq(&t) == 4);
        CHECK(n_tty_set_termios(&t, &raw) == 7);
        CHECK(n_tty_inq(&t) == 7);
        feed_str(&t, &cp, "!");
        n = n_tty_read(&t, buf, sizeof(buf));
        CHECK(n == 8 && buf[0] == 'o' && buf[3] == '\n' && buf[4] == 'n' && buf[7] == '!');
        CHECK(t.counters.partial_moved == 7);
    }
    /* A partial line the full queue cannot take at the switch is kept. */
    n_tty_init(&t);
    cap_reset(&cp);
    for (i = 0; i < N_TTY_READ_MAX; i++) {
        feed(&t, &cp, (unsigned char)(i % 16 == 15 ? '\n' : 'x'));
    }
    feed_str(&t, &cp, "kept");
    CHECK(t.counters.queue_drops == 0);
    {
        struct ktermios raw = t.tio;
        raw.c_lflag &= ~(ICANON | ECHO);
        CHECK(n_tty_set_termios(&t, &raw) == N_TTY_READ_MAX + 4);
    }
    for (i = 0; i < N_TTY_READ_MAX / 512; i++) {
        CHECK(n_tty_read(&t, buf, 512) == 512);
    }
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 4 && buf[0] == 'k' && buf[3] == 't');
    CHECK(t.counters.partial_moved == 4);
    CHECK(n_tty_readable(&t) == 0);
    /* A flush counts what it discards: the partial line and the queue. */
    n_tty_init(&t);
    feed_str(&t, &cp, "one\ntw");
    n_tty_flush(&t);
    CHECK(t.counters.flush_bytes == 6);
    CHECK(n_tty_readable(&t) == 0);

    /* raw mode: every byte immediately readable, gated only by VMIN */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO); /* cfmakeraw-like */
    CHECK(n_tty_readable(&t) == 0);           /* nothing queued */
    feed(&t, &cp, 'x');
    CHECK(n_tty_readable(&t) == 1); /* available without a newline */
    CHECK(cp.len == 0);             /* ECHO off: nothing echoed */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 1 && buf[0] == 'x');
    /* With ISIG off, a control byte passes straight through as data. */
    feed(&t, &cp, 0x03);
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 1 && buf[0] == 0x03);

    /* VMIN > 1: a read is not satisfied until VMIN bytes have arrived. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 3;
    feed(&t, &cp, 'a');
    feed(&t, &cp, 'b');
    CHECK(n_tty_readable(&t) == 0); /* only 2 of 3 */
    feed(&t, &cp, 'c');
    CHECK(n_tty_readable(&t) == 3);
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 3 && buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c');

    /* VMIN == 0: a polling read never blocks (readable even when empty). */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 0;
    CHECK(n_tty_readable(&t) == 1);
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* ...but nothing to return */

    /* output processing (OPOST/ONLCR/OCRNL) via n_tty_output */

    /* OPOST|ONLCR: a write of NL goes out as CRLF. */
    n_tty_init(&t);
    cap_reset(&cp);
    n_tty_output(&t, '\n', sink, &cp);
    CHECK(cap_is(&cp, "\r\n", 2));

    /* OCRNL: a write of CR goes out as NL. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_oflag |= OCRNL;
    n_tty_output(&t, '\r', sink, &cp);
    CHECK(cap_is(&cp, "\n", 1));

    /* OPOST off: bytes pass through untouched. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_oflag = 0;
    n_tty_output(&t, '\n', sink, &cp);
    n_tty_output(&t, 'q', sink, &cp);
    CHECK(cap_is(&cp, "\nq", 2));

    /* With VINTR 0, disabled, a NUL raises no signal and is data. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_cc[VINTR] = 0;
    CHECK(feed(&t, &cp, 0) == 0); /* NUL is not INTR */
    CHECK(t.line_len == 1);       /* it is ordinary data */

    /* With VEOF disabled, a NUL on an empty line is not end-of-file. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_cc[VEOF] = 0;
    feed(&t, &cp, 0);
    CHECK(t.eof == 0);              /* NUL did not signal EOF */
    CHECK(n_tty_readable(&t) == 0); /* and no line completed */

    /* With VERASE disabled, a NUL does not rub out the last char. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_cc[VERASE] = 0;
    feed_str(&t, &cp, "ab");
    feed(&t, &cp, 0);
    CHECK(t.line_len == 3); /* 'a', 'b', and the NUL, nothing erased */

    /* raw mode: a hangup sets EOF, which reads as readable. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 1;
    CHECK(n_tty_readable(&t) == 0);               /* empty, no hangup: blocks */
    t.eof = 1;                                    /* tty_hangup marks EOF */
    CHECK(n_tty_readable(&t) == 1);               /* now readable... */
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* ...and reads as EOF */

    /* A hangup with bytes still queued drains them first, then reads 0. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 3; /* would otherwise wait for 3 bytes */
    feed(&t, &cp, 'a');
    t.eof = 1;
    CHECK(n_tty_readable(&t) == 1); /* the queued byte, despite VMIN 3 */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 1 && buf[0] == 'a');
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* then persistent EOF */

    /* buffer bounds: a canonical line longer than the buffer is capped */
    n_tty_init(&t);
    cap_reset(&cp);
    for (i = 0; i < 300; i++) {
        feed(&t, &cp, 'a');
    }
    feed(&t, &cp, '\n');
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == N_TTY_LINE_MAX); /* 255 'a' + '\n' == 256 */
    CHECK(buf[N_TTY_LINE_MAX - 1] == '\n');
    for (i = 0; i < N_TTY_LINE_MAX - 1; i++) {
        if (buf[i] != 'a') {
            fails++;
            break;
        }
    }

    return fails > 255 ? 255 : fails;
}
