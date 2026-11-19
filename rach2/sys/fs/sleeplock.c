/*
 * The sleeping lock of a mount or a file, and the record of its longest hold
 * across a disk wait (sleeplock.h).
 */

#include "sleeplock.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/tgroup.h"
#include "../timer.h"

/* The kinds of lock, in the order a lock first named each, and the lock of
 * the list and of the records' counts. */
static struct sleep_lock_record *records;
static struct spinlock records_lock = SPINLOCK_INIT_NAMED("sleep_lock_records");

int sleep_lock_stat;

#ifdef RACH2_LOCKDEP
/* The checker's class of the locks of no kind. */
static struct lockdep_key unrecorded_key;
#endif

/* The checker's notes of a take and a give, from the caller of the function
 * that makes them. */
#define DEP_CALLER ((uintptr_t)(const char *)__builtin_return_address(0))

/*
 * A waiter of `lock`, on its own stack, on the lock's list from the time it
 * finds the lock held until the lock is handed to it or it leaves. Only the
 * give that hands it the lock sets `granted`, under the list's lock, and
 * touches the record no more after that, since the waiter may return at
 * once.
 */
struct sleep_waiter {
    struct sleep_waiter *next; /* the waiter after it, or NULL */
    struct sleep_waiter *prev; /* the waiter before it, or NULL */
    struct sleep_lock *lock;
    volatile unsigned int granted; /* 1 once the lock is handed to it */
    int killable;                  /* SIGKILL ends the wait (`sleep_lock_take_killable`) */
};

/** The calling thread's ID, 0 before the first process runs. */
static int sleep_caller(void)
{
    return current_proc ? (int)current_proc->pid : 0;
}

/** The wait condition of `sleep_lock_take`: the lock is handed to the
 * waiter `arg`, a `struct sleep_waiter`. */
static int sleep_my_turn(void *arg)
{
    const struct sleep_waiter *w = arg;

    return w->granted != 0;
}

/** Whether the calling thread has SIGKILL pending, its own or its
 * process's; 0 with no process. */
static int sleep_killed(void)
{
    return current_proc &&
           ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/** The wait condition of a killable take: the caller's turn, as
 * `sleep_my_turn`, or SIGKILL pending, which `wq_wait_event_killable`
 * needs of its condition. */
static int sleep_turn_or_killed(void *arg)
{
    return sleep_my_turn(arg) || sleep_killed();
}

/**
 * Hand `l` to its first waiter, or free it when none is listed, for the
 * last give of a hold whose state is queued, or for a killed waiter the
 * lock was handed to as it left. The lock stays queued while another
 * waiter is listed and becomes held when the list empties, so that the
 * new holder's give needs no list. The exchange that hands the lock on is a
 * full barrier before the read of the queue, and the waiter passes one
 * between its filing on the queue and its test of `granted` (`wq_wait`):
 * either the waiter's test sees the lock handed or this read sees the
 * waiter. A waiter that was handed the lock may return at once, so the
 * record is not touched after the exchange.
 */
static void sleep_pass(struct sleep_lock *l)
{
    uint32_t irq = spin_lock_irqsave(&l->wait_lock);
    struct sleep_waiter *w = l->first;

    if (w) {
        l->first = w->next;
        if (l->first) {
            l->first->prev = 0;
        } else {
            l->last = 0;
            (void)arch_atomic_xchg(&l->state, SLEEP_LOCK_HELD);
        }
        (void)arch_atomic_xchg(&w->granted, 1u);
    } else {
        (void)arch_atomic_xchg(&l->state, SLEEP_LOCK_FREE);
    }
    spin_unlock_irqrestore(&l->wait_lock, irq);
    if (wq_active(&l->wq)) {
        wq_wake_all(&l->wq);
    }
}

/**
 * File `w` at the end of its lock's list, for a taker that found the lock
 * held: 0 once filed, 1 when the lock was freed meanwhile and the caller
 * took it instead, or -EINTR, with nothing filed, when the take is killable
 * and SIGKILL is pending. The state becomes queued before the record is
 * filed, both under the list's lock, so that a give that finds the state
 * held finds no waiter, and one that finds it queued takes the list's lock
 * after the filing.
 */
static int sleep_enqueue(struct sleep_waiter *w)
{
    struct sleep_lock *l = w->lock;
    uint32_t irq = spin_lock_irqsave(&l->wait_lock);
    unsigned int s;

    for (;;) {
        s = l->state;
        if (s == SLEEP_LOCK_FREE) {
            if (arch_atomic_cmpxchg(&l->state, s, SLEEP_LOCK_HELD) == s) {
                spin_unlock_irqrestore(&l->wait_lock, irq);
                return 1;
            }
        } else if (w->killable && sleep_killed()) {
            spin_unlock_irqrestore(&l->wait_lock, irq);
            return -EINTR;
        } else if (s == SLEEP_LOCK_QUEUED ||
                   arch_atomic_cmpxchg(&l->state, s, SLEEP_LOCK_QUEUED) == s) {
            break;
        }
    }
    w->next = 0;
    w->prev = l->last;
    if (l->last) {
        l->last->next = w;
    } else {
        l->first = w;
    }
    l->last = w;
    spin_unlock_irqrestore(&l->wait_lock, irq);
    return 0;
}

/**
 * Take `w` off its lock's list, for a taker that SIGKILL ends: the waiters
 * behind it move up, and the lock, still held, is no longer queued once
 * the list is empty. A waiter the lock was handed to as it left hands it
 * on (`sleep_pass`). Nothing is allocated, so the leave cannot fail.
 */
static void sleep_leave(struct sleep_waiter *w)
{
    struct sleep_lock *l = w->lock;
    uint32_t irq = spin_lock_irqsave(&l->wait_lock);

    if (w->granted) {
        spin_unlock_irqrestore(&l->wait_lock, irq);
        sleep_pass(l);
        return;
    }
    if (w->prev) {
        w->prev->next = w->next;
    } else {
        l->first = w->next;
    }
    if (w->next) {
        w->next->prev = w->prev;
    } else {
        l->last = w->prev;
    }
    if (!l->first) {
        (void)arch_atomic_xchg(&l->state, SLEEP_LOCK_HELD);
    }
    spin_unlock_irqrestore(&l->wait_lock, irq);
}

void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags)
{
    struct sleep_lock_record **pp;
    uint32_t irq;

#ifdef RACH2_LOCKDEP
    lockdep_init_map(&l->dep, record ? record->kind : "sleep_lock",
                     record ? &record->key : &unrecorded_key);
#endif
    l->holder = -1;
    l->depth = 0;
    l->owner = 0;
    l->owner_cpu = 0;
    l->state = SLEEP_LOCK_FREE;
    spin_lock_init(&l->wait_lock, "sleep_lock_wait");
    l->first = 0;
    l->last = 0;
    l->taken_at = 0;
    l->taken_by_cycles = 0;
    l->timed = 0;
    l->taken_waits = 0;
    l->flags = flags;
    l->record = record;
    wait_queue_init(&l->wq);
    if (!record) {
        return;
    }
    irq = spin_lock_irqsave(&records_lock);
    if (!record->listed) {
        pp = &records;
        while (*pp) {
            pp = &(*pp)->next;
        }
        record->next = 0;
        record->listed = 1;
        *pp = record;
    }
    spin_unlock_irqrestore(&records_lock, irq);
}

