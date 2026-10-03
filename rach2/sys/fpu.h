#ifndef _FPU_H_
#define _FPU_H_

#include "arch/arch.h"

#include "lib/kint.h"

/*
 * fpu: the floating-point register state of a process, saved at every switch.
 *
 * The state is kept eagerly: `schedule` saves the outgoing process's
 * registers into its record and loads the incoming one's before the stack
 * pointer changes, so two programs that both compute in floating point never
 * see each other's registers. The kernel itself computes in none, which is
 * what lets the save and the restore stand at the switch alone. A save area
 * is `FPU_STATE_SIZE` bytes, from the architecture's cpu.h; the
 * architecture defines the functions (arch/x86/fpu.c on a PC).
 */

/**
 * Choose the format, set the processor up for it, and record the initial
 * state. Runs once at boot, before the first process exists.
 */
void fpu_init(void);

/**
 * Save the live floating-point state into `area` without changing it.
 *
 * `area` is `FPU_STATE_SIZE` bytes. An area aligned as the architecture's
 * save instruction needs is written directly; any other goes through an
 * aligned copy under masked interrupts, which is what a frame on the user
 * stack needs.
 */
void fpu_save(void *area);

/** Load the live floating-point state from `area`, saved by `fpu_save`. */
void fpu_restore(const void *area);

/** Write the initial state into `area`, every register empty. */
void fpu_init_area(void *area);

/** Load the initial state into the registers, for a handler about to run. */
void fpu_reset(void);

#endif /* _FPU_H_ */
