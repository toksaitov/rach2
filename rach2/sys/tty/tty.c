/*
 * tty.c: the terminal object, an N_TTY line discipline over a wait-queue of
 * readers, plus the one console_tty wired to the VGA screen and the serial
 * port.
 *
 * The discipline (n_tty.c) is pure and host-tested; this file is the kernel
 * glue that blocks/wakes readers, posts ISIG signals, and drives real
 * hardware. A reader parks in tty_read until tty_input_char (from the
 * keyboard IRQ or the serial pump) completes a line; output and echo flow
 * through a per-tty sink.
 */

#include "tty.h"
#include "../latency.h"
#include "../pit.h"

#include "../usb/usb.h"
#include "../io/poll.h"
#include "../serial.h"
#include "../kio.h"
#include "../klog.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"

/* Most serial bytes drained into the discipline per interrupt-side pump
 * call, so a flood cannot hold interrupts off for an unbounded stretch. The
 * reader's own drain empties the ring, since a reader that parks with bytes
 * still in the ring is woken for them only by the tick; it echoes only when
 * the ring held no more than a keyboard's worth at its start, because an
 * echo waits on the UART's transmitter for 87 microseconds a byte with
 * interrupts off, and a flood typed by a harness is not read by a person. */
#define TTY_PUMP_BUDGET 64
#define TTY_ECHO_MAX    64

/* A sink that discards: a tty initialized without one still has a valid (never
 * NULL) sink, so the discipline's echo/output never jumps through NULL. */
static void null_sink(void *ctx, unsigned char c)
{
    (void)ctx;
    (void)c;
}

void tty_init(struct tty *t, n_tty_out_fn sink, void *sink_ctx)
{
    n_tty_init(&t->ldisc);
    wait_queue_init(&t->input_wq);
    t->winsize.ws_row = 25;
    t->winsize.ws_col = 80;
    t->winsize.ws_xpixel = 0;
    t->winsize.ws_ypixel = 0;
    t->sink = sink ? sink : null_sink;
    t->sink_ctx = sink_ctx;
    t->pump = NULL;
    t->sid = 0;
    t->fg_pgid = 0;
}

/** Post a signal to every live, user member of process group `pgid`. */
void tty_signal_group(int pgid, int signo)
{
    process_t *p;

    if (pgid == 0) {
        return;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->state != PROC_ZOMBIE && p->cr3 != 0 && (int)p->pgid == pgid) {
            signal_post(p, signo);
        }
    }
}

/**
 * Deliver an ISIG-recognized signal to the tty's foreground process group.
 *
 * The target is the foreground group, not current_proc, which (when input is
 * fed from an IRQ) may be an unrelated process, and not a single process: a ^C
 * reaches every member of the foreground group.
 */
static void tty_deliver_signal(struct tty *t, int signo)
{
    tty_signal_group(t->fg_pgid, signo);
}

/**
 * Report whether the caller is a background process on this tty.
 *
 * True when this tty is the caller's controlling terminal (same session) but
 * the caller's group is not the foreground group: a background process whose
 * tty I/O must be gated by SIGTTIN/SIGTTOU.
 */
int tty_is_background(struct tty *t, int sid, int pgid)
{
    return t->sid != 0 && t->sid == sid && t->fg_pgid != 0 && t->fg_pgid != pgid;
}

/**
 * Publish the controlling session and foreground group as one indivisible step.
 *
 * The timer-IRQ drain (tty_console_poll) gates on sid != 0 and then delivers
 * ISIG to fg_pgid by value, so a torn update (sid already set while fg_pgid is
 * still the stale 0) would route a serial ^C/^Z to group 0 and drop it.
 * Interrupts off makes the two stores atomic against that drain. Used both to
 * claim the console (exec) and to release it (sid = pgid = 0).
 */
void tty_set_session(struct tty *t, int sid, int pgid)
{
    uint32_t flags;

    flags = kirq_save();
    t->sid = sid;
    t->fg_pgid = pgid;
    kirq_restore(flags);
}

/**
 * Report whether (sid, pid) is the session leader that owns the console.
 *
 * True iff the session has the controlling terminal (console_tty.sid ==
 * sid) and the process is that session's leader (sid == pid). This is the
 * exact condition under which a process exiting, being killed, or replacing
 * its image must release (and on a successful exec re-publish) the
 * controlling terminal, so the timer-IRQ serial drain never targets a
 * session that is vanishing. It is true for the session leader that owns the
 * console: the login shell a real init spawned (which `setsid`s and claims
 * the console via TIOCSCTTY), a program the kernel debug shell exec'd (for
 * which `exec_program` publishes the sid), or any later leader that claimed
 * it via TIOCSCTTY; it is false for a mere job-group member of the owning
 * session, whose exit must not pull the terminal out from under the shell.
 */
