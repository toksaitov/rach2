/*
 * dirty.c: dirty logging: a stub with no log, until the memory task
 * replaces it.
 */

#include "kvm.h"

#include "../lib/kerrno.h"

int32_t kvm_dirty_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}
