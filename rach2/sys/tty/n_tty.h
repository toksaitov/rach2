/*
 * n_tty.h: the N_TTY line discipline, the canonical and raw input editor and
 * the output processor of a terminal, with the kernel termios it works from.
 *
 * The discipline has no kernel dependencies: its state is one fixed-size
 * structure, and echo and output leave through a byte sink the caller
 * supplies, so it compiles freestanding and hosted. The kernel points the sink
 * at the console (the screen and the serial port) or at a pty's master ring
 * and acts on the signals it recognizes; the host test collects the sink's
 * bytes in a buffer.
 */
#ifndef _N_TTY_H_
#define _N_TTY_H_

#include "../lib/kint.h"

/*
 * Kernel termios: the TCGETS/TCSETS struct, the 19-c_cc prefix of musl's
 * larger userspace struct termios (NCCS=32 plus speeds). The kernel reads and
 * writes only this prefix; the baud rate is a code in c_cflag, and the
 * termios2 of TCGETS2 adds the speeds its codes stand for. 36 bytes.
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

/* c_cc indices: Linux's values on i386 and x86-64, which musl's headers
 * use. */
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

/* c_cflag bits: the speed code 38400 and the character size, receiver and
 * hang-up bits a new Linux terminal starts with (tty_std_termios). */
#define B38400 0000017
#define CS8    0000060
#define CREAD  0000200
#define HUPCL  0002000

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
    /* 1 at the slot of a committed line's last byte, where a canonical read
     * stops; 0 elsewhere. */
    unsigned char rq_end[N_TTY_READ_MAX];
    unsigned int rq_head; /* monotonic; slot = head & (SIZE-1) */
    unsigned int rq_tail;
    int eof;              /* a pending end-of-file: a VEOF on an empty line, or a hangup */
    unsigned int eof_pos; /* the rq stream position of the pending EOF */
    struct n_tty_counters counters;
};

/**
 * Reset `t` to the default canonical termios, empty buffers and zeroed
 * counters. The defaults are a standard tty's: ICRNL input, OPOST and ONLCR
 * output, 38400 baud with eight-bit characters, ISIG and ICANON with ECHO,
 * ECHOE, ECHOK and ECHOCTL, the conventional control characters (^C, ^\, DEL,
 * ^U, ^D, ^Z, ^W) and VMIN 1.
 */
void n_tty_init(struct n_tty *t);

/**
 * Process one received input byte.
 *
 * Applies the input flags (ISTRIP, IGNCR, ICRNL, INLCR), then the ISIG signal
 * characters, then either canonical line editing (ERASE, WERASE, KILL, EOF,
 * newline and EOL) or, in raw mode, queues the byte. Echo and any output go
 * through `out`. `*signo` is set to an NTTY_SIG* code when a signal character
 * fires, else 0.
 *
 * A signal character is echoed and discards all unread input (the line under
 * edit, committed lines and a pending EOF), so input typed before the signal
 * does not survive it, as Linux does without NOFLSH. A `c_cc` entry of 0 is
 * disabled (the _POSIX_VDISABLE convention) and never matches a NUL byte.
 * VEOF commits the line so far; on an empty line it marks end of file at the
 * current stream position, so it is read back in order with the input around
 * it. An ordinary character is dropped when the line has one byte left, which
 * is kept for the newline or VEOL that ends it.
 */
void n_tty_receive(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int *signo);

/**
 * Whether a read would return now: nonzero if so, 0 if it would block.
 *
 * In canonical mode any queued byte is readable, and a pending EOF is too (the
 * read returns 0). In raw mode a pending EOF (a hangup: the master closed) is
 * readable as well, so a blocked reader drains the queue and then returns 0
 * instead of waiting for VMIN bytes that will never arrive; otherwise VMIN 0
 * never blocks and a VMIN > 0 read waits until at least VMIN bytes are there.
 *
 * VTIME is stored (a TCSETS with it does not fail) but not implemented: a
 * program that sets VMIN > 0 with VTIME > 0 gets a plain blocking read for
 * VMIN bytes, without the inter-byte timeout that would let it return short.
 */
int n_tty_readable(const struct n_tty *t);

/**
 * Whether the discipline can take a whole line more without dropping: the
 * bytes it holds plus N_TTY_LINE_MAX fit the read queue. A feeder that stops
 * when this is false leaves its bytes where they are, and the sender waits
 * instead of losing them.
 */
int n_tty_has_room(const struct n_tty *t);

/** The bytes the discipline holds, the read queue and the line under edit:
 * what `n_tty_has_room` counts as taken. */
unsigned int n_tty_held(const struct n_tty *t);

/** Install `tio`. Returns nonzero when a read would not block, so the caller
 * can wake a reader the change made ready: a partial canonical line is
 * readable once ICANON is off. */
int n_tty_set_termios(struct n_tty *t, const struct ktermios *tio);

/** Discard every unread byte, the partial line and a pending EOF, counting
 * them in `flush_bytes`. */
void n_tty_flush(struct n_tty *t);

/**
 * Copy available input into `buf`; returns the byte count, 0 at end of file.
 *
 * A canonical read returns at most one line and stops at a pending EOF, which
 * it reports as a 0-length read once the head reaches the EOF position, so
 * input committed before the EOF is returned first and input committed after
 * it next. A raw read returns up to `count` bytes with no line structure: the
 * queue, then a partial canonical line a mode switch could not queue.
 */
int n_tty_read(struct n_tty *t, unsigned char *buf, int count);

/**
 * Count of bytes queued for reading (the TIOCINQ value).
 *
 * In canonical mode only completed lines reach the queue (the line under edit
 * is not counted), and a read returns only the first of them; a pending EOF
 * has no bytes, so EOF alone counts as 0. In raw mode the count includes a
 * partial line a mode switch could not queue.
 */
int n_tty_inq(const struct n_tty *t);

/**
 * Emit one output byte through OPOST processing.
 *
 * With OPOST set, ONLCR maps `\n` to CR LF and OCRNL maps `\r` to `\n`; any
 * other byte, or output with OPOST clear, passes through unchanged.
 */
void n_tty_output(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx);

#endif /* _N_TTY_H_ */
