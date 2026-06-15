#ifndef _TTY_H_
#define _TTY_H_

#include "../lib/kint.h"
#include "n_tty.h"
#include "../proc/wait_queue.h"

/*
 * A terminal -- the kernel object a console (and, in Phase 18, a pty) sits
 * behind. The N_TTY line discipline (see n_tty.h) does the cooked/raw editing,
 * echo, and ISIG recognition over an input ring; readers block on input_wq until
 * a line (or a raw byte) is ready; an input source -- the keyboard IRQ, the
 * serial poll -- feeds bytes in via tty_input_char, and write output and echo
 * leave through `sink`. `pump`, when set, is a polled input source (the
 * console's serial port) drained before each read-sleep, so a headless host with
 * no input IRQ still makes progress on each timer wake.
 */

/* The TIOCGWINSZ/TIOCSWINSZ window size (Linux `struct winsize`). */
struct tty_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct tty {
    struct n_tty ldisc;          /* line discipline: termios + cooked line + read queue */
    wait_queue_t input_wq;       /* readers parked until input is ready */
    struct tty_winsize winsize;  /* TIOCGWINSZ window size */
    n_tty_out_fn sink;           /* echo/output byte sink (NULL discards) */
    void *sink_ctx;              /* opaque context handed to sink */
    void (*pump)(struct tty *t); /* optional polled input drain (NULL = none) */
    int sid;                     /* controlling session id (0 = none) (Phase 17) */
    int fg_pgid;                 /* foreground process group (ISIG target); 0 = none */
};

/* The system console: VGA + serial, bound to every process's fds 0/1/2. */
extern struct tty console_tty;

/* Bring a tty up: cooked defaults, empty queue, 25x80 window, the given sink. */
void tty_init(struct tty *t, n_tty_out_fn sink, void *sink_ctx);

/* Bring up console_tty (sink = VGA + serial, pump = serial drain). Boot-time. */
void tty_console_init(void);

/* Post `signo` to every live member of process group `pgid` (job-control routing). */
void tty_signal_group(int pgid, int signo);

/* Set the controlling session and foreground group as one interrupts-off step,
 * so the timer-IRQ drain never sees a torn (sid set, fg_pgid stale) owner. Used
 * to hand the console to a launched program (exec) and to release it (0, 0). */
void tty_set_session(struct tty *t, int sid, int pgid);

/* True iff (sid, pid) is the session leader that owns the console (console_tty.sid
 * == sid && sid == pid). The condition under which a process exiting/dying/exec'ing
 * must release (and a successful exec re-publish) the controlling terminal, so the
 * timer-IRQ serial drain never targets a vanishing session. False for a non-leader
 * member of the owning session. */
int tty_console_owned_by_leader(int sid, int pid);

/* True if the tty is the caller's controlling terminal but its group is not the
 * foreground -- a background process, whose read/write is gated by SIGTTIN/SIGTTOU. */
int tty_is_background(struct tty *t, int sid, int pgid);

/* Timer-tick hook: drain the console's (interrupt-less) serial port into the
 * discipline when a reader/poller is waiting, so serial input wakes them even
 * while another process runs. A no-op when nobody is waiting. */
void tty_console_poll(void);

/*
 * Feed one input byte from an IRQ or poll source: run it through the discipline
 * (cook + echo via sink), wake a blocked reader once a line/byte is ready, and
 * for an ISIG control char post its signal to the foreground process. Safe to
 * call with interrupts off / from interrupt context.
 */
void tty_input_char(struct tty *t, unsigned char c);

/*
 * Blocking read into a kernel buffer: park on input_wq (lost-wakeup-safe, the
 * pump drained before each sleep) until a line or raw byte is ready, then return
 * up to count bytes. Returns the byte count, 0 at end-of-file, or -EINTR.
 */
int tty_read(struct tty *t, unsigned char *kbuf, int count);

/* Write count kernel bytes through output processing (OPOST/ONLCR) to the sink.
 * Returns count (a console write never blocks or fails). */
int tty_write(struct tty *t, const unsigned char *kbuf, int count);

/* Install a new termios (TCSETS family). Done with interrupts off so an input
 * IRQ never reads a half-updated termios; flush_input also drops pending input. */
void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input);

/* poll(2) readiness: POLLIN when a read would not block, plus the writable bit;
 * *wq is set to input_wq for the poll core to re-check on. */
short tty_poll(struct tty *t, short events, wait_queue_t **wq);

/* FIONREAD: bytes a read would return now. Drains the polled source (serial)
 * first, interrupts off, so input already arrived is counted -- matching tty_poll. */
int tty_inq(struct tty *t);

/* TCFLSH / TCSETSF input flush: discard the cooked line under edit and every
 * queued-but-unread byte. */
void tty_flush_input(struct tty *t);

/* Hang up: mark end-of-file at the current input position and wake all readers,
 * so a blocked reader/poller returns EOF (used when a pty master closes on its
 * slave). Already-queued input is still read first. */
void tty_hangup(struct tty *t);

#endif /* _TTY_H_ */
