/*
 * poll.c - I/O multiplexing (poll/ppoll/select/_newselect/pselect6).
 *
 * The poll core scans the polled descriptors for readiness; if none is ready
 * and the timeout has not expired, it parks the process on EVERY polled
 * object's wait queue at once (wq_poll_add), yields, and on any wake re-scans.
 * This is the lost-wakeup-safe discipline generalized to many queues. Each
 * backing answers readiness through its file_ops poll op (pipes, ptys, device
 * and /proc files) or, for the enum-dispatched descriptor types (the console,
 * sockets), a small per-type check here.
 *
 * select() is layered on poll(): the fd_set bitmaps are translated to/from a
 * pollfd array (pure logic in poll_select.c, host-tested), then the same
 * core runs. The i386 calls select(82), _newselect(142), poll(168),
 * pselect6(308) and ppoll(309) all reach this core, so whichever number musl
 * emits works.
 */

#include "poll.h"
#include "poll_select.h"
#include "epoll.h"
#include "../isr.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../proc/sched.h"
#include "../tty/tty.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../network/socket/socket.h"

/* At most this many descriptors can be polled at once: the most a process
 * may hold. The per-call arrays come from the heap, sized by the call. */
#define POLL_MAX_FDS FD_TABLE_LIMIT

extern volatile uint32_t kernel_ticks;

/* 32-bit timespec as the i386 kernel ABI passes it (ppoll/pselect6). */
struct poll_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* timeval (select). */
struct poll_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

static uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

/**
 * Compute the readiness of one descriptor.
 *
 * Returns the revents: a subset of `events` for POLLIN/POLLOUT, plus POLLHUP
 * and POLLERR which are always reported. Sets `*wq` to the wait queue whose
 * wake means "re-check me", or NULL if the descriptor is always ready.
 *
 * A regular file or directory never blocks. The console answers through its
 * tty, masked to what this descriptor can do: POLLIN, whose query drains
 * serial input, only for a readable descriptor, and POLLOUT only for a
 * writable one, so poll(stdout, POLLOUT) never drains serial input.
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
        if (e->flags & FD_FLAG_WRITE) {
            want = (short)(want | (events & POLLOUT));
        }
        return tty_poll(&console_tty, want, wq);
    }
    /* Framebuffer/mouse: reported ready (no wait queue). */
    return (short)(events & (POLLIN | POLLOUT));
}

/* A poll's or a select's descriptors, for the scan of `poll_wait`: the
 * queues it has records for, and the queues the last scan found, when they
 * were more with nothing ready, for the wait to begin again with. */
struct pollfd_scan {
    struct kernel_pollfd *fds;
    uint32_t nfds;
    int sel;
    uint32_t room;
    uint32_t grow;
};

/*
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
 * Scan all pollfds once.
 *
 * Fills revents for each entry, records the wait queues in `wqs[]`, and
 * returns the number that are ready. `sel` selects the readiness rule:
 * poll() counts any non-zero revents, while select() counts an fd only on a
 * revent that projects onto a direction it was selected in (so a write- or
 * except-only fd reporting only POLLHUP is not "ready", matching the fd_set
 * projection and not busy-spinning). Called with interrupts off so the
 * readiness reads and the enqueue that follows cannot be split by a waker.
 *
 * An epoll set has a queue for each descriptor it watches, so the queues
 * can be more than the records the wait has; with nothing ready, the scan
 * says so in `grow` and returns 1 to end the wait, which begins again with
 * a record for each.
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
            fds[i].revents = 0; /* a negative fd is skipped (POSIX) */
            continue;
        }
        e = fd_get(current_proc->fds, fds[i].fd);
        if (!e || (e->flags & FD_FLAG_PATH)) {
            fds[i].revents = POLLNVAL; /* an invalid or O_PATH fd is reported, not waited on */
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
 * The wait of the poll core: block until `scan` finds an object ready, a
 * signal arrives, or the deadline passes.
 *
 * `has_deadline` == 0 means wait forever. `scan` names the wait queue of each
 * of at most `n` objects in the array it is given and returns how many are
 * ready; it runs with interrupts off, so the readiness it reads and the
 * enqueue that follows cannot be split by a waker.
 *
 * The process is parked on every object's wait queue under the same cli as
 * the scan, so a readiness change cannot be lost. A finite timeout also sets
 * the timer wake, `wakeup_tick`, which proc_wake_sleepers acts on at the
 * deadline: with another process runnable, schedule() does not return here on
 * its own, and a wait on no queue or on objects that never fire would
 * otherwise sleep past its deadline. A computed tick of 0 is stored as 1,
 * since 0 means not sleeping. A process still blocked when schedule()
 * returns, which nothing woke and nothing else could run, idles until the
 * next interrupt, still on every queue so a waker can find it, and leaves the
 * queues after. On return the process is on no queue and its timer wake is
 * cleared.
 *
 * @return the ready count, 0 once the deadline has passed, -EINTR, or
 *         -ENOMEM.
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
            rc = 0; /* timed out */
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

    current_proc->wakeup_tick = 0; /* disarm the timer wake */
    for (i = 0; i < n; i++) {
        wq_poll_remove(wqs[i], &recs[i]);
    }
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    kfree(wqs);
    kfree(recs);
    return rc;
}

