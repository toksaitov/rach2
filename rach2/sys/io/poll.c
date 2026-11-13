/*
 * poll.c: the poll core, and poll, ppoll, select, _newselect and pselect6
 * on it, as poll.h describes them.
 *
 * A wait scans the descriptors with interrupts on and, when none is ready
 * and the deadline has not passed, puts a record on the wait queue of each
 * (`wq_poll_add`), scans once more, and sleeps unless a wake came since the
 * scan began; the records stay on their queues until the wait ends, and
 * every wake brings one new scan, as Linux's `do_poll` (fs/select.c:
 * 884-968). A descriptor answers through its poll operation (pipes,
 * terminals, devices, /proc files, the anonymous descriptors) or, for the
 * console and sockets, through a check of its type here. select translates
 * its fd_sets to an array of pollfd and back (poll_select.c) around the
 * same core.
 */

#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "poll.h"
#include "poll_select.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../tty/tty.h"
#include "../rtc.h"
#include "../timer.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../arch/arch.h"
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
 * is not left waiting; its write is then EBADF. A description of the console
 * opened before a vhangup(2) answers TTY_HUNG_POLL whatever was asked, as
 * Linux's hung_up_tty_poll does.
 */
static short poll_locked(fd_entry_t *e, short events, struct wait_queue **wq)
{
    if (e->ops && e->ops->poll) {
        return e->ops->poll(e, events, wq);
    }
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        return socket_poll(e->sock, events, wq);
    }
    if (e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) {
        return (short)(events & (POLLIN | POLLOUT));
    }
    if (e->type == FD_TYPE_CONSOLE && e->ofd && tty_desc_hung(&console_tty, e->ofd->tty_hangups)) {
        return (short)TTY_HUNG_POLL;
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

/** The question runs under the big lock unless the descriptor's poll runs
 * without it (`file_poll_unlocked`); the caller holds no spinlock. */
short poll_fd(fd_entry_t *e, short events, struct wait_queue **wq)
{
    int big = !file_poll_unlocked(e) && file_ops_lock(e);
    short revents;

    *wq = NULL;
    revents = poll_locked(e, events, wq);
    file_ops_unlock(big);
    return revents;
}

short poll_fd_held(fd_entry_t *e, short events, struct wait_queue **wq)
{
    *wq = NULL;
    return poll_locked(e, events, wq);
}

/* A poll's or a select's descriptors, for the scan of `poll_wait`. */
struct pollfd_scan {
    struct kernel_pollfd *fds;
    uint32_t nfds;
    int sel; /* nonzero for select's rule of readiness (`poll_scan`) */
};

/**
 * One scan of the descriptors of `p`: each entry's revents filled, the wait
 * queue of each descriptor put in `wqs`, one for each entry, NULL for an
 * entry with none, and the count of entries ready returned. A negative
 * descriptor is skipped with no events, as POSIX has it, and one that is
 * not open or was opened with O_PATH is POLLNVAL, ready, and not waited
 * on. Called with interrupts on (`poll_wait`).
 *
 * `p->sel` selects select's rule over poll's: poll counts an entry with any
 * revents, while select counts one only for an event that projects onto a
 * direction it was selected in (`pollfd_select_ready`), so a descriptor
 * selected for writing or exceptions that reports only POLLHUP is not ready,
 * as the projection onto the fd_sets says, and the wait does not spin.
 */
static int poll_scan(struct pollfd_scan *p, struct wait_queue **wqs)
{
    struct kernel_pollfd *fds = p->fds;
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
        revents = poll_fd(e, (short)(fds[i].events | POLLERR | POLLHUP), &wqs[i]);
        revents &= (short)(fds[i].events | POLLERR | POLLHUP | POLLNVAL);
        fds[i].revents = revents;
        if (p->sel ? pollfd_select_ready(fds[i].events, revents) : (revents != 0)) {
            ready++;
        }
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
 * The function of a wait's record on a queue (`wq_poll_add`): the thread
 * that owns it is marked woken, so that a wait whose scan raced with the
 * wake does not sleep, and is made ready if it sleeps, as Linux's
 * `pollwake` sets `triggered` before `default_wake_function` (fs/select.c:
 * 208-216). The record stays on its queue. Returns whether the thread was
 * asleep, which ends the turn of an exclusive record.
 */
static int poll_rec_wake(struct wq_waiter *w, uint32_t kinds, unsigned int nest, uint64_t *cpus)
{
    (void)nest;
    if (kinds & WQ_FREE) {
        return 0;
    }
    w->proc->poll_woken = 1;
    return wq_ready_deferred(w->proc, cpus);
}

/** Make `rec` the current thread's record, for a queue, with the flags
 * `flags`. */
static void poll_rec_init(struct wq_waiter *rec, uint32_t flags)
{
    rec->proc = current_proc;
    rec->next = NULL;
    rec->func = poll_rec_wake;
    rec->wq = NULL;
    rec->flags = flags;
}

/** Put the current thread's records `recs` on the `n` queues of `wqs`,
 * those not NULL. */
static void poll_file(struct wait_queue **wqs, struct wq_waiter *recs, uint32_t n)
{
    uint32_t flags = irq_save();
    uint32_t i;

    for (i = 0; i < n; i++) {
        poll_rec_init(&recs[i], 0);
        wq_poll_add(wqs[i], &recs[i]);
    }
    irq_restore(flags);
}

/** Take the current thread's records `recs` off the `n` queues of `wqs`. */
static void poll_unfile(struct wait_queue **wqs, struct wq_waiter *recs, uint32_t n)
{
    uint32_t flags = irq_save();
    uint32_t i;

    for (i = 0; i < n; i++) {
        wq_poll_remove(wqs[i], &recs[i]);
    }
    irq_restore(flags);
}

/** Clear the current thread's woken mark before a scan, with a full
 * barrier after the store, as Linux's `smp_store_mb(pwq->triggered, 0)`
 * (fs/select.c:255): a waker that changes what the scan reads and then
 * marks the thread woken either is seen by the scan or leaves the mark
 * set. */
static void poll_woken_clear(void)
{
    (void)arch_atomic_xchg((volatile unsigned int *)&current_proc->poll_woken, 0);
}

/**
 * Whether a signal ends the wait: 0 without the big lock
 * while the thread's signal-work flag (`sig_woken`) is clear, as Linux's
 * signal_pending tests TIF_SIGPENDING alone (include/linux/sched/signal.h:
 * 379-382, 404-416), and otherwise `signal_pending` under the big lock, which
 * the signal state needs and a poll, ppoll or epoll wait, which run without
 * it, takes for the question. A post sets the flag, then a full barrier, then
 * reads the sleeper's state (`sig_wake`), which the wait sets before its
 * second check.
 */
static int poll_signal_pending(void)
{
    int big, pending;

    if (!*(volatile uint32_t *)&current_proc->sig_woken) {
        return 0;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    pending = signal_pending();
    if (big) {
        bkl_exit();
    }
    return pending;
}

/** Whether the wait whose deadline is `d` is over at this tick. */
static int poll_expired(struct poll_deadline *d)
{
    return d->set && deadline_reached(d->tick) && deadline_passed(d);
}

/**
 * Sleep until a wake of a queue the thread has a record on, its deadline
 * `d` when set, or a signal, unless a wake came since its mark was cleared
 * (`poll_woken`) or signal work was posted since the caller's check of it
 * (`sig_woken`), as Linux's `poll_schedule_timeout` (fs/select.c:234-258)
 * and `__schedule`'s `signal_pending_state` (kernel/sched/core.c). The
 * thread is marked BLOCKED, with the barrier `sched_set_current_state`
 * makes, before it reads either flag: a wake or a post after that read finds
 * it blocked and makes it ready (`sig_wake`), so `schedule` keeps it
 * running or returns. The caller scans again and judges the signal. A finite
 * timeout sets the timer wake, `wakeup_tick`, which `proc_wake_sleepers`
 * acts on at the deadline: with another process runnable, schedule() does
 * not return here on its own. A computed tick of 0 is stored as 1, since 0
 * means not sleeping. A thread still blocked when schedule() returns,
 * which nothing woke and nothing else could run, idles until the next
 * interrupt, still on every queue so a waker can find it.
 */
static void poll_sleep(struct poll_deadline *d, const char *wchan)
{
    uint32_t flags;

    if (d->set) {
        current_proc->wakeup_tick = d->tick ? d->tick : 1;
    }
    current_proc->wchan = wchan;
    sched_set_current_state(PROC_BLOCKED);
    if (!current_proc->poll_woken && !*(volatile uint32_t *)&current_proc->sig_woken) {
        schedule();
        flags = irq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
        irq_restore(flags);
    }
    current_proc->state = PROC_RUNNING;
}

/**
 * The scan runs with interrupts on and the thread RUNNING, its woken mark
 * cleared first (`poll_woken_clear`). When it finds nothing ready, the
 * thread puts a record on every queue it named, which stay there until the
 * wait ends, and scans again, as Linux's first scan queues each entry
 * before it reads that file's mask (fs/select.c, `__pollwait`): a change
 * of readiness before that scan's look is seen by it, and one after wakes
 * a record and marks the thread woken. After that, a scan that finds
 * nothing ready is followed by a sleep (`poll_sleep`), and each wake by
 * one scan. A scan that names other queues than the records are on, as a
 * descriptor another thread replaced would, moves the records to them and
 * scans again. On return the thread is on no queue and its timer wake is
 * cleared.
 */
int poll_wait(poll_scan_fn scan, void *ctx, uint32_t n, struct poll_deadline *d, const char *wchan)
{
    struct wait_queue **wqs, **again;
    struct wq_waiter *recs;
    uint32_t m = n ? n : 1;
    int rc, filed = 0;
    uint32_t i;

    wqs = kzalloc(sizeof(*wqs) * 2u * m);
    recs = kzalloc(sizeof(*recs) * m);
    if (!wqs || !recs) {
        kfree(wqs);
        kfree(recs);
        return -ENOMEM;
    }
    again = wqs + m;
    for (;;) {
        int ready, changed = 0;

        poll_woken_clear();
        for (i = 0; i < n; i++) {
            again[i] = NULL;
        }
        ready = scan(ctx, again);
        if (ready > 0) {
            rc = ready;
            break;
        }
        if (poll_signal_pending()) {
            rc = -EINTR;
            break;
        }
        if (poll_expired(d)) {
            rc = 0;
            break;
        }
        for (i = 0; i < n; i++) {
            changed |= again[i] != wqs[i];
        }
        if (!filed || changed) {
            if (filed) {
                poll_unfile(wqs, recs, n);
            }
            for (i = 0; i < n; i++) {
                wqs[i] = again[i];
            }
            poll_file(wqs, recs, n);
            filed = 1;
            continue;
        }
        poll_sleep(d, wchan);
    }
    if (filed) {
        poll_unfile(wqs, recs, n);
    }
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;
    kfree(wqs);
    kfree(recs);
    return rc;
}

int poll_wait_queue(struct wait_queue *wq, int (*ready)(void *ctx), void *ctx,
                    struct poll_deadline *d, const char *wchan)
{
    struct wq_waiter rec;
    int rc;

    poll_rec_init(&rec, WQ_EXCLUSIVE);
    {
        uint32_t flags = irq_save();

        wq_poll_add(wq, &rec);
        irq_restore(flags);
    }
    poll_woken_clear();
    for (;;) {
        if (ready(ctx)) {
            rc = 1;
            break;
        }
        if (poll_signal_pending()) {
            rc = -EINTR;
            break;
        }
        if (poll_expired(d)) {
            rc = 0;
            break;
        }
        poll_sleep(d, wchan);
        poll_woken_clear();
    }
    {
        uint32_t flags = irq_save();

        wq_poll_remove(wq, &rec);
        irq_restore(flags);
    }
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;
    return rc;
}

/** `poll_scan` as a `poll_scan_fn`, whose context is a struct pollfd_scan. */
static int scan_pollfds(void *ctx, struct wait_queue **wqs)
{
    return poll_scan((struct pollfd_scan *)ctx, wqs);
}

/** The poll core over the `nfds` entries of `fds`, with select's rule of
 * readiness when `sel` (`poll_scan`); returns as `poll_wait` does, with a
 * record for each entry. */
static int do_poll(struct kernel_pollfd *fds, uint32_t nfds, struct poll_deadline *d, int sel)
{
    struct pollfd_scan p;

    p.fds = fds;
    p.nfds = nfds;
    p.sel = sel;
    return poll_wait(scan_pollfds, &p, nfds, d, sel ? "do_select" : "do_sys_poll");
}

/* The largest second a deadline holds, at which Linux's
 * timespec64_add_safe stops. */
#define DEADLINE_SEC_MAX ((int64_t)(((uint64_t)1 << 63) - 1u))

/** In `d`, the end of a wait of `sec` seconds and `nsec` nanoseconds that
 * begins now, by time since boot (`rtc_monotonic`). It is read before the
 * tick (`tick_deadline`), so that without a lost tick the clock has reached
 * the end by the deadline tick. An end past the largest second stops
 * there, as Linux's does. */
static void deadline_end(struct poll_deadline *d, long sec, long nsec)
{
    rtc_monotonic(&d->sec, &d->nsec);
    if ((int64_t)sec > DEADLINE_SEC_MAX - 1 - d->sec) {
        d->sec = DEADLINE_SEC_MAX;
        d->nsec = 0;
        return;
    }
    d->sec += sec;
    d->nsec += nsec;
    if (d->nsec >= 1000000000L) {
        d->nsec -= 1000000000L;
        d->sec++;
    }
}

/**
 * The time left until the end of `d` by time since boot, none when it has
 * passed, in `*sec` and `*nsec`: what Linux's select, pselect6 and ppoll
 * write back to the caller's timeout (poll_select_finish).
 */
static void deadline_left(const struct poll_deadline *d, int64_t *sec, int64_t *nsec)
{
    int64_t now_sec, now_nsec;

    rtc_monotonic(&now_sec, &now_nsec);
    *sec = d->sec - now_sec;
    *nsec = d->nsec - now_nsec;
    if (*nsec < 0) {
        *nsec += 1000000000L;
        (*sec)--;
    }
    if (*sec < 0) {
        *sec = 0;
        *nsec = 0;
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

/** Whether the timespec `ts` is one ppoll and pselect6 take: seconds not
 * negative and nanoseconds from 0 to 999999999. */
static int ts_valid(const struct poll_timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

/**
 * In `d`, the deadline of the timespec `ts`: the first tick at or after its
 * end (`tick_deadline`), and its end by time since boot; none for a NULL
 * `ts`. A timeout of 2^40 seconds or more, which `tick_span` cuts, reaches
 * its tick only after that span.
 */
static void ts_deadline(const struct poll_timespec *ts, struct poll_deadline *d)
{
    d->set = ts != NULL;
    if (!d->set) {
        return;
    }
    deadline_end(d, ts->tv_sec, ts->tv_nsec);
    d->tick = tick_deadline(tick_span(ts->tv_sec, 0), ((uint32_t)ts->tv_nsec + 999u) / 1000u);
}

/** Normalize `tv` to seconds and microseconds, as Linux's select does,
 * and put its deadline in `d`, or none for a NULL `tv`. Return 0, or
 * -EINVAL if the normalized seconds or fraction are negative. The sum
 * wraps in the caller's native long width without signed overflow. */
static int tv_deadline(struct poll_timeval *tv, struct poll_deadline *d)
{
    struct poll_timespec ts;

    if (!tv) {
        ts_deadline(NULL, d);
        return 0;
    }
    ts.tv_sec = (long)((unsigned long)tv->tv_sec + (unsigned long)(tv->tv_usec / 1000000L));
    ts.tv_nsec = (tv->tv_usec % 1000000L) * 1000L;
    if (!ts_valid(&ts)) {
        return -EINVAL;
    }
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec %= 1000000L;
    ts_deadline(&ts, d);
    return 0;
}

/* Linux's personality flag that keeps select's and ppoll's timeout as the
 * caller gave it. */
#define STICKY_TIMEOUTS 0x4000000u

/* The form of a timeval and of a timespec of longs, the native calls'. */
#define LEFT_LONGS (sizeof(long) > 4u ? POLL_LEFT_WIDE : 0)
#define LEFT_TV    (POLL_LEFT_USEC | LEFT_LONGS)

/** Write `sec` seconds and `nsec` nanoseconds to the timeout at `ut`, in
 * the form `form`, POLL_LEFT_ bits; 0 or -EFAULT. */
static int poll_put_left(uintptr_t ut, int form, int64_t sec, int64_t nsec)
{
    void __user *u = (void __user *)ut;
    /* Under a second, so 32 bits divide it, with no libgcc on i686. */
    int64_t part = (form & POLL_LEFT_USEC) ? (int32_t)nsec / 1000 : nsec;

    if (form & POLL_LEFT_WIDE) {
        int64_t w[2];

        w[0] = sec;
        w[1] = part;
        return copy_to_user(u, w, sizeof(w)) != 0 ? -EFAULT : 0;
    } else {
        int32_t w[2];

        w[0] = (int32_t)sec;
        w[1] = (int32_t)part;
        return copy_to_user(u, w, sizeof(w)) != 0 ? -EFAULT : 0;
    }
}

int32_t poll_left_out(int32_t ret, uintptr_t ut, int form, int zero, int64_t sec, int64_t nsec)
{
    if (!ut || zero) {
        return ret;
    }
    if ((current_proc->persona & STICKY_TIMEOUTS) != 0 || poll_put_left(ut, form, sec, nsec) != 0) {
        if (ret == -EINTR && current_proc->restart_kind == RESTART_NOHAND) {
            current_proc->restart_kind = RESTART_NONE;
        }
    }
    return ret;
}

int32_t poll_finish(int32_t ret, uintptr_t ut, const struct poll_deadline *d, int form, int zero)
{
    int64_t sec = 0, nsec = 0;

    if (ret == -EINTR) {
        current_proc->restart_kind = RESTART_NOHAND;
    }
    if (ut && !zero) {
        deadline_left(d, &sec, &nsec);
    }
    return poll_left_out(ret, ut, form, zero, sec, nsec);
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

static long poll_restart(struct isr_params *params);

/**
 * File a poll a signal cut short, its result `ret`, for its restart, as
 * Linux's poll does: the array, its count and the deadline `d` go into the
 * thread's restart block, and the call stands for ERESTART_RESTARTBLOCK,
 * which a signal that runs no handler restarts through restart_syscall
 * (`poll_restart`), to the same deadline, and a handler ends with EINTR.
 */
static int32_t poll_filed(int32_t ret, uintptr_t ufds, uint32_t nfds, const struct poll_deadline *d)
{
    struct restart_block *b = &current_proc->restart;

    if (ret == -EINTR) {
        b->fn = poll_restart;
        b->uaddr = ufds;
        b->val = nfds;
        b->timed = (uint8_t)(d->set != 0);
        b->tick = d->tick;
        b->sec = d->sec;
        b->nsec = d->nsec;
        current_proc->restart_kind = RESTART_BLOCK;
    }
    return ret;
}

/** The resumption of a poll a signal that ran no handler cut short
 * (restart_syscall): the array the block keeps, to the deadline it keeps,
 * filed again for a further signal, as Linux's do_restart_poll does. */
static long poll_restart(struct isr_params *params)
{
    const struct restart_block *b = &current_proc->restart;
    struct poll_deadline d;
    uintptr_t ufds = b->uaddr;
    uint32_t nfds = b->val;

    (void)params;
    d.set = b->timed;
    d.tick = b->tick;
    d.sec = b->sec;
    d.nsec = b->nsec;
    return poll_filed(poll_common(ufds, nfds, &d), ufds, nfds, &d);
}

long sys_poll(struct isr_params *params)
{
    uintptr_t ufds = SC_ARG(params, 0);
    uint32_t nfds = (uint32_t)SC_ARG(params, 1);
    struct poll_deadline d;

    if (!current_proc) {
        return -ESRCH;
    }
    poll_ms_deadline((int)SC_ARG(params, 2), &d);
    return poll_filed(poll_common(ufds, nfds, &d), ufds, nfds, &d);
}

/**
 * The mask it replaces becomes the thread's saved mask: `deliver_signals`
 * puts it back after the handler a signal runs for this wait, or at once
 * when none runs, and a wait that did not end for a signal puts it back
 * through `poll_mask_done`. Both masks are set through `signal_set_blocked`,
 * which leaves a signal of the process that a mask blocks to another thread,
 * under the big lock, which the signal state needs and ppoll, which runs
 * without it, takes for the change.
 */
int32_t poll_mask_set(uintptr_t umask, uint32_t size)
{
    uint64_t mask;
    int big;

    if (!umask) {
        return 0;
    }
    if (size != sizeof(mask)) {
        return -EINVAL;
    }
    if (copy_from_user(&mask, (const void __user *)umask, sizeof(mask)) != 0) {
        return -EFAULT;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    current_proc->saved_sigmask = current_proc->sig_blocked;
    current_proc->sigmask_saved = 1;
    proc_work_set(current_proc, WORK_SIGNAL);
    signal_set_blocked(mask);
    if (big) {
        bkl_exit();
    }
    return 0;
}

int32_t poll_mask_done(int32_t ret)
{
    if (current_proc->sigmask_saved && ret != -EINTR) {
        int big = !bkl_held();

        if (big) {
            bkl_enter();
        }
        current_proc->sigmask_saved = 0;
        signal_set_blocked(current_proc->saved_sigmask);
        if (big) {
            bkl_exit();
        }
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

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
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
    return poll_finish(poll_mask_done(poll_common(ufds, nfds, &d)), uts, &d, LEFT_LONGS,
                       ts.tv_sec == 0 && ts.tv_nsec == 0);
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

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (utv) {
        if (copy_from_user(&tv, (const void __user *)utv, sizeof(tv)) != 0) {
            return -EFAULT;
        }
    }
    if (tv_deadline(utv ? &tv : NULL, &d) != 0) {
        return -EINVAL;
    }
    return poll_finish(select_common(nfds, ur, uw, ue, &d), utv, &d, LEFT_TV,
                       tv.tv_sec == 0 && tv.tv_usec == 0);
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

        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (a[4]) {
            if (copy_from_user(&tv, (const void __user *)a[4], sizeof(tv)) != 0) {
                return -EFAULT;
            }
        }
        if (tv_deadline(a[4] ? &tv : NULL, &d) != 0) {
            return -EINVAL;
        }
        return poll_finish(select_common((int)a[0], a[1], a[2], a[3], &d), a[4], &d, LEFT_TV,
                           tv.tv_sec == 0 && tv.tv_usec == 0);
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
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
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
    return poll_finish(poll_mask_done(select_common(nfds, ur, uw, ue, &d)), uts, &d, LEFT_LONGS,
                       ts.tv_sec == 0 && ts.tv_nsec == 0);
}
