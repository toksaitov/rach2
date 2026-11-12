/*
 * The kernel's spinlocks for a host test of one processor (lib/spinlock.h):
 * a lock is a word the holder sets, and the interrupt flag is whatever the
 * test's own `kirq_save` and `kirq_restore` model. A lock taken while it is
 * held can only be the same code path taking it twice, which on the machine
 * spins for ever, so it stops the test at once.
 */
#include "../../../rach2/sys/lib/spinlock.h"
#include "../../../rach2/sys/lib/kirq.h"

/** Take `lock`; trap if it is already held. */
void spin_lock(struct spinlock *lock)
{
    if (lock->locked) {
        __builtin_trap();
    }
    lock->locked = 1;
}

/** Take `lock` as one of a nest of its class, which one processor takes as
 * it takes any lock. */
void spin_lock_nested(struct spinlock *lock, unsigned int subclass)
{
    (void)subclass;
    spin_lock(lock);
}

/** Take `lock` when it is free: 1, or 0 when it is held. */
int spin_trylock(struct spinlock *lock)
{
    if (lock->locked) {
        return 0;
    }
    lock->locked = 1;
    return 1;
}

/** Release `lock`. */
void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
}

/** Save the interrupt flag with `kirq_save`, take `lock`, and return the
 * saved flag for `spin_unlock_irqrestore`. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    spin_lock(lock);
    return flags;
}

/** Release `lock` and restore the interrupt flag `flags` saved with it. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    spin_unlock(lock);
    kirq_restore(flags);
}
