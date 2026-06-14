/*
 * tty.c -- the terminal object: an N_TTY line discipline over a wait-queue of
 * readers, plus the one console_tty wired to the VGA screen and the serial port.
 *
 * The discipline (n_tty.c) is pure and host-tested; this file is the kernel glue
 * that blocks/wakes readers, posts ISIG signals, and drives real hardware. A
 * reader parks in tty_read until tty_input_char (from the keyboard IRQ or the
 * serial pump) completes a line; output and echo flow through a per-tty sink.
 */

#include "tty.h"
#include "../io/poll.h"
#include "../serial.h"
#include "../kio.h"
#include "../lib/kstring.h"

/* Most serial bytes drained into the discipline per pump call, so a flood cannot
 * hold interrupts off (the drain runs under cli) for an unbounded stretch; a
 * caller that still has input loops and drains the rest on the next pass. */
#define TTY_PUMP_BUDGET 64

/* The kernel termios is the 36-byte TCGETS/TCSETS ABI prefix (see n_tty.h). Pin
 * its size so a struct change can never silently desync from musl's view. */
typedef char ktermios_is_36_bytes[(sizeof(struct ktermios) == 36 && KNCCS == 19) ? 1 : -1];

struct tty console_tty;

/* Save EFLAGS and disable interrupts; restore puts IF back. On this single CPU
 * this is the whole lock between a reader and an input IRQ (same idea as
 * wait_queue.c's). */
