/* wait_queue.c: the sleep on a wait queue and the wake, as wait_queue.h
 * describes them. */
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "wait_queue.h"
#include "sched.h"
#include "../lib/kerrno.h"

/* The kinds of wait: one a signal ends, one it does not, one it does not
 * that /proc counts as idle rather than blocked, and one whose caller's
 * condition must accept SIGKILL, which the hung-task detector skips. */
#define WQ_SIGNAL   0
#define WQ_NOSIGNAL 1
#define WQ_IDLE     2
#define WQ_KILLABLE 3

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
static struct spinlock wq_spin = SPINLOCK_INIT_NAMED("wq_spin");

static uint32_t irq_save(void)
{
    return kirq_save();
}

static void irq_restore(uint32_t flags)
{
    kirq_restore(flags);
}

/** Take the wait queues' spinlock; interrupts are off. */
static void wq_lock(void)
{
    spin_lock(&wq_spin);
}

/** Let the wait queues' spinlock go. */
static void wq_unlock(void)
{
    spin_unlock(&wq_spin);
}

/** Make the woken process `p` ready (`sched_make_ready`). */
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

/**
 * Put `w` at the tail of the queue unless it is on it; `wq_lock` is held.
 * A wake makes the sleepers ready in queue order and the ready list runs
 * them in that order, so the tail keeps a lock's oldest waiter first: at the
 * head, the newest waiter would take each release and starve the oldest.
 */
static void wq_enqueue(wait_queue_t *wq, struct wq_waiter *w)
{
    struct wq_waiter **link = &wq->head;

    while (*link) {
        if (*link == w) {
            return;
        }
        link = &(*link)->next;
    }
    w->next = NULL;
    *link = w;
}

/* Take `w` off the queue if it is there; `wq_lock` is held. */
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
 * The wait behind `wq_wait_event`, `wq_wait_event_as`, `wq_wait_event_nointr`,
 * `wq_wait_event_killable` and `wq_wait_event_idle`: a pending signal ends a
 * wait of kind WQ_SIGNAL, a sleeper of kind WQ_NOSIGNAL or WQ_KILLABLE is
 * marked uninterruptible, and one of WQ_KILLABLE killable too. `name` is
 * the wait's name for /proc/PID/wchan, stored before each block.
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
static int wq_wait(wait_queue_t *wq, wq_cond_fn cond, void *arg, int kind, const char *name)
{
    uint32_t flags;
    uint64_t deadline;
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
        if (kind == WQ_SIGNAL && signal_pending()) {
            rc = -EINTR;
            break;
        }

        wq_lock();
        wq_enqueue(wq, &current_proc->waiter);
        wq_unlock();
        current_proc->wakeup_tick = deadline;
        current_proc->wchan = name;
        current_proc->state = PROC_BLOCKED;
        current_proc->uninterruptible = kind == WQ_NOSIGNAL || kind == WQ_KILLABLE;
        current_proc->killable = kind == WQ_KILLABLE;
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
    current_proc->killable = 0;
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    return rc;
}

void wq_poll_add(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_lock();
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

void wq_wake_proc(process_t *p)
{
    uint32_t flags = irq_save();

    if (p->state == PROC_BLOCKED) {
        p->woken = 1;
        wq_ready(p);
    }
    irq_restore(flags);
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
    return wq_wait(wq, cond, arg, WQ_SIGNAL, "wq_wait_event");
}

int wq_wait_event_as(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    return wq_wait(wq, cond, arg, WQ_SIGNAL, name);
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait(wq, cond, arg, WQ_NOSIGNAL, "wq_wait_event_nointr");
}

int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    return wq_wait(wq, cond, arg, WQ_KILLABLE, name);
}

int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait(wq, cond, arg, WQ_IDLE, "wq_wait_event_idle");
}
