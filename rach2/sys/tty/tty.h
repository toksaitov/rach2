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
    uint32_t ready_us;          /* timer_now_us() when input became readable; 0 once read */
    struct tty_winsize winsize; /* TIOCGWINSZ window size */
    n_tty_out_fn sink;          /* echo/output byte sink; a NULL at tty_init discards */
    void *sink_ctx;             /* opaque context handed to sink */
    /* The bytes `sink` can take now, as Linux's tty_write_room: echo and
     * output stop there and wait; NULL for a sink that takes every byte. */
    unsigned int (*room)(struct tty *t);
    void (*pump)(struct tty *t); /* optional polled input drain (NULL = none) */
    /* A pty slave's flush (`TTY_FLUSH_*`); NULL for a terminal with no
     * master. */
    void (*flush)(struct tty *t, int what);
    /* What a change of output flow does beyond the terminal (`TTY_FLOW_*`):
     * a pty's packet status, the console's Scroll Lock LED; NULL for
     * nothing. Called with interrupts off, from an interrupt as well. */
    void (*flow)(struct tty *t, int what);
    int sid;     /* the session whose controlling terminal this is; 0 = none */
    int fg_pgid; /* foreground process group (ISIG target); 0 = none */
    int hungup;  /* tty_hangup ran: a read with nothing to take returns 0 */
    /* The vhangup(2)s of the terminal (`tty_vhangup`). A description opened
     * when the count was lower is hung up: its reads give 0, its writes and
     * ioctls EIO (`tty_desc_hung`). */
    uint32_t vhangups;
};

/* What a slave's `flush` does, as Linux's pty_flush_buffer and
 * n_tty_packet_mode_flush. TTY_FLUSH_IN reports TIOCPKT_FLUSHREAD to a
 * master in packet mode after the discipline's input was discarded;
 * TTY_FLUSH_OUT discards the output still queued for the master and reports
 * TIOCPKT_FLUSHWRITE; TTY_FLUSH_HANGUP discards that output and drops every
 * pending status bit, as `__tty_hangup` ends with pktstatus 0. */
#define TTY_FLUSH_IN     1
#define TTY_FLUSH_OUT    2
#define TTY_FLUSH_HANGUP 4

/* What a terminal's `flow` is told, as Linux's driver start and stop and
 * pty_set_termios see it. TTY_FLOW_STOP and TTY_FLOW_START: output stopped or
 * restarted (TIOCPKT_STOP, TIOCPKT_START). TTY_FLOW_DOSTOP and
 * TTY_FLOW_NOSTOP: IXON with ^S and ^Q was turned on or off
 * (TIOCPKT_DOSTOP, TIOCPKT_NOSTOP). TTY_FLOW_DATA: bytes reached the sink
 * outside a write, the echo held while output was stopped or a TCIOFF or
 * TCION character, and the reader of the output is to be woken. */
#define TTY_FLOW_STOP   1
#define TTY_FLOW_START  2
#define TTY_FLOW_DOSTOP 4
#define TTY_FLOW_NOSTOP 8
#define TTY_FLOW_DATA   16

/* tcflow(3)'s actions, the argument of TCXONC. */
#define TTY_TCOOFF 0
#define TTY_TCOON  1
#define TTY_TCIOFF 2
#define TTY_TCION  3

/* Whether a description of `t` opened when its count of vhangups was
 * `opened` is hung up. */
#define tty_desc_hung(t, opened) ((t)->vhangups != (opened))

/* What poll gives a hung-up description, as Linux's `hung_up_tty_poll`:
 * POLLIN, POLLOUT, POLLERR, POLLHUP, POLLRDNORM and POLLWRNORM. */
