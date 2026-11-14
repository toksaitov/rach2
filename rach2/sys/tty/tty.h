/*
 * tty.h: the terminal object behind the console and each pty slave, and the
 * calls the system calls, the pty driver and the input drivers make on it.
 */
#ifndef _TTY_H_
#define _TTY_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "n_tty.h"
#include "../proc/wait_queue.h"
#include "../proc/ns.h"

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
    /* Set for a terminal whose echo is held at receipt, which may be in an
     * interrupt, and given to the sink later from a thread with interrupts
     * on: by a writer of the terminal, or by the thread this wakes, the
     * console's echo thread. Called with interrupts off, from an
     * interrupt as well, while echo is held and output runs. NULL for a
     * terminal whose receipt gives its echo to the sink, a pty slave's. */
    void (*echo_kick)(struct tty *t);
    int sid;     /* the session whose controlling terminal this is; 0 = none */
    int fg_pgid; /* foreground process group (ISIG target); 0 = none */
    /* The `pgrp_serial` (process.h) of the foreground group, which tells it
     * from a later group given `fg_pgid`, as Linux's tty->ctrl.pgrp is a
     * reference to one struct pid; 0 with `fg_pgid` 0. Once the group has
     * ended, `fg_pgid` still reads as its number and no signal reaches any
     * group. */
    uint64_t fg_serial;
    /* The foreground group's numbers in its PID namespace and those above
     * it, which TIOCGPGRP reads once the group has ended and the numbers'
     * record is gone (`tty_fg_seen`). */
    struct pid_nums fg_nums;
    int hungup; /* tty_hangup ran: a read with nothing to take returns 0 */
    /* The hangups of the terminal (`tty_vhangup`). A description opened
     * when the count was lower is hung up, but the console's own
     * (`tty_console` in fd.h): its reads give 0, its writes and ioctls EIO
     * (`tty_desc_hung`). */
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
 * its echo held at receipt for its echo thread (`echo_kick`), and its
 * receive notify registered with the serial driver; then the screen's
 * blanking and backlights (`blank_init`).
 */
void tty_console_init(void);

/**
 * Start the console's echo thread, `kconsoled`, which gives the echo held
 * to the screen and COM1 a batch at a time, with interrupts on and a
 * service point after each batch, as Linux's n_tty echoes from its receive
 * worker. Process context, once init has its pid, as the kernel's other
 * threads start; before it, the echo waits for the console's next writer.
 */
void tty_console_thread_start(void);

/** Post `signo` to every live user process of process group `pgid` whose
 * `pgrp_serial` (process.h) is `serial`, the lifetime of the id that the
 * caller holds; a `pgid` of 0 posts nothing. */
void tty_signal_group(int pgid, uint64_t serial, int signo);

/**
 * Set `t`'s session and foreground group, the group `pgid` of the lifetime
 * `serial`, together, with interrupts off, so the console's interrupt-side
 * drain never runs between the stores. Used to claim the console (exec) and
 * to release a terminal (0, 0, 0).
 */
void tty_set_session(struct tty *t, int sid, int pgid, uint64_t serial);

/**
 * Give the foreground of `t` to the group `pgid` of the lifetime `serial`,
 * with interrupts off, as TIOCSPGRP does.
 */
void tty_set_foreground(struct tty *t, int pgid, uint64_t serial);

/** The foreground group of `t` as the current process's PID namespace sees
 * it, its number even once it has ended, as Linux's pid_vnr of the
 * terminal's struct pid; 0 for none, or for a group outside that
 * namespace. */
uint32_t tty_fg_seen(const struct tty *t);

/**
 * Whether `t` is the controlling terminal of the process of `p`: the
 * terminal of its session, which it has not left by TIOCNOTTY
 * (`tty_detached` in tgroup.h), as Linux's `p->signal->tty == t`.
 */
int tty_is_ctty(const struct tty *t, const process_t *p);

