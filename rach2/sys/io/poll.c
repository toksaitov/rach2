/*
 * poll.c: the poll core, and poll, ppoll, select, _newselect and pselect6
 * on it, as poll.h describes them.
 *
 * A wait scans the descriptors with interrupts off and, when none is ready
 * and the deadline has not passed, puts the process on the wait queue of
 * each (`wq_poll_add`) before interrupts are on again, so that no wake
 * falls between the scan and the sleep; every wake brings a new scan. A
 * descriptor answers through its poll operation (pipes, terminals, devices,
 * /proc files, the anonymous descriptors) or, for the console and sockets,
 * through a check of its type here. select translates its fd_sets to an
 * array of pollfd and back (poll_select.c) around the same core.
 */

#include "../lib/kirq.h"
#include "poll.h"
#include "poll_select.h"
#include "epoll.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../proc/sched.h"
#include "../tty/tty.h"
#include "../rtc.h"
#include "../timer.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../network/socket/socket.h"

/* The timespec of longs ppoll and pselect6 take, 32 bits each on i686 and
 * 64 on x86-64. */
struct poll_timespec {
    long tv_sec;
    long tv_nsec;
};

/* The timeval of longs the select calls take. */
struct poll_timeval {
    long tv_sec;
    long tv_usec;
};

/** Turn interrupts off and return the flags that turn them back
 * (`kirq_save`). */
static uint32_t irq_save(void)
{
    return kirq_save();
}

/** Restore the interrupt flag `flags` that `irq_save` returned. */
static void irq_restore(uint32_t flags)
{
    kirq_restore(flags);
}

/**
 * A descriptor with a poll operation answers through it, and a socket
 * through the network stack. A regular file, a directory, the framebuffer
 * and the mouse are always ready and have no queue. The console answers
 * through its tty: POLLIN, whose query drains serial input, only for a
 * readable descriptor, so poll(stdout, POLLOUT) never drains serial input,
 * and POLLOUT for any descriptor, as a terminal answers on Linux whatever
 * the descriptor's mode, so that a caller polling a read-only one for output
 * is not left waiting; its write is then EBADF.
 */
short poll_fd(fd_entry_t *e, short events, struct wait_queue **wq)
{
    *wq = NULL;

    if (e->ops && e->ops->poll) {
        return e->ops->poll(e, events, wq);
    }
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        return socket_poll(e->sock, events, wq);
    }
    if (e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) {
        return (short)(events & (POLLIN | POLLOUT));
    }
    if (e->type == FD_TYPE_CONSOLE) {
        short want = 0;
        if (e->flags & FD_FLAG_READ) {
            want = (short)(events & POLLIN);
        }
        want = (short)(want | (events & POLLOUT));
        return tty_poll(&console_tty, want, wq);
    }
    return (short)(events & (POLLIN | POLLOUT));
}

/* A poll's or a select's descriptors, for the scan of `poll_wait`: the
 * queues it has records for, and the queues the last scan found, when they
 * were more with nothing ready, for the wait to begin again with. */
struct pollfd_scan {
    struct kernel_pollfd *fds;
    uint32_t nfds;
    int sel; /* nonzero for select's rule of readiness (`poll_scan`) */
    uint32_t room;
    uint32_t grow;
};

/**
 * The readiness of `e` and its wait queues, into `wqs` from `*used` on as
 * far as `room` goes, `*used` counting them all: one queue, or for an epoll
 * set the set's own and those of what it watches, so that a change in any
 * of them wakes a poll of the set (`epoll_poll_set`).
 */
static short poll_one(fd_entry_t *e, short events, struct wait_queue **wqs, uint32_t room,
                      uint32_t *used)
{
    struct wait_queue *wq;
    short revents;

    if (epoll_is_set(e)) {
        uint32_t n = 0;

        revents = epoll_poll_set(e, events, *used < room ? wqs + *used : NULL,
                                 *used < room ? room - *used : 0, &n);
        *used += n;
        return revents;
    }
    revents = poll_fd(e, events, &wq);
    if (wq) {
        if (*used < room) {
            wqs[*used] = wq;
        }
        (*used)++;
    }
    return revents;
}

