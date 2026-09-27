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
 * kernel has no 64-bit division, so a time is kept as seconds and
 * nanoseconds and never as one count of nanoseconds. */
#define TICKS_PER_SEC 100u
#define NS_PER_TICK   10000000
#define NS_PER_SEC    1000000000
/* Policy: the longest wait, in ticks, a timer is armed for. */
#define TICKS_MAX     0x3FFFFFFFu

extern volatile uint32_t kernel_ticks;

struct timerfd {
    struct anon_obj anon;
    struct timerfd *next; /* the list of armed timers the tick walks */
    int clock;
    int armed;
    uint32_t due;      /* the tick of the next expiration */
    uint32_t interval; /* ticks between expirations, 0 for one */
    uint64_t count;    /* expirations not yet read */
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

void timerfd_tick(uint32_t now)
{
    struct timerfd *t = armed;

    while (t) {
        struct timerfd *next = t->next;

        if ((int32_t)(now - t->due) >= 0) {
            if (t->interval) {
                uint32_t late = (now - t->due) / t->interval;

                t->count += 1u + late;
                t->due += (late + 1u) * t->interval;
            } else {
                t->count++;
                disarm(t);
            }
            wq_wake_all(&t->wq);
        }
        t = next;
    }
}

/* Whole ticks of at least `sec` seconds and `nsec` nanoseconds, which is
 * below a second: a timer never expires early. */
static uint32_t ticks_of(int64_t sec, int32_t nsec)
{
    if (sec < 0) {
        return 0;
    }
    if (sec >= (int64_t)(TICKS_MAX / TICKS_PER_SEC)) {
        return TICKS_MAX;
    }
    return (uint32_t)sec * TICKS_PER_SEC + ((uint32_t)nsec + NS_PER_TICK - 1u) / NS_PER_TICK;
}

/* The same count of ticks back as seconds and nanoseconds. */
static void time_of(uint32_t ticks, int64_t *sec, int32_t *nsec)
{
    *sec = (int64_t)(ticks / TICKS_PER_SEC);
    *nsec = (int32_t)(ticks % TICKS_PER_SEC) * NS_PER_TICK;
}

static int readable(void *arg)
{
    return ((struct timerfd *)arg)->count != 0;
}

static int32_t timerfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct timerfd *t = (struct timerfd *)e->priv;
    uint64_t n;

    if (count < sizeof(n)) {
        return -EINVAL;
    }
    while (t->count == 0) {
        if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&t->wq, readable, t) < 0) {
            return -EINTR;
        }
    }
    n = t->count;
    if (copy_to_user((void __user *)buf, &n, sizeof(n)) != 0) {
        return -EFAULT;
    }
    t->count = 0;
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
    uint32_t flags = irq_save();

    if (t->armed) {
        disarm(t);
    }
    irq_restore(flags);
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
        return -EMFILE;
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
 * interval, both zero when it is disarmed. */
static void current_setting(const struct timerfd *t, struct setting *s)
{
    k_memset(s, 0, sizeof(*s));
    if (t->armed) {
        int32_t left = (int32_t)(t->due - kernel_ticks);

        if (left > 0) {
            time_of((uint32_t)left, &s->vsec, &s->vnsec);
        } else {
            s->vnsec = 1; /* due now: reported as not yet expired */
        }
        time_of(t->interval, &s->isec, &s->insec);
    }
}

/* Arm or disarm `t` from the setting `s`, which `flags` makes absolute. */
static int32_t arm(struct timerfd *t, int flags, const struct setting *s)
{
    int on = s->vsec != 0 || s->vnsec != 0;
    uint32_t wait = ticks_of(s->vsec, s->vnsec);
    uint32_t irq;

    if (s->vsec < 0 || s->vnsec < 0 || s->vnsec >= NS_PER_SEC) {
        return -EINVAL;
    }
    if (on && (flags & TFD_TIMER_ABSTIME)) {
        int64_t sec;
        int32_t nsec;
        int32_t rc = clock_now(t->clock, &sec, &nsec);

        if (rc != 0) {
            return rc;
        }
        sec = s->vsec - sec;
        nsec = s->vnsec - nsec;
        if (nsec < 0) {
            nsec += NS_PER_SEC;
            sec--;
        }
        wait = sec < 0 ? 0u : ticks_of(sec, nsec);
    }
    irq = irq_save();
    if (t->armed) {
        disarm(t);
    }
    t->count = 0;
    if (on) {
        t->due = kernel_ticks + wait + 1u;
        t->interval = ticks_of(s->isec, s->insec);
        t->armed = 1;
        t->next = armed;
        armed = t;
    }
    irq_restore(irq);
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
