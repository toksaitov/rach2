/*
 * vector.c: the pool of interrupt vectors given to devices for their
 * messages, which isr.c hands out with a chain of handlers each.
 */

#include "vector.h"
#include "../../lib/spinlock.h"

/* One byte for each vector, 1 while a device has it. */
static unsigned char taken[256];
static struct spinlock vector_lock = SPINLOCK_INIT_NAMED("vector_lock");

int vector_take(int align)
{
    int v;
    uint32_t flags = spin_lock_irqsave(&vector_lock);

    if (align < 1 || align > 32 || (align & (align - 1)) != 0) {
        align = 1;
    }
    for (v = ISR_MSI_FIRST; v <= ISR_MSI_LAST; v++) {
        if (v % align == 0 && v != ISR_SYSCALL_VECTOR && !taken[v]) {
            taken[v] = 1;
            spin_unlock_irqrestore(&vector_lock, flags);
            return v;
        }
    }
    spin_unlock_irqrestore(&vector_lock, flags);
    return -1;
}

int vector_give(int vector)
{
    uint32_t flags = spin_lock_irqsave(&vector_lock);
    if (vector < ISR_MSI_FIRST || vector > ISR_MSI_LAST || vector == ISR_SYSCALL_VECTOR ||
        !taken[vector]) {
        spin_unlock_irqrestore(&vector_lock, flags);
        return 0;
    }
    taken[vector] = 0;
    spin_unlock_irqrestore(&vector_lock, flags);
    return 1;
}

int vector_taken(int vector)
{
    uint32_t flags = spin_lock_irqsave(&vector_lock);
    int value = vector >= 0 && vector < 256 && taken[vector];

    spin_unlock_irqrestore(&vector_lock, flags);
    return value;
}

int vector_free_count(void)
{
    int v, n = 0;
    uint32_t flags = spin_lock_irqsave(&vector_lock);

    for (v = ISR_MSI_FIRST; v <= ISR_MSI_LAST; v++) {
        if (v != ISR_SYSCALL_VECTOR && !taken[v]) {
            n++;
        }
    }
    spin_unlock_irqrestore(&vector_lock, flags);
    return n;
}
