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
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VWERASE 14

/* c_lflag bits. */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define NOFLSH  0000200 /* a signal character flushes nothing */
#define TOSTOP  0000400
#define ECHOCTL 0001000

/* c_iflag bits. */
#define ISTRIP 0000040
#define INLCR  0000100
#define IGNCR  0000200
#define ICRNL  0000400
#define IXON   0002000 /* VSTOP and VSTART stop and restart output */
#define IXANY  0004000 /* with IXON, any character restarts output */
#define IXOFF  0010000 /* kept; no terminal here sends VSTOP for its input */
#define IUTF8  0040000 /* the input is UTF-8: an erase removes a whole character */

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

/* The longest canonical line with its terminator, Linux's N_TTY_BUF_SIZE:
 * 4095 bytes and a newline. */
#define N_TTY_LINE_MAX 4096
/* The cooked or raw bytes ready for read(): a power of two, and two lines,
 * so that a whole line still fits beside a queue `n_tty_has_room` let fill. */
#define N_TTY_READ_MAX 8192
#define N_TTY_READ_CHUNK \
    1024 /* what one read() copies through its stack bounce; the caller loops */
/* The echo held while output is stopped: a power of two, Linux's echo
 * buffer's size. When it is full the oldest byte gives way. */
#define N_TTY_ECHO_MAX 4096

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
    int rq_mid;           /* the last canonical read stopped inside a line */
    int eof;              /* a pending end-of-file: a VEOF on an empty line, or a hangup */
    unsigned int eof_pos; /* the rq stream position of the pending EOF */
    /* Output is stopped, by VSTOP under IXON or by TCOOFF: writes wait and
     * echo is held in `echo_buf` until it restarts. */
    int stopped;
    int tco_stopped; /* TCOOFF stopped it, and only TCOON restarts it */
    /* Echo not yet given to the sink: held while output is stopped, and a
     * signal character's own echo, which follows its flush of the output. */
    unsigned char echo_buf[N_TTY_ECHO_MAX];
    unsigned int echo_head; /* monotonic; slot = head & (SIZE-1) */
    unsigned int echo_tail;
    struct n_tty_counters counters;
};

/**
 * Reset `t` to the default canonical termios (`n_tty_default_termios`),
 * empty buffers and zeroed counters.
 */
void n_tty_init(struct n_tty *t);

/**
 * Fill `tio` with the default canonical termios. The defaults are a
 * standard tty's: ICRNL and IXON input, OPOST and ONLCR
 * output, IUTF8, since Rach2's text is UTF-8, as Linux's console has it in
 * its UTF-8 mode, 38400 baud with eight-bit characters, ISIG and ICANON with
 * ECHO, ECHOE, ECHOK and ECHOCTL, the conventional control characters (^C,
 * ^\, DEL, ^U, ^D, ^Z, ^W, and ^Q and ^S for VSTART and VSTOP) and VMIN 1.
 */
void n_tty_default_termios(struct ktermios *tio);

/**
 * Process one received input byte.
 *
 * Applies ISTRIP, then IXON's VSTOP and VSTART, then the ISIG signal
 * characters, then IXANY, then the input flags IGNCR, ICRNL and INLCR, then
 * either canonical line editing (ERASE, WERASE, KILL, EOF, newline and EOL)
 * or, in raw mode, queues the byte: Linux's order. Echo and any output go
 * through `out`, or wait in `echo_buf` while output is stopped (`stopped`).
 * `*signo` is set to an NTTY_SIG* code when a signal character fires, else 0.
 *
 * Under IXON, VSTOP stops output and VSTART restarts it, unless TCOOFF
 * stopped it; neither reaches the reader nor is echoed, and when both are
 * the same byte it restarts. Under IXON with IXANY any other byte restarts
 * output as well, and so does a signal character under IXON alone.
 *
 * A signal character without NOFLSH discards all unread input (the line
 * under edit, committed lines and a pending EOF) and the echo held, so input
 * typed before the signal does not survive it, as Linux's isig does; its own
 * echo is then held in `echo_buf` for the caller, which discards its output
 * queue first and then emits it (`n_tty_echo_flush`). With NOFLSH it
 * discards nothing and is echoed as any byte. A `c_cc` entry of 0 is
 * disabled (the _POSIX_VDISABLE convention) and never matches a NUL byte.
 * VEOF commits the line so far; on an empty line it marks end of file at the
 * current stream position, so it is read back in order with the input around
 * it. An ordinary character is dropped when the line has one byte left, which
 * is kept for the newline or VEOL that ends it.
 */
void n_tty_receive(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int *signo);

