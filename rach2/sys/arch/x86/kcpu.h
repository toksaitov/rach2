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

/** The model-specific register `msr`, its halves in `lo` and `hi`, each
 * stored unless its pointer is NULL. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi);

/** Write `hi` and `lo` to the model-specific register `msr`. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi);

#if defined(__x86_64__) || defined(RACH2_HOST_X86_64)
/** Read an optional MSR into value on x86-64. Return 0, or -1 on #GP,
 * leaving value unchanged. The fault fixup is local to the instruction. */
int kcpu_rdmsr_safe(uint32_t msr, uint64_t *value);

/** Write an optional MSR on x86-64. Return 0, or -1 on #GP. */
int kcpu_wrmsr_safe(uint32_t msr, uint64_t value);
#endif

/** Whether XSAVES may be used (xsaves.c): leaf 0xD subleaf 1 reports it,
 * and the part is not an AMD Zen 1 or Zen 2, family 0x17, whose XSAVES
 * malfunctions by Erratum 1386, unless its model, stepping and microcode
 * revision are the fixed ones Linux 6.12's `fix_erratum_1386` names. */
int kcpu_xsaves_usable(void);

#endif /* _KCPU_H_ */
