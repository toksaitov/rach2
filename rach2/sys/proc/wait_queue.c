#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "wait_queue.h"
#include "sched.h"
#include "../lib/kerrno.h"

/*
 * Save the interrupt flag and disable interrupts; restore puts it back. With
 * the big lock held, which keeps every other processor's kernel code out,
 * this is all the mutual exclusion a sleeper's test and block need against
 * an IRQ-context waker. The queues themselves, which the sleeper edits and
 * a waker walks, are under the wait queues' spinlock as well (`wq_lock`).
 * The pairs are this file's own: under WAIT_QUEUE_HOSTTEST, in the host
 * tests that include this file, there is no interrupt to mask and no other
 * processor, they are no-ops, and a wake marks the process ready itself;
 * the list logic exercised there is identical either way.
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
static void wq_lock(void)
{}
static void wq_unlock(void)
{}
static void wq_ready(process_t *p)
{
    p->state = PROC_READY;
}
#else
static struct spinlock wq_spin = SPINLOCK_INIT;

static uint32_t irq_save(void)
{
    return kirq_save();
}

static void irq_restore(uint32_t flags)
{
    kirq_restore(flags);
}

/* Interrupts off. */
static void wq_lock(void)
{
    spin_lock(&wq_spin);
}

static void wq_unlock(void)
{
    spin_unlock(&wq_spin);
}

static void wq_ready(process_t *p)
{
    sched_make_ready(p);
}
#endif

void wait_queue_init(wait_queue_t *wq)
{
    int k;

    if (wq) {
        wq->head = NULL;
        for (k = 0; k < WQ_KINDS; k++) {
            wq->wakes[k] = 0;
        }
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
 *
 * A deadline the caller armed in `wakeup_tick` before the wait is armed again
 * before each block. `signal_post` clears it when it wakes the process, and a
 * signal the wait does not end on, one blocked or ignored, would otherwise
 * send the wait back to sleep with no timer to end it. The caller's condition
 * must be true once its deadline has passed.
 */
static int wq_wait(wait_queue_t *wq, wq_cond_fn cond, void *arg, int interruptible)
{
    uint32_t flags;
    uint32_t deadline;
    int rc;

    if (!wq || !cond || !current_proc) {
        return -EINVAL;
    }

    flags = irq_save();
    deadline = current_proc->wakeup_tick;
    for (;;) {
        if (cond(arg)) {
            rc = 0;
            break;
        }
        if (interruptible && signal_pending()) {
            rc = -EINTR;
            break;
        }

        wq_lock();
        wq_enqueue(wq, &current_proc->waiter);
        wq_unlock();
        current_proc->wakeup_tick = deadline;
        current_proc->state = PROC_BLOCKED;
        current_proc->uninterruptible = !interruptible;
        irq_restore(flags);

        schedule();

        flags = irq_save();
        if (current_proc->state == PROC_BLOCKED) {
#ifndef WAIT_QUEUE_HOSTTEST
            idle_wait_irq_off();
#endif
        }
    }

    wq_lock();
    wq_dequeue(wq, &current_proc->waiter);
    wq_unlock();
    current_proc->woken = 0;
    current_proc->uninterruptible = 0;
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    return rc;
}

void wq_poll_add(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_lock(); /* caller holds interrupts off (see wait_queue.h) */
        wq_enqueue(wq, w);
        wq_unlock();
    }
}

void wq_poll_remove(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_lock();
        wq_dequeue(wq, w);
        wq_unlock();
    }
}

int wq_active(const wait_queue_t *wq)
{
    return wq != NULL && wq->head != NULL;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    uint32_t flags;
    struct wq_waiter *w, *next;
    int k;

    if (!wq) {
        return;
    }
    flags = irq_save();
    wq_lock();
    w = wq->head;
    wq->head = NULL;
    for (k = 0; k < WQ_KINDS; k++) {
        if (kinds & (1u << k)) {
            wq->wakes[k]++;
        }
    }
    wq_unlock();
    for (; w; w = next) {
        next = w->next;
        w->next = NULL;
        if (w->proc && w->proc->state == PROC_BLOCKED) {
            w->proc->woken = 1;
            wq_ready(w->proc);
        }
    }
    irq_restore(flags);
}

void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

uint32_t wq_wakes(const wait_queue_t *wq, uint32_t kinds)
{
    uint32_t sum = 0;
    int k;

    for (k = 0; k < WQ_KINDS; k++) {
        if (kinds & (1u << k)) {
            sum += wq->wakes[k];
        }
    }
    return sum;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait(wq, cond, arg, 1);
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait(wq, cond, arg, 0);
}
