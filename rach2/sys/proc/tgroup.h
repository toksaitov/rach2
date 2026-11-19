/* The thread group: what the threads of one process share. */
#ifndef _TGROUP_H_
#define _TGROUP_H_

#include "../lib/spinlock.h"
#include "../lib/kint.h"
#include "../lib/ksignal.h"
#include "process.h"
#include "wait_queue.h"

/*
 * A thread group: the threads of one process, and what they share beside
 * the address space. Each thread is a process record of its own, with its
 * own id, registers, TLS base, blocked mask and pending signals; the group
 * has the dispositions, the signals sent to the process as a whole, and the
 * process's end.
 *
 * fork makes a new group; clone with CLONE_THREAD adds the new thread to the
 * caller's. The thread whose id is the process id, `leader`, keeps its
 * record until the whole group has ended, so the parent reaps the process
 * once, when the last thread is gone; the other threads' records are freed
 * after they end, at the next return to user mode or, for a traced thread,
 * by its tracer's wait. `refs` counts the records that point at the group.
 */
struct ktimer;
struct pidfd;

/* Signals sent to several processes while one fork builds a child. The
 * record lives on the calling thread's stack, linked under interrupts off
 * until the child starts or the fork fails, as Linux's multiprocess list. */
struct fork_pending {
    struct fork_pending *next;
    uint64_t pending; /* bit signo-1, with no siginfo, as Linux keeps it */
};

