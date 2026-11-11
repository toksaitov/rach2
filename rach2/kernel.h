/* What kernel.c gives the rest of the kernel beside the shared boot of
 * sys/boot.h: the end of a process by a signal, and the stop of the machine. */

#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "sys/boot.h"

#include "sys/lib/kint.h"
#include "sys/lib/kdef.h"

/* The system's init, or the debug shell's transient init; 0 before either. */
extern volatile uint32_t init_pid;
/* The debug shell supervises programs instead of the system's init. */
extern bool debug_shell_is_home;

/** Print a transient init's end from its encoded wait status `status`,
 * naming `pid`, for the debug shell and the probe runner. */
void init_end_line(uint32_t pid, int32_t status);

/**
 * End the current process by the signal `signo`, for a signal whose default
 * action ends it, a seccomp kill, or a signal frame that cannot be built or
 * read back, and run another process.
 *
 * With WCOREFLAG in `signo` a core file is written first, when the process is
 * dumpable, its soft RLIMIT_CORE allows a page, and no other thread has begun
 * to end it or an exec; the parent's waitpid then sees WCOREDUMP when the
 * whole file was written. The process's other threads end too, and the
 * parent sees a death by `signo` (WIFSIGNALED). The end of init halts the
 * machine (`panic_init_died`) unless the debug shell supervises, which then
 * runs again.
 *
 * May be called with interrupts on or off. Returns at once, doing nothing,
 * in a kernel context, which has no address space; otherwise does not return.
 */
void terminate_current_process(int32_t signo);

/**
 * Flush the root, stop the devices that write into memory, and halt, power
 * off or restart the machine as `how` asks: POWER_HALT, POWER_OFF or
 * POWER_RESTART (sys/power.h). Does not return; a failed flush does not keep
 * the machine from stopping, and leaves the dirty-mount flag set.
 */
void kernel_shutdown(int how);

#endif