/**
 * Spin while the holder of `w`'s lock runs (`sched_spin_on`), as Linux's
 * mutex_optimistic_spin spins on a mutex's owner before it sleeps
 * (kernel/locking/mutex.c:350-385, 613): a hold is often shorter than a
 * sleep and a wake, and the wake of a halted processor costs an
 * interprocessor interrupt, which under a hypervisor is an exit for the
 * sender and an injection for the receiver. A lock handed to a waiter
 * ahead is waited for the same way while no waiter sleeps, since the one
 * ahead then spins too and runs at once; with one asleep, the one ahead
 * may be it, and its wake is long. Returns 1 when the lock was handed to
 * the caller, 0 when it should sleep or, in a killable take, has SIGKILL
 * pending. The holder's fields are read as hints, which only the holder
 * writes; the caller tests `granted` again.
 */
static int sleep_spin(const struct sleep_waiter *w)
{
    const volatile struct sleep_lock *l = w->lock;
    const struct process *owner;

    for (;;) {
        if (w->killable && sleep_killed()) {
            return 0;
        }
        if (w->granted) {
            return 1;
        }
        owner = l->owner;
        if (l->holder == -1) {
            if (wq_active(&w->lock->wq) || !sched_spin_on(0, -1)) {
                return 0;
            }
        } else if (!owner || !sched_spin_on(owner, l->owner_cpu)) {
            return 0;
        }
    }
}

/**
 * Take `l` for `sleep_lock_take` or, `killable`, for
 * `sleep_lock_take_killable`, the checker told of the take at `caller`. A
 * take of a free lock is one compare-and-exchange of `state` and passes no
 * spinlock. A take of a held one files a record on the lock's list
 * (`sleep_enqueue`) and waits until a give hands it the lock; the order
 * costs no switch of its own, since giving the lock back does not yield,
 * and the waiter runs when the scheduler picks it. Only the holder writes
 * `holder` and `depth`, so a thread that reads itself as the holder is the
 * holder, and the holder fields are its own until its last give. The
 * wait's test is made with the waiter on the queue, so that a give between
 * the two wakes it. A killable waiter looks for SIGKILL before each spin
 * and sleep, and takes its record off the list when it finds it
 * (`sleep_leave`). 0 with `l` held, or -EINTR without it.
 */
