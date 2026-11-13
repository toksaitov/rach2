/*
 * epoll.c: epoll sets, as epoll.h describes them, in the shape of Linux's
 * fs/eventpoll.c: each registration keeps a record on the wait queue of
 * what it watches, whose wake puts it on the set's ready list, and a wait
 * looks only at that list.
 *
 * The locks, outermost first:
 * `ep_nest_mtx`, over the sets inside others (`holders`, `subs`), as
 * Linux's `epnested_mutex` (fs/eventpoll.c:392); a set's `mtx`, over its
 * registrations, their events and data, and the polls of a wait, as
 * `ep->mtx`; `ep_files_lock`, over each description's list of the
 * registrations made of it, as Linux's `file->f_lock` over `f_ep`; and a
 * set's `lock`, a spinlock taken with interrupts off, over its ready list
 * and the state of each registration on it, as `ep->lock`. A wake takes
 * the queue's lock and then the set's `lock`. The big lock, where a poll
 * still needs it, comes before all of them.
 */

#include "epoll.h"

#include "anon.h"
#include "poll.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/mutex.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"

#define EPOLL_CLOEXEC 02000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN        0x001u
#define EPOLLPRI       0x002u
#define EPOLLOUT       0x004u
#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLRDNORM    0x040u
#define EPOLLRDBAND    0x080u
#define EPOLLWRNORM    0x100u
#define EPOLLWRBAND    0x200u
#define EPOLLMSG       0x400u
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLET        (1u << 31)

/* The events a registration may watch; the rest are how it is reported. */
#define EPOLL_EVENTS 0x0000FFFFu

/* The events an EPOLLEXCLUSIVE registration may have, Linux's
 * EPOLLEXCLUSIVE_OK_BITS (fs/eventpoll.c:216). */
#define EP_EXCLUSIVE_OK \
    (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLWAKEUP | EPOLLET | EPOLLEXCLUSIVE)

/* Format: struct epoll_event, packed on every architecture: the events,
 * then the data. */
#define EP_EVENT_SIZE 12u

/* Policy: the events one wait returns at most, as Linux bounds maxevents. */
#define EP_MAX_EVENTS (0x7FFFFFFF / EP_EVENT_SIZE)

/* Policy: how many sets deep a set may hold others, and a poll looks into
 * them, Linux's EP_MAX_NESTS; it bounds the kernel stack a poll takes. */
#define EP_MAX_NESTS 4

/* Where a registration is, under its set's `lock`: on no list, on the
 * set's ready list, or on the batch a wait or a poll of the set took from
 * that list and is looking at. */
#define EPI_IDLE  0
#define EPI_READY 1
#define EPI_BATCH 2

/* A registration. */
struct epitem {
    /* The set's list of registrations, under the set's `mtx`. */
    struct epitem *next;
    struct epoll *ep;
    int fd;
    /* The description registered, and a copy of the descriptor's entry
     * the add found, through which the registration is polled: what it
     * names lasts until the description's last close, which takes the
     * registration first (`epoll_release_file`). */
    struct open_file *ofd;
    fd_entry_t target;
    /* The events, with EPOLLERR and EPOLLHUP, and the flags; a one-shot
     * registration that has reported keeps its flags alone. Written under
     * the set's `mtx` and `lock`, read by a wake under `lock`. */
    uint32_t events;
    uint32_t data[2];
    /* Whether a poll of the target needs the big lock
     * (`file_poll_unlocked`). */
    int big;
    /* Its record on the target's wait queue, whose wake is `ep_wake`; its
     * `wq` is NULL while it is on none. */
    struct wq_waiter wait;
    /* Under the set's `lock`: EPI_IDLE, EPI_READY or EPI_BATCH, the links
     * of the ready list or the batch, and whether a wake came while it
     * was on a batch, which puts it back on the list after. */
    int rdstate;
    int again;
    struct epitem *rdnext;
    struct epitem *rdprev;
    /* The description's list of registrations made of it, and whether its
     * release has taken this one, under `ep_files_lock`. */
    struct epitem *fnext;
    struct epitem **fprev;
    int dying;
    /* For a registration of a set, under `ep_nest_mtx`: that set, the next
     * registration of it in any set (its `holders`), and the next
     * registration of a set in this one's set (its `subs`). */
    struct epoll *set;
    struct epitem *hold_next;
    struct epitem *sub_next;
};

struct epoll {
    struct anon_obj anon;
    /* Over `items`, `count` and each registration's events and data, and
     * held by a wait or a poll of the set while it polls registrations, as
     * Linux's `ep->mtx`; an inner set's is taken under its outer set's at
     * the depth it is looked into. */
    struct mutex mtx;
    struct epitem *items;
    uint32_t count;
    /* Over the ready list, `rdbig` and each registration's `rdstate`,
     * `again` and links, as Linux's `ep->lock`; taken by a wake with
     * interrupts off. */
    struct spinlock lock;
    struct epitem *rdhead;
    struct epitem *rdtail;
    /* The registrations on the ready list whose poll needs the big lock. */
    uint32_t rdbig;
    /* The sleepers of epoll_wait, as Linux's `ep->wq`, and the pollers of
     * the set, poll's and select's records and the registrations of the
     * set in other sets, as `ep->poll_wait`. */
    wait_queue_t wq;
    wait_queue_t poll_wait;
    /* The set's description, 1, and each registration in it, as Linux's
     * `ep->refcount`: the last of them frees the set. Changed by an atomic
     * step. */
    volatile unsigned int refs;
    /* Under `ep_nest_mtx`: the registrations of this set in the sets it is
     * in, the registrations of sets in it, and what a nesting check found,
     * the check's mark and the longest chain of sets below or above it
     * (`sets_below`, `sets_above`). */
    struct epitem *holders;
    struct epitem *subs;
    uint32_t walk;
    int height;
};

