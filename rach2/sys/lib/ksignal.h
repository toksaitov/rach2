#ifndef _KSIGNAL_H_
#define _KSIGNAL_H_

#include "kint.h"

/*
 * Linux i386 signals: numbers, the kernel-side sigaction, and the per-signal
 * default-action classes. The delivery machinery (per-process disposition,
 * pending and blocked masks, delivery on return-to-user via a signal frame on
 * the user stack, and rt_sigreturn) lives in syscall.c and the ISR return path.
 * Standard signals only (1..64); no real-time queueing.
 */
#define SIGHUP    1
#define SIGINT    2 /* terminal interrupt (CTRL+C) */
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6 /* abort() */
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9 /* uncatchable kill */
#define SIGUSR1   10
#define SIGSEGV   11 /* invalid memory reference */
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15 /* termination */
#define SIGSTKFLT 16
#define SIGCHLD   17 /* child stopped or exited */
#define SIGCONT   18
#define SIGSTOP   19 /* uncatchable stop */
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define KSIG_MAX 64 /* signals 1..64 are deliverable */

/* Disposition sentinels stored where a handler address goes. */
#define KSIG_DFL 0u /* default action */
#define KSIG_IGN 1u /* ignore */

/* sa_flags bits (Linux i386 values). The kernel honors SA_RESTORER, SA_NODEFER,
 * and SA_RESETHAND. SA_RESTART is accepted and round-trips through sigaction
 * but is NOT honored: an interrupted slow syscall always returns -EINTR rather
 * than restarting, so a handler must not rely on the kernel re-issuing the
 * call. */
#define SA_SIGINFO   0x00000004u
#define SA_RESTORER  0x04000000u
#define SA_RESTART   0x10000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

/*
 * The kernel-side sigaction as musl's rt_sigaction(174) passes it on i386:
 * handler, flags, the restorer trampoline (musl's __restore_rt then
 * rt_sigreturn), and an 8-byte signal mask (signals 1..64). Byte-identical to
 * musl's `struct k_sigaction`.
 */
struct k_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint32_t mask[2]; /* low = signals 1..32, high = 33..64 */
};

/* A 64-bit signal set: bit (signo-1) set means signo is in the set. */
#define KSIG_BIT(signo) (1ULL << ((signo) - 1))

/* Signals that can never be caught, blocked, or ignored. */
#define KSIG_UNCATCHABLE (KSIG_BIT(SIGKILL) | KSIG_BIT(SIGSTOP))

/* Default-action classes; ksig_default_action in syscall.c maps a signal to
 * one. The common default is TERM; SIGCHLD/SIGURG/SIGWINCH/SIGCONT ignore; the
 * job-control signals SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU stop. */
enum ksig_default {
    KSIG_DEFAULT_TERM,   /* terminate (the common case, incl. core signals) */
    KSIG_DEFAULT_IGNORE, /* ignore */
    KSIG_DEFAULT_STOP    /* stop (job control) */
};

#endif /* _KSIGNAL_H_ */
