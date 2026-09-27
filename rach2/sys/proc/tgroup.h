#ifndef _TGROUP_H_
#define _TGROUP_H_

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
 * as they end. `refs` counts the records that point at the group.
 */
struct tgroup {
    uint32_t refs;
    uint32_t live;          /* threads that have not ended */
    struct process *leader; /* its id is the process id */
    struct k_sigaction action[KSIG_MAX + 1];
    uint64_t pending; /* signals sent to the process, which any thread takes */
    struct k_siginfo info[KSIG_MAX + 1];
    uint32_t stop;             /* the signal of a stop in effect, 0 for none */
    uint32_t exiting;          /* exit_group or a fatal signal ends every thread */
    int32_t exit_code;         /* the wait status the process ends with then */
    struct process *exec_task; /* the thread whose execve ends the others */
    uint32_t utime;            /* the CPU time of the threads that ended */
    uint32_t stime;
    wait_queue_t chld; /* threads waiting for a child to change state */
    wait_queue_t gone; /* an execve waiting for the other threads to end */
};

/** A new group with `leader` as its one thread; NULL when the heap has no
 * room. */
struct tgroup *tgroup_new(struct process *leader);

/** Drop a record's reference to `tg`; the last one frees it. */
void tgroup_put(struct tgroup *tg);

#endif /* _TGROUP_H_ */
