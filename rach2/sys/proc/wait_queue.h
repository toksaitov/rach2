#ifndef _WAIT_QUEUE_H_
#define _WAIT_QUEUE_H_

#include "../lib/kint.h"
#include "process.h"

/*
 * Wait queue: the set of processes blocked waiting for one condition (data on a
 * pipe, a datagram on a socket, a tty line, a disk request). It is the same
 * idea as the Linux kernel wait queue: a sleeper enqueues itself, marks itself
 * BLOCKED, and yields the CPU; a waker (an interrupt handler, the timer, or
 * another process) walks the queue and makes every sleeper READY again.
 *
 * Two properties pipes, poll/select, ttys, and ptys all need:
 *   * multi-waiter: several processes can block on one object at once (two
 *     readers can share a pipe, and a poll puts a record on each object);
 *   * signal-interruptible: a pending signal breaks the sleep with -EINTR, so a
 *     blocked read/recv unwinds to deliver the signal instead of hanging.
 *
 * The queue is a list of the sleepers' own records (`struct wq_waiter` in
 * process.h): a process sleeps with the record in its own record, and a
 * poller supplies one per watched object, from an array it took from the
 * heap before it began, so entering a queue allocates nothing and no queue
 * has a size. The single-CPU lock between a sleeper
 * and an IRQ waker is just "interrupts off"; there is no second core to
 * race.
 */
typedef struct wait_queue {
    struct wq_waiter *head;
    /* The times `wq_wake_all` has woken the queue, whoever was on it, so a
     * reader can tell that its object changed since it looked, as an
     * edge-triggered epoll registration does. It wraps. */
    uint32_t wakes;
} wait_queue_t;

/*
 * Wait condition predicate: returns non-zero when the wait should end (the
 * event the caller is waiting for has happened). wq_wait_event re-checks it
 * with interrupts disabled before each sleep, which is what makes the sleep
 * lost-wakeup-safe.
 */
typedef int (*wq_cond_fn)(void *arg);

/* Clear a wait queue to empty. Call once before first use. */
void wait_queue_init(wait_queue_t *wq);

/**
 * Block the current process on `wq` until `cond(arg)` returns nonzero.
 *
 * Returns 0, -EINTR when a signal is pending and the condition is still
 * false, or -EINVAL without a queue, a condition or a current process. May
 * sleep. On return the process is off the queue and RUNNING, and the
 * caller's interrupt flag is as it was.
 *
 * A deadline the caller armed in `wakeup_tick` before the call is armed again
 * before each block, since `signal_post` clears it when it wakes the process.
 * The condition must be true once that deadline has passed, and the caller
 * clears `wakeup_tick` after the wait; a caller with no deadline enters with
 * it zero.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg);

/**
 * The same wait with a pending signal ignored, for a lock held by another
 * process and for a transfer a device has in flight for the caller: an
 * interruptible wait returns at once while a signal is pending, and a waiter
 * that then retries never leaves the CPU to the holder it waits for.
 */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg);

/*
 * Make every process currently blocked on wq runnable, with one preferred
 * selection by the scheduler, empty the queue, and count the wake in
 * `wakes`. Safe to call from IRQ context (it disables interrupts around the
 * walk); a woken sleeper re-checks its own condition, so a spurious wake is
 * harmless.
 */
void wq_wake_all(wait_queue_t *wq);

/*
 * poll/select support: a poller puts a record of its own on every polled
 * object's queue with wq_poll_add, sleeps once, and on any wake takes each
 * record off with wq_poll_remove; any object's wq_wake_all wakes it. Both
 * are idempotent (a wake already removed the waiter) and must be called
 * with interrupts off. They do NOT block or change process state; the poll
 * core owns the BLOCKED/schedule()/RUNNING transitions, the same
 * lost-wakeup-safe way wq_wait_event does.
 */
void wq_poll_add(wait_queue_t *wq, struct wq_waiter *w);
void wq_poll_remove(wait_queue_t *wq, struct wq_waiter *w);

/*
 * Non-zero if any process is currently blocked on wq. Used to skip work that
 * is needed only while someone is waiting; for example, polling an
 * interrupt-less device (the console serial port) for input from the timer
 * tick. Read it with interrupts off if a concurrent waker could change it
 * under you.
 */
int wq_active(const wait_queue_t *wq);

#endif /* _WAIT_QUEUE_H_ */
