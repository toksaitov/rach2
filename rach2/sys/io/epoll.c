/*
 * epoll.c: epoll sets, as epoll.h describes them.
 *
 * Each set has its own spinlock over its registrations and what a scan
 * keeps in them, as Linux's `ep->lock` and `ep->mtx` (fs/eventpoll.c:
 * 314-326) keep a set's lists; a wait scans the set under it with no other
 * lock, and takes the big lock first only while the set watches a
 * descriptor whose poll still needs it, or another set. The sets inside
 * others, and the registrations that link them (`holders`, `subs`), change
 * only under the big lock, as Linux's `epnested_mutex` keeps the graph of
 * sets (fs/eventpoll.c:392).
 */

#include "epoll.h"

#include "anon.h"
#include "poll.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
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

/* Format: struct epoll_event, packed on every architecture: the events,
 * then the data. */
#define EP_EVENT_SIZE 12u

/* Policy: the events one wait returns at most, as Linux bounds maxevents. */
#define EP_MAX_EVENTS (0x7FFFFFFF / EP_EVENT_SIZE)

/* Policy: how many sets deep a set may hold others, and a scan looks into
 * them, Linux's EP_MAX_NESTS; it bounds the kernel stack a scan takes. */
#define EP_MAX_NESTS 4

/* A registration. Its fields are under its set's lock; `set`, `hold_next`
 * and `sub_next` change under the big lock as well. */
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
     * queue of the descriptor at that report with its count of the wakes
     * of the kinds the registration watches. */
    uint32_t seen;
    const struct wait_queue *at;
    uint32_t wakes;
    /* The set the registration is in; for a registration of a set, that
     * set, whose last close drops the registration (`epoll_close`), the
     * next registration of it (its `holders`), and the next registration
     * of a set in the set this one is in (its `subs`). */
    struct epoll *owner;
    struct epoll *set;
    struct epitem *hold_next;
    struct epitem *sub_next;
};

struct epoll {
    struct anon_obj anon;
    /* Over `items`, `count`, the registrations' fields, `mark`, `ready`,
     * `sig` and `big`, as Linux's `ep->lock`; an inner set's is taken
     * under its outer set's at the depth it is looked into. */
    struct spinlock lock;
    struct epitem *items;
    uint32_t count;
    wait_queue_t wq;
    /* The scan that last looked into the set under the big lock, from
     * `scan_marks`, and what it found: whether a wait would report an
     * event, and the sum of the wakes its registrations watch and of its
     * own queue's. */
    uint32_t mark;
    int ready;
    uint32_t sig;
    /* Whether a wait takes the big lock before its scan: set when a scan
     * met a registration whose poll needs it, cleared by a scan under the
     * big lock that met none (`scan_set`). */
    int big;
    /* Under the big lock: the registrations of this set in the sets it is
     * in, the registrations of sets in it, and what a nesting check found,
     * the check's mark and the longest chain of sets below or above it
     * (`sets_below`, `sets_above`). */
    struct epitem *holders;
    struct epitem *subs;
    uint32_t walk;
    int height;
};

/* One look at a set by a scan: the queues found, which a wait must be on,
 * where they go, as many as `room`, and how many were found, which may be
 * more; the scan's mark under the big lock; whether a registration whose
 * poll needs the big lock was met without it, and was passed by
 * (`retry`); and how many such were polled under it (`locked`). */
struct ep_pass {
    struct wait_queue **wqs;
    uint32_t room;
    uint32_t n;
    uint32_t mark;
    int retry;
    int locked;
};

/* An event a wait took: the struct epoll_event it goes out as, the
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
 * one at the same address, taken by an atomic step; the scans under the
 * big lock that have looked into sets, which tell a set looked into by the
 * scan in progress (`set_ready`); and the nesting checks, under the big
 * lock too. */
static volatile unsigned int ep_ids;
static uint32_t scan_marks;
static uint32_t walk_marks;

static struct epoll *epoll_of(int fd);
static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq);
static int set_ready(struct epoll *ep, int depth, struct ep_pass *q);

