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
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../network/socket/socket.h"

/* At most this many descriptors can be polled at once: the most a process
 * may hold. The per-call arrays come from the heap, sized by the call. */
#define POLL_MAX_FDS FD_TABLE_LIMIT

extern volatile uint32_t kernel_ticks;

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

/**
 * Whether the tick `deadline` has come: it is the count's tick or an earlier
 * one, compared by the signed difference so the count's wrap does not matter.
 */
static int deadline_reached(uint32_t deadline)
{
    return (int32_t)(deadline - kernel_ticks) <= 0;
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
 */
int poll_wait(poll_scan_fn scan, void *ctx, uint32_t n, int has_deadline, uint32_t deadline)
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
        if (has_deadline && deadline_reached(deadline)) {
            rc = 0;
            break;
        }

        for (i = 0; i < n; i++) {
            recs[i].proc = current_proc;
            recs[i].next = NULL;
            wq_poll_add(wqs[i], &recs[i]);
        }
        if (has_deadline) {
            current_proc->wakeup_tick = deadline ? deadline : 1;
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

/** The poll core over the `nfds` entries of `fds`, at most POLL_MAX_FDS,
 * with select's rule of readiness when `sel` (`poll_scan`); returns as
 * `poll_wait` does. A wait whose scan found more queues than it had records
 * for begins again with a record for each. */
static int do_poll(struct kernel_pollfd *fds, uint32_t nfds, int has_deadline, uint32_t deadline,
                   int sel)
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
        rc = poll_wait(scan_pollfds, &p, p.room, has_deadline, deadline);
    } while (rc > 0 && p.grow != 0);
    return rc;
}

/**
 * The tick at which a wait of `ticks` whole ticks and `us` microseconds ends:
 * the first tick at or after its end, measured from where in the tick in
 * progress the call is (`rtc_tick_phase`), so a nonzero wait is never shorter
 * than asked and at most a tick longer. The callers clamp `ticks` at
 * 0x7FFFFFFE, and the deadline is at most 0x7FFFFFFF ticks ahead, so the
 * signed difference `deadline - kernel_ticks` stays valid.
 */
static uint32_t tick_deadline(uint32_t ticks, uint32_t us)
{
    uint32_t now, phase_ns;

    rtc_tick_phase(&now, &phase_ns);
    if (ticks == 0 && us == 0) {
        return now;
    }
    ticks += (phase_ns / 1000u + us + 9999u) / 10000u;
    return now + (ticks > 0x7FFFFFFFu ? 0x7FFFFFFFu : ticks);
}

