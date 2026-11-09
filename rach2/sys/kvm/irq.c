/*
 * irq.c: interrupt routing, irqfd and MSIs: a stub that offers none, until
 * the interrupt task replaces it.
 */

#include "kvm.h"

#include "../lib/kerrno.h"

int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/** KVM_CAP_SPLIT_IRQCHIP is not offered, and Linux answers -EINVAL to the
 * enable of a capability it does not offer. */
int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    (void)vm;
    (void)cap;
    return -EINVAL;
}

int32_t kvm_irq_check_extension(long cap)
{
    (void)cap;
    return 0;
}

void kvm_irq_free(struct kvm_vm *vm)
{
    (void)vm;
}