/* The nesting checks, which tell a set looked into by the check in
 * progress, under `ep_nest_mtx`. */
static uint32_t walk_marks;
static struct mutex ep_nest_mtx = {0,
                                   -1,
                                   0,
                                   {0}
#ifdef RACH2_LOCKDEP
                                   ,
                                   LOCKDEP_MAP_STATIC("epnested_mutex")
#endif
};
static struct spinlock ep_files_lock = SPINLOCK_INIT_NAMED("ep_files_lock");

static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq);
static void epoll_close(fd_entry_t *e);

static const struct file_ops epoll_ops = {NULL,        NULL, epoll_poll, NULL,
                                          epoll_close, NULL, NULL,       FILE_OPS_UNLOCKED};

/** Whether the descriptor `e` is an epoll set. */
static int is_set(const fd_entry_t *e)
{
    return e->ops == &epoll_ops;
}

/** Add `delta` to the set's references by an atomic step: the count after. */
static unsigned int refs_add(struct epoll *ep, int delta)
{
    unsigned int old;

    do {
        old = ep->refs;
    } while (arch_atomic_cmpxchg(&ep->refs, old, old + (unsigned int)delta) != old);
    return old + (unsigned int)delta;
}

/** Drop one reference to `ep`, and free it with the last. */
static void ep_put(struct epoll *ep)
{
    if (refs_add(ep, -1) == 0) {
        kfree(ep);
    }
}

/** The kinds of change (`wq_wake`) among the events `want`: data for the
 * events of input, room for those of output, and a hangup, which every
 * registration watches with EPOLLERR and EPOLLHUP. A wake of no kind
 * reaches no registration, as a wake whose key holds none of a
 * registration's events does not in Linux (fs/eventpoll.c:1457). */
static uint32_t kinds_of(uint32_t want)
{
    uint32_t kinds = WQ_HANGUP;

    if (want & (EPOLLIN | EPOLLPRI | EPOLLRDNORM | EPOLLRDBAND | EPOLLMSG)) {
        kinds |= WQ_DATA;
    }
    if (want & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
        kinds |= WQ_ROOM;
    }
    return kinds;
}

/** Put `it` on the ready list of `ep`, at its tail or its head; the set's
 * `lock` is held. */
static void rd_link(struct epoll *ep, struct epitem *it, int at_head)
{
    if (at_head) {
        it->rdprev = NULL;
        it->rdnext = ep->rdhead;
        if (ep->rdhead) {
            ep->rdhead->rdprev = it;
        } else {
            ep->rdtail = it;
        }
        ep->rdhead = it;
    } else {
        it->rdnext = NULL;
        it->rdprev = ep->rdtail;
        if (ep->rdtail) {
            ep->rdtail->rdnext = it;
        } else {
            ep->rdhead = it;
        }
        ep->rdtail = it;
    }
    it->rdstate = EPI_READY;
    if (it->big) {
        ep->rdbig++;
    }
}

/** Take `it` off the ready list of `ep`; the set's `lock` is held. */
static void rd_unlink(struct epoll *ep, struct epitem *it)
{
    if (it->rdprev) {
        it->rdprev->rdnext = it->rdnext;
    } else {
        ep->rdhead = it->rdnext;
    }
    if (it->rdnext) {
        it->rdnext->rdprev = it->rdprev;
    } else {
        ep->rdtail = it->rdprev;
    }
    it->rdnext = it->rdprev = NULL;
    it->rdstate = EPI_IDLE;
    if (it->big) {
        ep->rdbig--;
    }
}

/**
 * Wake `wq`, a queue of a set, for data, when a waiter is on it, read after
 * a full barrier, as Linux's `wq_has_sleeper` (include/linux/wait.h:
 * 143-162): a waiter files itself on the queue before it looks at the
 * ready list, with a barrier between (`poll_wait_queue`, `poll_wait`), so
 * that it sees the registration this caller put there or this caller sees
 * its record.
 */
static void ep_wake_if(wait_queue_t *wq)
{
    sched_mb();
    if (wq_active(wq)) {
        wq_wake(wq, WQ_DATA);
    }
}

/**
 * The wake of a queue a registration is on, as Linux's `ep_poll_callback`
 * (fs/eventpoll.c:1429-1534), under that queue's lock with interrupts off,
 * at the nesting `nest` of the set's wake (wait_queue.h). A wake of a kind
 * the registration watches puts it on the set's ready list, or, while a
 * wait has it on a batch, marks it to go back, and wakes one sleeper of
 * epoll_wait and every poller of the set. A one-shot registration that has
 * reported, and a wake of no kind the registration watches, change
 * nothing. The test for waiters follows a full barrier, as in `ep_wake_if`.
 * Returns whether the wake counts as woken for the queue's turn of
 * exclusive records: for an EPOLLEXCLUSIVE registration, when it woke a
 * sleeper of the set for an event it watches, as Linux's `ewake`.
 */
