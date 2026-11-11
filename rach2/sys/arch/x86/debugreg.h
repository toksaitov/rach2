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
 * Drop a debug exception taken in the kernel on this processor, as Linux
 * does, whose ptrace breakpoints exclude the kernel: clear DR6, and clear
 * DR7 until the next switch or return to user mode, so that the rest of a
 * copy through a watched range takes no further exception. Nothing is
 * posted, and the thread's saved DR6 is unchanged. It takes no lock and
 * needs no thread's stack, so x86-64's debug entry calls it on its
 * interrupt stack.
 */
void debugreg_drop(void);

/**
 * Take a debug exception in the kernel at `pc` inside x86-64's `sysenter`
 * entry, from its first instruction to `sysenter_stepped`, where the
 * program's TF steps the processor until the entry clears it: drop it as
 * `debugreg_drop` does, for the caller to return with TF kept. 1 for such a
 * `pc`; 0, with the debug state unchanged, for any other, and always on
 * i686.
 */
int debugreg_drop_sysenter(uintptr_t pc);

/** Make this processor's DR7 match the current thread's enables before the
 * return to user mode: reloaded when a dropped exception cleared it,
 * cleared when the thread has none. `isr_leave` calls it with interrupts
 * off. */
void debugreg_resume(void);

/** Whether this processor's DR7 has a breakpoint enabled. 0 means it holds
 * DR7_ONE, which a hypervisor may take without reading the register, since
 * a read can trap to the hypervisor beneath. Call with interrupts off. */
int debugreg_loaded(void);

#endif /* _X86_DEBUGREG_H_ */
