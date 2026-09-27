#include "process.h"
#include "../fs/vfs.h"

/**
 * Release files before making the current process available to its reaper.
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
 * This helper releases file descriptions and cwd and reparents the children
 * onto init; it does not free the address space or the descriptor-table
 * allocation. Normal child exit and failed exec leave their remaining
 * mappings to do_wait. Signal death frees its mappings and clears cr3 before
 * scheduling. A successful reap releases the remaining storage.
 *
 * `status` is the encoded wait status. The caller must not return to user mode.
 */
void proc_exit_cleanup(int32_t status)
{
    process_t *parent;

    current_proc->in_syscall = 1;
    release_fd_table(&current_proc->fds);
    vfs_cwd_clear(current_proc);
    reparent_children(current_proc->pid);

    current_proc->exit_code = status;
    current_proc->state = PROC_ZOMBIE;
    parent = get_process(current_proc->parent_pid);
    if (parent) {
        struct k_siginfo info;

        proc_child_info(current_proc, &info);
        signal_post_info(parent, SIGCHLD, &info);
        if (parent->state == PROC_BLOCKED) {
            parent->state = PROC_READY;
        }
    }
}