static int ep_wake(struct wq_waiter *w, uint32_t kinds, unsigned int nest, uint64_t *cpus)
{
    struct epitem *it = (struct epitem *)((char *)w - (char *)&((struct epitem *)0)->wait);
    struct epoll *ep = it->ep;
    uint32_t ev;
    int ewake = 0, pwake = 0;

    if (kinds & WQ_FREE) {
        return 0;
    }
    spin_lock_nested(&ep->lock, nest);
    ev = it->events;
    if (!(ev & EPOLL_EVENTS) || !(kinds & kinds_of(ev))) {
        spin_unlock(&ep->lock);
        return (ev & EPOLLEXCLUSIVE) ? 0 : 1;
    }
    if (it->rdstate == EPI_BATCH) {
        it->again = 1;
    } else if (it->rdstate == EPI_IDLE) {
        rd_link(ep, it, 0);
    }
    sched_mb();
    if (wq_active(&ep->wq)) {
        if (ev & EPOLLEXCLUSIVE) {
            uint32_t io = kinds & (WQ_DATA | WQ_ROOM);

            ewake = io == WQ_DATA   ? (ev & EPOLLIN) != 0
                    : io == WQ_ROOM ? (ev & EPOLLOUT) != 0
                                    : io == 0;
        }
        wq_wake_nested(&ep->wq, WQ_DATA, nest, cpus);
    }
    pwake = wq_active(&ep->poll_wait);
    spin_unlock(&ep->lock);
    if (pwake) {
        wq_wake_nested(&ep->poll_wait, WQ_DATA, nest + 1u, cpus);
    }
    return (ev & EPOLLEXCLUSIVE) ? ewake : 1;
}

static int set_poll(struct epoll *ep, int depth);

/**
 * The events of `it` among those it watches, polled now, as Linux's
 * `ep_item_poll` (fs/eventpoll.c:1219-1239): a set inside is readable while
 * a wait on it would report an event (`set_poll`), looked into at `depth`
 * plus one; any other target is polled through the registration's copy of
 * its descriptor. The queue the poll names goes to `*wq` when `wq` is not
 * NULL. The caller holds the set's `mtx`, and the big lock when `it->big`.
 */
static uint32_t item_poll(struct epitem *it, int depth, struct wait_queue **wq)
{
    uint32_t want = it->events & EPOLL_EVENTS;
    struct wait_queue *q = NULL;
    uint32_t ready;

    if (it->set) {
        ready = set_poll(it->set, depth + 1) ? EPOLLIN | EPOLLRDNORM : 0u;
        q = &it->set->poll_wait;
    } else {
        ready = (uint32_t)(uint16_t)poll_fd_held(&it->target, (short)want, &q);
    }
    if (wq) {
        *wq = q;
    }
    return ready & want;
}

/* A batch a wait or a poll of a set took from the ready list
 * (`batch_take`): the registrations not yet looked at, in order, and those
 * to go back to the tail after the look and to be dropped from the list,
 * linked through `rdnext`. */
struct ep_batch {
    struct epitem *todo;
    struct epitem *tail;
    struct epitem **tail_end;
    struct epitem *done;
};

/** Take the whole ready list of `ep` as `b`, each registration marked as
 * on a batch, as Linux's `ep_start_scan` (fs/eventpoll.c:893-912). */
static void batch_take(struct epoll *ep, struct ep_batch *b)
{
    uint32_t irq = spin_lock_irqsave(&ep->lock);
    struct epitem *it;

    b->todo = ep->rdhead;
    b->tail = NULL;
    b->tail_end = &b->tail;
    b->done = NULL;
    for (it = ep->rdhead; it; it = it->rdnext) {
        it->rdstate = EPI_BATCH;
        it->again = 0;
    }
    ep->rdhead = ep->rdtail = NULL;
    ep->rdbig = 0;
    spin_unlock_irqrestore(&ep->lock, irq);
}

/** The next registration of `b` to look at, taken off it, or NULL. */
static struct epitem *batch_next(struct ep_batch *b)
{
    struct epitem *it = b->todo;

    if (it) {
        b->todo = it->rdnext;
    }
    return it;
}

/** Put `it`, taken from `b`, back before the rest of it, to be looked at
 * by a later wait. */
static void batch_unget(struct ep_batch *b, struct epitem *it)
{
    it->rdnext = b->todo;
    b->todo = it;
}

/** Mark `it`, taken from `b`, to go to the tail of the ready list, or with
 * `keep` 0 to leave it unless a wake came meanwhile. */
static void batch_done(struct ep_batch *b, struct epitem *it, int keep)
{
    if (keep) {
        it->rdnext = NULL;
        *b->tail_end = it;
        b->tail_end = &it->rdnext;
    } else {
        it->rdnext = b->done;
        b->done = it;
    }
}

/**
 * End the batch `b` of `ep`, as Linux's `ep_done_scan` (fs/eventpoll.c:
 * 914-962): the registrations not looked at go back to the head of the
 * ready list, in order, those to be kept to its tail, and those dropped
 * leave it, but for any a wake marked meanwhile, which go to the tail. The
 * sleepers are woken again when the list is not empty, since a wake that
 * came while the batch was out found nothing new to put on it.
 */
