/*
 * The processor identification a driver asks for, on the host. The AHCI
 * driver reads cpuid leaf 1 for the cache line flush: whether `clflush`
 * exists (EDX bit 19) and its line size (EBX bits 8 to 15, in eight-byte
 * units). Without it the driver falls back to `wbinvd`, a privileged
 * instruction the host would fault on, so the answer here is a processor
 * with `clflush` and 64-byte lines; every other leaf reads as zero.
 */

#include "lib/kcpu.h"

void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t ebx = leaf == 1 ? 8u << 8 : 0;
    uint32_t edx = leaf == 1 ? 1u << 19 : 0;

    if (a) {
        *a = 0;
    }
    if (b) {
        *b = ebx;
    }
    if (c) {
        *c = 0;
    }
    if (d) {
        *d = edx;
    }
}
