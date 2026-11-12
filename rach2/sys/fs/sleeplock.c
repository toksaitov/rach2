/*
 * The sleeping lock of a mount or a file, and the record of its longest hold
 * across a disk wait (sleeplock.h).
 */

#include "sleeplock.h"

#include "../arch/arch.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/sched.h"
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

/* A waiter's place in the line for `lock`. */
struct sleep_turn {
    struct sleep_lock *lock;
    uint32_t place;
};

/** The calling thread's ID, 0 before the first process runs. */
static int sleep_caller(void)
{
    return current_proc ? (int)current_proc->pid : 0;
}

/** The wait condition of `sleep_lock_take`: the place of `arg`, a
 * `struct sleep_turn`, is the one served, which the last give of the hold
 * before it made so after it freed the lock. */
static int sleep_my_turn(void *arg)
{
    const struct sleep_turn *t = arg;

    return t->lock->serving == t->place;
}

/** Take the next place in the line of `l` and return it: one
 * compare-and-exchange of `next` when no other taker draws one at the same
 * moment, as Linux's `__mutex_trylock_fast` takes a free mutex by one. */
static uint32_t sleep_place(struct sleep_lock *l)
{
    uint32_t n;

    do {
        n = l->next;
    } while (arch_atomic_cmpxchg(&l->next, n, n + 1u) != n);
    return n;
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
    l->next = 0;
    l->serving = 0;
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
 * Spin while the holder of `t`'s lock runs (`sched_spin_on`), as Linux's
 * mutex_optimistic_spin spins on a mutex's owner before it sleeps
 * (kernel/locking/mutex.c:350-385, 613): a hold is often shorter than a
 * sleep and a wake, and the wake of a halted processor costs an
 * interprocessor interrupt, which under a hypervisor is an exit for the
 * sender and an injection for the receiver. A lock given to a waiter
 * ahead is waited for the same way while no waiter sleeps, since the one
 * ahead then spins too and takes it at once; with one asleep, the one
 * ahead may be it, and its wake is long. Returns 1 when the caller's turn
 * came, 0 when it should sleep. The holder's fields are read as hints,
 * which only the holder writes; the caller tests its turn again.
 */
static int sleep_spin(const struct sleep_turn *t)
{
    const volatile struct sleep_lock *l = t->lock;
    const struct process *owner;

    for (;;) {
        owner = l->owner;
        if (l->holder == -1) {
            if (l->serving == t->place) {
                return 1;
            }
            if (wq_active(&t->lock->wq) || !sched_spin_on(0, -1)) {
                return 0;
            }
        } else if (!owner || !sched_spin_on(owner, l->owner_cpu)) {
            return 0;
        }
    }
}

/**
 * The order costs no switch of its own: giving the lock back does not
 * yield, and the waiter whose place is next runs when the scheduler picks
 * it. A take with no one in line draws the place served and has the lock
 * with no wait and no spinlock. Only the holder writes `holder`, `depth`
 * and `serving`, so a thread that reads itself as the holder is the holder,
 * and the holder fields are its own until its last give. The wait's test is
 * made with the waiter on the queue, so that a give between the two wakes
 * it.
 */
void sleep_lock_take(struct sleep_lock *l)
{
    int me = sleep_caller();
    struct sleep_turn turn;

    if (l->holder == me && l->depth > 0) {
        l->depth++;
        return;
    }
#ifdef RACH2_LOCKDEP
    lockdep_acquire(&l->dep, current_proc ? current_proc->lockdep_subclass : 0, LOCKDEP_SLEEPING,
                    DEP_CALLER);
    if (current_proc) {
        current_proc->lockdep_subclass = 0;
    }
#endif
    turn.lock = l;
    turn.place = sleep_place(l);
    while (!sleep_my_turn(&turn)) {
        if (!sleep_spin(&turn)) {
            wq_wait_event_nointr(&l->wq, sleep_my_turn, &turn);
        }
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
 * The last give frees the holder fields, then serves the next place by an
 * exchange of `serving`, as Linux's `__mutex_unlock_fast` frees a mutex by
 * one atomic operation. It wakes the queue only when a waiter is on it, as
 * Linux's __mutex_unlock_slowpath wakes only a waiter it finds on the
 * lock's list (kernel/locking/mutex.c:937-956). The exchange is a full
 * barrier between the release and the read of the queue, and so is the
 * unlock of the queue a waiter files itself on before it tests its turn
 * (`wq_wait`): either the waiter's test sees the release or this read sees
 * the waiter.
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
    (void)arch_atomic_xchg(&l->serving, l->serving + 1u);
#ifdef RACH2_LOCKDEP
    lockdep_release(&l->dep, DEP_CALLER);
#endif
    if (wq_active(&l->wq)) {
        wq_wake_all(&l->wq);
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
