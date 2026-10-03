#ifndef _FPU_H_
#define _FPU_H_

#include "arch/arch.h"

#include "lib/kdef.h"
#include "lib/kint.h"

/*
 * fpu: the floating-point register state of a process, saved at every switch.
 *
 * The state is kept eagerly: `schedule` saves the outgoing process's
 * registers into its record and loads the incoming one's before the stack
 * pointer changes, so two programs that both compute in floating point never
 * see each other's registers. The kernel itself computes in none, which is
 * what lets the save and the restore stand at the switch alone. A save area
 * is `fpu_area_size` bytes, which the processor decides at boot, from the
 * heap; the architecture defines the functions (arch/x86/fpu.c on a PC).
 */

/**
 * Choose the format, set the processor up for it, and record the initial
 * state. Runs once at boot, before the first process exists and before the
 * heap.
 */
void fpu_init(void);

/** The bytes of a save area, fixed by `fpu_init`. */
size_t fpu_area_size(void);

/**
 * A save area from the heap, aligned as the architecture's save instruction
 * needs and holding the initial state, or NULL when the heap has no room.
 * Freed with `fpu_area_free`.
 */
void *fpu_area_alloc(void);

/** Give back what `fpu_area_alloc` returned; NULL is accepted and ignored. */
void fpu_area_free(void *area);

/** Save the live floating-point state into `area`, which `fpu_area_alloc`
 * gave, without changing it. */
void fpu_save(void *area);

/** Load the live floating-point state from `area`, saved by `fpu_save`. */
void fpu_restore(const void *area);

/** Write the initial state into `area`, every register empty. */
void fpu_init_area(void *area);

/** Load the initial state into the registers, for a handler about to run. */
void fpu_reset(void);

#endif /* _FPU_H_ */
