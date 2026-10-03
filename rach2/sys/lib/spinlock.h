#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#include "kint.h"

/*
 * A spinlock: a word that one processor at a time sets, which the others
 * wait on by reading it in a loop.
 *
 * A subsystem that every processor enters on its ticks, faults and
 * allocations has one of these over its data, taken with interrupts off,
 * so that an interrupt on the holding processor cannot ask for the lock it
 * already holds. A lock is taken after the big lock (proc/smp.h), never
 * before it, and is held for a short region that does not sleep. A
 * processor that waits for one still answers the requests other processors
 * leave it, the TLB's and the stop's, since the holder may be waiting for
 * its answer.
 */
struct spinlock {
    volatile unsigned int locked;
};

#define SPINLOCK_INIT {0}

/** Take `lock`, with interrupts already off. */
void spin_lock(struct spinlock *lock);

/** Give `lock` back, leaving interrupts as they are. */
void spin_unlock(struct spinlock *lock);

/** Turn interrupts off, take `lock`, and return the flags to restore. */
uint32_t spin_lock_irqsave(struct spinlock *lock);

/** Give `lock` back and put the interrupt flag back as `flags` has it. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags);

#endif /* _SPINLOCK_H_ */
