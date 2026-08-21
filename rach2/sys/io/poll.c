/*
 * poll.c - I/O multiplexing (poll/ppoll/select/_newselect/pselect6).
 *
 * The poll core scans the polled descriptors for readiness; if none is ready
 * and the timeout has not expired, it parks the process on EVERY polled
 * object's wait queue at once (wq_poll_add), yields, and on any wake re-scans.
 * This is the lost-wakeup-safe discipline generalized to many queues. Each
 * backing answers readiness through its file_ops poll op (pipes, ttys, ptys)
 * or, for the enum-dispatched descriptor types, a small per-type check here.
 *
 * select() is layered on poll(): the fd_set bitmaps are translated to/from a
 * pollfd array (pure logic in poll_select.c, host-tested), then the same
 * core runs. The i386 family
 * select(82)/_newselect(142)/poll(168)/pselect6(308)/ ppoll(309) all funnel
 * here so whichever number musl emits works.
 */

#include "poll.h"
#include "poll_select.h"
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
#include "../network/socket/socket.h"

/* At most this many descriptors can be polled at once: a process has at most
 * MAX_FDS open, so this also caps the per-call stack arrays. */
#define POLL_MAX_FDS MAX_FDS

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
 */
static short fd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    *wq = NULL;

    if (e->ops && e->ops->poll) {
        return e->ops->poll(e, events, wq);
    }
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        return socket_poll(e->sock, events, wq);
    }
    if (e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) {
        /* A regular file/directory never blocks: always readable, and
         * writable. */
        return (short)(events & (POLLIN | POLLOUT));
    }
    if (e->type == FD_TYPE_CONSOLE) {
        /* The console answers through its tty, but masked to what THIS
         * descriptor can do: only a readable fd queries (and drains) input;
         * only a writable fd reports POLLOUT. So poll(stdout, POLLOUT) never
         * drains serial input or claims the console foreground. */
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

/**
 * Scan all pollfds once.
 *
 * Fills revents for each entry, records each one's wait queue in `wqs[]`,
 * and returns the number that are ready. `sel` selects the readiness rule:
 * poll() counts any non-zero revents, while select() counts an fd only on a
 * revent that projects onto a direction it was selected in (so a write- or
 * except-only fd reporting only POLLHUP is not "ready", matching the fd_set
 * projection and not busy-spinning). Called with interrupts off so the
 * readiness reads and the enqueue that follows cannot be split by a waker.
 */
static int poll_scan(struct kernel_pollfd *fds, uint32_t nfds, struct wait_queue **wqs, int sel)
{
    uint32_t i;
    int ready = 0;

    for (i = 0; i < nfds; i++) {
        fd_entry_t *e;
        short revents;

        wqs[i] = NULL;
        if (fds[i].fd < 0) {
            fds[i].revents = 0; /* a negative fd is skipped (POSIX) */
            continue;
        }
        e = fd_get(&current_proc->fds, fds[i].fd);
        if (!e) {
            fds[i].revents = POLLNVAL; /* an invalid fd is reported, not waited on */
            ready++;
            continue;
        }
        revents = fd_poll(e, (short)(fds[i].events | POLLERR | POLLHUP), &wqs[i]);
        revents &= (short)(fds[i].events | POLLERR | POLLHUP | POLLNVAL);
        fds[i].revents = revents;
        if (sel ? pollfd_select_ready(fds[i].events, revents) : (revents != 0)) {
            ready++;
        }
    }
    return ready;
}

/**
 * The poll core: block until at least one of `fds` is ready, a signal arrives,
 * or the deadline passes.
 *
 * `has_deadline` == 0 means wait forever. nfds is already bounded to
 * POLL_MAX_FDS by the callers.
 *
 * `sel` selects the select() per-direction readiness rule over poll()'s.
 *
 * @return the ready count (>= 0), or -EINTR.
 */
static int do_poll(struct kernel_pollfd *fds, uint32_t nfds, int has_deadline, uint32_t deadline,
                   int sel)
{
    struct wait_queue *wqs[POLL_MAX_FDS];
    uint32_t flags;
    int rc;
    uint32_t i;

    flags = irq_save();
    for (;;) {
        int ready = poll_scan(fds, nfds, wqs, sel);
        if (ready > 0) {
            rc = ready;
            break;
        }
        if (signal_pending()) {
            rc = -EINTR;
            break;
        }
        if (has_deadline && (int32_t)(deadline - kernel_ticks) <= 0) {
            rc = 0; /* timed out */
            break;
        }

        /* Park on every polled object's wait queue, then yield. Done under the
         * same cli as the scan, so a readiness change cannot be lost. A finite
         * timeout also arms the timer wake (the nanosleep mechanism): with
         * another process runnable, schedule() switches away and never returns
         * here on its own, and a poll on no ready queue (nfds == 0) or one
         * whose objects never fire would otherwise sleep past its deadline
         * forever. proc_wake_sleepers makes us READY at the deadline. A
         * computed tick of 0 maps to 1, the value 0 being the "not sleeping"
         * sentinel. */
        for (i = 0; i < nfds; i++) {
            wq_poll_add(wqs[i], current_proc);
        }
        if (has_deadline) {
            current_proc->wakeup_tick = deadline ? deadline : 1;
        }
        current_proc->state = PROC_BLOCKED;
        irq_restore(flags);

        schedule();

        /* Leave every queue and re-arm. If nothing woke us and nothing else ran
         * (still BLOCKED), idle until the next interrupt (a wake or the tick
         * that advances the deadline). sti;hlt;cli is atomic from IF=0. */
        flags = irq_save();
        for (i = 0; i < nfds; i++) {
            wq_poll_remove(wqs[i], current_proc);
        }
        if (current_proc->state == PROC_BLOCKED) {
            __asm__ __volatile__("sti; hlt; cli");
        }
        current_proc->state = PROC_RUNNING;
    }

    /* On a break we still hold the loop's cli but may be enqueued (broke right
     * after a scan that ran before any sleep this iteration, in which case we
     * never enqueued, so the removes are no-ops). Belt-and-suspenders dequeue.
     */
    current_proc->wakeup_tick = 0; /* disarm the timer wake */
    for (i = 0; i < nfds; i++) {
        wq_poll_remove(wqs[i], current_proc);
    }
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    return rc;
}

/**
 * Convert a poll(2) millisecond timeout to a deadline. `*has_deadline` is 0
 * for an infinite wait (timeout_ms < 0).
 */
static uint32_t ms_deadline(int timeout_ms, int *has_deadline)
{
    uint32_t ticks;

    if (timeout_ms < 0) {
        *has_deadline = 0;
        return 0;
    }
    *has_deadline = 1;
    /* 10 ms per tick, rounded up; clamp absurd values to keep the signed delta
     * (deadline - kernel_ticks) valid. */
    ticks = ((uint32_t)timeout_ms + 9u) / 10u;
    if (ticks > 0x7FFFFFFFu) {
        ticks = 0x7FFFFFFFu;
    }
    return kernel_ticks + ticks;
}

/**
 * Validate a user timeval: seconds non-negative and tv_usec in [0, 1000000).
 *
 * select/poll reject anything else with -EINVAL rather than silently
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
 * Convert a timeval to a millisecond timeout for ms_deadline.
 *
 * Saturating: tv_sec is clamped so tv_sec*1000 cannot overflow the signed
 * result (INT_MAX ms is ~24 days, which for a select() timeout is
 * effectively forever), and a negative tv_sec degrades to a non-blocking
 * poll rather than an accidental infinite wait. A non-NULL timeval thus
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
    if (ticks > 0x7FFFFFFFu) {
        ticks = 0x7FFFFFFFu;
    }
    return kernel_ticks + ticks;
}

/** Shared poll(2) implementation: ufds/nfds plus an already-computed
 * deadline. */
static int32_t poll_common(uint32_t ufds, uint32_t nfds, int has_deadline, uint32_t deadline)
{
    struct kernel_pollfd fds[POLL_MAX_FDS];
    uint32_t bytes;
    int ready;

    if (!current_proc) {
        return -ESRCH;
    }
    if (nfds > POLL_MAX_FDS) {
        return -EINVAL; /* a process has at most MAX_FDS descriptors */
    }
    /* nfds == 0 is valid: a pure sleep until the deadline (do_poll finds
     * nothing ready, parks on no queues, and waits out the timeout). */
    bytes = nfds * (uint32_t)sizeof(struct kernel_pollfd);
    if (nfds > 0 && copy_from_user(fds, (const void __user *)ufds, bytes) != 0) {
        return -EFAULT;
    }

    ready = do_poll(fds, nfds, has_deadline, deadline, 0);
    if (ready < 0) {
        return ready;
    }
    if (nfds > 0 && copy_to_user((void __user *)ufds, fds, bytes) != 0) {
        return -EFAULT;
    }
    return ready;
}

int32_t sys_poll(struct isr_params *params)
{
    uint32_t ufds = params->ebx;
    uint32_t nfds = params->ecx;
    int timeout_ms = (int)params->edx;
    int has_deadline;
    uint32_t deadline = ms_deadline(timeout_ms, &has_deadline);

    return poll_common(ufds, nfds, has_deadline, deadline);
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
    /* The sigmask argument (esi) is accepted but not applied: the kernel has no
     * pending-mask swap primitive, and busybox/dropbear pass NULL or their
     * normal mask here. Documented limitation. */
    return poll_common(ufds, nfds, has_deadline, deadline);
}

/** Shared select() core over the three fd_sets. nfds is the highest fd + 1. */
static int32_t select_common(int nfds, uint32_t ur, uint32_t uw, uint32_t ue,
                             int has_deadline, uint32_t deadline)
{
    struct kernel_pollfd fds[POLL_MAX_FDS];
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
    /* A large nfds is fine as long as no bit is set for an fd the kernel cannot
     * have: we scan the whole [0, nfds) range and only fail (below, -EBADF) if
     * a selected bit names fd >= MAX_FDS, never a silent truncation. */
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

    /* fd_sets -> pollfds (pure, host-tested). */
    npoll = fdsets_to_pollfds((uint32_t)nfds, rset, wset, eset, fds, POLL_MAX_FDS);
    if (npoll < 0) {
        return -EBADF; /* a selected bit named an fd the kernel can never have */
    }

    ready = do_poll(fds, (uint32_t)npoll, has_deadline, deadline, 1);
    if (ready < 0) {
        return ready;
    }

    /* select() fails wholesale if any selected fd is invalid. poll_scan flags
     * such an fd POLLNVAL (an in-range bit whose descriptor was closed), which
     * has no fd_set projection, so surface it as -EBADF; never a silent
     * success. An invalid fd always makes the scan "ready", so this never masks
     * a real block. */
    {
        int i;
        for (i = 0; i < npoll; i++) {
            if (fds[i].revents & POLLNVAL) {
                return -EBADF;
            }
        }
    }

    /* pollfds -> fd_sets (pure), then write back only the sets the caller
     * gave. */
    k_memset(orset, 0, setbytes);
    k_memset(owset, 0, setbytes);
    k_memset(oeset, 0, setbytes);
    ready = pollfds_to_fdsets(fds, npoll, orset, owset, oeset);
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
        deadline = ms_deadline(tv_to_ms(&tv), &has_deadline);
    } else {
        deadline = ms_deadline(-1, &has_deadline); /* NULL timeout == wait forever */
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
            deadline = ms_deadline(tv_to_ms(&tv), &has_deadline);
        } else {
            deadline = ms_deadline(-1, &has_deadline);
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

    /* pselect6's 6th arg (the sigmask) is passed on the stack; like ppoll, it
     * is accepted but not applied (documented). */
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
    return select_common(nfds, ur, uw, ue, has_deadline, deadline);
}
