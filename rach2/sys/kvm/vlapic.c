/*
 * vlapic.c: the local APIC in the kernel: a stub with no local APIC, until
 * the interrupt task replaces it.
 */

#include "kvm.h"

#include "../lib/kerrno.h"

int32_t kvm_lapic_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    (void)v;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

int kvm_lapic_halt(struct kvm_vcpu *v)
{
    (void)v;
    return 1;
}

void kvm_lapic_free(struct kvm_vcpu *v)
{
    (void)v;
}