static int scan_pollfds(void *ctx, struct wait_queue **wqs)
{
    return poll_scan((struct pollfd_scan *)ctx, wqs);
}

/* The poll core over `fds`; `sel` selects the select() per-direction
 * readiness rule over poll()'s. nfds is already bounded to POLL_MAX_FDS by
 * the callers. A wait whose scan found more queues than it had records for
 * begins again with a record for each (`poll_scan`). */
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
 * The tick at which a wait of `ticks` whole ticks ends. The tick in progress
 * is not counted, so a nonzero wait is never shorter than asked. The
 * callers clamp `ticks` at 0x7FFFFFFE, so the signed difference
 * `deadline - kernel_ticks` stays valid with that tick added.
 */
static uint32_t tick_deadline(uint32_t ticks)
{
    return kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);
}

/**
 * Convert a poll(2) millisecond timeout to a deadline in 10 ms ticks, rounded
 * up. `*has_deadline` is 0 for an infinite wait (timeout_ms < 0).
 */
uint32_t poll_ms_deadline(int timeout_ms, int *has_deadline)
{
    uint32_t ticks;

    if (timeout_ms < 0) {
        *has_deadline = 0;
        return 0;
    }
    *has_deadline = 1;
    ticks = ((uint32_t)timeout_ms + 9u) / 10u;
    if (ticks > 0x7FFFFFFEu) {
        ticks = 0x7FFFFFFEu;
    }
    return tick_deadline(ticks);
}

/**
 * Validate a user timeval: seconds non-negative and tv_usec in [0, 1000000).
 *
 * select rejects anything else with -EINVAL, not silently
 * clamping, and the in-range usec also keeps the conversions below free of
 * signed overflow. musl always passes normalized values; a raw int-0x80
 * caller might not.
 */
static int tv_valid(const struct poll_timeval *tv)
{
    return tv->tv_sec >= 0 && tv->tv_usec >= 0 && tv->tv_usec < 1000000;
}