/**
 * One scan of the descriptors of `p`: each entry's revents filled, the wait
 * queues put in `wqs` as far as `p->room` goes, and the count of entries
 * ready returned. A negative descriptor is skipped with no events, as POSIX
 * has it, and one that is not open or was opened with O_PATH is POLLNVAL,
 * ready, and not waited on. Called with interrupts off (`poll_wait`).
 *
 * `p->sel` selects select's rule over poll's: poll counts an entry with any
 * revents, while select counts one only for an event that projects onto a
 * direction it was selected in (`pollfd_select_ready`), so a descriptor
 * selected for writing or exceptions that reports only POLLHUP is not ready,
 * as the projection onto the fd_sets says, and the wait does not spin.
 *
 * An epoll set has a queue for each descriptor it watches, so the queues
 * can be more than the records the wait has; with nothing ready, the scan
 * says so in `p->grow` and returns 1 to end the wait, which begins again
 * with a record for each.
 */
static int poll_scan(struct pollfd_scan *p, struct wait_queue **wqs)
{
    struct kernel_pollfd *fds = p->fds;
    uint32_t used = 0;
    uint32_t i;
    int ready = 0;

    for (i = 0; i < p->nfds; i++) {
        fd_entry_t *e;
        short revents;

        if (fds[i].fd < 0) {
            fds[i].revents = 0;
            continue;
        }
        e = fd_get(current_proc->fds, fds[i].fd);
        if (!e || (e->flags & FD_FLAG_PATH)) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        revents = poll_one(e, (short)(fds[i].events | POLLERR | POLLHUP), wqs, p->room, &used);
        revents &= (short)(fds[i].events | POLLERR | POLLHUP | POLLNVAL);
        fds[i].revents = revents;
        if (p->sel ? pollfd_select_ready(fds[i].events, revents) : (revents != 0)) {
            ready++;
        }
    }
    if (ready == 0 && used > p->room) {
        p->grow = used;
        return 1;
    }
    return ready;
}

/** Whether the tick `deadline` has come: it is the count's tick or an
 * earlier one. */
static int deadline_reached(uint64_t deadline)
{
    return tick_count() >= deadline;
}

/**
 * Whether a wait whose deadline tick has come is over: whether time since
 * boot, read as `clock_gettime` reads it, has reached the end of `d`.
 *
 * A tick the timer lost while the processor could not take it leaves the
 * tick count behind that clock, which keeps its last reading
 * (`rtc_monotonic`), and a wait counted in ticks alone would end before the
 * clock showed it had lasted its timeout. The deadline then moves to the
 * first tick at which time since boot, counted in ticks, reaches the end;
 * the tick count and that time are read as one reading (`tick_now`).
 */
static int deadline_passed(struct poll_deadline *d)
{
    int64_t sec, nsec;
    uint32_t seconds32, ticks32;
    uint64_t now, due, base;

    rtc_monotonic(&sec, &nsec);
    if (sec > d->sec || (sec == d->sec && nsec >= d->nsec)) {
        return 1;
    }
    tick_now(&seconds32, &ticks32, &base);
    now = (uint64_t)seconds32 * TICK_HZ + ticks32;
    due = tick_span(d->sec, (uint32_t)d->nsec);
    if (due <= now) {
        return 1;
    }
    d->tick = base + (due - now);
    return 0;
}

/**
 * The process goes on every queue the scan named with interrupts still off
 * from the scan, so that a change of readiness cannot fall between the two
 * and be lost. A finite timeout also sets the timer wake, `wakeup_tick`,
 * which `proc_wake_sleepers` acts on at the deadline: with another process
 * runnable, schedule() does not return here on its own, and a wait on no
 * queue or on objects that never fire would otherwise sleep past its
 * deadline. A computed tick of 0 is stored as 1, since 0 means not
 * sleeping. A process still blocked when schedule() returns, which nothing
 * woke and nothing else could run, idles until the next interrupt, still on
 * every queue so a waker can find it, and leaves the queues after. On
 * return the process is on no queue and its timer wake is cleared.
 *
 * While it sleeps, the process's record names the queues and its records
 * on them (`poll_wqs`), so that a close by another thread can take it off
 * them all, with each queue set to NULL, before it frees what an epoll
 * wait sleeps on (`epoll_waits_let_go`); the process looks at the arrays
 * again only with the big lock, which that close holds.
 */
