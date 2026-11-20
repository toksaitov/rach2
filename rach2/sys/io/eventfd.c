/*
 * eventfd.c: the eventfd counter, as eventfd.h describes it.
 */

#include "eventfd.h"

#include "anon.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  04000
#define EFD_CLOEXEC   02000000

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008

/* The counter's largest value: a write that would pass it waits. Only a
 * kernel signal (`eventfd_signal`) carries the counter above it, to
 * 0xFFFFFFFFFFFFFFFF, where every write waits until a read. */
#define EFD_MAX 0xFFFFFFFFFFFFFFFEull

/*
 * The counter. `lock` is over `count` and `hooks`, which a kernel signal
 * from an interrupt handler changes and walks, `closed`, and `refs`, which
 * counts the open description, until its last close, and each kernel
 * holder: the descriptor's calls and its last close run without the big
 * lock (FILE_OPS_UNLOCKED), beside holders that take and drop references
 * with or without it, as Linux's eventfd_ctx keeps a kref.
 */
struct eventfd {
    struct anon_obj anon;
    uint64_t count;
    int semaphore;
    wait_queue_t wq;
    struct spinlock lock;
    uint32_t refs;
    int closed; /* the last descriptor has closed */
    struct eventfd_hook *hooks;
};

/** Take what a read takes from the counter of `ev`, whose lock the caller
 * holds: 1 with EFD_SEMAPHORE, else the whole counter. */
static uint64_t take_locked(struct eventfd *ev)
{
    uint64_t value = (ev->semaphore && ev->count) ? 1u : ev->count;

    ev->count -= value;
    return value;
}

/** Tell the hooks of `ev`, whose lock the caller holds, that the counter
 * rose, passing over `skip` when non-NULL. The counter is taken when a
 * hook asks. Returns 1 when one did. */
static int signal_locked(struct eventfd *ev, const struct eventfd_hook *skip)
{
    struct eventfd_hook *h, *next;
    int took = 0;

    for (h = ev->hooks; h; h = next) {
        next = h->next;
        /* Compared as addresses: cppcheck takes h == skip, skip NULL, as a
         * null test of h. */
        if ((uintptr_t)h == (uintptr_t)skip)
            continue;
        if (h->signal(h) && ev->count != 0) {
            take_locked(ev);
            took = 1;
        }
    }
    return took;
}

/** Whether the description of `e` is non-blocking. */
static int nonblocking(const fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/** The wait condition of a read: the counter of the eventfd `arg` is not
 * zero, read under its lock, since a 64-bit word is two loads on i686. */
static int readable(void *arg)
{
    struct eventfd *ev = (struct eventfd *)arg;
    uint32_t flags = spin_lock_irqsave(&ev->lock);
    int r = ev->count != 0;

    spin_unlock_irqrestore(&ev->lock, flags);
    return r;
}

/* A write's addition, the argument of its wait condition `fits`. */
struct add {
    struct eventfd *ev;
    uint64_t value;
};

/** Whether the addition `a` keeps its counter, whose lock the caller
 * holds, at EFD_MAX or below: never while the counter is above EFD_MAX, as
 * Linux's eventfd_write tests ULLONG_MAX - ctx->count > ucnt. */
static int fits_locked(const struct add *a)
{
    return a->ev->count <= EFD_MAX && a->value <= EFD_MAX - a->ev->count;
}

/** The wait condition of a write: `fits_locked` of the addition `arg`,
 * under its counter's lock. */
static int fits(void *arg)
{
    struct add *a = (struct add *)arg;
    uint32_t flags = spin_lock_irqsave(&a->ev->lock);
    int r = fits_locked(a);

    spin_unlock_irqrestore(&a->ev->lock, flags);
    return r;
}

/**
 * read(2) of an eventfd: the counter, or 1 with EFD_SEMAPHORE, taken off
 * the counter as it is read, and the writers woken, before the copy to the
 * caller, as Linux takes it. The copy can sleep in a page fault, and another
 * reader must not find the same value meanwhile; a copy that fails loses
 * the value, as in Linux. The take, and the choice to wait or fail with
 * EAGAIN on a counter it found zero, are made in one hold of the counter's
 * lock, as Linux's eventfd_read makes them under ctx->wqh.lock. Returns
 * 8, -EINVAL for a buffer under eight bytes, -EAGAIN, -EINTR or -EFAULT.
 */
static int32_t eventfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    uint64_t value;
    uint32_t flags;

    if (count < sizeof(value)) {
        return -EINVAL;
    }
    for (;;) {
        flags = spin_lock_irqsave(&ev->lock);
        value = take_locked(ev);
        spin_unlock_irqrestore(&ev->lock, flags);
        if (value != 0) {
            break;
        }
        if (nonblocking(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&ev->wq, readable, ev) < 0) {
            return -EINTR;
        }
    }
    wq_wake(&ev->wq, WQ_ROOM);
    if (copy_to_user((void __user *)buf, &value, sizeof(value)) != 0) {
        return -EFAULT;
    }
    return (int32_t)sizeof(value);
}

