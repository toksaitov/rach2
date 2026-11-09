/*
 * iobus.c: the in-kernel bus and ioeventfd: a stub on which every access
 * goes to the caller, until the interrupt task replaces it.
 */

#include "kvm.h"

#include "../lib/kerrno.h"

int32_t kvm_iobus_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

int kvm_iobus_write(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

int32_t kvm_iobus_check_extension(long cap)
{
    (void)cap;
    return 0;
}

void kvm_iobus_free(struct kvm_vm *vm)
{
    (void)vm;
}
