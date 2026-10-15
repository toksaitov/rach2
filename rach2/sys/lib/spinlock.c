/* The spinlock's acquire and release, over the architecture's exchange. */
#include "spinlock.h"

#include "kirq.h"
#include "../arch/arch.h"
#include "../proc/smp.h"

/**
 * The exchange is tried first, and then the word is only read until it
 * reads free, so that a waiting processor does not take the bus on every
 * turn. Interrupts are off, so a waiter answers the requests left in its
 * record itself (`smp_service`): a holder that is dropping a translation on
 * every processor waits for this one's answer, and would wait for ever.
 */
void spin_lock(struct spinlock *lock)
{
    while (arch_atomic_xchg(&lock->locked, 1u) != 0) {
        while (lock->locked) {
            smp_service();
            arch_relax();
        }
    }
}

void spin_unlock(struct spinlock *lock)
{
    (void)arch_atomic_xchg(&lock->locked, 0u);
}

uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    spin_unlock(lock);
    kirq_restore(flags);
}
