/**
 * n_tty_hosttest.c: the N_TTY line discipline (`tty/n_tty.c`).
 *
 * host-deps: rach2/sys/tty/n_tty.c rach2/sys/wcwidth.c
 *
 * The cooked line editor (ERASE, WERASE and KILL with ECHOE's rub-out, a
 * whole UTF-8 character under IUTF8 rubbed out by its width, and
 * the rules by which VEOF and a newline end a line), raw mode's VMIN and
 * VTIME, the
 * recognition of ISIG's control characters with NOFLSH, IXON's VSTOP and
 * VSTART with IXANY, the look ahead and the echo held while output is
 * stopped, and the OPOST and ONLCR mapping
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

/** Feed one byte and then give the echo held to the sink, as the terminal
 * layer does after it flushes its output; the signal the discipline
 * recognized, or 0. */
static int feed(struct n_tty *t, struct cap *cp, unsigned char c)
{
    int signo = -1;
    n_tty_receive(t, c, sink, cp, &signo);
    n_tty_echo_flush(t, sink, cp, ~0u);
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

/**
 * A canonical line holds 4095 bytes before its newline, as Linux's n_tty
 * does: of 4096 bytes typed and a newline, the 4096th is dropped and counted
 * in line_drops, and a read returns the rest with its newline. A read for
 * less stops inside the line, which `n_tty_line_open` reports until a read
 * takes the rest; the next line is not joined to it.
 */
static void long_line(void)
{
    static struct n_tty t;
    static unsigned char buf[N_TTY_LINE_MAX + 16];
    struct cap cp;
    int i, n, ok;

    n_tty_init(&t);
    cap_reset(&cp);
    for (i = 0; i < N_TTY_LINE_MAX; i++) {
        feed(&t, &cp, (unsigned char)('a' + i % 26));
    }
    feed(&t, &cp, '\n');
    CHECK(t.counters.line_drops == 1 && t.counters.line_max == N_TTY_LINE_MAX - 1);
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == N_TTY_LINE_MAX && buf[N_TTY_LINE_MAX - 1] == '\n');
    for (i = 0, ok = 1; i < N_TTY_LINE_MAX - 1; i++) {
        ok &= buf[i] == (unsigned char)('a' + i % 26);
    }
    CHECK(ok);
    CHECK(!n_tty_line_open(&t) && n_tty_inq(&t) == 0);

    /* A read through a smaller buffer leaves the line open for the rest. */
    for (i = 0; i < N_TTY_LINE_MAX - 1; i++) {
        feed(&t, &cp, 'b');
    }
    feed(&t, &cp, '\n');
    feed_str(&t, &cp, "c\n");
    CHECK(t.counters.line_drops == 1);
    CHECK(n_tty_read(&t, buf, 1024) == 1024 && n_tty_line_open(&t));
    CHECK(n_tty_read(&t, buf, 1024) == 1024 && n_tty_line_open(&t));
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == N_TTY_LINE_MAX - 2048 && buf[n - 1] == '\n' && !n_tty_line_open(&t));
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 2 && buf[0] == 'c');

    /* A flush closes an open line. */
    feed_str(&t, &cp, "long\n");
    CHECK(n_tty_read(&t, buf, 2) == 2 && n_tty_line_open(&t));
    n_tty_flush(&t);
    CHECK(!n_tty_line_open(&t));
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

/** Whether the line under edit is the `n` bytes of `want`. */
static int line_is(const struct n_tty *t, const char *want, int n)
{
    int i;

    if (t->line_len != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (t->line[i] != (unsigned char)want[i]) {
            return 0;
        }
    }
    return 1;
}

/** Report a failed erase check `what` on stderr and count it. */
static void erase_fail(const char *what)
{
    dprintf(2, "n-tty-test: fail: %s\n", what);
    fails++;
}

/**
 * Under IUTF8, which a new terminal has, ERASE removes a whole UTF-8
 * character and rubs out as many cells as the console drew it in: one for a
 * Cyrillic letter, two for a CJK character or an emoji, and none for a
 * combining mark, which leaves its base. Without IUTF8 it removes one byte.
 * Bytes that end in no whole sequence go one at a time. A control character
 * echoed as `^X` under ECHOCTL takes two cells. WERASE and KILL remove whole
 * characters too.
 */
