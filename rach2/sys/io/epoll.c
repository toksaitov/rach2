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
#include "../lib/kirq.h"

#define EPOLL_CLOEXEC 02000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN        0x001u
#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLRDNORM    0x040u
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

/* Policy: how deep a scan looks into sets inside sets, Linux's
 * EP_MAX_NESTS; it bounds the kernel stack a scan takes. */
#define EP_MAX_NESTS 4

struct epitem {
    struct epitem *next;
    /* The registration's id, from `ep_ids`; EPOLL_CTL_MOD gives another. */
    uint32_t id;
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
    /* The scan that last looked into the set, from `scan_marks`, and what
     * it found: whether a wait would report an event, and the sum of the
     * wakes of the queues the set watches, its own among them. */
    uint32_t mark;
    int ready;
    uint32_t sig;
};

/* The queues a scan finds, which a wait must be on: where they go, as many
 * as `room`, and how many were found, which may be more. */
struct qlist {
    struct wait_queue **wqs;
    uint32_t room;
    uint32_t n;
};

/* An event a wait took: the i386 struct epoll_event it goes out as, the
 * events and the data word, and the registration it came from by its id,
 * with what the report changed in it, which a failed copy puts back. */
struct epoll_out {
    uint32_t user[3];
    uint32_t id;
    int off;
    uint32_t seen;
    const struct wait_queue *at;
    uint32_t wakes;
};

/* The ids registrations have had, which tell a registration from a later
 * one at the same address; and the scans that have looked into sets, which
 * tell a set looked into by the scan in progress (`set_ready`). */
static uint32_t ep_ids;
static uint32_t scan_marks;

static struct epoll *epoll_of(int fd);
static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq);
static int set_ready(struct epoll *ep, int depth, struct qlist *q);

static void q_add(struct qlist *q, struct wait_queue *wq)
{
    if (wq) {
        if (q->n < q->room) {
            q->wqs[q->n] = wq;
        }
        q->n++;
    }
}

/* Whether the descriptor `e` is an epoll set. */
static int is_set(const fd_entry_t *e)
{
    return e->ops && e->ops->poll == epoll_poll;
}

/* Whether the descriptor `e` is the open file the registration `it` was
 * made for. */
static int item_names(const struct epitem *it, const fd_entry_t *e)
{
    return e && e->ofd == it->ofd && e->ofd->serial == it->serial;
}

/* Whether the descriptor of an edge-triggered registration, whose wait
 * queue is `wq` with `wakes` counted, has changed since `it` was last
 * reported. */
static int item_woken(const struct epitem *it, const struct wait_queue *wq, uint32_t wakes)
{
    return wq != it->at || wakes != it->wakes;
}

/*
 * The events among `want` of the descriptor `e`, its wait queue in `*wq`,
 * and in `*wakes` the wakes the queue has counted, which tell a change. An
 * epoll set, looked into at `depth`, is readable while a wait on it would
 * report an event, and its count is the sum over the queues it watches, so
 * that a change in any of them is one of the set. The queues go on `q`.
 */
static uint32_t target_poll(fd_entry_t *e, uint32_t want, int depth, struct qlist *q,
                            struct wait_queue **wq, uint32_t *wakes)
{
    uint32_t ready;

    if (is_set(e)) {
        struct epoll *inner = (struct epoll *)e->priv;

        ready = set_ready(inner, depth + 1, q) ? EPOLLIN | EPOLLRDNORM : 0u;
        *wq = &inner->wq;
        *wakes = inner->sig;
        return ready & want;
    }
    ready = (uint32_t)(uint16_t)poll_fd(e, (short)want, wq) & want;
    q_add(q, *wq);
    *wakes = *wq ? (*wq)->wakes : 0u;
    return ready;
}

/**
 * The events of `it` a wait would report now, or 0, taking none; the wait
 * queue of its descriptor in `*wq`, with its count of wakes in `*wakes`,
 * and the queues a wait must be on in `q` (`target_poll`), for a set
 * inside the one scanned at `depth`. EPOLLERR and EPOLLHUP are watched
 * whatever the registration asked, none included, and nothing is while a
 * one-shot registration is off. A registration whose number names another
 * open file, or none, is marked for removal with `*gone`.
 *
 * An edge-triggered registration is reported when an event is ready that
 * was not at its last report, or has gone since, or when its descriptor's
 * queue has been woken since that report: a pipe drained and filled again
 * between two waits is ready as it was, and its reader's queue is what
 * tells the new edge, as Linux's wakeup callback does.
 */
static uint32_t item_ready(struct epitem *it, int depth, struct qlist *q, struct wait_queue **wq,
                           uint32_t *wakes, int *gone)
{
    fd_entry_t *e = fd_get(current_proc->fds, it->fd);
    uint32_t want = (it->events & EPOLL_EVENTS) | EPOLLERR | EPOLLHUP;
    uint32_t ready;

    *wq = NULL;
    *wakes = 0;
    *gone = !item_names(it, e);
    if (*gone || it->off) {
        return 0;
    }
    ready = target_poll(e, want, depth, q, wq, wakes);
    if (it->events & EPOLLET) {
        it->seen &= ready;
        if (!(ready & ~it->seen) && !item_woken(it, *wq, *wakes)) {
            ready = 0;
        }
    }
    return ready;
}

