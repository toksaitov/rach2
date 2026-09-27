/*
 * epoll.c: epoll sets, as epoll.h describes them.
 */

#include "epoll.h"

#include "anon.h"
#include "poll.h"
#include "../isr.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"

#define EPOLL_CLOEXEC 02000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLET        (1u << 31)

/* The events a registration may watch; the rest are how it is reported. */
#define EPOLL_EVENTS 0x0000FFFFu

/* Format: the i386 struct epoll_event, packed: the events, then the data. */
#define EP_EVENT_SIZE 12u

/* Policy: the events one wait returns at most, as Linux bounds maxevents. */
#define EP_MAX_EVENTS (0x7FFFFFFF / EP_EVENT_SIZE)

struct epitem {
    struct epitem *next;
    int fd;
    /* The open file the number named when added, and its serial, which a
     * new description at the same address after its last close has not. */
    const struct open_file *ofd;
    uint32_t serial;
    uint32_t events;
    uint32_t data[2];
    int off; /* EPOLLONESHOT: reported, and off until EPOLL_CTL_MOD */
    /* EPOLLET: the events reported that have not gone since, and the wait
     * queue of the descriptor at that report with its count of wakes. */
    uint32_t seen;
    const struct wait_queue *at;
    uint32_t wakes;
};

struct epoll {
    struct anon_obj anon;
    struct epitem *items;
    uint32_t count;
    wait_queue_t wq;
};

struct epoll_out {
    uint32_t events;
    uint32_t data[2];
};

static struct epoll *epoll_of(int fd);

/* Whether the descriptor `e` is the open file the registration `it` was
 * made for. */
static int item_names(const struct epitem *it, const fd_entry_t *e)
{
    return e && e->ofd == it->ofd && e->ofd->serial == it->serial;
}

/* Whether the wait queue `wq` of an edge-triggered registration's
 * descriptor has been woken since `it` was last reported, or is another. */
static int item_woken(const struct epitem *it, const struct wait_queue *wq)
{
    return wq != it->at || (wq && wq->wakes != it->wakes);
}

/**
 * The events of `it` a wait would report now, or 0; the wait queue of its
 * descriptor in `*wq`. EPOLLERR and EPOLLHUP are watched whatever the
 * registration asked, none included, and nothing is while a one-shot
 * registration is off. A registration whose number names another open
 * file, or none, is marked for removal with `*gone`.
 *
 * An edge-triggered registration is reported when an event is ready that
 * was not at its last report, or has gone since, or when its descriptor's
 * queue has been woken since that report: a pipe drained and filled again
 * between two waits is ready as it was, and its reader's queue is what
 * tells the new edge, as Linux's wakeup callback does.
 */
static uint32_t item_ready(struct epitem *it, struct wait_queue **wq, int *gone)
{
    fd_entry_t *e = fd_get(current_proc->fds, it->fd);
    uint32_t want = (it->events & EPOLL_EVENTS) | EPOLLERR | EPOLLHUP;
    uint32_t ready;

    *wq = NULL;
    *gone = !item_names(it, e);
    if (*gone || it->off) {
        return 0;
    }
    ready = (uint32_t)(uint16_t)poll_fd(e, (short)want, wq) & want;
    if (it->events & EPOLLET) {
        it->seen &= ready;
        if (!(ready & ~it->seen) && !item_woken(it, *wq)) {
            ready = 0;
        }
    }
    return ready;
}

/* Record the report of `ready` for `it`, whose descriptor's wait queue is
 * `wq`: what an edge-triggered registration is not reported again for, and
 * the end of a one-shot one. */
static void item_reported(struct epitem *it, uint32_t ready, const struct wait_queue *wq)
{
    if (it->events & EPOLLET) {
        it->seen = ready;
        it->at = wq;
        it->wakes = wq ? wq->wakes : 0;
    }
    if (it->events & EPOLLONESHOT) {
        it->off = 1;
    }
}

/* A wait's view of the set: where the ready events go, and how many fit. */
struct epoll_scan {
    struct epoll *ep;
    struct epoll_out *out;
    uint32_t max;
    uint32_t n;
    uint32_t queues;
};

static void drop_item(struct epoll *ep, struct epitem **link)
{
    struct epitem *it = *link;

    *link = it->next;
    ep->count--;
    kfree(it);
}