static int sleep_take(struct sleep_lock *l, int killable, uintptr_t caller)
{
    int me = sleep_caller();
    struct sleep_waiter w;
    int rc;

    (void)caller;
    if (l->holder == me && l->depth > 0) {
        l->depth++;
        return 0;
    }
#ifdef RACH2_LOCKDEP
    lockdep_acquire(&l->dep, current_proc ? current_proc->lockdep_subclass : 0, LOCKDEP_SLEEPING,
                    caller);
    if (current_proc) {
        current_proc->lockdep_subclass = 0;
    }
#endif
    w.lock = l;
    w.granted = 0;
    w.killable = killable && current_proc;
    rc = arch_atomic_cmpxchg(&l->state, SLEEP_LOCK_FREE, SLEEP_LOCK_HELD) == SLEEP_LOCK_FREE
             ? 1
             : sleep_enqueue(&w);
    while (rc == 0 && !sleep_my_turn(&w)) {
        if (w.killable && sleep_killed()) {
            sleep_leave(&w);
            rc = -EINTR;
            break;
        }
        if (sleep_spin(&w)) {
            continue;
        }
        if (w.killable) {
            (void)wq_wait_event_killable(&l->wq, sleep_turn_or_killed, &w, "__mutex_lock");
        } else {
            (void)wq_wait_event_nointr(&l->wq, sleep_my_turn, &w);
        }
    }
    if (rc < 0) {
#ifdef RACH2_LOCKDEP
        lockdep_release(&l->dep, caller);
#endif
        return -EINTR;
    }
    l->holder = me;
    l->owner = current_proc;
    l->owner_cpu = arch_cpu_id();
    l->depth = 1;
    l->timed = *(volatile int *)&sleep_lock_stat != 0;
    if (l->timed) {
        l->taken_by_cycles = timer_cycles_agree();
        l->taken_at = l->taken_by_cycles ? arch_cycles() : timer_now_us();
        l->taken_waits = current_proc ? current_proc->disk_waits : 0;
    }
    if ((l->flags & SLEEP_LOCK_FILE) && current_proc) {
        current_proc->file_locks++;
    }
    return 0;
}

void sleep_lock_take(struct sleep_lock *l)
{
    (void)sleep_take(l, 0, DEP_CALLER);
}

int sleep_lock_take_killable(struct sleep_lock *l)
{
    return sleep_take(l, 1, DEP_CALLER);
}

/** Count the hold of `l` that is ending in its record when it was timed
 * and its holder finished a disk wait during it, under the records' lock,
 * since every lock of a kind shares its record. A hold stamped by the
 * processor's counter is converted from cycles here, the one place it is
 * read, and one stamped by the clock is read from it, so that every
 * machine times its holds, as Linux's lock_stat times a hold by
 * local_clock, which falls back from the TSC where it cannot be trusted
 * (kernel/locking/lockdep.c:246-249, 350). A counter read below the stamp
 * reads 0. Its microseconds are kept in 32 bits, so a hold longer than 71
 * minutes reads short. The holder calls it before it gives `l` up. */
static void sleep_record_hold(const struct sleep_lock *l)
{
    struct sleep_lock_record *r = l->record;
    uint32_t held, irq;
    uint64_t now;

    if (!l->timed || !r || !current_proc || current_proc->disk_waits == l->taken_waits) {
        return;
    }
    if (l->taken_by_cycles) {
        now = arch_cycles();
        held = now > l->taken_at ? (uint32_t)timer_cycles_us(now - l->taken_at) : 0;
    } else {
        held = timer_now_us() - (uint32_t)l->taken_at;
    }
    irq = spin_lock_irqsave(&records_lock);
    r->crossings++;
    if (held > r->longest_us) {
        r->longest_us = held;
        r->longest_pid = current_proc->pid;
        k_memcpy(r->longest_comm, current_proc->comm, sizeof(r->longest_comm));
    }
    spin_unlock_irqrestore(&records_lock, irq);
}

/**
 * The last give frees the holder fields, then makes a held lock free by
 * one compare-and-exchange of `state`, as Linux's `__mutex_unlock_fast`
 * frees a mutex by one atomic operation, and otherwise hands the lock to
 * the first waiter on its list (`sleep_pass`), as Linux's
 * __mutex_unlock_slowpath wakes the first waiter it finds on the lock's
 * list (kernel/locking/mutex.c:937-956).
 */
void sleep_lock_give(struct sleep_lock *l)
{
    if (!sleep_lock_held(l)) {
        k_panic("sleeplock: a give by a thread that does not hold the lock");
    }
    if (l->depth > 1) {
        l->depth--;
        return;
    }
    sleep_record_hold(l);
    if ((l->flags & SLEEP_LOCK_FILE) && current_proc) {
        current_proc->file_locks--;
    }
    l->depth = 0;
    l->owner = 0;
    l->holder = -1;
#ifdef RACH2_LOCKDEP
    lockdep_release(&l->dep, DEP_CALLER);
#endif
    if (arch_atomic_cmpxchg(&l->state, SLEEP_LOCK_HELD, SLEEP_LOCK_FREE) != SLEEP_LOCK_HELD) {
        sleep_pass(l);
    }
}

int sleep_lock_held(const struct sleep_lock *l)
{
    return l->depth > 0 && l->holder == sleep_caller();
}

void sleep_lock_assert(const struct sleep_lock *l, const char *message)
{
    if (!sleep_lock_held(l)) {
        k_panic(message);
    }
}

const struct sleep_lock_record *sleep_lock_records(void)
{
    return records;
}
