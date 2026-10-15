/*
 * panic.h: the kernel's stop on an error it cannot recover from.
 */
#ifndef _PANIC_H_
#define _PANIC_H_

#include "display/textcon.h"
#include "kio.h"
#include "lib/kdef.h"

/**
 * Report `str` as the reason the kernel stops, and stop it: begin the panic
 * (`k_panic_begin`), draw the panic picture over the screen, write the record
 * `panic: stopped: STR` at `KLOG_CRIT` to every destination at once, and halt
 * this processor with interrupts off. From any context; it does not return.
 */
void k_panic(const char *str) K_NORETURN;

/**
 * Begin a panic on this processor before its record is written: interrupts
 * off, the other processors asked to stop, and the console this processor's
 * without its lock. A second processor to begin one stops here. `k_panic`
 * begins its own; a handler that writes a record first, without the big
 * lock, begins it before.
 */
void k_panic_begin(void);

/** Whether this processor has begun a panic. */
int k_panicking(void);

#endif
