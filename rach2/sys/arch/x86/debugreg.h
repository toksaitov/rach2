#ifndef _X86_DEBUGREG_H_
#define _X86_DEBUGREG_H_

#include "../../lib/kint.h"
#include "../../proc/process.h"

/*
 * The x86 debug registers as a tracer sees them: DR0 to DR3 hold the four
 * breakpoints' addresses, DR6 the status of the last debug exception and DR7
 * which breakpoints are enabled, on what access and for how many bytes. A
 * thread keeps its own copy in `debugreg`, which the scheduler loads when it
 * runs (`arch_debug_switch`).
 */

/* DR7's bits the processor keeps set, which a read shows and a write may
 * leave out. */
#define DR7_ONE 0x00000400u

/** Debug register `n` of `t` as PTRACE_PEEKUSER reads it. */
unsigned long debugreg_peek(const process_t *t, unsigned n);

/** Set debug register `n` of `t`, a stopped thread, to `v` for
 * PTRACE_POKEUSER: 0, -EIO for DR4, DR5 and a number above 7, or -EINVAL,
 * with the register unchanged, for a DR7 the processor must not be given or
 * an address that an enabled breakpoint would then watch outside user
 * space. */
int debugreg_poke(process_t *t, unsigned n, unsigned long v);

/* What `debugreg_status` returns for a breakpoint the current thread no
 * longer has, which the processor's DR7 still had because a tracer's end
 * cleared the thread's copy while it ran: no signal is sent. */
#define DEBUGREG_STALE (-1)

/** At a debug exception: move DR6 into the current thread's copy and clear
 * it. Returns the trap's si_code, TRAP_HWBKPT for a breakpoint and
 * TRAP_TRACE for a single step, 0 when DR6 shows neither, or DEBUGREG_STALE,
 * with the processor's DR7 cleared, for a breakpoint of a DR7 the thread no
 * longer has. */
int32_t debugreg_status(void);

/**
 * Defer a debug exception taken in the kernel on this processor, without a
 * lock or a thread's stack: record DR6's breakpoint and single-step bits
 * for `debugreg_deliver`, clear DR6, and clear DR7 until the next switch or
 * return to user mode, so that a second exception cannot nest on the
 * interrupt stack. x86-64's debug entry calls it on that stack.
 */
void debugreg_defer(void);

/**
 * Defer a debug exception taken in the kernel at `pc` when `pc` is the
 * system call entry's first instruction (`isr128` on i686, `syscall_entry`
 * on x86-64), where the processor can report a user data breakpoint whose
 * exception it delayed past the system call instruction: record it as
 * `debugreg_defer` does and, while the thread has breakpoints enabled, mark
 * it a user hit, whose SIGTRAP `debugreg_deliver` forces at the return to
 * user mode even when the thread blocks SIGTRAP. 1 for that entry; 0, with
 * the debug state unchanged, for any other `pc`.
 */
int debugreg_defer_user(uintptr_t pc);

/**
 * Post the current thread's deferred debug exceptions, this processor's and
 * those a switch saved in the thread, and clear both records: a user hit as
 * a forced SIGTRAP, and a kernel hit of a breakpoint the thread still has
 * enabled as a SIGTRAP its signal mask governs; a single step or a stale
 * hit posts nothing. `isr_leave` calls it at the return to user mode, and
 * `arch_debug_deliver` before a signal check, each on the thread's stack
 * with the big lock held and interrupts off.
 */
void debugreg_deliver(void);

/** Make this processor's DR7 match the current thread's enables before the
 * return to user mode: reloaded when a deferred exception cleared it,
 * cleared when the thread has none. `isr_leave` calls it with interrupts
 * off. */
void debugreg_resume(void);

#endif /* _X86_DEBUGREG_H_ */