static void utf8_erase(void)
{
    struct n_tty t;
    struct cap cp;
    unsigned char erase;

    n_tty_init(&t);
    if (!(t.tio.c_iflag & IUTF8)) {
        erase_fail("a new terminal lacks IUTF8");
    }
    erase = t.tio.c_cc[VERASE];

    cap_reset(&cp);
    feed_str(&t, &cp, "a\320\264");
    cap_reset(&cp);
    feed(&t, &cp, erase);
    if (!line_is(&t, "a", 1) || !cap_is(&cp, "\b \b", 3)) {
        erase_fail("a Cyrillic letter is not erased whole in one cell");
    }

    cap_reset(&cp);
    feed_str(&t, &cp, "\344\275\240");
    cap_reset(&cp);
    feed(&t, &cp, erase);
    if (!line_is(&t, "a", 1) || !cap_is(&cp, "\b \b\b \b", 6)) {
        erase_fail("a CJK character is not erased whole in two cells");
    }

    cap_reset(&cp);
    feed_str(&t, &cp, "\360\237\232\200");
    cap_reset(&cp);
    feed(&t, &cp, erase);
    if (!line_is(&t, "a", 1) || !cap_is(&cp, "\b \b\b \b", 6)) {
        erase_fail("an emoji is not erased whole in two cells");
    }

    cap_reset(&cp);
    feed_str(&t, &cp, "e\314\201");
    cap_reset(&cp);
    feed(&t, &cp, erase);
    if (!line_is(&t, "ae", 2) || cp.len != 0) {
        erase_fail("a combining mark is not erased alone with no rub-out");
    }

    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "a\240\240");
    feed(&t, &cp, erase);
    if (!line_is(&t, "a\240", 2)) {
        erase_fail("a stray continuation byte is not erased alone");
    }

    n_tty_init(&t);
    t.tio.c_iflag &= ~(unsigned int)IUTF8;
    feed_str(&t, &cp, "a\320\264");
    feed(&t, &cp, erase);
    if (!line_is(&t, "a\320", 2)) {
        erase_fail("without IUTF8 an erase does not remove one byte");
    }

    n_tty_init(&t);
    feed(&t, &cp, 'a');
    feed(&t, &cp, 0x01);
    cap_reset(&cp);
    feed(&t, &cp, erase);
    if (!line_is(&t, "a", 1) || !cap_is(&cp, "\b \b\b \b", 6)) {
        erase_fail("a control character's ^A is not rubbed out in two cells");
    }

    n_tty_init(&t);
    feed_str(&t, &cp, "\320\264\320\260 \344\275\240\345\245\275");
    feed(&t, &cp, t.tio.c_cc[VWERASE]);
    if (!line_is(&t, "\320\264\320\260 ", 5)) {
        erase_fail("WERASE does not remove a word of CJK characters");
    }
    feed(&t, &cp, t.tio.c_cc[VKILL]);
    if (t.line_len != 0) {
        erase_fail("KILL does not remove a line of Cyrillic letters");
    }
}

/** The bytes the read queue holds, read out into `buf` of `size`. */
static int drain(struct n_tty *t, unsigned char *buf, int size)
{
    return n_tty_read(t, buf, size);
}

/**
 * IXON and IXANY, as Linux 6.12's n_tty has them: the defaults, VSTOP and
 * VSTART consumed, a stop that holds the echo and the start that gives it
 * out, a disabled character, ISTRIP before the test, VSTART winning when both
 * are one byte, IXANY, and a signal character that restarts output.
 */
static void flow_control(void)
{
    struct n_tty t;
    struct cap cp;
    unsigned char buf[16];

    n_tty_init(&t);
    cap_reset(&cp);
    CHECK((t.tio.c_iflag & IXON) && !(t.tio.c_iflag & (IXANY | IXOFF)));
    CHECK(t.tio.c_cc[VSTART] == 021 && t.tio.c_cc[VSTOP] == 023);
    CHECK(!(t.tio.c_lflag & NOFLSH));

    /* VSTOP stops, holds the echo, and reaches no reader; VSTART restarts. */
    CHECK(feed(&t, &cp, 023) == 0 && t.stopped && cp.len == 0);
    feed_str(&t, &cp, "ab");
    CHECK(cp.len == 0 && t.echo_tail - t.echo_head == 2); /* held */
    CHECK(feed(&t, &cp, 021) == 0 && !t.stopped);
    CHECK(cap_is(&cp, "ab", 2)); /* the held echo comes out */
    feed(&t, &cp, '\n');
    CHECK(drain(&t, buf, sizeof(buf)) == 3 && buf[0] == 'a' && buf[2] == '\n');

    /* A disabled VSTOP never matches a NUL; ISTRIP applies first. */
    n_tty_init(&t);
    t.tio.c_cc[VSTOP] = 0;
    feed(&t, &cp, 0);
    CHECK(!t.stopped && t.line_len == 1);
    n_tty_init(&t);
    t.tio.c_iflag |= ISTRIP;
    feed(&t, &cp, 0223); /* 0x93 stripped is ^S */
    CHECK(t.stopped && t.line_len == 0);

    /* When VSTART and VSTOP are one byte, it restarts. */
    n_tty_init(&t);
    t.tio.c_cc[VSTART] = t.tio.c_cc[VSTOP] = 023;
    t.stopped = 1;
    feed(&t, &cp, 023);
    CHECK(!t.stopped);

    /* IXANY: any byte restarts and is input as well; without it, none. */
    n_tty_init(&t);
    feed(&t, &cp, 023);
    feed(&t, &cp, 'x');
    CHECK(t.stopped && t.line_len == 1);
    t.tio.c_iflag |= IXANY;
    feed(&t, &cp, 'y');
    CHECK(!t.stopped && t.line_len == 2);

    /* A signal character under IXON restarts output; its echo follows. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, 023);
    feed_str(&t, &cp, "zz");
    CHECK(feed(&t, &cp, t.tio.c_cc[VINTR]) == NTTY_SIGINT && !t.stopped);
    CHECK(cap_is(&cp, "^C", 2)); /* the held "zz" was flushed with the input */
}

