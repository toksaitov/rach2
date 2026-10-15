/*
 * n_tty.c: the N_TTY line discipline, which has no kernel dependencies and is
 * host-tested.
 *
 * Input is cooked (ICANON: a line is assembled with ERASE/WERASE/KILL
 * editing and ECHO, and only becomes readable when terminated) or raw (each
 * byte is readable at once, a read waiting for VMIN of them); ISIG control
 * chars are recognized for signal delivery. Output is post-processed
 * (OPOST/ONLCR). Echo and output bytes leave through a caller sink, so
 * nothing here touches kernel I/O; the kernel points the sink at the console
 * or a pty's master ring, the host test at a buffer.
 */

#include "n_tty.h"

/* the readable-byte ring (monotonic head/tail, slot = pos & (SIZE-1)) */

/** The bytes in the read queue; the unsigned difference is right across a
 * wrap of the counters. */
static unsigned int rq_count(const struct n_tty *t)
{
    return t->rq_tail - t->rq_head;
}

/** Append `c` to the read queue as a byte that ends no line, or count it in
 * queue_drops when the queue is full. */
static void rq_push(struct n_tty *t, unsigned char c)
{
    if (rq_count(t) < N_TTY_READ_MAX) {
        t->rq[t->rq_tail & (N_TTY_READ_MAX - 1)] = c;
        t->rq_end[t->rq_tail & (N_TTY_READ_MAX - 1)] = 0;
        t->rq_tail++;
        if (rq_count(t) > t->counters.queue_max) {
            t->counters.queue_max = rq_count(t);
        }
    } else {
        t->counters.queue_drops++;
    }
}

unsigned int n_tty_held(const struct n_tty *t)
{
    return rq_count(t) + (unsigned int)t->line_len;
}

int n_tty_has_room(const struct n_tty *t)
{
    return n_tty_held(t) + N_TTY_LINE_MAX <= N_TTY_READ_MAX;
}

void n_tty_flush(struct n_tty *t)
{
    t->counters.flush_bytes += (unsigned int)t->line_len + rq_count(t);
    t->line_len = 0;
    t->rq_head = t->rq_tail;
    t->eof = 0;
}

/**
 * A partial canonical line is readable the moment ICANON is off, since raw
 * mode has no line to keep it in and a line left there would be lost (Linux
 * makes the same bytes readable). The line joins the queue behind what was
 * committed before it, which keeps the order typed; the serial and pty
 * feeders stop a whole line short of the queue's end (`n_tty_has_room`), so
 * it fits. Should it not, it stays in `line` and a raw read takes it once the
 * queue is empty, behind what was committed before it and, if any arrived
 * between, behind raw bytes that came later; nothing is dropped.
 */
int n_tty_set_termios(struct n_tty *t, const struct ktermios *tio)
{
    int was_canon = (t->tio.c_lflag & ICANON) != 0;
    int i;

    for (i = 0; i < (int)sizeof(struct ktermios); i++) {
        ((unsigned char *)&t->tio)[i] = ((const unsigned char *)tio)[i];
    }
    if (was_canon && !(t->tio.c_lflag & ICANON) && t->line_len > 0 &&
        rq_count(t) + (unsigned int)t->line_len <= N_TTY_READ_MAX) {
        for (i = 0; i < t->line_len; i++) {
            rq_push(t, t->line[i]);
        }
        t->counters.partial_moved += (unsigned int)t->line_len;
        t->line_len = 0;
    }
    return n_tty_readable(t);
}

/** The bytes a raw read has: the queue, and a canonical line left partial
 * when the mode changed that did not fit the queue then. */
static unsigned int raw_pending(const struct n_tty *t)
{
    return rq_count(t) + (unsigned int)t->line_len;
}

/** Take the oldest byte of the read queue, which must not be empty. */
static unsigned char rq_pop(struct n_tty *t)
{
    unsigned char c = t->rq[t->rq_head & (N_TTY_READ_MAX - 1)];
    t->rq_head++;
    return c;
}

/* output + echo */

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

/* canonical-mode line editing */

/** Erase the last byte of the line under edit; under ECHO and ECHOE, rub it
 * out on the terminal with backspace, space, backspace. */
static void erase_one(struct n_tty *t, n_tty_out_fn out, void *ctx)
{
    if (t->line_len > 0) {
        t->line_len--;
        if ((t->tio.c_lflag & ECHO) && (t->tio.c_lflag & ECHOE)) {
            out(ctx, '\b');
            out(ctx, ' ');
            out(ctx, '\b');
        }
    }
}

/** Erase the spaces at the end of the line under edit, then the word before
 * them (WERASE). */
static void erase_word(struct n_tty *t, n_tty_out_fn out, void *ctx)
{
    while (t->line_len > 0 && t->line[t->line_len - 1] == ' ') {
        erase_one(t, out, ctx);
    }
    while (t->line_len > 0 && t->line[t->line_len - 1] != ' ') {
        erase_one(t, out, ctx);
    }
}

/** Erase the whole line under edit (KILL). */
static void erase_line(struct n_tty *t, n_tty_out_fn out, void *ctx)
{
    while (t->line_len > 0) {
        erase_one(t, out, ctx);
    }
}

/**
 * Move the line under edit to the read queue and start an empty one. Its last
 * byte is marked in rq_end, so a canonical read stops there even when VEOL
 * changes later or VEOF committed the line without a terminator.
 */
