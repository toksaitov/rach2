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
 * no-ops; the queue array logic exercised there is identical either way.
 */
#ifdef WAIT_QUEUE_HOSTTEST
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t flags) { (void)flags; }
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
    int i;

    if (!wq) {
        return;
    }
    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        wq->waiters[i] = NULL;
    }
}

/* Add p to wq if not already queued (idempotent). Caller holds interrupts
 * off. */
static void wq_enqueue(wait_queue_t *wq, process_t *p)
{
    int i;
    int free_slot = -1;

    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->waiters[i] == p) {
            return;
        }
        if (wq->waiters[i] == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        wq->waiters[free_slot] = p;
    }
}

/* Remove p from wq (idempotent). Caller holds interrupts off. */
static void wq_dequeue(wait_queue_t *wq, process_t *p)
{
    int i;

    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->waiters[i] == p) {
            wq->waiters[i] = NULL;
            return;
        }
    }
}

/**
 * Block the current process on `wq` until `cond` has or a signal arrives.
 *
 * Re-checks `cond(arg)` and re-enqueues on every wakeup, so it is robust to
 * spurious wakes and to the condition flipping back before we run. Returns 0
 * when the condition became true, `-EINTR` if a pending signal cut the wait
 * short, `-EINVAL` on a bad argument or no current process.
 *
 * The sleep is interrupt-discipline critical: the condition test, the enqueue,
 * and the transition to BLOCKED all happen with interrupts off, so a waker IRQ
 * either ran before the test (and we saw the condition true) or after the
 * enqueue (and finds us queued). See the in-body notes for the idle path.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
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
        if (signal_pending()) {
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
        wq_enqueue(wq, current_proc);
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
            __asm__ __volatile__("sti; hlt; cli");
#endif
        }
    }

    /*
     * Reached from a break with interrupts off: leave the queue and become
     * runnable again, atomically with any waker. We are executing, so
     * RUNNING is correct whether a wake/signal had set us READY or we never
     * slept.
     */
    wq_dequeue(wq, current_proc);
    current_proc->state = PROC_RUNNING;
    irq_restore(flags);
    return rc;
}

void wq_poll_add(wait_queue_t *wq, process_t *p)
{
    if (wq && p) {
        wq_enqueue(wq, p); /* caller holds interrupts off (see wait_queue.h) */
    }
}

void wq_poll_remove(wait_queue_t *wq, process_t *p)
{
    if (wq && p) {
        wq_dequeue(wq, p);
    }
}

int wq_active(const wait_queue_t *wq)
{
    int i;

    if (!wq) {
        return 0;
    }
    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->waiters[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    uint32_t flags;
    int i;

    if (!wq) {
        return;
    }

    flags = irq_save();
    for (i = 0; i < WQ_MAX_WAITERS; i++) {
        process_t *p = wq->waiters[i];
        if (p) {
            if (p->state == PROC_BLOCKED) {
                p->state = PROC_READY;
            }
            wq->waiters[i] = NULL;
        }
    }
    irq_restore(flags);
}