/** A new registration id: one more than the last, by an atomic step, since
 * each set's ctl takes only that set's lock. */
static uint32_t next_id(void)
{
    unsigned int old;

    do {
        old = ep_ids;
    } while (arch_atomic_cmpxchg(&ep_ids, old, old + 1u) != old);
    return old + 1u;
}

/** Add `wq`, unless NULL, to the queues `q` has found: kept while there is
 * room, counted whether or not. */
static void q_add(struct ep_pass *q, struct wait_queue *wq)
{
    if (wq) {
        if (q->n < q->room) {
            q->wqs[q->n] = wq;
        }
        q->n++;
    }
}

/** A pass that keeps the queues it finds in the `room` of `wqs`. */
static void pass_init(struct ep_pass *q, struct wait_queue **wqs, uint32_t room)
{
    q->wqs = wqs;
    q->room = room;
    q->n = 0;
    q->mark = 0;
    q->retry = 0;
    q->locked = 0;
}

/** Whether the descriptor `e` is an epoll set. */
static int is_set(const fd_entry_t *e)
{
    return e->ops && e->ops->poll == epoll_poll;
}

/** Whether a scan looks at the descriptor `e` only under the big lock: a
 * set, whose sets inside change under it, or a descriptor whose poll needs
 * it (`file_poll_unlocked`). */
static int needs_big(const fd_entry_t *e)
{
    return is_set(e) || !file_poll_unlocked(e);
}

/** Whether the descriptor `e` is the open file the registration `it` was
 * made for. */
static int item_names(const struct epitem *it, const fd_entry_t *e)
{
    return e && e->ofd == it->ofd && e->ofd->serial == it->serial;
}

/** Whether the descriptor of an edge-triggered registration, whose wait
 * queue is `wq` with `wakes` counted of the kinds it watches, has changed
 * in a way it watches since `it` was last reported. */
static int item_woken(const struct epitem *it, const struct wait_queue *wq, uint32_t wakes)
{
    return wq != it->at || wakes != it->wakes;
}

/** The kinds of change (`wq_wake`) among the events `want`: data for the
 * events of input, room for those of output, and a hangup, which every
 * registration watches with EPOLLERR and EPOLLHUP. */
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

/**
 * The events among `want` of the descriptor `e`, its wait queue in `*wq`,
 * and in `*wakes` the wakes the queue has counted of the kinds of change
 * among `want`, which tell a change a registration watching `want` is told
 * of. An epoll set, looked into at `depth` under its lock, which is taken
 * here under the lock of the set it is in, is readable while a wait on it
 * would report an event, and its count is the sum of the counts of its
 * registrations and of its own queue, so that a change any of them watches
 * is one of the set. The queues go on `q`. The caller has the big lock
 * when `needs_big` says so.
 */
static uint32_t target_poll(fd_entry_t *e, uint32_t want, int depth, struct ep_pass *q,
                            struct wait_queue **wq, uint32_t *wakes)
{
    uint32_t ready;

    if (is_set(e)) {
        struct epoll *inner = (struct epoll *)e->priv;

        spin_lock_nested(&inner->lock, (unsigned int)depth + 1u);
        ready = set_ready(inner, depth + 1, q) ? EPOLLIN | EPOLLRDNORM : 0u;
        *wq = &inner->wq;
        *wakes = inner->sig;
        spin_unlock(&inner->lock);
        return ready & want;
    }
    ready = (uint32_t)(uint16_t)poll_fd_held(e, (short)want, wq) & want;
    q_add(q, *wq);
    *wakes = *wq ? wq_wakes(*wq, kinds_of(want)) : 0u;
    return ready;
}

