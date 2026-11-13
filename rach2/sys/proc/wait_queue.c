/* wait_queue.c: the sleep on a wait queue and the wake, as wait_queue.h
 * describes them. */
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "wait_queue.h"
#include "sched.h"
#include "smp.h"
#include "../lib/kerrno.h"

/* The kinds of wait: one a signal ends, one it does not, one it does not
 * that /proc counts as idle rather than blocked, one whose caller's
 * condition must accept SIGKILL, which the hung-task detector skips, and
 * one the signal-work flag alone ends, for a caller without the big lock. */
#define WQ_SIGNAL   0
#define WQ_NOSIGNAL 1
#define WQ_IDLE     2
#define WQ_KILLABLE 3
#define WQ_SIGFLAG  4

/*
 * Save the interrupt flag and disable interrupts; restore puts it back,
 * which keeps a waker on the sleeper's own processor out between its
 * filing and its block. Each queue's list, which the sleeper edits and a
 * waker walks, and the wakes made from it are under the queue's own
 * spinlock (`wq_lock`), which keeps out the other processors' wakers of
 * that queue, with the big lock or without it, and no other queue's.
 * A wake tests and sets the sleeper's state under the run queues' lock
 * (`sched_wake_blocked`), so that wakers of two queues, or a queue's and a
 * signal's, do not both make it ready; the interrupts that tell other
 * processors to select go out after the queue's lock (`wq_kick`).
 * The pairs are this file's own: under WAIT_QUEUE_HOSTTEST, in the host
 * tests that include this file, there is no interrupt to mask and no other
 * processor; a lock is a word that is set and cleared, `wq_host_held`
 * counts the queues held, a wake marks the process ready itself and asks
 * for an interrupt to the processor it ran on, and `wq_kick` counts the
 * interrupts and those sent while a queue was held. The list logic
 * exercised there is identical either way.
 */
#ifdef WAIT_QUEUE_HOSTTEST
static int wq_host_held, wq_host_kicks, wq_host_kicks_held;

static uint32_t irq_save(void)
{
    return 0;
}
static void irq_restore(uint32_t flags)
{
    (void)flags;
}
/** Mark the lock of `wq` held and count it. */
static void wq_lock(wait_queue_t *wq)
{
    wq->lock.locked = 1;
    wq_host_held++;
}
/** Mark the lock of `wq` free and count it gone. */
static void wq_unlock(wait_queue_t *wq)
{
    wq->lock.locked = 0;
    wq_host_held--;
}
/** Make `p` ready and woken if it is blocked: the processor it ran on, to
 * interrupt, or -1. */
static int wq_ready(process_t *p)
{
    if (p->state != PROC_BLOCKED) {
        return -1;
    }
    p->woken = 1;
    p->state = PROC_READY;
    return (int)((unsigned int)p->cpu % NR_CPUS);
}
/** Count the interrupts of the set `cpus`, and those sent while a queue's
 * lock is held. */
static void wq_kick(uint64_t cpus)
{
    for (; cpus != 0; cpus >>= 1) {
        if (cpus & 1u) {
            wq_host_kicks++;
            wq_host_kicks_held += wq_host_held != 0;
        }
    }
}
#else
static uint32_t irq_save(void)
{
    return kirq_save();
}

static void irq_restore(uint32_t flags)
{
    kirq_restore(flags);
}

#ifdef RACH2_LOCKDEP
/* The class of every queue's lock, as one init site's in Linux. */
static struct lockdep_key wq_lock_key;
#endif

/**
 * Take the spinlock of `wq`; interrupts are off. In the lockdep build a
 * queue in a zeroed record, never passed to `wait_queue_init`, is given
 * the queues' class here before its first take; every taker writes the
 * same two words, and each reads its own write.
 */
static void wq_lock(wait_queue_t *wq)
{
#ifdef RACH2_LOCKDEP
    if (wq->lock.dep.key == NULL) {
        wq->lock.dep.name = "wq_head->lock";
        wq->lock.dep.key = &wq_lock_key;
    }
#endif
    spin_lock(&wq->lock);
}

/** Let the spinlock of `wq` go. */
static void wq_unlock(wait_queue_t *wq)
{
    spin_unlock(&wq->lock);
}

/** Make the woken process `p` ready if it is blocked
 * (`sched_wake_blocked`): the processor to interrupt, or -1. */
static int wq_ready(process_t *p)
{
    return sched_wake_blocked(p);
}

/** Send the interrupts of the set `cpus` (`sched_kick_cpus`). */
static void wq_kick(uint64_t cpus)
{
    sched_kick_cpus(cpus);
}
#endif

/** Add processor `cpu`, unless it is -1, to the set `*cpus`. */
static void wq_kick_add(uint64_t *cpus, int cpu)
{
    if (cpu >= 0) {
        *cpus |= (uint64_t)1 << cpu;
    }
}

