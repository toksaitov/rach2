#include "process.h"
#include "ptrace.h"
#include "sched.h"
#include "tgroup.h"
#include "../fs/vfs.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../io/timerfd.h"
#include "../ipc/sysv.h"
#include "ns.h"

/**
 * End the current thread: release what it holds, and when it is the last
 * thread of its process, make the process available to its reaper.
 *
 * A close or a working-directory release can wait for a mount lock. A wait
 * returns with the process RUNNING, so publishing ZOMBIE before those calls
 * both exposes unfinished cleanup to waitpid and loses the terminal state.
 * These sleeping releases precede publication. After this helper returns,
 * the caller must neither block nor permit interrupt preemption before its
 * final schedule: the parent may then reap the record and kernel stack. The
 * helper sets `in_syscall` first, so an interrupt only asks for a switch
 * (`sched_preempt`), also on the exception and signal-delivery paths, which
 * run outside a system call.
 *
 * The thread's own part comes first: the word CLONE_CHILD_CLEARTID names and
 * its robust futexes, which are in its address space; then the address space,
 * whose last user frees the memory and lets go of the program's file, as
 * Linux's exit_mm does before the zombie can be reaped, and the processor goes
 * on in the kernel's directory; then its file descriptions and working
 * directory, and a parent waiting in vfork. A descriptor table another process
 * shares keeps its record locks, which it owns (`lock.h`), and the release of a
 * table by its last sharer drops them. A thread that is not the last becomes a
 * zombie no wait reports but its tracer's; one that is not the leader and has
 * no tracer is freed from the next return to user mode, a traced one by its
 * tracer's wait, and the leader, whose id is the process id, waits for the rest
 * of its group. The last thread detaches the process's own tracees, reparents
 * the children onto init, gives the process its status, the group's when
 * exit_group or a fatal signal ended it and the leader's own otherwise, and
 * notifies the parent; or, when a process other than the parent traces the
 * leader, the tracer, whose wait reports the end first and then tells the
 * parent (`ptrace_exit_release`). The descriptor-table allocation is not
 * released here: the reap frees it. A death by signal writes its core file
 * before it calls this, while the address space is there.
 *
 * `status` is the thread's encoded wait status. Returns nonzero when the
 * process ended with this thread. The caller must not return to user mode.
 */
int proc_exit_cleanup(int32_t status)
{
    process_t *self = current_proc;
    struct tgroup *tg = self->tg;
    process_t *leader = tg->leader;
    process_t *parent;

    self->in_syscall = 1;
    proc_thread_gone(self);
    mm_drop(self);
    switch_page_directory(_kernel_dir);
    proc_files_put(self);
    proc_fs_put(self);
    proc_vfork_release(self);
    self->exit_code = status;
    tg->live--;
    if (tg->live == 0) {
        ptimer_delete_all(tg);
        sysv_exit(tg);
        ptrace_tracer_gone(self->tgid);
        ns_reaper_gone(leader);
        reparent_children(self->tgid);
        if (tg->exiting) {
            leader->exit_code = tg->exit_code;
        }
    }
    self->state = PROC_ZOMBIE;
    if (self->ptrace & PT_TRACED) {
        ptrace_exit_notify(self);
    } else if (self != leader) {
        proc_dead_threads++;
    }
    if (tg->live != 0) {
        wq_wake_all(&tg->gone);
        return 0;
    }
    if ((leader->ptrace & PT_TRACED) && leader->tracer != leader->parent_pid) {
        if (leader != self) {
            ptrace_exit_notify(leader);
        }
        return 1;
    }
    parent = get_process(leader->parent_pid);
    if (parent) {
        struct k_siginfo info;

        proc_child_info(leader, &info);
        signal_post_info(parent, SIGCHLD, &info);
        wq_wake_all(&parent->tg->chld);
        if (parent->state == PROC_BLOCKED) {
            sched_make_ready(parent);
        }
    }
    return 1;
}