int tty_console_owned_by_leader(int sid, int pid)
{
    return console_tty.sid != 0 && console_tty.sid == sid && sid == pid;
}

void tty_input_char(struct tty *t, unsigned char c)
{
    int signo = 0;

    n_tty_receive(&t->ldisc, c, t->sink, t->sink_ctx, &signo);
    if (signo != 0) {
        tty_deliver_signal(t, signo);
    }
    if (n_tty_readable(&t->ldisc)) {
        if (t->ready_us == 0) {
            t->ready_us = pit_now_us();
        }
        wq_wake_all(&t->input_wq);
    }
}

/* The read-sleep predicate: drain the polled input source (if any) into the
 * discipline, then report whether a read can now proceed. wq_wait_event calls
 * this with interrupts off before each sleep, so the serial drain and the
 * readiness test cannot be split by a waker. */
static int tty_ready_cond(void *arg)
{
    struct tty *t = (struct tty *)arg;

    if (t->pump) {
        t->pump(t);
    }
    return n_tty_readable(&t->ldisc);
}

/**
 * Block until a line is readable, then copy up to `count` bytes into `kbuf`.
 *
 * Returns the byte count, 0 at EOF, or -EINTR if a signal cut the wait short.
 * When `nonblock` is set the read never parks: it drains the polled source and,
 * if nothing is ready (and the discipline is not at EOF, which reads as ready),
 * returns -EAGAIN instead of waiting.
 */
int tty_read(struct tty *t, unsigned char *kbuf, int count, int nonblock)
{
    int rc;
    int n;
    uint32_t flags;

    if (count <= 0) {
        return 0;
    }
    if (nonblock) {
        flags = kirq_save();
        if (tty_ready_cond(t)) {
            n = n_tty_read(&t->ldisc, kbuf, count);
            kirq_restore(flags);
            usb_stamp_read();
            return n;
        }
        kirq_restore(flags);
        return -EAGAIN;
    }
    for (;;) {
        rc = wq_wait_event(&t->input_wq, tty_ready_cond, t);
        if (rc != 0) {
            return rc; /* -EINTR: a signal cut the wait short */
        }
        /*
         * Re-test readiness and drain in one interrupts-off step: an input
         * IRQ cannot then mutate the queue mid-read, and no other reader
         * woken by the same line can slip in and consume it first (which
         * would leave us seeing an empty queue and wrongly returning 0 ==
         * EOF). If the line was taken, loop back and wait again.
         */
        flags = kirq_save();
        if (n_tty_readable(&t->ldisc)) {
            n = n_tty_read(&t->ldisc, kbuf, count);
            if (t->ready_us) {
                latency_note(LAT_CONSOLE, pit_now_us() - t->ready_us);
                t->ready_us = 0;
            }
            kirq_restore(flags);
            /* Where a reader first has the byte, which is the last of the
             * four stamps the USB latency record prints. A byte from
             * anywhere else prints nothing. */
            usb_stamp_read();
            return n;
        }
        kirq_restore(flags);
    }
}

static void console_sink(void *ctx, unsigned char c);

int tty_write(struct tty *t, const unsigned char *kbuf, int count)
{
    int i;

    /* A record raised while the console is written waits for the write,
     * so what a program prints in one write is one line of the log. */
    if (t->sink == console_sink) {
        console_write_begin();
    }
    for (i = 0; i < count; i++) {
        n_tty_output(&t->ldisc, kbuf[i], t->sink, t->sink_ctx);
    }
    if (t->sink == console_sink) {
        console_write_end();
    }
    return count;
}

short tty_poll(struct tty *t, short events, wait_queue_t **wq)
{
    short revents = 0;

    /*
     * Only an input query touches the input side: park on input_wq, drain
     * the polled source (surfacing input that arrived but was not yet
     * drained), and (because draining claims foreground) it must NOT run for
     * a POLLOUT-only poll on a write-only console fd, or that process would
     * pull in serial input and could be signalled by a serial ^C. The poll
     * core calls this with interrupts off.
     */
    if (events & POLLIN) {
        *wq = &t->input_wq;
        if (t->pump) {
            t->pump(t);
        }
        if (n_tty_readable(&t->ldisc)) {
            revents = POLLIN;
        }
    }
    if (events & POLLOUT) {
        revents = (short)(revents | POLLOUT); /* the console is always writable */
    }
    return revents;
}

int tty_inq(struct tty *t)
{
    uint32_t flags;
    int n;

    /* Drain a polled source and read the count in one interrupts-off step: the
     * drain (echo, queue updates) cannot then race an input IRQ, and the count
     * is a consistent snapshot. */
    flags = kirq_save();
    if (t->pump) {
        t->pump(t);
    }
    n = n_tty_inq(&t->ldisc);
    kirq_restore(flags);
    return n;
}

