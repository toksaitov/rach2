#ifndef _KCPU_H_
#define _KCPU_H_

#include "../../lib/kint.h"

/*
 * kcpu: `cpuid` and the model-specific register instructions, `rdmsr` and
 * `wrmsr`, in one place. The local APIC is enabled through one of those
 * registers.
 *
 * `kcpu_msr_present` is asked before `kcpu_rdmsr`: a processor without the
 * instruction faults on it, and this kernel has no handler that would make
 * that survivable.
 */

/** The four registers `cpuid` leaf `leaf`, subleaf zero, returns, each
 * stored through its pointer unless the pointer is NULL. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

/** The four registers `cpuid` leaf `leaf` returns for subleaf `sub`, which
 * the leaves that describe caches and extended features take in ECX; a NULL
 * pointer is skipped, as in `kcpu_id`. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

/** Whether `cpuid` reports the model-specific registers (leaf 1, EDX bit 5). */
int kcpu_msr_present(void);

/** Whether `cpuid` reports a local APIC (leaf 1, EDX bit 9). */
int kcpu_apic_present(void);

/** Whether `cpuid` reports x2APIC mode (leaf 1, ECX bit 21). */
int kcpu_x2apic_present(void);

/** The running processor's x2APIC identity, which xAPIC mode shows only
 * the low eight bits of: leaf 0xB's EDX where the leaf describes a level,
 * else the initial APIC id of leaf 1's EBX bits 31:24. */
uint32_t kcpu_x2apic_id(void);

/** Whether the processor has x2APIC mode and runs under a hypervisor that
 * delivers interrupts to it without interrupt remapping: KVM, Hyper-V or
 * Xen, as Linux's `x86_init.hyper.x2apic_available` answers. */
int kcpu_x2apic_unremapped(void);

/** KVM's paravirtual features, leaf 0x40000001's EAX at the base where KVM's
 * signature is, or 0 when the kernel runs under no KVM: Linux's
 * kvm_arch_para_features (arch/x86/include/asm/kvm_para.h). */
uint32_t kcpu_kvm_features(void);

/** KVM's hints, the same leaf's EDX, as Linux's kvm_arch_para_hints
 * (arch/x86/kernel/kvm.c:934-937); 0 without KVM. */
uint32_t kcpu_kvm_hints(void);

/** Whether the processor is AMD's or Hygon's, whose instruction for a call
 * to the hypervisor is VMMCALL where Intel's is VMCALL, as Linux sets
 * X86_FEATURE_VMMCALL for those two vendors alone. */
int kcpu_vmmcall(void);

/**
 * Call KVM's hypercall `nr` with the arguments `a0` to `a3`, in RBX, RCX,
 * RDX and RSI, by VMMCALL when `vmmcall` is set and VMCALL otherwise, as
 * Linux's kvm_hypercall4 (arch/x86/include/asm/kvm_para.h). Returns RAX:
 * the call's answer, or a negated KVM error. Only under KVM, which takes
 * the instruction as an exit; elsewhere it raises #UD.
 */
long kcpu_hypercall4(int vmmcall, unsigned long nr, unsigned long a0, unsigned long a1,
                     unsigned long a2, unsigned long a3);

/** Whether the firmware locked the APIC in x2APIC mode, so that it cannot
 * be returned to xAPIC mode. */
int kcpu_x2apic_locked(void);

/** The model-specific register `msr`, its halves in `lo` and `hi`, each
 * stored unless its pointer is NULL. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi);

/** Write `hi` and `lo` to the model-specific register `msr`. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi);

/** Read an optional MSR into value. Return 0, or -1 on #GP, leaving value
 * unchanged. The fault fixup is local to the instruction. */
int kcpu_rdmsr_safe(uint32_t msr, uint64_t *value);

/** Write an optional MSR. Return 0, or -1 on #GP. */
int kcpu_wrmsr_safe(uint32_t msr, uint64_t value);

/**
 * RDMSR, or WRMSR when `write` is not 0, with EAX, ECX, EDX, EBX, EBP, ESI
 * and EDI taken from regs[0] to regs[7], regs[4] standing for ESP and left
 * alone, and given back there, as Linux's rdmsr_safe_regs and
 * wrmsr_safe_regs, for the msr device's X86_IOC_RDMSR_REGS and
 * X86_IOC_WRMSR_REGS. Return 0, or -1 on #GP, with regs as they were.
 */
int kcpu_msr_safe_regs(int write, uint32_t regs[8]);

/** Whether XSAVES may be used (xsaves.c): leaf 0xD subleaf 1 reports it,
 * and the part is not an AMD Zen 1 or Zen 2, family 0x17, whose XSAVES
 * malfunctions by Erratum 1386, unless its model, stepping and microcode
 * revision are the fixed ones Linux 6.12's `fix_erratum_1386` names. */
int kcpu_xsaves_usable(void);

#endif /* _KCPU_H_ */
