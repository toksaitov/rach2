#ifndef _N_TTY_H_
#define _N_TTY_H_

#include "../lib/kint.h"

/*
 * The N_TTY line discipline -- the cooked/raw input editor and output processor a
 * terminal sits behind. It is pure logic over a fixed-size state, with echo and
 * output emitted through a caller-supplied byte sink, so it compiles freestanding
 * AND hosted and is unit-tested under ASan (tests/host/n_tty_hosttest.c). The
 * kernel wires the sink to the console (VGA + serial) and acts on the recognized
 * signals; the host test captures the sink into a buffer.
 */

/*
 * Kernel termios -- the TCGETS/TCSETS struct, the 19-c_cc prefix of musl's larger
 * userspace struct termios (NCCS=32 + speeds). The kernel reads/writes only this
 * prefix; baud lives in c_cflag, so the speeds are musl's concern. 36 bytes.
 */
#define KNCCS 19
struct ktermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[KNCCS];
};

/* c_cc indices (i386/asm-generic; verified against the vendored musl headers). */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSUSP 10
#define VEOL 11
#define VWERASE 14

/* c_lflag bits. */
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define TOSTOP 0000400
#define ECHOCTL 0001000

/* c_iflag bits. */
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400

/* c_oflag bits. */
#define OPOST 0000001
#define ONLCR 0000004
#define OCRNL 0000010

/* Signals the discipline recognizes from c_cc (Linux numbers); delivery is the
 * caller's job (to the foreground group, in Phase 17). */
#define NTTY_SIGINT 2
#define NTTY_SIGQUIT 3
#define NTTY_SIGTSTP 20

#define N_TTY_LINE_MAX 256 /* longest editable canonical line */
#define N_TTY_READ_MAX 512 /* the cooked/raw bytes ready for read() */

/* The sink echo and output bytes are emitted through. Must be non-NULL (the tty
 * layer substitutes a discarding sink when a caller passes none). */
typedef void (*n_tty_out_fn)(void *ctx, unsigned char c);

struct n_tty {
    struct ktermios tio;
    unsigned char line[N_TTY_LINE_MAX]; /* the canonical line under edit */
    int line_len;
    unsigned char rq[N_TTY_READ_MAX]; /* ring of bytes ready for read() */
    unsigned int rq_head;             /* monotonic; slot = head & (SIZE-1) */
    unsigned int rq_tail;
    int eof;              /* a VEOF on an empty line -> a pending end-of-file */
    unsigned int eof_pos; /* the rq stream position the pending EOF sits at */
};

/* Sane cooked defaults (ICANON|ECHO|ISIG, ICRNL, OPOST|ONLCR, the usual c_cc). */
void n_tty_init(struct n_tty *t);

/*
 * Feed one raw input byte through the discipline. Echo (and erase feedback) is
 * emitted via out(ctx, ..). *signo is set to NTTY_SIG* when an ISIG control char
 * is seen (0 otherwise) -- the caller delivers it.
 */
void n_tty_receive(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int *signo);

/* How many bytes read() may return right now: in canonical mode the size of the
 * first completed line (0 until a line terminates); in raw mode the queued count
 * once it reaches VMIN. A pending EOF counts as "readable" (read returns 0). */
int n_tty_readable(const struct n_tty *t);

/* Pull readable bytes into buf (canonical: at most one line, stopping after the
 * terminator; raw: up to count). Returns the byte count, or 0 at end-of-file. */
int n_tty_read(struct n_tty *t, unsigned char *buf, int count);

/* Bytes a read would return right now -- the count in the ready queue (FIONREAD).
 * A pending EOF reads as 0 bytes, so it counts as 0 here (unlike n_tty_readable,
 * which reports it as "ready"). */
int n_tty_inq(const struct n_tty *t);

/* Output processing for one write() byte (OPOST/ONLCR/OCRNL), emitted via out. */
void n_tty_output(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx);

#endif /* _N_TTY_H_ */