static void batch_end(struct epoll *ep, struct ep_batch *b)
{
    struct epitem *it, *next, *rev = NULL;
    uint32_t irq = spin_lock_irqsave(&ep->lock);
    int wake;

    for (it = b->todo; it; it = next) {
        next = it->rdnext;
        it->rdnext = rev;
        rev = it;
    }
    for (it = rev; it; it = next) {
        next = it->rdnext;
        rd_link(ep, it, 1);
    }
    for (it = b->tail; it; it = next) {
        next = it->rdnext;
        rd_link(ep, it, 0);
    }
    for (it = b->done; it; it = next) {
        next = it->rdnext;
        if (it->again) {
            rd_link(ep, it, 0);
        } else {
            it->rdnext = NULL;
            it->rdstate = EPI_IDLE;
        }
    }
    wake = ep->rdhead != NULL;
    spin_unlock_irqrestore(&ep->lock, irq);
    if (wake) {
        ep_wake_if(&ep->wq);
    }
}

/**
 * Whether a wait on the set `ep`, looked into at `depth`, would report an
 * event, taking none, as Linux's `__ep_eventpoll_poll` (fs/eventpoll.c:
 * 1176-1212): the registrations on its ready list are polled in turn
 * until one is ready, and each that is not leaves the list. A set deeper
 * than EP_MAX_NESTS gives nothing. A registration whose poll needs the big
 * lock, met without it, counts as ready, since its poll cannot be asked
 * under the set's `mtx`; a wait on the set then asks it. The list is read
 * only under the `mtx`, as Linux reads it (fs/eventpoll.c:1191-1192): a
 * wait on the set holds the `mtx` while its batch has the list, and an
 * outer set that found the list empty meanwhile would drop its
 * registration of this set, which the batch's end, waking only this set's
 * sleepers, would not put back. Sleeps, for the set's `mtx`.
 */
static int set_poll(struct epoll *ep, int depth)
{
    struct ep_batch b;
    struct epitem *it;
    int ready = 0;

    if (depth > EP_MAX_NESTS) {
        return 0;
    }
    mutex_lock_nested(&ep->mtx, (unsigned int)depth);
    batch_take(ep, &b);
    while (!ready && (it = batch_next(&b)) != NULL) {
        if ((it->big && !bkl_held()) || item_poll(it, depth, NULL)) {
            ready = 1;
            batch_unget(&b, it);
        } else {
            batch_done(&b, it, 0);
        }
    }
    batch_end(ep, &b);
    mutex_unlock(&ep->mtx);
    return ready;
}

/**
 * Report the events of the ready registrations of `ep`, at most `max`, to
 * the user array `uevents`, as Linux's `ep_send_events` and
 * `ep_deliver_event` (fs/eventpoll.c:1995-2101): the ready list is taken
 * as a batch under the set's `mtx`, each registration on it is polled
 * again, and one with an event is copied out at once. A level-triggered
 * registration goes back to the tail of the list, so the next wait asks it
 * again and waits with room for fewer events than are ready take them in
 * turn; an edge-triggered one leaves it until a wake puts it back, and a
 * one-shot one keeps its flags alone until EPOLL_CTL_MOD. A registration
 * whose copy faults goes back to the head of the list, and the call
 * returns the events copied before it, or -EFAULT when none.
 *
 * A registration whose poll needs the big lock is polled under it, taken
 * before the set's `mtx` when the list holds one; one met without it, put
 * there meanwhile, is passed by, and when nothing else was reported the
 * batch is taken again under the big lock. Returns the events copied.
 */
static int32_t send_events(struct epoll *ep, uintptr_t uevents, uint32_t max)
{
    int force_big = 0;

    for (;;) {
        struct ep_batch b;
        struct epitem *it;
        int32_t res = 0;
        int big = 0, passed = 0;

        if ((force_big || ep->rdbig) && !bkl_held()) {
            bkl_enter();
            big = 1;
        }
        mutex_lock(&ep->mtx);
        batch_take(ep, &b);
        while ((uint32_t)res < max && (it = batch_next(&b)) != NULL) {
            uint32_t ready, user[3];

            if (it->big && !bkl_held()) {
                passed = 1;
                batch_done(&b, it, 1);
                continue;
            }
            ready = item_poll(it, 0, NULL);
            if (!ready) {
                batch_done(&b, it, 0);
                continue;
            }
            user[0] = ready;
            user[1] = it->data[0];
            user[2] = it->data[1];
            if (copy_to_user((void __user *)(uevents + (uint32_t)res * EP_EVENT_SIZE), user,
                             EP_EVENT_SIZE) != 0) {
                batch_unget(&b, it);
                if (res == 0) {
                    res = -EFAULT;
                }
                break;
            }
            res++;
            if (it->events & EPOLLONESHOT) {
                uint32_t irq = spin_lock_irqsave(&ep->lock);

                it->events &= ~EPOLL_EVENTS;
                spin_unlock_irqrestore(&ep->lock, irq);
                batch_done(&b, it, 0);
            } else {
                batch_done(&b, it, !(it->events & EPOLLET));
            }
        }
        batch_end(ep, &b);
        mutex_unlock(&ep->mtx);
        if (big) {
            bkl_exit();
        }
        if (res == 0 && passed && !force_big && !bkl_held()) {
            force_big = 1;
            continue;
        }
        return res;
    }
}

/**
 * The readiness of an epoll set for poll, select and a set it is in:
 * readable when a wait would report an event (`set_poll`), so an
 * edge-triggered event reported already, or a one-shot registration that
 * has reported, does not make the set readable. The queue named is the
 * set's poll queue, which a registration's wake wakes when it puts the
 * registration on the ready list (`ep_wake`).
 */
