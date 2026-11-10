/*
 * fpu.h: the floating-point register state of a process, saved and loaded
 * at every switch.
 */
#ifndef _FPU_H_
#define _FPU_H_

#include "arch/arch.h"

#include "lib/kdef.h"
#include "lib/kint.h"

/*
 * The state is kept eagerly: `schedule` saves the outgoing process's
 * registers into its record and loads the incoming one's before the stack
 * pointer changes, so two programs that both compute in floating point never
 * see each other's registers. The kernel itself computes in none, so its own
 * code leaves the registers as the process left them. A save area is
 * `fpu_area_size` bytes, which `fpu_init` fixes at boot from what the
 * processor has, from the heap; the architecture defines the functions
 * (arch/x86/fpu.c on a PC).
 */

/**
 * Choose the format, set the processor up for it, and record the initial
 * state. On a PC the format is the x87's own, `fxsave`'s, or XSAVE's with
 * the components the processor has, the newest it has unless `rach2.noxsave`
 * or, on i686, `rach2.nofxsr` is on the boot line. Runs once at boot, before
 * the first process exists and before the heap.
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

/*
 * Memory protection keys, which the x86-64 kernel turns on where the
 * processor has them (PKU): each user page carries a key from 0 to 15 in
 * its page table entry, and each thread has a register, PKRU, with two
 * bits for each key, the low one denying every data access to the pages of
 * the key and the high one denying writes to them, the kernel's on the
 * thread's behalf too. Instruction fetches are never refused by a key.
 * PKRU is a component of the extended state, so it follows the thread
 * through every switch, a fork and a signal as the rest of the state does,
 * and an exec or a signal handler starts with PKRU_INIT. Elsewhere, on the
 * i686 kernel and on a processor without the keys, no key is ever given
 * and PKRU reads as 0.
 */
#define FPU_PKEYS  16
/* PKRU's access-disable and write-disable bits of the key `k`. */
#define PKRU_AD(k) (1u << (2u * (uint32_t)(k)))
#define PKRU_WD(k) (2u << (2u * (uint32_t)(k)))
/* Linux's init_pkru_value: key 0 open, every other key denied all access. */
#define PKRU_INIT  0x55555554u

/** Nonzero when the processor's protection keys are on. */
int fpu_pkeys(void);

/** The running thread's live PKRU, or 0 with the keys off. */
uint32_t fpu_pkru(void);

/** Write the running thread's live PKRU; nothing with the keys off. */
void fpu_pkru_set(uint32_t pkru);

#endif /* _FPU_H_ */
