/**
 * Host test of the list of waiters of proc/wait_queue.c.
 *
 * host-deps:
 *
 * The test runs the enqueue, which adds a waiter once at the tail, the
 * dequeue, and wq_wake_all, which makes every blocked waiter ready, oldest
 * first, and empties the queue, under the sanitizers and with process records
 * of its own. The block and
 * the idle wait run only in the kernel, where the guest probe waitprobe
 * covers them.
 *
 * A wake is counted in the queue's `wakes` for each kind of change it names,
 * data, room or hangup, with waiters or none, and wq_wake_all names all three;
 * an edge-triggered epoll registration reads the kinds it watches to tell
 * that its object changed. A wake that names none still wakes every waiter,
 * and wq_wakes sums the kinds asked for.
 *
 * One row does run the wait: a model `schedule` records the deadline the
 * waiter blocked with and wakes it as `signal_post` does, clearing that
 * deadline. A wait whose condition is still false must arm the caller's
 * deadline again before it blocks again, or a blocked or ignored signal would
 * leave it asleep with no timer to end it. A wait files itself and is
 * blocked before it tests its condition, so a wake made during a false
 * test is not lost, and `wq_complete` sets its flag and wakes together.
 *
 * Each queue has its own lock, and the interrupts a wake asks for go out
 * after it: every call leaves its queue's lock free, a poller on two queues
 * is made ready by the first wake and asks for one interrupt, and no
 * interrupt is sent while a queue's lock is held. A wake for a waker with
 * a lock of its own adds its interrupts to the waker's set and sends none.
 *
 * The test defines WAIT_QUEUE_HOSTTEST, under which wait_queue.c masks no
 * interrupt and does not halt, a queue's lock is a word it sets and clears
 * and counts in `wq_host_held`, a wake marks the process ready itself, and
 * an interrupt is counted in `wq_host_kicks`, and in `wq_host_kicks_held`
 * when a queue's lock is held; it includes the file whole so that its
 * static enqueue and dequeue and those counts can be reached.
 */
#define WAIT_QUEUE_HOSTTEST

#include "proc/wait_queue.c"

extern int dprintf(int fd, const char *format, ...);

struct percpu cpu_table[NR_CPUS];
static int blocks, conds;
static uint32_t deadline_at_block[4];

/**
 * The model scheduler: record the deadline the waiter blocked with, then wake
 * it as `signal_post` does, clearing the deadline.
 */
void schedule(void)
{
    if (current_proc && current_proc->state == PROC_BLOCKED && blocks < 4) {
        deadline_at_block[blocks++] = current_proc->wakeup_tick;
        current_proc->wakeup_tick = 0;
        current_proc->state = PROC_READY;
    }
}

/** 0: no signal is pending. */
int signal_pending(void)
{
    return 0;
}

/** A condition false twice and then true, so the wait blocks twice. */
static int true_third(void *arg)
{
    (void)arg;
    return ++conds >= 3;
}

static int fails;