static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct epoll *ep = (struct epoll *)e->priv;

    if (wq) {
        *wq = &ep->poll_wait;
    }
    return set_poll(ep, 0) ? (short)((uint32_t)events & (EPOLLIN | EPOLLRDNORM)) : 0;
}

int epoll_is_set(const fd_entry_t *e)
{
    return is_set(e);
}

int32_t epoll_target(const fd_entry_t *e, int tfd, uint32_t toff, const struct open_file **ofd)
{
    struct epoll *ep;
    const struct epitem *it;
    int32_t rc = -ENOENT;

    if (!e || !is_set(e)) {
        return -EINVAL;
    }
    ep = (struct epoll *)e->priv;
    mutex_lock(&ep->mtx);
    for (it = ep->items; it; it = it->next) {
        if (it->fd != tfd) {
            continue;
        }
        if (toff-- == 0) {
            *ofd = it->ofd;
            rc = 0;
            break;
        }
    }
    mutex_unlock(&ep->mtx);
    return rc;
}

/** Take the registration `it` of a set off that set's `holders`; under
 * `ep_nest_mtx`. */
static void unhold(struct epitem *it)
{
    struct epitem **link = &it->set->holders;

    while (*link && *link != it) {
        link = &(*link)->hold_next;
    }
    if (*link) {
        *link = it->hold_next;
    }
}

/** Take the registration `it` of a set off the `subs` of `ep`, the set it
 * is in; under `ep_nest_mtx`. */
static void unsub(struct epoll *ep, struct epitem *it)
{
    struct epitem **link = &ep->subs;

    while (*link && *link != it) {
        link = &(*link)->sub_next;
    }
    if (*link) {
        *link = it->sub_next;
    }
}

/**
 * Remove the registration `it` from `ep`, as Linux's `ep_remove`
 * (fs/eventpoll.c:844-1090): its record leaves the target's queue, so no
 * wake reaches it after, it leaves its description's list unless the
 * description's release has taken it (`dying`) and `force` is not set, in
 * which case the release removes it, and it leaves the set's list and the
 * ready list and is freed. The caller holds the set's `mtx`, and
 * `ep_nest_mtx` for a registration of a set. Returns 1 when removed; the
 * caller then drops the set's reference the registration held.
 */
static int ep_remove(struct epoll *ep, struct epitem *it, int force)
{
    struct epitem **link;
    uint32_t irq;

    wq_entry_remove(&it->wait);
    irq = spin_lock_irqsave(&ep_files_lock);
    if (it->dying && !force) {
        spin_unlock_irqrestore(&ep_files_lock, irq);
        return 0;
    }
    if (it->fprev) {
        *it->fprev = it->fnext;
        if (it->fnext) {
            it->fnext->fprev = it->fprev;
        }
        it->fprev = NULL;
    }
    spin_unlock_irqrestore(&ep_files_lock, irq);
    for (link = &ep->items; *link && *link != it; link = &(*link)->next) {
    }
    if (*link) {
        *link = it->next;
        ep->count--;
    }
    irq = spin_lock_irqsave(&ep->lock);
    if (it->rdstate == EPI_READY) {
        rd_unlink(ep, it);
    }
    spin_unlock_irqrestore(&ep->lock, irq);
    if (it->set) {
        unsub(ep, it);
        unhold(it);
    }
    kfree(it);
    return 1;
}

void epoll_release_file(struct open_file *ofd)
{
    for (;;) {
        uint32_t irq = spin_lock_irqsave(&ep_files_lock);
        struct epitem *it = ofd->epolls;
        struct epoll *ep;
        int nested;

        if (!it) {
            spin_unlock_irqrestore(&ep_files_lock, irq);
            return;
        }
        it->dying = 1;
        ep = it->ep;
        nested = it->set != NULL;
        spin_unlock_irqrestore(&ep_files_lock, irq);
        if (nested) {
            mutex_lock(&ep_nest_mtx);
        }
        mutex_lock(&ep->mtx);
        (void)ep_remove(ep, it, 1);
        mutex_unlock(&ep->mtx);
        if (nested) {
            mutex_unlock(&ep_nest_mtx);
        }
        ep_put(ep);
    }
}

/**
 * The longest chain of sets below the set `inner`, which a path puts
 * `depth` sets below the set `ep` it is to be added to: 0 when it holds no
 * set. The chain goes through every registration of a set (`subs`),
 * whichever process made it, since a wait by any process that has the
 * outer set looks into each. A chain that reaches `ep`, which the add
 * would close into a cycle, one deeper than EP_MAX_NESTS, and one through
 * a cycle below count as longer than EP_MAX_NESTS. Each set is looked into
 * once per check (`walk_marks`) and keeps what it found in `height`, so a
 * set met first by a short path counts the same for a longer one. Under
 * `ep_nest_mtx`, as Linux's ep_loop_check holds `epnested_mutex`
 * (fs/eventpoll.c:2364).
 */
static int sets_below(struct epoll *inner, const struct epoll *ep, int depth)
{
    struct epitem *it;
    int most = 0;

    if (inner == ep || depth > EP_MAX_NESTS) {
        return EP_MAX_NESTS + 1;
    }
    if (inner->walk == walk_marks) {
        return inner->height;
    }
    inner->walk = walk_marks;
    inner->height = EP_MAX_NESTS + 1;
    for (it = inner->subs; it && most <= EP_MAX_NESTS; it = it->sub_next) {
        int below = sets_below(it->set, ep, depth + 1) + 1;

        if (below > most) {
            most = below;
        }
    }
    inner->height = most;
    return most;
}