struct tgroup {
    uint32_t refs;
    uint32_t live;          /* threads that have not ended */
    struct process *leader; /* its id is the process id */
    /* Every published record of the process, oldest first, by
     * `group_link` (process.c, under the big kernel lock), as Linux's
     * `thread_head`: the sums of the process's usage walk it. */
    struct process *threads;
    /* Unique for the boot, from 1: names the lifetime of the process id, so
     * that a process group given that id later is not taken for this
     * process's (`pgrp_serial` in process.h). */
    uint64_t serial;
    /* The dispositions, by signal number, index 0 unused. */
    struct k_sigaction action[KSIG_MAX + 1];
    uint64_t pending;      /* signals sent to the process, which any thread takes */
    struct ktimer *timers; /* timer_create's, in the order of their ids */
    /* Deleted POSIX timers whose expirations wait for another sender's
     * signal of their number to be taken, which posts them (`ptimer_rearm`),
     * or discarded (`ptimer_discard`, `ptimer_thread_exit`); an exec and the
     * end of the process free them all (`ptimer_delete_all`). */
    struct ktimer *timers_gone;
    /* The siginfo of each signal pending in `pending`, by signal number. */
    struct k_siginfo info[KSIG_MAX + 1];
    uint32_t stop;             /* the signal of a stop in effect, 0 for none */
    uint32_t exiting;          /* exit_group or a fatal signal ends every thread */
    uint32_t core_dumping;     /* its core is being written; the others park */
    int32_t exit_code;         /* the wait status the process ends with then */
    struct process *exec_task; /* the thread whose execve ends the others */
    /* An init, which a signal left at its default does not end, as Linux's
     * SIGNAL_UNKILLABLE: the system's real init and the first process of a
     * PID namespace below the first. A forced signal at its default clears
     * it when the thread it is forced on is not traced (`signal_force`), as
     * Linux 6.12.115's force_sig_info_to_task does (kernel/signal.c:
     * 1348-1354). */
    uint32_t unkillable;
    /* A successful exec since this group was made; fork starts at 0. */
    uint32_t execed;
    /* clone with exit signal zero is reaped with __WCLONE, without SIGCHLD. */
    uint32_t no_child_signal;
    /* Exec generation of this group and of its real parent at clone. */
    uint32_t exec_generation;
    uint32_t parent_exec_generation;
    /* The user and system CPU time, in ticks, of the threads that ended. */
    uint64_t utime;
    uint64_t stime;
    wait_queue_t chld; /* threads waiting for a child to change state */
    wait_queue_t gone; /* an execve or a core waiting on the other threads */
    /* The signalfd readers and pollers of every thread, woken by each signal
     * sent to the process or to one of its threads, as Linux's
     * sighand->signalfd_wqh (`sig_notify_fd`). */
    wait_queue_t sigfd;
    /* The CPU time of the children the process has reaped, each with the
     * children it reaped, which a reap adds and wait4, times and getrusage
     * report. The group keeps it, not the leader, since a traced thread's
     * record, whose end a tracer's wait reports, may outlive the leader's. */
    uint64_t cutime;
    uint64_t cstime;
    /* The page faults of the threads that ended, minor and major, and of the
     * children the process has reaped, each with the children it reaped, as
     * `utime` and `cutime` keep the CPU time (`proc_faults`). */
    unsigned long min_flt;
    unsigned long maj_flt;
    unsigned long cmin_flt;
    unsigned long cmaj_flt;
    /* Context switches of ended threads, and of reaped children with their
     * children, as the page-fault totals above. Native unsigned longs. */
    unsigned long nvcsw;
    unsigned long nivcsw;
    unsigned long cnvcsw;
    unsigned long cnivcsw;
    /* The most memory, in kilobytes, an address space of the process that
     * has ended had in use, at an exec or at the end of the process
     * (`mm_resident_peak`), and the most any reaped child reports, which
     * getrusage and wait4 give as `ru_maxrss`. */
    unsigned long maxrss_kb;
    unsigned long cmaxrss_kb;
    /* The interval timers of the process's processor time, setitimer's
     * ITIMER_VIRTUAL and ITIMER_PROF, at 0 and 1: the nanoseconds left before
     * SIGVTALRM, counted in user mode, or SIGPROF, counted in both modes, 0
     * when disarmed, and the nanoseconds to arm the timer with again, 0 for a
     * single signal, kept while disarmed, as Linux's `struct cpu_itimer` keeps
     * them; setitimer adds a tick to a first value, as Linux's set_cpu_itimer
     * does. Each tick charged takes TICK_NS off. A fork starts with both
     * disarmed and an exec keeps them, as on Linux. The tick handler of each
     * processor counts them down without the big lock, under `cpu_lock`, which
     * setitimer and getitimer take too. */
    uint64_t cpu_left[2];
    uint64_t cpu_interval[2];
    struct spinlock cpu_lock;
    /* The OOM score adjustment /proc/<pid>/oom_score_adj reads and writes,
     * -1000 to 1000, and the least value a caller without privilege may
     * write there, which a privileged caller's write sets to what it wrote,
     * as Linux keeps `oom_score_adj_min`. Both start at 0, a fork passes them
     * on and an exec keeps them. Nothing else reads them: this kernel has no
     * OOM killer to weigh a process by them. */
    int32_t oom_score_adj;
    int32_t oom_score_adj_min;
    /* Whether the process is a child subreaper, which prctl's
     * PR_SET_CHILD_SUBREAPER sets: the orphans of its descendants come to it
     * rather than to init (`reparent_children`). A fork starts at 0 and an
     * exec keeps it, as on Linux. */
    uint32_t child_subreaper;
    /* The process has left its session's controlling terminal by TIOCNOTTY
     * without leading the session, as Linux's no_tty clears signal->tty:
     * the terminal is not its controlling terminal (`tty_is_ctty`), though
     * the session keeps it. A fork passes it on, an exec keeps it and
     * setsid clears it, as on Linux. */
    uint32_t tty_detached;
    /* The pidfds of the process and of its threads, each open one, linked
     * through their `next` under the big lock: an end or a reap wakes the
     * pollers of those whose id's lifetime it changed (`pidfd.h`). */
    struct pidfd *pidfds;
    struct fork_pending *forks; /* children being built by threads of this group */
    /* The session's scheduling group, which setsid makes and a fork passes
     * on, or NULL for the root group, as Linux's signal->autogroup
     * (autogroup.h). The group keeps a reference for each group naming it. */
    struct autogroup *autogroup;
};

/** A new group with `leader` as its one thread; NULL when the heap has no
 * room. */
struct tgroup *tgroup_new(struct process *leader);

/** Drop a record's reference to `tg`; the last one frees it. */
void tgroup_put(struct tgroup *tg);

#endif /* _TGROUP_H_ */
