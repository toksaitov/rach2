/*
 * eventfd.c: the eventfd counter, as eventfd.h describes it.
 */

#include "eventfd.h"

#include "anon.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  04000
#define EFD_CLOEXEC   02000000

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008

/* The counter's largest value: a write that would pass it waits. */
#define EFD_MAX 0xFFFFFFFFFFFFFFFEull

struct eventfd {
    struct anon_obj anon;
    uint64_t count;
    int semaphore;
    wait_queue_t wq;
};

/** Whether the description of `e` is non-blocking. */
static int nonblocking(const fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/** The wait condition of a read: the counter of the eventfd `arg` is not
 * zero. */
static int readable(void *arg)
{
    return ((struct eventfd *)arg)->count != 0;
}

/* A write's addition, the argument of its wait condition `fits`. */
struct add {
    struct eventfd *ev;
    uint64_t value;
};

/** The wait condition of a write: the value of the addition `arg` keeps
 * the counter at EFD_MAX or below. */
static int fits(void *arg)
{
    const struct add *a = (const struct add *)arg;

    return a->value <= EFD_MAX - a->ev->count;
}

/**
 * read(2) of an eventfd: the counter, or 1 with EFD_SEMAPHORE, taken off
 * the counter as it is read, and the writers woken, before the copy to the
 * caller, as Linux takes it. The copy can sleep in a page fault, and another
 * reader must not find the same value meanwhile; a copy that fails loses
 * the value, as in Linux. Returns 8, -EINVAL for a buffer under eight
 * bytes, -EAGAIN, -EINTR or -EFAULT.
 */
static int32_t eventfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    uint64_t value;

    if (count < sizeof(value)) {
        return -EINVAL;
    }
    while (ev->count == 0) {
        if (nonblocking(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&ev->wq, readable, ev) < 0) {
            return -EINTR;
        }
    }
    value = ev->semaphore ? 1u : ev->count;
    ev->count -= value;
    wq_wake(&ev->wq, WQ_ROOM);
    if (copy_to_user((void __user *)buf, &value, sizeof(value)) != 0) {
        return -EFAULT;
    }
    return (int32_t)sizeof(value);
}

/**
 * write(2) of an eventfd: add the eight-byte value at `buf` to the counter,
 * waiting while the sum would pass EFD_MAX, and wake the readers when the
 * value is not zero. Returns 8, -EINVAL for a buffer under eight bytes or
 * the value 0xFFFFFFFFFFFFFFFF, -EFAULT, -EAGAIN or -EINTR.
 */
static int32_t eventfd_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    struct add a;

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
    while (!fits(&a)) {
        if (nonblocking(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&ev->wq, fits, &a) < 0) {
            return -EINTR;
        }
    }
    ev->count += a.value;
    if (a.value != 0) {
        wq_wake(&ev->wq, WQ_DATA);
    }
    return (int32_t)sizeof(a.value);
}

/** poll of an eventfd: POLLIN while the counter is not zero and POLLOUT
 * while it is below EFD_MAX, among `events`, and its wait queue. */
static short eventfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct eventfd *ev = (struct eventfd *)e->priv;
    short mask = 0;

    if (wq) {
        *wq = &ev->wq;
    }
    if (ev->count != 0) {
        mask |= POLLIN;
    }
    if (ev->count < EFD_MAX) {
        mask |= POLLOUT;
    }
    return (short)(mask & events);
}

/** The last close of an eventfd: the counter is freed. */
static void eventfd_close(fd_entry_t *e)
{
    kfree(e->priv);
    e->priv = NULL;
}

static const struct file_ops eventfd_ops = {eventfd_read, eventfd_write, eventfd_poll, NULL,
                                            eventfd_close};

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
    ev->anon.name = "[eventfd]";
    ev->count = count;
    ev->semaphore = (flags & EFD_SEMAPHORE) != 0;
    wait_queue_init(&ev->wq);
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(ev);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &eventfd_ops;
    e->priv = ev;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | ((flags & EFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (flags & EFD_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
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