/**
 * The longest chain of sets above the set `ep`, through every registration
 * of it (`holders`), from `depth` sets above the set a check began at: 0
 * when no set holds it. A chain longer than EP_MAX_NESTS, or one through a
 * cycle, counts as longer; each set is looked into once per check, as in
 * `sets_below`, under `ep_nest_mtx`.
 */
static int sets_above(struct epoll *ep, int depth)
{
    struct epitem *h;
    int most = 0;

    if (depth > EP_MAX_NESTS) {
        return EP_MAX_NESTS + 1;
    }
    if (ep->walk == walk_marks) {
        return ep->height;
    }
    ep->walk = walk_marks;
    ep->height = EP_MAX_NESTS + 1;
    for (h = ep->holders; h && most <= EP_MAX_NESTS; h = h->hold_next) {
        int above = sets_above(h->ep, depth + 1) + 1;

        if (above > most) {
            most = above;
        }
    }
    ep->height = most;
    return most;
}

/**
 * The last close of a set, as Linux's `ep_clear_and_put` (fs/eventpoll.c:
 * 1102-1139): its registrations go, but for those whose description's
 * release has taken them and removes them itself, and the set goes with
 * its last reference. The registrations of the set in other sets went
 * before, at the release of its description (`epoll_release_file`). No
 * wait is on the set, since a wait holds its descriptor.
 */
static void epoll_close(fd_entry_t *e)
{
    struct epoll *ep = (struct epoll *)e->priv;
    struct epitem *it, *next;

    mutex_lock(&ep_nest_mtx);
    mutex_lock(&ep->mtx);
    for (it = ep->items; it; it = next) {
        next = it->next;
        if (ep_remove(ep, it, 0)) {
            refs_add(ep, -1);
        }
    }
    mutex_unlock(&ep->mtx);
    mutex_unlock(&ep_nest_mtx);
    e->priv = NULL;
    ep_put(ep);
}

/** The epoll set the caller's descriptor `fd` is, or NULL when `fd` is not
 * open or not a set. */
static struct epoll *epoll_of(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return (e && is_set(e)) ? (struct epoll *)e->priv : NULL;
}

