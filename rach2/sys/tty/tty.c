/*
 * tty.c: the terminal object, an N_TTY line discipline with a queue of
 * waiting readers, and console_tty, which writes to the screen (VGA text or
 * the framebuffer console, display/fbcon.c) and to the serial port.
 *
 * The discipline (n_tty.c) has no kernel dependencies and is host-tested;
 * this file makes readers wait and wakes them, posts the signals the
 * discipline recognizes, and drains the console's serial ring. A reader waits
 * in tty_read until tty_input_char (from the keyboard drivers or the serial
 * drain) completes a line; output and echo leave through each tty's sink.
 */

#include "tty.h"
#include "../latency.h"
#include "../timer.h"
#include "../rtc.h"

#include "../usb/usb.h"
#include "../io/poll.h"
#include "../serial.h"
#include "../kd.h"
#include "../kio.h"
#include "../display/blank.h"
#include "../klog.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"

/* Most serial bytes drained into the discipline per interrupt-side pump
 * call, so a flood cannot hold interrupts off for an unbounded stretch. The
 * reader's own drain empties the ring, since a reader that parks with bytes
 * still in the ring is woken for them only by the tick; it echoes only when
 * the ring held no more than a keyboard's worth at its start, because an
 * echo waits on the UART's transmitter for 87 microseconds a byte with
 * interrupts off, and a flood typed by a harness is not read by a person. */
#define TTY_PUMP_BUDGET 64
#define TTY_ECHO_MAX    64

/* The ticks in VTIME's unit, a tenth of a second. */
#define TTY_TICKS_PER_VTIME (TICK_HZ / 10u)

/** A sink that discards: tty_init installs it when the caller gives none, so
 * the discipline's echo and output never call through NULL. */
static void null_sink(void *ctx, unsigned char c)
{
    (void)ctx;
    (void)c;
}

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
    t->room = NULL;
    t->pump = NULL;
    t->flush = NULL;
    t->flow = NULL;
    t->sid = 0;
    t->fg_pgid = 0;
    t->hungup = 0;
    t->vhangups = 0;
}

/** Signal the processes of `pgid`, with the big lock held, from a call or
 * an interrupt, each through its leader, zombie or not, as Linux
 * 6.12.115's __kill_pgrp_info does (kernel/signal.c:1478-1479). A process
 * whose threads have all ended and a kernel thread take nothing; a
 * process's unfinished forks get the signal (`signal_post_multi`). */
void tty_signal_group(int pgid, int signo)
{
    process_t *p;
    struct k_siginfo info;

    if (pgid == 0) {
        return;
    }
    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    for (p = proc_list; p; p = p->next) {
        process_t *target = signal_process_target(p);

        if (target && (int)target->pgid == pgid) {
            signal_post_multi(target, signo, &info);
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

int tty_is_background(struct tty *t, int sid, int pgid)
{
    return t->sid != 0 && t->sid == sid && t->fg_pgid != 0 && t->fg_pgid != pgid;
}

/**
 * A live thread of the user process numbered `pid`, as a member's
 * parent_pid names its parent, or NULL once every thread has ended. A
 * leader that has exited while its other threads run is a zombie, but the
 * process lives on and its children keep it as their parent, so any live
 * thread answers for it; every thread has the process's group and session.
 */
static process_t *tty_live_process(uint32_t pid)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->tgid == pid && p->state != PROC_ZOMBIE && p->space_root != 0) {
            return p;
        }
    }
    return NULL;
}

/**
 * Whether process group `pgid` is orphaned, as Linux's
 * will_become_orphaned_pgrp: no live member has a parent in another group
 * of the same session. A member whose parent is init, or has gone, does not
 * count.
 */
static int tty_pgrp_orphaned(int pgid)
{
    process_t *p, *parent;

    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_ZOMBIE || p->space_root == 0 || (int)p->pgid != pgid ||
            p->parent_pid <= 1) {
            continue;
        }
        parent = tty_live_process(p->parent_pid);
        if (parent && (int)parent->pgid != pgid && parent->sid == p->sid) {
            return 0;
        }
    }
    return 1;
}

