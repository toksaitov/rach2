/*
 * tty.h: the terminal object behind the console and each pty slave, and the
 * calls the system calls, the pty driver and the input drivers make on it.
 */
#ifndef _TTY_H_
#define _TTY_H_

#include "../lib/kint.h"
#include "n_tty.h"
#include "../proc/wait_queue.h"

/*
 * A terminal. The N_TTY line discipline (n_tty.h) edits canonical lines,
 * echoes and recognizes the signal characters over its read queue. An input
 * source (the keyboard drivers, the console's serial drain, a pty master's
 * write) feeds bytes in through tty_input_char; readers wait on input_wq until
 * a line, or in raw mode enough bytes, is ready; output and echo leave through
 * `sink`. `pump`, when set, is a polled input source (the console's serial
 * ring) drained before each read wait, poll and FIONREAD, so a port whose
 * interrupt is off still delivers its bytes at each timer wake.
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
    /* Woken with WQ_ROOM when input leaves the discipline; NULL for none. */
    wait_queue_t *space_wq;
    uint32_t ready_us;           /* timer_now_us() when input became readable; 0 once read */
    struct tty_winsize winsize;  /* TIOCGWINSZ window size */
    n_tty_out_fn sink;           /* echo/output byte sink; a NULL at tty_init discards */
    void *sink_ctx;              /* opaque context handed to sink */
    void (*pump)(struct tty *t); /* optional polled input drain (NULL = none) */
    int sid;                     /* the session whose controlling terminal this is; 0 = none */
    int fg_pgid;                 /* foreground process group (ISIG target); 0 = none */
};

/* The system console: the screen and COM1, the tty behind every console
 * descriptor (FD_TYPE_CONSOLE), as a new process's descriptors 0, 1 and 2
 * are. */
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

/** Copy the console's counters out, with interrupts off. */
void tty_console_counters(struct console_counters *out);

/**
 * Initialize `t`: the discipline's cooked defaults, an empty queue, a 25-row
 * 80-column window, `sink` (NULL for one that discards), and no session, pump
 * or `space_wq`. An input producer may then set `space_wq` to be woken for
 * room: after a flush, a termios change, or an input byte that took bytes out
 * of the discipline.
 */
void tty_init(struct tty *t, n_tty_out_fn sink, void *sink_ctx);

/**
 * Initialize console_tty, once at boot: output to the screen and the serial
 * port, the serial ring as its pump, the text console's size as its window,
 * and its receive notify registered with the serial driver.
 */
void tty_console_init(void);

/** Post `signo` to every live user process of process group `pgid`; a `pgid`
 * of 0 posts nothing. */
void tty_signal_group(int pgid, int signo);

/**
 * Set `t`'s session and foreground group together, with interrupts off, so
 * the console's interrupt-side drain never runs between the two stores. Used
 * to claim the console (exec) and to release it (0, 0).
 */
void tty_set_session(struct tty *t, int sid, int pgid);

/**
 * Whether (sid, pid) is the leader of the session whose controlling terminal
 * is the console: console_tty.sid is `sid`, nonzero, and `sid` is `pid`.
 *
 * This is the condition under which a process that exits, is killed or
 * replaces its image releases the console (and on a successful exec publishes
 * it again), so the interrupt-side serial drain never targets a session that
 * is going away.
 */
int tty_console_owned_by_leader(int sid, int pid);

/**
 * Whether a process of session `sid` and group `pgid` is in the background on
 * `t`: `t` is its controlling terminal and another group is in the
 * foreground. The callers stop such a reader with SIGTTIN, and such a writer
 * with SIGTTOU under TOSTOP.
 */
int tty_is_background(struct tty *t, int sid, int pgid);

/**
 * Service the console's serial port, from the timer tick and from the port's
 * interrupt, with interrupts off: move the UART's bytes to the ring, then
 * wake a reader or poller waiting on the console, or, with none waiting and a
 * session owning the console, drain the ring into the discipline without
 * echo.
 */
void tty_console_poll(void);

/**
 * Feed one input byte from an interrupt or a polled source: run it through
 * the discipline (editing, and echo through the sink), wake a waiting reader
 * once a line or byte is ready, and post the signal of a signal character to
 * the foreground process group. A byte that takes bytes out of the
 * discipline, the flush of a signal character or an erase, word erase or
 * kill, wakes `space_wq` for room. Safe with interrupts off and in interrupt
 * context.
 */
void tty_input_char(struct tty *t, unsigned char c);

/**
 * Wait until `t` has input a read can take, then copy up to `count` bytes of
 * it into `kbuf`.
 *
 * Returns the byte count, 0 at end of file, or -EINTR when a signal ends the
 * wait. With `nonblock` set it does not wait: it drains the pump and returns
 * -EAGAIN when nothing is ready (a pending end of file counts as ready).
 * Thread context; it takes interrupts off itself.
 */
int tty_read(struct tty *t, unsigned char *kbuf, int count, int nonblock);

/**
 * Write `count` bytes of `kbuf` through output processing (OPOST, ONLCR) to
 * the sink. Returns `count`: a tty write neither waits nor fails. On the
 * console the write is bracketed by console_write_begin and
 * console_write_end, so a kernel log record raised during it waits and is not
 * printed inside the program's output, and the screen's answers to queries in
 * it are then fed back as input.
 */
int tty_write(struct tty *t, const unsigned char *kbuf, int count);

/**
 * Install `kt` on `t`, as TCSETS and its kin do; with `flush_input`, discard
 * the unread input first, on the console the serial ring too. Wakes
 * `space_wq` for room, and the readers when the change makes input readable.
 * Takes interrupts off itself.
 */
void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input);

/**
 * poll(2) readiness: POLLIN when a read would not block, and POLLOUT, which
 * is always true. Only a query with POLLIN drains the polled source and sets
 * *wq to input_wq for the poll core to re-check on. Called by the poll core
 * with interrupts off.
 */
short tty_poll(struct tty *t, short events, wait_queue_t **wq);

/**
 * FIONREAD: the bytes ready to read, every completed line in canonical mode.
 * Drains the polled source (serial) first, interrupts off, so input already
 * arrived is counted (matching tty_poll).
 */
int tty_inq(struct tty *t);

/**
 * TCFLSH input flush: discard the line under edit, every queued-but-unread
 * byte and a pending EOF, and on the console the serial ring as well; wakes
 * space_wq. Takes interrupts off itself.
 */
void tty_flush_input(struct tty *t);

/**
 * Set an EOF marker after queued input and wake every reader.
 *
 * A canonical read consumes the marker once, so the endpoint keeps its own
 * record of the hangup for later reads and for other waiting readers.
 */
void tty_hangup(struct tty *t);

#endif /* _TTY_H_ */