static int scan_set(void *ctx, struct wait_queue **wqs)
{
    struct epoll_scan *sc = (struct epoll_scan *)ctx;
    struct epitem **link = &sc->ep->items;
    struct epitem *it;
    uint32_t q = 1;

    sc->n = 0;
    wqs[0] = &sc->ep->wq;
    while ((it = *link) != NULL && sc->n < sc->max) {
        struct wait_queue *wq;
        uint32_t ready;
        int gone;

        ready = item_ready(it, &wq, &gone);
        if (gone) {
            drop_item(sc->ep, link);
            continue;
        }
        if (q < sc->queues) {
            wqs[q++] = wq;
        }
        if (ready) {
            sc->out[sc->n].events = ready;
            sc->out[sc->n].data[0] = it->data[0];
            sc->out[sc->n].data[1] = it->data[1];
            sc->n++;
            item_reported(it, ready, wq);
        }
        link = &it->next;
    }
    return (int)sc->n;
}

static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct epoll *ep = (struct epoll *)e->priv;
    struct epitem *it;

    if (wq) {
        *wq = &ep->wq;
    }
    for (it = ep->items; it; it = it->next) {
        fd_entry_t *t = fd_get(current_proc->fds, it->fd);
        struct wait_queue *ignored;
        uint32_t want = (it->events & EPOLL_EVENTS) | EPOLLERR | EPOLLHUP;

        if (item_names(it, t) && t->ops != e->ops && !it->off &&
            ((uint32_t)(uint16_t)poll_fd(t, (short)want, &ignored) & want)) {
            return (short)(events & 0x001);
        }
    }
    return 0;
}

static void epoll_close(fd_entry_t *e)
{
    struct epoll *ep = (struct epoll *)e->priv;

    while (ep->items) {
        drop_item(ep, &ep->items);
    }
    kfree(ep);
    e->priv = NULL;
}

static const struct file_ops epoll_ops = {NULL, NULL, epoll_poll, NULL, epoll_close};

static struct epoll *epoll_of(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return (e && e->ops == &epoll_ops) ? (struct epoll *)e->priv : NULL;
}

