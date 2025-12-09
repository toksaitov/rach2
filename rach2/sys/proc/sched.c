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
    current_proc = next;

    /* Perform the actual context switch */
    switch_context(old, next);
}

void sched_yield(void)
{
    /* Voluntarily yield - just call schedule */
    schedule();
}