#define CHECK(cond)                                           \
    do {                                                      \
        if (!(cond)) {                                        \
            dprintf(2, "wait-queue-test: fail: %s\n", #cond); \
            fails++;                                          \
        }                                                     \
    } while (0)

/** How many of the waiters on `wq` are `p`'s. */
static int count_in(wait_queue_t *wq, process_t *p)
{
    struct wq_waiter *w;
    int n = 0;

    for (w = wq->head; w; w = w->next) {
        if (w->proc == p) {
            n++;
        }
    }
    return n;
}

/** The number of waiters on `wq`. */
static int total_queued(wait_queue_t *wq)
{
    struct wq_waiter *w;
    int n = 0;

    for (w = wq->head; w; w = w->next) {
        n++;
    }
    return n;
}

/** Put `p`'s own waiter on `wq`. */
static void enqueue(wait_queue_t *wq, process_t *p)
{
    p->waiter.proc = p;
    wq_enqueue(wq, &p->waiter);
}

/** Take `p`'s own waiter off `wq`. */
static void dequeue(wait_queue_t *wq, process_t *p)
{
    wq_dequeue(wq, &p->waiter);
}

/** Say whether `wq` holds the waiters of `p` and then `q` and then `r`, and
 * nothing more. */
static int order_is(wait_queue_t *wq, process_t *p, process_t *q, process_t *r)
{
    struct wq_waiter *w = wq->head;

    return w && w->proc == p && (w = w->next) && w->proc == q && (w = w->next) && w->proc == r &&
           !w->next;
}

/** The record of the order in which a wake made processes ready. */
static process_t *woke[3];
static int nwoke;

/** Note the processes of `wq` that a wake will make ready, head first. */
static void note_wake_order(wait_queue_t *wq)
{
    struct wq_waiter *w;

    nwoke = 0;
    for (w = wq->head; w && nwoke < 3; w = w->next) {
        woke[nwoke++] = w->proc;
    }
}

/**
 * Waiters keep the order they came in, so a wake makes the oldest ready
 * first, and a waiter whose condition is still false joins the tail again,
 * behind the older waiters and ahead of a later one. At the head, the newest
 * waiter would be made ready first at every wake, and a waiter for a lock
 * that two others take in turn would never get it.
 */
static void wake_order(void)
{
    wait_queue_t wq;
    static process_t a, b, c;

    wait_queue_init(&wq);
    a.state = b.state = c.state = PROC_BLOCKED;
    enqueue(&wq, &a);
    enqueue(&wq, &b);
    enqueue(&wq, &c);
    enqueue(&wq, &a); /* already queued: keeps its place */
    CHECK(order_is(&wq, &a, &b, &c));
    note_wake_order(&wq);
    wq_wake_all(&wq);
    CHECK(nwoke == 3 && woke[0] == &a && woke[1] == &b && woke[2] == &c);

    /* a and c find their conditions false and join again; b asks later. */
    a.state = c.state = PROC_BLOCKED;
    enqueue(&wq, &a);
    enqueue(&wq, &c);
    b.state = PROC_BLOCKED;
    enqueue(&wq, &b);
    CHECK(order_is(&wq, &a, &c, &b));
    dequeue(&wq, &c);
    enqueue(&wq, &c);
    CHECK(order_is(&wq, &a, &b, &c));
    wq_wake_all(&wq);
}

/**
 * Each wake is counted for every kind it names, with waiters or none, a kind
 * woken twice twice, and an init starts the counts over; a wake of one kind,
 * or of none, still wakes every waiter.
 */
static void wake_kinds(void)
{
    wait_queue_t wq;
    static process_t a, b;

    wait_queue_init(&wq);
    wq_wake(&wq, WQ_DATA);
    CHECK(wq_wakes(&wq, WQ_DATA) == 1);
    CHECK(wq_wakes(&wq, WQ_ROOM) == 0 && wq_wakes(&wq, WQ_HANGUP) == 0);
    wq_wake(&wq, WQ_ROOM | WQ_HANGUP);
    CHECK(wq_wakes(&wq, WQ_DATA) == 1 && wq_wakes(&wq, WQ_ROOM) == 1);
    CHECK(wq_wakes(&wq, WQ_HANGUP) == 1 && wq_wakes(&wq, WQ_DATA | WQ_HANGUP) == 2);
    wq_wake(&wq, 0);
    CHECK(wq_wakes(&wq, WQ_ALL) == 3);
    wait_queue_init(&wq);
    CHECK(wq_wakes(&wq, WQ_ALL) == 0);

    a.state = PROC_BLOCKED;
    b.state = PROC_BLOCKED;
    enqueue(&wq, &a);
    wq_wake(&wq, WQ_ROOM);
    CHECK(a.state == PROC_READY && total_queued(&wq) == 0);
    enqueue(&wq, &b);
    wq_wake(&wq, 0);
    CHECK(b.state == PROC_READY && total_queued(&wq) == 0);
    CHECK(wq_wakes(&wq, WQ_DATA | WQ_HANGUP) == 0 && wq_wakes(&wq, WQ_ROOM) == 1);
    wq_wake(&wq, WQ_DATA);
    wq_wake(&wq, WQ_DATA);
    CHECK(wq_wakes(&wq, WQ_DATA) == 2);
}

/* The queue the rows of `filed_first` wait on, and what their conditions
 * saw. */
static wait_queue_t filed_wq;
static int filed_seen, filed_calls;

/** A condition that notes whether the caller was on `filed_wq` and blocked
 * when it was tested, and is true. */
static int filed_test(void *arg)
{
    (void)arg;
    filed_calls++;
    filed_seen = count_in(&filed_wq, current_proc) == 1 && current_proc->state == PROC_BLOCKED;
    return 1;
}

/** A condition whose first test finds it false while a waker on another
 * processor wakes the queue, and whose second finds it true. */
static int woken_in_test(void *arg)
{
    (void)arg;
    if (++filed_calls == 1) {
        wq_wake_all(&filed_wq);
        return 0;
    }
    return 1;
}

/**
 * A sleep files itself on its queue and is blocked before it tests its
 * condition, so a wake that comes after a false test finds it and makes it
 * ready, and it tests again instead of sleeping; and `wq_complete` sets its
 * flag and wakes the waiters in one hold of the queue's lock.
 */
static void filed_first(void)
{
    static process_t sleeper, waiter;
    volatile int done = 0;
    int before = blocks;

    wait_queue_init(&filed_wq);
    sleeper.waiter.proc = &sleeper;
    sleeper.state = PROC_RUNNING;
    current_proc = &sleeper;
    filed_calls = 0;
    CHECK(wq_wait_event(&filed_wq, filed_test, 0) == 0);
    CHECK(filed_seen && filed_calls == 1);
    filed_calls = 0;
    CHECK(wq_wait_event(&filed_wq, woken_in_test, 0) == 0);
    CHECK(filed_calls == 2 && blocks == before);
    CHECK(sleeper.state == PROC_RUNNING && total_queued(&filed_wq) == 0);
    current_proc = 0;

    wait_queue_init(&filed_wq);
    waiter.waiter.proc = &waiter;
    waiter.state = PROC_BLOCKED;
    enqueue(&filed_wq, &waiter);
    wq_complete(&filed_wq, &done);
    CHECK(done == 1 && waiter.state == PROC_READY && total_queued(&filed_wq) == 0);
}

/**
 * Each queue's lock is free after every call, a poller's two records make
 * it ready once and ask for one interrupt, to the processor it ran on, and
 * every interrupt goes out with no queue's lock held; `wq_complete` and
 * `wq_wake_proc` send theirs the same way, and `wq_wake_all_deferred`
 * leaves them in the caller's set.
 */
static void own_locks(void)
{
    static wait_queue_t qa, qb, qc;
    static process_t poller, other, lone;
    static struct wq_waiter rec[2];
    volatile int done = 0;
    uint64_t cpus = 0;
    int kicks = wq_host_kicks;

    wait_queue_init(&qa);
    wait_queue_init(&qb);
    wait_queue_init(&qc);
    poller.cpu = 3;
    other.cpu = 5;
    poller.state = other.state = PROC_BLOCKED;
    poller.woken = other.woken = 0;
    rec[0].proc = rec[1].proc = &poller;
    wq_poll_add(&qa, &rec[0]);
    wq_poll_add(&qb, &rec[1]);
    enqueue(&qa, &other);
    CHECK(qa.lock.locked == 0 && qb.lock.locked == 0 && wq_host_held == 0);

    wq_wake(&qa, WQ_DATA);
    CHECK(poller.state == PROC_READY && poller.woken == 1);
    CHECK(other.state == PROC_READY && other.woken == 1);
    CHECK(wq_host_kicks == kicks + 2 && wq_host_kicks_held == 0);
    wq_wake(&qb, WQ_DATA);
    CHECK(wq_host_kicks == kicks + 2 && total_queued(&qb) == 0);
    CHECK(qa.lock.locked == 0 && qb.lock.locked == 0 && wq_host_held == 0);

    other.state = PROC_BLOCKED;
    enqueue(&qc, &other);
    wq_complete(&qc, &done);
    CHECK(done == 1 && other.state == PROC_READY && wq_host_kicks == kicks + 3);
    lone.cpu = 7;
    lone.state = PROC_BLOCKED;
    wq_wake_proc(&lone);
    CHECK(lone.state == PROC_READY && wq_host_kicks == kicks + 4);
    wq_wake_proc(&lone);
    CHECK(wq_host_kicks == kicks + 4);

    poller.state = other.state = PROC_BLOCKED;
    wq_poll_add(&qa, &rec[0]);
    enqueue(&qa, &other);
    wq_wake_all_deferred(&qa, &cpus);
    CHECK(cpus == (((uint64_t)1 << 3) | ((uint64_t)1 << 5)));
    CHECK(wq_host_kicks == kicks + 4 && wq_wakes(&qa, WQ_ALL) == 4);
    CHECK(qa.lock.locked == 0 && wq_host_held == 0 && wq_host_kicks_held == 0);
}

/** Run every check; the exit status is the count of failed checks, at most
 * 255. */
int main(void)
{
    wait_queue_t wq;
    /* Static, so that each record is whole before it is queued: a wake
     * reads its `cpu` to pick the processor to interrupt. */
    static process_t a, b, c;
    int i;

    wait_queue_init(&wq);
    CHECK(total_queued(&wq) == 0);

    a.state = PROC_BLOCKED;
    b.state = PROC_BLOCKED;
    c.state = PROC_BLOCKED;

    /* enqueue adds a waiter once. */
    enqueue(&wq, &a);
    enqueue(&wq, &b);
    enqueue(&wq, &a); /* already queued */
    CHECK(count_in(&wq, &a) == 1);
    CHECK(count_in(&wq, &b) == 1);
    CHECK(total_queued(&wq) == 2);

    /* dequeue removes a waiter, and a second dequeue does nothing. */
    dequeue(&wq, &a);
    dequeue(&wq, &a); /* already gone */
    CHECK(count_in(&wq, &a) == 0);
    CHECK(total_queued(&wq) == 1);

    /* wq_wake_all makes every BLOCKED waiter READY and empties the queue. */
    enqueue(&wq, &a);
    enqueue(&wq, &c);
    CHECK(total_queued(&wq) == 3); /* a, b, c */
    wq_wake_all(&wq);
    CHECK(a.state == PROC_READY);
    CHECK(b.state == PROC_READY);
    CHECK(c.state == PROC_READY);
    CHECK(total_queued(&wq) == 0); /* the wake empties the list */

    /* The wake is counted for every kind (wake_kinds). */
    CHECK(wq_wakes(&wq, WQ_DATA) == 1 && wq_wakes(&wq, WQ_ROOM) == 1);
    CHECK(wq_wakes(&wq, WQ_HANGUP) == 1 && wq_wakes(&wq, WQ_ALL) == 3);

    /* A non-BLOCKED waiter is left alone but still dequeued. */
    wait_queue_init(&wq);
    a.state = PROC_RUNNING;
    b.state = PROC_BLOCKED;
    enqueue(&wq, &a);
    enqueue(&wq, &b);
    wq_wake_all(&wq);
    CHECK(a.state == PROC_RUNNING); /* not forced READY */
    CHECK(b.state == PROC_READY);
    CHECK(total_queued(&wq) == 0);

    /* Sixty-four waiters: the list has no fixed size. */
    wait_queue_init(&wq);
    {
        static process_t pool[64];
        for (i = 0; i < 64; i++) {
            pool[i].state = PROC_BLOCKED;
            enqueue(&wq, &pool[i]);
        }
        CHECK(total_queued(&wq) == 64);
        wq_wake_all(&wq);
        for (i = 0; i < 64; i++) {
            CHECK(pool[i].state == PROC_READY);
        }
        CHECK(total_queued(&wq) == 0);
    }

    /* The wait arms the deadline a wake cleared before it blocks again. */
    {
        static process_t sleeper;

        wait_queue_init(&wq);
        sleeper.waiter.proc = &sleeper;
        sleeper.state = PROC_RUNNING;
        sleeper.wakeup_tick = 77;
        current_proc = &sleeper;
        CHECK(wq_wait_event(&wq, true_third, 0) == 0);
        CHECK(blocks == 2);
        CHECK(deadline_at_block[0] == 77 && deadline_at_block[1] == 77);
        CHECK(sleeper.state == PROC_RUNNING && total_queued(&wq) == 0);
        CHECK(wq.lock.locked == 0 && wq_host_held == 0);
        current_proc = 0;
    }
    wake_kinds();
    wake_order();
    filed_first();
    own_locks();
    return fails > 255 ? 255 : fails;
}