void wait_queue_init(wait_queue_t *wq)
{
    int k;

    if (wq) {
        wq->head = NULL;
        wq->lock.locked = 0;
#if defined(RACH2_LOCKDEP) && !defined(WAIT_QUEUE_HOSTTEST)
        lockdep_init_map(&wq->lock.dep, "wq_head->lock", &wq_lock_key);
#endif
        for (k = 0; k < WQ_KINDS; k++) {
            wq->wakes[k] = 0;
        }
    }
}

/**
 * Put `w` at the tail of the queue unless it is on it; the queue's lock is
 * held.
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

/* Take `w` off the queue if it is there; the queue's lock is held. */
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
 * `wq_wait_event_killable`, `wq_wait_event_idle` and `wq_wait_event_sigflag`:
 * a pending signal ends a wait of kind WQ_SIGNAL, the signal-work flag one
 * of kind WQ_SIGFLAG, a sleeper of kind WQ_NOSIGNAL or WQ_KILLABLE is
 * marked uninterruptible, and one of WQ_KILLABLE killable too. `name` is
 * the wait's name for /proc/PID/wchan, stored before each block.
 *
 * The process files itself on the queue and is marked BLOCKED, under the
 * queue's lock, whose release is a full barrier, before it tests the
 * condition and the signal, as Linux's `prepare_to_wait` comes before the
 * test (kernel/sched/wait.c:229). A waker on any processor therefore
 * either changed the condition before the test, which then sees it true,
 * or wakes after it and finds the process on the queue and BLOCKED, and
 * makes it ready, in which case `schedule` keeps it running. Interrupts are
 * off from the filing to `schedule`. Interrupts go off again before the
 * state is read after `schedule`: still BLOCKED means nothing woke the
 * process and nothing else was ready, so it idles in `idle_wait_irq_off`,
 * whose `sti; hlt; cli` from IF=0 loses no wake. On the way out, with
 * interrupts still off, the process leaves the queue under its lock, as
 * `finish_wait` (wait.c:356), loses its wake mark and is RUNNING, whether a
 * wake had set it READY or it never slept. The condition is called with
 * the process BLOCKED, and must not sleep.
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
        wq_lock(wq);
        wq_enqueue(wq, &current_proc->waiter);
        current_proc->wakeup_tick = deadline;
        current_proc->wchan = name;
        current_proc->uninterruptible = kind == WQ_NOSIGNAL || kind == WQ_KILLABLE;
        current_proc->killable = kind == WQ_KILLABLE;
        current_proc->state = PROC_BLOCKED;
        wq_unlock(wq);
        if (cond(arg)) {
            rc = 0;
            break;
        }
        if (kind == WQ_SIGNAL && signal_pending()) {
            rc = -EINTR;
            break;
        }
        if (kind == WQ_SIGFLAG && *(volatile uint32_t *)&current_proc->sig_woken) {
            rc = -EINTR;
            break;
        }
        irq_restore(flags);

        schedule();

        flags = irq_save();
        if (current_proc->state == PROC_BLOCKED) {
#ifndef WAIT_QUEUE_HOSTTEST
            idle_wait_irq_off();
#endif
        }
    }

    wq_lock(wq);
    wq_dequeue(wq, &current_proc->waiter);
    current_proc->woken = 0;
    current_proc->uninterruptible = 0;
    current_proc->killable = 0;
    current_proc->state = PROC_RUNNING;
    wq_unlock(wq);
    irq_restore(flags);
    return rc;
}

void wq_poll_add(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_lock(wq);
        wq_enqueue(wq, w);
        wq_unlock(wq);
    }
}

void wq_poll_remove(wait_queue_t *wq, struct wq_waiter *w)
{
    if (wq && w) {
        wq_lock(wq);
        wq_dequeue(wq, w);
        wq_unlock(wq);
    }
}

void wq_each_waiter(wait_queue_t *wq, void (*fn)(struct process *p, uint64_t arg), uint64_t arg)
{
    struct wq_waiter *w;
    uint32_t flags;

    if (!wq || !fn) {
        return;
    }
    flags = irq_save();
    wq_lock(wq);
    for (w = wq->head; w; w = w->next) {
        if (w->proc) {
            fn(w->proc, arg);
        }
    }
    wq_unlock(wq);
    irq_restore(flags);
}

int wq_active(const wait_queue_t *wq)
{
    return wq != NULL && wq->head != NULL;
}

/**
 * Wake every process on `wq` and count the wake of each kind among `kinds`;
 * the queue's lock is held. Each wake is made under the lock, as Linux's
 * `__wake_up_common_lock` walks its list under the head's lock
 * (kernel/sched/wait.c:99-108): a record leaves its queue only under the
 * lock, so a poller's or a stack's record is not read after its owner has
 * let it go. The processors the wakes would interrupt are added to `*cpus`.
 */
