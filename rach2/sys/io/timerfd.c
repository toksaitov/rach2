/*
 * timerfd.c: timers read through descriptors, and POSIX timers, which share
 * their timer, as timerfd.h describes them.
 */

#include "timerfd.h"

#include "anon.h"
#include "../irq.h"
#include "../rtc.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/tgroup.h"
#include "../lib/ksignal.h"
#include "../proc/wait_queue.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"

#define CLOCK_REALTIME       0
#define CLOCK_MONOTONIC      1
#define CLOCK_BOOTTIME       7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9

#define TFD_TIMER_ABSTIME       1
#define TFD_TIMER_CANCEL_ON_SET 2
#define TFD_NONBLOCK            04000
#define TFD_CLOEXEC             02000000

#define POLLIN 0x001

#define TIMER_ABSTIME   1
#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD_ID 4

/* Format: the kernel's ticks in a second, and nanoseconds in a tick. The
 * kernel has no 64-bit division, so a time is kept as whole seconds and
 * ticks below a second, and never as one count of ticks or nanoseconds. */
#define TICKS_PER_SEC 100u
#define NS_PER_TICK   10000000
#define NS_PER_SEC    1000000000
/*
 * Policy: a span, the most seconds a timer waits in one piece, which keeps
 * the signed comparison of ticks in `timerfd_tick` valid; a longer time is
 * the part below a span, then whole spans. And the most seconds a setting
 * keeps, about 136 years: a longer time is taken as that, as Linux takes
 * one past its own limit, about 292 years.
 */
#define SPAN_SEC      10737417u
#define TIME_MAX      0xFFFFFFFFu

extern volatile uint32_t kernel_ticks;

/* A timer: a timerfd's, read through its descriptor (`anon` and `wq`), or a
 * POSIX timer's, which `tg` names and whose expirations post a signal. A
 * POSIX timer on a clock of processor time counts the ticks charged to
 * `cpu_tg`'s threads, or to its thread `cpu_tid` alone, user ticks alone for
 * `cpu_user`: `due` is then the ticks left, and `spans` is unused. */
struct ktimer {
    struct anon_obj anon;
    struct ktimer *next; /* the list of armed timers the tick walks */
    int clock;
    int armed;
    uint32_t due;   /* the tick the wait in progress ends */
    uint32_t spans; /* whole spans to wait after it, before the expiration */
    uint32_t isec;  /* the interval, 0 and 0 for none: whole seconds, */
    uint32_t isub;  /* and ticks below a second */
    uint64_t count; /* expirations not yet read, or not yet signalled */
    wait_queue_t wq;
    struct tgroup *tg;       /* a POSIX timer's process, or NULL */
    struct ktimer *in_group; /* the process's next timer */
    int id;
    int notify; /* SIGEV_SIGNAL, SIGEV_NONE or SIGEV_THREAD_ID */
    int signo;
    uintptr_t value;
    uint32_t tid;          /* the thread SIGEV_THREAD_ID names */
    uint32_t overrun;      /* the overrun of the last signal posted */
    struct tgroup *cpu_tg; /* held: the process whose time it counts, or NULL */
    uint32_t cpu_tid;
    int cpu_user;
};

/* A setting as the call gives and takes it, whichever width of time: the
 * interval, then the first expiration. */
struct setting {
    int64_t isec;
    int32_t insec;
    int64_t vsec;
    int32_t vnsec;
};

/*
 * The armed timers, and those of processor time, which `ptimer_cpu_tick`
 * counts. Their lock covers the two lists, each timer's count and arming,
 * and each process's list of POSIX timers, against the ticks of every
 * processor; it is taken with interrupts off, after the big lock, and
 * before the locks of the wait queues and the run queues a firing takes.
 */
static struct spinlock timer_lock = SPINLOCK_INIT;
static struct ktimer *armed;
static struct ktimer *cpu_armed;

static void disarm(struct ktimer *t)
{
    struct ktimer **link = t->cpu_tg ? &cpu_armed : &armed;

    while (*link && *link != t) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = t->next;
    }
    t->armed = 0;
}

/* Set `t` to expire `sec` seconds and `sub` ticks after the tick `from`:
 * the part below a whole span first, then the spans. */
static void wait_from(struct ktimer *t, uint32_t from, uint32_t sec, uint32_t sub)
{
    t->spans = sec / SPAN_SEC;
    t->due = from + (sec % SPAN_SEC) * TICKS_PER_SEC + sub;
}

