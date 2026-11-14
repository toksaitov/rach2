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
#include "../arch/arch.h"

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

/* The layouts of a siginfo's union, Linux 6.12's enum siginfo_layout
 * (include/linux/signal.h), but SIL_FAULT_TRAPNO, which only the codes of
 * alpha and sparc have. */
#define SIL_KILL             0
#define SIL_TIMER            1
#define SIL_POLL             2
#define SIL_FAULT            3
#define SIL_FAULT_MCEERR     4
#define SIL_FAULT_BNDERR     5
#define SIL_FAULT_PKUERR     6
#define SIL_FAULT_PERF_EVENT 7
#define SIL_CHLD             8
#define SIL_RT               9
#define SIL_SYS              10

/**
 * The layout of the union of a siginfo with the signal `signo` and the code
 * `code`, as Linux 6.12's siginfo_layout (kernel/signal.c) chooses it: a
 * code between SI_USER and SI_KERNEL has the layout of its signal's codes up
 * to the last of them, with SEGV_BNDERR, SEGV_PKUERR, BUS_MCEERR_AR and _AO
 * and TRAP_PERF fault layouts of their own, and otherwise SIL_POLL up to
 * NSIGPOLL, whatever the signal, and SIL_KILL past it; SI_TIMER is
 * SIL_TIMER, SI_SIGIO SIL_POLL and any other negative code SIL_RT; SI_USER
 * and SI_KERNEL and the codes above it are SIL_KILL (siginfo.c).
 */
int siginfo_layout(int signo, int32_t code);

/** Build the union `info` keeps from its members by `layout` and mark it
 * kept, so that every output reads it by its signal's and code's layout:
 * for a ptrace stop no signal makes, whose union has the thread's pid and
 * uid as SIL_KILL places them, as Linux's ptrace_do_notify writes them,
 * whatever its code says. */
void siginfo_pack(struct k_siginfo *info, int layout);

/** Write the native `siginfo_t` of `signo` with `info` into the
 * SIGINFO_SIZE bytes at `out`: signo, errno and code, then the union at 12
 * on i386 and 16 on x86-64, kept whole or built by the layout (siginfo.c). */
void sigframe_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info);

/** Read the native `siginfo_t` at `in` into `info`, its union whole, for a
 * signal `signo`, whose layout gives the members the rest of the kernel
 * reads, for rt_sigqueueinfo and PTRACE_SETSIGINFO: 0, or -E2BIG for a code
 * of a layout Linux does not know with bytes past the union (siginfo.c). */
int32_t sigframe_take_siginfo(const uint8_t *in, int signo, struct k_siginfo *info);

/** Write the 128-byte `struct signalfd_siginfo` of `signo` with `info` at
 * `out`, the fields of the layout, as Linux's signalfd_copyinfo writes them
 * (siginfo.c). */
void signalfd_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info);

/** Write the `siginfo_t` of `signo` with `info` as the calling system
 * call's ABI lays it out: the i386 one (`ia32_fill_siginfo`) for a call by
 * `int $0x80` on x86-64, else the architecture's own. For the siginfo a
 * call returns, such as sigtimedwait's, waitid's and ptrace's. Defined in
 * syscall_proc.c. */
void siginfo_to_caller(uint8_t *out, int signo, const struct k_siginfo *info);

/** Read a `siginfo_t` the calling system call passed, in its ABI's layout,
 * as `siginfo_to_caller` chooses it, as one of the signal `signo`: 0, or
 * the native reader's -E2BIG; the i386 one, as Linux's
 * post_copy_siginfo_from_user32, refuses nothing. Defined in
 * syscall_proc.c. */
int32_t siginfo_from_caller(const uint8_t *in, int signo, struct k_siginfo *info);

#if defined(IA32_SYSCALL_TABLE_SIZE)
/* The bytes of an i386 `siginfo_t` through si_status, which waitid writes
 * for an i386 caller. */
#define SIGINFO32_CHLD_BYTES 24

/** Write the i386 `siginfo_t`, Linux's `compat_siginfo`, of `signo` with
 * `info` into the SIGINFO_SIZE bytes at `out`: the fields of the layout
 * Linux's siginfo_layout gives the signal and code, at their i386 offsets,
 * with 32-bit addresses and values (siginfo.c). */
void ia32_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info);

/** Read the i386 `siginfo_t` at `in` into `info` as one of the signal
 * `signo`: the fields of the layout it and the code have, and no others,
 * as Linux's post_copy_siginfo_from_user32 reads them (siginfo.c). */
void ia32_take_siginfo(const uint8_t *in, int signo, struct k_siginfo *info);
#endif

/**
 * Whether the user stack pointer `sp` lies on the current process's
 * alternate signal stack.
 */
int sigframe_on_altstack(uintptr_t sp);

#endif /* _SIGFRAME_H_ */
