#include "sched.h"

/** Nonzero once preemptive scheduling is enabled; schedule() is a no-op
 * until then. */
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
 * other process is READY. The switch runs with interrupts disabled: a timer
 * IRQ landing between publishing current_proc and the stack and CR3 swap
 * inside switch_context would see current_proc == next with next->in_syscall
 * == 0, pass the timer handler's reschedule gate, and re-enter schedule() on
 * the old stack with the TLS descriptor already rewritten, corrupting
 * next->regs.esp. So schedule() saves the caller's IF, clears it across the
 * switch, and restores it afterward. Each process does this symmetrically
 * and resumes with its own IF; a process entered for the first time skips
 * the restore and takes its IF from iret instead.
 *
 * The floating-point registers are the one part of the processor
 * `switch_context` does not touch. They are saved into the outgoing record
 * and loaded from the incoming one here, inside the same masked window and
 * before the stack pointer changes; the kernel computes in none of them, so
 * nothing between the load and the switch can disturb the loaded state.
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
    fpu_save(old->fpu);
    fpu_restore(next->fpu);
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

/* The idle marker and the two tick counters. `volatile` because the timer
 * handler reads the marker from an interrupt that arrives in the middle of the
 * halt it describes. The counters are 32-bit, which is one aligned store on
 * this machine, so a reader never sees half of one; at 100 Hz they wrap after
 * about 497 days, the bound `kernel_ticks` already carries. */
static volatile int _in_idle_wait = 0;
static volatile uint32_t _idle_ticks = 0;
static volatile uint32_t _busy_ticks = 0;

int sched_in_idle_wait(void)
{
    return _in_idle_wait;
}

uint32_t sched_idle_ticks(void)
{
    return _idle_ticks;
}

uint32_t sched_busy_ticks(void)
{
    return _busy_ticks;
}

void sched_account_tick(void)
{
    if (_in_idle_wait) {
        ++_idle_ticks;
    } else {
        ++_busy_ticks;
    }
}

/**
 * Park the processor until an interrupt, and return with interrupts disabled.
 *
 * Every caller of this one enters with IF already clear, having checked its
 * condition there, and that is what makes the wait free of a lost wakeup:
 * `sti; hlt` issued from IF=0 is atomic against a wake that became pending
 * during the check, because the interrupt fires the instant after `hlt` and
 * never before it. The marker is set while interrupts are still off, so no
 * tick can be delivered between setting it and the halt it describes.
 */
void idle_wait_irq_off(void)
{
    _in_idle_wait = 1;
    __asm__ __volatile__("sti; hlt; cli");
    _in_idle_wait = 0;
}

/**
 * Park the processor until an interrupt, and return with interrupts enabled.
 *
 * The callers of this one are already running with IF set, so an interrupt can
 * arrive at any instruction and the `sti` changes nothing; a wake that lands
 * between the marker and the `hlt` costs one more tick of waiting, which is why
 * every such caller re-checks its condition in a loop rather than assuming the
 * halt ended for its own reason. That was true of the bare `sti; hlt` these
 * replaced and is not something the marker introduced. The tick the marker
 * catches in that window is counted idle, which is what the processor is about
 * to be; the error is at most one tick per wait.
 */
void idle_wait_irq_on(void)
{
    _in_idle_wait = 1;
    __asm__ __volatile__("sti; hlt");
    _in_idle_wait = 0;
}