/**
 * write(2) of an eventfd: add the eight-byte value at `buf` to the counter,
 * waiting while the sum would pass EFD_MAX, and wake the readers when the
 * value is not zero. The test, the add, and the choice to wait or fail
 * with EAGAIN are made under the counter's lock, as Linux's eventfd_write
 * makes them under ctx->wqh.lock. Returns 8, -EINVAL for a buffer under
 * eight bytes or the value 0xFFFFFFFFFFFFFFFF, -EFAULT, -EAGAIN or -EINTR.
 */
static int32_t eventfd_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    struct add a;
    uint32_t flags;
    int took = 0;

    if (count < sizeof(a.value)) {
        return -EINVAL;
    }
    if (copy_from_user(&a.value, (const void __user *)buf, sizeof(a.value)) != 0) {
        return -EFAULT;
    }
    if (a.value == 0xFFFFFFFFFFFFFFFFull) {
        return -EINVAL;
    }
    a.ev = ev;
    for (;;) {
        flags = spin_lock_irqsave(&ev->lock);
        if (fits_locked(&a)) {
            break;
        }
        spin_unlock_irqrestore(&ev->lock, flags);
        if (nonblocking(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&ev->wq, fits, &a) < 0) {
            return -EINTR;
        }
    }
    ev->count += a.value;
    if (a.value != 0) {
        took = signal_locked(ev, NULL);
    }
    spin_unlock_irqrestore(&ev->lock, flags);
    if (a.value != 0) {
        wq_wake(&ev->wq, took ? (WQ_DATA | WQ_ROOM) : WQ_DATA);
    }
    return (int32_t)sizeof(a.value);
}

/** poll of an eventfd: POLLIN while the counter is not zero, POLLOUT
 * while it is below EFD_MAX and POLLERR at 0xFFFFFFFFFFFFFFFF, which only
 * a kernel signal reaches, among `events`, and its wait queue. It runs
 * without the big lock (FILE_OPS_POLL_UNLOCKED) and reads the counter under
 * the counter's lock, since a 64-bit word is two loads on i686, where
 * Linux's eventfd_poll reads it once after poll_wait (fs/eventfd.c:
 * 118-174). */
static short eventfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    short mask = 0;
    uint64_t count;
    uint32_t flags;

    if (wq) {
        *wq = &ev->wq;
    }
    flags = spin_lock_irqsave(&ev->lock);
    count = ev->count;
    spin_unlock_irqrestore(&ev->lock, flags);
    if (count != 0) {
        mask |= POLLIN;
    }
    if (count < EFD_MAX) {
        mask |= POLLOUT;
    }
    if (count == 0xFFFFFFFFFFFFFFFFull) {
        mask |= POLLERR;
    }
    return (short)(mask & events);
}

/**
 * The last close of an eventfd, which runs without the big lock: the
 * counter is marked closed, which stops every add, and when hooks are on
 * it, the big lock is taken and each leaves the list in its own hold of the
 * counter's lock and is told (`eventfd_hook`), its hangup read while it is
 * still on the list. A remover, under the big lock, so finds its hook on
 * the list or finds it handed to its hangup, never one taken off and not
 * yet told, even across a hangup that sleeps and lets the big lock go.
 * Then the description's reference goes.
 */
static void eventfd_close(fd_entry_t *e)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    void (*hangup)(struct eventfd_hook *);
    struct eventfd_hook *h;
    uint32_t flags;
    int hooked;

    flags = spin_lock_irqsave(&ev->lock);
    ev->closed = 1;
    hooked = ev->hooks != NULL;
    spin_unlock_irqrestore(&ev->lock, flags);
    if (!hooked) {
        e->priv = NULL;
        eventfd_put(ev);
        return;
    }
    bkl_enter();
    for (;;) {
        flags = spin_lock_irqsave(&ev->lock);
        h = ev->hooks;
        hangup = NULL;
        if (h) {
            ev->hooks = h->next;
            h->next = NULL;
            hangup = h->hangup;
        }
        spin_unlock_irqrestore(&ev->lock, flags);
        if (!h) {
            break;
        }
        if (hangup) {
            hangup(h);
        }
    }
    bkl_exit();
    e->priv = NULL;
    eventfd_put(ev);
}

/* Every operation runs without the big lock, under the counter's lock and
 * its wait queue's, as Linux's eventfd_read, eventfd_write and
 * eventfd_release take only ctx->wqh.lock (fs/eventfd.c:109-116, 214-280):
 * a write that ends another thread's epoll wait takes no lock of the whole
 * kernel. */