#define TTY_HUNG_POLL 0x15D

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
 * and its receive notify registered with the serial driver; then the
 * screen's blanking and backlights (`blank_init`).
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
 * The job-control check of a background process on its controlling terminal
 * `t`, as Linux's tty_check_change for `signo` SIGTTOU and the read's check
 * for SIGTTIN. Returns 0 when the caller is not in the background of `t`, or
 * blocks or ignores SIGTTOU; -EIO when it blocks or ignores SIGTTIN, or its
 * group is orphaned; and otherwise sends `signo` to its group and returns
 * -EINTR. A read of `t` runs it for SIGTTIN; a write under TOSTOP, and
 * TCSETS and its kin, TCFLSH, TCXONC and TIOCSPGRP, for SIGTTOU.
 */
int tty_job_check(struct tty *t, int signo);

/**
 * Run the screen's blanking countdown (`blank_tick`) and service the
 * console's serial port, from the boot processor's timer tick with
 * interrupts off; the port's interrupt services the port alone. Servicing
 * it moves the UART's bytes to the ring, at every tick until the receive
 * interrupt has taken a byte and every fifth of a second after, then wakes
 * a reader or poller waiting on the console, or, with none waiting and a
 * session owning the console, drains the ring into the discipline without
 * echo.
 */
void tty_console_poll(void);

/**
 * Feed one input byte from an interrupt or a polled source: run it through
 * the discipline (editing, and echo through the sink), wake a waiting reader
 * once a line or byte is ready, and post the signal of a signal character to
 * the foreground process group. A signal character without NOFLSH also
 * discards the output queued (`flush`, TTY_FLUSH_IN and TTY_FLUSH_OUT) before
 * its echo, as Linux's isig. A VSTOP or VSTART that stops or restarts output
 * tells `flow`, and a restart wakes the writers. A byte that takes bytes out
 * of the discipline, the flush of a signal character or an erase, word
 * erase or kill, wakes `space_wq` for room. Safe with interrupts off and in
 * interrupt context.
 */
void tty_input_char(struct tty *t, unsigned char c);

/**
 * Feed a byte `tty_lookahead` has seen, as `tty_input_char` does, without
 * acting again on its VSTOP or VSTART.
 */
void tty_input_looked(struct tty *t, unsigned char c);

/**
 * Act now on a byte that waits for room in `t`'s discipline: a VSTOP or
 * VSTART under IXON (`n_tty_lookahead`), as Linux looks ahead in the bytes
 * its discipline cannot take yet. A restart with `echo` nonzero gives the
 * echo held to the sink, as far as its room allows, as Linux's VSTART runs
 * process_echoes; with `echo` 0 the echo waits for the next input or write.
 * The byte is later fed with `tty_input_looked`. Returns nonzero when the
 * byte stopped or restarted output. Takes interrupts off itself.
 */
int tty_lookahead(struct tty *t, unsigned char c, int echo);

/**
 * tcflow(3), TCXONC with `action` one of TTY_TC*: TCOOFF stops output until
 * TCOON, which nothing else restarts; TCOON restarts it; TCIOFF and TCION
 * write VSTOP or VSTART, unless disabled, to the terminal's output past a
 * stop, as Linux's tty_send_xchar, except a pty's under TCOOFF. Returns 0,
 * or -EINVAL for another action.
 */
int tty_tcflow(struct tty *t, int action);

/**
 * Wait until output stopped on `t` restarts, for a writer whose description
 * was opened at `opened` vhangups. Returns 0 when it runs, -EAGAIN at once
 * when it is stopped and `nonblock` is set, -EIO when a vhangup has hung the
 * description up, or -EINTR when a signal ends the wait. The wait drains
 * the pump, as a read's does, so a VSTART from COM1 restarts it. Thread
 * context.
 */
int tty_wait_output(struct tty *t, int nonblock, uint32_t opened);

/**
 * Wait until `t` has input a read can take, then copy up to `count` bytes of
 * it into `kbuf`.
 *
 * Returns the byte count, 0 at end of file, after a hangup with nothing
 * queued and when a noncanonical read's VTIME timer runs out with no byte,
 * or -EINTR when a signal ends the wait. The wait is `n_tty_read_ready`'s,
 * with the VTIME timer `n_tty_read_timer` describes armed in the process's
 * `wakeup_tick` and cleared before the return. With `nonblock` set it does
 * not wait: it drains the pump and returns -EAGAIN when nothing is ready (a
 * pending end of file counts as ready) and the terminal has not hung up.
 * Thread context, with a current process; it takes interrupts off itself.
 */
