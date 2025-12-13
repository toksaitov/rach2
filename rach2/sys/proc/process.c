#include "process.h"
#include "../lib/kstring.h"
#include "../fs/fd.h"

/*
 * Process table - all process slots
 */
process_t proc_table[MAX_PROCS];

/*
 * Currently running process
 */
process_t *current_proc = NULL;

/*
 * Next PID to assign
 */
static uint32_t next_pid = 0;

void init_proc(void)
{
    uint32_t i;

    /* Clear all process slots */
    for (i = 0; i < MAX_PROCS; i++) {
        k_memset(&proc_table[i], 0, sizeof(process_t));
        proc_table[i].state = PROC_UNUSED;
    }

    next_pid = 0;
    current_proc = NULL;
}

process_t *alloc_process(void)
{
    uint32_t i;

    /* Find first unused slot */
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            k_memset(&proc_table[i], 0, sizeof(process_t));
            proc_table[i].pid = next_pid++;
            proc_table[i].state = PROC_READY;

            /* Initialize file descriptor table with stdin/stdout/stderr */
            fd_table_init(&proc_table[i].fds);
            fd_init_stdio(&proc_table[i].fds);

            return &proc_table[i];
        }
    }

    /* No free slots */
    return NULL;
}

void free_process(process_t *proc)
{
    if (proc) {
        proc->state = PROC_UNUSED;
    }
}

process_t *get_process(uint32_t pid)
{
    uint32_t i;

    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid) {
            return &proc_table[i];
        }
    }

    return NULL;
}

void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    uint32_t *sp;

    /*
     * Set up the kernel stack for first context switch.
     * switch_context expects the stack to have saved callee registers:
     *
     *   [entry_point]  <- return address (where to start)
     *   [ebp]          <- saved EBP (0 for new process)
     *   [ebx]          <- saved EBX (0)
     *   [esi]          <- saved ESI (0)
     *   [edi]          <- saved EDI (0) <- ESP points here
     */

    sp = (uint32_t *)proc->kernel_stack;

    /* Push return address (entry point) */
    --sp;
    *sp = (uint32_t)entry_point;

    /* Push saved callee registers (all zero for new process) */
    --sp;
    *sp = 0;  /* EBP */
    --sp;
    *sp = 0;  /* EBX */
    --sp;
    *sp = 0;  /* ESI */
    --sp;
    *sp = 0;  /* EDI */

    /* Save stack pointer in process regs */
    proc->regs.esp = (uint32_t)sp;
}