static void wq_wake_locked(wait_queue_t *wq, uint32_t kinds, uint64_t *cpus)
{
    struct wq_waiter *w, *next;
    int k;

    w = wq->head;
    wq->head = NULL;
    for (k = 0; k < WQ_KINDS; k++) {
        if (kinds & (1u << k)) {
            wq->wakes[k]++;
        }
    }
    for (; w; w = next) {
        next = w->next;
        w->next = NULL;
        if (w->proc && w->proc->state == PROC_BLOCKED) {
            wq_kick_add(cpus, wq_ready(w->proc));
        }
    }
}

/**
 * The interrupts go out after the queue's lock, so that the queue's other
 * wakers and its sleepers on other processors do not wait while each is
 * sent, and before the interrupt flag comes back, from the caller's stack,
 * since `wq` may be gone once its lock is let go.
 */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    uint64_t cpus = 0;
    uint32_t flags;

    if (!wq) {
        return;
    }
    flags = irq_save();
    wq_lock(wq);
    wq_wake_locked(wq, kinds, &cpus);
    wq_unlock(wq);
    wq_kick(cpus);
    irq_restore(flags);
}

void wq_wake_all_deferred(wait_queue_t *wq, uint64_t *cpus)
{
    uint32_t flags;

    if (!wq) {
        return;
    }
    flags = irq_save();
    wq_lock(wq);
    wq_wake_locked(wq, WQ_ALL, cpus);
    wq_unlock(wq);
    irq_restore(flags);
}

void wq_complete(wait_queue_t *wq, volatile int *done)
{
    uint64_t cpus = 0;
    uint32_t flags = irq_save();

    wq_lock(wq);
    *done = 1;
    wq_wake_locked(wq, WQ_ALL, &cpus);
    wq_unlock(wq);
    wq_kick(cpus);
    irq_restore(flags);
}

/**
 * The sleeper is taken off the queue under its lock before its wake, as
 * `wq_wake_locked` takes each, and goes back on at the tail when it files
 * itself again; a poller's record, which has no process, is passed over.
 */
void wq_wake_first(wait_queue_t *wq, void (*fn)(struct process *first, void *arg), void *arg)
{
    struct wq_waiter **link, *w = NULL;
    uint64_t cpus = 0;
    uint32_t flags;

    if (!wq || !fn) {
        return;
    }
    flags = irq_save();
    wq_lock(wq);
    for (link = &wq->head; *link; link = &(*link)->next) {
        if ((*link)->proc) {
            w = *link;
            break;
        }
    }
    fn(w ? w->proc : NULL, arg);
    if (w) {
        *link = w->next;
        w->next = NULL;
        if (w->proc->state == PROC_BLOCKED) {
            wq_kick_add(&cpus, wq_ready(w->proc));
        }
    }
    wq_unlock(wq);
    wq_kick(cpus);
    irq_restore(flags);
}

void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

void wq_wake_proc(process_t *p)
{
    uint64_t cpus = 0;
    uint32_t flags = irq_save();

    wq_kick_add(&cpus, wq_ready(p));
    wq_kick(cpus);
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

/**
 * The wait a signal ends, `wq_wait_event`'s, named `name`. `signal_pending`
 * reads and changes the signal state under the big lock, so a caller that
 * runs without it sleeps until the signal-work flag is set (WQ_SIGFLAG),
 * as Linux's wait tests TIF_SIGPENDING (include/linux/sched/signal.h:404 in
 * 6.12), and then judges the flag under the big lock, as Linux dequeues
 * under `sighand->siglock` (kernel/signal.c:632). An ignored signal is
 * discarded there and the flag cleared, and the wait sleeps again, with the
 * caller's deadline armed again, since the signal's wake cleared it. No
 * spinlock is held at a wait, so the big lock may be taken here.
 */
static int wq_wait_signal(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    uint64_t deadline;
    int rc, pending;

    if (!current_proc || bkl_held()) {
        return wq_wait(wq, cond, arg, WQ_SIGNAL, name);
    }
    deadline = current_proc->wakeup_tick;
    for (;;) {
        rc = wq_wait(wq, cond, arg, WQ_SIGFLAG, name);
        if (rc != -EINTR) {
            return rc;
        }
        bkl_enter();
        pending = signal_pending();
        bkl_exit();
        if (pending) {
            return -EINTR;
        }
        current_proc->wakeup_tick = deadline;
    }
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_signal(wq, cond, arg, "wq_wait_event");
}

int wq_wait_event_as(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    return wq_wait_signal(wq, cond, arg, name);
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

int wq_wait_event_sigflag(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    return wq_wait(wq, cond, arg, WQ_SIGFLAG, name);
}
