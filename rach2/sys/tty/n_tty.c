/*
 * n_tty.c -- the N_TTY line discipline (pure logic; host-tested).
 *
 * Input is cooked (ICANON: a line is assembled with ERASE/WERASE/KILL editing and
 * ECHO, and only becomes readable when terminated) or raw (each byte is readable at
 * once, gated by VMIN); ISIG control chars are recognized for signal delivery. Output
 * is post-processed (OPOST/ONLCR). Echo and output bytes leave through a caller sink,
 * so nothing here touches kernel I/O -- the kernel points the sink at the console, the
 * host test at a buffer.
 */

#include "n_tty.h"

/* --- the readable-byte ring (monotonic head/tail, slot = pos & (SIZE-1)) --- */

static unsigned int rq_count(const struct n_tty *t)
{
    return t->rq_tail - t->rq_head; /* unsigned: correct across wrap */
}

static void rq_push(struct n_tty *t, unsigned char c)
{
    if (rq_count(t) < N_TTY_READ_MAX) {
        t->rq[t->rq_tail & (N_TTY_READ_MAX - 1)] = c;
        t->rq_tail++;
    }
}

static unsigned char rq_pop(struct n_tty *t)
{
    unsigned char c = t->rq[t->rq_head & (N_TTY_READ_MAX - 1)];
    t->rq_head++;
    return c;
}

/* --- output + echo --- */

/**
 * Emit one output byte through OPOST processing.
 *
 * With OPOST set, ONLCR maps `\n` to CR LF and OCRNL maps `\r` to `\n`; any
 * other byte, or output with OPOST clear, passes through unchanged.
 */
void n_tty_output(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx)
{
    if (t->tio.c_oflag & OPOST) {
        if (c == '\n' && (t->tio.c_oflag & ONLCR)) {
            out(ctx, '\r');
            out(ctx, '\n');
            return;
        }
        if (c == '\r' && (t->tio.c_oflag & OCRNL)) {
            out(ctx, '\n');
            return;
        }
    }
    out(ctx, c);
}

/**
 * Echo an input byte through output processing.
 *
 * A control char is rendered as `^X` when ECHOCTL is set. A no-op unless ECHO
 * is on.
 */
static void echo(struct n_tty *t, n_tty_out_fn out, void *ctx, unsigned char c)
{
    if (!(t->tio.c_lflag & ECHO)) {
        return;
    }
    if (c < 0x20 && c != '\n' && c != '\t' && (t->tio.c_lflag & ECHOCTL)) {
        n_tty_output(t, '^', out, ctx);
        n_tty_output(t, (unsigned char)(c | 0x40), out, ctx);
    } else {
        n_tty_output(t, c, out, ctx);
    }
}

/* --- canonical-mode line editing --- */

static void erase_one(struct n_tty *t, n_tty_out_fn out, void *ctx)
{
    if (t->line_len > 0) {
        t->line_len--;
        if ((t->tio.c_lflag & ECHO) && (t->tio.c_lflag & ECHOE)) {
            out(ctx, '\b'); /* visually rub out: back up, blank, back up */
            out(ctx, ' ');
            out(ctx, '\b');
        }
    }
}

static void erase_word(struct n_tty *t, n_tty_out_fn out, void *ctx)
{
    while (t->line_len > 0 && t->line[t->line_len - 1] == ' ') {
        erase_one(t, out, ctx);
    }
    while (t->line_len > 0 && t->line[t->line_len - 1] != ' ') {
        erase_one(t, out, ctx);
    }
}

static void erase_line(struct n_tty *t, n_tty_out_fn out, void *ctx)
{
    while (t->line_len > 0) {
        erase_one(t, out, ctx);
    }
}

/**
 * Move the assembled line into the readable queue and start a fresh line.
 */
static void commit_line(struct n_tty *t)
{
    int i;
    for (i = 0; i < t->line_len; i++) {
        rq_push(t, t->line[i]);
    }
    t->line_len = 0;
}

/**
 * Reset a line discipline to its default cooked-mode termios and empty buffers.
 *
 * Defaults match a standard tty: ICRNL input, OPOST|ONLCR output, and
 * ISIG|ICANON with ECHO editing, plus the conventional control chars (^C, ^\,
 * DEL, ^U, ^D, ^Z, ^W) and VMIN 1.
 */
void n_tty_init(struct n_tty *t)
{
    int i;

    t->tio.c_iflag = ICRNL;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_cflag = 0;
    t->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL;
    t->tio.c_line = 0;
    for (i = 0; i < KNCCS; i++) {
        t->tio.c_cc[i] = 0;
    }
    t->tio.c_cc[VINTR] = 3;     /* ^C */
    t->tio.c_cc[VQUIT] = 28;    /* ^\ */
    t->tio.c_cc[VERASE] = 0177; /* DEL */
    t->tio.c_cc[VKILL] = 21;    /* ^U */
    t->tio.c_cc[VEOF] = 4;      /* ^D */
    t->tio.c_cc[VSUSP] = 26;    /* ^Z */
    t->tio.c_cc[VWERASE] = 23;  /* ^W */
    t->tio.c_cc[VMIN] = 1;

    t->line_len = 0;
    t->rq_head = 0;
    t->rq_tail = 0;
    t->eof = 0;
    t->eof_pos = 0;
}

/**
 * Process one received input byte through the line discipline.
 *
 * Applies input flags (ISTRIP, IGNCR, ICRNL, INLCR), then ISIG signal chars,
 * then either canonical line editing (ERASE/WERASE/KILL/EOF/EOL) or raw queuing.
 * Echo and any output go through `out`. `*signo` is set to a NTTY_SIG* code when
 * an ISIG char fires, else 0.
 */
