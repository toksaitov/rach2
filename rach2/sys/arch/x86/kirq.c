#include "../../lib/kirq.h"

/*
 * On x86-64 the flags register is RFLAGS, pushed and popped whole; its
 * upper half is reserved and zero, so the 32 bits kept are all of it.
 */

/** Return the flags register as it stands, then disable interrupts. */
uint32_t kirq_save(void)
{
#if defined(__x86_64__)
    unsigned long flags;

    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return (uint32_t)flags;
#else
    uint32_t flags;

    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}

/**
 * Load the flags register from `flags`, a value `kirq_save` returned, which
 * enables interrupts again only if they were on at that call.
 */
void kirq_restore(uint32_t flags)
{
#if defined(__x86_64__)
    unsigned long wide = flags;

    __asm__ __volatile__("pushq %0; popfq" : : "r"(wide) : "memory", "cc");
#else
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
#endif
}

int kirq_enabled(void)
{
#if defined(__x86_64__)
    unsigned long flags;

    __asm__ __volatile__("pushfq; popq %0" : "=r"(flags) : : "memory");
#else
    uint32_t flags;

    __asm__ __volatile__("pushfl; popl %0" : "=r"(flags) : : "memory");
#endif
    return (flags & 0x200u) != 0; /* EFLAGS.IF */
}