/** A new set and a descriptor of the caller's on it, close-on-exec under
 * EPOLL_CLOEXEC: the descriptor, -EINVAL for another flag, -ENOMEM, or
 * `fd_alloc`'s error. */
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
    mutex_init(&ep->mtx, "ep->mtx");
    spin_lock_init(&ep->lock, "ep->lock");
    wait_queue_init_named(&ep->wq, "ep->wq");
    wait_queue_init_named(&ep->poll_wait, "ep->poll_wait");
    ep->refs = 1;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(ep);
        return fd;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &epoll_ops;
    e->priv = ep;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | ((flags & EPOLL_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    fd_install(current_proc->fds, fd);
    return fd;
}

long sys_epoll_create(struct isr_params *params)
{
    if ((int32_t)SC_ARG(params, 0) <= 0) {
        return -EINVAL;
    }
    return make_epoll(0);
}

long sys_epoll_create1(struct isr_params *params)
{
    return make_epoll((uint32_t)SC_ARG(params, 0));
}

/**
 * Whether adding the set `inner` to `ep` would make a cycle of sets, or a
 * chain of them more than EP_MAX_NESTS deep below its outermost set,
 * counting the sets above `ep` as well as those below `inner`, through the
 * registrations of every process. Linux refuses either with ELOOP, and a
 * poll of the outermost set would not look into the innermost of a longer
 * chain, whose events a wait on it would then never report. Under
 * `ep_nest_mtx`.
 */
static int nests_badly(struct epoll *inner, struct epoll *ep)
{
    int below, above;

    walk_marks++;
    below = sets_below(inner, ep, 1);
    walk_marks++;
    above = sets_above(ep, 0);
    return below + 1 + above > EP_MAX_NESTS;
}

/**
 * Wake one sleeper of epoll_wait on `ep` and every poller of it, for a
 * registration a change put on the ready list, from a context that holds
 * no queue's lock, as Linux's `ep_insert` and `ep_modify` wake `ep->wq` and
 * `ep_poll_safewake` (fs/eventpoll.c:1885-1899, 1960-1975).
 */
static void ep_wake_all(struct epoll *ep)
{
    ep_wake_if(&ep->wq);
    ep_wake_if(&ep->poll_wait);
}

/**
 * Poll `it` and put it on the ready list of `ep` when an event it watches
 * is ready, waking the set's sleepers and pollers. The caller holds the
 * set's `mtx` and, when `it->big`, the big lock.
 */
static void ep_poll_queue(struct epoll *ep, struct epitem *it)
{
    uint32_t irq;
    int wake = 0;

    if (!item_poll(it, 0, NULL)) {
        return;
    }
    irq = spin_lock_irqsave(&ep->lock);
    if (it->rdstate == EPI_IDLE) {
        rd_link(ep, it, 0);
        wake = 1;
    }
    spin_unlock_irqrestore(&ep->lock, irq);
    if (wake) {
        ep_wake_all(ep);
    }
}

/**
 * Register `target` at `fd` in `ep` with the events and data of `ev`, in
 * the record `it`, as Linux's `ep_insert` (fs/eventpoll.c:1827-1904): the
 * registration goes on the set's list and the description's, puts its
 * record on the queue the target's poll names, and is polled once more
 * after, so that a change before the record was on the queue is seen
 * there, and put on the ready list when ready. The caller holds the set's
 * `mtx`, `ep_nest_mtx` for a target that is a set, and the big lock when
 * the target's poll needs it.
 */
static void ep_insert(struct epoll *ep, struct epitem *it, int fd, fd_entry_t *target,
                      const uint32_t *ev)
{
    struct wait_queue *wq = NULL;
    uint32_t irq;

    it->ep = ep;
    it->fd = fd;
    it->ofd = target->ofd;
    it->target = *target;
    it->events = (ev[0] & ~EPOLLWAKEUP) | EPOLLERR | EPOLLHUP;
    it->data[0] = ev[1];
    it->data[1] = ev[2];
    it->big = !file_poll_unlocked(target);
    it->wait.proc = NULL;
    it->wait.next = NULL;
    it->wait.func = ep_wake;
    it->wait.wq = NULL;
    it->wait.flags = (it->events & EPOLLEXCLUSIVE) ? WQ_EXCLUSIVE : 0u;
    it->rdstate = EPI_IDLE;
    if (is_set(target)) {
        it->set = (struct epoll *)target->priv;
        it->hold_next = it->set->holders;
        it->set->holders = it;
        it->sub_next = ep->subs;
        ep->subs = it;
    }
    it->next = ep->items;
    ep->items = it;
    ep->count++;
    (void)refs_add(ep, 1);
    irq = spin_lock_irqsave(&ep_files_lock);
    it->fnext = target->ofd->epolls;
    if (it->fnext) {
        it->fnext->fprev = &it->fnext;
    }
    it->fprev = &target->ofd->epolls;
    target->ofd->epolls = it;
    spin_unlock_irqrestore(&ep_files_lock, irq);
    (void)item_poll(it, 0, &wq);
    if (wq) {
        wq_entry_add(wq, &it->wait);
    }
    ep_poll_queue(ep, it);
}

/**
 * The change `op` of the registration of `target` at `fd` in `ep`, with
 * the events and data `ev`: 0 or -errno, as `sys_epoll_ctl` gives them.
 * An add takes the record at `*fresh` and sets it to NULL. The caller holds
 * the set's `mtx`, and `ep_nest_mtx` when `target` is a set.
 */
static long ctl_locked(struct epoll *ep, int op, int fd, fd_entry_t *target, const uint32_t *ev,
                       struct epitem **fresh)
{
    struct epitem *it;
    uint32_t irq;

    for (it = ep->items; it; it = it->next) {
        if (it->fd == fd && it->ofd == target->ofd) {
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
        ep_insert(ep, *fresh, fd, target, ev);
        *fresh = NULL;
        return 0;
    case EPOLL_CTL_MOD:
        if (!it) {
            return -ENOENT;
        }
        if (it->events & EPOLLEXCLUSIVE) {
            return -EINVAL;
        }
        irq = spin_lock_irqsave(&ep->lock);
        it->events = (ev[0] & ~EPOLLWAKEUP) | EPOLLERR | EPOLLHUP;
        it->data[0] = ev[1];
        it->data[1] = ev[2];
        spin_unlock_irqrestore(&ep->lock, irq);
        ep_poll_queue(ep, it);
        return 0;
    case EPOLL_CTL_DEL:
        if (!it) {
            return -ENOENT;
        }
        if (ep_remove(ep, it, 0)) {
            refs_add(ep, -1);
        }
        return 0;
    default:
        return -EINVAL;
    }
}

/**
 * The checks of the epoll_ctl of `params` on the set `ep` and the
 * descriptor `target` it names, with the event read into `ev` unless the
 * operation is EPOLL_CTL_DEL: 0, or -EBADF, -EINVAL, -EPERM or -EFAULT, as
 * `sys_epoll_ctl` gives them.
 */
static long ctl_check(struct isr_params *params, struct epoll *ep, fd_entry_t *target, uint32_t *ev)
{
    int epfd = (int)SC_ARG(params, 0);
    int op = (int)SC_ARG(params, 1);
    int fd = (int)SC_ARG(params, 2);

    if (!fd_get(current_proc->fds, epfd) || !target) {
        return -EBADF;
    }
    if (!ep || fd == epfd || (is_set(target) && target->priv == ep)) {
        return -EINVAL;
    }
    if (target->type == FD_TYPE_FILE || target->type == FD_TYPE_DIR || !target->ofd) {
        return -EPERM;
    }
    if (op == EPOLL_CTL_DEL) {
        return 0;
    }
    if (copy_from_user(ev, (const void __user *)SC_ARG(params, 3), EP_EVENT_SIZE) != 0) {
        return -EFAULT;
    }
    if ((ev[0] & EPOLLEXCLUSIVE) &&
        (op == EPOLL_CTL_MOD ||
         (op == EPOLL_CTL_ADD && (is_set(target) || (ev[0] & ~EP_EXCLUSIVE_OK))))) {
        return -EINVAL;
    }
    return 0;
}

/**
 * The change runs without the big lock but where the target's poll needs
 * it, under the set's `mtx`, and `ep_nest_mtx` before it when the
 * descriptor added, changed or removed is a set, as Linux's `do_epoll_ctl`
 * (fs/eventpoll.c:2479-2619). EPOLLEXCLUSIVE is refused with EPOLL_CTL_MOD,
 * on a set, and with an event outside EP_EXCLUSIVE_OK, as Linux refuses
 * them. The registration's record comes from the heap before the locks,
 * so an add with no room fails with ENOMEM before the checks made under
 * them, EEXIST and ELOOP, where Linux's allocates after those.
 */
long sys_epoll_ctl(struct isr_params *params)
{
    int epfd = (int)SC_ARG(params, 0);
    struct epoll *ep = epoll_of(epfd);
    int op = (int)SC_ARG(params, 1);
    int fd = (int)SC_ARG(params, 2);
    fd_entry_t *target = fd_get(current_proc->fds, fd);
    uint32_t ev[3] = {0, 0, 0};
    struct epitem *fresh = NULL;
    int nested, big;
    long rc;

    rc = ctl_check(params, ep, target, ev);
    if (rc != 0) {
        return rc;
    }
    if (op == EPOLL_CTL_ADD && (fresh = kzalloc(sizeof(*fresh))) == NULL) {
        return -ENOMEM;
    }
    nested = is_set(target);
    big = op != EPOLL_CTL_DEL && !file_poll_unlocked(target) && !bkl_held();
    if (big) {
        bkl_enter();
    }
    if (nested) {
        mutex_lock(&ep_nest_mtx);
    }
    mutex_lock(&ep->mtx);
    rc = ctl_locked(ep, op, fd, target, ev, &fresh);
    mutex_unlock(&ep->mtx);
    if (nested) {
        mutex_unlock(&ep_nest_mtx);
    }
    if (big) {
        bkl_exit();
    }
    kfree(fresh);
    return rc;
}

/** Whether the set `ctx` has a registration on its ready list, read
 * without its lock, as Linux's `ep_events_available`: the wait's test
 * before it sleeps, which a wake that puts one there follows with its
 * wake of the sleepers. */
static int ep_available(void *ctx)
{
    return ((const struct epoll *)ctx)->rdhead != NULL;
}

/**
 * A wait on the set `epfd` for at most `max` events into the user array
 * `uevents`, until the deadline `d` when it is set, as Linux's `ep_poll`
 * (fs/eventpoll.c:2161-2283): the events of the ready list are reported
 * (`send_events`), and while there are none the thread sleeps on the
 * set's queue (`poll_wait_queue`) and looks again at each wake, and once
 * more at the deadline. Returns the events copied, 0 at the deadline,
 * -EBADF, -EINVAL, -EFAULT or -EINTR, as `sys_epoll_wait` gives them. It
 * runs without the big lock but where a registration's poll needs it.
 */
static int32_t epoll_wait_common(int epfd, uintptr_t uevents, int max, struct poll_deadline *d)
{
    struct epoll *ep = epoll_of(epfd);
    int32_t rc;

    if (!fd_get(current_proc->fds, epfd)) {
        return -EBADF;
    }
    if (!ep || max <= 0 || (uint32_t)max > EP_MAX_EVENTS) {
        return -EINVAL;
    }
    if (!user_range_ok(uevents, (uint32_t)max * EP_EVENT_SIZE)) {
        return -EFAULT;
    }
    for (;;) {
        if (ep_available(ep)) {
            rc = send_events(ep, uevents, (uint32_t)max);
            if (rc != 0) {
                return rc;
            }
        }
        rc = poll_wait_queue(&ep->wq, ep_available, ep, d, "ep_poll");
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return ep_available(ep) ? send_events(ep, uevents, (uint32_t)max) : 0;
        }
    }
}

long sys_epoll_wait(struct isr_params *params)
{
    struct poll_deadline d;

    poll_ms_deadline((int)SC_ARG(params, 3), &d);
    return epoll_wait_common((int)SC_ARG(params, 0), SC_ARG(params, 1), (int)SC_ARG(params, 2), &d);
}

long sys_epoll_pwait(struct isr_params *params)
{
    struct poll_deadline d;
    int32_t rc;

    poll_ms_deadline((int)SC_ARG(params, 3), &d);
    rc = poll_mask_set(SC_ARG(params, 4), USER_LEN32(SC_ARG(params, 5)));
    if (rc != 0) {
        return rc;
    }
    return poll_mask_done(
        epoll_wait_common((int)SC_ARG(params, 0), SC_ARG(params, 1), (int)SC_ARG(params, 2), &d));
}

/** The timespec is read as four 32-bit words, the seconds' low word first,
 * so that i386 and x86-64 read it alike. */
long sys_epoll_pwait2(struct isr_params *params)
{
    struct poll_deadline d;
    int32_t rc;

    d.set = 0;
    if (SC_ARG(params, 3)) {
        /* struct __kernel_timespec: 64-bit seconds and nanoseconds. */
        uint32_t ts[4];
        int64_t sec;
        uint64_t ms;

        if (copy_from_user(ts, (const void __user *)SC_ARG(params, 3), sizeof(ts)) != 0) {
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
        poll_ms_deadline((int)ms, &d);
    }
    rc = poll_mask_set(SC_ARG(params, 4), USER_LEN32(SC_ARG(params, 5)));
    if (rc != 0) {
        return rc;
    }
    return poll_mask_done(
        epoll_wait_common((int)SC_ARG(params, 0), SC_ARG(params, 1), (int)SC_ARG(params, 2), &d));
}
