/*
 * The architecture's atomic exchanges for a host test (arch/arch.h), over
 * the compiler's builtins, each a full barrier as the kernel's are.
 */

#include <stdint.h>

unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value);
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value);
uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value);
void arch_store_u8(volatile unsigned char *p, unsigned char value);
void arch_store_u16(volatile unsigned short *p, unsigned short value);

/** Store `value` at `*p` and return what was there. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
}

/** Store `value` at `*p` if `*p` is `old`, and return what was there. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    __atomic_compare_exchange_n(p, &old, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}

/** Store `value` at `*p` if `*p` is `old`, and return what was there, as
 * the kernel's exchange of a word as wide as a pointer does. */
uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value)
{
    __atomic_compare_exchange_n(p, &old, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}

/** Store `value` at the byte `*p`, a release. */
void arch_store_u8(volatile unsigned char *p, unsigned char value)
{
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
}

/** Store `value` at the half-word `*p`, a release. */
void arch_store_u16(volatile unsigned short *p, unsigned short value)
{
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
}