static void ptimer_fire(struct ktimer *t);
static void ptimer_free(struct ktimer **link);

/* An expiration of `t`, due by the tick `now`: counted, with those of an
 * interval missed since, and the timer armed again by its interval or
 * disarmed; then its readers woken, or its signal posted. */
static void expire(struct ktimer *t, uint32_t now)
{
    if (t->isec >= SPAN_SEC) {
        t->count++;
        wait_from(t, t->due, t->isec, t->isub);
    } else if (t->isec != 0 || t->isub != 0) {
        uint32_t interval = t->isec * TICKS_PER_SEC + t->isub;
        uint32_t late = (now - t->due) / interval;

        t->count += 1u + late;
        t->due += (late + 1u) * interval;
    } else {
        t->count++;
        disarm(t);
    }
    if (t->tg) {
        ptimer_fire(t);
    } else {
        wq_wake_all(&t->wq);
    }
}

/* `sec` seconds and `sub` ticks as ticks, at most 0xFFFFFFFF and at least
 * one, so that a timer of processor time armed for no time expires at the
 * next tick charged. */
static uint32_t cpu_ticks_of(uint32_t sec, uint32_t sub)
{
    if (sec >= 0xFFFFFFFFu / TICKS_PER_SEC) {
        return 0xFFFFFFFFu;
    }
    sec = sec * TICKS_PER_SEC + sub;
    return sec != 0 ? sec : 1u;
}

void ptimer_cpu_tick(const process_t *p, int user)
{
    struct ktimer *t;

    spin_lock(&timer_lock);
    t = cpu_armed;
    while (t) {
        struct ktimer *next = t->next;

        if (t->cpu_tg == p->tg && (t->cpu_tid == 0 || t->cpu_tid == p->pid) &&
            (user || !t->cpu_user) && --t->due == 0) {
            t->count++;
            if (t->isec != 0 || t->isub != 0) {
                t->due = cpu_ticks_of(t->isec, t->isub);
            } else {
                disarm(t);
            }
            ptimer_fire(t);
        }
        t = next;
    }
    spin_unlock(&timer_lock);
}

void timerfd_tick(uint32_t now)
{
    struct ktimer *t;

    spin_lock(&timer_lock);
    t = armed;
    while (t) {
        struct ktimer *next = t->next;

        while (t->armed && (int32_t)(now - t->due) >= 0) {
            if (t->spans != 0) {
                t->spans--;
                t->due += SPAN_SEC * TICKS_PER_SEC;
            } else {
                expire(t, now);
            }
        }
        t = next;
    }
    spin_unlock(&timer_lock);
}

/*
 * A time of `sec` seconds and `nsec` nanoseconds, below a second, as whole
 * seconds and ticks below a second, rounded up to a tick so that a timer
 * never expires early. A negative time is none, and a time past TIME_MAX
 * seconds is that.
 */
static void to_wait(int64_t sec, int32_t nsec, uint32_t *s, uint32_t *sub)
{
    *s = 0;
    *sub = 0;
    if (sec < 0) {
        return;
    }
    if (sec >= (int64_t)TIME_MAX) {
        *s = TIME_MAX;
        return;
    }
    *s = (uint32_t)sec;
    *sub = ((uint32_t)nsec + NS_PER_TICK - 1u) / NS_PER_TICK;
    if (*sub == TICKS_PER_SEC) {
        *s += 1u;
        *sub = 0;
    }
}

static int readable(void *arg)
{
    return ((struct ktimer *)arg)->count != 0;
}

/**
 * read(2) of a timerfd: the expirations since the last read, which it takes.
 *
 * The count is read and cleared under the timers' lock, since the timer
 * interrupt adds to it (`timerfd_tick`), and before the copy to the caller,
 * which can sleep in a page fault: an expiration that comes meanwhile is
 * kept for the next read, and a second reader finds none of the ones this
 * read took. A copy that fails loses the count, as in Linux.
 */
static int32_t timerfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct ktimer *t = (struct ktimer *)e->priv;
    uint64_t n;

    if (count < sizeof(n)) {
        return -EINVAL;
    }
    for (;;) {
        uint32_t irq = spin_lock_irqsave(&timer_lock);

        n = t->count;
        t->count = 0;
        spin_unlock_irqrestore(&timer_lock, irq);
        if (n != 0) {
            break;
        }
        if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&t->wq, readable, t) < 0) {
            return -EINTR;
        }
    }
    if (copy_to_user((void __user *)buf, &n, sizeof(n)) != 0) {
        return -EFAULT;
    }
    return (int32_t)sizeof(n);
}

