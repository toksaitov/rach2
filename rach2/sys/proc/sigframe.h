#ifndef _SIGFRAME_H_
#define _SIGFRAME_H_

#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../isr.h"

/*
 * The Linux i386 signal frames: `struct sigframe` for a plain handler and
 * `struct rt_sigframe` for an SA_SIGINFO one, each with a `sigcontext` the
 * return restores and the floating-point state as `struct _fpstate_32`. See
 * signal.Readme.md.
 */

/**
 * Build the frame for a handler of `signo` on the user stack of the current
 * process and point the trapframe `tf` at the handler.
 *
 * `sa` is the handler's action: SA_SIGINFO chooses the rt frame, SA_ONSTACK
 * the alternate stack when one is set and not in use, and SA_RESTORER the
 * address the handler returns to, the frame's own `retcode` without it.
 * `info` fills the rt frame's siginfo, and `oldmask` is the mask the return
 * restores. The handler starts with the initial floating-point state, its
 * signal number in EAX and, for an rt frame, the siginfo and ucontext
 * addresses in EDX and ECX. Returns 0, or -1 when the stack cannot take the
 * frame, with the process's state unchanged.
 */
int sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                   const struct k_siginfo *info, uint64_t oldmask);

/**
 * Restore the state a frame saved, for rt_sigreturn with `rt` and sigreturn
 * without: the general registers, the flags a program may change, the segment
 * selectors this kernel has, and the floating-point state; the rt frame's
 * alternate stack as well. `*mask` receives the mask to restore. Returns 0,
 * or -1 for a frame that cannot be read or would load a faulting state.
 */
int sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask);

/**
 * Whether the user stack pointer `sp` lies on the current process's
 * alternate signal stack.
 */
int sigframe_on_altstack(uint32_t sp);

#endif /* _SIGFRAME_H_ */