/**
 * The events of `it` a wait would report now, or 0, taking none; the wait
 * queue of its descriptor in `*wq`, with its count of wakes in `*wakes`,
 * and the queues a wait must be on in `q` (`target_poll`), for a set
 * inside the one scanned at `depth`. EPOLLERR and EPOLLHUP are watched
 * whatever the registration asked, none included, and nothing is while a
 * one-shot registration is off. The caller holds the lock of the set `it`
 * is in. A descriptor that `needs_big` and a scan without the big lock
 * meet is not looked at, and the scan is told so in `q->retry`.
 *
 * A registration lasts while the open file it was made for is open in any
 * process, as in Linux, and is marked for removal with `*gone` once it is
 * not; a registration of a set goes at the set's last close. It is looked
 * at through the caller's descriptor at its number alone, since a wait
 * sleeps on the queues of what it looks at and only the caller's own
 * descriptors keep those while it sleeps: a caller whose number names
 * another open file, or none, is told nothing of it.
 *
 * An edge-triggered registration is reported when an event is ready that
 * was not at its last report, or has gone since, or when its descriptor's
 * queue has been woken since that report for a kind of change among its
 * events (`kinds_of`): a pipe drained and filled again between two waits is
 * ready as it was, and its reader's queue is what tells the new edge.
 * Linux's wakeup callback likewise queues a registration for a wake whose
 * events it watches.
 */
