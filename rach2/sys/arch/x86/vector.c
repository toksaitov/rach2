/*
 * vector.c: the pool of interrupt vectors given to devices for their
 * messages, which isr.c hands out with a chain of handlers each.
 */

#include "vector.h"

/* One byte for each vector, 1 while a device has it. */
static unsigned char taken[256];

int vector_take(int align)
{
    int v;

    if (align < 1 || align > 32 || (align & (align - 1)) != 0) {
        align = 1;
    }
    for (v = ISR_MSI_FIRST; v <= ISR_MSI_LAST; v++) {
        if (v % align == 0 && v != ISR_SYSCALL_VECTOR && !taken[v]) {
            taken[v] = 1;
            return v;
        }
    }
    return -1;
}

int vector_give(int vector)
{
    if (vector < ISR_MSI_FIRST || vector > ISR_MSI_LAST || vector == ISR_SYSCALL_VECTOR ||
        !taken[vector]) {
        return 0;
    }
    taken[vector] = 0;
    return 1;
}

int vector_taken(int vector)
{
    return vector >= 0 && vector < 256 && taken[vector];
}

int vector_free_count(void)
{
    int v, n = 0;

    for (v = ISR_MSI_FIRST; v <= ISR_MSI_LAST; v++) {
        if (v != ISR_SYSCALL_VECTOR && !taken[v]) {
            n++;
        }
    }
    return n;
}
