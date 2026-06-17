/*
 * wait_queue_hosttest.c
 *
 * host-deps:
 *
 * Dual-target seam test for the multi-waiter queue logic of proc/wait_queue.c.
 * The fixed-slot array ops, enqueue (idempotent), dequeue, and wq_wake_all
 * (mark every BLOCKED waiter READY and empty the queue), are the part a
 * fence-post bug could break, so they run natively here under ASan/UBSan with a
 * stand-in PCB. The block/yield path (schedule + sti;hlt) is kernel-only and is
 * covered in-guest by waitprobe and by the live httpd accept/recv wake; here we
 * exercise exactly the array bookkeeping wq_wake_all relies on.
 *
 * Built with WAIT_QUEUE_HOSTTEST, which drops the asm irq_save/restore and the
 * idle hlt and includes wait_queue.c directly so the static enqueue/dequeue are
 * reachable. Returns the failed-check count (0 == pass).
 */
#define WAIT_QUEUE_HOSTTEST

#include "proc/wait_queue.c"

/* Kernel symbols wait_queue.c references; never called by the array-logic
 * tests below (no test path sleeps), but needed to satisfy the link. */
process_t *current_proc = 0;
void schedule(void)
{}
int signal_pending(void)
{
    return 0;
}

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* Count how many slots in wq point at p. */
static int count_in(wait_queue_t *wq, process_t *p)
{
    int i, n = 0;
    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->waiters[i] == p) {
            n++;
        }
    }
    return n;
}

static int total_queued(wait_queue_t *wq)
{
    int i, n = 0;
    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->waiters[i]) {
            n++;
        }
    }
    return n;
}

int main(void)
{
    wait_queue_t wq;
    process_t a, b, c;
    int i;

    wait_queue_init(&wq);
    CHECK(total_queued(&wq) == 0);

    a.state = PROC_BLOCKED;
    b.state = PROC_BLOCKED;
    c.state = PROC_BLOCKED;

    /* enqueue adds, and is idempotent (no double-insert). */
    wq_enqueue(&wq, &a);
    wq_enqueue(&wq, &b);
    wq_enqueue(&wq, &a); /* already queued */
    CHECK(count_in(&wq, &a) == 1);
    CHECK(count_in(&wq, &b) == 1);
    CHECK(total_queued(&wq) == 2);

    /* dequeue removes one, idempotently. */
    wq_dequeue(&wq, &a);
    wq_dequeue(&wq, &a); /* already gone */
    CHECK(count_in(&wq, &a) == 0);
    CHECK(total_queued(&wq) == 1);

    /* wq_wake_all makes every BLOCKED waiter READY and empties the queue. */
    wq_enqueue(&wq, &a);
    wq_enqueue(&wq, &c);
    CHECK(total_queued(&wq) == 3); /* a, b, c */
    wq_wake_all(&wq);
    CHECK(a.state == PROC_READY);
    CHECK(b.state == PROC_READY);
    CHECK(c.state == PROC_READY);
    CHECK(total_queued(&wq) == 0); /* multi-waiter wake clears all slots */

    /* A non-BLOCKED waiter is left alone but still dequeued. */
    wait_queue_init(&wq);
    a.state = PROC_RUNNING;
    b.state = PROC_BLOCKED;
    wq_enqueue(&wq, &a);
    wq_enqueue(&wq, &b);
    wq_wake_all(&wq);
    CHECK(a.state == PROC_RUNNING); /* not forced READY */
    CHECK(b.state == PROC_READY);
    CHECK(total_queued(&wq) == 0);

    /* The fixed array holds exactly WQ_MAX_WAITERS distinct waiters. */
    wait_queue_init(&wq);
    {
        static process_t pool[WQ_MAX_WAITERS];
        for (i = 0; i < WQ_MAX_WAITERS; i++) {
            pool[i].state = PROC_BLOCKED;
            wq_enqueue(&wq, &pool[i]);
        }
        CHECK(total_queued(&wq) == WQ_MAX_WAITERS);
        wq_wake_all(&wq);
        for (i = 0; i < WQ_MAX_WAITERS; i++) {
            CHECK(pool[i].state == PROC_READY);
        }
        CHECK(total_queued(&wq) == 0);
    }

    return fails;
}