/**
 * Install a termios; with `flush_input`, discard the unread input first.
 *
 * Interrupts are off so an input IRQ feeding the discipline never reads a
 * half-written termios. A switch out of canonical mode can make a partial
 * line readable, so a reader parked on the tty is woken when the discipline
 * reports something to read. The console's flush also empties the serial
 * ring: input taken from the port but not yet in the discipline is input
 * the program asked to discard.
 */
void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input)
{
    uint32_t flags;
    int readable;

    flags = kirq_save();
    if (flush_input) {
        if (t->pump) {
            serial_rx_flush();
        }
        n_tty_flush(&t->ldisc);
    }
    readable = n_tty_set_termios(&t->ldisc, kt);
    if (readable) {
        wq_wake_all(&t->input_wq);
    }
    kirq_restore(flags);
}

void tty_flush_input(struct tty *t)
{
    uint32_t flags;

    /* Interrupts off so the reset is not split by an input IRQ pushing onto the
     * queue (which would desync head/tail). */
    flags = kirq_save();
    if (t->pump) {
        serial_rx_flush();
    }
    n_tty_flush(&t->ldisc);
    kirq_restore(flags);
}

void tty_hangup(struct tty *t)
{
    uint32_t flags;

    /* Mark end-of-file at the current queue tail (so any already-cooked input
     * is still read first, then reads return 0) and wake every parked
     * reader/poller. A pty slave whose master has closed uses this so its
     * blocked readers return EOF instead of hanging forever. */
    flags = kirq_save();
    t->ldisc.eof = 1;
    t->ldisc.eof_pos = t->ldisc.rq_tail;
    kirq_restore(flags);
    wq_wake_all(&t->input_wq);
}

/* the system console: VGA screen + COM1 serial */

/**
 * The console's output: the screen and the serial line together.
 *
 * A serial terminal mirrors what is on the VGA console. OPOST and ONLCR in the
 * discipline have already expanded '\n' to CR LF, so the raw console call is
 * the one to use: `console_putc` would expand it again. It goes through a
 * console call and not through the writers under it so the kernel log knows
 * whether a program has left a line open on the terminal, and does not begin a
 * record in the middle of it.
 */
static void console_sink(void *ctx, unsigned char c)
{
    (void)ctx;
    console_putc_raw((char)c);
}

/* When the discipline has no room for a line, ordinary bytes wait in the
 * ring; a signal character behind them does not, since what it does first
 * is discard everything typed before it. The ring is searched for one, the
 * bytes before it are discarded as the character would have discarded them,
 * and it is fed. Returns 1 when a byte was fed. */
static int console_signal_through(struct tty *t, n_tty_out_fn out, void *ctx)
{
    const struct ktermios *tio = &t->ldisc.tio;
    unsigned char set[3];
    int at, ch, signo = 0;

    if (!(tio->c_lflag & ISIG)) {
        return 0;
    }
    set[0] = tio->c_cc[VINTR];
    set[1] = tio->c_cc[VQUIT];
    set[2] = tio->c_cc[VSUSP];
    at = serial_rx_find(set, 3);
    if (at < 0) {
        return 0;
    }
    serial_rx_discard(at);
    t->ldisc.counters.flush_bytes += (unsigned int)at;
    ch = serial_rx_take();
    if (ch < 0) {
        return 0;
    }
    n_tty_receive(&t->ldisc, (unsigned char)ch, out, ctx, &signo);
    if (signo != 0) {
        tty_deliver_signal(t, signo);
    }
    return 1;
}

/* The kernel termios is the 36-byte TCGETS/TCSETS ABI prefix (see n_tty.h). Pin
 * its size so a struct change can never silently desync from musl's view. */
typedef char ktermios_is_36_bytes[(sizeof(struct ktermios) == 36 && KNCCS == 19) ? 1 : -1];

struct tty console_tty;

/* Save EFLAGS and disable interrupts; restore puts IF back. On this single CPU
 * this is the whole lock between a reader and an input IRQ (same idea as
 * wait_queue.c's). */
static uint32_t reader_drains, tick_drains;

/* The console's polled input source: the serial ring, which the port's
 * interrupt and the tick fill, drained into the discipline with echo. Run in
 * thread context, where current_proc is the reader/poller, so an ISIG signal
 * targets the right process, from tty_ready_cond before each read-sleep and
 * from tty_poll; the interrupt and the timer only wake a waiter, so the
 * echoing drain happens here and not in an IRQ. Bounded per call so a flood
 * cannot hold interrupts off indefinitely; the caller loops. */
