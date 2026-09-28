#ifndef _TTY_H_
#define _TTY_H_

#include "../lib/kint.h"
#include "n_tty.h"
#include "../proc/wait_queue.h"

/*
 * A terminal: the kernel object behind a console or a pty. The N_TTY line
 * discipline (see n_tty.h) does the cooked/raw editing, echo, and ISIG
 * recognition over an input ring; readers block on input_wq until a line (or
 * a raw byte) is ready; an input source (the keyboard IRQ, the serial pump, a
 * pty master's write) feeds bytes in via tty_input_char, and write output and
 * echo leave through `sink`. `pump`, when set, is a polled input source (the
 * console's serial ring) drained before each read-sleep, poll and FIONREAD,
 * so a port whose interrupt is off still makes progress on each timer wake.
 */

/* The TIOCGWINSZ/TIOCSWINSZ window size (Linux `struct winsize`). */
struct tty_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct tty {
    struct n_tty ldisc;    /* line discipline: termios + cooked line + read queue */
    wait_queue_t input_wq; /* readers parked until input is ready */
    wait_queue_t *space_wq;
    uint32_t ready_us;           /* when a line became readable, until a reader has it */
    struct tty_winsize winsize;  /* TIOCGWINSZ window size */
    n_tty_out_fn sink;           /* echo/output byte sink; a NULL at tty_init discards */
    void *sink_ctx;              /* opaque context handed to sink */
    void (*pump)(struct tty *t); /* optional polled input drain (NULL = none) */
    int sid;                     /* controlling session id (0 = none) */
    int fg_pgid;                 /* foreground process group (ISIG target); 0 = none */
};

/* The system console: VGA + serial, bound to every process's fds 0/1/2. */
extern struct tty console_tty;

/* The console's receive path, end to end, for `/proc/console`. */
struct console_counters {
    uint32_t uart_bytes;    /* taken from the UART */
    uint32_t uart_overruns; /* overruns the line status reported */
    uint32_t irq_drains;    /* services run from the port's interrupt */
    uint32_t irq_bytes;     /* bytes those services took */
    uint32_t tick_drains;   /* drains into the discipline from the tick or the interrupt */
    uint32_t reader_drains; /* drains by a reader, a poller or FIONREAD */
    uint32_t ring_full;     /* services stopped at a full ring, the sender held */
    uint32_t ring_max;      /* the most the ring held */
    uint32_t shell_bytes;   /* taken by the kernel's own polls */
    struct n_tty_counters ldisc;
};

/** Copy the console's counters out. */
void tty_console_counters(struct console_counters *out);

/* Bring a tty up: cooked defaults, empty queue, 25x80 window, the given
 * sink. */
void tty_init(struct tty *t, n_tty_out_fn sink, void *sink_ctx);

/* Bring up console_tty (sink = VGA + serial, pump = serial drain) and
 * register its receive notify with the serial driver. Boot-time. */
void tty_console_init(void);

/** Post `signo` to every live member of process group `pgid`. */
void tty_signal_group(int pgid, int signo);

/** Publish the controlling session and foreground group as one atomic step. */
void tty_set_session(struct tty *t, int sid, int pgid);

/** True iff (sid, pid) is the session leader that owns the console. */
int tty_console_owned_by_leader(int sid, int pid);

/** True if the caller is a background process on this tty (gated by
 * SIGTTIN/SIGTTOU). */
int tty_is_background(struct tty *t, int sid, int pgid);

/* Service the console's serial port, from the timer tick and from the port's
 * interrupt, with interrupts off: move the UART's bytes to the ring, then
 * wake a parked reader or poller, or, with none parked and a session owning
 * the console, drain the ring into the discipline without echo. */
void tty_console_poll(void);

/*
 * Feed one input byte from an IRQ or poll source: run it through the discipline
 * (cook + echo via sink), wake a blocked reader once a line/byte is ready, and
 * for an ISIG control char post its signal to the foreground process group.
 * A byte that takes bytes out of the discipline, the flush of a signal char or
 * an erase, word erase or kill, wakes `space_wq` for room. Safe to call with
 * interrupts off / from interrupt context.
 */
void tty_input_char(struct tty *t, unsigned char c);

/** Read into `kbuf`; returns the byte count, 0 at EOF, -EINTR, or (when
 * `nonblock` is set and nothing is ready) -EAGAIN. */
int tty_read(struct tty *t, unsigned char *kbuf, int count, int nonblock);

/* Write count kernel bytes through output processing (OPOST/ONLCR) to the sink.
 * Returns count (a console write never blocks or fails). On the console the
 * write is bracketed by console_write_begin/end, so a kernel log record raised
 * during it waits and is not printed inside the program's output. */
int tty_write(struct tty *t, const unsigned char *kbuf, int count);

/* Install a new termios (TCSETS family). Done with interrupts off so an input
 * IRQ never reads a half-updated termios; flush_input also drops pending input.
 */
void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input);

/* poll(2) readiness: POLLIN when a read would not block, and POLLOUT, which
 * is always true. Only a query with POLLIN drains the polled source and sets
 * *wq to input_wq for the poll core to re-check on. Called by the poll core
 * with interrupts off. */
short tty_poll(struct tty *t, short events, wait_queue_t **wq);

/* FIONREAD: the bytes ready to read, every completed line in canonical mode.
 * Drains the polled source (serial) first, interrupts off, so input already
 * arrived is counted (matching tty_poll). */
int tty_inq(struct tty *t);

/* TCFLSH input flush: discard the line under edit, every queued-but-unread
 * byte and a pending EOF, and on the console the serial ring as well; wakes
 * space_wq. */
void tty_flush_input(struct tty *t);

/**
 * Set an EOF marker after queued input and wake readers.
 *
 * Canonical reads consume this marker once. The endpoint must maintain
 * persistent hangup state for subsequent reads and other blocked readers.
 */
void tty_hangup(struct tty *t);

#endif /* _TTY_H_ */