int tty_read(struct tty *t, unsigned char *kbuf, int count, int nonblock);

/**
 * Copy up to `count` bytes more of the canonical line the last read of `t`
 * stopped inside (`n_tty_line_open`) into `kbuf`, without waiting.
 *
 * Returns the byte count, or 0 when no line is open, as after a read that
 * ended at its line's end or when another reader took the rest. A read of
 * the console or a slave calls it after `tty_read` while its user buffer has
 * room, so that one read returns a line longer than its bounce buffer whole.
 * Thread context; it takes interrupts off itself.
 */
int tty_read_rest(struct tty *t, unsigned char *kbuf, int count);

/**
 * Write `count` bytes of `kbuf` through output processing (OPOST, ONLCR) to
 * the sink, after the echo held. Returns the bytes written, fewer than
 * `count` when output is stopped, where the caller waits
 * (`tty_wait_output`), or when the sink's `room` cannot take a byte at its
 * largest; a tty write does not wait itself. The echo held goes only as far
 * as the room allows, and the rest stays held. On the
 * console the write is bracketed by console_write_begin and
 * console_write_end, so a kernel log record raised during it waits and is not
 * printed inside the program's output, and the screen's answers to queries in
 * it are then fed back as input.
 */
int tty_write(struct tty *t, const unsigned char *kbuf, int count);

/**
 * Install `kt` on `t`, as TCSETS and its kin do; with `flush_input`, discard
 * the unread input first, on the console the serial ring too, and report the
 * flush through `flush` (TTY_FLUSH_IN) as TCSETSF does on Linux. Wakes
 * `space_wq` for room, and the readers when the change makes input readable.
 * A change of whether IXON is on with ^S and ^Q tells `flow`
 * (TTY_FLOW_DOSTOP, TTY_FLOW_NOSTOP), as Linux's pty_set_termios; turning
 * IXON off restarts output VSTOP stopped. Takes interrupts off itself.
 */
void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input);

/**
 * poll(2) readiness: POLLIN when a read would not block, and POLLOUT while
 * output is not stopped. A query with POLLIN, or with POLLOUT on stopped
 * output, drains the polled source and sets *wq to input_wq for the poll
 * core to re-check on. Called by the poll core with interrupts off.
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
 * Set an EOF marker after queued input, mark `t` hung up and wake every
 * reader.
 *
 * A canonical read consumes the marker once; `hungup` stays, so the later
 * reads and the other waiting readers return 0 once the queue is empty.
 */
void tty_hangup(struct tty *t);

/**
 * Hang `t` up as vhangup(2) does, Linux's `__tty_hangup` for a terminal that
 * a session leader's exit does not hang up: post SIGHUP and then SIGCONT to
 * the leader of the session `t` controls, take the terminal from that
 * session and its foreground group, discard unread input, queued output and
 * the echo held, give `t` the default termios (`n_tty_default_termios`), as
 * Linux resets a pty's and a virtual console's, and count the hangup, which
 * makes every description opened before it a hung-up one (`tty_desc_hung`).
 * A stop of output stays, as Linux's flow state does. Every reader, writer
 * and poller of `t` is woken; a read waiting then returns 0. A pty's master
 * is left no pending packet status (TTY_FLUSH_HANGUP). Thread context; it
 * takes interrupts off itself.
 */
void tty_vhangup(struct tty *t);

/**
 * The console keyboard's Hold, Scroll Lock in the US keymap: stop the
 * console's output when it runs and restart it when it is stopped, unless
 * TCOOFF stopped it, as Linux's fn_hold. Any context.
 */
void tty_console_hold(void);

/** The console's count of vhangups, which a description of it opened now
 * records. */
uint32_t tty_console_vhangups(void);

#endif /* _TTY_H_ */
