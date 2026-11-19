#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#include "kint.h"
#include "lockdep.h"

/*
 * A spinlock: Linux's queued spinlock, a word whose locked byte one
 * processor at a time sets, and which hands the lock to its waiters in the
 * order they came (spinlock.Readme.md).
 *
 * Taking a free lock is one compare-and-exchange, and giving it back one
 * store. The first waiter sets the word's pending bit and spins on the
 * locked byte; the others queue, each spinning on a place of its own
 * processor's, and only the queue's head reads the word. A holder that
 * gives the lock back cannot take it again ahead of a waiter, and a
 * trylock fails while any waits. Under a hypervisor that may preempt the
 * guest's processors, waiters instead take the free lock by test-and-set,
 * in no order, as Linux's guest without paravirtual spin locks does.
 *
 * A subsystem that every processor enters on its ticks, faults and
 * allocations has one of these over its data, taken with interrupts off,
 * so that an interrupt on the holding processor cannot try to take the
 * lock that processor already holds. A lock is taken after the big lock
 * (proc/smp.h), never before it, and is held for a short region that does
 * not sleep. A processor that waits for one still answers the requests
 * other processors leave it, the TLB's and the stop's, since the holder may
 * be waiting for its answer. What that answer runs, and an NMI's handler,
 * may wait for another lock, up to four levels deep, but not for the lock
 * the wait it interrupted is for, which would be queued behind its own
 * place: where waiters queue, that stops with a panic, and such code takes
 * a lock by a try.
 */
struct spinlock {
    volatile unsigned int locked; /* the locked byte, pending byte and tail; 0 when free */
#ifdef RACH2_LOCKDEP
    struct lockdep_map dep; /* its class, for the lock-order checker */
#endif
};

/*
 * A lock in the kernel's image is initialised with SPINLOCK_INIT_NAMED, or
 * SPINLOCK_INIT, and the checker keys its class by its address. A lock in a
 * record that is allocated is initialised with `spin_lock_init` where the
 * record is made, and every lock initialised at that line is of one class,
 * keyed by a static object of the line's own, as Linux's spin_lock_init
 * (include/linux/spinlock.h:331). `name` is a string constant.
 */
#ifdef RACH2_LOCKDEP
#define SPINLOCK_INIT_NAMED(name) {0, LOCKDEP_MAP_STATIC(name)}
#define SPINLOCK_INIT             SPINLOCK_INIT_NAMED(0)
#define spin_lock_init(lock, name)                          \
    do {                                                    \
        static struct lockdep_key spin_key_;                \
        (lock)->locked = 0;                                 \
        lockdep_init_map(&(lock)->dep, (name), &spin_key_); \
    } while (0)
#else
#define SPINLOCK_INIT_NAMED(name)  {0}
#define SPINLOCK_INIT              {0}
#define spin_lock_init(lock, name) ((void)((lock)->locked = 0))
#endif

/** Take `lock`, with interrupts already off. */
void spin_lock(struct spinlock *lock);

/** Take `lock`, with interrupts already off, as the `subclass`th of two or
 * more locks of one class held at once, in an order the caller keeps, such
 * as by address (`spin_lock_nested`, include/linux/spinlock.h:221): the
 * checker orders the subclasses as it orders classes. */
void spin_lock_nested(struct spinlock *lock, unsigned int subclass);

/** Take `lock` if it is free, with interrupts already off: 1 when this
 * call took it, 0 when it was held or waited for, in which case nothing
 * changed, so a try never takes the lock ahead of a waiter. A
 * caller that an interrupt may have stopped in the middle of the holder's
 * region, on the same processor, takes the lock this way, since spinning
 * there would never end. */
int spin_trylock(struct spinlock *lock);

/** Give `lock` back, leaving interrupts as they are. */
void spin_unlock(struct spinlock *lock);

/** Turn interrupts off, take `lock`, and return the flags to restore. */
uint32_t spin_lock_irqsave(struct spinlock *lock);

/** Give `lock` back and put the interrupt flag back as `flags` has it. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags);

/** Take `lock` as `spin_lock` does, without the checker: for the locks
 * the checker and its reports take themselves (Linux's arch_spin_lock). */
void raw_spin_lock(struct spinlock *lock);

/** Give back a lock `raw_spin_lock` took. */
void raw_spin_unlock(struct spinlock *lock);

#endif /* _SPINLOCK_H_ */