static uint32_t tty_irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void tty_irq_restore(uint32_t flags)
{
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

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

/* Post a signal to every (live, user) member of process group pgid. */
void tty_signal_group(int pgid, int signo)
{
    int i;

    if (pgid == 0) {
        return;
    }
    for (i = 0; i < MAX_PROCS; i++) {
        process_t *p = &proc_table[i];
        if (p->state != PROC_UNUSED && p->state != PROC_ZOMBIE && p->cr3 != 0 &&
            (int)p->pgid == pgid) {
            signal_post(p, signo);
        }
    }
}

/* Deliver an ISIG-recognized signal to the tty's FOREGROUND PROCESS GROUP -- NOT
 * to current_proc, which (when input is fed from an IRQ) may be an unrelated
 * process, and not to a single process: ^C reaches every member of the fg group. */
static void tty_deliver_signal(struct tty *t, int signo)
{
    tty_signal_group(t->fg_pgid, signo);
}

int tty_is_background(struct tty *t, int sid, int pgid)
{
    /* True when this tty is the caller's controlling terminal (same session) but
     * the caller's group is NOT the foreground group -- a background process whose
     * tty I/O must be gated by SIGTTIN/SIGTTOU. */
    return t->sid != 0 && t->sid == sid && t->fg_pgid != 0 && t->fg_pgid != pgid;
}

void tty_acquire_ctty(struct tty *t, int sid, int pgid)
{
    /* A process using a console with no controlling session claims it: its session
     * becomes the console's, its group the foreground -- so a lone program gets ^C
     * without an explicit TIOCSCTTY. Once claimed, only TIOCSCTTY/TIOCSPGRP change
     * it. (sid/fg_pgid are ints; the ISIG path reads fg_pgid by value, so a racing
     * single-int update is never seen torn.) */
    if (t->sid == 0) {
        t->sid = sid;
        t->fg_pgid = pgid;
    }
}

void tty_input_char(struct tty *t, unsigned char c)
{
    int signo = 0;

    n_tty_receive(&t->ldisc, c, t->sink, t->sink_ctx, &signo);
    if (signo != 0) {
        tty_deliver_signal(t, signo);
    }
    if (n_tty_readable(&t->ldisc)) {
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

int tty_read(struct tty *t, unsigned char *kbuf, int count)
{
    int rc;
    int n;
    uint32_t flags;

    if (count <= 0) {
        return 0;
    }
    for (;;) {
        rc = wq_wait_event(&t->input_wq, tty_ready_cond, t);
        if (rc != 0) {
            return rc; /* -EINTR: a signal cut the wait short */
        }
        /*
         * Re-test readiness and drain in one interrupts-off step: an input IRQ
         * cannot then mutate the queue mid-read, and no other reader woken by the
         * same line can slip in and consume it first (which would leave us seeing
         * an empty queue and wrongly returning 0 == EOF). If the line was taken,
         * loop back and wait again.
         */
        flags = tty_irq_save();
        if (n_tty_readable(&t->ldisc)) {
            n = n_tty_read(&t->ldisc, kbuf, count);
            tty_irq_restore(flags);
            return n;
        }
        tty_irq_restore(flags);
    }
}

int tty_write(struct tty *t, const unsigned char *kbuf, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        n_tty_output(&t->ldisc, kbuf[i], t->sink, t->sink_ctx);
    }
    return count;
}

short tty_poll(struct tty *t, short events, wait_queue_t **wq)
{
    short revents = 0;

    /*
     * Only an input query touches the input side: park on input_wq, drain the
     * polled source (surfacing input that arrived but was not yet drained), and
     * -- because draining claims foreground -- it must NOT run for a POLLOUT-only
     * poll on a write-only console fd, or that process would pull in serial input
     * and could be signalled by a serial ^C. The poll core calls this with
     * interrupts off.
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
     * drain (echo, queue updates) cannot then race an input IRQ, and the count is
     * a consistent snapshot. */
    flags = tty_irq_save();
    if (t->pump) {
        t->pump(t);
    }
    n = n_tty_inq(&t->ldisc);
    tty_irq_restore(flags);
    return n;
}

void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input)
{
    uint32_t flags;

    /* Interrupts off so an input IRQ feeding the discipline never reads a
     * half-written termios (the 36-byte copy is several stores). */
    flags = tty_irq_save();
    k_memcpy(&t->ldisc.tio, kt, sizeof(struct ktermios));
    if (flush_input) {
        t->ldisc.line_len = 0;
        t->ldisc.rq_head = t->ldisc.rq_tail;
        t->ldisc.eof = 0;
    }
    tty_irq_restore(flags);
}

void tty_flush_input(struct tty *t)
{
    uint32_t flags;

    /* Interrupts off so the reset is not split by an input IRQ pushing onto the
     * queue (which would desync head/tail). */
    flags = tty_irq_save();
    t->ldisc.line_len = 0;
    t->ldisc.rq_head = t->ldisc.rq_tail;
    t->ldisc.eof = 0;
    tty_irq_restore(flags);
}

/* --- the system console: VGA screen + COM1 serial --- */

/* Echo and output bytes go to both the screen and the serial line, so a serial
 * terminal mirrors what is on the VGA console. OPOST/ONLCR in the discipline has
 * already expanded '\n' to CR LF, which k_putc and serial_putc emit verbatim. */
static void console_sink(void *ctx, unsigned char c)
{
    (void)ctx;
    k_putc((char)c);
    serial_putc((char)c);
}

/* The console's polled input source: COM1 is not wired to an IRQ here, so drain
 * the bytes that have arrived into the discipline. Drained (in thread context,
 * where current_proc is the reader/poller -- so an ISIG signal targets the right
 * process) from tty_ready_cond before each read-sleep and from tty_poll; the
 * timer only wakes a waiter so this drain happens here, not in the IRQ. Bounded
 * per call so a flood cannot hold interrupts off indefinitely; the caller loops. */
static void console_pump(struct tty *t)
{
    int budget = TTY_PUMP_BUDGET;

    while (budget-- > 0 && serial_data_available()) {
        int ch = serial_try_getc();
        if (ch < 0) {
            break;
        }
        tty_input_char(t, (unsigned char)ch);
    }
}

void tty_console_init(void)
{
    tty_init(&console_tty, console_sink, NULL);
    console_tty.pump = console_pump;
}

void tty_console_poll(void)
{
    /*
     * Called from the timer IRQ. COM1 has no receive interrupt here, so a reader
     * or poller parked on the console would never learn of serial input while
     * another process runs -- the wait queue is its only waker. When someone is
     * waiting AND a byte has arrived, wake them; the woken thread drains and
     * echoes the port itself (console_pump in its own context), so no draining,
     * echoing, or signal delivery happens in this IRQ. Gated on a waiter so it
     * never steals bytes from the kernel debug shell, which polls COM1 itself.
     */
    if (wq_active(&console_tty.input_wq) && serial_data_available()) {
        wq_wake_all(&console_tty.input_wq);
    }
}
