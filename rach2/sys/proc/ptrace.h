#ifndef _PTRACE_H_
#define _PTRACE_H_

#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "../isr.h"
#include "process.h"

/*
 * ptrace(2): one process, the tracer, stops, reads, changes and resumes the
 * threads of another, its tracees, as GDB and strace do. See
 * ptrace.Readme.md.
 */

/* The PT_ flags of a thread's `ptrace`. */
#define PT_TRACED    0x01u /* a tracer has it */
#define PT_SEIZED    0x02u /* the tracer attached with PTRACE_SEIZE */
#define PT_SYSCALL   0x04u /* it stops at each system call's entry and exit */
#define PT_STEP      0x08u /* the kernel set TF for PTRACE_SINGLESTEP */
#define PT_INTERRUPT 0x10u /* PTRACE_INTERRUPT asked for a stop */
#define PT_START     0x20u /* a seized thread's first stop is still to come */

/* The events of a stop, in bits 16-23 of its wait status, and the option bit
 * that asks for each, 1 << event. */
#define PTRACE_EVENT_FORK       1
#define PTRACE_EVENT_VFORK      2
#define PTRACE_EVENT_CLONE      3
#define PTRACE_EVENT_EXEC       4
#define PTRACE_EVENT_VFORK_DONE 5
#define PTRACE_EVENT_EXIT       6
#define PTRACE_EVENT_STOP       128

/** ptrace (26): EBX=request, ECX=pid, EDX=addr, ESI=data. */
int32_t sys_ptrace(isr_params_t *params);

/**
 * The stops a traced thread makes as it returns to user mode, before any
 * signal: the first stop of a thread a seized tracer attached, and the stop
 * PTRACE_INTERRUPT asked for.
 */
void ptrace_return_stops(void);

/**
 * A signal-delivery stop for `signo`, which the current thread has taken off
 * its pending sets with `*info`: a traced thread stops for its tracer, which
 * may change the signal, or its siginfo, or give none. Returns the signal to
 * act on, 0 for none; SIGKILL and an untraced thread's signal pass.
 */
int ptrace_signal_stop(int signo, struct k_siginfo *info);

/**
 * The stop a traced thread makes when its process stops: it is reported as a
 * stop with the stop signal, PTRACE_EVENT_STOP when the tracer seized it, and
 * the tracer may resume the thread while the process stays stopped. Returns
 * nonzero when it stopped.
 */
int ptrace_group_stop(int signo);

/** The event stop `event` with `msg` for PTRACE_GETEVENTMSG, when the current
 * thread is traced and its options ask for the event. */
void ptrace_event(int event, uint32_t msg);

/** The stop at the entry, or with `leaving` the exit, of system call `nr`,
 * when the current thread's tracer asked for them with PTRACE_SYSCALL; `tf`
 * is its trap frame. */
void ptrace_syscall_stop(isr_params_t *tf, uint32_t nr, int leaving);

/**
 * Attach the child `child` the current thread makes with the clone flags
 * `flags` to its tracer, when the options ask for the event this clone is;
 * the child then stops first. Returns the event to report, 0 for none.
 */
int ptrace_fork_attach(process_t *child, uint32_t flags);

/** The stop a traced thread makes after its execve, whose thread id was
 * `old_tid` before: PTRACE_EVENT_EXEC with the options, else a SIGTRAP. */
void ptrace_exec(uint32_t old_tid);

/** Tell the tracer of `p` of its end, the thread's, or, for a leader whose
 * process has ended, the process's: a SIGCHLD with the siginfo
 * `proc_child_info` gives, CLD_EXITED with the exit code or CLD_KILLED or
 * CLD_DUMPED with the signal, and a wake of its wait. */
void ptrace_exit_notify(process_t *p);

/** The tracer's wait has reported the end of the process `t` leads, whose
 * parent is another process: `t` is let go, and its parent is told of the
 * end and may reap it. */
void ptrace_exit_release(process_t *t);

/** Detach the tracees of the process `tgid`, which is ending; with
 * PTRACE_O_EXITKILL they are sent SIGKILL. The parent of a tracee whose
 * process has ended, the tracer's wait not having reported it, is told of
 * the end, as `ptrace_exit_release` tells it. */
void ptrace_tracer_gone(uint32_t tgid);

/** Load the debug registers of `next`, or clear what `old` left, at a switch
 * between the two. */
void ptrace_switch_debug(const process_t *old, const process_t *next);

/**
 * Read the debug status (DR6) after a debug exception into the current
 * thread's copy, clear it for the next exception, and say what it was: the
 * siginfo code, TRAP_TRACE for a single step and TRAP_HWBKPT for a hardware
 * breakpoint, 0 for neither.
 */
int32_t ptrace_debug_status(void);

#endif /* _PTRACE_H_ */
