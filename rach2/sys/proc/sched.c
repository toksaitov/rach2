#include "sched.h"

/*
 * Scheduler enabled flag
 * When 0, schedule() does nothing (for critical sections or boot)
 */
static volatile int _sched_enabled = 0;

/*
 * Last scheduled process index for round-robin
 */
static uint32_t _last_sched_idx = 0;

void init_sched(void)
{
    _sched_enabled = 0;
    _last_sched_idx = 0;
}

void sched_enable(void)
{
    _sched_enabled = 1;
}

void sched_disable(void)
{
    _sched_enabled = 0;
}

int sched_is_enabled(void)
{
    return _sched_enabled;
}

void schedule(void)
{
    uint32_t i;
    uint32_t start_idx;
    process_t *next = NULL;
    process_t *old;
    uint32_t saved_flags;

    /* Do nothing if scheduler is disabled */
    if (!_sched_enabled) {
        return;
    }

    /* Do nothing if no current process */
    if (!current_proc) {
        return;
    }

    /*
     * Round-robin: start searching from the process after the last scheduled
     * Wrap around the process table looking for a READY process
     */
    start_idx = (_last_sched_idx + 1) % MAX_PROCS;

    for (i = 0; i < MAX_PROCS; i++) {
        uint32_t idx = (start_idx + i) % MAX_PROCS;
        process_t *p = &proc_table[idx];

        if (p->state == PROC_READY) {
            next = p;
            _last_sched_idx = idx;
            break;
        }
    }

    /* If no other READY process found, continue running current */
    if (!next) {
        return;
    }

    /* If same process, nothing to do */
    if (next == current_proc) {
        return;
    }

    /* Switch to the new process */
    old = current_proc;

    /* Mark old process as READY (unless it's blocked/zombie) */
    if (old->state == PROC_RUNNING) {
        old->state = PROC_READY;
    }

    /* Mark new process as RUNNING */
    next->state = PROC_RUNNING;

    /*
     * Publish current_proc and switch with interrupts OFF. A timer IRQ between
     * "current_proc = next" and the stack/CR3 swap inside switch_context would
     * see current_proc == next with next->in_syscall == 0 and re-enter
     * schedule() while still on the old stack with the TLS descriptor already
     * rewritten -- corrupting next->regs.esp. We save the caller's IF, disable
     * interrupts across the switch, and restore IF afterward. Each process does
     * this symmetrically, so it resumes with its own IF; a process entered for
     * the first time bypasses the restore and gets its IF from iret instead.
     */
    __asm__ __volatile__("pushf\n\t"
                         "cli\n\t"
                         "pop %0"
                         : "=r"(saved_flags)
                         :
                         : "memory");
    current_proc = next;

    /* Perform the actual context switch */
    switch_context(old, next);

    if (saved_flags & 0x200) {
        __asm__ __volatile__("sti");
    }
}

void sched_yield(void)
{
    /* Voluntarily yield - just call schedule */
    schedule();
}
