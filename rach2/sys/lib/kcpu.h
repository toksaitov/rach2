#ifndef _KCPU_H_
#define _KCPU_H_

#include "kint.h"

/*
 * kcpu: `cpuid` and the model-specific register instructions, `rdmsr` and
 * `wrmsr`, in one place. The local APIC is enabled through one of those
 * registers.
 *
 * `kcpu_msr_present` is asked before `kcpu_rdmsr`: a processor without the
 * instruction faults on it, and this kernel has no handler that would make
 * that survivable.
 */

/** The four registers `cpuid` leaf `leaf`, subleaf zero, returns. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

/** The four registers `cpuid` leaf `leaf` returns for subleaf `sub`, which
 * the leaves that describe caches and extended features take in ECX. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

/** Whether `cpuid` reports the model-specific registers (leaf 1, EDX bit 5). */
int kcpu_msr_present(void);

/** Whether `cpuid` reports a local APIC (leaf 1, EDX bit 9). */
int kcpu_apic_present(void);

/** The model-specific register `msr`, its halves in `lo` and `hi`. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi);

/** Write the model-specific register `msr`. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi);

#endif /* _KCPU_H_ */