uint32_t poll_ms_deadline(int timeout_ms, int *has_deadline)
{
    uint32_t ticks;

    if (timeout_ms < 0) {
        *has_deadline = 0;
        return 0;
    }
    *has_deadline = 1;
    ticks = (uint32_t)timeout_ms / 10u;
    return tick_deadline(ticks, (uint32_t)timeout_ms % 10u * 1000u);
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
 * The deadline tick of the timespec `ts`, the first tick at or after its end
 * (`tick_deadline`) and at most about 248 days ahead, with `*has_deadline`
 * 1; none for a NULL `ts`, with `*has_deadline` 0. The seconds are compared
 * as a long before they are narrowed, since on x86-64 a timeout of 2^32
 * seconds would read as 0 in 32 bits.
 */
static uint32_t ts_deadline(const struct poll_timespec *ts, int *has_deadline)
{
    uint32_t sec, ticks;

    if (!ts) {
        *has_deadline = 0;
        return 0;
    }
    *has_deadline = 1;
    sec = (unsigned long)ts->tv_sec > 21474836ul ? 21474836u : (uint32_t)ts->tv_sec;
    ticks = sec * 100u + (uint32_t)ts->tv_nsec / 10000000u;
    if (ticks > 0x7FFFFFFEu) {
        ticks = 0x7FFFFFFEu;
    }
    return tick_deadline(ticks, ((uint32_t)ts->tv_nsec % 10000000u + 999u) / 1000u);
}

/** The body of poll and ppoll: the `nfds` entries at `ufds` waited on until
 * the tick `deadline` when `has_deadline`, and their revents copied back.
 * No entries is a sleep until the deadline. Returns as `sys_poll` does. */
static int32_t poll_common(uintptr_t ufds, uint32_t nfds, int has_deadline, uint32_t deadline)
{
    struct kernel_pollfd *fds;
    uint32_t bytes;
    int ready;

    if (!current_proc) {
        return -ESRCH;
    }
    if (nfds > POLL_MAX_FDS) {
        return -EINVAL; /* more than a process can hold */
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

    ready = do_poll(fds, nfds, has_deadline, deadline, 0);
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
    int timeout_ms = (int)SC_ARG(params, 2);
    int has_deadline;
    uint32_t deadline = poll_ms_deadline(timeout_ms, &has_deadline);

    return poll_common(ufds, nfds, has_deadline, deadline);
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
    int has_deadline;
    uint32_t deadline;

    if (uts) {
        if (copy_from_user(&ts, (const void __user *)uts, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (!ts_valid(&ts)) {
            return -EINVAL;
        }
        deadline = ts_deadline(&ts, &has_deadline);
    } else {
        deadline = ts_deadline(NULL, &has_deadline);
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
    return poll_mask_done(poll_common(ufds, nfds, has_deadline, deadline));
}

/**
 * The body of the select calls: the descriptors below `nfds` set in the
 * user fd_sets at `ur`, `uw` and `ue`, each 0 for none, waited on until the
 * tick `deadline` when `has_deadline`, and the sets the caller gave written
 * back. Returns as `sys__newselect` does.
 *
 * A set bit whose descriptor is not open fails the call with -EBADF: the
 * scan reports such a descriptor as POLLNVAL, which no fd_set can show, and
 * counts it ready, so the wait ends at once.
 */
static int32_t select_common(int nfds, uintptr_t ur, uintptr_t uw, uintptr_t ue, int has_deadline,
                             uint32_t deadline)
{
    struct kernel_pollfd *fds;
    uint32_t rset[FDSET_WORDS], wset[FDSET_WORDS], eset[FDSET_WORDS];
    uint32_t orset[FDSET_WORDS], owset[FDSET_WORDS], oeset[FDSET_WORDS];
    uint32_t setbytes;
    int npoll, ready;

    if (!current_proc) {
        return -ESRCH;
    }
    if (nfds < 0 || nfds > FDSET_BITS) {
        return -EINVAL; /* nfds is bounded by the fd_set width (FD_SETSIZE = 1024) */
    }
    setbytes = (uint32_t)FDSET_WORDS * (uint32_t)sizeof(uint32_t);

    k_memset(rset, 0, setbytes);
    k_memset(wset, 0, setbytes);
    k_memset(eset, 0, setbytes);
    if (ur && copy_from_user(rset, (const void __user *)ur, setbytes) != 0) {
        return -EFAULT;
    }
    if (uw && copy_from_user(wset, (const void __user *)uw, setbytes) != 0) {
        return -EFAULT;
    }
    if (ue && copy_from_user(eset, (const void __user *)ue, setbytes) != 0) {
        return -EFAULT;
    }

    fds = kmalloc(sizeof(*fds) * (size_t)(nfds ? nfds : 1));
    if (!fds) {
        return -ENOMEM;
    }
    npoll = fdsets_to_pollfds((uint32_t)nfds, rset, wset, eset, fds, nfds);
    if (npoll < 0) {
        kfree(fds);
        return -EBADF; /* more selected descriptors than `fds` holds */
    }

    ready = do_poll(fds, (uint32_t)npoll, has_deadline, deadline, 1);
    if (ready < 0) {
        kfree(fds);
        return ready;
    }

    {
        int i;
        for (i = 0; i < npoll; i++) {
            if (fds[i].revents & POLLNVAL) {
                kfree(fds);
                return -EBADF;
            }
        }
    }

    k_memset(orset, 0, setbytes);
    k_memset(owset, 0, setbytes);
    k_memset(oeset, 0, setbytes);
    ready = pollfds_to_fdsets(fds, npoll, orset, owset, oeset);
    kfree(fds);
    if (ur && copy_to_user((void __user *)ur, orset, setbytes) != 0) {
        return -EFAULT;
    }
    if (uw && copy_to_user((void __user *)uw, owset, setbytes) != 0) {
        return -EFAULT;
    }
    if (ue && copy_to_user((void __user *)ue, oeset, setbytes) != 0) {
        return -EFAULT;
    }
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
    int has_deadline = 0;
    uint32_t deadline = 0;

    if (utv) {
        if (copy_from_user(&tv, (const void __user *)utv, sizeof(tv)) != 0) {
            return -EFAULT;
        }
        if (!tv_valid(&tv)) {
            return -EINVAL;
        }
        deadline = poll_ms_deadline(tv_to_ms(&tv), &has_deadline);
    } else {
        deadline = poll_ms_deadline(-1, &has_deadline);
    }
    return select_common(nfds, ur, uw, ue, has_deadline, deadline);
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
        int has_deadline = 0;
        uint32_t deadline = 0;
        if (a[4]) {
            if (copy_from_user(&tv, (const void __user *)a[4], sizeof(tv)) != 0) {
                return -EFAULT;
            }
            if (!tv_valid(&tv)) {
                return -EINVAL;
            }
            deadline = poll_ms_deadline(tv_to_ms(&tv), &has_deadline);
        } else {
            deadline = poll_ms_deadline(-1, &has_deadline);
        }
        return select_common((int)a[0], a[1], a[2], a[3], has_deadline, deadline);
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
    int has_deadline;
    uint32_t deadline;

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
        deadline = ts_deadline(&ts, &has_deadline);
    } else {
        deadline = ts_deadline(NULL, &has_deadline);
    }
    {
        int32_t rc = poll_mask_set(sigarg.mask, USER_LEN32(sigarg.size));

        if (rc != 0) {
            return rc;
        }
    }
    return poll_mask_done(select_common(nfds, ur, uw, ue, has_deadline, deadline));
}
