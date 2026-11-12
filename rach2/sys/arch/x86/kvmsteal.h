/* KVM's per-CPU shared records for steal time and interrupt completion. */
#ifndef _KVMSTEAL_H_
#define _KVMSTEAL_H_

#include "../../lib/kint.h"

/**
 * Give KVM processor `cpu`'s EOI word and steal-time record where KVM
 * offers each feature (KVM_FEATURE_PV_EOI and KVM_FEATURE_STEAL_TIME), as
 * Linux's kvm_register_steal_time
 * does at each processor's start (arch/x86/kernel/kvm.c:348-359 in
 * 6.12.115), and its word for the paravirtual end of interrupt
 * (KVM_FEATURE_PV_EOI), which `lapic_eoi` then reads, as Linux's
 * kvm_guest_cpu_init (kvm.c:392-401). Nothing elsewhere. Runs on the
 * processor itself, with interrupts off, before its first tick.
 */
void kvm_steal_cpu_setup(int cpu);

/** Complete this processor's virtual interrupt through KVM's shared word.
 * Return one when handled, zero when the APIC EOI must be written.
 * Interrupts are off; zero before setup or without KVM_FEATURE_PV_EOI. */
int kvm_guest_eoi(void);

#endif