static short timerfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct ktimer *t = (struct ktimer *)e->priv;

    if (wq) {
        *wq = &t->wq;
    }
    return (short)(t->count ? (events & POLLIN) : 0);
}

static void timerfd_close(fd_entry_t *e)
{
    struct ktimer *t = (struct ktimer *)e->priv;
    uint32_t flags = spin_lock_irqsave(&timer_lock);

    if (t->armed) {
        disarm(t);
    }
    spin_unlock_irqrestore(&timer_lock, flags);
    kfree(t);
    e->priv = NULL;
}

static const struct file_ops timerfd_ops = {timerfd_read, NULL, timerfd_poll, NULL, timerfd_close};

static struct ktimer *timer_of(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return (e && e->ops == &timerfd_ops) ? (struct ktimer *)e->priv : NULL;
}

long sys_timerfd_create(struct isr_params *params)
{
    int clock = (int)SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 1);
    struct ktimer *t;
    fd_entry_t *e;
    int fd;

    if (clock == CLOCK_REALTIME_ALARM || clock == CLOCK_BOOTTIME_ALARM) {
        return -EPERM;
    }
    if ((clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != CLOCK_BOOTTIME) ||
        (flags & ~(uint32_t)(TFD_NONBLOCK | TFD_CLOEXEC))) {
        return -EINVAL;
    }
    t = kzalloc(sizeof(*t));
    if (!t) {
        return -ENOMEM;
    }
    t->anon.name = "[timerfd]";
    t->clock = clock;
    wait_queue_init(&t->wq);
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(t);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &timerfd_ops;
    e->priv = t;
    e->flags = FD_FLAG_READ | ((flags & TFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (flags & TFD_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    return fd;
}

/* The timer's setting now: the time to its next expiration and its
 * interval, both zero when it is disarmed. It is read under the timers'
 * lock, since the tick moves the timer. */
static void current_setting(const struct ktimer *t, struct setting *s)
{
    uint32_t irq = spin_lock_irqsave(&timer_lock);

    k_memset(s, 0, sizeof(*s));
    if (t->armed && t->cpu_tg) {
        s->vsec = t->due / TICKS_PER_SEC;
        s->vnsec = (int32_t)(t->due % TICKS_PER_SEC) * NS_PER_TICK;
        s->isec = t->isec;
        s->insec = (int32_t)t->isub * NS_PER_TICK;
    } else if (t->armed) {
        int32_t left = (int32_t)(t->due - rtc_ticks_now());

        s->vsec = (int64_t)t->spans * SPAN_SEC;
        if (left > 0) {
            s->vsec += (uint32_t)left / TICKS_PER_SEC;
            s->vnsec = (int32_t)((uint32_t)left % TICKS_PER_SEC) * NS_PER_TICK;
        } else if (s->vsec == 0) {
            s->vnsec = 1; /* due now: reported as not yet expired */
        }
        s->isec = t->isec;
        s->insec = (int32_t)t->isub * NS_PER_TICK;
    }
    spin_unlock_irqrestore(&timer_lock, irq);
}

/* Arm or disarm `t` from the setting `s`, which `flags` makes absolute. */
static int32_t arm(struct ktimer *t, int flags, const struct setting *s)
{
    int on = s->vsec != 0 || s->vnsec != 0;
    uint32_t sec, sub, isec, isub;
    uint32_t irq;

    if (s->vsec < 0 || s->vnsec < 0 || s->vnsec >= NS_PER_SEC) {
        return -EINVAL;
    }
    to_wait(s->vsec, s->vnsec, &sec, &sub);
    if (on && (flags & TFD_TIMER_ABSTIME)) {
        int64_t now;
        int32_t nsec;
        int32_t rc = clock_now(t->clock, &now, &nsec);

        if (rc != 0) {
            return rc;
        }
        now = s->vsec - now;
        nsec = s->vnsec - nsec;
        if (nsec < 0) {
            nsec += NS_PER_SEC;
            now--;
        }
        to_wait(now, nsec, &sec, &sub);
    }
    to_wait(s->isec, s->insec, &isec, &isub);
    irq = spin_lock_irqsave(&timer_lock);
    if (t->armed) {
        disarm(t);
    }
    t->count = 0;
    if (on && t->cpu_tg) {
        t->due = cpu_ticks_of(sec, sub);
        t->isec = isec;
        t->isub = isub;
        t->armed = 1;
        t->next = cpu_armed;
        cpu_armed = t;
    } else if (on) {
        wait_from(t, rtc_ticks_now() + 1u, sec, sub);
        t->isec = isec;
        t->isub = isub;
        t->armed = 1;
        t->next = armed;
        armed = t;
    }
    spin_unlock_irqrestore(&timer_lock, irq);
    return 0;
}

/* The two widths of struct itimerspec, the interval first: seconds and
 * nanoseconds of a long, 32 bits on i686 and 64 on x86-64, or of 64 bits,
 * the time64 calls' on i686. */
static int32_t get_setting(uintptr_t u, int wide, struct setting *s)
{
    if (wide) {
        int64_t v[4];

        if (copy_from_user(v, (const void __user *)u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v[1] < 0 || v[1] >= NS_PER_SEC || v[3] < 0 || v[3] >= NS_PER_SEC) {
            return -EINVAL;
        }
        s->isec = v[0];
        s->insec = (int32_t)v[1];
        s->vsec = v[2];
        s->vnsec = (int32_t)v[3];
    } else {
        long v[4];

        if (copy_from_user(v, (const void __user *)u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v[1] < 0 || v[1] >= NS_PER_SEC || v[3] < 0 || v[3] >= NS_PER_SEC) {
            return -EINVAL;
        }
        s->isec = v[0];
        s->insec = (int32_t)v[1];
        s->vsec = v[2];
        s->vnsec = (int32_t)v[3];
    }
    return s->isec < 0 || s->vsec < 0 ? -EINVAL : 0;
}

static int32_t put_setting(uintptr_t u, int wide, const struct setting *s)
{
    if (wide) {
        int64_t v[4];

        v[0] = s->isec;
        v[1] = s->insec;
        v[2] = s->vsec;
        v[3] = s->vnsec;
        return copy_to_user((void __user *)u, v, sizeof(v)) != 0 ? -EFAULT : 0;
    } else {
        long v[4];

        v[0] = (long)s->isec;
        v[1] = s->insec;
        v[2] = (long)s->vsec;
        v[3] = s->vnsec;
        return copy_to_user((void __user *)u, v, sizeof(v)) != 0 ? -EFAULT : 0;
    }
}

/* Arm `t` from the setting at `want`, which `flags` may make absolute, and
 * give the setting it had at `old` when that is not 0. */
static int32_t set_and_report(struct ktimer *t, int flags, uintptr_t want_u, uintptr_t old_u,
                              int wide)
{
    struct setting now, want;
    int32_t rc = get_setting(want_u, wide, &want);

    if (rc != 0) {
        return rc;
    }
    current_setting(t, &now);
    rc = arm(t, flags, &want);
    if (rc == 0 && old_u) {
        rc = put_setting(old_u, wide, &now);
    }
    return rc;
}

static int32_t settime(struct isr_params *params, int wide)
{
    struct ktimer *t = timer_of((int)SC_ARG(params, 0));
    int flags = (int)SC_ARG(params, 1);

    if (!fd_get(current_proc->fds, (int)SC_ARG(params, 0))) {
        return -EBADF;
    }
    if (!t || (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))) {
        return -EINVAL;
    }
    return set_and_report(t, flags, SC_ARG(params, 2), SC_ARG(params, 3), wide);
}

static int32_t gettime(struct isr_params *params, int wide)
{
    struct ktimer *t = timer_of((int)SC_ARG(params, 0));
    struct setting now;

    if (!fd_get(current_proc->fds, (int)SC_ARG(params, 0))) {
        return -EBADF;
    }
    if (!t) {
        return -EINVAL;
    }
    current_setting(t, &now);
    return put_setting(SC_ARG(params, 1), wide, &now);
}

long sys_timerfd_settime(struct isr_params *params)
{
    return settime(params, 0);
}

long sys_timerfd_settime64(struct isr_params *params)
{
    return settime(params, 1);
}

long sys_timerfd_gettime(struct isr_params *params)
{
    return gettime(params, 0);
}

long sys_timerfd_gettime64(struct isr_params *params)
{
    return gettime(params, 1);
}

/* The thread `tid` of the process `tg`, or NULL when it has none. */
static process_t *thread_of(struct tgroup *tg, uint32_t tid)
{
    process_t *p = get_process(tid);

    return p && p->tg == tg && p->state != PROC_ZOMBIE ? p : NULL;
}

/*
 * The signal of the POSIX timer `t`, whose expirations since its last
 * signal are its `count`: posted, with all but one as its overrun; or, while
 * its signal is still pending, added to that signal's overrun, which is what
 * a handler is given, as Linux reports the overrun at delivery. While a
 * signal of the same number from elsewhere is pending, the expirations wait
 * for the next. SIGEV_NONE posts nothing; a signal for a thread
 * SIGEV_THREAD_ID named that has ended goes to the process.
 */
static void ptimer_fire(struct ktimer *t)
{
    process_t *thread = t->notify == SIGEV_THREAD_ID ? thread_of(t->tg, t->tid) : NULL;
    uint64_t pending = thread ? thread->sig_pending : t->tg->pending;
    struct k_siginfo info;

    if (t->notify == SIGEV_NONE) {
        t->count = 0;
        return;
    }
    if (pending & KSIG_BIT(t->signo)) {
        struct k_siginfo *held = thread ? &thread->sig_info[t->signo] : &t->tg->info[t->signo];

        if (held->code == SI_TIMER && held->pid == (uint32_t)t->id) {
            held->uid =
                held->uid + t->count > 0x7FFFFFFFu ? 0x7FFFFFFFu : held->uid + (uint32_t)t->count;
            t->overrun = held->uid;
            t->count = 0;
        }
        return;
    }
    t->overrun = t->count - 1u > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)(t->count - 1u);
    t->count = 0;
    k_memset(&info, 0, sizeof(info));
    info.code = SI_TIMER;
    info.pid = (uint32_t)t->id; /* si_timerid, where a child's pid is */
    info.uid = t->overrun;      /* si_overrun */
    info.value = t->value;      /* si_value */
    info.status = (int32_t)t->value;
    if (thread) {
        signal_post_thread(thread, t->signo, &info);
    } else {
        signal_post_info(t->tg->leader, t->signo, &info);
    }
}

/* The calling process's POSIX timer `id`, or NULL. */
static struct ktimer *ptimer_of(int id)
{
    struct ktimer *t = current_proc->tg->timers;

    while (t && t->id != id) {
        t = t->in_group;
    }
    return t;
}

/* Take the target of the clock `clock` into `t`: none for real time and time
 * since boot, and for a clock of processor time the process it counts,
 * held, its thread and whether it counts user time alone. 0, or -EINVAL
 * for a clock `clock_cpu_target` refuses. */
static int32_t ptimer_clock(struct ktimer *t, int clock)
{
    if (clock == CLOCK_REALTIME || clock == CLOCK_MONOTONIC || clock == CLOCK_BOOTTIME) {
        return 0;
    }
    return clock_cpu_target(clock, &t->cpu_tg, &t->cpu_tid, &t->cpu_user);
}

/**
 * timer_create(2): a POSIX timer on the clock its first argument names,
 * notified as the `struct sigevent` at the second says, or by SIGALRM with its
 * id as the value when the second is 0, whose id is stored at the third: the
 * lowest the process does not have. The clock is real time, time since boot, or
 * the processor time of a process or of a thread of the caller's. A thread
 * SIGEV_THREAD_ID names must be one of the caller's process. EINVAL for another
 * clock, notification or signal, EAGAIN past PTIMER_MAX. The id is stored
 * before the timer is on the process's list, since the store can sleep in a
 * page fault, which the timers' lock cannot be held across, and a timer
 * already listed could be deleted by another thread meanwhile; a store that
 * fails is EFAULT with nothing listed.
 */
long sys_timer_create(struct isr_params *params)
{
    int clock = (int)SC_ARG(params, 0);
    struct tgroup *tg = current_proc->tg;
    struct ktimer *t, **link = &tg->timers;
    struct {
        uintptr_t value;
        int32_t signo;
        int32_t notify;
        int32_t tid;
    } ev; /* the head of Linux's struct sigevent */
    uint32_t irq;
    int id = 0;

    ev.value = 0;
    ev.signo = SIGALRM;
    ev.notify = SIGEV_SIGNAL;
    ev.tid = 0;
    if (SC_ARG(params, 1) &&
        copy_from_user(&ev, (const void __user *)SC_ARG(params, 1), sizeof(ev)) != 0) {
        return -EFAULT;
    }
    if ((ev.notify != SIGEV_SIGNAL && ev.notify != SIGEV_NONE && ev.notify != SIGEV_THREAD_ID) ||
        (ev.notify != SIGEV_NONE && (ev.signo < 1 || ev.signo > KSIG_MAX)) ||
        (ev.notify == SIGEV_THREAD_ID && !thread_of(tg, (uint32_t)ev.tid))) {
        return -EINVAL;
    }
    t = kzalloc(sizeof(*t));
    if (!t) {
        return -EAGAIN;
    }
    if (ptimer_clock(t, clock) != 0) {
        kfree(t);
        return -EINVAL;
    }
    /* The identifier goes to the caller before the timer is published: a
     * copy that sleeps in a fault would leave a published timer for another
     * thread to delete under it. A thread that takes the identifier while
     * the copy sleeps has the next free one tried. */
    for (;;) {
        irq = spin_lock_irqsave(&timer_lock);
        id = 0;
        for (link = &tg->timers; *link && (*link)->id == id; link = &(*link)->in_group) {
            id++;
        }
        spin_unlock_irqrestore(&timer_lock, irq);
        if (id >= PTIMER_MAX ||
            copy_to_user((void __user *)SC_ARG(params, 2), &id, sizeof(id)) != 0) {
            tgroup_put(t->cpu_tg);
            kfree(t);
            return id >= PTIMER_MAX ? -EAGAIN : -EFAULT;
        }
        irq = spin_lock_irqsave(&timer_lock);
        for (link = &tg->timers; *link && (*link)->id < id; link = &(*link)->in_group) {
        }
        if (!*link || (*link)->id != id) {
            break;
        }
        spin_unlock_irqrestore(&timer_lock, irq);
    }
    t->clock = clock;
    t->tg = tg;
    t->id = id;
    t->notify = ev.notify;
    t->signo = ev.signo;
    t->value = SC_ARG(params, 1) ? ev.value : (uintptr_t)id;
    t->tid = (uint32_t)ev.tid;
    t->in_group = *link;
    *link = t;
    spin_unlock_irqrestore(&timer_lock, irq);
    return 0;
}

static int32_t ptimer_settime(struct isr_params *params, int wide)
{
    struct ktimer *t = ptimer_of((int)SC_ARG(params, 0));

    if (!t || ((int)SC_ARG(params, 1) & ~TIMER_ABSTIME) || SC_ARG(params, 2) == 0) {
        return -EINVAL;
    }
    return set_and_report(t, (int)SC_ARG(params, 1), SC_ARG(params, 2), SC_ARG(params, 3), wide);
}

static int32_t ptimer_gettime(struct isr_params *params, int wide)
{
    struct ktimer *t = ptimer_of((int)SC_ARG(params, 0));
    struct setting now;

    if (!t) {
        return -EINVAL;
    }
    current_setting(t, &now);
    return put_setting(SC_ARG(params, 1), wide, &now);
}

long sys_timer_settime(struct isr_params *params)
{
    return ptimer_settime(params, 0);
}

long sys_timer_settime64(struct isr_params *params)
{
    return ptimer_settime(params, 1);
}

long sys_timer_gettime(struct isr_params *params)
{
    return ptimer_gettime(params, 0);
}

long sys_timer_gettime64(struct isr_params *params)
{
    return ptimer_gettime(params, 1);
}

long sys_timer_getoverrun(struct isr_params *params)
{
    struct ktimer *t = ptimer_of((int)SC_ARG(params, 0));

    return t ? (int32_t)t->overrun : -EINVAL;
}

/* Take the timer `link` names out of its process's list, disarmed, and
 * free it. */
static void ptimer_free(struct ktimer **link)
{
    struct ktimer *t = *link;
    uint32_t irq = spin_lock_irqsave(&timer_lock);

    if (t->armed) {
        disarm(t);
    }
    *link = t->in_group;
    spin_unlock_irqrestore(&timer_lock, irq);
    tgroup_put(t->cpu_tg);
    kfree(t);
}

long sys_timer_delete(struct isr_params *params)
{
    struct ktimer **link = &current_proc->tg->timers;

    while (*link && (*link)->id != (int)SC_ARG(params, 0)) {
        link = &(*link)->in_group;
    }
    if (!*link) {
        return -EINVAL;
    }
    ptimer_free(link);
    return 0;
}

void ptimer_delete_all(struct tgroup *tg)
{
    while (tg->timers) {
        ptimer_free(&tg->timers);
    }
}