void n_tty_receive(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int *signo)
{
    struct ktermios *tio = &t->tio;

    *signo = 0;

    /* input flag mapping. */
    if (tio->c_iflag & ISTRIP) {
        c = (unsigned char)(c & 0x7f);
    }
    if (c == '\r') {
        if (tio->c_iflag & IGNCR) {
            return; /* drop CR entirely */
        }
        if (tio->c_iflag & ICRNL) {
            c = '\n';
        }
    } else if (c == '\n') {
        if (tio->c_iflag & INLCR) {
            c = '\r';
        }
    }

    /* ISIG: an INTR/QUIT/SUSP char is recognized, echoed, flushes the line, and is
     * reported for delivery to the foreground group. */
    if (tio->c_lflag & ISIG) {
        if (c == tio->c_cc[VINTR] || c == tio->c_cc[VQUIT] || c == tio->c_cc[VSUSP]) {
            echo(t, out, ctx, c);
            /* Flush ALL unread input -- the edit line AND any committed-but-unread
             * lines and a pending EOF -- so input typed before the signal does not
             * survive it (the default !NOFLSH behaviour). */
            t->line_len = 0;
            t->rq_head = t->rq_tail;
            t->eof = 0;
            *signo = (c == tio->c_cc[VINTR]) ? NTTY_SIGINT
                                             : (c == tio->c_cc[VQUIT]) ? NTTY_SIGQUIT : NTTY_SIGTSTP;
            return;
        }
    }

    if (tio->c_lflag & ICANON) {
        if (c == tio->c_cc[VERASE]) {
            erase_one(t, out, ctx);
            return;
        }
        if (c == tio->c_cc[VWERASE]) {
            erase_word(t, out, ctx);
            return;
        }
        if (c == tio->c_cc[VKILL]) {
            erase_line(t, out, ctx);
            return;
        }
        if (c == tio->c_cc[VEOF]) {
            /* ^D delivers the line so far; on an empty line it is end-of-file,
             * marked at the current stream position so it is read back in order
             * relative to any input that follows it. */
            if (t->line_len > 0) {
                commit_line(t);
            } else if (!t->eof) {
                t->eof = 1;
                t->eof_pos = t->rq_tail;
            }
            return;
        }
        if (c == '\n' || (tio->c_cc[VEOL] && c == tio->c_cc[VEOL])) {
            echo(t, out, ctx, '\n');
            if (t->line_len < N_TTY_LINE_MAX) {
                t->line[t->line_len++] = '\n';
            }
            commit_line(t);
            return;
        }
        /* an ordinary character joins the line (dropped if the line is full). */
        if (t->line_len < N_TTY_LINE_MAX - 1) {
            t->line[t->line_len++] = c;
            echo(t, out, ctx, c);
        }
        return;
    }

    /* raw mode: the byte is immediately readable. */
    rq_push(t, c);
    echo(t, out, ctx, c);
}

/**
 * Report how many bytes a read could return now, or 0 if it would block.
 *
 * In canonical mode any queued byte means a completed line is ready, and a
 * pending EOF counts as readable (the read returns 0). In raw mode VMIN 0 never
 * blocks; otherwise the queue must hold at least VMIN bytes.
 */
int n_tty_readable(const struct n_tty *t)
{
    if (t->tio.c_lflag & ICANON) {
        /* bytes reach the queue only when a line completes, so any queued byte
         * means a line is ready; a pending EOF is "readable" (read returns 0). */
        if (rq_count(t) > 0) {
            return (int)rq_count(t);
        }
        return t->eof ? 1 : 0;
    } else {
        unsigned int vmin = t->tio.c_cc[VMIN];
        if (vmin == 0) {
            return 1; /* a polling read never blocks */
        }
        return rq_count(t) >= vmin ? (int)rq_count(t) : 0;
    }
}

/**
 * Return the count of bytes the next read can return (the TIOCINQ value).
 *
 * Only completed lines reach the queue (the edit line is not counted), and a
 * pending EOF holds no bytes, so the ready-queue count is exact, EOF reading
 * as 0.
 */
int n_tty_inq(const struct n_tty *t)
{
    return (int)rq_count(t);
}

/**
 * Copy available input into `buf`, returning the byte count.
 *
 * A canonical read returns at most one line and stops at a pending EOF, which
 * it reports as a 0-length read once the head reaches the EOF position. A raw
 * read returns up to `count` queued bytes with no line structure.
 */
int n_tty_read(struct n_tty *t, unsigned char *buf, int count)
{
    int canon = (t->tio.c_lflag & ICANON) != 0;
    int n = 0;

    if (count <= 0) {
        return 0;
    }
    if (canon) {
        /* A pending EOF is delivered (read returns 0) only once the read head has
         * reached the position it was typed at -- so input committed before the
         * EOF is returned first, and input committed after is returned next. */
        if (t->eof && t->rq_head == t->eof_pos) {
            t->eof = 0;
            return 0;
        }
        while (n < count && rq_count(t) > 0) {
            unsigned char c;
            if (t->eof && t->rq_head == t->eof_pos) {
                break; /* stop at the EOF boundary; the next read returns 0 */
            }
            c = rq_pop(t);
            buf[n++] = c;
            if (c == '\n') {
                break; /* a canonical read returns at most one line */
            }
        }
        return n;
    }
    /* raw mode: up to count bytes, no line structure. */
    while (n < count && rq_count(t) > 0) {
        buf[n++] = rq_pop(t);
    }
    return n;
}