static void commit_line(struct n_tty *t)
{
    int i;
    unsigned int start = t->rq_tail;

    for (i = 0; i < t->line_len; i++) {
        rq_push(t, t->line[i]);
    }
    if (t->rq_tail != start) {
        t->rq_end[(t->rq_tail - 1) & (N_TTY_READ_MAX - 1)] = 1;
    }
    t->line_len = 0;
}

void n_tty_init(struct n_tty *t)
{
    int i;

    t->tio.c_iflag = ICRNL;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_cflag = B38400 | CS8 | CREAD | HUPCL;
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
    for (i = 0; i < (int)sizeof(t->counters); i++) {
        ((unsigned char *)&t->counters)[i] = 0;
    }
    t->eof_pos = 0;
}

void n_tty_receive(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int *signo)
{
    struct ktermios *tio = &t->tio;

    *signo = 0;

    if (tio->c_iflag & ISTRIP) {
        c = (unsigned char)(c & 0x7f);
    }
    if (c == '\r') {
        if (tio->c_iflag & IGNCR) {
            return;
        }
        if (tio->c_iflag & ICRNL) {
            c = '\n';
        }
    } else if (c == '\n') {
        if (tio->c_iflag & INLCR) {
            c = '\r';
        }
    }

    if (tio->c_lflag & ISIG) {
        if ((tio->c_cc[VINTR] && c == tio->c_cc[VINTR]) ||
            (tio->c_cc[VQUIT] && c == tio->c_cc[VQUIT]) ||
            (tio->c_cc[VSUSP] && c == tio->c_cc[VSUSP])) {
            echo(t, out, ctx, c);
            n_tty_flush(t);
            *signo = (c == tio->c_cc[VINTR])   ? NTTY_SIGINT
                     : (c == tio->c_cc[VQUIT]) ? NTTY_SIGQUIT
                                               : NTTY_SIGTSTP;
            return;
        }
    }

    if (tio->c_lflag & ICANON) {
        if (tio->c_cc[VERASE] && c == tio->c_cc[VERASE]) {
            erase_one(t, out, ctx);
            return;
        }
        if (tio->c_cc[VWERASE] && c == tio->c_cc[VWERASE]) {
            erase_word(t, out, ctx);
            return;
        }
        if (tio->c_cc[VKILL] && c == tio->c_cc[VKILL]) {
            erase_line(t, out, ctx);
            return;
        }
        if (tio->c_cc[VEOF] && c == tio->c_cc[VEOF]) {
            if (t->line_len > 0) {
                commit_line(t);
            } else if (!t->eof) {
                t->eof = 1;
                t->eof_pos = t->rq_tail;
            }
            return;
        }
        if (c == '\n' || (tio->c_cc[VEOL] && c == tio->c_cc[VEOL])) {
            echo(t, out, ctx, c);
            if (t->line_len < N_TTY_LINE_MAX) {
                t->line[t->line_len++] = c;
            }
            commit_line(t);
            return;
        }
        if (t->line_len < N_TTY_LINE_MAX - 1) {
            t->line[t->line_len++] = c;
            echo(t, out, ctx, c);
            if ((unsigned int)t->line_len > t->counters.line_max) {
                t->counters.line_max = (unsigned int)t->line_len;
            }
        } else {
            t->counters.line_drops++;
        }
        return;
    }

    rq_push(t, c);
    echo(t, out, ctx, c);
}

int n_tty_readable(const struct n_tty *t)
{
    if (t->tio.c_lflag & ICANON) {
        if (rq_count(t) > 0) {
            return (int)rq_count(t);
        }
        return t->eof ? 1 : 0;
    } else {
        unsigned int vmin = t->tio.c_cc[VMIN];
        if (t->eof) {
            return raw_pending(t) > 0 ? (int)raw_pending(t) : 1;
        }
        if (vmin == 0) {
            return 1;
        }
        return raw_pending(t) >= vmin ? (int)raw_pending(t) : 0;
    }
}

int n_tty_inq(const struct n_tty *t)
{
    if (t->tio.c_lflag & ICANON) {
        return (int)rq_count(t);
    }
    return (int)raw_pending(t);
}

int n_tty_read(struct n_tty *t, unsigned char *buf, int count)
{
    int canon = (t->tio.c_lflag & ICANON) != 0;
    int n = 0;

    if (count <= 0) {
        return 0;
    }
    if (canon) {
        if (t->eof && t->rq_head == t->eof_pos) {
            t->eof = 0;
            return 0;
        }
        while (n < count && rq_count(t) > 0) {
            unsigned char c;
            if (t->eof && t->rq_head == t->eof_pos) {
                break;
            }
            c = rq_pop(t);
            buf[n++] = c;
            if (c == '\n' || t->rq_end[(t->rq_head - 1) & (N_TTY_READ_MAX - 1)]) {
                break;
            }
        }
        t->counters.read_bytes += (unsigned int)n;
        return n;
    }
    while (n < count && rq_count(t) > 0) {
        buf[n++] = rq_pop(t);
    }
    if (n < count && t->line_len > 0) {
        int taken = 0, i;

        while (n < count && taken < t->line_len) {
            buf[n++] = t->line[taken++];
        }
        for (i = taken; i < t->line_len; i++) {
            t->line[i - taken] = t->line[i];
        }
        t->line_len -= taken;
        t->counters.partial_moved += (unsigned int)taken;
    }
    t->counters.read_bytes += (unsigned int)n;
    return n;
}
