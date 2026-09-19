#ifndef _FPU_H_
#define _FPU_H_

/*
 * fpu: the x87 and SSE register state of a process, saved at every switch.
 *
 * The state is kept eagerly: `schedule` saves the outgoing process's
 * registers into its record and loads the incoming one's before the stack
 * pointer changes, so two programs that both compute in floating point never
 * see each other's registers. The kernel itself computes in none, which is
 * what lets the save and the restore stand at the switch alone.
 *
 * Two formats. When `cpuid` reports FXSR and `rach2.nofxsr` is not on the
 * boot line, `fxsave` and `fxrstor` keep the x87 and the XMM registers with
 * MXCSR in 512 bytes at a 16-byte alignment, and CR4.OSFXSR is set, when
 * `cpuid` reports SSE as well, so the SSE instructions run. Otherwise
 * `fnsave` and `frstor` keep the x87 state in the first 108 bytes,
 * CR4.OSFXSR is cleared, and an SSE instruction is an invalid opcode: a
 * register file that is not saved is not offered.
 */

/** The bytes a save area has, the `fxsave` image; `fnsave` uses 108 of it. */
#define FPU_STATE_SIZE 512

/**
 * Choose the format, set CR0 and CR4 for it, and record the initial state.
 *
 * Runs once at boot, before the first process exists. CR0 gets MP and NE
 * and loses EM and TS, so the unit is always present and never traps on
 * first use; the lazy switch through TS is not used here.
 */
void fpu_init(void);

/**
 * Save the live floating-point state into `area` without changing it.
 *
 * `area` is `FPU_STATE_SIZE` bytes. An area aligned to 16 bytes is written
 * directly; any other goes through an aligned copy under masked interrupts,
 * which is what a frame on the user stack needs.
 */
void fpu_save(void *area);

/** Load the live floating-point state from `area`, saved by `fpu_save`. */
void fpu_restore(const void *area);

/** Write the initial state into `area`: control word 0x37F, MXCSR 0x1F80,
 * every register empty. */
void fpu_init_area(void *area);

/** Load the initial state into the registers, for a handler about to run. */
void fpu_reset(void);

/**
 * Check a state that came from user memory before it is restored.
 *
 * Returns 0 when `area` may be loaded. In the FXSR format a set reserved bit
 * of MXCSR makes `fxrstor` fault, in the kernel, so such an area answers -1
 * and the caller refuses the frame. The x87 format cannot fault on a load.
 */
int fpu_check_area(const void *area);

#endif /* _FPU_H_ */
