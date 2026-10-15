/* exit.c: the end of a thread, and of a process with its last thread. */
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
 * A close or a working-directory release can wait for a mount lock, and a
 * wait returns with the process RUNNING, so a ZOMBIE published before those
 * calls would show waitpid a cleanup not yet done, and be lost when the
 * wait returned. The releases that may sleep therefore come first.
 * `in_syscall` is set before them, so an interrupt only asks for a switch
 * (`sched_preempt`), also on the exception and signal-delivery paths, which
 * run outside a system call.
 *
 * The thread's own part comes first: its robust futexes and the word
 * CLONE_CHILD_CLEARTID names, which are in its address space; then the
 * address space, whose last user frees the memory and lets go of the
 * program's file, as Linux's exit_mm does before the zombie can be reaped,
 * and the processor goes on in the kernel's directory; then its descriptor
 * table and directories, and a parent waiting in vfork. A descriptor table
 * another process shares keeps its record locks, which it owns (`lock.h`),
 * and the release of a table by its last sharer drops them.
 *
 * A thread that is not the last becomes a zombie no wait reports but its
 * tracer's, and wakes an execve or a core that waits for the other threads
 * to end; one that is not the leader and has no tracer is freed at the next
 * return to user mode, a traced one by its tracer's wait, and the leader,
 * whose id is the process id, waits for the rest of its group. The last
 * thread deletes the process's POSIX timers, applies its semaphore
 * adjustments, detaches its tracees, ends the rest of a PID namespace whose
 * init it is, gives the children to init, and gives the process its
 * status, the group's when exit_group or a fatal signal ended it and the
 * leader's own otherwise. It then tells the parent; or, when a process
 * other than the parent traces the leader, the tracer, whose wait reports
 * the end first and then tells the parent (`ptrace_exit_release`).
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

        proc_child_info(leader, parent, &info);
        signal_post_info(parent, SIGCHLD, &info);
        wq_wake_all(&parent->tg->chld);
        if (parent->state == PROC_BLOCKED) {
            sched_make_ready(parent);
        }
    }
    return 1;
}
