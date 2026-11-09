/*
 * decode.c: the decoder of the instructions that reach MMIO: a stub that
 * knows none, until the decoder's task replaces it.
 */

#include "kvm.h"

int kvm_decode(const uint8_t *bytes, uint32_t n, int mode, struct kvm_insn *out)
{
    (void)bytes;
    (void)n;
    (void)mode;
    (void)out;
    return -1;
}