/* Record the report of `ready` for `it`, whose descriptor's wait queue is
 * `wq` with `wakes` counted: what an edge-triggered registration is not
 * reported again for, and the end of a one-shot one. */
static void item_reported(struct epitem *it, uint32_t ready, const struct wait_queue *wq,
                          uint32_t wakes)
{
    if (it->events & EPOLLET) {
        it->seen = ready;
        it->at = wq;
        it->wakes = wakes;
    }
    if (it->events & EPOLLONESHOT) {
        it->off = 1;
    }
}

/* A wait's view of the set: where the ready events go, and how many fit;
 * the queues it has records for; and the queues the last scan found, when
 * they were more, with nothing ready, for the wait to begin again with. */
struct epoll_scan {
    struct epoll *ep;
    struct epoll_out *out;
    uint32_t max;
    uint32_t n;
    uint32_t queues;
    uint32_t grow;
};

static void drop_item(struct epoll *ep, struct epitem **link)
{
    struct epitem *it = *link;

    *link = it->next;
    ep->count--;
    kfree(it);
}

/* Fill `o` with the event `ready` of `it`, and what its report is about to
 * change in `it`. */
static void took(struct epoll_out *o, const struct epitem *it, uint32_t ready)
{
    o->user[0] = ready;
    o->user[1] = it->data[0];
    o->user[2] = it->data[1];
    o->id = it->id;
    o->off = it->off;
    o->seen = it->seen;
    o->at = it->at;
    o->wakes = it->wakes;
}

/* Put back what the report of `o` changed in its registration, when the
 * registration is in `ep` still as it was: the event goes to a later
 * wait. */
static void untook(struct epoll *ep, const struct epoll_out *o)
{
    struct epitem *it;

    for (it = ep->items; it && it->id != o->id; it = it->next) {
    }
    if (it) {
        it->off = o->off;
        it->seen = o->seen;
        it->at = o->at;
        it->wakes = o->wakes;
    }
}

/**
 * Whether a wait on the set `ep`, looked into at `depth`, would report an
 * event, taking none; the queues a wait on it must be on, its own and
 * those of what it watches, go on `q`, and the sum of their wakes to
 * `ep->sig`.
 *
 * A scan looks into a set once: a set it meets again, by another path or
 * inside itself, gives what the first look found, or nothing while that
 * look is not done, so a cycle ends, and a set deeper than EP_MAX_NESTS is
 * not looked into and gives nothing. `scan_marks` counts the scans.
 */
static int set_ready(struct epoll *ep, int depth, struct qlist *q)
{
    struct epitem *it;

    if (ep->mark == scan_marks) {
        return ep->ready;
    }
    ep->mark = scan_marks;
    ep->ready = 0;
    ep->sig = ep->wq.wakes;
    q_add(q, &ep->wq);
    if (depth > EP_MAX_NESTS) {
        return 0;
    }
    for (it = ep->items; it; it = it->next) {
        struct wait_queue *wq;
        uint32_t wakes;
        int gone;

        if (item_ready(it, depth, q, &wq, &wakes, &gone)) {
            ep->ready = 1;
        }
        ep->sig += wakes;
    }
    return ep->ready;
}

/*
 * The scan of a wait: the events of the set's registrations into the wait's
 * array, at most `max`, each taken as it is reported, and the queues to wait
 * on while there are none, those of the sets inside it and of what they
 * watch among them (`set_ready`).
 *
 * A reported registration moves to the end of the list, so a wait with
 * room for fewer events than are ready is followed by one that reports the
 * others, as Linux's waits take the ready descriptors in turn. A set that
 * another thread grew while the wait slept has more queues than the wait
 * has records for; with nothing ready, the scan says so in `grow`, and the
 * wait begins again with a record for each.
 */
static int scan_set(void *ctx, struct wait_queue **wqs)
{
    struct epoll_scan *sc = (struct epoll_scan *)ctx;
    struct epitem **link = &sc->ep->items;
    struct epitem *reported = NULL;
    struct epitem **tail = &reported;
    struct epitem *it;
    struct qlist q;

    q.wqs = wqs;
    q.room = sc->queues;
    q.n = 0;
    sc->n = 0;
    sc->ep->mark = ++scan_marks;
    sc->ep->ready = 0;
    q_add(&q, &sc->ep->wq);
    while ((it = *link) != NULL && sc->n < sc->max) {
        struct wait_queue *wq;
        uint32_t ready, wakes;
        int gone;

        ready = item_ready(it, 0, &q, &wq, &wakes, &gone);
        if (gone) {
            drop_item(sc->ep, link);
            continue;
        }
        if (!ready) {
            link = &it->next;
            continue;
        }
        took(&sc->out[sc->n++], it, ready);
        item_reported(it, ready, wq, wakes);
        *link = it->next;
        it->next = NULL;
        *tail = it;
        tail = &it->next;
    }
    while (*link) {
        link = &(*link)->next;
    }
    *link = reported;
    if (sc->n == 0 && q.n > sc->queues) {
        sc->grow = q.n;
        return 1;
    }
    return (int)sc->n;
}

