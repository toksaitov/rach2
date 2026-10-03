#ifndef _KSIGNAL_H_
#define _KSIGNAL_H_

#include "kint.h"

/*
 * Linux i386 signals: numbers, the kernel-side sigaction, and the per-signal
 * default-action classes. The delivery machinery (per-process disposition,
 * pending and blocked masks, delivery on return-to-user via a signal frame on
 * the user stack, and rt_sigreturn) lives in syscall_proc.c, sigframe.c and the
 * ISR return path. Signals 1..64, each with one pending bit; no real-time
 * queueing.
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

/* sa_flags bits (Linux i386 values). The kernel honors SA_SIGINFO, SA_ONSTACK,
 * SA_RESTORER, SA_RESTART, SA_NODEFER and SA_RESETHAND; SA_NOCLDSTOP and
 * SA_NOCLDWAIT are stored and have no effect. */
#define SA_NOCLDSTOP 0x00000001u
#define SA_SIGINFO   0x00000004u
#define SA_RESTORER  0x04000000u
#define SA_ONSTACK   0x08000000u
#define SA_RESTART   0x10000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

/* si_code values (Linux). */
#define SI_USER       0
#define SI_KERNEL     0x80
#define SI_QUEUE      (-1)
#define SI_MESGQ      (-3)
#define SI_TKILL      (-6)
#define SI_TIMER      (-2)
#define ILL_ILLOPN    2
#define FPE_INTDIV    1
#define FPE_FLTDIV    3
#define FPE_FLTOVF    4
#define FPE_FLTUND    5
#define FPE_FLTRES    6
#define FPE_FLTINV    7
#define SEGV_MAPERR   1
#define SEGV_ACCERR   2
#define BUS_ADRALN    1
#define TRAP_BRKPT    1
#define TRAP_TRACE    2
#define TRAP_HWBKPT   4
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

/*
 * What the kernel keeps with a pending signal for the `siginfo_t` its
 * handler receives: `si_code`, the sender's pid and uid, a child's status,
 * a fault's address, the word a timer or a queued signal carries
 * (`si_value`, which on i386 is where a child's status is, and is kept in
 * `status` as well), and `si_errno`, which only SIGSYS from a seccomp
 * filter sets. One record per signal number, like the pending bit: the
 * first instance's record stays until the signal is delivered.
 */
struct k_siginfo {
    int32_t code;
    uint32_t pid;
    uint32_t uid;
    int32_t status;
    uintptr_t addr;
    uintptr_t value;
    int32_t err;
};

/*
 * The kernel-side sigaction as musl's rt_sigaction passes it on i386:
 * handler, flags, the restorer trampoline (musl's __restore_rt then
 * rt_sigreturn), and an 8-byte signal mask (signals 1..64). Byte-identical to
 * musl's `struct k_sigaction`.
 */
struct k_sigaction {
    uintptr_t handler;
    unsigned long flags;
    uintptr_t restorer;
    uint32_t mask[2]; /* low = signals 1..32, high = 33..64 */
};

/* A 64-bit signal set: bit (signo-1) set means signo is in the set. */
#define KSIG_BIT(signo) (1ULL << ((signo) - 1))

/* The bit of a wait status that says the process left a core file. */
#define WCOREFLAG 0x80

/* Signals that can never be caught, blocked, or ignored. */
#define KSIG_UNCATCHABLE (KSIG_BIT(SIGKILL) | KSIG_BIT(SIGSTOP))

/* Default-action classes; ksig_default_action in syscall_proc.c maps a
 * signal to one. The common default is TERM; the signals of a program fault,
 * SIGQUIT, SIGABRT, SIGXCPU and SIGXFSZ end the process with a core file;
 * SIGCHLD/SIGURG/SIGWINCH/SIGCONT ignore; the job-control signals
 * SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU stop. */
enum ksig_default {
    KSIG_DEFAULT_TERM,   /* terminate (the common case) */
    KSIG_DEFAULT_CORE,   /* terminate with a core file */
    KSIG_DEFAULT_IGNORE, /* ignore */
    KSIG_DEFAULT_STOP    /* stop (job control) */
};

#endif /* _KSIGNAL_H_ */
