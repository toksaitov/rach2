/*
 * kvmsteal.h: the time KVM's host kept a processor of this guest ready to
 * run but did not run it, which the tick charges as stolen (../arch.h,
 * `arch_steal_ns`), and KVM's hypercall that sends an interrupt to many
 * processors.
 */
#ifndef _KVMSTEAL_H_
#define _KVMSTEAL_H_

#include "../../lib/kint.h"

/**
 * Give KVM this processor's steal-time record, numbered `cpu`, where KVM
 * offers it (KVM_FEATURE_STEAL_TIME), as Linux's kvm_register_steal_time
 * does at each processor's start (arch/x86/kernel/kvm.c:348-359 in
 * 6.12.115), and its word for the paravirtual end of interrupt
 * (KVM_FEATURE_PV_EOI), which `lapic_eoi` then reads, as Linux's
 * kvm_guest_cpu_init (kvm.c:392-401). Nothing elsewhere. Runs on the
 * processor itself, with interrupts off, before its first tick.
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