/**
 * Linux's __tty_check_change: when another group is in the foreground of the
 * terminal `t` that controls the caller's session, a SIGTTOU the caller
 * blocks or ignores lets the change go ahead and a SIGTTIN so treated is
 * EIO; an orphaned group, which no shell would continue, is EIO; otherwise
 * the signal goes to the caller's group and the call is EINTR, which the
 * dispatcher restarts once the group is continued.
 */
int tty_job_check(struct tty *t, int signo)
{
    process_t *self = current_proc;

    if (!self || !tty_is_background(t, (int)self->sid, (int)self->pgid)) {
        return 0;
    }
    if ((self->sig_blocked & KSIG_BIT(signo)) || self->sigacts[signo].handler == KSIG_IGN) {
        return signo == SIGTTIN ? -EIO : 0;
    }
    if (tty_pgrp_orphaned((int)self->pgid)) {
        return -EIO;
    }
    tty_signal_group((int)self->pgid, signo);
    return -EINTR;
}

/**
 * The interrupt-side drain (tty_console_poll) drains the console only while
 * its sid is nonzero, and then delivers a signal to fg_pgid by value, so a
 * torn update (sid already set while fg_pgid is still 0) would send a serial
 * ^C or ^Z to group 0, which drops it. With interrupts off the two stores are
 * one step to that drain.
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
 * It holds for the login shell an init started (which calls setsid and claims
 * the console with TIOCSCTTY), for a program the kernel shell ran (for which
 * `exec_program` publishes the sid), and for any later leader that claimed
 * the console with TIOCSCTTY. It does not hold for a member of a job group in
 * the owning session, whose exit must leave the terminal to the shell.
 */
int tty_console_owned_by_leader(int sid, int pid)
{
    return console_tty.sid != 0 && console_tty.sid == sid && sid == pid;
}

/**
 * Tell `flow` that output stopped or restarted, when `stopped` differs from
 * `was`, the discipline's state before; a restart wakes the writers and the
 * pollers for POLLOUT on `input_wq`. Interrupts off.
 */
static void tty_flow_changed(struct tty *t, int was)
{
    if (t->ldisc.stopped == was) {
        return;
    }
    if (t->flow) {
        t->flow(t, t->ldisc.stopped ? TTY_FLOW_STOP : TTY_FLOW_START);
    }
    if (!t->ldisc.stopped) {
        wq_wake(&t->input_wq, WQ_ROOM);
    }
}

/** The bytes `t`'s sink can take now (`room`). */
static unsigned int tty_room(struct tty *t)
{
    return t->room ? t->room(t) : ~0u;
}

/** Give the echo held to the sink, unless output is stopped, as far as its
 * room allows, and tell `flow` when any went. Interrupts off. */
static void tty_echo_out(struct tty *t)
{
    if (n_tty_echo_flush(&t->ldisc, t->sink, t->sink_ctx, tty_room(t)) > 0 && t->flow) {
        t->flow(t, TTY_FLOW_DATA);
    }
}

/** The echo sink of a receive on a terminal whose sink has a `room`: the
 * sink while it has room for the byte, and the echo held otherwise, so that
 * echo waits for room as Linux's does and is never dropped. `ctx` is the
 * terminal. */
static void tty_room_sink(void *ctx, unsigned char c)
{
    struct tty *t = (struct tty *)ctx;

    if (t->room(t) > 0) {
        t->sink(t->sink_ctx, c);
    } else {
        n_tty_hold_echo(&t->ldisc, c);
    }
}

/**
 * Run `c` through the discipline with echo to `out`, `looked` nonzero for a
 * byte `tty_lookahead` has seen, as Linux's n_tty_receive_char_special and
 * isig: a signal goes to the foreground group, and without NOFLSH the output
 * queued is discarded before the signal character's echo; then a change of
 * output flow goes to `flow`. Echo to the terminal's sink stops at its room,
 * and the echo held then goes as far as the room allows. With another
 * `out`, the null sink of a drain that suppresses echo, this byte's echo is
 * dropped before it reaches the echo held (`echo_muted`), which waits whole
 * for a sink that shows it, though it fills `echo_buf`: a restart this byte
 * makes would otherwise throw it away, and a byte held behind a full buffer
 * would push out its oldest. A signal character's flush still discards it.
 */