/**
 * Set the window size of `t` to `*ws`, as Linux's tty_do_resize: when the
 * size changes, the foreground group gets SIGWINCH.
 */
void tty_resize(struct tty *t, const struct tty_winsize *ws);

/**
 * Release `t` from the session it controls when that session's leader gives
 * it up, as Linux's disassociate_ctty: its foreground group, if it has one,
 * gets SIGHUP, and SIGCONT after it when `on_exit` is 0, which TIOCNOTTY
 * passes and the leader's end does not; then the terminal has no session and
 * no foreground group. The leader's end on the console is
 * `tty_vhangup_session`'s instead, as on Linux for any terminal but a pty.
 */
void tty_disassociate(struct tty *t, int on_exit);

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
 * Whether a process of session `sid` and group `pgid` of the lifetime
 * `serial` is in the background on `t`: `t` is its controlling terminal and
 * another group, or an ended one given the same id, is in the foreground, as
 * Linux compares struct pids. The callers stop such a reader with SIGTTIN,
 * and such a writer with SIGTTOU under TOSTOP.
 */
int tty_is_background(struct tty *t, int sid, int pgid, uint64_t serial);

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
 * it moves the UART's bytes to the ring, at every tick while the receive
 * interrupt is off and every fifth of a second while it is on, then, while
 * a session owns the console, drains at most 64 bytes of the ring into the
 * discipline with their echo held for later, and wakes a reader or poller
 * waiting on the console.
 */
void tty_console_poll(void);

/**
 * Feed one input byte from an interrupt or a polled source: run it through
 * the discipline (editing, and echo through the sink, or held for later
 * where `echo_kick` is set, whose call it then makes), wake a waiting reader
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
 * process_echoes, or wakes `echo_kick`'s thread for it where the echo is
 * deferred; with `echo` 0 the echo waits for the next input or write, or
 * for that thread, which any restart wakes.
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
 * was opened at `*opened` vhangups, or which no hangup reaches when `opened`
 * is NULL. Returns 0 when it runs, -EAGAIN at once when it is stopped and
 * `nonblock` is set, -EIO when a vhangup has hung the description up, or
 * -EINTR when a signal ends the wait. The wait drains the pump, as a read's
 * does, so a VSTART from COM1 restarts it. Thread context.
 */
int tty_wait_output(struct tty *t, int nonblock, const uint32_t *opened);

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
 * `opened` is the count of vhangups the read belongs to: its description's
 * (`tty_hangups` of `struct open_file`), or the count when the read began
 * for a description no hangup reaches. Once `t`'s count differs, the read
 * drains and takes nothing and returns 0, even with a later session's input
 * queued, as Linux's hung-up file reads end of file and its hangup aborts
 * every reader. Thread context, with a current process; it takes interrupts
 * off itself.
 */
int tty_read(struct tty *t, unsigned char *kbuf, int count, int nonblock, uint32_t opened);

/**
 * Copy up to `count` bytes more of the canonical line the last read of `t`
 * stopped inside (`n_tty_line_open`) into `kbuf`, without waiting.
 *
 * Returns the byte count, or 0 when no line is open, as after a read that
 * ended at its line's end or when another reader took the rest, or when a
 * vhangup has come since `opened`, the count `tty_read` was given, during
 * the caller's copy to the user. A read of the console or a slave calls it
 * after `tty_read` while its user buffer has room, so that one read returns
 * a line longer than its bounce buffer whole. Thread context; it takes
 * interrupts off itself.
 */
int tty_read_rest(struct tty *t, unsigned char *kbuf, int count, uint32_t opened);

/**
 * Write `count` bytes of `kbuf` through output processing (OPOST, ONLCR) to
 * the sink, after the echo held. Returns the bytes written, fewer than
 * `count` when output is stopped, where the caller waits
 * (`tty_wait_output`), or when the sink's `room` cannot take a byte at its
 * largest; a tty write does not wait itself. The echo held goes only as far
 * as the room allows, or one batch of it on a terminal that defers its echo
 * (`echo_kick`), and the rest stays held. On the
 * console the write is bracketed by console_write_begin and
 * console_write_end, so a kernel log record raised during it waits and is not
 * printed inside the program's output, and the screen's answers to queries in
 * it are then fed back as input.
 */
