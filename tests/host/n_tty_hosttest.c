/*
 * n_tty_hosttest.c
 *
 * host-deps: rach2/sys/tty/n_tty.c
 *
 * Dual-target seam test for the N_TTY line discipline (tty/n_tty.c). The cooked
 * line editor (ERASE/WERASE/KILL with ECHOE rub-out, the VEOF/newline commit
 * rules), the raw VMIN gating, the ISIG control-char recognition, and the
 * OPOST/ONLCR output mapping are pure state-machine logic every console read
 * leans on, so they run natively here under ASan/UBSan with echo and output
 * captured into a buffer instead of a screen. Returns the failed-check count
 * (0 == pass).
 */

#include "tty/n_tty.h"

static int fails;

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

static void sink(void *ctx, unsigned char c)
{
    struct cap *cp = (struct cap *)ctx;
    if (cp->len < (int)sizeof(cp->buf)) {
        cp->buf[cp->len++] = c;
    }
}

static void cap_reset(struct cap *cp)
{
    cp->len = 0;
}

/* The captured stream equals the first n bytes of expect (and is exactly n long). */
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

/* Feed one byte; return the signal the discipline recognized (0 if none). */
static int feed(struct n_tty *t, struct cap *cp, unsigned char c)
{
    int signo = -1;
    n_tty_receive(t, c, sink, cp, &signo);
    return signo;
}

/* Feed a NUL-terminated run of bytes, ignoring the per-byte signal. */
static void feed_str(struct n_tty *t, struct cap *cp, const char *s)
{
    while (*s) {
        feed(t, cp, (unsigned char)*s++);
    }
}

int main(void)
{
    struct n_tty t;
    struct cap cp;
    unsigned char buf[512];
    int n, i;

    /* --- cooked: assemble a line, ERASE rubs out, ECHOE feedback is exact --- */
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

    /* A canonical read returns at most one line; a second line waits its turn. */
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

    /* ERASE on an empty line is a harmless no-op (no underflow, no stray echo). */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, t.tio.c_cc[VERASE]);
    CHECK(cp.len == 0 && t.line_len == 0);

    /* --- VEOF: empty line is end-of-file; mid-line delivers the partial line --- */
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

    /* EOF ordering: an empty-line ^D typed BEFORE a later line is read back first
     * (read returns 0), then the line -- not the other way round. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed(&t, &cp, t.tio.c_cc[VEOF]);              /* ^D on empty line: EOF at this position */
    feed_str(&t, &cp, "x\n");                     /* ...then a line arrives before any read */
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* EOF delivered first */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'x' && buf[1] == '\n'); /* then the line */

    /* The reverse: a committed line, then ^D -- the line comes first, then EOF. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "foo\n");
    feed(&t, &cp, t.tio.c_cc[VEOF]); /* ^D on the now-empty line */
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 4 && buf[0] == 'f' && buf[3] == '\n');
    CHECK(n_tty_read(&t, buf, sizeof(buf)) == 0); /* then EOF */

    /* --- ISIG: INTR/QUIT/SUSP are recognized, echoed ^X, and flush the line --- */
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

    /* ISIG flushes the WHOLE input queue, not just the edit line: a line committed
     * (and unread) before ^C must not survive the signal. */
    n_tty_init(&t);
    cap_reset(&cp);
    feed_str(&t, &cp, "old\n"); /* committed to the read queue, unread */
    feed_str(&t, &cp, "part");  /* plus a partial edit line */
    CHECK(feed(&t, &cp, t.tio.c_cc[VINTR]) == NTTY_SIGINT);
    CHECK(n_tty_readable(&t) == 0); /* both the line and the queue are gone */
    feed_str(&t, &cp, "new\n");
    n = n_tty_read(&t, buf, sizeof(buf));
    CHECK(n == 4 && buf[0] == 'n' && buf[3] == '\n'); /* only post-signal input */

    /* --- input flag mapping: ICRNL, IGNCR, INLCR, ISTRIP --- */

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

    /* INLCR (with ICRNL off): NL becomes CR, which is an ordinary char, not a
     * terminator -- so no line completes from a lone NL. */
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

    /* --- raw mode: every byte is immediately readable, gated only by VMIN --- */
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

    /* --- output processing (OPOST/ONLCR/OCRNL), driven through n_tty_output --- */

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

    /* --- buffer bounds: a canonical line longer than the buffer is capped, the
     * terminator still lands, and read returns the whole capped line at once --- */
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

    return fails;
}
