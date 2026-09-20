#ifndef _N_TTY_H_
#define _N_TTY_H_

#include "../lib/kint.h"

/*
 * The N_TTY line discipline: the cooked/raw input editor and output
 * processor a terminal sits behind. It is pure logic over a fixed-size
 * state, with echo and output emitted through a caller-supplied byte sink,
 * so it compiles freestanding and hosted. The kernel wires the sink to the
 * console (VGA + serial) and acts on the recognized signals; the host test
 * captures the sink into a buffer.
 */

/*
 * Kernel termios: the TCGETS/TCSETS struct, the 19-c_cc prefix of musl's
 * larger userspace struct termios (NCCS=32 plus speeds). The kernel
 * reads/writes only this prefix; baud lives in c_cflag, so the speeds are
 * musl's concern. 36 bytes.
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

/* c_cc indices (i386/asm-generic; verified against the vendored musl
 * headers). */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSUSP   10
#define VEOL    11
#define VWERASE 14

/* c_lflag bits. */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define TOSTOP  0000400
#define ECHOCTL 0001000

/* c_iflag bits. */
#define ISTRIP 0000040
#define INLCR  0000100
#define IGNCR  0000200
#define ICRNL  0000400

/* c_oflag bits. */
#define OPOST 0000001
#define ONLCR 0000004
#define OCRNL 0000010

/* Signals the discipline recognizes from c_cc (Linux numbers); delivery to the
 * foreground group is the caller's job. */
#define NTTY_SIGINT  2
#define NTTY_SIGQUIT 3
#define NTTY_SIGTSTP 20

#define N_TTY_LINE_MAX 256  /* longest editable canonical line */
#define N_TTY_READ_MAX 4096 /* the cooked/raw bytes ready for read(); a power of two */
#define N_TTY_READ_CHUNK \
    1024 /* what one read() copies through its stack bounce; the caller loops */

/* What the discipline did with its input, for `/proc/console`: the bytes
 * it dropped and why, the bytes it handed to readers, and the most it held. */
struct n_tty_counters {
    unsigned int line_drops;    /* bytes dropped from a full canonical line */
    unsigned int queue_drops;   /* bytes dropped from a full read queue */
    unsigned int flush_bytes;   /* bytes discarded by a signal, TCSETSF or TCFLSH */
    unsigned int partial_moved; /* bytes of a partial canonical line made readable in raw mode */
    unsigned int read_bytes;    /* bytes handed to readers */
    unsigned int queue_max;     /* the most the read queue held */
    unsigned int line_max;      /* the longest canonical line held */
};

/* The sink echo and output bytes are emitted through. Must be non-NULL (the tty
 * layer substitutes a discarding sink when a caller passes none). */
typedef void (*n_tty_out_fn)(void *ctx, unsigned char c);

struct n_tty {
    struct ktermios tio;
    unsigned char line[N_TTY_LINE_MAX]; /* the canonical line under edit */
    int line_len;
    unsigned char rq[N_TTY_READ_MAX]; /* ring of bytes ready for read() */
    unsigned char rq_end[N_TTY_READ_MAX];
    unsigned int rq_head; /* monotonic; slot = head & (SIZE-1) */
    unsigned int rq_tail;
    int eof;              /* a VEOF on an empty line -> a pending end-of-file */
    unsigned int eof_pos; /* the rq stream position the pending EOF sits at */
    struct n_tty_counters counters;
};

/** Reset to default cooked-mode termios and empty buffers. */
void n_tty_init(struct n_tty *t);

/** Process one received input byte; sets `*signo` to a NTTY_SIG* code or 0. */
void n_tty_receive(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int *signo);

/** Bytes a read could return now, or 0 if it would block. */
int n_tty_readable(const struct n_tty *t);

/** Whether the discipline can take a whole line more without dropping. */
int n_tty_has_room(const struct n_tty *t);

/** Install `tio`. Returns what a read could return now, since a partial
 * canonical line is readable once ICANON is off. */
int n_tty_set_termios(struct n_tty *t, const struct ktermios *tio);

/** Discard every unread byte, the partial line and a pending EOF. */
void n_tty_flush(struct n_tty *t);

/** Copy available input into `buf`; returns the byte count, 0 at
 * end-of-file. */
int n_tty_read(struct n_tty *t, unsigned char *buf, int count);

/** Count of bytes the next read can return (the TIOCINQ value). */
int n_tty_inq(const struct n_tty *t);

/** Emit one output byte through OPOST processing. */
void n_tty_output(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx);

#endif /* _N_TTY_H_ */