static void console_pump(struct tty *t)
{
    int echo = serial_rx_pending() <= TTY_ECHO_MAX;

    reader_drains++;
    for (;;) {
        int ch, signo = 0;

        if (!n_tty_has_room(&t->ldisc)) {
            if (console_signal_through(t, echo ? t->sink : null_sink, echo ? t->sink_ctx : NULL)) {
                continue;
            }
            break;
        }
        ch = serial_rx_take();
        if (ch < 0) {
            break;
        }
        if (echo) {
            tty_input_char(t, (unsigned char)ch);
            continue;
        }
        n_tty_receive(&t->ldisc, (unsigned char)ch, null_sink, NULL, &signo);
        if (signo != 0) {
            tty_deliver_signal(t, signo);
        }
    }
    if (!echo && n_tty_readable(&t->ldisc)) {
        if (t->ready_us == 0) {
            t->ready_us = pit_now_us();
        }
        wq_wake_all(&t->input_wq);
    }
}

/* The timer-IRQ drain: like console_pump but with echo SUPPRESSED. Echo runs
 * through console_sink to serial_out, which busy-waits on the UART
 * transmit-holding register:
 * unacceptable in interrupt context, where a 64-byte burst could mask
 * interrupts for milliseconds. n_tty_receive still recognizes ISIG chars and
 * queues type-ahead independent of the (here null) sink, so a serial ^C/^Z
 * reaches the foreground group and ordinary bytes are queued un-echoed for the
 * next reader; the echoing drain (console_pump) runs only in thread context.
 * Bounded by the pump budget so a flood cannot hold the IRQ off. */
static void console_pump_irq(struct tty *t)
{
    int budget = TTY_PUMP_BUDGET;

    tick_drains++;
    while (budget-- > 0 && serial_data_available()) {
        int ch, signo = 0;

        if (!n_tty_has_room(&t->ldisc)) {
            if (console_signal_through(t, null_sink, NULL)) {
                continue;
            }
            break;
        }
        ch = serial_rx_take();

        if (ch < 0) {
            break;
        }
        n_tty_receive(&t->ldisc, (unsigned char)ch, null_sink, NULL, &signo);
        if (signo != 0) {
            tty_deliver_signal(t, signo);
        }
    }
}

/* What runs, with interrupts off, once bytes are in the ring: from the
 * port's interrupt and from the tick alike, so a parked reader learns of
 * input at once and a session nobody reads for has its bytes moved into the
 * discipline at once, and the tick is the fallback for a port whose
 * interrupt is not enabled. */
static void console_rx_notify(void)
{
    tty_console_poll();
}

void tty_console_init(void)
{
    tty_init(&console_tty, console_sink, NULL);
    console_tty.pump = console_pump;
    serial_set_rx_notify(console_rx_notify);
}

void tty_console_counters(struct console_counters *out)
{
    struct serial_rx_counters rx;
    uint32_t flags = kirq_save();

    serial_rx_counters(&rx);
    out->uart_bytes = rx.bytes;
    out->uart_overruns = rx.overruns;
    out->irq_drains = rx.irq_services;
    out->tick_drains = tick_drains;
    out->reader_drains = reader_drains;
    out->ring_full = rx.ring_full;
    out->ring_max = rx.ring_max;
    out->shell_bytes = rx.polled_bytes;
    out->ldisc = console_tty.ldisc.counters;
    kirq_restore(flags);
}

void tty_console_poll(void)
{
    /*
     * Called from the timer IRQ and from the port's interrupt. The tick
     * takes what the port holds first, so a port whose interrupt is not
     * enabled is still served every ten milliseconds. A reader or poller
     * parked on the console would never learn of serial input while another
     * process runs; the wait queue is its only waker. When someone is
     * waiting AND a byte has arrived, wake them; the woken thread drains and
     * echoes the ring itself (console_pump in its own context), so no
     * echoing or signal delivery to the wrong process happens here. Gated on
     * a waiter so it never steals bytes from the kernel debug shell, which
     * polls the ring itself.
     */
    serial_rx_service(TTY_PUMP_BUDGET);
    if (!serial_data_available()) {
        return;
    }
    if (wq_active(&console_tty.input_wq)) {
        /* A reader/poller is parked: wake it to drain (WITH echo) in its own
         * thread context. No draining, echo, or signalling in this IRQ. */
        wq_wake_all(&console_tty.input_wq);
    } else if (console_tty.sid != 0) {
        /* A session owns the console but nobody is parked reading it: a
         * foreground job is running while the shell waits in wait4. Drain here,
         * echo suppressed (console_pump_irq avoids blocking serial TX in the
         * IRQ), so a serial ^C/^Z still reaches the foreground group;
         * type-ahead is queued un-echoed for the next reader. The shell
         * releases the console (tty_set_session 0) when it regains control, so
         * this never steals the kernel debug shell's own serial input. */
        console_pump_irq(&console_tty);
    }
}
