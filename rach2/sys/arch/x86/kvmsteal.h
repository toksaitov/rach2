/* KVM's per-CPU shared records for steal time and interrupt completion. */
#ifndef _KVMSTEAL_H_
#define _KVMSTEAL_H_

#include "../../lib/kint.h"

/**
 * Give KVM processor `cpu`'s EOI word and steal-time record where KVM
 * offers each feature (KVM_FEATURE_PV_EOI and KVM_FEATURE_STEAL_TIME), as
 * Linux's kvm_guest_cpu_init does at each processor's start
 * (arch/x86/kernel/kvm.c:330-360 in
 * 6.12.115). Nothing elsewhere. Runs on the processor itself, with
 * interrupts off, before its first tick.
 */
void kvm_steal_cpu_setup(int cpu);

/**
 * Send the fixed interrupt `vector` to each processor of the set `cpus`
 * through KVM's KVM_HC_SEND_IPI, where KVM offers it
 * (KVM_FEATURE_PV_SEND_IPI), as Linux's kvm_send_ipi_mask: one hypercall,
 * one exit, for each span of 128 APIC ids on x86-64 or 64 on i686, where
 * the APIC takes one write of the command register for each processor in
 * xAPIC mode and for each cluster in x2APIC mode. 1 when sent; 0 when the
 * hypercall is not offered or KVM refused it, and the caller sends the
 * set itself. Any context.
 */
int kvm_send_ipi_many(uint64_t cpus, uint8_t vector);

#endif
