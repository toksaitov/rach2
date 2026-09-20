#include "wait_queue.h"
#include "sched.h"
#include "../lib/kerrno.h"

/*
 * Save EFLAGS and disable interrupts; restore puts EFLAGS (hence IF) back. On a
 * single CPU this is the whole mutual-exclusion story between a sleeper editing
 * the queue and an IRQ-context waker walking it. Kept local so the wait queue
 * does not depend on the network module's net_lock/net_unlock.
 *
 * Under the dual-target host test there is no interrupt to mask, so these are
 * no-ops; the list logic exercised there is identical either way.
 */
#ifdef WAIT_QUEUE_HOSTTEST
static uint32_t irq_save(void)
{
    return 0;
}
static void irq_restore(uint32_t flags)
{
    (void)flags;
}
#else
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
#endif

void wait_queue_init(wait_queue_t *wq)
{
    if (wq) {
        wq->head = NULL;
    }
}

/* Interrupts off. Put `w` on the queue unless it is there. */
static void wq_enqueue(wait_queue_t *wq, struct wq_waiter *w)
{
    struct wq_waiter *q;

    for (q = wq->head; q; q = q->next) {
        if (q == w) {
            return;
        }
    }
    w->next = wq->head;
    wq->head = w;
}

/* Interrupts off. Take `w` off the queue if it is there. */
static void wq_dequeue(wait_queue_t *wq, struct wq_waiter *w)
{
    struct wq_waiter **link = &wq->head;

    while (*link) {
        if (*link == w) {
            *link = w->next;
            w->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static int wq_wait(wait_queue_t *wq, wq_cond_fn cond, void *arg, int interruptible)
{
    uint32_t flags;
    int rc;

    if (!wq || !cond || !current_proc) {
        return -EINVAL;
    }

    flags = irq_save();
    for (;;) {
        if (cond(arg)) {
            rc = 0;
            break;
        }
        if (interruptible && signal_pending()) {
            rc = -EINTR;
            break;
        }

        /*
         * Enqueue and mark ourselves BLOCKED with interrupts still off, then
         * re-enable them only as we yield. A waker IRQ can therefore never run
         * against a not-yet-queued sleeper: either it ran before this critical
         * section (and changed the condition, which we saw as true above) or it
         * runs after, finds us queued, and makes us READY.
         */
        wq_enqueue(wq, &current_proc->waiter);
        current_proc->state = PROC_BLOCKED;
        irq_restore(flags);

        schedule();

        /*
         * Re-acquire interrupts-off BEFORE inspecting our state, so the
         * decision to idle and the halt are not split by a waker IRQ. If
         * schedule() returned because a waker made us READY (we ran again)
         * we are no longer BLOCKED and just re-loop. If we are still
         * BLOCKED, nothing woke us and nothing else was runnable: idle until
         * an interrupt. `sti; hlt; cli` issued from IF=0 is atomic (a wake
         * that became pending during the check fires the instant after hlt,
         * never before, so it is not lost) and leaves interrupts off again
         * for the next iteration.
         */
        flags = irq_save();
        if (current_proc->state == PROC_BLOCKED) {
#ifndef WAIT_QUEUE_HOSTTEST
            idle_wait_irq_off();
#endif
        }
    }

    /*
     * Reached from a break with interrupts off: leave the queue and become
     * runnable again, atomically with any waker. We are executing, so
     * RUNNING is correct whether a wake/signal had set us READY or we never
     * slept.
     */
    wq_dequeue(wq, &current_proc->waiter);
    current_proc->woken = 0;
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    return rc;
}

void wq_poll_add(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_enqueue(wq, w); /* caller holds interrupts off (see wait_queue.h) */
    }
}

void wq_poll_remove(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_dequeue(wq, w);
    }
}

int wq_active(const wait_queue_t *wq)
{
    return wq != NULL && wq->head != NULL;
}

void wq_wake_all(wait_queue_t *wq)
{
    uint32_t flags;
    struct wq_waiter *w, *next;

    if (!wq) {
        return;
    }
    flags = irq_save();
    for (w = wq->head; w; w = next) {
        next = w->next;
        w->next = NULL;
        if (w->proc && w->proc->state == PROC_BLOCKED) {
            w->proc->woken = 1;
            w->proc->state = PROC_READY;
        }
    }
    wq->head = NULL;
    irq_restore(flags);
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait(wq, cond, arg, 1);
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait(wq, cond, arg, 0);
}
