/* The spinlock's acquire and release, over the architecture's exchange. */
#include "spinlock.h"

#include "kirq.h"
#include "../arch/arch.h"
#include "../proc/smp.h"

/* The instruction that called the function this is used in, for the
 * checker's reports. */
#define SPIN_CALLER ((uintptr_t)(const char *)__builtin_return_address(0))

/**
 * The exchange is tried first, and then the word is only read until it
 * reads free, so that a waiting processor does not take the bus on every
 * turn. Interrupts are off, so a waiter answers the requests left in its
 * record itself (`smp_service`): a holder that is dropping a translation on
 * every processor waits for this one's answer, and would wait for ever.
 */
void raw_spin_lock(struct spinlock *lock)
{
    while (arch_atomic_xchg(&lock->locked, 1u) != 0) {
        while (lock->locked) {
            smp_service();
            arch_relax();
        }
    }
}

void raw_spin_unlock(struct spinlock *lock)
{
    (void)arch_atomic_xchg(&lock->locked, 0u);
}

void spin_lock(struct spinlock *lock)
{
    lockdep_acquire(&lock->dep, 0, LOCKDEP_SPIN, SPIN_CALLER);
    raw_spin_lock(lock);
}

void spin_lock_nested(struct spinlock *lock, unsigned int subclass)
{
    lockdep_acquire(&lock->dep, subclass, LOCKDEP_SPIN, SPIN_CALLER);
    (void)subclass;
    raw_spin_lock(lock);
}

int spin_trylock(struct spinlock *lock)
{
    if (arch_atomic_xchg(&lock->locked, 1u) != 0) {
        return 0;
    }
    lockdep_acquire(&lock->dep, 0, LOCKDEP_TRY, SPIN_CALLER);
    return 1;
}

void spin_unlock(struct spinlock *lock)
{
    lockdep_release(&lock->dep, SPIN_CALLER);
    raw_spin_unlock(lock);
}

uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    lockdep_acquire(&lock->dep, 0, LOCKDEP_SPIN, SPIN_CALLER);
    raw_spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    lockdep_release(&lock->dep, SPIN_CALLER);
    raw_spin_unlock(lock);
    kirq_restore(flags);
}