int tty_write(struct tty *t, const unsigned char *kbuf, int count);

/**
 * Write `count` bytes of the user's `buf` to `t`, for a description opened
 * at `*opened` vhangups, or one no hangup reaches when `opened` is NULL.
 *
 * The bytes go in pieces of 256 through a kernel buffer and `tty_write`.
 * While output is stopped the write waits for it to restart
 * (`tty_wait_output`), or returns on a nonblocking description. The copy
 * from the user may sleep on a fault, and the scheduler may run others
 * between pieces, so the hangup is tested before each `tty_write`, as
 * Linux's n_tty_write tests tty_hung_up_p on every turn. On a terminal
 * that defers its echo (`echo_kick`), the echo held goes first, a batch at
 * a time with a service point after each, and the hangup and output's flow
 * are tested again after each. Returns `count`,
 * or the bytes written before a fault, a hangup, a signal or a nonblocking
 * stop, or when none were: -EFAULT, -EIO, -EINTR or -EAGAIN. Thread
 * context.
 */
int32_t tty_write_user(struct tty *t, const char __user *buf, uint32_t count, int nonblock,
                       const uint32_t *opened);

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
 * output is not stopped. Every query sets *wq to input_wq for the poll core
 * to re-check on, one with no events among them, since a hangup wakes that
 * queue; one with POLLIN, or with POLLOUT on stopped output, drains the
 * polled source. Takes interrupts off itself.
 */
short tty_poll(struct tty *t, short events, wait_queue_t **wq);

/**
 * poll(2) readiness of a descriptor of the console, whose description was
 * opened at `*opened` vhangups, or which no hangup reaches when `opened` is
 * NULL, and which is `readable` when it was opened for reading. A
 * description hung up answers TTY_HUNG_POLL whatever was asked, as Linux's
 * hung_up_tty_poll does, and sets no queue. Any other answers through
 * `tty_poll`: POLLIN, whose query drains serial input, only when readable,
 * so poll(stdout, POLLOUT) never drains serial input, and POLLOUT for any
 * descriptor, as a terminal answers on Linux whatever the descriptor's
 * mode, so that a caller polling a read-only one for output is not left
 * waiting; its write is then EBADF. Every such poller, one that asks for no
 * events among them, waits on the queue the hangup wakes.
 */
short tty_console_desc_poll(const uint32_t *opened, int readable, short events, wait_queue_t **wq);

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
 * Hang `t` up as vhangup(2) does, Linux's `__tty_hangup`: post SIGHUP and
 * then SIGCONT to the leader of the session `t` controls, take the terminal
 * from that session and its foreground group, discard unread input, queued
 * output and the echo held, give `t` the default termios
 * (`n_tty_default_termios`), as Linux resets a pty's and a virtual
 * console's, and count the hangup, which makes every description opened
 * before it a hung-up one (`tty_desc_hung`), but the console's own
 * (`tty_console` in fd.h), as Linux's hangup passes over a file of
 * /dev/console. A stop of output stays, as Linux's flow state does. Every
 * reader, writer and poller of `t` is woken; a read waiting then returns 0,
 * as Linux aborts every reader during a hangup. A pty's master is left no
 * pending packet status (TTY_FLUSH_HANGUP). Thread context; it takes
 * interrupts off itself.
 */
void tty_vhangup(struct tty *t);

/**
 * Hang up `t`, the console, at the end of the leader of the session it
 * controls, as Linux's disassociate_ctty calls tty_vhangup_session for any
 * terminal but a pty: as `tty_vhangup`, but the foreground group gets
 * SIGHUP, the leader, which is ending, nothing, and COM1's ring, not yet
 * the terminal's input, keeps its bytes.
 */
void tty_vhangup_session(struct tty *t);

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
