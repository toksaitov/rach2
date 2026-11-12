/* The wait queue, on which a process sleeps until a condition holds. */
#ifndef _WAIT_QUEUE_H_
#define _WAIT_QUEUE_H_

#include "../lib/kint.h"
#include "../lib/spinlock.h"
#include "process.h"

/*
 * A queue is the set of processes blocked waiting for one condition (data on a
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
 * has a size. A sleeper files itself and is marked BLOCKED before it tests
 * its condition, with interrupts off until it blocks, and each queue's list
 * and its wakes are under the queue's own spinlock, as Linux's
 * `wq_head->lock` (kernel/sched/wait.c:8-13), so no wake is lost to a
 * waker on any processor and wakers of two queues do not wait on each
 * other (wait_queue.c).
 *
 * A wake says what kind of change it is for the pollers of the queue: data
 * to read, room to write, or a hangup or an error. The queue counts the
 * wakes of each kind, whoever was on it, so a reader can tell that its
 * object changed in a way it watches since it looked, as an edge-triggered
 * epoll registration does.
 */
#define WQ_DATA   1u
#define WQ_ROOM   2u
#define WQ_HANGUP 4u
#define WQ_ALL    (WQ_DATA | WQ_ROOM | WQ_HANGUP)
#define WQ_KINDS  3

typedef struct wait_queue {
    /* The sleepers' and pollers' records, the oldest first. */
    struct wq_waiter *head;
    /* Over `head`, `wakes` and the wakes made from the list; zero is free,
     * so a queue in a zeroed record needs no init. */
    struct spinlock lock;
    /* The wakes of each kind, in the order of their bits. They wrap. */
    uint32_t wakes[WQ_KINDS];
} wait_queue_t;

/*
 * Wait condition predicate: returns non-zero when the wait should end (the
 * event the caller is waiting for has happened). wq_wait_event calls it
 * with interrupts disabled before each sleep, after the process is on the
 * queue and BLOCKED, which is what makes the sleep lost-wakeup-safe; it
 * must not sleep.
 */
typedef int (*wq_cond_fn)(void *arg);

/** Make `wq` an empty queue, with no wakes counted and its lock free; a
 * NULL `wq` is ignored. Called once before the queue's first use, never on
 * a queue in use. */
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
 * `wq_wait_event` with `name`, a static string, as the function the sleep
 * began in, which /proc/PID/wchan gives while the process sleeps; Linux's
 * name of the function that sleeps there, where Linux has one. The waits of
 * the three calls above give their own names.
 */
int wq_wait_event_as(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name);

/**
 * The same wait with a pending signal ignored, for a lock held by another
 * process and for a transfer a device has in flight for the caller: an
 * interruptible wait returns at once while a signal is pending, and a waiter
 * that then retries never leaves the CPU to the holder it waits for. Returns
 * 0, or -EINVAL as `wq_wait_event` does; /proc reports the sleeper in the
 * state D.
 */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg);

/**
 * The same wait as `wq_wait_event_nointr` where Linux sleeps in
 * TASK_KILLABLE: the vfork parent's wait and execve's for the threads it
 * has sent SIGKILL. The caller's condition must return true on SIGKILL
 * pending for the thread or its process; the wait itself tests no signal
 * and returns 0 when the condition is true. /proc reports the sleeper in
 * the state D, and the hung-task detector skips it, as Linux's skips one with
 * TASK_WAKEKILL (kernel/hung_task.c:206-210). `name` is the wait's name
 * for /proc/PID/wchan.
 */
int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name);

/**
 * The same wait as `wq_wait_event_nointr` for a kernel thread that has no
 * work: /proc reports the sleeper in the state S and does not count it in
 * `procs_blocked`, as Linux does not count a thread in TASK_IDLE. A kernel
 * thread never returns to user mode to take a signal, so a wait a signal
 * ends would return at once without end once one was pending.
 */
int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg);

