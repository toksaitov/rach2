/*
 * kcpu.c: `cpuid` and the model-specific register instructions, as kcpu.h
 * describes them.
 */

#include "kcpu.h"

/**
 * The four registers `cpuid` leaf `leaf`, subleaf zero, returns, each stored
 * through its pointer unless the pointer is null.
 */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    kcpu_id_sub(leaf, 0, a, b, c, d);
}

void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c,
                 uint32_t *d)
{
    uint32_t ra = leaf, rb = 0, rc = sub, rd = 0;

    __asm__ __volatile__("cpuid" : "+a"(ra), "=b"(rb), "+c"(rc), "=d"(rd) : : "memory");
    if (a) {
        *a = ra;
    }
    if (b) {
        *b = rb;
    }
    if (c) {
        *c = rc;
    }
    if (d) {
        *d = rd;
    }
}

/** Whether `cpuid` reports the model-specific registers (leaf 1, EDX bit 5). */
int kcpu_msr_present(void)
{
    uint32_t a = 0, d = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, 0, 0, &d);
    return (d & (1u << 5)) != 0;
}

/** Whether `cpuid` reports a local APIC (leaf 1, EDX bit 9). */
int kcpu_apic_present(void)
{
    uint32_t a = 0, d = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, 0, 0, &d);
    return (d & (1u << 9)) != 0;
}

/** The model-specific register `msr`, its halves in `lo` and `hi`. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    uint32_t l = 0, h = 0;

    __asm__ __volatile__("rdmsr" : "=a"(l), "=d"(h) : "c"(msr));
    if (lo) {
        *lo = l;
    }
    if (hi) {
        *hi = h;
    }
}

/** Write the model-specific register `msr`. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
