/*
 * slots.c: a VM's memory slots: a stub with no slot, until the memory task
 * replaces it.
 */

#include "kvm.h"

#include "../lib/kerrno.h"

int kvm_slots_init(struct kvm_vm *vm)
{
    vm->slots = NULL;
    return 0;
}

void kvm_slots_free(struct kvm_vm *vm)
{
    (void)vm;
}

int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r)
{
    (void)vm;
    (void)r;
    return -ENOTTY;
}

int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    (void)vm;
    (void)gfn;
    (void)out;
    return 0;
}

int32_t kvm_slots_check_extension(long cap)
{
    (void)cap;
    return 0;
}