/**
 * `wq_wait_event` for a caller that does not hold the big lock, which
 * `signal_pending` needs: the wait ends with -EINTR when the thread's
 * signal-work flag (`sig_woken`) is set, read without a lock and changing
 * nothing, as Linux's kvm_vcpu_check_block reads TIF_SIGPENDING through
 * signal_pending (virt/kvm/kvm_main.c:3738, include/linux/sched/signal.h:
 * 404-415). A signal wakes the sleeper as it wakes one of `wq_wait_event`.
 * The caller learns under the big lock which signal, if any, is taken;
 * an ignored signal's flag is cleared there, as the flag's recomputation
 * in `signal_pending` does. `name` is the wait's name for /proc/PID/wchan.
 */
int wq_wait_event_sigflag(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name);

/**
 * Make every process currently blocked on `wq` runnable, with one preferred
 * selection by the scheduler, empty the queue, and count the wake for each
 * kind of change among `kinds`. The kinds are those the change can bring to
 * a poller of the queue; a wake for sleepers alone, such as room in a
 * socket's queue for the senders that wait on it, passes none. A NULL `wq`
 * is ignored. Safe to call from an interrupt handler, since it turns
 * interrupts off around the walk; a woken sleeper re-checks its own
 * condition, so a spurious wake is harmless. The interrupts that tell other
 * processors to select are sent after the queue's lock is let go.
 */
void wq_wake(wait_queue_t *wq, uint32_t kinds);

/** Call `fn` with the process of each waiter on `wq` and `arg`, under the
 * queue's lock, with interrupts off, as timerfd's expiry starts each
 * waiter's wake record (`waketime_expire`); `fn` must not sleep or take the
 * queue's lock. */
void wq_each_waiter(wait_queue_t *wq, void (*fn)(struct process *p, uint64_t arg), uint64_t arg);

/**
 * `wq_wake_all` for a waker that holds a spinlock of its own: the
 * processors the wakes would interrupt are added to the set `*cpus`, one
 * bit each, which the caller passes to `sched_kick_cpus` once its lock is
 * let go, as Linux's futex_wake fills a wake_q under the bucket's lock and
 * wakes it after (kernel/futex/waitwake.c:160-199).
 */
void wq_wake_all_deferred(wait_queue_t *wq, uint64_t *cpus);

/** wq_wake for every kind of change: for a queue no poller watches, and for
 * a change of every kind, such as a close. */
void wq_wake_all(wait_queue_t *wq);

/**
 * Set `*done` to 1 and wake every process on `wq`, both under the queue's
 * lock, as Linux's `complete` sets its count under the wait's lock: a
 * waiter whose condition reads `*done` leaves its queue under the same
 * lock, so the caller is done with `wq` and `done` before the waiter can
 * return and let the memory that holds them go, a record on its stack. The
 * interrupts the wakes need are sent after, from the caller's stack.
 */
void wq_complete(wait_queue_t *wq, volatile int *done);

/** Make `p` runnable if it is blocked, as a wake of a queue it is on would,
 * for a waker that has taken it off its queues itself. */
void wq_wake_proc(process_t *p);

/** The wakes `wq` has counted of the kinds among `kinds`, summed. */
uint32_t wq_wakes(const wait_queue_t *wq, uint32_t kinds);

/**
 * poll/select support: a poller puts a record of its own on every polled
 * object's queue with wq_poll_add, sleeps once, and on any wake takes each
 * record off with wq_poll_remove; a wake of any object's queue wakes it. Both
 * are idempotent (a wake already removed the waiter), ignore a NULL `wq` or
 * `w`, and must be called with interrupts off. They do not block or change
 * the process's state; the poll core makes the BLOCKED, schedule() and
 * RUNNING steps itself, in the order wq_wait_event makes them, so that no
 * wake is lost.
 */
void wq_poll_add(wait_queue_t *wq, struct wq_waiter *w);
void wq_poll_remove(wait_queue_t *wq, struct wq_waiter *w);

/**
 * Nonzero if a sleeper or a poller has a record on `wq`. Used to skip work
 * that is needed only while someone is waiting; for example, polling an
 * interrupt-less device (the console serial port) for input from the timer
 * tick. A caller that a waker could race reads it with interrupts off.
 */
int wq_active(const wait_queue_t *wq);

#endif /* _WAIT_QUEUE_H_ */
