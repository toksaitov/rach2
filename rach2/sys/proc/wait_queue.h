#ifndef _WAIT_QUEUE_H_
#define _WAIT_QUEUE_H_

#include "../lib/kint.h"
#include "process.h"

/*
 * Wait queue: the set of processes blocked waiting for one condition -- data on
 * a pipe, a datagram on a socket, a tty line, a child to stop. It is the same
 * idea as the Linux kernel wait queue: a sleeper enqueues itself, marks itself
 * BLOCKED, and yields the CPU; a waker (usually an IRQ handler -- a NIC packet,
 * the timer) walks the queue and makes every sleeper READY again.
 *
 * Two properties the old single-waiter socket park lacked, and that pipes,
 * poll/select, ttys, and ptys all need:
 *   - multi-waiter: several processes can block on one object at once (poll
 *     parks one process per fd; two readers can share a pipe);
 *   - signal-interruptible: a pending signal breaks the sleep with -EINTR, so a
 *     blocked read/recv unwinds to deliver the signal instead of hanging.
 *
 * At most MAX_PROCS processes exist, so a fixed array of that many slots can
 * never overflow. The single-CPU lock between a sleeper and an IRQ waker is just
 * "interrupts off" -- there is no second core to race.
 */
#define WQ_MAX_WAITERS MAX_PROCS

typedef struct wait_queue {
    process_t *waiters[WQ_MAX_WAITERS];
} wait_queue_t;

/*
 * Wait condition predicate: returns non-zero when the wait should END (the
 * event the caller is waiting for has happened). wq_wait_event re-checks it with
 * interrupts disabled before each sleep, which is what makes the sleep
 * lost-wakeup-safe.
 */
typedef int (*wq_cond_fn)(void *arg);

/* Clear a wait queue to empty. Call once before first use. */
void wait_queue_init(wait_queue_t *wq);

/*
 * Block the current process on wq until cond(arg) is true or a signal is
 * pending. Returns 0 when cond became true, -EINTR when a signal interrupted the
 * wait. Other READY processes run while we sleep (it yields via schedule()
 * rather than spinning), and a wake delivered from IRQ context the instant
 * before we sleep is never lost.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg);

/*
 * Make every process currently blocked on wq runnable and empty the queue. Safe
 * to call from IRQ context (it disables interrupts around the walk); a woken
 * sleeper re-checks its own condition, so a spurious wake is harmless.
 */
void wq_wake_all(wait_queue_t *wq);

/*
 * poll/select support: park the current process on SEVERAL wait queues at once.
 * wq_wait_event blocks on one queue; a poller instead calls wq_poll_add on every
 * polled object's queue, then sleeps once (with interrupts disciplined by the
 * caller), and any object's wq_wake_all wakes it; on wake it calls wq_poll_remove
 * on each. Both are idempotent (a wake already removed the waiter) and must be
 * called with interrupts off. They do NOT block or change process state -- the
 * poll core owns the BLOCKED/schedule()/RUNNING transitions, the same
 * lost-wakeup-safe way wq_wait_event does.
 */
void wq_poll_add(wait_queue_t *wq, process_t *p);
void wq_poll_remove(wait_queue_t *wq, process_t *p);

#endif /* _WAIT_QUEUE_H_ */
