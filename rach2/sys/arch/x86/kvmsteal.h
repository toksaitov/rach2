/*
 * kvmsteal.h: the time KVM's host kept a processor of this guest ready to
 * run but did not run it, which the tick charges as stolen (../arch.h,
 * `arch_steal_ns`).
 */
#ifndef _KVMSTEAL_H_
#define _KVMSTEAL_H_

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

#endif
