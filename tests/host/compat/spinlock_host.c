/*
 * The kernel's spinlocks for a host test of one processor (lib/spinlock.h):
 * a lock is a word the holder sets, and the interrupt flag is whatever the
 * test's own `kirq_save` and `kirq_restore` model. A lock taken while it is
 * held can only be the same code path asking twice, which on the machine
 * spins for ever, so it stops the test at once.
 */
#include "../../../rach2/sys/lib/spinlock.h"
#include "../../../rach2/sys/lib/kirq.h"

void spin_lock(struct spinlock *lock)
{
    if (lock->locked) {
        __builtin_trap();
    }
    lock->locked = 1;
}

void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
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