static uint32_t item_ready(struct epitem *it, int depth, struct ep_pass *q, struct wait_queue **wq,
                           uint32_t *wakes, int *gone)
{
    fd_entry_t *e = fd_get(current_proc->fds, it->fd);
    uint32_t want = (it->events & EPOLL_EVENTS) | EPOLLERR | EPOLLHUP;
    uint32_t ready;

    *wq = NULL;
    *wakes = 0;
    *gone = 0;
    if (!item_names(it, e)) {
        *gone = !it->set && !fd_ofd_live(it->ofd, it->serial);
        return 0;
    }
    if (it->off) {
        return 0;
    }
    if (needs_big(e)) {
        if (!bkl_held()) {
            q->retry = 1;
            return 0;
        }
        q->locked++;
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

/** Record the report of `ready` for `it`, whose descriptor's wait queue is
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
 * the queues it has records for; the queues the last scan found, when
 * they were more, with nothing ready, for the wait to begin again with;
 * and whether a close asked the wait to give back its holds of the
 * descriptors it closes (`poll_drop_take`). */
struct epoll_scan {
    struct epoll *ep;
    struct epoll_out *out;
    uint32_t max;
    uint32_t n;
    uint32_t queues;
    uint32_t grow;
    int drop;
};

/** Take the registration `it` of a set off that set's `holders`; under
 * the big lock. */
static void unhold(struct epitem *it)
{
    struct epitem **link = &it->set->holders;

    while (*link && *link != it) {
        link = &(*link)->hold_next;
    }
    if (*link) {
        *link = it->hold_next;
    }
    it->set = NULL;
}

/** Take the registration `it` of a set off the `subs` of `ep`, the set it
 * is in; under the big lock and `ep`'s lock. */
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

/** Remove the registration at `*link` from `ep`, off the holders of the set
 * it registers when it registers one, and free it. The caller holds `ep`'s
 * lock, and the big lock for a registration of a set. */
static void drop_item(struct epoll *ep, struct epitem **link)
{
    struct epitem *it = *link;

    *link = it->next;
    ep->count--;
    if (it->set) {
        unsub(ep, it);
        unhold(it);
    }
    kfree(it);
}

/** Fill `o` with the event `ready` of `it`, and what its report is about to
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

/** Put back what the report of `o` changed in its registration, when the
 * registration is in `ep` still as it was: the event goes to a later
 * wait. The caller holds `ep`'s lock. */
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
 * those of what it watches, go on `q`, and to `ep->sig` the sum of the
 * wakes of its own queue and those its registrations watch.
 *
 * A scan looks into a set once: a set it meets again, by another path or
 * inside itself, gives what the first look found, or nothing while that
 * look is not done, so a cycle ends, and a set deeper than EP_MAX_NESTS is
 * not looked into and gives nothing. `scan_marks` counts the scans. The
 * caller holds the big lock and `ep`'s lock, with interrupts off.
 */
static int set_ready(struct epoll *ep, int depth, struct ep_pass *q)
{
    struct epitem *it;

    if (ep->mark == q->mark) {
        return ep->ready;
    }
    ep->mark = q->mark;
    ep->ready = 0;
    ep->sig = wq_wakes(&ep->wq, WQ_ALL);
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

/**
 * One pass of a wait's scan over its set, under the set's lock: the events
 * of the set's registrations into the wait's array, at most `sc->max`, each
 * taken as it is reported, and the queues to wait on while there are none
 * into `q`, those of the sets inside it and of what they watch among them
 * (`set_ready`).
 *
 * A reported registration moves to the end of the list, so a wait with
 * room for fewer events than are ready is followed by one that reports the
 * others, as Linux's waits take the ready descriptors in turn. A pass
 * under the big lock keeps in `big` whether the set still needs it; one
 * without it that met a registration needing it sets `big` and says so in
 * `q->retry`.
 */
static void scan_pass(struct epoll_scan *sc, struct ep_pass *q)
{
    struct epoll *ep = sc->ep;
    struct epitem **link = &ep->items;
    struct epitem *reported = NULL;
    struct epitem **tail = &reported;
    struct epitem *it;
    int big = bkl_held();

    spin_lock(&ep->lock);
    sc->n = 0;
    if (big) {
        q->mark = ++scan_marks;
        ep->mark = q->mark;
        ep->ready = 0;
    }
    q_add(q, &ep->wq);
    while ((it = *link) != NULL && sc->n < sc->max) {
        struct wait_queue *wq;
        uint32_t ready, wakes;
        int gone;

        ready = item_ready(it, 0, q, &wq, &wakes, &gone);
        if (gone) {
            drop_item(ep, link);
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
    if (big) {
        ep->big = q->locked != 0;
    } else if (q->retry) {
        ep->big = 1;
    }
    spin_unlock(&ep->lock);
}

/**
 * The scan of a wait (`poll_wait`), with interrupts off. A set that has
 * needed the big lock (`big`) is scanned under it; one that has not is
 * scanned without it, and again under it when that pass met a registration
 * needing it and found nothing ready, so that the wait is on every queue
 * before it sleeps. A pass that found events returns them, and the next
 * wait looks at what it passed by.
 *
 * A set that another thread grew while the wait slept has more queues than
 * the wait has records for; with nothing ready, the scan says so in
 * `grow`, and the wait begins again with a record for each. A close that
 * asked the wait to give back its holds ends the wait at once (`drop`).
 */
static int scan_set(void *ctx, struct wait_queue **wqs)
{
    struct epoll_scan *sc = (struct epoll_scan *)ctx;
    struct ep_pass q;
    int took_big = 0;

    if (current_proc->poll_drop) {
        sc->n = 0;
        sc->drop = 1;
        return 1;
    }
    if (sc->ep->big && !bkl_held()) {
        bkl_enter();
        took_big = 1;
    }
    for (;;) {
        pass_init(&q, wqs, sc->queues);
        scan_pass(sc, &q);
        if (!q.retry || sc->n > 0 || bkl_held()) {
            break;
        }
        bkl_enter();
        took_big = 1;
    }
    if (took_big) {
        bkl_exit();
    }
    if (sc->n == 0 && q.n > sc->queues) {
        sc->grow = q.n;
        return 1;
    }
    return (int)sc->n;
}

/**
 * The readiness of an epoll set for poll: readable when a wait would report
 * an event, which `set_ready` says without taking it, so an edge-triggered
 * event reported already, or a one-shot registration that is off, does not
 * make the set readable. The queue named is the set's own; poll's scan
 * takes all a set's queues from `epoll_poll_set`. The set's table does not
 * mark its poll unlocked, so the caller holds the big lock.
 */
static short epoll_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct epoll *ep = (struct epoll *)e->priv;
    struct ep_pass none;
    uint32_t irq = spin_lock_irqsave(&ep->lock);
    int ready;

    pass_init(&none, NULL, 0);
    none.mark = ++scan_marks;
    ready = set_ready(ep, 0, &none);
    spin_unlock_irqrestore(&ep->lock, irq);
    if (wq) {
        *wq = &ep->wq;
    }
    return ready ? (short)((uint32_t)events & (EPOLLIN | EPOLLRDNORM)) : 0;
}

int epoll_is_set(const fd_entry_t *e)
{
    return is_set(e);
}

int32_t epoll_target(const fd_entry_t *e, int tfd, uint32_t toff, const struct open_file **ofd)
{
    struct epoll *ep;
    const struct epitem *it;
    uint32_t irq;
    int32_t rc = -ENOENT;

    if (!e || !is_set(e)) {
        return -EINVAL;
    }
    ep = (struct epoll *)e->priv;
    irq = spin_lock_irqsave(&ep->lock);
    for (it = ep->items; it; it = it->next) {
        if (it->fd != tfd || !fd_ofd_live(it->ofd, it->serial)) {
            continue;
        }
        if (toff-- == 0) {
            *ofd = it->ofd;
            rc = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&ep->lock, irq);
    return rc;
}

/** The caller holds the big lock, which poll and select take for a set. */
short epoll_poll_set(fd_entry_t *e, short events, struct wait_queue **wqs, uint32_t room,
                     uint32_t *n)
{
    struct epoll *ep = (struct epoll *)e->priv;
    struct ep_pass q;
    int ready;

    pass_init(&q, wqs, room);
    q.mark = ++scan_marks;
    spin_lock(&ep->lock);
    ready = set_ready(ep, 0, &q);
    spin_unlock(&ep->lock);
    *n = q.n;
    return ready ? (short)((uint32_t)events & (EPOLLIN | EPOLLRDNORM)) : 0;
}

/**
 * The longest chain of sets below the set `inner`, which a path puts
 * `depth` sets below the set `ep` it is to be added to: 0 when it holds no
 * set. The chain goes through every registration of a set (`subs`),
 * whichever process made it and whatever became of its number, since a
 * scan by a process that has all their numbers looks into each. A chain
 * that reaches `ep`, which the add would close into a cycle, one deeper
 * than EP_MAX_NESTS, and one through a cycle below count as longer than
 * EP_MAX_NESTS. Each set is looked into once per check (`walk_marks`) and
 * keeps what it found in `height`, so a set met first by a short path
 * counts the same for a longer one. Under the big lock, which every change
 * of `subs` holds, as Linux's ep_loop_check holds `epnested_mutex`
 * (fs/eventpoll.c:2364, 2555).
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
 * of it (`holders`), whichever process made it and whatever became of its
 * number, from `depth` sets above the set a check began at: 0 when no set
 * holds it. A chain longer than EP_MAX_NESTS, or one through a cycle,
 * counts as longer; each set is looked into once per check, as in
 * `sets_below`, under the big lock.
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
        int above = sets_above(h->owner, depth + 1) + 1;

        if (above > most) {
            most = above;
        }
    }
    ep->height = most;
    return most;
}

/** Take the registration `h` of a set out of the set that has it, under
 * that set's lock, and off the holders of the set it is of; under the big
 * lock. */
static void drop_holder(struct epitem *h)
{
    struct epoll *owner = h->owner;
    uint32_t irq = spin_lock_irqsave(&owner->lock);
    struct epitem **link = &owner->items;

    while (*link && *link != h) {
        link = &(*link)->next;
    }
    if (*link) {
        drop_item(owner, link);
    } else {
        unsub(owner, h);
        unhold(h);
    }
    spin_unlock_irqrestore(&owner->lock, irq);
}

/**
 * The last close of a set, when its open file is closed in every process:
 * its registrations go, and so do the registrations of it in other sets, as
 * Linux removes a file's registrations at its last close. It runs under
 * the big lock, as the release of every descriptor whose table is not
 * marked unlocked does, and no wait scans the set any more: a wait holds
 * the set's descriptor, and a scan of a set it is in holds the big lock.
 */
static void epoll_close(fd_entry_t *e)
{
    struct epoll *ep = (struct epoll *)e->priv;
    uint32_t irq = spin_lock_irqsave(&ep->lock);

    while (ep->items) {
        drop_item(ep, &ep->items);
    }
    spin_unlock_irqrestore(&ep->lock, irq);
    while (ep->holders) {
        drop_holder(ep->holders);
    }
    kfree(ep);
    e->priv = NULL;
}

static const struct file_ops epoll_ops = {NULL, NULL, epoll_poll, NULL, epoll_close, NULL, NULL, 0};

/** The epoll set the caller's descriptor `fd` is, or NULL when `fd` is not
 * open or not a set. */
static struct epoll *epoll_of(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return (e && e->ops == &epoll_ops) ? (struct epoll *)e->priv : NULL;
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
    spin_lock_init(&ep->lock, "ep->lock");
    wait_queue_init(&ep->wq);
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
 * registrations of every process: a fork's child that has closed its copy
 * of a number shares the sets with a parent that has not. Linux refuses
 * either with ELOOP, and a scan of the outermost set would not look into
 * the innermost of a longer chain, whose events a wait on it would then
 * never report. Under the big lock.
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

/** Add `it`, the caller's record, to `ep` as the registration of the
 * descriptor `target` at `fd`, a set's under the big lock. */
static void add_item(struct epoll *ep, struct epitem *it, int fd, fd_entry_t *target)
{
    it->fd = fd;
    it->ofd = target->ofd;
    it->serial = target->ofd->serial;
    it->owner = ep;
    if (is_set(target)) {
        it->set = (struct epoll *)target->priv;
        it->hold_next = it->set->holders;
        it->set->holders = it;
        it->sub_next = ep->subs;
        ep->subs = it;
    }
    if (needs_big(target)) {
        ep->big = 1;
    }
    it->next = ep->items;
    ep->items = it;
    ep->count++;
}

/**
 * The change `op` of the registration of `target` at `fd` in `ep`, with
 * the events and data `ev`, under `ep`'s lock, and the big lock when
 * `target` is a set: 0 or -errno, as `sys_epoll_ctl` gives them. An add
 * takes the record at `*fresh` and sets it to NULL.
 */
static long ctl_locked(struct epoll *ep, int op, int fd, fd_entry_t *target, const uint32_t *ev,
                       struct epitem **fresh)
{
    struct epitem **link;
    struct epitem *it;

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
        it = *fresh;
        *fresh = NULL;
        add_item(ep, it, fd, target);
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
    it->id = next_id();
    it->events = ev[0] & ~EPOLLWAKEUP;
    it->data[0] = ev[1];
    it->data[1] = ev[2];
    it->off = 0;
    it->seen = 0;
    it->at = NULL;
    it->wakes = 0;
    return 0;
}

/**
 * The change runs without the big lock, under the set's lock, unless the
 * descriptor added, changed or removed is a set: the graph of sets changes
 * under the big lock, which every scan that looks into a set inside
 * another holds. A registration of a descriptor whose poll needs the big
 * lock marks the set to take it for its scans (`big`). The registration's
 * record comes from the heap before the lock, so an add with no room
 * fails with ENOMEM before the other checks.
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
    uint32_t irq;
    int nested;
    long rc;

    if (!fd_get(current_proc->fds, epfd) || !target) {
        return -EBADF;
    }
    if (!ep || fd == epfd || (is_set(target) && target->priv == ep)) {
        return -EINVAL;
    }
    if (target->type == FD_TYPE_FILE || target->type == FD_TYPE_DIR) {
        return -EPERM;
    }
    if (op != EPOLL_CTL_DEL &&
        copy_from_user(ev, (const void __user *)SC_ARG(params, 3), EP_EVENT_SIZE) != 0) {
        return -EFAULT;
    }
    if (op == EPOLL_CTL_ADD && (fresh = kzalloc(sizeof(*fresh))) == NULL) {
        return -ENOMEM;
    }
    nested = is_set(target);
    if (nested) {
        bkl_enter();
    }
    irq = spin_lock_irqsave(&ep->lock);
    rc = ctl_locked(ep, op, fd, target, ev, &fresh);
    spin_unlock_irqrestore(&ep->lock, irq);
    if (nested) {
        bkl_exit();
    }
    kfree(fresh);
    if (rc == 0 && op != EPOLL_CTL_DEL) {
        wq_wake_all(&ep->wq);
    }
    return rc;
}

/**
 * A wait on the set `epfd` for at most `max` events into the user array
 * `uevents`, until the deadline `d` when it is set. Returns the events
 * copied, 0 at the deadline, -EBADF, -EINVAL, -EFAULT, -EINTR or
 * -ENOMEM, as `sys_epoll_wait` gives them. It runs without the big lock
 * but where its scan needs it (`scan_set`).
 *
 * The scan takes each event it reports, so that a second waiter does not
 * report an edge-triggered or one-shot event again, and the copy to the
 * caller comes after the wait, and can sleep in a page fault. An event that
 * cannot be copied is put back in its registration (`untook`), unless the
 * registration has gone or changed meanwhile, and the wait returns the
 * events copied before it, or EFAULT when there are none, as Linux's
 * does.
 *
 * A close of a descriptor the wait holds, by another thread, while the
 * wait was not quiet asks it to give back its holds of the descriptors
 * being closed (`poll_let_go`): the wait then lets them go, releasing
 * each no other call holds (`fd_holds_release_closing`), and waits again.
 * A request already there when the wait begins is left from an earlier
 * one, since the wait holds nothing yet but its set.
 */
static int32_t epoll_wait_common(int epfd, uintptr_t uevents, int max, struct poll_deadline *d)
{
    struct epoll *ep = epoll_of(epfd);
    struct epoll_scan sc;
    int32_t rc;
    uint32_t i, irq;

    if (!fd_get(current_proc->fds, epfd)) {
        return -EBADF;
    }
    if (!ep || max <= 0 || (uint32_t)max > EP_MAX_EVENTS) {
        return -EINVAL;
    }
    if (!user_range_ok(uevents, (uint32_t)max * EP_EVENT_SIZE)) {
        return -EFAULT;
    }
    current_proc->epoll_wait_fd = epfd;
    current_proc->epoll_waiting = 1;
    poll_drop_take();
    sc.ep = ep;
    sc.grow = ep->count + 1u;
    for (;;) {
        uint32_t count = ep->count;

        sc.max = (uint32_t)max < count + 1u ? (uint32_t)max : count + 1u;
        sc.out = kmalloc(sizeof(struct epoll_out) * sc.max);
        if (!sc.out) {
            current_proc->epoll_waiting = 0;
            return -ENOMEM;
        }
        sc.queues = sc.grow;
        sc.grow = 0;
        sc.n = 0;
        sc.drop = 0;
        rc = poll_wait(scan_set, &sc, sc.queues, d, "ep_poll");
        if (sc.drop) {
            poll_drop_take();
            fd_holds_release_closing(epfd);
            kfree(sc.out);
            sc.grow = sc.queues;
            continue;
        }
        if (rc <= 0 || sc.grow == 0) {
            break;
        }
        kfree(sc.out);
    }
    current_proc->epoll_waiting = 0;
    for (i = 0; rc > 0 && i < sc.n; i++) {
        if (copy_to_user((void __user *)(uevents + i * EP_EVENT_SIZE), sc.out[i].user,
                         EP_EVENT_SIZE) != 0) {
            rc = i > 0 ? (int32_t)i : -EFAULT;
            irq = spin_lock_irqsave(&ep->lock);
            while (i < sc.n) {
                untook(ep, &sc.out[i++]);
            }
            spin_unlock_irqrestore(&ep->lock, irq);
        }
    }
    kfree(sc.out);
    return rc;
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