int poll_wait(poll_scan_fn scan, void *ctx, uint32_t n, struct poll_deadline *d)
{
    struct wait_queue **wqs;
    struct wq_waiter *recs;
    uint32_t flags;
    int rc;
    uint32_t i;

    wqs = kzalloc(sizeof(*wqs) * (n ? n : 1));
    recs = kzalloc(sizeof(*recs) * (n ? n : 1));
    if (!wqs || !recs) {
        kfree(wqs);
        kfree(recs);
        return -ENOMEM;
    }
    flags = irq_save();
    for (;;) {
        int ready;

        for (i = 0; i < n; i++) {
            wqs[i] = NULL;
        }
        ready = scan(ctx, wqs);
        if (ready > 0) {
            rc = ready;
            break;
        }
        if (signal_pending()) {
            rc = -EINTR;
            break;
        }
        if (d->set && deadline_reached(d->tick) && deadline_passed(d)) {
            rc = 0;
            break;
        }

        for (i = 0; i < n; i++) {
            recs[i].proc = current_proc;
            recs[i].next = NULL;
            wq_poll_add(wqs[i], &recs[i]);
        }
        current_proc->poll_wqs = wqs;
        current_proc->poll_recs = recs;
        current_proc->poll_n = n;
        if (d->set) {
            current_proc->wakeup_tick = d->tick ? d->tick : 1;
        }
        current_proc->state = PROC_BLOCKED;
        irq_restore(flags);

        schedule();

        flags = irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
        for (i = 0; i < n; i++) {
            wq_poll_remove(wqs[i], &recs[i]);
        }
        current_proc->poll_wqs = NULL;
        current_proc->poll_recs = NULL;
        current_proc->poll_n = 0;
        current_proc->state = PROC_RUNNING;
    }

    current_proc->wakeup_tick = 0;
    for (i = 0; i < n; i++) {
        wq_poll_remove(wqs[i], &recs[i]);
    }
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    kfree(wqs);
    kfree(recs);
    return rc;
}

/** `poll_scan` as a `poll_scan_fn`, whose context is a struct pollfd_scan. */
static int scan_pollfds(void *ctx, struct wait_queue **wqs)
{
    return poll_scan((struct pollfd_scan *)ctx, wqs);
}

/** The poll core over the `nfds` entries of `fds`, with select's rule of
 * readiness when `sel` (`poll_scan`); returns as `poll_wait` does. A wait
 * whose scan found more queues than it had records for begins again with a
 * record for each. */
static int do_poll(struct kernel_pollfd *fds, uint32_t nfds, struct poll_deadline *d, int sel)
{
    struct pollfd_scan p;
    int rc;

    p.fds = fds;
    p.nfds = nfds;
    p.sel = sel;
    p.grow = nfds;
    do {
        p.room = p.grow;
        p.grow = 0;
        rc = poll_wait(scan_pollfds, &p, p.room, d);
    } while (rc > 0 && p.grow != 0);
    return rc;
}

/** In `d`, the end of a wait of `sec` seconds and `nsec` nanoseconds that
 * begins now, by time since boot (`rtc_monotonic`). It is read before the
 * tick (`tick_deadline`), so that without a lost tick the clock has reached
 * the end by the deadline tick. */
static void deadline_end(struct poll_deadline *d, long sec, long nsec)
{
    rtc_monotonic(&d->sec, &d->nsec);
    d->sec += sec;
    d->nsec += nsec;
    if (d->nsec >= 1000000000L) {
        d->nsec -= 1000000000L;
        d->sec++;
    }
}

/**
 * The tick at which a wait of `ticks` whole ticks and `us` microseconds, at
 * most a second's, ends: the first tick at or after its end, measured from
 * where in the tick in progress the call is (`rtc_tick_phase`), so a nonzero
 * wait is never shorter than asked and at most a tick longer, unless a tick
 * is lost. A zero wait ends at the tick count itself (`tick_count`), which
 * `deadline_reached` reads, and not at `rtc_tick_phase`'s, which counts a
 * tick the timer has reached and not yet raised: that tick would put the
 * deadline ahead, and the wait would sleep until it, where Linux's zero
 * timeout never sleeps.
 */
static uint64_t tick_deadline(uint64_t ticks, uint32_t us)
{
    uint64_t now;
    uint32_t phase_ns;

    if (ticks == 0 && us == 0) {
        return tick_count();
    }
    rtc_tick_phase(&now, &phase_ns);
    return now + ticks + (phase_ns / 1000u + us + (TICK_US - 1u)) / TICK_US;
}

void poll_ms_deadline(int timeout_ms, struct poll_deadline *d)
{
    d->set = timeout_ms >= 0;
    if (!d->set) {
        return;
    }
    deadline_end(d, timeout_ms / 1000, timeout_ms % 1000 * 1000000L);
    d->tick = tick_deadline(tick_span(timeout_ms / 1000, 0), (uint32_t)(timeout_ms % 1000) * 1000u);
}

