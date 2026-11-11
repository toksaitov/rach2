/*
 * The sleeping lock of a mount or a file, and the record of its longest hold
 * across a disk wait (sleeplock.h).
 */

#include "sleeplock.h"

#include "../arch/arch.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../timer.h"

/* The kinds of lock, in the order a lock first named each, and the lock of
 * the list and of the records' counts. */
static struct sleep_lock_record *records;
static struct spinlock records_lock = SPINLOCK_INIT_NAMED("sleep_lock_records");

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

/** The wait condition of `sleep_lock_take`: the lock is free and the place
 * of `arg`, a `struct sleep_turn`, is the one served. */
static int sleep_my_turn(void *arg)
{
    const struct sleep_turn *t = arg;

    return t->lock->holder == -1 && t->lock->serving == t->place;
}

void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags)
{
    struct sleep_lock_record **pp;
    uint32_t irq;

    spin_lock_init(&l->spin, "sleep_lock.spin");
#ifdef RACH2_LOCKDEP
    lockdep_init_map(&l->dep, record ? record->kind : "sleep_lock",
                     record ? &record->key : &unrecorded_key);
#endif
    l->holder = -1;
    l->depth = 0;
    l->next = 0;
    l->serving = 0;
    l->taken_cycles = 0;
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
 * The order costs no switch of its own: giving the lock back does not
 * yield, and the waiter whose place is next runs when the scheduler picks
 * it. A waiter tests its turn again under the lock's spinlock before it
 * takes the lock; the wait's own test is a hint, made with the waiter on
 * the queue, so that a give between the two wakes it.
 */
void sleep_lock_take(struct sleep_lock *l)
{
    int me = sleep_caller();
    struct sleep_turn turn;
    uint32_t irq = spin_lock_irqsave(&l->spin);

    if (l->holder == me && l->depth > 0) {
        l->depth++;
        spin_unlock_irqrestore(&l->spin, irq);
        return;
    }
#ifdef RACH2_LOCKDEP
    spin_unlock_irqrestore(&l->spin, irq);
    lockdep_acquire(&l->dep, current_proc ? current_proc->lockdep_subclass : 0, LOCKDEP_SLEEPING,
                    DEP_CALLER);
    if (current_proc) {
        current_proc->lockdep_subclass = 0;
    }
    irq = spin_lock_irqsave(&l->spin);
#endif
    turn.lock = l;
    turn.place = l->next++;
    while (!sleep_my_turn(&turn)) {
        spin_unlock_irqrestore(&l->spin, irq);
        wq_wait_event_nointr(&l->wq, sleep_my_turn, &turn);
        irq = spin_lock_irqsave(&l->spin);
    }
    l->holder = me;
    l->depth = 1;
    l->taken_cycles = arch_cycles();
    l->taken_waits = current_proc ? current_proc->disk_waits : 0;
    spin_unlock_irqrestore(&l->spin, irq);
    if ((l->flags & SLEEP_LOCK_FILE) && current_proc) {
        current_proc->file_locks++;
    }
}

/** Count the hold of `l` that is ending in its record when its holder
 * finished a disk wait during it, under the records' lock, since every
 * lock of a kind shares its record. The hold is converted from processor
 * cycles here, the one place it is read; it reads 0 where the counter was
 * not calibrated, and its microseconds are kept in 32 bits, so a hold longer
 * than 71 minutes reads short. The holder calls it before it gives `l` up. */
static void sleep_record_hold(const struct sleep_lock *l)
{
    struct sleep_lock_record *r = l->record;
    uint32_t held, irq;

    if (!r || !current_proc || current_proc->disk_waits == l->taken_waits) {
        return;
    }
    held = (uint32_t)timer_cycles_us(arch_cycles() - l->taken_cycles);
    irq = spin_lock_irqsave(&records_lock);
    r->crossings++;
    if (held > r->longest_us) {
        r->longest_us = held;
        r->longest_pid = current_proc->pid;
        k_memcpy(r->longest_comm, current_proc->comm, sizeof(r->longest_comm));
    }
    spin_unlock_irqrestore(&records_lock, irq);
}

void sleep_lock_give(struct sleep_lock *l)
{
    uint32_t irq;

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
    irq = spin_lock_irqsave(&l->spin);
    l->depth = 0;
    l->holder = -1;
    l->serving++;
    spin_unlock_irqrestore(&l->spin, irq);
#ifdef RACH2_LOCKDEP
    lockdep_release(&l->dep, DEP_CALLER);
#endif
    wq_wake_all(&l->wq);
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
