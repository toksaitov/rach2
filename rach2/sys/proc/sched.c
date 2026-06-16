#include "sched.h"

/** Nonzero once preemptive scheduling is enabled; schedule() is a no-op until then. */
static volatile int _sched_enabled = 0;

/** Index of the last process scheduled, advanced for round-robin fairness. */
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

/**
 * Pick the next READY process round-robin and switch to it.
 *
 * Does nothing if scheduling is disabled, there is no current process, or no
 * other process is READY. The switch runs with interrupts disabled: a timer IRQ
 * landing between publishing current_proc and the stack and CR3 swap inside
 * switch_context would see current_proc == next with next->in_syscall == 0,
 * pass the timer handler's reschedule gate, and re-enter schedule() on the old
 * stack with the TLS descriptor already rewritten, corrupting next->regs.esp.
 * So schedule() saves
 * the caller's IF, clears it across the switch, and restores it afterward. Each
 * process does this symmetrically and resumes with its own IF; a process entered
 * for the first time skips the restore and takes its IF from iret instead.
 */
void schedule(void)
{
    uint32_t i;
    uint32_t start_idx;
    process_t *next = NULL;
    process_t *old;
    uint32_t saved_flags;

    if (!_sched_enabled) {
        return;
    }

    if (!current_proc) {
        return;
    }

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

    if (!next) {
        return;
    }

    if (next == current_proc) {
        return;
    }

    old = current_proc;

    if (old->state == PROC_RUNNING) {
        old->state = PROC_READY;
    }

    next->state = PROC_RUNNING;

    __asm__ __volatile__("pushf\n\t"
                         "cli\n\t"
                         "pop %0"
                         : "=r"(saved_flags)
                         :
                         : "memory");
    current_proc = next;

    switch_context(old, next);

    if (saved_flags & 0x200) {
        __asm__ __volatile__("sti");
    }
}

void sched_yield(void)
{
    schedule();
}