static const struct file_ops eventfd_ops = {
    eventfd_read,  eventfd_write, eventfd_poll, NULL,
    eventfd_close, NULL,          NULL,         FILE_OPS_UNLOCKED | FILE_OPS_POLL_UNLOCKED};

/** A new eventfd with the counter `count` and the flags `flags`, and a
 * descriptor of the caller's on it: the descriptor, -EINVAL for another
 * flag, -ENOMEM, or `fd_alloc`'s error. */
static int32_t eventfd_open(uint32_t count, uint32_t flags)
{
    struct eventfd *ev;
    fd_entry_t *e;
    int fd;

    if (flags & ~(uint32_t)(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC)) {
        return -EINVAL;
    }
    ev = kzalloc(sizeof(*ev));
    if (!ev) {
        return -ENOMEM;
    }
    spin_lock_init(&ev->lock, "eventfd");
    ev->anon.name = "[eventfd]";
    ev->refs = 1;
    ev->count = count;
    ev->semaphore = (flags & EFD_SEMAPHORE) != 0;
    wait_queue_init(&ev->wq);
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(ev);
        return fd;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &eventfd_ops;
    e->priv = ev;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | ((flags & EFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (flags & EFD_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    fd_install(current_proc->fds, fd);
    return fd;
}

long sys_eventfd2(struct isr_params *params)
{
    return eventfd_open((uint32_t)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1));
}

/** eventfd has no flags. The register of a second argument is not one of
 * its arguments, and comes back as it went, as every register but the
 * result's does. */
long sys_eventfd(struct isr_params *params)
{
    return eventfd_open((uint32_t)SC_ARG(params, 0), 0);
}

struct eventfd *eventfd_get(int fd, int32_t *err)
{
    return eventfd_entry_get(current_proc ? fd_get(current_proc->fds, fd) : NULL, err);
}

struct eventfd *eventfd_entry_get(const struct fd_entry *e, int32_t *err)
{
    struct eventfd *ev;
    uint32_t flags;

    if (!e) {
        *err = -EBADF;
        return NULL;
    }
    if (e->ops != &eventfd_ops || !e->priv) {
        *err = -EINVAL;
        return NULL;
    }
    ev = (struct eventfd *)e->priv;
    flags = spin_lock_irqsave(&ev->lock);
    ev->refs++;
    spin_unlock_irqrestore(&ev->lock, flags);
    return ev;
}

void eventfd_put(struct eventfd *ev)
{
    uint32_t flags = spin_lock_irqsave(&ev->lock);
    uint32_t left = --ev->refs;

    spin_unlock_irqrestore(&ev->lock, flags);
    if (left == 0) {
        kfree(ev);
    }
}

void eventfd_signal_except(struct eventfd *ev, const struct eventfd_hook *skip)
{
    uint32_t flags = spin_lock_irqsave(&ev->lock);
    int took = 0;

    if (ev->count == 0xFFFFFFFFFFFFFFFFull) {
        spin_unlock_irqrestore(&ev->lock, flags);
        return;
    }
    ev->count++;
    took = signal_locked(ev, skip);
    spin_unlock_irqrestore(&ev->lock, flags);
    wq_wake(&ev->wq, took ? (WQ_DATA | WQ_ROOM) : WQ_DATA);
}

void eventfd_signal(struct eventfd *ev)
{
    eventfd_signal_except(ev, NULL);
}

int eventfd_hook_add(struct eventfd *ev, struct eventfd_hook *h)
{
    uint32_t flags;
    int r;

    flags = spin_lock_irqsave(&ev->lock);
    if (ev->closed) {
        spin_unlock_irqrestore(&ev->lock, flags);
        h->next = NULL;
        if (h->hangup) {
            h->hangup(h);
        }
        return -1;
    }
    h->next = ev->hooks;
    ev->hooks = h;
    r = ev->count != 0;
    spin_unlock_irqrestore(&ev->lock, flags);
    return r;
}

int eventfd_hook_remove(struct eventfd *ev, struct eventfd_hook *h, uint64_t *cnt)
{
    struct eventfd_hook **p;
    uint64_t value = 0;
    uint32_t flags;
    int found = 0;

    flags = spin_lock_irqsave(&ev->lock);
    for (p = &ev->hooks; *p; p = &(*p)->next) {
        if (*p == h) {
            *p = h->next;
            h->next = NULL;
            found = 1;
            break;
        }
    }
    if (cnt) {
        value = take_locked(ev);
        *cnt = value;
    }
    spin_unlock_irqrestore(&ev->lock, flags);
    if (value != 0) {
        wq_wake(&ev->wq, WQ_ROOM);
    }
    return found;
}
