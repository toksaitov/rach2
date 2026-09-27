#include "process.h"
#include "ptrace.h"
#include "tgroup.h"
#include "../fs/vfs.h"

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
 * its robust futexes, its file descriptions and working directory, and a
 * parent waiting in vfork. The process's last thread drops the process's
 * record locks itself when its descriptor table outlives it, shared with
 * another process. A thread that is not the last becomes a zombie
 * no wait reports but its tracer's; one that is not the leader and has no
 * tracer is freed from the next return to user mode, a traced one by its
 * tracer's wait, and the leader, whose id is the process id, waits for the
 * rest of its group. The last thread detaches the process's own tracees,
 * reparents the children onto init, gives the process its status, the
 * group's when exit_group or a fatal signal ended it and the leader's own
 * otherwise, and notifies the parent; or, when a process other than the
 * parent traces the leader, the tracer, whose wait reports the end first and
 * then tells the parent (`ptrace_exit_release`). The address space and the
 * descriptor-table allocation are not released here: a normal exit leaves
 * them to the reap, and a death by signal drops the address space before it
 * calls this, since a release that sleeps must come before the zombie is
 * published.
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
    if (tg->live == 1 && self->fds && self->fds->refs > 1) {
        fd_table_drop_locks(self->fds);
    }
    proc_files_put(self);
    proc_fs_put(self);
    proc_vfork_release(self);
    self->exit_code = status;
    tg->live--;
    if (tg->live == 0) {
        ptrace_tracer_gone(self->tgid);
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
            parent->state = PROC_READY;
        }
    }
    return 1;
}
