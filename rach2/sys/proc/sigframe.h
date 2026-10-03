#ifndef _SIGFRAME_H_
#define _SIGFRAME_H_

#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../irq.h"

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
 * frame or the frame would run off the bottom of the alternate stack it goes
 * on, which Linux refuses too, with the process's state unchanged.
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

/* The bytes of a `siginfo_t`, on every architecture. */
#define SIGINFO_SIZE 128

/** Write the `siginfo_t` of `signo` with `info` into `out`: signo, errno and
 * code, then the fault's address, or the sender's pid and uid and a child's
 * status. */
void sigframe_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info);

/** Read the code, the sender's pid and uid, the status and the address of
 * the `siginfo_t` at `in` into `info`, for PTRACE_SETSIGINFO. */
void sigframe_take_siginfo(const uint8_t *in, struct k_siginfo *info);

/**
 * Whether the user stack pointer `sp` lies on the current process's
 * alternate signal stack.
 */
int sigframe_on_altstack(uint32_t sp);

#endif /* _SIGFRAME_H_ */
