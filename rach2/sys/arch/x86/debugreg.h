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

/** At a debug exception: move DR6 into the current thread's copy and clear
 * it. Returns the trap's si_code, TRAP_HWBKPT for a breakpoint and
 * TRAP_TRACE for a single step, or 0 when DR6 shows neither. */
int32_t debugreg_status(void);

#endif /* _X86_DEBUGREG_H_ */