static void tty_receive(struct tty *t, unsigned char c, n_tty_out_fn out, void *ctx, int looked)
{
    int was = t->ldisc.stopped;
    int signo = 0;
    int shown = out == t->sink;
    uint32_t flags = kirq_save();

    if (shown && t->room) {
        out = tty_room_sink;
        ctx = t;
    }
    t->ldisc.echo_muted = !shown;
    if (looked) {
        n_tty_receive_looked(&t->ldisc, c, out, ctx, &signo);
    } else {
        n_tty_receive(&t->ldisc, c, out, ctx, &signo);
    }
    if (signo != 0) {
        tty_deliver_signal(t, signo);
        if (!(t->ldisc.tio.c_lflag & NOFLSH) && t->flush) {
            t->flush(t, TTY_FLUSH_IN | TTY_FLUSH_OUT);
        }
    }
    t->ldisc.echo_muted = 0;
    if (shown) {
        n_tty_echo_flush(&t->ldisc, t->sink, t->sink_ctx, tty_room(t));
    }
    tty_flow_changed(t, was);
    kirq_restore(flags);
}

/** `tty_input_char`, with `looked` as for `tty_receive`. */
static void tty_input(struct tty *t, unsigned char c, int looked)
{
    unsigned int held = n_tty_held(&t->ldisc);

    tty_receive(t, c, t->sink, t->sink_ctx, looked);
    if (n_tty_held(&t->ldisc) < held) {
        wq_wake(t->space_wq, WQ_ROOM);
    }
    if (n_tty_readable(&t->ldisc)) {
        if (t->ready_us == 0) {
            t->ready_us = timer_now_us();
        }
        wq_wake(&t->input_wq, WQ_DATA);
    } else if (!(t->ldisc.tio.c_lflag & ICANON) && n_tty_inq(&t->ldisc) > 0) {
        /* Not readable to poll, but a read for fewer than VMIN bytes, or
         * one whose VTIME timer starts or restarts, needs to see it. */
        wq_wake(&t->input_wq, 0);
    }
}

void tty_input_char(struct tty *t, unsigned char c)
{
    tty_input(t, c, 0);
}

void tty_input_looked(struct tty *t, unsigned char c)
{
    tty_input(t, c, 1);
}

int tty_lookahead(struct tty *t, unsigned char c, int echo)
{
    uint32_t flags = kirq_save();
    int was = t->ldisc.stopped;
    int changed = n_tty_lookahead(&t->ldisc, c);

    tty_flow_changed(t, was);
    if (echo && was && !t->ldisc.stopped) {
        tty_echo_out(t);
    }
    kirq_restore(flags);
    return changed;
}

/**
 * TCOOFF and TCOON set and clear `tco_stopped` around Linux's stop_tty and
 * start_tty; a TCOON does not give the echo held to the sink, which the next
 * input or write does, as on Linux. TCIOFF and TCION give their character
 * to the sink without output processing, as the driver's write does, and
 * restart and stop output again around it when VSTOP stopped it, which a
 * pty's master in packet mode sees as TIOCPKT_START and TIOCPKT_STOP. Under
 * TCOOFF's stop, which start_tty does not lift, a sink with a `room`, a
 * pty's, takes nothing, as Linux's pty_write refuses while stopped, and the
 * console's screen and COM1 take the character, as Linux's con_write and
 * uart_send_xchar write through any stop.
 */
int tty_tcflow(struct tty *t, int action)
{
    struct n_tty *ld = &t->ldisc;
    uint32_t flags;
    int was;
    unsigned char c;

    if (action < TTY_TCOOFF || action > TTY_TCION) {
        return -EINVAL;
    }
    flags = kirq_save();
    was = ld->stopped;
    if (action == TTY_TCOOFF && !ld->tco_stopped) {
        ld->tco_stopped = 1;
        n_tty_stop(ld);
    } else if (action == TTY_TCOON && ld->tco_stopped) {
        ld->tco_stopped = 0;
        n_tty_start(ld);
    } else if (action == TTY_TCIOFF || action == TTY_TCION) {
        c = ld->tio.c_cc[action == TTY_TCIOFF ? VSTOP : VSTART];
        if (c != 0 && (!ld->tco_stopped || !t->room)) {
            int lift = was && !ld->tco_stopped;

            if (lift && t->flow) {
                t->flow(t, TTY_FLOW_START);
            }
            t->sink(t->sink_ctx, c);
            if (t->flow) {
                t->flow(t, TTY_FLOW_DATA);
            }
            if (lift && t->flow) {
                t->flow(t, TTY_FLOW_STOP);
            }
        }
    }
    tty_flow_changed(t, was);
    kirq_restore(flags);
    return 0;
}

