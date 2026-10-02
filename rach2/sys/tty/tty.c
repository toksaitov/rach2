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

/**
 * Initialize a terminal with default termios, window size and input wait.
 * An input producer may set `space_wq` to be woken for room: after a flush,
 * a termios change, or an input byte that took bytes out of the discipline.
 */
void tty_init(struct tty *t, n_tty_out_fn sink, void *sink_ctx)
{
    n_tty_init(&t->ldisc);
    wait_queue_init(&t->input_wq);
    t->space_wq = NULL;
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
 * The interrupt-side drain (tty_console_poll) gates on sid != 0 and then
 * delivers ISIG to fg_pgid by value, so a torn update (sid already set while
 * fg_pgid is still the stale 0) would route a serial ^C/^Z to group 0 and
 * drop it.
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
 * controlling terminal, so the interrupt-side serial drain never targets a
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
    unsigned int held = n_tty_held(&t->ldisc);
    int signo = 0;

    n_tty_receive(&t->ldisc, c, t->sink, t->sink_ctx, &signo);
    if (n_tty_held(&t->ldisc) < held) {
        wq_wake(t->space_wq, WQ_ROOM);
    }
    if (signo != 0) {
        tty_deliver_signal(t, signo);
    }
    if (n_tty_readable(&t->ldisc)) {
        if (t->ready_us == 0) {
            t->ready_us = pit_now_us();
        }
        wq_wake(&t->input_wq, WQ_DATA);
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
 *
 * After a wake the readiness test and the copy run in one interrupts-off
 * step, so an input IRQ cannot change the queue mid-read. Another reader
 * woken by the same line may have taken it, and a read of the empty queue
 * would then return 0, which means end of file; such a reader waits again.
 *
 * While the USB latency record is on, a reader about to park logs `latency:
 * reader waiting`, and a console reader arms the record, under the same
 * interrupt mask as its wait, so a key sent after that line cannot arrive
 * before the reader is on the queue. Every read that returns data or end of
 * file, on any tty, calls `usb_stamp_read`, the last of the record's four
 * stamps.
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
        if (usb_stamp_recording()) {
            flags = kirq_save();
            rc = tty_ready_cond(t);
            if (!rc && t == &console_tty && current_proc) {
                usb_stamp_wait(current_proc->pid);
            }
            if (!rc) {
                klog(KLOG_INFO, "usb", "latency: reader waiting");
            }
            rc = wq_wait_event(&t->input_wq, tty_ready_cond, t);
            kirq_restore(flags);
        } else {
            rc = wq_wait_event(&t->input_wq, tty_ready_cond, t);
        }
        if (rc != 0) {
            return rc; /* -EINTR: a signal cut the wait short */
        }
        flags = kirq_save();
        if (n_tty_readable(&t->ldisc)) {
            n = n_tty_read(&t->ldisc, kbuf, count);
            if (t->ready_us) {
                latency_span(LAT_CONSOLE, t->ready_us, pit_now_us());
                t->ready_us = 0;
            }
            kirq_restore(flags);
            usb_stamp_read();
            return n;
        }
        kirq_restore(flags);
    }
}

static void console_sink(void *ctx, unsigned char c);

/**
 * Give the screen's answers to the queries just written to the console to its
 * input, as a terminal on a wire sends them back, so the program that asked
 * for the device attributes or the cursor's position reads them as typed
 * bytes. They are fed here and not where the screen's parser makes them,
 * since the parser runs inside output and the discipline's echo writes
 * through it.
 */
static void console_answer(struct tty *t)
{
    char buf[K_ANSWER_MAX];
    unsigned int i, n;
    uint32_t flags;

    flags = kirq_save();
    n = k_take_answer(buf, sizeof buf);
    for (i = 0; i < n; i++) {
        tty_input_char(t, (unsigned char)buf[i]);
    }
    kirq_restore(flags);
}

int tty_write(struct tty *t, const unsigned char *kbuf, int count)
{
    int i;

    if (t->sink == console_sink) {
        console_write_begin();
    }
    for (i = 0; i < count; i++) {
        n_tty_output(&t->ldisc, kbuf[i], t->sink, t->sink_ctx);
    }
    if (t->sink == console_sink) {
        console_write_end();
        console_answer(t);
    }
    return count;
}

short tty_poll(struct tty *t, short events, wait_queue_t **wq)
{
    short revents = 0;

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
 * Wake the optional input-producer queue to recheck space after a flush or
 * mode change.
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
    wq_wake(t->space_wq, WQ_ROOM);
    if (readable) {
        wq_wake(&t->input_wq, WQ_DATA);
    }
    kirq_restore(flags);
}