/*
 * The readiness of an epoll set for poll: readable when a wait would report
 * an event, which `set_ready` says without taking it, so an edge-triggered
 * event reported already, or a one-shot registration that is off, does not
 * make the set readable. The queue named is the set's own; poll's scan
 * takes all a set's queues from `epoll_poll_set`.
 */
static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct epoll *ep = (struct epoll *)e->priv;
    struct qlist none;
    uint32_t irq = kirq_save();
    int ready;

    none.wqs = NULL;
    none.room = 0;
    none.n = 0;
    scan_marks++;
    ready = set_ready(ep, 0, &none);
    kirq_restore(irq);
    if (wq) {
        *wq = &ep->wq;
    }
    return ready ? (short)((uint32_t)events & (EPOLLIN | EPOLLRDNORM)) : 0;
}

int epoll_is_set(const fd_entry_t *e)
{
    return is_set(e);
}

short epoll_poll_set(fd_entry_t *e, short events, struct wait_queue **wqs, uint32_t room,
                     uint32_t *n)
{
    struct epoll *ep = (struct epoll *)e->priv;
    struct qlist q;
    int ready;

    q.wqs = wqs;
    q.room = room;
    q.n = 0;
    scan_marks++;
    ready = set_ready(ep, 0, &q);
    *n = q.n;
    return ready ? (short)((uint32_t)events & (EPOLLIN | EPOLLRDNORM)) : 0;
}

/*
 * Whether the set `inner`, which sets `depth` deep hold, reaches the set
 * `ep` through the sets it watches, or holds sets deeper than
 * EP_MAX_NESTS: adding it to `ep` would make a cycle, or nesting a scan
 * does not look into, which Linux refuses with ELOOP. Each set is looked
 * into once, as Linux's check does.
 */
static int set_loops(struct epoll *inner, const struct epoll *ep, int depth)
{
    struct epitem *it;

    if (inner == ep || depth > EP_MAX_NESTS) {
        return 1;
    }
    if (inner->mark == scan_marks) {
        return 0;
    }
    inner->mark = scan_marks;
    inner->ready = 0;
    for (it = inner->items; it; it = it->next) {
        fd_entry_t *e = fd_get(current_proc->fds, it->fd);

        if (item_names(it, e) && is_set(e) && set_loops((struct epoll *)e->priv, ep, depth + 1)) {
            return 1;
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

/* Whether adding the set `inner` to `ep` would make a cycle or nesting too
 * deep (`set_loops`). */
static int nests_badly(struct epoll *inner, const struct epoll *ep)
{
    uint32_t irq = kirq_save();
    int loops;

    scan_marks++;
    loops = set_loops(inner, ep, 1);
    kirq_restore(irq);
    return loops;
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
    if (!ep || fd == (int)params->ebx || (is_set(target) && target->priv == ep)) {
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
        if (is_set(target) && nests_badly((struct epoll *)target->priv, ep)) {
            return -ELOOP;
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
    it->id = ++ep_ids;
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

/**
 * A wait on the set `epfd` for at most `max` events into the user array
 * `uevents`, until the tick `deadline` when `has_deadline`.
 *
 * The scan takes each event it reports, so that a second waiter does not
 * report an edge-triggered or one-shot event again, and the copy to the
 * caller comes after the wait, and can sleep in a page fault. An event that
 * cannot be copied is put back in its registration (`untook`), unless the
 * registration has gone or changed meanwhile, and the wait returns the
 * events copied before it, or EFAULT when there are none, as Linux's
 * does.
 */
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
    sc.grow = ep->count + 1u;
    for (;;) {
        sc.max = (uint32_t)max < ep->count + 1u ? (uint32_t)max : ep->count + 1u;
        sc.out = kmalloc(sizeof(struct epoll_out) * sc.max);
        if (!sc.out) {
            return -ENOMEM;
        }
        sc.queues = sc.grow;
        sc.grow = 0;
        sc.n = 0;
        rc = poll_wait(scan_set, &sc, sc.queues, has_deadline, deadline);
        if (rc <= 0 || sc.grow == 0) {
            break;
        }
        kfree(sc.out);
    }
    for (i = 0; rc > 0 && i < sc.n; i++) {
        if (copy_to_user((void __user *)(uevents + i * EP_EVENT_SIZE), sc.out[i].user,
                         EP_EVENT_SIZE) != 0) {
            rc = i > 0 ? (int32_t)i : -EFAULT;
            while (i < sc.n) {
                untook(ep, &sc.out[i++]);
            }
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
        /* struct __kernel_timespec: 64-bit seconds and nanoseconds. */
        uint32_t ts[4];
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