static int32_t make_epoll(uint32_t flags)
{
    struct epoll *ep;
    fd_entry_t *e;
    int fd;

    if (flags & ~(uint32_t)EPOLL_CLOEXEC) {
        return -EINVAL;
    }
    ep = kzalloc(sizeof(*ep));
    if (!ep) {
        return -ENOMEM;
    }
    ep->anon.name = "[eventpoll]";
    wait_queue_init(&ep->wq);
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(ep);
        return -EMFILE;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &epoll_ops;
    e->priv = ep;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | ((flags & EPOLL_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    return fd;
}

int32_t sys_epoll_create(struct isr_params *params)
{
    if ((int32_t)params->ebx <= 0) {
        return -EINVAL;
    }
    return make_epoll(0);
}

int32_t sys_epoll_create1(struct isr_params *params)
{
    return make_epoll(params->ebx);
}

int32_t sys_epoll_ctl(struct isr_params *params)
{
    struct epoll *ep = epoll_of((int)params->ebx);
    int op = (int)params->ecx;
    int fd = (int)params->edx;
    fd_entry_t *target = fd_get(current_proc->fds, fd);
    uint32_t ev[3] = {0, 0, 0};
    struct epitem **link;
    struct epitem *it;

    if (!fd_get(current_proc->fds, (int)params->ebx) || !target) {
        return -EBADF;
    }
    if (!ep || fd == (int)params->ebx) {
        return -EINVAL;
    }
    if (target->type == FD_TYPE_FILE || target->type == FD_TYPE_DIR) {
        return -EPERM;
    }
    if (op != EPOLL_CTL_DEL &&
        copy_from_user(ev, (const void __user *)params->esi, EP_EVENT_SIZE) != 0) {
        return -EFAULT;
    }
    for (link = &ep->items; (it = *link) != NULL; link = &it->next) {
        if (it->fd == fd && item_names(it, target)) {
            break;
        }
    }
    switch (op) {
    case EPOLL_CTL_ADD:
        if (it) {
            return -EEXIST;
        }
        it = kzalloc(sizeof(*it));
        if (!it) {
            return -ENOMEM;
        }
        it->fd = fd;
        it->ofd = target->ofd;
        it->serial = target->ofd->serial;
        it->next = ep->items;
        ep->items = it;
        ep->count++;
        break;
    case EPOLL_CTL_MOD:
        if (!it) {
            return -ENOENT;
        }
        if ((ev[0] & EPOLLEXCLUSIVE) || (it->events & EPOLLEXCLUSIVE)) {
            return -EINVAL;
        }
        break;
    case EPOLL_CTL_DEL:
        if (!it) {
            return -ENOENT;
        }
        drop_item(ep, link);
        return 0;
    default:
        return -EINVAL;
    }
    it->events = ev[0] & ~EPOLLWAKEUP;
    it->data[0] = ev[1];
    it->data[1] = ev[2];
    it->off = 0;
    it->seen = 0;
    it->at = NULL;
    it->wakes = 0;
    wq_wake_all(&ep->wq);
    return 0;
}

/* A wait on the set `epfd` for at most `max` events into the user array
 * `uevents`, until the tick `deadline` when `has_deadline`. */
static int32_t epoll_wait_common(int epfd, uint32_t uevents, int max, int has_deadline,
                                 uint32_t deadline)
{
    struct epoll *ep = epoll_of(epfd);
    struct epoll_scan sc;
    int32_t rc;
    uint32_t i;

    if (!fd_get(current_proc->fds, epfd)) {
        return -EBADF;
    }
    if (!ep || max <= 0 || (uint32_t)max > EP_MAX_EVENTS) {
        return -EINVAL;
    }
    if (!user_range_ok(uevents, (uint32_t)max * EP_EVENT_SIZE)) {
        return -EFAULT;
    }
    sc.ep = ep;
    sc.max = (uint32_t)max < ep->count + 1u ? (uint32_t)max : ep->count + 1u;
    sc.out = kmalloc(sizeof(struct epoll_out) * sc.max);
    sc.queues = ep->count + 1u;
    sc.n = 0;
    if (!sc.out) {
        return -ENOMEM;
    }
    rc = poll_wait(scan_set, &sc, sc.queues, has_deadline, deadline);
    for (i = 0; rc > 0 && i < sc.n; i++) {
        if (copy_to_user((void __user *)(uevents + i * EP_EVENT_SIZE), &sc.out[i], EP_EVENT_SIZE) !=
            0) {
            rc = -EFAULT;
        }
    }
    kfree(sc.out);
    return rc;
}

int32_t sys_epoll_wait(struct isr_params *params)
{
    int has_deadline;
    uint32_t deadline = poll_ms_deadline((int)params->esi, &has_deadline);

    return epoll_wait_common((int)params->ebx, params->ecx, (int)params->edx, has_deadline,
                             deadline);
}

int32_t sys_epoll_pwait(struct isr_params *params)
{
    int has_deadline;
    uint32_t deadline = poll_ms_deadline((int)params->esi, &has_deadline);
    int32_t rc = poll_mask_set(params->edi, params->ebp);

    if (rc != 0) {
        return rc;
    }
    return poll_mask_done(
        epoll_wait_common((int)params->ebx, params->ecx, (int)params->edx, has_deadline, deadline));
}

/**
 * epoll_pwait2(2): epoll_pwait with a struct __kernel_timespec timeout, 64-bit
 * seconds and nanoseconds, or none for a null pointer. The seconds are one
 * signed 64-bit value, low word first; a negative one is EINVAL, and a
 * timeout past 2147483647 milliseconds, about 24.8 days, is shortened to it.
 */
int32_t sys_epoll_pwait2(struct isr_params *params)
{
    int has_deadline = 0;
    uint32_t deadline = 0;
    int32_t rc;

    if (params->esi) {
        uint32_t ts[4]; /* struct __kernel_timespec: 64-bit seconds and nanoseconds */
        int64_t sec;
        uint64_t ms;

        if (copy_from_user(ts, (const void __user *)params->esi, sizeof(ts)) != 0) {
            return -EFAULT;
        }
        sec = (int64_t)(((uint64_t)ts[1] << 32) | ts[0]);
        if (sec < 0 || ts[3] != 0 || ts[2] >= 1000000000u) {
            return -EINVAL;
        }
        ms = 0x7FFFFFFFu;
        if (sec < 0x7FFFFFFF / 1000) {
            ms = (uint64_t)sec * 1000u + (ts[2] + 999999u) / 1000000u;
        }
        if (ms > 0x7FFFFFFFu) {
            ms = 0x7FFFFFFFu;
        }
        deadline = poll_ms_deadline((int)ms, &has_deadline);
    }
    rc = poll_mask_set(params->edi, params->ebp);
    if (rc != 0) {
        return rc;
    }
    return poll_mask_done(
        epoll_wait_common((int)params->ebx, params->ecx, (int)params->edx, has_deadline, deadline));
}
