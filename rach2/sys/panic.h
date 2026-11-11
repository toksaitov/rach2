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
 * (`k_panic_begin`), draw the panic picture over the screen, show under it the
 * last `KLOG_CRIT` record (`klog_show_crit`), on the screen alone, write the
 * record `panic: stopped: STR` at `KLOG_CRIT` to every destination at once,
 * and then act on `panic_timeout`, as Linux's `panic` does (kernel/panic.c):
 * 0 halts this processor with interrupts off; N above 0 says
 * `panic: rebooting in N seconds..`, keeps the screen as it is for N seconds
 * and resets the machine (`power_emergency_restart`); below 0 resets at once.
 * From any context; it does not return.
 */
void k_panic(const char *str) K_NORETURN;

/**
 * What a panic does once its record is out, as Linux's `panic_timeout`: 0
 * halts, N above 0 resets the machine after N seconds, below 0 resets at
 * once. `panic=N` on the boot line sets it (`k_panic_setup`), and
 * /proc/sys/kernel/panic reads and writes it; 0 when neither does.
 */
extern int panic_timeout;

/**
 * Whether an oops panics, as Linux's `panic_on_oops`: `oops=panic` on the
 * boot line sets it to 1, and /proc/sys/kernel/panic_on_oops reads and writes
 * it. This kernel has no oops that it survives: an exception in kernel mode
 * panics whatever the value (arch/x86/isr.c), which is what Linux does with
 * the value set.
 */
extern int panic_on_oops;

/**
 * Read `panic=N` and `oops=panic` from the boot line, as Linux's
 * `core_param(panic, ...)` and `early_param("oops", ...)` read them: each
 * `panic=` in turn, so that the last one whose value is an integer wins, and
 * one that is not an integer is logged and changes nothing. The boot calls
 * it once the command line is captured.
 */
void k_panic_setup(void);

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

/**
 * Called by `k_panic_begin` on the processor that panics, with interrupts
 * off, before anything else, to leave the processor as a reset needs it:
 * vmx.c's turns VMX off, which blocks INIT while on. NULL when none is
 * set. It must not fault.
 */
extern void (*k_panic_hook)(void);

#endif
