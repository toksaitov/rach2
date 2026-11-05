/*
 * The sleeping lock of a mount or a file, and the record of its longest hold
 * across a disk wait (sleeplock.h).
 */

#include "sleeplock.h"

#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../timer.h"

/* The kinds of lock, in the order a lock first named each. */
static struct sleep_lock_record *records;

/* A waiter's place in the line for `lock`. */
struct sleep_turn {
    struct sleep_lock *lock;
    uint32_t place;
};

/** The calling thread's ID, 0 before the first process runs. */
static int caller(void)
{
    return current_proc ? (int)current_proc->pid : 0;
}

/** The wait condition of `sleep_lock_take`: the lock is free and the place
 * of `arg`, a `struct sleep_turn`, is the one served. */
static int my_turn(void *arg)
{
    const struct sleep_turn *t = arg;

    return t->lock->holder == -1 && t->lock->serving == t->place;
}

void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags)
{
    struct sleep_lock_record **pp;

    l->holder = -1;
    l->depth = 0;
    l->next = 0;
    l->serving = 0;
    l->taken_us = 0;
    l->taken_waits = 0;
    l->flags = flags;
    l->record = record;
    wait_queue_init(&l->wq);
    if (record && !record->listed) {
        pp = &records;
        while (*pp) {
            pp = &(*pp)->next;
        }
        record->next = 0;
        record->listed = 1;
        *pp = record;
    }
}

/**
 * The order costs no switch of its own: giving the lock back does not
 * yield, and the waiter whose place is next runs when the scheduler picks
 * it.
 */
void sleep_lock_take(struct sleep_lock *l)
{
    int me = caller();
    struct sleep_turn turn;

    if (l->holder == me && l->depth > 0) {
        l->depth++;
        return;
    }
    turn.lock = l;
    turn.place = l->next++;
    while (!my_turn(&turn)) {
        wq_wait_event_nointr(&l->wq, my_turn, &turn);
    }
    l->holder = me;
    l->depth = 1;
    l->taken_us = timer_now_us();
    l->taken_waits = current_proc ? current_proc->disk_waits : 0;
    if ((l->flags & SLEEP_LOCK_FILE) && current_proc) {
        current_proc->file_locks++;
    }
}

/** Count the hold of `l` that is ending in its record when its holder
 * finished a disk wait during it. The clock wraps after 71 minutes, so a
 * longer hold reads short. */
static void record_hold(struct sleep_lock *l)
{
    struct sleep_lock_record *r = l->record;
    uint32_t held;

    if (!r || !current_proc || current_proc->disk_waits == l->taken_waits) {
        return;
    }
    held = timer_now_us() - l->taken_us;
    r->crossings++;
    if (held > r->longest_us) {
        r->longest_us = held;
        r->longest_pid = current_proc->pid;
        k_memcpy(r->longest_comm, current_proc->comm, sizeof(r->longest_comm));
    }
}

void sleep_lock_give(struct sleep_lock *l)
{
    if (!sleep_lock_held(l)) {
        k_panic("sleeplock: a give by a thread that does not hold the lock");
    }
    if (l->depth > 1) {
        l->depth--;
        return;
    }
    record_hold(l);
    if ((l->flags & SLEEP_LOCK_FILE) && current_proc) {
        current_proc->file_locks--;
    }
    l->depth = 0;
    l->holder = -1;
    l->serving++;
    wq_wake_all(&l->wq);
}

int sleep_lock_held(const struct sleep_lock *l)
{
    return l->depth > 0 && l->holder == caller();
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
