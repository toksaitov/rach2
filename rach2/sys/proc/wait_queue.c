#include "wait_queue.h"
#include "sched.h"
#include "../lib/kerrno.h"

/*
 * Save EFLAGS and disable interrupts; restore puts EFLAGS (hence IF) back. On a
 * single CPU this is all the mutual exclusion a sleeper editing the queue
 * needs against an IRQ-context waker walking it. The pair is this file's own
 * copy of `kirq_save` and `kirq_restore`: under WAIT_QUEUE_HOSTTEST, in the
 * host tests that include this file, there is no interrupt to mask and they
 * are no-ops; the list logic exercised there is identical either way.
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

/**
 * The wait behind `wq_wait_event` and `wq_wait_event_nointr`.
 *
 * The condition and the signal are tested, and the process queued and
 * marked BLOCKED, with interrupts off, and interrupts come back on only for
 * `schedule`. A waker therefore either ran before the test, which then saw
 * the condition true, or runs after it and finds the process queued.
 * Interrupts go off again before the state is read after `schedule`: still
 * BLOCKED means nothing woke the process and nothing else was ready, so it
 * idles in `idle_wait_irq_off`, whose `sti; hlt; cli` from IF=0 loses no
 * wake. On the way out, with interrupts still off, the process leaves the
 * queue, loses its wake mark and is RUNNING, whether a wake had set it
 * READY or it never slept.
 */
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

        wq_enqueue(wq, &current_proc->waiter);
        current_proc->state = PROC_BLOCKED;
        irq_restore(flags);

        schedule();

        flags = irq_save();
        if (current_proc->state == PROC_BLOCKED) {
#ifndef WAIT_QUEUE_HOSTTEST
            idle_wait_irq_off();
#endif
        }
    }

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
