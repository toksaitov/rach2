#include "kirq.h"

/** Return the flags register as it stands, then disable interrupts. */
uint32_t kirq_save(void)
{
    uint32_t flags;

    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/**
 * Load the flags register from `flags`, a value `kirq_save` returned, which
 * enables interrupts again only if they were on at that call.
 */
void kirq_restore(uint32_t flags)
{
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

int kirq_enabled(void)
{
    uint32_t flags;

    __asm__ __volatile__("pushfl; popl %0" : "=r"(flags) : : "memory");
    return (flags & 0x200u) != 0; /* EFLAGS.IF */
}
