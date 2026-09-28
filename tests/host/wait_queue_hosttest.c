/**
 * Dual-target host test for the multi-waiter queue logic of proc/wait_queue.c.
 *
 * host-deps:
 *
 * The fixed-slot array ops, enqueue (idempotent), dequeue, and wq_wake_all
 * (mark every BLOCKED waiter READY and empty the queue), are the part a
 * fence-post bug could break, so they run natively here under ASan/UBSan with a
 * stand-in PCB. The block/yield path (schedule + sti;hlt) is kernel-only and is
 * covered in-guest by waitprobe and by the live httpd accept/recv wake; here we
 * exercise exactly the array bookkeeping wq_wake_all relies on.
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
 * leave it asleep with no timer to end it.
 *
 * Built with WAIT_QUEUE_HOSTTEST, which drops the asm irq_save/restore and the
 * idle hlt and includes wait_queue.c directly so the static enqueue/dequeue are
 * reachable. Returns the failed-check count (0 == pass).
 */
#define WAIT_QUEUE_HOSTTEST

#include "proc/wait_queue.c"

extern int dprintf(int fd, const char *format, ...);

process_t *current_proc = 0;
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

/* How many records on wq are p's. */
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

static int total_queued(wait_queue_t *wq)
{
    struct wq_waiter *w;
    int n = 0;

    for (w = wq->head; w; w = w->next) {
        n++;
    }
    return n;
}

static void enqueue(wait_queue_t *wq, process_t *p)
{
    p->waiter.proc = p;
    wq_enqueue(wq, &p->waiter);
}

static void dequeue(wait_queue_t *wq, process_t *p)
{
    wq_dequeue(wq, &p->waiter);
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

int main(void)
{
    wait_queue_t wq;
    process_t a, b, c;
    int i;

    wait_queue_init(&wq);
    CHECK(total_queued(&wq) == 0);

    a.waiter.next = 0;
    b.waiter.next = 0;
    c.waiter.next = 0;
    a.state = PROC_BLOCKED;
    b.state = PROC_BLOCKED;
    c.state = PROC_BLOCKED;

    /* enqueue adds, and is idempotent (no double-insert). */
    enqueue(&wq, &a);
    enqueue(&wq, &b);
    enqueue(&wq, &a); /* already queued */
    CHECK(count_in(&wq, &a) == 1);
    CHECK(count_in(&wq, &b) == 1);
    CHECK(total_queued(&wq) == 2);

    /* dequeue removes one, idempotently. */
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
    CHECK(total_queued(&wq) == 0); /* multi-waiter wake clears all slots */

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

    /* Sixty-four distinct waiters, four times the process table that was:
     * the list has no size. */
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

    /* A wake that does not end the wait clears the deadline; the wait arms
     * it again before it blocks again. */
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
        current_proc = 0;
    }
    wake_kinds();
    return fails > 255 ? 255 : fails;
}
