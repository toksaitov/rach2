/*
 * tdp.c: the guest-physical tables: a stub in which no frame is mapped,
 * until the memory task replaces it.
 */

#include "kvm.h"

int kvm_tdp_init(struct kvm_vm *vm)
{
    (void)vm;
    return 0;
}

void kvm_tdp_free(struct kvm_vm *vm)
{
    (void)vm;
}

/** No slot holds a frame, so every fault is an access to MMIO: 1. */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write)
{
    (void)vm;
    (void)gfn;
    (void)write;
    return 1;
}
