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
 * (`k_panic_begin`); write the record `panic: stopped: STR`, the report's
 * lines as `panic: report: LINE` and, with `panic_timeout` above 0,
 * `panic: rebooting in N seconds..`, all at `KLOG_CRIT` to every
 * destination but the screen; save the crash record (`k_panic_dump`); draw
 * the panic picture with a short report beside it, the reason, the
 * exception that `k_panic_note_fault` named, the processor and process, the
 * build and what becomes of the machine; and then act on `panic_timeout`,
 * as Linux's `panic` does (kernel/panic.c): 0 halts this processor with
 * interrupts off; N above 0 counts N seconds down on the screen and resets
 * the machine (`power_emergency_restart`); below 0 resets at once. From
 * any context; it does not return.
 */
void k_panic(const char *str) K_NORETURN;

/* `panic_timeout` with no `panic=` on the boot line: the seconds a panic
 * shows its report before the reset, which is the owner's choice over
 * Linux's 0, so that a machine nobody watches comes back by itself. */
#define PANIC_TIMEOUT_DEFAULT 30

/**
 * What a panic does once its record is out, as Linux's `panic_timeout`: 0
 * halts, N above 0 resets the machine after N seconds, below 0 resets at
 * once. `panic=N` on the boot line sets it (`k_panic_setup`), and
 * /proc/sys/kernel/panic reads and writes it; PANIC_TIMEOUT_DEFAULT when
 * neither does.
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

/** No error code, for `k_panic_note_fault`. */
#define PANIC_NO_ERROR (-1L)

/**
 * Name the exception a panic that follows is for, for its report: `name`,
 * a string that lives for good, such as "page fault"; `ip`, the
 * instruction it was raised at; and `error`, its error code, or
 * PANIC_NO_ERROR for an exception that has none. `k_panic_note_address`
 * adds the address it was about, CR2 for a page fault. From the handler,
 * with interrupts off, before `k_panic`.
 */
void k_panic_note_fault(const char *name, uintptr_t ip, long error);
void k_panic_note_address(uintptr_t address);

/** Whether this processor has begun a panic. */
int k_panicking(void);

/**
 * Called by `k_panic_begin` on the processor that panics, and by a stop on
 * each processor it halts (`smp_stop_nmi`, `smp_service`), with interrupts
 * off, before anything else, to leave the processor as a reset needs it:
 * vmx.c's turns VMX off, which blocks INIT while on, and svm.c's SVM. It
 * acts on the calling processor alone. NULL when none is set. It must not
 * fault.
 */
extern void (*k_panic_hook)(void);

/**
 * Called by `k_panic` once its record is out, with interrupts off, to save
 * the crash record for the next boot; it returns nonzero when the record is
 * saved, and the report says so. NULL when no store is set. It must not
 * fault, allocate or wait for an interrupt.
 */
extern int (*k_panic_dump)(void);

#endif
