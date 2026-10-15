/*
 * The signal frames, Linux's for each architecture, which the sigframe.c of
 * each builds and restores: on i686 `struct sigframe` for a plain handler
 * and `struct rt_sigframe` for an SA_SIGINFO one, and on x86-64 the rt frame
 * for every handler, each with a `sigcontext` the return restores and the
 * floating-point state. See signal.Readme.md.
 */
#ifndef _SIGFRAME_H_
#define _SIGFRAME_H_

#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../irq.h"

/**
 * Build the frame for a handler of `signo` on the user stack of the current
 * process and point the trapframe `tf` at the handler.
 *
 * `sa` is the handler's action: SA_ONSTACK chooses the alternate stack when
 * one is set and not in use, and SA_RESTORER the address the handler
 * returns to. On i686 SA_SIGINFO chooses the rt frame, and without
 * SA_RESTORER the handler returns to the frame's own `retcode`; on x86-64
 * every handler gets the rt frame, and an action without SA_RESTORER gets
 * none. `info` fills the rt frame's siginfo, and `oldmask` is the mask the
 * return restores. The handler starts with the initial floating-point state
 * and its signal number, and for an rt frame the siginfo and ucontext
 * addresses, in the registers the architecture's ABI passes them in.
 * Returns 0, or -1, with `tf` unchanged, when the stack cannot take the
 * frame, when the frame would run off the bottom of the alternate stack it
 * goes on, as Linux does not allow either, or for an x86-64 action without
 * SA_RESTORER.
 */
int sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                   const struct k_siginfo *info, uint64_t oldmask);

/**
 * The most bytes `sigframe_setup` puts below the top of an alternate stack:
 * the floating-point state, which XSAVE makes as large as its components,
 * and the larger frame, each with its alignment. A stack smaller than this
 * may not take a frame. The initial stack's auxiliary vector gives it as
 * AT_MINSIGSTKSZ, as Linux gives it on x86.
 */
uintptr_t sigframe_stack_bytes(void);

/**
 * Restore the state a frame saved, for rt_sigreturn with `rt` and sigreturn
 * without: the general registers, the flags a program may change, the segment
 * selectors this kernel has, and the floating-point state; the rt frame's
 * alternate stack as well. `*mask` receives the mask to restore. Returns 0,
 * or -1 for a frame that cannot be read or would load a faulting state, and
 * on x86-64, which has no plain sigreturn, for a call without `rt`.
 */
int sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask);

/* The bytes of a `siginfo_t`, on every architecture. */
#define SIGINFO_SIZE 128

/** Write the `siginfo_t` of `signo` with `info` into the SIGINFO_SIZE bytes
 * at `out`, as the architecture lays it out: signo, errno and code, then
 * the fault's address, or the sender's pid and uid and a child's status;
 * x86-64 also writes a seccomp SIGSYS's fields and a timer's or a queued
 * signal's value. */
void sigframe_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info);

/** Read the errno, the code, the sender's pid and uid, the status, the
 * value and the address of the `siginfo_t` at `in` into `info`, for
 * rt_sigqueueinfo and PTRACE_SETSIGINFO. */
void sigframe_take_siginfo(const uint8_t *in, struct k_siginfo *info);

/**
 * Whether the user stack pointer `sp` lies on the current process's
 * alternate signal stack.
 */
int sigframe_on_altstack(uintptr_t sp);

#endif /* _SIGFRAME_H_ */
