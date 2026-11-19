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
 * to read, room to write, or a hangup or an error, as the key of Linux's
 * wake (include/linux/poll.h); an epoll registration is told only of the
 * kinds its events watch. The queue counts the wakes of each kind, whoever
 * was on it.
 *
 * A record with a function (`struct wq_waiter`'s `func`) stays on the
 * queue through every wake, which calls the function under the queue's
 * lock, as Linux's wait entries with a wake function stay
 * (kernel/sched/wait.c:73-97): a poll's records, until the poll ends, and
 * an epoll registration's, until it is removed. A record marked
 * WQ_EXCLUSIVE is one of the queue's exclusive records: a wake calls
 * every other record and the exclusive ones in turn until one of their
 * functions says it woke a waiter, as Linux's wake of one exclusive waiter
 * (`nr_exclusive`, wait.c:92).
 */
#define WQ_DATA   1u
#define WQ_ROOM   2u
#define WQ_HANGUP 4u
#define WQ_ALL    (WQ_DATA | WQ_ROOM | WQ_HANGUP)
#define WQ_KINDS  3
/* The kind a record's function is told of when its queue is about to be
 * freed (`wq_wake_free`), Linux's POLLFREE; it is counted as no wake. */
#define WQ_FREE   8u

/* The flag of a record (`struct wq_waiter`'s `flags`) that is one of its
 * queue's exclusive records. */
#define WQ_EXCLUSIVE 1u

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

/** `wait_queue_init` with the queue's lock of the lock-order checker's
 * class `key`, named `name`, for a queue whose lock is taken under another
 * queue's; `wait_queue_init_named` passes its site's key, as Linux's
 * `init_waitqueue_head` makes a class of each site. */
void wait_queue_init_key(wait_queue_t *wq, const char *name, const struct lockdep_key *key);

#ifdef RACH2_LOCKDEP
#define wait_queue_init_named(wq, name)              \
    do {                                             \
        static struct lockdep_key wq_key_;           \
        wait_queue_init_key((wq), (name), &wq_key_); \
    } while (0)
#else
#define wait_queue_init_named(wq, name) wait_queue_init_key((wq), (name), 0)
#endif

/**
 * Block the current process on `wq` until `cond(arg)` returns nonzero.
 *
 * Returns 0, -EINTR when a signal is pending and the condition is still
 * false, or -EINVAL without a queue, a condition or a current process. May
 * sleep. On return the process is off the queue and RUNNING, and the
 * caller's interrupt flag is as it was.
 *
 * A caller may hold the big lock or not, but no spinlock. One without it
 * sleeps until the signal-work flag is set, as `wq_wait_event_sigflag`
 * does, and the wait takes the big lock to learn whether a signal ends it
 * (`signal_pending`), which goes back to sleep when the signal is ignored.
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
 * `wq_wait_event` for a caller that judges a signal itself, under the big
 * lock and after letting go of its own locks: the wait ends with -EINTR
 * when the thread's signal-work flag (`sig_woken`) is set, read without a
 * lock and changing nothing, as Linux's kvm_vcpu_check_block reads
 * TIF_SIGPENDING through signal_pending (virt/kvm/kvm_main.c:3738,
 * include/linux/sched/signal.h:404-415). A signal wakes the sleeper as it
 * wakes one of `wq_wait_event`. The caller learns under the big lock which
 * signal, if any, is taken; an ignored signal's flag is cleared there, as
 * the flag's recomputation in `signal_pending` does. `name` is the wait's
 * name for /proc/PID/wchan.
 */
int wq_wait_event_sigflag(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name);

/** `wq_wait_event_sigflag` with optional accounting. Set `*waited` to 1
 * if the thread switched out blocked or entered the idle wait, else 0.
 * Add to `*wakeups` each time a queue or deadline wake makes it ready.
 * A signal can end the wait but does not increment the counter. Keep both
 * destinations live until the call returns. */
int wq_wait_event_sigflag_counted(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name,
                                  int *waited, uint64_t *wakeups);

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
 * waiter's wake record under the run queues' lock (`waketime_expire`); `fn`
 * must not sleep or take the queue's lock. */
void wq_each_waiter(wait_queue_t *wq, void (*fn)(struct process *p, uint64_t arg), uint64_t arg);

/**
 * `wq_wake_all` for a waker that holds a spinlock of its own: the
 * processors the wakes would interrupt are added to the set `*cpus`, one
 * bit each, which the caller passes to `sched_kick_cpus` once its lock is
 * let go, as Linux's futex_wake fills a wake_q under the bucket's lock and
 * wakes it after (kernel/futex/waitwake.c:160-199).
 */
void wq_wake_all_deferred(wait_queue_t *wq, uint64_t *cpus);

/**
 * Call `fn` with the process of the oldest sleeper on `wq`, or NULL when no
 * sleeper is on it, and `arg`, then make that sleeper alone runnable, all
 * under the queue's lock, for a sleeping lock that hands itself to its
 * oldest waiter, as Linux's __mutex_unlock_slowpath hands the mutex to its
 * first waiter and wakes that one (kernel/locking/mutex.c:950-976). The
 * others stay on the queue. `fn` must not sleep or take the queue's lock.
 * Counts no wake for the pollers.
 */
void wq_wake_first(wait_queue_t *wq, void (*fn)(struct process *first, void *arg), void *arg);
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

/**
 * Make `p` runnable if it is blocked, from a record's function, which runs
 * under its queue's lock: the processor to interrupt goes to `*cpus`,
 * which the wake interrupts once the lock is let go. Returns 1 when `p` was
 * blocked, as Linux's `default_wake_function` returns whether it woke the
 * task (kernel/sched/core.c).
 */
int wq_ready_deferred(process_t *p, uint64_t *cpus);

/**
 * The wake of `wq` for the kinds `kinds` from a record's function, which
 * holds the lock of the queue it was called from: the lock of `wq` is
 * taken as the `nest`th of the queues held at once (`spin_lock_nested`),
 * the functions of its records are called with `nest`, and the processors
 * to interrupt go to `*cpus`. Interrupts are off. Linux's
 * `ep_poll_safewake` wakes a set's poll queue so (fs/eventpoll.c:776-821).
 */
void wq_wake_nested(wait_queue_t *wq, uint32_t kinds, unsigned int nest, uint64_t *cpus);

/**
 * Put the record `w`, whose `func` is set, at the tail of `wq` and record
 * the queue in `w->wq`, as Linux's `add_wait_queue`; a record with
 * WQ_EXCLUSIVE goes there too (`add_wait_queue_exclusive`). Interrupts may
 * be on.
 */
void wq_entry_add(wait_queue_t *wq, struct wq_waiter *w);

/**
 * Take the record `w` off the queue `wq_entry_add` put it on, unless
 * `wq_wake_free` has taken it already, and clear `w->wq`. The queue is
 * read and left under one lock that `wq_wake_free` also takes, so the
 * queue is not freed between, as Linux's `ep_remove_wait_queue` reads
 * `whead` under RCU (fs/eventpoll.c:823-838). Interrupts may be on.
 */
void wq_entry_remove(struct wq_waiter *w);

/**
 * Tell every record with a function on `wq` that the queue is about to be
 * freed, with the kind WQ_FREE, and take each off it, clearing its `wq`,
 * as Linux's `wake_up_pollfree` (kernel/sched/wait.c:209-214): for a
 * queue whose memory goes while an epoll registration made through it may
 * last, the signalfd's queue of a thread group. Interrupts may be on.
 */
void wq_wake_free(wait_queue_t *wq);

/** The wakes `wq` has counted of the kinds among `kinds`, summed. */
uint32_t wq_wakes(const wait_queue_t *wq, uint32_t kinds);

/**
 * poll/select support: a poller puts a record of its own on every polled
 * object's queue with wq_poll_add and takes each off with wq_poll_remove
 * when its wait ends; a record without a function leaves its queue at a
 * wake, and one with a function stays. Both are idempotent, ignore a NULL
 * `wq` or `w`, and must be called with interrupts off. They do not block or
 * change the process's state; the poll core makes the BLOCKED, schedule()
 * and RUNNING steps itself, so that no wake is lost.
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
