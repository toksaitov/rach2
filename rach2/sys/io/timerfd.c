/*
 * timerfd.c: timers read through descriptors, as timerfd.h describes them.
 */

#include "timerfd.h"

#include "anon.h"
#include "../isr.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
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

struct timerfd {
    struct anon_obj anon;
    struct timerfd *next; /* the list of armed timers the tick walks */
    int clock;
    int armed;
    uint32_t due;   /* the tick the wait in progress ends */
    uint32_t spans; /* whole spans to wait after it, before the expiration */
    uint32_t isec;  /* the interval, 0 and 0 for none: whole seconds, */
    uint32_t isub;  /* and ticks below a second */
    uint64_t count; /* expirations not yet read */
    wait_queue_t wq;
};

/* A setting as the call gives and takes it, whichever width of time: the
 * interval, then the first expiration. */
struct setting {
    int64_t isec;
    int32_t insec;
    int64_t vsec;
    int32_t vnsec;
};

static struct timerfd *armed;

static void disarm(struct timerfd *t)
{
    struct timerfd **link = &armed;

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
static void wait_from(struct timerfd *t, uint32_t from, uint32_t sec, uint32_t sub)
{
    t->spans = sec / SPAN_SEC;
    t->due = from + (sec % SPAN_SEC) * TICKS_PER_SEC + sub;
}

/* An expiration of `t`, due by the tick `now`: counted, with those of an
 * interval missed since, and the timer armed again by its interval or
 * disarmed. */
static void expire(struct timerfd *t, uint32_t now)
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
    wq_wake_all(&t->wq);
}

void timerfd_tick(uint32_t now)
{
    struct timerfd *t = armed;

    while (t) {
        struct timerfd *next = t->next;

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
    return ((struct timerfd *)arg)->count != 0;
}

/**
 * read(2) of a timerfd: the expirations since the last read, which it takes.
 *
 * The count is read and cleared with interrupts off, since the timer
 * interrupt adds to it (`timerfd_tick`), and before the copy to the caller,
 * which can sleep in a page fault: an expiration that comes meanwhile is
 * kept for the next read, and a second reader finds none of the ones this
 * read took. A copy that fails loses the count, as in Linux.
 */
static int32_t timerfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct timerfd *t = (struct timerfd *)e->priv;
    uint64_t n;

    if (count < sizeof(n)) {
        return -EINVAL;
    }
    for (;;) {
        uint32_t irq = kirq_save();

        n = t->count;
        t->count = 0;
        kirq_restore(irq);
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
    struct timerfd *t = (struct timerfd *)e->priv;

    if (wq) {
        *wq = &t->wq;
    }
    return (short)(t->count ? (events & POLLIN) : 0);
}

static void timerfd_close(fd_entry_t *e)
{
    struct timerfd *t = (struct timerfd *)e->priv;
    uint32_t flags = kirq_save();

    if (t->armed) {
        disarm(t);
    }
    kirq_restore(flags);
    kfree(t);
    e->priv = NULL;
}

static const struct file_ops timerfd_ops = {timerfd_read, NULL, timerfd_poll, NULL, timerfd_close};

static struct timerfd *timer_of(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return (e && e->ops == &timerfd_ops) ? (struct timerfd *)e->priv : NULL;
}

int32_t sys_timerfd_create(struct isr_params *params)
{
    int clock = (int)params->ebx;
    uint32_t flags = params->ecx;
    struct timerfd *t;
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
 * interval, both zero when it is disarmed. It is read with interrupts off,
 * since the tick moves the timer. */
static void current_setting(const struct timerfd *t, struct setting *s)
{
    uint32_t irq = kirq_save();

    k_memset(s, 0, sizeof(*s));
    if (t->armed) {
        int32_t left = (int32_t)(t->due - kernel_ticks);

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
    kirq_restore(irq);
}

/* Arm or disarm `t` from the setting `s`, which `flags` makes absolute. */
static int32_t arm(struct timerfd *t, int flags, const struct setting *s)
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
    irq = kirq_save();
    if (t->armed) {
        disarm(t);
    }
    t->count = 0;
    if (on) {
        wait_from(t, kernel_ticks + 1u, sec, sub);
        t->isec = isec;
        t->isub = isub;
        t->armed = 1;
        t->next = armed;
        armed = t;
    }
    kirq_restore(irq);
    return 0;
}

/* The two widths of struct itimerspec: 32-bit seconds and nanoseconds, or
 * 64-bit ones, the interval first. */
static int32_t get_setting(uint32_t u, int wide, struct setting *s)
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
        int32_t v[4];

        if (copy_from_user(v, (const void __user *)u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v[1] < 0 || v[1] >= NS_PER_SEC || v[3] < 0 || v[3] >= NS_PER_SEC) {
            return -EINVAL;
        }
        s->isec = v[0];
        s->insec = v[1];
        s->vsec = v[2];
        s->vnsec = v[3];
    }
    return s->isec < 0 || s->vsec < 0 ? -EINVAL : 0;
}

static int32_t put_setting(uint32_t u, int wide, const struct setting *s)
{
    if (wide) {
        int64_t v[4];

        v[0] = s->isec;
        v[1] = s->insec;
        v[2] = s->vsec;
        v[3] = s->vnsec;
        return copy_to_user((void __user *)u, v, sizeof(v)) != 0 ? -EFAULT : 0;
    } else {
        int32_t v[4];

        v[0] = (int32_t)s->isec;
        v[1] = s->insec;
        v[2] = (int32_t)s->vsec;
        v[3] = s->vnsec;
        return copy_to_user((void __user *)u, v, sizeof(v)) != 0 ? -EFAULT : 0;
    }
}

static int32_t settime(struct isr_params *params, int wide)
{
    struct timerfd *t = timer_of((int)params->ebx);
    int flags = (int)params->ecx;
    struct setting now, want;
    int32_t rc;

    if (!fd_get(current_proc->fds, (int)params->ebx)) {
        return -EBADF;
    }
    if (!t || (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))) {
        return -EINVAL;
    }
    rc = get_setting(params->edx, wide, &want);
    if (rc != 0) {
        return rc;
    }
    current_setting(t, &now);
    rc = arm(t, flags, &want);
    if (rc == 0 && params->esi) {
        rc = put_setting(params->esi, wide, &now);
    }
    return rc;
}

static int32_t gettime(struct isr_params *params, int wide)
{
    struct timerfd *t = timer_of((int)params->ebx);
    struct setting now;

    if (!fd_get(current_proc->fds, (int)params->ebx)) {
        return -EBADF;
    }
    if (!t) {
        return -EINVAL;
    }
    current_setting(t, &now);
    return put_setting(params->ecx, wide, &now);
}

int32_t sys_timerfd_settime(struct isr_params *params)
{
    return settime(params, 0);
}

int32_t sys_timerfd_settime64(struct isr_params *params)
{
    return settime(params, 1);
}

int32_t sys_timerfd_gettime(struct isr_params *params)
{
    return gettime(params, 0);
}

int32_t sys_timerfd_gettime64(struct isr_params *params)
{
    return gettime(params, 1);
}