/**
 * TCOOFF's stop that neither VSTART, IXANY nor a signal character ends, the
 * look ahead and the byte fed after it, IXON turned off, and a signal
 * character under NOFLSH that keeps the input and echoes in its turn, as
 * Linux 6.12's n_tty has them.
 */
static void flow_stops(void)
{
    struct n_tty t;
    struct ktermios tio;
    struct cap cp;

    cap_reset(&cp);
    /* TCOOFF's stop: neither VSTART, IXANY nor a signal character ends it. */
    n_tty_init(&t);
    t.tio.c_iflag |= IXANY;
    t.tco_stopped = 1;
    CHECK(n_tty_stop(&t) == 1);
    feed(&t, &cp, 021);
    feed(&t, &cp, 'q');
    feed(&t, &cp, t.tio.c_cc[VINTR]);
    CHECK(t.stopped && n_tty_start(&t) == 0);
    t.tco_stopped = 0;
    CHECK(n_tty_start(&t) == 1 && !t.stopped);

    /* The look ahead acts now, and the byte fed after it does not again. */
    n_tty_init(&t);
    CHECK(n_tty_lookahead(&t, 023) == 1 && t.stopped);
    CHECK(n_tty_lookahead(&t, 'k') == 0);
    CHECK(n_tty_start(&t) == 1);
    {
        int signo;

        n_tty_receive_looked(&t, 023, sink, &cp, &signo);
        CHECK(!t.stopped && signo == 0 && t.line_len == 0);
    }
    t.tio.c_iflag &= ~(unsigned int)IXON;
    CHECK(n_tty_lookahead(&t, 023) == 0 && !t.stopped);

    /* Turning IXON off restarts a VSTOP's stop, not TCOOFF's. */
    n_tty_init(&t);
    feed(&t, &cp, 023);
    tio = t.tio;
    tio.c_iflag &= ~(unsigned int)IXON;
    n_tty_set_termios(&t, &tio);
    CHECK(!t.stopped);
    feed(&t, &cp, 023); /* now input */
    CHECK(!t.stopped && t.line_len == 1);

    /* NOFLSH: the signal keeps the input, and its echo is not held. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag |= NOFLSH;
    feed_str(&t, &cp, "old\npart");
    cap_reset(&cp);
    {
        int signo;

        n_tty_receive(&t, t.tio.c_cc[VINTR], sink, &cp, &signo);
        CHECK(signo == NTTY_SIGINT && cap_is(&cp, "^C", 2));
    }
    CHECK(t.line_len == 4 && n_tty_inq(&t) == 4);
    /* Without NOFLSH the signal's echo waits for the caller's flush. */
    t.tio.c_lflag &= ~(unsigned int)NOFLSH;
    cap_reset(&cp);
    {
        int signo;

        n_tty_receive(&t, t.tio.c_cc[VINTR], sink, &cp, &signo);
        CHECK(signo == NTTY_SIGINT && cp.len == 0 && n_tty_inq(&t) == 0 && t.line_len == 0);
    }
    CHECK(n_tty_echo_flush(&t, sink, &cp, ~0u) == 2 && cap_is(&cp, "^C", 2));

    /* The echo held goes out at most `max` bytes at a time, the rest held;
     * a byte fed muted holds no echo, even behind a full buffer, whose
     * oldest byte stays. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, 023);
    feed_str(&t, &cp, "abc");
    CHECK(n_tty_start(&t) == 1);
    CHECK(n_tty_echo_flush(&t, sink, &cp, 2) == 2 && cap_is(&cp, "ab", 2));
    CHECK(t.echo_tail - t.echo_head == 1);
    {
        int k, signo;

        t.echo_muted = 1;
        n_tty_receive(&t, 'd', sink, &cp, &signo);
        t.echo_muted = 0;
        CHECK(t.echo_tail - t.echo_head == 1 && cp.len == 2);
        CHECK(n_tty_echo_flush(&t, sink, &cp, ~0u) == 1 && cap_is(&cp, "abc", 3));
        feed(&t, &cp, 023);
        for (k = 0; k < N_TTY_ECHO_MAX; k++) {
            n_tty_hold_echo(&t, (unsigned char)('a' + k % 26));
        }
        t.echo_muted = 1;
        n_tty_receive(&t, 'z', sink, &cp, &signo);
        t.echo_muted = 0;
        CHECK(t.echo_tail - t.echo_head == N_TTY_ECHO_MAX &&
              t.echo_buf[t.echo_head & (N_TTY_ECHO_MAX - 1)] == 'a');
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
    utf8_erase();
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

    /* VMIN == 0: a read never blocks, but poll, as Linux's does, sees the
     * terminal readable only once a byte is queued. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 0;
    CHECK(n_tty_readable(&t) == 0);
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* nothing to return */
    feed(&t, &cp, 'k');
    CHECK(n_tty_readable(&t) == 1);
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 1 && buf[0] == 'k');

    /* A read's own test, n_tty_read_ready, in POSIX's four cases of VMIN
     * and VTIME, with its timer run out or not. VMIN 0, VTIME 0: at once. */
    n_tty_init(&t);
    cap_reset(&cp);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 0;
    CHECK(n_tty_read_timer(&t) == N_TTY_TIMER_NONE);
    CHECK(n_tty_read_ready(&t, 8, 0) != 0);
    /* VMIN 3, VTIME 0: three bytes, or the read's count when that is less;
     * poll still wants VMIN. */
    t.tio.c_cc[VMIN] = 3;
    feed(&t, &cp, 'a');
    CHECK(n_tty_read_timer(&t) == N_TTY_TIMER_NONE);
    CHECK(n_tty_read_ready(&t, 8, 1) == 0);
    CHECK(n_tty_read_ready(&t, 1, 0) != 0 && n_tty_readable(&t) == 0);
    feed_str(&t, &cp, "bc");
    CHECK(n_tty_read_ready(&t, 8, 0) != 0);
    /* VMIN 0, VTIME 2: the first byte, or none once the timer runs out. */
    n_tty_init(&t);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 0;
    t.tio.c_cc[VTIME] = 2;
    CHECK(n_tty_read_timer(&t) == N_TTY_TIMER_READ);
    CHECK(n_tty_read_ready(&t, 8, 0) == 0 && n_tty_readable(&t) == 0);
    CHECK(n_tty_read_ready(&t, 8, 1) != 0);
    feed(&t, &cp, 'a');
    CHECK(n_tty_read_ready(&t, 8, 0) != 0 && n_tty_readable(&t) == 1);
    /* VMIN 18, VTIME 1, as kbd's showkey sets them: 18 bytes, or fewer
     * once the timer runs out after one; none waits for the first. */
    n_tty_init(&t);
    t.tio.c_lflag &= ~(ICANON | ISIG | ECHO);
    t.tio.c_cc[VMIN] = 18;
    t.tio.c_cc[VTIME] = 1;
    CHECK(n_tty_read_timer(&t) == N_TTY_TIMER_BYTE);
    CHECK(n_tty_read_ready(&t, 18, 1) == 0);
    feed(&t, &cp, 'a');
    CHECK(n_tty_read_ready(&t, 18, 0) == 0 && n_tty_read_ready(&t, 18, 1) != 0);
    CHECK(n_tty_readable(&t) == 1); /* VTIME set: poll wants one byte */
    for (i = 1; i < 18; i++) {
        feed(&t, &cp, 'a');
    }
    CHECK(n_tty_read_ready(&t, 18, 0) != 0);
    /* Canonical mode has no timer and waits for a line. */
    t.tio.c_lflag |= ICANON;
    CHECK(n_tty_read_timer(&t) == N_TTY_TIMER_NONE);
    n_tty_init(&t);
    t.tio.c_cc[VTIME] = 1;
    feed(&t, &cp, 'a');
    CHECK(n_tty_read_timer(&t) == N_TTY_TIMER_NONE && n_tty_read_ready(&t, 8, 1) == 0);

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

    long_line();
    flow_control();
    flow_stops();

    return fails > 255 ? 255 : fails;
}