static int ts_valid(const struct poll_timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

/**
 * Convert a timeval to a millisecond timeout for poll_ms_deadline.
 *
 * Saturating: tv_sec is clamped so tv_sec*1000 cannot overflow the signed
 * result (INT_MAX ms is ~24 days, which for a select() timeout is
 * effectively forever), and a negative tv_sec degrades to a non-blocking
 * poll, not an accidental infinite wait. A non-NULL timeval thus
 * always yields a finite deadline; only a NULL one waits forever.
 */
static int tv_to_ms(const struct poll_timeval *tv)
{
    if (tv->tv_sec < 0) {
        return 0;
    }
    if (tv->tv_sec > 2147482) {
        return 0x7FFFFFFF;
    }
    return tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
}

/** Convert a timespec to a deadline (NULL == infinite). */
static uint32_t ts_deadline(const struct poll_timespec *ts, int *has_deadline)
{
    uint32_t sec, nsec_ticks, ticks;

    if (!ts) {
        *has_deadline = 0;
        return 0;
    }
    *has_deadline = 1;
    sec = (uint32_t)ts->tv_sec;
    nsec_ticks = ((uint32_t)ts->tv_nsec + 9999999u) / 10000000u;
    if (sec > 21474836u) {
        sec = 21474836u;
    }
    ticks = sec * 100u + nsec_ticks;
    if (ticks > 0x7FFFFFFEu) {
        ticks = 0x7FFFFFFEu;
    }
    return tick_deadline(ticks);
}

/** Shared poll(2) implementation: ufds/nfds plus an already-computed
 * deadline. An nfds of 0 is a sleep until the deadline. */
static int32_t poll_common(uint32_t ufds, uint32_t nfds, int has_deadline, uint32_t deadline)
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

int32_t sys_poll(struct isr_params *params)
{
    uint32_t ufds = params->ebx;
    uint32_t nfds = params->ecx;
    int timeout_ms = (int)params->edx;
    int has_deadline;
    uint32_t deadline = poll_ms_deadline(timeout_ms, &has_deadline);

    return poll_common(ufds, nfds, has_deadline, deadline);
}

/**
 * Install the signal mask a ppoll or pselect6 waits under.
 *
 * `umask` is a user signal set of `size` bytes, or 0 for none. The mask it
 * replaces becomes the thread's saved mask: `deliver_signals` puts it back
 * after the handler a signal runs for this wait, or at once when none runs,
 * and a wait that did not end for a signal puts it back through
 * `poll_mask_done`. Both masks are set through `signal_set_blocked`, which
 * leaves a signal of the process that a mask blocks to another thread.
 * Returns 0, or -EINVAL or -EFAULT.
 */
int32_t poll_mask_set(uint32_t umask, uint32_t size)
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

/* The mask back, unless the wait ended for a signal, whose delivery puts it
 * back after its handler. */
int32_t poll_mask_done(int32_t ret)
{
    if (current_proc->sigmask_saved && ret != -EINTR) {
        current_proc->sigmask_saved = 0;
        signal_set_blocked(current_proc->saved_sigmask);
    }
    return ret;
}

int32_t sys_ppoll(struct isr_params *params)
{
    uint32_t ufds = params->ebx;
    uint32_t nfds = params->ecx;
    uint32_t uts = params->edx;
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
        int32_t rc = poll_mask_set(params->esi, params->edi);

        if (rc != 0) {
            return rc;
        }
    }
    return poll_mask_done(poll_common(ufds, nfds, has_deadline, deadline));
}

/**
 * Shared select() core over the three fd_sets. nfds is the highest fd + 1.
 *
 * The whole [0, nfds) range is scanned, and a selected bit whose descriptor
 * is not open fails the call with -EBADF: the scan reports such a descriptor
 * as POLLNVAL, which no fd_set can show, and counts it ready, so the check
 * never hides a real wait.
 */
static int32_t select_common(int nfds, uint32_t ur, uint32_t uw, uint32_t ue, int has_deadline,
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

    /* fd_sets -> pollfds: one per selected descriptor, `nfds` at most. */
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

    /* pollfds -> fd_sets; only the sets the caller gave are written back. */
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

int32_t sys__newselect(struct isr_params *params)
{
    int nfds = (int)params->ebx;
    uint32_t ur = params->ecx;
    uint32_t uw = params->edx;
    uint32_t ue = params->esi;
    uint32_t utv = params->edi;
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
        deadline = poll_ms_deadline(-1, &has_deadline); /* NULL timeout == wait forever */
    }
    return select_common(nfds, ur, uw, ue, has_deadline, deadline);
}

int32_t sys_select(struct isr_params *params)
{
    /* The old i386 select(82) packs its 5 args behind one pointer (ebx). */
    uint32_t argp = params->ebx;
    uint32_t a[5];

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

int32_t sys_pselect6(struct isr_params *params)
{
    int nfds = (int)params->ebx;
    uint32_t ur = params->ecx;
    uint32_t uw = params->edx;
    uint32_t ue = params->esi;
    uint32_t uts = params->edi;
    struct poll_timespec ts;
    int has_deadline;
    uint32_t deadline;

    uint32_t sigarg[2] = {0, 0};

    if (!current_proc) {
        return -ESRCH;
    }
    /* EBP points at the mask's address and its size in bytes. */
    if (params->ebp &&
        copy_from_user(sigarg, (const void __user *)params->ebp, sizeof(sigarg)) != 0) {
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
        int32_t rc = poll_mask_set(sigarg[0], sigarg[1]);

        if (rc != 0) {
            return rc;
        }
    }
    return poll_mask_done(select_common(nfds, ur, uw, ue, has_deadline, deadline));
}