/** Discard unread input with interrupts off and wake input producers. */
void tty_flush_input(struct tty *t)
{
    uint32_t flags;

    flags = kirq_save();
    if (t->pump) {
        serial_rx_flush();
    }
    n_tty_flush(&t->ldisc);
    wq_wake(t->space_wq, WQ_ROOM);
    kirq_restore(flags);
}

/**
 * Set an EOF marker after queued input and wake readers.
 *
 * Canonical reads consume this marker once. The endpoint must maintain
 * persistent hangup state for subsequent reads and other blocked readers.
 */
void tty_hangup(struct tty *t)
{
    uint32_t flags;

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
 * console call and not through the writers under it so the kernel log tracks
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

/* The kernel termios is the 36-byte TCGETS/TCSETS ABI prefix (see n_tty.h).
 * The build fails if its size changes, since musl reads these 36 bytes as the
 * start of its own struct termios. */
typedef char ktermios_is_36_bytes[(sizeof(struct ktermios) == 36 && KNCCS == 19) ? 1 : -1];

struct tty console_tty;

/* Drains by console_pump and by console_pump_irq, for `/proc/console`. */
static uint32_t reader_drains, tick_drains;

/**
 * The console's polled input source: drain the serial ring, which the port's
 * interrupt and the tick fill, into the discipline.
 *
 * Runs in thread context with interrupts off, from tty_ready_cond before each
 * read-sleep, from tty_poll and from tty_inq; the interrupt and the tick wake
 * a waiter or drain without echo, so the echoing drain happens here and not in
 * an IRQ. It goes on until the ring is empty or the discipline is full; a
 * full discipline still takes a signal character from the ring
 * (console_signal_through). It echoes only when the ring held at most
 * TTY_ECHO_MAX bytes at its start; without echo the bytes go straight to the
 * discipline and parked readers are woken once at the end.
 */
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

/**
 * The interrupt-side drain, run from tty_console_poll: like console_pump but
 * with echo suppressed.
 *
 * Echo runs through console_sink to serial_out, which busy-waits on the
 * UART's transmit-holding register, and a 64-byte burst of it in interrupt
 * context would mask interrupts for milliseconds. n_tty_receive still
 * recognizes ISIG chars and queues type-ahead with the null sink, so a serial
 * ^C/^Z reaches the foreground group and ordinary bytes wait un-echoed for the
 * next reader. At most TTY_PUMP_BUDGET bytes per call, so a flood cannot hold
 * the IRQ off.
 */
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

/* The serial driver's receive notify, run from the port's interrupt with
 * interrupts off once bytes are in the ring. It does what the tick does
 * (tty_console_poll), so a parked reader learns of input at once and a
 * session nobody reads for has its bytes moved into the discipline at once;
 * the tick remains for a port whose interrupt is not enabled. */
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
    out->irq_bytes = rx.irq_bytes;
    out->tick_drains = tick_drains;
    out->reader_drains = reader_drains;
    out->ring_full = rx.ring_full;
    out->ring_max = rx.ring_max;
    out->shell_bytes = rx.polled_bytes;
    out->ldisc = console_tty.ldisc.counters;
    kirq_restore(flags);
}

/**
 * Service the console's serial port, from the timer tick and from the port's
 * interrupt, both with interrupts off.
 *
 * The UART's bytes go to the ring first, so a port whose interrupt is not
 * enabled is still served every tick. A reader or poller parked on the
 * console, which nothing else would tell of serial input while another
 * process runs, is then only woken: it drains and echoes the ring in its own
 * context (console_pump). With nobody parked and a session owning the
 * console, as when a foreground job runs while its shell waits in wait4, the
 * ring is drained here without echo (console_pump_irq), so a serial ^C/^Z
 * still reaches the foreground group and type-ahead waits for the next
 * reader. With no session the ring is left to the kernel debug shell, which
 * polls it itself and releases the console (tty_set_session 0) whenever it
 * regains control.
 */
void tty_console_poll(void)
{
    serial_rx_service(TTY_PUMP_BUDGET);
    if (!serial_data_available()) {
        return;
    }
    if (wq_active(&console_tty.input_wq)) {
        wq_wake_all(&console_tty.input_wq);
    } else if (console_tty.sid != 0) {
        console_pump_irq(&console_tty);
    }
}