/* A writer's wait for stopped output to restart. */
struct tty_out_wait {
    struct tty *t;
    uint32_t opened; /* the vhangups when the writer's description opened */
};

/** The writer's wait predicate: drain the pump, then whether output runs
 * or the description has been hung up. Interrupts off. */
static int tty_output_cond(void *arg)
{
    struct tty_out_wait *w = (struct tty_out_wait *)arg;

    if (w->t->pump) {
        w->t->pump(w->t);
    }
    return !w->t->ldisc.stopped || tty_desc_hung(w->t, w->opened);
}

int tty_wait_output(struct tty *t, int nonblock, uint32_t opened)
{
    struct tty_out_wait w;
    uint32_t flags;
    int go, rc;

    w.t = t;
    w.opened = opened;
    for (;;) {
        flags = kirq_save();
        go = tty_output_cond(&w);
        kirq_restore(flags);
        if (tty_desc_hung(t, opened)) {
            return -EIO;
        }
        if (go) {
            return 0;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        rc = wq_wait_event(&t->input_wq, tty_output_cond, &w);
        if (rc != 0) {
            return rc;
        }
    }
}

/* A blocking read's wait: what it asks for, and its VTIME timer. */
struct tty_wait {
    struct tty *t;
    int count;            /* the bytes the read asks for */
    unsigned int pending; /* the bytes queued when the timer was last armed */
    uint64_t deadline;    /* the tick at which the timer runs out, while armed */
    int armed;            /* the timer runs */
    uint32_t vhangups;    /* the terminal's vhangups when the read began */
};

/** Whether the read's VTIME timer has run out. */
static int tty_wait_expired(const struct tty_wait *w)
{
    return w->armed && tick_count() >= w->deadline;
}

/** Whether the read can take its input, or its 0, now. The caller has
 * interrupts off. */
static int tty_wait_ready(const struct tty_wait *w)
{
    return n_tty_read_ready(&w->t->ldisc, w->count, tty_wait_expired(w));
}

/**
 * The read-sleep predicate: drain the polled input source (if any) into the
 * discipline, then report whether the wait ends: the read can proceed, the
 * terminal hung up, or the queue changed under a timer that restarts with
 * each byte, which the caller arms again. wq_wait_event calls this with
 * interrupts off before each sleep, so the serial drain and the test cannot
 * be split by a waker.
 */
static int tty_wait_cond(void *arg)
{
    struct tty_wait *w = (struct tty_wait *)arg;
    struct tty *t = w->t;

    if (t->pump) {
        t->pump(t);
    }
    return tty_wait_ready(w) || t->hungup || t->vhangups != w->vhangups ||
           (n_tty_read_timer(&t->ldisc) == N_TTY_TIMER_BYTE &&
            (unsigned int)n_tty_inq(&t->ldisc) != w->pending);
}

/**
 * Arm the read's VTIME timer as `n_tty_read_timer` says, and the process's
 * `wakeup_tick` with it: with VMIN 0 once, from the read's start; with VMIN
 * above 0 from the last change of the queued bytes, while any are queued.
 * The caller has interrupts off and a current process.
 */
static void tty_wait_arm(struct tty_wait *w)
{
    struct n_tty *ld = &w->t->ldisc;
    int timer = n_tty_read_timer(ld);
    unsigned int n = (unsigned int)n_tty_inq(ld);

    if (timer == N_TTY_TIMER_NONE || (timer == N_TTY_TIMER_BYTE && n == 0)) {
        w->armed = 0;
    } else if (!w->armed || (timer == N_TTY_TIMER_BYTE && n != w->pending)) {
        w->deadline = rtc_ticks_now() + ld->tio.c_cc[VTIME] * TTY_TICKS_PER_VTIME;
        w->armed = 1;
    }
    w->pending = n;
    current_proc->wakeup_tick = w->armed ? (w->deadline != 0 ? w->deadline : 1u) : 0;
}

/** Copy up to `count` bytes of readable input to `kbuf`, and record the
 * time since the input became readable (ready_us) in the console latency
 * span, LAT_CONSOLE, clearing ready_us so that the next span starts from
 * later input. The caller has interrupts off. */
static int tty_take(struct tty *t, unsigned char *kbuf, int count)
{
    int n = n_tty_read(&t->ldisc, kbuf, count);

    if (t->ready_us) {
        latency_span(LAT_CONSOLE, t->ready_us, timer_now_us());
        t->ready_us = 0;
    }
    return n;
}

/**
 * After a wake the readiness test and the copy run in one interrupts-off
 * step, so an input IRQ cannot change the queue mid-read. Another reader
 * woken by the same line may have taken it, and a read of the empty queue
 * would then return 0, which means end of file; such a reader waits again.
 * A wait that ended for its timer to restart is armed and waits again too.
 * Every read that returns input or end of file, blocking or not, ends the
 * console latency span (`tty_take`).
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
    struct tty_wait w;
    int rc;
    int n;
    int hungup;
    uint32_t flags;

    if (count <= 0) {
        return 0;
    }
    w.t = t;
    w.count = count;
    w.pending = 0;
    w.deadline = 0;
    w.armed = 0;
    w.vhangups = t->vhangups;
    if (nonblock) {
        flags = kirq_save();
        if (t->pump) {
            t->pump(t);
        }
        if (tty_wait_ready(&w)) {
            n = tty_take(t, kbuf, count);
            kirq_restore(flags);
            usb_stamp_read();
            return n;
        }
        hungup = t->hungup;
        kirq_restore(flags);
        return hungup ? 0 : -EAGAIN;
    }
    for (;;) {
        flags = kirq_save();
        tty_wait_arm(&w);
        if (usb_stamp_recording()) {
            rc = tty_wait_cond(&w);
            if (!rc && t == &console_tty) {
                usb_stamp_wait(current_proc->pid);
            }
            if (!rc) {
                klog(KLOG_INFO, "usb", "latency: reader waiting");
            }
            rc = wq_wait_event(&t->input_wq, tty_wait_cond, &w);
            kirq_restore(flags);
        } else {
            kirq_restore(flags);
            rc = wq_wait_event(&t->input_wq, tty_wait_cond, &w);
        }
        current_proc->wakeup_tick = 0;
        if (rc != 0) {
            return rc;
        }
        flags = kirq_save();
        if (tty_wait_ready(&w)) {
            n = tty_take(t, kbuf, count);
            kirq_restore(flags);
            usb_stamp_read();
            return n;
        }
        hungup = t->hungup || t->vhangups != w.vhangups;
        kirq_restore(flags);
        if (hungup) {
            usb_stamp_read();
            return 0;
        }
    }
}

int tty_read_rest(struct tty *t, unsigned char *kbuf, int count)
{
    uint32_t flags;
    int n = 0;

    if (count <= 0) {
        return 0;
    }
    flags = kirq_save();
    if (n_tty_line_open(&t->ldisc)) {
        n = n_tty_read(&t->ldisc, kbuf, count);
    }
    kirq_restore(flags);
    return n;
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

/** Whether `t`'s sink has room for one byte of output at its largest: two
 * under OPOST, where `n_tty_output` makes ONLCR's CR LF of a newline, and
 * one for raw output. */
static int tty_output_fits(struct tty *t)
{
    return !t->room || t->room(t) >= ((t->ldisc.tio.c_oflag & OPOST) ? 2u : 1u);
}

/**
 * The echo held goes first, as far as the sink's room allows, as Linux's
 * n_tty_write begins with process_echoes, and the bytes then go one at a
 * time while output runs and the sink has room: a VSTOP from the keyboard's
 * interrupt stops the write between two bytes, and echo that took room the
 * caller counted on makes the write short instead of dropping its bytes.
 */
int tty_write(struct tty *t, const unsigned char *kbuf, int count)
{
    uint32_t flags;
    int i;

    if (t->sink == console_sink) {
        console_write_begin();
    }
    flags = kirq_save();
    n_tty_echo_flush(&t->ldisc, t->sink, t->sink_ctx, tty_room(t));
    kirq_restore(flags);
    for (i = 0; i < count && !t->ldisc.stopped && tty_output_fits(t); i++) {
        n_tty_output(&t->ldisc, kbuf[i], t->sink, t->sink_ctx);
    }
    if (t->sink == console_sink) {
        console_write_end();
        console_answer(t);
    }
    return i;
}

/** A poller for POLLOUT alone on stopped output waits on `input_wq` too,
 * which a restart wakes, and drains the pump, so a VSTART from COM1 reaches
 * the discipline while it waits. */
short tty_poll(struct tty *t, short events, wait_queue_t **wq)
{
    short revents = 0;

    if ((events & POLLIN) || ((events & POLLOUT) && t->ldisc.stopped)) {
        *wq = &t->input_wq;
        if (t->pump) {
            t->pump(t);
        }
    }
    if ((events & POLLIN) && n_tty_readable(&t->ldisc)) {
        revents = POLLIN;
    }
    if ((events & POLLOUT) && !t->ldisc.stopped) {
        revents = (short)(revents | POLLOUT);
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

/** Whether `tio` has IXON with ^S and ^Q, the flow control a packet-mode
 * master is told of, as Linux's pty_set_termios tests it. */
static int tty_std_flow(const struct ktermios *tio)
{
    return (tio->c_iflag & IXON) && tio->c_cc[VSTOP] == 023 && tio->c_cc[VSTART] == 021;
}

/**
 * Interrupts are off so an input IRQ feeding the discipline never reads a
 * half-written termios. A switch out of canonical mode can make a partial
 * line readable, so a reader parked on the tty is woken when the discipline
 * reports something to read. The console's flush also empties the serial
 * ring: input taken from the port but not yet in the discipline is input
 * the program asked to discard. `flow` hears of a change of IXON with ^S
 * and ^Q before the restart that turning IXON off makes, which then gives
 * the echo held to the sink, the order of Linux's tty_set_termios, which
 * calls the driver's set_termios before the discipline's.
 */
void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input)
{
    uint32_t flags;
    int readable, was, old_flow;

    flags = kirq_save();
    was = t->ldisc.stopped;
    old_flow = tty_std_flow(&t->ldisc.tio);
    if (flush_input) {
        if (t->pump) {
            serial_rx_flush();
        }
        n_tty_flush(&t->ldisc);
    }
    readable = n_tty_set_termios(&t->ldisc, kt);
    if (t->flow && old_flow != tty_std_flow(kt)) {
        t->flow(t, old_flow ? TTY_FLOW_NOSTOP : TTY_FLOW_DOSTOP);
    }
    tty_flow_changed(t, was);
    if (was && !t->ldisc.stopped) {
        tty_echo_out(t);
    }
    wq_wake(t->space_wq, WQ_ROOM);
    if (readable) {
        wq_wake(&t->input_wq, WQ_DATA);
    } else {
        /* A read blocked under the old VMIN or VTIME tests the new ones. */
        wq_wake(&t->input_wq, 0);
    }
    kirq_restore(flags);
    if (flush_input && t->flush) {
        t->flush(t, TTY_FLUSH_IN);
    }
}

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

void tty_hangup(struct tty *t)
{
    uint32_t flags;

    flags = kirq_save();
    t->ldisc.eof = 1;
    t->ldisc.eof_pos = t->ldisc.rq_tail;
    t->hungup = 1;
    kirq_restore(flags);
    wq_wake_all(&t->input_wq);
}

void tty_vhangup(struct tty *t)
{
    struct ktermios kt;
    process_t *p;
    uint32_t flags;

    if (t->sid != 0) {
        for (p = proc_list; p; p = p->next) {
            if (p->state != PROC_ZOMBIE && p->space_root != 0 && (int)p->tgid == t->sid) {
                signal_post(p, SIGHUP);
                signal_post(p, SIGCONT);
                break;
            }
        }
    }
    tty_set_session(t, 0, 0);
    flags = kirq_save();
    t->vhangups++;
    t->ldisc.echo_head = t->ldisc.echo_tail;
    kirq_restore(flags);
    n_tty_default_termios(&kt);
    tty_set_termios(t, &kt, 1);
    if (t->flush) {
        t->flush(t, TTY_FLUSH_HANGUP);
    }
    wq_wake_all(&t->input_wq);
    wq_wake(t->space_wq, WQ_ROOM);
}

void tty_console_hold(void)
{
    uint32_t flags = kirq_save();
    int was = console_tty.ldisc.stopped;

    if (was) {
        n_tty_start(&console_tty.ldisc);
    } else {
        n_tty_stop(&console_tty.ldisc);
    }
    tty_flow_changed(&console_tty, was);
    kirq_restore(flags);
}

uint32_t tty_console_vhangups(void)
{
    return console_tty.vhangups;
}

/* the system console: the screen and COM1 */

/**
 * The console's output: the screen and the serial line together.
 *
 * A serial terminal mirrors what is on the screen. OPOST and ONLCR in the
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

/** The console's change of output flow: a stop or a restart sets or clears
 * the Scroll Lock flag and its LED (`kd_scroll_lock`), as Linux's con_stop
 * and con_start. */
static void console_flow(struct tty *t, int what)
{
    (void)t;
    if (what & (TTY_FLOW_STOP | TTY_FLOW_START)) {
        kd_scroll_lock((what & TTY_FLOW_STOP) != 0);
    }
}

/** When the discipline has no room for a line, ordinary bytes wait in the
 * ring; a signal character behind them does not, since what it does first
 * is discard everything typed before it. The ring is searched for one, the
 * bytes before it are discarded as the character would have discarded them,
 * and it is fed. Under NOFLSH a signal character discards nothing and waits
 * its turn. Returns 1 when a byte was fed. */
static int console_signal_through(struct tty *t, n_tty_out_fn out, void *ctx)
{
    const struct ktermios *tio = &t->ldisc.tio;
    unsigned char set[3];
    int at, ch, looked;

    if (!(tio->c_lflag & ISIG) || (tio->c_lflag & NOFLSH)) {
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
    looked = serial_rx_looked();
    ch = serial_rx_take();
    if (ch < 0) {
        return 0;
    }
    tty_receive(t, (unsigned char)ch, out, ctx, looked);
    return 1;
}

/** When the discipline has no room for a line, act on the VSTOP and VSTART
 * among the bytes that wait in the ring (`tty_lookahead`), so that a full
 * console still stops and restarts its output, as Linux's
 * n_tty_lookahead_flow_ctrl; a restart gives the echo held to the screen
 * when `echo` is nonzero. Interrupts off. */
static void console_lookahead(struct tty *t, int echo)
{
    unsigned char ahead[64];
    int n, i;

    if (!(t->ldisc.tio.c_iflag & IXON)) {
        return;
    }
    while ((n = serial_rx_lookahead(ahead, (int)sizeof(ahead))) > 0) {
        for (i = 0; i < n; i++) {
            tty_lookahead(t, ahead[i], echo);
        }
    }
}

/* The kernel termios is the 36-byte TCGETS/TCSETS ABI prefix (see n_tty.h).
 * The build fails if its size changes, since musl reads these 36 bytes as the
 * start of its own struct termios. */
typedef char ktermios_is_36_bytes[(sizeof(struct ktermios) == 36 && KNCCS == 19) ? 1 : -1];

struct tty console_tty;

/* Drains by console_pump and by console_pump_irq, for `/proc/console`. */
static uint32_t reader_drains, tick_drains;

/**
 * Whether COM1's bytes are kept out of the console's input, because its
 * keyboard is in K_RAW, K_MEDIUMRAW or K_OFF and the input holds key codes
 * alone (`kd_translates`), a DRM master's hold of K_OFF aside; the ring is
 * then emptied, its bytes discarded, so that none waits to be typed after
 * the mode ends. Interrupts off.
 */
static int console_serial_closed(void)
{
    if (kd_translates()) {
        return 0;
    }
    serial_rx_discard((int)serial_rx_pending());
    return 1;
}

/**
 * The console's polled input source: drain the serial ring, which the port's
 * interrupt and the tick fill, into the discipline.
 *
 * Runs in thread context with interrupts off, from tty_wait_cond before each
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
    if (console_serial_closed()) {
        return;
    }
    for (;;) {
        int ch, looked;

        if (!n_tty_has_room(&t->ldisc)) {
            if (console_signal_through(t, echo ? t->sink : null_sink, echo ? t->sink_ctx : NULL)) {
                continue;
            }
            console_lookahead(t, echo);
            break;
        }
        looked = serial_rx_looked();
        ch = serial_rx_take();
        if (ch < 0) {
            break;
        }
        if (echo) {
            tty_input(t, (unsigned char)ch, looked);
            continue;
        }
        tty_receive(t, (unsigned char)ch, null_sink, NULL, looked);
    }
    if (!echo && n_tty_readable(&t->ldisc)) {
        if (t->ready_us == 0) {
            t->ready_us = timer_now_us();
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
    if (console_serial_closed()) {
        return;
    }
    while (budget-- > 0 && serial_data_available()) {
        int ch, looked;

        if (!n_tty_has_room(&t->ldisc)) {
            if (console_signal_through(t, null_sink, NULL)) {
                continue;
            }
            console_lookahead(t, 0);
            break;
        }
        looked = serial_rx_looked();
        ch = serial_rx_take();
        if (ch < 0) {
            break;
        }
        tty_receive(t, (unsigned char)ch, null_sink, NULL, looked);
    }
}

static void console_service(void);

/** The serial driver's receive notify, run from the port's interrupt with
 * interrupts off once bytes are in the ring. It services the port as the
 * tick does (tty_console_poll), so a parked reader learns of input at once
 * and a session nobody reads for has its bytes moved into the discipline at
 * once; the tick remains for a port whose interrupt is not enabled. */
static void console_rx_notify(void)
{
    console_service();
}

void tty_console_init(void)
{
    unsigned int cols, rows;

    tty_init(&console_tty, console_sink, NULL);
    k_console_size(&cols, &rows);
    console_tty.winsize.ws_col = (uint16_t)cols;
    console_tty.winsize.ws_row = (uint16_t)rows;
    console_tty.pump = console_pump;
    console_tty.flow = console_flow;
    serial_set_rx_notify(console_rx_notify);
    blank_init();
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

/* The ticks between two readings of the port by the tick while its receive
 * interrupt is enabled, a fifth of a second: Rach2's own fallback for
 * an edge the interrupt lost, which the handler's loop makes rare. Linux
 * reads an 8250 that has an interrupt from no timer; its backup timer only
 * sends, for a UART with the THRE bug (8250_core.c:269-275), and a port
 * without an interrupt is polled every uart_poll_timeout
 * (8250_core.c:207-213). */
#define CONSOLE_BACKUP_TICKS (TICK_HZ / 5)

static void console_deliver(void);

/**
 * The UART's bytes go to the ring first, so a port whose interrupt is not
 * enabled is still read every tick; while it is enabled, the tick reads the
 * port every CONSOLE_BACKUP_TICKS, and the interrupt takes the rest: under KVM
 * a reading is a port access that QEMU serves in its own process, some 200 us,
 * which a reading every tick took from each millisecond. A reader or poller
 * parked on the console, which nothing else would tell of serial input while
 * another process runs, is then only woken: it drains and echoes the ring in
 * its own context (console_pump). With nobody parked and a session owning the
 * console, as when a foreground job runs while its shell waits in wait4, the
 * ring is drained here without echo (console_pump_irq), so a serial ^C/^Z still
 * reaches the foreground group and type-ahead waits for the next reader. With
 * no session the ring is left to the kernel debug shell, which polls it itself
 * and releases the console (tty_set_session 0) whenever it regains control.
 */
void tty_console_poll(void)
{
    static uint32_t read_at; /* the low half of `kernel_ticks` at the next reading */
    uint32_t now = (uint32_t)kernel_ticks;

    blank_tick();
    if (!serial_rx_irq_on() || (int32_t)(now - read_at) >= 0) {
        read_at = now + CONSOLE_BACKUP_TICKS;
        serial_rx_service(TTY_PUMP_BUDGET);
    }
    console_deliver();
}

/** Service the console's serial port, as `tty_console_poll` describes; the
 * port's interrupt runs this alone, without the blanking's countdown. */
static void console_service(void)
{
    serial_rx_service(TTY_PUMP_BUDGET);
    console_deliver();
}

/** Wake a reader or poller parked on the console when the ring holds bytes,
 * or with none parked and a session owning the console drain the ring into
 * the discipline, as `tty_console_poll` describes; no port access. */
static void console_deliver(void)
{
    if (!serial_data_available() || console_serial_closed()) {
        return;
    }
    if (wq_active(&console_tty.input_wq)) {
        wq_wake_all(&console_tty.input_wq);
    } else if (console_tty.sid != 0) {
        console_pump_irq(&console_tty);
    }
}
