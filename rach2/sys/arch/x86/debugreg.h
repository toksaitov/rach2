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

/** Set debug register `n` of `t` to `v` for PTRACE_POKEUSER: -EIO for DR4
 * and DR5, -EINVAL for a DR7 the processor must not be given, or an address
 * that an enabled breakpoint would then watch outside user space. */
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

/** Defer a kernel debug exception on this processor, without taking a lock. */
void debugreg_defer(void);

/** Defer a user hit if pc is the first instruction of system-call entry.
 * Return 1 for that entry, or 0 without changing the debug state otherwise.
 * Delivery forces its SIGTRAP once the syscall's user frame is complete. */
int debugreg_defer_user(uintptr_t pc);

/** Post this thread's deferred hit once, before a pending-signal check or
 * user return, on its stack with the big lock held and interrupts off. */
void debugreg_deliver(void);

/** Restore the current thread's debug enables before returning to user mode. */
void debugreg_resume(void);

#endif /* _X86_DEBUGREG_H_ */
