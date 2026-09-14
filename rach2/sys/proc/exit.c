#include "process.h"
#include "../fs/vfs.h"

/**
 * Release files before making the current process available to its reaper.
 *
 * A close or a working-directory release can wait for a mount lock. A wait
 * returns with the process RUNNING, so publishing ZOMBIE before those calls
 * both exposes unfinished cleanup to waitpid and loses the terminal state.
 * Keep interrupt preemption deferred through the caller's remaining teardown;
 * only an explicit wait or the final schedule may switch away.
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
        signal_post(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED) {
            parent->state = PROC_READY;
        }
    }
}