/**
 * Whether the timeval `tv` is one the select calls take: seconds not
 * negative and microseconds from 0 to 999999. They refuse another with
 * -EINVAL, as Linux does, and the range keeps the conversions below free of
 * signed overflow.
 */
static int tv_valid(const struct poll_timeval *tv)
{
    return tv->tv_sec >= 0 && tv->tv_usec >= 0 && tv->tv_usec < 1000000;
}

/** Whether the timespec `ts` is one ppoll and pselect6 take: seconds not
 * negative and nanoseconds from 0 to 999999999. */
static int ts_valid(const struct poll_timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

/**
 * The timeval `tv` as a timeout in milliseconds for `poll_ms_deadline`,
 * rounded up. Seconds past 2147482 give 2147483647 milliseconds, about 24.8
 * days, so the product cannot overflow, and negative seconds give 0, a poll
 * that does not wait. A timeval thus always gives a finite timeout; only a
 * null one waits without end.
 */
static int tv_to_ms(const struct poll_timeval *tv)
{
    if (tv->tv_sec < 0) {
        return 0;
    }
    if (tv->tv_sec > 2147482) {
        return 0x7FFFFFFF;
    }
    return (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);
}

/**
 * In `d`, the deadline of the timespec `ts`: the first tick at or after its
 * end (`tick_deadline`), and its end by time since boot; none for a NULL
 * `ts`. A timeout of 2^40 seconds or more, which `tick_span` cuts, ends at
 * its tick alone.
 */
static void ts_deadline(const struct poll_timespec *ts, struct poll_deadline *d)
{
    int64_t sec;

    d->set = ts != NULL;
    if (!d->set) {
        return;
    }
    sec = ts->tv_sec;
    d->sec = 0;
    d->nsec = 0;
    if (sec < ((int64_t)1 << 40)) {
        deadline_end(d, ts->tv_sec, ts->tv_nsec);
    }
    d->tick = tick_deadline(tick_span(ts->tv_sec, 0), ((uint32_t)ts->tv_nsec + 999u) / 1000u);
}

/** The body of poll and ppoll: the `nfds` entries at `ufds` waited on until
 * the deadline `d` when it is set, and their revents copied back. No entries
 * is a sleep until the deadline. Linux takes no more entries than the soft
 * RLIMIT_NOFILE; here 1024 are taken under any limit, so that OpenSSH's
 * rlimit sandbox, a limit of 0, still polls. Returns as `sys_poll` does. */
static int32_t poll_common(uintptr_t ufds, uint32_t nfds, struct poll_deadline *d)
{
    struct kernel_pollfd *fds;
    uint32_t bytes;
    int ready;

    if (!current_proc) {
        return -ESRCH;
    }
    if (nfds > FD_TABLE_LIMIT && nfds > current_proc->rlim[RLIM_NOFILE].cur) {
        return -EINVAL; /* more than the process may hold */
    }
    bytes = nfds * (uint32_t)sizeof(struct kernel_pollfd);
    fds = kmalloc(bytes ? bytes : 1);
    if (!fds) {
        return -ENOMEM;
    }
    if (nfds > 0 && copy_from_user(fds, (const void __user *)ufds, bytes) != 0) {
        kfree(fds);
        return -EFAULT;
    }

    ready = do_poll(fds, nfds, d, 0);
    if (ready >= 0 && nfds > 0 && copy_to_user((void __user *)ufds, fds, bytes) != 0) {
        ready = -EFAULT;
    }
    kfree(fds);
    return ready;
}

long sys_poll(struct isr_params *params)
{
    uintptr_t ufds = SC_ARG(params, 0);
    uint32_t nfds = (uint32_t)SC_ARG(params, 1);
    struct poll_deadline d;

    poll_ms_deadline((int)SC_ARG(params, 2), &d);
    return poll_common(ufds, nfds, &d);
}

/**
 * The mask it replaces becomes the thread's saved mask: `deliver_signals`
 * puts it back after the handler a signal runs for this wait, or at once
 * when none runs, and a wait that did not end for a signal puts it back
 * through `poll_mask_done`. Both masks are set through `signal_set_blocked`,
 * which leaves a signal of the process that a mask blocks to another thread.
 */
int32_t poll_mask_set(uintptr_t umask, uint32_t size)
{
    uint64_t mask;

    if (!umask) {
        return 0;
    }
    if (size != sizeof(mask)) {
        return -EINVAL;
    }
    if (copy_from_user(&mask, (const void __user *)umask, sizeof(mask)) != 0) {
        return -EFAULT;
    }
    current_proc->saved_sigmask = current_proc->sig_blocked;
    current_proc->sigmask_saved = 1;
    signal_set_blocked(mask);
    return 0;
}

int32_t poll_mask_done(int32_t ret)
{
    if (current_proc->sigmask_saved && ret != -EINTR) {
        current_proc->sigmask_saved = 0;
        signal_set_blocked(current_proc->saved_sigmask);
    }
    return ret;
}

long sys_ppoll(struct isr_params *params)
{
    uintptr_t ufds = SC_ARG(params, 0);
    uint32_t nfds = (uint32_t)SC_ARG(params, 1);
    uintptr_t uts = SC_ARG(params, 2);
    struct poll_timespec ts;
    struct poll_deadline d;

    if (uts) {
        if (copy_from_user(&ts, (const void __user *)uts, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (!ts_valid(&ts)) {
            return -EINVAL;
        }
        ts_deadline(&ts, &d);
    } else {
        ts_deadline(NULL, &d);
    }
    if (!current_proc) {
        return -ESRCH;
    }
    {
        int32_t rc = poll_mask_set(SC_ARG(params, 3), USER_LEN32(SC_ARG(params, 4)));

        if (rc != 0) {
            return rc;
        }
    }
    return poll_mask_done(poll_common(ufds, nfds, &d));
}

/**
 * Copy `bytes` of the user fd_set at `u`, when `u` is not 0, into `set`.
 * Returns 0, or -EFAULT when the set cannot be read.
 */
static int32_t fdset_in(uint32_t *set, uintptr_t u, uint32_t bytes)
{
    return u && copy_from_user(set, (const void __user *)u, bytes) != 0 ? -EFAULT : 0;
}

/**
 * Copy `bytes` of `set` to the user fd_set at `u`, when `u` is not 0.
 * Returns 0, or -EFAULT when the set cannot be written.
 */
static int32_t fdset_out(uintptr_t u, const uint32_t *set, uint32_t bytes)
{
    return u && copy_to_user((void __user *)u, set, bytes) != 0 ? -EFAULT : 0;
}

/**
 * The body of the select calls: the descriptors below `nfds` set in the
 * user fd_sets at `ur`, `uw` and `ue`, each 0 for none, waited on until the
 * deadline `d` when it is set, and the sets the caller gave written back.
 * Returns as `sys__newselect` does.
 *
 * As on Linux, `nfds` is cut to the size of the caller's table, past which
 * no descriptor is open, and the table counts as at least a long's bits
 * wide, as Linux's always is, so that a bit in the first long for a
 * descriptor that is not open is -EBADF and not a bit ignored, after which
 * a wait with no timeout would never end. Each set is read and written for
 * the longs that hold its first `nfds` bits, no more, so that a caller may
 * pass sets wider than FD_SETSIZE for a descriptor past 1023. A long's bits
 * are the bits of the 32-bit words the translation reads, on a little-endian
 * machine. The sets come from the heap, sized by the call.
 *
 * A set bit whose descriptor is not open fails the call with -EBADF: the
 * scan reports such a descriptor as POLLNVAL, which no fd_set can show, and
 * counts it ready, so the wait ends at once.
 */
static int32_t select_common(int nfds, uintptr_t ur, uintptr_t uw, uintptr_t ue,
                             struct poll_deadline *d)
{
    const uint32_t long_bits = 8u * (uint32_t)sizeof(long);
    struct kernel_pollfd *fds;
    uint32_t *sets, *rset, *wset, *eset, *orset, *owset, *oeset;
    uint32_t setbytes, words;
    int npoll, ready, i, cut;

    if (!current_proc) {
        return -ESRCH;
    }
    if (nfds < 0) {
        return -EINVAL;
    }
    cut = fd_table_cap(current_proc->fds);
    if (cut < (int)long_bits) {
        cut = (int)long_bits;
    }
    if (nfds > cut) {
        nfds = cut;
    }
    setbytes = ((uint32_t)nfds + long_bits - 1) / long_bits * (uint32_t)sizeof(long);
    words = setbytes / 4;

    sets = kzalloc(6 * (size_t)setbytes + sizeof(*fds) * (size_t)(nfds ? nfds : 1));
    if (!sets) {
        return -ENOMEM;
    }
    rset = sets;
    wset = rset + words;
    eset = wset + words;
    orset = eset + words;
    owset = orset + words;
    oeset = owset + words;
    fds = (struct kernel_pollfd *)(oeset + words);
    ready = 0;
    if (fdset_in(rset, ur, setbytes) || fdset_in(wset, uw, setbytes) ||
        fdset_in(eset, ue, setbytes)) {
        ready = -EFAULT;
    }
    npoll = ready == 0 ? fdsets_to_pollfds((uint32_t)nfds, rset, wset, eset, fds, nfds) : 0;
    if (ready == 0 && npoll < 0) {
        ready = -EBADF; /* more selected descriptors than `fds` holds */
    }
    if (ready == 0) {
        ready = do_poll(fds, (uint32_t)npoll, d, 1);
    }
    for (i = 0; ready >= 0 && i < npoll; i++) {
        if (fds[i].revents & POLLNVAL) {
            ready = -EBADF;
        }
    }
    if (ready >= 0) {
        ready = pollfds_to_fdsets(fds, npoll, orset, owset, oeset);
        if (fdset_out(ur, orset, setbytes) || fdset_out(uw, owset, setbytes) ||
            fdset_out(ue, oeset, setbytes)) {
            ready = -EFAULT;
        }
    }
    kfree(sets);
    return ready;
}

long sys__newselect(struct isr_params *params)
{
    int nfds = (int)SC_ARG(params, 0);
    uintptr_t ur = SC_ARG(params, 1);
    uintptr_t uw = SC_ARG(params, 2);
    uintptr_t ue = SC_ARG(params, 3);
    uintptr_t utv = SC_ARG(params, 4);
    struct poll_timeval tv;
    struct poll_deadline d;

    if (utv) {
        if (copy_from_user(&tv, (const void __user *)utv, sizeof(tv)) != 0) {
            return -EFAULT;
        }
        if (!tv_valid(&tv)) {
            return -EINVAL;
        }
        poll_ms_deadline(tv_to_ms(&tv), &d);
    } else {
        poll_ms_deadline(-1, &d);
    }
    return select_common(nfds, ur, uw, ue, &d);
}

long sys_select(struct isr_params *params)
{
    /* The old i386 select packs its five arguments behind one pointer. */
    uintptr_t argp = SC_ARG(params, 0);
    unsigned long a[5]; /* Linux's sel_arg_struct: n, inp, outp, exp, tvp */

    if (!argp || copy_from_user(a, (const void __user *)argp, sizeof(a)) != 0) {
        return -EFAULT;
    }
    {
        struct poll_timeval tv;
        struct poll_deadline d;

        if (a[4]) {
            if (copy_from_user(&tv, (const void __user *)a[4], sizeof(tv)) != 0) {
                return -EFAULT;
            }
            if (!tv_valid(&tv)) {
                return -EINVAL;
            }
            poll_ms_deadline(tv_to_ms(&tv), &d);
        } else {
            poll_ms_deadline(-1, &d);
        }
        return select_common((int)a[0], a[1], a[2], a[3], &d);
    }
}

long sys_pselect6(struct isr_params *params)
{
    int nfds = (int)SC_ARG(params, 0);
    uintptr_t ur = SC_ARG(params, 1);
    uintptr_t uw = SC_ARG(params, 2);
    uintptr_t ue = SC_ARG(params, 3);
    uintptr_t uts = SC_ARG(params, 4);
    struct poll_timespec ts;
    struct poll_deadline d;

    struct {
        uintptr_t mask;
        unsigned long size;
    } sigarg = {0, 0}; /* the mask's address and its size in bytes */

    if (!current_proc) {
        return -ESRCH;
    }
    /* The sixth argument points at the mask's address and its size. */
    if (SC_ARG(params, 5) &&
        copy_from_user(&sigarg, (const void __user *)SC_ARG(params, 5), sizeof(sigarg)) != 0) {
        return -EFAULT;
    }
    if (uts) {
        if (copy_from_user(&ts, (const void __user *)uts, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (!ts_valid(&ts)) {
            return -EINVAL;
        }
        ts_deadline(&ts, &d);
    } else {
        ts_deadline(NULL, &d);
    }
    {
        int32_t rc = poll_mask_set(sigarg.mask, USER_LEN32(sigarg.size));

        if (rc != 0) {
            return rc;
        }
    }
    return poll_mask_done(select_common(nfds, ur, uw, ue, &d));
}