/**
 * Process a received byte that `n_tty_lookahead` has already seen, as
 * `n_tty_receive` does, but leave out the stop or restart of a VSTOP or
 * VSTART, which the look ahead made; the byte is still not passed on.
 */
void n_tty_receive_looked(struct n_tty *t, unsigned char c, n_tty_out_fn out, void *ctx,
                          int *signo);

/**
 * Act now on a byte that waits for room ahead of the discipline, as Linux's
 * n_tty_lookahead_flow_ctrl: under IXON, VSTOP stops output and VSTART
 * restarts it, so a terminal whose input is full still obeys them. The
 * byte is later given to `n_tty_receive_looked`. Returns nonzero when
 * `stopped` changed.
 */
int n_tty_lookahead(struct n_tty *t, unsigned char c);

/** Stop output, as Linux's stop_tty. Returns nonzero when it was running. */
int n_tty_stop(struct n_tty *t);

/** Restart output unless TCOOFF stopped it, as Linux's start_tty. Returns
 * nonzero when it was stopped and now runs; the echo held waits for
 * `n_tty_echo_flush`. */
int n_tty_start(struct n_tty *t);

/**
 * Give at most `max` bytes of the echo held in `echo_buf` to `out`, oldest
 * first, unless output is stopped; the rest stays held, as Linux's
 * process_echoes stops at the driver's write room. Returns the bytes given.
 * The caller has interrupts off, since a byte received from an interrupt may
 * add to the echo meanwhile.
 */
int n_tty_echo_flush(struct n_tty *t, n_tty_out_fn out, void *ctx, unsigned int max);

/** Hold the echo byte `c` in `echo_buf`, behind the echo held, the oldest
 * byte giving way when it is full: for a sink that has no room for it. */
void n_tty_hold_echo(struct n_tty *t, unsigned char c);

/**
 * Drop the echo held since `echo_tail` was `tail`, keeping what was held
 * before it; when less is held than was added since, all of it. A caller
 * that feeds a byte with its echo suppressed calls it after the byte, so the
 * older echo waits for a sink that shows it.
 */
void n_tty_echo_cut(struct n_tty *t, unsigned int tail);

/**
 * Whether the terminal is readable to poll(2): nonzero if so, 0 if not.
 *
 * In canonical mode any queued byte is readable, and a pending EOF is too (the
 * read returns 0). In raw mode a pending EOF (a hangup: the master closed) is
 * readable as well, so a blocked reader drains the queue and then returns 0
 * instead of waiting for VMIN bytes that will never arrive; otherwise it is
 * readable with VMIN bytes when VMIN is above 0 and VTIME is 0, and otherwise
 * with one byte, as Linux's input_available_p has it, so a program that polls
 * a VMIN 0 terminal for keys sees none until one comes. A read's own test is
 * `n_tty_read_ready`.
 */
int n_tty_readable(const struct n_tty *t);

/**
 * Whether a read of `count` bytes returns now: nonzero if so, 0 if it waits.
 * `expired` is nonzero once the read's VTIME timer has run out, as
 * `n_tty_read_timer` says when it runs.
 *
 * Canonical mode and a pending EOF are as `n_tty_readable` has them. A
 * noncanonical read needs VMIN bytes, or `count` when that is fewer, and its
 * four cases are POSIX's: VMIN 0 and VTIME 0 returns at once, with what is
 * there; VMIN > 0 and VTIME 0 waits for the bytes; VMIN 0 and VTIME > 0
 * returns with the first byte, or with none when the timer runs out; VMIN > 0
 * and VTIME > 0 waits for the bytes, or returns short when the timer runs out
 * after a byte.
 */
int n_tty_read_ready(const struct n_tty *t, int count, int expired);

/* When a read's VTIME timer runs, from `n_tty_read_timer`. */
#define N_TTY_TIMER_NONE 0 /* never: canonical mode, or VTIME 0 */
#define N_TTY_TIMER_READ 1 /* VMIN 0: VTIME tenths of a second from the read's start */
#define N_TTY_TIMER_BYTE 2 /* VMIN > 0: VTIME tenths from the last byte, once one is queued */

/** When a read's VTIME timer runs, as POSIX has it: one of N_TTY_TIMER_*. */
int n_tty_read_timer(const struct n_tty *t);

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
 * readable once ICANON is off. Turning IXON off restarts output that VSTOP
 * stopped, as Linux's n_tty_set_termios does. */
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
 * Whether the last canonical read stopped inside a line, for want of room in
 * its buffer: nonzero if so, and a read now returns the rest of that line.
 * A caller that copies a read through a smaller buffer than its reader's
 * reads on while this holds, so that a read returns a whole line, as Linux's
 * does. 0 in raw mode and after a flush.
 */
int n_tty_line_open(const struct n_tty *t);

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
